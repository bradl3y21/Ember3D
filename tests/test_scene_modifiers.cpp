// tests/test_scene_modifiers.cpp — owned by Verity · QA.
//
// Contract under test: the modifier stack is NON-DESTRUCTIVE (D40, items
// 120/121; Types.h:329-370, Scene.h:96-106). Item 123's four obligations:
//
//   1. the BASE mesh survives evaluation unchanged — `mesh` and `edit` are
//      never touched (Scene.h:98-99);
//   2. evaluation is DETERMINISTIC — same stack, same base, same output, and
//      the version-stamped cache serves stale geometry if a bump is missed
//      (Scene.h:100-103 names that as THIS item's test obligation);
//   3. evaluation is ORDER-DEPENDENT in the documented way — the stack is an
//      ordered list, so [A,B] and [B,A] must differ where the ops do not
//      commute;
//   4. REMOVING a modifier restores the original mesh EXACTLY;
//   5. plus the case the task names: a modifier on an object under a
//      NON-UNIFORM parent scale.
//
// WHY BOTH CHECKS RUN: `world.modifiers()` and the version counters are the
// implementation's own view. Every assertion here therefore ALSO reads the
// evaluated MeshData out of the RenderPacket's DrawItem — the thing the
// renderer would actually draw — rather than trusting the bookkeeping. A
// version counter that bumps while the geometry stays stale is exactly the
// missed-bump failure Scene.h:102-103 warns about, and only the geometry read
// catches it.
//
// Every expected number was MEASURED before it was asserted, because guessing
// a count and watching it fail would read as a product defect when it is
// really a test author's error.
#include "harness.h"

#include "core/Types.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using ember::ArrayParams;
using ember::DrawItem;
using ember::MeshData;
using ember::MirrorParams;
using ember::Modifier;
using ember::ModifierKind;
using ember::ModifierParams;
using ember::ObjectId;
using ember::SubdivisionParams;
using ember::Transform;
using ember::u32;
using ember::Vec3;
using ember::scene::EditableMesh;
using ember::scene::Object;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

// Local to this file: the harness has CHECK/CHECK_EQ/CHECK_NEAR but no
// "check with a reason" form, and the reason matters here — a bare CHECK(x.ok)
// tells a future reader nothing about WHICH invariant broke. Built on the
// harness's own reportFailure so counting and printing stay correct.
#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__,                           \
                                   harness::fmtf(__VA_ARGS__));                  \
    } while (0)

// Reads the evaluated mesh for `id` out of a freshly built packet — the exact
// bytes the renderer would draw. NOT the object's own `mesh` member, which the
// contract says evaluation must never touch. Returns an empty mesh if the
// object drew nothing.
MeshData evaluatedViaPacket(World& w, ObjectId id) {
    ember::RenderPacket pk = w.buildPacket(640, 480);
    for (const DrawItem& it : pk.items)
        if (it.object == id && it.mesh) return *it.mesh;
    return MeshData{};
}

bool sameFloats(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](float x, float y) { return x == y; });
}

// Exact MeshData identity. Bitwise on the float arrays: for IEEE-754 non-NaN
// values this is exact equality, which is what "restores the original EXACTLY"
// has to mean.
bool identical(const MeshData& a, const MeshData& b) {
    return sameFloats(a.positions, b.positions) && a.indices == b.indices &&
           sameFloats(a.normals, b.normals) && sameFloats(a.uvs, b.uvs);
}

std::string describe(const MeshData& m) {
    char b[128];
    std::snprintf(b, sizeof b, "v=%u idx=%u nrm=%zu uv=%zu",
                  m.vertexCount(), m.indexCount(),
                  m.normals.size() / 3, m.uvs.size() / 2);
    return b;
}

// A cube with an explicit, memorable base: 8 verts, 12 tris, 36 indices.
MeshData baseCube(const World& w, ObjectId id) {
    const Object* o = w.get(id);
    return o ? o->mesh : MeshData{};
}

Modifier make(ModifierKind kind) { return Modifier{ModifierParams{}, true}; }

} // namespace

// ---- (1) THE BASE MESH SURVIVES EVALUATION UNCHANGED -----------------------

