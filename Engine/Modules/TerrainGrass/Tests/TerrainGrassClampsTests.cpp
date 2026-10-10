// The TerrainGrass field-bound pass: the single source every authoring surface runs after a
// write, so a value authored in a .scene and the same value set over the debug server's
// set_component land identically.
//
// Two things are worth pinning. The bounds themselves — one test per family, driven from an
// out-of-range input at both ends, because a clamp that silently stops clamping is invisible.
// And the pass's RESTRAINT: the fields with no meaningful bound (seeds, directions, colours,
// asset refs, the enum) must survive it untouched, or the pass would quietly become a second
// authoring policy instead of a bounds check.

#include "TerrainGrass/TerrainGrassClamps.h"

#include "AssetCore/GUID.h"
#include "Terrain/TerrainTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace GameEngine::TerrainGrass
{
namespace
{

struct BoundedField
{
    const char* Name;
    float32 Components::TerrainGrass::* Member;
};

// Every float field the pass floors at zero and does not cap.
constexpr BoundedField kNonNegativeFields[] = {
    {"BladesPerSquareMeter", &Components::TerrainGrass::BladesPerSquareMeter},
    {"Range", &Components::TerrainGrass::Range},
    {"DensityFalloff", &Components::TerrainGrass::DensityFalloff},
    {"ClumpSize", &Components::TerrainGrass::ClumpSize},
    {"BladeHeight", &Components::TerrainGrass::BladeHeight},
    {"BladeWidth", &Components::TerrainGrass::BladeWidth},
    {"MaxWidthRatio", &Components::TerrainGrass::MaxWidthRatio},
    {"RandomScale", &Components::TerrainGrass::RandomScale},
    {"WindGustSpeed", &Components::TerrainGrass::WindGustSpeed},
    {"WindStrength", &Components::TerrainGrass::WindStrength},
    {"WindFlutterAmount", &Components::TerrainGrass::WindFlutterAmount},
    {"WindFlutterSpeed", &Components::TerrainGrass::WindFlutterSpeed},
    {"Brightness", &Components::TerrainGrass::Brightness},
    {"RandomBrightness", &Components::TerrainGrass::RandomBrightness},
    {"BladeScatterGain", &Components::TerrainGrass::BladeScatterGain},
    {"Translucency", &Components::TerrainGrass::Translucency},
    {"TextureCardsPerSquareMeter", &Components::TerrainGrass::TextureCardsPerSquareMeter},
    {"TextureSize", &Components::TerrainGrass::TextureSize},
    {"NormalStrength", &Components::TerrainGrass::NormalStrength},
};

// Every float field the pass holds inside [0, 1].
constexpr BoundedField kNormalizedFields[] = {
    {"ClumpHeightVariance", &Components::TerrainGrass::ClumpHeightVariance},
    {"ClumpAlignment", &Components::TerrainGrass::ClumpAlignment},
    {"ClumpGather", &Components::TerrainGrass::ClumpGather},
    {"MaskThreshold", &Components::TerrainGrass::MaskThreshold},
    {"HueVariation", &Components::TerrainGrass::HueVariation},
    {"RootShade", &Components::TerrainGrass::RootShade},
    {"BladeNormalForm", &Components::TerrainGrass::BladeNormalForm},
    {"GroundingStrength", &Components::TerrainGrass::GroundingStrength},
    {"AlphaCutoff", &Components::TerrainGrass::AlphaCutoff},
};

} // namespace

// --- Float bounds -----------------------------------------------------------------------------

TEST(TerrainGrassClamps, NonNegativeFieldsFloorAtZeroAndKeepLargeValues)
{
    for (const BoundedField& field : kNonNegativeFields)
    {
        Components::TerrainGrass grass{};
        grass.*field.Member = -1.0f;
        ClampFields(grass);
        EXPECT_FLOAT_EQ(grass.*field.Member, 0.0f) << field.Name << " was not floored at zero";

        // The floor is one-sided: these are magnitudes with no authored ceiling, and a pass that
        // capped them would silently retune a scene rather than repair it.
        grass = Components::TerrainGrass{};
        grass.*field.Member = 1000.0f;
        ClampFields(grass);
        EXPECT_FLOAT_EQ(grass.*field.Member, 1000.0f) << field.Name << " was capped";
    }
}

TEST(TerrainGrassClamps, NormalizedFieldsHoldBothEndsOfTheUnitRange)
{
    for (const BoundedField& field : kNormalizedFields)
    {
        Components::TerrainGrass grass{};
        grass.*field.Member = -0.5f;
        ClampFields(grass);
        EXPECT_FLOAT_EQ(grass.*field.Member, 0.0f) << field.Name << " was not floored at zero";

        grass = Components::TerrainGrass{};
        grass.*field.Member = 2.0f;
        ClampFields(grass);
        EXPECT_FLOAT_EQ(grass.*field.Member, 1.0f) << field.Name << " was not capped at one";
    }
}

// smoothstep is undefined on an empty span, so the root fade's end is held above its start. A
// shader precondition, not a taste bound.
TEST(TerrainGrassClamps, RootFadeSpanStaysNonEmpty)
{
    constexpr float32 kSpan = Components::kMinTerrainGrassRootFadeSpan;
    Components::TerrainGrass grass{};
    grass.RootFadeStart = 0.7f;
    grass.RootFadeEnd = 0.2f;
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.RootFadeStart, 0.7f);
    EXPECT_FLOAT_EQ(grass.RootFadeEnd, 0.7f + kSpan);

    grass = Components::TerrainGrass{};
    grass.RootFadeStart = 5.0f;
    grass.RootFadeEnd = 5.0f;
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.RootFadeStart, 1.0f - kSpan);
    EXPECT_FLOAT_EQ(grass.RootFadeEnd, 1.0f);

    // The start may sit below the root; the end still has to sit above the start.
    grass = Components::TerrainGrass{};
    grass.RootFadeStart = -5.0f;
    grass.RootFadeEnd = -5.0f;
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.RootFadeStart, Components::kMinTerrainGrassRootFadeStart);
    EXPECT_FLOAT_EQ(grass.RootFadeEnd, Components::kMinTerrainGrassRootFadeStart + kSpan);
}

