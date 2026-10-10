#include "Editor/Entities/EntityDisplayName.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine::Editor
{

std::string EntityDisplayName(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* name = world.GetComponent<Components::Name>(entity); name && !name->View().empty())
        return std::string(name->View());
    return EntityIdLabel(entity);
}

std::string EntityIdLabel(ECS::EntityHandle entity)
{
    return "Entity " + std::to_string(entity.id);
}

} // namespace GameEngine::Editor
