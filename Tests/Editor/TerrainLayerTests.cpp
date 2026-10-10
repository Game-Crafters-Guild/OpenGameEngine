#include <gtest/gtest.h>

#include "Terrain/TerrainLayers.h"

#include <cstdint>

using namespace GameEngine;

namespace
{
constexpr const char* kTextureGuidText = "f78498e7-2643-4d6e-a345-d96c7bede21e";

} // namespace

// The blend order is fixed end-to-end (RGBA splat channel == layer index), so the names are a
// label for a fixed schema. A reordering here would mislabel every terrain in the editor.
TEST(TerrainLayers, NamesFollowTheSplatBlendOrder)
{
    ASSERT_EQ(TerrainLayers::kCount, 4u);
    EXPECT_STREQ(TerrainLayers::kNames[0], "Grass");
    EXPECT_STREQ(TerrainLayers::kNames[1], "Rock");
    EXPECT_STREQ(TerrainLayers::kNames[2], "Dirt");
    EXPECT_STREQ(TerrainLayers::kNames[3], "Snow");
}

// Clearing is the empty-picker path: the asset field hands back a null GUID, and the layer must
// go back to the tint fallback rather than binding a null texture.
TEST(TerrainLayers, SetAlbedoBindsAndClears)
{
    Components::Terrain terrain{};
    EXPECT_FALSE(TerrainLayers::IsTextured(terrain, 1));

    TerrainLayers::SetAlbedo(terrain, 1, GUID(kTextureGuidText));
    EXPECT_TRUE(TerrainLayers::IsTextured(terrain, 1));
    EXPECT_EQ(terrain.LayerAlbedoTexture[1].ToGuid(), GUID(kTextureGuidText));
    EXPECT_FALSE(TerrainLayers::IsTextured(terrain, 0));
    EXPECT_FALSE(TerrainLayers::IsTextured(terrain, 2));

    TerrainLayers::SetAlbedo(terrain, 1, GUID::Null());
    EXPECT_FALSE(TerrainLayers::IsTextured(terrain, 1));
    EXPECT_TRUE(terrain.LayerAlbedoTexture[1].IsNull());
}

TEST(TerrainLayers, SetAlbedoIgnoresOutOfRangeLayers)
{
    Components::Terrain terrain{};
    TerrainLayers::SetAlbedo(terrain, TerrainLayers::kCount, GUID(kTextureGuidText));
    for (uint32_t layer = 0; layer < TerrainLayers::kCount; ++layer)
        EXPECT_FALSE(TerrainLayers::IsTextured(terrain, layer)) << "layer " << layer;
}

// The scene schema clamps layerTiling to >= 0 on parse (TerrainSceneSchemas.cpp). The inspector
// must land on the same value, or a dragged-negative tiling would save as something the reload
// then changes.
TEST(TerrainLayers, SetTilingClampsAtZeroLikeTheSceneSchema)
{
    Components::Terrain terrain{};

    TerrainLayers::SetTiling(terrain, 2, 3.5f);
    EXPECT_FLOAT_EQ(terrain.LayerTiling[2], 3.5f);

    TerrainLayers::SetTiling(terrain, 2, -4.0f);
    EXPECT_FLOAT_EQ(terrain.LayerTiling[2], 0.0f);

    // Untouched layers keep their default multiplier.
    EXPECT_FLOAT_EQ(terrain.LayerTiling[0], 1.0f);
    EXPECT_FLOAT_EQ(terrain.LayerTiling[3], 1.0f);
}

TEST(TerrainLayers, SetHexTilingTouchesOnlyItsOwnBit)
{
    Components::Terrain terrain{};

    TerrainLayers::SetHexTiling(terrain, 2, true);
    EXPECT_EQ(terrain.LayerHexTiling, 1u << 2);
    EXPECT_TRUE(TerrainLayers::IsHexTiling(terrain, 2));
    EXPECT_FALSE(TerrainLayers::IsHexTiling(terrain, 1));

    TerrainLayers::SetHexTiling(terrain, 0, true);
    EXPECT_EQ(terrain.LayerHexTiling, (1u << 2) | 1u);

    TerrainLayers::SetHexTiling(terrain, 2, false);
    EXPECT_EQ(terrain.LayerHexTiling, 1u);
    EXPECT_TRUE(TerrainLayers::IsHexTiling(terrain, 0));
}

// The swatch is the card's honest statement of what an unbound layer looks like, so it has to be
// the sRGB encoding of the engine's linear tint — not the raw linear bytes, which would read far
// too dark. Expected values are the sRGB encode of Terrain::kDefaultTerrainMaterials, which the
// swatch reads directly (there is no editor-side copy of the palette), so a value change in the
// engine palette reds this test rather than silently making the card lie.
TEST(TerrainLayers, FallbackTintSwatchIsSrgbEncoded)
{
    EXPECT_EQ(TerrainLayers::FallbackTintSwatchArgb(0), 0xFFA0C476u); // grass
    EXPECT_EQ(TerrainLayers::FallbackTintSwatchArgb(1), 0xFFC4BCADu); // rock
    EXPECT_EQ(TerrainLayers::FallbackTintSwatchArgb(2), 0xFFBCA689u); // dirt
    EXPECT_EQ(TerrainLayers::FallbackTintSwatchArgb(3), 0xFFF3F6F9u); // snow

    // Every channel is brighter than the raw linear byte would be — the check that actually fails
    // if the encode is dropped.
    for (uint32_t layer = 0; layer < TerrainLayers::kCount; ++layer)
    {
        const uint32_t argb = TerrainLayers::FallbackTintSwatchArgb(layer);
        EXPECT_EQ(argb >> 24, 0xFFu) << "layer " << layer << " must be opaque";
        for (int channel = 0; channel < 3; ++channel)
        {
            const uint32_t encoded = (argb >> (16 - channel * 8)) & 0xFFu;
            const Terrain::TerrainMaterialRecord& material =
                Terrain::kDefaultTerrainMaterials[layer];
            const float linear = channel == 0   ? material.AlbedoR
                                 : channel == 1 ? material.AlbedoG
                                                : material.AlbedoB;
            const uint32_t rawLinear = static_cast<uint32_t>(linear * 255.0f);
            EXPECT_GT(encoded, rawLinear) << "layer " << layer << " channel " << channel;
        }
    }
}

