// Tests for the decode tier of the texture alpha probe (ProbeTextureMinAlpha*).
// The safety property: a minimum may only be reported when it is EXACT over
// every texel AND the container's cooked delta is bounded, so anything
// oversized, unrecognized, malformed, or cooked through an unbounded encode
// must return nullopt and leave the caller on the authored Mask. Fixtures are
// generated in-memory — no binary blobs.

#include <gtest/gtest.h>

#include "Assets/AlphaCutoffThreshold.h"
#include "Assets/TextureAlphaDecodeProbe.h"

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
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

uint32 Crc32(const uint8* data, size_t size)
{
    static std::array<uint32, 256> table{};
    static bool built = false;
    if (!built)
    {
        for (uint32 i = 0; i < 256; ++i)
        {
            uint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        crc = table[static_cast<uint8>(crc ^ data[i])] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32 Adler32(const uint8* data, size_t size)
{
    uint32 a = 1, b = 0;
    for (size_t i = 0; i < size; ++i)
    {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PushChunk(std::vector<uint8>& v, const char* type, const std::vector<uint8>& payload)
{
    PushU32BE(v, static_cast<uint32>(payload.size()));
    std::vector<uint8> typed(reinterpret_cast<const uint8*>(type),
                             reinterpret_cast<const uint8*>(type) + 4);
    typed.insert(typed.end(), payload.begin(), payload.end());
    v.insert(v.end(), typed.begin(), typed.end());
    PushU32BE(v, Crc32(typed.data(), typed.size()));
}

/// zlib stream wrapping `raw` in stored (uncompressed) deflate blocks — a fully
/// valid stream without pulling in a compressor.
std::vector<uint8> ZlibStored(const std::vector<uint8>& raw)
{
    std::vector<uint8> out = {0x78, 0x01};
    constexpr size_t kMaxBlock = 65535;
    size_t offset = 0;
    do
    {
        const size_t n = std::min(kMaxBlock, raw.size() - offset);
        const bool last = offset + n >= raw.size();
        out.push_back(last ? 0x01 : 0x00);
        out.push_back(static_cast<uint8>(n & 0xFF));
        out.push_back(static_cast<uint8>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8>(~n & 0xFF));
        out.push_back(static_cast<uint8>((~n >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + n);
        offset += n;
    } while (offset < raw.size());
    const uint32 adler = Adler32(raw.data(), raw.size());
    PushU32BE(out, adler);
    return out;
}

/// RGBA (colorType 6) or RGB (colorType 2) PNG whose alpha channel takes
/// `alphas` in row-major order (ignored for RGB).
std::vector<uint8> MakePng(uint32 w, uint32 h, uint8 colorType, const std::vector<uint8>& alphas)
{
    const uint32 channels = colorType == 6 ? 4u : 3u;
    std::vector<uint8> raw;
    for (uint32 y = 0; y < h; ++y)
    {
        raw.push_back(0); // filter: none
        for (uint32 x = 0; x < w; ++x)
        {
            raw.push_back(0x40); // r
            raw.push_back(0x80); // g
            raw.push_back(0xC0); // b
            if (channels == 4)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                raw.push_back(i < alphas.size() ? alphas[i] : 255);
            }
        }
    }

    std::vector<uint8> v = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8> ihdr;
    PushU32BE(ihdr, w);
    PushU32BE(ihdr, h);
    ihdr.push_back(8);         // bit depth
    ihdr.push_back(colorType);
    ihdr.push_back(0);         // compression
    ihdr.push_back(0);         // filter
    ihdr.push_back(0);         // interlace
    PushChunk(v, "IHDR", ihdr);
    PushChunk(v, "IDAT", ZlibStored(raw));
    PushChunk(v, "IEND", {});
    return v;
}

/// Uncompressed 32-bit BGRA TGA whose alpha channel takes `alphas`.
std::vector<uint8> MakeTga32(uint32 w, uint32 h, const std::vector<uint8>& alphas)
{
    std::vector<uint8> v(18, 0);
    v[2] = 2;  // uncompressed truecolor
    v[12] = static_cast<uint8>(w & 0xFF);
    v[13] = static_cast<uint8>((w >> 8) & 0xFF);
    v[14] = static_cast<uint8>(h & 0xFF);
    v[15] = static_cast<uint8>((h >> 8) & 0xFF);
    v[16] = 32; // bpp
    v[17] = 8;  // 8 alpha bits
    for (uint32 i = 0; i < w * h; ++i)
    {
        v.push_back(0xC0); // b
        v.push_back(0x80); // g
        v.push_back(0x40); // r
        v.push_back(i < alphas.size() ? alphas[i] : 255);
    }
    return v;
}

/// The minimum when the probe answered, nullopt for Unprobeable. Deferred is
/// a separate axis and is asserted explicitly where it matters — collapsing it
/// into nullopt here would hide the very distinction the status exists for.
std::optional<uint8> Probe(const std::vector<uint8>& v)
{
    const TextureAlphaProbeResult r = ProbeTextureMinAlphaFromBytes(v.data(), v.size());
    if (r.Status == TextureAlphaProbeStatus::Answered)
        return r.MinAlpha;
    return std::nullopt;
}

TextureAlphaProbeStatus ProbeStatus(const std::vector<uint8>& v)
{
    return ProbeTextureMinAlphaFromBytes(v.data(), v.size()).Status;
}

} // namespace

// ---- Sources whose cooked delta is not bounded ----
//
// PNG and TGA decode perfectly well; the tier refuses them anyway because the
// import cook re-encodes them to BC7 and that delta is not a function of their
// alpha (measured: 29 steps on ordinary content, 96 on adversarial content,
// including sources whose alpha was constant).
// These are contract tests, not decoder tests: they must fail if someone
// re-enables the stb path without re-measuring the cook.

TEST(TextureAlphaDecodeProbe, FullyOpaquePngIsRefusedBecauseItsCookIsUnbounded)
{
    EXPECT_FALSE(Probe(MakePng(2, 2, 6, {255, 255, 255, 255})).has_value());
}

TEST(TextureAlphaDecodeProbe, PngWithoutAlphaChannelIsAlsoRefusedHere)
{
    // The header tier (tier 1) answers this one margin-free; the decode tier
    // must not duplicate that verdict through an unbounded cook.
    EXPECT_FALSE(Probe(MakePng(2, 2, 2, {})).has_value());
}

TEST(TextureAlphaDecodeProbe, FullyOpaqueTgaIsRefusedBecauseItsCookIsUnbounded)
{
    EXPECT_FALSE(Probe(MakeTga32(2, 2, {255, 255, 255, 255})).has_value());
}

// ---- Format gating ----

TEST(TextureAlphaDecodeProbe, UnrecognizedBytesAreUnknown)
{
    const std::vector<uint8> garbage(64, 0x5A);
    EXPECT_FALSE(Probe(garbage).has_value());
}

TEST(TextureAlphaDecodeProbe, EmptyAndNullInputsAreUnknown)
{
    EXPECT_EQ(ProbeTextureMinAlphaFromBytes(nullptr, 0).Status,
              TextureAlphaProbeStatus::Unprobeable);
    const std::vector<uint8> empty;
    EXPECT_EQ(ProbeTextureMinAlphaFromBytes(empty.data(), 0).Status,
              TextureAlphaProbeStatus::Unprobeable);
}

TEST(TextureAlphaDecodeProbe, MissingFileIsUnknown)
{
    EXPECT_EQ(ProbeTextureMinAlphaFromFile("does-not-exist-alpha-probe.png").Status,
              TextureAlphaProbeStatus::Unprobeable);
}


// ---- Cutoff threshold arithmetic ----

TEST(AlphaCutoffThreshold, NonFiniteCutoffsAreRejectedRatherThanCastToZero)
{
    // NaN passes every ordered guard, and casting it to an integer is UB that
    // lands on 0 with this toolchain — the LOWEST threshold, which would demote
    // nearly everything. The failure direction must be "refuse".
    EXPECT_FALSE(AlphaCutoffOpaqueThreshold(std::numeric_limits<float>::quiet_NaN(), 8).has_value());
    EXPECT_FALSE(AlphaCutoffOpaqueThreshold(std::numeric_limits<float>::infinity(), 8).has_value());
    EXPECT_FALSE(
        AlphaCutoffOpaqueThreshold(-std::numeric_limits<float>::infinity(), 8).has_value());
}

TEST(AlphaCutoffThreshold, CutoffsThatLeaveNoRoomForTheMarginAreRefusedNotClamped)
{
    // ceil(0.99 * 255) = 253, so an 8-step margin does not fit under 255.
    // Clamping the threshold to 255 (the previous behaviour) silently reduced
    // the margin to 2 while still claiming 8.
    EXPECT_FALSE(AlphaCutoffOpaqueThreshold(0.99f, 8).has_value());
    EXPECT_FALSE(AlphaCutoffOpaqueThreshold(1.0f, 1).has_value());
    // With no margin demanded, a cutoff of 1.0 is exactly representable.
    EXPECT_EQ(AlphaCutoffOpaqueThreshold(1.0f, 0), std::optional<uint32>(255));
}

TEST(AlphaCutoffThreshold, ThresholdIsTheCutoffCeilingPlusTheMargin)
{
    // Pins the composed value, so dropping the margin term is not a silent
    // no-op: at the default cutoff the gate demands 136, not 128.
    EXPECT_EQ(AlphaCutoffOpaqueThreshold(0.5f, 0), std::optional<uint32>(128));
    EXPECT_EQ(AlphaCutoffOpaqueThreshold(0.5f, kCookedAlphaDropBound), std::optional<uint32>(136));
    EXPECT_EQ(AlphaCutoffOpaqueThreshold(0.0f, 0), std::optional<uint32>(0));
    // Out-of-range cutoffs clamp into [0,1] rather than producing nonsense.
    EXPECT_EQ(AlphaCutoffOpaqueThreshold(-5.0f, 0), std::optional<uint32>(0));
}

TEST(AlphaCutoffThreshold, TheMarginIsNonZero)
{
    // A zero margin would make every "cleared the cutoff" claim a claim about
    // the SOURCE only, which is not what the GPU samples.
    EXPECT_GT(kCookedAlphaDropBound, 0u);
}

// ---- KTX2 ----
//
// libktx builds every fixture so the container under test is a real one.

#if defined(GE_HAVE_KTX)
namespace {

struct Ktx2Spec
{
    uint32 Width = 4;
    uint32 Height = 4;
    uint32 Levels = 1;
    uint32 Layers = 1;
    uint32 Faces = 1;
    uint32 Dimensions = 2;
    uint32 Depth = 1;
    bool Array = false;
    /// Alpha written to every texel, keyed by [level][layer][face]; the first
    /// entry is reused wherever the vector runs out.
    std::vector<uint8> AlphaPerImage{255};
    bool Zstd = false;
    bool Uastc = false;
    bool Etc1s = false;
};

std::vector<uint8> MakeKtx2(const Ktx2Spec& spec)
{
    ktxTextureCreateInfo info{};
    info.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    info.baseWidth = spec.Width;
    info.baseHeight = spec.Height;
    info.baseDepth = spec.Depth;
    info.numDimensions = spec.Dimensions;
    info.numLevels = spec.Levels;
    info.numLayers = spec.Layers;
    info.numFaces = spec.Faces;
    info.isArray = spec.Array ? KTX_TRUE : KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS)
        return {};

    size_t imageIndex = 0;
    for (uint32 level = 0; level < spec.Levels; ++level)
    {
        const uint32 lw = std::max(1u, spec.Width >> level);
        const uint32 lh = std::max(1u, spec.Height >> level);
        const uint32 ld = std::max(1u, spec.Depth >> level);
        std::vector<uint8> texels(static_cast<size_t>(lw) * lh * ld * 4, 0xFF);
        for (uint32 layer = 0; layer < spec.Layers; ++layer)
        {
            for (uint32 face = 0; face < spec.Faces; ++face)
            {
                const uint8 alpha = spec.AlphaPerImage[std::min(
                    imageIndex, spec.AlphaPerImage.size() - 1)];
                for (size_t i = 3; i < texels.size(); i += 4)
                    texels[i] = alpha;
                ktxTexture_SetImageFromMemory(ktxTexture(texture), level, layer, face,
                                              texels.data(), texels.size());
                ++imageIndex;
            }
        }
    }

    if (spec.Uastc || spec.Etc1s)
    {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = spec.Uastc ? KTX_TRUE : KTX_FALSE;
        params.threadCount = 1;
        const KTX_error_code err = ktxTexture2_CompressBasisEx(texture, &params);
        if (err != KTX_SUCCESS)
        {
            ADD_FAILURE() << "ktxTexture2_CompressBasisEx: " << ktxErrorString(err);
            ktxTexture2_Destroy(texture);
            return {};
        }
    }
    else if (spec.Zstd)
    {
        // Uniform payloads deflate to almost nothing, which is exactly how a
        // few-KB file can declare hundreds of MiB of images.
        const KTX_error_code err = ktxTexture2_DeflateZstd(texture, 1);
        if (err != KTX_SUCCESS)
        {
            ADD_FAILURE() << "ktxTexture2_DeflateZstd: " << ktxErrorString(err);
            ktxTexture2_Destroy(texture);
            return {};
        }
    }

    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    std::vector<uint8> out;
    if (ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size) == KTX_SUCCESS && bytes)
    {
        out.assign(bytes, bytes + size);
        free(bytes);
    }
    ktxTexture2_Destroy(texture);
    return out;
}

} // namespace

TEST(TextureAlphaDecodeProbe, Ktx2Rgba8FullyOpaqueReportsMax)
{
    const std::vector<uint8> v = MakeKtx2({.AlphaPerImage = {255}});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(255));
}

TEST(TextureAlphaDecodeProbe, Ktx2Rgba8ReportsTheExactMinimum)
{
    const std::vector<uint8> v = MakeKtx2({.AlphaPerImage = {90}});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(90));
}

TEST(TextureAlphaDecodeProbe, Ktx2ScansEveryMipLevel)
{
    // Level 0 is opaque and a coarser level is not. Levels are compressed
    // independently in real containers, so a level-0-only scan would report a
    // minimum the GPU can undercut at minification.
    const std::vector<uint8> v = MakeKtx2({.Levels = 3, .AlphaPerImage = {255, 255, 17}});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(17));
}

TEST(TextureAlphaDecodeProbe, Ktx2ScansEveryArrayLayer)
{
    // Kills the "iterate layer 0 only" mutant: the transparent texel lives in
    // the last layer, and a layer-0 scan would call the whole array opaque.
    const std::vector<uint8> v =
        MakeKtx2({.Layers = 4, .Array = true, .AlphaPerImage = {255, 255, 255, 42}});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(42));
}

TEST(TextureAlphaDecodeProbe, Ktx2ScansEveryCubeFace)
{
    // Kills the "iterate face 0 only" mutant, on the container shape that
    // motivated the allocation cap in the first place.
    const std::vector<uint8> v =
        MakeKtx2({.Faces = 6, .AlphaPerImage = {255, 255, 255, 255, 255, 7}});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(7));
}

TEST(TextureAlphaDecodeProbe, Ktx2ThreeDimensionalContainerIsRefused)
{
    // Kills the "drop the numDimensions == 2 guard" mutant. GetImageOffset
    // addresses levels/layers/faces but not depth slices, so a 3D container
    // would silently leave every slice past the first unscanned — and the one
    // texel skipped may be the transparent one.
    const std::vector<uint8> v =
        MakeKtx2({.Width = 4, .Height = 4, .Dimensions = 3, .Depth = 4, .AlphaPerImage = {0}});
    ASSERT_FALSE(v.empty());
    EXPECT_FALSE(Probe(v).has_value());
}

