/**
 * @file PrevVisibleResetTracker.cpp
 * @brief Implementation of the per-view HZB prevVisible reset pending sets.
 */

#include "Rendering/Core/PrevVisibleResetTracker.h"

#include <algorithm>
#include <iterator>

namespace GameEngine
{
namespace Rendering
{

void PrevVisibleResetTracker::OnSlotsRecycled(std::vector<uint32_t>& recycledSlots)
{
    if (recycledSlots.empty() || m_Pending.empty())
        return;

    std::sort(recycledSlots.begin(), recycledSlots.end());
    recycledSlots.erase(std::unique(recycledSlots.begin(), recycledSlots.end()), recycledSlots.end());

    std::vector<uint32_t> merged;
    for (auto& [viewId, pending] : m_Pending)
    {
        (void)viewId;
        if (pending.empty())
        {
            // Fast path — the common case for a view flushed last frame.
            pending = recycledSlots;
            continue;
        }
        merged.clear();
        merged.reserve(pending.size() + recycledSlots.size());
        std::set_union(pending.begin(), pending.end(), recycledSlots.begin(), recycledSlots.end(),
                       std::back_inserter(merged));
        pending.swap(merged);
    }
}

void PrevVisibleResetTracker::OnBufferInitialized(ViewId viewId)
{
    // Registers the view (empty) or clears an existing pending set — the
    // first-touch full-buffer 0xFFFFFFFF init supersedes anything queued.
    m_Pending[viewId].clear();
}

void PrevVisibleResetTracker::TakeViewPending(ViewId viewId, std::vector<uint32_t>& out)
{
    out.clear();
    auto it = m_Pending.find(viewId);
    if (it == m_Pending.end())
        return;
    out.swap(it->second); // it->second becomes empty; the view stays tracked
}

std::size_t PrevVisibleResetTracker::PendingCount(ViewId viewId) const
{
    auto it = m_Pending.find(viewId);
    return it == m_Pending.end() ? 0u : it->second.size();
}

void PrevVisibleResetTracker::CoalesceResetRuns(
    std::vector<uint32_t>& slots, std::vector<std::pair<uint32_t, uint32_t>>& outRuns)
{
    outRuns.clear();
    if (slots.empty())
        return;
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

    // Merge consecutive indices into [startElement, countElements) runs so a
    // clustered eviction (spatially/temporally adjacent slots) becomes a handful
    // of FillBuffer ranges instead of one per slot.
    uint32_t runStart = slots[0];
    uint32_t runLen = 1u;
    for (size_t i = 1; i < slots.size(); ++i)
    {
        if (slots[i] == runStart + runLen)
        {
            ++runLen;
        }
        else
        {
            outRuns.emplace_back(runStart, runLen);
            runStart = slots[i];
            runLen = 1u;
        }
    }
    outRuns.emplace_back(runStart, runLen);
}

} // namespace Rendering
} // namespace GameEngine
