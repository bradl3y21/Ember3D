// tests/test_scene_io.cpp — owned by Verity · QA.
//
// Contract under test: ARCHITECTURE.md §4.4 `save`/`load` — the M6 round-trip
// (TEAM.md milestone table names Verity as the round-trip test owner).
//
// Scene I/O behavior is asserted against src/scene/FORMAT.md, including
// versioned material layouts, migrations, canonical writer output and failure
// atomicity. These tests use the CPU-only scene API; no GPU context is needed.
#include "harness.h"

#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using ember::DrawItem;
using ember::MaterialDesc;
using ember::Mat4;
using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
using ember::scene::load;
using ember::scene::Primitive;
using ember::scene::save;
using ember::scene::World;

namespace {

std::string tempPath(const char* leaf) {
    return (std::filesystem::temp_directory_path() / "opencode" / leaf).string();
}

std::vector<DrawItem> packetItems(const World& w) {
    return w.buildPacket(640, 480).items;
}

const DrawItem* findItem(const std::vector<DrawItem>& items, ObjectId id) {
    for (const DrawItem& d : items)
        if (d.object.value == id.value) return &d;
    return nullptr;
}

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

// Builds a world with every feature the format must carry: several objects,
// distinct transforms, a parent/child link, a non-default material, a
// primitive (mesh reference), and a selection.
// Returns the file path used for round-trips.
struct FixtureIds {
    ObjectId parent, child, solo;
};

FixtureIds buildRichWorld(World& w) {
    FixtureIds ids;
    ids.parent = w.createObject("parent");
    Transform tp{}; tp.position = {10.0f, -3.0f, 2.0f};
    tp.rotation = {0.25f, -1.0f, 2.5f}; tp.scale = {2.0f, 0.5f, 1.5f};
    w.setTransform(ids.parent, tp);

    ids.child = w.createObject("child");
    Transform tc{}; tc.position = {0.0f, 4.0f, 0.0f}; tc.scale = {3, 3, 3};
    w.setTransform(ids.child, tc);
    w.setParent(ids.child, ids.parent);

    MaterialDesc gold{{0.9f, 0.7f, 0.1f, 1.0f}, 1.0f, 0.35f};
    w.setMaterial(ids.child, gold);

    ids.solo = w.createPrimitive(Primitive::Cube, "cube");
    MaterialDesc emissive;
    emissive.emissionColor = ember::Color{1.25f, 0.5f, 3.0f, 1.0f};
    emissive.emissionStrength = 2.75f;
    w.setMaterial(ids.solo, emissive);

    w.select(ids.solo);
    w.select(ids.parent, /*additive=*/true);
    return ids;
}

void checkWorldsEquivalent(const World& a, const World& b, const char* ctx) {
    const std::vector<DrawItem> ia = packetItems(a);
    const std::vector<DrawItem> ib = packetItems(b);
    if (ia.size() != ib.size())
        harness::reportFailure(__FILE__, __LINE__,
                      std::string(ctx) + ": item count differs (" +
                      std::to_string(ia.size()) + " vs " + std::to_string(ib.size()) + ")");
    ++harness::g_checks;

    // Match by ordinal (format must preserve order per FORMAT.md when it
    // lands; §4.4 already requires objects() ordering to be stable).
    const std::size_t n = std::min(ia.size(), ib.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (!matNear(ia[i].model, ib[i].model))
        harness::reportFailure(__FILE__, __LINE__,
                          std::string(ctx) + ": model matrix differs at item " +
                          std::to_string(i));
        const MaterialDesc& ma = ia[i].material;
        const MaterialDesc& mb = ib[i].material;
        if (std::fabs(ma.baseColor.r - mb.baseColor.r) > 1e-5f ||
            std::fabs(ma.baseColor.g - mb.baseColor.g) > 1e-5f ||
            std::fabs(ma.baseColor.b - mb.baseColor.b) > 1e-5f ||
            std::fabs(ma.baseColor.a - mb.baseColor.a) > 1e-5f ||
            std::fabs(ma.metallic  - mb.metallic)  > 1e-5f ||
            std::fabs(ma.roughness - mb.roughness) > 1e-5f ||
            std::fabs(ma.emissionColor.r - mb.emissionColor.r) > 1e-5f ||
            std::fabs(ma.emissionColor.g - mb.emissionColor.g) > 1e-5f ||
            std::fabs(ma.emissionColor.b - mb.emissionColor.b) > 1e-5f ||
            std::fabs(ma.emissionStrength - mb.emissionStrength) > 1e-5f ||
            std::fabs(ma.transmission - mb.transmission) > 1e-5f ||
            std::fabs(ma.ior - mb.ior) > 1e-5f ||
            ma.baseColorTexture != mb.baseColorTexture)
        harness::reportFailure(__FILE__, __LINE__,
                          std::string(ctx) + ": material differs at item " +
                          std::to_string(i));
        if (ia[i].mesh == nullptr || ib[i].mesh == nullptr)
        harness::reportFailure(__FILE__, __LINE__,
                          std::string(ctx) + ": null mesh at item " + std::to_string(i));
        else if (ia[i].mesh->positions != ib[i].mesh->positions ||
                 ia[i].mesh->indices   != ib[i].mesh->indices)
        harness::reportFailure(__FILE__, __LINE__,
                          std::string(ctx) + ": mesh data differs at item " +
                          std::to_string(i));
        ++harness::g_checks;
    }

    if (a.selection().size() != b.selection().size())
        harness::reportFailure(__FILE__, __LINE__,
                      std::string(ctx) + ": selection size differs");
    ++harness::g_checks;
}

// Whole-file read, used by the byte-identity assertions below. Declared in the
// FIRST anonymous namespace because the round-trip tests (which follow this
// block) use it — a definition placed after them is not visible to them.
std::vector<char> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

std::filesystem::path resolveAsset(const std::string& base,
                                   const std::string& path) {
    const std::filesystem::path authored(path);
    return (authored.is_absolute() ? authored
                                   : std::filesystem::path(base) / authored)
        .lexically_normal();
}

std::string legacyMaterialFile(int version) {
    std::string text = "EMBER3D.SCENE v" + std::to_string(version) +
                       "\nnextid 2\n";
    if (version == 1) text += "light 0 -1 0 1 1 1 1 1\n";
    text += "selection 0\n";
    if (version >= 6) text += "render cycles 12 6 42\n";
    text += "object 1 \"Legacy\" 0 cube visible 0 0 0 0 0 0 1 1 1 "
            "0.8 0.7 0.6 1 0.2 0.4";
    if (version >= 4) text += " 0.3 0.4 0.5 2.0";
    if (version >= 5) text += " 0.7 1.6";
    text += "\n";
    if (version >= 2) {
        text += "materialslot 1 1 \"Legacy slot\" 0.1 0.2 0.3 1 0.4 0.6";
        if (version >= 4) text += " 0.6 0.7 0.8 1.5";
        if (version >= 5) text += " 0.9 1.8";
        text += "\n";
    }
    text += "end\n";
    return text;
}

std::string v8TriangleEditableFixture() {
    return
        "EMBER3D.SCENE v8\n"
        "nextid 3\nselection 0\nactive 0\nnextcollection 1\n"
        "worldbg 0.05 0.05 0.06 1 1 0\nframe 1 250 1\n"
        "cursor 0 0 0\nrender raster 64 4 1\n"
        "object 1 \"Tri\" 0 cube visible 0 0 0 0 0 0 1 1 1 "
        "0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45\n"
        "object 2 \"Sun\" 0 none visible 0 0 0 0 0 0 1 1 1 "
        "1 1 1 1 0 0.5 0 0 0 0 0 1.45\n"
        "light 2 directional 1 1 1 1 1 1\n"
        "editable 1 3 3 3 1 3 0\n"
        "editposition 0 0 0 0\neditposition 1 1 0 0\n"
        "editposition 2 0 1 0\n"
        "editnormal 0 0 0 1\neditnormal 1 0 0 1\n"
        "editnormal 2 0 0 1\n"
        "editedge 0 0 1\neditedge 1 1 2\neditedge 2 0 2\n"
        "editface 0 0 3 0\n"
        "editloop 0 0 0 1 2 0 0 0\n"
        "editloop 1 1 1 2 0 0 1 0\n"
        "editloop 2 2 2 0 1 0 0 1\n"
        "endedit\nend\n";
}

std::string v8NonManifoldEditableFixture() {
    std::string text = v8TriangleEditableFixture();
    const std::size_t begin = text.find("editable 1 ");
    const std::size_t end = text.find("end\n", begin);
    if (begin == std::string::npos || end == std::string::npos) return {};
    const std::string records =
        "editable 1 5 5 7 3 9 4\n"
        "editposition 0 0 0 0\neditposition 1 1 0 0\n"
        "editposition 2 0 1 0\neditposition 3 0 0 1\n"
        "editposition 4 0 -1 0\n"
        "editnormal 0 0 0 1\neditnormal 1 0 0 1\n"
        "editnormal 2 0 0 1\neditnormal 3 0 0 1\neditnormal 4 0 0 1\n"
        "editedge 0 0 1\neditedge 1 1 2\neditedge 2 0 2\n"
        "editedge 3 1 3\neditedge 4 0 3\neditedge 5 1 4\neditedge 6 0 4\n"
        "editface 0 0 3 0\neditface 1 3 3 0\neditface 2 6 3 0\n"
        "editloop 0 0 0 1 2 0 0 0\neditloop 1 1 1 2 0 0 1 0\n"
        "editloop 2 2 2 0 1 0 0 1\n"
        "editloop 3 0 0 4 5 1 0 0\neditloop 4 1 3 5 3 1 1 0\n"
        "editloop 5 3 4 3 4 1 0 1\n"
        "editloop 6 0 0 7 8 2 0 0\neditloop 7 1 5 8 6 2 1 0\n"
        "editloop 8 4 6 6 7 2 0 -1\nendedit\n";
    text.replace(begin, end - begin, records);
    return text;
}

bool replaceOnce(std::string& text, const std::string& from,
                 const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

} // namespace

// ---- the round-trip itself --------------------------------------------------

TEST(ember_roundtrip_preserves_observable_world) {
    World src;
    buildRichWorld(src);
    const std::string path = tempPath("roundtrip.ember");

    CHECK(save(src, path));

    World dst;
    CHECK(load(dst, path));
    checkWorldsEquivalent(src, dst, "roundtrip");
}

TEST(ember_v6_roundtrips_emission_and_dielectric_values_on_base_and_slots) {
    World src;
    const ObjectId id = src.createPrimitive(Primitive::Cube, "emissive");
    MaterialDesc base;
    base.emissionColor = ember::Color{2.5f, 0.25f, 4.0f, 1.0f};
    base.emissionStrength = 3.5f;
    base.transmission = 0.625f;
    base.ior = 1.333f;
    src.setMaterial(id, base);

    MaterialDesc slot;
    slot.emissionColor = ember::Color{0.125f, 1.75f, 0.5f, 1.0f};
    slot.emissionStrength = 0.75f;
    slot.transmission = 0.875f;
    slot.ior = 1.7f;
    const ember::u32 slotIndex = src.addMaterialSlot(id, slot, "Emissive slot");

    const std::string path = tempPath("emission-roundtrip.ember");
    CHECK(save(src, path));
    World dst;
    CHECK(load(dst, path));
    const ember::scene::Object* loaded = dst.get(id);
    CHECK(loaded != nullptr);
    if (!loaded) return;
    CHECK_NEAR(loaded->material.emissionColor.r, base.emissionColor.r, 1e-6);
    CHECK_NEAR(loaded->material.emissionColor.g, base.emissionColor.g, 1e-6);
    CHECK_NEAR(loaded->material.emissionColor.b, base.emissionColor.b, 1e-6);
    CHECK_NEAR(loaded->material.emissionStrength, base.emissionStrength, 1e-6);
    CHECK_NEAR(loaded->material.transmission, base.transmission, 1e-6);
    CHECK_NEAR(loaded->material.ior, base.ior, 1e-6);
    CHECK(slotIndex < loaded->materials.size());
    if (slotIndex < loaded->materials.size()) {
        const MaterialDesc& loadedSlot = loaded->materials[slotIndex].material;
        CHECK_NEAR(loadedSlot.emissionColor.r, slot.emissionColor.r, 1e-6);
        CHECK_NEAR(loadedSlot.emissionColor.g, slot.emissionColor.g, 1e-6);
        CHECK_NEAR(loadedSlot.emissionColor.b, slot.emissionColor.b, 1e-6);
        CHECK_NEAR(loadedSlot.emissionStrength, slot.emissionStrength, 1e-6);
        CHECK_NEAR(loadedSlot.transmission, slot.transmission, 1e-6);
        CHECK_NEAR(loadedSlot.ior, slot.ior, 1e-6);
    }
}

TEST(ember_v8_roundtrips_scene_render_settings_and_writes_canonical_record) {
    World src;
    src.createLight(ember::LightType::Directional, "Sun");
    ember::SceneRenderSettings settings;
    settings.engine = ember::RenderEngine::Cycles;
    settings.samplesPerPixel = 257;
    settings.maxBounces = 11;
    settings.seed = 0x1234abcdu;
    src.setRenderSettings(settings);

    const std::string first = tempPath("render-settings-v8.ember");
    const std::string second = tempPath("render-settings-v8-resaved.ember");
    CHECK(save(src, first));
    const std::vector<char> contents = readFileBytes(first);
    const std::string text(contents.begin(), contents.end());
    CHECK(text.find("EMBER3D.SCENE v8\n") == 0);
    const std::size_t cursor = text.find("cursor ");
    const std::size_t render = text.find("render cycles 257 11 305441741\n");
    const std::size_t object = text.find("object ");
    CHECK(cursor != std::string::npos && render != std::string::npos &&
          object != std::string::npos && cursor < render && render < object);

    World loaded;
    CHECK(load(loaded, first));
    CHECK(loaded.renderSettings().engine == ember::RenderEngine::Cycles);
    CHECK_EQ((int)loaded.renderSettings().samplesPerPixel, 257);
    CHECK_EQ((int)loaded.renderSettings().maxBounces, 11);
    CHECK_EQ((int)loaded.renderSettings().seed, (int)settings.seed);
    const ember::RenderPacket packet = loaded.buildPacket(32, 32);
    CHECK(packet.renderSettings.engine == ember::RenderEngine::Cycles);
    CHECK_EQ((int)packet.renderSettings.samplesPerPixel, 257);
    CHECK_EQ((int)packet.renderSettings.maxBounces, 11);
    CHECK_EQ((int)packet.renderSettings.seed, (int)settings.seed);

    CHECK(save(loaded, second));
    CHECK(readFileBytes(first) == readFileBytes(second));
}

TEST(ember_v8_roundtrips_extruded_edit_mesh_and_face_material_slots) {
    World source;
    const ObjectId id = source.createPrimitive(Primitive::Cube, "Edited cube");
    source.createLight(ember::LightType::Directional, "Sun");
    MaterialDesc accent;
    accent.baseColor = ember::Color{0.9f, 0.15f, 0.04f, 1.0f};
    const ember::u32 slot = source.addMaterialSlot(id, accent, "Accent");
    ember::scene::EditableMesh* edit = source.editable(id);
    CHECK(edit != nullptr);
    if (!edit) return;
    CHECK(!edit->faces.empty());
    if (edit->faces.empty()) return;
    edit->faces[0].materialIndex = slot;
    CHECK(source.editExtrudeFace(id, 0, Vec3{0.0f, 0.0f, 0.25f}));
    const ember::scene::Object* original = source.get(id);
    CHECK(original != nullptr);
    if (!original) return;
    CHECK(original->hasEdit);
    CHECK(original->edit.validate());
    CHECK(std::any_of(original->edit.faces.begin(), original->edit.faces.end(),
                      [&](const ember::scene::Face& face) {
                          return face.materialIndex == slot;
                      }));

    const std::string first = tempPath("editable-extrusion-v8.ember");
    const std::string second = tempPath("editable-extrusion-v8-resaved.ember");
    CHECK(save(source, first));
    const std::vector<char> firstBytes = readFileBytes(first);
    const std::string serialized(firstBytes.begin(), firstBytes.end());
    CHECK(serialized.find("EMBER3D.SCENE v8\n") == 0);
    CHECK(serialized.find("editable 1 ") != std::string::npos);

    World loaded;
    CHECK(load(loaded, first));
    const ember::scene::Object* restored = loaded.get(id);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->hasEdit);
    CHECK_EQ(restored->edit.positions.size(), original->edit.positions.size());
    CHECK_EQ(restored->edit.normals.size(), original->edit.normals.size());
    CHECK_EQ(restored->edit.edges.size(), original->edit.edges.size());
    CHECK_EQ(restored->edit.faces.size(), original->edit.faces.size());
    CHECK_EQ(restored->edit.loops.size(), original->edit.loops.size());
    CHECK_EQ(restored->edit.topologyVersion, original->edit.topologyVersion);
    for (std::size_t i = 0; i < original->edit.positions.size(); ++i) {
        CHECK_NEAR(restored->edit.positions[i].x, original->edit.positions[i].x, 1e-6);
        CHECK_NEAR(restored->edit.positions[i].y, original->edit.positions[i].y, 1e-6);
        CHECK_NEAR(restored->edit.positions[i].z, original->edit.positions[i].z, 1e-6);
        if (i < original->edit.normals.size()) {
            CHECK_NEAR(restored->edit.normals[i].x, original->edit.normals[i].x, 1e-6);
            CHECK_NEAR(restored->edit.normals[i].y, original->edit.normals[i].y, 1e-6);
            CHECK_NEAR(restored->edit.normals[i].z, original->edit.normals[i].z, 1e-6);
        }
    }
    for (std::size_t i = 0; i < original->edit.edges.size(); ++i) {
        CHECK_EQ(restored->edit.edges[i].v0, original->edit.edges[i].v0);
        CHECK_EQ(restored->edit.edges[i].v1, original->edit.edges[i].v1);
    }
    bool foundAssignedFace = false;
    for (std::size_t i = 0; i < original->edit.faces.size(); ++i) {
        const auto& a = original->edit.faces[i];
        const auto& b = restored->edit.faces[i];
        CHECK_EQ(b.firstLoop, a.firstLoop);
        CHECK_EQ(b.loopCount, a.loopCount);
        CHECK_EQ(b.materialIndex, a.materialIndex);
        foundAssignedFace = foundAssignedFace || b.materialIndex == slot;
    }
    CHECK(foundAssignedFace);
    const ember::RenderPacket packet = loaded.buildPacket(32, 32);
    const DrawItem* packetItem = findItem(packet.items, id);
    CHECK(packetItem != nullptr);
    if (packetItem) {
        CHECK(packetItem->materialCount > slot);
        CHECK_EQ(packetItem->triangleMaterialSlotCount,
                 packetItem->mesh ? packetItem->mesh->indexCount() / 3 : 0u);
        CHECK(packetItem->triangleMaterialSlots != nullptr);
        bool mappedAccent = false;
        if (packetItem->triangleMaterialSlots)
            for (ember::u32 i = 0; i < packetItem->triangleMaterialSlotCount; ++i)
                mappedAccent = mappedAccent || packetItem->triangleMaterialSlots[i] == slot;
        CHECK(mappedAccent);
    }
    for (std::size_t i = 0; i < original->edit.loops.size(); ++i) {
        const auto& a = original->edit.loops[i];
        const auto& b = restored->edit.loops[i];
        CHECK_EQ(b.vertex, a.vertex); CHECK_EQ(b.edge, a.edge);
        CHECK_EQ(b.next, a.next); CHECK_EQ(b.prev, a.prev); CHECK_EQ(b.face, a.face);
        CHECK_NEAR(b.uv.x, a.uv.x, 1e-6); CHECK_NEAR(b.uv.y, a.uv.y, 1e-6);
    }
    CHECK_EQ((int)restored->materials.size(), (int)original->materials.size());
    CHECK(restored->materials[slot].name == "Accent");
    CHECK(restored->mesh.positions == original->mesh.positions);
    CHECK(restored->mesh.normals == original->mesh.normals);
    CHECK(restored->mesh.uvs == original->mesh.uvs);
    CHECK(restored->mesh.indices == original->mesh.indices);
    CHECK(restored->edit.validate());
    CHECK(save(loaded, second));
    CHECK(readFileBytes(first) == readFileBytes(second));
}

