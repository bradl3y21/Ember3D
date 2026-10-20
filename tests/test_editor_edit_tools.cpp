// Edit Mode's first reachable mesh tools: extrude, delete, merge, and undo/redo.
#include "harness.h"

#include "core/Types.h"
#include "editor/EditModeTools.h"
#include "editor/Modal.h"
#include "editor/Panels.h"
#include "render/Camera.h"
#include "scene/Scene.h"

#include <glad/gl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include "platform/Window.h"
#include "render/Renderer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

using ember::ObjectId;
using ember::SelectMode;
using ember::Mat4;
using ember::Vec3;
using ember::length;
using ember::transformPoint;
using ember::u32;
using ember::editor::EditMeshOperation;

struct Rig {
    ImGuiContext* context = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::editor::ModalTransform modal;
    ObjectId object;
    ember::render::OrbitCamera camera;
    ember::Rect viewport{100, 50, 600, 500};

    Rig() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        object = world.createPrimitive(ember::scene::Primitive::Cube, "Cube");
        world.select(object);
        world.editable(object);  // the actual Tab-to-Edit transition lazily creates this
    }

    ~Rig() { ImGui::DestroyContext(context); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    bool press(ImGuiKey key, SelectMode mode, bool ctrl = false, bool textInput = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(key, true);
        ImGui::NewFrame();
        io.WantTextInput = textInput;
        const bool handled = ember::editor::handleEditMeshShortcuts(
            io, world, undo, object, mode);
        ImGui::EndFrame();
        io.AddKeyEvent(key, false);
        ImGui::NewFrame();
        ImGui::EndFrame();
        return handled;
    }

    bool modalFrame(float mouseX, float mouseY, ImGuiKey key = ImGuiKey_None,
                    bool keyDown = false, bool ctrl = false, bool shift = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouseX, mouseY);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, keyDown);
        ImGui::NewFrame();
        const bool handled = modal.update(world, undo, camera,
            ember::editor::TransformSpace::Global, ember::InteractionMode::Edit,
            SelectMode::Face);
        ImGui::EndFrame();
        return handled;
    }

    void drawToolPanel(float mouseX = -100.0f, float mouseY = -100.0f,
                       int mouseDown = -1) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouseX, mouseY);
        if (mouseDown >= 0)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouseDown != 0);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(500.0f, 500.0f));
        ImGui::Begin("mesh-tools-test");
        ember::editor::drawEditMeshTools(world, undo, object, SelectMode::Face);
        ImGui::End();
        ImGui::Render();
    }

    void clickEdit(SelectMode mode, float x, float y, bool shift = false,
                   bool xray = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ImGui::NewFrame();
        ember::editor::handleViewportEditPick(world, camera, mode, true, viewport, xray);
        ImGui::EndFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        ImGui::NewFrame();
        ember::editor::handleViewportEditPick(world, camera, mode, true, viewport, xray);
        ImGui::EndFrame();
    }

    ImGuiWindow* toolPanelWindow() const {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
            if (std::string(window->Name) == "mesh-tools-test") return window;
        return nullptr;
    }

    ImGuiID extrudeButtonId() const {
        ImGuiWindow* window = toolPanelWindow();
        if (!window) return 0;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiWindow* previous = g.CurrentWindow;
        g.CurrentWindow = window;
        ImGui::PushID("MeshTools");
        const ImGuiID id = ImGui::GetID("Extrude Face##extrude");
        ImGui::PopID();
        g.CurrentWindow = previous;
        return id;
    }

    bool findExtrudeButton(float& x, float& y) {
        ImGuiWindow* window = toolPanelWindow();
        const ImGuiID id = extrudeButtonId();
        if (!window || id == 0) return false;
        const float targetX = window->InnerRect.Min.x + 20.0f;
        for (float py = window->InnerRect.Min.y; py < window->InnerRect.Max.y; py += 1.0f) {
            drawToolPanel(targetX, py + 0.5f);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredWindow == window && g.HoveredId == id) {
                x = targetX;
                y = py + 0.5f;
                return true;
            }
        }
        return false;
    }

    const ember::scene::EditableMesh* mesh() const { return world.readableEdit(object); }
};

} // namespace

TEST(edit_mode_mesh_keys_extrude_face_and_undo_redo_the_topology) {
    Rig rig;
    const auto* beforeMesh = rig.mesh();
    CHECK_MSG(beforeMesh != nullptr, "setup did not create the editable mesh");
    if (!beforeMesh) return;
    const std::size_t facesBefore = beforeMesh->faceCount();
    const ember::scene::Face face = beforeMesh->faces.front();
    rig.world.editSetSelection(rig.object, SelectMode::Face, {0});

    CHECK_MSG(ember::editor::canPerformEditMeshOperation(
                  rig.world, rig.object, SelectMode::Face, EditMeshOperation::ExtrudeFace),
              "a single selected face should enable Extrude Face");
    const Mat4 model = rig.world.worldMatrix(rig.object);
    Vec3 center{};
    for (u32 i = 0; i < face.loopCount; ++i) {
        const u32 vertex = beforeMesh->loops[face.firstLoop + i].vertex;
        center += transformPoint(model, beforeMesh->positions[vertex]);
    }
    center = center * (1.0f / static_cast<float>(face.loopCount));
    const Vec3 p0 = transformPoint(model, beforeMesh->positions[
        beforeMesh->loops[face.firstLoop].vertex]);
    const Vec3 p1 = transformPoint(model, beforeMesh->positions[
        beforeMesh->loops[face.firstLoop + 1].vertex]);
    const Vec3 p2 = transformPoint(model, beforeMesh->positions[
        beforeMesh->loops[face.firstLoop + 2].vertex]);
    const Vec3 normal = ember::normalize(ember::cross(p1 - p0, p2 - p0));
    float sx = 0.0f, sy = 0.0f, tx = 0.0f, ty = 0.0f;
    CHECK_MSG(rig.camera.projectScreen(center, 800, 600, sx, sy),
              "selected face center did not project for the E drag test");
    CHECK_MSG(rig.camera.projectScreen(center + normal * 0.5f, 800, 600, tx, ty),
              "drag target did not project for the E drag test");
    CHECK_MSG(rig.modalFrame(sx, sy, ImGuiKey_E, true),
              "E did not start the Edit Mode extrusion modal");
    rig.modalFrame(sx, sy, ImGuiKey_E, false);
    CHECK_MSG(rig.modal.active() && rig.undo.doneCount() == 0,
              "extrusion should preview without recording undo before confirmation");
    rig.modalFrame(tx, ty);
    const auto* previewMesh = rig.mesh();
    CHECK_MSG(previewMesh && previewMesh->faceCount() == facesBefore + 3,
              "E did not create a live face extrusion preview");
    const ember::scene::MeshSelection* previewSelection =
        rig.world.editReadSelection(rig.object, SelectMode::Face);
    if (previewMesh && previewSelection && !previewSelection->indices.empty() &&
        previewSelection->indices.back() < previewMesh->faces.size()) {
        const ember::scene::Face& previewFace = previewMesh->faces[previewSelection->indices.back()];
        Vec3 previewCenter{};
        for (u32 i = 0; i < previewFace.loopCount; ++i) {
            const u32 vertex = previewMesh->loops[previewFace.firstLoop + i].vertex;
            previewCenter += transformPoint(model, previewMesh->positions[vertex]);
        }
        previewCenter = previewCenter * (1.0f / static_cast<float>(previewFace.loopCount));
        CHECK_MSG(ember::dot(previewCenter - center, normal) > 0.3f,
                  "mouse movement did not move the extruded cap along its normal");
    } else {
        CHECK_MSG(false, "live extrusion cap was not selected during the drag preview");
    }
    CHECK_MSG(rig.modalFrame(tx, ty, ImGuiKey_Enter, true),
              "Enter did not confirm the face extrusion modal");
    rig.modalFrame(tx, ty, ImGuiKey_Enter, false);
    const auto* afterMesh = rig.mesh();
    CHECK_MSG(afterMesh && afterMesh->faceCount() == facesBefore + 3,
              "E did not add the three wall faces for the selected cube triangle");
    const ember::scene::MeshSelection* extruded =
        rig.world.editReadSelection(rig.object, SelectMode::Face);
    CHECK_MSG(extruded && extruded->indices.size() == 1 &&
                  afterMesh && extruded->indices.front() == afterMesh->faceCount() - 1,
              "the newly extruded face was not left selected");
    CHECK_MSG(rig.undo.canUndo() && std::string(rig.undo.undoLabel()) == "Extrude Face",
              "Extrude Face did not create one undo entry");

    CHECK_MSG(rig.undo.undo(), "Extrude Face did not undo");
    const auto* undoneMesh = rig.mesh();
    CHECK_MSG(undoneMesh && undoneMesh->faceCount() == facesBefore,
              "undo did not restore the original face count");
    const ember::scene::MeshSelection* restored =
        rig.world.editReadSelection(rig.object, SelectMode::Face);
    CHECK_MSG(restored && restored->indices == std::vector<u32>{u32{0}},
              "undo did not restore the selected face");
    CHECK_MSG(rig.undo.redo(), "Extrude Face did not redo");
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() == facesBefore + 3,
              "redo did not restore the extrusion");

    std::printf("       E extruded a %u-vertex face: %zu -> %u faces; undo/redo restored topology and selection\n",
                face.loopCount, facesBefore, rig.mesh()->faceCount());
}

