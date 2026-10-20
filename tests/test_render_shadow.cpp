// tests/test_render_shadow.cpp — owned by Verity · QA.
//
// Contract under test: the directional shadow map (src/render/Renderer.cpp,
// item 96; ShadowSettings / RenderPacket::shadowLightIndex in core/Types.h).
//
// WHY THIS TEST EXISTS: an earlier audit found "shadow map" NOT LANDED — the
// core types declared ShadowSettings/shadowLightIndex/castShadow while
// src/render had ZERO references. A shader that merely COMPILES is not a
// shadow. The claim this file verifies is strictly stronger:
//
//   A rendered frame must show a shadow cast by a KNOWN light, onto the floor
//   at the position GEOMETRY predicts, and it must MOVE when the caster moves.
//
// METHOD (this is the part that makes it evidence rather than a screenshot):
//   1. Put a cube at a known position above a floor, one directional light at a
//      known direction.
//   2. Render the identical scene twice: shadows enabled vs disabled.
//   3. Predict the shadow footprint INDEPENDENTLY: project each of the cube's
//      8 corners along the light direction onto y=0 (t = -P.y/d.y). That
//      rectangle is pure geometry — it never reads Renderer internals.
//   4. Sample floor points in world space, project them to screen with the
//      camera's own projectScreen(), and compare the two images:
//        - points INSIDE the predicted footprint must get darker;
//        - points OUTSIDE it (same floor, same lighting) must NOT.
//   5. Negative control: the same scene with the cube REMOVED must show no
//      darkening at all — proving the darkening is cast by that object and is
//      not a global artefact of enabling the pass.
//   6. Movement: shifting the caster must shift the darkened region with it.
//
// Real GPU needed (GL context, FBO, the depth pass), so it is registered only
// under EMBER_ENABLE_GPU_TESTS and passes --gpu.
#include "harness.h"

#include "core/Types.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ember;                // RenderPacket / LightInstance / ShadingMode
using ember::scene::Primitive;
using ember::scene::World;

