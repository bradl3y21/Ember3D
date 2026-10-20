// tests/test_render_audit.cpp - owned by Verity / QA. Item 160.
//
// THE PERMANENT RENDER-CLAIMS AUDIT. Slate wrote a temporary public-API pixel harness
// (audit138.cpp, outside the repo) that probed the wave 1-2 RENDER claims by rendering real
// frames and reading pixels back, and that deliberately exited 1 because five claims were
// NOT LANDED. It was the first time on this project that an overclaim (a 'done' column, a
// header comment, a roadmap row) was caught by a MACHINE instead of a human reading a
// screenshot. This file keeps that, as a permanent test:
//
//   * every probe is ported with its ORIGINAL logic and thresholds (nothing was weakened to
//     make a claim pass);
//   * the five findings that were open are each their OWN named test, FINDING_1..FINDING_5.
//     Each FAILS while the claim is not true of the pixels and PASSES the moment it is -
//     a fix flips exactly one test green, and nobody can reopen it silently because the
//     test is the standing proof;
//   * the probes that were BACKED stay as regression guards, so landing one fix cannot
//     quietly break a claim that already held;
//   * every FINDING prints OPEN or CLOSED, so a log line (not a screenshot) says where it is;
//   * controls prove the predicates can tell the difference: the same scene rendered twice
//     is pixel-identical (a non-zero diff is a real difference, not noise), and the
//     predicates are exercised on synthetic images with known answers.
//
// The five findings (state when this was ported - re-measured, see the item-160 finding):
//   FINDING_1  Solid vs Rendered are pixel-distinct with shadows off and the default world
//              (item 97 'the four shading modes are pixel-distinct')
//   FINDING_2  baseColor.a changes the picture (transparency; D12, item 143)
//   FINDING_3  a differing slot-0 material in DrawItem::materials changes the picture (D22)
//   FINDING_4  cold start with shadows ON and no light says WHY there is no shadow (warning)
//   FINDING_5  the point-light star fits within lightMarkerRadiusPixels() (Renderer.h comment)
//
// Reads the Renderer ONLY through its public API (renderFrame / readViewport / drawGizmo /
// lastStats / camera) plus the renderer's stderr warnings. Needs a GL context: registered only
// under EMBER_ENABLE_GPU_TESTS, run with --gpu.
#include "harness.h"

#include <glad/gl.h>

#include "core/Math.h"
#include "core/Types.h"
#include "editor/Editor.h"
#include "editor/Undo.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace ember;

namespace {

// A claim that does not hold of the pixels. The prefix is what a reader greps for.
#define CLAIM(cond, msg)                                                                     \
    do {                                                                                     \
        ++harness::g_checks;                                                                 \
        if (!(cond)) harness::reportFailure(__FILE__, __LINE__, std::string("NOT LANDED: ") + (msg)); \
    } while (0)
#define NOTE(...)                                  \
    do {                                           \
        std::printf("       ");                    \
        std::printf(__VA_ARGS__);                  \
        std::printf("\n");                         \
    } while (0)

constexpr int W = 480, H = 320;
constexpr int FBW = 800, FBH = 600;

struct Img {
    std::vector<u8> px;
    int w = W, h = H;
    void rgb(int x, int y, int& r, int& g, int& b) const {
        const std::size_t s = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
        r = px[s]; g = px[s + 1]; b = px[s + 2];
    }
    float lum(int x, int y) const {
        int r, g, b; rgb(x, y, r, g, b);
        return 0.2126f * float(r) + 0.7152f * float(g) + 0.0722f * float(b);
    }
};

int diffPixels(const Img& a, const Img& b, int tol = 2) {
    int n = 0;
    for (std::size_t i = 0; i + 3 < a.px.size() && i + 3 < b.px.size(); i += 4)
        if (std::abs(int(a.px[i]) - int(b.px[i])) > tol || std::abs(int(a.px[i + 1]) - int(b.px[i + 1])) > tol ||
            std::abs(int(a.px[i + 2]) - int(b.px[i + 2])) > tol)
            ++n;
    return n;
}

struct Spec {
    std::vector<LightInstance> lights;
    int shadowIdx = -1;
    bool cube = true, floor = true;
    bool cubeSelected = false;
    bool castShadow = true;
    Color cubeColor{0.8f, 0.2f, 0.2f, 1.0f};
    WorldBackground bg;
    bool useSlots = false;
};

LightInstance dirLight(Vec3 d, float inten = 1.0f, Color c = {1, 1, 1, 1}) {
    LightInstance l; l.type = LightType::Directional; l.direction = d; l.intensity = inten; l.color = c; return l;
}
LightInstance pointLight(Vec3 p, float inten, float radius) {
    LightInstance l; l.type = LightType::Point; l.position = p; l.intensity = inten; l.radius = radius; return l;
}

MeshData gCube, gPlane;
MaterialSlot gSlot;
std::string gLogPath;
platform::Window* gWin = nullptr;

// stderr carries the renderer's warnings. Redirect it ONCE to a file we can scrape (harness
// output goes to stdout, so test results are untouched).
bool ensureContext() {
    if (gWin) return true;
    const auto dir = std::filesystem::temp_directory_path() / "opencode";
    std::filesystem::create_directories(dir);
    gLogPath = (dir / "render_audit.log").string();
    std::freopen(gLogPath.c_str(), "wb", stderr);
    auto* win = new platform::Window();
    platform::WindowConfig wc;
    wc.width = FBW; wc.height = FBH; wc.visible = false; wc.title = "render-audit";
    if (!win->create(wc)) { delete win; return false; }
    gWin = win;
    gCube = scene::makePrimitiveMesh(scene::Primitive::Cube);
    gPlane = scene::makePrimitiveMesh(scene::Primitive::Plane);
    return true;
}

RenderPacket makePacket(render::Renderer& r, const Spec& s, int w = W, int h = H) {
    RenderPacket p;
    p.width = w; p.height = h;
    p.view = r.camera().view();
    p.projection = r.camera().projection(float(w) / float(h));
    p.cameraPosition = r.camera().position();
    p.background = s.bg;
    p.lights = s.lights;
    p.shadowLightIndex = s.shadowIdx;
    if (s.floor) {
        DrawItem it; it.object = ObjectId{2}; it.model = scaling(Vec3{8, 1, 8});
        it.mesh = &gPlane; it.material.baseColor = {0.5f, 0.5f, 0.5f, 1};
        it.material.metallic = 0.0f; it.material.roughness = 0.8f;
        it.castShadow = true; it.receiveShadow = true;
        p.items.push_back(it);
    }
    if (s.cube) {
        DrawItem it; it.object = ObjectId{1}; it.model = translation(Vec3{0, 0.75f, 0});
        it.mesh = &gCube; it.material.baseColor = s.cubeColor;
        it.material.metallic = 0.0f; it.material.roughness = 0.6f;
        it.selected = s.cubeSelected; it.castShadow = s.castShadow; it.receiveShadow = true;
        if (s.useSlots) {
            gSlot.name = "slot0";
            gSlot.material.baseColor = {0.1f, 0.9f, 0.1f, 1};
            it.materials = &gSlot; it.materialCount = 1;
        }
        p.items.push_back(it);
    }
    return p;
}

ViewportSettings quiet(ShadingMode m) {
    ViewportSettings v;
    v.shading = m; v.showGrid = false; v.showAxes = false; v.showGizmo = false;
    v.showOrigins = false; v.showSelectionOutline = false;
    return v;
}

Img shoot(render::Renderer& r, const ViewportSettings& st, const Spec& s, int frames = 1) {
    Img img; img.w = W; img.h = H;
    r.setViewportSettings(st);
    for (int i = 0; i < frames; ++i) {
        r.setViewportRect(Rect{});
        r.resize(W, H);
        r.renderFrame(makePacket(r, s));
    }
    r.readViewport(0, 0, W, H, img.px);
    return img;
}

bool project(render::Renderer& r, Vec3 p, int& px, int& py) {
    float x, y;
    if (!r.camera().projectScreen(p, W, H, x, y)) return false;
    px = int(x); py = int(y);
    return px >= 0 && py >= 0 && px < W && py < H;
}

void mark(const char* name) { std::fprintf(stderr, "### PROBE %s\n", name); std::fflush(stderr); }

int countLog(const char* needle, const char* afterMarker) {
    std::fflush(stderr);
    std::FILE* f = std::fopen(gLogPath.c_str(), "rb");
    if (!f) return -1;
    std::string all; char buf[4096]; std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);
    std::size_t start = all.rfind(std::string("### PROBE ") + afterMarker);
    if (start == std::string::npos) start = 0;
    int c = 0; std::size_t pos = start;
    while ((pos = all.find(needle, pos)) != std::string::npos) { ++c; pos += std::strlen(needle); }
    return c;
}

