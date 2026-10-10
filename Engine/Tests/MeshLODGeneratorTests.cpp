#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/MeshLODGeometry.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshLODThresholds.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include "StagedTestPaths.h"

using namespace GameEngine;

namespace {

// Build an N x N subdivided plane in the XZ plane. Produces a smooth,
// highly-tessellated surface that meshopt_simplify can reduce substantially.
Mesh MakeGridPlane(uint32 n) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    mesh.Vertices.reserve(static_cast<size_t>(n) * n);
    for (uint32 z = 0; z < n; ++z) {
        for (uint32 x = 0; x < n; ++x) {
            Vertex v;
            v.Position[0] = static_cast<float>(x);
            v.Position[1] = 0.0f;
            v.Position[2] = static_cast<float>(z);
            v.Normal[1]   = 1.0f;
            v.TexCoords[0] = static_cast<float>(x) / static_cast<float>(n - 1);
            v.TexCoords[1] = static_cast<float>(z) / static_cast<float>(n - 1);
            mesh.Vertices.push_back(v);
        }
    }
    for (uint32 z = 0; z < n - 1; ++z) {
        for (uint32 x = 0; x < n - 1; ++x) {
            uint32 i0 = z * n + x;
            uint32 i1 = z * n + x + 1;
            uint32 i2 = (z + 1) * n + x;
            uint32 i3 = (z + 1) * n + x + 1;
            mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

uint32 MaxIndex(const Vector<uint32>& indices) {
    uint32 m = 0;
    for (uint32 i : indices) m = std::max(m, i);
    return m;
}

// Grid plane tagged as skinned: single-joint rigid bind so IsSkinned() is true.
Mesh MakeSkinnedGridPlane(uint32 n) {
    Mesh mesh = MakeGridPlane(n);
    mesh.Skinned = true;
    const size_t verts = mesh.Vertices.size();
    mesh.Joints0.assign(verts * 4, 0);
    mesh.Weights0.assign(verts * 4, 0.0f);
    for (size_t v = 0; v < verts; ++v)
        mesh.Weights0[v * 4] = 1.0f; // full influence on joint 0
    return mesh;
}

// Radially-closed cylinder with a DUPLICATED seam column. Column `radial` shares
// column 0's position but a different U (0..1 wrap), exactly like a UV sphere or
// a DCC-exported wrapped mesh. The duplicate is offset by a sub-feature epsilon
// so it is a NEAR-duplicate — what procedural trig round-off produces — which
// meshopt's exact internal position weld misses. Without a tolerant pre-weld the
// seam is an open border and each side collapses independently, opening a crack
// at LOD1+.
Mesh MakeSeamedCylinder(uint32 radial, uint32 rings) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kSeamEpsilon = 1.0e-6f; // near-duplicate offset (~trig round-off)
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const uint32 cols = radial + 1; // inclusive seam column duplicates column 0
    for (uint32 y = 0; y < rings; ++y) {
        for (uint32 r = 0; r < cols; ++r) {
            const uint32 ringIndex = (r == radial) ? 0u : r;
            const float angle = 2.0f * kPi * static_cast<float>(ringIndex) / static_cast<float>(radial);
            Vertex v;
            v.Position[0] = std::cos(angle);
            v.Position[1] = static_cast<float>(y);
            v.Position[2] = std::sin(angle) + (r == radial ? kSeamEpsilon : 0.0f);
            v.Normal[0]   = std::cos(angle);
            v.Normal[2]   = std::sin(angle);
            v.TexCoords[0] = static_cast<float>(r) / static_cast<float>(radial); // seam col U = 1.0
            v.TexCoords[1] = static_cast<float>(y) / static_cast<float>(rings - 1);
            mesh.Vertices.push_back(v);
        }
    }
    for (uint32 y = 0; y < rings - 1; ++y) {
        for (uint32 r = 0; r < radial; ++r) {
            const uint32 i0 = y * cols + r;
            const uint32 i1 = y * cols + r + 1;
            const uint32 i2 = (y + 1) * cols + r;
            const uint32 i3 = (y + 1) * cols + r + 1;
            mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

// Count boundary edges (referenced by exactly one triangle) in the POSITION-
// welded domain: vertices sharing a grid cell canonicalize to one representative
// so seam duplicates count as a single vertex. This measures watertightness
// independently of the generator's own weld — a torn seam shows up as extra
// boundary edges the closed surface should not have.
size_t CountWeldedBoundaryEdges(const Mesh& mesh, const Vector<uint32>& indices) {
    const size_t vertexCount = mesh.Vertices.size();
    if (vertexCount == 0) return 0;
    float minB[3] = {mesh.Vertices[0].Position[0], mesh.Vertices[0].Position[1], mesh.Vertices[0].Position[2]};
    float maxB[3] = {minB[0], minB[1], minB[2]};
    for (const auto& vtx : mesh.Vertices) {
        for (int c = 0; c < 3; ++c) {
            minB[c] = std::min(minB[c], vtx.Position[c]);
            maxB[c] = std::max(maxB[c], vtx.Position[c]);
        }
    }
    const float extent = std::max({maxB[0] - minB[0], maxB[1] - minB[1], maxB[2] - minB[2]});
    const float eps = std::max(extent * 1.0e-4f, 1.0e-8f);
    const auto cell = [eps](float f) { return static_cast<int64_t>(std::llround(static_cast<double>(f) / eps)); };

    std::map<std::array<int64_t, 3>, uint32> representative;
    Vector<uint32> canon(vertexCount);
    for (uint32 v = 0; v < vertexCount; ++v) {
        const std::array<int64_t, 3> key{cell(mesh.Vertices[v].Position[0]),
                                         cell(mesh.Vertices[v].Position[1]),
                                         cell(mesh.Vertices[v].Position[2])};
        canon[v] = representative.emplace(key, v).first->second;
    }

    std::map<std::pair<uint32, uint32>, int> edgeUses;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32 tri[3] = {canon[indices[i]], canon[indices[i + 1]], canon[indices[i + 2]]};
        for (int e = 0; e < 3; ++e) {
            uint32 a = tri[e], b = tri[(e + 1) % 3];
            if (a == b) continue; // degenerate edge (collapsed) — not a boundary
            if (a > b) std::swap(a, b);
            ++edgeUses[{a, b}];
        }
    }
    size_t boundary = 0;
    for (const auto& [edge, uses] : edgeUses)
        if (uses == 1) ++boundary;
    return boundary;
}

} // namespace

TEST(MeshLODGenerator, Lod0IsSourceUnchangedTopology) {
    Mesh mesh = MakeGridPlane(32);
    const size_t srcTris = mesh.Indices.size() / 3;

    MeshLODs lods = GenerateMeshLODs(mesh, {});

    ASSERT_GE(lods.LodCount(), 1u);
    EXPECT_EQ(lods.TriangleCount(0), srcTris);
    EXPECT_FLOAT_EQ(lods.LodErrors[0], 0.0f);
}

TEST(MeshLODGenerator, GeneratesDescendingLODs) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeGridPlane(64); // ~7938 triangles
    MeshLODConfig cfg;
    cfg.LodCount     = 4;
    cfg.TargetRatios[1] = 0.5f;
    cfg.TargetRatios[2] = 0.25f;
    cfg.TargetRatios[3] = 0.10f;

    MeshLODs lods = GenerateMeshLODs(mesh, cfg);

    // We expect more than one level on a smooth, reducible surface.
    ASSERT_GT(lods.LodCount(), 1u);

    // Strictly decreasing triangle counts across levels.
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        EXPECT_LT(lods.TriangleCount(lod), lods.TriangleCount(lod - 1))
            << "LOD " << lod << " not smaller than LOD " << (lod - 1);
    }

    // LOD1 should be in the neighbourhood of its 50% budget (generous bounds).
    const uint32 srcTris = lods.TriangleCount(0);
    EXPECT_LE(lods.TriangleCount(1), srcTris);
    EXPECT_GE(lods.TriangleCount(1), srcTris / 4);
}

TEST(MeshLODGenerator, AllIndicesReferenceSourceVertices) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeGridPlane(48);
    MeshLODs lods = GenerateMeshLODs(mesh, {});

    const uint32 vertexCount = static_cast<uint32>(mesh.Vertices.size());
    for (uint32 lod = 0; lod < lods.LodCount(); ++lod) {
        const auto& idx = lods.LodIndices[lod];
        ASSERT_EQ(idx.size() % 3u, 0u) << "LOD " << lod << " index count not a multiple of 3";
        ASSERT_FALSE(idx.empty());
        EXPECT_LT(MaxIndex(idx), vertexCount) << "LOD " << lod << " references out-of-range vertex";
    }
}

TEST(MeshLODGenerator, RespectsLodCountOfOne) {
    Mesh mesh = MakeGridPlane(32);
    MeshLODConfig cfg;
    cfg.LodCount = 1;
    MeshLODs lods = GenerateMeshLODs(mesh, cfg);
    EXPECT_EQ(lods.LodCount(), 1u);
}

TEST(MeshLODGenerator, NonTriangleTopologyReturnsLod0Only) {
    Mesh mesh = MakeGridPlane(16);
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Lines;
    MeshLODs lods = GenerateMeshLODs(mesh, {});
    EXPECT_EQ(lods.LodCount(), 1u);
}

TEST(MeshLODGenerator, ErrorAndSloppyArraysAreParallelToLodIndices) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    MeshLODs lods = GenerateMeshLODs(MakeGridPlane(64), {});
    ASSERT_GT(lods.LodCount(), 1u);
    EXPECT_EQ(lods.LodErrors.size(), lods.LodIndices.size());
    EXPECT_EQ(lods.LodSloppy.size(), lods.LodIndices.size());
    EXPECT_EQ(lods.LodSloppy[0], 0u);       // LOD0 is never sloppy
    for (float e : lods.LodErrors) EXPECT_GE(e, 0.0f);
}

TEST(MeshLODGenerator, IntoFillsParallelErrorAndSloppyOffByOne) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeGridPlane(64);
    const uint32 total = GenerateMeshLODsInto(mesh, {});
    ASSERT_GT(total, 1u);
    // ExtraLODErrors[j] is the error of ExtraLODs[j] == LOD(j+1); the three
    // parallel arrays stay the same size (LOD0's error is implicit).
    EXPECT_EQ(mesh.ExtraLODErrors.size(), mesh.ExtraLODs.size());
    EXPECT_EQ(mesh.ExtraLODSloppy.size(), mesh.ExtraLODs.size());
}

TEST(MeshLODGenerator, SkinnedSkipPredicate) {
    Mesh rigid = MakeGridPlane(16);
    Mesh skinned = MakeSkinnedGridPlane(16);
    ASSERT_FALSE(rigid.IsSkinned());
    ASSERT_TRUE(skinned.IsSkinned());
    // Rigid always generates; skinned only when explicitly opted in.
    EXPECT_TRUE(ShouldGenerateLODsForMesh(rigid, /*generateSkinned*/ false));
    EXPECT_TRUE(ShouldGenerateLODsForMesh(rigid, /*generateSkinned*/ true));
    EXPECT_FALSE(ShouldGenerateLODsForMesh(skinned, /*generateSkinned*/ false));
    EXPECT_TRUE(ShouldGenerateLODsForMesh(skinned, /*generateSkinned*/ true));
}

// A UV-seam duplicate column must not tear open during simplification. Because
// the generator position-welds before simplifying, the closed cylinder stays
// closed: a simplified level's welded boundary-edge count can only shrink
// (top/bottom rings collapse) and never exceed LOD0's. Without the weld the
// seam collapses per-side and this count climbs (regression guard).
TEST(MeshLODGenerator, SeamedMeshStaysWatertightAcrossLODs) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeSeamedCylinder(24, 12);
    // Border locks would also pin the seam and mask a weld regression — so
    // isolate the weld here by freeing every border.
    MeshLODConfig cfg;
    cfg.BorderRule = MeshLODBorderRule::Free;
    MeshLODs lods = GenerateMeshLODs(mesh, cfg);
    ASSERT_GT(lods.LodCount(), 1u) << "seamed cylinder should simplify past LOD0";

    const size_t lod0Boundary = CountWeldedBoundaryEdges(mesh, lods.LodIndices[0]);
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        const size_t boundary = CountWeldedBoundaryEdges(mesh, lods.LodIndices[lod]);
        EXPECT_LE(boundary, lod0Boundary)
            << "LOD " << lod << " opened " << (boundary > lod0Boundary ? boundary - lod0Boundary : 0)
            << " new boundary edges along the seam (weld regression)";
    }
}

// Welding only merges vertices that already share a position, so a genuinely
// open border (a plane's outer edge, no position twin) stays a border and
// LockAll still pins it. The plane keeps its full perimeter at every level.
TEST(MeshLODGenerator, OpenPlaneBorderPreservedWithLockAll) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeGridPlane(24);
    MeshLODConfig cfg;
    cfg.BorderRule = MeshLODBorderRule::LockAll;
    MeshLODs lods = GenerateMeshLODs(mesh, cfg);
    ASSERT_GT(lods.LodCount(), 1u);

    const size_t lod0Boundary = CountWeldedBoundaryEdges(mesh, lods.LodIndices[0]);
    EXPECT_GT(lod0Boundary, 0u) << "an open plane must have a border";
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        if (lods.LodSloppy[lod] != 0u)
            continue; // sloppy far level is the documented border-moving exception
        EXPECT_EQ(CountWeldedBoundaryEdges(mesh, lods.LodIndices[lod]), lod0Boundary)
            << "LockAll must preserve the plane perimeter at LOD " << lod;
    }
}

// The modular-kit crack fix. A flat kit tile (ground/wall piece) tiles against
// its neighbours along its outer perimeter, which lies on the piece's AABB
// face planes by construction — the default SeamPlanes rule locks exactly that
// perimeter, so kit seams stay watertight across LODs with no per-asset
// opt-in, while mesh-internal borders stay free to drift. The explicit-Free
// leg is the negative control that reproduces the cross-piece crack the lock
// prevents.
TEST(MeshLODGenerator, DefaultConfigPreservesSeamPlaneBorderThatCollapsesFree) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    EXPECT_EQ(MeshLODConfig{}.BorderRule, MeshLODBorderRule::SeamPlanes)
        << "auto-LOD must lock kit-seam borders by default";

    Mesh mesh = MakeGridPlane(24);
    const size_t lod0Boundary = CountWeldedBoundaryEdges(mesh, mesh.Indices);
    ASSERT_GT(lod0Boundary, 0u) << "an open plane must have a border";

    // Default config: the tile's perimeter (all on AABB planes) is preserved
    // at every non-sloppy LOD.
    MeshLODs locked = GenerateMeshLODs(mesh, {});
    ASSERT_GT(locked.LodCount(), 1u);
    for (uint32 lod = 1; lod < locked.LodCount(); ++lod) {
        if (locked.LodSloppy[lod] != 0u)
            continue;
        EXPECT_EQ(CountWeldedBoundaryEdges(mesh, locked.LodIndices[lod]), lod0Boundary)
            << "default config must preserve the seam-plane perimeter at LOD " << lod;
    }

    // Negative control: with free borders the perimeter collapses inward.
    MeshLODConfig unlocked;
    unlocked.BorderRule = MeshLODBorderRule::Free;
    MeshLODs open = GenerateMeshLODs(mesh, unlocked);
    ASSERT_GT(open.LodCount(), 1u);
    bool collapsedSomewhere = false;
    for (uint32 lod = 1; lod < open.LodCount(); ++lod)
        if (CountWeldedBoundaryEdges(mesh, open.LodIndices[lod]) != lod0Boundary)
            collapsedSomewhere = true;
    EXPECT_TRUE(collapsedSomewhere)
        << "free simplification must alter the open perimeter (the crack the lock prevents)";
}

// The no-level rule (bridge-arch holes fix). A one-quad-band shell (every
// vertex on the top or bottom open edge — the topology of the Elven props/
// statues) has both rings on its AABB's Y faces, so the default SeamPlanes
// rule locks every vertex and the mesh can never reduce watertight. The old
// behaviour retried such levels with UNLOCKED borders, emitting torn (cracked)
// LODs — real holes in the rendered mesh. Now a level that cannot reduce under
// its locks is simply not emitted: every non-sloppy level of the cook
// preserves the welded boundary-edge count exactly; only the documented sloppy
// far level may move borders.
TEST(MeshLODGenerator, SeamLockedCookNeverEmitsTornLevel) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh shell = MakeSeamedCylinder(48, 2); // rings == 2 => no interior vertices

    MeshLODConfig locked; // SeamPlanes by default; both rings sit on AABB faces
    ASSERT_EQ(locked.BorderRule, MeshLODBorderRule::SeamPlanes);
    const MeshLODs lods = GenerateMeshLODs(shell, locked);

    const size_t lod0Boundary = CountWeldedBoundaryEdges(shell, lods.LodIndices[0]);
    ASSERT_GT(lod0Boundary, 0u) << "the band shell must have open borders";
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        if (lods.LodSloppy[lod] != 0u)
            continue; // sloppy far level is the documented border-moving exception
        EXPECT_EQ(CountWeldedBoundaryEdges(shell, lods.LodIndices[lod]), lod0Boundary)
            << "non-sloppy LOD " << lod << " tore the locked border (cracked LOD)";
    }
}

// Graduated far levels: skipping an unemittable level must not truncate the
// chain. The band shell's mid levels (ratios .5/.25) cannot reduce watertight
// and are skipped, but the far level (ratio .10 <= SloppyRatioThreshold) still
// arrives via the sloppy fallback, so the open shell keeps distance savings
// without any torn non-sloppy level.
TEST(MeshLODGenerator, OpenShellKeepsFarSloppyLevelWhenMidLevelsSkip) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh shell = MakeSeamedCylinder(48, 2);

    MeshLODConfig cfg; // defaults: SeamPlanes locks (both rings on AABB faces) + AllowSloppy
    ASSERT_EQ(cfg.BorderRule, MeshLODBorderRule::SeamPlanes);
    ASSERT_TRUE(cfg.AllowSloppy);
    const MeshLODs lods = GenerateMeshLODs(shell, cfg);

    ASSERT_GT(lods.LodCount(), 1u)
        << "skipped mid levels must not truncate the chain before the sloppy far level";
    EXPECT_EQ(lods.LodSloppy[lods.LodCount() - 1u], 1u)
        << "the surviving far level must come from the sloppy fallback";
    EXPECT_LT(lods.TriangleCount(lods.LodCount() - 1u), lods.TriangleCount(0));
}

