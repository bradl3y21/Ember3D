// tests/test_render_camera.cpp — owned by Verity · QA.
//
// Contract under test: src/render/Camera.h (the M1 orbit camera), which
// ARCHITECTURE.md §6 lists as M1's core deliverable.
//
// NO GPU, NO GL CONTEXT: OrbitCamera is pure matrix math. It links ember_render
// (which links glad) but calls no GL entry point, so the suite still passes on
// a machine with no context — verified by running this binary outside any GL.
//
// The strongest checks here are DIFFERENTIAL: OrbitCamera::view() and
// ::projection() are independent reimplementations of core's lookAt() and
// perspective(). Two implementations of one contract must agree; if they
// diverge, one of them is wrong and both modules are suspect.
#include "harness.h"

#include "core/Math.h"
#include "render/Camera.h"

#include <cmath>

using namespace ember;
using ember::render::OrbitCamera;

namespace {

constexpr float kHalfPiLocal = 1.57079632679489661923f;

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

bool vecNear(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

bool finite16(const Mat4& m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m.m[i])) return false;
    return true;
}

// v -> M * v under the column-major/column-vector convention (§4.1).
Vec3 apply(const Mat4& m, Vec3 v) {
    return Vec3{
        m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
        m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14],
    };
}

// Reference eye position, transcribed from the Camera.h documented contract
// ("orbits an eye position around a target point"), NOT from Camera.cpp —
// an independent expectation for updateEye().
Vec3 expectedEye(Vec3 target, float distance, float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return Vec3{target.x + distance * cp * std::sin(yaw),
                target.y + distance * std::sin(pitch),
                target.z + distance * cp * std::cos(yaw)};
}

} // namespace

// ---- differential: view() vs core lookAt() ---------------------------------

TEST(camera_view_matches_core_lookAt_default) {
    // Two independent implementations of "eye looks at target, up = +Y".
    OrbitCamera c;
    const Mat4 theirs = c.view();
    const Mat4 mine   = lookAt(c.position(), c.target(), Vec3{0, 1, 0});
    CHECK(matNear(theirs, mine));
}

TEST(camera_view_matches_core_lookAt_after_orbit_and_target_moves) {
    OrbitCamera c;
    c.orbit(137.0f, -42.0f);
    c.setTarget({3.0f, -1.5f, 9.0f});
    c.orbit(-250.0f, 61.0f);
    const Mat4 theirs = c.view();
    const Mat4 mine   = lookAt(c.position(), c.target(), Vec3{0, 1, 0});
    CHECK(matNear(theirs, mine));
}

TEST(camera_view_maps_eye_to_origin_and_target_to_minus_z) {
    OrbitCamera c;
    c.orbit(311.0f, 77.0f);
    c.setTarget({-2.0f, 4.0f, 1.0f});
    const Mat4 v = c.view();
    CHECK(vecNear(apply(v, c.position()), {0, 0, 0}, 1e-4f));
    const Vec3 t = apply(v, c.target());
    CHECK_NEAR(t.x, 0.0f, 1e-4);
    CHECK_NEAR(t.y, 0.0f, 1e-4);
    CHECK_NEAR(t.z, -c.distance(), 1e-3);   // straight in front, at |distance|
}

TEST(camera_view_basis_is_orthonormal) {
    // A view matrix that scales or shears silently distorts the scene.
    OrbitCamera c;
    c.orbit(-90.0f, 220.0f);
    const Mat4 v = c.view();
    const Vec3 s{v.m[0], v.m[1], v.m[2]};
    const Vec3 u{v.m[4], v.m[5], v.m[6]};
    const Vec3 f{v.m[8], v.m[9], v.m[10]};
    CHECK_NEAR(length(s), 1.0f, 1e-4);
    CHECK_NEAR(length(u), 1.0f, 1e-4);
    CHECK_NEAR(length(f), 1.0f, 1e-4);
    CHECK_NEAR(dot(s, u), 0.0f, 1e-4);
    CHECK_NEAR(dot(s, f), 0.0f, 1e-4);
    CHECK_NEAR(dot(u, f), 0.0f, 1e-4);
}

// ---- differential: projection() vs core perspective() ----------------------

TEST(camera_projection_matches_core_perspective_across_aspects) {
    OrbitCamera c;
    for (float aspect : {0.5f, 1.0f, 16.0f / 9.0f, 4.0f}) {
        const Mat4 theirs = c.projection(aspect);
        const Mat4 mine   = perspective(c.fovY(), aspect, c.nearPlane(), c.farPlane());
        CHECK(matNear(theirs, mine));
    }
}

