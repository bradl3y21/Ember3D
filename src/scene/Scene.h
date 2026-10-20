// Scene.h — World: object storage, hierarchy, selection, RenderPacket.
// Owned by Slate · Scene. Contract: ARCHITECTURE.md §4.4 (+ D8: Object is
// scene-owned, declared here, never in core).
//
// The renderer never walks the scene graph: it consumes the RenderPacket this
// World produces. The editor mutates the world exclusively through this
// class's public API.
#pragma once

#include "core/Types.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene/EditableMesh.h"

namespace ember::scene {

enum class Primitive { Cube, Sphere, Plane, Cylinder, Torus };

// ---- D22: collections — a named grouping tree, independent of parenting ----
// Collections never transform anything. Membership is ONE CollectionId per
// Object (invalid membership = the implicit root collection, so no root
// collection record is needed for scenes that never use collections).
struct CollectionId {
  u32 value = 0;
  bool valid() const { return value != 0; }
};
inline bool operator==(CollectionId a, CollectionId b) {
  return a.value == b.value;
}
inline bool operator!=(CollectionId a, CollectionId b) {
  return a.value != b.value;
}

// EXCLUSION (D22 rule 3, verbatim): an object emits a DrawItem iff
//     object.visible && (!object.collection.valid() || collection.visible)
// the same rule the outliner's eye icons show, so the panel and the frame
// cannot disagree. NOTE (posted to the Lead): the decision text tests only
// the member's OWN collection flag, not those of ancestors — ancestor
// visibility propagation is NOT in the decision and stays out until decided.
struct Collection {
  CollectionId id;
  CollectionId parent;             // invalid => a root collection
  std::string  name;               // uniquified across collections
  bool         visible = true;
};

// Per D8: Scene owns Object; core never defines it. Editor code includes this
// header (editor links scene, so that is legal).
//
// Field notes:
//   * id == the key the object is stored under; parent invalid (value 0) ==
//     a root object. children lists are the inverse edges, kept consistent by
//     setParent/reparentKeepWorld/destroy; they are never derived.
//   * MeshData mesh is owned BY the object (by-value copy, D8). DrawItem::mesh
//     points INTO it and is valid until the next World mutation (D9) — consume
//     the packet you built before mutating the world.
//   * MeshData::name mirrors the *kind* ("Cube", "Sphere"...), not the object
//     name; the object name lives above.
//   * Material slots (D22(c)): slot 0 IS the base material — the legacy
//     `material` field keeps reading it until the staging deletion lands.
//     Every object has exactly one implicit default slot.
struct Object {
  ObjectId     id;
  std::string  name;
  ObjectId     parent;                       // invalid => root
  std::vector<ObjectId> children;
  Transform    transform;
  MaterialDesc material;
  MeshData     mesh;                         // owned by value (D8)
  Primitive    primitive = Primitive::Cube;  // meaningful when hasMesh
  bool         hasMesh   = false;
  bool         visible   = true;             // invisible => no DrawItem

  // D19: lights are scene objects. isLight true marks a light object; aim and
  // position come from Transform (local -Z for directional, origin for point)
  // exactly as in Blender — no direction/position duplicated in the
  // component. Unmeshed by construction, hence never picked (D16/D22).
  bool         isLight   = false;
  Light        light;                        // authored component
  CameraSettings camera;                     // authored projection; pose is transform

  // D22: collection membership (invalid = implicit root collection).
  CollectionId collection;
  // D22(c): ordered material slots (slot 0 mirrors `material`).
  std::vector<ember::MaterialSlot> materials;

  // D34: the optional editable mesh (source of truth once edited; `mesh`
  // regenerates from it on mutation). Adds `hasEdit` to make "optional"
  // representable — additive, flagged to the Lead with the normals note.
  bool          hasEdit = false;
  EditableMesh  edit;

