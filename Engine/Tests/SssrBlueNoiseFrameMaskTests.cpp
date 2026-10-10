// Frame-wrap contract on GE_BlueNoise (ScreenSpaceReflections/sssr_common.glsl).
//
// The per-frame Cranley-Patterson rotation is `fract(base + R2 * float(frame))`.
// Computed in fp32, the product's ULP grows with its exponent, so the reachable
// rotation set shrinks as the frame counter climbs: the rotation is quantised to
// about 1/ulp(R2*frame) distinct values. That makes the noise quality a function
// of how long the session has been running — an artifact with no scene cause,
// which appears only after minutes of uptime and so never shows up in a short
// A/B. Wrapping the counter to a power-of-two period before the float conversion
// pins the product inside one exponent range and the rotation set stays full.
//
// The numeric arms below run the shader's own expression in float, once with the
// mask and once without, so the test discriminates: the unmasked arm reproduces
// the collapse rather than assuming it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>

#include "SssrShaderSource.h"

using GE::Tests::ReadSssrShaderSource;

namespace
{

// The R2 low-discrepancy pair in sssr_common.glsl. Only the x component is needed
// to count reachable rotations: fract() collapses per component independently.
constexpr float kR2X = 0.7548776662f;

// Mirrors the shader's rotation term for one component. `float` throughout, which
// is the whole point — the defect lives in fp32 rounding, so a double-precision
// mirror would silently pass.
float RotationTerm(std::uint32_t frame)
{
    const float scaled = kR2X * static_cast<float>(frame);
    return scaled - std::floor(scaled);
}

// Distinct rotations reachable over a window of consecutive frames starting at
// `firstFrame`. `period` of 0 means "no wrap", i.e. the unmasked expression.
std::size_t DistinctRotations(std::uint32_t firstFrame, std::uint32_t windowFrames,
                              std::uint32_t period)
{
    std::set<float> values;
    for (std::uint32_t i = 0; i < windowFrames; ++i)
    {
        const std::uint32_t frame = firstFrame + i;
        values.insert(RotationTerm(period == 0u ? frame : (frame & (period - 1u))));
    }
    return values.size();
}

constexpr std::uint32_t kShaderPeriod = 256u;

// A window comfortably wider than the period, so a wrapped counter has room to
// reach every rotation it can reach.
constexpr std::uint32_t kWindow = 2048u;

} // namespace

// The shipped shader must wrap the counter, and must do it on the integer before
// the float conversion. Wrapping after the multiply would not help: the precision
// is already gone by then.
TEST(SssrBlueNoiseFrameMask, ShaderWrapsTheFrameCounterBeforeTheFloatConversion)
{
    const std::string source = ReadSssrShaderSource("ScreenSpaceReflections/sssr_common.glsl");
    ASSERT_FALSE(source.empty()) << "sssr_common.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t at = source.find("vec2 GE_BlueNoise(");
    ASSERT_NE(at, std::string::npos) << "GE_BlueNoise missing from sssr_common.glsl";
    const std::string body = source.substr(at);

    EXPECT_NE(body.find("float(frame & (kBlueNoiseFramePeriod - 1u))"), std::string::npos)
        << "the frame counter must be masked INSIDE the float() conversion: an unbounded "
           "fp32 product starves on ULP and the rotation set collapses with session age";
    EXPECT_EQ(body.find("* float(frame)"), std::string::npos)
        << "an unmasked float(frame) is the collapse defect";
}

// Both blue-noise consumers must inherit the wrap. They pass different counters
// (the ray direction uses the frame index, the march start offset uses frame+17),
// so masking at the call sites is the version of this fix that rots — the helper
// is the one place that cannot be forgotten by a future third caller.
TEST(SssrBlueNoiseFrameMask, WrapLivesInTheHelperSoEveryCallSiteInheritsIt)
{
    const std::string common = ReadSssrShaderSource("ScreenSpaceReflections/sssr_common.glsl");
    const std::string intersect = ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    ASSERT_FALSE(common.empty());
    ASSERT_FALSE(intersect.empty());

    // The period is declared once, next to the helper that applies it.
    EXPECT_NE(common.find("const uint kBlueNoiseFramePeriod = 256u;"), std::string::npos)
        << "the wrap period must be a named constant, not an inline literal mask";

    // Every call site reaches the rotation only through the helper.
    EXPECT_EQ(intersect.find("0.7548776662"), std::string::npos)
        << "a call site must not open-code the R2 rotation and bypass the helper's wrap";
    EXPECT_NE(intersect.find("GE_BlueNoise(uBlueNoise, uvec2(pixel), sssr.frameIndex)"),
              std::string::npos)
        << "the VNDF sample must come from the shared helper";
    EXPECT_NE(intersect.find("GE_BlueNoise(uBlueNoise, uvec2(pixel), sssr.frameIndex + 17u)"),
              std::string::npos)
        << "the march start offset must come from the shared helper (same collapse class: "
           "float(frame + 17u) starves on ULP exactly as float(frame) does)";
}

// The defect, reproduced. Without the wrap the reachable rotation set shrinks as
// the counter climbs; this is the arm that makes the fix falsifiable rather than
// asserted.
TEST(SssrBlueNoiseFrameMask, UnmaskedCounterCollapsesAsTheSessionAges)
{
    // Early frames: fp32 still resolves every frame to its own rotation.
    EXPECT_EQ(DistinctRotations(0u, kWindow, 0u), kWindow)
        << "at session start the unmasked expression is not yet degraded, so a test that "
           "only sampled early frames would see no defect at all";

    // The decay is monotone in the counter's magnitude.
    const std::size_t at17k = DistinctRotations(17000u, kWindow, 0u);
    const std::size_t at250k = DistinctRotations(250000u, kWindow, 0u);
    const std::size_t at1m = DistinctRotations(1000000u, kWindow, 0u);

    EXPECT_LT(at250k, at17k) << "rotation set must shrink between frame 17k and 250k";
    EXPECT_LT(at1m, at250k) << "rotation set must shrink between frame 250k and 1M";

    // The magnitudes the frame-250k measurement reported: one exponent step per
    // doubling of the product, so 2048 consecutive frames reach only 64 rotations.
    EXPECT_EQ(at250k, 64u) << "frame 250k must reach exactly 64 rotations in fp32";
    EXPECT_EQ(at1m, 16u) << "frame 1M must reach exactly 16 rotations in fp32";
}

// The fix, verified against the same instrument: the wrapped counter reaches its
// full period at every session age, including ages where the unmasked expression
// has already collapsed.
TEST(SssrBlueNoiseFrameMask, WrappedCounterHoldsItsFullPeriodAtEverySessionAge)
{
    for (std::uint32_t firstFrame : {0u, 17000u, 250000u, 1000000u, 4000000000u})
    {
        const std::size_t distinct = DistinctRotations(firstFrame, kWindow, kShaderPeriod);
        EXPECT_EQ(distinct, kShaderPeriod)
            << "wrapped rotation set must be the full period at frame " << firstFrame
            << "; got " << distinct;
    }
}

// The period must outlast the temporal window that consumes the sequence, or the
// resolve would integrate a repeating rotation and the wrap would trade one
// artifact for another. Feedback weight w averages ~1/(1-w) frames: 0.86 at the
// mirror end is ~7 frames, 0.97 at the rough end is ~33.
TEST(SssrBlueNoiseFrameMask, PeriodComfortablyExceedsTheTemporalWindow)
{
    const std::string temporal = ReadSssrShaderSource("ScreenSpaceReflections/sssr_history_policy.glsl");
    ASSERT_FALSE(temporal.empty());
    ASSERT_NE(temporal.find("mix(0.86f, 0.97f, roughness)"), std::string::npos)
        << "temporal feedback weights changed — re-derive the window this period must exceed";

    constexpr double kSlowestFeedback = 0.97;
    const double windowFrames = 1.0 / (1.0 - kSlowestFeedback);
    EXPECT_GT(static_cast<double>(kShaderPeriod), windowFrames * 4.0)
        << "wrap period " << kShaderPeriod << " must exceed the ~" << windowFrames
        << "-frame accumulation window by a wide margin";
}