TEST(ember_v7_fixture_migrates_without_edit_topology) {
    const std::string path = tempPath("legacy-v7-no-edit.ember");
    {
        std::ofstream file(path, std::ios::binary);
        file << legacyMaterialFile(7);
    }
    World loaded;
    CHECK(load(loaded, path));
    const ember::scene::Object* object = loaded.get(ObjectId{1});
    CHECK(object != nullptr);
    if (!object) return;
    CHECK(object->hasMesh);
    CHECK(!object->hasEdit);
    CHECK_EQ(object->mesh.vertexCount(), ember::u32{24});
    CHECK_EQ((int)object->edit.faces.size(), 0);
    CHECK_EQ((int)object->materials.size(), 2);
    CHECK(loaded.renderSettings().engine == ember::RenderEngine::Cycles);
    CHECK(object->material.baseColorTexture.path.empty());
}

TEST(ember_v8_texture_refs_roundtrip_and_save_as_preserves_asset_identity) {
    const std::filesystem::path root = tempPath("texture-save-as");
    const std::filesystem::path sourceDir = root / "source";
    const std::filesystem::path destinationDir = root / "destination" / "scenes";
    const std::filesystem::path assetDir = sourceDir / "assets";
    std::filesystem::create_directories(assetDir);
    std::filesystem::create_directories(destinationDir);
    const std::filesystem::path baseAsset = assetDir / "color image.ppm";
    const std::filesystem::path slotAsset = assetDir / "height map.pfm";
    const std::filesystem::path absoluteEnvironment = root / "sky.pfm";
    std::ofstream(baseAsset, std::ios::binary) << "not decoded by scene IO";
    std::ofstream(slotAsset, std::ios::binary) << "not decoded by scene IO";

    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::string baseFromCwd =
        baseAsset.lexically_relative(cwd).generic_string();
    const std::string slotFromCwd =
        slotAsset.lexically_relative(cwd).generic_string();
    CHECK(!baseFromCwd.empty());
    CHECK(!slotFromCwd.empty());

    World source;
    const ObjectId id = source.createPrimitive(Primitive::Cube, "Textured");
    MaterialDesc base;
    base.baseColorTexture = {baseFromCwd, ember::ImageColorSpace::SRgb};
    source.setMaterial(id, base);
    MaterialDesc slot;
    slot.baseColorTexture = {slotFromCwd, ember::ImageColorSpace::Linear};
    const ember::u32 slotIndex = source.addMaterialSlot(id, slot, "Height");
    ember::scene::EditableMesh* editable = source.editable(id);
    CHECK(editable != nullptr);
    if (!editable || editable->faces.empty()) return;
    editable->faces[0].materialIndex = slotIndex;
    CHECK(source.editExtrudeFace(id, 0, Vec3{0.0f, 0.0f, 0.2f}));
    ember::WorldBackground background;
    background.environmentTexture = {
        absoluteEnvironment.generic_string(), ember::ImageColorSpace::Linear};
    source.setBackground(background);
    source.createLight(ember::LightType::Directional, "Sun");

    const std::filesystem::path sourceScene = sourceDir / "scene.ember";
    const std::filesystem::path destinationScene = destinationDir / "final.ember";
    CHECK(save(source, sourceScene.string()));
    World loadedSource;
    CHECK(load(loadedSource, sourceScene.string()));
    CHECK(loadedSource.assetBaseDirectory() == sourceDir.lexically_normal().generic_string());
    const ember::scene::Object* sourceObject = loadedSource.get(id);
    CHECK(sourceObject != nullptr);
    if (!sourceObject || slotIndex >= sourceObject->materials.size()) return;
    CHECK(resolveAsset(loadedSource.assetBaseDirectory(),
                       sourceObject->material.baseColorTexture.path) ==
          baseAsset.lexically_normal());
    CHECK(resolveAsset(loadedSource.assetBaseDirectory(),
                       sourceObject->materials[slotIndex].material.baseColorTexture.path) ==
          slotAsset.lexically_normal());
    CHECK(sourceObject->material.baseColorTexture.colorSpace ==
          ember::ImageColorSpace::SRgb);
    CHECK(sourceObject->materials[slotIndex].material.baseColorTexture.colorSpace ==
          ember::ImageColorSpace::Linear);
    CHECK(sourceObject->hasEdit);
    CHECK(std::any_of(sourceObject->edit.faces.begin(), sourceObject->edit.faces.end(),
                      [&](const ember::scene::Face& face) {
                          return face.materialIndex == slotIndex;
                      }));

    const std::string originalRelativePath =
        sourceObject->material.baseColorTexture.path;
    CHECK(save(loadedSource, destinationScene.string()));
    // Saving elsewhere is a pure serialization transform; it does not change
    // the world or its source asset base directory.
    CHECK(loadedSource.assetBaseDirectory() == sourceDir.lexically_normal().generic_string());
    CHECK(loadedSource.get(id)->material.baseColorTexture.path == originalRelativePath);
    const std::vector<char> serializedBytes = readFileBytes(destinationScene.string());
    const std::string serialized(serializedBytes.begin(), serializedBytes.end());
    CHECK(serialized.find("basetexture 1 \"../../source/assets/color image.ppm\" srgb") !=
          std::string::npos);
    CHECK(serialized.find("environmenttexture \"" +
          absoluteEnvironment.lexically_normal().generic_string() + "\" linear") !=
          std::string::npos);

    World loadedDestination;
    CHECK(load(loadedDestination, destinationScene.string()));
    CHECK(loadedDestination.assetBaseDirectory() == destinationDir.lexically_normal().generic_string());
    const ember::scene::Object* destinationObject = loadedDestination.get(id);
    CHECK(destinationObject != nullptr);
    if (destinationObject && slotIndex < destinationObject->materials.size()) {
        CHECK(destinationObject->hasEdit);
        CHECK_EQ(destinationObject->edit.positions.size(), sourceObject->edit.positions.size());
        CHECK_EQ(destinationObject->edit.faces.size(), sourceObject->edit.faces.size());
        CHECK(std::any_of(destinationObject->edit.faces.begin(), destinationObject->edit.faces.end(),
                          [&](const ember::scene::Face& face) {
                              return face.materialIndex == slotIndex;
                          }));
        CHECK(resolveAsset(loadedDestination.assetBaseDirectory(),
                           destinationObject->material.baseColorTexture.path) ==
              baseAsset.lexically_normal());
        CHECK(resolveAsset(loadedDestination.assetBaseDirectory(),
                           destinationObject->materials[slotIndex].material.baseColorTexture.path) ==
              slotAsset.lexically_normal());
        CHECK(destinationObject->material.baseColorTexture.colorSpace ==
              ember::ImageColorSpace::SRgb);
        CHECK(destinationObject->materials[slotIndex].material.baseColorTexture.colorSpace ==
              ember::ImageColorSpace::Linear);
    }
    CHECK(resolveAsset(loadedDestination.assetBaseDirectory(),
                       loadedDestination.background().environmentTexture.path) ==
          absoluteEnvironment.lexically_normal());
    const ember::RenderPacket packet = loadedDestination.buildPacket(32, 32);
    CHECK(packet.assetBaseDirectory == loadedDestination.assetBaseDirectory());
}

