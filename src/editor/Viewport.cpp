#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

#include "editor/Commands.h"

namespace ember::editor {

namespace {

// Window points <-> viewport-local framebuffer pixels (D16/D17): ImGui's mouse
// is in window points; screenToRay wants pixels local to the rendered viewport,
// top-left, y down. Convert to framebuffer once, subtract the viewport origin,
// then use the same viewport dimensions that built the projection matrix.
struct Pixel {
    int x = 0;
    int y = 0;
};

Pixel toFramebuffer(const ImGuiIO& io, ImVec2 p) {
    return {static_cast<int>(std::lround(p.x * io.DisplayFramebufferScale.x)),
            static_cast<int>(std::lround(p.y * io.DisplayFramebufferScale.y))};
}

// The viewport ray through a window-point position.
std::optional<Ray> rayAt(const render::OrbitCamera& camera, const ImGuiIO& io,
                         const Rect& viewport, ImVec2 p) {
    if (!viewport.valid()) return std::nullopt;
    const Pixel px = toFramebuffer(io, p);
    return camera.screenToRay(px.x - viewport.x, px.y - viewport.y,
                              viewport.w, viewport.h);
}

std::optional<scene::PickResult> pickAt(const scene::World& world, const render::OrbitCamera& camera,
                                        const ImGuiIO& io, const Rect& viewport, ImVec2 p) {
    const std::optional<Ray> ray = rayAt(camera, io, viewport, p);
    if (!ray) return std::nullopt;
    return world.pick(*ray);
}

float pointSegmentDistanceSquared(float px, float py, float ax, float ay,
                                  float bx, float by, float& outT) {
    const float dx = bx - ax, dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    outT = lengthSquared > 1e-6f
        ? std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0f, 1.0f)
        : 0.0f;
    const float ex = ax + dx * outT - px;
    const float ey = ay + dy * outT - py;
    return ex * ex + ey * ey;
}

bool pointInsideFace(const std::vector<Vec3>& polygon, const Vec3& point,
                     const Vec3& normal) {
    if (polygon.size() < 3) return false;
    const float ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
    const auto project = [&](const Vec3& p) -> ImVec2 {
        if (az >= ax && az >= ay) return ImVec2(p.x, p.y);
        if (ax >= ay && ax >= az) return ImVec2(p.y, p.z);
        return ImVec2(p.z, p.x);
    };
    const ImVec2 query = project(point);
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const ImVec2 a = project(polygon[i]);
        const ImVec2 b = project(polygon[j]);
        const bool crosses = ((a.y > query.y) != (b.y > query.y)) &&
            (query.x < (b.x - a.x) * (query.y - a.y) /
                           ((b.y - a.y) == 0.0f ? 1e-20f : (b.y - a.y)) + a.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

bool elementVisible(const scene::World& world, ObjectId object, const Ray& ray,
                    const Vec3& point, bool xray) {
    if (xray) return true;
    const std::optional<scene::PickResult> front = world.pick(ray);
    if (!front) return true;
    const float elementDepth = dot(point - ray.origin, ray.direction);
    constexpr float kDepthTolerance = 0.04f;
    return elementDepth <= front->distance + kDepthTolerance;
}

int pickEditElement(const scene::World& world, const render::OrbitCamera& camera,
                    const ImGuiIO& io, const Rect& viewport, ObjectId object,
                    SelectMode mode, bool xray, ImVec2 mouse) {
    const scene::EditableMesh* mesh = world.readableEdit(object);
    if (!mesh || !viewport.valid()) return -1;
    const Mat4 model = world.worldMatrix(object);
    const Pixel pixel = toFramebuffer(io, mouse);
    const float mx = static_cast<float>(pixel.x - viewport.x) + 0.5f;
    const float my = static_cast<float>(pixel.y - viewport.y) + 0.5f;
    const std::optional<Ray> ray = rayAt(camera, io, viewport, mouse);
    if (!ray) return -1;
    const float scale = std::max(io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);

    if (mode == SelectMode::Vertex) {
        const float radius = ember::kEditVertexRadiusPixels * scale;
        float bestDistanceSquared = radius * radius;
        float bestDepth = std::numeric_limits<float>::infinity();
        int best = -1;
        for (u32 vertex = 0; vertex < mesh->positions.size(); ++vertex) {
            const Vec3 worldPosition = transformPoint(model, mesh->positions[vertex]);
            float sx = 0.0f, sy = 0.0f;
            if (!camera.projectScreen(worldPosition, viewport.w, viewport.h, sx, sy)) continue;
            const float dx = sx - mx, dy = sy - my;
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > bestDistanceSquared ||
                !elementVisible(world, object, *ray, worldPosition, xray)) continue;
            const float depth = dot(worldPosition - ray->origin, ray->direction);
            if (distanceSquared < bestDistanceSquared || depth < bestDepth) {
                bestDistanceSquared = distanceSquared;
                bestDepth = depth;
                best = static_cast<int>(vertex);
            }
        }
        return best;
    }

    if (mode == SelectMode::Edge) {
        const float radius = ember::kEditEdgeWidthPixels * scale;
        float bestDistanceSquared = radius * radius;
        float bestDepth = std::numeric_limits<float>::infinity();
        int best = -1;
        for (u32 edgeIndex = 0; edgeIndex < mesh->edges.size(); ++edgeIndex) {
            const scene::Edge& edge = mesh->edges[edgeIndex];
            if (edge.v0 >= mesh->positions.size() || edge.v1 >= mesh->positions.size()) continue;
            const Vec3 a = transformPoint(model, mesh->positions[edge.v0]);
            const Vec3 b = transformPoint(model, mesh->positions[edge.v1]);
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (!camera.projectScreen(a, viewport.w, viewport.h, ax, ay) ||
                !camera.projectScreen(b, viewport.w, viewport.h, bx, by)) continue;
            float t = 0.0f;
            const float distanceSquared = pointSegmentDistanceSquared(mx, my, ax, ay, bx, by, t);
            const Vec3 worldPoint = a + (b - a) * t;
            if (distanceSquared > bestDistanceSquared ||
                !elementVisible(world, object, *ray, worldPoint, xray)) continue;
            const float depth = dot(worldPoint - ray->origin, ray->direction);
            if (distanceSquared < bestDistanceSquared || depth < bestDepth) {
                bestDistanceSquared = distanceSquared;
                bestDepth = depth;
                best = static_cast<int>(edgeIndex);
            }
        }
        return best;
    }

    // Face selection uses the true polygon ring (not its rendered triangle
    // fan), so concave n-gons are still selectable in the interior.
    float bestDepth = std::numeric_limits<float>::infinity();
    int best = -1;
    for (u32 faceIndex = 0; faceIndex < mesh->faces.size(); ++faceIndex) {
        const scene::Face& face = mesh->faces[faceIndex];
        if (face.loopCount < 3 || face.firstLoop + face.loopCount > mesh->loops.size()) continue;
        std::vector<Vec3> polygon;
        polygon.reserve(face.loopCount);
        for (u32 i = 0; i < face.loopCount; ++i) {
            const u32 vertex = mesh->loops[face.firstLoop + i].vertex;
            if (vertex >= mesh->positions.size()) { polygon.clear(); break; }
            polygon.push_back(transformPoint(model, mesh->positions[vertex]));
        }
        if (polygon.size() < 3) continue;
        Vec3 normal{};
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const Vec3& a = polygon[i];
            const Vec3& b = polygon[(i + 1) % polygon.size()];
            normal += Vec3{(a.y - b.y) * (a.z + b.z),
                           (a.z - b.z) * (a.x + b.x),
                           (a.x - b.x) * (a.y + b.y)};
        }
        normal = normalize(normal);
        if (dot(normal, normal) < 1e-8f) continue;
        const float denominator = dot(ray->direction, normal);
        if (std::fabs(denominator) < 1e-6f) continue;
        const float t = dot(polygon.front() - ray->origin, normal) / denominator;
        if (t <= 0.0f || t >= bestDepth) continue;
        const Vec3 hit = ray->origin + ray->direction * t;
        if (!pointInsideFace(polygon, hit, normal) ||
            !elementVisible(world, object, *ray, hit, xray)) continue;
        best = static_cast<int>(faceIndex);
        bestDepth = t;
    }
    return best;
}

void updateEditSelection(scene::World& world, ObjectId object, SelectMode mode,
                         int hit, bool shift) {
    std::vector<u32> selection;
    if (shift) {
        if (const scene::MeshSelection* current = world.editReadSelection(object, mode))
            selection = current->indices;
    }
    if (hit >= 0) {
        const u32 index = static_cast<u32>(hit);
        const auto it = std::find(selection.begin(), selection.end(), index);
        if (shift && it != selection.end()) selection.erase(it);
        else if (it == selection.end()) selection.push_back(index);
    }
    world.editSetSelection(object, mode, selection);
}

bool isSelected(const scene::World& world, ObjectId id) {
    for (ObjectId s : world.selection()) {
        if (s == id) return true;
    }
    return false;
}

// Intersection of a ray with the plane through p with normal n.
bool planeHit(const Ray& ray, Vec3 p, Vec3 n, Vec3& hit) {
    const float denom = dot(ray.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(p - ray.origin, n) / denom;
    if (t <= 0.0f) return false;
    hit = ray.origin + ray.direction * t;
    return true;
}

struct ScreenBounds {
    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();
    bool valid = false;
};

// Object-mode box selection is evaluated in the same viewport-local pixel
// space as picking. The projected mesh bounds provide the familiar Blender
// rule: left-to-right encloses, right-to-left crosses. This is intentionally a
// screen-space bounds test (not an exact clipped triangle test); an object with
// any vertex behind the eye is skipped rather than assigned an incomplete box.
ScreenBounds projectedBounds(const DrawItem& item, const render::OrbitCamera& camera,
                             const Rect& viewport) {
    ScreenBounds bounds;
    if (!item.mesh || !viewport.valid() || item.mesh->positions.size() < 3) return bounds;
    const std::size_t vertexCount = item.mesh->positions.size() / 3;
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const std::size_t offset = vertex * 3;
        const Vec3 local{item.mesh->positions[offset], item.mesh->positions[offset + 1],
                         item.mesh->positions[offset + 2]};
        float x = 0.0f, y = 0.0f;
        if (!camera.projectScreen(transformPoint(item.model, local), viewport.w, viewport.h, x, y))
            return {};
        bounds.minX = std::min(bounds.minX, x);
        bounds.minY = std::min(bounds.minY, y);
        bounds.maxX = std::max(bounds.maxX, x);
        bounds.maxY = std::max(bounds.maxY, y);
        bounds.valid = true;
    }
    return bounds;
}

float orient2d(ImVec2 a, ImVec2 b, ImVec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool pointOnSegment(ImVec2 a, ImVec2 b, ImVec2 p) {
    constexpr float epsilon = 1e-4f;
    return std::fabs(orient2d(a, b, p)) <= epsilon &&
           p.x >= std::min(a.x, b.x) - epsilon && p.x <= std::max(a.x, b.x) + epsilon &&
           p.y >= std::min(a.y, b.y) - epsilon && p.y <= std::max(a.y, b.y) + epsilon;
}

bool segmentsIntersect(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
    const float abC = orient2d(a, b, c);
    const float abD = orient2d(a, b, d);
    const float cdA = orient2d(c, d, a);
    const float cdB = orient2d(c, d, b);
    if (((abC > 0.0f && abD < 0.0f) || (abC < 0.0f && abD > 0.0f)) &&
        ((cdA > 0.0f && cdB < 0.0f) || (cdA < 0.0f && cdB > 0.0f))) return true;
    return pointOnSegment(a, b, c) || pointOnSegment(a, b, d) ||
           pointOnSegment(c, d, a) || pointOnSegment(c, d, b);
}

bool pointInTriangle(ImVec2 point, ImVec2 a, ImVec2 b, ImVec2 c) {
    const float ab = orient2d(a, b, point);
    const float bc = orient2d(b, c, point);
    const float ca = orient2d(c, a, point);
    const bool hasNegative = ab < -1e-4f || bc < -1e-4f || ca < -1e-4f;
    const bool hasPositive = ab > 1e-4f || bc > 1e-4f || ca > 1e-4f;
    return !(hasNegative && hasPositive);
}

bool triangleIntersectsRect(ImVec2 a, ImVec2 b, ImVec2 c,
                            float minX, float minY, float maxX, float maxY) {
    const float triangleMinX = std::min({a.x, b.x, c.x});
    const float triangleMaxX = std::max({a.x, b.x, c.x});
    const float triangleMinY = std::min({a.y, b.y, c.y});
    const float triangleMaxY = std::max({a.y, b.y, c.y});
    if (triangleMaxX < minX || triangleMinX > maxX ||
        triangleMaxY < minY || triangleMinY > maxY) return false;

    const auto inRect = [=](ImVec2 p) {
        return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
    };
    if (inRect(a) || inRect(b) || inRect(c)) return true;

    const ImVec2 corners[] = {{minX, minY}, {maxX, minY},
                              {maxX, maxY}, {minX, maxY}};
    for (const ImVec2 corner : corners)
        if (pointInTriangle(corner, a, b, c)) return true;

    const ImVec2 rectEdges[][2] = {{corners[0], corners[1]}, {corners[1], corners[2]},
                                   {corners[2], corners[3]}, {corners[3], corners[0]}};
    const ImVec2 triEdges[][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& triEdge : triEdges)
        for (const auto& rectEdge : rectEdges)
            if (segmentsIntersect(triEdge[0], triEdge[1], rectEdge[0], rectEdge[1])) return true;
    return false;
}

bool projectedGeometryCrossesRect(const DrawItem& item, const render::OrbitCamera& camera,
                                 const Rect& viewport, float minX, float minY,
                                 float maxX, float maxY) {
    if (!item.mesh || item.mesh->indices.size() < 3 || !viewport.valid()) return false;
    const std::size_t vertexCount = item.mesh->positions.size() / 3;
    std::vector<std::optional<ImVec2>> screenVertices(vertexCount);
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const std::size_t offset = vertex * 3;
        const Vec3 local{item.mesh->positions[offset], item.mesh->positions[offset + 1],
                         item.mesh->positions[offset + 2]};
        float x = 0.0f, y = 0.0f;
        if (camera.projectScreen(transformPoint(item.model, local), viewport.w, viewport.h, x, y))
            screenVertices[vertex] = ImVec2(x, y);
    }

    for (std::size_t index = 0; index + 2 < item.mesh->indices.size(); index += 3) {
        const u32 ia = item.mesh->indices[index];
        const u32 ib = item.mesh->indices[index + 1];
        const u32 ic = item.mesh->indices[index + 2];
        if (ia >= screenVertices.size() || ib >= screenVertices.size() ||
            ic >= screenVertices.size() || !screenVertices[ia] ||
            !screenVertices[ib] || !screenVertices[ic]) continue;
        if (triangleIntersectsRect(*screenVertices[ia], *screenVertices[ib],
                                   *screenVertices[ic], minX, minY, maxX, maxY)) return true;
    }
    return false;
}

std::vector<ObjectId> boxSelectCandidates(const scene::World& world,
                                          const render::OrbitCamera& camera,
                                          const Rect& viewport, const ImGuiIO& io,
                                          ImVec2 start, ImVec2 end) {
    const Pixel a = toFramebuffer(io, start);
    const Pixel b = toFramebuffer(io, end);
    const float minX = static_cast<float>(std::min(a.x, b.x) - viewport.x);
    const float minY = static_cast<float>(std::min(a.y, b.y) - viewport.y);
    const float maxX = static_cast<float>(std::max(a.x, b.x) - viewport.x);
    const float maxY = static_cast<float>(std::max(a.y, b.y) - viewport.y);
    const bool crossing = b.x < a.x;

    // buildPacket applies the exact visibility/collection and mesh filters used
    // by drawing, while keeping all the non-owning data within this call.
    const RenderPacket packet = world.buildPacket(viewport.w, viewport.h);
    std::vector<ObjectId> selected;
    for (const DrawItem& item : packet.items) {
        if (crossing) {
            if (projectedGeometryCrossesRect(item, camera, viewport, minX, minY, maxX, maxY))
                selected.push_back(item.object);
            continue;
        }
        const ScreenBounds bounds = projectedBounds(item, camera, viewport);
        const bool enclosed = bounds.valid && bounds.minX >= minX && bounds.maxX <= maxX &&
                              bounds.minY >= minY && bounds.maxY <= maxY;
        if (enclosed) selected.push_back(item.object);
    }
    return selected;
}

// Alt+click: deselect the first SELECTED object along the ray, looking through
// the front ones. Each step restarts the ray just past the last hit, so the
// search goes through what is in front and behind the cursor in turn.
void deselectThrough(scene::World& world, const render::OrbitCamera& camera, const ImGuiIO& io,
                     const Rect& viewport, ImVec2 p) {
    std::optional<Ray> ray = rayAt(camera, io, viewport, p);
    for (int step = 0; ray && step < 32; ++step) {
        const std::optional<scene::PickResult> hit = world.pick(*ray);
        if (!hit) return;
        if (isSelected(world, hit->object)) {
            world.deselect(hit->object);
            return;
        }
        ray->origin = hit->worldPoint + ray->direction * 1e-3f;
    }
}

// The move drag of an object under the Move tool. The object follows the mouse
// on the plane through its origin, facing the camera, and the whole drag is one
// undo entry (the first frame pushes, later frames continue it).
struct MoveDrag {
    bool started = false;     // a transform command is on the stack
    ObjectId id;
    Transform before;
    Vec3 pivot;               // object origin at grab
    Vec3 normal;              // view-facing plane normal
    Vec3 hit0;                // where the grab ray met that plane
};

Vec3 translationOf(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

bool beginMove(scene::World& world, const render::OrbitCamera& camera, const ImGuiIO& io,
               const Rect& viewport, ImVec2 start, ObjectId id, MoveDrag& drag) {
    const scene::Object* o = world.get(id);
    if (!o) return false;
    const std::optional<Ray> ray = rayAt(camera, io, viewport, start);
    if (!ray) return false;
    drag = MoveDrag{};
    drag.id = id;
    drag.before = o->transform;
    drag.pivot = translationOf(world.worldMatrix(id));
    drag.normal = normalize(camera.target() - camera.position());
    return planeHit(*ray, drag.pivot, drag.normal, drag.hit0);
}

void continueMove(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                  const ImGuiIO& io, const Rect& viewport, MoveDrag& drag) {
    const scene::Object* o = world.get(drag.id);
    const std::optional<Ray> ray = rayAt(camera, io, viewport, io.MousePos);
    if (!o || !ray) return;
    Vec3 hit;
    if (!planeHit(*ray, drag.pivot, drag.normal, hit)) return;

    Vec3 delta = hit - drag.hit0;
    if (io.KeyCtrl) {  // Ctrl: whole-unit steps
        delta = Vec3{std::round(delta.x), std::round(delta.y), std::round(delta.z)};
    }
    const Vec3 newWorld = drag.pivot + delta;
    Vec3 local = newWorld;
    if (o->parent.valid() && world.has(o->parent)) {
        local = transformPoint(inverse(world.worldMatrix(o->parent)), newWorld);
    }
    Transform next = drag.before;
    next.position = local;
    if (next == o->transform) return;
    undo.push(std::make_unique<TransformCommand>(world, drag.id, drag.before, next, drag.started));
    drag.started = true;
}

} // namespace

// Left-button precedence, first match wins:
//   1. a press on a gizmo handle is the gizmo's (handled before this function);
//   2. a press on an object with the Move tool, dragged: move that object;
//   3. a press on empty space, dragged: box select (any tool);
//   4. a release without movement: click-select, Shift adds, Alt deselects through,
//      a click on empty space clears (Shift keeps the selection).
// Selection is view state, so only the moves go through the undo stack.
void handleViewportPick(scene::World& world, const render::OrbitCamera& camera, UndoStack& undo,
                        Tool tool, bool viewportOwnsPointer, const Rect& viewport) {
    static bool armed = false;      // a press began in the viewport, not over ImGui
    static bool onObject = false;   // ...and it landed on an object
    static bool draggedDuringPress = false;
    static ObjectId pressedId;
    static bool moving = false;     // a Move-tool object drag is in progress
    static MoveDrag move;
    static ImVec2 start{};

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kDragThreshold = 4.0f;

    // WantCaptureMouse is true over the viewport Area itself because Area D is
    // an ImGui window. The caller supplies the editor's region-aware pointer
    // ownership test (viewport body, excluding Toolbar/Sidebar/popups).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && viewportOwnsPointer) {
        start = io.MousePos;
        const std::optional<scene::PickResult> hit = pickAt(world, camera, io, viewport, start);
        onObject = hit.has_value();
        pressedId = hit ? hit->object : ObjectId{};
        armed = true;
        moving = false;
        draggedDuringPress = false;
    }
    if (!armed) return;