  // D40 (item 120, frozen; landed by item 121): the modifier STACK — ordered,
  // per-object, NON-DESTRUCTIVE. Scene evaluates it into a per-frame MeshData
  // (owned by World's evaluated-mesh cache, alive for the frame); `mesh` and
  // `edit` are never touched by evaluation. Version stamps drive the cache
  // (D40): `modifierVersion` is bumped by ANY stack mutation
  // (add/remove/reorder/enable/param), `meshVersion` by ANY code path that
  // replaces `mesh` — both are contractual (a missed bump serves stale
  // geometry, hence item 123's test obligation).
  std::vector<ember::Modifier> modifiers;
  u32 modifierVersion = 0;
  u32 meshVersion     = 0;
};

// Reversible-delete support for the editor's undo stack (M5, Chisel-approved
// design). The editor's sequence is always:
//
//   captureSnapshot(id) -> destroy(id) -> (on undo) reinsert(snapshot)
//
// The snapshot captures one object PLUS its full descendant subtree with the
// ORIGINAL ids and the original parent links (the subtree root keeps its
// world-parent link; descendants keep their subtree-internal links). Meshes
// are copied by value, so the snapshot is fully inert against later world
// edits and reinsert() restores the exact same geometry bytes.
//
// reinsert() semantics: only the ROOT id must be free (it is, after
// destroy()). Live descendants — destroy() re-parents survivors — are
// ADOPTED and reset to their captured state; this is safe under the editor's
// LIFO undo, where only the delete itself touched them. The subtree root
// silently detaches to a world root when its captured world parent no longer
// exists.
struct ObjectSnapshot {
  ObjectId activeCameraBefore;  // active camera captured for delete undo
  struct Node {
    ObjectId     id;
    std::string  name;
    ObjectId     parent;    // subtree-internal; ROOT node = world-parent link
    Transform    transform;
    MaterialDesc material;
    MeshData     mesh;      // by value: exact copy of the captured mesh
    Primitive    primitive = Primitive::Cube;
    bool         hasMesh  = false;
    bool         visible  = true;

    // D19: light objects survive snapshots (undo of Add/Delete Sun/Point).
    bool         isLight  = false;
    Light        light;     // authored component
    CameraSettings camera;

    // D22: the membership the object carried at capture time.
    CollectionId collection;
    // D22(c): material slots, restored bitwise on reinsert.
    std::vector<ember::MaterialSlot> materials;

    // D34 (item 111): the edited mesh survives snapshots, so op reversibility
    // goes through the same capture/reinsert pair as everything else.
    bool hasEdit = false;
    EditableMesh edit;

    // D40 (item 121): the modifier stack survives snapshots, so undo of
    // object deletion also restores undoable-per-modifier state (item 122).
    // The versions ride along to keep the evaluated-mesh cache key honest.
    std::vector<ember::Modifier> modifiers;
    u32 modifierVersion = 0;
    u32 meshVersion     = 0;
  };

  // Depth-first; nodes[0] is always the subtree root.
  std::vector<Node> nodes;
};

// Viewport picking — D16, frozen contract (ARCHITECTURE.md §4.4).
// Closest hit of a world-space ray against the world, or nullopt for a miss.
//
//  * ray.direction must be NORMALIZED, world-space (render builds it).
//    `distance` is the hit's t along the ray, in WORLD units, so callers can
//    order hits and reason about distances.
//  * The mesh is never transformed: the RAY is mapped into each object's
//    local space with inverse(worldMatrix(id)). With that exact mapping the
//    local hit parameter equals the world ray length (M·d' == d), so distance
//    stays in world units under scaled/rotated parents.
//  * Objects that are invisible, collection-hidden (D22 rule 3) or unmeshed
//    are skipped. Lights are therefore never picked — that is CORRECT (D19);
//    lights are selected through the outliner.
//  * Exact ties — a ray along an edge shared by two triangles: FIRST hit in
//    iteration order wins. Iteration order: objects in objects() order
//    (creation order), triangles in mesh index order, keep strictly-smaller t.
struct PickResult {
  ObjectId object;
  float distance = 0.0f;
  Vec3  worldPoint{0, 0, 0};
};

// What "frame everything"/"frame selection" (Home / period, Blender nav) aims
// a camera at — backlog item 5. Scene-owned; world-space, rotation/scale
// resolved (consumers read it directly, they do not transform it).
struct WorldBounds {
  Vec3  center{0, 0, 0};   // center of the union AABB of the framed objects
  float radius = 0.0f;     // half-diagonal of that AABB; positive floor kept
};

class World {
public:
  // New, unsaved worlds resolve relative image references from the process
  // working directory. SceneIO replaces this with the loaded file's directory.
  World();

  const std::string& assetBaseDirectory() const { return assetBaseDirectory_; }

  // ---- creation / destruction -------------------------------------
  // Blender-style names: createObject("Cube") on a fresh world makes
  // "Cube"; a collision suffixes "Cube.001", "Cube.002", ... An empty name
  // becomes "Object". createPrimitive defaults the name to the kind's word.
  ObjectId createObject(std::string name, Transform transform = {});
  ObjectId createPrimitive(Primitive kind, std::string name = {});
  ObjectId createCamera(std::string name = "Camera", Transform transform = {});

  // Children of the destroyed object are re-parented to its parent (Blender's
  // delete behaviour), its id leaves every parent/selection list, and the
  // generated mesh dies with it. Returns false for invalid/unknown ids.
  bool destroy(ObjectId id);

