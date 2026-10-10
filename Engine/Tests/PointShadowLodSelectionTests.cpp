// Headless tests for point-shadow face LOD selection — the light-relative
// substitution that makes the atlas render cache sound.
//
// The contract under test: a cube face selects LODs as if the LIGHT were the
// camera and the face's tile were the viewport. Nothing camera- or viewport-
// derived may reach the params, because the atlas cache keys on these knobs and
// replays depth rasterized under them.

#include "Engine/Rendering/PointShadowLodSelection.h"

#include "Mathematics/MatrixOps.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
using GameEngine::Engine::Renderer::kPointShadowFaceProjScaleY;
using GameEngine::Engine::Renderer::MakePointFaceLodParams;
using GameEngine::Engine::Renderer::PointShadowLodKey;
using GameEngine::Mathematics::Vector3;

constexpr uint32_t kLodAuto = 0xFFFFFFFFu;
constexpr uint32_t kTileHigh = 1024u; // kPointAtlasTileResolution (tier High/Inherit)
constexpr uint32_t kTileLow = 256u;   // tier Low
constexpr float kBudgetPx = 10.0f;
constexpr float kSkinnedScale = 0.75f;

PointShadowLodKey Knobs(float bias = 0.0f, uint32_t forceLevel = kLodAuto,
                        float budgetPx = kBudgetPx, float skinnedScale = kSkinnedScale)
{
    PointShadowLodKey k{};
    k.Bias = bias;
    k.ForceLevel = forceLevel;
    k.ErrorBudgetPx = budgetPx;
    k.SkinnedBudgetScale = skinnedScale;
    return k;
}

// The scatter's coverage metric, reproduced from draw_command_scatter.comp
// (ge_SelectLOD) so these tests compare against what the GPU actually computes
// rather than against a restatement of the CPU side.
float ScatterCoverage(const GameEngine::Rendering::GPUDrawStreamBuilder::ViewLODParams& p,
                      const Vector3& casterCenter, float casterRadius)
{
    const float dx = p.cameraPos[0] - casterCenter.x;
    const float dy = p.cameraPos[1] - casterCenter.y;
    const float dz = p.cameraPos[2] - casterCenter.z;
    const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-4f);
    return casterRadius * p.projScaleY / dist * std::exp2(p.lodBiasGlobal);
}
} // namespace

// Pins kPointShadowFaceProjScaleY against the projection builder the faces are
// actually built with. This catches a convention change in
// MakePerspectiveLH_ZO_ReverseZ, and shows the focal is independent of near/far
// (so a light's range can move without moving selection). It does NOT catch a
// change to the 90° face FOV itself — that value is authored in
// PopulatePointShadowGeometry and must be changed here in the same edit.
TEST(PointShadowLodSelection, PointFaceProjScaleYMatchesFaceProjection)
{
    constexpr float kPi = 3.14159265358979323846f;
    for (const auto [nearPlane, farPlane] : {std::pair{0.025f, 10.0f}, std::pair{0.025f, 500.0f},
                                             std::pair{1.0f, 4.0f}})
    {
        const GameEngine::Mathematics::Matrix4x4 faceProj =
            GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(0.5f * kPi, 1.0f, nearPlane,
                                                                   farPlane);
        // Data()[5] == column-major proj[1][1], the focal term ge_SelectLOD reads
        // as projScaleY (the same element RenderServices reads as proj[5]).
        EXPECT_NEAR(std::fabs(faceProj.Data()[5]), kPointShadowFaceProjScaleY, 1e-6f)
            << "near " << nearPlane << " far " << farPlane;
    }
}

TEST(PointShadowLodSelection, SelectsFromTheLightNotTheCamera)
{
    // The whole point: the params carry the LIGHT's position. There is no camera
    // parameter to pass, so no camera term can reach selection.
    const Vector3 lightPos{12.0f, 3.5f, -40.0f};
    const auto p = MakePointFaceLodParams(lightPos, kTileHigh, Knobs());
    EXPECT_FLOAT_EQ(p.cameraPos[0], lightPos.x);
    EXPECT_FLOAT_EQ(p.cameraPos[1], lightPos.y);
    EXPECT_FLOAT_EQ(p.cameraPos[2], lightPos.z);
}

