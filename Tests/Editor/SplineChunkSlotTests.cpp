#include "Placement/SplineChunkSlots.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using Editor::CarveChunkRanges;
using Editor::ChunkSlotAction;
using Editor::ChunkSlotPlan;
using Editor::ReconcileChunkSlots;

namespace
{

// Cumulative distances for `count` stations at a uniform step, starting at 0 —
// the shape SampleUniform hands the extrude rebuild.
std::vector<float32> UniformRun(uint32 count, float32 stepMetres)
{
    std::vector<float32> distance(count, 0.0f);
    for (uint32 i = 1; i < count; ++i)
        distance[i] = distance[i - 1u] + stepMetres;
    return distance;
}

// Every station belongs to exactly one range except the shared boundaries:
// ranges start at 0, chain first==previous.last, and end on the final station.
void ExpectFullCoverage(const std::vector<std::pair<uint32, uint32>>& ranges, uint32 stationCount)
{
    ASSERT_FALSE(ranges.empty());
    EXPECT_EQ(ranges.front().first, 0u) << "coverage must start at the first station";
    EXPECT_EQ(ranges.back().second, stationCount - 1u) << "coverage must reach the last station";
    for (size_t c = 0; c + 1u < ranges.size(); ++c)
        EXPECT_EQ(ranges[c].second, ranges[c + 1u].first)
            << "chunks " << c << " and " << c + 1 << " must share their boundary station";
    for (const auto& [first, last] : ranges)
        EXPECT_LT(first, last) << "a range needs two stations to sweep a strip";
}

} // namespace

// The RiverVignette water run: ~247.6 m of stations at the 0.5 m drape step.
// Five chunks at the 50 m pitch, no head hole, no tail gap — the run's first
// station is distance 0 and its last is the full arc length.
TEST(CarveChunkRanges, ARiverLengthRunIsFullyCoveredInFiveChunks)
{
    const std::vector<float32> distance = UniformRun(497u, 247.5864f / 496.0f);

    const auto ranges = CarveChunkRanges(distance, 50.0f, 200u);

    EXPECT_EQ(ranges.size(), 5u);
    ExpectFullCoverage(ranges, 497u);
    // Every non-final chunk spans at least the pitch; the remainder chunk
    // carries the tail.
    for (size_t c = 0; c + 1u < ranges.size(); ++c)
        EXPECT_GE(distance[ranges[c].second] - distance[ranges[c].first], 50.0f);
}

TEST(CarveChunkRanges, ARunShorterThanOneChunkIsOneRange)
{
    const std::vector<float32> distance = UniformRun(20u, 0.5f); // 9.5 m

    const auto ranges = CarveChunkRanges(distance, 50.0f, 200u);

    ASSERT_EQ(ranges.size(), 1u);
    ExpectFullCoverage(ranges, 20u);
}

TEST(CarveChunkRanges, FewerThanTwoStationsYieldNoRanges)
{
    EXPECT_TRUE(CarveChunkRanges({}, 50.0f, 200u).empty());
    const float32 one[] = {0.0f};
    EXPECT_TRUE(CarveChunkRanges(one, 50.0f, 200u).empty());
}

// The cap folds the remainder into the final range: a capped run is coarser at
// its tail, never shorter than the spline.
TEST(CarveChunkRanges, TheChunkCapFoldsTheTailInsteadOfDroppingIt)
{
    const std::vector<float32> distance = UniformRun(101u, 1.0f); // 100 m

    const auto ranges = CarveChunkRanges(distance, 10.0f, 4u);

    ASSERT_EQ(ranges.size(), 4u);
    ExpectFullCoverage(ranges, 101u);
    EXPECT_GT(distance[ranges.back().second] - distance[ranges.back().first], 10.0f)
        << "the final range must absorb everything past the cap";
}

