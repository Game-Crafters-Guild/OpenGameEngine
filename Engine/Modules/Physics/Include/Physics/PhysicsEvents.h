#pragma once

#include "Physics/PhysicsQuery.h"

namespace GameEngine::Physics
{
enum class ContactState : uint8
{
    Begin,
    Persist,
    End,
};

struct ContactEvent
{
    BodyHandle bodyA{};
    BodyHandle bodyB{};
    uint64 userDataA = 0;
    uint64 userDataB = 0;
    ContactState state = ContactState::Persist;

    Vector3 point{0.0f, 0.0f, 0.0f};
    Vector3 normal{0.0f, 1.0f, 0.0f};
    float32 impulse = 0.0f;
};

struct TriggerEvent
{
    BodyHandle trigger{};
    BodyHandle other{};
    uint64 triggerUserData = 0;
    uint64 otherUserData = 0;
    bool isEntering = false;
};

} // namespace GameEngine::Physics