// Zero divides in the wind noise lookup, so this floor is a shader precondition rather than a
// taste bound — the one non-negative field whose floor is not zero.
TEST(TerrainGrassClamps, WindGustScaleFloorsAboveZero)
{
    Components::TerrainGrass grass{};
    grass.WindGustScale = 0.0f;
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.WindGustScale, kMinWindGustScale);

    grass.WindGustScale = -3.0f;
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.WindGustScale, kMinWindGustScale);
    EXPECT_GT(grass.WindGustScale, 0.0f);
}

// --- Integer bounds ---------------------------------------------------------------------------

TEST(TerrainGrassClamps, BladeSegmentsHoldsTheGeometryBudget)
{
    Components::TerrainGrass grass{};
    grass.BladeSegments = 0u;
    ClampFields(grass);
    EXPECT_EQ(grass.BladeSegments, Components::kMinTerrainGrassBladeSegments);

    grass.BladeSegments = 10000u;
    ClampFields(grass);
    EXPECT_EQ(grass.BladeSegments, Components::kMaxTerrainGrassBladeSegments);
}

// LayerIndex addresses a splat CHANNEL, so its ceiling is the splat's channel count and an
// out-of-range index would sample a layer the terrain does not carry.
TEST(TerrainGrassClamps, LayerIndexStaysInsideTheSplatChannelCount)
{
    Components::TerrainGrass grass{};
    grass.LayerIndex = 99u;
    ClampFields(grass);
    EXPECT_EQ(grass.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u);

    grass.LayerIndex = 0u;
    ClampFields(grass);
    EXPECT_EQ(grass.LayerIndex, 0u);
}

TEST(TerrainGrassClamps, AtlasGridStaysInsideTheAddressableRange)
{
    Components::TerrainGrass grass{};
    grass.AtlasColumns = 0u;
    grass.AtlasRows = 0u;
    grass.AtlasTileCount = 0u;
    ClampFields(grass);
    EXPECT_EQ(grass.AtlasColumns, kMinGrassAtlasDimension);
    EXPECT_EQ(grass.AtlasRows, kMinGrassAtlasDimension);
    EXPECT_EQ(grass.AtlasTileCount, kMinGrassAtlasTileCount);

    grass.AtlasColumns = 64u;
    grass.AtlasRows = 64u;
    ClampFields(grass);
    EXPECT_EQ(grass.AtlasColumns, kMaxGrassAtlasDimension);
    EXPECT_EQ(grass.AtlasRows, kMaxGrassAtlasDimension);
}

