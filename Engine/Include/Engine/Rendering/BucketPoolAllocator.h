#pragma once

// BucketPoolAllocator: pure-CPU best-fit range allocator for geometry buffer
// pools. Tracks (offset, size) ranges against a logical capacity. The
// underlying GPU buffer (VkBuffer + memory) is owned by the caller; this
// allocator only decides where in that buffer a request lands.
//
// Frees can be deferred by frame index. ReclaimUpToFrame returns deferred
// ranges to the live free-list once the GPU has progressed past the frame
// in which the allocation was last used. This is the standard pattern for
// safely reusing GPU memory across kMaxFramesInFlight without an explicit
// fence wait.
//
// Allocation strategy: best-fit. Free ranges are kept sorted by offset and
// coalesced with adjacent neighbours on Free.
//
// Thread safety: NOT thread-safe, and it does not need to be. Every pool lives
// inside a MeshGPURegistry bucket and is only ever reached from a registry
// mutator, all of which run under the registry's table mutex. The serialisation
// is mutual exclusion, not thread affinity: which thread mutates varies.

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class BucketPoolAllocator
{
  public:
    struct Allocation
    {
        uint64_t offset = 0; // in bytes
        uint64_t size   = 0; // in bytes (already rounded up to alignment)

        bool IsValid() const { return size > 0; }
    };

    struct Stats
    {
        uint64_t capacity         = 0;
        uint64_t used             = 0;
        uint64_t peak             = 0;
        uint64_t largestFreeRange = 0;
        uint32_t freeRangeCount   = 0;
        uint32_t deferredCount    = 0;
        // (capacity - used - largestFreeRange) / capacity
        // 0.0 = perfectly packed; high values mean live ranges are
        // separating large blocks of free space (fragmentation).
        float fragRatio = 0.0f;
    };

    BucketPoolAllocator() = default;
    // A live allocator owns its ranges and provisioned retirement capacity.
    // Copying would duplicate ownership and discard the spare vector capacity.
    BucketPoolAllocator(const BucketPoolAllocator&) = delete;
    BucketPoolAllocator& operator=(const BucketPoolAllocator&) = delete;
    BucketPoolAllocator(BucketPoolAllocator&&) noexcept = default;
    BucketPoolAllocator& operator=(BucketPoolAllocator&&) noexcept = default;

    // Configure capacity + alignment. Must be called before Allocate/Free.
    // `alignment` is rounded up to the next power-of-two; values smaller
    // than 1 are clamped to 1.
    void Initialize(uint64_t capacity, uint64_t alignment);

    // Returns Allocation{} on failure (caller may grow or chain a sibling
    // pool). On success the offset is aligned to the configured alignment
    // and size is the rounded-up byte count.
    Allocation Allocate(uint64_t size);

    // Return the range to the live free-list immediately, coalescing with
    // adjacent free ranges.
    void Free(Allocation alloc);

    // Queue the range to be returned to the live free-list when
    // ReclaimUpToFrame(>= retireFrame) is later called. Use this when the
    // GPU may still be reading the range during in-flight frames; the
    // caller passes the current frame index, and the allocator releases
    // the range once kMaxFramesInFlight frames have advanced past it.
    void FreeDeferred(Allocation alloc, uint64_t retireAfterFrame);

    // Move deferred frees with retireAfterFrame <= safeFrame back into the
    // live free-list. Called once per frame from the rendering thread
    // after the device confirms the corresponding frame slot has retired.
    void ReclaimUpToFrame(uint64_t safeFrame);

    Stats GetStats() const;

    // Test / debug helpers.
    uint64_t Capacity() const { return m_Capacity; }
    uint64_t Alignment() const { return m_Alignment; }
    uint64_t Used() const { return m_Used; }
    bool     IsEmpty() const { return m_Used == 0 && m_DeferredFrees.empty(); }

  private:
    struct FreeRange
    {
        uint64_t offset;
        uint64_t size;
    };

    struct DeferredFree
    {
        uint64_t offset;
        uint64_t size;
        uint64_t retireAfterFrame;
    };

    // Insert into m_FreeRanges keeping it sorted by offset, coalescing
    // with adjacent neighbours.
    void InsertFreeRange(uint64_t offset, uint64_t size);

    static uint64_t RoundUpPow2(uint64_t v);
    static uint64_t AlignUp(uint64_t v, uint64_t alignment);

    uint64_t                  m_Capacity  = 0;
    uint64_t                  m_Alignment = 1;
    uint64_t                  m_Used      = 0;
    uint64_t                  m_Peak      = 0;
    // Includes ranges awaiting a deferred free. Allocation provisions enough
    // metadata to return every outstanding range without allocating at release.
    size_t m_OutstandingAllocations = 0;
    std::vector<FreeRange>    m_FreeRanges;
    std::vector<DeferredFree> m_DeferredFrees;
};

} // namespace Rendering
} // namespace GameEngine
