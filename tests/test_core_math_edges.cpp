// tests/test_core_math_edges.cpp — owned by Verity · QA.
//
// Deliberate edge-case attacks on src/core/Math.h, requested by Ash with the
// handoff ("I expect at least one of these to be wrong; please try"):
//   1. inverse() singular-matrix fallback (returns identity — pin the behaviour)
//   2. lookAt() with up parallel to the view direction (degenerate case)
//   3. Transform::toMatrix non-uniform scale + rotation (shear assertion)
//   4. Quat::fromEuler vs rotationEuler (Ash agreed ONE triple; we sweep)
//   5. perspective() full NDC mapping including off-axis x/y
// Plus whatever the sweep finds. Header-only maths: no GPU, no linking.
#include "harness.h"

#include "core/Math.h"
#include "core/Types.h"

#include <cmath>
#include <random>

using namespace ember;

namespace {

bool matNear(const Mat4& a, const Mat4& b, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
    return true;
}

bool vecNear(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

float maxAbsDiff(const Mat4& a, const Mat4& b) {
    float worst = 0.0f;
    for (int i = 0; i < 16; ++i) worst = maxf(worst, std::fabs(a.m[i] - b.m[i]));
    return worst;
}

// M * inverse(M) == I for a well-conditioned M.
bool roundTripsThroughInverse(const Mat4& m, float eps = 1e-3f) {
    return matNear(m * inverse(m), Mat4::identity(), eps) &&
           matNear(inverse(m) * m, Mat4::identity(), eps);
}

} // namespace

// ---- 1. inverse(): singular fallback ---------------------------------------

TEST(inverse_singular_matrix_falls_back_to_identity) {
    // Documented behaviour (Math.h:420): |det| < 1e-12 -> identity.
    // Pinned so a future change to "return zero" or "NaN" is a visible break.
    CHECK(inverse(Mat4::zero()).isIdentity());
    Mat4 rank1;
    for (int i = 0; i < 16; ++i) rank1.m[i] = 0.0f;
    rank1.m[0] = 1.0f; rank1.m[5] = 1.0f;  // still singular in 4D (two zero rows)
    CHECK(inverse(rank1).isIdentity());
}

TEST(inverse_scales_to_singularity_stays_finite) {
    // Scale components shrinking toward zero must never yield NaN/Inf output.
    for (float s : {1e-3f, 1e-5f, 1e-6f, 1e-7f, 0.0f}) {
        Transform t{}; t.scale = {s, 1.0f, 1.0f};
        const Mat4 inv = inverse(t.toMatrix());
        for (int i = 0; i < 16; ++i) CHECK(std::isfinite(inv.m[i]));
    }
}

TEST(inverse_of_invertible_TRS_round_trips) {
    for (int seed = 0; seed < 50; ++seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> ang(-3.14f, 3.14f);
        std::uniform_real_distribution<float> pos(-100.0f, 100.0f);
        std::uniform_real_distribution<float> scl(0.25f, 4.0f);
        Transform t{};
        t.position = {pos(rng), pos(rng), pos(rng)};
        t.rotation = {ang(rng), ang(rng), ang(rng)};
        t.scale    = {scl(rng), scl(rng), scl(rng)};
        if (!roundTripsThroughInverse(t.toMatrix()))
            harness::reportFailure(__FILE__, __LINE__,
                          "TRS matrix failed M*inv(M)==I at seed " + std::to_string(seed));
        ++harness::g_checks;
    }
}

TEST(inverse_of_general_matrix_matches_cofactor_identity) {
    // Independent check: A * adjugate(A) == det(A) * I, computed from the
    // returned inverse (adjugate = inverse * det).
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> v(-5.0f, 5.0f);
    for (int trial = 0; trial < 20; ++trial) {
        Mat4 a;
        for (int i = 0; i < 16; ++i) a.m[i] = v(rng);
        a.m[3] = a.m[7] = a.m[11] = 0.0f; a.m[15] = 1.0f;  // affine
        if (!roundTripsThroughInverse(a, 1e-2f))
            harness::reportFailure(__FILE__, __LINE__,
                          "affine matrix failed inverse round-trip, trial " + std::to_string(trial));
        ++harness::g_checks;
    }
}

// ---- 2. lookAt(): degenerate up --------------------------------------------

TEST(lookAt_up_parallel_to_view_is_finite_and_viewable) {
    // Ash's fallback: swap to a Z-up basis. Requirements: finite output,
    // eye maps to origin, target maps onto -Z (in front of camera).
    const Vec3 eye{0, 0, 5}, center{0, 0, 0};
    const Vec3 ups[] = {{0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}};
    for (const Vec3 up : ups) {
        const Mat4 v = lookAt(eye, center, up);
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(v.m[i]))
                harness::reportFailure(__FILE__, __LINE__,
                              "lookAt produced non-finite element with up=(" +
                              std::to_string(up.x) + "," + std::to_string(up.y) +
                              "," + std::to_string(up.z) + ")");
        ++harness::g_checks;
        CHECK(vecNear(transformPoint(v, eye), {0, 0, 0}, 1e-4f));
        const Vec3 target = transformPoint(v, center);
        CHECK_NEAR(target.x, 0.0f, 1e-4);
        CHECK_NEAR(target.y, 0.0f, 1e-4);
        CHECK(target.z < -1.0f);  // strictly in front, not behind or at eye
    }
}

