#include "Editor/Hierarchy/HierarchyReparentRules.h"

#include "Components/RuntimeOnlyEntity.h"
#include "ECS/World.h"

namespace GameEngine::Editor
{

bool CanAcceptAuthoredChildren(ECS::World& world, ECS::EntityHandle destParent)
{
    if (!destParent.IsValid() || !world.IsValid(destParent))
        return true; // scene root
    return !world.HasComponent<Components::RuntimeOnlyEntity>(destParent);
}

} // namespace GameEngine::Editor
