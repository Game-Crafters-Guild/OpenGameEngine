#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Copies simulated body transforms back into ECS Transform + WorldTransform.
class PhysicsWritebackSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsWritebackSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace GameEngine::PhysicsECS