TEST(edit_mode_extrude_drag_cancel_restores_exact_topology_without_undo) {
    Rig rig;
    const auto* mesh = rig.mesh();
    CHECK_MSG(mesh && !mesh->faces.empty(), "setup did not create an editable mesh");
    if (!mesh || mesh->faces.empty()) return;
    const std::vector<Vec3> positionsBefore = mesh->positions;
    const std::size_t facesBefore = mesh->faceCount();
    rig.world.editSetSelection(rig.object, SelectMode::Face, {0});
    const ember::scene::Face face = mesh->faces[0];
    Vec3 center{};
    for (u32 i = 0; i < face.loopCount; ++i)
        center += mesh->positions[mesh->loops[face.firstLoop + i].vertex];
    center = center * (1.0f / static_cast<float>(face.loopCount));
    const Vec3 p0 = mesh->positions[mesh->loops[face.firstLoop].vertex];
    const Vec3 p1 = mesh->positions[mesh->loops[face.firstLoop + 1].vertex];
    const Vec3 p2 = mesh->positions[mesh->loops[face.firstLoop + 2].vertex];
    const Vec3 normal = ember::normalize(ember::cross(p1 - p0, p2 - p0));
    float sx = 0.0f, sy = 0.0f, tx = 0.0f, ty = 0.0f;
    CHECK_MSG(rig.camera.projectScreen(center, 800, 600, sx, sy),
              "selected face center did not project for extrusion cancellation");
    CHECK_MSG(rig.camera.projectScreen(center + normal * 0.75f, 800, 600, tx, ty),
              "extrusion cancel target did not project");
    rig.modalFrame(sx, sy, ImGuiKey_E, true);
    rig.modalFrame(sx, sy, ImGuiKey_E, false);
    rig.modalFrame(tx, ty);
    CHECK_MSG(rig.modalFrame(tx, ty, ImGuiKey_Escape, true),
              "Escape did not cancel the face extrusion modal");
    rig.modalFrame(tx, ty, ImGuiKey_Escape, false);
    const auto* restored = rig.mesh();
    CHECK_MSG(!rig.modal.active(), "cancel left the extrusion modal active");
    CHECK_MSG(restored && restored->faceCount() == facesBefore &&
                  restored->positions == positionsBefore,
              "cancel did not restore the exact original editable topology");
    const ember::scene::MeshSelection* restoredSelection =
        rig.world.editReadSelection(rig.object, SelectMode::Face);
    CHECK_MSG(restoredSelection && restoredSelection->indices == std::vector<u32>{u32{0}},
              "cancel did not restore the originally selected face");
    CHECK_MSG(rig.undo.doneCount() == 0,
              "cancelled extrusion incorrectly added an undo entry");
}

