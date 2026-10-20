# UI Geometry Contract (item 177)

This is the normative geometry contract consumed by editor layout fixes. `ThemeTokens`
and the D42 color/metric defaults remain defined in the root [`ARCHITECTURE.md`](../ARCHITECTURE.md)
§4.5 and `src/core/Theme.h`; this addendum states how those metrics control layout.

## One logical-pixel system

Every `ThemeTokens` metric is a logical pixel at `uiScale == 1.0`. Multiply positions,
sizes, padding, spacing and rounding by the same `uiScale` exactly once. Do not combine
scaled window coordinates with unscaled widget metrics. This follows Blender's
Resolution Scale model in [Interface Preferences](https://docs.blender.org/manual/en/latest/editors/preferences/interface.html).

## Header, widgets and Regions

**An Area Header is exactly `headerHeight × uiScale` high** (27 logical px at scale
1). It starts at the top of the Area; the editor Region begins immediately below it.
The 1px separator, if drawn, is inside that 27px budget—not extra padding after it.
No per-Area title bar, second dock tab strip or implicit ImGui window padding may
increase the reserved height. The Header's controls are laid out within that fixed
rectangle; the Header does not grow to accommodate them. This is the correction for
item 169: fixed `SetNextItemWidth(72/74)` controls clipped “Global” / “Increment”
instead of deriving width from the displayed content.

`widgetHeight` is the standard framed control's **vertical** target (19 logical px:
13px baseline text plus 3px frame padding above and below); it is not the text height
alone and it is never a control-width constant. `regionRounding` is the Area/Region
window corner radius in logical pixels; D42 sets it to 0 so tiled Areas stay square
and rounding cannot create visible gaps. `widgetRounding` is a separate control
metric (3 logical px). `uiScale` is the single logical-to-physical multiplier for
all of them.

## Content-sized controls; no clipping

**Control width is intrinsic, not a magic number.** For a combo/button, measure the
widest visible label/value, then add the control's arrow/glyph and horizontal frame
padding. For a fill control, use the available Region width. Never set an arbitrary
constant width that is smaller than its content. If the sum of viewport Header
controls exceeds the available width, move secondary controls to an overflow popup;
do not clip text, overlap neighbors, or wrap into a taller Header. `widgetHeight`
sets vertical size only.

## Status Bar owns the bottom edge

The Status Bar's height is exactly `theme::statusBarHeight() == 32 * uiScale` logical
pixels. Its top is `WorkPos.y + WorkSize.y - statusHeight`; its bottom is the window's
bottom edge. The DockSpace and every Area end at the Status Bar's top coordinate. No
Area, splitter, Header, child Region or Timeline content may cross into its rectangle.
Reserve this strip once in `Workspace`, not independently in each Area. This is the
correction for item 171, where the Timeline's DockSpace allocation extended below the
reserved Status Bar and was clipped/overlapped.

## Testable invariants

For every supported display scale:

```text
areaHeader.h == 27 * uiScale
status.y + status.h == WorkPos.y + WorkSize.y
dockspace.bottom == status.y
control width >= measured label/value + adornment + frame padding
```

A layout/UI test must exercise the narrowest supported viewport Header and prove
there is no truncation, overlap or Area crossing the Status Bar. These are geometry
contracts, not per-widget styling preferences.
