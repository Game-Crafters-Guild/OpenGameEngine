#include "Assets/HlodClustering.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

// A member centered at (cx,cy,cz) with a cube AABB of the given full extent.
// StableId is derived from `tag` so tests can identify members after a permute.
MemberInput MakeMember(uint32 tag, float cx, float cy, float cz, float extent) {
    MemberInput m;
    m.StableId = GUID::Derive(GUID::Null(), "hlod/test/member/" + std::to_string(tag));
    m.MeshContentHash = 0x1000u + tag;
    const float h = extent * 0.5f;
    m.WorldAabbMin[0] = cx - h; m.WorldAabbMin[1] = cy - h; m.WorldAabbMin[2] = cz - h;
    m.WorldAabbMax[0] = cx + h; m.WorldAabbMax[1] = cy + h; m.WorldAabbMax[2] = cz + h;
    m.WorldMatrix[12] = cx; m.WorldMatrix[13] = cy; m.WorldMatrix[14] = cz;
    return m;
}

GridConfig DefaultConfig(float cellSize = 10.0f) {
    GridConfig c;
    c.CellSize = cellSize;
    return c;
}

const Cluster* FindClusterByCell(const ClusterTable& t, CellCoord cell) {
    for (const Cluster& c : t.Clusters)
        if (c.Cell == cell)
            return &c;
    return nullptr;
}

} // namespace

TEST(HlodClustering, SingleMemberFormsOneCluster) {
    MemberInput members[] = {MakeMember(0, 3.0f, 0.0f, 3.0f, 2.0f)};
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 1u);
    EXPECT_TRUE(t.OversizedMembers.empty());
    EXPECT_EQ(t.Clusters[0].MemberIndices.size(), 1u);
    EXPECT_EQ(t.Clusters[0].MemberIndices[0], 0u);
    EXPECT_EQ((t.Clusters[0].Cell), (CellCoord{0, 0, 0}));
}

TEST(HlodClustering, MembersInSameCellCluster) {
    // Both centers within cell (0,0,0) of a 10-unit grid.
    MemberInput members[] = {
        MakeMember(0, 1.0f, 1.0f, 1.0f, 1.0f),
        MakeMember(1, 8.0f, 8.0f, 8.0f, 1.0f),
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 1u);
    EXPECT_EQ(t.Clusters[0].MemberIndices.size(), 2u);
    EXPECT_EQ(t.Clusters[0].MemberIndices[0], 0u);
    EXPECT_EQ(t.Clusters[0].MemberIndices[1], 1u);
}

TEST(HlodClustering, MembersInDifferentCellsSeparate) {
    MemberInput members[] = {
        MakeMember(0, 1.0f, 0.0f, 1.0f, 1.0f),   // cell (0,0,0)
        MakeMember(1, 25.0f, 0.0f, 1.0f, 1.0f),  // cell (2,0,0)
        MakeMember(2, 1.0f, 0.0f, 25.0f, 1.0f),  // cell (0,0,2)
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 3u);
    // Ascending cell-coord order: (0,0,0), (0,0,2), (2,0,0).
    EXPECT_EQ((t.Clusters[0].Cell), (CellCoord{0, 0, 0}));
    EXPECT_EQ((t.Clusters[1].Cell), (CellCoord{0, 0, 2}));
    EXPECT_EQ((t.Clusters[2].Cell), (CellCoord{2, 0, 0}));
}

TEST(HlodClustering, OwnerByAabbCenterNotCorner) {
    // Center at 11 (cell 1), but the AABB straddles the 10-unit boundary
    // (min 9 in cell 0). Single-owner must place it in cell 1 (the center),
    // not cell 0 (a corner).
    MemberInput members[] = {MakeMember(0, 11.0f, 5.0f, 5.0f, 4.0f)};
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 1u);
    EXPECT_EQ((t.Clusters[0].Cell), (CellCoord{1, 0, 0}));
}

TEST(HlodClustering, OversizedMemberExcluded) {
    MemberInput members[] = {
        MakeMember(0, 5.0f, 5.0f, 5.0f, 2.0f),   // fits
        MakeMember(1, 50.0f, 5.0f, 5.0f, 12.0f), // extent 12 > cell 10 → oversized
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 1u);
    EXPECT_EQ(t.Clusters[0].MemberIndices[0], 0u);
    ASSERT_EQ(t.OversizedMembers.size(), 1u);
    EXPECT_EQ(t.OversizedMembers[0], 1u);
}

