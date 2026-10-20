# Blender Cycles and Eevee: source survey for Ember3D

**Audience:** Ember3D - Copy renderer and scene work  
**Blender source surveyed:** tag `v5.2.2`, commit `d13f752e`  
**Survey date:** 2026-10-19  
**Scope:** architecture, data flow, principal algorithms, interfaces, and a practical comparison with Ember3D - Copy. This is a source-backed orientation to the major subsystems, not a claim that every Blender source file or every feature has been audited line by line.

## Executive summary

Blender has two very different renderers behind a shared render-engine/result interface:

- **Cycles** is an offline/progressive path tracer. Blender synchronizes the evaluated scene into Cycles-owned scene data; Cycles compiles node graphs for its execution backends, builds an acceleration structure, schedules sample work, traces paths, accumulates results, and sends passes/progress through Blender's render-result API. Its device abstraction spans CPU and optional GPU backends. It is a large renderer subsystem, not a small library that can be dropped into an OpenGL viewport.
- **Eevee** (the 5.2.2 source directory is named `source/blender/draw/engines/eevee/`) is a GPU raster renderer. It synchronizes scene objects/materials into draw resources and executes a coordinated set of depth, G-buffer, lighting, shadow, probe, screen-space ray, transparency, volume, and post-process passes. It trades general path-traced transport for real-time approximations and temporal/reused data.
- **They do not share one compiled material shader.** Cycles builds its own shader graph and CPU/GPU kernel representations; Eevee uses Blender's GPU material/node-tree compilation and engine-specific pass variants. Shared node authoring does not mean identical render semantics.

Ember3D - Copy already follows a useful separation: an OpenGL `Renderer` consumes `RenderPacket`, while a separate CPU `PathTracer` consumes an owned snapshot through `OfflineRenderJob`. Its path tracer is a useful prototype, but not Cycles parity. Its raster backend is a useful Eevee-like starting point, but not Eevee parity. The main lesson is to preserve these independent backends and improve them in small, testable stages rather than attempting to import either Blender renderer wholesale.

## 1. Blender's common integration boundary

### 1.1 Render engine and result API

`source/blender/render/RE_engine.h` defines `RenderEngineType` and the callback surface for an engine: render, view update/draw, bake, render-pass updates, and related lifecycle hooks. It also exposes engine creation/registration and the `RE_engine_begin_result` / `RE_engine_end_result` result lifecycle. This is the public-ish boundary around renderer implementations, not the renderer implementation itself.

Eevee registers an internal draw engine in `source/blender/draw/engines/eevee/eevee_engine.cc` (`DRW_engine_viewport_eevee_type`, id `BLENDER_EEVEE`). Its render callback creates an `Instance`, initializes it from the evaluated dependency graph and camera, and calls `render_frame`. The draw manager supplies the GPU context and pass submission machinery.

Cycles integrates through `intern/cycles/blender/session.cpp` and `intern/cycles/blender/sync.cpp`. `BlenderSession::render` creates/updates a result, synchronizes each view, configures passes, resets/starts/waits for the Cycles session, and writes metadata/results back. `BlenderSync::sync_data` responds to dependency-graph updates and synchronizes scene attributes, view layer, integrator, film, shaders, images, objects, and motion. The `intern/cycles/blender/` directory is the Blender adapter; the renderer core is under `intern/cycles/`.

### 1.2 Node graphs are compiled per renderer

Blender's shader nodes are authored through the node system, but renderers consume compiled representations:

- Cycles uses `intern/cycles/scene/shader_graph.cpp`, `shader_nodes.cpp`, `svm.cpp`, and `osl.cpp`. `ShaderGraph::connect` inserts supported value conversions; closure conversions are restricted. `simplify`, `clean`, and `finalize` expand/default/optimize graph nodes and transform the surface/volume closures before backend compilation.
- Eevee's `source/blender/draw/engines/eevee/eevee_material.cc` builds `GPUMaterial` pass variants for geometry, velocity, deferred/forward, probes, and other material uses. The GPU material/node-tree layer is declared through `source/blender/gpu/GPU_material.hh`; Eevee shaders and material closures live under `source/blender/draw/engines/eevee/shaders/`.

Therefore an Ember3D node/material system should define its own explicit material graph/IR and decide how each backend evaluates it. Reusing node labels or socket names alone does not provide render parity.

## 2. Cycles architecture and data flow

### 2.1 Scene synchronization and device update

The high-level path is:

```text
Blender depsgraph / scene settings
  -> BlenderSync (intern/cycles/blender)
  -> Cycles Scene and managers (intern/cycles/scene)
  -> shader graph simplification/compilation and kernel feature selection
  -> geometry/object/light/background upload and BVH
  -> Session / RenderScheduler / PathTrace
  -> Film, passes, denoising/display/output drivers
  -> Blender RenderResult
```

`Scene::device_update` in `intern/cycles/scene/scene.cpp` makes update order explicit: update shader/OSL state first, compute render passes and kernel features from the shaders, load kernels, then upload shader data and update background, attributes, camera, geometry, objects, lights/integrator/film and related scene state. Changes may require re-running parts of the update; cancellation and device errors are checked between stages.

The scene representation is renderer-owned and richer than Ember3D's current `MaterialDesc`/`RenderPacket`. Cycles tracks geometry, objects/instances, attributes, images, volumes, curves/hair, point clouds, lights, cameras, shader graphs, film passes, and integrator settings in separate managers under `intern/cycles/scene/`.

### 2.2 Acceleration and devices

`intern/cycles/bvh/bvh.cpp`, `bvh2.cpp`, and `geometry_bvh.cpp` define a layout-selected BVH abstraction. `BVH::create` can choose native BVH2 and, when enabled by build configuration, Embree, OptiX, Metal, HIPRT, and multi-device layouts. These are explicit build/runtime capabilities, not assumptions that every binary supports every backend. Device abstraction and queues/memory live in `intern/cycles/device/`; subdirectories contain CPU, CUDA, HIP, HIPRT, Metal, OptiX, oneAPI, multi-device and dummy implementations.

This distinction matters for Ember3D: the current CPU median-split BVH is an appropriate first implementation, while replacing it with Embree should be an explicit dependency/build decision made against measured scene sizes and target platforms. Cycles' device layer is not equivalent to simply dispatching the current OpenGL renderer on a GPU.

### 2.3 Path tracing, sampling, and output

`intern/cycles/session/session.cpp` owns the session/render thread lifecycle. `Session::run_main_render_loop` asks the scheduler for `RenderWork`, handles pause/cancel/device errors, and invokes `PathTrace::render`. `intern/cycles/integrator/path_trace.cpp` performs work over reusable per-device `PathTraceWork` instances in parallel. Its render pipeline includes buffer/work setup, path tracing, adaptive convergence filtering, Cryptomatte post-processing, denoising, display/tile updates, and final buffer output.

The kernel implementation is under `intern/cycles/kernel/`: integrator state/intersection/shading, light and shadow work, film, camera, geometry, and closure evaluation. `intern/cycles/kernel/closure/` contains the concrete BSDF/BSSRDF/volume closure implementations; `intern/cycles/scene/light_tree.cpp` and integrator/light code provide importance selection and direct-light strategies. Cycles is not just a loop that casts camera rays: it manages multiple sampling strategies and their PDFs/weights, light selection, visibility, bounce state, closure mixtures, and output passes.

Sampling is scheduled in work/tile units, supports progressive updates, and may rebalance work across devices. Adaptive sampling, guiding, denoising (OIDN/OptiX paths where enabled), Cryptomatte, render passes/AOVs, and display/output handling are integrated with this lifecycle. The exact feature available depends on build flags, device, scene settings, and pass requests.

### 2.4 Important Cycles source map

| Concern | Start here |
|---|---|
| Blender scene/node sync | `intern/cycles/blender/sync.cpp`, `session.cpp`, `shader.cpp`, `mesh.cpp`, `image.cpp`, `light.cpp` |
| Scene and update ordering | `intern/cycles/scene/scene.cpp`, `shader_graph.cpp`, `shader_nodes.cpp`, `svm.cpp`, `osl.cpp`, `film.cpp`, `integrator.cpp`, `light_tree.cpp` |
| Acceleration | `intern/cycles/bvh/bvh.cpp`, `bvh2.cpp`, `embree.cpp`, `optix.cpp`, `hiprt.cpp`, `geometry_bvh.cpp` |
| Scheduling and progressive trace | `intern/cycles/session/session.cpp`, `integrator/render_scheduler.cpp`, `path_trace.cpp`, `path_trace_work.cpp`, `work_balancer.cpp` |
| Kernel and transport | `intern/cycles/kernel/integrator/`, `kernel/closure/`, `kernel/light/`, `kernel/bvh/`, `kernel/film/` |
| Devices | `intern/cycles/device/device.cpp`, `device/{cpu,cuda,hip,hiprt,metal,optix,oneapi,multi}/` |
| Output/denoise | `intern/cycles/integrator/denoiser*.cpp`, `pass_accessor*.cpp`, `path_trace_display.cpp`, `intern/cycles/blender/output_driver.cpp` |

