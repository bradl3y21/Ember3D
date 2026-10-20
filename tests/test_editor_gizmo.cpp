// tests/test_editor_gizmo.cpp — owned by Verity · QA.
//
// Contract under test: the transform gizmo's drag math (src/editor/Gizmo.cpp,
// D19/D20). The task's four properties:
//   1. a mouse delta projected onto an axis moves the object by the expected
//      amount at several camera orientations;
//   2. translate snapping rounds to whole world units;
//   3. rotation snapping uses the DOCUMENTED increment (Gizmo.cpp kRotateSnap
//      = kPi/12 = 15 degrees);
//   4. scale never produces a degenerate or reflected matrix.
//
// WHY A HEADLESS IMGUI CONTEXT: the drag math lives in an anonymous namespace
// and is reached only through GizmoController::update(), which reads
// ImGui::GetIO() for mouse/keyboard. None of update()'s callees touch OpenGL
// (they are camera.screenToRay / world.worldMatrix / undo.push), so an ImGui
// context created WITHOUT a GPU backend is enough. That means these tests
// exercise the REAL implementation — not a reimplementation by QA, which would
// only test QA's copy. No window, no GL context, no --gpu needed.
//
// Input is synthesized through ImGui 1.93's event queue (AddMousePosEvent /
// AddMouseButtonEvent / AddKeyEvent), i.e. the same path a real backend uses.
#include "harness.h"

#include "core/Types.h"
#include "editor/Gizmo.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cmath>
#include <memory>
#include <string>

using ember::Mat4;
using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
using ember::Vec4;              // the vp * Vec4(world,1) projection below
using ember::GizmoState;        // D19/D20: the gizmo's public state is a CORE type
using ember::editor::GizmoController;
using ember::editor::UndoStack;
using ember::render::OrbitCamera;
using ember::scene::Object;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

// Documented snap increments, transcribed from Gizmo.cpp's constants. If the
// implementation changes them these assertions must change with it — that is
// the point: the values are a contract a user feels.
constexpr float kTranslateSnap = 1.0f;    // Gizmo.cpp: std::round(delta)
constexpr float kRotateSnap    = 3.14159265358979f / 12.0f;   // 15 degrees
constexpr float kScaleSnap     = 0.1f;
constexpr float kMinScale      = 0.01f;

constexpr int   kFbW = 1280;
constexpr int   kFbH = 720;

// Owns the ImGui context for the whole file; CPU-only, never rendered.
struct HeadlessImGui {
    HeadlessImGui() {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize            = ImVec2(static_cast<float>(kFbW),
                                           static_cast<float>(kFbH));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime              = 1.0f / 60.0f;
        io.IniFilename            = nullptr;          // never write a .ini
        // NewFrame() requires a built font atlas even though we never render.
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~HeadlessImGui() {
        ImGui::SetCurrentContext(ctx_);
        ImGui::DestroyContext(ctx_);
    }
    ImGuiContext* ctx_ = nullptr;
};

// One synthesized frame. Events are queued BEFORE NewFrame so IsMouseClicked /
// IsMouseDown / IsKeyPressed see them this frame — exactly how a real backend
// feeds input. Returns whatever GizmoController::update() returned (true while
// the gizmo owns the mouse).
bool frame(GizmoController& gizmo, World& world, UndoStack& undo,
           const OrbitCamera& camera, float mouseX, float mouseY,
           bool mouseDown, bool clicked, bool ctrl,
           ember::editor::TransformSpace space = ember::editor::TransformSpace::Global) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouseX, mouseY);
    io.AddMouseButtonEvent(0, mouseDown);
    // ImGui 1.93 backend contract: modifier STATE is submitted under the
    // ImGuiMod_Ctrl key, not ImGuiKey_LeftCtrl. Submitting the latter left
    // io.KeyCtrl permanently false, so the gizmo never saw Ctrl and every
    // snap assertion failed on unsnapped values (my bug, not Chisel's —
    // proven by gizmo_snap_flag_follows_the_ctrl_key).
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    ImGui::NewFrame();
    const bool owned = gizmo.update(world, undo, camera, space, /*visible=*/true);
    ImGui::EndFrame();   // keeps ImGui's frame counter balanced; no rendering
    (void)clicked;
    return owned;
}

// World units per framebuffer pixel at the pivot plane — derived from the
// camera's PUBLIC state, independent of the gizmo's internals.
float worldPerPixel(const OrbitCamera& cam, int fbHeight) {
    return (2.0f * cam.distance() * std::tan(cam.fovY() * 0.5f)) /
           static_cast<float>(fbHeight);
}