TEST(PointShadowLodSelection, CoverageIsAngularHalfExtentSeenFromTheLight)
{
    // With projScaleY == 1 the scatter metric reduces to radius / distance-to-light
    // — the caster's angular half-extent in the face's NDC, which is exactly the
    // fraction of the shadow map it occupies.
    const Vector3 lightPos{0.0f, 0.0f, 0.0f};
    const auto p = MakePointFaceLodParams(lightPos, kTileHigh, Knobs());

    const Vector3 near{0.0f, 0.0f, 4.0f};
    const Vector3 far{0.0f, 0.0f, 16.0f};
    constexpr float kRadius = 2.0f;
    EXPECT_NEAR(ScatterCoverage(p, near, kRadius), kRadius / 4.0f, 1e-6f);
    EXPECT_NEAR(ScatterCoverage(p, far, kRadius), kRadius / 16.0f, 1e-6f);
    // 4x the distance from the light => 1/4 the coverage, regardless of where any
    // camera is: moving the caster away from the LIGHT is what coarsens it.
    EXPECT_NEAR(ScatterCoverage(p, near, kRadius) / ScatterCoverage(p, far, kRadius), 4.0f, 1e-5f);
}

TEST(PointShadowLodSelection, SseBudgetIsSpentInFaceTexels)
{
    // LodSseThresholdToCoverage(h, budget) = 2*budget/h with h = the FACE tile,
    // so the error budget buys the same number of SHADOW-MAP texels at every tier.
    const Vector3 lightPos{0.0f, 0.0f, 0.0f};
    const auto high = MakePointFaceLodParams(lightPos, kTileHigh, Knobs());
    EXPECT_NEAR(high.sseThresholdToCoverage, 2.0f * kBudgetPx / static_cast<float>(kTileHigh),
                1e-9f);
    EXPECT_NEAR(high.sseThresholdToCoverageTight,
                2.0f * kBudgetPx * kSkinnedScale / static_cast<float>(kTileHigh), 1e-9f);
}

TEST(PointShadowLodSelection, LowerTierSelectsCoarser)
{
    // This is the channel camera importance still travels: a smaller tile raises
    // every SSE switch point, so a demoted (low-tier) slot coarsens earlier. The
    // tier is quantized, hysteresis-damped and cache-keyed — unlike a per-frame
    // camera position, which is why it can influence selection safely.
    const Vector3 lightPos{0.0f, 0.0f, 0.0f};
    const auto high = MakePointFaceLodParams(lightPos, kTileHigh, Knobs());
    const auto low = MakePointFaceLodParams(lightPos, kTileLow, Knobs());
    EXPECT_GT(low.sseThresholdToCoverage, high.sseThresholdToCoverage);
    EXPECT_NEAR(low.sseThresholdToCoverage / high.sseThresholdToCoverage,
                static_cast<float>(kTileHigh) / static_cast<float>(kTileLow), 1e-5f);
}

TEST(PointShadowLodSelection, ZeroBudgetKeepsDetail)
{
    // The scatter's keep-detail fail-safe: <= 0 makes SSE slots match at any
    // coverage, so a tools/test path that never sets a budget never coarsens.
    const auto p = MakePointFaceLodParams(Vector3{0.0f, 0.0f, 0.0f}, kTileHigh,
                                          Knobs(0.0f, kLodAuto, 0.0f, kSkinnedScale));
    EXPECT_FLOAT_EQ(p.sseThresholdToCoverage, 0.0f);
    EXPECT_FLOAT_EQ(p.sseThresholdToCoverageTight, 0.0f);
}

TEST(PointShadowLodSelection, NeverSmallObjectCulls)
{
    // A culled caster's shadow can be far larger than the caster's own coverage,
    // so no shadow slice may small-object cull — including these.
    const auto p = MakePointFaceLodParams(Vector3{1.0f, 2.0f, 3.0f}, kTileHigh, Knobs());
    EXPECT_FLOAT_EQ(p.smallCullCoverage, 0.0f);
}

TEST(PointShadowLodSelection, BiasAndForceLevelReachTheSlice)
{
    // Both knobs are keyed, so both must actually arrive — a dropped one would
    // key an invalidation the slices never honored.
    const auto p = MakePointFaceLodParams(Vector3{0.0f, 0.0f, 0.0f}, kTileHigh,
                                          Knobs(-1.5f, 2u));
    EXPECT_FLOAT_EQ(p.lodBiasGlobal, -1.5f);
    EXPECT_EQ(p.forceLod, 2u);

    // A negative bias scales coverage by exp2(bias): half the coverage at -1.
    const auto biased = MakePointFaceLodParams(Vector3{0.0f, 0.0f, 0.0f}, kTileHigh,
                                               Knobs(-1.0f));
    const auto plain = MakePointFaceLodParams(Vector3{0.0f, 0.0f, 0.0f}, kTileHigh, Knobs(0.0f));
    const Vector3 caster{0.0f, 0.0f, 8.0f};
    EXPECT_NEAR(ScatterCoverage(biased, caster, 2.0f),
                0.5f * ScatterCoverage(plain, caster, 2.0f), 1e-6f);
}
