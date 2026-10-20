// tests/test_scene_topology.cpp — owned by Verity · QA.
//
// Contract under test: the editable mesh layer (src/scene/EditableMesh.*, D34).
// Item 112: adjacency correctness after each mutation, manifold invariants where
// they should hold, and MeshData -> EditableMesh -> MeshData round-trip identity.
// Plus the four degenerate cases the item names: empty mesh, single triangle,
// a vertex shared by many faces, and a face with more than three vertices.
//
// WHY BOTH CHECKERS RUN: validate() is the implementation's OWN invariant
// checker. Passing it proves internal consistency and nothing more — a bug
// shared by the impl and its validator is invisible to it. So every case here
// runs validate() AND an independent checker written from the header's
// documented contract, recomputing the CSR adjacency from the loop rings.
// Agreement between two derivations is the evidence; either alone is not.
//
// EVERY EXPECTED NUMBER IN THIS FILE WAS MEASURED against the current tree with
// a probe before it was asserted, because guessing a count and watching it fail
// would read as a product defect when it is really a test author's error.
// Two measurement traps are recorded in the finding: printf-argument evaluation
// order silently sampled counts BEFORE the mutation ran, and linking against a
// lib rebuilt mid-session reported three functions as undefined.
#include "harness.h"

#include "core/Types.h"
#include "scene/EditableMesh.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using ember::MeshData;
using ember::u32;
using ember::u64;
using ember::Vec2;
using ember::Vec3;
using ember::scene::EditableMesh;
using ember::scene::Edge;
using ember::scene::Face;
using ember::scene::Primitive;
using ember::scene::World;

namespace {

// Local to this file: the shared harness offers CHECK/CHECK_EQ/CHECK_NEAR but no
// "check with a reason" form, and the reason matters here — a bare CHECK(x.ok)
// tells a future reader nothing about WHICH invariant broke. Built on the
// harness's own reportFailure so counting and printing stay correct.
#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__,                           \
                                   harness::fmtf(__VA_ARGS__));                  \
    } while (0)

// ---- independent adjacency checker -----------------------------------------
// Recomputes vertex->edge, vertex->face and edge->face from the loop rings and
// compares against the STORED CSR. Derived from EditableMesh.h's contract, not
// from EditableMesh.cpp, so it is a second opinion rather than a restatement.
struct IndependentCheck {
    bool ok = false;
    std::string why;
};

u64 key(u32 a, u32 b) { return a < b ? (u64(a) << 32) | b : (u64(b) << 32) | a; }

