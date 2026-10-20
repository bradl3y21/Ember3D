// tests/test_scene_undo.cpp — owned by Verity · QA.
//
// Contract under test: src/scene/Scene.h's editor-support surface (M5 prep):
//   std::optional<ObjectSnapshot> World::captureSnapshot(ObjectId) const;
//   ObjectId World::reinsert(const ObjectSnapshot&);
//   ObjectId World::duplicate(ObjectId, std::string name = {});
// plus the documented destroy() survivor-consistency guarantee
// ("reparents the object's children to its parent first, then destroys it").
//
// These are the primitives Chisel's M5 undo/redo stack is built on. A defect
// here is invisible until M5 UI exercises it, by which time the root cause is
// far from the code — so they are pinned now, against the PUBLIC API only.
//
// Pure data: no GPU, no GL context, no window.
#include "harness.h"

#include "scene/Scene.h"

#include <cmath>
#include <string>
#include <vector>

using ember::Color;
using ember::MaterialDesc;
using ember::Mat4;
using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
using ember::scene::Object;
using ember::scene::ObjectSnapshot;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

bool vecNear(Vec3 a, Vec3 b, float eps = 1e-6f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-6f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

bool transformEqual(const Transform& a, const Transform& b) {
    return vecNear(a.position, b.position) && vecNear(a.rotation, b.rotation) &&
           vecNear(a.scale, b.scale);
}

bool materialEqual(const MaterialDesc& a, const MaterialDesc& b) {
    return a.baseColor.r == b.baseColor.r && a.baseColor.g == b.baseColor.g &&
           a.baseColor.b == b.baseColor.b && a.baseColor.a == b.baseColor.a &&
           a.metallic == b.metallic && a.roughness == b.roughness;
}

// Is `child` present in parent's children list? (the list-consistency half of
// the destroy() contract — the field half is parentOf().)
bool childListed(World& w, ObjectId parent, ObjectId child) {
    for (ObjectId c : w.childrenOf(parent))
        if (c.value == child.value) return true;
    return false;
}

} // namespace

// ---- snapshot capture -------------------------------------------------------

TEST(captureSnapshot_is_empty_for_invalid_id) {
    World w;
    auto snap = w.captureSnapshot(ObjectId{999});
    CHECK(!snap.has_value());
    auto none = w.captureSnapshot(ObjectId{0});
    CHECK(!none.has_value());
}

TEST(captureSnapshot_root_is_nodes_zero_with_original_id) {
    World w;
    ObjectId root = w.createPrimitive(Primitive::Cube, "root");
    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;
    CHECK_EQ((int)snap->nodes.size(), 1);
    CHECK(snap->nodes[0].id.value == root.value);
}

TEST(captureSnapshot_captures_full_subtree_depth_first) {
    // root -> child -> grandchild, plus a second child of root.
    World w;
    ObjectId root  = w.createPrimitive(Primitive::Cube, "root");
    ObjectId child = w.createPrimitive(Primitive::Sphere, "child");
    ObjectId grand = w.createPrimitive(Primitive::Torus, "grand");
    ObjectId sib   = w.createPrimitive(Primitive::Plane, "sib");
    w.setParent(child, root);
    w.setParent(grand, child);
    w.setParent(sib, root);

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;
    CHECK_EQ((int)snap->nodes.size(), 4);
    CHECK(snap->nodes[0].id.value == root.value);   // nodes[0] is always the root

    // Every captured node must name a live object, and ids must be unique.
    std::vector<ember::u32> seen;
    for (const auto& n : snap->nodes) {
        CHECK(w.get(n.id) != nullptr);
        bool dup = false;
        for (ember::u32 s : seen) if (s == n.id.value) dup = true;
        CHECK(!dup);
        seen.push_back(n.id.value);
    }
}

TEST(captureSnapshot_is_inert_against_later_world_edits) {
    // Documented: "Meshes are copied by value, so the snapshot is fully inert
    // against later world edits." Mutating the world after capture must not
    // change what reinsert() restores.
    World w;
    ObjectId root = w.createPrimitive(Primitive::Cube, "root");
    Transform t{}; t.position = {1, 2, 3};
    w.setTransform(root, t);

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;

    // Now mutate the live object aggressively.
    Transform t2{}; t2.position = {99, 99, 99}; t2.scale = {5, 5, 5};
    w.setTransform(root, t2);
    w.setName(root, "renamed");

    // The snapshot still holds the ORIGINAL transform.
    CHECK(vecNear(snap->nodes[0].transform.position, {1, 2, 3}));
}

