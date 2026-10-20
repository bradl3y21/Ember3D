#include "editor/EditModeTools.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/Theme.h"
#include "editor/Commands.h"

namespace ember::editor {

namespace {

float gExtrudeDistance = 1.0f;

std::vector<u32> selectedVertices(const scene::World& world, ObjectId object,
                                  SelectMode mode) {
    std::vector<u32> vertices;
    const scene::MeshSelection* selection = world.editReadSelection(object, mode);
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!selection || !mesh) return vertices;

    const auto add = [&](u32 vertex) {
        if (vertex < mesh->positions.size() &&
            std::find(vertices.begin(), vertices.end(), vertex) == vertices.end())
            vertices.push_back(vertex);
    };
    for (u32 index : selection->indices) {
        if (mode == SelectMode::Vertex) {
            add(index);
        } else if (mode == SelectMode::Edge) {
            if (index < mesh->edges.size()) {
                add(mesh->edges[index].v0);
                add(mesh->edges[index].v1);
            }
        } else if (index < mesh->faces.size()) {
            const scene::Face& face = mesh->faces[index];
            for (u32 loop = 0; loop < face.loopCount; ++loop) {
                const u32 li = face.firstLoop + loop;
                if (li < mesh->loops.size()) add(mesh->loops[li].vertex);
            }
        }
    }
    return vertices;
}

bool selectedFaceNormalWorld(const scene::World& world, ObjectId object,
                             u32 faceIndex, Vec3& outNormal) {
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!mesh || faceIndex >= mesh->faces.size()) return false;
    const scene::Face& face = mesh->faces[faceIndex];
    if (face.loopCount < 3) return false;
    const Mat4 model = world.worldMatrix(object);
    const u32 firstLoop = face.firstLoop;
    if (firstLoop >= mesh->loops.size()) return false;
    const Vec3 p0 = transformPoint(model, mesh->positions[mesh->loops[firstLoop].vertex]);
    for (u32 i = 1; i + 1 < face.loopCount; ++i) {
        const u32 li1 = firstLoop + i;
        const u32 li2 = firstLoop + i + 1;
        if (li2 >= mesh->loops.size()) return false;
        const Vec3 p1 = transformPoint(model, mesh->positions[mesh->loops[li1].vertex]);
        const Vec3 p2 = transformPoint(model, mesh->positions[mesh->loops[li2].vertex]);
        const Vec3 normal = cross(p1 - p0, p2 - p0);
        if (dot(normal, normal) > 1e-12f) {
            outNormal = normalize(normal);
            return true;
        }
    }
    return false;
}

std::string operationLabel(EditMeshOperation operation) {
    switch (operation) {
        case EditMeshOperation::ExtrudeFace: return "Extrude Face";
        case EditMeshOperation::DeleteSelection: return "Delete Mesh Selection";
        case EditMeshOperation::MergeAtCenter: return "Merge at Center";
    }
    return "Edit Mesh";
}

} // namespace

bool canPerformEditMeshOperation(const scene::World& world, ObjectId object,
                                 SelectMode mode, EditMeshOperation operation) {
    if (!world.get(object) || !world.readableEdit(object)) return false;
    const scene::MeshSelection* selection = world.editReadSelection(object, mode);
    if (!selection || selection->indices.empty()) return false;

    switch (operation) {
        case EditMeshOperation::ExtrudeFace:
            return mode == SelectMode::Face && selection->indices.size() == 1;
        case EditMeshOperation::DeleteSelection:
            return true;
        case EditMeshOperation::MergeAtCenter:
            return selectedVertices(world, object, mode).size() >= 2;
    }
    return false;
}

float& editExtrudeDistance() { return gExtrudeDistance; }