TEST(edit_mode_x_deletes_the_selected_elements_and_is_reversible) {
    Rig rig;
    const auto* beforeMesh = rig.mesh();
    CHECK_MSG(beforeMesh != nullptr, "setup did not create the editable mesh");
    if (!beforeMesh) return;
    const std::size_t facesBefore = beforeMesh->faceCount();
    rig.world.editSetSelection(rig.object, SelectMode::Face, {0});
    CHECK_MSG(rig.press(ImGuiKey_X, SelectMode::Face), "X was not consumed in Edit Mode");
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() < facesBefore,
              "X did not delete the selected face");
    CHECK_MSG(rig.undo.canUndo() && std::string(rig.undo.undoLabel()) == "Delete Mesh Selection",
              "mesh deletion did not create one undo entry");
    CHECK_MSG(rig.undo.undo(), "mesh deletion did not undo");
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() == facesBefore,
              "undo did not restore deleted geometry");
    CHECK_MSG(rig.undo.redo(), "mesh deletion did not redo");
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() < facesBefore,
              "redo did not delete the selected face again");
}

TEST(edit_mode_m_merges_selected_vertices_at_their_center) {
    Rig rig;
    const auto* beforeMesh = rig.mesh();
    CHECK_MSG(beforeMesh && beforeMesh->positions.size() >= 2,
              "setup cube needs at least two editable vertices");
    if (!beforeMesh || beforeMesh->positions.size() < 2) return;
    const std::size_t verticesBefore = beforeMesh->positions.size();
    rig.world.editSetSelection(rig.object, SelectMode::Vertex, {0, 1});
    CHECK_MSG(ember::editor::canPerformEditMeshOperation(
                  rig.world, rig.object, SelectMode::Vertex, EditMeshOperation::MergeAtCenter),
              "two selected vertices should enable Merge at Center");
    CHECK_MSG(rig.press(ImGuiKey_M, SelectMode::Vertex), "M was not consumed in Edit Mode");
    CHECK_MSG(rig.mesh() && rig.mesh()->positions.size() < verticesBefore,
              "M did not merge the selected vertices");
    CHECK_MSG(rig.undo.canUndo() && std::string(rig.undo.undoLabel()) == "Merge at Center",
              "Merge at Center did not create one undo entry");
    CHECK_MSG(rig.undo.undo(), "Merge at Center did not undo");
    CHECK_MSG(rig.mesh() && rig.mesh()->positions.size() == verticesBefore,
              "undo did not restore the merged vertices");
}

TEST(edit_mode_mesh_shortcuts_do_not_consume_text_or_modified_input) {
    Rig rig;
    const auto* mesh = rig.mesh();
    CHECK_MSG(mesh != nullptr, "setup did not create the editable mesh");
    if (!mesh) return;
    const std::size_t facesBefore = mesh->faceCount();
    rig.world.editSetSelection(rig.object, SelectMode::Face, {0});
    CHECK_MSG(!rig.press(ImGuiKey_E, SelectMode::Face, false, true),
              "E was consumed while text input owned the keyboard");
    CHECK_MSG(!rig.press(ImGuiKey_E, SelectMode::Face, true, false),
              "Ctrl+E was consumed by the Edit Mode extrude shortcut");
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() == facesBefore,
              "a guarded shortcut modified the mesh");
}

