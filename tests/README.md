# `tests` — owned by `Verity · QA`

**Provides:** `harness.h` and the test suite, registered with CTest through
`tests/CMakeLists.txt` (included by the root `CMakeLists.txt`).

**Depends on:** every module — that is the job.

## The one thing to get right

**You never edit `src/**`.** Not to fix a bug you found, not to make a test pass,
not "just this once". You are the only member whose verdict is worth anything
precisely because you cannot make the code agree with you.

When you find a defect:

1. Write the failing test **first**, and commit it.
2. Publish a finding with the exact reproduction as evidence
   (`kind: "command"`, the command, the exit code, the output).
3. Hand it to the owner with `meta.handoff = { done, left, artifacts }`.
4. Re-run after they claim a fix. Only then mark it `verified`.

## What to test

| Module | What is worth checking |
| ------ | ---------------------- |
| `core` | `Mat4` multiply/inverse, euler → matrix, `Transform::toMatrix` |
| `scene` | primitive generation (non-empty, closed, correct counts), parenting, `.ember` round-trip from `FORMAT.md` |
| `render` | shader compile/link failures are reported, not crashed on; mesh upload index bounds |
| `editor` | undo/redo returns the world to a byte-equal state; gizmo maths |

Start with the **`.ember` round-trip** and `Transform::toMatrix` — they are pure,
they need no GPU, and they will catch real breakage early.

## No GPU required

CTest must pass on a machine with no OpenGL context. Anything needing a context
goes behind an explicit `--gpu` flag and is skipped by default.

---

## Status (Verity · QA) — 2026-10-16

```
ctest --test-dir build -C Release                → 13/13 passed, exit 0  (NO GPU)
ctest --test-dir build-gpu -C Release -R app.smoke_frames
                                                 → Passed 0.49s, exit 0  (real GL ctx)
```

| File | State |
| ---- | ----- |
| `harness.h` | Green. Exit 0 clean / exit 1 with file:line; `--gpu` gating fail-closed. |
| `test_harness_selftest.cpp` | Green, no engine code needed. |
| `test_core_math.cpp` | Green — 17 tests vs real `src/core`. |
| `test_core_math_edges.cpp` | Green — 17 tests / 759 checks. |
| `test_scene.cpp` | Green — 24 tests vs real `src/scene`. |
| `test_scene_io.cpp` | Green — 25 tests vs FORMAT.md v1, incl. all ten §5 failure rules. |
| `test_scene_undo.cpp` | Green — 17 tests / 120 checks. captureSnapshot/reinsert/duplicate + destroy() survivor-consistency. |
| `test_render_camera.cpp` | Green — 23 tests / 141 checks. Differential vs core `lookAt`/`perspective`. |

### Runtime smoke (registered from the ROOT CMakeLists by Kiln)
`app.smoke_frames` = `Ember3D --frames 60 --exit`, gated on `EMBER_ENABLE_GPU_TESTS`
(default OFF). **Proven fail-closed**: passes on a real GL 4.5 context, and
`***Failed` (ctest exit 8) on a deliberately broken shader in a scratch copy.
QA does not double-register it; `tests/CMakeLists.txt` owns the 5 negative
CLI-contract cases (`--help`→0; `--frames 0` / `--exit` / `--bogus` /
`--frames abc` → non-zero), which need no GL and always run.

### Known limit of the smoke test (do not over-trust it)
`drawCalls=1` proves the grid+axes overlay drew over an **empty** scene. It does
**not** prove an ImGui panel is visible — Editor.cpp initialises ImGui but no
assertion ties a panel to pixels. Next honest step: `readViewport()` + a pixel
probe once Chisel's panel content is stable.

### Open defect I found and handed off (verified fixed)
`OrbitCamera::projection()` had `m[15]=1` (`Mat4{}` is identity, not zero) →
`ndcZ(near) = -0.0476` instead of -1. Halide fixed it (`Mat4::zero()` in both
`view()`/`projection()`); re-ran and marked verified.
