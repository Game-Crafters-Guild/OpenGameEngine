#pragma once

#include "ECS/Systems.h"

namespace GameEngine::PhysicsECS
{
// Clears per-world physics event buffers and routes physics callbacks into ECS buffers.
// Must run before PhysicsStepSystem so buffers contain current-frame events.
class PhysicsEventsSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "PhysicsEventsSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};
} // namespace GameEngine::PhysicsECS