TEST(ember_v7_rejects_malformed_texture_records_atomically) {
    const std::string path = tempPath("texture-malformed.ember");
    World target;
    const ObjectId keep = target.createObject("Keep");
    const std::vector<std::string> malformed = {
        "basetexture 1 \"image.ppm\" DisplayP3\n",
        "basetexture 1 \"image.ppm\" srgb\nbasetexture 1 \"other.ppm\" linear\n",
        "slottexture 1 1 \"missing-slot.ppm\" srgb\n",
        "basetexture 1 \"image.ppm\" srgb\nobject 2 \"Late\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5 0 0 0 0 0 1.45\n",
    };
    for (const std::string& textureRecords : malformed) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "EMBER3D.SCENE v7\n"
                "nextid 3\n"
                "selection 0\n"
                "render raster 64 4 1\n"
                "object 1 \"Replace\" 0 cube visible 0 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45\n"
             << textureRecords << "end\n";
        file.close();
        CHECK(!load(target, path));
        CHECK_EQ((int)target.objects().size(), 1);
        CHECK(target.get(keep) != nullptr);
        CHECK(target.get(keep)->name == "Keep");
    }

    std::string nulPath = "EMBER3D.SCENE v7\n"
                          "nextid 3\nselection 0\nrender raster 64 4 1\n"
                          "object 1 \"Replace\" 0 cube visible 0 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45\n"
                          "basetexture 1 \"bad";
    nulPath.push_back('\0');
    nulPath += "path\" srgb\nend\n";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(nulPath.data(), static_cast<std::streamsize>(nulPath.size()));
    }
    CHECK(!load(target, path));
    CHECK_EQ((int)target.objects().size(), 1);
    CHECK(target.get(keep) != nullptr);

    MaterialDesc invalid;
    invalid.baseColorTexture = {
        "image.ppm", static_cast<ember::ImageColorSpace>(255)};
    target.setMaterial(target.createPrimitive(Primitive::Cube), invalid);
    CHECK(!save(target, tempPath("texture-invalid-colorspace-save.ember")));
}

