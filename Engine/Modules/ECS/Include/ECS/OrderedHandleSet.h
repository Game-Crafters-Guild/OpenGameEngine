#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ECS/ECS.h"

namespace GameEngine::ECS
{

/// Orders a list of entity handles by entity index and removes repeats in
/// O(n + indexBound / 64), whatever order the handles arrive in.
///
/// For consumers of the component dirty feed: its producers append in their
/// own orders (a parallel pass appends chunk by chunk as each one finishes,
/// and a snapshot concatenates two windows), so a comparison sort's cost
/// depends on who wrote this frame. This pass costs the same either way.
///
/// The order is by entity index, then by id. An index that appears with two
/// ids (destroyed and reused within the window) and a handle at or past
/// `indexBound` (stale, from a shrunk world) are both kept; they are the only
/// entries that go through a comparison sort, and there are normally none.
///
/// Holds its scratch between calls, sized to the largest index bound seen:
/// one instance per consumer, used from one thread at a time.
class OrderedHandleSet
{
public:
    /// Reorders `handles` in place: unique, by entity index, then id.
    void OrderUnique(std::vector<EntityHandle>& handles, std::size_t indexBound);

    /// Entries the last call ordered with a comparison sort (reused indices
    /// and handles past the bound). Test seam.
    std::size_t GetComparisonSortedCountForTesting() const { return m_ComparisonSorted; }

private:
    std::vector<uint32_t> m_SeenId;      // per index: the first id seen this call (valid where its bit is set)
    std::vector<uint64_t> m_Present;     // one bit per index seen this call; all zero between calls
    std::vector<EntityHandle> m_Extra;   // the comparison-sorted remainder
    std::size_t m_ComparisonSorted = 0;
};

} // namespace GameEngine::ECS
