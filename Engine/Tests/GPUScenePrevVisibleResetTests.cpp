// Two-phase HZB prevVisible reset plumbing — HLOD prerequisite P2 (design C1).
//
// A recycled GPUScene instance slot must not inherit the prior tenant's HZB
// occlusion history, or the new instance pops in one frame late at the next
// camera motion. GPUScene queues every RECYCLED slot (free-list reuse or
// re-append into a trimmed index) for a prevVisible[slot]=0xFFFFFFFF reset;
// GPUCullingPipeline drains + coalesces the queue each frame and fills those
// entries before phase A reads them.
//
// These tests cover the CPU plumbing: which slots the queue captures (reuse,
// NOT bare frees), the drain/clear contract, and the run-coalescing math. The
// end-to-end GPU application rides the runtime gate (bench churn with HZB on)
// plus the existing HzbCullingComputeTest.PhaseAGatesOnPrevVisible, which
// already proves prevVisible != 0 ⇒ phase A treats the slot as visible.

#include <gtest/gtest.h>

#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PrevVisibleResetTracker.h"

#include "TestDeviceHelper.h"

#include <algorithm>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

GPUInstance MakeInstance(uint32_t materialIndex, uint32_t meshIndex)
{
    GPUInstance instance{};
    instance.materialIndex = materialIndex;
    instance.meshIndex = meshIndex;
    instance.boundingRadius = 1.0f;
    return instance;
}

// ---- Coalescing helper (pure, no device) -----------------------------------

using ResetRun = std::pair<uint32_t, uint32_t>;

std::vector<ResetRun> Coalesce(std::vector<uint32_t> slots)
{
    std::vector<ResetRun> runs;
    PrevVisibleResetTracker::CoalesceResetRuns(slots, runs);
    return runs;
}

// Compare element-wise on scalars (gtest's container printer/operator== on a
// vector<pair> does not resolve cleanly under this toolchain's C++20 mode).
void ExpectRuns(const std::vector<ResetRun>& got, const std::vector<ResetRun>& expected)
{
    ASSERT_EQ(got.size(), expected.size()) << "run count";
    for (size_t i = 0; i < got.size(); ++i)
    {
        EXPECT_EQ(got[i].first, expected[i].first) << "run " << i << " start";
        EXPECT_EQ(got[i].second, expected[i].second) << "run " << i << " count";
    }
}

TEST(PrevVisibleResetRunsUnit, EmptyProducesNoRuns)
{
    const std::vector<uint32_t> in;
    EXPECT_TRUE(Coalesce(in).empty());
}

TEST(PrevVisibleResetRunsUnit, SingleSlotIsOneRunOfOne)
{
    const std::vector<uint32_t> in = {7u};
    ExpectRuns(Coalesce(in), {{7u, 1u}});
}

TEST(PrevVisibleResetRunsUnit, ContiguousMergesIntoOneRun)
{
    const std::vector<uint32_t> in = {3u, 4u, 5u, 6u};
    ExpectRuns(Coalesce(in), {{3u, 4u}});
}

TEST(PrevVisibleResetRunsUnit, DisjointStaysSeparate)
{
    const std::vector<uint32_t> in = {1u, 5u, 6u, 10u};
    ExpectRuns(Coalesce(in), {{1u, 1u}, {5u, 2u}, {10u, 1u}});
}

TEST(PrevVisibleResetRunsUnit, UnsortedWithDuplicatesSortsDedupsAndMerges)
{
    // sorted-unique = {3,4,6,10} → runs (3,2),(6,1),(10,1)
    const std::vector<uint32_t> in = {6u, 3u, 3u, 4u, 10u, 6u};
    ExpectRuns(Coalesce(in), {{3u, 2u}, {6u, 1u}, {10u, 1u}});
}

// ---- Per-view pending tracker (pure, no device) ----------------------------
//
// The tracker fixes the review MUST-FIX: the once-per-frame drain fans recycled
// slots into EVERY tracked view's pending set, and a view flushes only its own
// set when it next schedules phase A — so a view idle for a frame still applies
// the resets it missed when it returns, instead of losing them at the drain.

std::vector<uint32_t> Take(PrevVisibleResetTracker& tracker, ViewId viewId)
{
    std::vector<uint32_t> out;
    tracker.TakeViewPending(viewId, out);
    return out; // sorted-unique by construction
}

