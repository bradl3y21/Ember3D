// tests/test_scene_cursor.cpp - owned by Verity / QA. Item 157 (D22 3D cursor).
//
// Contract under test (src/scene/FORMAT.md, "cursor x y z" and §7):
//   * World::cursor() / setCursor() is a position-only scene datum, default (0,0,0).
//   * The canonical v3 writer emits EXACTLY ONE `cursor` record, after `frame`
//     (if any) and before every `object` record, with the 9-significant-digit
//     float rule.
//   * save() then load() reproduces the cursor BIT-EXACTLY.
//   * An older v2 file with NO cursor record loads with the cursor at the origin
//     (and that replaces whatever the target world had: "replaced wholesale").
//   * Failure rules 3, 4, 10, 11: wrong field count, out-of-order record,
//     non-finite coordinate, duplicate record -> load() returns false and the
//     target world is COMPLETELY unchanged.
//   * The cursor is scene data only: setting it moves no object.
//
// Pure scene-graph data; no GL. Every assertion goes through the public
// save/load API and the saved file's text, not through World's internals.
#include "harness.h"

#include "scene/Scene.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using ember::ObjectId;
using ember::Vec3;
using ember::scene::load;
using ember::scene::Primitive;
using ember::scene::save;
using ember::scene::World;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

std::string tempPath(const char* leaf) {
    const auto dir = std::filesystem::temp_directory_path() / "opencode";
    std::filesystem::create_directories(dir);
    return (dir / leaf).string();
}

std::uint32_t bits(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

bool sameBits(const Vec3& a, const Vec3& b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z);
}

std::string show(const Vec3& v) {
    char b[160];
    std::snprintf(b, sizeof b, "(%.9g, %.9g, %.9g) bits=%08x %08x %08x", double(v.x), double(v.y),
                  double(v.z), unsigned(bits(v.x)), unsigned(bits(v.y)), unsigned(bits(v.z)));
    return b;
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

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The cube/group/sun objects of FORMAT.md §9, so every fixture is otherwise valid.
const char* kObjects =
    "object 1 \"Cube\" 0 cube visible 0 0.5 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n"
    "object 2 \"Group\" 1 none visible 1.5 0 0 0 0 0 1 1 1 0.8 0.8 0.8 1 0 0.5\n"
    "object 3 \"Sun\" 0 none visible 0 0 0 0 -1.5707963 0 1 1 1 0 0 0 1 0 0.5\n"
    "light 3 directional 1 1 1 1 1 1\n";

// header lines come first, then `middle` (where the cursor record goes), then objects.
std::string scene(const std::string& middle, const std::string& tail = "end\n") {
    return std::string("EMBER3D.SCENE v2\nnextid 5\nselection 1 1\n") + middle + kObjects + tail;
}

// A world that is observably non-default, to prove a failed load leaves it alone.
struct Sentinel {
    World w;
    ObjectId keep;
    Sentinel() {
        keep = w.createObject("keep_me");
        w.setCursor(Vec3{7.0f, 8.0f, 9.0f});
    }
    bool untouched(std::string& why) const {
        if (!sameBits(w.cursor(), Vec3{7.0f, 8.0f, 9.0f})) { why = "cursor changed to " + show(w.cursor()); return false; }
        const ember::scene::Object* o = w.get(keep);
        if (!o || o->name != "keep_me") { why = "the sentinel object is gone/renamed"; return false; }
        for (const auto& obj : w.objects())
            if (obj.name != "keep_me") { why = "an object from the failed file leaked in: " + obj.name; return false; }
        return true;
    }
};

void expectRejected(const char* what, const std::string& fileText) {
    const std::string path = tempPath("cursor_bad.ember");
    writeText(path, fileText);
    Sentinel s;
    const bool ok = load(s.w, path);
    CHECK_MSG(!ok, "%s: load() returned true; FORMAT.md §7 says it must fail", what);
    std::string why;
    CHECK_MSG(s.untouched(why), "%s: load failed but the target world was modified: %s", what, why.c_str());
}

} // namespace

