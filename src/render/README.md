# `src/render` — owned by `Halide · Render`

**Provides:** `Renderer` (frame lifecycle and passes), `Shader`, `Mesh` upload,
`Material`.

**Depends on:** `core` only.

**Contract:** `ARCHITECTURE.md` §4.3.

## The one thing to get right

**The renderer never walks the scene graph.** It draws the `RenderPacket` it is
handed. That is what keeps `render` from depending on `scene`, and it is what
lets you and `Slate · Scene` work in parallel without touching each other's
files.

If you need more than `RenderPacket` carries, ask Ash to extend it in `core`.

## Order of work

1. M1 — clear the viewport, a grid, world axes, an orbit camera you own
   (the camera type is yours; the view/projection matrices arrive in the packet).
2. M4 — one directional light and a PBR-ish material on top of `MaterialDesc`.

GPU resources are yours to manage: you own upload, lifetime, and the GL objects.
Report `RenderStats` honestly — `Chisel · Editor` displays them and `Verity · QA`
will check them.
