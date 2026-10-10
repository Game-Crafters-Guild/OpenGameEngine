// KTX2 container loading: transcode-target selection (BC7 vs RGBA32), mip
// chain adoption, and DFD-derived color space. Headless — no GPU device.

#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "Rendering/Core/Device.h"

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#endif

#include <cstring>
#include <vector>

using namespace GameEngine;

namespace
{

#if defined(GE_HAVE_KTX)

// VkFormat values for the KTX2 createInfo (ktx.h takes a raw uint32; the test
// deliberately avoids a Vulkan header dependency).
constexpr ktx_uint32_t kVkFormatR8G8B8A8Unorm = 37;
constexpr ktx_uint32_t kVkFormatR8G8B8A8Srgb = 43;

// Encode a 16x16 RGBA8 gradient with a full 5-level mip chain to UASTC KTX2,
// returned as file bytes. `zstdSupercompress` mirrors the TextureCompiler
// output (UASTC + Zstd), exercising the inflate path at load time.
std::vector<uint8> EncodeTestKtx2(bool srgb, bool zstdSupercompress = false)
{
    constexpr uint32_t kBaseSize = 16;
    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = srgb ? kVkFormatR8G8B8A8Srgb : kVkFormatR8G8B8A8Unorm;
    createInfo.baseWidth = kBaseSize;
    createInfo.baseHeight = kBaseSize;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 5; // 16, 8, 4, 2, 1
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    EXPECT_EQ(ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture), KTX_SUCCESS);
    if (!texture)
        return {};

    for (uint32_t level = 0; level < createInfo.numLevels; ++level)
    {
        const uint32_t dim = kBaseSize >> level;
        std::vector<uint8> pixels(static_cast<size_t>(dim) * dim * 4);
        for (uint32_t y = 0; y < dim; ++y)
        {
            for (uint32_t x = 0; x < dim; ++x)
            {
                uint8* px = pixels.data() + (static_cast<size_t>(y) * dim + x) * 4;
                px[0] = static_cast<uint8>((x * 255) / (dim > 1 ? dim - 1 : 1));
                px[1] = static_cast<uint8>((y * 255) / (dim > 1 ? dim - 1 : 1));
                px[2] = static_cast<uint8>(level * 40);
                px[3] = 255;
            }
        }
        EXPECT_EQ(ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0,
                                                pixels.data(), pixels.size()),
                  KTX_SUCCESS);
    }

    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    params.threadCount = 1;
    params.uastcFlags = KTX_PACK_UASTC_LEVEL_FASTEST;
    // Every later libktx call reads state the encode produces; supercompressing
    // an un-encoded texture dereferences null inside libktx, and writing one
    // returns KTX_INVALID_OPERATION. Bail so an encoder failure surfaces as an
    // empty fixture, not a crash.
    KTX_error_code err = ktxTexture2_CompressBasisEx(texture, &params);
    if (err != KTX_SUCCESS)
    {
        ADD_FAILURE() << "ktxTexture2_CompressBasisEx: " << ktxErrorString(err);
        ktxTexture_Destroy(ktxTexture(texture));
        return {};
    }
    if (zstdSupercompress)
    {
        err = ktxTexture2_DeflateZstd(texture, 10);
        if (err != KTX_SUCCESS)
        {
            ADD_FAILURE() << "ktxTexture2_DeflateZstd: " << ktxErrorString(err);
            ktxTexture_Destroy(ktxTexture(texture));
            return {};
        }
    }

    ktx_uint8_t* fileBytes = nullptr;
    ktx_size_t fileSize = 0;
    EXPECT_EQ(ktxTexture_WriteToMemory(ktxTexture(texture), &fileBytes, &fileSize), KTX_SUCCESS);

    std::vector<uint8> out;
    if (fileBytes && fileSize)
        out.assign(fileBytes, fileBytes + fileSize);
    if (fileBytes)
        free(fileBytes);
    ktxTexture_Destroy(ktxTexture(texture));
    return out;
}

// Restores the process-wide transcode target after each test.
class TextureAssetKtx2Test : public ::testing::Test
{
protected:
    void SetUp() override { m_Previous = TextureAsset::GetGpuTranscodeTarget(); }
    void TearDown() override { TextureAsset::SetGpuTranscodeTarget(m_Previous); }

private:
    TextureGpuTranscodeTarget m_Previous{};
};

