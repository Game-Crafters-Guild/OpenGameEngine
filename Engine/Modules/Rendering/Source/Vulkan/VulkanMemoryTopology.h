#pragma once

#include "Rendering/Core/Device.h"

#include <vulkan/vulkan.h>

#include <algorithm>

namespace GameEngine::Rendering
{

/**
 * @brief Derive the device's memory arrangement from its Vulkan memory properties.
 *
 * A pure function over data the backend already queried, so the derivation is
 * assertable without a GPU. Every value describes what the device CAN do; where
 * any particular allocation LANDED is a separate question only the allocator can
 * answer (IDevice::GetBufferMemoryResidency).
 */
inline DeviceMemoryTopology DeriveMemoryTopology(const VkPhysicalDeviceMemoryProperties& memoryProperties,
                                                 VkPhysicalDeviceType deviceType)
{
    DeviceMemoryTopology topology{};

    // Unified memory is a property of the device type, not of any one heap, and
    // it is wider than the integrated-GPU test the buffer residency policy uses:
    // a software rasterizer (CPU) and a paravirtualized device (VIRTUAL_GPU)
    // also have no separate device memory to stage a copy into.
    topology.isUnifiedMemory = deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                               deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                               deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;

    // SUM, because this is the denominator: the question the gate asks is
    // whether the CPU-writable window covers ALL of the device's device-local
    // memory, and a driver is free to declare that memory as several heaps. A
    // max here would silently reduce the gate to a fraction-of-VRAM threshold on
    // any driver that partitions (RADV and ANV both carve the mappable aperture
    // out of the VRAM total and declare the remainder separately), because the
    // aperture would then be compared against the larger PART instead of the
    // whole.
    const uint32_t heapCount = std::min<uint32_t>(memoryProperties.memoryHeapCount, VK_MAX_MEMORY_HEAPS);
    for (uint32_t i = 0; i < heapCount; ++i)
    {
        if ((memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            topology.deviceLocalHeapBytesTotal += memoryProperties.memoryHeaps[i].size;
    }

    // MAX, because this is the numerator and the aperture is ONE heap. Summing
    // distinct host-visible device-local heaps would add up windows a single
    // allocation can never span, and on a driver that declares the aperture as
    // an extra heap alongside a full-size VRAM heap the sum can reach the total
    // without any one window covering the card.
    //
    // Scanned over TYPES but maxed over their HEAPS: several types differing
    // only in their cached/coherent bits routinely point at one heap.
    constexpr VkMemoryPropertyFlags kHostWritableDeviceMemory =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const uint32_t typeCount = std::min<uint32_t>(memoryProperties.memoryTypeCount, VK_MAX_MEMORY_TYPES);
    for (uint32_t i = 0; i < typeCount; ++i)
    {
        const VkMemoryType& type = memoryProperties.memoryTypes[i];
        if ((type.propertyFlags & kHostWritableDeviceMemory) != kHostWritableDeviceMemory)
            continue;
        if (type.heapIndex >= heapCount)
            continue;
        topology.largestHostVisibleDeviceLocalHeapBytes =
            std::max<uint64_t>(topology.largestHostVisibleDeviceLocalHeapBytes,
                               memoryProperties.memoryHeaps[type.heapIndex].size);
    }

    return topology;
}

} // namespace GameEngine::Rendering
