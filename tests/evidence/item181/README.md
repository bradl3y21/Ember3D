# Item 181 — QA fixture migration to Area D child Regions

Owner: Verity · QA. Files changed: `tests/` only.

## What item 181 changed (Chisel, `src/editor`)

`drawArea()` now draws the Toolbar (`T`) and Sidebar (`N`) as **child Regions of the
Area D window** (`ImGui::BeginChild("##toolbar")` / `("##sidebar")`, hence the real
window names `Area D/##toolbar_<hash>`), and returns the **middle** Region — the
rectangle the renderer draws into (`Editor::viewportRegion_`). Before item 181 the
Toolbar and Sidebar were standalone top-level ImGui windows.

## Why the fixture had to move

`tests/test_editor_layout.cpp` asserted the OLD shape: it looked for top-level
windows literally named `##toolbar` / `##sidebar` and called `drawToolbar` /
`drawSidebar` outside any window. That is now false by construction.

## Changes (tests/** only)

`tests/test_editor_layout.cpp`

- `Rig::frame()` drives the **real** `drawArea(Area D, …, Viewport)` path instead of
  calling the Toolbar/Sidebar draw calls at top level, and keeps the returned body.
- `launch()` / `checkTiles()` now read and assert Area D's body, toolbar and sidebar
  Regions at all three sizes (1280x720, 1920x1080, **1366x777** — the awkward size
  where a rounding gap between the three Regions would show).
- `layout_toolbar_and_sidebar_lie_inside_the_viewport` now asserts: both are child
  windows of the Area D window, neither is a dock node, widths are
  `toolbarRegionWidth()` / `sidebarRegionWidth()`, and Toolbar | renderer body |
  Sidebar abut with exact edges, one top and one height.
- `layout_no_window_can_float` asserts there is **no** top-level `##toolbar` /
  `##sidebar` (the floating panel the human called awkward) and that both are drawn
  as children of Area D; control: `layout_control_a_floating_toolbar_is_reported`
  proves that predicate can fail.
- `layout_default_is_identical_across_two_launches` now also compares the body,
  toolbar and sidebar rects.

`tests/test_editor_camera_gate.cpp`

- `camera_gate_is_false_while_a_popup_is_open_and_true_again_when_it_closes` clicked
  `body.x + 40` to open the header's editor-type combo. Item 181 moved the body's
  left edge from the Area edge to the Area edge + the Toolbar (136 px), so that
  point now lands on the header strip *past* the combo and no popup opened. The
  click is anchored to the **Area D window's** left edge again.

## Receipts

```
.\bin\test_editor_layout.exe
== 17 test(s), 9 skipped (no --gpu), 354 check(s), 0 failed check(s) in 0 test(s) ==   exit 0

ctest --test-dir build -C Release
  build.gate ............................... Passed
  test_editor_layout ....................... Passed
  editor.layout_invariants_real_editor ..... Passed
  editor.layout_content_fits_real_editor ... Passed
  editor.camera_gate_follows_the_pointer ... Passed   (was Failed before the click fix)

.\bin\test_editor_camera_gate.exe --gpu --only camera_gate_
== 2 test(s), 31 check(s), 0 failed check(s) ==   exit 0
```

Full run: **41/51 pass, 10 fail** — byte-for-byte the same 10 failures already
recorded in `tests/evidence/item178/ctest-full.txt` before item 181, i.e. no
regression from this change:

```
17 test_editor_gizmo            25 editor.vocabulary_and_theme_contract
26 editor.theme_token_swap       36 editor.header_controls_fit_their_labels
39 editor.viewport_regions_...   40 editor.viewport_click_selects_...
41 render.audit_claims           44 render.no_pixel_outside_render_rect_...
45 render.alpha_and_material_slots  46 render.gizmo_axis_colours
```

Measured region geometry (CPU rig, 1280x720 — Area D `0,19 996x543`):
`toolbar (0,47 136x515) | body (136,47 560x515) | sidebar (696,47 300x515)`.
Live app (`camera-gate` GPU rig, 1280x720): `body x=136 y=73 w=560 h=489`.