TEST(camera_projection_after_fov_and_nearfar_changes) {
    OrbitCamera c;
    c.setFOV(1.1f);
    c.setNearFar(0.25f, 500.0f);
    const Mat4 theirs = c.projection(2.0f);
    const Mat4 mine   = perspective(c.fovY(), 2.0f, c.nearPlane(), c.farPlane());
    CHECK(matNear(theirs, mine));
}

TEST(camera_projection_maps_near_far_to_minus1_plus1) {
    OrbitCamera c;
    const Mat4 p = c.projection(16.0f / 9.0f);
    auto ndcZ = [&](float viewZ) {
        const Vec4 clip = p * Vec4{0, 0, viewZ, 1};
        return clip.z / clip.w;
    };
    CHECK_NEAR(ndcZ(-c.nearPlane()), -1.0f, 1e-5);
    CHECK_NEAR(ndcZ(-c.farPlane()),   1.0f, 1e-4);
}

TEST(camera_projection_degenerate_aspect_falls_back_to_square) {
    OrbitCamera c;
    const Mat4 bad  = c.projection(0.0f);
    const Mat4 good = c.projection(1.0f);
    CHECK(matNear(bad, good));
    for (int i = 0; i < 16; ++i) CHECK(std::isfinite(bad.m[i]));
}

// ---- eye placement / orbit semantics ---------------------------------------

TEST(camera_default_framing_is_documented_values) {
    OrbitCamera c;
    CHECK_NEAR(c.distance(), 6.0f, 1e-6);
    CHECK_NEAR(c.yaw(),   0.7853982f, 1e-6);
    CHECK_NEAR(c.pitch(), 0.61f,      1e-6);
    CHECK_NEAR(c.fovY(),  0.7853982f, 1e-6);
    CHECK_NEAR(c.nearPlane(), 0.05f,  1e-6);
    CHECK_NEAR(c.farPlane(),  2000.0f, 1e-3);
    CHECK(vecNear(c.target(), {0, 0, 0}));
}

TEST(camera_eye_sits_at_distance_from_target) {
    OrbitCamera c;
    for (float dx : {0.0f, 55.0f, -310.0f}) {
        for (float dy : {0.0f, 90.0f, -220.0f}) {
            OrbitCamera t; t.orbit(dx, dy);
            CHECK_NEAR(distance(t.position(), t.target()), t.distance(), 1e-4);
        }
    }
}

TEST(camera_eye_matches_independent_expectation) {
    // updateEye() transcribed independently from the header's contract.
    OrbitCamera c;
    c.orbit(83.0f, -19.0f);
    c.setTarget({1.0f, 2.0f, 3.0f});
    const Vec3 want = expectedEye(c.target(), c.distance(), c.yaw(), c.pitch());
    CHECK(vecNear(c.position(), want, 1e-5f));
}

TEST(camera_yaw_zero_pitch_zero_places_eye_on_plus_z) {
    // Documented in Camera.cpp: "at yaw=0/pitch=0 the eye sits on +Z and looks
    // down -Z — the OpenGL right-handed default."
    OrbitCamera c;
    c.setTarget({0, 0, 0});
    // Drive yaw/pitch to 0 via orbit deltas from the known defaults.
    c.orbit(c.yaw() / 0.0087f, -c.pitch() / 0.0087f);
    CHECK_NEAR(c.yaw(),   0.0f, 1e-3);
    CHECK_NEAR(c.pitch(), 0.0f, 1e-3);
    CHECK(vecNear(c.position(), {0.0f, 0.0f, c.distance()}, 1e-2f));
}

TEST(camera_orbit_drag_right_orbits_right_and_drag_up_orbits_up) {
    // Documented sign convention: drag right orbits right (yaw decreases),
    // drag up orbits up (pitch increases).
    OrbitCamera c;
    const float yaw0 = c.yaw(), pitch0 = c.pitch();
    c.orbit(10.0f, 0.0f);
    CHECK(c.yaw() < yaw0);
    OrbitCamera d;
    d.orbit(0.0f, 10.0f);
    CHECK(d.pitch() > pitch0);
}

