// Renderer.cpp — see Renderer.h and ARCHITECTURE.md §4.3.
//
// Frame order (M1/M4):
//   1. viewport + clear from the packet
//   2. grid + world axes (renderer-owned overlays, each a gateable GL_LINES
//      draw — D19 showGrid/showAxes; both on = two counted draw calls)
//   3. each DrawItem with the PBR-ish directional-light shader
// Stats are measured, not modelled: drawCalls counts real GL draw calls
// (including the overlay and final HDR display triangle), triangles counts only indexed scene triangles,
// frameMs is CPU time inside renderFrame (no GPU-sync timestamp yet — see
// the finding published with M1).
#include "render/Renderer.h"

#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "core/Log.h"

namespace ember::render {

namespace {

// ---------------------------------------------------------------- shaders --

const char* kOverlayVS = R"GLSL(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
uniform mat4 uView;
uniform mat4 uProj;
out vec3 vColor;
out vec3 vWorld;
void main() {
    vColor = aColor;
    vWorld = aPosition;           // grid distance fade is computed per-PIXEL from
    gl_Position = uProj * uView * vec4(aPosition, 1.0);   // ALONG themselves, not just at ends
}
)GLSL";

const char* kOverlayFS = R"GLSL(#version 450 core
in vec3 vColor;
in vec3 vWorld;
out vec4 fragColor;

// Item 85 grid distance fade: uFade selects it per draw (grid = 1, axes/markers/
// origins/gizmo = 0 -> untouched). Defaults to 0, so passes that never set it
// are immune by construction.
uniform float uFade;
uniform vec3  uFadeCam;
uniform float uFadeStart;
uniform float uFadeEnd;
uniform float uExposure;
uniform float uPrecompensate;

vec3 srgbToLinear(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(0.9999));
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}
vec3 sceneColorForDisplay(vec3 c) {
    vec3 linearDisplay = srgbToLinear(c);
    return linearDisplay / max(vec3(1.0) - linearDisplay, vec3(1e-5)) /
           max(uExposure, 1e-8);
}

void main() {
    vec3 c = vColor;
    if (uFade > 0.5) {
        const float d = distance(vWorld.xz, uFadeCam.xz);
        const float span = max(uFadeEnd - uFadeStart, 1e-3);
        const float f = 1.0 - clamp((d - uFadeStart) / span, 0.0, 1.0);
        if (f <= 0.004) discard;
        c *= f;
    }
    // Viewport chrome is authored in display colors, not scene radiance. Undo
    // the view transform so overlays retain their intended color and contrast.
    fragColor = vec4(mix(c, sceneColorForDisplay(c), step(0.5, uPrecompensate)), 1.0);
}
)GLSL";

// Selection outline (item 85): an extruded hull pass in Blender orange. Chosen
// technique = normal-extrusion + glPolygonOffset (push hull depth away, object
// paints over its interior, ring survives at the silhouette) — deliberately NOT
// the inverted-hull/cull trick, because DrawItem winding is still unpinned
// (see init(): culling is off on purpose) and cull-based outlines would fail
// silently on any mesh that disagrees.
const char* kOutlineVS = R"GLSL(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uExtrude;      // world units at item depth (CPU keeps it px-constant)
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vec3 n = normalize(mat3(uModel) * aNormal);   // non-uniform scale skews the ring; primitives are uniform
    gl_Position = uProj * uView * (world + vec4(n * uExtrude, 0.0));
}
)GLSL";

const char* kOutlineFS = R"GLSL(#version 450 core
out vec4 fragColor;
uniform float uExposure;
vec3 srgbToLinear(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(0.9999));
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}
void main() {
    vec3 displayOrange = srgbToLinear(vec3(0.94, 0.49, 0.18));
    vec3 sceneOrange = displayOrange / max(vec3(1.0) - displayOrange, vec3(1e-5)) /
                       max(uExposure, 1e-8);
    fragColor = vec4(sceneOrange, 1.0); // selection stays display-referred
}
)GLSL";

// Item 96: directional shadow depth pass. Only depth is written (FBO draw/read
// buffers are GL_NONE). The normal offset is in WORLD units and driven by
// ShadowSettings::normalBias; this is the caster-side acne control.
const char* kShadowVS = R"GLSL(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
uniform mat4 uModel;
uniform mat4 uLightVP;
uniform float uNormalBias;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vec3 normal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    world.xyz += normal * uNormalBias;
    gl_Position = uLightVP * world;
}
)GLSL";

const char* kShadowFS = R"GLSL(#version 450 core
void main() { } // depth-only pass; fixed-function depth write is the output
)GLSL";

// The viewport renders into linear RGBA16F, then one fullscreen triangle
// performs fixed exposure, a simple Reinhard display curve, and linear-to-sRGB
// encoding. Exposure is intentionally manual and deterministic (no temporal
// adaptation that could make pixel tests or editor interaction flicker).
const char* kDisplayVS = R"GLSL(#version 450 core
out vec2 vUv;
void main() {
    vec2 p = (gl_VertexID == 0) ? vec2(-1.0, -1.0)
           : (gl_VertexID == 1) ? vec2( 3.0, -1.0)
                                : vec2(-1.0,  3.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

const char* kDisplayFS = R"GLSL(#version 450 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uHdrColor;
uniform sampler2D uCyclesImage;
uniform float uExposure;
uniform float uCompareSplit;
uniform int uComparePreview;
vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}
void main() {
    vec4 hdr = texture(uHdrColor, vUv);
    // PathTraceImage rows are uploaded top-to-bottom into a GL texture whose
    // v=0 row is the first (top) row. The fullscreen triangle has bottom-left
    // UV origin, so flip v when presenting the right half.
    if (uComparePreview != 0 && vUv.x >= uCompareSplit) {
        float cyclesU = (vUv.x - uCompareSplit) / max(1.0 - uCompareSplit, 1e-6);
        hdr = texture(uCyclesImage, vec2(cyclesU, 1.0 - vUv.y));
    }
    vec3 exposed = max(hdr.rgb, vec3(0.0)) * uExposure;
    vec3 mapped = exposed / (vec3(1.0) + exposed);
    fragColor = vec4(linearToSrgb(mapped), clamp(hdr.a, 0.0, 1.0));
}
)GLSL";

// Raster environment presentation. The source texture is linear RGBA and uses
// a lat-long layout; the inverse view-projection maps each viewport pixel to a
// world-space ray. This pass is bounded to one fullscreen triangle per frame.
const char* kEnvironmentVS = R"GLSL(#version 450 core
out vec2 vUv;
void main() {
    vec2 p = (gl_VertexID == 0) ? vec2(-1.0, -1.0)
           : (gl_VertexID == 1) ? vec2( 3.0, -1.0)
                                : vec2(-1.0,  3.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

const char* kEnvironmentFS = R"GLSL(#version 450 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uEnvironmentMap;
uniform mat4 uInverseViewProjection;
uniform vec3 uCameraPosition;
uniform float uEnvironmentStrength;
const float kPi = 3.14159265359;
vec2 environmentUv(vec3 direction) {
    vec3 d = normalize(direction);
    return vec2(atan(d.z, d.x) / (2.0 * kPi) + 0.5,
                asin(clamp(d.y, -1.0, 1.0)) / kPi + 0.5);
}
void main() {
    vec2 ndc = vUv * 2.0 - 1.0;
    vec4 farPoint = uInverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 world = farPoint.xyz / max(abs(farPoint.w), 1e-7) * sign(farPoint.w);
    vec3 direction = normalize(world - uCameraPosition);
    vec3 radiance = texture(uEnvironmentMap, environmentUv(direction)).rgb;
    fragColor = vec4(max(radiance, vec3(0.0)) * max(uEnvironmentStrength, 0.0), 1.0);
}
)GLSL";

const char* kSceneVS = R"GLSL(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform mat4 uLightVP;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vShadowClip;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPos = world.xyz;
    // Normals transform by the inverse-transpose of the model matrix.
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    vUV = aUV;
    vShadowClip = uLightVP * world;
    gl_Position = uProj * uView * world;
}
)GLSL";

const char* kSceneFS = R"GLSL(#version 450 core
// D19: the packet's light list — every directional AND point light up to a
// fixed cap (kMaxLights: MUST MATCH Material::kMaxLights in Material.h),
// Cook-Torrance-ish specular over Lambert diffuse per light. Point lights get
// a smooth radial falloff (att = (1 - d/radius)^2, zero beyond radius).
// This shader writes linear scene-referred values to the RGBA16F viewport target;
// the final display pass owns exposure, tone mapping and sRGB encoding.
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vShadowClip;
out vec4 fragColor;

uniform vec3 uCamPos;
uniform vec3 uAmbient;       // D22: world background as flat ambient (or the default 0.05)
uniform vec4 uBaseColor;
uniform sampler2D uBaseColorTexture;
uniform int uHasBaseColorTexture;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uEmissionColor;       // authored linear emission colour
uniform float uEmissionStrength;   // radiance multiplier
uniform sampler2D uEnvironmentMap;
uniform int uHasEnvironment;
uniform float uEnvironmentStrength;
uniform float uSelected;     // 0/1
uniform int uShadingMode;     // 0=Solid fast studio diffuse, 1=Material Preview PBR, 2=Rendered PBR

uniform int   uLightCount;               // <= 8, clamped CPU-side by Material
uniform int   uLightType[8];             // 0 = Directional, 1 = Point
uniform vec3  uLightDir[8];              // Directional: travel direction, normalized
uniform vec3  uLightPos[8];              // Point: world position
uniform vec3  uLightColorI[8];           // colour PRE-MULTIPLIED by intensity (CPU)
uniform float uLightRadius[8];           // Point: falloff distance
uniform sampler2D uShadowMap;
uniform int   uShadowEnabled;
uniform int   uShadowLightIndex;
uniform int   uReceiveShadow;
uniform float uShadowBias;
uniform float uShadowNormalBias;
uniform mat4  uLightVP;

const float kPi = 3.14159265;

vec2 environmentUv(vec3 direction) {
    vec3 d = normalize(direction);
    return vec2(atan(d.z, d.x) / (2.0 * kPi) + 0.5,
                asin(clamp(d.y, -1.0, 1.0)) / kPi + 0.5);
}

