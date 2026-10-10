// Unit tests for the pure prop-coverage-floor math (PropCoverageFloor.h): NDC
// coverage, the resolution-independent pixel->NDC floor conversion, and the
// hysteresis cull decision a future scatter floor block must mirror. The math
// currently feeds the GE_PROP_CENSUS sizing diagnostic only. No device.

#include "Engine/Rendering/PropCoverageFloor.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine::Rendering;

namespace
{

// The threshold-pair derivation in one place, so the frame-sequence tests
// below exercise exactly the (floor, admit) pair a scatter floor would use.
struct FloorParams
{
    float floorNdc;
    float admitNdc;
};

FloorParams MakeParams(float floorPx)
{
    FloorParams p{};
    p.floorNdc = CoverageFloorNdc(floorPx, kCoverageFloorReferenceHeightPx);
    p.admitNdc = CoverageFloorAdmitNdc(p.floorNdc, kCoverageFloorHysteresis);
    return p;
}

} // namespace

TEST(PropCoverageFloor, CoverageNdcMatchesScatterLodIdiom)
{
    // coverage = r * projScaleY / dist — the ge_SelectLOD idiom pre-bias.
    EXPECT_FLOAT_EQ(CoverageNdc(1.0f, 1.0f, 100.0f), 0.01f);
    EXPECT_FLOAT_EQ(CoverageNdc(2.0f, 1.5f, 300.0f), 0.01f);
    // Distance clamps at 1e-4 so a camera-coincident center cannot divide by 0.
    EXPECT_FLOAT_EQ(CoverageNdc(1.0f, 1.0f, 0.0f), CoverageNdc(1.0f, 1.0f, 1e-4f));
}

TEST(PropCoverageFloor, FloorNdcConversionIsDiameterOverHeight)
{
    // A sphere of NDC radius r projects to r * H pixels of DIAMETER, so the
    // 1.5 px diameter floor at the 1080 reference height is 1.5/1080 NDC radius.
    EXPECT_FLOAT_EQ(CoverageFloorNdc(1.5f, 1080.0f), 1.5f / 1080.0f);
    // Disabled for non-positive floor or degenerate height.
    EXPECT_EQ(CoverageFloorNdc(0.0f, 1080.0f), 0.0f);
    EXPECT_EQ(CoverageFloorNdc(-2.0f, 1080.0f), 0.0f);
    EXPECT_EQ(CoverageFloorNdc(1.5f, 0.0f), 0.0f);
    EXPECT_EQ(CoverageFloorNdc(1.5f, 1.0f), 0.0f);
}

TEST(PropCoverageFloor, FloorIsResolutionIndependent)
{
    // The floor is denominated against the REFERENCE height, so the same
    // NDC floor applies at 1080p and 4K: a prop covering 2/1080 of the screen
    // height survives everywhere; 1/1080 is culled everywhere.
    const FloorParams p = MakeParams(1.5f);
    const float cov2px1080 = 2.0f / 1080.0f;
    const float cov1px1080 = 1.0f / 1080.0f;
    EXPECT_FALSE(CoverageFloorCulls(cov2px1080, p.floorNdc, kCoverageFloorHysteresis, false));
    EXPECT_TRUE(CoverageFloorCulls(cov1px1080, p.floorNdc, kCoverageFloorHysteresis, false));
}

TEST(PropCoverageFloor, AdmitThresholdAppliesHysteresisBand)
{
    EXPECT_FLOAT_EQ(CoverageFloorAdmitNdc(0.001f, 0.6f), 0.0016f);
    // Negative hysteresis clamps to zero: admit == floor, band collapses.
    EXPECT_FLOAT_EQ(CoverageFloorAdmitNdc(0.001f, -1.0f), 0.001f);
    EXPECT_FLOAT_EQ(CoverageFloorAdmitNdc(0.001f, 0.0f), 0.001f);
}

