// Terrain DEFAULT surface rules: the measured ground the default rule rows are
// placed against, and what those rows put on it.
//
// THE TRAP THIS FILE GUARDS. A slope threshold is only meaningful in a stated
// unit. `1 - max(N.y, 0)` off a heightfield whose samples are normalized to [0,1]
// but differenced over METRE spacing (Heightfield.cpp ComputeNormal) is divided by
// the terrain's HeightScale, so a threshold expressed in it means a different angle
// on every terrain -- at HeightScale 60 a band that reads as 33.6 degrees actually
// sits at 88.6, which no ground reaches, and the rows using it paint nothing. The
// defaults therefore author slope in TRUE DEGREES (TerrainRuleSample::SlopeDegrees,
// recovered from all three normal components).
//
// So these tests MEASURE the distribution and assert the authored bands fall inside
// it, rather than asserting the constants against themselves. A band placed out of
// reach paints nothing, and a bake that paints nothing looks exactly like a bake
// that works -- which is why the placement test below carries a positive control.

#include <gtest/gtest.h>

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSplatComposite.h"
#include "TerrainECS/TerrainSurfaceRuleEval.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{

// ComposedIsland-mintgate.scene, verbatim: a 512 m terrain at HeightScale 60,
// two samples per metre. The scene's base is Flat and its relief comes from
// modifier noise, so this stands in for that relief with the engine's own base
// noise generator at the same extent and height scale — the quantity under test
// is the SLOPE DISTRIBUTION of realistic ground, which is set by extent and
// HeightScale, not by which generator wrote the samples.
constexpr float32 kIslandWorldSize = 512.0f;
constexpr float32 kIslandHeightScale = 60.0f;
constexpr uint32 kIslandDim = 1025; // 512 m at samplesPerMeter = 2, plus the shared edge

struct MeasuredTerrain
{
    Terrain::HeightfieldData Heightfield;
    float32 SpacingX = 0.0f;
    float32 SpacingZ = 0.0f;
    float32 MinH = 0.0f;
    float32 MaxH = 0.0f;
    float32 HeightRange = 0.0f;
};

MeasuredTerrain MakeIslandScaleTerrain()
{
    MeasuredTerrain t{};
    t.Heightfield = Terrain::HeightfieldData(kIslandDim, kIslandDim);
    FillHeightfieldBaseRegion(t.Heightfield, Components::TerrainBaseSource::ProceduralNoise,
                              nullptr, 0, 0,
                              static_cast<int32>(kIslandDim) - 1,
                              static_cast<int32>(kIslandDim) - 1);

    t.SpacingX = kIslandWorldSize / static_cast<float32>(kIslandDim - 1);
    t.SpacingZ = t.SpacingX;

    t.MinH = 1e30f;
    t.MaxH = -1e30f;
    for (uint32 z = 0; z < kIslandDim; ++z)
        for (uint32 x = 0; x < kIslandDim; ++x)
        {
            const float32 s = t.Heightfield.GetSample(x, z);
            t.MinH = std::min(t.MinH, s);
            t.MaxH = std::max(t.MaxH, s);
        }
    t.HeightRange = std::max(t.MaxH - t.MinH, 0.001f);
    return t;
}

float32 Percentile(std::vector<float32>& values, float32 fraction)
{
    if (values.empty())
        return 0.0f;
    const std::size_t idx = std::min(values.size() - 1,
        static_cast<std::size_t>(fraction * static_cast<float32>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx), values.end());
    return values[idx];
}

// The dominant (largest-weight) channel of an RGBA8 splat texel. Ties go to the
// lowest index, which is how a reader looking at the surface would call it.
uint32 DominantChannel(const uint8* pixel)
{
    uint32 best = 0;
    for (uint32 i = 1; i < 4; ++i)
        if (pixel[i] > pixel[best])
            best = i;
    return best;
}

std::array<uint32, 4> DominantCounts(const std::vector<uint8>& splat)
{
    std::array<uint32, 4> counts{};
    for (std::size_t i = 0; i + 3 < splat.size(); i += 4)
        ++counts[DominantChannel(&splat[i])];
    return counts;
}