float distributionGGX(float nOh, float alpha) {
    float a2 = alpha * alpha;
    float d = nOh * nOh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d + 1e-7);
}
float geometrySmith(float nOv, float nOl, float k) {
    float v = nOv * (1.0 - k) + k;
    float l = nOl * (1.0 - k) + k;
    return 1.0 / (v * l + 1e-7);
}
vec3 fresnelSchlick(float uOh, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - uOh, 5.0);
}
float shadowVisibility(vec3 N, float nOl) {
    if (uShadowEnabled == 0 || uReceiveShadow == 0 || uShadowLightIndex < 0) return 1.0;
    // Receiver normal bias offsets the sample point in world space; caster
    // normal bias was applied in kShadowVS. Both controls are user-facing.
    vec4 clip = uLightVP * vec4(vWorldPos + N * uShadowNormalBias, 1.0);
    vec3 p = clip.xyz / max(clip.w, 1e-6);
    p = p * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z <= 0.0 || p.z >= 1.0)
        return 1.0;
    float bias = uShadowBias * (1.0 + 3.0 * (1.0 - clamp(nOl, 0.0, 1.0)));
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float visible = 0.0;
    // 3x3 percentage-closer filtering. Fixed small loop, same quality at both
    // resolutions; resolution changes texel footprint, not kernel cost.
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, p.xy + vec2(x, y) * texel).r;
            visible += (p.z - bias <= stored) ? 1.0 : 0.0;
        }
    return visible / 9.0;
}
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);

    vec3 albedo = uBaseColor.rgb;
    float alpha = uBaseColor.a;
    // Image samples are already scene-linear (the decoder handles the chosen
    // sRGB/Linear interpretation); Solid deliberately keeps its old flat
    // material colour and never samples authored images.
    if (uShadingMode != 0 && uHasBaseColorTexture != 0) {
        vec4 texel = texture(uBaseColorTexture, vUV);
        albedo *= texel.rgb;
        alpha *= texel.a;
    }
    // Blender Solid is the fast, scene-light-independent modeling view. It
    // uses a fixed neutral studio key + hemisphere fill, skips the PBR BRDF,
    // and never samples authored lights or shadows. Material Preview and
    // Rendered continue through the PBR path below.
    if (uShadingMode == 0) {
        vec3 studioL = normalize(vec3(0.45, 0.82, 0.34));
        float key = max(dot(N, studioL), 0.0);
        float hemi = 0.12 + 0.12 * max(N.y, 0.0);
        vec3 solidColor = albedo * (hemi + 0.72 * key);
        solidColor = mix(solidColor, solidColor * 1.15 + vec3(0.10, 0.06, 0.01), uSelected * 0.7);
        fragColor = vec4(solidColor, alpha);
        return;
    }

    vec3 f0 = mix(vec3(0.04), albedo, uMetallic);
    float rough = clamp(uRoughness, 0.045, 1.0);   // keep energy finite at 0
    float roughAlpha = rough * rough;
    float k = roughAlpha * 0.5;
    float nOv = max(dot(N, V), 1e-4);

    vec3 lit = vec3(0.0);
    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        float att = 1.0;
        if (uLightType[i] == 0) {
            L = normalize(-uLightDir[i]);
        } else {
            vec3 d = uLightPos[i] - vWorldPos;
            float dist = length(d);
            if (dist >= uLightRadius[i]) continue;      // outside the pool
            L = d / max(dist, 1e-4);
            float t = dist / uLightRadius[i];
            att = (1.0 - t) * (1.0 - t);
        }
        float nOl = max(dot(N, L), 0.0);
        if (nOl <= 0.0) continue;                       // back-facing to this light

        vec3 H = normalize(V + L);
        float nOh = max(dot(N, H), 0.0);
        float uOh = max(dot(V, H), 0.0);

        vec3 F = fresnelSchlick(uOh, f0);
        vec3 spec = distributionGGX(nOh, roughAlpha) * geometrySmith(nOv, nOl, k) * F /
                    (4.0 * nOv * nOl + 1e-6);
        vec3 diffuse = (1.0 - F) * (1.0 - uMetallic) * albedo / kPi;
        float visibility = (i == uShadowLightIndex)
            ? shadowVisibility(N, nOl) : 1.0;
        lit += (diffuse + spec) * nOl * uLightColorI[i] * att * visibility;
    }

    // D22: the world background is the ambient term (set from the packet;
    // capped CPU-side). Selection tint stays Blender-ish warm.
    vec3 color = uAmbient * albedo + lit;
    if (uHasEnvironment != 0) {
        float maxLod = float(max(textureQueryLevels(uEnvironmentMap) - 1, 0));
        vec3 diffuseEnvironment = textureLod(uEnvironmentMap, environmentUv(N), maxLod).rgb;
        vec3 reflectionDirection = reflect(-V, N);
        vec3 specularEnvironment = textureLod(
            uEnvironmentMap, environmentUv(reflectionDirection), rough * maxLod).rgb;
        vec3 viewFresnel = fresnelSchlick(nOv, f0);
        // A deliberately small bounded IBL approximation: the broadest
        // generated mip stands in for diffuse irradiance; roughness selects a
        // prefiltered-enough specular mip. This is not a probe/convolution pass.
        color += diffuseEnvironment * max(uEnvironmentStrength, 0.0) *
                 albedo * (1.0 - uMetallic) * 0.25;
        color += specularEnvironment * max(uEnvironmentStrength, 0.0) * viewFresnel *
                 (1.0 - rough * 0.65);
    }
    color = mix(color, color * 1.15 + vec3(0.10, 0.06, 0.01), uSelected * 0.7);
    // Emission is scene-linear radiance, independent of the light list and
    // studio ambient. It is intentionally not included in Solid's fixed studio
    // branch, which remains a scene-light-independent modelling view.
    color += uEmissionColor * uEmissionStrength;

    fragColor = vec4(color, alpha);
}
)GLSL";

// --------------------------------------------------------------- overlay ---

constexpr float kGridHalf = 24.0f;      // item 85: ±24 so the distance fade has room
constexpr float kGridMinor = 0.30f;
constexpr float kGridMajor = 0.42f;     // every 1 world unit (D24)
constexpr float kGridFadeStart = 6.0f;  // full brightness within...
constexpr float kGridFadeEnd   = 11.0f; // ...gone beyond (Blender grid falloff)
constexpr int   kGridGsMin = 1;         // D24: clamped — 0 would divide-by-zero
constexpr int   kGridGsMax = 100;       //        mid-overlay into a black viewport
constexpr float kAxisLen = 10.5f;
constexpr float kAxisPlaneOffset = 0.002f; // lift X/Z lines over old grid rows

void addLine(std::vector<float>& pos, std::vector<float>& col,
             const float x1, const float y1, const float z1,
             const float x2, const float y2, const float z2,
             const float r, const float g, const float b) {
    pos.insert(pos.end(), {x1, y1, z1, x2, y2, z2});
    col.insert(col.end(), {r, g, b, r, g, b});
}

int clampGridGs(const int v) {
    return v < kGridGsMin ? kGridGsMin : (v > kGridGsMax ? kGridGsMax : v);
}

// World units per SCREEN PIXEL at an anchor's depth, derived purely from the
// packet's matrices (THE ONE RULE: no camera access needed — projection carries
// the field, view carries the depth). Perspective: visible height = 2*d*tan.
// Orthographic (m[11] != -1): visible height = 2/m[5]. Returns false when the
// anchor is at/behind the eye (perspective) or inputs are degenerate.
bool worldPerPixel(const Mat4& view, const Mat4& proj, const Vec3& anchor,
                   const int heightPx, float& out) {
    if (heightPx <= 0) return false;
    const float vz = view.m[2] * anchor.x + view.m[6] * anchor.y +
                     view.m[10] * anchor.z + view.m[14];
    const float d = -vz;   // camera looks down -Z in view space
    if (proj.m[11] == -1.0f) {
        if (!(d > 1e-4f)) return false;
        const float tanHalf = 1.0f / proj.m[5];
        out = 2.0f * d * tanHalf / static_cast<float>(heightPx);
    } else {
        const float visibleH = 2.0f / proj.m[5];
        out = visibleH / static_cast<float>(heightPx);
    }
    return out > 0.0f;
}

// A screen-facing circle of line segments in the (right,up) basis.
void addCircle(std::vector<float>& pos, std::vector<float>& col, const Vec3& center,
               const Vec3& right, const Vec3& up, const float radius, const int segs,
               const float r, const float g, const float b) {
    for (int i = 0; i < segs; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(segs);
        const float a1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(segs);
        const float c0 = std::cos(a0) * radius, s0 = std::sin(a0) * radius;
        const float c1 = std::cos(a1) * radius, s1 = std::sin(a1) * radius;
        addLine(pos, col,
                center.x + right.x * c0 + up.x * s0,
                center.y + right.y * c0 + up.y * s0,
                center.z + right.z * c0 + up.z * s0,
                center.x + right.x * c1 + up.x * s1,
                center.y + right.y * c1 + up.y * s1,
                center.z + right.z * c1 + up.z * s1,
                r, g, b);
    }
}

} // namespace

// ------------------------------------------------------------------ life ---

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init() {
    if (initialized_) return true;

    if (!overlayShader_.build(kOverlayVS, kOverlayFS, "viewport_overlay")) {
        return false;
    }
    if (!sceneShader_.build(kSceneVS, kSceneFS, "pbr_directional")) {
        return false;
    }
    if (!outlineShader_.build(kOutlineVS, kOutlineFS, "selection_outline")) {
        return false;
    }
    if (!shadowShader_.build(kShadowVS, kShadowFS, "directional_shadow_depth")) {
        return false;
    }
    if (!displayShader_.build(kDisplayVS, kDisplayFS, "hdr_display_transform")) {
        return false;
    }
    if (!environmentShader_.build(kEnvironmentVS, kEnvironmentFS, "world_environment_background")) {
        return false;
    }
    glGenVertexArrays(1, &displayVao_);

    buildOverlay();
    if (!grid_.valid() || !axes_.valid()) {
        EMBER_LOG_ERROR("ember_render: overlay build failed");
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    // Deliberately no face culling: DrawItem winding is Slate's contract and
    // is not pinned yet. One flag flip once M2 winding is agreed.
    glDisable(GL_CULL_FACE);

    initialized_ = true;
    EMBER_LOG_INFO("ember_render: initialised");
    return true;
}

void Renderer::shutdown() {
    if (!initialized_ && !overlayShader_.valid() && !sceneShader_.valid() && !shadowShader_.valid() &&
        !displayShader_.valid() && !environmentShader_.valid() && meshCache_.empty() &&
        imageTextureCache_.empty() && !grid_.valid() && !axes_.valid() &&
        hdrFbo_ == 0 && pathTraceFbo_ == 0 && displayVao_ == 0) {
        return; // already down (destructor path)
    }
    meshCache_.clear();
    for (auto& [key, entry] : imageTextureCache_) {
        (void)key;
        if (entry.texture) glDeleteTextures(1, &entry.texture);
    }
    imageTextureCache_.clear();
    grid_.destroy();
    axes_.destroy();
    lightMesh_.destroy();
    gizmoMesh_.destroy();
    originsMesh_.destroy();
    editOverlayMesh_.destroy();
    overlayShader_.destroy();
    outlineShader_.destroy();
    shadowShader_.destroy();
    displayShader_.destroy();
    environmentShader_.destroy();
    sceneShader_.destroy();
    if (shadowDepth_) glDeleteTextures(1, &shadowDepth_);
    if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
    if (shadowTimerQuery_) glDeleteQueries(1, &shadowTimerQuery_);
    if (hdrColor_) glDeleteTextures(1, &hdrColor_);
    if (hdrDepth_) glDeleteRenderbuffers(1, &hdrDepth_);
    if (hdrFbo_) glDeleteFramebuffers(1, &hdrFbo_);
    if (pathTraceTexture_) glDeleteTextures(1, &pathTraceTexture_);
    if (pathTraceFbo_) glDeleteFramebuffers(1, &pathTraceFbo_);
    if (displayVao_) glDeleteVertexArrays(1, &displayVao_);
    shadowDepth_ = shadowFbo_ = shadowTimerQuery_ = shadowResolution_ = 0;
    hdrColor_ = hdrDepth_ = hdrFbo_ = displayVao_ = 0;
    pathTraceTexture_ = pathTraceFbo_ = 0;
    pathTraceWidth_ = pathTraceHeight_ = 0;
    pathTraceImage_ = nullptr;
    hdrWidth_ = hdrHeight_ = 0;
    lastOutputReadFramebuffer_ = 0;
    shadowTimerPending_ = false;
    shadowValid_ = false;
    initialized_ = false;
    stats_ = RenderStats{};
}

void Renderer::setViewportSettings(const ViewportSettings& settings) {
    settings_ = settings;
}

void Renderer::setExposureStops(const float stops) {
    if (!std::isfinite(stops)) return;
    exposureStops_ = std::clamp(stops, -16.0f, 16.0f);
}

void Renderer::resize(const int width, const int height) {
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
        fbW_ = width;      // D29: framebuffer size — the rect maps into THIS,
        fbH_ = height;     // not into the area's own packet dimensions
    }
}

