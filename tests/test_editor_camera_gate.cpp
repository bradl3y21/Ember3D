// tests/test_editor_camera_gate.cpp - owned by Verity / QA. Item 166.
//
// THE BUG THIS PINS: for a whole wave the camera could not be moved at all - no orbit, no pan, no
// wheel, no numpad views - because Editor::wantsViewportFocus() returned !WantCaptureMouse, and
// WantCaptureMouse is TRUE over the docked 3D viewport window as well as over every panel. Nothing
// caught it: the layout suite asserts WHERE the panels are, never that the camera RESPONDS TO THE
// POINTER. The camera gate is a contract about the pointer, so the test puts the pointer places:
//
//   viewport BODY ........................... wantsViewportFocus() == true
//   viewport Header strip ................... false   (it is not the body)
//   Outliner / Properties / Timeline ........ false
//   Topbar / Status bar ..................... false
//   a popup open (pointer anywhere) ......... false   (the popup owns the mouse)
//   popup closed again, pointer in the body . true    (so the false above was the popup, not stale)
//
// It drives the REAL Editor (init / draw / viewportRect / wantsViewportFocus) in a hidden GLFW
// window - the layout is the real dock layout, the popup is a real ImGui combo popup, the pointer is
// queued through ImGui's event queue. Editor needs a GL context and a GLFW window, so this is
// GPU-gated: registered only under EMBER_ENABLE_GPU_TESTS and run with --gpu.
//
// MUST FAIL AGAINST THE OLD PREDICATE: the same contract is evaluated against a re-creation of the
// old one, `!ImGui::GetIO().WantCaptureMouse`, and the test asserts that it VIOLATES the contract
// (and reports where). So the test cannot pass on a predicate that has the bug.
#include "harness.h"

#include <glad/gl.h>

#include "editor/Editor.h"
#include "editor/Undo.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

struct Rig {
    ember::platform::Window window;
    ember::render::Renderer renderer;
    ember::editor::Editor editor;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    bool ok = false;
    std::string why;

