// tests/test_render_material_pixels.cpp - owned by Verity / QA. Item 158.
//
// Two renderer claims survived a whole wave because NOTHING MEASURED THEM:
//   (1) transparency: a material with alpha 0.2 must draw differently from alpha 1.0 -
//       specifically as source-over compositing predicts;
//   (2) material slots: a DrawItem's slot array is authoritative (Types.h D22), and two faces
//       assigned to different slots must draw with different materials.
// A shader that compiles, or blend code that exists, is not a pixel. These tests
// read the final display pixels. Source-over checks invert the documented sRGB +
// Reinhard display transform so compositing is still checked in linear HDR space.
//
//   alpha:  P(a) must equal  bg + a * (P(1) - bg)           (one layer over the background)
//           through a translucent layer the backdrop shows:  a*F + (1-a)*S
//   slots:  the colour on screen must come from the slot the face is assigned to.
//
// Slot test S2 is END TO END (scene API -> packet -> pixels) and reports each stage so a
// failure is attributed to the right owner: the scene storing the face's slot, the packet
// carrying the D45 triangle mapping, and the pixels. A separate hand-built packet test
// isolates renderer consumption from scene triangulation/transport.
//
// Needs a GL context: registered only under EMBER_ENABLE_GPU_TESTS, run with --gpu.
#include "harness.h"

#include "core/Types.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ember;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kW = 640;
constexpr int kH = 360;
constexpr float kHalfPi = 1.57079632679f;

bool ensureContext() {
    static ember::platform::Window* win = [] {
        auto* w = new ember::platform::Window();
        ember::platform::WindowConfig wc;
        wc.width = kW;
        wc.height = kH;
        wc.visible = false;
        wc.title = "ember-material-pixels";
        if (!w->create(wc)) { delete w; return static_cast<ember::platform::Window*>(nullptr); }
        return w;
    }();
    return win != nullptr;
}

struct Rgb { float r = 0, g = 0, b = 0; };
Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb operator-(Rgb a, Rgb b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
Rgb operator*(float k, Rgb a) { return {k * a.r, k * a.g, k * a.b}; }
float maxAbs(Rgb a) { return std::max({std::fabs(a.r), std::fabs(a.g), std::fabs(a.b)}); }
float sumAbs(Rgb a) { return std::fabs(a.r) + std::fabs(a.g) + std::fabs(a.b); }
std::string str(Rgb c) {
    char b[64]; std::snprintf(b, sizeof b, "(%.0f,%.0f,%.0f)", double(c.r), double(c.g), double(c.b)); return b;
}

struct Frame {
    std::vector<u8> rgba;
    static float srgbToLinear(float v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    static float inverseReinhard(float v) {
        v = std::min(v, 0.999f);
        return v / (1.0f - v);
    }
    // Scene-linear RGB reconstructed from the 8-bit sRGB display output. Values
    // retain the historical 0..255 scale used by the alpha/material assertions.
    Rgb linearAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return {};
        const std::size_t i = (std::size_t(y) * kW + std::size_t(x)) * 4;
        return {255.0f * inverseReinhard(srgbToLinear(rgba[i] / 255.0f)),
                255.0f * inverseReinhard(srgbToLinear(rgba[i + 1] / 255.0f)),
                255.0f * inverseReinhard(srgbToLinear(rgba[i + 2] / 255.0f))};
    }
    // Final display-encoded pixel values, useful for checking the tone-map pass.
    Rgb displayAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return {};
        const std::size_t i = (std::size_t(y) * kW + std::size_t(x)) * 4;
        return {float(rgba[i]), float(rgba[i + 1]), float(rgba[i + 2])};
    }
    // Mean of a 5x5 patch: robust to a one-pixel edge.
    Rgb at(int cx, int cy) const {
        Rgb s; int n = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
                s = s + linearAt(x, y); ++n;
            }
        return n ? (1.0f / float(n)) * s : s;
    }
};

// What a scene renders to. `yawDeg/pitchDeg` from the Front view (OrbitCamera: yaw -= dx*k,
// pitch += dy*k, k = 0.0087 rad/px); the camera is read back through projectScreen below.
struct Shot {
    Frame frame;
    render::OrbitCamera cam;   // copy of the camera that took the picture
    bool ok = false;
};

// `edit` may rewrite the packet (used by the slot-authority test to hand-build a DrawItem).
template <typename Edit>
Shot shoot(World& world, float yawDeg, float pitchDeg, Edit edit, float exposureStops = 0.0f,
           ShadingMode shading = ShadingMode::Rendered) {
    Shot out;
    if (!ensureContext()) return out;
    render::Renderer r;
    if (!r.init()) return out;
    r.resize(kW, kH);
    r.setExposureStops(exposureStops);
    r.camera().setView(render::StandardView::Front);
    r.camera().orbit(-yawDeg * 0.017453292f / 0.0087f, pitchDeg * 0.017453292f / 0.0087f);

    RenderPacket p = world.buildPacket(kW, kH);
    p.width = kW; p.height = kH;
    p.view = r.camera().view();
    p.projection = r.camera().projection(static_cast<float>(kW) / kH);
    p.cameraPosition = r.camera().position();
    p.background.color = {0.035f, 0.04f, 0.05f, 1.0f};
    p.background.strength = 1.0f;
    p.background.lighting = false;
    p.lights.clear();
    LightInstance sun;
    sun.type = LightType::Directional;
    sun.direction = {-0.3f, -0.5f, -1.0f};   // travels toward -Z and a little down/left: lights +Z, +X and top faces
    sun.color = {1.0f, 1.0f, 1.0f, 1.0f};
    sun.intensity = 3.0f;
    p.lights.push_back(sun);
    p.shadowLightIndex = -1;
    edit(p);

    ViewportSettings s;
    s.shading = shading;
    s.showGrid = false; s.showAxes = false; s.showGizmo = false; s.showOrigins = false;
    s.showSelectionOutline = false;
    s.shadows.enabled = false;
    r.setViewportSettings(s);
    for (int i = 0; i < 4; ++i) r.renderFrame(p);
    r.readViewport(0, 0, kW, kH, out.frame.rgba);
    out.cam = r.camera();
    r.shutdown();
    out.ok = out.frame.rgba.size() == std::size_t(kW) * kH * 4;
    return out;
}
Shot shoot(World& world, float yawDeg = 0.0f, float pitchDeg = 0.0f) {
    return shoot(world, yawDeg, pitchDeg, [](RenderPacket&) {});
}

// A plane facing the camera (Front view looks down -Z): the Plane primitive's normal is +Y,
// rotating +90 deg about X takes it to +Z.
ObjectId addFacingPlane(World& w, Vec3 pos, float size, MaterialDesc mat, const char* name) {
    const ObjectId id = w.createPrimitive(Primitive::Plane, name);
    Transform t{};
    t.position = pos;
    t.rotation = {kHalfPi, 0.0f, 0.0f};
    t.scale = {size, 1.0f, size};
    w.setTransform(id, t);
    w.setMaterial(id, mat);
    return id;
}

MaterialDesc mat(float r, float g, float b, float a) {
    MaterialDesc m;
    m.baseColor = {r, g, b, a};
    m.metallic = 0.0f;
    m.roughness = 0.8f;
    return m;
}

const int kCx = kW / 2, kCy = kH / 2;

Rgb backgroundOnly() {
    World w;
    Shot s = shoot(w);
    return s.ok ? s.frame.at(kCx, kCy) : Rgb{};
}

float expectedDisplayByte(float linear, float exposureStops) {
    const float exposed = std::max(0.0f, linear) * std::exp2(exposureStops);
    const float mapped = exposed / (1.0f + exposed);
    const float srgb = mapped <= 0.0031308f ? 12.92f * mapped
                                           : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return 255.0f * srgb;
}

} // namespace

TEST_GPU(hdr_target_tone_maps_and_manual_exposure_matches_the_display_transform) {
    World w;
    auto setHdrBackground = [](RenderPacket& p) {
        p.background.color = {2.0f, 1.0f, 0.25f, 1.0f};
        p.background.strength = 1.0f;
    };
    const Shot base = shoot(w, 0.0f, 0.0f, setHdrBackground, 0.0f);
    const Shot exposed = shoot(w, 0.0f, 0.0f, setHdrBackground, 2.0f);
    CHECK_MSG(base.ok && exposed.ok, "render failed");
    if (!(base.ok && exposed.ok)) return;

    const Rgb basePx = base.frame.displayAt(kCx, kCy);
    const Rgb exposedPx = exposed.frame.displayAt(kCx, kCy);
    std::printf("       HDR background display: EV0 %s, EV+2 %s\n", str(basePx).c_str(), str(exposedPx).c_str());
    CHECK_MSG(std::fabs(basePx.r - expectedDisplayByte(2.0f, 0.0f)) <= 2.0f &&
              std::fabs(basePx.g - expectedDisplayByte(1.0f, 0.0f)) <= 2.0f &&
              std::fabs(basePx.b - expectedDisplayByte(0.25f, 0.0f)) <= 2.0f,
              "linear HDR (2,1,0.25) should receive Reinhard+sRGB display transform; got %s", str(basePx).c_str());
    CHECK_MSG(std::fabs(exposedPx.r - expectedDisplayByte(2.0f, 2.0f)) <= 2.0f &&
              std::fabs(exposedPx.g - expectedDisplayByte(1.0f, 2.0f)) <= 2.0f &&
              std::fabs(exposedPx.b - expectedDisplayByte(0.25f, 2.0f)) <= 2.0f,
              "EV+2 should expose the HDR scene before tone mapping; got %s", str(exposedPx).c_str());
    CHECK_MSG(basePx.r < 255.0f && exposedPx.r < 255.0f && exposedPx.r > basePx.r,
              "bright HDR values should remain tone-mapped rather than clip: EV0 %.0f EV+2 %.0f",
              double(basePx.r), double(exposedPx.r));
}

TEST_GPU(hdr_target_reallocates_and_displays_the_new_scene_after_viewport_resize) {
    World world;
    render::Renderer renderer;
    CHECK_MSG(ensureContext(), "no GL context");
    CHECK_MSG(renderer.init(), "renderer initialization failed");
    renderer.resize(kW, kH);
    ViewportSettings settings;
    settings.shading = ShadingMode::Rendered;
    settings.showGrid = false;
    settings.showAxes = false;
    settings.showGizmo = false;
    settings.showOrigins = false;
    settings.showSelectionOutline = false;
    settings.shadows.enabled = false;
    renderer.setViewportSettings(settings);

    const auto renderBackground = [&](int width, int height, Color background,
                                      std::vector<u8>& pixels) {
        renderer.resize(width, height);
        RenderPacket packet = world.buildPacket(width, height);
        packet.width = width;
        packet.height = height;
        packet.view = renderer.camera().view();
        packet.projection = renderer.camera().projection(static_cast<float>(width) / height);
        packet.cameraPosition = renderer.camera().position();
        packet.background.color = background;
        packet.background.strength = 1.0f;
        renderer.renderFrame(packet);
        renderer.readViewport(0, 0, width, height, pixels);
    };

    std::vector<u8> first, resized;
    renderBackground(kW, kH, Color{2.0f, 0.25f, 0.1f, 1.0f}, first);
    // Keep the second extent odd and non-square to catch stale target sizes and
    // accidental reuse of the previous viewport's filtering/aspect state.
    const int resizedWidth = 319, resizedHeight = 177;
    renderBackground(resizedWidth, resizedHeight, Color{0.2f, 2.0f, 0.4f, 1.0f}, resized);
    const auto channelAt = [](const std::vector<u8>& rgba, int width, int x, int y, int channel) {
        return rgba[(static_cast<std::size_t>(y) * width + x) * 4 + channel];
    };
    CHECK_MSG(first.size() == static_cast<std::size_t>(kW) * kH * 4,
              "initial HDR target render did not cover the original viewport size");
    CHECK_MSG(resized.size() == static_cast<std::size_t>(resizedWidth) * resizedHeight * 4,
              "HDR target/readback did not rebuild for the resized viewport");
    if (first.size() == static_cast<std::size_t>(kW) * kH * 4 &&
        resized.size() == static_cast<std::size_t>(resizedWidth) * resizedHeight * 4) {
        const int x = resizedWidth / 2, y = resizedHeight / 2;
        const Rgb center{float(channelAt(resized, resizedWidth, x, y, 0)),
                         float(channelAt(resized, resizedWidth, x, y, 1)),
                         float(channelAt(resized, resizedWidth, x, y, 2))};
        const Rgb predicted{expectedDisplayByte(0.2f, 0.0f),
                            expectedDisplayByte(2.0f, 0.0f),
                            expectedDisplayByte(0.4f, 0.0f)};
        std::printf("       resized HDR viewport %dx%d center %s (expected %s)\n",
                    resizedWidth, resizedHeight, str(center).c_str(), str(predicted).c_str());
        CHECK_MSG(maxAbs(center - predicted) <= 2.0f,
                  "resized HDR target retained stale pixels or the old extent: got %s, expected %s",
                  str(center).c_str(), str(predicted).c_str());
    }
    renderer.shutdown();
}

TEST_GPU(emission_is_visible_without_scene_lights_in_rendered_and_material_preview) {
    for (const ShadingMode mode : {ShadingMode::Rendered, ShadingMode::Material}) {
        World w;
        MaterialDesc emissive = mat(0.0f, 0.0f, 0.0f, 1.0f);
        emissive.emissionColor = {0.1f, 0.7f, 0.2f, 1.0f};
        emissive.emissionStrength = 1.5f;
        addFacingPlane(w, {0, 0, 0}, 3.0f, emissive, "emissive_plane");
        const Shot s = shoot(w, 0.0f, 0.0f, [](RenderPacket& p) { p.lights.clear(); }, 0.0f, mode);
        CHECK_MSG(s.ok, "emission render failed in shading mode %d", static_cast<int>(mode));
        if (!s.ok) continue;

        // Black albedo isolates the emitted radiance from both the Rendered
        // empty-list fallback sun and Material Preview's neutral studio lights.
        const Rgb pixel = s.frame.at(kCx, kCy);
        const Rgb expected{0.1f * 1.5f * 255.0f,
                           0.7f * 1.5f * 255.0f,
                           0.2f * 1.5f * 255.0f};
        CHECK_MSG(maxAbs(pixel - expected) <= 10.0f,
                  "linear emission should survive light-independent in both modes; mode %d got %s, expected %s",
                  static_cast<int>(mode), str(pixel).c_str(), str(expected).c_str());
    }
}

TEST_GPU(zero_emission_preserves_the_existing_material_output) {
    auto render = [](bool setZeroEmission) {
        World w;
        MaterialDesc material = mat(0.35f, 0.6f, 0.2f, 1.0f);
        if (setZeroEmission) {
            material.emissionColor = {0.0f, 0.0f, 0.0f, 1.0f};
            material.emissionStrength = 0.0f;
        }
        addFacingPlane(w, {0, 0, 0}, 3.0f, material, "zero_emission_control");
        return shoot(w);
    };
    const Shot implicitZero = render(false);
    const Shot explicitZero = render(true);
    CHECK_MSG(implicitZero.ok && explicitZero.ok, "zero-emission render failed");
    if (!(implicitZero.ok && explicitZero.ok)) return;
    CHECK_MSG(implicitZero.frame.rgba == explicitZero.frame.rgba,
              "explicit black/zero emission changed pixels compared with the default zero-emission material");
}

TEST_GPU(per_triangle_material_mapping_selects_each_slots_emission) {
    World w;
    w.createPrimitive(Primitive::Plane, "mapped_emission_quad");

    MeshData quad;
    quad.name = "material_emission_mapping_test_quad";
    quad.positions = {-1,-1,0, 1,-1,0, 1,1,0, -1,1,0};
    quad.normals = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    quad.uvs = {0,0, 1,0, 1,1, 0,1};
    quad.indices = {0,1,2, 0,2,3};

    MaterialSlot slots[2];
    slots[0].name = "red_emission";
    slots[0].material = mat(0.0f, 0.0f, 0.0f, 1.0f);
    slots[0].material.emissionColor = {1.4f, 0.0f, 0.0f, 1.0f};
    slots[0].material.emissionStrength = 1.0f;
    slots[1].name = "green_emission";
    slots[1].material = mat(0.0f, 0.0f, 0.0f, 1.0f);
    slots[1].material.emissionColor = {0.0f, 1.2f, 0.0f, 1.0f};
    slots[1].material.emissionStrength = 1.0f;
    const u32 mapping[] = {0, 1};

    const Shot s = shoot(w, 0.0f, 0.0f, [&](RenderPacket& p) {
        p.lights.clear();
        for (DrawItem& item : p.items) {
            item.mesh = &quad;
            item.materials = slots;
            item.materialCount = 2;
            item.triangleMaterialSlots = mapping;
            item.triangleMaterialSlotCount = 2;
        }
    });
    CHECK_MSG(s.ok, "per-triangle emission render failed");
    if (!s.ok) return;

    float rx = 0.0f, ry = 0.0f, gx = 0.0f, gy = 0.0f;
    const bool projects = s.cam.projectScreen(Vec3{0.55f, -0.55f, 0}, kW, kH, rx, ry) &&
                          s.cam.projectScreen(Vec3{-0.55f, 0.55f, 0}, kW, kH, gx, gy);
    CHECK_MSG(projects, "cannot project per-triangle emission sample points");
    if (!projects) return;
    const Rgb redTri = s.frame.at(int(std::lround(rx)), int(std::lround(ry)));
    const Rgb greenTri = s.frame.at(int(std::lround(gx)), int(std::lround(gy)));
    std::printf("       mapped emission: slot0 %s, slot1 %s\n", str(redTri).c_str(), str(greenTri).c_str());
    CHECK_MSG(redTri.r > redTri.g + 200.0f,
              "triangle 0 should use slot 0 red emission, got %s", str(redTri).c_str());
    CHECK_MSG(greenTri.g > greenTri.r + 180.0f,
              "triangle 1 should use slot 1 green emission, got %s", str(greenTri).c_str());
}

// ---- (1) transparency -----------------------------------------------------------

TEST_GPU(alpha_point_two_composites_over_the_background_as_source_over_predicts) {
    const Rgb bg = backgroundOnly();
    World opaque;
    addFacingPlane(opaque, {0, 0, 0}, 3.0f, mat(0.9f, 0.1f, 0.1f, 1.0f), "plane");
    const Shot full = shoot(opaque);
    CHECK_MSG(full.ok, "render failed");
    if (!full.ok) return;
    const Rgb f = full.frame.at(kCx, kCy);

    World ghost;
    addFacingPlane(ghost, {0, 0, 0}, 3.0f, mat(0.9f, 0.1f, 0.1f, 0.2f), "plane");
    const Shot faint = shoot(ghost);
    const Rgb p = faint.frame.at(kCx, kCy);
    const Rgb expected = bg + 0.2f * (f - bg);
    std::printf("       bg %s  opaque %s  alpha0.2 %s  predicted %s\n", str(bg).c_str(), str(f).c_str(), str(p).c_str(),
                str(expected).c_str());
    CHECK_MSG(sumAbs(f - bg) > 120.0f, "setup: the opaque plane %s is not distinguishable from the background %s", str(f).c_str(), str(bg).c_str());
    CHECK_MSG(sumAbs(p - f) > 100.0f, "alpha 0.2 renders %s, essentially the same as alpha 1.0 %s: transparency has no effect",
              str(p).c_str(), str(f).c_str());
    CHECK_MSG(maxAbs(p - expected) <= 12.0f, "alpha 0.2 renders %s; source-over over %s predicts %s (max channel error %.1f)",
              str(p).c_str(), str(bg).c_str(), str(expected).c_str(), double(maxAbs(p - expected)));
}

TEST_GPU(alpha_opacity_is_monotonic_0_2_then_0_5_then_1_0) {
    const Rgb bg = backgroundOnly();
    float dist[3];
    const float alphas[3] = {0.2f, 0.5f, 1.0f};
    for (int i = 0; i < 3; ++i) {
        World w;
        addFacingPlane(w, {0, 0, 0}, 3.0f, mat(0.9f, 0.1f, 0.1f, alphas[i]), "plane");
        const Shot s = shoot(w);
        CHECK_MSG(s.ok, "render failed");
        if (!s.ok) return;
        dist[i] = sumAbs(s.frame.at(kCx, kCy) - bg);
    }
    std::printf("       distance from background: alpha 0.2 -> %.0f, 0.5 -> %.0f, 1.0 -> %.0f\n", double(dist[0]), double(dist[1]), double(dist[2]));
    CHECK_MSG(dist[1] > dist[0] * 1.5f, "alpha 0.5 (%.0f) is not clearly more opaque than alpha 0.2 (%.0f)", double(dist[1]), double(dist[0]));
    CHECK_MSG(dist[2] > dist[1] * 1.4f, "alpha 1.0 (%.0f) is not clearly more opaque than alpha 0.5 (%.0f)", double(dist[2]), double(dist[1]));
}

TEST_GPU(alpha_zero_contributes_nothing) {
    const Rgb bg = backgroundOnly();
    World w;
    addFacingPlane(w, {0, 0, 0}, 3.0f, mat(0.9f, 0.1f, 0.1f, 0.0f), "plane");
    const Shot s = shoot(w);
    CHECK_MSG(s.ok, "render failed");
    if (!s.ok) return;
    const Rgb p = s.frame.at(kCx, kCy);
    CHECK_MSG(maxAbs(p - bg) <= 3.0f, "alpha 0 draws %s over background %s: it must be invisible", str(p).c_str(), str(bg).c_str());
}

