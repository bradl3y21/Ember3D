// Ember3D — core math vocabulary.
//
// Owner: Ash · Lead.  Contract: ARCHITECTURE.md §4.1 (Vec2/3/4, Mat4, Quat, Color).
//
// Header-only on purpose: `core` is the bottom of the dependency graph and the
// tests must be able to use this with no GPU and no linking. Everything here is
// `inline`/`constexpr`; nothing needs a .cpp.
//
// Conventions (Blender `BLI_math`-ish, right-handed, Y-up, OpenGL depth range):
//   * Mat4 is COLUMN-MAJOR and stored as float m[16], so m[col * 4 + row].
//   * Matrix * vector multiplies as M * v (column vector on the right).
//   * Euler angles are in RADIANS, XYZ order: R = Rz * Ry * Rx.
//   * Angles passed to rotateX/Y/Z and to Quat::fromAxisAngle are radians.

#pragma once

#include <cmath>
#include <cstdint>

namespace ember {

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------

inline constexpr float kPi        = 3.14159265358979323846f;
inline constexpr float kTwoPi     = 6.28318530717958647692f;
inline constexpr float kHalfPi    = 1.57079632679489661923f;
inline constexpr float kEpsilon   = 1.19209290e-07f;   // FLT_EPSILON
inline constexpr float kDegToRad  = kPi / 180.0f;
inline constexpr float kRadToDeg  = 180.0f / kPi;

inline constexpr float radians(float degrees) { return degrees * kDegToRad; }
inline constexpr float degrees(float radians) { return radians * kRadToDeg; }

// Hand-rolled so <windows.h> min/max macros can never break this header.
inline constexpr float minf(float a, float b) { return a < b ? a : b; }
inline constexpr float maxf(float a, float b) { return a > b ? a : b; }
inline constexpr float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline constexpr float lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline constexpr int imin(int a, int b) { return a < b ? a : b; }
inline constexpr int imax(int a, int b) { return a > b ? a : b; }
inline constexpr int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool nearlyEqual(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

inline float invSqrt(float x) { return x > 0.0f ? 1.0f / std::sqrt(x) : 0.0f; }

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------

struct Vec2 {
  float x = 0.0f, y = 0.0f;

  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

  constexpr float&       operator[](int i)       { return (&x)[i]; }
  constexpr const float& operator[](int i) const { return (&x)[i]; }
};

inline constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
inline constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline constexpr Vec2 operator*(float s, Vec2 a) { return {a.x * s, a.y * s}; }
inline constexpr Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }

inline constexpr Vec2& operator+=(Vec2& a, Vec2 b) { a.x += b.x; a.y += b.y; return a; }
inline constexpr Vec2& operator-=(Vec2& a, Vec2 b) { a.x -= b.x; a.y -= b.y; return a; }
inline constexpr Vec2& operator*=(Vec2& a, float s) { a.x *= s; a.y *= s; return a; }

inline constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
inline constexpr bool operator!=(Vec2 a, Vec2 b) { return !(a == b); }

inline constexpr float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline constexpr float lengthSq(Vec2 a) { return dot(a, a); }
inline float length(Vec2 a) { return std::sqrt(lengthSq(a)); }
inline constexpr float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 normalize(Vec2 a) { const float len = length(a); return len > kEpsilon ? a / len : Vec2{}; }

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;

  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}

  constexpr float&       operator[](int i)       { return (&x)[i]; }
  constexpr const float& operator[](int i) const { return (&x)[i]; }

  static constexpr Vec3 zero()     { return {0.0f, 0.0f, 0.0f}; }
  static constexpr Vec3 one()      { return {1.0f, 1.0f, 1.0f}; }
  static constexpr Vec3 unitX()    { return {1.0f, 0.0f, 0.0f}; }
  static constexpr Vec3 unitY()    { return {0.0f, 1.0f, 0.0f}; }
  static constexpr Vec3 unitZ()    { return {0.0f, 0.0f, 1.0f}; }
};

