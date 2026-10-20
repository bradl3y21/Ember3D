// Independent headless regression coverage for the tiled workspace chrome.
// It drives the real Workspace, Area Header and Status Bar draw paths through
// ImGui; no OpenGL context or floating windows are involved.
#include "harness.h"

#include <glad/gl.h>

#include "core/Types.h"
#include "editor/Editor.h"
#include "editor/EditorLabels.h"
#include "editor/Gizmo.h"
#include "editor/Modal.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

// The Area Header and applyTheme are file-local to Editor.cpp. Include the
// implementation as the editor layout probe does so this test drives the real
// header widgets and theme mapping rather than a visual stand-in.
#include "editor/Editor.cpp"

namespace {

using namespace ember;
using namespace ember::editor;

bool near(float a, float b, float epsilon = 0.01f) { return std::fabs(a - b) <= epsilon; }

bool hasVertexColorInRect(const ImDrawList* list, ImU32 color, ImVec2 min, ImVec2 max) {
    if (!list) return false;
    for (const ImDrawVert& vertex : list->VtxBuffer) {
        if (vertex.col == color && vertex.pos.x >= min.x && vertex.pos.x <= max.x &&
            vertex.pos.y >= min.y && vertex.pos.y <= max.y)
            return true;
    }
    return false;
}

struct ChromeRig {
    ImGuiContext* context = nullptr;
    scene::World world;
    UndoStack undo;
    FileDialog file;
    DocumentState document;
    RenderStats stats{};
    TimelineState timeline{};
    ViewportSettings viewport{};
    ToolState tools{};
    GizmoController gizmo;
    ModalTransform modal;
    PropertiesTab propertiesTab = PropertiesTab::Object;
    AreaType typeA = AreaType::Outliner;
    AreaType typeB = AreaType::Properties;
    AreaType typeC = AreaType::Timeline;
    AreaType typeD = AreaType::Viewport;
    SidebarTab sidebarTab = SidebarTab::Item;
    SelectMode selectMode = SelectMode::Vertex;
    SnapTarget snapTarget = SnapTarget::GridIncrement;
    TransformSpace space = TransformSpace::Global;
    InteractionMode interaction = InteractionMode::Object;
    bool sidebarOpen = false;
    bool historyOpen = false;
    bool snapEnabled = false;
    bool layoutBuilt = false;

    ChromeRig() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
        const float accent[3] = {237.0f / 255.0f, 115.0f / 255.0f, 26.0f / 255.0f};
        ember::editor::applyTheme(accent);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~ChromeRig() { ImGui::DestroyContext(context); }

    void frame() {
        ImGui::NewFrame();
        drawMainMenu(world, undo, Vec3{0.0f, 0.0f, 0.0f}, file, document, viewport, space,
                     sidebarOpen, historyOpen);
        drawWorkspace(layoutBuilt);
        drawStatusBar(tools, gizmo, space, modal, snapEnabled);

        drawArea(labels::DockAreaA, typeA, false, world, undo, stats, timeline,
                 Vec3{0.0f, 0.0f, 0.0f}, viewport, space, interaction, selectMode,
                 propertiesTab, snapEnabled, snapTarget);
        drawArea(labels::DockAreaB, typeB, false, world, undo, stats, timeline,
                 Vec3{0.0f, 0.0f, 0.0f}, viewport, space, interaction, selectMode,
                 propertiesTab, snapEnabled, snapTarget);
        drawArea(labels::DockAreaC, typeC, false, world, undo, stats, timeline,
                 Vec3{0.0f, 0.0f, 0.0f}, viewport, space, interaction, selectMode,
                 propertiesTab, snapEnabled, snapTarget);
        drawArea(labels::DockAreaD, typeD, true, world, undo, stats, timeline,
                 Vec3{0.0f, 0.0f, 0.0f}, viewport, space, tools, sidebarOpen, sidebarTab,
                 interaction, selectMode, propertiesTab, snapEnabled, snapTarget);
        ImGui::Render();
    }

    void frames(int count) { for (int i = 0; i < count; ++i) frame(); }
};

} // namespace

TEST(tiled_workspace_chrome_has_semantic_contrast_and_pinned_status) {
    ChromeRig rig;
    rig.frames(6);

    const ImGuiStyle& style = ImGui::GetStyle();
    const auto& tokens = theme::tokens();
    const ImVec4 selected = style.Colors[ImGuiCol_TextSelectedBg];
    const ImVec4 active = style.Colors[ImGuiCol_ButtonActive];
    const ImVec4 hover = style.Colors[ImGuiCol_ButtonHovered];
    CHECK(selected.x > selected.y); // selection retains its warm/orange meaning
    CHECK(active.z > active.x);      // active controls use the distinct blue token
    CHECK(hover.x == hover.y && hover.y == hover.z); // hover remains neutral
    CHECK(style.Colors[ImGuiCol_SeparatorActive].x > 0.6f);
    CHECK(style.Colors[ImGuiCol_SeparatorActive].x != selected.x);
    CHECK_NEAR(style.DockingSeparatorSize, theme::metric(theme::splitterThickness), 0.01f);
    CHECK(tokens.textPrimary.r > tokens.regionBackground.r + 100); // readable on dark regions

    ImGuiWindow* status = ImGui::FindWindowByName("##status");
    ImGuiWindow* area = ImGui::FindWindowByName(labels::DockAreaA);
    CHECK(status != nullptr);
    CHECK(area != nullptr);
    if (!status || !area) return;

    CHECK(near(status->Pos.y + status->Size.y, ImGui::GetIO().DisplaySize.y));
    CHECK(near(status->Size.y, theme::statusBarHeight()));
    const ImU32 headerColor = ImGui::GetColorU32(theme::color(tokens.headerBackground));
    const ImU32 activeColor = ImGui::GetColorU32(theme::color(tokens.active));
    const ImU32 separatorColor = ImGui::GetColorU32(theme::color(tokens.widgetButtonBackground));
    CHECK(hasVertexColorInRect(area->DrawList, headerColor, area->Pos,
                               ImVec2(area->Pos.x + area->Size.x, area->Pos.y + theme::metric(tokens.headerHeight) + 12.0f)));
    CHECK(hasVertexColorInRect(status->DrawList, activeColor, status->Pos,
                               ImVec2(status->Pos.x + status->Size.x, status->Pos.y + status->Size.y)));
    CHECK(hasVertexColorInRect(status->DrawList, separatorColor, status->Pos,
                               ImVec2(status->Pos.x + status->Size.x, status->Pos.y + 2.0f)));
}

TEST(area_header_selector_is_interactive) {
    ChromeRig rig;
    rig.frames(6);

    ImGuiWindow* area = ImGui::FindWindowByName(labels::DockAreaA);
    CHECK(area && area->DockNode);
    if (!area || !area->DockNode) return;

    // Click the actual Area Header editor selector and require its popup. This
    // is a live ImGui hit test, not a source-string or disabled-widget check.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(area->Pos.x + 52.0f, area->Pos.y + 17.0f);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    rig.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    rig.frame();
    const ImGuiID comboPopupId = ImHashStr("##ComboPopup", 0, area->GetID("##area-editor"));
    CHECK(ImGui::IsPopupOpen(comboPopupId, ImGuiPopupFlags_None));
    io.AddKeyEvent(ImGuiKey_Escape, true);
    rig.frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    rig.frame();
}

HARNESS_MAIN()
