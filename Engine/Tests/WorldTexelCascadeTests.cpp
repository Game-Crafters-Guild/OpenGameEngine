// World-texel cascade fit: texel size is AUTHORED and the coverage is derived,
// inverting the usual relationship.
//
// Every other projection mode fits the camera's frustum slice and lets the texel
// size fall out of it (worldPerTexel = 2*halfExtent/resolution). That makes
// sharpness a function of resolution, camera rotation and split placement, and
// it is why the Stable-vs-Close trade exists at all: Stable buys a texel size
// that cannot move by bounding a rotating slice with a sphere, paying ~19%
// coarser texels than the mean AABB for the privilege.
//
// Authoring the texel size removes the trade rather than picking a side. The
// extent cannot breathe, so nothing needs to be made invariant, and the centre
// snap quantum becomes a CONSTANT — which is what welds the shadow lattice to a
// fixed world grid instead of to a grid that moves with the fit.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include <gtest/gtest.h>

#include <cmath>

using GameEngine::Engine::Renderer::CascadedShadowConfig;
using GameEngine::Engine::Renderer::ShadowMapRenderFeature;
using GameEngine::Engine::Renderer::ShadowProjection;

namespace
{
CascadedShadowConfig MakeConfig()
{
    CascadedShadowConfig cfg{};
    cfg.NumCascades = 4;
    cfg.Resolution = 2048;
    cfg.Projection = ShadowProjection::WorldTexel;
    cfg.Cascade0TexelSize = 0.02f; // 2 cm
    cfg.CascadeTexelRatio = 3.0f;
    cfg.MaxShadowDistance = 1000.0f;
    return cfg;
}
} // namespace

// The authored ladder: texel_i = texel0 * ratio^i, and nothing else feeds it.
TEST(WorldTexelCascade, TexelSizeIsTheAuthoredLadder)
{
    const CascadedShadowConfig cfg = MakeConfig();

    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 0), 0.02f);
    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 1), 0.06f);
    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 2), 0.18f);
    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 3), 0.54f);
}

// THE point of the slice. Resolution becomes a COVERAGE knob: halving it halves
// the reach and leaves sharpness untouched. Under every other mode halving the
// resolution doubles the texel size instead.
TEST(WorldTexelCascade, TexelSizeIsIndependentOfResolution)
{
    CascadedShadowConfig hi = MakeConfig();
    CascadedShadowConfig lo = MakeConfig();
    lo.Resolution = 1024;

    for (uint32_t i = 0; i < 4; ++i)
    {
        EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(hi, i),
                        ShadowMapRenderFeature::CascadeTexelSize(lo, i))
            << "cascade " << i << ": texel size must not depend on resolution";

        // ...and the coverage is what absorbs the change instead.
        EXPECT_NEAR(ShadowMapRenderFeature::CascadeHalfExtentForTexel(hi, i),
                    2.0f * ShadowMapRenderFeature::CascadeHalfExtentForTexel(lo, i), 1e-3f)
            << "cascade " << i << ": half the resolution must mean half the reach";
    }
}

// halfExtent = resolution * texelSize / 2, so worldPerTexel (2*halfExtent/res)
// returns exactly the authored size. This is the round-trip the shader relies on
// — ge_shadowPcssCascades[i].x is computed from the extent, not from the config.
TEST(WorldTexelCascade, HalfExtentRoundTripsToTheAuthoredTexelSize)
{
    const CascadedShadowConfig cfg = MakeConfig();

    for (uint32_t i = 0; i < 4; ++i)
    {
        const float halfExtent = ShadowMapRenderFeature::CascadeHalfExtentForTexel(cfg, i);
        const float worldPerTexel = 2.0f * halfExtent / static_cast<float>(cfg.Resolution);
        EXPECT_NEAR(worldPerTexel, ShadowMapRenderFeature::CascadeTexelSize(cfg, i), 1e-6f);
    }
}

// The split solver. A frustum slice [near, far] has a bounding sphere whose
// radius grows monotonically with far, so the reach of a cascade is the far
// distance at which that radius reaches the authored half-extent.
TEST(WorldTexelCascade, SolvedFarProducesTheRequestedSphereRadius)
{
    const float tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;
    const float nearDist = 0.1f;

    for (float radius : {2.0f, 10.0f, 50.0f, 200.0f})
    {
        const float far = ShadowMapRenderFeature::SolveSliceFarForRadius(
            nearDist, radius, tanHalfY, aspect, 10000.0f);
        ASSERT_GT(far, nearDist) << "radius " << radius;

        const float achieved =
            ShadowMapRenderFeature::SliceBoundingSphereRadius(nearDist, far, tanHalfY, aspect);
        // Solved to the radius it was asked for, within bisection tolerance.
        EXPECT_NEAR(achieved, radius, radius * 0.01f) << "radius " << radius;
    }
}