std::unique_ptr<render::Renderer> freshRenderer() {
    auto r = std::make_unique<render::Renderer>();
    if (!r->init()) { std::printf("renderer init failed\n"); std::exit(2); }
    r->resize(W, H);
    return r;
}

const Vec3 kDir{0.45f, -1.0f, 0.30f};

// Prints one machine-readable line per finding, so the state is a log line, not a screenshot.
void state(int n, const char* what, bool closed) {
    std::printf("       FINDING_%d %s: %s\n", n, closed ? "CLOSED" : "OPEN  ", what);
}

} // namespace

// =================================================================================
// THE FIVE FINDINGS
// =================================================================================

TEST_GPU(FINDING_1_solid_and_rendered_are_pixel_distinct_with_shadows_off) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir, 2.0f)}; s.shadowIdx = 0;
    ViewportSettings solidV = quiet(ShadingMode::Solid), rendV = quiet(ShadingMode::Rendered);
    solidV.shadows.enabled = false; rendV.shadows.enabled = false;
    const Img solid = shoot(*r, solidV, s), rend = shoot(*r, rendV, s);
    const int sr = diffPixels(solid, rend);
    NOTE("Solid vs Rendered (shadows OFF, default world): %d differing px", sr);
    if (sr == 0) NOTE("Solid and Rendered are IDENTICAL until shadows or world lighting are on");
    state(1, "item 97 'pixel-distinct': Solid vs Rendered differ with shadows off", sr > 100);
    CLAIM(sr > 100, "Solid differs from Rendered with shadows OFF and default world (item 97 'pixel-distinct')");
}

TEST_GPU(FINDING_2_base_color_alpha_changes_the_picture) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir, 2.0f)};
    s.cubeColor = {0.8f, 0.2f, 0.2f, 1.0f};
    const Img opaque = shoot(*r, quiet(ShadingMode::Rendered), s);
    s.cubeColor.a = 0.2f;
    const Img ghost = shoot(*r, quiet(ShadingMode::Rendered), s);
    const int d = diffPixels(opaque, ghost);
    NOTE("alpha 1.0 vs 0.2: %d differing px", d);
    state(2, "baseColor.a changes the picture (blending enabled)", d > 100);
    CLAIM(d > 100, "baseColor.a changes the picture (blending enabled)");
}

TEST_GPU(FINDING_3_a_differing_slot_0_material_changes_the_picture) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir, 2.0f)}; s.floor = false;
    const Img legacy = shoot(*r, quiet(ShadingMode::Solid), s);
    s.useSlots = true;
    const Img slots = shoot(*r, quiet(ShadingMode::Solid), s);
    const int d = diffPixels(legacy, slots);
    NOTE("slot0=green, legacy material=red: %d differing px", d);
    state(3, "DrawItem::materials slot 0 is consumed by render (D22)", d > 100);
    CLAIM(d > 100, "a differing slot-0 material changes the picture");
}

TEST_GPU(FINDING_4_cold_start_with_shadows_on_and_no_light_says_why_there_is_no_shadow) {
    CLAIM(ensureContext(), "no GL context");
    ViewportSettings v = quiet(ShadingMode::Rendered);
    v.shadows.enabled = true;
    auto r = freshRenderer();
    mark("shadowwarn");
    Spec s; s.lights = {}; s.shadowIdx = -1;
    shoot(*r, v, s, 3);
    const int n = countLog("no valid shadowLightIndex", "shadowwarn");
    NOTE("cold start, no light: 'no valid shadowLightIndex' warnings = %d", n);
    state(4, "COLD START with shadows on and no light warns why", n >= 1);
    CLAIM(n >= 1, "COLD START with shadows on and no light says why there is no shadow");
}

TEST_GPU(FINDING_5_point_light_star_fits_within_lightMarkerRadiusPixels) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    const Vec3 at{2.0f, 2.0f, 0.0f};
    Spec none; none.cube = false; none.floor = false;
    ViewportSettings off = quiet(ShadingMode::Solid), mOn = off;
    mOn.showGizmo = true;
    shoot(*r, off, none);                      // establishes the camera / projection
    int lx, ly;
    CLAIM(project(*r, at, lx, ly), "cannot project the light position");
    Spec pl; pl.cube = false; pl.floor = false;
    pl.lights = {pointLight(at, 1.0f, 5.0f)};
    pl.lights[0].color = {0, 0, 0, 1};
    const Img pk = shoot(*r, mOn, pl);
    float pmax = 0; int pn = 0;
    for (int y = 0; y < pk.h; ++y)
        for (int x = 0; x < pk.w; ++x)
            if (pk.lum(x, y) > 150.0f) { ++pn; pmax = std::fmax(pmax, std::hypot(float(x - lx), float(y - ly))); }
    const float accessor = render::Renderer::lightMarkerRadiusPixels();
    NOTE("point star: %d bright px, farthest %.1f px (accessor %.1f)", pn, double(pmax), double(accessor));
    const bool fits = pn > 8 && pmax <= accessor + 1.5f;
    state(5, "the point-light star fits within lightMarkerRadiusPixels()", fits);
    CLAIM(fits, "the point-light star fits within lightMarkerRadiusPixels() (Renderer.h/.cpp comment: 'fits within')");
}

// =================================================================================
// REGRESSION GUARDS: probes that were BACKED. Landing a fix must not break these.
// =================================================================================

TEST_GPU(guard_world_background_clears_with_color_times_strength) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.cube = false; s.floor = false;
    s.bg.color = {0.2f, 0.4f, 0.6f, 1.0f}; s.bg.strength = 0.5f;
    const Img a = shoot(*r, quiet(ShadingMode::Solid), s);
    int R, G, B; a.rgb(5, 5, R, G, B);
    NOTE("display pixel(5,5) = (%d,%d,%d), expected tone-mapped (85,113,132)", R, G, B);
    CLAIM(std::abs(R - 85) <= 2 && std::abs(G - 113) <= 2 && std::abs(B - 132) <= 2,
          "background colour * strength reaches HDR and the final display transform");
}

