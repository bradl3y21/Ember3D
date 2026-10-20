// tests/test_editor_pie.cpp - owned by Verity / QA. Item 154 (the Z shading pie).
//
// WHAT-REMAINS.md section 4: "Pie menus for the things Blender uses them for: Z for
// shading mode." This drives the REAL handleShadingPie() / PieState through ImGui's event
// queue in a headless, CPU-only context (the function draws only to the foreground draw
// list), so no window and no GL are needed.
//
// THE SLOT MAPPING IS BLENDER'S, and it is cited rather than remembered:
//   scripts/startup/bl_ui/space_view3d.py, class VIEW3D_MT_shading_pie:
//       pie = layout.menu_pie()
//       pie.prop(view.shading, "type", expand=True)
//   i.e. the shading-type enum in RNA order (WIREFRAME, SOLID, MATERIAL, RENDERED) laid out
//   in Blender's pie order (first Left, second Right, third Bottom, fourth Top):
//       LEFT = Wireframe   RIGHT = Solid   BOTTOM = Material Preview   TOP = Rendered
//   (https://raw.githubusercontent.com/blender/blender/main/scripts/startup/bl_ui/space_view3d.py)
// NOTE: the item brief (Chisel's question) listed a different arrangement (Solid top,
// Material right, Rendered bottom, Wireframe left); it contradicts both Blender and Chisel's
// own implementation, so the brief is wrong and this file follows Blender.
//
// Contract asserted: Z opens at the cursor; hover picks the slice under the pointer; a
// held-Z release confirms the hovered slice; a tap leaves the pie open for click-to-confirm;
// arrow keys + Enter select with no mouse; Esc / right-click cancel and leave the shading
// untouched; and the function reports that it OWNS the input for as long as the pie is up
// (Editor::draw uses that to keep the viewport pick, gizmo and context menu quiet).
#include "harness.h"

#include "core/Types.h"
#include "editor/Panels.h"

#include <imgui.h>

#include <cstdio>
#include <string>

using ember::ShadingMode;
using ember::editor::PieState;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kW = 1280, kH = 720;
constexpr float kCx = 640.0f, kCy = 360.0f;

const char* name(ShadingMode m) {
    switch (m) {
        case ShadingMode::Wireframe: return "Wireframe";
        case ShadingMode::Solid: return "Solid";
        case ShadingMode::Material: return "Material Preview";
        case ShadingMode::Rendered: return "Rendered";
    }
    return "?";
}

// Blender's slots, as pointer offsets from the pie centre (screen y grows DOWNWARD).
struct Dir { const char* label; float dx, dy; int slot; ShadingMode mode; ImGuiKey arrow; };
const Dir kDirs[4] = {
    {"LEFT", -55.0f, 0.0f, 0, ShadingMode::Wireframe, ImGuiKey_LeftArrow},
    {"RIGHT", 55.0f, 0.0f, 1, ShadingMode::Solid, ImGuiKey_RightArrow},
    {"BOTTOM", 0.0f, 55.0f, 2, ShadingMode::Material, ImGuiKey_DownArrow},
    {"TOP", 0.0f, -55.0f, 3, ShadingMode::Rendered, ImGuiKey_UpArrow},
};

struct Rig {
    ImGuiContext* ctx = nullptr;
    ember::ViewportSettings viewport{};
    PieState pie{};
    bool lastOwned = false;

    Rig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(kW), float(kH));
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;   // key + mouse events apply in the frame they are queued
        unsigned char* px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        viewport.shading = ShadingMode::Solid;
    }
    ~Rig() { ImGui::DestroyContext(ctx); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // One frame. key: ImGuiKey_None = no key event. left/right: -1 unchanged, 0 up, 1 down.
    bool frame(float x, float y, ImGuiKey key = ImGuiKey_None, bool keyDown = false, int left = -1, int right = -1,
               bool ctrl = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, keyDown);
        if (left >= 0) io.AddMouseButtonEvent(0, left != 0);
        if (right >= 0) io.AddMouseButtonEvent(1, right != 0);
        ImGui::NewFrame();
        lastOwned = ember::editor::handleShadingPie(viewport, pie);
        ImGui::EndFrame();
        return lastOwned;
    }
    // Z pressed (down) at (x,y): the opening frame.
    bool zDown(float x, float y) { return frame(x, y, ImGuiKey_Z, true); }
    bool zUp(float x, float y) { return frame(x, y, ImGuiKey_Z, false); }
    // A tap: down then up with the pointer not moving.
    void tapZ(float x, float y) { zDown(x, y); zUp(x, y); }
    void press(float x, float y, ImGuiKey k) { frame(x, y, k, true); frame(x, y, k, false); }
};

