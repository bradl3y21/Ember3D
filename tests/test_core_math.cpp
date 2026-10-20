// tests/test_core_math.cpp — owned by Verity · QA.
//
// Contract under test: ARCHITECTURE.md §4.1 (core types) and tests/README.md
// ("Mat4 multiply/inverse, euler -> matrix, Transform::toMatrix").
// Pure math only: no GPU, no window, no engine state.
//
// Convention used throughout (documented here, and asked of Ash to pin in
// §4.1): Mat4 is column-major with column vectors, i.e. element (row r,
// col c) is m[c*4+r], and a transform maps v -> T * R * S * v.
// Multi-axis euler tests below assume Blender's default XYZ order, which for
// column vectors means R = Rz * Ry * Rx (X applied first). Single-axis tests
// are convention-independent and remain valid regardless.
#include "harness.h"

#include "core/Types.h"

using ember::Mat4;
using ember::Transform;
using ember::Vec3;

namespace {

// ---- local reference math (test-side; never touches src/) ------------------

constexpr float kPi = 3.14159265358979f;

Mat4 identity() {
    Mat4 m{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) m.m[c * 4 + r] = (r == c) ? 1.0f : 0.0f;
    return m;
}

// v -> M * v under the column-major/column-vector convention.
Vec3 apply(const Mat4& m, Vec3 v) {
    return Vec3{
        m.m[0] * v.x + m.m[4] * v.y + m.m[8]  * v.z + m.m[12],
        m.m[1] * v.x + m.m[5] * v.y + m.m[9]  * v.z + m.m[13],
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14],
    };
}

