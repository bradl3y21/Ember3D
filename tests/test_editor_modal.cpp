// tests/test_editor_modal.cpp — owned by Verity · QA.
//
// Contract under test: the modal transform state machine (src/editor/Modal.cpp,
// WHAT-REMAINS §A2 / state machine D29 §4.5). Chisel's EMBER_SELFTEST_TRANSFORM
// scripted path proves the TRANSLATE gesture end-to-end and states plainly that
// "rotate and scale gestures end-to-end (script covers translate; rotate/scale
// share the same machine but have no scripted assertions yet)" are NOT verified.
// This file closes exactly that gap, and re-confirms the three invariants the
// task names, independently of his harness:
//
//   1. a typed value produces the EXACT commanded transform (translate AND
//      rotate AND scale);
//   2. cancel restores the EXACT original (component-wise equality, which for
//      IEEE-754 non-NaN floats is bitwise);
//   3. ONE gesture produces EXACTLY ONE undo entry — including a gesture that
//      spans many frames, which is where a continuation-merge bug would hide.
//
// WHY A HEADLESS IMGUI CONTEXT: ModalTransform::update() reads ImGui::GetIO()
// for mouse/keyboard, and its callees are world.resolveWorldAxis /
// world.typedTranslate / world.typedRotate / world.composeWorldScalePivot /
// camera.projectScreen / undo.push — none of which touch OpenGL. A context
// created WITHOUT a GPU backend is therefore enough, and the tests exercise the
// REAL implementation rather than a QA copy of it.
//
// The drag-math assertion is NOT circular: the expected world delta is derived
// from the camera's own public projectScreen() (pivot vs pivot+axis), i.e. the
// documented D19 formula (pixels along the axis divided by pixels-per-unit) —
// not from Modal.cpp's internals.
#include "harness.h"

#include "core/Types.h"
#include "editor/Modal.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <string>

using ember::ObjectId;
using ember::Transform;
using ember::Vec3;
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

// Owns the ImGui context for the whole file; CPU-only, never rendered.
struct HeadlessImGui {
    HeadlessImGui() {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx_);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize             = ImVec2(static_cast<float>(kFbW),
                                            static_cast<float>(kFbH));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime               = 1.0f / 60.0f;
        io.IniFilename             = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~HeadlessImGui() {
        ImGui::SetCurrentContext(ctx_);
        ImGui::DestroyContext(ctx_);
    }
    ImGuiContext* ctx_ = nullptr;
};

// Exact equality. For IEEE-754 non-NaN values this IS bitwise equality; it is
// written as component-wise so a failure reports WHICH component moved.
bool exactlyEqual(const Vec3& a, const Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool exactlyEqual(const Transform& a, const Transform& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
}

std::string fmt(const Transform& t) {
    char b[160];
    std::snprintf(b, sizeof b,
                  "pos=(%.6f,%.6f,%.6f) rot=(%.6f,%.6f,%.6f) scale=(%.6f,%.6f,%.6f)",
                  static_cast<double>(t.position.x), static_cast<double>(t.position.y),
                  static_cast<double>(t.position.z), static_cast<double>(t.rotation.x),
                  static_cast<double>(t.rotation.y), static_cast<double>(t.rotation.z),
                  static_cast<double>(t.scale.x), static_cast<double>(t.scale.y),
                  static_cast<double>(t.scale.z));
    return b;
}

// One synthesized frame through the REAL modal machine. Events are queued
// BEFORE NewFrame, exactly how a real backend feeds input.
void frame(ModalTransform& modal, World& world, UndoStack& undo,
           const OrbitCamera& cam, float mouseX, float mouseY, bool ctrl = false,
           bool shift = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouseX, mouseY);
    // Modifier STATE is submitted under ImGuiMod_Ctrl / ImGuiMod_Shift, not the
    // Left* variants — the same 1.93 backend contract that bit the gizmo tests.
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    ImGui::NewFrame();
    (void)modal.update(world, undo, cam, TransformSpace::Global);
    ImGui::EndFrame();
}

// A one-frame key press (down this frame, up the next).
void tap(ModalTransform& modal, World& world, UndoStack& undo, const OrbitCamera& cam,
         int frame, int downFrame, ImGuiKey key, float mouseX, float mouseY) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouseX, mouseY);
    io.AddKeyEvent(key, frame == downFrame);
    ImGui::NewFrame();
    (void)modal.update(world, undo, cam, TransformSpace::Global);
    ImGui::EndFrame();
}

struct Rig {
    World world;
    UndoStack undo;
    OrbitCamera cam;
    ModalTransform modal;
    ObjectId id;
    HeadlessImGui imgui;   // must outlive any ImGui call

    Rig() {
        id = world.createPrimitive(Primitive::Cube, "modal-target");
        Transform t{};                    // identity: pos (0,0,0), rot (0,0,0), scale 1
        world.setTransform(id, t);
        world.select(id);
    }
};

const float kCx = static_cast<float>(kFbW) * 0.5f;
const float kCy = static_cast<float>(kFbH) * 0.5f;

// ---- typed value -> exact transform, for all THREE modes -------------------

TEST(modal_typed_translate_is_exact_and_one_undo) {
    Rig r;
    const Transform before = r.world.get(r.id)->transform;

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_2, kCx, kCy);
    // Let the machine apply the typed value over a few steady frames.
    for (int f = 6; f <= 9; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 10, 10, ImGuiKey_Enter, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.position.x, before.position.x + 2.0f, 1e-5f);
    CHECK(after.position.y == before.position.y);
    CHECK(after.position.z == before.position.z);
    CHECK(exactlyEqual(after.rotation, before.rotation));
    CHECK(exactlyEqual(after.scale, before.scale));
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});
}

TEST(modal_typed_rotate_is_exact_and_one_undo) {
    // The path Chisel's script does NOT cover. Rotating about world Z by 90
    // degrees from an identity rotation must land on euler (0, 0, +pi/2) and
    // must not move the object (the pivot is its own origin).
    Rig r;
    const Transform before = r.world.get(r.id)->transform;

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_R, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_Z, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_9, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 7, 7, ImGuiKey_0, kCx, kCy);
    for (int f = 8; f <= 11; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 12, 12, ImGuiKey_Enter, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.rotation.z, 3.14159265358979f * 0.5f, 1e-4f);
    CHECK_NEAR(after.rotation.x, 0.0f, 1e-5f);
    CHECK_NEAR(after.rotation.y, 0.0f, 1e-5f);
    CHECK(exactlyEqual(after.position, before.position));
    CHECK(exactlyEqual(after.scale, before.scale));
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});
}

TEST(modal_typed_rotate_neg90_direction) {
    // Direction check: -90 must go the OTHER way from +90. A sign error in the
    // degrees->radians conversion would still "look rotated"; this catches it.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_R, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_Z, kCx, kCy);
    // type "-90"
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_Minus, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 7, 7, ImGuiKey_9, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 9, 9, ImGuiKey_0, kCx, kCy);
    for (int f = 10; f <= 13; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 14, 14, ImGuiKey_Enter, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.rotation.z, -3.14159265358979f * 0.5f, 1e-4f);
}

TEST(modal_typed_scale_is_exact_and_one_undo) {
    // The other unverified path. S then "2" from scale 1 must be exactly 2 on
    // all three axes, with position and rotation untouched.
    Rig r;
    const Transform before = r.world.get(r.id)->transform;

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_S, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_2, kCx, kCy);
    for (int f = 4; f <= 7; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 8, 8, ImGuiKey_Enter, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.scale.x, 2.0f, 1e-5f);
    CHECK_NEAR(after.scale.y, 2.0f, 1e-5f);
    CHECK_NEAR(after.scale.z, 2.0f, 1e-5f);
    CHECK(exactlyEqual(after.position, before.position));
    CHECK(exactlyEqual(after.rotation, before.rotation));
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});
}

TEST(modal_typed_scale_axis_constrained) {
    // S, X, "3" — only the constrained axis may scale; the others stay 1.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_S, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_3, kCx, kCy);
    for (int f = 6; f <= 9; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 10, 10, ImGuiKey_Enter, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.scale.x, 3.0f, 1e-5f);
    CHECK_NEAR(after.scale.y, 1.0f, 1e-5f);
    CHECK_NEAR(after.scale.z, 1.0f, 1e-5f);
}