bool performEditMeshOperation(scene::World& world, UndoStack& undo, ObjectId object,
                              SelectMode mode, EditMeshOperation operation,
                              float extrudeDistance) {
    if (!canPerformEditMeshOperation(world, object, mode, operation)) return false;
    const scene::MeshSelection* selection = world.editReadSelection(object, mode);
    if (!selection) return false;

    const std::vector<u32> editSelectionBefore = selection->indices;
    const std::vector<ObjectId> objectSelectionBefore = world.selection();
    const std::optional<scene::ObjectSnapshot> before = world.captureSnapshot(object);
    if (!before) return false;

    bool changed = false;
    std::vector<u32> editSelectionAfter;
    switch (operation) {
        case EditMeshOperation::ExtrudeFace: {
            const u32 face = selection->indices.front();
            Vec3 normal{};
            if (!std::isfinite(extrudeDistance) || extrudeDistance <= 0.0f)
                return false;
            changed = selectedFaceNormalWorld(world, object, face, normal) &&
                      world.editExtrudeFace(object, face, normal * extrudeDistance);
            if (changed) {
                const scene::EditableMesh* mesh = world.readableEdit(object);
                if (mesh && !mesh->faces.empty()) {
                    const u32 newFace = static_cast<u32>(mesh->faces.size() - 1);
                    world.editSetSelection(object, SelectMode::Face, {newFace});
                    editSelectionAfter = {newFace};
                }
            }
            break;
        }
        case EditMeshOperation::DeleteSelection:
            changed = world.editDeleteSubElements(object, mode, selection->indices);
            break;
        case EditMeshOperation::MergeAtCenter: {
            const std::vector<u32> vertices = selectedVertices(world, object, mode);
            const scene::EditableMesh* mesh = world.readableEdit(object);
            if (mesh && vertices.size() >= 2) {
                const Mat4 model = world.worldMatrix(object);
                Vec3 center{};
                for (u32 vertex : vertices)
                    center += transformPoint(model, mesh->positions[vertex]);
                center = center * (1.0f / static_cast<float>(vertices.size()));
                changed = world.editMergeVertices(object, vertices, center);
                if (changed) {
                    const scene::EditableMesh* merged = world.readableEdit(object);
                    if (merged && !merged->positions.empty()) {
                        const Vec3 localCenter = transformPoint(inverse(model), center);
                        u32 nearest = 0;
                        float nearestDistance = dot(merged->positions[0] - localCenter,
                                                    merged->positions[0] - localCenter);
                        for (u32 i = 1; i < merged->positions.size(); ++i) {
                            const Vec3 delta = merged->positions[i] - localCenter;
                            const float distance = dot(delta, delta);
                            if (distance < nearestDistance) {
                                nearest = i;
                                nearestDistance = distance;
                            }
                        }
                        world.editSetSelection(object, SelectMode::Vertex, {nearest});
                        editSelectionAfter = {nearest};
                    }
                }
            }
            break;
        }
    }
    if (!changed) return false;

    const std::optional<scene::ObjectSnapshot> after = world.captureSnapshot(object);
    if (!after) {
        // A successful mutator must leave its original object snapshot-able.
        world.destroy(object);
        world.reinsert(*before);
        world.clearSelection();
        for (ObjectId selected : objectSelectionBefore) world.select(selected, true);
        world.editSetSelection(object, mode, editSelectionBefore);
        return false;
    }
    undo.push(std::make_unique<EditTopologyCommand>(
        world, object, *before, *after, objectSelectionBefore, world.selection(),
        mode, editSelectionBefore, editSelectionAfter, operationLabel(operation)));
    return true;
}