inline constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline constexpr Vec3 operator*(float s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline constexpr Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline constexpr Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline constexpr Vec3 operator/(Vec3 a, Vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }

inline constexpr Vec3& operator+=(Vec3& a, Vec3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline constexpr Vec3& operator-=(Vec3& a, Vec3 b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
inline constexpr Vec3& operator*=(Vec3& a, float s) { a.x *= s; a.y *= s; a.z *= s; return a; }
inline constexpr Vec3& operator/=(Vec3& a, float s) { a.x /= s; a.y /= s; a.z /= s; return a; }

inline constexpr bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline constexpr bool operator!=(Vec3 a, Vec3 b) { return !(a == b); }

inline constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline constexpr Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline constexpr float lengthSq(Vec3 a) { return dot(a, a); }
inline float length(Vec3 a) { return std::sqrt(lengthSq(a)); }
inline float distance(Vec3 a, Vec3 b) { return length(a - b); }
inline Vec3 normalize(Vec3 a) { const float len = length(a); return len > kEpsilon ? a / len : Vec3{}; }
inline constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline constexpr Vec3 minv(Vec3 a, Vec3 b) { return {minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z)}; }
inline constexpr Vec3 maxv(Vec3 a, Vec3 b) { return {maxf(a.x, b.x), maxf(a.y, b.y), maxf(a.z, b.z)}; }
inline constexpr Vec3 reflect(Vec3 v, Vec3 n) { return v - n * (2.0f * dot(v, n)); }

// ---------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------

struct Vec4 {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

  constexpr Vec4() = default;
  constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

  constexpr float&       operator[](int i)       { return (&x)[i]; }
  constexpr const float& operator[](int i) const { return (&x)[i]; }

  constexpr Vec3 xyz() const { return {x, y, z}; }
};

inline constexpr Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline constexpr Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline constexpr Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline constexpr Vec4 operator*(float s, Vec4 a) { return a * s; }

inline constexpr bool operator==(Vec4 a, Vec4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
inline constexpr bool operator!=(Vec4 a, Vec4 b) { return !(a == b); }

inline constexpr float dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline constexpr float lengthSq(Vec4 a) { return dot(a, a); }
inline float length(Vec4 a) { return std::sqrt(lengthSq(a)); }
inline Vec4 normalize(Vec4 a) { const float len = length(a); return len > kEpsilon ? a * (1.0f / len) : Vec4{}; }

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

// Straight (non-premultiplied) linear RGBA in [0,1] per channel. Alpha 1 = opaque.
struct Color {
  float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

  constexpr Color() = default;
  constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
  constexpr explicit Color(Vec3 rgb, float a_ = 1.0f) : r(rgb.x), g(rgb.y), b(rgb.z), a(a_) {}

  constexpr float&       operator[](int i)       { return (&r)[i]; }
  constexpr const float& operator[](int i) const { return (&r)[i]; }

  constexpr Vec3 rgb() const { return {r, g, b}; }
  constexpr Vec4 rgba() const { return {r, g, b, a}; }

  // NOTE: spelled with std:: types, not the `u8` alias — the aliases live in
  // Types.h, which includes this header, so Math.h must not depend on them.
  static constexpr Color fromBytes(std::uint8_t r_, std::uint8_t g_, std::uint8_t b_,
                                   std::uint8_t a_ = 255) {
    constexpr float s = 1.0f / 255.0f;
    return {r_ * s, g_ * s, b_ * s, a_ * s};
  }
};

inline constexpr bool operator==(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
inline constexpr bool operator!=(Color a, Color b) { return !(a == b); }

// ---------------------------------------------------------------------------
// Mat4 — column-major, m[col * 4 + row]
// ---------------------------------------------------------------------------

struct Mat4 {
  float m[16] = {1, 0, 0, 0,
                 0, 1, 0, 0,
                 0, 0, 1, 0,
                 0, 0, 0, 1};

  constexpr Mat4() = default;

  constexpr float&       at(int row, int col)       { return m[col * 4 + row]; }
  constexpr const float& at(int row, int col) const { return m[col * 4 + row]; }

  constexpr Vec4 column(int col) const {
    return {m[col * 4 + 0], m[col * 4 + 1], m[col * 4 + 2], m[col * 4 + 3]};
  }
  constexpr void setColumn(int col, Vec4 v) {
    m[col * 4 + 0] = v.x; m[col * 4 + 1] = v.y; m[col * 4 + 2] = v.z; m[col * 4 + 3] = v.w;
  }
  constexpr Vec3 translation() const { return {m[12], m[13], m[14]}; }

  constexpr bool isIdentity(float eps = 1e-5f) const {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        const float want = (r == c) ? 1.0f : 0.0f;
        if (!nearlyEqual(m[c * 4 + r], want, eps)) return false;
      }
    return true;
  }

  static constexpr Mat4 identity() { return Mat4{}; }

  static constexpr Mat4 zero() {
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = 0.0f;
    return r;
  }
};

// Row-vector-free convention: result = a * b means "apply b, then a".
constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r = Mat4::zero();
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = sum;
    }
  return r;
}

constexpr Vec4 operator*(const Mat4& a, const Vec4& v) {
  Vec4 r;
  for (int row = 0; row < 4; ++row)
    r[row] = a.m[0 * 4 + row] * v.x + a.m[1 * 4 + row] * v.y + a.m[2 * 4 + row] * v.z + a.m[3 * 4 + row] * v.w;
  return r;
}

inline constexpr bool operator==(const Mat4& a, const Mat4& b) {
  for (int i = 0; i < 16; ++i) if (a.m[i] != b.m[i]) return false;
  return true;
}

// --- construction ----------------------------------------------------------

inline constexpr Mat4 translation(Vec3 t) {
  Mat4 r;
  r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
  return r;
}

inline constexpr Mat4 scaling(Vec3 s) {
  Mat4 r;
  r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
  return r;
}

inline constexpr Mat4 scale(float s) { return scaling({s, s, s}); }

inline Mat4 rotationX(float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  Mat4 r;
  r.m[5] = c;  r.m[6] = s;
  r.m[9] = -s; r.m[10] = c;
  return r;
}

inline Mat4 rotationY(float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  Mat4 r;
  r.m[0] = c;  r.m[2] = -s;
  r.m[8] = s;  r.m[10] = c;
  return r;
}

inline Mat4 rotationZ(float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  Mat4 r;
  r.m[0] = c;  r.m[1] = s;
  r.m[4] = -s; r.m[5] = c;
  return r;
}

inline Mat4 rotationAxis(Vec3 axis, float angle) {
  const Vec3 n = normalize(axis);
  if (lengthSq(n) < kEpsilon) return Mat4::identity();
  const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
  const float x = n.x, y = n.y, z = n.z;

  Mat4 r;
  r.m[0] = t * x * x + c;      r.m[1] = t * x * y + s * z;  r.m[2] = t * x * z - s * y;
  r.m[4] = t * x * y - s * z;  r.m[5] = t * y * y + c;      r.m[6] = t * y * z + s * x;
  r.m[8] = t * x * z + s * y;  r.m[9] = t * y * z - s * x;  r.m[10] = t * z * z + c;
  return r;
}

// Euler angles in radians, XYZ order (Blender default): R = Rz * Ry * Rx.
inline Mat4 rotationEuler(Vec3 eulerRadians) {
  return rotationZ(eulerRadians.z) * rotationY(eulerRadians.y) * rotationX(eulerRadians.x);
}

// --- projections -----------------------------------------------------------

inline Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
  Mat4 r = Mat4::zero();
  const float f = 1.0f / std::tan(fovYRadians * 0.5f);
  r.m[0]  = f / (aspect > kEpsilon ? aspect : 1.0f);
  r.m[5]  = f;
  r.m[10] = (farZ + nearZ) / (nearZ - farZ);
  r.m[11] = -1.0f;
  r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
  return r;
}

