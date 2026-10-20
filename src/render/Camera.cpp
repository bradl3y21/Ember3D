// Camera.cpp — orbit camera math with Blender navigation parity.
// All matrix building lives here (D1): the class owns view/projection and
// nothing else constructs them. See Camera.h for conventions.
#include "render/Camera.h"

#include <cmath>

namespace ember::render {

namespace {

constexpr float kHalfPi  = 1.570796326794896619f;
constexpr float kPi      = 3.141592653589793238f;
constexpr float kPitchLimit = kHalfPi - 0.01f;   // drag stops just short of poles

constexpr float kZoomStep   = 1.1f;   // per scroll notch
constexpr float kMargin     = 1.15f;  // frameBounds margin
constexpr float kOrthoMin   = 0.01f;
constexpr float kOrthoMax   = 10000.0f;

void normalize3(float& x, float& y, float& z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-8f) { x /= len; y /= len; z /= len; }
}

float clampf(const float v, const float lo, const float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float dot3(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

OrbitCamera::OrbitCamera() {
    updateEye();
    syncOrthoHeight();
}

// ------------------------------------------------------------------- orbit ---

void OrbitCamera::orbit(const float dxPixels, const float dyPixels) {
    // Blender-ish: ~0.5 deg per pixel horizontally.
    yaw_   -= dxPixels * 0.0087f;
    pitch_ += dyPixels * 0.0087f;
    if (pitch_ >  kPitchLimit) pitch_ =  kPitchLimit;
    if (pitch_ < -kPitchLimit) pitch_ = -kPitchLimit;
    updateEye();
}

void OrbitCamera::dolly(const float scrollSteps) {
    if (ortho_) {
        orthoHeight_ = clampf(orthoHeight_ * std::pow(kZoomStep, -scrollSteps),
                              kOrthoMin, kOrthoMax);
        return;   // eye stays; the view volume shrinks/grows
    }
    distance_ *= std::pow(kZoomStep, -scrollSteps);
    if (distance_ < 0.05f)  distance_ = 0.05f;
    if (distance_ > 1000.f) distance_ = 1000.f;
    updateEye();
}

// --------------------------------------------------------------------- pan ---

void OrbitCamera::pan(const float dxPixels, const float dyPixels,
                      const float viewportHeightPx) {
    if (viewportHeightPx <= 0.0f) return;   // documented no-op guard

    // World units per pixel at the target plane.
    const float worldPerPx =
        ortho_ ? orthoHeight_ / viewportHeightPx
               : (2.0f * distance_ * std::tan(fovY_ * 0.5f)) / viewportHeightPx;

    const Basis b = basis();
    // Content follows the cursor: dragging right (dx>0) moves the target
    // left (-S); dragging down (dy>0, y-down cursor) raises it (+U).
    target_.x += b.s.x * (-dxPixels * worldPerPx) + b.u.x * (dyPixels * worldPerPx);
    target_.y += b.s.y * (-dxPixels * worldPerPx) + b.u.y * (dyPixels * worldPerPx);
    target_.z += b.s.z * (-dxPixels * worldPerPx) + b.u.z * (dyPixels * worldPerPx);
    updateEye();
}

// ------------------------------------------------------------ zoom-to-cursor --

void OrbitCamera::zoomToCursor(const float scrollSteps, const int x, const int y,
                               const int width, const int height) {
    if (width <= 0 || height <= 0) { dolly(scrollSteps); return; }

    const Basis b = basis();
    const Ray r = screenToRay(x, y, width, height);

    // Intersect the cursor ray with the plane through the target, normal f.
    const float denom = dot3(r.direction, b.f);
    if (std::fabs(denom) < 1e-4f) { dolly(scrollSteps); return; }   // degenerate
    const Vec3 toTarget{target_.x - r.origin.x, target_.y - r.origin.y,
                        target_.z - r.origin.z};
    const float t = dot3(toTarget, b.f) / denom;
    if (t < 0.0f) { dolly(scrollSteps); return; }   // plane behind camera

    const Vec3 pivot{r.origin.x + r.direction.x * t,
                     r.origin.y + r.direction.y * t,
                     r.origin.z + r.direction.z * t};

    const float k = std::pow(kZoomStep, -scrollSteps);
    float appliedK = 1.0f;
    if (ortho_) {
        const float newH = clampf(orthoHeight_ * k, kOrthoMin, kOrthoMax);
        appliedK = newH / orthoHeight_;
        orthoHeight_ = newH;
    } else {
        const float newD = clampf(distance_ * k, 0.05f, 1000.0f);
        appliedK = newD / distance_;
        distance_ = newD;
    }

    // Keep the world point under the cursor on the same pixel: the target
    // slides toward the pivot by the same factor the view shrank.
    target_.x = pivot.x + (target_.x - pivot.x) * appliedK;
    target_.y = pivot.y + (target_.y - pivot.y) * appliedK;
    target_.z = pivot.z + (target_.z - pivot.z) * appliedK;
    updateEye();
}

// ------------------------------------------------------------- views/framing --

void OrbitCamera::setFOV(const float fovYRadians) {
    if (fovYRadians > 0.01f && fovYRadians < 3.0f) fovY_ = fovYRadians;
}

void OrbitCamera::setNearFar(const float nearPlane, const float farPlane) {
    if (nearPlane > 0.0f && farPlane > nearPlane) {
        near_ = nearPlane;
        far_  = farPlane;
    }
}

void OrbitCamera::reset() {
    target_ = {0.0f, 0.0f, 0.0f};
    distance_ = 6.0f;
    yaw_   = 0.7853982f;  // 45 degrees
    pitch_ = 0.61f;       // ~35 degrees above the horizon
    fovY_  = 0.7853982f;  // 45 degrees
    near_  = 0.05f;
    far_   = 2000.0f;
    ortho_ = false;
    updateEye();
    syncOrthoHeight();
}

void OrbitCamera::setTarget(const Vec3& target) {
    target_ = target;
    updateEye();
}

void OrbitCamera::setView(const StandardView view) {
    // Orientation only; distance / orthoHeight / target are kept.
    // Front matches the default direction (+Z eye side, yaw 0). The exact
    // poles bypass the drag clamp on purpose - basis() is pole-safe.
    pitch_ = 0.0f;
    switch (view) {
    case StandardView::Front:  yaw_ = 0.0f;      break;
    case StandardView::Back:   yaw_ = kPi;       break;
    case StandardView::Right:  yaw_ =  kHalfPi;  break;   // eye on +X
    case StandardView::Left:   yaw_ = -kHalfPi;  break;   // eye on -X
    case StandardView::Top:    yaw_ = 0.0f; pitch_ =  kHalfPi; break;
    case StandardView::Bottom: yaw_ = 0.0f; pitch_ = -kHalfPi; break;
    }
    updateEye();
}

void OrbitCamera::setOrthographic(const bool on) {
    if (on == ortho_) return;
    ortho_ = on;
    if (on) syncOrthoHeight();   // seamless handoff from perspective framing
    // Leaving ortho: distance_ never changed while in ortho, so perspective
    // framing resumes unchanged.
}

void OrbitCamera::setOrthoHeight(const float worldUnits) {
    if (worldUnits >= kOrthoMin && worldUnits <= kOrthoMax) orthoHeight_ = worldUnits;
}

void OrbitCamera::frameSphere(const Vec3& center, const float radius) {
    // The ONE fit rule (see header): callers holding a center+radius use this
    // directly; frameBounds computes the box's bounding sphere and defers here.
    // A 15% margin so the framed subject never touches the viewport edge.
    if (!(radius > 0.0f)) return;   // degenerate: no-op (also NaN-safe)
    target_ = center;
    if (ortho_) {
        orthoHeight_ = clampf(2.0f * radius * kMargin, kOrthoMin, kOrthoMax);
    } else {
        const float needed = radius * kMargin / std::tan(fovY_ * 0.5f);
        distance_ = clampf(needed, 0.05f, 1000.0f);
    }
    updateEye();
}

void OrbitCamera::frameBounds(const Vec3& minCorner, const Vec3& maxCorner) {
    const Vec3 center{
        0.5f * (minCorner.x + maxCorner.x),
        0.5f * (minCorner.y + maxCorner.y),
        0.5f * (minCorner.z + maxCorner.z)};
    const float dx = maxCorner.x - minCorner.x;
    const float dy = maxCorner.y - minCorner.y;
    const float dz = maxCorner.z - minCorner.z;
    const float halfDiagonal =
        0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    frameSphere(center, halfDiagonal);   // shared fit; degenerate box no-ops there
}

// ------------------------------------------------------------------ internals --

void OrbitCamera::syncOrthoHeight() {
    orthoHeight_ = clampf(2.0f * distance_ * std::tan(fovY_ * 0.5f),
                          kOrthoMin, kOrthoMax);
}

void OrbitCamera::updateEye() {
    // Eye orbits the target on a sphere. At yaw=0/pitch=0 the eye sits on
    // +Z and looks down -Z — the OpenGL right-handed default.
    const float cp = std::cos(pitch_);
    eye_.x = target_.x + distance_ * cp * std::sin(yaw_);
    eye_.y = target_.y + distance_ * std::sin(pitch_);
    eye_.z = target_.z + distance_ * cp * std::cos(yaw_);
}

OrbitCamera::Basis OrbitCamera::basis() const {
    float fx = target_.x - eye_.x;
    float fy = target_.y - eye_.y;
    float fz = target_.z - eye_.z;
    normalize3(fx, fy, fz);

    // s = normalize(f x up), up = (0,1,0)  => (-fz, 0, fx) normalized.
    // At the exact poles (fx = fz = 0) that vector is zero; the canonical
    // fallback +X is the continuous limit of approaching from the default
    // yaw — Top shows screen-up = -Z, Bottom = +Z (documented roll).
    float sx = -fz;
    float sy = 0.0f;
    float sz = fx;
    if (sx * sx + sz * sz < 1e-12f) {
        sx = 1.0f; sy = 0.0f; sz = 0.0f;
    } else {
        normalize3(sx, sy, sz);
    }

    // u = s x f (unit: f, s orthonormal)
    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    return Basis{{fx, fy, fz}, {sx, sy, sz}, {ux, uy, uz}};
}

Mat4 OrbitCamera::view() const {
    // lookAt written into a column-major Mat4 (m[col*4 + row]) from the ONE
    // basis every screen-space function also uses — view, screenToRay, pan
    // and zoomToCursor cannot drift.
    const Basis b = basis();

    Mat4 m = Mat4::zero();   // NOT Mat4{} — that is the IDENTITY (Math.h NSDMI); m[15] must be 0 for clip.w = -viewZ
    m.m[0]  =  b.s.x; m.m[4]  =  b.s.y; m.m[8]  =  b.s.z;
    m.m[1]  =  b.u.x; m.m[5]  =  b.u.y; m.m[9]  =  b.u.z;
    m.m[2]  = -b.f.x; m.m[6]  = -b.f.y; m.m[10] = -b.f.z;
    m.m[12] = -(b.s.x * eye_.x + b.s.y * eye_.y + b.s.z * eye_.z);
    m.m[13] = -(b.u.x * eye_.x + b.u.y * eye_.y + b.u.z * eye_.z);
    m.m[14] =  (b.f.x * eye_.x + b.f.y * eye_.y + b.f.z * eye_.z);
    m.m[15] = 1.0f;
    return m;
}

Mat4 OrbitCamera::projection(const float aspect) const {
    const float a = aspect > 0.0001f ? aspect : 1.0f;

    Mat4 m = Mat4::zero();   // NOT Mat4{} — see view(); projections are zero-based
    if (ortho_) {
        // Ortho volume: vertical extent = orthoHeight_, horizontal = * aspect,
        // depth range stays [near, far] in front of the eye (GL convention:
        // view z in [-far, -near] maps to [-1, 1]).
        const float hh = 0.5f * orthoHeight_;
        const float hw = hh * a;
        m.m[0]  = 1.0f / hw;              // 2 / (r - l), r = +hw, l = -hw
        m.m[5]  = 1.0f / hh;              // 2 / (t - b)
        m.m[10] = -2.0f / (far_ - near_);
        m.m[14] = -(far_ + near_) / (far_ - near_);
        m.m[15] = 1.0f;
        return m;
    }

    const float f = 1.0f / std::tan(fovY_ * 0.5f);
    m.m[0]  = f / a;
    m.m[5]  = f;
    m.m[10] = (far_ + near_) / (near_ - far_);
    m.m[11] = -1.0f;
    m.m[14] = (2.0f * far_ * near_) / (near_ - far_);
    return m;
}

Ray OrbitCamera::screenToRay(const int x, const int y, const int width, const int height) const {
    // D16 producer half (ARCHITECTURE §4.3). x,y are FRAMEBUFFER pixels with
    // y measured DOWN from the top; the flip to NDC-up happens HERE, in one
    // place — callers pass raw pixels and must NOT pre-flip.
    Ray ray;
    ray.origin = eye_;

    if (width <= 0 || height <= 0) {
        ray.direction = basis().f;   // documented degenerate case: straight ahead
        return ray;
    }

    // Pixel CENTER convention: (x + 0.5) so the middle of the framebuffer
    // maps to ndc 0 exactly, and half-pixels cannot round into the wrong
    // neighbouring pixel at the edges.
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const float ndcX = (2.0f * (static_cast<float>(x) + 0.5f)) / fw - 1.0f;
    const float ndcY = 1.0f - (2.0f * (static_cast<float>(y) + 0.5f)) / fh;   // the flip

    const Basis b = basis();

    if (ortho_) {
        // Parallel rays: direction is always the camera forward; the origin
        // slides over the near-plane footprint of the ortho volume.
        const float hh = 0.5f * orthoHeight_;
        const float hw = hh * (fw / fh);
        ray.origin.x = eye_.x + b.s.x * (ndcX * hw) + b.u.x * (ndcY * hh);
        ray.origin.y = eye_.y + b.s.y * (ndcX * hw) + b.u.y * (ndcY * hh);
        ray.origin.z = eye_.z + b.s.z * (ndcX * hw) + b.u.z * (ndcY * hh);
        ray.direction = b.f;         // already unit length
        return ray;
    }

    // Perspective: view-space ray ( ndcX*tan*aspect, ndcY*tan, -1 ), and the
    // view -Z axis is world forward F: dir_world = a*S + b*U + F.
    const float tanHalf = std::tan(fovY_ * 0.5f);
    const float a = ndcX * tanHalf * (fw / fh);
    const float u = ndcY * tanHalf;

    float dx = b.f.x + b.s.x * a + b.u.x * u;
    float dy = b.f.y + b.s.y * a + b.u.y * u;
    float dz = b.f.z + b.s.z * a + b.u.z * u;
    normalize3(dx, dy, dz);
    ray.direction = {dx, dy, dz};
    return ray;
}

// Item 82: screen projection — the exact inverse of screenToRay's math, in the
// same px space (top-left origin, y down). Multiply clip = P * V * p by hand:
// no matrix multiply exists for Mat4 yet and this one product does not need
// one — column-major m[col*4+row] and four dots per output row.
bool OrbitCamera::projectScreen(const Vec3& worldPoint, const int width, const int height,
                                float& outX, float& outY) const {
    if (width <= 0 || height <= 0) return false;
    const Mat4 V = view();
    const Mat4 P = projection(static_cast<float>(width) / static_cast<float>(height));
    const float v[4] = {worldPoint.x, worldPoint.y, worldPoint.z, 1.0f};
    float ev[4];
    for (int r = 0; r < 4; ++r)
        ev[r] = V.m[r] * v[0] + V.m[4 + r] * v[1] + V.m[8 + r] * v[2] + V.m[12 + r] * v[3];
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = P.m[r] * ev[0] + P.m[4 + r] * ev[1] + P.m[8 + r] * ev[2] + P.m[12 + r] * ev[3];
    if (!(clip[3] > 1e-6f)) return false;            // at/behind the eye
    const float invW = 1.0f / clip[3];
    const float ndcX = clip[0] * invW, ndcY = clip[1] * invW;
    if (!(ndcX == ndcX) || !(ndcY == ndcY)) return false;   // NaN guard
    outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    outY = (0.5f - ndcY * 0.5f) * static_cast<float>(height);   // y-DOWN, like screenToRay's input
    return true;
}

} // namespace ember::render