// Monotonicity is what makes the bisection valid; if it ever fails, the solver
// silently returns a wrong branch rather than erroring.
TEST(WorldTexelCascade, SliceSphereRadiusIsMonotonicInFar)
{
    const float tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;

    float prev = -1.0f;
    for (float far = 1.0f; far <= 500.0f; far *= 1.2f)
    {
        const float r =
            ShadowMapRenderFeature::SliceBoundingSphereRadius(0.1f, far, tanHalfY, aspect);
        EXPECT_GT(r, prev) << "far " << far;
        prev = r;
    }
}

// A cascade cannot reach further than its own box, so the achieved reach is a
// readout rather than a request: MaxShadowDistance can only SHORTEN it. An
// author who asks for 500 m and gets 90 m has been misled unless the number is
// visible, which is why the reach is reported rather than silently clamped.
TEST(WorldTexelCascade, MaxShadowDistanceOnlyShortensTheReach)
{
    const float tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;

    const float uncapped =
        ShadowMapRenderFeature::SolveSliceFarForRadius(0.1f, 50.0f, tanHalfY, aspect, 10000.0f);
    const float capped =
        ShadowMapRenderFeature::SolveSliceFarForRadius(0.1f, 50.0f, tanHalfY, aspect, 20.0f);

    EXPECT_GT(uncapped, 20.0f) << "the test is vacuous unless the uncapped reach exceeds the cap";
    EXPECT_FLOAT_EQ(capped, 20.0f);
}

// ── AUTO ladder: shadow distance as a single knob ──
//
// The complaint this answers: under a fitted projection, SplitLambda has to be
// re-tuned every time MaxShadowDistance moves, because the extents (and so the
// texel sizes) are derived from the fit. AUTO removes the second knob by
// solving the ladder to span whatever distance is asked for.

TEST(WorldTexelCascade, AutoLadderSpansTheRequestedDistance)
{
    CascadedShadowConfig cfg = MakeConfig();
    cfg.Cascade0TexelSize = 0.0f; // AUTO
    const float tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;

    for (float distance : {50.0f, 200.0f, 800.0f})
    {
        const float texel0 = ShadowMapRenderFeature::SolveCascade0TexelForDistance(
            cfg, 0.1f, distance, tanHalfY, aspect);
        const float reach =
            ShadowMapRenderFeature::CascadeChainReach(cfg, texel0, 0.1f, tanHalfY, aspect, 1e9f);
        EXPECT_NEAR(reach, distance, distance * 0.02f) << "distance " << distance;
    }
}

// The property the user actually asked for: halve the shadow distance and the
// texels get FINER rather than needing a lambda re-tune. Under a fitted
// projection this relationship is what SplitLambda exists to hand-balance.
TEST(WorldTexelCascade, ShorterDistanceAutomaticallySharpens)
{
    CascadedShadowConfig cfg = MakeConfig();
    cfg.Cascade0TexelSize = 0.0f;
    const float tanHalfY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;

    const float farTexel =
        ShadowMapRenderFeature::SolveCascade0TexelForDistance(cfg, 0.1f, 400.0f, tanHalfY, aspect);
    const float nearTexel =
        ShadowMapRenderFeature::SolveCascade0TexelForDistance(cfg, 0.1f, 100.0f, tanHalfY, aspect);

    EXPECT_LT(nearTexel, farTexel) << "a shorter range must spend the same texels on less ground";
    // Reach scales roughly linearly with the base texel, so a 4x shorter range
    // should land near 4x finer. Loose bound: the near plane and the sphere
    // formula's branch make it not exactly linear.
    EXPECT_NEAR(farTexel / nearTexel, 4.0f, 1.0f);
}

// AUTO must stay opt-out: an explicitly authored base is used verbatim, so a
// project that wants a FIXED physical texel size still gets one and distance
// only changes coverage.
TEST(WorldTexelCascade, ExplicitLadderOverridesAuto)
{
    CascadedShadowConfig cfg = MakeConfig();
    cfg.Cascade0TexelSize = 0.05f;
    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 0), 0.05f);
    EXPECT_FLOAT_EQ(ShadowMapRenderFeature::CascadeTexelSize(cfg, 1), 0.15f);
}