    if (moving) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (move.started) undo.discardLast();
            moving = false;
            armed = false;
            draggedDuringPress = false;
            return;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            moving = false;
            armed = false;
            draggedDuringPress = false;
            return;
        }
        continueMove(world, undo, camera, io, viewport, move);
        return;
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, kDragThreshold))
        draggedDuringPress = true;
    const bool dragged = draggedDuringPress;
    if (dragged && onObject && tool == Tool::Move) {
        if (!isSelected(world, pressedId)) world.select(pressedId, false);
        moving = beginMove(world, camera, io, viewport, start, pressedId, move);
        if (moving) return;
    }

    const bool boxing = dragged && !onObject;
    if (boxing && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 boxMin(std::min(start.x, io.MousePos.x), std::min(start.y, io.MousePos.y));
        const ImVec2 boxMax(std::max(start.x, io.MousePos.x), std::max(start.y, io.MousePos.y));
        fg->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
        ImVec4 outline = ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg);
        outline.w = 1.0f;
        fg->AddRect(boxMin, boxMax, ImGui::GetColorU32(outline));
    }

    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    armed = false;
    draggedDuringPress = false;

    if (!dragged) {
        if (io.KeyAlt) {
            deselectThrough(world, camera, io, viewport, start);
            return;
        }
        const std::optional<scene::PickResult> hit = pickAt(world, camera, io, viewport, start);
        if (hit) {
            world.select(hit->object, io.KeyShift);
        } else if (!io.KeyShift) {
            world.clearSelection();
        }
        return;
    }
    if (!boxing) return;  // a drag from an object under a non-Move tool does nothing

    const std::vector<ObjectId> boxSelection =
        boxSelectCandidates(world, camera, viewport, io, start, io.MousePos);
    if (!io.KeyShift) world.clearSelection();
    for (ObjectId id : boxSelection) {
        if (!isSelected(world, id)) world.select(id, true);
    }
}

