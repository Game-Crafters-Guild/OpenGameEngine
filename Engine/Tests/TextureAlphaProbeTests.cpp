// Tests for TextureAlphaProbe: header-metadata alpha detection for PNG, KTX2,
// JPEG, and TGA. The safety property: only a provable absence of alpha may
// report NoAlphaSource — everything undecidable reports MaybeAlpha.

#include <gtest/gtest.h>

#include "Assets/TextureAlphaProbe.h"

#include <cstring>
#include <vector>

using namespace GameEngine;

namespace {

void PushU32BE(std::vector<uint8>& v, uint32 x)
{
    v.push_back(static_cast<uint8>(x >> 24));
    v.push_back(static_cast<uint8>(x >> 16));
    v.push_back(static_cast<uint8>(x >> 8));
    v.push_back(static_cast<uint8>(x));
}

void PushU32LE(std::vector<uint8>& v, uint32 x)
{
    v.push_back(static_cast<uint8>(x));
    v.push_back(static_cast<uint8>(x >> 8));
    v.push_back(static_cast<uint8>(x >> 16));
    v.push_back(static_cast<uint8>(x >> 24));
}

void PushBytes(std::vector<uint8>& v, const char* s, size_t n)
{
    v.insert(v.end(), reinterpret_cast<const uint8*>(s), reinterpret_cast<const uint8*>(s) + n);
}

std::vector<uint8> MakePng(uint8 colorType, bool withTrns, bool truncateBeforeIdat = false)
{
    std::vector<uint8> v = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    // IHDR
    PushU32BE(v, 13);
    PushBytes(v, "IHDR", 4);
    PushU32BE(v, 1); // width
    PushU32BE(v, 1); // height
    v.push_back(8);  // bit depth
    v.push_back(colorType);
    v.push_back(0); // compression
    v.push_back(0); // filter
    v.push_back(0); // interlace
    PushU32BE(v, 0); // crc (not validated by the probe)
    if (withTrns)
    {
        PushU32BE(v, 1);
        PushBytes(v, "tRNS", 4);
        v.push_back(0);
        PushU32BE(v, 0);
    }
    if (!truncateBeforeIdat)
    {
        PushU32BE(v, 0);
        PushBytes(v, "IDAT", 4);
        PushU32BE(v, 0);
    }
    return v;
}

std::vector<uint8> MakeKtx2(uint32 vkFormat, uint8 colorModel,
                            const std::vector<uint8>& sampleChannelTypes)
{
    std::vector<uint8> v = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    PushU32LE(v, vkFormat); // vkFormat
    PushU32LE(v, 1);        // typeSize
    PushU32LE(v, 4);        // pixelWidth
    PushU32LE(v, 4);        // pixelHeight
    PushU32LE(v, 0);        // pixelDepth
    PushU32LE(v, 0);        // layerCount
    PushU32LE(v, 1);        // faceCount
    PushU32LE(v, 1);        // levelCount
    PushU32LE(v, 0);        // supercompressionScheme
    const uint32 blockSize = 24 + 16 * static_cast<uint32>(sampleChannelTypes.size());
    const uint32 dfdLen = 4 + blockSize;
    const uint32 dfdOff = 80;
    PushU32LE(v, dfdOff);  // dfdByteOffset
    PushU32LE(v, dfdLen);  // dfdByteLength
    PushU32LE(v, 0);       // kvdByteOffset
    PushU32LE(v, 0);       // kvdByteLength
    for (int i = 0; i < 16; ++i)
        v.push_back(0); // sgd offset/length (u64 x2)
    EXPECT_EQ(v.size(), 80u);
    // DFD
    PushU32LE(v, dfdLen);                 // dfdTotalSize
    PushU32LE(v, 0);                      // vendorId | descriptorType
    PushU32LE(v, (blockSize << 16) | 2);  // versionNumber | descriptorBlockSize
    v.push_back(colorModel);
    v.push_back(1); // colorPrimaries
    v.push_back(1); // transferFunction
    v.push_back(0); // flags
    for (int i = 0; i < 4; ++i)
        v.push_back(0); // texelBlockDimension
    for (int i = 0; i < 8; ++i)
        v.push_back(0); // bytesPlane0-7
    for (const uint8 channelType : sampleChannelTypes)
    {
        v.push_back(0); // bitOffset lo
        v.push_back(0); // bitOffset hi
        v.push_back(63); // bitLength
        v.push_back(channelType);
        for (int i = 0; i < 12; ++i)
            v.push_back(0); // samplePosition / lower / upper
    }
    return v;
}

TextureAlphaContent Probe(const std::vector<uint8>& v, std::string_view hint = {})
{
    return ProbeTextureAlphaFromBytes(v.data(), v.size(), hint);
}

constexpr uint8 kEtc1s = 163;
constexpr uint8 kUastc = 166;

} // namespace

// --- PNG ---

TEST(TextureAlphaProbeTest, Png_TruecolorWithoutTrns_NoAlpha)
{
    EXPECT_EQ(Probe(MakePng(2, false)), TextureAlphaContent::NoAlphaSource);
}