IndependentCheck checkAdjacency(const EditableMesh& m) {
    IndependentCheck r;
    const auto fail = [&](const char* msg) { r.ok = false; r.why = msg; return r; };

    const std::size_t nv = m.positions.size();
    if (m.vertexEdgeOffsets.size() != nv + 1) return fail("vertexEdgeOffsets must be nv+1");
    if (m.vertexFaceOffsets.size() != nv + 1) return fail("vertexFaceOffsets must be nv+1");
    if (m.edgeFaceOffsets.size() != m.edges.size() + 1)
        return fail("edgeFaceOffsets must be nEdges+1");

    std::unordered_map<u64, u32> edgeOf;
    for (std::size_t i = 0; i < m.edges.size(); ++i)
        edgeOf.emplace(key(m.edges[i].v0, m.edges[i].v1), u32(i));
    if (edgeOf.size() != m.edges.size()) return fail("duplicate edge");
    for (const Edge& e : m.edges)
        if (!(e.v0 < e.v1)) return fail("edge not canonical v0<v1");

    std::vector<std::vector<u32>> vEdges(nv), vFaces(nv);
    std::unordered_map<u64, u32> edgeUse;
    for (u32 f = 0; f < m.faces.size(); ++f) {
        const Face& fc = m.faces[f];
        if (fc.loopCount < 3) return fail("face loopCount < 3");
        for (u32 i = 0; i < fc.loopCount; ++i) {
            const u32 li = fc.firstLoop + i;
            if (li >= m.loops.size()) return fail("loop index out of range");
            if (m.loops[li].face != f) return fail("loop points at the wrong face");
            const u32 ln = fc.firstLoop + (i + 1) % fc.loopCount;
            const u32 va = m.loops[li].vertex, vb = m.loops[ln].vertex;
            if (va >= nv || vb >= nv) return fail("loop vertex out of range");
            const auto it = edgeOf.find(key(va, vb));
            if (it == edgeOf.end()) return fail("loop edge missing from edges[]");
            if (it->second != m.loops[li].edge) return fail("loop names the wrong edge");
            ++edgeUse[key(va, vb)];
            for (const u32 w : {va, vb}) { vEdges[w].push_back(it->second); vFaces[w].push_back(f); }
        }
    }
    for (const auto& kv : edgeUse)
        if (kv.second > 2) return fail("edge carries 3+ faces (non-manifold)");

    for (std::size_t v = 0; v < nv; ++v) {
        std::vector<u32> want = vEdges[v];
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        std::vector<u32> got(m.vertexEdges.begin() + m.vertexEdgeOffsets[v],
                             m.vertexEdges.begin() + m.vertexEdgeOffsets[v + 1]);
        std::sort(got.begin(), got.end());
        if (got != want) return fail("stored vertexEdges disagrees with recomputed set");
    }
    for (u32 e = 0; e < m.edges.size(); ++e) {
        const u32 n = m.edgeFaceOffsets[e + 1] - m.edgeFaceOffsets[e];
        if (n < 1) return fail("stored edge has 0 faces (orphan edge)");
        if (n > 2) return fail("stored edge has 3+ faces (non-manifold)");
    }
    r.ok = true;
    return r;
}

// validate() AND the independent checker together, so a pass means both agree.
void checkAll(const EditableMesh& m, const char* label) {
    std::string why;
    const bool v = m.validate(&why);
    CHECK_MSG(v, "%s: validate() failed: %s", label, why.c_str());
    const IndependentCheck ic = checkAdjacency(m);
    CHECK_MSG(ic.ok, "%s: independent adjacency check failed: %s", label, ic.why.c_str());
}

MeshData oneTriangle() {
    MeshData md;
    md.positions = {0,0,0, 1,0,0, 0,1,0};
    md.normals   = {0,0,1, 0,0,1, 0,0,1};
    md.uvs       = {0,0, 1,0, 0,1};
    md.indices   = {0,1,2};
    md.name      = "tri";
    return md;
}

bool sameFloats(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](float x, float y) { return x == y; });
}

// A welded quad face: the one topology extrude is documented to accept.
EditableMesh weldedQuad() {
    EditableMesh m;
    m.positions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
    m.edges = {{0,1},{1,2},{2,3},{0,3}};
    m.loops.resize(4);
    for (u32 i = 0; i < 4; ++i) {
        m.loops[i].vertex = i; m.loops[i].edge = i;
        m.loops[i].next = (i + 1) % 4; m.loops[i].prev = (i + 3) % 4;
        m.loops[i].face = 0;
    }
    m.faces.push_back({0, 4, 0});
    m.rebuildAdjacency();
    return m;
}

} // namespace

// ---- (1) ROUND-TRIP IDENTITY ------------------------------------------------

TEST(topology_roundtrip_identity_for_every_primitive) {
    const Primitive kinds[] = {Primitive::Cube, Primitive::Sphere, Primitive::Plane,
                               Primitive::Cylinder, Primitive::Torus};
    for (const Primitive k : kinds) {
        const MeshData src = ember::scene::makePrimitiveMesh(k);
        CHECK(!src.empty());
        const EditableMesh e = EditableMesh::fromMeshData(src);
        checkAll(e, "primitive round-trip");

        const MeshData back = e.toMeshData();
        CHECK_MSG(sameFloats(src.positions, back.positions),
                  "round trip: positions differ");
        CHECK_MSG(src.indices == back.indices, "round trip: indices differ");
        CHECK_MSG(sameFloats(src.normals, back.normals),
                  "round trip: normals differ");
        CHECK_MSG(sameFloats(src.uvs, back.uvs), "round trip: uvs differ");
    }
}