TEST(edit_mode_tool_panel_exposes_a_clickable_extrude_action) {
    Rig rig;
    const auto* mesh = rig.mesh();
    CHECK_MSG(mesh != nullptr && mesh->faceCount() > 0,
              "setup did not create an editable mesh with faces");
    if (!mesh || mesh->faceCount() == 0) return;
    const std::size_t facesBefore = mesh->faceCount();
    rig.world.editSetSelection(rig.object, SelectMode::Face, {0});
    CHECK_MSG(ember::editor::canPerformEditMeshOperation(
                  rig.world, rig.object, SelectMode::Face,
                  EditMeshOperation::ExtrudeFace),
              "the selected face should enable the actual Tool tab action");
    rig.drawToolPanel();

    float x = 0.0f, y = 0.0f;
    CHECK_MSG(rig.findExtrudeButton(x, y),
              "the Tool tab's Extrude Face button was not hit-testable");
    if (x == 0.0f && y == 0.0f) return;
    rig.drawToolPanel(x, y);
    rig.drawToolPanel(x, y, 1);
    rig.drawToolPanel(x, y, 0);
    CHECK_MSG(rig.mesh() && rig.mesh()->faceCount() == facesBefore + 3,
              "clicking the Tool tab's Extrude Face button did not mutate the selected geometry");
    CHECK_MSG(rig.undo.canUndo() && std::string(rig.undo.undoLabel()) == "Extrude Face",
              "the Tool tab extrusion did not create an undo entry");
}