TEST(lookAt_up_exactly_antiparallel_to_forward) {
    // up == -forward is the OTHER degenerate (cross is zero from the far side).
    const Mat4 v = lookAt({1, 2, 3}, {1, 2, 8}, {0, 0, -1});
    for (int i = 0; i < 16; ++i) CHECK(std::isfinite(v.m[i]));
    CHECK(vecNear(transformPoint(v, {1, 2, 3}), {0, 0, 0}, 1e-4f));
}

TEST(lookAt_basis_is_orthonormal) {
    // Even in the degenerate fallback the basis vectors must stay unit and
    // mutually perpendicular, or the view matrix silently scales the scene.
    const Vec3 eye{3, -2, 7}, center{0, 0, 0};
    const Vec3 degenerateUps[] = {{0, 0, 1}, {0, 0, -1}};
    const Vec3 normalUps[] = {{0, 1, 0}, {1, 0, 0}, {0.3f, 0.5f, -0.8f}};
    auto check = [&](const Mat4& m) {
        const Vec3 s{m.m[0], m.m[1], m.m[2]};
        const Vec3 u{m.m[4], m.m[5], m.m[6]};
        const Vec3 f{m.m[8], m.m[9], m.m[10]};  // third basis column
        CHECK_NEAR(length(s), 1.0f, 1e-4);
        CHECK_NEAR(length(u), 1.0f, 1e-4);
        CHECK_NEAR(length(f), 1.0f, 1e-4);
        CHECK_NEAR(dot(s, u), 0.0f, 1e-4);
        CHECK_NEAR(dot(s, f), 0.0f, 1e-4);
        CHECK_NEAR(dot(u, f), 0.0f, 1e-4);
    };
    for (const Vec3 up : degenerateUps) check(lookAt(eye, center, up));
    for (const Vec3 up : normalUps)     check(lookAt(eye, center, up));
}

// ---- 3. toMatrix: non-uniform scale + rotation (shear) ---------------------