inline Mat4 ortho(float left, float right, float bottom, float top, float nearZ, float farZ) {
  Mat4 r;
  r.m[0]  = 2.0f / (right - left);
  r.m[5]  = 2.0f / (top - bottom);
  r.m[10] = -2.0f / (farZ - nearZ);
  r.m[12] = -(right + left) / (right - left);
  r.m[13] = -(top + bottom) / (top - bottom);
  r.m[14] = -(farZ + nearZ) / (farZ - nearZ);
  return r;
}

inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
  const Vec3 f = normalize(center - eye);
  Vec3 s = cross(f, up);
  if (lengthSq(s) < kEpsilon) s = cross(f, Vec3{0.0f, 0.0f, 1.0f});  // up parallel to f
  s = normalize(s);
  const Vec3 u = cross(s, f);

  Mat4 r;
  r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;  r.m[12] = -dot(s, eye);
  r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;  r.m[13] = -dot(u, eye);
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
  r.m[3] = 0.0f; r.m[7] = 0.0f; r.m[11] = 0.0f; r.m[15] = 1.0f;
  return r;
}

// --- transforms ------------------------------------------------------------

// Applies the full 4x4 (translation included) to a position.
inline Vec3 transformPoint(const Mat4& a, Vec3 p) {
  const Vec4 r = a * Vec4{p, 1.0f};
  if (std::fabs(r.w) > kEpsilon && r.w != 1.0f) return {r.x / r.w, r.y / r.w, r.z / r.w};
  return {r.x, r.y, r.z};
}

// Applies rotation/scale only (w = 0), for directions and normals of rigid transforms.
inline Vec3 transformDirection(const Mat4& a, Vec3 d) {
  const Vec4 r = a * Vec4{d, 0.0f};
  return {r.x, r.y, r.z};
}

