// tests/test_render_alpha.cpp — owned by Verity · QA. Item 214, closing item
// 158's alpha gap (docs/ROADMAP.md gap #13).
//
// WHY THIS FILE EXISTS. Transparency blending code landed in item 143
// (src/render/Renderer.cpp: an opaque pass; transparent surfaces stable-sorted
// far-to-near by view depth; depth writes off; glBlendFuncSeparate source-over).
// A pass that EXISTS is not a pass that is CORRECT, and item 158's original
// pixel file mixed alpha with material slots, so its board result cited
// unrelated evidence and its S2 slot case fails on purpose. Roadmap gap #13:
// "no dedicated alpha test file in tests/ ... treat visual alpha correctness as
// unverified." This is that dedicated file: one controlled fixture, three
// properties, every number printed.
//
// THE CONTROLLED FIXTURE. Planes are built facing the camera (the Plane
// primitive's normal is +Y; a +90° X rotation points it at +Z, and the Front
// view looks down -Z). Each plane's alpha is the ONLY thing that varies between
// shots — light, camera, background, geometry and material colour are identical.
//
// THE PREDICTION IS BUILT FROM OTHER RENDERS, NOT FROM RENDERER INTERNALS.
// Source-over compositing of a layer with own colour F and alpha a over a
// backdrop S is  R = a*F + (1-a)*S. We do not know F or S analytically: F is the
// layer rendered OPAQUELY (alpha 1) and S is the backdrop rendered alone, both
// through the very same shading path, so lighting, exposure and tone cancel.
// The alpha channel of MaterialDesc (D12) is the only input we vary.
//
//   before := layer alpha 1.0 over the backdrop  -> must equal F (it HIDES S)
//   after  := layer alpha 0.5 over the backdrop  -> must equal 0.5*F + 0.5*S
//
// Needs a GL context: registered only under EMBER_ENABLE_GPU_TESTS, run with
// --gpu (tests/README.md hard rule).
//
// RESIDUE — what this does NOT prove: it reads 8-bit back-buffer RGB, so it
// cannot see sub-1/255 blend error, and it does not exercise destination alpha
// or a framebuffer with an sRGB-encoded blend. It proves RGB source-over at the
// sampled patch for the fixture below, nothing about texture alpha (no textures
// exist — D12) or per-object opacity (out of scope — D12).
#include "harness.h"

#include "core/Types.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ember;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                          \
        ++harness::g_checks;                                                      \
        if (!(cond))                                                              \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kW = 640;
constexpr int kH = 360;
constexpr float kHalfPi = 1.57079632679f;
const int kCx = kW / 2, kCy = kH / 2;

