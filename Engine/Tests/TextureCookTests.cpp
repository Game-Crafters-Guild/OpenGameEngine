// Texture import cook: format selection, mip math, gamma-correct downsampling,
// normal renormalization, swizzle baking, BCn encode + KTX2 loader roundtrip,
// and cook determinism. Headless — no GPU device, no AssetManager (CookTexture
// is a pure function; the derived-cache integration is exercised at runtime).

#include <gtest/gtest.h>

#include "Assets/AssetDecodeGate.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Assets/TextureCookWorkers.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

// Minimal uncompressed 32-bit TGA (top-left origin) — stb-decodable without
// linking an encoder. Pixels are tightly-packed RGBA8, converted to BGRA here.
std::vector<uint8> MakeTga(uint32 width, uint32 height, const std::vector<uint8>& rgba)
{
    std::vector<uint8> tga(18, 0);
    tga[2] = 2; // uncompressed true-color
    tga[12] = static_cast<uint8>(width & 0xFF);
    tga[13] = static_cast<uint8>((width >> 8) & 0xFF);
    tga[14] = static_cast<uint8>(height & 0xFF);
    tga[15] = static_cast<uint8>((height >> 8) & 0xFF);
    tga[16] = 32;   // bits per pixel
    tga[17] = 0x20; // top-left origin
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
    {
        const uint8* px = rgba.data() + i * 4;
        tga.push_back(px[2]); // B
        tga.push_back(px[1]); // G
        tga.push_back(px[0]); // R
        tga.push_back(px[3]); // A
    }
    return tga;
}

// Minimal flat (non-RLE) Radiance .hdr — stb decodes plain RGBE scanlines.
std::vector<uint8> MakeHdr(uint32 width, uint32 height, float r, float g, float b)
{
    std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) +
                         " +X " + std::to_string(width) + "\n";
    std::vector<uint8> hdr(header.begin(), header.end());
    // RGBE encode (shared exponent).
    const float maxc = std::max(r, std::max(g, b));
    uint8 rgbe[4] = {0, 0, 0, 0};
    if (maxc >= 1e-32f)
    {
        int e = 0;
        const float scale = std::frexp(maxc, &e) * 256.0f / maxc;
        rgbe[0] = static_cast<uint8>(r * scale);
        rgbe[1] = static_cast<uint8>(g * scale);
        rgbe[2] = static_cast<uint8>(b * scale);
        rgbe[3] = static_cast<uint8>(e + 128);
    }
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
        hdr.insert(hdr.end(), rgbe, rgbe + 4);
    return hdr;
}

TextureCookInputs MakeInputs(TextureCookUsage usage, TextureColorSpace cs,
                             bool mips = true, uint32 mipLimit = 0)
{
    TextureCookInputs in;
    in.Settings.Usage = usage;
    in.Settings.MipsEnabled = mips;
    in.Settings.MipLimit = mipLimit;
    in.ColorSpace = cs;
    return in;
}

// ── Format selection ─────────────────────────────────────────────────────────

TEST(TextureCookResolve, SharedInputsRejectInvalidCoverageInsteadOfDefaultingItAway)
{
    for (const std::string compression : {"none", "bc1", "bc4", "bc5", "bc6h"})
    {
        TextureCookInputs inputs;
        std::string error;
        const bool ok = ResolveTextureCookInputs(".tga", [&](const char* key, std::string& value) {
            if (std::strcmp(key, kTextureCompressionMetaKey) == 0) value = compression;
            else if (std::strcmp(key, kTextureAlphaCoverageMetaKey) == 0) value = "1";
            else if (std::strcmp(key, kTextureAlphaCutoffMetaKey) == 0) value = "0.5000000596046448";
            else return false;
            return true;
        }, inputs, error);
        EXPECT_EQ(ok, compression == "none") << compression;
        EXPECT_EQ(error.empty(), ok);
        if (ok)
        {
            EXPECT_TRUE(inputs.Settings.PreserveAlphaCoverage);
            EXPECT_EQ(inputs.Settings.AlphaCoverageCutoff, std::nextafter(0.5f, 1.0f));
        }
    }
}

TEST(TextureCookResolve, AutoMapsUsageToFormat)
{
    TextureCookSettings s;
    s.Usage = TextureCookUsage::Color;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC7);
    s.Usage = TextureCookUsage::Normal;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC5);
    s.Usage = TextureCookUsage::Mask;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC4);
    s.Usage = TextureCookUsage::Packed;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC7);
}

TEST(TextureCookResolve, AutoUnknownUsageStaysUncompressed)
{
    TextureCookSettings s; // Usage = Auto
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::Uncompressed);
}

TEST(TextureCookResolve, AutoNeverPicksBc1)
{
    // BC1 is an explicit size-critical opt-in (gradient banding risk); no
    // usage may resolve to it implicitly.
    TextureCookSettings s;
    for (auto usage : {TextureCookUsage::Auto, TextureCookUsage::Color, TextureCookUsage::Normal,
                       TextureCookUsage::Mask, TextureCookUsage::Packed})
    {
        s.Usage = usage;
        EXPECT_NE(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC1);
        EXPECT_NE(ResolveTextureCookOutput(s, true, true, true), TextureCookOutput::BC1);
    }
}

TEST(TextureCookResolve, HdrSourceResolvesToBc6h)
{
    TextureCookSettings s;
    for (auto usage : {TextureCookUsage::Auto, TextureCookUsage::Color})
    {
        s.Usage = usage;
        EXPECT_EQ(ResolveTextureCookOutput(s, true, true, true), TextureCookOutput::BC6H);
    }
}

TEST(TextureCookResolve, ExplicitCompressionWins)
{
    TextureCookSettings s;
    s.Usage = TextureCookUsage::Color;
    s.Compression = TextureCookCompression::BC1;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::BC1);
    s.Compression = TextureCookCompression::None;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, true), TextureCookOutput::Uncompressed);
}

TEST(TextureCookResolve, BcDegradesToUncompressedWithoutDeviceOrEncoder)
{
    TextureCookSettings s;
    s.Usage = TextureCookUsage::Color;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, /*deviceSupportsBC=*/false, true),
              TextureCookOutput::Uncompressed);
    EXPECT_EQ(ResolveTextureCookOutput(s, false, true, /*encoderAvailable=*/false),
              TextureCookOutput::Uncompressed);
    s.Compression = TextureCookCompression::BC7;
    EXPECT_EQ(ResolveTextureCookOutput(s, false, false, true), TextureCookOutput::Uncompressed);
}

// Every desktop host cooks BC: vcpkg.json installs DirectXTex on Windows, Linux
// and macOS, and the Editor's import cook and the export's package cook encode
// through it. A host build without it (a platform missing from the manifest's
// gate, or the quiet find_package in Engine/CMakeLists.txt failing) resolves
// every texture to RGBA8, four times the bytes of BC7, and nothing else says so.
TEST(TextureCookEncoder, DesktopHostsCompileTheBcEncoder)
{
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    EXPECT_TRUE(IsTextureCookEncoderAvailable());
#else
    GTEST_SKIP() << "BC is cooked on desktop hosts only";
#endif
}

// ── Usage lattice ────────────────────────────────────────────────────────────

constexpr TextureCookUsage kAllUsages[] = {TextureCookUsage::Auto, TextureCookUsage::Color,
                                          TextureCookUsage::Normal, TextureCookUsage::Mask,
                                          TextureCookUsage::Packed};

