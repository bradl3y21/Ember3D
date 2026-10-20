# UI defect inventory — WHAT-REMAINS §§1 and 4

Item 167's input to the fixes in this wave. Every entry below states the observed
defect, the Blender/WHAT-REMAINS target, the source location, and a measurement. This
is an inventory, not a claim that the defects are already fixed.

## Re-audit after the UI workstream — 2026-10-18

The capture and defect rows below record the earlier item-167 baseline; they are
historical measurements, not today's implementation status. Since that capture,
the Properties rail has an actual scrollbar and fits all seven categories at the
default size; Render/Output controls and object-local Material Slots/Face Assign
are interactive; Toolbar/Sidebar controls and Viewport Add menus are hit-tested;
Outliner has a visible Scene Collection tree, active/selected feedback, compact
kind/visibility controls, and object-to-object/collection drag/drop; the Timeline
split is set to D41's 20% startup share; and the status/header/splitter colors use
the semantic dark-theme tokens. Regression coverage is in `test_editor_properties`,
`test_editor_outliner`, `test_editor_layout`, and `test_editor_ui_chrome`; default
CTest 40/40 and full GPU CTest 70/70 pass. Remaining visual quality still needs a
fresh human review of the live frame. The inventory below should be read against
that status note, not as a claim that these initial measurements persist.

## Capture and method

The primary frame is `docs/evidence/item167/live-default.png`: 1280×720 client capture
from the isolated Release app, `build-verity/bin/Ember3D.exe --frames 100000 --demo-object`,
captured with `CopyFromScreen`. It shows one `SmokeCube`; the root `bin/Ember3D.exe`
was not closed or replaced. Properties tab captures (`properties-render.png`,
`properties-output.png`, `properties-material.png`) and the selected-object Outliner
capture (`outliner-selected.png`) use the same client size and executable. Chisel's
fresh header capture is `item170-header.png`. Crops are saved alongside them. All
rectangles use the 1280×720 client image's top-left origin.

### Surface coverage

| Visible surface | Measured frame region | Audit result |
| --------------- | --------------------- | ------------ |
| Topbar | `(0,0)–(1279,31)`; 32 px high | 3 workspace tabs and File/Edit/Render/Window/Help are visible; matches WHAT-REMAINS §1. |
| Viewport Area/Header | Area centre-left; Header crop `(0,37)`, 997×42 | Area Header exists; UI-01/02 are below. |
| Outliner Area/Header | `(997,40)–(1279,322)`; header crop in item170 `(925,39)–(1279,75)` | Outliner is top-right as required; UI-01/11. |
| Properties Area/Header | `(997,331)–(1279,562)`; header crop in item170 `(925,331)–(1279,367)` | Properties is bottom-right; UI-01/06–10. |
| Toolbar (`T`) | visible at viewport left; crop `toolbar-crop.png` | UI-03. |
| Sidebar (`N`) | viewport-right region `(688,81)–(988,553)`, 300×472 | Item/Tool/View tabs are present; Tool/View contents are UI-04/05. |
| Timeline | `(0,571)–(1279,688)`, 118 px | Full-width bottom placement matches §1; contents and height are UI-12/13. |
| Status Bar | `(0,689)–(1279,719)`, 31 px | Pinned along the bottom with tool/snap/help text; matches §1. |

## Defects

