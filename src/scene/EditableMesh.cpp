// EditableMesh.cpp — the D34 editable mesh (item 110) + topology ops (111).
// Owner: Slate · Scene. DESIGN notes in EditableMesh.h; the invariants this
// file enforces are the frozen D34 list (ARCHITECTURE.md §4.4).
//
// Port-of-design note (GPL-3.0-or-later, AGENTS.md §0): the topology model is
// Blender's bmesh (loop/edge/face records, corner attributes on loops, the
// one-vertex-per-corner-split rule in the round trip) ported as a DESIGN into
// ember types. NO Blender source text was copied and no BLI_*/DNA subsystem
// was imported — everything below is written against ember::Vec2/Vec3,
// std::vector and this project's MeshData.
#include "scene/EditableMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ember::scene {

namespace {

// Undirected edge key: canonical pair packed into one u64 (v0 < v1).
u64 edgeKey(u32 a, u32 b) {
  if (a > b) std::swap(a, b);
  return (static_cast<u64>(a) << 32) | static_cast<u64>(b);
}

void pushUnique(std::vector<u32>& v, u32 x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

// Face normal via Newell's method — robust for concave n-gon rings.
Vec3 faceNormalNewell(const EditableMesh& e, const Face& f) {
  Vec3 n{0, 0, 0};
  const u32 end = f.firstLoop + f.loopCount;
  for (u32 i = f.firstLoop; i < end; ++i) {
    const u32 iNext = (i + 1 == end) ? f.firstLoop : i + 1;
    const Vec3& a = e.positions[e.loops[i].vertex];
    const Vec3& b = e.positions[e.loops[iNext].vertex];
    n = Vec3{n.x + (a.y - b.y) * (a.z + b.z),
             n.y + (a.z - b.z) * (a.x + b.x),
             n.z + (a.x - b.x) * (a.y + b.y)};
  }
  return ember::normalize(n);
}

// ---- ear clipping (concave-safe triangulation of one face ring) -----------

struct Poly2 { float x, y; };

// Project the ring onto its best-fit plane (Newell normal).
bool ringTo2D(const EditableMesh& e, const Face& f, std::vector<Poly2>& out) {
  const Vec3 n = faceNormalNewell(e, f);
  const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
  const u32 end = f.firstLoop + f.loopCount;
  out.resize(f.loopCount);
  u32 w = 0;
  for (u32 i = f.firstLoop; i < end; ++i) {
    const Vec3& p = e.positions[e.loops[i].vertex];
    if (az >= ax && az >= ay)      out[w++] = Poly2{p.x, p.y};
    else if (ax >= ay && ax >= az) out[w++] = Poly2{p.y, p.z};
    else                           out[w++] = Poly2{p.z, p.x};
  }
  float area = 0.0f;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const Poly2& a = out[i];
    const Poly2& b = out[(i + 1) % out.size()];
    area += a.x * b.y - b.x * a.y;
  }
  return std::fabs(area) > 1e-12f;   // degenerate ring refused
}

// Ear clip over the projected ring; emits triangles as RING-INDEX triples.
void earClip(const std::vector<Poly2>& poly, std::vector<u32>& tri) {
  const std::size_t n = poly.size();
  std::vector<u32> ring(n);
  for (std::size_t i = 0; i < n; ++i) ring[i] = static_cast<u32>(i);
  const auto area2 = [&poly](u32 a, u32 b, u32 c) {
    const Poly2& A = poly[a];
    const Poly2& B = poly[b];
    const Poly2& C = poly[c];
    return (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
  };
  const auto pointInTri = [&poly, &area2](u32 p, u32 a, u32 b, u32 c) {
    const float d1 = area2(a, b, p);
    const float d2 = area2(b, c, p);
    const float d3 = area2(c, a, p);
    const bool hasNeg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    const bool hasPos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(hasNeg && hasPos);   // inside or exactly on an edge
  };
  std::size_t guard = 0;
  while (ring.size() > 3 && guard++ < n * 4) {
    bool clipped = false;
    for (std::size_t i = 0; i < ring.size() && !clipped; ++i) {
      const std::size_t ip = (i + ring.size() - 1) % ring.size();
      const std::size_t in = (i + 1) % ring.size();
      const u32 a = ring[ip], b = ring[i], c = ring[in];
      if (area2(a, b, c) <= 1e-12f) continue;   // reflex or degenerate
      bool containsOther = false;
      for (const u32 p : ring) {
        if (p == a || p == b || p == c) continue;
        if (pointInTri(p, a, b, c)) { containsOther = true; break; }
      }
      if (containsOther) continue;
      tri.push_back(a); tri.push_back(b); tri.push_back(c);
      ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
    }
    if (!clipped) break;   // simple faces always clip; guard for safety
  }
  tri.push_back(ring[0]);
  tri.push_back(ring[1]);
  tri.push_back(ring[2]);
}

} // namespace

// ---------------------------------------------------------------- validate

bool EditableMesh::validate(std::string* why) const {
  const auto fail = [&](const char* msg) {
    if (why) *why = msg;
    return false;
  };

  // (1) Every loop in exactly one face; faces contiguous, loopCount >= 3.
  for (std::size_t fi = 0; fi < faces.size(); ++fi) {
    const Face& f = faces[fi];
    if (f.loopCount < 3) return fail("face with loopCount < 3");
    if (f.firstLoop + f.loopCount > loops.size())
      return fail("face loop range out of the loop pool");
    for (u32 i = f.firstLoop; i < f.firstLoop + f.loopCount; ++i)
      if (loops[i].face != fi)
        return fail("loop outside its face's contiguous range");
  }
  {
    std::vector<bool> covered(loops.size(), false);
    for (const Face& f : faces)
      for (u32 i = f.firstLoop; i < f.firstLoop + f.loopCount; ++i)
        covered[i] = true;
    for (const bool c : covered)
      if (!c) return fail("loop not claimed by any face");
  }

  // (2) Edges unique, canonical, in range.
  std::unordered_map<u64, u32> edgeOf;
  for (std::size_t i = 0; i < edges.size(); ++i)
    edgeOf.emplace(edgeKey(edges[i].v0, edges[i].v1), static_cast<u32>(i));
  if (edgeOf.size() != edges.size()) return fail("duplicate edge");
  for (const Edge& ed : edges) {
    if (!(ed.v0 < ed.v1)) return fail("edge not canonical (v0 < v1)");
    if (ed.v1 >= positions.size()) return fail("edge vertex out of range");
  }

  // Loop rings: next/prev shape the face in order; each loop names its edge.
  for (u32 f = 0; f < faces.size(); ++f) {
    for (u32 i = 0; i < faces[f].loopCount; ++i) {
      const u32 li = faces[f].firstLoop + i;
      const u32 liNext = faces[f].firstLoop + (i + 1) % faces[f].loopCount;
      const u32 liPrev = faces[f].firstLoop +
                         (i + faces[f].loopCount - 1) % faces[f].loopCount;
      if (loops[li].next != liNext || loops[li].prev != liPrev)
        return fail("loop ring broken (next/prev)");
      const u32 va = loops[li].vertex, vb = loops[liNext].vertex;
      const auto it = edgeOf.find(edgeKey(va, vb));
      if (it == edgeOf.end()) return fail("loop edge does not exist");
      if (it->second != loops[li].edge) return fail("loop names the wrong edge");
    }
  }

  // (3)+(4) Recompute usage from the loops; the STORED CSR must agree.
  {
    std::unordered_map<u64, u32> edgeFaceUse;
    std::vector<std::vector<u32>> vEdges(positions.size());
    std::vector<std::vector<u32>> vFaces(positions.size());
    for (u32 f = 0; f < faces.size(); ++f) {
      for (u32 i = 0; i < faces[f].loopCount; ++i) {
        const u32 li = faces[f].firstLoop + i;
        const u32 liNext = faces[f].firstLoop + (i + 1) % faces[f].loopCount;
        const u32 va = loops[li].vertex, vb = loops[liNext].vertex;
        const u32 eid = edgeOf.at(edgeKey(va, vb));
        ++edgeFaceUse[edgeKey(va, vb)];
        pushUnique(vEdges[va], eid);
        pushUnique(vEdges[vb], eid);
        pushUnique(vFaces[va], f);
        pushUnique(vFaces[vb], f);
      }
    }
    if (edgeFaceUse.size() != edges.size())
      return fail("unused edge present (0-face edge)");
    for (const auto& kv : edgeFaceUse)
      if (kv.second > 2) return fail("edge with 3+ faces (non-manifold)");

    for (std::size_t v = 0; v < positions.size(); ++v) {
      if (vertexEdgeOffsets[v + 1] - vertexEdgeOffsets[v] != vEdges[v].size())
        return fail("vertexEdges CSR size mismatch");
      for (u32 k = vertexEdgeOffsets[v]; k < vertexEdgeOffsets[v + 1]; ++k)
        if (std::find(vEdges[v].begin(), vEdges[v].end(), vertexEdges[k]) ==
            vEdges[v].end())
          return fail("vertexEdges CSR content mismatch");
      if (vertexFaceOffsets[v + 1] - vertexFaceOffsets[v] != vFaces[v].size())
        return fail("vertexFaces CSR size mismatch");
      for (u32 k = vertexFaceOffsets[v]; k < vertexFaceOffsets[v + 1]; ++k)
        if (std::find(vFaces[v].begin(), vFaces[v].end(), vertexFaces[k]) ==
            vFaces[v].end())
          return fail("vertexFaces CSR content mismatch");
    }
    for (u32 e = 0; e < edges.size(); ++e) {
      if (edgeFaceOffsets[e + 1] - edgeFaceOffsets[e] < 1 ||
          edgeFaceOffsets[e + 1] - edgeFaceOffsets[e] > 2)
        return fail("edgeFaces CSR: edge must carry 1 or 2 faces");
      for (u32 k = edgeFaceOffsets[e]; k < edgeFaceOffsets[e + 1]; ++k) {
        const u32 fi = edgeFaces[k];
        bool uses = false;
        for (u32 i = 0; i < faces[fi].loopCount; ++i) {
          const u32 li = faces[fi].firstLoop + i;
          const u32 liNext =
              faces[fi].firstLoop + (i + 1) % faces[fi].loopCount;
          if (edgeOf.at(edgeKey(loops[li].vertex, loops[liNext].vertex)) == e) {
            uses = true;
            break;
          }
        }
        if (!uses) return fail("edgeFaces CSR content mismatch");
      }
    }
  }

  // (5) positions == vertices; no orphans (ops remove them, never accumulate).
  {
    std::vector<bool> used(positions.size(), false);
    for (const Loop& l : loops) {
      if (l.vertex >= positions.size()) return fail("loop vertex out of range");
      used[l.vertex] = true;
    }
    for (const bool u : used)
      if (!u) return fail("orphan vertex");
  }

  if (vertexEdgeOffsets.size() != positions.size() + 1 ||
      vertexFaceOffsets.size() != positions.size() + 1 ||
      edgeFaceOffsets.size() != edges.size() + 1)
    return fail("CSR offsets size mismatch");

  return true;
}

// --------------------------------------------- adjacency (stored, not derived)

// The public shared CSR fallback: rebuild from loops/faces. fromMeshData and
// mutation ops that take the fallback path use this; anything that adds or
// removes an element then bumps topologyVersion.
void EditableMesh::rebuildAdjacency() {
  const u32 vc = static_cast<u32>(positions.size());
  vertexEdgeOffsets.assign(vc + 1, 0);
  vertexFaceOffsets.assign(vc + 1, 0);
  edgeFaceOffsets.assign(edges.size() + 1, 0);
  vertexEdges.clear();
  vertexFaces.clear();
  edgeFaces.clear();

  std::unordered_map<u64, u32> edgeOf;
  edgeOf.reserve(edges.size() * 2);
  for (std::size_t i = 0; i < edges.size(); ++i)
    edgeOf.emplace(edgeKey(edges[i].v0, edges[i].v1), static_cast<u32>(i));

  std::vector<std::vector<u32>> vE(vc), vF(vc);
  std::vector<std::vector<u32>> eF(edges.size());
  for (u32 f = 0; f < faces.size(); ++f)
    for (u32 k = 0; k < faces[f].loopCount; ++k) {
      const u32 li = faces[f].firstLoop + k;
      const u32 liNext = faces[f].firstLoop + (k + 1) % faces[f].loopCount;
      const u32 va = loops[li].vertex, vb = loops[liNext].vertex;
      const auto it = edgeOf.find(edgeKey(va, vb));
      if (it == edgeOf.end()) continue;   // validate() refuses this upstream
      const u32 eid = it->second;
      pushUnique(vE[va], eid);
      pushUnique(vE[vb], eid);
      pushUnique(vF[va], f);
      pushUnique(vF[vb], f);
      pushUnique(eF[eid], f);
    }

  u32 acc = 0;
  for (u32 v = 0; v < vc; ++v) {
    vertexEdgeOffsets[v] = acc;
    for (const u32 x : vE[v]) vertexEdges.push_back(x);
    acc += static_cast<u32>(vE[v].size());
  }
  vertexEdgeOffsets[vc] = acc;

  acc = 0;
  for (u32 v = 0; v < vc; ++v) {
    vertexFaceOffsets[v] = acc;
    for (const u32 x : vF[v]) vertexFaces.push_back(x);
    acc += static_cast<u32>(vF[v].size());
  }
  vertexFaceOffsets[vc] = acc;

  acc = 0;
  for (std::size_t ei = 0; ei < edges.size(); ++ei) {
    edgeFaceOffsets[ei] = acc;
    for (const u32 x : eF[ei]) edgeFaces.push_back(x);
    acc += static_cast<u32>(eF[ei].size());
  }
  edgeFaceOffsets[edges.size()] = acc;
}

// --------------------------------------------- restructure funnel (item 111)

// Rebuild everything from survivor rings: loops/edges/faces/CSR, bump
// topologyVersion. Refuses (leaves `this` untouched) when the result fails
// validate() — so no op can leave a half-mesh.
static bool buildFromRings(const std::vector<Vec3>& positions,
                           const std::vector<Vec3>& normals,
                           const std::vector<std::vector<u32>>& rings,
                           const std::vector<u32>& materials, u32 oldVersion,
                           EditableMesh& out) {
  EditableMesh fresh;
  fresh.positions = positions;
  fresh.normals = normals;
  fresh.faces.resize(rings.size());
  for (std::size_t f = 0; f < rings.size(); ++f) {
    Face& face = fresh.faces[f];
    face.firstLoop = static_cast<u32>(fresh.loops.size());
    face.loopCount = static_cast<u32>(rings[f].size());
    face.materialIndex = f < materials.size() ? materials[f] : 0u;
    fresh.loops.resize(fresh.loops.size() + rings[f].size());
    for (u32 k = 0; k < face.loopCount; ++k) {
      const u32 li = face.firstLoop + k;
      Loop& l = fresh.loops[li];
      l.vertex = rings[f][k];
      l.face = static_cast<u32>(f);
      l.next = face.firstLoop + (k + 1) % face.loopCount;
      l.prev = face.firstLoop + (face.loopCount + k - 1) % face.loopCount;
    }
  }
  std::unordered_map<u64, u32> edgeOf;
  for (Face& face : fresh.faces) {
    for (u32 k = 0; k < face.loopCount; ++k) {
      const u32 li = face.firstLoop + k;
      const u32 liNext = face.firstLoop + (k + 1) % face.loopCount;
      const u64 key = edgeKey(fresh.loops[li].vertex, fresh.loops[liNext].vertex);
      const auto it = edgeOf.find(key);
      if (it == edgeOf.end()) {
        const u32 id = static_cast<u32>(edgeOf.size());
        edgeOf.emplace(key, id);
        fresh.edges.push_back(Edge{std::min(fresh.loops[li].vertex, fresh.loops[liNext].vertex),
                                   std::max(fresh.loops[li].vertex, fresh.loops[liNext].vertex)});
      }
      fresh.loops[li].edge = edgeOf.at(key);
    }
  }
  fresh.topologyVersion = oldVersion + 1;
  fresh.rebuildAdjacency();
  if (!fresh.validate(nullptr)) return false;   // refused: out untouched
  out = std::move(fresh);
  return true;
}

// ------------------------------------------------------- conversion (D34)

EditableMesh EditableMesh::fromMeshData(const MeshData& m) {
  EditableMesh e;
  const u32 vertexCount = static_cast<u32>(m.positions.size() / 3);
  e.positions.resize(vertexCount);
  for (u32 v = 0; v < vertexCount; ++v)
    e.positions[v] = Vec3{m.positions[v * 3], m.positions[v * 3 + 1],
                          m.positions[v * 3 + 2]};
  e.normals.resize(m.normals.size() / 3);   // ADDITIVE field, verbatim
  for (std::size_t i = 0; i < e.normals.size(); ++i)
    e.normals[i] = Vec3{m.normals[i * 3], m.normals[i * 3 + 1],
                        m.normals[i * 3 + 2]};

  const std::size_t triCount = m.indices.size() / 3;
  e.faces.resize(triCount);
  e.loops.resize(triCount * 3);
  std::unordered_map<u64, u32> edgeOf;
  edgeOf.reserve(triCount * 4 + 8);
  for (std::size_t t = 0; t < triCount; ++t) {
    const u32 fid = static_cast<u32>(t);
    Face& f = e.faces[fid];
    f.firstLoop = static_cast<u32>(t * 3);
    f.loopCount = 3;
    f.materialIndex = 0;
    const u32 ids[3] = {m.indices[t * 3], m.indices[t * 3 + 1],
                        m.indices[t * 3 + 2]};
    for (u32 k = 0; k < 3; ++k) {
      const u32 li = f.firstLoop + k;
      Loop& l = e.loops[li];
      l.vertex = ids[k];
      l.face = fid;
      l.next = f.firstLoop + (k + 1) % 3;
      l.prev = f.firstLoop + (k + 2) % 3;
      if (m.uvs.size() >= (static_cast<std::size_t>(l.vertex) + 1) * 2) {
        l.uv.x = m.uvs[static_cast<std::size_t>(l.vertex) * 2];
        l.uv.y = m.uvs[static_cast<std::size_t>(l.vertex) * 2 + 1];
      } else {
        l.uv.x = 0.0f;
        l.uv.y = 0.0f;
      }
      const u64 key = edgeKey(l.vertex, ids[(k + 1) % 3]);
      const auto it = edgeOf.find(key);
      if (it == edgeOf.end()) {
        const u32 newEid = static_cast<u32>(e.edges.size());
        e.edges.push_back(Edge{std::min(l.vertex, ids[(k + 1) % 3]),
                               std::max(l.vertex, ids[(k + 1) % 3])});
        edgeOf.emplace(key, newEid);
        l.edge = newEid;
      } else {
        l.edge = it->second;
      }
    }
  }
  e.rebuildAdjacency();
  return e;
}

MeshData EditableMesh::toMeshData() const {
  MeshData m;

  // ---- output vertices: one per ORIGINAL vertex, plus corner splits. -----
  // Per original vertex: the DISTINCT uvs carried by its loops, first-seen
  // order. One distinct uv -> the vertex is reused as-is (identity for
  // MeshData-born meshes, whose uvs are per-vertex and never disagree).
  // More than one distinct uv -> ONE split per extra uv: new output vertex,
  // positions/normals copied, uv = that corner's uv (one-split-per-corner).
  std::vector<std::vector<Vec2>> vUvs(positions.size());
  for (const Loop& l : loops) {
    if (l.vertex >= positions.size()) continue;
    auto& lst = vUvs[l.vertex];
    if (std::none_of(lst.begin(), lst.end(), [&](const Vec2& u) {
          return u.x == l.uv.x && u.y == l.uv.y;
        }))
      lst.push_back(l.uv);
  }

  // vOutIds[v][k] = output vertex id for original vertex v, uv slot k.
  std::vector<std::vector<u32>> vOutIds(positions.size());
  for (u32 v = 0; v < vertexCount(); ++v) {
    const auto& lst = vUvs[v];
    if (lst.empty()) {
      // Defensive: an orphan vertex (validate() refuses upstream) — emit
      // unchanged so the output still round-trips the position.
      vOutIds[v].push_back(static_cast<u32>(m.positions.size() / 3));
      m.positions.push_back(positions[v].x);
      m.positions.push_back(positions[v].y);
      m.positions.push_back(positions[v].z);
      m.normals.push_back(v < normals.size() ? normals[v].x : 0.0f);
      m.normals.push_back(v < normals.size() ? normals[v].y : 0.0f);
      m.normals.push_back(v < normals.size() ? normals[v].z : 0.0f);
      m.uvs.push_back(0.0f);
      m.uvs.push_back(0.0f);
      continue;
    }
    for (const Vec2& u : lst) {
      vOutIds[v].push_back(static_cast<u32>(m.positions.size() / 3));
      m.positions.push_back(positions[v].x);
      m.positions.push_back(positions[v].y);
      m.positions.push_back(positions[v].z);
      m.normals.push_back(v < normals.size() ? normals[v].x : 0.0f);
      m.normals.push_back(v < normals.size() ? normals[v].y : 0.0f);
      m.normals.push_back(v < normals.size() ? normals[v].z : 0.0f);
      m.uvs.push_back(u.x);
      m.uvs.push_back(u.y);
    }
  }

  // ---- faces: loop corner -> output vertex id, then triangulate. ---------
  std::vector<u32> rids(loops.size(), 0);
  for (std::size_t li = 0; li < loops.size(); ++li) {
    const Loop& l = loops[li];
    if (l.vertex >= positions.size()) continue;
    const auto& lst = vUvs[l.vertex];
    u32 k = 0;
    for (; k < lst.size(); ++k)
      if (lst[k].x == l.uv.x && lst[k].y == l.uv.y) break;
    if (k >= lst.size()) k = 0;   // defensive: validate() refuses this state
    rids[li] = vOutIds[l.vertex][k];
  }

  for (const Face& f : faces) {
    if (f.loopCount == 3) {
      // Triangle face: identity order for MeshData-born meshes (no split,
      // no re-triangulation) — indices come back in the original order.
      m.indices.push_back(rids[f.firstLoop]);
      m.indices.push_back(rids[f.firstLoop + 1]);
      m.indices.push_back(rids[f.firstLoop + 2]);
      continue;
    }
    // n-gon: ear-clip the ring (concave-safe).
    std::vector<Poly2> poly;
    if (!ringTo2D(*this, f, poly)) {
      // Degenerate ring (validate-level defect): fan fallback (still drawn).
      for (u32 i = 0; i < f.loopCount; ++i) {
        m.indices.push_back(rids[f.firstLoop]);
        m.indices.push_back(rids[f.firstLoop + (i + 1) % f.loopCount]);
        m.indices.push_back(rids[f.firstLoop + (i + 2) % f.loopCount]);
      }
      continue;
    }
    float signedArea = 0.0f;
    for (std::size_t i = 0; i < poly.size(); ++i) {
      const Poly2& A = poly[i];
      const Poly2& B = poly[(i + 1) % poly.size()];
      signedArea += A.x * B.y - B.x * A.y;
    }
    std::vector<u32> ringRids;
    if (signedArea > 0.0f) {
      for (u32 i = 0; i < f.loopCount; ++i)
        ringRids.push_back(rids[f.firstLoop + i]);
    } else {
      for (u32 i = 0; i < f.loopCount; ++i)
        ringRids.push_back(rids[f.firstLoop + f.loopCount - 1 - i]);
    }
    std::vector<u32> tri;
    earClip(poly, tri);
    for (const u32 ringIdx : tri)
      m.indices.push_back(ringRids[ringIdx]);
  }
  return m;
}

// ---------------------------------------------------------------- item 111

// Vertex-id ring of a face in loop order.
static std::vector<u32> faceRing(const EditableMesh& e, const Face& f) {
  std::vector<u32> ring;
  ring.reserve(f.loopCount);
  for (u32 i = 0; i < f.loopCount; ++i)
    ring.push_back(e.loops[f.firstLoop + i].vertex);
  return ring;
}

// Shared purge after face kills: drop killed faces, rebuild edges from the
// surviving rings, purge 0-face edges + loop-less vertices, bump version.
static bool purgeAfterFaceKills(const EditableMesh& e,
                                const std::vector<bool>& killed,
                                EditableMesh& out) {
  std::vector<std::vector<u32>> rings;         // survivor rings (object space)
  std::vector<u32> materials;
  const u32 faceTotal = static_cast<u32>(e.faces.size());
  for (u32 f = 0; f < faceTotal; ++f) {
    if (f < killed.size() && killed[f]) continue;
    rings.push_back(faceRing(e, e.faces[f]));
    materials.push_back(e.faces[f].materialIndex);
  }
  // Vertices still used by surviving rings.
  std::vector<bool> usedV(e.positions.size(), false);
  for (const auto& r : rings)
    for (const u32 v : r) usedV[v] = true;

  std::vector<u32> forward(e.positions.size(), 0xFFFFFFFFu);
  std::vector<Vec3> p, nrm;
  for (u32 v = 0; v < e.positions.size(); ++v) {
    if (!usedV[v]) continue;
    forward[v] = static_cast<u32>(p.size());
    p.push_back(e.positions[v]);
    nrm.push_back(v < e.normals.size() ? e.normals[v] : Vec3{0, 0, 0});
  }
  if (p.empty()) return false;   // everything orphaned: empty mesh refused
  std::vector<std::vector<u32>> compact;
  for (const auto& r : rings) {
    std::vector<u32> ring;
    ring.reserve(r.size());
    for (const u32 v : r) ring.push_back(forward[v]);
    compact.push_back(std::move(ring));
  }
  return buildFromRings(p, nrm, compact, materials, e.topologyVersion, out);
}

bool EditableMesh::moveVertices(const std::vector<u32>& vertexIds,
                                const Vec3& delta) {
  for (const u32 v : vertexIds)
    if (v >= positions.size()) return false;
  // positions edit = indices stable = NO topologyVersion bump (D34 rule).
  for (const u32 v : vertexIds)
    positions[v] = Vec3{positions[v].x + delta.x, positions[v].y + delta.y,
                        positions[v].z + delta.z};
  return true;
}

bool EditableMesh::setVertexPosition(u32 vertex, const Vec3& position) {
  if (vertex >= positions.size()) return false;
  positions[vertex] = position;
  return true;
}

// Item 128: absolute batch set. ALL-OR-NOTHING: validate everything first,
// then write — a single bad id leaves the mesh bit-untouched.
bool EditableMesh::setVertexPositions(
    const std::vector<u32>& vertexIds, const std::vector<Vec3>& newPositions) {
  if (vertexIds.size() != newPositions.size()) return false;  // half-apply refused
  for (const u32 v : vertexIds)
    if (v >= this->positions.size()) return false;          // half-apply refused
  for (std::size_t i = 0; i < vertexIds.size(); ++i)
    this->positions[vertexIds[i]] = newPositions[i];
  // Positions-only: no topologyVersion bump (D34 header rule at item 128).
  return true;
}

bool EditableMesh::extrudeFace(u32 faceIndex, const Vec3& localOffset) {
  if (faceIndex >= faces.size()) return false;

  const Face f = faces[faceIndex];
  const std::vector<u32> ring = faceRing(*this, f);
  const u32 n = static_cast<u32>(ring.size());

  // GUARD: every ring edge must carry one or two faces. The selected face is
  // removed before the walls are rebuilt, so an edge shared by this face and
  // one neighbor remains two-manifold (neighbor + wall). Three or more faces
  // would still be non-manifold after removing the selected face.
  for (u32 k = 0; k < n; ++k) {
    const u32 li = f.firstLoop + k;
    const u32 liNext = f.firstLoop + (k + 1) % n;
    const u64 key = edgeKey(loops[li].vertex, loops[liNext].vertex);
    u32 eid = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < edges.size(); ++i)
      if (edgeKey(edges[i].v0, edges[i].v1) == key) {
        eid = static_cast<u32>(i);
        break;
      }
    if (eid == 0xFFFFFFFFu) return false;
    const u32 adjacentFaces = edgeFaceOffsets[eid + 1] - edgeFaceOffsets[eid];
    if (adjacentFaces == 0 || adjacentFaces > 2) return false;
  }