TEST(TextureCookUsageLattice, DataUsagesWidenAndNeverNarrow)
{
    using U = TextureCookUsage;
    EXPECT_EQ(WidenTextureCookUsage(U::Auto, U::Mask), U::Mask);
    EXPECT_EQ(WidenTextureCookUsage(U::Mask, U::Packed), U::Packed);
    EXPECT_EQ(WidenTextureCookUsage(U::Packed, U::Mask), U::Packed);
    EXPECT_EQ(WidenTextureCookUsage(U::Packed, U::Auto), U::Packed);
    for (const U usage : kAllUsages)
        EXPECT_EQ(WidenTextureCookUsage(usage, usage), usage);
}

TEST(TextureCookUsageLattice, ColourAndNormalConflictWithEveryOtherUsageButAuto)
{
    using U = TextureCookUsage;
    for (const U other : {U::Color, U::Normal})
    {
        EXPECT_EQ(WidenTextureCookUsage(U::Auto, other), other);
        EXPECT_EQ(WidenTextureCookUsage(other, U::Auto), other);
    }
    EXPECT_FALSE(WidenTextureCookUsage(U::Color, U::Normal).has_value());
    EXPECT_FALSE(WidenTextureCookUsage(U::Color, U::Mask).has_value());
    EXPECT_FALSE(WidenTextureCookUsage(U::Color, U::Packed).has_value());
    EXPECT_FALSE(WidenTextureCookUsage(U::Normal, U::Mask).has_value());
    EXPECT_FALSE(WidenTextureCookUsage(U::Normal, U::Packed).has_value());
}

// The order two slots bind a texture in must not decide its cook.
TEST(TextureCookUsageLattice, EveryPairWidensTheSameInEitherOrder)
{
    for (const TextureCookUsage a : kAllUsages)
        for (const TextureCookUsage b : kAllUsages)
            EXPECT_EQ(WidenTextureCookUsage(a, b), WidenTextureCookUsage(b, a))
                << static_cast<int>(a) << " " << static_cast<int>(b);
}

// ── Mip math ─────────────────────────────────────────────────────────────────

// The relief march reads one channel, R, of a height map that is linear data: a single-channel
// cook (BC4) and a UNORM upload, never the source-extension sRGB guess a PNG would get.
TEST(TextureCookSlotUsage, HeightMapCooksSingleChannelAndUploadsLinear)
{
    EXPECT_EQ(TextureCookUsageForMaterialSlot(HashStringId("heightMap")), TextureCookUsage::Mask);
    EXPECT_TRUE(IsLinearTextureSlot(HashStringId("heightMap")));
    EXPECT_EQ(TextureCookUsageSlotName(HashStringId("heightMap")), "heightMap");
}

TEST(TextureCookMips, FullChainCounts)
{
    TextureCookSettings s;
    EXPECT_EQ(ComputeTextureCookMipCount(16, 16, s), 5u);
    EXPECT_EQ(ComputeTextureCookMipCount(1, 1, s), 1u);
    EXPECT_EQ(ComputeTextureCookMipCount(256, 1, s), 9u);  // non-square chains to 1x1
    EXPECT_EQ(ComputeTextureCookMipCount(100, 30, s), 7u); // NPOT: 100->50->25->12->6->3->1
}

TEST(TextureCookMips, DisabledAndLimited)
{
    TextureCookSettings s;
    s.MipsEnabled = false;
    EXPECT_EQ(ComputeTextureCookMipCount(256, 256, s), 1u);
    s.MipsEnabled = true;
    s.MipLimit = 3;
    EXPECT_EQ(ComputeTextureCookMipCount(256, 256, s), 3u);
    s.MipLimit = 99; // clamps to the natural chain
    EXPECT_EQ(ComputeTextureCookMipCount(256, 256, s), 9u);
}

// ── Meta round-trips + cook key ──────────────────────────────────────────────

TEST(TextureCookMeta, ParseFormatsRoundTrip)
{
    for (auto c : {TextureCookCompression::BC1, TextureCookCompression::BC4,
                   TextureCookCompression::BC5, TextureCookCompression::BC6H,
                   TextureCookCompression::BC7, TextureCookCompression::None})
    {
        TextureCookCompression parsed{};
        EXPECT_TRUE(ParseTextureCookCompressionMeta(TextureCookCompressionMetaValue(c), parsed));
        EXPECT_EQ(parsed, c);
    }
    TextureCookCompression parsed = TextureCookCompression::BC7;
    EXPECT_FALSE(ParseTextureCookCompressionMeta("auto", parsed));
    EXPECT_FALSE(ParseTextureCookCompressionMeta("", parsed));
    EXPECT_FALSE(ParseTextureCookCompressionMeta("astc", parsed));

    for (auto u : {TextureCookUsage::Color, TextureCookUsage::Normal, TextureCookUsage::Mask,
                   TextureCookUsage::Packed})
    {
        TextureCookUsage parsedU{};
        EXPECT_TRUE(ParseTextureCookUsageMeta(TextureCookUsageMetaValue(u), parsedU));
        EXPECT_EQ(parsedU, u);
    }
}

TEST(TextureCookKey, ConfigHashKeysEveryInput)
{
    const TextureCookInputs base = MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB);
    const uint64 h0 = ComputeTextureCookConfigHash(base, TextureCookOutput::BC7);
    EXPECT_EQ(h0, ComputeTextureCookConfigHash(base, TextureCookOutput::BC7)); // stable

    TextureCookInputs v = base;
    v.Settings.Compression = TextureCookCompression::BC1;
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    v = base;
    v.Settings.Usage = TextureCookUsage::Mask;
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    v = base;
    v.Settings.MipsEnabled = false;
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    v = base;
    v.Settings.MipLimit = 4;
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    v = base;
    v.ColorSpace = TextureColorSpace::Linear;
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    v = base;
    std::memcpy(v.Swizzle, "rrra", 4);
    EXPECT_NE(ComputeTextureCookConfigHash(v, TextureCookOutput::BC7), h0);
    EXPECT_NE(ComputeTextureCookConfigHash(base, TextureCookOutput::Uncompressed), h0);
}

// The Debug-only quick BC7 encode emits different bytes for the same source and
// settings, so it must land on a different cache key — otherwise a Debug-cooked
// low-quality artifact is silently adopted by a DebugFast/Release build. Quality
// defaults to the host configuration; export explicitly supplies the Player
// target quality. Both policies must use the same byte/key input.
TEST(TextureCookKey, ConfigHashSeparatesEncodeQuality)
{
    const TextureCookInputs base = MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB);

    const uint64 full = ComputeTextureCookConfigHash(base, TextureCookOutput::BC7,
                                                            TextureCookEncodeQuality::Full);
    const uint64 quick = ComputeTextureCookConfigHash(base, TextureCookOutput::BC7,
                                                             TextureCookEncodeQuality::QuickBC7);
    EXPECT_NE(full, quick) << "quick and full BC7 share a cache key";

    // The production hash must key as the build actually encodes, or the seam
    // above proves nothing about the artifacts that really get written.
    EXPECT_EQ(ComputeTextureCookConfigHash(base, TextureCookOutput::BC7),
              ComputeTextureCookConfigHash(
                  base, TextureCookOutput::BC7,
                  TextureCookEncodeQualityFor(TextureCookOutput::BC7)));

    // Quality is a BC7 property: every other output encodes full quality in
    // every config, so the two arms must not fork a key that cannot differ.
    for (auto out : {TextureCookOutput::BC1, TextureCookOutput::BC4, TextureCookOutput::BC5,
                     TextureCookOutput::BC6H})
    {
        EXPECT_EQ(TextureCookEncodeQualityFor(out), TextureCookEncodeQuality::Full);
        EXPECT_EQ(ComputeTextureCookConfigHash(base, out, TextureCookEncodeQuality::Full),
                  ComputeTextureCookConfigHash(base, out, TextureCookEncodeQuality::QuickBC7));
    }

    // And the policy itself, not merely its agreement with the key: the
    // mode-6-only search is a Debug-config cost tradeoff, and a shipping build
    // that quietly adopted it would ship visibly worse textures. Nothing else
    // catches that — a decoded-error bound cannot, because the smooth-gradient
    // content BC7 is near-lossless on decodes to the same error under both
    // searches (TextureCookQuality.Bc1BandsOnSmoothGradientsBc7DoesNot).
