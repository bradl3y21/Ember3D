// tests/test_editor_shading_entry_points.cpp — owned by Verity · QA. Item 215
// (verifying ROADMAP "Next, in order" #1; the pixel half is items 97/142).
//
// THE CLAIM UNDER TEST. The four viewport shading modes — Wireframe, Solid,
// Material Preview, Rendered — are each REACHABLE. ROADMAP #1 says two entries
// exist in source (the viewport header exposes all four; Pie.cpp defines four Z
// slices) but that no owner finding/receipt names them, so this item had to be
// driven rather than asserted from reading the code.
//
// A mode that is only WRITTEN in an array is not reachable. This file drives
// each of the four through the REAL editor code:
//   (A) the viewport header's shading combo — the real drawArea() (the function
//       Editor::drawArea calls), with real ImGui mouse events: click the combo,
//       find the four option rows in the popup it opened, click the row, and read
//       the ViewportSettings the combo wrote.
//   (B) the Z glow pie — the real handleShadingPie() (the function Editor::draw
//       calls), with real ImGui events: tap Z, hover a slice, click it.
// Both write the SAME ViewportSettings::shading, which is what the renderer
// consumes (Renderer::viewportSettings()).
//
// Blender's slot mapping, cited not remembered (the same source as test_editor_pie.cpp):
//   scripts/startup/bl_ui/space_view3d.py, VIEW3D_MT_shading_pie lays the shading
//   enum out in RNA order: LEFT = Wireframe, RIGHT = Solid, BOTTOM = Material
//   Preview, TOP = Rendered.
//   (https://raw.githubusercontent.com/blender/blender/main/scripts/startup/bl_ui/space_view3d.py)
//
// WHAT IS NOT CLAIMED (residue): these modes select the renderer's shading PATH;
// this file does not claim any two paths produce different PIXELS (that is items
// 97/142, and the startup scene has no mesh to draw). The header is driven through
// drawArea directly, not through App's event loop — but drawArea IS the function
// the live Editor::draw calls for that area (Editor.cpp:756), and the F3 command
// palette only offers Solid and Wireframe (Menu.cpp:451), which is recorded here
// rather than hidden.
//
// CPU-only: drawArea() and handleShadingPie() touch only the ImGui foreground
// draw list, so no window and no GL are needed. Runs in the default CTest run.
#include "harness.h"

#include "core/Types.h"
#include "editor/Panels.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiWindow internals: the header window's own id and rects

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// The REAL drawArea() and applyTheme() are inside Editor.cpp's anonymous namespace, so they cannot be
// linked from the ember_editor archive — this TU includes Editor.cpp to define them (exactly as item
// 175's header-width test does). The archive's Editor.cpp.obj is then not pulled, so no duplication.
#include "editor/Editor.cpp"

using ember::ShadingMode;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                          \
        ++harness::g_checks;                                                      \
        if (!(cond))                                                              \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr float kW = 2600.0f;   // wide enough that every header control fits (item 175's measured size)
constexpr float kH = 720.0f;

const char* name(ShadingMode m) {
    switch (m) {
        case ShadingMode::Wireframe: return "Wireframe";
        case ShadingMode::Solid:     return "Solid";
        case ShadingMode::Material:  return "Material Preview";
        case ShadingMode::Rendered:  return "Rendered";
    }
    return "?";
}

// The four entry points, in the order the header combo and the shading enum list them.
struct Shade { const char* label; ShadingMode mode; };
const Shade kShades[4] = {
    {"Wireframe", ShadingMode::Wireframe},
    {"Solid", ShadingMode::Solid},
    {"Material Preview", ShadingMode::Material},
    {"Rendered", ShadingMode::Rendered},
};

// ---- (A) the viewport header combo, drawn by the real drawArea() -------------------------
struct HeaderRig {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::ViewportSettings viewport{};
    ImGuiID lastHovered = 0;