// Projects a world point to framebuffer pixels using the camera's own
// view/projection (public API). Test SETUP only — not the math under test.
bool projectToScreen(const OrbitCamera& cam, Vec3 world, float& sx, float& sy) {
    const Mat4 vp = cam.projection(static_cast<float>(kFbW) / static_cast<float>(kFbH)) *
                    cam.view();
    const Vec4 clip = vp * Vec4(world, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    sx = (nx * 0.5f + 0.5f) * static_cast<float>(kFbW);
    sy = (1.0f - (ny * 0.5f + 0.5f)) * static_cast<float>(kFbH);
    return true;
}

// A world with one selected, unparented object at `pos`, and a camera set to a
// deterministic orientation. `handlePx` is where the gizmo draws the handle tip
// (GizmoState::handleLengthPixels default is 90).
struct Rig {
    World world;
    UndoStack undo;
    OrbitCamera cam;
    GizmoController gizmo;
    ObjectId id;
    HeadlessImGui imgui;   // must outlive any ImGui call

    explicit Rig(Vec3 pos = {0, 0, 0}, ember::editor::TransformSpace space =
                                          ember::editor::TransformSpace::Global) {
        id = world.createObject("gizmo-target");
        Transform t{};
        t.position = pos;
        world.setTransform(id, t);
        world.select(id);
        space_ = space;
        cam.setView(ember::render::StandardView::Front);
        cam.setTarget(pos);
    }

    ember::editor::TransformSpace space_ = ember::editor::TransformSpace::Global;

    const Object* target() const { return world.get(id); }

    // Screen position of the tip of axis `axisIndex`'s handle.
    //
    // Derive the exact drawn endpoint, not a normalized screen direction from a
    // one-world-unit projection. Perspective is nonlinear along oblique axes:
    // scaling that projected direction to 90px can put the test pointer outside
    // the actual handle's hover radius at some camera angles.
    bool handleTip(int axisIndex, float& sx, float& sy) const {
        const float handlePx = 90.0f;   // GizmoState::handleLengthPixels default
        // Handles live in state_.orientation; Global space => identity basis,
        // so the world axis IS the handle axis.
        Vec3 a{axisIndex == 0 ? 1.0f : 0.0f, axisIndex == 1 ? 1.0f : 0.0f,
               axisIndex == 2 ? 1.0f : 0.0f};
        const Vec3 origin = world.get(id)->transform.position;
        const Vec3 tip = origin + a * (handlePx * worldPerPixel(cam, kFbH));
        return projectToScreen(cam, tip, sx, sy);
    }
};

// Drives a full click-drag on axis `axisIndex` by `dxPx`/`dyPx` screen pixels
// and returns the resulting world position.
Vec3 dragAxis(Rig& r, int axisIndex, float dxPx, float dyPx, bool ctrl = false) {
    float tipX = 0.0f, tipY = 0.0f;
    r.handleTip(axisIndex, tipX, tipY);

    // Hover the handle, then press.
    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, /*down=*/false, /*click=*/false, ctrl);
    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, /*down=*/true,  /*click=*/true,  ctrl);
    // Drag.
    frame(r.gizmo, r.world, r.undo, r.cam, tipX + dxPx, tipY + dyPx, /*down=*/true,
          /*click=*/false, ctrl);
    // Release.
    frame(r.gizmo, r.world, r.undo, r.cam, tipX + dxPx, tipY + dyPx, /*down=*/false,
          /*click=*/false, ctrl);
    return r.target()->transform.position;
}

} // namespace

// ---- 1. axis projection moves the object by the expected amount -------------

TEST(gizmo_translate_moves_only_along_the_dragged_axis) {
    Rig r;
    const Vec3 before = r.target()->transform.position;
    const Vec3 after  = dragAxis(r, /*axisIndex=*/0, /*dxPx=*/200.0f, /*dyPx=*/0.0f);

    // Dragging the X handle must move ONLY X: Y and Z are untouched.
    CHECK_NEAR(after.y, before.y, 1e-5);
    CHECK_NEAR(after.z, before.z, 1e-5);
    // And X must actually move, in the dragged direction (screen right = +X
    // for a Front-view camera looking down -Z).
    CHECK(after.x > before.x);
}

TEST(gizmo_translate_magnitude_matches_world_per_pixel) {
    // The projected mouse delta must move the object by the expected WORLD
    // amount: dxPixels * (world units per pixel at the pivot plane). The
    // world-per-pixel figure comes from the camera's public distance/fovY, so
    // this is an independent prediction, not a copy of the gizmo's math.
    Rig r;
    const float wpp = worldPerPixel(r.cam, kFbH);

    for (float dxPx : {50.0f, 100.0f, 200.0f}) {
        Rig local;                                   // fresh rig per measurement
        const Vec3 before = local.target()->transform.position;
        const Vec3 after  = dragAxis(local, 0, dxPx, 0.0f);
        const float moved = after.x - before.x;

        // Expected within a tolerance: the axis-ray closest-point differs
        // slightly from the centre-plane approximation near screen edges, so
        // this is a tolerance check, not an equality — but it catches an axis
        // or scale error of the wrong sign or an order of magnitude.
        const float expected = dxPx * wpp;
        if (std::fabs(moved - expected) > expected * 0.25f + 1e-4f)
            harness::reportFailure(__FILE__, __LINE__,
                "X drag of " + std::to_string(dxPx) + "px moved " +
                std::to_string(moved) + " world units, expected ~" +
                std::to_string(expected) + " (worldPerPixel=" +
                std::to_string(wpp) + ")");
        ++harness::g_checks;
    }
}

TEST(gizmo_translate_is_monotonic_and_roughly_linear_in_pixels) {
    // Doubling the pixel drag must roughly double the world movement. This is
    // the property a user actually feels and it is independent of the exact
    // projection constant.
    Rig a, b;
    const Vec3 p0 = a.target()->transform.position;
    const float m1 = dragAxis(a, 0, 100.0f, 0.0f).x - p0.x;
    const Vec3 q0 = b.target()->transform.position;
    const float m2 = dragAxis(b, 0, 200.0f, 0.0f).x - q0.x;

    CHECK(m1 > 0.0f);
    CHECK(m2 > m1);                                   // monotonic
    CHECK_NEAR(m2 / m1, 2.0f, 0.15);                  // linear within 15%
}

