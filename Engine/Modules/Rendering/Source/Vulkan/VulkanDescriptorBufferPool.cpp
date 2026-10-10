#include "VulkanDescriptorBufferPool.h"

#include "VulkanDevice.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Rendering
{

namespace
{
inline VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize align)
{
    if (align <= 1)
        return value;
    return (value + align - 1) & ~(align - 1);
}
} // namespace

VulkanDescriptorBufferPool::~VulkanDescriptorBufferPool()
{
    Destroy();
}

bool VulkanDescriptorBufferPool::Initialize(VulkanDevice& device, const Config& cfg)
{
    if (m_Initialized)
    {
        Logger::Log::Warning("VulkanDescriptorBufferPool::Initialize called twice");
        return false;
    }
    if (!device.IsDescriptorBufferEnabled())
    {
        Logger::Log::Error("VulkanDescriptorBufferPool::Initialize: descriptor buffer feature not enabled");
        return false;
    }
    if (cfg.BlockSize == 0)
    {
        Logger::Log::Error("VulkanDescriptorBufferPool::Initialize: invalid config");
        return false;
    }

    m_Device = &device;
    m_Config = cfg;
    if (m_Config.FramesInFlight == 0)
        m_Config.FramesInFlight = device.GetFramesInFlight();
    m_OffsetAlignment = std::max<VkDeviceSize>(device.GetDescriptorBufferOffsetAlignment(), 1);

    // Set up region metadata only — the backing VkBuffer is created lazily on
    // the first Allocate*() call because VulkanDevice initialises its VMA
    // allocator after this Initialize() runs in some call orders (the pool
    // lives inside the device and Initialize is called before m_Allocator
    // finishes wiring in certain paths).
    //
    // Layout (relative offsets; absolute offset added once backing buffer exists):
    //   [0 .. perRegion)                 → m_Persistent
    //   [perRegion .. 2*perRegion)       → m_Frames[0]
    //   [2*perRegion .. 3*perRegion)     → m_Frames[1]
    //   ...
    // m_Config, not cfg: the derive above may have adjusted the copy, and the
    // rest of Initialize must agree with what later frames read.
    const VkDeviceSize perRegion = AlignUp(m_Config.BlockSize, m_OffsetAlignment);

    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Persistent = FrameState{};
    m_Persistent.Primary.BaseOffset = 0;
    m_Persistent.Primary.Size       = perRegion;
    m_Persistent.Primary.Cursor     = 0;

    m_Frames.clear();
    m_Frames.resize(m_Config.FramesInFlight);
    for (uint32_t i = 0; i < m_Config.FramesInFlight; ++i)
    {
        m_Frames[i].Primary.BaseOffset = perRegion * (VkDeviceSize{1} + i);
        m_Frames[i].Primary.Size       = perRegion;
        m_Frames[i].Primary.Cursor     = 0;
    }

    m_CurrentFrame = 0;
    m_Initialized = true;
    return true;
}

bool VulkanDescriptorBufferPool::EnsureBackingBufferLocked()
{
    if (m_BackingBuffer != VK_NULL_HANDLE)
        return true;
    const VkDeviceSize perRegion = AlignUp(m_Config.BlockSize, m_OffsetAlignment);
    const VkDeviceSize totalSize = perRegion * (VkDeviceSize{1} + m_Config.FramesInFlight);
    return CreateBackingBufferLocked(totalSize);
}

void VulkanDescriptorBufferPool::Destroy()
{
    if (!m_Initialized)
        return;

    std::lock_guard<std::mutex> lk(m_Mutex);
    for (auto& frame : m_Frames)
    {
        DestroySpillBlocksLocked(frame);
    }
    m_Frames.clear();
    DestroySpillBlocksLocked(m_Persistent);
    DestroyBackingBufferLocked();
    m_Device = nullptr;
    m_Initialized = false;
}

void VulkanDescriptorBufferPool::SetCurrentFrame(uint32_t frameIndex)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    assert(frameIndex < m_Frames.size());
    m_CurrentFrame = frameIndex % static_cast<uint32_t>(m_Frames.size());
}

