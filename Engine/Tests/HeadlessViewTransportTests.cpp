// I4 — transport byte equality. The gate for "every output pixel is a copy of
// exactly one source texel", which PixelPerfectUpscalePass has published since
// it was written and which no test in the tree has ever checked a pixel of.
//
// WHAT BREAKS TODAY WITHOUT ANY OF THIS FIRING: make the pass's sampler linear
// instead of nearest, or let its encoded arm regain a transfer function, and
// every pixel-art viewport smears or double-encodes. The only existing test
// over the pass (RenderPipelineDeclareTests.PixelPerfectUpscaleTwinDeclares-
// AndGuards) reads back no pixels.
//
// ── HOW THIS AVOIDS BEING A GATE THAT CANNOT FAIL ───────────────────────────
// There is no golden. The expectation for every mapped destination pixel is the
// SAME frame's own pre-transport readback, addressed through an index map built
// from the desc's geometry by BuildTransportIndexMap — which never reads the
// pass, its shader or its push constants. So a mutation of the pass's origin or
// window math moves the render and leaves the expectation standing, and a
// mutation of the sampler moves the pixel values and leaves both standing.
//
// The comparison is in stored CODES (DestinationCodes), never decoded floats,
// with zero tolerance: chain rule 5 rejects a band, and a 1-LSB smear is
// exactly what a convenient band hides.
//
// ── THE ARM SHAPE RULE, AND WHY IT IS ASSERTED RATHER THAN COMMENTED ────────
// Zoom 1 with frac 0 places every sample on an exact texel CENTRE, where a
// bilinear sampler returns precisely what a nearest one does — such an arm is
// blind to the mutation this file exists to catch, and would pass it. So every
// arm runs zoom >= 2 with samples kept clear of both texel boundaries (>= 1/32
// texel, the rule the spec sets: Vulkan guarantees only 8 fractional bits of
// texel-address precision, so 1/256 is the floor and 1/32 is eight times it)
// and texel centres. TransportGeometry measures both minima over the arm's real
// samples and the instrument helper asserts them.
//
// Skip policy (chain contract rule 6, three classes):
//   absent device            -> FAIL
//   absent staged asset      -> FAIL (an undeclared transport pass is this class)
//   absent format capability -> SKIP, naming the format and the usage bits.

#include "HeadlessViewFixture.h"
#include "RGPassQuery.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
namespace RGQuery = GameEngine::Testing::RGQuery;
using namespace GameEngine::Testing;
using GameEngine::Rendering::TextureFormat;
namespace Passes = GameEngine::Rendering::Passes;

namespace
{

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// The dither and deband kill switches latch into function-local statics on
// first read, so they are a property of the PROCESS. This TU pins them itself
// rather than relying on the sibling TU's static: cross-TU initialization order
// is unspecified, and a gate whose instrument depends on which translation unit
// happened to construct first is not a gate. Both TUs write the same values, so
// either order leaves the process in the state both describe.
//
// GE_DEBAND=0 is load-bearing HERE for a different reason than in the sibling:
// the deband is a neighbourhood filter, and a filter running inside the finalize
// would SMOOTH the transport's source — which is the one property the
// adjacent-texels-differ instrument needs the source not to have.
struct PinEnvironment
{
    PinEnvironment()
    {
        SetEnv("GE_HEADLESS_TEST", "1");
        SetEnv("GE_OUTPUT_DITHER", "1");
        SetEnv("GE_DEBAND", "0");
        SetEnv("GE_DEBAND_THRESHOLD", "");
        SetEnv("GE_DEBAND_RADIUS", "");
    }
} g_pinEnvironment;

// The view, and therefore the transport's source. The pass's contract pads its
// source by one texel per side, so this is the reference window plus 2.
constexpr uint32_t kViewExtent = 66;
constexpr uint32_t kReferenceExtent = kViewExtent - 2;

// Surplus destination beyond the upscaled rect, so there are letterbox bars on
// all four sides and the letterbox assertion has a subject. Asymmetric so a
// transposed origin does not accidentally land in the right place.
constexpr uint32_t kLetterboxSurplusX = 16;
constexpr uint32_t kLetterboxSurplusY = 8;

// How much of the mapped source must be locally non-constant. Adjacent texels
// carry independent 6-bit levels per channel, so a colliding pair needs all
// three channels to collide — about 4 in a million. The floor is set orders of
// magnitude below that so it reports the instrument's health rather than
// tracking it.
constexpr double kMinAdjacentDifferingFraction = 0.90;

// Distances the arm's samples must keep, in source texels.
constexpr float kMinTexelBoundaryDistance = 1.0f / 32.0f;
// A sample AT a texel centre is one where bilinear and nearest agree, so it
// cannot witness a filtering sampler. Same mutation as the byte gate's
// (nearest -> linear); this is that gate's precondition, not a second pin.
constexpr float kMinTexelCentreDistance = 1.0f / 64.0f;

// Diagnostic budget. Chain rule 1: a gate must be able to produce the pixels it
// disagreed about, because a bare "they differ" is not diagnosable.
constexpr std::size_t kMaxReportedMismatches = 8;

float SrgbToLinearContent(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

uint32_t ContentHash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 0x9E3779B1u ^ (y + 0x85EBCA6Bu) * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    h ^= h >> 16;
    return h;
}

// Content whose ADJACENT TEXELS DIFFER, which the transport gate needs and the
// sibling suite's smooth ramp would not supply: a point sampler and a filtering
// one produce the same answer wherever a texel's neighbours equal it, so a
// smooth source is a source on which the mutation is invisible.
//
// Levels are placed in ENCODED space and inverted into the linear the fixture
// authors, so the spacing survives the finalize's OETF uniformly instead of
// collapsing at the top of the range: 64 levels across 0.12..0.88 encoded is a
// step of about 3 eight-bit codes, which the +/-1 LSB dither cannot close.
std::vector<float> MakeSharpContent(uint32_t width, uint32_t height)
{
    std::vector<float> px(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t h = ContentHash(x, y);
            const size_t base = (static_cast<size_t>(y) * width + x) * 4;
            for (int ch = 0; ch < 3; ++ch)
            {
                const uint32_t level = (h >> (ch * 8)) & 63u;
                const float encoded = 0.12f + (static_cast<float>(level) / 63.0f) * 0.76f;
                px[base + static_cast<size_t>(ch)] = SrgbToLinearContent(encoded);
            }
            px[base + 3] = 1.0f;
        }
    }
    return px;
}

HeadlessViewDesc TransportDescFor(FinalizeDriver driver, TextureFormat finalizeFormat,
                                  TextureFormat transportFormat, uint32_t zoom, float fracX,
                                  float fracY)
{
    HeadlessViewDesc d{};
    d.Width = kViewExtent;
    d.Height = kViewExtent;
    d.ClearLinear[0] = 0.0f;
    d.ClearLinear[1] = 0.0f;
    d.ClearLinear[2] = 0.0f;
    d.ClearLinear[3] = 1.0f;
    d.ContentLinearRgba = MakeSharpContent(kViewExtent, kViewExtent);
    d.Driver = driver;
    d.DestinationFormat = finalizeFormat;
    d.Quantizer = Passes::FinalizeQuantizer::Destination;

    TransportDesc t{};
    t.DestinationWidth = kReferenceExtent * zoom + kLetterboxSurplusX;
    t.DestinationHeight = kReferenceExtent * zoom + kLetterboxSurplusY;
    t.DestinationFormat = transportFormat;
    t.Zoom = zoom;
    t.FracX = fracX;
    t.FracY = fracY;
    // The finalize already applied the OETF, so the transport must apply none:
    // this is the arm the whole byte claim lives on.
    t.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb;
    d.Transport = t;
    return d;
}

#define HV_REQUIRE_UP(fx)                                                                         \
    do                                                                                            \
    {                                                                                             \
        const HeadlessViewStatus st = (fx).Up();                                                  \
        ASSERT_NE(st, HeadlessViewStatus::NoDevice)                                               \
            << "device absent is a FAILURE for this suite, not a skip: " << (fx).StatusMessage(); \
        ASSERT_NE(st, HeadlessViewStatus::StagedAssetMissing)                                     \
            << "staging is part of the test: " << (fx).StatusMessage();                           \
        ASSERT_EQ(st, HeadlessViewStatus::Ok) << (fx).StatusMessage();                            \
    } while (false)

#define HV_RENDER_OR_SKIP(fx, desc)                                                            \
    do                                                                                         \
    {                                                                                          \
        const HeadlessViewStatus st = (fx).Render(desc);                                       \
        if (st == HeadlessViewStatus::FormatUnsupported)                                       \
            GTEST_SKIP() << "capability skip: " << (fx).StatusMessage();                       \
        ASSERT_EQ(st, HeadlessViewStatus::Ok) << ToString(st) << ": " << (fx).StatusMessage(); \
    } while (false)

// ── The instrument. Every arm runs it, because each of these is a route by
// which a byte-equality assertion passes while measuring nothing: a transport
// that never ran, a source whose neighbours are equal (a filtering sampler
// reproduces a point sampler exactly there), or a sample geometry that lands on
// texel centres (where the two are the same function).
void ExpectTransportInstrument(const HeadlessViewFixture& fx, const char* arm)
{
    const TransportGeometry& g = fx.TransportShape();
    const ViewOutputBytes& src = fx.Handoff();

    // 1. The pass ran, and after the finalize. Scheduled order, not declaration
    //    order: a culled pass still appears in the declaration list.
    // Two finalize arms, one property: the direct arm declares
    // "HeadlessView.Finalize", the host policy "HeadlessView.Finalized";
    // exactly one is scheduled in any run.
    std::optional<std::size_t> finalizeIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalize"});
    if (!finalizeIdx)
        finalizeIdx =
            RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalized"});
    const std::optional<std::size_t> transportIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Transport"});
    ASSERT_TRUE(transportIdx.has_value())
        << arm << ": no scheduled pass named Transport — the upscale was culled or never "
                  "declared, so every byte compared below came from somewhere else";
    ASSERT_TRUE(finalizeIdx.has_value())
        << arm << ": no scheduled finalize pass, so the transport's source is not a finalized "
                  "image";
    EXPECT_LT(*finalizeIdx, *transportIdx)
        << arm << ": the transport is scheduled at " << *transportIdx << " and the finalize at "
        << *finalizeIdx << " — it sampled the finalize's destination before the finalize wrote it";

    // 2. The map has both populations. No mapped pixels and the byte assertion
    //    has no subject; no letterbox pixels and the letterbox assertion has
    //    none, which is also the state a destination the same size as its
    //    source would be in.
    EXPECT_GT(g.MappedPixels, 0u) << arm << ": the index map maps no destination pixel at all";
    EXPECT_GT(g.LetterboxPixels, 0u)
        << arm << ": the destination has no letterbox bars, so the centering and the "
                  "dst-aliases-src class both go unasserted";
    EXPECT_EQ(g.ClampedSamples, 0u)
        << arm << ": " << g.ClampedSamples
        << " samples needed the sampler's clamp-to-edge — the window has walked off the padded "
           "border and 'a copy of exactly one source texel' would be satisfied by the clamp";

    // 3. The sample geometry can witness a filtering sampler.
    EXPECT_GE(g.MinDistanceToTexelBoundary, kMinTexelBoundaryDistance)
        << arm << ": a sample sits " << g.MinDistanceToTexelBoundary
        << " texels from an integer boundary, inside the 1/32 rule — Vulkan guarantees only 8 "
           "fractional bits of texel-address precision, so which texel the nearest snap picks "
           "stops being predictable and the index map stops being a prediction";
    EXPECT_GE(g.MinDistanceToTexelCentre, kMinTexelCentreDistance)
        << arm << ": a sample sits " << g.MinDistanceToTexelCentre
        << " texels from a texel CENTRE, where a bilinear sampler returns exactly what a nearest "
           "one does — this arm cannot see the sampler mutation it exists to catch";

    // 4. The source the transport sampled is sharp. A filtering sampler is
    //    indistinguishable from a point sampler wherever neighbours are equal,
    //    so this is measured over the texels the map ACTUALLY READS rather than
    //    over the whole image.
    ASSERT_GT(src.Width, 0u);
    ASSERT_GT(src.Height, 0u);
    std::vector<uint8_t> used(static_cast<size_t>(src.Width) * src.Height, 0u);
    for (const TransportIndex& e : fx.TransportIndexMap())
        if (!e.Letterbox)
            used[static_cast<size_t>(e.SourceY) * src.Width + e.SourceX] = 1u;

    size_t pairs = 0;
    size_t differing = 0;
    uint32_t lo[4] = {~0u, ~0u, ~0u, ~0u};
    uint32_t hi[4] = {0u, 0u, 0u, 0u};
    for (uint32_t y = 0; y < src.Height; ++y)
    {
        for (uint32_t x = 0; x < src.Width; ++x)
        {
            const size_t here = static_cast<size_t>(y) * src.Width + x;
            if (!used[here])
                continue;
            uint32_t a[4] = {};
            ASSERT_TRUE(DestinationCodes(src, here, a))
                << arm << ": the transport source format is not one this fixture unpacks";
            for (int ch = 0; ch < 4; ++ch)
            {
                lo[ch] = std::min(lo[ch], a[ch]);
                hi[ch] = std::max(hi[ch], a[ch]);
            }
            const size_t neighbours[2] = {here + 1, here + src.Width};
            const bool inRange[2] = {x + 1 < src.Width, y + 1 < src.Height};
            for (int n = 0; n < 2; ++n)
            {
                if (!inRange[n] || !used[neighbours[n]])
                    continue;
                uint32_t b[4] = {};
                ASSERT_TRUE(DestinationCodes(src, neighbours[n], b));
                ++pairs;
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])
                    ++differing;
            }
        }
    }
    ASSERT_GT(pairs, 0u) << arm << ": no adjacent mapped texel pairs to measure sharpness over";
    const double fraction = static_cast<double>(differing) / static_cast<double>(pairs);

    std::cout << "[ MEASURED ] " << arm << ": source " << src.Width << "x" << src.Height << " -> "
              << g.DestinationWidth << "x" << g.DestinationHeight << " (reference "
              << g.ReferenceWidth << "x" << g.ReferenceHeight << ", zoom " << g.Zoom << ", frac "
              << g.FracX << "," << g.FracY << ", origin " << g.OutputOriginX << ","
              << g.OutputOriginY << "), " << g.MappedPixels << " mapped + " << g.LetterboxPixels
              << " letterbox; min texel-boundary distance " << g.MinDistanceToTexelBoundary
              << ", min texel-centre distance " << g.MinDistanceToTexelCentre << "; "
              << fraction * 100.0 << "% of " << pairs << " adjacent mapped pairs differ; source R "
              << lo[0] << ".." << hi[0] << std::endl;

    EXPECT_GE(fraction, kMinAdjacentDifferingFraction)
        << arm << ": only " << fraction * 100.0
        << "% of adjacent sampled texels differ — over the rest a filtering sampler produces "
           "exactly what a point sampler does, so the byte gate is passing on texels that cannot "
           "disagree";
    EXPECT_GT(hi[0], lo[0]) << arm
                            << ": the transport's source is flat, so nothing downstream of it "
                               "can be measured";
}

