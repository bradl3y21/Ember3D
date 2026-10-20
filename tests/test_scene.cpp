// tests/test_scene.cpp — owned by Verity · QA.
//
// Contract under test: ARCHITECTURE.md §4.4 (scene/World) as seen through its
// PUBLIC API only. Where §4.4 does not specify something (the layout of
// `Object`, where a primitive's name lands, camera fields of the packet),
// this file deliberately does NOT assert it — those gaps are reported to the
// owners instead of guessed at.
//
// Observation strategy: `World::buildPacket` is const and fully specified by
// §4.1/§4.4, so world state (transforms, hierarchy, materials, membership) is
// verified through the packet it produces. Pure data, no GPU, no context.
#include "harness.h"

#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using ember::Color;
using ember::DrawItem;
using ember::Mat4;
using ember::MaterialDesc;
using ember::MeshData;
using ember::ObjectId;
using ember::RenderPacket;
using ember::Transform;
using ember::Vec3;
using ember::scene::Primitive;
using ember::scene::EditableMesh;
using ember::scene::Object;
using ember::scene::World;

namespace {

std::vector<DrawItem> packetItems(const World& w, int width = 800, int height = 600) {
    return w.buildPacket(width, height).items;
}

const DrawItem* findItem(const std::vector<DrawItem>& items, ObjectId id) {
    for (const DrawItem& d : items)
        if (d.object.value == id.value) return &d;
    return nullptr;
}

Vec3 applyColMajor(const Mat4& m, Vec3 v) {
    return Vec3{
        m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
        m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14],
    };
}

