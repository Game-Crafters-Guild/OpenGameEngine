// BatchRegistry: incremental live-instance counts per (materialIndex,
// meshIndex) batch, maintained by GPUScene's three instance mutators. These
// tests lock the count invariants the R1.3/R1.4 scatter bucketer sizes its
// output regions from — above all that tombstone keys (0xFFFFFFFF) never
// enter the map on either the increment or the decrement side, since an
// underflowed count would demand a ~4 billion record region.

#include <gtest/gtest.h>

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"

#include "TestDeviceHelper.h"

#include <random>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

GPUInstance MakeInstance(uint32_t materialIndex, uint32_t meshIndex, bool mirrored = false)
{
    GPUInstance instance{};
    instance.materialIndex = materialIndex;
    instance.meshIndex     = meshIndex;
    instance.boundingRadius = 1.0f;
    if (mirrored)
        instance.flags |= kInstanceFlagMirrored;
    return instance;
}

GPUInstance MakeTombstone()
{
    GPUInstance instance{};
    instance.materialIndex = 0xFFFFFFFFu;
    instance.meshIndex     = 0xFFFFFFFFu;
    return instance;
}

// ---- Pure-type tests (no device) -------------------------------------------

TEST(BatchRegistryUnit, KeyPacksAndSortsByMaterialThenMesh)
{
    const uint64_t a = BatchRegistry::MakeKey(1u, 500u);
    const uint64_t b = BatchRegistry::MakeKey(2u, 3u);
    EXPECT_LT(a, b); // materialIndex is the primary sort field
    EXPECT_EQ(BatchRegistry::MaterialIndexFromKey(b), 2u);
    EXPECT_EQ(BatchRegistry::MeshIndexFromKey(b), 3u);

    const uint64_t c = BatchRegistry::MakeKey(2u, 4u);
    EXPECT_LT(b, c); // meshIndex is the secondary sort field
}

TEST(BatchRegistryUnit, AddRemoveCountsAndErasesAtZero)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(5u, 7u, /*mirrored=*/false);
    reg.OnInstanceAdded(5u, 7u, /*mirrored=*/false);
    ASSERT_EQ(reg.BatchCount(), 1u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Total(), 2u);

    reg.OnInstanceRemoved(5u, 7u, /*mirrored=*/false);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Total(), 1u);
    reg.OnInstanceRemoved(5u, 7u, /*mirrored=*/false);
    EXPECT_EQ(reg.BatchCount(), 0u); // erased at zero, no lingering zero entry
}

TEST(BatchRegistryUnit, ParitySplitsCountAndConservesTotal)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(5u, 7u, /*mirrored=*/false);
    reg.OnInstanceAdded(5u, 7u, /*mirrored=*/true);
    reg.OnInstanceAdded(5u, 7u, /*mirrored=*/true);

    const auto& c = reg.Counts().at(BatchRegistry::MakeKey(5u, 7u));
    EXPECT_EQ(c.Even, 1u);
    EXPECT_EQ(c.Odd, 2u);
    EXPECT_EQ(c.Total(), 3u); // conservation: even + odd == old single count

    // Removing the right parity lane drains only that lane.
    reg.OnInstanceRemoved(5u, 7u, /*mirrored=*/true);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Odd, 1u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Even, 1u);

    // A parity FLIP with the same (mat, mesh) moves the count between lanes.
    reg.OnInstanceUpdated(5u, 7u, /*oldMirrored=*/false, 5u, 7u, /*newMirrored=*/true);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Even, 0u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 7u)).Odd, 2u);
}