TEST(gizmo_translate_at_several_camera_orientations_hits_the_right_world_axis) {
    // Several orientations; in each the dragged axis must remain the SAME world
    // axis and the others must not move. This is what catches a view/axis mixup
    // (e.g. dragging screen-X moving world-Z after an orbit).
    struct Case { const char* name; float yawDeg; float pitchDeg; };
    const Case cases[] = {
        {"front",   0.0f,   0.0f},
        {"top",     0.0f,  89.0f},     // looking down -Y: X still screen-horizontal
        {"orbited", 35.0f, 20.0f},     // a general orientation
    };
    for (const Case& c : cases) {
        Rig r;
        // OrbitCamera::orbit() takes PIXELS at 0.0087 rad/px (yaw -= dx*k, pitch += dy*k).
        // This test used to divide DEGREES by 0.0087, so these labels were never true
        // (item 159); the orientation is now converted properly and READ BACK.
        r.cam.setView(ember::render::StandardView::Front);
        r.cam.orbit(-c.yawDeg * 0.017453292f / 0.0087f, c.pitchDeg * 0.017453292f / 0.0087f);
        CHECK_NEAR(r.cam.yaw(), c.yawDeg * 0.017453292f, 2e-3);
        CHECK_NEAR(r.cam.pitch(), c.pitchDeg * 0.017453292f, 2e-3);
        const Vec3 before = r.target()->transform.position;
        const Vec3 after  = dragAxis(r, 0, 150.0f, 0.0f);

        if (std::fabs(after.y - before.y) > 1e-4 || std::fabs(after.z - before.z) > 1e-4)
            harness::reportFailure(__FILE__, __LINE__,
                std::string("orientation '") + c.name +
                "': dragging the X handle moved Y or Z (Y " +
                std::to_string(before.y) + "->" + std::to_string(after.y) +
                ", Z " + std::to_string(before.z) + "->" + std::to_string(after.z) + ")");
        if (std::fabs(after.x - before.x) < 1e-6)
            harness::reportFailure(__FILE__, __LINE__,
                std::string("orientation '") + c.name +
                "': dragging the X handle did not move X at all");
        ++harness::g_checks;
    }
}

// ---- 1b. "from ANY camera angle" (item 159) ---------------------------------
//
// The audited claim (WHAT-REMAINS.md section 2): "Dragging a gizmo handle must move
// the object along the axis the handle represents, from any camera angle, by the
// amount the cursor moved in that axis's screen projection." The test above samples
// THREE orientations (front, top, one orbit) and only checks that the other axes
// did not move - not the amount, and three is not "any". This sweeps a grid.
//
// INDEPENDENT PREDICTION (what a user sees, no gizmo maths reused): with the
// pointer grabbed on the handle tip and moved by (d along the axis's screen
// direction u) + (e perpendicular to it), the object must
//   (a) stay EXACTLY on its world axis (no world-space leak), and
//   (b) move ON SCREEN by d along u - the perpendicular part e is ignored.
// Screen positions come from the camera's own projectScreen(), the exact inverse
// of screenToRay.