    Rig() {
        ember::platform::WindowConfig wc;
        wc.width = 1280; wc.height = 720; wc.visible = false; wc.title = "camera-gate";
        if (!window.create(wc)) { why = "no GL window"; return; }
        if (!renderer.init()) { why = "renderer init failed"; return; }
        renderer.resize(1280, 720);
        if (!editor.init(window.nativeHandle())) { why = "editor init failed"; return; }
        world.createPrimitive(ember::scene::Primitive::Cube, "cube");
        for (int i = 0; i < 6; ++i) draw();       // build the dock layout
        ok = editor.viewportRect().w > 0;
        if (!ok) why = "the editor never produced a viewport rect";
    }
    ~Rig() { editor.shutdown(); renderer.shutdown(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void draw() { editor.draw(world, renderer, renderer.lastStats(), undo); }

    ImVec2 scale() const { return ImGui::GetIO().DisplayFramebufferScale; }

    // The viewport BODY in ImGui (point) coordinates.
    ImVec4 body() const {
        const ember::Rect r = editor.viewportRect();
        const ImVec2 s = scale();
        return ImVec4(float(r.x) / s.x, float(r.y) / s.y, float(r.w) / s.x, float(r.h) / s.y);
    }

    // Move the pointer and run two frames so ImGui's hover state (WantCaptureMouse) settles.
    // Returns false if the pointer is not where we put it (a backend overriding it would make
    // every later assertion meaningless, so that is reported as a setup failure).
    bool pointerAt(float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        draw();
        draw();
        const ImVec2 p = ImGui::GetIO().MousePos;
        return std::fabs(p.x - x) < 1.0f && std::fabs(p.y - y) < 1.0f;
    }

    void click(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y); draw();
        io.AddMouseButtonEvent(0, true); draw();
        io.AddMouseButtonEvent(0, false); draw();
        draw();
    }
};

struct Probe { const char* name; float x, y; bool expect; };

// One pointer position, evaluated by `predicate`. Returns true when it agrees with `expect`.
int runContract(Rig& r, const std::function<bool()>& predicate, bool print, const char* who) {
    const ImVec4 b = r.body();
    const float dispW = ImGui::GetIO().DisplaySize.x, dispH = ImGui::GetIO().DisplaySize.y;
    // Geometry is DERIVED from the viewport body rect the editor itself reports, so the probes follow the
    // layout rather than hard-coding it. Right column = to the right of the body; Timeline = below it.
    const float bx = b.x, by = b.y, bw = b.z, bh = b.w;
    const std::vector<Probe> probes = {
        {"viewport BODY (centre-left, clear of the Toolbar and Sidebar overlays)", bx + bw * 0.45f, by + bh * 0.55f, true},
        {"viewport BODY (a second point, upper middle)", bx + bw * 0.30f, by + bh * 0.20f, true},
        {"viewport Header strip (above the body)", bx + bw * 0.45f, by - 16.0f, false},
        {"Outliner (right column, top)", bx + bw + 60.0f, by + 30.0f, false},
        {"Properties (right column, bottom)", bx + bw + 60.0f, by + bh - 50.0f, false},
        {"Timeline (below the viewport)", bx + bw * 0.40f, by + bh + 60.0f, false},
        {"Topbar", 400.0f, 6.0f, false},
        {"Status bar", 300.0f, dispH - 8.0f, false},
    };
    int violations = 0;
    for (const Probe& p : probes) {
        if (p.x < 0 || p.y < 0 || p.x >= dispW || p.y >= dispH) {
            CHECK_MSG(false, "[%s] probe '%s' at (%.0f,%.0f) is outside the %.0fx%.0f window - the layout assumption broke", who, p.name,
                      double(p.x), double(p.y), double(dispW), double(dispH));
            ++violations;
            continue;
        }
        const bool placed = r.pointerAt(p.x, p.y);
        CHECK_MSG(placed, "[%s] could not place the pointer at (%.0f,%.0f): ImGui reports (%.0f,%.0f)", who, double(p.x), double(p.y),
                  double(ImGui::GetIO().MousePos.x), double(ImGui::GetIO().MousePos.y));
        const bool got = predicate();
        if (print)
            std::printf("       [%s] pointer over %-66s (%4.0f,%4.0f): %s  (expect %s)  WantCaptureMouse=%d\n", who, p.name, double(p.x), double(p.y),
                        got ? "TRUE " : "false", p.expect ? "TRUE " : "false", ImGui::GetIO().WantCaptureMouse ? 1 : 0);
        if (got != p.expect) {
            ++violations;
            CHECK_MSG(false, "[%s] pointer over %s (%.0f,%.0f): wantsViewportFocus %s, the camera gate must be %s", who, p.name, double(p.x),
                      double(p.y), got ? "TRUE" : "false", p.expect ? "TRUE" : "false");
        } else {
            ++harness::g_checks;
        }
    }
    return violations;
}

} // namespace

TEST_GPU(camera_gate_follows_the_pointer_over_body_header_panels_and_bars) {
    Rig r;
    CHECK_MSG(r.ok, "rig: %s", r.why.c_str());
    if (!r.ok) return;
    const ImVec4 b = r.body();
    std::printf("       viewport body (ImGui points): x=%.0f y=%.0f w=%.0f h=%.0f  display %.0fx%.0f  fb scale %.2f\n", double(b.x), double(b.y),
                double(b.z), double(b.w), double(ImGui::GetIO().DisplaySize.x), double(ImGui::GetIO().DisplaySize.y), double(r.scale().x));
    const int v = runContract(r, [&] { return r.editor.wantsViewportFocus(); }, true, "REAL Editor::wantsViewportFocus");
    CHECK_MSG(v == 0, "%d contract violation(s) by the real predicate", v);
}

