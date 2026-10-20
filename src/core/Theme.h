// Ember3D — shared default UI theme tokens and vocabulary (D42 / item 151).
//
// Owner: Ash · Lead. Contract: ARCHITECTURE.md §4.5.
// Header-only so editor and render can consume the same values without a link-time
// dependency or duplicate palettes. Numeric RGB defaults are Ember3D's explicit
// Blender-dark choices, NOT claimed to be Blender's universal source constants.

#pragma once

#include <cmath>
#include <cstdint>

#include "core/Math.h"

namespace ember {

// Theme colors are stored as sRGB bytes because these are presentation tokens.
// Core Color is linear RGBA; convert explicitly at a render boundary rather than
// silently treating an sRGB theme value as linear scene color.
struct ThemeColor {
  std::uint8_t r = 0, g = 0, b = 0, a = 255;

  constexpr ThemeColor() = default;
  constexpr ThemeColor(std::uint8_t r_, std::uint8_t g_, std::uint8_t b_, std::uint8_t a_ = 255)
      : r(r_), g(g_), b(b_), a(a_) {}

  // Direct normalized channels for UI APIs (e.g. ImGui, whose style colors are
  // presentation-space values).
  constexpr Vec4 srgbFloat() const {
    constexpr float inv = 1.0f / 255.0f;
    return {r * inv, g * inv, b * inv, a * inv};
  }

  // Linear-light conversion for render consumers of the same shared token.
  static inline float srgbByteToLinear(std::uint8_t value) {
    const float c = static_cast<float>(value) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  }
  inline Color linear() const {
    return {srgbByteToLinear(r), srgbByteToLinear(g), srgbByteToLinear(b),
            static_cast<float>(a) / 255.0f};
  }
};

struct ThemeTokens {
  // D42 / Blender Manual Themes: editor chrome uses a neutral dark header. The
  // manual makes theme colors configurable per editor; #222 is Ember3D's pinned
  // default, matching Editor.cpp::drawArea's existing (34,34,34) header fill.
  ThemeColor headerBackground{34, 34, 34, 255};

  // D42 / existing Editor.cpp ImGuiCol_WindowBg (0.18 ~= byte 46): shared dark
  // Region base. Alpha is separate below so RGB and opacity are not duplicated.
  ThemeColor regionBackground{46, 46, 46, 255};

  // D42 / Areas and Regions are editor subdivisions (Blender Manual); the
  // Viewport host must reveal the rendered scene beneath it, not paint over it.
  ThemeColor viewportBodyBackground{0, 0, 0, 0};

  // D42 / existing Editor.cpp ImGuiCol_FrameBg (0.11 ~= byte 28), retained as
  // the neutral field surface; Blender Themes manual supports per-editor colors.
  ThemeColor widgetFieldBackground{28, 28, 28, 255};

  // D42 / existing Editor.cpp ImGuiCol_Button (0.24 ~= byte 61), neutral button
  // surface; selection color is reserved for selection, not ordinary controls.
  ThemeColor widgetButtonBackground{61, 61, 61, 255};

  // D42 / existing Editor.cpp ImGuiCol_Text (0.80 ~= byte 204); readable light
  // text against the dark Region token.
  ThemeColor textPrimary{204, 204, 204, 255};

  // D42 / existing Editor.cpp ImGuiCol_TextDisabled (0.46 ~= byte 117).
  ThemeColor textDisabled{117, 117, 117, 255};

  // D42 / Preferences.h default accent (0.93, 0.45, 0.10), encoded as sRGB
  // bytes. Blender's selection cue is orange, not blue (WHAT-REMAINS §4).
  ThemeColor selection{237, 115, 26, 255};

  // D42 / explicitly frozen lighter-blue active-state token, distinct from
  // selection. This is Ember3D's chosen semantic color, not claimed as a Blender
  // universal constant.
  ThemeColor active{128, 191, 255, 255};

  // D42 / quiet neutral hover wash; does not impersonate orange selection or
  // blue active. 20/255 ~= 8% alpha.
  ThemeColor hover{255, 255, 255, 20};

  // D42: alpha is a layout metric, stored once rather than duplicated in the
  // region RGB token. Blender Themes manual establishes color configurability;
  // 0.96 is Ember3D's explicit “transparent-ish” choice from WHAT-REMAINS §4.
  float regionBackgroundAlpha = 0.96f;

  // D42 / Blender Areas model: the rendered Viewport is visible behind its
  // transparent host region. Kept separately from regionBackgroundAlpha.
  float viewportBodyAlpha = 0.0f;

  // D42: translucent semantic fills preserve the dark base while retaining the
  // same selection/active hue in rows and headers.
  float selectionFillAlpha = 0.45f;
  float activeFillAlpha = 0.85f;

  // Blender Manual — Interface Preferences, Resolution Scale: UI dimensions
  // follow detected DPI. 1.0 is the design baseline; consumers multiply every
  // logical metric by the platform content scale.
  float uiScale = 1.0f;

  // D42 metric source: Editor.cpp::drawArea headerHeight=27.0f. This is an Area
  // Header only, not the Topbar or Status Bar height.
  float headerHeight = 27.0f;

  // D42 metric source: 13 px baseline text + FramePadding.y=3.0f on both sides
  // in Editor.cpp::applyTheme => 19 logical px. DPI scale applies uniformly.
  float widgetHeight = 19.0f;

  // D42 metric source: Editor.cpp::applyTheme ItemSpacing=(8,4),
  // ItemInnerSpacing=(4,4); these are logical pixels before uiScale.
  Vec2 itemSpacing{8.0f, 4.0f};
  Vec2 itemInnerSpacing{4.0f, 4.0f};

  // D42 metric source: Editor.cpp::applyTheme sets square window/child/tab
  // regions and 3 px Frame/Grab rounding.
  float regionRounding = 0.0f;
  float widgetRounding = 3.0f;
};

// Stable user-facing labels frozen by D42. These are names for the listed
// concepts, not synonyms for other UI terms.
struct UiVocabulary {
  static constexpr const char* Outliner = "Outliner";
  static constexpr const char* Properties = "Properties";
  static constexpr const char* Sidebar = "Sidebar";
  static constexpr const char* Toolbar = "Toolbar";
  static constexpr const char* Area = "Area";
  static constexpr const char* Header = "Header";
  static constexpr const char* Region = "Region";
  static constexpr const char* Collection = "Collection";
  static constexpr const char* Modifier = "Modifier";
  static constexpr const char* ObjectMode = "Object Mode";
};

inline constexpr ThemeTokens kDefaultTheme{};

} // namespace ember
