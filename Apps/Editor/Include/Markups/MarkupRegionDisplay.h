#pragma once

#include "ECS/Entity.h"
#include "MarkupECS/MarkupRegionDisplayCache.h"
#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <optional>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// What every region's ground shares in one frame: the scene's sea level and the terrain's ground
// revision. Read once per drawing, only when a region draws.
struct MarkupRegionFrameGround
{
    float32 SeaLevel = 0.0f;
    uint64 GroundRevision = 0;
};

// The scene's calm water height a region stands on where the ground is below it: the first enabled
// OceanSurface's (Ocean::ResolveOceanSeaLevel), else a terrain's authored Terrain::SeaLevel, else
// Components::kNoTerrainSeaLevel (no water). Spline water above sea level (a river, a mountain
// lake) is not counted: a region over it stands on its bed.
float32 ResolveMarkupSeaLevel(ECS::World& world);

MarkupRegionFrameGround ReadMarkupRegionFrameGround(ECS::World& world);

// The display of the region or path `region`, rebuilt in `cache` when its outline, placement, sea
// level or ground changed: its outline sampled at the spline drape's spacing and a region's lid
// points, each on the terrain under it (MarkupTerrainHeightAt's ray, the terrain only, so its walls
// or line stand on the terrain under a house or a tree, not on its roof, and on a hill of any height
// inside it), each height raised to the sea level. A path (no MarkupRegion) stands no height above
// its ground. Null for an entity without a mark-up's spline (Markup, a SplineComponent with live
// data, a WorldTransform). Main thread only.
const MarkupECS::MarkupRegionDisplayCache::Entry* ResolveMarkupRegionDisplay(
    ECS::World& world, MarkupECS::MarkupRegionDisplayCache& cache, ECS::EntityHandle region,
    const MarkupRegionFrameGround& frameGround, uint64 frame);

// The display of the region or path `entity` this frame (Application's frame count), from the
// bridge's cache: built there now for one no view has drawn (markup_get, the inspector). Null
// without a display or with an empty outline.
const MarkupECS::MarkupRegionDisplayCache::Entry* ResolveMarkupDisplayNow(ECS::World& world,
                                                                         const MarkupEditorBridge& bridge,
                                                                         ECS::EntityHandle entity);

// The terrain's height under the world point (x, z), the terrain only, from any altitude (the ray
// starts above any terrain); none off the terrain. Where a new region's label point stands, and
// the ground a region's display stands on.
std::optional<float32> MarkupTerrainHeightAt(ECS::World& world, const Mathematics::Vector2& xz);

} // namespace GameEngine::Editor