TEST(cursor_defaults_to_the_origin) {
    World w;
    CHECK_MSG(sameBits(w.cursor(), Vec3{0, 0, 0}), "fresh world cursor is %s, expected the origin",
              show(w.cursor()).c_str());
}

TEST(cursor_set_get_is_exact_and_moves_nothing_else) {
    World w;
    const ObjectId cube = w.createPrimitive(Primitive::Cube, "cube");
    const ember::Transform before = w.get(cube)->transform;
    w.select(cube);
    w.setCursor(Vec3{1.25f, -2.5f, 0.75f});
    CHECK_MSG(sameBits(w.cursor(), Vec3{1.25f, -2.5f, 0.75f}), "got %s", show(w.cursor()).c_str());
    // "scene data only": no object moved, selection unchanged.
    const ember::Transform after = w.get(cube)->transform;
    CHECK_MSG(sameBits(before.position, after.position) && sameBits(before.rotation, after.rotation) &&
                  sameBits(before.scale, after.scale),
              "setCursor changed an object's transform");
    CHECK_MSG(w.selection().size() == 1 && w.selection()[0].value == cube.value,
              "setCursor changed the selection");
}

TEST(cursor_survives_save_load_bit_exactly) {
    // Includes floats that are NOT representable in few decimal digits, the
    // extremes of the range, a denormal, and negative zero: the 9-digit rule is
    // what makes every one of them round-trip.
    const Vec3 cases[] = {
        {1.25f, -2.5f, 0.75f},
        {0.1f, -123456.78f, 3.14159274f},
        {FLT_MAX, -FLT_MAX, FLT_MIN},
        {1e-45f, -1e-45f, 16777217.0f},
        {-0.0f, 0.0f, -0.0f},
    };
    int n = 0;
    for (const Vec3& c : cases) {
        World src;
        src.createPrimitive(Primitive::Cube, "cube");
        src.setCursor(c);
        const std::string path = tempPath("cursor_rt.ember");
        CHECK_MSG(save(src, path), "case %d: save failed", n);
        World dst;
        CHECK_MSG(load(dst, path), "case %d: load of our own file failed", n);
        CHECK_MSG(sameBits(dst.cursor(), c), "case %d: saved %s, loaded %s", n, show(c).c_str(),
                  show(dst.cursor()).c_str());
        ++n;
    }
}

TEST(cursor_record_is_written_once_in_the_documented_place_and_format) {
    World src;
    src.createPrimitive(Primitive::Cube, "cube");
    src.createPrimitive(Primitive::Sphere, "ball");
    src.setCursor(Vec3{0.1f, -2.5f, 1.0f});
    const std::string path = tempPath("cursor_fmt.ember");
    CHECK(save(src, path));
    const std::vector<std::string> ls = lines(readAll(path));

    int cursorLines = 0, cursorAt = -1, firstObject = -1, frameAt = -1;
    for (int i = 0; i < int(ls.size()); ++i) {
        if (startsWith(ls[i], "cursor ")) { ++cursorLines; cursorAt = i; }
        if (startsWith(ls[i], "object ") && firstObject < 0) firstObject = i;
        if (startsWith(ls[i], "frame ")) frameAt = i;
    }
    CHECK_MSG(cursorLines == 1, "expected exactly one cursor record, found %d", cursorLines);
    CHECK_MSG(cursorAt >= 0 && firstObject >= 0 && cursorAt < firstObject,
              "cursor record (line %d) must precede the first object record (line %d)", cursorAt, firstObject);
    if (frameAt >= 0)
        CHECK_MSG(cursorAt > frameAt, "cursor record (line %d) must come AFTER the frame record (line %d)",
                  cursorAt, frameAt);
    if (cursorAt >= 0) {
        // 9 significant digits: 0.1f is 0.100000001, not "0.1".
        CHECK_MSG(ls[cursorAt] == "cursor 0.100000001 -2.5 1", "cursor line is '%s', expected "
                  "'cursor 0.100000001 -2.5 1' (9 significant digits)", ls[cursorAt].c_str());
    }
}