// The default rows baked over the same terrain, through the same evaluator and
// the same compositor the bake uses (ApplySplatModifiers' inner loop), with a
// volume weight of 1 everywhere — which is what Shape::Global resolves to.
std::vector<uint8> BakeDefaultRules(const MeasuredTerrain& t)
{
    const auto rules = MakeDefaultTerrainSurfaceRules();
    std::vector<uint8> splat(static_cast<std::size_t>(kIslandDim) * kIslandDim * 4, 0u);

    for (uint32 z = 0; z < kIslandDim; ++z)
    {
        const float32 worldZ = static_cast<float32>(z) * t.SpacingZ;
        for (uint32 x = 0; x < kIslandDim; ++x)
        {
            const float32 worldX = static_cast<float32>(x) * t.SpacingX;
            const auto normal = t.Heightfield.ComputeNormal(static_cast<int32>(x),
                                                            static_cast<int32>(z),
                                                            t.SpacingX, t.SpacingZ);
            const TerrainRuleSample sample = MakeTerrainRuleSample(
                normal.x, normal.y, normal.z, t.Heightfield.GetSample(x, z),
                kIslandHeightScale, t.MinH, t.HeightRange, worldX, worldZ);

            uint8* pixel = &splat[(static_cast<std::size_t>(z) * kIslandDim + x) * 4];
            for (uint32 r = 0; r < rules.RuleCount; ++r)
            {
                const float32 weight =
                    EvaluateTerrainSurfaceRuleWeight(rules.Rules[r], sample, SurfaceRuleNoiseSample);
                if (weight <= 0.0f)
                    continue;
                CompositeSplatTexel(pixel, rules.Rules[r].MaterialSlot, weight,
                                    rules.Rules[r].Replace);
            }
        }
    }
    return splat;
}

} // namespace

// ---- 1 · The measured ground ------------------------------------------------

// The histogram the default bands are placed against, and the pin that keeps them
// REACHABLE. A band placed outside the measured distribution paints nothing, and
// a bake that paints nothing looks exactly like one that works — so the guard is
// the assertion below that each authored band still falls inside the measured
// distribution.
TEST(TerrainDefaultSurfaceRules, RealGroundReachesEveryDefaultSlopeBand)
{
    const MeasuredTerrain t = MakeIslandScaleTerrain();

    std::vector<float32> slopeNorm;
    std::vector<float32> slopeDegrees;
    std::vector<float32> heightMetres;
    std::vector<float32> heightNormalized;
    slopeNorm.reserve(static_cast<std::size_t>(kIslandDim) * kIslandDim);
    slopeDegrees.reserve(slopeNorm.capacity());
    heightMetres.reserve(slopeNorm.capacity());
    heightNormalized.reserve(slopeNorm.capacity());

    for (uint32 z = 0; z < kIslandDim; ++z)
        for (uint32 x = 0; x < kIslandDim; ++x)
        {
            const auto n = t.Heightfield.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                                        t.SpacingX, t.SpacingZ);
            const TerrainRuleSample s = MakeTerrainRuleSample(
                n.x, n.y, n.z, t.Heightfield.GetSample(x, z), kIslandHeightScale,
                t.MinH, t.HeightRange, 0.0f, 0.0f);
            slopeNorm.push_back(s.SlopeNormalized);
            slopeDegrees.push_back(s.SlopeDegrees);
            heightMetres.push_back(s.HeightMetres);
            heightNormalized.push_back(s.HeightNormalized);
        }

    const float32 slopeNormP50 = Percentile(slopeNorm, 0.50f);
    const float32 slopeNormP95 = Percentile(slopeNorm, 0.95f);
    const float32 slopeNormMax = *std::max_element(slopeNorm.begin(), slopeNorm.end());
    const float32 degP50 = Percentile(slopeDegrees, 0.50f);
    const float32 degP95 = Percentile(slopeDegrees, 0.95f);
    const float32 degMax = *std::max_element(slopeDegrees.begin(), slopeDegrees.end());
    const float32 mP05 = Percentile(heightMetres, 0.05f);
    const float32 mP50 = Percentile(heightMetres, 0.50f);
    const float32 mP95 = Percentile(heightMetres, 0.95f);

    std::printf("\n[measured] %.0f m terrain, HeightScale %.0f, %u samples/side\n",
                kIslandWorldSize, kIslandHeightScale, kIslandDim);
    std::printf("[measured] slope TRUE degrees : p50=%.2f p95=%.2f max=%.2f\n",
                degP50, degP95, degMax);
    std::printf("[measured] slope normalized   : p50=%.6f p95=%.6f max=%.6f\n",
                slopeNormP50, slopeNormP95, slopeNormMax);
    std::printf("[measured] slope degrees more : p75=%.2f p90=%.2f p99=%.2f\n",
                Percentile(slopeDegrees, 0.75f), Percentile(slopeDegrees, 0.90f),
                Percentile(slopeDegrees, 0.99f));
    std::printf("[measured] height metres      : p05=%.2f p50=%.2f p95=%.2f\n",
                mP05, mP50, mP95);
    std::printf("[measured] height normalized  : p50=%.3f p75=%.3f p90=%.3f p95=%.3f p99=%.3f\n",
                Percentile(heightNormalized, 0.50f), Percentile(heightNormalized, 0.75f),
                Percentile(heightNormalized, 0.90f), Percentile(heightNormalized, 0.95f),
                Percentile(heightNormalized, 0.99f));

    // Real ground has real slopes, in degrees.
    EXPECT_GT(degMax, 20.0f) << "this terrain is too flat to place slope materials against";

    // EVERY authored band must be reachable on this ground. These are the assertions
    // that would have caught the defect the rows replaced: a band placed outside the
    // measured distribution silently paints nothing, and nothing else in the suite
    // notices, because "no texels" and "correct texels" both look like a passing bake.
    EXPECT_LT(degP50, kDefaultGrassSlopeMax)
        << "grass no longer covers the gentle half of the terrain";
    EXPECT_GT(degP95, kDefaultDirtSlopeMin)
        << "the dirt shoulder sits above the p95 slope - it can barely fire";
    EXPECT_GT(degMax, kDefaultRockSlopeMin)
        << "the steepest texel never reaches the rock onset - rock is dead on this ground, "
           "a band out of reach paints nothing and no other test would notice";
    EXPECT_LT(kDefaultRockSlopeMin - kDefaultRockSlopeFeather, degMax)
        << "even the rock band's feathered-in edge is out of reach";

    // The normalized-slope domain is reported for scale, not asserted: on this
    // HeightScale it sits orders of magnitude below any threshold one would think
    // to express in it, which is why the rows author degrees.
    (void)slopeNormP50;
    (void)slopeNormP95;
    (void)slopeNormMax;
}

