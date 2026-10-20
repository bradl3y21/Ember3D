// Gizmo.h — the transform gizmo's hover and handle hit-testing. Owner: Chisel.
//
// This class is deliberately NOT a transform machine. It draws and hit-tests the
// handles; grabbing one enters the SAME ModalTransform the G/R/S keys start
// (item 99: one state machine, two entries — the drag is an embedded
// ModalTransform, so confirm/cancel/constraints/snapping/undo cannot diverge).
// Contract: D19/D20 (GizmoState in core; hit-testing stays editor-side).
#pragma once

#include "core/Types.h"
#include "editor/Modal.h"
#include "editor/Panels.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

namespace ember::editor {

class GizmoController {
public:
    // Call once per frame, after ImGui::NewFrame and before the viewport pick.
    // Returns true while the gizmo owns the mouse (hover click entered a drag, or
    // the drag is running), so the viewport must not pick.
    bool update(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                TransformSpace space, bool visible, bool snapEnabled = false,
                PivotMode pivotMode = PivotMode::MedianPoint,
                InteractionMode interaction = InteractionMode::Object,
                SelectMode selectMode = SelectMode::Vertex);

    // Ignored while a drag is in progress.
    void setMode(GizmoState::Mode mode);
    GizmoState::Mode mode() const { return mode_; }

    // Handle length in pixels, from the Preferences gizmo size.
    void setHandleLength(float pixels) { state_.handleLengthPixels = pixels; }

    // The state render draws from (hover, active axis, pivot, basis, snap).
    const GizmoState& state() const { return state_; }

    // The modal machine running a handle-started drag (for the status bar).
    const ModalTransform& dragModal() const { return dragModal_; }

private:
    GizmoState state_;
    GizmoState::Mode mode_ = GizmoState::Mode::Translate;
    ModalTransform dragModal_;   // THE machine: the keyboard modal's own class
};

} // namespace ember::editor
