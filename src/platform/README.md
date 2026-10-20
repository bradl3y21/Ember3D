# `src/platform` — owned by `Kiln · Platform`

**Provides:** `Window`, `Input`, GL function loading, monotonic time.

**Also owns:** `CMakeLists.txt` (root), `cmake/`, `vendor/`, `scripts/`.

**Depends on:** `core` only.

**Contract:** `ARCHITECTURE.md` §4.2.

This module is the **gate for M0**. Nothing else can be verified until
`cmake --build build --config Release` produces `bin/Ember3D.exe` and a window
opens and closes cleanly. Do this first, announce it, then help unblock others.

`vendor/` holds GLFW, GLAD and Dear ImGui as committed source — not fetched at
configure time, so builds work offline. Exactly one copy of each. If you find a
duplicate vendor directory, delete it and say so.
