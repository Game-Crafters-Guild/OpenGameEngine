// GPUScene::RemoveInstances (batched removal) — HLOD prerequisite P1.
//
// The batched path must be observationally identical to N single-index
// RemoveInstance calls: same instance rows, same 32 B scatter-hot mirror, same
// free-slot set, same BatchRegistry counts, same live count. It must also fold
// duplicate indices, skip out-of-range / already-free indices, run the trailing
// trim exactly once, and evict a large cluster without the single path's
// O(cluster·free-list) std::find blowup.

#include <gtest/gtest.h>

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

GPUInstance MakeInstance(uint32_t materialIndex, uint32_t meshIndex, bool mirrored = false)
{
    GPUInstance instance{};
    instance.materialIndex = materialIndex;
    instance.meshIndex = meshIndex;
    instance.boundingRadius = 1.0f;
    if (mirrored)
        instance.flags |= kInstanceFlagMirrored;
    return instance;
}

class GPUSceneRemoveInstancesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<GPUScene> MakeScene(uint32_t maxInstances = 4096u)
    {
        auto scene = std::make_unique<GPUScene>(m_Device.get());
        EXPECT_TRUE(scene->Initialize(maxInstances, 256u));
        return scene;
    }

    // Deterministic filler: material/mesh cycle over a few batches so the
    // BatchRegistry has real multi-batch structure to keep in lockstep.
    static GPUInstance FillerInstance(uint32_t i)
    {
        return MakeInstance(1u + (i % 4u), 10u + (i % 3u), (i % 5u) == 0u);
    }

    // Assert two scenes are in an identical observable state.
    static void ExpectStateEqual(const GPUScene& a, const GPUScene& b)
    {
        ASSERT_EQ(a.GetInstanceCount(), b.GetInstanceCount()) << "allocated slot count (array size)";
        EXPECT_EQ(a.GetLiveInstanceCount(), b.GetLiveInstanceCount()) << "live count";

        const auto& ia = a.GetInstances();
        const auto& ib = b.GetInstances();
        ASSERT_EQ(ia.size(), ib.size());
        for (size_t i = 0; i < ia.size(); ++i)
            EXPECT_EQ(std::memcmp(&ia[i], &ib[i], sizeof(GPUInstance)), 0)
                << "instance row " << i << " differs";

        // Scatter-hot mirror: size-locked and byte-identical.
        const auto& sa = a.GetScatterHot();
        const auto& sb = b.GetScatterHot();
        ASSERT_EQ(sa.size(), sb.size());
        for (size_t i = 0; i < sa.size(); ++i)
            EXPECT_EQ(std::memcmp(&sa[i], &sb[i], sizeof(GPUInstanceScatterHot)), 0)
                << "scatter-hot row " << i << " differs";
        // Mirror stays a pure function of the instance rows.
        if (a.IsScatterHotEnabled())
        {
            ASSERT_EQ(sa.size(), ia.size());
            for (size_t i = 0; i < ia.size(); ++i)
            {
                const GPUInstanceScatterHot expected = MakeScatterHot(ia[i]);
                EXPECT_EQ(std::memcmp(&sa[i], &expected, sizeof(GPUInstanceScatterHot)), 0)
                    << "scatter-hot row " << i << " diverged from its instance";
            }
        }

        // Free-list membership compared as a SET, not a sequence. Push order is
        // not part of the contract: only the surviving free SET is load-bearing
        // (it decides which slots the next adds reuse, LIFO within it), and the
        // trailing-trim fixpoint is order-independent either way.
        std::vector<uint32_t> fa(a.GetFreeInstanceSlots());
        std::vector<uint32_t> fb(b.GetFreeInstanceSlots());
        std::sort(fa.begin(), fa.end());
        std::sort(fb.begin(), fb.end());
        EXPECT_EQ(fa, fb) << "free-slot set differs";

        // BatchRegistry counts.
        EXPECT_EQ(a.GetBatchRegistry().Counts(), b.GetBatchRegistry().Counts())
            << "batch registry counts differ";
    }

    std::unique_ptr<IDevice> m_Device;
};

// The core equivalence: RemoveInstances(span) == N single RemoveInstance calls.
TEST_F(GPUSceneRemoveInstancesTest, BatchEqualsNSingleRemoves)
{
    constexpr uint32_t kCount = 64u;
    // Remove a scattered mid-array subset (leaves live tails so the trim is a
    // no-op on some and cascades on others across the two paths identically).
    const std::vector<uint32_t> doomed = {3u, 5u, 6u, 20u, 21u, 22u, 40u, 63u, 62u, 61u};

    auto single = MakeScene();
    auto batch = MakeScene();
    for (uint32_t i = 0; i < kCount; ++i)
    {
        single->AddInstance(FillerInstance(i));
        batch->AddInstance(FillerInstance(i));
    }

    for (uint32_t idx : doomed)
        single->RemoveInstance(idx);
    batch->RemoveInstances(doomed);

    ExpectStateEqual(*single, *batch);
    EXPECT_EQ(batch->GetLiveInstanceCount(), kCount - static_cast<uint32_t>(doomed.size()));
}

// Span order must not matter: reversed span produces the same state.
TEST_F(GPUSceneRemoveInstancesTest, SpanOrderIndependent)
{
    constexpr uint32_t kCount = 32u;
    std::vector<uint32_t> a = {2u, 7u, 8u, 9u, 30u, 31u};
    std::vector<uint32_t> b(a.rbegin(), a.rend());

    auto s1 = MakeScene();
    auto s2 = MakeScene();
    for (uint32_t i = 0; i < kCount; ++i)
    {
        s1->AddInstance(FillerInstance(i));
        s2->AddInstance(FillerInstance(i));
    }
    s1->RemoveInstances(a);
    s2->RemoveInstances(b);
    ExpectStateEqual(*s1, *s2);
}

// Interleaved add / removeBatch / add: reused slots come from the free-list and
// the state still matches the single-remove twin.
TEST_F(GPUSceneRemoveInstancesTest, InterleavedAddRemoveBatchAddReuse)
{
    auto single = MakeScene();
    auto batch = MakeScene();
    for (uint32_t i = 0; i < 16u; ++i)
    {
        single->AddInstance(FillerInstance(i));
        batch->AddInstance(FillerInstance(i));
    }

    const std::vector<uint32_t> doomed = {2u, 4u, 8u, 10u}; // mid-array → no trim
    for (uint32_t idx : doomed)
        single->RemoveInstance(idx);
    batch->RemoveInstances(doomed);

    // Re-add: both should recycle freed slots (array does not grow).
    const uint32_t sizeAfterRemove = batch->GetInstanceCount();
    for (uint32_t i = 0; i < doomed.size(); ++i)
    {
        single->AddInstance(FillerInstance(100u + i));
        batch->AddInstance(FillerInstance(100u + i));
    }
    EXPECT_EQ(batch->GetInstanceCount(), sizeAfterRemove) << "re-adds must recycle, not grow";
    EXPECT_TRUE(batch->GetFreeInstanceSlots().empty()) << "all freed slots reused";
    ExpectStateEqual(*single, *batch);
}

// A slot listed twice in the span is removed exactly once.
TEST_F(GPUSceneRemoveInstancesTest, DuplicateIndexInSpanRemovedOnce)
{
    auto scene = MakeScene();
    const uint32_t a = scene->AddInstance(MakeInstance(6u, 60u));
    scene->AddInstance(MakeInstance(6u, 60u));
    const uint32_t liveBefore = scene->GetLiveInstanceCount();

    scene->RemoveInstances(std::vector<uint32_t>{a, a, a});

    EXPECT_EQ(scene->GetLiveInstanceCount(), liveBefore - 1u);
    EXPECT_EQ(scene->GetBatchRegistry().Counts().at(BatchRegistry::MakeKey(6u, 60u)).Total(), 1u)
        << "duplicate index must decrement the batch exactly once";
}

// Out-of-range and already-free indices are skipped; the one live index drops.
TEST_F(GPUSceneRemoveInstancesTest, OutOfRangeAndAlreadyFreeSkipped)
{
    auto scene = MakeScene();
    const uint32_t keep = scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t freed = scene->AddInstance(MakeInstance(2u, 20u));
    const uint32_t live = scene->AddInstance(MakeInstance(3u, 30u));
    // A trailing keeper so removing `live` tombstones it IN PLACE instead of the
    // trailing-trim collapsing its slot (freed is already-free and adjacent, so
    // without this the whole tail [freed,live] would trim away and reading
    // GetInstances()[live] below would be out of range).
    const uint32_t tail = scene->AddInstance(MakeInstance(4u, 40u));
    scene->RemoveInstance(freed); // now on the free-list (mid-array, not trimmed)
    const uint32_t liveBefore = scene->GetLiveInstanceCount();

    const std::vector<uint32_t> mixed = {0xFFFFFFFFu, live, freed, 999999u};
    scene->RemoveInstances(mixed);

    EXPECT_EQ(scene->GetLiveInstanceCount(), liveBefore - 1u) << "only `live` should drop";
    // `keep` and the trailing keeper untouched.
    EXPECT_EQ(scene->GetInstances()[keep].meshIndex, 10u);
    EXPECT_EQ(scene->GetInstances()[tail].meshIndex, 40u);
    // `live` tombstoned in place (kept mid-array by `tail`).
    EXPECT_EQ(scene->GetInstances()[live].meshIndex, 0xFFFFFFFFu);
}

// Removing the whole trailing run trims the array back; matches single twin.
TEST_F(GPUSceneRemoveInstancesTest, TrailingRunTrimsArray)
{
    constexpr uint32_t kCount = 24u;
    auto single = MakeScene();
    auto batch = MakeScene();
    for (uint32_t i = 0; i < kCount; ++i)
    {
        single->AddInstance(FillerInstance(i));
        batch->AddInstance(FillerInstance(i));
    }

    // Remove the entire tail [8, 24) — the array should shrink to 8.
    std::vector<uint32_t> tail(kCount - 8u);
    std::iota(tail.begin(), tail.end(), 8u);

    for (uint32_t idx : tail)
        single->RemoveInstance(idx);
    batch->RemoveInstances(tail);

    EXPECT_EQ(batch->GetInstanceCount(), 8u) << "trailing free suffix must be trimmed away";
    EXPECT_TRUE(batch->GetFreeInstanceSlots().empty()) << "trimmed slots leave the free-list";
    ExpectStateEqual(*single, *batch);
}

// A pre-existing free slot sitting just below the batch's trailing run must be
// absorbed by the single trailing-trim: free 6 first (mid-array, no trim), then
// batch-remove {5,7} — with 5,6,7 all free the trim strips the whole 5..7
// suffix, converging to size 5 and an empty free-list, identical to the singles.
TEST_F(GPUSceneRemoveInstancesTest, TrailingTrimAbsorbsPreExistingFreeSlot)
{
    constexpr uint32_t kCount = 8u;
    auto single = MakeScene();
    auto batch = MakeScene();
    for (uint32_t i = 0; i < kCount; ++i)
    {
        single->AddInstance(FillerInstance(i));
        batch->AddInstance(FillerInstance(i));
    }

    // Pre-existing free slot 6 (mid-array: slot 7 still live → no trim yet).
    single->RemoveInstance(6u);
    batch->RemoveInstance(6u);
    ASSERT_EQ(batch->GetInstanceCount(), kCount);
    ASSERT_EQ(batch->GetFreeInstanceSlots().size(), 1u);

    single->RemoveInstance(5u);
    single->RemoveInstance(7u);
    batch->RemoveInstances(std::vector<uint32_t>{5u, 7u});

    EXPECT_EQ(batch->GetInstanceCount(), 5u) << "trim strips the 5..7 suffix incl. pre-free 6";
    EXPECT_TRUE(batch->GetFreeInstanceSlots().empty());
    ExpectStateEqual(*single, *batch);
}

// Removing every instance in one batch empties the array and all mirrors.
TEST_F(GPUSceneRemoveInstancesTest, RemoveAllEmptiesEverything)
{
    constexpr uint32_t kCount = 40u;
    auto scene = MakeScene();
    std::vector<uint32_t> all(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
        all[i] = scene->AddInstance(FillerInstance(i));

    scene->RemoveInstances(all);

    EXPECT_EQ(scene->GetInstanceCount(), 0u);
    EXPECT_EQ(scene->GetLiveInstanceCount(), 0u);
    EXPECT_TRUE(scene->GetFreeInstanceSlots().empty());
    EXPECT_TRUE(scene->GetScatterHot().empty());
    EXPECT_EQ(scene->GetBatchRegistry().BatchCount(), 0u);
}

// Perf smoke: a large single-call cluster eviction must not exhibit the old
// O(cluster·free-list) blowup. The pre-P1 path did a std::find over the growing
// free-list both in the double-remove guard and once per trimmed slot; at this
// scale that is billions of comparisons. The batched path is O(M log M + free).
// The bound is deliberately generous (this is a regression tripwire, not a
// micro-benchmark) — the quadratic path runs many seconds here.
TEST_F(GPUSceneRemoveInstancesTest, LargeBatchRemoveIsNotQuadratic)
{
    constexpr uint32_t kCount = 40000u;
    auto scene = MakeScene(kCount);
    std::vector<uint32_t> all(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
        all[i] = scene->AddInstance(FillerInstance(i));

    const auto t0 = std::chrono::steady_clock::now();
    scene->RemoveInstances(all);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    EXPECT_EQ(scene->GetInstanceCount(), 0u);
    EXPECT_EQ(scene->GetLiveInstanceCount(), 0u);
    EXPECT_LT(elapsedMs, 2000) << "batched removal of " << kCount
                               << " instances took " << elapsedMs
                               << " ms — smells like the O(M^2) std::find path";
}

} // namespace
