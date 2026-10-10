// SwapGenerationGuardTest.cpp — lock tests for the shared consumer-cadence
// guard (ECS/SwapGenerationGuard.h) and the ComponentDirtyFeed swap
// generation it consumes. The guard's arithmetic must match the bespoke
// consumer code it replaced exactly: compare-then-assign, gap > 1 (not
// >= 1), unsigned wrap on source restart. uint64 wraparound of the
// generation itself is unreachable at one swap per frame and is deliberately
// unhandled.

#include <gtest/gtest.h>

#include "ECS/ComponentDirtyFeed.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SwapGenerationGuard.h"
#include "ECS/World.h"
#include "TestComponents.h"

#include <vector>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace
{
constexpr uint64 kWorldA = 1;
constexpr uint64 kWorldB = 2;
} // namespace

// gap 1 = normal cadence; gap 0 = multiple consumer steps within one window
// (editor passive-refresh re-runs the waves between swaps). Neither is a
// missed window.
TEST(SwapGenerationGuard, NormalCadenceAndMultiStepReportNoMiss)
{
    SwapGenerationGuard guard;
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 1)); // first window
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 2)); // gap 1
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 2)); // gap 0 (multi-step)
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 3)); // gap 1
}

// gap > 1 reports the miss exactly once; the following normal window resumes
// without a second recovery.
TEST(SwapGenerationGuard, GapReportsMissOnceThenResumes)
{
    SwapGenerationGuard guard;
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 1));
    EXPECT_TRUE(guard.ConsumeAndCheckMissed(kWorldA, 3));  // gap 2: one window unseen
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 4)); // recovered, gap 1
    EXPECT_TRUE(guard.ConsumeAndCheckMissed(kWorldA, 9));  // gap 5
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 10));
}

// Documented first-call contract: a fresh guard observing generation 0 or 1
// does NOT report a miss (1 - 0 == 1). All call sites are constructed before
// their source's first swap; first-window recovery is guaranteed by other
// means (first-tick reconcile, lazy insert, unprimed caches), never by this
// predicate. Generation 2+ on a fresh guard IS a miss: the source ran at
// least one whole window before the consumer's first run.
TEST(SwapGenerationGuard, FreshGuardLowGenerationsReportNoMiss)
{
    SwapGenerationGuard atZero;
    EXPECT_FALSE(atZero.ConsumeAndCheckMissed(kWorldA, 0));

    SwapGenerationGuard atOne;
    EXPECT_FALSE(atOne.ConsumeAndCheckMissed(kWorldA, 1));

    SwapGenerationGuard atTwo;
    EXPECT_TRUE(atTwo.ConsumeAndCheckMissed(kWorldA, 2));
}

// A source restart (generation below the cached value) reads as a huge gap
// under unsigned arithmetic => recover.
TEST(SwapGenerationGuard, SourceRestartBelowCachedRecovers)
{
    SwapGenerationGuard guard;
    EXPECT_TRUE(guard.ConsumeAndCheckMissed(kWorldA, 100)); // fresh vs long-running source
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 101));
    EXPECT_TRUE(guard.ConsumeAndCheckMissed(kWorldA, 5)); // restart: wrap = huge gap
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 6));
}

// Generations from different worlds are not comparable: a world switch
// re-baselines the guard as freshly constructed against the new source — an
// already-running world reports a miss (its windows were never seen); a
// just-born world gets the documented first-call semantics.
TEST(SwapGenerationGuard, WorldSwitchRebaselinesPairing)
{
    SwapGenerationGuard guard;
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 1));
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldA, 2));
    // Same generation value, different world: without the pairing this would
    // read as gap 0 and silently trust a source the consumer never consumed.
    EXPECT_TRUE(guard.ConsumeAndCheckMissed(kWorldB, 2));
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(kWorldB, 3));

    SwapGenerationGuard guard2;
    EXPECT_TRUE(guard2.ConsumeAndCheckMissed(kWorldA, 7));  // fresh vs running source
    EXPECT_FALSE(guard2.ConsumeAndCheckMissed(kWorldB, 1)); // just-born world: first-call rule
}