TEST_GPU(guard_harness_validity_orientation_and_determinism) {
    // The audit is only evidence if reading pixels back is sound.
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir)}; s.floor = false;
    const Img a = shoot(*r, quiet(ShadingMode::Solid), s);
    int cx, cy;
    const bool ok = project(*r, Vec3{0, 1.25f, 0}, cx, cy);   // cube TOP: lit by kDir
    const float lc = ok ? a.lum(cx, cy) : 0, lb = a.lum(3, 3);
    NOTE("cube top px (%d,%d) lum %.1f vs bg lum %.1f", cx, cy, double(lc), double(lb));
    CLAIM(ok && std::fabs(lc - lb) > 10.0f, "readViewport orientation matches projectScreen (harness valid)");
    // CONTROL: the same scene rendered twice is pixel-identical, so any non-zero diff in the
    // FINDING tests is a real difference and not noise.
    const Img b = shoot(*r, quiet(ShadingMode::Solid), s);
    CLAIM(diffPixels(a, b, 0) == 0, "the same scene rendered twice is pixel-identical (control for every diff-based finding)");
    // CONTROL: the predicate used by FINDING_1/2/3 (>100 differing px) does fire on a real change.
    Spec moved = s; moved.cubeColor = {0.1f, 0.1f, 0.9f, 1.0f};
    CLAIM(diffPixels(a, shoot(*r, quiet(ShadingMode::Solid), moved)) > 100, "control: a genuinely different material IS detected by diffPixels");
}

TEST_GPU(guard_finding_5_predicate_can_fail) {
    // CONTROL: FINDING_5 passes when the star is drawn AND fits. With the gizmo toggle OFF no star is
    // drawn at all, so the same measurement must NOT satisfy the predicate (no vacuous pass).
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    const Vec3 at{2.0f, 2.0f, 0.0f};
    Spec none; none.cube = false; none.floor = false;
    const ViewportSettings off = quiet(ShadingMode::Solid);
    shoot(*r, off, none);
    int lx, ly;
    CLAIM(project(*r, at, lx, ly), "cannot project the light position");
    Spec pl; pl.cube = false; pl.floor = false;
    pl.lights = {pointLight(at, 1.0f, 5.0f)};
    pl.lights[0].color = {0, 0, 0, 1};
    const Img pk = shoot(*r, off, pl);          // showGizmo = false: no marker
    int pn = 0;
    for (int y = 0; y < pk.h; ++y)
        for (int x = 0; x < pk.w; ++x)
            if (pk.lum(x, y) > 150.0f) ++pn;
    CLAIM(pn <= 8, "control: with the gizmo off there are no marker pixels, so FINDING_5's predicate (pn > 8) is false");
}

TEST_GPU(guard_empty_light_list_falls_back_to_the_documented_default_sun) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec none; none.floor = false;
    Spec sun = none; sun.lights = {dirLight(Vec3{0, -1, 0})};
    const Img a = shoot(*r, quiet(ShadingMode::Solid), none);
    const Img b = shoot(*r, quiet(ShadingMode::Solid), sun);
    int cx, cy; project(*r, Vec3{0, 1.25f, 0}, cx, cy);
    NOTE("cube-top lum: empty list %.1f, explicit down-sun %.1f, diff pixels %d", double(a.lum(cx, cy)), double(b.lum(cx, cy)), diffPixels(a, b));
    CLAIM(a.lum(cx, cy) > 25.0f, "an empty light list does not render black");
    CLAIM(diffPixels(a, b) == 0, "default sun == white intensity-1 directional straight down (as documented)");
}

TEST_GPU(guard_point_lights_are_evaluated_in_rendered_mode) {
    // Original probe used Solid. Renderer.h:48 (7:40, after the first audit) made Solid scene-light-INDEPENDENT
    // by design (Blender's Solid uses a studio light), so the claim now lives in Rendered. Same thresholds.
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.cube = false;
    s.lights = {pointLight(Vec3{2.5f, 1.5f, 0}, 6.0f, 12.0f)};
    const Img right = shoot(*r, quiet(ShadingMode::Rendered), s);
    s.lights = {pointLight(Vec3{-2.5f, 1.5f, 0}, 6.0f, 12.0f)};
    const Img left = shoot(*r, quiet(ShadingMode::Rendered), s);
    int nx, ny, fx, fy;
    project(*r, Vec3{2.5f, 0, 0}, nx, ny);
    project(*r, Vec3{-2.5f, 0, 0}, fx, fy);
    CLAIM(right.lum(nx, ny) > right.lum(fx, fy) + 8.0f, "point light at +x lights the +x floor more than the -x floor");
    CLAIM(left.lum(fx, fy) > left.lum(nx, ny) + 8.0f, "moving the point light to -x reverses the gradient");
}

TEST_GPU(guard_shading_modes_other_than_the_solid_vs_rendered_claim) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir, 2.0f)}; s.shadowIdx = 0;
    auto shotMode = [&](ShadingMode m, bool shadows) {
        ViewportSettings v = quiet(m); v.shadows.enabled = shadows; return shoot(*r, v, s);
    };
    const Img solid = shotMode(ShadingMode::Solid, false), wire = shotMode(ShadingMode::Wireframe, false);
    const Img mat = shotMode(ShadingMode::Material, false), rend = shotMode(ShadingMode::Rendered, false);
    const Img rendS = shotMode(ShadingMode::Rendered, true), solidS = shotMode(ShadingMode::Solid, true);
    CLAIM(diffPixels(solid, wire) > 500, "Wireframe differs from Solid");
    CLAIM(diffPixels(solid, mat) > 500, "Material differs from Solid");
    CLAIM(diffPixels(mat, rend) > 500, "Material differs from Rendered");
    CLAIM(diffPixels(rend, rendS) > 100, "Rendered + shadows differs from Rendered without");
    CLAIM(diffPixels(solid, solidS) == 0, "Solid ignores the shadow toggle (legacy fast path, as the header says)");
    Spec red = s, blue = s;
    red.lights = {dirLight(kDir, 2.0f, {1, 0, 0, 1})};
    blue.lights = {dirLight(kDir, 2.0f, {0, 0, 1, 1})};
    const Img mr = shoot(*r, quiet(ShadingMode::Material), red), mb = shoot(*r, quiet(ShadingMode::Material), blue);
    const Img sr2 = shoot(*r, quiet(ShadingMode::Solid), red), sb2 = shoot(*r, quiet(ShadingMode::Solid), blue);
    const Img rr2 = shoot(*r, quiet(ShadingMode::Rendered), red), rb2 = shoot(*r, quiet(ShadingMode::Rendered), blue);
    CLAIM(diffPixels(mr, mb) == 0, "Material Preview ignores the authored lights (neutral studio)");
    CLAIM(diffPixels(rr2, rb2) > 200, "Rendered does follow the authored lights (was asserted of Solid before 7:40)");
    CLAIM(diffPixels(sr2, sb2) == 0, "Solid is scene-light-independent: the authored lights do not change it (Renderer.h:48; Blender Solid uses a studio light)");
    Spec amb = s; amb.bg.color = {0.45f, 0.45f, 0.45f, 1}; amb.bg.strength = 1.0f; amb.bg.lighting = true;
    Spec amb2 = amb; amb2.bg.lighting = false;
    const Img rA = shoot(*r, quiet(ShadingMode::Rendered), amb), rB = shoot(*r, quiet(ShadingMode::Rendered), amb2);
    const Img sA = shoot(*r, quiet(ShadingMode::Solid), amb), sB = shoot(*r, quiet(ShadingMode::Solid), amb2);
    const Img mA = shoot(*r, quiet(ShadingMode::Material), amb), mB = shoot(*r, quiet(ShadingMode::Material), amb2);
    CLAIM(diffPixels(rA, rB) > 200, "Rendered adds the world background's ambient when background.lighting");
    CLAIM(diffPixels(sA, sB) == 0 && diffPixels(mA, mB) == 0, "Solid and Material ignore background.lighting");
    int cx, cy; project(*r, Vec3{0, 0.75f, 0}, cx, cy);
    int wr, wg, wb; wire.rgb(cx, cy, wr, wg, wb);
    int br, bgc, bb; wire.rgb(3, 3, br, bgc, bb);
    CLAIM(std::abs(wr - br) <= 4 && std::abs(wg - bgc) <= 4 && std::abs(wb - bb) <= 4, "Wireframe is hollow (GL_LINE: the face interior shows the background)");
}

