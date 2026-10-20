// Scene.cpp — World implementation: storage, hierarchy, selection, packet.
// Owned by Slate · Scene.
//
// All vector/matrix maths is core's (one Math.h — Ash's rule). Scene owns only
// the object graph and the primitives.
#include "scene/Scene.h"

#include "core/Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace ember::scene {

namespace {

struct ProjectedPoint {
  float x;
  float y;
};

// Match EditableMesh::toMeshData()'s emitted triangle count for a face. The
// packet only needs the face-to-triangle correspondence, but it must follow
// the exact ear-clipping fallback as well as the normal n-2 case.
u32 emittedTriangleCount(const EditableMesh& edit, const Face& face) {
  if (face.loopCount < 3 ||
      static_cast<std::size_t>(face.firstLoop) + face.loopCount >
          edit.loops.size())
    return 0;
  if (face.loopCount == 3) return 1;

  Vec3 normal{0, 0, 0};
  for (u32 i = 0; i < face.loopCount; ++i) {
    const u32 li = face.firstLoop + i;
    const u32 next = face.firstLoop + (i + 1) % face.loopCount;
    const u32 va = edit.loops[li].vertex;
    const u32 vb = edit.loops[next].vertex;
    if (va >= edit.positions.size() || vb >= edit.positions.size()) return 0;
    const Vec3& a = edit.positions[va];
    const Vec3& b = edit.positions[vb];
    normal = Vec3{normal.x + (a.y - b.y) * (a.z + b.z),
                  normal.y + (a.z - b.z) * (a.x + b.x),
                  normal.z + (a.x - b.x) * (a.y + b.y)};
  }
  normal = ember::normalize(normal);

  const float ax = std::fabs(normal.x);
  const float ay = std::fabs(normal.y);
  const float az = std::fabs(normal.z);
  std::vector<ProjectedPoint> poly(face.loopCount);
  float signedArea = 0.0f;
  for (u32 i = 0; i < face.loopCount; ++i) {
    const Vec3& p = edit.positions[edit.loops[face.firstLoop + i].vertex];
    if (az >= ax && az >= ay)      poly[i] = ProjectedPoint{p.x, p.y};
    else if (ax >= ay && ax >= az) poly[i] = ProjectedPoint{p.y, p.z};
    else                           poly[i] = ProjectedPoint{p.z, p.x};
  }
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const ProjectedPoint& a = poly[i];
    const ProjectedPoint& b = poly[(i + 1) % poly.size()];
    signedArea += a.x * b.y - b.x * a.y;
  }
  if (std::fabs(signedArea) <= 1e-12f) return face.loopCount;  // fan fallback

  const auto area2 = [&poly](u32 a, u32 b, u32 c) {
    const ProjectedPoint& A = poly[a];
    const ProjectedPoint& B = poly[b];
    const ProjectedPoint& C = poly[c];
    return (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
  };
  const auto pointInTri = [&area2](u32 p, u32 a, u32 b, u32 c) {
    const float d1 = area2(a, b, p);
    const float d2 = area2(b, c, p);
    const float d3 = area2(c, a, p);
    const bool hasNeg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    const bool hasPos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(hasNeg && hasPos);
  };

  std::vector<u32> ring(face.loopCount);
  for (u32 i = 0; i < face.loopCount; ++i) ring[i] = i;
  u32 emitted = 0;
  std::size_t guard = 0;
  while (ring.size() > 3 &&
         guard++ < static_cast<std::size_t>(face.loopCount) * 4) {
    bool clipped = false;
    for (std::size_t i = 0; i < ring.size() && !clipped; ++i) {
      const std::size_t ip = (i + ring.size() - 1) % ring.size();
      const std::size_t in = (i + 1) % ring.size();
      const u32 a = ring[ip], b = ring[i], c = ring[in];
      if (area2(a, b, c) <= 1e-12f) continue;
      bool containsOther = false;
      for (const u32 p : ring) {
        if (p == a || p == b || p == c) continue;
        if (pointInTri(p, a, b, c)) { containsOther = true; break; }
      }
      if (containsOther) continue;
      ++emitted;
      ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
    }
    if (!clipped) break;
  }
  return emitted + 1;  // toMeshData emits the final three-corner ring
}

std::vector<u32> baseTriangleMaterialSlots(const Object& object,
                                           u32 triangleCount) {
  std::vector<u32> slots;
  slots.reserve(triangleCount);
  if (object.hasEdit) {
    for (const Face& face : object.edit.faces) {
      const u32 count = emittedTriangleCount(object.edit, face);
      for (u32 i = 0; i < count && slots.size() < triangleCount; ++i)
        slots.push_back(face.materialIndex);
    }
  }
  // Procedural/legacy meshes and any defensively unmatched generated triangles
  // use the D45 default slot. Never expose a mapping with the wrong length.
  slots.resize(triangleCount, 0u);
  return slots;
}

void applyModifierMaterialMapping(const std::vector<ember::Modifier>& modifiers,
                                  u32 failedIndex,
                                  std::vector<u32>& slots) {
  for (u32 i = 0; i < modifiers.size(); ++i) {
    if (i == failedIndex) break;  // evaluation returned the last good mesh
    const ember::Modifier& modifier = modifiers[i];
    if (!modifier.enabled) continue;
    switch (modifier.kind()) {
      case ember::ModifierKind::Subdivision: {
        const int levels = std::clamp(
            std::get<ember::SubdivisionParams>(modifier.params).levels, 0, 6);
        for (int level = 0; level < levels; ++level) {
          std::vector<u32> next;
          next.reserve(slots.size() * 6);
          for (const u32 slot : slots)
            for (int child = 0; child < 6; ++child) next.push_back(slot);
          slots = std::move(next);
        }
        break;
      }
      case ember::ModifierKind::Mirror: {
        const ember::MirrorParams& params =
            std::get<ember::MirrorParams>(modifier.params);
        const int axes = static_cast<int>(params.axisX) +
                         static_cast<int>(params.axisY) +
                         static_cast<int>(params.axisZ);
        for (int axis = 0; axis < axes; ++axis) {
          const std::vector<u32> original = slots;
          slots.insert(slots.end(), original.begin(), original.end());
        }
        break;
      }
      case ember::ModifierKind::Array: {
        const int count = std::clamp(
            std::get<ember::ArrayParams>(modifier.params).count, 1, 64);
        if (count > 1) {
          const std::vector<u32> oneCopy = slots;
          slots.clear();
          slots.reserve(oneCopy.size() * static_cast<std::size_t>(count));
          for (int copy = 0; copy < count; ++copy)
            slots.insert(slots.end(), oneCopy.begin(), oneCopy.end());
        }
        break;
      }
    }
  }
}

}  // namespace

World::World() {
  std::error_code error;
  const std::filesystem::path current = std::filesystem::current_path(error);
  if (!error) assetBaseDirectory_ = current.lexically_normal().generic_string();
}

const u32* World::cachedTriangleMaterialSlots(ObjectId object,
                                               std::vector<u32> slots) const {
  std::vector<u32>& cached = triangleMaterialCache_[object.value];
  if (cached != slots) cached = std::move(slots);
  return cached.empty() ? nullptr : cached.data();
}

// ---------------------------------------------------------------- helpers

Object* World::getImpl(ObjectId id) {
  const auto it = id.valid()
    ? std::find_if(objects_.begin(), objects_.end(),
                   [&](const Object& o) { return o.id == id; })
    : objects_.end();
  return it == objects_.end() ? nullptr : &*it;
}

const Object* World::getImpl(ObjectId id) const {
  return const_cast<World*>(this)->getImpl(id);
}

Object* World::get(ObjectId id) { return getImpl(id); }
const Object* World::get(ObjectId id) const { return getImpl(id); }

// Blender-style suffixing: appends ".001", ".002", ... onto `name` if (and
// only if) a clash was found. `self` is excluded from the comparison.
void World::uniquifyAgainst(const std::vector<Object>& objects,
                            const Object* self, std::string& name) {
  const auto exists = [&](const std::string& n) {
    for (const Object& o : objects)
      if (&o != self && o.name == n) return true;
    return false;
  };
  if (!exists(name)) return;
  for (u32 i = 1;; ++i) {
    std::ostringstream os;
    os << name << '.' << std::setfill('0') << std::setw(3) << i;
    if (!exists(os.str())) { name = os.str(); return; }
  }
}

// ---------------------------------------------------------------- creation

ObjectId World::createObject(std::string name, Transform transform) {
  Object o;
  o.id = ObjectId{nextId_++};
  o.name = name.empty() ? "Object" : std::move(name);
  o.transform = transform;
  o.parent = ObjectId{0};
  o.hasMesh = false;

  uniquifyAgainst(objects_, nullptr, o.name);

  // D22(c): slot 0 mirrors the base material; one implicit default slot.
  ember::MaterialSlot baseSlot;
  baseSlot.material = o.material;
  o.materials.push_back(std::move(baseSlot));

  objects_.push_back(std::move(o));
  return objects_.back().id;
}

