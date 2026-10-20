// Ember3D — the shared vocabulary.
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §4.1.
//
// core is the ONLY module allowed to define a type used by two modules. If you
// need a new field here, that is a message to Ash — never an edit. Every module
// includes this header; no module includes another module's header.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "core/Math.h"
#include "core/Theme.h"

namespace ember {

// --- fixed-width aliases ---------------------------------------------------

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

// --- identity --------------------------------------------------------------

// 0 is never a valid id, so a default-constructed ObjectId means "none".
struct ObjectId {
  u32 value = 0;
  constexpr bool valid() const { return value != 0; }
  constexpr explicit operator bool() const { return valid(); }
};

inline constexpr bool operator==(ObjectId a, ObjectId b) { return a.value == b.value; }
inline constexpr bool operator!=(ObjectId a, ObjectId b) { return a.value != b.value; }
inline constexpr bool operator<(ObjectId a, ObjectId b)  { return a.value < b.value; }

// Generational handle for slot pools (mesh/shader caches). `index` 0 is the
// null slot; `generation` catches stale handles after a slot is recycled.
template <typename T>
struct Handle {
  u32 index = 0;
  u32 generation = 0;
  constexpr bool valid() const { return index != 0; }
  constexpr explicit operator bool() const { return valid(); }
};

template <typename T>
inline constexpr bool operator==(Handle<T> a, Handle<T> b) {
  return a.index == b.index && a.generation == b.generation;
}
template <typename T>
inline constexpr bool operator!=(Handle<T> a, Handle<T> b) { return !(a == b); }

// --- rectangles (D29) ------------------------------------------------------

// A screen rectangle in FRAMEBUFFER PIXELS, origin TOP-LEFT (the space of §4.2's
// framebuffer sizes and of screenToRay). Used by `editor` (which produces the 3D
// viewport area's rect) and by `render` (which scopes its 3D pass to it), so §4.1
// puts it in core rather than in either module. An invalid rect (w or h <= 0)
// means "not laid out yet" and all consumers must treat it as "draw nothing".
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool valid() const { return w > 0 && h > 0; }
};

// --- transform -------------------------------------------------------------

// Vec3 / Mat4 / Color come from core/Math.h (§4.1: "defined in Math.h").
struct Transform {
  Vec3 position{0.0f, 0.0f, 0.0f};
  Vec3 rotation{0.0f, 0.0f, 0.0f};   // euler radians, XYZ order
  Vec3 scale{1.0f, 1.0f, 1.0f};

  // T * R * S, column-major. Non-uniform scale is applied before rotation,
  // which is what the editor gizmos and the scene graph both expect.
  Mat4 toMatrix() const {
    return translation(position) * rotationEuler(rotation) * scaling(scale);
  }
};

inline constexpr bool operator==(const Transform& a, const Transform& b) {
  return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}
inline constexpr bool operator!=(const Transform& a, const Transform& b) { return !(a == b); }

// --- geometry --------------------------------------------------------------

// What the scene produces and the renderer uploads: interleaved-free parallel
// arrays, so either side can build whatever layout it wants without a copy step
// being baked into the contract.
struct MeshData {
  std::vector<float> positions;   // xyz
  std::vector<float> normals;     // xyz
  std::vector<float> uvs;         // uv
  std::vector<u32>   indices;
  std::string        name;

  u32 vertexCount() const { return static_cast<u32>(positions.size() / 3); }
  u32 indexCount()  const { return static_cast<u32>(indices.size()); }
  bool empty()      const { return indices.empty(); }
};

// --- material --------------------------------------------------------------

// An authored image reference is deliberately just a path and an interpretation
// tag. The scene owns path resolution; render backends own decoding/caching.
// Empty paths mean that no image is assigned. Relative paths are resolved
// against RenderPacket::assetBaseDirectory; absolute paths are used as-is.
enum class ImageColorSpace : u8 { SRgb, Linear };

struct ImageTextureRef {
  std::string path;
  ImageColorSpace colorSpace = ImageColorSpace::SRgb;
};

