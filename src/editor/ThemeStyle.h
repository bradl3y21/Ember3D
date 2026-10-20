#pragma once

#include <imgui.h>

#include "core/Theme.h"

namespace ember::editor::theme {

inline const ember::ThemeTokens& tokens() { return ember::kDefaultTheme; }

// D42 deliberately keeps active blue and selection orange semantically distinct.
// Splitter feedback is a third kind of chrome. These editor-specific neutral
// tokens preserve item 141's sampled active pixel and live with this adapter
// because D42's frozen shared token set does not include splitter geometry.
inline constexpr ember::ThemeColor splitterHovered{122, 143, 168, 209};
inline constexpr ember::ThemeColor splitterActive{184, 201, 224, 255};
// A four-pixel hit target makes tiled Area borders easier to grab without
// turning them into visual gutters. Feedback remains neutral chrome.
inline constexpr float splitterThickness = 4.0f;
// Diagnostic text is distinct from orange selection and blue active controls.
inline constexpr ember::ThemeColor modifierError{230, 64, 54, 255};

// The Properties editor's left icon strip is a fixed editor metric, not a
// per-tab width. Keep its inner content wide enough for the two-character
// object glyph after child-window padding is applied.
inline constexpr float propertiesTabStripWidth = 48.0f;

inline ImVec4 color(const ember::ThemeColor& token) {
    const ember::Vec4 c = token.srgbFloat();
    return ImVec4(c.x, c.y, c.z, c.w);
}

inline ImVec4 color(const ember::ThemeColor& token, float alpha) {
    ImVec4 c = color(token);
    c.w = alpha;
    return c;
}

// Keep the semantic styles distinct: orange is selection, blue is an active
// editor control, and hover is only a neutral wash. Header-style widgets share
// ImGui's Header colors, so opt in to the correct meaning at each call site.
inline ImVec4 activeFill() {
    const ember::ThemeTokens& t = tokens();
    return color(t.active, t.activeFillAlpha);
}

inline ImVec4 hoverFill() { return color(tokens().hover); }

inline void pushActiveItemColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, activeFill());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hoverFill());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, activeFill());
}

inline void pushSelectedObjectColors() {
    // TextSelectedBg is configured from the user's selection accent in applyTheme.
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hoverFill());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, activeFill());
}

inline void popHeaderColors() { ImGui::PopStyleColor(3); }

inline ImVec4 accent(const float* rgb, float alpha = 1.0f) {
    return ImVec4(rgb[0], rgb[1], rgb[2], alpha);
}

inline float metric(float value) { return value * tokens().uiScale; }

inline float statusBarHeight() { return metric(32.0f); }

inline ImVec2 metric(const ember::Vec2& value) {
    return ImVec2(metric(value.x), metric(value.y));
}

} // namespace ember::editor::theme
