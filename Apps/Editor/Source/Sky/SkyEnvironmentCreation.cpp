#include "Sky/SkyEnvironmentCreation.h"

#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kCreateSkyEnvironmentLabel = "Create Sky Environment";
constexpr const char* kSkyEnvironmentEntityName = "Sky Environment";

ECS::EntityHandle FirstDirectionalLight(ECS::World& world)
{
    for (const ECS::EntityHandle candidate : world.GetAliveEntitiesSnapshot())
    {
        const auto* light = world.GetComponent<Components::Light>(candidate);
        if (light && light->Type == Components::LightType::Directional)
            return candidate;
    }
    return {};
}

ECS::EntityHandle AddSkyEnvironmentEntity(ECS::World& world)
{
    Components::SkyEnvironment sky{};
    sky.SunLight = FirstDirectionalLight(world);

    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return entity;

    Components::Transform transform{};
    transform.SetIdentity();
    Components::Name name{};
    std::strncpy(name.value, kSkyEnvironmentEntityName, sizeof(name.value) - 1);

    world.AddComponentImmediate(entity, transform);
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, sky);
    world.AddComponentImmediate(entity, NextHierarchyOrderAtBottom(&world));
    return entity;
}
} // namespace

ECS::EntityHandle CreateSkyEnvironmentEntity(ECS::World& world, UndoRedoService* undo,
                                             EditorChangeNotifications* notifications)
{
    if (!undo)
        return AddSkyEnvironmentEntity(world);

    ECS::EntityHandle entity{};
    CommitGenericEdit(world, *undo, kCreateSkyEnvironmentLabel, [&]() {
        entity = AddSkyEnvironmentEntity(world);
        if (!entity.IsValid())
            return;
        // Created before the command is built, so it is committed as already applied: undo
        // removes the entity, redo revives the same handle (DuplicateEntitiesCommand).
        undo->CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
            kCreateSkyEnvironmentLabel, &world, notifications, std::vector<ECS::EntityHandle>{entity}, nullptr,
            nullptr));
    });
    return entity;
}

} // namespace GameEngine::Editor
