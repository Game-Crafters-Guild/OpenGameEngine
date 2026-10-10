#include "SceneView/EditorSceneViewDropWiring.h"

#include "Panels/HierarchyPanel.h"
#include "Panels/SceneViewPanel.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/SceneViewDropUndoCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Entity.h"
#include "ECS/World.h"

#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

void WireSceneViewAssetDropUndo(SceneViewPanel* sceneViewPanel,
                                HierarchyPanel* hierarchyPanel,
                                UndoRedoService* undoRedo,
                                EditorChangeNotifications* notifications)
{
    if (!sceneViewPanel || !hierarchyPanel)
        return;

    sceneViewPanel->SetCaptureHierarchySelectionForDrop(
        [hierarchyPanel]()
        {
            return std::make_pair(hierarchyPanel->GetSelectionItemIds(), hierarchyPanel->GetSelectionAnchor());
        });

    sceneViewPanel->SetOnSceneAssetDropComplete(
        [hierarchyPanel, undoRedo, notifications](ECS::World* world,
                                                  const std::vector<ECS::EntityHandle>& droppedRoots,
                                                  const std::vector<UI::Interaction::ItemId>& selBefore,
                                                  UI::Interaction::ItemId anchorBefore)
        {
            if (!hierarchyPanel || !world || droppedRoots.empty())
            {
                return;
            }

            hierarchyPanel->Refresh();
            hierarchyPanel->SelectEntities(droppedRoots);

            if (!undoRedo)
            {
                return;
            }

            const std::vector<UI::Interaction::ItemId> selAfter = hierarchyPanel->GetSelectionItemIds();
            const UI::Interaction::ItemId anchorAfter = hierarchyPanel->GetSelectionAnchor();

            auto applySelection =
                [hierarchyPanel](const std::vector<UI::Interaction::ItemId>& ids, UI::Interaction::ItemId anchor)
            {
                hierarchyPanel->ApplySelectionProgrammatic(ids, anchor);
            };

            std::vector<ECS::EntityHandle> rootsCopy = droppedRoots;
            undoRedo->CommitAlreadyApplied(std::make_unique<SceneViewDropUndoCommand>(
                "Scene Drop",
                world,
                notifications,
                std::move(rootsCopy),
                selBefore,
                anchorBefore,
                selAfter,
                anchorAfter,
                std::move(applySelection)));
        });
}

} // namespace GameEngine::Editor
