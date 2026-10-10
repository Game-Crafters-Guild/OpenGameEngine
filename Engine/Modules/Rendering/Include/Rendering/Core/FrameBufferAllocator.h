#pragma once

// FrameBufferAllocator: double-buffered per-frame linear allocator for
// CPU-written GPU buffers (SSBOs, UBOs, instance indirection, etc.).
//
// Design:
//   - Owns N BufferRings and advances a cursor through them, one ring per
//     device frame. N is a RING DEPTH the caller sizes from its write phase and
//     its readers, not the device's pacing — see BeginFrame for the rule.
//   - BeginFrame(deviceFrameIndex) steps the cursor. The argument is a CHANGE
//     TOKEN, not a ring selector — see BeginFrame.
//   - Suballocations within the current frame's ring are contiguous and aligned.
//   - The returned (BufferHandle, offset, size) triple is used for descriptor binding.
//   - After the frame's GPU work completes (guaranteed by device fences), the
//     ring is safe to reset and reuse.
//
// This avoids per-draw or per-mesh SSBO creation. All per-frame transient data
// of a given usage class shares one large buffer; only ranges/offsets differ.
//
// Usage:
//   allocator.Initialize(device, capacity, usage, slotCount);
//   // Each frame:
//   allocator.BeginFrame(device->GetFrameIndex());
//   auto alloc = allocator.Allocate(dataSize);
//   memcpy(alloc.ptr, data, dataSize);
//   // Bind alloc.buffer + alloc.offset in descriptor
//
// Ownership:
//   - Owned by the system that needs per-frame transient buffers (e.g., WorldDrawBuilder).
//   - Device must outlive the allocator.
//
// Thread safety:
//   - Not thread-safe. Use from a single thread per BeginFrame/Allocate sequence.
//   - Multiple allocators can be used from different threads if they target different rings.

