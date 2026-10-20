#include "editor/Gizmo.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace ember::editor {

namespace {

// Hover radius in window points; scaled to framebuffer pixels at the call site.
constexpr float kHoverRadiusPoints = 8.0f;
constexpr int kRingSegments = 64;

struct Px {
    float x = 0.0f;
    float y = 0.0f;
};

float segmentDistance(Px p, Px a, Px b) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a.x + vx * t - p.x;
    const float cy = a.y + vy * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

// Match the renderer's worldPerPixel calculation at the gizmo pivot. The
// renderer places a handle tip handlePixels * worldPerPixel world units from
// the pivot; hit-testing must project that same endpoint instead of scaling
// each foreshortened screen-space axis back up to handlePixels.
bool worldPerPixelAt(const render::OrbitCamera& camera, const Vec3& anchor,
                     int width, int height, float& out) {
    if (width <= 0 || height <= 0) return false;
    const Mat4 view = camera.view();
    const Mat4 projection = camera.projection(static_cast<float>(width) / static_cast<float>(height));
    const float viewZ = view.m[2] * anchor.x + view.m[6] * anchor.y +
                        view.m[10] * anchor.z + view.m[14];
    const float depth = -viewZ;
    if (projection.m[11] == -1.0f) {
        if (!(depth > 1e-4f)) return false;
        out = 2.0f * depth / (projection.m[5] * static_cast<float>(height));
    } else {
        out = 2.0f / (projection.m[5] * static_cast<float>(height));
    }
    return std::isfinite(out) && out > 0.0f;
}

void perpendicularBasis(Vec3 a, Vec3& u, Vec3& v) {
    const Vec3 helper = std::fabs(a.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    u = normalize(cross(a, helper));
    v = cross(a, u);
}

// Translate and scale handles: the segment from the pivot to the handle tip.
// Camera maths come from render (projectScreen), so drawn and clickable handles
// cannot drift apart.
int nearestAxis(const render::OrbitCamera& camera, int width, int height, Vec3 origin,
                const Mat4& orientation, float handlePixels, Px mouse, float radius) {
    Px p0{};
    if (!camera.projectScreen(origin, width, height, p0.x, p0.y)) return -1;
    float worldPerPixel = 0.0f;
    if (!worldPerPixelAt(camera, origin, width, height, worldPerPixel)) return -1;
    const float handleLengthWorld = handlePixels * worldPerPixel;
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < 3; ++i) {
        const Vec3 a{orientation.m[i * 4 + 0], orientation.m[i * 4 + 1], orientation.m[i * 4 + 2]};
        Px tip{};
        if (!camera.projectScreen(origin + a * handleLengthWorld, width, height, tip.x, tip.y)) continue;
        if (std::hypot(tip.x - p0.x, tip.y - p0.y) < 1e-3f) continue;
        const float dist = segmentDistance(mouse, p0, tip);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

// Rotate handles: a circle around each axis, handlePixels in radius on screen.
int nearestRing(const render::OrbitCamera& camera, int width, int height, Vec3 origin,
                const Mat4& orientation, float handlePixels, Px mouse, float radius) {
    Px c{};
    if (!camera.projectScreen(origin, width, height, c.x, c.y)) return -1;
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < 3; ++i) {
        const Vec3 axis{orientation.m[i * 4 + 0], orientation.m[i * 4 + 1], orientation.m[i * 4 + 2]};
        Vec3 u{};
        Vec3 v{};
        perpendicularBasis(axis, u, v);
        Px pu{};
        if (!camera.projectScreen(origin + u, width, height, pu.x, pu.y)) continue;
        const float perUnit = std::hypot(pu.x - c.x, pu.y - c.y);
        if (perUnit < 1e-3f) continue;
        const float ringRadius = handlePixels / perUnit;
        Px prev{};
        bool havePrev = false;
        for (int k = 0; k <= kRingSegments; ++k) {
            const float t = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(kRingSegments);
            Px cur{};
            if (!camera.projectScreen(origin + (u * std::cos(t) + v * std::sin(t)) * ringRadius,
                                      width, height, cur.x, cur.y)) {
                havePrev = false;
                continue;
            }
            if (havePrev) {
                const float dist = segmentDistance(mouse, prev, cur);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i;
                }
            }
            prev = cur;
            havePrev = true;
        }
    }
    return best;
}

} // namespace

bool GizmoController::update(scene::World& world, UndoStack& undo,
                             const render::OrbitCamera& camera, TransformSpace space,
                             bool visible, bool snapEnabled, PivotMode pivotMode,
                             InteractionMode interaction, SelectMode selectMode) {
    const ImGuiIO& io = ImGui::GetIO();

    // A handle-started drag is the modal machine running: forward everything.
    if (dragModal_.active()) {
        const bool busy = dragModal_.update(world, undo, camera, space, interaction,
                                            selectMode, snapEnabled, pivotMode);
        state_.activeAxis = dragModal_.constraintAxis();
        state_.snap = snapEnabled != io.KeyCtrl;
        return busy;
    }

    state_.activeAxis = -1;
    state_.hoveredAxis = -1;
    state_.mode = mode_;
    state_.snap = snapEnabled != io.KeyCtrl;

    const ObjectId id = world.selection().empty() ? ObjectId{} : world.selection().back();
    const scene::Object* o = world.get(id);
    if (!visible || !o) return false;

    const int width = static_cast<int>(std::lround(io.DisplaySize.x * io.DisplayFramebufferScale.x));
    const int height = static_cast<int>(std::lround(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    if (width <= 0 || height <= 0) return false;

    state_.origin = resolveTransformPivot(world, pivotMode, interaction, selectMode);
    state_.orientation = world.gizmoOrientation(id, space == TransformSpace::Local);

    if (io.WantCaptureMouse) return false;

    const Px mouse{io.MousePos.x * io.DisplayFramebufferScale.x,
                   io.MousePos.y * io.DisplayFramebufferScale.y};
    const float radius = kHoverRadiusPoints * io.DisplayFramebufferScale.x;
    const int axis = mode_ == GizmoState::Mode::Rotate
                         ? nearestRing(camera, width, height, state_.origin, state_.orientation,
                                       state_.handleLengthPixels, mouse, radius)
                         : nearestAxis(camera, width, height, state_.origin, state_.orientation,
                                       state_.handleLengthPixels, mouse, radius);
    state_.hoveredAxis = axis;

    // Grabbing a handle enters the SAME modal transform the keys start (item 99).
    if (axis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (dragModal_.beginHandleDrag(world, io, mode_, axis, space, pivotMode, state_.origin,
                                       interaction, selectMode)) {
            state_.activeAxis = axis;
            return true;
        }
    }
    return false;
}

void GizmoController::setMode(GizmoState::Mode mode) {
    if (!dragModal_.active()) mode_ = mode;
}

} // namespace ember::editor