// The cross-field one, and the reason this pass is whole-component: a tile count indexes the grid
// the other two fields define, so a count past columns*rows names a tile no atlas has. The
// inspector enforced this alone; every other writer could set 999 tiles on a 2x2 grid.
TEST(TerrainGrassClamps, AtlasTileCountCannotExceedTheGridItIndexes)
{
    Components::TerrainGrass grass{};
    grass.AtlasColumns = 2u;
    grass.AtlasRows = 2u;
    grass.AtlasTileCount = 999u;
    ClampFields(grass);
    EXPECT_EQ(grass.AtlasTileCount, 4u);

    // The ceiling follows the CORRECTED grid, not the one that arrived: an out-of-range column
    // count is clamped first, so the tile ceiling is 16*3 and not 64*3.
    grass = Components::TerrainGrass{};
    grass.AtlasColumns = 64u;
    grass.AtlasRows = 3u;
    grass.AtlasTileCount = 999u;
    ClampFields(grass);
    EXPECT_EQ(grass.AtlasTileCount, kMaxGrassAtlasDimension * 3u);

    // A count already inside the grid is left alone.
    grass = Components::TerrainGrass{};
    grass.AtlasColumns = 4u;
    grass.AtlasRows = 4u;
    grass.AtlasTileCount = 9u;
    ClampFields(grass);
    EXPECT_EQ(grass.AtlasTileCount, 9u);
}

// --- Restraint --------------------------------------------------------------------------------

// Seeds, directions, colours, asset refs and the render mode carry no range. A pass that touched
// them would be authoring policy wearing a bounds check's name — and a wind direction silently
// floored at zero is exactly the kind of change nobody looks for.
TEST(TerrainGrassClamps, UnboundedFieldsSurviveUntouched)
{
    Components::TerrainGrass grass{};
    grass.RenderMode = Components::TerrainGrassRenderMode::Blend;
    grass.PlacementSeed = -12.5f;
    grass.WindDirection = -2.5f;
    grass.WindRestingLean = -0.4f;
    grass.WindSeed = -7.0f;
    grass.TextureGrass = true;
    grass.UseSplatRootColor = false;
    grass.RootColor = 0xDEADBEEFu;
    grass.TipColor = 0x00000000u;
    grass.BacklightColor = 0x12345678u;
    grass.AlbedoTextureAssetGuid.Set(GUID("11111111-2222-3333-4444-555555555555"));
    grass.AlphaTextureAssetGuid.Set(GUID("66666666-7777-8888-9999-aaaaaaaaaaaa"));
    grass.NormalTextureAssetGuid.Set(GUID("bbbbbbbb-cccc-dddd-eeee-ffffffffffff"));

    const Components::TerrainGrass before = grass;
    ClampFields(grass);

    EXPECT_EQ(grass.RenderMode, before.RenderMode);
    EXPECT_FLOAT_EQ(grass.PlacementSeed, before.PlacementSeed);
    EXPECT_FLOAT_EQ(grass.WindDirection, before.WindDirection);
    EXPECT_FLOAT_EQ(grass.WindRestingLean, before.WindRestingLean);
    EXPECT_FLOAT_EQ(grass.WindSeed, before.WindSeed);
    EXPECT_EQ(grass.TextureGrass, before.TextureGrass);
    EXPECT_EQ(grass.UseSplatRootColor, before.UseSplatRootColor);
    EXPECT_EQ(grass.RootColor, before.RootColor);
    EXPECT_EQ(grass.TipColor, before.TipColor);
    EXPECT_EQ(grass.BacklightColor, before.BacklightColor);
    EXPECT_EQ(grass.AlbedoTextureAssetGuid, before.AlbedoTextureAssetGuid);
    EXPECT_EQ(grass.AlphaTextureAssetGuid, before.AlphaTextureAssetGuid);
    EXPECT_EQ(grass.NormalTextureAssetGuid, before.NormalTextureAssetGuid);
}