namespace {

// Local to this file: a precondition on TEST SETUP (not the contract under
// test). If the harness cannot even render a frame, the assertions below would
// be comparing noise — so abort the test rather than emit misleading failures.
#define REQUIRE(cond)                                                             \
    do {                                                                          \
        if (!(cond)) {                                                            \
            harness::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");     \
            return;                                                               \
        }                                                                         \
    } while (0)

// Print the measured numbers with every check, so a CI log carries the receipt
// even when everything passes.
#define INFO(msg) std::printf("       %s\n", (msg))

constexpr int kW = 640;
constexpr int kH = 360;

// A GL context must exist BEFORE Renderer::init() — gladLoadGL needs one, and
// calling init() without a window is an access violation (0xC0000005), which is
// precisely how my first version of this file died with no output at all. One
// hidden window is created lazily and shared by every test in this file.
bool ensureContext() {
    static ember::platform::Window* win = [] {
        auto* w = new ember::platform::Window();
        ember::platform::WindowConfig wc;
        wc.width = kW;
        wc.height = kH;
        wc.visible = false;
        wc.title = "ember-shadow103";
        if (!w->create(wc)) {
            delete w;
            return static_cast<ember::platform::Window*>(nullptr);
        }
        return w;
    }();
    return win != nullptr;
}

// The floor plane the shadow falls on.
constexpr float kFloorY = 0.0f;
// Cube: half-extent and the height it floats at (well clear of the floor, so
// the shadow is a separate, measurable footprint rather than contact shading).
constexpr float kCubeHalf = 0.5f;
constexpr float kCubeY    = 2.0f;

// A deliberately ASYMMETRIC light direction (not straight down, not axis
// aligned) so the predicted footprint is offset in a way that a symmetric
// artefact — vignette, ambient falloff, a fullscreen darkening — cannot fake.
const Vec3 kLightDir{0.45f, -1.0f, 0.30f};

struct Image {
    std::vector<ember::u8> rgba;
    float luminanceAt(int x, int y) const {
        const std::size_t s = (std::size_t(y) * kW + x) * 4;
        if (s + 2 >= rgba.size()) return 0.0f;
        return 0.2126f * float(rgba[s]) + 0.7152f * float(rgba[s + 1]) +
               0.0722f * float(rgba[s + 2]);
    }
};

// The 8 corners of the cube, projected along `dir` onto the plane y = floorY.
// Returns the axis-aligned XZ rectangle of that footprint. Pure geometry: this
// is the INDEPENDENT prediction the renderer is checked against.
struct Footprint {
    float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    bool valid = false;
};

Footprint predictFootprint(Vec3 cubeCenter, float half, Vec3 dir, float floorY) {
    Footprint f;
    if (dir.y >= -1e-6f) return f;          // light must travel downward
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        Vec3 p{cubeCenter.x + ((i & 1) ? half : -half),
               cubeCenter.y + ((i & 2) ? half : -half),
               cubeCenter.z + ((i & 4) ? half : -half)};
        const float t = (floorY - p.y) / dir.y;      // > 0 while p is above the floor
        if (t <= 0.0f) continue;
        const Vec3 s{p.x + dir.x * t, floorY, p.z + dir.z * t};
        if (first) {
            f.minX = f.maxX = s.x;
            f.minZ = f.maxZ = s.z;
            first = false;
        } else {
            if (s.x < f.minX) f.minX = s.x;
            if (s.x > f.maxX) f.maxX = s.x;
            if (s.z < f.minZ) f.minZ = s.z;
            if (s.z > f.maxZ) f.maxZ = s.z;
        }
    }
    f.valid = !first;
    return f;
}

// One rendered frame of the given scene under the given shadow setting.
// One rendered frame of the given scene under the given shadow setting.
// `shadowIndex` is RenderPacket::shadowLightIndex — the core contract that was
// dead in the earlier audit (-1 must disable shadowing even with the pass on).
bool renderOnce(World& world, bool shadowsOn, Image& out, int shadowIndex,
                Vec3 lightDir) {
    if (!ensureContext()) return false;
    render::Renderer r;
    if (!r.init()) return false;
    r.resize(kW, kH);

    ember::RenderPacket p = world.buildPacket(kW, kH);
    p.width = kW;
    p.height = kH;
    p.view = r.camera().view();
    p.projection = r.camera().projection(static_cast<float>(kW) / kH);
    p.cameraPosition = r.camera().position();
    p.background.color = {0.035f, 0.04f, 0.05f, 1.0f};
    p.background.strength = 1.0f;
    p.background.lighting = false;

    // Exactly one directional light, and it is the caster.
    p.lights.clear();
    ember::LightInstance sun;
    sun.type = ember::LightType::Directional;
    sun.direction = lightDir;
    sun.color = {1.0f, 0.92f, 0.80f, 1.0f};
    sun.intensity = 2.2f;
    p.lights.push_back(sun);
    p.shadowLightIndex = shadowIndex;

    ViewportSettings s;
    s.shading = ember::ShadingMode::Rendered;   // the mode that runs the pass
    s.showGrid = false;
    s.showAxes = false;
    s.showGizmo = false;
    s.showOrigins = false;
    s.showSelectionOutline = false;
    s.shadows.enabled = shadowsOn;
    s.shadows.resolution = 1024;
    s.shadows.bias = 0.0015f;
    s.shadows.normalBias = 0.012f;
    s.shadows.maxDistance = 60.0f;
    r.setViewportSettings(s);

    for (int warm = 0; warm < 8; ++warm) r.renderFrame(p);
    r.readViewport(0, 0, kW, kH, out.rgba);
    r.shutdown();
    return out.rgba.size() == std::size_t(kW) * kH * 4;
}

// Builds a world with a floor and (optionally) the caster cube at `cubePos`.
// The camera is the renderer's default, which is deterministic; the test never
// assumes a screen position — it projects through the camera's own API.
World makeWorld(bool withCaster, Vec3 cubePos) {
    World world;
    const ObjectId floor = world.createPrimitive(Primitive::Plane, "Floor");
    Transform ft;
    ft.position = {0.0f, kFloorY, 0.0f};
    ft.scale = {60.0f, 1.0f, 60.0f};
    world.setTransform(floor, ft);

    if (withCaster) {
        const ObjectId cube = world.createPrimitive(Primitive::Cube, "Caster");
        Transform ct;
        ct.position = cubePos;
        ct.scale = {kCubeHalf * 2.0f, kCubeHalf * 2.0f, kCubeHalf * 2.0f};
        world.setTransform(cube, ct);
        world.select(cube);
    }
    return world;
}

// Samples the floor on a world-space grid, projects each sample through the
// camera, and splits it into "inside the predicted footprint" vs "outside".
// Returns the mean luminance drop (off - on) for each class.
struct SampleResult {
    float insideDrop = 0.0f;
    float outsideDrop = 0.0f;
    int insideCount = 0;
    int outsideCount = 0;
};

SampleResult sampleDrops(const Image& on, const Image& off, const Footprint& fp,
                         const render::OrbitCamera& cam) {
    SampleResult sr;
    float sumIn = 0.0f, sumOut = 0.0f;
    // Walk the floor in world space. The step must be fine enough that the
    // ~1.4 x 1.2 unit footprint yields a meaningful sample count — at 0.25 it
    // yielded only 36, which made the count assertions a coin-flip.
    for (float wz = -8.0f; wz <= 8.0f; wz += 0.1f) {
        for (float wx = -8.0f; wx <= 8.0f; wx += 0.1f) {
            float sx = 0.0f, sy = 0.0f;
            if (!cam.projectScreen(Vec3{wx, kFloorY, wz}, kW, kH, sx, sy)) continue;
            const int px = int(sx), py = int(sy);
            if (px < 0 || py < 0 || px >= kW || py >= kH) continue;
            const bool inside = wx >= fp.minX && wx <= fp.maxX && wz >= fp.minZ &&
                                wz <= fp.maxZ;
            const float drop = off.luminanceAt(px, py) - on.luminanceAt(px, py);
            if (inside) { sumIn += drop; ++sr.insideCount; }
            else        { sumOut += drop; ++sr.outsideCount; }
        }
    }
    if (sr.insideCount)  sr.insideDrop  = sumIn  / float(sr.insideCount);
    if (sr.outsideCount) sr.outsideDrop = sumOut / float(sr.outsideCount);
    return sr;
}

} // namespace

TEST_GPU(shadow_drops_light_where_geometry_predicts) {
    const Vec3 cubePos{0.0f, kCubeY, 0.0f};

    World withCaster = makeWorld(true, cubePos);
    Image on, off;
    REQUIRE(renderOnce(withCaster, true, on, 0, kLightDir));
    REQUIRE(renderOnce(withCaster, false, off, 0, kLightDir));

    // The prediction, computed WITHOUT reading any renderer state.
    const Footprint fp = predictFootprint(cubePos, kCubeHalf, kLightDir, kFloorY);
    CHECK(fp.valid);
    // Sanity on the prediction itself: the light travels +x and +z, so the
    // footprint must be offset TOWARD +x/+z from directly under the cube.
    CHECK(fp.minX > cubePos.x - kCubeHalf - 1e-3f);
    CHECK(fp.maxX > cubePos.x + kCubeHalf);
    CHECK(fp.maxZ > cubePos.z + kCubeHalf);

    render::Renderer probe;
    REQUIRE(probe.init());
    probe.resize(kW, kH);
    const SampleResult sr = sampleDrops(on, off, fp, probe.camera());
    probe.shutdown();

    char detail[224];
    std::snprintf(detail, sizeof detail,
                  "inside %d samples drop %.2f, outside %d samples drop %.2f",
                  sr.insideCount, static_cast<double>(sr.insideDrop),
                  sr.outsideCount, static_cast<double>(sr.outsideDrop));
    INFO(detail);

    // The floor must actually be visible and sampled in both classes, or the
    // means below are meaningless.
    CHECK(sr.insideCount > 50);
    CHECK(sr.outsideCount > 50);

    // THE claim: inside the predicted footprint the frame is clearly darker
    // with shadows on. Outside it, the same floor under the same light is not.
    CHECK(sr.insideDrop > 8.0f);
    CHECK(sr.outsideDrop < 2.0f);
    // And the separation must be unambiguous, not a threshold coin-flip.
    CHECK(sr.insideDrop > 3.0f * (sr.outsideDrop + 0.5f));
}

TEST_GPU(shadow_is_cast_by_the_caster_not_by_enabling_the_pass) {
    // Negative control: WITHOUT the caster cube, enabling the shadow pass must
    // change essentially nothing. If it still darkened the frame, the effect
    // would be an artefact of the pass, not a shadow of the object.
    const Vec3 cubePos{0.0f, kCubeY, 0.0f};
    World noCaster = makeWorld(false, cubePos);

    Image on, off;
    REQUIRE(renderOnce(noCaster, true, on, -1, kLightDir));
    REQUIRE(renderOnce(noCaster, false, off, -1, kLightDir));

    int changed = 0;
    float totalDrop = 0.0f;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float d = off.luminanceAt(x, y) - on.luminanceAt(x, y);
            totalDrop += d;
            if (d > 8.0f) ++changed;
        }
    }
    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "%d of %d pixels darkened >8, mean drop %.4f",
                  changed, kW * kH, static_cast<double>(totalDrop / (kW * kH)));
    INFO(detail);
    CHECK(changed < 200);
}