## 3. Eevee architecture and data flow

### 3.1 Per-render `Instance` and synchronization

`eevee_engine.cc` registers Eevee and establishes the render callback. `eevee_instance.cc` owns an `Instance` for the render/viewport lifecycle. `begin_sync` starts modules for world, materials, velocity, lights, shadows, volumes, pipelines, probes, depth/Hi-Z, ray tracing, motion blur, film, ambient occlusion, lookdev and related state. `object_sync` filters by visibility/type and routes meshes, curves, point clouds, volumes, lights and probes to their sync paths. `end_sync` finalizes dependencies/resources; some shaders are compiled asynchronously, while image renders wait for required shader readiness.

`Instance::render_sample` updates sampling/uniforms and renders the world, probe captures, main view and lookdev view. `render_frame` drives samples and readback. This is a real-time/frame-oriented engine rather than a single offline path-trace job.

### 3.2 Deferred and forward material pipelines

`MaterialModule::material_sync` chooses a forward or deferred pipeline based on the material's surface render method, and selects prepass variants for velocity/motion. Materials may also receive separate variants for volume, reflection/planar probes, capture, and other geometry uses.

`eevee_pipeline.cc` contains separate world, volume, shadow, prepass, forward, deferred and probe pipelines. In a deferred layer, the broad sequence is:

1. Depth/prepass and Hi-Z update.
2. Probe views and shadows.
3. G-buffer material submission (closures/material data are encoded into intermediate textures).
4. Optional screen-space ray-tracing work for applicable closures.
5. Light evaluation, subsurface work, and combine/radiance feedback.

Refraction uses a separate deferred layer. Forward rendering handles materials that need that path and sorted transparency. The G-buffer/closure representation is a key tradeoff: it allows screen-space processing and shared lighting, but has finite closure/storage limits and special cases.

### 3.3 Lighting, shadows, probes, and effects

Eevee's lighting is built from GPU passes and data structures, not the Cycles path integrator. Light culling/evaluation, shadow maps, light probes and world evaluation are managed in dedicated modules (`eevee_light.*`, `eevee_shadow.*`, `eevee_lightprobe*.cc`, `eevee_world.*`). The shadow system uses page/tile allocation and tile-map update/render passes, with directional clip maps/cascades and punctual-light cubeface maps. Probe systems include sphere, planar and volume workflows and associated capture/update logic.

`eevee_raytrace.cc` handles screen-space ray work (including ray generation/tile classification and denoising/fallback stages); this is not general scene path tracing. Other separate modules implement ambient occlusion, subsurface scattering, volumes, depth of field, motion blur, velocity, film, Cryptomatte and sampling/temporal behavior. A feature's source presence does not imply every mode/platform supports it identically; inspect its module's sync, render pass, shader and test paths.

The shader directory (`source/blender/draw/engines/eevee/shaders/`) is the other half of the implementation: G-buffer write/read, deferred evaluation/combine, forward shading, light evaluation, shadow setup/visibility/tracing, screen-ray tracing, probes, volumes, film and closures. The `.bsl.hh` and GLSL includes are compiled/assembled by Blender's GPU shader/material infrastructure.

### 3.4 Important Eevee source map

| Concern | Start here |
|---|---|
| Engine registration and render callback | `source/blender/draw/engines/eevee/eevee_engine.cc`, `eevee_engine.h` |
| Frame lifecycle and object sync | `eevee_instance.cc`, `eevee_instance.hh` |
| Deferred/forward pass sequencing | `eevee_pipeline.cc`, `eevee_pipeline.hh` |
| Material compilation/pass variants | `eevee_material.cc`, `eevee_shader.cc` |
| Light/shadow system | `eevee_light.cc`, `eevee_shadow.cc`, matching `eevee_light_*.bsl.hh` / `eevee_shadow_*.bsl.hh` |
| Screen-space rays | `eevee_raytrace.cc`, `eevee_raytrace_shared.hh`, `eevee_ray_*.bsl.hh` |
| Probes/world | `eevee_lightprobe*.cc`, `eevee_world.cc`, `eevee_surf_capture.bsl.hh` |
| Volume/subsurface/temporal | `eevee_volume.cc`, `eevee_subsurface.cc`, `eevee_sampling.cc`, `eevee_motion_blur.cc`, `eevee_depth_of_field.cc` |
| Shader/material interface | `source/blender/gpu/GPU_material.hh`, `source/blender/nodes/shader/`, Eevee `shaders/` |

