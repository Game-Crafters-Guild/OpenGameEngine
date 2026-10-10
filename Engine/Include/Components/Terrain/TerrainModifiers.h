#pragma once

#include "Components/AssetRef.h"
#include "Components/Terrain/TerrainModifierBlend.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Shape a terrain modifier's bounds resolve to. Not a component field: a
// TerrainModifierVolume authors TerrainVolumeShape, and the resolved model
// (ResolvedModifier::Shape) converts into this to pick a weight ramp.
enum class TerrainModifierShape : uint8
{
    Circle = 0,     // XZ circle centered on entity position
    Rectangle = 1,  // Axis-aligned rectangle (respects entity Y rotation)
    Spline = 2,     // Along a spline path (requires SplineComponent on same entity)
};

// ---- Paintable zone modifiers (edit-pipeline design §3.2) ----
//
// A zone is the brush-authored generalization of the stamp: a transformable
// rectangular entity whose effect comes from a GUID-referenced payload asset
// (TerrainZoneData) sized to the zone's bounds and sampled in zone-local UV
// during bake. Moving / Y-rotating / scaling the entity moves the effect — the
// bake inverse-transforms each terrain sample into the zone, exactly as stamps
// do, and the transform's XZ scale multiplies the local extents. Deleting the
// zone restores what's beneath. Zones default to a high priority so new brush
// strokes sit atop the existing modifier stack ("read baked, write offset" is
// exact only while the sculpt zone is effectively topmost — design §3.2).
//
// The payload's DataVersion and per-stroke dirty rect live in the TerrainService
// zone-payload store (keyed by the payload GUID), not on the component: a brush
// stroke mutates the payload, never any ECS component field.

// Sculpt zone: R32F height-offset field. height += offset(zoneUV) * falloff at
// the zone's stack priority. Written by the Raise / Lower sculpt brushes.
struct TerrainSculptZone
{
    // Payload asset (R32F height offsets, world units). Null until the first
    // stroke auto-creates one.
    Components::AssetRef<AssetType::TerrainZoneData> PayloadRef;

    // Local half-extents in meters, before the transform's XZ scale multiplies
    // them. The payload texture maps across [-Extent, +Extent] in each axis.
    float32 ExtentX = 32.0f;
    float32 ExtentZ = 32.0f;

    // Falloff distance beyond the rect edge (0 = hard edge; brush strokes bake
    // their own soft edges into the payload, so zones default to a small skirt).
    float32 Falloff = 4.0f;

    // Priority in the modifier stack (higher = applied later). Zones default
    // above the existing modifiers.
    float32 Priority = 1000.0f;

    // Add (default) accumulates the offset; Set blends the offset toward it as
    // an absolute contribution. Only Add/Set are meaningful for sculpt zones —
    // the zone bakes its payload on its own path, and every other mode
    // (Subtract and the union operators) falls through to Add there.
    TerrainModifierBlend Blend = TerrainModifierBlend::Add;
    uint8 _Pad[3] = {};
};

static_assert(std::is_trivially_copyable_v<TerrainSculptZone>);
static_assert(std::is_standard_layout_v<TerrainSculptZone>);

// Paint zone: R8 weight mask + target splat layer index. Splat weight blend at
// the zone's priority (PaintLayer math, mask-scaled). Written by the Paint brush.
struct TerrainPaintZone
{
    // Payload asset (R8 weight mask, 0-255). Null until the first stroke.
    Components::AssetRef<AssetType::TerrainZoneData> PayloadRef;

    float32 ExtentX = 32.0f;
    float32 ExtentZ = 32.0f;
    float32 Falloff = 4.0f;
    float32 Priority = 1000.0f;

    // Which material layer (0-3) the mask paints into.
    uint32 LayerIndex = 0;

    // Overall paint strength; multiplies the mask weight (like PaintLayer).
    float32 Strength = 1.0f;
};

static_assert(std::is_trivially_copyable_v<TerrainPaintZone>);
static_assert(std::is_standard_layout_v<TerrainPaintZone>);

} // namespace GameEngine::Components
