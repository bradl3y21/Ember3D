# Wave 5 — scoped plan (implementation in progress)

> **Progress update — 2026-10-18 (Ember3D - Copy):** this file is the original
> scope, not a claim that the work is untouched. Tracks B/C have landed partially:
> editable topology and mutations exist; Edit Mode supports screen-space
> vertex/edge/face selection and visible overlays; E/X/M modeling operations are
> reachable from shortcuts, Sidebar Tool, and Edit > Mesh, with snapshot-backed
> undo. Remaining fidelity includes drag-modal extrusion, multi-face region
> extrusion, more Blender delete/merge choices, and the complete behaviour
> checklist. See `docs/ROADMAP.md` and `BUILD-STATUS.md` for current verification.

Written by the operator while wave 4 was still running. **Do not start this until
wave 4's verification item (103) has published its result** — every wave so far has
had at least one item that closed as done without landing, and the only reason we
know is that verification caught it late.

---

## 0. The cost lesson that shapes this plan

Measured, not theorised. Two probes, both asking the model to reply with one word:

| Model | Provider | Probe cost |
| --- | --- | --- |
| `gpt-6-luna` | `opencode-go` | **+$0.095** |
| `claude-opus-5-5` | `opencode` (frontier) | **+$3.28** |
| `kimi-k3` | `opencode-go` | **+$0.99** |

And the same work across a fifteen-minute window: `gpt-6-luna` **+$0.75**,
`kimi-k3` **+$4.76** — 6.4x for comparable output.

**The driver is context, not output.** These sessions re-read **~700k tokens of
cached context on every single step**. A frontier model on a long-lived session pays
frontier prices to re-read its own history on every tool call. So:

> **Rule: use frontier models on FRESH sessions with a tight brief, never by
> escalating a long-lived one.** A fresh session reading `ARCHITECTURE.md` and its
> own lane is a fraction of the cost of one that has been running for ten hours.

This is an operational rule, not a nicety. It is the difference between a frontier
model being usable and being unaffordable.

## 1.5 What the GPL decision changes (added after the licence was accepted)

**Ember3D is now GPL-3.0-or-later** (see `LICENSE` and §0 of `AGENTS.md`). Blender's
`bmesh` / `blenlib` / `blenkernel` / `editors/mesh` are `GPL-2.0-or-later`, which is
compatible — so **Blender source is now a licit source, not a grey area.**

What this changes in the tracks below:

- **Track B (mesh model) is materially cheaper.** Do not invent a topology structure
  from first principles — read `bmesh` first. Its design has had twenty years of hard
  cases baked into it. Port the *design* into our types; do not import the subsystem
  (it drags `BLI_*` and `DNA` with it).
- **Track C (edit mode) is materially cheaper for the same reason.** Blender's
  edit-mode operators (`editors/mesh/`) are the specification for extrude, delete and
  merge, including the cases you would not think of.
- **Track D (modifiers): use OpenSubdiv for subdivision rather than porting subsurf.**
  OpenSubdiv is Apache-2.0-equivalent, it is what Blender itself calls, and it will be
  better tested and faster than a hand port. Mirror and array are trivial — implement
  those natively.
- **The verifier from Track A gets a new job**: check that every ported file carries
  its original copyright line, an `SPDX-License-Identifier` and an origin note. A
  missing header is now a build failure, not a style nit.

Unchanged by the licence:

- **The engineering wall.** Taking one Blender operator still drags its dependencies.
  Port the algorithm when the tail is too long.
- **Cost.** A frontier model re-reading 700k tokens of context is still unaffordable;
  §0 above still stands.
- **AgentDock stays MIT.** GPL code must never cross that boundary. Ember3D stays a
  separate tree.

## Preconditions