TEST(camera_pitch_clamps_just_short_of_the_poles) {
    // Camera.cpp clamps to ±(π/2 − 0.01) so the view never goes singular.
    OrbitCamera c;
    c.orbit(0.0f, 100000.0f);       // slam into the upper clamp
    const float limit = kHalfPiLocal - 0.01f;
    CHECK_NEAR(c.pitch(), limit, 1e-5);
    CHECK(finite16(c.view()));      // and the view stays well-formed there
    OrbitCamera d;
    d.orbit(0.0f, -100000.0f);      // slam into the lower clamp
    CHECK_NEAR(d.pitch(), -limit, 1e-5);
    CHECK(finite16(d.view()));
}

// ---- dolly / zoom semantics -------------------------------------------------

TEST(camera_dolly_positive_scrolls_in_and_negative_scrolls_out) {
    OrbitCamera c;
    const float far0 = c.distance();
    c.dolly(1.0f);                          // positive = zoom in
    CHECK(c.distance() < far0);
    const float in0 = c.distance();
    c.dolly(-1.0f);                         // negative = zoom out
    CHECK(c.distance() > in0);
}

TEST(camera_dolly_is_multiplicative_and_reversible) {
    // dolly(n) then dolly(-n) must return to the start (distance *= 1.1^-n).
    OrbitCamera c;
    const float start = c.distance();
    c.dolly(3.0f);
    c.dolly(-3.0f);
    CHECK_NEAR(c.distance(), start, 1e-3);
}

TEST(camera_dolly_clamps_to_documented_range) {
    OrbitCamera c;
    for (int i = 0; i < 200; ++i) c.dolly(1.0f);   // zoom all the way in
    CHECK_NEAR(c.distance(), 0.05f, 1e-6);
    OrbitCamera d;
    for (int i = 0; i < 200; ++i) d.dolly(-1.0f);  // zoom all the way out
    CHECK_NEAR(d.distance(), 1000.0f, 1e-3);
}

TEST(camera_dolly_keeps_eye_on_the_orbit_sphere) {
    OrbitCamera c;
    c.orbit(45.0f, -30.0f);
    c.dolly(7.0f);
    CHECK_NEAR(distance(c.position(), c.target()), c.distance(), 1e-4);
    CHECK(finite16(c.view()));
}

// ---- setters, validation, reset ---------------------------------------------

TEST(camera_setTarget_moves_eye_with_target_preserving_geometry) {
    OrbitCamera c;
    const float dist0 = c.distance();
    const float yaw0 = c.yaw(), pitch0 = c.pitch();
    const Vec3 eye0 = c.position();
    c.setTarget({10.0f, 0.0f, -5.0f});
    CHECK_NEAR(c.distance(), dist0, 1e-6);
    CHECK_NEAR(c.yaw(),   yaw0,   1e-6);
    CHECK_NEAR(c.pitch(), pitch0, 1e-6);
    // Eye translated by exactly the target delta.
    CHECK(vecNear(c.position(), eye0 + Vec3{10.0f, 0.0f, -5.0f}, 1e-4f));
}

TEST(camera_setFOV_rejects_out_of_range) {
    OrbitCamera c;
    const float before = c.fovY();
    c.setFOV(0.0f);      // too small
    CHECK_NEAR(c.fovY(), before, 1e-6);
    c.setFOV(3.5f);      // too large
    CHECK_NEAR(c.fovY(), before, 1e-6);
    c.setFOV(-1.0f);     // negative
    CHECK_NEAR(c.fovY(), before, 1e-6);
    c.setFOV(1.2f);      // in range (0.01, 3.0) — accepted
    CHECK_NEAR(c.fovY(), 1.2f, 1e-6);
}

TEST(camera_setNearFar_rejects_invalid_pairs) {
    OrbitCamera c;
    const float n0 = c.nearPlane(), f0 = c.farPlane();
    c.setNearFar(0.0f, 100.0f);      // near must be > 0
    CHECK_NEAR(c.nearPlane(), n0, 1e-6);
    CHECK_NEAR(c.farPlane(),  f0, 1e-6);
    c.setNearFar(10.0f, 5.0f);       // far must exceed near
    CHECK_NEAR(c.nearPlane(), n0, 1e-6);
    CHECK_NEAR(c.farPlane(),  f0, 1e-6);
    c.setNearFar(2.0f, 30.0f);       // valid
    CHECK_NEAR(c.nearPlane(), 2.0f,  1e-6);
    CHECK_NEAR(c.farPlane(),  30.0f, 1e-5);
}