TEST(edit_mode_viewport_clicks_select_visible_vertices_edges_and_faces) {
    Rig rig;
    const auto* mesh = rig.mesh();
    CHECK_MSG(mesh != nullptr, "setup did not create an editable mesh");
    if (!mesh) return;
    const Mat4 model = rig.world.worldMatrix(rig.object);

    // Vertex: pick a front-facing cube corner using its actual projected pixel.
    u32 targetVertex = static_cast<u32>(mesh->positions.size());
    for (u32 i = 0; i < mesh->positions.size(); ++i)
        if (mesh->positions[i].z > 0.49f) { targetVertex = i; break; }
    CHECK_MSG(targetVertex < mesh->positions.size(), "setup cube has no front vertex");
    if (targetVertex >= mesh->positions.size()) return;
    const Vec3 vertexWorld = transformPoint(model, mesh->positions[targetVertex]);
    float sx = 0.0f, sy = 0.0f;
    CHECK_MSG(rig.camera.projectScreen(vertexWorld, rig.viewport.w, rig.viewport.h, sx, sy),
              "front vertex did not project into the viewport");
    rig.clickEdit(SelectMode::Vertex, rig.viewport.x + std::round(sx),
                  rig.viewport.y + std::round(sy));
    const ember::scene::MeshSelection* vertexSelection =
        rig.world.editReadSelection(rig.object, SelectMode::Vertex);
    CHECK_MSG(vertexSelection && vertexSelection->indices.size() == 1,
              "clicking the visible vertex did not select a vertex");
    if (vertexSelection && !vertexSelection->indices.empty()) {
        const Vec3 selectedPosition = rig.mesh()->positions[vertexSelection->indices.front()];
        CHECK_MSG(length(selectedPosition - mesh->positions[targetVertex]) < 1e-4f,
                  "vertex click did not select the projected world vertex");
    }

    // Edge: target a front face boundary edge midpoint, away from its vertices.
    const auto* edgeMesh = rig.mesh();
    u32 targetEdge = static_cast<u32>(edgeMesh->edges.size());
    for (u32 i = 0; i < edgeMesh->edges.size(); ++i) {
        const auto& edge = edgeMesh->edges[i];
        const Vec3& a = edgeMesh->positions[edge.v0];
        const Vec3& b = edgeMesh->positions[edge.v1];
        if (a.z > 0.49f && b.z > 0.49f &&
            (std::fabs(a.x - b.x) < 1e-4f || std::fabs(a.y - b.y) < 1e-4f)) {
            targetEdge = i;
            break;
        }
    }
    CHECK_MSG(targetEdge < edgeMesh->edges.size(), "setup cube has no front boundary edge");
    if (targetEdge >= edgeMesh->edges.size()) return;
    const auto edge = edgeMesh->edges[targetEdge];
    const Vec3 midpoint = (edgeMesh->positions[edge.v0] + edgeMesh->positions[edge.v1]) * 0.5f;
    const Vec3 edgeWorld = transformPoint(model, midpoint);
    CHECK_MSG(rig.camera.projectScreen(edgeWorld, rig.viewport.w, rig.viewport.h, sx, sy),
              "front edge did not project into the viewport");
    rig.clickEdit(SelectMode::Edge, rig.viewport.x + std::round(sx),
                  rig.viewport.y + std::round(sy));
    const ember::scene::MeshSelection* edgeSelection =
        rig.world.editReadSelection(rig.object, SelectMode::Edge);
    CHECK_MSG(edgeSelection && edgeSelection->indices.size() == 1,
              "clicking the visible edge did not select an edge");

    // Face: click at the centroid of a front-facing polygon.
    const auto* faceMesh = rig.mesh();
    u32 targetFace = static_cast<u32>(faceMesh->faces.size());
    Vec3 faceCenter{};
    for (u32 i = 0; i < faceMesh->faces.size(); ++i) {
        const auto& face = faceMesh->faces[i];
        bool front = true;
        Vec3 center{};
        for (u32 j = 0; j < face.loopCount; ++j) {
            const u32 vertex = faceMesh->loops[face.firstLoop + j].vertex;
            front = front && faceMesh->positions[vertex].z > 0.49f;
            center += faceMesh->positions[vertex];
        }
        if (front) {
            targetFace = i;
            faceCenter = center * (1.0f / static_cast<float>(face.loopCount));
            break;
        }
    }
    CHECK_MSG(targetFace < faceMesh->faces.size(), "setup cube has no front face");
    if (targetFace >= faceMesh->faces.size()) return;
    const Vec3 faceWorld = transformPoint(model, faceCenter);
    CHECK_MSG(rig.camera.projectScreen(faceWorld, rig.viewport.w, rig.viewport.h, sx, sy),
              "front face center did not project into the viewport");
    rig.clickEdit(SelectMode::Face, rig.viewport.x + std::round(sx),
                  rig.viewport.y + std::round(sy));
    const ember::scene::MeshSelection* faceSelection =
        rig.world.editReadSelection(rig.object, SelectMode::Face);
    CHECK_MSG(faceSelection && faceSelection->indices.size() == 1 &&
                  faceSelection->indices.front() == targetFace,
              "clicking the front face did not select the polygon under the pointer");

    const ember::EditOverlayState overlay =
        ember::editor::buildEditOverlayState(rig.world, rig.object, SelectMode::Face);
    CHECK_MSG(overlay.valid() && overlay.vertexPoints.size() == rig.mesh()->positions.size() &&
                  overlay.edgeLines.size() == rig.mesh()->edges.size() * 2,
              "Edit Mode did not build render-ready vertex/edge overlay data");
    CHECK_MSG(targetFace < overlay.faceSelected.size() && overlay.faceSelected[targetFace],
              "the selected polygon was absent from EditOverlayState");
}