// A start mode guaranteed to differ from `target`, so "it changed" is observable.
ShadingMode otherThan(ShadingMode target) { return target == ShadingMode::Material ? ShadingMode::Solid : ShadingMode::Material; }

} // namespace

TEST(pie_z_opens_the_pie_at_the_cursor_and_owns_input) {
    Rig r;
    CHECK_MSG(!r.pie.open, "pie open before any key");
    CHECK_MSG(!r.frame(500, 300), "an idle frame with the pie closed must not claim input");
    const bool owned = r.zDown(500.0f, 300.0f);
    CHECK_MSG(r.pie.open, "Z did not open the pie");
    CHECK_MSG(owned, "the opening frame must report that the pie owns input");
    CHECK_MSG(r.pie.center.x == 500.0f && r.pie.center.y == 300.0f, "pie centred at (%g,%g), expected the cursor (500,300)",
              double(r.pie.center.x), double(r.pie.center.y));
    CHECK_MSG(r.pie.openedByZ, "a held Z must be recorded (openedByZ)");
    CHECK_MSG(r.pie.hoveredSlice == -1, "pointer on the centre must hover no slice, got %d", r.pie.hoveredSlice);
}

TEST(pie_ctrl_z_is_undo_not_the_pie) {
    Rig r;
    const bool owned = r.frame(500, 300, ImGuiKey_Z, true, -1, -1, /*ctrl*/ true);
    CHECK_MSG(!r.pie.open, "Ctrl+Z opened the shading pie");
    CHECK_MSG(!owned, "Ctrl+Z must not be claimed by the pie");
}

TEST(pie_four_directions_map_to_blenders_slots_hover_then_click) {
    for (const Dir& d : kDirs) {
        Rig r;
        r.viewport.shading = otherThan(d.mode);
        r.tapZ(kCx, kCy);
        CHECK_MSG(r.pie.open, "%s: a tap must leave the pie open", d.label);
        r.frame(kCx + d.dx, kCy + d.dy);
        CHECK_MSG(r.pie.hoveredSlice == d.slot, "%s: hovered slice %d, expected %d", d.label, r.pie.hoveredSlice, d.slot);
        CHECK_MSG(r.pie.open, "%s: hovering alone must not confirm", d.label);
        r.frame(kCx + d.dx, kCy + d.dy, ImGuiKey_None, false, /*left down*/ 1);
        CHECK_MSG(!r.pie.open, "%s: a left click on the slice must close the pie", d.label);
        CHECK_MSG(r.viewport.shading == d.mode, "%s: shading is %s, expected %s (Blender's slot)", d.label,
                  name(r.viewport.shading), name(d.mode));
    }
}

TEST(pie_hover_follows_the_pointer_including_diagonals_and_the_dead_zone) {
    Rig r;
    r.tapZ(kCx, kCy);
    struct Probe { float dx, dy; int expect; const char* what; };
    const Probe probes[] = {
        {4, 3, -1, "inside the dead zone"},
        {60, 20, 1, "right, a little below the axis"},
        {-60, -25, 0, "left, a little above the axis"},
        {20, 60, 2, "bottom, a little right of the axis"},
        {-20, -60, 3, "top, a little left of the axis"},
        {0, 300, -1, "far outside the pie"},
    };
    for (const Probe& p : probes) {
        r.frame(kCx + p.dx, kCy + p.dy);
        CHECK_MSG(r.pie.hoveredSlice == p.expect, "%s (%.0f,%.0f): hovered %d, expected %d", p.what, double(p.dx), double(p.dy),
                  r.pie.hoveredSlice, p.expect);
        CHECK_MSG(r.pie.open, "%s: the pie closed by itself", p.what);
    }
}

TEST(pie_held_z_released_over_a_slice_confirms_it) {
    for (const Dir& d : kDirs) {
        Rig r;
        r.viewport.shading = otherThan(d.mode);
        r.zDown(kCx, kCy);                                  // Z stays held from here
        r.frame(kCx + d.dx, kCy + d.dy);
        r.frame(kCx + d.dx, kCy + d.dy);
        CHECK_MSG(r.pie.open && r.viewport.shading != d.mode, "%s: must not confirm while Z is still held", d.label);
        r.zUp(kCx + d.dx, kCy + d.dy);
        CHECK_MSG(!r.pie.open, "%s: releasing Z over the slice must close the pie", d.label);
        CHECK_MSG(r.viewport.shading == d.mode, "%s: shading is %s, expected %s", d.label, name(r.viewport.shading), name(d.mode));
    }
}