TEST(camera_reset_restores_default_framing_after_mutation) {
    OrbitCamera c;
    c.orbit(999.0f, -400.0f);
    c.dolly(-50.0f);
    c.setTarget({7.0f, 7.0f, 7.0f});
    c.setFOV(1.9f);
    c.setNearFar(1.0f, 10.0f);
    c.reset();
    OrbitCamera def;      // a factory-default camera
    CHECK_NEAR(c.distance(), def.distance(), 1e-6);
    CHECK_NEAR(c.yaw(),   def.yaw(),   1e-6);
    CHECK_NEAR(c.pitch(), def.pitch(), 1e-6);
    CHECK_NEAR(c.fovY(),  def.fovY(),  1e-6);
    CHECK_NEAR(c.nearPlane(), def.nearPlane(), 1e-6);
    CHECK_NEAR(c.farPlane(),  def.farPlane(),  1e-3);
    CHECK(vecNear(c.target(), def.target()));
    CHECK(vecNear(c.position(), def.position(), 1e-5f));
}

// ---- robustness: no NaN/Inf escapes under stress ---------------------------

TEST(camera_view_and_projection_stay_finite_under_stress) {
    OrbitCamera c;
    const float dxs[] = {0.0f, 1e4f, -1e4f, 3.14159f, 12345.6f};
    const float dys[] = {0.0f, 1e4f, -1e4f, 1.5f, -999.9f};
    for (float dx : dxs)
        for (float dy : dys) {
            OrbitCamera t; t.orbit(dx, dy);
            CHECK(finite16(t.view()));
            CHECK(finite16(t.projection(16.0f / 9.0f)));
        }
}

// ============================================================================
// Camera NAVIGATION APIs (item 9's second half). These were verified to EXIST
// but had zero coverage when the wave-2 sweep flagged it — closing that gap
// here. Contract read from Camera.h + Camera.cpp before writing.
// ============================================================================

namespace {
// Project a world point to NDC using the camera's own view/projection.
// Returns false when the point is behind the eye (clip.w <= 0).
bool toNdc(const OrbitCamera& cam, Vec3 world, float& nx, float& ny) {
    const Mat4 vp = cam.projection(16.0f / 9.0f) * cam.view();
    const Vec4 clip = vp * Vec4(world, 1.0f);
    if (clip.w <= 1e-6f) return false;
    nx = clip.x / clip.w;
    ny = clip.y / clip.w;
    return true;
}

// The world point the cursor ray hits on the plane through `pivot` facing the
// camera. This is the point zoom-to-cursor promises to hold fixed.
Vec3 worldPointUnderCursor(const OrbitCamera& cam, float px, float py,
                           int w, int h, Vec3 pivot) {
    const Ray r = cam.screenToRay(static_cast<int>(px), static_cast<int>(py), w, h);
    // Forward = normalised eye->target.
    const Vec3 f = normalize(pivot - cam.position());
    const float denom = dot(r.direction, f);
    const float t = dot(pivot - r.origin, f) / denom;
    return r.origin + r.direction * t;
}
} // namespace

// ---- StandardView: each view points the eye the documented way --------------

TEST(setview_each_standard_view_points_the_expected_way) {
    // Camera.h's contract (updateEye): at yaw=0/pitch=0 the eye sits on +Z of
    // the target looking down -Z. Front matches that; the other views are the
    // documented yaw/pitch pairs. What must hold for ALL of them: the eye is
    // exactly `distance` from the target, and the view matrix sends the eye to
    // the origin and the target onto -Z.
    struct Case { ember::render::StandardView v; const char* name; };
    const Case cases[] = {
        {ember::render::StandardView::Front,  "Front"},
        {ember::render::StandardView::Back,   "Back"},
        {ember::render::StandardView::Left,   "Left"},
        {ember::render::StandardView::Right,  "Right"},
        {ember::render::StandardView::Top,    "Top"},
        {ember::render::StandardView::Bottom, "Bottom"},
    };
    const Vec3 target{1.5f, -2.0f, 3.0f};
    for (const Case& c : cases) {
        OrbitCamera cam;
        cam.setTarget(target);
        cam.setView(c.v);

        // Eye is at the documented distance from the target.
        if (std::fabs(distance(cam.position(), target) - cam.distance()) > 1e-4f)
            harness::reportFailure(__FILE__, __LINE__,
                std::string("setView(") + c.name + "): eye is not at distance() "
                "from the target");
        ++harness::g_checks;

        // View sends eye -> origin, target -> straight down -Z at |distance|.
        const Mat4 v = cam.view();
        CHECK(vecNear(apply(v, cam.position()), {0, 0, 0}, 1e-4f));
        const Vec3 t = apply(v, target);
        CHECK_NEAR(t.x, 0.0f, 1e-4);
        CHECK_NEAR(t.y, 0.0f, 1e-4);
        CHECK_NEAR(t.z, -cam.distance(), 1e-3);
    }
}

