// tests/test_scene_camera.cpp — owned by Verity · QA. Item 213.
//
// CONTRACT UNDER TEST: "the camera datum round-trips through .ember" — an
// independent test of the scene-camera datum that SceneIO persists today:
//
//   * "a camera object" — an ordinary unmeshed, non-light scene object used as
//     the camera (the shape D47 names). Its identity (id), name, pose and
//     visibility ARE persisted and must survive save()/load() with their fields
//     intact.
//   * "the selected scene camera" — World::activeCamera(), the active-camera
//     POINTER. The canonical v3 writer emits exactly one `active <id|0>` record
//     and load() restores it (src/scene/FORMAT.md §2/§3, rule §7).
//   * "it fails if the camera datum is not persisted" — every positive
//     assertion below has a control that removes or corrupts the record and
//     observes the datum vanish or the load be rejected atomically.
//
// Item 213 covers camera-object and active-camera persistence. The typed
// projection fields are covered separately below: every projection value is
// checked on the loaded Object after a v3 save/load round-trip.
//
// Pure scene-graph data; no GL, no window. Every assertion goes through the
// public save()/load() API and the saved file's text, never World internals.
#include "harness.h"

#include "scene/Scene.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
using ember::scene::load;
using ember::scene::save;
using ember::scene::World;