TEST(TextureAlphaProbeTest, Png_Rgba_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakePng(6, false)), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Png_GrayAlpha_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakePng(4, false)), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Png_TruecolorWithTrns_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakePng(2, true)), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Png_PaletteWithoutTrns_NoAlpha)
{
    EXPECT_EQ(Probe(MakePng(3, false)), TextureAlphaContent::NoAlphaSource);
}

TEST(TextureAlphaProbeTest, Png_PaletteWithTrns_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakePng(3, true)), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Png_TruncatedBeforeIdat_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakePng(2, false, /*truncateBeforeIdat=*/true)),
              TextureAlphaContent::MaybeAlpha);
}

// --- KTX2 ---

TEST(TextureAlphaProbeTest, Ktx2_Etc1sSingleSlice_NoAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kEtc1s, {0})), TextureAlphaContent::NoAlphaSource);
}

TEST(TextureAlphaProbeTest, Ktx2_Etc1sAlphaSlice_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kEtc1s, {0, 15})), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Ktx2_UastcRgb_NoAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {0})), TextureAlphaContent::NoAlphaSource);
}

TEST(TextureAlphaProbeTest, Ktx2_UastcRgba_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {3})), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Ktx2_UastcAlphalessChannelTypes_NoAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {4})), TextureAlphaContent::NoAlphaSource); // RRR
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {6})), TextureAlphaContent::NoAlphaSource); // RG
}

TEST(TextureAlphaProbeTest, Ktx2_UastcRrrg_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {5})), TextureAlphaContent::MaybeAlpha); // RRRG: G is alpha
}

TEST(TextureAlphaProbeTest, Ktx2_UastcUnknownChannelType_MaybeAlpha)
{
    // Unknown/future channel types must classify as undecidable, not as
    // alpha-less — only the whitelisted {0, 4, 6} are provably without alpha.
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {7})), TextureAlphaContent::MaybeAlpha);
    EXPECT_EQ(Probe(MakeKtx2(0, kUastc, {15})), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Ktx2_UnknownColorModel_MaybeAlpha)
{
    EXPECT_EQ(Probe(MakeKtx2(0, 42, {0})), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Ktx2_HugeDfdOffset_MaybeAlpha)
{
    // dfdByteOffset >= 0xFFFFFFD4 wrapped the old uint32 bounds check
    // (dfdOff + 44 overflows to a small value) while the 64-bit pointer math
    // then read far out of bounds. Must classify as undecidable, untouched.
    std::vector<uint8> v = MakeKtx2(0, kEtc1s, {0});
    v[48] = 0xF0; // dfdByteOffset = 0xFFFFFFF0 (little-endian)
    v[49] = 0xFF;
    v[50] = 0xFF;
    v[51] = 0xFF;
    EXPECT_EQ(Probe(v), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Ktx2_VkFormats)
{
    EXPECT_EQ(Probe(MakeKtx2(23, 0, {0})), TextureAlphaContent::NoAlphaSource);  // R8G8B8_UNORM
    EXPECT_EQ(Probe(MakeKtx2(37, 0, {0})), TextureAlphaContent::MaybeAlpha);     // R8G8B8A8_UNORM
    EXPECT_EQ(Probe(MakeKtx2(131, 0, {0})), TextureAlphaContent::NoAlphaSource); // BC1_RGB
    EXPECT_EQ(Probe(MakeKtx2(133, 0, {0})), TextureAlphaContent::MaybeAlpha);    // BC1_RGBA (punch-through)
    EXPECT_EQ(Probe(MakeKtx2(145, 0, {0})), TextureAlphaContent::MaybeAlpha);    // BC7
}

TEST(TextureAlphaProbeTest, Ktx2_TruncatedDfd_MaybeAlpha)
{
    std::vector<uint8> v = MakeKtx2(0, kEtc1s, {0});
    v.resize(84); // header + dfdTotalSize only
    EXPECT_EQ(Probe(v), TextureAlphaContent::MaybeAlpha);
}

// --- JPEG / TGA / unknown ---

TEST(TextureAlphaProbeTest, Jpeg_NoAlpha)
{
    const std::vector<uint8> v = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
    EXPECT_EQ(Probe(v), TextureAlphaContent::NoAlphaSource);
}

TEST(TextureAlphaProbeTest, Tga_24bpp_NoAlphaWithHint)
{
    std::vector<uint8> v(18, 0);
    v[2] = 2;   // truecolor
    v[16] = 24; // bpp
    EXPECT_EQ(Probe(v, ".tga"), TextureAlphaContent::NoAlphaSource);
    EXPECT_EQ(Probe(v, ".TGA"), TextureAlphaContent::NoAlphaSource);
    // Without the extension hint TGA is unrecognizable.
    EXPECT_EQ(Probe(v), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, Tga_32bpp_MaybeAlpha)
{
    std::vector<uint8> v(18, 0);
    v[2] = 2;
    v[16] = 32;
    v[17] = 8; // 8 attribute bits
    EXPECT_EQ(Probe(v, ".tga"), TextureAlphaContent::MaybeAlpha);
}

TEST(TextureAlphaProbeTest, GarbageAndEmpty_MaybeAlpha)
{
    const std::vector<uint8> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(Probe(garbage), TextureAlphaContent::MaybeAlpha);
    EXPECT_EQ(ProbeTextureAlphaFromBytes(nullptr, 0), TextureAlphaContent::MaybeAlpha);
}
