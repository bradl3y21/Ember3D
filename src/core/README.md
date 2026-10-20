# `src/core` — owned by `Ash · Lead`

**Provides:** the shared vocabulary (`Types.h`, `Math.h`), logging, and the
composition root (`App.h`, `main.cpp`).

**This is the only module that may define a type used by two other modules.**
If you need a new field on `DrawItem`, `RenderPacket`, `Transform`, `MaterialDesc`
or `MeshData`, ask Ash — do not add it yourself.

**Depends on:** nothing of ours (that is the point).

**Contract:** `ARCHITECTURE.md` §4.1 and §4.6.

`main.cpp` stays thin: parse the command line, construct `App`, `init`, `run`,
`shutdown`. All wiring lives in `App.cpp` — the single place the modules meet.
