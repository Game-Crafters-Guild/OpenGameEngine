// Tests for MeshPoolGroupPlan: geometry-bind identity grouping for opaque draw
// consolidation. Meshes sharing {vertex-flags bucket, stream pools, index pool,
// index type, topology} must share a group id; any bind-relevant difference
// must split; ids must be append-only across Refresh calls (the consumer-vs-
// snapshot stability contract).

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Assets/ModelAsset.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"

#include "TestDeviceHelper.h"

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

class MeshPoolGroupPlanTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(64u, 16u));
        m_Registry.Initialize(m_Device.get());
        m_Registry.SetGPUScene(m_Scene.get());
    }

    void TearDown() override
    {
        m_Registry.Shutdown();
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Minimal triangle mesh; the optional streams control the registry bucket.
    Mesh MakeTriangle(bool withColor, bool wideIndices)
    {
        Mesh m{};
        m.Name = "PlanTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        m.Vertices = {v0, v1, v2};
        m.Indices = {0u, 1u, 2u};
        if (wideIndices)
        {
            // Force a vertex count past 65535 so the upload picks uint32
            // indices (a distinct index pool + index type).
            m.Vertices.resize(70000, v2);
            m.Indices = {0u, 1u, 69999u};
        }
        if (withColor)
            m.Color0.resize(m.Vertices.size() * 4, 1.0f);
        m.MinBounds[0] = -1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
        m.MaxBounds[0] = 1.0f;  m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;
        return m;
    }

    uint32_t RegisterMesh(uint32_t submesh, const Mesh& mesh)
    {
        MeshGPUKey key{};
        key.assetGuid = m_Guid;
        key.submeshIndex = submesh;
        const MeshGPUHandle handle = m_Registry.RegisterSubmesh(key, mesh);
        const MeshGPUEntry* entry = m_Registry.Find(handle);
        EXPECT_NE(entry, nullptr);
        return entry ? entry->gpuMeshIndex : ~0u;
    }

    uint32_t MeshTableCount() const
    {
        return static_cast<uint32_t>(m_Scene->GetMeshes().size());
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
    MeshGPURegistry m_Registry;
    GUID m_Guid = GUID::Generate();
};

TEST_F(MeshPoolGroupPlanTest, SameBucketAndPoolsShareOneGroup)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(false, false));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_EQ(plan.GroupOf(meshA), plan.GroupOf(meshB));
    EXPECT_NE(plan.GroupOf(meshA), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_EQ(plan.LiveGroupCount(), 1u);
}

TEST_F(MeshPoolGroupPlanTest, DifferentVertexFlagBucketsSplitGroups)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_NE(plan.GroupOf(meshA), plan.GroupOf(meshB));
    EXPECT_EQ(plan.LiveGroupCount(), 2u);
}

TEST_F(MeshPoolGroupPlanTest, IndexTypeSplitsGroups)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(false, true));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    // 16-bit vs 32-bit indices bind different index pools — never one draw.
    EXPECT_NE(plan.GroupOf(meshA), plan.GroupOf(meshB));
}

TEST_F(MeshPoolGroupPlanTest, IdsAreStableAndAppendOnlyAcrossRefresh)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    const uint32_t groupA = plan.GroupOf(meshA);

    // A mid-frame registration of a new identity must only APPEND ids.
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_EQ(plan.GroupOf(meshA), groupA);
    EXPECT_NE(plan.GroupOf(meshB), groupA);
    EXPECT_EQ(plan.GroupCount(), 2u);
}

// Renderer-core cannot include the engine-layer plan header, so the absent
// sentinel is duplicated on both sides of the draw stream; the scatter routes
// by one and consumers filter by the other, so they must stay equal.
TEST(MeshPoolGroupPlanContract, AbsentSentinelMatchesDrawStreamBuilder)
{
    EXPECT_EQ(MeshPoolGroupPlan::kAbsentGroup, GPUDrawStreamBuilder::kAbsentPoolGroup);
}

