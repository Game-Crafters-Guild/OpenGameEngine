#include "Docking/LayoutPresetController.h"

#include "EditorDockNodeJson.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/LayoutPresetToolbar.h"
#include "UI/Layout/DockLayoutController.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

namespace GameEngine::Editor
{
namespace
{
UIElement* GetMainRoot(UIManager* mainUi)
{
    return mainUi ? mainUi->GetRootElement() : nullptr;
}

DockspaceElement* GetDockspace(UIManager* mainUi)
{
    UIElement* root = GetMainRoot(mainUi);
    return root ? dynamic_cast<DockspaceElement*>(root->FindById("dock")) : nullptr;
}

LayoutPresetToolbar* GetToolbar(UIManager* mainUi)
{
    UIElement* root = GetMainRoot(mainUi);
    return root
               ? dynamic_cast<LayoutPresetToolbar*>(root->FindById("LayoutButtonContainer"))
               : nullptr;
}
} // namespace

bool LayoutPresetController::Apply(DockingManager& docking,
                                   UIManager* mainUi,
                                   const DockNode* preset,
                                   bool rebuildDockspace)
{
    DockLayoutController layoutController(docking);
    if (!layoutController.ApplyForRegisteredPanels(preset))
        return false;

    if (rebuildDockspace)
    {
        if (auto* dockspace = GetDockspace(mainUi))
            dockspace->RequestRebuildFromModel();
    }
    return true;
}

bool LayoutPresetController::IsCurrentLayoutDirty(DockingManager& docking,
                                                  UIManager* mainUi,
                                                  const DockNode* preset)
{
    if (auto* dockspace = GetDockspace(mainUi))
        dockspace->SyncSplitRatiosFromUI();

    DockLayoutController layoutController(docking);
    auto sanitized = layoutController.CloneForRegisteredPanels(preset);
    return DockNodeToJson(docking.GetRoot()) != DockNodeToJson(sanitized.get());
}

void LayoutPresetController::SetActiveToolbarIndex(UIManager* mainUi, int activeIndex)
{
    if (auto* toolbar = GetToolbar(mainUi))
        toolbar->SetActiveIndex(activeIndex);
}

} // namespace GameEngine::Editor