  // ---- queries -----------------------------------------------------
  Object*       get(ObjectId id);
  const Object* get(ObjectId id) const;
  const std::vector<Object>& objects() const { return objects_; }

  ObjectId parentOf(ObjectId id) const;            // invalid id if none
  std::vector<ObjectId> childrenOf(ObjectId id) const;
  bool  has(ObjectId id) const { return get(id) != nullptr; }
  bool  isDescendant(ObjectId id, ObjectId maybeAncestor) const;

  // World-space matrix: walks the parent chain, parentWorld * toMatrix.
  // Depth-capped at 64 to keep a corrupted (never producible via this API)
  // graph from recursing forever.
  Mat4 worldMatrix(ObjectId id) const;

  // Framing (backlog 5). Only objects that are BOTH on-screen (D22 rule-3
  // mask) and meshed count — the same rule buildPacket() and pick() use —
  // so 'frame everything' agrees with what is actually rendered. nullopt
  // when nothing qualifies (empty world / empty selection), so the caller
  // leaves the camera alone instead of jumping to the origin.
  std::optional<WorldBounds> bounds() const;           // the whole world
  std::optional<WorldBounds> selectionBounds() const;  // union over selection

  // Box select (backlog item 11): every object whose WORLD-space AABB
  // (computed after worldMatrix(i), so parent transforms are respected)
  // intersects the axis-aligned box [boxMin, boxMax], in creation order.
  //
  // GUARANTEE (conservative, no false negatives): any object the box fully
  // encloses on screen is IN the result — the world AABB always contains
  // the transformed geometry — but the test is AABB-level, so it may return
  // more than a pixel-exact screen-space clip would. Never fewer.
  //
  // Invisible objects (visible == false or collection-hidden via the D22
  // rule-3 mask) and unmeshed ones (hasMesh == false — groups, lights) are
  // NEVER returned. Ray picking (pick(), D16) stays exact and untouched.
  std::vector<ObjectId> pickBox(const Vec3& boxMin, const Vec3& boxMax) const;

  // Viewport picking (D16). See PickResult above for the full contract.
  std::optional<PickResult> pick(const ember::Ray& ray) const;

  // ---- edit-mode sub-element selection (D34 selection rule) ----------
  // The sets SCENE holds per object+mode, stamped with the topologyVersion
  // they were made at. Reading after a topology change returns nullptr (the
  // invalidate-or-remap rule resolved as: bumped-version ops CLEAR scene
  // state, so the editor reselects); move-only edits keep selections valid.
  // The mode is CORE's SelectMode (D37 removed scene's duplicate enum).
  void editSetSelection(ObjectId object, SelectMode mode,
                        const std::vector<u32>& indices);
  // nullptr when absent (never converted) OR stale (topology bumped since).
  const ember::scene::MeshSelection*
  editReadSelection(ObjectId object, SelectMode mode) const;
  void editClearEditSelections(ObjectId object);
  // Absence-honouring READ (cast costs nothing; never creates an edit).
  const ember::scene::EditableMesh* readableEdit(ObjectId object) const;
  // Absolute vertex-position capture/apply (item 116 exact cancel/redo):
  // mesh-local, EXACT (no delta arithmetic, no drift), no version bump.
  std::vector<ember::Vec3>
  editCaptureVertexPositions(ObjectId object,
                             const std::vector<u32>& vertexIds) const;
  bool editApplyVertexPositions(ObjectId object,
                                const std::vector<u32>& vertexIds,
                                const std::vector<ember::Vec3>& positions);

  // ---- edit-mode topology ops (item 111, on the D34 EditableMesh) -----
  // DATA-level ops Chisel's operators drive. All are reversible through the
  // existing captureSnapshot/reinsert machinery (the snapshot carries the
  // object's `edit` too). The object's `mesh` (render+pick form) is
  // regenerated from `edit` after every mutation. World deltas map through
  // the item-66 helpers' mapping so the editor never does matrix maths.
  // First edit converts `mesh` -> `edit` lazily (D34's Object-storage rule).
  // Non-const read over an object's EditableMesh (part of the op API:
  // mutations go through these methods, reads can inspect the structure).
  ember::scene::EditableMesh* editable(ObjectId object);
  bool editMoveVertices(ObjectId object, const std::vector<u32>& vertexIds,
                        const Vec3& worldDelta);
  bool editExtrudeFace(ObjectId object, u32 faceIndex,
                       const Vec3& worldOffset);
  // Deletes per the mode: Vertex -> the faces touching them; Edge -> the
  // faces using them; Face -> the faces themselves. Always purges unused
  // edges and orphaned vertices (D34 invariant 5).
  bool editDeleteSubElements(ObjectId object, SelectMode mode,
                             const std::vector<u32>& ids);
  bool editMergeVertices(ObjectId object, const std::vector<u32>& vertexIds,
                         const Vec3& worldPoint);

