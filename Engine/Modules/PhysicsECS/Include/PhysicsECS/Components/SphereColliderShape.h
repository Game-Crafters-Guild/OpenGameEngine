#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct SphereColliderShape
{
    float32 radius = 0.5f;
};
} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::SphereColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail

