// The per-view grass draw-mode decision: which alpha path a frame's grass draws take, and the
// "only if the texture needs it" rule that can switch every one of them off.
//
// ResolveGrassDrawMode is constexpr and depends on nothing but its three inputs, so these run as
// static_asserts as well as tests — the EXPECT bodies are what report a failure legibly.

#include "TerrainGrass/GrassDrawMode.h"

#include <gtest/gtest.h>

namespace GameEngine::TerrainGrass
{
namespace
{

// Sample counts either side of the alpha-to-coverage threshold. 4 is the shipped default on a
// device that reaches it and 2 the rung below (ResolveDefaultAntiAliasing), 1 is a view with
// MSAA off.
constexpr uint32 kSingleSample = 1u;
constexpr uint32 kMsaa2x = 2u;
constexpr uint32 kMsaa4x = 4u;

} // namespace

// --- The engagement rule: no soft alpha anywhere means no alpha path at all -------------------

// A texture whose alpha probes uniformly opaque, and geometric ribbon grass, which binds no
// texture at all, both arrive here as anyAlphaNeeded == false. Neither has anything for a cutout,
// a screen door or alpha-to-coverage to resolve, so the draw takes the opaque fast path.
TEST(GrassDrawModeTests, NoAlphaNeededTakesTheOpaquePathAtEverySampleCount)
{
    EXPECT_EQ(ResolveGrassDrawMode(false, false, kSingleSample), GrassDrawMode::Opaque);
    EXPECT_EQ(ResolveGrassDrawMode(false, false, kMsaa2x), GrassDrawMode::Opaque);
    EXPECT_EQ(ResolveGrassDrawMode(false, false, kMsaa4x), GrassDrawMode::Opaque);
}

// The rule outranks the authored mode: "only if the texture needs it" is about whether soft alpha
// EXISTS, and the authored mode only says how it would resolve if it did. Resolving Blend to
// Opaque here also drops the ordering artefact Blend otherwise pays, where blades composite in
// placement order because the pipeline writes no depth.
TEST(GrassDrawModeTests, NoAlphaNeededOverridesAuthoredBlend)
{
    EXPECT_EQ(ResolveGrassDrawMode(true, false, kSingleSample), GrassDrawMode::Opaque);
    EXPECT_EQ(ResolveGrassDrawMode(true, false, kMsaa2x), GrassDrawMode::Opaque);
}

// --- With soft alpha present, the shipped behaviour is unchanged ------------------------------

// The control for the rule above: the SAME inputs but with alpha present must NOT take the opaque
// path, or the two tests together would pass on a function that ignored anyAlphaNeeded entirely.
TEST(GrassDrawModeTests, AlphaNeededKeepsAnAlphaPath)
{
    EXPECT_NE(ResolveGrassDrawMode(false, true, kSingleSample), GrassDrawMode::Opaque);
    EXPECT_NE(ResolveGrassDrawMode(false, true, kMsaa2x), GrassDrawMode::Opaque);
    EXPECT_NE(ResolveGrassDrawMode(true, true, kMsaa2x), GrassDrawMode::Opaque);
}

TEST(GrassDrawModeTests, AuthoredBlendWithAlphaTakesTheBlendPath)
{
    EXPECT_EQ(ResolveGrassDrawMode(true, true, kSingleSample), GrassDrawMode::Blend);
    EXPECT_EQ(ResolveGrassDrawMode(true, true, kMsaa4x), GrassDrawMode::Blend);
}

// Alpha-to-coverage needs real MSAA samples: at one sample the hardware degenerates it to a hard
// 0.5 cutoff, so a single-sample view takes the screen-door discard instead.
TEST(GrassDrawModeTests, DitherSplitsOnSampleCount)
{
    EXPECT_EQ(ResolveGrassDrawMode(false, true, kSingleSample), GrassDrawMode::Dither);
    EXPECT_EQ(ResolveGrassDrawMode(false, true, kMsaa2x), GrassDrawMode::DitherA2C);
    EXPECT_EQ(ResolveGrassDrawMode(false, true, kMsaa4x), GrassDrawMode::DitherA2C);
}

// A sample count of 0 is what a view reports before its first world pass has published a snapshot.
// It must read as "not multisampled" rather than wrapping into the A2C arm, because enabling
// alpha-to-coverage on a single-sample target is the degenerate 0.5 cutoff above.
TEST(GrassDrawModeTests, UnpublishedSampleCountIsNotTreatedAsMultisampled)
{
    EXPECT_EQ(ResolveGrassDrawMode(false, true, 0u), GrassDrawMode::Dither);
}

// --- Ordering contract ------------------------------------------------------------------------

// TerrainGrassRenderFeature indexes its kModes / m_Materials / m_PipelineIds arrays by the raw
// enum value, so a reordered enumerator would silently bind a material to the wrong draw mode.
TEST(GrassDrawModeTests, EnumeratorsMatchTheMaterialTableOrder)
{
    EXPECT_EQ(static_cast<uint32>(GrassDrawMode::Blend), 0u);
    EXPECT_EQ(static_cast<uint32>(GrassDrawMode::Opaque), 1u);
    EXPECT_EQ(static_cast<uint32>(GrassDrawMode::Dither), 2u);
    EXPECT_EQ(static_cast<uint32>(GrassDrawMode::DitherA2C), 3u);
    EXPECT_EQ(kGrassDrawModeCount, 4u);
}

// Every reachable input maps to a mode inside the table the feature indexes with it.
TEST(GrassDrawModeTests, EveryResolvedModeIsInRange)
{
    for (const bool allBlend : {false, true})
        for (const bool anyAlpha : {false, true})
            for (const uint32 samples : {0u, 1u, 2u, 4u, 8u})
                EXPECT_LT(static_cast<uint32>(ResolveGrassDrawMode(allBlend, anyAlpha, samples)),
                          kGrassDrawModeCount);
}

// The decision is constexpr, so the contract holds at compile time too. A regression that made it
// runtime-only would still compile; one that changed an answer would not.
static_assert(ResolveGrassDrawMode(false, false, 4u) == GrassDrawMode::Opaque);
static_assert(ResolveGrassDrawMode(true, false, 4u) == GrassDrawMode::Opaque);
static_assert(ResolveGrassDrawMode(true, true, 4u) == GrassDrawMode::Blend);
static_assert(ResolveGrassDrawMode(false, true, 1u) == GrassDrawMode::Dither);
static_assert(ResolveGrassDrawMode(false, true, 2u) == GrassDrawMode::DitherA2C);

} // namespace GameEngine::TerrainGrass
