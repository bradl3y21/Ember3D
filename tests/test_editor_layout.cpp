// tests/test_editor_layout.cpp â€” owned by Verity Â· QA. Item 135.
//
// Contract under test: the editor's fixed layout TILES the window â€” no gap, no
// overlap, nothing on top of anything â€” and the default layout is identical
// across two launches.
//
// WHAT "TILES" MEANS HERE, and why it is not "zero pixels between areas":
// Blender's own manual (docs.blender.org/manual/en/latest/interface/
// window_system/areas.html): "The Blender window is divided into a number of
// rectangles called Areas", resized "by dragging their borders", and "The size
// of the border around areas can be adjusted in the user preferences with
// Border Width". So Blender has a border between adjacent areas, and that
// border is the resize handle. The faithful contract is therefore:
//
//   every window pixel belongs to EXACTLY ONE of {menu bar, status bar,
//   Area A, Area B, Area C, Area D (the 3D viewport; each area has its own header)} â€” or to a SPLIT BORDER that lies on a
//   split line and has the one uniform border width. Nothing else.
//
// A pixel in no area and no border is a GAP. A pixel in two areas, or in an
// area and a border, is an OVERLAP. A border of any other width is a defect.
//
// HOW: this drives the REAL layout code â€” drawWorkspace(), beginDockedPanel()
// for the four areas (exactly as Editor::drawArea opens them), drawMainMenu() and
// drawStatusBar() â€” in a headless ImGui context with docking enabled. None of
// those touch OpenGL. Then it reads the resulting dock-node and window rects
// out of ImGui and rasterises coverage at every pixel centre.
//
// ITEM 181 - Area D's body is not empty space. Inside it are THREE contiguous
// child Regions: the Toolbar (T, left), the renderer body (middle) and the
// Sidebar (N, right). drawArea() returns the middle one, which is the rectangle
// the renderer draws into (Editor::viewportRegion_). The Toolbar and the Sidebar
// must be CHILD windows of the Area D window: a Toolbar drawn as its own
// top-level ImGui window is exactly the floating panel the human called awkward,
// and the tests below must fail on one.
//
// Editor::draw itself is not driven: it needs a GLFW window and a GL renderer.
// What is skipped is the drawing of the CONTENTS of each area; the geometry of
// the layout is entirely buildLayout()'s, and that is what this exercises.
#include "harness.h"

#include <glad/gl.h>   // before the editor/render headers, as everywhere else in this tree

#include "core/Types.h"
#include "editor/Editor.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "editor/Gizmo.h"
#include "editor/Modal.h"
#include "editor/Panels.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Item 175: the REAL drawArea()/applyTheme() are file-local to Editor.cpp, so this file includes it (as theme_probe.cpp does) to
// draw the real viewport header headlessly. The static ember_editor library is not pulled for those symbols: this TU defines them.
#include "editor/Editor.cpp"
#include <map>
#include <regex>
#include <fstream>
#include <sstream>
#include <algorithm>

using ember::editor::GizmoController;
using ember::editor::ModalTransform;
using ember::editor::ToolState;
using ember::editor::TransformSpace;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

struct R {
    float x = 0, y = 0, w = 0, h = 0;
    bool operator==(const R& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

std::string fmt(const R& r) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.2f, %.2f, %.2f x %.2f)", r.x, r.y, r.w, r.h);
    return b;
}

struct Layout {
    bool ok = false;
    std::string why;
    float border = 0;            // style.DockingSeparatorSize in effect
    R menu, status, workspace, viewport, areaA, areaB, areaC, areaD;
    R winA, winB, winC, winD;    // the area WINDOWS (what ImGui draws), as opposed to their dock nodes
    R body;                      // item 181: what drawArea (Area D) returned - the renderer Region
    R toolbar, sidebar;          // item 181: Area D's child Regions
    bool regionsFound = false;   // both child Regions were drawn (Toolbar and Sidebar both open)
    R reportedViewport;          // what drawWorkspace returned (the central node, i.e. Area D)
    std::vector<R> borders;      // one per split in the dock tree
};

void collectBorders(const ImGuiDockNode* n, std::vector<R>& out) {
    if (!n || !n->ChildNodes[0] || !n->ChildNodes[1]) return;
    const ImGuiDockNode* a = n->ChildNodes[0];
    const ImGuiDockNode* b = n->ChildNodes[1];
    if (n->SplitAxis == ImGuiAxis_X) {
        const float x0 = a->Pos.x + a->Size.x;
        out.push_back({x0, n->Pos.y, b->Pos.x - x0, n->Size.y});
    } else {
        const float y0 = a->Pos.y + a->Size.y;
        out.push_back({n->Pos.x, y0, n->Size.x, b->Pos.y - y0});
    }
    collectBorders(a, out);
    collectBorders(b, out);
}

R nodeRect(const ImGuiDockNode* n) { return {n->Pos.x, n->Pos.y, n->Size.x, n->Size.y}; }
R winRect(const ImGuiWindow* w) { return {w->Pos.x, w->Pos.y, w->Size.x, w->Size.y}; }

// Item 181: a Region of an Area is a CHILD window of that area's window (ImGui stores it as
// "<parent>/<id>_<hash>"), not a top-level window and not a dock node. Found by parent AND id
// fragment, so this cannot pass by finding an unrelated window whose name merely looks alike.
ImGuiWindow* childRegion(ImGuiWindow* parent, const char* idFragment) {
    if (!parent) return nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->ParentWindow == parent && std::string(w->Name).find(idFragment) != std::string::npos)
            return w;
    return nullptr;
}

// One "launch": fresh context, the real layout code for several frames, then
// read every rect back. The context is destroyed before returning, so two calls
// share no ImGui state â€” the same isolation two process launches would have.
// A headless editor "session": one ImGui context plus the state Editor::draw
// threads through the layout code. frame() runs the layout calls in the order
// Editor::draw makes them. Destroyed with the Rig, so two Rigs share nothing.
struct Rig {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::RenderStats stats{};
    ember::SelectMode selectMode = ember::SelectMode::Vertex;
    ToolState tools{};
    GizmoController gizmo;
    ModalTransform modal;
    ember::editor::UndoStack undo;
    ember::editor::FileDialog file;
    ember::editor::DocumentState doc;
    ember::ViewportSettings viewport{};
    ember::editor::SidebarTab tab = ember::editor::SidebarTab::Item;
    ember::editor::TimelineState timeline{};
    ember::editor::PropertiesTab propertiesTab = ember::editor::PropertiesTab::Object;
    ember::editor::AreaType areaDType = ember::editor::AreaType::Viewport;
    ember::InteractionMode interaction = ember::InteractionMode::Object;
    ember::editor::SnapTarget snapTarget = ember::editor::SnapTarget::GridIncrement;
    TransformSpace space = TransformSpace::Global;
    bool sidebarOpen = true, historyOpen = false, layoutBuilt = false, snapEnabled = false;
    ImVec4 region{};      // drawWorkspace's return: the central node (Area D)
    ImVec4 body{};        // drawArea's return: the renderer Region between Toolbar and Sidebar

    Rig(float width, float height) {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // as Editor::init does
        io.IniFilename = nullptr;                            // as Editor::init does
        io.DisplaySize = ImVec2(width, height);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        // The metrics Editor.cpp's applyTheme sets (it is file-local, so mirrored here).
        // Deliberately NOT mirrored: WindowMinSize, which applyTheme never touches, so
        // ImGui's default (32x32) is what the real app runs with too.
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowBorderSize = 1.0f;
        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(5.0f, 3.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
        style.ScrollbarSize = 12.0f;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    }
    ~Rig() { ImGui::DestroyContext(ctx); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // The calls Editor::draw makes, in its order. Areas A/B/C are placeholders: this suite
    // asserts the GEOMETRY of the layout and their contents are not part of it. Area D is the
    // real thing - the REAL drawArea(), which opens the Area D window, draws the Toolbar and
    // Sidebar as child Regions inside it, and returns the renderer's middle Region (item 181).
    void frame() {
        ImGui::NewFrame();
        // Same order as Editor::draw: the Topbar first (it sets WorkPos), then the workspace.
        ember::editor::drawMainMenu(world, undo, ember::Vec3{0, 0, 0}, file, doc, viewport, space,
                                    sidebarOpen, historyOpen);
        region = ember::editor::drawWorkspace(layoutBuilt);
        ember::editor::drawStatusBar(tools, gizmo, TransformSpace::Global, modal, false);
        for (const char* name : {"Area A", "Area B", "Area C"}) {
            // NoTitleBar, as Editor::drawArea opens them (each area draws its own header).
            ember::editor::beginDockedPanel(name, nullptr, ImGuiWindowFlags_NoTitleBar);
            ImGui::TextUnformatted(name);
            ember::editor::endDockedPanel();
        }
        body = ember::editor::drawArea(ember::editor::labels::DockAreaD, areaDType, true, world, undo,
                                       stats, timeline, ember::Vec3{0, 0, 0}, viewport, space, tools,
                                       sidebarOpen, tab, interaction, selectMode, propertiesTab,
                                       snapEnabled, snapTarget);
        ImGui::EndFrame();
    }
    void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
};

Layout launch(float width, float height) {
    Layout L;
    Rig rig(width, height);
    rig.frames(6);
    const ImVec4 region = rig.region;

    L.border = ImGui::GetStyle().DockingSeparatorSize;
    L.reportedViewport = {region.x, region.y, region.z, region.w};

    ImGuiWindow* ws = ImGui::FindWindowByName("##workspace");
    ImGuiWindow* mn = ImGui::FindWindowByName("##MainMenuBar");
    ImGuiWindow* st = ImGui::FindWindowByName("##status");
    ImGuiWindow* wa = ImGui::FindWindowByName("Area A");
    ImGuiWindow* wb = ImGui::FindWindowByName("Area B");
    ImGuiWindow* wc = ImGui::FindWindowByName("Area C");
    ImGuiWindow* wd = ImGui::FindWindowByName("Area D");
    if (!ws || !mn || !st || !wa || !wb || !wc || !wd) {
        L.why = "a layout window was never created";
    } else if (!wa->DockNode || !wb->DockNode || !wc->DockNode || !wd->DockNode) {
        L.why = "an area window is not docked (it would float)";
    } else {
        const ImGuiID dockId = ws->GetID("EmberDock");
        const ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockId);
        const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId);
        if (!root || !central) {
            L.why = "dock root or central node missing";
        } else {
            L.menu = winRect(mn);
            L.status = winRect(st);
            L.workspace = nodeRect(root);
            L.viewport = nodeRect(central);
            L.areaA = nodeRect(wa->DockNode);
            L.areaB = nodeRect(wb->DockNode);
            L.areaC = nodeRect(wc->DockNode);
            L.areaD = nodeRect(wd->DockNode);
            L.winA = winRect(wa); L.winB = winRect(wb); L.winC = winRect(wc); L.winD = winRect(wd);
            // Item 181: Area D's Toolbar and Sidebar are CHILD Regions of the Area D window.
            ImGuiWindow* tb = childRegion(wd, "##toolbar");
            ImGuiWindow* sb = childRegion(wd, "##sidebar");
            L.regionsFound = tb != nullptr && sb != nullptr;
            if (tb) L.toolbar = winRect(tb);
            if (sb) L.sidebar = winRect(sb);
            L.body = {rig.body.x, rig.body.y, rig.body.z, rig.body.w};
            collectBorders(root, L.borders);
            L.ok = true;
        }
    }
    return L;
}

bool covers(const R& r, float px, float py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

struct Coverage {
    long gaps = 0, overlaps = 0, borderPixels = 0, pixels = 0;
    R firstGap{}, firstOverlap{};
};

// Rasterise at every pixel centre. Areas = the six tiles; borders = split bands.
Coverage rasterise(const Layout& L, int width, int height) {
    // The viewport IS Area D now (the central node holds it), so it is not listed twice.
    const R areas[6] = {L.menu, L.status, L.areaA, L.areaB, L.areaC, L.areaD};
    Coverage c;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            int inArea = 0;
            for (const R& r : areas) inArea += covers(r, px, py) ? 1 : 0;
            int inBorder = 0;
            for (const R& r : L.borders) inBorder += covers(r, px, py) ? 1 : 0;
            ++c.pixels;
            if (inArea == 0 && inBorder == 0) {
                if (c.gaps++ == 0) c.firstGap = {float(x), float(y), 1, 1};
            } else if (inArea + inBorder > 1 && !(inArea == 0 && inBorder > 1)) {
                // two areas, or an area under a border. (Two borders crossing at
                // a T-junction is not an overlap of AREAS, so it is allowed.)
                if (c.overlaps++ == 0) c.firstOverlap = {float(x), float(y), 1, 1};
            } else if (inArea == 0) {
                ++c.borderPixels;
            }
        }
    }
    return c;
}

