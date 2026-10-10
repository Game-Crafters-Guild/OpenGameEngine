// Pool-health classification (CBTPoolHealth.h) against stats recorded from real converged runs of
// CBTPlanarDemandGate. The samples predate the parent-level merge metric, so their merge demand is
// the pre-fix planar magnitude — which is what these cases need: they exist to lock the DISTINCTION
// the previous rule (MergeDemand > 1000 && MergeServed == 0) could not draw, and a sample under
// that rule's threshold would not exercise it. The spherical domain still reaches these magnitudes.
// The two cases it could not tell apart:
//
//   a converged planar tree at 32% occupancy with 708k free slots and zero overflow, carrying
//   25k of standing merge demand that no LEB diamond can ever satisfy — HEALTHY, and
//
//   a genuinely saturated pool at 100% occupancy whose splits are being rolled back for want of
//   slots while the merge frees none — the frozen topology.
//
// The old rule called BOTH of them stalled, which is what sent three investigations after a healthy
// terrain. These lock the distinction.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTPoolHealth.h"

using namespace GameEngine::CBTTerrain;

namespace
{
constexpr uint32_t kPool = 1048576u;
// DiagnoseCBTPool's second argument: whether any terrain was published to the renderer.
constexpr bool kPublished = true;
constexpr bool kSkipped = false;

CBTTessellationStats Stats(uint32_t live, int32_t splitDemand, int32_t splitServed,
                          int32_t mergeDemand, int32_t mergeServed)
{
    CBTTessellationStats st;
    st.PoolSize = kPool;
    st.LiveCount = live;
    st.FreeCount = static_cast<int32_t>(kPool - live);
    st.SplitDemand = splitDemand;
    st.SplitServed = splitServed;
    st.MergeDemand = mergeDemand;
    st.MergeServed = mergeServed;
    return st;
}

// The rule this replaced, kept only so the red arm below is legible as a comparison.
bool LegacyMergeStalled(const CBTTessellationStats& st)
{
    return st.MergeDemand > 1000 && st.MergeServed == 0;
}
} // namespace

// [planar-comfort] CONVERGED: live=340612/1048576 occ=0.3248 free=707964 splitDemand=0
// splitServed=0 mergeDemand=25214 mergeServed=0 overflowTotal=0
TEST(CBTPoolHealthTest, ConvergedTreeWithStandingMergeDemandIsHealthy)
{
    const CBTTessellationStats st = Stats(340612u, 0, 0, 25214, 0);
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_TRUE(LegacyMergeStalled(st)) << "sample no longer reproduces the false positive";
    EXPECT_FALSE(h.MergeStalled)
        << "a converged tree at 32% occupancy with 708k free slots is not deadlocked";
    EXPECT_FALSE(h.NearCapacity);
    EXPECT_FALSE(h.Saturated);
    EXPECT_EQ(h.State, CBTPoolState::Healthy);
}

// [planar-live] CONVERGED: live=629703 occ=0.6005 free=418873 splitDemand=0 splitServed=0
// mergeDemand=37009 mergeServed=0
TEST(CBTPoolHealthTest, ConvergedLivePoseWithHeadroomIsHealthy)
{
    const CBTTessellationStats st = Stats(629703u, 0, 0, 37009, 0);
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_TRUE(LegacyMergeStalled(st));
    EXPECT_FALSE(h.MergeStalled);
    EXPECT_EQ(h.State, CBTPoolState::Healthy);
}

// [steady-ungated] CONVERGED: live=1048575/1048576 occ=1.0000 free=1 splitDemand=134852
// splitServed=0 mergeDemand=37853 mergeServed=0 overflowGrowthTail=1718664
TEST(CBTPoolHealthTest, SaturatedPoolWithRolledBackSplitsIsStalled)
{
    const CBTTessellationStats st = Stats(1048575u, 134852, 0, 37853, 0);
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_TRUE(h.Saturated);
    EXPECT_TRUE(h.MergeStalled) << "splits rolled back at a full pool while the merge frees none IS "
                                   "the frozen topology";
    EXPECT_EQ(h.State, CBTPoolState::SaturatedSplitsBlocked);
}