Mat4 mul4(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

bool vecNear(Vec3 a, Vec3 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

std::vector<Primitive> allPrimitives() {
    return {Primitive::Cube, Primitive::Sphere, Primitive::Plane,
            Primitive::Cylinder, Primitive::Torus};
}

EditableMesh makeNgon(const std::vector<Vec3>& points, ember::u32 materialIndex) {
    EditableMesh mesh;
    mesh.positions = points;
    mesh.faces.push_back(ember::scene::Face{0u,
        static_cast<ember::u32>(points.size()), materialIndex});
    mesh.loops.resize(points.size());
    for (ember::u32 i = 0; i < points.size(); ++i) {
        const ember::u32 next = (i + 1) % static_cast<ember::u32>(points.size());
        mesh.edges.push_back(ember::scene::Edge{std::min(i, next), std::max(i, next)});
        mesh.loops[i].vertex = i;
        mesh.loops[i].edge = i;
        mesh.loops[i].face = 0;
        mesh.loops[i].next = next;
        mesh.loops[i].prev = (i + static_cast<ember::u32>(points.size()) - 1) %
                             static_cast<ember::u32>(points.size());
    }
    mesh.rebuildAdjacency();
    return mesh;
}

const char* primName(Primitive p) {
    switch (p) {
        case Primitive::Cube:     return "Cube";
        case Primitive::Sphere:   return "Sphere";
        case Primitive::Plane:    return "Plane";
        case Primitive::Cylinder: return "Cylinder";
        case Primitive::Torus:    return "Torus";
    }
    return "?";
}

} // namespace

// ---- object lifecycle ------------------------------------------------------

TEST(world_starts_empty) {
    World w;
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(scene_render_settings_default_sanitize_and_packet_snapshot) {
    World w;
    CHECK(w.renderSettings().engine == ember::RenderEngine::Raster);
    CHECK_EQ((int)w.renderSettings().samplesPerPixel, 64);
    CHECK_EQ((int)w.renderSettings().maxBounces, 4);
    CHECK_EQ((int)w.renderSettings().seed, 1);

    ember::SceneRenderSettings requested;
    requested.engine = ember::RenderEngine::Cycles;
    requested.samplesPerPixel = 0;
    requested.maxBounces = ember::kMaxRenderBounces + 1;
    requested.seed = 0x12345678u;
    w.setRenderSettings(requested);
    CHECK(w.renderSettings().engine == ember::RenderEngine::Cycles);
    CHECK_EQ((int)w.renderSettings().samplesPerPixel,
             (int)ember::kMinRenderSamplesPerPixel);
    CHECK_EQ((int)w.renderSettings().maxBounces,
             (int)ember::kMaxRenderBounces);
    CHECK_EQ((int)w.renderSettings().seed, (int)requested.seed);

    const RenderPacket packet = w.buildPacket(64, 64);
    requested.samplesPerPixel = 8;
    requested.maxBounces = 0;
    w.setRenderSettings(requested);
    CHECK(packet.renderSettings.engine == ember::RenderEngine::Cycles);
    CHECK_EQ((int)packet.renderSettings.samplesPerPixel,
             (int)ember::kMinRenderSamplesPerPixel);
    CHECK_EQ((int)packet.renderSettings.maxBounces,
             (int)ember::kMaxRenderBounces);
    CHECK_EQ((int)w.renderSettings().samplesPerPixel, 8);
    CHECK_EQ((int)w.renderSettings().maxBounces, 0);
}

TEST(image_texture_defaults_and_world_packet_carries_asset_base) {
    World w;
    const std::string current =
        std::filesystem::current_path().lexically_normal().generic_string();
    CHECK(w.assetBaseDirectory() == current);
    const ember::ImageTextureRef unassigned;
    CHECK(unassigned.path.empty());
    CHECK(unassigned.colorSpace == ember::ImageColorSpace::SRgb);

    const ObjectId id = w.createPrimitive(Primitive::Cube);
    MaterialDesc material;
    material.baseColorTexture = {"textures/albedo.ppm", ember::ImageColorSpace::SRgb};
    w.setMaterial(id, material);
    ember::WorldBackground background;
    background.environmentTexture = {"world/studio.pfm", ember::ImageColorSpace::Linear};
    w.setBackground(background);

    const RenderPacket packet = w.buildPacket(32, 32);
    CHECK(packet.assetBaseDirectory == w.assetBaseDirectory());
    CHECK(packet.background.environmentTexture.path == "world/studio.pfm");
    CHECK(packet.background.environmentTexture.colorSpace == ember::ImageColorSpace::Linear);
    CHECK_EQ((int)packet.items.size(), 1);
    if (!packet.items.empty()) {
        CHECK(packet.items[0].material.baseColorTexture.path == "textures/albedo.ppm");
        CHECK(packet.items[0].materials != nullptr);
        if (packet.items[0].materials)
            CHECK(packet.items[0].materials[0].material.baseColorTexture ==
                  material.baseColorTexture);
    }
}

TEST(scene_render_settings_invalid_enum_and_upper_sample_limit_are_sanitized) {
    World w;
    ember::SceneRenderSettings malformed;
    malformed.engine = static_cast<ember::RenderEngine>(255);
    malformed.samplesPerPixel = ember::kMaxRenderSamplesPerPixel + 1;
    malformed.maxBounces = 0;
    malformed.seed = 99;
    w.setRenderSettings(malformed);
    CHECK(w.renderSettings().engine == ember::RenderEngine::Raster);
    CHECK_EQ((int)w.renderSettings().samplesPerPixel,
             (int)ember::kMaxRenderSamplesPerPixel);
    CHECK_EQ((int)w.renderSettings().maxBounces, 0);
    CHECK_EQ((int)w.renderSettings().seed, 99);
}

TEST(createObject_returns_valid_unique_ids) {
    World w;
    ObjectId a = w.createObject("a");
    ObjectId b = w.createObject("b");
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a.value != b.value);
    CHECK_EQ((int)w.objects().size(), 2);
    CHECK(w.get(a) != nullptr);
    CHECK(w.get(b) != nullptr);
}

TEST(get_on_unknown_id_returns_null) {
    World w;
    CHECK(w.get(ObjectId{9999u}) == nullptr);
    CHECK(w.get(ObjectId{0}) == nullptr);  // 0 is never a valid id (§4.1)
}

TEST(destroy_removes_object) {
    World w;
    ObjectId a = w.createObject("a");
    CHECK(w.destroy(a));
    CHECK(w.get(a) == nullptr);
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(destroy_unknown_id_returns_false) {
    World w;
    CHECK(!w.destroy(ObjectId{12345u}));
    CHECK(!w.destroy(ObjectId{0}));
}

TEST(destroyed_id_can_be_observed_as_gone_from_packet) {
    // Uses primitives (which DO emit DrawItems) so "gone from the packet" is
    // observable. Unmeshed objects emit no DrawItem at all (see
    // buildPacket_unmeshed_object_emits_no_item).
    World w;
    ObjectId a = w.createPrimitive(Primitive::Cube, "a");
    ObjectId b = w.createPrimitive(Primitive::Sphere, "b");
    CHECK(w.destroy(a));
    const std::vector<DrawItem> items = packetItems(w);
    CHECK(findItem(items, a) == nullptr);
    CHECK(findItem(items, b) != nullptr);
}

// ---- transforms through the packet ------------------------------------------

TEST(setTransform_appears_in_packet_model_matrix) {
    // Observes the model matrix through the packet, so the object must be
    // meshed (unmeshed objects emit no DrawItem).
    World w;
    ObjectId a = w.createPrimitive(Primitive::Cube, "a");
    Transform t{};
    t.position = {1.0f, 2.0f, 3.0f};
    w.setTransform(a, t);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, a);
    CHECK(d != nullptr);
    if (d) {
        CHECK(vecNear(applyColMajor(d->model, {0, 0, 0}), {1, 2, 3}));
        CHECK(matNear(d->model, t.toMatrix()));
    }
}

TEST(createPrimitive_then_setTransform_matches_toMatrix) {
    World w;
    ObjectId a = w.createPrimitive(Primitive::Sphere, "s");
    Transform t{};
    t.position = {-5.0f, 0.0f, 2.0f};
    t.scale    = {2.0f, 2.0f, 2.0f};
    w.setTransform(a, t);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, a);
    CHECK(d != nullptr);
    if (d) CHECK(matNear(d->model, t.toMatrix()));
}

TEST(worldMatrix_matches_packet_model) {
    // worldMatrix(id) is the scene's own world-transform query; it must agree
    // with what buildPacket put in DrawItem::model for the same object.
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "p");
    ObjectId child  = w.createPrimitive(Primitive::Cube, "c");
    Transform tp{}; tp.position = {10.0f, 0.0f, 0.0f};
    Transform tc{}; tc.position = {0.0f, 5.0f, 0.0f}; tc.scale = {2, 2, 2};
    w.setTransform(parent, tp);
    w.setTransform(child, tc);
    w.setParent(child, parent);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* dc = findItem(items, child);
    CHECK(dc != nullptr);
    if (dc) CHECK(matNear(dc->model, w.worldMatrix(child)));
}

// ---- parenting / hierarchy --------------------------------------------------

