// The Inspector's derived-sizing wording. Size and Samples Per Meter are two raw float drags
// that between them decide resolution, tile grid, height source, VRAM and how much of the
// terrain stays sharp — and an author could previously set a legal pair and get a terrain that
// rendered nothing. These oracles pin that the notice states the consequence in the author's
// units, and that the partially-resident case says so rather than reading as free detail.

#include <gtest/gtest.h>

#include "Inspectors/TerrainSizingNotice.h"
#include "TerrainECS/TerrainSizingPlan.h"

#include <string>

using GameEngine::Editor::DescribeTerrainResidency;
using GameEngine::Editor::DescribeTerrainSizing;
using GameEngine::TerrainECS::DeriveTerrainSizingPlan;
using GameEngine::TerrainECS::TerrainHeightSource;
using GameEngine::TerrainECS::TerrainSizingPlan;

namespace
{
bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

TEST(TerrainSizingNotice, ASmallTerrainReportsItsResolutionAndThatItDoesNotStream)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(1024.0f, 1024.0f, 1.0f);
    const std::string text = DescribeTerrainSizing(plan);

    EXPECT_TRUE(Contains(text, "1025 x 1025")) << text;
    EXPECT_TRUE(Contains(text, "one tile, no streaming")) << text;
}

// The plan derives a tile grid for every terrain because the service needs one, and for a
// non-square sub-cap terrain that grid has several tiles in it. The author must still be told
// this terrain is a single heightmap that does not stream.
TEST(TerrainSizingNotice, ANonSquareSmallTerrainStillReportsThatItDoesNotStream)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(512.0f, 128.0f, 1.0f);
    ASSERT_EQ(plan.Source, TerrainHeightSource::Single);
    ASSERT_GT(plan.TotalTiles, 1u) << "the derived grid really does have several tiles";

    EXPECT_TRUE(Contains(DescribeTerrainSizing(plan), "one tile, no streaming"))
        << DescribeTerrainSizing(plan);
}

TEST(TerrainSizingNotice, ATiledTerrainReportsItsTileGrid)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(4096.0f, 4096.0f, 2.0f);
    const std::string text = DescribeTerrainSizing(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Unified);
    EXPECT_TRUE(Contains(text, "8 x 8 streamed tiles")) << text;
}

// Detail is quoted in metres per texel because that is the unit an author reasons in — "0.5 m
// per texel on an 8 km island" — not samples per metre, and never raw sample counts alone.
TEST(TerrainSizingNotice, DetailIsQuotedInMetresPerTexel)
{
    EXPECT_TRUE(Contains(DescribeTerrainSizing(DeriveTerrainSizingPlan(4096.0f, 4096.0f, 2.0f)),
                         "0.50 m per texel"));
    EXPECT_TRUE(Contains(DescribeTerrainSizing(DeriveTerrainSizingPlan(4096.0f, 4096.0f, 0.5f)),
                         "2.0 m per texel"));
}

TEST(TerrainSizingNotice, AFullyResidentTerrainSaysAllOfItIsAtFullDetail)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(2048.0f, 2048.0f, 1.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_TRUE(plan.FullyResident());
    EXPECT_TRUE(Contains(text, "full detail")) << text;
    EXPECT_TRUE(Contains(text, "MiB")) << text;
    EXPECT_FALSE(Contains(text, "falls back")) << text;
}

// Engaging the atlas is NOT the same thing as losing detail. One metre past the unified ceiling
// (4097 m at 2 samples/m) the slot budget still covers all 81 tiles, so the terrain streams but
// nothing is coarse — and the notice must not tell the author their distant ground reads smooth
// when there is no far field at all.
TEST(TerrainSizingNotice, AFullyResidentAtlasDoesNotClaimAnythingFallsBack)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(4097.0f, 4097.0f, 2.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Atlas) << "one metre past the ceiling";
    ASSERT_TRUE(plan.FullyResident()) << "81 tiles, 81 slots";

    // Assert the CONTRADICTION is absent, not the bare phrase "falls back": the correct wording
    // legitimately contains those two words as a negation ("Nothing falls back to the coarse
    // field"), so a substring test on them fails for the same reason it would pass.
    EXPECT_FALSE(Contains(text, "Beyond the window"))
        << "there is no outside at this size — " << text;
    EXPECT_FALSE(Contains(text, "reads smooth rather than detailed")) << text;
    EXPECT_TRUE(Contains(text, "all 81 tiles")) << text;
    EXPECT_TRUE(Contains(text, "Nothing falls back")) << text;

    // CONTROL: the partially-resident wording must still say the opposite, or the assertions
    // above would hold for a notice that simply never mentions the far field.
    const std::string island =
        DescribeTerrainResidency(DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f));
    EXPECT_TRUE(Contains(island, "Beyond the window")) << island;
    EXPECT_FALSE(Contains(island, "Nothing falls back")) << island;
}

// THE CALL the notice exists to surface: past the unified ceiling most of an island is coarse,
// and the author has to know that before they commit to the size.
TEST(TerrainSizingNotice, AnIslandScaleTerrainStatesTheCoarseFarFieldItFallsBackTo)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f);
    const std::string text = DescribeTerrainResidency(plan);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Atlas);
    ASSERT_FALSE(plan.FullyResident());
    EXPECT_TRUE(Contains(text, "of 256 tiles")) << text;
    EXPECT_TRUE(Contains(text, "falls back")) << text;
    // 8192 m over a 128-texel coarse field spanning the whole terrain = 8192/127.
    EXPECT_TRUE(Contains(text, "64.5 m per texel"))
        << "the far-field density must be stated as a number, not implied: " << text;
}

TEST(TerrainSizingNotice, ADegenerateTerrainProducesNoWordingRatherThanNonsense)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(0.0f, 0.0f, 1.0f);

    EXPECT_TRUE(DescribeTerrainSizing(plan).empty());
    EXPECT_TRUE(DescribeTerrainResidency(plan).empty());
}
