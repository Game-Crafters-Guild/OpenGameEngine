#include "PhysicsECS/PhysicsWorldSettingsHelpers.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::PhysicsECS
{
PhysicsWorldSettingsUpsertResult UpsertPhysicsWorldSettingsComponent(
    ECS::World& world,
    const Physics::PhysicsWorldSettings& settings,
    bool createIfMissing)
{
    PhysicsWorldSettingsUpsertResult r{};

    ECS::EntityHandle first{};

    world.Query<ECS::Read<GameEngine::Components::PhysicsWorldSettingsComponent>>().Each(
        [&](ECS::EntityHandle e, const GameEngine::Components::PhysicsWorldSettingsComponent& /*c*/)
        {
            ++r.foundCount;
            if (!first.IsValid())
                first = e;
        });

    if (r.foundCount > 1)
    {
        Logger::Log::Warning(
            "[PhysicsECS] Multiple PhysicsWorldSettingsComponent found ({}); updating/using the first one encountered",
            r.foundCount);
    }

    if (first.IsValid())
    {
        r.entity = first;
        if (auto* c = world.GetComponentForWrite<GameEngine::Components::PhysicsWorldSettingsComponent>(first))
        {
            // Always update; this is called during initialization paths where caller intends
            // these settings to become authoritative defaults (unless overridden elsewhere).
            c->settings = settings;
            r.updated = true;
        }
        return r;
    }

    if (!createIfMissing)
        return r;

    r.entity = world.CreateEntity();
    world.AddComponentImmediate(r.entity, GameEngine::Components::PhysicsWorldSettingsComponent{settings});
    // Ensure a stable editor-facing name for the bootstrap physics settings entity.
    {
        using GameEngine::Components::Name;
        Name n{};
        std::memset(n.value, 0, sizeof(n.value));
        constexpr const char* kName = "Physics Settings";
        constexpr size_t kMaxCopy = sizeof(n.value) - 1;
        const size_t toCopy = std::min(kMaxCopy, std::strlen(kName));
        std::memcpy(n.value, kName, toCopy);
        n.value[toCopy] = '\0';
        world.AddComponentImmediate(r.entity, n);
    }
    r.created = true;
    r.updated = true;
    return r;
}
} // namespace GameEngine::PhysicsECS

