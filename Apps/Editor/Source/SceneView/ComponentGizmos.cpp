#include "SceneView/ComponentGizmos.h"

#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "SceneView/ComponentGizmoRegistry.h"

#include <algorithm>

namespace GameEngine::Editor::SceneTools
{

void ComponentGizmos::SetSelection(const std::vector<ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void ComponentGizmos::SetHovered(ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void ComponentGizmos::Render(GizmoRenderContext& context)
{
    if (m_SelectedEntities.empty() && !m_HoveredEntity.IsValid())
        return;
    const ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const ComponentGizmoRegistry& registry = ComponentGizmoRegistry::Get();
    for (const ECS::EntityHandle entity : m_SelectedEntities)
        registry.Draw(context, *world, entity);
    // A hovered entity that is also selected is drawn once.
    if (m_HoveredEntity.IsValid() &&
        std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), m_HoveredEntity) == m_SelectedEntities.end())
        registry.Draw(context, *world, m_HoveredEntity);
}

} // namespace GameEngine::Editor::SceneTools
