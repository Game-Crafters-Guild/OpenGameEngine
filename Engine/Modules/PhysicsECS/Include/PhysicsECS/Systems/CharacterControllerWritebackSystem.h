#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Copies character pose and grounded state back onto the entity.
class CharacterControllerWritebackSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "CharacterControllerWritebackSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace GameEngine::PhysicsECS