// ── The gate. Zero tolerance, in stored codes, against this frame's own
// pre-transport readback through the index map.
void ExpectTransportedBytesMatchTheIndexMap(const HeadlessViewFixture& fx, const char* arm)
{
    const ViewOutputBytes& src = fx.Handoff();
    const ViewOutputBytes& dst = fx.TransportOutput();
    const std::vector<TransportIndex>& map = fx.TransportIndexMap();
    const TransportGeometry& g = fx.TransportShape();

    ASSERT_EQ(dst.Width, g.DestinationWidth);
    ASSERT_EQ(dst.Height, g.DestinationHeight);
    ASSERT_EQ(map.size(), static_cast<size_t>(dst.Width) * dst.Height)
        << arm << ": the index map does not cover the transported image";

    const uint32_t dstFullScaleAlpha = DestinationFullScaleCode(dst.Format, 3);
    ASSERT_GT(dstFullScaleAlpha, 0u)
        << arm << ": the transport destination format is not one this fixture unpacks";

    size_t mappedChecked = 0;
    size_t mappedBad = 0;
    size_t letterboxChecked = 0;
    size_t letterboxBad = 0;
    size_t alphaBad = 0;
    // One buffer per assertion. A shared one attaches the wrong class's
    // examples to whichever message fires, which is worse than no examples:
    // it points the reader at pixels that are not what the assertion is about.
    std::string mappedReport;
    size_t mappedReported = 0;
    std::string letterboxReport;
    size_t letterboxReported = 0;

    for (size_t i = 0; i < map.size(); ++i)
    {
        const TransportIndex& e = map[i];
        uint32_t got[4] = {};
        ASSERT_TRUE(DestinationCodes(dst, i, got))
            << arm << ": the transported image's format is not one this fixture unpacks";
        const uint32_t dx = static_cast<uint32_t>(i % dst.Width);
        const uint32_t dy = static_cast<uint32_t>(i / dst.Width);

        if (got[3] != dstFullScaleAlpha)
            ++alphaBad;

        if (e.Letterbox)
        {
            ++letterboxChecked;
            if (got[0] != 0u || got[1] != 0u || got[2] != 0u || got[3] != dstFullScaleAlpha)
            {
                ++letterboxBad;
                if (letterboxReported < kMaxReportedMismatches)
                {
                    ++letterboxReported;
                    letterboxReport += "\n  letterbox dst(" + std::to_string(dx) + "," +
                                       std::to_string(dy) + ") = " + std::to_string(got[0]) + "," +
                                       std::to_string(got[1]) + "," + std::to_string(got[2]) + "," +
                                       std::to_string(got[3]) + " (want 0,0,0," +
                                       std::to_string(dstFullScaleAlpha) + ")";
                }
            }
            continue;
        }

        ++mappedChecked;
        const size_t srcIndex = static_cast<size_t>(e.SourceY) * src.Width + e.SourceX;
        uint32_t want[4] = {};
        ASSERT_TRUE(DestinationCodes(src, srcIndex, want))
            << arm << ": the pre-transport image's format is not one this fixture unpacks";
        if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2])
        {
            ++mappedBad;
            if (mappedReported < kMaxReportedMismatches)
            {
                ++mappedReported;
                mappedReport += "\n  dst(" + std::to_string(dx) + "," + std::to_string(dy) +
                                ") = " + std::to_string(got[0]) + "," + std::to_string(got[1]) +
                                "," + std::to_string(got[2]) + " but src texel (" +
                                std::to_string(e.SourceX) + "," + std::to_string(e.SourceY) +
                                ") [sample " + std::to_string(e.SampleX) + "," +
                                std::to_string(e.SampleY) + "] = " + std::to_string(want[0]) + "," +
                                std::to_string(want[1]) + "," + std::to_string(want[2]);
            }
        }
    }

    std::cout << "[ MEASURED ] " << arm << ": " << mappedChecked << " mapped pixels, " << mappedBad
              << " disagreeing; " << letterboxChecked << " letterbox pixels, " << letterboxBad
              << " disagreeing; " << alphaBad << " pixels not at full-scale alpha" << std::endl;

    EXPECT_EQ(mappedBad, 0u)
        << arm << ": " << mappedBad << " of " << mappedChecked
        << " mapped pixels are not a copy of the source texel the pass's own contract sends them "
           "to. A filtering sampler, a regained transfer function on the encoded arm, or a change "
           "to the window/origin math all land here." << mappedReport;
    EXPECT_EQ(letterboxBad, 0u)
        << arm << ": " << letterboxBad << " of " << letterboxChecked
        << " letterbox pixels are not opaque black. The centering math has moved, or the "
           "destination is aliasing the source and its bars still hold source content."
        << letterboxReport;
    EXPECT_EQ(alphaBad, 0u)
        << arm << ": " << alphaBad
        << " transported pixels do not carry full-scale alpha, which the shader writes "
           "unconditionally";
}

} // namespace

// ── ARM (a): F16 -> F16, zoom 2, frac 0 — the production editor Game View
// shape, driven by the HOST policy rather than a re-implementation of it:
// DeclareViewFinalize pins its target to the source's F16 and stamps
// SdrFinalized, and GameViewController then transports that. This arm asserts
// byte equality against the same frame's own bytes; the fixture's HostPolicy
// pins carry the absolute amplitudes, so no amplitude is re-asserted here.
//
// Zoom 2 with frac 0 is NOT the degenerate case: it places samples at
// fractional texel coordinates .25 and .75, a quarter texel from both the
// boundary and the centre. Zoom 1 with frac 0 is the degenerate one, and the
// instrument's centre-distance assertion is what refuses it.
//
// REDDENS IF: the pass's sampler becomes linear; the encoded arm regains a
// transfer function (outEncoding leaves 0 for a non-_SRGB destination); the
// sample-window origin or the centering math changes; the destination stops
// being letterboxed.
TEST(HeadlessViewTransport, Float16GameViewTransportCopiesEveryPixelFromOneSourceTexel)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    const HeadlessViewDesc desc =
        TransportDescFor(FinalizeDriver::HostPolicy, TextureFormat::R16G16B16A16_FLOAT,
                         TextureFormat::R16G16B16A16_FLOAT, 2, 0.0f, 0.0f);
    HV_RENDER_OR_SKIP(fx, desc);

    // The host policy pins its target to the source's format, so this is the
    // F16->F16 arm by the policy's decision and not by the desc's request.
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::R16G16B16A16_FLOAT);
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::R16G16B16A16_FLOAT);
    ASSERT_TRUE(fx.Handoff().Space.has_value());
    EXPECT_EQ(*fx.Handoff().Space, UI::UITextureSpace::SdrFinalized())
        << "the transport's source must be a FINALIZED image — an unfinalized one would make the "
           "EncodedSrgb input space a lie and the byte claim meaningless";

    ExpectTransportInstrument(fx, "F16->F16 zoom 2 frac 0 (host policy)");
    ExpectTransportedBytesMatchTheIndexMap(fx, "F16->F16 zoom 2 frac 0 (host policy)");
}