TEST_GPU(guard_light_cap_of_8_warns_once_per_change_in_rendered_mode) {
    // Solid no longer runs a light loop (Renderer.cpp:1159) so it cannot report ignored lights; Rendered does.
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    mark("cap");
    Spec s; s.floor = false;
    for (int i = 0; i < 10; ++i) s.lights.push_back(dirLight(kDir, 0.2f));
    shoot(*r, quiet(ShadingMode::Rendered), s, 3);
    const int c1 = countLog("ignored 2 light(s)", "cap");
    s.lights.resize(3);
    shoot(*r, quiet(ShadingMode::Rendered), s, 2);
    for (int i = 0; i < 7; ++i) s.lights.push_back(dirLight(kDir, 0.2f));
    shoot(*r, quiet(ShadingMode::Rendered), s, 2);
    const int c2 = countLog("ignored 2 light(s)", "cap");
    NOTE("warnings after 3 frames at 10 lights: %d; after drop to 3 and back to 10: %d", c1, c2);
    CLAIM(c1 == 1, "10 lights over 3 frames warns exactly once");
    CLAIM(c2 == 2, "warns again after the count changes and returns (once per change)");
}

TEST_GPU(guard_viewport_rect_is_viewport_and_scissor) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    r->resize(FBW, FBH);
    r->setViewportSettings(quiet(ShadingMode::Solid));
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, FBW, FBH);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    r->setViewportRect(Rect{100, 50, 400, 300});
    Spec s; s.cube = false; s.floor = false;
    s.bg.color = {1, 0, 0, 1}; s.bg.strength = 1.0f;
    RenderPacket p = makePacket(*r, s, 400, 300);
    r->renderFrame(p);
    std::vector<u8> fb(std::size_t(FBW) * FBH * 4);
    glReadPixels(0, 0, FBW, FBH, GL_RGBA, GL_UNSIGNED_BYTE, fb.data());
    auto at = [&](int x, int y, int& R, int& G, int& B) { const std::size_t o = (std::size_t(y) * FBW + std::size_t(x)) * 4; R = fb[o]; G = fb[o + 1]; B = fb[o + 2]; };
    auto isRed = [&](int x, int y) { int R, G, B; at(x, y, R, G, B); return R > 180 && G < 15; };
    auto isGreen = [&](int x, int y) { int R, G, B; at(x, y, R, G, B); return G > 240 && R < 15; };
    CLAIM(isRed(300, 400) && isRed(100, 250) && isRed(499, 549), "inside the rect (incl. both corners) is the 3D pass's colour");
    CLAIM(isGreen(99, 400) && isGreen(500, 400) && isGreen(300, 249) && isGreen(300, 550), "pixels immediately outside all four edges are untouched (scissor holds)");
    CLAIM(isGreen(5, 5) && isGreen(795, 595), "far corners untouched");
    std::vector<u8> area;
    r->readViewport(0, 0, 400, 300, area);
    int red = 0;
    for (std::size_t i = 0; i + 3 < area.size(); i += 4) if (area[i] > 180 && area[i + 1] < 15) ++red;
    CLAIM(red == 400 * 300, "readViewport is area-local (reads exactly the rect)");
    glDisable(GL_SCISSOR_TEST);
}

TEST_GPU(guard_selection_outline_draws_an_orange_rim_only_when_enabled) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir, 2.0f)}; s.cubeSelected = true;
    ViewportSettings on = quiet(ShadingMode::Solid), off = quiet(ShadingMode::Solid);
    on.showSelectionOutline = true; off.showSelectionOutline = false;
    const Img a = shoot(*r, on, s), b = shoot(*r, off, s);
    auto orange = [](const Img& im) {
        int n = 0;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) { int R, G, B; im.rgb(x, y, R, G, B); if (R > 215 && G > 100 && G < 160 && B < 70) ++n; }
        return n;
    };
    NOTE("orange px: outline on %d, off %d", orange(a), orange(b));
    CLAIM(orange(a) > 40 && orange(b) == 0, "outline draws an orange rim only when showSelectionOutline");
}

TEST_GPU(guard_render_stats_are_honest) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec s; s.lights = {dirLight(kDir)}; s.shadowIdx = 0;
    shoot(*r, quiet(ShadingMode::Solid), s);
    const RenderStats a = r->lastStats();
    NOTE("cube+floor Solid, overlays off: drawCalls %d triangles %d (expect 3 / 14 incl. HDR display resolve)", a.drawCalls, a.triangles);
    CLAIM(a.triangles == 14 && a.drawCalls == 3, "triangles == sum(indices/3); one draw per item plus HDR resolve");
    ViewportSettings v = quiet(ShadingMode::Rendered); v.shadows.enabled = true;
    shoot(*r, v, s);
    const RenderStats b = r->lastStats();
    NOTE("Rendered + shadows: drawCalls %d triangles %d (expect 5 / 14: 2 casters + 2 items + resolve)", b.drawCalls, b.triangles);
    CLAIM(b.drawCalls == 5 && b.triangles == 14, "shadow and final display draws are counted, not triangles");
}

TEST_GPU(guard_other_shadow_warnings_fire_once) {
    CLAIM(ensureContext(), "no GL context");
    ViewportSettings v = quiet(ShadingMode::Rendered); v.shadows.enabled = true;
    {   // CONTROL for FINDING_4: with shadows OFF and no light there is nothing to explain, so NO such warning.
        auto r = freshRenderer();
        mark("shadowctl");
        ViewportSettings offV = quiet(ShadingMode::Rendered);
        Spec s; s.lights = {}; s.shadowIdx = -1;
        shoot(*r, offV, s, 3);
        CLAIM(countLog("no valid shadowLightIndex", "shadowctl") == 0, "control: shadows OFF + no light must not warn about a missing shadow light");
    }
    {   // caster is a POINT light
        auto r = freshRenderer();
        mark("shadowwarn2");
        Spec s; s.lights = {pointLight(Vec3{2, 3, 0}, 5, 10)}; s.shadowIdx = 0;
        shoot(*r, v, s, 3);
        CLAIM(countLog("is not directional", "shadowwarn2") == 1, "non-directional shadow light warns once");
    }
    {   // directional light but no castShadow meshes
        auto r = freshRenderer();
        mark("shadowwarn3");
        Spec s; s.lights = {dirLight(kDir)}; s.shadowIdx = 0; s.castShadow = false; s.floor = false;
        shoot(*r, v, s, 3);
        CLAIM(countLog("has no castShadow meshes", "shadowwarn3") == 1, "no-caster case warns once");
    }
    {   // WARM: a valid pass first, then the light disappears
        auto r = freshRenderer();
        mark("shadowwarn4");
        Spec good; good.lights = {dirLight(kDir)}; good.shadowIdx = 0;
        shoot(*r, v, good, 2);
        Spec bad = good; bad.lights = {}; bad.shadowIdx = -1;
        shoot(*r, v, bad, 3);
        CLAIM(countLog("no valid shadowLightIndex", "shadowwarn4") == 1, "the same condition DOES warn once a valid pass preceded it");
    }
}

