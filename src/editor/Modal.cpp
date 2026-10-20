#include "editor/Modal.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

#include "editor/Commands.h"
#include "core/Log.h"

namespace ember::editor {

namespace {

constexpr float kMinScale = 0.01f;
constexpr float kPrecision = 0.1f;          // Shift: a tenth of the mouse movement

struct Px {
    float x = 0.0f;
    float y = 0.0f;
};

// Framebuffer size for this frame (D16/D17: window points x DisplayFramebufferScale).
bool framebuffer(const ImGuiIO& io, int& width, int& height) {
    width = static_cast<int>(std::lround(io.DisplaySize.x * io.DisplayFramebufferScale.x));
    height = static_cast<int>(std::lround(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    return width > 0 && height > 0;
}

Px pixelsOf(const ImGuiIO& io, ImVec2 p) {
    return {p.x * io.DisplayFramebufferScale.x, p.y * io.DisplayFramebufferScale.y};
}

Ray rayThrough(const render::OrbitCamera& camera, int width, int height, Px p) {
    return camera.screenToRay(static_cast<int>(std::lround(p.x)),
                              static_cast<int>(std::lround(p.y)), width, height);
}

// Ray/plane intersection (interaction geometry, not transform maths).
bool planeHit(const Ray& ray, Vec3 p, Vec3 n, Vec3& hit) {
    const float denom = dot(ray.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(p - ray.origin, n) / denom;
    if (t <= 0.0f) return false;
    hit = ray.origin + ray.direction * t;
    return true;
}

float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

// Scene-owned increments are passed in by the active modal path. Keep snapping
// in one place so translation, rotation, and scale use identical rounding.
float Snap(float value, float increment) {
    return increment > 0.0f ? std::round(value / increment) * increment : value;
}

Vec3 translationOf(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

Vec3 unitAxis(int i) { return {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f}; }

std::vector<Vec3> selectedEditPointsWorld(const scene::World& world, ObjectId object,
                                          SelectMode mode) {
    std::vector<Vec3> points;
    const scene::MeshSelection* selection = world.editReadSelection(object, mode);
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!selection || !mesh) return points;

    std::vector<u32> vertices;
    auto addVertex = [&](u32 vertex) {
        if (vertex < mesh->positions.size() &&
            std::find(vertices.begin(), vertices.end(), vertex) == vertices.end())
            vertices.push_back(vertex);
    };
    for (u32 index : selection->indices) {
        if (mode == SelectMode::Vertex) {
            addVertex(index);
        } else if (mode == SelectMode::Edge) {
            if (index < mesh->edges.size()) {
                addVertex(mesh->edges[index].v0);
                addVertex(mesh->edges[index].v1);
            }
        } else if (index < mesh->faces.size()) {
            const scene::Face& face = mesh->faces[index];
            for (u32 k = 0; k < face.loopCount; ++k) {
                const u32 loop = face.firstLoop + k;
                if (loop < mesh->loops.size()) addVertex(mesh->loops[loop].vertex);
            }
        }
    }

    const Mat4 model = world.worldMatrix(object);
    points.reserve(vertices.size());
    for (u32 vertex : vertices) points.push_back(transformPoint(model, mesh->positions[vertex]));
    return points;
}

Vec3 activeEditElementWorld(const scene::World& world, ObjectId object, SelectMode mode,
                            Vec3 fallback) {
    const scene::MeshSelection* selection = world.editReadSelection(object, mode);
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!selection || selection->indices.empty() || !mesh) return fallback;
    const Mat4 model = world.worldMatrix(object);
    const u32 index = selection->indices.back();
    if (mode == SelectMode::Vertex) {
        if (index < mesh->positions.size()) return transformPoint(model, mesh->positions[index]);
        return fallback;
    }
    if (mode == SelectMode::Edge) {
        if (index >= mesh->edges.size()) return fallback;
        const scene::Edge& edge = mesh->edges[index];
        return (transformPoint(model, mesh->positions[edge.v0]) +
                transformPoint(model, mesh->positions[edge.v1])) * 0.5f;
    }
    if (index >= mesh->faces.size()) return fallback;
    const scene::Face& face = mesh->faces[index];
    Vec3 center{};
    u32 count = 0;
    for (u32 k = 0; k < face.loopCount; ++k) {
        const u32 loop = face.firstLoop + k;
        if (loop >= mesh->loops.size()) continue;
        const u32 vertex = mesh->loops[loop].vertex;
        if (vertex >= mesh->positions.size()) continue;
        center += transformPoint(model, mesh->positions[vertex]);
        ++count;
    }
    return count == 0 ? fallback : center * (1.0f / static_cast<float>(count));
}

bool faceGeometryWorld(const scene::World& world, ObjectId object, u32 faceIndex,
                       Vec3& center, Vec3& normal, std::vector<u32>* vertices = nullptr) {
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!mesh || faceIndex >= mesh->faces.size()) return false;
    const scene::Face& face = mesh->faces[faceIndex];
    if (face.loopCount < 3 || face.firstLoop + face.loopCount > mesh->loops.size())
        return false;

    const Mat4 model = world.worldMatrix(object);
    center = {};
    if (vertices) vertices->clear();
    for (u32 i = 0; i < face.loopCount; ++i) {
        const u32 vertex = mesh->loops[face.firstLoop + i].vertex;
        if (vertex >= mesh->positions.size()) return false;
        center += transformPoint(model, mesh->positions[vertex]);
        if (vertices) vertices->push_back(vertex);
    }
    center = center * (1.0f / static_cast<float>(face.loopCount));

    const Vec3 p0 = transformPoint(model, mesh->positions[mesh->loops[face.firstLoop].vertex]);
    for (u32 i = 1; i + 1 < face.loopCount; ++i) {
        const Vec3 p1 = transformPoint(model, mesh->positions[mesh->loops[face.firstLoop + i].vertex]);
        const Vec3 p2 = transformPoint(model, mesh->positions[mesh->loops[face.firstLoop + i + 1].vertex]);
        const Vec3 crossNormal = cross(p1 - p0, p2 - p0);
        if (dot(crossNormal, crossNormal) > 1e-12f) {
            normal = normalize(crossNormal);
            return true;
        }
    }
    return false;
}

std::vector<u32> faceVertexSet(const scene::EditableMesh& mesh, const scene::Face& face) {
    std::vector<u32> vertices;
    if (face.firstLoop + face.loopCount > mesh.loops.size()) return vertices;
    vertices.reserve(face.loopCount);
    for (u32 i = 0; i < face.loopCount; ++i)
        vertices.push_back(mesh.loops[face.firstLoop + i].vertex);
    std::sort(vertices.begin(), vertices.end());
    return vertices;
}

void restoreExtrudeBefore(scene::World& world, ObjectId object,
                          const scene::ObjectSnapshot& before,
                          const std::vector<ObjectId>& objectSelection,
                          const std::vector<u32>& faceSelection) {
    if (world.get(object)) world.destroy(object);
    world.reinsert(before);
    world.clearSelection();
    for (ObjectId selected : objectSelection) world.select(selected, true);
    world.editSetSelection(object, SelectMode::Face, faceSelection);
}

} // namespace

Vec3 resolveTransformPivot(const scene::World& world, PivotMode pivotMode,
                           InteractionMode interaction, SelectMode selectMode) {
    const std::vector<ObjectId>& selection = world.selection();
    const ObjectId active = selection.empty() ? ObjectId{} : selection.back();
    const scene::Object* activeObject = world.get(active);
    const Vec3 activeOrigin = activeObject ? translationOf(world.worldMatrix(active)) : Vec3{};

    if (interaction == InteractionMode::Edit && activeObject) {
        const std::vector<Vec3> points = selectedEditPointsWorld(world, active, selectMode);
        if (pivotMode == PivotMode::Cursor3D) return world.cursor();
        if (pivotMode == PivotMode::IndividualOrigins) return activeOrigin;
        if (pivotMode == PivotMode::ActiveElement)
            return activeEditElementWorld(world, active, selectMode, activeOrigin);
        if (points.empty()) return activeOrigin;
        if (pivotMode == PivotMode::BoundingBoxCenter) {
            Vec3 lo = points.front();
            Vec3 hi = points.front();
            for (const Vec3& p : points) {
                lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            return (lo + hi) * 0.5f;
        }
        Vec3 median{};
        for (const Vec3& p : points) median += p;
        return median * (1.0f / static_cast<float>(points.size()));
    }

    switch (pivotMode) {
        case PivotMode::Cursor3D:
            return world.cursor();
        case PivotMode::BoundingBoxCenter: {
            const std::optional<scene::WorldBounds> bounds = world.selectionBounds();
            if (bounds) return bounds->center;
            return activeOrigin;
        }
        case PivotMode::MedianPoint: {
            Vec3 sum{};
            std::size_t count = 0;
            for (ObjectId id : selection) {
                if (!world.get(id)) continue;
                sum += translationOf(world.worldMatrix(id));
                ++count;
            }
            return count == 0 ? activeOrigin : sum * (1.0f / static_cast<float>(count));
        }
        case PivotMode::IndividualOrigins:
        case PivotMode::ActiveElement:
            // ModalTransform currently edits the active object; its individual
            // origin is therefore the only applicable origin in object mode.
            return activeOrigin;
    }
    return activeOrigin;
}

bool ModalTransform::beginFaceExtrude(scene::World& world, const ImGuiIO& io,
                                      const render::OrbitCamera& camera,
                                      SelectMode selectMode) {
    if (selectMode != SelectMode::Face) return false;
    const ObjectId object = world.selection().empty() ? ObjectId{} : world.selection().back();
    const scene::MeshSelection* selection = world.editReadSelection(object, SelectMode::Face);
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!world.get(object) || !selection || selection->indices.empty() || !mesh) return false;

    int width = 0;
    int height = 0;
    if (!framebuffer(io, width, height)) return false;
    const Px mouse = pixelsOf(io, io.MousePos);
    Vec3 center{};
    Vec3 averageNormal{};
    struct SelectedFace {
        u32 index;
        Vec3 center;
        Vec3 normal;
        std::vector<u32> vertices;
    };
    std::vector<SelectedFace> selectedFaces;
    selectedFaces.reserve(selection->indices.size());
    std::vector<u32> allVertices;
    for (u32 face : selection->indices) {
        Vec3 faceCenter{}, faceNormal{};
        std::vector<u32> vertices;
        if (!faceGeometryWorld(world, object, face, faceCenter, faceNormal, &vertices)) return false;
        // The existing scene operator extrudes one face at a time. Disjoint
        // faces are safe to compose; a shared edge/vertex needs a region
        // operator to avoid creating interior walls, so refuse that case.
        for (u32 vertex : vertices)
            if (std::find(allVertices.begin(), allVertices.end(), vertex) != allVertices.end())
                return false;
        allVertices.insert(allVertices.end(), vertices.begin(), vertices.end());
        selectedFaces.push_back({face, faceCenter, faceNormal, std::move(vertices)});
        center += faceCenter;
        averageNormal += faceNormal;
    }
    if (dot(averageNormal, averageNormal) < 1e-10f) return false;
    center = center * (1.0f / static_cast<float>(selectedFaces.size()));
    averageNormal = normalize(averageNormal);
    const std::optional<scene::ObjectSnapshot> before = world.captureSnapshot(object);
    if (!before) return false;
    const std::vector<ObjectId> objectSelectionBefore = world.selection();
    const std::vector<u32> faceSelectionBefore = selection->indices;

    std::vector<std::size_t> order(selectedFaces.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return selectedFaces[a].index > selectedFaces[b].index;
    });
    std::vector<std::vector<u32>> capRings;
    std::vector<u32> capVertices;
    std::vector<Vec3> capNormals;
    capRings.reserve(order.size());
    for (std::size_t selectedIndex : order) {
        const SelectedFace& selectedFace = selectedFaces[selectedIndex];
        if (!world.editExtrudeFace(object, selectedFace.index, Vec3{})) {
            restoreExtrudeBefore(world, object, *before, objectSelectionBefore,
                                 faceSelectionBefore);
            return false;
        }
        mesh = world.readableEdit(object);
        if (!mesh || mesh->faces.empty()) {
            restoreExtrudeBefore(world, object, *before, objectSelectionBefore,
                                 faceSelectionBefore);
            return false;
        }
        const std::vector<u32> ring = faceVertexSet(*mesh, mesh->faces.back());
        if (ring.size() != selectedFace.vertices.size()) {
            restoreExtrudeBefore(world, object, *before, objectSelectionBefore,
                                 faceSelectionBefore);
            return false;
        }
        capRings.push_back(ring);
        for (u32 vertex : ring) {
            capVertices.push_back(vertex);
            capNormals.push_back(selectedFace.normal);
        }
    }

