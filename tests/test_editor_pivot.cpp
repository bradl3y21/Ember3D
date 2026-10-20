// tests/test_editor_pivot.cpp - owned by Verity / QA. Item 149.
//
// Contract under test: PivotMode (core/Types.h) decides WHERE a rotate/scale happens
// and where the gizmo is drawn. Blender's pivot menu: Median Point, 3D Cursor,
// Individual Origins, Bounding Box Center. The header offered a pivot control before any
// of them did anything; this proves each mode now has an observable effect.
//
// WHAT IS TESTED, by observable outcome and not by reading the resolver's code:
//   * the resolver (resolveTransformPivot) returns the point the mode names -
//     computed independently here (mean of origins; union AABB centre; cursor);
//   * GizmoController puts the gizmo ORIGIN at that point;
//   * a modal ROTATE or SCALE about the 3D cursor moves the object along the arc /
//     radius that geometry predicts (a +90 deg rotation about the cursor takes (2,0,0)
//     to (0,2,0)), as ONE undo entry, and cancel restores the exact original;
//   * the pivot does NOT affect a translate;
//   * a single object about its own origin (Median / Individual / BBox) rotates in place.
//
// SCOPE (stated, not hidden): ModalTransform transforms ONE object - the active one - so
// "each selected object about its own origin" for a MULTI-object selection, and the edit
// mode ActiveElement, are not testable here and are not claimed.
//
// Headless ImGui context, no GL; the real GizmoController / ModalTransform run.
#include "harness.h"

#include "core/Types.h"
#include "editor/Gizmo.h"
#include "editor/Modal.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <string>

using ember::ObjectId;
using ember::PivotMode;
using ember::Transform;
using ember::Vec3;
using ember::editor::GizmoController;
using ember::editor::ModalTransform;
using ember::editor::TransformSpace;
using ember::editor::UndoStack;
using ember::render::OrbitCamera;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr int kFbW = 1280;
constexpr int kFbH = 720;
constexpr float kHalfPi = 1.57079632679f;

struct HeadlessImGui {
    HeadlessImGui() {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(kFbW), float(kFbH));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~HeadlessImGui() { ImGui::SetCurrentContext(ctx_); ImGui::DestroyContext(ctx_); }
    ImGuiContext* ctx_ = nullptr;
};

struct Rig {
    World world;
    UndoStack undo;
    OrbitCamera cam;
    ModalTransform modal;
    GizmoController gizmo;
    ObjectId id;
    HeadlessImGui imgui;   // must outlive any ImGui call

    explicit Rig(Vec3 pos = {2, 0, 0}) {
        id = world.createPrimitive(Primitive::Cube, "pivot-target");
        Transform t{};
        t.position = pos;
        world.setTransform(id, t);
        world.select(id);
    }
    Vec3 pos() const { return world.get(id)->transform.position; }
    const Transform& xf() const { return world.get(id)->transform; }
};

// One frame through the real modal machine with an explicit pivot mode.
void modalFrame(Rig& r, PivotMode pivot, ImGuiKey key = ImGuiKey_None, bool down = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(float(kFbW) * 0.5f, float(kFbH) * 0.5f);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    ImGui::NewFrame();
    (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global, ember::InteractionMode::Object,
                         ember::SelectMode::Vertex, /*snap*/ false, pivot);
    ImGui::EndFrame();
}

void press(Rig& r, PivotMode pivot, ImGuiKey key) {
    modalFrame(r, pivot, key, true);
    modalFrame(r, pivot, key, false);
}

// <startKey> <axisKey> <typed digits...> Enter  (or Escape when cancel)
void gesture(Rig& r, PivotMode pivot, ImGuiKey startKey, ImGuiKey axisKey, const char* digits, bool cancel = false) {
    press(r, pivot, startKey);
    if (axisKey != ImGuiKey_None) press(r, pivot, axisKey);
    for (const char* d = digits; *d; ++d) press(r, pivot, static_cast<ImGuiKey>(ImGuiKey_0 + (*d - '0')));
    for (int i = 0; i < 4; ++i) modalFrame(r, pivot);
    press(r, pivot, cancel ? ImGuiKey_Escape : ImGuiKey_Enter);
}