// ---- the editor's delete-undo cycle ----------------------------------------

TEST(delete_undo_cycle_restores_ids_parents_transforms_bitwise) {
    // The documented editor sequence: capture -> destroy -> reinsert.
    World w;
    ObjectId root  = w.createPrimitive(Primitive::Cube, "root");
    ObjectId child = w.createPrimitive(Primitive::Sphere, "child");
    ObjectId grand = w.createPrimitive(Primitive::Torus, "grand");
    w.setParent(child, root);
    w.setParent(grand, child);

    Transform tr{}; tr.position = {1, 0, 0}; tr.rotation = {0.1f, 0.2f, 0.3f};
    Transform tc{}; tc.position = {0, 5, 0}; tc.scale = {2, 2, 2};
    w.setTransform(root, tr);
    w.setTransform(child, tc);
    w.setMaterial(child, MaterialDesc{Color{1, 0, 0, 1}, 0.7f, 0.25f});

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;

    CHECK(w.destroy(root));
    CHECK(w.get(root) == nullptr);

    ObjectId back = w.reinsert(*snap);
    CHECK(back.value == root.value);          // original ROOT id comes back
    CHECK(w.get(root) != nullptr);
    CHECK(w.get(child) != nullptr);
    CHECK(w.get(grand) != nullptr);

    // Transforms bitwise-equal, parents restored, material restored.
    const Object* or_ = w.get(root);
    const Object* oc  = w.get(child);
    const Object* og  = w.get(grand);
    CHECK(or_ && oc && og);
    if (!or_ || !oc || !og) return;
    CHECK(transformEqual(or_->transform, tr));
    CHECK(transformEqual(oc->transform, tc));
    CHECK(w.parentOf(child).value == root.value);
    CHECK(w.parentOf(grand).value == child.value);
    CHECK(materialEqual(oc->material, MaterialDesc{Color{1, 0, 0, 1}, 0.7f, 0.25f}));
}

TEST(delete_undo_restores_names_mesh_kinds_and_visibility) {
    World w;
    ObjectId root = w.createPrimitive(Primitive::Cylinder, "MyCyl");
    ObjectId kid  = w.createPrimitive(Primitive::Torus, "Kid");
    w.setParent(kid, root);
    if (Object* o = w.get(kid)) o->visible = false;

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;
    CHECK(w.destroy(root));

    ObjectId back = w.reinsert(*snap);
    CHECK(back.value == root.value);
    const Object* or_ = w.get(root);
    const Object* ok  = w.get(kid);
    CHECK(or_ && ok);
    if (!or_ || !ok) return;
    CHECK(or_->name == "MyCyl");
    CHECK(ok->name == "Kid");
    CHECK(or_->hasMesh && or_->primitive == Primitive::Cylinder);
    CHECK(ok->hasMesh && ok->primitive == Primitive::Torus);
    CHECK(!ok->visible);                     // hidden flag survives
}

TEST(reinsert_requires_only_the_root_id_to_be_free) {
    // Documented: "only the ROOT id must be free." destroy() re-parents
    // survivors, so descendants may still be live when reinsert() runs; they
    // must be ADOPTED and reset, not duplicated or rejected.
    World w;
    ObjectId root  = w.createPrimitive(Primitive::Cube, "root");
    ObjectId child = w.createPrimitive(Primitive::Sphere, "child");
    w.setParent(child, root);

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;

    CHECK(w.destroy(root));
    // The child survived (re-parented to world root) and is still live.
    CHECK(w.get(child) != nullptr);
    CHECK(!w.parentOf(child).valid());

    // Mutate the survivor so we can prove reinsert RESET it.
    Transform t{}; t.position = {42, 0, 0};
    w.setTransform(child, t);

    ObjectId back = w.reinsert(*snap);
    CHECK(back.value == root.value);

    // Survivor was adopted under the restored root, and reset to captured state.
    const Object* oc = w.get(child);
    CHECK(oc != nullptr);
    if (oc) CHECK(vecNear(oc->transform.position, {0, 0, 0}));
    CHECK(w.parentOf(child).value == root.value);
    // Exactly one child under root — no duplicate created.
    CHECK_EQ((int)w.childrenOf(root).size(), 1);
}