void VulkanDescriptorBufferPool::BeginFrameReset(uint32_t frameIndex)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (frameIndex >= m_Frames.size())
        return;
    auto& frame = m_Frames[frameIndex];
    // Rewind primary region and drop any spill blocks accumulated last time
    // this frame slot was active. Spill blocks are only created under
    // overflow; when they fire, they log a warning so we can grow BlockSize.
    frame.Primary.Cursor = 0;
    DestroySpillBlocksLocked(frame);
}

DescriptorBufferAllocation VulkanDescriptorBufferPool::AllocateFromFrameLocked(
    FrameState& frame, VkDeviceSize size, VkDeviceSize effectiveAlign)
{
    if (!EnsureBackingBufferLocked())
        return {};

    // Primary region bump-allocate: offsets within the single backing buffer.
    {
        const VkDeviceSize regionOffsetStart = AlignUp(frame.Primary.Cursor, effectiveAlign);
        if (regionOffsetStart + size <= frame.Primary.Size)
        {
            const VkDeviceSize absOffset = frame.Primary.BaseOffset + regionOffsetStart;

            DescriptorBufferAllocation out;
            out.Buffer     = m_BackingBuffer;
            out.Address    = m_BackingAddress + absOffset;
            out.Offset     = absOffset;
            out.HostMapped = static_cast<uint8_t*>(m_BackingHostMapped) + absOffset;
            out.Size       = size;

            frame.Primary.Cursor = regionOffsetStart + size;
            return out;
        }
    }

    // Overflow → spill block (separate VkBuffer). Reuse the tail spill block if it
    // has room; otherwise push a new one, which is where the spill is reported.
    SpillBlock* block = frame.SpillBlocks.empty() ? nullptr : &frame.SpillBlocks.back();
    if (block)
    {
        const VkDeviceSize aligned = AlignUp(block->Cursor, effectiveAlign);
        if (aligned + size > block->Size)
            block = nullptr;
    }
    if (!block)
    {
        if (!PushSpillBlockLocked(frame, size))
            return {};
        block = &frame.SpillBlocks.back();
    }

    const VkDeviceSize aligned = AlignUp(block->Cursor, effectiveAlign);
    DescriptorBufferAllocation out;
    out.Buffer     = block->Buffer;
    out.Address    = block->Address + aligned;
    out.Offset     = aligned;
    out.HostMapped = static_cast<uint8_t*>(block->HostMapped) + aligned;
    out.Size       = size;
    block->Cursor  = aligned + size;
    return out;
}

DescriptorBufferAllocation VulkanDescriptorBufferPool::Allocate(VkDeviceSize size, VkDeviceSize alignment)
{
    if (!m_Initialized || size == 0)
        return {};
    const VkDeviceSize effectiveAlign = std::max<VkDeviceSize>(alignment, m_OffsetAlignment);
    std::lock_guard<std::mutex> lk(m_Mutex);
    return AllocateFromFrameLocked(m_Frames[m_CurrentFrame], size, effectiveAlign);
}

DescriptorBufferAllocation VulkanDescriptorBufferPool::AllocatePersistent(VkDeviceSize size, VkDeviceSize alignment)
{
    if (!m_Initialized || size == 0)
        return {};
    const VkDeviceSize effectiveAlign = std::max<VkDeviceSize>(alignment, m_OffsetAlignment);
    std::lock_guard<std::mutex> lk(m_Mutex);
    return AllocateFromFrameLocked(m_Persistent, size, effectiveAlign);
}

