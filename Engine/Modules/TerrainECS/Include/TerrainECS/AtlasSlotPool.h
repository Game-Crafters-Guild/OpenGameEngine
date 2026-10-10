#pragma once

#include "PageStreaming/ResidentSlotPool.h"
#include "TerrainECS/TerrainService.h" // TileCoord / TileCoordHash

namespace GameEngine::TerrainECS
{

// "Not resident" sentinel for an indirection row's slot (mirror CBT_ATLAS_NO_SLOT in
// cbt_atlas.glsl). A tile whose row holds this samples the coarse out-of-window fallback, never
// garbage (Risk 3).
inline constexpr uint32 kAtlasNoSlot = PageStreaming::kNoResidentSlot;

// The resident-window atlas's tile <-> slot pool: the shared residency pool keyed by tile. Splat and
// normal stay per tile until they move to pages; height takes the page cache's pool keyed by page.
using AtlasSlotPool = PageStreaming::ResidentSlotPool<TileCoord, TileCoordHash>;
using AtlasSlotRef = PageStreaming::ResidentSlotRef;

} // namespace GameEngine::TerrainECS