#if defined(_DEBUG)
    EXPECT_EQ(TextureCookEncodeQualityFor(TextureCookOutput::BC7),
              TextureCookEncodeQuality::QuickBC7)
        << "Debug must cook the quick BC7 search; the full search is minutes per level here";
#else
    EXPECT_EQ(TextureCookEncodeQualityFor(TextureCookOutput::BC7),
              TextureCookEncodeQuality::Full)
        << "a non-Debug build must cook the full BC7 search — quick is a Debug-only tradeoff";
#endif
}

TEST(TextureCookKey, SourceHashSeparatesContent)
{
    const uint8 a[] = {1, 2, 3, 4};
    const uint8 b[] = {1, 2, 3, 5};
    EXPECT_NE(ComputeTextureCookSourceHash(a, sizeof(a)), ComputeTextureCookSourceHash(b, sizeof(b)));
    EXPECT_EQ(ComputeTextureCookSourceHash(a, sizeof(a)), ComputeTextureCookSourceHash(a, sizeof(a)));
}

#if defined(GE_HAVE_KTX)

// Load cooked KTX2 bytes through the production loader.
bool LoadCooked(const std::vector<uint8>& ktx2, TextureAsset& asset)
{
    Vector<uint8> bytes(ktx2.begin(), ktx2.end());
    return asset.LoadFromData(bytes);
}

// ── Gamma-correct downsampling ───────────────────────────────────────────────

TEST(TextureCookFilter, SrgbDownsampleAveragesInLinearSpace)
{
    // 2x2 white/black checker -> 1x1. Linear-space average is 0.5 -> sRGB
    // encode ~= 188. A naive byte average would land at ~128 (the classic
    // "mips get dark" bug).
    std::vector<uint8> rgba = {255, 255, 255, 255, 0, 0, 0, 255,
                               0, 0, 0, 255, 255, 255, 255, 255};
    const std::vector<uint8> tga = MakeTga(2, 2, rgba);

    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga",
                            MakeInputs(TextureCookUsage::Auto, TextureColorSpace::SRGB),
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;

    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    ASSERT_EQ(asset.GetMipmapLevels(), 2u);
    ASSERT_EQ(asset.GetFormat(), TextureFormat::RGBA8);
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::SRGB);

    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 2u);
    const uint8* mip1 = asset.GetPixelData() + chain[1].Offset;
    EXPECT_GE(mip1[0], 183) << "sRGB downsample must average in linear space";
    EXPECT_LE(mip1[0], 193);
    EXPECT_EQ(mip1[3], 255);
}

TEST(TextureCookFilter, LinearDownsampleAveragesBytes)
{
    std::vector<uint8> rgba = {255, 255, 255, 255, 0, 0, 0, 255,
                               0, 0, 0, 255, 255, 255, 255, 255};
    const std::vector<uint8> tga = MakeTga(2, 2, rgba);

    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga",
                            MakeInputs(TextureCookUsage::Auto, TextureColorSpace::Linear),
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;

    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::Linear);
    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 2u);
    const uint8* mip1 = asset.GetPixelData() + chain[1].Offset;
    EXPECT_GE(mip1[0], 125);
    EXPECT_LE(mip1[0], 131);
}

// ── Normal renormalization ───────────────────────────────────────────────────

TEST(TextureCookFilter, NormalUsageRenormalizesMips)
{
    // Two normals tilted ±45° about X average to (0, 0, 0.707) — a shortened
    // vector that flattens relief at distance if left unnormalized (mip B would
    // sit at ~218). Renormalization must restore unit length: (0, 0, 1).
    std::vector<uint8> rgba = {218, 128, 218, 255, 37, 128, 218, 255,
                               218, 128, 218, 255, 37, 128, 218, 255};
    const std::vector<uint8> tga = MakeTga(2, 2, rgba);

    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga",
                            MakeInputs(TextureCookUsage::Normal, TextureColorSpace::Linear),
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;

    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 2u);
    const uint8* mip1 = asset.GetPixelData() + chain[1].Offset;
    EXPECT_GE(mip1[2], 250) << "renormalized filtered normal must point +Z (unnormalized would be ~218)";
    EXPECT_NEAR(mip1[0], 128, 3);
    EXPECT_NEAR(mip1[1], 128, 3);
}

// ── Swizzle baking ───────────────────────────────────────────────────────────

TEST(TextureCookSwizzle, BakedIntoPayload)
{
    // Source R=200, G=10; swizzle "gggg" must land G in every output channel.
    std::vector<uint8> rgba(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i)
    {
        rgba[i * 4 + 0] = 200;
        rgba[i * 4 + 1] = 10;
        rgba[i * 4 + 2] = 60;
        rgba[i * 4 + 3] = 255;
    }
    const std::vector<uint8> tga = MakeTga(4, 4, rgba);

    TextureCookInputs in = MakeInputs(TextureCookUsage::Auto, TextureColorSpace::Linear,
                                      /*mips=*/false);
    std::memcpy(in.Swizzle, "gggg", 4);

    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in,
                            TextureCookOutput::Uncompressed, cooked, error))
        << error;

    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    const uint8* px = asset.GetPixelData();
    EXPECT_EQ(px[0], 10);
    EXPECT_EQ(px[1], 10);
    EXPECT_EQ(px[2], 10);
    EXPECT_EQ(px[3], 10);
}

// ── Determinism ──────────────────────────────────────────────────────────────

TEST(TextureCookDeterminism, UncompressedCookIsByteIdentical)
{
    std::vector<uint8> rgba(16 * 16 * 4);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<uint8>((i * 7) & 0xFF);
    const std::vector<uint8> tga = MakeTga(16, 16, rgba);
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Auto, TextureColorSpace::SRGB);

    std::vector<uint8> cookedA, cookedB;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::Uncompressed,
                            cookedA, error)) << error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::Uncompressed,
                            cookedB, error)) << error;
    ASSERT_EQ(cookedA.size(), cookedB.size());
    EXPECT_EQ(std::memcmp(cookedA.data(), cookedB.data(), cookedA.size()), 0);
}

TEST(TextureCookDeterminism, Bc7CookIsByteIdentical)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";
    std::vector<uint8> rgba(16 * 16 * 4);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<uint8>((i * 13) & 0xFF);
    const std::vector<uint8> tga = MakeTga(16, 16, rgba);
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB);

    std::vector<uint8> cookedA, cookedB;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::BC7,
                            cookedA, error)) << error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::BC7,
                            cookedB, error)) << error;
    ASSERT_EQ(cookedA.size(), cookedB.size());
    EXPECT_EQ(std::memcmp(cookedA.data(), cookedB.data(), cookedA.size()), 0);
}

