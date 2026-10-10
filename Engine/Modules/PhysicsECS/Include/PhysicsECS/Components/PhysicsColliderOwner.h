#pragma once

#include "ECS/ECS.h"

namespace GameEngine::Components
{
// If present, this collider entity contributes to the physics body on `body`.
// If absent, a collider is assumed to belong to its own entity (single-collider authoring).
struct PhysicsColliderOwner
{
    ECS::EntityHandle body{}; // invalid => self
};
} // namespace GameEngine::Components

