#pragma once

#include <vector>

#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

class ReflectionProbeGizmo : public IGizmo
{
public:
    ReflectionProbeGizmo() = default;

    void SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities);
    void SetHovered(GameEngine::ECS::EntityHandle entity);

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    GameEngine::ECS::EntityHandle m_HoveredEntity{};
};

} // namespace GameEngine::Editor::SceneTools