  // ---- modal transform / gizmo helpers (backlog item 66) -------------
  // SCENE-OWNED matrix plumbing so the editor never reimplements maths.
  // All three are EXACT (no epsilons), pure, and consistent with
  // worldMatrix()/Transform::toMatrix() — round-tripping a delta through
  // them restores the same matrix the delta was measured against.
  //
  // resolveWorldAxis: the WORLD-space direction of an object's axis (0=X,
  // 1=Y, 2=Z) with rotation+scale STRIPPED — i.e. what the gizmo axis arrow
  // points along in world space. With `space == Global` it returns the
  // canonical world axis verbatim (an object axis is ignored); with Local it
  // applies the object's world ROTATION only (never its scale — a scaled
  // handle must not skew, matching GizmoState::orientation/D20). The object
  // frame is the PARENT frame when the object has one: for gizmo translation
  // in local space the drag happens in the frame the transform is edited in.
  Vec3 resolveWorldAxis(ObjectId object, int axis,
                        bool localSpace) const;
  // worldAxisInParentFrame: map a world-space DELTA into the frame
  // setTransform() composes in (the PARENT frame) — the full inverse linear
  // part, S_p⁻¹·R_pᵀ (the parent's scale DIVIDES: a 1-world-unit drag under
  // a 2x-scaled parent maps to 0.5 local units, which is what makes the
  // world position move by exactly 1). Normalize the result if a direction
  // is wanted; this function returns the delta mapping, not a unit vector.
  Vec3 worldAxisInParentFrame(ObjectId object, const Vec3& worldDelta) const;
  // composeWorldTranslate: apply a WORLD-space translation delta to an
  // object that may have a parent (translation commutes with rotation, so
  // this is exact for every transform) and return the new Transform (does
  // not touch the world; setTransform the result).
  Transform composeWorldTranslate(ObjectId object, const Vec3& worldDelta) const;
  // composeWorldRotate: apply a WORLD-space rotation (axis + radians) about
  // the object's world ORIGIN (its current world translation) and return the
  // new local Transform. Exact when the parent chain is shear-free (the
  // header guarantee of reparentKeepWorld applies: uniform ancestor scales);
  // sheared chains are best-effort, documented, same as the decomposer.
  Transform composeWorldRotate(ObjectId object, const Vec3& worldAxis,
                               float radians) const;
  // composeWorldScale: multiply a WORLD-space scale ratio onto the object
  // along each of the three WORLD axes of `space` (see resolveWorldAxis);
  // returns the new local Transform. Uniform ratios (sx==sy==sz) are exact
  // for every chain; per-axis world ratios are exact when the chain is
  // shear-free, best-effort otherwise (documented above).
  Transform composeWorldScale(ObjectId object, const Vec3& worldRatio,
                              bool localSpace) const;
  // gizmoOrientation: the GizmoState::orientation basis (D20) for `space`:
  // identity for Global, the object's world ROTATION (scale/translation
  // stripped) for Local. A 4x4 with the basis in columns, translation zero.
  Mat4 gizmoOrientation(ObjectId object, bool localSpace) const;

  // ---- modal numeric entries + snap + pivots (backlog item 101) ------
  // All three helpers are PURE: they take the gesture's START transform
  // (the state at grab, before any running delta) and return the new
  // Transform; the editor then calls setTransform() with the result and
  // folds frames into one undo entry (D20's mergeWith contract).
  //
  // TYPED entries (Blender semantics, item 101): the number REPLACES the
  // running magnitude — an ABSOLUTE displacement/angle/factor measured from
  // the gesture's START state, in the modal operator's unit (translate:
  // world metres along the constrained axis; rotate: DEGREES about the
  // axis; scale: a multiplicative ratio). worldAxis must be unit-length —
  // the constraint axis the editor resolves (see resolveWorldAxis).
  Transform typedTranslate(ObjectId object, const Transform& start,
                           const Vec3& worldAxis, float metres) const;
  // The pivot for rotate/scale is the editor's resolved pointer (D20/D22:
  // GizmoState::origin — median, 3D cursor, bounding-box centre; individual
  // origins = the object's own world origin).
  Transform typedRotate(ObjectId object, const Transform& start,
                        const Vec3& worldAxis, float degrees,
                        const Vec3& pivotWorld) const;
  Transform typedScale(ObjectId object, const Transform& start,
                       const Vec3& ratio, const Vec3& pivotWorld,
                       bool localSpace = false) const;