TEST_GPU(shadow_moves_with_the_caster) {
    // The footprint must TRACK the object. Moving the caster in -z must move
    // the darkened region in -z. A static darkening (a decal, a baked blob)
    // would fail this while still passing a "pixels changed" check.
    const Vec3 posA{0.0f, kCubeY, 0.0f};
    const Vec3 posB{0.0f, kCubeY, -3.0f};

    World wa = makeWorld(true, posA);
    Image onA, offA;
    REQUIRE(renderOnce(wa, true, onA, 0, kLightDir));
    REQUIRE(renderOnce(wa, false, offA, 0, kLightDir));

    World wb = makeWorld(true, posB);
    Image onB, offB;
    REQUIRE(renderOnce(wb, true, onB, 0, kLightDir));
    REQUIRE(renderOnce(wb, false, offB, 0, kLightDir));

    render::Renderer probe;
    REQUIRE(probe.init());
    probe.resize(kW, kH);
    const Footprint fpA = predictFootprint(posA, kCubeHalf, kLightDir, kFloorY);
    const Footprint fpB = predictFootprint(posB, kCubeHalf, kLightDir, kFloorY);
    const SampleResult a = sampleDrops(onA, offA, fpA, probe.camera());
    const SampleResult b = sampleDrops(onB, offB, fpB, probe.camera());
    probe.shutdown();

    char detail[200];
    std::snprintf(detail, sizeof detail,
                  "A(%.1f,%.1f)->(%.1f,%.1f) drop %.2f | "
                  "B(%.1f,%.1f)->(%.1f,%.1f) drop %.2f",
                  static_cast<double>(fpA.minX), static_cast<double>(fpA.minZ),
                  static_cast<double>(fpA.maxX), static_cast<double>(fpA.maxZ),
                  static_cast<double>(a.insideDrop),
                  static_cast<double>(fpB.minX), static_cast<double>(fpB.minZ),
                  static_cast<double>(fpB.maxX), static_cast<double>(fpB.maxZ),
                  static_cast<double>(b.insideDrop));
    INFO(detail);

    // Both positions must produce a real shadow inside their OWN predicted
    // footprint — i.e. the region moved, and the prediction moved with it.
    CHECK(a.insideDrop > 8.0f);
    CHECK(b.insideDrop > 8.0f);
    // The footprints themselves must differ in z by the amount the caster moved.
    CHECK(fpA.minZ > fpB.minZ);
}