TEST(parent_child_model_is_parent_times_child) {
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId child  = w.createPrimitive(Primitive::Cube, "child");
    Transform tp{}; tp.position = {10.0f, 0.0f, 0.0f};
    Transform tc{}; tc.position = {0.0f, 5.0f, 0.0f};
    w.setTransform(parent, tp);
    w.setTransform(child, tc);
    w.setParent(child, parent);

    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* dp = findItem(items, parent);
    const DrawItem* dc = findItem(items, child);
    CHECK(dp && dc);
    if (dp && dc) {
        CHECK(matNear(dc->model, mul4(dp->model, tc.toMatrix())));
        // Child's world position ends up at (10, 5, 0).
        CHECK(vecNear(applyColMajor(dc->model, {0, 0, 0}), {10.0f, 5.0f, 0.0f}));
    }
}

TEST(reparenting_moves_child_to_new_parent) {
    World w;
    ObjectId p1 = w.createPrimitive(Primitive::Cube, "p1");
    ObjectId p2 = w.createPrimitive(Primitive::Cube, "p2");
    ObjectId child = w.createPrimitive(Primitive::Cube, "child");
    Transform t1{}; t1.position = {100.0f, 0.0f, 0.0f};
    Transform t2{}; t2.position = {0.0f, 0.0f, 100.0f};
    w.setTransform(p1, t1);
    w.setTransform(p2, t2);
    w.setParent(child, p1);
    w.setParent(child, p2);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* dc = findItem(items, child);
    CHECK(dc != nullptr);
    if (dc) CHECK(vecNear(applyColMajor(dc->model, {0, 0, 0}), {0.0f, 0.0f, 100.0f}));
}

TEST(destroyed_parent_reparents_child_to_grandparent_semantics) {
    // Scene.h documents destroy() as "reparents the object's children to its
    // parent first, then destroys it". Destroying a ROOT parent therefore
    // detaches the child (its new parent is the destroyed one's parent = none),
    // so the child's world transform becomes its own local transform.
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId child  = w.createPrimitive(Primitive::Cube, "child");
    Transform tp{}; tp.position = {50.0f, 0.0f, 0.0f};
    Transform tc{}; tc.position = {1.0f, 2.0f, 3.0f};
    w.setTransform(parent, tp);
    w.setTransform(child, tc);
    w.setParent(child, parent);
    CHECK(w.destroy(parent));
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* dc = findItem(items, child);
    CHECK(dc != nullptr);
    if (dc) {
        const Vec3 p = applyColMajor(dc->model, {0, 0, 0});
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        // Orphaned child keeps its OWN local transform (no dangling parent).
        CHECK(vecNear(p, {1.0f, 2.0f, 3.0f}));
    }
}

