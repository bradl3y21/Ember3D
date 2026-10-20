#include "editor/Panels.h"

#include <imgui.h>

#include <array>
#include <cmath>

#include "core/Theme.h"
#include "editor/ThemeStyle.h"

namespace ember::editor {

namespace {

constexpr float kPieRadius = 90.0f;
constexpr float kDeadZone = 18.0f;
constexpr int kSliceCount = 4;

// Blender menu_pie ordering: first Left, second Right, third Down, fourth Up.
// The shading enum's natural order maps Wireframe/Solid/Material/Rendered to
// those same directions; mouse and keyboard navigation share this mapping.
constexpr std::array<ember::ShadingMode, kSliceCount> kModes = {
    ember::ShadingMode::Wireframe, ember::ShadingMode::Solid,
    ember::ShadingMode::Material, ember::ShadingMode::Rendered};
constexpr const char* kLabels[kSliceCount] = {"Wireframe", "Solid", "Material Preview", "Rendered"};
constexpr float kAngles[kSliceCount] = {kPi, 0.0f, kPi * 0.5f, -kPi * 0.5f};

int mouseSlice(float dx, float dy, float radius, float deadZone, float outerRadius) {
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance <= deadZone || distance >= outerRadius) return -1;
    // Divide into four equal 90-degree wedges, with the diagonal as each edge.
    if (std::fabs(dx) > std::fabs(dy)) return dx > 0.0f ? 1 : 0;
    return dy > 0.0f ? 2 : 3;
}

int currentSlice(ember::ShadingMode mode) {
    for (int i = 0; i < kSliceCount; ++i)
        if (kModes[static_cast<std::size_t>(i)] == mode) return i;
    return -1;
}

void closePie(PieState& pie) {
    pie.open = false;
    pie.openedByZ = false;
    pie.keyboardSlice = -1;
    pie.hoveredSlice = -1;
}

void drawPie(const ember::ViewportSettings& viewport, const PieState& pie, int hovered) {
    const float radius = theme::metric(kPieRadius);
    const float halfWedge = kPi * 0.25f;
    const int current = currentSlice(viewport.shading);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircleFilled(pie.center, radius + theme::metric(10.0f),
                          ImGui::GetColorU32(ImGuiCol_PopupBg), 64);
    draw->AddCircle(pie.center, radius + theme::metric(10.0f),
                    ImGui::GetColorU32(ImGuiCol_Border), 64, theme::metric(1.0f));

    for (int i = 0; i < kSliceCount; ++i) {
        const bool hot = hovered == i;
        const bool active = current == i;
        const ImU32 fill = ImGui::GetColorU32(hot ? ImGuiCol_HeaderActive
                                                  : active ? ImGuiCol_ButtonActive : ImGuiCol_Button);
        const float start = kAngles[static_cast<std::size_t>(i)] - halfWedge;
        const float end = kAngles[static_cast<std::size_t>(i)] + halfWedge;
        draw->PathClear();
        draw->PathLineTo(pie.center);
        draw->PathArcTo(pie.center, radius, start, end, 24);
        draw->PathLineTo(pie.center);
        draw->PathFillConvex(fill);

        const ImVec2 edgeA(pie.center.x + std::cos(start) * radius,
                           pie.center.y + std::sin(start) * radius);
        const ImVec2 edgeB(pie.center.x + std::cos(end) * radius,
                           pie.center.y + std::sin(end) * radius);
        const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
        draw->AddLine(pie.center, edgeA, border, theme::metric(1.0f));
        draw->AddLine(pie.center, edgeB, border, theme::metric(1.0f));

        const ImVec2 textSize = ImGui::CalcTextSize(kLabels[i]);
        const float labelRadius = radius * 0.58f;
        const ImVec2 textPos(pie.center.x + std::cos(kAngles[static_cast<std::size_t>(i)]) * labelRadius -
                                 textSize.x * 0.5f,
                             pie.center.y + std::sin(kAngles[static_cast<std::size_t>(i)]) * labelRadius -
                                 textSize.y * 0.5f);
        draw->AddText(textPos, ImGui::GetColorU32((hot || active) ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                      kLabels[i]);
    }
}

void closeModePie(ModePieState& pie) {
    pie.open = false;
    pie.openedByShortcut = false;
    pie.keyboardSlice = -1;
    pie.hoveredSlice = -1;
}

void drawModePie(InteractionMode mode, bool canEdit, const ModePieState& pie, int selected) {
    const float radius = theme::metric(kPieRadius);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddCircleFilled(pie.center, radius + theme::metric(10.0f),
                          ImGui::GetColorU32(ImGuiCol_PopupBg), 64);
    draw->AddCircle(pie.center, radius + theme::metric(10.0f),
                    ImGui::GetColorU32(ImGuiCol_Border), 64, theme::metric(1.0f));

    constexpr int kSlices = 2;
    constexpr const char* labels[kSlices] = {ember::UiVocabulary::ObjectMode, "Edit Mode"};
    constexpr float angles[kSlices] = {kPi, 0.0f};
    for (int i = 0; i < kSlices; ++i) {
        const bool enabled = i == 0 || canEdit;
        const bool hot = enabled && selected == i;
        const bool active = (i == 0 && mode == InteractionMode::Object) ||
                            (i == 1 && mode == InteractionMode::Edit);
        const float start = angles[i] - kPi * 0.5f;
        const float end = angles[i] + kPi * 0.5f;
        const ImU32 fill = ImGui::GetColorU32(hot ? ImGuiCol_HeaderActive
                                                  : active ? ImGuiCol_ButtonActive
                                                           : ImGuiCol_Button);
        draw->PathClear();
        draw->PathLineTo(pie.center);
        draw->PathArcTo(pie.center, radius, start, end, 32);
        draw->PathLineTo(pie.center);
        draw->PathFillConvex(fill);

        const ImVec2 edgeA(pie.center.x + std::cos(start) * radius,
                           pie.center.y + std::sin(start) * radius);
        const ImVec2 edgeB(pie.center.x + std::cos(end) * radius,
                           pie.center.y + std::sin(end) * radius);
        const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
        draw->AddLine(pie.center, edgeA, border, theme::metric(1.0f));
        draw->AddLine(pie.center, edgeB, border, theme::metric(1.0f));

        const ImVec2 textSize = ImGui::CalcTextSize(labels[i]);
        const float labelRadius = radius * 0.55f;
        const ImVec2 textPos(pie.center.x + std::cos(angles[i]) * labelRadius - textSize.x * 0.5f,
                             pie.center.y + std::sin(angles[i]) * labelRadius - textSize.y * 0.5f);
        const ImGuiCol textColor = !enabled ? ImGuiCol_TextDisabled
                                 : (hot || active) ? ImGuiCol_Text
                                                   : ImGuiCol_TextDisabled;
        draw->AddText(textPos, ImGui::GetColorU32(textColor), labels[i]);
    }
}

} // namespace

// Blender-style Z shading pie. Holding Z and releasing on a hovered direction
// confirms; tapping Z leaves the menu open for click-to-confirm or keyboard-only
// arrow/Enter selection. Escape and right click always cancel.
bool handleShadingPie(ember::ViewportSettings& viewport, PieState& pie) {
    ImGuiIO& io = ImGui::GetIO();
    bool openedThisFrame = false;
    if (!pie.open) {
        if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift ||
            !ImGui::IsKeyPressed(ImGuiKey_Z))
            return false;
        pie.open = true;
        pie.center = io.MousePos;
        pie.keyboardSlice = -1;
        pie.hoveredSlice = -1;
        pie.openedByZ = ImGui::IsKeyDown(ImGuiKey_Z);
        openedThisFrame = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        closePie(pie);
        return true;
    }