bool ensureContext() {
    static ember::platform::Window* win = [] {
        auto* w = new ember::platform::Window();
        ember::platform::WindowConfig wc;
        wc.width = kW;
        wc.height = kH;
        wc.visible = false;
        wc.title = "ember-alpha-pixels";
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

// A read-back frame. `at()` averages a 5x5 patch so a single anti-aliased edge
// pixel cannot move a result (there is no MSAA — this is belt and braces).
struct Frame {
    std::vector<u8> rgba;
    static float srgbToLinear(float v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    static float inverseReinhard(float v) {
        v = std::min(v, 0.999f);
        return v / (1.0f - v);
    }
    Rgb at(int cx, int cy) const {
        Rgb s; int n = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
                const std::size_t i = (std::size_t(y) * kW + std::size_t(x)) * 4;
                s.r += 255.0f * inverseReinhard(srgbToLinear(rgba[i] / 255.0f));
                s.g += 255.0f * inverseReinhard(srgbToLinear(rgba[i + 1] / 255.0f));
                s.b += 255.0f * inverseReinhard(srgbToLinear(rgba[i + 2] / 255.0f));
                ++n;
            }
        return n ? (1.0f / float(n)) * s : s;
    }
};

// One facing plane in the fixture. `a` is the D12 alpha channel — the only knob
// the tests turn. `z` is world depth (camera at +Z looking down -Z).
struct Layer {
    float r, g, b, a;
    float z;
    float size;
};

MaterialDesc materialOf(const Layer& l) {
    MaterialDesc m;
    m.baseColor = {l.r, l.g, l.b, l.a};
    m.metallic = 0.0f;
    m.roughness = 0.8f;
    return m;
}

// Render the fixture and sample the centre pixel. Deterministic: four frames
// are drawn and the last is read, matching every other render QA test.
bool shoot(const std::vector<Layer>& layers, Rgb& out) {
    if (!ensureContext()) return false;
    auto world = std::make_unique<World>();
    for (const Layer& l : layers) {
        const ObjectId id = world->createPrimitive(Primitive::Plane, "layer");
        Transform t{};
        t.position = {0.0f, 0.0f, l.z};
        t.rotation = {kHalfPi, 0.0f, 0.0f};   // +Y normal -> +Z, facing the camera
        t.scale = {l.size, 1.0f, l.size};
        world->setTransform(id, t);
        world->setMaterial(id, materialOf(l));
    }

    render::Renderer r;
    if (!r.init()) return false;
    r.resize(kW, kH);
    r.camera().setView(render::StandardView::Front);

    RenderPacket p = world->buildPacket(kW, kH);
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
    sun.direction = {-0.3f, -0.5f, -1.0f};   // reaches the +Z face and a little from the left
    sun.color = {1.0f, 1.0f, 1.0f, 1.0f};
    sun.intensity = 3.0f;
    p.lights.push_back(sun);
    p.shadowLightIndex = -1;

    ViewportSettings s;
    s.shading = ShadingMode::Rendered;
    s.showGrid = false; s.showAxes = false; s.showGizmo = false; s.showOrigins = false;
    s.showSelectionOutline = false;
    s.shadows.enabled = false;
    r.setViewportSettings(s);
    for (int i = 0; i < 4; ++i) r.renderFrame(p);

    std::vector<u8> pixels;
    r.readViewport(0, 0, kW, kH, pixels);
    r.shutdown();
    if (pixels.size() != std::size_t(kW) * kH * 4) return false;
    Frame f; f.rgba = std::move(pixels);
    out = f.at(kCx, kCy);
    return true;
}

// The fixture. Backdrop is large and opaque; the layers sit in front of it,
// smaller, so every sampled patch is strictly inside each plane. Readback is
// inverted through the fixed sRGB/Reinhard display transform so the assertions
// continue to test source-over in the linear HDR scene target.
const Layer kBackdrop{0.1f, 0.2f, 0.9f, 1.0f, -2.0f, 9.0f};   // blue, opaque
const Layer kFront   {0.9f, 0.1f, 0.1f, 1.0f,  0.0f, 3.0f};   // red; alpha is the knob

Layer frontWithAlpha(float a) { Layer l = kFront; l.a = a; return l; }

// Source-over, written once so every test predicts with the same expression.
Rgb sourceOver(Rgb foreground, float a, Rgb backdrop) {
    return a * foreground + (1.0f - a) * backdrop;
}

} // namespace

// ============================================================================
// The named proof (item 214 contract symbol).
// ============================================================================
TEST_GPU(test_transparency_blends_source_over) {
    Rgb S, F, before, after;                       // backdrop, layer-alone, alpha 1, alpha 0.5
    CHECK_MSG(shoot({kBackdrop}, S), "render failed: backdrop alone");
    CHECK_MSG(shoot({kBackdrop, frontWithAlpha(1.0f)}, before), "render failed: opaque layer over backdrop");
    CHECK_MSG(shoot({frontWithAlpha(1.0f)}, F), "render failed: opaque layer alone (the layer's own colour)");
    CHECK_MSG(shoot({kBackdrop, frontWithAlpha(0.5f)}, after), "render failed: half-alpha layer over backdrop");

    const Rgb predicted = sourceOver(F, 0.5f, S);
    std::printf("       source-over: backdrop(alone) S=%s  layer(alone,opaque) F=%s\n",
                str(S).c_str(), str(F).c_str());
    std::printf("       before (layer alpha 1.00 over backdrop) = %s\n", str(before).c_str());
    std::printf("       after  (layer alpha 0.50 over backdrop) = %s   predicted 0.5*F+0.5*S = %s\n",
                str(after).c_str(), str(predicted).c_str());

    // Setup: the two surfaces must be far apart in colour, or a blend of them
    // could not be told apart from either.
    CHECK_MSG(sumAbs(S - F) > 150.0f,
              "setup: backdrop %s and layer %s are not distinguishable", str(S).c_str(), str(F).c_str());
    // Blending must actually do something: the 0.5 layer must not look like the
    // opaque one.
    CHECK_MSG(sumAbs(after - before) > 100.0f,
              "a 0.5-alpha layer renders %s, essentially the opaque layer %s: no blending",
              str(after).c_str(), str(before).c_str());
    // An OPAQUE layer hides the backdrop entirely (control: the prediction below
    // is not satisfied by some other effect).
    CHECK_MSG(maxAbs(before - F) <= 6.0f,
              "an opaque layer must hide the backdrop: got %s, the layer's own colour is %s",
              str(before).c_str(), str(F).c_str());
    // The alpha claim itself.
    CHECK_MSG(maxAbs(after - predicted) <= 10.0f,
              "alpha 0.5 renders %s; source-over of %s at 0.5 over %s predicts %s (max channel error %.1f)",
              str(after).c_str(), str(F).c_str(), str(S).c_str(), str(predicted).c_str(),
              double(maxAbs(after - predicted)));
}

// ============================================================================
// The sweep: source-over holds at the endpoints too.
// ============================================================================
TEST_GPU(alpha_opacity_sweep_matches_source_over_endpoints_included) {
    Rgb S, F;
    CHECK_MSG(shoot({kBackdrop}, S), "render failed: backdrop alone");
    CHECK_MSG(shoot({kBackdrop, frontWithAlpha(1.0f)}, F), "render failed: opaque layer");
    CHECK_MSG(sumAbs(S - F) > 150.0f, "setup: surfaces not distinguishable");

    const float alphas[] = {0.0f, 0.25f, 0.50f, 0.75f, 1.0f};
    for (float a : alphas) {
        Rgb got;
        CHECK_MSG(shoot({kBackdrop, frontWithAlpha(a)}, got), "render failed at alpha %.2f", double(a));
        const Rgb predicted = sourceOver(F, a, S);
        std::printf("       alpha %.2f -> %s   predicted %s\n", double(a), str(got).c_str(), str(predicted).c_str());
        // a == 0 must be exactly the backdrop (the renderer skips it entirely);
        // a == 1 must be exactly the layer. Same tolerance: one prediction.
        CHECK_MSG(maxAbs(got - predicted) <= 10.0f,
                  "alpha %.2f renders %s; source-over predicts %s (max channel error %.1f)",
                  double(a), str(got).c_str(), str(predicted).c_str(), double(maxAbs(got - predicted)));
    }
}

// ============================================================================
// Two transparent layers: far-to-near compositing, and the ORDER is observable.
// ============================================================================
TEST_GPU(two_transparent_layers_composite_far_to_near) {
    // backdrop (opaque) at z=-3, a green layer at z=-1.5, a red layer at z=0,
    // both transparent at 0.5.
    const Layer far {0.1f, 0.9f, 0.1f, 0.5f, -1.5f, 7.0f};   // green, behind
    const Layer near{0.9f, 0.1f, 0.1f, 0.5f,  0.0f, 3.0f};   // red, in front
    const Layer back{0.1f, 0.2f, 0.9f, 1.0f, -3.0f, 12.0f};

    Rgb S, G, R, got;
    CHECK_MSG(shoot({back}, S), "render failed: backdrop alone");
    CHECK_MSG(shoot({back, Layer{far.r, far.g, far.b, 1.0f, far.z, far.size}}, G),
              "render failed: green layer opaque over backdrop");
    CHECK_MSG(shoot({back, Layer{near.r, near.g, near.b, 1.0f, near.z, near.size}}, R),
              "render failed: red layer opaque over backdrop");
    CHECK_MSG(shoot({back, far, near}, got), "render failed: both transparent layers");

    // far-to-near: composite the FAR layer first, then the NEAR one over it.
    //   farSide  = 0.5*G + 0.5*S
    //   expected = 0.5*R + 0.5*farSide = 0.5*R + 0.25*G + 0.25*S
    // near-first (the wrong order) would give 0.5*G + 0.25*R + 0.25*S — a
    // different colour, so this test can tell the orders apart.
    const Rgb farSide  = sourceOver(G, 0.5f, S);
    const Rgb expected = sourceOver(R, 0.5f, farSide);
    // near-first (the wrong order): red composited first, then green over it.
    const Rgb wrongOrder = sourceOver(G, 0.5f, sourceOver(R, 0.5f, S));
    std::printf("       backdrop %s  green-opaque %s  red-opaque %s\n", str(S).c_str(), str(G).c_str(), str(R).c_str());
    std::printf("       two 0.5 layers -> %s   far-to-near predicts %s   near-first would be %s\n",
                str(got).c_str(), str(expected).c_str(), str(wrongOrder).c_str());

    CHECK_MSG(sumAbs(S - G) > 120.0f && sumAbs(S - R) > 120.0f,
              "setup: layers and backdrop not distinguishable");
    CHECK_MSG(sumAbs(expected - wrongOrder) > 40.0f,
              "the fixture cannot distinguish the two orders (far-to-near %s vs near-first %s)",
              str(expected).c_str(), str(wrongOrder).c_str());
    CHECK_MSG(maxAbs(got - expected) <= 12.0f,
              "two transparent layers render %s; far-to-near source-over predicts %s (near-first would be %s)",
              str(got).c_str(), str(expected).c_str(), str(wrongOrder).c_str());
}

HARNESS_MAIN()
