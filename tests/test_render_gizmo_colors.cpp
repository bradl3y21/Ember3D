// tests/test_render_gizmo_colors.cpp - owned by Verity / QA. Item 159 (and 148).
//
// Contract under test (WHAT-REMAINS.md section 2, Gizmos): "Axis handles coloured
// Blender's way (X red, Y green, Z blue), plus plane quads; hover highlights, active
// is brighter." Nothing checked the colours that actually reach the screen.
//
// METHOD - real pixels, no trusted coordinates: render an empty scene, call
// Renderer::drawGizmo for a known GizmoState, read the frame back, and find each
// handle's line by walking the SEGMENT the camera says it occupies (pivot -> drawn tip,
// from camera.projectScreen), taking the pixel that differs most from the background.
// X and Y are measured in the Front view (both lie in the screen plane), Z in the Right
// view. Assertions are about what a user perceives - hue dominance and brightness order -
// not about the shader's numeric constants.
//
// SCOPE NOTE: at the time of writing nothing in src/ CALLS Renderer::drawGizmo (grep: only
// the declaration and the definition), so this proves what the function draws, not that
// the app shows it. That reachability gap is reported separately in the finding.
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
#include <string>
#include <vector>

using namespace ember;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kW = 640;
constexpr int kH = 360;

bool ensureContext() {
    static ember::platform::Window* win = [] {
        auto* w = new ember::platform::Window();
        ember::platform::WindowConfig wc;
        wc.width = kW;
        wc.height = kH;
        wc.visible = false;
        wc.title = "ember-gizmo-colors";
        if (!w->create(wc)) { delete w; return static_cast<ember::platform::Window*>(nullptr); }
        return w;
    }();
    return win != nullptr;
}

struct Rgb { int r = 0, g = 0, b = 0; };

struct Frame {
    std::vector<u8> rgba;
    Rgb at(int x, int y) const {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return {};
        const std::size_t s = (std::size_t(y) * kW + std::size_t(x)) * 4;
        return {rgba[s], rgba[s + 1], rgba[s + 2]};
    }
};

const Rgb kBg{52, 55, 62};  // Reinhard+sRGB display of the 0.035/0.04/0.05 scene background

int dist(const Rgb& a, const Rgb& b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }
float luma(const Rgb& c) { return 0.2126f * float(c.r) + 0.7152f * float(c.g) + 0.0722f * float(c.b); }
std::string str(const Rgb& c) {
    char b[40]; std::snprintf(b, sizeof b, "(%d,%d,%d)", c.r, c.g, c.b); return b;
}