TEST(modifiers_base_mesh_is_never_touched_by_evaluation) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const MeshData before = baseCube(w, id);

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(w.addModifier(id, ModifierKind::Array));
    const MeshData evaluated = evaluatedViaPacket(w, id);

    // The evaluation must actually HAVE changed something, or this test proves
    // nothing about non-destructiveness — a no-op stack cannot damage anything.
    CHECK_MSG(!identical(evaluated, before), "subdivision+array produced no change",
              "the modifiers appear to be no-ops; this test would then be vacuous");

    // ...and the BASE must be byte-identical to what it was before.
    const MeshData after = baseCube(w, id);
    CHECK_MSG(identical(after, before), "base mesh changed under evaluation",
              describe(before).c_str());
    CHECK_MSG(after.positions == before.positions, "base positions changed", "");
    CHECK_MSG(after.indices == before.indices, "base indices changed", "");
}

TEST(modifiers_edit_source_is_also_untouched) {
    // Scene.h:98-99 says `mesh` AND `edit` are never touched. edit is the D34
    // source of truth, so damage there would survive a mesh regeneration.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK(w.editable(id));
    const u32 vertsBefore = w.readableEdit(id) ? w.readableEdit(id)->vertexCount() : 0;
    const u32 versionBefore = w.readableEdit(id) ? w.readableEdit(id)->topologyVersion : 0;

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    (void)evaluatedViaPacket(w, id);

    const EditableMesh* em = w.readableEdit(id);
    CHECK_MSG(em != nullptr, "edit mesh vanished after evaluation", "");
    if (em) {
        CHECK_MSG(em->vertexCount() == vertsBefore, "edit vertex count changed", "");
        CHECK_MSG(em->topologyVersion == versionBefore, "edit topologyVersion bumped",
                  "evaluation must not mutate the D34 source of truth");
    }
}

// ---- (2) EVALUATION IS DETERMINISTIC ---------------------------------------

TEST(modifiers_evaluation_is_deterministic_across_calls) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    MeshData p;
    SubdivisionParams sp; sp.levels = 2;
    CHECK(w.setModifierParams(id, 0, ModifierParams{sp}));

    const MeshData a = evaluatedViaPacket(w, id);
    const MeshData b = evaluatedViaPacket(w, id);
    const MeshData c = evaluatedViaPacket(w, id);
    CHECK(!a.empty());
    CHECK_MSG(identical(a, b), "two evaluations of the same stack differ", "");
    CHECK_MSG(identical(a, c), "third evaluation differs from the first", "");
}

TEST(modifiers_cache_invalidates_on_param_change) {
    // The version-stamped cache (Scene.h:100-103) is THIS item's named
    // obligation: a missed bump serves STALE geometry. Changing a parameter
    // must therefore change the evaluated output.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    SubdivisionParams sp1; sp1.levels = 1;
    CHECK(w.setModifierParams(id, 0, ModifierParams{sp1}));
    const MeshData at1 = evaluatedViaPacket(w, id);

    SubdivisionParams sp2; sp2.levels = 2;
    CHECK(w.setModifierParams(id, 0, ModifierParams{sp2}));
    const MeshData at2 = evaluatedViaPacket(w, id);

    CHECK_MSG(!identical(at1, at2),
              "cache served STALE geometry after a param change",
              "levels 1 vs 2 produced the same mesh; the modifierVersion bump "
              "is either missing or not consulted");
    CHECK(w.modifierStackVersion(id) > 0);
}

TEST(modifiers_version_bumps_on_every_mutator) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const u32 v0 = w.modifierStackVersion(id);

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    const u32 v1 = w.modifierStackVersion(id);
    CHECK(v1 > v0);

    SubdivisionParams sp; sp.levels = 3;
    CHECK(w.setModifierParams(id, 0, ModifierParams{sp}));
    const u32 v2 = w.modifierStackVersion(id);
    CHECK(v2 > v1);

    CHECK(w.setModifierEnabled(id, 0, false));
    const u32 v3 = w.modifierStackVersion(id);
    CHECK(v3 > v2);

    CHECK(w.setModifierEnabled(id, 0, true));
    CHECK(w.modifierStackVersion(id) > v3);
}

// ---- (3) ORDER-DEPENDENT IN THE DOCUMENTED WAY -----------------------------

