#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct CapsuleColliderShape
{
    float32 radius = 0.5f;
    float32 halfHeight = 0.5f;
    // 0=X, 1=Y, 2=Z (maps to Physics::CapsuleAxis)
    uint8 axis = 1;
};
} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::CapsuleColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail

