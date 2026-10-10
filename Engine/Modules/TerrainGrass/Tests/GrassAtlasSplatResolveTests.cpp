// Behavioural tests for the two ground-splat decisions grass placement and grass shading share.
//
// The SOURCE SELECTION: which of the unified splatmap, the resident atlas slot, the coarse atlas
// field, or nothing at all a point's four ground weights come from — one answer for both stages.
//
// The UPGRADE CROSSFADE: how a tile still streaming from the coarse field into its slot is shown —
// deliberately two answers, because colour must track the ground and a placement mask must not.
//
// These tests execute the SHIPPED shader code. The selection block of
// TerrainGrass/grass_atlas_splat.glsl is extracted verbatim at build time (ExtractShaderBlock.cmake)
// and compiled here through GlslShim.h, so an edit to the shader is an edit to what these tests
// measure. A hand-written C++ mirror would pass forever while the shader drifted underneath it,
// which is exactly the failure this arrangement exists to prevent.
//
// The selection is a pure function of residency and binding facts precisely so it CAN be lifted:
// the texture taps live outside the block, in the one wrapper both stages call. That split is what
// makes "the blade and its ground agree" testable on the host instead of only in a screenshot.
//
// The case that matters most is AtlasBackedTerrainNeverFallsBackToTheUnifiedSplatmap. An
// atlas-backed terrain carries SplatmapBindless == 0 by construction (TerrainExtractionSystem
// fills the unified indices only on the non-atlas path), so a resolve that reaches the unified
// branch on an atlas terrain answers channel 0 for the entire world — which is the defect this
// shared block was extracted to fix, and the one a future edit is most likely to reintroduce.

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "Terrain/TerrainTypes.h"

namespace
{
using GameEngine::GlslShim::uint;

namespace Shader
{
#include "GrassAtlasSplatSourceExtracted.h"
} // namespace Shader

// A terrain whose Flags carry the atlas bit, and one that does not.
constexpr uint kAtlasFlags = Shader::kTerrainFlagAtlasBacked;
constexpr uint kPlanarFlags = 0u;

// Bindless slot 0 means "unbound" everywhere in this engine; any non-zero value is a real slot.
constexpr uint kUnbound = 0u;
constexpr uint kBoundSplat = 7u;
constexpr uint kBoundCoarse = 9u;
constexpr uint kBoundUnified = 19u;

constexpr uint kAtlasOn = 1u;
constexpr uint kAtlasOff = 0u;

uint Select(uint terrainFlags, uint atlasEnabled, uint splatmapBindless, bool resident,
            uint atlasSplat, uint atlasCoarse)
{
    return Shader::GrassAtlas_SelectSplatSource(terrainFlags, atlasEnabled, splatmapBindless,
                                                resident, atlasSplat, atlasCoarse);
}

float SlotWeight(bool haveSlot, bool haveCoarse, float rowFade, bool showUpgradeFade)
{
    return Shader::GrassAtlas_SlotUpgradeWeight(haveSlot, haveCoarse, rowFade, showUpgradeFade);
}

// The two consumers, named for what they are rather than for the boolean they pass.
constexpr bool kColour = true;     // the grass surface: shows the ground's upgrade crossfade
constexpr bool kPlacement = false; // the placement compute: takes the slot outright

// A tile partway through its coarse->slot upgrade window, and one that has finished.
constexpr float kMidUpgrade = 0.35f;
constexpr float kSettled = 1.0f;

} // namespace

// ---------------------------------------------------------------------------
// The four sources, one test each.
// ---------------------------------------------------------------------------

TEST(GrassAtlasSplatSource, PlanarTerrainWithASplatmapReadsTheUnifiedSplatmap)
{
    EXPECT_EQ(Select(kPlanarFlags, kAtlasOff, kBoundUnified, false, kUnbound, kUnbound),
              Shader::kGrassSplatSourceUnified);
}

TEST(GrassAtlasSplatSource, ResidentAtlasTileReadsItsSlot)
{
    EXPECT_EQ(Select(kAtlasFlags, kAtlasOn, kUnbound, true, kBoundSplat, kBoundCoarse),
              Shader::kGrassSplatSourceAtlasSlot);
}

TEST(GrassAtlasSplatSource, NonResidentAtlasTileFallsToTheCoarseField)
{
    EXPECT_EQ(Select(kAtlasFlags, kAtlasOn, kUnbound, false, kBoundSplat, kBoundCoarse),
              Shader::kGrassSplatSourceAtlasCoarse);
}

// Residency is not enough: a resident tile whose atlas splat texture has not been registered yet
// (the first frames of a terrain) has no slot to read, and the coarse field is the continuous
// answer rather than a hole.
TEST(GrassAtlasSplatSource, ResidentTileWithNoAtlasSplatBoundStillTakesTheCoarseField)
{
    EXPECT_EQ(Select(kAtlasFlags, kAtlasOn, kUnbound, true, kUnbound, kBoundCoarse),
              Shader::kGrassSplatSourceAtlasCoarse);
}

TEST(GrassAtlasSplatSource, NothingBoundAtAllSelectsNoSource)
{
    EXPECT_EQ(Select(kPlanarFlags, kAtlasOff, kUnbound, false, kUnbound, kUnbound),
              Shader::kGrassSplatSourceNone);
    EXPECT_EQ(Select(kAtlasFlags, kAtlasOn, kUnbound, true, kUnbound, kUnbound),
              Shader::kGrassSplatSourceNone);
}

// ---------------------------------------------------------------------------
// The defect this block exists to prevent.
// ---------------------------------------------------------------------------

// THE RED ARM. Neutralize the atlas branch in GrassAtlas_SelectSplatSource (force its condition
// false — literal deletion trips unreferenced-parameter warnings-as-errors before any test runs)
// and every case here reports Unified or None instead of an atlas source. A terrain whose ground
// is painted channel 3 then shades its blades from channel 0 — the state the surface shipped in.
TEST(GrassAtlasSplatSource, AtlasBackedTerrainNeverFallsBackToTheUnifiedSplatmap)
{
    // Every combination of residency and atlas binding an atlas terrain can be in.
    for (const bool resident : {false, true})
    {
        for (const uint atlasSplat : {kUnbound, kBoundSplat})
        {
            for (const uint atlasCoarse : {kUnbound, kBoundCoarse})
            {
                // The value an atlas terrain actually carries, and a non-zero one to prove the
                // branch is not merely dead because the field happened to be 0.
                for (const uint unified : {kUnbound, kBoundUnified})
                {
                    const uint source = Select(kAtlasFlags, kAtlasOn, unified, resident,
                                               atlasSplat, atlasCoarse);
                    EXPECT_NE(source, Shader::kGrassSplatSourceUnified)
                        << "atlas terrain resolved through the unified splatmap"
                        << " (resident=" << resident << " atlasSplat=" << atlasSplat
                        << " atlasCoarse=" << atlasCoarse << " unified=" << unified << ")";
                }
            }
        }
    }
}

// The flag says where a terrain's data LIVES; Atlas.Enabled says whether the feature bound an atlas
// this frame. Before it has, an atlas-flagged terrain must fall back to its unified indices rather
// than resolve through a disabled atlas — the params UBO is zeroed exactly so this reads that way.
TEST(GrassAtlasSplatSource, AtlasFlagWithoutABoundAtlasIsNotAnAtlasResolve)
{
    EXPECT_FALSE(Shader::GrassAtlas_IsAtlasBacked(kAtlasFlags, kAtlasOff));
    EXPECT_TRUE(Shader::GrassAtlas_IsAtlasBacked(kAtlasFlags, kAtlasOn));
    EXPECT_FALSE(Shader::GrassAtlas_IsAtlasBacked(kPlanarFlags, kAtlasOn));

    EXPECT_EQ(Select(kAtlasFlags, kAtlasOff, kBoundUnified, true, kBoundSplat, kBoundCoarse),
              Shader::kGrassSplatSourceUnified);
    EXPECT_EQ(Select(kAtlasFlags, kAtlasOff, kUnbound, true, kBoundSplat, kBoundCoarse),
              Shader::kGrassSplatSourceNone);
}

// The atlas bit's numeric value is a mirror of Terrain::kTerrainFlagAtlasBacked, and the flags word
// carries other bits (bit0 = receive shadows). Selecting on equality rather than a mask test would
// send a shadow-casting atlas terrain down the unified path.
TEST(GrassAtlasSplatSource, TheAtlasBitIsTestedAsAMaskNotAsAWholeWord)
{
    // The bit the SHADER selects on is the bit TerrainExtractionSystem sets. Asserted against the
    // C++ constant rather than a literal: the shader's copy is a mirror, and a mirror nobody
    // compares is just a second source of truth.
    EXPECT_EQ(Shader::kTerrainFlagAtlasBacked,
              static_cast<uint>(GameEngine::Terrain::kTerrainFlagAtlasBacked));
    const uint kReceiveShadowsBit =
        static_cast<uint>(GameEngine::Terrain::kTerrainFlagReceiveShadows);
    EXPECT_TRUE(Shader::GrassAtlas_IsAtlasBacked(kAtlasFlags | kReceiveShadowsBit, kAtlasOn));
    EXPECT_EQ(Select(kAtlasFlags | kReceiveShadowsBit, kAtlasOn, kUnbound, true, kBoundSplat,
                     kBoundCoarse),
              Shader::kGrassSplatSourceAtlasSlot);
}

// ---------------------------------------------------------------------------
// The coarse->slot upgrade crossfade: which consumer shows it, and how much.
//
// The selection above answers WHERE a point's weights come from. This answers what happens while
// that answer is CHANGING — the ~0.4 s (kAtlasUpgradeFadeSeconds) a newly-resident tile spends
// crossfading from the coarse field into its slot. The ground has always faded across it; the
// blades used to jump, so a blade arrived at full detail while the ground under it was halfway
// there. These tests are what pins the two consumers to opposite answers on purpose.
// ---------------------------------------------------------------------------

// THE POINT OF THE SLICE. Mid-upgrade, the colour consumer reports the row's own Fade verbatim —
// the same number cbt_surface.glsl mixes the ground by — so the blade's ground colour and the
// ground travel together instead of one leading the other.
TEST(GrassAtlasSlotUpgrade, TheColourConsumerMidUpgradeReportsTheRowsOwnFade)
{
    EXPECT_FLOAT_EQ(SlotWeight(true, true, kMidUpgrade, kColour), kMidUpgrade);
    // Across the window, not at one lucky point: the weight IS the fade, with no reshaping.
    for (const float fade : {0.0f, 0.05f, 0.25f, 0.5f, 0.75f, 0.99f})
        EXPECT_FLOAT_EQ(SlotWeight(true, true, fade, kColour), fade) << "rowFade=" << fade;
}

// The placement invariant, and the reason this parameter exists at all. A placement mask is a
// THRESHOLD and a refusal, not a colour: a weight sliding between two sources across the upgrade
// window would spawn and unspawn blades along the residency frontier as it crossed the threshold.
// Exhaustive over every state a consumer can be in, so it cannot pass by landing on a lucky case.
TEST(GrassAtlasSlotUpgrade, ThePlacementConsumerNeverShowsTheCrossfade)
{
    for (const bool haveSlot : {false, true})
        for (const bool haveCoarse : {false, true})
            for (const float fade : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
                EXPECT_FLOAT_EQ(SlotWeight(haveSlot, haveCoarse, fade, kPlacement), 1.0f)
                    << "haveSlot=" << haveSlot << " haveCoarse=" << haveCoarse
                    << " rowFade=" << fade;
}

// A crossfade needs both of its endpoints. A tile whose terrain has no coarse splat field bound yet
// has nothing to fade in FROM, so it takes the slot outright even mid-window — mixing toward an
// unbound texture is a worse pop than the one being removed. The same holds with no slot to fade
// in TO: that consumer is already showing the coarse field and has no upgrade in flight.
TEST(GrassAtlasSlotUpgrade, WithOnlyOneEndpointTheSlotIsTakenOutright)
{
    EXPECT_FLOAT_EQ(SlotWeight(true, false, kMidUpgrade, kColour), 1.0f);
    EXPECT_FLOAT_EQ(SlotWeight(false, true, kMidUpgrade, kColour), 1.0f);
    EXPECT_FLOAT_EQ(SlotWeight(false, false, kMidUpgrade, kColour), 1.0f);
}

// A settled tile answers exactly 1, which is the value the resolve tests to SKIP its second tap. So
// a settled resident tile and an out-of-window tile both stay at one texture read, and only a tile
// actually mid-upgrade pays for the fade.
TEST(GrassAtlasSlotUpgrade, ASettledTilePaysForNoSecondTap)
{
    EXPECT_FLOAT_EQ(SlotWeight(true, true, kSettled, kColour), 1.0f);
    EXPECT_FLOAT_EQ(SlotWeight(true, true, kSettled, kPlacement), 1.0f);
}
