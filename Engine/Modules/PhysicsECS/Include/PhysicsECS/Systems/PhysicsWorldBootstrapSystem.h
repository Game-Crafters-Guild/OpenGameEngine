#pragma once

#include "ECS/Systems.h"

namespace GameEngine::PhysicsECS
{
// Ensures PhysicsWorldService is initialized for the current ECS world.
// If a PhysicsWorldSettingsComponent exists, it is used; otherwise defaults apply.
class PhysicsWorldBootstrapSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsWorldBootstrap"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};
} // namespace GameEngine::PhysicsECS

