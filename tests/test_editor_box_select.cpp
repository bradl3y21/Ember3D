// Object Mode's empty-space drag selection uses viewport-local framebuffer
// coordinates and does not create undo entries. This is a headless ImGui test.
#include "harness.h"

#include "editor/Panels.h"
#include "editor/Undo.h"
#include "render/Camera.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cmath>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

struct Rig {
    ImGuiContext* context = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::render::OrbitCamera camera;
    // Deliberately non-zero origin and 2x DPI: a full-window ray or a missing
    // scale conversion selects the wrong place.
    ember::Rect viewport{200, 100, 1200, 800};
    ember::ObjectId left;
    ember::ObjectId right;

    Rig() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DisplayFramebufferScale = ImVec2(2.0f, 2.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        camera.setView(ember::render::StandardView::Front);
        camera.setOrthographic(true);
        camera.setOrthoHeight(8.0f);
        left = world.createPrimitive(ember::scene::Primitive::Cube, "Left");
        right = world.createPrimitive(ember::scene::Primitive::Cube, "Right");
        ember::Transform leftTransform;
        leftTransform.position.x = -1.0f;
        world.setTransform(left, leftTransform);
        ember::Transform rightTransform;
        rightTransform.position.x = 1.0f;
        world.setTransform(right, rightTransform);
    }

    ~Rig() { ImGui::DestroyContext(context); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void frame(float x, float y, bool ownsPointer = true) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        ImGui::NewFrame();
        ember::editor::handleViewportPick(world, camera, undo, ember::editor::Tool::Select,
                                          ownsPointer, viewport);
        ImGui::EndFrame();
    }

    void click(float x, float y, bool ownsPointer = true) {
        ImGuiIO& io = ImGui::GetIO();
        frame(x, y, ownsPointer);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(x, y, ownsPointer);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(x, y, ownsPointer);
        frame(x, y, ownsPointer);
    }

    void drag(float x0, float y0, float x1, float y1, bool shift = false,
              bool ownsPointer = true) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        frame(x0, y0, ownsPointer);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(x0, y0, ownsPointer);
        frame((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, ownsPointer);
        frame(x1, y1, ownsPointer);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(x1, y1, ownsPointer);
        frame(x1, y1, ownsPointer);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        frame(x1, y1, ownsPointer);
    }

    ImVec2 localToWindow(float x, float y) const {
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        return ImVec2((static_cast<float>(viewport.x) + x) / scale.x,
                      (static_cast<float>(viewport.y) + y) / scale.y);
    }
};

} // namespace

TEST(object_mode_empty_drag_encloses_in_viewport_local_pixels_without_undo) {
    Rig rig;
    const std::size_t undoBefore = rig.undo.doneCount();

    // The left-to-right box contains both projected object bounds. Its press
    // starts in empty space; viewport origin and DPI are intentionally offset.
    const ImVec2 start = rig.localToWindow(150.0f, 150.0f);
    const ImVec2 end = rig.localToWindow(1050.0f, 650.0f);
    rig.drag(start.x, start.y, end.x, end.y);

    CHECK_MSG(rig.world.selection().size() == 2,
              "left-to-right empty-space box should select both enclosed objects (selected %zu)",
              rig.world.selection().size());
    CHECK_MSG(rig.world.selection().size() == 2 &&
                  rig.world.selection()[0] == rig.left && rig.world.selection()[1] == rig.right,
              "box selection should preserve creation order and use the viewport-local rectangle");
    CHECK_MSG(rig.undo.doneCount() == undoBefore,
              "selection must remain outside the transform/object undo history");
}

TEST(object_mode_right_to_left_box_crosses_and_shift_adds_without_ui_leaks) {
    Rig rig;
    rig.world.select(rig.left);
    const ImVec2 start = rig.localToWindow(900.0f, 200.0f);
    const ImVec2 end = rig.localToWindow(700.0f, 600.0f);
    rig.drag(start.x, start.y, end.x, end.y);

    CHECK_MSG(rig.world.selection().size() == 1 && rig.world.selection().back() == rig.right,
              "right-to-left crossing box should replace selection with only the intersected object");

    rig.world.clearSelection();
    rig.world.select(rig.left);
    rig.drag(start.x, start.y, end.x, end.y, true);

    CHECK_MSG(rig.world.selection().size() == 2 &&
                  rig.world.selection()[0] == rig.left && rig.world.selection()[1] == rig.right,
              "right-to-left crossing box with Shift should add only the intersected object");

    rig.world.clearSelection();
    const std::size_t undoBefore = rig.undo.doneCount();
    rig.drag(start.x, start.y, end.x, end.y, false, false); // Toolbar/sidebar/panel owns pointer.
    CHECK_MSG(rig.world.selection().empty(),
              "a drag begun outside the viewport body must not alter scene selection");
    CHECK_MSG(rig.undo.doneCount() == undoBefore,
              "an ignored UI-owned drag must not create an undo entry");
}

TEST(object_mode_crossing_box_does_not_select_an_object_from_bounds_alone) {
    Rig rig;
    const ember::ObjectId triangleId = rig.world.createObject("Triangle");
    ember::scene::Object* triangle = rig.world.get(triangleId);
    CHECK_MSG(triangle != nullptr, "test setup could not create a triangle object");
    if (!triangle) return;

    // In Front orthographic view this triangle spans screen bounds x=300..900,
    // y=100..700. Its lower-right AABB corner is empty; a small right-to-left
    // crossing box there must not select based on the projected AABB alone.
    triangle->hasMesh = true;
    triangle->mesh.positions = {-3.0f,  3.0f, 0.0f,
                                 3.0f,  3.0f, 0.0f,
                                -3.0f, -3.0f, 0.0f};
    triangle->mesh.normals = {0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 1.0f};
    triangle->mesh.indices = {0, 1, 2};
    ++triangle->meshVersion;

    const ImVec2 start = rig.localToWindow(900.0f, 700.0f);
    const ImVec2 end = rig.localToWindow(800.0f, 600.0f);
    rig.drag(start.x, start.y, end.x, end.y);

    CHECK_MSG(rig.world.selection().empty(),
              "crossing select must test projected triangles, not only their bounding box");
}

TEST(object_mode_click_picking_is_preserved_and_shift_click_adds) {
    Rig rig;
    float sx = 0.0f, sy = 0.0f;
    CHECK_MSG(rig.camera.projectScreen(ember::Vec3{1.0f, 0.0f, 0.0f},
                                       rig.viewport.w, rig.viewport.h, sx, sy),
              "test setup: right object did not project into the viewport");
    const ImVec2 point = rig.localToWindow(sx, sy);
    rig.click(point.x, point.y);
    CHECK_MSG(rig.world.selection().size() == 1 && rig.world.selection().back() == rig.right,
              "viewport-local click should still pick the object under the cursor");

    float lx = 0.0f, ly = 0.0f;
    rig.camera.projectScreen(ember::Vec3{-1.0f, 0.0f, 0.0f},
                              rig.viewport.w, rig.viewport.h, lx, ly);
    const ImVec2 leftPoint = rig.localToWindow(lx, ly);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, true);
    rig.click(leftPoint.x, leftPoint.y);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    rig.frame(leftPoint.x, leftPoint.y);
    CHECK_MSG(rig.world.selection().size() == 2,
              "Shift-click should preserve existing click-add selection behavior");
}

HARNESS_MAIN()
