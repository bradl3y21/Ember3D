// Modifiers.cpp — the first three modifiers + stack evaluation (item 121).
//
// D40 (item 120, frozen) fixed the stack, the boundary and the parameter
// mechanism; THIS file does the geometry maths, and the rules it must obey
// are all D40:
//
//   * each modifier is `const MeshData& -> MeshData` (a pure function);
//   * DETERMINISM is contract: a modifier reads only its parameters and the
//     incoming mesh — never frame time, the camera, the selection or a
//     global. Same inputs, bit-identical output, every call;
//   * FAILURE IS A NO-OP: a modifier that cannot evaluate leaves the
//     incoming mesh unchanged and reports failure; the stack continues;
//   * evaluation is NON-DESTRUCTIVE: the input is const, the output is a
//     value, nothing here ever writes an Object (that wiring lives in
//     Scene.cpp, which drives this from the object's stack).
//
// LICENCE / ORIGIN NOTE (GPL-3.0-or-later): the subdivision approach is a
// DESIGN port of Blender's Catmull-Clark (`source/blender/blenkernel/intern/
// mesh_ccg.c`, read around release 4.1) into this project's index-based
// MeshData — no text was copied and no BLI_/DNA subsystem was imported
// (AGENTS.md §0 rules). The plan (WAVE-5.md, Track D) prefers OpenSubdiv;
// it is NOT vendored in this tree yet, so this native implementation stands
// in (deterministic, unit-tested) and can be swapped when Kiln vendors it.
// Residue recorded in the item-121 finding.
//
// Triangle-soup limitation, stated rather than hidden: MeshData is a
// triangle list, so a flat quad face arrives as two triangles with a real
// input edge between them. Catmull-Clark on triangulated input keeps flat
// regions flat (all new points of a coplanar region stay in-plane) and the
// seam is invisible after smooth-normal re-computation, but quad-merge
// preprocessing or ngon-aware faces do not exist here yet (OpenSubdiv when
// vendored would supply the ngon path).
//
// New-vertex ORDER after one subdivision level (the tests pin it):
//   [0, C)             canonical vertex points (exact-position weld), in
//                      first-occurrence order
//   [C, C + E)         edge points, first-appearance edge order
//   [C + E, C + E + F) face points, face order
// and each input triangle becomes 3 quads = 6 output triangles.

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ember::scene {

namespace {

// ---------------------------------------------------------------------------
// shared plumbing: `ok = false` means "leave the incoming mesh alone" —
// the D40 failure geometry is a no-op.
// ---------------------------------------------------------------------------

MeshData noOp(const MeshData& in, bool* ok) {
  if (ok) *ok = false;
  return in;
}

void reportSuccess(bool* ok) {
  if (ok) *ok = true;
}

inline Vec3 posAt(const MeshData& m, u32 v) {
  return Vec3{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
}

inline void writeVec(std::vector<float>& out, u32 v, const Vec3& p) {
  out[v * 3]     = p.x;
  out[v * 3 + 1] = p.y;
  out[v * 3 + 2] = p.z;
}

// ---------------------------------------------------------------------------
// Subdivision — Catmull-Clark, one call per level.
// ---------------------------------------------------------------------------

constexpr int kMaxSubdivisionLevels = 6;   // equals modifierDesc's range

// One undirected edge with its incident-face bookkeeping.
struct SubEdge {
  u32 v0 = 0, v1 = 0;              // ascending canonical pair
  std::vector<u32> faces;          // 1 (boundary) or 2 (interior) entries
};

// CANONICALIZATION, and why it exists: the primitives emit duplicated
// corners (per-face split normals — Primitives.cpp's builder never shares),
// so index adjacency alone would see a disconnected 12-triangle cube whose
// every edge is "boundary", and CC's smoothing would collapse. The
// subdivision therefore welds by EXACT POSITION EQUALITY (bit patterns —
// deterministic, the D34-safe weld: no epsilon) and evaluates on the
// canonical vertices. The output is a properly welded mesh.
struct Bit3 {
  float v[3];
};
struct PosEq {
  bool operator()(const Bit3& a, const Bit3& b) const {
    return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2];
  }
};
struct PosHash {
  std::size_t operator()(const Bit3& b) const {
    u32 pat[3];
    std::memcpy(pat, b.v, sizeof(pat));
    std::size_t h = 1469598103934665603ull;
    for (int i = 0; i < 3; ++i) {
      h ^= static_cast<std::size_t>(pat[i]);
      h *= 1099511628211ull;
    }
    return h;
  }
};

// Adjacency for the incoming triangle soup, over canonical positions.
// Deterministic: edge ids are assigned in first-appearance order while
// scanning faces in order; canonical vertex ids by first occurrence.
struct SubAdjacency {
  u32 canonCount = 0;
  std::vector<Vec3> canonPos;    // canonical (exact-position-welded) coords
  std::vector<u32> canonOf;      // input slot -> canonical vertex id
  std::vector<SubEdge> edges;
  std::unordered_map<u64, u32> edgeIndex;     // (min<<32)|max over canonical
  std::vector<std::vector<u32>> vertexEdges;  // per canonical, UNIQUE edges
  std::vector<std::vector<u32>> vertexFaces;  // per canonical
};

u64 edgeKey(u32 a, u32 b) {
  const u32 lo = std::min(a, b);
  const u32 hi = std::max(a, b);
  return (static_cast<u64>(lo) << 32) | static_cast<u64>(hi);
}

// Structural validation is the refuse-rather-than-corrupt rule: any index
// out of range or a non-triangle index list refuses the modifier.
bool buildSubAdjacency(const MeshData& in, SubAdjacency& adj) {
  const u32 n = in.vertexCount();
  const u32 faceCount = in.indexCount() / 3;
  if (in.indexCount() % 3 != 0) return false;
  if (in.positions.size() != static_cast<std::size_t>(n) * 3) return false;
  for (const u32 i : in.indices)
    if (i >= n) return false;

  // Canonical slots by exact position equality.
  adj.canonOf.resize(n, 0xFFFFFFFFu);
  std::unordered_map<Bit3, u32, PosHash, PosEq> canon;
  canon.reserve(n * 2);
  adj.canonPos.clear();
  std::vector<Vec3> inPos(n);
  for (u32 v = 0; v < n; ++v) inPos[v] = posAt(in, v);
  for (u32 v = 0; v < n; ++v) {
    Bit3 key;
    key.v[0] = inPos[v].x;
    key.v[1] = inPos[v].y;
    key.v[2] = inPos[v].z;
    auto it = canon.find(key);
    if (it == canon.end()) {
      const u32 id = static_cast<u32>(adj.canonPos.size());
      adj.canonPos.push_back(inPos[v]);
      canon.emplace(key, id);
      adj.canonOf[v] = id;
    } else {
      adj.canonOf[v] = it->second;
    }
  }
  adj.canonCount = static_cast<u32>(adj.canonPos.size());

  adj.edges.clear();
  adj.edgeIndex.clear();
  adj.vertexEdges.assign(adj.canonCount, {});
  adj.vertexFaces.assign(adj.canonCount, {});

  for (u32 f = 0; f < faceCount; ++f) {
    const u32 tri[3] = {adj.canonOf[in.indices[f * 3]],
                        adj.canonOf[in.indices[f * 3 + 1]],
                        adj.canonOf[in.indices[f * 3 + 2]]};
    for (int k = 0; k < 3; ++k)
      adj.vertexFaces[tri[k]].push_back(f);
    for (int e = 0; e < 3; ++e) {
      const u32 a = tri[e];
      const u32 b = tri[(e + 1) % 3];
      const u64 key = edgeKey(a, b);
      auto it = adj.edgeIndex.find(key);
      if (it == adj.edgeIndex.end()) {
        // New edge: touch each endpoint's incident-edge list exactly ONCE
        // (valence = unique edges, not edge-uses).
        const u32 eid = static_cast<u32>(adj.edges.size());
        SubEdge edge;
        edge.v0 = std::min(a, b);
        edge.v1 = std::max(a, b);
        edge.faces.push_back(f);
        adj.edgeIndex.emplace(key, eid);
        adj.vertexEdges[a].push_back(eid);
        adj.edges.push_back(std::move(edge));
        if (b != a) adj.vertexEdges[b].push_back(eid);
      } else if (adj.edges[it->second].faces.size() < 2) {
        adj.edges[it->second].faces.push_back(f);   // >2: non-manifold, capped
      }
    }
  }
  return true;
}

std::vector<Vec3> facePoints(const MeshData& in) {
  const u32 faceCount = in.indexCount() / 3;
  std::vector<Vec3> fp(faceCount, Vec3{0, 0, 0});
  for (u32 f = 0; f < faceCount; ++f) {
    Vec3 acc{0, 0, 0};
    for (int k = 0; k < 3; ++k)
      acc = acc + posAt(in, in.indices[f * 3 + k]);
    fp[f] = acc / 3.0f;                        // centroid of the triangle
  }
  return fp;
}

// One Catmull-Clark level over a triangle soup (classic interior/boundary
// rules, see the origin note). Degenerate faces/vertices fall back to their
// inputs; nothing here can produce NaN (normalize() of a zero vector is the
// zero vector in core, and this function divides only by face counts).
MeshData subdivideOnce(const MeshData& in) {
  const u32 n = in.vertexCount();
  const u32 faceCount = in.indexCount() / 3;
  if (faceCount == 0) return in;               // nothing to subdivide

  SubAdjacency adj;
  if (!buildSubAdjacency(in, adj)) return in;  // un-evaluable -> level is a no-op
  const u32 vCount = adj.canonCount;           // canonical (welded) vertices
  const auto canon = [&adj](u32 c) {
    return Vec3{adj.canonPos[c].x, adj.canonPos[c].y, adj.canonPos[c].z};
  };

  const std::vector<Vec3> fp = facePoints(in);
  const u32 eCount = static_cast<u32>(adj.edges.size());

  MeshData out;
  out.name = in.name;
  // New layout (the tests pin it): V canonical vertex points, then E edge
  // points, then F face points. Output indices reference canonical slots,
  // so the result is a properly WELDED mesh regardless of the input's
  // per-face vertex splits.
  out.positions.resize(static_cast<std::size_t>(vCount + eCount + faceCount) * 3);
  out.indices.reserve(in.indices.size() * 6);  // 6 output tris per input tri

  // --- vertex points ------------------------------------------------------
  for (u32 v = 0; v < vCount; ++v) {
    const Vec3 p = canon(v);
    std::vector<u32> boundaryEdges;
    for (const u32 eid : adj.vertexEdges[v])
      if (adj.edges[eid].faces.size() == 1)
        boundaryEdges.push_back(eid);
    if (!boundaryEdges.empty()) {
      // Boundary: V' = (6P + sum(midpoints of incident boundary edges))
      //               / (6 + count) — the classic (6P + M1 + M2)/8 for the
      // normal two-edge coastline case; a 1-edge dead-end degrades smoothly.
      const float denom = 6.0f + static_cast<float>(boundaryEdges.size());
      Vec3 vNew = p * 6.0f;
      for (const u32 eid : boundaryEdges) {
        const SubEdge& e = adj.edges[eid];
        vNew = vNew + (canon(e.v0) + canon(e.v1)) / 2.0f;
      }
      writeVec(out.positions, v, vNew / denom);
    } else if (!adj.vertexFaces[v].empty()) {
      // Interior: V' = (Q + 2R + (n-3)P) / n, n = valence (UNIQUE edges).
      const u32 valence = static_cast<u32>(adj.vertexEdges[v].size());
      const float fn = static_cast<float>(valence);
      Vec3 q{0, 0, 0};
      for (const u32 f : adj.vertexFaces[v]) q = q + fp[f];
      q = q / static_cast<float>(adj.vertexFaces[v].size());
      Vec3 r{0, 0, 0};
      for (const u32 eid : adj.vertexEdges[v]) {
        const SubEdge& e = adj.edges[eid];
        r = r + (canon(e.v0) + canon(e.v1)) / 2.0f;
      }
      r = r / fn;
      writeVec(out.positions, v, (q + r * 2.0f + p * (fn - 3.0f)) / fn);
    } else {
      writeVec(out.positions, v, p);           // isolated canonical unchanged
    }
  }

  // --- edge points ----------------------------------------------------------
  for (u32 e = 0; e < eCount; ++e) {
    const SubEdge& edge = adj.edges[e];
    const Vec3 p0 = canon(edge.v0);
    const Vec3 p1 = canon(edge.v1);
    const Vec3 ep = (edge.faces.size() == 2)
        ? (p0 + p1 + fp[edge.faces[0]] + fp[edge.faces[1]]) / 4.0f
        : (p0 + p1) / 2.0f;                    // boundary/degenerate: midpoint
    writeVec(out.positions, vCount + e, ep);
  }

  // --- face points ------------------------------------------------------------
  for (u32 f = 0; f < faceCount; ++f)
    writeVec(out.positions, vCount + eCount + f, fp[f]);

  // --- new faces: 3 quads per input triangle -> 6 triangles -------------------
  const u32 fStart = vCount + eCount;
  for (u32 f = 0; f < faceCount; ++f) {
    const u32 tri[3] = {adj.canonOf[in.indices[f * 3]],
                        adj.canonOf[in.indices[f * 3 + 1]],
                        adj.canonOf[in.indices[f * 3 + 2]]};
    for (int corner = 0; corner < 3; ++corner) {
      const u32 self = tri[corner];
      const u32 next = tri[(corner + 1) % 3];
      const u32 prev = tri[(corner + 2) % 3];
      const u32 eSelfNext = vCount + adj.edgeIndex.at(edgeKey(self, next));
      const u32 ePrevSelf = vCount + adj.edgeIndex.at(edgeKey(prev, self));
      // Quad cycle (self', e(self,next), F, e(prev,self)) keeps the input
      // winding; emitted as two triangles sharing the face point.
      out.indices.push_back(self);
      out.indices.push_back(eSelfNext);
      out.indices.push_back(fStart + f);
      out.indices.push_back(self);
      out.indices.push_back(fStart + f);
      out.indices.push_back(ePrevSelf);
    }
  }
  return out;
}

// Smooth per-vertex normals for the FINAL evaluated mesh (the descriptor's
// smooth_normals). Zero-length stars accumulate to zero, which core's
// normalize() maps to the zero vector — no NaN can appear.
void computeSmoothNormals(MeshData& m) {
  const u32 n = m.vertexCount();
  m.normals.assign(m.positions.size(), 0.0f);
  std::vector<Vec3> acc(n, Vec3{0, 0, 0});
  const u32 faceCount = m.indexCount() / 3;
  for (u32 f = 0; f < faceCount; ++f) {
    const Vec3 a = posAt(m, m.indices[f * 3]);
    const Vec3 b = posAt(m, m.indices[f * 3 + 1]);
    const Vec3 c = posAt(m, m.indices[f * 3 + 2]);
    const Vec3 fn = ember::normalize(ember::cross(
        Vec3{b.x - a.x, b.y - a.y, b.z - a.z}, Vec3{c.x - a.x, c.y - a.y, c.z - a.z}));
    acc[m.indices[f * 3]]     = acc[m.indices[f * 3]] + fn;
    acc[m.indices[f * 3 + 1]] = acc[m.indices[f * 3 + 1]] + fn;
    acc[m.indices[f * 3 + 2]] = acc[m.indices[f * 3 + 2]] + fn;
  }
  for (u32 v = 0; v < n; ++v)
    writeVec(m.normals, v, ember::normalize(acc[v]));
}

MeshData doSubdivision(const MeshData& in, const SubdivisionParams& p, bool* ok) {
  const int levels = std::clamp(p.levels, 0, kMaxSubdivisionLevels);
  if (levels == 0) { reportSuccess(ok); return in; }        // declared no-op
  SubAdjacency probe;
  if (!buildSubAdjacency(in, probe)) return noOp(in, ok);   // structural refuse
  MeshData m = in;
  for (int i = 0; i < levels; ++i) m = subdivideOnce(m);
  if (p.smoothNormals) computeSmoothNormals(m);
  else m.normals.clear();                    // empty -> renderer computes flat
  m.uvs.clear();     // triangle-CC cannot carry a uv surface; header note
  reportSuccess(ok);
  return m;
}

// ---------------------------------------------------------------------------
// Mirror — duplicated half + winding flip + twin weld, axes in X,Y,Z order.
// ---------------------------------------------------------------------------

// One axis pass. axis: 0 = x, 1 = y, 2 = z.
MeshData mirrorOnce(const MeshData& in, int axis, float mergeThreshold) {
  const u32 n = in.vertexCount();
  const bool hasNormals = in.normals.size() == in.positions.size();
  const bool hasUvs = in.uvs.size() == static_cast<std::size_t>(n) * 2;
  const std::size_t total = static_cast<std::size_t>(n) * 2;

  // Twin weld: mirrors are vertex v and twin n+v by construction, so ONLY
  // those pairs are compared (arbitrary coincidences of different vertices
  // are NOT merged). A twin within threshold welds onto the ORIGINAL index;
  // every other vertex survives. The result is re-densified in a single
  // deterministic scan: originals keep [0, n) order, kept twins follow.
  std::vector<u32> remap(total, 0xFFFFFFFFu);
  u32 dense = 0;
  for (u32 v = 0; v < n; ++v) remap[v] = dense++;    // originals always live
  for (u32 v = 0; v < n; ++v) {
    const float daxis = std::fabs(in.positions[v * 3 + axis]);
    if (daxis * 2.0f <= mergeThreshold)
      remap[n + v] = remap[v];                       // welded onto the original
  }
  for (u32 twin = n; twin < total; ++twin)
    if (remap[twin] == 0xFFFFFFFFu) remap[twin] = dense++;
  const u32 outCount = dense;

  MeshData out;
  out.name = in.name;
  // Build the fresh arrays slot-by-slot through `remap` (no pass-to-pass
  // copying hazards): slot of an original carries the ORIGINAL; the slot of
  // a kept twin carries the mirrored values. Welded twins write nothing —
  // their slot is owned by their original.
  out.positions.assign(static_cast<std::size_t>(outCount) * 3, 0.0f);
  if (hasNormals) out.normals.assign(static_cast<std::size_t>(outCount) * 3, 0.0f);
  if (hasUvs) out.uvs.assign(static_cast<std::size_t>(outCount) * 2, 0.0f);
  for (u32 v = 0; v < n; ++v) {
    const u32 slot = remap[v];
    for (int k = 0; k < 3; ++k)
      out.positions[slot * 3 + k] = in.positions[v * 3 + k];
    if (hasNormals)
      for (int k = 0; k < 3; ++k)
        out.normals[slot * 3 + k] = in.normals[v * 3 + k];
    if (hasUvs) {
      out.uvs[slot * 2] = in.uvs[v * 2];
      out.uvs[slot * 2 + 1] = in.uvs[v * 2 + 1];
    }
    const u32 twinSlot = remap[n + v];
    if (twinSlot != slot) {                          // kept twin (not welded)
      for (int k = 0; k < 3; ++k)
        out.positions[twinSlot * 3 + k] = (k == axis)
            ? -in.positions[v * 3 + k]
            : in.positions[v * 3 + k];
      if (hasNormals)
        for (int k = 0; k < 3; ++k)
          out.normals[twinSlot * 3 + k] = (k == axis)
              ? -in.normals[v * 3 + k]
              : in.normals[v * 3 + k];
      if (hasUvs) {
        out.uvs[twinSlot * 2] = in.uvs[v * 2];
        out.uvs[twinSlot * 2 + 1] = in.uvs[v * 2 + 1];
      }
    }
  }

  // Indices: original half through the remap; mirrored half through the
  // remap with REVERSED winding (a,b,c -> A,C,B) so the duplicated surface
  // faces outward again.
  out.indices.resize(in.indices.size() * 2);
  const u32 triCount = in.indexCount() / 3;
  for (u32 f = 0; f < triCount; ++f) {
    out.indices[f * 3 + 0] = remap[in.indices[f * 3]];
    out.indices[f * 3 + 1] = remap[in.indices[f * 3 + 1]];
    out.indices[f * 3 + 2] = remap[in.indices[f * 3 + 2]];
    out.indices[in.indices.size() + f * 3 + 0] = remap[in.indices[f * 3] + n];
    out.indices[in.indices.size() + f * 3 + 1] = remap[in.indices[f * 3 + 2] + n];
    out.indices[in.indices.size() + f * 3 + 2] = remap[in.indices[f * 3 + 1] + n];
  }
  return out;
}

MeshData doMirror(const MeshData& in, const MirrorParams& p, bool* ok) {
  MeshData m = in;
  const bool flags[3] = {p.axisX, p.axisY, p.axisZ};
  for (int axis = 0; axis < 3; ++axis)
    if (flags[axis]) m = mirrorOnce(m, axis, p.mergeThreshold);
  reportSuccess(ok);
  return m;
}

// ---------------------------------------------------------------------------
// Array — per-copy displacement, object-local; deterministic concatenation.
// ---------------------------------------------------------------------------

MeshData doArray(const MeshData& in, const ArrayParams& p, bool* ok) {
  const int count = std::clamp(p.count, 1, 64);
  if (count <= 1) { reportSuccess(ok); return in; }         // declared no-op
  const u32 n = in.vertexCount();
  float extents[3] = {0.0f, 0.0f, 0.0f};
  if (!in.positions.empty()) {
    float mins[3] = {in.positions[0], in.positions[1], in.positions[2]};
    float maxs[3] = {mins[0], mins[1], mins[2]};
    for (std::size_t i = 0; i + 2 < in.positions.size(); i += 3) {
      for (int k = 0; k < 3; ++k) {
        mins[k] = std::min(mins[k], in.positions[i + k]);
        maxs[k] = std::max(maxs[k], in.positions[i + k]);
      }
    }
    for (int k = 0; k < 3; ++k) extents[k] = maxs[k] - mins[k];
  }
  const float step[3] = {
      p.relative ? p.offsetX * extents[0] : p.offsetX,
      p.relative ? p.offsetY * extents[1] : p.offsetY,
      p.relative ? p.offsetZ * extents[2] : p.offsetZ};
  const bool hasNormals = in.normals.size() == in.positions.size();
  const bool hasUvs = in.uvs.size() == static_cast<std::size_t>(n) * 2;

  MeshData out;
  out.name = in.name;
  out.positions.resize(in.positions.size() * static_cast<std::size_t>(count));
  if (hasNormals) out.normals.resize(out.positions.size());
  if (hasUvs) out.uvs.resize(in.uvs.size() * static_cast<std::size_t>(count));
  out.indices.resize(in.indices.size() * static_cast<std::size_t>(count));
  for (int c = 0; c < count; ++c) {
    const std::size_t vBase = static_cast<std::size_t>(c) * n;
    for (u32 v = 0; v < n; ++v) {
      for (int k = 0; k < 3; ++k) {
        float coord = in.positions[v * 3 + k];
        coord += (k == 0 ? step[0] : k == 1 ? step[1] : step[2]) *
                 static_cast<float>(c);
        out.positions[(vBase + v) * 3 + k] = coord;
      }
      if (hasNormals)
        for (int k = 0; k < 3; ++k)
          out.normals[(vBase + v) * 3 + k] = in.normals[v * 3 + k];
      if (hasUvs) {
        out.uvs[(vBase + v) * 2] = in.uvs[v * 2];
        out.uvs[(vBase + v) * 2 + 1] = in.uvs[v * 2 + 1];
      }
    }
    for (std::size_t i = 0; i < in.indices.size(); ++i)
      out.indices[static_cast<std::size_t>(c) * in.indices.size() + i] =
          in.indices[i] + static_cast<u32>(vBase);
  }
  reportSuccess(ok);
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Stack evaluation (Scene.cpp drives this; World owns the cache).
// ---------------------------------------------------------------------------

MeshData evaluateMeshStack(const MeshData& base,
                           const std::vector<ember::Modifier>& stack,
                           u32* failedIndex) {
  if (failedIndex) *failedIndex = 0xFFFFFFFFu;
  MeshData current = base;                     // by value: non-destructive
  for (u32 i = 0; i < stack.size(); ++i) {
    const Modifier& mod = stack[i];
    if (!mod.enabled) continue;          // D40: DISABLED entries are skipped
                                         // in place; position preserved.
    bool ok = false;
    MeshData next;
    switch (mod.kind()) {
      case ModifierKind::Subdivision:
        next = doSubdivision(current, std::get<SubdivisionParams>(mod.params), &ok);
        break;
      case ModifierKind::Mirror:
        next = doMirror(current, std::get<MirrorParams>(mod.params), &ok);
        break;
      case ModifierKind::Array:
        next = doArray(current, std::get<ArrayParams>(mod.params), &ok);
        break;
    }
    if (!ok) {
      // D40: failure is a no-op — the stack CONTINUES from the last good
      // hold (the call returns the last good mesh) and the FIRST failing
      // index is recorded for the editor to show.
      if (failedIndex) *failedIndex = i;
      return current;
    }
    current = std::move(next);
  }
  return current;
}

} // namespace ember::scene