ObjectId World::createPrimitive(Primitive kind, std::string name) {
  static const char* kDefaultNames[5] = {
    "Cube", "Sphere", "Plane", "Cylinder", "Torus"};
  const int index = static_cast<int>(kind);
  if (index < 0 || index > 4) return ObjectId{0};  // unknown kind: no object

  const ObjectId id = createObject(name.empty() ? kDefaultNames[index]
                                                : std::move(name));
  if (Object* o = get(id)) {
    o->hasMesh = true;
    o->primitive = kind;
    o->mesh = makePrimitiveMesh(kind);
    o->mesh.name = kDefaultNames[index];   // kind word, not object name
    ++o->meshVersion;                      // D40: ANY mesh replacement bumps
    // NOTE: the slot-0 mirror was already seeded by createObject; do NOT
    // seed again (a second seed grew an extra slot per primitive, bug caught
    // by the byte-identical round trip test).
  }
  return id;
}

ObjectId World::createCamera(std::string name, Transform transform) {
  const ObjectId id = createObject(name.empty() ? "Camera" : std::move(name), transform);
  if (Object* o = get(id)) o->camera.isCamera = true;
  return id;
}

// ---------------------------------------------------------------- destroy

bool World::destroy(ObjectId id) {
  Object* o = get(id);
  if (!o) return false;

  // Children jump up to their grandparent (Blender's delete behaviour): the
  // child's parent FIELD and the new parent's children LIST both move, and
  // the child's own transform is untouched.
  const std::vector<ObjectId> children = o->children;
  for (ObjectId childId : children) {
    Object* child = get(childId);
    if (!child) continue;
    child->parent = o->parent;
    if (o->parent.valid())
      if (Object* grandparent = get(o->parent))
        grandparent->children.push_back(childId);
  }

  // Drop the destroyed id from its parent's children list.
  if (o->parent.valid())
    if (Object* parent = get(o->parent)) {
      auto& v = parent->children;
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
    }

  // Selection never keeps a dangling id.
  selection_.erase(std::remove(selection_.begin(), selection_.end(), id),
                   selection_.end());
  if (activeCamera_ == id) activeCamera_ = ObjectId{0};
  // D40: the evaluated-mesh cache never outlives its owner.
  evalCache_.erase(id.value);
  triangleMaterialCache_.erase(id.value);

  // Swap-erase from the dense vector.
  const auto it = std::find_if(objects_.begin(), objects_.end(),
                               [&](const Object& e) { return e.id == id; });
  if (it == objects_.end()) return false;
  *it = std::move(objects_.back());
  objects_.pop_back();
  return true;
}

// ---------------------------------------------------------------- queries

ObjectId World::parentOf(ObjectId id) const {
  const Object* o = get(id);
  return o ? o->parent : ObjectId{0};
}

std::vector<ObjectId> World::childrenOf(ObjectId id) const {
  const Object* o = get(id);
  return o ? o->children : std::vector<ObjectId>{};
}

bool World::isDescendant(ObjectId id, ObjectId maybeAncestor) const {
  if (!id.valid() || !maybeAncestor.valid()) return false;
  for (ObjectId p = parentOf(id); p.valid(); p = parentOf(p))
    if (p == maybeAncestor) return true;
  return false;
}

Mat4 World::worldMatrixImpl(ObjectId id, u32 depth) const {
  const Object* o = get(id);
  if (!o) return Mat4{};                       // identity-aware consumers guard
  const Mat4 local = o->transform.toMatrix();
  if (depth >= 64 || !o->parent.valid())
    return local;
  return worldMatrixImpl(o->parent, depth + 1) * local;
}

Mat4 World::worldMatrix(ObjectId id) const { return worldMatrixImpl(id, 0); }

// ---------------------------------------------------------------- mutations

void World::setParent(ObjectId childId, ObjectId parentId) {
  Object* child = get(childId);
  if (!child) return;
  if (parentId.valid() && !get(parentId)) return;                  // unknown
  if (parentId.valid() && parentId == childId) return;              // self
  if (parentId.valid() && isDescendant(parentId, childId)) return; // cycle

  if (child->parent.valid())
    if (Object* old = get(child->parent)) {
      auto& v = old->children;
      v.erase(std::remove(v.begin(), v.end(), childId), v.end());
    }

  child->parent = parentId;
  if (parentId.valid())
    if (Object* parent = get(parentId))
      if (std::find(parent->children.begin(), parent->children.end(),
                    childId) == parent->children.end())
        parent->children.push_back(childId);
}

void World::setTransform(ObjectId id, const Transform& transform) {
  if (Object* o = get(id)) o->transform = transform;
}

void World::setMaterial(ObjectId id, const MaterialDesc& material) {
  if (Object* o = get(id)) {
    o->material = material;
    // D45: slot 0 is authoritative whenever the ordered slot array is present.
    // Keep the legacy setter as a slot-0 write so old callers and persistence
    // migration cannot leave the renderer reading a stale default material.
    if (!o->materials.empty()) o->materials[0].material = material;
  }
}

void World::setName(ObjectId id, std::string name) {
  Object* o = get(id);
  if (!o || name.empty()) return;
  uniquifyAgainst(objects_, o, name);
  o->name = std::move(name);
}

void World::setVisible(ObjectId id, bool visible) {
  if (Object* o = get(id)) o->visible = visible;
}

Vec3 World::cursor() const {
  return cursor_;
}

void World::setCursor(const Vec3& position) {
  cursor_ = position;
}

void World::setRenderSettings(ember::SceneRenderSettings settings) {
  renderSettings_ = ember::sanitizedSceneRenderSettings(settings);
}

void World::setFrameStart(int frame) {
  frameStart_ = frame;
  if (frameEnd_ < frameStart_) frameEnd_ = frameStart_;
  if (frameCurrent_ < frameStart_) frameCurrent_ = frameStart_;
  if (frameCurrent_ > frameEnd_) frameCurrent_ = frameEnd_;
}

void World::setFrameEnd(int frame) {
  frameEnd_ = frame;
  if (frameStart_ > frameEnd_) frameStart_ = frameEnd_;
  if (frameCurrent_ < frameStart_) frameCurrent_ = frameStart_;
  if (frameCurrent_ > frameEnd_) frameCurrent_ = frameEnd_;
}

void World::setFrameCurrent(int frame) {
  if (frame < frameStart_) frame = frameStart_;
  if (frame > frameEnd_) frame = frameEnd_;
  frameCurrent_ = frame;
}

// ---------------------------------------------------------------- helpers

// D22 rule 3, verbatim (an object emits a DrawItem iff):
//     object.visible && (!object.collection.valid() || collection.visible)
// The same mask drives pick()/pickBox()/bounds() so the outliner eye icons
// and everything "on screen" cannot disagree. NOTE per the decision text:
// only the member's OWN collection flag is consulted (post to the Lead when
// ancestor propagation is wanted for it).
bool World::emitsOnScreen(const Object& o) const {
  if (!o.visible) return false;
  if (!o.collection.valid()) return true;
  const Collection* c = getCollectionImpl(o.collection);
  return !c || c->visible;  // an unknown id (never producible) counts as shown
}

// ---------------------------------------------------------------- slots

u32 World::materialSlotCount(ObjectId id) const {
  const Object* o = get(id);
  return o ? static_cast<u32>(o->materials.size()) : 0;
}

void World::setMaterialSlot(ObjectId id, u32 slot,
                            const ember::MaterialDesc& material) {
  Object* o = get(id);
  if (!o || slot >= o->materials.size()) return;
  o->materials[slot].material = material;
  if (slot == 0) o->material = material;  // staging: slot 0 mirrors legacy
}

u32 World::addMaterialSlot(ObjectId id, const ember::MaterialDesc& material,
                           std::string name) {
  Object* o = get(id);
  if (!o) return 0;
  ember::MaterialSlot s;
  s.name = std::move(name);
  s.material = material;
  o->materials.push_back(std::move(s));
  return static_cast<u32>(o->materials.size() - 1);
}

bool World::removeMaterialSlot(ObjectId id, u32 slot) {
  Object* o = get(id);
  if (!o || slot == 0 || slot >= o->materials.size()) return false;
  o->materials.erase(o->materials.begin() + slot);
  return true;
}

const ember::MaterialSlot* World::materialSlots(ObjectId id, u32& count) const {
  const Object* o = get(id);
  if (!o) { count = 0; return nullptr; }
  count = static_cast<u32>(o->materials.size());
  return o->materials.data();
}

// ---------------------------------------------------------------- lights

ObjectId World::createLight(ember::LightType type, std::string name) {
  // Blender's words: a sun for directional, a point for point. The component
  // copies the core defaults (white, intensity 1, radius 1).
  const char* defaultName =
    (type == ember::LightType::Directional) ? "Sun" : "Point";
  const ObjectId id = createObject(name.empty() ? defaultName
                                                : std::move(name));
  if (Object* o = get(id)) {
    o->isLight = true;
    o->light.type = type;
    o->light.color = ember::Color{1.0f, 1.0f, 1.0f, 1.0f};
    o->light.intensity = 1.0f;
    o->light.radius = 1.0f;
  }
  return id;
}

void World::setLight(ObjectId id, const ember::Light& light) {
  Object* o = get(id);
  if (!o || !o->isLight) return;   // lights are objects; only they carry it
  o->light = light;
}