TEST_GPU(guard_grid_axes_and_light_markers_draw) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec none; none.cube = false; none.floor = false;
    ViewportSettings off = quiet(ShadingMode::Solid), grid = off, axes = off;
    grid.showGrid = true; axes.showAxes = true;
    const Img base = shoot(*r, off, none), g = shoot(*r, grid, none), a = shoot(*r, axes, none);
    CLAIM(diffPixels(base, g) > 300, "showGrid draws a grid");
    auto count = [&](const Img& im, int rl, int gl, int bl, bool rHigh, bool gHigh, bool bHigh) {
        int n = 0;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                int R, G, B; im.rgb(x, y, R, G, B);
                if ((rHigh ? R > rl : R < rl) && (gHigh ? G > gl : G < gl) && (bHigh ? B > bl : B < bl)) ++n;
            }
        return n;
    };
    const int xr = count(a, 150, 90, 90, true, false, false);
    const int yg = count(a, 90, 150, 90, false, true, false);
    const int zb = count(a, 130, 160, 180, false, false, true);
    NOTE("axes pixels: red(X) %d  green(Y) %d  blue(Z) %d", xr, yg, zb);
    CLAIM(xr > 15 && yg > 15 && zb > 15, "world axes draw as red / green / blue lines");
    // Sun marker (a BLACK light, so any bright pixel is the marker).
    Spec lit; lit.cube = false; lit.floor = false;
    lit.lights = {dirLight(kDir, 1.0f, {0, 0, 0, 1})};
    lit.lights[0].position = Vec3{2.0f, 2.0f, 0.0f};
    ViewportSettings mOn = off; mOn.showGizmo = true;
    const Img mk = shoot(*r, mOn, lit), nomk = shoot(*r, off, lit);
    int lx, ly; project(*r, Vec3{2.0f, 2.0f, 0.0f}, lx, ly);
    int bright = 0; bool sector[16] = {};
    for (int y = 0; y < mk.h; ++y)
        for (int x = 0; x < mk.w; ++x)
            if (mk.lum(x, y) > 150.0f) {
                ++bright;
                const float d = std::hypot(float(x - lx), float(y - ly));
                if (d >= 11.0f && d <= 17.0f) {
                    const float ang = std::atan2(float(y - ly), float(x - lx)) + 3.14159265f;
                    sector[int(ang / (2 * 3.14159265f) * 16) % 16] = true;
                }
            }
    int ring = 0; for (bool b : sector) ring += b ? 1 : 0;
    CLAIM(diffPixels(mk, nomk) > 30 && bright > 10, "a light draws a visible marker (a black light falls back to white)");
    CLAIM(ring >= 12, "the sun disc is a ring at ~lightMarkerRadiusPixels() (drawn size == hit-test size)");
}

TEST_GPU(guard_grid_clamp_origins_and_cast_shadow_flag) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec none; none.cube = false; none.floor = false;
    ViewportSettings g1 = quiet(ShadingMode::Solid); g1.showGrid = true; g1.gridSubdivisions = 1;
    ViewportSettings g0 = g1, gBig = g1, gHuge = g1;
    g0.gridSubdivisions = 0; gBig.gridSubdivisions = 100; gHuge.gridSubdivisions = 100000;
    const Img i1 = shoot(*r, g1, none), i0 = shoot(*r, g0, none, 3);
    const Img iBig = shoot(*r, gBig, none), iHuge = shoot(*r, gHuge, none, 3);
    auto r0 = freshRenderer();
    mark("clamp0");
    shoot(*r0, g0, none);
    const int w0 = countLog("outside [1,100]", "clamp0");
    auto r100 = freshRenderer();
    mark("clamp100");
    shoot(*r100, gHuge, none);
    const int w100 = countLog("outside [1,100]", "clamp100");
    CLAIM(diffPixels(i0, i1) == 0 && diffPixels(iHuge, iBig) == 0, "out-of-range gridSubdivisions clamps to [1,100] (no divide-by-zero, same picture)");
    CLAIM(w0 == 1 && w100 == 1, "each out-of-range initial value warns once per clamped setting");
    Spec floorOnly; floorOnly.cube = false; floorOnly.floor = true; floorOnly.lights = {dirLight(kDir)};
    ViewportSettings oOn = quiet(ShadingMode::Solid); oOn.showOrigins = true;
    CLAIM(diffPixels(shoot(*r, oOn, floorOnly), shoot(*r, quiet(ShadingMode::Solid), floorOnly)) > 0, "showOrigins draws object-origin crosses");
    ViewportSettings sOn = quiet(ShadingMode::Rendered); sOn.shadows.enabled = true;
    ViewportSettings sOff = quiet(ShadingMode::Rendered);
    Spec sh; sh.lights = {dirLight(kDir, 2.0f)}; sh.shadowIdx = 0;
    const int full = diffPixels(shoot(*r, sOn, sh), shoot(*r, sOff, sh));
    Spec nc = sh; nc.castShadow = false;
    const int noCast = diffPixels(shoot(*r, sOn, nc), shoot(*r, sOff, nc));
    NOTE("shadow footprint: full %d px | cube castShadow=false %d px", full, noCast);
    CLAIM(full > 500 && noCast < full / 5, "castShadow=false removes the cube's shadow");
}

TEST_GPU(guard_gizmo_constant_pixel_length_hover_gating_and_orientation) {
    CLAIM(ensureContext(), "no GL context");
    auto r = freshRenderer();
    Spec none; none.cube = false; none.floor = false;
    ViewportSettings on = quiet(ShadingMode::Solid), off = quiet(ShadingMode::Solid);
    on.showGizmo = true;
    GizmoState g; g.handleLengthPixels = 100.0f;
    auto shootG = [&](const ViewportSettings& st, const GizmoState& gs) {
        Img img; img.w = W; img.h = H;
        r->setViewportSettings(st); r->setViewportRect(Rect{}); r->resize(W, H);
        RenderPacket p = makePacket(*r, none);
        r->renderFrame(p);
        r->drawGizmo(gs, p.view, p.projection);
        r->readViewport(0, 0, W, H, img.px);
        return img;
    };
    struct Red { int n = 0; float maxd = 0; float cx = 0, cy = 0; float meanR = 0; };
    auto redOf = [&](const Img& im, int ox, int oy) {
        Red q; double sx = 0, sy = 0, sr = 0;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                int R, G, B; im.rgb(x, y, R, G, B);
                if (R > 140 && G < 100 && B < 100) { ++q.n; sx += x; sy += y; sr += R; q.maxd = std::fmax(q.maxd, std::hypot(float(x - ox), float(y - oy))); }
            }
        if (q.n) { q.cx = float(sx / q.n); q.cy = float(sy / q.n); q.meanR = float(sr / q.n); }
        return q;
    };
    r->camera().frameSphere(Vec3{0, 0, 0}, 3.0f);
    const float dNear = r->camera().distance();
    int ox, oy; project(*r, Vec3{0, 0, 0}, ox, oy);
    const Img near1 = shootG(on, g);
    const Red qNear = redOf(near1, ox, oy);
    r->camera().frameSphere(Vec3{0, 0, 0}, 12.0f);
    const float dFar = r->camera().distance();
    project(*r, Vec3{0, 0, 0}, ox, oy);
    const Img far1 = shootG(on, g);
    const Red qFar = redOf(far1, ox, oy);
    CLAIM(dFar > dNear * 1.5f && qNear.n > 20 && qFar.n > 20 && std::fabs(qNear.maxd - qFar.maxd) <= 0.12f * qNear.maxd,
          "handle length is constant in PIXELS across a >1.5x camera distance change");
    CLAIM(qNear.maxd > 60.0f && qNear.maxd <= 104.0f, "the X handle reaches ~handleLengthPixels on screen");
    const Img gone = shootG(off, g), none2 = shoot(*r, off, none);
    CLAIM(diffPixels(gone, none2) == 0, "showGizmo=false removes drawGizmo's output entirely");
    GizmoState hov = g; hov.hoveredAxis = 0;
    const Img hv = shootG(on, hov);
    const Red qHov = redOf(hv, ox, oy);
    CLAIM(qHov.meanR > qFar.meanR + 5.0f || diffPixels(hv, far1) > 20, "hoveredAxis visibly changes the handle");
    GizmoState rot = g; rot.orientation = rotationEuler(Vec3{0, 0, 1.5707963f});
    const Img rt = shootG(on, rot);
    const Red qRot = redOf(rt, ox, oy);
    CLAIM(qRot.n > 20 && qRot.cy < qFar.cy - 15.0f, "orientation matrix rotates the drawn handles (local-space gizmo)");
}

