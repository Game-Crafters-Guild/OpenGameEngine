#pragma once

#include "Rendering/Utils/RingBufferHelpers.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GameEngine
{
namespace UI
{

// Auto-growing per-frame ring buffer for UI SSBOs (UIPrimitive, UIClipRect).
// Wraps the engine's RingBuffer<T> with automatic capacity doubling on overflow.
// After a few frames, capacity stabilizes and no further allocations occur.
template <typename T>
class FrameBufferRing
{
public:
    FrameBufferRing() = default;
    ~FrameBufferRing() { Destroy(); }

    // Non-copyable, movable.
    FrameBufferRing(const FrameBufferRing&) = delete;
    FrameBufferRing& operator=(const FrameBufferRing&) = delete;
    FrameBufferRing(FrameBufferRing&& other) noexcept
        : m_Ring(other.m_Ring), m_Device(other.m_Device),
          m_DebugName(other.m_DebugName), m_MinCapacity(other.m_MinCapacity),
          m_Mode(other.m_Mode), m_SliceVersion(std::move(other.m_SliceVersion)),
          m_PendingLo(std::move(other.m_PendingLo)),
          m_PendingHi(std::move(other.m_PendingHi))
    {
        other.m_Device = nullptr;
        other.m_Ring = {};
    }
    FrameBufferRing& operator=(FrameBufferRing&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_Ring = other.m_Ring;
            m_Device = other.m_Device;
            m_DebugName = other.m_DebugName;
            m_MinCapacity = other.m_MinCapacity;
            m_Mode = other.m_Mode;
            m_SliceVersion = std::move(other.m_SliceVersion);
            m_PendingLo = std::move(other.m_PendingLo);
            m_PendingHi = std::move(other.m_PendingHi);
            other.m_Device = nullptr;
            other.m_Ring = {};
        }
        return *this;
    }

    void Create(Rendering::IDevice* device, uint32_t framesInFlight,
                size_t initialCapacity, const char* debugName = nullptr,
                Rendering::RingBufferMode mode = Rendering::RingBufferMode::Upload)
    {
        Destroy();
        m_Device = device;
        m_DebugName = debugName ? debugName : "UIFrameBufferRing";
        m_MinCapacity = initialCapacity;
        m_Mode = mode;
        m_Ring = Rendering::CreateRingBuffer<T>(
            device, framesInFlight, initialCapacity, m_DebugName, mode);
        m_SliceVersion.assign(framesInFlight, 0u);
        m_PendingLo.assign(framesInFlight, kNoPending);
        m_PendingHi.assign(framesInFlight, 0u);
    }

    // Record that elements [start, start+count) of the CPU snapshot changed.
    // Unions into EVERY slice's pending range — each frame-in-flight slice is
    // an independent copy last synced at a different version, so a slice
    // that is N versions behind needs the union of everything written since
    // it was last uploaded. A slice's pending clears when it uploads.
    // Callers must mark every mutation between version bumps (whole-store
    // rewrites call MarkAllRangesDirty instead); an unmarked write would
    // leave stale bytes in slices served by the range path.
    void MarkRangeDirty(size_t start, size_t count)
    {
        if (count == 0)
            return;
        const size_t hi = start + count - 1;
        for (size_t i = 0; i < m_PendingLo.size(); ++i)
        {
            m_PendingLo[i] = std::min(m_PendingLo[i], start);
            m_PendingHi[i] = std::max(m_PendingHi[i], hi);
        }
    }

    // Whole-store rewrite (full regen): every slice re-uploads in full.
    void MarkAllRangesDirty()
    {
        std::fill(m_PendingLo.begin(), m_PendingLo.end(), size_t{0});
        std::fill(m_PendingHi.begin(), m_PendingHi.end(), kMaxPending);
    }

    void Destroy()
    {
        if (m_Device)
        {
            Rendering::DestroyRingBuffer(m_Device, m_Ring);
            m_Device = nullptr;
        }
    }

    // Begin a new frame. Call at frame start before Upload().
    void BeginFrame(uint32_t frameIndex)
    {
        Rendering::ResetRingBufferFrame(m_Ring, frameIndex);
    }

    // Upload data to the current frame's slice. Returns true on success.
    // If the buffer is too small, it is destroyed and recreated at 2x capacity,
    // then the upload is retried. This may stall the GPU for one frame during growth.
    bool Upload(uint32_t frameIndex, const T* data, size_t count)
    {
        if (count == 0)
            return true;

        auto alloc = Rendering::MapRingBuffer(m_Device, m_Ring, frameIndex, count);
        if (!alloc.ptr)
        {
            // Overflow — grow and retry.
            size_t currentCapacity = m_Ring.capacityPerFrame / sizeof(T);
            size_t newCapacity = std::max(count, currentCapacity * 2);
            newCapacity = std::max(newCapacity, m_MinCapacity);

            uint32_t framesInFlight = m_Ring.framesInFlight;
            Rendering::DestroyRingBuffer(m_Device, m_Ring);
            m_Ring = Rendering::CreateRingBuffer<T>(
                m_Device, framesInFlight, newCapacity, m_DebugName, m_Mode);
            Rendering::ResetRingBufferFrame(m_Ring, frameIndex);
            // The new allocation holds garbage in every per-frame slice —
            // invalidate all recorded content versions so UploadIfChanged
            // re-uploads each slice the next time its frame comes around.
            std::fill(m_SliceVersion.begin(), m_SliceVersion.end(), 0u);

            alloc = Rendering::MapRingBuffer(m_Device, m_Ring, frameIndex, count);
            if (!alloc.ptr)
                return false;
        }

        std::memcpy(alloc.ptr, data, count * sizeof(T));
        Rendering::AdvanceRingBuffer(m_Ring, frameIndex, count);
        return true;
    }

    // Upload only when this frame's slice doesn't already hold `version`'s
    // content. Each frame-in-flight slice is an independent copy, so after a
    // content change every slice re-uploads once as its frame index comes
    // around; from then on clean frames upload zero bytes. `version` must be
    // bumped by the caller on EVERY CPU-side mutation of `data` and never
    // reuse 0 (the "unknown" sentinel set at creation and growth).
    //
    // When the slice holds an OLDER full snapshot (version != 0) and the
    // pending dirty range covers everything written since (MarkRangeDirty),
    // only that span is rewritten in place — a drain frame uploads the
    // touched slots instead of the whole store.
    bool UploadIfChanged(uint32_t frameIndex, const T* data, size_t count, uint64_t version)
    {
        if (frameIndex < m_SliceVersion.size() && m_SliceVersion[frameIndex] == version)
            return true;

        const bool sliceHasSnapshot =
            frameIndex < m_SliceVersion.size() && m_SliceVersion[frameIndex] != 0;
        if (sliceHasSnapshot && m_PendingLo[frameIndex] != kNoPending)
        {
            const size_t lo = m_PendingLo[frameIndex];
            const size_t hi = std::min(m_PendingHi[frameIndex], count > 0 ? count - 1 : 0);
            if (lo <= hi && UploadRange(frameIndex, data, lo, hi - lo + 1))
            {
                m_SliceVersion[frameIndex] = version;
                ClearPending(frameIndex);
                return true;
            }
            // Range write impossible (span exceeds capacity — growth needed)
            // — fall through to the whole-slice path.
        }

        if (!Upload(frameIndex, data, count))
            return false;
        if (frameIndex < m_SliceVersion.size())
        {
            m_SliceVersion[frameIndex] = version;
            ClearPending(frameIndex);
        }
        return true;
    }

    // Flush staging → GPU copy. Call after Upload(), before draw.
    void Flush(Rendering::CommandList* cmd, uint32_t frameIndex)
    {
        Rendering::FlushRingBufferWrites(m_Device, cmd, m_Ring, frameIndex);
    }

    // Get the GPU buffer handle for binding to a descriptor set.
    Rendering::BufferHandle GetBuffer() const
    {
        return Rendering::GetBufferForBinding(m_Ring);
    }

    // Byte offset into the buffer for the current frame's data.
    size_t GetFrameOffset(uint32_t frameIndex) const
    {
        return size_t(frameIndex) * m_Ring.capacityPerFrame;
    }

    // Bytes written in the current frame.
    size_t GetFrameBytes(uint32_t frameIndex) const
    {
        if (frameIndex >= m_Ring.framesInFlight)
            return 0;
        return m_Ring.heads[frameIndex];
    }

    // Current capacity per frame (in elements).
    size_t GetCapacity() const
    {
        return m_Ring.capacityPerFrame / sizeof(T);
    }

    bool IsValid() const
    {
        return m_Device != nullptr && m_Ring.buffer.IsValid();
    }

private:
    // Rewrite a sub-range of this frame's persistent slice region in place.
    // Requires the slice to already hold a full snapshot. Upload-mode rings
    // are host-visible and bound directly (Flush is a no-op for them), so
    // no copy plumbing is needed; the per-frame head is untouched — it only
    // serves the whole-slice map path.
    bool UploadRange(uint32_t frameIndex, const T* data, size_t rangeStart, size_t rangeCount)
    {
        if (rangeCount == 0)
            return true;
        if (m_Mode != Rendering::RingBufferMode::Upload)
            return false;
        if (frameIndex >= m_Ring.framesInFlight || !m_Ring.buffer.IsValid())
            return false;
        const size_t endBytes = (rangeStart + rangeCount) * sizeof(T);
        if (endBytes > m_Ring.capacityPerFrame)
            return false; // caller falls back to the whole-slice growth path
        void* base = m_Device->MapBuffer(m_Ring.buffer);
        if (!base)
            return false;
        const size_t byteOffset = size_t(frameIndex) * m_Ring.capacityPerFrame +
                                  rangeStart * sizeof(T);
        std::memcpy(reinterpret_cast<uint8_t*>(base) + byteOffset, data + rangeStart,
                    rangeCount * sizeof(T));
        // This path writes past the per-frame head, so no MapRingBuffer covered
        // it — the span publishes itself.
        m_Device->FlushMappedRange(m_Ring.buffer, byteOffset, rangeCount * sizeof(T));
        return true;
    }

    void ClearPending(uint32_t frameIndex)
    {
        if (frameIndex < m_PendingLo.size())
        {
            m_PendingLo[frameIndex] = kNoPending;
            m_PendingHi[frameIndex] = 0;
        }
    }

    static constexpr size_t kNoPending = ~size_t{0};
    static constexpr size_t kMaxPending = ~size_t{0} - 1;

    Rendering::RingBuffer<T> m_Ring{};
    Rendering::IDevice* m_Device = nullptr;
    const char* m_DebugName = nullptr;
    size_t m_MinCapacity = 256;
    Rendering::RingBufferMode m_Mode = Rendering::RingBufferMode::Upload;
    // Content version last uploaded into each frame-in-flight slice
    // (see UploadIfChanged). 0 = slice content unknown.
    std::vector<uint64_t> m_SliceVersion;
    // Per-slice pending dirty span (element indices, inclusive): the union
    // of MarkRangeDirty calls since that slice last uploaded. kNoPending in
    // Lo = empty.
    std::vector<size_t> m_PendingLo;
    std::vector<size_t> m_PendingHi;
};

} // namespace UI
} // namespace GameEngine
