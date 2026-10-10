// CPU-only unit tests for the view-priority near-bias radius curve (CBTNearBias.h). No GPU
// device is needed — these pin the shape of the sub-linear altitude curve so a future retune
// is caught, and lock the PR #620 zoom-in acceptance case (a mid-distance feature must KEEP
// full detail as the camera descends toward it). The GPU demand/occupancy probes
// (CBTFarFieldDrainProbe, CBTSaturationRetess, CBTLookBackPriority) exercise the same curve
// through the real kernels; these tests bound the curve itself.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "CBTTerrain/CBTNearBias.h"

using namespace GameEngine::CBTTerrain;

namespace
{
// The pre-#620 linear curve, kept here purely as the CONTRAST baseline the new curve must beat
// on the zoom-in case. It is no longer used in production or the GPU probes.
float OldLinearNearR(float altitude)
{
    const float alt = std::max(altitude, 1.0f);
    return std::max(50.0f, 1.5f * alt);
}

// The fixed ground distance of the PR #620 acceptance feature, measured from the sub-camera
// point. It must stay inside the full-detail near disc through the whole 4000 m -> 1000 m
// descent under the new curve (and did NOT under the old one).
constexpr float kAcceptanceFeatureM = 3000.0f;
} // namespace

// Locks the exact tuned curve at the reference altitudes reported in the PR. A retune that
// moves these must update this table WITH the rationale — the table is the contract.
TEST(CBTNearBias, RadiiTableMatchesTunedCurve)
{
    struct Row { float Alt, NearR, FarR; };
    // nearR = 100 * sqrt(alt); farR = max(6 * alt, 2 * nearR).
    const Row kExpected[] = {
        {5.0f, 223.607f, 447.214f},   // 2*nearR wins the far radius (6*5=30 << 447)
        {50.0f, 707.107f, 1414.214f}, // 2*nearR wins (6*50=300 << 1414)
        {150.0f, 1224.745f, 2449.490f},
        {1000.0f, 3162.278f, 6324.555f}, // 2*nearR just wins (6*1000=6000 < 6325)
        {4000.0f, 6324.555f, 24000.0f},  // 6*alt wins the far radius (24000 > 12649)
    };
    for (const Row& r : kExpected)
    {
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(r.Alt);
        EXPECT_NEAR(nb.NearRadius, r.NearR, 0.05f) << "nearR at alt " << r.Alt;
        EXPECT_NEAR(nb.FarRadius, r.FarR, 0.05f) << "farR at alt " << r.Alt;
    }
}

// THE PR #620 acceptance. A feature 3000 m from the sub-camera point must stay INSIDE the
// full-detail near disc (nearR >= 3000) across the whole 4000 m -> 1000 m descent, so it keeps
// its tessellation as the camera zooms in. Sampled densely so a non-monotone dip is caught.
TEST(CBTNearBias, ZoomInKeepsMidFeatureDetailedThroughDescent)
{
    for (float alt = 4000.0f; alt >= 1000.0f; alt -= 50.0f)
    {
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(alt);
        EXPECT_GE(nb.NearRadius, kAcceptanceFeatureM)
            << "new curve: the " << kAcceptanceFeatureM << " m feature fell out of the full-detail "
            << "disc at alt " << alt << " (nearR " << nb.NearRadius << ") — it would coarsen on zoom-in";
    }
    // The bottom of the descent is the binding case: nearR(1000) = 3162 m > 3000 m (162 m margin).
    EXPECT_GT(ComputeNearBiasRadii(1000.0f).NearRadius, kAcceptanceFeatureM);
}

// The regression this fixes: the OLD linear curve pulled the same feature OUT of the disc as
// the camera descended (nearR(1000) = 1500 m < 3000 m), so it coarsened up to ~2.3x on zoom-in.
// This documents why the curve changed — if the old curve had also held the feature, no change
// would have been warranted.
TEST(CBTNearBias, OldLinearCurveCoarsenedTheFeatureOnDescent)
{
    // High up, both curves hold the feature (it starts inside the disc).
    EXPECT_GT(OldLinearNearR(4000.0f), kAcceptanceFeatureM);
    // Descending toward it, the old disc shrinks past the feature — the counterintuitive bug.
    EXPECT_LT(OldLinearNearR(1000.0f), kAcceptanceFeatureM)
        << "old linear curve should have coarsened the feature at alt 1000 (that was the bug)";
}

// Sub-linear means the disc shrinks SLOWLY on descent: over a 4x altitude drop the sqrt disc
// halves (ratio 0.5), where the old linear disc quartered (ratio 0.25). This is the mechanism
// that keeps mid-distance features detailed on zoom-in.
TEST(CBTNearBias, DiscHalvesPerFourfoldAltitudeDrop)
{
    const float nearHi = ComputeNearBiasRadii(4000.0f).NearRadius;
    const float nearLo = ComputeNearBiasRadii(1000.0f).NearRadius;
    EXPECT_NEAR(nearLo / nearHi, 0.5f, 0.001f);
    // Strictly slower-shrinking than the old linear curve (ratio 0.25 over the same 4x drop).
    const float oldRatio = OldLinearNearR(1000.0f) / OldLinearNearR(4000.0f);
    EXPECT_GT(nearLo / nearHi, oldRatio);
}

// Structural invariant the shader's smoothstep(nearR, farR, .) requires: the far radius must
// strictly exceed the near radius (by at least the 2x band) at every altitude.
TEST(CBTNearBias, FarRadiusIsAtLeastTwiceNearRadius)
{
    for (float alt : {1.0f, 5.0f, 50.0f, 150.0f, 500.0f, 1000.0f, 4000.0f, 20000.0f, 100000.0f})
    {
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(alt);
        EXPECT_GE(nb.FarRadius, 2.0f * nb.NearRadius - 0.05f) << "at alt " << alt;
        EXPECT_GT(nb.FarRadius, nb.NearRadius) << "at alt " << alt;
    }
}

// Boundedness at the extremes. The near disc must not blow up: at the surface the altitude
// clamp floors it at coef metres (100 m), and the high-altitude saturated pose (alt 4000) must
// stay within ~5% of the prior 6000 m disc so the alt-4000 R=50000 pool budget is not upset
// (the GPU probes verify the occupancy bound; this guards the CPU curve feeding them).
TEST(CBTNearBias, DiscStaysBoundedAtExtremes)
{
    // Surface / below-clamp: nearR floored at 100 m, never zero or negative.
    EXPECT_FLOAT_EQ(ComputeNearBiasRadii(0.0f).NearRadius, 100.0f);
    EXPECT_FLOAT_EQ(ComputeNearBiasRadii(1.0f).NearRadius, 100.0f);
    EXPECT_FLOAT_EQ(ComputeNearBiasRadii(-500.0f).NearRadius, 100.0f);
    // High-altitude saturated pose: within 5% of the prior 6000 m disc (no demand blow-up).
    const float nearHi = ComputeNearBiasRadii(4000.0f).NearRadius;
    EXPECT_LT(nearHi, 6000.0f * 1.06f);
    EXPECT_GT(nearHi, 6000.0f);
    // Monotone non-decreasing in altitude (a higher camera never gets a smaller disc).
    float prev = 0.0f;
    for (float alt = 1.0f; alt <= 50000.0f; alt *= 1.5f)
    {
        const float nearR = ComputeNearBiasRadii(alt).NearRadius;
        EXPECT_GE(nearR, prev) << "nearR decreased going up to alt " << alt;
        prev = nearR;
    }
}
