# Ember3D — what remains

> **Wave 5 is underway:** see [`WAVE-5.md`](WAVE-5.md) for the scoped plan and item owners.
>
> **Cost rule — measured, not theorised:** use frontier models on a **fresh session with a
> tight brief**, never by escalating a long-lived one. One-word probes: `gpt-6-luna`
> **+$0.095**, `kimi-k3` **+$0.99**, `claude-opus-5-5` (frontier) **+$3.28**. Same work
> over 15 minutes: `gpt-6-luna` **+$0.75** vs `kimi-k3` **+$4.76** (**6.4x**). Context
> drives it: sessions re-read **~700k cached tokens per step**; the frontier probe was
> **+$3.28 for one word**. Fresh context is the cost control.
>
> **Declaration-order rule (three incidents): nothing is used before it exists, and nothing
> is inferred that the build system cannot see.** `CollectionId` before declaration was
> loud (compiler); `__has_include` was silent (CMake missed the new editor in a green build;
> use tracked `EMBER_HAS_*` flags); `scaleStrippedRotation` before definition was shared
> (peer's linker). Declare above first use, use tracked build dependencies, and grep before
> adding a definition.

**Wave 3: Blender fidelity.** The human used the app and said:

> *"the UI is awkward (I don't like the floating menus), moving object barely works,
> just copy how blender does it, which also true for most things including UI"*

That is a specification. Where this file and your instinct disagree, **Blender wins** —
and if you are unsure what Blender does, say so and ask rather than inventing a
third way. The item list is in the project backlog; this is the *behaviour* to hit.

---

## 1. Layout: delete the floating windows

**Blender has no floating panels.** Panels you can drag around the screen are the
"awkward" the human is complaining about. Blender's model:

| Blender concept | What to build |
| --- | --- |
| **Topbar** | A thin bar pinned to the top: workspace tabs, then anchored drop-down menus (File / Edit / Render / Window / Help). Menus drop *from* this bar. |
| **Area** | A region of the screen running one editor. Areas tile the window completely — no gaps, no overlaps, nothing on top of anything. |
| **Splitters** | Draggable borders between areas so the layout can be resized. |
| **Header** | A strip belonging to an area, at its top or bottom. Left end = the editor-type selector. Right end = that editor's controls. Every area has one. |
| **Toolbar (`T`)** | A left region *inside* the 3D viewport: a vertical column of tool buttons. Not a floating palette. |
| **Sidebar (`N`)** | A right region *inside* the 3D viewport: tabs down the edge (Item / Tool / View), collapsible panels within. |
| **Status bar** | Pinned to the bottom edge of the window, always visible, showing hints and the live transform readout. |
| **Properties editor** | A vertical icon column down the left (Render / Output / Scene / World / Object / Material / …) with a single scrollable column of collapsible panels. Not one window per category. |

**Default startup screen** (match this, it is the layout the human expects):
3D Viewport filling the centre-left; **Outliner** top-right; **Properties**
bottom-right; **Timeline** across the bottom; status bar under everything.

Rules to implement, not to interpret:

- One `DockSpace` covering the whole window. Set the ImGui flags so windows **cannot**
  be moved, resized, collapsed or brought to front.
- A fixed default layout applied on first run, deterministically — no `.ini`, so it
  never drifts or surprises.
- The main menu bar is the **Topbar**: it stays at the top and does not float.
- Every editor gets a **header**, including the 3D viewport — its header carries
  shading mode, overlays, transform orientation / pivot / snap, and the gizmo toggles.
- The 3D viewport's Toolbar and Sidebar live **inside** the viewport area.

## 2. Interaction: make transforms actually work

"Moving object barely works" is the other complaint, and it is an interaction-model
problem, not a maths bug. Do it Blender's way:

**Modal transforms (`G` / `R` / `S`)**
1. `G` (or `R`, `S`) begins the transform. It follows the mouse with **no mouse button
   held** — the button state is released, the operation is modal.
2. **Left-click or `Enter` confirms. Right-click or `Esc` cancels and restores the
   original value exactly.**
3. `X` / `Y` / `Z` constrain to a **global** axis. Pressing the same key again switches
   to the **local** axis. `Shift`+`X`/`Y`/`Z` constrains to a **plane**.
4. Typing a number sets an **exact** value; `Shift` on its own refines (precision);
   `Ctrl` toggles **snapping**.
5. The running value is displayed live as you drag — in the viewport header, beside the
   transform axes.
6. **Exactly one undo entry per confirmed gesture.** Never one per frame.

**Gizmos**
- Dragging a gizmo handle must move the object **along the axis the handle represents**,
  from any camera angle, by the amount the cursor moved *in that axis's screen
  projection*. Raw pixel deltas are why it "barely works".
- Axis handles coloured Blender's way (X red, Y green, Z blue), plus plane quads;
  hover highlights, active is brighter; constant screen size regardless of distance.
- `Shift` for precision, `Ctrl` for snapping, `Esc`/right-click cancels.

**Navigation** — middle-drag orbits, `Shift`+middle-drag pans, wheel zooms toward the
cursor, numpad `1`/`3`/`7` are front/right/top (`Ctrl` = the opposite), numpad `5`
toggles ortho, numpad `0` looks through the camera, `.` frames the selection, `Home`
frames everything.

**Selection** — click selects, `Shift`+click adds, `A` selects all, `Alt`+`A` deselects,
drag from empty space box-selects, `X`/`Delete` deletes, `Shift`+`D` duplicates.

## 3. Fix what closed without landing

One item was marked done last wave and is **not implemented**: the **shadow map**.
`ShadowSettings`, `castShadow`/`receiveShadow` and `shadowLightIndex` exist in
`Types.h`, and `Renderer.h` claims the renderer honours them — but `Renderer.cpp`
contains **zero** references to shadow. Either implement it or take the claim out of
the headers. Do not leave a header asserting behaviour the code does not have.

**Re-audit the whole plan against the tree.** For every item marked done in waves 1–2,
check the code. Publish the ones that did not land. A plan that says "done" when it is
not is worse than no plan.

## 4. Fidelity of everything else

The human said Blender's way is true "for most things including UI". So when you build
anything:

- **Match Blender's default dark theme**: header heights, widget sizes, spacing,
  selected (orange) vs active (lighter blue) vs hover, and the transparent-ish region
  backgrounds. Blender's selection colour is not blue.
- **Prefer Blender's word** over a new word: Outliner, Properties, Sidebar, Toolbar,
  Area, Header, Region, Collection, Modifier, "Object Mode".
- **Pie menus** for the things Blender uses them for: `Z` for shading mode.
- If something is genuinely different here (unsupported Blender modes/operators), say so
  plainly rather than half-copying it. Mesh Edit Mode is in scope; its remaining
  modeling gaps are listed below.

---

## Out of scope — decided by a human, do not start

**Mesh edit mode is IN SCOPE as of wave 5.** Sub-element selection, moving
vertices/edges/faces, extrude, delete and merge are Tracks B and C of `WAVE-5.md`.
The initial path is implemented: Tab enters Edit Mode, visible vertex/edge/face
selection and overlays work, and E/X/M plus the Tool tab/Edit > Mesh expose
undoable modeling operations. E now starts a mouse-driven face extrusion preview
with Enter/click confirm and Esc/right-click cancel. It supports a single face or
multiple disjoint selected faces; selected faces sharing a vertex are refused
because proper region extrusion is not implemented yet. Edit Mode box selection
and the broader Blender operator set remain. This file said
"deferred" and that was right for wave 3 — but leaving it would tell a fresh session to skip
the current headline work, so it is corrected rather than deleted.

Still out of scope: **knife, loop cut, proportional editing**, and **performance work**
(Verity measured the O(n²) packet build and correctly left it alone). Object-level fidelity in
this file still applies.

## House rules that still apply

- One owner per file. Need a change outside your paths? **Message the owner.**
- No `build` or `module` claim without a pasted command receipt.
- `bin/Ember3D.exe --frames 60 --exit --demo-object` is the runtime proof;
  `ctest --test-dir build-gpu -C Release` (needs `-DEMBER_ENABLE_GPU_TESTS=ON`).
- If a decision lands after a freeze, re-freeze. Only the Lead may re-freeze.