    mesh = world.readableEdit(object);
    if (!mesh) {
        restoreExtrudeBefore(world, object, *before, objectSelectionBefore,
                             faceSelectionBefore);
        return false;
    }
    std::vector<u32> capsAfter;
    for (u32 face = 0; face < mesh->faces.size(); ++face) {
        const std::vector<u32> vertices = faceVertexSet(*mesh, mesh->faces[face]);
        if (std::find(capRings.begin(), capRings.end(), vertices) != capRings.end())
            capsAfter.push_back(face);
    }
    if (capsAfter.size() != capRings.size()) {
        restoreExtrudeBefore(world, object, *before, objectSelectionBefore,
                             faceSelectionBefore);
        return false;
    }

    id_ = object;
    active_ = true;
    dragMode_ = false;
    started_ = false;
    editTarget_ = true;
    faceExtrude_ = true;
    mode_ = GizmoState::Mode::Translate;
    extrudeBefore_ = *before;
    extrudeObjectSelectionBefore_ = objectSelectionBefore;
    extrudeSelectionBefore_ = faceSelectionBefore;
    extrudeSelectionAfter_ = capsAfter;
    editVertexIds_ = std::move(capVertices);
    extrudeVertexNormals_ = std::move(capNormals);
    editBefore_ = world.editCaptureVertexPositions(id_, editVertexIds_);
    if (editBefore_.size() != editVertexIds_.size()) {
        restoreExtrudeBefore(world, id_, *extrudeBefore_, extrudeObjectSelectionBefore_,
                             extrudeSelectionBefore_);
        active_ = false;
        editTarget_ = false;
        faceExtrude_ = false;
        extrudeBefore_.reset();
        editVertexIds_.clear();
        editBefore_.clear();
        extrudeVertexNormals_.clear();
        return false;
    }
    extrudeOrigin_ = center;
    extrudeNormal_ = averageNormal;
    grabX_ = mouse.x;
    grabY_ = mouse.y;
    world.editSetSelection(id_, SelectMode::Face, extrudeSelectionAfter_);
    label_ = "Extrude Face  +0.000 units";
    hint_ = "Move the mouse along the face normal  |  Left click / Enter confirm  |  Right click / Esc cancel";
    return true;
}

bool ModalTransform::updateFaceExtrude(scene::World& world, UndoStack& undo,
                                       const render::OrbitCamera& camera,
                                       bool snapEnabled) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        finishFaceExtrude(world, undo, false);
        return true;
    }
    if (!world.get(id_)) {
        finishFaceExtrude(world, undo, false);
        return true;
    }

    int width = 0;
    int height = 0;
    if (!framebuffer(io, width, height)) {
        finishFaceExtrude(world, undo, false);
        return true;
    }
    const Px now = pixelsOf(io, io.MousePos);
    float distance = 0.0f;
    Px axisBase{}, axisTip{};
    if (camera.projectScreen(extrudeOrigin_, width, height, axisBase.x, axisBase.y) &&
        camera.projectScreen(extrudeOrigin_ + extrudeNormal_, width, height,
                             axisTip.x, axisTip.y)) {
        const float dx = axisTip.x - axisBase.x;
        const float dy = axisTip.y - axisBase.y;
        const float len2 = dx * dx + dy * dy;
        if (len2 > 1e-6f) {
            distance = ((now.x - grabX_) * dx + (now.y - grabY_) * dy) / len2;
        } else {
            // A face normal aimed straight at the camera has no screen-space
            // axis projection. In that singular view use Blender's vertical
            // drag convention, scaled to world units at the face depth.
            float pixelsPerUnit = 0.0f;
            if (camera.isOrthographic()) {
                pixelsPerUnit = static_cast<float>(height) / camera.orthoHeight();
            } else {
                const Vec3 viewNormal = normalize(camera.target() - camera.position());
                const float depth = dot(extrudeOrigin_ - camera.position(), viewNormal);
                if (depth > 1e-6f)
                    pixelsPerUnit = static_cast<float>(height) /
                        (2.0f * std::tan(camera.fovY() * 0.5f) * depth);
            }
            if (pixelsPerUnit > 1e-6f)
                distance = -(now.y - grabY_) / pixelsPerUnit;
        }
    }
    distance *= io.KeyShift ? kPrecision : 1.0f;
    const bool snap = snapEnabled != io.KeyCtrl;
    if (snap) distance = Snap(distance, world.snapTranslateIncrement());

    const Mat4 invModel = inverse(world.worldMatrix(id_));
    std::vector<Vec3> positions;
    positions.reserve(editBefore_.size());
    for (std::size_t i = 0; i < editBefore_.size(); ++i) {
        const Vec3 worldOffset = extrudeVertexNormals_[i] * distance;
        positions.push_back(editBefore_[i] + transformDirection(invModel, worldOffset));
    }
    world.editApplyVertexPositions(id_, editVertexIds_, positions);
    char value[64] = {};
    std::snprintf(value, sizeof value, "%+.3f units", static_cast<double>(distance));
    label_ = std::string("Extrude Face  ") + value;
    if (io.KeyShift) label_ += "  (precision)";
    if (snap) label_ += "  (snapped)";

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        finishFaceExtrude(world, undo, true);
    return true;
}

