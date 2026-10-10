#include "UI/Controls/VirtualWindowCore.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::UI
{

namespace
{
// The one ring mod. localLine may briefly exceed desired during shift
// arithmetic; offsets are always kept in [0, desired).
int RingMod(int value, int desired)
{
    if (desired <= 0)
        return 0;
    int m = value % desired;
    if (m < 0)
        m += desired;
    return m;
}
} // namespace

int VirtualWindowCore::FirstFromUniform(float scrollY, float lineH, int lineCount)
{
    if (lineH <= 0.0f || lineCount <= 0)
        return 0;
    const int first = static_cast<int>(std::floor(scrollY / lineH));
    return std::clamp(first, 0, lineCount - 1);
}

int VirtualWindowCore::FirstFromPrefixSum(std::span<const int> cumulativeYPx, int scrollYPx, int count)
{
    if (count <= 0 || cumulativeYPx.empty())
        return 0;
    // cumulativeYPx[i] = y offset of the START of item i; a trailing total
    // entry (size == count + 1) is tolerated. First visible = last item whose
    // start is <= scrollY.
    const size_t n = std::min(static_cast<size_t>(count), cumulativeYPx.size());
    auto it = std::upper_bound(cumulativeYPx.begin(), cumulativeYPx.begin() + n, scrollYPx);
    const int idx = static_cast<int>(it - cumulativeYPx.begin()) - 1;
    return std::clamp(idx, 0, count - 1);
}

int VirtualWindowCore::SlotFor(int localLine) const
{
    return RingMod(localLine + m_RingOffset, m_Last.Desired > 0 ? m_Last.Desired : 1);
}

void VirtualWindowCore::Reset()
{
    m_Last = Guard{};
    m_RingOffset = 0;
    m_StableUpdates = 0;
}

VirtualWindowCore::Impact VirtualWindowCore::FullRebind(const Guard& g, int lanes, Host& host)
{
    // A full pass owns the whole window: reset the ring so slot order is
    // DFS-stable again (matches the views' full-refresh behavior).
    m_RingOffset = 0;
    Impact impact = Impact::None;
    for (int local = 0; local < g.Desired; ++local)
    {
        for (int lane = 0; lane < lanes; ++lane)
        {
            const int slot = local * lanes + lane;
            const int item = (g.First + local) * lanes + lane;
            if (item < g.ItemCount)
                impact |= host.Rebind(slot, item);
            else
                host.UnbindSlot(slot);
        }
    }
    // Slots beyond the window (retained after the window — or the lane
    // count — shrank, before the pool physically shrinks) would otherwise
    // keep showing stale rows that reappear as duplicates after a scroll.
    // A full pass owns the whole pool, bounded by the ACTUAL slot count
    // (lane changes resize the pool without moving the line count).
    const int poolSlots = host.SlotCount();
    for (int slot = g.Desired * lanes; slot < poolSlots; ++slot)
        host.UnbindSlot(slot);
    return impact;
}

VirtualWindowCore::Impact VirtualWindowCore::Update(const Guard& next,
                                                    VirtualizationCoordinator::Reason reason,
                                                    int lanes,
                                                    const ChangeSetView& changes,
                                                    Host& host,
                                                    bool allowShrink)
{
    lanes = std::max(1, lanes);

    if (next.Desired <= 0 || next.ItemCount <= 0)
    {
        // Empty window: unbind whatever the pool still shows and forget the
        // ring. Pool slots are kept (grow-only between shrink cycles).
        const int poolSlots = host.SlotCount();
        for (int slot = 0; slot < poolSlots; ++slot)
            host.UnbindSlot(slot);
        m_Last = next;
        m_RingOffset = 0;
        m_StableUpdates = 0;
        return Impact::None;
    }

    Impact impact = Impact::None;

    // Pool ensure (grow-only). Growth is a topology change: new elements
    // exist that the next solve must lay out. Compared in flat SLOTS, not
    // lines: GridView's lane count changes independently of the line count.
    if (next.Desired * lanes > host.SlotCount())
    {
        host.EnsurePool(next.Desired * lanes);
        impact |= Impact::Topology;
    }

    const bool guardEqual = (next == m_Last);

    if (guardEqual)
    {
        switch (changes.ChangeKind)
        {
        case ChangeSetView::Kind::None:
        {
            // Steady state: safety-net validation. Repairs report impact
            // (TreeView semantics — a silent repair hides real staleness).
            bool repaired = false;
            for (int local = 0; local < next.Desired; ++local)
            {
                for (int lane = 0; lane < lanes; ++lane)
                {
                    const int slot = SlotFor(local) * lanes + lane;
                    const int item = (next.First + local) * lanes + lane;
                    if (item >= next.ItemCount)
                        continue;
                    if (!host.SlotBinds(slot, item))
                    {
                        impact |= host.Rebind(slot, item);
                        repaired = true;
                    }
                }
            }
            if (repaired)
                impact |= Impact::Rebind;
            // Sub-line scroll events keep the guard equal but mean the user
            // is mid-gesture — advancing the shrink counter there lets a
            // slow scrollbar drag trigger element destruction mid-drag (the
            // old ListView reset its counter on any pointer movement).
            if (reason != VirtualizationCoordinator::Reason::ScrollChanged)
                ++m_StableUpdates;
            break;
        }
        case ChangeSetView::Kind::Subset:
        {
            // Paint-only subset: rebind exactly the affected slots with the
            // item they currently represent (derived from the ring inverse).
            for (int slot : changes.AffectedSlots)
            {
                if (slot < 0 || slot >= host.SlotCount())
                    continue;
                const int lineSlot = slot / lanes;
                const int lane = slot % lanes;
                const int local = RingMod(lineSlot - m_RingOffset, next.Desired);
                const int item = (next.First + local) * lanes + lane;
                if (item < next.ItemCount)
                    impact |= host.Rebind(slot, item) | Impact::Rebind;
            }
            m_StableUpdates = 0;
            break;
        }
        case ChangeSetView::Kind::All:
            impact |= FullRebind(next, lanes, host) | Impact::Rebind;
            m_StableUpdates = 0;
            break;
        }
    }
    else
    {
        // Ring-shift eligibility: only the window position moved — same
        // window size, item count, structure, and measure-relevant extents.
        Guard positionOnly = next;
        positionOnly.First = m_Last.First;
        const bool ringEligible = m_Last.Desired > 0 && positionOnly == m_Last &&
                                  changes.ChangeKind == ChangeSetView::Kind::None;
        const int delta = next.First - m_Last.First;

        if (ringEligible && delta != 0 && std::abs(delta) < next.Desired)
        {
            // Slots keep their bindings; only entering lines rebind.
            m_RingOffset = RingMod(m_RingOffset + delta, next.Desired);
            const int enterBegin = delta > 0 ? next.Desired - delta : 0;
            const int enterEnd = delta > 0 ? next.Desired : -delta;
            for (int local = enterBegin; local < enterEnd; ++local)
            {
                for (int lane = 0; lane < lanes; ++lane)
                {
                    const int slot = SlotFor(local) * lanes + lane;
                    const int item = (next.First + local) * lanes + lane;
                    if (item < next.ItemCount)
                        impact |= host.Rebind(slot, item);
                    else
                        host.UnbindSlot(slot);
                }
            }
            impact |= Impact::Rebind;
        }
        else
        {
            impact |= FullRebind(next, lanes, host) | Impact::Rebind;
        }
        m_StableUpdates = 0;
    }

    // Shrink hysteresis (shared ListView policy): only after a long run of
    // fully-stable updates, and only down to Desired + slack. Shrinking
    // culls tail slots, so the ring must be identity first — reached via a
    // full rebind of the (stable) window. Blocked while the caller is in
    // event dispatch (allowShrink) — destroying live elements there is
    // unsafe; the counter keeps advancing so the shrink fires on the next
    // safe update. Bounded by the actual slot count so pools retained
    // across a view-side Reset() still shrink.
    const int targetSlots = (next.Desired + kShrinkSlackLines) * lanes;
    if (allowShrink && m_StableUpdates >= kShrinkStableUpdates &&
        host.SlotCount() > targetSlots)
    {
        if (m_RingOffset != 0)
            impact |= FullRebind(next, lanes, host) | Impact::Rebind;
        const int removeSlots = host.SlotCount() - targetSlots;
        for (int i = 0; i < removeSlots; ++i)
            host.DestroyTailSlot();
        m_StableUpdates = 0;
        impact |= Impact::Topology;
    }

    m_Last = next;
    return impact;
}

} // namespace GameEngine::UI
