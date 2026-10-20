# Ember3D — roadmap

Owner: **Ash · Lead**. Companion to `ARCHITECTURE.md` (§6 milestones) and
`TEAM.md` (ownership). Keep this current: it is the first thing a new agent reads
to find out what is unblocked.

Last updated: 2026-10-18.

---

## Where we are

| # | Milestone | Lead | Status |
| - | --------- | ---- | ------ |
| M0 | `cmake -B build && cmake --build build --config Release` produces `bin/Ember3D.exe`; window opens and closes cleanly | Kiln | **CLOSED 2026-10-16** — independently re-reproduced by Ash (non-author of `platform`) on the real root build; receipt in the wire finding. M1+ is verifiable for real now |
| M1 | Viewport clears, grid + world axes, orbit camera | Halide | **DONE — visually verified 2026-10-16.** Frame dumped and inspected: grid + red/green/blue world axes on the clear colour, Outliner + Properties + Stats panels visible and bordered, Stats reading `Draw calls: 1 / Triangles: 0`. Evidence: `docs/evidence/m1-first-pixels.png`; reproduce with `EMBER_FRAME_DUMP=out.bmp bin\Ember3D.exe --frames 30 --exit`. Orbit input wired (middle-drag + scroll); eyeball confirmation of drag feel is Halide's |
| M2 | Primitives, transforms, parenting, `RenderPacket` | Slate | **runtime-verified 2026-10-16** — a cube created via `World::createPrimitive` draws as 12 triangles (`--demo-object`), and the same happens for a cube loaded from disk. Pixel evidence: `docs/evidence/m2-object-renders.png` |
| M3 | Outliner + Properties + selection | Editor | **wired and regression-tested** — Outliner, Properties and viewport picking are live. A GPU test now clicks the rendered cube in the actual viewport body and verifies the selected ID; it guards both viewport-region ownership and viewport-local `screenToRay` coordinates. Human confirmation of selection feel remains welcome. |
| M4 | Directional light + PBR-ish material | Halide | **EFFECTIVELY DONE** — light OBJECTS now light the scene: every Directional and Point light is evaluated (cap 8, ignored lights logged once per change, documented default sun when the list is empty), the world background reaches the renderer, and `packet.light` is read nowhere in `render`. Outstanding: point-light pixel evidence. The deprecated `packet.light`/`DirectionalLight` pair is now **deleted** (D25), so this row is about verification only, not code |
| M5 | Gizmos + undo/redo | Chisel | **NEARLY DONE** — undo stack, commands, reversible delete, `GizmoController`, `Renderer::drawGizmo`, projected translation and drag coalescing are covered by `test_editor_gizmo`. A false orbited-camera failure was traced to the test aiming along a normalized 1-unit projection instead of the actual perspective handle endpoint; the setup now projects the true endpoint. GPU axis-color coverage now passes after active/hover brightening was changed to preserve red/green/blue dominance. Residue: human gesture/feel verification. |
| M6 | Save/load `.ember` | Scene & Data | **v8 writer implemented** — image references and editable mesh topology/per-face material indices persist; reads v1-v7. Scene-I/O runtime tests for v8 are built but blocked by Application Control. |
| M7 | Test suite green under CTest | QA | **2026-10-20** — clean alternate Release build passes; 82 targets register. Direct path-tracer/offline/topology/box-select/image-writer and compare-pixel suites pass. Several executables and app smoke are blocked by Windows Application Control; see BUILD-STATUS.md. |

### The critical path

**M0 is closed.** The normal `bin\Ember3D.exe` was rebuilt after the earlier
instance closed; clean Release build, runtime smoke and default CTest passed.

**`App` wiring has landed** (Ash, 2026-10-16): the frame loop initialises the
renderer, builds the packet, fills the camera matrices from `render::OrbitCamera`
or the active scene camera during KP_0 look-through, calls `renderer.renderFrame`,
and shuts down in reverse order. The headless smoke
flag exists: `bin/Ember3D.exe --frames 60 --exit` takes the same code path and
exits non-zero on failure.

**Current state (rechecked 2026-10-20):** scene cameras and `.ember` v8 migration,
Properties, Outliner, Viewport Toolbar/Sidebar, headers and pinned status bar have
received a Blender-focused usability pass: reachable categories, functional material
slots/Face Assign, a Scene Collection tree, compact object controls, working tool rows,
and anchored Viewport actions. Keypad navigation, modal transforms, pivot modes,
Object/Edit keymap, modal E/X/M, sub-element selection/overlays and Ctrl+Tab remain.
The D45 material-slot path is pixel-tested. Rendering has a viewport-sized RGBA16F
target with Reinhard/sRGB resolve and manual exposure, plus a separate CPU path
tracer with a BVH, diffuse/GGX/dielectric sampling, textured materials, importance-
sampled environment, NEE/MIS, Russian roulette and a progressive viewport job. Raster
now has UV image sampling and environment-background/approximate IBL input; Render
Properties selects Raster/Cycles and Rendered shading presents path samples with
progress/cancel. A distinctive **Raster/Cycles A/B wipe** lets the user compare
both backends in one Rendered viewport with a draggable divider and shared camera;
its GPU pixel test passes. The CPU tracer and offline job support true per-pixel
adaptive sampling and denoised output. PFM/PPM output APIs and a viewport-preview
save control exist, not a full-resolution production render. `.ember` v8 serializes
editable face topology and face material assignment. BMP/PPM/PFM image input landed,
but image-loader, scene-I/O and editor interaction tests are blocked by Application
Control. TAA remains unimplemented; renderers stay separate and are not Cycles/Eevee
parity. Remaining renderer/modeling work is tracked in
`docs/RENDERING-ROADMAP.md`.

Remaining UI fidelity is **human gesture feel**, not floating panels: the fixed tiled
areas/splitters are implemented and tested. The current "Does not match" table below
tracks the remaining breadth and fidelity work.

---

## Done

### `core` — the shared vocabulary and app shell (Ash)

Authored against `ARCHITECTURE.md` §4.1 and §4.6, verified by compiling the
module standalone with MSVC (see the finding on the wire for the exact output):

- `src/core/Math.h` — `Vec2/3/4`, `Mat4` (column-major, `m[col*4+row]`), `Quat`,
  `Color`, scalar helpers. Header-only by design: no `.cpp`, no GPU, no linking,
  so QA can test it with nothing built.
- `src/core/Types.h` — `ObjectId`, `Handle<T>`, `Transform`, `MeshData`,
  `MaterialDesc`, `DirectionalLight`, `DrawItem`, `RenderPacket`, `RenderStats`,
  the integer aliases, and `Transform::toMatrix()`.
- `src/core/Log.h` — `EMBER_LOG_TRACE/DEBUG/INFO/WARN/ERROR`, `EMBER_ASSERT`.
- `src/core/App.h` / `App.cpp` — the composition root. Compiles with only `core`
  present and picks up `platform`/`render`/`scene`/`editor` as each lands.
- `src/core/main.cpp` — thin entry point: `--width`, `--height`, `--title`,
  `--log`, `--help`.
- `src/core/CMakeLists.txt` — `ember_core` (library) and, if the root has not
  already defined it, the `Ember3D` executable.

---

## Conventions fixed for everyone

These are decisions, not preferences. If one is wrong, disagree *now* — on the
wire — not after two modules are built on it.

1. **`Mat4` is column-major**, `m[col * 4 + row]`, right-handed, Y-up. `A * B`
   means "apply B, then A".
2. **Euler angles are radians, XYZ order**: `R = Rz * Ry * Rx`, i.e. `R * S` for
   `Transform::toMatrix()` (`T * R * S`).
3. **`ObjectId{0}` is the null id.** `valid()` is `value != 0`.
4. **`MeshData` is parallel flat arrays** (xyz / xyz / uv) with a shared index
   list. The renderer chooses its own GPU layout; the scene does not know it.
5. **`DrawItem::mesh` is non-owning.** The scene owns meshes and must keep them
   alive for the duration of the frame it built the packet for.
6. **Include paths are rooted at `src/`**: `#include "core/Types.h"`. That is
   what `target_include_directories(... PUBLIC ${CMAKE_SOURCE_DIR}/src)` gives
   every module.
7. **`RenderStats` is reported honestly.** The editor displays it, QA checks it.

---

## Decisions (the lead's calls)

