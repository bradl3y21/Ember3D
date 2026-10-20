// Primitives.cpp — mesh generators for the five primitive kinds.
// Owned by Slate · Scene.
//
// Conventions (what tests can pin down):
//   * All hand-built, indexed triangles, wound CCW as seen from outside,
//     right-handed, Y up.
//   * Centred at the origin, Y up. Every kind EXCEPT the torus spans exactly
//     one unit (half extent 0.5). Torus: 0.7 half-extent in XZ, 0.2 in Y.
//     EXACT local AABB per kind, verified by measurement (2026-10-16, via
//     World::createPrimitive), float-exact for cube/plane and <=1e-6 from the
//     ideal value for the trig-attained extremes of sphere/cylinder/torus:
//       cube/sphere/cylinder: min (-0.5,-0.5,-0.5)  max (+0.5,+0.5,+0.5)
//       plane:                min (-0.5,  0, -0.5)  max (+0.5,  0,+0.5)
//       torus (minor R 0.2):  min (-0.7,-0.2,-0.7)  max (+0.7,+0.2,+0.7)
//   * Vertex/index counts: cube 24/36;  sphere (32x16) 561/3072;
//     plane 4/6;        cylinder (32 seg) 134/384;  torus (32x16) 561/3072.
//   * Every ring is closed with a duplicated vertex (+1) so UVs can tile.
//   * Normals at poles/seams come straight from the position direction (unit).
#include "scene/Scene.h"

#include <cmath>
#include <utility>

namespace ember::scene {

namespace {

constexpr float kHalf = 0.5f;
constexpr float kPi   = 3.14159265f;
constexpr float k2Pi  = 6.28318530f;

constexpr u32 kSphereLongitudeSegs = 32;
constexpr u32 kSphereLatitudeSegs  = 16;
constexpr u32 kCylinderSegments    = 32;
constexpr u32 kTorusMajorSegments  = 32;
constexpr u32 kTorusMinorSegments  = 16;
constexpr float kTorusMinorRadius  = 0.2f;

struct Builder {
  MeshData mesh;

  u32 vertex(float px, float py, float pz, float nx, float ny, float nz,
             float u, float v) {
    mesh.positions.push_back(px);  mesh.positions.push_back(py);
    mesh.positions.push_back(pz);
    mesh.normals.push_back(nx);    mesh.normals.push_back(ny);
    mesh.normals.push_back(nz);
    mesh.uvs.push_back(u);         mesh.uvs.push_back(v);
    return static_cast<u32>(mesh.positions.size() / 3 - 1);
  }

  void tri(u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);  mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  }

  // Two triangles of a grid quad: (p00, p01, p10) and (p01, p11, p10).
  void quad(u32 p00, u32 p01, u32 p10, u32 p11) {
    tri(p00, p01, p10);
    tri(p01, p11, p10);
  }
};

// phi = 0 is the +Y pole, theta runs around the equator, all on radius r.
void spherePoint(float phi, float theta, float r, float out[3]) {
  const float s = std::sin(phi) * r;
  out[0] = s * std::cos(theta);
  out[1] = std::cos(phi) * r;
  out[2] = s * std::sin(theta);
}

} // namespace

