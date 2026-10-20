// EditableMesh.h — the editable mesh, D34 (frozen), scene-owned.
//
// DESIGN: Blender's bmesh expressed with indices instead of pointers. That
// choice buys three properties the tests and undo need: an EditableMesh
// survives reallocation, it serialises to .ember directly, and a test can
// COMPARE it (a pointer graph cannot).
//
// OWNERSHIP: scene's, like Object and Collection (D8). The editor links scene
// and reads it for selection and overlays; RENDER NEVER SEES IT — render keeps
// consuming MeshData, so nothing in the render path changes.
//
// ROUND TRIP (the two things the topology tests assert):
//   * MeshData -> EditableMesh -> MeshData is an IDENTITY for any MeshData
//     our primitives produce: positions, indices, normals, uvs equal. This
//     header carries one ADDITIVE field beyond the frozen block — the
//     per-vertex `normals` array — so the normals identity holds VERBATIM:
//     fromMeshData copies them, toMeshData writes them back unchanged
//     (flat-by-default derivation would break the sphere's smooth per-vertex
//     normals). Flagged to the Lead for the ack/delete decision.
//   * The asymmetry is documented, not hidden: polygon grouping does not
//     survive a MeshData round trip (MeshData is triangles by construction),
//     so EditableMesh -> MeshData -> EditableMesh == e exactly only when e's
//     faces are all triangles with per-vertex-consistent corner attributes.
//   * toMeshData() triangulates with EAR CLIPPING (concave n-gons correct;
//     a fan over a concave face produces overlapping triangles) and splits a
//     corner into a new vertex ONLY when two faces sharing that vertex
//     disagree on a corner attribute. It does not mutate the source.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace ember::scene {

// Corner of a face. Per-corner attributes (uv) live HERE, which is what makes
// flat and smooth shading representable and the MeshData conversion exact.
struct Loop {
  u32 vertex;   // vertex id (== index into positions)
  u32 edge;     // the undirected edge (v(vertex), v(next))
  u32 next;     // the next corner of the same face
  u32 prev;     // the previous corner of the same face
  u32 face;     // owning face
  Vec2 uv;      // per-corner uv
};

// Undirected edge; v0 < v1 by construction (validate() refuses otherwise).
struct Edge {
  u32 v0, v1;
};

// n-gon face; loopCount >= 3, concave allowed (the triangulation handles it).
struct Face {
  u32 firstLoop;
  u32 loopCount;
  u32 materialIndex = 0;   // stored per D22; render still uses slot 0 (residue)
};

// Sub-element selection (D34): index sets stamped with the topologyVersion
// they were made at. A selection made at version v is INVALID at v+1 unless
// an op remapped it (move: indices stable) or cleared it (extrude/delete/
// merge: indices shift) — ops must never leave it dangling. The editor holds
// the active set per mode; these live NEXT to the frozen struct, additive.
// The mode is CORE's SelectMode (Types.h, D35): D37 deleted the duplicate
// scene-side enum — two vertex/edge/face enums is the conversion-function
// drift class (D21), and core's is the one render needs.
struct MeshSelection {
  SelectMode mode = SelectMode::Vertex;
  u32 topologyVersion = 0;
  std::vector<u32> indices;   // positions / edges / faces per the mode
  bool sameVersion(u32 v) const { return topologyVersion == v; }
  bool empty() const { return indices.empty(); }
  bool contains(u32 id) const {
    for (const u32 i : indices)
      if (i == id) return true;
    return false;
  }
};

struct EditableMesh {
  std::vector<Vec3>  positions;   // vertex positions, index == vertex id
  // ADDITIVE to the frozen block (flagged to the Lead): per-vertex normals,
  // preserved verbatim through conversion so the normals identity holds.
  std::vector<Vec3>  normals;     // fromMeshData copies; toMeshData writes back
  std::vector<Loop>  loops;       // every face's corners, contiguous per face
  std::vector<Edge>  edges;       // unique undirected edges
  std::vector<Face>  faces;       // n-gons; n >= 3, concave allowed

