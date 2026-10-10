#include "Assets/HlodBenefit.h"
#include "Assets/HlodClustering.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

// A member centered at (cx,*,*) in a fixed small cube, with a chosen mesh
// content hash (mesh identity) and material GUID. VertexCount drives the VB
// estimate.
MemberInput Member(uint32 tag, float cx, uint64 meshHash, const char* material,
                   uint32 vertexCount, bool casts = true) {
    MemberInput m;
    m.StableId = GUID::Derive(GUID::Null(), "hlod/benefit/" + std::to_string(tag));
    m.MeshContentHash = meshHash;
    m.MaterialGuid = GUID::Derive(GUID::Null(), material);
    m.VertexCount = vertexCount;
    m.CastsShadow = casts;
    m.WorldAabbMin[0] = cx - 0.5f; m.WorldAabbMin[1] = -0.5f; m.WorldAabbMin[2] = -0.5f;
    m.WorldAabbMax[0] = cx + 0.5f; m.WorldAabbMax[1] = 0.5f; m.WorldAabbMax[2] = 0.5f;
    return m;
}

GridConfig BenefitConfig() {
    GridConfig c;
    c.CellSize = 1000.0f; // one big cell so all members land together
    c.MaxInstancingRatio = 4.0f;
    c.MinMembers = 8u;
    c.VbBudgetBytes = 1ull << 30;
    return c;
}

} // namespace

TEST(HlodBenefit, HighInstancingUniformCellExcluded) {
    // 20 copies of ONE mesh (r = 20) → well above MaxInstancingRatio 4.
    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 20; ++i)
        members.push_back(Member(i, 1.0f, /*meshHash*/ 0xAAAA, "m/shared", 100));
    ClusterTable t = BuildClusters(members, BenefitConfig());
    std::vector<ClusterBenefit> b;
    EvaluateClusterBenefit(t, members, BenefitConfig(), b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].Disposition, ClusterDisposition::HighInstancing);
    EXPECT_FLOAT_EQ(b[0].InstancingRatio, 20.0f);
}

TEST(HlodBenefit, LowInstancingFarCellAdmitted) {
    // 12 distinct meshes (r = 1) → admitted.
    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 12; ++i)
        members.push_back(Member(i, 1.0f, 0x1000 + i, ("m/" + std::to_string(i)).c_str(), 100));
    ClusterTable t = BuildClusters(members, BenefitConfig());
    std::vector<ClusterBenefit> b;
    EvaluateClusterBenefit(t, members, BenefitConfig(), b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].Disposition, ClusterDisposition::Admitted);
    EXPECT_FLOAT_EQ(b[0].InstancingRatio, 1.0f);
    EXPECT_EQ(b[0].SubmeshCount, 12u); // 12 distinct materials
    EXPECT_EQ(b[0].EstimatedVbBytes, 12ull * 100ull * kHlodCoreVertexStrideBytes);
}

TEST(HlodBenefit, TooFewMembersExcluded) {
    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 4; ++i) // below MinMembers 8
        members.push_back(Member(i, 1.0f, 0x1000 + i, ("m/" + std::to_string(i)).c_str(), 100));
    ClusterTable t = BuildClusters(members, BenefitConfig());
    std::vector<ClusterBenefit> b;
    EvaluateClusterBenefit(t, members, BenefitConfig(), b);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].Disposition, ClusterDisposition::TooFewMembers);
}

TEST(HlodBenefit, BenefitCountsRecordsAndCasters) {
    // 10 distinct meshes, 2 distinct materials (M=2), all cast shadows.
    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 10; ++i)
        members.push_back(Member(i, 1.0f, 0x1000 + i, (i % 2 == 0 ? "m/a" : "m/b"), 50));
    ClusterTable t = BuildClusters(members, BenefitConfig());
    std::vector<ClusterBenefit> b;
    EvaluateClusterBenefit(t, members, BenefitConfig(), b);
    ASSERT_EQ(b[0].Disposition, ClusterDisposition::Admitted);
    EXPECT_EQ(b[0].MemberCount, 10u);
    EXPECT_EQ(b[0].SubmeshCount, 2u);
    EXPECT_EQ(b[0].CastingMembers, 10u);
    // (N - M) + (casting - M) = (10-2) + (10-2) = 16.
    EXPECT_EQ(b[0].Benefit, 16);
}

TEST(HlodBenefit, GreedyAdmissionStopsAtBudget) {
    // Two separate cells, each an admissible low-instancing cluster, but the
    // budget only fits one. cellSize small so the far cell is a distinct cluster.
    GridConfig cfg = BenefitConfig();
    cfg.CellSize = 10.0f;
    // Each cluster: 10 distinct meshes × 100 verts × 48 B = 48000 B.
    const uint64 perCluster = 10ull * 100ull * kHlodCoreVertexStrideBytes;
    cfg.VbBudgetBytes = perCluster + perCluster / 2; // fits exactly one

    std::vector<MemberInput> members;
    for (uint32 i = 0; i < 10; ++i)
        members.push_back(Member(i, 1.0f, 0x1000 + i, ("m/a" + std::to_string(i)).c_str(), 100));
    for (uint32 i = 0; i < 10; ++i)
        members.push_back(Member(100 + i, 50.0f, 0x2000 + i, ("m/b" + std::to_string(i)).c_str(), 100));

    ClusterTable t = BuildClusters(members, cfg);
    ASSERT_EQ(t.Clusters.size(), 2u);
    std::vector<ClusterBenefit> b;
    EvaluateClusterBenefit(t, members, cfg, b);

    uint32 admitted = 0, overBudget = 0;
    for (const ClusterBenefit& cb : b) {
        if (cb.Disposition == ClusterDisposition::Admitted) ++admitted;
        if (cb.Disposition == ClusterDisposition::OverBudget) ++overBudget;
    }
    EXPECT_EQ(admitted, 1u);
    EXPECT_EQ(overBudget, 1u);
}
