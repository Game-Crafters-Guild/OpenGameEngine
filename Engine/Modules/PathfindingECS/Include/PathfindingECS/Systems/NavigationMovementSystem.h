#pragma once
#include "ECS/Systems.h"

namespace GameEngine::PathfindingECS
{

class NavigationMovementSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "NavigationMovementSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    float32 m_AccumulatedTime = 0.0f;
};

} // namespace GameEngine::PathfindingECS
