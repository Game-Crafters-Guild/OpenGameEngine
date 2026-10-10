#pragma once

#include "Physics/PhysicsTypes.h"

namespace GameEngine::Physics
{
struct ConstraintSettingsBase
{
    BodyHandle bodyA{};
    BodyHandle bodyB{}; // invalid => world
    bool enableCollision = false;
};

struct DistanceConstraintSettings : ConstraintSettingsBase
{
    Vector3 anchorA{0.0f, 0.0f, 0.0f};
    Vector3 anchorB{0.0f, 0.0f, 0.0f};
    float32 minDistance = 0.0f;
    float32 maxDistance = 0.0f;
};

} // namespace GameEngine::Physics

