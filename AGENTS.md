# Ember3D — rules for this session

You are the **only** engineer on this project. There is no team, no Lead, no contract
freeze, and no lane you are forbidden to touch. Every rule below is here because it
already cost this project real money or real work.

Read `ARCHITECTURE.md` (the interfaces) and `WHAT-REMAINS.md` (the current
specification). The team-era documents are kept for reference in `docs/`:
`docs/AGENTS.team.md`, `docs/TEAM.team.md`.

> ## ⚠️ Read `BUILD-STATUS.md` before you claim anything — then distrust it
>
> It records the last **independently built** state, with the command to reproduce it.
> It is a snapshot and it goes stale in minutes. Run the command in §3 yourself and
> paste the output — do not quote `BUILD-STATUS.md` as though it were a measurement
> you made.

> ## ➜ Then read `WHAT-REMAINS.md`
>
> The goal is **Blender fidelity**. The human used the app and said: *"the UI is
> awkward, I don't like the floating menus, moving objects barely works — just copy
> how Blender does it, which is also true for most things including UI."*
>
> Treat that as a **specification, not a vibe.** The short version:
>
> **Layout — no floating windows.** Blender has no floating panels. It has a fixed
> screen of **areas** (editors) separated by draggable splitters, each area carrying
> its own **header** strip, plus a **topbar** of anchored menus and a **status bar**
> pinned to the bottom. The 3D viewport has a **left Toolbar (`T`)** and a **right
> Sidebar (`N`)** *inside* the area. An ImGui window the user can drag anywhere is a
> bug, not a feature.
>
> **Interaction — modal transforms.** `G`/`R`/`S` start a transform that follows the
> mouse with **no button held**. Left-click or Enter confirms; right-click or Esc
> cancels and restores; `X`/`Y`/`Z` constrain to a world axis (press twice for local);
> `Shift`+`X`/`Y`/`Z` constrains to a plane; typing a number sets an exact value;
> `Shift` is precision; `Ctrl` is snapping. The running value shows live in the
> header. **Exactly one undo entry per confirmed gesture.** Middle-drag orbits,
> `Shift`+middle-drag pans, wheel zooms, numpad views jump, `.` frames the selection.
>
> Where this file and your instinct disagree, **Blender wins**. If you are unsure what
> Blender does, say so and ask — do not invent a third way.
>
> **Assume nothing is done because a column says done.** A previous wave closed an item
> without implementing it: `ShadowSettings` and `shadowLightIndex` exist in `Types.h`,
> but `Renderer.cpp` contains **zero** references to shadow. Check the tree, not the
> status.

---

## Start here — the two rules that cost the most

**1. Cost: context is the driver, not output.** Measured on this project: these
sessions re-read **~700k tokens of cached context on every step**, so a long-lived
session pays to re-read its own history on every tool call — measured at **+$3.28 for
a one-word reply** on a frontier model. A **fresh session with a tight brief** costs a
fraction of a ten-hour one. If a session has gone long, start a new one and re-read
`ARCHITECTURE.md` rather than escalating in place.

**2. Declaration order: nothing is used before it exists, and nothing is inferred
that the build system cannot see.** Hit three times in one wave, in three shapes:

- `CollectionId` used before it was declared in `Scene.h` — **loud**: the compiler
  catches it, locally.
- `__has_include` used as a module probe in `App.cpp` — **silent**: CMake cannot see
  it as a dependency, so when `src/editor` appeared the build stayed **green** and the
  editor was silently missing. Fixed by tracked `EMBER_HAS_*` compile flags: a flag
  changes the command line, which forces the rebuild.
- `scaleStrippedRotation` used before its definition, then defined twice — **shared**:
  caught by a linker, not by the author.

So: declare above first use; prefer a **tracked compile flag** over a compile-time
existence probe; and grep the module for the symbol before adding a definition
(`Select-String -Path src/<module> -Pattern '<symbol>'`). The silent one is the only
one that survives a green suite.

## ⚖️ 0. Licence: GPL-3.0-or-later — and what you may take from Blender

**This project is GPL-3.0-or-later. That was a deliberate decision and it changes
what you are allowed to do.**

**Why v3 *and later*, specifically.** Blender itself is `GPL-2.0-or-later`, so we can
use it under v3. Apache-2.0 is compatible with GPLv3 but **not** with GPLv2 — and the
libraries we want (Cycles, OpenSubdiv, Manifold, Embree) are Apache-2.0. Declaring
"or-later" is what lets both coexist.

### You may study and port Blender source

Blender's `bmesh`, `blenlib`, `blenkernel` and `editors/mesh` are all
`GPL-2.0-or-later`, which is compatible with this project's licence. So:

- **Reading Blender source to understand an algorithm is explicitly allowed.**
- **Porting Blender code into this project is allowed**, subject to the rules below.
- **Porting is not the same as pasting.** Blender's mesh code does not arrive alone:
  `bmesh` needs `BLI_*` (math, memory arenas, listbase, KD-tree, BVH) and `DNA`/`RNA`.
  Before you take a function, know what it drags with it. If it drags a subsystem,
  **port the algorithm into our types instead** of importing the subsystem.