  EditableMesh before = *this;   // rollback: never leave a half-mesh

  // Duplicate the ring (fresh vertices, same positions/normals).
  std::vector<u32> freshRing(n);
  for (u32 i = 0; i < n; ++i) {
    const u32 id = static_cast<u32>(positions.size());
    positions.push_back(positions[ring[i]]);
    if (normals.size() == static_cast<std::size_t>(id))
      normals.push_back(normals.size() > ring[i] ? normals[ring[i]]
                                                 : Vec3{0, 0, 0});
    freshRing[i] = id;
  }
  for (const u32 id : freshRing)
    positions[id] = Vec3{positions[id].x + localOffset.x,
                         positions[id].y + localOffset.y,
                         positions[id].z + localOffset.z};

  // Rings: wall quads (original vs fresh ring), then the face rebuilt over
  // the fresh ring. All other faces keep their rings verbatim.
  std::vector<std::vector<u32>> rings;
  std::vector<u32> materials;
  for (u32 g = 0; g < faces.size(); ++g) {
    if (g == faceIndex) continue;
    rings.push_back(faceRing(*this, faces[g]));
    materials.push_back(faces[g].materialIndex);
  }
  for (u32 i = 0; i < n; ++i) {
    const u32 j = (i + 1) % n;
    rings.push_back(
        std::vector<u32>{ring[i], ring[j], freshRing[j], freshRing[i]});
    materials.push_back(f.materialIndex);
  }
  rings.push_back(freshRing);
  materials.push_back(f.materialIndex);