// The SeamPlanes/LockAll differential. A band shell whose rings do NOT lie on
// the AABB face planes (the band is rotated off-axis) is reducible under the
// default SeamPlanes rule — its borders are mesh-internal, free to drift
// within the error budget — while LockAll pins every border vertex and the
// same mesh cannot emit any non-sloppy level. This is the exact mechanism
// that unlocks mid-tier LODs on open-border kit content.
TEST(MeshLODGenerator, SeamPlanesFreesOffPlaneBordersLockAllPins) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh band = MakeSeamedCylinder(48, 2); // every vertex on an open ring border
    // Rotate off-axis so neither ring lies in an AABB face plane.
    const float ax = 0.53f, az = 0.41f; // arbitrary non-axis-aligned tilt
    const float ca = std::cos(ax), sa = std::sin(ax);
    const float cb = std::cos(az), sb = std::sin(az);
    for (Vertex& v : band.Vertices) {
        const float x = v.Position[0], y = v.Position[1], z = v.Position[2];
        const float y1 = y * ca - z * sa, z1 = y * sa + z * ca;   // rotate about X
        v.Position[0] = x * cb - y1 * sb;                          // rotate about Z
        v.Position[1] = x * sb + y1 * cb;
        v.Position[2] = z1;
    }

    MeshLODConfig seamPlanes; // default rule
    ASSERT_EQ(seamPlanes.BorderRule, MeshLODBorderRule::SeamPlanes);
    const MeshLODs freed = GenerateMeshLODs(band, seamPlanes);
    bool anyNonSloppyReduction = false;
    for (uint32 lod = 1; lod < freed.LodCount(); ++lod)
        if (freed.LodSloppy[lod] == 0u && freed.TriangleCount(lod) < freed.TriangleCount(0))
            anyNonSloppyReduction = true;
    EXPECT_TRUE(anyNonSloppyReduction)
        << "SeamPlanes must let off-plane borders collapse into a real mid level";

    MeshLODConfig lockAll;
    lockAll.BorderRule = MeshLODBorderRule::LockAll;
    const MeshLODs pinned = GenerateMeshLODs(band, lockAll);
    for (uint32 lod = 1; lod < pinned.LodCount(); ++lod)
        EXPECT_EQ(pinned.LodSloppy[lod], 1u)
            << "LockAll on an all-border band must only ever emit the sloppy far level";
    // Levels that failed the reduction gate under active locks are counted —
    // the cook log's visibility into lock-pinned (vs budget-pinned) chains.
    EXPECT_GT(pinned.LevelsSkippedWithLocks, 0u);
}

namespace {

// Bumpy grid plane plus a disconnected component: `detailExtent` controls the
// attached component's size relative to the plane (0..63 span). Returns the
// first vertex index of the component so survival is checkable per level.
Mesh MakePlaneWithFloater(float detailExtent, uint32* outDetailBase) {
    Mesh mesh = MakeGridPlane(64); // ~7938 tris spanning 0..63
    // Prune is error-ordered: a perfectly flat plane simplifies at ~zero error
    // and would reach its target before the incremental error ever climbs to
    // the component's removal cost. Gentle bumps give the coarse levels a real
    // error ramp, as any actual content has.
    for (Vertex& v : mesh.Vertices)
        v.Position[1] = 0.6f * std::sin(v.Position[0] * 0.35f) *
                        std::sin(v.Position[2] * 0.35f);
    const uint32 detailBase = static_cast<uint32>(mesh.Vertices.size());
    // A closed tetrahedron floating past the plane's edge.
    const float s = detailExtent;
    const float t[4][3] = {{70.0f, 0.2f, 0.5f},
                           {70.0f + s, 0.2f, 0.5f},
                           {70.0f + s * 0.5f, 0.2f, 0.5f + s},
                           {70.0f + s * 0.5f, 0.2f + s, 0.5f + s * 0.5f}};
    for (const auto& p : t) {
        Vertex v{};
        v.Position[0] = p[0]; v.Position[1] = p[1]; v.Position[2] = p[2];
        v.Normal[1] = 1.0f;
        mesh.Vertices.push_back(v);
    }
    const uint32 b = detailBase;
    mesh.Indices.insert(mesh.Indices.end(),
                        {b, b + 2u, b + 1u, b, b + 1u, b + 3u,
                         b + 1u, b + 2u, b + 3u, b, b + 3u, b + 2u});
    if (outDetailBase)
        *outDetailBase = detailBase;
    return mesh;
}

bool AnyIndexAtOrAbove(const Vector<uint32>& idx, uint32 base) {
    return std::any_of(idx.begin(), idx.end(), [&](uint32 i) { return i >= base; });
}

} // namespace

// Coarse levels (ratio <= the prune ceiling) simplify over the pruned source,
// shedding micro components within kPruneErrorBudget; near levels must keep
// every component. AllowSloppy is disabled so the coarse assertion always runs
// against a quality level (a sloppy fallback would silently skip it).
TEST(MeshLODGenerator, PruneDropsSmallComponentOnlyAtCoarseLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    uint32 detailBase = 0;
    Mesh mesh = MakePlaneWithFloater(0.2f, &detailBase); // ~0.3% of extent
    MeshLODConfig config;
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 2u);

    ASSERT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_TRUE(AnyIndexAtOrAbove(lods.LodIndices[1], detailBase))
        << "near level (unpruned source) must keep the small component";
    const uint32 last = lods.LodCount() - 1u;
    ASSERT_EQ(lods.LodSloppy[last], 0u);
    EXPECT_FALSE(AnyIndexAtOrAbove(lods.LodIndices[last], detailBase))
        << "coarsest quality level (pruned source) should shed the micro component";
}

// The prune budget is DECOUPLED from the per-level error budgets: a component
// well above kPruneErrorBudget in extent must survive every quality level even
// though the far level's generous TargetError (0.35) would license deleting it
// under v6's coupled pruning. Pins the defect class: vine
// garlands and window-decoration cascades (0.34 of mesh extent) vanishing at
// mid tiers.
TEST(MeshLODGenerator, PruneNeverEatsComponentsAboveItsOwnBudget) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    uint32 detailBase = 0;
    Mesh mesh = MakePlaneWithFloater(18.0f, &detailBase); // ~28% of extent
    MeshLODConfig config;
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 2u);
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        ASSERT_EQ(lods.LodSloppy[lod], 0u);
        EXPECT_TRUE(AnyIndexAtOrAbove(lods.LodIndices[lod], detailBase))
            << "LOD" << lod << " deleted a structurally-sized component";
    }
}

namespace {

// A mesh whose PRUNED source is far SMALLER than a coarse level's budget: one
// structural component carrying few triangles plus a swarm of micro floaters
// carrying most of them (cloud rings, shelf clutter, scattered decoration).
// Every floater sits inside kPruneErrorBudget, so the coarse-level prune sheds
// all of them and hands the simplifier an input smaller than the level budget —
// which is a fraction of the ORIGINAL index count, not of the pruned source.
// `outFloaterBase` is the first floater vertex; `outBodyIndexCount` is what the
// prune must leave behind.
Mesh MakeSparseBodyWithFloaterSwarm(uint32 floatersPerAxis, uint32* outFloaterBase,
                                    size_t* outBodyIndexCount) {
    constexpr float kSpan = 100.0f;         // the structural component spans the mesh
    constexpr float kFloaterSize = 0.2f;    // 0.2% of extent — well inside the prune budget
    constexpr float kFloaterHeight = 5.0f;  // lifted clear of the body in Y
    constexpr uint32 kBodyVertsPerAxis = 6; // coarse: large radius, few triangles

    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const float step = kSpan / static_cast<float>(kBodyVertsPerAxis - 1);
    for (uint32 z = 0; z < kBodyVertsPerAxis; ++z) {
        for (uint32 x = 0; x < kBodyVertsPerAxis; ++x) {
            Vertex v{};
            v.Position[0] = static_cast<float>(x) * step;
            v.Position[2] = static_cast<float>(z) * step;
            v.Normal[1] = 1.0f;
            v.TexCoords[0] = static_cast<float>(x) / static_cast<float>(kBodyVertsPerAxis - 1);
            v.TexCoords[1] = static_cast<float>(z) / static_cast<float>(kBodyVertsPerAxis - 1);
            mesh.Vertices.push_back(v);
        }
    }
    for (uint32 z = 0; z + 1 < kBodyVertsPerAxis; ++z) {
        for (uint32 x = 0; x + 1 < kBodyVertsPerAxis; ++x) {
            const uint32 i0 = z * kBodyVertsPerAxis + x;
            const uint32 i1 = i0 + 1;
            const uint32 i2 = i0 + kBodyVertsPerAxis;
            const uint32 i3 = i2 + 1;
            mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    const size_t bodyIndexCount = mesh.Indices.size();
    const uint32 floaterBase = static_cast<uint32>(mesh.Vertices.size());

    // Closed tetrahedra, spaced far enough apart that each is its own component
    // and none extends the mesh AABB past the body in X or Z. The body is flat
    // at y=0, so the floaters do define the mesh's whole Y range (0 -> 5.2) —
    // the extent that drives the prune budget is still the 100-unit X/Z span.
    const float spacing = kSpan / static_cast<float>(floatersPerAxis);
    for (uint32 fz = 0; fz < floatersPerAxis; ++fz) {
        for (uint32 fx = 0; fx < floatersPerAxis; ++fx) {
            const float ox = static_cast<float>(fx) * spacing;
            const float oz = static_cast<float>(fz) * spacing;
            const float s = kFloaterSize;
            const float t[4][3] = {{ox, kFloaterHeight, oz},
                                   {ox + s, kFloaterHeight, oz},
                                   {ox + s * 0.5f, kFloaterHeight, oz + s},
                                   {ox + s * 0.5f, kFloaterHeight + s, oz + s * 0.5f}};
            const uint32 b = static_cast<uint32>(mesh.Vertices.size());
            for (const auto& p : t) {
                Vertex v{};
                v.Position[0] = p[0]; v.Position[1] = p[1]; v.Position[2] = p[2];
                v.Normal[1] = 1.0f;
                mesh.Vertices.push_back(v);
            }
            mesh.Indices.insert(mesh.Indices.end(),
                                {b, b + 2u, b + 1u, b, b + 1u, b + 3u,
                                 b + 1u, b + 2u, b + 3u, b, b + 3u, b + 2u});
        }
    }
    if (outFloaterBase)
        *outFloaterBase = floaterBase;
    if (outBodyIndexCount)
        *outBodyIndexCount = bodyIndexCount;
    return mesh;
}

} // namespace

// A coarse level's budget is a fraction of the ORIGINAL index count, but its
// simplify INPUT is the pruned source. On floater-dominated content the pruned
// source is smaller than the budget, so the generator must ask for no more than
// its own input supplies — meshopt asserts target_index_count <= index_count and
// aborts the cook otherwise (observed on SM_Prop_Shelf_01 and
// sm_generic_cloudring_01 during a cold import). Such a level has already MET
// its budget: pruning alone got it under, so the pruned source ships as-is.
TEST(MeshLODGenerator, PrunedSourceBelowLevelBudgetStillCooks) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    uint32 floaterBase = 0;
    size_t bodyIndexCount = 0;
    Mesh mesh = MakeSparseBodyWithFloaterSwarm(18, &floaterBase, &bodyIndexCount);
    const size_t srcIndexCount = mesh.Indices.size();
    ASSERT_GT(srcIndexCount, bodyIndexCount * 4)
        << "floaters must dominate the index count for the budget to overshoot";

    MeshLODConfig config;
    config.TargetRatios[1] = 0.25f; // at the prune ceiling: LOD1 takes the pruned source
    config.AllowSloppy = false;     // keep the assertion on a quality level
    const MeshLODs lods = GenerateMeshLODs(mesh, config);

    ASSERT_EQ(lods.LodCount(), 2u)
        << "LOD1 emits the pruned body; the coarser levels' budgets are already "
           "met by it, so they skip";
    ASSERT_EQ(lods.LodSloppy[1], 0u);
    const size_t budget = ((static_cast<size_t>(static_cast<float>(srcIndexCount) * 0.25f)) / 3) * 3;
    EXPECT_LE(lods.LodIndices[1].size(), budget);
    EXPECT_EQ(lods.LodIndices[1].size(), bodyIndexCount)
        << "the level is the pruned source: every floater shed, body untouched";
    EXPECT_FALSE(AnyIndexAtOrAbove(lods.LodIndices[1], floaterBase))
        << "coarse level (pruned source) must shed every micro floater";
}

// Same clamp, on the ratio band where the sloppy fallback is ELIGIBLE — which
// is where production content (cloud rings, shelf clutter) actually reaches its
// coarsest level. The fallback triggers on newCount > 1.5x the budget; once the
// budget is clamped to the level's own source the simplifier returns that
// source unchanged, so newCount EQUALS the budget and the fallback must stay
// out. Pins that: a clamped coarse level ships the pruned source through the
// quality path, not an attribute-rebuilt sloppy shell with its own vertices.
TEST(MeshLODGenerator, PrunedSourceBelowLevelBudgetKeepsSloppyOut) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    uint32 floaterBase = 0;
    size_t bodyIndexCount = 0;
    Mesh mesh = MakeSparseBodyWithFloaterSwarm(18, &floaterBase, &bodyIndexCount);
    const size_t srcIndexCount = mesh.Indices.size();

    MeshLODConfig config;
    config.AllowSloppy = true;      // the fallback is armed for LOD3
    config.TargetRatios[2] = 0.30f; // above the prune ceiling: keeps LOD3 reachable
    config.TargetRatios[3] = 0.06f; // under SloppyRatioThreshold AND the prune ceiling
    ASSERT_LE(config.TargetRatios[3], config.SloppyRatioThreshold);
    ASSERT_GT(static_cast<size_t>(static_cast<float>(srcIndexCount) * config.TargetRatios[3]),
              bodyIndexCount)
        << "the coarsest budget must overshoot the pruned source for the clamp to bind";

    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    const uint32 last = lods.LodCount() - 1u;
    ASSERT_EQ(last, 3u)
        << "the level under test must be the ratio-0.06 one — the only sloppy-eligible "
           "level in this config; a shorter chain would satisfy every check below vacuously";
    EXPECT_EQ(lods.LodIndices[last].size(), bodyIndexCount)
        << "the coarsest level must be the clamped pruned source";
    EXPECT_EQ(lods.LodSloppy[last], 0u)
        << "a budget-met level must not fall back to sloppy";
    EXPECT_TRUE(lods.LodVertices[last].empty())
        << "a non-sloppy level shares the source vertex block";
    EXPECT_FALSE(AnyIndexAtOrAbove(lods.LodIndices[last], floaterBase))
        << "coarse level (pruned source) must shed every micro floater";
}

namespace {

// Flat-shaded (faceted) bumpy grid: every quad carries its own four wedges
// with a per-face normal, so each interior position hosts 4+ attribute wedges
// — the corner class non-permissive meshopt tags complex and locks outright.
Mesh MakeFacetedGrid(uint32 n) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto height = [](float x, float z) {
        return 0.8f * std::sin(x * 0.37f) * std::sin(z * 0.41f);
    };
    for (uint32 z = 0; z + 1 < n; ++z) {
        for (uint32 x = 0; x + 1 < n; ++x) {
            const float xs[4] = {(float)x, (float)(x + 1), (float)x, (float)(x + 1)};
            const float zs[4] = {(float)z, (float)z, (float)(z + 1), (float)(z + 1)};
            const uint32 base = static_cast<uint32>(mesh.Vertices.size());
            // Face normal from the quad's first triangle.
            float p[4][3];
            for (int c = 0; c < 4; ++c) {
                p[c][0] = xs[c];
                p[c][1] = height(xs[c], zs[c]);
                p[c][2] = zs[c];
            }
            const float e1[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const float e2[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            float nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
            const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            for (float& f : nrm) f /= (len > 0 ? len : 1.0f);
            for (int c = 0; c < 4; ++c) {
                Vertex v{};
                v.Position[0] = p[c][0]; v.Position[1] = p[c][1]; v.Position[2] = p[c][2];
                v.Normal[0] = nrm[0]; v.Normal[1] = nrm[1]; v.Normal[2] = nrm[2];
                v.TexCoords[0] = xs[c] / (n - 1);
                v.TexCoords[1] = zs[c] / (n - 1);
                mesh.Vertices.push_back(v);
            }
            mesh.Indices.insert(mesh.Indices.end(),
                                {base, base + 2u, base + 1u, base + 1u, base + 2u, base + 3u});
        }
    }
    return mesh;
}

} // namespace

// Pins meshopt_SimplifyPermissive on the quality path: faceted content makes
// every interior position a 3+-wedge attribute corner, which non-permissive
// meshopt classifies complex and locks outright — the whole mesh then refuses
// to simplify (gate corpus v5: zero quality levels on any faceted mesh).
// Permissive is documented Experimental in meshopt 1.1, so a silent vcpkg
// regression would revert faceted content to LOD0+shell; this fixture makes
// that loud.
TEST(MeshLODGenerator, FacetedContentEmitsQualityLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeFacetedGrid(24);
    MeshLODConfig config;
    config.AllowSloppy = false; // a sloppy shell must not stand in for the pin
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u) << "faceted mesh emitted no quality level";
    EXPECT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_LT(lods.TriangleCount(1), lods.TriangleCount(0));
}

// Pins wedge preservation: the emitted levels must keep referencing distinct
// co-located wedges (split attributes at one position). A regression to the
// old representative-index pre-weld collapses every co-located pair onto one
// index — attributes flatten — while the whole suite otherwise stays green.
TEST(MeshLODGenerator, EmittedLevelsPreserveSplitAttributeWedges) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeSeamedCylinder(24, 8); // seam column: same position, U=0 vs U=1
    const MeshLODs lods = GenerateMeshLODs(mesh, {});
    ASSERT_GT(lods.LodCount(), 1u);
    ASSERT_EQ(lods.LodSloppy[1], 0u);

    // Group the level's referenced vertices by exact position; look for a
    // position whose referenced wedges disagree in U by ~1 (the wrap seam).
    std::map<std::array<int64_t, 3>, std::pair<float, float>> uRangeAtPosition;
    for (uint32 index : lods.LodIndices[1]) {
        const Vertex& v = mesh.Vertices[index];
        // Seam twins differ by kSeamEpsilon in Z; quantize coarsely enough to
        // group them while keeping distinct ring vertices apart.
        const std::array<int64_t, 3> key = {
            static_cast<int64_t>(std::llround(v.Position[0] * 1e3)),
            static_cast<int64_t>(std::llround(v.Position[1] * 1e3)),
            static_cast<int64_t>(std::llround(v.Position[2] * 1e3))};
        auto [it, inserted] =
            uRangeAtPosition.emplace(key, std::make_pair(v.TexCoords[0], v.TexCoords[0]));
        if (!inserted) {
            it->second.first = std::min(it->second.first, v.TexCoords[0]);
            it->second.second = std::max(it->second.second, v.TexCoords[0]);
        }
    }
    bool anySplitWedgeSurvives = false;
    for (const auto& [key, range] : uRangeAtPosition)
        if (range.second - range.first > 0.9f)
            anySplitWedgeSurvives = true;
    EXPECT_TRUE(anySplitWedgeSurvives)
        << "no surviving position references both wedges of the UV wrap seam — "
           "split attributes were flattened";
}

// Pins the per-axis seam-plane epsilon: a thin frame (large XY, hair-thin Z)
// under the old largest-extent epsilon put EVERY vertex inside the Z-face band
// (band wider than the whole Z extent), so all open borders locked and no
// quality level could emit — silently reproducing the v5 no-mids failure per
// thin piece. Per-axis, the inner rim (off the XY faces) stays free and the
// frame reduces.
TEST(MeshLODGenerator, PerAxisSeamEpsilonFreesThinFrameInnerRim) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    constexpr uint32 kOuter = 48, kHole = 16; // frame 16 quads wide
    constexpr float kTilt = 0.0008f;          // Z extent ~0.038 of an XY extent 47
    std::map<std::pair<uint32, uint32>, uint32> vertexAt;
    const auto vertex = [&](uint32 x, uint32 z) {
        const auto key = std::make_pair(x, z);
        auto it = vertexAt.find(key);
        if (it != vertexAt.end())
            return it->second;
        Vertex v{};
        v.Position[0] = static_cast<float>(x);
        v.Position[1] = static_cast<float>(z);
        v.Position[2] = static_cast<float>(x) * kTilt;
        v.Normal[2] = 1.0f;
        v.TexCoords[0] = static_cast<float>(x) / (kOuter - 1);
        v.TexCoords[1] = static_cast<float>(z) / (kOuter - 1);
        const uint32 id = static_cast<uint32>(mesh.Vertices.size());
        mesh.Vertices.push_back(v);
        vertexAt.emplace(key, id);
        return id;
    };
    const uint32 holeLo = (kOuter - kHole) / 2, holeHi = holeLo + kHole;
    for (uint32 z = 0; z + 1 < kOuter; ++z) {
        for (uint32 x = 0; x + 1 < kOuter; ++x) {
            if (x >= holeLo && x < holeHi && z >= holeLo && z < holeHi)
                continue; // the hole
            const uint32 i0 = vertex(x, z), i1 = vertex(x + 1, z);
            const uint32 i2 = vertex(x, z + 1), i3 = vertex(x + 1, z + 1);
            mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    MeshLODConfig config; // default SeamPlanes
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u)
        << "thin frame emitted no quality level — inner rim over-locked";
    EXPECT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_LT(lods.TriangleCount(1), lods.TriangleCount(0));
}

namespace {

// Plane strip whose two halves map to DISTANT atlas regions, with the seam
// column duplicated per side (co-located wedges, UV spread ~1) — the chromatic
// chart boundary class (emblem on cloth, awning stripes).
Mesh MakeTwoChartStrip(uint32 n) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const uint32 mid = n / 2;
    const auto addHalf = [&](uint32 x0, uint32 x1, float u0, float u1) {
        const uint32 base = static_cast<uint32>(mesh.Vertices.size());
        const uint32 cols = x1 - x0 + 1;
        for (uint32 z = 0; z < n; ++z) {
            for (uint32 x = x0; x <= x1; ++x) {
                Vertex v{};
                v.Position[0] = static_cast<float>(x);
                v.Position[1] = 0.15f * std::sin(x * 0.9f) * std::sin(z * 0.8f);
                v.Position[2] = static_cast<float>(z);
                v.Normal[1] = 1.0f;
                v.TexCoords[0] = u0 + (u1 - u0) * static_cast<float>(x - x0) / (x1 - x0);
                v.TexCoords[1] = static_cast<float>(z) / (n - 1);
                mesh.Vertices.push_back(v);
            }
        }
        for (uint32 z = 0; z + 1 < n; ++z) {
            for (uint32 x = 0; x + 1 < cols; ++x) {
                const uint32 i0 = base + z * cols + x;
                const uint32 i1 = i0 + 1;
                const uint32 i2 = base + (z + 1) * cols + x;
                const uint32 i3 = i2 + 1;
                mesh.Indices.insert(mesh.Indices.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
    };
    addHalf(0, mid, 0.02f, 0.10f);      // chart A: low-left of the atlas
    addHalf(mid, n - 1, 0.90f, 0.98f);  // chart B: high-right (duplicated seam column)
    return mesh;
}

} // namespace

// Chromatic chart boundaries must survive simplification: the co-located seam
// wedges span distant atlas regions, so collapses across them would remap
// surface to the wrong palette cell (the moon-emblem/awning-stripe class).
// The Protect flags keep every emitted level free of cross-chart triangles
// while both charts still simplify internally.
TEST(MeshLODGenerator, ChromaticChartBoundarySurvivesQualityLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeTwoChartStrip(24);
    MeshLODConfig config;
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u);
    const auto chartOf = [&](uint32 index) {
        return mesh.Vertices[index].TexCoords[0] < 0.5f ? 0 : 1;
    };
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        ASSERT_EQ(lods.LodSloppy[lod], 0u);
        EXPECT_LT(lods.TriangleCount(lod), lods.TriangleCount(0));
        const Vector<uint32>& idx = lods.LodIndices[lod];
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const int c0 = chartOf(idx[i]), c1 = chartOf(idx[i + 1]), c2 = chartOf(idx[i + 2]);
            ASSERT_TRUE(c0 == c1 && c1 == c2)
                << "LOD" << lod << " emitted a cross-chart (amalgamated) triangle";
        }
    }
}

// --- Generator v8: silhouette honesty + wedge-normal re-point ---------------

namespace {

// Two coplanar unit cards side by side in the XY plane, disjoint components.
Mesh MakeTwoCards() {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto addCard = [&mesh](float x0) {
        const uint32 base = static_cast<uint32>(mesh.Vertices.size());
        const float corners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        for (const auto& c : corners) {
            Vertex v{};
            v.Position[0] = x0 + c[0];
            v.Position[1] = c[1];
            v.Position[2] = 0.0f;
            v.Normal[2] = 1.0f;
            v.TexCoords[0] = c[0];
            v.TexCoords[1] = c[1];
            mesh.Vertices.push_back(v);
        }
        mesh.Indices.insert(mesh.Indices.end(),
                            {base, base + 2u, base + 1u, base + 1u, base + 2u, base + 3u});
    };
    addCard(0.0f);
    addCard(1.1f);
    return mesh;
}

// A grid plane doubled into a two-sided sheet: back-face copy at identical
// positions with inverted normals and reversed winding (the foliage-card /
// cloth authoring pattern).
Mesh MakeTwoSidedPlane(uint32 n) {
    Mesh mesh = MakeGridPlane(n);
    const uint32 frontVerts = static_cast<uint32>(mesh.Vertices.size());
    const size_t frontIndices = mesh.Indices.size();
    for (uint32 v = 0; v < frontVerts; ++v) {
        Vertex back = mesh.Vertices[v];
        back.Normal[1] = -1.0f;
        mesh.Vertices.push_back(back);
    }
    for (size_t i = 0; i + 2 < frontIndices; i += 3) {
        mesh.Indices.push_back(frontVerts + mesh.Indices[i]);
        mesh.Indices.push_back(frontVerts + mesh.Indices[i + 2]);
        mesh.Indices.push_back(frontVerts + mesh.Indices[i + 1]);
    }
    return mesh;
}

// Area fraction of a level whose worst corner normal sits more than
// `maxDegrees` off the triangle's own geometric normal — the mixed-facet
// shading damage a collapse leaves behind on flat-shaded content, and what the
// wedge re-point pass exists to repair. Signed, so a backwards-facing corner
// counts as fully wrong.
float ShadingNormalDamageArea(const Mesh& mesh, const Vector<uint32>& indices,
                              float maxDegrees) {
    const float minMatch = std::cos(maxDegrees * 3.14159265f / 180.0f);
    double total = 0.0;
    double damaged = 0.0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const float* p0 = mesh.Vertices[indices[i]].Position;
        const float* p1 = mesh.Vertices[indices[i + 1]].Position;
        const float* p2 = mesh.Vertices[indices[i + 2]].Position;
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 0.0f)
            continue;
        for (float& f : n)
            f /= len;
        const double area = 0.5 * static_cast<double>(len);
        float worst = 1.0f;
        for (int c = 0; c < 3; ++c) {
            const Vertex& v = mesh.Vertices[indices[i + static_cast<size_t>(c)]];
            worst = std::min(worst,
                             v.Normal[0] * n[0] + v.Normal[1] * n[1] + v.Normal[2] * n[2]);
        }
        total += area;
        if (worst < minMatch)
            damaged += area;
    }
    return total > 0.0 ? static_cast<float>(damaged / total) : 0.0f;
}

double SurfaceArea(const Mesh& mesh, const Vector<uint32>& indices) {
    double area = 0.0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const float* p0 = mesh.Vertices[indices[i]].Position;
        const float* p1 = mesh.Vertices[indices[i + 1]].Position;
        const float* p2 = mesh.Vertices[indices[i + 2]].Position;
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]};
        area += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    return area;
}

} // namespace

// The metric itself: a level that draws the same outline measures ~0; a level
// that dropped one of two cards loses ~half the face-view coverage.
TEST(SilhouetteHonesty, LossMeasuresVanishedSheetAndCleanLevelMeasuresZero) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    const Mesh mesh = MakeTwoCards();
    EXPECT_FLOAT_EQ(ComputeSilhouetteLossForTest(mesh, mesh.Indices), 0.0f);
    const Vector<uint32> oneCard(mesh.Indices.begin(), mesh.Indices.begin() + 6);
    const float loss = ComputeSilhouetteLossForTest(mesh, oneCard);
    EXPECT_GT(loss, 0.3f);
    EXPECT_LT(loss, 0.7f);
}

// The fold contract (her defect-1/2 class): in-plane shrinkage of an open
// sheet costs the position quadric ~nothing, so the achieved error must carry
// the silhouette mismatch instead — every emitted quality level satisfies
// err >= kSilhouetteErrorScale * loss. Two disjoint cards at a 25% target
// cannot keep their combined outline (each card is already two triangles), so
// the fold is exercised, not vacuously true.
TEST(SilhouetteHonesty, InPlaneSheetShrinkFoldsIntoAchievedError) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeTwoCards();
    MeshLODConfig config;
    config.BorderRule = MeshLODBorderRule::Free;
    config.AllowSloppy = false;
    config.LodCount = 2;
    config.TargetRatios[1] = 0.25f;
    config.TargetError[1] = 0.9f;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_EQ(lods.LodCount(), 2u);
    ASSERT_EQ(lods.LodSloppy[1], 0u);
    const float loss = ComputeSilhouetteLossForTest(mesh, lods.LodIndices[1]);
    EXPECT_GT(loss, 0.1f) << "25% target on two cards did not erode the outline "
                             "— the fold is not being exercised";
    EXPECT_GE(lods.LodErrors[1] + 1e-4f, 2.0f * loss)
        << "silhouette mismatch did not fold into the achieved error";
}

// False-positive pin for the metric: honest structural coarsening keeps the
// outline, so its silhouette loss must stay near zero — if the rasterizer or
// mask compare regresses, every level's error inflates and all engagement
// silently dies. A 32-grid plane at a 25% target coarsens interior geometry
// only; its outline is the same square.
TEST(SilhouetteHonesty, HonestCoarseningMeasuresNearZero) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeGridPlane(32);
    MeshLODConfig config;
    config.BorderRule = MeshLODBorderRule::Free;
    config.AllowSloppy = false;
    config.LodCount = 2;
    config.TargetRatios[1] = 0.25f;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_EQ(lods.LodCount(), 2u);
    ASSERT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_LT(ComputeSilhouetteLossForTest(mesh, lods.LodIndices[1]), 0.05f);
}

// Two-sided card content (foliage presets, cloth) must keep REAL quality mid
// levels. Iteration 3 first tried Protecting co-located wedge groups with
// opposed normals; on a two-sided sheet EVERY group is such a pair, so the
// whole surface locked, the quality path could not reach any ratio target, and
// slot 1 fell through to the 2-6 triangle sloppy shell — measured on the gate
// corpus's vine presets, which is exactly the "the vines are missing" defect it
// was meant to cure. This pins the outcome that matters: the near slot carries
// real geometry, not a shell.
TEST(WedgeNormalRepoint, TwoSidedSheetKeepsRealQualityMidLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeTwoSidedPlane(9);
    MeshLODConfig config;
    config.BorderRule = MeshLODBorderRule::Free;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u) << "two-sided plane emitted no level at all";
    EXPECT_EQ(lods.LodSloppy[1], 0u)
        << "slot 1 of a two-sided sheet is a sloppy shell — the quality path is "
           "over-constrained on card content";
    EXPECT_LT(lods.LodIndices[1].size(), lods.LodIndices[0].size());
    // ...and it still draws the sheet: the shell it replaced retained a fifth of
    // the card area, which is what "the vines are missing" looks like.
    EXPECT_GT(SurfaceArea(mesh, lods.LodIndices[1]),
              0.8 * SurfaceArea(mesh, lods.LodIndices[0]))
        << "slot 1 lost most of the sheet's surface area";
}

// Re-point contract on flat-shaded content: a collapse leaves corners holding
// other facets' normals, which breaks highlights on large flat faces. After
// re-pointing, almost no emitted area may sit more than 25 degrees off its own
// triangle plane. Measured on this fixture without the pass: 20-30% of area
// damaged; the fixture's own LOD0 is 0%.
TEST(WedgeNormalRepoint, FacetedLevelCornersMatchTheirOwnTrianglePlane) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeFacetedGrid(24);
    MeshLODConfig config;
    config.BorderRule = MeshLODBorderRule::Free;
    config.AllowSloppy = false;
    config.LodCount = 2;
    config.TargetRatios[1] = 0.5f;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_EQ(lods.LodCount(), 2u);
    ASSERT_EQ(lods.LodSloppy[1], 0u);
    EXPECT_FLOAT_EQ(ShadingNormalDamageArea(mesh, lods.LodIndices[0], 25.0f), 0.0f);
    EXPECT_LT(ShadingNormalDamageArea(mesh, lods.LodIndices[1], 25.0f), 0.05f)
        << "emitted corners do not match their own triangle plane — the wedge "
           "re-point pass regressed";
}

// The front/back mixing case the re-point pass subsumes: on a two-sided sheet a
// corner that kept the opposite sheet's normal faces backwards, which shades as
// a black facet. Signed match must stay positive on every emitted corner.
TEST(WedgeNormalRepoint, TwoSidedSheetCornersNeverFaceBackwards) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeTwoSidedPlane(9);
    MeshLODConfig config;
    config.BorderRule = MeshLODBorderRule::Free;
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u);
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        if (lods.LodSloppy[lod] != 0u)
            continue; // shells carry their own rebuilt flat normals
        EXPECT_FLOAT_EQ(ShadingNormalDamageArea(mesh, lods.LodIndices[lod], 90.0f), 0.0f)
            << "LOD" << lod << " emitted a corner facing away from its own triangle";
    }
}

// Chart protection must survive the re-point pass: a corner may only adopt a
// co-located wedge from its OWN chart, so no emitted triangle may span a
// chromatic seam LOD0 kept separate. The seamed cylinder's seam column carries
// u=0 and u=1 twins at identical positions.
TEST(WedgeNormalRepoint, RepointNeverCrossesAChromaticChartSeam) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeSeamedCylinder(24, 12);
    MeshLODConfig config;
    config.AllowSloppy = false;
    const MeshLODs lods = GenerateMeshLODs(mesh, config);
    ASSERT_GT(lods.LodCount(), 1u);
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        if (lods.LodSloppy[lod] != 0u)
            continue;
        const Vector<uint32>& idx = lods.LodIndices[lod];
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            float minU = 1e9f, maxU = -1e9f;
            for (int c = 0; c < 3; ++c) {
                const float u = mesh.Vertices[idx[i + static_cast<size_t>(c)]].TexCoords[0];
                minU = std::min(minU, u);
                maxU = std::max(maxU, u);
            }
            ASSERT_LT(maxU - minU, 0.9f)
                << "LOD" << lod << " triangle spans the full u range — a re-pointed "
                   "corner jumped the chart seam";
        }
    }
}

namespace {

// One +Y-facing triangle (face normal = cross(p1-p0, p2-p0) = +Y) whose corner
// 0 wedge carries a BACKWARDS normal — the exact corner the re-point pass
// exists to repair. Extra co-located wedges at p0 are appended by the tests.
Mesh MakeRepointTriangle() {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto add = [&mesh](float px, float py, float pz, float nx, float ny, float nz) {
        Vertex v;
        v.Position[0] = px; v.Position[1] = py; v.Position[2] = pz;
        v.Normal[0] = nx; v.Normal[1] = ny; v.Normal[2] = nz;
        mesh.Vertices.push_back(v);
        return static_cast<uint32>(mesh.Vertices.size() - 1);
    };
    const uint32 a = add(0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f); // backwards corner
    const uint32 b = add(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    const uint32 c = add(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    mesh.Indices = {a, b, c};
    return mesh;
}

uint32 AddColocatedWedge(Mesh& mesh, float nx, float ny, float nz,
                         float tx = 0.0f, float ty = 0.0f, float tz = 0.0f,
                         float tw = 1.0f) {
    Vertex v = mesh.Vertices[0]; // same position + UV as corner 0
    v.Normal[0] = nx; v.Normal[1] = ny; v.Normal[2] = nz;
    v.Tangent[0] = tx; v.Tangent[1] = ty; v.Tangent[2] = tz; v.Tangent[3] = tw;
    mesh.Vertices.push_back(v);
    return static_cast<uint32>(mesh.Vertices.size() - 1);
}

} // namespace

// A zero-length shading normal has no direction to match. The raw dot scored
// it 0.0 — beating any backwards corner, i.e. preferred exactly where the
// re-point repairs — so the match must be a true cosine with zero-length
// excluded: with only a zero-length twin on offer, the backwards corner stays.
TEST(WedgeNormalRepoint, ZeroLengthNormalWedgeIsNeverSelected) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeRepointTriangle();
    AddColocatedWedge(mesh, 0.0f, 0.0f, 0.0f); // zero-length normal twin
    Vector<uint32> indices = mesh.Indices;
    RepointCornersForTest(mesh, indices);
    EXPECT_EQ(indices, mesh.Indices)
        << "a zero-length-normal wedge was adopted over the current corner";
}

// The match must compare direction, not magnitude: a long poorly-aligned
// normal must lose to a short well-aligned one.
TEST(WedgeNormalRepoint, MatchIsCosineNotRawDot) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh = MakeRepointTriangle();
    const uint32 longOff = AddColocatedWedge(mesh, 2.0f, 2.0f, 0.0f);  // raw dot 2.0, cos ~0.71
    const uint32 shortOn = AddColocatedWedge(mesh, 0.0f, 0.5f, 0.0f);  // raw dot 0.5, cos 1.0
    (void)longOff;
    Vector<uint32> indices = mesh.Indices;
    RepointCornersForTest(mesh, indices);
    EXPECT_EQ(indices[0], shortOn)
        << "selection is length-biased — the raw dot picked the longer normal";
}

// Tangent[4] rides the wedge swap. A candidate with a mirrored handedness or a
// reversed tangent would break normal mapping outright, so neither may ever be
// adopted — while a same-hemisphere tangent (the legitimate facet repair) must
// still be.
TEST(WedgeNormalRepoint, MirroredOrReversedTangentWedgeIsNeverAdopted) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    {
        Mesh mesh = MakeRepointTriangle();
        mesh.Vertices[0].Tangent[0] = 1.0f; // authored frame on the corner
        AddColocatedWedge(mesh, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f); // mirrored w
        AddColocatedWedge(mesh, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f); // reversed xyz
        Vector<uint32> indices = mesh.Indices;
        RepointCornersForTest(mesh, indices);
        EXPECT_EQ(indices, mesh.Indices)
            << "a tangent-breaking wedge (mirrored handedness or reversed tangent) "
               "was adopted";
    }
    {
        Mesh mesh = MakeRepointTriangle();
        mesh.Vertices[0].Tangent[0] = 1.0f;
        const uint32 ok =
            AddColocatedWedge(mesh, 0.0f, 1.0f, 0.0f, 0.7f, 0.0f, 0.7f, 1.0f);
        Vector<uint32> indices = mesh.Indices;
        RepointCornersForTest(mesh, indices);
        EXPECT_EQ(indices[0], ok)
            << "the tangent gate blocked a same-hemisphere facet repair";
    }
}

// Two corners of one triangle CAN share a position group (a within-weld-epsilon
// sliver: the groups are keyed on snapped positions). Both corners would pick
// the same best-matching wedge, fusing the triangle into an index-degenerate
// one that silently drops from the level. The guard must keep the corners
// distinct while still letting one of them take the repair.
TEST(WedgeNormalRepoint, RepointNeverFusesTwoCornersOfATriangle) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto add = [&mesh](float px, float py, float pz, float nz) {
        Vertex v;
        v.Position[0] = px; v.Position[1] = py; v.Position[2] = pz;
        v.Normal[2] = nz;
        mesh.Vertices.push_back(v);
        return static_cast<uint32>(mesh.Vertices.size() - 1);
    };
    // Extent 1 => weld epsilon 1e-5: corners a and b (2e-6 apart) share a
    // snapped position group, as does the perfect-normal wedge w between them.
    // Face normal is +Z; a and b carry backwards (-Z) normals.
    const uint32 a = add(0.0f, 0.0f, 0.0f, -1.0f);
    const uint32 b = add(2e-6f, 0.0f, 0.0f, -1.0f);
    const uint32 c = add(0.5f, 1.0f, 0.0f, 1.0f);
    const uint32 w = add(1e-6f, 0.0f, 0.0f, 1.0f);
    mesh.Indices = {a, b, c};
    Vector<uint32> indices = mesh.Indices;
    RepointCornersForTest(mesh, indices);
    EXPECT_TRUE(indices[0] != indices[1] && indices[1] != indices[2] &&
                indices[0] != indices[2])
        << "re-point fused two within-weld-epsilon corners into one wedge — "
           "the triangle degenerated";
    EXPECT_TRUE(indices[0] == w || indices[1] == w)
        << "the degeneracy guard blocked the repair for BOTH corners";
}

// --- Generator v4: attribute-honest sloppy shells --------------------------

namespace {

// Two planar quads with disjoint UV charts: island A (x in [0,1], UVs in
// [0,0.2]^2) and island B (x in [xB, xB+1], UVs in [0.8,1.0]^2). Distinct
// vertex indices per quad => two UV islands under vertex-graph connectivity.
Mesh MakeTwoIslandQuads(float xB) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto addQuad = [&mesh](float x0, float u0, float u1) {
        const uint32 base = static_cast<uint32>(mesh.Vertices.size());
        const float corners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}}; // (dx, z)
        for (const auto& c : corners) {
            Vertex v;
            v.Position[0] = x0 + c[0];
            v.Position[1] = 0.0f;
            v.Position[2] = c[1];
            v.Normal[1] = 1.0f;
            v.TexCoords[0] = u0 + c[0] * (u1 - u0);
            v.TexCoords[1] = u0 + c[1] * (u1 - u0);
            mesh.Vertices.push_back(v);
        }
        mesh.Indices.insert(mesh.Indices.end(),
                            {base, base + 2u, base + 1u, base + 1u, base + 2u, base + 3u});
    };
    addQuad(0.0f, 0.0f, 0.2f);  // island A
    addQuad(xB, 0.8f, 1.0f);    // island B
    return mesh;
}

bool UvInBox(const Vertex& v, float lo, float hi) {
    return v.TexCoords[0] >= lo && v.TexCoords[0] <= hi &&
           v.TexCoords[1] >= lo && v.TexCoords[1] <= hi;
}

} // namespace

// Island snapping: a triangle whose corners span two UV islands must come out
// with ALL THREE corner UVs inside the dominant island's chart — interpolation
// can then never sweep the atlas between islands. Same-island triangles keep
// their exact source UVs.
TEST(SloppyShell, IslandSnapKeepsTriangleUvsInOneIsland) {
    // Islands share the x==1 seam: B0 position-duplicates A1 (the classic
    // position-twin UV seam), so the cross corner has a zero-distance donor.
    const Mesh mesh = MakeTwoIslandQuads(/*xB=*/1.0f);
    // Cross-island triangle {A0, A2, B0} plus a pure island-A triangle.
    const Vector<uint32> sloppy = {0u, 2u, 4u, 0u, 2u, 1u};

    Vector<uint32> localIndices;
    Vector<Vertex> verts;
    ASSERT_TRUE(BuildSloppyShellForTest(mesh, sloppy, localIndices, verts));
    ASSERT_EQ(verts.size(), 6u);

    // Cross triangle: dominant island is A (majority) — B0's corner UV must be
    // snapped to its position-twin donor A1 inside A's chart; A corners keep
    // their own UVs.
    EXPECT_FLOAT_EQ(verts[0].TexCoords[0], mesh.Vertices[0].TexCoords[0]);
    EXPECT_FLOAT_EQ(verts[1].TexCoords[1], mesh.Vertices[2].TexCoords[1]);
    EXPECT_FLOAT_EQ(verts[2].TexCoords[0], mesh.Vertices[1].TexCoords[0]);
    EXPECT_FLOAT_EQ(verts[2].TexCoords[1], mesh.Vertices[1].TexCoords[1]);
    for (int c = 0; c < 3; ++c)
        EXPECT_TRUE(UvInBox(verts[c], 0.0f, 0.2f))
            << "corner " << c << " UV escaped the dominant island's chart";

    // Pure-A triangle: UVs bit-exact.
    EXPECT_FLOAT_EQ(verts[3].TexCoords[0], mesh.Vertices[0].TexCoords[0]);
    EXPECT_FLOAT_EQ(verts[3].TexCoords[1], mesh.Vertices[0].TexCoords[1]);
    EXPECT_FLOAT_EQ(verts[4].TexCoords[0], mesh.Vertices[2].TexCoords[0]);
    EXPECT_FLOAT_EQ(verts[5].TexCoords[0], mesh.Vertices[1].TexCoords[0]);
}

// The shell is per-face: 3 unique vertices per triangle, LOD-local sequential
// indices, source positions preserved, and one unit-length flat normal per face
// oriented by the engine winding (cross(p1-p0, p2-p0)).
TEST(SloppyShell, PerFaceFlatShadedBlockPreservesPositions) {
    const Mesh mesh = MakeTwoIslandQuads(1.0f);
    const Vector<uint32> sloppy = {0u, 2u, 1u, 4u, 6u, 5u};

    Vector<uint32> localIndices;
    Vector<Vertex> verts;
    ASSERT_TRUE(BuildSloppyShellForTest(mesh, sloppy, localIndices, verts));
    ASSERT_EQ(localIndices.size(), sloppy.size());
    ASSERT_EQ(verts.size(), sloppy.size()); // per-face: one vertex per index

    for (size_t i = 0; i < localIndices.size(); ++i)
        EXPECT_EQ(localIndices[i], static_cast<uint32>(i)) << "LOD-local iota expected";

    for (size_t t = 0; t < sloppy.size() / 3; ++t) {
        const Vertex* tri[3] = {&verts[t * 3], &verts[t * 3 + 1], &verts[t * 3 + 2]};
        for (int c = 0; c < 3; ++c) {
            const Vertex& src = mesh.Vertices[sloppy[t * 3 + static_cast<size_t>(c)]];
            EXPECT_FLOAT_EQ(tri[c]->Position[0], src.Position[0]);
            EXPECT_FLOAT_EQ(tri[c]->Position[1], src.Position[1]);
            EXPECT_FLOAT_EQ(tri[c]->Position[2], src.Position[2]);
        }
        // Flat: all three corners share one normal; both fixture quads face +Y.
        const float len = std::sqrt(tri[0]->Normal[0] * tri[0]->Normal[0] +
                                    tri[0]->Normal[1] * tri[0]->Normal[1] +
                                    tri[0]->Normal[2] * tri[0]->Normal[2]);
        EXPECT_NEAR(len, 1.0f, 1e-5f);
        EXPECT_NEAR(tri[0]->Normal[1], 1.0f, 1e-5f) << "winding-oriented face normal";
        for (int c = 1; c < 3; ++c) {
            EXPECT_FLOAT_EQ(tri[c]->Normal[0], tri[0]->Normal[0]);
            EXPECT_FLOAT_EQ(tri[c]->Normal[1], tri[0]->Normal[1]);
            EXPECT_FLOAT_EQ(tri[c]->Normal[2], tri[0]->Normal[2]);
        }
    }
}

// Dominant island is chosen by SOURCE AREA, not corner majority: sloppy
// collapse samples charts by vertex density, so a vertex-dense trim chart can
// out-vote the perceptually dominant material (gate corpus: banner shells
// painted white instead of cloth-blue). Two corners in a tiny chart must still
// snap into the big chart a single corner brings.
TEST(SloppyShell, AreaDominatesCornerMajority) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    const auto addVert = [&mesh](float x, float z, float u, float v) {
        Vertex vert;
        vert.Position[0] = x;
        vert.Position[2] = z;
        vert.Normal[1] = 1.0f;
        vert.TexCoords[0] = u;
        vert.TexCoords[1] = v;
        mesh.Vertices.push_back(vert);
    };
    // Island A: one tiny triangle (area 0.02), UVs near (0.05, 0.05).
    addVert(0.0f, 0.0f, 0.05f, 0.05f);
    addVert(0.2f, 0.0f, 0.08f, 0.05f);
    addVert(0.0f, 0.2f, 0.05f, 0.08f);
    // Island B: big quad (area 4), UVs in [0.5, 1.0].
    addVert(0.5f, 0.0f, 0.5f, 0.5f);
    addVert(2.5f, 0.0f, 1.0f, 0.5f);
    addVert(0.5f, 2.0f, 0.5f, 1.0f);
    addVert(2.5f, 2.0f, 1.0f, 1.0f);
    mesh.Indices = {0u, 2u, 1u, 3u, 5u, 4u, 4u, 5u, 6u};

    // Shell face: TWO island-A corners + one island-B corner.
    Vector<uint32> localIndices;
    Vector<Vertex> verts;
    ASSERT_TRUE(BuildSloppyShellForTest(mesh, {0u, 2u, 3u}, localIndices, verts));
    ASSERT_EQ(verts.size(), 3u);
    for (int c = 0; c < 3; ++c)
        EXPECT_TRUE(UvInBox(verts[static_cast<size_t>(c)], 0.5f, 1.0f))
            << "corner " << c << " must snap into the LARGER island's chart";
}

// Positionally-degenerate sloppy faces (all corners collapsed) are dropped from
// the shell instead of shipping zero-area triangles with NaN normals; a shell
// that is ALL degenerate is refused outright.
TEST(SloppyShell, DegenerateFacesDropped) {
    const Mesh mesh = MakeTwoIslandQuads(1.0f);
    Vector<uint32> localIndices;
    Vector<Vertex> verts;

    // One real face + one collapsed face (same corner thrice).
    ASSERT_TRUE(BuildSloppyShellForTest(mesh, {0u, 2u, 1u, 3u, 3u, 3u},
                                        localIndices, verts));
    EXPECT_EQ(verts.size(), 3u) << "collapsed face must not survive";

    EXPECT_FALSE(BuildSloppyShellForTest(mesh, {3u, 3u, 3u}, localIndices, verts))
        << "an all-degenerate shell has nothing honest to emit";
}

// End-to-end through the generator: the band shell's surviving sloppy far level
// now carries its own attribute-honest vertex block with LOD-local indices, and
// GenerateMeshLODsInto lands it in mesh.ExtraLODVertices parallel to ExtraLODs.
TEST(SloppyShell, GeneratedSloppyLevelCarriesOwnVertexBlock) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh shell = MakeSeamedCylinder(48, 2);
    const MeshLODs lods = GenerateMeshLODs(shell, {});
    ASSERT_GT(lods.LodCount(), 1u);
    const uint32 far = lods.LodCount() - 1u;
    ASSERT_EQ(lods.LodSloppy[far], 1u);
    ASSERT_EQ(lods.LodVertices.size(), lods.LodIndices.size());

    const auto& block = lods.LodVertices[far];
    const auto& idx = lods.LodIndices[far];
    ASSERT_FALSE(block.empty()) << "sloppy level must own its vertices";
    EXPECT_EQ(block.size(), idx.size()) << "per-face shell: one vertex per index";
    for (size_t i = 0; i < idx.size(); ++i)
        ASSERT_EQ(idx[i], static_cast<uint32>(i)) << "LOD-local iota expected";

    Mesh into = MakeSeamedCylinder(48, 2);
    MeshLODGenStats stats;
    GenerateMeshLODsInto(into, {}, &stats);
    ASSERT_EQ(into.ExtraLODVertices.size(), into.ExtraLODs.size());
    EXPECT_TRUE(into.HasOwnVertexLODs());
    EXPECT_FALSE(into.HasAuthoredLODs()) << "generated shells are NOT authored";
    // Own-vertex levels are LOD-local; index-only levels stay bounded by the
    // source vertex count.
    for (size_t j = 0; j < into.ExtraLODs.size(); ++j) {
        const uint32 bound = into.ExtraLODVertices[j].empty()
                                 ? static_cast<uint32>(into.Vertices.size())
                                 : static_cast<uint32>(into.ExtraLODVertices[j].size());
        EXPECT_LT(MaxIndex(into.ExtraLODs[j]), bound);
    }
}

// A mesh carrying optional parallel streams (vertex color here) cannot get an
// own-vertex shell yet (C3): the generator must SKIP the sloppy level rather
// than emit position-only corruption or a level the GPU would drop to LOD0-only.
TEST(SloppyShell, OptionalStreamMeshSkipsSloppyLevel) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh shell = MakeSeamedCylinder(48, 2);
    shell.Color0.assign(shell.Vertices.size() * 4, 1.0f);
    ASSERT_TRUE(shell.HasColor0());
    const MeshLODs lods = GenerateMeshLODs(shell, {});
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        EXPECT_EQ(lods.LodSloppy[lod], 0u) << "no sloppy level for optional-stream meshes";
        EXPECT_TRUE(lods.LodVertices[lod].empty());
    }
    EXPECT_GT(lods.SloppyLevelsSkippedForStreams, 0u)
        << "the forfeited far shell must be counted, not silently dropped";
}

// Regeneration discipline: a generated own-vertex chain must regenerate (the
// authored-wins rule keys on explicit provenance, not on block presence), and
// repeated generation keeps the parallel arrays canonical.
TEST(SloppyShell, RegenerationReplacesOwnVertexChain) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";
    Mesh shell = MakeSeamedCylinder(48, 2);
    GenerateMeshLODsInto(shell, {});
    ASSERT_TRUE(shell.HasOwnVertexLODs());
    const size_t levels = shell.ExtraLODs.size();

    GenerateMeshLODsInto(shell, {});
    EXPECT_EQ(shell.ExtraLODs.size(), levels) << "second cook reproduces the chain";
    EXPECT_EQ(shell.ExtraLODVertices.size(), shell.ExtraLODs.size());
    EXPECT_FALSE(shell.HasAuthoredLODs());
}

// The amalgamation tripwire measures cross-island interpolation: a hand-built
// level with a triangle spanning both islands of MakeTwoIslandQuads reports a
// positive area-weighted UV spread; a level confined to one island reports 0.
TEST(MeshLODGenerator, ChartAmalgamationErrorMeasuresCrossIslandArea) {
    Mesh mesh = MakeTwoIslandQuads(/*xB=*/1.0f);
    // One honest triangle (island A) + one amalgamated triangle whose third
    // corner comes from island B.
    const Vector<uint32> honest = {0u, 2u, 1u};
    const Vector<uint32> amalgamated = {0u, 2u, 4u};
    EXPECT_FLOAT_EQ(ComputeChartAmalgamationErrorForTest(mesh, honest), 0.0f);
    const float err = ComputeChartAmalgamationErrorForTest(mesh, amalgamated);
    if (IsMeshLODGenerationAvailable()) {
        // Corner UV spread is ~sqrt(0.8^2 + 0.8^2) over the full level area.
        EXPECT_GT(err, 0.5f);
        EXPECT_LT(err, 1.5f);
    } else {
        EXPECT_FLOAT_EQ(err, 0.0f);
    }
}

// --- Error -> SSE switch-point mapping (Rendering::DeriveLODThresholds) ---

namespace {
constexpr float kDefaults[4] = {0.5f, 0.2f, 0.08f, 0.0f};
// Unit test mesh: radius 1, maxExtent 1, so an SSE threshold is exactly 1/e
// and the switch sphere diameter is 2 * budgetPx / e. Real meshes carry a
// shape factor maxExtent/r in [2/sqrt(3), 2]; it cancels out of every
// assertion here, which is the point — the mapping is shape-linear.
constexpr float kUnitRadius = 1.0f;
constexpr float kUnitExtent = 1.0f;

uint32 Derive(const float* err, const uint8_t* slop, uint32 lodCount, bool authored,
              float* out, float radius = kUnitRadius, float extent = kUnitExtent) {
    return Rendering::DeriveLODThresholds(err, slop, lodCount, authored, radius, extent,
                                          kDefaults, 4u, out);
}

// Mirror of draw_command_scatter.comp::ge_SelectLOD's selection loop, built on
// the shared LodEffectiveThreshold so the descend/ceiling rule under test is
// the SAME code the shader mirrors. Returns the selected LOD for a coverage.
uint32 SelectLod(const float* threshold, uint32 sseSlotMask, uint32 lodCount,
                 float coverage, float sseToCoverage, float sseScaleCeil = 0.0f) {
    uint32 lod = lodCount - 1u;
    float prevEff = Rendering::kLodEffNone;
    for (uint32 k = 0; k < lodCount; ++k) {
        const float eff = Rendering::LodEffectiveThreshold(
            threshold[k], ((sseSlotMask >> k) & 1u) != 0u, sseToCoverage, sseScaleCeil,
            prevEff);
        prevEff = eff;
        if (coverage >= eff) return k;
    }
    return lod;
}

// The effective (coverage-space) switch points the scan compares against, so a
// test can assert on the switch points themselves and not only on the level a
// given coverage selects.
void EffectiveRow(const float* threshold, uint32 sseSlotMask, uint32 lodCount,
                  float sseToCoverage, float sseScaleCeil, float* outEff) {
    float prevEff = Rendering::kLodEffNone;
    for (uint32 k = 0; k < lodCount; ++k) {
        outEff[k] = Rendering::LodEffectiveThreshold(
            threshold[k], ((sseSlotMask >> k) & 1u) != 0u, sseToCoverage, sseScaleCeil,
            prevEff);
        prevEff = outEff[k];
    }
}
} // namespace

// A sloppy level in the far slot keeps the cap, and the non-sloppy slots come
// back SSE-normalized (mask bits set) rather than in coverage space.
TEST(LODThresholds, SloppyLevelFallsBackToCappedCoverageSlot) {
    const float err[4]    = {0.0f, 0.04f, 0.10f, 0.5f};
    const uint8_t slop[4] = {0, 0, 0, 1};   // LOD3 fell back to sloppy
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 4u, /*authored=*/false, out);
    EXPECT_NEAR(out[0], 1.0f / 0.04f, 1e-3f);   // SSE: radius/(e*extent)
    EXPECT_NEAR(out[1], 1.0f / 0.10f, 1e-3f);
    // Sloppy far slot: min(defaults[2]=0.08, cap), in COVERAGE space.
    EXPECT_NEAR(out[2], Rendering::kLodSloppyThresholdCap, 1e-6f);
    EXPECT_NEAR(out[3], 0.0f, 1e-6f);
    EXPECT_EQ(mask, 0b0011u) << "only the two error-derived slots are SSE";
}

// Pins the shipped cap value with its re-raise contract. History: 0.1 at ship;
// 0.03 after the 2026-07-21 triage proved sloppy shells attribute-corrupted.
// Generator v4's attribute-honest shells deleted that chromatic damage class,
// but the re-raise experiment (real 0.06 + emulated 0.05/0.045 at the triage
// poses) showed the remaining artifact — thin multi-chart props amalgamating
// into their largest chart (banners fading to plaster-white, boats to pale
// hulls) — stays salient down to ~0.03 regardless of shell honesty, so the
// cap stays at the salience floor. Raising it is a deliberate act gated on
// real mid LOD levels (seam-aware mid-tier arc) plus fresh A/B evidence at
// the triage poses.
TEST(LODThresholds, SloppyCapValuePinnedAtSalienceFloor) {
    EXPECT_FLOAT_EQ(Rendering::kLodSloppyThresholdCap, 0.03f);
    // The cap sits BELOW every default engage-slot, including the far slot —
    // a sloppy level always engages later than any traditional level would.
    EXPECT_LT(Rendering::kLodSloppyThresholdCap, kDefaults[2]);
}