TEST_GPU(camera_gate_is_false_while_a_popup_is_open_and_true_again_when_it_closes) {
    Rig r;
    CHECK_MSG(r.ok, "rig: %s", r.why.c_str());
    if (!r.ok) return;
    const ImVec4 b = r.body();
    const float bx = b.x, by = b.y, bw = b.z, bh = b.w;
    const ImVec2 inBody(bx + bw * 0.30f, by + bh * 0.25f);
    CHECK_MSG(r.pointerAt(inBody.x, inBody.y) && r.editor.wantsViewportFocus(), "precondition: with no popup the body must grant the gate");

    // Open a REAL popup: click the 3D Viewport header's editor-type combo. The combo sits at
    // the Area D window's LEFT edge, which is no longer the body's left edge: since item 181
    // the Toolbar occupies the first toolbarRegionWidth() px of the body, so a click at
    // (body.x + 40) lands on the header strip PAST the combo and opens nothing.
    ImGuiWindow* areaD = ImGui::FindWindowByName("Area D");
    CHECK_MSG(areaD != nullptr, "setup: the Area D window was not found, so the header combo cannot be located");
    const float comboX = (areaD ? areaD->Pos.x : bx) + 40.0f;
    std::printf("       header combo click at (%.0f, %.0f): Area D left edge %.0f, body left edge %.0f\n",
                double(comboX), double(by - 16.0f), areaD ? double(areaD->Pos.x) : -1.0, double(bx));
    r.click(comboX, by - 16.0f);
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    CHECK_MSG(popupOpen, "setup: clicking the header's editor-type combo did not open a popup, so the popup cases below prove nothing");
    if (!popupOpen) return;

    // Pointer returned to the body (and to a far point of it, away from the popup's own window).
    const ImVec2 spots[] = {inBody, ImVec2(bx + bw * 0.20f, by + bh * 0.80f), ImVec2(bx + bw * 0.45f, by + bh * 0.55f)};
    for (const ImVec2& s : spots) {
        CHECK_MSG(r.pointerAt(s.x, s.y), "could not place the pointer");
        const bool got = r.editor.wantsViewportFocus();
        std::printf("       popup OPEN, pointer in the viewport body at (%.0f,%.0f): wantsViewportFocus %s (must be false)\n", double(s.x), double(s.y),
                    got ? "TRUE" : "false");
        CHECK_MSG(!got, "a popup is open and the pointer is in the viewport body (%.0f,%.0f), but the camera gate is TRUE: a popup must own the mouse", double(s.x), double(s.y));
    }

    // Close it (Escape) and the same body point must grant the gate again: the false above was the popup.
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Escape, true); r.draw();
    io.AddKeyEvent(ImGuiKey_Escape, false); r.draw();
    r.draw();
    const bool stillOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    CHECK_MSG(!stillOpen, "setup: Escape did not close the popup");
    CHECK_MSG(r.pointerAt(inBody.x, inBody.y), "could not place the pointer");
    CHECK_MSG(r.editor.wantsViewportFocus(), "the popup is closed and the pointer is in the viewport body, but the camera gate is still false");
}