bool World::setCameraSettings(ObjectId id, const CameraSettings& settings) {
  Object* o = get(id);
  if (!o || !o->camera.isCamera || !settings.isCamera ||
      !std::isfinite(settings.fovY) || !std::isfinite(settings.clipNear) ||
      !std::isfinite(settings.clipFar) || !std::isfinite(settings.orthoScale) ||
      settings.fovY <= 0.0f || settings.fovY >= ember::kPi ||
      settings.clipNear <= 0.0f || settings.clipFar <= settings.clipNear ||
      settings.orthoScale <= 0.0f) {
    return false;
  }
  o->camera = settings;
  return true;
}

void World::setActiveCamera(ObjectId id) {
  if (!id.valid()) {
    activeCamera_ = ObjectId{0};
    return;
  }
  const Object* o = get(id);
  if (o && o->camera.isCamera) activeCamera_ = id;
}

// ---------------------------------------------------------------- collections

const Collection* World::getCollectionImpl(CollectionId id) const {
  for (const Collection& c : collections_)
    if (c.id == id) return &c;
  return nullptr;
}

const Collection* World::get(CollectionId id) const {
  return getCollectionImpl(id);
}

// ".001"-style uniquify against every existing collection.
static void uniquifyCollectionName(std::vector<Collection>& collections,
                                   std::string& name) {
  const auto exists = [&](const std::string& n) {
    for (const Collection& c : collections)
      if (c.name == n) return true;
    return false;
  };
  if (!exists(name)) return;
  for (u32 i = 1;; ++i) {
    std::ostringstream os;
    os << name << '.' << std::setfill('0') << std::setw(3) << i;
    if (!exists(os.str())) { name = os.str(); return; }
  }
}

CollectionId World::createCollection(std::string name, CollectionId parentId) {
  if (parentId.valid() && !getCollectionImpl(parentId))
    return CollectionId{0};  // unknown parent
  Collection c;
  c.id = CollectionId{nextCollectionId_++};
  c.parent = parentId;
  c.name = name.empty() ? ember::UiVocabulary::Collection : std::move(name);
  uniquifyCollectionName(collections_, c.name);
  collections_.push_back(std::move(c));
  return collections_.back().id;
}

bool World::destroyCollection(CollectionId id) {
  const auto it = std::find_if(collections_.begin(), collections_.end(),
                               [&](const Collection& c) { return c.id == id; });
  if (it == collections_.end()) return false;
  const CollectionId parent = it->parent;  // children + members move here

  for (Collection& c : collections_)
    if (c.parent == id) c.parent = parent;
  for (Object& o : objects_)
    if (o.collection == id) o.collection = parent;

  collections_.erase(it);
  return true;
}

void World::setCollectionVisible(CollectionId id, bool visible) {
  if (Collection* c = const_cast<Collection*>(getCollectionImpl(id)))
    c->visible = visible;
}

void World::assignToCollection(ObjectId objectId, CollectionId target) {
  Object* o = get(objectId);
  if (!o) return;
  if (target.valid() && !getCollectionImpl(target)) return;  // unknown target
  o->collection = target;
}

CollectionId World::collectionOf(ObjectId objectId) const {
  const Object* o = get(objectId);
  return o ? o->collection : CollectionId{0};
}

std::optional<World::CollectionSnapshot>
World::captureCollection(CollectionId root) const {
  const Collection* r = getCollectionImpl(root);
  if (!r) return std::nullopt;

  CollectionSnapshot snap;
  snap.nodes.push_back(World::CollectionSnapshot::Node{*r, {}});

  // Depth-first over the subtree in creation order (same walk shape as the
  // object snapshot; the collection tree is cycle-guarded upstream).
  std::size_t processed = 0;
  while (processed < snap.nodes.size()) {
    const CollectionId current = snap.nodes[processed].node.id;
    for (const Collection& c : collections_)
      if (c.parent == current)
        snap.nodes.emplace_back(
          World::CollectionSnapshot::Node{c, {}});
    ++processed;
  }
  // Members: everything whose collection is any captured node (direct
  // membership recorded per node, creation order over objects_).
  for (std::size_t n = 0; n < snap.nodes.size(); ++n) {
    const CollectionId nodeId = snap.nodes[n].node.id;
    for (const Object& o : objects_)
      if (o.collection == nodeId)
        snap.nodes[n].members.push_back(o.id);
  }
  return snap;
}

CollectionId World::reinsertCollection(
    const World::CollectionSnapshot& snapshot) {
  if (snapshot.nodes.empty()) return CollectionId{0};
  const World::CollectionSnapshot::Node& rootNode = snapshot.nodes.front();
  const CollectionId rootId = rootNode.node.id;
  if (getCollectionImpl(rootId)) return CollectionId{0};   // ids alive: refuse

  u32 maxId = 0;
  for (const auto& node : snapshot.nodes)
    if (node.node.id.value > maxId) maxId = node.node.id.value;
  if (nextCollectionId_ <= maxId) nextCollectionId_ = maxId + 1;

  // All nodes first, id-preserved; internal links remapped after (a parent
  // may be created later in the same restore).
  std::unordered_map<u32, CollectionId> restoredIds;
  for (const auto& node : snapshot.nodes) {
    Collection c = node.node;
    c.id = CollectionId{nextCollectionId_++};
    collections_.push_back(std::move(c));
    restoredIds.emplace(node.node.id.value, collections_.back().id);
  }

  // Wire parents: the root keeps its captured world-parent link only while
  // that collection still exists (Blender-style silent detach otherwise);
  // descendants are subtree-internal (guaranteed by the captured walk).
  for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
    const Collection captured = snapshot.nodes[i].node;
    Collection& restored = collections_[collections_.size() -
                                         snapshot.nodes.size() + i];
    if (i == 0) {
      if (restored.parent.valid() && !getCollectionImpl(restored.parent))
        restored.parent = CollectionId{0};
      continue;
    }
    const auto pit = restoredIds.find(captured.parent.value);
    restored.parent = pit != restoredIds.end()
        ? pit->second
        : (restored.parent.valid() && getCollectionImpl(restored.parent)
             ? restored.parent
             : CollectionId{0});   // defensive: leaves exotic fallback valid
  }

  // Direct members: LIFO undo — only the destroy touched them, so restore
  // each captured member's captured membership (if the object still exists).
  for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
    const CollectionId capturedId = snapshot.nodes[i].node.id;
    for (ObjectId memberId : snapshot.nodes[i].members)
      if (Object* o = get(memberId))
        o->collection = restoredIds.at(capturedId.value);
  }
  return restoredIds.at(rootId.value);
}

// ---------------------------------------------------------------- snapshots

std::optional<ObjectSnapshot> World::captureSnapshot(ObjectId subtreeRoot) const {
  const Object* root = get(subtreeRoot);
  if (!root) return std::nullopt;
  ObjectSnapshot snap;
  snap.activeCameraBefore = activeCamera_;
  collectSubtree(subtreeRoot, snap, root->parent, 0);
  return snap;
}

void World::collectSubtree(ObjectId id, ObjectSnapshot& snap,
                           ObjectId parentLink, u32 depth) const {
  const Object* o = get(id);
  if (!o || depth >= 64) return;
  ObjectSnapshot::Node n;
  n.id = o->id;
  n.name = o->name;
  n.parent = parentLink;
  n.transform = o->transform;
  n.material = o->material;
  n.mesh = o->mesh;
  n.primitive = o->primitive;
  n.hasMesh = o->hasMesh;
  n.visible = o->visible;
  n.isLight = o->isLight;
  n.light = o->light;
  n.camera = o->camera;
  n.collection = o->collection;
  n.materials = o->materials;
  n.hasEdit = o->hasEdit;
  n.edit = o->edit;
  n.modifiers = o->modifiers;         // D40 (item 121): stack rides snapshots
  n.modifierVersion = o->modifierVersion;
  n.meshVersion = o->meshVersion;
  snap.nodes.push_back(std::move(n));
  for (ObjectId childId : o->children)
    collectSubtree(childId, snap, id, depth + 1);
}