// The SSE budget must never reach a sloppy slot: sloppy errors are on a
// different scale, so the cap is a COVERAGE-space constant and no per-view
// budget may scale it. Sweeping the budget over three decades must leave the
// sloppy slot's effective switch point pinned at the cap.
TEST(LODThresholds, SloppyCapIsImmuneToTheErrorBudget) {
    const float err[4]    = {0.0f, 0.9f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 1, 0, 0};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 2u, /*authored=*/false, out);
    EXPECT_EQ(mask, 0u) << "a sloppy slot is never SSE-normalized";
    for (float budgetPx : {0.5f, 5.0f, 10.0f, 100.0f}) {
        const float toCov = Rendering::LodSseThresholdToCoverage(1080u, budgetPx);
        const float eff = Rendering::LodEffectiveThreshold(
            out[0], /*sseSlot=*/false, toCov, /*sseScaleCeil=*/0.0f,
            Rendering::kLodEffNone);
        EXPECT_FLOAT_EQ(eff, Rendering::kLodSloppyThresholdCap)
            << "budget " << budgetPx << " must not move the sloppy cap";
    }
}

// A generated sloppy level landing in an EARLY slot (skip-created chain, e.g.
// [LOD0, sloppy] on an open shell whose locked mid levels all skipped) must
// cap at kLodSloppyThresholdCap — the early defaults (0.5/0.2) would engage
// the crude border-moving level at half-screen coverage.
TEST(LODThresholds, SloppyLevelInEarlySlotCapsEngagement) {
    const float err[4]    = {0.0f, 0.9f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 1, 0, 0};   // one-level chain, level is sloppy
    float out[4] = {};
    Derive(err, slop, 2u, /*authored=*/false, out);
    EXPECT_NEAR(out[0], Rendering::kLodSloppyThresholdCap, 1e-6f);
    EXPECT_NEAR(out[1], 0.0f, 1e-6f);
    EXPECT_GT(kDefaults[2], Rendering::kLodSloppyThresholdCap);
}

// A GENERATED sloppy level with achieved error exactly 0.0 must still get the
// sloppy cap, not the authored default table. Regression lock for the deleted
// err==0 authored-marker sentinel: the shipped ElvenRealm v3 corpus cooked a
// 1-triangle sloppy level with error exactly 0.0 that the sentinel classified
// as authored. Provenance is the explicit authoredChain flag now.
TEST(LODThresholds, GeneratedSloppyLevelWithZeroErrorStillCaps) {
    const float err[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 1, 0, 0};   // one-level chain [LOD0, sloppy@err0]
    float out[4] = {};
    Derive(err, slop, 2u, /*authored=*/false, out);
    EXPECT_NEAR(out[0], Rendering::kLodSloppyThresholdCap, 1e-6f);
}

// The provenance bit beats any error/sloppy content: an authored chain
// reproduces the tuned default table exactly, whatever its parallel arrays
// carry, and stays entirely in coverage space (mask 0) so selection is
// byte-identical to the pre-SSE path.
TEST(LODThresholds, AuthoredChainProvenanceBeatsErrorContent) {
    const float err[4]    = {0.0f, 0.0f, 0.7f, 0.001f}; // mixed junk content
    const uint8_t slop[4] = {0, 1, 0, 1};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 4u, /*authored=*/true, out);
    EXPECT_NEAR(out[0], kDefaults[0], 1e-6f);
    EXPECT_NEAR(out[1], kDefaults[1], 1e-6f);
    EXPECT_NEAR(out[2], kDefaults[2], 1e-6f);
    EXPECT_NEAR(out[3], 0.0f, 1e-6f);   // coarsest always matches
    EXPECT_EQ(mask, 0u) << "authored rows must carry no SSE bit";
}

// An authored row's effective thresholds must equal its stored thresholds at
// EVERY budget — the SSE machinery has to be inert for authored content, not
// merely close.
TEST(LODThresholds, AuthoredRowSelectionIsBudgetInvariant) {
    const float err[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 4u, /*authored=*/true, out);
    for (float budgetPx : {1.0f, 10.0f, 250.0f}) {
        const float toCov = Rendering::LodSseThresholdToCoverage(1080u, budgetPx);
        float prevEff = Rendering::kLodEffNone;
        for (uint32 k = 0; k < 4u; ++k) {
            const float eff = Rendering::LodEffectiveThreshold(
                out[k], ((mask >> k) & 1u) != 0u, toCov, /*sseScaleCeil=*/0.0f, prevEff);
            EXPECT_FLOAT_EQ(eff, out[k])
                << "authored slot " << k << " moved at budget " << budgetPx;
            prevEff = eff;
        }
    }
}

// Near-lossless cooks (achieved error far below the slot's floor) must floor
// at the anchor instead of saturating. Unfloored, 1/e is unbounded: these
// errors would derive thresholds of 1e4..1e3, putting the switch tens of
// thousands of pixels wide — the coarser level engages at EVERY on-screen
// size and LOD0 is unreachable (the bridge-arch "permanently LOD1" bug).
// Both cooked corpora measured 2026-07-25 sit in exactly this regime.
TEST(LODThresholds, NearLosslessErrorsFloorAtAnchorFloors) {
    const float err[4]    = {0.0f, 1e-4f, 5e-4f, 1e-3f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    Derive(err, slop, 4u, /*authored=*/false, out);
    // Literals, NOT 1/kLodErrorFloor[k] — deriving the expectation from the
    // constant under test would pass for any floor value, including none.
    // radius/extent are 1 here, so these are 1/0.04, 1/0.10, 1/0.20.
    EXPECT_NEAR(out[0], 25.0f, 1e-3f);
    EXPECT_NEAR(out[1], 10.0f, 1e-3f);
    EXPECT_NEAR(out[2], 5.0f, 1e-3f);
    EXPECT_NEAR(out[3], 0.0f, 1e-6f);
}

// The floor VALUES themselves, pinned like the sloppy cap: moving an anchor is
// a deliberate re-tune, not a side effect of touching the mapping.
TEST(LODThresholds, ErrorFloorAnchorsPinned) {
    ASSERT_GE(std::size(Rendering::kLodErrorFloor), 4u);
    EXPECT_FLOAT_EQ(Rendering::kLodErrorFloor[0], 0.04f);
    EXPECT_FLOAT_EQ(Rendering::kLodErrorFloor[1], 0.10f);
    EXPECT_FLOAT_EQ(Rendering::kLodErrorFloor[2], 0.20f);
    EXPECT_FLOAT_EQ(Rendering::kLodErrorFloor[3], 0.20f);
    // Every floor must stay above the divide-by-zero guard, or the guard would
    // silently become the real floor.
    for (float f : Rendering::kLodErrorFloor)
        EXPECT_GT(f, Rendering::kMinLodError);
}

// The floor expressed as the thing it actually controls: the LARGEST on-screen
// size at which a slot may engage. At the default budget a floor-error slot0
// switches when the mesh's bounding sphere spans 500 px; without the floor an
// e=1e-4 cook would switch at 200000 px, i.e. always.
TEST(LODThresholds, FloorBoundsTheSwitchSizeInPixels) {
    const float err[4]    = {0.0f, 1e-4f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    Derive(err, slop, 2u, /*authored=*/false, out);
    const float switchPx = 2.0f * Rendering::kDefaultLodErrorBudgetPx * out[0];
    EXPECT_NEAR(switchPx, 2.0f * Rendering::kDefaultLodErrorBudgetPx /
                              Rendering::kLodErrorFloor[0], 0.5f);
    EXPECT_LT(switchPx, 1080.0f)
        << "a floored slot must not engage at every on-screen size";
}

// Worse-than-floor errors still adapt: a level that spends a bigger error
// self-delays, switching at a proportionally smaller on-screen size.
TEST(LODThresholds, WorseThanFloorErrorsStillDelayEngagement) {
    const float err[4]    = {0.0f, 0.08f, 0.16f, 0.40f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 4u, /*authored=*/false, out);
    EXPECT_EQ(mask, 0b0111u);
    EXPECT_NEAR(out[0], 1.0f / 0.08f, 1e-3f);
    EXPECT_NEAR(out[1], 1.0f / 0.16f, 1e-3f);
    EXPECT_NEAR(out[2], 1.0f / 0.40f, 1e-3f);
    // Worse than the floor everywhere, so nothing was clamped.
    EXPECT_LT(out[0], 1.0f / Rendering::kLodErrorFloor[0]);
}

// Budgets must stay at or above their slot's floor — a budget below the floor
// could never delay engagement and would silently re-couple the two.
TEST(LODThresholds, CookBudgetsStayAboveTheirEngagementFloors) {
    ASSERT_GE(std::size(Rendering::kLodErrorFloor), 3u);
    const MeshLODConfig defaults{};
    for (uint32 k = 0; k + 1u < MeshLODConfig::kMaxLODs; ++k)
        EXPECT_GE(defaults.TargetError[k + 1u], Rendering::kLodErrorFloor[k])
            << "default TargetError[" << (k + 1u) << "] below its engagement floor";
}

// No error data (legacy / procedural meshes) falls back to the default table
// in coverage space, and so do degenerate bounds — a zero-radius or
// zero-extent mesh must not divide by zero into an SSE threshold.
TEST(LODThresholds, NoErrorDataAndDegenerateBoundsFallBackToDefaults) {
    const float err[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    EXPECT_EQ(Derive(err, slop, 4u, /*authored=*/false, out), 0u);
    EXPECT_NEAR(out[0], kDefaults[0], 1e-6f);

    const float lossy[4] = {0.0f, 0.05f, 0.10f, 0.20f};
    float degen[4] = {};
    EXPECT_EQ(Derive(lossy, slop, 4u, false, degen, /*radius=*/0.0f, /*extent=*/1.0f), 0u);
    EXPECT_NEAR(degen[0], kDefaults[0], 1e-6f);
    EXPECT_EQ(Derive(lossy, slop, 4u, false, degen, /*radius=*/1.0f, /*extent=*/0.0f), 0u);
    EXPECT_NEAR(degen[0], kDefaults[0], 1e-6f);
    for (float v : degen) EXPECT_TRUE(std::isfinite(v));
}

TEST(LODThresholds, SingleLodIsAllZero) {
    const float err[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {9, 9, 9, 9};
    EXPECT_EQ(Derive(err, slop, 1u, /*authored=*/false, out), 0u);
    for (float v : out) EXPECT_NEAR(v, 0.0f, 1e-6f);
}

// The clamp the authored direct-coverage path uses (F8): a slot not strictly
// below its predecessor is pulled down; the coarsest slot is left as-is.
TEST(LODThresholds, ClampCallablePullsNonDescendingSlotsDown) {
    float t[4] = {0.5f, 0.7f, 0.3f, 0.0f}; // t[1] > t[0] violates descent
    Rendering::ClampLODThresholdsDescending(t, 4u, 4u);
    EXPECT_GT(t[0], t[1]);
    EXPECT_GT(t[1], t[2]);
    EXPECT_FLOAT_EQ(t[3], 0.0f); // coarsest slot untouched
}

// --- Selection-time descent (mirrors ge_SelectLOD) ------------------------

// THE regression this fix exists for. A chain whose slot0 is a capped sloppy
// level (coverage 0.03) followed by a near-lossless SSE slot1 has effective
// thresholds that ASCEND across the space boundary. Clamping only within
// same-space runs left LOD1 unreachable at every coverage: the scan fell from
// LOD0 straight to LOD2, promoting a COARSER level earlier and popping two
// levels at once. Selection-time descent must make LOD1 reachable.
TEST(LodSelection, MidLevelStaysReachableAcrossASpaceBoundary) {
    // slot0: sloppy -> coverage 0.03. slot1: e=0.0087 -> SSE threshold ~115.
    const float err[4]    = {0.0f, 0.5f, 0.0087f, 0.0f};
    const uint8_t slop[4] = {0, 1, 0, 0};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 3u, /*authored=*/false, out);
    ASSERT_EQ(mask, 0b010u) << "slot0 coverage (sloppy), slot1 SSE";
    const float toCov = Rendering::LodSseThresholdToCoverage(902u, 10.0f);
    ASSERT_GT(out[1] * toCov, out[0])
        << "precondition: raw effective thresholds ascend across the boundary";

    bool sawLod1 = false;
    for (int i = 1; i <= 100000; ++i)
        if (SelectLod(out, mask, 3u, static_cast<float>(i) * 0.001f, toCov) == 1u) {
            sawLod1 = true;
            break;
        }
    EXPECT_TRUE(sawLod1) << "LOD1 must be reachable at some coverage";
}

// The two invariants descent actually buys, over a coverage sweep:
//   1. monotonic — as the object shrinks the selected LOD only gets coarser;
//   2. no level is SKIPPED — strictly descending effective thresholds leave a
//      non-empty coverage band for every level.
// (1) alone is not sufficient: a chain that jumps LOD0 -> LOD2 is still
// monotone, which is exactly how the mixed-space bug hid.
TEST(LodSelection, CoarsensMonotonicallyAndReachesEveryLevel) {
    struct Case { float err[4]; uint8_t slop[4]; uint32 lodCount; };
    const Case cases[] = {
        {{0.0f, 0.5f, 0.0087f, 0.0f},  {0, 1, 0, 0}, 3u},  // sloppy then SSE
        {{0.0f, 0.10f, 0.02f, 0.01f},  {0, 0, 0, 0}, 4u},  // non-monotonic errors
        {{0.0f, 0.05f, 0.9f, 0.06f},   {0, 0, 1, 0}, 4u},  // sloppy in the middle
        {{0.0f, 0.30f, 0.0f, 0.20f},   {0, 0, 0, 0}, 4u},  // no-data slot mid-chain
    };
    for (const Case& c : cases) {
        float out[4] = {};
        const uint32 mask = Derive(c.err, c.slop, c.lodCount, false, out);
        const float toCov = Rendering::LodSseThresholdToCoverage(1080u, 10.0f);
        uint32 prev = 0u;
        uint32 seen = 0u;
        for (int i = 20000; i >= 1; --i) {
            const uint32 lod = SelectLod(out, mask, c.lodCount,
                                         static_cast<float>(i) * 0.001f, toCov);
            EXPECT_GE(lod, prev) << "LOD got FINER as coverage fell (mask " << mask << ")";
            prev = lod;
            seen |= 1u << lod;
        }
        const uint32 allLevels = (1u << c.lodCount) - 1u;
        EXPECT_EQ(seen, allLevels)
            << "a level was unreachable at every coverage (mask " << mask
            << ", seen " << seen << " of " << allLevels << ")";
    }
}

// The ceiling still guards a custom budget: however large budgetPx grows, an
// SSE slot may not engage earlier than the default LOD0->LOD1 switch point.
TEST(LodSelection, CeilingBoundsSseSlotsUnderALargeBudget) {
    for (float budgetPx : {10.0f, 100.0f, 1000.0f}) {
        const float toCov = Rendering::LodSseThresholdToCoverage(1080u, budgetPx);
        const float eff = Rendering::LodEffectiveThreshold(
            /*threshold=*/1.0f / Rendering::kLodErrorFloor[0], /*sseSlot=*/true,
            toCov, /*sseScaleCeil=*/0.0f, Rendering::kLodEffNone);
        EXPECT_LE(eff, Rendering::kLodThresholdCeil);
    }
}

// --- Small-viewport contract: the per-mesh SSE scale ceiling -----------------
//
// A fixed pixel budget is a fixed FRACTION 2*budgetPx/viewportH of the image, so
// a short view spends it over a proportionally larger share of the frame and the
// SSE mapping coarsens without bound. LodSseScaleCeil floors that at the coverage
// mapping the SSE path replaced. These pin the three parts of the contract:
// below break-even SSE never goes coarser, at break-even the two coincide, and
// above break-even nothing moves at all.

namespace {
// Break-even viewport height for a mesh of shape factor phi at a given budget:
// where 2*budgetPx/viewportH == kLodCoverageErrorScale * phi.
float BreakEvenHeight(float phi, float budgetPx) {
    return 2.0f * budgetPx / (Rendering::kLodCoverageErrorScale * phi);
}

// The geometric range of phi = maxExtent / boundingRadius: 2/sqrt(3) for a cube,
// 2.0 for a degenerate sliver. The AncientEmpire cook measured min 1.163,
// median 1.548, max 1.998 over its 638 chained submeshes.
constexpr float kPhiCube = 1.1547005f;
constexpr float kPhiSliver = 2.0f;
} // namespace

// The contract: on a row whose switch slots are all SSE-normalized, no SSE slot
// may engage at a HIGHER coverage than the coverage mapping would put it at, at
// any viewport height (a mixed row's descend coupling is outside the guarantee
// — see LodSseScaleCeil). Swept over the geometric phi range, the whole
// plausible viewport range, and errors both above and below the slot floors.
TEST(LodSelection, SseIsNeverCoarserThanCoverageAtAnyViewportHeight) {
    const float budgetPx = Rendering::kDefaultLodErrorBudgetPx;
    const uint8_t slop[4] = {0, 0, 0, 0};
    // Below every floor, straddling them, and above every floor.
    const float errorSets[3][4] = {{0.0f, 0.0001f, 0.0001f, 0.0001f},
                                   {0.0f, 0.0600f, 0.1200f, 0.2500f},
                                   {0.0f, 0.3000f, 0.4000f, 0.5000f}};
    int comparisons = 0;
    for (float phi : {kPhiCube, 1.30f, 1.5483f, 1.86f, kPhiSliver}) {
        for (const auto& err : errorSets) {
            float sseT[4] = {};
            const uint32 mask = Rendering::DeriveLODThresholds(
                err, slop, 4u, /*authoredChain=*/false, /*boundingRadius=*/1.0f,
                /*maxExtent=*/phi, kDefaults, 4u, sseT);
            ASSERT_NE(mask, 0u) << "phi " << phi << " must produce SSE slots";
            float covT[4] = {};
            Rendering::DeriveLODThresholdsCoverage(err, slop, 4u, /*authoredChain=*/false,
                                                   kDefaults, 4u, covT);
            const float ceil = Rendering::LodSseScaleCeil(1.0f, phi);
            for (uint32 h : {2160u, 1080u, 902u, 866u, 720u, 600u, 500u, 400u, 256u,
                             128u, 72u, 32u}) {
                const float toCov = Rendering::LodSseThresholdToCoverage(h, budgetPx);
                float sseEff[4] = {}, covEff[4] = {};
                EffectiveRow(sseT, mask, 4u, toCov, ceil, sseEff);
                EffectiveRow(covT, 0u, 4u, toCov, 0.0f, covEff);
                for (uint32 k = 0; k + 1u < 4u; ++k) {
                    if (((mask >> k) & 1u) == 0u) continue;
                    ++comparisons;
                    EXPECT_LE(sseEff[k], covEff[k] * 1.0001f)
                        << "SSE coarser than coverage: phi " << phi << " height " << h
                        << " slot " << k << " (" << sseEff[k] << " vs " << covEff[k] << ")";
                }
            }
        }
    }
    EXPECT_GT(comparisons, 100) << "the sweep must actually compare SSE slots";
}

// Above a mesh's break-even height the ceiling is inert: the effective row is
// bit-for-bit what it was without any cap, so tall views — every shipping camera
// at 1080p and up — are untouched.
TEST(LodSelection, ScaleCeilingIsInertAboveTheBreakEvenHeight) {
    const float budgetPx = Rendering::kDefaultLodErrorBudgetPx;
    const float err[4]    = {0.0f, 0.06f, 0.12f, 0.25f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    for (float phi : {kPhiCube, 1.5483f, kPhiSliver}) {
        float sseT[4] = {};
        const uint32 mask = Rendering::DeriveLODThresholds(
            err, slop, 4u, /*authoredChain=*/false, 1.0f, phi, kDefaults, 4u, sseT);
        const float ceil = Rendering::LodSseScaleCeil(1.0f, phi);
        const float hStar = BreakEvenHeight(phi, budgetPx);
        // Strictly above break-even, in whole pixels.
        for (float mult : {1.0f, 1.5f, 3.0f}) {
            const uint32 h = static_cast<uint32>(std::ceil(hStar * mult)) + 1u;
            const float toCov = Rendering::LodSseThresholdToCoverage(h, budgetPx);
            float capped[4] = {}, uncapped[4] = {};
            EffectiveRow(sseT, mask, 4u, toCov, ceil, capped);
            EffectiveRow(sseT, mask, 4u, toCov, /*sseScaleCeil=*/0.0f, uncapped);
            for (uint32 k = 0; k < 4u; ++k)
                EXPECT_FLOAT_EQ(capped[k], uncapped[k])
                    << "ceiling moved slot " << k << " at height " << h
                    << " (above break-even " << hStar << ", phi " << phi << ")";
        }
    }
}

// At the break-even height the two mappings agree exactly — the property that
// makes the cap a floor on quality rather than a second tuning knob.
TEST(LodSelection, SseMeetsCoverageAtTheBreakEvenHeight) {
    const float budgetPx = Rendering::kDefaultLodErrorBudgetPx;
    const float err[4]    = {0.0f, 0.06f, 0.12f, 0.25f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    for (float phi : {kPhiCube, 1.5483f, kPhiSliver}) {
        float sseT[4] = {};
        const uint32 mask = Rendering::DeriveLODThresholds(
            err, slop, 4u, /*authoredChain=*/false, 1.0f, phi, kDefaults, 4u, sseT);
        float covT[4] = {};
        Rendering::DeriveLODThresholdsCoverage(err, slop, 4u, /*authoredChain=*/false,
                                               kDefaults, 4u, covT);
        // The factor AT break-even is the ceiling itself, by construction.
        const float ceil = Rendering::LodSseScaleCeil(1.0f, phi);
        EXPECT_NEAR(ceil, Rendering::LodSseThresholdToCoverage(
                              static_cast<uint32>(std::lround(BreakEvenHeight(phi, budgetPx))),
                              budgetPx),
                    1e-4f);
        float sseEff[4] = {}, covEff[4] = {};
        EffectiveRow(sseT, mask, 4u, ceil, ceil, sseEff);
        EffectiveRow(covT, 0u, 4u, ceil, 0.0f, covEff);
        for (uint32 k = 0; k + 1u < 4u; ++k) {
            if (((mask >> k) & 1u) == 0u) continue;
            EXPECT_NEAR(sseEff[k], covEff[k], 1e-5f)
                << "slot " << k << " disagrees at break-even (phi " << phi << ")";
        }
    }
}

// The ceiling is derived from the mesh's TRUE shape factor, so a compact mesh and
// an elongated one get different ceilings. A single per-view clamp would have to
// assume the compact end and would hold an elongated mesh's detail up to
// phi_max/phi_cube = 1.73x too long — the reason the cap lives on the row.
TEST(LodSelection, ScaleCeilingTracksTheMeshShapeFactor) {
    const float cube = Rendering::LodSseScaleCeil(1.0f, kPhiCube);
    const float sliver = Rendering::LodSseScaleCeil(1.0f, kPhiSliver);
    EXPECT_FLOAT_EQ(cube, Rendering::kLodCoverageErrorScale * kPhiCube);
    EXPECT_FLOAT_EQ(sliver, Rendering::kLodCoverageErrorScale * kPhiSliver);
    EXPECT_LT(cube, sliver) << "a compact mesh must cap the factor sooner";
    EXPECT_NEAR(sliver / cube, kPhiSliver / kPhiCube, 1e-5f);
    // Scale-invariant: the ceiling depends on the shape, not the size.
    EXPECT_FLOAT_EQ(Rendering::LodSseScaleCeil(10.0f, 10.0f * kPhiCube), cube);
    // Degenerate bounds carry no ceiling.
    EXPECT_FLOAT_EQ(Rendering::LodSseScaleCeil(0.0f, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(Rendering::LodSseScaleCeil(1.0f, 0.0f), 0.0f);
}

// budgetPx <= 0 is the keep-detail fail-safe: SSE slots resolve to 0 and match
// at any coverage, so a slice that never sets the factor never coarsens
// through one.
TEST(LodSelection, NonPositiveBudgetKeepsDetail) {
    EXPECT_FLOAT_EQ(Rendering::LodSseThresholdToCoverage(1080u, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Rendering::LodSseThresholdToCoverage(1080u, -1.0f), 0.0f);
    const float err[4]    = {0.0f, 0.05f, 0.10f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    const uint32 mask = Derive(err, slop, 3u, /*authored=*/false, out);
    EXPECT_EQ(SelectLod(out, mask, 3u, 1e-6f, 0.0f), 0u) << "must stay at LOD0";
}

// The per-view factor holds a fixed ON-SCREEN size: doubling the viewport height
// must not move the pixel size at which a slot engages (it moves the world
// distance instead). This is the property the whole reformulation buys, and it
// governs every viewport at or above a mesh's break-even height. Below that the
// per-mesh ceiling takes over and the slot switches at a fixed coverage instead
// — see SseIsNeverCoarserThanCoverageAtAnyViewportHeight and friends.
TEST(LodSelection, SwitchSizeInPixelsIsViewportInvariant) {
    const float err[4]    = {0.0f, 0.08f, 0.0f, 0.0f};
    const uint8_t slop[4] = {0, 0, 0, 0};
    float out[4] = {};
    Derive(err, slop, 2u, /*authored=*/false, out);
    const float budgetPx = Rendering::kDefaultLodErrorBudgetPx;
    for (uint32 h : {720u, 1080u, 2160u}) {
        const float toCov = Rendering::LodSseThresholdToCoverage(h, budgetPx);
        // coverage at the switch, converted to sphere diameter in pixels.
        const float switchPx = out[0] * toCov * static_cast<float>(h);
        EXPECT_NEAR(switchPx, 2.0f * budgetPx * out[0], 0.5f)
            << "switch size moved with viewport height " << h;
    }
}

// --- Phase C1: authored-LOD interactions ---------------------------------

// A small mesh carrying an authored LOD chain (own vertices + LOD-local indices).
namespace {
Mesh MakeAuthoredMesh() {
    Mesh mesh;
    mesh.Name = "Authored";
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    mesh.Vertices.resize(4);
    mesh.Indices = {0u, 1u, 2u, 0u, 2u, 3u};
    mesh.MinBounds[0] = -1.0f; mesh.MinBounds[1] = -1.0f; mesh.MinBounds[2] = -1.0f;
    mesh.MaxBounds[0] = 1.0f;  mesh.MaxBounds[1] = 1.0f;  mesh.MaxBounds[2] = 1.0f;
    mesh.ExtraLODVertices.push_back(Vector<Vertex>(3)); // authored LOD1 vertices
    mesh.ExtraLODs.push_back({0u, 1u, 2u});             // LOD-local indices
    mesh.AuthoredLODs = true; // explicit provenance (the import funnel stamps it)
    return mesh;
}
} // namespace

// Authored-wins (#2a): GenerateLODs must skip an authored submesh WITHOUT
// clearing its chain, while still generating for a generatable sibling.
TEST(AuthoredLODGeneration, GenerateSkipsAuthoredSubmeshWithoutClearing) {
    ModelAsset model(GUID::Generate(), "authored.fbx");
    Vector<Mesh> meshes;
    meshes.push_back(MakeAuthoredMesh());
    meshes.push_back(MakeGridPlane(16)); // generatable
    model.SetMeshesForTest(std::move(meshes));

    model.GenerateLODs(MeshLODConfig{}, /*generateSkinned=*/false);

    // Authored submesh: chain intact, not regenerated, not cleared.
    EXPECT_TRUE(model.GetMesh(0).HasAuthoredLODs());
    ASSERT_EQ(model.GetMesh(0).ExtraLODVertices.size(), 1u);
    EXPECT_EQ(model.GetMesh(0).ExtraLODVertices[0].size(), 3u);
    ASSERT_EQ(model.GetMesh(0).ExtraLODs.size(), 1u);
    EXPECT_EQ(model.GetMesh(0).ExtraLODs[0], (Vector<uint32>{0u, 1u, 2u}));

    // Generatable sibling: still gets generated LODs (when meshopt is available).
    EXPECT_FALSE(model.GetMesh(1).HasAuthoredLODs());
    if (IsMeshLODGenerationAvailable())
        EXPECT_GT(model.GetMesh(1).ExtraLODs.size(), 0u);
}

// --- Phase C2a: authored-LOD import sources ------------------------------

namespace {

// A minimal named triangle-list submesh with `nVerts` distinct vertices inside
// [-1,1] and one triangle (indices {0,1,2}). No optional streams.
Mesh MakeNamedTriMesh(const std::string& name, uint32 nVerts = 3u) {
    Mesh mesh;
    mesh.MaterialIndex = 0;
    mesh.Name = name;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    mesh.Vertices.resize(nVerts);
    for (uint32 i = 0; i < nVerts; ++i)
        mesh.Vertices[i].Position[0] = static_cast<float>(i) * 0.1f;
    mesh.Indices = {0u, 1u, 2u};
    mesh.MinBounds[0] = -1.0f; mesh.MinBounds[1] = -1.0f; mesh.MinBounds[2] = -1.0f;
    mesh.MaxBounds[0] = 1.0f;  mesh.MaxBounds[1] = 1.0f;  mesh.MaxBounds[2] = 1.0f;
    return mesh;
}

// Feed an authored chain through the SAME error/sloppy -> lodThreshold path the
// GPU row build uses (UploadMesh maps ExtraLODErrors/Sloppy off-by-one into the
// entry plus the explicit HasAuthoredLODs provenance bit, then
// DeriveLODThresholds runs). Returns the derived thresholds.
std::array<float, 4> DeriveThresholdsForAuthored(const Mesh& mesh) {
    const uint32 lodCount = std::min<uint32>(mesh.LODCount(), 4u);
    float err[4] = {0, 0, 0, 0};
    uint8_t slop[4] = {0, 0, 0, 0};
    for (uint32 k = 1; k < lodCount; ++k) {
        err[k]  = (k - 1u) < mesh.ExtraLODErrors.size() ? mesh.ExtraLODErrors[k - 1u] : 0.0f;
        slop[k] = (k - 1u) < mesh.ExtraLODSloppy.size() ? mesh.ExtraLODSloppy[k - 1u] : 0u;
    }
    std::array<float, 4> out{};
    // Bounds are irrelevant on this path: an authored chain reproduces the
    // default table in coverage space and never reaches the SSE mapping.
    Rendering::DeriveLODThresholds(err, slop, lodCount, mesh.HasAuthoredLODs(),
                                   /*boundingRadius=*/1.0f, /*maxExtent=*/1.0f,
                                   kDefaults, 4u, out.data());
    return out;
}

} // namespace

TEST(AuthoredLodImport, ConsumesContiguousSuffixFamily) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("Torso", 4u));
    meshes.push_back(MakeNamedTriMesh("Torso_LOD1", 3u));
    meshes.push_back(MakeNamedTriMesh("Torso_LOD2", 3u));

    const uint32 consumed = ConsumeLodSuffixFamilies(meshes);

    EXPECT_EQ(consumed, 2u);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].Name, "Torso");           // base name, siblings gone
    ASSERT_EQ(meshes[0].ExtraLODVertices.size(), 2u);
    ASSERT_EQ(meshes[0].ExtraLODs.size(), 2u);
    EXPECT_EQ(meshes[0].ExtraLODVertices[0].size(), 3u); // LOD1 own vertices
    EXPECT_EQ(meshes[0].ExtraLODs[0], (Vector<uint32>{0u, 1u, 2u})); // LOD-local
    EXPECT_TRUE(meshes[0].HasAuthoredLODs());
}

TEST(AuthoredLodImport, StripsLod0BaseToBaseName) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("Barrel_LOD0", 4u));
    meshes.push_back(MakeNamedTriMesh("Barrel_LOD1", 3u));

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 1u);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].Name, "Barrel");          // _LOD0 stripped to base
    ASSERT_EQ(meshes[0].ExtraLODs.size(), 1u);
}