TEST(reinsert_detaches_to_world_root_when_captured_parent_is_gone) {
    // Documented: "The subtree root silently detaches to a world root when its
    // captured world parent no longer exists."
    //
    // QA NOTE (my own earlier bug, caught by diagnostic): destroy() destroys ONE
    // object and re-parents its children — it does NOT recursively destroy the
    // subtree. So to make the captured world parent vanish AND free the root's
    // id, the subtree root must be destroyed first (freeing its id), then its
    // world parent destroyed (which re-parents the surviving descendant away).
    World w;
    ObjectId grandparent = w.createObject("gp");
    ObjectId root  = w.createPrimitive(Primitive::Cube, "root");
    ObjectId child = w.createPrimitive(Primitive::Sphere, "child");
    w.setParent(root, grandparent);
    w.setParent(child, root);

    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;

    // Free the root's id, then remove its world parent, so that by reinsert
    // time the captured parent (grandparent) no longer exists.
    CHECK(w.destroy(root));
    CHECK(w.destroy(grandparent));
    CHECK(w.get(grandparent) == nullptr);
    CHECK(w.get(root) == nullptr);          // id 2 is now free

    ObjectId back = w.reinsert(*snap);
    CHECK(back.value == root.value);        // root id restored
    CHECK(w.get(root) != nullptr);
    CHECK(!w.parentOf(root).valid());       // detached to a world root
    CHECK(w.parentOf(child).value == root.value);  // subtree-internal link intact
}

TEST(delete_undo_is_idempotent_across_repeated_cycles) {
    // Undo/redo stacks cycle many times; ids must not collide or leak.
    World w;
    ObjectId root = w.createPrimitive(Primitive::Cube, "root");
    auto snap = w.captureSnapshot(root);
    CHECK(snap.has_value());
    if (!snap) return;

    for (int i = 0; i < 5; ++i) {
        CHECK(w.destroy(root));
        ObjectId back = w.reinsert(*snap);
        CHECK(back.value == root.value);
    }
    CHECK_EQ((int)w.objects().size(), 1);    // never duplicated
}

// ---- duplicate ---------------------------------------------------------------

TEST(duplicate_returns_zero_for_invalid_id) {
    World w;
    CHECK(w.duplicate(ObjectId{0}).value == 0);
    CHECK(w.duplicate(ObjectId{12345}).value == 0);
}

TEST(duplicate_gets_a_fresh_id_and_leaves_source_untouched) {
    World w;
    ObjectId src = w.createPrimitive(Primitive::Cube, "src");
    Transform t{}; t.position = {3, 0, 0}; t.scale = {2, 1, 1};
    w.setTransform(src, t);
    w.setMaterial(src, MaterialDesc{Color{0, 1, 0, 1}, 0.5f, 0.5f});
    const std::size_t before = w.objects().size();

    ObjectId copy = w.duplicate(src);
    CHECK(copy.valid());
    CHECK(copy.value != src.value);
    CHECK_EQ((int)w.objects().size(), (int)before + 1);

    // Source untouched: same transform, same material, same id.
    const Object* s = w.get(src);
    CHECK(s != nullptr);
    if (s) {
        CHECK(transformEqual(s->transform, t));
        CHECK(materialEqual(s->material, MaterialDesc{Color{0, 1, 0, 1}, 0.5f, 0.5f}));
    }
    // Copy carries the source's transform and material.
    const Object* c = w.get(copy);
    CHECK(c != nullptr);
    if (c) {
        CHECK(transformEqual(c->transform, t));
        CHECK(materialEqual(c->material, MaterialDesc{Color{0, 1, 0, 1}, 0.5f, 0.5f}));
        CHECK(c->hasMesh && c->primitive == Primitive::Cube);
    }
}