// =================================================================================
// ITEM 176: ONLY THE VIEWPORT AREA MAY SHOW THE 3D RENDER
// =================================================================================
//
// The viewport area's body is transparent ON PURPOSE (the renderer draws underneath it), so any gap
// between that area and a neighbour shows LIVE 3D RENDER instead of a panel - the sidebar looked as if
// it intersected the Outliner, and it flickered as the render rect rounded differently by a pixel.
// This pins the whole CLASS, on the COMPOSITED frame (the real Editor::draw over the real Renderer, in
// the order App::run does them: viewportRect -> setViewportRect -> renderFrame -> editor.draw).
//
// METHOD (differential, so no colour has to be 'recognised as render'): composite the frame twice with
// DIFFERENT 3D content (opposite background colour, orbited camera, the same panels) and compare every
// pixel OUTSIDE the viewport area's window rectangle. A pixel there that differs depends on the render;
// it is render content showing where a panel should be. The allowed rectangle is ImGui's own 'Area D'
// window (the independent truth), NOT Editor::viewportRect() - the latter is the very rect the renderer
// is handed, so testing against it would be circular. Each leaking pixel is attributed to the panel
// neighbourhood it sits in (Outliner / Properties / Timeline / topbar / status bar / between areas).
//
// CONTROLS: (a) the same scene with the render rect deliberately grown must be caught (a 2px gap is
// what the bug looked like); (b) INSIDE the viewport the two frames must differ (the method can see
// render content at all).
namespace {

struct Px { int w = 0, h = 0; std::vector<u8> rgba; };   // top-left origin

Px readBackBuffer(int w, int h) {
    Px p; p.w = w; p.h = h;
    std::vector<u8> gl(std::size_t(w) * std::size_t(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl.data());
    p.rgba.resize(gl.size());
    for (int y = 0; y < h; ++y)
        std::memcpy(&p.rgba[std::size_t(y) * std::size_t(w) * 4], &gl[std::size_t(h - 1 - y) * std::size_t(w) * 4], std::size_t(w) * 4);
    return p;
}

struct IRect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool has(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; } };

struct ShowRig {
    platform::Window window;
    render::Renderer renderer;
    editor::Editor editor;
    scene::World world;
    editor::UndoStack undo;
    std::string why;
    bool ok = false;
    int W = 0, H = 0;
    int growRender = 0;          // CONTROL: pixels by which the render rect is wrongly enlarged on every side

    ShowRig(int w, int h) : W(w), H(h) {
        platform::WindowConfig wc;
        wc.width = w; wc.height = h; wc.visible = false; wc.title = "viewport-only-render";
        if (!window.create(wc)) { why = "no GL window"; return; }
        if (!renderer.init()) { why = "renderer init failed"; return; }
        renderer.resize(w, h);
        if (!editor.init(window.nativeHandle())) { why = "editor init failed"; return; }
        world.createPrimitive(scene::Primitive::Cube, "cube");
        for (int i = 0; i < 6; ++i) frame(Color{0.5f, 0.5f, 0.5f, 1}, 0.0f);
        const ImGuiIO& io = ImGui::GetIO();
        if (std::abs(io.DisplaySize.x * io.DisplayFramebufferScale.x - float(w)) > 1.0f) {
            why = "the window came up at a different size than requested (display limit?)"; return;
        }
        ok = editor.viewportRect().valid();
        if (!ok) why = "the editor never produced a viewport rect";
    }
    ~ShowRig() { editor.shutdown(); renderer.shutdown(); }
    ShowRig(const ShowRig&) = delete;
    ShowRig& operator=(const ShowRig&) = delete;

    // One App::run iteration. `clear` is what the back buffer holds where nothing paints (App never clears it).
    void frame(const Color& bg, float clearGrey) {
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, W, H);
        glClearColor(clearGrey, clearGrey, clearGrey, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        Rect area{0, 0, W, H};
        const Rect edited = editor.viewportRect();
        if (edited.valid()) area = edited;
        if (growRender > 0) { area.x -= growRender; area.y -= growRender; area.w += 2 * growRender; area.h += 2 * growRender; }
        renderer.setViewportRect(area, editor.viewportAreaRect());
        RenderPacket packet = world.buildPacket(area.w, area.h);
        packet.width = area.w; packet.height = area.h;
        packet.view = renderer.camera().view();
        packet.projection = renderer.camera().projection(float(area.w) / float(area.h));
        packet.cameraPosition = renderer.camera().position();
        packet.background.color = bg;
        packet.background.strength = 1.0f;
        renderer.renderFrame(packet);
        editor.draw(world, renderer, renderer.lastStats(), undo);
    }

    Px shoot(const Color& bg, float clearGrey, int frames = 6) {
        for (int i = 0; i < frames; ++i) frame(bg, clearGrey);
        return readBackBuffer(W, H);
    }