bool VulkanDescriptorBufferPool::CreateBackingBufferLocked(VkDeviceSize totalSize)
{
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = totalSize;
    bi.usage       = m_Config.UsageFlags;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
             | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    VkResult r = vmaCreateBuffer(m_Device->GetVmaAllocator(), &bi, &ai, &buf, &alloc, &allocInfo);
    if (r != VK_SUCCESS)
    {
        Logger::Log::Error("VulkanDescriptorBufferPool: vmaCreateBuffer (backing) failed ({}) size={}",
                           static_cast<int>(r), static_cast<uint64_t>(totalSize));
        return false;
    }

    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = buf;
    const VkDeviceAddress addr = vkGetBufferDeviceAddress(m_Device->GetVkDevice(), &addrInfo);

    m_BackingBuffer     = buf;
    m_BackingAllocation = alloc;
    m_BackingAddress    = addr;
    m_BackingHostMapped = allocInfo.pMappedData;
    m_BackingSize       = totalSize;
    return true;
}

void VulkanDescriptorBufferPool::DestroyBackingBufferLocked()
{
    if (m_BackingBuffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(m_Device->GetVmaAllocator(), m_BackingBuffer, m_BackingAllocation);
        m_BackingBuffer     = VK_NULL_HANDLE;
        m_BackingAllocation = VK_NULL_HANDLE;
        m_BackingAddress    = 0;
        m_BackingHostMapped = nullptr;
        m_BackingSize       = 0;
    }
}

bool VulkanDescriptorBufferPool::PushSpillBlockLocked(FrameState& frame, VkDeviceSize requestSize)
{
    // The two regions exhaust for different reasons and have different remedies: a
    // per-frame region is rewound every frame, so a larger one ends the spilling,
    // while the persistent region is never rewound and a larger one only postpones it.
    const bool  persistent = (&frame == &m_Persistent);
    const char* regionName = persistent ? "persistent" : "per-frame";
    const char* remedy     = persistent
                               ? "the persistent region is never rewound, so allocate fewer "
                                 "app-lifetime sets; a larger Config::BlockSize only postpones this"
                               : "raise Config::BlockSize";

    const VkDeviceSize size = std::max<VkDeviceSize>(m_Config.BlockSize, requestSize);

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = m_Config.UsageFlags;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
             | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    VkResult r = vmaCreateBuffer(m_Device->GetVmaAllocator(), &bi, &ai, &buf, &alloc, &allocInfo);
    if (r != VK_SUCCESS)
    {
        Logger::Log::Error(
            "VulkanDescriptorBufferPool: vmaCreateBuffer ({} region spill) failed ({}) size={}",
            regionName, static_cast<int>(r), static_cast<uint64_t>(size));
        return false;
    }

    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = buf;
    const VkDeviceAddress addr = vkGetBufferDeviceAddress(m_Device->GetVkDevice(), &addrInfo);

    SpillBlock b;
    b.Buffer     = buf;
    b.Allocation = alloc;
    b.Address    = addr;
    b.HostMapped = allocInfo.pMappedData;
    b.Size       = size;
    b.Cursor     = 0;
    frame.SpillBlocks.push_back(b);

    // One line per block, not per allocation. Once a region is full every subsequent
    // allocation takes the spill path, so reporting at the allocation site is unbounded;
    // a new VkBuffer is the event that costs something. Each block is another descriptor
    // buffer the command list must bind and counts against the device's
    // maxResourceDescriptorBufferBindings (spec floor 3) — the bind path refuses sets
    // once that limit is reached.
    Logger::Log::Warning(
        "VulkanDescriptorBufferPool: {} region full (capacity={}, request={}); spill block #{} "
        "added ({} bytes) — {}. Each spill block counts against the device's "
        "maxResourceDescriptorBufferBindings.",
        regionName,
        static_cast<uint64_t>(frame.Primary.Size),
        static_cast<uint64_t>(requestSize),
        static_cast<uint64_t>(frame.SpillBlocks.size()),
        static_cast<uint64_t>(size),
        remedy);
    return true;
}

void VulkanDescriptorBufferPool::DestroySpillBlocksLocked(FrameState& frame)
{
    for (auto& b : frame.SpillBlocks)
    {
        if (b.Buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_Device->GetVmaAllocator(), b.Buffer, b.Allocation);
    }
    frame.SpillBlocks.clear();
}

} // namespace GameEngine::Rendering
