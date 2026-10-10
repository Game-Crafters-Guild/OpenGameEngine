# Physics collision/trigger events (how to use)

This guide shows how gameplay code should consume physics collision/trigger events via ECS.

## Preferred approach (entity-scoped callbacks)

If you want “tell me when **this sword** hits something” (or “when **this trigger** is entered/exited”), use the entity-scoped callback router:

- `PhysicsECS/PhysicsEntityEventCallbacks.h`
- `GameEngine::PhysicsECS::PhysicsEntityEventCallbacks`

This avoids iterating/buffering every collision each frame, and **only enables backend event generation when something is subscribed**.

### Example: sword hit detection (solid contact Begin/End)

Register per-entity callbacks (for example inside a gameplay system when the sword is spawned):

```cpp
using namespace GameEngine;
using namespace GameEngine::PhysicsECS;

ECS::EntityHandle swordEntity = /* ... */;

// Called when the sword starts colliding with something solid.
const auto beginId = PhysicsEntityEventCallbacks::SubscribeContactBegin(
    swordEntity,
    [&](const EntityContactEvent& e)
    {
        // e.self == swordEntity
        // e.other is the entity we hit
        // e.point / e.normal provide contact details
    });

// Called when the sword stops colliding with that object (separates).
const auto endId = PhysicsEntityEventCallbacks::SubscribeContactEnd(
    swordEntity,
    [&](const EntityContactEvent& e)
    {
        // e.self == swordEntity
        // e.other is the entity we separated from
    });

// Later, if the sword is destroyed:
PhysicsEntityEventCallbacks::Unsubscribe(beginId);
PhysicsEntityEventCallbacks::Unsubscribe(endId);
```

### Example: sword trigger hitbox (Enter/Exit)

If your sword uses a trigger collider (sensor), subscribe to trigger events:

```cpp
using namespace GameEngine;
using namespace GameEngine::PhysicsECS;

ECS::EntityHandle swordHitboxEntity = /* ... */;

PhysicsEntityEventCallbacks::SubscribeTriggerEnter(
    swordHitboxEntity,
    [&](const EntityTriggerEvent& e)
    {
        // e.self == swordHitboxEntity
        // e.other is the entity that entered the trigger
        // e.selfIsTrigger tells you whether self was the trigger body
    });

PhysicsEntityEventCallbacks::SubscribeTriggerExit(
    swordHitboxEntity,
    [&](const EntityTriggerEvent& e)
    {
        // e.other is the entity that exited the trigger
    });
```

## The data model (ECS mapping)
- `PhysicsECS` reserves backend `BodySettings.userData` for the **ECS entity id**.
- The Physics backend emits events containing `userDataA/userDataB`, which PhysicsECS converts to `ECS::EntityHandle`.

## Buffered events (optional, “listen to everything”)

If you want to consume **all** contacts/triggers in a world (for analytics/debugging/global gameplay), you can opt into ECS buffers:

- `GameEngine::Components::PhysicsContactEventsBuffer`
- `GameEngine::Components::PhysicsTriggerEventsBuffer`

`PhysicsEventsSystem` routes physics callbacks into these buffers **only if both components exist on the same entity in the world**.

### Reading buffered events in ECS systems

### Find the singleton buffers

In a gameplay system you typically do:

```cpp
auto q = world.Query<
    ECS::Read<GameEngine::Components::PhysicsContactEventsBuffer>,
    ECS::Read<GameEngine::Components::PhysicsTriggerEventsBuffer>>();

q.Each([&](ECS::EntityHandle /*e*/,
           const GameEngine::Components::PhysicsContactEventsBuffer& contacts,
           const GameEngine::Components::PhysicsTriggerEventsBuffer& triggers)
{
    // iterate contacts.events[0..contacts.count)
    // iterate triggers.events[0..triggers.count)
});
```

Because these are singleton-style, you usually just use the first one encountered.

## Example: filtering buffered events for a specific entity

Assume:
- `ECS::EntityHandle swordEntity` identifies the sword.
- You want to detect when the sword begins contact with anything.

```cpp
for (uint32 i = 0; i < contacts.count; ++i)
{
    const auto& ev = contacts.events[i];

    if (ev.state != Physics::ContactState::Begin)
        continue;

    const bool swordIsA = (ev.entityA == swordEntity);
    const bool swordIsB = (ev.entityB == swordEntity);
    if (!swordIsA && !swordIsB)
        continue;

    const ECS::EntityHandle other = swordIsA ? ev.entityB : ev.entityA;

    // Example: apply damage by reading components on `other`:
    // if (auto* hp = world.GetComponent<Components::Health>(other)) { ... }
}
```

### Triggers for sword hitboxes (buffered)

```cpp
for (uint32 i = 0; i < triggers.count; ++i)
{
    const auto& ev = triggers.events[i];
    if (!ev.isEntering)
        continue;

    if (ev.trigger != swordEntity)
        continue;

    const ECS::EntityHandle hit = ev.other;
    // apply hit logic...
}
```

## Example: rolling tree damages anything it hits (entity-scoped)

If a specific entity is “the damaging object” (a rolling tree, a boulder, a car), register a contact-begin callback for **that entity**:

```cpp
using namespace GameEngine;
using namespace GameEngine::PhysicsECS;

ECS::EntityHandle treeEntity = /* ... */;

PhysicsEntityEventCallbacks::SubscribeContactBegin(
    treeEntity,
    [&](const EntityContactEvent& e)
    {
        // e.self == treeEntity
        const ECS::EntityHandle target = e.other;

        // Apply damage to target if it has health:
        // if (auto* hp = world.GetComponent<Components::Health>(target))
        //     hp->value -= damage;
    });
```

## “Listen to every collision in the world”

This is supported and cheap **only when enabled**:
- If no listeners/buffers are active, the backend does not record events.
- If enabled, cost is proportional to the number of contacts/triggers that occur.

In ECS, “listen to every collision” just means iterating the buffers without filtering.

## Notes / future improvements

- **Impulse/strength**: Jolt doesn’t provide the final impulse at `OnContactAdded`; we can add an impulse estimate or a post-solve pipeline later.

