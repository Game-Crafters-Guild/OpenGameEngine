// I11 — the nearest-integer replication exemption, with pixels.
//
// WHAT THE ENGINE PROMISES AND NOTHING CHECKS: the finalize's dither lands at
// FINAL resolution and never below a filtering resample, and a point-sampled
// integer upscale is exempt from that rule because it rounds nothing and mixes
// nothing. GameViewController.cpp carries the exemption in a comment; the merge
// row that accepted it carries "0 LSB variance inside every replicated block" as
// a one-off measurement. Neither is a fixture. Let a filtering resample appear
// between the finalize and the upscale and the dither is averaged away — the
// banding the finalize exists to break comes back — while every test in the tree
// stays green.
//
// ── THE THREE THINGS THIS PIN ASSERTS, AND WHAT REDDENS EACH ────────────────
// (1) The dither is IN the finalized image at the finalize's own resolution,
//     sized to one code of the surface it was written to.
//     REDDENS ON: the RGB10A2_UNORM row leaving EncodeDitherLsbForFormat; the
//     Destination case of the quantizer selector collapsing into the Presented
//     one; the amplitude being scaled; the dither being removed; the pattern
//     degenerating (the spread catches that class even if TriangularDitherRef is
//     resynced to it). On the Presented arm, additionally: SRGBEncodePass.cpp
//     dropping `presentedFormat.value_or(...)` back to the bare device read.
// (2) The integer upscale rounds nothing and mixes nothing — every pixel of a
//     zoom x zoom block carries byte-identical codes, those codes ARE the source
//     texel's, and the letterbox is opaque black.
//     REDDENS ON: the pass's sampler becoming linear; the encoded arm regaining
//     a transfer function; the window origin or centering math moving; the
//     destination aliasing the source (the letterbox bars would hold content).
// (3) De-replicated, the transported image still carries the SAME dither, in the
//     finalize's own pattern phase and at the destination's own step.
//     REDDENS ON: a filtering resample entering between the finalize and the
//     upscale. See the honesty row below — at step 0 that is a fixture-local
//     mutation, not a production one.
//
// ── HONESTY ROW ───────────────────────────────────────────────────────────────
// At step 0 assertion (3) is LARGELY IMPLIED by (1) and (2), because this
// fixture is its own sequencer: it declares the finalize and the transport
// itself, in that order, with nothing between them, so a de-replicated block is
// arithmetically the same byte (2) already compared. Its non-redundant life
// begins the day step 1 replaces hand-sequencing with the chain — (2) then
// compares against whatever the chain handed the transport, so a filtering pass
// sneaking in between S2 and S4 leaves (2) green and only (3) can see it. Its
// red arm TODAY is therefore a LOCAL FIXTURE MUTATION — a bilinear resample
// declared between the finalize and the transport — not a production one, and
// per landing 0's bar that red run is required before this pin is accepted
// green. It was run; the commit message records what moved.
//
// ── NO GOLDEN, AND NO EXPECTATION READ FROM THE TABLE UNDER TEST ────────────
// Every expectation is either same-frame relational (a block against the same
// submission's pre-transport readback, through an index map built from the
// desc's geometry alone) or derived from the destination format's BIT DEPTH via
// DestinationCodeScale. Nothing here reads EncodeDitherLsbForFormat, so a
// mutation of that table moves the measurement and leaves the expectation
// standing.
//
// ── THE A2 RULE ─────────────────────────────────────────────────────────────
// An absolute amplitude may be expected on FinalizeQuantizer::Destination, or on
// Presented WITH an explicitly supplied presentedFormat. Never on Presented left
// unset headless, where the amplitude collapses to the 8-bit row and an absolute
// expectation would bake that collapse into itself. All three arms below satisfy
// that rule; none uses FinalizeDriver::HostPolicy, whose policy still passes
// std::nullopt.
//
// Skip policy (chain contract rule 6, three classes):
//   absent device            -> FAIL
//   absent staged asset      -> FAIL (an undeclared pass is this class)
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

// The dither and deband kill switches latch into function-local statics on first
// read, so they are a property of the PROCESS. This TU pins them itself rather
// than relying on a sibling TU's static: cross-TU initialization order is
// unspecified, and a gate whose instrument depends on which translation unit
// happened to construct first is not a gate. All three TUs write the same
// values, so any order leaves the process in the state each describes.
//
// GE_OUTPUT_DITHER=1 is the subject here — with the dither off every amplitude
// below reads 0 and the pin has nothing to measure. GE_DEBAND=0 keeps the
// encode a per-pixel function: the deband is a neighbourhood filter, and a
// filter inside the finalize would smooth the transport's source, which is the
// one property the adjacent-blocks-differ instrument needs it not to have.
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