void checkTiles(float W, float H) {
    const Layout L = launch(W, H);
    CHECK_MSG(L.ok, "%gx%g: layout could not be read: %s", W, H, L.why.c_str());
    if (!L.ok) return;

    std::printf("       %gx%g  border=%.1f\n", W, H, L.border);
    std::printf("         menu bar %s\n", fmt(L.menu).c_str());
    std::printf("         status   %s\n         central  %s\n", fmt(L.status).c_str(),
                fmt(L.viewport).c_str());
    std::printf("         area A   %s\n         area B   %s\n         area C   %s\n         area D   %s\n",
                fmt(L.areaA).c_str(), fmt(L.areaB).c_str(), fmt(L.areaC).c_str(), fmt(L.areaD).c_str());
    for (const R& b : L.borders) std::printf("         border   %s\n", fmt(b).c_str());

    // The bands stack with no gap: menu bar on the top edge, then the workspace
    // (each area carries its own header INSIDE it), then the status bar on the
    // bottom edge.
    CHECK_MSG(L.menu.x == 0 && L.menu.y == 0 && L.menu.w == W,
              "%gx%g: main menu bar does not span the top edge: %s", W, H, fmt(L.menu).c_str());
    CHECK_MSG(L.workspace.x == 0 && L.workspace.w == W && L.workspace.y == L.menu.y + L.menu.h,
              "%gx%g: gap/overlap between menu bar (ends %.2f) and workspace (starts %.2f)",
              W, H, L.menu.y + L.menu.h, L.workspace.y);
    CHECK_MSG(L.viewport == L.areaD,
              "%gx%g: the central (renderer) node %s is not the viewport area D %s",
              W, H, fmt(L.viewport).c_str(), fmt(L.areaD).c_str());
    CHECK_MSG(L.status.y == L.workspace.y + L.workspace.h,
              "%gx%g: gap/overlap between workspace (ends %.2f) and status bar (starts %.2f)",
              W, H, L.workspace.y + L.workspace.h, L.status.y);
    CHECK_MSG(L.status.y + L.status.h == H && L.status.w == W,
              "%gx%g: status bar does not sit on the bottom edge: %s", W, H, fmt(L.status).c_str());

    // Every split border has the ONE uniform width, and that width is >0.
    CHECK_MSG(L.borders.size() == 3, "%gx%g: expected 3 splits, got %zu", W, H, L.borders.size());
    for (const R& b : L.borders) {
        const float thick = (b.w < b.h) ? b.w : b.h;
        CHECK_MSG(std::fabs(thick - L.border) < 1e-3f,
                  "%gx%g: split border %s is %.3fpx thick, expected the uniform %.1fpx",
                  W, H, fmt(b).c_str(), thick, L.border);
    }

    // Item 181: inside Area D, the three Regions tile the Area's body exactly - Toolbar
    // flush with the left edge, the renderer body in the middle, Sidebar flush with the
    // right, all three sharing the body's top and height. Run at 1366x777 too: that is the
    // size where a rounding gap or overlap between the three would show up.
    CHECK_MSG(L.regionsFound, "%gx%g: the Toolbar/Sidebar child Regions of Area D were not both drawn", W, H);
    if (L.regionsFound) {
        std::printf("         body     %s\n         toolbar  %s\n         sidebar  %s\n",
                    fmt(L.body).c_str(), fmt(L.toolbar).c_str(), fmt(L.sidebar).c_str());
        CHECK_MSG(std::fabs(L.toolbar.x - L.winD.x) < 1e-3f,
                  "%gx%g: the Toolbar starts at %.2f, not at Area D's left edge %.2f", W, H, L.toolbar.x, L.winD.x);
        CHECK_MSG(std::fabs((L.toolbar.x + L.toolbar.w) - L.body.x) < 1e-3f,
                  "%gx%g: gap/overlap between the Toolbar (ends %.2f) and the renderer body (starts %.2f)",
                  W, H, L.toolbar.x + L.toolbar.w, L.body.x);
        CHECK_MSG(std::fabs((L.body.x + L.body.w) - L.sidebar.x) < 1e-3f,
                  "%gx%g: gap/overlap between the renderer body (ends %.2f) and the Sidebar (starts %.2f)",
                  W, H, L.body.x + L.body.w, L.sidebar.x);
        CHECK_MSG(std::fabs((L.sidebar.x + L.sidebar.w) - (L.winD.x + L.winD.w)) < 1e-3f,
                  "%gx%g: the Sidebar ends at %.2f, not at Area D's right edge %.2f",
                  W, H, L.sidebar.x + L.sidebar.w, L.winD.x + L.winD.w);
        for (const R& r : {L.toolbar, L.body, L.sidebar}) {
            CHECK_MSG(std::fabs(r.y - L.body.y) < 1e-3f && std::fabs(r.h - L.body.h) < 1e-3f,
                      "%gx%g: Region %s does not share the body's top %.2f / height %.2f",
                      W, H, fmt(r).c_str(), L.body.y, L.body.h);
            CHECK_MSG(r.w > 0.0f, "%gx%g: Region %s is empty", W, H, fmt(r).c_str());
        }
    }

    // Mechanical tiling, pixel by pixel.
    const Coverage c = rasterise(L, int(W), int(H));
    std::printf("         pixels=%ld gaps=%ld overlaps=%ld border=%ld\n",
                c.pixels, c.gaps, c.overlaps, c.borderPixels);
    CHECK_MSG(c.gaps == 0, "%gx%g: %ld uncovered pixel(s), first at %s",
              W, H, c.gaps, fmt(c.firstGap).c_str());
    CHECK_MSG(c.overlaps == 0, "%gx%g: %ld doubly-covered pixel(s), first at %s",
              W, H, c.overlaps, fmt(c.firstOverlap).c_str());

    // drawWorkspace reports the central node, and that node is Area D. (The rectangle the
    // renderer actually draws into is drawArea's return - the middle Region between the
    // Toolbar and the Sidebar - and is asserted in the item-181 block above and in the
    // regions test below.)
    CHECK_MSG(L.reportedViewport == L.viewport,
              "%gx%g: drawWorkspace returned %s but the central node is %s",
              W, H, fmt(L.reportedViewport).c_str(), fmt(L.viewport).c_str());
}

} // namespace

TEST(layout_tiles_the_window_at_1280x720) { checkTiles(1280, 720); }
TEST(layout_tiles_the_window_at_1920x1080) { checkTiles(1920, 1080); }
// An awkward size: 0.18 / 0.22 / 0.55 splits of these do not land on whole pixels,
// which is where a rounding gap or overlap would show up.
TEST(layout_tiles_the_window_at_1366x777) { checkTiles(1366, 777); }

TEST(layout_default_is_identical_across_two_launches) {
    const Layout a = launch(1280, 720);
    const Layout b = launch(1280, 720);
    CHECK_MSG(a.ok && b.ok, "a launch failed: '%s' / '%s'", a.why.c_str(), b.why.c_str());
    if (!a.ok || !b.ok) return;
    const R ra[] = {a.menu, a.status, a.workspace, a.viewport, a.areaA, a.areaB, a.areaC, a.areaD,
                    a.body, a.toolbar, a.sidebar};
    const R rb[] = {b.menu, b.status, b.workspace, b.viewport, b.areaA, b.areaB, b.areaC, b.areaD,
                    b.body, b.toolbar, b.sidebar};
    const char* names[] = {"menu bar", "status", "workspace", "viewport", "area A", "area B", "area C",
                           "area D", "renderer body", "toolbar Region", "sidebar Region"};
    CHECK_MSG(a.regionsFound && b.regionsFound, "the Toolbar/Sidebar Regions were not drawn in both launches");
    for (int i = 0; i < 11; ++i)
        CHECK_MSG(ra[i] == rb[i], "%s differs between launches: %s vs %s",
                  names[i], fmt(ra[i]).c_str(), fmt(rb[i]).c_str());
    CHECK_MSG(a.borders.size() == b.borders.size(), "split count differs between launches");
    for (std::size_t i = 0; i < a.borders.size() && i < b.borders.size(); ++i)
        CHECK_MSG(a.borders[i] == b.borders[i], "border %zu differs: %s vs %s",
                  i, fmt(a.borders[i]).c_str(), fmt(b.borders[i]).c_str());
}

// ---- WHAT-REMAINS.md §1: the other rules, as executable checks (item 136) --------

// "Splitters: draggable borders between areas so the layout can be resized."
// Press on the vertical split between the 3D viewport and the right-hand column,
// drag 60px left, release: the border must have MOVED by about that much. This is
// the real input path (ImGui's event queue), not a poke at dock-node fields.
TEST(layout_splitter_between_areas_is_draggable) {
    Rig rig(1280, 720);
    rig.frames(6);
    ImGuiWindow* ws = ImGui::FindWindowByName("##workspace");
    CHECK_MSG(ws != nullptr, "no ##workspace window");
    if (!ws) return;
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(ws->GetID("EmberDock"));
    CHECK_MSG(root != nullptr, "no dock root");
    if (!root) return;

    std::vector<R> before;
    collectBorders(root, before);
    const R* vertical = nullptr;
    for (const R& b : before) if (b.w < b.h) { vertical = &b; break; }
    CHECK_MSG(vertical != nullptr, "no vertical split found between viewport and right column");
    if (!vertical) return;
    const float startX = vertical->x + vertical->w * 0.5f;
    const float y = vertical->y + vertical->h * 0.5f;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(startX, y);
    rig.frames(3);
    io.AddMouseButtonEvent(0, true);
    rig.frames(2);
    for (int step = 1; step <= 6; ++step) {
        io.AddMousePosEvent(startX - 10.0f * step, y);
        rig.frames(2);
    }
    io.AddMouseButtonEvent(0, false);
    rig.frames(3);

    std::vector<R> after;
    collectBorders(ImGui::DockBuilderGetNode(ws->GetID("EmberDock")), after);
    const R* vAfter = nullptr;
    for (const R& b : after) if (b.w < b.h) { vAfter = &b; break; }
    CHECK_MSG(vAfter != nullptr, "vertical split vanished after the drag");
    if (!vAfter) return;
    std::printf("       splitter x: %.1f -> %.1f after a 60px drag left\n", vertical->x, vAfter->x);
    CHECK_MSG(vAfter->x < vertical->x - 40.0f,
              "dragging the splitter 60px left moved it %.1fpx (from %.1f to %.1f); expected >=40",
              vertical->x - vAfter->x, vertical->x, vAfter->x);
}

// "One DockSpace covering the whole window. Set the ImGui flags so windows cannot
// be moved, resized, collapsed or brought to front." Walk every live top-level
// window: each must be immovable and unresizable, and every AREA must also be
// uncollapsible and sit in a dock node that forbids undocking. Anything else is a
// floating window — the thing the human called awkward.
TEST(layout_no_window_can_float) {
    Rig rig(1280, 720);
    rig.frames(6);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    int topLevel = 0;
    for (ImGuiWindow* w : g.Windows) {
        if (!w->WasActive) continue;
        const ImGuiWindowFlags f = w->Flags;
        if (f & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip |
                 ImGuiWindowFlags_DockNodeHost))
            continue;
        if (std::string(w->Name).rfind("Debug##", 0) == 0) continue;   // ImGui's own
        ++topLevel;
        CHECK_MSG((f & ImGuiWindowFlags_NoMove) != 0, "window '%s' can be MOVED (no NoMove)", w->Name);
        CHECK_MSG((f & ImGuiWindowFlags_NoResize) != 0, "window '%s' can be RESIZED (no NoResize)", w->Name);
    }
    // Docked windows are drawn as children of their dock host, so the walk above sees the
    // top-level chrome; the areas are checked by name below. Assert the walk really visited
    // the windows it is meant to police.
    for (const char* name : {"##workspace", "##MainMenuBar", "##status"}) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        CHECK_MSG(w != nullptr && w->WasActive, "layout window '%s' was never active", name);
    }
    CHECK_MSG(topLevel >= 3, "expected >=3 top-level layout windows (workspace, topbar, status bar), saw %d", topLevel);

    // ITEM 181: the Toolbar and the Sidebar are Regions of Area D - CHILD windows of the Area D
    // window. A top-level window with either name is the floating panel this item exists to
    // remove, so it fails here even if the child Regions are also drawn.
    ImGuiWindow* wd = ImGui::FindWindowByName("Area D");
    CHECK_MSG(wd != nullptr, "Area D is missing, so its child Regions cannot be checked");
    struct RegionName { const char* id; const char* what; };
    for (const RegionName& rn : {RegionName{"##toolbar", "Toolbar"}, RegionName{"##sidebar", "Sidebar"}}) {
        ImGuiWindow* floating = ImGui::FindWindowByName(rn.id);
        CHECK_MSG(floating == nullptr,
                  "'%s' exists as a top-level window: the %s is a floating window, not an Area D Region",
                  rn.id, rn.what);
        ImGuiWindow* child = childRegion(wd, rn.id);
        CHECK_MSG(child != nullptr && child->WasActive, "the %s was not drawn as a child Region of Area D (item 181)", rn.what);
        if (!child) continue;
        CHECK_MSG(child->ParentWindow == wd, "the %s is not a CHILD of Area D: it is parented to '%s'", rn.what,
                  child->ParentWindow ? child->ParentWindow->Name : "(top level)");
        CHECK_MSG(child->DockNode == nullptr, "the %s is docked; a Region is a child of the area, not a dock node", rn.what);
    }
    for (const char* name : {"Area A", "Area B", "Area C", "Area D"}) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        CHECK_MSG(w != nullptr, "area '%s' missing", name);
        if (!w) continue;
        CHECK_MSG((w->Flags & ImGuiWindowFlags_NoCollapse) != 0, "area '%s' can be COLLAPSED", name);
        CHECK_MSG((w->Flags & ImGuiWindowFlags_NoBringToFrontOnFocus) != 0,
                  "area '%s' can be BROUGHT TO FRONT", name);
        CHECK_MSG(w->DockNode != nullptr, "area '%s' is not docked: it floats", name);
        if (w->DockNode)
            CHECK_MSG((w->DockNode->MergedFlags & ImGuiDockNodeFlags_NoUndocking) != 0,
                      "area '%s' sits in a dock node that allows UNDOCKING it", name);
    }
}