inline constexpr bool operator==(const ImageTextureRef& a,
                                 const ImageTextureRef& b) {
  return a.path == b.path && a.colorSpace == b.colorSpace;
}
inline constexpr bool operator!=(const ImageTextureRef& a,
                                 const ImageTextureRef& b) {
  return !(a == b);
}

// Material contract (original base/emission fields: decision D12, 2026-10-16).
// Invariants, so nobody has to guess:
//   * baseColor is straight (non-premultiplied) LINEAR RGBA; `a` is the ONLY
//     transparency channel. The renderer already writes uBaseColor.a to the
//     fragment alpha.
//   * metallic and roughness are nominally [0,1]. The renderer clamps roughness
//     to >= 0.045 to keep specular energy finite; it does not renormalise metallic.
// Emission is authored in linear RGB. Its components and strength are
// non-negative; values above one are valid HDR values. Alpha is not used by
// emission and defaults to opaque for the shared Color representation.
// Transmission is a [0,1] dielectric weight. IOR is finite and bounded to
// [1,3]; malformed IOR values use the project default of 1.45.
struct MaterialDesc {
  Color baseColor{0.8f, 0.8f, 0.8f, 1.0f};
  float metallic  = 0.0f;
  float roughness = 0.5f;
  Color emissionColor{0.0f, 0.0f, 0.0f, 1.0f};
  float emissionStrength = 0.0f;
  float transmission = 0.0f;
  float ior = 1.45f;
  ImageTextureRef baseColorTexture;
};

inline constexpr float kDefaultMaterialIor = 1.45f;