// Optional frame dump for diagnosis: EMBER_GIZMO_DUMP=<path.ppm> writes the last drawn frame.
void maybeDump(const Frame& f, const char* tag) {
    const char* base = std::getenv("EMBER_GIZMO_DUMP");
    if (!base || !*base) return;
    const std::string path = std::string(base) + "_" + tag + ".ppm";
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    std::fprintf(fp, "P6\n%d %d\n255\n", kW, kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const Rgb c = f.at(x, y);
            const unsigned char px[3] = {(unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b};
            std::fwrite(px, 1, 3, fp);
        }
    std::fclose(fp);
}
// Render an empty scene, then the gizmo. `view` selects which standard view the camera uses.
bool drawScene(GizmoState gizmo, render::StandardView view, bool showGizmo, Frame& out, render::Renderer& r) {
    if (!ensureContext()) return false;
    if (!r.init()) return false;
    r.resize(kW, kH);
    r.camera().setView(view);
    scene::World world;
    RenderPacket p = world.buildPacket(kW, kH);
    p.width = kW; p.height = kH;
    p.view = r.camera().view();
    p.projection = r.camera().projection(static_cast<float>(kW) / kH);
    p.cameraPosition = r.camera().position();
    p.background.color = {0.035f, 0.04f, 0.05f, 1.0f};
    p.background.strength = 1.0f;
    p.background.lighting = false;
    ViewportSettings s;
    s.showGrid = false; s.showAxes = false; s.showOrigins = false; s.showSelectionOutline = false;
    s.showGizmo = showGizmo;
    r.setViewportSettings(s);
    for (int i = 0; i < 3; ++i) r.renderFrame(p);
    r.drawGizmo(gizmo, p.view, p.projection);
    r.readViewport(0, 0, kW, kH, out.rgba);
    maybeDump(out, view == render::StandardView::Right ? "right" : "front");
    return out.rgba.size() == std::size_t(kW) * kH * 4;
}

// The colour a handle's line shows: walk 70%..86% of the pivot->tip screen segment. That stretch is clear
// of the pivot pile-up, of the plane quads (which sit at 32..58 px and, seen edge-on in the Front/Right
// views, collapse into a differently-coloured line ON the handle) and of the tip barbs (from ~88%) and take the pixel farthest from the background in each 3x3.
bool handleColour(const render::OrbitCamera& cam, const Frame& f, Vec3 origin, Vec3 axis, float wpp,
                  float lengthPx, Rgb& out) {
    float ox, oy, tx, ty;
    if (!cam.projectScreen(origin, kW, kH, ox, oy) ||
        !cam.projectScreen(origin + axis * (lengthPx * wpp), kW, kH, tx, ty)) return false;
    std::vector<Rgb> hits;
    for (int s = 0; s <= 10; ++s) {
        const float t = 0.70f + 0.16f * float(s) / 10.0f;
        const int cx = int(std::lround(ox + (tx - ox) * t)), cy = int(std::lround(oy + (ty - oy) * t));
        Rgb best{}; int bestD = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const Rgb c = f.at(cx + dx, cy + dy);
                const int d = dist(c, kBg);
                if (d > bestD) { bestD = d; best = c; }
            }
        if (bestD > 60) hits.push_back(best);
    }
    if (hits.size() < 5) return false;
    std::sort(hits.begin(), hits.end(), [](const Rgb& a, const Rgb& b) { return luma(a) < luma(b); });
    out = hits[hits.size() / 2];
    return true;
}

float worldPerPixel(const render::OrbitCamera& cam) {
    return (2.0f * cam.distance() * std::tan(cam.fovY() * 0.5f)) / static_cast<float>(kH);
}

struct Sample { bool ok = false; Rgb base, hover, active; };

// Measure axis `axis` at the three highlight tiers.
Sample measure(int axis) {
    Sample s;
    const render::StandardView view = axis == 2 ? render::StandardView::Right : render::StandardView::Front;
    const Vec3 dir{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
    for (int tier = 0; tier < 3; ++tier) {
        GizmoState g;
        g.mode = GizmoState::Mode::Translate;
        g.origin = {0, 0, 0};
        g.handleLengthPixels = 90.0f;
        g.hoveredAxis = tier == 1 ? axis : -1;
        g.activeAxis = tier == 2 ? axis : -1;
        render::Renderer r;
        Frame f;
        if (!drawScene(g, view, true, f, r)) return s;
        Rgb c;
        if (!handleColour(r.camera(), f, g.origin, dir, worldPerPixel(r.camera()), 90.0f, c)) { r.shutdown(); return s; }
        (tier == 0 ? s.base : tier == 1 ? s.hover : s.active) = c;
        r.shutdown();
    }
    s.ok = true;
    return s;
}

bool dominant(const Rgb& c, int axis, int margin) {
    const int v[3] = {c.r, c.g, c.b};
    for (int j = 0; j < 3; ++j)
        if (j != axis && v[axis] < v[j] + margin) return false;
    return true;
}

} // namespace

TEST_GPU(gizmo_axis_handles_are_red_green_blue_at_rest) {
    const char* names[3] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; ++axis) {
        const Sample s = measure(axis);
        CHECK_MSG(s.ok, "axis %s: no handle pixels found on its drawn segment (gizmo not drawn?)", names[axis]);
        if (!s.ok) continue;
        std::printf("       %s base %s\n", names[axis], str(s.base).c_str());
        CHECK_MSG(dominant(s.base, axis, 40), "axis %s handle at rest is %s - %s must dominate by >=40", names[axis],
                  str(s.base).c_str(), axis == 0 ? "red" : axis == 1 ? "green" : "blue");
    }
}