TEST(BatchRegistryUnit, TombstoneKeyIgnoredOnBothSides)
{
    BatchRegistry reg;
    // Increment side: adding a tombstone tracks nothing.
    reg.OnInstanceAdded(0xFFFFFFFFu, 0xFFFFFFFFu, /*mirrored=*/false);
    EXPECT_EQ(reg.BatchCount(), 0u);

    // Decrement side: removing a tombstone decrements nothing (would
    // underflow to 0xFFFFFFFF and explode region sizing otherwise).
    reg.OnInstanceRemoved(0xFFFFFFFFu, 0xFFFFFFFFu, /*mirrored=*/false);
    EXPECT_EQ(reg.BatchCount(), 0u);

    // Half-invalid pairs are equally untracked.
    reg.OnInstanceAdded(3u, 0xFFFFFFFFu, /*mirrored=*/false);
    reg.OnInstanceAdded(0xFFFFFFFFu, 3u, /*mirrored=*/false);
    EXPECT_EQ(reg.BatchCount(), 0u);
}

TEST(BatchRegistryUnit, UpdateMovesCountBetweenBatches)
{
    BatchRegistry reg;
    reg.OnInstanceAdded(1u, 1u, /*mirrored=*/false);
    reg.OnInstanceUpdated(1u, 1u, /*oldMirrored=*/false, 2u, 2u, /*newMirrored=*/false);
    EXPECT_EQ(reg.Counts().count(BatchRegistry::MakeKey(1u, 1u)), 0u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 2u)).Total(), 1u);

    // Same-key same-parity update is a no-op.
    reg.OnInstanceUpdated(2u, 2u, /*oldMirrored=*/false, 2u, 2u, /*newMirrored=*/false);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 2u)).Total(), 1u);

    // Update TO a tombstone key (ClearMeshGpuInstance) drains the old batch
    // without creating a tombstone entry.
    reg.OnInstanceUpdated(2u, 2u, /*oldMirrored=*/false, 0xFFFFFFFFu, 0xFFFFFFFFu, /*newMirrored=*/false);
    EXPECT_EQ(reg.BatchCount(), 0u);

    // ...and back (re-enable) re-tracks.
    reg.OnInstanceUpdated(0xFFFFFFFFu, 0xFFFFFFFFu, /*oldMirrored=*/false, 2u, 2u, /*newMirrored=*/false);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 2u)).Total(), 1u);
}

// ---- GPUScene integration (mutator hook placement) --------------------------

class GPUSceneBatchRegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
        {
            GTEST_SKIP() << "No Vulkan device available";
        }
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(1024u, 256u));
    }

    void TearDown() override
    {
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Brute-force recount from the live instance rows — ground truth the
    // incremental registry must match after any mutation sequence. Splits by
    // the mirror flag exactly as the registry does.
    std::unordered_map<uint64_t, BatchRegistry::ParityCounts> Rescan() const
    {
        std::unordered_map<uint64_t, BatchRegistry::ParityCounts> counts;
        for (uint32_t i = 0; i < m_Scene->GetInstanceCount(); ++i)
        {
            const GPUInstance& inst = m_Scene->GetInstances()[i];
            if (!BatchRegistry::IsTracked(inst.materialIndex, inst.meshIndex))
                continue;
            BatchRegistry::ParityCounts& c =
                counts[BatchRegistry::MakeKey(inst.materialIndex, inst.meshIndex)];
            if ((inst.flags & kInstanceFlagMirrored) != 0u)
                ++c.Odd;
            else
                ++c.Even;
        }
        return counts;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
};

TEST_F(GPUSceneBatchRegistryTest, AddUpdateRemoveMaintainCounts)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t b = m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->AddInstance(MakeInstance(2u, 20u));

    const auto& reg = m_Scene->GetBatchRegistry();
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(1u, 10u)).Total(), 2u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 20u)).Total(), 1u);

    // Re-home instance b to another batch via UpdateInstance.
    m_Scene->UpdateInstance(b, MakeInstance(2u, 20u));
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(1u, 10u)).Total(), 1u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 20u)).Total(), 2u);

    m_Scene->RemoveInstance(a);
    EXPECT_EQ(reg.Counts().count(BatchRegistry::MakeKey(1u, 10u)), 0u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(2u, 20u)).Total(), 2u);
    EXPECT_EQ(Rescan(), reg.Counts());
}