// The view, and therefore the finalize destination and the transport's source.
// The pass pads its source by one texel per side, so this is the reference
// window plus 2.
constexpr uint32_t kViewExtent = 130;
constexpr uint32_t kReferenceExtent = kViewExtent - 2;

// The production Game View shape: a whole-number zoom, no sub-texel remainder.
// Zoom 2 with frac 0 places samples at fractional texel coordinates .25 and .75
// — a quarter texel from both an integer boundary and a texel centre, so it is
// NOT the degenerate zoom-1/frac-0 geometry where a bilinear sampler returns
// exactly what a nearest one does.
constexpr uint32_t kZoom = 2;

// Surplus beyond the upscaled rect so there are bars on all four sides.
// Asymmetric, so a transposed origin cannot accidentally land right.
constexpr uint32_t kLetterboxSurplusX = 16;
constexpr uint32_t kLetterboxSurplusY = 8;

// Amplitude band, in the arm's own destination codes. Carried from the sibling
// suite, where the healthy arms measure within 0.001 of expectation: 0.05 is
// fifty times the observed spread and a tenth of the smallest sizing error
// anyone has proposed as plausible (a 2x scale is 0.5 away).
constexpr double kAmplitudeToleranceLsb = 0.05;

// Displacement-spread band. A 1-LSB TPDF plus the destination's own rounding
// gives sqrt(1/6 + 1/12) = 0.5 destination LSB; rounding alone — which every
// degenerate pattern collapses to, whatever DC offset it carries — gives
// sqrt(1/12) = 0.2887. The spread never reads the dither pattern, so no
// transcription of a broken shader into the reference can satisfy it.
constexpr double kHealthyDitherStdLsb = 0.5;
constexpr double kStdToleranceLsb = 0.05;

// How many adjacent block pairs must differ. Blocks that equal their neighbours
// are blocks on which a filtering sampler and a point sampler agree, so the
// replication assertion would be passing on pixels that cannot disagree.
constexpr double kMinAdjacentDifferingFraction = 0.90;

// Diagnostic budget — chain rule 1: a gate must be able to name the pixels it
// disagreed about.
constexpr std::size_t kMaxReportedMismatches = 8;

float SrgbToLinearContent(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
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

// Content this pin needs two independent things from, which is why it is
// neither the sibling suite's smooth ramp nor the transport suite's pure hash.
//
// SHARP, so replication is a claim about something: adjacent texels carry
// independent levels one 64th of kLevelSpan apart — 0.0121 encoded, three codes
// of the COARSEST destination any arm here writes. A flat neighbourhood makes a
// constant block trivially constant and makes a filtering sampler
// indistinguishable from a point one, and a step narrower than the dither would
// let neighbours collide after quantization.
//
// PHASE-SWEEPING, so the amplitude projection is unbiased: 64 discrete levels
// sample the destination's sub-code rounding phase at 64 fixed points, and the
// least-squares slope averages the rounding term properly only when that phase
// is swept. The slow full-image ramp moves every level continuously across tens
// of codes, at a spatial frequency two orders below the dither pattern's.
constexpr uint32_t kContentLevelMask = 63u; // 64 levels; must stay 2^k - 1

std::vector<float> MakeReplicationContent(uint32_t width, uint32_t height)
{
    constexpr float kEncodedFloor = 0.10f;
    constexpr float kLevelSpan = 0.76f;
    constexpr float kRampSpan = 0.06f;
    std::vector<float> px(static_cast<size_t>(width) * height * 4);
    const size_t pixels = static_cast<size_t>(width) * height;
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * width + x;
            const float ramp =
                kRampSpan * (static_cast<float>(index) / static_cast<float>(pixels - 1));
            const uint32_t h = ContentHash(x, y);
            for (int ch = 0; ch < 3; ++ch)
            {
                const uint32_t level = (h >> (ch * 8)) & kContentLevelMask;
                const float encoded =
                    kEncodedFloor +
                    kLevelSpan * (static_cast<float>(level) /
                                  static_cast<float>(kContentLevelMask)) +
                    ramp;
                px[index * 4 + static_cast<size_t>(ch)] = SrgbToLinearContent(encoded);
            }
            px[index * 4 + 3] = 1.0f;
        }
    }
    return px;
}