// "The 3D viewport's Toolbar and Sidebar live inside the viewport area." ITEM 181 makes that
// precise: they are CHILD REGIONS of the Area D window that tile its body together with the
// renderer body - Toolbar flush left, renderer body in the middle, Sidebar flush right, one
// shared top and height, no gap and no overlap. Their windows lie wholly inside Area D and are
// not dock nodes; the rectangle drawArea() hands the renderer is exactly the middle Region.
TEST(layout_toolbar_and_sidebar_lie_inside_the_viewport) {
    for (float W : {800.0f, 1280.0f, 1920.0f}) {
        const float H = W == 800.0f ? 600.0f : (W == 1280.0f ? 720.0f : 1080.0f);
        Rig rig(W, H);
        rig.frames(6);
        const R v = {rig.region.x, rig.region.y, rig.region.z, rig.region.w};   // the whole Area D
        const R body = {rig.body.x, rig.body.y, rig.body.z, rig.body.w};       // the renderer Region
        ImGuiWindow* areaD = ImGui::FindWindowByName("Area D");
        CHECK_MSG(areaD != nullptr && areaD->WasActive, "%gx%g: Area D was never drawn", W, H);
        if (!areaD) continue;
        const R areaDRect = winRect(areaD);
        ImGuiWindow* tb = childRegion(areaD, "##toolbar");
        ImGuiWindow* sb = childRegion(areaD, "##sidebar");
        CHECK_MSG(tb != nullptr && sb != nullptr,
                  "%gx%g: the Toolbar/Sidebar child Regions of Area D were not both drawn (toolbar %s, sidebar %s)",
                  W, H, tb ? "found" : "missing", sb ? "found" : "missing");
        if (!tb || !sb) continue;
        const R tr = winRect(tb), sr = winRect(sb);
        std::printf("       %gx%g  Area D %s\n", W, H, fmt(areaDRect).c_str());
        std::printf("         toolbar %s  body %s  sidebar %s\n", fmt(tr).c_str(), fmt(body).c_str(),
                    fmt(sr).c_str());

        // Both Regions sit inside the viewport area, and neither is a dock node.
        struct NamedRegion { const char* what; R r; };
        for (const NamedRegion& nr : {NamedRegion{"Toolbar", tr}, NamedRegion{"Sidebar", sr}}) {
            CHECK_MSG(nr.r.x >= v.x && nr.r.y >= v.y && nr.r.x + nr.r.w <= v.x + v.w && nr.r.y + nr.r.h <= v.y + v.h,
                      "%gx%g: the %s %s is NOT inside the viewport area D %s", W, H, nr.what, fmt(nr.r).c_str(), fmt(v).c_str());
        }
        CHECK_MSG(tb->DockNode == nullptr && sb->DockNode == nullptr,
                  "%gx%g: the Toolbar/Sidebar are docked; they must be Regions of the area", W, H);
        CHECK_MSG(tb->ParentWindow == areaD && sb->ParentWindow == areaD,
                  "%gx%g: a Region is not a child of the Area D window", W, H);

        // The three Regions tile the Area's body: exact edges, one top, one height.
        CHECK_MSG(std::fabs(tr.x - areaDRect.x) < 1e-3f,
                  "%gx%g: the Toolbar starts at %.2f, not at Area D's left edge %.2f", W, H, tr.x, areaDRect.x);
        CHECK_MSG(std::fabs((tr.x + tr.w) - body.x) < 1e-3f,
                  "%gx%g: gap/overlap between the Toolbar (ends %.2f) and the renderer body (starts %.2f)",
                  W, H, tr.x + tr.w, body.x);
        CHECK_MSG(std::fabs((body.x + body.w) - sr.x) < 1e-3f,
                  "%gx%g: gap/overlap between the renderer body (ends %.2f) and the Sidebar (starts %.2f)",
                  W, H, body.x + body.w, sr.x);
        CHECK_MSG(std::fabs((sr.x + sr.w) - (areaDRect.x + areaDRect.w)) < 1e-3f,
                  "%gx%g: the Sidebar ends at %.2f, not at Area D's right edge %.2f",
                  W, H, sr.x + sr.w, areaDRect.x + areaDRect.w);
        CHECK_MSG(std::fabs(tr.y - body.y) < 1e-3f && std::fabs(sr.y - body.y) < 1e-3f,
                  "%gx%g: the Regions do not share the body's top edge (toolbar %.2f, body %.2f, sidebar %.2f)",
                  W, H, tr.y, body.y, sr.y);
        CHECK_MSG(std::fabs(tr.h - body.h) < 1e-3f && std::fabs(sr.h - body.h) < 1e-3f,
                  "%gx%g: the Regions do not share the body's height (toolbar %.2f, body %.2f, sidebar %.2f)",
                  W, H, tr.h, body.h, sr.h);
        // The drawn widths are the editor's own theme-scaled Region metrics.
        CHECK_MSG(std::fabs(tr.w - ember::editor::toolbarRegionWidth()) < 0.5f,
                  "%gx%g: the Toolbar is %.1f px wide, not its metric %.1f", W, H, double(tr.w),
                  double(ember::editor::toolbarRegionWidth()));
        CHECK_MSG(std::fabs(sr.w - ember::editor::sidebarRegionWidth()) < 0.5f,
                  "%gx%g: the Sidebar is %.1f px wide, not its metric %.1f", W, H, double(sr.w),
                  double(ember::editor::sidebarRegionWidth()));
        CHECK_MSG(body.w > 0.0f && body.h > 0.0f, "%gx%g: the renderer body Region is empty: %s", W, H, fmt(body).c_str());
        if (W == 800.0f)
            CHECK_MSG(body.w >= 120.0f,
                      "800x600: Toolbar/Sidebar leave only %.1f px for the renderer; expected a useful narrow Area body",
                      double(body.w));
        // Toolbar and Sidebar never cross each other.
        CHECK_MSG(tr.x + tr.w <= sr.x, "%gx%g: the Toolbar and the Sidebar overlap each other", W, H);
    }
}

// CONTROL: the rule above must be able to FAIL. Draw the Toolbar the OLD way - a top-level
// ImGui window named "##toolbar", the floating panel item 181 removes - and assert that the
// predicate used above ("no top-level ##toolbar exists") reports it. Without this, a test that
// found nothing at all would also appear to pass.
TEST(layout_control_a_floating_toolbar_is_reported) {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    unsigned char* px = nullptr; int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f));
    ImGui::Begin("##toolbar");            // the floating Toolbar, drawn as its own window
    ImGui::TextUnformatted("Toolbar");
    ImGui::End();
    ImGui::EndFrame();
    CHECK_MSG(ImGui::FindWindowByName("##toolbar") != nullptr,
              "control: a floating top-level '##toolbar' was not found, so the Region predicate could not catch one");
    ImGui::DestroyContext(ctx);
}

// ==== ITEM 168: the layout INVARIANTS (WHAT-REMAINS 1) ==========================================
//
// "Areas tile the window completely - no gaps, no overlaps, nothing on top of anything", with a status
// bar pinned to the bottom and a topbar pinned to the top. Pinned as five separate invariants, each run
// at 1280x720 AND 1920x1080, each over BOTH the area dock nodes and the area WINDOWS:
//   1. every area rect lies inside the workspace rect;
//   2. no two areas overlap;
//   3. no area rect crosses the topbar or the status bar;
//   4. the topbar + workspace + status bar EXACTLY cover the client area (no gap, no overlap);
//   5. the topbar is pinned to the top edge and the status bar to the bottom edge, full width.
// A sixth, REAL-Editor-only invariant covers the visible symptom the dock geometry cannot show: an
// area's CONTENT must fit its own area (no vertical scrollbar), because a Timeline whose ruler is
// pushed under the status bar is "content crossing the status bar" even when every rect tiles.
// The CPU tests use the same drawWorkspace/drawStatusBar/drawMainMenu as the app with placeholder
// area content; the GPU tests (--gpu) run the real Editor::draw in a hidden window.
namespace {

struct Named { std::string name; R r; };
struct ContentFit { std::string name; float contentH = 0, innerH = 0; bool scrollbar = false; };

struct Snap {
    std::string tag;
    float W = 0, H = 0;
    R top, status, work;
    std::vector<Named> nodes;     // area dock-node rects
    std::vector<Named> windows;   // area window rects
    std::vector<ContentFit> content;   // real Editor only
};

float overlapArea(const R& a, const R& b) {
    const float w = std::fmin(a.x + a.w, b.x + b.w) - std::fmax(a.x, b.x);
    const float h = std::fmin(a.y + a.h, b.y + b.h) - std::fmax(a.y, b.y);
    return (w > 0 && h > 0) ? w * h : 0.0f;
}
bool inside(const R& r, const R& outer, float tol = 0.01f) {
    return r.x >= outer.x - tol && r.y >= outer.y - tol && r.x + r.w <= outer.x + outer.w + tol && r.y + r.h <= outer.y + outer.h + tol;
}

Snap snapFrom(const Layout& L, float W, float H) {
    Snap s;
    s.W = W; s.H = H;
    s.top = L.menu; s.status = L.status; s.work = L.workspace;
    s.nodes = {{"Area A", L.areaA}, {"Area B", L.areaB}, {"Area C", L.areaC}, {"Area D", L.areaD}};
    s.windows = {{"Area A", L.winA}, {"Area B", L.winB}, {"Area C", L.winC}, {"Area D", L.winD}};
    return s;
}

const float kSizes[2][2] = {{1280, 720}, {1920, 1080}};

// Invariant assertions. Normally an ordinary failing check; when run as a CONTROL (quiet) they only COUNT,
// so the injected, expected violations do not print FAIL lines into a passing test's log.
bool g_quiet = false;
int g_quietCount = 0;
#define INV_CHECK(cond, ...)                                                          \
    do {                                                                              \
        if (g_quiet) { if (!(cond)) ++g_quietCount; }                                  \
        else { CHECK_MSG(cond, __VA_ARGS__); }                                         \
    } while (0)
void inv1_inside_workspace(const Snap& s) {
    for (const auto* set : {&s.nodes, &s.windows})
        for (const Named& a : *set)
            INV_CHECK(inside(a.r, s.work), "[%s] %s %s lies OUTSIDE the workspace %s", s.tag.c_str(), set == &s.nodes ? "node" : "window", (a.name + " " + fmt(a.r)).c_str(), fmt(s.work).c_str());
}
void inv2_no_overlap(const Snap& s) {
    for (const auto* set : {&s.nodes, &s.windows})
        for (std::size_t i = 0; i < set->size(); ++i)
            for (std::size_t j = i + 1; j < set->size(); ++j) {
                const float o = overlapArea((*set)[i].r, (*set)[j].r);
                INV_CHECK(o == 0.0f, "[%s] %s %s and %s %s OVERLAP by %.1f px^2", s.tag.c_str(), set == &s.nodes ? "node" : "window", (*set)[i].name.c_str(), fmt((*set)[i].r).c_str(),
                          (*set)[j].name.c_str(), double(o));
            }
}
void inv3_no_crossing_of_bars(const Snap& s) {
    for (const auto* set : {&s.nodes, &s.windows})
        for (const Named& a : *set) {
            const float ot = overlapArea(a.r, s.top), os = overlapArea(a.r, s.status);
            INV_CHECK(ot == 0.0f, "[%s] %s %s %s crosses the TOPBAR %s by %.1f px^2", s.tag.c_str(), set == &s.nodes ? "node" : "window", a.name.c_str(), fmt(a.r).c_str(), fmt(s.top).c_str(), double(ot));
            INV_CHECK(os == 0.0f, "[%s] %s %s %s crosses the STATUS BAR %s by %.1f px^2", s.tag.c_str(), set == &s.nodes ? "node" : "window", a.name.c_str(), fmt(a.r).c_str(), fmt(s.status).c_str(), double(os));
        }
}
void inv4_exact_cover(const Snap& s) {
    INV_CHECK(s.top.x == 0 && s.top.w == s.W, "[%s] topbar %s does not span the full width %g", s.tag.c_str(), fmt(s.top).c_str(), double(s.W));
    INV_CHECK(s.work.x == 0 && s.work.w == s.W, "[%s] workspace %s does not span the full width %g", s.tag.c_str(), fmt(s.work).c_str(), double(s.W));
    INV_CHECK(s.status.x == 0 && s.status.w == s.W, "[%s] status bar %s does not span the full width %g", s.tag.c_str(), fmt(s.status).c_str(), double(s.W));
    INV_CHECK(s.work.y == s.top.y + s.top.h, "[%s] gap/overlap between topbar (ends %.2f) and workspace (starts %.2f)", s.tag.c_str(), double(s.top.y + s.top.h), double(s.work.y));
    INV_CHECK(s.status.y == s.work.y + s.work.h, "[%s] gap/overlap between workspace (ends %.2f) and status bar (starts %.2f)", s.tag.c_str(), double(s.work.y + s.work.h), double(s.status.y));
    INV_CHECK(s.status.y + s.status.h == s.H, "[%s] status bar ends at %.2f, the client area ends at %g", s.tag.c_str(), double(s.status.y + s.status.h), double(s.H));
    const float sum = s.top.w * s.top.h + s.work.w * s.work.h + s.status.w * s.status.h;
    INV_CHECK(std::fabs(sum - s.W * s.H) < 0.5f, "[%s] topbar + workspace + status cover %.0f px^2 of a %.0f px^2 client area", s.tag.c_str(), double(sum), double(s.W * s.H));
}
void inv5_pinned(const Snap& s) {
    INV_CHECK(s.top.y == 0, "[%s] topbar is not pinned to the top edge: y=%.2f", s.tag.c_str(), double(s.top.y));
    INV_CHECK(s.status.y + s.status.h == s.H, "[%s] status bar is not pinned to the bottom edge: bottom=%.2f, client bottom=%g", s.tag.c_str(), double(s.status.y + s.status.h), double(s.H));
}
void inv6_content_fits(const Snap& s) {
    for (const ContentFit& c : s.content) {
        std::printf("       [%s] %s content %.0f px in a %.0f px inner area, vertical scrollbar: %s\n", s.tag.c_str(), c.name.c_str(), double(c.contentH), double(c.innerH), c.scrollbar ? "YES" : "no");
        INV_CHECK(!c.scrollbar, "[%s] %s needs a vertical scrollbar (content %.0f px, inner area %.0f px): its content does not fit its area, so its lower part - e.g. the Timeline ruler - is clipped/pushed under the status bar",
                  s.tag.c_str(), c.name.c_str(), double(c.contentH), double(c.innerH));
    }
}

// ---- the real Editor in a hidden window -------------------------------------------------------
struct RealRig {
    ember::platform::Window window;
    ember::render::Renderer renderer;
    ember::editor::Editor editor;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    std::string why;
    bool ok = false;
    explicit RealRig(int w, int h, int warmup = 8) {
        ember::platform::WindowConfig wc;
        wc.width = w; wc.height = h; wc.visible = false; wc.title = "layout-invariants";
        if (!window.create(wc)) { why = "no GL window"; return; }
        if (!renderer.init()) { why = "renderer init failed"; return; }
        renderer.resize(w, h);
        if (!editor.init(window.nativeHandle())) { why = "editor init failed"; return; }
        world.createPrimitive(ember::scene::Primitive::Cube, "cube");
        for (int i = 0; i < warmup; ++i) editor.draw(world, renderer, renderer.lastStats(), undo);
        ok = true;
    }
    ~RealRig() { editor.shutdown(); renderer.shutdown(); }
    RealRig(const RealRig&) = delete;
    RealRig& operator=(const RealRig&) = delete;