void ModalTransform::finishFaceExtrude(scene::World& world, UndoStack& undo, bool confirm) {
    if (confirm && extrudeBefore_) {
        const std::optional<scene::ObjectSnapshot> after = world.captureSnapshot(id_);
        if (after) {
            undo.push(std::make_unique<EditTopologyCommand>(
                world, id_, *extrudeBefore_, *after, extrudeObjectSelectionBefore_,
                world.selection(), SelectMode::Face, extrudeSelectionBefore_,
                extrudeSelectionAfter_, "Extrude Face"));
        } else {
            confirm = false;
        }
    }
    if (!confirm && extrudeBefore_)
        restoreExtrudeBefore(world, id_, *extrudeBefore_, extrudeObjectSelectionBefore_,
                             extrudeSelectionBefore_);
    active_ = false;
    dragMode_ = false;
    started_ = false;
    editTarget_ = false;
    faceExtrude_ = false;
    extrudeBefore_.reset();
    extrudeObjectSelectionBefore_.clear();
    extrudeSelectionBefore_.clear();
    extrudeSelectionAfter_.clear();
    editVertexIds_.clear();
    editBefore_.clear();
    extrudeVertexNormals_.clear();
}

bool ModalTransform::update(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                            TransformSpace space, InteractionMode interaction,
                            SelectMode selectMode, bool snapEnabled, PivotMode pivotMode) {
    ImGuiIO& io = ImGui::GetIO();

    // ---- start -------------------------------------------------------------
    if (!active_) {
        if (io.WantTextInput || io.KeyCtrl || io.KeyAlt) return false;
        if (interaction == InteractionMode::Edit && !io.KeyShift &&
            ImGui::IsKeyPressed(ImGuiKey_E))
            return beginFaceExtrude(world, io, camera, selectMode);
        GizmoState::Mode mode = GizmoState::Mode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_G)) {
            mode = GizmoState::Mode::Translate;
        } else if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            mode = GizmoState::Mode::Rotate;
        } else if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            mode = GizmoState::Mode::Scale;
        } else {
            return false;
        }
        start(world, io, mode, space, interaction, selectMode, pivotMode);
        return active_;
    }

    if (faceExtrude_) return updateFaceExtrude(world, undo, camera, snapEnabled);

    // ---- confirm / cancel --------------------------------------------------
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        finish(undo, false);
        return true;
    }
    // Confirm: Enter always works. A keyboard gesture confirms on the click; a
    // handle drag confirms on the button RELEASE (the button was already down).
    const bool mouseConfirm = dragMode_ ? ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                                        : ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
        mouseConfirm) {
        finish(undo, true);
        return true;
    }

    // ---- axis constraint: X/Y/Z, again = local, Shift = plane --------------
    const ImGuiKey axisKeys[3] = {ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z};
    for (int i = 0; i < 3; ++i) {
        if (!ImGui::IsKeyPressed(axisKeys[i])) continue;
        const bool plane = io.KeyShift;
        if (axis_ == i && plane_ == plane) {
            local_ = !local_;
        } else {
            axis_ = i;
            plane_ = plane;
            local_ = false;
        }
    }

    // ---- typed value: MAIN number row only (D29: numpad stays navigation) --
    {
        bool changed = false;
        for (int k = 0; k <= 9; ++k) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + k))) {
                typed_.push_back(static_cast<char>('0' + k));
                changed = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus)) {
            typed_.push_back('-');
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Period)) {
            typed_.push_back('.');
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !typed_.empty()) {
            typed_.pop_back();
            changed = true;
        }
        if (changed) {
            char* end = nullptr;
            typedValue_ = std::strtof(typed_.c_str(), &end);
            typedValid_ = !typed_.empty() && end != typed_.c_str() && *end == '\0';
        }
    }

    // ---- per-frame value, absolute from the grab point ---------------------
    const scene::Object* o = world.get(id_);
    int width = 0;
    int height = 0;
    if (!o || !framebuffer(io, width, height)) {
        finish(undo, false);
        return true;
    }

    const Px now = pixelsOf(io, io.MousePos);
    const Px grab{grabX_, grabY_};
    const float factor = io.KeyShift ? kPrecision : 1.0f;
    const bool snap = snapEnabled != io.KeyCtrl;  // Ctrl temporarily toggles the header magnet
    const Vec3 viewNormal = normalize(camera.target() - camera.position());
    // The constraint direction in world space, scene-resolved (item 66).
    const Vec3 constraint = axis_ >= 0 ? world.resolveWorldAxis(id_, axis_, local_) : viewNormal;

    Px pivotPx{};
    const bool havePivot = camera.projectScreen(pivot_, width, height, pivotPx.x, pivotPx.y);
    Px interactionPx{};
    const bool haveInteractionOrigin =
        camera.projectScreen(interactionOrigin_, width, height, interactionPx.x, interactionPx.y);

    Transform next = before_;
    char value[64] = {};

    if (mode_ == GizmoState::Mode::Translate) {
        const float snapInc = world.snapTranslateIncrement();
        float shown = 0.0f;
        if (axis_ >= 0 && !plane_) {
            // Screen projection: pixels along the axis's screen direction, scaled by
            // the pixels one world unit spans at the translation origin's depth.
            float total = 0.0f;
            if (haveInteractionOrigin) {
                Px tipPx{};
                if (camera.projectScreen(interactionOrigin_ + constraint, width, height,
                                         tipPx.x, tipPx.y)) {
                    const float dx = tipPx.x - interactionPx.x;
                    const float dy = tipPx.y - interactionPx.y;
                    const float len2 = dx * dx + dy * dy;   // (px per world unit)^2
                    if (len2 > 1e-6f) {
                        total = ((now.x - grab.x) * dx + (now.y - grab.y) * dy) / len2 * factor;
                    }
                }
            }
            if (typedValid_) total = typedValue_;
            // Exact numeric entry replaces the mouse magnitude, even while Ctrl
            // snapping is active; typed values are not quantized a second time.
            if (snap && !typedValid_) total = Snap(total, snapInc);
            const Vec3 delta = constraint * total;
            if (editTarget_) {
                const Mat4 model = world.worldMatrix(id_);
                const Mat4 inv = inverse(model);
                std::vector<Vec3> after;
                after.reserve(editBefore_.size());
                for (const Vec3& local : editBefore_)
                    after.push_back(transformPoint(inv, transformPoint(model, local) + delta));
                applyEditPositions(world, undo, after);
            } else {
                next = world.typedTranslate(id_, before_, constraint, total); // absolute from start
            }
            shown = total;
        } else {
            // Free move (or a Shift+axis plane): translation stays anchored to
            // the gesture/gizmo origin; PivotMode only changes rotate/scale center.
            Vec3 delta{};
            const Vec3 normal = plane_ ? constraint : viewNormal;
            const Ray rayGrab = rayThrough(camera, width, height, grab);
            const Ray rayNow = rayThrough(camera, width, height, now);
            Vec3 p0{}, p1{};
            if (planeHit(rayGrab, interactionOrigin_, normal, p0) &&
                planeHit(rayNow, interactionOrigin_, normal, p1)) {
                delta = (p1 - p0) * factor;
            }
            if (snap) {
                delta = Vec3{Snap(delta.x, snapInc), Snap(delta.y, snapInc),
                             Snap(delta.z, snapInc)};
            }
            if (typedValid_) {
                // Free move: the number is along global X (Blender). Plane: the first
                // axis of that plane.
                const Vec3 dir = plane_ ? unitAxis(axis_ == 0 ? 1 : 0) : unitAxis(0);
                delta = dir * typedValue_;
                shown = typedValue_;
            } else {
                shown = length(delta);
            }
            if (editTarget_) {
                const Mat4 model = world.worldMatrix(id_);
                const Mat4 inv = inverse(model);
                std::vector<Vec3> after;
                after.reserve(editBefore_.size());
                for (const Vec3& local : editBefore_)
                    after.push_back(transformPoint(inv, transformPoint(model, local) + delta));
                applyEditPositions(world, undo, after);
            } else if (typedValid_) {
                const Vec3 dir = plane_ ? unitAxis(axis_ == 0 ? 1 : 0) : unitAxis(0);
                next = world.typedTranslate(id_, before_, dir, typedValue_);
            } else {
                // Apply the absolute-from-start delta to the captured transform.
                next = before_;
                next.position = before_.position + world.worldAxisInParentFrame(id_, delta);
            }
        }
        std::snprintf(value, sizeof value, "%+.3f units", static_cast<double>(shown));
    } else if (mode_ == GizmoState::Mode::Rotate) {
        float angle = 0.0f;
        if (havePivot) {
            const float aNow = std::atan2(now.y - pivotPx.y, now.x - pivotPx.x);
            const float aGrab = std::atan2(grab.y - pivotPx.y, grab.x - pivotPx.x);
            angle = wrapAngle(aNow - aGrab) * factor;
        }
        // Plane makes no sense for a single-angle rotate: Shift+axis acts as the axis.
        const Vec3 rotAxis = axis_ >= 0 ? world.resolveWorldAxis(id_, axis_, local_) : viewNormal;
        const float snapRad = world.snapRotateDegrees() * kPi / 180.0f;
        if (snap && !typedValid_) angle = Snap(angle, snapRad);
        if (typedValid_) {
            next = world.typedRotate(id_, before_, rotAxis, typedValue_, pivot_);
        } else {
            next = world.composeWorldRotatePivot(id_, before_, rotAxis, angle, pivot_);
        }
        if (editTarget_) {
            const float applied = typedValid_ ? typedValue_ * kPi / 180.0f : angle;
            const Mat4 rotation = rotationAxis(rotAxis, applied);
            const Mat4 model = world.worldMatrix(id_);
            const Mat4 inv = inverse(model);
            std::vector<Vec3> after;
            after.reserve(editBefore_.size());
            for (const Vec3& local : editBefore_) {
                const Vec3 p = transformPoint(model, local);
                const Vec3 rotated = pivot_ + transformDirection(rotation, p - pivot_);
                after.push_back(transformPoint(inv, rotated));
            }
            applyEditPositions(world, undo, after);
        }
        const float degrees = typedValid_ ? typedValue_ : angle * 180.0f / kPi;
        std::snprintf(value, sizeof value, "%+.2f deg", static_cast<double>(degrees));
    } else {
        float ratio = 1.0f;
        if (havePivot) {
            const float rNow = std::hypot(now.x - pivotPx.x, now.y - pivotPx.y);
            const float rGrab = std::hypot(grab.x - pivotPx.x, grab.y - pivotPx.y);
            if (rGrab > 2.0f) ratio = 1.0f + (rNow / rGrab - 1.0f) * factor;
        }
        if (typedValid_) ratio = typedValue_;
        ratio = std::max(ratio, kMinScale);
        const float snapInc = world.snapScaleIncrement();
        if (snap && !typedValid_) ratio = std::max(kMinScale, Snap(ratio, snapInc));
        Vec3 ratios{ratio, ratio, ratio};
        if (axis_ >= 0 && !plane_) {
            ratios = {1.0f, 1.0f, 1.0f};
            (axis_ == 0 ? ratios.x : axis_ == 1 ? ratios.y : ratios.z) = ratio;
        } else if (plane_) {
            // The constrained axis keeps 1; the other two scale together.
            (axis_ == 0 ? ratios.x : axis_ == 1 ? ratios.y : ratios.z) = 1.0f;
        }
        // Scale's typed value is already an absolute-from-start ratio, so the typed
        // and mouse paths share composeWorldScalePivot with start = the captured
        // transform. (typedScale would be this same call once its body lands.)
        next = world.composeWorldScalePivot(id_, before_, ratios, pivot_, local_);
        if (editTarget_) {
            const Vec3 axes[3] = {world.resolveWorldAxis(id_, 0, local_),
                                  world.resolveWorldAxis(id_, 1, local_),
                                  world.resolveWorldAxis(id_, 2, local_)};
            const Mat4 model = world.worldMatrix(id_);
            const Mat4 inv = inverse(model);
            std::vector<Vec3> after;
            after.reserve(editBefore_.size());
            for (const Vec3& local : editBefore_) {
                const Vec3 p = transformPoint(model, local);
                const Vec3 d = p - pivot_;
                const Vec3 scaled = axes[0] * (dot(d, axes[0]) * ratios.x) +
                                    axes[1] * (dot(d, axes[1]) * ratios.y) +
                                    axes[2] * (dot(d, axes[2]) * ratios.z);
                after.push_back(transformPoint(inv, pivot_ + scaled));
            }
            applyEditPositions(world, undo, after);
        }
        std::snprintf(value, sizeof value, "x%.3f", static_cast<double>(ratio));
    }

    // ---- apply: one command for the whole gesture ---------------------------
    if (!editTarget_ && next != o->transform) {
        undo.push(std::make_unique<TransformCommand>(world, id_, before_, next, started_));
        started_ = true;
    }

    // ---- status bar text ----------------------------------------------------
    {
        const char* verb = mode_ == GizmoState::Mode::Translate ? "Move"
                         : mode_ == GizmoState::Mode::Rotate   ? "Rotate"
                                                              : "Scale";
        const char* names[3] = {"X", "Y", "Z"};
        std::string c = "free";
        if (axis_ >= 0) {
            c = std::string(local_ ? "local " : "global ") + names[axis_];
            if (plane_ && mode_ != GizmoState::Mode::Rotate) c = std::string("plane ") + c;
        }
        label_ = std::string(verb) + " (" + c + ")  " + value;
        if (!typed_.empty()) label_ += "   [typed: " + typed_ + "]";
    }
    buildText();
    return true;
}

