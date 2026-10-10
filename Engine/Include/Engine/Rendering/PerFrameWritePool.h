#pragma once

// PerFrameWritePool: manages a set of double-buffered FrameBufferAllocators
// keyed by usage class. Provides a single point of ownership for all per-frame
// CPU-written GPU buffers (material params, bone palettes).
//
// Design:
//   - Each usage class gets its own FrameBufferAllocator with independent
//     capacity, alignment, and buffer usage flags.
//   - Usage classes are identified by a small enum (not open-ended hashing).
//   - BeginFrame(frameIndex) resets all allocators for the current frame.
//   - Callers request a specific usage class and get Allocation results.
//
// This enforces the "no per-mesh SSBO" policy: all per-frame data of a given
// class lives in one shared buffer per frame slot. Only offsets/ranges vary.
//
// Ownership:
//   - Owned by RenderServices (or WorldDrawBuilder).
//   - Device must outlive the pool.
//
// Thread safety:
//   - Not thread-safe. Call from the render thread only.

#include "Rendering/Core/FrameBufferAllocator.h"

#include <algorithm>
#include <array>
#include <cassert>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::BufferUsage;
using ::GameEngine::Rendering::FrameBufferAllocator;
using ::GameEngine::Rendering::IDevice;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Engine::Renderer
{
// Usage classes for per-frame GPU buffer pools.
// Each class gets an independent allocator with tuned capacity.
enum class FrameWriteUsage : uint8_t
{
    // Per-material parameter blocks: material UBO data packed contiguously.
    // Written when materials change, read by shaders.
    MaterialParams = 0,

    // Bone palette data: packed mat4 arrays for skinned instances.
    // Written by SkinningUploadSystem, read by vertex shaders.
    BonePalette = 1,

    Count
};

constexpr uint32_t kFrameWriteUsageCount = static_cast<uint32_t>(FrameWriteUsage::Count);

// Default capacities per usage class (bytes). Tuned for typical scenes.
// These can be overridden at initialization time.
struct PerFrameWritePoolConfig
{
    struct UsageConfig
    {
        size_t capacityBytes = 0;
        size_t alignment = 16;
        Rendering::BufferUsage bufferUsage = Rendering::BufferUsage::Storage;
        // Upper bound for grow-on-demand. 0 disables growth (legacy
        // fixed-size). When non-zero and a frame's allocation demand
        // exceeds capacityBytes, the FrameBufferAllocator doubles the
        // ring at next BeginFrame up to this cap (Reserve can grow an
        // empty ring before allocation).
        size_t maxCapacityBytes = 0;
        // Frames BEYOND the writing one that still read a slot — 1 for a usage
        // class whose previous frame's buffer is read by GPU work. It is the `k`
        // of FrameBufferAllocator::BeginFrame's depth rule; the pool adds the
        // device's pacing and the update-phase margin itself. Costs one
        // additional ring of capacityBytes each.
        uint32_t ExtraReaderFrames = 0;
    };

    std::array<UsageConfig, kFrameWriteUsageCount> usages = {{
        // MaterialParams: one row per registered material, kMaterialEntryStride
        // bytes each (MaterialSsboLayout.h — the material_params.glsl block,
        // the bindless texture indices, the per-slot UV transform rows and the
        // packed sampler indices; 784 B for today's field list, and that list
        // grows by appending). The starting capacity therefore holds
        // capacityBytes / kMaterialEntryStride materials — about 1.3K, which
        // covers ordinary scenes — and grows on demand up to 32 MB (~40K).
        // The cap is per ring slot and MaterialParams has no extra reader, so
        // the worst case is 32 MB x the ring depth (128 MB at Vulkan's 3-frame
        // pacing plus the update-phase slot), and only for a scene that demands it.
        {1u << 20, 256, Rendering::BufferUsage::Storage, 32u << 20},
        // BonePalette: starts at 32 MB (~700K mat3x4 bones — see
        // BonePaletteLayout.h); grows on demand up to 256 MB if a crowd
        // scene exceeds the steady-state budget. The starting size
        // covers normal scenes (1000 chars × 50 joints × 48 B × 2
        // frames-in-flight = ~4.8 MB live with ~6x headroom). Grow-on-
        // demand removes the hard cap that previously forced overflow
        // entities to render at bind pose.
        // ExtraReaderFrames=1: the TAA skinned motion-vector pass reads LAST
        // frame's palettes to evaluate the previous skinned pose, so a slot is
        // still read one frame after the frame that wrote it.
        {32u << 20, 16, Rendering::BufferUsage::Storage, 256u << 20, 1},
    }};

    // 0 = read the device's pacing at Initialize. Only pass an explicit value
    // for a pool with self-managed pacing; Initialize derives the ring depth
    // from it and must not be handed a number that is already a depth.
    uint32_t FramesInFlight = 0;
};

class PerFrameWritePool
{
  public:
    using Allocation = Rendering::FrameBufferAllocator::Allocation;

    PerFrameWritePool() = default;
    ~PerFrameWritePool() = default;

    // Non-copyable.
    PerFrameWritePool(const PerFrameWritePool&) = delete;
    PerFrameWritePool& operator=(const PerFrameWritePool&) = delete;

    bool Initialize(Rendering::IDevice* device,
                    const PerFrameWritePoolConfig& config = {})
    {
        m_FramesInFlight = config.FramesInFlight != 0
            ? config.FramesInFlight
            : device->GetFramesInFlight();

        for (uint32_t i = 0; i < kFrameWriteUsageCount; ++i)
        {
            const auto& uc = config.usages[i];
            if (uc.capacityBytes == 0)
                continue;

            static const char* kNames[] = {
                "PerFramePool.MaterialParams",
                "PerFramePool.BonePalette",
            };

            // Every usage class is filled during the application UPDATE phase
            // (RenderingLoop::Update, before the frame's IDevice::BeginFrame), so
            // the ring carries the update-phase margin on top of the device's
            // pacing — FrameBufferAllocator::BeginFrame states the rule.
            // The clamp is a bound, not a policy: kMaxRingSlots is sized so the
            // deepest class here (one extra reader frame) still fits at the deepest
            // pacing a backend may report, so it never actually reduces the depth.
            const uint32_t slots =
                std::min(m_FramesInFlight + 1u + uc.ExtraReaderFrames,
                         Rendering::FrameBufferAllocator::kMaxRingSlots);

            // The static usage table can't consult the device; drop BDA here
            // when unsupported (readers of these buffers bail on address 0
            // and keep the SSBO binding).
            Rendering::BufferUsage bufferUsage = uc.bufferUsage;
            if (!device->GetCapabilities().supportsBufferDeviceAddress)
            {
                bufferUsage = static_cast<Rendering::BufferUsage>(
                    static_cast<uint32_t>(bufferUsage) &
                    ~static_cast<uint32_t>(Rendering::BufferUsage::ShaderDeviceAddress));
            }

            if (!m_Allocators[i].Initialize(
                    device, uc.capacityBytes, bufferUsage,
                    slots, uc.alignment, kNames[i],
                    uc.maxCapacityBytes))
            {
                Shutdown();
                return false;
            }
        }

        m_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        for (auto& alloc : m_Allocators)
            alloc.Shutdown();
        m_Initialized = false;
    }

    // Step every allocator to its next ring and reset it. Call once per frame
    // before writing. `deviceFrameIndex` is a change token, not a ring index —
    // every class owns more rings than the device's index can address
    // (see FrameBufferAllocator::BeginFrame).
    void BeginFrame(uint32_t deviceFrameIndex)
    {
        for (auto& alloc : m_Allocators)
        {
            if (alloc.IsInitialized())
                alloc.BeginFrame(deviceFrameIndex);
        }
    }

    // Suballocate from the specified usage class (single-threaded).
    Allocation Allocate(FrameWriteUsage usage, size_t bytes, size_t alignment = 0)
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        assert(m_Allocators[idx].IsInitialized());
        return m_Allocators[idx].Allocate(bytes, alignment);
    }

    // Reserve total capacity before allocating or publishing the buffer binding.
    bool Reserve(FrameWriteUsage usage, size_t capacityBytes)
    {
        if (!m_Initialized)
            return false;
        const auto idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        assert(m_Allocators[idx].IsInitialized());
        return m_Allocators[idx].Reserve(capacityBytes);
    }

    // Get the buffer handle for the current frame of a specific usage class.
    Rendering::BufferHandle GetBuffer(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        return m_Allocators[idx].GetCurrentBuffer();
    }

    // Last frame's buffer for a usage class, still holding that frame's bytes.
    // Only valid — and only safe for GPU work to read — while
    // HasPreviousFrameBuffer() is true for the same usage.
    Rendering::BufferHandle GetPreviousBuffer(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        return m_Allocators[idx].GetPreviousBuffer();
    }

    // False on the first frame, after a device rebuild, and for any usage
    // class that did not get the extra slot its prev-frame read requires.
    bool HasPreviousFrameBuffer(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        return m_Allocators[idx].IsInitialized() &&
               m_Allocators[idx].GetSlotCount() > m_FramesInFlight + 1u &&
               m_Allocators[idx].HasPreviousFrame();
    }

    size_t GetPreviousCapacity(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        return m_Allocators[idx].GetPreviousCapacity();
    }

    // Current frame slot for a specific usage class. Advances by one per device frame
    // and wraps at that class's ring depth, which is deeper than the device's pacing.
    uint32_t GetCurrentSlot(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        assert(idx < kFrameWriteUsageCount);
        return m_Allocators[idx].GetCurrentSlot();
    }

    // Query current usage for a specific class.
    size_t GetCurrentUsage(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        return m_Allocators[idx].GetCurrentUsage();
    }

    // Query capacity for a specific class.
    size_t GetCapacity(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        return m_Allocators[idx].GetCapacity();
    }

    // Grow-on-demand ceiling for a specific class; 0 when growth is disabled.
    size_t GetMaxCapacity(FrameWriteUsage usage) const
    {
        const uint32_t idx = static_cast<uint32_t>(usage);
        return m_Allocators[idx].GetMaxCapacity();
    }

    bool IsInitialized() const { return m_Initialized; }

  private:
    std::array<Rendering::FrameBufferAllocator, kFrameWriteUsageCount> m_Allocators;
    uint32_t m_FramesInFlight = 0;
    bool m_Initialized = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