TEST(setview_front_eye_sits_on_plus_z) {
    // The one view with an exact documented position: Front == yaw 0/pitch 0,
    // so eye = target + (0,0,distance) (Camera.cpp updateEye).
    OrbitCamera cam;
    const Vec3 target{0, 0, 0};
    cam.setTarget(target);
    cam.setView(ember::render::StandardView::Front);
    CHECK(vecNear(cam.position(), {0.0f, 0.0f, cam.distance()}, 1e-4f));
    CHECK_NEAR(cam.yaw(),   0.0f, 1e-6);
    CHECK_NEAR(cam.pitch(), 0.0f, 1e-6);
}

TEST(setview_preserves_distance_and_target) {
    // Contract: "orientation change; framing kept".
    OrbitCamera cam;
    cam.setTarget({4.0f, 1.0f, -2.0f});
    cam.dolly(3.0f);
    const float distBefore = cam.distance();
    const Vec3 targetBefore = cam.target();
    cam.setView(ember::render::StandardView::Left);
    CHECK_NEAR(cam.distance(), distBefore, 1e-6);
    CHECK(vecNear(cam.target(), targetBefore));
}

// ---- frameBounds / frameSphere: fit a known box with the 15% margin --------

TEST(framebounds_targets_the_box_center) {
    OrbitCamera cam;
    const Vec3 lo{-2.0f, -1.0f, -4.0f};
    const Vec3 hi{ 6.0f,  3.0f,  0.0f};
    cam.frameBounds(lo, hi);
    CHECK(vecNear(cam.target(), {2.0f, 1.0f, -2.0f}, 1e-4f));   // midpoint
}

TEST(framebounds_fits_the_bounding_sphere_with_the_documented_margin) {
    // Camera.h: "the bounding sphere of the box fits with a 15% margin", and
    // Camera.cpp computes distance = radius * kMargin / tan(fovY/2) with
    // kMargin = 1.15. Assert the FORMULA, so a margin change is a visible break.
    const float kMargin = 1.15f;
    OrbitCamera cam;
    cam.setFOV(0.7853982f);   // 45 deg, the default
    const Vec3 lo{-1.0f, -1.0f, -1.0f};
    const Vec3 hi{ 1.0f,  1.0f,  1.0f};
    const float radius = 0.5f * std::sqrt(3.0f * 4.0f);   // half-diagonal

    cam.frameBounds(lo, hi);
    const float expected = radius * kMargin / std::tan(cam.fovY() * 0.5f);
    CHECK_NEAR(cam.distance(), expected, 1e-4);
}

TEST(framebounds_grows_distance_for_a_bigger_box) {
    // A strictly larger box must not be framed CLOSER — that would clip it.
    OrbitCamera small;
    small.frameBounds({-1, -1, -1}, {1, 1, 1});
    OrbitCamera large;
    large.frameBounds({-10, -10, -10}, {10, 10, 10});
    CHECK(large.distance() > small.distance());
    CHECK(large.target().x == 0.0f && large.target().y == 0.0f &&
          large.target().z == 0.0f);
}

TEST(framebounds_degenerate_box_is_a_noop) {
    // "A degenerate (empty) box is a no-op." Also NaN-safe: a zero-size box at
    // the origin must not poison distance with NaN.
    OrbitCamera cam;
    cam.setTarget({3.0f, 3.0f, 3.0f});
    const float distBefore = cam.distance();
    cam.frameBounds({3.0f, 3.0f, 3.0f}, {3.0f, 3.0f, 3.0f});   // empty box
    CHECK_NEAR(cam.distance(), distBefore, 1e-6);              // unchanged
    CHECK(vecNear(cam.target(), {3.0f, 3.0f, 3.0f}));          // unchanged
    CHECK(std::isfinite(cam.distance()));
}