TEST(destroyed_parent_leaves_child_with_valid_transform) {
    // Kept minimal: §4.4 does not itself specify orphan handling, so the only
    // hard requirement here is a well-formed (finite) model matrix. The
    // stronger "orphan keeps its own local transform" claim is asserted in
    // destroyed_parent_reparents_child_to_grandparent_semantics, based on
    // Scene.h's documented destroy() contract.
    World w;
    ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    ObjectId child  = w.createPrimitive(Primitive::Cube, "child");
    Transform tp{}; tp.position = {50.0f, 0.0f, 0.0f};
    w.setTransform(parent, tp);
    w.setParent(child, parent);
    CHECK(w.destroy(parent));
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* dc = findItem(items, child);
    CHECK(dc != nullptr);
    if (dc) {
        const Vec3 p = applyColMajor(dc->model, {0, 0, 0});
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
}

// ---- selection --------------------------------------------------------------

TEST(select_and_selection) {
    World w;
    ObjectId a = w.createObject("a");
    ObjectId b = w.createObject("b");
    w.select(a);
    CHECK_EQ((int)w.selection().size(), 1);
    w.select(b, /*additive=*/true);
    CHECK_EQ((int)w.selection().size(), 2);
    w.clearSelection();
    CHECK_EQ((int)w.selection().size(), 0);
}

TEST(select_nonadditive_replaces_selection) {
    World w;
    ObjectId a = w.createObject("a");
    ObjectId b = w.createObject("b");
    w.select(a);
    w.select(b);  // not additive: previous selection must go
    CHECK_EQ((int)w.selection().size(), 1);
    CHECK(w.selection().size() == 1 && w.selection()[0].value == b.value);
}

TEST(selection_of_destroyed_object_is_not_dangling) {
    World w;
    ObjectId a = w.createObject("a");
    w.select(a);
    CHECK(w.destroy(a));
    for (ObjectId id : w.selection()) CHECK(w.get(id) != nullptr);
}

// ---- materials --------------------------------------------------------------

TEST(material_emission_defaults_to_linear_black_and_zero_strength) {
    const MaterialDesc material{};
    CHECK_NEAR(material.emissionColor.r, 0.0f, 1e-6);
    CHECK_NEAR(material.emissionColor.g, 0.0f, 1e-6);
    CHECK_NEAR(material.emissionColor.b, 0.0f, 1e-6);
    CHECK_NEAR(material.emissionStrength, 0.0f, 1e-6);

    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* item = findItem(items, id);
    CHECK(item != nullptr);
    if (item) {
        CHECK_NEAR(item->material.emissionColor.r, 0.0f, 1e-6);
        CHECK_NEAR(item->material.emissionColor.g, 0.0f, 1e-6);
        CHECK_NEAR(item->material.emissionColor.b, 0.0f, 1e-6);
        CHECK_NEAR(item->material.emissionStrength, 0.0f, 1e-6);
    }
}

TEST(material_dielectric_defaults_and_values_reach_packet_slots) {
    const MaterialDesc defaults{};
    CHECK_NEAR(defaults.transmission, 0.0f, 1e-6);
    CHECK_NEAR(defaults.ior, 1.45f, 1e-6);

    World w;
    const ObjectId id = w.createPrimitive(Primitive::Cube);
    MaterialDesc base;
    base.transmission = 0.65f;
    base.ior = 1.33f;
    w.setMaterial(id, base);
    MaterialDesc additional;
    additional.transmission = 0.25f;
    additional.ior = 1.52f;
    const ember::u32 slot = w.addMaterialSlot(id, additional, "Dielectric");

    const auto items = packetItems(w);
    const DrawItem* item = findItem(items, id);
    CHECK(item != nullptr);
    if (!item) return;
    CHECK_NEAR(item->material.transmission, 0.65f, 1e-6);
    CHECK_NEAR(item->material.ior, 1.33f, 1e-6);
    CHECK(item->materials != nullptr);
    if (item->materials && slot < item->materialCount) {
        CHECK_NEAR(item->materials[slot].material.transmission, 0.25f, 1e-6);
        CHECK_NEAR(item->materials[slot].material.ior, 1.52f, 1e-6);
    }
}

TEST(material_sanitization_bounds_dielectric_values_and_keeps_hdr_emission) {
    MaterialDesc malformed;
    malformed.transmission = 1.5f;
    malformed.ior = std::numeric_limits<float>::quiet_NaN();
    malformed.emissionColor = Color{2.5f, -1.0f,
                                    std::numeric_limits<float>::infinity(), 1.0f};
    malformed.emissionStrength = 3.0f;
    const MaterialDesc clean = ember::sanitizedMaterialDesc(malformed);
    CHECK_NEAR(clean.transmission, 1.0f, 1e-6);
    CHECK_NEAR(clean.ior, 1.45f, 1e-6);
    CHECK_NEAR(clean.emissionColor.r, 2.5f, 1e-6);
    CHECK_NEAR(clean.emissionColor.g, 0.0f, 1e-6);
    CHECK_NEAR(clean.emissionColor.b, 0.0f, 1e-6);
    CHECK_NEAR(clean.emissionStrength, 3.0f, 1e-6);

    malformed.transmission = -0.25f;
    malformed.ior = 3.5f;
    const MaterialDesc invalid = ember::sanitizedMaterialDesc(malformed);
    CHECK_NEAR(invalid.transmission, 0.0f, 1e-6);
    CHECK_NEAR(invalid.ior, 1.45f, 1e-6);
}

TEST(setMaterial_lands_on_the_right_item) {
    World w;
    ObjectId a = w.createPrimitive(Primitive::Cube, "a");
    ObjectId b = w.createPrimitive(Primitive::Sphere, "b");
    MaterialDesc red{{1.0f, 0.0f, 0.0f, 1.0f}, 0.2f, 0.8f};
    w.setMaterial(a, red);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* da = findItem(items, a);
    const DrawItem* db = findItem(items, b);
    CHECK(da && db);
    if (da) {
        CHECK_NEAR(da->material.baseColor.r, 1.0f, 1e-6);
        CHECK_NEAR(da->material.baseColor.g, 0.0f, 1e-6);
        CHECK_NEAR(da->material.metallic,  0.2f, 1e-6);
        CHECK_NEAR(da->material.roughness, 0.8f, 1e-6);
        CHECK(da->materials != nullptr);
        CHECK_EQ((int)da->materialCount, 1);
        if (da->materials && da->materialCount > 0) {
            CHECK_NEAR(da->materials[0].material.baseColor.r, 1.0f, 1e-6);
            CHECK_NEAR(da->materials[0].material.baseColor.g, 0.0f, 1e-6);
            CHECK_NEAR(da->materials[0].material.metallic,  0.2f, 1e-6);
            CHECK_NEAR(da->materials[0].material.roughness, 0.8f, 1e-6);
        }
    }
    if (db) {  // untouched object keeps §4.1 defaults
        CHECK_NEAR(db->material.roughness, 0.5f, 1e-6);
        CHECK_NEAR(db->material.metallic,  0.0f, 1e-6);
    }
}

TEST(buildPacket_maps_every_primitive_triangle_to_default_slot_zero) {
    World w;
    const ObjectId cube = w.createPrimitive(Primitive::Cube);
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, cube);
    CHECK(d != nullptr);
    if (!d || !d->mesh) return;
    CHECK_EQ((int)d->materialCount, 1);
    CHECK(d->materials != nullptr);
    CHECK_EQ((int)d->triangleMaterialSlotCount,
             (int)(d->mesh->indexCount() / 3));
    CHECK(d->triangleMaterialSlots != nullptr);
    for (ember::u32 i = 0; i < d->triangleMaterialSlotCount; ++i)
        CHECK_EQ((int)d->triangleMaterialSlots[i], 0);
}