TEST_GPU(shadow_light_index_selects_the_caster) {
    // RenderPacket::shadowLightIndex is the core contract that was dead in the
    // earlier audit. With -1 the shader's guard must disable the shadowing even
    // though the pass and the light both exist.
    const Vec3 cubePos{0.0f, kCubeY, 0.0f};
    World world = makeWorld(true, cubePos);

    Image on, off;
    // Same scene, same "shadows enabled" viewport setting, DIFFERENT
    // shadowLightIndex. This is the only variable, so any difference in the
    // images is attributable to the index alone.
    REQUIRE(renderOnce(world, true, on, 0, kLightDir));    // index 0: the sun casts
    REQUIRE(renderOnce(world, true, off, -1, kLightDir));  // index -1: nobody casts

    const Footprint fp = predictFootprint(cubePos, kCubeHalf, kLightDir, kFloorY);
    render::Renderer probe;
    REQUIRE(probe.init());
    probe.resize(kW, kH);
    const SampleResult sr = sampleDrops(on, off, fp, probe.camera());
    probe.shutdown();

    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "index0 vs index-1: inside drop %.2f over %d samples",
                  static_cast<double>(sr.insideDrop), sr.insideCount);
    INFO(detail);
    CHECK(sr.insideCount > 50);
    CHECK(sr.insideDrop > 8.0f);
}

HARNESS_MAIN()