// One tightly-packed level, content varying strongly along Y so a band written
// at the wrong offset cannot coincidentally hold the right bytes.
TextureCookMipLevel MakeLevel(uint32 width, uint32 height, bool isFloat)
{
    TextureCookMipLevel level;
    level.Width = width;
    level.Height = height;
    const float spanX = static_cast<float>(std::max(1u, width - 1));
    const float spanY = static_cast<float>(std::max(1u, height - 1));

    if (isFloat)
    {
        std::vector<float> pixels(static_cast<size_t>(width) * height * 4);
        for (uint32 y = 0; y < height; ++y)
        {
            for (uint32 x = 0; x < width; ++x)
            {
                float* px = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
                px[0] = 0.05f + 3.0f * (static_cast<float>(y) / spanY);
                px[1] = 0.05f + 1.5f * (static_cast<float>(x) / spanX);
                px[2] = 0.5f;
                px[3] = 1.0f;
            }
        }
        const auto* bytes = reinterpret_cast<const uint8*>(pixels.data());
        level.Bytes.assign(bytes, bytes + pixels.size() * sizeof(float));
        return level;
    }

    level.Bytes.resize(static_cast<size_t>(width) * height * 4);
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            uint8* px = level.Bytes.data() + (static_cast<size_t>(y) * width + x) * 4;
            px[0] = static_cast<uint8>(20 + (210.0f * static_cast<float>(y)) / spanY);
            px[1] = static_cast<uint8>(30 + (180.0f * static_cast<float>(x)) / spanX);
            px[2] = static_cast<uint8>(128);
            px[3] = 255;
        }
    }
    return level;
}

// Band size is a latency knob, not a quality one: the tight per-band payloads
// must concatenate into exactly the bytes a single whole-level Compress
// produces. That byte-exactness is the whole safety argument for banding (BCn
// blocks encode independently at TEX_COMPRESS_DEFAULT, and every band starts on
// a block-row boundary), so it is asserted directly rather than inferred —
// every level of a full mip chain down to 1x1, encoded three ways: as one
// whole-level call, at the cook's default granularity, and at maximum banding
// (one block per band, so even a 2x1 level bands).
//
// Levels are synthesized rather than downsampled because the property under test
// is per-level band assembly; building them directly hits the awkward extents
// (partial right column, partial bottom block row, 2x1, 1x1) exactly.
//
// This covers the ENCODER's band assembly. That the cook actually encodes in
// bands — rather than passing a whole-level granularity — is what
// TextureCookCancel.PreemptsEncodeInProgress pins, by cancelling one mid-level.
TEST(TextureCookDeterminism, BandedEncodeMatchesWholeLevel)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    struct BandCase
    {
        TextureCookOutput Output;
        uint32 Width;
        uint32 Height;
        bool IsFloat;
    };
    const BandCase cases[] = {
        // Odd on both axes (partial right block column AND partial bottom block
        // row) and past kTextureCookBandBlocks, so the DEFAULT granularity is
        // genuinely multi-band here — 1056 base blocks over 512.
        {TextureCookOutput::BC1, 130, 126, false},
        {TextureCookOutput::BC4, 130, 126, false},
        // BC7 is what the cook picks for color, and by far the costliest per
        // block: same odd geometry, at a size the suite can afford.
        {TextureCookOutput::BC7, 66, 62, false},
        {TextureCookOutput::BC5, 66, 62, false},
        // Float source: band offsets are byte arithmetic over the row pitch, so
        // the 16-byte pixel pitch is its own case.
        {TextureCookOutput::BC6H, 34, 30, true},
    };

    // Above any level's block count here, so the band loop issues exactly one
    // Compress over the full level — the whole-level call being differenced.
    constexpr uint32 kWholeLevelBand = 1u << 24;

    for (const auto& c : cases)
    {
        TextureCookSettings fullChain; // mips on, no limit
        const uint32 levels = ComputeTextureCookMipCount(c.Width, c.Height, fullChain);
        ASSERT_GT(levels, 1u);

        uint32 w = c.Width, h = c.Height;
        for (uint32 level = 0; level < levels; ++level)
        {
            const TextureCookMipLevel src = MakeLevel(w, h, c.IsFloat);
            const std::string where = "output " + std::to_string(static_cast<int>(c.Output)) +
                                      " level " + std::to_string(level) + " (" +
                                      std::to_string(w) + "x" + std::to_string(h) + ")";

            std::vector<uint8> whole, defaultBand, maxBand;
            std::string error;
            ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, kWholeLevelBand, whole,
                                                 error))
                << where << ": " << error;
            ASSERT_FALSE(whole.empty()) << where;
            ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, /*bandBlocks=*/1, maxBand,
                                                 error))
                << where << ": " << error;

            // The default granularity only adds information on a level it
            // actually splits; below that it IS the whole-level call, and BC7 /
            // BC6H levels are too costly to encode for nothing.
            const uint32 blocks = ((w + 3) / 4) * ((h + 3) / 4);
            if (blocks > kTextureCookBandBlocks)
            {
                ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, /*bandBlocks=*/0,
                                                     defaultBand, error))
                    << where << ": " << error;
                ASSERT_EQ(defaultBand.size(), whole.size()) << where;
                EXPECT_EQ(std::memcmp(defaultBand.data(), whole.data(), whole.size()), 0)
                    << where << ": default granularity differs from the whole-level encode";
            }

            ASSERT_EQ(maxBand.size(), whole.size()) << where;
            EXPECT_EQ(std::memcmp(maxBand.data(), whole.data(), whole.size()), 0)
                << where << ": one-block bands differ from the whole-level encode";

            w = std::max(w / 2u, 1u);
            h = std::max(h / 2u, 1u);
        }
    }
}

// ── BCn encode + loader roundtrips ───────────────────────────────────────────

std::vector<uint8> GradientTga(uint32 size)
{
    std::vector<uint8> rgba(static_cast<size_t>(size) * size * 4);
    for (uint32 y = 0; y < size; ++y)
    {
        for (uint32 x = 0; x < size; ++x)
        {
            uint8* px = rgba.data() + (static_cast<size_t>(y) * size + x) * 4;
            px[0] = static_cast<uint8>((x * 255) / (size - 1));
            px[1] = static_cast<uint8>((y * 255) / (size - 1));
            px[2] = 90;
            px[3] = 255;
        }
    }
    return MakeTga(size, size, rgba);
}

struct BcRoundtripCase
{
    TextureCookOutput Output;
    TextureFormat ExpectedFormat;
    uint32 ExpectedChannels;
    uint64 ExpectedMip0Bytes; // 16x16 = 4x4 blocks
};

TEST(TextureCookBc, RoundtripsThroughKtx2Loader)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    const std::vector<uint8> tga = GradientTga(16);
    const BcRoundtripCase cases[] = {
        {TextureCookOutput::BC7, TextureFormat::BC7, 4, 16 * 16},
        {TextureCookOutput::BC5, TextureFormat::BC5, 2, 16 * 16},
        {TextureCookOutput::BC4, TextureFormat::BC4, 1, 16 * 8},
        {TextureCookOutput::BC1, TextureFormat::BC1, 4, 16 * 8},
    };
    for (const auto& c : cases)
    {
        std::vector<uint8> cooked;
        std::string error;
        ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga",
                                MakeInputs(TextureCookUsage::Auto, TextureColorSpace::SRGB),
                                c.Output, cooked, error))
            << error << " (output " << static_cast<int>(c.Output) << ")";

        TextureAsset asset(GUID::Null(), "cooktest.ktx2");
        ASSERT_TRUE(LoadCooked(cooked, asset)) << "output " << static_cast<int>(c.Output);
        EXPECT_EQ(asset.GetFormat(), c.ExpectedFormat);
        EXPECT_TRUE(asset.IsBlockCompressed());
        EXPECT_EQ(asset.GetChannels(), c.ExpectedChannels);
        EXPECT_EQ(asset.GetMipmapLevels(), 5u);
        const auto& chain = asset.GetMipChain();
        ASSERT_EQ(chain.size(), 5u);
        EXPECT_EQ(chain[0].Size, c.ExpectedMip0Bytes) << "output " << static_cast<int>(c.Output);
        EXPECT_EQ(chain[0].Offset, 0u);
    }
}

TEST(TextureCookBc, HdrCooksToBc6hLinear)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    const std::vector<uint8> hdr = MakeHdr(8, 8, 2.5f, 1.0f, 0.25f);
    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(hdr.data(), hdr.size(), ".hdr",
                            MakeInputs(TextureCookUsage::Auto, TextureColorSpace::Linear),
                            TextureCookOutput::BC6H, cooked, error))
        << error;

    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    EXPECT_EQ(asset.GetFormat(), TextureFormat::BC6H);
    EXPECT_TRUE(asset.IsBlockCompressed());
    EXPECT_EQ(asset.GetColorSpace(), TextureColorSpace::Linear);
    EXPECT_EQ(asset.GetMipmapLevels(), 4u); // 8,4,2,1
    // BC6H: 16 bytes per 4x4 block -> 8x8 = 4 blocks.
    EXPECT_EQ(asset.GetMipChain()[0].Size, 4u * 16u);
}

// The gradient-banding argument behind "Auto never picks BC1", in numbers:
// encode a smooth low-frequency gradient (the classic stylized-content case)
// to BC1 and BC7, decode both, and compare max per-pixel error. BC1's 5:6:5
// endpoints + 4-level interpolation band visibly; BC7 stays near-lossless.
//
// Keep the ramp 1-D — every channel varying together along one diagonal. That is
// not just a BC1 argument: Debug encodes BC7 with a single-subset mode only (see
// TextureCookEncodeQualityFor), so content whose channels ramp along DIFFERENT
// axes cannot be fitted by one line and reds the BC7 bound in Debug alone.
TEST(TextureCookQuality, Bc1BandsOnSmoothGradientsBc7DoesNot)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    constexpr uint32 kSize = 64;
    std::vector<uint8> rgba(static_cast<size_t>(kSize) * kSize * 4);
    for (uint32 y = 0; y < kSize; ++y)
    {
        for (uint32 x = 0; x < kSize; ++x)
        {
            uint8* px = rgba.data() + (static_cast<size_t>(y) * kSize + x) * 4;
            // Slow diagonal ramp over a mid-tone band: the worst case for
            // 4-level BC1 interpolation, gentle for BC7.
            const float t = (x + y) / float(2 * kSize - 2);
            px[0] = static_cast<uint8>(96.0f + 48.0f * t);
            px[1] = static_cast<uint8>(104.0f + 40.0f * t);
            px[2] = static_cast<uint8>(112.0f + 32.0f * t);
            px[3] = 255;
        }
    }
    const std::vector<uint8> tga = MakeTga(kSize, kSize, rgba);
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Color, TextureColorSpace::Linear,
                                            /*mips=*/false);

    auto maxError = [&](TextureCookOutput out, TextureFormat fmt) -> int {
        std::vector<uint8> cooked;
        std::string error;
        EXPECT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, out, cooked, error)) << error;
        TextureAsset asset(GUID::Null(), "cooktest.ktx2");
        EXPECT_TRUE(LoadCooked(cooked, asset));
        EXPECT_EQ(asset.GetFormat(), fmt);
        std::vector<uint8> decoded;
        EXPECT_TRUE(DecodeBlockPayloadRGBA8(fmt, kSize, kSize, asset.GetPixelData(),
                                            static_cast<size_t>(asset.GetMipChain()[0].Size),
                                            decoded));
        int worst = 0;
        for (size_t i = 0; i < decoded.size(); ++i)
        {
            if (i % 4 == 3)
                continue; // alpha
            worst = std::max(worst, std::abs(int(decoded[i]) - int(rgba[i])));
        }
        return worst;
    };

    const int bc7Err = maxError(TextureCookOutput::BC7, TextureFormat::BC7);
    const int bc1Err = maxError(TextureCookOutput::BC1, TextureFormat::BC1);
    EXPECT_LE(bc7Err, 2) << "BC7 must be near-lossless on smooth gradients";
    EXPECT_GT(bc1Err, bc7Err) << "if BC1 matches BC7 here, revisit the Auto bias";
    // Not asserting a big BC1 bound — encoder-dependent — but record it.
    RecordProperty("bc1MaxError", bc1Err);
    RecordProperty("bc7MaxError", bc7Err);
}

TEST(TextureCookBc, MipLimitClampsChain)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";
    const std::vector<uint8> tga = GradientTga(16);
    std::vector<uint8> cooked;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga",
                            MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB,
                                       /*mips=*/true, /*mipLimit=*/2),
                            TextureCookOutput::BC7, cooked, error))
        << error;
    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    EXPECT_EQ(asset.GetMipmapLevels(), 2u);
}

// A huge bandBlocks over a NARROW level is the corner where the band arithmetic
// can overflow: few blocks per row scales the row count up, and the multiply to
// pixels wraps a uint32 to 0. The band is then a zero-height image, which the
// encoder rejects (E_INVALIDARG), so an unclamped cook fails outright on a level
// it should have encoded in a single call.
TEST(TextureCookBc, HugeBandBlocksOnNarrowLevelEncodesWholeLevel)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    // Width 4 == one block per row, the worst case for the scaling above.
    constexpr uint32 kWidth = 4;
    constexpr uint32 kHeight = 64;
    const TextureCookMipLevel src = MakeLevel(kWidth, kHeight, /*isFloat=*/false);

    std::vector<uint8> whole;
    std::string error;
    ASSERT_TRUE(CompressTextureCookLevel(src, /*isFloat=*/false, TextureCookOutput::BC1,
                                         /*bandBlocks=*/1u << 24, whole, error))
        << error;
    ASSERT_FALSE(whole.empty());

    // bandHeight is bandBlockRows * 4, so it wraps to exactly 0 precisely when
    // the row count is a multiple of 2^30 — these are the values that wrap
    // bandHeight to zero, and they are the whole set of them for a
    // one-block-per-row level. (Most other huge values wrap to a merely enormous
    // bandHeight, which still encodes in a single band, so they would not
    // exercise this at all.)
    for (uint32 bandBlocks : {1u << 30, 1u << 31, 3u << 30})
    {
        std::vector<uint8> banded;
        ASSERT_TRUE(CompressTextureCookLevel(src, /*isFloat=*/false, TextureCookOutput::BC1,
                                             bandBlocks, banded, error))
            << "bandBlocks=" << bandBlocks << ": " << error;
        ASSERT_EQ(banded.size(), whole.size()) << "bandBlocks=" << bandBlocks;
        EXPECT_EQ(std::memcmp(banded.data(), whole.data(), whole.size()), 0)
            << "bandBlocks=" << bandBlocks << ": an over-large band must issue one whole-level call";
    }
}