TEST(buildPacket_maps_each_editable_triangle_from_its_face_material) {
    World w;
    const ObjectId plane = w.createPrimitive(Primitive::Plane);
    const ember::u32 slot1 = w.addMaterialSlot(plane, MaterialDesc{}, "First");
    const ember::u32 slot2 = w.addMaterialSlot(plane, MaterialDesc{}, "Second");
    EditableMesh* edit = w.editable(plane);
    CHECK(edit != nullptr);
    if (!edit || edit->faces.size() != 2) return;
    edit->faces[0].materialIndex = slot1;
    edit->faces[1].materialIndex = slot2;

    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, plane);
    CHECK(d != nullptr);
    if (!d || !d->mesh) return;
    CHECK_EQ((int)d->triangleMaterialSlotCount,
             (int)(d->mesh->indexCount() / 3));
    CHECK_EQ((int)d->triangleMaterialSlotCount, 2);
    CHECK(d->triangleMaterialSlots != nullptr);
    if (d->triangleMaterialSlots) {
        CHECK_EQ((int)d->triangleMaterialSlots[0], (int)slot1);
        CHECK_EQ((int)d->triangleMaterialSlots[1], (int)slot2);
    }
}

TEST(buildPacket_preserves_triangle_materials_through_array_modifier) {
    World w;
    const ObjectId plane = w.createPrimitive(Primitive::Plane);
    const ember::u32 slot1 = w.addMaterialSlot(plane, MaterialDesc{}, "First");
    const ember::u32 slot2 = w.addMaterialSlot(plane, MaterialDesc{}, "Second");
    EditableMesh* edit = w.editable(plane);
    CHECK(edit != nullptr);
    if (!edit || edit->faces.size() != 2) return;
    edit->faces[0].materialIndex = slot1;
    edit->faces[1].materialIndex = slot2;

    ember::Modifier array;
    array.params = ember::ArrayParams{2, 1.0f, 0.0f, 0.0f, false};
    CHECK(w.setModifierStack(plane, {array}));
    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, plane);
    CHECK(d != nullptr);
    if (!d || !d->mesh) return;
    CHECK_EQ((int)d->triangleMaterialSlotCount,
             (int)(d->mesh->indexCount() / 3));
    CHECK_EQ((int)d->triangleMaterialSlotCount, 4);
    CHECK(d->triangleMaterialSlots != nullptr);
    if (d->triangleMaterialSlots) {
        CHECK_EQ((int)d->triangleMaterialSlots[0], (int)slot1);
        CHECK_EQ((int)d->triangleMaterialSlots[1], (int)slot2);
        CHECK_EQ((int)d->triangleMaterialSlots[2], (int)slot1);
        CHECK_EQ((int)d->triangleMaterialSlots[3], (int)slot2);
    }
}

TEST(buildPacket_repeats_face_material_for_all_ngon_triangles) {
    World w;
    const ObjectId polygon = w.createObject("Concave polygon");
    const ember::u32 materialSlot =
        w.addMaterialSlot(polygon, MaterialDesc{}, "Polygon material");
    Object* object = w.get(polygon);
    CHECK(object != nullptr);
    if (!object) return;
    object->hasMesh = true;
    object->hasEdit = true;
    object->edit = makeNgon({Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{2, 2, 0},
                             Vec3{1, 1, 0}, Vec3{0, 2, 0}}, materialSlot);
    object->mesh = object->edit.toMeshData();

    const std::vector<DrawItem> items = packetItems(w);
    const DrawItem* d = findItem(items, polygon);
    CHECK(d != nullptr);
    if (!d || !d->mesh) return;
    CHECK_EQ((int)d->mesh->indexCount(), 9);
    CHECK_EQ((int)d->triangleMaterialSlotCount, 3);
    CHECK(d->triangleMaterialSlots != nullptr);
    for (ember::u32 i = 0; i < d->triangleMaterialSlotCount; ++i)
        CHECK_EQ((int)d->triangleMaterialSlots[i], (int)materialSlot);
}

// ---- buildPacket ------------------------------------------------------------

TEST(buildPacket_one_item_per_live_MESHED_object) {
    // Per D9 (docs/ROADMAP.md), DrawItem::mesh points INTO Object::mesh, which
    // exists only for meshed objects. So the packet carries one item per live
    // meshed object — not per live object. Unmeshed objects emit no item.
    World w;
    for (int i = 0; i < 5; ++i) w.createPrimitive(Primitive::Cube, "o" + std::to_string(i));
    ObjectId unmeshed = w.createObject("groupStyle");
    CHECK(unmeshed.valid());
    ObjectId doomed = w.createPrimitive(Primitive::Sphere, "doomed");
    CHECK(w.destroy(doomed));
    CHECK_EQ((int)packetItems(w).size(), 5);        // 5 meshed survive, doomed gone
    CHECK_EQ(w.buildPacket(800, 600).width, 800);
    CHECK_EQ(w.buildPacket(800, 600).height, 600);
}

TEST(buildPacket_unmeshed_object_emits_no_item) {
    // mesh none => hasMesh false => no DrawItem (nothing to draw; a null-mesh
    // DrawItem would be a renderer hazard). This is the documented contract
    // via D9 + FORMAT.md §3's "mesh none marks an unmeshed object".
    World w;
    w.createObject("group");
    CHECK_EQ((int)packetItems(w).size(), 0);
}

TEST(buildPacket_items_reference_live_meshes) {
    // §4.1: DrawItem::mesh is non-owning; it must point at something valid
    // for every item, and be null-free.
    World w;
    w.createPrimitive(Primitive::Cube);
    w.createObject("empty");   // emits no item; the cube's item must still be valid
    const std::vector<DrawItem> items = packetItems(w);
    CHECK_EQ((int)items.size(), 1);
    for (const DrawItem& d : items) CHECK(d.mesh != nullptr);
}

TEST(buildPacket_of_empty_world_is_empty_not_crash) {
    World w;
    CHECK_EQ((int)packetItems(w).size(), 0);
    CHECK_EQ((int)w.buildPacket(0, 0).items.size(), 0);
}

