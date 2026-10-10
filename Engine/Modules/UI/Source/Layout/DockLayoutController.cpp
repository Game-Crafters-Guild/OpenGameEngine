#include "UI/Layout/DockLayoutController.h"

#include "UI/Layout/Docking.h"

#include <utility>

namespace GameEngine
{

DockLayoutController::DockLayoutController(DockingManager& docking)
    : m_Docking(docking)
{
}

bool DockLayoutController::KeepPanel(const std::string& panelId) const
{
    return m_Docking.GetPanel(panelId) != nullptr;
}

std::unique_ptr<DockNode> DockLayoutController::CloneForRegisteredPanels(
    const DockNode* source) const
{
    return CloneForRegisteredPanelsRecursive(source);
}

bool DockLayoutController::ApplyForRegisteredPanels(const DockNode* source)
{
    auto sanitized = CloneForRegisteredPanels(source);
    if (!sanitized)
        return false;

    m_Docking.SetRoot(std::move(sanitized));
    return true;
}

std::unique_ptr<DockNode> DockLayoutController::CloneForRegisteredPanelsRecursive(
    const DockNode* source) const
{
    if (!source)
        return nullptr;

    if (source->IsLeaf())
    {
        auto result = DockNode::MakeLeaf();
        for (const auto& panelId : source->GetTabs())
        {
            if (KeepPanel(panelId))
                result->AddTab(panelId);
        }

        const std::string& active = source->GetActivePanelId();
        if (!active.empty() && KeepPanel(active))
            (void)result->ActivateTab(active);

        if (result->GetTabs().empty())
            return nullptr;
        return result;
    }

    auto first = CloneForRegisteredPanelsRecursive(source->First());
    auto second = CloneForRegisteredPanelsRecursive(source->Second());
    if (!first)
        return second;
    if (!second)
        return first;

    auto result = std::make_unique<DockNode>();
    result->SetSplit(source->GetSplitDirection(),
                     source->GetSplitRatio(),
                     std::move(first),
                     std::move(second));
    result->SetMinChildSizes(source->GetMinFirstPx(), source->GetMinSecondPx());
    return result;
}

} // namespace GameEngine