TEST(modifiers_order_changes_the_result_where_ops_do_not_commute) {
    // Scene.h:96 calls it "the modifier STACK — ordered". Array-then-mirror and
    // mirror-then-array are the classic non-commuting pair: the first op's
    // output is the second op's input, so the vertex sets differ.
    World w;
    const ObjectId a = w.createPrimitive(Primitive::Cube, "arrayFirst");
    const ObjectId b = w.createPrimitive(Primitive::Cube, "mirrorFirst");

    CHECK(w.addModifier(a, ModifierKind::Array));
    CHECK(w.addModifier(a, ModifierKind::Mirror));
    CHECK(w.addModifier(b, ModifierKind::Mirror));
    CHECK(w.addModifier(b, ModifierKind::Array));

    ArrayParams ap; ap.count = 3; ap.offsetX = 2.0f; ap.relative = false;
    MirrorParams mp; mp.axisX = true; mp.axisY = false; mp.axisZ = false;
    mp.mergeThreshold = 0.0f;                       // never weld: keep both halves
    CHECK(w.setModifierParams(a, 0, ModifierParams{ap}));
    CHECK(w.setModifierParams(a, 1, ModifierParams{mp}));
    CHECK(w.setModifierParams(b, 0, ModifierParams{mp}));
    CHECK(w.setModifierParams(b, 1, ModifierParams{ap}));

    const MeshData arrayThenMirror = evaluatedViaPacket(w, a);
    const MeshData mirrorThenArray = evaluatedViaPacket(w, b);
    CHECK(!arrayThenMirror.empty());
    CHECK(!mirrorThenArray.empty());
    CHECK_MSG(!identical(arrayThenMirror, mirrorThenArray),
              "stack order made no difference — the stack is not ordered",
              "array->mirror and mirror->array produced identical meshes");
}

TEST(modifiers_moveModifier_reorders_and_the_result_follows) {
    // The reorder path must move the SAME two modifiers and produce the SAME
    // result as authoring them in the other order — not a third thing.
    World w;
    const ObjectId authored = w.createPrimitive(Primitive::Cube, "authored");
    const ObjectId moved = w.createPrimitive(Primitive::Cube, "moved");

    ArrayParams ap; ap.count = 3; ap.offsetX = 2.0f; ap.relative = false;
    MirrorParams mp; mp.axisX = true; mp.mergeThreshold = 0.0f;

    // authored: [Array, Mirror]
    CHECK(w.addModifier(authored, ModifierKind::Array));
    CHECK(w.addModifier(authored, ModifierKind::Mirror));
    CHECK(w.setModifierParams(authored, 0, ModifierParams{ap}));
    CHECK(w.setModifierParams(authored, 1, ModifierParams{mp}));

    // moved: authored as [Mirror, Array], then moveModifier 1 -> 0 => [Array, Mirror]
    CHECK(w.addModifier(moved, ModifierKind::Mirror));
    CHECK(w.addModifier(moved, ModifierKind::Array));
    CHECK(w.setModifierParams(moved, 0, ModifierParams{mp}));
    CHECK(w.setModifierParams(moved, 1, ModifierParams{ap}));
    CHECK(w.moveModifier(moved, 1, 0));

    const std::vector<Modifier>& st = w.modifiers(moved);
    CHECK_MSG(st.size() == 2, "stack size changed by reorder", "");
    if (st.size() == 2) {
        CHECK_MSG(st[0].kind() == ModifierKind::Array, "reorder left wrong order",
                  "index 0 should now be Array");
        CHECK_MSG(st[1].kind() == ModifierKind::Mirror, "reorder left wrong order",
                  "index 1 should now be Mirror");
    }
    CHECK_MSG(identical(evaluatedViaPacket(w, moved), evaluatedViaPacket(w, authored)),
              "reordering produced a different result than authoring in that order", "");
}

// ---- (4) REMOVING A MODIFIER RESTORES THE ORIGINAL EXACTLY -----------------