// ---- cancel restores the EXACT original ------------------------------------

TEST(modal_cancel_restores_exact_after_a_many_frame_drag) {
    // The strongest form of the cancel claim: a drag long enough to push a
    // command and several continuations, then Esc. The transform must come back
    // component-for-component, and the stack must be untouched.
    Rig r;
    const Transform before = r.world.get(r.id)->transform;
    const std::size_t depthBefore = r.undo.doneCount();

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    // Move the mouse steadily for 12 frames: enough for a push + 11 continuations.
    for (int f = 4; f <= 15; ++f) {
        const float x = kCx + 12.0f * static_cast<float>(f - 3);
        frame(r.modal, r.world, r.undo, r.cam, x, kCy);
    }
    // The drag MUST have actually moved the object, or this test proves nothing.
    const Transform dragged = r.world.get(r.id)->transform;
    CHECK(!exactlyEqual(dragged, before));

    tap(r.modal, r.world, r.undo, r.cam, 16, 16, ImGuiKey_Escape, kCx, kCy);

    const Transform after = r.world.get(r.id)->transform;
    CHECK(exactlyEqual(after, before));
    CHECK_EQ(r.undo.doneCount(), depthBefore);
}

TEST(modal_right_click_cancels_too) {
    // D29: right-click is a second, equal cancel path.
    Rig r;
    const Transform before = r.world.get(r.id)->transform;

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    for (int f = 2; f <= 8; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx + 10.0f * f, kCy);
    CHECK(!exactlyEqual(r.world.get(r.id)->transform, before));

    // Right button click on frame 9.
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(kCx, kCy);
        io.AddMouseButtonEvent(1, true);
        ImGui::NewFrame();
        (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global);
        ImGui::EndFrame();
        io.AddMouseButtonEvent(1, false);
    }
    CHECK(exactlyEqual(r.world.get(r.id)->transform, before));
    CHECK_EQ(r.undo.doneCount(), std::size_t{0});
}

// ---- exactly ONE undo entry per gesture ------------------------------------

TEST(modal_many_frame_drag_is_exactly_one_undo_entry) {
    // This is the invariant a continuation-merge bug would break: 12 frames of
    // dragging push 1 command + 11 continuations. mergeWith() must collapse
    // them into a SINGLE entry, so a confirmed gesture is one undo.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    for (int f = 4; f <= 15; ++f) {
        frame(r.modal, r.world, r.undo, r.cam, kCx + 12.0f * static_cast<float>(f - 3), kCy);
    }
    tap(r.modal, r.world, r.undo, r.cam, 16, 16, ImGuiKey_Enter, kCx, kCy);

    CHECK_EQ(r.undo.doneCount(), std::size_t{1});

    // ...and that ONE entry, when undone, returns the object to the start.
    const Transform before = r.world.get(r.id)->transform;   // after confirm
    (void)before;
    r.undo.undo();
    const Transform undone = r.world.get(r.id)->transform;
    Transform expected{};                       // the identity the object started at
    CHECK(exactlyEqual(undone, expected));
}

TEST(modal_two_gestures_are_two_entries) {
    Rig r;
    // gesture 1: translate +2 x
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_1, kCx, kCy);
    for (int f = 6; f <= 8; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 9, 9, ImGuiKey_Enter, kCx, kCy);
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});

    // gesture 2: rotate 45 about Z
    tap(r.modal, r.world, r.undo, r.cam, 20, 20, ImGuiKey_R, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 22, 22, ImGuiKey_Z, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 24, 24, ImGuiKey_4, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 26, 26, ImGuiKey_5, kCx, kCy);
    for (int f = 27; f <= 29; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 30, 30, ImGuiKey_Enter, kCx, kCy);
    CHECK_EQ(r.undo.doneCount(), std::size_t{2});

    const Transform after = r.world.get(r.id)->transform;
    CHECK_NEAR(after.position.x, 1.0f, 1e-5f);
    CHECK_NEAR(after.rotation.z, 3.14159265358979f / 4.0f, 1e-4f);

    // Undo runs them back in turn: two undos return to the identity.
    r.undo.undo();
    r.undo.undo();
    CHECK(exactlyEqual(r.world.get(r.id)->transform, Transform{}));
}

TEST(modal_cancelled_gesture_leaves_the_stack_untouched) {
    // A cancel after a drag must leave NO entry — not a merged one, not a
    // discarded-but-counted one.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    for (int f = 2; f <= 10; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx + 8.0f * f, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 11, 11, ImGuiKey_Escape, kCx, kCy);

    CHECK_EQ(r.undo.doneCount(), std::size_t{0});
    CHECK(exactlyEqual(r.world.get(r.id)->transform, Transform{}));
}

TEST(modal_confirm_by_left_click) {
    // D29: left click is a confirm path equal to Enter.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 5, 5, ImGuiKey_3, kCx, kCy);
    for (int f = 6; f <= 8; ++f) frame(r.modal, r.world, r.undo, r.cam, kCx, kCy);
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(kCx, kCy);
        io.AddMouseButtonEvent(0, true);
        ImGui::NewFrame();
        (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global);
        ImGui::EndFrame();
        io.AddMouseButtonEvent(0, false);
    }
    CHECK_NEAR(r.world.get(r.id)->transform.position.x, 3.0f, 1e-5f);
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});
}

// ---- the axis constraint on the drag path ----------------------------------

TEST(modal_drag_axis_constraint_holds_and_direction_is_right) {
    // Drag along +screen-x under a +X constraint: y and z must not move AT ALL,
    // and x must move in the direction the camera's own projection implies.
    // The expected magnitude is computed from camera.projectScreen (the D19
    // formula), NOT from Modal.cpp — so this is an independent check.
    Rig r;
    const Transform before = r.world.get(r.id)->transform;

    float px = 0.0f, py = 0.0f, tx = 0.0f, ty = 0.0f;
    const bool okPivot = r.cam.projectScreen(Vec3{0, 0, 0}, kFbW, kFbH, px, py);
    const bool okTip = r.cam.projectScreen(Vec3{1, 0, 0}, kFbW, kFbH, tx, ty);
    CHECK(okPivot && okTip);

    const float dx = tx - px;
    const float dy = ty - py;
    const float len2 = dx * dx + dy * dy;
    CHECK(len2 > 1e-6f);

    const float dragPx = 150.0f;
    const float expectedUnits = dragPx * dx / len2;    // D19: px along axis / px-per-unit

    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    for (int f = 4; f <= 12; ++f) {
        const float x = kCx + dragPx * static_cast<float>(f - 3) / 9.0f;
        frame(r.modal, r.world, r.undo, r.cam, x, kCy);
    }
    const Transform dragged = r.world.get(r.id)->transform;

    CHECK(dragged.position.y == before.position.y);
    CHECK(dragged.position.z == before.position.z);
    // Magnitude: the final mouse position is kCx+dragPx, so the total drag is
    // exactly dragPx along screen x.
    CHECK_NEAR(dragged.position.x, before.position.x + expectedUnits, 1e-3f);

    tap(r.modal, r.world, r.undo, r.cam, 13, 13, ImGuiKey_Enter, kCx, kCy);
    CHECK_EQ(r.undo.doneCount(), std::size_t{1});
}

TEST(modal_precision_shift_shrinks_the_drag) {
    // Shift = precision (kPrecision = 0.1): the same mouse travel must produce
    // a tenth of the displacement.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    for (int f = 4; f <= 12; ++f) {
        const float x = kCx + 150.0f * static_cast<float>(f - 3) / 9.0f;
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, kCy);
        io.AddKeyEvent(ImGuiMod_Shift, true);
        ImGui::NewFrame();
        (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global);
        ImGui::EndFrame();
    }
    const float precise = r.world.get(r.id)->transform.position.x;

    // Same drag without Shift, from a fresh rig, must be ~10x larger.
    Rig r2;
    tap(r2.modal, r2.world, r2.undo, r2.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r2.modal, r2.world, r2.undo, r2.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    for (int f = 4; f <= 12; ++f) {
        const float x = kCx + 150.0f * static_cast<float>(f - 3) / 9.0f;
        frame(r2.modal, r2.world, r2.undo, r2.cam, x, kCy);
    }
    const float normal = r2.world.get(r2.id)->transform.position.x;

    CHECK(std::fabs(normal) > std::fabs(precise));
    if (std::fabs(normal) > 1e-4f) {
        CHECK_NEAR(precise / normal, 0.1f, 0.02f);
    }
}