// ---- primitive generation ----------------------------------------------------
// Checked through the packet's MeshData (fully specified in §4.1):
// non-empty, well-formed counts, in-bounds indices, unit normals, watertight
// for the solids (plane exempt).

TEST(primitives_generate_wellformed_meshes) {
    for (Primitive p : allPrimitives()) {
        World w;
        w.createPrimitive(p);
        const std::vector<DrawItem> items = packetItems(w);
        CHECK_EQ((int)items.size(), 1);
        const DrawItem* d = items.empty() ? nullptr : &items[0];
        if (!d || !d->mesh) continue;
        const MeshData& mesh = *d->mesh;

        const std::string ctx = primName(p);
        if (mesh.positions.empty())
            harness::reportFailure(__FILE__, __LINE__, ctx + ": positions empty");
        if (mesh.positions.size() % 3 != 0)
            harness::reportFailure(__FILE__, __LINE__, ctx + ": positions not xyz-triples");
        if (mesh.normals.size() != mesh.positions.size())
            harness::reportFailure(__FILE__, __LINE__, ctx + ": normals count != positions count");
        if (mesh.uvs.size() * 3 != mesh.positions.size() * 2)
            harness::reportFailure(__FILE__, __LINE__, ctx + ": uvs not 2 per vertex");
        if (mesh.indices.empty() || mesh.indices.size() % 3 != 0)
            harness::reportFailure(__FILE__, __LINE__, ctx + ": indices not triangle list");
        ++harness::g_checks;

        const std::size_t vertexCount = mesh.positions.size() / 3;
        bool indicesInBounds = true;
        for (ember::u32 idx : mesh.indices)
            if (idx >= vertexCount) indicesInBounds = false;
        CHECK(indicesInBounds);

        bool normalsUnit = true;
        for (std::size_t i = 0; i + 2 < mesh.normals.size(); i += 3) {
            const float len = std::sqrt(mesh.normals[i] * mesh.normals[i] +
                                        mesh.normals[i + 1] * mesh.normals[i + 1] +
                                        mesh.normals[i + 2] * mesh.normals[i + 2]);
            if (std::fabs(len - 1.0f) > 1e-3f) normalsUnit = false;
        }
        CHECK(normalsUnit);
    }
}

TEST(solid_primitives_are_watertight) {
    // Watertight = geometrically closed. Two subtleties, both established by
    // measurement rather than assumption (QA diagnostics on the real meshes):
    //   1. Raw index-parity is INVALID here: the generators duplicate vertices
    //      for per-face/per-seam normals (a cube is 24 verts, not 8), so no
    //      index is shared between triangles. We must weld by position first.
    //   2. After welding, a UV-seam edge is legitimately shared by FOUR
    //      triangles (the seam's two coincident vertices merge), so the test
    //      asserts every edge is used an EVEN number of times, not exactly
    //      two. An ODD count is a genuine hole.
    // Measured result: cube/cylinder/torus have 0 bad edges; sphere has 64
    // non-2 edges, all used exactly 4x (pure seam), 0 odd => all CLOSED.
    const float weldEps = 1e-5f;
    for (Primitive p : allPrimitives()) {
        if (p == Primitive::Plane) continue;  // a plane is open by definition
        World w;
        w.createPrimitive(p);
        const std::vector<DrawItem> items = packetItems(w);
        if (items.empty() || !items[0].mesh) continue;
        const MeshData& mesh = *items[0].mesh;

        const std::size_t verts = mesh.positions.size() / 3;
        const std::size_t tris  = mesh.indices.size() / 3;

        // Weld coincident vertices by brute-force proximity (n is small).
        std::vector<ember::u32> repOf;
        std::vector<ember::u32> remap(verts);
        for (std::size_t v = 0; v < verts; ++v) {
            const float* pv = &mesh.positions[v * 3];
            bool placed = false;
            for (ember::u32 c = 0; c < repOf.size(); ++c) {
                const float* pr = &mesh.positions[repOf[c] * 3];
                const float dx = pv[0] - pr[0], dy = pv[1] - pr[1], dz = pv[2] - pr[2];
                if (dx * dx + dy * dy + dz * dz <= weldEps * weldEps) {
                    remap[v] = c; placed = true; break;
                }
            }
            if (!placed) { remap[v] = (ember::u32)repOf.size(); repOf.push_back((ember::u32)v); }
        }

        std::map<std::pair<ember::u32, ember::u32>, int> edgeCount;
        for (std::size_t t = 0; t < tris; ++t) {
            const ember::u32 e[3] = {remap[mesh.indices[t * 3]],
                                     remap[mesh.indices[t * 3 + 1]],
                                     remap[mesh.indices[t * 3 + 2]]};
            for (int j = 0; j < 3; ++j) {
                ember::u32 a = e[j], b = e[(j + 1) % 3];
                if (a == b) continue;  // degenerate edge
                if (b < a) std::swap(a, b);
                ++edgeCount[{a, b}];
            }
        }
        bool closed = !edgeCount.empty();
        std::string detail;
        for (const auto& [edge, n] : edgeCount)
            if (n % 2 != 0) {  // odd use = a real boundary hole
                closed = false;
                detail = " (edge " + std::to_string(edge.first) + "," +
                         std::to_string(edge.second) + " used " +
                         std::to_string(n) + "x)";
                break;
            }
        if (!closed)
            harness::reportFailure(__FILE__, __LINE__,
                std::string(primName(p)) + ": mesh is not closed (an odd-use "
                "edge remains after welding coincident vertices)" + detail);
        ++harness::g_checks;
    }
}

