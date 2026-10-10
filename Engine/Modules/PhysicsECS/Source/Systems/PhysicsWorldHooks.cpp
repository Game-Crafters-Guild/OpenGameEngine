#include "PhysicsECS/Systems/PhysicsWorldHooks.h"

#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"

#include "ECS/Entity.h"

namespace GameEngine::PhysicsECS
{

void ReleasePhysicsBody(Components::PhysicsBody& body)
{
    // Also runs from ~World at process shutdown, where the physics world may
    // already be gone. Clear the handles either way: leaving them set would
    // hand a revived component (undo of a delete) a body id that names nothing.
    auto* physicsWorld = PhysicsWorldService::TryGet();

    if (physicsWorld)
    {
        if (body.body.IsValid())
            physicsWorld->DestroyBody(body.body);
        if (body.shape.IsValid())
            physicsWorld->DestroyShape(body.shape);
        for (uint16 i = 0; i < body.childShapeCount; ++i)
        {
            if (body.childShapes[i].IsValid())
                physicsWorld->DestroyShape(body.childShapes[i]);
        }
    }

    body.body = {};
    body.shape = {};
    for (uint16 i = 0; i < body.childShapeCount; ++i)
        body.childShapes[i] = {};
    body.childShapeCount = 0;
    body.initialized = false;
}

void ReleaseCharacterController(Components::CharacterController& character)
{
    auto* physicsWorld = PhysicsWorldService::TryGet();
    if (physicsWorld && character.character.IsValid())
        physicsWorld->DestroyCharacter(character.character);

    GameEngine::Components::ClearCharacterControllerRuntimeState(character);
}

void RegisterPhysicsWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::PhysicsBody>(&ReleasePhysicsBody);
    world.RegisterOnRemove<Components::CharacterController>(&ReleaseCharacterController);
    world.EnableLifecycleEvents<Components::PhysicsBody>();
    world.EnableLifecycleEvents<Components::CharacterController>();
}

} // namespace GameEngine::PhysicsECS