// A full pool whose splits are still being served is filling, not frozen — the merge is doing its
// job even though some demand goes unserved every frame.
TEST(CBTPoolHealthTest, NearCapacityWithServedSplitsIsNotStalled)
{
    const CBTTessellationStats st = Stats(950000u, 5000, 4800, 20000, 0);
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_TRUE(h.NearCapacity);
    EXPECT_FALSE(h.Saturated);
    EXPECT_FALSE(h.MergeStalled);
    EXPECT_EQ(h.State, CBTPoolState::NearCapacity);
}

// Blocked splits below the near-capacity floor are not a MERGE problem: the slots a merge would
// free are already there in the hundreds of thousands. They are still a problem, and a distinct
// one — calling them healthy is how this branch's split starvation stayed invisible.
TEST(CBTPoolHealthTest, BlockedSplitsWithAnEmptyPoolAreNotAMergeStall)
{
    const CBTTessellationStats st = Stats(340612u, 4000, 0, 25214, 0);
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_FALSE(h.MergeStalled) << "free slots are not what these splits are waiting for";
    EXPECT_TRUE(h.SplitsStarved);
    EXPECT_EQ(h.State, CBTPoolState::SplitsStarvedWithHeadroom);
}

// The red arm for the starvation signal, on the two samples that bracket the Kernel_Split bail fix
// at the SAME comfort pose. Pre-fix, 24 candidates stand unserved against 707,988 free slots;
// post-fix the fixed point asks for nothing. A diagnosis that cannot separate these two cannot see
// the defect this branch fixes.
// [split:comfort] pre-fix  live=340588 free=707988 splitDemand=24 splitServed=0
// [split:comfort] post-fix live=340612 free=707964 splitDemand=0  splitServed=0
TEST(CBTPoolHealthTest, ComfortPoseSplitStarvationIsVisibleBeforeAndGoneAfter)
{
    const CBTPoolHealth before = DiagnoseCBTPool(Stats(340588u, 24, 0, 25202, 0), kPublished);
    EXPECT_TRUE(before.SplitsStarved)
        << "24 unserved splits against 707,988 free slots must not read as healthy";
    EXPECT_EQ(before.State, CBTPoolState::SplitsStarvedWithHeadroom);
    EXPECT_FALSE(before.MergeStalled) << "the merge side is not what blocks them";

    const CBTPoolHealth after = DiagnoseCBTPool(Stats(340612u, 0, 0, 25214, 0), kPublished);
    EXPECT_FALSE(after.SplitsStarved);
    EXPECT_EQ(after.State, CBTPoolState::Healthy);
}

// A near-capacity pool whose splits go unserved is the merge-stall case, not the starvation case:
// there the slots really are the constraint. The two must not collapse into one another.
TEST(CBTPoolHealthTest, StarvationAndMergeStallAreDistinctStates)
{
    const CBTPoolHealth starved = DiagnoseCBTPool(Stats(340612u, 4000, 0, 25214, 0), kPublished);
    const CBTPoolHealth stalled = DiagnoseCBTPool(Stats(1048575u, 134852, 0, 37853, 0), kPublished);

    EXPECT_TRUE(starved.SplitsStarved);
    EXPECT_FALSE(starved.MergeStalled);
    EXPECT_FALSE(stalled.SplitsStarved) << "at a full pool the splits ARE waiting on free slots";
    EXPECT_TRUE(stalled.MergeStalled);
    EXPECT_NE(starved.State, stalled.State);
}

// ---------------------------------------------------------------------------
// Inertness — "is anything tessellated at all", which occupancy alone cannot answer.
//
// A terrain the extraction system skips never reaches the renderer, so its CBT tree stands at
// the roots it was seeded with. Every occupancy-derived reading then describes an EMPTY
// triangulation: nothing is saturated, nothing is demanded, nothing is starved — and the report
// read "HEALTHY: the triangulation refines to TargetPixelError" for a terrain drawing nothing.
// ---------------------------------------------------------------------------