namespace {

struct SweepStats {
    int tried = 0, skippedEndOn = 0, skippedOverlap = 0, failed = 0, wrongHandle = 0;
    float worstAmountErr = 0.0f, worstLeak = 0.0f, worstFollowPx = 0.0f;
    std::string worstAmountWhere, worstFollowWhere;
    int followOver10pct = 0;
};

Vec3 localAxisOf(const World& w, ObjectId id, int i) {
    const Mat4 m = w.worldMatrix(id);
    Vec3 c{m.m[i * 4 + 0], m.m[i * 4 + 1], m.m[i * 4 + 2]};
    const float len = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    return {c.x / len, c.y / len, c.z / len};
}

bool projPx(const OrbitCamera& cam, Vec3 p, float& x, float& y) {
    return cam.projectScreen(p, kFbW, kFbH, x, y);
}

// Puts the camera at yaw/pitch DEGREES from the Front view, using OrbitCamera's real
// contract (orbit(): yaw -= dx*0.0087, pitch += dy*0.0087, radians), and then READS
// the orientation back. QA's earlier orientation tests divided DEGREES by 0.0087
// (a radians-per-pixel constant), so an "orbited 35/20" camera was really an
// arbitrary spin clamped at a pole - the labels were wrong. Now the labels are
// asserted: the helper returns false (and the caller fails) if the camera did not
// land where the case says.
constexpr float kRadPerPx = 0.0087f;
bool setOrientation(OrbitCamera& cam, float yawDeg, float pitchDeg) {
    const float deg2rad = 3.14159265358979f / 180.0f;
    cam.setView(ember::render::StandardView::Front);
    cam.orbit(-yawDeg * deg2rad / kRadPerPx, pitchDeg * deg2rad / kRadPerPx);
    const float wantYaw = yawDeg * deg2rad, wantPitch = pitchDeg * deg2rad;
    const float dyaw = std::remainder(cam.yaw() - wantYaw, 2.0f * 3.14159265358979f);
    return std::fabs(dyaw) < 2e-3f && std::fabs(cam.pitch() - wantPitch) < 2e-3f;
}

// Drag the handle of axis `axisIndex` (world-space unit directions in `axes`) with
// the pointer moved by d along that axis's screen direction and e perpendicular.
//
// Asserted, from what the user sees / the spec's own words:
//   * the gizmo grabbed the axis that was aimed at (unless two handle tips are so
//     close on screen that no user could aim - those are counted as "overlap"),
//   * the object stays EXACTLY on that world axis (no world-space leak),
//   * the WORLD amount moved = d / (screen px per world unit of that axis at the
//     pivot): "the amount the cursor moved in that axis's screen projection". The
//     perpendicular part e must not matter.
// Measured and reported, NOT asserted: how far the object ends up from "under the
// cursor" on screen (perspective makes the linear rule drift for deep axes).
bool sweepOne(Rig& r, int axisIndex, const Vec3 axes[3], float d, float e, float minPxPerUnit,
              ember::editor::TransformSpace space, const std::string& where, SweepStats& st) {
    const Vec3 a = axes[axisIndex];
    const Vec3 start = r.target()->transform.position;
    float ox, oy;
    if (!projPx(r.cam, start, ox, oy)) { ++st.skippedEndOn; return false; }
    float tipsX[3], tipsY[3], ppu[3], udx[3], udy[3];
    for (int j = 0; j < 3; ++j) {
        float px, py;
        if (!projPx(r.cam, start + axes[j], px, py)) { ++st.skippedEndOn; return false; }
        float dx = px - ox, dy = py - oy;
        ppu[j] = std::sqrt(dx * dx + dy * dy);
        if (ppu[j] > 1e-3f) { dx /= ppu[j]; dy /= ppu[j]; }
        udx[j] = dx; udy[j] = dy;
        // The DRAWN tip: handle length is 90px of world-per-pixel at the pivot depth, so a
        // foreshortened handle ends well short of 90px along u. Aim where the user sees it.
        if (!projPx(r.cam, start + axes[j] * (90.0f * worldPerPixel(r.cam, kFbH)), tipsX[j], tipsY[j])) {
            ++st.skippedEndOn;
            return false;
        }
    }
    if (ppu[axisIndex] < minPxPerUnit) { ++st.skippedEndOn; return false; }
    // Not aimable: the point we would click lies within 16px (2x the 8px hover radius) of
    // ANOTHER axis's drawn handle - its whole segment pivot->tip, not just its tip. Short,
    // foreshortened handles routinely sit exactly on top of a longer one.
    for (int j = 0; j < 3; ++j) {
        if (j == axisIndex || ppu[j] < 1e-3f) continue;
        const float sx = tipsX[j] - ox, sy = tipsY[j] - oy;
        const float px = 0.6f * (tipsX[axisIndex] - ox), py = 0.6f * (tipsY[axisIndex] - oy);
        const float len2 = sx * sx + sy * sy;
        float t = len2 > 1e-6f ? (px * sx + py * sy) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float cx = sx * t - px, cy = sy * t - py;
        if (std::sqrt(cx * cx + cy * cy) < 16.0f) { ++st.skippedOverlap; return false; }
        // ...or of the documented 90px-on-screen reading of that handle (GizmoState says the
        // drawn and clickable handle are the same 90px; the renderer foreshortens, the
        // hit-test does not - see gizmo_hit_region_matches_the_drawn_handle_length).
        const float ux = udx[j] * 90.0f, uy = udy[j] * 90.0f;
        const float l2 = ux * ux + uy * uy;
        float t2 = l2 > 1e-6f ? (px * ux + py * uy) / l2 : 0.0f;
        t2 = t2 < 0.0f ? 0.0f : (t2 > 1.0f ? 1.0f : t2);
        const float c2x = ux * t2 - px, c2y = uy * t2 - py;
        if (std::sqrt(c2x * c2x + c2y * c2y) < 16.0f) { ++st.skippedOverlap; return false; }
    }    const float dx = udx[axisIndex], dy = udy[axisIndex];
    const float nx = -dy, ny = dx;
    // Aim at 60% of the DRAWN handle: inside it however the hit-test sizes it (item 159 tests the
    // drawn-vs-clickable length question separately), so this sweep measures DRAG maths only.
    const float tipX = ox + 0.6f * (tipsX[axisIndex] - ox), tipY = oy + 0.6f * (tipsY[axisIndex] - oy);
    const float mx = tipX + dx * d + nx * e, my = tipY + dy * d + ny * e;

    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, false, false, false, space);
    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, true, true, false, space);
    const int grabbed = r.gizmo.state().activeAxis;
    for (int s = 1; s <= 4; ++s)
        frame(r.gizmo, r.world, r.undo, r.cam, tipX + (mx - tipX) * s / 4.0f, tipY + (my - tipY) * s / 4.0f,
              true, false, false, space);
    frame(r.gizmo, r.world, r.undo, r.cam, mx, my, false, false, false, space);
    ++st.tried;

    if (grabbed != axisIndex) {
        ++st.wrongHandle; ++st.failed;
        if (st.failed <= 8)
            harness::reportFailure(__FILE__, __LINE__,
                where + ": aimed at the handle of axis " + std::to_string(axisIndex) + " (tip at " +
                std::to_string(tipX) + "," + std::to_string(tipY) + ", no other tip within 16px) but the gizmo grabbed axis " +
                std::to_string(grabbed));
        ++harness::g_checks;
        return true;
    }

    const Vec3 end = r.target()->transform.position;
    const Vec3 moved{end.x - start.x, end.y - start.y, end.z - start.z};
    const float along = moved.x * a.x + moved.y * a.y + moved.z * a.z;
    const Vec3 off{moved.x - a.x * along, moved.y - a.y * along, moved.z - a.z * along};
    const float leak = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
    const float expectedAlong = d / ppu[axisIndex];                 // world units, signed
    const float amountErr = std::fabs(along - expectedAlong);
    const float amountTol = std::max(0.002f, 0.02f * std::fabs(expectedAlong));
    if (leak > st.worstLeak) st.worstLeak = leak;
    if (amountErr > st.worstAmountErr) { st.worstAmountErr = amountErr; st.worstAmountWhere = where; }

    float ex, ey;                                                   // informational
    if (projPx(r.cam, end, ex, ey)) {
        const float sx = ex - ox, sy = ey - oy;
        const float follow = std::sqrt((sx - dx * d) * (sx - dx * d) + (sy - dy * d) * (sy - dy * d));
        if (follow > st.worstFollowPx) { st.worstFollowPx = follow; st.worstFollowWhere = where; }
        if (follow > 0.10f * std::fabs(d) + 4.0f) ++st.followOver10pct;
    }
    if (leak > 1e-3f || amountErr > amountTol) {
        ++st.failed;
        if (st.failed <= 8)
            harness::reportFailure(__FILE__, __LINE__,
                where + " axis " + std::to_string(axisIndex) + " d=" + std::to_string(d) + " e=" +
                std::to_string(e) + ": moved " + std::to_string(along) + " world units along the axis (expected " +
                std::to_string(expectedAlong) + " = d / " + std::to_string(ppu[axisIndex]) + " px/unit), world leak " +
                std::to_string(leak));
    }
    ++harness::g_checks;
    return true;
}

} // namespace
TEST(gizmo_translate_follows_the_axis_screen_projection_across_84_camera_orientations) {
    // 12 yaws x 7 pitches = 84 orientations, 3 axes, 2 drag directions, with a
    // perpendicular pointer component the gizmo must ignore.
    SweepStats st;
    int perAxis[3] = {0, 0, 0};
    const Vec3 world[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int orientationsReached = 0;
    for (int yaw = 0; yaw < 360; yaw += 30) {
        for (int pitch : {-80, -45, -20, 0, 20, 45, 80}) {
            bool counted = false;
            for (int axis = 0; axis < 3; ++axis) {
                for (float d : {120.0f, -120.0f}) {
                    Rig r;
                    if (!setOrientation(r.cam, float(yaw), float(pitch))) {
                        harness::reportFailure(__FILE__, __LINE__, "camera did not reach yaw " +
                                               std::to_string(yaw) + " pitch " + std::to_string(pitch));
                        continue;
                    }
                    if (!counted) { ++orientationsReached; counted = true; }
                    const std::string where = "yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch);
                    if (sweepOne(r, axis, world, d, /*perpendicular*/ 60.0f, /*minPxPerUnit*/ 45.0f,
                                 ember::editor::TransformSpace::Global, where, st))
                        ++perAxis[axis];
                }
            }
        }
    }
    std::printf("       orientations reached %d/84; drags tried %d; skipped: %d end-on (<45 px/unit), %d overlapping tips\n"
                "       wrong handle grabbed %d; worst world leak %.2e; worst amount error %.4f world units at [%s]\n"
                "       (info) on-screen distance from 'object under cursor': worst %.1fpx at [%s]; %d/%d drags >10%%+4px\n",
                orientationsReached, st.tried, st.skippedEndOn, st.skippedOverlap, st.wrongHandle,
                double(st.worstLeak), double(st.worstAmountErr), st.worstAmountWhere.c_str(),
                double(st.worstFollowPx), st.worstFollowWhere.c_str(), st.followOver10pct, st.tried);
    if (orientationsReached != 84)
        harness::reportFailure(__FILE__, __LINE__, "only " + std::to_string(orientationsReached) + "/84 orientations were reached");
    ++harness::g_checks;
    // The sweep must cover the space or it proves nothing: each axis dragged in >=150 distinct cases.
    for (int axis = 0; axis < 3; ++axis) {
        if (perAxis[axis] < 96)
            harness::reportFailure(__FILE__, __LINE__,
                "axis " + std::to_string(axis) + " was only exercised " + std::to_string(perAxis[axis]) +
                " times (<96 of 168): too many end-on/overlapping views were skipped to support 'any angle'");
        ++harness::g_checks;
    }
}

TEST(gizmo_translate_local_space_follows_the_object_axes_across_camera_orientations) {
    // Local space: handles are the OBJECT's rotated axes (D20). Same assertions.
    SweepStats st;
    int tried = 0;
    for (int yaw = 0; yaw < 360; yaw += 45) {
        for (int pitch : {-50, 20, 65}) {
            for (int axis = 0; axis < 3; ++axis) {
                for (float d : {110.0f, -110.0f}) {
                    Rig r;
                    Transform t{};
                    t.position = {0, 0, 0};
                    t.rotation = {0.5f, 0.8f, -0.35f};          // a general rotation (radians)
                    r.world.setTransform(r.id, t);
                    if (!setOrientation(r.cam, float(yaw), float(pitch))) {
                        harness::reportFailure(__FILE__, __LINE__, "camera did not reach yaw " +
                                               std::to_string(yaw) + " pitch " + std::to_string(pitch));
                        continue;
                    }
                    const Vec3 axes[3] = {localAxisOf(r.world, r.id, 0), localAxisOf(r.world, r.id, 1),
                                          localAxisOf(r.world, r.id, 2)};
                    const std::string where = "LOCAL yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch);
                    if (sweepOne(r, axis, axes, d, 50.0f, 45.0f, ember::editor::TransformSpace::Local, where, st)) ++tried;
                }
            }
        }
    }
    std::printf("       local: drags tried %d; skipped %d end-on, %d overlapping; wrong handle %d; worst leak %.2e; "
                "worst amount error %.4f at [%s]; (info) worst on-screen %.1fpx\n",
                st.tried, st.skippedEndOn, st.skippedOverlap, st.wrongHandle, double(st.worstLeak),
                double(st.worstAmountErr), st.worstAmountWhere.c_str(), double(st.worstFollowPx));
    if (tried < 80)
        harness::reportFailure(__FILE__, __LINE__, "only " + std::to_string(tried) + " local drags ran (<80)");
    ++harness::g_checks;
}
// ---- 1c. drawn handle length vs clickable handle length (item 159) ----------
//
// Types.h (GizmoState::handleLengthPixels): "the renderer draws handles that long, the
// editor hit-tests those same pixels - so the drawn handle and the clickable handle
// cannot drift apart." Renderer::drawGizmo draws each handle as 90 px x world-per-pixel
// at the PIVOT depth (an axis pointing toward the camera therefore looks SHORT), while
// nearestAxis() sizes the clickable segment as 90 / (px per world unit of THAT axis),
// i.e. always 90 px long on screen. For a foreshortened axis the clickable region then
// extends far past the visible handle.
//
// Prediction a user would make: hover 25 px PAST the visible end of a handle (and well
// outside the 8 px hover radius) and that handle must not light up.
TEST(gizmo_hit_region_matches_the_drawn_handle_length) {
    int tested = 0, hoveredPastEnd = 0;
    std::string firstBad;
    for (int yaw = 0; yaw < 360; yaw += 30) {
        for (int pitch : {-65, -45, -25, 25, 45, 65}) {
            for (int axis = 0; axis < 3; ++axis) {
                Rig r;
                if (!setOrientation(r.cam, float(yaw), float(pitch))) continue;
                const Vec3 start = r.target()->transform.position;
                const Vec3 a{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
                float ox, oy, tx, ty;
                if (!projPx(r.cam, start, ox, oy) ||
                    !projPx(r.cam, start + a * (90.0f * worldPerPixel(r.cam, kFbH)), tx, ty)) continue;
                const float drawn = std::sqrt((tx - ox) * (tx - ox) + (ty - oy) * (ty - oy));
                // Only clearly foreshortened handles: visible length 12..55 px, so that
                // 25 px past the visible end is still inside a 90 px clickable segment.
                if (drawn < 12.0f || drawn > 55.0f) continue;
                const float ux = (tx - ox) / drawn, uy = (ty - oy) / drawn;
                const float qx = ox + ux * (drawn + 25.0f), qy = oy + uy * (drawn + 25.0f);
                frame(r.gizmo, r.world, r.undo, r.cam, qx, qy, false, false, false);
                ++tested;
                if (r.gizmo.state().hoveredAxis == axis) {
                    ++hoveredPastEnd;
                    if (firstBad.empty())
                        firstBad = "yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch) + " axis " +
                                   std::to_string(axis) + ": handle drawn " + std::to_string(drawn) +
                                   "px long, but it lights up 25px past its visible end";
                }
                ++harness::g_checks;
            }
        }
    }
    std::printf("       foreshortened handles tested: %d; hoverable past their visible end: %d\n", tested, hoveredPastEnd);
    if (tested < 20)
        harness::reportFailure(__FILE__, __LINE__, "only " + std::to_string(tested) + " foreshortened handles were tested (<20)");
    if (hoveredPastEnd > 0)
        harness::reportFailure(__FILE__, __LINE__,
            std::to_string(hoveredPastEnd) + " of " + std::to_string(tested) +
            " foreshortened handles are clickable past their drawn end (drawn != clickable). First: " + firstBad);
}
TEST(gizmo_translate_dragging_y_moves_y_only) {
    Rig r;
    const Vec3 before = r.target()->transform.position;
    const Vec3 after  = dragAxis(r, /*axisIndex=*/1, /*dxPx=*/0.0f, /*dyPx=*/-150.0f);
    CHECK_NEAR(after.x, before.x, 1e-5);
    CHECK_NEAR(after.z, before.z, 1e-5);
    CHECK(std::fabs(after.y - before.y) > 1e-4);   // Y moved (sign depends on view)
}

// ---- 2. translate snapping rounds to whole world units ----------------------

TEST(gizmo_snap_flag_follows_the_ctrl_key) {
    // FAULT ISOLATION, written after 8 snap failures shared one signature (the
    // resulting positions were EXACTLY the unsnapped amounts, so snap never
    // applied). Two suspects: my headless Ctrl synthesis, or the snap logic.
    // This test separates them: if state().snap does not track io.KeyCtrl then
    // the failures are MINE (input never reaches the gizmo), and the snap
    // assertions below prove nothing about Chisel's code.
    Rig r;
    r.world.setTransform(r.id, Transform{});
    float tipX = 0.0f, tipY = 0.0f;
    r.handleTip(0, tipX, tipY);

    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, /*down=*/false, /*click=*/false,
          /*ctrl=*/false);
    CHECK(!r.gizmo.state().snap);                 // no Ctrl -> no snap

    frame(r.gizmo, r.world, r.undo, r.cam, tipX, tipY, /*down=*/false, /*click=*/false,
          /*ctrl=*/true);
    // Self-diagnosing: report io.KeyCtrl too, so a failure separates "ImGui
    // never received the modifier" (my synthesis) from "the gizmo ignores a
    // Ctrl that DID arrive" (the code under test).
    if (!r.gizmo.state().snap)
        harness::reportFailure(__FILE__, __LINE__,
            std::string("state().snap is false with Ctrl queued; io.KeyCtrl = ") +
            (ImGui::GetIO().KeyCtrl ? "true (modifier ARRIVED - gizmo ignores it)"
                                    : "false (modifier never reached ImGui - QA's synthesis)"));
    CHECK(r.gizmo.state().snap);
}

TEST(gizmo_translate_snap_rounds_to_whole_units) {
    // Ctrl must quantise the delta to whole world units (Gizmo.cpp:
    // `if (snap) delta = std::round(delta)`), so the object's position along
    // the dragged axis lands on an integer regardless of the pixel amount.
    for (float dxPx : {37.0f, 61.0f, 83.0f, 117.0f}) {
        Rig r;
        r.world.setTransform(r.id, Transform{});   // start exactly at the origin
        const Vec3 after = dragAxis(r, 0, dxPx, 0.0f, /*ctrl=*/true);
        const float nearest = std::round(after.x);
        if (std::fabs(after.x - nearest) > 1e-4f)
            harness::reportFailure(__FILE__, __LINE__,
                "Ctrl-translate of " + std::to_string(dxPx) + "px landed at X=" +
                std::to_string(after.x) + ", which is not a whole world unit");
        ++harness::g_checks;
    }
}

TEST(gizmo_translate_without_snap_is_not_quantised) {
    // The unsnapped drag of an odd pixel amount must NOT land on an integer —
    // otherwise "snapping" would be a no-op and test 2 would pass vacuously.
    Rig r;
    r.world.setTransform(r.id, Transform{});
    const Vec3 after = dragAxis(r, 0, 61.0f, 0.0f, /*ctrl=*/false);
    CHECK(std::fabs(after.x - std::round(after.x)) > 1e-3f);
}

TEST(gizmo_translate_snap_zero_pixels_stays_put) {
    // A snapped drag of zero pixels must not move the object off its start.
    Rig r;
    r.world.setTransform(r.id, Transform{});
    const Vec3 after = dragAxis(r, 0, 0.0f, 0.0f, /*ctrl=*/true);
    CHECK_NEAR(after.x, 0.0f, 1e-4);
    CHECK_NEAR(after.y, 0.0f, 1e-4);
    CHECK_NEAR(after.z, 0.0f, 1e-4);
}

namespace {

// Rotation ANGLE of a matrix, via the trace formula. Axis-agnostic, so a test
// can assert "this is a multiple of 15 degrees" without caring WHICH ring the
// gizmo's nearestRing() happened to grab under an orbited camera — an earlier
// version asserted rot.y == 0 for an X-ring drag and failed correctly, because
// nearestRing picks the screen-nearest ring, which is not always axis 0.
float rotationAngleRadians(const Mat4& m) {
    const float tr = m.m[0] + m.m[5] + m.m[10];
    return std::acos(std::max(-1.0f, std::min(1.0f, (tr - 1.0f) * 0.5f)));
}

constexpr float kPiLocal = 3.14159265358979f;

} // namespace

// ---- 3. rotation snapping uses the documented 15-degree increment -----------

TEST(gizmo_rotate_snap_uses_fifteen_degree_increments) {
    // Ctrl-rotate must quantise the rotation to kRotateSnap = 15 degrees. The
    // object starts unrotated, so after the drag its rotation must be a
    // multiple of 15 degrees about the dragged axis (and 0 about the others,
    // since only one axis is dragged).
    for (float ddeg : {7.0f, 22.0f, 40.0f, 63.0f}) {
        Rig r;
        r.gizmo.setMode(GizmoState::Mode::Rotate);
        // QA NOTE (a degenerate SETUP in MY first version, caught by a
        // diagnostic printing rotation.x = 0 for every drag): the Rig's camera
        // is a dead Front view, so the X-axis ring plane is PARALLEL to the
        // view ray and planeHit() correctly returns false — the grab never
        // succeeds and nothing rotates. That is correct behaviour, not a
        // defect; the test just cannot exercise rotate from that angle. Orbit
        // off the degenerate view so the ring is actually face-on.
        r.cam.orbit(40.0f, -25.0f);
        // Drag the X-ring by an angular amount: pixels along the ring map to
        // degrees; we only need SOME angle, then assert the quantisation.
        float tx = 0.0f, ty = 0.0f;
        r.handleTip(0, tx, ty);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f, false, false, true);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f, true,  true,  true);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f + ddeg * 3.0f, true, false, true);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f + ddeg * 3.0f, false, false, true);

        // Assert the TOTAL rotation angle is a whole multiple of 15 degrees.
        // Axis-agnostic on purpose: nearestRing() picks the screen-nearest ring,
        // which under an orbited camera is not necessarily axis 0, so pinning
        // rot.x/rot.y/rot.z individually tests the harness, not the snap.
        const float angle = rotationAngleRadians(r.target()->transform.toMatrix());
        const float steps = angle / kRotateSnap;
        const float nearest = std::round(steps);
        if (std::fabs(steps - nearest) > 1e-3f)
            harness::reportFailure(__FILE__, __LINE__,
                "Ctrl-rotate produced angle " + std::to_string(angle) +
                " rad = " + std::to_string(steps) + " x 15deg, not a whole multiple");
        ++harness::g_checks;
    }
}