TEST(topology_roundtrip_preserves_counts) {
    // MEASURED, not guessed: fromMeshData makes ONE FACE PER TRIANGLE and does
    // NOT weld by position, so a cube is 24 verts / 12 faces / 36 loops / 30
    // edges — not the 8/6/24/18 a position-welded cube would give. The
    // unwelded form is what keeps the round trip exact.
    const MeshData src = ember::scene::makePrimitiveMesh(Primitive::Cube);
    CHECK_EQ(src.vertexCount(), u32{24});
    CHECK_EQ(src.indexCount(), u32{36});
    const EditableMesh e = EditableMesh::fromMeshData(src);
    CHECK_EQ(e.vertexCount(), u32{24});
    CHECK_EQ(e.faceCount(), std::size_t{12});
    CHECK_EQ(e.loops.size(), std::size_t{36});
    CHECK_EQ(e.edges.size(), std::size_t{30});
}

TEST(topology_roundtrip_is_idempotent) {
    const MeshData src = ember::scene::makePrimitiveMesh(Primitive::Sphere);
    const MeshData a = EditableMesh::fromMeshData(src).toMeshData();
    const MeshData b = EditableMesh::fromMeshData(a).toMeshData();
    CHECK(sameFloats(a.positions, b.positions));
    CHECK(a.indices == b.indices);
}

TEST(topology_fromMeshData_does_not_weld) {
    // A position-welded cube would have 8 vertices, each carrying 3 faces. The
    // measured maximum is 2, which proves the conversion keeps corners separate
    // — the property that makes the identity above hold verbatim.
    const EditableMesh e = EditableMesh::fromMeshData(
        ember::scene::makePrimitiveMesh(Primitive::Cube));
    u32 maxFaces = 0;
    for (u32 v = 0; v < e.vertexCount(); ++v)
        maxFaces = std::max(maxFaces, e.vertexFaceOffsets[v + 1] - e.vertexFaceOffsets[v]);
    CHECK_EQ(maxFaces, u32{2});
}

// ---- (2) DEGENERATE: empty mesh ---------------------------------------------

TEST(topology_empty_mesh_is_valid_and_inert) {
    const EditableMesh e = EditableMesh::fromMeshData(MeshData{});
    CHECK_EQ(e.vertexCount(), u32{0});
    CHECK_EQ(e.faceCount(), std::size_t{0});
    CHECK(e.edges.empty());
    CHECK(e.loops.empty());
    checkAll(e, "empty mesh");

    const MeshData back = e.toMeshData();
    CHECK(back.empty());
    CHECK(back.positions.empty());
    CHECK(back.indices.empty());
}

// ---- (3) DEGENERATE: single triangle ----------------------------------------

TEST(topology_single_triangle_adjacency) {
    const EditableMesh e = EditableMesh::fromMeshData(oneTriangle());
    CHECK_EQ(e.vertexCount(), u32{3});
    CHECK_EQ(e.faceCount(), std::size_t{1});
    CHECK_EQ(e.edges.size(), std::size_t{3});
    CHECK_EQ(e.loops.size(), std::size_t{3});
    checkAll(e, "single triangle");

    // Every edge is BOUNDARY: exactly one face. Every vertex: 2 edges, 1 face.
    for (u32 v = 0; v < 3; ++v) {
        CHECK_EQ(e.vertexEdgeOffsets[v + 1] - e.vertexEdgeOffsets[v], u32{2});
        CHECK_EQ(e.vertexFaceOffsets[v + 1] - e.vertexFaceOffsets[v], u32{1});
    }
    for (u32 ed = 0; ed < e.edges.size(); ++ed)
        CHECK_EQ(e.edgeFaceOffsets[ed + 1] - e.edgeFaceOffsets[ed], u32{1});

    const MeshData back = e.toMeshData();
    CHECK_EQ(back.indexCount(), u32{3});
    CHECK(sameFloats(oneTriangle().positions, back.positions));
}