namespace {

#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                           \
        ++harness::g_checks;                                                       \
        if (!(cond))                                                               \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

std::string tempPath(const char* leaf) {
    const auto dir = std::filesystem::temp_directory_path() / "opencode";
    std::filesystem::create_directories(dir);
    return (dir / leaf).string();
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::uint32_t bits(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

bool sameBits(const Vec3& a, const Vec3& b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z);
}

bool sameBits(const Transform& a, const Transform& b) {
    return sameBits(a.position, b.position) && sameBits(a.rotation, b.rotation) &&
           sameBits(a.scale, b.scale);
}

std::string show(const Vec3& v) {
    char b[160];
    std::snprintf(b, sizeof b, "(%.9g, %.9g, %.9g)", double(v.x), double(v.y), double(v.z));
    return b;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A camera-shaped object: unmeshed and non-light (D47 says a camera object is
// exactly that), with a pose distinctive enough that a bit flip is visible.
ObjectId makeCamera(World& w, const char* name = "Camera") {
    const ObjectId cam = w.createCamera(name);
    Transform t{};
    t.position = Vec3{4.25f, -1.5f, 7.75f};
    t.rotation = Vec3{0.4636476f, 1.10714877f, -0.24497867f};  // radians, non-round
    t.scale    = Vec3{1.0f, 1.0f, 1.0f};                       // camera scale is pose-neutral
    w.setTransform(cam, t);
    return cam;
}

// FORMAT.md §9's object set, so a hand-written file is otherwise valid.
const char* kObjects =
    "object 1 \"Cube\" 0 cube visible 0 0.5 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n"
    "object 2 \"Group\" 1 none visible 1.5 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n"
    "object 3 \"Sun\" 0 none visible 0 0 0 0 -1.5707963 0 1 1 1 0 0 0 1 0 0.5\n"
    "light 3 directional 1 1 1 1 1 1\n";

std::string scene(const std::string& middle, const std::string& tail = "end\n") {
    return std::string("EMBER3D.SCENE v2\nnextid 5\nselection 1 1\n") + middle + kObjects + tail;
}

// A world that is observably non-default, so a rejected load must leave it alone.
struct Sentinel {
    World w;
    ObjectId keep;
    Sentinel() {
        keep = w.createObject("keep_me");
        w.setActiveCamera(ObjectId{0});  // explicit "no camera" sentinel state
    }
    bool untouched(std::string& why) const {
        const ember::scene::Object* o = w.get(keep);
        if (!o || o->name != "keep_me") { why = "the sentinel object is gone/renamed"; return false; }
        if (w.objects().size() != 1) {
            why = "objects changed: now " + std::to_string(w.objects().size());
            return false;
        }
        if (w.activeCamera().valid()) { why = "the active-camera pointer changed"; return false; }
        return true;
    }
};

// Strip the optional/tolerant `active` record from a saved file. If the camera
// datum really is carried by it, load() must then read "no active camera".
std::string withoutActiveRecord(const std::string& text) {
    std::string out;
    for (const std::string& l : lines(text))
        if (!startsWith(l, "active ")) out += l + "\n";
    return out;
}

int countPrefix(const std::string& text, const char* prefix) {
    int n = 0;
    for (const std::string& l : lines(text))
        if (startsWith(l, prefix)) ++n;
    return n;
}

}  // namespace

// ---- the required symbol: item 213's named contract -------------------------

TEST(test_camera_round_trips) {
    World src;
    const ObjectId cam = makeCamera(src, "Camera");
    src.select(cam);
    src.setActiveCamera(cam);
    // A second object proves the camera is not special-cased away on load.
    src.createPrimitive(ember::scene::Primitive::Cube, "cube");

    const Transform poseBefore = src.get(cam)->transform;

    const std::string path = tempPath("camera_rt.ember");
    CHECK_MSG(save(src, path), "save() failed for a world with a camera");
    World dst;
    CHECK_MSG(load(dst, path), "load() of our own camera file failed");

    // "a camera object ... survive[s]": identity, name, shape and pose intact.
    const ember::scene::Object* loaded = dst.get(cam);
    CHECK_MSG(loaded != nullptr, "the camera object (id %u) is missing after load", cam.value);
    if (!loaded) return;
    CHECK_MSG(loaded->name == "Camera", "camera name is '%s', expected 'Camera'",
              loaded->name.c_str());
    CHECK_MSG(!loaded->hasMesh, "a camera object must stay unmeshed");
    CHECK_MSG(!loaded->isLight, "a camera object must not become a light");
    CHECK_MSG(sameBits(loaded->transform, poseBefore),
              "camera pose drifted: position %s vs %s", show(loaded->transform.position).c_str(),
              show(poseBefore.position).c_str());

    // "the selected scene camera": the active-camera pointer names it again.
    CHECK_MSG(dst.activeCamera().valid(), "the active-camera pointer was not restored");
    CHECK_MSG(dst.activeCamera().value == cam.value,
              "active camera is id %u, expected the saved camera id %u",
              dst.activeCamera().value, cam.value);

    // Selection also survives (the camera object was the selected scene object).
    CHECK_MSG(dst.selection().size() == 1 && dst.selection()[0].value == cam.value,
              "the camera object's selection did not survive the round trip");
}

// Typed camera projection fields are persisted by the v3 `camera` record. Keep
// this separate from the active-camera pointer test above: every value must
// survive save/load, and the writer must not silently emit the old v2 grammar.
TEST(typed_camera_projection_fields_round_trip) {
    World src;
    const ObjectId cam = src.createCamera("Camera");
    ember::CameraSettings settings;
    settings.isCamera = true;
    settings.fovY = 1.25f;
    settings.clipNear = 0.125f;
    settings.clipFar = 512.0f;
    settings.orthographic = true;
    settings.orthoScale = 7.5f;
    CHECK_MSG(src.setCameraSettings(cam, settings), "setCameraSettings rejected valid settings");
    src.setActiveCamera(cam);

    const std::string inputPath = tempPath("camera_projection_source.ember");
    const std::string outputPath = tempPath("camera_projection_round_trip.ember");
    CHECK(save(src, inputPath));
    World loaded;
    const bool loadedOk = load(loaded, inputPath);
    CHECK_MSG(loadedOk, "load() rejected the saved v3 camera settings");
    if (!loadedOk) return;

    const ember::scene::Object* camera = loaded.get(cam);
    CHECK_MSG(camera && camera->camera.isCamera, "camera component was not restored");
    if (!camera) return;
    CHECK_MSG(bits(camera->camera.fovY) == bits(settings.fovY), "fovY did not round-trip");
    CHECK_MSG(bits(camera->camera.clipNear) == bits(settings.clipNear), "clipNear did not round-trip");
    CHECK_MSG(bits(camera->camera.clipFar) == bits(settings.clipFar), "clipFar did not round-trip");
    CHECK_MSG(camera->camera.orthographic == settings.orthographic,
              "orthographic did not round-trip");
    CHECK_MSG(bits(camera->camera.orthoScale) == bits(settings.orthoScale),
              "orthoScale did not round-trip");
    CHECK_MSG(loaded.activeCamera() == cam, "active camera did not round-trip with its component");

    // v8 appends editable topology while retaining the camera record.
    CHECK_MSG(save(loaded, outputPath), "save() failed after loading camera projection fields");
    const std::string output = readAll(outputPath);
    CHECK_MSG(startsWith(output, "EMBER3D.SCENE v8\n"),
              "camera projection round-trip did not write v8");
    CHECK_MSG(countPrefix(output, "camera ") == 1,
              "expected exactly one camera projection record after save");
}

TEST(v2_active_pointer_migrates_to_default_camera_component) {
    const std::string path = tempPath("camera_v2_migration.ember");
    writeText(path, scene("active 2\n"));  // object 2 is the legacy camera-shaped empty
    World loaded;
    CHECK_MSG(load(loaded, path), "reader no longer accepts the legacy v2 active-camera pointer");
    const ember::scene::Object* camera = loaded.get(ObjectId{2});
    CHECK_MSG(camera && camera->camera.isCamera,
              "v2 active id did not migrate to a camera component");
    CHECK_MSG(loaded.activeCamera() == ObjectId{2}, "v2 active camera id was not preserved");
    if (camera) {
        const ember::CameraSettings defaults;
        CHECK_MSG(bits(camera->camera.fovY) == bits(defaults.fovY), "v2 migration changed default fovY");
        CHECK_MSG(bits(camera->camera.clipNear) == bits(defaults.clipNear), "v2 migration changed default clipNear");
        CHECK_MSG(bits(camera->camera.clipFar) == bits(defaults.clipFar), "v2 migration changed default clipFar");
        CHECK_MSG(camera->camera.orthographic == defaults.orthographic,
                  "v2 migration changed default orthographic mode");
        CHECK_MSG(bits(camera->camera.orthoScale) == bits(defaults.orthoScale),
                  "v2 migration changed default orthoScale");
    }
}

TEST(v2_cannot_silently_gain_a_camera_record) {
    const std::string path = tempPath("camera_v2_unversioned_record.ember");
    writeText(path, std::string("EMBER3D.SCENE v2\nnextid 2\nselection 0\n") +
                    "active 1\n"
                    "object 1 \"Camera\" 0 none visible 0 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n"
                    "camera 1 1.25 0.125 512 1 7.5\nend\n");
    World loaded;
    const ObjectId keep = loaded.createObject("keep");
    CHECK_MSG(!load(loaded, path), "v2 parser accepted a v3 camera record without a version bump");
    CHECK_MSG(loaded.objects().size() == 1 && loaded.get(keep) && loaded.get(keep)->name == "keep",
              "rejected v2 camera record changed the target world");
}

// ---- controls: the datum is only there because it was persisted -------------

TEST(camera_datum_is_carried_by_the_active_record_not_guessed) {
    World src;
    const ObjectId cam = makeCamera(src, "Camera");
    src.setActiveCamera(cam);
    const std::string path = tempPath("camera_rt2.ember");
    CHECK(save(src, path));

    // The canonical writer emits the pointer exactly once.
    const std::string text = readAll(path);
    CHECK_MSG(countPrefix(text, "active ") == 1,
              "expected exactly one `active` record, found %d", countPrefix(text, "active "));

    // Control: remove that optional record -> load must report NO active camera.
    // If it still reported one, the assertion above would not be testing the file.
    const std::string stripped = tempPath("camera_no_active.ember");
    writeText(stripped, withoutActiveRecord(text));
    World w;
    CHECK_MSG(load(w, stripped), "a v3 file without the optional `active` record must load");
    CHECK_MSG(!w.activeCamera().valid(),
              "with no `active` record the camera datum must be absent, got id %u",
              w.activeCamera().value);
    // ...and the camera OBJECT can still be there; only the pointer is gone.
    CHECK_MSG(w.get(cam) != nullptr, "removing `active` wrongly removed the camera object");
}

TEST(camera_component_is_not_implicitly_active) {
    World src;
    // An explicit camera object is not made active by creation order or shape.
    makeCamera(src, "Camera");
    src.createPrimitive(ember::scene::Primitive::Cube, "cube");
    CHECK_MSG(!src.activeCamera().valid(), "no camera was designated, so activeCamera() must be empty");

    const std::string path = tempPath("camera_none.ember");
    CHECK(save(src, path));
    World dst;
    CHECK(load(dst, path));
    CHECK_MSG(!dst.activeCamera().valid(),
              "load() inferred an active camera (id %u) where none was set",
              dst.activeCamera().value);

    // A fresh world has no camera datum either.
    World fresh;
    CHECK_MSG(!fresh.activeCamera().valid(), "a fresh World must have no active camera");
}

// ---- atomic rejection: a camera pointer that names nothing ------------------

TEST(camera_load_rejects_an_active_id_that_names_no_object_and_changes_nothing) {
    const std::string path = tempPath("camera_bad.ember");
    writeText(path, scene("active 9\n"));  // 9 names no object in the file
    Sentinel s;
    const bool ok = load(s.w, path);
    CHECK_MSG(!ok, "`active 9` (no such object) must be rejected (FORMAT.md §7)");
    std::string why;
    CHECK_MSG(s.untouched(why), "load failed but the target world was modified: %s", why.c_str());
}

TEST(camera_load_rejects_a_dangling_active_id_after_the_sun_migration) {
    // Same rule when the id could only be satisfied by the default-sun append:
    // a missing-object active id must still fail before any migration.
    const std::string path = tempPath("camera_bad2.ember");
    writeText(path, std::string("EMBER3D.SCENE v2\nnextid 3\nselection 0\nactive 7\n") +
                        "object 1 \"Cube\" 0 cube visible 0 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n" +
                        "end\n");
    Sentinel s;
    CHECK_MSG(!load(s.w, path), "a dangling active id must be rejected, not repaired");
    std::string why;
    CHECK_MSG(s.untouched(why), "load failed but the target world was modified: %s", why.c_str());
}

// ---- setter semantics that make the pointer trustworthy ---------------------

// The active-camera pointer accepts only a camera object. Unknown ids are a
// no-op; invalid id clears the explicit active-camera state.
TEST(active_camera_pointer_refuses_unknown_ids_and_clears_on_zero) {
    World w;
    const ObjectId cam = makeCamera(w, "Camera");
    const ObjectId cube = w.createPrimitive(ember::scene::Primitive::Cube, "cube");
    (void)cube;

    w.setActiveCamera(cam);
    CHECK_MSG(w.activeCamera().value == cam.value, "setActiveCamera(existing) did not take");

    w.setActiveCamera(cube);
    CHECK_MSG(w.activeCamera() == cam,
              "setActiveCamera accepted a non-camera object id %u", cube.value);

    // An id that names no object is a silent no-op: the pointer must NOT move
    // to a dangling value, or save() would write a file its own reader rejects.
    w.setActiveCamera(ObjectId{9999});
    CHECK_MSG(w.activeCamera().value == cam.value,
              "setActiveCamera(unknown id) moved the pointer to %u", w.activeCamera().value);

    // invalid ObjectId clears it.
    w.setActiveCamera(ObjectId{0});
    CHECK_MSG(!w.activeCamera().valid(), "setActiveCamera(invalid) must clear the pointer");

    // Setting the pointer is independent of the selection (D47: explicit state,
    // never inferred from selection or creation order).
    w.setActiveCamera(cam);
    CHECK_MSG(w.selection().empty(),
              "designating a camera changed the selection (%zu entries)", w.selection().size());
}

// ---- canonicity: the datum is stable over repeated hops ---------------------

TEST(camera_datum_survives_repeated_save_load_hops) {
    World a;
    const ObjectId cam = makeCamera(a, "Camera");
    a.setActiveCamera(cam);
    a.createPrimitive(ember::scene::Primitive::Cube, "cube");

    const std::string p1 = tempPath("camera_h1.ember");
    const std::string p2 = tempPath("camera_h2.ember");
    const std::string p3 = tempPath("camera_h3.ember");
    CHECK(save(a, p1));
    World b; CHECK(load(b, p1)); CHECK(save(b, p2));
    World c; CHECK(load(c, p2)); CHECK(save(c, p3));

    CHECK_MSG(c.activeCamera().value == cam.value, "camera pointer drifted to %u over hops",
              c.activeCamera().value);
    CHECK_MSG(c.get(cam) != nullptr && sameBits(c.get(cam)->transform, a.get(cam)->transform),
              "camera pose drifted over hops");
    // FORMAT.md §6: from the second hop the file is canonical/stable.
    CHECK_MSG(readAll(p2) == readAll(p3), "second and third hop files differ (camera not canonical)");
}

HARNESS_MAIN()
