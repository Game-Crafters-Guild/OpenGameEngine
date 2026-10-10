#include "PhysicsECS/Systems/PhysicsWorldBootstrapSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/PhysicsEntityEventCallbacks.h"

#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"

#include "ECS/Query.h"
#include "Logger/Logger.h"

namespace GameEngine::PhysicsECS
{
void PhysicsWorldBootstrapSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (PhysicsWorldService::IsInitialized())
        return;

    Physics::PhysicsWorldSettings settings{};
    bool foundSettings = false;
    int settingsCount = 0;

    world.Query<ECS::Read<GameEngine::Components::PhysicsWorldSettingsComponent>>().Each(
        [&](ECS::EntityHandle /*e*/, const GameEngine::Components::PhysicsWorldSettingsComponent& c)
        {
            ++settingsCount;
            if (!foundSettings)
            {
                settings = c.settings;
                foundSettings = true;
            }
        });

    if (settingsCount > 1)
    {
        Logger::Log::Warning("[PhysicsECS] Multiple PhysicsWorldSettingsComponent found ({}); using the first one encountered", settingsCount);
    }

    PhysicsWorldService::Initialize(settings);
    PhysicsEntityEventCallbacks::NotifyWorldStateChanged();
}
} // namespace GameEngine::PhysicsECS