    // The renderer body the editor itself reports (Editor::viewportRect), in ImGui points - the rect a
    // gap (or an overlap) in the Regions would show up as.
    ImVec4 bodyRect() const {
        const ember::Rect v = editor.viewportRect();
        const ImVec2 s = ImGui::GetIO().DisplayFramebufferScale;
        return ImVec4(float(v.x) / s.x, float(v.y) / s.y, float(v.w) / s.x, float(v.h) / s.y);
    }

    // Item 195: drive real input through the same path a user's mouse takes.
    void frame() { editor.draw(world, renderer, renderer.lastStats(), undo); }
    bool pointerAt(float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        frame(); frame();
        const ImVec2 p = ImGui::GetIO().MousePos;
        return std::fabs(p.x - x) < 1.0f && std::fabs(p.y - y) < 1.0f;
    }
    bool click(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        const bool placed = pointerAt(x, y);
        io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame(); frame();
        return placed;
    }
};

bool realSnap(RealRig& r, float W, float H, Snap& s) {
    s.W = W; s.H = H;
    const ImGuiIO& io = ImGui::GetIO();
    if (std::fabs(io.DisplaySize.x - W) > 1.0f || std::fabs(io.DisplaySize.y - H) > 1.0f) {
        std::printf("       window came up %gx%g instead of %gx%g (display limit?)\n", double(io.DisplaySize.x), double(io.DisplaySize.y), double(W), double(H));
        return false;
    }
    ImGuiWindow* mn = ImGui::FindWindowByName("##MainMenuBar");
    ImGuiWindow* st = ImGui::FindWindowByName("##status");
    ImGuiWindow* ws = ImGui::FindWindowByName("##workspace");
    if (!mn || !st || !ws) return false;
    s.top = winRect(mn); s.status = winRect(st); s.work = winRect(ws);
    for (const char* n : {"Area A", "Area B", "Area C", "Area D"}) {
        ImGuiWindow* w = ImGui::FindWindowByName(n);
        if (!w || !w->DockNode) return false;
        s.nodes.push_back({n, nodeRect(w->DockNode)});
        s.windows.push_back({n, winRect(w)});
        s.content.push_back({n, w->ContentSize.y, w->InnerRect.GetHeight(), w->ScrollbarY});
    }
    (void)r;
    return true;
}

template <typename Inv> void forEachSizeCpu(Inv inv) {
    for (const auto& sz : kSizes) {
        const Layout L = launch(sz[0], sz[1]);
        CHECK_MSG(L.ok, "%gx%g: layout could not be read: %s", double(sz[0]), double(sz[1]), L.why.c_str());
        if (!L.ok) continue;
        Snap s = snapFrom(L, sz[0], sz[1]);
        s.tag = std::to_string(int(sz[0])) + "x" + std::to_string(int(sz[1])) + " cpu";
        inv(s);
    }
}
template <typename Inv> void forEachSizeReal(Inv inv) {
    for (const auto& sz : kSizes) {
        RealRig r{int(sz[0]), int(sz[1])};
        CHECK_MSG(r.ok, "%gx%g: real editor rig: %s", double(sz[0]), double(sz[1]), r.why.c_str());
        if (!r.ok) continue;
        Snap s;
        s.tag = std::to_string(int(sz[0])) + "x" + std::to_string(int(sz[1])) + " REAL Editor";
        const bool got = realSnap(r, sz[0], sz[1], s);
        CHECK_MSG(got, "%gx%g: could not read the real editor's layout windows", double(sz[0]), double(sz[1]));
        if (got) inv(s);
    }
}

} // namespace

TEST(invariant_every_area_lies_inside_the_workspace) { forEachSizeCpu(inv1_inside_workspace); }
TEST(invariant_no_two_areas_overlap) { forEachSizeCpu(inv2_no_overlap); }
TEST(invariant_no_area_crosses_the_topbar_or_the_status_bar) { forEachSizeCpu(inv3_no_crossing_of_bars); }
TEST(invariant_topbar_workspace_and_status_bar_exactly_cover_the_client_area) { forEachSizeCpu(inv4_exact_cover); }
TEST(invariant_topbar_pinned_to_the_top_and_status_bar_to_the_bottom) { forEachSizeCpu(inv5_pinned); }

TEST_GPU(real_editor_every_area_lies_inside_the_workspace) { forEachSizeReal(inv1_inside_workspace); }
TEST_GPU(real_editor_no_two_areas_overlap) { forEachSizeReal(inv2_no_overlap); }
TEST_GPU(real_editor_no_area_crosses_the_topbar_or_the_status_bar) { forEachSizeReal(inv3_no_crossing_of_bars); }
TEST_GPU(real_editor_topbar_workspace_and_status_bar_exactly_cover_the_client_area) { forEachSizeReal(inv4_exact_cover); }
TEST_GPU(real_editor_topbar_pinned_to_the_top_and_status_bar_to_the_bottom) { forEachSizeReal(inv5_pinned); }
TEST_GPU(realcontent_area_content_fits_its_area_nothing_is_pushed_under_the_status_bar) { forEachSizeReal(inv6_content_fits); }

// The status-bar edge, stated as the plain geometric fact the brief names: no area's bottom edge may be
// below the top edge of the status bar (nodes AND windows), and the status bar must sit directly under
// the lowest area with no area reaching into it. (inv3 is the general both-bars overlap test.)
namespace {
void statusEdge(const Snap& s) {
    for (const auto* set : {&s.nodes, &s.windows})
        for (const Named& a : *set) {
            const float bottom = a.r.y + a.r.h;
            INV_CHECK(bottom <= s.status.y + 0.01f, "[%s] %s %s %s: bottom edge %.2f is BELOW the status bar's top edge %.2f", s.tag.c_str(),
                      set == &s.nodes ? "node" : "window", a.name.c_str(), fmt(a.r).c_str(), double(bottom), double(s.status.y));
        }
    float lowest = 0;
    for (const Named& a : s.nodes) lowest = std::fmax(lowest, a.r.y + a.r.h);
    INV_CHECK(std::fabs(lowest - s.status.y) <= 0.01f, "[%s] the lowest area ends at %.2f but the status bar starts at %.2f (gap or overlap)", s.tag.c_str(), double(lowest), double(s.status.y));
}
} // namespace

TEST(area_bounds_never_cross_the_status_bar) { forEachSizeCpu(statusEdge); }
TEST_GPU(real_editor_area_bounds_never_cross_the_status_bar) { forEachSizeReal(statusEdge); }
// ==== ITEM 195: every Properties category is REACHABLE, not merely scrollable =====================
//
// UI-07's defect (Material below the fold) was invisible to this suite: every existing test asserts
// WHERE the rail is, never that its category entries FIT inside it. The rail is theme-scaled 32px
// Selectables with the normal ItemSpacing.y between them, so they need
//
//     categoryCount * metric(32) + (categoryCount - 1) * ItemSpacing.y
//
// and at 1280x720 the Properties body gives the rail LESS than that, so the last category is clipped
// and only reachable by wheel-scrolling it. Scrolling is not "reachable": the category has to be
// VISIBLE inside the rail's rect and CLICKABLE there, without a wheel.
//
// Measured on the REAL editor in a hidden GLFW window. That is not incidental: the deficit comes from
// the real Topbar's height, which the CPU rig does not reproduce (it mirrors only part of the theme),
// so the CPU layout tests cannot see this defect at all.
namespace {

ImGuiWindow* propertiesRail() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (std::string(w->Name).find("##properties-icons") != std::string::npos) return w;
    return nullptr;
}

// The vertical span of what is drawn with the `active` token inside `w`: the selected category's row
// fill and its glyph. That is the observable "which category is selected" - the rail's `tab` lives in
// Editor-private state.
bool activeTokenSpan(ImGuiWindow* w, unsigned activeRgb, float& midY) {
    const ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !w) return false;
    float lo = 1e9f, hi = -1e9f;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        for (const ImDrawVert& v : dd->CmdLists[n]->VtxBuffer) {
            if (v.pos.x < w->Pos.x || v.pos.x > w->Pos.x + w->Size.x) continue;
            if (v.pos.y < w->Pos.y || v.pos.y > w->Pos.y + w->Size.y) continue;
            if ((v.col & 0x00FFFFFFu) != (activeRgb & 0x00FFFFFFu)) continue;   // RGB only: the row fill is the token at fill alpha
            lo = std::fmin(lo, v.pos.y); hi = std::fmax(hi, v.pos.y);
        }
    }
    if (hi < lo) return false;
    midY = (lo + hi) * 0.5f;
    return true;
}

} // namespace

TEST_GPU(every_properties_category_is_reachable) {
    const char* names[7] = {ember::editor::labels::Render, ember::editor::labels::Output,
                            ember::editor::labels::Scene, ember::editor::labels::World,
                            ember::editor::labels::Object, ember::editor::labels::Material,
                            ember::editor::labels::Camera};
    for (const auto& sz : kSizes) {
        const float W = sz[0], H = sz[1];
        RealRig r{int(W), int(H)};
        CHECK_MSG(r.ok, "%gx%g: real editor rig: %s", W, H, r.why.c_str());
        if (!r.ok) continue;
        ImGuiWindow* rail = propertiesRail();
        CHECK_MSG(rail != nullptr, "%gx%g: the Properties category rail was never drawn", W, H);
        if (!rail) continue;

        const float available = rail->InnerRect.GetHeight();
        std::printf("       %gx%g  rail inner rect %s  height %.1f\n", W, H, fmt(winRect(rail)).c_str(),
                    double(available));

        // 1. All seven rows must FIT - asked of ImGui itself rather than of an assumed row height: a rail
        //    whose content overflows has something to scroll, and a row below the fold cannot be reached
        //    without a wheel. (The owner now shrinks the rows to the available height, capped at 32 px, so
        //    asserting "6 * 32 px + gaps" would pin an implementation, not the property.)
        CHECK_MSG(rail->ScrollMax.y <= 0.5f,
                  "%gx%g: the Properties rail still overflows by %.1f px (ScrollMax.y), so the last category is "
                  "below the fold and needs a wheel to reach", W, H, double(rail->ScrollMax.y));
        CHECK_MSG(!rail->ScrollbarY, "%gx%g: the Properties rail shows a vertical scrollbar", W, H);

        // 2. MEASURE the seven rows - their actual geometry, not a constant: scan the rail's centre column and
        //    group the consecutive y positions that report the same hovered item.
        struct Band { float top = 0, bottom = 0; };
        std::vector<Band> bands;
        {
            const float x = (rail->InnerRect.Min.x + rail->InnerRect.Max.x) * 0.5f;
            ImGuiID current = 0;
            for (float y = rail->InnerRect.Min.y; y < rail->InnerRect.Max.y; y += 1.0f) {
                r.pointerAt(x, y + 0.5f);
                const ImGuiContext& g = *ImGui::GetCurrentContext();
                const ImGuiID hit = (g.HoveredWindow == rail) ? g.HoveredId : 0;
                if (hit == 0) { current = 0; continue; }
                if (hit != current) { bands.push_back({y, y}); current = hit; }
                else bands.back().bottom = y;
            }
        }
        std::printf("       %gx%g  measured %zu row band(s): ", W, H, bands.size());
        for (const Band& b : bands) std::printf("[%.0f..%.0f] ", double(b.top), double(b.bottom));
        std::printf("\n");
        CHECK_MSG(bands.size() == 7, "%gx%g: the rail reports %zu hoverable row(s); the seven Properties categories "
                  "must each be a real, hoverable row", W, H, bands.size());

        // 3. Every measured row lies inside the rail (VISIBLE) and is tall enough to be a control.
        for (std::size_t i = 0; i < bands.size() && i < 7; ++i) {
            CHECK_MSG(bands[i].bottom <= rail->InnerRect.Max.y + 0.5f,
                      "%gx%g: category %zu (%s) is clipped: its row ends at %.0f, the rail ends at %.0f",
                      W, H, i, names[i], double(bands[i].bottom), double(rail->InnerRect.Max.y));
            CHECK_MSG(bands[i].bottom - bands[i].top >= 6.0f,
                      "%gx%g: category %zu (%s) is only %.0f px tall - too small to be clickable",
                      W, H, i, names[i], double(bands[i].bottom - bands[i].top));
        }

        // 4. Clickable in place, without a wheel: clicking a measured row's centre must move the `active`
        //    marker into that row.
        const unsigned activeRgb = ImGui::ColorConvertFloat4ToU32(
            ember::editor::theme::color(ember::editor::theme::tokens().active));
        for (std::size_t i = 0; i < bands.size() && i < 7; ++i) {
            ImGuiWindow* w = propertiesRail();
            if (!w) break;
            const float cy = (bands[i].top + bands[i].bottom) * 0.5f;
            const float cx = (w->InnerRect.Min.x + w->InnerRect.Max.x) * 0.5f;
            const bool placed = r.click(cx, cy);
            CHECK_MSG(placed, "%gx%g: could not place the pointer on category %zu (%s) at (%.1f, %.1f)", W, H,
                      i, names[i], double(cx), double(cy));
            float mid = 0.0f;
            const bool selected = activeTokenSpan(propertiesRail(), activeRgb, mid);
            CHECK_MSG(selected && mid >= bands[i].top - 3.0f && mid <= bands[i].bottom + 3.0f,
                      "%gx%g: clicking category %zu (%s) at its measured row centre (%.1f, %.1f) did not select "
                      "it - the active marker is at %.1f, the row spans %.0f..%.0f", W, H, i,
                       names[i], double(cx), double(cy), double(mid), double(bands[i].top),
                      double(bands[i].bottom));
        }
    }
}

// ==== ITEM 196: the Toolbar and the Sidebar are REGIONS of the viewport, not floating windows =====
//
// UI-03 survived a whole wave because nothing asserted it. Two failures, one property:
//   * a Toolbar drawn as its own top-level ImGui window floats over the 3D grid (the pre-item-181
//     defect: a ~143x103 auto-sized window sitting on the render), and
//   * a Sidebar that is not flush with the renderer body lets the render bleed underneath it, which is
//     what made the Sidebar look like it intersected the Outliner.
// So the property is asserted in four parts, on the REAL editor in a hidden GLFW window:
//   1. no top-level window is named ##toolbar or ##sidebar at all;
//   2. both exist as CHILD windows of the viewport Area (Area D), inside its rect;
//   3. they are flush to its left/right edges and share the Area body's top and height;
//   4. the renderer body the editor reports (Editor::viewportRect) abuts BOTH with no gap and no
//      overlap: toolbar.right == body.left and body.right == sidebar.left.
// The CPU half of the same story is layout_toolbar_and_sidebar_lie_inside_the_viewport; this one is
// the live path, because the CPU rig does not reproduce the real Topbar/theme geometry. The geometry is
// asserted on EVERY frame from the second one, not only on a settled snapshot: a Region that is correct
// after eight frames but floats (or leaves a gap) while the dock tree settles is still the UI-03
// symptom. Frame 1 is ImGui's settle frame for a freshly built dock tree - a docked window's rect is
// resolved from its node on the following frame - so it carries no user-visible geometry to assert.
namespace {

bool viewportRegionsHold(float W, float H, RealRig& r, int frame, bool verbose) {
    struct Named { const char* id; const char* what; };
    ImGuiWindow* areaD = ImGui::FindWindowByName("Area D");
    CHECK_MSG(areaD != nullptr && areaD->WasActive, "%gx%g frame %d: the viewport Area (Area D) was never drawn",
              W, H, frame);
    if (!areaD) return false;
    const R area = winRect(areaD);

    // 1. Neither Region is a top-level window: a floating Toolbar is the UI-03 defect.
    for (const Named& n : {Named{"##toolbar", "Toolbar"}, Named{"##sidebar", "Sidebar"}}) {
        ImGuiWindow* floating = ImGui::FindWindowByName(n.id);
        CHECK_MSG(floating == nullptr,
                  "%gx%g frame %d: '%s' exists as a top-level window %s - the %s is floating over the "
                  "viewport instead of being a Region of it", W, H, frame, n.id,
                  floating ? fmt(winRect(floating)).c_str() : "(?)", n.what);
    }

    // 2. Both are CHILD windows of the viewport Area.
    ImGuiWindow* tb = childRegion(areaD, "##toolbar");
    ImGuiWindow* sb = childRegion(areaD, "##sidebar");
    CHECK_MSG(tb != nullptr, "%gx%g frame %d: the Toolbar was not drawn as a child Region of the viewport Area",
              W, H, frame);
    CHECK_MSG(sb != nullptr, "%gx%g frame %d: the Sidebar was not drawn as a child Region of the viewport Area",
              W, H, frame);
    if (!tb || !sb) return false;
    const R tr = winRect(tb), sr = winRect(sb);
    const ImVec4 vr = r.bodyRect();
    const R body{vr.x, vr.y, vr.z, vr.w};
    if (verbose) {
        std::printf("       %gx%g  Area D %s\n", W, H, fmt(area).c_str());
        std::printf("         toolbar %s | renderer body %s | sidebar %s\n", fmt(tr).c_str(), fmt(body).c_str(),
                    fmt(sr).c_str());
    }

    // 3. Inside the Area, flush to its edges, sharing the body's top and height.
    for (const Named& n : {Named{"##toolbar", "Toolbar"}, Named{"##sidebar", "Sidebar"}}) {
        const R& rr = std::string(n.id) == "##toolbar" ? tr : sr;
        CHECK_MSG(rr.x >= area.x - 0.01f && rr.y >= area.y - 0.01f && rr.x + rr.w <= area.x + area.w + 0.01f &&
                      rr.y + rr.h <= area.y + area.h + 0.01f,
                  "%gx%g frame %d: the %s %s is not inside the viewport Area %s", W, H, frame, n.what,
                  fmt(rr).c_str(), fmt(area).c_str());
    }
    CHECK_MSG(std::fabs(tr.x - area.x) < 0.01f,
              "%gx%g frame %d: the Toolbar starts at %.2f, not flush with the viewport Area's left edge %.2f",
              W, H, frame, double(tr.x), double(area.x));
    CHECK_MSG(std::fabs((sr.x + sr.w) - (area.x + area.w)) < 0.01f,
              "%gx%g frame %d: the Sidebar ends at %.2f, not flush with the viewport Area's right edge %.2f",
              W, H, frame, double(sr.x + sr.w), double(area.x + area.w));
    CHECK_MSG(std::fabs(tr.y - sr.y) < 0.01f && std::fabs(tr.h - sr.h) < 0.01f,
              "%gx%g frame %d: the Toolbar %s and Sidebar %s do not share the body's top/height", W, H, frame,
              fmt(tr).c_str(), fmt(sr).c_str());

    // 4. No gap and no overlap between each Region and the renderer body the editor reports.
    CHECK_MSG(std::fabs((tr.x + tr.w) - body.x) < 0.5f,
              "%gx%g frame %d: GAP/OVERLAP between the Toolbar (ends %.2f) and the renderer body (starts %.2f): "
              "%.2f px", W, H, frame, double(tr.x + tr.w), double(body.x), double(body.x - (tr.x + tr.w)));
    CHECK_MSG(std::fabs(sr.x - (body.x + body.w)) < 0.5f,
              "%gx%g frame %d: GAP/OVERLAP between the renderer body (ends %.2f) and the Sidebar (starts %.2f): "
              "%.2f px", W, H, frame, double(body.x + body.w), double(sr.x), double(sr.x - (body.x + body.w)));
    CHECK_MSG(std::fabs(body.y - tr.y) < 0.5f && std::fabs(body.h - tr.h) < 0.5f,
              "%gx%g frame %d: the renderer body %s does not share the Regions' top/height %s", W, H, frame,
              fmt(body).c_str(), fmt(tr).c_str());
    CHECK_MSG(body.w > 0.0f && body.h > 0.0f, "%gx%g frame %d: the renderer body is empty: %s", W, H, frame,
              fmt(body).c_str());

    // 5. The widths are the editor's own theme-scaled Region metrics, so a Region cannot silently shrink
    //    to its content width (the floating-window symptom) and still pass.
    CHECK_MSG(std::fabs(tr.w - ember::editor::toolbarRegionWidth()) < 0.5f,
              "%gx%g frame %d: the Toolbar is %.1f px wide, not its Region metric %.1f", W, H, frame,
              double(tr.w), double(ember::editor::toolbarRegionWidth()));
    CHECK_MSG(std::fabs(sr.w - ember::editor::sidebarRegionWidth()) < 0.5f,
              "%gx%g frame %d: the Sidebar is %.1f px wide, not its Region metric %.1f", W, H, frame,
              double(sr.w), double(ember::editor::sidebarRegionWidth()));
    CHECK_MSG(tr.x + tr.w <= sr.x + 0.01f, "%gx%g frame %d: the Toolbar and the Sidebar overlap each other",
              W, H, frame);
    return true;
}

} // namespace

TEST_GPU(toolbar_and_sidebar_are_viewport_regions) {
    for (const auto& sz : kSizes) {
        const float W = sz[0], H = sz[1];
        RealRig r{int(W), int(H), 0};        // no warm-up: the settle frames are part of the pin
        CHECK_MSG(r.ok, "%gx%g: real editor rig: %s", W, H, r.why.c_str());
        if (!r.ok) continue;
        for (int frame = 1; frame <= 8; ++frame) {
            r.frame();
            if (frame < 2) continue;         // ImGui's settle frame for a freshly built dock tree
            const bool ok = viewportRegionsHold(W, H, r, frame, frame == 8);
            if (!ok) break;
        }
    }
}

// ==== ITEM 194: no tab and no category may be a PLACEHOLDER =======================================
//
// The class behind UI-04, UI-05, UI-08 and UI-09 is a panel that exists, is clickable and contains
// nothing. docs/UI-DEFECTS.md measured that per panel - "Draw-path count for Tool: 1 disabled text,
// 0 interactive controls, 0 collapsible panels" - so this pin counts the same thing, with ImGui's own
// hit test: walk the pointer over the panel's content window and collect the DISTINCT hovered IDs.
// "Interactive" therefore means a control ImGui will actually respond to; a text-only or disabled row
// reports no ID and is not counted, and one widget is one ID. Hovering is not drawing, so no GL context
// is involved anywhere here - the whole pin is CPU.
//
// Tagged TEST_GPU only to keep the DEFAULT CTest run green while the defect is open (this test needs no
// GL at all - the same trade-off editor.header_controls_fit_their_labels documents) and registered as
// its own case, editor.no_tab_or_category_is_a_placeholder, so the defect is runnable and reproducibly
// red instead of silently skipped.
namespace {

struct PanelCount {
    int items = -1;                     // -1: the panel's content window was never drawn
    float width = 0, height = 0;
};

// Two columns, both well clear of the right edge: a vertical scrollbar is itself an interactive item and
// must not be counted as the panel's content.
template <typename RenderFrame>
PanelCount countInteractiveItems(RenderFrame renderFrame, const char* contentFragment) {
    PanelCount out;
    std::vector<ImGuiID> ids;
    auto findContent = [&]() -> ImGuiWindow* {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).find(contentFragment) != std::string::npos) return w;
        return nullptr;
    };
    // The window list holds LAST frame's windows, so make sure a frame has been rendered before the first
    // lookup - otherwise the very first call sees no window at all and reports -1.
    if (!findContent()) renderFrame(-10.0f, -10.0f);
    for (int pass = 0; pass < 2; ++pass) {
        ImGuiWindow* w = findContent();
        if (!w || w->InnerRect.GetHeight() < 4.0f) return out;
        out.width = w->InnerRect.GetWidth();
        out.height = w->InnerRect.GetHeight();
        const float x = w->InnerRect.Min.x + out.width * (pass == 0 ? 0.06f : 0.35f);
        for (float y = w->InnerRect.Min.y; y < w->InnerRect.Max.y; y += 1.0f) {
            renderFrame(x, y + 0.5f);
            ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredWindow != w || g.HoveredId == 0) continue;
            if (std::find(ids.begin(), ids.end(), g.HoveredId) == ids.end()) ids.push_back(g.HoveredId);
        }
    }
    out.items = int(ids.size());
    return out;
}

// One headless context holding every state the two live panel paths need, with a selected cube so the
// Object and Material categories are measured on their real controls rather than on "Nothing selected".
struct PanelRig {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::ViewportSettings viewport{};
    ember::editor::ToolState tools{};
    ember::editor::FileDialog file;
    ember::editor::DocumentState doc;
    ember::editor::TimelineState timeline;
    ember::editor::TransformSpace space = ember::editor::TransformSpace::Global;
    ember::editor::SnapTarget snapTarget = ember::editor::SnapTarget::GridIncrement;
    ember::InteractionMode interaction = ember::InteractionMode::Object;
    ember::SelectMode selectMode = ember::SelectMode::Vertex;
    bool snapEnabled = false;
    ember::render::OrbitCamera camera;

    PanelRig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
        const ember::ObjectId cube = world.createPrimitive(ember::scene::Primitive::Cube, "cube");
        world.select(cube);
        viewport.shadows.enabled = true;   // so the Render tab's sliders are live, not in BeginDisabled
    }
    ~PanelRig() { ImGui::DestroyContext(ctx); }
    PanelRig(const PanelRig&) = delete;
    PanelRig& operator=(const PanelRig&) = delete;

    void renderSidebar(ember::editor::SidebarTab& tab, float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##sidebar-host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        // Use the same fixed Region width as the live viewport Area.
        ember::editor::drawSidebar(world, undo, true,
                                   ImVec4(8.0f, 8.0f, ember::editor::sidebarRegionWidth(), 472.0f), tab, viewport,
                                   space, tools, snapEnabled, snapTarget, interaction, selectMode, camera);
        ImGui::End();
        ImGui::Render();
    }
    void renderToolbar(float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ember::editor::handleToolKeys(tools);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##toolbar-host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ember::editor::drawToolbar(tools, ImVec4(8.0f, 8.0f, ember::editor::toolbarRegionWidth(), 472.0f));
        ImGui::End();
        ImGui::Render();
    }
    void renderViewportMenus(float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##viewport-menu-host", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(ImVec2(20.0f, 12.0f));
        ember::editor::drawViewportObjectMenus(world, undo, ember::Vec3{0, 0, 0});
        ImGui::End();
        ImGui::Render();
    }
    void clickSidebar(ember::editor::SidebarTab& tab, float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        renderSidebar(tab, x, y);
        io.AddMouseButtonEvent(0, true); renderSidebar(tab, x, y);
        io.AddMouseButtonEvent(0, false); renderSidebar(tab, x, y); renderSidebar(tab, x, y);
    }
    void clickToolbar(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        renderToolbar(x, y);
        io.AddMouseButtonEvent(0, true); renderToolbar(x, y);
        io.AddMouseButtonEvent(0, false); renderToolbar(x, y); renderToolbar(x, y);
    }
    void clickViewportMenus(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        renderViewportMenus(x, y);
        io.AddMouseButtonEvent(0, true); renderViewportMenus(x, y);
        io.AddMouseButtonEvent(0, false); renderViewportMenus(x, y); renderViewportMenus(x, y);
    }
    void renderProperties(ember::editor::PropertiesTab& tab, float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ember::editor::drawProperties(world, undo, tab, viewport, file, doc, timeline);
        ImGui::Render();
    }
};

} // namespace