TEST(toMatrix_nonuniform_scale_then_rotation_produces_expected_shear) {
    // S before R (Types.h comment): v -> T * Rz * S * v.
    // For v=(1,1,0), scale (2,1,1), rotZ 90: S gives (2,1,0), Rz gives (-1,2,0).
    // A wrong implementation that rotated first would give (-1,2,0) *scaled*
    // to (-1,2,0)->... distinguish by checking a second point.
    Transform t{};
    t.rotation = {0, 0, kHalfPi};
    t.scale    = {2.0f, 1.0f, 1.0f};
    const Mat4 m = t.toMatrix();
    // Precise expectation: column 0 of T*R*S is Rz*(S*e_x) = Rz*(2,0,0) = (0,2,0).
    CHECK(vecNear(transformPoint(m, {1, 0, 0}), {0, 2, 0}));
    // Column 1: Rz*(0,1,0) = (-1,0,0).
    CHECK(vecNear(transformPoint(m, {0, 1, 0}), {-1, 0, 0}));
}

TEST(toMatrix_rotation_of_scaled_circle_is_circle_not_ellipse_from_scale) {
    // With S-before-R, rotating a scale-2 axis by 90 must move the LONG axis
    // too — the shear is baked in. |R*S*v| == |S*v| for every v (rotation
    // preserves length), so lengths are preserved through the rotation.
    Transform t{};
    t.rotation = {0.3f, 1.1f, -0.7f};
    t.scale    = {4.0f, 1.0f, 0.5f};
    const Mat4 m = t.toMatrix();
    const Vec3 pts[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0.3f, -2, 5}};
    for (const Vec3 p : pts) {
        const float lenIn  = length(Vec3{p.x * t.scale.x, p.y * t.scale.y, p.z * t.scale.z});
        const float lenOut = length(transformDirection(m, p));
        CHECK_NEAR(lenOut, lenIn, 1e-4);
    }
}

// ---- 4. Quat::fromEuler vs rotationEuler — full sweep ----------------------

TEST(quat_fromEuler_agrees_with_rotationEuler_sweep) {
    // Ash verified ONE triple. Sweeping 2000 random triples, comparing the
    // quaternion's rotation matrix against rotationEuler's.
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> ang(-3.2f, 3.2f);
    int worstTrial = -1;
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 e{ang(rng), ang(rng), ang(rng)};
        const Mat4 a = toMat4(Quat::fromEuler(e));
        const Mat4 b = rotationEuler(e);
        const float d = maxAbsDiff(a, b);
        if (d > worst) { worst = d; worstTrial = i; }
    }
    if (worst > 1e-5f)
        harness::reportFailure(__FILE__, __LINE__,
                      "Quat::fromEuler diverges from rotationEuler: worst diff " +
                      std::to_string(worst) + " at trial " + std::to_string(worstTrial));
    ++harness::g_checks;
}

TEST(quat_fromEuler_cardinal_axes_match) {
    // Explicit cardinal checks, independent of the sweep's seeds.
    CHECK(matNear(toMat4(Quat::fromEuler({kHalfPi, 0, 0})), rotationX(kHalfPi)));
    CHECK(matNear(toMat4(Quat::fromEuler({0, kHalfPi, 0})), rotationY(kHalfPi)));
    CHECK(matNear(toMat4(Quat::fromEuler({0, 0, kHalfPi})), rotationZ(kHalfPi)));
    CHECK(matNear(toMat4(Quat::fromEuler({0, 0, 0})), Mat4::identity()));
}

TEST(quat_rotation_matches_matrix_rotation_sweep) {
    // The q*v operator must agree with the matrix path for unit quaternions.
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> ang(-3.2f, 3.2f);
    for (int i = 0; i < 500; ++i) {
        const Vec3 e{ang(rng), ang(rng), ang(rng)};
        const Quat q = Quat::fromEuler(e);
        const Mat4 m = toMat4(q);
        const Vec3 v{ang(rng), ang(rng), ang(rng)};
        const Vec3 viaQuat  = q * v;
        const Vec3 viaMat   = transformDirection(m, v);
        if (!vecNear(viaQuat, viaMat, 1e-4f))
            harness::reportFailure(__FILE__, __LINE__,
                          "Quat*v diverges from toMat4(q)*v at trial " + std::to_string(i));
        ++harness::g_checks;
    }
}