TEST(ember_v8_rejects_invalid_edit_topology_atomically) {
    const std::string path = tempPath("editable-invalid-v8.ember");
    World target;
    const ObjectId keep = target.createObject("Keep topology target");
    const std::vector<std::pair<std::string, std::string>> corruptions = {
        {"editloop 2 2 2 0 1 0 0 1", "editloop 2 9 2 0 1 0 0 1"},
        {"editedge 2 0 2", "editedge 2 0 1"},
        {"editface 0 0 3 0", "editface 0 1 3 0"},
        {"editface 0 0 3 0", "editface 0 0 3 7"},
        {"editable 1 3 3 3 1 3 0", "editable 1 1000001 3 3 1 3 0"},
        {"editloop 1 1 1 2 0 0 1 0", "editloop 1 1 1 0 2 0 1 0"},
    };
    for (const auto& corruption : corruptions) {
        std::string contents = v8TriangleEditableFixture();
        CHECK(replaceOnce(contents, corruption.first, corruption.second));
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << contents;
        }
        CHECK(!load(target, path));
        CHECK_EQ((int)target.objects().size(), 1);
        CHECK(target.get(keep) != nullptr);
        CHECK(target.get(keep)->name == "Keep topology target");
    }

    const std::string nonManifold = v8NonManifoldEditableFixture();
    CHECK(!nonManifold.empty());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << nonManifold;
    }
    CHECK(!load(target, path)); // the shared edge has three incident faces
    CHECK_EQ((int)target.objects().size(), 1);
    CHECK(target.get(keep) != nullptr);

    World invalidForSave;
    const ObjectId edited = invalidForSave.createPrimitive(Primitive::Cube, "Bad edit");
    ember::scene::EditableMesh* edit = invalidForSave.editable(edited);
    CHECK(edit != nullptr);
    if (!edit || edit->loops.empty()) return;
    edit->loops[0].edge = 999999;
    const std::string preservedPath = tempPath("editable-save-preserves-destination.ember");
    {
        std::ofstream file(preservedPath, std::ios::binary | std::ios::trunc);
        file << "keep existing scene bytes";
    }
    CHECK(!save(invalidForSave, preservedPath));
    const std::vector<char> preserved = readFileBytes(preservedPath);
    CHECK(std::string(preserved.begin(), preserved.end()) == "keep existing scene bytes");
}

