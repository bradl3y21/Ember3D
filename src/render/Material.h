// Material.h — mapping from core types onto the scene shader's uniform slots.
//
// Lights are consumed as the packet's LIST (D19, ARCHITECTURE §4.1/§4.3):
// render reads `packet.lights`, never the deprecated `packet.light` — that
// field and `DirectionalLight` become deletable the moment this file stopped
// mentioning either. Point lights get radial falloff; anything render cannot
// evaluate is REPORTED, not silently dropped.
#pragma once

#include <vector>

#include "core/Types.h"
#include "render/Shader.h"

namespace ember::render {

class Material {
public:
    // Fixed maximum lights evaluated per frame (the shader's array size must
    // match — see kMaxLights in Renderer.cpp's scene shader). Entries beyond
    // this cap, or of a kind render does not evaluate, are returned as
    // ignored so the caller can log them (D19: never silent).
    static constexpr int kMaxLights = 8;

    // Push a MaterialDesc onto the scene shader's uniform slots, including its
    // linear emission radiance inputs.
    static void apply(Shader& shader, const MaterialDesc& desc);

    // Raster image resources are owned/cached by Renderer. These helpers only
    // map their bindings and enable bits to the scene shader contract.
    static void setBaseColorTexture(Shader& shader, bool enabled);
    static void setEnvironmentTexture(Shader& shader, bool enabled);

    // Evaluate the light list onto the shader's uniform arrays and return the
    // number of lights actually shaded. An EMPTY list evaluates a documented
    // default sun (straight down, white, 1.0) so an empty scene does not go
    // black. Unsupported kinds (e.g. Spot before it lands in core) are skipped.
    static int applyLights(Shader& shader, const std::vector<LightInstance>& lights);
};

} // namespace ember::render
