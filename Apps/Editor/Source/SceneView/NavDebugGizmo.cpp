#include "SceneView/NavDebugGizmo.h"
#include "Mathematics/Vector3.h"
#include "PathfindingECS/NavigationService.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

void NavDebugGizmo::Render(GizmoRenderContext& context)
{
    if (!PathfindingECS::NavigationService::IsInitialized())
        return;

    const auto& debugData = PathfindingECS::NavigationService::GetDebugData();

    // Draw debug lines (grids, paths, agent radii, reservations)
    for (const auto& line : debugData.Lines)
    {
        context.DrawColoredLine(Mathematics::Vector3(line.X1, line.Y1, line.Z1),
                                Mathematics::Vector3(line.X2, line.Y2, line.Z2),
                                Color(line.R, line.G, line.B, line.A));
    }

    // Draw debug triangles (navmesh faces)
    if (!debugData.Triangles.empty())
    {
        // Triangles may have different colors, so batch by color.
        // For simplicity, draw each triangle individually since
        // DrawTriangles requires uniform color per call.
        context.SetTriangleLayer(1); // transparent layer for overlay

        for (const auto& tri : debugData.Triangles)
        {
            const Mathematics::Vector3 verts[3] = {
                Mathematics::Vector3(tri.X1, tri.Y1, tri.Z1),
                Mathematics::Vector3(tri.X2, tri.Y2, tri.Z2),
                Mathematics::Vector3(tri.X3, tri.Y3, tri.Z3)};
            context.DrawTriangles(verts, 3, Color(tri.R, tri.G, tri.B, tri.A));
        }
    }
}

} // namespace GameEngine::Editor::SceneTools