TEST_GPU(gizmo_hover_is_brighter_than_rest_and_active_is_brighter_than_hover) {
    const char* names[3] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; ++axis) {
        const Sample s = measure(axis);
        CHECK_MSG(s.ok, "axis %s: handle not found", names[axis]);
        if (!s.ok) continue;
        std::printf("       %s base %s hover %s active %s (luma %.0f / %.0f / %.0f)\n", names[axis],
                    str(s.base).c_str(), str(s.hover).c_str(), str(s.active).c_str(), double(luma(s.base)),
                    double(luma(s.hover)), double(luma(s.active)));
        CHECK_MSG(luma(s.hover) > luma(s.base) * 1.03f, "axis %s: hover %s is not brighter than rest %s", names[axis],
                  str(s.hover).c_str(), str(s.base).c_str());
        CHECK_MSG(luma(s.active) > luma(s.hover) * 1.03f, "axis %s: active %s is not brighter than hover %s", names[axis],
                  str(s.active).c_str(), str(s.hover).c_str());
    }
}

TEST_GPU(gizmo_axis_keeps_its_hue_when_hovered_and_when_active) {
    // "X red, Y green, Z blue" must still be true while the handle is highlighted, or the user
    // cannot tell which axis they are dragging. The highlight multiplies the colour and clamps
    // each channel, which can wash a hue out toward white/cyan.
    const char* names[3] = {"X", "Y", "Z"};
    for (int axis = 0; axis < 3; ++axis) {
        const Sample s = measure(axis);
        if (!s.ok) { CHECK_MSG(false, "axis %s: handle not found", names[axis]); continue; }
        CHECK_MSG(dominant(s.hover, axis, 20), "axis %s HOVERED handle is %s: its own hue no longer dominates (washed out)",
                  names[axis], str(s.hover).c_str());
        CHECK_MSG(dominant(s.active, axis, 20), "axis %s ACTIVE handle is %s: its own hue no longer dominates (washed out)",
                  names[axis], str(s.active).c_str());
    }
}

TEST_GPU(gizmo_translate_plane_quads_are_tinted_by_the_third_axis) {
    // Blender: the XY grab square is coloured like Z (the axis it does NOT move along).
    GizmoState g;
    g.mode = GizmoState::Mode::Translate;
    g.origin = {0, 0, 0};
    g.handleLengthPixels = 90.0f;
    render::Renderer r;
    Frame f;
    CHECK_MSG(drawScene(g, render::StandardView::Front, true, f, r), "render failed");
    const float wpp = worldPerPixel(r.camera());
    const float len = 90.0f * wpp;
    // XY quad (Front view: X right, Y up): midpoint of its right-hand edge.
    const Vec3 edge{(0.5f + 0.14f) * len, 0.5f * len, 0.0f};
    float ex, ey;
    CHECK_MSG(r.camera().projectScreen(edge, kW, kH, ex, ey), "cannot project the quad edge");
    Rgb best{}; int bestD = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const Rgb c = f.at(int(std::lround(ex)) + dx, int(std::lround(ey)) + dy);
            if (dist(c, kBg) > bestD) { bestD = dist(c, kBg); best = c; }
        }
    std::printf("       XY plane quad edge at (%.0f,%.0f): %s\n", double(ex), double(ey), str(best).c_str());
    CHECK_MSG(bestD > 60, "no plane-quad edge pixels found at (%.0f,%.0f): the XY plane quad is not drawn", double(ex), double(ey));
    CHECK_MSG(bestD <= 60 || dominant(best, 2, 40), "XY plane quad is %s: it should carry the blue of the third axis (Z)",
              str(best).c_str());
    r.shutdown();
}

TEST_GPU(gizmo_draws_nothing_when_the_gizmo_toggle_is_off) {
    // Control: the same sampling that finds the handle must find NOTHING when showGizmo is off,
    // so the colour tests above cannot be passing on stray pixels.
    GizmoState g;
    g.origin = {0, 0, 0};
    g.handleLengthPixels = 90.0f;
    render::Renderer r;
    Frame f;
    CHECK_MSG(drawScene(g, render::StandardView::Front, false, f, r), "render failed");
    Rgb c;
    const bool found = handleColour(r.camera(), f, g.origin, Vec3{1, 0, 0}, worldPerPixel(r.camera()), 90.0f, c);
    CHECK_MSG(!found, "found handle-coloured pixels %s with showGizmo OFF: the control is contaminated", str(c).c_str());
    r.shutdown();
}

HARNESS_MAIN()