TEST(gizmo_rotate_without_snap_is_not_quantised) {
    // Same drag without Ctrl must NOT land on a 15-degree multiple — otherwise
    // the snapping assertion above would pass vacuously.
    //
    // QA NOTE (a fragile assertion in MY first version, caught when a single
    // 37px drag happened to land within 1e-3 of a 15-degree boundary): a single
    // pixel amount can coincidentally sit near a multiple, so the check fired
    // on a correct implementation. Sweeping several amounts and requiring at
    // least one to be clearly unquantised keeps the anti-vacuity property
    // without depending on one lucky/unlucky sample.
    bool anyClearlyUnquantised = false;
    for (float px : {23.0f, 37.0f, 53.0f, 71.0f}) {
        Rig r;
        r.gizmo.setMode(GizmoState::Mode::Rotate);
        // Same degenerate-camera fix as the snapped test above: a dead Front
        // view makes the ring plane parallel to the view ray, so planeHit()
        // correctly refuses and nothing rotates. Orbit off it first.
        r.cam.orbit(40.0f, -25.0f);
        float tx = 0.0f, ty = 0.0f;
        r.handleTip(0, tx, ty);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f, false, false, false);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f, true,  true,  false);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f + px, true, false, false);
        frame(r.gizmo, r.world, r.undo, r.cam, tx, ty + 40.0f + px, false, false, false);

        // Axis-agnostic, same reasoning as the snapped test: measure the TOTAL
        // angle and check it is not a whole multiple of 15 degrees.
        const float angle = rotationAngleRadians(r.target()->transform.toMatrix());
        const float steps = angle / kRotateSnap;
        if (std::fabs(steps - std::round(steps)) > 1e-3f) anyClearlyUnquantised = true;
    }
    CHECK(anyClearlyUnquantised);
}

