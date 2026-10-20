#pragma once

#include <imgui.h>

#include "core/Types.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

namespace ember::editor {

enum class EditMeshOperation { ExtrudeFace, DeleteSelection, MergeAtCenter };

// Whether the operation is currently available for the active Edit Mode
// selection. The Tool tab uses this to disable only actions that cannot apply.
bool canPerformEditMeshOperation(const scene::World& world, ObjectId object,
                                 SelectMode mode, EditMeshOperation operation);
float& editExtrudeDistance();

// Discrete, undoable mesh operations used by the Edit Mode Tool tab. Keyboard
// E is owned by ModalTransform and performs a mouse-drag extrusion instead.
bool performEditMeshOperation(scene::World& world, UndoStack& undo, ObjectId object,
                              SelectMode mode, EditMeshOperation operation,
                              float extrudeDistance = 1.0f);
EditOverlayState buildEditOverlayState(const scene::World& world, ObjectId object,
                                      SelectMode mode, float framebufferScale = 1.0f);
void drawEditMeshTools(scene::World& world, UndoStack& undo, ObjectId object,
                       SelectMode mode);

// Blender-like Edit Mode keys: E is consumed here only when the modal path could
// not start; X/Delete removes the current selection and M merges it at its center.
// Returns true when a key was consumed, even if the selection makes it unavailable.
bool handleEditMeshShortcuts(const ImGuiIO& io, scene::World& world, UndoStack& undo,
                             ObjectId object, SelectMode mode);

} // namespace ember::editor