// Root counts are domain constants: kRootHalfedgeCount (planar), kSphereRootCount (spherical).
CBTTessellationStats RootedStats(uint32_t live, uint32_t roots, int32_t splitDemand = 0,
                                 int32_t splitServed = 0)
{
    CBTTessellationStats st = Stats(live, splitDemand, splitServed, 0, 0);
    st.RootCount = roots;
    return st;
}

// The exact state the proof arm recorded: an 8 km terrain skipped before it ever streamed,
// liveCount 2 against a 1,048,576-slot pool.
TEST(CBTPoolHealthTest, APlanarTreeStandingAtItsRootsIsInertNotHealthy)
{
    const CBTPoolHealth h = DiagnoseCBTPool(RootedStats(2u, 2u), kSkipped);

    EXPECT_TRUE(h.Inert) << "two live bisectors IS the seeded pair — nothing refined";
    EXPECT_FALSE(h.Saturated);
    EXPECT_FALSE(h.SplitsStarved);
    EXPECT_EQ(h.State, CBTPoolState::Inert);
    EXPECT_NE(h.State, CBTPoolState::Healthy)
        << "a terrain drawing no geometry must never report healthy";
}

TEST(CBTPoolHealthTest, ASphericalTreeStandingAtIts24RootsIsAlsoInert)
{
    EXPECT_EQ(DiagnoseCBTPool(RootedStats(24u, 24u), kSkipped).State, CBTPoolState::Inert);
    // 24 live on a PLANAR terrain (2 roots) is refinement, not inertness — the root count is
    // what makes the reading domain-correct rather than a magic threshold.
    EXPECT_EQ(DiagnoseCBTPool(RootedStats(24u, 2u), kPublished).State, CBTPoolState::Healthy);
}

// A published terrain at its roots draws them: flat ground the content-aware split keeps whole
// reads that way on every frame, and must not be told it draws nothing.
TEST(CBTPoolHealthTest, APublishedTreeAtItsRootsIsAtRootsNotInert)
{
    const CBTPoolHealth h = DiagnoseCBTPool(RootedStats(2u, 2u), kPublished);

    EXPECT_FALSE(h.Inert);
    EXPECT_EQ(h.State, CBTPoolState::AtRoots);
}

// DISCRIMINATOR: the converged live pose must stay Healthy, or the new state is just noise.
TEST(CBTPoolHealthTest, AConvergedTreeIsNotInert)
{
    const CBTPoolHealth h = DiagnoseCBTPool(RootedStats(340612u, 2u), kPublished);

    EXPECT_FALSE(h.Inert);
    EXPECT_EQ(h.State, CBTPoolState::Healthy);
}

// An inert tree that is ASKING to split and getting nothing is described better by the refusal.
TEST(CBTPoolHealthTest, StarvationOutranksInertnessBecauseItNamesTheCause)
{
    const CBTPoolHealth h = DiagnoseCBTPool(RootedStats(2u, 2u, /*splitDemand*/ 512, 0), kSkipped);

    EXPECT_TRUE(h.Inert) << "the flag still reports the emptiness";
    EXPECT_TRUE(h.SplitsStarved);
    EXPECT_EQ(h.State, CBTPoolState::SplitsStarvedWithHeadroom);
}

// Inertness is unknowable without the root count, and a guess would misfire on every caller
// that has not been migrated.
TEST(CBTPoolHealthTest, AnUnpopulatedRootCountIsNeverGuessedFromLiveCount)
{
    const CBTTessellationStats st = Stats(2u, 0, 0, 0, 0);
    ASSERT_EQ(st.RootCount, 0u) << "the base helper deliberately leaves it unset";

    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);
    EXPECT_FALSE(h.Inert);
    EXPECT_EQ(h.State, CBTPoolState::Healthy);
}

TEST(CBTPoolHealthTest, EmptyPoolReportsZeroOccupancyRatherThanDividingByZero)
{
    CBTTessellationStats st;
    st.PoolSize = 0u;
    const CBTPoolHealth h = DiagnoseCBTPool(st, kPublished);

    EXPECT_DOUBLE_EQ(h.Occupancy, 0.0);
    EXPECT_EQ(h.State, CBTPoolState::Healthy);
}