HeadlessViewDesc ReplicationDesc(TextureFormat format, Passes::FinalizeQuantizer quantizer,
                                 std::optional<TextureFormat> presentedFormat)
{
    HeadlessViewDesc d{};
    d.Width = kViewExtent;
    d.Height = kViewExtent;
    d.ClearLinear[0] = 0.0f;
    d.ClearLinear[1] = 0.0f;
    d.ClearLinear[2] = 0.0f;
    d.ClearLinear[3] = 1.0f;
    d.ContentLinearRgba = MakeReplicationContent(kViewExtent, kViewExtent);
    d.Driver = FinalizeDriver::PassDirect;
    d.DestinationFormat = format;
    d.InputSpace = Passes::FinalizeInputSpace::Linear;
    d.Quantizer = quantizer;
    d.PresentedFormat = presentedFormat;

    TransportDesc t{};
    t.DestinationWidth = kReferenceExtent * kZoom + kLetterboxSurplusX;
    t.DestinationHeight = kReferenceExtent * kZoom + kLetterboxSurplusY;
    // Same format as the finalize destination: the replication claim is about
    // bytes, and a format change between the two would put a conversion where
    // the claim says there is a copy.
    t.DestinationFormat = format;
    t.Zoom = kZoom;
    t.FracX = 0.0f;
    t.FracY = 0.0f;
    // The finalize already applied the OETF, so the transport applies none.
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

// One replicated block: every destination pixel the index map sends to one
// source texel.
struct Block
{
    uint32_t Codes[4] = {0, 0, 0, 0};
    uint32_t Pixels = 0;
    bool Constant = true;
};

struct ReplicationMeasurement
{
    std::size_t Blocks = 0;
    std::size_t NonConstantBlocks = 0;
    std::size_t MiscopiedBlocks = 0;
    uint32_t MinBlockPixels = 0;
    uint32_t MaxBlockPixels = 0;

    std::size_t LetterboxChecked = 0;
    std::size_t LetterboxBad = 0;
    std::size_t AlphaBad = 0;

    std::size_t AdjacentPairs = 0;
    double AdjacentDifferingFraction = 0.0;

    /// De-replicated: one sample per block per colour channel, projected onto
    /// the FINALIZE's own dither phase at that block's source texel.
    double AmplitudeEncoded = 0.0;
    double SpreadEncoded = 0.0;
    std::size_t AmplitudeSamples = 0;

    std::string ConstancyReport;
    std::string CopyReport;
    std::string LetterboxReport;
};

void Append(std::string& report, std::size_t& reported, const std::string& line)
{
    if (reported >= kMaxReportedMismatches)
        return;
    ++reported;
    report += "\n  " + line;
}

std::string CodesToString(const uint32_t codes[4])
{
    return std::to_string(codes[0]) + "," + std::to_string(codes[1]) + "," +
           std::to_string(codes[2]) + "," + std::to_string(codes[3]);
}

// Walk the transported image once, folding it back onto the source grid.
//
// Everything below is computed from the fixture's own last frame: the index map
// (built from the desc's geometry, never from the pass), the pre-transport
// readback and the pre-finalize source tap, all from ONE submission. There is
// nothing here for a golden to have been regenerated from.
ReplicationMeasurement MeasureReplication(const HeadlessViewFixture& fx)
{
    ReplicationMeasurement m{};
    const ViewOutputBytes& src = fx.Handoff();
    const ViewOutputBytes& dst = fx.TransportOutput();
    const ViewStageTap& tap = fx.SourceBeforeFinalize();
    const std::vector<TransportIndex>& map = fx.TransportIndexMap();
    if (src.Width == 0 || dst.Width == 0 || map.size() != static_cast<size_t>(dst.Width) * dst.Height)
        return m;
    // The finalize writes the view's own grid, so the pre-finalize tap and the
    // pre-transport readback index the same way. Checked rather than assumed:
    // the amplitude projection pairs a destination code with a source pixel by
    // that identity, and a mismatched pair is a number about two different
    // places.
    if (tap.Width != src.Width || tap.Height != src.Height ||
        tap.Rgba.size() != static_cast<size_t>(src.Width) * src.Height * 4)
        return m;

    const uint32_t fullScaleAlpha = DestinationFullScaleCode(dst.Format, 3);
    const float codeScale = DestinationCodeScale(dst.Format);
    if (fullScaleAlpha == 0 || codeScale <= 0.0f)
        return m;

    std::vector<Block> blocks(static_cast<size_t>(src.Width) * src.Height);
    std::size_t constancyReported = 0;
    std::size_t copyReported = 0;
    std::size_t letterboxReported = 0;

    for (size_t i = 0; i < map.size(); ++i)
    {
        const TransportIndex& e = map[i];
        uint32_t got[4] = {};
        if (!DestinationCodes(dst, i, got))
            return {};
        if (got[3] != fullScaleAlpha)
            ++m.AlphaBad;

        if (e.Letterbox)
        {
            ++m.LetterboxChecked;
            if (got[0] != 0u || got[1] != 0u || got[2] != 0u || got[3] != fullScaleAlpha)
            {
                ++m.LetterboxBad;
                Append(m.LetterboxReport, letterboxReported,
                       "letterbox dst(" + std::to_string(i % dst.Width) + "," +
                           std::to_string(i / dst.Width) + ") = " + CodesToString(got) +
                           " (want 0,0,0," + std::to_string(fullScaleAlpha) + ")");
            }
            continue;
        }

        Block& b = blocks[static_cast<size_t>(e.SourceY) * src.Width + e.SourceX];
        if (b.Pixels == 0)
        {
            for (int ch = 0; ch < 4; ++ch)
                b.Codes[ch] = got[ch];
        }
        else if (b.Constant &&
                 (got[0] != b.Codes[0] || got[1] != b.Codes[1] || got[2] != b.Codes[2]))
        {
            b.Constant = false;
            Append(m.ConstancyReport, constancyReported,
                   "block for src texel (" + std::to_string(e.SourceX) + "," +
                       std::to_string(e.SourceY) + ") holds both " + CodesToString(b.Codes) +
                       " and " + CodesToString(got) + " (first seen at dst(" +
                       std::to_string(i % dst.Width) + "," + std::to_string(i / dst.Width) + "))");
        }
        ++b.Pixels;
    }

    // Second pass over the blocks: size, the copy claim, adjacency and the
    // de-replicated amplitude.
    double sumTd = 0.0;
    double sumTt = 0.0;
    double sumD = 0.0;
    double sumDd = 0.0;
    std::size_t differing = 0;
    for (uint32_t sy = 0; sy < src.Height; ++sy)
    {
        for (uint32_t sx = 0; sx < src.Width; ++sx)
        {
            const size_t here = static_cast<size_t>(sy) * src.Width + sx;
            const Block& b = blocks[here];
            if (b.Pixels == 0)
                continue;
            ++m.Blocks;
            if (!b.Constant)
                ++m.NonConstantBlocks;
            m.MinBlockPixels = m.MinBlockPixels == 0 ? b.Pixels : std::min(m.MinBlockPixels, b.Pixels);
            m.MaxBlockPixels = std::max(m.MaxBlockPixels, b.Pixels);

            uint32_t want[4] = {};
            if (!DestinationCodes(src, here, want))
                return {};
            if (b.Codes[0] != want[0] || b.Codes[1] != want[1] || b.Codes[2] != want[2])
            {
                ++m.MiscopiedBlocks;
                Append(m.CopyReport, copyReported,
                       "block for src texel (" + std::to_string(sx) + "," + std::to_string(sy) +
                           ") = " + CodesToString(b.Codes) + " but that texel holds " +
                           CodesToString(want));
            }

            const size_t neighbours[2] = {here + 1, here + src.Width};
            const bool inRange[2] = {sx + 1 < src.Width, sy + 1 < src.Height};
            for (int n = 0; n < 2; ++n)
            {
                if (!inRange[n] || blocks[neighbours[n]].Pixels == 0)
                    continue;
                ++m.AdjacentPairs;
                const Block& o = blocks[neighbours[n]];
                if (b.Codes[0] != o.Codes[0] || b.Codes[1] != o.Codes[1] || b.Codes[2] != o.Codes[2])
                    ++differing;
            }

            // The dither is a function of the FINALIZE's fragment coordinate,
            // and the finalize wrote this source texel — so the phase belongs to
            // (sx, sy). A resample between the two grids decorrelates the
            // displacement from this pattern even when the blocks are perfectly
            // constant, which is the whole point of measuring it here rather
            // than trusting that the bytes matched.
            const double t = TriangularDitherRef(sx, sy);
            for (int ch = 0; ch < 3; ++ch)
            {
                const double observed = static_cast<double>(b.Codes[ch]) / codeScale;
                const float lin =
                    std::max(0.0f, tap.Rgba[here * 4 + static_cast<size_t>(ch)]);
                const double d = observed - LinearToSrgbRef(lin);
                sumTd += t * d;
                sumTt += t * t;
                sumD += d;
                sumDd += d * d;
                ++m.AmplitudeSamples;
            }
        }
    }

    if (m.AdjacentPairs > 0)
        m.AdjacentDifferingFraction =
            static_cast<double>(differing) / static_cast<double>(m.AdjacentPairs);
    if (sumTt > 0.0)
        m.AmplitudeEncoded = sumTd / sumTt;
    if (m.AmplitudeSamples > 1)
    {
        const double n = static_cast<double>(m.AmplitudeSamples);
        const double mean = sumD / n;
        m.SpreadEncoded = std::sqrt(std::max(0.0, (sumDd - n * mean * mean) / (n - 1.0)));
    }
    return m;
}

// ── The instrument. Each of these is a route by which the assertions below pass
// while measuring nothing.
void ExpectReplicationInstrument(const HeadlessViewFixture& fx, const ReplicationMeasurement& m,
                                 const char* arm)
{
    const TransportGeometry& g = fx.TransportShape();

    // 1. Both passes ran, and in the order the claim depends on. Scheduled
    //    order, not declaration order: a culled pass appears in both lists.
    // Two finalize arms, one property (direct "…Finalize" / host-policy
    // "…Finalized"); exactly one is scheduled in any run.
    std::optional<std::size_t> finalizeIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalize"});
    if (!finalizeIdx)
        finalizeIdx =
            RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalized"});
    const std::optional<std::size_t> transportIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Transport"});
    ASSERT_TRUE(transportIdx.has_value())
        << arm << ": no scheduled transport pass — the upscale was culled or never declared, so "
                  "every block below came from somewhere else";
    ASSERT_TRUE(finalizeIdx.has_value())
        << arm << ": no scheduled finalize pass, so nothing dithered the transport's source";
    EXPECT_LT(*finalizeIdx, *transportIdx)
        << arm << ": the transport is scheduled at " << *transportIdx << " and the finalize at "
        << *finalizeIdx << " — it sampled the finalize's destination before the finalize wrote it";

    // 2. The geometry is the replicating one and has both populations.
    EXPECT_GT(m.Blocks, 0u) << arm << ": the map produced no replicated blocks at all";
    EXPECT_GT(g.LetterboxPixels, 0u)
        << arm << ": the destination has no letterbox bars, so the centering and the "
                  "dst-aliases-src class both go unasserted";
    EXPECT_EQ(g.ClampedSamples, 0u)
        << arm << ": " << g.ClampedSamples
        << " samples needed clamp-to-edge — the window has walked off the padded border and the "
           "one-texel-copy claim would be satisfied by the addressing mode";
    const uint32_t expectedBlockPixels = kZoom * kZoom;
    EXPECT_EQ(m.MinBlockPixels, expectedBlockPixels)
        << arm << ": the smallest block holds " << m.MinBlockPixels << " pixels, not zoom^2 = "
        << expectedBlockPixels << " — the map is not the replicating geometry this pin describes";
    EXPECT_EQ(m.MaxBlockPixels, expectedBlockPixels)
        << arm << ": the largest block holds " << m.MaxBlockPixels << " pixels, not zoom^2 = "
        << expectedBlockPixels;

    // 3. Neighbouring blocks differ, so constancy is a property of the transport
    //    and not of the content. Where neighbours are equal a filtering sampler
    //    produces exactly what a point sampler does.
    EXPECT_GE(m.AdjacentDifferingFraction, kMinAdjacentDifferingFraction)
        << arm << ": only " << m.AdjacentDifferingFraction * 100.0 << "% of " << m.AdjacentPairs
        << " adjacent block pairs differ — over the rest the replication assertion is passing on "
           "blocks that could not have disagreed";

    // 4. Something to project onto.
    EXPECT_GT(m.AmplitudeSamples, 0u) << arm << ": no de-replicated samples";
}

void ReportArm(const char* arm, double codeScale, const ReplicationMeasurement& m,
               const DitherAmplitude& preTransport, const ResidualStats& preTransportResidual)
{
    std::cout << "[ MEASURED ] " << arm << ": " << m.Blocks << " blocks of "
              << m.MinBlockPixels << ".." << m.MaxBlockPixels << " px, " << m.NonConstantBlocks
              << " non-constant, " << m.MiscopiedBlocks << " miscopied; " << m.LetterboxChecked
              << " letterbox px, " << m.LetterboxBad << " bad, " << m.AlphaBad
              << " off full-scale alpha; " << m.AdjacentDifferingFraction * 100.0 << "% of "
              << m.AdjacentPairs << " adjacent block pairs differ; pre-transport amplitude "
              << preTransport.Encoded * codeScale << " LSB (spread "
              << preTransportResidual.StdEncoded * codeScale << "), de-replicated amplitude "
              << m.AmplitudeEncoded * codeScale << " LSB (spread " << m.SpreadEncoded * codeScale
              << ") over " << m.AmplitudeSamples << " samples" << std::endl;
}

} // namespace