  if (!buildFromRings(positions, normals, rings, materials, topologyVersion,
                      *this)) {
    *this = before;
    return false;
  }
  return true;
}

bool EditableMesh::deleteVertices(const std::vector<u32>& vertexIds) {
  if (vertexIds.empty()) return false;   // no-op, refused (not bumped)
  EditableMesh before = *this;
  std::vector<bool> killed(faces.size(), false);
  bool anyKilled = false;
  for (std::size_t f = 0; f < faces.size(); ++f) {
    for (u32 i = 0; i < faces[f].loopCount; ++i) {
      const u32 v = loops[faces[f].firstLoop + i].vertex;
      for (const u32 id : vertexIds) {
        if (id == v) {
          if (v < positions.size()) {
            killed[f] = true;
            anyKilled = true;
          }
          break;
        }
      }
      if (killed[f] || anyKilled) break;
    }
    if (killed[f]) break;
  }
  (void)anyKilled;
  EditableMesh out;
  if (!purgeAfterFaceKills(*this, killed, out)) {
    *this = before;   // would empty the mesh
    return false;
  }
  *this = std::move(out);
  return true;
}

bool EditableMesh::deleteEdges(const std::vector<u32>& edgeIds) {
  if (edgeIds.empty()) return false;
  EditableMesh before = *this;
  std::unordered_set<u32> listed;
  for (const u32 e : edgeIds) listed.insert(e);
  std::vector<bool> killed(faces.size(), false);
  for (std::size_t f = 0; f < faces.size(); ++f)
    for (u32 i = 0; i < faces[f].loopCount; ++i) {
      const u32 li = faces[f].firstLoop + i;
      const u32 liNext = faces[f].firstLoop + (i + 1) % faces[f].loopCount;
      const u64 key = edgeKey(loops[li].vertex, loops[liNext].vertex);
      u32 eid = 0xFFFFFFFFu;
      for (std::size_t e2 = 0; e2 < edges.size(); ++e2)
        if (edgeKey(edges[e2].v0, edges[e2].v1) == key) {
          eid = static_cast<u32>(e2);
          break;
        }
      if (eid != 0xFFFFFFFFu && listed.count(eid)) {
        killed[f] = true;
        break;
      }
    }
  EditableMesh out;
  if (!purgeAfterFaceKills(*this, killed, out)) {
    *this = before;
    return false;
  }
  *this = std::move(out);
  return true;
}