TEST(ember_v6_render_settings_are_sanitized_on_load) {
    const std::string path = tempPath("render-settings-sanitize.ember");
    {
        std::ofstream file(path, std::ios::binary);
        file << "EMBER3D.SCENE v6\n"
                "nextid 2\n"
                "selection 0\n"
                "cursor 0 0 0\n"
                "render cycles 0 99 37\n"
                "object 1 \"Settings\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5 0 0 0 0 0 1.45\n"
                "end\n";
    }
    World loaded;
    CHECK(load(loaded, path));
    CHECK(loaded.renderSettings().engine == ember::RenderEngine::Cycles);
    CHECK_EQ((int)loaded.renderSettings().samplesPerPixel,
             (int)ember::kMinRenderSamplesPerPixel);
    CHECK_EQ((int)loaded.renderSettings().maxBounces,
             (int)ember::kMaxRenderBounces);
    CHECK_EQ((int)loaded.renderSettings().seed, 37);
}

TEST(ember_v6_invalid_render_engine_or_missing_settings_fails_atomically) {
    const std::string path = tempPath("render-settings-invalid.ember");
    World target;
    const ObjectId existing = target.createObject("Keep me");
    const auto write = [&](const std::string& renderRecord) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "EMBER3D.SCENE v6\n"
                "nextid 2\n"
                "selection 0\n"
                "cursor 0 0 0\n"
             << renderRecord
             << "object 1 \"Replacement\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5 0 0 0 0 0 1.45\n"
                "end\n";
    };
    write("render unknown 64 4 1\n");
    CHECK(!load(target, path));
    CHECK_EQ((int)target.objects().size(), 1);
    CHECK(target.get(existing) != nullptr);
    CHECK(target.get(existing)->name == "Keep me");
    write("");
    CHECK(!load(target, path));
    CHECK_EQ((int)target.objects().size(), 1);
    CHECK(target.get(existing) != nullptr);
}

TEST(ember_v1_through_v7_migrate_render_settings_and_legacy_material_defaults) {
    for (int version = 1; version <= 7; ++version) {
        const std::string path = tempPath(
            ("legacy-emission-v" + std::to_string(version) + ".ember").c_str());
        {
            std::ofstream file(path, std::ios::binary);
            file << legacyMaterialFile(version);
        }
        World loaded;
        CHECK(load(loaded, path));
        CHECK(loaded.renderSettings().engine ==
              (version >= 6 ? ember::RenderEngine::Cycles : ember::RenderEngine::Raster));
        CHECK_EQ((int)loaded.renderSettings().samplesPerPixel,
                  version >= 6 ? 12 : 64);
        CHECK_EQ((int)loaded.renderSettings().maxBounces,
                  version >= 6 ? 6 : 4);
        CHECK_EQ((int)loaded.renderSettings().seed, version >= 6 ? 42 : 1);
        const ember::scene::Object* object = loaded.get(ObjectId{1});
        CHECK(object != nullptr);
        if (!object) continue;
        CHECK(object->material.baseColorTexture.path.empty());
        CHECK(object->material.baseColorTexture.colorSpace ==
              ember::ImageColorSpace::SRgb);
        CHECK_NEAR(object->material.transmission,
                   version >= 5 ? 0.7f : 0.0f, 1e-6);
        CHECK_NEAR(object->material.ior,
                   version >= 5 ? 1.6f : 1.45f, 1e-6);
        if (version <= 3) {
            CHECK_NEAR(object->material.emissionColor.r, 0.0f, 1e-6);
            CHECK_NEAR(object->material.emissionColor.g, 0.0f, 1e-6);
            CHECK_NEAR(object->material.emissionColor.b, 0.0f, 1e-6);
            CHECK_NEAR(object->material.emissionStrength, 0.0f, 1e-6);
        } else {
            CHECK_NEAR(object->material.emissionColor.r, 0.3f, 1e-6);
            CHECK_NEAR(object->material.emissionColor.g, 0.4f, 1e-6);
            CHECK_NEAR(object->material.emissionColor.b, 0.5f, 1e-6);
            CHECK_NEAR(object->material.emissionStrength, 2.0f, 1e-6);
        }
        if (version >= 2) {
            CHECK_EQ((int)object->materials.size(), 2);
            if (object->materials.size() > 1) {
                const MaterialDesc& slot = object->materials[1].material;
                CHECK_NEAR(slot.transmission,
                           version >= 5 ? 0.9f : 0.0f, 1e-6);
                CHECK_NEAR(slot.ior,
                           version >= 5 ? 1.8f : 1.45f, 1e-6);
                if (version <= 3) {
                    CHECK_NEAR(slot.emissionColor.r, 0.0f, 1e-6);
                    CHECK_NEAR(slot.emissionColor.g, 0.0f, 1e-6);
                    CHECK_NEAR(slot.emissionColor.b, 0.0f, 1e-6);
                    CHECK_NEAR(slot.emissionStrength, 0.0f, 1e-6);
                } else {
                    CHECK_NEAR(slot.emissionColor.r, 0.6f, 1e-6);
                    CHECK_NEAR(slot.emissionColor.g, 0.7f, 1e-6);
                    CHECK_NEAR(slot.emissionColor.b, 0.8f, 1e-6);
                    CHECK_NEAR(slot.emissionStrength, 1.5f, 1e-6);
                }
            }
        }
    }
}

