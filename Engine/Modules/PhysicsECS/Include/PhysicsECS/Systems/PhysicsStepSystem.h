#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
class PhysicsStepSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsStepSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};
} // namespace GameEngine::PhysicsECS