TEST(pie_tap_z_keeps_the_pie_open_for_a_click) {
    Rig r;
    r.viewport.shading = ShadingMode::Material;
    r.tapZ(kCx, kCy);
    for (int i = 0; i < 10; ++i) r.frame(kCx, kCy);
    CHECK_MSG(r.pie.open, "the pie closed on its own after a tap");
    CHECK_MSG(r.viewport.shading == ShadingMode::Material, "a tap with no selection changed the shading to %s", name(r.viewport.shading));
    CHECK_MSG(!r.pie.openedByZ, "after a tap the pie must no longer be in hold-to-confirm mode");
    r.frame(kCx - 55, kCy);
    r.zDown(kCx - 55, kCy);   // pressing Z AGAIN while open must not re-centre or confirm by release
    CHECK_MSG(r.pie.open, "pressing Z again while the pie is open closed it");
}

TEST(pie_left_click_in_the_dead_zone_does_not_change_the_shading) {
    Rig r;
    r.viewport.shading = ShadingMode::Material;
    r.tapZ(kCx, kCy);
    r.frame(kCx + 3, kCy + 3, ImGuiKey_None, false, 1);
    CHECK_MSG(r.viewport.shading == ShadingMode::Material, "a click on no slice set the shading to %s", name(r.viewport.shading));
}

TEST(pie_arrow_keys_and_enter_select_without_the_mouse) {
    for (const Dir& d : kDirs) {
        Rig r;
        r.viewport.shading = otherThan(d.mode);
        r.tapZ(kCx, kCy);
        r.press(kCx, kCy, d.arrow);                          // the pointer never moves
        CHECK_MSG(r.pie.keyboardSlice == d.slot, "%s arrow: keyboardSlice %d, expected %d", d.label, r.pie.keyboardSlice, d.slot);
        CHECK_MSG(r.pie.open, "%s arrow alone must not confirm", d.label);
        r.press(kCx, kCy, ImGuiKey_Enter);
        CHECK_MSG(!r.pie.open, "%s: Enter must close the pie", d.label);
        CHECK_MSG(r.viewport.shading == d.mode, "%s arrow + Enter: shading %s, expected %s", d.label, name(r.viewport.shading),
                  name(d.mode));
    }
}

TEST(pie_escape_and_right_click_cancel_and_leave_the_shading_alone) {
    for (int how = 0; how < 2; ++how) {
        Rig r;
        r.viewport.shading = ShadingMode::Material;
        r.tapZ(kCx, kCy);
        r.frame(kCx + 55, kCy);                              // hovering Solid
        CHECK_MSG(r.pie.hoveredSlice == 1, "setup: not hovering the right slice");
        if (how == 0) r.press(kCx + 55, kCy, ImGuiKey_Escape);
        else r.frame(kCx + 55, kCy, ImGuiKey_None, false, -1, /*right down*/ 1);
        const char* what = how == 0 ? "Escape" : "right-click";
        CHECK_MSG(!r.pie.open, "%s did not close the pie", what);
        CHECK_MSG(r.viewport.shading == ShadingMode::Material, "%s changed the shading to %s", what, name(r.viewport.shading));
        CHECK_MSG(r.pie.hoveredSlice == -1 && r.pie.keyboardSlice == -1, "%s left stale hover/keyboard state", what);
    }
}

TEST(pie_reports_it_owns_the_input_on_every_frame_it_is_up) {
    // Editor::draw skips the viewport pick, gizmo and context menu while handleShadingPie()
    // returns true. So it must be true on: opening, hover, idle, confirm and cancel frames.
    Rig r;
    CHECK_MSG(r.zDown(kCx, kCy), "opening frame not owned");
    CHECK_MSG(r.zUp(kCx, kCy), "tap-release frame not owned");
    CHECK_MSG(r.frame(kCx + 55, kCy), "hover frame not owned");
    CHECK_MSG(r.frame(kCx + 55, kCy), "idle frame not owned");
    CHECK_MSG(r.frame(kCx + 55, kCy, ImGuiKey_None, false, 1), "confirming click frame not owned (the viewport would also pick)");
    CHECK_MSG(!r.pie.open, "pie should have closed on the click");
    // (info) the following RELEASE of that same click happens after the pie closed.
    const bool releaseOwned = r.frame(kCx + 55, kCy, ImGuiKey_None, false, 0);
    std::printf("       (info) the mouse-RELEASE frame after a click-confirm is %s by the pie\n", releaseOwned ? "owned" : "NOT owned");
    CHECK_MSG(!r.frame(kCx, kCy), "a closed pie must not claim later idle frames");

    Rig c;
    c.tapZ(kCx, kCy);
    const bool cancelOwned = c.frame(kCx, kCy, ImGuiKey_Escape, true);
    CHECK_MSG(cancelOwned, "the Escape (cancel) frame must be owned, or the Esc key would also reach the viewport");
    CHECK_MSG(!c.pie.open, "Escape did not close the pie");
}

HARNESS_MAIN()
