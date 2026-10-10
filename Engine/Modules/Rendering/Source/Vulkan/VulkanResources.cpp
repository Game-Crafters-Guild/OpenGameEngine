/**
 * @file VulkanResources.cpp
 * @brief Vulkan resource utilities and helpers
 */

#include "Rendering/Core/Device.h"
#include "VulkanDevice.h"
#include <iostream>

namespace GameEngine::Rendering
{

// Vulkan resource utilities
namespace VulkanResourceUtils
{

VkFormat GetVulkanFormat(Format format)
{
    switch (format)
    {
    case Format::R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16G16B16A16_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32G32B32A32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32G32B32_FLOAT:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::R32G32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case Format::R16G16B16A16_UINT:
        return VK_FORMAT_R16G16B16A16_UINT;
    case Format::D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case Format::D24_UNORM_S8_UINT:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkFormat GetVulkanFormat(TextureFormat format)
{
    switch (format)
    {
    // 8-bit
    case TextureFormat::R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case TextureFormat::R8G8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::BGRA8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    // 10/11-bit packed
    case TextureFormat::R11G11B10_FLOAT:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case TextureFormat::RGB10A2_UNORM:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // 16-bit
    case TextureFormat::R16_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::R16G16_FLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case TextureFormat::R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::R32G32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case TextureFormat::R16G16B16A16_FLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R16G16B16A16_UNORM:
        return VK_FORMAT_R16G16B16A16_UNORM;
    // 32-bit float
    case TextureFormat::R32G32B32A32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    // Integer color formats
    case TextureFormat::R8_UINT:
        return VK_FORMAT_R8_UINT;
    case TextureFormat::R8_SINT:
        return VK_FORMAT_R8_SINT;
    case TextureFormat::R8G8_UINT:
        return VK_FORMAT_R8G8_UINT;
    case TextureFormat::R8G8_SINT:
        return VK_FORMAT_R8G8_SINT;
    case TextureFormat::R16_UINT:
        return VK_FORMAT_R16_UINT;
    case TextureFormat::R16_SINT:
        return VK_FORMAT_R16_SINT;
    case TextureFormat::R16G16_UINT:
        return VK_FORMAT_R16G16_UINT;
    case TextureFormat::R16G16_SINT:
        return VK_FORMAT_R16G16_SINT;
    case TextureFormat::RGBA16_UINT:
        return VK_FORMAT_R16G16B16A16_UINT;
    case TextureFormat::RGBA16_SINT:
        return VK_FORMAT_R16G16B16A16_SINT;
    case TextureFormat::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case TextureFormat::R32_SINT:
        return VK_FORMAT_R32_SINT;
    case TextureFormat::R32G32_UINT:
        return VK_FORMAT_R32G32_UINT;
    case TextureFormat::R32G32_SINT:
        return VK_FORMAT_R32G32_SINT;
    case TextureFormat::R32G32B32_UINT:
        return VK_FORMAT_R32G32B32_UINT;
    case TextureFormat::R32G32B32_SINT:
        return VK_FORMAT_R32G32B32_SINT;
    case TextureFormat::RGBA32_UINT:
        return VK_FORMAT_R32G32B32A32_UINT;
    case TextureFormat::RGBA32_SINT:
        return VK_FORMAT_R32G32B32A32_SINT;
    // 8-bit normalized/SRGB integer variants
    case TextureFormat::RGBA8_UINT:
        return VK_FORMAT_R8G8B8A8_UINT;
    case TextureFormat::RGBA8_SINT:
        return VK_FORMAT_R8G8B8A8_SINT;
    // Depth/stencil
    case TextureFormat::D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::D24_UNORM_S8_UINT:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::D32_SFLOAT_S8_UINT:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case TextureFormat::D16_UNORM:
        return VK_FORMAT_D16_UNORM;
    case TextureFormat::S8_UINT:
        return VK_FORMAT_S8_UINT;
    case TextureFormat::X8_D24_UNORM_PACK32:
        return VK_FORMAT_X8_D24_UNORM_PACK32;
    // Block compressed
    case TextureFormat::BC1_UNORM:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case TextureFormat::BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::BC7_SRGB:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case TextureFormat::BC1_SRGB:
        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case TextureFormat::BC4_UNORM:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case TextureFormat::BC6H_UF16:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

VkBufferUsageFlags GetVulkanBufferUsage(BufferUsage usage)
{
    VkBufferUsageFlags flags = 0;

    if (static_cast<uint32_t>(usage & BufferUsage::Vertex))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (static_cast<uint32_t>(usage & BufferUsage::Index))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (static_cast<uint32_t>(usage & BufferUsage::Uniform))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (static_cast<uint32_t>(usage & BufferUsage::Storage))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (static_cast<uint32_t>(usage & BufferUsage::Indirect))
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    // Always add transfer bits for flexibility
    flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    return flags;
}

VkMemoryPropertyFlags GetVulkanMemoryProperties(MemoryType memoryType)
{
    switch (memoryType)
    {
    case MemoryType::DeviceLocal:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    case MemoryType::HostVisible:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    case MemoryType::HostCached:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    default:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
}

} // namespace VulkanResourceUtils

} // namespace GameEngine::Rendering
