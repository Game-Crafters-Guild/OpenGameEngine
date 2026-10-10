#include "SceneView/ComponentGizmoRegistry.h"

#include "ECS/Entity.h"

#include <algorithm>

namespace GameEngine::Editor::SceneTools
{

ComponentGizmoRegistry& ComponentGizmoRegistry::Get()
{
    static ComponentGizmoRegistry registry;
    return registry;
}

void ComponentGizmoRegistry::Register(ECS::ComponentTypeId typeId, ComponentGizmoDraw draw)
{
    const auto existing = std::find_if(m_Draws.begin(), m_Draws.end(),
                                       [typeId](const auto& entry) { return entry.first == typeId; });
    if (existing != m_Draws.end())
        existing->second = draw;
    else
        m_Draws.emplace_back(typeId, draw);
}

void ComponentGizmoRegistry::Draw(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity) const
{
    if (!world.IsValid(entity))
        return;
    for (const auto& [typeId, draw] : m_Draws)
    {
        if (draw && world.HasComponent(entity, typeId) && world.IsComponentEnabled(entity, typeId))
            draw(context, world, entity);
    }
}

} // namespace GameEngine::Editor::SceneTools
