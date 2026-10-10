#pragma once

#include "Components/Transform.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Optional per-entity sector index used for large/world-space partitioning.
// When absent, entities are implicitly treated as being in sector (0,0,0)
// and WorldTransform is interpreted as global world space.
// When present, WorldTransform is interpreted as sector-local and the
// true world position is derived from sector * sectorSize + localPosition.
// User-addable in the editor: tag an entity with a coarse integer sector so it
// can be authored far out at small local coordinates (the Inspector shows the
// composed true world position; the render/pick paths reconstruct it).
struct WorldSectorCoord
{
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;
};

// The ONE sector size (world meters per sector), shared by every system that
// gives WorldSectorCoord a meaning so they cannot disagree on "world space":
// the CPU composition below (Scene TLAS broad-phase + picker narrow-phase) AND
// the GPU camera-relative render reconstruction (RenderOrigin.h re-exports this
// as kSectorSize, and the vertex stage rebuilds sector*kSectorSize + local).
// Power-of-two so the render path's integer-sector delta scales exactly in fp32.
inline constexpr float32 kWorldSectorSize = 1024.0f;

// Single source of truth for sector-local -> world-space composition.
// Adds (sector * sectorSize) to the column-major translation column.
// Left-multiplication by a pure translation only shifts columns 12-14;
// the rotation/scale sub-matrix (cols 0-11) is preserved.
//
// Fast path: when sector is null or sector.x/y/z all zero, the input
// matrix is already world-space and is returned unchanged. Both the
// Scene TLAS broad-phase (SceneTlas.cpp) and the picker narrow-phase
// (MeshPickingService.cpp) call this so they always agree on what
// "world space" means for a sector-aware entity. Callers pass kWorldSectorSize
// (the TLAS default) so composition and the render path share one scale.
inline WorldTransform ComposeEffectiveWorldTransform(
    const WorldTransform& wt,
    const WorldSectorCoord* sector,
    float32 sectorSize)
{
    if (!sector || (sector->x == 0 && sector->y == 0 && sector->z == 0))
        return wt;
    WorldTransform out = wt;
    out.matrix[12] += static_cast<float32>(sector->x) * sectorSize;
    out.matrix[13] += static_cast<float32>(sector->y) * sectorSize;
    out.matrix[14] += static_cast<float32>(sector->z) * sectorSize;
    return out;
}

} // namespace Components
} // namespace GameEngine