TEST(PrevVisibleResetTrackerUnit, FansRecycledSlotsToEveryTrackedView)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);
    t.OnBufferInitialized(2u);

    std::vector<uint32_t> recycled = {5u};
    t.OnSlotsRecycled(recycled);

    // Flushing view 1 must not touch view 2's pending.
    EXPECT_EQ(Take(t, 1u), std::vector<uint32_t>{5u});
    EXPECT_EQ(t.PendingCount(2u), 1u);
    EXPECT_EQ(Take(t, 2u), std::vector<uint32_t>{5u});
}

// The core fix: a view NOT flushed for several frames accumulates every frame's
// recycled slots and applies them all when it finally schedules phase A.
TEST(PrevVisibleResetTrackerUnit, IdleViewAccumulatesThenFlushesOnReturn)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);

    std::vector<uint32_t> f1 = {2u, 4u};
    t.OnSlotsRecycled(f1); // view 1 not flushed this frame
    std::vector<uint32_t> f2 = {7u};
    t.OnSlotsRecycled(f2); // still not flushed

    const std::vector<uint32_t> expected = {2u, 4u, 7u};
    EXPECT_EQ(Take(t, 1u), expected) << "returning view applies every missed reset";
    EXPECT_TRUE(Take(t, 1u).empty()) << "flush clears the view's pending";
}

// A freshly (re)created buffer is fully reset by its first-touch init, so its
// pending must be dropped.
TEST(PrevVisibleResetTrackerUnit, NewBufferClearsPending)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);
    std::vector<uint32_t> s = {2u, 4u};
    t.OnSlotsRecycled(s);
    ASSERT_EQ(t.PendingCount(1u), 2u);

    t.OnBufferInitialized(1u); // buffer regrown → first-touch supersedes
    EXPECT_EQ(t.PendingCount(1u), 0u);
    EXPECT_TRUE(Take(t, 1u).empty());
}

TEST(PrevVisibleResetTrackerUnit, FlushClearsOnlyTheFlushedView)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);
    t.OnBufferInitialized(2u);
    std::vector<uint32_t> s = {5u, 9u};
    t.OnSlotsRecycled(s);

    EXPECT_EQ(Take(t, 1u), (std::vector<uint32_t>{5u, 9u}));
    EXPECT_EQ(t.PendingCount(1u), 0u);
    EXPECT_EQ(t.PendingCount(2u), 2u) << "view 2 keeps its pending until it flushes";
}

TEST(PrevVisibleResetTrackerUnit, MergesSortsAndDedupsAcrossFrames)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);
    std::vector<uint32_t> s1 = {3u, 3u, 1u};
    t.OnSlotsRecycled(s1);
    std::vector<uint32_t> s2 = {3u, 5u};
    t.OnSlotsRecycled(s2);
    EXPECT_EQ(Take(t, 1u), (std::vector<uint32_t>{1u, 3u, 5u}));
}

TEST(PrevVisibleResetTrackerUnit, NoTrackedViewsDropsSlots)
{
    PrevVisibleResetTracker t;
    std::vector<uint32_t> s = {1u, 2u};
    t.OnSlotsRecycled(s); // no views yet → dropped; a future view gets first-touch
    EXPECT_EQ(t.TrackedViewCount(), 0u);
    EXPECT_TRUE(Take(t, 42u).empty());
}

TEST(PrevVisibleResetTrackerUnit, EmptyRecycledIsNoOp)
{
    PrevVisibleResetTracker t;
    t.OnBufferInitialized(1u);
    std::vector<uint32_t> empty;
    t.OnSlotsRecycled(empty);
    EXPECT_EQ(t.PendingCount(1u), 0u);
}

// ---- GPUScene reset-queue plumbing -----------------------------------------

class GPUScenePrevVisibleResetTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(1024u, 64u));
    }

    void TearDown() override
    {
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    std::vector<uint32_t> Drain()
    {
        std::vector<uint32_t> out;
        m_Scene->DrainPrevVisibleResetSlots(out);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
};

// Genuinely fresh appends occupy pristine slots (prevVisible still 0xFFFFFFFF
// from first-touch) → nothing to reset.
TEST_F(GPUScenePrevVisibleResetTest, FreshAppendsQueueNothing)
{
    for (uint32_t i = 0; i < 8u; ++i)
        m_Scene->AddInstance(MakeInstance(1u, 10u));
    EXPECT_TRUE(Drain().empty());
}

// Reset-on-REUSE, not reset-on-free: removing (without re-adding) must NOT
// queue, because the freed tombstone stays in the cull range and phase B keeps
// rewriting its prevVisible until the slot is actually reused.
TEST_F(GPUScenePrevVisibleResetTest, BareRemovesQueueNothing)
{
    std::vector<uint32_t> idx;
    for (uint32_t i = 0; i < 8u; ++i)
        idx.push_back(m_Scene->AddInstance(MakeInstance(1u, 10u)));
    (void)Drain(); // clear the fresh-add bookkeeping

    m_Scene->RemoveInstances(std::vector<uint32_t>{idx[1], idx[3], idx[5]}); // mid-array, no trim
    EXPECT_TRUE(Drain().empty()) << "a free with no reuse must not queue a reset";
}

// Removing a mid-array slot then re-adding recycles it → that slot is queued.
TEST_F(GPUScenePrevVisibleResetTest, ReuseAfterSingleRemoveQueuesSlot)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // keeps `a` mid-array (no trim)
    (void)Drain();

    m_Scene->RemoveInstance(a);
    EXPECT_TRUE(Drain().empty()) << "remove alone does not queue";

    const uint32_t reused = m_Scene->AddInstance(MakeInstance(2u, 20u));
    EXPECT_EQ(reused, a) << "LIFO free list should hand back the same slot";
    EXPECT_EQ(Drain(), std::vector<uint32_t>{a});
}

// Batched eviction + re-adds: every recycled slot is queued exactly once.
TEST_F(GPUScenePrevVisibleResetTest, ReuseAfterBatchRemoveQueuesEachReusedSlot)
{
    std::vector<uint32_t> idx;
    for (uint32_t i = 0; i < 10u; ++i)
        idx.push_back(m_Scene->AddInstance(MakeInstance(1u, 10u)));
    (void)Drain();

    const std::vector<uint32_t> doomed = {2u, 4u, 6u}; // mid-array
    m_Scene->RemoveInstances(doomed);
    (void)Drain(); // frees queue nothing

    for (uint32_t i = 0; i < doomed.size(); ++i)
        m_Scene->AddInstance(MakeInstance(3u, 30u));

    std::vector<uint32_t> expected = doomed;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(Drain(), expected);
}

// Re-append into a TRIMMED index (free list empty after trim) also recycles a
// previously-live slot → it must be queued.
TEST_F(GPUScenePrevVisibleResetTest, ReAppendIntoTrimmedIndexQueuesSlot)
{
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 0
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 1
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 2 (high-water = 3)
    (void)Drain();

    m_Scene->RemoveInstance(2u); // tail → trims away; free list empties
    ASSERT_EQ(m_Scene->GetInstanceCount(), 2u);
    ASSERT_TRUE(m_Scene->GetFreeInstanceSlots().empty());
    EXPECT_TRUE(Drain().empty());

    const uint32_t reAppended = m_Scene->AddInstance(MakeInstance(2u, 20u));
    EXPECT_EQ(reAppended, 2u) << "append reuses the trimmed index";
    EXPECT_EQ(Drain(), std::vector<uint32_t>{2u}) << "re-appended trimmed index needs a reset";
}

// Drain clears the queue: a second drain in the same churn window is empty.
TEST_F(GPUScenePrevVisibleResetTest, DrainClearsQueue)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->AddInstance(MakeInstance(1u, 10u));
    (void)Drain();
    m_Scene->RemoveInstance(a);
    m_Scene->AddInstance(MakeInstance(2u, 20u)); // recycles `a`

    EXPECT_EQ(Drain(), std::vector<uint32_t>{a});
    EXPECT_TRUE(Drain().empty()) << "queue must be empty after a drain";
}

// ---- Per-slot continuity stamp (T18, lifetime half) -------------------------
//
// A consumer that carries per-instance state across frames may trust it only
// while the slot's stamp is unchanged, so two tenants of one slot must never
// share a stamp — through free-list reuse AND through a trimmed index that a
// later append reoccupies. The stamp is what a batch key cannot be: a recycled
// slot rejoins the very same (material, mesh) key with a different tenant.

TEST_F(GPUScenePrevVisibleResetTest, ContinuityStampIsFreshForEveryTenantOfASlot)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t firstTenant = m_Scene->GetInstanceContinuityStamp(a);
    EXPECT_NE(firstTenant, 0u) << "a live slot always carries a non-zero stamp";

    m_Scene->RemoveInstance(a);
    const uint32_t recycled = m_Scene->AddInstance(MakeInstance(1u, 10u)); // same key, new tenant
    ASSERT_EQ(recycled, a) << "precondition: the free-list handed back the same slot";
    EXPECT_NE(m_Scene->GetInstanceContinuityStamp(a), firstTenant);
}

