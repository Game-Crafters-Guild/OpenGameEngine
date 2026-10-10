#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/TextureCook.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using namespace GameEngine;

namespace
{
TextureCookMipLevel CoveragePattern(uint32 width, uint32 height)
{
    TextureCookMipLevel image;
    image.Width = width;
    image.Height = height;
    image.Bytes.resize(static_cast<size_t>(width) * height * 4);
    constexpr uint8 alpha[] = {0, 0, 17, 63, 127, 128, 190, 255, 255, 255, 1};
    for (size_t i = 0; i < image.Bytes.size() / 4; ++i)
    {
        image.Bytes[i * 4] = static_cast<uint8>(i * 37);
        image.Bytes[i * 4 + 1] = static_cast<uint8>(i * 19 + 41);
        image.Bytes[i * 4 + 2] = static_cast<uint8>(i * 73 + 13);
        image.Bytes[i * 4 + 3] = alpha[(i * 7 + i / width) % std::size(alpha)];
    }
    return image;
}

std::vector<uint8> CoverageTga(const TextureCookMipLevel& image)
{
    std::vector<uint8> bytes(18, 0);
    bytes[2] = 2;
    bytes[12] = static_cast<uint8>(image.Width);
    bytes[13] = static_cast<uint8>(image.Width >> 8);
    bytes[14] = static_cast<uint8>(image.Height);
    bytes[15] = static_cast<uint8>(image.Height >> 8);
    bytes[16] = 32;
    bytes[17] = 0x28;
    for (size_t i = 0; i < image.Bytes.size(); i += 4)
        bytes.insert(bytes.end(), {image.Bytes[i + 2], image.Bytes[i + 1], image.Bytes[i], image.Bytes[i + 3]});
    return bytes;
}

uint64 PassingTexels(const TextureCookMipLevel& image, float cutoff)
{
    uint64 count = 0;
    for (size_t i = 3; i < image.Bytes.size(); i += 4)
        count += static_cast<float>(image.Bytes[i]) / 255.0f >= cutoff;
    return count;
}

uint64 CoverageError(uint64 passing, uint64 pixels, uint64 reference, uint64 referencePixels)
{
    const uint64 have = passing * referencePixels, want = reference * pixels;
    return have > want ? have - want : want - have;
}

// Independent exhaustive oracle: a uniform nonnegative alpha scale can only
// include a suffix of the original nonzero byte values. Recount every possible
// threshold directly from pixels; do not use the production histogram/search.
uint64 MinimumAttainableError(const TextureCookMipLevel& ordinary, float cutoff,
                              uint64 reference, uint64 referencePixels)
{
    const uint64 pixels = ordinary.Bytes.size() / 4;
    if (cutoff == 0.0f)
        return CoverageError(pixels, pixels, reference, referencePixels);
    uint64 best = std::numeric_limits<uint64>::max();
    for (uint32 threshold = 1; threshold <= 256; ++threshold)
    {
        uint64 count = 0;
        for (size_t i = 3; i < ordinary.Bytes.size(); i += 4)
            count += ordinary.Bytes[i] >= threshold;
        best = std::min(best, CoverageError(count, pixels, reference, referencePixels));
    }
    return best;
}
} // namespace