    HeaderRig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // drawArea opens a docked panel
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(kW, kH);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;   // the frame's events apply in that frame
        unsigned char* px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        viewport.shading = ShadingMode::Solid;
    }
    ~HeaderRig() { ImGui::DestroyContext(ctx); }
    HeaderRig(const HeaderRig&) = delete;
    HeaderRig& operator=(const HeaderRig&) = delete;

    void frame(float mx, float my) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(kW, 300.0f));
        ember::RenderStats stats{};
        ember::editor::TimelineState timeline{};
        ember::editor::TransformSpace space = ember::editor::TransformSpace::Global;
        ember::SelectMode selectMode = ember::SelectMode::Vertex;
        ember::editor::PropertiesTab tab = ember::editor::PropertiesTab::Object;
        bool snap = false;
        ember::editor::SnapTarget snapTarget = ember::editor::SnapTarget::GridIncrement;
        ember::editor::AreaType type = ember::editor::AreaType::Viewport;
        ember::editor::drawArea("hdr", type, true, world, undo, stats, timeline, ember::Vec3{0, 0, 0}, viewport,
                 space, ember::InteractionMode::Object, selectMode, tab, snap, snapTarget);
        lastHovered = ImGui::GetCurrentContext()->HoveredId;   // EndFrame clears it; read now
        ImGui::EndFrame();
    }

    void click(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);          frame(x, y);
        io.AddMouseButtonEvent(0, true);    frame(x, y);
        io.AddMouseButtonEvent(0, false);   frame(x, y); frame(x, y);
    }

    ImGuiWindow* header() const { return ImGui::FindWindowByName("hdr"); }

    // The combo's true x-range: hover each pixel column and record where ImGui reports ITS id.
    bool findComboX(float y, float& outX0, float& outX1) {
        ImGuiWindow* win = header();
        if (!win) return false;
        const ImGuiID want = win->GetID("##shade");
        outX0 = 1e9f; outX1 = -1e9f;
        for (int x = 0; x < int(kW); ++x) {
            frame(float(x) + 0.5f, y);
            if (lastHovered == want) { outX0 = std::fmin(outX0, float(x)); outX1 = std::fmax(outX1, float(x)); }
        }
        return outX1 >= outX0;
    }

    // The rows of the option list the combo opened, as (y0,y1) spans, top to bottom. Each row is
    // found by hovering it — the popup reports the row's id as hovered, exactly as for a real pointer.
    struct Row { ImGuiID id; float y0, y1; };
    bool popupRows(float& outX, std::vector<Row>& rows) {
        ImGuiWindow* pop = ImGui::FindWindowByName("##Combo_00");
        if (!pop || !pop->Active) return false;   // a closed popup lingers in the window list
        outX = pop->Pos.x + pop->Size.x * 0.5f;
        const float top = pop->Pos.y + 2.0f;
        const float bot = pop->Pos.y + pop->Size.y - 2.0f;
        ImGuiID cur = 0; float start = top;
        for (float y = top; y <= bot; y += 1.0f) {
            frame(outX, y);
            if (lastHovered != cur) {
                if (cur != 0) rows.push_back({cur, start, y - 1.0f});
                cur = lastHovered;
                start = y;
            }
        }
        if (cur != 0) rows.push_back({cur, start, bot});
        return true;
    }
};

// ---- (B) the Z shading pie, driven by the real handleShadingPie() ------------------------
struct PieRig {
    ImGuiContext* ctx = nullptr;
    ember::ViewportSettings viewport{};
    ember::editor::PieState pie{};

    PieRig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 720);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        viewport.shading = ShadingMode::Solid;
    }
    ~PieRig() { ImGui::DestroyContext(ctx); }
    PieRig(const PieRig&) = delete;
    PieRig& operator=(const PieRig&) = delete;

    void frame(float x, float y, ImGuiKey key = ImGuiKey_None, bool keyDown = false, int left = -1) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, keyDown);
        if (left >= 0) io.AddMouseButtonEvent(0, left != 0);
        ImGui::NewFrame();
        ember::editor::handleShadingPie(viewport, pie);
        ImGui::EndFrame();
    }
};

} // namespace