ObjectId World::reinsert(const ObjectSnapshot& snapshot) {
  if (snapshot.nodes.empty()) return ObjectId{0};
  const ObjectSnapshot::Node& rootN = snapshot.nodes.front();

  // Only the ROOT id must be free: destroy() kept the descendants alive
  // (reparented to the grandparent), so reinsert ADOPTS them — which is why
  // undo works at all. Safe under the editor's LIFO undo: nothing but the
  // delete itself touched those descendants.
  if (has(rootN.id)) return ObjectId{0};

  // Defensive: keep the id allocator strictly above every restored id.
  u32 maxId = 0;
  for (const ObjectSnapshot::Node& n : snapshot.nodes)
    if (n.id.value > maxId) maxId = n.id.value;
  if (nextId_ <= maxId) nextId_ = maxId + 1;

  std::unordered_map<u32, std::size_t> slot;  // captured id -> objects_ index
  const auto findIndex = [&](u32 idValue) -> std::size_t {
    for (std::size_t i = 0; i < objects_.size(); ++i)
      if (objects_[i].id.value == idValue) return i;
    return static_cast<std::size_t>(-1);
  };

  for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
    const ObjectSnapshot::Node& n = snapshot.nodes[i];
    if (i > 0 && has(n.id)) {
      // adopt: reapply the captured state and detach from wherever the
      // world has this survivor now.
      const std::size_t idx = findIndex(n.id.value);
      if (idx == static_cast<std::size_t>(-1)) continue;  // unreachable
      Object& live = objects_[idx];
      if (live.parent.valid())
        if (Object* old = get(live.parent)) {
          auto& v = old->children;
          v.erase(std::remove(v.begin(), v.end(), n.id), v.end());
        }
      live.name = n.name;
      live.transform = n.transform;
      live.material = n.material;
      live.mesh = n.mesh;
      ++live.meshVersion;              // D40: the restore REPLACED `mesh`
      live.primitive = n.primitive;
      live.hasMesh = n.hasMesh;
      live.visible = n.visible;
      live.isLight = n.isLight;
      live.light = n.light;
      live.camera = n.camera;
      live.collection = n.collection;
      live.materials = n.materials;
      live.hasEdit = n.hasEdit;
      live.edit = n.edit;
      live.modifiers = n.modifiers;    // D40: stack restored with the object
      live.modifierVersion = n.modifierVersion;
      live.parent = ObjectId{0};   // re-wired below
      slot.emplace(n.id.value, idx);
    } else {
      Object o;
      o.id = n.id;
      o.name = n.name;
      o.transform = n.transform;
      o.material = n.material;
      o.mesh = n.mesh;             // exact copy of the captured geometry
      o.primitive = n.primitive;
      o.hasMesh = n.hasMesh;
      o.visible = n.visible;
      o.isLight = n.isLight;
      o.light = n.light;
      o.camera = n.camera;
      o.collection = n.collection;
      o.materials = n.materials;
      o.hasEdit = n.hasEdit;
      o.edit = n.edit;
      o.modifiers = n.modifiers;          // D40: stack restored with the object
      o.modifierVersion = n.modifierVersion;
      o.meshVersion = n.meshVersion;
      ++o.meshVersion;             // D40: the restore REPLACED `mesh`
      o.parent = ObjectId{0};      // re-wired below
      objects_.push_back(std::move(o));
      slot.emplace(n.id.value, objects_.size() - 1);
    }
  }

  // nodes[0] carries the root's world-parent link; the rest are internal.
  const ObjectId rootWorldParent = rootN.parent;
  if (rootWorldParent.valid() && has(rootWorldParent)) {
    Object* parent = get(rootWorldParent);
    objects_[slot.at(rootN.id.value)].parent = rootWorldParent;
    parent->children.push_back(rootN.id);
  }
  for (std::size_t i = 1; i < snapshot.nodes.size(); ++i) {
    const ObjectSnapshot::Node& n = snapshot.nodes[i];
    const auto pit = slot.find(n.parent.value);
    if (pit == slot.end()) continue;    // capture guarantees one hit
    Object& child = objects_[slot.at(n.id.value)];
    child.parent = ObjectId{pit->first};
    objects_[pit->second].children.push_back(child.id);
  }
  if (snapshot.activeCameraBefore.valid() && has(snapshot.activeCameraBefore))
    setActiveCamera(snapshot.activeCameraBefore);
  return rootN.id;
}

ObjectId World::duplicate(ObjectId id, std::string name) {
  const std::optional<ObjectSnapshot> snap = captureSnapshot(id);
  if (!snap || snap->nodes.empty()) return ObjectId{0};

  std::string rootName = name.empty() ? snap->nodes.front().name
                                      : std::move(name);
  uniquifyAgainst(objects_, nullptr, rootName);

  std::unordered_map<u32, ObjectId> oldToNew;
  oldToNew.reserve(snap->nodes.size());
  std::unordered_map<u32, std::size_t> indexOfNew;
  indexOfNew.reserve(snap->nodes.size());
  for (std::size_t i = 0; i < snap->nodes.size(); ++i) {
    const ObjectSnapshot::Node& n = snap->nodes[i];
    Object o;
    o.id = ObjectId{nextId_++};
    o.name = (i == 0) ? rootName : n.name;
    o.transform = n.transform;
    o.material = n.material;
    o.mesh = n.mesh;
    o.primitive = n.primitive;
    o.hasMesh = n.hasMesh;
    o.visible = n.visible;
    o.isLight = n.isLight;
    o.light = n.light;
    o.camera = n.camera;
    o.collection = n.collection;
    o.materials = n.materials;
    o.hasEdit = n.hasEdit;
    o.edit = n.edit;
    o.modifiers = n.modifiers;     // D40 (item 121): duplicates carry the stack
    o.parent = ObjectId{0};        // re-wired below
    objects_.push_back(std::move(o));
    oldToNew.emplace(n.id.value, objects_.back().id);
    indexOfNew.emplace(objects_.back().id.value, objects_.size() - 1);
  }

  const ObjectId newRoot = oldToNew.at(snap->nodes.front().id.value);
  const ObjectId sourceWorldParent = snap->nodes.front().parent;
  if (sourceWorldParent.valid() && has(sourceWorldParent)) {
    Object* parent = get(sourceWorldParent);
    Object& rootCopy = objects_[indexOfNew.at(newRoot.value)];
    rootCopy.parent = sourceWorldParent;
    parent->children.push_back(newRoot);
  }
  for (std::size_t i = 1; i < snap->nodes.size(); ++i) {
    const ObjectSnapshot::Node& n = snap->nodes[i];
    const auto pit = oldToNew.find(n.parent.value);
    if (pit == oldToNew.end()) continue;  // capture guarantees one hit
    Object& child = objects_[indexOfNew.at(oldToNew.at(n.id.value).value)];
    child.parent = pit->second;
    objects_[indexOfNew.at(pit->second.value)].children.push_back(child.id);
  }
  return newRoot;
}

// ---------------------------------------------------------------- picking

// World-space ray against one object's mesh, mapped into local space.
// Möller–Trumbore with the local direction NOT re-normalized: since
// d_local = M⁻¹·d_world, applying M gives back d_world exactly, so the local
// parameter t IS the world ray length — meaning `distance` below is in world
// units even for objects under scaled/rotated parents (D16 requirement).
static bool rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a,
                        const Vec3& b, const Vec3& c, float& outT) {
  const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
  const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
  const Vec3 p = ember::cross(d, e2);
  const float det = ember::dot(e1, p);
  if (std::fabs(det) < 1e-12f) return false;          // parallel / degenerate
  const float invDet = 1.0f / det;
  const Vec3 s{o.x - a.x, o.y - a.y, o.z - a.z};
  const float u = ember::dot(s, p) * invDet;
  // Small tolerance on both barycentric tests so a ray travelling exactly
  // along a shared edge still registers once on the first triangle tried
  // (D16: first hit wins; see the iteration-order note on PickResult).
  if (u < -1e-6f || u > 1.0f + 1e-6f) return false;
  const Vec3 q = ember::cross(s, e1);
  const float v = ember::dot(d, q) * invDet;
  if (v < -1e-6f || u + v > 1.0f + 1e-6f) return false;
  const float t = ember::dot(e2, q) * invDet;
  if (t <= 1e-6f) return false;                        // behind / at origin
  outT = t;
  return true;
}

// Decompose a shear-free T·R·S matrix (core convention: position,
// rotationEuler XYZ with R = Rz·Ry·Rx, axis scale) back into its three
// components. Used by reparentKeepWorld to rewrite a child's local
// transform; extract translation from the last column, scale from the
// norms of the rotation-axis columns, euler from the orthonormalized axes.
// Candidate for core/Math.h as a Mat4 → euler helper (told the Lead).
static void decomposeTRS(const Mat4& m, Vec3& position, Vec3& rotation,
                         Vec3& scale) {
  position = Vec3{m.m[12], m.m[13], m.m[14]};
  const float x1 = m.m[0], y1 = m.m[1], z1 = m.m[2];      // column 0
  const float x2 = m.m[4], y2 = m.m[5], z2 = m.m[6];      // column 1
  const float x3 = m.m[8], y3 = m.m[9], z3 = m.m[10];     // column 2
  const float sx = std::sqrt(x1 * x1 + y1 * y1 + z1 * z1);
  const float sy = std::sqrt(x2 * x2 + y2 * y2 + z2 * z2);
  const float sz = std::sqrt(x3 * x3 + y3 * y3 + z3 * z3);
  scale = Vec3{sx, sy, sz};

  // Euler XYZ on the orthonormalized rotation (columns normalized):
  //   col0 = (cos y cos z, cos z' sin y, -sin y)  =>
  //         beta = asin(-col0.z), gamma = atan2(col0.y, col0.x)
  //   col1.z = cos beta sin alpha, col2.z = cos beta cos alpha
  //         => alpha = atan2(col1.z, col2.z)
  const float b = std::asin(-z1 / sx);
  const float cb = std::cos(b);
  const float gTolerance = 1e-6f;
  if (cb > gTolerance) {
    rotation = Vec3{std::atan2(z2 / sy, z3 / sz),   // alpha
                    b,                              // beta
                    std::atan2(y1 / sx, x1 / sx)};  // gamma
  } else {
    // Gimbal: beta = ±90°, gamma pinned to 0 (documented convention),
    // alpha recovered from the remaining plane.
    rotation = Vec3{std::atan2(x3 / sz, -x2 / sy), b, 0.0f};
  }
}

