#pragma once
#include "Types/Types.h"
#include "Pathfinding/PathfindingTypes.h"
#include <type_traits>

namespace GameEngine::Components
{

enum class NavigationBakeSource : uint8
{
    Physics = 0,
    RenderGeometry = 1,
    BoundingBoxes = 2,
    Manual = 3
};

struct NavigationGrid
{
    Pathfinding::GridType GridType = Pathfinding::GridType::Square;
    float32 CellSize = 1.0f;
    uint32 Width = 64;
    uint32 Depth = 64;
    float32 MaxSlope = 45.0f;
    float32 MaxStepHeight = 0.4f;
    float32 RaycastOriginHeight = 100.0f;

    NavigationBakeSource BakeSource = NavigationBakeSource::Physics;

    // Agent dimensions for volume-based obstacle detection during physics bake.
    // A box of (BakeAgentRadius, BakeAgentHeight, BakeAgentRadius) is tested above each
    // cell's ground height; any overlap marks the cell as blocked.
    float32 BakeAgentRadius = 0.5f;
    float32 BakeAgentHeight = 2.0f;

    // Optional asset reference (GUID bytes, all zeros = no asset)
    uint8 AssetGuid[16] = {};

    // Runtime binding to the NavigationWorld map this component created, never
    // scene data. Written only through PathfindingECS/NavigationGridRuntime.h.
    // OwnerWorldId stamps the creating world: a copy of a bound component in
    // another world names a map it does not own.
    uint32 NavMapIndex = 0;
    uint32 NavMapGeneration = 0;
    bool Initialized = false;
    bool NeedsRebake = true;
    uint64 OwnerWorldId = 0;
};

static_assert(std::is_trivially_copyable_v<NavigationGrid>);
static_assert(std::is_standard_layout_v<NavigationGrid>);

} // namespace GameEngine::Components
