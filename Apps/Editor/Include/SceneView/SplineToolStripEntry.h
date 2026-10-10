#pragma once

#include "SceneView/SceneViewToolStripRegistry.h"

#include <functional>
#include <memory>
#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;
namespace SceneTools
{
class SplineTool;
class TransformTool;
} // namespace SceneTools

// What a spline tool reads from the Scene View that hosts it.
struct SplineToolServices
{
    UndoRedoService* Undo = nullptr;
    EditorChangeNotifications* Notifications = nullptr;
    // The view's transform tool, which moves the selected knots.
    SceneTools::TransformTool* Transform = nullptr;
    // The view's selection, primary first; the tool edits a selected spline.
    std::function<const std::vector<ECS::EntityHandle>&()> Selection;
    // Selects the spline a finished stroke created.
    std::function<void(ECS::EntityHandle)> OnSplineCreated;
};

// A spline tool connected to its view's services.
std::unique_ptr<SceneTools::SplineTool> CreateSplineTool(ECS::World& world, SplineToolServices services);

// The spline tool's tool strip entry (id "spline"): its menu of spline editor settings,
// activation when the view selects a spline, and Frame Selected on a selected knot.
SceneViewToolStripEntry MakeSplineToolStripEntry();

} // namespace GameEngine::Editor