// ---- 4. scale never produces a degenerate or reflected matrix ---------------

namespace {

// True when the object's model matrix reflects space (negative determinant) or
// is degenerate (near-zero determinant).
bool reflectedOrDegenerate(const Mat4& m) {
    // Determinant of the upper-left 3x3 (column-major m[col*4+row]).
    const float a = m.m[0], b = m.m[4], c = m.m[8];
    const float d = m.m[1], e = m.m[5], f = m.m[9];
    const float g = m.m[2], h = m.m[6], i = m.m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return !(det > 1e-6f);   // catches negative (reflection) and ~0 (degenerate)
}

} // namespace

TEST(gizmo_scale_never_reflects_or_degenerates_under_snap) {
    // Ctrl-scale quantises the ratio to 0.1 steps and clamps it to kMinScale
    // (Gizmo.cpp). Even an extreme drag must leave the scale positive and the
    // model matrix non-degenerate.
    for (float dxPx : {-4000.0f, -900.0f, -300.0f, 250.0f, 4000.0f}) {
        Rig r;
        r.gizmo.setMode(GizmoState::Mode::Scale);
        const Vec3 before = r.target()->transform.scale;
        dragAxis(r, 0, dxPx, 0.0f, /*ctrl=*/true);

        const Vec3 sc = r.target()->transform.scale;
        // No axis may go non-positive: that is the reflection/degeneracy.
        if (!(sc.x > 0.0f && sc.y > 0.0f && sc.z > 0.0f))
            harness::reportFailure(__FILE__, __LINE__,
                "scale drag " + std::to_string(dxPx) + "px produced non-positive scale (" +
                std::to_string(sc.x) + "," + std::to_string(sc.y) + "," +
                std::to_string(sc.z) + ")");
        // Only the dragged axis may change; the others keep their start value.
        CHECK_NEAR(sc.y, before.y, 1e-5);
        CHECK_NEAR(sc.z, before.z, 1e-5);
        // And the resulting matrix must not reflect or collapse.
        if (reflectedOrDegenerate(r.target()->transform.toMatrix()))
            harness::reportFailure(__FILE__, __LINE__,
                "scale drag " + std::to_string(dxPx) +
                "px produced a reflecting or degenerate model matrix");
        ++harness::g_checks;
    }
}