Renderer::ImageTextureCacheEntry* Renderer::imageTexture(
    const ImageTextureRef& reference, const std::string& baseDirectory) {
    if (reference.path.empty()) return nullptr;

    std::string key = baseDirectory;
    key.push_back('\x1f');
    key += reference.path;
    key.push_back('\x1f');
    key += std::to_string(static_cast<unsigned int>(reference.colorSpace));
    auto [it, inserted] = imageTextureCache_.try_emplace(std::move(key));
    ImageTextureCacheEntry& entry = it->second;
    if (!inserted && entry.attempted) return &entry;
    entry.attempted = true;

    const std::optional<TextureImage> decoded =
        loadTextureImage(std::filesystem::path(reference.path),
                         std::filesystem::path(baseDirectory), reference.colorSpace);
    if (!decoded || decoded->width <= 0 || decoded->height <= 0) {
        EMBER_LOG_WARN("ember_render: could not load image texture '%s'", reference.path.c_str());
        return &entry;
    }
    const std::size_t width = static_cast<std::size_t>(decoded->width);
    const std::size_t height = static_cast<std::size_t>(decoded->height);
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        decoded->linearRgba.size() != width * height) {
        EMBER_LOG_WARN("ember_render: rejected malformed decoded image '%s'", reference.path.c_str());
        return &entry;
    }
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (maxTextureSize <= 0 || decoded->width > maxTextureSize || decoded->height > maxTextureSize) {
        EMBER_LOG_WARN("ember_render: image texture '%s' exceeds the GL texture-size limit",
                       reference.path.c_str());
        return &entry;
    }

    // The decoder owns top-to-bottom image rows while standard mesh UVs use
    // v=0 at the bottom. Flip once during upload so GPU sampling matches the
    // CPU sampler's bottom-left UV convention. The retained cache is GPU-only.
    std::vector<float> upload(width * height * 4u);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const Color& pixel = decoded->linearRgba[(height - 1u - y) * width + x];
            const std::size_t dst = (y * width + x) * 4u;
            upload[dst + 0] = pixel.r;
            upload[dst + 1] = pixel.g;
            upload[dst + 2] = pixel.b;
            upload[dst + 3] = pixel.a;
            if (pixel.a < 0.99999f) entry.hasAlpha = true;
        }
    }

    GLint oldActiveTexture = GL_TEXTURE0, oldTexture0 = 0;
    GLint oldUnpackBuffer = 0, oldUnpackAlignment = 4, oldUnpackRowLength = 0;
    GLint oldUnpackSkipPixels = 0, oldUnpackSkipRows = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldUnpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &oldUnpackSkipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &oldUnpackSkipRows);

    glGenTextures(1, &entry.texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, decoded->width, decoded->height,
                 0, GL_RGBA, GL_FLOAT, upload.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldUnpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, oldUnpackSkipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, oldUnpackSkipRows);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(oldUnpackBuffer));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (entry.texture == 0) {
        EMBER_LOG_WARN("ember_render: GL failed to allocate image texture '%s'", reference.path.c_str());
        return &entry;
    }
    return &entry;
}

void Renderer::drawEnvironmentBackground(const RenderPacket& packet, const Mat4& view,
                                         const Mat4& proj, const Vec3& cameraPosition) {
    const ImageTextureCacheEntry* entry =
        imageTexture(packet.background.environmentTexture, packet.assetBaseDirectory);
    if (!entry || entry->texture == 0) return;

    GLint oldViewport[4]{}, oldScissor[4]{}, oldProgram = 0, oldVao = 0;
    GLint oldActiveTexture = GL_TEXTURE0, oldTexture0 = 0;
    GLboolean oldDepthMask = GL_TRUE, oldColorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean srgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);

    glViewport(0, 0, width_, height_);
    // Respect the caller's active viewport scissor (the comparison wipe may
    // restrict raster work to the left half). Normal rendering already passes
    // the full target here.
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, entry->texture);
    environmentShader_.use();
    environmentShader_.setInt("uEnvironmentMap", 0);
    environmentShader_.setMat4("uInverseViewProjection", inverse(proj * view));
    environmentShader_.setVec3("uCameraPosition", cameraPosition.x, cameraPosition.y,
                               cameraPosition.z);
    float strength = packet.background.strength;
    if (!std::isfinite(strength) || strength < 0.0f) strength = 0.0f;
    environmentShader_.setFloat("uEnvironmentStrength", strength);
    glBindVertexArray(displayVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls;

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    glBindVertexArray(static_cast<GLuint>(oldVao));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (depthEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (srgbEnabled) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
    glDepthMask(oldDepthMask);
    glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
}

bool Renderer::ensureHdrTarget(const int width, const int height) {
    if (width <= 0 || height <= 0) return false;
    if (hdrFbo_ != 0 && hdrColor_ != 0 && hdrDepth_ != 0 &&
        hdrWidth_ == width && hdrHeight_ == height) return true;

    GLint oldDrawFbo = 0, oldReadFbo = 0, oldTexture = 0, oldRenderbuffer = 0, oldActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &oldRenderbuffer);

    GLuint newFbo = 0, newColor = 0, newDepth = 0;
    glGenFramebuffers(1, &newFbo);
    glGenTextures(1, &newColor);
    glGenRenderbuffers(1, &newDepth);
    glBindTexture(GL_TEXTURE_2D, newColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, newDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, newFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newColor, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, newDepth);
    const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuffer);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFbo));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(oldRenderbuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        EMBER_LOG_ERROR("ember_render: HDR framebuffer incomplete (0x%04x) at %dx%d", status, width, height);
        if (newFbo) glDeleteFramebuffers(1, &newFbo);
        if (newColor) glDeleteTextures(1, &newColor);
        if (newDepth) glDeleteRenderbuffers(1, &newDepth);
        return false;
    }

    if (hdrColor_) glDeleteTextures(1, &hdrColor_);
    if (hdrDepth_) glDeleteRenderbuffers(1, &hdrDepth_);
    if (hdrFbo_) glDeleteFramebuffers(1, &hdrFbo_);
    hdrFbo_ = newFbo;
    hdrColor_ = newColor;
    hdrDepth_ = newDepth;
    hdrWidth_ = width;
    hdrHeight_ = height;
    return true;
}

bool Renderer::ensurePathTraceTarget(const int width, const int height) {
    if (width <= 0 || height <= 0) return false;
    if (pathTraceFbo_ != 0 && pathTraceTexture_ != 0 &&
        pathTraceWidth_ == width && pathTraceHeight_ == height) return true;

    GLint oldDrawFbo = 0, oldReadFbo = 0, oldTexture = 0;
    GLint oldActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

    GLuint newFbo = 0, newTexture = 0;
    glGenFramebuffers(1, &newFbo);
    glGenTextures(1, &newTexture);
    glBindTexture(GL_TEXTURE_2D, newTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, newFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           newTexture, 0);
    const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        EMBER_LOG_ERROR("ember_render: path-trace upload framebuffer incomplete (0x%04x) at %dx%d",
                        status, width, height);
        if (newFbo) glDeleteFramebuffers(1, &newFbo);
        if (newTexture) glDeleteTextures(1, &newTexture);
        return false;
    }

    if (pathTraceTexture_) glDeleteTextures(1, &pathTraceTexture_);
    if (pathTraceFbo_) glDeleteFramebuffers(1, &pathTraceFbo_);
    pathTraceFbo_ = newFbo;
    pathTraceTexture_ = newTexture;
    pathTraceWidth_ = width;
    pathTraceHeight_ = height;
    return true;
}

bool Renderer::presentPathTraceImage(const PathTraceImage& image, const bool blitToHdr) {
    if (image.width <= 0 || image.height <= 0) return false;
    const std::size_t imageWidth = static_cast<std::size_t>(image.width);
    const std::size_t imageHeight = static_cast<std::size_t>(image.height);
    if (imageWidth > std::numeric_limits<std::size_t>::max() / imageHeight ||
        image.linearPixels.size() != imageWidth * imageHeight ||
        !ensurePathTraceTarget(image.width, image.height)) {
        return false;
    }

    GLint oldActiveTexture = GL_TEXTURE0, oldTexture0 = 0;
    GLint oldUnpackBuffer = 0, oldUnpackAlignment = 4, oldUnpackRowLength = 0;
    GLint oldUnpackSkipPixels = 0, oldUnpackSkipRows = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldUnpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &oldUnpackSkipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &oldUnpackSkipRows);

    // PathTraceImage rows are top-to-bottom. Texture/FBO coordinates are
    // bottom-to-top; the subsequent blit flips vertically while it scales.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindTexture(GL_TEXTURE_2D, pathTraceTexture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width, image.height,
                    GL_RGBA, GL_FLOAT, image.linearPixels.data());

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldUnpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, oldUnpackSkipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, oldUnpackSkipRows);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(oldUnpackBuffer));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));

    if (!blitToHdr) return true;

    GLint oldDrawFbo = 0, oldReadFbo = 0, oldScissor[4]{};
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, pathTraceFbo_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hdrFbo_);
    glBlitFramebuffer(0, 0, image.width, image.height,
                      0, height_, width_, 0,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFbo));
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return true;
}

void Renderer::drawDisplayPass(const u32 outputFramebuffer, const bool comparePreview,
                               const float compareSplit) {
    GLint oldDrawFbo = 0, oldReadFbo = 0, oldViewport[4]{}, oldScissor[4]{};
    GLint oldProgram = 0, oldVao = 0, oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0, oldTexture1 = 0;
    GLint oldPolygonMode[2]{};
    GLboolean oldDepthMask = GL_TRUE, oldColorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean srgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFbo);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
    glGetIntegerv(GL_POLYGON_MODE, oldPolygonMode);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture1);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, outputFramebuffer);
    glViewport(vpOriginX_, vpOriginY_, width_, height_);
    glScissor(vpOriginX_, vpOriginY_, width_, height_);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB); // shader already emits display-encoded sRGB
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrColor_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pathTraceTexture_);
    displayShader_.use();
    displayShader_.setInt("uHdrColor", 0);
    displayShader_.setInt("uCyclesImage", 1);
    displayShader_.setInt("uComparePreview", comparePreview ? 1 : 0);
    displayShader_.setFloat("uCompareSplit", compareSplit);
    displayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    glBindVertexArray(displayVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls; // the actual final fullscreen display-transform draw

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    glBindVertexArray(static_cast<GLuint>(oldVao));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (depthEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (srgbEnabled) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
    glDepthMask(oldDepthMask);
    glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
    glPolygonMode(GL_FRONT, static_cast<GLenum>(oldPolygonMode[0]));
    glPolygonMode(GL_BACK, static_cast<GLenum>(oldPolygonMode[1]));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFbo));
}

