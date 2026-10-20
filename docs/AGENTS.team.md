# Ember3D — rules for every session

Read `TEAM.md` (ownership) and `ARCHITECTURE.md` (interfaces). These are the hard
rules that stop wasted work. They are here because they have already burned us.

> ## ⚠️ Read `BUILD-STATUS.md` before you claim anything
>
> It records the last **independently built** state, with the command to
> reproduce it. As of 04:10 the build is **green** and all five suites pass.
>
> It is a snapshot and it goes stale in minutes. If you are about to claim a
> build or a module is done, run the command in §3 yourself and paste the output
> — do not quote `BUILD-STATUS.md` as though it were a measurement you made.

> ## ➜ Then read `WHAT-REMAINS.md`
>
> Wave 3 is **Blender fidelity**. The human used the app and said: *"the UI is awkward,
> I don't like the floating menus, moving objects barely works — just copy how Blender
> does it, which is also true for most things including UI."*
>
> Treat that as a **specification, not a vibe.** The short version:
>
> **Layout — no floating windows.** Blender has no floating panels. It has a fixed
> screen of **areas** (editors) separated by draggable splitters, each area carrying its
> own **header** strip, plus a **topbar** of anchored menus and a **status bar** pinned
> to the bottom. The 3D viewport has a **left Toolbar (`T`)** and a **right Sidebar
> (`N`)** *inside* the area. Recreate that. An ImGui window the user can drag anywhere
> is a bug, not a feature.
>
> **Interaction — modal transforms.** `G`/`R`/`S` start a transform that follows the
> mouse with **no button held**. Left-click or Enter confirms; right-click or Esc
> cancels and restores; `X`/`Y`/`Z` constrain to a world axis (press twice for local);
> `Shift`+`X`/`Y`/`Z` constrains to a plane; typing a number sets an exact value;
> `Shift` is precision; `Ctrl` is snapping. The running value is shown live in the
> header. **Exactly one undo entry per confirmed gesture.** Middle-drag orbits,
> `Shift`+middle-drag pans, wheel zooms, numpad views jump, `.` frames the selection.
>
> **Assume nothing is done because a column says done.** Last wave, one item closed
> without being implemented — the shadow map: `ShadowSettings` and `shadowLightIndex`
> exist in `Types.h`, but `Renderer.cpp` contains **zero** references to shadow. Check
> the tree, not the status.

---

## Start here — the two rules that cost us the most

**1. Cost: a frontier model on a FRESH session, never by escalating a long-lived one.**
Measured, not theorised — one-word probes: `gpt-6-luna` **+$0.095**, `kimi-k3` **+$0.99**,
`claude-opus-5-5` (frontier) **+$3.28**. The same work over a fifteen-minute window:
`gpt-6-luna` **+$0.75** vs `kimi-k3` **+$4.76** — **6.4x** for comparable output. The driver is
**context, not output**: these sessions re-read **~700k tokens of cached context on every
step**, so a frontier model on a long-lived session pays frontier prices to re-read its own
history on every tool call — measured at **+$3.28 for a one-word reply**. A fresh session that
reads `ARCHITECTURE.md` and its own lane costs a fraction of a ten-hour one.

**2. Declaration order: nothing is used before it exists, and nothing is inferred that the
build system cannot see.** Hit three times in one wave, in three shapes:

- `CollectionId` used before it was declared in `Scene.h` — **loud**: the compiler catches it,
  locally.
- `__has_include` used as a module probe in `App.cpp` — **silent**: CMake cannot see it as a
  dependency, so when `src/editor` appeared the build stayed **green** and the editor was
  silently missing. Fixed by tracked `EMBER_HAS_*` compile flags: a flag changes the command
  line, which forces the rebuild.
- `scaleStrippedRotation` used before its definition, then defined twice — **shared**: a peer's
  linker caught it, not the author's.

So: declare above first use; prefer a **tracked compile flag** over a compile-time existence
probe; and grep the module for the symbol before adding a definition
(`Select-String -Path src/<module> -Pattern '<symbol>'`). The silent one is the only one that
survives a green suite — which is exactly why `build.freshness` exists.

## ⚖️ 0. Licence: GPL-3.0-or-later — and what you may now take from Blender

**This project is GPL-3.0-or-later. That was a deliberate decision and it changes
what you are allowed to do.**

**Why v3 *and later*, specifically.** Blender itself is `GPL-2.0-or-later`, so we can
use it under v3. Apache-2.0 is compatible with GPLv3 but **not** with GPLv2 — and the
libraries we want (Cycles, OpenSubdiv, Manifold, Embree) are Apache-2.0. Declaring
"or-later" is what lets both coexist.

### You may now study and port Blender source

Blender's `bmesh`, `blenlib`, `blenkernel` and `editors/mesh` are all
`GPL-2.0-or-later`, which is compatible with this project's licence. So:

- **Reading Blender source to understand an algorithm is now explicitly allowed** —
  previously it was a grey area, now it is a licence-compatible source.
