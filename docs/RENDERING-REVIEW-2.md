# Rendering architecture review 2

**Scope:** read-only audit of the raster foundation and the planned HDR display
pass / CPU path-tracer additions. No build or tests were run. All source statements
below are **as audited before this run**; concurrent changes and the planned additions
are not verified as landed or passing.

## Capability snapshot

| Direction | Implemented (as audited before this run) | Prototype / planned, not verified | Missing for the stated direction |
|---|---|---|---|
| Real-time raster / Eevee direction | One OpenGL 4.5 `Renderer`; Solid studio diffuse, Material Preview PBR-ish lighting, Rendered packet lights/world ambient, one directional shadow map with 3x3 PCF, mesh GPU cache, D45 per-triangle material slots, transparent sorting/blending, viewport overlays. | Worker scope describes an RGBA16F viewport target, exposure/tone-map final pass, and renderer-owned exposure; none was present in the initial source scan. | HDR scene/display boundary and verified resize path; image-based environment/texture sampling, scalable lighting beyond the fixed eight-light shader loop, temporal AA, richer pass/resource graph, and production-level material/shadow parity. |
| Offline / Cycles direction | `RenderPacket` provides a useful frame input, but its mesh/material/mapping data is frame-valid and non-owning; no offline backend was in the initial source scan. | Worker scope describes a separate CPU `PathTracer` with owned scene snapshot, triangle rays, diffuse bounces, lights/environment, seeded progressive accumulation, and linear output. `PathTracer.*` and its regression test were absent at the audit scan, so this is a contract-level review only. | Acceleration structure, robust production integrator/BSDFs, emission/textures, MIS, adaptive sampling, denoising, cancellation/job scheduling, saved-image path, and editor progress/settings. No Cycles parity. |

## Architecture observations and risks

- **Ownership/lifetimes.** `RenderPacket::items` contains non-owning mesh/material/mapping views; scene documents mesh lifetime only through the next `World` mutation. Raster upload copies mesh data to GPU, but `Renderer` indexes `meshCache_` by raw `MeshData*`; the audited cache uses array sizes as its freshness check, cannot detect same-size in-place edits, and does not visibly prune absent entries. This can retain stale GPU allocations and can reuse a stale entry if an address is recycled. A path tracer must copy every consumed mesh array, transform, material slot/mapping, light, world value, and camera value during `setScene`; it must retain no packet pointers.
- **Accumulation determinism.** The planned contract is deterministic `(pixel, sample, seed)` sampling and progressive batches. Test both repeat renders and batch partition equivalence (e.g. 4 samples once versus 1+3); reset after `setScene`/`reset`; prove packet destruction and source mutation do not affect subsequent samples. Keep per-pixel sample counts and accumulation owned by the tracer. Bit identity across compilers/architectures is stronger than deterministic sampling within one build; document the intended scope, especially if transcendental math differs.
- **Color-space boundary.** `Color`/`MaterialDesc` are documented as straight-alpha linear values, while the audited raster writes shader values directly with no display transform. The byte readback and existing source-over GPU tests are useful for alpha/slot regressions but do not define a linear-light reference or display encoding. With HDR, composite transparent geometry in the linear scene target before tone mapping (tone mapping is nonlinear and does not commute with source-over); define whether overlays are tone-mapped with the scene or drawn after display conversion. CPU path-tracer output should stay linear and should not call the viewport tone mapper. Specify exposure units, tone curve, and output/sRGB encoding once, and test values above 1.0 as well as low values.
- **FBO and GL state.** The audited raster currently draws into the bound framebuffer and scopes viewport/scissor to the viewport, while separately clearing the owning-area rectangle. A new target must not replace that area clear: preserve untouched neighboring UI/area pixels, composite only the viewport rectangle, and account for top-left `Rect` to bottom-left GL conversion. Check target completeness and allocation failure; resize/reallocate only for valid dimensions; delete resources while a valid GL context exists. Explicitly establish and restore framebuffer, draw/read buffers, viewport/scissor, active texture, depth test/function/write mask, blend equation/functions, color mask, polygon mode, and program around the pass. Existing shadow code restores several states to assumed defaults rather than prior values, so a final pass must not inherit or leak that behavior. Ensure later overlay/editor drawing targets the intended framebuffer.
- **Stats and cost.** A fullscreen resolve is an actual draw and should be counted once without changing triangle semantics. RGBA16F costs 8 bytes/pixel (about 16 MiB at 1920x1080 before depth/other attachments); viewport-sized targets and bounded allocation avoid needlessly scaling to the window/4K. The current raster rebuilds per-frame overlay vertex buffers and makes per-triangle draw ranges for material/transparent handling; monitor draw-call growth on slot-heavy meshes. A CPU brute-force tracer scales roughly with pixels × samples × bounces × triangles (plus direct-light/shadow rays): even tiny tests can pass while real scenes stall. Bound/validate dimensions and sample counts and make the initial prototype explicitly small-scene only until acceleration exists.