TEST(duplicate_copies_the_whole_subtree_with_fresh_ids) {
    World w;
    ObjectId root  = w.createPrimitive(Primitive::Cube, "root");
    ObjectId child = w.createPrimitive(Primitive::Sphere, "child");
    ObjectId grand = w.createPrimitive(Primitive::Torus, "grand");
    w.setParent(child, root);
    w.setParent(grand, child);
    const std::size_t before = w.objects().size();

    ObjectId copy = w.duplicate(root);
    CHECK(copy.valid());
    CHECK_EQ((int)w.objects().size(), (int)before + 3);   // all 3 copied

    // The copied root's subtree has 3 members and none reuse a source id.
    std::vector<ObjectId> stack{copy};
    std::vector<ObjectId> copiedIds;
    while (!stack.empty()) {
        ObjectId cur = stack.back(); stack.pop_back();
        copiedIds.push_back(cur);
        for (ObjectId c : w.childrenOf(cur)) stack.push_back(c);
    }
    CHECK_EQ((int)copiedIds.size(), 3);
    for (ObjectId cid : copiedIds) {
        CHECK(cid.value != root.value);
        CHECK(cid.value != child.value);
        CHECK(cid.value != grand.value);
        CHECK(w.get(cid) != nullptr);
    }
}

TEST(duplicate_root_inherits_source_parent_and_descendants_keep_names) {
    // Documented: "The duplicate root inherits the source root's CURRENT
    // parent. Descendants keep their source names."
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId root   = w.createPrimitive(Primitive::Cube, "root");
    ObjectId kid    = w.createPrimitive(Primitive::Sphere, "KidName");
    w.setParent(root, parent);
    w.setParent(kid, root);

    ObjectId copy = w.duplicate(root);
    CHECK(copy.valid());
    CHECK(w.parentOf(copy).value == parent.value);   // inherits current parent

    // The copied child keeps the source child's NAME (no suffix on descendants).
    std::vector<ObjectId> kids = w.childrenOf(copy);
    CHECK_EQ((int)kids.size(), 1);
    if (!kids.empty()) {
        const Object* k = w.get(kids[0]);
        CHECK(k != nullptr);
        if (k) CHECK(k->name == "KidName");
    }
}

TEST(duplicate_with_explicit_name_uses_it_uniquely) {
    World w;
    ObjectId src = w.createPrimitive(Primitive::Cube, "src");
    ObjectId a = w.duplicate(src, "Chosen");
    ObjectId b = w.duplicate(src, "Chosen");
    CHECK(a.valid() && b.valid());
    CHECK(a.value != b.value);                       // no id collision
    const Object* oa = w.get(a);
    const Object* ob = w.get(b);
    CHECK(oa && ob);
    if (oa && ob) {
        CHECK(oa->name == "Chosen");
        CHECK(ob->name != "Chosen");                 // second clash is uniquified
    }
}

TEST(duplicate_of_unmeshed_object_stays_unmeshed) {
    World w;
    ObjectId src = w.createObject("groupOnly");
    ObjectId copy = w.duplicate(src);
    CHECK(copy.valid());
    const Object* c = w.get(copy);
    CHECK(c != nullptr);
    if (c) CHECK(!c->hasMesh);
}

// ---- destroy survivor consistency (the documented fix) ----------------------

TEST(destroy_reparents_survivor_and_keeps_field_and_list_consistent) {
    // Scene.h: destroy() "reparents the object's children to its parent first,
    // then destroys it." The child must be consistent in BOTH places afterwards:
    // its parent field points at the new parent, AND that parent's children
    // list contains it. A stale list here was a real bug, now fixed — pinned.
    World w;
    ObjectId gp     = w.createPrimitive(Primitive::Cube, "gp");
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId child  = w.createPrimitive(Primitive::Sphere, "child");
    w.setParent(parent, gp);
    w.setParent(child, parent);

    CHECK(w.destroy(parent));

    // Child was promoted to gp.
    CHECK(w.get(child) != nullptr);
    CHECK(w.parentOf(child).value == gp.value);
    CHECK(childListed(w, gp, child));      // list agrees with the field
    // And gp's list has exactly the one promoted child.
    CHECK_EQ((int)w.childrenOf(gp).size(), 1);
}

TEST(destroy_root_parent_detaches_children_consistently) {
    // Destroying a ROOT parent leaves children parentless; both the field and
    // the (now empty) parent must agree the child is a world root.
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId c1 = w.createPrimitive(Primitive::Sphere, "c1");
    ObjectId c2 = w.createPrimitive(Primitive::Torus, "c2");
    w.setParent(c1, parent);
    w.setParent(c2, parent);

    CHECK(w.destroy(parent));
    CHECK(!w.parentOf(c1).valid());
    CHECK(!w.parentOf(c2).valid());
    CHECK_EQ((int)w.childrenOf(c1).size(), 0);
    CHECK_EQ((int)w.childrenOf(c2).size(), 0);
}

HARNESS_MAIN()
