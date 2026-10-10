// RepairBC7TransparentEndpoints on hand-built blocks: no encoder, so these run on
// every platform. The encoder half (DirectXTex output decoded back) is
// TextureCookBc.Bc7TransparentTexelsDecodeToZeroAlpha in TextureCookTests.cpp.

#include <gtest/gtest.h>

#include "Assets/Textures/BC7TransparentEndpointRepair.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

using namespace GameEngine;

namespace
{

using Block = std::array<uint8, 16>;

constexpr uint32 kMode6AlphaBits = 7;
constexpr uint32 kMode6FirstAlphaBit = 49;
constexpr uint32 kMode6FirstPBit = 63;
constexpr uint32 kMode7AlphaBits = 5;
constexpr uint32 kMode7FirstAlphaBit = 74;
constexpr uint32 kMode7FirstPBit = 94;

// Writes `value` into `count` bits starting at `firstBit`, lowest bit first (BC7 order).
void SetBits(Block& block, uint32 firstBit, uint32 count, uint32 value)
{
    for (uint32 i = 0; i < count; ++i)
    {
        const uint32 bit = firstBit + i;
        const uint8 mask = static_cast<uint8>(1u << (bit % 8));
        if ((value >> i) & 1u)
            block[bit / 8] = static_cast<uint8>(block[bit / 8] | mask);
        else
            block[bit / 8] = static_cast<uint8>(block[bit / 8] & ~mask);
    }
}

// A mode 6 block: white RGB fields, the given alpha fields, both p-bits set, and a
// non-trivial index pattern so a stray write anywhere in the block shows.
Block MakeMode6Block(uint32 alpha0, uint32 alpha1)
{
    Block block{};
    for (uint8& byte : block)
        byte = 0xA5;
    SetBits(block, 0, 7, 0x40);
    for (uint32 field = 0; field < 6; ++field)
        SetBits(block, 7 + field * kMode6AlphaBits, kMode6AlphaBits, 0x7F);
    SetBits(block, kMode6FirstAlphaBit, kMode6AlphaBits, alpha0);
    SetBits(block, kMode6FirstAlphaBit + kMode6AlphaBits, kMode6AlphaBits, alpha1);
    SetBits(block, kMode6FirstPBit, 2, 0x3);
    return block;
}

// A mode 7 block: partition 5, white RGB fields, the given four alpha fields, all
// four p-bits set, and the same index filler.
Block MakeMode7Block(const std::array<uint32, 4>& alphas)
{
    Block block{};
    for (uint8& byte : block)
        byte = 0xA5;
    SetBits(block, 0, 8, 0x80);
    SetBits(block, 8, 6, 5);
    for (uint32 field = 0; field < 12; ++field)
        SetBits(block, 14 + field * kMode7AlphaBits, kMode7AlphaBits, 0x1F);
    for (uint32 endpoint = 0; endpoint < 4; ++endpoint)
        SetBits(block, kMode7FirstAlphaBit + endpoint * kMode7AlphaBits, kMode7AlphaBits, alphas[endpoint]);
    SetBits(block, kMode7FirstPBit, 4, 0xF);
    return block;
}

// White RGBA8 texels at `alpha`, one texel at (transparentX, transparentY) at alpha 0
// when it lies inside the image.
std::vector<uint8> MakeSource(uint32 width, uint32 height, uint8 alpha, uint32 transparentX, uint32 transparentY)
{
    std::vector<uint8> rgba(static_cast<size_t>(width) * height * 4, 255);
    for (size_t texel = 0; texel < static_cast<size_t>(width) * height; ++texel)
        rgba[texel * 4 + 3] = alpha;
    if (transparentX < width && transparentY < height)
        rgba[(static_cast<size_t>(transparentY) * width + transparentX) * 4 + 3] = 0;
    return rgba;
}

Block WithBitCleared(Block block, uint32 bit)
{
    SetBits(block, bit, 1, 0);
    return block;
}

} // namespace

TEST(BC7TransparentEndpointRepair, Mode6TransparentFirstEndpointLosesItsPBit)
{
    const std::vector<uint8> source = MakeSource(4, 4, 255, 0, 0);
    const Block encoded = MakeMode6Block(0, 0x7F);
    Block repaired = encoded;
    RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
    EXPECT_EQ(repaired, WithBitCleared(encoded, kMode6FirstPBit))
        << "endpoint 0 stores alpha 0 with p = 1 (alpha 1/255): only its p-bit may change";
}

