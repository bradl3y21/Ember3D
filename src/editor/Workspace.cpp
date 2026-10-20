#include "editor/Panels.h"
#include "editor/ThemeStyle.h"
#include "editor/EditorLabels.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace ember::editor {

namespace {

constexpr float kHeaderHeight = 0.0f;    // Topbar owns the top edge; each area draws its own header
constexpr float kTimelineHeightShare = 0.20f; // D41's frozen initial Timeline proportion

// One class for every docked panel. Windows of the same class can dock together,
// and NoUndocking stops any of them leaving the layout.
ImGuiWindowClass panelClass() {
    ImGuiWindowClass wc;
    wc.ClassId = ImHashStr("EmberPanels");
    // Each tiled Area runs one editor and its own Header. Hide ImGui's incidental
    // window-tab strip: its internal Area A/B labels are not Blender vocabulary.
    wc.DockNodeFlagsOverrideSet = static_cast<ImGuiDockNodeFlags>(
        static_cast<int>(ImGuiDockNodeFlags_NoUndocking) | static_cast<int>(ImGuiDockNodeFlags_NoTabBar));
    return wc;
}

// The fixed layout, rebuilt on every launch (there is no .ini to restore one from).
// Centre-left: the 3D view (transparent Area D region), with Toolbar and Sidebar
// regions inside it. Right: Outliner above Properties. Timeline spans the full
// bottom edge, matching WHAT-REMAINS.md §1.
void buildLayout(ImGuiID dock, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dock);
    // Area D occupies the central node. ImGui's PassthruCentralNode is only a
    // true hole while that node is empty; with a docked window it paints the
    // host background over the renderer. The dockspace host is transparent, and
    // Area D receives the shared zero-alpha Viewport background token.
    const int nodeFlags = static_cast<int>(ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderAddNode(dock, static_cast<ImGuiDockNodeFlags>(nodeFlags));
    ImGui::DockBuilderSetNodeSize(dock, size);

    // Split the bottom BEFORE the right column so the Timeline spans the whole
    // width rather than only the viewport column.
    ImGuiID bottom = 0;
    ImGuiID main = 0;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Down, kTimelineHeightShare, &bottom, &main);

    ImGuiID right = 0;
    ImGuiID centre = 0;
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.22f, &right, &centre);

    ImGuiID rightTop = 0;
    ImGuiID rightBottom = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, &rightTop, &rightBottom);

    // Three side/bottom areas plus the central viewport area; every area header
    // carries its editor-type selector.
    ImGui::DockBuilderDockWindow(labels::DockAreaA, rightTop);
    ImGui::DockBuilderDockWindow(labels::DockAreaB, rightBottom);
    ImGui::DockBuilderDockWindow(labels::DockAreaC, bottom);
    ImGui::DockBuilderDockWindow(labels::DockAreaD, centre);
    ImGui::DockBuilderFinish(dock);
}

} // namespace

ImVec4 drawWorkspace(bool& layoutBuilt) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusHeight = theme::statusBarHeight();
    float workTop = vp->WorkPos.y;
    // BeginMainMenuBar is drawn before the Workspace, but on frame 1 ImGui has
    // not yet propagated its top-bar inset into viewport->WorkPos. Use the real
    // bar rectangle so the first DockBuilder split is based on the settled area.
    if (const ImGuiWindow* topbar = ImGui::FindWindowByName("##MainMenuBar")) {
        workTop = std::max(workTop, topbar->Pos.y + topbar->Size.y);
    }
    const float workBottom = vp->WorkPos.y + vp->WorkSize.y;
    const ImVec2 pos(vp->WorkPos.x, workTop + kHeaderHeight);
    const float statusTop = workBottom - statusHeight;
    const ImVec2 size(vp->WorkSize.x, statusTop - pos.y);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        theme::metric(theme::tokens().regionRounding));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##workspace", nullptr, flags);

    // The separator itself is the resize target; Areas remain edge-to-edge.
    ImGui::GetStyle().DockingSeparatorSize = theme::metric(theme::splitterThickness);

    const ImGuiID dock = ImGui::GetID("EmberDock");
    if (!layoutBuilt) {
        buildLayout(dock, size);
        layoutBuilt = true;
    }
    // Area D is docked into the centre and has a zero-alpha themed background,
    // allowing the 3D render produced before this UI frame to show through it.
    ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // The 3D view's rectangle: the central node, once the layout has been laid out.
    ImVec4 region(pos.x, pos.y, size.x, size.y);
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock)) {
        region = ImVec4(central->Pos.x, central->Pos.y, central->Size.x, central->Size.y);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    return region;
}

namespace {
bool gEmbeddedArea = false;
} // namespace

void setEmbeddedArea(bool embedded) { gEmbeddedArea = embedded; }

void endDockedPanel() {
    if (!gEmbeddedArea) ImGui::End();
}

bool beginDockedPanel(const char* name, bool* open, ImGuiWindowFlags extra) {
    if (gEmbeddedArea) return true;   // the area's window is already open
    const ImGuiWindowClass wc = panelClass();
    ImGui::SetNextWindowClass(&wc);
    return ImGui::Begin(name, open,
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | extra);
}

bool beginDialog(const char* name, bool* open, ImGuiWindowFlags extra) {
    // Dialogs are drawn after the docked area windows have ended, so there is no
    // stable parent popup ID stack to open a modal from. Use a regular, anchored
    // window instead; it stays fixed and cannot dock, move, resize, or persist.
    if (!open || !*open) return false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(vp->ID);
    return ImGui::Begin(name, open,
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings | extra);
}

void endDialog() {
    ImGui::End();
}

} // namespace ember::editor