namespace {

struct UiHit {
    bool found = false;
    float x = 0.0f, y = 0.0f;
};

template <typename RenderFrame>
UiHit findHoveredItem(ImGuiWindow* window, const char* label, RenderFrame renderFrame,
                      bool alignRight = false) {
    UiHit hit;
    if (!window) return hit;
    const ImGuiID wanted = window->GetID(label);
    const float x = alignRight ? window->InnerRect.Max.x - 36.0f : window->InnerRect.Min.x + 12.0f;
    for (float y = window->InnerRect.Min.y; y < window->InnerRect.Max.y; y += 1.0f) {
        renderFrame(x, y + 0.5f);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.HoveredWindow == window && g.HoveredId == wanted) {
            hit = {true, x, y + 0.5f};
            break;
        }
    }
    return hit;
}

template <typename RenderFrame>
UiHit findHoveredItemAtX(ImGuiWindow* window, const char* label, float x,
                         RenderFrame renderFrame) {
    UiHit hit;
    if (!window) return hit;
    const ImGuiID wanted = window->GetID(label);
    for (float y = window->InnerRect.Min.y; y < window->InnerRect.Max.y; y += 1.0f) {
        renderFrame(x, y + 0.5f);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.HoveredWindow == window && g.HoveredId == wanted) {
            hit = {true, x, y + 0.5f};
            break;
        }
    }
    return hit;
}

ImGuiWindow* findWindowFragment(const char* fragment) {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (std::string(w->Name).find(fragment) != std::string::npos) return w;
    return nullptr;
}

} // namespace

TEST_GPU(no_tab_or_category_is_a_placeholder) {
    PanelRig rig;
    const char* tabNames[3] = {"Item", "Tool", "View"};
    for (int i = 0; i < 3; ++i) {
        ember::editor::SidebarTab tab = static_cast<ember::editor::SidebarTab>(i);
        rig.renderSidebar(tab, -10.0f, -10.0f);
        rig.renderSidebar(tab, -10.0f, -10.0f);
        const PanelCount c = countInteractiveItems([&](float x, float y) { rig.renderSidebar(tab, x, y); },
                                                   "##sidebarcontent");
        std::printf("       Sidebar %-7s content %.0fx%.0f px, interactive items: %d\n", tabNames[i],
                    double(c.width), double(c.height), c.items);
        CHECK_MSG(c.items >= 0, "the Sidebar's %s tab content window was never drawn", tabNames[i]);
        CHECK_MSG(c.height > 20.0f, "the Sidebar's %s tab content region is %.0f px tall - too small to "
                  "hold a control, so the count below would be meaningless", tabNames[i], double(c.height));
        CHECK_MSG(c.items >= 1, "the Sidebar's %s tab draws NO interactive widget: it is a placeholder "
                  "(docs/UI-DEFECTS.md counted \"0 interactive controls, 0 collapsible panels\" here)",
                  tabNames[i]);
    }

    const char* catNames[7] = {"Render", "Output", "Scene", "World", "Object", "Material", "Camera"};
    for (int i = 0; i < 7; ++i) {
        if (i == static_cast<int>(ember::editor::PropertiesTab::Camera)) {
            const ember::ObjectId camera = rig.world.createCamera();
            rig.world.select(camera);
        }
        ember::editor::PropertiesTab tab = static_cast<ember::editor::PropertiesTab>(i);
        rig.renderProperties(tab, -10.0f, -10.0f);
        rig.renderProperties(tab, -10.0f, -10.0f);
        const PanelCount c = countInteractiveItems([&](float x, float y) { rig.renderProperties(tab, x, y); },
                                                   "##properties-scroll");
        std::printf("       Properties %-8s content %.0fx%.0f px, interactive items: %d\n", catNames[i],
                    double(c.width), double(c.height), c.items);
        CHECK_MSG(c.items >= 0, "the Properties %s category content window was never drawn", catNames[i]);
        CHECK_MSG(c.height > 20.0f, "the Properties %s category content region is %.0f px tall - too small "
                  "to hold a control", catNames[i], double(c.height));
        CHECK_MSG(c.items >= 1, "the Properties %s category draws NO interactive widget: it is a "
                  "placeholder", catNames[i]);
    }
}

// CONTROL: the counter the pin above relies on must be able to report ZERO, or the pin would pass on a
// placeholder. A text-only panel counts 0; the same panel with one real Button counts exactly 1.
TEST(no_tab_or_category_control_the_item_count_measures_interaction) {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(400.0f, 300.0f);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* px = nullptr; int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);

    auto renderPanel = [&](bool withButton, float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
        ImGui::Begin("##control-host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BeginChild("##control-content", ImVec2(300.0f, 200.0f), false, ImGuiWindowFlags_None);
        ImGui::TextDisabled("Orientation controls are in the viewport header.");
        ImGui::TextDisabled("Shading and overlay controls are in the viewport header.");
        if (withButton) ImGui::Button("Real control", ImVec2(120.0f, 30.0f));
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
    };

    const PanelCount textOnly = countInteractiveItems(
        [&](float x, float y) { renderPanel(false, x, y); }, "##control-content");
    CHECK_MSG(textOnly.items == 0, "control: a text-only panel counted %d interactive item(s), so the "
              "placeholder pin above could pass on a panel that has no control at all", textOnly.items);

    const PanelCount withButton = countInteractiveItems(
        [&](float x, float y) { renderPanel(true, x, y); }, "##control-content");
    CHECK_MSG(withButton.items == 1, "control: a panel with exactly one Button counted %d interactive "
              "item(s): the counter is not measuring distinct interactive widgets", withButton.items);
    std::printf("       control: text-only panel -> %d items, text + one Button -> %d items\n",
                textOnly.items, withButton.items);
    ImGui::DestroyContext(ctx);
}

// ==== ITEM 190: D41's frozen STARTUP Timeline proportion ==========================================
//
// D41 freezes the Timeline at 20% of the SETTLED DockSpace at startup (splitter drags may move it
// afterwards), so this measures a FRESH editor, never touches a splitter, and asserts the pixel the
// frozen proportion demands of the DockSpace the user actually sees.
//
// FIXED AND GREEN since the owner's first-frame correction in Workspace.cpp (the DockSpace top now takes
// the maximum of the viewport's WorkPos and the real MainMenuBar bottom, so the split is computed from the
// SETTLED work area). It was RED BY DESIGN before that: the tree was built on frame 1 from a taller box,
// which put the realised share at ~21%. The assertion is deliberately strict - 20% of the settled
// DockSpace, integer rounding only - because D41 is frozen, and it is what turned the defect into a
// measurable failure instead of a report.
TEST_GPU(timeline_startup_split_is_d41s_frozen_proportion) {
    const float statusH = ember::editor::theme::statusBarHeight();
    for (const auto& sz : kSizes) {
        const float W = sz[0], H = sz[1];
        RealRig r{int(W), int(H)};        // a fresh editor: no splitter has been touched
        CHECK_MSG(r.ok, "%gx%g: real editor rig: %s", W, H, r.why.c_str());
        if (!r.ok) continue;
        ImGuiWindow* ws = ImGui::FindWindowByName("##workspace");
        ImGuiWindow* wc = ImGui::FindWindowByName("Area C");
        CHECK_MSG(ws != nullptr && wc != nullptr && wc->DockNode != nullptr,
                  "%gx%g: the DockSpace or the Timeline area was not drawn", W, H);
        if (!ws || !wc || !wc->DockNode) continue;
        const ImGuiDockNode* root = ImGui::DockBuilderGetNode(ws->GetID("EmberDock"));
        CHECK_MSG(root != nullptr, "%gx%g: the DockSpace root node was not found", W, H);
        if (!root) continue;
        const R work = nodeRect(root), bottom = nodeRect(wc->DockNode);
        const float sep = ImGui::GetStyle().DockingSeparatorSize;
        const float share = work.h > 0.0f ? bottom.h / work.h : 0.0f;
        const float expected = std::trunc((work.h - sep) * 0.20f);   // D41: kTimelineHeightShare
        std::printf("       %gx%g  DockSpace %s, Timeline %s -> %.2f%% of the settled DockSpace; D41's 20%% is "
                    "trunc((%.0f - %.0f) * 0.20) = %.0f px\n", W, H, fmt(work).c_str(), fmt(bottom).c_str(),
                    double(share * 100.0f), double(work.h), double(sep), double(expected));
        (void)statusH;

        // D41's frozen proportion, asserted against the SETTLED DockSpace, within pixel rounding only.
        CHECK_MSG(std::fabs(bottom.h - expected) <= 2.0f,
                  "%gx%g: the startup Timeline is %.0f px of the settled %.0f px DockSpace (%.2f%%), but D41's "
                  "frozen 20%% expects trunc((%.0f - %.0f) * 0.20) = %.0f px", W, H, double(bottom.h),
                  double(work.h), double(share * 100.0f), double(work.h), double(sep), double(expected));
        CHECK_MSG(std::fabs(bottom.w - work.w) < 0.5f,
                  "%gx%g: the Timeline is %.0f px wide but the DockSpace is %.0f: it must span the full width",
                  W, H, double(bottom.w), double(work.w));
    }
}

