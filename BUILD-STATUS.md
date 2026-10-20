# Ember3D — build status

**A snapshot, not a story.** Re-run the command at the bottom before you trust a
line of it. On a moving team this file is wrong within minutes — that is why the
last line is a command and not a sentence.

---

## Latest measured verification — 2026-10-20 (Ember3D - Copy)

Fresh configure and Release build were run to an alternate output directory because
root `bin/Ember3D.exe` is still open. A full CTest attempt was not green: Windows
Application Control blocked numerous launches and the first run exposed three
regressions (a stale v7 camera assertion, inaccessible-width Transmission/IOR widgets,
and hard-coded compare-divider colors). The camera assertion was updated; the UI
width and theme-token issues are fixed. The post-fix targeted CTest run passed 11
tests; the rebuilt camera executable was blocked on that run.

```
cmake -B build-parity-swarm-clean -DEMBER_ENABLE_GPU_TESTS=ON "-DEMBER_OUTPUT_DIR=$PWD/bin-parity-swarm-clean" -> exit 0 (run from the repository root)
cmake --build build-parity-swarm-clean --config Release -> exit 0 (one pre-existing C4189 warning in Modifiers.cpp)
ctest --test-dir build-parity-swarm-clean -N -> 82 registered targets
ctest --test-dir build-parity-swarm-clean -C Release --output-on-failure -R "test_editor_properties|editor.vocabulary_and_theme_contract|test_scene_camera|render.raster_cycles_compare_pixels|render.texture_pixels|test_render_texture_image|test_editor_cycles_viewport|test_render_path_image_io|test_render_offline_job|test_scene_topology|test_editor_box_select" -> 11 passed, 1 not run (Application Control blocked test_scene_camera)
ctest --test-dir build-parity-swarm-clean -C Release --output-on-failure -> 48/82 passed; remaining failures/not-run include Application Control, root-binary evidence, and the now-fixed UI/theme/camera regressions
bin-parity-swarm-clean\test_render_compare_pixels.exe --gpu -> 4 tests / 21 checks passed
bin-parity-swarm-clean\test_render_path_tracer.exe -> 19 tests / 13,241 checks passed
bin-parity-swarm-clean\test_render_offline_job.exe -> 11 tests / 143 checks passed
bin-parity-swarm-clean\test_render_path_image_io.exe -> 7 tests / 123 checks passed
bin-parity-swarm-clean\test_scene_topology.exe -> 21 tests / 266 checks passed
bin-parity-swarm-clean\test_editor_box_select.exe -> 4 tests / 12 checks passed
```

`test_scene_io`, `test_render_path_sampling`, `test_render_taa`, and alternate-app
smoke were blocked in direct/full-suite attempts. The targeted CTest run did pass
`test_render_texture_image`, `render.texture_pixels`, `test_editor_cycles_viewport`,
`test_editor_properties`, and the compare compositor; `.ember` v8 persistence remains
unverified because `test_scene_io` is blocked. The root-binary evidence test still
points at the old `bin/Ember3D.exe` while it is open.

One existing warning remains in `src/scene/Modifiers.cpp` (unused local `n`).

UI verification in the current tree: Properties has reachable/scrollable categories,
working shadow/output/material/emission/engine controls, Edit Mode Face Assign, and
Raster/Cycles compare controls. The targeted Properties test verifies engine/compare
state and per-slot Transmission/IOR/image controls. The A/B GPU compositor test passes.
Outliner has live
collection/object rows, multi-selection, visibility and drag/drop; Toolbar rows and
anchored Viewport Add menu are hit-tested; workspace chrome remains tiled and the
default Properties category rail fits seven categories. Material Assign is disabled
outside Edit Mode and without a face selection.

Latest direct feature receipts:

```
test_scene_io.exe: rebuilt with v8 editable-mesh tests; launch blocked by Application Control
  v8 edit topology/material persistence and v1-v7 migration are compiled, not runtime-verified
test_render_path_tracer.exe: 19 CPU tests, 13,241 checks, 0 failed
  textures, environment importance/MIS, roulette, true per-pixel adaptive sampling, BSDF/BVH
test_render_offline_job.exe: 11 tests, 143 checks, 0 failed
  per-pixel scheduling/progress, denoised/raw separation, progressive snapshots and cancellation
test_render_path_image_io.exe: 7 tests, 123 checks, 0 failed
  PFM/PPM HDR/row order, failure atomicity and concurrent writer publication
test_scene_topology.exe: 21 tests, 266 checks, 0 failed
  edit topology, extrusion, and face material assignment
test_editor_box_select.exe: 4 tests, 12 checks, 0 failed
  enclosure/crossing selection, false-positive bounds guard, click selection preservation
test_render_compare_pixels.exe --gpu: 4 tests, 21 checks, 0 failed
  Raster-left/Cycles-right wipe, split limits, image mapping, stale-preview fallback, engine behavior, area scissor
test_render_texture_image.exe: PPM/PFM/BMP tests compiled; process launch blocked by Application Control
  The same suite passed when launched by the targeted CTest run.
test_scene_io.exe: v8 topology/material persistence tests compiled; process launch blocked by Application Control
test_render_taa.exe: deterministic no-history fallback test compiled; process launch blocked by Application Control
test_editor_properties.exe: 12 tests, 190 checks, 0 failed; targeted CTest passed
  compare state and slot-local Transmission/IOR/image path/color-space controls
test_editor_vocabulary.exe: 9 tests, 3,562 checks, 0 failed; hard-coded divider colors fixed
test_editor_cycles_viewport.exe: targeted CTest passed
test_scene_camera.exe: v8 assertion compiled; process launch blocked by Application Control
```

The current scene-format contract is v8. It persists material/world image references,
color-space tags, render settings, editable topology and per-face material indices;
v1-v7 remain readable. The v8 SceneIO test executable is built but not run successfully.
The Rendered viewport consumes progressive CPU samples only when the scene engine is
Cycles; other shading modes remain on the raster path.
The pre-fix audit of falsely satisfied camera items 33/34 is preserved at
`docs/evidence/camera-items-33-34-audit.md`. This latest measurement supersedes
the historical snapshots below. Root `bin/Ember3D.exe` remains locked, and Application
Control blocked multiple test launches. The updated texture color-space GPU test now
passes under targeted CTest. The updated camera v8 assertion compiles but its process
launch was blocked. Application smoke remains unverified on this build.
The CPU
PathTracer is a deterministic Lambertian/GGX/dielectric prototype with a median-split
BVH, textures, environment importance sampling, NEE/MIS, Russian roulette and emission.
Offline jobs support true per-pixel adaptive scheduling and optional spatial denoising.
PFM/PPM writer APIs exist. The Cycles-style CPU preview is
selectable in Render Properties and progressively displayed in Rendered viewport
shading, with sample progress and cancellation.
The RGBA16F renderer uses fixed Reinhard+sRGB display conversion and a public
exposure API; no exposure UI or automatic adaptation exists yet. The additional
`Ctrl+Tab` mode-switch pie is headless-tested for Object/Edit selection and draw
submission; default CTest also verifies the pie and keymap vocabulary.

## Verified after both feature waves — 18:40

The plan is complete: **64 of 64 backlog items done**, build green, tests green.
Verified by the operator on the tree as left by the team.

```
cmake --build build --config Release            -> exit 0, no errors, no /W4 warnings
bin\Ember3D.exe --frames 60 --exit --demo-object -> exit 0
  shader built: viewport_overlay
  shader built: pbr_directional
  editor: ImGui 1.93.1 WIP initialised (GLFW + OpenGL3)
  --demo-object: created a Cube (id 1)
  last frame: drawCalls=3 triangles=12 frameMs=0.009
  App shut down cleanly

ctest (build-gpu) -> 100% tests passed out of 22      (was 13 at M1)
```

`drawCalls` went 1 → 3 and `triangles` 0 → 12: the world is seeded, the object is
drawn, and the smoke test now fails if that stops being true.

---

## Verified **rendering** — 04:55

Not blank. The gap analysis in `WHAT-REMAINS.md` closed within one round.
Independently built **and run** by the operator:

```
cmake --build build --config Release  -> exit 0        ✅
bin\Ember3D.exe --frames 60 --exit    -> exit 0        ✅
```

```
OpenGL 4.5.0 NVIDIA 617.42 | NVIDIA GeForce RTX 5080
window created: 1280x720, "Ember3D"
shader built: viewport_overlay         <- M1 grid + world axes
shader built: pbr_directional          <- M4 lighting
renderer initialised
editor: ImGui 1.93.1 WIP initialised (GLFW + OpenGL3)
frame loop ended after 60 frames
last frame: drawCalls=1 triangles=0 frameMs=0.002
App shut down cleanly
```

The window clears, draws the M1 overlay, and runs a real ImGui frame. `triangles=0`
is **correct**: M1 is a `GL_LINES` overlay and no scene objects have been spawned
yet.

**It is not a blank window.** See `docs/evidence/m1-first-pixels.png` — the grid,
the world axes (red X / green Y / blue Z), the **Outliner**, **Properties**
("Nothing selected") and **Stats** (draw calls 1, UI 233 fps) panels all render.

The `WHAT-REMAINS.md` definition of done is **met in full**: clearing, grid, axes,
a visible panel, and a runtime test.

