#pragma once

#include "Rendering/Core/Device.h"
#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{

/// "ShaderResource|RenderTarget" for a TextureDesc::usage mask; "None" for 0.
inline std::string FormatTextureUsage(uint32_t usageFlags)
{
    struct Bit { TextureUsage Value; const char* Name; };
    static constexpr Bit kBits[] = {
        {TextureUsage::ShaderResource, "ShaderResource"}, {TextureUsage::RenderTarget, "RenderTarget"},
        {TextureUsage::DepthStencil, "DepthStencil"},     {TextureUsage::UnorderedAccess, "UnorderedAccess"},
        {TextureUsage::TransferSrc, "TransferSrc"},       {TextureUsage::TransferDst, "TransferDst"},
    };
    std::string out;
    for (const Bit& bit : kBits)
    {
        if ((usageFlags & static_cast<uint32_t>(bit.Value)) == 0)
            continue;
        if (!out.empty())
            out += '|';
        out += bit.Name;
    }
    return out.empty() ? std::string("None") : out;
}

inline const char* ToString(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::Unknown:            return "Unknown";
    case TextureFormat::RGBA8_UNORM:        return "RGBA8_UNORM";
    case TextureFormat::RGBA8_SRGB:         return "RGBA8_SRGB";
    case TextureFormat::BGRA8_UNORM:        return "BGRA8_UNORM";
    case TextureFormat::BGRA8_SRGB:         return "BGRA8_SRGB";
    case TextureFormat::R8_UINT:            return "R8_UINT";
    case TextureFormat::R8_SINT:            return "R8_SINT";
    case TextureFormat::R8G8_UINT:          return "R8G8_UINT";
    case TextureFormat::R8G8_SINT:          return "R8G8_SINT";
    case TextureFormat::RGBA8_UINT:         return "RGBA8_UINT";
    case TextureFormat::RGBA8_SINT:         return "RGBA8_SINT";
    case TextureFormat::R16_UINT:           return "R16_UINT";
    case TextureFormat::R16_SINT:           return "R16_SINT";
    case TextureFormat::R16G16_UINT:        return "R16G16_UINT";
    case TextureFormat::R16G16_SINT:        return "R16G16_SINT";
    case TextureFormat::RGBA16_UINT:        return "RGBA16_UINT";
    case TextureFormat::RGBA16_SINT:        return "RGBA16_SINT";
    case TextureFormat::R32_UINT:           return "R32_UINT";
    case TextureFormat::R32_SINT:           return "R32_SINT";
    case TextureFormat::R32G32_UINT:        return "R32G32_UINT";
    case TextureFormat::R32G32_SINT:        return "R32G32_SINT";
    case TextureFormat::R32G32B32_UINT:     return "R32G32B32_UINT";
    case TextureFormat::R32G32B32_SINT:     return "R32G32B32_SINT";
    case TextureFormat::RGBA32_UINT:        return "RGBA32_UINT";
    case TextureFormat::RGBA32_SINT:        return "RGBA32_SINT";
    case TextureFormat::R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case TextureFormat::R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case TextureFormat::R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case TextureFormat::R11G11B10_FLOAT:    return "R11G11B10_FLOAT";
    case TextureFormat::RGB10A2_UNORM:      return "RGB10A2_UNORM";
    case TextureFormat::R16_FLOAT:          return "R16_FLOAT";
    case TextureFormat::R16G16_FLOAT:       return "R16G16_FLOAT";
    case TextureFormat::R32_FLOAT:          return "R32_FLOAT";
    case TextureFormat::R8_UNORM:           return "R8_UNORM";
    case TextureFormat::R8G8_UNORM:         return "R8G8_UNORM";
    case TextureFormat::D32_FLOAT:          return "D32_FLOAT";
    case TextureFormat::D24_UNORM_S8_UINT:  return "D24_UNORM_S8_UINT";
    case TextureFormat::D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    case TextureFormat::BC1_UNORM:          return "BC1_UNORM";
    case TextureFormat::BC1_SRGB:           return "BC1_SRGB";
    case TextureFormat::BC3_UNORM:          return "BC3_UNORM";
    case TextureFormat::BC4_UNORM:          return "BC4_UNORM";
    case TextureFormat::BC5_UNORM:          return "BC5_UNORM";
    case TextureFormat::BC6H_UF16:          return "BC6H_UF16";
    case TextureFormat::BC7_UNORM:          return "BC7_UNORM";
    case TextureFormat::BC7_SRGB:           return "BC7_SRGB";
    case TextureFormat::D16_UNORM:          return "D16_UNORM";
    case TextureFormat::S8_UINT:            return "S8_UINT";
    case TextureFormat::X8_D24_UNORM_PACK32: return "X8_D24_UNORM_PACK32";
    case TextureFormat::R32G32_FLOAT:       return "R32G32_FLOAT";
    default:                                return "Unknown";
    }
}

inline const char* ToString(BufferMemoryUsage m)
{
    switch (m)
    {
    case BufferMemoryUsage::Auto:        return "Auto";
    case BufferMemoryUsage::DeviceLocal: return "DeviceLocal";
    case BufferMemoryUsage::Upload:      return "Upload";
    case BufferMemoryUsage::Readback:    return "Readback";
    case BufferMemoryUsage::UploadDeviceLocalPreferred: return "UploadDeviceLocalPreferred";
    default:                             return "Unknown";
    }
}

inline const char* ToString(ResourceState s)
{
    switch (s)
    {
    case ResourceState::Undefined:       return "Undefined";
    case ResourceState::Common:          return "Common";
    case ResourceState::VertexBuffer:    return "VertexBuffer";
    case ResourceState::IndexBuffer:     return "IndexBuffer";
    case ResourceState::ConstantBuffer:  return "ConstantBuffer";
    case ResourceState::ShaderResource:  return "ShaderResource";
    case ResourceState::UnorderedAccess: return "UnorderedAccess";
    case ResourceState::RenderTarget:    return "RenderTarget";
    case ResourceState::DepthWrite:      return "DepthWrite";
    case ResourceState::DepthRead:       return "DepthRead";
    case ResourceState::DepthSampled:    return "DepthSampled";
    case ResourceState::CopySource:      return "CopySource";
    case ResourceState::CopyDest:        return "CopyDest";
    default:                             return "Unknown";
    }
}

inline std::string ToString(BufferCreateFlags f)
{
    if (f == BufferCreateFlags::None)
        return "None";
    std::string s;
    auto add = [&](const char* n)
    { s += (s.empty()?"":"|"); s += n; };
    auto v = static_cast<uint32_t>(f);
    if (v & (1u << 0)) add("PersistentlyMapped");
    if (v & (1u << 1)) add("Transient");
    if (v & (1u << 2)) add("AllowAlias");
    if (v & (1u << 3)) add("DedicatedAllocation");
    return s;
}

} // namespace GameEngine::Rendering
