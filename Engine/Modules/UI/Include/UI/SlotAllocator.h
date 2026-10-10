#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace UI
{

// SlotAllocator — variable-size range allocator over a logical slot space.
// Used by the UI primitive store to give each UIElement a contiguous range
// of slots in m_Primitives (or m_DrawOrder) without rebuilding the array.
//
// The allocator tracks indices only; the caller owns the backing storage
// (e.g. std::vector<UIPrimitive>) and is responsible for sizing it to at
// least GetTotalSlots() before reading/writing returned indices.
//
// Strategy: size-class segregated free lists for ranges 1..64 slots
// (power-of-two buckets), plus a first-fit "large" list for ranges > 64.
// Allocations pop from the matching bucket; on empty, walk to a larger
// bucket; on starvation, extend the high-water mark. No splitting on free.
//
// In-place growth is tail-only: if the range sits at the high-water mark,
// the mark advances. Otherwise the caller must Allocate a new range,
// memcpy live slots, and Free the old. See plan §3 for design rationale.
class SlotAllocator
{
  public:
    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

    SlotAllocator() = default;

    // Allocate a contiguous range of `count` slots. Returns the start index.
    // outCap receives the granted capacity (>= count). For count <= 64 the
    // cap is the power-of-two size class; for count > 64 the cap equals
    // count exactly.
    //
    // Walking up size classes when the matching bucket is empty may grant
    // a cap larger than 2*count; this trades per-element waste for fewer
    // storage extensions and is preferred for a long-lived editor workload.
    //
    // Asserts on count == 0.
    uint32_t Allocate(uint16_t count, uint16_t& outCap);

    // Free a previously allocated range. cap MUST equal the cap returned
    // by the matching Allocate (or by TryGrowInPlace if it grew the range).
    void Free(uint32_t start, uint16_t cap);

    // Try to grow [start, start+oldCap) in place to hold newCount slots.
    // Currently succeeds only when the range sits at the high-water mark
    // (start + oldCap == GetTotalSlots()). Returns the new cap on success
    // (>= newCount), or 0 if the caller must relocate.
    //
    // newCount must be > oldCap (otherwise the caller already fits).
    uint16_t TryGrowInPlace(uint32_t start, uint16_t oldCap, uint16_t newCount);

    // Defrag is deliberately not implemented in Stage 0 — the gating
    // bench (plan §3.6) measures steady-state behavior without defrag.
    // Stage 1+ will add it; the signature will be:
    //   void Defrag(const std::function<void(uint32_t oldStart,
    //                                        uint32_t newStart,
    //                                        uint16_t cap)>& onRelocate);
    // where the callback performs the actual storage memcpy. Storage
    // type stays opaque to the allocator.

    // Currently in-use slots (sum of caps of all live ranges).
    size_t GetUsedSlots() const { return m_UsedSlots; }

    // High-water mark — minimum size the backing storage must be sized to.
    size_t GetTotalSlots() const { return m_TotalSlots; }

    // Free slots / total slots. 0.0 = no fragmentation, 1.0 = all storage is holes.
    // The microbench gating criterion is <= 0.20 (see plan §3.6).
    float GetFragmentationRatio() const;

    // Diagnostics for tests/bench. Not for runtime use.
    size_t GetSizeClassFreeCount(size_t classIdx) const;
    size_t GetLargeFreeListSize() const { return m_LargeFreeList.size(); }

  private:
    // Size class caps: 1, 2, 4, 8, 16, 32, 64. Index = bit_width(count - 1)
    // for count in [1, 64]; count > 64 falls into the large bucket.
    static constexpr size_t kNumSizeClasses = 7;
    static constexpr uint16_t kMaxSmallCap = 64;

    static size_t SizeClassIndex(uint16_t count);
    static uint16_t SizeClassCap(size_t classIdx);

    struct LargeRange
    {
        uint32_t Start;
        uint16_t Cap;
    };

    std::vector<uint32_t> m_FreeLists[kNumSizeClasses];
    std::vector<LargeRange> m_LargeFreeList;

    uint32_t m_TotalSlots = 0;
    uint32_t m_UsedSlots  = 0;
    uint32_t m_FreeSlots  = 0;
};

} // namespace UI
} // namespace GameEngine
