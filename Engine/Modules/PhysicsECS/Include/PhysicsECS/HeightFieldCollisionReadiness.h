#pragma once

#include "ECS/ECS.h"
#include "Physics/PhysicsTypes.h"

namespace GameEngine::Physics
{
class PhysicsWorld;
}

namespace GameEngine::PhysicsECS
{

// Why heightfield collision inside a box is not current yet: the entity to look
// at and the unmet condition, for a log line. Condition is a string literal.
struct HeightFieldCollisionWait
{
    ECS::EntityHandle Entity{};
    const char* Condition = "";
};

// Reports a height source that overlaps `box` but has no current collider shape
// yet: one whose HeightFieldColliderShape has not been provisioned, or whose shape
// no longer describes it. Registered by the module that owns the height sources.
// Returns true and fills `wait` for the first one found.
using PendingHeightFieldSource = bool (*)(ECS::World& world, const Physics::AABB& box,
                                          HeightFieldCollisionWait& wait);

// Registration happens during module initialization and shutdown on the main
// thread; queries only read the list.
void AddPendingHeightFieldSource(PendingHeightFieldSource source);
void RemovePendingHeightFieldSource(PendingHeightFieldSource source);

// True when every enabled heightfield collider whose footprint overlaps `box` has
// a live body built from its provider's current data version, and no registered
// source reports a collider still to be provisioned there. Without a registered
// HeightFieldDataProvider nothing can build or rebuild a heightfield, so none is
// waited for. Without a registered source, only colliders that already exist are
// checked. A collider that cannot be placed (missing or non-finite transform)
// counts as overlapping.
bool IsHeightFieldCollisionCurrent(ECS::World& world, const Physics::PhysicsWorld& physics,
                                   const Physics::AABB& box, HeightFieldCollisionWait& wait);

// World-space bounds of a heightfield of the given extent placed by `worldMatrix`
// (column-major, centred on its origin, heights along local Y in [0, heightScale]).
// Returns false when the matrix or an extent is not finite.
bool HeightFieldFootprint(const float32 worldMatrix[16], float32 sizeX, float32 sizeZ,
                          float32 heightScale, Physics::AABB& footprint);

// Closed-interval overlap of two boxes.
bool BoxesOverlap(const Physics::AABB& a, const Physics::AABB& b);

} // namespace GameEngine::PhysicsECS