TEST(modifiers_remove_restores_the_original_exactly) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const MeshData original = baseCube(w, id);

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    const MeshData withMod = evaluatedViaPacket(w, id);
    CHECK(!identical(withMod, original));

    CHECK(w.removeModifier(id, 0));
    CHECK_MSG(w.modifiers(id).empty(), "stack not empty after removing the only modifier", "");

    const MeshData restored = evaluatedViaPacket(w, id);
    CHECK_MSG(identical(restored, original),
              "removing a modifier did NOT restore the original exactly",
              describe(restored).c_str());
}

TEST(modifiers_disabling_restores_the_original_without_removing) {
    // enabled=false must be equivalent to the modifier not being there, and
    // must be reversible — that is what makes the panel's checkbox useful.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const MeshData original = baseCube(w, id);

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(!identical(evaluatedViaPacket(w, id), original));

    CHECK(w.setModifierEnabled(id, 0, false));
    CHECK_MSG(identical(evaluatedViaPacket(w, id), original),
              "disabling a modifier did not restore the original", "");

    CHECK(w.setModifierEnabled(id, 0, true));
    CHECK_MSG(!identical(evaluatedViaPacket(w, id), original),
              "re-enabling did not re-apply the modifier", "");
}

TEST(modifiers_removing_one_of_several_keeps_the_rest) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(w.addModifier(id, ModifierKind::Array));
    CHECK(w.setModifierParams(id, 1, ModifierParams{ArrayParams{}}));

    const MeshData both = evaluatedViaPacket(w, id);
    CHECK(w.removeModifier(id, 0));                 // drop the subdivision
    const MeshData afterDrop = evaluatedViaPacket(w, id);

    // Rebuild the same state as "array only" from scratch; must be identical.
    World w2;
    const ObjectId id2 = w2.createPrimitive(Primitive::Cube, "cube2");
    CHECK(w2.addModifier(id2, ModifierKind::Array));
    CHECK(w2.setModifierParams(id2, 0, ModifierParams{ArrayParams{}}));

    CHECK_MSG(identical(afterDrop, evaluatedViaPacket(w2, id2)),
              "removing modifier 0 left a state that differs from authoring array-only",
              "");
    CHECK_MSG(!identical(afterDrop, both), "removing a modifier changed nothing", "");
}

// ---- (5) NON-UNIFORM PARENT SCALE — the case the task names ----------------

TEST(modifiers_under_a_non_uniform_parent_scale_evaluate_in_local_space) {
    // Modifiers operate on the object's LOCAL mesh; the parent's transform is
    // applied at draw time via the model matrix. So a non-uniform parent scale
    // must NOT change the EVALUATED geometry — only the DrawItem::model.
    // If the evaluated mesh changes, evaluation is reading world state, which
    // would make the same object evaluate differently depending on where it is
    // parented. That is the usual way these break.
    World w;
    const ObjectId parent = w.createObject("parent");
    Transform pt; pt.scale = Vec3{3.0f, 0.5f, 1.7f};       // deliberately non-uniform
    w.setTransform(parent, pt);

    const ObjectId child = w.createPrimitive(Primitive::Cube, "child");
    w.setParent(child, parent);
    CHECK(w.addModifier(child, ModifierKind::Subdivision));
    SubdivisionParams sp; sp.levels = 1;
    CHECK(w.setModifierParams(child, 0, ModifierParams{sp}));

    // The same modifier on the same base, unparented.
    World w2;
    const ObjectId flat = w2.createPrimitive(Primitive::Cube, "flat");
    CHECK(w2.addModifier(flat, ModifierKind::Subdivision));
    CHECK(w2.setModifierParams(flat, 0, ModifierParams{sp}));

    const MeshData parented = evaluatedViaPacket(w, child);
    const MeshData unparented = evaluatedViaPacket(w2, flat);
    CHECK(!parented.empty());
    CHECK_MSG(identical(parented, unparented),
              "a non-uniform parent scale changed the EVALUATED mesh",
              "evaluation is reading world state; modifiers must be local-space");

    // The parent's scale must still reach the DRAW via the model matrix.
    ember::RenderPacket pk = w.buildPacket(640, 480);
    bool sawChild = false;
    for (const DrawItem& it : pk.items) {
        if (it.object != child) continue;
        sawChild = true;
        const float sx = std::sqrt(it.model.m[0]*it.model.m[0] +
                                   it.model.m[1]*it.model.m[1] +
                                   it.model.m[2]*it.model.m[2]);
        CHECK_MSG(std::fabs(sx - 3.0f) < 1e-4f, "parent X scale did not reach the model matrix",
                  "extracted X scale was not 3.0");
    }
    CHECK_MSG(sawChild, "the child did not draw at all", "");
}

