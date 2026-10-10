#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Runtime tag placed by TerrainPhysicsSystem on each per-tile heightfield
// collider entity (E6). A tiled terrain is one entity but needs one Jolt body
// per tile, so each Full tile gets its own collider entity; this component
// records which tile it serves and the tile-physics handle to release when the
// tile unloads or the collider is torn down. Not authored, not serialized —
// a pure runtime provisioning link.
struct TerrainTileCollider
{
    uint32 TiledIndex = 0;
    uint32 TiledGeneration = 0;
    int32 TileX = 0;
    int32 TileZ = 0;

    // HeightFieldColliderShape.dataHandle value (already OR'd with
    // TerrainService::kTilePhysicsHandleBit); passed to
    // TerrainService::ReleaseTilePhysicsHandle on teardown.
    uint32 PhysicsHandleIndex = 0;

    // HeightFieldColliderShape.dataGeneration value. Paired with
    // PhysicsHandleIndex so teardown release is generation-checked — a stale
    // index alone would silently free whichever slot currently owns it.
    uint32 PhysicsHandleGeneration = 0;
};

static_assert(std::is_trivially_copyable_v<TerrainTileCollider>,
              "TerrainTileCollider must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainTileCollider>,
              "TerrainTileCollider must be standard layout for ECS storage");

} // namespace GameEngine::Components
