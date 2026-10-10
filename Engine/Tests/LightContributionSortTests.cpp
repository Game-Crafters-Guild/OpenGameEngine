// LightContributionSortTests.cpp
// Covers RenderServices::FinalizeWorldLights — the CPU-side contribution sort
// that makes light-list truncation (the 1024-light global cap and the
// per-cluster cap) deterministic and least-visible. The sort is the single
// source of truth for light order; both the LightBuffer upload packing and the
// shadow-index builders walk the finalized list, so a stable strongest-first
// order is what keeps dense-scene truncation from flickering frame to frame.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using GameEngine::Engine::Renderer::ExtractedLight;
using GameEngine::Engine::Renderer::RenderServices;
using LightType = GameEngine::Components::LightType;

namespace
{
// Mirror of RenderServices' view-independent contribution metric (kept in sync
// intentionally so the test asserts against the documented ordering key, not
// against the implementation).
double Contribution(const ExtractedLight& l)
{
    if (l.castsLight == 0u)
        return 0.0;
    const double luma = 0.2126 * l.color[0] + 0.7152 * l.color[1] + 0.0722 * l.color[2];
    const double reach = double(l.range) * double(l.range);
    return luma * double(l.intensity) * reach;
}

ExtractedLight MakePoint(uint32_t sortId, float intensity, float range, float gray = 1.0f, uint32_t castsLight = 1u)
{
    ExtractedLight l{};
    l.type = LightType::Point;
    l.SortId = sortId;
    l.intensity = intensity;
    l.range = range;
    l.color[0] = gray;
    l.color[1] = gray;
    l.color[2] = gray;
    l.castsLight = castsLight;
    return l;
}

// Directional with the extraction defaults that matter to selection: range
// stays at the component default (10) — extraction passes it through — so the
// contribution sort orders directionals by luma * intensity.
ExtractedLight MakeDirectional(uint32_t sortId, float intensity, uint32_t castsShadows, float gray = 1.0f)
{
    ExtractedLight l{};
    l.type = LightType::Directional;
    l.SortId = sortId;
    l.intensity = intensity;
    l.range = 10.0f;
    l.color[0] = gray;
    l.color[1] = gray;
    l.color[2] = gray;
    l.castsLight = 1u;
    l.castsShadows = castsShadows;
    return l;
}

// Submit in the given order, finalize, and run the shared selector — the exact
// consumer path (WriteViewLightBuffer / ShadowMapNode / fog resolver).
const ExtractedLight* FinalizeAndSelect(RenderServices& rs, const std::vector<ExtractedLight>& lightsInOrder)
{
    for (const auto& l : lightsInOrder)
        rs.SubmitLight(0u, l);
    rs.FinalizeWorldLights(0u);
    return GameEngine::Engine::Renderer::SelectPrimaryDirectional(rs.GetWorldLights(0u));
}

// Submit a list in the given order, finalize, and return the finalized SortId
// sequence.
std::vector<uint32_t> FinalizeAndReadIds(const std::vector<ExtractedLight>& lightsInOrder)
{
    RenderServices rs;
    for (const auto& l : lightsInOrder)
        rs.SubmitLight(0u, l);
    rs.FinalizeWorldLights(0u);
    const auto sorted = rs.GetWorldLights(0u);
    std::vector<uint32_t> ids;
    ids.reserve(sorted.size());
    for (const auto& l : sorted)
        ids.push_back(l.SortId);
    return ids;
}
} // namespace

// The finalized order is strictly non-increasing in contribution, so a first-N
// truncation (both the 1024 global cap and the per-cluster cap walk the list in
// order) keeps exactly the top-N contributors.
TEST(LightContributionSort, TruncationKeepsTopContributionSet)
{
    constexpr int kCount = 1500;      // > the 1024 global cap
    constexpr int kCap = 1024;

    std::vector<ExtractedLight> lights;
    lights.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        // Distinct contributions: intensity climbs with the id so the mapping
        // id -> contribution is a strict, easy-to-verify order.
        lights.push_back(MakePoint(uint32_t(i), /*intensity=*/1.0f + float(i), /*range=*/5.0f));
    }

    // Submit shuffled — the pre-sort order must not affect the kept set.
    std::mt19937 rng(1234);
    std::shuffle(lights.begin(), lights.end(), rng);

    const std::vector<uint32_t> ids = FinalizeAndReadIds(lights);
    ASSERT_EQ(ids.size(), size_t(kCount));

    // Strictly strongest-first: higher id = higher intensity = higher contribution.
    for (int i = 1; i < kCount; ++i)
        EXPECT_GT(ids[i - 1], ids[i]) << "not strongest-first at position " << i;

    // Kept set (first kCap) == the kCap highest ids, as a set.
    std::vector<uint32_t> kept(ids.begin(), ids.begin() + kCap);
    std::sort(kept.begin(), kept.end());
    for (int i = 0; i < kCap; ++i)
        EXPECT_EQ(kept[i], uint32_t(kCount - kCap + i)) << "kept set differs from top-N at " << i;
}

