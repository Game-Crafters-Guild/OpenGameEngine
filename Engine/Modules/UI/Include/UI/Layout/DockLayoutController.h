#pragma once

#include <memory>
#include <string>

namespace GameEngine
{
class DockNode;
class DockingManager;

/// Edits docking layouts without making DockingManager responsible for
/// preset-specific cloning and pruning policy.
class DockLayoutController
{
  public:
    explicit DockLayoutController(DockingManager& docking);

    /// Clone a layout while dropping tabs for unregistered panels and branches
    /// that become empty.
    std::unique_ptr<DockNode> CloneForRegisteredPanels(const DockNode* source) const;

    /// Replace the active docking layout with the registered-panel subset.
    /// Returns false without changing the current layout when nothing survives.
    bool ApplyForRegisteredPanels(const DockNode* source);

  private:
    std::unique_ptr<DockNode> CloneForRegisteredPanelsRecursive(const DockNode* source) const;
    bool KeepPanel(const std::string& panelId) const;

    DockingManager& m_Docking;
};

} // namespace GameEngine