TEST(BC7TransparentEndpointRepair, Mode6TransparentSecondEndpointLosesItsPBit)
{
    const std::vector<uint8> source = MakeSource(4, 4, 255, 3, 3);
    const Block encoded = MakeMode6Block(0x7F, 0);
    Block repaired = encoded;
    RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
    EXPECT_EQ(repaired, WithBitCleared(encoded, kMode6FirstPBit + 1));
}

TEST(BC7TransparentEndpointRepair, Mode7ClearsEveryTransparentEndpointAndNoOther)
{
    const std::vector<uint8> source = MakeSource(4, 4, 255, 1, 2);
    const Block encoded = MakeMode7Block({0, 0x1F, 0, 0x10});
    Block repaired = encoded;
    RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
    EXPECT_EQ(repaired, WithBitCleared(WithBitCleared(encoded, kMode7FirstPBit), kMode7FirstPBit + 2))
        << "endpoints 0 and 2 store alpha 0 with p = 1 (alpha 4/255); endpoints 1 and 3 must keep theirs";
}

TEST(BC7TransparentEndpointRepair, BlockWithoutTransparentTexelIsUnchanged)
{
    // Alpha 1 everywhere: an endpoint at alpha 1 is what the source asks for.
    const std::vector<uint8> source = MakeSource(4, 4, 1, 4, 4);
    const Block encoded = MakeMode6Block(0, 0);
    Block repaired = encoded;
    RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
    EXPECT_EQ(repaired, encoded);
}

TEST(BC7TransparentEndpointRepair, EndpointWithNonZeroAlphaBitsIsUnchanged)
{
    const std::vector<uint8> source = MakeSource(4, 4, 255, 0, 0);
    const Block encoded = MakeMode6Block(1, 0x7F);
    Block repaired = encoded;
    RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
    EXPECT_EQ(repaired, encoded) << "alpha bits 1 with p = 1 decode to 3/255, which the p-bit alone cannot zero";
}

TEST(BC7TransparentEndpointRepair, ModesWithoutAlphaPBitsAreUnchanged)
{
    const std::vector<uint8> source = MakeSource(4, 4, 0, 0, 0);
    // Mode 5 (separate alpha, no p-bits) and mode 1 (no alpha); every other bit is 0.
    for (const uint8 modeBits : {uint8{0x20}, uint8{0x02}})
    {
        Block encoded{};
        encoded[0] = modeBits;
        Block repaired = encoded;
        RepairBC7TransparentEndpoints(source.data(), 4, 4, repaired);
        EXPECT_EQ(repaired, encoded) << "mode bits 0x" << std::hex << static_cast<int>(modeBits);
    }
}

TEST(BC7TransparentEndpointRepair, OnlyTheBlockHoldingTheTransparentTexelChanges)
{
    // 6 x 2 texels: two blocks in one row, the right one partial. The transparent
    // texel is at x = 5, inside the right block.
    const std::vector<uint8> source = MakeSource(6, 2, 255, 5, 1);
    const Block encoded = MakeMode6Block(0, 0x7F);
    std::array<uint8, 32> payload{};
    std::copy(encoded.begin(), encoded.end(), payload.begin());
    std::copy(encoded.begin(), encoded.end(), payload.begin() + 16);

    RepairBC7TransparentEndpoints(source.data(), 6, 2, payload);

    Block left{};
    Block right{};
    std::copy(payload.begin(), payload.begin() + 16, left.begin());
    std::copy(payload.begin() + 16, payload.end(), right.begin());
    EXPECT_EQ(left, encoded);
    EXPECT_EQ(right, WithBitCleared(encoded, kMode6FirstPBit));
}

// The right block of a 6 x 2 image holds texels 4 and 5 only. Texels 6 and 7 of a row
// are the next row's texels 0 and 1 in memory; the transparent texel at (0, 1) must not
// reach the right block through them.
TEST(BC7TransparentEndpointRepair, PaddingOfAPartialEdgeBlockIsNotSampled)
{
    const std::vector<uint8> source = MakeSource(6, 2, 255, 0, 1);
    const Block encoded = MakeMode6Block(0, 0x7F);
    std::array<uint8, 32> payload{};
    std::copy(encoded.begin(), encoded.end(), payload.begin());
    std::copy(encoded.begin(), encoded.end(), payload.begin() + 16);
    RepairBC7TransparentEndpoints(source.data(), 6, 2, payload);
    Block right{};
    std::copy(payload.begin() + 16, payload.end(), right.begin());
    EXPECT_EQ(right, encoded);
}