- **Porting Blender code into this project is allowed**, subject to the rules below.
- **Porting is not the same as pasting.** Blender's mesh code does not arrive alone:
  `bmesh` needs `BLI_*` (math, memory arenas, listbase, KD-tree, BVH) and `DNA`/`RNA`.
  Before you take a function, know what it drags with it. If it drags a subsystem,
  **port the algorithm into our types instead** of importing the subsystem.

### Rules for anything ported from Blender

1. **Keep the original header.** Preserve the copyright line and the
   `SPDX-License-Identifier` exactly as found.
2. **Add an origin note**: the file path in Blender and the commit or release you
   read it from, so the provenance is checkable.
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

Ask the Lead and record the answer as a decision. A licence mistake is the one class
of error on this project that cannot be fixed by a later commit.

---

## 1. The launch order — writers start last

This is enforced by the server, not a convention.

1. **The Lead is launched first.** They publish the interface decisions as a
   finding with `"claim": "decision"` — the shapes every other module codes against.
2. **The Lead freezes the contracts:**
   `POST /agent-api/v1/contracts/freeze { "projectId": "…", "note": "…" }`.
   Only a Lead may do it, and only once a decision finding exists. Freezing an
   empty contract is refused.
3. **Only now are writers launchable.** Before the freeze, the server refuses to
   launch a member whose role class is `writer`.
4. **The verifier may start at any time.** Writing tests is not blocked by a
   moving interface, and having them early is the point.

Ask `GET /agent-api/v1/contracts?projectId=…` to see your role and whether you
may start writing. If a writer is refused, the answer is *"ask the lead"*, not
*"find a way around it"*.

A late interface change is the most expensive mistake made on this project so
far — it cost one team **more than half its budget** in rework. If you are the
Lead and an interface genuinely has to move, **say so loudly and re-freeze**;
do not let it drift silently.

---

## 2. You may only edit files you own

| You | Own |
| --- | --- |
| Ash · Lead | `src/core/**`, `docs/**`, `ARCHITECTURE.md`, `TEAM.md` |
| Kiln · Platform | `src/platform/**`, `CMakeLists.txt`, `cmake/**`, `vendor/**`, `scripts/**` |
| Halide · Render | `src/render/**` |
| Slate · Scene | `src/scene/**` |
| Chisel · Editor | `src/editor/**` |
| Verity · QA | `tests/**` — **only** tests |

Need a change outside your paths? **Message the owner. Do not edit it.**

## 3. Pinned conventions — do not improvise

Several of these have already cost the team a full build.

- **GLAD is version 2.** Include **`<glad/gl.h>`**. `glad1`'s `<glad/glad.h>`
  **does not exist in this tree** — including it breaks the renderer build.
- GLFW must never pull in GL headers: `GLFW_INCLUDE_NONE` is set on the `glfw`
  target. Include `<glad/gl.h>` first, then `<GLFW/glfw3.h>`.
- `gladLoadGL` takes `GLADapiproc`, not `void*`.
- Math: column-major `Mat4` (`Mat4::m[16]`). Do not add a second matrix type.
- `core` holds the shared vocabulary. A type used by two modules lives in
  `src/core/Types.h` and is edited **only by Ash**.

## 4. The build command — one, shared, every time

```powershell
cmake -B build
cmake --build build --config Release
```

`cmake -B build` exiting 0 means **nothing**. It only configures. The build is
green when `cmake --build` exits 0 **and** `bin/Ember3D.exe` exists.

## 5. No progress claim without a command receipt

This is enforced by the API. A finding that claims a build or a module landed
**must** carry evidence at creation:

```
POST /agent-api/v1/findings/{id}/evidence
  { "kind": "command", "command": "cmake --build build --config Release",
    "exitCode": 0, "output": "<the last ~20 lines>" }
```

or pass `"claim": "build" | "module"` and an `evidence` object in the
`POST /findings` body, which is stored in one call.

Two agents have now reported "compiles clean" / "module green" on code that did
not compile in the root build. That is the single most expensive failure mode
here, because everyone downstream builds on it. **A claim you cannot paste output
for is not a claim.**

## 6. Do not write against an unfrozen contract

If the interface you consume is still moving, say so and work on something else
in your own paths — do not guess and implement. Guessing wrong cost this project
**more than half its budget** in one rewrite.

If you believe a contract is wrong, raise it **before** implementing against it.

## 7. Milestones are gated

**M0 first.** Nothing else is verifiable until the root build is green and
`bin/Ember3D.exe` opens a window. While M0 is open: write headers and
implementation against `ARCHITECTURE.md` §4, but do not claim a module is finished.

## 8. Verification is the point

Independent verification is the only part of this that has been reliably correct.
Write the failing test first, publish the exact reproduction, hand it to the
owner, and re-run after they claim a fix. **Nothing is done until it is reproduced
by someone other than its author.**