bool EditableMesh::deleteFaces(const std::vector<u32>& faceIds) {
  if (faceIds.empty()) return false;
  EditableMesh before = *this;
  std::vector<bool> killed(faces.size(), false);
  for (const u32 f : faceIds) {
    if (f >= faces.size()) {
      *this = before;
      return false;
    }
    killed[f] = true;
  }
  EditableMesh out;
  if (!purgeAfterFaceKills(*this, killed, out)) {
    *this = before;
    return false;
  }
  *this = std::move(out);
  return true;
}

bool EditableMesh::mergeVertices(const std::vector<u32>& vertexIds,
                                 const Vec3& to) {
  if (vertexIds.size() < 2) return false;   // a merge needs >= 2 vertices
  EditableMesh before = *this;
  bool allValid = true;
  for (const u32 v : vertexIds)
    if (v >= positions.size()) allValid = false;
  if (!allValid) return false;

  const u32 survivor = vertexIds[0];
  for (std::size_t i = 1; i < vertexIds.size(); ++i) {
    const u32 v = vertexIds[i];
    if (v == survivor) continue;
    for (Loop& l : loops)
      if (l.vertex == v) l.vertex = survivor;
  }
  positions[survivor] = to;

  // Faces that degenerate (adjacent duplicate pair or <3 distinct vertices)
  // are deleted; the rest keep the merge.
  std::vector<bool> killed(faces.size(), false);
  for (std::size_t f = 0; f < faces.size(); ++f) {
    std::vector<u32> ring;
    for (u32 i = 0; i < faces[f].loopCount; ++i)
      ring.push_back(loops[faces[f].firstLoop + i].vertex);
    bool degenerate = false;
    for (u32 i = 0; i < ring.size(); ++i)
      if (ring[i] == ring[(i + 1) % ring.size()]) { degenerate = true; break; }
    std::vector<u32> distinct;
    for (const u32 v : ring)
      if (std::find(distinct.begin(), distinct.end(), v) == distinct.end())
        distinct.push_back(v);
    if (distinct.size() < 3) degenerate = true;
    if (degenerate) killed[f] = true;
  }
  EditableMesh out;
  if (!purgeAfterFaceKills(*this, killed, out)) {
    *this = before;   // everything collapsed: empty mesh refused
    return false;
  }
  *this = std::move(out);
  return true;
}

} // namespace ember::scene