// Stations that do not advance (a doubled-back drape) never split a chunk:
// only walked distance does.
TEST(CarveChunkRanges, StalledDistanceDoesNotSplitAChunk)
{
    std::vector<float32> distance = UniformRun(30u, 1.0f);
    for (uint32 i = 10; i < 20; ++i)
        distance[i] = distance[9]; // ten stations pinned at 9 m
    for (uint32 i = 20; i < 30; ++i)
        distance[i] = distance[19] + static_cast<float32>(i - 19);

    const auto ranges = CarveChunkRanges(distance, 12.0f, 200u);

    ExpectFullCoverage(ranges, 30u);
    for (size_t c = 0; c + 1u < ranges.size(); ++c)
        EXPECT_GE(distance[ranges[c].second] - distance[ranges[c].first], 12.0f);
}

// THE case the by-index addressing exists for: an empty chunk in the middle of
// the run must leave a hole, not shift its neighbour's mesh into its slot. An
// appended (compacted) assignment would bind chunk 2 to chunk 1's mesh key and
// strand the displaced key with nothing to release it.
TEST(SplineChunkSlots, AMidRunHoleLeavesItsSlotEmptyAndItsNeighboursInPlace)
{
    const uint8 live[] = {1, 1, 1};
    const uint8 geometry[] = {1, 0, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, geometry);

    ASSERT_EQ(plan.Slots.size(), 3u);
    EXPECT_EQ(plan.ChunkCount, 3u);
    EXPECT_EQ(plan.Slots[0], ChunkSlotAction::Update);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Retire) << "the empty stretch must leave a hole";
    EXPECT_EQ(plan.Slots[2], ChunkSlotAction::Update)
        << "the chunk after the hole must keep its own slot";
}

TEST(SplineChunkSlots, AShrunkenRunRetiresItsTail)
{
    const uint8 live[] = {1, 1, 1, 1};
    const uint8 geometry[] = {1, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, geometry);

    ASSERT_EQ(plan.Slots.size(), 4u);
    EXPECT_EQ(plan.ChunkCount, 2u);
    EXPECT_EQ(plan.Slots[0], ChunkSlotAction::Update);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Update);
    EXPECT_EQ(plan.Slots[2], ChunkSlotAction::Retire);
    EXPECT_EQ(plan.Slots[3], ChunkSlotAction::Retire);
}

TEST(SplineChunkSlots, AGrownRunCreatesTheNewTrailingSlots)
{
    const uint8 live[] = {1};
    const uint8 geometry[] = {1, 1, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, geometry);

    ASSERT_EQ(plan.Slots.size(), 3u);
    EXPECT_EQ(plan.Slots[0], ChunkSlotAction::Update);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Create);
    EXPECT_EQ(plan.Slots[2], ChunkSlotAction::Create);
}

// A scene reload can kill a chunk entity while the slot's mesh key survives in
// the registry: the slot is recreated in place, not shifted or leaked.
TEST(SplineChunkSlots, ADeadEntityIsRecreatedInItsOwnSlot)
{
    const uint8 live[] = {1, 0, 1};
    const uint8 geometry[] = {1, 1, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, geometry);

    ASSERT_EQ(plan.Slots.size(), 3u);
    EXPECT_EQ(plan.Slots[0], ChunkSlotAction::Update);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Create);
    EXPECT_EQ(plan.Slots[2], ChunkSlotAction::Update);
}

// A hole that persists across rebuilds keeps reading Retire; the executor's
// retire of an already-empty slot is a no-op, never a re-armed rebuild.
TEST(SplineChunkSlots, AHoleOverAnAlreadyEmptySlotStaysAHole)
{
    const uint8 live[] = {1, 0, 1};
    const uint8 geometry[] = {1, 0, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, geometry);

    ASSERT_EQ(plan.Slots.size(), 3u);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Retire);
    EXPECT_EQ(plan.Slots[2], ChunkSlotAction::Update);
}

TEST(SplineChunkSlots, NoChunksAtAllRetiresEverySlot)
{
    const uint8 live[] = {1, 1};

    const ChunkSlotPlan plan = ReconcileChunkSlots(live, {});

    ASSERT_EQ(plan.Slots.size(), 2u);
    EXPECT_EQ(plan.ChunkCount, 0u);
    EXPECT_EQ(plan.Slots[0], ChunkSlotAction::Retire);
    EXPECT_EQ(plan.Slots[1], ChunkSlotAction::Retire);
}