TEST(ember_malformed_material_values_are_sanitized_and_hdr_emission_is_preserved) {
    World src;
    const ObjectId id = src.createPrimitive(Primitive::Cube, "sanitized");
    MaterialDesc material;
    material.emissionColor = ember::Color{-1.0f,
        std::numeric_limits<float>::infinity(), 4.5f, 0.0f};
    material.emissionStrength = std::numeric_limits<float>::quiet_NaN();
    material.transmission = std::numeric_limits<float>::infinity();
    material.ior = std::numeric_limits<float>::quiet_NaN();
    material.baseColor.r = std::numeric_limits<float>::quiet_NaN();
    material.metallic = 1.5f;
    material.roughness = -0.5f;
    src.setMaterial(id, material);
    MaterialDesc badSlot;
    badSlot.emissionColor = ember::Color{5.25f, 0.0f, 0.0f, 1.0f};
    badSlot.emissionStrength = 2.0f;
    badSlot.transmission = 1.75f;
    badSlot.ior = 0.5f;
    const ember::u32 slotIndex = src.addMaterialSlot(id, badSlot, "Malformed");
    const std::string path = tempPath("emission-sanitized.ember");
    CHECK(save(src, path));
    World dst;
    CHECK(load(dst, path));
    const ember::scene::Object* loaded = dst.get(id);
    CHECK(loaded != nullptr);
    if (!loaded) return;
    CHECK_NEAR(loaded->material.emissionColor.r, 0.0f, 1e-6);
    CHECK_NEAR(loaded->material.emissionColor.g, 0.0f, 1e-6);
    CHECK_NEAR(loaded->material.emissionColor.b, 4.5f, 1e-6);
    CHECK_NEAR(loaded->material.emissionColor.a, 1.0f, 1e-6);
    CHECK_NEAR(loaded->material.emissionStrength, 0.0f, 1e-6);
    CHECK_NEAR(loaded->material.transmission, 0.0f, 1e-6);
    CHECK_NEAR(loaded->material.ior, 1.45f, 1e-6);
    CHECK_NEAR(loaded->material.baseColor.r, 0.8f, 1e-6);
    CHECK_NEAR(loaded->material.metallic, 1.0f, 1e-6);
    CHECK_NEAR(loaded->material.roughness, 0.0f, 1e-6);
    CHECK(slotIndex < loaded->materials.size());
    if (slotIndex < loaded->materials.size()) {
        const MaterialDesc& slot = loaded->materials[slotIndex].material;
        CHECK_NEAR(slot.emissionColor.r, 5.25f, 1e-6);
        CHECK_NEAR(slot.emissionStrength, 2.0f, 1e-6);
        CHECK_NEAR(slot.transmission, 1.0f, 1e-6);
        CHECK_NEAR(slot.ior, 1.45f, 1e-6);
    }
}

TEST(ember_roundtrip_plateaus_by_the_second_hop_per_FORMAT_md_6) {
    // FORMAT.md §6 (D19): load() appends a default sun to a lightless world,
    // so save(load(T)) differs from T by exactly that sun — ONE hop is not
    // byte-stable for a lightless input. The documented plateau is the SECOND
    // hop: save(load(save(load(T)))) == save(load(T)) byte-for-byte.
    World src;
    buildRichWorld(src);                       // no lights -> sun will be added
    const std::string p1 = tempPath("idem1.ember");
    const std::string p2 = tempPath("idem2.ember");
    const std::string p3 = tempPath("idem3.ember");

    CHECK(save(src, p1));
    World w1; CHECK(load(w1, p1));             // hop 1: sun appended
    CHECK_EQ((int)w1.objects().size(), (int)src.objects().size() + 1);
    CHECK(save(w1, p2));                       // canonical from here on
    World w2; CHECK(load(w2, p2));
    CHECK(save(w2, p3));                       // hop 2: must not grow again

    // §6: the canonical form is stable from the second hop.
    CHECK(std::filesystem::file_size(p2) == std::filesystem::file_size(p3));
    CHECK(readFileBytes(p2) == readFileBytes(p3));
}

TEST(ember_roundtrip_double_stable) {
    // save -> load -> save -> load must land on the same observable state.
    World src;
    buildRichWorld(src);
    const std::string p1 = tempPath("dbl1.ember");
    const std::string p2 = tempPath("dbl2.ember");

    CHECK(save(src, p1));
    World w1; CHECK(load(w1, p1));
    CHECK(save(w1, p2));
    World w2; CHECK(load(w2, p2));
    checkWorldsEquivalent(w1, w2, "double-roundtrip");
}

// ---- FORMAT.md §6: byte-identical writer ------------------------------------

namespace {

} // namespace

TEST(ember_writer_is_canonical_per_FORMAT_md_6) {
    // FORMAT.md §6: the writer is canonical — save(load(T)) == T byte-for-byte.
    // NOTE (D19): for a LIGHTLESS input this holds only from the second hop,
    // because load() appends a default sun on the first (§6's plateau, covered
    // by ember_roundtrip_plateaus_by_the_second_hop). To assert the one-hop
    // canonical property directly, seed a world that ALREADY has a light, so
    // no sun is appended and byte identity must hold immediately.
    World src;
    buildRichWorld(src);
    src.createLight(ember::LightType::Directional, "Sun");   // no default sun needed

    const std::string p1 = tempPath("canon1.ember");
    const std::string p2 = tempPath("canon2.ember");

    CHECK(save(src, p1));
    World mid; CHECK(load(mid, p1));
    CHECK(save(mid, p2));

    const std::vector<char> a = readFileBytes(p1);
    const std::vector<char> b = readFileBytes(p2);
    CHECK(!a.empty());
    if (a.size() != b.size())
        harness::reportFailure(__FILE__, __LINE__,
                      "canonical re-save changed file size: " +
                      std::to_string(a.size()) + " -> " + std::to_string(b.size()));
    else {
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) {
                harness::reportFailure(__FILE__, __LINE__,
                              "canonical re-save differs at byte " + std::to_string(i));
                break;
            }
        ++harness::g_checks;
    }
}