  // SNAP increments derived from the scene unit settings (item 101; Blender
  // defaults): translate = the scene's unit scale in metres (default 1.0),
  // rotate = 15 degrees, scale = 0.1. unitScale is SCENE state: it
  // round-trips in .ember (the tolerant `units` record, FORMAT.md §3) so the
  // snapping stays consistent across save/load.
  float unitScale() const { return unitScale_; }
  void  setUnitScale(float metres);          // clamped to > 0
  float snapTranslateIncrement() const { return unitScale_; }
  float snapRotateDegrees() const { return 15.0f; }
  float snapScaleIncrement() const { return 0.1f; }

  // Arbitrary-pivot applications (item 101) — the same guarantees as the
  // pivotless composeWorldRotate/composeWorldScale (item 66) with the pivot
  // as an input instead of the object's own world origin:
  //  * POSITION changes are EXACT for every parent chain (per-point math);
  //  * SCALE is untouched by rotation (Blender's TRS semantics);
  //  * the scale/rotation rewrite is exact for shear-free chains (uniform
  //    ancestor scales), best-effort sheared — the same documented class as
  //    the decomposer/reparentKeepWorld.
  Transform composeWorldRotatePivot(ObjectId object, const Transform& start,
                                    const Vec3& worldAxis, float radians,
                                    const Vec3& pivotWorld) const;
  Transform composeWorldScalePivot(ObjectId object, const Transform& start,
                                   const Vec3& ratio,
                                   const Vec3& pivotWorld,
                                   bool localSpace = false) const;

  // ---- mutations (the editor's only route into the world) ----------
  // setParent is cycle-guarded (self, ancestor loops, unknown ids are all
  // silent no-ops); an invalid parent id detaches. It is O(children) at the
  // two parents only.
  void setParent(ObjectId child, ObjectId parent);
  // THE transform mutation: O(1) plain assign; safe to call every frame of a
  // gizmo drag. One call captures for undo; there is no per-axis variant.
  void setTransform(ObjectId id, const Transform& transform);
  void setMaterial(ObjectId id, const MaterialDesc& material);
  void setName(ObjectId id, std::string name);     // uniquified on clash
  void setVisible(ObjectId id, bool visible);

  // Blender-style "keep world transform" reparent (backlog item 25): the
  // child's WORLD matrix is preserved by REWRITING its local transform.
  // Cycle/self/unknown-id rules are identical to setParent() — a rejected
  // call is a silent no-op that leaves both transforms untouched.
  //
  // Guarantees:
  //  * worldMatrix(child) after the call equals the world matrix before, to
  //    floating rounding (1e-4 in tests), whenever the chain is shear-free
  //    (all ANCESTOR scales uniform or axis-aligned with their rotations —
  //    which is what gizmo-driven editing produces anyway).
  //  * Translation is preserved EXACTLY in every case (including sheared
  //    chains); sheared chains only lose precision in the linear part,
  //    because the child's local transform is T·R·S and the decomposer drops
  //    shear — the same best-effort as Blender's TRS decompose.
  //  * An invalid newParent detaches to root with the transform kept.
  //  * The decomposition writes the adjusted local position/rotation/scale
  //    into the child (euler XYZ, core convention R = Rz*Ry*Rx).
  void reparentKeepWorld(ObjectId child, ObjectId newParent);

  // ---- clear colour (§4.4) — DEPRECATED in favour of the D22 world
  // background: buildPacket() fills packet.background from setWorldBackground
  // below; packet.clearColor is NOT written any more (core stages the
  // deletion). Keep the accessor until the core deletion lands, then drop.
  void setClearColor(const ember::Color& color) { clearColor_ = color; }
  const ember::Color& clearColor() const { return clearColor_; }

  // ---- world background (D22(d)) — the World properties item 57 needs --
  // Scene state per the decision: buildPacket() copies it to the packet,
  // including its optional environment image reference.
  void setBackground(const ember::WorldBackground& background) {
    background_ = background;
  }
  const ember::WorldBackground& background() const { return background_; }

  // Scene-level render policy, persisted with the World and copied by value
  // into each RenderPacket. Invalid enum values fall back to Raster; numeric
  // controls are clamped by the shared core sanitizer.
  const ember::SceneRenderSettings& renderSettings() const {
    return renderSettings_;
  }
  void setRenderSettings(ember::SceneRenderSettings settings);

  // ---- 3D cursor (D22, item 157) -------------------------------------
  // A scene-space point, persisted in .ember. This is only the datum: the
  // editor's PivotMode::Cursor3D consumer is a separate item and is not
  // implied by this API. The getter returns a value (read-only to consumers).
  Vec3 cursor() const;
  void setCursor(const Vec3& position);

