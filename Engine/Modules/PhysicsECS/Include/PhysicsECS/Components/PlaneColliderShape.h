#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
// Plane: normal . x + d = 0 (negative half-space is solid)
struct PlaneColliderShape
{
    float32 normalX = 0.0f;
    float32 normalY = 1.0f;
    float32 normalZ = 0.0f;
    float32 d = 0.0f;
    float32 halfExtent = 1000.0f;
};
} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::PlaneColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail

