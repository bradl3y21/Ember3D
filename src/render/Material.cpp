// Material.cpp — see Material.h. Uniform names must match the scene shader
// in Renderer.cpp; the Shader's location cache tolerates renames at runtime.
//
// Shader/CPU sync: kMaxLights here == the "[8]" array sizes in kSceneFS. If
// either changes, change both.
#include "render/Material.h"

#include <cmath>

#include "core/Types.h"

namespace ember::render {

namespace {

struct Packed {
    int   type[Material::kMaxLights];      // 0 = Directional, 1 = Point
    float dir[3 * Material::kMaxLights];   // direction rays TRAVEL, normalized
    float pos[3 * Material::kMaxLights];   // point world position
    float colI[3 * Material::kMaxLights];  // colour PRE-MULTIPLIED by intensity
    float radius[Material::kMaxLights];    // point falloff distance
};

void normalizeOrDown(float& x, float& y, float& z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    else { x = 0.0f; y = -1.0f; z = 0.0f; }   // documented default sun
}

} // namespace

void Material::apply(Shader& shader, const MaterialDesc& desc) {
    shader.setVec4("uBaseColor", desc.baseColor.r, desc.baseColor.g,
                   desc.baseColor.b, desc.baseColor.a);
    shader.setFloat("uMetallic", desc.metallic);
    shader.setFloat("uRoughness", desc.roughness);
    shader.setVec3("uEmissionColor", desc.emissionColor.r, desc.emissionColor.g,
                   desc.emissionColor.b);
    shader.setFloat("uEmissionStrength", desc.emissionStrength);
    setBaseColorTexture(shader, false);
}

void Material::setBaseColorTexture(Shader& shader, const bool enabled) {
    shader.setInt("uBaseColorTexture", 0);
    shader.setInt("uHasBaseColorTexture", enabled ? 1 : 0);
}

void Material::setEnvironmentTexture(Shader& shader, const bool enabled) {
    shader.setInt("uEnvironmentMap", 6);
    shader.setInt("uHasEnvironment", enabled ? 1 : 0);
}

int Material::applyLights(Shader& shader, const std::vector<LightInstance>& lights) {
    Packed p{};
    int n = 0;

    for (const LightInstance& L : lights) {
        if (n >= kMaxLights) break;   // caller reports the excess as ignored
        if (L.type == LightType::Directional) {
            float dx = L.direction.x, dy = L.direction.y, dz = L.direction.z;
            normalizeOrDown(dx, dy, dz);
            p.type[n] = 0;
            p.dir[3 * n + 0] = dx; p.dir[3 * n + 1] = dy; p.dir[3 * n + 2] = dz;
            p.pos[3 * n + 0] = 0.0f; p.pos[3 * n + 1] = 0.0f; p.pos[3 * n + 2] = 0.0f;
            p.colI[3 * n + 0] = L.color.r * L.intensity;
            p.colI[3 * n + 1] = L.color.g * L.intensity;
            p.colI[3 * n + 2] = L.color.b * L.intensity;
            p.radius[n] = 0.0f;
            ++n;
        } else if (L.type == LightType::Point) {
            p.type[n] = 1;
            p.dir[3 * n + 0] = 0.0f; p.dir[3 * n + 1] = 0.0f; p.dir[3 * n + 2] = -1.0f;
            p.pos[3 * n + 0] = L.position.x; p.pos[3 * n + 1] = L.position.y; p.pos[3 * n + 2] = L.position.z;
            p.colI[3 * n + 0] = L.color.r * L.intensity;
            p.colI[3 * n + 1] = L.color.g * L.intensity;
            p.colI[3 * n + 2] = L.color.b * L.intensity;
            p.radius[n] = L.radius > 1e-3f ? L.radius : 1e-3f;
            ++n;
        }
        // Spot / unknown kinds: not evaluated, not silently dropped — they
        // never increment n, so the caller's ignored-count reports them.
    }

    if (n == 0) {
        // Documented default sun (D19: an empty list must not go black).
        p.type[0] = 0;
        p.dir[0] = 0.0f; p.dir[1] = -1.0f; p.dir[2] = 0.0f;
        p.colI[0] = 1.0f; p.colI[1] = 1.0f; p.colI[2] = 1.0f;
        n = 1;
    }

    shader.setInt("uLightCount", n);
    shader.setIntArray("uLightType", p.type, n);
    shader.setVec3Array("uLightDir", p.dir, n);
    shader.setVec3Array("uLightPos", p.pos, n);
    shader.setVec3Array("uLightColorI", p.colI, n);
    shader.setFloatArray("uLightRadius", p.radius, n);
    return n;
}

} // namespace ember::render
