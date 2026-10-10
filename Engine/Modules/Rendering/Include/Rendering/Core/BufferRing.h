#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <string>
#include <utility>
#include "Rendering/Core/Device.h"

namespace GameEngine { namespace Rendering {

// Low-level single-buffer linear allocator for transient CPU->GPU uploads.
// Owns one persistently-mapped buffer with a head pointer that resets each frame.
//
// This is a building block -- it manages a single physical buffer and does NOT
// account for multiple frames in flight.  For per-frame GPU data that must be
// safe across concurrent frame submissions, prefer FrameBufferAllocator
// (N BufferRings, rotated by frame index).
//
// Direct use of BufferRing is appropriate only when you manage frame rotation
// externally (e.g., PerFrameWritePool already owns one BufferRing per frame slot).
//
// The ring declares every region it hands out to the device
// (IDevice::FlushMappedRange), so callers write through the returned pointer and
// never think about publication. A caller that writes anywhere else in the
// buffer is on its own — see that contract.
class BufferRing {
public:
    BufferRing() = default;

    // Move support (std::atomic is not movable, so we handle it explicitly)
    BufferRing(BufferRing&& other) noexcept
        : m_Device(other.m_Device), m_Buffer(other.m_Buffer), m_BasePtr(other.m_BasePtr)
        , m_Capacity(other.m_Capacity), m_Head(other.m_Head)
        , m_AtomicHead(other.m_AtomicHead.load(std::memory_order_relaxed))
        , m_FrameStart(other.m_FrameStart), m_DefaultAlign(other.m_DefaultAlign)
        , m_AnyAllocThisFrame(other.m_AnyAllocThisFrame.load(std::memory_order_relaxed))
        , m_RequestedThisFrame(other.m_RequestedThisFrame.load(std::memory_order_relaxed))
    {
        other.m_Device = nullptr; other.m_Buffer = {}; other.m_BasePtr = nullptr;
        other.m_Capacity = 0; other.m_Head = 0; other.m_AtomicHead.store(0);
    }
    BufferRing& operator=(BufferRing&& other) noexcept
    {
        if (this != &other)
        {
            Shutdown();
            m_Device = other.m_Device; m_Buffer = other.m_Buffer; m_BasePtr = other.m_BasePtr;
            m_Capacity = other.m_Capacity; m_Head = other.m_Head;
            m_AtomicHead.store(other.m_AtomicHead.load(std::memory_order_relaxed));
            m_FrameStart = other.m_FrameStart; m_DefaultAlign = other.m_DefaultAlign;
            m_AnyAllocThisFrame.store(other.m_AnyAllocThisFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_RequestedThisFrame.store(other.m_RequestedThisFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.m_Device = nullptr; other.m_Buffer = {}; other.m_BasePtr = nullptr;
            other.m_Capacity = 0; other.m_Head = 0; other.m_AtomicHead.store(0);
        }
        return *this;
    }
    BufferRing(const BufferRing&) = delete;
    BufferRing& operator=(const BufferRing&) = delete;

    bool Initialize(IDevice* dev,
                    size_t capacityBytes,
                    BufferUsage usage = BufferUsage::Uniform,
                    size_t DefaultAlign = 256,
                    const char* DebugName = "BufferRing")
    {
        if (!dev || capacityBytes == 0) return false;
        m_Device = dev;
        m_Capacity = capacityBytes;
        m_DefaultAlign = std::max<size_t>(1, DefaultAlign);
        BufferDesc bd{};
        bd.size = capacityBytes;
        bd.usage = static_cast<uint32_t>(usage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.persistent = true;
        bd.debugName = DebugName;
        m_Buffer = m_Device->CreateBuffer(bd);
        if (!m_Buffer.IsValid()) return false;
        m_BasePtr = m_Device->MapBuffer(m_Buffer);
        if (!m_BasePtr)
        {
            // The buffer exists but is not mappable (MapBuffer refuses non
            // host-visible memory). Release it here and leave the ring
            // as-if-uninitialized: callers retry Initialize at a different
            // capacity and would otherwise overwrite m_Buffer, leaking this
            // one for the life of the device.
            m_Device->DestroyBuffer(m_Buffer);
            m_Buffer = {};
            m_Capacity = 0;
            m_Device = nullptr;
            return false;
        }
        m_Head = 0; m_FrameStart = 0; m_AnyAllocThisFrame.store(false, std::memory_order_relaxed);
        return true;
    }

    void Shutdown() {
        if (m_Device && m_Buffer.IsValid()) {
            m_Device->UnmapBuffer(m_Buffer);
            m_Device->DestroyBuffer(m_Buffer);
        }
        m_Buffer = {}; m_BasePtr = nullptr; m_Capacity = 0; m_Head = 0;
        m_AtomicHead.store(0, std::memory_order_relaxed);
        m_FrameStart = 0; m_AnyAllocThisFrame.store(false, std::memory_order_relaxed); m_Device = nullptr;
        m_RequestedThisFrame.store(0, std::memory_order_relaxed);
    }

    // Q6 slice 4 (§8-completion): the buffer this ring owned was freed by an
    // in-place device rebuild, but m_Buffer still reads IsValid() (the wholesale VMA
    // teardown does not bump handle generations). Drop it WITHOUT Unmap/DestroyBuffer
    // (both would touch a recycled slot). Returns the prior capacity so the owning
    // allocator can re-create at the same (possibly grown) size.
    size_t Reprovision() {
        const size_t priorCapacity = m_Capacity;
        m_Buffer = {}; m_BasePtr = nullptr; m_Capacity = 0; m_Head = 0;
        m_AtomicHead.store(0, std::memory_order_relaxed);
        m_FrameStart = 0; m_AnyAllocThisFrame.store(false, std::memory_order_relaxed); m_Device = nullptr;
        m_RequestedThisFrame.store(0, std::memory_order_relaxed);
        return priorCapacity;
    }

    // Reset allocation cursor for a new frame; safe for per-frame transient usage.
    // Call exactly once per device frame for this BufferRing, after the GPU has
    // finished consuming any data allocated from it in the previous frame.
    // Clears the per-frame demand counter — the owning allocator should
    // call GetRequestedThisFrame() BEFORE this reset if it wants to act
    // on the previous-cycle demand (e.g. for grow-on-overflow).
    void ResetForNewFrame() {
        m_FrameStart = m_Head;
        m_Head = 0;
        m_AtomicHead.store(0, std::memory_order_relaxed);
        m_AnyAllocThisFrame.store(false, std::memory_order_relaxed);
        m_RequestedThisFrame.store(0, std::memory_order_relaxed);
    }

    struct Allocation { size_t Offset = 0; void* Ptr = nullptr; size_t Size = 0; };

    // Single-threaded allocation. Do NOT mix with AllocateAtomic() on the same
    // BufferRing in the same frame — they use independent head pointers and will
    // hand out overlapping regions.
    // Returns an empty allocation when the request does not fit in the space left
    // this frame, matching AllocateAtomic.
    Allocation Allocate(size_t sizeBytes, size_t alignment = 0) {
        if (sizeBytes == 0 || !m_BasePtr) return {};
        assert(m_AtomicHead.load(std::memory_order_relaxed) == 0 &&
               "Cannot mix Allocate() and AllocateAtomic() on the same BufferRing in one frame");
        const size_t align = alignment ? alignment : m_DefaultAlign;
        auto AlignUp = [](size_t x, size_t a){ return (x + (a - 1)) & ~(a - 1); };

        // Track demand whether or not the request will fit so the owning
        // allocator can grow the ring on the NEXT cycle.
        m_RequestedThisFrame.fetch_add(sizeBytes, std::memory_order_relaxed);

        size_t head = AlignUp(m_Head, align);
        // Refuse rather than rewind. Every caller writes its region and hands the
        // (buffer, offset) pair to the GPU, so re-issuing offset 0 mid-frame does
        // not recycle space — it aliases a region an earlier allocation of the SAME
        // frame is still using, silently and with no diagnostic. ResetForNewFrame is
        // the only legitimate rewind: it runs once the GPU is done with the slot.
        // The demand recorded above still grows the ring on the next cycle.
        if (head + sizeBytes > m_Capacity) return {};
        m_Head = head + sizeBytes;
        m_AnyAllocThisFrame.store(true, std::memory_order_relaxed);
        // Declared here rather than after the caller writes: an allocation the
        // ring hands out is a region the caller is about to fill, so covering
        // it at the point of issue is what makes the mapping's dirty set
        // complete by construction. Backends with coherent mappings ignore it.
        m_Device->FlushMappedRange(m_Buffer, head, sizeBytes);
        return Allocation{ head, static_cast<uint8_t*>(m_BasePtr) + head, sizeBytes};
    }

    // Thread-safe allocation using an atomic bump pointer (CAS loop).
    // Multiple threads can call this concurrently on the same BufferRing.
    // Each caller gets a disjoint, correctly aligned region.
    // Do NOT mix with Allocate() on the same BufferRing in the same frame.
    // Returns an empty allocation if the ring is full.
    Allocation AllocateAtomic(size_t sizeBytes, size_t alignment = 0)
    {
        if (sizeBytes == 0 || !m_BasePtr) return {};
        const size_t align = alignment ? alignment : m_DefaultAlign;

        // Demand tracking — record requested bytes whether we end up returning
        // success or out-of-space. Owning allocator uses this in BeginFrame to
        // decide whether to grow this slot before the next frame's writes.
        m_RequestedThisFrame.fetch_add(sizeBytes, std::memory_order_relaxed);

        size_t oldHead, newHead, aligned;
        do
        {
            oldHead = m_AtomicHead.load(std::memory_order_relaxed);
            aligned = (oldHead + (align - 1)) & ~(align - 1);
            newHead = aligned + sizeBytes;
            if (newHead > m_Capacity)
                return {}; // out of space
        } while (!m_AtomicHead.compare_exchange_weak(
            oldHead, newHead, std::memory_order_relaxed, std::memory_order_relaxed));

        m_AnyAllocThisFrame.store(true, std::memory_order_relaxed); // benign race — only used as a hint
        m_Device->FlushMappedRange(m_Buffer, aligned, sizeBytes);
        return Allocation{ aligned, static_cast<uint8_t*>(m_BasePtr) + aligned, sizeBytes };
    }

    // Reset the atomic head (call from the same thread as ResetForNewFrame).
    void ResetAtomicHead() { m_AtomicHead.store(0, std::memory_order_relaxed); }

    // Query current atomic head position (for diagnostics).
    size_t GetAtomicHead() const { return m_AtomicHead.load(std::memory_order_relaxed); }

    BufferHandle GetBuffer() const { return m_Buffer; }
    void* GetBasePtr() const { return m_BasePtr; }
    size_t GetCapacity() const { return m_Capacity; }
    size_t GetHead() const { return m_Head; }
    bool AnyAllocThisFrame() const { return m_AnyAllocThisFrame.load(std::memory_order_relaxed); }

    // Total bytes requested in the CURRENT pending frame (successful +
    // failed allocations) — i.e. demand accumulated since the last
    // ResetForNewFrame. Read this BEFORE ResetForNewFrame to see the
    // demand from when this slot was last used; it is the signal the
    // owning allocator uses to decide whether to grow.
    size_t GetRequestedThisFrame() const {
        return m_RequestedThisFrame.load(std::memory_order_relaxed);
    }

private:
    IDevice* m_Device = nullptr;
    BufferHandle m_Buffer{};
    void* m_BasePtr = nullptr;
    size_t m_Capacity = 0;
    size_t m_Head = 0;
    std::atomic<size_t> m_AtomicHead{0};
    size_t m_FrameStart = 0;
    size_t m_DefaultAlign = 256;
    std::atomic<bool> m_AnyAllocThisFrame{false};

    // Demand tracking for grow-on-overflow. Incremented atomically by
    // Allocate / AllocateAtomic on every request (success or fail).
    // ResetForNewFrame clears the counter; the owning allocator reads
    // it BEFORE reset to act on prior-cycle demand.
    std::atomic<size_t> m_RequestedThisFrame{0};
};

}} // namespace GameEngine::Rendering
