#pragma once

#include "SceneView/SceneViewGizmos.h"

namespace GameEngine::Editor::SceneTools
{

// Gizmo that renders pathfinding debug visualization (grids, navmeshes, paths,
// agent radii, reservations). Reads pre-computed line/triangle data from
// NavigationService::GetDebugData(), which is populated each frame by NavigationDebugSystem.
class NavDebugGizmo : public IGizmo
{
public:
    NavDebugGizmo() = default;
    void Render(GizmoRenderContext& context) override;
};

} // namespace GameEngine::Editor::SceneTools