#include "Rendering/Core/BufferRing.h"
#include "Rendering/Core/Device.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class FrameBufferAllocator
{
  public:
    // Deepest ring any caller can ask for. A RING DEPTH, not the device's pacing:
    // the two differ by the caller's write phase and by how many frames read a slot
    // (see BeginFrame). The deepest the rule can demand is the deepest pacing, plus
    // one for an update-phase write, plus one for a reader that outlives the writing
    // frame.
    static constexpr uint32_t kMaxRingSlots = IDevice::kMaxSupportedFramesInFlight + 2;

    struct Allocation
    {
        BufferHandle buffer{};  // The backing GPU buffer for this frame slot.
        size_t offset = 0;      // Byte offset into the buffer.
        void* ptr = nullptr;    // CPU-mapped pointer for writing.
        size_t size = 0;        // Allocated size in bytes.

        bool IsValid() const { return ptr != nullptr && size > 0; }
    };

    FrameBufferAllocator() = default;
    ~FrameBufferAllocator() { Shutdown(); }

    // Non-copyable.
    FrameBufferAllocator(const FrameBufferAllocator&) = delete;
    FrameBufferAllocator& operator=(const FrameBufferAllocator&) = delete;

    // Move is allowed (for container storage).
    FrameBufferAllocator(FrameBufferAllocator&& other) noexcept
        : m_Rings(std::move(other.m_Rings))
        , m_SlotCount(other.m_SlotCount)
        , m_CurrentSlot(other.m_CurrentSlot)
        , m_PreviousSlot(other.m_PreviousSlot)
        , m_LastDeviceFrameIndex(other.m_LastDeviceFrameIndex)
        , m_HasCurrentSlot(other.m_HasCurrentSlot)
        , m_HasPreviousSlot(other.m_HasPreviousSlot)
        , m_Initialized(other.m_Initialized)
        , m_Device(other.m_Device)
        , m_Usage(other.m_Usage)
        , m_DefaultAlign(other.m_DefaultAlign)
        , m_DebugName(std::move(other.m_DebugName))
        , m_MaxCapacityBytes(other.m_MaxCapacityBytes)
    {
        other.m_Initialized = false;
        other.m_Device = nullptr;
    }

    FrameBufferAllocator& operator=(FrameBufferAllocator&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_Rings = std::move(other.m_Rings);
            m_SlotCount = other.m_SlotCount;
            m_CurrentSlot = other.m_CurrentSlot;
            m_PreviousSlot = other.m_PreviousSlot;
            m_LastDeviceFrameIndex = other.m_LastDeviceFrameIndex;
            m_HasCurrentSlot = other.m_HasCurrentSlot;
            m_HasPreviousSlot = other.m_HasPreviousSlot;
            m_Initialized = other.m_Initialized;
            m_Device = other.m_Device;
            m_Usage = other.m_Usage;
            m_DefaultAlign = other.m_DefaultAlign;
            m_DebugName = std::move(other.m_DebugName);
            m_MaxCapacityBytes = other.m_MaxCapacityBytes;
            other.m_Initialized = false;
            other.m_Device = nullptr;
        }
        return *this;
    }

    // Initialize with one ring per frame-in-flight.
    //
    // `capacityBytes` is the size of each ring (all frames get the same capacity).
    // `usage` should include Storage for SSBO, Uniform for UBO, etc.
    // `defaultAlign` is the minimum alignment for suballocations (256 for UBOs, 16 for SSBOs).
    // Initialize. `capacityBytes` is the per-ring starting size. If
    // `maxCapacityBytes` is non-zero, BeginFrame will grow individual
    // rings up to (but not past) that cap when prior-frame demand
    // exceeds the current ring capacity. A zero `maxCapacityBytes`
    // disables grow-on-demand (legacy fixed-size behaviour).
    bool Initialize(IDevice* device,
                    size_t capacityBytes,
                    BufferUsage usage = BufferUsage::Storage,
                    uint32_t slotCount = 2,
                    size_t defaultAlign = 16,
                    const char* debugName = "FrameBufferAllocator",
                    size_t maxCapacityBytes = 0)
    {
        assert(device);
        assert(slotCount > 0 && slotCount <= kMaxRingSlots);

        m_Device = device;
        m_Usage = usage;
        m_DefaultAlign = defaultAlign;
        m_DebugName = debugName;
        m_SlotCount = slotCount;
        m_MaxCapacityBytes = maxCapacityBytes;
        m_Rings.resize(slotCount);

        for (uint32_t i = 0; i < slotCount; ++i)
        {
            std::string name = m_DebugName + ".Frame" + std::to_string(i);
            if (!m_Rings[i].Initialize(device, capacityBytes, usage, defaultAlign, name.c_str()))
            {
                Shutdown();
                return false;
            }
        }

        m_Initialized = true;
        m_CurrentSlot = 0;
        m_PreviousSlot = 0;
        m_LastDeviceFrameIndex = 0;
        m_HasCurrentSlot = false;
        m_HasPreviousSlot = false;
        return true;
    }

    void Shutdown()
    {
        for (auto& ring : m_Rings)
        {
            ring.Shutdown();
        }
        m_Rings.clear();
        m_Initialized = false;
        m_HasCurrentSlot = false;
        m_HasPreviousSlot = false;
    }

    // Q6 slice 4 (§8-completion): after an in-place device rebuild every ring's
    // backing buffer + mapped pointer is dead. Drop them WITHOUT Unmap/Destroy — not
    // to avoid a double-free (a stale Destroy* is a generational no-op) but because
    // the cached m_BasePtr is a RAW pointer into freed VMA memory: BeginFrame/Allocate
    // writing through it is a true use-after-free that generations do not guard.
    // Re-create each ring at its prior (possibly grown) capacity so the next
    // BeginFrame/Allocate writes into live, freshly-mapped memory.
    void ReprovisionAfterDeviceRebuild()
    {
        if (!m_Initialized || !m_Device)
            return;
        IDevice* const device = m_Device;
        // Every ring's contents die with the old device, so last frame's bytes
        // are gone even though the slot indices survive.
        m_HasCurrentSlot = false;
        m_HasPreviousSlot = false;
        for (auto& ring : m_Rings)
        {
            const size_t priorCapacity = ring.Reprovision();
            const size_t capacity = priorCapacity > 0 ? priorCapacity : m_DefaultAlign;
            const size_t idx = static_cast<size_t>(&ring - m_Rings.data());
            std::string name = m_DebugName + ".Frame" + std::to_string(idx);
            if (ring.Initialize(device, capacity, m_Usage, m_DefaultAlign, name.c_str()))
                continue;
            // The rebuilt device could not satisfy the prior (possibly grown)
            // capacity. Re-create at the smallest useful size rather than leaving
            // the ring dead: a dead ring refuses every allocation for the rest of
            // the session, while a small one loses a cycle and then grows back from
            // the demand its refusals record.
            ring.Initialize(device, m_DefaultAlign, m_Usage, m_DefaultAlign, name.c_str());
        }
    }

    // Step to the next ring and reset it for new allocations. Call exactly once
    // per frame before any Allocate() calls. If grow-on-demand is enabled
    // (maxCapacityBytes > 0 at Initialize), also checks whether the LAST frame's
    // allocation demand exceeded this ring's capacity and grows the buffer in
    // place before reset. Growth doubles the current capacity (clamped to
    // maxCapacityBytes and the demanded size) and is performed by destroying +
    // recreating this slot's BufferRing — safe because at BeginFrame the GPU has
    // finished its work on this slot from N frames ago.
    //
    // `deviceFrameIndex` is a CHANGE TOKEN, never a ring index. Backends report
    // a frame slot in [0, devicePacing) — VulkanDevice::GetFrameIndex() wraps at
    // MAX_FRAMES_IN_FLIGHT — so using its VALUE to select a ring would cap the
    // effective rotation at the device's pacing and leave every ring beyond it
    // unreachable, silently deleting the margin a deeper ring pays for.
    // Only the fact that it CHANGED is used: it changes exactly when the device
    // advances a frame, which is what the rule below is stated in.
    //
    // HOW DEEP THE RING MUST BE. IDevice::BeginFrame waits the fences of the slot
    // it is about to use, so after tick N's BeginFrame every tick through
    // N-devicePacing is proven complete — but BeginFrame runs in the tick's RENDER
    // phase, so during tick N's UPDATE phase the newest proven tick is only
    // N-devicePacing-1. For a ring whose element written at tick T is still read by
    // tick T+k (k=0 unless a pass reads the previous frame's slot):
    //
    //   written behind BeginFrame (render phase): slotCount >= devicePacing + k
    //   written before it (update phase):         slotCount >= devicePacing + 1 + k
    //
    // The update phase costs exactly one extra slot. Sizing an update-phase ring at
    // devicePacing hands the CPU a slot a frame in flight is still reading, which is
    // silent corruption, not a stall.
    void BeginFrame(uint32_t deviceFrameIndex)
    {
        assert(m_Initialized);
        // Previous slot is tracked, not derived from index arithmetic: a caller
        // that skips or repeats a frame must still name the ring this allocator
        // actually wrote last.
        if (m_HasCurrentSlot)
        {
            m_PreviousSlot = m_CurrentSlot;
            m_HasPreviousSlot = true;
            // Same token means the device did not advance, so this frame writes
            // over the ring it already owns rather than claiming the next one.
            if (deviceFrameIndex != m_LastDeviceFrameIndex)
                m_CurrentSlot = (m_CurrentSlot + 1) % m_SlotCount;
        }
        m_LastDeviceFrameIndex = deviceFrameIndex;
        m_HasCurrentSlot = true;
        // The ring about to be reset cannot also serve as "previous": either the
        // device did not advance, or there is only one ring to go round.
        if (m_HasPreviousSlot && m_PreviousSlot == m_CurrentSlot)
            m_HasPreviousSlot = false;
        MaybeGrowCurrentSlot();
        m_Rings[m_CurrentSlot].ResetForNewFrame();
    }

    // Suballocate from the current frame's ring (single-threaded).
    // Returns an Allocation with the buffer handle, offset, CPU pointer, and size.
    // Returns an invalid allocation (size=0) if the ring is full.
    Allocation Allocate(size_t bytes, size_t alignment = 0)
    {
        assert(m_Initialized);
        auto ringAlloc = m_Rings[m_CurrentSlot].Allocate(bytes, alignment);
        Allocation result{};
        result.buffer = m_Rings[m_CurrentSlot].GetBuffer();
        result.offset = ringAlloc.Offset;
        result.ptr = ringAlloc.Ptr;
        result.size = ringAlloc.Size;
        return result;
    }

    // Ensure total capacity for this frame before allocating or publishing a
    // buffer binding. Existing allocations are never invalidated. Single-threaded;
    // returns false when growth is disabled, exceeds the cap, or would replace
    // a ring that already served an allocation.
    bool Reserve(size_t capacityBytes)
    {
        assert(m_Initialized);
        const BufferRing& ring = m_Rings[m_CurrentSlot];
        if (capacityBytes <= ring.GetCapacity())
            return true;
        if (ring.AnyAllocThisFrame() || m_MaxCapacityBytes == 0 || capacityBytes > m_MaxCapacityBytes)
            return false;
        GrowCurrentSlot(capacityBytes);
        return capacityBytes <= ring.GetCapacity();
    }

    // Thread-safe suballocate from the current frame's ring (lock-free CAS).
    // Multiple threads can call this concurrently; each gets a disjoint region.
    Allocation AllocateAtomic(size_t bytes, size_t alignment = 0)
    {
        assert(m_Initialized);
        auto ringAlloc = m_Rings[m_CurrentSlot].AllocateAtomic(bytes, alignment);
        Allocation result{};
        result.buffer = m_Rings[m_CurrentSlot].GetBuffer();
        result.offset = ringAlloc.Offset;
        result.ptr = ringAlloc.Ptr;
        result.size = ringAlloc.Size;
        return result;
    }

    // Get the buffer handle for the current frame slot (for descriptor binding).
    BufferHandle GetCurrentBuffer() const
    {
        assert(m_Initialized);
        return m_Rings[m_CurrentSlot].GetBuffer();
    }

    // Current frame slot index (set by BeginFrame; < slotCount).
    uint32_t GetCurrentSlot() const
    {
        assert(m_Initialized);
        return m_CurrentSlot;
    }

    // The ring this allocator filled on the PREVIOUS BeginFrame, still holding
    // that frame's bytes: only the current slot is reset, so every other ring
    // is untouched until its own turn comes round.
    //
    // LIFETIME: a reader that binds this buffer extends the slot's GPU read
    // lifetime by one frame, which is one more slot on top of whatever the writer's
    // phase already costs. The cursor advances one ring per device frame, so slot s
    // written at frame F is next overwritten at frame F+slotCount, and the prev-slot
    // read happens at F+1, so this is the k=1 case of BeginFrame's rule: a
    // render-phase writer needs devicePacing+1 slots, an update-phase writer
    // devicePacing+2. A ring too shallow for its own phase reports false here rather
    // than hand out a buffer the CPU may overwrite mid-flight.
    bool HasPreviousFrame() const { return m_Initialized && m_HasPreviousSlot; }

    BufferHandle GetPreviousBuffer() const
    {
        assert(m_Initialized);
        return m_HasPreviousSlot ? m_Rings[m_PreviousSlot].GetBuffer() : BufferHandle{};
    }

    // Capacity of the previous slot's ring. Slots grow independently, so this
    // is not necessarily GetCapacity().
    size_t GetPreviousCapacity() const
    {
        assert(m_Initialized);
        return m_HasPreviousSlot ? m_Rings[m_PreviousSlot].GetCapacity() : 0;
    }

    // Query the capacity of each ring.
    size_t GetCapacity() const
    {
        return m_Rings.empty() ? 0 : m_Rings[0].GetCapacity();
    }

    // Query how much has been allocated in the current frame.
    size_t GetCurrentUsage() const
    {
        assert(m_Initialized);
        return m_Rings[m_CurrentSlot].GetHead();
    }

    uint32_t GetSlotCount() const { return m_SlotCount; }
    bool IsInitialized() const { return m_Initialized; }
    size_t GetMaxCapacity() const { return m_MaxCapacityBytes; }

  private:
    // If the current slot's prior-cycle demand exceeded its capacity and
    // a max cap is configured, recreate the slot's buffer at a larger
    // size. Reads m_RequestedThisFrame BEFORE ResetForNewFrame clears it
    // — the value reflects the demand from when this slot was last used
    // (slotCount frames ago for slot N's BeginFrame).
    void MaybeGrowCurrentSlot()
    {
        GrowCurrentSlot(m_Rings[m_CurrentSlot].GetRequestedThisFrame());
    }

    // Recreate the current slot's ring at the doubled capacity that covers
    // `demand`, clamped to maxCapacityBytes. No-op when growth is disabled or
    // the ring already covers the demand. Only while the GPU is done with the
    // slot and nothing allocated from it is in use (BeginFrame, Reserve).
    void GrowCurrentSlot(size_t demand)
    {
        if (m_MaxCapacityBytes == 0)
            return;
        BufferRing& ring = m_Rings[m_CurrentSlot];
        const size_t currentCap = ring.GetCapacity();
        if (demand <= currentCap || currentCap >= m_MaxCapacityBytes)
            return;

        // Double until we cover the demand, capped at max.
        size_t newCap = currentCap > 0 ? currentCap : 1;
        while (newCap < demand)
        {
            const size_t doubled = newCap * 2;
            if (doubled <= newCap)
            {
                newCap = m_MaxCapacityBytes; // overflow guard
                break;
            }
            newCap = doubled;
        }
        if (newCap > m_MaxCapacityBytes)
            newCap = m_MaxCapacityBytes;
        if (newCap <= currentCap)
            return;

        std::string name = m_DebugName + ".Frame" + std::to_string(m_CurrentSlot);
        ring.Shutdown();
        if (!ring.Initialize(m_Device, newCap, m_Usage, m_DefaultAlign, name.c_str()))
        {
            // The larger buffer did not fit. Restore the ring at the size it had —
            // the Shutdown above already released it, so bailing here would leave
            // the slot dead and fail every allocation from this frame on.
            ring.Initialize(m_Device, currentCap, m_Usage, m_DefaultAlign, name.c_str());
        }
    }

    std::vector<BufferRing> m_Rings;
    uint32_t m_SlotCount = 0;
    uint32_t m_CurrentSlot = 0;
    uint32_t m_PreviousSlot = 0;
    // Last token BeginFrame was called with, to detect device frame advances.
    uint32_t m_LastDeviceFrameIndex = 0;
    bool m_HasCurrentSlot = false;  // false until the first BeginFrame
    bool m_HasPreviousSlot = false; // false until the second BeginFrame
    bool m_Initialized = false;

    // Init parameters retained so MaybeGrowCurrentSlot can recreate a
    // ring at a larger size with the same usage / alignment / name.
    IDevice* m_Device = nullptr;
    BufferUsage m_Usage = BufferUsage::Storage;
    size_t m_DefaultAlign = 16;
    std::string m_DebugName;
    size_t m_MaxCapacityBytes = 0; // 0 disables grow-on-demand
};

} // namespace Rendering
} // namespace GameEngine