bool near3(const Vec3& a, const Vec3& b, float eps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}
std::string show(const Vec3& v) {
    char b[96]; std::snprintf(b, sizeof b, "(%.4f, %.4f, %.4f)", double(v.x), double(v.y), double(v.z)); return b;
}

// The gizmo origin for a mode: one real GizmoController::update frame.
Vec3 gizmoOrigin(Rig& r, PivotMode pivot) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(5.0f, 5.0f);   // nowhere near a handle
    ImGui::NewFrame();
    (void)r.gizmo.update(r.world, r.undo, r.cam, TransformSpace::Global, /*visible*/ true, /*snap*/ false, pivot);
    ImGui::EndFrame();
    return r.gizmo.state().origin;
}

} // namespace

// ---- the resolver ---------------------------------------------------------------

TEST(pivot_resolver_single_object_names_the_right_point_for_every_mode) {
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{1.0f, 2.0f, 3.0f});
    const Vec3 origin{2, 0, 0};
    const Vec3 cursor{1, 2, 3};
    using ember::editor::resolveTransformPivot;
    CHECK_MSG(near3(resolveTransformPivot(r.world, PivotMode::MedianPoint), origin, 1e-5f), "Median -> %s",
              show(resolveTransformPivot(r.world, PivotMode::MedianPoint)).c_str());
    CHECK_MSG(near3(resolveTransformPivot(r.world, PivotMode::IndividualOrigins), origin, 1e-5f), "Individual -> %s",
              show(resolveTransformPivot(r.world, PivotMode::IndividualOrigins)).c_str());
    CHECK_MSG(near3(resolveTransformPivot(r.world, PivotMode::BoundingBoxCenter), origin, 1e-3f), "BBox (centred cube) -> %s",
              show(resolveTransformPivot(r.world, PivotMode::BoundingBoxCenter)).c_str());
    CHECK_MSG(near3(resolveTransformPivot(r.world, PivotMode::Cursor3D), cursor, 1e-5f), "Cursor3D -> %s (cursor %s)",
              show(resolveTransformPivot(r.world, PivotMode::Cursor3D)).c_str(), show(cursor).c_str());
}

TEST(pivot_resolver_median_and_bbox_differ_for_two_unequal_objects) {
    // A: unit cube at the origin. B: the same cube scaled x2 at (6,0,0). Origins' mean is
    // (3,0,0); the union bounding box is NOT symmetric about that, so the two modes must
    // disagree - which is exactly what a user choosing between them expects to see.
    Rig r({0.0f, 0.0f, 0.0f});
    const ObjectId a = r.id;
    // Half-extent of the stock cube, read from its own bounds (centre 0, radius = half-diagonal).
    float h;
    {
        World w;
        const ObjectId c = w.createPrimitive(Primitive::Cube, "c");
        w.select(c);
        const auto b = w.selectionBounds();
        h = b ? b->radius / std::sqrt(3.0f) : 0.5f;
    }
    const ObjectId b = r.world.createPrimitive(Primitive::Cube, "B");
    Transform tb{};
    tb.position = {6.0f, 0.0f, 0.0f};
    tb.scale = {2.0f, 2.0f, 2.0f};
    r.world.setTransform(b, tb);
    r.world.select(a);
    r.world.select(b, /*additive*/ true);
    CHECK_MSG(r.world.selection().size() == 2, "expected 2 selected objects, got %zu", r.world.selection().size());

    using ember::editor::resolveTransformPivot;
    const Vec3 median = resolveTransformPivot(r.world, PivotMode::MedianPoint);
    const Vec3 bbox = resolveTransformPivot(r.world, PivotMode::BoundingBoxCenter);
    const Vec3 individual = resolveTransformPivot(r.world, PivotMode::IndividualOrigins);
    const float expectedBboxX = ((0.0f - h) + (6.0f + 2.0f * h)) * 0.5f;   // union of [-h,h] and [6-2h, 6+2h]
    CHECK_MSG(near3(median, Vec3{3, 0, 0}, 1e-4f), "Median of origins (0,0,0) and (6,0,0) -> %s, expected (3,0,0)",
              show(median).c_str());
    CHECK_MSG(std::fabs(bbox.x - expectedBboxX) < 2e-3f && std::fabs(bbox.y) < 2e-3f && std::fabs(bbox.z) < 2e-3f,
              "BBox centre -> %s, expected (%.4f,0,0) from half-extent %.3f", show(bbox).c_str(), double(expectedBboxX), double(h));
    CHECK_MSG(std::fabs(bbox.x - median.x) > 0.05f, "Median %s and BBox %s must differ for unequal objects",
              show(median).c_str(), show(bbox).c_str());
    // Individual: the ACTIVE (last selected) object's origin.
    CHECK_MSG(near3(individual, Vec3{6, 0, 0}, 1e-4f), "Individual -> %s, expected the active object's origin (6,0,0)",
              show(individual).c_str());
}

