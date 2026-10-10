#pragma once

#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"

#include "ECS/Entity.h"

namespace GameEngine::PhysicsECS
{
struct PhysicsWorldSettingsUpsertResult
{
    ECS::EntityHandle entity{};
    int foundCount = 0;
    bool created = false;
    bool updated = false;
};

// Ensures the world has exactly-one-authoritative settings component (singleton-style).
//
// Behavior:
// - If 1+ components exist: updates the first one found (if different) and returns it.
// - If none exist: creates an entity and adds the component (when createIfMissing=true).
//
// Note: if multiple exist, this does not delete extras (we just pick the first).
PhysicsWorldSettingsUpsertResult UpsertPhysicsWorldSettingsComponent(
    ECS::World& world,
    const Physics::PhysicsWorldSettings& settings,
    bool createIfMissing = true);

} // namespace GameEngine::PhysicsECS