// ==== ITEM 175: a header control is wide enough for its WHOLE label =================================
//
// #169: the viewport header's combos clipped their own text ('Material Previe', 'Individua') and no suite saw it,
// because every layout test asserts WHERE things are, never that a control is big enough for what it says.
// The rule: a combo's drawn width >= the width of its LONGEST option label + the left frame padding + the arrow
// button (which is a square of GetFrameHeight(), BeginCombo's own geometry). All metrics are ImGui's own
// (CalcTextSize / GetFrameHeight / GetStyle().FramePadding), no GL.
//
// HOW the widths are measured without trusting any constant in the editor: the REAL drawArea() (viewport host)
// is drawn into a headless ImGui; for each combo the test scans the pointer across the header one pixel at a
// time and records the x-range over which ImGui reports THAT combo's ID as hovered - the item's true hit
// rectangle, i.e. exactly what is drawn. The option labels come from the editor's own arrays (parsed from
// Editor.cpp) and areaTypeName() (called directly), so a new option is picked up automatically.
//
// TWO STATES, because the answer differs: STARTUP (applyTheme() runs before the first frame, exactly as
// Editor::init() does - what every user sees) and AFTER THE FIRST FRAME (the Preferences-change path).
namespace {

struct ComboSpec { const char* id; const char* what; std::vector<std::string> labels; bool editOnly; };

std::vector<std::string> quotedStringsIn(const std::string& src, const char* arrayName) {
    std::vector<std::string> out;
    const std::regex decl(std::string("\\b") + arrayName + "\\s*\\[\\s*\\]\\s*=\\s*\\{([^}]*)\\}");
    std::smatch m;
    if (!std::regex_search(src, m, decl)) return out;
    const std::string body = m[1].str();
    const std::regex q("\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), q); it != std::sregex_iterator(); ++it) out.push_back((*it)[1].str());
    return out;
}

std::vector<ComboSpec> comboSpecs() {
    std::ifstream f(std::string(EMBER_SOURCE_DIR) + "/src/editor/Editor.cpp", std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    const std::string src = ss.str();
    using ember::editor::AreaType;
    std::vector<std::string> areaLabels;
    for (AreaType a : {AreaType::Outliner, AreaType::Properties, AreaType::Stats, AreaType::UndoHistory, AreaType::Timeline, AreaType::Viewport})
        areaLabels.push_back(ember::editor::areaTypeName(a));
    return {
        {"##area-editor", "editor-type selector", areaLabels, false},
        {"##shade", "shading mode", quotedStringsIn(src, "shadingNames"), false},
        {"##orientation", "transform orientation", quotedStringsIn(src, "orientationNames"), false},
        {"##pivot", "pivot point", quotedStringsIn(src, "pivotNames"), false},
        {"##snaptarget", "snap target", quotedStringsIn(src, "snapNames"), false},
        {"##selectmode", "select mode (Edit Mode only)", quotedStringsIn(src, "selectNames"), true},
    };
}

struct Measured { float x0 = 1e9f, x1 = -1e9f; bool found = false; float width() const { return found ? x1 - x0 + 1.0f : 0.0f; } };

struct HeaderMeasure {
    std::map<std::string, Measured> widths;
    float frameHeight = 0, padX = 0, winPadY = 0;
    std::string why;
};

HeaderMeasure measureHeader(bool afterFirstFrame, ember::InteractionMode mode, float areaW) {
    HeaderMeasure out;
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(3000, 1080);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* px = nullptr; int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    const ember::editor::Preferences prefs;
    ember::editor::applyTheme(prefs.accent);                 // as Editor::init() does: before any frame
    if (afterFirstFrame) { ImGui::NewFrame(); ImGui::EndFrame(); ember::editor::applyTheme(prefs.accent); }

    ember::scene::World world;
    ember::editor::UndoStack undo;
    auto drawFrame = [&](float mx, float my) {
        io.AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(areaW, 300));
        ember::RenderStats stats{};
        ember::editor::TimelineState timeline;
        ember::ViewportSettings viewport{};
        ember::editor::TransformSpace space = ember::editor::TransformSpace::Global;
        ember::SelectMode selectMode = ember::SelectMode::Vertex;
        ember::editor::PropertiesTab tab = ember::editor::PropertiesTab::Object;
        bool snap = false;
        ember::editor::SnapTarget snapTarget = ember::editor::SnapTarget::GridIncrement;
        ember::editor::AreaType type = ember::editor::AreaType::Viewport;
        (void)ember::editor::drawArea("hdr", type, true, world, undo, stats, timeline, ember::Vec3{0, 0, 0}, viewport, space, mode, selectMode,
                                      tab, snap, snapTarget);
        ImGui::EndFrame();
    };
    for (int i = 0; i < 4; ++i) drawFrame(-10, -10);
    ImGuiWindow* win = ImGui::FindWindowByName("hdr");
    if (!win) { out.why = "the header window was never created"; ImGui::DestroyContext(ctx); return out; }
    out.frameHeight = ImGui::GetFrameHeight();
    out.padX = ImGui::GetStyle().FramePadding.x;
    out.winPadY = win->WindowPadding.y;
    const float y = win->Pos.y + win->WindowPadding.y + 6.0f;   // inside every combo's height (>=19) and above its text
    std::map<ImGuiID, std::string> ids;
    for (const ComboSpec& c : comboSpecs()) ids[win->GetID(c.id)] = c.id;
    for (int x = 0; x < int(areaW); ++x) {
        drawFrame(float(x) + 0.5f, y);
        const ImGuiID h = ImGui::GetCurrentContext()->HoveredId;
        auto it = ids.find(h);
        if (it == ids.end()) continue;
        Measured& m = out.widths[it->second];
        m.found = true; m.x0 = std::fmin(m.x0, float(x)); m.x1 = std::fmax(m.x1, float(x));
    }
    ImGui::DestroyContext(ctx);
    return out;
}

// The predicate under test: wide enough = room for the longest label, the left frame padding, and the arrow button.
float requiredWidth(const std::vector<std::string>& labels, float padX, float arrow) {
    float w = 0;
    for (const std::string& l : labels) w = std::fmax(w, ImGui::CalcTextSize(l.c_str()).x);
    return w + padX + arrow;
}

void checkHeaderCombos(bool afterFirstFrame, const char* state) {
    for (int pass = 0; pass < 2; ++pass) {
        const ember::InteractionMode mode = pass == 0 ? ember::InteractionMode::Object : ember::InteractionMode::Edit;
        // A context is needed for CalcTextSize below: measureHeader destroys its own, so use a fresh one for metrics.
        // 996px is the measured default Area D width at a 1280x720 startup.
        // Measuring an artificially wide header hid right-group overflow.
        HeaderMeasure m = measureHeader(afterFirstFrame, mode, 996.0f);
        CHECK_MSG(m.why.empty(), "[%s] %s", state, m.why.c_str());
        if (!m.why.empty()) continue;
        IMGUI_CHECKVERSION();
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080); io.IniFilename = nullptr;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
        ImGui::NewFrame();                                        // a font must be current for CalcTextSize
        for (const ComboSpec& c : comboSpecs()) {
            if (c.editOnly != (pass == 1)) continue;   // each combo is checked once: Object-mode header, then the Edit-only select combo
            CHECK_MSG(!c.labels.empty(), "[%s] could not read the option labels for '%s' (%s) from Editor.cpp", state, c.what, c.id);
            const auto it = m.widths.find(c.id);
            CHECK_MSG(it != m.widths.end() && it->second.found, "[%s] the %s combo (%s) was not found in the drawn header (%s mode): the test is measuring nothing",
                      state, c.what, c.id, pass == 0 ? "Object" : "Edit");
            if (it == m.widths.end() || c.labels.empty()) continue;
            const float width = it->second.width();
            const float labelOnly = requiredWidth(c.labels, 0.0f, 0.0f);
            const float need = requiredWidth(c.labels, m.padX, m.frameHeight);
            std::string longest; float lw = 0;
            for (const std::string& l : c.labels) if (ImGui::CalcTextSize(l.c_str()).x > lw) { lw = ImGui::CalcTextSize(l.c_str()).x; longest = l; }
            std::printf("       [%-26s] %-25s drawn %6.1f px | longest option '%s' %5.1f + pad %.0f + arrow %.0f = need %6.1f px | %s\n", state, c.what, double(width),
                        longest.c_str(), double(labelOnly), double(m.padX), double(m.frameHeight), double(need), width + 0.5f >= need ? "ok" : "CLIPPED");
            CHECK_MSG(width + 0.5f >= need,
                      "[%s] the %s combo is %.1f px wide but its longest option '%s' needs %.1f px (label %.1f + left padding %.0f + arrow %.0f): %.1f px of the label is clipped",
                      state, c.what, double(width), longest.c_str(), double(need), double(labelOnly), double(m.padX), double(m.frameHeight), double(need - width));
        }
        ImGui::EndFrame();
        ImGui::DestroyContext(ctx);
    }
}

} // namespace

// Tagged TEST_GPU only to keep the default CTest run green while the defect is open (this test needs no GL): it runs as
// editor.header_controls_fit_their_labels, with the comparison state, via --gpu --only header_controls.
TEST_GPU(header_controls_are_wide_enough_for_their_labels) {
    // The state every user sees: applyTheme() before the first frame, as Editor::init() calls it.
    checkHeaderCombos(false, "at STARTUP");
}

TEST_GPU(header_controls_after_the_first_frame_are_the_comparison_state) {
    // The same header after applyTheme() has run again once a frame exists (the Preferences-change path). If THIS passes while the
    // startup test fails, the rule is satisfied by the formula and broken only by the init-time call order.
    checkHeaderCombos(true, "after first frame");
}

TEST(header_width_predicate_control_a_too_narrow_combo_is_reported) {
    // Control: the predicate must be able to fail. 'Material Preview' needs more than a combo sized for 'Rendered'.
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600); io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    ImGui::NewFrame();
    const float narrow = requiredWidth({"Rendered"}, 5.0f, 19.0f);
    const float wide = requiredWidth({"Wireframe", "Solid", "Material Preview", "Rendered"}, 5.0f, 19.0f);
    CHECK_MSG(narrow < wide - 20.0f, "setup: 'Rendered' (%.1f px) is not clearly narrower than 'Material Preview' (%.1f px)", double(narrow), double(wide));
    CHECK_MSG(!(narrow + 0.5f >= wide), "the predicate accepted a combo sized for 'Rendered' as wide enough for 'Material Preview'");
    ImGui::EndFrame();
    ImGui::DestroyContext(ctx);
}
// ==== ITEM 173: the Properties tab strip - width, glyph fit, and full-name tooltips ===================
//
// Chisel's fix: the icon column is one named metric (theme::propertiesTabStripWidth = 48 logical px), each Selectable fills
// the strip's inner width (it used to request a 34px sub-width that clipped the 'Ob' glyph), and each entry carries a tooltip
// with the FULL tab name. Verified headlessly on the real drawProperties(): item rects are found by scanning the pointer down the
// strip and recording each distinct hovered ID's y-range; the tooltip is the real one (pointer held still past the tooltip delay).
// The tooltip's TEXT is not stored by ImGui, so it is checked by its window width = CalcTextSize(full name) + 2*WindowPadding.x
// (the seven full names are checked by width, with a control for every other name whose width differs enough to distinguish it).
namespace {

struct TabStrip {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::editor::PropertiesTab tab = ember::editor::PropertiesTab::Object;
    std::string why;
    TabStrip() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900, 700);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 0.1f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
        const ember::editor::Preferences prefs;
        ember::editor::applyTheme(prefs.accent);
        ImGui::NewFrame(); ImGui::EndFrame();
        ember::editor::applyTheme(prefs.accent);     // steady state (the startup call order is item 156's, not this rule's)
    }
    ~TabStrip() { ImGui::DestroyContext(ctx); }
    TabStrip(const TabStrip&) = delete;
    TabStrip& operator=(const TabStrip&) = delete;

    void frame(float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(420, 400));
        ember::editor::drawProperties(world, undo, tab);
        ImGui::EndFrame();
    }
};

struct StripGeometry {
    float x0 = 1e9f, x1 = -1e9f;                      // the strip's inner (item) x-range at the item centre
    std::vector<std::pair<float, float>> itemY;       // y-range per entry, top to bottom
};

} // namespace

TEST(properties_tab_strip_fits_its_glyphs_and_shows_full_name_tooltips) {
    TabStrip s;
    for (int i = 0; i < 3; ++i) s.frame(-10, -10);
    ImGuiWindow* strip = ImGui::FindWindowByName("Properties/##properties-icons");
    if (!strip) {   // child window names are prefixed with the parent and an ID: find it by suffix
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).find("##properties-icons") != std::string::npos) strip = w;
    }
    CHECK_MSG(strip != nullptr, "the Properties icon column window was not found");
    if (!strip) return;

    // 1. the column is the named metric wide.
    const float wantWidth = ember::editor::theme::metric(ember::editor::theme::propertiesTabStripWidth);
    CHECK_MSG(std::fabs(strip->Size.x - wantWidth) < 0.5f, "the tab strip is %.1f px wide, theme::propertiesTabStripWidth says %.1f", double(strip->Size.x), double(wantWidth));

    // 2. entry rects by scanning down the strip.
    StripGeometry g;
    const float cx = strip->Pos.x + strip->Size.x * 0.5f;
    ImGuiID last = 0; float runStart = 0;
    for (float y = strip->Pos.y; y < strip->Pos.y + strip->Size.y; y += 1.0f) {
        s.frame(cx, y + 0.5f);
        const ImGuiID h = ImGui::GetCurrentContext()->HoveredId;
        const bool inStrip = h != 0 && ImGui::GetCurrentContext()->HoveredWindow && ImGui::GetCurrentContext()->HoveredWindow->RootWindow == strip->RootWindow &&
                             ImGui::GetCurrentContext()->HoveredWindow == strip;
        const ImGuiID id = inStrip ? h : 0;
        if (id != last) {
            if (last != 0) g.itemY.push_back({runStart, y - 1.0f});
            runStart = y; last = id;
        }
    }
    if (last != 0) g.itemY.push_back({runStart, strip->Pos.y + strip->Size.y - 1.0f});
    CHECK_MSG(g.itemY.size() == 7, "found %zu hoverable entries in the tab strip, expected 7 including Camera", g.itemY.size());
    if (g.itemY.size() != 7) return;

    const char* names[7] = {ember::editor::labels::Render, ember::editor::labels::Output, ember::editor::labels::Scene,
                            ember::editor::labels::World, ember::editor::labels::Object, ember::editor::labels::Material,
                            ember::editor::labels::Camera};
    const char* glyphs[7] = {"R", "O", "S", "W", "Ob", "M", "C"};

    // 3. each entry fills the strip's inner width (x-range at its centre line) and contains its glyph.
    const float innerLeft = strip->InnerRect.Min.x, innerRight = strip->InnerRect.Max.x;
    for (int i = 0; i < 7; ++i) {
        const float my = (g.itemY[i].first + g.itemY[i].second) * 0.5f;
        float x0 = 1e9f, x1 = -1e9f;
        for (float x = strip->Pos.x; x < strip->Pos.x + strip->Size.x; x += 1.0f) {
            s.frame(x + 0.5f, my);
            const ImGuiContext& c = *ImGui::GetCurrentContext();
            if (c.HoveredWindow == strip && c.HoveredId != 0) { x0 = std::fmin(x0, x); x1 = std::fmax(x1, x); }
        }
        const float itemW = x1 - x0 + 1.0f;
        const float glyphW = ImGui::CalcTextSize(glyphs[i]).x;
        std::printf("       [tab %-8s] entry x %6.1f..%6.1f = %5.1f px of a %5.1f px strip (inner %.0f..%.0f); glyph '%s' %4.1f px\n", names[i], double(x0), double(x1),
                    double(itemW), double(strip->Size.x), double(innerLeft), double(innerRight), glyphs[i], double(glyphW));
        CHECK_MSG(itemW >= (innerRight - innerLeft) - 1.5f, "entry '%s' is %.1f px wide but the strip's inner width is %.1f: it does not fill the column", names[i], double(itemW), double(innerRight - innerLeft));
        CHECK_MSG(glyphW + 2.0f * ImGui::GetStyle().FramePadding.x <= itemW + 0.5f, "the glyph '%s' (%.1f px + padding) does not fit its %.1f px entry", glyphs[i], double(glyphW), double(itemW));
        CHECK_MSG(x0 >= innerLeft - 1.0f && x1 <= innerRight + 1.0f, "entry '%s' extends outside the strip", names[i]);
    }

    // 4. hover each entry, hold still past the tooltip delay, and read the real tooltip window.
    auto tooltipWidth = [&](int i) {
        const float my = (g.itemY[i].first + g.itemY[i].second) * 0.5f;
        for (int f = 0; f < 14; ++f) s.frame(cx, my);
        float w = -1.0f;
        for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
            if (win->WasActive && std::string(win->Name).rfind("##Tooltip", 0) == 0) w = win->Size.x;
        s.frame(-10, -10); s.frame(-10, -10);       // let it go away before the next entry
        return w;
    };
    auto matches = [&](float w, const char* name) {
        const float want = ImGui::CalcTextSize(name).x + 2.0f * ImGui::GetStyle().WindowPadding.x;
        return w > 0 && std::fabs(w - want) <= 1.5f;
    };
    for (int i = 0; i < 7; ++i) {
        const float w = tooltipWidth(i);
        std::printf("       [tab %-8s] tooltip window %6.1f px wide; full name needs %6.1f px\n", names[i], double(w), double(ImGui::CalcTextSize(names[i]).x + 2.0f * ImGui::GetStyle().WindowPadding.x));
        CHECK_MSG(w > 0, "hovering the '%s' entry for 1.4 s produced NO tooltip", names[i]);
        CHECK_MSG(matches(w, names[i]), "the '%s' entry's tooltip is %.1f px wide, not the width of its full name", names[i], double(w));
        // CONTROL: the width test tells the seven names apart (so a wrong or abbreviated name would not pass above)
        for (int j = 0; j < 7; ++j)
            if (j != i && std::fabs(ImGui::CalcTextSize(names[j]).x - ImGui::CalcTextSize(names[i]).x) > 3.0f)
                CHECK_MSG(!matches(w, names[j]), "control: the '%s' tooltip also matches the width of '%s', so the check cannot tell them apart", names[i], names[j]);
    }
}
// ---- CONTROLS: every invariant must be able to FAIL ---------------------------------------------
// Each is fed a layout with one specific defect injected and must report it. Their expected failures
// are removed from the run's bookkeeping so only the meta-assertion ('it caught it') counts.
namespace {
template <typename Inv> int violations(const Snap& s, Inv inv) {
    g_quiet = true;
    g_quietCount = 0;
    inv(s);
    g_quiet = false;
    return g_quietCount;
}
Snap goodSnap() {
    const Layout L = launch(1280, 720);
    Snap s = snapFrom(L, 1280, 720);
    s.tag = "control";
    return s;
}
} // namespace