    IRect windowRect(const char* name) const {
        IRect r;
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        if (!w) return r;
        const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
        r.x0 = int(std::floor(w->Pos.x * s.x)); r.y0 = int(std::floor(w->Pos.y * s.y));
        r.x1 = int(std::ceil((w->Pos.x + w->Size.x) * s.x)); r.y1 = int(std::ceil((w->Pos.y + w->Size.y) * s.y));
        return r;
    }
};

struct Leak {
    long long outside = 0, inside = 0;
    struct Where { const char* name; long long n = 0; int maxDelta = 0; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };
    std::vector<Where> by;
};

Leak compareFrames(const ShowRig& rig, const Px& a, const Px& b, bool allowRenderRectOnly = false, int minDelta = 0) {
    IRect viewportArea = rig.windowRect("Area D");
    if (allowRenderRectOnly) {   // stricter: only the rect handed to the renderer is exempt
        const Rect r = rig.editor.viewportRect();
        viewportArea = IRect{r.x, r.y, r.x + r.w, r.y + r.h};
    }
    struct Named { const char* name; IRect r; };
    const Named hood[] = {{"Outliner/Area A", rig.windowRect("Area A")}, {"Properties/Area B", rig.windowRect("Area B")},
                          {"Timeline/Area C", rig.windowRect("Area C")}, {"topbar", rig.windowRect("##MainMenuBar")},
                          {"status bar", rig.windowRect("##status")}, {"viewport area (outside the render rect)", rig.windowRect("Area D")}};
    Leak L;
    for (const Named& n : hood) L.by.push_back({n.name});
    L.by.push_back({"between areas (splitter / gap, no window)"});
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            const std::size_t i = (std::size_t(y) * std::size_t(a.w) + std::size_t(x)) * 4;
            const int d = std::max({std::abs(int(a.rgba[i]) - int(b.rgba[i])), std::abs(int(a.rgba[i + 1]) - int(b.rgba[i + 1])), std::abs(int(a.rgba[i + 2]) - int(b.rgba[i + 2]))});
            const bool differs = d > minDelta;
            if (!differs) continue;
            if (viewportArea.has(x, y)) { ++L.inside; continue; }
            ++L.outside;
            std::size_t k = L.by.size() - 1;
            for (std::size_t j = 0; j < std::size(hood); ++j) if (hood[j].r.has(x, y)) { k = j; break; }
            Leak::Where& w = L.by[k];
            w.maxDelta = std::max({w.maxDelta, std::abs(int(a.rgba[i]) - int(b.rgba[i])), std::abs(int(a.rgba[i + 1]) - int(b.rgba[i + 1])), std::abs(int(a.rgba[i + 2]) - int(b.rgba[i + 2]))});
            ++w.n; w.x0 = std::min(w.x0, x); w.y0 = std::min(w.y0, y); w.x1 = std::max(w.x1, x); w.y1 = std::max(w.y1, y);
        }
    return L;
}

constexpr Color kBgA{0.90f, 0.10f, 0.10f, 1.0f};
constexpr Color kBgB{0.10f, 0.20f, 0.90f, 1.0f};

// Composite two frames with different 3D content; return how the pixels differ.
Leak differential(ShowRig& rig, const char* tag, bool print) {
    const Px a = rig.shoot(kBgA, 0.20f);
    rig.renderer.camera().orbit(140.0f, 60.0f);          // different geometry on screen, panels unchanged
    const Px b = rig.shoot(kBgB, 0.20f);
    const Leak L = compareFrames(rig, a, b);
    const IRect va = rig.windowRect("Area D");
    if (print) {
        NOTE("[%s] %dx%d  viewport area window (%d,%d)-(%d,%d); render rect %d,%d %dx%d", tag, rig.W, rig.H, va.x0, va.y0, va.x1, va.y1,
             rig.editor.viewportRect().x, rig.editor.viewportRect().y, rig.editor.viewportRect().w, rig.editor.viewportRect().h);
        NOTE("[%s] pixels that depend on the 3D render: %lld INSIDE the viewport area, %lld OUTSIDE it", tag, L.inside, L.outside);
        for (const auto& w : L.by)
            if (w.n > 0) NOTE("[%s]    leaking into %-42s %6lld px, bbox (%d,%d)-(%d,%d)", tag, w.name, w.n, w.x0, w.y0, w.x1, w.y1);
    }
    return L;
}

} // namespace

TEST_GPU(only_the_viewport_area_shows_the_render) {
    // 1283x719 is deliberately odd: it is where a rounded render rect and ImGui's fractional layout disagree.
    const int sizes[3][2] = {{1280, 720}, {1283, 719}, {1920, 1080}};
    for (const auto& sz : sizes) {
        ShowRig rig(sz[0], sz[1]);
        CLAIM(rig.ok, std::string("rig: ") + rig.why);
        if (!rig.ok) continue;
        const Leak L = differential(rig, "composited", true);
        CLAIM(L.inside > 1000, "the method is blind: the two frames do not even differ inside the viewport area");
        CLAIM(L.outside == 0, "3D render content is visible OUTSIDE the viewport area (see the neighbourhood list above): a gap between the "
                              "viewport area and its neighbours shows live render instead of a panel");
        const IRect va = rig.windowRect("Area D");
        const Rect r = rig.editor.viewportRect();
        CLAIM(r.x >= va.x0 && r.y >= va.y0 && r.x + r.w <= va.x1 && r.y + r.h <= va.y1,
              "the renderer's viewport rect extends beyond the viewport area window (so it can paint under a neighbour)");
    }
}

TEST_GPU(guard_viewport_only_render_control_a_render_rect_grown_by_14px_is_caught) {
    ShowRig rig(1280, 720);
    CLAIM(rig.ok, std::string("rig: ") + rig.why);
    if (!rig.ok) return;
    rig.growRender = 14;   // > the 8px window padding between the area edge and the render rect, so it reaches the 2px splitters
    const Leak L = differential(rig, "CONTROL render rect +14px", true);
    CLAIM(L.outside > 0, "control failed: a render rect grown 14px into the neighbours went UNDETECTED, so the real test could not catch a gap");
}

TEST_GPU(no_pixel_outside_the_render_rect_is_left_unpainted) {
    // The sibling of the rule above. The viewport body is transparent and nothing in App clears the back buffer, so a pixel that
    // neither a panel nor the render covers shows STALE buffer content (previous frames' render, in the real app). Detect it by
    // changing only what the buffer was cleared to: any pixel OUTSIDE the render rect that follows the clear colour is unpainted.
    // Differences of <= 16/255 are ignored: the panels are ~96% opaque (measured: max delta 6 of 153), a faint ghost, not a gap.
    ShowRig rig(1280, 720);
    CLAIM(rig.ok, std::string("rig: ") + rig.why);
    if (!rig.ok) return;
    const Px a = rig.shoot(kBgA, 0.20f);
    const Px b = rig.shoot(kBgA, 0.80f);
    const Leak L = compareFrames(rig, a, b, true, 16);
    const Rect r = rig.editor.viewportRect();
    NOTE("[unpainted] render rect %d,%d %dx%d; pixels outside it that follow the clear colour: %lld", r.x, r.y, r.w, r.h, L.inside + L.outside);
    // (inside here counts pixels in the render rect, which are all repainted by the render; outside is the claim)
    for (const auto& w : L.by)
        if (w.n > 0) NOTE("[unpainted]    %-42s %6lld px, max channel delta %3d (clear 0.2 vs 0.8 = 153), bbox (%d,%d)-(%d,%d)", w.name, w.n, w.maxDelta, w.x0, w.y0, w.x1, w.y1);
    CLAIM(L.outside == 0, "pixels outside the render rect are painted by nothing: they show whatever the back buffer held (stale 3D render in the real app)");
}
TEST_GPU(guard_viewport_only_render_control_an_unrendered_frame_difference_is_zero) {
    // Same 3D content twice: nothing may differ anywhere. Proves the comparison itself has no noise (e.g. a blinking caret).
    ShowRig rig(1280, 720);
    CLAIM(rig.ok, std::string("rig: ") + rig.why);
    if (!rig.ok) return;
    const Px a = rig.shoot(kBgA, 0.20f);
    const Px b = rig.shoot(kBgA, 0.20f);
    const Leak L = compareFrames(rig, a, b);
    NOTE("identical scene twice: %lld px differ inside, %lld outside", L.inside, L.outside);
    CLAIM(L.inside + L.outside == 0, "two composites of the SAME scene differ: the differential has noise and cannot be trusted");
}
// ==== ITEM 205: only a SELECTED item may be drawn in the selection colour ==========================
//
// Blender's rule (WHAT-REMAINS §4) and D42's own comment on the token: orange is the SELECTION cue -
// "selection color is reserved for selection, not ordinary controls". Anything else painted in it makes the
// cue meaningless: a user can no longer tell "this row is selected" from "this tab is active". The defect
// this pins survived every suite because nothing measured colour SEMANTICS.
//
// METHOD: the COMPOSITED frame (the same back buffer EMBER_EDITOR_FRAME_DUMP reads: the renderer first, then
// ImGui's RenderDrawData) in a hidden window, captured TWICE with IDENTICAL 3D content - once with nothing
// selected, once with one object selected - and selection-orange pixels counted PER REGION over the chrome
// rectangles ImGui itself reports. Growth is allowed in exactly ONE place, the Outliner's own content,
// bounded by a single row; growth in a header strip, the Properties rail, the Properties content, the
// Timeline band, the topbar or the status bar IS the defect. The viewport area is deliberately excluded:
// the 3D selection rim lives there and is a legitimate cue.
//
// The match is the selection FAMILY, not one triple: the cue is ThemeTokens::selection {237,115,26,255} and
// selected FILLS are that colour at selectionFillAlpha (0.45), i.e. a blend over the panel colour
// (~121,66,16 over a dark panel). Both must count, and nothing blue/white may.
namespace {

bool isSelectionOrange(int r, int g, int b) {
    return r >= 90 && (r - g) >= 45 && g >= 35 && (g - b) >= 15 && b <= 90;
}

IRect rectOf(const char* nameFragment) {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
        if (std::string(w->Name).find(nameFragment) == std::string::npos) continue;
        const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
        IRect r;
        r.x0 = int(std::floor(w->Pos.x * s.x)); r.y0 = int(std::floor(w->Pos.y * s.y));
        r.x1 = int(std::ceil((w->Pos.x + w->Size.x) * s.x)); r.y1 = int(std::ceil((w->Pos.y + w->Size.y) * s.y));
        return r;
    }
    return IRect{};
}

} // namespace