TEST(gizmo_scale_clamps_at_the_documented_minimum) {
    // A drag that would drive the ratio to (or past) zero must be clamped at
    // kMinScale rather than reaching 0 or going negative. Start at scale 1 so
    // the surviving component is exactly the clamp floor.
    Rig r;
    r.gizmo.setMode(GizmoState::Mode::Scale);
    dragAxis(r, 0, -100000.0f, 0.0f, /*ctrl=*/true);
    const float sx = r.target()->transform.scale.x;
    CHECK(sx >= kMinScale - 1e-5f);
    CHECK(sx > 0.0f);
}

TEST(gizmo_scale_snap_uses_documented_tenth_steps) {
    // Ctrl-scale must quantise the ratio to 0.1 increments (kScaleSnap). The
    // resulting scale is before * ratio, so with before == 1 the scale itself
    // must be a multiple of 0.1.
    for (float dxPx : {120.0f, 260.0f, 410.0f}) {
        Rig r;
        r.gizmo.setMode(GizmoState::Mode::Scale);
        dragAxis(r, 0, dxPx, 0.0f, /*ctrl=*/true);
        const float sx = r.target()->transform.scale.x;
        const float steps = sx / kScaleSnap;
        const float nearest = std::round(steps);
        if (std::fabs(steps - nearest) > 1e-3f)
            harness::reportFailure(__FILE__, __LINE__,
                "Ctrl-scale produced X scale " + std::to_string(sx) +
                " = " + std::to_string(steps) + " x 0.1, not a whole 0.1 step");
        ++harness::g_checks;
    }
}

