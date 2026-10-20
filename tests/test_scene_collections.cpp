// tests/test_scene_collections.cpp — owned by Verity · QA.
//
// Contract under test: D22 collections (src/scene/Scene.h) — the named
// grouping tree that is INDEPENDENT of Object parenting and NEVER transforms
// anything.
//
// SCOPE, stated honestly: of the wave-2 pure-logic areas this file could
// cover, only COLLECTION SEMANTICS has a declared API today. Verified absent
// by tree-wide search on 2026-10-16 (see the finding):
//   * modal transform delta maths  — no modal-transform API anywhere
//   * four pivot modes             — GizmoState carries one `origin`, no enum
//   * snap-to-GEOMETRY             — only grid snap exists (kRotateSnap etc,
//                                    already covered in test_editor_gizmo.cpp)
//   * origin-shift                 — no origin-shift API anywhere
// Those areas get tests when their APIs land; I do not test a guess.
//
// What IS pinned here, from the header's own contract:
//   * an object is in exactly ONE collection; invalid target = implicit root
//   * membership move is silent no-op for unknown ids
//   * collections never transform: a world matrix is unchanged by membership
//   * destroyCollection re-parents children collections AND member objects up
//     to the destroyed collection's parent
//   * visibility exclusion (rule 3): an object is hidden if its OWN collection
//     is hidden — ancestors are NOT consulted (that is the documented rule)
//   * setCollectionVisible on an unknown id is a no-op
//   * collection names uniquify like object names
//   * captureCollection/reinsertCollection restores membership
//
// Pure scene-graph data: no GPU, no window, no context.
#include "harness.h"

#include "scene/Scene.h"

#include <string>
#include <vector>

using ember::Mat4;
using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
using ember::scene::Collection;
using ember::scene::CollectionId;
using ember::scene::Object;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-6f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

} // namespace

// ---- creation and identity --------------------------------------------------

TEST(collections_create_returns_valid_unique_ids) {
    World w;
    const CollectionId a = w.createCollection("Props");
    const CollectionId b = w.createCollection("Lights");
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a.value != b.value);
    CHECK_EQ((int)w.collections().size(), 2);
}

TEST(collections_new_collection_is_a_root_and_visible) {
    World w;
    const CollectionId c = w.createCollection("Rootish");
    const Collection* got = w.get(c);
    CHECK(got != nullptr);
    if (!got) return;
    CHECK(!got->parent.valid());       // invalid parent == root collection
    CHECK(got->visible);               // default visible
    CHECK(got->name == "Rootish");
}

TEST(collections_get_unknown_id_returns_null) {
    World w;
    CHECK(w.get(CollectionId{9999}) == nullptr);
    CHECK(w.get(CollectionId{0}) == nullptr);
}

TEST(collections_names_uniquify_on_clash) {
    // The header says names uniquify "across collections", the same Blender
    // rule object names use. Two collections called "Props" must not both be
    // named exactly "Props".
    World w;
    w.createCollection("Props");
    const CollectionId second = w.createCollection("Props");
    const Collection* got = w.get(second);
    CHECK(got != nullptr);
    if (!got) return;
    CHECK(got->name != "Props");
}

// ---- membership: exactly ONE collection -------------------------------------

TEST(collections_object_starts_in_the_implicit_root) {
    World w;
    const ObjectId o = w.createObject("loose");
    // Invalid CollectionId == the implicit root: no membership was assigned.
    CHECK(!w.collectionOf(o).valid());
}

TEST(collections_assign_moves_object_into_target) {
    World w;
    const ObjectId o = w.createObject("widget");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    CHECK(w.collectionOf(o).value == c.value);
}

TEST(collections_object_is_in_exactly_one_collection) {
    // The header's rule: membership is ONE CollectionId, so moving to a second
    // collection must LEAVE the first, not join both.
    World w;
    const ObjectId o = w.createObject("widget");
    const CollectionId a = w.createCollection("A");
    const CollectionId b = w.createCollection("B");
    w.assignToCollection(o, a);
    w.assignToCollection(o, b);
    CHECK(w.collectionOf(o).value == b.value);   // now in B...
    // ...and therefore no longer counted as a member of A. collections() does
    // not carry member lists, so the observable proof is that a second move
    // away from B lands in the new target and nowhere else.
    const CollectionId c = w.createCollection("C");
    w.assignToCollection(o, c);
    CHECK(w.collectionOf(o).value == c.value);
}