std::optional<PickResult> World::pick(const ember::Ray& ray) const {
  std::optional<PickResult> hit;
  float bestT = std::numeric_limits<float>::infinity();

  for (const Object& o : objects_) {
    if (!o.hasMesh || !emitsOnScreen(o)) continue;      // D16 + D22 rule 3
    if (o.mesh.indexCount() < 3 || o.mesh.positions.size() < 3) continue;

    // Ray into local space (D16: inverse(worldMatrix), mesh untouched).
    const Mat4 world = worldMatrix(o.id);
    const Mat4 inv = ember::inverse(world);
    const Vec3 originLocal = ember::transformPoint(inv, ray.origin);
    const Vec3 dirLocal = ember::transformDirection(inv, ray.direction);

    // D40: object-mode picking uses the EVALUATED mesh (what you see is
    // what you click); an empty stack (or an evaluation failure) falls back
    // to the base mesh. The evaluated cache is shared with buildPacket().
    u32 pickFailure = 0xFFFFFFFFu;
    const MeshData& src = !o.modifiers.empty()
        ? evaluatedFor(o, pickFailure) : o.mesh;
    const auto& pos = src.positions;
    const auto& idx = src.indices;
    for (u32 i = 0; i + 2 < src.indexCount(); i += 3) {
      const u32 ia = idx[i] * 3, ib = idx[i + 1] * 3, ic = idx[i + 2] * 3;
      if (ib + 2 >= pos.size() || ic + 2 >= pos.size()) break;  // corrupt indices
      const Vec3 a{pos[ia], pos[ia + 1], pos[ia + 2]};
      const Vec3 b{pos[ib], pos[ib + 1], pos[ib + 2]};
      const Vec3 c{pos[ic], pos[ic + 1], pos[ic + 2]};
      float t = 0.0f;
      if (rayTriangle(originLocal, dirLocal, a, b, c, t) && t < bestT) {
        bestT = t;
        hit = PickResult{o.id, t, Vec3{0, 0, 0}};
      }
    }
  }

  if (hit) hit->worldPoint = ray.pointAt(hit->distance);
  return hit;
}

// ------------------------------------------------- transform helpers (66)
//
// Matrix conventions come from core: column-major, R = Rz·Ry·Rx (XYZ euler),
// Transform::toMatrix() = T·R·S. Every helper here is a pure function over
// (worldMatrix, toMatrix, inverse) from core — no scene-local maths.

// Shared: the object's/world matrix's rotation basis (columns normalized,
// scale and translation stripped) — D20's "orientation" definition, reused
// by the gizmo basis, the parented rotations and the decomposition paths.
static Mat4 scaleStrippedRotation(const Mat4& m) {
  Mat4 basis;
  const Vec3 c0 = ember::normalize(Vec3{m.m[0], m.m[1], m.m[2]});
  const Vec3 c1 = ember::normalize(Vec3{m.m[4], m.m[5], m.m[6]});
  const Vec3 c2 = ember::normalize(Vec3{m.m[8], m.m[9], m.m[10]});
  basis.m[0] = c0.x; basis.m[1] = c0.y; basis.m[2] = c0.z;
  basis.m[4] = c1.x; basis.m[5] = c1.y; basis.m[6] = c1.z;
  basis.m[8] = c2.x; basis.m[9] = c2.y; basis.m[10] = c2.z;
  return basis;
}

Vec3 World::resolveWorldAxis(ObjectId object, int axis, bool localSpace) const {
  static const Vec3 kAxes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
  if (axis < 0 || axis > 2) return Vec3{0, 0, 0};
  if (!localSpace) return kAxes[axis];           // Global: the axis verbatim

  // Local: the object's world ROTATION applied (never scale — see header).
  const Object* o = get(object);
  if (!o) return kAxes[axis];
  const Mat4 world = worldMatrix(object);
  // Strip scale+translation: normalize the rotation columns.
  Vec3 dir = ember::transformDirection(world, kAxes[axis]);
  return ember::normalize(dir);
}

Vec3 World::worldAxisInParentFrame(ObjectId object, const Vec3& worldAxis) const {
  const Object* o = get(object);
  if (!o) return worldAxis;
  // setTransform composes in the PARENT frame: world = parentWorld * local.
  // The correct DELTA mapping is the full inverse LINEAR part:
  //   d_local = S_p⁻¹ · R_pᵀ · d_world
  // (transformDirection of inverse(parentWorld)) — the parent's scale MUST
  // divide here: a 1-world-unit drag under a 2x-scaled parent is 0.5 local
  // units, which is exactly what makes the world position move by 1 unit.
  // Callers wanting a pure DIRECTION normalize the result themselves.
  const Mat4 parentWorld = o->parent.valid() ? worldMatrix(o->parent)
                                             : Mat4{};  // identity = root
  return ember::transformDirection(ember::inverse(parentWorld), worldAxis);
}

Transform World::composeWorldTranslate(ObjectId object, const Vec3& worldDelta) const {
  Transform t;
  const Object* o = get(object);
  if (!o) return t;
  t = o->transform;
  // Translation commutes with rotation/scale: the world delta lands in the
  // object's local translation verbatim when there is no parent; with one,
  // the parent's scale must not stretch it (length-preserving mapping).
  if (!o->parent.valid()) {
    t.position = t.position + worldDelta;
    return t;
  }
  const Vec3 localDelta = worldAxisInParentFrame(object, worldDelta);
  t.position = t.position + localDelta;
  return t;
}

const float kDegreesToRadians = 0.017453292519943295f;

Transform World::composeWorldRotatePivot(ObjectId object,
                                         const Transform& start,
                                         const Vec3& worldAxis, float radians,
                                         const Vec3& pivotWorld) const {
  Transform r = start;
  const Object* o = get(object);
  if (!o) return r;
  const Mat4 R = ember::rotationAxis(ember::normalize(worldAxis), radians);
  const Mat4 parentWorld = o->parent.valid() ? worldMatrix(o->parent)
                                             : Mat4{};   // identity = root

  // POSITION: per-point world rotate about the pivot, then map back through
  // the parent — exact for every chain (no decomposition involved).
  const Vec3 startWorldPos = ember::transformPoint(parentWorld, r.position);
  const Vec3 newWorldPos = pivotWorld + ember::transformPoint(R, startWorldPos - pivotWorld);
  r.position = ember::transformPoint(ember::inverse(parentWorld), newWorldPos);

  // ROTATION: parent-R similarity transform; exact when the chain is
  // shear-free (uniform ancestor scales), documented best-effort sheared.
  const Mat4 newRot = ember::inverse(scaleStrippedRotation(parentWorld)) * R *
                      scaleStrippedRotation(parentWorld) *
                      ember::rotationEuler(r.rotation);
  Vec3 pos, rot, scl;
  decomposeTRS(newRot, pos, rot, scl);
  r.rotation = rot;   // scale UNTOUCHED — Blender's rotate-leaves-scale rule
  return r;
}

Transform World::composeWorldRotate(ObjectId object, const Vec3& worldAxis,
                                    float radians) const {
  const Object* o = get(object);
  if (!o) return Transform{};
  // Pivotless modal form = pivot at the object's own world origin, applied
  // to the LIVE transform (the gesture-drag path; typed entries use the
  // `start`-taking variants).
  return composeWorldRotatePivot(object, o->transform, worldAxis, radians,
                                 ember::transformPoint(worldMatrix(o->parent.valid()
                                                       ? o->parent : ObjectId{}),
                                                       o->transform.position));
}

Transform World::typedTranslate(ObjectId object, const Transform& start,
                                const Vec3& worldAxis, float metres) const {
  Transform r = start;
  const Object* o = get(object);
  if (!o) return r;
  const Vec3 axis = ember::normalize(worldAxis);
  // ABSOLUTE entry (Blender): the world position becomes the START state's
  // world position displaced by exactly `metres` along the axis. Solve the
  // local translation directly — no decomposition, exact for every chain.
  const Mat4 parentWorld = o->parent.valid() ? worldMatrix(o->parent) : Mat4{};
  const Vec3 startWorldPos = ember::transformPoint(parentWorld, r.position);
  const Vec3 targetWorldPos = startWorldPos + axis * metres;
  r.position = ember::transformPoint(ember::inverse(parentWorld),
                                     targetWorldPos);
  return r;
}

Transform World::typedRotate(ObjectId object, const Transform& start,
                             const Vec3& worldAxis, float degrees,
                             const Vec3& pivotWorld) const {
  // Absolute angle from START, in degrees (Blender's modal unit) — TYPING
  // REPLACES the running magnitude (pure: the editor sets the returned
  // Transform, discarding any running drag delta).
  return composeWorldRotatePivot(object, start, worldAxis,
                                 degrees * kDegreesToRadians, pivotWorld);
}

Transform World::typedScale(ObjectId object, const Transform& start,
                            const Vec3& ratio, const Vec3& pivotWorld,
                            bool localSpace) const {
  // Absolute RATIO from the START state's scale (Blender's modal unit) —
  // typing REPLACES the running factor: the returned scale is
  // start.scale * ratio, applied about the editor's resolved pivot. The
  // compose helper owns the space/exactness rules (uniform exact for every
  // chain; per-axis best-effort sheared) and the per-point position math.
  return composeWorldScalePivot(object, start, ratio, pivotWorld, localSpace);
}