bool ModalTransform::beginHandleDrag(scene::World& world, const ImGuiIO& io, GizmoState::Mode mode,
                                     int axis, TransformSpace space, PivotMode pivotMode,
                                     const Vec3& gizmoOrigin, InteractionMode interaction,
                                     SelectMode selectMode) {
    start(world, io, mode, space, interaction, selectMode, pivotMode);
    if (!active_) return false;
    dragMode_ = true;
    interactionOrigin_ = gizmoOrigin;
    axis_ = axis;           // the handle's axis pre-constrains the drag
    plane_ = false;
    return true;
}

void ModalTransform::start(scene::World& world, const ImGuiIO& io, GizmoState::Mode mode,
                           TransformSpace space, InteractionMode interaction,
                           SelectMode selectMode, PivotMode pivotMode) {
    const ObjectId id = world.selection().empty() ? ObjectId{} : world.selection().back();
    const scene::Object* o = world.get(id);
    if (!o) return;

    id_ = id;
    editTarget_ = interaction == InteractionMode::Edit;
    if (editTarget_ && !captureEditSelection(world, selectMode)) return;

    active_ = true;
    dragMode_ = false;
    started_ = false;
    mode_ = mode;
    before_ = o->transform;
    pivot_ = resolveTransformPivot(world, pivotMode, interaction, selectMode);
    interactionOrigin_ = resolveTransformPivot(world, PivotMode::MedianPoint,
                                                interaction, selectMode);
    axis_ = -1;
    local_ = space == TransformSpace::Local;   // Blender: first axis press follows the UI orientation
    plane_ = false;
    typed_.clear();
    typedValue_ = 0.0f;
    typedValid_ = false;
    const Px mouse = pixelsOf(io, io.MousePos);
    grabX_ = mouse.x;
    grabY_ = mouse.y;
    buildText();
}