| ID / surface | What is wrong; what Blender does instead | Source cause | Measurement |
| ------------ | --------------------------------------- | ------------ | ----------- |
| **UI-01 — Area Headers** | There is a blank spacer after the Header fill before editor content. WHAT-REMAINS §1: “Every editor gets a header.” D42 pins a 27 px Header; the Area should proceed from that strip to its Region, not spend another ~9 px on empty band. | `src/editor/Editor.cpp:344-349` draws the 27 px fill; `:378` adds a separator/default ImGui spacing before content. | Chisel's `item170-header.png`: Outliner Header crop `(925,39)–(1279,75)` = 36 px; Properties `(925,331)–(1279,367)` = 36 px; Timeline `(0,571)–(1279,605)` = 34 px. The fill is 27 px; total header-to-content band is 7–9 px taller. |
| **UI-02 — 3D Viewport Header** | The snap-target label is cut off: `Increment` appears as `Incremen`. WHAT-REMAINS §1 requires the viewport Header to carry snap controls; the chosen value must remain readable. | `src/editor/Editor.cpp:294-297` gives the snap-target combo a fixed 74 px width. | `viewport-header-crop.png` (origin `(0,37)`, 997×42): 8 of 9 label characters fit before the dropdown arrow. The frame is 1280×720. |
| **UI-03 — Toolbar region** | Toolbar is a separate top-level no-docking ImGui window positioned over the rendered viewport, not an Area-owned Region. WHAT-REMAINS §1: Toolbar is inside the 3D Viewport and “not a floating palette.” Blender Regions divide an Editor; the Toolbar is the left-side region. | `src/editor/Toolbar.cpp:42-49` calls `SetNextWindowPos` then `ImGui::Begin("##toolbar", … NoDocking …)`. | `toolbar-crop.png`: panel bbox `(8,90)–(151,193)` = 143×103 = 14,729 px over the grid. The crop shows the grid hidden beneath its opaque panel. Blender Manual: [Regions](https://docs.blender.org/manual/en/latest/interface/window_system/regions.html). |
| **UI-04 — Sidebar / Tool** | Tool tab is a placeholder, not the collapsible Tool panels required by WHAT-REMAINS §1. | `src/editor/Sidebar.cpp:136-140` emits only `TextDisabled("Orientation controls are in the viewport header.")`. | Sidebar rect in the live frame is 300×472 px. Draw-path count for Tool: **1 disabled text, 0 interactive controls, 0 collapsible panels**. The region rect is visible in `sidebar-crop.png`; this is a source-count measurement because the Tool tab was not captured in a separate tab-state frame. |
| **UI-05 — Sidebar / View** | View tab is also a placeholder, not View panels. WHAT-REMAINS §1 specifies Item/Tool/View tabs with collapsible panels within. | `src/editor/Sidebar.cpp:142-146` emits only `TextDisabled("Shading and overlay controls are in the viewport header.")`. | Same 300×472 px Sidebar rect. Draw-path count for View: **1 disabled text, 0 interactive controls, 0 collapsible panels**; source-count measurement, not an assumed visual. |
| **UI-06 — Properties category rail** | The required vertical icon column is rendered as letter abbreviations (`R`, `O`, `S`, `W`, `Ob`, `M`), not Blender category icons. WHAT-REMAINS §1 calls for a vertical icon column. | `src/editor/Properties.cpp:294-304` defines `icons[]` as strings and draws each using `ImGui::Selectable`. | `properties-crop.png` (crop origin `(990,325)`, 290×245): the 42 px rail contains letter glyphs; no icon glyphs are drawn. The live frame visibly shows R/O/S/W/Ob. |
| **UI-07 — Properties category clipping** | Material is the sixth category, but it is below the visible rail and the child cannot scroll. | `src/editor/Properties.cpp:297-304`: 42 px child uses `NoScrollbar|NoScrollWithMouse`; each of six entries is 34×32 px. | Properties body gives the rail ~183 px; six buttons require `6×32 + 5×4 = 212` px at the current 4 px item spacing. Deficit ≈29 px; only 5/6 labels (R/O/S/W/Ob) appear in the 1280×720 capture, Material is absent. |
| **UI-08 — Properties / Render** | Render is a visible category whose panel contains no Render controls. WHAT-REMAINS §1 describes a scrollable column of collapsible settings panels. | `src/editor/Properties.cpp:314-316`. | `properties-render.png`, Area crop `(997,331)–(1279,562)`: one `Render` collapsing header, one disabled “Render settings are not available yet.” line, **0 settings widgets**. |
| **UI-09 — Properties / Output** | Output is likewise a visible category with no Output controls. | `src/editor/Properties.cpp:318-320`. | `properties-output.png`, same Area crop: one `Output` header, one disabled “Output settings are not available yet.” line, **0 settings widgets**. |
| **UI-10 — Properties / Material slots** | Material exposes Base Color/Metallic/Roughness but says “Material slots are not available yet”; ordered slots and renderer-side per-face consumption now work, but the visible panel still has no slot UI. D45 freezes the remaining target: active-slot fields/list/add/remove and Edit Mode Face-selection Assign. | `src/editor/Properties.cpp:280-286`; packet mapping is implemented, while slot selection/add/remove/Assign and editable-face persistence remain. | `properties-material.png`: the Surface controls are visible and the disabled slot notice appears at the bottom; **0 slot select/add/assign/remove controls**. This is the measured UI defect. |
| **UI-11 — Outliner Collection tree** | Outliner shows `SmokeCube` directly under its header with no visible Collection/root row. Blender Outliner is a tree that manages object/collection relationships; WHAT-REMAINS §4 reserves the canonical term `Collection`. | `src/editor/Outliner.cpp:237-244` draws explicit top-level collections, then implicit-root objects directly. | `outliner-crop.png`, Area `(997,40)–(1279,322)`: **1 object row, 0 Collection rows**. This matches the still-open Collections UI gap in ROADMAP. Blender Manual: [Outliner Introduction](https://docs.blender.org/manual/en/latest/editors/outliner/introduction.html). |
| **UI-12 — Timeline contents** | Timeline explicitly says it is not connected to animation; it has no channel rows or keyframe markers. Blender's Timeline is used to jump frames, manipulate keyframes and control playback. | `src/editor/Timeline.cpp:15-28,39-54`. | `timeline-crop.png`, `(0,560)`, 1280×130: **1 disclaimer line, 2 range fields (Start/End), 1 ruler/playhead, 0 channels, 0 keyframe markers**. Blender Manual: [Timeline](https://docs.blender.org/manual/en/latest/editors/timeline.html). |
| **UI-13 — Timeline height** | Timeline is full-width but smaller than the frozen D41 startup proportion. | `src/editor/Workspace.cpp:40-45` splits the DockSpace bottom at `0.18f`; D41 freezes 20%. | Live frame Timeline band y=571–688 = 118 px. DockSpace workspace band y=32–689 = 657 px; `118/657 = 18.0%`, about 13 px short of the D41 20% target (~131 px). |

## Surfaces measured with no defect recorded

The Topbar is 1280×32 px and shows Layout/Modeling/Animation plus File/Edit/Render/Window/Help. The Status Bar is
1280×31 px at the bottom and visibly carries tool, snap and selection hints. The four named Areas are in the D41
positions. This records the coverage rather than inventing defects for parts that matched the measured spec.

## Evidence files

- `docs/evidence/item167/live-default.png` — full 1280×720 current UI frame.
- `docs/evidence/item167/viewport-header-crop.png`, `toolbar-crop.png`, `sidebar-crop.png`, `outliner-crop.png`,
  `properties-crop.png`, `timeline-crop.png`, `status-crop.png`, `topbar-crop.png` — measured regions of that frame.
- `docs/evidence/item167/properties-render.png`, `properties-output.png`, `properties-material.png`,
  `outliner-selected.png` — separate visible panel states.
- `docs/evidence/item167/item170-header.png` — Chisel's measured Area Header capture.

### Fix routing

UI-01–UI-09 and UI-11–UI-13 are editor presentation/layout work in `src/editor/**` (Chisel is the sole writer under
D41). UI-10 now depends on completing the Properties controls and editable-face persistence; the scene/render
triangle-slot contract is implemented and pixel-tested. Verity was asked for additional independent crop measurements;
none arrived before this inventory was written. The measurements above therefore use the fresh item-167 capture,
Chisel's item-170 header capture, explicit code/widget counts where noted, and no superseded blank-viewport capture.
