// CPU oracle for the cube-sphere base mesh (plan §8 C7). The 24 pie-slice roots must
// form a valid, consistently-wound CLOSED manifold whose neighbor links match the
// geometry — this is the base the GPU refinement builds on, and a wrong face table or
// leg/twin assignment here would put geometry and topology on different meshes (cracks
// across faces the moment refinement crosses an edge). All exact-integer, no device.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>

#include "CBTTerrain/CBTSphereRoots.h"

using namespace GameEngine::CBTTerrain;

namespace
{
uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1;
        ++d;
    }
    return d;
}

using IVec3 = std::array<int32_t, 3>;

// Unordered integer edge key (min corner first) so a shared edge from two triangles
// maps to the same key regardless of traversal direction.
std::array<int32_t, 6> EdgeKey(const IVec3& a, const IVec3& b)
{
    const bool aFirst = a < b;
    const IVec3& lo = aFirst ? a : b;
    const IVec3& hi = aFirst ? b : a;
    return {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
}

bool SameEdge(const IVec3& a0, const IVec3& a1, const IVec3& b0, const IVec3& b1)
{
    return EdgeKey(a0, a1) == EdgeKey(b0, b1);
}
} // namespace

// The 24 roots occupy heapIDs 32..55, all at the uniform base depth 5.
TEST(CBTSphereRoots, HeapIDsUniformDepth)
{
    const auto roots = BuildSphereRoots();
    ASSERT_EQ(roots.size(), 24u);
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        EXPECT_EQ(roots[r].HeapID, (uint64_t(1) << kSphereBaseDepth) + r) << "root " << r;
        EXPECT_EQ(HeapDepth(roots[r].HeapID), kSphereBaseDepth) << "root " << r << " depth";
    }
}

// Closed manifold: every neighbor slot is a valid in-range root (no boundary), and the
// links are reciprocal (each neighbor references the root back in some slot).
TEST(CBTSphereRoots, NeighborsValidAndReciprocal)
{
    const auto roots = BuildSphereRoots();
    auto references = [&](uint32_t from, uint32_t to) {
        const CBTNeighbors& n = roots[from].Neighbors;
        return n.Neighbor0 == to || n.Neighbor1 == to || n.Twin == to;
    };
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        const CBTNeighbors& n = roots[r].Neighbors;
        for (uint32_t nb : {n.Neighbor0, n.Neighbor1, n.Twin})
        {
            ASSERT_NE(nb, kInvalidPointer) << "root " << r << " has a boundary link (not closed)";
            ASSERT_LT(nb, kSphereRootCount) << "root " << r << " neighbor out of range";
            EXPECT_TRUE(references(nb, r))
                << "root " << r << " links neighbor " << nb << " but it does not link back";
        }
    }
}

// The twin is a pie slice on a DIFFERENT face whose split edge is the SAME cube edge —
// so cross-face adjacency rides the twin link (the property that lets the compatibility
// chain conform across faces). The two leg neighbors are same-face siblings.
TEST(CBTSphereRoots, TwinCrossesFaceOnSplitEdge)
{
    const auto roots = BuildSphereRoots();
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        const uint32_t face = r / 4u;
        const CBTNeighbors& n = roots[r].Neighbors;
        // Twin on another face, split edges coincide, and it is mutual.
        EXPECT_NE(n.Twin / 4u, face) << "root " << r << " twin must be on another face";
        EXPECT_TRUE(SameEdge(roots[r].V0, roots[r].V2, roots[n.Twin].V0, roots[n.Twin].V2))
            << "root " << r << " twin does not share the split (cube) edge";
        EXPECT_EQ(roots[n.Twin].Neighbors.Twin, r) << "twin link not mutual for root " << r;
        // Legs are same-face siblings sharing the centroid legs.
        EXPECT_EQ(n.Neighbor0 / 4u, face) << "root " << r << " n0 must be a same-face sibling";
        EXPECT_EQ(n.Neighbor1 / 4u, face) << "root " << r << " n1 must be a same-face sibling";
        EXPECT_TRUE(SameEdge(roots[r].V1, roots[r].V2, roots[n.Neighbor0].V0, roots[n.Neighbor0].V1) ||
                    SameEdge(roots[r].V1, roots[r].V2, roots[n.Neighbor0].V1, roots[n.Neighbor0].V2) ||
                    SameEdge(roots[r].V1, roots[r].V2, roots[n.Neighbor0].V2, roots[n.Neighbor0].V0))
            << "root " << r << " n0 does not share the (v1,v2) leg";
        EXPECT_TRUE(SameEdge(roots[r].V0, roots[r].V1, roots[n.Neighbor1].V0, roots[n.Neighbor1].V1) ||
                    SameEdge(roots[r].V0, roots[r].V1, roots[n.Neighbor1].V1, roots[n.Neighbor1].V2) ||
                    SameEdge(roots[r].V0, roots[r].V1, roots[n.Neighbor1].V2, roots[n.Neighbor1].V0))
            << "root " << r << " n1 does not share the (v0,v1) leg";
    }
}