// ---- the gizmo ------------------------------------------------------------------

TEST(pivot_gizmo_origin_follows_the_pivot_mode) {
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{0.0f, 0.0f, 0.0f});
    const Vec3 med = gizmoOrigin(r, PivotMode::MedianPoint);
    const Vec3 cur = gizmoOrigin(r, PivotMode::Cursor3D);
    CHECK_MSG(near3(med, Vec3{2, 0, 0}, 1e-4f), "Median: gizmo origin %s, expected the object (2,0,0)", show(med).c_str());
    CHECK_MSG(near3(cur, Vec3{0, 0, 0}, 1e-4f), "Cursor3D: gizmo origin %s, expected the cursor (0,0,0)", show(cur).c_str());
    r.world.setCursor(Vec3{-3.0f, 1.0f, 4.0f});
    const Vec3 cur2 = gizmoOrigin(r, PivotMode::Cursor3D);
    CHECK_MSG(near3(cur2, Vec3{-3, 1, 4}, 1e-4f), "after moving the cursor, gizmo origin %s, expected (-3,1,4)", show(cur2).c_str());
}

// ---- rotate / scale about the pivot ---------------------------------------------

TEST(pivot_cursor3d_rotate_90_about_z_takes_2_0_0_to_0_2_0_as_one_undo) {
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{0, 0, 0});
    gesture(r, PivotMode::Cursor3D, ImGuiKey_R, ImGuiKey_Z, "90");
    CHECK_MSG(near3(r.pos(), Vec3{0, 2, 0}, 2e-3f), "rotated 90 deg about the cursor: position %s, expected (0,2,0)",
              show(r.pos()).c_str());
    CHECK_MSG(std::fabs(r.xf().rotation.z - kHalfPi) < 2e-3f, "object's own rotation.z = %g, expected pi/2", double(r.xf().rotation.z));
    CHECK_MSG(r.undo.doneCount() == 1, "%zu undo entries, expected 1", r.undo.doneCount());
}

TEST(pivot_cursor3d_rotation_uses_the_cursor_not_the_origin) {
    // Cursor at (1,0,0): the object at (2,0,0) is 1 unit from it. +90 about Z -> (1,1,0).
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{1, 0, 0});
    gesture(r, PivotMode::Cursor3D, ImGuiKey_R, ImGuiKey_Z, "90");
    CHECK_MSG(near3(r.pos(), Vec3{1, 1, 0}, 2e-3f), "position %s, expected (1,1,0)", show(r.pos()).c_str());
}