// Item 96: allocate/reallocate the depth-only target only when the clamped
// resolution changes. The clamp and log are inside this gate by construction:
// invalid UI values cannot trigger repeated allocation or log spam.
bool Renderer::ensureShadowTarget(const int requestedResolution) {
    constexpr int kMinShadowResolution = 256;
    constexpr int kMaxShadowResolution = 4096;
    const int resolution = requestedResolution < kMinShadowResolution ? kMinShadowResolution
                          : requestedResolution > kMaxShadowResolution ? kMaxShadowResolution
                                                                        : requestedResolution;
    if (shadowResolution_ == static_cast<u32>(resolution) && shadowFbo_ != 0 && shadowDepth_ != 0)
        return true;

    if (resolution != requestedResolution && lastShadowResolutionLogged_ != requestedResolution) {
        lastShadowResolutionLogged_ = requestedResolution;
        EMBER_LOG_WARN("ember_render: shadow resolution %d clamped to %d [256,4096]", requestedResolution, resolution);
    } else if (resolution == requestedResolution) {
        lastShadowResolutionLogged_ = -1;
    }

    if (shadowDepth_) glDeleteTextures(1, &shadowDepth_);
    if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
    shadowDepth_ = 0;
    shadowFbo_ = 0;
    shadowResolution_ = 0;

    glGenTextures(1, &shadowDepth_);
    glBindTexture(GL_TEXTURE_2D, shadowDepth_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, resolution, resolution,
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const GLfloat border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE); // manual 3x3 PCF in shader

    glGenFramebuffers(1, &shadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepth_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        EMBER_LOG_ERROR("ember_render: shadow FBO incomplete (0x%04x) at resolution %d", status, resolution);
        glDeleteFramebuffers(1, &shadowFbo_);
        glDeleteTextures(1, &shadowDepth_);
        shadowFbo_ = shadowDepth_ = 0;
        return false;
    }
    shadowResolution_ = static_cast<u32>(resolution);
    EMBER_LOG_INFO("ember_render: shadow depth target %dx%d", resolution, resolution);
    return true;
}

void Renderer::collectShadowGpuTime() {
    if (!shadowTimerPending_ || shadowTimerQuery_ == 0) return;
    GLint available = GL_FALSE;
    glGetQueryObjectiv(shadowTimerQuery_, GL_QUERY_RESULT_AVAILABLE, &available);
    if (available != GL_TRUE) return; // never stall the render thread
    GLuint64 ns = 0;
    glGetQueryObjectui64v(shadowTimerQuery_, GL_QUERY_RESULT, &ns);
    lastShadowGpuMs_ = static_cast<double>(ns) * 1e-6;
    shadowTimerPending_ = false;
}

bool Renderer::renderShadowPass(const RenderPacket& packet) {
    shadowValid_ = false;
    activeShadowLight_ = -1;
    const int index = packet.shadowLightIndex;
    if (!settings_.shadows.enabled || index < 0 || index >= static_cast<int>(packet.lights.size())) {
        if (settings_.shadows.enabled && lastShadowWarning_ != -1) {
            lastShadowWarning_ = -1;
            EMBER_LOG_WARN("ember_render: shadows enabled but packet has no valid shadowLightIndex; shadow pass skipped");
        }
        return false;
    }
    const LightInstance& light = packet.lights[static_cast<std::size_t>(index)];
    if (light.type != LightType::Directional) {
        const int code = index + 1;
        if (lastShadowWarning_ != code) {
            lastShadowWarning_ = code;
            EMBER_LOG_WARN("ember_render: shadowLightIndex %d is not directional; only directional maps are supported", index);
        }
        return false;
    }
    if (!ensureShadowTarget(settings_.shadows.resolution)) return false;

    // The renderer owns this light-space matrix (D22). The frustum is centered
    // on the orbit target; maxDistance is the full world-space diameter.
    Vec3 direction = light.direction;
    if (lengthSq(direction) < 1e-8f) direction = {0.0f, -1.0f, 0.0f};
    direction = normalize(direction);
    const Vec3 center = camera_.target();
    const float maxDistance = settings_.shadows.maxDistance < 1.0f ? 1.0f
                            : settings_.shadows.maxDistance > 10000.0f ? 10000.0f
                            : settings_.shadows.maxDistance;
    const Vec3 eye = center - direction * maxDistance;
    const Vec3 up = std::fabs(direction.y) > 0.95f ? Vec3{0.0f, 0.0f, 1.0f}
                                                   : Vec3{0.0f, 1.0f, 0.0f};
    const float extent = maxDistance * 0.5f;
    const Mat4 lightView = lookAt(eye, center, up);
    const Mat4 lightProjection = ortho(-extent, extent, -extent, extent, 0.1f, maxDistance * 2.0f);
    lightViewProjection_ = lightProjection * lightView;

    // Shadow pass owns a separate viewport and never inherits the area's
    // scissor. Restore the local HDR viewport immediately afterwards.
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, static_cast<GLsizei>(shadowResolution_), static_cast<GLsizei>(shadowResolution_));
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Include the depth-buffer clear in the GPU timing: at 2048² the clear is
    // part of the actual shadow-map cost, not a free setup step.
    if (shadowTimerQuery_ == 0) glGenQueries(1, &shadowTimerQuery_);
    collectShadowGpuTime();
    const bool timeThisPass = shadowTimerQuery_ != 0 && !shadowTimerPending_;
    if (timeThisPass) glBeginQuery(GL_TIME_ELAPSED, shadowTimerQuery_);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    shadowShader_.use();
    shadowShader_.setMat4("uLightVP", lightViewProjection_);
    float normalBias = settings_.shadows.normalBias;
    if (normalBias < 0.0f) normalBias = 0.0f;
    if (normalBias > 1.0f) normalBias = 1.0f;
    shadowShader_.setFloat("uNormalBias", normalBias);
    int casterCount = 0;
    for (const DrawItem& item : packet.items) {
        if (!item.castShadow || !item.mesh) continue;
        const auto it = meshCache_.find(item.mesh);
        if (it == meshCache_.end() || !it->second.uploaded || it->second.mesh.indexCount() == 0) continue;
        shadowShader_.setMat4("uModel", item.model);
        it->second.mesh.drawElements();
        ++casterCount;
        ++stats_.drawCalls;
    }
    if (timeThisPass) {
        glEndQuery(GL_TIME_ELAPSED);
        shadowTimerPending_ = true;
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, hdrFbo_);
    glViewport(0, 0, width_, height_);
    glScissor(0, 0, width_, height_);
    glEnable(GL_SCISSOR_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);

    if (casterCount == 0) {
        if (lastShadowWarning_ != -2) {
            lastShadowWarning_ = -2;
            EMBER_LOG_WARN("ember_render: shadow map has no castShadow meshes; receiver shadows disabled");
        }
        return false;
    }
    lastShadowWarning_ = -3; // valid pass; a future invalid condition reports again
    activeShadowLight_ = index;
    shadowValid_ = true;
    return true;
}

// ---------------------------------------------------------------- frame ----

void Renderer::renderFrame(const RenderPacket& packet) {
    const PathTraceImage* const submittedPathTraceImage = pathTraceImage_;
    pathTraceImage_ = nullptr; // borrowed input expires at this frame boundary, including early returns
    if (!initialized_) return;

    stats_ = RenderStats{};
    collectShadowGpuTime();
    const auto t0 = std::chrono::steady_clock::now();

    if (packet.width > 0 && packet.height > 0) {
        width_ = packet.width;
        height_ = packet.height;
    }
    if (width_ <= 0 || height_ <= 0) {
        return; // nothing addressable to draw
    }

    GLint outputDrawFramebuffer = 0, outputReadFramebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &outputDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &outputReadFramebuffer);
    lastOutputReadFramebuffer_ = static_cast<u32>(outputDrawFramebuffer);

    // D24: the grid is a viewport preference — rebuild only when the value
    // actually changed (clamped here; the renderer never trusts the struct).
    // If clamping moved it, say so once per change: the log line is what tells
    // a future debugger whether render or the UI squashed the value (Ash).
    const int reqGrid = settings_.gridSubdivisions;
    const int gs = clampGridGs(reqGrid);
    if (gs != builtGridGs_) {
        if (gs != reqGrid) {
            EMBER_LOG_WARN("ember_render: gridSubdivisions %d outside [1,100] — render clamped to %d", reqGrid, gs);
        }
        buildGrid(gs);
    }

    // D29: scope the 3D pass to the viewport rect — viewport AND scissor, so
    // areas around this one (headers, panels, other areas) are never
    // overdrawn. Invalid rect = legacy full-framebuffer. vpOrigin is stored
    // for readViewport; GL is bottom-up, the Rect is top-left origin.
    const int fbH = (fbH_ > 0) ? fbH_ : height_;
    if (viewportRect_.valid()) {
        vpOriginX_ = viewportRect_.x;
        vpOriginY_ = fbH - (viewportRect_.y + viewportRect_.h);
        width_     = viewportRect_.w;   // area dims drive px math + aspect
        height_    = viewportRect_.h;   // (packet agrees per D29; rect wins)
    } else {
        vpOriginX_ = 0;
        vpOriginY_ = 0;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(outputDrawFramebuffer));
    glViewport(vpOriginX_, vpOriginY_, width_, height_);
    glScissor(vpOriginX_, vpOriginY_, width_, height_);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(outputReadFramebuffer));
    // Clear the owning editor Area on the output target first. This preserves
    // the established D29 behavior around a smaller viewport, and the display
    // transform below only touches the viewport rect itself.
    const Color& bg = packet.background.color;
    const float bs = packet.background.strength;
    glClearColor(bg.r * bs, bg.g * bs, bg.b * bs, bg.a);
    const Rect clearRect = viewportClearRect_.valid() ? viewportClearRect_ : viewportRect_;
    if (clearRect.valid()) {
        const int clearY = fbH - (clearRect.y + clearRect.h);
        glScissor(clearRect.x, clearY, clearRect.w, clearRect.h);
    } else {
        glScissor(0, 0, width_, height_);
    }
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // All viewport raster output (scene, grid, edit overlay and markers) is
    // rendered in linear floating point. The owning-area clear above remains
    // on the output target and is deliberately not fed through tone mapping.
    if (!ensureHdrTarget(width_, height_)) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(outputDrawFramebuffer));
        glViewport(vpOriginX_, vpOriginY_, width_, height_);
        glScissor(vpOriginX_, vpOriginY_, width_, height_);
        glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(outputReadFramebuffer));
        return;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hdrFbo_);
    glViewport(0, 0, width_, height_);
    glScissor(0, 0, width_, height_);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(bg.r * bs, bg.g * bs, bg.b * bs, bg.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // THE ONE RULE, applied to the camera: App fills view/projection/
    // cameraPosition from renderer.camera() every frame (ARCHITECTURE §4.4).
    // The former "unwired packet" fallback (identity-or-zero detection) was
    // deleted on Ash's instruction 2026-10-16 — decision D10: dead code by
    // design once App wires; the packet is the sole truth.
    const Mat4& view = packet.view;
    const Mat4& proj = packet.projection;
    const Vec3& cameraPosition = packet.cameraPosition;

    // A Cycles-style viewport image replaces raster color only in Rendered
    // shading. Compare mode is viewport-only, independent of the scene's
    // engine selector, and uses the same packet camera as the raster half. A
    // missing/malformed image falls through to full-viewport raster (never a
    // stale preview).
    const bool cyclesRendered = packet.renderSettings.engine == RenderEngine::Cycles &&
                                settings_.shading == ShadingMode::Rendered;
    const bool compareRequested = settings_.shading == ShadingMode::Rendered &&
                                  settings_.compareRasterCycles;
    float compareSplit = std::isfinite(settings_.compareSplit) ? settings_.compareSplit : 0.5f;
    compareSplit = std::clamp(compareSplit, 0.05f, 0.95f);
    const bool compareRequestedPreview = compareRequested && submittedPathTraceImage &&
        presentPathTraceImage(*submittedPathTraceImage, false);
    const bool presentedPathTrace = !compareRequested && cyclesRendered && submittedPathTraceImage &&
        presentPathTraceImage(*submittedPathTraceImage);
    const bool comparePreview = compareRequestedPreview;
    int rasterScissorWidth = width_;
    if (comparePreview) {
        // Keep every pixel whose center lies left of the normalized wipe
        // available to raster; the display shader makes the exact per-pixel
        // choice using the same normalized split.
        rasterScissorWidth = std::clamp(static_cast<int>(std::ceil(width_ * compareSplit)),
                                        0, width_);
    }
    if (comparePreview) glScissor(0, 0, rasterScissorWidth, height_);

    // Upload once, then reuse the same cache for optional depth/colour passes.
    // In compare mode the raster scene is rendered normally, but scissored to
    // the left half; the Cycles texture is sampled on the right at display time.
    if (!packet.items.empty()) uploadMeshes(packet);
    shadowValid_ = false;
    activeShadowLight_ = -1;
    if (presentedPathTrace && !comparePreview) {
        if (!packet.items.empty()) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            drawItems(packet, view, proj, cameraPosition);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        drawOverlay(view, proj, cameraPosition);
        if (!packet.items.empty()) {
            drawSelectionOutlines(packet, view, proj);
            drawOrigins(packet, view, proj);
        }
        drawEditOverlay(packet.editOverlay, view, proj);
        if (settings_.showGizmo && !packet.lights.empty()) {
            drawLightMarkers(packet, view, proj);
        }
    } else {
        // Rendered raster mode presents the authored world image behind the
        // scene. Material Preview keeps its independent neutral studio setup;
        // a valid Cycles preview replaces this entire raster presentation.
        if (settings_.shading == ShadingMode::Rendered)
            drawEnvironmentBackground(packet, view, proj, cameraPosition);

        // Shadows are part of raster Rendered shading, not the fast Solid or
        // neutral Material Preview path.
        if (settings_.shading == ShadingMode::Rendered && settings_.shadows.enabled && !packet.items.empty()) {
            renderShadowPass(packet);
        }

        // The shadow pass uses its own target and restores a full HDR scissor;
        // reinstate the raster half before any viewport color/overlay draws.
        if (comparePreview) glScissor(0, 0, rasterScissorWidth, height_);

        drawOverlay(view, proj, cameraPosition);

        if (!packet.items.empty()) {
            // Hull before fill: the scene paints over its interior, leaving a rim.
            drawSelectionOutlines(packet, view, proj);
            drawItems(packet, view, proj, cameraPosition);
            // Origins are overlay chrome, depth-tested like real geometry aids.
            drawOrigins(packet, view, proj);
        }
        drawEditOverlay(packet.editOverlay, view, proj);

        // Item 58: light markers, after the scene, gated by showGizmo.
        if (settings_.showGizmo && !packet.lights.empty()) {
            drawLightMarkers(packet, view, proj);
        }
    }

    drawDisplayPass(static_cast<u32>(outputDrawFramebuffer), comparePreview, compareSplit);
    // Restore the externally visible render region and output framebuffer.
    // Subsequent editor rendering must never accidentally target the HDR FBO.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(outputDrawFramebuffer));
    glViewport(vpOriginX_, vpOriginY_, width_, height_);
    glScissor(vpOriginX_, vpOriginY_, width_, height_);
    glEnable(GL_SCISSOR_TEST);

    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    stats_.frameMs = static_cast<double>(ns) * 1e-6;
}