MeshData makePrimitiveMesh(Primitive kind) {
  Builder b;
  switch (kind) {
  case Primitive::Cube: {
    // u x v == n for every face, so each quad is CCW seen from outside.
    struct Face { Vec3 n, u, v, c; };
    const float h = kHalf;
    const Face faces[6] = {
      {Vec3{ 1, 0, 0}, Vec3{ 0, 0, -1}, Vec3{0, 1,  0}, Vec3{ h, 0, 0}},
      {Vec3{-1, 0, 0}, Vec3{ 0, 0,  1}, Vec3{0, 1,  0}, Vec3{-h, 0, 0}},
      {Vec3{ 0, 1, 0}, Vec3{ 1, 0,  0}, Vec3{0, 0, -1}, Vec3{0,  h, 0}},
      {Vec3{ 0,-1, 0}, Vec3{ 1, 0,  0}, Vec3{0, 0,  1}, Vec3{0, -h, 0}},
      {Vec3{ 0, 0, 1}, Vec3{ 1, 0,  0}, Vec3{0, 1,  0}, Vec3{0, 0,  h}},
      {Vec3{ 0, 0,-1}, Vec3{-1, 0,  0}, Vec3{0, 1,  0}, Vec3{0, 0, -h}},
    };
    for (const Face& f : faces) {
      const u32 v00 = b.vertex(f.c.x - f.u.x * h - f.v.x * h,
                               f.c.y - f.u.y * h - f.v.y * h,
                               f.c.z - f.u.z * h - f.v.z * h,
                               f.n.x, f.n.y, f.n.z, 0.0f, 0.0f);
      const u32 v01 = b.vertex(f.c.x + f.u.x * h - f.v.x * h,
                               f.c.y + f.u.y * h - f.v.y * h,
                               f.c.z + f.u.z * h - f.v.z * h,
                               f.n.x, f.n.y, f.n.z, 1.0f, 0.0f);
      const u32 v11 = b.vertex(f.c.x + f.u.x * h + f.v.x * h,
                               f.c.y + f.u.y * h + f.v.y * h,
                               f.c.z + f.u.z * h + f.v.z * h,
                               f.n.x, f.n.y, f.n.z, 1.0f, 1.0f);
      const u32 v10 = b.vertex(f.c.x - f.u.x * h + f.v.x * h,
                               f.c.y - f.u.y * h + f.v.y * h,
                               f.c.z - f.u.z * h + f.v.z * h,
                               f.n.x, f.n.y, f.n.z, 0.0f, 1.0f);
      b.quad(v00, v01, v10, v11);
    }
    break;
  }

  case Primitive::Sphere: {
    const u32 segs = kSphereLongitudeSegs;   // around (theta)
    const u32 rings = kSphereLatitudeSegs;   // pole to pole (phi)
    const float r = kHalf;
    for (u32 i = 0; i <= rings; ++i) {
      const float phi = kPi * i / rings;
      for (u32 j = 0; j <= segs; ++j) {
        const float theta = k2Pi * j / segs;
        float p[3];
        spherePoint(phi, theta, r, p);
        b.vertex(p[0], p[1], p[2], p[0] / r, p[1] / r, p[2] / r,
                 static_cast<float>(j) / segs,
                 1.0f - static_cast<float>(i) / rings);
      }
    }
    for (u32 i = 0; i < rings; ++i) {
      for (u32 j = 0; j < segs; ++j) {
        const u32 row  = segs + 1;
        const u32 p00  = i * row + j;
        const u32 p01  = i * row + j + 1;
        const u32 p10  = (i + 1) * row + j;
        const u32 p11  = (i + 1) * row + j + 1;
        b.quad(p00, p01, p10, p11);
      }
    }
    break;
  }

  case Primitive::Plane: {
    // Single flat quad on XZ at y = 0.
    const float h = kHalf;
    const u32 v00 = b.vertex(-h, 0, -h, 0, 1, 0, 0.0f, 0.0f);
    const u32 v01 = b.vertex( h, 0, -h, 0, 1, 0, 1.0f, 0.0f);
    const u32 v11 = b.vertex( h, 0,  h, 0, 1, 0, 1.0f, 1.0f);
    const u32 v10 = b.vertex(-h, 0,  h, 0, 1, 0, 0.0f, 1.0f);
    b.quad(v00, v01, v10, v11);
    break;
  }

  case Primitive::Cylinder: {
    const u32 segs  = kCylinderSegments;
    const float r   = kHalf, top = kHalf, bottom = -kHalf;

    // Side: bottom ring then top ring, duplicated seam, radial normals.
    const u32 bottomRow = 0;
    for (u32 j = 0; j <= segs; ++j) {
      const float theta = k2Pi * j / segs;
      const float cx = std::cos(theta), sz = std::sin(theta);
      b.vertex(cx * r, bottom, sz * r, cx, 0, sz,
               static_cast<float>(j) / segs, 0.0f);
    }
    for (u32 j = 0; j <= segs; ++j) {
      const float theta = k2Pi * j / segs;
      const float cx = std::cos(theta), sz = std::sin(theta);
      b.vertex(cx * r, top, sz * r, cx, 0, sz,
               static_cast<float>(j) / segs, 1.0f);
    }
    const u32 topRow = segs + 1;
    for (u32 j = 0; j < segs; ++j) {
      const u32 b0 = bottomRow + j,  b1 = bottomRow + j + 1;
      const u32 t0 = topRow + j,     t1 = topRow + j + 1;
      b.tri(t0, b1, b0);
      b.tri(t0, t1, b1);
    }

    // Top cap: centre + rim ring. Wound (c, r_{j+1}, r_j) so the normal is +Y.
    const u32 topCentre = b.vertex(0, top, 0, 0, 1, 0, 0.5f, 0.5f);
    const u32 topBase   = static_cast<u32>(b.mesh.positions.size() / 3);
    for (u32 j = 0; j <= segs; ++j) {
      const float theta = k2Pi * j / segs;
      const float cx = std::cos(theta), sz = std::sin(theta);
      b.vertex(cx * r, top, sz * r, 0, 1, 0,
               cx * 0.5f + 0.5f, sz * 0.5f + 0.5f);
    }
    for (u32 j = 0; j < segs; ++j)
      b.tri(topCentre, topBase + j + 1, topBase + j);

    // Bottom cap, mirrored winding so the normal is -Y.
    const u32 bottomCentre = b.vertex(0, bottom, 0, 0, -1, 0, 0.5f, 0.5f);
    const u32 bottomBase   = static_cast<u32>(b.mesh.positions.size() / 3);
    for (u32 j = 0; j <= segs; ++j) {
      const float theta = k2Pi * j / segs;
      const float cx = std::cos(theta), sz = std::sin(theta);
      b.vertex(cx * r, bottom, sz * r, 0, -1, 0,
               cx * 0.5f + 0.5f, sz * 0.5f + 0.5f);
    }
    for (u32 j = 0; j < segs; ++j)
      b.tri(bottomCentre, bottomBase + j, bottomBase + j + 1);
    break;
  }

  case Primitive::Torus: {
    const u32 maj = kTorusMajorSegments, min = kTorusMinorSegments;
    const float R = kHalf, r = kTorusMinorRadius;
    for (u32 i = 0; i <= maj; ++i) {
      const float u  = k2Pi * i / maj;      // around the main ring
      const float cu = std::cos(u), su = std::sin(u);
      for (u32 j = 0; j <= min; ++j) {
        const float w  = k2Pi * j / min;    // around the tube
        const float cw = std::cos(w), sw = std::sin(w);
        b.vertex((R + r * cw) * cu, r * sw, (R + r * cw) * su,
                 cw * cu, sw, cw * su,      // outward tube normal
                 static_cast<float>(i) / maj, static_cast<float>(j) / min);
      }
    }
    for (u32 i = 0; i < maj; ++i) {
      for (u32 j = 0; j < min; ++j) {
        const u32 row  = min + 1;
        const u32 p00  = i * row + j;
        const u32 p01  = i * row + j + 1;
        const u32 p10  = (i + 1) * row + j;
        const u32 p11  = (i + 1) * row + j + 1;
        b.quad(p00, p01, p10, p11);
      }
    }
    break;
  }
  }
  return std::move(b.mesh);
}

} // namespace ember::scene