TEST(pivot_median_individual_and_bbox_rotate_a_single_object_in_place) {
    for (PivotMode m : {PivotMode::MedianPoint, PivotMode::IndividualOrigins, PivotMode::BoundingBoxCenter}) {
        Rig r({2.0f, 0.0f, 0.0f});
        r.world.setCursor(Vec3{0, 0, 0});   // the cursor must NOT matter here
        gesture(r, m, ImGuiKey_R, ImGuiKey_Z, "90");
        CHECK_MSG(near3(r.pos(), Vec3{2, 0, 0}, 2e-3f), "pivot mode %d: position %s, expected it to stay at (2,0,0)", int(m),
                  show(r.pos()).c_str());
        CHECK_MSG(std::fabs(r.xf().rotation.z - kHalfPi) < 2e-3f, "pivot mode %d: rotation.z = %g, expected pi/2", int(m),
                  double(r.xf().rotation.z));
    }
}

TEST(pivot_cursor3d_scale_moves_the_object_away_from_the_cursor) {
    // Uniform scale x2 about the cursor (0,0,0): an object at (2,0,0) goes to (4,0,0) and doubles.
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{0, 0, 0});
    gesture(r, PivotMode::Cursor3D, ImGuiKey_S, ImGuiKey_None, "2");
    CHECK_MSG(near3(r.pos(), Vec3{4, 0, 0}, 2e-3f), "scaled x2 about the cursor: position %s, expected (4,0,0)", show(r.pos()).c_str());
    CHECK_MSG(near3(r.xf().scale, Vec3{2, 2, 2}, 1e-4f), "scale %s, expected (2,2,2)", show(r.xf().scale).c_str());
    CHECK_MSG(r.undo.doneCount() == 1, "%zu undo entries, expected 1", r.undo.doneCount());
}

TEST(pivot_median_scale_a_single_object_stays_in_place) {
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{0, 0, 0});
    gesture(r, PivotMode::MedianPoint, ImGuiKey_S, ImGuiKey_None, "2");
    CHECK_MSG(near3(r.pos(), Vec3{2, 0, 0}, 2e-3f), "position %s, expected (2,0,0)", show(r.pos()).c_str());
    CHECK_MSG(near3(r.xf().scale, Vec3{2, 2, 2}, 1e-4f), "scale %s, expected (2,2,2)", show(r.xf().scale).c_str());
}

TEST(pivot_does_not_change_a_translate) {
    for (PivotMode m : {PivotMode::MedianPoint, PivotMode::Cursor3D, PivotMode::IndividualOrigins, PivotMode::BoundingBoxCenter}) {
        Rig r({2.0f, 0.0f, 0.0f});
        r.world.setCursor(Vec3{7, 7, 7});
        gesture(r, m, ImGuiKey_G, ImGuiKey_X, "3");
        CHECK_MSG(near3(r.pos(), Vec3{5, 0, 0}, 1e-4f), "pivot mode %d: G X 3 gave %s, expected (5,0,0)", int(m), show(r.pos()).c_str());
    }
}

TEST(pivot_cursor3d_rotate_cancel_restores_the_exact_original) {
    Rig r({2.0f, 0.0f, 0.0f});
    r.world.setCursor(Vec3{0, 0, 0});
    const Transform before = r.xf();
    gesture(r, PivotMode::Cursor3D, ImGuiKey_R, ImGuiKey_Z, "90", /*cancel*/ true);
    const Transform after = r.xf();
    const bool same = before.position.x == after.position.x && before.position.y == after.position.y &&
                      before.position.z == after.position.z && before.rotation.x == after.rotation.x &&
                      before.rotation.y == after.rotation.y && before.rotation.z == after.rotation.z &&
                      before.scale.x == after.scale.x && before.scale.y == after.scale.y && before.scale.z == after.scale.z;
    CHECK_MSG(same, "cancel did not restore the exact original: position %s rotation.z %g", show(after.position).c_str(),
              double(after.rotation.z));
    CHECK_MSG(r.undo.doneCount() == 0, "a cancelled gesture left %zu undo entries", r.undo.doneCount());
}

HARNESS_MAIN()