TEST_F(GPUSceneBatchRegistryTest, ClearThenReleaseDoesNotUnderflow)
{
    // The disable-then-delete editor sequence: ClearMeshGpuInstance writes a
    // tombstone via UpdateInstance (slot stays live), then a later
    // ReleaseMeshGpuInstance calls RemoveInstance on the already-tombstoned
    // row. The registry must drain the batch exactly once.
    const uint32_t idx = m_Scene->AddInstance(MakeInstance(3u, 30u));
    const auto& reg = m_Scene->GetBatchRegistry();
    ASSERT_EQ(reg.Counts().at(BatchRegistry::MakeKey(3u, 30u)).Total(), 1u);

    m_Scene->UpdateInstance(idx, MakeTombstone()); // Clear
    EXPECT_EQ(reg.BatchCount(), 0u);

    m_Scene->UpdateInstance(idx, MakeTombstone()); // repeated Clear (memcmp no-op)
    EXPECT_EQ(reg.BatchCount(), 0u);

    m_Scene->RemoveInstance(idx); // Release of a tombstoned row
    EXPECT_EQ(reg.BatchCount(), 0u);
    EXPECT_EQ(Rescan(), reg.Counts());
}

TEST_F(GPUSceneBatchRegistryTest, SlotReuseRetracksNewTenant)
{
    const uint32_t first = m_Scene->AddInstance(MakeInstance(4u, 40u));
    m_Scene->RemoveInstance(first);

    // LIFO free list hands the same slot to the next Add.
    const uint32_t second = m_Scene->AddInstance(MakeInstance(5u, 50u));
    EXPECT_EQ(second, first);

    const auto& reg = m_Scene->GetBatchRegistry();
    EXPECT_EQ(reg.Counts().count(BatchRegistry::MakeKey(4u, 40u)), 0u);
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(5u, 50u)).Total(), 1u);
}

TEST_F(GPUSceneBatchRegistryTest, DoubleRemoveDecrementsOnce)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(6u, 60u));
    m_Scene->AddInstance(MakeInstance(6u, 60u));

    m_Scene->RemoveInstance(a);
    m_Scene->RemoveInstance(a); // second remove hits the free-slot guard

    const auto& reg = m_Scene->GetBatchRegistry();
    EXPECT_EQ(reg.Counts().at(BatchRegistry::MakeKey(6u, 60u)).Total(), 1u);
    EXPECT_EQ(Rescan(), reg.Counts());
}

TEST_F(GPUSceneBatchRegistryTest, FuzzMatchesBruteForceRescan)
{
    std::mt19937 rng(0xC0FFEEu); // deterministic
    std::vector<uint32_t> live;

    for (int op = 0; op < 2000; ++op)
    {
        const uint32_t roll = rng() % 100u;
        if (roll < 45u || live.empty())
        {
            const uint32_t mat  = 1u + rng() % 8u;
            const uint32_t mesh = 1u + rng() % 8u;
            // ~1/3 mirrored so the even/odd parity lanes get fuzz coverage.
            live.push_back(m_Scene->AddInstance(MakeInstance(mat, mesh, (rng() % 3u) == 0u)));
        }
        else if (roll < 75u)
        {
            const uint32_t pick = static_cast<uint32_t>(rng() % live.size());
            if (roll < 55u)
            {
                // Re-home (or occasionally tombstone via Clear-style update),
                // sometimes flipping parity to move a count between lanes.
                const bool clear = (rng() % 10u) == 0u;
                m_Scene->UpdateInstance(
                    live[pick],
                    clear ? MakeTombstone()
                          : MakeInstance(1u + rng() % 8u, 1u + rng() % 8u, (rng() % 3u) == 0u));
            }
            else
            {
                // Byte-identical rewrite — must be a registry no-op.
                const GPUInstance copy = m_Scene->GetInstances()[live[pick]];
                m_Scene->UpdateInstance(live[pick], copy);
            }
        }
        else
        {
            const uint32_t pick = static_cast<uint32_t>(rng() % live.size());
            m_Scene->RemoveInstance(live[pick]);
            live.erase(live.begin() + pick);
        }
    }

    EXPECT_EQ(Rescan(), m_Scene->GetBatchRegistry().Counts());
}

} // namespace