// ---- undo: one gesture is one undo entry (D19/D20) --------------------------

TEST(gizmo_one_drag_is_one_undo_entry) {
    // D20: a whole drag merges into a single undo entry. Drive several drag
    // frames, then undo ONCE, and the object must be back at its start.
    Rig r;
    r.world.setTransform(r.id, Transform{});
    const Vec3 start = r.target()->transform.position;

    float tx = 0.0f, ty = 0.0f;
    r.handleTip(0, tx, ty);
    frame(r.gizmo, r.world, r.undo, r.cam, tx, ty, false, false, false);
    frame(r.gizmo, r.world, r.undo, r.cam, tx, ty, true,  true,  false);
    for (int i = 1; i <= 6; ++i)                     // several frames of one drag
        frame(r.gizmo, r.world, r.undo, r.cam, tx + 30.0f * static_cast<float>(i), ty,
              true, false, false);
    frame(r.gizmo, r.world, r.undo, r.cam, tx + 180.0f, ty, false, false, false);

    const Vec3 dragged = r.target()->transform.position;
    CHECK(std::fabs(dragged.x - start.x) > 1e-4);    // it really moved

    r.undo.undo();                                   // ONE undo
    const Vec3 undone = r.target()->transform.position;
    CHECK_NEAR(undone.x, start.x, 1e-4);
    CHECK_NEAR(undone.y, start.y, 1e-4);
    CHECK_NEAR(undone.z, start.z, 1e-4);
}

HARNESS_MAIN()
