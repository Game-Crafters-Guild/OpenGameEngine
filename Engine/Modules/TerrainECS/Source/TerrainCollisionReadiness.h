#pragma once

#include "PhysicsECS/HeightFieldCollisionReadiness.h"

namespace GameEngine::TerrainECS
{

// PendingHeightFieldSource for planar terrain: reports an enabled terrain (or a
// resident Full tile of a tiled terrain) overlapping `box` whose collider has not
// been provisioned yet or no longer matches the terrain. Spherical terrain is not
// reported: its face colliders are checked once they exist, like any heightfield.
// Tiles that are not resident at Full detail have no collider by design and are
// not waited for.
bool FindPendingTerrainCollision(ECS::World& world, const Physics::AABB& box,
                                 PhysicsECS::HeightFieldCollisionWait& wait);

} // namespace GameEngine::TerrainECS