TEST(ember_header_matches_FORMAT_md_example) {
    // FORMAT.md §2/§3: save() always writes the canonical current version,
    // which is v8 (v1-v7 are reader-supported, migrated on load per §5).
    World src;
    const std::string path = tempPath("header.ember");
    CHECK(save(src, path));
    std::ifstream f(path);
    std::string header;
    std::getline(f, header);
    if (header != "EMBER3D.SCENE v8")
        harness::reportFailure(__FILE__, __LINE__,
                      "header is '" + header + "', FORMAT.md §3 requires "
                      "'EMBER3D.SCENE v8' (save always writes v8)");
    ++harness::g_checks;
    const std::vector<char> contents = readFileBytes(path);
    const std::string text(contents.begin(), contents.end());
    CHECK(text.find("render raster 64 4 1\n") != std::string::npos);
}

TEST(ember_example_from_FORMAT_md_parses) {
    // FORMAT.md §9 example, copied verbatim from the CURRENT document (v8).
    // Per the
    // document's own note, tests are written against it — if this fails while
    // the implementation passes its own tests, one of them is wrong.
    const std::string path = tempPath("example.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v8\n"
              "nextid 5\n"
              "selection 1 1\n"
              "cursor 1.25 -2.5 0.75\n"
              "render raster 64 4 1\n"
              "object 1 \"Cube\" 0 cube visible 0 0.5 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45\n"
              "object 2 \"Group\" 1 none visible 1.5 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5 0 0 0 0 0 1.45\n"
              "object 3 \"Sun\" 0 none visible 0 0 0 0 -1.5707963 0 1 1 1 0 0 0 1 0 0.5 0 0 0 0 0 1.45\n"
             "light 3 directional 1 1 1 1 1 1\n"
             "end\n";
    }
    World w;
    if (!load(w, path))
        harness::reportFailure(__FILE__, __LINE__,
                      "FORMAT.md §9's own example failed to load");
    else {
        CHECK_EQ((int)w.objects().size(), 3);
        CHECK_EQ((int)w.selection().size(), 1);
        ObjectId id1{1}, id2{2}, id3{3};
        CHECK(w.get(id1) != nullptr);
        CHECK(w.get(id2) != nullptr);
        CHECK(w.get(id3) != nullptr);
        if (w.get(id2)) CHECK(w.get(id2)->parent.value == id1.value);
        // §9: object 1 meshed cube, object 2 unmeshed group, object 3 is the
        // light object (unmeshed, isLight).
        if (w.get(id1)) CHECK(w.get(id1)->hasMesh);
        if (w.get(id2)) CHECK(!w.get(id2)->hasMesh);
        if (w.get(id3)) {
            CHECK(!w.get(id3)->hasMesh);
            CHECK(w.get(id3)->isLight);            // D19: the light record marks it
            CHECK(w.get(id3)->light.type == ember::LightType::Directional);
            CHECK_NEAR(w.get(id3)->light.intensity, 1.0f, 1e-6);
        }
    }
    ++harness::g_checks;
}

