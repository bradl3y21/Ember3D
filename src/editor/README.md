# `src/editor` — owned by `Chisel · Editor`

**Provides:** the ImGui shell (`Editor`), the viewport, the outliner, the
properties panel, gizmos, and the undo stack.

**Depends on:** `core`, `render`, `scene`.

**Contract:** `ARCHITECTURE.md` §4.5.

## The one thing to get right

You are a **client** of the other modules. You call their public APIs and you
display their state. You do not reach inside them, and you do not "just fix"
a line in `scene` or `render` — you ask the owner.

Mutating the world goes through `World`'s API. If you need a mutation that does
not exist (e.g. `setParent` with a cycle check, a rename, a duplicate),
message `Slate · Scene` and they add it.

## Order of work

1. M3 — panel host, outliner, properties, selection.
2. M5 — translate/rotate/scale gizmos and the undo/redo stack.

The theme should read like Blender's: dark, compact, high-contrast, keyboard-first
(**G** move, **R** rotate, **S** scale, **X** delete). Gizmos operate on the
selection through `World`; the undo stack records *commands*, not raw state
snapshots, so it stays cheap.