TEST(modal_ctrl_snap_rounds_to_whole_units) {
    // Ctrl = snap, translate increment 1.0 world unit.
    Rig r;
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    tap(r.modal, r.world, r.undo, r.cam, 3, 3, ImGuiKey_X, kCx, kCy);
    for (int f = 4; f <= 12; ++f) {
        const float x = kCx + 137.0f * static_cast<float>(f - 3) / 9.0f;
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, kCy);
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        ImGui::NewFrame();
        (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global);
        ImGui::EndFrame();
    }
    const float snapped = r.world.get(r.id)->transform.position.x;
    CHECK_NEAR(snapped, std::floor(snapped + 0.5f), 1e-4f);
}

TEST(modal_inactive_when_nothing_selected) {
    // start() bails when there is no selection: no crash, no entry, no motion.
    Rig r;
    r.world.clearSelection();
    const std::size_t depth = r.undo.doneCount();
    tap(r.modal, r.world, r.undo, r.cam, 1, 1, ImGuiKey_G, kCx, kCy);
    CHECK(!r.modal.active());
    CHECK_EQ(r.undo.doneCount(), depth);
}

} // namespace

// ---- Shift+X/Y/Z plane constraint (item 159) -----------------------------------
//
// WHAT-REMAINS section 2: "Shift+X/Y/Z constrains to a plane". Blender's meaning: Shift+X
// EXCLUDES X - the object moves freely in the plane perpendicular to X (the YZ plane).
// Nothing tested this before. Prediction, from what a user sees and no Modal.cpp maths:
// grab the object on its own screen position and move the pointer by (dx,dy) -
//   (a) the constrained world coordinate is unchanged EXACTLY,
//   (b) the object ends up UNDER THE CURSOR (its projected position is the pointer),
//       which is what moving an object on a plane means,
//   (c) the whole gesture is ONE undo entry.
// A plane seen edge-on cannot be dragged (no unique point under the cursor); there the
// requirement is only that nothing blows up.

namespace {

constexpr float kRadPerPx = 0.0087f;   // OrbitCamera::orbit(): yaw -= dx*k, pitch += dy*k

bool setOrientation(OrbitCamera& cam, float yawDeg, float pitchDeg) {
    const float d2r = 3.14159265358979f / 180.0f;
    cam.setView(ember::render::StandardView::Front);
    cam.orbit(-yawDeg * d2r / kRadPerPx, pitchDeg * d2r / kRadPerPx);
    const float dyaw = std::remainder(cam.yaw() - yawDeg * d2r, 2.0f * 3.14159265358979f);
    return std::fabs(dyaw) < 2e-3f && std::fabs(cam.pitch() - pitchDeg * d2r) < 2e-3f;
}

// One frame with explicit key + modifier state (key edge = down this frame).
void keyFrame(Rig& r, float mx, float my, ImGuiKey key, bool keyDown, bool shift) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mx, my);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, keyDown);
    ImGui::NewFrame();
    (void)r.modal.update(r.world, r.undo, r.cam, TransformSpace::Global);
    ImGui::EndFrame();
}

struct PlaneResult {
    Vec3 pos{};
    bool finite = true;
    float screenErr = 0.0f;          // px between the object's projection and the pointer
    std::size_t undoEntries = 0;
    bool ok = false;
};