// The whole point of the sort: the kept set is identical regardless of the
// order lights were submitted in. This is the anti-flicker guarantee — ECS
// iteration order churns frame to frame, the finalized order must not.
TEST(LightContributionSort, StableAcrossSubmissionOrders)
{
    std::vector<ExtractedLight> base;
    for (int i = 0; i < 800; ++i)
        base.push_back(MakePoint(uint32_t(i), /*intensity=*/1.0f + float(i % 37), /*range=*/2.0f + float(i % 11)));

    auto a = base;
    auto b = base;
    std::mt19937 rngA(1), rngB(999);
    std::shuffle(a.begin(), a.end(), rngA);
    std::shuffle(b.begin(), b.end(), rngB);

    const std::vector<uint32_t> idsA = FinalizeAndReadIds(a);
    const std::vector<uint32_t> idsB = FinalizeAndReadIds(b);
    EXPECT_EQ(idsA, idsB) << "finalized order must be independent of submission order";
}

// Equal-contribution lights order by SortId ascending — a deterministic
// tie-break so a boundary between equal contributors is frame-stable.
TEST(LightContributionSort, DeterministicTieBreakBySortId)
{
    std::vector<ExtractedLight> lights;
    // All identical contribution (same intensity/range/color), ids out of order.
    for (uint32_t id : {40u, 10u, 30u, 20u, 50u})
        lights.push_back(MakePoint(id, /*intensity=*/7.0f, /*range=*/3.0f));

    const std::vector<uint32_t> ids = FinalizeAndReadIds(lights);
    const std::vector<uint32_t> expected{10u, 20u, 30u, 40u, 50u};
    EXPECT_EQ(ids, expected);
}

// A non-emitting light (castsLight == 0) contributes nothing to the frame, so
// it must sink below every emitting light regardless of its intensity/range —
// it should be the first thing truncated.
TEST(LightContributionSort, NonEmittingSinksBelowEmitting)
{
    std::vector<ExtractedLight> lights;
    lights.push_back(MakePoint(1u, /*intensity=*/9999.0f, /*range=*/9999.0f, 1.0f, /*castsLight=*/0u)); // huge but dark
    lights.push_back(MakePoint(2u, /*intensity=*/1.0f, /*range=*/1.0f));                                 // tiny but lit
    lights.push_back(MakePoint(3u, /*intensity=*/2.0f, /*range=*/1.0f));

    const std::vector<uint32_t> ids = FinalizeAndReadIds(lights);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids.back(), 1u) << "non-emitting light must sort last";
    EXPECT_GT(Contribution(lights[2]), 0.0);
}

// Perf gate: the sort is per-frame CPU work on the light array. Confirm it is
// cheap at 1024+ lights (well under a frame). Prints the measured cost.
TEST(LightContributionSort, SortCostIsCheapAt2048Lights)
{
    constexpr int kCount = 2048;
    std::vector<ExtractedLight> lights;
    lights.reserve(kCount);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(0.5f, 200.0f);
    for (int i = 0; i < kCount; ++i)
        lights.push_back(MakePoint(uint32_t(i), dist(rng), dist(rng)));

    RenderServices rs;
    for (const auto& l : lights)
        rs.SubmitLight(0u, l);

    const auto start = std::chrono::steady_clock::now();
    rs.FinalizeWorldLights(0u);
    const auto end = std::chrono::steady_clock::now();
    const double micros = std::chrono::duration<double, std::micro>(end - start).count();

    // Sanity: strongest-first.
    const auto sorted = rs.GetWorldLights(0u);
    for (size_t i = 1; i < sorted.size(); ++i)
        EXPECT_GE(Contribution(sorted[i - 1]) + 1e-6, Contribution(sorted[i]));

    std::cout << "[perf] FinalizeWorldLights(" << kCount << " lights) = " << micros << " us\n";
    EXPECT_LT(micros, 5000.0) << "sort cost regressed far past expected microseconds";
}

// ---- SelectPrimaryDirectional: the single shading/shadow/fog directional ----

// The strongest directional wins regardless of submission order, and the
// caster flag consumers read rides THE SAME light — never a weaker one's.
TEST(SelectPrimaryDirectional, StrongestWinsAndCasterFlagRidesTheSameLight)
{
    const ExtractedLight strong = MakeDirectional(/*sortId=*/7u, /*intensity=*/3160.0f, /*castsShadows=*/1u);
    const ExtractedLight weak = MakeDirectional(/*sortId=*/9u, /*intensity=*/960.0f, /*castsShadows=*/0u);

    for (const std::vector<ExtractedLight>& order :
         {std::vector<ExtractedLight>{strong, weak}, std::vector<ExtractedLight>{weak, strong}})
    {
        RenderServices rs;
        const ExtractedLight* sun = FinalizeAndSelect(rs, order);
        ASSERT_NE(sun, nullptr);
        EXPECT_EQ(sun->SortId, 7u) << "strongest directional must be selected";
        EXPECT_EQ(sun->castsShadows, 1u) << "caster flag must come from the selected light";
    }
}

