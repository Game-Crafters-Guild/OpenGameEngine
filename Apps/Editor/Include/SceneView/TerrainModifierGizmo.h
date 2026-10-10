#pragma once

#include <vector>

#include "ECS/Entity.h"
#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Selection-only Scene View gizmo for terrain modifier entities. When a modifier
// (Flatten / Noise / Stamp / PaintLayer / SculptZone / PaintZone) is selected or
// hovered it draws the modifier's footprint volume and its affected region
// projected onto the terrain surface — draped onto the heightfield for a planar
// terrain, projected radially onto the sphere for a planet. Following the light-
// gizmo precedent, nothing is drawn unless the entity is selected/hovered.
//
// The gizmo is strictly read-only: it reads modifier component fields (via
// ECS::Read<>, which the ECS enforces as const), the active terrain's domain, and
// the sphere/heightfield surface for draping. It never touches the modifier
// system's bake / dirty path, so drawing a gizmo causes zero re-bakes (the #530
// guarantee).
class TerrainModifierGizmo : public IGizmo
{
public:
    TerrainModifierGizmo() = default;

    void SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities);
    void SetHovered(GameEngine::ECS::EntityHandle entity);

    void Render(GizmoRenderContext& context) override;

private:
    std::vector<GameEngine::ECS::EntityHandle> m_SelectedEntities;
    GameEngine::ECS::EntityHandle              m_HoveredEntity{};
};

} // namespace GameEngine::Editor::SceneTools