TEST(framesphere_matches_framebounds_for_the_same_subject) {
    // Camera.h: frameSphere is "identical fit to frameBounds' bounding-sphere
    // rule", and frameBounds defers to it. So framing a box directly and
    // framing its own bounding sphere must land on the SAME distance.
    const Vec3 lo{-3.0f, -1.0f, 2.0f};
    const Vec3 hi{ 5.0f,  3.0f, 6.0f};
    const Vec3 center{1.0f, 1.0f, 4.0f};
    const float radius = 0.5f * std::sqrt(64.0f + 16.0f + 16.0f);

    OrbitCamera viaBox;
    viaBox.frameBounds(lo, hi);
    OrbitCamera viaSphere;
    viaSphere.frameSphere(center, radius);

    CHECK_NEAR(viaBox.distance(), viaSphere.distance(), 1e-4);
    CHECK(vecNear(viaBox.target(), viaSphere.target(), 1e-4f));
}

TEST(framesphere_degenerate_radius_is_a_noop_and_nan_safe) {
    OrbitCamera cam;
    cam.setTarget({1.0f, 2.0f, 3.0f});
    const float d0 = cam.distance();
    cam.frameSphere({0, 0, 0}, 0.0f);          // radius 0 -> no-op
    cam.frameSphere({0, 0, 0}, -5.0f);         // negative -> no-op
    CHECK_NEAR(cam.distance(), d0, 1e-6);
    CHECK(vecNear(cam.target(), {1.0f, 2.0f, 3.0f}));
    CHECK(std::isfinite(cam.distance()));
}

// ---- orthographic: sane and never divides by zero --------------------------

TEST(ortho_switch_syncs_height_to_the_perspective_framing) {
    // Camera.h: "Switching modes syncs orthoHeight to the perspective framing
    // at the current distance, so the picture does not jump."
    OrbitCamera cam;
    const float expected = 2.0f * cam.distance() * std::tan(cam.fovY() * 0.5f);
    CHECK(!cam.isOrthographic());
    cam.setOrthographic(true);
    CHECK(cam.isOrthographic());
    CHECK_NEAR(cam.orthoHeight(), expected, 1e-4);   // the sync
}

TEST(ortho_switch_back_to_perspective_keeps_distance) {
    // "Leaving ortho: distance_ never changed while in ortho, so perspective
    // framing resumes unchanged."
    OrbitCamera cam;
    const float d0 = cam.distance();
    cam.setOrthographic(true);
    cam.dolly(5.0f);                    // zooms orthoHeight, NOT distance
    CHECK_NEAR(cam.distance(), d0, 1e-6);
    cam.setOrthographic(false);
    CHECK(!cam.isOrthographic());
    CHECK_NEAR(cam.distance(), d0, 1e-6);
}

TEST(ortho_projection_never_divides_by_zero_across_extremes) {
    // The task's explicit ask: "the orthographic projection is sane and never
    // divides by zero". Sweep orthoHeight over its full clamped range AND
    // aspect over extremes (including degenerate) — every element must be
    // finite. This is the check that would catch a zero extent reaching 1/hw.
    OrbitCamera cam;
    cam.setOrthographic(true);
    for (float h : {0.01f, 0.5f, 4.97f, 100.0f, 10000.0f}) {      // kOrthoMin..Max
        cam.setOrthoHeight(h);
        for (float aspect : {0.0f, 0.01f, 1.0f, 16.0f / 9.0f, 100.0f}) {
            const Mat4 p = cam.projection(aspect);
            bool allFinite = true;
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(p.m[i])) allFinite = false;
            if (!allFinite)
                harness::reportFailure(__FILE__, __LINE__,
                    "ortho projection not finite at height=" + std::to_string(h) +
                    " aspect=" + std::to_string(aspect));
            ++harness::g_checks;
        }
    }
}

TEST(ortho_perspective_projection_also_stays_finite_for_degenerate_aspect) {
    // Same never-divides-by-zero guarantee on the perspective path.
    OrbitCamera cam;
    for (float aspect : {0.0f, -3.0f, 0.0001f, 1.0f, 500.0f}) {
        const Mat4 p = cam.projection(aspect);
        for (int i = 0; i < 16; ++i) CHECK(std::isfinite(p.m[i]));
    }
}

