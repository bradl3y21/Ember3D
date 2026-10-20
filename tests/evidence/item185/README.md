# Item 185 — QA pixel test for the D43 icon glyphs

Owner: Verity · QA. Files changed: `tests/**` only (`test_editor_icons.cpp` added,
`CMakeLists.txt` registers it). No `src/editor/**` edits.

## What is under test

`src/editor/Icons.h/.cpp` (Chisel): `enum class IconId {Render..Material}`, the public
`kPropertiesTabOrder` / `kPropertiesTabIcons` / `iconForTab()` mapping, and
`drawIcon(ImDrawList&, IconId, buttonMin, buttonMax, ImU32 color, float uiScale)`.
D43 requires a Verity-owned pixel test proving *"a non-empty bounded glyph at scale 1
and a second DPI scale plus correct active/hover tint"*.

## How it is measured

Headless ImGui (no GL context, so CTest stays context-free). `drawIcon()` is called with
explicit button rects and `uiScale`, and the **real `ImDrawData` is software-rasterised**
(the `theme_probe.cpp` pattern) to answer geometry questions; vertex colours answer tint
questions exactly, with no blending in the way.

### Two methodology traps this test had to work around (both cost a run each)

1. **`ImDrawListFlags_AntiAliasedLinesUseTex` is on in this build.** An AA stroke is a
   *textured* triangle whose uv samples the atlas's AA band, so its uvs differ. The
   `theme_probe.cpp` "all three vertices share a uv" filter therefore silently drops
   **every stroked shape** (it only ever samples filled rects there). First run of this
   test: 55 failures, all from that filter, none from the icons. The rasteriser here
   includes every triangle, which is safe because each sampled frame contains only glyph
   geometry.
2. **ImGui puts its antialiasing gradient in the VERTEX ALPHA of strokes.** So "every vertex
   carries the caller's colour" must be stated as *RGB exact, alpha never raised*, with at
   least one vertex at full alpha — not "alpha == the caller's alpha". (All six ids are
   outline/hatching today, so every non-fringe vertex is the exact `ImU32`; the AA fringe is
   still fainter.)

## Assertions

- **non-empty**: >= 15 painted pixel centres, both scales.
- **bounded**: the exact vertex extent lies inside the caller's button rect (+1px), and no
  painted pixel lies outside the `min(rect, 16*uiScale)` square the implementation derives
  — i.e. a wider button does not stretch the glyph.
- **centred**: painted bbox centre within one 16x16 grid unit of the button centre.
- **scaled, not re-authored**: at `uiScale` 2 the vertex extent doubles (ratio in
  [1.8, 2.2]), the painted area ~quadruples, the vertex count is identical, and the two
  vertex *sets* coincide in normalized grid units (worst nearest-vertex miss <= 0.8, of
  which the fixed 1px AA feather accounts for 0.5).
- **tint**: RGB exact for every vertex; alpha attenuated only; full alpha present. (Chisel's
  refinement made Material outline/hatching only, so all six ids carry the exact `ImU32`;
  the AA fringe is the only reason alpha varies, and the rule is "never raised".)
- **controls** (a test that cannot fail proves nothing): an empty frame is reported empty,
  two tints are distinguishable, and a vertex drawn outside the rect **is** reported.
- **clamp / degenerate**: a 12x12 button at `uiScale` 2 clamps the glyph inside the rect;
  a zero-size rect draws nothing.
- **the real rail** (`drawProperties`, 6 tabs): exactly six icon bands; the **selected**
  category is drawn in `ThemeTokens::active`, the other five in `textPrimary`; hovering a
  row paints `ThemeTokens::hover` over that row and leaves the others unchanged.

## Receipts

```
ctest --test-dir build -C Release -R test_editor_icons
  1/2 Test #3: build.gate ........... Passed
  2/2 Test #20: test_editor_icons ... Passed
  100% tests passed out of 2

bin\test_editor_icons.exe
== 5 test(s), 0 skipped (no --gpu), 194 check(s), 0 failed check(s) in 0 test(s) ==   exit 0
```

Measured, all six glyphs (button 40x40, centre 120,120):

| icon | 1x vtx / painted | 2x vtx / painted | extent ratio | grid-space miss |
| ---- | ---------------- | ---------------- | ------------ | --------------- |
| Render   | 167 / 138 | 167 / 551 | 1.97 | 0.51 |
| Output   |  44 / 124 |  44 / 417 | 1.96 | 0.21 |
| Scene    |  96 / 112 |  96 / 428 | 1.96 | 0.35 |
| World    |  66 / 132 |  66 / 488 | 1.96 | 0.31 |
| Object   |  30 / 148 |  30 / 490 | 1.93 | 0.31 |
| Material |  62 / 104 |  62 / 376 | 1.93 | 0.31 |

Rail: 6 bands for every one of the six selected tabs; the selected row drawn
`RGB(128,191,255)` (`active`), the others `RGB(204,204,204)` (`textPrimary`).
Hover: row background `46,46,46 -> 62,62,62`, and the source-over prediction from the
`hover` token (alpha 0.08) is **62,62,62** — an exact match.

## Scope note (not a defect, but D43 is larger than Icons.h today)

D43's table covers the **Toolbar** too (`Select`, `Move`, `Rotate`, `Scale`). `IconId`
today has only the six Properties categories, and `src/editor/Toolbar.cpp` still draws
text. So D43 is satisfied for Properties and **not yet implementable** for the Toolbar:
the four ids, their glyph paths and a toolbar-side pixel assertion are outstanding. The
test enumerates `kPropertiesTabIcons`, so it will not silently claim Toolbar coverage.