  // STORED adjacency, CSR (contiguous, index arithmetic only): maintained by
  // construction and by every topology change, NEVER derived on demand.
  std::vector<u32>  vertexEdgeOffsets, vertexEdges;   // vertex -> its edges
  std::vector<u32>  vertexFaceOffsets, vertexFaces;   // vertex -> its faces
  std::vector<u32>  edgeFaceOffsets,   edgeFaces;     // edge -> its faces (1|2)
  u32 topologyVersion = 0;   // bumped by every topology change; position
                             // edits are exempt (indices stable)

  static EditableMesh fromMeshData(const MeshData&);  // one face per triangle
  MeshData toMeshData() const;   // triangulate (ear clip); split corners only
                                 // on attribute conflict; pure (no mutation)
  bool validate(std::string* why = nullptr) const;    // the D34 invariants

  // ADDITIVE method (flagged to the Lead with the normals/hasEdit notes):
  // the shared CSR fallback. Hand-built meshes and mutation ops that choose
  // the fallback path call this, then see topologyVersion bumped. Item 111's
  // ops will normally maintain adjacency incrementally instead.
  void rebuildAdjacency();

  // ---- topology mutation ops (item 111; data level, D34-refusing) ------
  // Every op: (a) refuses and leaves the mesh BIT-UNTOUCHEN when the result
  // would violate the D34 invariants (e.g. a 3-face edge) or when it is a
  // no-op; (b) on success bumps topologyVersion; (c) does NOT remap external
  // index references — per the D34 rule, callers holding MeshSelections must
  // CLEAR them on version change (delete/merge/extrude shift ids; move does
  // not bump).
  //
  // moveVertices/setVertexPosition/setVertexPositions edit positions
  // only — indices stable, so NO topologyVersion bump (and `move` selections
  // stay valid).
  //
  // ABSOLUTE batch set (item 128; the modal operators' numeric entry):
  // sets each listed vertex to its EXACT mesh-local position in one pass —
  // ALL-OR-NOTHING: if any id is out of range or the arrays disagree in
  // length, nothing is written (refuse rather than half-apply). No
  // topologyVersion bump; the caller's MeshSelection stays valid.
  bool setVertexPositions(const std::vector<u32>& vertexIds,
                          const std::vector<Vec3>& positions);
  bool moveVertices(const std::vector<u32>& vertexIds, const Vec3& delta);
  bool setVertexPosition(u32 vertex, const Vec3& position);

  // Delete faces that use any listed vertex, then purge now-unused edges
  // and orphaned vertices (D34 invariant 5: no leftovers). All SURVIVING
  // ids stay in order; listed ids die (compaction may shift survivors).
  bool deleteVertices(const std::vector<u32>& vertexIds);
  // Delete the faces using each listed edge (Blender's edge-delete), then
  // purge unused edges + orphaned vertices.
  bool deleteEdges(const std::vector<u32>& edgeIds);
  // Delete the faces; purge unused edges + orphaned vertices.
  bool deleteFaces(const std::vector<u32>& faceIds);

  // Blender's extrude: duplicate the face's ring (new vertices at the same
  // positions), turn each original ring edge into a side quad, rebuild the
  // face over the NEW ring, then displace the new ring by localOffset
  // (object space — zero is valid for a modal extrude that moves afterward).
  // Ring edges may have one or two incident faces before extrusion: the
  // selected face is removed, so at most one neighboring face remains beside
  // each new wall. Three-or-more incident faces are refused as non-manifold.
  bool extrudeFace(u32 faceIndex, const Vec3& localOffset);

  // Merge the listed vertices into ONE vertex placed at `to` (Blender's
  // merge-at-cursor), retarget every loop, then delete faces that became
  // degenerate (adjacent duplicate pairs / fewer than 3 distinct vertices)
  // and purge now-dead edges + the absorbed vertices.
  bool mergeVertices(const std::vector<u32>& vertexIds, const Vec3& to);

  u32 vertexCount() const { return static_cast<u32>(positions.size()); }
  u32 faceCount() const { return static_cast<u32>(faces.size()); }
};

} // namespace ember::scene
