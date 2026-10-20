# `src/scene` — owned by `Slate · Scene`

**Provides:** `World` (objects, hierarchy, selection), primitive generation,
`.ember` save/load, and `World::buildPacket()`.

**Depends on:** `core` only.

**Contract:** `ARCHITECTURE.md` §4.4.

## The one thing to get right

You **produce** the `RenderPacket`; `Halide · Render` consumes it. The packet
type lives in `core` and belongs to Ash — if it does not carry what you need to
express, ask Ash rather than reaching into `render`.

Scene data is yours: object storage, parenting, transforms, materials, the
selection set. `Chisel · Editor` mutates the world through your public API only
(`setTransform`, `setMaterial`, `select`, …). If she needs a mutation that does
not exist, she asks you and **you** add it — never the other way round.

## Order of work

1. M2 — primitives (cube/sphere/plane/cylinder/torus), transforms, parenting,
   `buildPacket`.
2. M6 — the `.ember` format. Document it in `FORMAT.md` **before** implementing
   it, and keep the two in sync; `Verity · QA` will write the round-trip test
   from your document, not from your code.