TEST(primitive_names_dont_collide_with_ids) {
    // Two primitives of different kinds get distinct ids and both survive.
    World w;
    ObjectId c = w.createPrimitive(Primitive::Cube);
    ObjectId s = w.createPrimitive(Primitive::Sphere);
    CHECK(c.valid() && s.valid());
    CHECK(c.value != s.value);
    CHECK_EQ((int)packetItems(w).size(), 2);
}

// ---- box selection (pickBox, D22) -------------------------------------------
// Contract read from Scene.h before writing: returns objects whose WORLD-space
// AABB intersects the box; invisible, collection-hidden and unmeshed objects
// are NEVER returned; lights are never returned.

TEST(pickbox_selects_an_object_inside_the_box) {
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "in");
    w.setTransform(o, Transform{});   // cube at origin
    // A box that contains the whole cube.
    const auto hits = w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2});
    CHECK_EQ((int)hits.size(), 1);
    if (!hits.empty()) CHECK(hits[0].value == o.value);
}

TEST(pickbox_excludes_an_object_outside_the_box) {
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "far");
    Transform t{}; t.position = {50.0f, 0.0f, 0.0f};
    w.setTransform(o, t);
    // Box nowhere near the object.
    const auto hits = w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2});
    bool found = false;
    for (const ObjectId id : hits) if (id.value == o.value) found = true;
    CHECK(!found);
}

TEST(pickbox_straddling_edge_is_included) {
    // An AABB test is inclusive of touching. A box whose face sits exactly on
    // the cube's far edge must still catch it — this pins the boundary
    // convention so a future < vs <= change is a visible break.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "edge");
    w.setTransform(o, Transform{});   // unit cube: half-extent 0.5
    const auto hits = w.pickBox(Vec3{-2, -2, -2}, Vec3{0.5f, 0.5f, 0.5f});
    bool found = false;
    for (const ObjectId id : hits) if (id.value == o.value) found = true;
    CHECK(found);                     // touching the far corner counts as inside
}

TEST(pickbox_excludes_unmeshed_objects) {
    // Unmeshed (groups, lights) have no AABB; Scene.h says they are NEVER
    // returned. Assert both a plain group and a light are absent.
    World w;
    w.createObject("groupOnly");                      // unmeshed
    w.createLight(ember::LightType::Directional, "Sun");  // light: also unmeshed
    const ObjectId cube = w.createPrimitive(Primitive::Cube, "cube");
    w.setTransform(cube, Transform{});

    const auto hits = w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2});
    CHECK_EQ((int)hits.size(), 1);                    // ONLY the cube
    if (!hits.empty()) CHECK(hits[0].value == cube.value);
}

TEST(pickbox_excludes_invisible_objects) {
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "ghost");
    w.setTransform(o, Transform{});
    w.setVisible(o, false);
    const auto hits = w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2});
    CHECK_EQ((int)hits.size(), 0);
}

TEST(pickbox_excludes_collection_hidden_objects_rule3) {
    // D22 rule 3: collection-hidden objects are excluded from picking.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "hiddenBin");
    w.setTransform(o, Transform{});
    const auto coll = w.createCollection("Bin");
    w.assignToCollection(o, coll);
    CHECK_EQ((int)w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2}).size(), 1);
    w.setCollectionVisible(coll, false);
    CHECK_EQ((int)w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2}).size(), 0);
    w.setCollectionVisible(coll, true);
    CHECK_EQ((int)w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2}).size(), 1);
}

TEST(pickbox_empty_box_returns_nothing) {
    World w;
    w.createPrimitive(Primitive::Cube, "cube");
    // A box far from everything.
    CHECK_EQ((int)w.pickBox(Vec3{100, 100, 100}, Vec3{110, 110, 110}).size(), 0);
}

TEST(pickbox_respects_world_position_not_local) {
    // The box is in WORLD space. A translated object must be hit by a box at
    // its world position, not by one at the origin.
    World w;
    const ObjectId o = w.createPrimitive(Primitive::Cube, "moved");
    Transform t{}; t.position = {20.0f, 0.0f, 0.0f};
    w.setTransform(o, t);
    CHECK_EQ((int)w.pickBox(Vec3{-2, -2, -2}, Vec3{2, 2, 2}).size(), 0);
    CHECK_EQ((int)w.pickBox(Vec3{18, -2, -2}, Vec3{22, 2, 2}).size(), 1);
}

TEST(pickbox_parented_child_is_hit_at_its_world_position) {
    // A child offset by its parent must be hit where it ENDS UP in world
    // space, not where its local transform says.
    World w;
    const ObjectId parent = w.createPrimitive(Primitive::Cube, "parent");
    const ObjectId child  = w.createPrimitive(Primitive::Cube, "child");
    Transform tp{}; tp.position = {10.0f, 0.0f, 0.0f};
    Transform tc{}; tc.position = {0.0f, 5.0f, 0.0f};
    w.setTransform(parent, tp);
    w.setTransform(child, tc);
    w.setParent(child, parent);
    // Child's LOCAL position is (0,5,0) but its WORLD position is (10,5,0).
    // The box must catch it at the WORLD position, not the local one.
    CHECK_EQ((int)w.pickBox(Vec3{8, 3, -2}, Vec3{12, 7, 2}).size(), 1);
    // And a box at the child's LOCAL position (0,5,0) must MISS it — that is
    // the actual world-vs-local assertion.
    CHECK_EQ((int)w.pickBox(Vec3{-2, 3, -2}, Vec3{2, 7, 2}).size(), 0);
    // The parent sits at (10,0,0), so a box there catches it.
    CHECK_EQ((int)w.pickBox(Vec3{8, -2, -2}, Vec3{12, 2, 2}).size(), 1);
}