  // ---- frame range (backlog item 86) ---------------------------------
  // The frame state Blender's Scene keeps — NOT animation. This is what the
  // Timeline area displays and edits; playback is NOT implemented and the
  // UI must say so plainly. Defaults match Blender: 1..250, current 1.
  int  frameStart() const { return frameStart_; }
  int  frameEnd() const { return frameEnd_; }
  int  frameCurrent() const { return frameCurrent_; }
  // Invariants, enforced by the setters (and by load, which clamps):
  //  * setFrameStart: if the new start would exceed the end, the end moves
  //    up with it (start <= end always).
  //  * setFrameEnd: if the new end would undercut the start, the start
  //    moves down with it.
  //  * setFrameCurrent: clamped into [start, end].
  void setFrameStart(int frame);
  void setFrameEnd(int frame);
  void setFrameCurrent(int frame);

  // ---- lights (D19; re-frozen) --------------------------------------
  // A light is an ordinary object: unmeshed, visible by default, Transform
  // carries aim (directional: local -Z) or position (point). Default names:
  // "Sun" (directional) and "Point" (point), uniquified like mesh names.
  ObjectId createLight(ember::LightType type, std::string name = {});
  // No-op unless the id names an existing LIGHT object (isLight); the
  // component is plain storage (position/aim live in the Transform — never
  // duplicated here).
  void setLight(ObjectId id, const ember::Light& light);

  // Camera projection is authored on a camera object; invalid or non-camera
  // ids and invalid projection ranges are rejected without changing the object.
  bool setCameraSettings(ObjectId id, const CameraSettings& settings);

  // ---- editor support (M3/M5, gizmos/outliner/undo) ----------------
  // delete-undo primitive pair: capture the subtree, destroy, reinsert on
  // undo — original ids and hierarchy come back identical. reinsert()
  // semantics live on ObjectSnapshot above (adopt-and-reset for survivors).
  std::optional<ObjectSnapshot> captureSnapshot(ObjectId subtreeRoot) const;
  ObjectId reinsert(const ObjectSnapshot& snapshot);

  // Subtree copy with fresh ids: the duplicated root gets the given name
  // (or the source root's name, uniquified "Name.001" style); descendants
  // keep their source names. The duplicate root inherits the source root's
  // current parent. Returns 0 for invalid id.
  ObjectId duplicate(ObjectId id, std::string name = {});

  // ---- collections (D22; re-frozen) ---------------------------------
  // A named grouping tree, independent of Object parenting; collections
  // NEVER transform anything. Membership rules live on Object; the
  // exclusion rule is rule 3 (see the comment by struct Collection).
  CollectionId createCollection(std::string name, CollectionId parent = {});
  // Children collections AND the destroyed collection's member objects move
  // up to its parent collection (invalid parent = implicit root). UNDOABLE —
  // captureCollection/reinsertCollection below.
  bool destroyCollection(CollectionId id);
  void setCollectionVisible(CollectionId id, bool visible);
  // Membership move. Invalid target = the implicit root collection (an
  // object is in exactly ONE collection). Silent no-op for unknown ids.
  void assignToCollection(ObjectId objectId, CollectionId target = {});

  const std::vector<Collection>& collections() const { return collections_; }
  const Collection* get(CollectionId id) const;
  CollectionId collectionOf(ObjectId objectId) const;

  // Undo support for destroyCollection (D22: must be reversible; the same
  // capture -> destroy -> reinsert pattern as object snapshots).
  struct CollectionSnapshot {
    // Subtree in DFS order (nodes[0] = the destroyed root, whose parent
    // field was its world-parent link at capture time), each with the
    // member ids that lived in THAT node at capture time.
    struct Node {
      Collection node;
      std::vector<ObjectId> members;   // object ids with collection == node
    };
    std::vector<Node> nodes;
  };
  std::optional<CollectionSnapshot> captureCollection(CollectionId root) const;
  CollectionId reinsertCollection(const CollectionSnapshot& snapshot);

  // ---- material slots (D22(c)) ---------------------------------------
  // Ordered per-object slots; slot 0 IS the base material (the legacy
  // Object::material keeps reading it until the staging deletion lands).
  // Every object starts with exactly one implicit default slot.
  u32  materialSlotCount(ObjectId id) const;
  void setMaterialSlot(ObjectId id, u32 slot,
                       const ember::MaterialDesc& material);
  u32  addMaterialSlot(ObjectId id, const ember::MaterialDesc& material,
                       std::string name = {});
  bool removeMaterialSlot(ObjectId id, u32 slot);   // slot 0 cannot go
  const ember::MaterialSlot* materialSlots(ObjectId id, u32& count) const;