// The shipped bug: the last-extracted directional overwrote the UBO, so a
// weak non-casting fill submitted LAST dimmed the world and zeroed the shadow
// gate. The selector reads sorted order — last position can never flip it.
TEST(SelectPrimaryDirectional, WeakestSubmittedLastCanNeverFlipTheResult)
{
    RenderServices rs;
    const ExtractedLight* sun = FinalizeAndSelect(
        rs, {MakeDirectional(1u, 3160.0f, /*castsShadows=*/1u),
             MakePoint(2u, /*intensity=*/50000.0f, /*range=*/3.0f), // stronger contribution than both
             MakeDirectional(3u, 960.0f, /*castsShadows=*/0u)});    // the weak fill, last
    ASSERT_NE(sun, nullptr);
    EXPECT_EQ(sun->SortId, 1u);
    EXPECT_EQ(sun->castsShadows, 1u);
}

// Dimming the strong light below the fill flips selection to the fill — WITH
// the fill's own castsShadows honestly applied (no shadows), not the old
// light's.
TEST(SelectPrimaryDirectional, DimmingFlipsSelectionWithItsOwnCasterFlag)
{
    RenderServices rs;
    const ExtractedLight* sun = FinalizeAndSelect(
        rs, {MakeDirectional(1u, /*intensity=*/100.0f, /*castsShadows=*/1u),  // dimmed strong
             MakeDirectional(2u, /*intensity=*/960.0f, /*castsShadows=*/0u)}); // fill, now strongest
    ASSERT_NE(sun, nullptr);
    EXPECT_EQ(sun->SortId, 2u);
    EXPECT_EQ(sun->castsShadows, 0u) << "the selected light's own gate applies";
}

// No directional at all -> null (consumers publish the zeroed ShadowData /
// fall back to the sky anchor).
TEST(SelectPrimaryDirectional, NoDirectionalReturnsNull)
{
    RenderServices rs;
    const ExtractedLight* sun =
        FinalizeAndSelect(rs, {MakePoint(1u, 100.0f, 5.0f), MakePoint(2u, 10.0f, 2.0f)});
    EXPECT_EQ(sun, nullptr);
}

// ---- PackForwardLightDirectionals: primary + unshadowed secondaries ---------

namespace
{
using GameEngine::Engine::Renderer::ForwardLightUBO;
using GameEngine::Engine::Renderer::PackForwardLightDirectionals;
using GameEngine::Engine::Renderer::kMaxSecondaryDirectionals;

ExtractedLight MakeColoredDirectional(uint32_t sortId, float intensity, uint32_t castsShadows,
                                      const float (&color)[3], const float (&dir)[3])
{
    ExtractedLight l = MakeDirectional(sortId, intensity, castsShadows);
    l.color[0] = color[0];
    l.color[1] = color[1];
    l.color[2] = color[2];
    l.directionWS[0] = dir[0];
    l.directionWS[1] = dir[1];
    l.directionWS[2] = dir[2];
    return l;
}

// Submit, finalize, pack — the exact WriteViewLightBuffer path.
ForwardLightUBO FinalizeAndPack(const std::vector<ExtractedLight>& lightsInOrder)
{
    RenderServices rs;
    for (const auto& l : lightsInOrder)
        rs.SubmitLight(0u, l);
    rs.FinalizeWorldLights(0u);
    ForwardLightUBO ubo{};
    PackForwardLightDirectionals(rs.GetWorldLights(0u), ubo);
    return ubo;
}
} // namespace