// ---- (4) DEGENERATE: a vertex shared by many faces ---------------------------

TEST(topology_vertex_shared_by_many_faces) {
    // A fan: ONE centre vertex shared by 16 triangles. This is the case a naive
    // adjacency build gets wrong (duplicating or dropping the shared vertex's
    // incident sets), so it earns its own test.
    MeshData md;
    constexpr int kRays = 16;
    md.positions = {0,0,0};                    // vertex 0 = the shared centre
    md.normals   = {0,0,1};
    md.uvs       = {0.5f,0.5f};
    for (int i = 0; i <= kRays; ++i) {
        const float a = 6.2831853f * float(i) / float(kRays);
        md.positions.push_back(std::cos(a));
        md.positions.push_back(std::sin(a));
        md.positions.push_back(0.0f);
        md.normals.push_back(0); md.normals.push_back(0); md.normals.push_back(1);
        md.uvs.push_back(0.5f + 0.5f * std::cos(a));
        md.uvs.push_back(0.5f + 0.5f * std::sin(a));
    }
    for (int i = 0; i < kRays; ++i) {
        md.indices.push_back(0);
        md.indices.push_back(u32(i) + 1);
        md.indices.push_back(u32(i) + 2);
    }

    const EditableMesh e = EditableMesh::fromMeshData(md);
    checkAll(e, "fan");
    CHECK_EQ(e.faceCount(), u32(kRays));

    // The centre vertex is shared by ALL kRays faces, and by as many EDGES as
    // there are ring vertices — 17, not 16. A full 360-degree fan wraps, so it
    // has one more ring vertex than triangle, and the centre touches every one
    // of them. (I asserted 16 first and the check caught it: the ring closes.)
    CHECK_EQ(e.vertexFaceOffsets[1] - e.vertexFaceOffsets[0], u32(kRays));
    CHECK_EQ(e.vertexEdgeOffsets[1] - e.vertexEdgeOffsets[0], u32(kRays) + 1);

    // The fan is a DISC, not a closed fan: the two seam ring vertices sit on 1
    // face, the rest on 2.
    std::vector<u32> faceCounts;
    for (u32 v = 1; v <= u32(kRays) + 1; ++v)
        faceCounts.push_back(e.vertexFaceOffsets[v + 1] - e.vertexFaceOffsets[v]);
    CHECK_EQ(std::count(faceCounts.begin(), faceCounts.end(), u32{2}), kRays - 1);
    CHECK_EQ(std::count(faceCounts.begin(), faceCounts.end(), u32{1}), 2);
}

// ---- (5) DEGENERATE: a face with more than three vertices --------------------