TEST(modifiers_under_parent_scale_survive_remove_exactly) {
    // The non-destructive promise must hold under a parent too: removing the
    // modifier has to return the base mesh, not some world-space residue.
    World w;
    const ObjectId parent = w.createObject("parent");
    Transform pt; pt.scale = Vec3{2.0f, 0.25f, 4.0f};
    w.setTransform(parent, pt);
    const ObjectId child = w.createPrimitive(Primitive::Cube, "child");
    w.setParent(child, parent);
    const MeshData original = baseCube(w, child);

    CHECK(w.addModifier(child, ModifierKind::Mirror));
    MirrorParams mp; mp.axisX = true; mp.mergeThreshold = 0.0f;
    CHECK(w.setModifierParams(child, 0, ModifierParams{mp}));
    CHECK(!identical(evaluatedViaPacket(w, child), original));

    CHECK(w.removeModifier(child, 0));
    CHECK_MSG(identical(evaluatedViaPacket(w, child), original),
              "remove did not restore the original under a non-uniform parent scale", "");
}

// ---- (6) THE PACKET BOUNDARY: render must not see the stack -----------------

TEST(modifiers_suspendModifierStack_draws_the_base_mesh) {
    // D40's documented split (Scene.h:577-583): suspendModifierStack makes
    // buildPacket draw the BASE mesh — the edit-mode hook. If it does not, the
    // parameter is declared and unreachable.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const MeshData original = baseCube(w, id);
    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(!identical(evaluatedViaPacket(w, id), original));

    ember::RenderPacket pk = w.buildPacket(640, 480, /*suspendModifierStack=*/true);
    bool saw = false;
    for (const DrawItem& it : pk.items) {
        if (it.object != id || !it.mesh) continue;
        saw = true;
        CHECK_MSG(identical(*it.mesh, original),
                  "suspendModifierStack did not draw the base mesh",
                  describe(*it.mesh).c_str());
    }
    CHECK_MSG(saw, "the object did not draw with the stack suspended", "");
}

TEST(modifiers_empty_stack_is_a_clean_no_op) {
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    const MeshData original = baseCube(w, id);
    CHECK(w.modifiers(id).empty());
    CHECK_MSG(identical(evaluatedViaPacket(w, id), original),
              "an empty stack changed the mesh", "");
    CHECK_EQ(w.modifierStackVersion(id), u32{0});
}

// ---- (7) THE FAILURE CONTRACT AND DISABLED-SKIP SEMANTICS ------------------
//
// Added on the third pass of item 123. The first two passes covered the four
// named obligations; this section closes the residue I named both times.
//
// HONEST LIMIT, FOUND WHILE WRITING IT: I could NOT drive a real structural
// failure. The only noOp path is buildSubAdjacency refusing (Modifiers.cpp:348),
// which needs a malformed index buffer — an index out of range, or
// indexCount % 3 != 0. But World::get() returns const Object* and there is no
// public setMesh, so a test cannot inject a malformed base through the API the
// app itself uses. The failure path is therefore UNREACHABLE from a test
// without an API change. What IS reachable is the sentinel contract around it,
// and that is what these tests pin.

TEST(modifiers_failure_index_is_the_no_failure_sentinel_when_healthy) {
    // evaluatedFailureIndex returns 0xFFFFFFFF when nothing failed. If it ever
    // returned a real index on a healthy stack, the editor would show a failure
    // badge on working geometry.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK_EQ(w.evaluatedFailureIndex(id), 0xFFFFFFFFu);      // no modifiers yet

    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(w.addModifier(id, ModifierKind::Array));
    (void)evaluatedViaPacket(w, id);                          // populate the cache
    CHECK_MSG(w.evaluatedFailureIndex(id) == 0xFFFFFFFFu,
              "a healthy stack reported a FAILURE index",
              "evaluatedFailureIndex should be the 0xFFFFFFFF sentinel when "
              "every modifier succeeded");
}