// ============================================================================
// (A) the viewport header combo reaches all four shading modes
// ============================================================================
TEST(header_combo_reaches_all_four_shading_modes) {
    HeaderRig r;
    for (int i = 0; i < 4; ++i) r.frame(-10.0f, -10.0f);   // settle the docked area

    ImGuiWindow* win = r.header();
    CHECK_MSG(win != nullptr, "the real drawArea() did not create the viewport area window");
    if (!win) return;

    // The shading combo sits in the header strip, one line down from the window's content top
    // (the same y item 175 measures every header control at).
    const float y = win->Pos.y + win->WindowPadding.y + 6.0f;
    float x0 = 0, x1 = 0;
    const bool found = r.findComboX(y, x0, x1);
    CHECK_MSG(found, "the viewport header's shading combo ('##shade') was not drawn by the real drawArea()");
    if (!found) return;
    const float cx = 0.5f * (x0 + x1);
    std::printf("       [header] entry point '##shade' found at x=[%.0f..%.0f], clicked at %.0f\n",
                double(x0), double(x1), double(cx));

    for (const Shade& s : kShades) {
        r.click(cx, y);                                     // open the option list
        ImGuiWindow* pop = ImGui::FindWindowByName("##Combo_00");
        CHECK_MSG(pop != nullptr && pop->Active,
                  "[header] %s: clicking '##shade' did not open its option list", s.label);
        if (!pop || !pop->Active) return;
        float px = 0;
        std::vector<HeaderRig::Row> rows;
        CHECK_MSG(r.popupRows(px, rows), "[header] %s: the option list vanished while reading it", s.label);
        CHECK_MSG(rows.size() == 4, "[header] %s: the shading option list has %zu rows, expected the four modes",
                  s.label, rows.size());
        if (rows.size() != 4) return;

        const std::size_t idx = std::size_t(&s - kShades);
        r.click(px, 0.5f * (rows[idx].y0 + rows[idx].y1));  // click the row for this mode
        CHECK_MSG(r.viewport.shading == s.mode,
                  "[header] clicking row %zu ('%s') left viewport.shading = %s",
                  idx, s.label, name(r.viewport.shading));
        std::printf("       [header] row %zu '%s' -> viewport.shading == %s\n",
                    idx, s.label, name(r.viewport.shading));
    }
}

// ============================================================================
// (B) the Z pie reaches all four shading modes
// ============================================================================
TEST(pie_reaches_all_four_shading_modes) {
    // Blender's slots as pointer offsets from the pie centre (screen y grows downward).
    struct Dir { const char* label; float dx, dy; ShadingMode mode; };
    const Dir dirs[4] = {
        {"LEFT", -55.0f, 0.0f, ShadingMode::Wireframe},
        {"RIGHT", 55.0f, 0.0f, ShadingMode::Solid},
        {"BOTTOM", 0.0f, 55.0f, ShadingMode::Material},
        {"TOP", 0.0f, -55.0f, ShadingMode::Rendered},
    };
    const float cx = 640.0f, cy = 360.0f;

    for (const Dir& d : dirs) {
        PieRig r;
        // Start from another mode, so "it changed to THIS one" is observable.
        r.viewport.shading = d.mode == ShadingMode::Material ? ShadingMode::Solid : ShadingMode::Material;
        r.frame(cx, cy, ImGuiKey_Z, true);                  // tap Z: the pie opens, stays open
        r.frame(cx, cy, ImGuiKey_Z, false);
        r.frame(cx + d.dx, cy + d.dy);                      // hover the slice
        r.frame(cx + d.dx, cy + d.dy, ImGuiKey_None, false, 1);   // left click selects it
        CHECK_MSG(r.viewport.shading == d.mode,
                  "[pie] %s: selecting the %s slice left viewport.shading = %s",
                  d.label, name(d.mode), name(r.viewport.shading));
        std::printf("       [pie] %s slice -> viewport.shading == %s\n", d.label, name(r.viewport.shading));
    }
}

HARNESS_MAIN()
