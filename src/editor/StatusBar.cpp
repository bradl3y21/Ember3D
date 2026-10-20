#include "editor/Panels.h"

#include <imgui.h>

#include <cstdio>

#include "editor/Gizmo.h"
#include "editor/Modal.h"
#include "editor/ThemeStyle.h"

namespace ember::editor {

namespace {

const char* toolName(Tool tool) {
    switch (tool) {
        case Tool::Select: return "Select";
        case Tool::Move:   return "Move";
        case Tool::Rotate: return "Rotate";
        case Tool::Scale:  return "Scale";
    }
    return "?";
}

const char* axisName(int axis) {
    switch (axis) {
        case 0: return "X";
        case 1: return "Y";
        case 2: return "Z";
    }
    return "?";
}

const char* modeVerb(GizmoState::Mode mode) {
    switch (mode) {
        case GizmoState::Mode::Translate: return "Move";
        case GizmoState::Mode::Rotate:    return "Rotate";
        case GizmoState::Mode::Scale:     return "Scale";
    }
    return "?";
}

} // namespace

void drawStatusBar(const ToolState& tools, const GizmoController& gizmo, TransformSpace space,
                   const ModalTransform& modal, bool snapEnabled) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // ImGui's minimum undecorated window height is 32 px in this build; reserve
    // that exact strip so it fits the bottom edge rather than clipping 8 px.
    const float kHeight = theme::statusBarHeight();

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color(theme::tokens().headerBackground));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme::metric(theme::tokens().itemSpacing.x), theme::metric(2.0f)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##status", nullptr, flags);

    // Separate the pinned strip from the tiled workspace with a subtle rule.
    const ImVec2 barPos = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(barPos.x, barPos.y + 0.5f),
        ImVec2(barPos.x + ImGui::GetWindowWidth(), barPos.y + 0.5f),
        ImGui::GetColorU32(theme::color(theme::tokens().widgetButtonBackground)), 1.0f);

    // Active tool uses the semantic active color; selection orange remains
    // reserved for selected scene data.
    ImGui::TextColored(theme::color(theme::tokens().active), "%s", toolName(tools.active));
    ImGui::SameLine(0.0f, theme::metric(6.0f));
    ImGui::TextDisabled("· %s", space == TransformSpace::Local ? "Local" : "Global");

    // Running value: the live modal transform, from whichever entry started it
    // (G/R/S keys or a gizmo handle — one machine, item 99).
    const ModalTransform* live = modal.active() ? &modal
                                 : gizmo.dragModal().active() ? &gizmo.dragModal()
                                                              : nullptr;
    if (live) {
        ImGui::SameLine(0.0f, theme::metric(14.0f));
        ImGui::TextColored(theme::color(theme::tokens().active), "%s", live->label().c_str());
    }

    // Snap feedback mirrors the effective toggle (magnet XOR Ctrl).
    ImGui::SameLine(0.0f, theme::metric(14.0f));
    const bool compact = ImGui::GetWindowWidth() < theme::metric(900.0f);
    if (snapEnabled != io.KeyCtrl) {
        ImGui::TextColored(theme::color(theme::tokens().active), compact ? "Snap on" : "Snap · 1u / 15° / 0.1x");
    } else {
        ImGui::TextDisabled(compact ? "Snap off" : "Snap off · Ctrl toggles");
    }

    // One-line hint for the operation the mouse is on.
    const char* hint = compact ? "G/R/S · X/Y/Z · Esc" : "G/R/S transform · X/Y/Z constrain · Esc cancel";
    char hintBuffer[96];
    if (live) {
        hint = live->hint().c_str();
    } else if (gizmo.state().hoveredAxis >= 0 && tools.active != Tool::Select) {
        std::snprintf(hintBuffer, sizeof hintBuffer, "Drag %s on %s · Ctrl snap",
                      modeVerb(gizmo.state().mode), axisName(gizmo.state().hoveredAxis));
        hint = hintBuffer;
    } else if (tools.active == Tool::Select) {
        hint = compact ? "Click · Drag box · Shift add" : "Click select · Drag box · Shift add · A all";
    }
    if (ImGui::GetWindowWidth() >= theme::metric(680.0f)) {
        ImGui::SameLine(0.0f, theme::metric(14.0f));
        ImGui::TextDisabled("%s", hint);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace ember::editor