// G, Shift+<axis>, move the pointer from the object's screen position by (dx,dy), Enter.
PlaneResult planeGesture(Rig& r, int axis, float dx, float dy) {
    PlaneResult out;
    float ox = 0, oy = 0;
    if (!r.cam.projectScreen(Vec3{0, 0, 0}, kFbW, kFbH, ox, oy)) return out;
    const ImGuiKey axisKey = axis == 0 ? ImGuiKey_X : (axis == 1 ? ImGuiKey_Y : ImGuiKey_Z);
    keyFrame(r, ox, oy, ImGuiKey_G, true, false);
    keyFrame(r, ox, oy, ImGuiKey_G, false, false);
    keyFrame(r, ox, oy, axisKey, true, true);           // Shift+X/Y/Z
    keyFrame(r, ox, oy, axisKey, false, true);
    keyFrame(r, ox, oy, ImGuiKey_None, false, false);   // Shift released: no precision scaling
    for (int s = 1; s <= 4; ++s)
        keyFrame(r, ox + dx * s / 4.0f, oy + dy * s / 4.0f, ImGuiKey_None, false, false);
    keyFrame(r, ox + dx, oy + dy, ImGuiKey_None, false, false);
    keyFrame(r, ox + dx, oy + dy, ImGuiKey_Enter, true, false);
    keyFrame(r, ox + dx, oy + dy, ImGuiKey_Enter, false, false);
    out.pos = r.world.get(r.id)->transform.position;
    out.finite = std::isfinite(out.pos.x) && std::isfinite(out.pos.y) && std::isfinite(out.pos.z);
    float ex = 0, ey = 0;
    if (out.finite && r.cam.projectScreen(out.pos, kFbW, kFbH, ex, ey))
        out.screenErr = std::sqrt((ex - (ox + dx)) * (ex - (ox + dx)) + (ey - (oy + dy)) * (ey - (oy + dy)));
    else
        out.screenErr = 1e9f;
    out.undoEntries = r.undo.doneCount();
    out.ok = true;
    return out;
}

float coord(const Vec3& p, int axis) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); }

} // namespace

TEST(modal_shift_axis_plane_keeps_the_axis_and_follows_the_cursor_at_144_orientations) {
    int tried = 0, skippedEdgeOn = 0, failed = 0;
    float worstScreen = 0.0f, worstLeak = 0.0f;
    std::string worstWhere;
    for (int yaw = 0; yaw < 360; yaw += 30) {
        for (int pitch : {-60, -30, 20, 50}) {
            for (int axis = 0; axis < 3; ++axis) {
                Rig r;
                if (!setOrientation(r.cam, float(yaw), float(pitch))) {
                    harness::reportFailure(__FILE__, __LINE__, "camera did not reach yaw " + std::to_string(yaw) +
                                           " pitch " + std::to_string(pitch));
                    continue;
                }
                // Skip a plane seen (nearly) edge-on: normal ~ perpendicular to the view direction.
                const Vec3 eye = r.cam.position(), tgt = r.cam.target();
                Vec3 v{tgt.x - eye.x, tgt.y - eye.y, tgt.z - eye.z};
                const float vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                const float facing = std::fabs(coord(Vec3{v.x / vl, v.y / vl, v.z / vl}, axis));
                if (facing < 0.25f) { ++skippedEdgeOn; continue; }

                const std::string where = "yaw " + std::to_string(yaw) + " pitch " + std::to_string(pitch) +
                                          " plane perpendicular to axis " + std::to_string(axis);
                const PlaneResult g = planeGesture(r, axis, 80.0f, -50.0f);
                ++tried;
                const float leak = std::fabs(coord(g.pos, axis));
                if (leak > worstLeak) worstLeak = leak;
                if (g.screenErr < 1e8f && g.screenErr > worstScreen) { worstScreen = g.screenErr; worstWhere = where; }
                const bool bad = !g.ok || !g.finite || leak > 1e-4f || g.screenErr > 2.0f || g.undoEntries != 1;
                if (bad && ++failed <= 8)
                    harness::reportFailure(__FILE__, __LINE__,
                        where + ": constrained coordinate drifted by " + std::to_string(leak) +
                        ", object is " + std::to_string(g.screenErr) + "px from the cursor, undo entries " +
                        std::to_string(g.undoEntries) + " (expected 1)");
                ++harness::g_checks;
            }
        }
    }
    std::printf("       plane gestures tried %d, skipped %d edge-on (facing<0.25); worst constrained-axis drift %.2e; "
                "worst distance from cursor %.2fpx at [%s]; failed %d\n",
                tried, skippedEdgeOn, double(worstLeak), double(worstScreen), worstWhere.c_str(), failed);
    if (tried < 80)
        harness::reportFailure(__FILE__, __LINE__, "only " + std::to_string(tried) + " plane gestures ran (<80)");
    ++harness::g_checks;
}