// --------------------------------------------------------------- overlays --

void Renderer::buildOverlay() {
    buildGrid(clampGridGs(settings_.gridSubdivisions));

    // World axes through the origin: X red, Y green, Z blue — separate mesh so
    // showAxes can gate them independently of showGrid (D19 toggles).
    std::vector<float> apos;
    std::vector<float> acol;
    addLine(apos, acol, -kAxisLen, kAxisPlaneOffset, 0.0f,
            kAxisLen, kAxisPlaneOffset, 0.0f, 0.85f, 0.25f, 0.25f);
    addLine(apos, acol, 0.0f, 0.0f, 0.0f,
            0.0f, kAxisLen, 0.0f, 0.25f, 0.85f, 0.25f);
    addLine(apos, acol, 0.0f, kAxisPlaneOffset, -kAxisLen,
            0.0f, kAxisPlaneOffset, kAxisLen, 0.30f, 0.45f, 0.95f);

    axes_.createLines(apos.data(), acol.data(), apos.size() / 3);
}

// D24: major grid lines every 1 world unit, minor lines every 1/gridSubdivisions.
// subdivisions arrives through a struct anyone can write — clamp [1,100] HERE,
// not in the UI: 0 would become a divide-by-zero that presents as a black
// viewport, which is the worst possible error message.
void Renderer::buildGrid(const int subdivisions) {
    const int gs = clampGridGs(subdivisions);
    builtGridGs_ = gs;

    std::vector<float> pos;
    std::vector<float> col;
    pos.reserve(1024 * gs);   // 4 verts per line, ~40*gs lines
    col.reserve(1024 * gs);

    const int steps = static_cast<int>(kGridHalf) * gs;
    for (int k = -steps; k <= steps; ++k) {
        if (k == 0) continue;                       // axes carry x=0 / z=0
        const float v = static_cast<float>(k) / static_cast<float>(gs);
        const bool major = (k % gs) == 0;           // every 1 world unit (D24)
        const float c = major ? kGridMajor : kGridMinor;
        addLine(pos, col, v, 0.0f, -kGridHalf, v, 0.0f, kGridHalf, c, c, c + 0.02f);
        addLine(pos, col, -kGridHalf, 0.0f, v, kGridHalf, 0.0f, v, c, c, c + 0.02f);
    }

    grid_.createLines(pos.data(), col.data(), pos.size() / 3);
}

void Renderer::drawOverlay(const Mat4& view, const Mat4& proj, const Vec3& cameraPosition) {
    // D19 overlay toggles: each visible half is its own real draw call, counted.
    // Item 85: the grid fades with distance from the camera; axes deliberately
    // do not (they are short, and "where is origin" must never dim out).
    if (!settings_.showGrid && !settings_.showAxes) return;
    overlayShader_.use();
    overlayShader_.setMat4("uView", view);
    overlayShader_.setMat4("uProj", proj);
    overlayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    overlayShader_.setFloat("uPrecompensate", 1.0f);
    overlayShader_.setVec3("uFadeCam", cameraPosition.x, cameraPosition.y, cameraPosition.z);
    overlayShader_.setFloat("uFadeStart", kGridFadeStart);
    overlayShader_.setFloat("uFadeEnd", kGridFadeEnd);
    if (settings_.showGrid && grid_.valid()) {
        overlayShader_.setFloat("uFade", 1.0f);
        grid_.drawLines(grid_.vertexCount());
        stats_.drawCalls += 1; // a real draw call — counted, honestly, no triangles
    }
    if (settings_.showAxes && axes_.valid()) {
        overlayShader_.setFloat("uFade", 0.0f);
        axes_.drawLines(axes_.vertexCount());
        stats_.drawCalls += 1;
    }
}

// Item 85: small screen-constant crosses at each object origin (model column 3),
// depth-tested like real geometry aids (occluded by what is in front of them).
void Renderer::drawOrigins(const RenderPacket& packet, const Mat4& view, const Mat4& proj) {
    if (!settings_.showOrigins) return;
    const Vec3 right{view.m[0], view.m[4], view.m[8]};
    const Vec3 up{view.m[1], view.m[5], view.m[9]};

    std::vector<float> pos;
    std::vector<float> col;
    pos.reserve(64 * packet.items.size());
    col.reserve(64 * packet.items.size());

    for (const DrawItem& item : packet.items) {
        const Vec3 o{item.model.m[12], item.model.m[13], item.model.m[14]};
        float wpp = 0.0f;
        if (!worldPerPixel(view, proj, o, height_, wpp)) continue;
        const float arm = 5.0f * wpp;
        addLine(pos, col,
                o.x - right.x * arm, o.y - right.y * arm, o.z - right.z * arm,
                o.x + right.x * arm, o.y + right.y * arm, o.z + right.z * arm,
                0.92f, 0.92f, 0.92f);
        addLine(pos, col,
                o.x - up.x * arm, o.y - up.y * arm, o.z - up.z * arm,
                o.x + up.x * arm, o.y + up.y * arm, o.z + up.z * arm,
                0.92f, 0.92f, 0.92f);
    }
    if (pos.empty()) return;

    originsMesh_.createLines(pos.data(), col.data(), pos.size() / 3);
    if (!originsMesh_.valid()) return;
    overlayShader_.use();
    overlayShader_.setFloat("uFade", 0.0f);       // uniforms persist across passes in a
    overlayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    overlayShader_.setFloat("uPrecompensate", 1.0f);
    overlayShader_.setMat4("uView", view);        // frame — never inherit the grid's 1
    overlayShader_.setMat4("uProj", proj);
    originsMesh_.drawLines(originsMesh_.vertexCount());
    stats_.drawCalls += 1;
}

void Renderer::drawEditOverlay(const EditOverlayState& state, const Mat4& view,
                               const Mat4& proj) {
    if (!initialized_ || !state.valid()) return;

    std::vector<float> pos;
    std::vector<float> col;
    const auto worldPoint = [&](const Vec3& local) {
        return transformPoint(state.model, local);
    };
    const auto appendLine = [&](const Vec3& a, const Vec3& b, const Color& c) {
        addLine(pos, col, a.x, a.y, a.z, b.x, b.y, b.z, c.r, c.g, c.b);
    };

    if (settings_.showEditEdges ||
        (settings_.showEditFaces && state.selectMode == SelectMode::Face)) {
        const std::size_t edgeCount = state.edgeLines.size() / 2;
        for (std::size_t edge = 0; edge < edgeCount; ++edge) {
            const bool selected = edge < state.edgeSelected.size() && state.edgeSelected[edge];
            const bool faceOutline = settings_.showEditFaces &&
                                     state.selectMode == SelectMode::Face && selected;
            if (!settings_.showEditEdges && !faceOutline) continue;
            const bool active = static_cast<int>(edge) == state.activeEdge;
            const Color& color = active ? state.activeColor
                               : selected ? state.selectedColor
                                          : state.unselectedColor;
            appendLine(worldPoint(state.edgeLines[edge * 2]),
                       worldPoint(state.edgeLines[edge * 2 + 1]), color);
        }
    }

    // Vertices are camera-facing crosses. Their world size is derived from the
    // same screen radius used by the editor's vertex hit test.
    if (settings_.showEditVertices) {
        const Vec3 right{view.m[0], view.m[4], view.m[8]};
        const Vec3 up{view.m[1], view.m[5], view.m[9]};
        for (std::size_t vertex = 0; vertex < state.vertexPoints.size(); ++vertex) {
            const Vec3 center = worldPoint(state.vertexPoints[vertex]);
            float worldPerPx = 0.0f;
            if (!worldPerPixel(view, proj, center, height_, worldPerPx)) continue;
            const float radius = state.vertexRadiusPixels * worldPerPx;
            const bool selected = vertex < state.vertexSelected.size() && state.vertexSelected[vertex];
            const bool active = static_cast<int>(vertex) == state.activeVertex;
            const Color& color = active ? state.activeColor
                               : selected ? state.selectedColor
                                          : state.unselectedColor;
            const Vec3 dx{right.x * radius, right.y * radius, right.z * radius};
            const Vec3 dy{up.x * radius, up.y * radius, up.z * radius};
            appendLine(center - dx, center + dx, color);
            appendLine(center - dy, center + dy, color);
        }
    }

    if (pos.empty()) return;
    editOverlayMesh_.createLines(pos.data(), col.data(), pos.size() / 3);
    if (!editOverlayMesh_.valid()) return;

    GLint oldDepthFunc = GL_LESS;
    GLfloat oldLineWidth = 1.0f;
    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
    glGetFloatv(GL_LINE_WIDTH, &oldLineWidth);
    if (settings_.xray) glDisable(GL_DEPTH_TEST);
    else {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);  // overlays sit on the surface depth
    }
    glLineWidth(std::max(1.0f, state.edgeWidthPixels));
    overlayShader_.use();
    overlayShader_.setFloat("uFade", 0.0f);
    overlayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    overlayShader_.setFloat("uPrecompensate", 1.0f);
    overlayShader_.setMat4("uView", view);
    overlayShader_.setMat4("uProj", proj);
    editOverlayMesh_.drawLines(editOverlayMesh_.vertexCount());
    glLineWidth(oldLineWidth);
    glDepthFunc(static_cast<GLenum>(oldDepthFunc));
    if (depthWasEnabled) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    stats_.drawCalls += 1;
}

