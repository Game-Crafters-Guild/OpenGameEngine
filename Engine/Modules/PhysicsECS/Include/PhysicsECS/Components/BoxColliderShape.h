#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct BoxColliderShape
{
    float32 halfExtentsX = 0.5f;
    float32 halfExtentsY = 0.5f;
    float32 halfExtentsZ = 0.5f;
};
} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::BoxColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail

