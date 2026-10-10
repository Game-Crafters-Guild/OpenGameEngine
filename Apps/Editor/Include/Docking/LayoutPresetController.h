#pragma once

namespace GameEngine
{
class DockNode;
class DockingManager;
class UIManager;

namespace Editor
{

/// Coordinates layout-preset edits with the main dockspace and toolbar.
/// Preset UI/model policy stays out of EditorApplication and DockingManager.
class LayoutPresetController
{
  public:
    static bool Apply(DockingManager& docking,
                      UIManager* mainUi,
                      const DockNode* preset,
                      bool rebuildDockspace);
    static bool IsCurrentLayoutDirty(DockingManager& docking,
                                     UIManager* mainUi,
                                     const DockNode* preset);
    static void SetActiveToolbarIndex(UIManager* mainUi, int activeIndex);
};

} // namespace Editor
} // namespace GameEngine