TEST(HlodClustering, NegativeCoordinatesNoSeamAtOrigin) {
    MemberInput members[] = {
        MakeMember(0, -1.0f, 0.0f, 0.0f, 1.0f),  // cell (-1,0,0)
        MakeMember(1, -11.0f, 0.0f, 0.0f, 1.0f), // cell (-2,0,0)
        MakeMember(2, 1.0f, 0.0f, 0.0f, 1.0f),   // cell (0,0,0)
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 3u);
    EXPECT_EQ((t.Clusters[0].Cell), (CellCoord{-2, 0, 0}));
    EXPECT_EQ((t.Clusters[1].Cell), (CellCoord{-1, 0, 0}));
    EXPECT_EQ((t.Clusters[2].Cell), (CellCoord{0, 0, 0}));
}

TEST(HlodClustering, ClusterBoundsAndSphere) {
    // Two members spanning x [0..10], centered so the union AABB is [0,0,0]..[10,2,2].
    MemberInput members[] = {
        MakeMember(0, 1.0f, 1.0f, 1.0f, 2.0f),  // [0..2]^3
        MakeMember(1, 9.0f, 1.0f, 1.0f, 2.0f),  // [8..10] x, [0..2] y,z
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 1u);
    const Cluster& c = t.Clusters[0];
    EXPECT_FLOAT_EQ(c.BoundsMin[0], 0.0f);
    EXPECT_FLOAT_EQ(c.BoundsMax[0], 10.0f);
    EXPECT_FLOAT_EQ(c.BoundsMin[1], 0.0f);
    EXPECT_FLOAT_EQ(c.BoundsMax[1], 2.0f);
    EXPECT_FLOAT_EQ(c.SphereCenter[0], 5.0f);
    EXPECT_FLOAT_EQ(c.SphereCenter[1], 1.0f);
    // Half-diagonal of a 10x2x2 box: sqrt(5^2 + 1^2 + 1^2).
    EXPECT_NEAR(c.SphereRadius, std::sqrt(25.0f + 1.0f + 1.0f), 1e-4f);
}

TEST(HlodClustering, DeterministicUnderPermutation) {
    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 40; ++i) {
        const float cx = static_cast<float>((i % 7) * 13);
        const float cy = static_cast<float>(((i / 7) % 3) * 13);
        const float cz = static_cast<float>((i * 5) % 40);
        members.push_back(MakeMember(i, cx, cy, cz, 2.0f));
    }

    ClusterTable a = BuildClusters(members, DefaultConfig());

    std::vector<MemberInput> shuffled = members;
    std::mt19937 rng(12345);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    ClusterTable b = BuildClusters(shuffled, DefaultConfig());

    // Same set of occupied cells, same ascending order.
    ASSERT_EQ(a.Clusters.size(), b.Clusters.size());
    for (size_t i = 0; i < a.Clusters.size(); ++i)
        EXPECT_EQ(a.Clusters[i].Cell, b.Clusters[i].Cell);

    // Same member StableId set per cell regardless of input order.
    for (const Cluster& ca : a.Clusters) {
        const Cluster* cb = FindClusterByCell(b, ca.Cell);
        ASSERT_NE(cb, nullptr);
        std::vector<GUID> idsA, idsB;
        for (uint32 mi : ca.MemberIndices) idsA.push_back(members[mi].StableId);
        for (uint32 mi : cb->MemberIndices) idsB.push_back(shuffled[mi].StableId);
        std::sort(idsA.begin(), idsA.end());
        std::sort(idsB.begin(), idsB.end());
        EXPECT_EQ(idsA, idsB);
    }
}

TEST(HlodClustering, EmptyOnNonPositiveCellSize) {
    MemberInput members[] = {MakeMember(0, 1.0f, 1.0f, 1.0f, 1.0f)};
    EXPECT_TRUE(BuildClusters(members, DefaultConfig(0.0f)).Clusters.empty());
    EXPECT_TRUE(BuildClusters(members, DefaultConfig(-5.0f)).Clusters.empty());
}

TEST(HlodClustering, MemberIndicesAscendingWithinCluster) {
    // Interleave two cells so within-cluster member indices are non-contiguous
    // but must still be ascending.
    MemberInput members[] = {
        MakeMember(0, 1.0f, 0.0f, 0.0f, 1.0f),   // cell A
        MakeMember(1, 25.0f, 0.0f, 0.0f, 1.0f),  // cell B
        MakeMember(2, 2.0f, 0.0f, 0.0f, 1.0f),   // cell A
        MakeMember(3, 26.0f, 0.0f, 0.0f, 1.0f),  // cell B
    };
    ClusterTable t = BuildClusters(members, DefaultConfig());
    ASSERT_EQ(t.Clusters.size(), 2u);
    const Cluster* a = FindClusterByCell(t, CellCoord{0, 0, 0});
    const Cluster* b = FindClusterByCell(t, CellCoord{2, 0, 0});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->MemberIndices, (std::vector<uint32>{0u, 2u}));
    EXPECT_EQ(b->MemberIndices, (std::vector<uint32>{1u, 3u}));
}