TEST(ember_load_replaces_stale_world_per_FORMAT_md_5) {
    // FORMAT.md §5: "On success, the target world is replaced wholesale."
    World dst;
    dst.createObject("stale1");
    dst.createObject("stale2");
    dst.select(ObjectId{1});

    const std::string path = tempPath("replace.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"Fresh\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    CHECK(load(dst, path));
    // FORMAT.md §5/§6: the target is replaced wholesale — the two stale
    // objects and the stale selection are gone. The one v1 world-light record
    // migrates to a "Sun" object (§5), so 1 file object + 1 migrated sun = 2.
    CHECK_EQ((int)dst.objects().size(), 2);   // stale objects are GONE
    CHECK_EQ((int)dst.selection().size(), 0); // stale selection is GONE
    // And the stale objects really are absent, not just outnumbered.
    bool staleAbsent = true;
    for (const auto& o : dst.objects())
        if (o.name == "stale1" || o.name == "stale2") staleAbsent = false;
    CHECK(staleAbsent);
}

TEST(ember_visibility_survives_roundtrip) {
    // FORMAT.md §3/§6: `visible`/`hidden` is stored and restored per object,
    // and hidden objects emit no DrawItem (§3). Checks both halves.
    World src;
    ObjectId vis = src.createPrimitive(Primitive::Cube, "viscube");
    ObjectId hid = src.createPrimitive(Primitive::Sphere, "hidsphere");
    if (ember::scene::Object* o = src.get(hid)) o->visible = false;

    const std::string path = tempPath("visible.ember");
    CHECK(save(src, path));

    // §3: a hidden object must not appear in the packet.
    const std::vector<DrawItem> items = packetItems(src);
    CHECK(findItem(items, hid) == nullptr);
    CHECK(findItem(items, vis) != nullptr);

    World dst;
    CHECK(load(dst, path));
    const ember::scene::Object* oh = dst.get(hid);
    const ember::scene::Object* ov = dst.get(vis);
    CHECK(oh != nullptr);
    CHECK(ov != nullptr);
    if (oh) CHECK(!oh->visible);
    if (ov) CHECK(ov->visible);
}

TEST(ember_light_survives_roundtrip) {
    // Item 23: light fields survive a save/load round trip. D19 made lights
    // scene objects, so the authored component (type/colour/intensity/radius)
    // is set via setLight(id, Light) and read back off the loaded object.
    // FORMAT.md §3: aim/position are NOT in the record — they live in the
    // object's Transform, so those are asserted through the packet below.
    World src;
    const ObjectId sun = src.createLight(ember::LightType::Directional, "SunA");
    ember::Light L;
    L.type      = ember::LightType::Directional;
    L.color     = ember::Color{0.2f, 0.4f, 0.9f, 1.0f};
    L.intensity = 2.75f;
    L.radius    = 7.5f;                 // stored for all kinds (§3)
    src.setLight(sun, L);
    // Aim the sun: rotation that resolves local -Z to a known world direction.
    Transform t{};
    t.rotation = {-1.5707963f, 0.0f, 0.0f};   // -Z -> straight down (0,-1,0)
    src.setTransform(sun, t);

    const std::string path = tempPath("light.ember");
    CHECK(save(src, path));
    World dst;
    CHECK(load(dst, path));

    // Find the loaded light object (ids are preserved by the format).
    const ember::scene::Object* o = dst.get(sun);
    CHECK(o != nullptr);
    if (!o) return;
    CHECK(o->isLight);
    CHECK(o->light.type == ember::LightType::Directional);
    CHECK_NEAR(o->light.color.r, L.color.r, 1e-6);
    CHECK_NEAR(o->light.color.g, L.color.g, 1e-6);
    CHECK_NEAR(o->light.color.b, L.color.b, 1e-6);
    CHECK_NEAR(o->light.color.a, L.color.a, 1e-6);
    CHECK_NEAR(o->light.intensity, L.intensity, 1e-6);
    CHECK_NEAR(o->light.radius,    L.radius,    1e-6);
    // The aim survives too (it is the Transform, §3).
    CHECK_NEAR(o->transform.rotation.x, t.rotation.x, 1e-6);

    // And it resolves into the packet with the same colour and strength.
    const ember::RenderPacket pk = dst.buildPacket(640, 480);
    CHECK_EQ((int)pk.lights.size(), 1);
    if (!pk.lights.empty()) {
        CHECK_NEAR(pk.lights[0].color.r, L.color.r, 1e-6);
        CHECK_NEAR(pk.lights[0].intensity, L.intensity, 1e-6);
        CHECK_NEAR(pk.lights[0].direction.y, -1.0f, 1e-4);   // resolved aim
    }
}

TEST(ember_name_escaping_roundtrips_per_FORMAT_md_4) {
    // FORMAT.md §4: only backslash and double-quote are escaped; any other
    // byte (newline, tab) is stored verbatim.
    World src;
    ObjectId a = src.createObject("quote\"and\\slash");
    ObjectId b = src.createObject("tab\there");
    ObjectId c = src.createObject("multi\nline");
    CHECK(a.valid() && b.valid() && c.valid());

    const std::string p1 = tempPath("esc1.ember");
    const std::string p2 = tempPath("esc2.ember");
    CHECK(save(src, p1));
    World mid; CHECK(load(mid, p1));
    CHECK(save(mid, p2));

    // Canonical-writer byte identity, from the second hop: the first load
    // appends the §6 default sun, so p1 -> mid grows once; from p2 onward the
    // form is stable (same plateau as ember_roundtrip_plateaus_by_the_second_hop).
    World mid2; CHECK(load(mid2, p2));
    const std::string p3 = tempPath("esc3.ember");
    CHECK(save(mid2, p3));
    CHECK(readFileBytes(p2) == readFileBytes(p3));

    // The three ORIGINAL names must survive unescaped and equal. (mid also
    // holds the appended Sun, which has no counterpart in src — skip it.)
    bool allNamesEqual = true;
    for (const auto& o : mid.objects()) {
        const ember::scene::Object* orig = src.get(o.id);
        if (!orig) continue;                      // the default sun, not ours
        if (orig->name != o.name) allNamesEqual = false;
    }
    CHECK(allNamesEqual);
}

TEST(ember_bad_escape_sequence_is_parse_error_per_FORMAT_md_4) {
    // §4: "an unrecognized escape (backslash followed by anything else) is a
    // parse error -> load returns false."
    const std::string path = tempPath("badescape.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"bad\\name\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_nonfinite_float_fails_per_FORMAT_md_5_9) {
    // §5.9: any float that is inf/nan fails load. "nan" is not a legal
    // decimal FLOAT, so it must be rejected as a field-kind error too.
    const std::string path = tempPath("nonfinite.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"NaNy\" 0 none visible nan 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_missing_light_record_fails_per_FORMAT_md_5_4) {
    // §2/§5.4: record order is fixed and mandatory; a file from the PRE-light
    // grammar (no light record) must now fail rather than be guessed at.
    const std::string path = tempPath("nolight.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "selection 0\n"
             "object 1 \"Old\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_wrong_version_fails_per_FORMAT_md_3) {
    // §3: v1-v8 are supported (v8 is canonical); v9 is unsupported.
    // Any OTHER version must fail, and readers must not guess at it.
    const std::string path = tempPath("unsupported-v9.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v9\n"
             "nextid 2\n"
             "selection 0\n"
              "object 1 \"V4\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_v2_with_world_light_record_fails_per_FORMAT_md_2) {
    // §2: "a v2 file must not contain the world-level `light` record" — that
    // record is v1-only. A v2 file with one is a parse error.
    const std::string path = tempPath("v2worldlight.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v2\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"V2\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_parent_cycle_fails_per_FORMAT_md_5_7) {
    // §5.7: a parenting cycle must be detected and fail load.
    const std::string path = tempPath("cycle.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 4\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"A\" 2 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "object 2 \"B\" 1 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_nextid_not_greater_than_max_id_fails_per_FORMAT_md_5_6) {
    // §5.6: nextid must be strictly greater than every object id.
    const std::string path = tempPath("badnextid.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 3\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 3 \"High\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_selection_id_absent_fails_per_FORMAT_md_5_8) {
    // §5.8: a selection id naming an absent object fails load.
    const std::string path = tempPath("badselection.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 1 99\n"
             "object 1 \"Solo\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_trailing_content_after_end_fails_per_FORMAT_md_5_10) {
    // §5.10: anything after `end` makes load fail.
    const std::string path = tempPath("trailing.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "EMBER3D.SCENE v1\n"
             "nextid 2\n"
             "light 0 -1 0 1 1 1 1 1\n"
             "selection 0\n"
             "object 1 \"Solo\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n"
             "end\n"
             "object 2 \"Ghost\" 0 none visible 0 0 0 0 0 0 1 1 1 1 1 1 1 0 0.5\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_roundtrip_empty_world) {
    // FORMAT.md §6: even an empty world gains the default sun on load, so the
    // loaded world has exactly one object — an unmeshed light, which draws
    // nothing (0 DrawItems is still correct for it).
    World src;
    const std::string path = tempPath("empty.ember");
    CHECK(save(src, path));
    World dst;
    CHECK(load(dst, path));
    CHECK_EQ((int)dst.objects().size(), 1);          // the §6 default sun
    CHECK(dst.objects().front().isLight);            // ... and it IS a light
    CHECK_EQ((int)packetItems(dst).size(), 0);       // unmeshed: draws nothing
}

TEST(ember_roundtrip_preserves_hierarchy_observable_state) {
    // §4.4 does not define the layout of `Object`, so nothing about Object
    // fields is asserted here. The hierarchy IS verified: the child's world
    // model matrix (which encodes the parent link) survives the round-trip,
    // checked item-by-item in checkWorldsEquivalent.
    World src;
    FixtureIds ids = buildRichWorld(src);
    const std::string path = tempPath("order.ember");
    CHECK(save(src, path));
    World dst; CHECK(load(dst, path));
    checkWorldsEquivalent(src, dst, "hierarchy");

    // Object count: src objects all survive, PLUS the §6 default sun (the
    // fixture has no lights). The sun is unmeshed so it emits no DrawItem,
    // which is why checkWorldsEquivalent above still matches item-for-item.
    CHECK_EQ((int)dst.objects().size(), (int)src.objects().size() + 1);
    (void)ids;
}

// ---- failure paths (must return false, not crash) ---------------------------

TEST(ember_save_to_impossible_path_returns_false) {
    World w;
    buildRichWorld(w);
    CHECK(!save(w, "Q:/definitely/not/a/real/dir/out.ember"));
}

TEST(ember_load_missing_file_returns_false) {
    World w;
    CHECK(!load(w, tempPath("does_not_exist.ember")));
    CHECK_EQ((int)w.objects().size(), 0);  // failed load must not half-populate
}

TEST(ember_load_garbage_file_returns_false) {
    const std::string path = tempPath("garbage.ember");
    {
        std::ofstream f(path, std::ios::binary);
        f << "this is not an ember scene, it is a love letter\n";
    }
    World w;
    CHECK(!load(w, path));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_load_truncated_file_returns_false) {
    World src; buildRichWorld(src);
    const std::string full = tempPath("full.ember");
    const std::string cut  = tempPath("truncated.ember");
    CHECK(save(src, full));

    const auto sz = std::filesystem::file_size(full);
    if (sz < 4) {  // pathologically small file: nothing to truncate
        CHECK(true);
        return;
    }
    std::filesystem::copy_file(full, cut, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(cut, sz / 2);

    World w;
    CHECK(!load(w, cut));
    CHECK_EQ((int)w.objects().size(), 0);
}

TEST(ember_load_empty_file_returns_false) {
    const std::string path = tempPath("empty.ember.zero");
    std::ofstream(path, std::ios::binary).close();
    World w;
    CHECK(!load(w, path));
}

HARNESS_MAIN()
