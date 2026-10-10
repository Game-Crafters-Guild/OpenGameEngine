#include "Engine/Rendering/BucketPoolAllocator.h"

#include "Types/GeometricReserve.h"

#include <algorithm>
#include <cassert>
#include <new>

namespace GameEngine
{
namespace Rendering
{

uint64_t BucketPoolAllocator::RoundUpPow2(uint64_t v)
{
    if (v <= 1)
        return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

uint64_t BucketPoolAllocator::AlignUp(uint64_t v, uint64_t alignment)
{
    return (v + (alignment - 1)) & ~(alignment - 1);
}

void BucketPoolAllocator::Initialize(uint64_t capacity, uint64_t alignment)
{
    assert(m_Capacity == 0 && "BucketPoolAllocator::Initialize called twice");
    m_Capacity  = capacity;
    m_Alignment = RoundUpPow2(alignment == 0 ? 1 : alignment);
    m_Used      = 0;
    m_Peak      = 0;
    m_OutstandingAllocations = 0;
    m_FreeRanges.clear();
    m_DeferredFrees.clear();
    if (capacity > 0)
        m_FreeRanges.push_back({0, capacity});
}

BucketPoolAllocator::Allocation BucketPoolAllocator::Allocate(uint64_t size)
{
    if (size == 0 || m_Capacity == 0)
        return {};

    const uint64_t alignedSize = AlignUp(size, m_Alignment);

    // Best-fit: smallest free range that fits.
    auto bestIt = m_FreeRanges.end();
    for (auto it = m_FreeRanges.begin(); it != m_FreeRanges.end(); ++it)
    {
        if (it->size < alignedSize)
            continue;
        if (bestIt == m_FreeRanges.end() || it->size < bestIt->size)
            bestIt = it;
    }
    if (bestIt == m_FreeRanges.end())
        return {};

    // Provision retirement before publishing the allocation. Outstanding ranges
    // include deferred frees; freeing each can add at most one free-list range.
    // Geometric growth keeps this amortized rather than reserving on each free.
    const auto bestIndex = static_cast<size_t>(bestIt - m_FreeRanges.begin());
    try
    {
        ReserveGeometric(m_DeferredFrees, m_OutstandingAllocations + 1);
        ReserveGeometric(m_FreeRanges, m_FreeRanges.size() + m_OutstandingAllocations + 1);
    }
    catch (const std::bad_alloc&)
    {
        // Keep Allocate's invalid-result contract so a multi-stream caller
        // returns its earlier ranges through the existing cleanup/fallback.
        return {};
    }
    bestIt = m_FreeRanges.begin() + bestIndex;

    Allocation out{};
    out.offset = bestIt->offset;
    out.size   = alignedSize;

    if (bestIt->size == alignedSize)
    {
        m_FreeRanges.erase(bestIt);
    }
    else
    {
        bestIt->offset += alignedSize;
        bestIt->size -= alignedSize;
    }

    m_Used += alignedSize;
    ++m_OutstandingAllocations;
    if (m_Used > m_Peak)
        m_Peak = m_Used;
    return out;
}

void BucketPoolAllocator::InsertFreeRange(uint64_t offset, uint64_t size)
{
    if (size == 0)
        return;

    // Find insertion point: first free range with offset > our offset.
    auto it = std::upper_bound(
        m_FreeRanges.begin(), m_FreeRanges.end(), offset,
        [](uint64_t off, const FreeRange& fr) { return off < fr.offset; });

    // Coalesce with the previous range if adjacent.
    bool coalescedLeft = false;
    if (it != m_FreeRanges.begin())
    {
        auto prev = std::prev(it);
        if (prev->offset + prev->size == offset)
        {
            prev->size += size;
            offset       = prev->offset;
            size         = prev->size;
            coalescedLeft = true;
            // `it` is unchanged (still points past prev).
        }
    }

    // Coalesce with the next range if adjacent.
    if (it != m_FreeRanges.end() && offset + size == it->offset)
    {
        if (coalescedLeft)
        {
            auto prev = std::prev(it);
            prev->size += it->size;
            m_FreeRanges.erase(it);
        }
        else
        {
            it->offset = offset;
            it->size += size;
        }
        return;
    }

    if (coalescedLeft)
        return; // already merged into prev

    m_FreeRanges.insert(it, FreeRange{offset, size});
}

void BucketPoolAllocator::Free(Allocation alloc)
{
    if (!alloc.IsValid())
        return;
    assert(alloc.size <= m_Used && "free larger than tracked used");
    assert(m_OutstandingAllocations > 0 && "free of a range this allocator did not hand out");
    // Insert before the count drops: the reservation Allocate made covers
    // size + outstanding, so the insert is still inside it here.
    InsertFreeRange(alloc.offset, alloc.size);
    m_Used -= alloc.size;
    --m_OutstandingAllocations;
}

void BucketPoolAllocator::FreeDeferred(Allocation alloc, uint64_t retireAfterFrame)
{
    if (!alloc.IsValid())
        return;
    assert(m_DeferredFrees.size() < m_DeferredFrees.capacity());
    m_DeferredFrees.push_back({alloc.offset, alloc.size, retireAfterFrame});
}

void BucketPoolAllocator::ReclaimUpToFrame(uint64_t safeFrame)
{
    auto it = m_DeferredFrees.begin();
    while (it != m_DeferredFrees.end())
    {
        if (it->retireAfterFrame <= safeFrame)
        {
            assert(it->size <= m_Used && "deferred free larger than tracked used");
            assert(m_OutstandingAllocations > 0 && "reclaim of a range this allocator did not hand out");
            InsertFreeRange(it->offset, it->size);
            m_Used -= it->size;
            --m_OutstandingAllocations;
            it = m_DeferredFrees.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

BucketPoolAllocator::Stats BucketPoolAllocator::GetStats() const
{
    Stats s{};
    s.capacity       = m_Capacity;
    s.used           = m_Used;
    s.peak           = m_Peak;
    s.freeRangeCount = static_cast<uint32_t>(m_FreeRanges.size());
    s.deferredCount  = static_cast<uint32_t>(m_DeferredFrees.size());
    for (const auto& fr : m_FreeRanges)
        s.largestFreeRange = std::max(s.largestFreeRange, fr.size);
    if (m_Capacity > 0)
    {
        const uint64_t live = m_Used;
        const uint64_t freeBytes =
            m_Capacity > live ? (m_Capacity - live) : 0;
        const uint64_t scattered =
            freeBytes > s.largestFreeRange ? (freeBytes - s.largestFreeRange) : 0;
        s.fragRatio = freeBytes > 0
                          ? static_cast<float>(scattered) / static_cast<float>(freeBytes)
                          : 0.0f;
    }
    return s;
}

} // namespace Rendering
} // namespace GameEngine
