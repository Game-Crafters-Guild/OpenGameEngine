#pragma once

#include "TerrainECS/TerrainService.h"

namespace GameEngine::TerrainECS
{

// Create terrain data with procedural noise heightfield. Sets handle on the component.
TerrainHandle CreateDefaultTerrainWithNoise(TerrainService& service,
                                             const Terrain::TerrainConfig& config,
                                             uint32 seed = 42);

} // namespace GameEngine::TerrainECS
