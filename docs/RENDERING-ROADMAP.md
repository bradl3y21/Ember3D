# Rendering Roadmap

This is a staged direction, not a claim that either Blender renderer already
exists here. Keep the editor responsive raster viewport and the offline
integrator as distinct backends with different quality/performance contracts.
For the source-backed Cycles/Eevee architecture and capability comparison, see
[`BLENDER_CYCLES_EEVEE_SOURCE_SURVEY.md`](BLENDER_CYCLES_EEVEE_SOURCE_SURVEY.md).

## Audited starting point

`render::Renderer` is still a single OpenGL 4.5 real-time backend. It draws a
fast studio-diffuse Solid path, a small neutral-light PBR-ish Material Preview,
and a Rendered path with up to eight directional/point lights, flat world
ambient, and one directional shadow map with 3x3 PCF. Viewport geometry now
renders into a viewport-sized RGBA16F target and resolves through fixed
Reinhard + sRGB display conversion with a manual exposure-stops API. It also
owns viewport overlays and GPU mesh caching. `MaterialDesc` contains base color,
metallic, roughness, linear emission, dielectric transmission/IOR and an image
reference. PPM/PFM/BMP UV sampling and a lat-long world image have a first implementation;
the raster environment lighting is an approximation and texture color-space pixel
coverage still needs to pass. Scene render settings, image references, and editable
topology are persisted by `.ember` v8; Render Properties selects Raster or Cycles.
Rendered shading has an Ember-specific Raster/Cycles A/B wipe: same packet camera,
draggable split, and full-raster fallback while a Cycles preview warms up.

The source boundary is already useful: `scene::World::buildPacket()` produces
the frame data, and render consumes `RenderPacket` without walking the scene.
`DrawItem` carries a mesh pointer and material-slot/per-triangle mapping fields.
The initial D45 path is now implemented: scene maps each emitted triangle to its
editable face's material index (defaulting generated triangles to slot 0), and
render groups adjacent opaque triangles by material while sorting transparent
triangles back-to-front by centroid. Missing or malformed mappings and invalid
slot indices fall back to slot 0; the legacy scalar is used only when no slot
array is present. Scene `setMaterial()` synchronizes slot 0 with the deprecated
scalar for compatibility.

The separate CPU `PathTracer` owns a `RenderPacket` snapshot, builds a deterministic
median-split BVH, and supports diffuse/GGX/dielectric transport, textured base color,
emission, importance-sampled lat-long environment, NEE/MIS, Russian roulette, and
progressive linear-RGB samples. `OfflineRenderJob` publishes progressive CPU
snapshots and optional true per-pixel adaptive sampling/spatial denoising; in Rendered shading,
the Cycles engine sends them through the viewport HDR/display transform and exposes
progress/cancel. PFM/PPM writer APIs and an asynchronous viewport-preview save control
exist, but there is no full-resolution final-render workflow. This is a Cycles-style
prototype, not Cycles parity.

## Boundaries to preserve

- **`core`** owns the small shared vocabulary and frozen packet contracts
  (`MeshData`, `MaterialDesc`/slots, resolved lights, world background, camera
  matrices). Add shared data only when both producer and renderer need it, and
  settle contract changes in `ARCHITECTURE.md` first.
- **`scene`** owns editable topology, modifiers, and authored material values.
  It emits frame-valid `RenderPacket` data; it does not own GPU objects or
  backend policy. Renderers must not read `EditableMesh` or traverse `World`.
- **`render`** owns GPU resources and backend implementation. Keep a stable
  facade for the editor-facing frame/viewport API, then isolate an
  `Eevee-like` raster backend (passes, shaders, GPU caches) from a
  `Cycles-like` path-tracing backend (scene snapshot, acceleration, integrator,
  sample state). Both consume the same explicit packet/snapshot contract, not
  each other's private state.