    if (!openedThisFrame && std::sqrt(io.MouseDelta.x * io.MouseDelta.x +
                                      io.MouseDelta.y * io.MouseDelta.y) > theme::metric(2.0f))
        pie.keyboardSlice = -1;

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) pie.keyboardSlice = 3;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pie.keyboardSlice = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) pie.keyboardSlice = 2;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pie.keyboardSlice = 0;

    const float dx = io.MousePos.x - pie.center.x;
    const float dy = io.MousePos.y - pie.center.y;
    const int hovered = mouseSlice(dx, dy, theme::metric(kPieRadius),
                                   theme::metric(kDeadZone), theme::metric(kPieRadius + 30.0f));
    pie.hoveredSlice = hovered;
    const int selected = pie.keyboardSlice >= 0 ? pie.keyboardSlice : hovered;
    const bool keyReleaseConfirm = pie.openedByZ && ImGui::IsKeyReleased(ImGuiKey_Z);
    const bool mouseConfirm = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool keyboardConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                 ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (selected >= 0 && (keyReleaseConfirm || mouseConfirm || keyboardConfirm)) {
        viewport.shading = kModes[static_cast<std::size_t>(selected)];
        closePie(pie);
        return true;
    }
    if (keyReleaseConfirm) pie.openedByZ = false; // tap Z: stay open for click/direction selection

    drawPie(viewport, pie, selected);
    return true;
}

// Blender's Ctrl+Tab mode-switch pie. Ember3D currently supports only Object
// and Edit; do not display speculative paint/sculpt modes as dead choices.
bool handleModePie(InteractionMode& mode, bool canEdit, ModePieState& pie) {
    ImGuiIO& io = ImGui::GetIO();
    bool openedThisFrame = false;
    if (!pie.open) {
        if (io.WantTextInput || !io.KeyCtrl || io.KeyAlt || io.KeyShift ||
            (!canEdit && mode != InteractionMode::Edit) ||
            !ImGui::IsKeyPressed(ImGuiKey_Tab))
            return false;
        pie.open = true;
        pie.center = io.MousePos;
        pie.keyboardSlice = -1;
        pie.hoveredSlice = -1;
        pie.openedByShortcut = ImGui::IsKeyDown(ImGuiKey_Tab);
        openedThisFrame = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        closeModePie(pie);
        return true;
    }

    if (!openedThisFrame && std::sqrt(io.MouseDelta.x * io.MouseDelta.x +
                                      io.MouseDelta.y * io.MouseDelta.y) > theme::metric(2.0f))
        pie.keyboardSlice = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pie.keyboardSlice = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pie.keyboardSlice = 1;

    const float dx = io.MousePos.x - pie.center.x;
    const float dy = io.MousePos.y - pie.center.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const int hovered = distance <= theme::metric(kDeadZone) ||
                        distance >= theme::metric(kPieRadius + 30.0f)
                            ? -1
                            : dx >= 0.0f ? 1 : 0;
    pie.hoveredSlice = hovered;
    int selected = pie.keyboardSlice >= 0 ? pie.keyboardSlice : hovered;
    if (selected == 1 && !canEdit) selected = -1;

    const bool keyReleaseConfirm = pie.openedByShortcut && ImGui::IsKeyReleased(ImGuiKey_Tab);
    const bool mouseConfirm = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool keyboardConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                 ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (selected >= 0 && (keyReleaseConfirm || mouseConfirm || keyboardConfirm)) {
        mode = selected == 0 ? InteractionMode::Object : InteractionMode::Edit;
        closeModePie(pie);
        return true;
    }
    if (keyReleaseConfirm) pie.openedByShortcut = false;

    drawModePie(mode, canEdit, pie, selected);
    return true;
}

} // namespace ember::editor