Transform World::composeWorldScalePivot(ObjectId object,
                                        const Transform& start,
                                        const Vec3& ratio,
                                        const Vec3& pivotWorld,
                                        bool localSpace) const {
  Transform r = start;
  const Object* o = get(object);
  if (!o) return r;
  const Mat4 parentWorld = o->parent.valid() ? worldMatrix(o->parent) : Mat4{};

  // Scale component:
  //  * Local space: componentwise multiply in the object's own axes.
  //  * Global uniform: same (uniform commutes; the axis choice does not
  //    matter).
  //  * Global per-axis: best-effort world-stretch decomposition (header).
  const bool uniform =
    std::fabs(ratio.x - ratio.y) < 1e-9f &&
    std::fabs(ratio.y - ratio.z) < 1e-9f;
  if (localSpace || uniform) {
    r.scale = Vec3{r.scale.x * ratio.x, r.scale.y * ratio.y,
                   r.scale.z * ratio.z};
  } else {
    // Reuse the item-66 global per-axis page: divide theCURRENT world
    // columns (via START's own world state) and multiply.
    const Mat4 worldOfStart =
      parentWorld * start.toMatrix();
    const float sx = ember::length(Vec3{worldOfStart.m[0], worldOfStart.m[1],
                                        worldOfStart.m[2]});
    const float sy = ember::length(Vec3{worldOfStart.m[4], worldOfStart.m[5],
                                        worldOfStart.m[6]});
    const float sz = ember::length(Vec3{worldOfStart.m[8], worldOfStart.m[9],
                                        worldOfStart.m[10]});
    Vec3 localRatio = ratio;
    if (sx > 1e-6f) localRatio.x /= sx;
    if (sy > 1e-6f) localRatio.y /= sy;
    if (sz > 1e-6f) localRatio.z /= sz;
    r.scale = Vec3{r.scale.x * localRatio.x, r.scale.y * localRatio.y,
                   r.scale.z * localRatio.z};
  }

  // POSITION: per-point stretch about the pivot — EXACT for every chain.
  // Per-axis Local stretches are oriented by the object's own rotation
  // (Blender: the ratio is applied in the object's axes); uniform/global
  // stretches are axis-aligned in world.
  Vec3 stretchAxis = ratio;
  if (localSpace && !uniform) {
    const Mat4 orheld = scaleStrippedRotation(
      parentWorld * start.toMatrix());
    // Oriented stretch: S_world = R * diag(r) * R^T, applied per point.
    const Vec3 startWorldPos = ember::transformPoint(parentWorld, r.position);
    const Vec3 rel = startWorldPos - pivotWorld;
    const Vec3 a{ember::dot(Vec3{orheld.m[0], orheld.m[1], orheld.m[2]}, rel),
                 ember::dot(Vec3{orheld.m[4], orheld.m[5], orheld.m[6]}, rel),
                 ember::dot(Vec3{orheld.m[8], orheld.m[9], orheld.m[10]}, rel)};
    const Vec3 stretched = Vec3{a.x * ratio.x, a.y * ratio.y, a.z * ratio.z};
    const Vec3 out = ember::transformDirection(orheld, stretched);
    const Vec3 newWorldPos = pivotWorld + out;
    r.position = ember::transformPoint(ember::inverse(parentWorld),
                                       newWorldPos);
  } else {
    const Vec3 startWorldPos = ember::transformPoint(parentWorld, r.position);
    const Vec3 rel = startWorldPos - pivotWorld;
    const Vec3 newWorldPos = pivotWorld + Vec3{rel.x * ratio.x,
                                               rel.y * ratio.y,
                                               rel.z * ratio.z};
    r.position = ember::transformPoint(ember::inverse(parentWorld),
                                       newWorldPos);
  }
  return r;
}

Transform World::composeWorldScale(ObjectId object, const Vec3& worldRatio,
                                   bool localSpace) const {
  const Object* o = get(object);
  if (!o) return Transform{};
  // Pivotless modal form = scale about the object's own world origin,
  // applied to the LIVE transform (the item-66 contract unchanged).
  return composeWorldScalePivot(object, o->transform, worldRatio,
                                ember::transformPoint(
                                  worldMatrix(o->parent.valid()
                                              ? o->parent : ObjectId{}),
                                  o->transform.position),
                                localSpace);
}

void World::setUnitScale(float metres) {
  if (metres > 1e-5f) unitScale_ = metres;
}

// ---------------------------------------------------------------- edit ops

// D34 Object-storage rule fulfilled: the FIRST edit converts `mesh` into
// `edit` (lazily), after which `edit` is the source of truth and `mesh` is
// regenerated on every mutation. Returns nullptr for unknown objects.
static EditableMesh* ensureEditObjectMesh(Object* o) {
  if (!o) return nullptr;
  if (!o->hasEdit) {
    o->edit = EditableMesh::fromMeshData(o->mesh);
    o->hasEdit = true;
  }
  return &o->edit;
}

EditableMesh* World::editable(ObjectId object) {
  return ensureEditObjectMesh(get(object));
}

// The world mesh keeps its identity: regenerated from the source of truth
// after every mutation, keeping the previous name string.
static void syncMeshFromEdit(Object* o) {
  const std::string name = o->mesh.name;
  o->mesh = o->edit.toMeshData();
  ++o->meshVersion;   // D40: the sync REPLACED `mesh` (cache soundness)
  o->mesh.name = name;
}

bool World::editMoveVertices(ObjectId object,
                             const std::vector<u32>& vertexIds,
                             const Vec3& worldDelta) {
  Object* o = get(object);
  EditableMesh* em = ensureEditObjectMesh(o);
  if (!em) return false;
  // Map the world delta into the mesh's own frame (items 66/101 mapping):
  // worldDelta through inv(world) is exact for translation under any
  // object transform (S^-1 R^T; parent chain folded in by the matrix).
  const Vec3 localDelta =
      ember::transformDirection(ember::inverse(worldMatrix(object)),
                                worldDelta);
  if (!em->moveVertices(vertexIds, localDelta)) return false;
  syncMeshFromEdit(o);
  return true;
}

bool World::editExtrudeFace(ObjectId object, u32 faceIndex,
                            const Vec3& worldOffset) {
  Object* o = get(object);
  EditableMesh* em = ensureEditObjectMesh(o);
  if (!em) return false;
  const Vec3 localOffset =
      ember::transformDirection(ember::inverse(worldMatrix(object)),
                                worldOffset);
  if (!em->extrudeFace(faceIndex, localOffset)) return false;  syncMeshFromEdit(o);
  editClearEditSelections(object);   // bumped version: selections cleared
  return true;
}

bool World::editDeleteSubElements(ObjectId object, SelectMode mode,
                                  const std::vector<u32>& ids) {
  Object* o = get(object);
  EditableMesh* em = ensureEditObjectMesh(o);
  if (!em) return false;
  if (ids.empty() || ids.size() > em->positions.size() + em->edges.size() + em->faces.size())
    return false;   // nonsense input: refuse rather than corrupt
  bool ok = false;
  switch (mode) {
  case SelectMode::Vertex: ok = em->deleteVertices(ids); break;
  case SelectMode::Edge:   ok = em->deleteEdges(ids);     break;
  case SelectMode::Face:   ok = em->deleteFaces(ids);     break;
  }
  if (!ok) return false;
  syncMeshFromEdit(o);
  editClearEditSelections(object);   // bumped version: selections cleared
  return true;
}

bool World::editMergeVertices(ObjectId object,
                              const std::vector<u32>& vertexIds,
                              const Vec3& worldPoint) {
  Object* o = get(object);
  EditableMesh* em = ensureEditObjectMesh(o);
  if (!em) return false;
  // World point -> mesh-local point (full point transform, not a direction).
  const Vec3 localPoint =
      ember::transformPoint(ember::inverse(worldMatrix(object)), worldPoint);
  if (!em->mergeVertices(vertexIds, localPoint)) return false;
  syncMeshFromEdit(o);
  editClearEditSelections(object);   // bumped version: selections cleared
  return true;
}

// ------------------------------- sub-element selection (D34 rule) ---------

static u64 selectionKey(ObjectId object, SelectMode mode) {
  return (static_cast<u64>(object.value) << 2) | static_cast<u64>(mode);
}

// Mesh-local effective version: the edit's stamp when it exists; for a
// not-yet-converted mesh the FIRST edit conversion produces version 0
// (fromMeshData leaves the default), so 0 is the consistent stamp.
static u32 effectiveTopologyVersion(const Object* o) {
  return o != nullptr && o->hasEdit ? o->edit.topologyVersion : 0u;
}

void World::editSetSelection(ObjectId object, SelectMode mode,
                             const std::vector<u32>& indices) {
  const Object* o = get(object);
  if (!o) return;
  ember::scene::MeshSelection sel;
  sel.mode = mode;
  sel.topologyVersion = effectiveTopologyVersion(o);
  sel.indices = indices;
  editSelections_[selectionKey(object, mode)] = std::move(sel);
}

const ember::scene::MeshSelection*
World::editReadSelection(ObjectId object, SelectMode mode) const {
  const auto it = editSelections_.find(selectionKey(object, mode));
  if (it == editSelections_.end()) return nullptr;
  const Object* o = get(object);
  if (it->second.topologyVersion != effectiveTopologyVersion(o))
    return nullptr;   // stale: the D34 rule resolved as invalid
  return &it->second;
}

