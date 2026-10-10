#include "Assets/Textures/BC7TransparentEndpointRepair.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace GameEngine {

namespace
{
    constexpr uint32 kBlockDim = 4;
    constexpr size_t kBlockBytes = 16;
    constexpr size_t kTexelBytes = 4;
    constexpr size_t kAlphaOffset = 3;

    // Where one alpha mode with p-bits stores its endpoint alphas and p-bits. Endpoint e
    // keeps its alpha bits at FirstAlphaBit + e * AlphaBits and its p-bit at FirstPBit + e.
    struct EndpointAlphaLayout
    {
        uint32 AlphaBits;
        uint32 FirstAlphaBit;
        uint32 FirstPBit;
        uint32 EndpointCount;
    };

    constexpr EndpointAlphaLayout kMode6Layout{7, 49, 63, 2};
    constexpr EndpointAlphaLayout kMode7Layout{5, 74, 94, 4};

    // The mode is the position of the lowest set bit: mode 6 is 0b1000000, mode 7 is 0b10000000.
    constexpr uint8 kMode6Mask = 0x7F;
    constexpr uint8 kMode6Bits = 0x40;
    constexpr uint8 kMode7Bits = 0x80;

    const EndpointAlphaLayout* FindEndpointAlphaLayout(const uint8* block)
    {
        if ((block[0] & kMode6Mask) == kMode6Bits)
            return &kMode6Layout;
        if (block[0] == kMode7Bits)
            return &kMode7Layout;
        return nullptr;
    }

    bool ReadBit(const uint8* block, uint32 bit)
    {
        return ((block[bit / 8] >> (bit % 8)) & 1u) != 0;
    }

    bool AreBitsZero(const uint8* block, uint32 firstBit, uint32 count)
    {
        for (uint32 bit = firstBit; bit < firstBit + count; ++bit)
            if (ReadBit(block, bit))
                return false;
        return true;
    }

    void ClearBit(uint8* block, uint32 bit)
    {
        block[bit / 8] = static_cast<uint8>(block[bit / 8] & ~(1u << (bit % 8)));
    }

    // Only texels inside the image count: the encoder pads a partial edge block with
    // texels that are not part of the source.
    bool BlockHasTransparentTexel(const uint8* rgba, uint32 width, uint32 height, uint32 blockX, uint32 blockY)
    {
        const uint32 x0 = blockX * kBlockDim;
        const uint32 y0 = blockY * kBlockDim;
        const uint32 x1 = std::min(x0 + kBlockDim, width);
        const uint32 y1 = std::min(y0 + kBlockDim, height);
        for (uint32 y = y0; y < y1; ++y)
            for (uint32 x = x0; x < x1; ++x)
                if (rgba[(static_cast<size_t>(y) * width + x) * kTexelBytes + kAlphaOffset] == 0)
                    return true;
        return false;
    }

    void ClearTransparentEndpointPBits(uint8* block, const EndpointAlphaLayout& layout)
    {
        for (uint32 endpoint = 0; endpoint < layout.EndpointCount; ++endpoint)
        {
            const uint32 pBit = layout.FirstPBit + endpoint;
            if (AreBitsZero(block, layout.FirstAlphaBit + endpoint * layout.AlphaBits, layout.AlphaBits) &&
                ReadBit(block, pBit))
                ClearBit(block, pBit);
        }
    }
}

void RepairBC7TransparentEndpoints(const uint8* rgba, uint32 width, uint32 height, std::span<uint8> blocks)
{
    const uint32 blocksWide = (width + kBlockDim - 1) / kBlockDim;
    const uint32 blocksHigh = (height + kBlockDim - 1) / kBlockDim;
    const size_t expectedBytes = static_cast<size_t>(blocksWide) * blocksHigh * kBlockBytes;
    assert(blocks.size() == expectedBytes && "BC7 payload does not match the source extent");
    if (blocks.size() != expectedBytes)
        return;

    for (uint32 blockY = 0; blockY < blocksHigh; ++blockY)
    {
        for (uint32 blockX = 0; blockX < blocksWide; ++blockX)
        {
            uint8* block = blocks.data() + (static_cast<size_t>(blockY) * blocksWide + blockX) * kBlockBytes;
            const EndpointAlphaLayout* layout = FindEndpointAlphaLayout(block);
            if (layout && BlockHasTransparentTexel(rgba, width, height, blockX, blockY))
                ClearTransparentEndpointPBits(block, *layout);
        }
    }
}

} // namespace GameEngine
