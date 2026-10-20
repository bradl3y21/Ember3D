// Modal.h — Blender's modal transform: G / R / S, then the mouse drives it.
// Owner: Chisel · Editor. Behaviour spec: WHAT-REMAINS.md §2; state machine D29 §4.5.
//
// Starts from the keys, with no button held. Left click or Enter confirms; right
// click or Esc cancels and restores the captured Transform bitwise (a command undo,
// never a reverse-accumulated delta). X/Y/Z constrain to a global axis, pressing
// the same key again switches to local; Shift+X/Y/Z constrains to a plane. Main-row
// digits type an exact value (the numpad stays navigation, D29). Shift refines,
// Ctrl snaps. The running value and the expected-next hint go to the status bar.
//
// Matrix maths are scene's (item 66): resolveWorldAxis / composeWorldTranslate /
// composeWorldRotate / composeWorldScale. Camera maths are render's: screenToRay /
// projectScreen. This file owns only the interaction.
//
// Every frame of a transform gesture pushes a continuation of ONE command, so a
// confirmed gesture is exactly one undo entry; a cancelled one leaves none.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Types.h"
#include "editor/Panels.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

namespace ember::editor {

class ModalTransform {
public:
    // Call once per frame, before the other keyboard and mouse handling. Returns
    // true while the operation owns the input, so shortcuts and picking must wait.
    bool update(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                TransformSpace space, InteractionMode interaction = InteractionMode::Object,
                SelectMode selectMode = SelectMode::Vertex, bool snapEnabled = false,
                PivotMode pivotMode = PivotMode::MedianPoint);

    // Second entry into the SAME machine: a gizmo handle grab starts a drag. The
    // axis constraint comes from the handle; confirm is the left button RELEASE,
    // cancel is Esc or right-click, undo is the same single entry per gesture.
    bool beginHandleDrag(scene::World& world, const ImGuiIO& io, GizmoState::Mode mode,
                         int axis, TransformSpace space,
                         PivotMode pivotMode = PivotMode::MedianPoint,
                         const Vec3& gizmoOrigin = Vec3{},
                         InteractionMode interaction = InteractionMode::Object,
                         SelectMode selectMode = SelectMode::Vertex);

    bool active() const { return active_; }
    bool fromHandle() const { return dragMode_; }            // started by a handle grab
    int constraintAxis() const { return active_ ? axis_ : -1; }
    const std::string& label() const { return label_; }   // live value, for the status bar
    const std::string& hint() const { return hint_; }     // what the operation expects next

private:
    void start(scene::World& world, const ImGuiIO& io, GizmoState::Mode mode, TransformSpace space,
               InteractionMode interaction, SelectMode selectMode, PivotMode pivotMode);
    void finish(UndoStack& undo, bool confirm);
    void buildText();
    void applyEditPositions(scene::World& world, UndoStack& undo,
                            const std::vector<Vec3>& positions);
    bool captureEditSelection(scene::World& world, SelectMode mode);
    bool beginFaceExtrude(scene::World& world, const ImGuiIO& io,
                          const render::OrbitCamera& camera, SelectMode selectMode);
    bool updateFaceExtrude(scene::World& world, UndoStack& undo,
                           const render::OrbitCamera& camera, bool snapEnabled);
    void finishFaceExtrude(scene::World& world, UndoStack& undo, bool confirm);

    bool active_ = false;
    bool dragMode_ = false;  // handle entry: confirm on button release instead of click
    bool started_ = false;   // a transform command is on the stack for this gesture
    bool editTarget_ = false;
    bool faceExtrude_ = false;
    GizmoState::Mode mode_ = GizmoState::Mode::Translate;
    ObjectId id_;
    Transform before_;       // captured at grab: the undo's "before", restored on cancel
    Vec3 pivot_{0.0f, 0.0f, 0.0f};   // resolved mode pivot: rotation / scale center
    Vec3 interactionOrigin_{0.0f, 0.0f, 0.0f}; // median/drawn gizmo center for translations
    std::vector<u32> editVertexIds_;
    std::vector<Vec3> editBefore_;    // exact mesh-local positions captured at gesture start
    std::optional<scene::ObjectSnapshot> extrudeBefore_;
    std::vector<ObjectId> extrudeObjectSelectionBefore_;
    std::vector<u32> extrudeSelectionBefore_;
    std::vector<u32> extrudeSelectionAfter_;
    std::vector<Vec3> extrudeVertexNormals_;
    Vec3 extrudeOrigin_{0.0f, 0.0f, 0.0f};
    Vec3 extrudeNormal_{0.0f, 0.0f, 1.0f};
    float grabX_ = 0.0f;     // mouse position at grab, framebuffer pixels
    float grabY_ = 0.0f;
    int axis_ = -1;          // 0=X 1=Y 2=Z, -1 = free
    bool local_ = false;     // the constraint is the object's own axis
    bool plane_ = false;     // Shift+axis: the plane perpendicular to the axis
    std::string typed_;      // main-row digits typed so far
    float typedValue_ = 0.0f;
    bool typedValid_ = false;
    std::string label_;
    std::string hint_;
};

// Shared by the visible gizmo and the modal transform so both resolve the same
// viewport pivot. Multi-object transforms currently operate on the active object;
// IndividualOrigins therefore resolves to that object's own world origin.
Vec3 resolveTransformPivot(const scene::World& world, PivotMode pivotMode,
                           InteractionMode interaction = InteractionMode::Object,
                           SelectMode selectMode = SelectMode::Vertex);

} // namespace ember::editor