TEST(PropCoverageFloor, DisabledFloorNeverCulls)
{
    EXPECT_FALSE(CoverageFloorCulls(0.0f, 0.0f, kCoverageFloorHysteresis, false));
    EXPECT_FALSE(CoverageFloorCulls(0.0f, 0.0f, kCoverageFloorHysteresis, true));
    EXPECT_FALSE(CoverageFloorCulls(1e-6f, -1.0f, kCoverageFloorHysteresis, true));
}

TEST(PropCoverageFloor, CullDecisionTruthTable)
{
    const float floorNdc = 1.5f / 1080.0f;
    const float hyst     = 0.6f;
    const float admit    = CoverageFloorAdmitNdc(floorNdc, hyst);

    // Drawn last frame: cull iff below the base floor.
    EXPECT_FALSE(CoverageFloorCulls(floorNdc * 1.01f, floorNdc, hyst, false));
    EXPECT_FALSE(CoverageFloorCulls(floorNdc, floorNdc, hyst, false)); // boundary draws
    EXPECT_TRUE(CoverageFloorCulls(floorNdc * 0.99f, floorNdc, hyst, false));

    // Culled last frame: stay culled through the whole band [floor, admit).
    EXPECT_TRUE(CoverageFloorCulls(floorNdc * 0.99f, floorNdc, hyst, true));
    EXPECT_TRUE(CoverageFloorCulls(floorNdc, floorNdc, hyst, true));
    EXPECT_TRUE(CoverageFloorCulls(admit * 0.99f, floorNdc, hyst, true));
    EXPECT_FALSE(CoverageFloorCulls(admit, floorNdc, hyst, true)); // boundary re-admits
    EXPECT_FALSE(CoverageFloorCulls(admit * 1.01f, floorNdc, hyst, true));
}

TEST(PropCoverageFloor, ZeroHysteresisCollapsesTheBand)
{
    const float floorNdc = 0.002f;
    EXPECT_FALSE(CoverageFloorCulls(floorNdc, floorNdc, 0.0f, true));
    EXPECT_TRUE(CoverageFloorCulls(floorNdc * 0.999f, floorNdc, 0.0f, true));
}

TEST(PropCoverageFloor, DollyThroughThresholdTransitionsExactlyOnceEachWay)
{
    // Simulate a slow dolly: coverage ramps down through the band, then back
    // up. The state machine must flip drawn->culled exactly once on the way
    // out and culled->drawn exactly once on the way in — no oscillation while
    // coverage sits inside [floor, admit).
    const FloorParams p = MakeParams(kMinPropCoveragePx);

    std::vector<float> ramp;
    const float start = p.admitNdc * 2.0f;
    const float end   = p.floorNdc * 0.5f;
    const int   steps = 200;
    for (int i = 0; i <= steps; ++i)
        ramp.push_back(start + (end - start) * (static_cast<float>(i) / steps));
    for (int i = steps; i >= 0; --i)
        ramp.push_back(start + (end - start) * (static_cast<float>(i) / steps));

    bool culled      = false;
    int  transitions = 0;
    for (float cov : ramp)
    {
        const bool next =
            CoverageFloorCulls(cov, p.floorNdc, kCoverageFloorHysteresis, culled);
        if (next != culled)
            ++transitions;
        culled = next;
    }
    EXPECT_EQ(transitions, 2);
    EXPECT_FALSE(culled); // back above admit at the end
}

TEST(PropCoverageFloor, JitterInsideTheBandNeverPops)
{
    // Coverage jitters around the base floor (±20%), the classic shimmer case
    // hysteresis exists for. Once culled, the instance must stay culled: the
    // jitter never reaches admit = floor * 1.6.
    const FloorParams p = MakeParams(kMinPropCoveragePx);
    bool culled = false;
    int  flips  = 0;
    for (int i = 0; i < 1000; ++i)
    {
        const float jitter = 0.8f + 0.4f * (static_cast<float>(i % 7) / 6.0f);
        const bool next = CoverageFloorCulls(p.floorNdc * jitter, p.floorNdc,
                                             kCoverageFloorHysteresis, culled);
        if (next != culled)
            ++flips;
        culled = next;
    }
    EXPECT_LE(flips, 1); // at most the single initial drawn->culled flip
    EXPECT_TRUE(culled);
}