// Item 85: orange selection outline — extruded hull + polygon offset (see
// kOutlineVS note for why not inverted-hull/cull). One counted draw per
// selected item; the fill pass overpaints the hull interior via depth.
void Renderer::drawSelectionOutlines(const RenderPacket& packet, const Mat4& view, const Mat4& proj) {
    if (!settings_.showSelectionOutline) return;
    if (settings_.shading == ShadingMode::Wireframe) return;   // edges already ARE the look

    bool anyDrawn = false;
    outlineShader_.use();
    outlineShader_.setMat4("uView", view);
    outlineShader_.setMat4("uProj", proj);
    outlineShader_.setFloat("uExposure", std::exp2(exposureStops_));
    // Only the inflated hull's BACK faces: they always lie behind the object's
    // own surface, so the fill pass paints the interior and just the rim
    // survives. Relies on Slate's documented CCW-from-outside winding
    // (Primitives.cpp header) — if a mesh ever disagrees, its outline paints
    // solid (loud, not silent). Front culling == back faces only.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glDepthFunc(GL_LEQUAL);              // hull sits at/behind the surface
    for (const DrawItem& item : packet.items) {
        if (!item.selected || !item.mesh) continue;
        auto it = meshCache_.find(item.mesh);
        if (it == meshCache_.end() || !it->second.uploaded || it->second.mesh.indexCount() == 0) continue;
        const Vec3 o{item.model.m[12], item.model.m[13], item.model.m[14]};
        float wpp = 0.0f;
        if (!worldPerPixel(view, proj, o, height_, wpp)) continue;
        outlineShader_.setMat4("uModel", item.model);
        outlineShader_.setFloat("uExtrude", 2.5f * wpp);       // ~2.5 px, constant at any zoom
        it->second.mesh.drawElements();
        stats_.drawCalls += 1;
        anyDrawn = true;
    }
    glDepthFunc(GL_LESS);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);             // global invariant: culling off (init)
    (void)anyDrawn;
}

// ------------------------------------------------------------- light markers --

float Renderer::lightMarkerRadiusPixels() {
    return 14.0f;   // sun disc radius; point star is smaller (fits within)
}

void Renderer::drawLightMarkers(const RenderPacket& packet, const Mat4& view, const Mat4& proj) {
    // Screen-facing basis extracted from the VIEW matrix: its rotation rows ARE
    // the camera axes in world space (view = R^T). Same worldPerPixel maths the
    // gizmo will use — constant pixel size at any zoom, in both projections.
    const Vec3 right{view.m[0], view.m[4], view.m[8]};
    const Vec3 up{view.m[1], view.m[5], view.m[9]};

    std::vector<float> pos;
    std::vector<float> col;
    pos.reserve(512);
    col.reserve(512);

    for (const LightInstance& L : packet.lights) {
        float wpp = 0.0f;
        if (!worldPerPixel(view, proj, L.position, height_, wpp)) continue;

        // Tint by the light's own colour; very dark lights get white markers
        // so "visible but not obtrusive" never becomes "invisible".
        const float mx = L.color.r > L.color.g ? (L.color.r > L.color.b ? L.color.r : L.color.b)
                                               : (L.color.g > L.color.b ? L.color.g : L.color.b);
        const bool dark = !(mx > 0.35f);
        const float cr = dark ? 0.85f : L.color.r;
        const float cg = dark ? 0.85f : L.color.g;
        const float cb = dark ? 0.85f : L.color.b;

        if (L.type == LightType::Directional) {
            // Sun disc + 8 spokes + travel line (Blender's sun marker).
            addCircle(pos, col, L.position, right, up, 14.0f * wpp, 16, cr, cg, cb);
            for (int i = 0; i < 8; ++i) {
                const float a0 = 6.2831853f * i / 8.0f + 0.3926991f;   // offset between spokes
                const float a1 = 6.2831853f * i / 8.0f;
                addLine(pos, col,
                        L.position.x + right.x * std::cos(a0) * 14.0f * wpp + up.x * std::sin(a0) * 14.0f * wpp,
                        L.position.y + right.y * std::cos(a0) * 14.0f * wpp + up.y * std::sin(a0) * 14.0f * wpp,
                        L.position.z + right.z * std::cos(a0) * 14.0f * wpp + up.z * std::sin(a0) * 14.0f * wpp,
                        L.position.x + right.x * std::cos(a1) * 20.0f * wpp + up.x * std::sin(a1) * 20.0f * wpp,
                        L.position.y + right.y * std::cos(a1) * 20.0f * wpp + up.y * std::sin(a1) * 20.0f * wpp,
                        L.position.z + right.z * std::cos(a1) * 20.0f * wpp + up.z * std::sin(a1) * 20.0f * wpp,
                        cr, cg, cb);
            }
            Vec3 dir{L.direction.x, L.direction.y, L.direction.z};
            const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (dl > 1e-6f) {
                const float sc = 26.0f * wpp / dl;   // travel line, screen-constant
                addLine(pos, col,
                        L.position.x, L.position.y, L.position.z,
                        L.position.x + dir.x * sc, L.position.y + dir.y * sc, L.position.z + dir.z * sc,
                        cr, cg, cb);
            }
        } else if (L.type == LightType::Point) {
            // Point star contract (item 146): the largest extent is 13 px,
            // strictly inside lightMarkerRadiusPixels() (the 14 px sun-disc
            // radius shared with the editor hit-test). Circle radius 8 px;
            // ray tips 13 px. Keep all point-marker geometry within that bound.
            addCircle(pos, col, L.position, right, up, 8.0f * wpp, 12, cr, cg, cb);
            for (int i = 0; i < 4; ++i) {
                const float a = 6.2831853f * i / 4.0f + 0.7853982f;
                addLine(pos, col,
                        L.position.x + right.x * std::cos(a) * 8.0f * wpp + up.x * std::sin(a) * 8.0f * wpp,
                        L.position.y + right.y * std::cos(a) * 8.0f * wpp + up.y * std::sin(a) * 8.0f * wpp,
                        L.position.z + right.z * std::cos(a) * 8.0f * wpp + up.z * std::sin(a) * 8.0f * wpp,
                        L.position.x + right.x * std::cos(a) * 13.0f * wpp + up.x * std::sin(a) * 13.0f * wpp,
                        L.position.y + right.y * std::cos(a) * 13.0f * wpp + up.y * std::sin(a) * 13.0f * wpp,
                        L.position.z + right.z * std::cos(a) * 13.0f * wpp + up.z * std::sin(a) * 13.0f * wpp,
                        cr, cg, cb);
            }
        }
        // Spot: no LightType::Spot in core yet — the cone drops in as soon as
        // the kind exists (request raised with Ash). Unknown kinds are skipped
        // here, not silently drawn wrong.
    }

    if (pos.empty()) return;
    lightMesh_.createLines(pos.data(), col.data(), pos.size() / 3);
    if (!lightMesh_.valid()) return;   // build failure stays silent (marker == decoration)

    overlayShader_.use();   // MUST bind: drawItems left sceneShader_ bound — without
                            // this the uniforms below would set the WRONG program and
                            // markers would shade as PBR grey (the harness caught it).
    overlayShader_.setFloat("uFade", 0.0f);   // never inherit the grid's fade state
    overlayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    overlayShader_.setFloat("uPrecompensate", 1.0f);
    overlayShader_.setMat4("uView", view);
    overlayShader_.setMat4("uProj", proj);
    // "drawn on top so the object cannot occlude it" — same convention the
    // gizmo will use: depth test off while markers draw.
    glDisable(GL_DEPTH_TEST);
    lightMesh_.drawLines(lightMesh_.vertexCount());
    glEnable(GL_DEPTH_TEST);
    stats_.drawCalls += 1;   // one real draw for all markers; lines add no triangles
}

// ---------------------------------------------------------------- gizmo -----