// ── ARM 1: dither ON, finalize Destination -> RGB10A2, transport zoom 2 into a
// letterboxed RGB10A2 destination. The quantizer whose amplitude is a property
// of the surface being written, so the expectation is a fact about the format's
// bit depth and owes the production amplitude table nothing.
TEST(HeadlessViewReplication, IntegerUpscaleCarriesTheDestinationDitherThroughUnchanged)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HV_RENDER_OR_SKIP(fx, ReplicationDesc(TextureFormat::RGB10A2_UNORM,
                                          Passes::FinalizeQuantizer::Destination, std::nullopt));

    constexpr const char* kArm = "Destination -> RGB10A2, zoom 2";
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGB10A2_UNORM);
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::RGB10A2_UNORM);
    // No space assertion here, deliberately: on the PassDirect arm the fixture
    // assigns Handoff().Space itself, so comparing it to SdrFinalized() would
    // be a literal against a copy of itself — the shape this fixture's first
    // incarnation failed in. The pairing gate lives in the shape-B suite,
    // where the HostPolicy publish is the operand and production code can
    // falsify it.

    const double codes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const DitherAmplitude preTransport = fx.MeasureDitherAmplitude();
    const ResidualStats preTransportResidual = fx.EncodeOnceResidual();
    const ReplicationMeasurement m = MeasureReplication(fx);
    ReportArm(kArm, codes, m, preTransport, preTransportResidual);
    ExpectReplicationInstrument(fx, m, kArm);

    // (1) The dither is in the finalized image, at the finalize's own
    //     resolution, sized to one code of the surface it was written to. This
    //     is what (3) is a claim ABOUT — with no dither upstream there is
    //     nothing for the upscale to carry through, and (3) would read ~0 for a
    //     healthy transport.
    ASSERT_GT(preTransport.Samples, 0u) << kArm << ": nothing to measure before the transport";
    EXPECT_NEAR(preTransport.Encoded * codes, 1.0, kAmplitudeToleranceLsb)
        << kArm << ": the finalize dithered its ten-bit destination at "
        << preTransport.Encoded * codes << " of its own codes rather than 1. Residual map: "
        << fx.WriteResidualMap("replication-pre-transport");
    EXPECT_NEAR(preTransportResidual.StdEncoded * codes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << kArm << ": the pre-transport displacement's spread is "
        << preTransportResidual.StdEncoded * codes
        << " ten-bit LSB where a 1-LSB TPDF plus rounding gives 0.5 — at sqrt(1/12) = 0.2887 the "
           "pixels hold rounding alone and the amplitude the projection reported is not in them";

    // (2) The upscale rounds nothing and mixes nothing.
    EXPECT_EQ(m.NonConstantBlocks, 0u)
        << kArm << ": " << m.NonConstantBlocks << " of " << m.Blocks
        << " replicated blocks are not internally constant. A point-sampled integer upscale reads "
           "one texel per block, so any variance inside one means the sampler filtered."
        << m.ConstancyReport;
    EXPECT_EQ(m.MiscopiedBlocks, 0u)
        << kArm << ": " << m.MiscopiedBlocks << " of " << m.Blocks
        << " blocks are constant but hold something other than their source texel's codes. A "
           "regained transfer function on the encoded arm, or a change to the window/origin math, "
           "lands here." << m.CopyReport;
    EXPECT_EQ(m.LetterboxBad, 0u)
        << kArm << ": " << m.LetterboxBad << " of " << m.LetterboxChecked
        << " letterbox pixels are not opaque black — the centering math has moved, or the "
           "destination is aliasing the source and its bars still hold content."
        << m.LetterboxReport;
    EXPECT_EQ(m.AlphaBad, 0u)
        << kArm << ": " << m.AlphaBad
        << " transported pixels are off full-scale alpha, which the shader writes unconditionally";

    // (3) De-replicated, the same dither is still there, in the finalize's own
    //     pattern phase and at the destination's own step. See the honesty row
    //     at the top of this file: at step 0 this is largely implied by (1) and
    //     (2), and its red arm is a fixture-local bilinear resample declared
    //     between the finalize and the transport.
    EXPECT_NEAR(m.AmplitudeEncoded * codes, 1.0, kAmplitudeToleranceLsb)
        << kArm << ": de-replicated, the transported image carries a dither of "
        << m.AmplitudeEncoded * codes
        << " ten-bit LSB rather than 1. The upscale is exempt from the 'dither at final "
           "resolution' rule only because it replicates; a filtering resample anywhere between "
           "the finalize and here averages the dither away and the banding comes back";
    EXPECT_NEAR(m.SpreadEncoded * codes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << kArm << ": the de-replicated displacement's spread is " << m.SpreadEncoded * codes
        << " ten-bit LSB where 0.5 is a 1-LSB TPDF plus rounding. This never reads the dither "
           "pattern, so a degenerate one collapses it to sqrt(1/12) = 0.2887 whatever the "
           "projection above reported";
    // Relational, and honestly redundant while the fixture sequences the frame
    // itself: with (2) green a block IS the byte (1) measured. It earns its keep
    // at step 1, when (2)'s reference becomes whatever the chain handed the
    // transport and this is the only assertion still looking at the finalize.
    EXPECT_NEAR(m.AmplitudeEncoded * codes, preTransport.Encoded * codes, kAmplitudeToleranceLsb)
        << kArm << ": the dither measured through the transport ("
        << m.AmplitudeEncoded * codes << " LSB) is not the one measured before it ("
        << preTransport.Encoded * codes << " LSB)";
}

// ── ARM 2: the same frame with the quantizer production hosts actually name.
// FinalizeQuantizer::Presented sizes the dither to what will be presented rather
// than to what this pass writes; headless that read collapses to the table's
// 8-bit row, so this arm supplies the format explicitly (the pass's
// `presentedFormat`, live since the finalize arc) and measures the Presented arm
// at its true depth off-screen for the first time.
//
// WHAT THIS ARM DOES NOT SETTLE — the quantizer's IDENTITY. Its destination and
// its presented format are the same RGB10A2, so a selector that ignored the
// Presented case and fell through to the destination's own step would also read
// 1.0 here. That question is arm 3's.
//
// REDDENS IF: SRGBEncodePass.cpp's Presented case drops `presentedFormat` and
// goes back to the bare device read (this reads 4.01 ten-bit LSB — the 8-bit
// step — instead of the supplied format's 1.0); plus everything arm 1
// reddens on.
TEST(HeadlessViewReplication, PresentedQuantizerSizedToASuppliedFormatReplicatesTheSameWay)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HV_RENDER_OR_SKIP(fx, ReplicationDesc(TextureFormat::RGB10A2_UNORM,
                                          Passes::FinalizeQuantizer::Presented,
                                          TextureFormat::RGB10A2_UNORM));

    constexpr const char* kArm = "Presented(RGB10A2) -> RGB10A2, zoom 2";
    ASSERT_EQ(fx.Device()->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << kArm
        << ": a headless device reported a swapchain format, so this arm is no longer proving "
           "that the SUPPLIED format is what sized the dither";
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGB10A2_UNORM);
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::RGB10A2_UNORM);

    const double codes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const DitherAmplitude preTransport = fx.MeasureDitherAmplitude();
    const ResidualStats preTransportResidual = fx.EncodeOnceResidual();
    const ReplicationMeasurement m = MeasureReplication(fx);
    ReportArm(kArm, codes, m, preTransport, preTransportResidual);
    ExpectReplicationInstrument(fx, m, kArm);

    ASSERT_GT(preTransport.Samples, 0u) << kArm << ": nothing to measure before the transport";
    EXPECT_NEAR(preTransport.Encoded * codes, 1.0, kAmplitudeToleranceLsb)
        << kArm << ": the Presented arm dithered at " << preTransport.Encoded * codes
        << " ten-bit LSB. 4.01 is the 8-bit step, i.e. the supplied presented format was ignored "
           "and the device's absent swapchain answered instead. Residual map: "
        << fx.WriteResidualMap("replication-presented");
    EXPECT_NEAR(preTransportResidual.StdEncoded * codes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << kArm << ": the pre-transport displacement's spread is "
        << preTransportResidual.StdEncoded * codes << " ten-bit LSB where 0.5 is a 1-LSB TPDF "
           "plus rounding";

    EXPECT_EQ(m.NonConstantBlocks, 0u)
        << kArm << ": " << m.NonConstantBlocks << " of " << m.Blocks
        << " replicated blocks are not internally constant" << m.ConstancyReport;
    EXPECT_EQ(m.MiscopiedBlocks, 0u)
        << kArm << ": " << m.MiscopiedBlocks << " of " << m.Blocks
        << " blocks hold something other than their source texel's codes" << m.CopyReport;
    EXPECT_EQ(m.LetterboxBad, 0u)
        << kArm << ": " << m.LetterboxBad << " of " << m.LetterboxChecked
        << " letterbox pixels are not opaque black" << m.LetterboxReport;
    EXPECT_EQ(m.AlphaBad, 0u) << kArm << ": " << m.AlphaBad << " pixels off full-scale alpha";

    EXPECT_NEAR(m.AmplitudeEncoded * codes, 1.0, kAmplitudeToleranceLsb)
        << kArm << ": de-replicated, the transported image carries " << m.AmplitudeEncoded * codes
        << " ten-bit LSB of dither rather than 1";
    EXPECT_NEAR(m.SpreadEncoded * codes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << kArm << ": the de-replicated displacement's spread is " << m.SpreadEncoded * codes
        << " ten-bit LSB where 0.5 is a 1-LSB TPDF plus rounding";
    EXPECT_NEAR(m.AmplitudeEncoded * codes, preTransport.Encoded * codes, kAmplitudeToleranceLsb)
        << kArm << ": the dither measured through the transport is not the one measured before it";
}

// ── ARM 3: the one number only the correct path produces.
//
// NOT in the spec's I11 subsection, so it states its own reddening mutations.
// Arms 1 and 2 both write a destination whose own step equals the presented
// step, which leaves three different implementations reading the same 1.0:
// Presented sized to the supplied format, Presented silently falling through to
// the destination, and Presented ignoring the supplied format on a device whose
// swapchain read happens to agree. Splitting the two formats separates all
// three. An EIGHT-bit destination told to size its dither to a TEN-bit presented
// surface must dither at one TENTH-bit code, which is 255/1023 = 0.2493 of its
// own — a value neither of the other two implementations can produce.
//
// The expectation is a ratio of two DestinationCodeScale values, i.e. of two bit
// depths. It reads nothing from EncodeDitherLsbForFormat.
//
// REDDENS IF: SRGBEncodePass.cpp's Presented case drops `presentedFormat` and
// takes the device read (Unknown -> the 8-bit row -> 1.0 here); the Presented
// case is collapsed into the Destination case (1.0 here); the dither is removed
// or rescaled; plus everything arm 1 reddens on.
//
// NO SPREAD ASSERTION HERE, and that is arithmetic rather than leniency: a
// 0.2493-LSB TPDF plus this destination's rounding gives sqrt(0.2493^2/6 +
// 1/12) = 0.306, and rounding alone gives 0.2887 — closer together than the
// band the healthy arms need. The spread cannot discriminate at this amplitude,
// so it is reported and not gated; the degenerate-pattern class stays covered by
// arms 1 and 2, which write a dither the size of their own code.
TEST(HeadlessViewReplication, PresentedFormatSizesTheDitherToTheSuppliedSurfaceNotTheWrittenOne)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HV_RENDER_OR_SKIP(fx, ReplicationDesc(TextureFormat::RGBA8_UNORM,
                                          Passes::FinalizeQuantizer::Presented,
                                          TextureFormat::RGB10A2_UNORM));

    constexpr const char* kArm = "Presented(RGB10A2) -> RGBA8_UNORM, zoom 2";
    ASSERT_EQ(fx.Device()->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << kArm << ": a headless device reported a swapchain format — the discriminator below "
                   "rests on the device read being the one thing that cannot answer";
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGBA8_UNORM)
        << kArm << ": the destination must be EIGHT-bit for this arm to separate the presented "
                   "step from the written one";
    ASSERT_EQ(fx.TransportOutput().Format, TextureFormat::RGBA8_UNORM);

    // One ten-bit code expressed in this destination's own eight-bit codes.
    // Both terms are bit depths; neither is the amplitude table.
    const double eightBitCodes = DestinationCodeScale(TextureFormat::RGBA8_UNORM);
    const double tenBitCodes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const double expected = eightBitCodes / tenBitCodes;

    const DitherAmplitude preTransport = fx.MeasureDitherAmplitude();
    const ResidualStats preTransportResidual = fx.EncodeOnceResidual();
    const ReplicationMeasurement m = MeasureReplication(fx);
    ReportArm(kArm, eightBitCodes, m, preTransport, preTransportResidual);
    std::cout << "[ MEASURED ] " << kArm << ": one ten-bit code is " << expected
              << " eight-bit codes" << std::endl;
    ExpectReplicationInstrument(fx, m, kArm);

    ASSERT_GT(preTransport.Samples, 0u) << kArm << ": nothing to measure before the transport";
    EXPECT_NEAR(preTransport.Encoded * eightBitCodes, expected, kAmplitudeToleranceLsb)
        << kArm << ": the finalize dithered its eight-bit destination at "
        << preTransport.Encoded * eightBitCodes << " of its own codes. " << expected
        << " is one code of the TEN-bit surface it was told it would be presented on; 1.0 means "
           "the supplied presented format was not what sized the dither — either it was dropped "
           "for the device read, or the Presented case fell through to the destination's own "
           "step. Residual map: "
        << fx.WriteResidualMap("replication-presented-8bit-dst");

    EXPECT_EQ(m.NonConstantBlocks, 0u)
        << kArm << ": " << m.NonConstantBlocks << " of " << m.Blocks
        << " replicated blocks are not internally constant" << m.ConstancyReport;
    EXPECT_EQ(m.MiscopiedBlocks, 0u)
        << kArm << ": " << m.MiscopiedBlocks << " of " << m.Blocks
        << " blocks hold something other than their source texel's codes" << m.CopyReport;
    EXPECT_EQ(m.LetterboxBad, 0u)
        << kArm << ": " << m.LetterboxBad << " of " << m.LetterboxChecked
        << " letterbox pixels are not opaque black" << m.LetterboxReport;
    EXPECT_EQ(m.AlphaBad, 0u) << kArm << ": " << m.AlphaBad << " pixels off full-scale alpha";

    EXPECT_NEAR(m.AmplitudeEncoded * eightBitCodes, expected, kAmplitudeToleranceLsb)
        << kArm << ": de-replicated, the transported image carries "
        << m.AmplitudeEncoded * eightBitCodes << " eight-bit LSB of dither rather than " << expected;
    EXPECT_NEAR(m.AmplitudeEncoded * eightBitCodes, preTransport.Encoded * eightBitCodes,
                kAmplitudeToleranceLsb)
        << kArm << ": the dither measured through the transport is not the one measured before it";
}