TEST(TextureAlphaCoverage, MetadataIsStrictTransactionalAndDisabledCutoffIsInert)
{
    TextureCookSettings settings;
    settings.PreserveAlphaCoverage = true;
    settings.AlphaCoverageCutoff = 0.7f;
    std::string error;
    for (const std::string enable : {"true", "yes", " 1", "2"})
    {
        EXPECT_FALSE(ParseTextureAlphaCoverageMeta(enable, ".5", settings, error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(settings.PreserveAlphaCoverage);
        EXPECT_FLOAT_EQ(settings.AlphaCoverageCutoff, 0.7f);
    }
    for (const std::string value : {"nan", "inf", "-0.1", "1.1", "0.5junk", "0,5", " 0.5", "+0.5", "0x1p-1", "0.5 ", "1e-999"})
    {
        EXPECT_FALSE(ParseTextureAlphaCoverageMeta("1", value, settings, error));
        EXPECT_FLOAT_EQ(settings.AlphaCoverageCutoff, 0.7f);
    }
    for (const float cutoff : {0.0f, -0.0f, 0.5f, 0.538f, std::nextafter(0.5f, 1.0f), 1.0f, std::numeric_limits<float>::denorm_min()})
    {
        ASSERT_TRUE(ParseTextureAlphaCoverageMeta("1", TextureAlphaCutoffMetaValue(cutoff), settings, error));
        EXPECT_EQ(settings.AlphaCoverageCutoff, cutoff);
        if (cutoff == 0.0f)
            EXPECT_FALSE(std::signbit(settings.AlphaCoverageCutoff));
    }
    ASSERT_TRUE(ParseTextureAlphaCoverageMeta("0", "nan", settings, error));
    EXPECT_FALSE(settings.PreserveAlphaCoverage);
    ASSERT_TRUE(ParseTextureAlphaCoverageMeta("1", "", settings, error));
    EXPECT_EQ(settings.AlphaCoverageCutoff, 0.5f);
}

TEST(TextureAlphaCoverage, RequestedAlphaLossAndInvalidActiveInputsAreRejected)
{
    TextureCookSettings settings;
    settings.PreserveAlphaCoverage = true;
    std::string error;
    for (const auto format : {TextureCookCompression::BC1, TextureCookCompression::BC4,
                              TextureCookCompression::BC5, TextureCookCompression::BC6H})
    {
        settings.Compression = format;
        EXPECT_FALSE(ValidateTextureAlphaCoverageSettings(settings, false, error));
        // A no-BC device's RGBA fallback must not erase requested-format admission.
        EXPECT_EQ(ResolveTextureCookOutput(settings, false, false, true), TextureCookOutput::Uncompressed);
    }
    settings.Compression = TextureCookCompression::Auto;
    settings.Usage = TextureCookUsage::Mask;
    EXPECT_FALSE(ValidateTextureAlphaCoverageSettings(settings, false, error));
    settings.Compression = TextureCookCompression::None;
    settings.Usage = TextureCookUsage::Normal;
    EXPECT_FALSE(ValidateTextureAlphaCoverageSettings(settings, false, error));
    settings.Usage = TextureCookUsage::Color;
    EXPECT_FALSE(ValidateTextureAlphaCoverageSettings(settings, true, error));
    for (float bad : {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        settings.AlphaCoverageCutoff = bad;
        EXPECT_FALSE(ValidateTextureAlphaCoverageSettings(settings, false, error));
    }
    settings.PreserveAlphaCoverage = false;
    EXPECT_TRUE(ValidateTextureAlphaCoverageSettings(settings, true, error));
}

#if defined(GE_HAVE_KTX)
// Follow the existing cook suite's feature boundary for source-image/roundtrip
// tests; metadata admission above remains available in minimal builds.
TEST(TextureAlphaCoverage, EveryMipReachesNearestAttainableCoverageWithoutChangingRgbOrBase)
{
    for (const auto extent : {std::array<uint32, 2>{33, 17}, {32, 16}, {17, 1}, {1, 17}, {1, 1}})
        for (const bool srgb : {false, true})
            for (const float cutoff : {0.0f, 127.0f / 255.0f, 0.5f, 128.0f / 255.0f, 0.538f, 1.0f})
            {
                SCOPED_TRACE(extent[0]);
                SCOPED_TRACE(extent[1]);
                SCOPED_TRACE(cutoff);
                const auto base = CoveragePattern(extent[0], extent[1]);
                TextureCookSettings settings;
                const auto count = ComputeTextureCookMipCount(extent[0], extent[1], settings);
                std::vector<TextureCookMipLevel> ordinary{base}, corrected{base};
                ASSERT_TRUE(BuildTextureCookMipChain(ordinary, count, false, srgb, settings));
                settings.PreserveAlphaCoverage = true;
                settings.AlphaCoverageCutoff = cutoff;
                ASSERT_TRUE(BuildTextureCookMipChain(corrected, count, false, srgb, settings));
                ASSERT_EQ(corrected.size(), ordinary.size());
                EXPECT_EQ(corrected[0].Bytes, base.Bytes);
                const uint64 reference = PassingTexels(base, cutoff), referencePixels = base.Bytes.size() / 4;
                for (size_t level = 0; level < count; ++level)
                {
                    SCOPED_TRACE(level);
                    const auto& before = ordinary[level];
                    const auto& after = corrected[level];
                    EXPECT_EQ(after.Width, before.Width);
                    EXPECT_EQ(after.Height, before.Height);
                    for (size_t i = 0; i < after.Bytes.size(); ++i)
                        if (i % 4 != 3)
                            EXPECT_EQ(after.Bytes[i], before.Bytes[i]);
                    for (size_t i = 3; i < after.Bytes.size(); i += 4)
                        if (before.Bytes[i] == 0)
                            EXPECT_EQ(after.Bytes[i], 0);
                    if (level)
                        EXPECT_EQ(CoverageError(PassingTexels(after, cutoff), after.Bytes.size() / 4, reference, referencePixels),
                                  MinimumAttainableError(before, cutoff, reference, referencePixels));
                }
            }
}

TEST(TextureAlphaCoverage, UniformAndTiedTailCoverageKeepsIdentityWhenEquallyGood)
{
    for (uint8 alpha : {uint8(0), uint8(127), uint8(128), uint8(255)})
        for (float cutoff : {0.0f, 0.5f, 1.0f})
        {
            auto base = CoveragePattern(8, 4);
            for (size_t i = 3; i < base.Bytes.size(); i += 4)
                base.Bytes[i] = alpha;
            TextureCookSettings settings;
            std::vector<TextureCookMipLevel> ordinary{base}, corrected{base};
            ASSERT_TRUE(BuildTextureCookMipChain(ordinary, 4, false, true, settings));
            settings.PreserveAlphaCoverage = true;
            settings.AlphaCoverageCutoff = cutoff;
            ASSERT_TRUE(BuildTextureCookMipChain(corrected, 4, false, true, settings));
            for (size_t i = 0; i < ordinary.size(); ++i)
                EXPECT_EQ(corrected[i].Bytes, ordinary[i].Bytes);
        }
    auto base = CoveragePattern(2, 1);
    base.Bytes[3] = 0;
    base.Bytes[7] = 255;
    TextureCookSettings settings;
    std::vector<TextureCookMipLevel> ordinary{base}, corrected{base};
    ASSERT_TRUE(BuildTextureCookMipChain(ordinary, 2, false, false, settings));
    settings.PreserveAlphaCoverage = true;
    ASSERT_TRUE(BuildTextureCookMipChain(corrected, 2, false, false, settings));
    EXPECT_EQ(corrected[1].Bytes, ordinary[1].Bytes); // 0%/100% equally far from 50%
}

TEST(TextureAlphaCoverage, MalformedActiveChainIsRefusedBeforeMutation)
{
    const auto good = CoveragePattern(3, 5);
    TextureCookSettings settings;
    settings.PreserveAlphaCoverage = true;
    for (int bad = 0; bad < 6; ++bad)
    {
        std::vector<TextureCookMipLevel> chain{good};
        uint32 count = 3;
        if (bad == 0)
            chain.clear();
        if (bad == 1)
            chain[0].Bytes.pop_back();
        if (bad == 2)
            chain[0].Width = 0;
        if (bad == 3)
            chain.push_back(good);
        if (bad == 4)
            count = 0;
        if (bad == 5)
            count = 5;
        auto before = chain;
        std::string error;
        EXPECT_FALSE(BuildTextureCookMipChain(chain, count, false, true, settings, &error));
        EXPECT_FALSE(error.empty());
        ASSERT_EQ(chain.size(), before.size());
        for (size_t i = 0; i < chain.size(); ++i)
            EXPECT_EQ(chain[i].Bytes, before[i].Bytes);
    }
}

TEST(TextureAlphaCoverage, DisabledBytesAndKeysAreIdenticalAndActiveCutoffRekeys)
{
    const auto base = CoveragePattern(19, 13);
    const auto source = CoverageTga(base);
    TextureCookInputs normal;
    normal.Settings.Compression = TextureCookCompression::None;
    normal.ColorSpace = TextureColorSpace::SRGB;
    auto disabled = normal;
    disabled.Settings.AlphaCoverageCutoff = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(ComputeTextureCookConfigHash(normal, TextureCookOutput::Uncompressed),
              ComputeTextureCookConfigHash(disabled, TextureCookOutput::Uncompressed));
    std::vector<uint8> legacy, inert, active, changed;
    std::string error;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", normal, TextureCookOutput::Uncompressed, legacy, error)) << error;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", disabled, TextureCookOutput::Uncompressed, inert, error)) << error;
    EXPECT_EQ(inert, legacy);
    auto enabled = normal;
    enabled.Settings.PreserveAlphaCoverage = true;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", enabled, TextureCookOutput::Uncompressed, active, error)) << error;
    EXPECT_NE(active, legacy);
    const auto enabledHash = ComputeTextureCookConfigHash(enabled, TextureCookOutput::Uncompressed);
    EXPECT_NE(enabledHash, ComputeTextureCookConfigHash(normal, TextureCookOutput::Uncompressed));
    enabled.Settings.AlphaCoverageCutoff = std::nextafter(0.5f, 1.0f);
    EXPECT_NE(enabledHash, ComputeTextureCookConfigHash(enabled, TextureCookOutput::Uncompressed));
    enabled.Settings.AlphaCoverageCutoff = 0.9f;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", enabled, TextureCookOutput::Uncompressed, changed, error)) << error;
    EXPECT_NE(changed, active);
    auto edited = source;
    edited.back() = 77;
    EXPECT_NE(ComputeTextureCookSourceHash(source.data(), source.size()), ComputeTextureCookSourceHash(edited.data(), edited.size()));
}

TEST(TextureAlphaCoverage, CookedKtxMatchesSharedBuilderAfterSwizzleAtEveryLevel)
{
    auto base = CoveragePattern(31, 17);
    const auto source = CoverageTga(base);
    TextureCookInputs inputs;
    inputs.Settings.PreserveAlphaCoverage = true;
    inputs.Settings.AlphaCoverageCutoff = 0.538f;
    inputs.Settings.Compression = TextureCookCompression::None;
    inputs.ColorSpace = TextureColorSpace::SRGB;
    std::memcpy(inputs.Swizzle, "gbar", 4);
    for (size_t i = 0; i < base.Bytes.size(); i += 4)
    {
        const std::array<uint8, 4> p = {base.Bytes[i], base.Bytes[i + 1], base.Bytes[i + 2], base.Bytes[i + 3]};
        base.Bytes[i] = p[1];
        base.Bytes[i + 1] = p[2];
        base.Bytes[i + 2] = p[3];
        base.Bytes[i + 3] = p[0];
    }
    std::vector<TextureCookMipLevel> expected{base};
    ASSERT_TRUE(BuildTextureCookMipChain(expected, ComputeTextureCookMipCount(31, 17, inputs.Settings), false, true, inputs.Settings));
    std::vector<uint8> ktx;
    std::string error;
    ASSERT_TRUE(CookTexture(source.data(), source.size(), ".tga", inputs, TextureCookOutput::Uncompressed, ktx, error)) << error;
    TextureAsset cooked(GUID::Generate(), "coverage.ktx2");
    ASSERT_TRUE(cooked.LoadFromData(Vector<uint8>(ktx.begin(), ktx.end())));
    const auto& actual = cooked.GetMipChain();
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t level = 0; level < actual.size(); ++level)
    {
        EXPECT_EQ(actual[level].Width, expected[level].Width);
        EXPECT_EQ(actual[level].Height, expected[level].Height);
        const auto* bytes = cooked.GetPixelData() + actual[level].Offset;
        EXPECT_EQ(std::vector<uint8>(bytes, bytes + actual[level].Size), expected[level].Bytes);
    }
}