TEST(ortho_set_height_rejects_out_of_range) {
    OrbitCamera cam;
    cam.setOrthographic(true);
    const float h0 = cam.orthoHeight();
    cam.setOrthoHeight(0.0f);        // below kOrthoMin -> rejected
    CHECK_NEAR(cam.orthoHeight(), h0, 1e-6);
    cam.setOrthoHeight(1.0e9f);      // above kOrthoMax -> rejected
    CHECK_NEAR(cam.orthoHeight(), h0, 1e-6);
    cam.setOrthoHeight(20.0f);       // in range -> accepted
    CHECK_NEAR(cam.orthoHeight(), 20.0f, 1e-6);
}

// ---- zoom-to-cursor: the point under the cursor stays put ------------------

TEST(zoom_to_cursor_keeps_the_point_under_the_cursor_fixed) {
    // The task's explicit ask. Take a cursor position, find the world point it
    // lands on (on the target plane), zoom, then re-project that SAME world
    // point — it must land on the same NDC within tolerance. This is exactly
    // what the implementation's pivot-slide is for.
    const int w = 1280, h = 720;
    for (float px : {300.0f, 640.0f, 1000.0f}) {
        for (float py : {200.0f, 360.0f, 500.0f}) {
            OrbitCamera cam;
            cam.setView(ember::render::StandardView::Front);
            cam.orbit(120.0f, -45.0f);                    // a general orientation
            const Vec3 pivot = worldPointUnderCursor(cam, px, py, w, h, cam.target());

            float nx0 = 0.0f, ny0 = 0.0f, nx1 = 0.0f, ny1 = 0.0f;
            if (!toNdc(cam, pivot, nx0, ny0)) continue;    // behind eye: skip

            cam.zoomToCursor(1.0f, static_cast<int>(px), static_cast<int>(py), w, h);

            if (!toNdc(cam, pivot, nx1, ny1))
                harness::reportFailure(__FILE__, __LINE__,
                    "zoomToCursor moved the pivot behind the eye at (" +
                    std::to_string(px) + "," + std::to_string(py) + ")");
            else if (std::fabs(nx1 - nx0) > 1e-3f || std::fabs(ny1 - ny0) > 1e-3f)
                harness::reportFailure(__FILE__, __LINE__,
                    "zoomToCursor let the point under the cursor drift: NDC (" +
                    std::to_string(nx0) + "," + std::to_string(ny0) + ") -> (" +
                    std::to_string(nx1) + "," + std::to_string(ny1) + ")");
            ++harness::g_checks;
        }
    }
}

TEST(zoom_to_cursor_zooms_in_and_still_works_in_ortho) {
    // Positive steps zoom in (distance shrinks in perspective; orthoHeight
    // shrinks in orthographic) and the pivot-invariance holds in BOTH modes.
    const int w = 1280, h = 720;
    for (bool ortho : {false, true}) {
        OrbitCamera cam;
        if (ortho) cam.setOrthographic(true);
        cam.setView(ember::render::StandardView::Front);
        cam.orbit(-80.0f, 30.0f);
        const float px = 800.0f, py = 300.0f;
        const Vec3 pivot = worldPointUnderCursor(cam, px, py, w, h, cam.target());

        const float before = ortho ? cam.orthoHeight() : cam.distance();
        cam.zoomToCursor(2.0f, static_cast<int>(px), static_cast<int>(py), w, h);
        const float after = ortho ? cam.orthoHeight() : cam.distance();

        if (!(after < before))
            harness::reportFailure(__FILE__, __LINE__,
                std::string("zoomToCursor(+2) did not zoom in (ortho=") +
                (ortho ? "true" : "false") + "): " + std::to_string(before) +
                " -> " + std::to_string(after));
        ++harness::g_checks;
        CHECK(std::isfinite(after));
    }
}

TEST(zoom_to_cursor_degenerate_sizes_fall_back_to_plain_dolly) {
    // Camera.cpp: "if (width <= 0 || height <= 0) { dolly(scrollSteps); return; }"
    // Must not crash and must still zoom.
    OrbitCamera cam;
    const float d0 = cam.distance();
    cam.zoomToCursor(1.0f, 100, 100, 0, 720);    // width 0
    CHECK(cam.distance() < d0);
    const float d1 = cam.distance();
    cam.zoomToCursor(1.0f, 100, 100, 1280, 0);   // height 0
    CHECK(cam.distance() < d1);
    CHECK(std::isfinite(cam.distance()));
}

HARNESS_MAIN()
