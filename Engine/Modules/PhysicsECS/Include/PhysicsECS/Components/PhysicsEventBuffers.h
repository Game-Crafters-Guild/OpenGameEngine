#pragma once

#include "ECS/ECS.h"

#include "Physics/PhysicsEvents.h"

namespace GameEngine::Components
{
struct PhysicsContactEvent
{
    ECS::EntityHandle entityA{};
    ECS::EntityHandle entityB{};
    uint64 userDataA = 0;
    uint64 userDataB = 0;

    Physics::ContactState state = Physics::ContactState::Persist;
    Physics::Vector3 point{0.0f, 0.0f, 0.0f};
    Physics::Vector3 normal{0.0f, 1.0f, 0.0f};
    float32 impulse = 0.0f;
};

struct PhysicsTriggerEvent
{
    ECS::EntityHandle trigger{};
    ECS::EntityHandle other{};
    uint64 triggerUserData = 0;
    uint64 otherUserData = 0;
    bool isEntering = false;
};

// Singleton-style per-world buffers for physics events (cleared every frame).
// This keeps event routing allocation-free and lets gameplay systems consume events
// without registering callbacks directly.
struct PhysicsContactEventsBuffer
{
    // Keep this reasonably small so it remains friendly to ECS internal copy paths.
    // If we need more, we can switch to chunked buffers or a dedicated event stream.
    static constexpr uint32 kMaxEvents = 256;
    uint32 count = 0;
    PhysicsContactEvent events[kMaxEvents]{};

    void Clear()
    {
        count = 0;
    }

    bool Push(const PhysicsContactEvent& e)
    {
        if (count >= kMaxEvents)
            return false;
        events[count++] = e;
        return true;
    }
};

struct PhysicsTriggerEventsBuffer
{
    static constexpr uint32 kMaxEvents = 256;
    uint32 count = 0;
    PhysicsTriggerEvent events[kMaxEvents]{};

    void Clear()
    {
        count = 0;
    }

    bool Push(const PhysicsTriggerEvent& e)
    {
        if (count >= kMaxEvents)
            return false;
        events[count++] = e;
        return true;
    }
};

} // namespace GameEngine::Components