TEST_GPU(alpha_lets_an_opaque_backdrop_show_through_and_alpha_one_hides_it) {
    // Backdrop: a big opaque blue plane at z=-2. Front plane (red) at z=0.
    auto scene = [](float alpha, bool withFront, bool withBack) {
        auto w = std::make_unique<World>();
        if (withBack) addFacingPlane(*w, {0, 0, -2.0f}, 9.0f, mat(0.1f, 0.2f, 0.9f, 1.0f), "backdrop");
        if (withFront) addFacingPlane(*w, {0, 0, 0}, 3.0f, mat(0.9f, 0.1f, 0.1f, alpha), "front");
        return w;
    };
    const Shot backOnly = shoot(*scene(1.0f, false, true));
    const Shot frontOpaque = shoot(*scene(1.0f, true, false));   // reference for the front layer's own shading
    const Shot through = shoot(*scene(0.2f, true, true));
    const Shot blocked = shoot(*scene(1.0f, true, true));
    CHECK_MSG(backOnly.ok && frontOpaque.ok && through.ok && blocked.ok, "render failed");
    if (!(backOnly.ok && frontOpaque.ok && through.ok && blocked.ok)) return;
    const Rgb S = backOnly.frame.at(kCx, kCy), F = frontOpaque.frame.at(kCx, kCy);
    const Rgb T = through.frame.at(kCx, kCy), B = blocked.frame.at(kCx, kCy);
    const Rgb expected = 0.2f * F + 0.8f * S;
    std::printf("       backdrop %s  front-alone %s  through alpha0.2 %s (predicted %s)  behind alpha1.0 %s\n", str(S).c_str(),
                str(F).c_str(), str(T).c_str(), str(expected).c_str(), str(B).c_str());
    CHECK_MSG(sumAbs(S - F) > 150.0f, "setup: backdrop %s and front layer %s are not distinguishable", str(S).c_str(), str(F).c_str());
    CHECK_MSG(maxAbs(B - F) <= 6.0f, "an OPAQUE front plane must hide the backdrop: got %s, expected the front colour %s", str(B).c_str(),
              str(F).c_str());
    CHECK_MSG(maxAbs(T - expected) <= 14.0f, "through a 0.2-alpha plane the backdrop must show: got %s, predicted 0.2*front+0.8*backdrop = %s",
              str(T).c_str(), str(expected).c_str());
    CHECK_MSG(sumAbs(T - B) > 120.0f, "alpha 0.2 (%s) looks like alpha 1.0 (%s) over a backdrop: transparency has no effect", str(T).c_str(),
              str(B).c_str());
}

// ---- (2) material slots ---------------------------------------------------------

TEST_GPU(slot_array_is_authoritative_for_slot_0_when_present) {
    // D45 retains D22's slot authority: materialCount == 0 -> `material` is used;
    // otherwise slot 0 is authoritative. Hand-build: legacy GREEN, slot 0 RED.
    auto build = [](bool useSlots, Rgb& out) {
        World w;
        addFacingPlane(w, {0, 0, 0}, 3.0f, mat(0.1f, 0.9f, 0.1f, 1.0f), "plane");   // legacy material: green
        MaterialSlot slot;
        slot.name = "slot0";
        slot.material = mat(0.9f, 0.1f, 0.1f, 1.0f);                                // slot 0: red
        const MaterialSlot slots[1] = {slot};
        Shot s = shoot(w, 0.0f, 0.0f, [&](RenderPacket& p) {
            for (DrawItem& it : p.items) {
                if (!useSlots) continue;
                it.materials = slots;
                it.materialCount = 1;
            }
        });
        if (s.ok) out = s.frame.at(kCx, kCy);
        return s.ok;
    };
    Rgb legacy, slotted;
    CHECK_MSG(build(false, legacy) && build(true, slotted), "render failed");
    std::printf("       materialCount=0 -> %s (legacy green)   materialCount=1 with slot 0 red -> %s\n", str(legacy).c_str(), str(slotted).c_str());
    CHECK_MSG(legacy.g > legacy.r + 40.0f, "control: the legacy material (green) is %s - expected green-dominant", str(legacy).c_str());
    CHECK_MSG(slotted.r > slotted.g + 40.0f,
              "the slot array says slot 0 is RED but the pixel is %s: renderer did not apply the authoritative slot 0",
              str(slotted).c_str());
}

