#pragma once

// TerrainGrassClamps.h — the TerrainGrass DATA invariants, in one place.
//
// These are the bounds a stored component must satisfy however it was written, so every surface
// that lands raw values on a TerrainGrass ends by running this pass: the scene schema
// (TerrainGrassSceneSchemas.cpp) after parsing a property, and the debug server's set_component
// after the generic reflected write. A second hand-maintained copy is how two authoring paths
// come to disagree about what a valid component is.
//
// This is not every bound in the editor, and deliberately so. TerrainInspector.cpp carries its
// own per-widget limits — slider travel, step size, display ranges — which are choices about how
// far a drag goes, not statements about which values are storable; those stay with the widgets.
// What belongs here is the subset that is true of the DATA: the per-field ranges below, plus the
// atlas invariant relating three fields to each other, which the inspector enforced alone until
// every other writer could set a tile count no atlas grid could address.

#include "Components/Terrain/TerrainGrass.h"
#include "Terrain/TerrainTypes.h"

#include <algorithm>

namespace GameEngine::TerrainGrass
{

// A gust scale of zero divides in the wind noise lookup; this is the smallest scale the
// shader can evaluate, not a taste bound.
inline constexpr float32 kMinWindGustScale = 0.0001f;
// Atlas grid bounds. One tile is the degenerate "whole texture is one card" case; the
// ceiling is the widest grid the card-atlas UV packing addresses.
inline constexpr uint32 kMinGrassAtlasDimension = 1u;
inline constexpr uint32 kMaxGrassAtlasDimension = 16u;
inline constexpr uint32 kMinGrassAtlasTileCount = 1u;

// Bring every field of `grass` inside its valid range.
//
// Whole-component rather than per-field, and idempotent, so a caller runs it once after a
// write of any shape — one field, several, or a raw byte blob — without tracking which
// fields it touched. Fields with no meaningful bound (seeds, directions, colours, asset
// refs, the enum) are deliberately absent.
inline void ClampFields(Components::TerrainGrass& grass)
{
    grass.BladesPerSquareMeter = std::max(0.0f, grass.BladesPerSquareMeter);
    grass.Range = std::max(0.0f, grass.Range);
    grass.DensityFalloff = std::max(0.0f, grass.DensityFalloff);
    grass.ClumpSize = std::max(0.0f, grass.ClumpSize);
    grass.ClumpHeightVariance = std::clamp(grass.ClumpHeightVariance, 0.0f, 1.0f);
    grass.ClumpAlignment = std::clamp(grass.ClumpAlignment, 0.0f, 1.0f);
    grass.ClumpGather = std::clamp(grass.ClumpGather, 0.0f, 1.0f);
    grass.BladeHeight = std::max(0.0f, grass.BladeHeight);
    grass.BladeWidth = std::max(0.0f, grass.BladeWidth);
    grass.MaxWidthRatio = std::max(0.0f, grass.MaxWidthRatio);
    grass.BladeSegments = std::clamp(grass.BladeSegments,
                                     Components::kMinTerrainGrassBladeSegments,
                                     Components::kMaxTerrainGrassBladeSegments);
    grass.RandomScale = std::max(0.0f, grass.RandomScale);
    grass.LayerIndex = std::min(grass.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u);
    grass.MaskThreshold = std::clamp(grass.MaskThreshold, 0.0f, 1.0f);
    grass.WindGustSpeed = std::max(0.0f, grass.WindGustSpeed);
    grass.WindGustScale = std::max(kMinWindGustScale, grass.WindGustScale);
    grass.WindStrength = std::max(0.0f, grass.WindStrength);
    grass.WindFlutterAmount = std::max(0.0f, grass.WindFlutterAmount);
    grass.WindFlutterSpeed = std::max(0.0f, grass.WindFlutterSpeed);
    grass.Brightness = std::max(0.0f, grass.Brightness);
    grass.RandomBrightness = std::max(0.0f, grass.RandomBrightness);
    grass.HueVariation = std::clamp(grass.HueVariation, 0.0f, 1.0f);
    grass.RootShade = std::clamp(grass.RootShade, 0.0f, 1.0f);
    grass.RootFadeStart = std::clamp(grass.RootFadeStart, Components::kMinTerrainGrassRootFadeStart,
                                     1.0f - Components::kMinTerrainGrassRootFadeSpan);
    grass.RootFadeEnd = std::clamp(grass.RootFadeEnd,
                                   grass.RootFadeStart + Components::kMinTerrainGrassRootFadeSpan, 1.0f);
    grass.BladeNormalForm = std::clamp(grass.BladeNormalForm, 0.0f, 1.0f);
    grass.BladeScatterGain = std::max(0.0f, grass.BladeScatterGain);
    grass.GroundingStrength = std::clamp(grass.GroundingStrength, 0.0f, 1.0f);
    grass.Translucency = std::max(0.0f, grass.Translucency);
    grass.TextureCardsPerSquareMeter = std::max(0.0f, grass.TextureCardsPerSquareMeter);
    grass.TextureSize = std::max(0.0f, grass.TextureSize);
    grass.AtlasColumns =
        std::clamp(grass.AtlasColumns, kMinGrassAtlasDimension, kMaxGrassAtlasDimension);
    grass.AtlasRows =
        std::clamp(grass.AtlasRows, kMinGrassAtlasDimension, kMaxGrassAtlasDimension);
    // Cross-field, and the reason this pass is whole-component: a tile count is an index space
    // into the grid the other two fields define, so it is only meaningful against them. Clamped
    // after both, so the ceiling is the corrected grid rather than whatever arrived.
    grass.AtlasTileCount = std::clamp(grass.AtlasTileCount, kMinGrassAtlasTileCount,
                                      grass.AtlasColumns * grass.AtlasRows);
    grass.AlphaCutoff = std::clamp(grass.AlphaCutoff, 0.0f, 1.0f);
    grass.NormalStrength = std::max(0.0f, grass.NormalStrength);
}

} // namespace GameEngine::TerrainGrass