TEST_F(MeshPoolGroupPlanTest, UnmappedRowsReadAbsent)
{
    RegisterMesh(0, MakeTriangle(false, false));
    MeshPoolGroupPlan plan;
    // Larger table than the registry populates: the tail rows are absent.
    plan.Refresh(m_Registry, MeshTableCount() + 4u);
    EXPECT_EQ(plan.GroupOf(MeshTableCount() + 1u), MeshPoolGroupPlan::kAbsentGroup);
    // Out-of-bounds lookups are absent too (consumer guard path).
    EXPECT_EQ(plan.GroupOf(0xFFFFFFu), MeshPoolGroupPlan::kAbsentGroup);

    // The ordered axis mirrors the absence: unmapped rows read the sentinel.
    ASSERT_GT(plan.MeshToOrderedSpan().size(), MeshTableCount() + 1u);
    EXPECT_EQ(plan.MeshToOrderedSpan()[MeshTableCount() + 1u], MeshPoolGroupPlan::kAbsentGroup);
}

// Both-or-neither invariant at the source: a Refresh that finds ZERO live
// registry entries (scene-load streaming transient / teardown seam) must
// publish EMPTY spans on EVERY axis — a non-empty all-absent mesh map paired
// with an empty rank->group inverse would trip ScheduleUnifiedScatter's
// paired-span assert (live in DebugFast) and split the consolidation signal
// between consumers and the scatter.
TEST_F(MeshPoolGroupPlanTest, EmptyRegistrySnapshotPublishesEmptySpansOnEveryAxis)
{
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, /*meshTableCount=*/4u); // rows exist, none live
    EXPECT_TRUE(plan.MeshToGroupSpan().empty());
    EXPECT_TRUE(plan.MeshToOrderedSpan().empty());
    EXPECT_TRUE(plan.OrderedToGroupSpan().empty());
    EXPECT_EQ(plan.GroupOf(0u), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_EQ(plan.LiveGroupCount(), 0u);

    // And the plan recovers on the next Refresh once a mesh goes live.
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_FALSE(plan.MeshToOrderedSpan().empty());
    EXPECT_EQ(plan.OrderedToGroupSpan().size(), 1u);
    EXPECT_NE(plan.GroupOf(meshA), MeshPoolGroupPlan::kAbsentGroup);
}

// The ordered mesh axis: dense unique ranks over the live meshes, sorted so a
// group's members are CONTIGUOUS (what lets the scatter scheduler span a
// group's per-mesh table rows with one published range and pack its region
// mesh-major), with OrderedToGroupSpan as the exact rank->group inverse.
TEST_F(MeshPoolGroupPlanTest, OrderedRanksAreDenseGroupContiguousAndInverseConsistent)
{
    // Interleave two identities so registration order differs from group order.
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false)); // g0
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));  // g1
    const uint32_t meshC = RegisterMesh(2, MakeTriangle(false, false)); // g0
    const uint32_t meshD = RegisterMesh(3, MakeTriangle(true, false));  // g1

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    const auto ordered = plan.MeshToOrderedSpan();
    const auto inverse = plan.OrderedToGroupSpan();
    ASSERT_EQ(inverse.size(), 4u); // one rank per live mesh

    // Dense unique ranks covering [0, live).
    std::vector<bool> seen(4, false);
    for (uint32_t mesh : {meshA, meshB, meshC, meshD})
    {
        ASSERT_LT(mesh, ordered.size());
        const uint32_t rank = ordered[mesh];
        ASSERT_LT(rank, 4u);
        EXPECT_FALSE(seen[rank]) << "ranks must be unique per live mesh";
        seen[rank] = true;
        // Inverse agrees with the consumer-facing group map.
        EXPECT_EQ(inverse[rank], plan.GroupOf(mesh));
    }

    // Group members contiguous: same-group meshes hold adjacent ranks.
    EXPECT_EQ(plan.GroupOf(meshA), plan.GroupOf(meshC));
    EXPECT_EQ(ordered[meshC], ordered[meshA] + 1u);
    EXPECT_EQ(plan.GroupOf(meshB), plan.GroupOf(meshD));
    EXPECT_EQ(ordered[meshD], ordered[meshB] + 1u);

    // The inverse is group-sorted (runs never interleave).
    for (size_t i = 1; i < inverse.size(); ++i)
        EXPECT_LE(inverse[i - 1], inverse[i]);
}

} // namespace