TEST(TextureAlphaDecodeProbe, Ktx2LayerCountCountsTowardTheTexelCap)
{
    // Kills the "cap only checks baseWidth * baseHeight" mutant, and pins the
    // fix for the allocation bug behind it: the cap must be enforced from the
    // HEADER, before image data is loaded. This fixture is a few KB on the
    // wire and declares ~28.7M texels — over the cap — so a probe that loaded
    // first would allocate ~115 MiB to decide it should not have.
    const std::vector<uint8> v = MakeKtx2({.Width = 64,
                                           .Height = 64,
                                           .Layers = 7000,
                                           .Array = true,
                                           .AlphaPerImage = {0},
                                           .Zstd = true});
    ASSERT_TRUE(64ull * 64ull * 7000ull > kMaxProbeTexels) << "fixture no longer exceeds the cap";
    ASSERT_FALSE(v.empty());
    EXPECT_LT(v.size(), kMaxProbeSourceBytes)
        << "fixture must be refused by the TEXEL cap, not the source-size cap";
    EXPECT_FALSE(Probe(v).has_value());
}

TEST(TextureAlphaDecodeProbe, Ktx2FaceCountCountsTowardTheTexelCap)
{
    // Same cap, via faces: a 2048x2048 two-level cubemap is comfortably under
    // the cap per face and over it across all six.
    const std::vector<uint8> v = MakeKtx2({.Width = 2048,
                                           .Height = 2048,
                                           .Levels = 2,
                                           .Faces = 6,
                                           .AlphaPerImage = {0},
                                           .Zstd = true});
    ASSERT_FALSE(v.empty());
    EXPECT_LT(v.size(), kMaxProbeSourceBytes)
        << "fixture must be refused by the TEXEL cap, not the source-size cap";
    EXPECT_FALSE(Probe(v).has_value());
}

TEST(TextureAlphaDecodeProbe, Ktx2MippedAtlasesTheSizeRealContentShipsAreStillAccepted)
{
    // The control that matters most for the cap, and the one whose absence let
    // a wrong cap formula through: counting the mip chain as levels x base
    // (instead of summing the halving chain) overstates these by 9-10x and
    // refuses them. Every shipped 2048^2 and 4096^2 atlas in the corpus has a
    // full mip chain, so that cap would have retained NONE of the content this
    // tier exists for — while every oversize test above still passed.
    for (const uint32 side : {2048u, 4096u})
    {
        uint32 levels = 1;
        for (uint32 s = side; s > 1; s /= 2)
            ++levels;
        const std::vector<uint8> v = MakeKtx2({.Width = side,
                                               .Height = side,
                                               .Levels = levels,
                                               .AlphaPerImage = {211},
                                               .Zstd = true});
        ASSERT_FALSE(v.empty()) << "side " << side;
        EXPECT_EQ(Probe(v), std::optional<uint8>(211))
            << side << "x" << side << " with " << levels << " mip levels must be probed, not "
            << "refused by the texel cap";
    }
}

TEST(TextureAlphaDecodeProbe, Ktx2WithinTheTexelCapIsStillAccepted)
{
    // Control for the two cap tests above: the cap must refuse oversized
    // containers without refusing everything.
    const std::vector<uint8> v = MakeKtx2({.Width = 1024,
                                           .Height = 1024,
                                           .Layers = 4,
                                           .Array = true,
                                           .AlphaPerImage = {255, 255, 255, 33},
                                           .Zstd = true});
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(Probe(v), std::optional<uint8>(33));
}

TEST(TextureAlphaDecodeProbe, Ktx2UastcOpaqueSourceIsProbedThroughTheTranscode)
{
    // The Basis/UASTC branch is what real content actually exercises (every
    // one of the 1742 shipped .ktx2 files is UASTC), and it had no test at all.
    // A uniformly opaque UASTC payload must come back opaque through the
    // RGBA32 transcode.
    const std::vector<uint8> v =
        MakeKtx2({.Width = 32, .Height = 32, .AlphaPerImage = {255}, .Uastc = true});
    ASSERT_FALSE(v.empty());
    const auto probed = Probe(v);
    ASSERT_TRUE(probed.has_value()) << "UASTC container must be decodable";
    EXPECT_GE(static_cast<uint32>(*probed), 255u - kCookedAlphaDropBound);
}

TEST(TextureAlphaDecodeProbe, Ktx2UastcTransparentSourceIsNotReportedOpaque)
{
    // The direction that matters: a UASTC payload carrying transparency must
    // never come back as a demotable minimum.
    const std::vector<uint8> v =
        MakeKtx2({.Width = 32, .Height = 32, .AlphaPerImage = {0}, .Uastc = true});
    ASSERT_FALSE(v.empty());
    const auto probed = Probe(v);
    ASSERT_TRUE(probed.has_value());
    EXPECT_LT(static_cast<uint32>(*probed), 128u);
}

TEST(TextureAlphaDecodeProbe, Ktx2Etc1sIsRefusedEvenThoughItWouldTranscode)
{
    // libktx transcodes ETC1S perfectly happily, so nothing stops the probe
    // from answering for it — except that ETC1S carries alpha as a separate,
    // much lossier slice, and kCookedAlphaDropBound was measured on UASTC only
    // (1742 UASTC files, zero ETC1S). Answering here would be applying a bound
    // to a population it was never measured on.
    const std::vector<uint8> etc1s =
        MakeKtx2({.Width = 32, .Height = 32, .AlphaPerImage = {255}, .Etc1s = true});
    ASSERT_FALSE(etc1s.empty());
    EXPECT_FALSE(Probe(etc1s).has_value());

    // Control: the identical image as UASTC IS answered, so the refusal above
    // is about the Basis flavour and not about the fixture being undecodable.
    const std::vector<uint8> uastc =
        MakeKtx2({.Width = 32, .Height = 32, .AlphaPerImage = {255}, .Uastc = true});
    ASSERT_FALSE(uastc.empty());
    EXPECT_TRUE(Probe(uastc).has_value());
}

