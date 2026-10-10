// ECS::OrderedHandleSet: a dirty-feed snapshot made unique and ordered by
// entity index in O(n), whatever order its producers appended in.

#include <gtest/gtest.h>

#include "ECS/OrderedHandleSet.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <random>
#include <vector>

namespace GameEngine::ECS
{
// gtest prints a handle as its index and version rather than through its bool conversion.
void PrintTo(const EntityHandle& handle, std::ostream* os)
{
    *os << "{index " << handle.index << ", version " << handle.version << "}";
}
} // namespace GameEngine::ECS

using namespace GameEngine::ECS;

namespace
{

EntityHandle Handle(uint32_t index, uint32_t version)
{
    return EntityHandle(static_cast<EntityIndex>(index), static_cast<EntityVersion>(version));
}

// What the ordering must equal: the unique handles, by index then id.
std::vector<EntityHandle> Reference(std::vector<EntityHandle> handles)
{
    std::sort(handles.begin(), handles.end(), [](EntityHandle a, EntityHandle b)
              { return a.index != b.index ? a.index < b.index : a.id < b.id; });
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
    return handles;
}

} // namespace

// The counter: a snapshot in the order a parallel producer writes it (chunks
// finishing out of order, two windows concatenated) is ordered with no
// comparison sort at all.
TEST(OrderedHandleSet, ScatteredSnapshotOrdersWithoutAComparisonSort)
{
    constexpr uint32_t kEntities = 100'000;
    constexpr uint32_t kChunk = 512;
    std::vector<EntityHandle> snapshot;
    for (int window = 0; window < 2; ++window)
        for (uint32_t chunk = kEntities / kChunk + 1; chunk-- > 0;)
            for (uint32_t i = chunk * kChunk; i < std::min(kEntities, (chunk + 1) * kChunk); ++i)
                snapshot.push_back(Handle(i, 1));

    OrderedHandleSet set;
    set.OrderUnique(snapshot, kEntities);
    ASSERT_EQ(snapshot.size(), kEntities);
    for (uint32_t i = 0; i < kEntities; ++i)
        ASSERT_EQ(snapshot[i].index, i) << "position " << i;
    EXPECT_EQ(set.GetComparisonSortedCountForTesting(), 0u);
}

// Parity: random snapshots with repeats, indices reused within the window (a
// second version of the same index) and stale handles past the bound give
// exactly the unique handles, by index then id. One instance serves every
// round, so an entry leaking from one call into the next fails a later round.
TEST(OrderedHandleSet, MatchesASortedUniqueSetOnRandomSnapshots)
{
    constexpr uint32_t kBound = 5'000;
    std::mt19937 rng(3355u);
    std::uniform_int_distribution<uint32_t> index(0, kBound - 1);
    std::uniform_int_distribution<int> kind(0, 19);
    OrderedHandleSet set;
    for (int round = 0; round < 20; ++round)
    {
        std::vector<EntityHandle> snapshot;
        for (int i = 0; i < 8'000; ++i)
        {
            const int k = kind(rng);
            if (k == 0)
                snapshot.push_back(Handle(kBound + index(rng) % 64u, 1)); // past the bound
            else if (k == 1)
                snapshot.push_back(Handle(index(rng), 2)); // a reused index
            else
                snapshot.push_back(Handle(index(rng), 1));
        }
        // Alternate the bound, as a world that shrinks and grows does: below
        // the scratch's capacity, handles past the bound still take the
        // comparison-sorted path.
        const uint32_t bound = round % 2 == 0 ? kBound : kBound / 2;
        const std::vector<EntityHandle> expected = Reference(snapshot);
        set.OrderUnique(snapshot, bound);
        ASSERT_EQ(snapshot, expected) << "round " << round;
        EXPECT_GT(set.GetComparisonSortedCountForTesting(), 0u) << "the round exercised no reuse or stale handle";
    }
}