namespace
{
class TextureCoverageCache : public testing::Test
{
  protected:
    std::filesystem::path m_Root, m_SourcePath;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<AssetManager> m_Assets;
    GUID m_Guid;

    void Write(const std::vector<uint8>& bytes)
    {
        std::ofstream stream(m_SourcePath, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(stream.good());
    }
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() / ("ge-coverage-" + GUID::Generate().ToString());
        std::filesystem::create_directories(m_Root / "Assets");
        m_SourcePath = m_Root / "Assets" / "leaf.tga";
        Write(CoverageTga(CoveragePattern(19, 13)));
        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        m_Assets = std::make_unique<AssetManager>();
        ASSERT_TRUE(m_Assets->Initialize(m_Pool.get()));
        AssetSourceDesc source;
        source.Alias = "project";
        source.Root = m_Root / "Assets";
        source.AuthoritativeDbFile = m_Root / "AssetDatabase.assetdb";
        source.CacheRoot = m_Root / "Derived" / "AssetDatabase";
        source.RequiresScan = true;
        ASSERT_TRUE(m_Assets->RegisterSource(source));
        m_Assets->WaitForStartupScan(source.Alias);
        m_Guid = m_Assets->ResolveAssetGuid(m_SourcePath, source.Alias);
        ASSERT_FALSE(m_Guid.IsNull());
        const auto cacheRoot = m_Assets->GetRegistry().TryGetCacheRoot(m_Guid);
        ASSERT_TRUE(cacheRoot.has_value());
        ASSERT_EQ(cacheRoot->lexically_normal(), (m_Root / "Derived").lexically_normal());
        Set(kTextureCompressionMetaKey, "none");
        Set(kTextureColorSpaceMetaKey, "srgb");
    }
    void TearDown() override
    {
        m_Assets.reset();
        m_Pool.reset();
        std::error_code error;
        std::filesystem::remove_all(m_Root, error);
    }
    void Set(const char* key, const std::string& value)
    {
        ASSERT_TRUE(m_Assets->GetRegistry().SetMetaValue(m_SourcePath, key, value));
    }
    std::vector<std::filesystem::path> Artifacts()
    {
        std::vector<std::filesystem::path> result;
        if (std::filesystem::exists(m_Root / "Derived" / "Tex"))
            for (const auto& file : std::filesystem::directory_iterator(m_Root / "Derived" / "Tex"))
                if (file.path().extension() == ".ktx2")
                    result.push_back(file.path());
        return result;
    }
    std::vector<uint8> Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<uint8>(std::istreambuf_iterator<char>(stream), {});
    }
};
} // namespace