TEST(TextureAlphaDecodeProbe, BlobsOverTheSourceCapAreRefusedEvenWhenStillDecodable)
{
    // kMaxProbeSourceBytes bounds the read and the decoder's own allocation and
    // had NO coverage: a mutant deleting the check passed every test. A blob of
    // garbage would not discriminate -- it is refused by the format gate too.
    // So this pads a VALID container past the cap with trailing bytes libktx
    // ignores: without the size gate it still decodes and answers, so the test
    // fails for exactly the right reason.
    const std::vector<uint8> valid = MakeKtx2({.AlphaPerImage = {77}});
    ASSERT_FALSE(valid.empty());
    ASSERT_EQ(Probe(valid), std::optional<uint8>(77)) << "control: unpadded container answers";

    std::vector<uint8> padded = valid;
    padded.resize(kMaxProbeSourceBytes + 1, 0);
    EXPECT_EQ(ProbeStatus(padded), TextureAlphaProbeStatus::Unprobeable);
}

TEST(TextureAlphaDecodeProbe, TruncatedKtx2IsUnknown)
{
    std::vector<uint8> v = MakeKtx2({.AlphaPerImage = {255}});
    ASSERT_FALSE(v.empty());
    v.resize(v.size() / 2);
    EXPECT_FALSE(Probe(v).has_value());
}

TEST(TextureAlphaDecodeProbe, ConcurrentProbesRespectTheCap)
{
    // The probe is reached from JobSystem workers, and each decode is a
    // three-figure-millisecond, hundred-MiB event. Prove the admission cap
    // actually holds under load rather than assuming it.
    const std::vector<uint8> v = MakeKtx2({.Width = 256, .Height = 256, .AlphaPerImage = {200}});
    ASSERT_FALSE(v.empty());

    // Peak is process-global, monotonic, AND saturates at the cap, so ~20
    // earlier tests in this binary have already pushed it to its ceiling.
    // Asserting `Peak() > 0` would stay green even if this section never ran,
    // and asserting it INCREASED cannot hold once it is saturated. The control
    // has to be its own observation window.
    ResetPeakConcurrentProbeDecodes();
    ASSERT_EQ(PeakConcurrentProbeDecodes(), 0u) << "window must start closed";

    std::vector<std::thread> threads;
    std::atomic<int> answered{0};
    std::atomic<int> deferred{0};
    std::atomic<int> wrong{0};
    for (int i = 0; i < 16; ++i)
    {
        threads.emplace_back([&] {
            for (int n = 0; n < 8; ++n)
            {
                const TextureAlphaProbeResult r =
                    ProbeTextureMinAlphaFromBytes(v.data(), v.size());
                switch (r.Status)
                {
                case TextureAlphaProbeStatus::Answered:
                    r.MinAlpha == 200 ? ++answered : ++wrong;
                    break;
                case TextureAlphaProbeStatus::Deferred: ++deferred; break;
                case TextureAlphaProbeStatus::Unprobeable: ++wrong; break;
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    // The cap never blocks, so under contention some probes legitimately come
    // back Deferred — that is the design, not a failure. What must hold: every
    // non-deferred probe is exactly right, none is Unprobeable, and progress is
    // still made rather than everything deferring.
    EXPECT_EQ(wrong.load(), 0) << "a probe that answered must answer exactly";
    EXPECT_EQ(answered.load() + deferred.load(), 16 * 8);
    EXPECT_GT(answered.load(), 0) << "contention must not starve every probe";
    EXPECT_LE(PeakConcurrentProbeDecodes(), kDefaultMaxConcurrentProbeDecodes);
    EXPECT_GT(PeakConcurrentProbeDecodes(), 0u)
        << "peak was reset before this section, so a zero here means the probes "
           "never actually ran and every other assertion above is vacuous";
    EXPECT_GT(PeakConcurrentProbeDecodes(), 0u) << "the counter must actually be observing";
}
#endif // GE_HAVE_KTX