## Test gaps and next milestone

The audited GPU material test checks slot authority/mapping and source-over behavior, but there is no audited HDR-value, exposure, tone-curve, target-resize/reallocation, final-pass accounting, or linear-vs-display reference test. Existing pixel checks are GPU-gated. The planned CPU tracer needs CPU-only tests for owned-snapshot independence, fixed-seed repeatability and batch equivalence, finite bounded energy, and convergence against a defensible reference; those tests have not been observed or run here.

**Next small testable milestone:** an HDR viewport lifecycle/pixel regression fixture: render a tiny known scene with one channel above 1.0; assert the linear reference is distinct from the exposed/tone-mapped output, verify exposure changes output monotonically, then resize the viewport and assert the target remains complete and no pixel outside the render/owning-area rules changes. Include a transparent layer to pin source-over-before-tone-map and assert the final resolve contributes exactly one draw call. This isolates the new resource/color boundary before adding more raster features.

Concrete follow-up risks: stale pointer-keyed mesh cache entries; framebuffer/state leakage into editor compositing; sRGB versus linear encoding being applied twice or not at all; transparent ordering on intersecting geometry remains approximate; path-tracer direct-light visibility/normal handling can bias energy; floating-point accumulation order can break batch determinism; and brute-force intersection becomes unusable as triangle count grows.

## Unverified assumptions

- The review file is the only file changed by this audit. No builds, tests, or runtime checks were performed.
- The HDR and CPU path-tracer descriptions above are the assigned worker scope, not verified implementation facts. At the source scan, `PathTracer.h/.cpp` and `tests/test_render_path_tracer.cpp` were absent; concurrent edits may have changed the tree after that scan.
- No claim is made that the prior `BUILD-STATUS.md` receipt applies to concurrent work, or that any new target, test, shader, or FBO passes verification.

## Coordinator verification — 2026-10-20 (A/B wipe and parity follow-up)

The source audit above is intentionally preserved as a pre-run review. The latest
tree clean-configured and built to `build-parity-swarm-clean` because the root
Ember3D.exe is running. Direct receipts:

- `cmake --build build-parity-swarm-clean --config Release` — pass, one pre-existing unused-local warning.
- `test_render_compare_pixels --gpu`: **4 tests / 21 checks**; split composition, complete preview mapping, clamps, stale-preview fallback and output scissor.
- `test_render_path_tracer`: **19 tests / 13,241 checks**; UV textures, environment MIS, roulette and true per-pixel adaptive sample scheduling.
- `test_render_offline_job`: **11 tests / 143 checks**; per-pixel scheduling/progress, denoising, cancellation.
- `test_render_path_image_io`: **7 tests / 123 checks**; atomic PFM/PPM, concurrent writers and failure handling.
- `test_scene_topology`: **21 tests / 266 checks**; editable topology and face assignment.
- `test_editor_box_select`: **4 tests / 12 checks**.
- CTest registers **82 targets**. The alternate app smoke, SceneIO v8, BMP loader, TAA fallback, and editor interaction executables were blocked by Application Control.

The viewport now has a distinctive synchronized-camera Raster/Cycles comparison wipe;
the GPU compositor test passes. Per-pixel adaptive sampling and atomic image writers
also pass direct tests. `.ember` v8 editable topology/face-slot round-trip and BMP loader
coverage are compiled but unverified due Application Control. TAA remains deferred;
there is only an asynchronous viewport-preview save, not a full-resolution render-result
workflow. The environment model, denoiser and path tracer remain prototypes, not Blender
or Cycles/Eevee parity. Scene v8 persists editable meshes and image references and reads
v1-v7.
