#pragma once

#include "ECS/SwapGenerationGuard.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine::PhysicsECS
{
// Creates/destroys backend characters and pushes desired velocity before Step.
class CharacterControllerSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "CharacterControllerSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Destroys the backend character of every CharacterController that went
    // off since the last swap, or of every one that is off when a window was
    // missed. Re-enabling needs nothing: the controller is visited again and
    // creates its character like a new one.
    void ReleaseDisabledCharacters(ECS::World& world);

    ECS::SwapGenerationGuard m_SwapGuard;
};

} // namespace GameEngine::PhysicsECS