### Rules for anything ported from Blender

1. **Keep the original header.** Preserve the copyright line and the
   `SPDX-License-Identifier` exactly as found.
2. **Add an origin note**: the file path in Blender and the commit or release you read
   it from, so the provenance is checkable.
3. **State what you changed.** A port is a modification, and the GPL requires that to
   be visible.
4. **Do not paste a whole subsystem** to get one feature. If you cannot explain what
   the code depends on, you are not ready to port it.
5. **Never copy code between this project and AgentDock.** AgentDock is MIT and
   published. GPL code must not cross that boundary in either direction.

### Prefer a maintained library when one exists

For the hard maths, a permissively-licensed library is usually better than a hand
port — it is tested, documented and faster. Verified for us:

| Need | Use | Licence |
| --- | --- | --- |
| Subdivision surfaces | **OpenSubdiv** (what Blender itself calls) | Apache-2.0-equivalent |
| Mesh booleans | **Manifold** | Apache-2.0 |
| Simplification / optimisation | **meshoptimizer** | MIT |
| Half-edge mesh structures | **geometry-central** | MIT |
| Ray tracing / AO | **Embree** | Apache-2.0 |
| A real renderer, if we ever want one | **Cycles** | Apache-2.0 |

`libigl` is **dual-licensed** (MPL-2.0 and GPL) — use only the MPL-2.0 parts, and say
which you used.

### If in doubt

Record the answer as a decision in `ARCHITECTURE.md` before acting on it. A licence
mistake is the one class of error here that cannot be fixed by a later commit.

---

## 1. You own every file — which makes you the only gate

There are no lanes. You may edit `src/**`, `tests/**`, `CMakeLists.txt`, `cmake/**`,
`vendor/**`, `scripts/**` and `docs/**`. Two consequences:

- **`src/core/Types.h` is the shared vocabulary.** A type used by two modules lives
  there. Keep it small and do not add a second type that duplicates an existing one.
- **You are the only reviewer.** On this project every expensive mistake was caught by
  someone *other than the author*. You do not have that. So you must manufacture it
  mechanically — see §5.

## 2. Pinned conventions — do not improvise

Several of these have already cost a full build.

- **GLAD is version 2.** Include **`<glad/gl.h>`**. `glad1`'s `<glad/glad.h>`
  **does not exist in this tree** — including it breaks the renderer build.
- GLFW must never pull in GL headers: `GLFW_INCLUDE_NONE` is set on the `glfw` target.
  Include `<glad/gl.h>` first, then `<GLFW/glfw3.h>`.
- `gladLoadGL` takes `GLADapiproc`, not `void*`.
- Math: column-major `Mat4` (`Mat4::m[16]`). Do not add a second matrix type.
- One way to do a thing. If Blender has a word for it, use Blender's word.

## 3. The build command — one, shared, every time

```powershell
cmake -B build
cmake --build build --config Release
```

`cmake -B build` exiting 0 means **nothing**. It only configures. The build is green
when `cmake --build` exits 0 **and** `bin/Ember3D.exe` exists. Then run it:

```powershell
bin\Ember3D.exe --frames 60 --exit --demo-object
```

## 4. No progress claim without a command receipt

**A claim you cannot paste output for is not a claim.** This project's single most
expensive failure mode is a report of "compiles clean" / "module green" on code that
did not compile in the root build — everyone downstream builds on it, so the cost
compounds.

When the team ran this, the API refused the claim mechanically. Nobody refuses you now,
so the rule is on you: paste the command, the exit code, and the last ~20 lines of real
output. Say plainly what you did not run.

## 5. Manufacture your own second opinion

The team's rule was *"nothing is done until it is reproduced by someone other than its
author."* You have no such someone. Replace it with three habits:

1. **Re-verify from a clean configure** before calling anything done — delete `build/`,
   configure fresh, build, run. A stale build tree is how the shadow-map item closed
   unimplemented.
2. **Prove every fix by breaking it.** If you fix a bug, add the test and then confirm
   the test **fails without** your change (comment it out or revert it, run, restore).
   A test that passes both ways proves nothing.
3. **Verify against the tree, not the history.** Grep for the symbol you claim to have
   wired up. `Renderer.cpp` containing zero references to shadow is what "done" looked
   like last time.

## 6. Do not write against a moving interface

If the interface you consume is not settled, say so and work on something else. Do not
guess and implement — guessing wrong cost this project **more than half its budget** in
one rewrite. Settle it in `ARCHITECTURE.md` first, then implement.

## 7. Verification is the point

Independent verification is the only part of this project that has been reliably
correct. Write the failing test first, publish the exact reproduction, then fix, then
re-run. **Nothing is done until it is reproduced** — and here, that means by you, from
a clean tree, with the command pasted.