TEST_F(TextureAssetKtx2Test, TranscodesToBc7WithFullMipChain)
{
    const std::vector<uint8> file = EncodeTestKtx2(/*srgb=*/true);
    ASSERT_FALSE(file.empty());

    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    TextureAsset asset(GUID::Null(), "TextureAssetKtx2Tests.ktx2");
    ASSERT_TRUE(asset.LoadFromData(file));

    EXPECT_EQ(asset.GetFormat(), TextureFormat::BC7);
    EXPECT_TRUE(asset.IsBlockCompressed());
    EXPECT_EQ(asset.GetWidth(), 16u);
    EXPECT_EQ(asset.GetHeight(), 16u);
    EXPECT_TRUE(asset.HasMipmaps());
    EXPECT_EQ(asset.GetMipmapLevels(), 5u);
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::SRGB);

    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 5u);
    // BC7: 16 bytes per 4x4 block; every level occupies at least one block.
    const uint64 kExpectedSizes[] = {16 * 16, 4 * 16, 1 * 16, 1 * 16, 1 * 16};
    uint64 expectedOffset = 0;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        EXPECT_EQ(chain[i].Width, 16u >> i) << "level " << i;
        EXPECT_EQ(chain[i].Height, 16u >> i) << "level " << i;
        EXPECT_EQ(chain[i].Size, kExpectedSizes[i]) << "level " << i;
        EXPECT_EQ(chain[i].Offset, expectedOffset) << "level " << i;
        expectedOffset += chain[i].Size;
    }
    EXPECT_EQ(asset.GetDataSize(), expectedOffset);
    EXPECT_EQ(chain[0].Offset, 0u); // GetPixelData() must address mip 0 at byte 0
}

TEST_F(TextureAssetKtx2Test, TranscodesToRgba32WhenTargetIsRgba32)
{
    const std::vector<uint8> file = EncodeTestKtx2(/*srgb=*/true);
    ASSERT_FALSE(file.empty());

    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::RGBA32);
    TextureAsset asset(GUID::Null(), "TextureAssetKtx2Tests.ktx2");
    ASSERT_TRUE(asset.LoadFromData(file));

    EXPECT_EQ(asset.GetFormat(), TextureFormat::RGBA8);
    EXPECT_FALSE(asset.IsBlockCompressed());
    EXPECT_EQ(asset.GetChannels(), 4u);
    EXPECT_EQ(asset.GetMipmapLevels(), 5u);
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::SRGB);

    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 5u);
    uint64 total = 0;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const uint64 dim = 16u >> i;
        EXPECT_EQ(chain[i].Size, dim * dim * 4) << "level " << i;
        total += chain[i].Size;
    }
    EXPECT_EQ(asset.GetDataSize(), total);
}

TEST_F(TextureAssetKtx2Test, ZstdSupercompressedContainerTranscodes)
{
    // Matches the TextureCompiler output shape: UASTC + Zstd supercompression.
    const std::vector<uint8> file = EncodeTestKtx2(/*srgb=*/true, /*zstdSupercompress=*/true);
    ASSERT_FALSE(file.empty());

    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    TextureAsset asset(GUID::Null(), "TextureAssetKtx2Tests.ktx2");
    ASSERT_TRUE(asset.LoadFromData(file));

    EXPECT_EQ(asset.GetFormat(), TextureFormat::BC7);
    EXPECT_EQ(asset.GetMipmapLevels(), 5u);
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::SRGB);
    // Same BC7 layout as the non-supercompressed container: inflate must be
    // transparent to the decoded payload.
    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 5u);
    EXPECT_EQ(asset.GetDataSize(), (16u * 16u + 4u * 16u + 3u * 16u));
}

TEST_F(TextureAssetKtx2Test, LinearDfdYieldsLinearColorSpace)
{
    const std::vector<uint8> file = EncodeTestKtx2(/*srgb=*/false);
    ASSERT_FALSE(file.empty());

    TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget::BC7);
    TextureAsset asset(GUID::Null(), "TextureAssetKtx2Tests.ktx2");
    ASSERT_TRUE(asset.LoadFromData(file));
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::Linear);
}

#endif // GE_HAVE_KTX

TEST(BlockCompressedFormatRules, BlockHelpersAgree)
{
    using Rendering::TextureFormat;
    EXPECT_TRUE(Rendering::IsBlockCompressedFormat(TextureFormat::BC7_UNORM));
    EXPECT_TRUE(Rendering::IsBlockCompressedFormat(TextureFormat::BC7_SRGB));
    EXPECT_TRUE(Rendering::IsBlockCompressedFormat(TextureFormat::BC5_UNORM));
    EXPECT_TRUE(Rendering::IsBlockCompressedFormat(TextureFormat::BC1_UNORM));
    EXPECT_FALSE(Rendering::IsBlockCompressedFormat(TextureFormat::RGBA8_SRGB));

    EXPECT_EQ(Rendering::BytesPerBlock(TextureFormat::BC1_UNORM), 8u);
    EXPECT_EQ(Rendering::BytesPerBlock(TextureFormat::BC5_UNORM), 16u);
    EXPECT_EQ(Rendering::BytesPerBlock(TextureFormat::BC7_UNORM), 16u);
    EXPECT_EQ(Rendering::BytesPerBlock(TextureFormat::BC7_SRGB), 16u);
    EXPECT_EQ(Rendering::BytesPerBlock(TextureFormat::RGBA8_UNORM), 0u);

    // Block-compressed formats have no per-pixel byte size; callers must use
    // block rules (this is what routes the copy path to tight packing).
    EXPECT_EQ(Rendering::BytesPerPixel(TextureFormat::BC7_SRGB), 0u);
    EXPECT_EQ(Rendering::BytesPerPixel(TextureFormat::BC7_UNORM), 0u);
}

} // namespace