void Renderer::drawGizmo(const GizmoState& g, const Mat4& view, const Mat4& proj) {
    if (!initialized_ || !settings_.showGizmo) return;   // the toggle owns this

    float wpp = 0.0f;
    if (!worldPerPixel(view, proj, g.origin, height_, wpp)) return;
    const float len = g.handleLengthPixels * wpp;        // CONSTANT on screen (D19/D21)
    if (!(len > 1e-6f)) return;

    // Handle directions = COLUMNS of orientation (world space). Identity means
    // Global; the editor resolves Local to the object's world rotation (D20) —
    // the same matrix it hit-tests against, so drawn and clickable agree.
    const Vec3 ax[3] = {
        {g.orientation.m[0], g.orientation.m[1], g.orientation.m[2]},
        {g.orientation.m[4], g.orientation.m[5], g.orientation.m[6]},
        {g.orientation.m[8], g.orientation.m[9], g.orientation.m[10]}};
    const float base[3][3] = {
        {0.95f, 0.30f, 0.30f},   // X
        {0.30f, 0.95f, 0.30f},   // Y
        {0.35f, 0.55f, 1.00f}};  // Z

    auto bright = [&](int i, float out[3]) {
        // Item 77 tiers: base < hovered < active. Brighten toward white rather
        // than multiplying/clamping each channel: clamping the blue Z base
        // (0.35,0.55,1) at 2.1x turned active Z cyan and erased its axis cue.
        // Interpolation keeps the axis channel dominant in every state.
        const float amount = (i == g.activeAxis) ? 0.55f
                            : (i == g.hoveredAxis) ? 0.25f
                                                   : 0.0f;
        out[0] = base[i][0] + (1.0f - base[i][0]) * amount;
        out[1] = base[i][1] + (1.0f - base[i][1]) * amount;
        out[2] = base[i][2] + (1.0f - base[i][2]) * amount;
    };

    std::vector<float> pos;
    std::vector<float> col;
    pos.reserve(1024);
    col.reserve(1024);

    auto line = [&](int colorIdx, const Vec3& from, const Vec3& to) {
        float c[3]; bright(colorIdx, c);
        addLine(pos, col, from.x, from.y, from.z, to.x, to.y, to.z, c[0], c[1], c[2]);
    };
    auto square = [&](int colorIdx, const Vec3& center, const Vec3& d1, const Vec3& d2, float half) {
        float c[3]; bright(colorIdx, c);
        const Vec3 p00{center.x - d1.x * half - d2.x * half, center.y - d1.y * half - d2.y * half, center.z - d1.z * half - d2.z * half};
        const Vec3 p01{center.x + d1.x * half - d2.x * half, center.y + d1.y * half - d2.y * half, center.z + d1.z * half - d2.z * half};
        const Vec3 p11{center.x + d1.x * half + d2.x * half, center.y + d1.y * half + d2.y * half, center.z + d1.z * half + d2.z * half};
        const Vec3 p10{center.x - d1.x * half + d2.x * half, center.y - d1.y * half + d2.y * half, center.z - d1.z * half + d2.z * half};
        addLine(pos, col, p00.x, p00.y, p00.z, p01.x, p01.y, p01.z, c[0], c[1], c[2]);
        addLine(pos, col, p01.x, p01.y, p01.z, p11.x, p11.y, p11.z, c[0], c[1], c[2]);
        addLine(pos, col, p11.x, p11.y, p11.z, p10.x, p10.y, p10.z, c[0], c[1], c[2]);
        addLine(pos, col, p10.x, p10.y, p10.z, p00.x, p00.y, p00.z, c[0], c[1], c[2]);
    };

    switch (g.mode) {
    case GizmoState::Mode::Translate: {
        for (int i = 0; i < 3; ++i) {
            const Vec3 tip{g.origin.x + ax[i].x * len, g.origin.y + ax[i].y * len, g.origin.z + ax[i].z * len};
            line(i, g.origin, tip);
            // Arrow barbs (item 77): 4 barbs at the tip, splayed into the two
            // other axes — Blender's arrow handle.
            const int o1 = (i + 1) % 3, o2 = (i + 2) % 3;
            for (int k = 0; k < 4; ++k) {
                const Vec3& o = (k < 2) ? ax[o1] : ax[o2];
                const float sgn = (k % 2) ? 1.0f : -1.0f;
                const Vec3 barb{tip.x - ax[i].x * 0.12f * len + o.x * sgn * 0.05f * len,
                                tip.y - ax[i].y * 0.12f * len + o.y * sgn * 0.05f * len,
                                tip.z - ax[i].z * 0.12f * len + o.z * sgn * 0.05f * len};
                line(i, tip, barb);
            }
        }
        // Plane handles at the diagonals, tinted the THIRD axis's colour
        // (Blender: the XY grab moves along Z's complement).
        const int third[3][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}};
        for (auto& t : third) {
            const Vec3 ctr{g.origin.x + (ax[t[0]].x + ax[t[1]].x) * 0.5f * len,
                           g.origin.y + (ax[t[0]].y + ax[t[1]].y) * 0.5f * len,
                           g.origin.z + (ax[t[0]].z + ax[t[1]].z) * 0.5f * len};
            square(t[2], ctr, ax[t[0]], ax[t[1]], 0.14f * len);
        }
        break;
    }
    case GizmoState::Mode::Rotate: {
        // Circle around axis i lives in the plane of the OTHER two axes.
        const int plane[3][2] = {{1, 2}, {2, 0}, {0, 1}};
        for (int i = 0; i < 3; ++i) {
            float c[3]; bright(i, c);
            addCircle(pos, col, g.origin, ax[plane[i][0]], ax[plane[i][1]], 0.92f * len, 32, c[0], c[1], c[2]);
        }
        break;
    }
    case GizmoState::Mode::Scale: {
        for (int i = 0; i < 3; ++i) {
            const Vec3 tip{g.origin.x + ax[i].x * len, g.origin.y + ax[i].y * len, g.origin.z + ax[i].z * len};
            line(i, g.origin, tip);
            // Cube handle (item 77): real 12-edge wire cube at the tip,
            // oriented by the gizmo basis — Blender's scale handle.
            const float h = 0.14f * len;
            Vec3 corner[8];
            for (int b = 0; b < 8; ++b) {
                const float sx = (b & 1) ? h : -h, sy = (b & 2) ? h : -h, sz = (b & 4) ? h : -h;
                corner[b] = {tip.x + ax[0].x * sx + ax[1].x * sy + ax[2].x * sz,
                             tip.y + ax[0].y * sx + ax[1].y * sy + ax[2].y * sz,
                             tip.z + ax[0].z * sx + ax[1].z * sy + ax[2].z * sz};
            }
            for (int b = 0; b < 8; ++b)                 // 12 edges: bit flips, each once
                for (int bit = 0; bit < 3; ++bit)
                    if ((b & (1 << bit)) == 0)
                        line(i, corner[b], corner[b | (1 << bit)]);
        }
        // Plane handles at the diagonals (item 77: plane quads on translate
        // AND scale), tinted the third axis — Blender's XY/XZ/YZ scale grips.
        const int thirdS[3][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}};
        for (auto& t : thirdS) {
            const Vec3 ctr{g.origin.x + (ax[t[0]].x + ax[t[1]].x) * 0.5f * len,
                           g.origin.y + (ax[t[0]].y + ax[t[1]].y) * 0.5f * len,
                           g.origin.z + (ax[t[0]].z + ax[t[1]].z) * 0.5f * len};
            square(t[2], ctr, ax[t[0]], ax[t[1]], 0.14f * len);
        }
        break;
    }
    }

    if (pos.empty()) return;
    gizmoMesh_.createLines(pos.data(), col.data(), pos.size() / 3);
    if (!gizmoMesh_.valid()) return;

    overlayShader_.use();
    overlayShader_.setFloat("uFade", 0.0f);   // never inherit the grid's fade state
    overlayShader_.setFloat("uExposure", std::exp2(exposureStops_));
    overlayShader_.setFloat("uPrecompensate", 0.0f); // gizmo is drawn after the display resolve
    overlayShader_.setMat4("uView", view);
    overlayShader_.setMat4("uProj", proj);
    glDisable(GL_DEPTH_TEST);   // "drawn on top so the object cannot occlude it"
    gizmoMesh_.drawLines(gizmoMesh_.vertexCount());
    glEnable(GL_DEPTH_TEST);
    stats_.drawCalls += 1;      // one real draw; lines contribute no triangles
}

// ------------------------------------------------------------------ scene --