TEST(AuthoredLodImport, CaseInsensitiveSuffixAndBase) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("Rock", 4u));
    meshes.push_back(MakeNamedTriMesh("rock_lod1", 3u)); // lower-case base + suffix

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 1u);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].Name, "Rock");            // base spelling preserved
    ASSERT_EQ(meshes[0].ExtraLODs.size(), 1u);
}

TEST(AuthoredLodImport, GapEndsChainOrphanStaysStandalone) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("A", 4u));
    meshes.push_back(MakeNamedTriMesh("A_LOD1", 3u));
    meshes.push_back(MakeNamedTriMesh("A_LOD3", 3u)); // gap at LOD2

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 1u); // only LOD1 consumed
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_EQ(meshes[0].Name, "A");
    ASSERT_EQ(meshes[0].ExtraLODs.size(), 1u);
    EXPECT_EQ(meshes[1].Name, "A_LOD3");          // orphan left standalone
}

TEST(AuthoredLodImport, OrphanSiblingWithoutBaseUntouched) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("B_LOD1", 3u));
    meshes.push_back(MakeNamedTriMesh("B_LOD2", 3u)); // no LOD0/base

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 0u);
    EXPECT_EQ(meshes.size(), 2u);                 // nothing consumed
}

TEST(AuthoredLodImport, MixedOptionalStreamsRefused) {
    Vector<Mesh> meshes;
    Mesh base = MakeNamedTriMesh("C", 4u);
    base.Color0.assign(base.Vertices.size() * 4u, 1.0f); // optional stream present
    meshes.push_back(std::move(base));
    meshes.push_back(MakeNamedTriMesh("C_LOD1", 3u));

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 0u); // refused
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());
    EXPECT_EQ(meshes[1].Name, "C_LOD1");
}

// Content with no _LOD suffixes is byte-identical (the ElvenRealm invariance the
// C2a gate checks at runtime, asserted here structurally).
TEST(AuthoredLodImport, NoSuffixIsNoOp) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("Mesh_0", 4u));
    meshes.push_back(MakeNamedTriMesh("Mesh_1", 4u));

    EXPECT_EQ(ConsumeLodSuffixFamilies(meshes), 0u);
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());
    EXPECT_FALSE(meshes[1].HasAuthoredLODs());
}

// A consumed chain has no meshopt error, so its thresholds must reproduce the
// default descending table via the explicit authored provenance bit
// (amendment #13), NOT the coverage ceiling that all-zero-error would produce
// and NOT the generated-sloppy cap its descriptive sloppy markers would take.
TEST(AuthoredLodImport, ThresholdsReproduceDefaultDescendingTable) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeNamedTriMesh("D", 4u));
    meshes.push_back(MakeNamedTriMesh("D_LOD1", 3u));
    meshes.push_back(MakeNamedTriMesh("D_LOD2", 3u));
    ASSERT_EQ(ConsumeLodSuffixFamilies(meshes), 2u);

    const Mesh& m = meshes[0];
    ASSERT_EQ(m.ExtraLODSloppy.size(), 2u);
    EXPECT_EQ(m.ExtraLODSloppy[0], 1u);           // all-sloppy routes to defaults
    EXPECT_EQ(m.ExtraLODSloppy[1], 1u);

    const std::array<float, 4> t = DeriveThresholdsForAuthored(m); // lodCount == 3
    EXPECT_NEAR(t[0], kDefaults[0], 1e-6f);
    EXPECT_NEAR(t[1], kDefaults[1], 1e-6f);
    EXPECT_NEAR(t[2], 0.0f, 1e-6f);               // coarsest present slot matches
    EXPECT_GT(t[0], t[1]);
    EXPECT_GT(t[1], t[2]);
}

TEST(AuthoredLodImport, SlotKvRoundTrip) {
    const GUID g = GUID::Generate();
    const std::string enc = EncodeLodSlotValue("Models/Torso_lod1.gltf", g);
    EXPECT_NE(enc.find(g.ToString()), std::string::npos);

    std::string path;
    const GUID decoded = DecodeLodSlotValue(enc, &path);
    EXPECT_EQ(decoded, g);
    EXPECT_EQ(path, "Models/Torso_lod1.gltf");

    // Empty / null cases.
    EXPECT_TRUE(EncodeLodSlotValue("", GUID::Null()).empty());
    EXPECT_TRUE(DecodeLodSlotValue("").IsNull());
    EXPECT_TRUE(DecodeLodSlotValue("not-an-assetref").IsNull());
}

TEST(AuthoredLodImport, AppendSlotLevelAlignsByName) {
    Vector<Mesh> target;
    target.push_back(MakeNamedTriMesh("Torso", 4u));
    target.push_back(MakeNamedTriMesh("Head", 4u));
    Vector<Mesh> slot;
    slot.push_back(MakeNamedTriMesh("Torso", 3u));
    slot.push_back(MakeNamedTriMesh("Head", 3u));

    EXPECT_EQ(AppendSlotLevelByName(target, slot, 1u, Vector<uint8>{}, "test"), 2u);
    ASSERT_EQ(target[0].ExtraLODVertices.size(), 1u);
    EXPECT_EQ(target[0].ExtraLODVertices[0].size(), 3u);
    EXPECT_TRUE(target[0].HasAuthoredLODs());
    EXPECT_TRUE(target[1].HasAuthoredLODs());
}

TEST(AuthoredLodImport, AppendSlotLevelFoldedNameMatch) {
    Vector<Mesh> target;
    target.push_back(MakeNamedTriMesh("Torso", 4u));
    target.push_back(MakeNamedTriMesh("Head", 4u));
    Vector<Mesh> slot;
    slot.push_back(MakeNamedTriMesh("Torso_LOD1", 3u)); // folds to "Torso"
    slot.push_back(MakeNamedTriMesh("Head_LOD1", 3u));

    EXPECT_EQ(AppendSlotLevelByName(target, slot, 1u, Vector<uint8>{}, "test"), 2u);
    EXPECT_TRUE(target[0].HasAuthoredLODs());
    EXPECT_TRUE(target[1].HasAuthoredLODs());
}

TEST(AuthoredLodImport, AppendSlotLevelMisalignedNameSkips) {
    Vector<Mesh> target;
    target.push_back(MakeNamedTriMesh("Torso", 4u));
    target.push_back(MakeNamedTriMesh("Head", 4u));
    Vector<Mesh> slot;
    slot.push_back(MakeNamedTriMesh("Leg", 3u));
    slot.push_back(MakeNamedTriMesh("Arm", 3u));

    EXPECT_EQ(AppendSlotLevelByName(target, slot, 1u, Vector<uint8>{}, "test"), 0u);
    EXPECT_FALSE(target[0].HasAuthoredLODs());
    EXPECT_FALSE(target[1].HasAuthoredLODs());
}