  // The active scene camera. Invalid id clears it; otherwise the id must refer
  // to an object with camera.isCamera. App consumes its transform/settings for
  // look-through; the orbit view remains App's fallback.
  void setActiveCamera(ObjectId id);
  ObjectId activeCamera() const { return activeCamera_; }

  // ---- selection -----------------------------------------------------
  const std::vector<ObjectId>& selection() const { return selection_; }
  // Non-additive replaces the selection with exactly this object (invalid ids
  // are ignored). Additive toggles membership.
  void select(ObjectId id, bool additive = false);
  void deselect(ObjectId id);
  void clearSelection() { selection_.clear(); }

  // ---- modifier stack (D40, frozen; items 121/122) ---------------------
  // Ordered per-object stack (index 0 evaluated first; a DISABLED entry keeps
  // its position). All mutators bump `modifierVersion`, which is what keeps
  // the evaluated-mesh cache honest — never write `o.modifiers` directly.
  // The panel (item 122) builds against these exactly.
  const std::vector<ember::Modifier>& modifiers(ObjectId object) const;
  bool addModifier(ObjectId object, ember::ModifierKind kind);
  bool removeModifier(ObjectId object, u32 index);
  // Reorder: moderator-preserving move (Blender's up/down arrows), from ->
  // to. Other entries keep their relative order.
  bool moveModifier(ObjectId object, u32 from, u32 to);
  bool setModifierEnabled(ObjectId object, u32 index, bool enabled);
  // Refused unless the params' variant index matches the entry's kind (the
  // variant index IS the kind in D40; a mismatched pair cannot be stored).
  bool setModifierParams(ObjectId object, u32 index,
                         const ember::ModifierParams& params);
  // Whole-stack restore for undo (item 122's panel ops are undoable).
  bool setModifierStack(ObjectId object,
                        const std::vector<ember::Modifier>& stack);
  u32 modifierStackVersion(ObjectId object) const;
  // Evaluation failure report (D40: failure is a no-op, recorded): the index
  // of the first modifier that failed the LAST evaluation, or 0xFFFFFFFF.
  u32 evaluatedFailureIndex(ObjectId object) const;

  // ---- scene -> renderer ---------------------------------------------
  // D2/D3/D9/D19(staged complete)/D22: fills items (one per meshed,
  // on-screen object: the D22 rule-3 mask), width/height, clearColor,
  // scene render settings, and the full RenderPacket::lights list
  // (world-resolved, creation order — including unmeshed/hidden lights;
  // render AND the editor's id mapping depend on the order). The deprecated
  // packet.light is no longer filled (render reads only packet.lights;
  // deletion handled by core). The camera
  // (view/projection/cameraPosition) belongs to render::OrbitCamera and is
  // filled by App — NOT here.
  //
  // D40: a non-empty modifier stack is EVALUATED here — DrawItem::mesh then
  // points into World-owned evaluated storage, valid for the frame (the D9
  // packet-lifetime rule). `suspendModifierStack` (additive, default false)
  // is the EDIT-MODE suspension hook (D40: in Edit the viewport draws the
  // BASE mesh — base-indexed overlays/picking), for the App loop to pass the
  // editor's mode; object-mode picking uses the EVALUATED mesh regardless
  // (pick(), below).
  RenderPacket buildPacket(int width, int height,
                           bool suspendModifierStack = false) const;

private:
  Object*       getImpl(ObjectId id);
  const Object* getImpl(ObjectId id) const;
  static void uniquifyAgainst(const std::vector<Object>& objects,
                              const Object* self, std::string& name);
  Mat4 worldMatrixImpl(ObjectId id, u32 depth) const;
  void collectSubtree(ObjectId id, ObjectSnapshot& snap,
                      ObjectId parentLink, u32 depth) const;
  // World-space AABB of one object's mesh (after worldMatrix); false when
  // unmeshed. Shared by bounds()/selectionBounds()/pickBox().
  bool worldAABB(ObjectId id, float outMin[3], float outMax[3]) const;
  std::optional<WorldBounds> boundsImpl(const std::vector<ObjectId>& subset,
                                        bool subsetIsAll) const;
  // D22 rule-3 mask, shared by the packet, picking and bounds paths.
  bool emitsOnScreen(const Object& o) const;
  const Collection* getCollectionImpl(CollectionId id) const;