TEST_F(GPUScenePrevVisibleResetTest, ContinuityStampSurvivesTrailingTrim)
{
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 0
    m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 1
    const uint32_t tail = m_Scene->AddInstance(MakeInstance(1u, 10u)); // slot 2
    ASSERT_EQ(tail, 2u);
    const uint32_t firstTenant = m_Scene->GetInstanceContinuityStamp(tail);

    m_Scene->RemoveInstance(tail); // trims the array back to two rows
    ASSERT_EQ(m_Scene->GetInstanceCount(), 2u);

    const uint32_t reAppended = m_Scene->AddInstance(MakeInstance(2u, 20u));
    ASSERT_EQ(reAppended, tail) << "precondition: append reuses the trimmed index";
    EXPECT_NE(m_Scene->GetInstanceContinuityStamp(tail), firstTenant)
        << "a trimmed index must not restart its count and collide with the prior tenant";
}

TEST_F(GPUScenePrevVisibleResetTest, ContinuityStampBumpMovesOnlyTheNamedSlot)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t b = m_Scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t aBefore = m_Scene->GetInstanceContinuityStamp(a);
    const uint32_t bBefore = m_Scene->GetInstanceContinuityStamp(b);

    m_Scene->BumpInstanceContinuityStamp(a);

    EXPECT_GT(m_Scene->GetInstanceContinuityStamp(a), aBefore);
    EXPECT_EQ(m_Scene->GetInstanceContinuityStamp(b), bBefore);
    // An index the scene has never handed out reads as no stamp and is ignored.
    EXPECT_EQ(m_Scene->GetInstanceContinuityStamp(1000u), 0u);
    m_Scene->BumpInstanceContinuityStamp(1000u);
    EXPECT_EQ(m_Scene->GetInstanceContinuityStamp(1000u), 0u);
}

// ---- Continuity reset queue (the device transport) --------------------------
//
// The stamp is processor state and GPUInstance has no spare word, so a
// device-side consumer learns about a break from an explicit reset list: every
// slot whose stamp moved, drained once per frame and refilled in the consumer's
// own previous-frame buffer. Queueing at the one place the stamp advances is
// what makes the list and the stamps agree by construction.

TEST_F(GPUScenePrevVisibleResetTest, ContinuityDrainReportsEverySlotWhoseStampMoved)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    const uint32_t b = m_Scene->AddInstance(MakeInstance(1u, 10u));
    {
        const std::vector<uint32_t> adds = m_Scene->DrainContinuityResetSlots();
        EXPECT_NE(std::find(adds.begin(), adds.end(), a), adds.end())
            << "a new tenant is a continuity break for anything keyed on the slot";
        EXPECT_NE(std::find(adds.begin(), adds.end(), b), adds.end());
    }
    EXPECT_TRUE(m_Scene->DrainContinuityResetSlots().empty()) << "the drain clears the queue";

    // A payload change keeps the tenant, so the recycled-slot queue says nothing
    // about it — this is the half only the stamp can report.
    m_Scene->BumpInstanceContinuityStamp(b);
    std::vector<uint32_t> recycled;
    m_Scene->DrainPrevVisibleResetSlots(recycled);
    EXPECT_TRUE(recycled.empty()) << "nothing was recycled";
    const std::vector<uint32_t> changed = m_Scene->DrainContinuityResetSlots();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0], b);
}

TEST_F(GPUScenePrevVisibleResetTest, ContinuityDrainCoversTheRecycledSlotsToo)
{
    const uint32_t a = m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->AddInstance(MakeInstance(1u, 10u));
    m_Scene->DrainContinuityResetSlots();
    std::vector<uint32_t> ignored;
    m_Scene->DrainPrevVisibleResetSlots(ignored);

    m_Scene->RemoveInstance(a);
    const uint32_t reused = m_Scene->AddInstance(MakeInstance(1u, 10u));
    ASSERT_EQ(reused, a) << "precondition: the free-list handed back the same slot";

    std::vector<uint32_t> recycled;
    m_Scene->DrainPrevVisibleResetSlots(recycled);
    ASSERT_EQ(recycled.size(), 1u);
    EXPECT_EQ(recycled[0], a);
    const std::vector<uint32_t> changed = m_Scene->DrainContinuityResetSlots();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0], a)
        << "one drain is enough for a consumer that needs both halves: the stamp "
           "moves for a recycled tenant as well as for a payload change";
}

} // namespace