// ---------------------------------------------------------------------------
// ComponentDirtyFeed swap generation — the guard's dirty-feed source.
// ---------------------------------------------------------------------------

TEST(DirtyFeedSwapGeneration, SwapIncrementsMonotonically)
{
    ComponentDirtyFeed feed;
    EXPECT_EQ(feed.SwapGeneration(), 0u);
    feed.Swap();
    EXPECT_EQ(feed.SwapGeneration(), 1u);
    feed.Swap();
    feed.Swap();
    EXPECT_EQ(feed.SwapGeneration(), 3u);
}

// Reset() wipes entries but NOT the generation: a World::Clear() faking an
// unbroken cadence would hide a real gap from a consumer paused across it.
TEST(DirtyFeedSwapGeneration, ResetPreservesGeneration)
{
    ComponentDirtyFeed feed;
    feed.Swap();
    feed.Swap();
    ASSERT_EQ(feed.SwapGeneration(), 2u);
    feed.Reset();
    EXPECT_EQ(feed.SwapGeneration(), 2u);
    feed.Swap();
    EXPECT_EQ(feed.SwapGeneration(), 3u);
}

TEST(DirtyFeedSwapGeneration, WorldClearPreservesGeneration)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());
    world.SwapComponentDirtyFeed();
    world.SwapComponentDirtyFeed();
    ASSERT_EQ(world.GetComponentDirtyFeed().SwapGeneration(), 2u);

    world.CreateHandle(Position{1, 0, 0});
    world.Clear(); // wipes feed entries; the generation must survive
    EXPECT_EQ(world.GetComponentDirtyFeed().SwapGeneration(), 2u);
    world.SwapComponentDirtyFeed();
    EXPECT_EQ(world.GetComponentDirtyFeed().SwapGeneration(), 3u);
}

// End-to-end pause shape over a real world feed (the dirty-feed twin of
// LifecycleEvents.SwapGenerationDetectsMissedWindows): a consumer idle across
// one whole window observes the miss, and the entry is provably gone from
// the snapshot; normal cadence resumes afterwards.
TEST(DirtyFeedSwapGeneration, PauseGapObservableThroughGuard)
{
    World world;
    world.EnableComponentDirtyFeed(GetComponentTypeId<Position>());
    SwapGenerationGuard guard;

    auto e = world.CreateHandle(Position{1, 0, 0});
    world.AddComponentImmediate<Position>(e, Position{2, 0, 0}); // emits
    world.SwapComponentDirtyFeed();
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(
        world.GetWorldId(), world.GetComponentDirtyFeed().SwapGeneration()));
    {
        std::vector<EntityHandle> snapshot;
        world.GetComponentDirtyFeed().Snapshot(snapshot);
        EXPECT_FALSE(snapshot.empty()); // readable at normal cadence
    }

    // Consumer pauses; a write lands and two swaps tick by unseen.
    world.AddComponentImmediate<Position>(e, Position{3, 0, 0});
    world.SwapComponentDirtyFeed(); // promoted — unseen
    world.SwapComponentDirtyFeed(); // discarded

    EXPECT_TRUE(guard.ConsumeAndCheckMissed(
        world.GetWorldId(), world.GetComponentDirtyFeed().SwapGeneration()));
    {
        std::vector<EntityHandle> snapshot;
        world.GetComponentDirtyFeed().Snapshot(snapshot);
        EXPECT_TRUE(snapshot.empty()); // the entry is gone, provably
    }

    // Normal cadence resumes: gap 1, no recovery.
    world.SwapComponentDirtyFeed();
    EXPECT_FALSE(guard.ConsumeAndCheckMissed(
        world.GetWorldId(), world.GetComponentDirtyFeed().SwapGeneration()));
}