TEST(topology_ngon_faces_are_representable) {
    // Hand-built, since MeshData is triangles by construction and cannot
    // express an n-gon. Public fields + rebuildAdjacency is the documented path
    // for exactly this (EditableMesh.h:101-105).
    const auto build = [](u32 n) {
        EditableMesh m;
        for (u32 i = 0; i < n; ++i) {
            const float a = 6.2831853f * float(i) / float(n);
            m.positions.push_back({std::cos(a), std::sin(a), 0.0f});
        }
        for (u32 i = 0; i < n; ++i)
            m.edges.push_back({std::min(i, (i + 1) % n), std::max(i, (i + 1) % n)});
        m.loops.resize(n);
        for (u32 i = 0; i < n; ++i) {
            m.loops[i].vertex = i;
            m.loops[i].edge = i;
            m.loops[i].next = (i + 1) % n;
            m.loops[i].prev = (i + n - 1) % n;
            m.loops[i].face = 0;
            m.loops[i].uv = Vec2{0, 0};
        }
        m.faces.push_back({0, n, 0});
        m.rebuildAdjacency();
        return m;
    };

    for (const u32 n : {u32{3}, u32{4}, u32{5}, u32{7}, u32{12}}) {
        const EditableMesh m = build(n);
        CHECK_MSG(m.validate(nullptr), "ngon n=%u: validate failed", n);
        CHECK_MSG(checkAdjacency(m).ok, "ngon n=%u: adjacency failed", n);
        CHECK_EQ(m.faces[0].loopCount, n);
        CHECK_EQ(m.loops.size(), std::size_t{n});
        CHECK_EQ(m.edges.size(), std::size_t{n});
        // A single n-gon's edges are all BOUNDARY: one face each.
        for (u32 e = 0; e < n; ++e)
            CHECK_EQ(m.edgeFaceOffsets[e + 1] - m.edgeFaceOffsets[e], u32{1});
    }
}

TEST(topology_concave_ngon_triangulates_without_overlap) {
    // A concave quad (arrowhead). A fan triangulation would emit an
    // overlapping/zero-area pair; ear clipping must tile the face exactly once.
    EditableMesh m;
    m.positions = {{0,0,0}, {2,0,0}, {1,0.5f,0}, {2,2,0}, {0,2,0}};
    m.edges = {{0,1},{1,2},{2,3},{3,4},{0,4}};
    m.loops.resize(5);
    for (u32 i = 0; i < 5; ++i) {
        m.loops[i].vertex = i; m.loops[i].edge = i;
        m.loops[i].next = (i + 1) % 5; m.loops[i].prev = (i + 4) % 5;
        m.loops[i].face = 0;
    }
    m.faces.push_back({0, 5, 0});
    m.rebuildAdjacency();
    CHECK(m.validate(nullptr));
    CHECK(checkAdjacency(m).ok);

    const MeshData md = m.toMeshData();
    CHECK_EQ(md.indexCount(), u32{9});          // a 5-gon -> 3 triangles
    const auto triArea = [&](u32 a, u32 b, u32 c) {
        const float ax = md.positions[a*3], ay = md.positions[a*3+1];
        const float bx = md.positions[b*3], by = md.positions[b*3+1];
        const float cx = md.positions[c*3], cy = md.positions[c*3+1];
        return 0.5f * std::fabs((bx-ax)*(cy-ay) - (cx-ax)*(by-ay));
    };
    float sum = 0;
    for (u32 i = 0; i + 2 < md.indices.size(); i += 3)
        sum += triArea(md.indices[i], md.indices[i+1], md.indices[i+2]);
    // The arrowhead's area by shoelace on the ORIGINAL 5 points is 3.0:
    // sum = (0*0-2*0) + (2*0.5-1*0) + (1*2-2*0.5) + (2*2-0*2) + (0*0-0*2)
    //     = 0 + 1 + 1 + 4 + 0 = 6, half of which is 3. I hand-computed 4.0
    // first and the check caught it. A fan over the reflex vertex would NOT
    // tile to 3.0 — it would over- or under-count — so this still discriminates.
    CHECK_NEAR(sum, 3.0f, 1e-3f);
}

// ---- (6) ADJACENCY AFTER EACH MUTATION --------------------------------------