void World::editClearEditSelections(ObjectId object) {
  const u64 base = static_cast<u64>(object.value) << 2;
  editSelections_.erase(base + static_cast<u64>(SelectMode::Vertex));
  editSelections_.erase(base + static_cast<u64>(SelectMode::Edge));
  editSelections_.erase(base + static_cast<u64>(SelectMode::Face));
}

const EditableMesh* World::readableEdit(ObjectId object) const {
  const Object* o = get(object);
  return o != nullptr && o->hasEdit ? &o->edit : nullptr;
}

std::vector<ember::Vec3>
World::editCaptureVertexPositions(ObjectId object,
                                  const std::vector<u32>& vertexIds) const {
  std::vector<ember::Vec3> out;
  const Object* o = get(object);
  if (!o) return out;
  if (o->hasEdit) {
    for (const u32 v : vertexIds)
      if (v < o->edit.positions.size()) out.push_back(o->edit.positions[v]);
  } else {
    for (const u32 v : vertexIds)
      if (v * 3 + 2 < o->mesh.positions.size())
        out.push_back(Vec3{o->mesh.positions[v * 3],
                           o->mesh.positions[v * 3 + 1],
                           o->mesh.positions[v * 3 + 2]});
  }
  return out;
}

bool World::editApplyVertexPositions(ObjectId object,
                                     const std::vector<u32>& vertexIds,
                                     const std::vector<ember::Vec3>& positions) {
  Object* o = get(object);
  EditableMesh* em = ensureEditObjectMesh(o);
  if (!em) return false;
  if (vertexIds.size() != positions.size()) return false;
  // Item 128: the absolute batch set lives ON the EditableMesh (all-or-
  // nothing, no version bump); the world wrapper maps nothing and simply
  // regenerates the render form after the exact write.
  if (!em->setVertexPositions(vertexIds, positions)) return false;
  syncMeshFromEdit(o);
  return true;
}

Mat4 World::gizmoOrientation(ObjectId object, bool localSpace) const {
  if (!localSpace) return Mat4{};                    // Global: identity (D20)
  const Object* o = get(object);
  if (!o) return Mat4{};
  return scaleStrippedRotation(worldMatrix(object));
}

// ---------------------------------------------------------------- bounds

static bool meshExtent(const MeshData& mesh, float outMin[3], float outMax[3]) {
  if (mesh.positions.size() < 3) return false;
  for (int k = 0; k < 3; ++k) {
    outMin[k] =  std::numeric_limits<float>::infinity();
    outMax[k] = -std::numeric_limits<float>::infinity();
  }
  for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
    for (int k = 0; k < 3; ++k) {
      const float v = mesh.positions[i + static_cast<std::size_t>(k)];
      if (v < outMin[k]) outMin[k] = v;
      if (v > outMax[k]) outMax[k] = v;
    }
  return true;
}

bool World::worldAABB(ObjectId id, float outMin[3], float outMax[3]) const {
  const Object* o = get(id);
  if (!o || !o->hasMesh) return false;
  float localMin[3], localMax[3];
  if (!meshExtent(o->mesh, localMin, localMax)) return false;

  for (int k = 0; k < 3; ++k) {
    outMin[k] =  std::numeric_limits<float>::infinity();
    outMax[k] = -std::numeric_limits<float>::infinity();
  }
  const Mat4 world = worldMatrix(id);
  for (int corner = 0; corner < 8; ++corner) {
    const Vec3 c = transformPoint(world,
      Vec3{(corner & 1) ? localMax[0] : localMin[0],
           (corner & 2) ? localMax[1] : localMin[1],
           (corner & 4) ? localMax[2] : localMin[2]});
    const float p[3] = {c.x, c.y, c.z};
    for (int k = 0; k < 3; ++k) {
      if (p[k] < outMin[k]) outMin[k] = p[k];
      if (p[k] > outMax[k]) outMax[k] = p[k];
    }
  }
  return true;
}