TEST_GPU(edit_mode_render_overlay_changes_viewport_pixels) {
    struct GpuRig {
        ember::platform::Window window;
        ember::render::Renderer renderer;
        ember::scene::World world;
        ObjectId object;
        bool ready = false;
        std::string why;

        GpuRig() {
            ember::platform::WindowConfig config;
            config.width = 800;
            config.height = 600;
            config.visible = false;
            config.title = "Edit Mode overlay test";
            if (!window.create(config)) { why = "could not create hidden GL window"; return; }
            if (!renderer.init()) { why = "renderer initialization failed"; return; }
            renderer.resize(800, 600);
            ember::ViewportSettings settings;
            settings.showGrid = false;
            settings.showAxes = false;
            settings.showOrigins = false;
            settings.showSelectionOutline = false;
            settings.showGizmo = false;
            renderer.setViewportSettings(settings);
            object = world.createPrimitive(ember::scene::Primitive::Cube, "Cube");
            world.select(object);
            world.editable(object);
            const auto* mesh = world.readableEdit(object);
            if (mesh) {
                for (u32 face = 0; face < mesh->faces.size(); ++face) {
                    const auto& f = mesh->faces[face];
                    bool front = f.loopCount >= 3;
                    for (u32 i = 0; i < f.loopCount && front; ++i) {
                        const u32 vertex = mesh->loops[f.firstLoop + i].vertex;
                        front = mesh->positions[vertex].z > 0.49f;
                    }
                    if (front) {
                        world.editSetSelection(object, SelectMode::Face, {face});
                        ready = true;
                        return;
                    }
                }
                why = "no front face could be selected";
            } else {
                why = "editable mesh unavailable";
            }
        }

        ~GpuRig() { renderer.shutdown(); }
        GpuRig(const GpuRig&) = delete;
        GpuRig& operator=(const GpuRig&) = delete;

        void render(const ember::EditOverlayState& overlay, std::vector<ember::u8>& pixels) {
            ember::RenderPacket packet = world.buildPacket(800, 600);
            packet.width = 800;
            packet.height = 600;
            packet.view = renderer.camera().view();
            packet.projection = renderer.camera().projection(4.0f / 3.0f);
            packet.cameraPosition = renderer.camera().position();
            packet.editOverlay = overlay;
            renderer.renderFrame(packet);
            renderer.readViewport(0, 0, 800, 600, pixels);
        }
    } rig;

    CHECK_MSG(rig.ready, "overlay test setup: %s", rig.why.c_str());
    if (!rig.ready) return;
    const ember::EditOverlayState overlay =
        ember::editor::buildEditOverlayState(rig.world, rig.object, SelectMode::Face);
    CHECK_MSG(overlay.valid() && !overlay.edgeLines.empty(),
              "Edit Mode overlay extraction returned no drawable edges");

    std::vector<ember::u8> withoutOverlay, withOverlay;
    rig.render({}, withoutOverlay);
    rig.render(overlay, withOverlay);
    CHECK_MSG(withoutOverlay.size() == withOverlay.size() && !withOverlay.empty(),
              "could not read matching rendered viewport images");
    if (withoutOverlay.size() != withOverlay.size() || withOverlay.empty()) return;

    int changedPixels = 0;
    int selectedOrangePixels = 0;
    for (std::size_t i = 0; i + 3 < withOverlay.size(); i += 4) {
        const int delta = std::abs(int(withOverlay[i]) - int(withoutOverlay[i])) +
                          std::abs(int(withOverlay[i + 1]) - int(withoutOverlay[i + 1])) +
                          std::abs(int(withOverlay[i + 2]) - int(withoutOverlay[i + 2]));
        if (delta > 8) ++changedPixels;
        if (withOverlay[i] > withOverlay[i + 1] * 1.5f &&
            withOverlay[i + 1] > withOverlay[i + 2] * 1.5f)
            ++selectedOrangePixels;
    }
    std::printf("       Edit Mode overlay changed %d px; selected-orange pixels %d\n",
                changedPixels, selectedOrangePixels);
    CHECK_MSG(changedPixels > 100, "Edit Mode vertices/edges were not drawn over the base mesh");
    CHECK_MSG(selectedOrangePixels > 0, "selected face outline was not rendered in the selection color");
}

HARNESS_MAIN()