TEST(collections_assign_to_invalid_target_detaches_to_root) {
    // "Invalid target = the implicit root collection."
    World w;
    const ObjectId o = w.createObject("widget");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    CHECK(w.collectionOf(o).valid());
    w.assignToCollection(o, CollectionId{});      // invalid == root
    CHECK(!w.collectionOf(o).valid());
}

TEST(collections_assign_unknown_ids_are_silent_noop) {
    // "Silent no-op for unknown ids" — must not crash, must not corrupt.
    World w;
    const ObjectId o = w.createObject("widget");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    w.assignToCollection(ObjectId{7777}, c);      // unknown object
    w.assignToCollection(o, CollectionId{8888});  // unknown collection
    // The real object is untouched by the unknown-object call, and the
    // unknown-collection call must not have moved it out of Bin.
    CHECK(w.collectionOf(o).value == c.value);
    CHECK(w.has(ObjectId{7777}) == false);        // nothing was created
}

// ---- collections NEVER transform --------------------------------------------

TEST(collections_membership_never_changes_the_world_matrix) {
    // The header's central guarantee: collections are grouping only, they
    // never transform. Moving an object between collections must leave its
    // world matrix bit-identical.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "cube");
    Transform t{};
    t.position = {3.0f, -2.0f, 7.0f};
    t.rotation = {0.4f, -1.1f, 2.2f};
    t.scale    = {2.0f, 0.5f, 1.5f};
    w.setTransform(o, t);
    const Mat4 before = w.worldMatrix(o);

    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    CHECK(matNear(w.worldMatrix(o), before));

    // And parenting a collection must not act as a transform either: a
    // child collection's membership changes nothing about member matrices.
    const CollectionId child = w.createCollection("Nested", c);
    w.assignToCollection(o, child);
    CHECK(matNear(w.worldMatrix(o), before));
}

TEST(collections_hierarchy_does_not_reparent_objects) {
    // Collections have their OWN parent tree; nesting a collection must not
    // change any object's OBJECT parent.
    World w;
    const ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    const ObjectId child  = w.createPrimitive(Primitive::Cube, "child");
    w.setParent(child, parent);

    const CollectionId a = w.createCollection("Outer");
    const CollectionId b = w.createCollection("Inner", a);
    w.assignToCollection(child, b);

    // Object parenting is untouched...
    CHECK(w.parentOf(child).value == parent.value);
    // ...and the child's world matrix still encodes the OBJECT parent link.
    const Mat4 expected = w.worldMatrix(parent) * w.get(child)->transform.toMatrix();
    CHECK(matNear(w.worldMatrix(child), expected));
}

// ---- destroyCollection: children collections AND members move up ------------

TEST(collections_destroy_moves_members_up_to_the_parent) {
    // "Children collections AND the destroyed collection's member objects move
    // up to its parent collection (invalid parent = implicit root)."
    World w;
    const CollectionId parent = w.createCollection("Parent");
    const CollectionId victim = w.createCollection("Victim", parent);
    const ObjectId o = w.createObject("widget");
    w.assignToCollection(o, victim);

    CHECK(w.destroyCollection(victim));
    CHECK(w.get(victim) == nullptr);
    // The member object now sits in the victim's PARENT, not the root.
    CHECK(w.collectionOf(o).value == parent.value);
}

TEST(collections_destroy_root_collection_detaches_members_to_root) {
    // A ROOT collection's parent is invalid, so its members fall to the
    // implicit root — i.e. membership becomes invalid.
    World w;
    const CollectionId victim = w.createCollection("SoloRoot");
    const ObjectId o = w.createObject("widget");
    w.assignToCollection(o, victim);

    CHECK(w.destroyCollection(victim));
    CHECK(!w.collectionOf(o).valid());      // back to the implicit root
}

TEST(collections_destroy_moves_child_collections_up) {
    // Child COLLECTIONS must be re-parented to the destroyed collection's
    // parent, so the grouping tree stays connected.
    World w;
    const CollectionId grandparent = w.createCollection("Grandparent");
    const CollectionId parent      = w.createCollection("Parent", grandparent);
    const CollectionId child       = w.createCollection("Child", parent);
    CHECK(w.get(child)->parent.value == parent.value);

    CHECK(w.destroyCollection(parent));
    CHECK(w.get(parent) == nullptr);
    // The child survives and is now rooted under the grandparent.
    const Collection* survived = w.get(child);
    CHECK(survived != nullptr);
    if (survived) CHECK(survived->parent.value == grandparent.value);
}