TEST_F(TextureCoverageCache, WorkerRecookRekeysCoverageCutoffAndSourceAndRetiresOldArtifact)
{
    AssetManager::ScopedThreadAssetManager context(m_Assets.get());
    auto cook = [&]()
    {
        TextureAsset texture(m_Guid, m_SourcePath);
        EXPECT_TRUE(texture.Load());
        EXPECT_EQ(texture.GetMipChain().size(), 5u);
    };
    cook();
    auto artifacts = Artifacts();
    ASSERT_EQ(artifacts.size(), 1u);
    const auto originalName = artifacts[0];
    const auto originalBytes = Read(originalName);
    Set(kTextureAlphaCoverageMetaKey, "0");
    Set(kTextureAlphaCutoffMetaKey, "nan");
    cook();
    EXPECT_EQ(Artifacts(), artifacts);
    EXPECT_EQ(Read(originalName), originalBytes);

    Set(kTextureAlphaCoverageMetaKey, "1");
    Set(kTextureAlphaCutoffMetaKey, ".538");
    cook();
    auto covered = Artifacts();
    ASSERT_EQ(covered.size(), 1u);
    EXPECT_NE(covered[0], originalName);
    EXPECT_FALSE(std::filesystem::exists(originalName));
    EXPECT_NE(Read(covered[0]), originalBytes);
    Set(kTextureAlphaCutoffMetaKey, ".9");
    cook();
    auto changedCutoff = Artifacts();
    ASSERT_EQ(changedCutoff.size(), 1u);
    EXPECT_NE(changedCutoff[0], covered[0]);
    EXPECT_FALSE(std::filesystem::exists(covered[0]));
    auto edited = CoveragePattern(19, 13);
    edited.Bytes[3] = 255;
    Write(CoverageTga(edited));
    cook();
    auto changedSource = Artifacts();
    ASSERT_EQ(changedSource.size(), 1u);
    EXPECT_NE(changedSource[0], changedCutoff[0]);
    EXPECT_FALSE(std::filesystem::exists(changedCutoff[0]));
}