void handleViewportEditPick(scene::World& world, const render::OrbitCamera& camera,
                            SelectMode mode, bool viewportOwnsPointer,
                            const Rect& viewport, bool xray) {
    static bool armed = false;
    static ObjectId pressedObject;
    static int pressedElement = -1;
    static ImVec2 start{};
    const ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && viewportOwnsPointer) {
        pressedObject = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
        start = io.MousePos;
        pressedElement = pressedObject.valid()
            ? pickEditElement(world, camera, io, viewport, pressedObject, mode, xray, start)
            : -1;
        armed = pressedObject.valid();
    }
    if (!armed) return;
    if (!world.get(pressedObject) || !world.readableEdit(pressedObject)) {
        armed = false;
        return;
    }
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;

    armed = false;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) return;
    if (pressedElement < 0 && io.KeyShift) return;
    updateEditSelection(world, pressedObject, mode, pressedElement, io.KeyShift);
}

// Right click over an object opens its context menu. Right click is the modal
// transform's cancel while one runs; Editor only calls this when no modal is active.
// Set Origin and the shading items need scene support that does not exist yet, so
// they are listed disabled rather than hidden or silently doing nothing.
void handleViewportContext(scene::World& world, UndoStack& undo, const render::OrbitCamera& camera,
                           bool viewportOwnsPointer, const Rect& viewport) {
    static bool renameRequest = false;
    static ObjectId renameId;
    static char renameBuffer[128] = {};

    const ImGuiIO& io = ImGui::GetIO();
    if (viewportOwnsPointer && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const std::optional<scene::PickResult> hit = pickAt(world, camera, io, viewport, io.MousePos);
        if (hit) {
            if (!isSelected(world, hit->object)) world.select(hit->object, false);
            ImGui::OpenPopup("##viewportcontext");
        }
    }

    if (ImGui::BeginPopup("##viewportcontext")) {
        if (ImGui::MenuItem("Rename")) {
            renameId = world.selection().back();
            if (const scene::Object* o = world.get(renameId)) {
                std::snprintf(renameBuffer, sizeof renameBuffer, "%s", o->name.c_str());
            }
            renameRequest = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Duplicate")) duplicateCurrentSelection(world, undo);
        if (ImGui::MenuItem("Delete")) deleteCurrentSelection(world, undo);
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("Set Origin");
        ImGui::MenuItem("Shade Smooth");
        ImGui::MenuItem("Shade Flat");
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    if (renameRequest) {
        ImGui::OpenPopup("Rename##viewport");
        renameRequest = false;
    }
    if (ImGui::BeginPopupModal("Rename##viewport", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rename", renameBuffer, sizeof renameBuffer,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("OK")) {
            const scene::Object* o = world.get(renameId);
            const std::string typed = renameBuffer;
            if (o && !typed.empty() && typed != o->name) {
                undo.push(std::make_unique<RenameCommand>(world, renameId, o->name, typed));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace ember::editor
