#pragma once

#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

#include <vector>

namespace GameEngine::Editor::SceneTools
{

// The Scene View's gizmo for registered components (ComponentGizmoRegistry): draws each selected
// entity's, and the hovered entity's, registered component gizmos once per frame.
class ComponentGizmos : public IGizmo
{
public:
    void SetSelection(const std::vector<ECS::EntityHandle>& entities);
    void SetHovered(ECS::EntityHandle entity);

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<ECS::EntityHandle> m_SelectedEntities;
    ECS::EntityHandle m_HoveredEntity{};
};

} // namespace GameEngine::Editor::SceneTools
