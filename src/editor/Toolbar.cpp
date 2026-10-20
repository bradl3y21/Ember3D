#include "editor/Panels.h"

#include <imgui.h>

#include <string>

#include "editor/ThemeStyle.h"

namespace ember::editor {

namespace {

struct ToolChoice {
    Tool tool;
    const char* label;
    ImGuiKey key;
    const char* keyHint;
};

constexpr ToolChoice kTools[] = {
    {Tool::Select, "Select", ImGuiKey_Q, "Q"},
    {Tool::Move, "Move", ImGuiKey_W, "W"},
    // E is reserved for Edit Mode face extrusion. The transform shortcuts
    // follow Blender's modal vocabulary: R rotates and S scales.
    {Tool::Rotate, "Rotate", ImGuiKey_R, "R"},
    {Tool::Scale, "Scale", ImGuiKey_S, "S"},
};

} // namespace

void handleToolKeys(ToolState& tools) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift) return;

    if (ImGui::IsKeyPressed(ImGuiKey_T)) tools.panelOpen = !tools.panelOpen;
    for (const ToolChoice& t : kTools) {
        if (t.key != ImGuiKey_None && ImGui::IsKeyPressed(t.key)) tools.active = t.tool;
    }
}

float toolbarRegionWidth() {
    // A compact in-Area Region. Rows use the available content width rather
    // than reserving a second fixed width inside the child.
    const float padding = theme::metric(theme::tokens().itemSpacing.x);
    return theme::metric(112.0f) + 2.0f * padding;
}

void drawToolbar(ToolState& tools, ImVec4 region) {
    if (!tools.panelOpen) return;

    ImGui::SetCursorScreenPos(ImVec2(region.x, region.y));
    const float padding = theme::metric(theme::tokens().itemSpacing.x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    const bool contentsVisible = ImGui::BeginChild(
        "##toolbar", ImVec2(region.z, region.w), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (contentsVisible) {
        ImGui::TextDisabled("Tools");
        ImGui::Separator();
        theme::pushActiveItemColors();
        for (const ToolChoice& t : kTools) {
            const bool active = tools.active == t.tool;
            const std::string label = std::string(t.label) + "    " + t.keyHint;
            // Selectable uses width 0 to fill the Region; negative sizes are
            // unsupported and silently create a dead row.
            if (ImGui::Selectable(label.c_str(), active, ImGuiSelectableFlags_None,
                                  ImVec2(0.0f, theme::metric(theme::tokens().widgetHeight)))) {
                tools.active = t.tool;
            }
        }
        theme::popHeaderColors();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Edit Mode: E Extrude, X Delete, M Merge");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

} // namespace ember::editor
