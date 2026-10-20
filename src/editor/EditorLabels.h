#pragma once

// Ember-specific UI labels and stable internal dock IDs. Blender vocabulary
// shared across modules remains in core/Theme.h::UiVocabulary.
namespace ember::editor::labels {

inline constexpr const char* Statistics = "Statistics";
inline constexpr const char* UndoHistory = "Undo History";
inline constexpr const char* EditMode = "Edit Mode";
inline constexpr const char* Object = "Object";
inline constexpr const char* Render = "Render";
inline constexpr const char* Output = "Output";
inline constexpr const char* Scene = "Scene";
inline constexpr const char* World = "World";
inline constexpr const char* Material = "Material";
inline constexpr const char* Camera = "Camera";
inline constexpr const char* Surface = "Surface";
inline constexpr const char* Units = "Units";
inline constexpr const char* Transform = "Transform";
inline constexpr const char* Visibility = "Visibility";
inline constexpr const char* Light = "Light";
inline constexpr const char* RenderOutputResolutionNotice = "Output resolution follows the current viewport size.";
inline constexpr const char* RenderOutputTargetNotice = "A separate render output target is not implemented.";
inline constexpr const char* RenderSampleCountNotice = "No configurable render sample count is implemented.";
inline constexpr const char* RenderSampleFilterNotice = "Shadow filtering uses a fixed 3x3 PCF kernel (9 taps).";
inline constexpr const char* OutputSceneSaveNotice = "File > Save writes the scene document at this path.";
inline constexpr const char* OutputSceneFormatNotice = "Scene format: .ember (fixed).";
inline constexpr const char* OutputImageFormatNotice = "Rendered image formats are not implemented.";
inline constexpr const char* OutputTimelineRangeNotice = "This editor-only range affects the Timeline ruler; render frame ranges and .ember persistence are not implemented.";
inline constexpr const char* OutputRenderPathNotice = "A separate rendered-image output path is not implemented.";
inline constexpr const char* Item = "Item";
inline constexpr const char* Tool = "Tool";
inline constexpr const char* View = "View";

// Stable docking IDs only; the dock tab strip is hidden and these are not
// user-facing editor names.
inline constexpr const char* DockAreaA = "Area A";
inline constexpr const char* DockAreaB = "Area B";
inline constexpr const char* DockAreaC = "Area C";
inline constexpr const char* DockAreaD = "Area D";

} // namespace ember::editor::labels