// ── ARM (b): RGB10A2 -> RGB10A2, zoom 3, frac (0.3, -0.2) — a window shift on
// both axes into a quantizing destination. The shift is what makes this arm
// different in kind from (a): it moves the sample window sub-texel, so the
// index map and the render can disagree about WHICH texel a pixel copies rather
// than only about what happened to its value.
//
// The frac values are not free. At zoom 3, frac -0.2 puts the closest sample
// 1/30 of a texel from an integer boundary — just inside the 1/32 rule, and the
// instrument asserts it rather than trusting this note.
//
// REDDENS IF: everything arm (a) reddens on, plus the sub-texel sampleOrigin
// (1 + fracX, 1 - fracY) losing a sign or its one-texel border term, which arm
// (a)'s zero frac cannot see.
TEST(HeadlessViewTransport, TenBitTransportCopiesEveryPixelUnderASubTexelWindowShift)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    const HeadlessViewDesc desc =
        TransportDescFor(FinalizeDriver::PassDirect, TextureFormat::RGB10A2_UNORM,
                         TextureFormat::RGB10A2_UNORM, 3, 0.3f, -0.2f);
    HV_RENDER_OR_SKIP(fx, desc);

    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGB10A2_UNORM);
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::RGB10A2_UNORM);

    ExpectTransportInstrument(fx, "RGB10A2->RGB10A2 zoom 3 frac (0.3,-0.2)");
    ExpectTransportedBytesMatchTheIndexMap(fx, "RGB10A2->RGB10A2 zoom 3 frac (0.3,-0.2)");
}

// ── ARM (c): RGBA8_UNORM -> RGBA8_SRGB — the outEncoding 6 round trip.
//
// This is the arm the other two cannot stand in for. Into an _SRGB destination
// the pass does NOT pass bytes through: it emits D(c) so that the ROP's
// fixed-function re-encode E(D(c)) lands back on the byte it started from. That
// sRGB transcription is this shader's OWN copy — pixelperfect_upscale.frag has
// its own SRGBToLinear, so SrgbEncodeRampRoundTripTests, which pins the shared
// encode_srgb implementation, does not cover it and would not move if it
// drifted.
//
// The source is UNORM on purpose: sampling an _SRGB source would apply the
// hardware decode and the bytes compared would no longer be the bytes the
// finalize wrote.
//
// REDDENS IF: outEncoding 6 becomes 0 (raw bytes into an _SRGB ROP get
// re-encoded, so every mapped pixel moves); this shader's SRGBToLinear drifts
// from the exact inverse of the ROP's encode; plus everything arms (a) and (b)
// redden on.
TEST(HeadlessViewTransport, EncodedTransportRoundTripsEveryByteThroughAnSrgbDestination)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    const HeadlessViewDesc desc =
        TransportDescFor(FinalizeDriver::PassDirect, TextureFormat::RGBA8_UNORM,
                         TextureFormat::RGBA8_SRGB, 2, 0.1f, -0.15f);
    HV_RENDER_OR_SKIP(fx, desc);

    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGBA8_UNORM)
        << "the source must be UNORM — an _SRGB source would be hardware-decoded at sample time "
           "and the bytes compared would not be the bytes the finalize wrote";
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::RGBA8_SRGB)
        << "without an _SRGB destination the pass takes outEncoding 0 and this arm silently "
           "becomes a second copy of arm (b)";

    ExpectTransportInstrument(fx, "RGBA8_UNORM->RGBA8_SRGB zoom 2 frac (0.1,-0.15)");
    ExpectTransportedBytesMatchTheIndexMap(fx, "RGBA8_UNORM->RGBA8_SRGB zoom 2 frac (0.1,-0.15)");
}