TEST_GPU(per_triangle_material_mapping_is_consumed_and_invalid_entries_fall_back_to_slot_zero) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Plane, "mapped_quad");
    Transform identity{};
    identity.scale = {1.0f, 1.0f, 1.0f};
    w.setTransform(id, identity);

    // Two triangles form a +Z-facing quad, separated by the diagonal x=y.
    // Their assignments are independent of Scene's editable-face mapping, so
    // this isolates renderer consumption from triangulation and transport.
    MeshData quad;
    quad.name = "material_mapping_test_quad";
    quad.positions = {-1,-1,0, 1,-1,0, 1,1,0, -1,1,0};
    quad.normals = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    quad.uvs = {0,0, 1,0, 1,1, 0,1};
    quad.indices = {0,1,2, 0,2,3};
    MaterialSlot slots[2];
    slots[0].name = "green";
    slots[0].material = mat(0.1f, 0.9f, 0.1f, 1.0f);
    slots[1].name = "blue";
    slots[1].material = mat(0.1f, 0.2f, 0.9f, 1.0f);

    const u32 validMap[] = {0, 1};
    const u32 shortMap[] = {0, 1};
    const u32 badIndexMap[] = {99, 1};
    auto mappedShot = [&](const u32* mapping, u32 mappingCount) {
        return shoot(w, 0.0f, 0.0f, [&](RenderPacket& p) {
            for (DrawItem& item : p.items) {
                item.mesh = &quad;
                item.materials = slots;
                item.materialCount = 2;
                item.triangleMaterialSlots = mapping;
                item.triangleMaterialSlotCount = mappingCount;
            }
        });
    };
    const Shot valid = mappedShot(validMap, 2);
    const Shot malformed = mappedShot(shortMap, 1); // wrong length: all triangles use slot 0
    const Shot outOfRange = mappedShot(badIndexMap, 2); // that triangle alone uses slot 0
    CHECK_MSG(valid.ok && malformed.ok && outOfRange.ok, "render failed");
    if (!(valid.ok && malformed.ok && outOfRange.ok)) return;

    float gx, gy, bx, by;
    const bool pointsProject = valid.cam.projectScreen(Vec3{0.55f, -0.55f, 0}, kW, kH, gx, gy) &&
                               valid.cam.projectScreen(Vec3{-0.55f, 0.55f, 0}, kW, kH, bx, by);
    CHECK_MSG(pointsProject, "cannot project triangle sample points");
    if (!pointsProject) return;
    const int ix = int(std::lround(gx)), iy = int(std::lround(gy));
    const int jx = int(std::lround(bx)), jy = int(std::lround(by));
    const Rgb greenTri = valid.frame.at(ix, iy), blueTri = valid.frame.at(jx, jy);
    const Rgb malformedGreen = malformed.frame.at(ix, iy), malformedBlue = malformed.frame.at(jx, jy);
    const Rgb fallbackGreen = outOfRange.frame.at(ix, iy), mappedBlue = outOfRange.frame.at(jx, jy);
    std::printf("       mapped triangles: slot0 %s, slot1 %s | malformed map slot0 %s/%s | out-of-range fallback %s, slot1 %s\n",
                str(greenTri).c_str(), str(blueTri).c_str(), str(malformedGreen).c_str(), str(malformedBlue).c_str(),
                str(fallbackGreen).c_str(), str(mappedBlue).c_str());
    CHECK_MSG(greenTri.g > greenTri.b + 35.0f, "triangle 0 should use green slot 0, got %s", str(greenTri).c_str());
    CHECK_MSG(blueTri.b > blueTri.g + 35.0f, "triangle 1 should use blue slot 1, got %s", str(blueTri).c_str());
    CHECK_MSG(malformedGreen.g > malformedGreen.b + 35.0f && malformedBlue.g > malformedBlue.b + 35.0f,
              "wrong-length triangle map must fall back to slot 0 on all triangles: %s / %s",
              str(malformedGreen).c_str(), str(malformedBlue).c_str());
    CHECK_MSG(fallbackGreen.g > fallbackGreen.b + 35.0f && mappedBlue.b > mappedBlue.g + 35.0f,
              "out-of-range slot index must fall back to slot 0 without changing the valid triangle: %s / %s",
              str(fallbackGreen).c_str(), str(mappedBlue).c_str());
}

