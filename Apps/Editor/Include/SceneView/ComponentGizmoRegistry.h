#pragma once

#include "ECS/ECS.h" // ComponentTypeId, EntityHandle

#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor::SceneTools
{
class GizmoRenderContext;

// Draws one component's Scene View gizmo for an entity that is selected or hovered.
using ComponentGizmoDraw = void (*)(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity);

// The Scene View gizmos drawn for a component on the entities an author has selected or hovers
// over, keyed by component type. The editor code that owns a component registers its drawing here,
// so the Scene View draws it without naming the component. Registration and drawing both happen on
// the main thread.
class ComponentGizmoRegistry
{
public:
    static ComponentGizmoRegistry& Get();

    // Replaces any drawing already registered for `typeId`.
    void Register(ECS::ComponentTypeId typeId, ComponentGizmoDraw draw);

    // Runs the drawing of every registered component `entity` carries and has switched on. A
    // switched-off component (its ECS::ComponentDisabled tag) draws nothing, so no registered drawing
    // checks the switch itself; the drawings read their component directly, which would see it.
    // The Scene View's query-driven gizmos (Light, DDGI volume, reflection probe) iterate queries,
    // which exclude a switched-off component by construction.
    void Draw(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity) const;

private:
    ComponentGizmoRegistry() = default;

    std::vector<std::pair<ECS::ComponentTypeId, ComponentGizmoDraw>> m_Draws;
};

} // namespace GameEngine::Editor::SceneTools