TEST(AuthoredLodImport, AppendSlotLevelRefusesOptionalStreams) {
    Vector<Mesh> target;
    Mesh t = MakeNamedTriMesh("Torso", 4u);
    t.TexCoords1.assign(t.Vertices.size() * 2u, 0.0f); // optional UV1 present
    target.push_back(std::move(t));
    target.push_back(MakeNamedTriMesh("Head", 4u));
    Vector<Mesh> slot;
    slot.push_back(MakeNamedTriMesh("Torso", 3u));
    slot.push_back(MakeNamedTriMesh("Head", 3u));

    EXPECT_EQ(AppendSlotLevelByName(target, slot, 1u, Vector<uint8>{}, "test"), 1u); // only Head
    EXPECT_FALSE(target[0].HasAuthoredLODs());
    EXPECT_TRUE(target[1].HasAuthoredLODs());
}

// Contiguous multi-level slot chain: slot1 then slot2 build a 2-level chain on a
// pure-slot submesh (not flagged in-file authored). This must keep working after
// the in-file guard — the guard skips in-file chains, not slot-built ones.
TEST(AuthoredLodImport, AppendSlotLevelBuildsMultiLevelPureSlotChain) {
    Vector<Mesh> target;
    target.push_back(MakeNamedTriMesh("Torso", 4u));
    Vector<Mesh> slot1; slot1.push_back(MakeNamedTriMesh("Torso", 3u));
    Vector<Mesh> slot2; slot2.push_back(MakeNamedTriMesh("Torso", 2u));

    EXPECT_EQ(AppendSlotLevelByName(target, slot1, 1u, Vector<uint8>{}, "test"), 1u);
    EXPECT_EQ(AppendSlotLevelByName(target, slot2, 2u, Vector<uint8>{}, "test"), 1u);
    EXPECT_EQ(target[0].ExtraLODs.size(), 2u);       // slot-built 2-level chain
}

// Slots layer UNDER in-file _LOD at EVERY level: a submesh flagged in-file authored
// (here a 2-level in-file chain) is NOT extended by slot3, so it cannot become a
// mixed-provenance chain. inFileAuthored[i] drives the skip (live HasAuthoredLODs()
// cannot tell an in-file chain from a slot-built one).
TEST(AuthoredLodImport, AppendSlotLevelSkipsInFileAuthored) {
    Vector<Mesh> target;
    target.push_back(MakeNamedTriMesh("Torso", 4u));
    target[0].ExtraLODVertices.push_back(Vector<Vertex>(3)); // 2-level in-file chain
    target[0].ExtraLODVertices.push_back(Vector<Vertex>(3));
    target[0].ExtraLODs.push_back({0u, 1u, 2u});
    target[0].ExtraLODs.push_back({0u, 1u, 2u});
    Vector<Mesh> slot;
    slot.push_back(MakeNamedTriMesh("Torso", 3u));
    const Vector<uint8> inFile = {1u}; // submesh 0 authored at parse

    // slot3 (level 3) would satisfy the raw contiguity guard (size 2 == levelIdx 2);
    // the in-file flag must veto it so the chain is not extended.
    EXPECT_EQ(AppendSlotLevelByName(target, slot, 3u, inFile, "test"), 0u);
    EXPECT_EQ(target[0].ExtraLODs.size(), 2u);       // unchanged, no mixed provenance
}

// The slot source-hash fold must change when a slot's content changes or a slot
// is added/removed — the identity that gates the cooked-LOD cache key.
TEST(AuthoredLodImport, FoldSlotSourceReactsToContentAndRef) {
    const GUID a = GUID::Generate();
    const GUID b = GUID::Generate();
    const uint64 base = FoldLodSlotSource(0u, a, 0x1111u);
    EXPECT_NE(base, FoldLodSlotSource(0u, a, 0x2222u)); // content edit
    EXPECT_NE(base, FoldLodSlotSource(0u, b, 0x1111u)); // different ref
    EXPECT_EQ(base, FoldLodSlotSource(0u, a, 0x1111u)); // deterministic
    // Adding a second slot changes the running fold (slot removal -> reverts).
    EXPECT_NE(base, FoldLodSlotSource(base, b, 0x3333u));
}

// The real file-based fixture: a glTF whose three meshes (Cube / Cube_LOD1 /
// Cube_LOD2) collapse into one authored submesh named "Cube" at parse.
TEST(AuthoredLodImport, LoadsGltfLodSuffixFixture) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/authored_lod_cube.gltf";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    ASSERT_EQ(model.GetMeshCount(), 1u);          // LOD1/LOD2 consumed, not standalone

    const Mesh& cube = model.GetMesh(0);
    EXPECT_EQ(cube.Name, "Cube");
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(cube.HasAuthoredLODs());
    EXPECT_EQ(cube.Vertices.size(), 4u);          // LOD0 quad
    EXPECT_EQ(cube.ExtraLODVertices[0].size(), 3u); // LOD1 triangle
    EXPECT_EQ(cube.ExtraLODVertices[1].size(), 3u); // LOD2 triangle
    for (const auto& lod : cube.ExtraLODs)
        for (uint32 idx : lod)
            EXPECT_LT(idx, 3u);                    // LOD-local indices
}

// --- Phase C2b: glTF MSFT_lod --------------------------------------------

namespace {

// A named triangle-list submesh tagged with the source node it came from and a
// material, for MSFT_lod assembly (which aligns lower-detail nodes' submeshes to
// LOD0's by MaterialIndex). No optional streams.
Mesh MakeMsftSubmesh(const std::string& name, int32 sourceNode, uint32 material,
                     uint32 nVerts = 3u) {
    Mesh mesh = MakeNamedTriMesh(name, nVerts);
    mesh.SourceNodeIndex = sourceNode;
    mesh.MaterialIndex = material;
    return mesh;
}

// A 3-node MSFT group: LOD0 = node 0, lower nodes = {1, 2}.
MsftLodGroup MakeCubeGroup(Vector<float> coverage) {
    MsftLodGroup group;
    group.Lod0Node = 0;
    group.LowerNodes = {1, 2};
    group.Coverage = std::move(coverage);
    return group;
}

} // namespace

TEST(MsftLodBlobParse, WellFormedIds) {
    Vector<int32> ids;
    EXPECT_TRUE(ParseMsftLodIds(R"({"ids":[3,5,7]})", ids));
    EXPECT_EQ(ids, (Vector<int32>{3, 5, 7}));
}

TEST(MsftLodBlobParse, MalformedJsonReturnsFalse) {
    Vector<int32> ids{99};
    EXPECT_FALSE(ParseMsftLodIds(R"({"ids":[3,5)", ids)); // truncated
    EXPECT_EQ(ids, (Vector<int32>{99}));                  // outIds left untouched
}

TEST(MsftLodBlobParse, MissingOrEmptyIdsReturnsFalse) {
    Vector<int32> ids;
    EXPECT_FALSE(ParseMsftLodIds(R"({"foo":1})", ids));   // no "ids"
    EXPECT_FALSE(ParseMsftLodIds(R"({"ids":[]})", ids));  // empty array
    EXPECT_FALSE(ParseMsftLodIds(R"({"ids":5})", ids));   // not an array
}

TEST(MsftLodBlobParse, NonIntegerIdReturnsFalse) {
    Vector<int32> ids;
    EXPECT_FALSE(ParseMsftLodIds(R"({"ids":[1,"x"]})", ids));
    EXPECT_FALSE(ParseMsftLodIds(R"({"ids":[1,2.5]})", ids));
}

TEST(MsftLodBlobParse, ScreenCoverageWellFormed) {
    Vector<float> cov;
    EXPECT_TRUE(ParseMsftScreenCoverage(R"({"MSFT_screencoverage":[0.5,0.25,0.01]})", cov));
    ASSERT_EQ(cov.size(), 3u);
    EXPECT_NEAR(cov[0], 0.5f, 1e-6f);
    EXPECT_NEAR(cov[2], 0.01f, 1e-6f);
}

TEST(MsftLodBlobParse, ScreenCoverageAbsentOrMalformed) {
    Vector<float> cov;
    EXPECT_FALSE(ParseMsftScreenCoverage(R"({"other":1})", cov));
    EXPECT_FALSE(ParseMsftScreenCoverage(R"({"MSFT_screencoverage":)", cov));
    EXPECT_FALSE(ParseMsftScreenCoverage(R"({"MSFT_screencoverage":["x"]})", cov));
}

TEST(MsftLodAssembly, BasicChainWithCoverage) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse2", 2, 0, 3u));

    const uint32 consumed =
        AssembleMsftLodChains(meshes, {MakeCubeGroup({0.5f, 0.25f, 0.01f})}, "test");

    EXPECT_EQ(consumed, 2u);
    ASSERT_EQ(meshes.size(), 1u);
    const Mesh& cube = meshes[0];
    EXPECT_EQ(cube.Name, "Cube");
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);
    EXPECT_EQ(cube.ExtraLODVertices[0].size(), 3u);
    EXPECT_TRUE(cube.HasAuthoredLODs());
    // Coverage trims the trailing cull entry (0.01): ExtraLODCoverage is parallel
    // to the two authored levels (amendment #13: coverage[0] => LOD0->LOD1).
    ASSERT_EQ(cube.ExtraLODCoverage.size(), 2u);
    EXPECT_NEAR(cube.ExtraLODCoverage[0], 0.5f, 1e-6f);
    EXPECT_NEAR(cube.ExtraLODCoverage[1], 0.25f, 1e-6f);
}

TEST(MsftLodAssembly, NoCoverageUsesDefaultTable) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse2", 2, 0, 3u));

    AssembleMsftLodChains(meshes, {MakeCubeGroup({})}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    const Mesh& cube = meshes[0];
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(cube.ExtraLODCoverage.empty());     // no coverage -> default table
    ASSERT_EQ(cube.ExtraLODSloppy.size(), 2u);      // sloppy flags route to defaults
    EXPECT_EQ(cube.ExtraLODSloppy[0], 1u);
}

TEST(MsftLodAssembly, WrongLengthCoverageFallsBackToDefault) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse2", 2, 0, 3u));

    // Two lower nodes => usable coverage length is 2 or 3; 5 is neither.
    AssembleMsftLodChains(meshes, {MakeCubeGroup({0.9f, 0.7f, 0.5f, 0.3f, 0.1f})}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_TRUE(meshes[0].ExtraLODCoverage.empty()); // wrong length -> default table
    EXPECT_TRUE(meshes[0].HasAuthoredLODs());        // chain still assembled
}

TEST(MsftLodAssembly, CoverageOnePerSwitchAccepted) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse2", 2, 0, 3u));

    // Length == lower node count (2), the one-per-switch form (no cull entry).
    AssembleMsftLodChains(meshes, {MakeCubeGroup({0.6f, 0.3f})}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    ASSERT_EQ(meshes[0].ExtraLODCoverage.size(), 2u);
    EXPECT_NEAR(meshes[0].ExtraLODCoverage[0], 0.6f, 1e-6f);
    EXPECT_NEAR(meshes[0].ExtraLODCoverage[1], 0.3f, 1e-6f);
}

TEST(MsftLodAssembly, AlignsMultiPrimitiveByMaterial) {
    Vector<Mesh> meshes;
    // LOD0 node 0: two submeshes, materials 7 and 9.
    meshes.push_back(MakeMsftSubmesh("Body", 0, 7, 5u));
    meshes.push_back(MakeMsftSubmesh("Trim", 0, 9, 4u));
    // LOD1 node 1: same materials, REVERSED order (aligns by material, not order).
    meshes.push_back(MakeMsftSubmesh("Trim_c", 1, 9, 3u));
    meshes.push_back(MakeMsftSubmesh("Body_c", 1, 7, 3u));

    MsftLodGroup group;
    group.Lod0Node = 0;
    group.LowerNodes = {1};
    const uint32 consumed = AssembleMsftLodChains(meshes, {group}, "test");

    EXPECT_EQ(consumed, 2u);
    ASSERT_EQ(meshes.size(), 2u);
    // Body (mat 7, 5 verts) aligned to Body_c (mat 7, 3 verts).
    const Mesh* body = meshes[0].MaterialIndex == 7u ? &meshes[0] : &meshes[1];
    const Mesh* trim = meshes[0].MaterialIndex == 9u ? &meshes[0] : &meshes[1];
    ASSERT_EQ(body->ExtraLODVertices.size(), 1u);
    EXPECT_EQ(body->ExtraLODVertices[0].size(), 3u);
    ASSERT_EQ(trim->ExtraLODVertices.size(), 1u);
    EXPECT_EQ(trim->ExtraLODVertices[0].size(), 3u);
}

TEST(MsftLodAssembly, MaterialMisalignmentSkipsSubmeshLevel) {
    Vector<Mesh> meshes;
    // LOD0 node 0: materials 1 and 2.
    meshes.push_back(MakeMsftSubmesh("Keep", 0, 1, 5u));
    meshes.push_back(MakeMsftSubmesh("Drop", 0, 2, 5u));
    // LOD1 node 1: only material 1 (the artist deleted material 2 at LOD1).
    meshes.push_back(MakeMsftSubmesh("Keep_c", 1, 1, 3u));

    MsftLodGroup group;
    group.Lod0Node = 0;
    group.LowerNodes = {1};
    AssembleMsftLodChains(meshes, {group}, "test");

    ASSERT_EQ(meshes.size(), 2u);                   // LOD1 submesh consumed, not standalone
    const Mesh* keep = meshes[0].MaterialIndex == 1u ? &meshes[0] : &meshes[1];
    const Mesh* drop = meshes[0].MaterialIndex == 2u ? &meshes[0] : &meshes[1];
    EXPECT_TRUE(keep->HasAuthoredLODs());           // aligned
    EXPECT_FALSE(drop->HasAuthoredLODs());          // no LOD1 match -> LOD0-only
}

TEST(MsftLodAssembly, LowerNodesNeverStandalone) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse2", 2, 0, 3u));

    AssembleMsftLodChains(meshes, {MakeCubeGroup({0.5f, 0.25f, 0.01f})}, "test");

    for (const Mesh& m : meshes) {
        EXPECT_NE(m.SourceNodeIndex, 1);            // no LOD1 node submesh survives
        EXPECT_NE(m.SourceNodeIndex, 2);
    }
}

TEST(MsftLodAssembly, OutOfRangeNodeIdSkipsLevel) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Cube", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));

    // Group references node 2 and 99, but only node 1 exists as a lower submesh.
    MsftLodGroup group;
    group.Lod0Node = 0;
    group.LowerNodes = {1, 99};
    AssembleMsftLodChains(meshes, {group}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    // Node 1 aligned (LOD1); node 99 has no submesh so the chain stops at LOD1.
    ASSERT_EQ(meshes[0].ExtraLODVertices.size(), 1u);
}

TEST(MsftLodAssembly, OptionalStreamsRefusedButLowersRemoved) {
    Vector<Mesh> meshes;
    Mesh base = MakeMsftSubmesh("Cube", 0, 0, 4u);
    base.Color0.assign(base.Vertices.size() * 4u, 1.0f); // optional stream on LOD0
    meshes.push_back(std::move(base));
    meshes.push_back(MakeMsftSubmesh("CubeCoarse1", 1, 0, 3u));

    AssembleMsftLodChains(meshes, {[]{ MsftLodGroup g; g.Lod0Node = 0; g.LowerNodes = {1}; return g; }()},
                          "test");

    ASSERT_EQ(meshes.size(), 1u);                   // LOD1 node submesh still removed
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());      // chain refused -> LOD0-only
}

// coverage[0] must land on threshold[0] (the LOD0->LOD1 switch) after the direct
// path, and non-monotonic authored coverages are pulled strictly descending.
TEST(ApplyAuthoredCoverage, Lod0ToLod1AndDescendingClamp) {
    float thr[4] = {0, 0, 0, 0};
    const float cov[2] = {0.5f, 0.25f};
    Rendering::ApplyAuthoredLODCoverage(cov, 2u, /*lodCount=*/3u, 4u, thr);
    EXPECT_NEAR(thr[0], 0.5f, 1e-6f);               // coverage[0] => LOD0->LOD1
    EXPECT_NEAR(thr[1], 0.25f, 1e-6f);
    EXPECT_NEAR(thr[2], 0.0f, 1e-6f);               // coarsest present slot matches

    float thr2[4] = {0, 0, 0, 0};
    const float nonMono[2] = {0.2f, 0.5f};          // ascending -> must clamp down
    Rendering::ApplyAuthoredLODCoverage(nonMono, 2u, 3u, 4u, thr2);
    EXPECT_GT(thr2[0], thr2[1]);                    // strictly descending after clamp
}

TEST(MsftLodImport, LoadsFixtureWithCoverage) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/msft_lod_cube.gltf";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    ASSERT_EQ(model.GetMeshCount(), 1u);            // lower LOD nodes consumed

    const Mesh& cube = model.GetMesh(0);
    EXPECT_EQ(cube.Name, "Cube");
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);
    EXPECT_EQ(cube.Vertices.size(), 4u);
    EXPECT_EQ(cube.ExtraLODVertices[0].size(), 3u);
    EXPECT_EQ(cube.ExtraLODVertices[1].size(), 3u);
    ASSERT_EQ(cube.ExtraLODCoverage.size(), 2u);    // authored coverage carried
    EXPECT_NEAR(cube.ExtraLODCoverage[0], 0.5f, 1e-6f);
    EXPECT_NEAR(cube.ExtraLODCoverage[1], 0.25f, 1e-6f);
    for (const auto& lod : cube.ExtraLODs)
        for (uint32 idx : lod)
            EXPECT_LT(idx, 3u);                     // LOD-local indices
}

TEST(MsftLodImport, LoadsFixtureNoCoverageUsesDefault) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/msft_lod_cube_nocoverage.gltf";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    ASSERT_EQ(model.GetMeshCount(), 1u);

    const Mesh& cube = model.GetMesh(0);
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(cube.HasAuthoredLODs());
    EXPECT_TRUE(cube.ExtraLODCoverage.empty());     // no extras -> default table
}

// Dual-tagged content (MSFT_lod + _LOD names): MSFT_lod wins. The lower LOD nodes
// are consumed by the MSFT pass before ConsumeLodSuffixFamilies runs, the base
// name is stripped, and the authored coverage proves the chain came from the MSFT
// path (not the _LOD default-table path). Verifies source priority + no double-
// application (authored-wins).
TEST(MsftLodImport, PriorityOverLodSuffixWhenDualTagged) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/msft_lod_cube_dualtag.gltf";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    ASSERT_EQ(model.GetMeshCount(), 1u);            // exactly one submesh, not 3

    const Mesh& cube = model.GetMesh(0);
    EXPECT_EQ(cube.Name, "Cube");                   // _LOD0 stripped to base
    ASSERT_EQ(cube.ExtraLODVertices.size(), 2u);    // MSFT chain, not double-consumed
    ASSERT_EQ(cube.ExtraLODCoverage.size(), 2u);    // MSFT coverage, not _LOD default
    EXPECT_NEAR(cube.ExtraLODCoverage[0], 0.5f, 1e-6f);
}

// --- Phase C2c: FBX LOD groups -----------------------------------------------

namespace {

FbxLodGroup MakeFbxGroup(int32 lod0, Vector<int32> lowers, Vector<float> distances,
                         bool relative, float unitScale = 1.0f) {
    FbxLodGroup group;
    group.Lod0Node = lod0;
    group.LowerNodes = std::move(lowers);
    group.SwitchDistances = std::move(distances);
    group.RelativeDistances = relative;
    group.UnitScale = unitScale;
    return group;
}

const Mesh* FindMeshByName(const ModelAsset& model, std::string_view name) {
    for (uint32 i = 0; i < model.GetMeshCount(); ++i)
        if (model.GetMesh(i).Name == name)
            return &model.GetMesh(i);
    return nullptr;
}

} // namespace