void Renderer::uploadMeshes(const RenderPacket& packet) {
    for (const DrawItem& item : packet.items) {
        if (!item.mesh) continue;

        auto it = meshCache_.find(item.mesh);
        if (it != meshCache_.end()) {
            // Scene-built packets carry geometry/modifier revisions, so an
            // in-place UV edit with unchanged array sizes still re-uploads.
            // Hand-built packets retain the legacy shape-only fallback.
            const MeshData& m = *item.mesh;
            const bool sameVersion = item.hasGeometryVersion &&
                it->second.hasGeometryVersion &&
                it->second.geometryVersion == item.geometryVersion &&
                it->second.modifierVersion == item.modifierVersion;
            if ((sameVersion || (!item.hasGeometryVersion && !it->second.hasGeometryVersion)) &&
                it->second.positionCount == m.positions.size() &&
                it->second.normalCount == m.normals.size() &&
                it->second.uvCount == m.uvs.size() &&
                it->second.indexCount == m.indices.size()) {
                continue;
            }
            it->second = MeshCacheEntry{}; // drop GPU data, rebuild below
        }

        MeshCacheEntry entry;
        const MeshData& m = *item.mesh;
        entry.uploaded = entry.mesh.create(m);
        entry.positionCount = m.positions.size();
        entry.normalCount = m.normals.size();
        entry.uvCount = m.uvs.size();
        entry.indexCount = m.indices.size();
        entry.geometryVersion = item.geometryVersion;
        entry.modifierVersion = item.modifierVersion;
        entry.hasGeometryVersion = item.hasGeometryVersion;
        if (m.positions.size() >= 3) {
            Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
            for (std::size_t v = 0; v + 2 < m.positions.size(); v += 3) {
                lo.x = std::min(lo.x, m.positions[v]);     hi.x = std::max(hi.x, m.positions[v]);
                lo.y = std::min(lo.y, m.positions[v + 1]); hi.y = std::max(hi.y, m.positions[v + 1]);
                lo.z = std::min(lo.z, m.positions[v + 2]); hi.z = std::max(hi.z, m.positions[v + 2]);
            }
            entry.localCenter = {0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
        }
        if (!entry.uploaded) {
            EMBER_LOG_ERROR("ember_render: rejected malformed MeshData: %s", m.name.c_str());
        }
        meshCache_.insert_or_assign(item.mesh, std::move(entry));
    }
}

void Renderer::drawItems(const RenderPacket& packet, const Mat4& view, const Mat4& proj,
                         const Vec3& cameraPosition) {
    GLint oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0, oldTexture6 = 0, oldTexture7 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glActiveTexture(GL_TEXTURE6);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture6);
    glActiveTexture(GL_TEXTURE7);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture7);
    glActiveTexture(GL_TEXTURE0);

    sceneShader_.use();
    sceneShader_.setMat4("uView", view);
    sceneShader_.setMat4("uProj", proj);
    sceneShader_.setVec3("uCamPos", cameraPosition.x, cameraPosition.y, cameraPosition.z);

    const bool solidFast = settings_.shading == ShadingMode::Solid ||
                           settings_.shading == ShadingMode::Wireframe;
    const bool materialPreview = settings_.shading == ShadingMode::Material;
    const bool rendered = settings_.shading == ShadingMode::Rendered;
    // Wireframe is selected by GL_POLYGON_MODE below; it shares Solid's
    // inexpensive studio shading for its edge colour.
    sceneShader_.setInt("uShadingMode", solidFast ? 0 : (materialPreview ? 1 : 2));

    ImageTextureCacheEntry* environment = nullptr;
    if (rendered && packet.background.lighting) {
        environment = imageTexture(packet.background.environmentTexture,
                                   packet.assetBaseDirectory);
    }
    const bool useEnvironment = environment && environment->texture != 0 &&
                                rendered && packet.background.lighting;
    Material::setEnvironmentTexture(sceneShader_, useEnvironment);
    float environmentStrength = packet.background.strength;
    if (!std::isfinite(environmentStrength) || environmentStrength < 0.0f)
        environmentStrength = 0.0f;
    sceneShader_.setFloat("uEnvironmentStrength", environmentStrength);
    if (useEnvironment) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, environment->texture);
        glActiveTexture(GL_TEXTURE0);
    }

    // Blender Material Preview is PBR under a neutral studio environment, not
    // the authored scene lights. The two white directional lobes are a small
    // deterministic stand-in for a studio HDRI. Rendered consumes packet
    // lights, optional world-image IBL and the shadow map; Solid is the fixed
    // studio-diffuse path above.
    static const std::vector<LightInstance> kNeutralStudio = [] {
        std::vector<LightInstance> v(2);
        v[0].type = LightType::Directional;
        v[0].direction = {0.45f, -1.0f, 0.30f};
        v[0].color = {1.0f, 1.0f, 1.0f, 1.0f};
        v[0].intensity = 1.15f;
        v[1].type = LightType::Directional;
        v[1].direction = {-0.8f, -0.25f, -0.5f};
        v[1].color = {1.0f, 1.0f, 1.0f, 1.0f};
        v[1].intensity = 0.35f;
        return v;
    }();

    // D19/62: Solid and Rendered consume every packet light (directional AND
    // point) up to the cap; Material Preview replaces them with the neutral
    // studio set. The deprecated `packet.light` is never read. Ignored lights
    // are reported only for the authored scene (never the internal studio set).
    int evaluatedLights = 0;
    {
        if (solidFast) {
            // The early Solid shader branch does no BRDF/light loop at all.
            sceneShader_.setInt("uLightCount", 0);
        } else if (materialPreview) {
            evaluatedLights = Material::applyLights(sceneShader_, kNeutralStudio);
        } else {
            evaluatedLights = Material::applyLights(sceneShader_, packet.lights);
        }
        const int ignored = static_cast<int>(packet.lights.size()) - evaluatedLights;
        if (rendered && ignored != lastLightIgnored_) {
            lastLightIgnored_ = ignored;
            if (ignored > 0) {
                // The ignored COUNT is render telling the user, in words,
                // "your scene has more lights than this renderer draws" — that
                // is the sentence someone needs when they hit it at 3am (D21 note).
                EMBER_LOG_WARN("ember_render: ignored %d light(s) — beyond the %d-light cap or an unsupported kind",
                               ignored, Material::kMaxLights);
            }
        }
    }

    // Material Preview gets a neutral gray studio environment; Rendered uses
    // the World background's optional ambient contribution. Solid's studio
    // diffuse branch above carries its own hemisphere fill.
    {
        const Color& b = packet.background.color;
        const float s = packet.background.strength;
        auto cap = [](float v) { return v < 0.0f ? 0.0f : (v > 0.5f ? 0.5f : v); };
        if (materialPreview) {
            sceneShader_.setVec3("uAmbient", 0.16f, 0.16f, 0.16f);
        } else if (rendered && packet.background.lighting) {
            if (useEnvironment) sceneShader_.setVec3("uAmbient", 0.0f, 0.0f, 0.0f);
            else sceneShader_.setVec3("uAmbient", cap(b.r * s), cap(b.g * s), cap(b.b * s));
        } else {
            sceneShader_.setVec3("uAmbient", 0.05f, 0.05f, 0.05f);
        }
    }

    // Bind/sample only if the optional pass really produced a map and the
    // caster is among the shader's evaluated light slots. With the toggle off
    // this is a single uniform write: no allocation, pass, texture bind, or
    // shader texture fetch. D22 shadowLightIndex is a packet-list index.
    const bool canSampleShadow = rendered && shadowValid_ && activeShadowLight_ >= 0 &&
                                 activeShadowLight_ < evaluatedLights &&
                                 activeShadowLight_ < Material::kMaxLights;
    sceneShader_.setMat4("uLightVP", lightViewProjection_);
    sceneShader_.setInt("uShadowLightIndex", canSampleShadow ? activeShadowLight_ : -1);
    sceneShader_.setInt("uShadowEnabled", canSampleShadow ? 1 : 0);
    sceneShader_.setInt("uShadowMap", 7);
    float depthBias = settings_.shadows.bias;
    if (depthBias < 0.0f) depthBias = 0.0f;
    if (depthBias > 0.05f) depthBias = 0.05f;
    float receiverNormalBias = settings_.shadows.normalBias;
    if (receiverNormalBias < 0.0f) receiverNormalBias = 0.0f;
    if (receiverNormalBias > 1.0f) receiverNormalBias = 1.0f;
    sceneShader_.setFloat("uShadowBias", depthBias);
    sceneShader_.setFloat("uShadowNormalBias", receiverNormalBias);
    if (canSampleShadow) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, shadowDepth_);
    }

    struct DrawRange {
        const DrawItem* item = nullptr;
        const MaterialDesc* material = nullptr;
        GLsizei firstIndex = 0;
        GLsizei indexCount = 0;
        float viewDepth = 0.0f;
        u32 baseColorTexture = 0;
    };
    std::vector<DrawRange> opaque;
    std::vector<DrawRange> transparent;

    // D45: slot assignment is packet data, parallel to emitted triangles.
    // Missing/wrong-length maps and invalid slot indices resolve to slot 0;
    // a missing slot array retains the legacy scalar material. Transparent
    // triangles are ordered by their own centroid rather than an item's AABB.
    for (const DrawItem& item : packet.items) {
        if (!item.mesh) continue;
        const auto cacheIt = meshCache_.find(item.mesh);
        if (cacheIt == meshCache_.end() || !cacheIt->second.uploaded || cacheIt->second.mesh.indexCount() == 0) continue;
        const GLsizei indexCount = cacheIt->second.mesh.indexCount();
        const GLsizei triangleCount = indexCount / 3;
        const bool hasSlots = item.materialCount > 0 && item.materials != nullptr;
        const bool validMapping = item.triangleMaterialSlots != nullptr &&
                                  item.triangleMaterialSlotCount == static_cast<u32>(triangleCount);

        auto materialForTriangle = [&](const GLsizei triangle) -> const MaterialDesc* {
            if (!hasSlots) return &item.material;
            u32 slot = 0;
            if (validMapping) {
                const u32 mapped = item.triangleMaterialSlots[triangle];
                if (mapped < item.materialCount) slot = mapped;
            }
            return &item.materials[slot].material;
        };

        auto textureForMaterial = [&](const MaterialDesc* material) -> ImageTextureCacheEntry* {
            if (solidFast || !material) return nullptr;
            return imageTexture(material->baseColorTexture, packet.assetBaseDirectory);
        };

        for (GLsizei tri = 0; tri < triangleCount; ++tri) {
            const MaterialDesc* material = materialForTriangle(tri);
            const float alpha = material->baseColor.a;
            if (!(alpha > 0.0f)) continue; // alpha <= 0 contributes nothing
            ImageTextureCacheEntry* texture = textureForMaterial(material);
            const bool hasTexture = texture && texture->texture != 0;
            const bool imageMayBeTransparent = hasTexture && texture->hasAlpha;
            const GLsizei first = tri * 3;
            if (alpha >= 1.0f && !imageMayBeTransparent) {
                // Merge adjacent opaque triangles with the same effective
                // material into one indexed draw; slot boundaries remain exact.
                if (!opaque.empty() && opaque.back().item == &item &&
                    opaque.back().material == material &&
                    opaque.back().firstIndex + opaque.back().indexCount == first) {
                    opaque.back().indexCount += 3;
                } else {
                    opaque.push_back({&item, material, first, 3, 0.0f,
                                      hasTexture ? texture->texture : 0});
                }
                if (opaque.back().baseColorTexture == 0 && hasTexture)
                    opaque.back().baseColorTexture = texture->texture;
                continue;
            }

            // D12 blend ordering uses a triangle centroid so independently
            // transparent material faces can be sorted, even on one object.
            Vec3 center{};
            bool centerValid = true;
            for (GLsizei corner = 0; corner < 3; ++corner) {
                const std::size_t indexOffset = static_cast<std::size_t>(first + corner);
                if (indexOffset >= item.mesh->indices.size()) { centerValid = false; break; }
                const std::size_t vertex = item.mesh->indices[indexOffset];
                const std::size_t p = vertex * 3;
                if (p + 2 >= item.mesh->positions.size()) { centerValid = false; break; }
                center.x += item.mesh->positions[p];
                center.y += item.mesh->positions[p + 1];
                center.z += item.mesh->positions[p + 2];
            }
            if (centerValid) {
                center.x /= 3.0f; center.y /= 3.0f; center.z /= 3.0f;
                center = transformPoint(item.model, center);
            } else {
                center = transformPoint(item.model, cacheIt->second.localCenter);
            }
            const float viewZ = view.m[2] * center.x + view.m[6] * center.y +
                                view.m[10] * center.z + view.m[14];
            const float depth = std::isfinite(viewZ) ? -viewZ : 0.0f;
            transparent.push_back({&item, material, first, 3, depth,
                                   hasTexture ? texture->texture : 0});
        }
    }
    std::stable_sort(transparent.begin(), transparent.end(),
                     [](const DrawRange& a, const DrawRange& b) {
                         return a.viewDepth > b.viewDepth; // farther (larger -viewZ) first
                     });

    auto drawRange = [&](const DrawRange& range) {
        const DrawItem& item = *range.item;
        const auto it = meshCache_.find(item.mesh);
        if (it == meshCache_.end()) return;
        sceneShader_.setMat4("uModel", item.model);
        Material::apply(sceneShader_, *range.material);
        const bool useBaseColorTexture = range.baseColorTexture != 0 && !solidFast;
        Material::setBaseColorTexture(sceneShader_, useBaseColorTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, useBaseColorTexture ? range.baseColorTexture : 0);
        sceneShader_.setFloat("uSelected", item.selected ? 1.0f : 0.0f);
        sceneShader_.setInt("uReceiveShadow", item.receiveShadow ? 1 : 0);
        // D19 shading: viewport Wireframe applies to every draw. MeshData has
        // no per-corner barycentric data, so glPolygonMode is the clean choice.
        const bool wire = settings_.shading == ShadingMode::Wireframe || item.wireframe;
        glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
        it->second.mesh.drawElements(range.firstIndex, range.indexCount);
        ++stats_.drawCalls;
        stats_.triangles += range.indexCount / 3;
    };

    // Pass 1: opaque surfaces establish the depth buffer and never blend.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    for (const DrawRange& range : opaque) drawRange(range);

    // Pass 2: transparent surfaces test against opaque depth but DO NOT write
    // it (so a nearer transparent layer cannot erase a farther one). Back-to-
    // front ordering makes standard source-over compositing deterministic.
    if (!transparent.empty()) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const DrawRange& range : transparent) drawRange(range);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture6));
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture7));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
}

// ------------------------------------------------------------- readback ----

void Renderer::readViewport(int x, int y, int w, int h, std::vector<u8>& outRGBA) {
    outRGBA.clear();
    if (!initialized_ || w <= 0 || h <= 0 || width_ <= 0 || height_ <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width_) w = width_ - x;
    if (y + h > height_) h = height_ - y;
    if (w <= 0 || h <= 0) return;

    outRGBA.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    GLint oldReadFramebuffer = 0, oldPackAlignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldPackAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, lastOutputReadFramebuffer_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // D29: x,y are area-local (same semantics as the no-rect full-framebuffer
    // case) — vpOrigin shifts them into framebuffer GL coords.
    glReadPixels(x + vpOriginX_, y + vpOriginY_, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA.data());
    glPixelStorei(GL_PACK_ALIGNMENT, oldPackAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));

    // GL rows read bottom-up; UI textures want top-down. Flip in place.
    const int rowBytes = w * 4;
    std::vector<u8> row(static_cast<std::size_t>(rowBytes));
    for (int r = 0; r < h / 2; ++r) {
        const std::size_t lo = static_cast<std::size_t>(r) * rowBytes;
        const std::size_t hi = static_cast<std::size_t>(h - 1 - r) * rowBytes;
        std::memcpy(row.data(), &outRGBA[lo], static_cast<std::size_t>(rowBytes));
        std::memcpy(&outRGBA[lo], &outRGBA[hi], static_cast<std::size_t>(rowBytes));
        std::memcpy(&outRGBA[hi], row.data(), static_cast<std::size_t>(rowBytes));
    }
}

} // namespace ember::render
