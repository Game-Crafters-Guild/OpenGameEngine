#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{

class DockNode;
class DockingManager;
class RenderDocCapture;
class UIElement;
enum class SettingsCategory : uint64_t;
namespace Editor { class FloatingFrameHost; }
namespace Rendering { class IDevice; }
namespace Rendering::RenderGraph { class RGFrame; }

/**
 * Manages editor panel lifecycle: showing, activating, and closing dock panels.
 * Extracted from EditorApplication to keep the application class focused on
 * initialization, rendering, and input dispatch.
 */
class EditorPanelManager
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    EditorPanelManager() = default;

    /// Set references to state owned by EditorApplication.
    void SetDocking(DockingManager* docking) { m_Docking = docking; }
    void SetPanelStorage(std::vector<std::unique_ptr<UIElement>>* storage) { m_PanelStorage = storage; }
    void SetWindows(std::vector<std::unique_ptr<EditorWindowContext>>* windows) { m_Windows = windows; }
    void SetIsLayoutDirtyCallback(std::function<bool()> callback) { m_IsLayoutDirty = std::move(callback); }
    /// The docking layer's in-canvas frames. ShowPanel asks it first: a panel
    /// that is currently a frame is raised, not docked a second time.
    void SetFloatingFrames(Editor::FloatingFrameHost* frames) { m_FloatingFrames = frames; }

    struct PanelMenuEntry
    {
        std::string panelId;
        std::string title;
        std::string iconClass;
        std::string iconPath;
    };

    /// Returns the live, sorted panel inventory used by editor panel launchers.
    std::vector<PanelMenuEntry> GetPanelMenuEntries() const;

    // -- Panel activation --

    /// Generic: show or activate a named panel in the given window context.
    /// If preferredLeafPanelId is non-empty, tries to place the new tab
    /// in the same leaf as that panel (falls back to first leaf).
    void ShowOrActivatePanel(EditorWindowContext* ctx, const std::string& panelId,
                             const std::string& preferredLeafPanelId = {});

    /// Opens a panel in its preferred workspace section. This placement is
    /// independent of the authored startup layout.
    void ShowOrActivatePanelAtDefaultPlacement(EditorWindowContext* ctx,
                                               const std::string& panelId);

    void ShowOrActivateUIDemoPanel(EditorWindowContext* ctx);
    void ShowOrActivateSettingsPanel(EditorWindowContext* ctx);
    /// Activates the Settings panel and shows `category`.
    void ShowOrActivateSettingsPanelToCategory(EditorWindowContext* ctx, SettingsCategory category);
    /// Activates the Settings panel and shows the EditorSettingsRegistry category `categoryId`.
    void ShowOrActivateSettingsPanelToRegistryCategory(EditorWindowContext* ctx, std::string_view categoryId);
    void ShowOrActivateScriptEditorPanel(EditorWindowContext* ctx);
    void ShowOrActivateGraphPanel(EditorWindowContext* ctx);
    void ShowOrActivateAnimationPanel(EditorWindowContext* ctx);
    void ShowOrActivateTodoPanel(EditorWindowContext* ctx);
    void ShowOrActivateBookmarksPanel(EditorWindowContext* ctx);
    void ShowOrActivateUndoHistoryPanel(EditorWindowContext* ctx);
    void ShowOrActivateBuildPanel(EditorWindowContext* ctx);
    void ShowOrActivateMixerPanel(EditorWindowContext* ctx);
    void ShowOrActivateWebPanel(EditorWindowContext* ctx);
    /// Opens the Web panel at the editor documentation URL in the main window.
    void ShowOrActivateHelpPanel();

    // -- Per-frame panel updates --
    // Panels register a callback once (typically in their constructor) so UpdatePanels
    // dispatches via a flat list rather than a dynamic_cast scan on every frame.
    struct PanelUpdateContext
    {
        Rendering::IDevice* device = nullptr;
        RenderDocCapture* renderDoc = nullptr;
        // Main window's live per-frame render graph (m_Windows[0]->RenderGraphStream.Frame).
        // Source for the render-graph / profiler / VRAM panels' introspection.
        Rendering::RenderGraph::RGFrame* rg2Frame = nullptr;
    };
    using UpdateCallback = std::function<void(const PanelUpdateContext&)>;
    // alwaysUpdate=false (default): UpdatePanels skips this entry when the
    // panel root has zero layout dimensions (inactive dock tab, hidden via
    // display:none, etc.). Set true for callbacks that must run even when
    // the panel UI is hidden — typically background data collection
    // (metric sampling, log capture queueing) that the user expects to
    // accumulate continuously and only render when the panel is reopened.
    void RegisterUpdateCallback(UIElement* panel, UpdateCallback callback, bool alwaysUpdate = false);
    void UnregisterUpdateCallback(UIElement* panel);
    void UpdatePanels(Rendering::IDevice* device,
                      RenderDocCapture* renderDoc,
                      Rendering::RenderGraph::RGFrame* rg2Frame);

    // -- Panel closing --
    void CloseActiveTabOrWindow(EditorWindowContext* ctx);

    // -- Dock tree helpers --
    static DockNode* FindFirstLeaf(DockNode* n);
    static DockNode* FindLeafContainingPanel(DockNode* n, const std::string& panelId);

    // -- Utility --
    template <typename T>
    T* FindFirstPanelOfType() const;

  private:
    friend class EditorApplication;
    void ClearUpdateCallbacks();

    /// Rebuild the "dock" DockspaceElement inside ctx after a model change.
    static void RebuildDockspace(EditorWindowContext* ctx);
    /// Rebuild immediately when a panel must be detached or mounted within the current event.
    static void RebuildDockspaceNow(EditorWindowContext* ctx, DockingManager* model);
    static void RefreshPanelUI(EditorWindowContext* ctx, const std::string& panelId);

    struct UpdateEntry
    {
        UIElement* panel = nullptr;
        UpdateCallback callback;
        bool alwaysUpdate = false;
    };

    DockingManager* m_Docking = nullptr;
    std::vector<std::unique_ptr<UIElement>>* m_PanelStorage = nullptr;
    std::vector<std::unique_ptr<EditorWindowContext>>* m_Windows = nullptr;
    std::vector<UpdateEntry> m_UpdateCallbacks;
    std::vector<UpdateEntry> m_PendingCallbacks;
    bool m_UpdatingPanels = false;
    std::function<bool()> m_IsLayoutDirty;
    Editor::FloatingFrameHost* m_FloatingFrames = nullptr;
};

// Template implementation
template <typename T>
T* EditorPanelManager::FindFirstPanelOfType() const
{
    if (!m_PanelStorage)
        return nullptr;
    for (const auto& p : *m_PanelStorage)
    {
        if (!p)
            continue;
        if (auto* casted = dynamic_cast<T*>(p.get()))
            return casted;
    }
    return nullptr;
}

} // namespace GameEngine