## 4. Cycles vs Eevee

| Dimension | Cycles | Eevee |
|---|---|---|
| Primary goal | Offline/high-quality physically based rendering; also progressive viewport use | Responsive real-time raster viewport and render engine |
| Core method | Monte Carlo path tracing with path state, sampled BSDFs/lights, accumulation | Raster/deferred/forward passes plus reusable screen/probe data and temporal effects |
| Scene update | Sync evaluated Blender scene into Cycles scene/managers; upload/compile per device | Sync visible objects/materials/lights into draw-manager resources and pass lists |
| Acceleration | BVH and optional backend-specific acceleration (Embree/OptiX/etc when built) | Depth/Hi-Z, tile/light culling, shadow page maps, probes and screen-space structures |
| Material execution | Cycles shader graph compiled to Cycles execution forms (SVM/OSL and device kernels) | Blender GPU material compiled to engine/pass-specific GPU shader variants |
| Quality/time | More samples generally reduce noise; denoising/adaptive scheduling can help | Approximate/temporal and bounded by real-time resource budgets; may have screen-space/probe limits |
| Failure modes | Noise, convergence, biased/incorrect PDFs, memory/device limits, long jobs | Temporal artifacts, missing off-screen data, G-buffer/closure limits, probe/shadow update cost |

An Eevee shader looking plausible is not evidence of path-integrator energy correctness; a converged Cycles image does not imply the Eevee pass graph is cheap enough for interactive use. Tests and quality targets need to name the backend.

## 5. What Ember3D - Copy has now

This section is grounded in the current `Ember3D - Copy/docs/RENDERING-ROADMAP.md`, `ARCHITECTURE.md` §4.3, and source paths—not the original `Ember3D` directory.

### Raster / Eevee-like direction

- `src/render/Renderer.cpp` is one OpenGL 4.5 renderer with Wireframe, fast Solid, neutral Material Preview, and Rendered raster modes. The Rendered raster shader supports the current packet lights and a directional shadow map; material lighting is PBR-ish, not Eevee's G-buffer/closure system.
- The viewport target is RGBA16F with fixed Reinhard + sRGB display conversion and manual exposure stops. Overlays are display-referred and compensated so they retain intended colors.
- The packet currently supports a bounded light list (up to eight in the raster material path), world ambient, material slots/per-triangle assignment, and shadow metadata. No image-texture sampling/HDRI environment lighting, scalable clustered-light system, temporal AA, or Eevee-like pass graph is documented as complete.
- `.ember` v6 stores the scene render-engine choice and Cycles quality settings. Material Properties exposes emission, transmission and IOR; the UI documents that the current Raster path ignores transmission/IOR while Cycles evaluates them. The raster/path material models are still not semantically equivalent.
- The stable design boundary is `scene::World::buildPacket()` -> `RenderPacket` -> `render::Renderer`; renderer must not walk the scene graph.

### Path-tracing / Cycles-like direction

- `src/render/PathTracer.cpp` copies packet data into tracer-owned structures, unprojects camera rays, builds a deterministic median-split BVH, traces triangle geometry, samples lights/materials, accumulates linear RGB and applies power-heuristic MIS where both strategies contribute.
- `src/render/PathBsdf.cpp` evaluates/samples diffuse and GGX-style reflection plus dielectric reflection/refraction using Fresnel and PDFs. `src/render/OfflineRenderJob.cpp` deep-copies packet meshes/material slots/triangle mappings, renders bounded sample batches on a worker thread, publishes progressive images, and supports cancellation/state reporting.
- `Renderer::setPathTraceImage` borrows a CPU preview through the next frame; Rendered shading can present the current Cycles-style image through the HDR display pass. This is an Ember-owned prototype, not Blender Cycles linked into the app.
- Render Properties selects Raster or Cycles. With Cycles selected, Rendered viewport shading starts/restarts an asynchronous CPU preview after packet changes settle, displays samples and offers cancellation. This is a viewport-preview workflow, not Blender's full render-result/pass/device workflow.
- The roadmap explicitly says standalone adaptive-sampling/denoising utilities are not yet integrated with the path tracer/job; image textures, HDRI importance sampling, more production BSDF coverage, saved final-image output, and renderer exposure controls remain.