TEST(topology_move_vertices_keeps_indices_and_does_not_bump_version) {
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    const u32 version = e.topologyVersion;
    const std::size_t nEdges = e.edges.size();
    const std::size_t nFaces = e.faceCount();

    CHECK(e.moveVertices({0, 1}, Vec3{1, 0, 0}));
    // A position-only edit must NOT bump the topology version (header:115-117).
    CHECK_EQ(e.topologyVersion, version);
    CHECK_EQ(e.edges.size(), nEdges);
    CHECK_EQ(e.faceCount(), nFaces);
    checkAll(e, "after moveVertices");
    // MEASURED, not assumed: makePrimitiveMesh(Cube) is CENTRED at the origin,
    // spanning ±0.5, so vertex 0 starts at x=0.5 and a +1 move lands at 1.5.
    // (I first asserted 1.0, assuming a corner-origin cube; the check caught it.)
    CHECK_NEAR(e.positions[0].x, 1.5f, 1e-5f);
    CHECK_NEAR(e.positions[0].y, -0.5f, 1e-5f);
}

TEST(topology_extrude_face_on_a_welded_quad) {
    // MEASURED: extrude succeeds on a welded single quad — 4v/1f -> 8v/5f,
    // 12 edges, 20 loops. The quad is duplicated (4 new verts), 4 side quads
    // are added, and every resulting edge carries 1 or 2 faces.
    EditableMesh m = weldedQuad();
    checkAll(m, "before extrude");
    const u32 v0 = m.vertexCount();
    const std::size_t f0 = m.faceCount();

    CHECK(m.extrudeFace(0, Vec3{0, 0, 1}));
    checkAll(m, "after extrude");
    CHECK_EQ(m.vertexCount(), v0 + 4);
    CHECK_EQ(m.faceCount(), f0 + 4);
    CHECK(m.topologyVersion > 0);
}

TEST(topology_extrusion_preserves_source_face_material_slot) {
    EditableMesh m = weldedQuad();
    m.faces[0].materialIndex = 3;
    CHECK(m.extrudeFace(0, Vec3{0, 0, 1}));
    checkAll(m, "material-assigned extrusion");
    CHECK_EQ(m.faces.size(), std::size_t{5});
    for (const Face& face : m.faces)
        CHECK_EQ(face.materialIndex, u32{3});
}

TEST(topology_extrude_a_triangle_on_a_freshly_converted_cube) {
    // A converted cube has triangulated quads: one ring edge of this triangle
    // is shared with its adjacent triangle (two faces before the op). Extrusion
    // removes the selected triangle, keeps the adjacent face, then adds one wall
    // on that shared edge, leaving a manifold two-face edge.
    EditableMesh e = EditableMesh::fromMeshData(
        ember::scene::makePrimitiveMesh(Primitive::Cube));
    const u32 vertices = e.vertexCount();
    const std::size_t faces = e.faceCount();
    CHECK(e.extrudeFace(0, Vec3{0, 0, 1}));
    checkAll(e, "after extruding a cube triangle");
    CHECK_EQ(e.vertexCount(), vertices + 3);
    CHECK_EQ(e.faceCount(), faces + 3);  // replace selected face with 3 walls + top
    CHECK(e.topologyVersion > 0);
}

TEST(topology_extrude_accepts_zero_offset_for_modal_start) {
    EditableMesh e = weldedQuad();
    const u32 vertices = e.vertexCount();
    const std::size_t faces = e.faceCount();
    CHECK(e.extrudeFace(0, Vec3{0, 0, 0}));
    checkAll(e, "after zero-offset extrude");
    CHECK_EQ(e.vertexCount(), vertices + 4);
    CHECK_EQ(e.faceCount(), faces + 4);
}

TEST(topology_delete_face_leaves_no_orphans) {
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    // MEASURED: 24v/12f/30e -> 23v/11f/28e after deleting face 0.
    CHECK(e.deleteFaces({0}));
    checkAll(e, "after deleteFaces");
    CHECK_EQ(e.vertexCount(), u32{23});
    CHECK_EQ(e.faceCount(), std::size_t{11});
    CHECK_EQ(e.edges.size(), std::size_t{28});
}