// The defaults are the shipped look. If any of them sat outside its own bound the pass would
// silently retune every terrain in the project the first time anything wrote to it.
TEST(TerrainGrassClamps, DefaultsAreAlreadyInRange)
{
    Components::TerrainGrass grass{};
    const Components::TerrainGrass defaults = grass;
    ClampFields(grass);
    EXPECT_EQ(std::memcmp(&grass, &defaults, sizeof(grass)), 0)
        << "a TerrainGrass default sits outside the bound this pass enforces";
}

// Callers run the pass after a write of any shape without tracking which fields they touched, so
// running it twice must be indistinguishable from running it once.
TEST(TerrainGrassClamps, PassIsIdempotent)
{
    Components::TerrainGrass grass{};
    grass.BladesPerSquareMeter = -5.0f;
    grass.RootShade = 4.0f;
    grass.BladeSegments = 999u;
    grass.LayerIndex = 99u;
    grass.WindGustScale = -1.0f;
    grass.AtlasColumns = 0u;

    ClampFields(grass);
    const Components::TerrainGrass once = grass;
    ClampFields(grass);
    EXPECT_EQ(std::memcmp(&grass, &once, sizeof(grass)), 0);
}

// A non-finite value reaching the component is a caller bug, but it must not survive as one:
// NaN defeats std::clamp's ordering (both comparisons are false), so the pass has to be checked
// against it rather than assumed to handle it.
TEST(TerrainGrassClamps, NonFiniteInputsAreRecordedNotAssumed)
{
    Components::TerrainGrass grass{};
    grass.RootShade = std::numeric_limits<float32>::infinity();
    grass.BladeHeight = -std::numeric_limits<float32>::infinity();
    ClampFields(grass);
    EXPECT_FLOAT_EQ(grass.RootShade, 1.0f);
    EXPECT_FLOAT_EQ(grass.BladeHeight, 0.0f);

    // NaN is the case std::clamp cannot repair: it returns the value unchanged because every
    // comparison against it is false. Pinned so the behaviour is a recorded fact rather than a
    // surprise found in a shader.
    grass = Components::TerrainGrass{};
    grass.RootShade = std::numeric_limits<float32>::quiet_NaN();
    ClampFields(grass);
    EXPECT_TRUE(std::isnan(grass.RootShade));
}

// --- The single source stays single ------------------------------------------------------------

// The scene schema used to carry its own clamp per property, and the debug server's set_component
// carried a second copy that had already drifted (the grounding strength was bounded on load and unbounded
// over IPC). Both now call ClampFields. This scan is what stops the schema growing a third: a
// bound written inline here is a bound the other authoring paths never learn about.
//
// It catches the shape the drift took, not every conceivable bound — a limit expressed some other
// way (a rejecting parser, a magic sentinel) would pass this and still be invisible to the other
// writers. It is a regrowth guard on the known form, not a proof of absence.
TEST(TerrainGrassClamps, SceneSchemaCarriesNoInlineClampCall)
{
    const std::filesystem::path path = std::filesystem::path(TERRAIN_GRASS_SOURCE_DIR) / "Scene" /
                                       "TerrainGrassSceneSchemas.cpp";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "could not read " << path.string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string src = buffer.str();

    // Instrument check first: a scan whose patterns match nothing anywhere passes on any file.
    // The schema must still be calling the shared pass, or this test is vacuous.
    ASSERT_NE(src.find("ClampFields"), std::string::npos)
        << "TerrainGrassSceneSchemas.cpp no longer calls ClampFields — re-point this scan at "
           "whatever replaced it before trusting it.";

    for (const char* banned : {"std::clamp(", "std::max(", "std::min("})
    {
        EXPECT_EQ(src.find(banned), std::string::npos)
            << "TerrainGrassSceneSchemas.cpp bounds a field inline with `" << banned
            << "` — that bound is invisible to set_component and to every other writer. Put it in "
               "TerrainGrassClamps.h so all of them get it.";
    }
}

} // namespace GameEngine::TerrainGrass