TEST(modifiers_a_stale_failure_report_is_not_served) {
    // Scene.cpp:1414-1417: "a cached failure is only meaningful while it
    // matches the object NOW" — the cache entry is keyed on meshVersion AND
    // modifierVersion. Mutating the stack must invalidate any prior report,
    // so a failure seen before a fix does not linger as a badge.
    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube, "cube");
    CHECK(w.addModifier(id, ModifierKind::Subdivision));
    CHECK(w.addModifier(id, ModifierKind::Array));
    (void)evaluatedViaPacket(w, id);
    CHECK_EQ(w.evaluatedFailureIndex(id), 0xFFFFFFFFu);

    // Any stack mutation invalidates the entry, so the next read must be the
    // sentinel again rather than a carried-over value.
    SubdivisionParams sp; sp.levels = 3;
    CHECK(w.setModifierParams(id, 0, ModifierParams{sp}));
    CHECK_MSG(w.evaluatedFailureIndex(id) == 0xFFFFFFFFu,
              "a param change served a STALE failure index",
              "the cache entry should be invalidated by the modifierVersion bump");
}

TEST(modifiers_disabled_entries_are_skipped_in_place_not_removed) {
    // Modifiers.cpp:520 — "DISABLED entries are skipped in place; position
    // preserved". So [Array, Mirror] with Mirror disabled must equal [Array]
    // alone, AND re-enabling must restore the two-modifier result. If disabling
    // instead removed the entry, re-enabling could not restore the ordering.
    World w;
    const ObjectId stacked = w.createPrimitive(Primitive::Cube, "stacked");
    const ObjectId arrayOnly = w.createPrimitive(Primitive::Cube, "arrayOnly");

    ArrayParams ap; ap.count = 3; ap.offsetX = 2.0f; ap.relative = false;
    MirrorParams mp; mp.axisX = true; mp.mergeThreshold = 0.0f;

    CHECK(w.addModifier(stacked, ModifierKind::Array));
    CHECK(w.addModifier(stacked, ModifierKind::Mirror));
    CHECK(w.setModifierParams(stacked, 0, ModifierParams{ap}));
    CHECK(w.setModifierParams(stacked, 1, ModifierParams{mp}));

    CHECK(w.addModifier(arrayOnly, ModifierKind::Array));
    CHECK(w.setModifierParams(arrayOnly, 0, ModifierParams{ap}));

    const MeshData both = evaluatedViaPacket(w, stacked);
    CHECK_MSG(!identical(both, evaluatedViaPacket(w, arrayOnly)),
              "disabling was needed to see a difference, but the two already matched", "");

    // Disable index 1 (the Mirror). Position must be preserved — the Array is
    // still index 0 — and the result must equal array-only.
    CHECK(w.setModifierEnabled(stacked, 1, false));
    CHECK_MSG(w.modifiers(stacked).size() == 2,
              "disabling REMOVED the entry instead of skipping it",
              "a disabled modifier must keep its position in the stack");
    CHECK_MSG(stacked && w.modifiers(stacked)[1].kind() == ModifierKind::Mirror,
              "disabling changed the entry's kind or position", "");
    CHECK_MSG(identical(evaluatedViaPacket(w, stacked), evaluatedViaPacket(w, arrayOnly)),
              "a disabled modifier still contributed to the result", "");

    // And re-enabling restores the original two-modifier result exactly.
    CHECK(w.setModifierEnabled(stacked, 1, true));
    CHECK_MSG(identical(evaluatedViaPacket(w, stacked), both),
              "re-enabling did not restore the two-modifier result exactly", "");
}