### Runtime test — registered and passing

`build-gpu/` was configured with `-DEMBER_ENABLE_GPU_TESTS=ON`:

```
ctest --test-dir build-gpu -C Release
  1/12 app.smoke_frames ................. Passed  0.47 sec
  100% tests passed out of 12
```

`app.smoke_frames` runs the real app for 60 frames and exits non-zero if the frame
path breaks — the first test in this project that can see a runtime/GL bug. Five
`runtime.cli_*` tests cover the argument contract.

A **clean rebuild** (`--clean-first`) was re-run after the guard change below and is
green, so the fix is not masking a stale object file.

### ⚠️ The trap that made a green build silently omit the editor

`App.cpp` decides whether to wire a module with `__has_include("editor/Editor.h")`.
**CMake does not track `__has_include` as a dependency.** When `App.cpp` was
compiled, `Editor.h` did not exist, so `EMBER_HAS_EDITOR` was undefined and the
entire editor was compiled out. Chisel then created `Editor.h`/`Editor.cpp` — and
**nothing recompiled `App.cpp`**, because no `#include` line changed. The build
stayed green and the editor stayed absent: `bin\Ember3D.exe` contained no ImGui
strings at all.

It was caught by **running the binary and noticing a missing log line**
(`editor: ImGui … initialised`). Forcing one recompile — `touch src/core/App.cpp`
— made it appear immediately.

**FIXED — 04:52** (Kiln · Platform, with Ash · Lead on the consuming TU).

CMake now defines the guard when the target exists, so it is a real dependency
CMake tracks and the probe can no longer go stale:

```cmake
# src/core/CMakeLists.txt
target_compile_definitions(ember_core PUBLIC EMBER_HAS_${_ember_module_upper})
```

and `App.cpp` no longer probes the filesystem at all — it tests the definition:

```cpp
#if defined(EMBER_HAS_EDITOR)   // set by CMake; CMake cannot see __has_include
```

The lesson is kept in the source comment on purpose: **a green `cmake --build` does
not prove a newly-landed module is linked.** That was true for a long time here and
nobody noticed, because the build stayed green the whole way.

If you ever reintroduce a filesystem probe in the composition root, recompile the
root with `--clean-first` and check the binary for the module's strings.

---

## Verified green — 04:10

Independently built by the AgentDock operator, not self-reported.

```
cmake -B build                       -> exit 0
cmake --build build --config Release -> exit 0        ✅
```

| Artefact | Result |
| -------- | ------ |
| `bin/Ember3D.exe` | **exists** — 404,992 bytes |
| `ember_core`, `ember_platform`, `ember_render`, `ember_scene`, `ember_editor` | all built |
| `glad`, `glfw3`, `imgui` | all built |

### All five suites pass

| Suite | Result |
| ----- | ------ |
| `test_harness_selftest` | exit 0 |
| `test_core_math` | exit 0 — 21 checks |
| `test_core_math_edges` | exit 0 — 759 checks |
| `test_scene` | exit 0 — 24 tests, 86 checks |
| `test_scene_io` | exit 0 — 25 tests, 96 checks |

**962 checks, 0 failures.** M0 is closed and M2's `buildPacket` contract holds.

---

## Two defects were real earlier this hour — both now fixed

Recorded because the *shape* of them is worth keeping, not because they are open.

**1. `ember_render` did not compile** (owner: `Halide · Render`).
`src/render/*` included `<glad/glad.h>` while the vendored loader is GLAD 2,
which only provides `glad/gl.h`. Four includes — fixed 04:05. The convention is
now pinned in `AGENTS.md` §2 so it cannot recur.

**2. `World::buildPacket` returned almost nothing** (owner: `Slate · Scene`).
`CHECK_EQ(packetItems(w).size(), 4): 0 != 4` — zero items for four live objects,
plus non-watertight cube and sphere meshes. 13 failing checks. Now 86/86.

## The claim that was wrong, and the rule that came from it

Two findings on the wire reported success on code that did not build:

- *"src/render M1+M4 module landed: 5 TUs compile /W4-clean against core"*
- *"M0 platform layer green: window opens/closes with GL 4.5 core"* — there was no
  executable to open.

Both were measured against something other than the root build. That is the most
expensive failure mode on this project, because everyone downstream builds on it.

Now enforced: `AGENTS.md` §3–4, and the findings API **refuses** a `build` or
`module` claim that arrives without a command receipt in the same request.

---

## Re-verify

```powershell
cmake -B build ; cmake --build build --config Release ; Test-Path bin\Ember3D.exe
```

If this file disagrees with that command, **the command is right** — and please
correct this file rather than narrating around it.