- **`editor` / App** selects viewport shading and presents progress/settings;
  backend-specific controls should not leak into scene data. Offline work must
  be cancellable and must not stall interactive viewport frames.
- **Tests** keep CPU packet/scene invariants separate from GPU pixel checks and
  path-integrator numerical checks. GPU tests should assert rendered pixels,
  not merely shader compilation or counters.

## Stages and first testable milestones

### 1. Make the current raster contract dependable — initial D45 pass landed

Finish and verify D45 consumption end to end: scene emits one slot index per
emitted triangle; renderer uses the object-local slot array, with slot 0 for
missing/invalid mappings and the legacy scalar only when there is no slot array.
Preserve transparent sorting/blending semantics and account for each actual draw
in `RenderStats`.

**Implemented and verified:** CPU tests cover primitive defaults, per-face
assignment, n-gon emission, and Array modifier mapping. GPU pixel coverage shows
slot authority, per-triangle differences, malformed-map/index fallback, and alpha
source-over behavior. This does not establish broader material fidelity or complete
mapping for every possible modifier/topology combination; extend targeted tests as
those operations evolve.

**First tests:** CPU tests for primitive defaults, n-gon triangulation order,
face assignments and fallback cases; a GPU pixel test showing two faces with
different slots render differently, plus a slot-zero authority test. Extend the
existing render material pixel coverage rather than inferring success from
`DrawItem` fields alone.

### 2. Establish an Eevee-like raster foundation — HDR/display first pass landed

Separate pass/resource responsibilities inside `render` without changing
scene/editor ownership: packet upload and cache, depth/opaque/transparent passes,
lighting, shadows, and overlays should have explicit inputs and lifetimes. Add a
defined linear-light working space, HDR color target, and final exposure/tone-map
and display transform before adding more BRDF features. Keep the existing
viewport settings and packet boundary stable while doing so.

**Implemented and verified:** viewport-sized RGBA16F storage, an exposure-stops
setter, fixed Reinhard tone mapping and sRGB output conversion. Pixel tests cover
HDR values above 1, exposure response, target resize/reallocation, alpha blending
in linear scene space, and overlays retaining their display colors. A final
resolve is counted in `RenderStats`. This is a color-management foundation, not
a full render graph or Eevee feature set. Exposure currently has no editor UI.

**First tests:** reference scenes with known linear colors and lighting, checking
pixel values before/after the display transform; regression tests for alpha,
shadow enable/disable, and overlays; a resize/rebuild test for every new target.

### 3. Grow raster lighting and materials incrementally — emission landed

Emission and dielectric transmission/IOR are represented on `MaterialDesc` and
serialized in scene v5 (v1-v3 migrate to black/zero emission; v1-v4 default to
transmission 0 and IOR 1.45). Emission is consumed in Rendered and Material Preview
and editable per material slot. GPU pixel tests cover zero-emission stability,
no-light emission, and per-triangle slot emission. Scene render-engine and Cycles
quality settings are persisted in v6; image references/color-space tags in v7; editable
topology and face material indices in v8.
Material Properties exposes transmission/IOR (Cycles evaluates them; Raster currently
does not) and base-color image references. Initial raster environment-image and
approximate IBL sampling are present, not parity. Next verify texture color space and
expand image-format support; then improve environment-map lighting and image textures
(UV sampling, color-space tags,
sampler/resource lifetime) before claiming material-preview fidelity. Replace
the fixed eight-light loop with a measured scalable strategy (for example,
clustered/tiled light lists) and improve shadow coverage/filtering as separate
features. Keep Solid fast and scene-light-independent. Do not conflate a feature
being represented in scene data with it being consumed by the shader.

**First tests:** a small known HDR environment producing different diffuse and
specular responses; a UV checker/material image pixel test; a scene with more
than eight local lights proving the documented selection/evaluation policy; and
shadow pixel tests for more than one relevant view/scale. Profile representative
scenes before choosing a complexity-driven lighting path.

