// Camera.h — the orbit camera with Blender navigation parity.
//
// Ownership: the camera type lives in render (decision D1/D2, ARCHITECTURE
// §4.3); all matrix building lives in Camera.cpp. The class is input-agnostic:
// `render` may depend only on `core`, so App polls platform::Input and feeds
// raw pixel deltas here. The renderer never reads input itself.
//
// Coordinate convention for everything that takes screen points (screenToRay,
// zoomToCursor): FRAMEBUFFER pixels, y measured DOWN from the top. The flip
// into NDC (y-up) happens inside this class — callers pass raw pixels and
// must not pre-flip. Window-vs-framebuffer scaling is the call site's job
// (D17: ImGui exposes io.DisplayFramebufferScale).
#pragma once

#include "core/Types.h"

namespace ember::render {

// Standard views, named after the side of the world the camera looks from
// (front = camera on +Z looking at -Z, matching the default orientation).
enum class StandardView { Front, Back, Left, Right, Top, Bottom };

// Orbits an eye position around a target point. Yaw/pitch in radians,
// distance in world units. Produces the view and projection matrices that
// App copies into RenderPacket.
class OrbitCamera {
public:
    OrbitCamera();

    // Feed pixel deltas from platform::Input::mouseDelta() while dragging
    // with the middle mouse (Blender convention) — drag right orbits right,
    // drag up orbits up.
    void orbit(float dxPixels, float dyPixels);

    // Feed platform::Input::scrollDelta().y — positive scrolls zoom in.
    // In orthographic mode this scales the ortho height, not the distance.
    void dolly(float scrollSteps);

    // Pan: slides the target in the VIEW PLANE (content follows the cursor,
    // grab-and-drag feel). dx/dy are pixel deltas (y-down, like the cursor);
    // viewportHeightPx is the framebuffer height used to convert pixels to
    // world units at target depth. Height <= 0 makes pan a no-op (caller bug
    // guard — never a silent scale surprise).
    void pan(float dxPixels, float dyPixels, float viewportHeightPx);

    // Scroll toward a screen point so the world point under the cursor stays
    // put (Blender behaviour). x,y are framebuffer pixels (y down); width/
    // height the framebuffer size — the same numbers the packet carries.
    // Degenerate sizes fall back to plain dolly.
    void zoomToCursor(float scrollSteps, int x, int y, int width, int height);

    void reset();                 // back to the default framing
    void setTarget(const Vec3& target);
    void setView(StandardView view);   // orientation change; framing kept

    const Vec3& target() const { return target_; }
    const Vec3& position() const { return eye_; }   // eye, world space
    float distance() const { return distance_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    // Projection mode. Orthographic scale is orthoHeight = the vertical world
    // extent visible at the target plane; horizontal = height * aspect.
    // Switching modes syncs orthoHeight to the perspective framing at the
    // current distance, so the picture does not jump.
    void setOrthographic(bool on);
    bool isOrthographic() const { return ortho_; }
    float orthoHeight() const { return orthoHeight_; }
    void setOrthoHeight(float worldUnits);

    // Fit a world-space box in view without changing orientation: target moves
    // to the box center; distance (perspective) or orthoHeight (orthographic)
    // grows so the bounding sphere of the box fits with a 15% margin. A
    // degenerate (empty) box is a no-op.
    void frameBounds(const Vec3& minCorner, const Vec3& maxCorner);

    // D26-requested sphere fit: the fitting maths lives in the camera, so
    // callers holding a center+radius (scene bounds, selection bounds) never
    // reconstruct a box from it — that used to leak a 1/sqrt(3) and a copy of
    // the margin into App. Identical fit to frameBounds' bounding-sphere rule.
    void frameSphere(const Vec3& center, float radius);

    // Right-handed, column-major (Mat4::m is column-major, see §4.1) —
    // directly consumable as GL uniformMatrix4fv arguments.
    Mat4 view() const;
    Mat4 projection(float aspect) const;            // aspect = width / height

    // Picking, the producer half (D16, ARCHITECTURE §4.3). World-space ray
    // through a screen point: origin at the eye (perspective) or on the ortho
    // near plane footprint (orthographic); direction NORMALIZED, exactly the
    // camera forward in orthographic mode. Degenerate sizes -> straight ahead.
    Ray screenToRay(int x, int y, int width, int height) const;

    // Item 82: screen projection — the EXACT inverse of screenToRay, in the
    // same px space (framebuffer pixels, origin top-left, y DOWN). This is
    // the helper the EDITOR hit-tests with (D20 keeps hit-testing in the
    // editor; nobody else may derive camera math, or drawn and clickable
    // handles drift). A gizmo handle is fully specified by data the editor
    // already holds: anchor = GizmoState::origin, tip = anchor +
    // orientation_column * handleLengthPixels * worldPerPixel — project both
    // with this call, then cursor-to-segment distance is pure screen math.
    // Returns false when the point does not project (at/behind the eye,
    // degenerate size, NaN); the caller must treat false as "no handle
    // here" — never clamp.
    bool projectScreen(const Vec3& worldPoint, int width, int height,
                       float& outX, float& outY) const;

    void setFOV(float fovYRadians);
    void setNearFar(float nearPlane, float farPlane);
    float fovY() const { return fovY_; }
    float nearPlane() const { return near_; }
    float farPlane() const { return far_; }

private:
    void updateEye();
    void syncOrthoHeight();   // orthoHeight = 2 * distance * tan(fovY/2)

    // Orthonormal world-space view basis: f = eye->target, s = right, u = up.
    // view(), screenToRay(), pan() and zoomToCursor() all derive from this
    // single source. At the exact poles (f parallel to world up) the
    // cross-product degenerates; there we pick canonical screen-right +X,
    // which is the continuous limit of the default (yaw=0) orbit — the roll
    // convention is: screen-up is -Z in Top view, +Z in Bottom view.
    struct Basis { Vec3 f, s, u; };
    Basis basis() const;

    Vec3  target_{0.0f, 0.0f, 0.0f};
    Vec3  eye_{0.0f, 0.0f, 0.0f};
    float distance_ = 6.0f;
    float yaw_      = 0.7853982f;   // 45 degrees
    float pitch_    = 0.61f;        // ~35 degrees above the horizon
    float fovY_     = 0.7853982f;   // 45 degrees
    float near_     = 0.05f;
    float far_      = 2000.0f;
    bool  ortho_    = false;
    float orthoHeight_ = 4.970564f; // synced from distance in the ctor
};

} // namespace ember::render
