#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "core/Types.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

namespace ember::editor {

// Editor-only UI state. The gizmo's resolved basis reaches render as a matrix
// (GizmoState::orientation), so core never needs this enum (D20).
enum class TransformSpace { Global, Local };

// The active tool (Blender's toolbar). Select makes a left-drag box-select and
// shows no gizmo. Move, Rotate and Scale show the matching gizmo, and a left-drag
// on a handle transforms the object. The gizmo reads this through Editor.
enum class Tool { Select, Move, Rotate, Scale };

struct ToolState {
    Tool active = Tool::Select;
    bool panelOpen = true;   // the T toolbar
};

// State of the Save / Load window. Owned by Editor, drawn by drawFileDialog.
struct FileDialog {
    bool open = false;
    bool wantLoad = false;
    bool focus = false;          // put the keyboard on the path field next frame
    char path[260] = "scene.ember";
    std::string status;
};

// The scene on disk that File > Save writes to, and the recent files list.
// Owned by Editor for the session; the recent list is not persisted between runs.
struct DocumentState {
    std::string path;
    std::vector<std::string> recent;
};

// Opens the Open or Save As window, suggesting the current or last-used file.
void openFileDialog(FileDialog& file, const DocumentState& doc, bool wantLoad);
enum class WorkspacePreset;

// Each panel draws one ImGui window. They run between ImGui::NewFrame and
// ImGui::Render, and they change the world only by pushing commands on `undo`.
void drawMainMenu(scene::World& world, UndoStack& undo, FileDialog& file,
                  DocumentState& doc, WorkspacePreset& workspace, bool& sidebar,
                  bool& toolbar, bool& history, bool& preferences,
                  InteractionMode interaction, SelectMode selectMode);
// Compatibility overload for CPU layout tests that exercise the Topbar without
// owning Editor's workspace/panel state.
void drawMainMenu(scene::World& world, UndoStack& undo, const Vec3& addAt, FileDialog& file,
                  DocumentState& doc, ember::ViewportSettings& viewport, TransformSpace& space,
                  bool& sidebar, bool& history);
void drawViewportObjectMenus(scene::World& world, UndoStack& undo, const Vec3& addAt);

// The undo stack by label, with the current position marked; a click jumps there.
void drawHistory(UndoStack& undo, bool& open);
void handleShortcuts(scene::World& world, UndoStack& undo);
// Left-button precedence in the viewport (a gizmo handle is handled before this):
// a drag on an object moves it under the Move tool; a drag on empty space
// box-selects; a click selects. Moves are undoable; selection is not.
void handleViewportPick(scene::World& world, const render::OrbitCamera& camera, UndoStack& undo,
                        Tool tool, bool viewportOwnsPointer, const Rect& viewport);
// Edit Mode's viewport click selects a vertex, edge, or face of the active mesh.
void handleViewportEditPick(scene::World& world, const render::OrbitCamera& camera,
                            SelectMode mode, bool viewportOwnsPointer,
                            const Rect& viewport, bool xray);
// Right-click context menu for an object in the viewport.
void handleViewportContext(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                           bool viewportOwnsPointer, const Rect& viewport);

// T toggles the toolbar; Q/W/E/R pick Select/Move/Rotate/Scale. Plain keys only,
// and never while a text field has focus.
void handleToolKeys(ToolState& tools);
// Region widths are theme-scaled; the draw calls create child Regions inside
// the active 3D Viewport Area. `region` is the child rectangle in screen points.
float toolbarRegionWidth();
void drawToolbar(ToolState& tools, ImVec4 region);

// The Sidebar's tabs down its right edge, also owned by the 3D Viewport Area.
enum class SidebarTab { Item, Tool, View };
// Snap target choices shared with the viewport header. Only GridIncrement is
// currently implemented; geometry targets remain visible but disabled until item 41.
enum class SnapTarget { GridIncrement, Vertex, Edge, Face };
float sidebarRegionWidth();
void drawSidebar(scene::World& world, UndoStack& undo, bool open, ImVec4 region,
                 SidebarTab& tab, ember::ViewportSettings& viewport, TransformSpace& space,
                 ToolState& tools, bool& snapEnabled, SnapTarget& snapTarget,
                 InteractionMode interaction, SelectMode& selectMode,
                 render::OrbitCamera& camera);
// Compatibility overload for standalone layout probes that do not own the
// Editor's tool/snap state. The live Editor path uses the shared-state overload.
void drawSidebar(scene::World& world, UndoStack& undo, bool open, ImVec4 region,
                 SidebarTab& tab, ember::ViewportSettings& viewport, TransformSpace& space);
void drawFileDialog(scene::World& world, UndoStack& undo, FileDialog& file, DocumentState& doc);
// F3 command palette state. The query is typed; the list is filtered by name.
struct PaletteState {
    bool open = false;
    bool focus = false;
    char query[128] = {};
    int selected = 0;
};

void drawCommandPalette(scene::World& world, UndoStack& undo, const Vec3& addAt, FileDialog& file,
                        DocumentState& doc, ember::ViewportSettings& viewport, TransformSpace& space,
                        bool& sidebar, PaletteState& palette);

// Duplicate / delete the whole current selection as one undo step (shared by
// the Object menu, the shortcuts and the outliner's context menu).
void duplicateCurrentSelection(scene::World& world, UndoStack& undo);
void deleteCurrentSelection(scene::World& world, UndoStack& undo);

// Workspace: one whole-window DockSpace with a fixed layout, rebuilt on every
// launch. Docked panels cannot be moved, resized, collapsed or undocked; dialogs
// float but cannot be dragged.
// Returns the central (3D viewport) node's rectangle for this frame.
ImVec4 drawWorkspace(bool& layoutBuilt);
bool beginDockedPanel(const char* name, bool* open = nullptr, ImGuiWindowFlags extra = 0);
// A panel drawn inside an area's window takes no window of its own. While an
// area is being drawn, beginDockedPanel/endDockedPanel are no-ops.
void setEmbeddedArea(bool embedded);
void endDockedPanel();

// What an area runs. The header's type selector changes it.
enum class AreaType { Outliner, Properties, Stats, UndoHistory, Timeline, Viewport };

enum class WorkspacePreset { Layout, Modeling, Animation };
enum class PropertiesTab { Render, Output, Scene, World, Object, Material, Camera };

// Editor-local range and playhead for the Timeline ruler; not scene animation state.
struct TimelineState {
    int start = 1;
    int end = 250;
    int current = 1;
};
void drawTimeline(TimelineState& timeline);
bool beginDialog(const char* name, bool* open = nullptr, ImGuiWindowFlags extra = 0);
void endDialog();

class GizmoController;
class ModalTransform;
// Bottom strip: active tool, running transform value, snap state, one-line hint.
// Frame statistics stay in the Stats panel and are not repeated here.
void drawStatusBar(const ToolState& tools, const GizmoController& gizmo, TransformSpace space,
                   const ModalTransform& modal, bool snapEnabled);

// Z pie menu for the four viewport shading modes. It writes the same setting as
// the viewport header and Sidebar's View tab.
struct PieState {
    bool open = false;
    ImVec2 center{0.0f, 0.0f};
    bool openedByZ = false;
    int keyboardSlice = -1;   // direction selected with the arrow keys
    int hoveredSlice = -1;    // current radial sector under the pointer, -1 in the dead zone
};
// Returns true while the pie owns pointer/keyboard input, including its
// confirmation/cancel frame, so the viewport cannot also pick or open context menus.
bool handleShadingPie(ember::ViewportSettings& viewport, PieState& pie);

// Ctrl+Tab mode-switch pie for the currently supported Object/Edit modes.
// Edit Mode is available only while an editable mesh is selected.
struct ModePieState {
    bool open = false;
    ImVec2 center{0.0f, 0.0f};
    bool openedByShortcut = false;
    int keyboardSlice = -1;   // 0 = Object Mode, 1 = Edit Mode
    int hoveredSlice = -1;
};
bool handleModePie(InteractionMode& mode, bool canEdit, ModePieState& pie);

void drawOutliner(scene::World& world, UndoStack& undo);
void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab);
void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab,
                    ember::ViewportSettings& viewport);
void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab,
                    ember::ViewportSettings& viewport, FileDialog& file, DocumentState& doc,
                    TimelineState& timeline,
                    InteractionMode interaction = InteractionMode::Object);
void drawProperties(scene::World& world, UndoStack& undo); // default Object tab for standalone clients
void drawModifiers(scene::World& world, UndoStack& undo, ObjectId object);

} // namespace ember::editor