// Geometry conformity: the 24 root triangles are a closed manifold, so EVERY undirected
// edge is shared by exactly two triangles. A wrong corner or face-table entry breaks this
// (an edge would appear once or thrice). Euler: 14 verts (8 corners + 6 centroids) - E +
// 24 tris = 2 => 36 edges, each shared twice.
TEST(CBTSphereRoots, BaseMeshIsClosedManifold)
{
    const auto roots = BuildSphereRoots();
    std::map<std::array<int32_t, 6>, int> edgeCount;
    for (const CBTSphereRoot& root : roots)
    {
        ++edgeCount[EdgeKey(root.V0, root.V1)];
        ++edgeCount[EdgeKey(root.V1, root.V2)];
        ++edgeCount[EdgeKey(root.V2, root.V0)];
    }
    EXPECT_EQ(edgeCount.size(), 36u) << "expected 36 distinct edges (Euler characteristic)";
    for (const auto& [key, count] : edgeCount)
        EXPECT_EQ(count, 2) << "an edge is not shared by exactly two triangles (not a manifold)";
}

// Consistent winding: every pie slice's triangle normal has the SAME sign relative to
// the outward (planet-centre) direction. A consistent orientation is what the LEB decode
// needs — it preserves orientation, so a uniformly-wound base yields a uniformly-wound
// sphere (no per-triangle backface flip). The pie-slice order (corner, faceCentre,
// nextCorner) is uniformly CW-as-seen-from-outside (normal . outward < 0) — that is the
// consistent orientation; the surface derives + orients the shading normal itself and the
// material is double-sided, so the direction is cosmetic, but consistency is required.
TEST(CBTSphereRoots, WindingConsistent)
{
    const auto roots = BuildSphereRoots();
    int32_t signRef = 0;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        const IVec3& v0 = roots[r].V0;
        const IVec3& v1 = roots[r].V1;
        const IVec3& v2 = roots[r].V2;
        const int32_t e0[3] = {static_cast<int32_t>(v1[0] - v0[0]), static_cast<int32_t>(v1[1] - v0[1]),
                               static_cast<int32_t>(v1[2] - v0[2])};
        const int32_t e1[3] = {static_cast<int32_t>(v2[0] - v0[0]), static_cast<int32_t>(v2[1] - v0[1]),
                               static_cast<int32_t>(v2[2] - v0[2])};
        const int32_t nrm[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                                e0[0] * e1[1] - e0[1] * e1[0]};
        const int32_t dotC = static_cast<int32_t>(nrm[0] * v1[0] + nrm[1] * v1[1] + nrm[2] * v1[2]);
        ASSERT_NE(dotC, 0) << "root " << r << " is degenerate";
        const int32_t sign = dotC > 0 ? 1 : -1;
        if (r == 0)
            signRef = sign;
        EXPECT_EQ(sign, signRef) << "root " << r << " winds opposite the rest (non-uniform)";
    }
}