TEST(collections_destroy_unknown_id_returns_false) {
    World w;
    CHECK(!w.destroyCollection(CollectionId{4242}));
    CHECK(!w.destroyCollection(CollectionId{0}));
}

// ---- visibility exclusion (rule 3) ------------------------------------------
// The documented rule (by struct Collection, quoted in Scene.h): an object is
// visible iff object.visible && (!object.collection.valid() ||
// collection.visible) — the member's OWN collection flag, NOT the ancestors'.

TEST(collections_setCollectionVisible_toggles_the_flag) {
    World w;
    const CollectionId c = w.createCollection("HiddenBin");
    w.setCollectionVisible(c, false);
    const Collection* got = w.get(c);
    CHECK(got != nullptr);
    if (got) CHECK(!got->visible);
    w.setCollectionVisible(c, true);
    CHECK(w.get(c)->visible);
}

TEST(collections_setCollectionVisible_unknown_id_is_noop) {
    World w;
    const CollectionId c = w.createCollection("Bin");
    w.setCollectionVisible(CollectionId{5555}, false);   // unknown: no-op
    CHECK(w.get(c)->visible);                            // nothing else changed
    CHECK_EQ((int)w.collections().size(), 1);
}

TEST(collections_hidden_collection_skips_pick_but_not_the_object) {
    // Rule 3 is about PICKING and the packet, not about deleting the object.
    // The observable contract: hiding a collection leaves the object alive and
    // its transform intact; it is the pick/packet path that must exclude it.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "cube");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    const Mat4 before = w.worldMatrix(o);

    w.setCollectionVisible(c, false);

    // Object is still there, transform untouched (collections never transform).
    CHECK(w.has(o));
    CHECK(matNear(w.worldMatrix(o), before));
    // And the packet must not draw a collection-hidden object.
    const ember::RenderPacket pk = w.buildPacket(640, 480);
    for (const ember::DrawItem& d : pk.items)
        CHECK(d.object.value != o.value);
}

TEST(collections_visible_collection_object_draws) {
    // Control for the test above: same object, collection VISIBLE, must draw.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "cube");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    const ember::RenderPacket pk = w.buildPacket(640, 480);
    bool found = false;
    for (const ember::DrawItem& d : pk.items)
        if (d.object.value == o.value) found = true;
    CHECK(found);
}

TEST(collections_object_visible_false_also_skips_draw) {
    // Rule 3 is a conjunction: object.visible AND collection.visible. Hiding
    // the OBJECT while the collection is visible must also exclude it.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "cube");
    const CollectionId c = w.createCollection("Bin");
    w.assignToCollection(o, c);
    w.setVisible(o, false);

    const ember::RenderPacket pk = w.buildPacket(640, 480);
    for (const ember::DrawItem& d : pk.items)
        CHECK(d.object.value != o.value);
}

// ---- capture / reinsert (undo for destroyCollection) ------------------------

TEST(collections_capture_reinsert_restores_membership) {
    World w;
    const CollectionId parent = w.createCollection("Parent");
    const CollectionId victim = w.createCollection("Victim", parent);
    const ObjectId o = w.createObject("widget");
    w.assignToCollection(o, victim);

    auto snap = w.captureCollection(victim);
    CHECK(snap.has_value());
    if (!snap) return;

    CHECK(w.destroyCollection(victim));
    // QA NOTE (bug in MY first version of this test, caught by Slate reviewing
    // against D22): I asserted `!w.collectionOf(o).valid()` here — "member fell
    // to the root". That is WRONG for this fixture. `victim`'s parent is a LIVE
    // collection, so the documented rule ("children collections AND the
    // destroyed collection's member objects move up to its parent") puts the
    // member in `parent`, not the root. My own passing test
    // collections_destroy_moves_members_up_to_the_parent asserts exactly that,
    // so the two tests contradicted each other. Root-ward fall only happens
    // when the destroyed collection's parent is invalid (covered separately by
    // collections_destroy_root_collection_detaches_members_to_root).
    CHECK(w.collectionOf(o).value == parent.value);   // member moved UP, not to root

    const CollectionId back = w.reinsertCollection(*snap);
    CHECK(back.valid());
    // The victim is back and the member is back inside it.
    const Collection* restored = w.get(back);
    CHECK(restored != nullptr);
    if (restored) CHECK(restored->parent.value == parent.value);
    CHECK(w.collectionOf(o).value == back.value);
}

TEST(collections_capture_unknown_id_is_empty) {
    World w;
    auto snap = w.captureCollection(CollectionId{31337});
    CHECK(!snap.has_value());
}

HARNESS_MAIN()