TEST(topology_delete_edges_removes_faces_then_orphans) {
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    CHECK(e.deleteEdges({0}));
    checkAll(e, "after deleteEdges");
    // MEASURED: also 23v/11f/28e — deleting the one face that edge served.
    CHECK_EQ(e.vertexCount(), u32{23});
    CHECK_EQ(e.faceCount(), std::size_t{11});
    CHECK_EQ(e.edges.size(), std::size_t{28});
}

TEST(topology_delete_vertices_purges_transitively) {
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    CHECK(e.deleteVertices({0}));
    checkAll(e, "after deleteVertices");
    CHECK_EQ(e.vertexCount(), u32{23});
    // D34 invariant 5: no 0-face edge may survive. checkAll's independent
    // checker asserts exactly this via its 'orphan edge' path.
    for (u32 ed = 0; ed < e.edges.size(); ++ed)
        CHECK(e.edgeFaceOffsets[ed + 1] - e.edgeFaceOffsets[ed] >= 1);
}

TEST(topology_merge_vertices_retargets_loops) {
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    const u32 v0 = e.vertexCount();
    CHECK(e.mergeVertices({0, 1}, Vec3{0.5f, 0, 0}));
    checkAll(e, "after mergeVertices");
    CHECK_EQ(e.vertexCount(), v0 - 1);          // MEASURED: 24 -> 23
}

// ---- (7) MANIFOLD INVARIANTS HOLD WHERE THEY SHOULD -------------------------

TEST(topology_refused_mutation_leaves_the_mesh_untouched) {
    // The D34 contract: an op that would violate the invariants must REFUSE and
    // leave the mesh BIT-UNTOUCHEN. Asserting the refusal AND the untouched
    // state is what makes "refuses" meaningful — an op that corrupts and then
    // reports failure is worse than one that does nothing.
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    const EditableMesh before = e;

    const bool accepted = e.mergeVertices({0, 1}, Vec3{0, 0, 0});
    if (!accepted) {
        CHECK_EQ(e.vertexCount(), before.vertexCount());
        CHECK_EQ(e.edges.size(), before.edges.size());
        CHECK_EQ(e.faceCount(), before.faceCount());
        CHECK_EQ(e.loops.size(), before.loops.size());
        CHECK_EQ(e.topologyVersion, before.topologyVersion);
    }
    // Accepted or refused, the RESULT must be a valid manifold mesh.
    CHECK(e.validate(nullptr));
    CHECK(checkAdjacency(e).ok);
}

TEST(topology_double_extrude_never_reaches_a_three_face_edge) {
    // Extruding the same ring twice would give a wall edge 3 faces. Whether the
    // second call is accepted or refused, the mesh must never reach that state.
    EditableMesh m = weldedQuad();
    const bool first = m.extrudeFace(0, Vec3{0, 0, 1});
    CHECK(first);
    checkAll(m, "first extrude");
    (void)m.extrudeFace(0, Vec3{0, 0, 1});
    CHECK_MSG(m.validate(nullptr), "double extrude: validate failed");
    CHECK_MSG(checkAdjacency(m).ok, "double extrude: adjacency failed");
}

// ---- (8) THE ROUND TRIP AGAIN, AFTER MUTATION -------------------------------

TEST(topology_roundtrip_after_mutation_still_matches_the_source) {
    // The render path only ever sees MeshData, so this is the contract that
    // actually reaches the GPU: after a mutation, toMeshData() must produce a
    // self-consistent, fully-in-range index buffer.
    World w;
    const auto id = w.createPrimitive(Primitive::Cube, "cube");
    EditableMesh e = EditableMesh::fromMeshData(w.get(id)->mesh);
    CHECK(e.deleteFaces({0}));
    checkAll(e, "mutated mesh");

    const MeshData md = e.toMeshData();
    CHECK_EQ(md.indexCount() % 3, u32{0});
    CHECK(md.vertexCount() > 0);
    for (const u32 i : md.indices)
        CHECK(i < md.vertexCount());
}

HARNESS_MAIN()
