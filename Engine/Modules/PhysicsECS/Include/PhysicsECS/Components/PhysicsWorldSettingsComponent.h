#pragma once

#include "Physics/PhysicsTypes.h"

namespace GameEngine::Components
{
// ECS singleton-style component: add this to (at most) one entity in a world to
// configure how the global PhysicsWorld is initialized.
//
// Notes:
// - PhysicsWorldSettings are applied when the PhysicsWorld is first created.
// - Changing settings at runtime generally requires recreating the PhysicsWorld.
struct PhysicsWorldSettingsComponent
{
    Physics::PhysicsWorldSettings settings{};
};
} // namespace GameEngine::Components

