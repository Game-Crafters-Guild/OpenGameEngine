#pragma once

#include "ECS/Entity.h"

#include "Physics/PhysicsEvents.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Entity-scoped physics event routing.
//
// Purpose:
// - Allow gameplay code (C++ systems / scripts) to subscribe to events for a specific ECS entity
//   without iterating all collisions every frame.
// - Avoid buffering every collision by default.
//
// Notes:
// - PhysicsECS reserves backend `BodySettings.userData` for the ECS entity id, so we can map
//   physics events back to `ECS::EntityHandle` cheaply.
// - Callbacks are invoked on the main thread during `PhysicsWorld::Step()` (after the simulation step).
struct EntityContactEvent
{
    ECS::EntityHandle self{};
    ECS::EntityHandle other{};

    // The original physics event (state + contact point/normal/impulse).
    Physics::ContactState state = Physics::ContactState::Persist;
    Physics::Vector3 point{0.0f, 0.0f, 0.0f};
    Physics::Vector3 normal{0.0f, 1.0f, 0.0f};
    float32 impulse = 0.0f;

    // Raw userData values (primarily for debugging/diagnostics).
    uint64 selfUserData = 0;
    uint64 otherUserData = 0;
};

struct EntityTriggerEvent
{
    ECS::EntityHandle self{};
    ECS::EntityHandle other{};

    // True if this callback is being invoked from the trigger body's perspective.
    // False means `self` is the "other" body in the trigger pair.
    bool selfIsTrigger = false;

    bool isEntering = false;
    uint64 selfUserData = 0;
    uint64 otherUserData = 0;
};

class PhysicsEntityEventCallbacks
{
public:
    using SubscriptionId = uint64;

    using ContactCallback = GameEngine::Function<void(const EntityContactEvent&)>;
    using TriggerCallback = GameEngine::Function<void(const EntityTriggerEvent&)>;

    // Contacts (solid): separate callbacks for Begin and End.
    static SubscriptionId SubscribeContactBegin(ECS::EntityHandle entity, ContactCallback callback);
    static SubscriptionId SubscribeContactEnd(ECS::EntityHandle entity, ContactCallback callback);

    // Triggers: separate callbacks for Enter and Exit.
    // Note: callbacks fire for BOTH entities:
    // - If `entity` is the trigger body: `selfIsTrigger=true`.
    // - If `entity` is the other body: `selfIsTrigger=false`.
    static SubscriptionId SubscribeTriggerEnter(ECS::EntityHandle entity, TriggerCallback callback);
    static SubscriptionId SubscribeTriggerExit(ECS::EntityHandle entity, TriggerCallback callback);

    static void Unsubscribe(SubscriptionId id);
    static void UnsubscribeAllForEntity(ECS::EntityHandle entity);
    static void ClearAll();

    // Called by PhysicsECS when the global physics world becomes available/unavailable.
    // (You usually don't need to call this manually.)
    static void NotifyWorldStateChanged();
};

} // namespace GameEngine::PhysicsECS

