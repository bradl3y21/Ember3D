#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/ThemeStyle.h"

namespace ember::editor {

// Content only: drawn inside an area's window (see Editor::drawArea).
void drawTimeline(TimelineState& tl) {
    const ember::ThemeTokens& tokens = theme::tokens();
    const float sectionSpacing = theme::metric(tokens.itemSpacing.y);
    const float labelFieldSpacing = theme::metric(tokens.itemInnerSpacing.x);
    const float inputGroupSpacing = theme::metric(tokens.itemSpacing.x);

    // Keep numeric fields at the theme's widget height even on the startup
    // frame, before applyTheme has a valid font size to derive FramePadding.y.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fieldPaddingY = std::max(0.0f,
        (theme::metric(tokens.widgetHeight) - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, fieldPaddingY));
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start");
    ImGui::SameLine(0.0f, labelFieldSpacing);
    ImGui::SetNextItemWidth(theme::metric(90.0f));
    ImGui::DragInt("##timeline-start", &tl.start, 1.0f, -100000, 100000);
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, inputGroupSpacing);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("End");
    ImGui::SameLine(0.0f, labelFieldSpacing);
    ImGui::SetNextItemWidth(theme::metric(90.0f));
    ImGui::DragInt("##timeline-end", &tl.end, 1.0f, -100000, 100000);
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    if (tl.end < tl.start) tl.end = tl.start;
    tl.current = std::clamp(tl.current, tl.start, tl.end);

    ImGui::Dummy(ImVec2(0.0f, sectionSpacing));
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = theme::metric(tokens.widgetHeight);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timelineruler", ImVec2(width, height));

    const int span = std::max(1, tl.end - tl.start);
    auto xOf = [&](int frame) { return origin.x + static_cast<float>(frame - tl.start) / static_cast<float>(span) * width; };

    if (ImGui::IsItemActive()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const int frame = tl.start + static_cast<int>(std::lround((mx - origin.x) / width * span));
        tl.current = std::clamp(frame, tl.start, tl.end);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      ImGui::GetColorU32(ImGuiCol_WindowBg));
    const int step = std::max(1, span / 10);
    for (int frame = tl.start; frame <= tl.end; frame += step) {
        const float x = xOf(frame);
        dl->AddLine(ImVec2(x, origin.y + height - theme::metric(8.0f)), ImVec2(x, origin.y + height),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled));
        dl->AddText(ImVec2(x + theme::metric(2.0f), origin.y + theme::metric(2.0f)),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), std::to_string(frame).c_str());
    }
    const float px = xOf(tl.current);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + height),
                ImGui::GetColorU32(ImGuiCol_ButtonActive), theme::metric(2.0f));

    // The current frame is already shown in the Timeline Area Header. Keep the
    // ruler as the last content row, with themed breathing room before the
    // status bar instead of duplicating a label at the bottom edge.
    ImGui::Dummy(ImVec2(0.0f, theme::metric(tokens.itemInnerSpacing.y * 0.5f)));
}

} // namespace ember::editor