// Backlog 5/11 shared accumulation: union AABB over the qualifying subset.
std::optional<WorldBounds> World::boundsImpl(const std::vector<ObjectId>& subset,
                                             bool subsetIsAll) const {
  bool any = false;
  float min[3], max[3];
  for (int k = 0; k < 3; ++k) {
    min[k] =  std::numeric_limits<float>::infinity();
    max[k] = -std::numeric_limits<float>::infinity();
  }
  if (subsetIsAll) {
    for (const Object& o : objects_) {
      if (!o.visible || !o.hasMesh) continue;     // on-screen objects only
      if (!emitsOnScreen(o)) continue;            // D22 rule 3 (collection eye)
      float omin[3], omax[3];
      if (!worldAABB(o.id, omin, omax)) continue;
      any = true;
      for (int k = 0; k < 3; ++k) {
        if (omin[k] < min[k]) min[k] = omin[k];
        if (omax[k] > max[k]) max[k] = omax[k];
      }
    }
  } else {
    for (ObjectId id : subset) {
      const Object* o = get(id);
      if (!o || !o->visible || !o->hasMesh) continue;
      float omin[3], omax[3];
      if (!worldAABB(id, omin, omax)) continue;
      any = true;
      for (int k = 0; k < 3; ++k) {
        if (omin[k] < min[k]) min[k] = omin[k];
        if (omax[k] > max[k]) max[k] = omax[k];
      }
    }
  }
  if (!any) return std::nullopt;

  WorldBounds b;
  b.center = Vec3{(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f,
                  (min[2] + max[2]) * 0.5f};
  // Radius: half-diagonal of the union AABB; slight over-approx is fine for a
  // camera fit, and the floor keeps a point-like object from degenerating.
  Vec3 edge{max[0] - min[0], max[1] - min[1], max[2] - min[2]};
  b.radius = 0.5f * ember::length(edge);
  if (b.radius < 0.25f) b.radius = 0.25f;
  return b;
}

std::optional<WorldBounds> World::bounds() const {
  return boundsImpl({}, true);
}

std::optional<WorldBounds> World::selectionBounds() const {
  return boundsImpl(selection_, false);
}

std::vector<ObjectId> World::pickBox(const Vec3& boxMin, const Vec3& boxMax) const {  std::vector<ObjectId> out;
  float qmin[3] = {boxMin.x, boxMin.y, boxMin.z};
  float qmax[3] = {boxMax.x, boxMax.y, boxMax.z};
  for (int k = 0; k < 3; ++k)
    if (qmin[k] > qmax[k]) std::swap(qmin[k], qmax[k]);   // tolerate swapped input

  for (const Object& o : objects_) {
    if (!o.visible || !o.hasMesh) continue;               // D16/D19 skip rule
    if (!emitsOnScreen(o)) continue;                      // D22 rule 3
    float omin[3], omax[3];
    if (!worldAABB(o.id, omin, omax)) continue;
    bool overlap = true;
    for (int k = 0; k < 3; ++k)
      if (omax[k] < qmin[k] || omin[k] > qmax[k]) { overlap = false; break; }
    if (overlap) out.push_back(o.id);
  }
  return out;
}

// ---------------------------------------------------------------- reparent

void World::reparentKeepWorld(ObjectId childId, ObjectId newParentId) {
  Object* child = get(childId);
  if (!child) return;
  if (newParentId.valid()) {
    if (!get(newParentId)) return;                                  // unknown
    if (newParentId.value == childId.value) return;                  // self
    if (isDescendant(newParentId, childId)) return;  // cycle, same as setParent
  }

  // Preserve the world matrix: new_local = inv(parentWorld) * childWorld.
  const Mat4 childWorld = worldMatrix(childId);
  const Mat4 parentWorld = newParentId.valid() ? worldMatrix(newParentId)
                                               : Mat4{};  // identity = detach
  const Mat4 local = ember::inverse(parentWorld) * childWorld;
  decomposeTRS(local, child->transform.position, child->transform.rotation,
               child->transform.scale);

  // Bookkeeping identical to setParent().
  if (child->parent.valid())
    if (Object* old = get(child->parent)) {
      auto& v = old->children;
      v.erase(std::remove(v.begin(), v.end(), childId), v.end());
    }
  child->parent = newParentId;
  if (newParentId.valid())
    if (Object* parent = get(newParentId))
      if (std::find(parent->children.begin(), parent->children.end(),
                    childId) == parent->children.end())
        parent->children.push_back(childId);
}


// ------------------------------- modifier stack (D40, item 121) ------------

const std::vector<ember::Modifier>& World::modifiers(ObjectId id) const {
  static const std::vector<ember::Modifier> none;
  const Object* o = getImpl(id);
  return o != nullptr ? o->modifiers : none;
}

bool World::addModifier(ObjectId id, ember::ModifierKind kind) {
  Object* o = get(id);
  if (!o || !o->hasMesh) return false;       // nothing to evaluate under it
  ember::Modifier m;
  // Default-construct the parameters for the requested kind (the variant
  // index IS the kind — D40 — so a switch here is the only place the three
  // defaults are named).
  switch (kind) {
    case ember::ModifierKind::Subdivision:
      m.params = ember::SubdivisionParams{}; break;
    case ember::ModifierKind::Mirror:
      m.params = ember::MirrorParams{}; break;
    case ember::ModifierKind::Array:
      m.params = ember::ArrayParams{}; break;
  }
  m.enabled = true;
  o->modifiers.push_back(std::move(m));
  ++o->modifierVersion;                      // D40: ANY stack mutation bumps
  return true;
}

bool World::removeModifier(ObjectId id, u32 index) {
  Object* o = get(id);
  if (!o || index >= o->modifiers.size()) return false;
  o->modifiers.erase(o->modifiers.begin() + index);
  ++o->modifierVersion;
  return true;
}

bool World::moveModifier(ObjectId id, u32 from, u32 to) {
  Object* o = get(id);
  if (!o) return false;
  if (from >= o->modifiers.size() || to >= o->modifiers.size() || from == to)
    return false;
  ember::Modifier m = o->modifiers[from];
  o->modifiers.erase(o->modifiers.begin() + from);
  o->modifiers.insert(o->modifiers.begin() + to, std::move(m));
  ++o->modifierVersion;
  return true;
}

bool World::setModifierEnabled(ObjectId id, u32 index, bool enabled) {
  Object* o = get(id);
  if (!o || index >= o->modifiers.size()) return false;
  if (o->modifiers[index].enabled == enabled) return true;  // no observable change
  o->modifiers[index].enabled = enabled;
  ++o->modifierVersion;
  return true;
}

bool World::setModifierParams(ObjectId id, u32 index,
                              const ember::ModifierParams& params) {
  Object* o = get(id);
  if (!o || index >= o->modifiers.size()) return false;
  // The variant index IS the kind (D40): an entry cannot hold foreign params.
  if (o->modifiers[index].kind() !=
      static_cast<ember::ModifierKind>(params.index()))
    return false;
  o->modifiers[index].params = params;
  ++o->modifierVersion;
  return true;
}

bool World::setModifierStack(ObjectId id,
                             const std::vector<ember::Modifier>& stack) {
  Object* o = get(id);
  if (!o) return false;
  o->modifiers = stack;
  ++o->modifierVersion;
  return true;
}

u32 World::modifierStackVersion(ObjectId id) const {
  const Object* o = getImpl(id);
  return o != nullptr ? o->modifierVersion : 0u;
}

u32 World::evaluatedFailureIndex(ObjectId id) const {
  const Object* o = getImpl(id);
  if (!o || o->modifiers.empty()) return 0xFFFFFFFFu;
  const auto it = evalCache_.find(id.value);
  if (it == evalCache_.end()) return 0xFFFFFFFFu;
  const EvalCacheEntry& e = it->second;
  // A cached failure is only meaningful while it matches the object NOW.
  if (e.id != id || e.meshVersion != o->meshVersion ||
      e.modifierVersion != o->modifierVersion)
    return 0xFFFFFFFFu;
  return e.failedIndex;
}

// Cache lookup / evaluation shared by buildPacket() and pick(). Returns a
// reference into the mutable cache — unordered_map references survive
// rehash, so the packet's mesh pointer stays valid for the frame (D9).
const ember::MeshData& World::evaluatedFor(const Object& o,
                                           u32& failedIndex) const {
  failedIndex = 0xFFFFFFFFu;
  if (o.modifiers.empty()) return o.mesh;
  const auto it = evalCache_.find(o.id.value);
  if (it != evalCache_.end() && it->second.id == o.id &&
      it->second.meshVersion == o.meshVersion &&
      it->second.modifierVersion == o.modifierVersion) {
    failedIndex = it->second.failedIndex;
    return it->second.mesh;
  }
  EvalCacheEntry& slot = evalCache_[o.id.value];
  slot.id = o.id;
  slot.meshVersion = o.meshVersion;
  slot.modifierVersion = o.modifierVersion;
  slot.failedIndex = 0xFFFFFFFFu;
  // The BASE mesh: `mesh` is always the regenerated render form of `edit`
  // (D34's sync rule), so evaluation reads EXACTLY ONE source — o.mesh —
  // and the hasEdit/baseSerial question collapses into meshVersion, which
  // every mutator that replaces `mesh` bumps (D40's rule; item 123 tests).
  slot.mesh = evaluateMeshStack(o.mesh, o.modifiers, &slot.failedIndex);
  failedIndex = slot.failedIndex;
  return slot.mesh;
}

// ---------------------------------------------------------------- selection

void World::select(ObjectId id, bool additive) {
  if (!has(id)) return;
  if (!additive) {
    selection_.clear();
    selection_.push_back(id);
    return;
  }
  const auto it = std::find(selection_.begin(), selection_.end(), id);
  if (it != selection_.end()) selection_.erase(it);
  else selection_.push_back(id);
}

void World::deselect(ObjectId id) {
  const auto it = std::find(selection_.begin(), selection_.end(), id);
  if (it != selection_.end()) selection_.erase(it);
}

// ---------------------------------------------------------------- packet

RenderPacket World::buildPacket(int width, int height,
                                bool suspendModifierStack) const {
  RenderPacket packet;

  // D2/D3/D9/D19: the camera is NOT ours (App fills view/projection/
  // cameraPosition from render::OrbitCamera). Width/height, clearColor, scene
  // render settings and lights go with the packet.
  packet.width = width;
  packet.height = height;
  packet.background = background_;   // D22(d): clears the frame (D22 wording)
  packet.assetBaseDirectory = assetBaseDirectory_;
  packet.renderSettings = renderSettings_;
  packet.clearColor = background_.color;  // deprecated staging: Halide may
                                          // still read clearColor until he
                                          // swaps to `background` (same rule
                                          // as packet.light); core default
                                          // colour otherwise.

  // Light resolution (D19 + the completed staging): creation order over light
  // objects; a directional resolves to normalize(world * (0,0,-1)) — its aim —
  // a point to its world translation. The deprecated packet.light staging is
  // GONE: render consumes packet.lights (D19 deletion condition met), so the
  // list is the single source and its ORDER is the contract (also the meaning
  // of packet.shadowLightIndex — D22: the ONE casting light, resolved here to
  // the FIRST directional in creation order, or the -1 default when the
  // world has no directional).
  int lightIndexCount = 0;
  int firstDirectionalIndex = -1;
  for (const Object& o : objects_) {
    if (!o.isLight) continue;
    ember::LightInstance inst;
    inst.type = o.light.type;
    inst.color = o.light.color;
    inst.intensity = o.light.intensity;
    inst.radius = o.light.radius;
    if (o.light.type == ember::LightType::Directional) {
      inst.direction = ember::normalize(
        ember::transformDirection(worldMatrix(o.id), Vec3{0, 0, -1}));
      if (firstDirectionalIndex < 0)
        firstDirectionalIndex = lightIndexCount;
    } else {
      inst.position = ember::transformPoint(worldMatrix(o.id), Vec3{0, 0, 0});
    }
    packet.lights.push_back(std::move(inst));
    ++lightIndexCount;
  }
  // D22: one casting light per frame — the first directional (or -1).
  packet.shadowLightIndex = firstDirectionalIndex;

  for (const Object& o : objects_) {
    if (!o.hasMesh || !emitsOnScreen(o)) continue;      // D16 + D22 rule 3
    DrawItem item;
    item.object = o.id;
    item.model = worldMatrix(o.id);
    item.geometryVersion = o.meshVersion;
    item.modifierVersion = o.modifierVersion;
    item.hasGeometryVersion = true;
    // D40: a non-empty stack is EVALUATED here — the DrawItem points into
    // World-owned evaluated storage, valid for the frame (D9). An empty
    // stack keeps the previous bit-exact path (pointer into o.mesh). Edit-
    // mode suspension (the App passes the editor's mode) draws the BASE.
    const MeshData* drawMesh = &o.mesh;
    u32 modifierFailureIndex = 0xFFFFFFFFu;
    if (!suspendModifierStack && !o.modifiers.empty()) {
      drawMesh = &evaluatedFor(o, modifierFailureIndex);  // failure -> last good mesh
    }
    item.mesh = const_cast<MeshData*>(drawMesh);  // packet is read-only by API
    item.material = o.material;
    // D22(c): the slot array travels alongside the legacy slot-0 field, both
    // valid for the frame (D9). materialCount >= 1 always (slot 0 exists).
    item.materials = o.materials.data();
    item.materialCount = static_cast<u32>(o.materials.size());
    const u32 triangleCount = static_cast<u32>(drawMesh->indexCount() / 3);
    std::vector<u32> triangleSlots = baseTriangleMaterialSlots(
        o, static_cast<u32>(o.mesh.indexCount() / 3));
    if (drawMesh != &o.mesh)
      applyModifierMaterialMapping(o.modifiers, modifierFailureIndex, triangleSlots);
    triangleSlots.resize(triangleCount, 0u);  // wholly generated triangles default to slot 0
    item.triangleMaterialSlots =
        cachedTriangleMaterialSlots(o.id, std::move(triangleSlots));
    item.triangleMaterialSlotCount = triangleCount;
    item.selected = std::find(selection_.begin(), selection_.end(), o.id) !=
                    selection_.end();
    item.wireframe = false;
    packet.items.push_back(std::move(item));
  }
  return packet;
}

} // namespace ember::scene