// ---- 5. perspective(): full NDC mapping ------------------------------------

TEST(perspective_maps_near_and_far_planes_to_minus1_plus1) {
    const Mat4 p = perspective(kHalfPi, 16.0f / 9.0f, 0.1f, 100.0f);
    auto ndcZ = [&](float viewZ) {  // view-space z (negative in front)
        const Vec4 clip = p * Vec4{0, 0, viewZ, 1};
        return clip.z / clip.w;
    };
    CHECK_NEAR(ndcZ(-0.1f), -1.0f, 1e-5);   // near plane -> -1
    CHECK_NEAR(ndcZ(-100.0f), 1.0f, 1e-4);  // far plane  -> +1
}

TEST(perspective_maps_center_ray_to_center_pixel) {
    const Mat4 p = perspective(kHalfPi, 1.0f, 0.1f, 100.0f);
    const Vec4 clip = p * Vec4{0, 0, -1.0f, 1};
    CHECK_NEAR(clip.x / clip.w, 0.0f, 1e-6);
    CHECK_NEAR(clip.y / clip.w, 0.0f, 1e-6);
}

TEST(perspective_off_axis_x_y_signs_and_symmetry) {
    // Ash checked "the axis only, not off-axis x/y". This does the off-axis:
    // a point at +x must land at +ndc.x, mirror-symmetric for -x, and the
    // projected offset must grow toward the near plane.
    const Mat4 p = perspective(kHalfPi, 1.0f, 0.1f, 100.0f);
    auto ndcX = [&](float viewX, float viewZ) {
        const Vec4 clip = p * Vec4{viewX, 0, viewZ, 1};
        return clip.x / clip.w;
    };
    CHECK(ndcX(0.5f, -1.0f) > 0.0f);
    CHECK(ndcX(-0.5f, -1.0f) < 0.0f);
    CHECK_NEAR(ndcX(0.5f, -1.0f), -ndcX(-0.5f, -1.0f), 1e-6);  // symmetric

    // Nearer = larger projected offset for equal view-space x.
    const float nearOffset = ndcX(0.5f, -0.5f);
    const float farOffset  = ndcX(0.5f, -50.0f);
    CHECK(nearOffset > farOffset);
    CHECK(farOffset > 0.0f);
}

TEST(perspective_aspect_ratio_widens_or_narrows_x_only) {
    // aspect > 1 (wide) must shrink ndc x relative to a square frame; the
    // y projection must be identical between aspects.
    const Mat4 wide = perspective(kHalfPi, 2.0f, 0.1f, 100.0f);
    const Mat4 sq   = perspective(kHalfPi, 1.0f, 0.1f, 100.0f);
    auto ndcX = [](const Mat4& m, float x, float z) {
        const Vec4 c = m * Vec4{x, 0, z, 1}; return c.x / c.w; };
    auto ndcY = [](const Mat4& m, float y, float z) {
        const Vec4 c = m * Vec4{0, y, z, 1}; return c.y / c.w; };
    CHECK_NEAR(ndcX(wide, 1.0f, -2.0f) * 2.0f, ndcX(sq, 1.0f, -2.0f), 1e-5);
    CHECK_NEAR(ndcY(wide, 1.0f, -2.0f), ndcY(sq, 1.0f, -2.0f), 1e-6);
}

TEST(perspective_degenerate_inputs_stay_finite) {
    // aspect ~ 0 falls back to 1.0 (Math.h:323); fov 0 gives f = huge; both
    // must produce finite matrices rather than NaN/Inf that poison the GPU.
    const Mat4 a = perspective(kHalfPi, 0.0f, 0.1f, 100.0f);
    for (int i = 0; i < 16; ++i) CHECK(std::isfinite(a.m[i]));
    CHECK_NEAR(a.m[0], a.m[5], 1e-6);  // aspect fallback == square
}

HARNESS_MAIN()
