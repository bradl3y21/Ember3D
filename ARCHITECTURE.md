# Ember3D — architecture and ownership

A Blender-like 3D editor, built from scratch. C++20, CMake, OpenGL 4.5 core.

> **Read `TEAM.md` first.** This file defines the module boundaries and the
> interfaces. Those boundaries are what stop the team from colliding; the
> interfaces are what let the modules compile separately.

---

## 1. The one rule

**You may only edit files inside the paths you own.** Everything else is
read-only to you, no matter how obvious the fix looks.

If you need a change outside your paths — a new field on a shared struct, a new
CMake flag, a new core type — you **do not make it**. You ask the owner over the
AgentDock wire (`agentdock_send_message` / `<slug>_send_message` with
`kind: "question"`), and they make it. Cross-boundary edits are the single
biggest cause of lost work here.

The reason is mechanical, not procedural: on this project every module is
compiled by its own `CMakeLists.txt` and every shared type has exactly one
owner. An edit from outside breaks an interface somebody else is mid-way
through implementing.

### Claim before you edit

Before you start editing, announce it so peers can see:

```
POST /agent-api/v1/claims   { "path": "src/render/Renderer.cpp", "note": "adding the depth pass" }
```

`GET /peers` returns everyone's active claims. If a peer already claims the file
you want, message them — do not both edit it. Release when you are done
(`POST /claims/release {"all": true}`).

---

## 2. Module map

```
Ember3D/
├── CMakeLists.txt              [Build]     root: toolchain, flags, vendor, subdirs
├── cmake/                      [Build]     helper modules (incl. RunSmokeSizeCheck.cmake)
├── vendor/                     [Build]     GLFW, GLAD **2**, Dear ImGui — committed, one copy each
├── scripts/                    [Build]     build/run/clean helpers
├── docs/                       [Lead]      ROADMAP.md (status + decisions), evidence/ (pixel proofs)
├── ARCHITECTURE.md             [Lead]      this file
├── TEAM.md                     [Lead]      roles, ownership, protocol
├── BUILD-STATUS.md             [Operator]  last independently measured build state
├── WHAT-REMAINS.md             [Operator]  the operator's gap analysis
├── src/
│   ├── core/                   [Lead]      shared vocabulary + the app shell
│   │   ├── CMakeLists.txt      ember_core + the Ember3D exe + the EMBER_HAS_* defines
│   │   ├── Math.h              Vec2/3/4, Mat4, Quat, Color, Ray  (header-only)
│   │   ├── Types.h             ObjectId, Handle<T>, Transform, MeshData, MaterialDesc,
│   │   │                       DrawItem, RenderPacket, RenderStats, Light/LightInstance,
│   │   │                       ViewportSettings, GizmoState
│   │   ├── Log.h               EMBER_LOG_* macros, EMBER_ASSERT
│   │   ├── App.h / App.cpp     the composition root (module presence = EMBER_HAS_* defines)
│   │   └── main.cpp            thin entry: --width/--height/--title/--log/--frames/--exit/
│   │                           --scene/--demo-object/--help
│   ├── platform/               [Platform & Build]
│   │   ├── Window.h / .cpp     window, events, GL context, size + framebuffer callbacks,
│   │   │                       F11 fullscreen, logical window size
│   │   ├── Input.h / .cpp      keyboard, mouse, monotonic time, viewport rect (+ staleness)
│   │   └── GLLoader.h / .cpp   GL function loading (gladLoadGL)
│   ├── render/                 [Renderer]
│   │   ├── Renderer.h / .cpp   frame lifecycle, passes, grid + axes overlay, RenderStats,
│   │   │                       mesh cache
│   │   ├── Camera.h / .cpp     orbit camera: standard views, orthographic, pan, zoom-to-cursor,
│   │   │                       frameBounds, screenToRay
│   │   ├── Shader.h / .cpp     program compile/link, uniform cache
│   │   ├── Mesh.h / .cpp       VAO/VBO upload from core::MeshData
│   │   └── Material.h / .cpp   material + light uniform mapping
│   ├── scene/                  [Scene & Data]
│   │   ├── Scene.h / .cpp      World, Object, hierarchy, selection, pick, bounds,
│   │   │                       undo snapshots, light objects
│   │   ├── Primitives.cpp      cube/sphere/plane/cylinder/torus generators
│   │   ├── SceneIO.cpp         save + load `.ember` (writes v2; v1 loads and migrates)
│   │   └── FORMAT.md           the scene file format, written down (v2 + v1 migration)
│   └── editor/                 [Editor]
│       ├── Editor.h / .cpp     ImGui context + backends, dockspace, the whole ImGui frame
│       ├── Panels.h            shared panel state/declarations
│       ├── Menu.cpp            menu bar: File / Edit / Add / Object / Viewport
│       ├── Toolbar.cpp         tool strip (Move/Rotate/Scale + mode)
│       ├── Sidebar.cpp         right-hand panels host
│       ├── Viewport.cpp        viewport region, picking (screenToRay -> World::pick)
│       ├── Outliner.cpp        scene hierarchy panel
│       ├── Properties.cpp      transform + material panel
│       ├── Gizmo.h / .cpp      GizmoController: hit-test, drag -> one undo command
│       ├── Commands.h / .cpp   undoable commands
│       └── Undo.h / .cpp       command stack (mergeWith coalesces drags)
├── tests/                      [QA]
│   ├── CMakeLists.txt          registers the suites with CTest, GPU-gated cases included
│   ├── harness.h               tiny assert-based runner (no external framework)
│   └── test_*.cpp              core math, math edges, scene, scene IO, scene undo,
│                               render camera, platform input, platform window, harness
└── bin/                        build output (git-ignored)
```

**The file list above is what is in the tree, checked 2026-10-16** — not the plan.
When a file lands or a contract is implemented, the gap between this map and the
tree is the first place to look (that is how the missing `Editor.h`, the duplicated
`ViewportSettings`, and the never-called `wantsViewportFocus()` were each found).

---

## 3. Pinned stack

Do not substitute these. Six models choosing six libraries is how this project
dies.

| Concern | Choice | Where |
| ------- | ------ | ----- |
| **Licence** | **GPL-3.0-or-later** — deliberately v3, so Blender's `GPL-2.0-or-later` is a licit source and the Apache-2.0 libraries we want (Cycles/OpenSubdiv/Manifold/Embree) can coexist. Vendored code must be licence-compatible. **See `AGENTS.md` §0 before copying anything from Blender**, and `WAVE-5.md` §1.5 | `LICENSE` |

### Ports, vendoring and attribution (D36, frozen)

**Read before deciding, not after.** Blender's real geometry (`main` branch, read 2026-10-17):
`source/blender/bmesh/bmesh_class.hh` end to end, plus the file inventories of
`bmesh/operators/` and `editors/mesh/`. Notes here are from that reading, and the
parts I did *not* read are named as residue.

**What Blender's model actually is** (so the difference from D34 is stated, not assumed):

| Blender | Ours (D34) |
| ------- | ---------- |
| elements start with `BMHeader` (type byte, flag byte, `index` with `-1` = invalid, `CustomData*`) | no header element; flags are fields on the record |
| `BMVert{co, no, e}` — **stores the normal** and one incident edge | `positions[]`, normal **derived** |
| `BMEdge{v1, v2, l, v1_disk_link, v2_disk_link}` — pointer cycles (disk) per end | `Edge{v0, v1}` + CSR `edgeFaces` |
| `BMLoop{v, e, f, radial_next/prev, next/prev}` — **ring** cycle for the face, **radial** cycle for the edge | `Loop{vertex, edge, face, next, prev, uv}` — indices, no cycles |
| `BMFace{l_first, len, no, mat_nr}` — stores normal, `short mat_nr` | `Face{firstLoop, loopCount, materialIndex}` |
| `BMesh` holds `totvertsel`/`totedgesel`/`totfacesel`, per-type `*_dirty` bits, `vtable/etable/ftable` index tables, `selectmode`, `act_face` | `vertexFaces`/`edgeFaces` CSR, one `topologyVersion` |
| `USE_BMESH_HOLES` is **commented out** — "disable holes for now … they use more memory and can't be saved in DNA currently" | D34's "one simple ring per face, no holes" |

So: **the DESIGN is ported; the representation is ours.** That matters for the
licence — a re-expression against new types is our code, while copying Blender's
expressions is not. Two honest deviations from Blender, both deliberate: we
**derive** normals where Blender caches them, and we use **one** `topologyVersion`
where Blender keeps per-element-type dirty bits (coarser; fine at our sizes).

**Port targets, by item, named from the real tree:**

| Item | Read this (design), then write it in our terms |
| ---- | ---------------------------------------------- |
| 110 `EditableMesh` | `bmesh_class.hh` — done: D34 is a transcription of that model into indices |
| 111 topology ops | `bmesh/operators/bmo_extrude.cc`, `bmo_dissolve.cc` (delete/dissolve), `bmo_removedoubles.cc` (merge), `bmo_dupe.cc` (duplicate), `bmo_split_edges.cc` |
| 110's triangulation | `bmesh/operators/bmo_triangulate.cc` (+ `bmo_beautify.cc`) — **read before writing `toMeshData()`**, because D34 requires correct concave handling. Port the algorithm shape; do **not** drag `BLI_polyfill`/`BLI_scanfill` with it |
| Track C (114–118) | `editors/mesh/editmesh_extrude.cc`, `editmesh_select.cc`, `editmesh_utils.cc` — **specification source, not port source**: that code is Blender's C/ImGui-less architecture, so none of it is pasteable into our ImGui editor |
| Track D (120–123) | `bmo_subdivide.cc` exists — and is still **not** the thing to port: use OpenSubdiv (below) |

**Vendored libraries — vendor when the item is seeded, never speculatively.** Each
addition is build surface in `vendor/**`, which is Kiln's path, so every one is a
request to him rather than an edit by anyone else:

| Need | Library | Licence | When |
| ---- | ------- | ------- | ---- |
| Subdivision surfaces (Track D) | **OpenSubdiv** | Apache-2.0-equivalent | **first**, when item 120/121 seeds — do not port subsurf |
| Mesh booleans | **Manifold** | Apache-2.0 | when a boolean op is scoped (not in wave 5's list) |
| Simplification / optimisation | **meshoptimizer** | MIT | when performance work is scoped — WAVE-5 says **not** in wave 5 |
| Ray tracing / AO | **Embree** | Apache-2.0 | only if ray-traced AO is scoped |
| A full renderer | **Cycles** | Apache-2.0 | not now; noted so the licence choice stays valid |
| Half-edge structures | ~~geometry-central~~ | MIT | **rejected — D34 already decided our structure**; vendoring it would duplicate the mesh model |
| `libigl` | — | **dual MPL-2.0 / GPL** | MPL-2.0 parts only, and say which ones are used |

**The attribution rule (item 106's verifier enforces it).** Blender's own header, read
from `bmesh_class.hh`, is the template:

```c
/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
```

Any file containing Blender **expression** must carry, at the top:

1. the original `SPDX-FileCopyrightText` line(s), **verbatim**;
2. the original `SPDX-License-Identifier` **verbatim** (`GPL-2.0-or-later`);
3. an origin note naming the Blender path **and** the release/tag/commit read —
   `// Origin: blender/blender source/blender/bmesh/operators/bmo_extrude.cc @ <sha>`;
4. a statement of what was changed for us;
5. our own `SPDX-FileCopyrightText: <year> Ember3D Authors` added **alongside**, never
   replacing.

**Mechanically checkable**: every declared port matches
`SPDX-License-Identifier: GPL-2.0-or-later` + `Origin: blender/blender <path> @` +
`Modifications:` within the first N lines, and a port is **discoverable by grep**
(`Origin: blender/blender`). **Residue the verifier cannot close: it can check that a
declared port carries its header, but it cannot detect an *undeclared* one** — that
needs a human or a source-comparison tool, and pretending otherwise would be the
"a gate that cannot fail" mistake in a new costume.

**AgentDock boundary, in writing:** AgentDock is **MIT** and published. **No code
crosses that boundary in either direction** — Ember3D (GPL) code must never be copied
into AgentDock, and AgentDock code must never be copied into Ember3D. The checkable
half is a grep for cross-references; the rest is a rule people follow.

**What the licence does NOT solve** — said plainly, because "GPL" reads like a
permission slip:

1. **The engineering wall, verified in the source rather than repeated:** `bmesh_class.hh`
   includes `BLI_assert.hh`, `BLI_sys_types.hh`, `DNA_customdata_types.h`,
   `DNA_listBase.h`, and `BMesh` holds `BLI_mempool*`, `CustomData` and `ListBaseT`.
   Taking one function still drags BLI and DNA, which is why the rule is *port the
   algorithm into our types*.
2. **Cost.** Context, not output, is the driver (WAVE-5 §0: ~700k tokens re-read per
   step; a one-word reply measured at +$3.28 on a frontier model). A licence change
   does not make frontier sessions cheap.
3. **Reading is not porting, and porting is not pasting.** Our loop walk is a
   different expression in a different representation; a file only needs the header
   above if Blender's *expression* is in it.
| Language | C++20 | everywhere |
| Build | CMake ≥ 3.25, MSVC, Release default | `CMakeLists.txt` |
| Windowing | GLFW | `vendor/glfw` |
| GL loader | **GLAD 2** — include `<glad/gl.h>`, never `<glad/glad.h>` (GLAD 1) | `vendor/glad` |
| UI | Dear ImGui (docking branch) | `vendor/imgui` |
| Math | our own, Blender `BLI_math`-style | `src/core/Math.h` |
| Tests | our own `harness.h`, wired into CTest | `tests/` |
| Output | `bin/Ember3D.exe` | root CMake |

---

## 4. Interfaces

These are contracts, not suggestions. `core` owns the shared vocabulary so that
no module has to include another module's header.

### 4.1 `core` owns the shared types — `src/core/Types.h`

```cpp
namespace ember {

using u8 = std::uint8_t; using u32 = std::uint32_t; /* … */

struct ObjectId { u32 value = 0; bool valid() const { return value != 0; } };

struct Vec3 { float x, y, z; };        // defined in Math.h
struct Mat4 { float m[16]; };          // COLUMN-major: m[col*4 + row]
struct Color { float r, g, b, a; };    // straight (non-premultiplied) linear RGBA
struct Ray  { Vec3 origin, direction; };// Math.h; `direction` normalized (D16 picking)
struct Rect { int x, y, w, h; };       // D29: framebuffer pixels, top-left origin

struct Transform {
  Vec3 position{0,0,0};
  Vec3 rotation{0,0,0};   // euler RADIANS, XYZ order
  Vec3 scale{1,1,1};
  Mat4 toMatrix() const;  // T * R * S
};

struct MeshData {           // what the scene produces, what the renderer uploads
  std::vector<float> positions;   // xyz
  std::vector<float> normals;     // xyz
  std::vector<float> uvs;         // uv
  std::vector<u32>   indices;
  std::string        name;
};

enum class ImageColorSpace : u8 { SRgb, Linear };
struct ImageTextureRef { std::string path; ImageColorSpace colorSpace = ImageColorSpace::SRgb; };

// FINAL for M4 (decision D12). Invariants: baseColor is straight
// (non-premultiplied) LINEAR RGBA and `a` is the only transparency channel;
// metallic and roughness are nominally [0,1], and the renderer clamps roughness
// to >= 0.045 to keep specular energy finite. Emission is authored linear RGB
// plus a non-negative strength; values above 1 are valid HDR radiance.
// Transmission is a [0,1] dielectric weight; IOR is finite and bounded to
// [1,3], with malformed values falling back to 1.45. Base-color image refs are
// present; normal maps and a node graph are not.
struct MaterialDesc {
  Color baseColor{0.8f,0.8f,0.8f,1.0f};
  float metallic  = 0.0f;
  float roughness = 0.5f;
  Color emissionColor{0.0f,0.0f,0.0f,1.0f};
  float emissionStrength = 0.0f;
  float transmission = 0.0f;
  float ior = 1.45f;
  ImageTextureRef baseColorTexture;
};

struct DirectionalLight {   // M4; direction = the way the rays travel
  Vec3  direction{0,-1,0};
  Color color{1,1,1,1};
  float intensity = 1.0f;
};

// D19: lights are scene objects, so the authored component carries no position
// or direction — the object's Transform supplies them (Blender's rule).
// `DirectionalLight` (pre-D19) was deleted in D25; there is one light-instance
// type now.
enum class LightType : u8 { Directional, Point };
struct Light         { LightType type; Color color; float intensity; float radius; };
struct LightInstance { LightType type; Vec3 position; Vec3 direction;
                       Color color; float intensity; float radius; };

// D47 (camera items 33/34/209): authored camera projection on a scene Object.
// The object's Transform is the camera pose; local -Z is forward and local +Y
// is up. The presence of a `camera` record in .ember sets isCamera=true.
struct CameraSettings { bool isCamera = false; float fovY = 0.78539816339f;
                        float clipNear = 0.1f, clipFar = 1000.0f;
                        bool orthographic = false; float orthoScale = 10.0f; };

// D19: viewport presentation state, and the gizmo's per-frame description.
enum class ShadingMode : u8 { Wireframe, Solid, Material, Rendered };
struct ViewportSettings { ShadingMode shading; PivotMode pivot; ShadowSettings shadows;
                           bool compareRasterCycles; float compareSplit;
                          int gridSubdivisions;   // D24: minor lines per major cell (clamp 1..100)
                          bool showGrid, showAxes, showGizmo, showOrigins, showStats,
                          showSelectionOutline; };
struct GizmoState { enum class Mode : u8 { Translate, Rotate, Scale };
                    Mode mode; Vec3 origin; float handleLengthPixels;
                    Mat4 orientation;   // D20: identity = Global, object world
                                        // rotation = Local; BOTH sides use it
                    int hoveredAxis, activeAxis; bool snap; };

// --- wave 2 (D22): types are implemented; status and limits are in the §4.3 table
struct MaterialSlot    { std::string name; MaterialDesc material; };   // per-object slots
struct WorldBackground { Color color; float strength; bool lighting;
                         ImageTextureRef environmentTexture; }; // scene world
struct ShadowSettings  { bool enabled; int resolution; float bias, normalBias, maxDistance; };
enum class PivotMode : u8 { MedianPoint, Cursor3D, IndividualOrigins,
                            BoundingBoxCenter, ActiveElement };
// DrawItem (D22): const MaterialSlot* materials; u32 materialCount;
//                 bool castShadow, receiveShadow;
// RenderPacket (D22): WorldBackground background; int shadowLightIndex;
// D49: std::string assetBaseDirectory; image refs resolve against it.
// D35: mode as state; core because render consumes them for edit overlays (item 118)
enum class InteractionMode : u8 { Object, Edit };
enum class SelectMode      : u8 { Vertex, Edge, Face };

// D37 (item 118): what render draws edit overlays from. The EDITOR fills it, from
// the editable topology; render draws points/lines/triangles and never learns what
// an EditableMesh is. Full contract in §4.3.
struct EditOverlayState {
  const MeshData* mesh = nullptr;   // non-owning (D9 lifetime rule)
  Mat4            model;
  std::vector<Vec3> vertexPoints;   // object-local
  std::vector<Vec3> edgeLines;      // 2 per edge
  std::vector<Vec3> faceTriangles;  // 3 per triangle
  std::vector<u32>  faceOfTri;      // one per triangle: which FACE it belongs to
  std::vector<u8>   vertexSelected, edgeSelected, faceSelected;  // parallel, 1 = selected
  InteractionMode   mode       = InteractionMode::Object;
  SelectMode        selectMode = SelectMode::Vertex;
  int activeVertex = -1, activeEdge = -1, activeFace = -1;
  float vertexRadiusPixels = 5.0f, edgeWidthPixels = 2.0f;  // drawn AND hit-tested
  Color selectedColor{1.0f,0.6f,0.15f,1.0f};   // Blender's edit selection is orange
  Color unselectedColor{0.05f,0.05f,0.05f,1.0f};
  Color activeColor{1.0f,1.0f,1.0f,1.0f};
  bool valid() const { return mesh != nullptr && mode == InteractionMode::Edit; }
};

// D40 (item 120): the modifier stack's vocabulary — an ordered, per-object,
// NON-DESTRUCTIVE list that SCENE evaluates into a MeshData. render never sees
// these types. The variant index IS the kind (pinned by static_asserts), and
// modifierDesc() names each kind's parameters for the editor AND for save/load.
struct SubdivisionParams { int levels; bool smoothNormals; };
struct MirrorParams      { bool axisX, axisY, axisZ; float mergeThreshold; };
struct ArrayParams       { int count; float offsetX, offsetY, offsetZ; bool relative; };
using ModifierParams = std::variant<SubdivisionParams, MirrorParams, ArrayParams>;
enum class ModifierKind : u8 { Subdivision, Mirror, Array };
struct Modifier { ModifierParams params; bool enabled = true;
                  ModifierKind kind() const; };
enum class ParamType : u8 { Float, Int, Bool };
struct ParamDesc   { const char* key; const char* label; ParamType type;
                     float minimum, maximum, step; };
struct ModifierDesc { const char* name; ModifierKind kind;
                      const ParamDesc* params; u8 paramCount; };
ModifierDesc modifierDesc(ModifierKind);
// On Object (scene, §4.4): std::vector<Modifier> modifiers;
//                          u32 modifierVersion, meshVersion;

struct DrawItem {           // one thing to draw this frame
  ObjectId     object;
  Mat4         model;
  MeshData*    mesh;        // non-owning
  MaterialDesc material;
  bool         selected = false;
  bool         wireframe = false;
};

struct RenderPacket {       // scene fills this, renderer consumes it
  std::vector<DrawItem> items;
  Vec3  cameraPosition{0,0,0};
  Mat4  view, projection;
  Color clearColor{0.12f,0.12f,0.13f,1.0f};
  std::vector<LightInstance> lights;   // D19: every light, world-space resolved.
                                       // ORDER is contract: creation-order slots,
                                       // including unmeshed/hidden lights.
  WorldBackground background;          // D22
  int shadowLightIndex = -1;           // D22
  int   width = 0, height = 0;
};

struct RenderStats { int drawCalls = 0; int triangles = 0; double frameMs = 0.0; };

} // namespace ember
```

**`core` is the only place a type used by two modules may be defined.** If
Render needs a new field on `DrawItem`, Render asks the Lead.

### Material slots and per-face assignment (D45, item 188)

**Decision: a material slot is an ordered, object-local value, not a Scene-level
material datablock.** `Object::materials` owns the ordered `MaterialSlot` array;
each slot contains its display name and a by-value `MaterialDesc`. Slot identity is
the zero-based `u32` index within that object's array—not a global or stable ID.
Different objects have independent slots, even when their values match. Every mesh
object has at least slot 0; it is the base/default material and cannot be removed.
The current legacy `Object::material` / `DrawItem::material` path continues to mean
slot 0 until that compatibility field is deleted in a separately staged change.

**Assignment lives on editable faces.** `EditableMesh::Face::materialIndex` is the
zero-based index into the owning object's slot array. New/procedural faces default to
0. Triangulating an n-gon copies its face index to every emitted triangle. Slot array
order is user-visible; removing slot `k > 0` remaps faces assigned to `k` to slot 0
and decrements every face index greater than `k`, atomically with the removal. Slot
0 is never removed. No shared-material linking, slot reordering, or stable slot IDs
are part of this contract.

**Packet/render boundary:** `DrawItem` carries the object slot view and an object-
specific per-triangle mapping, all non-owning and valid for the frame (D9):

```cpp
const MaterialSlot* materials = nullptr;
u32 materialCount = 0;
const u32* triangleMaterialSlots = nullptr;
u32 triangleMaterialSlotCount = 0;
```

`triangleMaterialSlotCount` equals `mesh->indexCount() / 3`; entry `i` describes the
triangle formed by `mesh->indices[3*i..3*i+2]`. The scene builds the mapping in the
same order as emitted triangles, from `Face::materialIndex`; modifiers preserve each
output triangle's source assignment, and wholly generated triangles default to slot
0. Primitive meshes map every triangle to 0. The renderer reads only this packet
mapping and slot array—it does not include or infer `EditableMesh` topology. If the
slot count is zero, the legacy `DrawItem::material` applies to all triangles. If a
mapping is absent, has the wrong length, or names an index outside `materialCount`,
that triangle uses slot 0 (and when there is no slot array, the legacy material).
When `materialCount > 0`, `materials[0]` is authoritative as well as every later
slot; the deprecated scalar is not a second source of truth.

**Properties / Material contract:** for a selected mesh object, show its ordered
slot list with one active row; show Base Color, Metallic, and Roughness for the
active slot. `+` appends a slot named `Material N`, initialized to `MaterialDesc{}`;
it does not silently assign any faces. `−` removes the active slot and is disabled
for slot 0; it applies the remap rule above. In Edit Mode with Face selection, `Assign`
sets all selected faces to the active slot index. Outside that mode or with no face
selection, Assign is disabled with a short explanation (never a dead control). The
active row is editor UI state and resets/clamps to a valid slot when the active object
changes or a slot is removed. Non-mesh selections do not show editable face controls.

Slots already round-trip in `.ember`; face assignment is authored mesh state and
must round-trip with the editable mesh before it is claimed persistent. The current
`.ember` implementation does **not** serialize `EditableMesh`/face indices, so that
save/load work is an explicit remaining scene-format task, not a behavior this
decision claims landed. Blender reference: [Material Assignment](https://docs.blender.org/manual/en/latest/render/materials/assignment.html)
describes per-object slots and assigning the active slot to selected faces in Edit
Mode; Ember3D's index and packet rules above are its own frozen contract.

**Status:** decision only. `DrawItem` does not yet carry the mapping and render does
not yet consume per-face materials. Item 144 (or its successor) owns that render
path; Slate owns scene mapping/removal/persistence; Chisel owns the Properties UI;
Verity owns independent tests. The interface decision does not widen file ownership.

**Pinned, so tests can assert them** (implemented in `src/core/Math.h` /
`src/core/Types.h`, verified on MSVC `/W4`):

- `Mat4` is **column-major with column vectors**: element `(row, col)` is `m[col*4 + row]`.
  A translation lives in `m[12]`, `m[13]`, `m[14]`.
- `A * B` means **apply B first, then A**; `M * v` is the matrix–vector product.
- `Transform::toMatrix()` is **`T * R * S`** — scale first, then rotation, then
  translation. Non-uniform scale therefore *does* shear once rotated. That is deliberate.
- Euler angles are **radians, XYZ order**: `rotationEuler(v) = Rz * Ry * Rx`
  (X applied first, Z last). Right-handed, Y-up.
- `Quat::fromEuler` agrees with `rotationEuler` for the same triple; `toMat4(q)`
  agrees with it too.
- `inverse()` of a singular matrix returns the identity rather than NaNs. **Not a
  contract to rely on** — callers must not depend on it.
- `perspective()` yields OpenGL NDC: near plane at `z = -1`, far plane at `z = +1`.

### 4.2 `platform` — `src/platform/Window.h`

```cpp
namespace ember::platform {

struct WindowConfig { int width = 1280; int height = 720; std::string title = "Ember3D"; };

enum class Key { W, A, S, D, Q, E, G, R, S_, Num0, Num1, /* … */
                 KP_0, KP_1, /* … */ KP_9, KP_Decimal, Delete, Escape };
enum class MouseButton { Left, Right, Middle };

class Window {
public:
  bool  create(const WindowConfig&);
  void  destroy();
  bool  shouldClose() const;
  void  pollEvents();
  void  swapBuffers();
  int   width()  const;
  int   height() const;
  void* nativeHandle() const;
};

class Input {
public:
  bool keyDown(Key) const;
  bool keyPressed(Key) const;          // edge, this frame
  bool mouseDown(MouseButton) const;
  Vec2 mousePosition() const;
  Vec2 mouseDelta() const;
  Vec2 scrollDelta() const;
  bool cursorOverViewport() const;
};

bool  loadGLFunctions();               // must be called with a current context
double timeSeconds();

} // namespace ember::platform
```

**Coordinate space (D16, revised by D17 — Kiln's argument, accepted).** Platform
publishes ONE space: **window coordinates (logical points), origin top-left, y
increasing downward**. That is the space `glfwGetCursorPos` and ImGui's GLFW
backend already use, so input and GUI hit-tests (`WantCaptureMouse`, panel
rectangles) agree with no conversion at all — which is the operation that happens
most often. `Input::mousePosition`, `Input::mouseDelta` and
`Input::setViewportRect` are all in it, and the viewport rectangle is **resynced
whenever the window size changes**: a rectangle captured once at creation is stale
after a resize or a fullscreen toggle, and that staleness is a real defect this
fixes. `Window::width()/height()` stay **framebuffer pixels**, because that is a
rendering size (viewport, scissor, projection), not an input coordinate; the two
diverge under OS DPI scaling, and anyone who needs the ratio has it (ImGui exposes
it as `io.DisplayFramebufferScale`). §4.3 has the one place a conversion is
required, and it is at the call site.

### 4.3 `render` — `src/render/Renderer.h`

```cpp
namespace ember::render {

class Renderer {
public:
  bool init();
  void shutdown();
  void resize(int width, int height);
  void renderFrame(const RenderPacket&);      // the whole draw
  void readViewport(int x, int y, int w, int h, std::vector<u8>& outRGBA); // for the UI
  RenderStats lastStats() const;
};

} // namespace ember::render
```

The renderer **never** walks the scene graph itself — it draws the
`RenderPacket` it is handed. That is what keeps it from depending on `scene`.

**The orbit camera is `render`'s** (`render::OrbitCamera`, M1) and is *not* a
`core` type: `core` carries only the matrices in the packet. M1's row in
`TEAM.md` used to say otherwise; it was wrong. See §4.4 for how the packet gets
its matrices.

**Picking, the producer half (D16).** The camera turns a screen point into a
world-space ray — it owns view/projection, so nobody else can do this correctly:

```cpp
Ray OrbitCamera::screenToRay(int x, int y, int width, int height) const;
```

`x, y` are **viewport-local framebuffer pixels, y measured down from the top** —
the space the projection and `glViewport` are built in, which is **NOT** the
window-coordinate space of §4.2. The editor converts ImGui window-point input with
`io.DisplayFramebufferScale`, then subtracts the viewport rect's framebuffer origin
before calling `screenToRay`. `width, height` are the viewport framebuffer dimensions
— the same numbers the packet and `projection(aspect)` use, not the full window size.
The returned `Ray` lives in core, has a normalized
`direction`, and world-space `origin` at the camera eye. The camera does **no**
hit-testing: that is scene's half, in §4.4.

### Gizmo drawing and viewport settings (D19)

Two more things belong to `render`, for the same reason the camera does — it owns
the geometry and the GL state:

```cpp
void Renderer::drawGizmo(const GizmoState&, const Mat4& view, const Mat4& projection);
void Renderer::setViewportSettings(const ViewportSettings&);
const ViewportSettings& Renderer::viewportSettings() const;
```

**The 3D area rectangle (D29).** With Blender-style areas the 3D viewport no longer
owns the window, so the renderer must be told which rectangle to draw into:

```cpp
void Renderer::setViewportRect(const Rect& renderRect,
                               const Rect& owningArea = {}); // framebuffer px, top-left
```

`renderFrame` clears only the owning docked Area, then scopes the 3D pass
(viewport + scissor) to `renderRect`; this prevents stale pixels around the smaller
viewport body without touching neighboring areas. ImGui composites over it.
**The packet's `width`/`height` are the viewport region's size, not
the window's**, because `projection(aspect)` is built from them — feeding the window
size would stretch the scene inside a non-square area. `App` fills them from the
editor's area rect (see §4.5), so there is exactly one source for "how big is the 3D
view".

**Render backends (initial implementation, 2026-10-18):** `Renderer` keeps its
editor-facing raster API and renders the viewport to a viewport-sized RGBA16F
target, then performs a fixed Reinhard + sRGB display resolve with manual exposure
stops. The owning Area clear stays on the output framebuffer, and display-referred
overlays compensate the view transform so selection/gizmo colors remain legible.
The final fullscreen resolve is an actual counted draw. There is no exposure UI or
automatic adaptation yet.

The separate CPU `PathTracer` consumes a `RenderPacket` synchronously and copies
geometry, material, light, world, image and camera inputs into owned storage. It
builds a deterministic median-split BVH; its progressive API returns linear RGB
with seeded batches and true per-pixel adaptive sample counts. It supports UV
base-color textures, importance-sampled lat-long environment, Lambertian/GGX/
dielectric transport, emission, NEE/MIS and Russian roulette. `OfflineRenderJob`
publishes progressive copies, supports cancellation, adaptive scheduling and
optional spatial denoising. Rendered shading can present the Cycles preview or a
viewport-only Raster/Cycles A/B wipe with a draggable split. PFM/PPM writers and an
asynchronous viewport-preview save control exist, but not a full-resolution production
render workflow. Image formats, BSDFs, denoising and environment lighting remain
limited prototypes; this is not Cycles/Eevee parity.

- **`drawGizmo` draws; it does not interpret clicks.** The editor calls it from
  inside `draw()` (it holds the `Renderer&`) with the `GizmoState` it built, so
  App and §4.5 are untouched. The renderer must draw handles
  `GizmoState::handleLengthPixels` long, because that is the number the editor
  hit-tests against — one value, two consumers, no drift. `hoveredAxis` and
  `activeAxis` are the editor's to set; the renderer only shows them.
- **Space / orientation (D20).** Handles are drawn and hit-tested in
  `GizmoState::orientation`: **identity = Global**, the selected object's world
  **rotation** with translation and scale stripped = **Local** (Blender's rule —
  local axes rotate with the object). It is passed as a matrix, not as a
  `Global/Local` enum, because `render` may not read the scene and handed only an
  enum it could not compute a local axis; the editor keeps its own enum for the
  UI. Both the drawn handle and the clickable handle come from this one matrix.
- **Pivot (D20).** `origin` is the median of the selection in world space; with a
  single object selected it is that object's world position. The editor gets the
  median from `World::selectionBounds()` (the bounds query requested for framing),
  so the gizmo sits where the camera frames — one definition of "where the
  selection is", not two.
- **Gizmo handle hit-testing lives in the editor**, in screen space, against the
  same `handleLengthPixels` and the same `orientation`. It is interaction, not
  rendering, and it needs the panel/rect knowledge only the editor has.
- **Drag → transform (D20), the whole contract.** Grab (left button on a handle):
  capture the object's `Transform` and push **one** `Command`; set `activeAxis`.
  Per frame: compute the delta — translate = ray/plane intersection projected onto
  the axis, rotate = the angle swept about the axis, scale = the ratio along it —
  and call `World::setTransform`; merge the frame's command with the previous one
  via `Command::mergeWith` so the whole drag is **one undo step**. Release: stop
  merging. Ctrl snaps (1 unit / 15°). The renderer is never told about the drag
  beyond `activeAxis`, so it cannot disagree with the scene about what moved.
- **`ViewportSettings` is the editor's UI state**, written during `draw()` and
  read by the renderer on the *next* frame (immediate-mode UI norm, documented
  rather than surprising). Shading mode and the overlay toggles all live here;
  nobody adds a second place to switch the grid off.

**Implementation status, checked 2026-10-18 against the tree** (a contract exists
before the code does, on purpose — this is the list of what is still frozen-only):

| Contract | State |
| -------- | ----- |
| `Renderer::drawGizmo` | **implemented** — handle geometry along `GizmoState::orientation`, constant screen size, modes for translate/rotate/scale, gated by `showGizmo`; the editor's drag application works too and `test_editor_gizmo` is **green** |
| `Renderer::setViewportSettings` / `viewportSettings()` | **implemented** — the Viewport menu's shading/overlay toggles reach the renderer (Editor.cpp calls it each frame) |
| `RenderPacket::background` consumed | **implemented** — the renderer clears with `background.color * strength`; `clearColor` is still filled for the deprecated path |
| `RenderPacket::lights` consumed | **implemented** — every Directional and Point light, up to a cap of 8, with ignored lights logged once per change; an empty list falls back to a documented default sun. **D19's deletion is COMPLETE (D25): `RenderPacket::light` and `DirectionalLight` are gone from core** — `render` reads `lights` exclusively and `scene` no longer fills the dead field. |
| Grid density (`ViewportSettings::gridSubdivisions`) | **implemented** — read from `viewportSettings()`, clamped [1,100] in render, grid rebuilt only when the clamped value changes |
| Shadows (`shadowLightIndex`, `ShadowSettings`, `castShadow`/`receiveShadow`) | **implemented (wave 4, item 103)** — `Renderer.cpp:478-564` renders a directional shadow map, gated on `shading == Rendered` plus `shadows.enabled`, with honest warnings when there is no caster or the light is not directional. Scene resolves `shadowLightIndex`. Verity's verification: "shadow map LANDED — grep 140 refs (was 0), shadow lands where geometry predicts" (`kb_mvbkbqewLxAHaNI1hrVU`) |
| `DrawItem::materials` / `triangleMaterialSlots` | **end-to-end initial path implemented:** scene builds per-emitted-triangle assignment, render consumes slots and fallback rules; GPU pixel tests cover slot authority, per-face differences, and alpha. Editable-face `.ember` persistence and exhaustive modifier mapping remain |
| Collections | **backend and Outliner UI implemented** — nested tree, create/delete context actions, visibility toggles, object drag/drop membership, and undo; `test_scene_collections` plus `test_editor_outliner` cover data and the live UI |
| 3D cursor (`World::cursor/setCursor`) | **implemented and persisted** — item 157; consumed by the pivot and modal/gizmo paths |
| `PivotMode` | **wired and tested** — cursor, median, bounding-box and individual-origin behavior; multi-object transforms still operate on the active object only |
| Scene camera | **implemented** — `CameraSettings` on `Object`, v3 camera record preserved in v8 with v1-v7 migration, Camera Properties, and App-owned KP_0 look-through; item 33's viewport camera glyph remains residual |
| `OrbitCamera` navigation API (`setView`, `pan`, `setOrthographic`, `frameBounds`) | **implemented**; App drives it for orbit view and falls back to it when scene-camera look-through is off |
| `World::pick` / `screenToRay` | **implemented**; the editor calls them from `Viewport.cpp` |
| **Wave 2 (D22):** material slots, `WorldBackground`, `ShadowSettings`/`shadowLightIndex`, `PivotMode`, the 3D cursor, collections | **Mixed: implemented and tested** — world background/shadows consumed by render; D45 material slots/per-face mapping now consumed and pixel-tested; cursor and PivotMode persisted/wired; collection UI in Outliner |
| **Wave 3 (D29):** `Rect` (core), `Renderer::setViewportRect`, `Editor::viewportRect`, the area/header/region layout, the `G`/`R`/`S` modal-transform state machine | **ALL FOUR NOW IMPLEMENTED (re-checked 2026-10-17).** `Rect`, `setViewportRect` and `viewportRect` are in use (measured: a 1280x720 window yields a **996x515** 3D area that the renderer, the packet aspect and the input relay all share); the modal state machine is reachable from `Editor.cpp:196`; and the **layout is built** — `Workspace.cpp` creates one DockSpace with `DockBuilderSplitNode` splitters and a fixed default layout, with the 3D view as the retained central node. Shadows, previously listed here as not implemented, **landed in wave 4** — see the row above. The earlier single-row "frozen, not implemented" was wrong in both directions; this ledger is why it got caught |

### 4.4 `scene` — `src/scene/Scene.h`

```cpp
namespace ember::scene {

enum class Primitive { Cube, Sphere, Plane, Cylinder, Torus };

// What a pick found (D16). `distance` is along the ray in world units, so the
// editor can order hits; `worldPoint` is where the surface was hit, which is
// what gizmo placement (M5) needs.
struct PickResult {
  ObjectId object;
  float    distance = 0.0f;
  Vec3     worldPoint{0, 0, 0};
};

// Owned by `scene` (see the note below the block): `editor` links `scene` and
// may include this header, so `Object` does NOT belong in `core`.
struct Object {
  ObjectId     id;                    // == the key it is stored under
  std::string  name;

  ObjectId     parent;                // invalid() == a root object
  Transform    transform;
  MaterialDesc material;

  MeshData     mesh;                  // owned here; DrawItem::mesh points into it

  // Additive, agreed with scene (D8/D9) — QA and the outliner may rely on them:
  std::vector<ObjectId> children;
  Primitive    primitive = Primitive::Cube;
  bool         hasMesh   = false;     // a plain createObject() has no geometry
  bool         visible   = true;      // invisible emits no DrawItem

  // D19: a light is an ordinary object with a light component. Position and
  // direction come from `transform` (directional shines along local -Z), so they
  // are never duplicated here.
  bool         isLight   = false;
  Light        light;                 // meaningful when isLight
  CameraSettings camera;              // D47: meaningful when camera.isCamera
};

class World {
public:
  ObjectId createObject(std::string name, Transform = {});
  ObjectId createPrimitive(Primitive, std::string name = {});
  ObjectId createCamera(std::string name = "Camera", Transform = {});
  bool     destroy(ObjectId);
  Object*  get(ObjectId);
  const std::vector<Object>& objects() const;

  void setParent(ObjectId child, ObjectId parent);
  void setTransform(ObjectId, const Transform&);
  void setMaterial(ObjectId, const MaterialDesc&);

  // D19: lights are objects. createLight makes an object with isLight + a Light;
  // setLight edits one. The old world-level setLight/getLight pair is REMOVED —
  // there is no world light any more, only light objects.
  ObjectId createLight(LightType, std::string name = {});
  void     setLight(ObjectId, const Light&);
  bool     setCameraSettings(ObjectId, const CameraSettings&);

  void     setActiveCamera(ObjectId);  // invalid clears; otherwise camera objects only
  ObjectId activeCamera() const;

  const std::vector<ObjectId>& selection() const;
  void select(ObjectId, bool additive = false);
  void clearSelection();

  RenderPacket buildPacket(int width, int height) const;  // scene → renderer

  // Picking (D16), the consumer half: closest hit along a world-space ray, or
  // std::nullopt. Skips invisible objects. Only `scene` knows the meshes and
  // world matrices, which is why the hit-test lives here and not in render.
  std::optional<PickResult> pick(const Ray& ray) const;
};

bool save(const World&, const std::string& path);
bool load(World&, const std::string& path);

} // namespace ember::scene
```

**`Object` is `scene`'s, not `core`'s.** §4.1's rule is about types two modules
need *without* including each other's headers; `editor` links `scene` and may
include `scene/Scene.h`, so the object model stays in the module that owns the
data. Everything above is the **minimum** contract — `scene` may add to it,
never remove or rename these without saying so, because QA asserts on them and
the outliner displays them.

**Lifetime of a packet (D9).** `Object::mesh` is owned by the `Object`, inside a
dense `std::vector<Object>`. So a `DrawItem::mesh` pointer is valid **from
`buildPacket()` until the next mutation of the `World`** (`createObject`,
`destroy`, `createPrimitive` — anything that can reallocate the vector), and not
one frame longer. There is deliberately no `Handle<MeshData>` pool.

That is safe because of the frame order, which the composition root enforces:

```
poll input → editor/UI (may mutate World) → buildPacket → renderFrame → swap
```

Build the packet *after* the mutations and consume it *before* the next ones, and
no pointer is ever held across a reallocation. A consumer that wants to keep
geometry must copy what it uploads.

`buildPacket` fills `items`, `width`/`height` and `clearColor` — the things the
scene knows. It does **not** fill `view`, `projection` or `cameraPosition`:
those are view state, and `scene` may not include `render` (links go downward
only). **`App` fills them**, because it is the one place that sees both modules.
Without look-through it uses `render::OrbitCamera`; while look-through is active
it resolves `World::activeCamera()` and that object's world pose instead:

```cpp
RenderPacket packet = world.buildPacket(width, height);
packet.view             = camera.view();
packet.projection       = camera.projection(aspect);
packet.cameraPosition   = camera.position();
```

`CameraSettings` carries only projection: `fovY` is vertical radians,
`clipNear`/`clipFar` are positive near/far distances, `orthographic` selects
projection type, and `orthoScale` is the orthographic vertical world span. A
camera transform looks along local −Z with local +Y up; parents contribute via
`World::worldMatrix()`. `World::setCameraSettings` accepts only an existing
camera object and rejects non-finite or invalid projection ranges. An active
camera must be a camera object; destroying it clears the active id. v1 and v2
remain readable. A nonzero v2 legacy `active` id is migrated to a camera
component with default projection settings before it is made active. v3
persists each camera component with one `camera` record.

Look-through is App-local view state toggled by KP_0 while the viewport is
focused. It uses the active camera's world pose and projection fields; toggling
off restores the unchanged orbit-camera view. No new camera state is added to
`RenderPacket` beyond its existing matrices and position. `clearColor` travels
the other way — scene → packet → renderer.

**Lights (D19).** `buildPacket` also resolves every light object into
`RenderPacket::lights`, in world space: a directional light's `direction` is its
object's local **−Z** run through the object's world rotation, a point light's
`position` is its object's world translation. `scene` performs that resolution,
so the renderer never sees a `Transform`. The deprecated `RenderPacket::light` is
filled from the first directional light for as long as `render` still reads it,
and is deleted in the same edit that stops reading it.

**The picking call path (D16), so nobody has to invent a link.** `editor` links
`core`, `render` and `scene` (§5), and `draw()` already receives the
`render::Renderer&`, so the editor can complete the whole gesture itself:

```cpp
const Ray ray  = renderer.camera().screenToRay(mx, my, packetWidth, packetHeight);
if (auto hit = world.pick(ray)) world.select(hit->object, additive);
```

That needs **no new signature on `Editor::draw`** — the two halves above plus
`Ray` in core are the entire interface. `App` stays out of it: it owns the camera
state (input deltas) but picking is a UI gesture, and the editor owns the click.

**Shipped beyond the block above** (checked 2026-10-16 — the class grew; these are
all in the tree, and the ones marked * are exercised by tests):

`Object::isLight`, `Object::light`, `Object::camera`, `Object::children`;
`createPrimitive`, `createLight`, `createCamera`, `setLight(ObjectId, Light)`,
`setCameraSettings`*, `setActiveCamera`*, `setMaterial`*, `setTransform`*,
`setName`, `setVisible`, `parentOf`*, `childrenOf`, `has`, `isDescendant`,
`worldMatrix`, `duplicate`*, `destroy`*, `captureSnapshot`/`reinsert`* (reversible
delete for the undo stack), `pick`*, `bounds`/`selectionBounds`, and the free
functions `save`/`load`*. `World` no longer has a world-level `setLight`/`light()`
— removing it broke `tests/test_scene_io.cpp` for a day, which is the practical
cost of a signature removal and the reason D19 was announced twice.

### Wave 2 scene surface (D22) — status per item, checked 2026-10-16

| Piece | State |
| ----- | ----- |
| 3D cursor — `Vec3 cursor() const; void setCursor(const Vec3&);` | **not implemented** (persisting it in `.ember` is Slate's, with a version bump) |
| Collections — `CollectionId`/`Collection` (scene-owned, like `Object` per D8), `createCollection`, `destroyCollection`, `setCollectionVisible`, `assignToCollection`, `collectionOf`, `get(CollectionId)`, `collections()`, plus `captureCollection`/`reinsertCollection` for undo | **written, build currently red** — `CollectionId` is declared *after* its first use in `Scene.h`, so nothing compiles until that is reordered |
| Material slots — `Object::materials`, `setMaterialSlot`, `addMaterialSlot`, `removeMaterialSlot`, `materialSlots` | **implemented scene-side**; `DrawItem::materials` is filled, and `render` does not read it yet |
| `RenderPacket::lights` | **filled by scene** (`buildPacket` resolves directional along local −Z and point at the world translation); `render` does not read it yet |

The rules that make this cheap to implement and hard to get subtly wrong (they are
the decision, not commentary):

1. **Collections do not transform.** A collection is grouping + inclusion; the
   transform hierarchy is still `Object::parent`. Moving an object between
   collections never moves it in space.
2. **Membership is a single `CollectionId` on `Object`** (`invalid` = the implicit
   root collection, so no root collection has to be created). Blender allows an
   object in several collections; that is **deferred**: adding it means changing
   the field to a vector plus a rule for exclusion, which is a deliberate later
   change, not a v1 ambiguity.
3. **Exclusion is one lookup:** an object emits a `DrawItem` iff
   `object.visible && (!collection.valid() || collection.visible)`. The same rule
   the outliner's eye icons show, so the panel and the frame cannot disagree.

`setMaterial` keeps its current meaning (writes slot 0) and `Object::material`
becomes slot 0's material once the array lands — same deprecation staging as
`DirectionalLight`/`packet.light`; that staging is COMPLETE — both were deleted
in D25 once `render` read `lights` exclusively and `scene` stopped filling them.

### The editable mesh — `scene`-owned, index-based, adjacency stored (D34, frozen)

Item 110 implements this; item 111 mutates it; item 112 tests the invariants.
**`EditableMesh` is `scene`'s**, like `Object` and `Collection` (D8): `editor` links
`scene` and reads it for selection and overlays, and **`render` never sees it** — the
renderer keeps consuming `MeshData` so nothing in the render path changes.

**The decision, and the alternative it rejects.** *Extending `MeshData` and rebuilding
adjacency on demand* loses on the stated requirement: "adjacency (vertex → edges →
faces) must be cheap, because selection and extrude need it every frame". A rebuild is
O(mesh) per query; a selection highlight asks per frame. So adjacency is **stored and
maintained**, not derived — and the structure is **Blender's `bmesh` data model
expressed with indices instead of pointers** (`WAVE-5.md` §1.5: read `bmesh` first, port
the design, do not import the subsystem — it drags `BLI_*` and `DNA`).

```cpp
namespace ember::scene {

// Corner of a face. Attributes that can differ per corner live HERE, which is what
// makes flat and smooth shading representable and the MeshData conversion exact.
struct Loop { u32 vertex; u32 edge; u32 next; u32 prev; u32 face; Vec2 uv; };
struct Edge { u32 v0, v1; };                    // undirected; v0 < v1 by construction
struct Face { u32 firstLoop; u32 loopCount; u32 materialIndex = 0; };  // loopCount >= 3

struct EditableMesh {
  std::vector<Vec3>  positions;      // vertex positions, index == vertex id
  std::vector<Loop>  loops;          // every face's corners, contiguous per face
  std::vector<Edge>  edges;          // unique undirected edges
  std::vector<Face>  faces;          // n-gons; n >= 3, concave allowed

  // STORED adjacency, CSR (contiguous, cache-friendly, index arithmetic only):
  std::vector<u32>  vertexEdgeOffsets, vertexEdges;   // vertex -> its edges
  std::vector<u32>  vertexFaceOffsets, vertexFaces;   // vertex -> its faces
  std::vector<u32>  edgeFaceOffsets,   edgeFaces;     // edge   -> its faces (1 or 2)
  u32 topologyVersion = 0;                            // bumped by every topology change

  static EditableMesh fromMeshData(const MeshData&);  // one face per triangle
  MeshData toMeshData() const;                        // triangulate; split corners only on attribute conflict
  bool validate(std::string* why = nullptr) const;    // the invariants below; tests call this
};

} // namespace ember::scene
```

**Invariants** (`validate()` checks them; item 112 asserts them after every op):

1. Every `Loop` sits in exactly one face, and every face's loops are contiguous and
   `loopCount >= 3`.
2. Every `Edge` is unique and undirected (`v0 < v1`), and is referenced by exactly the
   loops that lie on it — 1 loop for a boundary edge, 2 for an interior one.
3. `edgeFaceOffsets`/`edgeFaces` list **1 or 2** faces per edge: 0 is impossible (an
   unused edge is a defect, not a state) and 3+ means non-manifold, which ops must
   refuse rather than create.
4. The CSR arrays agree with the loops: `vertexEdges[v]` is exactly the set of edges
   incident to `v`, `vertexFaces[v]` exactly the faces touching `v`.
5. `positions.size() == vertices` — every vertex is referenced by at least one loop
   (orphan vertices are removed by the ops, not left to accumulate).

**Triangulation and the round trip (the two things item 112 will test):**

- **`EditableMesh → MeshData` is lossless for rendering**: positions are preserved
  exactly, and the surface is triangulated with **ear clipping so concave n-gons are
  correct** — a fan over a concave face produces overlapping triangles, which is the
  difference between "polygonal faces are representable" and "polygonal faces render
  right". The triangulation is *derived*: the n-gon stays the source of truth.
- **`MeshData → EditableMesh → MeshData` is an identity** (positions, indices, normals
  and uvs equal) for any `MeshData` our primitives produce. Vertices are preserved
  one-for-one, and `toMeshData` splits a corner into a new vertex **only** when two
  faces sharing that vertex disagree on a corner attribute — so a mesh that came from
  `MeshData` needs no split and comes back byte-equal.
- **The asymmetry to document, not to hide:** polygon grouping does **not** survive a
  `MeshData` round trip, because `MeshData` has no notion of an n-gon — it is triangles
  by construction. So `fromMeshData(toMeshData(e)) == e` holds only for an `e` whose
  faces are all triangles with consistent corner attributes. Item 112 must assert the
  identity in the direction that holds and the *weakening* in the other, rather than
  discovering it.

**Storage on `Object`, and why render does not move.** `Object` keeps `MeshData mesh`
as the render-and-pick form (the D8/D9 contract: `DrawItem::mesh` points into it, `pick`
ray-tests against it) and gains an **optional `EditableMesh edit`**. Once an object is
edited, `edit` is the source of truth and `mesh` is regenerated from it on mutation, so
the renderer and the picker keep working unchanged. The cost is honest: **edited objects
hold their geometry twice** — accepted for now because it keeps a large subsystem
(render) entirely out of this change; revisit only if it is ever measured to matter.

**Sub-element selection.** Selection is **index sets plus `topologyVersion`**: a
selection made at version *v* is invalid at *v+1*, and every op must either **remap** it
(move: indices are stable) or **clear** it (extrude, delete, merge: indices shift) — and
must never leave it dangling. That rule is what makes undo cheap (item 111 reuses
`captureSnapshot`/`reinsert`), and it is testable, which is why it is stated as a rule
rather than left to each op.

**The editor's route into the geometry: item 116's surface.** Listed here explicitly, because
it was previously declared and defined in `Scene.h` but documented **nowhere** — which is how
a careful reader came to conclude it did not exist and asked for it twice. All of it is
`scene`'s, on `World`; `src/editor/**` calls it and must not write into `Object::edit`:

| Call | What it is, and the rule it carries |
| ---- | ----------------------------------- |
| `EditableMesh* editable(ObjectId)` | the public entry; **lazily converts `mesh → edit`** on first call (D34's storage rule) |
| `const EditableMesh* readableEdit(ObjectId) const` | absence-honouring read — never creates an `edit`, so asking about an unedited object is free |
| `editSetSelection` / `editReadSelection` / `editClearEditSelections` | the sets `scene` holds, stamped with the `topologyVersion` they were made at; `editReadSelection` returns `nullptr` when absent **or stale** |
| `editMoveVertices(object, ids, worldDelta)` | a **world-space** delta; scene maps it through the item-66 helpers so the editor does no matrix maths |
| `editCaptureVertexPositions` / `editApplyVertexPositions` | **absolute**, mesh-local, **EXACT** (no delta arithmetic, no drift), **no version bump**, all-or-nothing (any bad id refuses the whole batch) |
| `editExtrudeFace` / `editDeleteSubElements` / `editMergeVertices` | topology ops; take core's `SelectMode` (D37), rebuild what they touch, bump `topologyVersion` |

The capture/apply pair is what D29's cancel needs: **capture at gesture start, apply to
restore, never reverse-accumulate** — and one command object holding both arrays is what
keeps a drag to exactly one undo entry. Its first consumer is item 116's editor code; before
that the whole surface was "declared and defined" in the sense that links, not "reachable"
in the sense that matters (TEAM.md rule 8).

**Residue — what this decision does NOT give you:**

- **Per-face `materialIndex` is persisted by `.ember` v8** and consumed through
  per-triangle packet slots. The new v8 round-trip tests compile, but Application
  Control prevented runtime verification in this measurement.
- **Normals are derived, flat by default** (face normal, corner-split) — which matches
  the renderer's current behaviour. Per-face smooth/flat flags are a later additive
  change (they are on Verity's gap list).
- **No subdivision, no bevel, no boolean** — those are item 111's ops and, where a
  library exists, WAVE-5 §1.5 says to use it (OpenSubdiv, Manifold) rather than port.
- **No holes or multiple loops per face**: a face is one simple ring. Blender allows
  more; this is a documented limitation until something needs it.
- **Adjacency maintenance cost is per-op, not per-frame** — that is the point — but no
  op may bypass it: a mutation that edits `positions` without touching topology is fine
  (indices stable, no version bump needed); anything that adds or removes an element
  must rebuild the CSR slices it affects **and** bump `topologyVersion`.

### Modifier stack: ordered, per-object, evaluated at packet build (D40, item 120)

**Status: contract only — NOT implemented.** `Object` does not have these fields yet, `render` has
no modifier code (verified: zero references), and nothing evaluates a stack. Item 121 adds the
scene side; item 122 the panel. This block describes the **frozen shape, not the tree** — stated
explicitly because a frozen shape that implies implementation is this project's most repeated
failure (the shadow map described in `Renderer.h` with zero references in `Renderer.cpp`,
`TEAM.md` §8).

**Storage.** `Object` gains `std::vector<Modifier> modifiers` in **evaluation order**
(index 0 first), plus two counters that make caching possible and honest:
`u32 modifierVersion` — bumped by *any* stack mutation (add, remove, reorder,
enable/disable, parameter change) — and `u32 meshVersion` — bumped by *any* API that
replaces `Object::mesh` (primitive creation, load, undo reinsert, edit-mode sync). Entries
are **declarative only**: parameters and an `enabled` flag, never geometry.

**Evaluation sits in `scene`, inside packet building** — not in render, not in the editor,
not in the input path. For each visible object, in this order:

1. resolve the **base mesh**: `hasEdit ? edit.toMeshData() : mesh` (D34: once edited, the
   editable mesh is the source of truth);
2. walk `modifiers` by index, **skipping disabled entries** — their position is preserved,
   so re-enabling restores the order rather than appending;
3. each modifier takes `const MeshData&` and returns a new `MeshData`; its output is the
   next one's input;
4. the final result is stored in the packet's **evaluated-mesh storage** (owned by the
   world, alive for the frame) and `DrawItem::mesh` points into it — the D9 lifetime rule.

**Non-destructive by construction.** Evaluation never writes `Object::mesh` or
`Object::edit`: the input is `const`, the output is a value, and a modifier is a pure
`MeshData -> MeshData` step with no reference to the object. Removing the stack — or one
modifier — therefore restores the original **exactly**, because the original was never
touched. That is what item 123 tests: base bytes unchanged, evaluation deterministic, and
removal byte-identical to the original.

**Determinism is contract, not hope.** A modifier may read only its parameters and the
incoming mesh — never frame time, the camera, the selection, or any global. Same inputs
produce identical bytes, or the cache below is unsound.

**Caching.** The evaluated result is reused while `(baseSerial, modifierVersion)` is
unchanged, with `baseSerial = edit.topologyVersion` when `hasEdit`, else `meshVersion`.
Without it a static scene pays the whole stack every frame; with it the cost is paid on
change. **Residue:** a missed `meshVersion` bump silently serves stale geometry, so the
bump is a rule for every mutator and mutation tests must assert it.

**Edit mode suspends the stack.** In `InteractionMode::Edit` the viewport draws the **base
mesh**, because the overlays, vertex dots and picking indices all describe the base
topology — drawing an evaluated result under base-indexed handles is a lie you can click
on. (Blender shows the evaluated mesh while editing and offers a per-modifier "show in edit
mode"; that is a *future additive* flag, recorded here as the deliberate deviation.)
Corollary, and it is the same rule in two places: **object-mode picking uses the evaluated
mesh** (what you see is what you click); **edit-mode picking uses the base** (what you edit
is what you click).

**Failure is a no-op, never a blank object.** A modifier that cannot evaluate (bad
parameters, empty input) leaves the incoming mesh unchanged, records the failure on the
evaluation result for the editor to show, and the stack continues. Non-destructiveness
extends to failure: a bad modifier cannot destroy the object or blank the viewport.

**The renderer needs no knowledge of modifiers.** It receives `DrawItem::mesh` — a
`MeshData` — exactly as before this decision. No render change, no modifier type in
`render`, no ordering logic there. **If `render` ever needs to know that a modifier exists,
this decision has been violated**, and that sentence is the test of the design.

**How a modifier declares its parameters.** The typed struct per kind *is* the declaration
(`SubdivisionParams`, `MirrorParams`, `ArrayParams` in `core`); `ModifierParams` is a
`std::variant` of them and **the variant index is the kind** (`Modifier::kind()` derives
it), pinned by `static_assert`s on the alternative order — so an entry cannot claim one
kind while holding another's parameters. `modifierDesc(kind)` returns that kind's table:
stable `key` (what is serialized), `label` (display only), `type` (`Float`/`Int`/`Bool`),
range and step. **The editor builds its controls from that table and save/load reads and
writes it; neither switches on the kind.** Residue: the compiler cannot verify that the
table matches the struct's fields, so item 123 must round-trip every declared key.

**Residue of this decision:** no per-modifier "show in edit mode" flag; no per-modifier UI
state; evaluation cost is per object per *change* (bounded by the cache), and the
`meshVersion` invariant is an obligation on every mutator — the exact class of obligation
that has been forgotten before, which is why it is a test requirement rather than a note.

### Edit-mode overlays: the editor extracts, render draws (D37, item 118)

The same split as the gizmo (D20/D29), for the same reason — `render` may not include
`scene` and must not walk topology:

```cpp
void Renderer::drawEditOverlay(const EditOverlayState&, const Mat4& view, const Mat4& projection);
```

**The payload is render-ready primitives, NOT the editable mesh.** `EditOverlayState`
(core, §4.1) carries `vertexPoints`, `edgeLines`, `faceTriangles` + `faceOfTri`,
parallel `*Selected` flags, the object's `mesh` and `model` matrix, the mode, the
screen-space sizes and the theme colours. The **editor** builds all of it from
`EditableMesh` — because `MeshData` is triangles and cannot express the model's edges or
its n-gons, so the overlay is the only place the editable topology becomes drawable.
`render` then draws points, lines and triangles like any other geometry.

**Rejected alternative, recorded:** passing `EditableMesh*` to render. It would make
`render` include `scene` and walk topology, which is the one dependency the module map
exists to prevent (`render` consumes `RenderPacket`, never the world).

Rules that are contract:

1. **Gating.** Overlays draw only when `state.mode == InteractionMode::Edit` and the
   object is visible; per-primitive visibility comes from `ViewportSettings`
   (`showEditVertices` / `showEditEdges` / `showEditFaces`), which the editor writes and
   render reads through `viewportSettings()` — the same one-knob rule as the grid.
2. **X-ray** (`ViewportSettings::xray`) means depth test off for the overlay pass, so
   vertices behind the surface are still reachable and visible — the reason Blender has
   it. Default off = depth-tested, so an overlay cannot show through geometry.
3. **One number, two consumers.** `vertexRadiusPixels` / `edgeWidthPixels` are what the
   renderer draws with AND what the editor hit-tests against (item 115's picking), so the
   dot you see and the dot you click cannot drift.
4. **Theme is the editor's.** Colours arrive in the state so `render` hardcodes no
   palette; Blender's edit selection is orange, and our default here is orange.
5. **Lifetime.** Non-owning, valid for the frame — the D9 rule. The editor should cache
   the extraction and rebuild it when `topologyVersion` changes rather than every frame;
   that is an optimisation the contract permits, not a requirement.
6. **Counts are not in the state.** WAVE-5's live vertex/edge/face counts are header UI:
   only the editor knows the selection, so it derives them from the mesh it already holds.

### 4.5 `editor` — `src/editor/Editor.h`
```cpp
namespace ember::editor {

class UndoStack;                   // editor-owned (M5)

class Editor {
public:
  // `nativeWindow` is platform::Window::nativeHandle() — a GLFWwindow*, typed as
  // void* so that editor does NOT link or include platform (see the note below).
  // Requires a current GL context with the loader already up (Window::create does
  // both). Creates the ImGui context and both backends.
  bool init(void* nativeWindow);
  void shutdown();
  void draw(scene::World& world, render::Renderer& renderer, const RenderStats&, UndoStack&);
  bool wantsViewportFocus() const;

  // D29: the 3D viewport AREA in framebuffer pixels, top-left origin. Invalid
  // (w/h <= 0) until the layout has been built. `App` needs it for three things:
  // the render viewport (`Renderer::setViewportRect`), the packet's width/height
  // (aspect), and relaying a window-coordinate copy to `Input::setViewportRect`.
  Rect viewportRect() const;
};

} // namespace ember::editor
```

`draw` takes `scene::World&`, not `Scene&` — §4.4 is the contract, and there is
no type called `Scene`. Naming `scene::World` and `render::Renderer` here is not
an upward link: `editor` is allowed to link `core`, `render` and `scene` (§5).

**Why `void*` and not `platform::Window&` (D13).** §5 lets `editor` link `core`,
`render` and `scene` — **not `platform`**. Passing the window as `void*` keeps
that true while still handing ImGui what `ImGui_ImplGlfw_InitForOpenGL` needs:
`editor` casts to `GLFWwindow*` itself and links the `glfw`/`glad` vendor targets
directly. A `platform::Window&` parameter would force a link §5 forbids.

**`editor` owns the ImGui frame.** `draw()` drives it end to end — `NewFrame`,
the panels, `Render`, backend draw — so `App` never touches ImGui. `App` owns the
`UndoStack` and passes it in, because `App` is the composition root (§4.6).

**Frame order** (the composition root's job, §4.6):

```
poll events → stats → buildPacket → renderer.renderFrame(packet)
            → editor.draw(world, renderer, stats, undo)   ← ImGui NewFrame..draw
            → swapBuffers
```

For M3 the simple composition is enough: the renderer draws the 3D viewport
region and ImGui draws the panels around it. If the viewport later has to live
*inside* a docked ImGui window, use `Renderer::readViewport` (§4.3) — that is what
it is for, and it needs no contract change.

### Layout: areas, headers, regions — nothing floats (D29)

**Blender's model, adopted as written.** An ImGui window the user can drag around
the screen is a bug in this editor, not a feature:

| Concept | Contract |
| --- | --- |
| **DockSpace** | ONE, covering the whole main viewport. Every area window is built with `NoMove \| NoResize \| NoCollapse \| NoBringToFrontOnFocus \| NoSavedSettings`, so none can float, overlap or be raised. |
| **Area** | A region running one editor (3D View, Outliner, Properties, Timeline). Areas **tile**: no gaps, no overlaps, nothing on top of anything. |
| **Splitters** | Area borders are draggable (dock-builder splits), so the layout resizes but never detaches. |
| **Header** | Every area has a header strip (top or bottom). Left end: editor-type selector. Right end: that editor's controls. The 3D viewport's header carries shading mode, overlays, transform orientation/pivot/snap and the gizmo toggles. |
| **Topbar** | The main menu bar, pinned at the top. Menus drop *from* it. |
| **Status bar** | Pinned to the bottom edge, always visible, carrying hints **and the live transform readout**. |
| **Toolbar (`T`)** / **Sidebar (`N`)** | **Regions inside** the 3D viewport area — child regions, not floating palettes. |
| **Properties** | One area: a vertical icon column plus a single scrollable column of collapsible panels — not one window per category. |

**Deterministic layout.** `io.IniFilename = nullptr` (no `.ini`, so nothing drifts
or surprises), and the default screen is built by code: 3D Viewport centre-left,
Outliner top-right, Properties bottom-right, Timeline across the bottom, status bar
under everything. Rebuilding the layout must be idempotent — running it twice
changes nothing.

**D41 — freeze clarification: the root, tree and writer boundary.** The screen has
three non-overlapping horizontal bands: the pinned Topbar, the tiled DockSpace, and
the pinned Status Bar, in that order. The DockSpace fills exactly the space between
Topbar and Status Bar; neither is a dock node. This resolves the shorthand “one
DockSpace covering the whole window”: it covers the whole *editor work area*, and no
area may cover either pinned strip. Every pixel of the work area belongs to exactly
one editor area — no gap, overlap or floating window.

The default dock tree is deterministic (applied on every launch; no `.ini` restore):

```text
DockSpace
├── upper: 80% height
│   ├── centre-left: 78% width  → 3D Viewport (central node)
│   └── right:       22% width
│       ├── top:    55% height → Outliner
│       └── bottom: 45% height → Properties
└── bottom: 20% height → Timeline (full DockSpace width)
```

The ratios are **initial proportions**, not locked sizes. A splitter moves only the
shared boundary between its sibling nodes; dragging it cannot undock, overlap, or
create a gap, and resizing the outer window resizes the same tree. The logical area
identities are stable (`Viewport`, `Outliner`, `Properties`, `Timeline`); each area
hosts one active editor, with its Header/type selector and that editor's controls.
Switching an editor type does not change the dock tree. Toolbar (`T`) and Sidebar
(`N`) remain child regions inside the Viewport area, never dock nodes. The Properties
editor remains one area with its category strip and one scrollable panel column.

**Ownership does NOT widen.** `Chisel` remains the sole writer of `src/editor/**`,
including the DockSpace, default tree, area headers, Topbar, Status Bar and viewport
regions. The layout is one tightly coupled ImGui composition surface; splitting its
files across writers would reintroduce concurrent edits to shared UI state and the
source-file locks this project has already hit. `Verity` can write independent layout
tests in `tests/**` in parallel, and other members can work in their own disjoint
paths; no one else edits the editor implementation. If implementation discovers a
needed platform/scene/render interface, request it from that owner before coding.

**Implementation status rechecked 2026-10-18:** `Workspace.cpp` now splits a full-width
bottom Timeline at the D41 20% startup share, then the centre/right column and Outliner/
Properties. The four docked Areas have editor-type Headers; Toolbar and Sidebar are
regions inside the 3D Viewport; Status Bar is pinned below the dockspace. `test_editor_layout`
and `test_editor_ui_chrome` cover tiling, splitter interaction, category/header reachability,
and the live pinned chrome. This implements the frozen startup structure; further work is
visual/interaction fidelity, not the missing-Timeline gap recorded at decision time.

**Reference checked for the Blender model:** the Blender Manual defines Areas as
rectangles reserving screen space for editors, and Regions as subdivisions within an
editor. See [Areas](https://docs.blender.org/manual/en/latest/interface/window_system/areas.html)
and [Regions](https://docs.blender.org/manual/en/latest/interface/window_system/regions.html).

**Vocabulary:** Blender's words, not new ones — Area, Header, Region, Topbar, Status
Bar, Outliner, Properties, Sidebar, Toolbar, Collection.

### Timeline with no animation system (D44, item 180)

**Decision: option (a) — the Timeline is a frame-range/playhead ruler only, not an
animation editor.** Scene has no animation curves, keyframes, channels or playback
evaluation. Do not pretend otherwise by adding transport/record buttons that do
nothing, and do not grow a partial animation system inside this Area. Blender's
Timeline is materially different: its manual says it is used to jump to frames,
manipulate keyframes and control animation playback. That full behavior is deferred;
the difference is recorded plainly here.

**Frozen user-facing wording:** the Area/editor name is `Timeline`; its header shows
`Frame N`. Remove the apologetic body sentence “Not connected to animation. The
playhead only moves this view.” There is no replacement warning paragraph in the
panel—the difference is a documented scope boundary, not a perpetual error banner.

**The panel carries exactly these controls:**

1. `Start` integer field (default 1) and `End` integer field (default 250). Both are
   editable; if `End < Start`, clamp End to Start.
2. A full-width ruler over the inclusive range `[Start, End]`, with numbered frame
   ticks and a visible playhead at `current`.
3. Click/drag on the ruler sets `current` to the nearest frame, clamped to the range.
   The header's `Frame N` readout updates with it.

**Explicitly absent, until a separate animation decision lands:** play/stop/rewind or
transport controls; keyframe diamonds or channels; markers; auto-keying; playback
range/sync controls; scene animation state; persistence of the editor-local
`TimelineState { start, end, current }`. The playhead changes only this Area's view
state and does not modify scene objects or produce animation. This matches the current
source model (`Panels.h` stores only those three integers) without implying Blender
animation parity.

Reference checked: [Blender Manual — Timeline](https://docs.blender.org/manual/en/latest/editors/timeline.html)
describes frame navigation, keyframe manipulation and playback; those features are
the deliberate difference, not a half-implemented promise.

### Default theme tokens, scale and closed UI vocabulary (D42, item 150)

**Scope:** these are Ember3D's frozen *default* tokens, not a claim that Blender has
one unchangeable RGB palette. Blender's manual says Themes customize interface
appearance/colors, with colors configurable per editor; its Interface preferences
define Resolution Scale for fonts/buttons. We use that as the model: one coherent
dark default across all our editor areas, with the following pinned values. Colors
are sRGB `#RRGGBBAA`; alpha is applied over the area below.

| Token | Value | Use |
| ----- | ----- | --- |
| `header.bg` | `#222222FF` | Area Header strip |
| `region.bg` | `#2E2E2EF5` | ordinary editor Region/panel; 96% opaque, leaving a faint underlying dark blend |
| `viewport.body.bg` | alpha `0` | 3D Viewport body only; the scene/grid is rendered behind the transparent host |
| `widget.field.bg` | `#1C1C1CFF` | input fields and framed controls |
| `widget.button.bg` | `#3D3D3DFF` | neutral buttons |
| `text.primary` | `#CCCCCCFF` | normal text |
| `text.disabled` | `#757575FF` | disabled/secondary text |
| `selection` | `#ED731AFF` | selected object/row/element — Blender orange, **not blue** |
| `active` | `#80BFFFFF` | active element — lighter blue, distinct from selection |
| `hover` | `#FFFFFF14` | neutral white wash (~8% alpha) over the underlying token; hover never impersonates selection/active |

Selection/header fills use the semantic token with 45% alpha; active fills use the
active token with 85% alpha; text/icon indicators use the opaque token. The existing
user accent preference may override the default selection orange, but the default is
the value above; active-blue and neutral-hover semantics do not change with that
preference.

**Metrics, logical pixels at UI scale 1.0 (13 px baseline font); platform DPI scale
multiplies every metric uniformly:**

| Metric | Frozen value |
| ------ | ------------ |
| Area Header height | `27` logical px |
| Standard widget/frame height | `19` logical px (`13` px baseline text + `2 × 3` vertical frame padding) |
| Inter-item spacing | `8 × 4` logical px (horizontal × vertical); inner spacing `4 × 4` |
| Region/panel/tab rounding | `0` px (square tiled areas) |
| Widget/button/grab rounding | `3` logical px |
| Region background alpha | `0.96`; Viewport body is the explicit `0.0` exception |

Metrics derive from one UI scale so a high-DPI display does not change proportions.
The Header height is the area strip, not the Topbar or Status Bar height. The 19 px
widget target assumes the pinned baseline font and padding; font/DPI scaling multiplies
both together rather than silently changing the theme's proportions.

**Closed user-facing vocabulary (spelling and capitalization are canonical):**

| Concept | Only label used |
| ------- | --------------- |
| Scene object tree | `Outliner` |
| Object/world settings editor | `Properties` |
| Viewport side region | `Sidebar` |
| Viewport tool region | `Toolbar` |
| Tiled editor rectangle | `Area` |
| Editor control strip | `Header` |
| Subdivision within an editor | `Region` |
| Scene grouping | `Collection` |
| Non-destructive stack operation | `Modifier` |
| Object interaction state | `Object Mode` |

Do not substitute synonyms such as “Hierarchy”, “Inspector”, “Toolbox”, “Panel” for
these labels in user-facing chrome. These are the closed names for these ten concepts,
not an exhaustive dictionary of ordinary prose.

**Reference checked:** [Blender Manual — Themes](https://docs.blender.org/manual/en/latest/editors/preferences/themes.html)
states that themes customize interface appearance/colors and that editor colors can
be set separately. [Interface Preferences](https://docs.blender.org/manual/en/latest/editors/preferences/interface.html)
describes Resolution Scale as adjusting font/button size relative to detected DPI.
Accordingly, the numerical tokens and shared scale above are Ember3D's explicit
selection, not falsely attributed Blender constants.

**Implementation gap at decision time:** `Editor.cpp::applyTheme` already has the
27 px Header, 3 px frame rounding, 8×4 item spacing and dark region colors, but its
region alpha is still 1.0, its active color is derived from the orange accent, and
its hover fill is accent-colored. D42 freezes the target; this is not a claim that
the implementation already conforms.

### UI geometry contract (item 177)

The normative geometry rules are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): exact Area Header allocation,
logical-pixel/UI-scale semantics, content-sized controls with overflow handling, and Status Bar ownership of the
bottom edge. Items 169 (viewport-header clipping) and 171 (Timeline/status overlap) are the measured regressions
that this contract prevents. This link is the single implementation contract; do not copy per-widget constants.

### Icon drawing strategy for Properties and Toolbar (item 179)

**Decision: original vector glyphs drawn at runtime with ImGui's `ImDrawList`.** Do
not bundle an icon font or sprite atlas. The tree has text fonts but no icon font or
icon atlas; adding either would add binary assets, font/atlas packing, versioning and
an additional licence/provenance surface for a small fixed set of editor glyphs.

**Artifact and owner:** `src/editor/Icons.h` declares `enum class IconId` and the
drawing interface; `src/editor/Icons.cpp` contains the glyph geometry and is added to
the editor target. `Chisel` owns both files under `src/editor/**`. The glyphs are
original Ember3D shapes drawn from primitive lines, rectangles, circles and polygons
on a normalized **16×16 logical-pixel grid**, scaled to the supplied button rect by
`ThemeTokens::uiScale`. This is newly authored source under the repository's
**GPL-3.0-or-later** licence; there is no third-party asset or upstream icon code to
attribute. Do not trace/copy Blender icon source into these paths; if a future change
ports such source, D36's original SPDX/Origin/Modifications requirements apply.

**Where the icons go:**

| Surface | Icons, one per visible entry | Text/accessibility behavior |
| ------- | ---------------------------- | --------------------------- |
| Properties category rail | `Render`, `Output`, `Scene`, `World`, `Object`, `Material`, `Camera` | Icon-only rail; hover tooltip uses the exact category name; preserve keyboard focus/selection. |
| Toolbar (`T`) | `Select`, `Move`, `Rotate`, `Scale` | Icon-only vertical tool buttons; hover tooltip shows tool name and shortcut where one exists. Scale has no fake shortcut: modal `S` remains distinct. |

**Color/size:** draw idle glyphs in `ThemeTokens::textPrimary`, disabled glyphs in
`textDisabled`, active tool/category in `active`, and hovered button background with
`hover`. Selection orange remains for selected scene elements, not as a blanket
Toolbar tint. The caller provides a button rect; glyph bounds are centered and
uniformly scaled, never given a per-icon magic pixel size. Tooltips retain the
canonical text names from `UiVocabulary`/editor labels, so replacing letters with
glyphs does not remove the name.

**Adding an icon:** add one `IconId`, implement its 16×16 normalized primitive path
in `Icons.cpp`, add its label/shortcut mapping to the appropriate table, and add a
Verity-owned `tests/**` pixel test proving a non-empty bounded glyph at scale 1 and a
second DPI scale plus correct active/hover tint. No UTF-8 letter, emoji or private-use
font codepoint is an icon fallback. The file is the source of truth; there is no
generated atlas to keep synchronized.

**Status:** implemented in `Icons.h/.cpp`; the seven Properties entries and four
Toolbar entries are original normalized vector glyphs. `test_editor_icons` measures
non-empty bounded shapes, scaling and active/hover token tint. The Camera icon was
added with item 209; it follows the same rule and is included in the pixel test.

### Modal transforms — `G` / `R` / `S` (D29)

**Ownership: the editor's, entirely.** All gesture state lives in `src/editor`; no
core type is needed and `App`, `render` and `scene` learn nothing new. Keys come from
ImGui IO (the editor already reads keys that way), gated by `!io.WantTextInput` so
typing in a field is never a command — the keyboard analogue of the defect-1 camera
gate.

```
Idle --(G|R|S, viewport hovered, !WantTextInput)--> Running{mode, constraint, numeric, startTransform}
Running + mouse move   -> update live (suppressed while a numeric buffer is non-empty)
Running + X|Y|Z        -> constraint cycles: None -> Global(axis) -> Local(axis) -> None
Running + Shift+X|Y|Z  -> plane constraint (the two axes that exclude the pressed one)
Running + digits/-/.   -> numeric entry on the MAIN number row (numpad stays navigation)
Running + Shift(held)  -> precision: value x 0.1
Running + Ctrl(held)   -> snapping: to the increment below
left-click | Enter     -> CONFIRM  -> exactly ONE command pushed
right-click | Esc      -> CANCEL   -> startTransform restored EXACTLY, no command pushed
```

Rules that are part of the contract, because each one is a way this goes wrong:

1. **One gesture at a time.** `G`/`R`/`S` while `Running` is ignored rather than
   nested. A modal gesture and a gizmo grab never overlap: while `Running`,
   left-click **confirms** and must not start a gizmo drag; while `Idle`, left-click
   on a handle starts a gizmo drag.
2. **Cancel restores, it does not undo.** The gesture re-applies the captured
   `Transform` (bitwise), so a cancelled gesture leaves no trace — including on
   parented objects, where reverse-accumulating a delta would drift.
3. **Exactly one undo entry per CONFIRMED gesture** — pushed at confirm, never per
   frame. Cancel pushes nothing.
4. **Axis semantics:** `X`/`Y`/`Z` are world axes first; the same key again switches
   that axis to the object's **local** basis (its world rotation, the same basis
   `GizmoState::orientation` carries in D20); a third press clears the constraint.
   `Shift`+axis constrains to the **plane** perpendicular to that axis.
5. **Value semantics:** translate is world units along the constraint; rotate is
   degrees about the axis; scale is a dimensionless factor. Readout lives in the
   viewport header and the status bar, live.
6. **Snapping increments** (Ctrl) come from the scene's settings when they land —
   translate 1.0 world unit, rotate 5 degrees, scale 0.1 until then. Increments are
   *scene semantics* (D24 rule), so they are never a viewport setting.
7. **Multi-selection:** translate applies the same delta to every selected object;
   rotate and scale act on the group about the active pivot (`PivotMode`, D22).
   Numeric entry applies the same value to all.
8. **Orbit stays live** during a modal transform (Blender allows viewing from
    another angle mid-gesture), which needs no coordination: the camera gate is
    unchanged and the transform follows the mouse in world space.

### Viewport object context menu (D46, item 201)

**Decision: in scope, but only for an object hit in Object Mode.** Right-clicking
empty viewport space is a no-op; this is not a generic viewport-body menu. On an
object hit, select that object if it is not already selected, then open the context
menu. Its enabled actions are **Rename**, **Duplicate**, and **Delete**; duplicate
and delete use the existing undoable commands. Do not show disabled placeholders
for operations that are not implemented. The Outliner's row context menu is a
separate surface. In Edit Mode, right-click is reserved for the element context
menu scoped by item 117, not this object menu.

**Input precedence prevents the transform-cancel collision.** A running modal
transform (including a gizmo drag) owns right-click first: cancel, restore the
captured value exactly, and do not open a menu. An open shading pie likewise owns
right-click to cancel itself. Only when neither modal owns input may the viewport
dispatch an object context menu; clicking empty space still opens nothing. This
explains a zero-pixel result at a viewport coordinate that does not hit an object,
without treating the context menu as out of scope.

**Implementation residue, checked against the tree for item 201:**
`Viewport.cpp::handleViewportContext` opens only after `pickAt` finds an object,
and `Editor.cpp` suppresses it while the pie, modal transform, or gizmo drag owns
input. The current call is not yet restricted to Object Mode and still displays
disabled Set Origin / Shade Smooth / Shade Flat placeholders. Chisel was asked to
align those two details with this decision. This contract does not claim that
follow-up has landed.

### Mode as state, and the select-mode keys (D35)

**Where the state lives — and it is two places, deliberately.**

| State | Owner | Persisted? |
| ----- | ----- | ---------- |
| `InteractionMode { Object, Edit }` and `SelectMode { Vertex, Edge, Face }` | **editor** (interaction/UI state, like the modal gesture) | **No.** `.ember` never stores it, and a load always starts in **Object** mode — otherwise a file could open in a mode whose target no longer exists. |
| the **edited object's geometry** (`EditableMesh`, D34) | **scene**, on the `Object` | yes, with the object |
| the **edit target** (`ObjectId`) | editor, but it must be a live object: if it is deleted or loses `hasMesh`, the editor **drops back to Object mode** rather than holding a dangling id | no |

The two enums are in **core** (`Types.h`) although only the editor consumes them
today, because `render` will need them for the edit overlays in item 118 — the same
forward-looking placement as `ViewportSettings`/`Light` in D19/D22.

**The key conflict and its resolution.** `platform::Input` exposes both the top-row
digits (`Num0..Num9`) and the physical keypad (`KP_0..KP_9`, `KP_Decimal`). App's
viewport navigation dispatch uses the physical keypad. Top-row `1/2/3` belongs to
Edit Mode's Vertex/Edge/Face selection, and top-row digits do nothing in Object Mode
unless the Preferences `Emulate Numpad` option is enabled.

**The matrix, by physical key:**

| Physical key | Object mode | Edit mode |
| ------------ | ----------- | --------- |
| **Tab** | enter Edit (requires exactly one selected object with geometry; otherwise a no-op with a status-bar hint) | leave Edit, keeping the selection of the object |
| **Ctrl+Tab** | Object/Edit mode-switch pie (Edit is available only for an editable mesh) | same pie; only supported modes are offered |
| **Top-row 1 / 2 / 3** | nothing | select mode **Vertex / Edge / Face** |
| **E** | no operation | extrude the selected face along its normal by the Edit Mesh Tools distance |
| **X / Delete** | delete object selection | delete faces implied by the current sub-element selection |
| **M** | no operation | merge selected vertices at their world-space center |
| **Numpad 1 / 3 / 7** (+Ctrl = opposite) | Front / Right / Top view | same — views work in both modes |
| **Numpad 5** | toggle orthographic | same |
| **Numpad 9** | opposite of the current axis view | same |
| **Numpad .** | frame the selection | same |
| `A` / `Alt+A` | select all / none | select all / none **within the select mode** |
| `G`/`R`/`S` | transform the object | transform the selected sub-elements (item 116, same state machine) |

**The laptop case, decided rather than left to chance.** The persisted preference
**`Emulate Numpad`** (off by default) makes top-row `0/1/3/5/7/9` act as the matching
keypad view commands. When it is ON, top-row `1/2/3` are consumed by the view path,
so select modes remain reachable from the header:

- the **viewport header's mode/select-mode control** — always available, never
  keyboard-only (this is the "a disabled item that names the gap beats a silent one"
  rule applied to input: a mode must be reachable without a numpad).

**Ctrl+Tab correction and implementation (verified against the Blender 5.2 LTS
Manual, 2026-10-18):** Ctrl+Tab opens Blender's **mode-switch pie**, not a
Vertex/Edge/Face select-mode pie. Ember3D's pie offers only supported Object/Edit
modes, opens at the viewport cursor, confirms on held-shortcut release or click/Enter,
supports arrow selection and Escape/right-click cancellation, and leaves mode
unchanged until confirmation. Selection mode remains the header control and top-row
1/2/3 in Edit Mode. Reference:
https://docs.blender.org/manual/en/latest/editors/3dview/modes.html

**Item 117 modeling tools, initial implementation (2026-10-18):** entering Edit
Mode switches the Sidebar to Tool, exposing Extrude Face (with an adjustable
distance), Delete Selected Geometry, and Merge at Center. The same operations are
in Edit > Mesh and on E/X/Delete/M. Each mutation is one snapshot-backed undo step.
Viewport clicks select visible vertices, edges, and faces; Shift-click toggles or
adds. E starts a mouse-driven normal extrusion preview, with exact mouse projection,
snap, Shift precision and Enter/click confirmation; Escape/right-click restores the
captured topology and selection. Disjoint faces can be extruded together. Selected
faces sharing vertices are refused until region extrusion can remove interior walls.

**Ownership of the keys, so two dispatchers never both act on one key:** view keys are
**App's** (it polls `platform::Input`, unchanged from D1/D17); mode and select-mode keys
are the **editor's** (ImGui keys, gated on `!io.WantTextInput`, same gate as the modal
flow). The bridge is a read-only accessor, exactly like `invertOrbit()` in D27:
`bool Editor::emulateNumpad() const`, which App reads to decide whether the top-row
digits are view keys. **The rule: the preference decides which side owns those keys,
and only that side acts on them** — when it is on, the editor must not also treat
`1/2/3` as select modes.

**Wiring:** Platform's `KP_*` key codes are tracked GLFW values. App polls them;
`Editor` consumes top-row `1/2/3` only in Edit Mode and only while Emulate Numpad is
off. The mode selector in the Viewport Header remains available in either setting.

**Initial modeling tools (item 117, implemented 2026-10-18):** entering Edit Mode
switches the Sidebar to its Tool tab, which exposes Extrude Face (with an adjustable
distance), Delete Selected Geometry, and Merge at Center. The same operations are in
the Topbar's `Edit > Mesh` menu and on E/X/Delete/M. Every operation is one
snapshot-backed undo step. The Tool/menu extrusion is a discrete distance action;
keyboard E is modal. Multi-face regions with shared vertices remain unsupported.
Edit Mode clicks pick vertices, edges or faces with Shift-add/toggle; the renderer consumes per-frame
`EditOverlayState` for viewport-local, depth-tested vertices/edges and selected-face
outlines, with X-Ray controlling depth testing.

**Residue — what this decision does not settle:**

- **Mode changes are not undoable**, by design: Tab pushes no `Command`, or a user
  toggling modes would fill the undo stack with entries that restore nothing.
- **Select mode is editor-global, not per object.** Blender remembers it per object;
  ours is one setting, which is simpler and is a documented deviation.
- **Item 118 overlay path landed 2026-10-18:** `App` places the editor-built
  `EditOverlayState` in `RenderPacket`; `render` draws its lines/crosses after the base
  mesh. The same framebuffer-scaled vertex radius and edge width are used for the
  drawing and click tests. Click/Shift-click selects vertices, edges or faces through
  the viewport-local pick path.
- **Keymap changes are editor-local state** and are not stored in scene files; the
  Emulate Numpad preference is persisted in the user's preferences file.
- **Residue:** mode switching is limited to Object/Edit because those are the only
  interaction modes Ember3D implements. Blender's other object/paint/sculpt modes are
  intentionally absent rather than shown as disabled pie entries. Vertex/Edge/Face
  selection has the header and top-row routes above.
- **Modeling residue:** the direct Tool/menu extrusion is discrete; keyboard E is
  modal but shared-vertex face regions are refused rather than creating internal walls.
  Edit Mode box selection and the wider Blender operator set remain.

### Item 118 ownership, and the order 115/116 land (D37)

- **`scene` owns the geometry and the selection.** `EditableMesh` lives on the object
  (D34) and `Object::edit` is scene's, so the editor must not write into it: it goes
  through scene's public API. **`MeshSelectMode` in `src/scene/EditableMesh.h` is a
  DUPLICATE of core's `SelectMode` (D35) and is deleted** — core's is the one render
  needs, and two enums for vertex/edge/face is how a conversion function gets written
  and then forgotten. Scene's edit ops take core's `SelectMode` — **landed 2026-10-17** (`EditableMesh.h:69`,
`Scene.h:235/239/269`, duplicate gone). The sequencing lesson is real and recorded: the rename
landed *while its first consumer was being written*, so it cost eight mechanical call-site edits
in `src/editor`. An interface that is free to move today is not free the moment a consumer
exists; land it before, not beside.
- **`editor` owns the interaction**: entering/leaving Edit mode, the select mode, the
  selection sets, picking, the modal gesture, and building `EditOverlayState`.
- **`render` owns the drawing** (`drawEditOverlay`) and nothing else — it receives
  primitives and flags and has no opinion about meshes.
- **Order:** item 115 may land selection state + picking **without** overlays (the
  cross-module dependency is only the pixel sizes, which are in the state), and overlays
  follow in item 118. Item 116 needs scene's public edit-entry + mutation + undo surface
  — which is scene's shape to define, with the D34 rule that a topology change remaps or
  clears the selection and never leaves it dangling.
- **Current implementation status (2026-10-18):** `EditOverlayState` is populated by
  `Editor::editOverlayState`, attached to the per-frame `RenderPacket` by App, and
  consumed by `Renderer::drawEditOverlay`. GPU pixel coverage proves vertex/edge
  marks and selected-face outlines affect the viewport. Edit Mode click picking and
  the first E/X/M operations are wired through viewport, Sidebar, Topbar and undo.

### Gizmo handle hit-testing: the editor's (reaffirmed from D20)

Unchanged and re-stated because wave 3's gizmo spec rides on it: **`render` draws
handles, `editor` hit-tests them.** `render` may not read input or the scene, and the
editor already owns the interaction, the undo stack and the pick path. Two
refinements from the wave-3 spec:

- **Drag deltas are screen-projected along the axis**, not raw pixel deltas: the
  handle moves the object by the cursor's movement *projected onto that axis's screen
  direction* (and onto the two axes for a plane handle). Raw pixel deltas are why the
  human said moving objects "barely works".
- With areas (D29 §Layout) the hit-test happens in the **3D area's** rect, converting
  window coordinates to framebuffer pixels once at the call site (D16/D17), against the
  same `handleLengthPixels` the renderer draws with.

### 4.6 `core` composition root — `src/core/App.h`

```cpp
namespace ember {

struct AppConfig {
  int width = 1280; int height = 720; std::string title = "Ember3D";
  int frames = 0;      // D14: 0 = run until the window closes; N = stop after N frames
  std::string scenePath;   // D18: load a .ember scene before the loop (via SceneIO)
  bool demoObject = false; // D18: add one cube through World::createPrimitive
};

class App {
public:
  bool init(const AppConfig&);
  void run();          // loop until the window closes
  void shutdown();
};

} // namespace ember
```

`App` is the **only** place the modules are wired together, and it belongs to the
Lead. If another module needs something wired, ask the Lead.

---

## 5. Build layout (how nobody fights over CMake)

- **Build owns** the root `CMakeLists.txt` and `cmake/`. It sets the toolchain,
  warnings, optimisations, `vendor/`, and `bin/Ember3D.exe`.
- **Each module owns its own `src/<module>/CMakeLists.txt`** and is included with
  `add_subdirectory`. You declare your own sources; you do not touch the root.
- **QA owns `tests/CMakeLists.txt`**, included by the root. That is how tests get
  registered without QA touching a file Build owns.

A module CMakeLists looks like this:

```cmake
add_library(ember_render STATIC Renderer.cpp Shader.cpp Mesh.cpp Material.cpp)
target_include_directories(ember_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ember_render PUBLIC ember_core glad)
```

Module targets link **downward only**: `core` links nothing of ours;
`platform`, `render`, `scene` link `core`; `editor` links `core`, `render`,
`scene`; `core/App` links everything. If you think you need an upward link, you
have found an interface that belongs in `core` — ask the Lead.

---

## 6. Milestones

Ordered so no two members are blocked on each other for long. The bracketed name
is who leads; others contribute inside their own paths.

| # | Milestone | Lead | Status, checked 2026-10-16 |
| - | --------- | ---- | -------------------------- |
| M0 | `cmake -B build && cmake --build build` produces `bin/Ember3D.exe`; a window opens and closes cleanly | Platform & Build | **DONE** — independently reproduced |
| M1 | Viewport clears, draws a grid and world axes with an orbit camera | Renderer | **DONE** — pixel evidence in `docs/evidence/m1-first-pixels.png` |
| M2 | Primitives can be created, transformed, parented; `RenderPacket` is produced | Scene & Data | **DONE** — a created cube draws 12 triangles (`--demo-object`) |
| M3 | Outliner + Properties panels drive the world; objects can be selected | Editor | **NEARLY** — panels, menu bar, Properties write-back and viewport picking are wired; selection has not been confirmed by a human end to end |
| M4 | One directional light + a PBR-ish material replaces flat colour | Renderer | **EFFECTIVELY DONE** — light OBJECTS light the scene: every Directional and Point light is evaluated (cap 8, ignored lights logged once per change, documented default sun when empty), the world background reaches the renderer, and the deprecated `packet.light`/`DirectionalLight` pair is **deleted** (D25). Outstanding: point-light pixel evidence |
| M5 | Translate/rotate/scale gizmos; undo/redo | Editor | **Implemented and covered** — undo stack, commands, reversible delete, `GizmoController`, `drawGizmo`, modal `G`/`R`/`S`, pivot modes and projected handle drags pass current CPU/GPU-focused tests. Human gesture-feel verification remains |
| M6 | Save and load a `.ember` scene | Scene & Data | **v8 writer implemented** — image references and editable topology/face-material indices; reads/migrates v1-v7. v8 SceneIO tests are built but blocked by Application Control. |
| M7 | The test suite runs green under CTest and covers each module | QA | **Not fully verified 2026-10-20** — clean alternate Release build; 82 targets register. Compare GPU, path tracer, offline, topology, image writer and box-select suites pass directly. Multiple executables and app smoke were blocked by Application Control; see BUILD-STATUS.md. |

**M0 first.** It landed, and everything after it became verifiable. The order now
matters less than the honesty of the status column above: three of these say
DONE, two say PARTIAL and one says NEARLY, and a reader who needs the real state
should re-run the commands rather than trust this table.

---

## 7. Definition of done

A change is done when **QA has reproduced it**, not when it compiles on your
machine. `share_finding` needs evidence — attach the build output or a test
result:

```
POST /agent-api/v1/findings/<id>/evidence
  { "kind": "command", "command": "cmake --build build --config Release", "exitCode": 0, "output": "…" }
```

A finding marked `verified` without evidence is rejected by the API. That is
deliberate.

### The evidence contract, per item class (D33, frozen)

**What artifact must exist, and what a verifier checks mechanically.** The table below is
the contract; `TEAM.md` §10 is the same thing written for the bench; and
`cmake/evidence/*.evidence` is where an item **declares** it. **The checker has landed
(item 106) and is not a sketch:** `cmake/VerifyEvidence.cmake` reads a `key: value`
declaration (repeatable keys append, `#` comments) and runs **six checks in order**, wired
into ctest as one `evidence.<item>` case per declaration:

| # | Check | What fails it |
| - | ----- | ------------- |
| 1 | `BUILD-FRESHNESS` | no binary in `BIN_DIR` postdates the newest source. **It gates everything** — nothing else about the item is admissible, which is why a stale tree reports one failure and not six. |
| 2 | artifact exists | missing, a directory, or **empty** (0 bytes is the shape of a run that crashed before producing output) |
| 3 | artifact postdates **its declared sources** | `depends:` is required whenever `artifact:` is present (D38), directory entries expand recursively, an empty directory fails as "asserting nothing", and any declared source newer than the artifact fails |
| 4 | the named test exists and passes | absent from `ctest -N`, or a non-zero exit |
| 5 | the symbol **in the named file, in code** | absent, or present only in a **comment** — comments are stripped before searching, because a comment is not a definition (item 107's hole 1); the test file must reference it in code; EDITOR additionally requires a `call_site_file:` that is **not under `tests/`** |
| 6 | the claim names its residue | no `residue:` line, or an empty one |

**Per-class extras, appended after check 6:** RENDER requires the run's
`shader built: <name>` line, a screenshot that exists, is non-empty and postdates sources,
and two named numbers (before/after); DOC requires `doc_must_contain` and honours
`doc_must_not_contain_regex` — a literal search let a **paraphrase** of the stale claim
through (item 107's hole 2), so the stale-claim test may be written as a regex; DECISION
reads the contracts endpoint and requires `decisionId` to equal the finding's id; and a
**declared Blender port** is additionally checked for the four attribution lines
(`SPDX-FileCopyrightText`, `SPDX-License-Identifier`, `Origin: blender/blender …`,
`Modifications:`) — D36, with the same residue: an *undeclared* port is invisible to grep.

**A gate that has never rejected anything is not a gate, and this one has been made to
reject (item 107).** In a scratch copy of the real scripts (SHA256-verified before and
after, so a QA reimplementation cannot pass for the verifier), Verity ran a mutation
matrix: **8 of 8 mutations rejected** once two holes had been found and closed — a symbol
that existed only in a comment passed check 5, and a paraphrased stale claim escaped the
DOC search. Both were fixed and re-proven. Kiln's negative proofs (residue removed →
check 6 fails; symbol renamed → check 5 fails) plus the four rejections he hit while
building the thing are the same evidence from the author's side. **The residue that
remains is what the checks cannot see:** an *undeclared* claim (a human must notice it), a
DECISION whose frozen-ness cannot be proven without authenticated HTTP, the per-target
mixed-state gap, and a DOC paraphrase written outside the declared regex.

**Its own residue, printed by the check rather than left implicit:** it cannot check
that a declaration is *complete* (an undeclared claim needs a human, exactly like an
undeclared port); the DECISION class can only confirm that a finding names its id, not
that it is frozen (CMake cannot send authenticated HTTP); and the per-target freshness
gap — one target stale while others are fresh — stays open until the per-target version
lands. **The output directory is shared by every build tree** (`EMBER_OUTPUT_DIR` defaults
to `<repo>/bin`, overridable per tree), so a *second* build can only ever make this check
pass more readily — a newer `bin/Ember3D.exe` masks staleness (a false PASS). It cannot
manufacture a false red: freshness asks "is any source newer than the newest binary", and
another tree's relink can only push the binary newer, i.e. toward passing. Same failure
direction as the gap above, for the same reason. A green `evidence.*` run means "the declaration matches the tree", not "the tree
is honest":

| Class | Artifact | Mechanical check |
| ----- | -------- | ---------------- |
| **BUILD** | build receipt + freshness | build exit 0, `ctest -R build.freshness` exit 0, `bin/Ember3D.exe` newer than the newest source |
| **SCENE** | symbol in the named file + a unit test calling it | symbol present; CTest case exists and exits 0; test file references the symbol |
| **RENDER** | named shader reported compiled + screenshot + measured property | output contains `shader built: <name>`; screenshot exists, non-empty, postdates sources; two numbers (before/after) named |
| **EDITOR** | interaction test through the real entry point | CTest case exits 0; test references the real symbol; that symbol has a non-test call site |
| **DECISION** | the finding itself | `GET /api/contracts` → `decisionId` equals the finding id |
| **DOC** | corrected text present, stale claim absent | grep for the new sentence succeeds; grep for the old claim finds nothing |

**The two rules that apply to all six**, because they are where the value is: a claim
must **name its residue** (what it does not prove — the gizmo's "hand-wiring unproven"),
and an artifact must **carry the tree state it was produced against** (the stale-ctest
trap generalised: a screenshot older than the newest source is evidence about the
previous tree).

**An artifact declaration must DECLARE its dependencies — the checker must not guess
(D38).** CHECK 3 compares the named artifact's mtime against the newest source *it depends
on*. When a declaration named an `artifact:` with no `depends:`, the checker fell back to
globbing `src/**` **and** `tests/**` — a guess, and specifically a guess *across a module
boundary*: `bin/Ember3D.exe` comes from the root target and does not compile `tests/*.cpp`,
so **editing any test invalidated evidence about the app**. A check that rejects correct
input is worse than no check, because it trains its readers to ignore it. The same comment
claimed the fallback matched `CheckFreshness`'s set "so the two checks cannot disagree",
and that was never true either: `build.freshness` compares the newest source against the
newest binary *anywhere*, while CHECK 3 compares it against *the named artifact* — matching
the glob sets does not make the reference binary the same. **The rule:** `depends:` is
required whenever `artifact:` is present; a directory entry means "everything under it,
recursively" (the author declares the scope, the tool never infers it); and a missing
`depends:` **fails** CHECK 3 saying so rather than falling back to a guess — because a
guessed dependency set is a guessed contract, the error class that has already cost this
project the most. **Residue, which moved rather than vanished:** the tool no longer guesses, so an
**over-broad `depends:`** can put the false positive back — declaring `src/, tests/` for an
app artifact re-opens exactly the hole this closed. Scope honesty is the author's job,
precisely as residue honesty is; the strongest thing the checker can do is refuse an
**empty** directory ("the declaration asserts nothing") rather than treat it as satisfied.

**D38 did not move a module interface, so it was a decision rather than a re-freeze. Item
108 folds the whole trust layer here — D33's contract, D38's rule, and D39 below — and
**re-freezes**, not because a §4 signature moved but because *the thing that decides whether
anybody's "done" is accepted is itself part of the frozen contract*. A document that
describes a verifier it does not have is the header-claims-behaviour mistake
(TEAM.md §8), in the one file everyone trusts.

### Declaration order: three incidents, one rule (D39)

This wave hit the same failure three times, and the third is what makes it a rule instead
of a lesson:

1. **`CollectionId` used before it was declared** in `Scene.h` — nothing compiles until the
   declarations are reordered. **Loud** and cheap: the compiler catches it immediately.
2. **`__has_include` used as a module probe** in `App.cpp` (D15) — CMake cannot see it as a
   dependency, so when `src/editor` appeared **the build stayed green and the editor was
   silently missing**. **Silent** and expensive: a green suite reported an application
   without its editor. Fixed by tracked compile definitions (`EMBER_HAS_*`), which change
   the command line and therefore force the rebuild.
3. **`scaleStrippedRotation` used before its definition** — and then defined twice. The
   linker caught it in *a peer's* build, not the author's (TEAM.md §8's mirror rule).
   **Shared**: the red lands on everyone downstream.

**The rule: nothing may be used before it exists, and the build system must be able to see
every dependency.** In the order the failures bite:

- **In a header, declaration order is read order.** A type or enum appears above its first
  use, or is forward-declared. The compiler enforces this, so the failure is loud and
  local — the good case. Do not "fix" it by moving the declaration into another header to
  silence one error.
- **Never infer a dependency the build system cannot see.** `__has_include`, or any
  compile-time existence probe, is invisible to CMake: the compiler's answer can change
  while no tracked file changes, so the tree stays green and missing the thing. Use a
  **tracked definition** — a compile flag such as `EMBER_HAS_*` — because a flag changes the
  command line and forces the rebuild. **This is also why `build.freshness` exists:** a
  probe hidden inside a green suite is precisely the failure it was built to make visible.
- **Before adding a definition, grep the whole module for the symbol** —
  `Select-String -Path src/<module> -Pattern '<symbol>'`. A duplicate or missing definition
  was caught by someone else's build three times in one wave; one grep turns "I am adding a
  function" into "I am adding a **second** one".