TEST(FbxLodCoverage, RelativeIsPercentageOverHundred) {
    EXPECT_NEAR(FbxLodSwitchCoverage(64.0f, /*relative=*/true, 0.0f, 0.0f), 0.64f, 1e-6f);
    EXPECT_NEAR(FbxLodSwitchCoverage(100.0f, true, 1.0f, 1.0f), 1.0f, 1e-6f);
    EXPECT_LT(FbxLodSwitchCoverage(0.0f, true, 1.0f, 1.0f), 0.0f);    // degenerate
    EXPECT_LT(FbxLodSwitchCoverage(150.0f, true, 1.0f, 1.0f), 0.0f);  // out of [0,100]
}

TEST(FbxLodCoverage, WorldDistanceMatchesRuntimeMetric) {
    const float projScaleY = 1.0f / std::tan(kLodRefVerticalFovDegrees * 0.5f *
                                             std::numbers::pi_v<float> / 180.0f);
    const float radius = 2.0f, unitScale = 0.5f, dist = 8.0f;
    const float expected = radius * projScaleY / (dist * unitScale);
    EXPECT_NEAR(FbxLodSwitchCoverage(dist, /*relative=*/false, radius, unitScale),
                expected, 1e-4f);
    // Inversely proportional to distance (a nearer switch -> larger coverage).
    EXPECT_GT(FbxLodSwitchCoverage(4.0f, false, radius, unitScale),
              FbxLodSwitchCoverage(8.0f, false, radius, unitScale));
    EXPECT_LT(FbxLodSwitchCoverage(-1.0f, false, radius, unitScale), 0.0f); // degenerate
}

TEST(FbxLodGroupAssembly, RelativeChainWithCoverage) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("pPlatonic1", 0, 0, 4u));  // LOD0 = node 0
    meshes.push_back(MakeMsftSubmesh("pPlatonic1", 1, 0, 3u));  // LOD1 = node 1
    meshes.push_back(MakeMsftSubmesh("pPlatonic1", 2, 0, 3u));  // LOD2 = node 2

    const uint32 consumed = AssembleFbxLodGroupChains(
        meshes, {MakeFbxGroup(0, {1, 2}, {64.0f, 32.0f}, /*relative=*/true)}, "test");

    EXPECT_EQ(consumed, 2u);
    ASSERT_EQ(meshes.size(), 1u);
    const Mesh& m = meshes[0];
    EXPECT_EQ(m.Name, "pPlatonic1");
    ASSERT_EQ(m.ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(m.HasAuthoredLODs());
    ASSERT_EQ(m.ExtraLODCoverage.size(), 2u);
    EXPECT_NEAR(m.ExtraLODCoverage[0], 0.64f, 1e-6f);  // 64% screen size -> 0.64
    EXPECT_NEAR(m.ExtraLODCoverage[1], 0.32f, 1e-6f);
}

TEST(FbxLodGroupAssembly, WorldDistanceCoverageDescending) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Sphere", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("Cube", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("Cone", 2, 0, 3u));

    // Ascending world switch distances -> descending coverage.
    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1, 2}, {4.0f, 8.0f}, /*relative=*/false)},
                              "test");

    ASSERT_EQ(meshes.size(), 1u);
    const Mesh& m = meshes[0];
    ASSERT_EQ(m.ExtraLODCoverage.size(), 2u);
    EXPECT_GT(m.ExtraLODCoverage[0], m.ExtraLODCoverage[1]);
    EXPECT_GT(m.ExtraLODCoverage[1], 0.0f);
}

TEST(FbxLodGroupAssembly, ExpandedLowerBoundsKeepSourceSwitchDistance) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Sphere", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("Cube", 1, 0, 3u));
    const float referenceRadius = ResolveMeshLODGeometry(meshes[0]).ReferenceBounds.Radius();
    meshes[1].Vertices[0].Position[0] = 9.0f;
    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {8.0f}, false)}, "bounds");
    ASSERT_EQ(meshes.size(), 1u);
    const auto geometry = ResolveMeshLODGeometry(meshes[0]);
    ASSERT_EQ(geometry.LevelCount, 2u);
    EXPECT_GT(geometry.Bounds.Radius(), referenceRadius);
    EXPECT_FLOAT_EQ(geometry.ReferenceBounds.Radius(), referenceRadius);
    ASSERT_EQ(meshes[0].ExtraLODCoverage.size(), 1u);
    EXPECT_FLOAT_EQ(meshes[0].ExtraLODCoverage[0], FbxLodSwitchCoverage(8.0f, false, referenceRadius, 1.0f));
    // Direct regeneration is also used by the Editor. Authored geometry wins,
    // but the model aggregate must still be rebuilt from its actual envelope.
    ModelAsset model(GUID::Generate(), "bounds.fbx");
    model.SetMeshesForTest(meshes);
    model.GenerateLODs(MeshLODConfig{}, false);
    float minimum[3], maximum[3]; model.GetBoundingBox(minimum, maximum);
    EXPECT_FLOAT_EQ(maximum[0], 9.0f);
    EXPECT_FLOAT_EQ(minimum[0], -9.0f);
}

TEST(FbxLodGroupAssembly, DegenerateDistanceFallsBackToDefaultTable) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("A", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("A", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("A", 2, 0, 3u));

    // Binary-export case: switch thresholds dropped to 0.
    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1, 2}, {0.0f, 0.0f}, /*relative=*/true)},
                              "test");

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_TRUE(meshes[0].HasAuthoredLODs());         // chain still assembled
    EXPECT_TRUE(meshes[0].ExtraLODCoverage.empty());  // degenerate -> default table
    ASSERT_EQ(meshes[0].ExtraLODSloppy.size(), 2u);   // sloppy flags route to defaults
    EXPECT_EQ(meshes[0].ExtraLODSloppy[0], 1u);
}

TEST(FbxLodGroupAssembly, LowerNodesNeverStandalone) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Base", 0, 0, 4u));
    meshes.push_back(MakeMsftSubmesh("Base", 1, 0, 3u));
    meshes.push_back(MakeMsftSubmesh("Base", 2, 0, 3u));

    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1, 2}, {64.0f, 32.0f}, true)}, "test");

    for (const Mesh& m : meshes) {
        EXPECT_NE(m.SourceNodeIndex, 1);  // no LOD1 node submesh survives
        EXPECT_NE(m.SourceNodeIndex, 2);
    }
}

TEST(FbxLodGroupAssembly, MaterialMisalignmentLod0Only) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Keep", 0, 1, 5u));   // LOD0 material 1
    meshes.push_back(MakeMsftSubmesh("Drop", 0, 2, 5u));   // LOD0 material 2
    meshes.push_back(MakeMsftSubmesh("Keep_c", 1, 1, 3u)); // LOD1 has only material 1

    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {64.0f}, true)}, "test");

    ASSERT_EQ(meshes.size(), 2u);  // LOD1 submesh consumed, not standalone
    const Mesh* keep = meshes[0].MaterialIndex == 1u ? &meshes[0] : &meshes[1];
    const Mesh* drop = meshes[0].MaterialIndex == 2u ? &meshes[0] : &meshes[1];
    EXPECT_TRUE(keep->HasAuthoredLODs());   // material-aligned
    EXPECT_FALSE(drop->HasAuthoredLODs());  // no LOD1 match -> LOD0-only
}

TEST(FbxLodGroupAssembly, MultiMaterialAlignsByMaterial) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Body", 0, 7, 5u));
    meshes.push_back(MakeMsftSubmesh("Trim", 0, 9, 4u));
    meshes.push_back(MakeMsftSubmesh("Trim_c", 1, 9, 3u));  // reversed order
    meshes.push_back(MakeMsftSubmesh("Body_c", 1, 7, 3u));

    const uint32 consumed =
        AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {64.0f}, true)}, "test");

    EXPECT_EQ(consumed, 2u);
    ASSERT_EQ(meshes.size(), 2u);
    const Mesh* body = meshes[0].MaterialIndex == 7u ? &meshes[0] : &meshes[1];
    ASSERT_EQ(body->ExtraLODVertices.size(), 1u);
    EXPECT_EQ(body->ExtraLODVertices[0].size(), 3u);  // aligned to Body_c by material
}

TEST(FbxLodGroupAssembly, OptionalStreamsRefusedLowersRemoved) {
    Vector<Mesh> meshes;
    Mesh base = MakeMsftSubmesh("Base", 0, 0, 4u);
    base.Color0.assign(base.Vertices.size() * 4u, 1.0f);  // optional stream on LOD0
    meshes.push_back(std::move(base));
    meshes.push_back(MakeMsftSubmesh("Base", 1, 0, 3u));

    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {64.0f}, true)}, "test");

    ASSERT_EQ(meshes.size(), 1u);               // LOD1 node submesh still removed
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());  // chain refused -> LOD0-only
}

TEST(FbxLodGroupAssembly, NonTriangleTopologyRefusedLowersRemoved) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Base", 0, 0, 4u));
    Mesh lower = MakeMsftSubmesh("Base", 1, 0, 3u);
    lower.PrimitiveTopology = MeshPrimitiveTopology::Points;
    meshes.push_back(std::move(lower));

    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {64.0f}, true)}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());   // non-triangle LOD refused
}

TEST(FbxLodGroupAssembly, EmptyLowerNodeLod0Only) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Base", 0, 0, 4u));

    // Lower node 5 owns no submesh (malformed / empty level).
    const uint32 consumed =
        AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {5}, {64.0f}, true)}, "test");

    EXPECT_EQ(consumed, 0u);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_FALSE(meshes[0].HasAuthoredLODs());
}

TEST(FbxLodGroupAssembly, StripsLodSuffixFromExposedName) {
    Vector<Mesh> meshes;
    meshes.push_back(MakeMsftSubmesh("Rock_LOD0", 0, 0, 4u));  // dual-tagged names
    meshes.push_back(MakeMsftSubmesh("Rock_LOD1", 1, 0, 3u));

    AssembleFbxLodGroupChains(meshes, {MakeFbxGroup(0, {1}, {64.0f}, true)}, "test");

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].Name, "Rock");  // _LOD0 stripped so the bare base resolves
}

TEST(FbxLodGroupImport, LoadsAsciiFixture) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/maya_lod_group_7500_ascii.fbx";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    // Two groups -> two surviving LOD0 submeshes; every lower level consumed.
    ASSERT_EQ(model.GetMeshCount(), 2u);
    EXPECT_EQ(FindMeshByName(model, "pCube1"), nullptr);  // group2 LOD1 not standalone
    EXPECT_EQ(FindMeshByName(model, "pCone1"), nullptr);  // group2 LOD2 not standalone

    // Group 1: relative_distances (screen-%) -> coverage used directly.
    const Mesh* platonic = FindMeshByName(model, "pPlatonic1");
    ASSERT_NE(platonic, nullptr);
    ASSERT_EQ(platonic->ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(platonic->HasAuthoredLODs());
    ASSERT_EQ(platonic->ExtraLODCoverage.size(), 2u);
    EXPECT_NEAR(platonic->ExtraLODCoverage[0], 0.64f, 1e-4f);  // Thresholds|Level0 = 64%
    EXPECT_NEAR(platonic->ExtraLODCoverage[1], 0.32f, 1e-4f);  // Thresholds|Level1 = 32%
    for (size_t k = 0; k < platonic->ExtraLODs.size(); ++k)
        for (uint32 idx : platonic->ExtraLODs[k])
            EXPECT_LT(idx, platonic->ExtraLODVertices[k].size());  // LOD-local indices

    // Group 2: world-distance thresholds -> approximate coverage, still descending.
    const Mesh* sphere = FindMeshByName(model, "pSphere1");
    ASSERT_NE(sphere, nullptr);
    ASSERT_EQ(sphere->ExtraLODVertices.size(), 2u);
    EXPECT_TRUE(sphere->HasAuthoredLODs());
    ASSERT_EQ(sphere->ExtraLODCoverage.size(), 2u);
    EXPECT_GT(sphere->ExtraLODCoverage[0], sphere->ExtraLODCoverage[1]);
    EXPECT_GT(sphere->ExtraLODCoverage[1], 0.0f);
}

TEST(FbxLodGroupImport, LoadsBinaryFixtureDefaultTable) {
    const std::filesystem::path fixture =
        GameEngine::TestPaths::StagedRoot() / "Engine/Tests/Fixtures/maya_lod_group_6100_binary.fbx";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture missing: " << fixture.string();

    ModelAsset model(GUID::Generate(), fixture);
    ASSERT_TRUE(model.Load());
    // Two groups -> two surviving LOD0 submeshes; every lower level consumed. The
    // binary export names geometry per-level uniquely (probe: pPlatonic1_ncl1_*),
    // so assert by structure, not name.
    ASSERT_EQ(model.GetMeshCount(), 2u);
    for (uint32 i = 0; i < model.GetMeshCount(); ++i) {
        const Mesh& m = model.GetMesh(i);
        ASSERT_EQ(m.ExtraLODVertices.size(), 2u) << "submesh '" << m.Name << "'";
        EXPECT_TRUE(m.HasAuthoredLODs()) << m.Name;
        // The binary export dropped its threshold property values (probe: 0/0), so
        // the chain assembles but coverage is degenerate -> default descending table.
        EXPECT_TRUE(m.ExtraLODCoverage.empty()) << m.Name;
        ASSERT_EQ(m.ExtraLODSloppy.size(), 2u) << m.Name;
        EXPECT_EQ(m.ExtraLODSloppy[0], 1u) << m.Name;
        for (size_t k = 0; k < m.ExtraLODs.size(); ++k)
            for (uint32 idx : m.ExtraLODs[k])
                EXPECT_LT(idx, m.ExtraLODVertices[k].size());  // LOD-local indices
    }
}

// --- Decode cancellation ---------------------------------------------------
//
// LOD generation runs inside ProcessAssetData, so it sees the load's cancel flag
// through the same thread-scoped seam the texture cook uses. One
// meshopt_simplify* call is opaque, so the poll sits between levels (and between
// submeshes): a cancelled generation returns a SHORT chain, never a wrong one.

TEST(MeshLODGeneratorCancel, FlagSetBeforeStartProducesNoExtraLevels) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";

    const Mesh mesh = MakeGridPlane(64);
    const MeshLODConfig cfg;

    // Control: this mesh really does simplify past LOD0, so the assertion below
    // is about the cancel and not about an ungeneratable input.
    const MeshLODs control = GenerateMeshLODs(mesh, cfg);
    ASSERT_GT(control.LodCount(), 1u) << "fixture must generate levels for this test to mean anything";

    auto cancel = std::make_shared<std::atomic<bool>>(true);
    ScopedAssetDecodeCancellation ctx(cancel);
    const MeshLODs cancelled = GenerateMeshLODs(mesh, cfg);
    EXPECT_EQ(cancelled.LodCount(), 1u) << "a cancelled generation must stop at LOD0";
}

// A published-but-unset flag must not change what the generator produces — the
// poll is an exit, not a behaviour switch.
TEST(MeshLODGeneratorCancel, UnsetFlagGeneratesIdenticallyToNoContext) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";

    const Mesh mesh = MakeGridPlane(48);
    const MeshLODConfig cfg;
    const MeshLODs noContext = GenerateMeshLODs(mesh, cfg);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ScopedAssetDecodeCancellation ctx(cancel);
    const MeshLODs withFlag = GenerateMeshLODs(mesh, cfg);

    ASSERT_EQ(withFlag.LodCount(), noContext.LodCount());
    for (uint32 lod = 0; lod < noContext.LodCount(); ++lod) {
        EXPECT_EQ(withFlag.LodIndices[lod], noContext.LodIndices[lod]) << "lod " << lod;
        EXPECT_EQ(withFlag.LodErrors[lod], noContext.LodErrors[lod]) << "lod " << lod;
        EXPECT_EQ(withFlag.LodSloppy[lod], noContext.LodSloppy[lod]) << "lod " << lod;
        ASSERT_EQ(withFlag.LodVertices[lod].size(), noContext.LodVertices[lod].size())
            << "lod " << lod;
        EXPECT_EQ(std::memcmp(withFlag.LodVertices[lod].data(), noContext.LodVertices[lod].data(),
                              withFlag.LodVertices[lod].size() * sizeof(Vertex)),
                  0)
            << "lod " << lod;
    }
    EXPECT_EQ(withFlag.SloppyShellsRejected, noContext.SloppyShellsRejected);
    EXPECT_EQ(withFlag.SloppyLevelsSkippedForStreams, noContext.SloppyLevelsSkippedForStreams);
    EXPECT_EQ(withFlag.LevelsSkippedWithLocks, noContext.LevelsSkippedWithLocks);
}

// The per-submesh poll: a cancelled model-level generate leaves the meshes at
// LOD0 rather than simplifying every submesh to completion.
TEST(MeshLODGeneratorCancel, ModelGenerateStopsAtCancelledSubmesh) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";

    const MeshLODConfig cfg;
    Vector<Mesh> meshes{MakeGridPlane(48), MakeGridPlane(48)};

    ModelAsset control(GUID{}, "cancel-control.glb");
    control.SetMeshesForTest(meshes);
    ASSERT_GT(control.GenerateLODs(cfg, /*generateSkinned*/ false), 1u);

    ModelAsset model(GUID{}, "cancel-fixture.glb");
    model.SetMeshesForTest(meshes);
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    ScopedAssetDecodeCancellation ctx(cancel);
    EXPECT_EQ(model.GenerateLODs(cfg, /*generateSkinned*/ false), 1u);
    for (uint32 i = 0; i < model.GetMeshCount(); ++i)
        EXPECT_TRUE(model.GetMesh(i).ExtraLODs.empty()) << "submesh " << i;
}

// The tests above only prove the flag is read BEFORE the first level — a poll
// hoisted out of the loop would satisfy them. This one cancels once
// simplification is already under way, which is the case per-level polling
// exists for. The assertion is structural (strictly fewer levels), not a timing
// ratio, so it does not rest on how long any one level took.
TEST(MeshLODGeneratorCancel, PreemptsGenerationInProgress) {
    if (!IsMeshLODGenerationAvailable()) GTEST_SKIP() << "meshoptimizer not compiled in";

    const Mesh mesh = MakeGridPlane(96);
    const MeshLODConfig cfg;

    const auto start = std::chrono::steady_clock::now();
    const MeshLODs control = GenerateMeshLODs(mesh, cfg);
    const double controlMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_GT(control.LodCount(), 1u) << "fixture must generate levels for this test to mean anything";

    // Instrument check: the cancel has to land inside the run, so the run must
    // outlast the delay below by a clear margin. A fast build is not a defect in
    // the poll, so skip rather than fail.
    if (controlMs < 20.0)
        GTEST_SKIP() << "control generation too short to interrupt: " << controlMs << "ms";

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    ScopedAssetDecodeCancellation ctx(cancel);
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64>(controlMs * 250.0)));
        cancel->store(true, std::memory_order_release);
    });
    const MeshLODs cancelled = GenerateMeshLODs(mesh, cfg);
    canceller.join();

    EXPECT_LT(cancelled.LodCount(), control.LodCount())
        << "a cancel arriving mid-generation must cut the chain short (control "
        << control.LodCount() << " levels in " << controlMs << "ms)";
}