| # | Question | Decision | Date |
| - | -------- | -------- | ---- |
| D1 | Who owns the orbit camera? `TEAM.md` said "Ash (camera type)", `src/render/README.md` said render. | **`render` owns `render::OrbitCamera`.** No `core` camera type exists. `core` carries only the matrices in `RenderPacket`. `TEAM.md` M1 row fixed. | 2026-10-16 |
| D2 | `scene` cannot include `render` (downward links only), but `buildPacket` is supposed to fill `view`/`projection`. Who fills them? | **`App`.** `buildPacket` fills `items`, `width`/`height`, `clearColor`; `App` fills the packet matrices from `render::OrbitCamera`, or from `World::activeCamera()` and its `CameraSettings` while KP_0 look-through is active. `scene` remains independent of `render`. | 2026-10-16 / updated 2026-10-18 |
| D3 | `RenderPacket` carried no light for M4. | **Added `DirectionalLight` to `core`** (`direction` = ray direction, `color`, `intensity`) and `RenderPacket::light`. `scene` fills it, `render` consumes it. §4.1 updated. | 2026-10-16 |
| D4 | §4.5 said `Editor::draw(Scene&, Renderer&, …)` but §4.4 defines `scene::World` and there is no `Scene` type. | **`draw(scene::World&, render::Renderer&, const RenderStats&, UndoStack&)`.** §4.5 corrected. `UndoStack` stays editor-owned. | 2026-10-16 |
| D5 | Vendor CMake target names (who `target_link_libraries` names). | **`glfw`, `glad`, `imgui`** — defined once by `Kiln` in the root/`cmake/`. Modules link the bare name; nobody adds a second definition. | 2026-10-16 |
| D6 | `Log.h` calling convention. | **printf-style**, and a plain literal with no arguments is valid: `EMBER_LOG_ERROR("shader link failed: %s", log.c_str());`. A literal containing `%` is a format string — use `("%s", msg)` for dynamic text. | 2026-10-16 |
| D7 | Who defines the `Ember3D` executable target. | **`src/core/CMakeLists.txt`**, guarded by `if(NOT TARGET Ember3D)`, so Kiln's root may define it instead — as long as that happens *before* `add_subdirectory(src/core)`. Two definitions is the only forbidden outcome. Core's exe auto-links each module target as it appears. | 2026-10-16 |
| D8 | §4.4 uses `Object` but §4.1 never defines it (QA can't assert, outliner can't display). | **`Object` is `scene`'s**, declared in `src/scene/Scene.h` — not `core`. §4.1's rule covers types two modules need *without* including each other; `editor` links `scene`, so the data model stays with its owner. §4.4 now pins the minimum field set (`id`, `name`, `parent`, `transform`, `material`, `mesh`) plus `children`, `primitive`, `hasMesh`, `visible`; scene may add, not remove. | 2026-10-16 |
| D9 | `MeshData` by value inside `std::vector<Object>` means `DrawItem::mesh` dangles after any reallocation. Do we need a `Handle<MeshData>` pool? | **No pool — keep it by value**, and pin the lifetime instead: a `DrawItem::mesh` pointer is valid from `buildPacket()` until the next `World` mutation. Safe because the composition root fixes the frame order **input → editor mutations → buildPacket → renderFrame → swap**. Reopen only if a real consumer must hold geometry across frames. | 2026-10-16 |
| D10 | `RenderPacket::view/projection` default to identity, so a packet nobody wired is indistinguishable from a real camera at the origin. Renderer added an identity-or-zero detection with a `WARN` fallback to its own camera. | **Approved as a temporary M0/M1 scaffold only.** It exists so the viewport is not black before `App` wires the camera, and it must be **deleted** the moment `App` fills `p.view`/`p.projection` — a silent second camera path is how two modules drift. The `WARN` must say what to fix, not just that it fired. | 2026-10-16 |
| D11 | `vendor/glad` ships the GLAD2 layout (`glad/gl.h`, `KHR/khrplatform.h`) but render includes `glad/glad.h`. | **The committed vendor layout wins: `#include <glad/gl.h>`** (GLAD2). Halide changes the include in `Mesh.h`/`Shader.cpp`; Kiln makes the `glad` target expose `vendor/glad/include` as an INTERFACE include dir. `ARCHITECTURE.md` §3's "GLAD (committed, not generated)" is unchanged. | 2026-10-16 |
| D12 | M4 needs `MaterialDesc` finalised and stable. Is the shape sufficient (textures? emissive? opacity?). | **FINAL as-is, no shape change: `baseColor` (Color), `metallic`, `roughness`.** Verified against the only consumers — `render::Material::apply` maps exactly those onto `uBaseColor`/`uMetallic`/`uRoughness`, `scene::World` stores and round-trips it, nothing else reads it. Invariants now written into `Types.h` + §4.1: linear straight RGBA, `a` is the only transparency channel, `[0,1]` nominal with the renderer clamping roughness to `>= 0.045`. Out of scope, on purpose: textures, emissive, normal maps, per-object opacity — any of those is a core change and needs a re-freeze. | 2026-10-16 |
| D13 | §4.5 `Editor::init()` took no window, but ImGui's GLFW/GL3 backends need the GLFW handle and a live GL loader (Chisel, blocking M3). | **`bool Editor::init(void* nativeWindow)`**, where the argument is `platform::Window::nativeHandle()`. `void*` and not `platform::Window&` because §5 lets editor link `core`/`render`/`scene` but **not `platform`**. `editor` owns the whole ImGui frame (`NewFrame`→panels→`Render`→backend draw); `App` owns the `UndoStack` and passes it. Frame order pinned in §4.5/§4.6. **This moved an interface → said so loudly and re-frozen.** | 2026-10-16 |
| D14 | The runtime path is untestable: no way to run the frame loop headlessly. | **Added `AppConfig::frames` (0 = until the window closes) and `--frames N --exit` on the command line.** Same code path as the interactive loop, so a broken shader/VAO/ImGui init fails the run. Exit codes become a contract for CTest: **0** ran and the last frame drew something, **1** startup failed (window or renderer), **2** ran but produced 0 draw calls or missed the frame budget. Renderer-init failure is now **fatal** rather than a warning. Additive to §4.6, and since it touches `AppConfig` it was announced and the contracts re-frozen. | 2026-10-16 |
| D15 | `App.cpp` selected modules with `__has_include`, which CMake cannot see: when the editor appeared, App.cpp was NOT recompiled and the editor was silently missing from a green build (operator). | **Module presence is now a real compile definition — `EMBER_HAS_PLATFORM/RENDER/SCENE/EDITOR=1`, PUBLIC on `ember_core`** (so tests may `#ifdef` too), set by a single section in `src/core/CMakeLists.txt` with `if(TARGET ...)`, and `App.cpp` switches on `#if defined(EMBER_HAS_*)`. A tracked definition changes the command line, so a module landing forces the rebuild. Implications encoded: render needs platform, scene needs render, editor needs scene+render. `__has_include` is gone; a build with no defines is labelled core-only at compile time. Kiln's patch and the pre-existing block were **reconciled into one site** (thread `cmsg_mvahk5ol5cLk8uLQ1oTv`) — two sites emitted both `EMBER_HAS_X` and `EMBER_HAS_X=1`, resolving by ordering. No §4 signature moved, so **no re-freeze was needed**. | 2026-10-16 |
| — | *How to see the frame without eyes.* | `EMBER_FRAME_DUMP=<path.bmp>` (env var, no contract change) writes a frame on the last iteration of a bounded run and logs bg / distinct colours / non-bg % / bbox / per-quadrant %. **Since D29 it dumps the 3D AREA, not the composited window** — `readViewport` is area-relative (it adds the renderer's viewport origin and clamps to the area dims), so the ImGui panels are outside it by construction. Measured 2026-10-16: a 1280x720 window gives a 996x515 area. | 2026-10-16 |
| — | *Standing policy: additive accessors.* | A module may **add** read-only accessors/helpers to its own class after telling the Lead (no decision, no re-freeze) — e.g. platform's `Input::viewportRect()/rectIsStaleFor()`, scene's read-only `World` queries. Anything that changes an **existing** signature, or defines/alters a type `core` owns, still needs a decision finding and a re-freeze. | 2026-10-16 |
| D19 | Blender-parity scope: lights as scene objects, gizmo geometry/hit-test split, ViewportSettings. | See the finding `kb_mvajdlfzrZLxF-S8Bc4v`; §4.1/§4.3/§4.4 updated, re-frozen. Lights are objects (`createLight`, `setLight`) resolved into `RenderPacket::lights`. The `DirectionalLight`/`packet.light` pair was deprecated here and **is now deleted (D25)** — the three-step staging is complete. | 2026-10-16 |
| D20 | Gizmo interface: who draws handles, who hit-tests, and the one thing D19 left out — **space**. | **render draws (`Renderer::drawGizmo`), editor hit-tests, both from the same `GizmoState`.** NEW: `Mat4 GizmoState::orientation` (identity = Global, object world rotation = Local) is used by *both* drawing and hit-testing — passed as a **matrix, not a Global/Local enum**, because render cannot read the scene and could not compute a local axis; and `origin` is the **selection median** from `World::selectionBounds()` (the same query framing uses), so "where the selection is" has one definition. §4.1/§4.3 updated, re-frozen. | 2026-10-16 |
| D22 | Wave-2 interfaces (item 38): 3D cursor + pivot, collections, material slots, shadows + world background. | Cursor API/persistence landed in item 157; PivotMode is wired and tested (item 149). Collection storage/backend and Outliner tree UI are present; `test_editor_outliner` now exercises collection visibility undo and drag/drop membership. D45 slots and per-face packet mapping are now consumed by render and covered by GPU pixels. WorldBackground and shadows are consumed. | Rechecked 2026-10-18 |
| D24 | Grid density (item 53): does the subdivision count go in core's `ViewportSettings` or with scene's unit settings? | **In `ViewportSettings`: `int gridSubdivisions = 10`.** Density is how a *viewport draws*; units/unit-scale/snap increment are scene *semantics* that change what a number means and persist with the world. Sharing a struct would let a scene reload silently move a user's grid. Minor spacing = `1.0f / gridSubdivisions`, and consumers **clamp to [1, 100]** — zero is a divide-by-zero inside the overlay pass. Read in render, clamped in render, rebuilt only on change (with a WARN logged once per change). | 2026-10-16 |
| D25 | Is D19's deprecated pair deletable? | **Yes — `RenderPacket::light` and `DirectionalLight` are DELETED from core** once render read `lights` exclusively and scene stopped filling the dead field. Order: render reads `lights` → scene stops filling → core deletes; the tree stayed green through all three steps. `lights` ORDER is contract: creation-order slots including unmeshed/hidden lights, so `shadowLightIndex` means the same to scene and render. | 2026-10-16 |
| D26 | `frameSphere(center, radius)` — needed, or is `frameBounds(min,max)` enough? | **Accepted and wired (additive, no freeze).** App had carried `radius * 0.5773503f` so that `frameBounds` would fit a sphere; that constant was the only place in `core` that knew how the camera's fit works, and it would have broken silently if the margin changed. App now passes what it holds and the fit maths stays in the camera. Verified **behaviour-preserving, not merely compiling**: `EMBER_NAV_SELFTEST`'s framing distance is identical before and after (`dist= 2.40`). | 2026-10-16 |
| D27 | Item 54: who applies the editor's persisted "invert orbit direction" preference? | **App does, by negating the orbit deltas before feeding `OrbitCamera::orbit`.** The camera is deliberately input-agnostic (its own header says so) and App is the only place that polls input, so honouring the preference costs one branch and no new camera API. Both axes are mirrored. The accessor `Editor::invertOrbit()` is additive on the editor's own class, so no freeze. Chisel can now drop the "saved; applied once the camera reads it" caveat from the Preferences label. | 2026-10-16 |
| D28 | Item 57: is a background **gradient** wanted, and where does it live? | **Not built; shape reserved.** Two reasons: (1) a `WorldBackground` gradient field would be a third frozen-unimplemented entry while the human's actual complaints are layout and transforms — and this wave's own instruction warns against closing things that are not implemented; (2) **a gradient is viewport *display* state, not world state** — Blender's Solid-mode background gradient is a theme choice, while the world colour is world state used in Rendered mode. So it belongs in `ViewportSettings`, not `WorldBackground`. Precedence when it lands: Solid/Wireframe/Material draw the viewport gradient; Rendered draws the world background — one knob per context, never two competing ones. Note for whoever implements it: Halide's pixel harness asserts the background **to the byte**, so a default-on gradient invalidates that assertion and must be updated in the same change rather than left to fail. | 2026-10-16 |
| D29 | Item 65: wave-3 layout + modal transforms + gizmo hit-testing ownership. | **Areas/regions adopted from WHAT-REMAINS.md as written**: one DockSpace, every area window `NoMove\|NoResize\|NoCollapse\|NoBringToFrontOnFocus\|NoSavedSettings` (a draggable ImGui window is a bug), splitters, a header per area, menu bar as Topbar, pinned status bar with the live transform readout, T/N as regions *inside* the viewport area, `io.IniFilename = nullptr` with a code-built default screen. Only three things cross a boundary: core `Rect` (framebuffer px, top-left, `valid()` gate), `Editor::viewportRect()` produces the 3D area rect, `Renderer::setViewportRect(const Rect&)` consumes it — and **`RenderPacket::width/height become the AREA's size**, since `projection(aspect)` is built from them. **Modal transforms are editor-internal** (no core types): one gesture at a time, left-click/Enter confirms with **exactly one undo entry**, right-click/Esc **restores the captured Transform bitwise** (never reverse-accumulates), X/Y/Z world-then-local, Shift+axis plane, numeric entry on the **main number row** so the numpad stays navigation, Shift precision ×0.1, Ctrl snapping to scene-semantic increments. Gizmo hit-testing stays in `editor` (D20) with **screen-projected drag deltas**, which is the specific fix for "moving objects barely works". Re-frozen. | 2026-10-16 |
| D31 | Licence: what may be taken from Blender, and are we compatible with the libraries we want? | **Human decision, recorded for traceability: Ember3D is GPL-3.0-or-later.** v3 *specifically* — Blender core is `GPL-2.0-or-later` (so it is now a licit source to read **and port from**) while Apache-2.0 (Cycles, OpenSubdiv, Manifold, Embree) is compatible with GPLv3 but **not** GPLv2. Porting rules — keep the copyright/SPDX lines, add an origin note (Blender path + commit), state your modifications, never paste a subsystem to get one function, and never move code between Ember3D and AgentDock (MIT, published) in either direction — live in `AGENTS.md` §0, `TEAM.md` §0 and `WAVE-5.md` §1.5; `ARCHITECTURE.md` §3 now carries the compatibility rule for the stack. Licence questions route to the Lead **and get a decision**, because this is the one error class a later commit cannot fix. **No interface moved → no re-freeze.** | 2026-10-16 |
| — | *Item 126 is **DECIDED*** — see D36 below. Blender-dependent work may now start under the attribution rule; ports and vendoring follow D36. | Was gated behind item 103; that gate is cleared. | 2026-10-17 |
| D36 | Item 126: how does GPL-3.0-or-later re-scope wave 5 — what becomes a Blender port, what gets vendored, what attribution is enforced? | **Read `bmesh_class.hh` end to end, plus the `bmesh/operators/` and `editors/mesh/` inventories, before answering** — not guessed. *Ports:* the **design** is already ported into D34's `EditableMesh` (Blender's `BMHeader`/`BMVert`/`BMEdge`/`BMLoop` ring+radial/`BMFace`/`BMesh` transcribed to indices), with two deliberate deviations — we **derive** normals where Blender caches `no[3]`, and one `topologyVersion` where Blender keeps per-type dirty bits — and one reassuring match: **Blender's own `USE_BMESH_HOLES` is commented out**, so our no-holes limit mirrors it. Item 111 reads `bmo_extrude.cc`, `bmo_dissolve.cc`, `bmo_removedoubles.cc`, `bmo_dupe.cc`; **`toMeshData`'s concave triangulation reads `bmo_triangulate.cc` first** and must not drag `BLI_polyfill`/`BLI_scanfill`. **Track C is specification source, not port source** — `editors/mesh` is Blender's ImGui-less architecture, nothing pasteable. *Vendoring:* **OpenSubdiv first** (subdivision, Apache-2.0-equivalent) rather than porting subsurf, then Manifold (booleans), meshoptimizer (perf — out of wave 5's scope), Embree/Cycles only if scoped; **geometry-central rejected** (D34 already decided our structure, vendoring it would duplicate the mesh model); `libigl` MPL-2.0 parts only. Vendor only when the item is seeded, and `vendor/**` is Kiln's, so every addition is a request. *Attribution (item 106 enforces it):* original `SPDX-FileCopyrightText` + `SPDX-License-Identifier` **verbatim**, `Origin: blender/blender <path> @ <sha\|tag>`, a `Modifications:` statement, and our own copyright added **alongside, never replacing** — mechanically checkable, with the honest residue that **an undeclared port cannot be detected by grep**. *The licence does NOT solve:* the engineering wall (verified in the file: `bmesh_class.hh` includes `BLI_assert.hh`, `BLI_sys_types.hh`, `DNA_customdata_types.h`, `DNA_listBase.h`, and `BMesh` holds `BLI_mempool*`/`CustomData`/`ListBaseT`) and cost (context, not output). **AgentDock is MIT: no code crosses that boundary in either direction.** Frozen in `ARCHITECTURE.md` §3 + `TEAM.md` §11. | 2026-10-17 |
| D34 | Item 109: the editable mesh representation — extend `MeshData` with on-demand adjacency, or a proper editable structure? | **A proper editable structure, `scene`-owned: `EditableMesh`, with adjacency STORED (CSR), not derived.** The requirement decided it: "adjacency must be cheap because selection and extrude need it every frame" rules out a rebuild-on-demand (O(mesh) per query, per frame). The shape is **Blender's `bmesh` data model with indices instead of pointers** — loops (corners, carrying per-corner uv), unique undirected edges, n-gon faces with a `materialIndex` — plus CSR adjacency (`vertex→edges`, `vertex→faces`, `edge→faces`) and a `topologyVersion`. `render` never sees it: `Object` keeps `MeshData mesh` as the render/pick form and gains an optional `EditableMesh edit` as the source of truth once edited, so the render path does not move (cost: edited objects hold geometry twice — accepted, and named as residue). Conversions: `fromMeshData` (one face per triangle) and `toMeshData` (triangulate with **ear clipping**, so concave n-gons are correct, not overlapping fans); `MeshData → EditableMesh → MeshData` is an **identity**, while polygon grouping does **not** survive the other direction — the asymmetry is documented so item 112 tests the direction that holds rather than discovering it. Selection is index sets + `topologyVersion`, and an op must remap or clear it, never dangle it. Frozen in `ARCHITECTURE.md` §4.4. | 2026-10-17 |
| D35 | Item 113: Object vs Edit mode as state, `Tab`, header readout, select modes, and Blender's mode-switch keymap. | **Implemented and covered:** `Tab` toggles Object/Edit for a single editable mesh; invalid Edit targets fall back to Object Mode. Ctrl+Tab opens a cursor-centered mode pie offering only supported Object/Edit modes (hold/release, click/Enter, arrows, Escape/right-click); `test_editor_mode_pie` proves the interaction. Numpad keys drive navigation; persisted Emulate Numpad optionally routes top-row view digits and suppresses Edit Mode 1/2/3 selection; the header selector remains available. Select mode is editor-global (Blender remembers it per object), and unsupported paint/sculpt modes are intentionally absent. `test_editor_layout` covers 1/2/3 ownership; App's navigation self-test covers keypad/emulation. | Rechecked 2026-10-18 |
| D42 | Item 150: freeze Blender-like default theme tokens, metric scale and closed UI vocabulary. | **One coherent dark default is frozen across all areas; numeric values are Ember3D's explicit tokens, not falsely attributed Blender constants.** sRGB RGBA: Header `#222222FF`; Region `#2E2E2EF5` (96% alpha); Viewport body transparent (alpha 0); field `#1C1C1CFF`; button `#3D3D3DFF`; text `#CCCCCCFF`; disabled `#757575FF`; selected `#ED731AFF` (orange); active `#80BFFFFF` (lighter blue); hover `#FFFFFF14` (~8% neutral wash). Selection/header fills use 45% alpha, active fills 85%; user accent preference may override only the default orange. Metrics at scale 1.0 / 13 px baseline: Header 27 logical px; widget 19 px (font + 2×3 px padding); spacing 8×4, inner 4×4; region/panel/tab rounding 0, widget/button/grab rounding 3; region alpha .96, viewport host alpha 0. One platform DPI scale multiplies all metrics uniformly. Closed labels, exact spelling/case: **Outliner, Properties, Sidebar, Toolbar, Area, Header, Region, Collection, Modifier, Object Mode** — no “Hierarchy”/“Inspector”/“Toolbox”/“Panel” substitutions for those concepts. Blender Manual checked: Themes says colors are customizable per editor; Interface Preferences defines Resolution Scale relative to detected DPI. Citations and explicit scope in `ARCHITECTURE.md` §4.5. Freeze requested from operator; not yet implemented in editor. | 2026-10-17 |
| D43 | Item 179: icon strategy for Properties and Toolbar — how are Blender-like icons authored, licensed and added? | **Drawn vector glyphs, no font and no atlas.** `Icons.h/.cpp` use original ImDrawList primitive geometry on a normalized 16×16 grid, scaled by `ThemeTokens::uiScale`; there are no external assets or copied Blender glyphs. The Properties rail has seven glyphs (Render/Output/Scene/World/Object/Material/Camera) with exact-name tooltips; the Toolbar has Select/Move/Rotate/Scale. Idle/disabled/active/hover states use D42 tokens. `test_editor_icons` pixel-checks bounds, 1x/2x scaling, and category tints, including Camera. Implemented; Camera glyph and tab were added with item 209. | 2026-10-18 |
| D44 | Item 180: decide what Timeline means without an animation system. | **Option (a): frame-range/playhead ruler only; do not fake an animation editor.** Keep the Area name `Timeline` and header readout `Frame N`; remove the panel apology “Not connected to animation. The playhead only moves this view.” Controls are exactly Start/End integer fields (defaults 1/250; clamp End≥Start), full-width numbered frame ruler and visible playhead; click/drag sets current frame, clamped to range. No play/stop/rewind, keyframe channels/diamonds, markers, auto-keying, playback controls, scene animation state or `.ember` persistence of the editor-local `TimelineState`. Current scene has no animation system; `TimelineState` is only `{start,end,current}` (`Panels.h`), so moving playhead changes this Area's view state only. Blender Manual says Timeline jumps frames, manipulates keyframes and controls animation playback; that is the deliberate, plainly documented difference. Frozen in `ARCHITECTURE.md` §4.5; operator re-freeze requested. | 2026-10-17 |
| D45 | Item 188: decide the object material-slot model, packet face mapping and Properties controls so UI-10 / item 144 can proceed. | **Slots are ordered, object-local values**: `Object::materials`; zero-based per-object `u32` index; slot 0 is required/default and cannot be removed; no global Material datablock or stable slot ID. `EditableMesh::Face::materialIndex` assigns faces. Removing k>0 maps k to 0 and decrements higher indices. The packet carries non-owning `materials/materialCount` plus `triangleMaterialSlots/triangleMaterialSlotCount`; scene emits one mapping per emitted index triangle; render resolves invalid/missing entries to slot 0 (legacy `material` only when count=0). GPU tests prove two faces can render from different slots and validate malformed-map/slot fallback. Material-slot Properties UI and `.ember` editable-face assignment persistence remain unimplemented. Blender Manual [Material Assignment](https://docs.blender.org/manual/en/latest/render/materials/assignment.html) supports per-object slots and selected-face assignment. Implementation status: initial scene/renderer path landed 2026-10-18; see `docs/RENDERING-ROADMAP.md`. | 2026-10-17 |
| D41 | Item 129: areas and splitters — freeze the DockSpace extent, tile tree, startup proportions, pinned chrome, in-area regions, and writer boundary. | **The usable window is three non-overlapping bands: pinned Topbar, one DockSpace, pinned Status Bar; the DockSpace fills exactly the region between the bars.** This resolves “one DockSpace over the whole window”: all editor work-area pixels tile exactly once, none floats or overlaps chrome. Deterministic startup tree, applied every launch without `.ini`: root splits Timeline to a full-width bottom 20%; upper 80% splits 78% centre-left Viewport / 22% right column; right column splits 55% top Outliner / 45% bottom Properties. These are initial proportions, not locked; a splitter moves only its shared sibling boundary, never undocks, overlaps or leaves a gap; outer resize preserves tree topology. Stable logical areas: Viewport/Outliner/Properties/Timeline, each one active editor with its own Header/type selector; switching type does not mutate tree. Toolbar (`T`) and Sidebar (`N`) are child regions inside Viewport, not DockNodes. **Ownership does NOT widen:** Chisel alone writes `src/editor/**` (one coupled ImGui composition surface); Verity may author independent layout tests in `tests/**` in parallel; other agents remain in their disjoint paths and request any needed interface before coding. Rationale: multiple writers in the DockSpace/editor state reintroduce the source collisions this ownership partition prevents. Blender Manual checked: Areas reserve rectangles for editors; Regions subdivide an editor ([Areas](https://docs.blender.org/manual/en/latest/interface/window_system/areas.html), [Regions](https://docs.blender.org/manual/en/latest/interface/window_system/regions.html)). **Current implementation is not compliant yet:** `Workspace.cpp` still builds generic Area A/B/C, puts Stats/Undo in the bottom slot, and splits Timeline's slot only under the centre; the frozen target is not a claim it landed. Frozen in `ARCHITECTURE.md` §4.5. | 2026-10-17 |
| D40 | Item 120: the modifier stack — an ordered list per object, evaluated into the packet, non-destructive by construction. Where does evaluation sit, and how does a modifier declare its parameters? | **Ordered, per-object, evaluated in `scene` at packet build, and `render` learns nothing.** `Object` gains `std::vector<Modifier> modifiers` (index 0 = first) plus `modifierVersion` (any stack mutation) and `meshVersion` (any API that replaces `mesh`) for caching. Evaluation: base mesh is `hasEdit ? edit.toMeshData() : mesh`; walk the stack in order skipping disabled entries (position preserved); each modifier is `const MeshData& -> MeshData`; the result lives in the packet's evaluated-mesh storage and `DrawItem::mesh` points at it (D9 lifetime). **Non-destructive by construction**: the input is `const`, the output is a value, and a modifier has no reference to the object — so removing the stack restores the original exactly because it was never written (item 123's test). Determinism is contract: a modifier reads only its parameters and the incoming mesh — no clock, camera or selection. Cache key `(baseSerial, modifierVersion)`, `baseSerial = edit.topologyVersion` when edited else `meshVersion`; a missed bump silently serves stale geometry, so it is a mutator rule AND a test. **Edit mode suspends the stack** (base mesh drawn, because overlays/picking indices describe base topology); corollary: object-mode picking uses the evaluated mesh, edit-mode picking uses the base. **Failure is a no-op**, recorded for the editor, and the stack continues — a bad modifier cannot blank an object. **Parameters**: typed struct per kind, `std::variant` where the index IS the kind (static_asserts pin the order — no entry can claim one kind while holding another's params), and `modifierDesc(kind)` exposes `key`/`label`/`type`/range/step so the editor's controls and save/load both read one table and neither switches on the kind. Residue: the compiler cannot verify the descriptor matches the struct's fields (item 123 must round-trip every key), and there is no per-modifier "show in edit mode" yet (Blender has one; named as a deliberate deviation). **`render` needs no knowledge of modifiers — it consumes the evaluated `MeshData` — and if it ever needs to know one exists, this decision is violated.** Frozen in `ARCHITECTURE.md` §4.1/§4.4. | 2026-10-17 |
| D39 | Item 108: fold the trust layer into `ARCHITECTURE.md` and re-freeze — and state the declaration-order rule that has now bitten three times. | **The trust layer is documented as built, and the three-incident class is stated as a rule.** `ARCHITECTURE.md` §7 now carries the verifier's **six checks in order** with what fails each (freshness gates everything; artifact exists and is non-empty; artifact postdates its **declared** `depends:` per D38; the named test exists and passes; the symbol **in the named file and in code** with comments stripped, plus a non-test `call_site_file:` for EDITOR; a non-empty residue), the per-class extras (RENDER's `shader built:` + screenshot + two named numbers; DOC's `doc_must_contain` and `doc_must_not_contain_regex`; DECISION's `decisionId` equality; D36's four attribution lines for a declared port), the item-107 proof that the gate **rejects** (8/8 mutations in a SHA256-verified scratch copy, after two real holes were found and closed: a symbol present only in a comment, and a paraphrased stale claim), and the residue the checks cannot see. **Declaration order (D39), the same failure in three shapes:** `CollectionId` used before it was declared (**loud** — the compiler catches it), `__has_include` used as a module probe (**silent** — CMake cannot see it as a dependency, so the editor landed into a green build that omitted it; D15's tracked `EMBER_HAS_*` flags are the fix), and `scaleStrippedRotation` used before its definition and then defined twice (**shared** — a peer's linker caught it). **The rule: nothing is used before it exists, and nothing is inferred that the build system cannot see** — declare above first use, prefer a trackable compile flag over a compile-time existence probe, and grep the module for the symbol before adding a definition. The silent case is the only one that survives a green suite, which is precisely why `build.freshness` exists. **Re-frozen:** no §4 signature moved, but the trust layer decides whether anybody's "done" is accepted, so it belongs in the frozen contract — a document describing a verifier it does not have would be the header-claims-behaviour mistake in the one file everyone trusts. | 2026-10-17 |
| D38 | Item 112 follow-up: the D33 evidence verifier is the only red test in a 29-test tree — is its fallback dependency set wrong? (Verity, with the code in front of her and option 1 recommended; Kiln's `cmake/**`, so a routing decision rather than an edit.) | **Her diagnosis is right and the ruling is option 1, strengthened: an artifact declaration must DECLARE `depends:`, and the checker must not guess.** `VerifyEvidence.cmake:230-232` fell back to globbing `src/**` **and** `tests/**` when a declaration had no `depends:` — and `bin/Ember3D.exe` is built from the root target and compiles no `tests/*.cpp` (root `CMakeLists.txt:77`), so **any test edit invalidated evidence about the app**: a check firing on correct input, the same guard-cries-wolf shape as the deferred per-target freshness gap, one layer up. Her asymmetry is the crux and is exactly right: `build.freshness` compares the newest source against the newest binary *anywhere* (her test exe is newer than her source, so it passes) while CHECK 3 compares it against **the named artifact**, which is stricter — so the comment at `:211-212` claiming the fallback matched `CheckFreshness`'s set "so the two checks cannot disagree" was never true, because matching the glob *sets* does not make the reference *binary* the same. **Rule: `depends:` is required whenever `artifact:` is present; a directory entry means "everything under it, recursively" (the author declares the scope, the tool expands mechanically — it never infers); a missing `depends:` FAILS CHECK 3 saying so, rather than falling back to a guess.** Calibration note, because the state moved under us: **on my run the suite is 100% of 29, not 28/29** — `Ember3D.exe` relinked at 03:23:32 after her test edit at 03:10:38 (Slate's item-128 change forced it), so the failure is *masked, not fixed*, and re-fires on the next `tests/` edit. The red was never hers and I said so. Rejected: dropping `tests/*.h` from the glob (leaves `tests/*.cpp` firing), and artifact-type-aware heuristics (still a guess, and a silent one). Implementation is Kiln's (`cmake/**`): the two existing declarations gain `depends:` (`src/` + `vendor/` for `bin/Ember3D.exe`), and the `:211-212` comment must be rewritten to say the two checks differ *by design*. No module interface moved → **decision, not a re-freeze** (the D31 precedent). **LANDED 2026-10-17** (Kiln, receipt `kb_mvbsjj0c1o9Qd1Dxu257`): `depends:` required with the fallback glob **deleted** (not narrowed), directory entries expand recursively, an empty directory fails as "asserting nothing", `item-102-build-gate.evidence` declares `depends: src/, vendor/`, and the `:211` comment now says the two checks ask different questions. Verified on my run — build exit 0, `ctest` **100% of 29**, and I read CHECK 3 (`:230-240`): the required-depends refusal is real code. Kiln demonstrated it both ways (the masked case passes; a declaration explicitly scoping `src/, tests/` still FAILS naming the test file), so the check keeps its teeth wherever the author declares that scope. New residue, named because the guess moved rather than vanished: **an over-broad `depends:` reopens the false positive** — scope honesty is the author's, like residue honesty. | 2026-10-17 |
| D37 | Item 118: the edit-overlay state, the render API, the viewport toggles, and the ownership split (asked by Chisel and relayed by Halide; three messages, no code written pending the answer). | **The editor extracts, render draws primitives — render never learns what an `EditableMesh` is.** `EditOverlayState` (core) carries render-ready `vertexPoints` / `edgeLines` / `faceTriangles` + `faceOfTri`, parallel `*Selected` flags, the object's `mesh` (non-owning, D9 rule) and `model` matrix, `mode` + `selectMode`, active-element indices, `vertexRadiusPixels`/`edgeWidthPixels`, and theme colours; `Renderer::drawEditOverlay(state, view, projection)` draws it, gated on `mode == Edit` and the object being visible. **Why primitives and not the mesh:** `MeshData` is triangles and cannot express the model's edges or its n-gons, so the extraction is the only place the editable topology becomes drawable — and passing `EditableMesh*` to render would make `render` include `scene` and walk topology, the one dependency the module map exists to prevent (recorded as a rejected alternative). `ViewportSettings` gains four additive fields — `showEditVertices` / `showEditEdges` / `showEditFaces` (highlight only; the surface is always drawn) / `xray` (depth test off for the overlay pass, which is the point of x-ray) — read in render through the existing `viewportSettings()` knob pattern. **Counts are NOT a field**: only the editor knows the selection, so the header derives them. **The sizes are one number with two consumers** (draw and hit-test, item 115's picking), the D20 rule, and colours arrive in the state so render hardcodes no palette (Blender's edit selection is orange). **Ownership:** scene owns the geometry *and* the selection (`Object::edit`, D34) and the editor must not write into it; editor owns the interaction, the selection sets, picking, the modal gesture and building the state; render owns only the drawing. **`MeshSelectMode` (scene, `EditableMesh.h:65`) is a DUPLICATE of core's `SelectMode` and is deleted** — scene's edit ops take core's, because core's is the one render needs and two enums for vertex/edge/face is how a conversion gets written and then forgotten (the D21 class of drift). **Order 115/116/118:** 115 may land selection state + picking with no cross-module dependency beyond the pixel sizes; 116 needs scene's public edit-entry + mutation + undo surface, which is scene's shape to define with D34's rule that a topology change remaps or clears the selection. Residue: the extraction cost per frame is real — the editor should rebuild the buffers when `topologyVersion` changes, not every frame, and that is permitted rather than required; and at decision time the tree was **red at link** in `scene` (`EditableMesh::fromMeshData`/`toMeshData`/`validate` declared but not defined). **That was cleared within the hour** — but the gate is still red on an unrelated cause: `tests/test_scene_topology.cpp` (newest source, 03:01:31) is wired into **no** CMake target, so `build.freshness` reports STALE (newest exe 02:58:04, cause 3 of the check's own list) and both `evidence.*` cases fail at CHECK 1/6 — **25/28, not 3 broken things.** A rebuild cannot fix an orphan; it is QA's to wire in or delete. So the overlay path remains unexercised (**orphan cleared 2026-10-17** — Verity wired it; the suite is 29 and green). Frozen in `ARCHITECTURE.md` §4.1/§4.3/§4.5. | 2026-10-17 |

### D46 — Item 201: viewport right-click context menu

**Decision: yes, in Object Mode only, and only on a picked object.** The object
menu's enabled actions are Rename, Duplicate and Delete (the latter two use the
existing undoable commands); empty viewport space opens no menu. The Outliner row
menu is separate. Edit Mode reserves right-click for the element context menu in
item 117. An active modal transform/gizmo drag has first refusal and right-click
cancels/restores it without opening a popup; the shading pie also consumes
right-click to cancel. This is consistent with a zero-pixel result when the click
does not hit an object. No disabled placeholders for unsupported operations are
part of the decided menu. See `ARCHITECTURE.md` §4.5 for the full precedence and
the implementation residue: current dispatch lacks the Object Mode guard and
still shows three disabled placeholders; Chisel was notified.

### Camera backlog audit — items 33, 34, and 209 (2026-10-18)

The pre-fix tree audit and post-fix residue are preserved in
[`docs/evidence/camera-items-33-34-audit.md`](evidence/camera-items-33-34-audit.md).
It found that #33 and #34 had been marked satisfied without reachable camera
storage or look-through: `CameraSettings` was unused, `.ember` v2 had no camera
record, and App always filled the packet from `OrbitCamera`. The new camera
contract is recorded in `ARCHITECTURE.md` §4.1/§4.4: scene-owned camera settings,
v3 persistence with v1/v2 readers, and App-owned KP_0 look-through. #209's
focused round-trip, Properties interaction, and real App packet tests pass. Item
33's residual (no viewport camera glyph / no camera `DrawItem`) is named in the
audit artifact rather than silently implied complete.

### D35 keymap implementation follow-up — 2026-10-18

The earlier D35 row's “App-side rebinding not done” residue is now closed in
`App.cpp`: the real key dispatcher uses `KP_1/3/5/7/9`, `KP_Decimal`, and Home;
Preferences persists Emulate Numpad (off by default), which routes top-row view
digits only when enabled. In Edit Mode, top-row 1/2/3 select Vertex/Edge/Face only
when text input and modifiers do not own the keys and Emulate Numpad is off. The
header selector remains available. `test_editor_layout` exercises the Edit Mode
key gating; `EMBER_NAV_SELFTEST` exercises keypad and emulated-key actions. Residue:
the Ctrl+Tab **mode-switch** pie now has a headless input test. A tree audit corrected
the old D35 claim: Blender's 5.2 LTS Manual defines Ctrl+Tab as a mode-switch pie, not a
Vertex/Edge/Face select-mode pie
([Mode Select](https://docs.blender.org/manual/en/latest/editors/3dview/modes.html)).

---

## Test suite state

**Measured 2026-10-19 after survey follow-up:** alternate-output Release build and
60-frame smoke pass; 40 default and 70 GPU targets passed. Root-binary evidence and
several Application Control-blocked test processes were excluded; material
Transmission/IOR UI interaction remains unverified. See `BUILD-STATUS.md`.
See `BUILD-STATUS.md` for exact command receipts.
| D16 | Picking is unimplemented and three people are blocked on its interfaces; the input coordinate space is undefined; orbit triggers over panels. | **Picking splits along ownership: `Ray` in `core` (Math.h — render produces, scene consumes, so §4.1 puts it there), `Ray OrbitCamera::screenToRay(x, y, w, h)` in `render` (only it owns view/projection), `std::optional<PickResult> World::pick(const Ray&)` in `scene` (only it knows meshes + world matrices). No signature changes needed anywhere: `editor` links render+scene, so the click is completed inside `draw()`.** Canonical input space = **framebuffer pixels, top-left origin, y-down** (= `Window::width/height` and `readViewport`); platform converts from GLFW and exposes `Window::contentScale()` for ImGui-point callers; the viewport rect must be refreshed on resize. Also: `scene` may add read-only helpers to `World` after telling me (no freeze); existing signatures and core types still need one. Carries the landed App-side orbit-gate fix (ask the editor, latch the drag). Re-frozen. | 2026-10-16 |
| D17 | **Revises D16 Part B.** Is the canonical input space framebuffer pixels (my first call) or window/logical coordinates (Kiln's)? | **Converted to Kiln's position: platform publishes WINDOW coordinates (logical points, top-left, y-down)** — the space `glfwGetCursorPos` and ImGui both live in, so "is the cursor over this panel?" needs no conversion at all. `screenToRay` keeps framebuffer pixels (it must match the projection and `glViewport`), so exactly one conversion happens at the picking call site, via `io.DisplayFramebufferScale`. `Window::contentScale()` **dropped** as unnecessary. Revised loudly *before* anyone built against the old rule; §4.2/§4.3 updated, re-frozen. | 2026-10-16 |
| D18 | The smoke test can never prove an object renders: there is no way to seed a scene from the command line, so `triangles` stays 0 and a broken mesh draw path looks like an empty world. | **Added `--scene <file.ember>` (loads through `SceneIO` — the File→Open path) and `--demo-object` (`World::createPrimitive`, the Add-menu path); both seed in `init` before the loop.** New gate: if ≥1 **visible meshed** object exists and the last frame draws 0 triangles → **exit 2**. The predicate is deliberately *visible AND meshed*, not "object exists" — my first cut counted all objects and false-failed on `repro.ember`, which is `mesh none` (an unmeshed group, correct at 0). Additive to §4.6; announced and re-frozen. Evidence: `docs/evidence/m2-object-renders.png`. | 2026-10-16 |

---

## Human-found defects (2026-10-16) — the current work

A person used the app and hit three things that no unit test could see. Root
causes were diagnosed by the operator; this is the ownership split.

| # | Defect | Owner | Status |
| - | ------ | ----- | ------ |
| 1 | Orbit breaks after resize/fullscreen, and orbiting over a panel spins the camera | **Ash** (App gate) + **Kiln** (rect + size callback) | **DONE, with a regression guard underway** — App asks `Editor::wantsViewportFocus()` and latches the drag; Kiln's callbacks + per-frame self-check + heal are live and the operator verified the fullscreen fix. App-side cross-check awaits `Window::windowWidth/Height()` (logical size); until then Verity asserts the stderr `STALE viewport rect` line never appears |
| 2 | No way to add an object — there is no menu bar | **Chisel** | **DONE** — menu bar landed (`src/editor/Menu.cpp`), driving the existing `World::createPrimitive/destroy/duplicate` |
| 3 | ~~Nothing can be selected by clicking a visible object in the viewport~~ | Editor | **FIXED 2026-10-18:** mouse capture was being rejected over Area D itself, and rays used full-window coordinates despite the offset viewport render rect. The editor now gates input by viewport-body ownership, converts window points to framebuffer pixels, subtracts the viewport origin, and uses viewport-local dimensions. The GPU regression proves the real click selects the expected object. Edit Mode now has vertex/edge/face click selection, overlays, and initial E/X/M modeling tools (items 115/117); drag-modal/multi-face extrusion remains. |

**Scope correction:** Mesh Edit Mode was added in wave 5. The first modeling pass is
implemented and tested (viewport sub-element selection/overlays; E/X/M; Tool and
Topbar actions; undo/redo). Modal extrusion, multi-face region extrusion, edit-mode
box selection, and broader topology tools remain follow-up work.

---

## What is missing compared to Blender

Re-audited against the **tree** on 2026-10-17, by reading code rather than status
columns. Wave 4 changed the picture substantially, so most of the previous version of
this section is superseded.

**Verity's item-103 verification, quoted verbatim** (these are her words, from findings
`kb_mvbkbqewLxAHaNI1hrVU` and `kb_mvbjyu1pz85abQroLY-2`):

> "Item 103 (shadow map only): LANDED — grep 140 refs (was 0), shadow lands where
> geometry predicts, timing at 1024/2048"
>
> "Item 103: shadow map LANDED (inside footprint drop 122.08 vs outside 0.10,
> caster-removal + movement controls) and modal transforms LANDED; Chisel selftest
> exits 0 even when it FAILs"

So both things that were **not** implemented at wave 3 (`kb_mvbc3zofMH9tlTfpywwi` said
so, accurately, for that tree) are implemented now — I re-verified in code: the shadow
pass is at `Renderer.cpp:478-564` (gated on `shading == Rendered` at :649) and modal
transforms are reached from `Editor.cpp:196`. **The one clause of hers that is no
longer true is the last**: the self-test exit hole is closed — App now fails the run on
`-1`, and on no verdict at all (see D33's receipt).

**Build state, measured for this audit:** clean configure/build exit 0, default `ctest` **100% of 37**, smoke
exit 0 (`drawCalls=3 triangles=12`).

### Matches Blender (shipped and working)

**The UI structure is built; fidelity work continues.** One whole-window DockSpace with a code-built
fixed layout and real splitters (`Workspace.cpp` uses `DockBuilderSplitNode` for the
right column, the right-column split and the Timeline strip; the 3D view is the
retained central node that `Editor::viewportRect()` reports), a header per area, a
topbar, a pinned status bar, and `T`/`N` as regions inside the viewport.

The Viewport Header's right-control group reserves 700px so the Edit Mode
Vertex/Edge/Face combo is not clamped by the former 660px budget; the label-fit
test measures the live `drawArea()` at the default 996px viewport width.

**Transforms: modal `G`/`R`/`S` is implemented; gizmo tests are green.** Item 149's full self-test is 17/17 checks: 17 individual PASS assertions plus one PASS summary (18 `[selftest] PASS` log lines total); repeated in this audit with `--frames 140`. The perspective hit-test setup and axis-color issues are corrected; manual gesture feel remains a human check.

Also shipped: object editing (create / parent / transform / select / rename /
visibility / duplicate / delete), undo/redo with drag coalescing and reversible delete,
Outliner, Properties (transform, material, light), the menu bar, viewport picking,
grid + world axes, orbit / pan / dolly / orthographic / standard views / framing,
light objects (directional **and** point, cap 8, once-per-change logging, documented
default sun), the world background colour **and** its flat-ambient contribution
(`worldbg` in `.ember`), **a shadow pass** (directional, one casting light, per-object
`castShadow`/`receiveShadow`, with honest warnings when it cannot run).
The four renderer shading implementations are real (items 97/142); pixel differences are fixture-specific: item 97's
controlled floor/cube/two-light harness produced four different images, and item 142 measured 215,043 changed
pixels for Solid vs Rendered with shadows/world-lighting off. The empty default startup has no mesh (App.cpp adds
one only with `--demo-object`). With no mesh draw, all four modes have byte-identical renderer output in the empty default scene; pixel-distinctness is fixture-scoped, not universal. Item 97 and item 142 use non-empty controlled fixtures.
All four are present in the viewport header combo and four-slice Z-pie source; item 154's pie and all four entry points are covered by headless tests. Menu.cpp lists only Solid/Wireframe. Material slots and per-triangle mapping are consumed by render; `.ember` v8 now serializes editable topology and per-face material indices. The v8 SceneIO regression executable is compiled but blocked by Application Control. CTest currently registers 82 targets (51 default + 31 GPU).

### Does not match — open, in priority order

| # | Gap | State |
| - | --- | ----- |
| 1 | ~~Layout: areas / headers / splitters~~ | **CLOSED 2026-10-17** — `Workspace.cpp` builds a DockSpace with `DockBuilderSplitNode` splitters and a fixed default layout (3D centre, right column split, Timeline strip); the 3D view is the central node. Measured consequence: a 1280x720 window yields a **996x515** 3D area, which the renderer scopes to and the packet's aspect now comes from (D29 wiring in `App.cpp`). |
| 2 | ~~Modal transforms (`G`/`R`/`S`)~~ | **CLOSED 2026-10-16 — my audit was wrong, in the reverse of the shadow trap.** The code exists **and is called**: `src/editor/Modal.cpp` (G/R/S at :151/153/155, Esc + right-click cancel :165, Enter/KeypadEnter confirm :169, X/Y/Z world-then-local :177, Shift+axis plane :177-184, numeric entry via `strtof` :195-206, bitwise cancel from `before_` :291-313, exactly one `undo.push` :318), reached from `Editor.cpp:196` on the input path, with the live readout in `StatusBar.cpp`. Verity caught it (item-88 finding `kb_mvbd4t5vYI2gS7DgKfal`); I re-verified in the code. **Lesson recorded in TEAM.md: grep for a CALL SITE, not a definition — "present" is not "reachable".** |
| 3 | ~~Gizmo drag accuracy / visual-handle agreement~~ | **Tests now pass.** The editor projects the actual handle segment for hit-testing and the modal drag uses the same world-axis projection; the independent test fixture was corrected to click the actual perspective endpoint. `render.gizmo_axis_colours` also passes after brighter hover/active colors preserve X/Y/Z hue dominance. Residue: visual feel still deserves hand verification. |
| 4 | ~~`Renderer::setViewportRect` + `Editor::viewportRect`~~ | **CLOSED 2026-10-17** — both implemented and in use: `Renderer.cpp:381-393` derives the GL origin (top-left rect → bottom-up), sets `glViewport` **and** `glScissor` **and** enables `GL_SCISSOR_TEST`, so the 3D pass cannot paint into a neighbour; `readViewport` adds the viewport origin and clamps to the area. |
| 5 | ~~Shadow pass~~ | **CLOSED 2026-10-17 — it landed in wave 4** (Verity: "shadow map LANDED … grep 140 refs (was 0), shadow lands where geometry predicts"; her footprint measurement is quoted above). In code: `Renderer.cpp:478-564`, gated on `shading == ShadingMode::Rendered` + `shadows.enabled`, with `shadowLightIndex` from scene and honest warnings for "no caster" / "not directional". |
| 6 | ~~The four shading modes are not reachable from the UI~~ | **Reachability is present in source:** Editor.cpp's viewport header has Wireframe/Solid/Material Preview/Rendered; Pie.cpp defines four Z-pie slices. Renderer implementations/pixel evidence are items 97 and 142. Item 154 (Z pie) still has no owner finding/receipt, so that distinct backlog item is not credited closed. Menu.cpp's top-level shading entries remain Solid/Wireframe only. Do not call modes universally “pixel-distinct”: the evidence is for named non-empty harness fixtures; default startup has no mesh draw. |
| 7 | **Render-side material slots / per-face assignment (item 144)** | **Implemented path:** scene emits one triangle slot per index triangle and render consumes per-face assignments; `.ember` v8 now serializes editable topology/material indices. Existing pixel tests cover slot semantics. New v8 save/load tests compile but could not launch under Application Control; exhaustive modifier mapping remains. |
| 8 | ~~3D cursor + `PivotMode`~~ | **Core/scene behavior landed:** PivotMode is consumed by gizmo/modal pivot resolution (item 149); World cursor is persisted as a scene datum (item 157). Residue: multi-object transforms still operate on the active object only; edit-mode Individual Origins falls back to the object origin (item 149). |
| 9 | ~~Collections UI~~ | **Implemented in `Outliner.cpp` and exercised by `test_editor_outliner`:** nested Collections, create/delete context actions, visibility toggle with undo, and drag/drop membership assignment. The previous “no tree/no assignment” claim was stale. |
| 10 | **Background gradient** (D28) | Reserved shape in `ViewportSettings`; not built. |
| 11 | **Performance**: `buildPacket` is O(n²) | Verity's item-61 measurement: 0.58 → 3.80 µs/object, 19 ms at 5000 objects, from a linear-scan `getImpl`. Not a correctness bug, not yet felt at demo sizes. |
| 12 | **Selection outline** | A basic orange extruded-hull rim pass exists and has GPU coverage; Blender's exact outline style/selection semantics remain a visual-fidelity gap. |
| 13 | Textures, anti-aliasing/post, final render output, OBJ/glTF import/export, native file dialogs | PPM/PFM/BMP image loading, UV/world images and PFM/PPM output APIs landed; updated image tests are blocked. Viewport-preview save exists but is not a full-resolution render workflow. TAA/post, import/export and native dialogs remain. See `docs/RENDERING-ROADMAP.md`. |

### Next, in order

1. **Verify the four shading entry points** — the viewport header already exposes all four; Pie.cpp has four Z slices, but item 154 has no receipt, while the renderer pixel fixtures are items 97/142. Keep the pixel claim fixture-scoped; the empty startup scene has no mesh draw.
2. **PivotMode/cursor core wiring landed** (items 149/157). Residue, not a missing feature: multi-object transforms still operate on the active object only; edit-mode Individual Origins falls back to object origin (item 149).
3. **Rendering:** raster and Cycles-style backends stay independent. The A/B wipe is Ember's distinctive comparison feature; image textures, environment support, per-pixel adaptive sampling and viewport-preview saving are initial implementations. Next verify v8/image-loader tests, implement TAA only with correct history invalidation, strengthen environment/BSDF references, and add full-resolution final output. See the source-backed `docs/BLENDER_CYCLES_EEVEE_SOURCE_SURVEY.md` and `docs/RENDERING-ROADMAP.md`.
4. **`buildPacket`'s O(n²)** (Slate) — 19 ms at 5000 objects from a linear-scan
   `getImpl`. Not urgent, but it is the only measured performance defect and it will be
   invisible until it is not.
5. Then the long tail: background gradient, temporal AA/post, a full-resolution final-render UI, import/export, and remaining modeling gaps. The CPU path-trace viewport and viewport-preview writer are implemented; transparency has a GPU source-over pixel proof.

**The one-line summary for a human:** Ember3D has a Blender-inspired tiled editor,
Object Mode drag selection, and a distinctive Raster/Cycles A/B wipe for comparing
its separate renderers. Remaining work includes verified asset I/O, node materials,
Eevee temporal/pass systems, production Cycles features, full-resolution final output,
and remaining modeling gaps.

### Deliberately out of scope (agreed, do not start without a decision)

| Gap | Where it was ruled out |
| --- | --- |
| **Knife, loop cut, proportional editing** | Still out of scope after wave 5; sub-element editing, moving vertices/edges/faces, extrude/delete/merge are in scope and have landed incrementally (items 109-119). See `WHAT-REMAINS.md` §3. |
| Spot and area lights, shadows, IES/light textures | D19 scope boundary. **Spot is reserved, not forgotten** (Chisel asked 2026-10-16): when `render` actually consumes `packet.lights` and shades point lights, I add `LightType::Spot` in one pass — `Light::coneAngleRadians` (full cone), `Light::softness` (penumbra fraction of the cone), direction from the object's local −Z like a directional light, `radius` reused for falloff — and Slate adds a `spot` KIND word to the `.ember` light record. Until then the menu entry stays **disabled**: a disabled item that names the gap beats an enabled one that silently does nothing. |
| Node-based materials/shading | The current D12 MaterialDesc is deliberately compact; a graph/IR needs an explicit shared authoring contract and separate backend compilation, as detailed in the source survey. |
| Normal/tangent gizmo spaces, snapping targets beyond Ctrl, 2D screen-space axis | D20 scope boundary |
| Constraint systems, animation, physics, particles | never scoped |

### Not started, not yet decided

| Gap | Evidence in the tree |
| --- | --- |
| **Background gradient** (item 57) | **Not built, shape reserved (D28).** Blender's Solid-mode background gradient is viewport *display* state, so it lands in `ViewportSettings` (a top/bottom colour pair + a flag), **not** in `WorldBackground`. Precedence when it lands: Solid/Wireframe/Material = viewport gradient, Rendered = world background. Whoever implements it must update Halide's byte-exact background assertion in the same change. |
| **No import/export except `.ember`** | `SceneIO` writes `.ember` v8 and reads v1-v7; there is no OBJ/glTF/FBX import/export. v8 edit-mesh persistence tests are not runtime-verified yet. |
| **Texture pipeline is a dependency-free PPM/PFM/BMP subset** | UV base-color and environment image sampling exist, but not Blender's broad image formats, packed assets, node graphs, or a complete asset browser. Updated loader/color-space tests remain blocked. |
| **No linked objects / collection instancing** | `World` supports one Collection membership per object; multiple Collection links and instancing remain unimplemented |
| **Camera viewport glyph / `DrawItem` representation** | Scene camera data, Outliner object rows, v8 persistence, and active-camera packet matrices are implemented; no camera wireframe glyph is drawn and unmeshed cameras are not `RenderPacket::items` entries (audit artifact #33/34/209). |
| **No fly/walk navigation** | `OrbitCamera` only: orbit, pan, dolly, standard views, ortho, framing |
| **No temporal post/anti-aliasing** | HDR storage, fixed Reinhard tone mapping and sRGB display resolve exist; there is no MSAA/TAA, history buffer or broader post-process graph. TAA was deferred rather than risk stale history/ghosting without scene revision invalidation. Transparency blending is implemented and pixel-tested. |
| **Transparency pixel proof** | Source code implements alpha classification/compositing in item 143; opaque first, stable far-to-near transparent sort, depth test retained, depth writes disabled, source-over blend. No dedicated alpha pixel test is present under tests/ yet; item 158 remains the verification target, and the board result currently cites unrelated evidence. |
| **No production final-render workflow** | PFM/PPM writers and an async viewport-preview save control exist; no full-resolution production render settings/result workflow is connected. `EMBER_FRAME_DUMP` remains diagnostic BMP capture only. |
| **No native file dialogs** | `Menu.cpp` Save/Load uses an in-app path field |
| **No preferences, node editor, asset browser, layout persistence** | none present in `src/editor` |
| **Grid snapping / numeric transform entry** | Ctrl snapping is frozen in D19/D20 but unimplemented |

**The one-line summary for a human** is at the end of "Next, in order" above; this
section lists the gaps, not the story.

**Nothing is pending** in interface terms — every request that used to be listed here
has landed (the camera navigation API, `World::bounds()`/`selectionBounds()`, `Rect` +
`setViewportRect` + `viewportRect`, the modal-transform helpers). It is kept as the place
to put the *next* one, and as a reminder of what it looked like: the previous version
listed four blocked items with requested signatures, and all four were delivered inside
two waves.

`core` answers interface requests; a request that changes a struct in `Types.h` is a
`core` change and goes through Ash. Additive read-only accessors on your own class need
only be announced (see TEAM.md §8).

### Blender navigation key table (WIRED in `App.cpp`, item 5)

Implemented 2026-10-16. All gated on `Editor::wantsViewportFocus()`.

| Key | Action | Call |
| --- | ------ | ---- |
| Numpad 1 | Front view | `setView(StandardView::Front)` |
| Numpad 3 | Right view | `setView(StandardView::Right)` |
| Numpad 7 | Top view | `setView(StandardView::Top)` |
| Ctrl + 1/3/7 | Back / Left / Bottom | `setView` of the opposite |
| Numpad 9 | Opposite of the current axis view | resolved in `NavActions::toggleAxisView()` from the view direction |
| Numpad 5 | Toggle orthographic / perspective | `setOrthographic(!isOrthographic())` — keeps the on-screen scale |
| Period (`.`) | Frame the selection | `World::selectionBounds()` → `frameBounds` |
| Home | Frame everything | `World::bounds()` → `frameBounds` |
| Shift + middle drag | Pan | `pan(dx, dy, framebufferHeight)`, deltas scaled from window coords |
| Middle drag | Orbit | `orbit(dx, dy)` |
| Wheel | Dolly | `dolly(scroll)` |

**Never fighting ImGui:** every key and the wheel are gated on `wantsViewportFocus()`; the middle-mouse gesture is
*latched at press* (orbit vs pan chosen once), so it survives the cursor crossing a panel and Shift cannot switch it
mid-drag.

**Evidence, no human needed:** `EMBER_NAV_SELFTEST=1` runs one of every action through the same `NavActions` methods
the keys call and prints the resulting camera state (`nav Front yaw=0.0 …`, `nav Left yaw=-90.0` for numpad 9 from
Right, orthographic keeping `dist=6.00`, `frame all` going `6.00 → 2.40`). It caught a real bug in the first cut:
the numpad-9 table had X and Y inverted, so from Right/Top the camera silently did not move. The key→action binding
itself and pan/orbit *feel* still need hands on the app.