// Block encoding is issued in horizontal bands so a cancel can preempt a level in
// progress (see CompressTextureCookLevel). That is only safe if banding is
// byte-exact, and the realistic failure mode is assembly: a wrong source offset or
// payload append order puts blocks in the wrong place. Difference the cooked
// payload against the same level encoded in ONE call — the whole-level encode has
// no assembly to get wrong, so byte-equality is a complete proof of assembly, down
// to a single-block-row offset. (Only of assembly: both arms are handed the same
// row pitch and source format, so a bug in those is invisible here.)
//
// The differential is deliberately not a decoded-error bound. How closely BC7
// tracks the source is a property of the encoder's mode SEARCH, which is a build
// property, not a cook one: Debug encodes BC7 mode-6-only (see
// TextureCookEncodeQualityFor), and one mode-6 line cannot follow this image's R
// ramp along Y and G ramp along X at once, so the same correctly-assembled bytes
// decode 4 off in Debug and 2 off in a full-search build. Which search a build is
// entitled to use is pinned directly, per config, by
// TextureCookKey.ConfigHashSeparatesEncodeQuality; near-losslessness itself by
// TextureCookQuality.Bc1BandsOnSmoothGradientsBc7DoesNot, on 1-D ramp content.
//
// What this adds over TextureCookDeterminism.BandedEncodeMatchesWholeLevel, which
// differences CompressTextureCookLevel directly: the whole path through
// CookTexture at its own default granularity, the KTX2 container, and the
// production loader. The extents keep that path honest — 256 wide is 64 blocks per
// row, so the default granularity is 8 block rows and the level really splits, and
// 126 rows is a multiple of neither the 32-row band nor the 4-pixel block, so the
// last band is short and its bottom block row is padded.
TEST(TextureCookBc, BandedEncodeAssemblesCorrectly)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    constexpr uint32 kWidth = 256;
    constexpr uint32 kHeight = 126;
    constexpr uint32 kLevelBlocks = ((kWidth + 3) / 4) * ((kHeight + 3) / 4);
    // A level the cook issues in one band would make the differential vacuous.
    static_assert(kLevelBlocks > kTextureCookBandBlocks,
                  "level must exceed the default band size for the cook to band it");

    std::vector<uint8> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint32 y = 0; y < kHeight; ++y)
    {
        for (uint32 x = 0; x < kWidth; ++x)
        {
            uint8* px = rgba.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            // Varies along Y within every block, so no two block rows encode to
            // the same bytes and a band read from the wrong offset cannot land on
            // a payload that happens to match.
            px[0] = static_cast<uint8>(40 + (200 * y) / (kHeight - 1));
            px[1] = static_cast<uint8>(30 + (180 * x) / (kWidth - 1));
            px[2] = static_cast<uint8>(128);
            px[3] = 255;
        }
    }
    const std::vector<uint8> tga = MakeTga(kWidth, kHeight, rgba);
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Color, TextureColorSpace::Linear,
                                            /*mips=*/false);

    std::vector<uint8> cooked, cooked2;
    std::string error;
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::BC7,
                            cooked, error))
        << error;
    TextureAsset asset(GUID::Null(), "cooktest.ktx2");
    ASSERT_TRUE(LoadCooked(cooked, asset));
    ASSERT_EQ(asset.GetFormat(), TextureFormat::BC7);

    // The control: one Compress over the whole level, from the source pixels — so
    // this pins the decode the cook fed the encoder as well as the assembly. A band
    // at the level's own block count issues a single call, which is the clamp
    // TextureCookBc.HugeBandBlocksOnNarrowLevelEncodesWholeLevel pins.
    TextureCookMipLevel level;
    level.Width = kWidth;
    level.Height = kHeight;
    level.Bytes = rgba;
    std::vector<uint8> whole;
    ASSERT_TRUE(CompressTextureCookLevel(level, /*isFloat=*/false, TextureCookOutput::BC7,
                                         /*bandBlocks=*/kLevelBlocks, whole, error))
        << error;

    const auto& chain = asset.GetMipChain();
    ASSERT_EQ(chain.size(), 1u);
    ASSERT_EQ(static_cast<size_t>(chain[0].Size), whole.size());
    EXPECT_EQ(std::memcmp(asset.GetPixelData(), whole.data(), whole.size()), 0)
        << "the cook's banded payload differs from a whole-level encode of the same pixels — a "
           "band read the wrong source rows, or the bands were appended out of order";

    // And the banded encode is stable run to run.
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::BC7,
                            cooked2, error))
        << error;
    ASSERT_EQ(cooked.size(), cooked2.size());
    EXPECT_EQ(std::memcmp(cooked.data(), cooked2.data(), cooked.size()), 0);
}

// A sprite exported white under its alpha mask (issue 2237). DirectXTex picks each
// BC7 mode 6 and mode 7 endpoint's p-bit by a vote of the channel LSBs, so a
// transparent endpoint under white RGB decodes to alpha 1/255 (mode 6) or 4/255
// (mode 7), and an HDR particle colour turns that floor into a visible quad. Three
// blocks, encoded at both searches the cook uses. Without the repair every alpha 0
// texel of all three blocks (16 + 4 + 8) decodes to 1/255 under both searches: the
// full search does not escape the vote by picking another mode.
//  - block 0: transparent white.
//  - block 1: white with an alpha ramp from four transparent texels up to 96, the
//    soft edge of a glow.
//  - block 2: two transparent white columns next to two opaque orange columns. The
//    same vote holds its opaque endpoint under even RGB at alpha 254 (issue 2477);
//    this test does not check that floor.
// Every alpha 0 source texel must decode to alpha 0. Opaque texels stay within 2
// steps of the source per channel: each colour is an exact endpoint, so only the
// encoder's own endpoint quantization (1 step) is left, and the repair never
// touches an endpoint with alpha bits. Visible ramp texels keep RGB within 5 steps
// of white: the repair lowers a transparent endpoint's RGB by one p-bit step, 4
// steps in mode 7, and the encoder's quantization adds 1.
TEST(TextureCookBc, Bc7TransparentTexelsDecodeToZeroAlpha)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    constexpr uint32 kWidth = 12;
    constexpr uint32 kHeight = 4;
    constexpr uint8 kRampStep = 8;
    constexpr int kOpaqueBound = 2;
    constexpr int kRampRgbBound = 5;
    const uint8 orange[4] = {200, 120, 40, 255};

    TextureCookMipLevel level;
    level.Width = kWidth;
    level.Height = kHeight;
    level.Bytes.assign(static_cast<size_t>(kWidth) * kHeight * 4, 255);
    for (uint32 y = 0; y < kHeight; ++y)
    {
        for (uint32 x = 0; x < kWidth; ++x)
        {
            uint8* px = level.Bytes.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            const uint32 block = x / 4;
            const uint32 inBlock = y * 4 + x % 4;
            if (block == 0)
                px[3] = 0;
            else if (block == 1)
                px[3] = static_cast<uint8>(inBlock < 4 ? 0u : (inBlock - 3) * kRampStep);
            else if (x % 4 < 2)
                px[3] = 0;
            else
                std::memcpy(px, orange, 4);
        }
    }

    for (const auto quality : {TextureCookEncodeQuality::Full, TextureCookEncodeQuality::QuickBC7})
    {
        SCOPED_TRACE(quality == TextureCookEncodeQuality::Full ? "full search" : "QuickBC7");
        std::vector<uint8> payload;
        std::string error;
        ASSERT_TRUE(CompressTextureCookLevel(level, /*isFloat=*/false, TextureCookOutput::BC7,
                                             /*bandBlocks=*/0, payload, error, {}, quality))
            << error;
        std::vector<uint8> decoded;
        ASSERT_TRUE(DecodeBlockPayloadRGBA8(TextureFormat::BC7, kWidth, kHeight, payload.data(),
                                            payload.size(), decoded));
        ASSERT_EQ(decoded.size(), level.Bytes.size());

        for (uint32 y = 0; y < kHeight; ++y)
        {
            for (uint32 x = 0; x < kWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 4;
                const uint8* source = level.Bytes.data() + offset;
                const uint8* result = decoded.data() + offset;
                if (source[3] == 0)
                {
                    EXPECT_EQ(result[3], 0) << "transparent texel (" << x << ", " << y << ") decodes to alpha "
                                            << static_cast<int>(result[3]) << "/255";
                    continue;
                }
                const int bound = source[3] == 255 ? kOpaqueBound : kRampRgbBound;
                const int channels = source[3] == 255 ? 4 : 3;
                for (int c = 0; c < channels; ++c)
                    EXPECT_LE(std::abs(static_cast<int>(result[c]) - static_cast<int>(source[c])), bound)
                        << "texel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

// ── Cancellation ─────────────────────────────────────────────────────────────
//
// The cook polls its cancel flag before each mip level's encode AND between the
// block-row bands within a level, so a cancel preempts it at the next band
// boundary rather than at the end of a level. The first three below pin the
// contract the callers rely on: a cancelled cook reports
// kTextureCookCancelledError and yields NO bytes — which is what makes a partial
// derived-cache entry impossible, since TextureAsset writes the cache only after
// a true return. PreemptsEncodeInProgress covers the band poll itself, which is
// the only poll that can fire once a single large level is under way.

TEST(TextureCookCancel, FlagSetBeforeStartProducesNoOutput)
{
    const std::vector<uint8> tga = GradientTga(16);
    auto cancel = std::make_shared<std::atomic<bool>>(true);

    std::vector<uint8> cooked;
    std::string error;
    EXPECT_FALSE(CookTexture(tga.data(), tga.size(), ".tga",
                             MakeInputs(TextureCookUsage::Auto, TextureColorSpace::Linear),
                             TextureCookOutput::Uncompressed, cooked, error, [cancel]() { return cancel && cancel->load(); }));
    EXPECT_EQ(error, std::string(kTextureCookCancelledError));
    EXPECT_TRUE(cooked.empty());
}

TEST(TextureCookCancel, FlagSetBeforeStartSkipsBlockEncode)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";
    const std::vector<uint8> tga = GradientTga(16);
    auto cancel = std::make_shared<std::atomic<bool>>(true);

    std::vector<uint8> cooked;
    std::string error;
    EXPECT_FALSE(CookTexture(tga.data(), tga.size(), ".tga",
                             MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB),
                             TextureCookOutput::BC7, cooked, error, [cancel]() { return cancel && cancel->load(); }));
    EXPECT_EQ(error, std::string(kTextureCookCancelledError));
    EXPECT_TRUE(cooked.empty());
}

TEST(TextureCookCancel, UnsetFlagCooksIdenticallyToNoFlag)
{
    const std::vector<uint8> tga = GradientTga(16);
    auto cancel = std::make_shared<std::atomic<bool>>(false);

    std::vector<uint8> withFlag, withoutFlag;
    std::string error;
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Auto, TextureColorSpace::Linear);
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::Uncompressed,
                            withFlag, error, [cancel]() { return cancel && cancel->load(); }))
        << error;
    // A null token is the not-cancellable case (synchronous load, tooling); it
    // must cook byte-identically to an unset flag.
    ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::Uncompressed,
                            withoutFlag, error))
        << error;
    ASSERT_FALSE(withFlag.empty());
    ASSERT_EQ(withFlag.size(), withoutFlag.size());
    EXPECT_EQ(std::memcmp(withFlag.data(), withoutFlag.data(), withFlag.size()), 0);
}

