#pragma once

#include <vector>

#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Always-on Scene View gizmo for Light entities. Draws a camera-facing icon at
// each enabled Light's position so users can locate lights even when the
// entity has no mesh, plus a direction arrow and a volume wireframe (sphere
// for point, cone for spot) so the influence region and aim are visible.
// Selected and hovered lights are drawn with brighter alpha and thicker lines.
class LightGizmo : public IGizmo
{
public:
    LightGizmo() = default;

    void SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities);
    void SetHovered(GameEngine::ECS::EntityHandle entity);

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    GameEngine::ECS::EntityHandle              m_HoveredEntity{};
};

} // namespace GameEngine::Editor::SceneTools