// ---- 2 · What the defaults actually place -----------------------------------

// The counts the look review needs: what the shipped rows put on the ground, over
// the unbaked (all-zero) base a real bake starts from. Rock and dirt are called out
// because they are the two most easily lost to an out-of-reach slope band -- if
// they go to zero here, the defaults have regressed into that failure mode.
//
// NOTE on reading ch0: DominantChannel returns 0 for an all-zero texel, and the
// surface resolves an all-zero splat to channel 0 too, so "ch0 dominant" folds
// together "grass won" and "no row claimed this texel". WeightedTexels separates
// them, which is why it is asserted rather than printed.
TEST(TerrainDefaultSurfaceRules, DefaultRowsPlaceSlopeMaterialsOnRealGround)
{
    const MeasuredTerrain t = MakeIslandScaleTerrain();
    const std::vector<uint8> rules = BakeDefaultRules(t);
    ASSERT_EQ(rules.size(), static_cast<std::size_t>(kIslandDim) * kIslandDim * 4);

    const auto after = DominantCounts(rules);

    std::size_t weightedTexels = 0;
    for (std::size_t i = 0; i + 3 < rules.size(); i += 4)
        if (rules[i] | rules[i + 1] | rules[i + 2] | rules[i + 3])
            ++weightedTexels;

    const auto texels = static_cast<float32>(rules.size() / 4);
    std::printf("\n[defaults] dominant channel : ch0=%u ch1=%u ch2=%u ch3=%u\n",
                after[0], after[1], after[2], after[3]);
    std::printf("[defaults] dominant percent : ch0=%.2f%% ch1=%.2f%% ch2=%.2f%% ch3=%.2f%%\n",
                100.0f * static_cast<float32>(after[0]) / texels,
                100.0f * static_cast<float32>(after[1]) / texels,
                100.0f * static_cast<float32>(after[2]) / texels,
                100.0f * static_cast<float32>(after[3]) / texels);
    std::printf("[defaults] texels carrying any weight: %zu (%.2f%%)\n",
                weightedTexels, 100.0f * static_cast<float32>(weightedTexels) / texels);

    // The positive control: an all-zero result would still report ch0=100%.
    ASSERT_GT(weightedTexels, 0u) << "the default rows placed NOTHING - every count above is the "
                                     "unbaked base being read as channel 0";

    EXPECT_GT(after[1], 0u) << "the default rows must put rock on steep ground";
    EXPECT_GT(after[2], 0u) << "the default rows must put dirt somewhere";
}