inline constexpr Mat4 transpose(const Mat4& a) {
  Mat4 r;
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) r.m[row * 4 + c] = a.m[c * 4 + row];
  return r;
}

inline Mat4 inverse(const Mat4& a) {
  const float* m = a.m;
  float inv[16];

  inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15]
           + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
           - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8]  =  m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
           + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
           - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

  inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
           - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
           + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9]  = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
           - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] =  m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
           + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

  inv[2]  =  m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
           + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6]  = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
           - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] =  m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
           + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
           - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

  inv[3]  = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
           - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7]  =  m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
           + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
           - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] =  m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
           + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return Mat4::identity();   // singular: identity is the safe fallback
  det = 1.0f / det;

  Mat4 r;
  for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
  return r;
}

// Inverse-transpose, for normals under non-uniform scale.
inline Mat4 normalMatrix(const Mat4& model) {
  Mat4 r = model;
  r.m[3] = r.m[7] = r.m[11] = 0.0f;   // drop translation
  r.m[12] = r.m[13] = r.m[14] = 0.0f;
  r.m[15] = 1.0f;
  return transpose(inverse(r));
}

// ---------------------------------------------------------------------------
// Quat
// ---------------------------------------------------------------------------

struct Quat {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

  constexpr Quat() = default;
  constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

  static constexpr Quat identity() { return {}; }

  static Quat fromAxisAngle(Vec3 axis, float angle) {
    const Vec3 n = normalize(axis);
    const float half = angle * 0.5f, s = std::sin(half);
    return {n.x * s, n.y * s, n.z * s, std::cos(half)};
  }

  // XYZ euler, radians — same order as rotationEuler().
  static Quat fromEuler(Vec3 eulerRadians) {
    const float hx = eulerRadians.x * 0.5f, hy = eulerRadians.y * 0.5f, hz = eulerRadians.z * 0.5f;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);
    return {sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz};
  }

  constexpr Vec3 xyz() const { return {x, y, z}; }
};

inline constexpr Quat operator*(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline constexpr Vec3 operator*(const Quat& q, Vec3 v) {
  const Vec3 u{q.x, q.y, q.z};
  return u * (2.0f * dot(u, v)) + v * (q.w * q.w - dot(u, u)) + cross(u, v) * (2.0f * q.w);
}

inline constexpr float lengthSq(const Quat& q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }
inline float length(const Quat& q) { return std::sqrt(lengthSq(q)); }

inline Quat normalize(const Quat& q) {
  const float len = length(q);
  if (len < kEpsilon) return Quat::identity();
  const float inv = 1.0f / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline constexpr Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat slerp(const Quat& a, const Quat& bIn, float t) {
  Quat b = bIn;
  float cosTheta = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (cosTheta < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; cosTheta = -cosTheta; }
  if (cosTheta > 0.9995f) {   // nearly parallel: lerp is stable and cheap
    return normalize(Quat{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)});
  }
  const float theta = std::acos(clampf(cosTheta, -1.0f, 1.0f));
  const float sinTheta = std::sin(theta);
  const float wa = std::sin((1.0f - t) * theta) / sinTheta;
  const float wb = std::sin(t * theta) / sinTheta;
  return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

inline Mat4 toMat4(const Quat& qIn) {
  const Quat q = normalize(qIn);
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;

  Mat4 r;
  r.m[0] = 1.0f - 2.0f * (yy + zz);  r.m[1] = 2.0f * (xy + wz);        r.m[2] = 2.0f * (xz - wy);
  r.m[4] = 2.0f * (xy - wz);         r.m[5] = 1.0f - 2.0f * (xx + zz);  r.m[6] = 2.0f * (yz + wx);
  r.m[8] = 2.0f * (xz + wy);         r.m[9] = 2.0f * (yz - wx);         r.m[10] = 1.0f - 2.0f * (xx + yy);
  return r;
}

// ---------------------------------------------------------------------------
// Ray
// ---------------------------------------------------------------------------

// Shared vocabulary: `render` PRODUCES one (OrbitCamera::screenToRay) and
// `scene` CONSUMES one (World::pick), so §4.1 puts it in core rather than in
// either module (decision D16). `direction` is normalized — screenToRay
// guarantees that; consumers may assume it.
struct Ray {
  Vec3 origin{0.0f, 0.0f, 0.0f};
  Vec3 direction{0.0f, 0.0f, -1.0f};

  Vec3 pointAt(float t) const { return origin + direction * t; }
};

} // namespace ember