## 6. Recommended Ember3D path

These are recommendations from the source comparison, not assertions that the work is already implemented.

1. **Keep the split backend boundary.** Preserve the editor-facing `Renderer::renderFrame(RenderPacket)` facade. Keep an Eevee-like raster implementation and Cycles-like CPU/offline path tracer as separate backend state machines. Avoid leaking GPU resources, BVHs, or integrator state into `scene` or the editor.
2. **Treat the owned snapshot as a hard invariant.** `RenderPacket` contains frame-valid pointers; the offline job must copy every field it consumes before starting asynchronous work. Extend invalidation and tests whenever packets gain textures, transforms, material data, or evaluated geometry.
3. **Define material semantics before expanding UI.** `MaterialDesc` is a compact contract, not a node system. Add texture/environment/node data only through a settled scene/core/serialization contract; prove that raster and path backends interpret color spaces, normals, alpha, emission, transmission, IOR and metallic/roughness consistently. Bump `.ember` version/migrate old files as needed.
4. **Improve the CPU tracer with measurable milestones.** Add randomized ray/BVH checks against brute force; analytic BSDF/PDF and energy tests; deterministic batch-partition tests; NEE/MIS reference cases; convergence tests; and tests for cancellation/scene mutation. Then evaluate Embree against current BVH on representative scenes before choosing an API/dependency.
5. **Improve raster quality in isolated steps.** First add UV/image textures and color-space/sampler lifetime, then HDR environment lighting/IBL, then scalable light selection, then temporal sampling/denoising or screen-space reflections. Each step needs a tiny reference scene and GPU pixel tests for resize, alpha ordering, viewport rect preservation, tone mapping, and resource reallocation.
6. **Keep interactive and offline controls distinct.** Raster should remain responsive and may show approximate results; path tracing should expose samples, bounces, seed, progress and cancellation. Do not let offline work block viewport frames or reuse raster accumulation state.
7. **Be honest about parity.** Use “Cycles-style CPU path tracer” and “Eevee-like raster backend” until the documented feature and test matrix demonstrates more. A shadow map, PBR shader, or ray loop alone is not renderer parity.

### Suggested proof gates

- CPU: brute-force vs BVH intersections; finite nonnegative radiance; BSDF sample/eval PDF consistency; energy/convergence references; fixed-seed repeated renders and batch-equivalence; snapshot independence after scene mutation; cancellation and reset.
- GPU: known linear/HDR values through the display transform; exposure monotonicity; texture color-space/UV checker; light counts past the current cap; shadows under more than one view/scale; per-material/triangle pixel differences; resize and render-area isolation; temporal-history invalidation.
- Workflow: a saved scene with a deterministic preview render; a separate final-image path with progress/cancel; tests compare pixels or image statistics, not only that shaders compile.

## 7. Licence and porting caution

Ember3D - Copy declares GPL-3.0-or-later. The surveyed Eevee engine source headers are GPL-2.0-or-later, which is license-compatible with GPLv3-or-later, subject to the project's attribution rules. `intern/cycles/CMakeLists.txt` is SPDX Apache-2.0, but that must **not** be generalized into “every file/dependency in Cycles is Apache”: inspect the exact source file and its dependency/license chain before copying or linking anything. Blender's overall repository contains multiple licenses and external dependencies.

The most useful first step is architectural learning, not copying: Blender's `BLI_*`, `DNA`/`RNA`, draw-manager, GPU, and device systems create a large dependency tail. Re-express algorithms against Ember3D's types or adopt a maintained library only after an explicit scope/build/license decision. This report contains no copied Blender implementation code.

## 8. Survey method and limits

The Blender checkout was pinned to `v5.2.2` / `d13f752e`. A focused three-track GPT-6 Luna source review covered Cycles internals, Eevee internals, and Blender integration/Ember3D mapping; the main findings were cross-checked against source files and Ember3D - Copy's current architecture/roadmap. A preceding broader four-lane run exceeded the worker time limit and produced no report; it is not counted as evidence. No source or project build was run for this survey, and no implementation files were changed.

The survey covers major architectural paths and the relevant source map. It is not an exhaustive review of every shader, closure, backend, render pass, UI setting, platform build flag, or third-party license. Follow the listed modules and tests for any concrete feature being considered.
