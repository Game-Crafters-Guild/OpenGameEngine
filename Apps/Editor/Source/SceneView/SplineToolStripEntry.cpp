#include "SceneView/SplineToolStripEntry.h"

#include "Components/Spline/SplineComponent.h"
#include "ECS/Entity.h"
#include "SceneView/SplineTool.h"
#include "SceneView/SplineToolMenu.h"
#include "SceneViewController.h"
#include "UI/EditorIcons.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{

const std::vector<ECS::EntityHandle> kNoSelection;

std::unique_ptr<SceneTools::ISceneTool> CreateSplineToolForView(SceneViewController& owner)
{
    SplineToolServices services;
    services.Undo = owner.GetUndoRedoService();
    services.Notifications = owner.GetChangeNotifications();
    services.Transform = owner.GetTransformTool();
    services.Selection = [&owner]() -> const std::vector<ECS::EntityHandle>& { return owner.GetSelectedEntities(); };
    services.OnSplineCreated = [&owner](ECS::EntityHandle entity) { owner.OnEntityPicked(entity); };
    return CreateSplineTool(owner.GetWorld(), std::move(services));
}

bool IsSplineEntitySelected(ECS::World& world, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& selection)
{
    if (primary.IsValid() && world.HasComponent<Components::SplineComponent>(primary))
        return true;
    for (const ECS::EntityHandle& entity : selection)
    {
        if (entity.IsValid() && world.HasComponent<Components::SplineComponent>(entity))
            return true;
    }
    return false;
}

} // namespace

std::unique_ptr<SceneTools::SplineTool> CreateSplineTool(ECS::World& world, SplineToolServices services)
{
    auto tool = std::make_unique<SceneTools::SplineTool>(world);
    tool->SetUndoRedoService(services.Undo);
    tool->SetChangeNotifications(services.Notifications);
    tool->SetTransformTool(services.Transform);
    if (!services.Selection)
        services.Selection = []() -> const std::vector<ECS::EntityHandle>& { return kNoSelection; };
    auto selection = std::move(services.Selection);
    tool->SetSelectionQuery([selection]() -> ECS::EntityHandle {
        const std::vector<ECS::EntityHandle>& entities = selection();
        return entities.empty() ? ECS::EntityHandle{} : entities.front();
    });
    tool->SetSelectionListQuery([selection]() -> std::vector<ECS::EntityHandle> { return selection(); });
    tool->SetOnSplineCreated(std::move(services.OnSplineCreated));
    return tool;
}

SceneViewToolStripEntry MakeSplineToolStripEntry()
{
    SceneViewToolStripEntry entry;
    entry.Id = "spline";
    entry.Tooltip = "Spline Tool (right-click for options)";
    entry.Icon = EditorIcons::kSpline;
    entry.ButtonClass = "spline-tool-btn";
    entry.CreateTool = &CreateSplineToolForView;
    entry.ContextMenuItems = &BuildSplineToolMenuItems;
    entry.ActivatesForSelection = &IsSplineEntitySelected;
    entry.FrameTarget = &SceneTools::SplineTool::GetActiveControlWorldPosition;
    return entry;
}

} // namespace GameEngine::Editor
