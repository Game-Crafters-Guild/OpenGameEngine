// Pins the CPU half of the colour-keyed text coverage correction: the
// target-space-to-arm translation and clamping that decide what the SDF shader
// is pushed.
//
// The correction curve itself lives in Shaders/UI/text_mask_gamma.glsl; its
// maths is pinned by TextMaskGammaGoldenTests against the blend-model
// definition (forward simulation), and its on-GPU behaviour against the
// measured transfer function. Tests here stay on UIManager's static resolver
// so they need no device.

#include <gtest/gtest.h>

#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"

using GameEngine::UIManager;
using GameEngine::UI::UITargetSpace;

namespace
{
// Every target space a host can declare today. A new Kind fails the
// static_assert below and must be added here WITH a decision about which
// retarget arm it selects — that decision is the #767 flip's one declared
// value, and this file is where it stops being dark.
const UITargetSpace kAllConstructibleSpaces[] = {
    UITargetSpace::LinearSdr(),
    UITargetSpace::EncodedSrgb(),
    UITargetSpace::HdrPq(),
    UITargetSpace::Hlg(),
    UITargetSpace::ScRgb(),
};
static_assert(UITargetSpace::kKindCount == 5,
              "New UITargetSpace: add it to kAllConstructibleSpaces and decide its "
              "retarget arm in the pins below.");
} // namespace

TEST(TextCoverageCorrection, KnobsPassThroughForEveryTargetSpace)
{
    // blendGamma names the TARGET space and passes through un-gated in both
    // arms — each arm's identity sits at its own blend space by mathematics
    // (linear arm at 0, encoded arm at 1), never by a forced constant. No
    // gate rewrites either knob on the way to the shader.
    for (const auto space : kAllConstructibleSpaces)
    {
        const auto resolved = UIManager::ResolveTextCoverageConstants(0.8f, 1.0f, space);
        EXPECT_FLOAT_EQ(0.8f, resolved.Contrast);
        EXPECT_FLOAT_EQ(1.0f, resolved.BlendGamma);
    }
}

// The #767 flip's one declared value, seen from the text stack: exactly the
// EncodedSrgb target selects the Skia-direction retarget arm. At the shipped
// blendGamma 0 (linear target) that arm is the FULL Skia correction —
// area-exact ink out of an encoded source-over, Chrome's SK_GAMMA_SRGB
// configuration (the measured stake of NOT selecting it: a naive flip is
// +34.9% thin; an unretargeted boost 1.16-1.34x heavy). Every linear-light
// attachment — LinearSdr and the three HDR flavours — stays on the linear
// arm. The arm's maths is pinned in TextMaskGammaGoldenTests.
TEST(TextCoverageCorrection, OnlyTheEncodedTargetSelectsTheEncodedArm)
{
    for (const auto space : kAllConstructibleSpaces)
    {
        const auto resolved = UIManager::ResolveTextCoverageConstants(1.0f, 0.0f, space);
        const int expected = space == UITargetSpace::EncodedSrgb() ? 1 : 0;
        EXPECT_EQ(expected, resolved.BlendSpaceEncoded)
            << "space=" << GameEngine::UI::ToString(space);
    }
}

TEST(TextCoverageCorrection, DefaultKnobsReduceToContrastOnly)
{
    // The shipped default: contrast 1.0 (Skia's SK_GAMMA_CONTRAST as Chrome
    // ships it on Windows) against a linear target on a linear attachment —
    // identity retarget, so the colour-keyed boost is the entire correction.
    const auto resolved =
        UIManager::ResolveTextCoverageConstants(1.0f, 0.0f, UITargetSpace::LinearSdr());
    EXPECT_FLOAT_EQ(1.0f, resolved.Contrast);
    EXPECT_FLOAT_EQ(0.0f, resolved.BlendGamma);
    EXPECT_EQ(0, resolved.BlendSpaceEncoded);
}

TEST(TextCoverageCorrection, ResolveClampsOutOfRangeInputs)
{
    const auto low =
        UIManager::ResolveTextCoverageConstants(-3.0f, -3.0f, UITargetSpace::LinearSdr());
    EXPECT_FLOAT_EQ(0.0f, low.Contrast);
    EXPECT_FLOAT_EQ(0.0f, low.BlendGamma);

    const auto high =
        UIManager::ResolveTextCoverageConstants(3.0f, 9.0f, UITargetSpace::LinearSdr());
    EXPECT_FLOAT_EQ(1.0f, high.Contrast);
    EXPECT_FLOAT_EQ(4.0f, high.BlendGamma);
}

// Subpixel RGB text AA activation gate. Same contract as the coverage
// resolver above: pure, static, pinned here so the fallback matrix cannot
// drift — a failed condition must resolve to the grayscale pipeline (same
// draws), never to dropped or differently-composed content.

// The SDF shader's outputEncoding convention (see SdfPushConstants):
// 0 = SDR linear, 1 = SDR encoded bytes, 2 = HDR10 PQ, 3 = HLG, 4 = scRGB.
namespace
{
constexpr int kEncodingSdr = 0;
constexpr int kEncodingSdrEncoded = 1;
constexpr int kEncodingHdr10Pq = 2;
constexpr int kEncodingHlg = 3;
constexpr int kEncodingScRgb = 4;
} // namespace

TEST(TextSubpixelResolve, ActiveOnlyWhenAllConditionsHold)
{
    EXPECT_TRUE(UIManager::ResolveTextSubpixelActive(
        /*enabled=*/true, kEncodingSdr, /*dualSourceBlending=*/true, /*pipelineAvailable=*/true));
    // The encoded SDR target is subpixel-native: per-channel source-over on
    // encoded bytes is the ClearType/Skia model itself.
    EXPECT_TRUE(UIManager::ResolveTextSubpixelActive(
        /*enabled=*/true, kEncodingSdrEncoded, /*dualSourceBlending=*/true,
        /*pipelineAvailable=*/true));
}

TEST(TextSubpixelResolve, TheSettingIsOptIn)
{
    EXPECT_FALSE(UIManager::ResolveTextSubpixelActive(
        /*enabled=*/false, kEncodingSdr, /*dualSourceBlending=*/true, /*pipelineAvailable=*/true));
}

TEST(TextSubpixelResolve, EveryHdrOutputModeFallsBackToGrayscale)
{
    // Fringe colours are computed for direct SDR presentation; any HDR
    // transfer/tone step downstream re-colours them (Windows itself drops
    // ClearType in HDR). PQ, HLG and scRGB must all resolve grayscale.
    for (int encoding : {kEncodingHdr10Pq, kEncodingHlg, kEncodingScRgb})
    {
        EXPECT_FALSE(UIManager::ResolveTextSubpixelActive(
            /*enabled=*/true, encoding, /*dualSourceBlending=*/true, /*pipelineAvailable=*/true))
            << "outputEncoding=" << encoding;
    }
}

TEST(TextSubpixelResolve, RequiresDeviceDualSourceBlending)
{
    EXPECT_FALSE(UIManager::ResolveTextSubpixelActive(
        /*enabled=*/true, kEncodingSdr, /*dualSourceBlending=*/false, /*pipelineAvailable=*/true));
}

TEST(TextSubpixelResolve, RequiresTheStagedShaderVariant)
{
    EXPECT_FALSE(UIManager::ResolveTextSubpixelActive(
        /*enabled=*/true, kEncodingSdr, /*dualSourceBlending=*/true, /*pipelineAvailable=*/false));
}