TEST_GPU(only_a_selected_item_is_selection_coloured) {
    ShowRig rig(1280, 720);
    CLAIM(rig.ok, std::string("rig: ") + rig.why);
    if (!rig.ok) return;

    const Color bg{0.5f, 0.5f, 0.5f, 1.0f};
    rig.world.clearSelection();
    const Px none = rig.shoot(bg, 0.0f, 8);

    ObjectId target{};
    for (const scene::Object& o : rig.world.objects())
        if (!o.isLight) { target = o.id; break; }
    CLAIM(target.value != 0, "setup: the rig's world has no meshed object to select");
    if (target.value == 0) return;
    rig.world.select(target, false);
    const Px one = rig.shoot(bg, 0.0f, 8);

    auto countIn = [](const Px& p, const IRect& r) {
        long long n = 0;
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                if (x < 0 || y < 0 || x >= p.w || y >= p.h) continue;
                const std::size_t i = (std::size_t(y) * std::size_t(p.w) + std::size_t(x)) * 4u;
                if (isSelectionOrange(p.rgba[i], p.rgba[i + 1], p.rgba[i + 2])) ++n;
            }
        return n;
    };
    auto below = [](IRect r, int strip) { r.y0 += strip; return r; };

    const int headerH = int(std::lround(ember::kDefaultTheme.headerHeight));   // D42: 27 logical px
    const IRect areaA = rig.windowRect("Area A");        // Outliner  (right, top)
    const IRect areaB = rig.windowRect("Area B");        // Properties (right, bottom)
    const IRect areaC = rig.windowRect("Area C");        // Timeline  (bottom, full width)
    const IRect rail = rectOf("##properties-icons");
    const IRect toolbar = rectOf("##toolbar");           // the viewport's own left Region (item 181)
    const IRect sidebar = rectOf("##sidebarcontent");    // the viewport's own right Region
    const IRect topbar = rig.windowRect("##MainMenuBar");
    const IRect status = rig.windowRect("##status");

    struct Region { const char* name; IRect r; bool mayGrow; };
    const Region regions[] = {
        {"topbar", topbar, false},
        {"status bar", status, false},
        {"Outliner header strip", areaA, false},                 // header only (y1 trimmed below)
        {"Properties header strip", areaB, false},
        {"Properties rail", rail, false},
        {"Toolbar (viewport Region)", toolbar, false},
        {"Sidebar (viewport Region)", sidebar, false},
        {"Properties content", below(areaB, headerH), false},
        {"Timeline area", areaC, false},
        {"Outliner content (the selected row)", below(areaA, headerH), true},
    };

    long long growthElsewhere = 0, colouredElsewhere = 0;
    const long long rowBudget = (long long)headerH * (areaA.x1 - areaA.x0);   // one row's worth of pixels
    for (const Region& rg : regions) {
        IRect r = rg.r;
        if (rg.name == std::string("Outliner header strip") || rg.name == std::string("Properties header strip"))
            r.y1 = std::min(r.y1, r.y0 + headerH);
        const long long n0 = countIn(none, r), n1 = countIn(one, r);
        NOTE("  %-38s rect (%4d,%4d)-(%4d,%4d) | nothing %6lld px | one selected %6lld px | growth %+6lld",
             rg.name, r.x0, r.y0, r.x1, r.y1, n0, n1, n1 - n0);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) {
            CLAIM(false, std::string("region '") + rg.name + "' resolved to an EMPTY rect, so its count of 0 "
                  "proves nothing - the region lookup is broken");
            continue;
        }
        if (rg.mayGrow) {
            CLAIM(n1 - n0 <= rowBudget,
                  "the Outliner's own growth is bigger than ONE selected row: a row is ~" + std::to_string(rowBudget) +
                  " px, growth was " + std::to_string(n1 - n0));
        } else {
            // The rule is absolute, not merely differential: chrome may never carry the selection colour -
            // counting only the GROWTH would let a permanently orange region pass.
            colouredElsewhere += n0 + n1;
            growthElsewhere += n1 - n0;
        }
    }

    // CONTROL: the measurement must be able to SEE the cue, or "no growth anywhere" would be vacuous.
    const long long rowNone = countIn(none, below(areaA, headerH));
    const long long rowOne = countIn(one, below(areaA, headerH));
    CLAIM(rowOne > 100, "the method is blind: selecting an object produced " + std::to_string(rowOne) +
                        " selection-orange pixels in the Outliner, so the counts above prove nothing");
    CLAIM(rowNone == 0, "nothing is selected, yet the Outliner already shows " + std::to_string(rowNone) +
                        " selection-orange pixels");

    CLAIM(growthElsewhere == 0,
          "only a SELECTED item may be drawn in the selection colour, but selecting one object added " +
          std::to_string(growthElsewhere) + " selection-orange pixel(s) to UI chrome (see the per-region "
          "table above): orange is the selection cue, not ordinary chrome");
    CLAIM(colouredElsewhere == 0,
          "UI chrome is painted in the selection colour: " + std::to_string(colouredElsewhere) + " "
          "selection-orange pixel(s) across the chrome regions (see the per-region table above), with nothing "
          "selected as well as with one object selected. In Blender only a selected item wears the selection "
          "colour; a permanently orange region (an accent, a header, a rail row) destroys the cue");
}

HARNESS_MAIN()