bool ModalTransform::captureEditSelection(scene::World& world, SelectMode mode) {
    editVertexIds_.clear();
    editBefore_.clear();
    const scene::MeshSelection* selection = world.editReadSelection(id_, mode);
    const scene::EditableMesh* mesh = world.readableEdit(id_);
    if (!selection || !mesh || selection->indices.empty()) return false;

    auto addVertex = [&](u32 v) {
        if (v < mesh->positions.size() &&
            std::find(editVertexIds_.begin(), editVertexIds_.end(), v) == editVertexIds_.end()) {
            editVertexIds_.push_back(v);
        }
    };
    for (u32 index : selection->indices) {
        if (mode == SelectMode::Vertex) {
            addVertex(index);
        } else if (mode == SelectMode::Edge) {
            if (index < mesh->edges.size()) {
                addVertex(mesh->edges[index].v0);
                addVertex(mesh->edges[index].v1);
            }
        } else if (index < mesh->faces.size()) {
            const scene::Face& face = mesh->faces[index];
            for (u32 k = 0; k < face.loopCount; ++k) {
                const u32 li = face.firstLoop + k;
                if (li < mesh->loops.size()) addVertex(mesh->loops[li].vertex);
            }
        }
    }
    editBefore_ = world.editCaptureVertexPositions(id_, editVertexIds_);
    return !editVertexIds_.empty() && editBefore_.size() == editVertexIds_.size();
}

void ModalTransform::applyEditPositions(scene::World& world, UndoStack& undo,
                                         const std::vector<Vec3>& positions) {
    if (positions.size() != editVertexIds_.size() || positions == editBefore_) return;
    undo.push(std::make_unique<EditVertexPositionsCommand>(world, id_, editVertexIds_,
                                                            editBefore_, positions, started_));
    started_ = true;
}

void ModalTransform::finish(UndoStack& undo, bool confirm) {
    // Cancel restores the captured Transform bitwise: discarding the one command
    // runs its undo (setTransform(before_)) and drops it. Confirm keeps one entry.
    if (!confirm && started_) undo.discardLast();
    active_ = false;
    dragMode_ = false;
    started_ = false;
}

void ModalTransform::buildText() {
    hint_ = "Move the mouse  |  X/Y/Z axis (again: local)  |  Shift+X/Y/Z plane  |  "
            "Shift precision  |  Ctrl snap  |  type a value (main row)  |  "
            "Left click / Enter confirm  |  Right click / Esc cancel";
}

} // namespace ember::editor