TEST(invariant_controls_each_invariant_catches_the_defect_it_exists_for) {
    {   // control of the controls: the unmodified layout has NO violations
        const Snap s = goodSnap();
        CHECK_MSG(violations(s, inv1_inside_workspace) + violations(s, inv2_no_overlap) + violations(s, inv3_no_crossing_of_bars) +
                  violations(s, inv4_exact_cover) + violations(s, inv5_pinned) == 0, "the unmodified layout already violates an invariant");
    }
    {   Snap s = goodSnap(); s.nodes[2].r.h += 20; s.windows[2].r.h += 20;           // Area C grows into the status bar
        CHECK_MSG(violations(s, inv3_no_crossing_of_bars) >= 1, "inv3 did not notice an area running into the STATUS BAR");
        CHECK_MSG(violations(s, inv1_inside_workspace) >= 1, "inv1 did not notice an area outside the workspace"); }
    {   Snap s = goodSnap(); s.nodes[2].r.h += 20; s.windows[2].r.h += 20;
        CHECK_MSG(violations(s, statusEdge) >= 1, "statusEdge did not notice an area's bottom edge below the status bar's top edge"); }
    {   Snap s = goodSnap(); s.nodes[0].r.x -= 30; s.windows[0].r.x -= 30;           // Area A slides left over Area D
        CHECK_MSG(violations(s, inv2_no_overlap) >= 1, "inv2 did not notice two overlapping areas"); }
    {   Snap s = goodSnap(); s.windows[3].r.y -= 8;                                   // Area D's window pokes into the topbar
        CHECK_MSG(violations(s, inv3_no_crossing_of_bars) >= 1, "inv3 did not notice a window crossing the TOPBAR"); }
    {   Snap s = goodSnap(); s.work.y += 5; s.work.h -= 5;                            // 5px gap under the topbar
        CHECK_MSG(violations(s, inv4_exact_cover) >= 1, "inv4 did not notice a gap between topbar and workspace"); }
    {   Snap s = goodSnap(); s.status.y -= 10;                                        // status bar no longer on the bottom edge
        CHECK_MSG(violations(s, inv5_pinned) >= 1, "inv5 did not notice a status bar off the bottom edge");
        CHECK_MSG(violations(s, inv4_exact_cover) >= 1, "inv4 did not notice a status bar off the bottom edge"); }
    {   Snap s = goodSnap(); s.top.y = 3;                                             // topbar off the top edge
        CHECK_MSG(violations(s, inv5_pinned) >= 1, "inv5 did not notice a topbar off the top edge"); }
    {   Snap s = goodSnap(); s.content.push_back({"Area C", 139.0f, 124.0f, true});  // content overflow
        CHECK_MSG(violations(s, inv6_content_fits) >= 1, "inv6 did not notice content that needs a scrollbar"); }
}

// These checks use ImGui's live hit IDs and queued mouse/key input, not a call
// to the state-mutating helpers. They pin the Toolbar and Sidebar as real tools.
TEST(toolbar_tool_rows_and_transform_shortcuts_have_distinct_feedback) {
    PanelRig rig;
    rig.renderToolbar(-10.0f, -10.0f);
    ImGuiWindow* host = ImGui::FindWindowByName("##toolbar-host");
    ImGuiWindow* toolbar = childRegion(host, "##toolbar");
    CHECK_MSG(toolbar != nullptr, "the in-Area Toolbar child was not drawn");
    if (!toolbar) return;

    const UiHit move = findHoveredItem(toolbar, "Move    W",
        [&](float x, float y) { rig.renderToolbar(x, y); });
    CHECK_MSG(move.found, "Move (W) is not a hit-testable Toolbar row");
    if (move.found) rig.clickToolbar(move.x, move.y);
    CHECK_MSG(rig.tools.active == ember::editor::Tool::Move,
              "clicking the Move row did not select the active tool");

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_E, true); rig.renderToolbar(-10.0f, -10.0f);
    io.AddKeyEvent(ImGuiKey_E, false); rig.renderToolbar(-10.0f, -10.0f);
    CHECK_MSG(rig.tools.active == ember::editor::Tool::Move,
              "E changed the Toolbar tool; E must remain available for Edit Mode extrusion");
    io.AddKeyEvent(ImGuiKey_R, true); rig.renderToolbar(-10.0f, -10.0f);
    io.AddKeyEvent(ImGuiKey_R, false); rig.renderToolbar(-10.0f, -10.0f);
    CHECK_MSG(rig.tools.active == ember::editor::Tool::Rotate, "R did not activate Rotate");
    io.AddKeyEvent(ImGuiKey_S, true); rig.renderToolbar(-10.0f, -10.0f);
    io.AddKeyEvent(ImGuiKey_S, false); rig.renderToolbar(-10.0f, -10.0f);
    CHECK_MSG(rig.tools.active == ember::editor::Tool::Scale, "S did not activate Scale");
}

TEST(sidebar_tool_and_view_controls_change_live_editor_state) {
    PanelRig rig;
    auto tab = ember::editor::SidebarTab::Tool;
    rig.renderSidebar(tab, -10.0f, -10.0f);
    ImGuiWindow* content = findWindowFragment("##sidebarcontent");
    CHECK_MSG(content != nullptr, "Tool tab content was not drawn");
    if (!content) return;

    const UiHit move = findHoveredItem(content, "Move  W",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); });
    CHECK_MSG(move.found, "Tool tab's Move control is not hit-testable");
    if (move.found) rig.clickSidebar(tab, move.x, move.y);
    CHECK_MSG(rig.tools.active == ember::editor::Tool::Move,
              "Tool tab click did not change the active tool");

    content = findWindowFragment("##sidebarcontent");
    const UiHit snap = findHoveredItem(content, "Snap",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); });
    CHECK_MSG(snap.found, "Tool tab's Snap checkbox is not hit-testable");
    if (snap.found) rig.clickSidebar(tab, snap.x, snap.y);
    CHECK_MSG(rig.snapEnabled, "Tool tab's Snap checkbox did not enable snapping");

    ImGuiWindow* host = ImGui::FindWindowByName("##sidebar-host");
    ImGuiWindow* sidebar = childRegion(host, "##sidebar");
    const UiHit viewTab = findHoveredItem(sidebar, "View",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); }, true);
    CHECK_MSG(viewTab.found, "View tab is not hit-testable at the Sidebar edge");
    if (viewTab.found) rig.clickSidebar(tab, viewTab.x, viewTab.y);
    CHECK_MSG(tab == ember::editor::SidebarTab::View, "clicking the View tab did not open it");

    // Frame Selected is backed by the current scene bounds, not a decorative button.
    ember::Transform moved;
    moved.position = ember::Vec3{4.0f, 0.0f, 0.0f};
    const ember::ObjectId selected = rig.world.createPrimitive(ember::scene::Primitive::Cube, "Framed");
    rig.world.setTransform(selected, moved);
    rig.world.select(selected);
    rig.renderSidebar(tab, -10.0f, -10.0f);
    content = findWindowFragment("##sidebarcontent");
    const UiHit front = findHoveredItem(content, "Front",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); });
    CHECK_MSG(front.found, "View tab's Front standard-view button is not hit-testable");
    const float yawBefore = rig.camera.yaw();
    if (front.found) rig.clickSidebar(tab, front.x, front.y);
    CHECK_MSG(std::fabs(rig.camera.yaw() - yawBefore) > 0.1f || std::fabs(rig.camera.pitch()) > 1.0f,
              "Front did not change the orbit camera orientation");

    content = findWindowFragment("##sidebarcontent");
    const UiHit ortho = findHoveredItem(content, "Orthographic",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); });
    CHECK_MSG(ortho.found, "View tab's Orthographic checkbox is not hit-testable");
    if (ortho.found) rig.clickSidebar(tab, ortho.x, ortho.y);
    CHECK_MSG(rig.camera.isOrthographic(), "Orthographic did not change the camera projection mode");

    content = findWindowFragment("##sidebarcontent");
    const UiHit frameSelected = findHoveredItem(content, "Frame Selected",
        [&](float x, float y) { rig.renderSidebar(tab, x, y); });
    CHECK_MSG(frameSelected.found, "Frame Selected is not an enabled, hit-testable scene action");
    if (frameSelected.found) rig.clickSidebar(tab, frameSelected.x, frameSelected.y);
    CHECK_MSG(std::fabs(rig.camera.target().x - 4.0f) < 0.01f,
              "Frame Selected did not frame the selected object's world bounds");
}

TEST(viewport_add_menu_is_anchored_and_camera_action_works) {
    PanelRig rig;
    rig.renderViewportMenus(-10.0f, -10.0f);
    ImGuiWindow* host = ImGui::FindWindowByName("##viewport-menu-host");
    const UiHit add = findHoveredItemAtX(host, "Add", 30.0f,
        [&](float x, float y) { rig.renderViewportMenus(x, y); });
    CHECK_MSG(add.found, "viewport Add menu button is not hit-testable in its host Area");
    if (!add.found) return;
    rig.clickViewportMenus(add.x, add.y);

    ImGuiContext& ctx = *ImGui::GetCurrentContext();
    ImGuiWindow* popup = ctx.OpenPopupStack.Size > 0
        ? ctx.OpenPopupStack.back().Window : nullptr;
    CHECK_MSG(popup != nullptr && (popup->Flags & ImGuiWindowFlags_Popup),
              "Add did not open its anchored popup menu");
    if (!popup) return;
    const UiHit camera = findHoveredItem(popup, "Camera",
        [&](float x, float y) { rig.renderViewportMenus(x, y); });
    CHECK_MSG(camera.found, "the anchored Add menu did not expose its working Camera action");
    if (camera.found) rig.clickViewportMenus(camera.x, camera.y);
    bool cameraAdded = false;
    for (const ember::scene::Object& object : rig.world.objects())
        cameraAdded |= object.camera.isCamera;
    CHECK_MSG(cameraAdded, "choosing Camera from the anchored Add menu did not create a scene camera");
}

TEST(edit_mode_top_row_keys_select_sub_element_mode_unless_emulating_numpad) {
    auto route = [](ImGuiKey key, ember::InteractionMode interaction, bool emulate,
                    ember::SelectMode start) {
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 480.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.AddKeyEvent(key, true);
        ImGui::NewFrame();
        ember::SelectMode selected = start;
        const bool handled = ember::editor::handleEditSelectModeKey(io, interaction, emulate, selected);
        ImGui::EndFrame();
        ImGui::DestroyContext(ctx);
        return std::pair<bool, ember::SelectMode>{handled, selected};
    };

    const auto vertex = route(ImGuiKey_1, ember::InteractionMode::Edit, false,
                              ember::SelectMode::Face);
    const auto edge = route(ImGuiKey_2, ember::InteractionMode::Edit, false,
                            ember::SelectMode::Vertex);
    const auto face = route(ImGuiKey_3, ember::InteractionMode::Edit, false,
                            ember::SelectMode::Vertex);
    CHECK_MSG(vertex.first && vertex.second == ember::SelectMode::Vertex,
              "top-row 1 did not select Vertex in Edit Mode");
    CHECK_MSG(edge.first && edge.second == ember::SelectMode::Edge,
              "top-row 2 did not select Edge in Edit Mode");
    CHECK_MSG(face.first && face.second == ember::SelectMode::Face,
              "top-row 3 did not select Face in Edit Mode");

    const auto objectMode = route(ImGuiKey_1, ember::InteractionMode::Object, false,
                                  ember::SelectMode::Edge);
    const auto emulated = route(ImGuiKey_1, ember::InteractionMode::Edit, true,
                                ember::SelectMode::Face);
    CHECK_MSG(!objectMode.first && objectMode.second == ember::SelectMode::Edge,
              "top-row 1 should be unowned in Object Mode");
    CHECK_MSG(!emulated.first && emulated.second == ember::SelectMode::Face,
              "top-row 1 was consumed by Edit Mode while Emulate Numpad owns it");
}

TEST(edit_mode_select_keys_are_gated_by_text_input_and_modifiers) {
    auto route = [](ImGuiKey key, bool ctrl, bool textInput, ember::SelectMode start) {
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 480.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(key, true);
        ImGui::NewFrame();
        io.WantTextInput = textInput;
        ember::SelectMode selected = start;
        const bool handled = ember::editor::handleEditSelectModeKey(
            io, ember::InteractionMode::Edit, false, selected);
        ImGui::EndFrame();
        ImGui::DestroyContext(ctx);
        return std::pair<bool, ember::SelectMode>{handled, selected};
    };
    const auto typing = route(ImGuiKey_2, false, true, ember::SelectMode::Vertex);
    const auto modified = route(ImGuiKey_3, true, false, ember::SelectMode::Edge);
    CHECK_MSG(!typing.first && typing.second == ember::SelectMode::Vertex,
              "top-row 2 changed select mode while a text field owned keyboard input");
    CHECK_MSG(!modified.first && modified.second == ember::SelectMode::Edge,
              "top-row 3 changed select mode while Ctrl modified the key");
}

HARNESS_MAIN()