TEST(modifiers_disabling_the_first_of_two_leaves_the_second) {
    // The mirror image of the above: disable index 0, keep index 1. This is the
    // case that would break if disabling shifted indices rather than preserving
    // position — the surviving modifier would be evaluated at the wrong slot.
    World w;
    const ObjectId stacked = w.createPrimitive(Primitive::Cube, "stacked");
    const ObjectId mirrorOnly = w.createPrimitive(Primitive::Cube, "mirrorOnly");

    ArrayParams ap; ap.count = 3; ap.offsetX = 2.0f; ap.relative = false;
    MirrorParams mp; mp.axisX = true; mp.mergeThreshold = 0.0f;

    CHECK(w.addModifier(stacked, ModifierKind::Array));
    CHECK(w.addModifier(stacked, ModifierKind::Mirror));
    CHECK(w.setModifierParams(stacked, 0, ModifierParams{ap}));
    CHECK(w.setModifierParams(stacked, 1, ModifierParams{mp}));
    CHECK(w.addModifier(mirrorOnly, ModifierKind::Mirror));
    CHECK(w.setModifierParams(mirrorOnly, 0, ModifierParams{mp}));

    CHECK(w.setModifierEnabled(stacked, 0, false));
    CHECK_MSG(identical(evaluatedViaPacket(w, stacked), evaluatedViaPacket(w, mirrorOnly)),
              "disabling index 0 did not leave exactly modifier 1's result",
              "disabled entries must be SKIPPED IN PLACE, not removed — if "
              "indices shifted, index 1 would be evaluated as if it were index 0");
}

// ---- (8) THE REAL STRUCTURAL-FAILURE PATH, driven directly ------------------
//
// The World API cannot produce a corrupt Object::mesh, so these drive the
// scene-exported ember::scene::evaluateMeshStack (Scene.h) directly, as Slate
// recommended. Expectations come from Slate's own run; I assert them here
// independently rather than taking his receipt on trust.

namespace {
std::vector<Modifier> subdivStack() {
    Modifier m;
    SubdivisionParams sp; sp.levels = 1;
    m.params = sp;
    return {m};
}
MeshData oneTri() {
    MeshData md;
    md.positions = {0,0,0, 1,0,0, 0,1,0};
    md.normals   = {0,0,1, 0,0,1, 0,0,1};
    md.uvs       = {0,0, 1,0, 0,1};
    md.indices   = {0,1,2};
    return md;
}
} // namespace

TEST(modifiers_failure_out_of_range_index_names_index_0_and_returns_input) {
    MeshData bad = oneTri();
    bad.indices = {0, 1, 99};                       // 99 is out of range
    u32 failed = 0;
    const MeshData out = ember::scene::evaluateMeshStack(bad, subdivStack(), &failed);
    CHECK_MSG(failed == 0u, "failedIndex should name modifier 0, got %u", failed);
    CHECK_MSG(identical(out, bad), "failure must return the INCOMING mesh bit-identical, "
              "not an empty or partial one");
    CHECK(!out.indices.empty());
}

TEST(modifiers_failure_index_count_not_multiple_of_3_is_refused) {
    MeshData bad = oneTri();
    bad.indices = {0, 1, 2, 0};                     // length 4, not a multiple of 3
    u32 failed = 0;
    const MeshData out = ember::scene::evaluateMeshStack(bad, subdivStack(), &failed);
    CHECK_MSG(failed == 0u, "failedIndex should name modifier 0, got %u", failed);
    CHECK_MSG(identical(out, bad), "refused input must come back intact");
}

TEST(modifiers_failure_later_index_keeps_earlier_good_result) {
    // [Array, Subdivision] on a corrupt-for-subdivision mesh is impossible to
    // construct (array would fail first on nothing), so use [disabled Subdiv,
    // Subdiv]: the disabled slot is skipped in place, the second fails, and the
    // reported index must be 1 — the SLOT, not a count of enabled entries.
    MeshData bad = oneTri();
    bad.indices = {0, 1, 99};
    std::vector<Modifier> st = subdivStack();
    st.push_back(st[0]);
    st[0].enabled = false;
    u32 failed = 0;
    const MeshData out = ember::scene::evaluateMeshStack(bad, st, &failed);
    CHECK_MSG(failed == 1u, "failedIndex must name the STACK SLOT (1), got %u", failed);
    CHECK(identical(out, bad));
}

TEST(modifiers_healthy_direct_evaluation_reports_sentinel) {
    u32 failed = 0;
    const MeshData out = ember::scene::evaluateMeshStack(oneTri(), subdivStack(), &failed);
    CHECK_MSG(failed == 0xFFFFFFFFu, "healthy stack reported failure %u", failed);
    CHECK_MSG(out.indexCount() == 18u, "level-1 subdivision of one triangle: expected 18 "
              "indices (Slate's measurement), got %u", out.indexCount());
}

HARNESS_MAIN()