TEST_GPU(regions_toolbar_and_sidebar_do_not_pass_the_camera_gate) {
    // The Toolbar (T) and Sidebar (N) are regions INSIDE the viewport area. In Blender a region consumes
    // its own input: the wheel over the Sidebar scrolls the Sidebar, a drag on it is not an orbit. If the
    // camera gate is TRUE over them, scrolling or dragging a Sidebar panel also zooms/orbits the camera.
    Rig r;
    CHECK_MSG(r.ok, "rig: %s", r.why.c_str());
    if (!r.ok) return;
    const ImVec4 b = r.body();
    struct Spot { const char* name; float x, y; };
    const Spot spots[] = {
        // editor.viewportRect() is the renderer BODY only. The Toolbar abuts
        // its left edge; the Sidebar abuts its right edge. Probe those Regions,
        // not offsets inside the body (the earlier setup mislabeled body pixels).
        {"Toolbar (left of renderer body)", b.x - ember::editor::toolbarRegionWidth() * 0.5f,
         b.y + 30.0f},
        {"Sidebar (right of renderer body)", b.x + b.z + ember::editor::sidebarRegionWidth() * 0.5f,
         b.y + 90.0f},
    };
    for (const Spot& s : spots) {
        CHECK_MSG(r.pointerAt(s.x, s.y), "could not place the pointer");
        ImGuiContext* g = ImGui::GetCurrentContext();
        const char* hovered = g->HoveredWindow ? g->HoveredWindow->Name : "(none)";
        const bool got = r.editor.wantsViewportFocus();
        std::printf("       pointer over %-48s (%4.0f,%4.0f): hovered window '%s' -> wantsViewportFocus %s\n", s.name, double(s.x), double(s.y), hovered,
                    got ? "TRUE" : "false");
        CHECK_MSG(!got, "pointer over the %s: the camera gate is TRUE, so wheel/drag on that region also moves the camera (hovered window '%s')", s.name, hovered);
    }
}
TEST_GPU(pick_a_left_click_in_the_viewport_body_selects_the_object_under_the_pointer) {
    // Click the actual rendered viewport center, where the cube at the camera target appears.
    // The click router must use region ownership rather than WantCaptureMouse (true for Area D),
    // and the pick ray must be local to the viewport framebuffer rect, not the full window.
    Rig r;
    CHECK_MSG(r.ok, "rig: %s", r.why.c_str());
    if (!r.ok) return;
    r.world.clearSelection();
    CHECK_MSG(r.world.selection().empty(), "setup: selection did not clear");
    const ImVec4 b = r.body();
    const ImVec2 centre(b.x + b.z * 0.5f, b.y + b.w * 0.5f);
    const ember::Rect viewport = r.editor.viewportRect();
    CHECK_MSG(centre.x > b.x && centre.x < b.x + b.z && centre.y > b.y && centre.y < b.y + b.w,
              "setup: the rendered object center is not inside the viewport body");
    CHECK_MSG(viewport.valid(), "setup: viewport framebuffer rect is invalid");
    // Rule out a geometry miss with the renderer's actual viewport-local projection.
    {
        const auto ray = r.renderer.camera().screenToRay(viewport.w / 2, viewport.h / 2,
                                                         viewport.w, viewport.h);
        const auto hit = r.world.pick(ray);
        CHECK_MSG(hit.has_value(), "setup: the viewport-center ray misses the rendered cube");
        if (!hit) return;
    }
    CHECK_MSG(r.pointerAt(centre.x, centre.y), "could not place the pointer");
    std::printf("       before the click: WantCaptureMouse=%d, selection size %zu\n", ImGui::GetIO().WantCaptureMouse ? 1 : 0, r.world.selection().size());
    r.click(centre.x, centre.y);
    std::printf("       after a left click at the rendered viewport center (%.0f, %.0f): selection size %zu\n",
                double(centre.x), double(centre.y), r.world.selection().size());
    CHECK_MSG(!r.world.selection().empty(),
              "clicking the rendered cube selected nothing: check viewport input ownership and viewport-local pick-ray coordinates");
    CHECK_MSG(r.world.selection().size() == 1 &&
                  r.world.selection().back() == r.world.objects().front().id,
              "the click selected an object other than the cube under the pointer");
}
TEST_GPU(the_OLD_predicate_not_WantCaptureMouse_violates_this_contract) {
    // THE REGRESSION CHECK. The old Editor::wantsViewportFocus() was `!WantCaptureMouse`. Run the same
    // contract against that predicate: it MUST be caught. If this ever passes with zero violations the
    // contract above has stopped being able to see the bug.
    Rig r;
    CHECK_MSG(r.ok, "rig: %s", r.why.c_str());
    if (!r.ok) return;
    const int before = harness::g_failures;
    const int v = runContract(r, [] { return !ImGui::GetIO().WantCaptureMouse; }, true, "OLD !WantCaptureMouse");
    const int newFailures = harness::g_failures - before;
    // The violations are EXPECTED here: undo them from the test's bookkeeping so only the meta-assertion counts.
    harness::g_failures = before;
    harness::g_currentFailed = false;
    std::printf("       the old predicate violated the contract %d time(s)\n", v);
    CHECK_MSG(v >= 1 && newFailures >= 1, "the old predicate '!WantCaptureMouse' satisfied every probe: this test cannot detect the bug it exists for");
    // And specifically the bug that was shipped: the pointer in the viewport BODY must have been refused.
    const ImVec4 b = r.body();
    CHECK_MSG(r.pointerAt(b.x + b.z * 0.45f, b.y + b.w * 0.55f), "could not place the pointer");
    CHECK_MSG(ImGui::GetIO().WantCaptureMouse, "WantCaptureMouse is false over the viewport body here, so the old-bug precondition no longer holds; "
                                               "re-examine what the viewport window captures");
    CHECK_MSG(!(!ImGui::GetIO().WantCaptureMouse), "sanity: the old predicate must answer false over the viewport body (WantCaptureMouse is TRUE there)");
}

HARNESS_MAIN()