// Strongest directional lands in the primary lanes (with ITS caster flag);
// the rest pack strongest-first as secondaries with premultiplied color —
// independent of submission order, with points interleaved.
TEST(PackForwardLightDirectionals, PrimaryAndSecondariesPackStrongestFirst)
{
    const float white[3]{1.0f, 1.0f, 1.0f};
    const float tint[3]{0.5f, 0.25f, 1.0f};
    const float dSun[3]{0.0f, -1.0f, 0.2f};
    const float dFill[3]{0.6f, -0.4f, -0.3f};
    const float dRim[3]{-0.7f, -0.1f, 0.5f};

    const ForwardLightUBO ubo = FinalizeAndPack({
        MakeColoredDirectional(3u, /*intensity=*/500.0f, /*castsShadows=*/0u, tint, dRim),
        MakePoint(9u, /*intensity=*/50000.0f, /*range=*/3.0f), // stronger than every directional
        MakeColoredDirectional(1u, /*intensity=*/3000.0f, /*castsShadows=*/1u, white, dSun),
        MakeColoredDirectional(2u, /*intensity=*/900.0f, /*castsShadows=*/0u, tint, dFill),
    });

    // Primary = strongest directional, direction + raw color + intensity + caster gate.
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[0], dSun[0]);
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[1], dSun[1]);
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[2], dSun[2]);
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[3], 3000.0f);
    EXPECT_FLOAT_EQ(ubo.uLightColorWorld[3], 1.0f) << "caster gate must ride the primary";

    // Secondaries strongest-first: fill (900) then rim (500), colors premultiplied.
    ASSERT_FLOAT_EQ(ubo.uSecondaryCount[0], 2.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryDirs[0][0], dFill[0]);
    EXPECT_FLOAT_EQ(ubo.uSecondaryDirs[0][1], dFill[1]);
    EXPECT_FLOAT_EQ(ubo.uSecondaryDirs[0][2], dFill[2]);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][0], tint[0] * 900.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][1], tint[1] * 900.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][2], tint[2] * 900.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryDirs[1][0], dRim[0]);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[1][0], tint[0] * 500.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[1][2], tint[2] * 500.0f);
}

// More directionals than the cap: exactly kMaxSecondaryDirectionals pack, the
// strongest ones, in order; the overflow contributes nothing.
TEST(PackForwardLightDirectionals, CapKeepsStrongestSecondaries)
{
    std::vector<ExtractedLight> lights;
    for (uint32_t i = 0; i < 6; ++i) // intensities 100, 200, ..., 600
        lights.push_back(MakeDirectional(i + 1u, 100.0f * float(i + 1u), /*castsShadows=*/0u));

    const ForwardLightUBO ubo = FinalizeAndPack(lights);
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[3], 600.0f) << "primary = strongest";
    ASSERT_FLOAT_EQ(ubo.uSecondaryCount[0], float(kMaxSecondaryDirectionals));
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][0], 500.0f); // white * intensity
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[1][0], 400.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[2][0], 300.0f);
}

// Non-emitting (castsLight=0) and zero-intensity directionals never pack as
// secondaries — no per-pixel BRDF is spent on a light that contributes nothing.
TEST(PackForwardLightDirectionals, NonEmittingAndZeroIntensityNeverPack)
{
    ExtractedLight dark = MakeDirectional(2u, /*intensity=*/9999.0f, 0u);
    dark.castsLight = 0u;
    const ForwardLightUBO ubo = FinalizeAndPack({
        MakeDirectional(1u, /*intensity=*/1000.0f, /*castsShadows=*/1u),
        dark,
        MakeDirectional(3u, /*intensity=*/0.0f, 0u),
        MakeDirectional(4u, /*intensity=*/250.0f, 0u),
    });
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[3], 1000.0f);
    ASSERT_FLOAT_EQ(ubo.uSecondaryCount[0], 1.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][0], 250.0f);
}

// A directional light that is on but black delivers nothing, and is skipped like one at zero
// intensity: a sky that drives its sun at night with the moon hidden turns it off through its
// colour, not its intensity, and no per-pixel BRDF may be spent on it.
TEST(DirectionalGates, ABlackSunSkipsTheDirectionalTerms)
{
    const float black[3]{0.0f, 0.0f, 0.0f};
    const float down[3]{0.0f, -1.0f, 0.0f};
    const ForwardLightUBO ubo = FinalizeAndPack({
        MakeDirectional(1u, /*intensity=*/1000.0f, /*castsShadows=*/1u),
        MakeColoredDirectional(2u, /*intensity=*/492.6f, /*castsShadows=*/0u, black, down),
        MakeDirectional(3u, /*intensity=*/250.0f, 0u),
    });
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[3], 1000.0f);
    ASSERT_FLOAT_EQ(ubo.uSecondaryCount[0], 1.0f) << "the black directional must not pack";
    EXPECT_FLOAT_EQ(ubo.uSecondaryColors[0][0], 250.0f);
}

// No directional: every directional lane stays zeroed (count 0, dark sun) so
// the shader's uniform secondary branch is a no-op and the primary is unlit.
TEST(PackForwardLightDirectionals, NoDirectionalLeavesLanesZero)
{
    const ForwardLightUBO ubo = FinalizeAndPack({MakePoint(1u, 100.0f, 5.0f)});
    EXPECT_FLOAT_EQ(ubo.uLightDirWorld[3], 0.0f);
    EXPECT_FLOAT_EQ(ubo.uLightColorWorld[3], 0.0f);
    EXPECT_FLOAT_EQ(ubo.uSecondaryCount[0], 0.0f);
}