TEST_F(TextureCoverageCache, InvalidEnabledMetadataRefusesWorkerLoadBeforeAnyArtifactOrRawSuccess)
{
    AssetManager::ScopedThreadAssetManager context(m_Assets.get());
    for (const auto& values : {std::array<const char*, 3>{"true", ".5", "none"},
                               {"1", "nan", "none"},
                               {"1", "1.1", "none"},
                               {"1", ".5", "bc1"},
                               {"1", ".5", "bc4"},
                               {"1", ".5", "bc5"},
                               {"1", ".5", "bc6h"}})
    {
        SCOPED_TRACE(values[0]);
        SCOPED_TRACE(values[1]);
        SCOPED_TRACE(values[2]);
        Set(kTextureAlphaCoverageMetaKey, values[0]);
        Set(kTextureAlphaCutoffMetaKey, values[1]);
        Set(kTextureCompressionMetaKey, values[2]);
        TextureAsset texture(m_Guid, m_SourcePath);
        EXPECT_FALSE(texture.Load());
        EXPECT_TRUE(Artifacts().empty());
    }
    Set(kTextureAlphaCoverageMetaKey, "0");
    Set(kTextureCompressionMetaKey, "none");
    TextureAsset disabled(m_Guid, m_SourcePath);
    EXPECT_TRUE(disabled.Load());
    EXPECT_EQ(Artifacts().size(), 1u);
}
TEST_F(TextureCoverageCache, EnabledCoverageRefusesPlaceholderPixelsAfterDecodeFailure)
{
    AssetManager::ScopedThreadAssetManager context(m_Assets.get());
    const std::string placeholder = "PNG_PLACEHOLDER_DATA";
    const Vector<uint8> bytes(placeholder.begin(), placeholder.end());
    Set(kTextureAlphaCoverageMetaKey, "1");
    Set(kTextureAlphaCutoffMetaKey, ".538");
    TextureAsset enabled(m_Guid, m_SourcePath);
    EXPECT_FALSE(enabled.LoadFromData(bytes));
    EXPECT_EQ(enabled.GetPixelData(), nullptr);
    EXPECT_TRUE(Artifacts().empty());

    // Preserve the existing disabled placeholder behavior for legacy callers.
    Set(kTextureAlphaCoverageMetaKey, "0");
    TextureAsset disabled(m_Guid, m_SourcePath);
    ASSERT_TRUE(disabled.LoadFromData(bytes));
    EXPECT_EQ(disabled.GetWidth(), 2u);
    EXPECT_EQ(disabled.GetHeight(), 2u);
    EXPECT_NE(disabled.GetPixelData(), nullptr);
    EXPECT_TRUE(Artifacts().empty());
}
#endif