// Shared sanitizer for serialized and renderer-facing material values. Emission
// malformed/negative channels become zero, while finite HDR values are kept.
inline float sanitizeEmissionComponent(float value) {
  return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

inline MaterialDesc sanitizedMaterialDesc(MaterialDesc material) {
  // Preserve finite authored base-colour values (including values above 1),
  // but prevent malformed values from escaping into a file or renderer.
  if (!std::isfinite(material.baseColor.r)) material.baseColor.r = 0.8f;
  if (!std::isfinite(material.baseColor.g)) material.baseColor.g = 0.8f;
  if (!std::isfinite(material.baseColor.b)) material.baseColor.b = 0.8f;
  if (!std::isfinite(material.baseColor.a)) material.baseColor.a = 1.0f;
  if (!std::isfinite(material.metallic)) material.metallic = 0.0f;
  else material.metallic = std::clamp(material.metallic, 0.0f, 1.0f);
  if (!std::isfinite(material.roughness)) material.roughness = 0.5f;
  else material.roughness = std::clamp(material.roughness, 0.0f, 1.0f);
  if (!std::isfinite(material.transmission)) material.transmission = 0.0f;
  else material.transmission = std::clamp(material.transmission, 0.0f, 1.0f);
  if (!std::isfinite(material.ior) || material.ior < 1.0f || material.ior > 3.0f)
    material.ior = kDefaultMaterialIor;
  material.emissionColor.r = sanitizeEmissionComponent(material.emissionColor.r);
  material.emissionColor.g = sanitizeEmissionComponent(material.emissionColor.g);
  material.emissionColor.b = sanitizeEmissionComponent(material.emissionColor.b);
  material.emissionColor.a = 1.0f;
  material.emissionStrength = sanitizeEmissionComponent(material.emissionStrength);
  return material;
}

inline constexpr bool operator==(const MaterialDesc& a, const MaterialDesc& b) {
  return a.baseColor == b.baseColor && a.metallic == b.metallic &&
         a.baseColorTexture == b.baseColorTexture &&
         a.roughness == b.roughness && a.emissionColor == b.emissionColor &&
         a.emissionStrength == b.emissionStrength &&
         a.transmission == b.transmission && a.ior == b.ior;
}

// One material slot on an object (D22, wave 2). Blender's per-object slot list:
// slot 0 is the object's base material, and a mesh can later assign faces to any
// slot. The array lives on the scene object (which owns it); DrawItem points into
// it non-owning, exactly like `mesh` (D8/D9).
struct MaterialSlot {
  std::string  name;
  MaterialDesc material;
};

// --- lighting --------------------------------------------------------------

// `DirectionalLight` (the pre-D19 single world light) is DELETED as of D25: the
// deprecation condition was "delete it in the same edit that stops reading it",
// and `render` now reads `lights` exclusively while `scene` no longer fills it.
// If history matters, D19's finding has the staging. Use `LightInstance`.

enum class LightType : u8 { Directional, Point };

// The AUTHORED light component, stored on a scene object (D19). Position and
// orientation are NOT duplicated here — they come from the object's Transform,
// exactly as in Blender: a directional light shines along its object's local -Z,
// a point light sits at its object's position.
struct Light {
  LightType type      = LightType::Directional;
  Color     color{1.0f, 1.0f, 1.0f, 1.0f};
  float     intensity = 1.0f;
  float     radius    = 1.0f;   // Point only: falloff radius (world units)
};

// Camera component vocabulary for scene objects (D47). The pose remains the
// object's Transform; these fields describe projection only. `isCamera` marks
// the object as a camera component, while validation and active-camera
// ownership remain scene responsibilities.
struct CameraSettings {
  bool  isCamera     = false;
  float fovY         = 0.78539816339f;  // vertical field of view, radians (45 degrees)
  float clipNear     = 0.1f;
  float clipFar      = 1000.0f;
  bool  orthographic = false;
  float orthoScale   = 10.0f;            // vertical world-space span
};

// The RESOLVED light, world space, as it reaches the renderer in the packet.
// `scene` does the transform resolution; the renderer never sees a Transform.
struct LightInstance {
  LightType type = LightType::Directional;
  Vec3      position{0.0f, 0.0f, 0.0f};    // Point: the light's world position
  Vec3      direction{0.0f, -1.0f, 0.0f};  // Directional: the way rays travel
  Color     color{1.0f, 1.0f, 1.0f, 1.0f};
  float     intensity = 1.0f;
  float     radius    = 1.0f;
};

// --- world + shadows (D22, wave 2) -----------------------------------------

// The scene's world background (Blender's World). Scene state, persisted; `scene`
// fills RenderPacket::background and the renderer clears with
// `color * strength`. `lighting` is the "environment lighting" switch. The image
// reference is shared input data; backend decode/lighting behavior is independent.
struct WorldBackground {
  Color color{0.05f, 0.05f, 0.06f, 1.0f};
  float strength  = 1.0f;
  bool  lighting  = false;
  ImageTextureRef environmentTexture;
};

// Scene-level render-engine selection and path-tracing quality controls.
// The engine is consumed only for Rendered viewport shading; other shading
// modes remain the raster renderer's responsibility.
enum class RenderEngine : u8 { Raster, Cycles };

struct SceneRenderSettings {
  RenderEngine engine = RenderEngine::Raster;
  u32 samplesPerPixel = 64;
  u32 maxBounces = 4;
  u32 seed = 1;
};

inline constexpr bool operator==(const SceneRenderSettings& a,
                                 const SceneRenderSettings& b) {
  return a.engine == b.engine && a.samplesPerPixel == b.samplesPerPixel &&
         a.maxBounces == b.maxBounces && a.seed == b.seed;
}
inline constexpr bool operator!=(const SceneRenderSettings& a,
                                 const SceneRenderSettings& b) {
  return !(a == b);
}

inline constexpr u32 kMinRenderSamplesPerPixel = 1;
inline constexpr u32 kMaxRenderSamplesPerPixel = 1u << 24;
inline constexpr u32 kMaxRenderBounces = 64;

// Keep setter, serializer, and IO materialization policy identical. Unknown
// enum values fall back to the safe existing raster backend.
inline SceneRenderSettings sanitizedSceneRenderSettings(
    SceneRenderSettings settings) {
  if (settings.engine != RenderEngine::Raster &&
      settings.engine != RenderEngine::Cycles)
    settings.engine = RenderEngine::Raster;
  settings.samplesPerPixel = std::clamp(
      settings.samplesPerPixel, kMinRenderSamplesPerPixel,
      kMaxRenderSamplesPerPixel);
  settings.maxBounces = std::min(settings.maxBounces, kMaxRenderBounces);
  return settings;
}

// What the editor is editing (D35). In core because `render` will need it too for
// the edit-mode overlays (item 118): the editor is the only consumer today.
enum class InteractionMode : u8 { Object, Edit };

// Sub-element select mode, Blender's 1/2/3 (D35). Editor state, not per object.
enum class SelectMode : u8 { Vertex, Edge, Face };

// Shadow configuration. User-facing, so it lives with the other viewport state
// rather than in the packet: `render` reads it from Renderer::viewportSettings().
// The packet carries only WHICH light casts (RenderPacket::shadowLightIndex).
struct ShadowSettings {
  bool  enabled     = false;
  int   resolution  = 1024;      // square shadow map, per casting light (one today)
  float bias        = 0.002f;
  float normalBias  = 0.02f;     // peter-panning vs acne; the one users tune
  float maxDistance = 50.0f;     // world units the casting light's frustum covers
};

// --- viewport settings (D19, extended D22) ---------------------------------

// What the gizmo orbits/scales about (D22). The EDITOR resolves the mode into
// GizmoState::origin, so render never needs this enum.
enum class PivotMode : u8 {
  MedianPoint,        // default: median of the selection
  Cursor3D,           // the scene's 3D cursor
  IndividualOrigins,  // each selected object about its own origin
  BoundingBoxCenter,  // centre of the selection's AABB
  ActiveElement,      // the active (last selected) object's origin
};

// How the viewport is shaded. Blender's four, minus their edit-mode extras.
enum class ShadingMode : u8 { Wireframe, Solid, Material, Rendered };

// Per-viewport presentation state. Owned by the editor's UI, consumed by the
// renderer (which reads it from Renderer::viewportSettings()); a toggle takes
// effect on the NEXT frame, which is what immediate-mode UI does everywhere.
//
// ONE definition, in core (D21). `src/editor/ViewportSettings.h` held a second
// copy with the same name — two structs for one concept, and the editor's copy
// could never reach the renderer, so the toggles in the Viewport menu did
// nothing. That header is deleted; the editor uses this one and passes it to
// Renderer::setViewportSettings().
struct ViewportSettings {
  ShadingMode shading = ShadingMode::Solid;
  PivotMode   pivot   = PivotMode::MedianPoint;   // D22: gizmo pivot mode
  ShadowSettings shadows;                          // D22: shadow pass config
  // Rendered-shading presentation only. `compareSplit` is the normalized
  // left-side width (0..1); the editor's draggable divider writes it from the
  // viewport-local horizontal position. Render clamps it to [0.05, 0.95].
  // These are viewport UI preferences, never scene/render settings.
  bool  compareRasterCycles = false;
  float compareSplit        = 0.5f;
  bool showGrid            = true;
  // D24 (item 53): how many MINOR grid lines sit between two major lines, i.e.
  // the grid's density. Major lines are 1 world unit apart; minor spacing is
  // 1.0 / gridSubdivisions. A DISPLAY preference, which is why it is here and not
  // with the scene's unit settings: units/scale/snap are scene semantics (Slate),
  // grid density is how the viewport draws. Consumers must clamp to [1, 100] —
  // zero here is a divide-by-zero in the middle of the overlay pass.
  int  gridSubdivisions    = 10;   // Blender's default
  bool showAxes            = true;
  bool showGizmo           = true;
  bool showOrigins         = false;   // empty/light object markers (M3 outliner parity)
  bool showStats           = true;
  bool showSelectionOutline = true;

  // D37 (item 118): edit-mode overlay toggles. Counts are NOT a field — the header
  // derives them from the mesh, because only the editor knows the selection.
  bool showEditVertices = true;
  bool showEditEdges    = true;
  bool showEditFaces    = true;    // face HIGHLIGHT only; the surface is always drawn
  bool xray             = false;   // draw overlays through geometry (depth test off)
};

// --- gizmo (D19) -----------------------------------------------------------

// What the transform gizmo needs to draw for one frame. Produced by the editor
// (which owns the interaction) and consumed by render (which owns the geometry),
// so the type lives in core. `handleLengthPixels` is ONE number used by both
// sides — the renderer draws handles that long, the editor hit-tests those same
// pixels — so the drawn handle and the clickable handle cannot drift apart.
struct GizmoState {
  enum class Mode : u8 { Translate, Rotate, Scale };

  Mode  mode = Mode::Translate;
  Vec3  origin{0.0f, 0.0f, 0.0f};   // world space: the selection's pivot
                                    // (median of the selection; a lone object's
                                    // world position when only one is selected)

  // The basis the handles live in (D20). Global = identity; Local = the selected
  // object's world ROTATION with translation and scale stripped. The editor
  // resolves it and BOTH sides use it — render draws the handles in this basis,
  // the editor hit-tests against the same basis — so a local-space handle cannot
  // be drawn one way and clicked another.
  //
  // It is a matrix and not a "Global/Local" enum on purpose: render has no scene
  // access (links go downward only), so handed only an enum it could not compute
  // the local axes. The editor keeps its own space enum for the UI.
  Mat4  orientation{};              // Mat4's default is identity

  float handleLengthPixels = 90.0f;
  int   hoveredAxis = -1;           // 0=X 1=Y 2=Z, -1 = none; editor sets it
  int   activeAxis  = -1;           // the axis being dragged, or -1
  bool  snap        = false;        // Ctrl: 1 unit / 15 degrees
};


// --- edit-mode overlay payload (D37, item 118) -----------------------------

inline constexpr float kEditVertexRadiusPixels = 5.0f;
inline constexpr float kEditEdgeWidthPixels = 6.0f;

// What `render` needs to draw edit-mode overlays WITHOUT knowing `EditableMesh`.
// The EDITOR extracts render-ready primitives from the editable topology and passes
// selection as parallel flags, so the renderer only ever draws points, lines and
// triangles — the same reason it never walks the scene graph. Non-owning per-frame
// data, with the D9 lifetime rule: valid until the next World/editor mutation.
struct EditOverlayState {
  const MeshData* mesh = nullptr;   // non-owning: the object's render form (positions live here)
  Mat4            model;            // object -> world, so overlays sit on the surface

  // Built by the editor from the editable mesh, in OBJECT-LOCAL space:
  std::vector<Vec3> vertexPoints;   // one per vertex
  std::vector<Vec3> edgeLines;      // 2 per edge (pairs)
  std::vector<Vec3> faceTriangles;  // 3 per triangulated face corner
  std::vector<u32>  faceOfTri;      // one per triangle: which FACE it belongs to (for highlight)

  // Selection, parallel to the arrays above: 1 = selected. Indices are the editor's.
  std::vector<u8> vertexSelected;
  std::vector<u8> edgeSelected;
  std::vector<u8> faceSelected;

  InteractionMode mode       = InteractionMode::Object;
  SelectMode      selectMode = SelectMode::Vertex;
  int activeVertex = -1, activeEdge = -1, activeFace = -1;   // index into the arrays; -1 = none

  // Screen-space sizes: the renderer draws with these AND the editor hit-tests with
  // them, so the dot you see and the dot you click cannot drift (the D20 rule).
  float vertexRadiusPixels = kEditVertexRadiusPixels;
  float edgeWidthPixels    = kEditEdgeWidthPixels;

  // Theme, resolved by the EDITOR so `render` hardcodes no palette. Blender's edit
  // selection is orange, not blue.
  Color selectedColor{1.0f, 0.6f, 0.15f, 1.0f};
  Color unselectedColor{0.05f, 0.05f, 0.05f, 1.0f};
  Color activeColor{1.0f, 1.0f, 1.0f, 1.0f};

  bool valid() const { return mesh != nullptr && mode == InteractionMode::Edit; }
};

// --- modifiers (D40, item 120): ordered, per-object, NON-DESTRUCTIVE ---------
//
// The SCENE evaluates a stack into a MeshData; the renderer consumes that
// MeshData and never sees a Modifier — no ordering rules, no parameter types, no
// knowledge that modifiers exist. That is the point of the design: evaluation
// stops at the packet boundary, so `render` gained nothing to maintain.
//
// The kind is DERIVED from the variant index, never stored beside it, so a stack
// entry cannot claim to be a Mirror while holding Array parameters — the
// duplicate-representation class D37 had to delete from SelectMode.

struct SubdivisionParams {
  int  levels        = 1;        // 0..6; 0 is a no-op
  bool smoothNormals = true;
};
struct MirrorParams {
  bool  axisX = true, axisY = false, axisZ = false;
  float mergeThreshold = 0.001f; // weld a mirrored vertex closer than this to its
                                 // twin; 0 = never weld
};
struct ArrayParams {
  int   count    = 2;            // total copies, including the original
  float offsetX  = 1.0f;         // per-copy displacement, object-local
  float offsetY  = 0.0f;
  float offsetZ  = 0.0f;
  bool  relative = true;         // offset scales with the object's bounds
};

using ModifierParams = std::variant<SubdivisionParams, MirrorParams, ArrayParams>;
enum class ModifierKind : u8 { Subdivision = 0, Mirror = 1, Array = 2 };

static_assert(std::variant_size_v<ModifierParams> == 3,
              "ModifierKind and ModifierParams must stay in lockstep");
static_assert(std::is_same_v<std::variant_alternative_t<0, ModifierParams>, SubdivisionParams>);
static_assert(std::is_same_v<std::variant_alternative_t<1, ModifierParams>, MirrorParams>);
static_assert(std::is_same_v<std::variant_alternative_t<2, ModifierParams>, ArrayParams>);

struct Modifier {
  ModifierParams params;         // the variant index IS the kind
  bool           enabled = true;
  ModifierKind kind() const { return static_cast<ModifierKind>(params.index()); }
};

// HOW A MODIFIER DECLARES ITS PARAMETERS (D40). The values live in the typed
// structs above; this descriptor names them for the editor's controls and for
// save/load, so neither consumer switches on the kind (no per-kind UI or
// serializer branch to forget). `key` is stable and is what gets serialized;
// `label` is display only.
//
// RESIDUE, stated because it is real: the compiler cannot check that this table
// matches the struct's fields. Item 123 must round-trip every declared key
// through save/load so a renamed field or a forgotten entry is caught by a test
// rather than by a user.
enum class ParamType : u8 { Float, Int, Bool };
struct ParamDesc {
  const char* key;               // stable save/load key — never a UI label
  const char* label;             // shown in the Modifiers panel
  ParamType   type;
  float       minimum, maximum, step;   // UI range; for Bool only 0/1 matters
};
struct ModifierDesc {
  const char*      name;         // "Subdivision"
  ModifierKind     kind;
  const ParamDesc* params;
  u8               paramCount;
};

inline ModifierDesc modifierDesc(ModifierKind kind) {
  static constexpr ParamDesc kSubdivision[] = {
    {"levels",         "Levels",         ParamType::Int,  0.0f, 6.0f,  1.0f},
    {"smooth_normals", "Smooth Normals", ParamType::Bool, 0.0f, 1.0f,  1.0f},
  };
  static constexpr ParamDesc kMirror[] = {
    {"axis_x",          "X",               ParamType::Bool,  0.0f, 1.0f,   1.0f},
    {"axis_y",          "Y",               ParamType::Bool,  0.0f, 1.0f,   1.0f},
    {"axis_z",          "Z",               ParamType::Bool,  0.0f, 1.0f,   1.0f},
    {"merge_threshold", "Merge Threshold", ParamType::Float, 0.0f, 0.1f,   0.0005f},
  };
  static constexpr ParamDesc kArray[] = {
    {"count",    "Count",    ParamType::Int,   1.0f,  64.0f, 1.0f},
    {"offset_x", "Offset X", ParamType::Float, -10.0f, 10.0f, 0.1f},
    {"offset_y", "Offset Y", ParamType::Float, -10.0f, 10.0f, 0.1f},
    {"offset_z", "Offset Z", ParamType::Float, -10.0f, 10.0f, 0.1f},
    {"relative", "Relative", ParamType::Bool,  0.0f,  1.0f,  1.0f},
  };
  switch (kind) {
    case ModifierKind::Subdivision: return {"Subdivision", kind, kSubdivision, 2};
    case ModifierKind::Mirror:      return {"Mirror",      kind, kMirror,      4};
    case ModifierKind::Array:       return {"Array",       kind, kArray,       5};
  }
  return {"Subdivision", ModifierKind::Subdivision, kSubdivision, 2};
}

// --- per-frame draw list ---------------------------------------------------

// One thing to draw this frame. `mesh` is non-owning: the scene owns the
// MeshData, and it stays alive for the whole frame.
struct DrawItem {
  ObjectId     object;
  Mat4         model;
  MeshData*    mesh = nullptr;        // non-owning
  // Scene-built packets carry mutation stamps to avoid hashing large meshes
  // in per-frame consumers. Hand-built packets leave the flag false.
  u32          geometryVersion = 0;
  u32          modifierVersion = 0;
  bool         hasGeometryVersion = false;
  MaterialDesc material;
  bool         selected  = false;
  bool         wireframe = false;

  // Wave 2 (D22): per-slot materials. Non-owning, into the object's slot array,
  // valid for the frame exactly like `mesh` (D9). materialCount == 0 means
  // "single material" and `material` is used; when it is > 0 the slot array is
  // authoritative for slot 0 too, and `material` is DEPRECATED and deleted in the
  // same edit that stops reading it (the DirectionalLight staging, D19).
  const MaterialSlot* materials = nullptr;
  u32          materialCount = 0;

  // D45: one non-owning slot index per emitted index-triangle, in mesh index
  // order. The mapping is valid for the frame like `mesh` and `materials`.
  const u32* triangleMaterialSlots = nullptr;
  u32       triangleMaterialSlotCount = 0;

  // Wave 2 (D22): per-object shadow participation (Blender's object visibility
  // switches). The renderer honours these only when the shadow pass is enabled
  // and this object's light is the caster.
  bool castShadow    = true;
  bool receiveShadow = true;
};

// The scene fills this; the renderer consumes it. The renderer never walks the
// scene graph, which is what keeps render from depending on scene.
//
// view / projection / cameraPosition are filled by App from render::OrbitCamera
// or World::activeCamera() while look-through is active (see ARCHITECTURE.md
// §4.4): App is the only place that sees both render and scene.
struct RenderPacket {
  std::vector<DrawItem> items;
  // Per-frame Edit Mode primitives are extracted by the editor and consumed by
  // render without exposing EditableMesh across the module boundary.
  EditOverlayState editOverlay;
  Vec3  cameraPosition{0.0f, 0.0f, 0.0f};
  Mat4  view, projection;
  Color clearColor{0.12f, 0.12f, 0.13f, 1.0f};

  // D19: every light in the world, resolved to world space. `scene` fills it.
  // The renderer evaluates what it supports (one directional today) and MUST
  // report any light it ignores rather than dropping it silently.
  std::vector<LightInstance> lights;

  // `packet.light` (D19) was DELETED in D25 once `render` read `lights`
  // exclusively and `scene` stopped filling it. `lights` is the single source,
  // and its ORDER is part of the contract: a light occupies its creation-order
  // slot even when it is unmeshed or hidden.

  // Wave 2 (D22). `background` is the scene's world background: the renderer
  // clears with it, and `clearColor` above is DEPRECATED in its favour (deleted in
  // the same edit that stops reading it). `shadowLightIndex` is the index into
  // `lights` of the ONE light that casts this frame, or -1 for none — `render`
  // builds the light-space matrix itself, because it owns the shadow map.
  WorldBackground background;
  // Absolute directory of the owning scene (or the process working directory
  // for a new unsaved World). Consumers resolve relative image references here.
  std::string assetBaseDirectory;
  int             shadowLightIndex = -1;

  // Value snapshot of the scene-level render policy (never editor state).
  SceneRenderSettings renderSettings;

  int   width = 0, height = 0;
};

// The renderer reports these honestly; the editor displays them and QA checks them.
struct RenderStats {
  int    drawCalls = 0;
  int    triangles = 0;
  double frameMs   = 0.0;
};

} // namespace ember
