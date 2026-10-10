#pragma once

// Small utilities over the engine's real resource descs, used by the RenderGraph pools.
// Desc equality (for pool match / desc-change detection) and a byte-size estimate
// (for budget + stats). These operate on the engine's TextureDesc/BufferDesc
// directly — RenderGraph binds to the real device types, no parallel descriptors.

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{

inline bool DescEqual(const TextureDesc& a, const TextureDesc& b)
{
    return a.width == b.width && a.height == b.height && a.depth == b.depth &&
           a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers && a.format == b.format &&
           a.usage == b.usage && a.sampleCount == b.sampleCount && a.flags == b.flags &&
           a.initialState == b.initialState && a.persistent == b.persistent;
}

inline bool DescEqual(const BufferDesc& a, const BufferDesc& b)
{
    return a.size == b.size && a.usage == b.usage && a.memoryUsage == b.memoryUsage &&
           a.flags == b.flags && a.stride == b.stride;
}

inline uint64_t EstimateBytes(const TextureDesc& d)
{
    // Sum the whole mip chain — base-level-only undercounts a full chain by ~33%,
    // which matters for pool budgets/eviction.
    const uint64_t bpp = BytesPerPixel(static_cast<TextureFormat>(d.format));
    uint64_t bytes = 0;
    const uint32_t mips = d.mipLevels ? d.mipLevels : 1;
    for (uint32_t m = 0; m < mips; ++m)
    {
        const uint64_t w = d.width >> m ? d.width >> m : 1;
        const uint64_t h = d.height >> m ? d.height >> m : 1;
        const uint64_t z = d.depth >> m ? d.depth >> m : 1;
        bytes += w * h * z * d.arrayLayers * d.sampleCount * bpp;
    }
    return bytes;
}

inline uint64_t EstimateBytes(const BufferDesc& d) { return d.size; }

} // namespace GameEngine::Rendering::RenderGraph
