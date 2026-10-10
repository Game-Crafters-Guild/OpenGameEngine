// Pixel-level pin for the Slug text reconstruction filter. The per-crossing
// coverage ramp in slug_functions.glsl is the exact-area ramp convolved with
// the kTextFilterWidthPx box (ui_sdf_common.glsl), so a straight stem edge is
// the step response of pixelbox(1) (x) box(0.945): a piecewise-quadratic ramp
// spanning 1.945 device px in total, 1.469 px of it between the 3% and 97%
// coverage levels (the quadratic tails spend the rest below/above the pads).
// This renders real glyphs through the real ui_sdf pipeline — staged font,
// primitive generation, compiled shaders, executed render graph — and measures
// the edge footprint off the readback, because a CPU transcription of the ramp
// would only ever agree with itself.
//
// The discriminator is the count of intermediate-coverage samples per stem
// edge. An interval of length L sampled at integer positions with uniform
// subpixel phase contains E[count] = L sample points, so the bare exact-area
// ramp (L = 0.94 between the pads) yields mean <= 1.0 while the filtered ramp
// (L = 1.469) yields ~1.47. Glyph advances alone land the stems on a narrow,
// deterministic set of phases, so the render is repeated with the label
// shifted by 0.2 px steps: run origins are not snapped in x (SnapRunOriginY
// snaps y only), and the five strata pin the pooled mean to 1.469 +- 0.07 by
// stratification rather than by luck. The white paint is deliberate: the
// colour-keyed contrast boost is zero for white text (text_mask_gamma.glsl),
// so the channel read back IS the raw rasteriser coverage.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

using GameEngine::UITesting::IsolatedUIFixture;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Ten lowercase l's: identical straight stems. Per-render fractional
// margin-left offsets stratify the stem subpixel phases (see file comment).
constexpr char kStemXml[] = R"(<uielement id="root">
  <label id="stem">llllllllll</label>
</uielement>)";

// 96px: the stem is ~8 px wide, so its core reaches full coverage (a plateau)
// with room to spare on both sides of the transition bands.
constexpr char kStemCssFmt[] = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: #000000; }
#stem { font-family: Roboto; font-size: 96px; line-height: 120px; color: #ffffff; margin: 20px 20px 20px %spx; }
)";

constexpr const char* kPhaseMargins[] = {"20.0", "20.2", "20.4", "20.6", "20.8"};

constexpr uint32_t kTargetW = 800;
constexpr uint32_t kTargetH = 600;

float HalfToFloat(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
    uint32_t exponent = (value >> 10u) & 0x1Fu;
    uint32_t mantissa = value & 0x03FFu;

    uint32_t bits;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            bits = sign;
        }
        else
        {
            exponent = 1u;
            while ((mantissa & 0x0400u) == 0u)
            {
                mantissa <<= 1u;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
        }
    }
    else if (exponent == 0x1Fu)
    {
        bits = sign | 0x7F800000u | (mantissa << 13u);
    }
    else
    {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }

    float out;
    static_assert(sizeof(out) == sizeof(bits));
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Executes one UI render into a transient RGBA16F target and returns the red
// channel (white text over the pass's black clear: red == coverage), row-major.
std::vector<float> RenderCoverage(UIManager& ui)
{
    IDevice* dev = ui.GetDevice();
    if (!dev)
        return {};

    const size_t pixelCount = static_cast<size_t>(kTargetW) * kTargetH;
    const size_t outBytes = pixelCount * 8; // RGBA16F
    const BufferHandle readback = dev->CreateReadbackBuffer(outBytes, "SlugEdge.Readback");
    if (!readback.IsValid())
        return {};

    std::vector<float> coverage;
    {
        RenderGraph::RGResourcePool persistent(dev);
        RenderGraph::RGTransientPool transient(dev);
        RenderGraph::RGUploadRing ring(dev, 2, 262144);
        RenderGraph::RGFrame frame(dev, &persistent, &transient, &ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = kTargetW;
        td.height = kTargetH;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "SlugEdge.Target";
        const RenderGraph::RGTexture target = frame.CreateTexture("SlugEdge.Target", td);
        // Deliberately LinearSdr while the harness itself is flipped (#767):
        // this test measures the RASTERISER's reconstruction filter, and the
        // white-on-black linear render is the one configuration where the red
        // channel read back IS raw coverage. An encoded target would put the
        // OETF between the filter and the measurement and turn the 3%/97%
        // pads into different coverage cut points — a distorted instrument,
        // not a stronger test.
        if (!target.IsValid() || !ui.RenderRG(frame, target, UI::UITargetSpace::LinearSdr()))
            return {};

        frame.AddPass(
            "SlugEdge.Readback", PassPhase::kFinalize,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(target, RenderGraph::RGTextureRead::CopySrc);
                p.PreventCulling(); // consumed by the CPU, not the graph
            },
            [target, readback](RenderGraph::RGContext& ctx)
            {
                ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(target), 0, 0, readback,
                                                        kTargetW, kTargetH);
            });

        frame.Execute();
        dev->WaitForIdle();

        const void* mapped = dev->MapBuffer(readback);
        if (mapped)
        {
            const uint16_t* halves = static_cast<const uint16_t*>(mapped);
            coverage.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; ++i)
                coverage[i] = HalfToFloat(halves[i * 4]); // red channel
            dev->UnmapBuffer(readback);
        }
    }
    dev->DestroyBuffer(readback);
    return coverage;
}

// Intermediate coverage: clearly off the background and clearly short of the
// plateau. The 0.03/0.97 pads keep plateau noise and background bleed out of
// the count; the expected footprint below accounts for the ramp length they
// exclude.
bool Intermediate(float v)
{
    return v > 0.03f && v < 0.97f;
}

struct EdgeFootprints
{
    int Edges = 0;
    int IntermediateSamples = 0;
    int WidestEdge = 0;
};

// For every full-coverage stem core in the row (a run of >= 2 samples at
// plateau), counts the contiguous intermediate samples flanking it on each
// side — the sampled footprint of one edge transition.
void ScanRow(const std::vector<float>& cov, uint32_t rowStart, uint32_t width, EdgeFootprints& out)
{
    uint32_t x = 0;
    while (x < width)
    {
        if (cov[rowStart + x] < 0.97f)
        {
            ++x;
            continue;
        }
        uint32_t runEnd = x;
        while (runEnd + 1 < width && cov[rowStart + runEnd + 1] >= 0.97f)
            ++runEnd;
        if (runEnd > x) // plateau of >= 2 samples: a resolved stem core
        {
            int left = 0;
            for (uint32_t c = x; c > 0 && Intermediate(cov[rowStart + c - 1]); --c)
                ++left;
            int right = 0;
            for (uint32_t c = runEnd; c + 1 < width && Intermediate(cov[rowStart + c + 1]); ++c)
                ++right;
            out.Edges += 2;
            out.IntermediateSamples += left + right;
            out.WidestEdge = std::max({out.WidestEdge, left, right});
        }
        x = runEnd + 1;
    }
}

// Renders one phase stratum and accumulates its stem-edge footprints.
// Returns false only for the device-missing / font-missing skip cases,
// with `why` naming which.
bool MeasureStratum(const char* margin, EdgeFootprints& fp, int& strataSamples, std::string& why)
{
    char css[512];
    snprintf(css, sizeof(css), kStemCssFmt, margin);

    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kStemXml, css);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    if (!built)
        return true;
    if (fx.ResolvedFontFamily("stem").find("Roboto") == std::string::npos)
    {
        why = "Staged Roboto-Regular.ttf not found next to the test executable";
        return false;
    }

    const std::vector<float> cov = RenderCoverage(fx.Manager());
    EXPECT_FALSE(cov.empty()) << "UI render/readback produced no pixels";
    if (cov.empty())
        return true;

    // Rows carrying stem ink; measure only the vertically interior half of
    // that band so every scanned row crosses full-height stem cores, not the
    // curved stem ends or the baseline overshoot.
    int inkTop = -1;
    int inkBottom = -1;
    for (uint32_t y = 0; y < kTargetH; ++y)
    {
        for (uint32_t x = 0; x < kTargetW; ++x)
        {
            if (cov[static_cast<size_t>(y) * kTargetW + x] >= 0.97f)
            {
                if (inkTop < 0)
                    inkTop = static_cast<int>(y);
                inkBottom = static_cast<int>(y);
                break;
            }
        }
    }
    EXPECT_GE(inkTop, 0) << "no full-coverage ink found in the render";
    if (inkTop < 0)
        return true;
    const int bandH = inkBottom - inkTop + 1;
    EXPECT_GE(bandH, 20) << "ink band unexpectedly short for 96px stems";

    const int before = fp.IntermediateSamples;
    const int rowFirst = inkTop + bandH / 4;
    const int rowLast = inkBottom - bandH / 4;
    for (int y = rowFirst; y <= rowLast; ++y)
        ScanRow(cov, static_cast<uint32_t>(y) * kTargetW, kTargetW, fp);
    strataSamples = fp.IntermediateSamples - before;
    return true;
}

} // namespace

TEST(SlugEdgeProfile, StemEdgesCarryTheWidenedFilterFootprint)
{
    EdgeFootprints fp;
    std::vector<int> perStratum;
    for (const char* margin : kPhaseMargins)
    {
        int stratumSamples = 0;
        std::string why;
        if (!MeasureStratum(margin, fp, stratumSamples, why))
            GTEST_SKIP() << why;
        perStratum.push_back(stratumSamples);
    }

    ASSERT_GE(fp.Edges, 500) << "too few resolved stem edges — layout or font changed under the test";

    // The strata only stratify if the fractional margins actually shift the
    // stems: identical per-stratum totals across all five means x got snapped
    // somewhere and the phase coverage silently collapsed.
    const bool allIdentical =
        std::all_of(perStratum.begin(), perStratum.end(),
                    [&](int v) { return v == perStratum.front(); });
    EXPECT_FALSE(allIdentical)
        << "all phase strata measured identically — fractional x offsets are being snapped";

    const double meanFootprint =
        static_cast<double>(fp.IntermediateSamples) / static_cast<double>(fp.Edges);

    // Bare exact-area coverage spans 0.94 px between the 3%/97% pads, so its
    // phase-averaged footprint is <= ~1.0 samples per edge; the 0.945 px box
    // kernel widens the counted span to 1.469 px (1.945 total minus the
    // quadratic tails the pads exclude), i.e. a stratified mean of ~1.47.
    // 1.25 / 1.70 bound that with margin covering the 0.2-px stratification
    // granularity (+-0.07) and pad-threshold jitter on either side.
    EXPECT_GE(meanFootprint, 1.25) << "stem edges are not carrying the widened filter footprint";
    EXPECT_LE(meanFootprint, 1.70) << "edge transitions wider than the filter can produce — smearing";

    // A 1.469 px counted span can cover at most 2 integer sample positions.
    EXPECT_LE(fp.WidestEdge, 2) << "an edge transition spans more samples than the filter footprint allows";
}