TEST_GPU(two_faces_in_different_material_slots_draw_with_different_materials) {
    // END TO END: scene API -> packet -> pixels. A cube with slot 0 RED and slot 1 BLUE; the +Z
    // faces are assigned to slot 1. From yaw 35 / pitch 25 the +Z face and the +X face are both
    // visible, so one image must show blue on one and red on the other.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    w.setMaterialSlot(id, 0, mat(0.9f, 0.1f, 0.1f, 1.0f));
    const u32 blueSlot = w.addMaterialSlot(id, mat(0.1f, 0.2f, 0.9f, 1.0f), "blue");

    // Stage 1 - the SCENE stores the assignment (EditableMesh::Face::materialIndex, D34/D22).
    scene::EditableMesh* em = w.editable(id);
    CHECK_MSG(em != nullptr && !em->faces.empty(), "stage 1: no editable mesh");
    if (!em || em->faces.empty()) return;
    float h = 0.0f;
    for (const Vec3& p : em->positions) h = std::max({h, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
    int front = 0;
    for (std::size_t f = 0; f < em->faces.size(); ++f) {
        const scene::Face& fc = em->faces[f];
        if (fc.loopCount < 3) continue;
        const Vec3 a = em->positions[em->loops[fc.firstLoop + 0].vertex];
        const Vec3 b = em->positions[em->loops[fc.firstLoop + 1].vertex];
        const Vec3 c = em->positions[em->loops[fc.firstLoop + 2].vertex];
        const Vec3 n = cross(b - a, c - a);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-6f && n.z / len > 0.9f) { em->faces[f].materialIndex = blueSlot; ++front; }
    }
    CHECK_MSG(front >= 2, "stage 1: found only %d +Z triangles on the cube", front);
    const bool stage1 = w.readableEdit(id) && w.readableEdit(id)->faces.size() > 0;
    u32 slotCount = 0;
    const MaterialSlot* slots = w.materialSlots(id, slotCount);
    CHECK_MSG(stage1 && slotCount == 2 && slots, "stage 1: scene reports %u slots", slotCount);

    // Stage 2 - the PACKET carries the ordered slots and one mapping entry for
    // each emitted triangle, including triangles from the assigned +Z faces.
    const RenderPacket probe = w.buildPacket(kW, kH);
    bool stage2Slots = false, stage2Mapping = false, stage2BlueAssignment = false;
    for (const DrawItem& it : probe.items) {
        if (it.object.value != id.value) continue;
        stage2Slots = it.materialCount == 2 && it.materials && it.materials[1].material.baseColor.b > 0.8f;
        stage2Mapping = it.mesh && it.triangleMaterialSlots &&
                        it.triangleMaterialSlotCount == it.mesh->indices.size() / 3;
        if (stage2Mapping) {
            for (u32 tri = 0; tri < it.triangleMaterialSlotCount; ++tri)
                if (it.triangleMaterialSlots[tri] == blueSlot) stage2BlueAssignment = true;
        }
    }
    CHECK_MSG(stage2Slots, "stage 2: the packet's DrawItem does not carry both slots");
    CHECK_MSG(stage2Mapping, "stage 2: the packet's per-triangle mapping is absent or has the wrong count");
    CHECK_MSG(stage2BlueAssignment, "stage 2: assigned +Z face triangles are not mapped to slot %u", blueSlot);

    // Stage 3 - the PIXELS: +Z face blue, +X face red.
    const Shot s = shoot(w, 35.0f, 25.0f);
    CHECK_MSG(s.ok, "render failed");
    if (!s.ok) return;
    float zx, zy, xx, xy;
    const bool okZ = s.cam.projectScreen(Vec3{0, 0, h}, kW, kH, zx, zy);
    const bool okX = s.cam.projectScreen(Vec3{h, 0, 0}, kW, kH, xx, xy);
    CHECK_MSG(okZ && okX, "cannot project the face centres");
    if (!okZ || !okX) return;
    const Rgb pz = s.frame.at(int(std::lround(zx)), int(std::lround(zy)));
    const Rgb px = s.frame.at(int(std::lround(xx)), int(std::lround(xy)));
    std::printf("       stage1 scene stores slot: %s | stage2 packet carries slots: %s | stage3 pixels: +Z face %s (slot 1 = blue), +X face %s (slot 0 = red)\n",
                stage1 ? "yes" : "NO", (stage2Slots && stage2Mapping && stage2BlueAssignment) ? "yes" : "NO",
                str(pz).c_str(), str(px).c_str());
    CHECK_MSG(px.r > px.b + 30.0f, "the +X face is in slot 0 (red) but renders %s", str(px).c_str());
    CHECK_MSG(pz.b > pz.r + 30.0f, "the +Z face is assigned to slot 1 (BLUE) but renders %s: renderer did not consume its packet triangle mapping",
              str(pz).c_str());
    // (Deliberately no "the two pixels differ" check: the faces differ in LIGHTING anyway, so that
    // would pass for the wrong reason. The hue assertions above are the discriminating ones.)
}

TEST_GPU(control_a_cube_with_every_face_in_slot_0_is_uniformly_the_slot_0_colour) {
    // Control for the test above: without any reassignment both visible faces are slot 0 (red), so
    // a hue test cannot be passing on lighting alone, and the slot-0 path works.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    w.setMaterialSlot(id, 0, mat(0.9f, 0.1f, 0.1f, 1.0f));
    w.addMaterialSlot(id, mat(0.1f, 0.2f, 0.9f, 1.0f), "blue");
    const Shot s = shoot(w, 35.0f, 25.0f);
    CHECK_MSG(s.ok, "render failed");
    if (!s.ok) return;
    float zx, zy, xx, xy;
    const float h = 0.5f;
    CHECK_MSG(s.cam.projectScreen(Vec3{0, 0, h}, kW, kH, zx, zy) && s.cam.projectScreen(Vec3{h, 0, 0}, kW, kH, xx, xy), "project failed");
    const Rgb pz = s.frame.at(int(std::lround(zx)), int(std::lround(zy)));
    const Rgb px = s.frame.at(int(std::lround(xx)), int(std::lround(xy)));
    CHECK_MSG(pz.r > pz.b + 30.0f, "+Z face of an all-slot-0 cube is %s, expected red", str(pz).c_str());
    CHECK_MSG(px.r > px.b + 30.0f, "+X face of an all-slot-0 cube is %s, expected red", str(px).c_str());
}

HARNESS_MAIN()
