#pragma once

#include <array>
#include <cstddef>

#include <imgui.h>

#include "editor/Panels.h"

namespace ember::editor {

// Original Ember3D vector icons for the Properties category rail. Each glyph is
// authored on a normalized 16x16 grid and drawn into the caller's display-space
// button rect. The centered square has side min(rect width, rect height,
// 16 * uiScale); the caller passes ThemeTokens::uiScale explicitly.
enum class IconId {
    Render,
    Output,
    Scene,
    World,
    Object,
    Material,
    Camera,
};

// The Properties rail order. Keeping the enum-to-glyph mapping here lets
// callers and the independent pixel test enumerate the same categories.
inline constexpr std::array<PropertiesTab, 7> kPropertiesTabOrder = {
    PropertiesTab::Render, PropertiesTab::Output, PropertiesTab::Scene,
    PropertiesTab::World, PropertiesTab::Object, PropertiesTab::Material,
    PropertiesTab::Camera,
};
inline constexpr std::array<IconId, 7> kPropertiesTabIcons = {
    IconId::Render, IconId::Output, IconId::Scene,
    IconId::World, IconId::Object, IconId::Material, IconId::Camera,
};

constexpr IconId iconForTab(PropertiesTab tab) {
    for (std::size_t i = 0; i < kPropertiesTabOrder.size(); ++i) {
        if (kPropertiesTabOrder[i] == tab) return kPropertiesTabIcons[i];
    }
    return IconId::Object;
}

void drawIcon(ImDrawList& drawList, IconId icon, const ImVec2& buttonMin,
              const ImVec2& buttonMax, ImU32 color, float uiScale);

} // namespace ember::editor
