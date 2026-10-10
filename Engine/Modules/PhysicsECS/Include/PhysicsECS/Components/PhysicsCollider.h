#pragma once

#include "Types/Types.h"

#include "Components/Transform.h"
#include "ECS/RequiredComponents.h"
#include "Physics/PhysicsTypes.h"

namespace GameEngine::Components
{
// ECS component describing common collider properties.
// Shape parameters live in separate *ColliderShape components.
struct PhysicsCollider
{
    // Broad collision layer for global matrix filtering.
    Physics::CollisionLayer layer = Physics::Layers::Dynamic;

    // Optional group/subgroup (future use).
    Physics::CollisionGroup collisionGroup{};

    // Optional fine masks (default: collide with everything).
    uint32 belongsToMask = 0xFFFFFFFFu;
    uint32 collidesWithMask = 0xFFFFFFFFu;

    // Trigger/sensor (non-solid).
    bool isTrigger = false;

    // Optional override material for this collider.
    bool overrideMaterial = false;
    Physics::PhysicsMaterial material{};
};

} // namespace GameEngine::Components

// Required components: a collider implies it has a Transform (identity if author doesn't set one).
namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::PhysicsCollider>
{
    using type = TypeList<GameEngine::Components::Transform>;
};
} // namespace GameEngine::ECS::Detail

