/**
 * @file VulkanDescriptorBufferPool.h
 * @brief Backend-internal sub-allocator for VK_EXT_descriptor_buffer regions.
 *
 * Allocates from a SINGLE backing VkBuffer carved into a persistent region
 * plus one per-frame region per frame-in-flight. All descriptor-buffer
 * allocations returned by this pool therefore share the same VkBuffer, so
 * downstream command-buffer binding only needs to issue
 * vkCmdBindDescriptorBuffersEXT once per command buffer (the offsets for
 * each set are set via vkCmdSetDescriptorBufferOffsetsEXT and do not
 * invalidate each other when they share a buffer).
 *
 * Frame partitioning: BeginFrameReset(frameIndex) rewinds the cursor for
 * that frame's region so the next frame's allocations reuse the same
 * memory. Persistent region (AllocatePersistent) is never rewound and
 * holds long-lived descriptor sets (e.g. the global bindless texture
 * array, cached depth-pass set0).
 *
 * Overflow: if a region runs out, a spill block (separate VkBuffer) is
 * added. Downstream code handles the buffer switch via rebind — the
 * expectation is overflow is rare; tune region sizes via Config if it
 * starts firing regularly.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 6326 6386 6387)
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <vulkan/vulkan.h>

namespace GameEngine::Rendering
{

class VulkanDevice;

struct DescriptorBufferAllocation
{
    VkBuffer        Buffer     = VK_NULL_HANDLE;   // VkBuffer containing the region.
    VkDeviceAddress Address    = 0;                // device address of Buffer + Offset.
    VkDeviceSize    Offset     = 0;                // byte offset from start of Buffer.
    void*           HostMapped = nullptr;          // host-writable pointer to Offset.
    VkDeviceSize    Size       = 0;                // allocation size in bytes.

    bool IsValid() const { return Buffer != VK_NULL_HANDLE && HostMapped != nullptr; }
};

class VulkanDescriptorBufferPool
{
  public:
    struct Config
    {
        // Size of each region (persistent + each per-frame ring). The single
        // backing VkBuffer is sized as BlockSize * (1 + FramesInFlight). If a
        // region runs out, a separate spill block of this size is allocated.
        VkDeviceSize BlockSize         = 4 * 1024 * 1024; // 4 MiB per region
        // 0 = derive from the device's frames-in-flight at Initialize. A pool
        // with fewer slots than the device paces would rewind a region the
        // GPU is still reading — never default below the device's count.
        uint32_t     FramesInFlight    = 0;
        VkBufferUsageFlags UsageFlags  = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
                                       | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    };

    VulkanDescriptorBufferPool() = default;
    ~VulkanDescriptorBufferPool();

    VulkanDescriptorBufferPool(const VulkanDescriptorBufferPool&) = delete;
    VulkanDescriptorBufferPool& operator=(const VulkanDescriptorBufferPool&) = delete;

    bool Initialize(VulkanDevice& device, const Config& cfg);
    void Destroy();

    // Set the current frame index used for subsequent Allocate() calls.
    void SetCurrentFrame(uint32_t frameIndex);

    // Rewind the per-frame region for frameIndex so its memory is reusable
    // on the next pass through. Also drops any per-frame spill blocks for
    // that frame.
    void BeginFrameReset(uint32_t frameIndex);

    // Allocate a contiguous region of the given size, aligned to the device's
    // descriptor-buffer offset alignment (descriptorBufferOffsetAlignment) or a
    // larger caller-specified alignment, whichever is greater. Falls through
    // to a spill block when the primary region is exhausted. Thread-safe via
    // m_Mutex.
    DescriptorBufferAllocation Allocate(VkDeviceSize size, VkDeviceSize alignment);

    // Application-lifetime allocation. Draws from the persistent region of
    // the backing buffer, which is never rewound by BeginFrameReset and stays
    // valid until Destroy(). Intended for long-lived descriptor sets like the
    // global bindless texture array and cached cross-frame draw sets. Memory
    // accumulates — callers are expected to allocate a bounded set of
    // persistent regions, not grow unboundedly per frame.
    DescriptorBufferAllocation AllocatePersistent(VkDeviceSize size, VkDeviceSize alignment);

    VkDeviceSize OffsetAlignment() const { return m_OffsetAlignment; }

    // Backing buffer accessor (internal tests). Returns the single VkBuffer
    // that all non-spill allocations share.
    VkBuffer GetBackingBuffer() const { return m_BackingBuffer; }

  private:
    // Primary region — a sub-range of m_BackingBuffer with a bump cursor.
    struct Region
    {
        VkDeviceSize BaseOffset = 0; // byte offset within m_BackingBuffer
        VkDeviceSize Size       = 0; // region capacity
        VkDeviceSize Cursor     = 0; // bump-alloc cursor (0-based within region)
    };

    // Spill block — a standalone VkBuffer added on region overflow.
    struct SpillBlock
    {
        VkBuffer        Buffer     = VK_NULL_HANDLE;
        VmaAllocation   Allocation = VK_NULL_HANDLE;
        VkDeviceAddress Address    = 0;
        void*           HostMapped = nullptr;
        VkDeviceSize    Size       = 0;
        VkDeviceSize    Cursor     = 0;
    };

    struct FrameState
    {
        Region                  Primary;      // inside m_BackingBuffer
        std::vector<SpillBlock> SpillBlocks;  // rare overflow path
    };

    bool CreateBackingBufferLocked(VkDeviceSize totalSize);
    bool EnsureBackingBufferLocked();
    void DestroyBackingBufferLocked();
    // requestSize is the allocation that overflowed the region, not the block size:
    // the block is sized to max(Config::BlockSize, requestSize).
    bool PushSpillBlockLocked(FrameState& frame, VkDeviceSize requestSize);
    void DestroySpillBlocksLocked(FrameState& frame);

    // Bump-allocate from the primary region first; fall back to a spill block
    // on overflow. Caller must hold m_Mutex.
    DescriptorBufferAllocation AllocateFromFrameLocked(FrameState& frame,
                                                       VkDeviceSize size,
                                                       VkDeviceSize effectiveAlign);

    VulkanDevice*            m_Device             = nullptr;
    Config                   m_Config;
    VkDeviceSize             m_OffsetAlignment    = 0;

    // Single backing VkBuffer shared by persistent + all per-frame regions.
    VkBuffer                 m_BackingBuffer      = VK_NULL_HANDLE;
    VmaAllocation            m_BackingAllocation  = VK_NULL_HANDLE;
    VkDeviceAddress          m_BackingAddress     = 0;
    void*                    m_BackingHostMapped  = nullptr;
    VkDeviceSize             m_BackingSize        = 0;

    std::vector<FrameState>  m_Frames;
    FrameState               m_Persistent;        // Never rewound; app-lifetime sets.
    uint32_t                 m_CurrentFrame       = 0;
    std::mutex               m_Mutex;
    bool                     m_Initialized        = false;
};

} // namespace GameEngine::Rendering
