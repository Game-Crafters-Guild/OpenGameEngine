#pragma once
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

struct NavigationMesh
{
    float32 CellSize = 0.3f;
    float32 CellHeight = 0.2f;
    float32 AgentRadius = 0.6f;
    float32 AgentHeight = 2.0f;
    float32 AgentMaxClimb = 0.9f;
    float32 AgentMaxSlope = 45.0f;
    float32 RegionMinSize = 8.0f;
    float32 RegionMergeSize = 20.0f;
    float32 EdgeMaxLen = 12.0f;
    float32 EdgeMaxError = 1.3f;
    float32 DetailSampleDist = 6.0f;
    float32 DetailSampleMaxError = 1.0f;
    int32 VertsPerPoly = 6;

    // Optional asset reference (GUID bytes, all zeros = no asset)
    uint8 AssetGuid[16] = {};

    uint32 NavMapIndex = 0;
    uint32 NavMapGeneration = 0;
    bool Initialized = false;
    bool NeedsRebuild = true;
};

static_assert(std::is_trivially_copyable_v<NavigationMesh>);
static_assert(std::is_standard_layout_v<NavigationMesh>);

} // namespace GameEngine::Components