// A cancel racing the start of a cook. The canceller sets the flag as soon as it
// is scheduled, so in practice this lands on the pre-encode check and returns in
// single-digit milliseconds; what it pins is that a flag set concurrently (not
// before the call) is still observed, whichever poll sees it first. Preemption of
// an encode ALREADY under way is PreemptsEncodeInProgress below — this test
// cannot cover it, because it never reaches the band loop.
TEST(TextureCookCancel, CancelRacingTheStartIsObserved)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";
    const std::vector<uint8> tga = GradientTga(256);
    auto cancel = std::make_shared<std::atomic<bool>>(false);

    std::thread canceller([cancel] { cancel->store(true, std::memory_order_release); });

    std::vector<uint8> cooked;
    std::string error;
    const bool ok = CookTexture(tga.data(), tga.size(), ".tga",
                                MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB),
                                TextureCookOutput::BC7, cooked, error, [cancel]() { return cancel && cancel->load(); });
    canceller.join();

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, std::string(kTextureCookCancelledError));
    EXPECT_TRUE(cooked.empty());
}

// The band poll is what bounds a cancel's wait once a single large level is
// under way — the case the whole banding change exists for (a 2048x2048 BC7 base
// level is one opaque Compress otherwise). With mips OFF there is exactly one
// level, so the per-level poll fires once before the encode starts and a cancel
// arriving later can ONLY be seen between bands. Cancel partway in and require
// the cook to stop well short of an uncancelled control measured here, on this
// machine and build.
TEST(TextureCookCancel, PreemptsEncodeInProgress)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    // BC7, and sized so the level splits into four bands (1600 blocks over
    // kTextureCookBandBlocks). BC7 is not incidental here: it is the format whose
    // per-block cost makes a base level minutes long, and it dwarfs the source
    // decode, so the cancel below lands inside the encode rather than ahead of
    // it — which is what makes this a test of the BAND poll and not of the
    // per-level one.
    const std::vector<uint8> tga = GradientTga(160);
    const TextureCookInputs in = MakeInputs(TextureCookUsage::Color, TextureColorSpace::Linear,
                                            /*mips=*/false);

    const auto timedCook = [&](const std::shared_ptr<std::atomic<bool>>& cancel, bool& okOut,
                               std::string& errorOut) {
        std::vector<uint8> cooked;
        const auto start = std::chrono::steady_clock::now();
        okOut = CookTexture(tga.data(), tga.size(), ".tga", in, TextureCookOutput::BC7, cooked,
                            errorOut, [cancel]() { return cancel && cancel->load(); });
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (!okOut)
            EXPECT_TRUE(cooked.empty());
        return ms;
    };

    bool controlOk = false;
    std::string controlError;
    const double controlMs = timedCook(std::make_shared<std::atomic<bool>>(false), controlOk,
                                       controlError);
    ASSERT_TRUE(controlOk) << controlError;
    // Instrument check: with a control this short there is no room to tell
    // "stopped at a band" from "ran to the end", so a pass would mean nothing.
    // A fast config (Release, or a quick-mode encoder) is not a defect in the
    // cook, so skip rather than fail — the reading, not the code, is what the
    // configuration invalidated.
    if (controlMs <= 40.0)
        GTEST_SKIP() << "uncancelled control too short to discriminate: " << controlMs << "ms";

    // Cancel an eighth of the way in: comfortably after the pre-encode poll,
    // comfortably before the level would finish. Spin to the deadline instead of
    // sleeping — sleep_for's granularity overruns a sub-100ms target.
    const double delayMs = controlMs / 8.0;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread canceller([cancel, delayMs] {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(delayMs));
        while (std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        cancel->store(true, std::memory_order_release);
    });

    bool ok = true;
    std::string error;
    const double cancelledMs = timedCook(cancel, ok, error);
    canceller.join();

    // With one level there are exactly two polls outside the band loop — before
    // the decode and at the top of the level loop — and both run within a few ms
    // of the start, far ahead of the cancel. So a cancelled result here can only
    // have come from a band poll; the timings below corroborate that and pin the
    // latency, but the discrimination does not rest on them.
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, std::string(kTextureCookCancelledError));
    // Ran until the cancel, so it was preempted mid-encode rather than at one of
    // those two early polls...
    EXPECT_GE(cancelledMs, delayMs * 0.9);
    // ...and stopped a band in, not a level in. Measured 2026-08-18 (DebugFast):
    // 1231ms against a 3648ms control, i.e. 34%; with the band poll deleted the
    // cook runs the control's full duration and returns true.
    EXPECT_LT(cancelledMs, controlMs * 0.7)
        << "cancel did not preempt the level: " << cancelledMs << "ms vs " << controlMs
        << "ms uncancelled";
    RecordProperty("controlMs", static_cast<int>(controlMs));
    RecordProperty("cancelledMs", static_cast<int>(cancelledMs));
}

// ── One texture's bands spread across workers ────────────────────────────────
//
// TextureCookWorkers hands a level's bands to pool workers; every band writes its
// own slice of the payload, so the bytes must be the one-thread encode's exactly —
// the same cache key has to name the same bytes whichever way it was cooked. The
// pools here have more workers than the decode gate's texture share, as the
// editor's do.

struct SpreadPool
{
    static constexpr size_t kPoolWorkers = 8; // a gate of six slots, four for texture work

