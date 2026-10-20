// SelfTest.h — headless proof of the modal transform (item 100).
// Owner: Chisel · Editor.
//
// Enabled by the EMBER_SELFTEST_TRANSFORM environment variable (the same
// env-var pattern as EMBER_AUTO_CLOSE_SECONDS / EMBER_FRAME_DUMP — no CLI
// contract moves). When set, Editor::draw feeds a fixed input script through
// ImGui's real event queue before NewFrame, so the modal transform runs
// through the same code path as a person at the keyboard:
//   begin (G) -> constrain (X) -> typed value / mouse move -> confirm (Enter),
//   begin -> move -> cancel (Esc), and begin -> Shift+X -> plane drag -> confirm.
// Every check is logged as a [selftest] line; the summary is PASS or FAIL.
#pragma once

#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

namespace ember::editor {

// Returns 0 while the script runs, 1 on a PASS summary, -1 on a FAIL summary.
// The editor surfaces that through Editor::selfTestStatus(); App turns it into
// the process exit code (Verity's gate: a failing selftest must exit non-zero).
int runTransformSelfTest(scene::World& world, UndoStack& undo,
                         const render::OrbitCamera& camera, InteractionMode& interaction,
                         SelectMode& selectMode, PivotMode& pivotMode, int frame);

} // namespace ember::editor
