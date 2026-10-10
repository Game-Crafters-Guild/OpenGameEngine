/**
 * @file VulkanMemory.cpp
 * @brief Vulkan memory management utilities
 */

#include "VulkanDevice.h"
#include <iostream>
#include <algorithm>

namespace GameEngine::Rendering {

    // Vulkan memory utilities
    namespace VulkanMemoryUtils {

        uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
                if ((typeFilter & (1 << i)) &&
                    (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                    return i;
                }
            }

            throw std::runtime_error("Failed to find suitable memory type!");
        }

        VkDeviceSize GetAlignedSize(VkDeviceSize size, VkDeviceSize alignment) {
            return (size + alignment - 1) & ~(alignment - 1);
        }

        bool IsMemoryTypeHostVisible(VkPhysicalDevice physicalDevice, uint32_t memoryTypeIndex) {
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            if (memoryTypeIndex >= memProperties.memoryTypeCount) {
                return false;
            }

            return (memProperties.memoryTypes[memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        }

        VkDeviceSize GetMemoryBudget(VkPhysicalDevice physicalDevice, uint32_t memoryTypeIndex) {
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            if (memoryTypeIndex >= memProperties.memoryTypeCount) {
                return 0;
            }

            uint32_t heapIndex = memProperties.memoryTypes[memoryTypeIndex].heapIndex;
            if (heapIndex >= memProperties.memoryHeapCount) {
                return 0;
            }

            return memProperties.memoryHeaps[heapIndex].size;
        }

        void PrintMemoryStats(VkPhysicalDevice physicalDevice) {
            VkPhysicalDeviceMemoryProperties memProperties;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            // Quiet mode: omit verbose memory property dump
        }

    } // namespace VulkanMemoryUtils

} // namespace GameEngine::Rendering