    // Gate slots given back, each by a helper: the cooking thread here is the
    // test's own and takes none. Zero means the encode never left that thread.
    std::atomic<uint32> HelperSlotsReleased{0};
    JobSystem::WorkStealingThreadPool Pool{kPoolWorkers};
    TextureCookWorkers Workers{Pool, std::make_shared<AssetDecodeGate>(kPoolWorkers, 0,
                                                                       [this] { HelperSlotsReleased.fetch_add(1); })};
};

// Every level of a full chain, odd extents included (a partial right block column,
// a partial bottom block row, a last band shorter than the rest), at the maximum
// split (one block row per band) and at the cook's default granularity.
TEST(TextureCookParallel, SpreadEncodeMatchesOneThread)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    struct SpreadCase
    {
        TextureCookOutput Output;
        uint32 Width;
        uint32 Height;
        bool IsFloat;
        TextureCookEncodeQuality Quality;
    };
    const SpreadCase cases[] = {
        // 33 blocks per row: the default band is 15 block rows, so 126 px (32 block
        // rows) splits into 15 + 15 + 2.
        {TextureCookOutput::BC1, 130, 126, false, TextureCookEncodeQuality::Full},
        {TextureCookOutput::BC4, 130, 126, false, TextureCookEncodeQuality::Full},
        {TextureCookOutput::BC5, 66, 62, false, TextureCookEncodeQuality::Full},
        {TextureCookOutput::BC7, 66, 62, false, TextureCookEncodeQuality::Full},
        {TextureCookOutput::BC7, 258, 98, false, TextureCookEncodeQuality::QuickBC7},
        {TextureCookOutput::BC6H, 34, 30, true, TextureCookEncodeQuality::Full},
    };
    constexpr uint32 kWholeLevelBand = 1u << 24;

    SpreadPool spread;
    for (const auto& c : cases)
    {
        const uint32 levels = ComputeTextureCookMipCount(c.Width, c.Height, TextureCookSettings{});
        uint32 w = c.Width, h = c.Height;
        for (uint32 level = 0; level < levels; ++level)
        {
            const TextureCookMipLevel src = MakeLevel(w, h, c.IsFloat);
            const std::string where = "output " + std::to_string(static_cast<int>(c.Output)) + " level " +
                                      std::to_string(level) + " (" + std::to_string(w) + "x" + std::to_string(h) + ")";

            std::vector<uint8> oneThread, maxSplit, defaultSplit;
            std::string error;
            ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, kWholeLevelBand, oneThread, error, {},
                                                 c.Quality))
                << where << ": " << error;
            ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, /*bandBlocks=*/1, maxSplit, error, {},
                                                 c.Quality, &spread.Workers))
                << where << ": " << error;
            ASSERT_TRUE(CompressTextureCookLevel(src, c.IsFloat, c.Output, /*bandBlocks=*/0, defaultSplit, error, {},
                                                 c.Quality, &spread.Workers))
                << where << ": " << error;

            ASSERT_FALSE(oneThread.empty()) << where;
            EXPECT_EQ(maxSplit, oneThread) << where << ": one-block-row bands across workers differ";
            EXPECT_EQ(defaultSplit, oneThread) << where << ": default bands across workers differ";

            w = std::max(w / 2u, 1u);
            h = std::max(h / 2u, 1u);
        }
    }
    EXPECT_GT(spread.HelperSlotsReleased.load(), 0u)
        << "no helper took a gate slot: the encode never left the calling thread";
}

// Opaque gradient with a cut-out lattice: BC7 encodes the transparent blocks
// through the endpoint repair, which a spread encode runs per band too.
std::vector<uint8> CutoutTga(uint32 width, uint32 height)
{
    std::vector<uint8> rgba(static_cast<size_t>(width) * height * 4);
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            uint8* px = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
            px[0] = static_cast<uint8>((x * 255) / (width - 1));
            px[1] = static_cast<uint8>((y * 255) / (height - 1));
            px[2] = static_cast<uint8>((x * 7 + y * 13) & 0xFF);
            px[3] = ((x / 3 + y / 5) % 4 == 0) ? 0 : 255;
        }
    }
    return MakeTga(width, height, rgba);
}

// The whole cook (decode, mips, every level, the KTX2 container) spread across
// workers is the one-thread cook byte for byte, so it keys and caches the same.
TEST(TextureCookParallel, SpreadCookMatchesOneThreadCook)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    const std::vector<uint8> tga = CutoutTga(136, 78);
    struct CookCase
    {
        TextureCookInputs Inputs;
        TextureCookOutput Output;
    };
    const CookCase cases[] = {
        {MakeInputs(TextureCookUsage::Color, TextureColorSpace::SRGB), TextureCookOutput::BC7},
        {MakeInputs(TextureCookUsage::Normal, TextureColorSpace::Linear), TextureCookOutput::BC5},
        {MakeInputs(TextureCookUsage::Mask, TextureColorSpace::Linear), TextureCookOutput::BC4},
    };

    SpreadPool spread;
    for (const auto& c : cases)
    {
        SCOPED_TRACE("output " + std::to_string(static_cast<int>(c.Output)));
        std::vector<uint8> oneThread, spreadCook;
        std::string error;
        ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", c.Inputs, c.Output, oneThread, error)) << error;
        ASSERT_TRUE(CookTexture(tga.data(), tga.size(), ".tga", c.Inputs, c.Output, spreadCook, error, {}, {},
                                &spread.Workers))
            << error;
        ASSERT_FALSE(oneThread.empty());
        EXPECT_EQ(spreadCook, oneThread);
    }

    const std::vector<uint8> hdr = MakeHdr(70, 46, 2.5f, 0.75f, 0.25f);
    const TextureCookInputs hdrInputs = MakeInputs(TextureCookUsage::Color, TextureColorSpace::Linear);
    std::vector<uint8> oneThread, spreadCook;
    std::string error;
    ASSERT_TRUE(CookTexture(hdr.data(), hdr.size(), ".hdr", hdrInputs, TextureCookOutput::BC6H, oneThread, error))
        << error;
    ASSERT_TRUE(CookTexture(hdr.data(), hdr.size(), ".hdr", hdrInputs, TextureCookOutput::BC6H, spreadCook, error,
                            {}, {}, &spread.Workers))
        << error;
    EXPECT_EQ(spreadCook, oneThread);
    EXPECT_GT(spread.HelperSlotsReleased.load(), 0u)
        << "no helper took a gate slot: the encode never left the calling thread";
}

// A cancel seen by the cooking thread mid-level ends the spread encode with the
// cancel error and no bytes, after the bands already started have finished.
TEST(TextureCookParallel, CancelStopsSpreadEncode)
{
    if (!IsTextureCookEncoderAvailable())
        GTEST_SKIP() << "no BC encoder compiled in";

    const TextureCookMipLevel src = MakeLevel(256, 256, /*isFloat=*/false);
    constexpr int kPollsBeforeCancel = 3;
    int polls = 0;
    const auto cancelOnThirdPoll = [&polls] { return ++polls >= kPollsBeforeCancel; };

    SpreadPool spread;
    std::vector<uint8> payload;
    std::string error;
    EXPECT_FALSE(CompressTextureCookLevel(src, /*isFloat=*/false, TextureCookOutput::BC7, /*bandBlocks=*/1, payload,
                                          error, cancelOnThirdPoll, TextureCookEncodeQuality::QuickBC7,
                                          &spread.Workers));
    EXPECT_EQ(error, std::string(kTextureCookCancelledError));
    EXPECT_TRUE(payload.empty());
    EXPECT_EQ(polls, kPollsBeforeCancel) << "the encode must stop polling once it has been cancelled";
}

#endif // GE_HAVE_KTX

} // namespace
