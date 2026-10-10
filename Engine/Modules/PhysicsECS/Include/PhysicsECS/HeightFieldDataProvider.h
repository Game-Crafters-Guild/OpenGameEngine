#pragma once

#include "Types/Types.h"

#include <vector>

namespace GameEngine::PhysicsECS
{

// Result returned by a HeightFieldDataProvider when resolving height data
// for a HeightFieldColliderShape. The provider fills this struct; the caller
// (PhysicsInitSystem) uses it to build a HeightFieldShapeDef or to update a
// region of an already-built shape in place.
struct HeightFieldData
{
    const float32* samples = nullptr; // Non-owning pointer; must remain valid until shape is created.
    uint32 sampleCount = 0;          // Width/height of the square sample grid.
    uint64 version = 0;              // Data version for change detection.

    // Union of the sample-space regions dirtied after the caller-supplied
    // sinceVersion cursor, clamped to the grid. Max bounds are exclusive.
    // When false, the caller must treat the whole grid as dirty.
    bool hasRegion = false;
    int32 regionMinX = 0;
    int32 regionMinZ = 0;
    int32 regionMaxX = 0;
    int32 regionMaxZ = 0;
};

// Callback type for resolving heightfield data from a handle.
// PhysicsInitSystem calls this during the gather phase for each
// HeightFieldColliderShape it encounters. sinceVersion is the consumer's
// cursor (the version its physics shape was last built/updated from); the
// provider reports the dirty region accumulated after it.
//
// TerrainECS registers its provider at engine startup so that PhysicsECS
// can build heightfield shapes without depending on the TerrainECS module.
using HeightFieldDataProvider = HeightFieldData(*)(uint32 handle, uint32 generation, uint64 sinceVersion);

// Register / query the global heightfield data provider.
void SetHeightFieldDataProvider(HeightFieldDataProvider provider);
HeightFieldDataProvider GetHeightFieldDataProvider();

} // namespace GameEngine::PhysicsECS