TEST(modal_shift_axis_plane_is_not_the_same_as_the_axis_constraint) {
    // Contrast, so the test cannot pass vacuously: plain X moves ONLY along X; Shift+X moves
    // in the YZ plane and leaves X alone. Looked at from a general angle.
    Rig a, b;
    CHECK(setOrientation(a.cam, 35.0f, 25.0f));
    CHECK(setOrientation(b.cam, 35.0f, 25.0f));
    const PlaneResult plane = planeGesture(a, 0, 90.0f, -60.0f);          // Shift+X: free in YZ
    CHECK_NEAR(plane.pos.x, 0.0f, 1e-4f);
    CHECK(std::fabs(plane.pos.y) > 0.05f || std::fabs(plane.pos.z) > 0.05f);
    float ox = 0, oy = 0;
    b.cam.projectScreen(Vec3{0, 0, 0}, kFbW, kFbH, ox, oy);
    keyFrame(b, ox, oy, ImGuiKey_G, true, false);
    keyFrame(b, ox, oy, ImGuiKey_G, false, false);
    keyFrame(b, ox, oy, ImGuiKey_X, true, false);                          // plain X
    keyFrame(b, ox, oy, ImGuiKey_X, false, false);
    for (int s = 1; s <= 4; ++s) keyFrame(b, ox + 90.0f * s / 4.0f, oy - 60.0f * s / 4.0f, ImGuiKey_None, false, false);
    keyFrame(b, ox + 90.0f, oy - 60.0f, ImGuiKey_Enter, true, false);
    keyFrame(b, ox + 90.0f, oy - 60.0f, ImGuiKey_Enter, false, false);
    const Vec3 axisOnly = b.world.get(b.id)->transform.position;
    CHECK_NEAR(axisOnly.y, 0.0f, 1e-4f);
    CHECK_NEAR(axisOnly.z, 0.0f, 1e-4f);
    CHECK(std::fabs(axisOnly.x) > 0.05f);
}

TEST(modal_shift_axis_plane_seen_nearly_edge_on_does_not_blow_up) {
    // Camera 0.5..3 degrees off the plane's edge: a 100px drag could shoot the object
    // across the scene. Required: finite, constrained coordinate untouched, AT MOST one undo entry.
    // Observed (not asserted): the pointer ray cannot meet an edge-on plane in front of the camera, so the
    // object simply does not move - and at 3 degrees such a gesture still records one undo entry that
    // changes nothing (a no-op entry; printed per case below, reported in the item-159 finding).
    float worst = 0.0f;
    std::string worstWhere;
    for (float offDeg : {0.0f, 0.5f, 1.0f, 3.0f}) {
        for (int axis : {0, 2}) {
            Rig r;
            // axis 0: plane normal X, view along Z (yaw ~0) is edge-on; axis 2: normal Z, view along X (yaw ~90).
            const float yaw = (axis == 0 ? 0.0f : 90.0f) + offDeg;
            CHECK(setOrientation(r.cam, yaw, 0.0f));
            const PlaneResult g = planeGesture(r, axis, 100.0f, -40.0f);
            const std::string where = "plane normal axis " + std::to_string(axis) + ", " + std::to_string(offDeg) + " deg off edge-on";
            CHECK_MSG(g.ok && g.finite, "%s: position became non-finite", where.c_str());
            CHECK_MSG(std::fabs(coord(g.pos, axis)) < 1e-3f, "%s: constrained coordinate moved to %g", where.c_str(),
                      double(coord(g.pos, axis)));
            CHECK_MSG(g.undoEntries <= 1, "%s: %zu undo entries (a gesture is at most ONE)", where.c_str(), g.undoEntries);
            const float mag = std::sqrt(g.pos.x * g.pos.x + g.pos.y * g.pos.y + g.pos.z * g.pos.z);
            std::printf("       [%s]: pos (%g, %g, %g) |d|=%g undo=%zu\n", where.c_str(), double(g.pos.x), double(g.pos.y), double(g.pos.z), double(mag), g.undoEntries);
            if (g.finite && mag > worst) { worst = mag; worstWhere = where; }
        }
    }
    std::printf("       (info) largest displacement for a 100px drag on a near-edge-on plane: %.1f world units [%s]\n",
                double(worst), worstWhere.c_str());
}
HARNESS_MAIN()