Mat4 refTranslation(Vec3 t) {
    Mat4 m = identity();
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

Mat4 refScale(Vec3 s) {
    Mat4 m = identity();
    m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z;
    return m;
}

// a * b under column-major/column-vector convention: (a*b)*v == a*(b*v).
Mat4 mul4(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

// Reference point application of R = Rz*Ry*Rx (X first), no matrices involved.
Vec3 applyReferenceEulerXyz(Vec3 v, Vec3 euler) {
    {  // Rx
        const float c = std::cos(euler.x), s = std::sin(euler.x);
        v = Vec3{v.x, c * v.y - s * v.z, s * v.y + c * v.z};
    }
    {  // Ry
        const float c = std::cos(euler.y), s = std::sin(euler.y);
        v = Vec3{c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
    }
    {  // Rz
        const float c = std::cos(euler.z), s = std::sin(euler.z);
        v = Vec3{c * v.x - s * v.y, s * v.x + c * v.y, v.z};
    }
    return v;
}

// Right-handed rotations, column vectors.
Mat4 refRotX(float a) {
    Mat4 m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m.m[5] = c; m.m[6] = s; m.m[9] = -s; m.m[10] = c;
    return m;
}
Mat4 refRotY(float a) {
    Mat4 m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c;
    return m;
}
Mat4 refRotZ(float a) {
    Mat4 m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c;
    return m;
}

bool vecNear(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

} // namespace

// ---- identity and defaults -------------------------------------------------

TEST(toMatrix_default_transform_is_identity) {
    Transform t{};  // position 0, rotation 0, scale 1 per §4.1 defaults
    Mat4 m = t.toMatrix();
    CHECK_MAT4_EQ(m, identity(), 1e-6f);
}

TEST(toMatrix_identity_maps_origin_to_origin) {
    CHECK(vecNear(apply(Transform{}.toMatrix(), {0, 0, 0}), {0, 0, 0}));
}

// ---- translation -----------------------------------------------------------

TEST(toMatrix_translation_only) {
    Transform t{};
    t.position = {3.0f, -4.0f, 5.5f};
    CHECK_MAT4_EQ(t.toMatrix(), refTranslation(t.position), 1e-6f);
}

TEST(toMatrix_translation_moves_origin_to_position) {
    Transform t{};
    t.position = {1.0f, 2.0f, 3.0f};
    CHECK(vecNear(apply(t.toMatrix(), {0, 0, 0}), {1, 2, 3}));
}

TEST(toMatrix_translation_is_uniform_over_points) {
    Transform t{};
    t.position = {7.0f, 0.0f, -2.0f};
    Vec3 p = apply(t.toMatrix(), {1.0f, 1.0f, 1.0f});
    CHECK(vecNear(p, {8.0f, 1.0f, -1.0f}));
}

// ---- scale -----------------------------------------------------------------

TEST(toMatrix_scale_only) {
    Transform t{};
    t.scale = {2.0f, 3.0f, 4.0f};
    CHECK_MAT4_EQ(t.toMatrix(), refScale(t.scale), 1e-6f);
}

TEST(toMatrix_scale_acts_about_origin) {
    Transform t{};
    t.scale = {2.0f, 2.0f, 2.0f};
    CHECK(vecNear(apply(t.toMatrix(), {1.0f, -3.0f, 0.5f}), {2.0f, -6.0f, 1.0f}));
}

TEST(toMatrix_unit_scale_is_identity_effect) {
    Transform t{};
    t.scale = {1.0f, 1.0f, 1.0f};
    CHECK_MAT4_EQ(t.toMatrix(), identity(), 1e-6f);
}

// ---- single-axis rotations (convention-independent) ------------------------

TEST(toMatrix_rotZ_90_maps_x_to_y) {
    Transform t{};
    t.rotation = {0.0f, 0.0f, kPi / 2.0f};
    CHECK(vecNear(apply(t.toMatrix(), {1, 0, 0}), {0, 1, 0}));
}

TEST(toMatrix_rotX_90_maps_y_to_z) {
    Transform t{};
    t.rotation = {kPi / 2.0f, 0.0f, 0.0f};
    CHECK(vecNear(apply(t.toMatrix(), {0, 1, 0}), {0, 0, 1}));
}

TEST(toMatrix_rotY_90_maps_x_to_minus_z) {
    Transform t{};
    t.rotation = {0.0f, kPi / 2.0f, 0.0f};
    CHECK(vecNear(apply(t.toMatrix(), {1, 0, 0}), {0, 0, -1}));
}

TEST(toMatrix_rotation_preserves_length) {
    Transform t{};
    t.rotation = {0.7f, -1.3f, 2.9f};
    const Vec3 v{0.3f, -1.7f, 4.2f};
    const Vec3 r = apply(t.toMatrix(), v);
    const float lenIn  = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const float lenOut = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    CHECK_NEAR(lenOut, lenIn, 1e-4);
}

TEST(toMatrix_rotation_fixes_origin) {
    Transform t{};
    t.rotation = {1.1f, 2.2f, -0.5f};
    CHECK(vecNear(apply(t.toMatrix(), {0, 0, 0}), {0, 0, 0}));
}

// ---- composition -----------------------------------------------------------

TEST(toMatrix_rotation_and_translation_scales_about_then_rotates_then_moves) {
    // v -> pos + R * (S * v): translation must NOT be scaled or rotated.
    Transform t{};
    t.position = {10.0f, 0.0f, 0.0f};
    t.rotation = {0.0f, 0.0f, kPi / 2.0f};
    t.scale    = {2.0f, 1.0f, 1.0f};
    // S*(1,0,0) = (2,0,0); Rz90 -> (0,2,0); +pos -> (10,2,0)
    CHECK(vecNear(apply(t.toMatrix(), {1, 0, 0}), {10.0f, 2.0f, 0.0f}));
}

TEST(toMatrix_matches_reference_TRS_product) {
    Transform t{};
    t.position = {1.5f, -2.5f, 0.25f};
    t.rotation = {0.4f, -0.9f, 1.7f};
    t.scale    = {2.0f, 0.5f, 3.0f};
    // Reference assembled column-major: M = T * R * S with R = Rz*Ry*Rx.
    const Mat4 ref = mul4(refTranslation(t.position),
                          mul4(refRotZ(t.rotation.z),
                               mul4(refRotY(t.rotation.y),
                                    mul4(refRotX(t.rotation.x),
                                         refScale(t.scale)))));
    CHECK_MAT4_EQ(t.toMatrix(), ref, 1e-5f);
}

TEST(toMatrix_composes_with_point_math_for_many_points) {
    Transform t{};
    t.position = {-1.0f, 4.0f, 2.0f};
    t.rotation = {0.3f, 1.2f, -2.1f};
    t.scale    = {0.5f, 2.0f, 1.5f};
    const Mat4 m = t.toMatrix();
    const Vec3 pts[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 2, 3}, {-4, 0.5f, 9}};
    for (const Vec3 p : pts) {
        // Manual TRS application, independent of any engine helper.
        Vec3 s{p.x * t.scale.x, p.y * t.scale.y, p.z * t.scale.z};
        const Vec3 r = applyReferenceEulerXyz(s, t.rotation);
        const Vec3 want{r.x + t.position.x, r.y + t.position.y, r.z + t.position.z};
        CHECK(vecNear(apply(m, p), want));
    }
}

TEST(toMatrix_zero_scale_squashes_axis) {
    Transform t{};
    t.scale = {0.0f, 1.0f, 1.0f};
    CHECK(vecNear(apply(t.toMatrix(), {5.0f, 3.0f, 2.0f}), {0.0f, 3.0f, 2.0f}));
}

HARNESS_MAIN()
