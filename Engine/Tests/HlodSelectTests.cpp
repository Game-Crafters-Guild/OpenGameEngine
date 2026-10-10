// HLOD runtime switch decision (design v0.2 §5.2). Pure coverage math (must
// match ge_SelectLOD) + the per-cluster hysteresis state machine.

#include "Assets/HlodSelect.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace GameEngine;
using namespace GameEngine::Hlod;

namespace {

ViewLod MakeView(float camZ, float projScaleY, float bias = 0.0f) {
    ViewLod v;
    v.CameraPos[0] = 0.0f;
    v.CameraPos[1] = 0.0f;
    v.CameraPos[2] = camZ;
    v.ProjScaleY = projScaleY;
    v.LodBiasGlobal = bias;
    return v;
}

} // namespace

TEST(HlodSelect, CoverageMatchesGeSelectLODFormula) {
    // coverage = radius * projScaleY / dist * exp2(bias)
    ViewLod v = MakeView(0.0f, 1.5f, 0.0f);
    const float center[3] = {0.0f, 0.0f, 100.0f};
    const float radius = 10.0f;
    const float expected = radius * 1.5f / 100.0f; // exp2(0) = 1
    EXPECT_NEAR(ClusterCoverage(v, center, radius), expected, 1e-5f);

    // Farther -> smaller coverage.
    const float far[3] = {0.0f, 0.0f, 400.0f};
    EXPECT_LT(ClusterCoverage(v, far, radius), ClusterCoverage(v, center, radius));

    // Global bias scales by exp2.
    ViewLod biased = MakeView(0.0f, 1.5f, 1.0f); // exp2(1) = 2
    EXPECT_NEAR(ClusterCoverage(biased, center, radius), expected * 2.0f, 1e-5f);
}

TEST(HlodSelect, LodDisabledKeepsMembers) {
    ViewLod ortho = MakeView(0.0f, 0.0f); // projScaleY <= 0
    const float center[3] = {0.0f, 0.0f, 1000.0f};
    EXPECT_EQ(ClusterCoverage(ortho, center, 10.0f), kAlwaysMembersCoverage);

    SwitchConfig cfg = MakeSwitchConfig(0.08f, 0.25f);
    // kAlwaysMembersCoverage is far above any enter threshold -> never proxy.
    EXPECT_FALSE(UpdateProxyActive(false, kAlwaysMembersCoverage, cfg));
    EXPECT_FALSE(UpdateProxyActive(true, kAlwaysMembersCoverage, cfg));
}

TEST(HlodSelect, HysteresisEntersAndExitsAtDistinctThresholds) {
    SwitchConfig cfg = MakeSwitchConfig(0.08f, 0.25f);
    EXPECT_FLOAT_EQ(cfg.EnterCoverage, 0.08f);
    EXPECT_FLOAT_EQ(cfg.ExitCoverage, 0.08f * 1.25f); // 0.10

    // Not proxy yet; in the band (between enter and exit) stays members.
    EXPECT_FALSE(UpdateProxyActive(false, 0.09f, cfg));
    // Drops below enter -> proxy engages.
    EXPECT_TRUE(UpdateProxyActive(false, 0.07f, cfg));
    // Once proxy, rising into the band keeps it proxy (no thrash).
    EXPECT_TRUE(UpdateProxyActive(true, 0.09f, cfg));
    // Rises past exit -> members restored.
    EXPECT_FALSE(UpdateProxyActive(true, 0.11f, cfg));
}

TEST(HlodSelect, zeroHysteresisCollapsesToSingleThreshold) {
    SwitchConfig cfg = MakeSwitchConfig(0.05f, 0.0f);
    EXPECT_FLOAT_EQ(cfg.EnterCoverage, cfg.ExitCoverage);
    EXPECT_TRUE(UpdateProxyActive(false, 0.049f, cfg));
    EXPECT_FALSE(UpdateProxyActive(true, 0.051f, cfg));
}