1. Item 103 has run and its result is in the record.
2. Item 104's gap list exists — wave 5 should absorb anything 104 says is missing.
3. The build is green **and fresh** (`cmake --build` exit code, not just `ctest` —
   see wave 4's item 102: ctest passed on a red tree because the binary was stale).

**Not in scope for wave 5:** performance work. Verity measured the O(n²) packet
build and correctly left it alone. Do not spend wave 5 on it.

---

## Track A — the trust layer (small, do it first)

Every failure on this project has been the same shape: something was reported done
that was not. Fix the mechanism before building a subsystem on top of it.

| # | Owner | Item |
| --- | --- | --- |
| 105 | Ash | **Decision: the evidence contract per item class.** What artifact must exist for a *render* item (named shader compiled + screenshot), a *scene* item (symbol present + unit test), an *editor* item (interaction test through the real code path), a *build* item (freshness check + exit code). Publish it and freeze. This turns "done" from a claim into a check. |
| 106 | Kiln | **Turn `cmake/CheckFreshness.cmake` into a per-item evidence verifier.** Given an item's declared evidence, check it mechanically — symbol exists in the named file, test exists and passes, output artifact is newer than its sources — and fail loudly. Wire it into ctest. |
| 107 | Verity | **Prove the verifier can fail.** Break something on purpose in a scratch copy and show the verifier catching it. A gate that has never rejected anything is not a gate. |
| 108 | Ash | Fold the result into `ARCHITECTURE.md` and re-freeze. |

## Track B — the mesh data model (the foundation)

| # | Owner | Item |
| --- | --- | --- |
| 109 | Ash | **Decision: the editable mesh representation.** Decide between extending `MeshData` (indexed triangles + adjacency rebuilt on demand) and introducing a proper editable structure. Polygonal faces must be representable; adjacency (vertex→edges→faces) must be cheap; conversion to and from `MeshData` must be lossless for rendering. Decide, justify, freeze. |
| 110 | Slate | **Implement `EditableMesh`**: vertices, edges, polygon faces, adjacency, selection sets (vertex / edge / face), and lossless conversion to `MeshData` for the render packet. Exact, documented, unit-tested. |
| 111 | Slate | **Topology mutation ops at the data level**: move verts, extrude a face, delete vert/edge/face, merge, and triangulate at render time. Each reversible — reuse the existing snapshot/reinsert machinery. Unit-tested. |
| 112 | Verity | **Topology tests**: adjacency correctness, manifold invariants after each op, and `MeshData → EditableMesh → MeshData` round-trip identity. |

## Track C — edit mode in the UI (Blender parity)

| # | Owner | Item |
| --- | --- | --- |
| 113 | Ash | **Decision: mode as state.** Object vs Edit mode, `Tab` to toggle, the mode shown in the viewport header, and the sub-element select modes on `1`/`2`/`3`. Resolve the conflict with the numpad view keys — Blender uses the top-row digits for this and the numpad for views, so the decision must say which. Freeze. |
| 114 | Chisel | **Enter and leave edit mode.** `Tab` toggles; the header shows the mode; the selected object's editable mesh replaces its solid draw with edit overlays (vertex dots, edge lines, face highlight). |
| 115 | Chisel | **Sub-element selection.** Click and box-select verts/edges/faces through the same picking path objects use — do not write a second picker. `1`/`2`/`3` switch the select mode; `A` / `Alt+A`; `Shift` adds. |
| 116 | Chisel | **Move the selection with the modal operator.** `G`/`R`/`S` operate on sub-elements using the *existing* modal state machine — same confirm/cancel, same constraints, same snapping, one undo entry per gesture. This is why Track A and the wave-4 modal work come first. |
| 117 | Chisel | **Extrude (`E`), delete (`X`), merge (`M`)** plus a face/edge context menu, all undoable. |
| 118 | Chisel | **Edit-mode overlays**: show verts/edges/faces toggles, x-ray, and a live vertex/edge/face count in the header. |
| 119 | Verity | **The Blender edit-mode behaviour checklist**, run against the built exe, per item, with raw output. |

## Track D — modifiers (only if Track C is solid)

| # | Owner | Item |
| --- | --- | --- |
| 120 | Ash | **Decision: the modifier stack.** An ordered list per object, evaluated into the packet, non-destructive by construction. Define the evaluation order and where it sits relative to the render packet. Freeze. |
| 121 | Slate | **Modifier stack + evaluation** with the first three: subdivision, mirror, array. Output is `MeshData`, so the renderer needs no knowledge of modifiers. Unit-tested. |
| 122 | Chisel | **The Modifiers panel**: list, add, remove, reorder, per-modifier parameters, all undoable. |
| 123 | Verity | **Tests that modifiers are non-destructive** — the base mesh survives, evaluation is deterministic, and removing a modifier restores the original exactly. |

## Track E — operating the harness

| # | Owner | Item |
| --- | --- | --- |
| 124 | Kiln | **Make a fresh model session easy**: the ability to start a session with a chosen model on a chosen directory from the UI, so "frontier model, fresh context" is a button rather than a workaround. Document it. |
| 125 | Ash | **Add §0 of this file to `WHAT-REMAINS.md` and `AGENTS.md`** so the cost rule reaches every session instead of living in a document nobody re-reads. |

---

## Sequencing, and why

1. **A (trust layer)** — because Tracks B–D are large, and a large subsystem built on
   a lying completion signal is how waves 1–4 lost time.
2. **B (data model)** — nothing in Track C is testable without it.
3. **C (edit mode)** — the headline. This is what makes Ember3D Blender-like rather
   than a scene viewer.
4. **D (modifiers)** — genuinely Blender-defining, and cheap once the mesh is editable.
5. **E** — runs alongside.

## Model allocation

Given §0: wave 5 is *large*, so the cost discipline matters more than wave 4.

- **Frontier, fresh sessions, tight briefs**: the mesh data model (Track B — it is
  exacting and small) and the modal-operator sub-element work (item 116).
- **`opencode-go/gpt-6-luna`**: the default for implementation. Measured at 6.4x
  *cheaper* than `kimi-k3` for comparable work — do not default to the expensive one.
- **Cheap models**: tests, docs, checklist verification.
- **Do not** escalate a long-lived session to a frontier model. Measured: +$3.28 for
  a one-word reply.

## The one thing to decide before seeding

Whether Track D is in wave 5 or wave 6. Tracks A–C are already a milestone on the
scale of the whole of wave 4. My recommendation: **seed A, B and C; hold D** until
edit mode works and has been used, for the same reason mesh editing was deferred
until object editing worked.