TEST(cursor_save_is_stable_across_further_hops) {
    World a;
    a.createPrimitive(Primitive::Cube, "cube");
    a.setCursor(Vec3{0.1f, 2.0f / 3.0f, -1e-3f});
    const std::string p1 = tempPath("cursor_h1.ember"), p2 = tempPath("cursor_h2.ember"),
                      p3 = tempPath("cursor_h3.ember");
    CHECK(save(a, p1));
    World b; CHECK(load(b, p1)); CHECK(save(b, p2));
    World c; CHECK(load(c, p2)); CHECK(save(c, p3));
    CHECK_MSG(readAll(p2) == readAll(p3), "second and third hop files differ (cursor not canonical)");
    CHECK_MSG(sameBits(c.cursor(), a.cursor()), "cursor drifted over three hops: %s vs %s",
              show(c.cursor()).c_str(), show(a.cursor()).c_str());
}

TEST(older_v2_without_a_cursor_record_loads_at_the_origin) {
    const std::string path = tempPath("cursor_absent.ember");
    writeText(path, scene(""));
    // The target already has a non-origin cursor: absent record means ORIGIN, not "keep mine".
    World w;
    w.setCursor(Vec3{5.0f, 5.0f, 5.0f});
    const bool ok = load(w, path);
    CHECK_MSG(ok, "a valid v2 file with no cursor record must load");
    if (!ok) return;
    CHECK_MSG(sameBits(w.cursor(), Vec3{0, 0, 0}), "absent cursor record must give the origin, got %s",
              show(w.cursor()).c_str());
    CHECK_MSG(w.objects().size() >= 3, "the file's objects did not load (%zu)", w.objects().size());
}

TEST(cursor_record_from_the_format_example_parses) {
    const std::string path = tempPath("cursor_example.ember");
    writeText(path, scene("cursor 1.25 -2.5 0.75\n"));
    World w;
    CHECK_MSG(load(w, path), "FORMAT.md section 9 shape with a cursor record failed to load");
    CHECK_MSG(sameBits(w.cursor(), Vec3{1.25f, -2.5f, 0.75f}), "got %s", show(w.cursor()).c_str());
}

TEST(cursor_load_rejects_nonfinite_coordinates_and_changes_nothing) {
    expectRejected("cursor nan 0 0", scene("cursor nan 0 0\n"));
    expectRejected("cursor 0 inf 0", scene("cursor 0 inf 0\n"));
    expectRejected("cursor 0 0 -inf", scene("cursor 0 0 -inf\n"));
    expectRejected("cursor 0 0 NaN", scene("cursor 0 0 NaN\n"));
}

TEST(cursor_load_rejects_duplicate_record_and_changes_nothing) {
    expectRejected("two cursor records", scene("cursor 1 2 3\ncursor 4 5 6\n"));
}

TEST(cursor_load_rejects_wrong_field_count_and_changes_nothing) {
    expectRejected("cursor with 2 floats", scene("cursor 1 2\n"));
    expectRejected("cursor with 4 floats", scene("cursor 1 2 3 4\n"));
    expectRejected("cursor with no floats", scene("cursor\n"));
    expectRejected("cursor with a word", scene("cursor 1 two 3\n"));
}

TEST(cursor_load_rejects_out_of_order_record_and_changes_nothing) {
    // After the objects: §2 fixes the order, §7 rule 4 says out-of-order fails.
    expectRejected("cursor after the objects", scene("", "cursor 1 2 3\nend\n"));
    // Before `nextid`.
    expectRejected("cursor before nextid",
                   std::string("EMBER3D.SCENE v2\ncursor 1 2 3\nnextid 5\nselection 1 1\n") + kObjects + "end\n");
}

HARNESS_MAIN()