EditOverlayState buildEditOverlayState(const scene::World& world, ObjectId object,
                                      SelectMode mode, float framebufferScale) {
    EditOverlayState state;
    const scene::Object* source = world.get(object);
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!source || !source->hasMesh || !mesh) return state;

    state.mesh = &source->mesh;
    state.model = world.worldMatrix(object);
    state.mode = InteractionMode::Edit;
    state.selectMode = mode;
    state.vertexPoints = mesh->positions;
    state.vertexSelected.assign(mesh->positions.size(), 0);
    state.edgeLines.reserve(mesh->edges.size() * 2);
    state.edgeSelected.assign(mesh->edges.size(), 0);
    state.faceSelected.assign(mesh->faces.size(), 0);

    if (mode == SelectMode::Vertex) {
        const scene::MeshSelection* selected = world.editReadSelection(object, SelectMode::Vertex);
        if (selected) {
            for (u32 vertex : selected->indices)
                if (vertex < state.vertexSelected.size()) state.vertexSelected[vertex] = 1;
            if (!selected->indices.empty())
                state.activeVertex = static_cast<int>(selected->indices.back());
        }
    }
    if (mode == SelectMode::Edge) {
        const scene::MeshSelection* selected = world.editReadSelection(object, SelectMode::Edge);
        if (selected) {
            for (u32 edge : selected->indices)
                if (edge < state.edgeSelected.size()) state.edgeSelected[edge] = 1;
            if (!selected->indices.empty()) state.activeEdge = static_cast<int>(selected->indices.back());
        }
    }
    if (mode == SelectMode::Face) {
        const scene::MeshSelection* selected = world.editReadSelection(object, SelectMode::Face);
        if (selected) {
            for (u32 face : selected->indices) {
                if (face >= state.faceSelected.size()) continue;
                state.faceSelected[face] = 1;
                const scene::Face& f = mesh->faces[face];
                for (u32 i = 0; i < f.loopCount; ++i) {
                    const u32 li = f.firstLoop + i;
                    if (li < mesh->loops.size() && mesh->loops[li].edge < state.edgeSelected.size())
                        state.edgeSelected[mesh->loops[li].edge] = 1;
                }
            }
            if (!selected->indices.empty()) state.activeFace = static_cast<int>(selected->indices.back());
        }
    }

    for (const scene::Edge& edge : mesh->edges) {
        if (edge.v0 >= mesh->positions.size() || edge.v1 >= mesh->positions.size()) continue;
        state.edgeLines.push_back(mesh->positions[edge.v0]);
        state.edgeLines.push_back(mesh->positions[edge.v1]);
    }
    std::size_t triangle = 0;
    for (u32 faceIndex = 0; faceIndex < mesh->faces.size(); ++faceIndex) {
        const scene::Face& face = mesh->faces[faceIndex];
        if (face.loopCount < 3 || face.firstLoop + face.loopCount > mesh->loops.size()) continue;
        // syncMeshFromEdit emits triangles face-by-face in EditableMesh order,
        // and ear-clips concave rings. Reuse that mapping instead of inventing
        // a fan that could shade outside a concave face.
        const std::size_t faceTriangleCount = face.loopCount - 2;
        for (std::size_t i = 0; i < faceTriangleCount; ++i, ++triangle) {
            const std::size_t indexOffset = triangle * 3;
            if (indexOffset + 2 >= source->mesh.indices.size()) break;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const u32 vertex = source->mesh.indices[indexOffset + corner];
                if (static_cast<std::size_t>(vertex) * 3 + 2 >= source->mesh.positions.size())
                    continue;
                state.faceTriangles.push_back(Vec3{
                    source->mesh.positions[static_cast<std::size_t>(vertex) * 3],
                    source->mesh.positions[static_cast<std::size_t>(vertex) * 3 + 1],
                    source->mesh.positions[static_cast<std::size_t>(vertex) * 3 + 2]});
            }
            state.faceOfTri.push_back(faceIndex);
        }
    }

    state.selectedColor = ember::kDefaultTheme.selection.linear();
    state.unselectedColor = ember::kDefaultTheme.textDisabled.linear();
    state.activeColor = ember::kDefaultTheme.active.linear();
    const float scale = std::max(0.01f, framebufferScale);
    state.vertexRadiusPixels = ember::kEditVertexRadiusPixels * scale;
    state.edgeWidthPixels = ember::kEditEdgeWidthPixels * scale;
    return state;
}

void drawEditMeshTools(scene::World& world, UndoStack& undo, ObjectId object,
                       SelectMode mode) {
    ImGui::PushID("MeshTools");
    if (ImGui::CollapsingHeader("Mesh Editing", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("E Extrude  |  X Delete  |  M Merge at Center");
        const bool canExtrude = canPerformEditMeshOperation(
            world, object, mode, EditMeshOperation::ExtrudeFace);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("Distance", &editExtrudeDistance(), 0.01f, 0.01f, 100.0f,
                         "%.2f units", ImGuiSliderFlags_AlwaysClamp);
        ImGui::BeginDisabled(!canExtrude);
        if (ImGui::Button("Extrude Face##extrude"))
            performEditMeshOperation(world, undo, object, mode,
                                     EditMeshOperation::ExtrudeFace,
                                     editExtrudeDistance());
        ImGui::EndDisabled();

        const bool canDelete = canPerformEditMeshOperation(
            world, object, mode, EditMeshOperation::DeleteSelection);
        ImGui::BeginDisabled(!canDelete);
        if (ImGui::Button("Delete Selected Geometry##delete"))
            performEditMeshOperation(world, undo, object, mode,
                                     EditMeshOperation::DeleteSelection);
        ImGui::EndDisabled();

        const bool canMerge = canPerformEditMeshOperation(
            world, object, mode, EditMeshOperation::MergeAtCenter);
        ImGui::BeginDisabled(!canMerge);
        if (ImGui::Button("Merge at Center##merge"))
            performEditMeshOperation(world, undo, object, mode,
                                     EditMeshOperation::MergeAtCenter);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Extrusion keeps the face selected; use G to move it further.");
    }
    ImGui::PopID();
}

bool handleEditMeshShortcuts(const ImGuiIO& io, scene::World& world, UndoStack& undo,
                             ObjectId object, SelectMode mode) {
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
        // ModalTransform gets first refusal and owns the actual mouse-driven
        // extrusion. Consume E here when there is no valid face modal to start,
        // rather than falling back to the old immediate-distance behavior.
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X) || ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        (void)performEditMeshOperation(world, undo, object, mode,
                                       EditMeshOperation::DeleteSelection);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M)) {
        (void)performEditMeshOperation(world, undo, object, mode,
                                       EditMeshOperation::MergeAtCenter);
        return true;
    }
    return false;
}

} // namespace ember::editor
