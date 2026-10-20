#include "editor/SelfTest.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

#include "core/Log.h"

namespace ember::editor {

namespace {

struct SelfTest {
    ObjectId cube;
    Vec3 startPos{0.0f, 0.0f, 0.0f};
    Vec3 phaseAPos{0.0f, 0.0f, 0.0f};
    Vec3 phaseFStart{0.0f, 0.0f, 0.0f};
    Vec3 phaseGStart{0.0f, 0.0f, 0.0f};
    Vec3 editBefore{};
    Vec3 editAfter{};
    int checks = 0;
    int failed = 0;
};

SelfTest g_st;

void check(bool ok, const char* name, const char* detail) {
    ++g_st.checks;
    if (!ok) ++g_st.failed;
    EMBER_LOG_INFO("[selftest] %s %s: %s", ok ? "PASS" : "FAIL", name, detail);
}

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// One scripted key press: down on `downFrame`, up on the next.
void tap(ImGuiIO& io, int frame, int downFrame, ImGuiKey key) {
    if (frame == downFrame) io.AddKeyEvent(key, true);
    if (frame == downFrame + 1) io.AddKeyEvent(key, false);
}

} // namespace

// The frames are spaced so every key edge lands in its own ImGui frame, the way
// Verity's gizmo harness drives input. Mouse position is re-posted every frame.
void runTransformSelfTestImpl(scene::World& world, UndoStack& undo,
                              const render::OrbitCamera& camera, InteractionMode& interaction,
                              SelectMode& selectMode, PivotMode& pivotMode, int frame) {
    (void)camera;
    ImGuiIO& io = ImGui::GetIO();
    // ABSOLUTE mouse positions: the drag vector must not depend on the window's
    // current size (live desktop — the window can be resized mid-run).
    const float cx = 640.0f;
    const float cy = 360.0f;

    // The script's current mouse target.
    float mx = cx;
    float my = cy;
    if (frame >= 26 && frame < 44) mx = cx + 120.0f;   // cancel-drag distance
    if (frame >= 56) mx = cx + 150.0f;                 // confirm-drag distance
    if (frame >= 102) mx = cx + 260.0f;                // Shift+X plane-drag distance
    io.AddMousePosEvent(mx, my);

    // ---- phase 0: a cube to transform, selected ---------------------------
    if (frame == 2) {
        g_st = SelfTest{};
        g_st.cube = world.createPrimitive(scene::Primitive::Cube, "SelfTestCube");
        world.select(g_st.cube);
        const scene::Object* o = world.get(g_st.cube);
        g_st.startPos = o ? o->transform.position : Vec3{};
        EMBER_LOG_INFO("[selftest] start: cube id=%u at (%.3f, %.3f, %.3f)", g_st.cube.value,
                       static_cast<double>(g_st.startPos.x), static_cast<double>(g_st.startPos.y),
                       static_cast<double>(g_st.startPos.z));
    }

    // ---- phase A: G, X, type "2", Enter -> x += 2 exactly, one undo -------
    tap(io, frame, 4, ImGuiKey_G);
    tap(io, frame, 6, ImGuiKey_X);
    tap(io, frame, 8, ImGuiKey_2);
    tap(io, frame, 10, ImGuiKey_Enter);

    if (frame == 14) {
        const scene::Object* o = world.get(g_st.cube);
        const Vec3 p = o ? o->transform.position : Vec3{};
        g_st.phaseAPos = p;
        char buf[128];
        std::snprintf(buf, sizeof buf, "expected x=%.3f, got (%.4f, %.4f, %.4f)",
                      static_cast<double>(g_st.startPos.x + 2.0f), static_cast<double>(p.x),
                      static_cast<double>(p.y), static_cast<double>(p.z));
        check(o && near(p.x, g_st.startPos.x + 2.0f, 1e-4f), "A1 typed x+2 exact", buf);
        std::snprintf(buf, sizeof buf, "y=%.4f z=%.4f (start %.4f, %.4f)", static_cast<double>(p.y),
                      static_cast<double>(p.z), static_cast<double>(g_st.startPos.y),
                      static_cast<double>(g_st.startPos.z));
        check(o && near(p.y, g_st.startPos.y, 1e-6f) && near(p.z, g_st.startPos.z, 1e-6f),
              "A2 other axes untouched", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 1", undo.doneCount());
        check(undo.doneCount() == 1, "A3 one undo entry", buf);
    }

    // ---- phase B: G, drag, Esc -> restored exactly, no new undo entry -----
    tap(io, frame, 20, ImGuiKey_G);
    // mouse is at cx+120 from frame 26 on: the drag happens
    tap(io, frame, 30, ImGuiKey_Escape);

    if (frame == 34) {
        const scene::Object* o = world.get(g_st.cube);
        const Vec3 p = o ? o->transform.position : Vec3{};
        char buf[128];
        std::snprintf(buf, sizeof buf, "expected (%.4f, %.4f, %.4f), got (%.4f, %.4f, %.4f)",
                      static_cast<double>(g_st.phaseAPos.x), static_cast<double>(g_st.phaseAPos.y),
                      static_cast<double>(g_st.phaseAPos.z), static_cast<double>(p.x),
                      static_cast<double>(p.y), static_cast<double>(p.z));
        check(o && near(p.x, g_st.phaseAPos.x, 1e-6f) && near(p.y, g_st.phaseAPos.y, 1e-6f) &&
                  near(p.z, g_st.phaseAPos.z, 1e-6f),
              "B1 cancel restores exactly", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 1", undo.doneCount());
        check(undo.doneCount() == 1, "B2 cancel leaves no entry", buf);
    }

    // ---- phase C: G, X, drag +150 px, Enter -> +x move, second undo entry --
    tap(io, frame, 44, ImGuiKey_G);
    tap(io, frame, 46, ImGuiKey_X);
    // mouse moves to cx+150 at frame 56
    tap(io, frame, 60, ImGuiKey_Enter);

    if (frame == 64) {
        // The pixel-drag magnitude is environment-dependent (real window, live
        // desktop): the proof here is direction + constraint + undo depth, which
        // are exact. The typed path (phase A) covers exact magnitude.
        const scene::Object* o = world.get(g_st.cube);
        const Vec3 p = o ? o->transform.position : Vec3{};
        char buf[128];
        std::snprintf(buf, sizeof buf, "phaseA x=%.4f, got x=%.4f", static_cast<double>(g_st.phaseAPos.x),
                      static_cast<double>(p.x));
        check(o && p.x > g_st.phaseAPos.x + 0.3f, "C1 mouse drag moves +x", buf);
        std::snprintf(buf, sizeof buf, "y=%.4f z=%.4f (phaseA %.4f, %.4f)", static_cast<double>(p.y),
                      static_cast<double>(p.z), static_cast<double>(g_st.phaseAPos.y),
                      static_cast<double>(g_st.phaseAPos.z));
        check(o && near(p.y, g_st.phaseAPos.y, 1e-6f) && near(p.z, g_st.phaseAPos.z, 1e-6f),
              "C2 axis constraint holds", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 2", undo.doneCount());
        check(undo.doneCount() == 2, "C3 second gesture is one more entry", buf);
    }

    // ---- phase D: select a real editable vertex, G/X/type 1/Enter -----------
    if (frame == 68) {
        if (world.editable(g_st.cube)) {
            interaction = InteractionMode::Edit;
            selectMode = SelectMode::Vertex;
            world.editSetSelection(g_st.cube, SelectMode::Vertex, {0u});
            const auto before = world.editCaptureVertexPositions(g_st.cube, {0u});
            if (!before.empty()) g_st.editBefore = before[0];
            EMBER_LOG_INFO("[selftest] edit vertex 0 before=(%.4f, %.4f, %.4f)",
                           static_cast<double>(g_st.editBefore.x), static_cast<double>(g_st.editBefore.y),
                           static_cast<double>(g_st.editBefore.z));
        }
    }
    tap(io, frame, 70, ImGuiKey_G);
    tap(io, frame, 72, ImGuiKey_X);
    tap(io, frame, 74, ImGuiKey_1);
    tap(io, frame, 76, ImGuiKey_Enter);
    if (frame == 80) {
        const auto after = world.editCaptureVertexPositions(g_st.cube, {0u});
        if (!after.empty()) g_st.editAfter = after[0];
        char buf[128];
        std::snprintf(buf, sizeof buf, "vertex0 local before=(%.4f,%.4f,%.4f) after=(%.4f,%.4f,%.4f)",
                      static_cast<double>(g_st.editBefore.x), static_cast<double>(g_st.editBefore.y),
                      static_cast<double>(g_st.editBefore.z), static_cast<double>(g_st.editAfter.x),
                      static_cast<double>(g_st.editAfter.y), static_cast<double>(g_st.editAfter.z));
        check(!after.empty() && std::fabs(g_st.editAfter.x - (g_st.editBefore.x + 1.0f)) < 1e-4f,
              "D1 modal edit vertex typed X+1", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 3", undo.doneCount());
        check(undo.doneCount() == 3, "D2 edit gesture one undo entry", buf);
    }

    // ---- phase E: second vertex gesture, then Esc restores bitwise ----------
    tap(io, frame, 82, ImGuiKey_G);
    tap(io, frame, 84, ImGuiKey_X);
    tap(io, frame, 86, ImGuiKey_3);
    tap(io, frame, 88, ImGuiKey_Escape);
    if (frame == 92) {
        const auto restored = world.editCaptureVertexPositions(g_st.cube, {0u});
        const Vec3 p = restored.empty() ? Vec3{} : restored[0];
        char buf[128];
        std::snprintf(buf, sizeof buf, "expected (%.4f,%.4f,%.4f), got (%.4f,%.4f,%.4f)",
                      static_cast<double>(g_st.editAfter.x), static_cast<double>(g_st.editAfter.y),
                      static_cast<double>(g_st.editAfter.z), static_cast<double>(p.x),
                      static_cast<double>(p.y), static_cast<double>(p.z));
        check(!restored.empty() && p == g_st.editAfter, "E1 cancel restores edit vertex exactly", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 3", undo.doneCount());
        check(undo.doneCount() == 3, "E2 edit cancel adds no undo entry", buf);
        interaction = InteractionMode::Object;
    }

    // ---- phase F: G, Shift+X, drag, Enter -> motion in the YZ plane --------
    // The camera is oblique, so a horizontal screen drag moves in Y/Z; the
    // component perpendicular to the plane must remain fixed.
    tap(io, frame, 96, ImGuiKey_G);
    // ImGui's modern event queue expects modifier state as ImGuiMod_Shift;
    // sending a physical LeftShift key alone does not synthesize the modifier.
    tap(io, frame, 98, ImGuiMod_Shift);
    tap(io, frame, 98, ImGuiKey_X);
    tap(io, frame, 106, ImGuiKey_Enter);
    if (frame == 96) {
        const scene::Object* o = world.get(g_st.cube);
        g_st.phaseFStart = o ? o->transform.position : Vec3{};
        EMBER_LOG_INFO("[selftest] plane start: (%.4f, %.4f, %.4f)",
                       static_cast<double>(g_st.phaseFStart.x),
                       static_cast<double>(g_st.phaseFStart.y),
                       static_cast<double>(g_st.phaseFStart.z));
    }
    if (frame == 110) {
        const scene::Object* o = world.get(g_st.cube);
        const Vec3 p = o ? o->transform.position : Vec3{};
        const Vec3 delta = p - g_st.phaseFStart;
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "delta=(%.6f, %.6f, %.6f), start=(%.4f, %.4f, %.4f), end=(%.4f, %.4f, %.4f)",
                      static_cast<double>(delta.x), static_cast<double>(delta.y),
                      static_cast<double>(delta.z), static_cast<double>(g_st.phaseFStart.x),
                      static_cast<double>(g_st.phaseFStart.y), static_cast<double>(g_st.phaseFStart.z),
                      static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
        check(o && std::fabs(delta.x) <= 1e-5f, "F1 Shift+X stays in YZ plane", buf);
        check(o && std::fabs(delta.y) + std::fabs(delta.z) > 0.05f,
              "F2 plane drag produces in-plane motion", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 4", undo.doneCount());
        check(undo.doneCount() == 4, "F3 plane gesture is one undo entry", buf);
    }

    // ---- phase G: Cursor3D changes the modal rotation center -----------------
    if (frame == 116) {
        const scene::Object* o = world.get(g_st.cube);
        g_st.phaseGStart = o ? o->transform.position : Vec3{};
        world.setCursor(Vec3{0.0f, 0.0f, 0.0f});
        pivotMode = PivotMode::Cursor3D;
        EMBER_LOG_INFO("[selftest] cursor pivot: cursor=(0,0,0), object=(%.4f, %.4f, %.4f)",
                       static_cast<double>(g_st.phaseGStart.x),
                       static_cast<double>(g_st.phaseGStart.y),
                       static_cast<double>(g_st.phaseGStart.z));
    }
    tap(io, frame, 118, ImGuiKey_R);
    tap(io, frame, 120, ImGuiKey_Z);
    tap(io, frame, 122, ImGuiKey_9);
    tap(io, frame, 124, ImGuiKey_0);
    tap(io, frame, 126, ImGuiKey_Enter);
    if (frame == 130) {
        const scene::Object* o = world.get(g_st.cube);
        const Vec3 p = o ? o->transform.position : Vec3{};
        const Vec3 expected{-g_st.phaseGStart.y, g_st.phaseGStart.x, g_st.phaseGStart.z};
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "start=(%.4f,%.4f,%.4f), expected=(%.4f,%.4f,%.4f), got=(%.4f,%.4f,%.4f)",
                      static_cast<double>(g_st.phaseGStart.x), static_cast<double>(g_st.phaseGStart.y),
                      static_cast<double>(g_st.phaseGStart.z), static_cast<double>(expected.x),
                      static_cast<double>(expected.y), static_cast<double>(expected.z),
                      static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
        check(o && near(p.x, expected.x, 1e-4f) && near(p.y, expected.y, 1e-4f) &&
                  near(p.z, expected.z, 1e-4f),
              "G1 Cursor3D pivots modal rotation around scene cursor", buf);
        std::snprintf(buf, sizeof buf, "doneCount=%zu expected 5", undo.doneCount());
        check(undo.doneCount() == 5, "G2 cursor-pivot gesture is one undo entry", buf);
    }
    if (frame == 132) pivotMode = PivotMode::MedianPoint;

    // ---- summary -------------------------------------------------------------
    if (frame == 59) {
        // Determinism probe: the scale the modal's axis path uses this frame.
        float px = 0.0f, py = 0.0f, tx = 0.0f, ty = 0.0f;
        const Vec3 pivot{2.0f, 0.0f, 0.0f};
        const int w = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x);
        const int h = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y);
        if (camera.projectScreen(pivot, w, h, px, py) &&
            camera.projectScreen(pivot + Vec3{1.0f, 0.0f, 0.0f}, w, h, tx, ty)) {
            const float ppu = std::hypot(tx - px, ty - py);
            EMBER_LOG_INFO("[selftest] probe: fb=%dx%d pivotPx=(%.1f,%.1f) pxPerUnit=%.3f mouse=(%.1f,%.1f)",
                           w, h, static_cast<double>(px), static_cast<double>(py),
                           static_cast<double>(ppu), static_cast<double>(mx), static_cast<double>(my));
        } else {
            EMBER_LOG_WARN("[selftest] probe: pivot did not project (fb=%dx%d)", w, h);
        }
    }
    if (frame == 136) {
        EMBER_LOG_INFO("[selftest] %s: %d/%d checks passed", g_st.failed == 0 ? "PASS" : "FAIL",
                       g_st.checks - g_st.failed, g_st.checks);
    }
}

int runTransformSelfTest(scene::World& world, UndoStack& undo,
                         const render::OrbitCamera& camera, InteractionMode& interaction,
                         SelectMode& selectMode, PivotMode& pivotMode, int frame) {
    runTransformSelfTestImpl(world, undo, camera, interaction, selectMode, pivotMode, frame);
    if (frame < 136) return 0;
    return g_st.failed == 0 ? 1 : -1;
}

} // namespace ember::editor