### 4. Integrate the path-tracing backend into the viewport — first pass landed

Start with a minimal offline `PathTracer` consuming an immutable snapshot made
from the same packet data. Give it its own camera rays, acceleration structure
(evaluate Embree or another compatible maintained library before writing one),
intersection/material evaluation, deterministic per-pixel sampling, and
progressive accumulation. Begin CPU-side if that makes correctness easiest;
choose GPU compute only with an explicit portability/performance case. Keep
sampling state, cancellation, and output buffers out of `RasterRenderer`.

**Implemented subset:** `render::PathTracer` copies packet geometry/material/light/
camera data into owned storage, builds a deterministic median-split BVH, and
progressively accumulates linear-RGB samples. It supports Lambertian/GGX/dielectric
transport, base-color UV images, emission, direct lights and importance-sampled
lat-long environments with MIS/Russian roulette. `OfflineRenderJob` snapshots the
packet, publishes progressive previews, schedules true per-pixel adaptive samples,
and can spatially denoise preview/final images. Render Properties selects
Raster/Cycles; Rendered shading shows the Cycles preview with progress/cancel and the
Raster/Cycles A/B wipe. A viewport-preview save control writes PFM/PPM asynchronously;
it does not start a full-resolution production render. Direct verification passed for
adaptive path batches, the A/B compositor (4 GPU tests / 21 checks), offline jobs,
topology and image writers. The v8 SceneIO, BMP loader, Properties interaction and
texture color-space tests are blocked by Application Control. TAA is intentionally not
implemented until there is a reliable history-invalidation/revision contract. This is
a prototype, not a production path tracer.

**Next tests:** rerun v8 SceneIO and BMP image tests; rerun the Raster sRGB-vs-linear
texture pixel test; add randomized analytic ray/intersection cases and compare BVH hits
against brute force; extend compare/viewport coverage to resize, cache invalidation,
and additional overlay states.

### 5. Continue toward Cycles-like quality and production workflow

Remaining high-value work: strengthen texture/cache error handling and image-format
coverage; add temporal AA/history only after reliable invalidation and expand the
Eevee-style raster pass graph; improve denoising; turn viewport-preview saving into a
full-resolution render-result workflow; and validate BSDF/light sampling against more
references. The raster viewport remains optimized for interaction; the offline tracer
is slower and is not pixel-identical to raster.

## Risks and decision gates

- **Packet lifetime and completeness:** `DrawItem` data is non-owning and valid
  for a frame. A progressive backend cannot retain those pointers; it needs an
  owned snapshot with explicit invalidation/versioning. Modifier-evaluated mesh
  lifetime must be covered too.
- **Feature gaps in shared material data:** `MaterialDesc` now represents a
  base-color image with an explicit color-space tag, but has no normal maps or
  node graph. Emission, transmission and IOR are simple fields, not a node-based
  production material model.
  Define and test each needed contract before promising parity or adding UI-only
  controls.
- **Correctness versus visual plausibility:** the current PBR-ish shader is not
  a reference integrator. Use numerical and pixel tests with tolerances; do not
  use a visually plausible screenshot as proof of energy conservation or
  convergence.
- **Complexity and performance:** render graph/deferred lighting, acceleration,
  accumulation memory, denoising, and asynchronous jobs each add lifecycle and
  synchronization risks. Measure before committing to architecture or quality
  targets.
- **Dependency/licensing/build cost:** before embedding Cycles or Embree, verify
  the exact version, license compatibility with this GPL-3.0-or-later project,
  platform support, API boundary, and reproducible build cost. The project
  architecture records Cycles as a possible future renderer, not as an adopted
  dependency.
- **Scope honesty:** no stage should be called Eevee-like or Cycles-like merely
  because it has a PBR shader, a shadow map, or a ray loop. Name the implemented
  subset and its unsupported features in the UI/docs.