  std::vector<Object> objects_;                 // creation order, dense
  std::string assetBaseDirectory_;               // absolute scene-relative asset root
  u32 nextId_ = 1;
  std::vector<ObjectId> selection_;
  ObjectId activeCamera_;                       // 0 = none (backlog 31/33)
  ember::Color clearColor_{0.12f, 0.12f, 0.13f, 1.0f};  // deprecated (D22(d))
  ember::WorldBackground background_;           // D22(d): the scene's World
  ember::SceneRenderSettings renderSettings_;
  Vec3 cursor_{0.0f, 0.0f, 0.0f};                // D22/item 157: world point
  int frameStart_ = 1, frameEnd_ = 250, frameCurrent_ = 1;  // item 86
  std::vector<Collection> collections_;         // D22: grouping tree
  u32 nextCollectionId_ = 1;
  float unitScale_ = 1.0f;                      // item 101: scene unit scale
  std::unordered_map<u64, ember::scene::MeshSelection>
      editSelections_;                          // key (obj<<2)|mode (D34)
  mutable std::unordered_map<u32, std::vector<u32>>
      triangleMaterialCache_;                   // packet-owned mapping storage, per World

  // ---- D40 evaluated-mesh cache ---------------------------------------
  // One entry per object with a non-empty stack, keyed by object id. Valid
  // while (meshVersion, modifierVersion) match; entries are pruned on
  // destroy() and cleared by load(). `mutable`: buildPacket()/pick() are
  // const, yet evaluation output must live SOMEWHERE owned by the world
  // (D40) and stay alive for the frame.
  struct EvalCacheEntry {
    ObjectId   id{0};
    u32        meshVersion = 0;
    u32        modifierVersion = 0;
    u32        failedIndex = 0xFFFFFFFFu;   // D40: recorded failure, if any
    ember::MeshData mesh;                   // the evaluated result (by value)
  };
  mutable std::unordered_map<u32, EvalCacheEntry> evalCache_;
  // Cache lookup/evaluation shared by buildPacket() and pick().
  const ember::MeshData& evaluatedFor(const Object& o,
                                      u32& failedIndex) const;
  const u32* cachedTriangleMaterialSlots(ObjectId object,
                                         std::vector<u32> slots) const;

  // .ember I/O is declared as free functions (contract §4.4) but needs the
  // private state by design; friendship is not transitive, so the
  // materialisation code lives inside load() — see SceneIO.cpp. The format
  // cheat sheet travels with the declarations below.
  friend bool save(const World&, const std::string&);
  friend bool load(World&, const std::string&);
};

// Generated mesh data for one primitive kind (Primitives.cpp). World copies
// the result into each object's own `mesh` at creation time.
MeshData makePrimitiveMesh(Primitive kind);

// Stack evaluation (Modifiers.cpp, item 121): walks the ordered stack,
// SKIPPING disabled entries (D40: position preserved), applying each
// modifier as `const MeshData& -> MeshData`. D40 rules live in the
// implementation header: determinism, failure-is-a-no-op (the first failing
// modifier's index is written through `failedIndex`, 0xFFFFFFFF = none),
// and non-destruction of the input. Scene-internal: the editor consumes the
// World API, not this function.
MeshData evaluateMeshStack(const MeshData& base,
                           const std::vector<ember::Modifier>& stack,
                           u32* failedIndex);

// ---- .ember persistence (SceneIO.cpp) --------------------------------------
//
// The format of records lives in src/scene/FORMAT.md (v7).
// Cheat sheet — details, failure rules and the exact grammar in that file:
//   * Writers emit `EMBER3D.SCENE v7`; readers ACCEPT v1-v7 and refuse
//     any other version — a wrong-version file fails load()
//     with the target world untouched, never a silent mis-read (backlog 31).
//   * v1 -> current migration on load: the v1 world `light` record becomes one
//     "Sun" light object; v1 files never carry per-object light records.
//   * v7 record order: header, nextid, selection, `active <id|0>` (the
//     active-camera pointer, 0 = none — the object it names becomes a typed
//     camera in backlog 33), optional `worldbg`/`frame`, optional
//     `cursor x y z` (item 157, absent means world origin), required `render`
//     engine/samples/bounces/seed record, { object },
//     { light }, { collection },
//     { incollection }, { materialslot }, { image references }, `end`. Texture
//     paths resolve from assetBaseDirectory; save-as rebases relative references
//     without mutating the World. Records absent from a file
//     leave those features at their defaults; v1-v5 render settings migrate to
//     Raster/64/4/1; a lightless load always gains the documented default sun
//     (FORMAT.md §6).
//   * Byte identity: 9-significant-digit floats make load(save(w)) restore w
//     bitwise; save(load(T)) is stable from the second hop for lightless T
//     (the default sun is added exactly once).
bool save(const World& world, const std::string& path);
bool load(World& world, const std::string& path);

} // namespace ember::scene
