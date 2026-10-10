#include "UI/SlotAllocator.h"

#include <bit>
#include <cassert>

namespace GameEngine
{
namespace UI
{

size_t SlotAllocator::SizeClassIndex(uint16_t count)
{
    if (count <= 1)
        return 0;
    if (count > kMaxSmallCap)
        return kNumSizeClasses;
    return static_cast<size_t>(std::bit_width(static_cast<uint16_t>(count - 1)));
}

uint16_t SlotAllocator::SizeClassCap(size_t classIdx)
{
    assert(classIdx < kNumSizeClasses);
    return static_cast<uint16_t>(1u << classIdx);
}

uint32_t SlotAllocator::Allocate(uint16_t count, uint16_t& outCap)
{
    assert(count > 0);

    if (count > kMaxSmallCap)
    {
        // Large: linear first-fit. Caps are exact, no rounding waste.
        for (size_t i = 0; i < m_LargeFreeList.size(); ++i)
        {
            if (m_LargeFreeList[i].Cap >= count)
            {
                LargeRange r = m_LargeFreeList[i];
                m_LargeFreeList[i] = m_LargeFreeList.back();
                m_LargeFreeList.pop_back();
                outCap = r.Cap;
                m_UsedSlots += r.Cap;
                m_FreeSlots -= r.Cap;
                return r.Start;
            }
        }
        const uint32_t start = m_TotalSlots;
        outCap = count;
        m_TotalSlots += count;
        m_UsedSlots += count;
        return start;
    }

    const size_t startClass = SizeClassIndex(count);

    // Walk up size classes; pop from the first non-empty bucket. Walk-up
    // recycles stranded entries from larger classes when smaller-class
    // demand starves; without it, larger holes accumulate forever (the
    // no-walk-up variant was tested in Stage 0 and made fragmentation
    // strictly worse — 0.24 vs 0.20).
    for (size_t i = startClass; i < kNumSizeClasses; ++i)
    {
        if (!m_FreeLists[i].empty())
        {
            const uint32_t start = m_FreeLists[i].back();
            m_FreeLists[i].pop_back();
            const uint16_t cap = SizeClassCap(i);
            outCap = cap;
            m_UsedSlots += cap;
            m_FreeSlots -= cap;
            return start;
        }
    }

    // All small classes empty: extend high-water by exact size class.
    const uint16_t cap = SizeClassCap(startClass);
    const uint32_t start = m_TotalSlots;
    outCap = cap;
    m_TotalSlots += cap;
    m_UsedSlots += cap;
    return start;
}

void SlotAllocator::Free(uint32_t start, uint16_t cap)
{
    assert(cap > 0);
    assert(start + cap <= m_TotalSlots);

    if (cap > kMaxSmallCap)
    {
        m_LargeFreeList.push_back({start, cap});
    }
    else
    {
        const size_t idx = SizeClassIndex(cap);
        // cap must be a valid size-class boundary — caller passes back what
        // Allocate or TryGrowInPlace returned.
        assert(SizeClassCap(idx) == cap);
        m_FreeLists[idx].push_back(start);
    }
    m_UsedSlots -= cap;
    m_FreeSlots += cap;
}

uint16_t SlotAllocator::TryGrowInPlace(uint32_t start, uint16_t oldCap, uint16_t newCount)
{
    assert(newCount > oldCap);

    // Tail-only: anything else requires the caller to relocate.
    if (start + oldCap != m_TotalSlots)
        return 0;

    uint16_t newCap;
    if (newCount > kMaxSmallCap)
        newCap = newCount;
    else
        newCap = SizeClassCap(SizeClassIndex(newCount));

    const uint32_t delta = static_cast<uint32_t>(newCap) - static_cast<uint32_t>(oldCap);
    m_TotalSlots += delta;
    m_UsedSlots += delta;
    return newCap;
}

float SlotAllocator::GetFragmentationRatio() const
{
    if (m_TotalSlots == 0)
        return 0.0f;
    return static_cast<float>(m_FreeSlots) / static_cast<float>(m_TotalSlots);
}

size_t SlotAllocator::GetSizeClassFreeCount(size_t classIdx) const
{
    if (classIdx >= kNumSizeClasses)
        return 0;
    return m_FreeLists[classIdx].size();
}

} // namespace UI
} // namespace GameEngine