// ---- two lights in the packet (item 23's first requirement) -----------------

TEST(two_lights_produce_two_packet_lights_with_right_colour_and_strength) {
    // Item 23: "a scene with two lights produces two packet lights with the
    // right colour and strength". D19 makes lights ordinary objects.
    World w;
    const ObjectId sun  = w.createLight(ember::LightType::Directional, "Sun");
    const ObjectId lamp = w.createLight(ember::LightType::Point, "Lamp");

    ember::Light L1; L1.type = ember::LightType::Directional;
    L1.color = ember::Color{1.0f, 0.9f, 0.8f, 1.0f}; L1.intensity = 3.5f;
    w.setLight(sun, L1);
    ember::Light L2; L2.type = ember::LightType::Point;
    L2.color = ember::Color{0.1f, 0.2f, 0.9f, 1.0f}; L2.intensity = 0.25f;
    L2.radius = 8.0f;
    w.setLight(lamp, L2);

    const ember::RenderPacket pk = w.buildPacket(640, 480);
    CHECK_EQ((int)pk.lights.size(), 2);
    if (pk.lights.size() < 2) return;

    // Creation order is the packet order (Scene.h: "world-resolved, creation
    // order"), so lights[0] is the sun and lights[1] the lamp.
    CHECK(pk.lights[0].type == ember::LightType::Directional);
    CHECK(pk.lights[1].type == ember::LightType::Point);
    CHECK_NEAR(pk.lights[0].color.r, 1.0f,  1e-6);
    CHECK_NEAR(pk.lights[0].color.g, 0.9f,  1e-6);
    CHECK_NEAR(pk.lights[0].color.b, 0.8f,  1e-6);
    CHECK_NEAR(pk.lights[0].intensity, 3.5f, 1e-6);
    CHECK_NEAR(pk.lights[1].color.r, 0.1f,  1e-6);
    CHECK_NEAR(pk.lights[1].color.g, 0.2f,  1e-6);
    CHECK_NEAR(pk.lights[1].color.b, 0.9f,  1e-6);
    CHECK_NEAR(pk.lights[1].intensity, 0.25f, 1e-6);
    CHECK_NEAR(pk.lights[1].radius,    8.0f,  1e-6);
}

TEST(two_lights_resolve_aim_and_position_independently) {
    // Directional resolves to its object's world -Z aim; point to its world
    // position. Assert both land where the Transform puts them.
    World w;
    const ObjectId sun = w.createLight(ember::LightType::Directional, "Sun");
    Transform ts{}; ts.rotation = {-1.5707963f, 0.0f, 0.0f};   // -Z -> down
    w.setTransform(sun, ts);

    const ObjectId lamp = w.createLight(ember::LightType::Point, "Lamp");
    Transform tl{}; tl.position = {4.0f, 7.0f, -2.0f};
    w.setTransform(lamp, tl);

    const ember::RenderPacket pk = w.buildPacket(640, 480);
    CHECK_EQ((int)pk.lights.size(), 2);
    if (pk.lights.size() < 2) return;
    // Sun aims straight down.
    CHECK_NEAR(pk.lights[0].direction.y, -1.0f, 1e-4);
    // Lamp sits at its world position.
    CHECK_NEAR(pk.lights[1].position.x, 4.0f, 1e-4);
    CHECK_NEAR(pk.lights[1].position.y, 7.0f, 1e-4);
    CHECK_NEAR(pk.lights[1].position.z, -2.0f, 1e-4);
}

TEST(lights_list_preserves_creation_order_with_two_directionals) {
    // The deprecated RenderPacket::light field has been REMOVED (Ash's
    // deletion condition: "deleted once render stops reading it" — that pass
    // has landed). The surviving contract is the `lights` LIST, which Scene.h
    // documents as world-resolved in CREATION ORDER. Two directionals: the
    // first-created must come first, so a consumer can still find "the first
    // directional" by taking lights[0].
    World w;
    const ObjectId first  = w.createLight(ember::LightType::Directional, "SunA");
    const ObjectId second = w.createLight(ember::LightType::Directional, "SunB");
    ember::Light A; A.color = ember::Color{1, 0, 0, 1}; A.intensity = 1.0f;
    ember::Light B; B.color = ember::Color{0, 0, 1, 1}; B.intensity = 9.0f;
    w.setLight(first, A);
    w.setLight(second, B);

    const ember::RenderPacket pk = w.buildPacket(640, 480);
    CHECK_EQ((int)pk.lights.size(), 2);
    if (pk.lights.size() < 2) return;
    // Order is creation order: SunA (red, 1.0) then SunB (blue, 9.0).
    CHECK_NEAR(pk.lights[0].color.r, 1.0f, 1e-6);
    CHECK_NEAR(pk.lights[0].color.b, 0.0f, 1e-6);
    CHECK_NEAR(pk.lights[0].intensity, 1.0f, 1e-6);
    CHECK_NEAR(pk.lights[1].color.r, 0.0f, 1e-6);
    CHECK_NEAR(pk.lights[1].color.b, 1.0f, 1e-6);
    CHECK_NEAR(pk.lights[1].intensity, 9.0f, 1e-6);
}

HARNESS_MAIN()
