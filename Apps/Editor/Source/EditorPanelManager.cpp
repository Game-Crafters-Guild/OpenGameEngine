#include "EditorPanelManager.h"

#include "Docking/FloatingFrameHost.h"
#include "EditorPanelIds.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Graph/GraphPanelFactory.h"
#include "Panels/GraphPanel.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UiContext.h"
#include "Panels/BuildPanel.h"
#include "Panels/SettingsPanel.h"
#include "Panels/WebPanel.h"
#include "SceneViewController.h"
#include "Editor/Settings/SceneViewSettings.h"

#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <typeinfo>

namespace GameEngine
{

std::vector<EditorPanelManager::PanelMenuEntry> EditorPanelManager::GetPanelMenuEntries() const
{
    std::vector<PanelMenuEntry> entries;
    if (!m_Docking)
        return entries;

    const UIManager* ui =
        (m_Windows && !m_Windows->empty() && (*m_Windows)[0]) ? (*m_Windows)[0]->ui.get() : nullptr;
    entries.reserve(m_Docking->GetPanels().size());
    for (const auto& [panelId, panelElement] : m_Docking->GetPanels())
    {
        const auto* panel = dynamic_cast<const DockPanel*>(panelElement);
        if (panel && !panel->IsListedInPanelMenus())
            continue;
        PanelMenuEntry entry;
        entry.panelId = panelId;
        entry.title = (panel && !panel->GetTitle().empty()) ? panel->GetTitle() : panelId;
        entry.iconClass = panel ? std::string(panel->GetTabIcon()) : std::string{};
        if (ui)
        {
            entry.iconPath = UIStyleQuery::ResolveClassBackgroundImageUrl(
                ui->GetStylesheets(), entry.iconClass);
            if (entry.iconPath.empty())
            {
                entry.iconPath = UIStyleQuery::ResolveClassBackgroundImageUrl(
                    ui->GetStylesheets(), "menulist-icon");
            }
        }
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const PanelMenuEntry& a, const PanelMenuEntry& b)
    {
        if (a.title != b.title)
            return a.title < b.title;
        return a.panelId < b.panelId;
    });
    return entries;
}

// ---- Static helpers ----

void EditorPanelManager::RebuildDockspace(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->ui)
        return;
    if (auto* rootEl = ctx->ui->GetRootElement())
    {
        if (auto* el = rootEl->FindById("dock"))
        {
            if (auto* ds = dynamic_cast<DockspaceElement*>(el))
            {
                ds->RequestRebuildFromModel();
            }
        }
    }
}

void EditorPanelManager::RebuildDockspaceNow(EditorWindowContext* ctx, DockingManager* model)
{
    if (!ctx || !ctx->ui || !model)
        return;
    UIElement* rootEl = ctx->ui->GetRootElement();
    if (!rootEl)
        return;
    auto* dockspace = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock"));
    if (!dockspace)
        return;

    dockspace->BindModel(model);
    UI::UiContextScope uiScope(ctx->ui->GetDispatcher(), ctx->ui->GetScheduler());
    dockspace->RebuildFromModel();
}

void EditorPanelManager::RefreshPanelUI(EditorWindowContext* ctx, const std::string& panelId)
{
    if (!ctx || !ctx->docking)
        return;

    // Panel launch can happen from a native context-menu callback while the
    // retained UI is otherwise idle. Explicitly refresh the activated subtree
    // and request a layout pass so both activation patches and newly-added tabs
    // are visible on the first frame after the menu closes.
    if (UIElement* panel = ctx->docking->GetPanel(panelId))
    {
        panel->MarkDirtySubtree(
            UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (ctx->ui)
        ctx->ui->RequestRelayout();
}

DockNode* EditorPanelManager::FindFirstLeaf(DockNode* n)
{
    if (!n)
        return nullptr;
    if (n->IsLeaf())
        return n;
    if (auto* a = FindFirstLeaf(n->First()))
        return a;
    return FindFirstLeaf(n->Second());
}

DockNode* EditorPanelManager::FindLeafContainingPanel(DockNode* n, const std::string& panelId)
{
    if (!n)
        return nullptr;
    if (n->IsLeaf())
    {
        for (const std::string& tab : n->GetTabs())
            if (tab == panelId)
                return n;
        return nullptr;
    }
    if (auto* a = FindLeafContainingPanel(n->First(), panelId))
        return a;
    return FindLeafContainingPanel(n->Second(), panelId);
}

// Build path (e.g. "0001") to the leaf that contains the given panel; path is sequence of '0'=First, '1'=Second.
static bool FindPathToLeafContainingPanel(DockNode* n, const std::string& panelId,
                                          const std::string& pathSoFar, std::string& outPath)
{
    if (!n)
        return false;
    if (n->IsLeaf())
    {
        for (const std::string& tab : n->GetTabs())
            if (tab == panelId)
            {
                outPath = pathSoFar;
                return true;
            }
        return false;
    }
    if (FindPathToLeafContainingPanel(n->First(), panelId, pathSoFar + "0", outPath))
        return true;
    return FindPathToLeafContainingPanel(n->Second(), panelId, pathSoFar + "1", outPath);
}

static const char* DefaultPanelLeafAnchor(const std::string& panelId)
{
    if (panelId == EditorPanelIds::Hierarchy ||
        panelId == EditorPanelIds::Settings || panelId == EditorPanelIds::Todo ||
        panelId == EditorPanelIds::Bookmarks)
        return EditorPanelIds::Hierarchy;

    if (panelId == EditorPanelIds::Inspector || panelId == EditorPanelIds::Inspector2 ||
        panelId == EditorPanelIds::UndoHistory || panelId == EditorPanelIds::Log)
        return EditorPanelIds::Inspector;

    /* Per-asset graph docks are "{kind tab}:{guid}" (MakeGraphDockIdForAsset).
       They belong with their kind tab in the centre workspace, not in whatever
       leaf happens to come first in the tree. */
    for (const char* graphTab : {EditorPanelIds::NodeGraph, EditorPanelIds::AnimationGraph,
                                 EditorPanelIds::GameLogicGraph})
    {
        const std::size_t len = std::strlen(graphTab);
        if (panelId.size() > len && panelId.compare(0, len, graphTab) == 0 && panelId[len] == ':')
            return EditorPanelIds::SceneView;
    }

    if (panelId == EditorPanelIds::SceneView || panelId == EditorPanelIds::GameView ||
        panelId == EditorPanelIds::NodeGraph || panelId == EditorPanelIds::AnimationGraph ||
        panelId == EditorPanelIds::GameLogicGraph || panelId == EditorPanelIds::Web ||
        panelId == EditorPanelIds::Diff || panelId == EditorPanelIds::UiDemo ||
        panelId == EditorPanelIds::ScriptEditor || panelId == EditorPanelIds::VisualProfiler ||
        panelId == EditorPanelIds::RenderGraph || panelId == EditorPanelIds::Vram)
        return EditorPanelIds::SceneView;

    if (panelId == EditorPanelIds::AssetView)
        return EditorPanelIds::AssetView;

    // Assets, script/CPU diagnostics, animation tools, project utilities, and
    // package-contributed panels default to the bottom workspace stack.
    return panelId == EditorPanelIds::Assets ? EditorPanelIds::ScriptErrors : EditorPanelIds::Assets;
}

// ---- Generic panel activation ----

void EditorPanelManager::ShowOrActivatePanelAtDefaultPlacement(
    EditorWindowContext* ctx, const std::string& panelId)
{
    // These panels intentionally own a dedicated split below Hierarchy. Their
    // specialized paths still restore the session's last-closed placement first.
    if (panelId == EditorPanelIds::Build)
    {
        ShowOrActivateBuildPanel(ctx);
        return;
    }
    if (panelId == EditorPanelIds::Mixer)
    {
        ShowOrActivateMixerPanel(ctx);
        return;
    }

    // Package panels declare their preferred workspace in their descriptor;
    // keep that registry policy in the panel manager rather than making the
    // application hub know about package-specific placement metadata.
    Editor::EditorPanelDescriptor descriptor;
    if (Editor::EditorPanelRegistry::Get().TryGetPanel(panelId, descriptor) &&
        !descriptor.DefaultDockAnchorPanelId.empty())
    {
        ShowOrActivatePanel(ctx, panelId, descriptor.DefaultDockAnchorPanelId);
        return;
    }

    ShowOrActivatePanel(ctx, panelId, DefaultPanelLeafAnchor(panelId));
}

void EditorPanelManager::ShowOrActivatePanel(EditorWindowContext* ctx, const std::string& panelId,
                                             const std::string& preferredLeafPanelId)
{
    if (!ctx || !ctx->docking)
        return;

    if (m_FloatingFrames && m_FloatingFrames->Raise(panelId))
        return;

    if (ctx->docking->ActivateTab(panelId))
    {
        // Try localized activation patch; fall back to full rebuild.
        bool patched = false;
        if (ctx->ui)
        {
            if (auto* rootEl = ctx->ui->GetRootElement())
            {
                if (auto* el = rootEl->FindById("dock"))
                {
                    if (auto* ds = dynamic_cast<DockspaceElement*>(el))
                        patched = ds->RequestActivationPatch(panelId);
                }
            }
        }
        if (!patched)
            RebuildDockspace(ctx);
    }
    else
    {
        if (m_IsLayoutDirty && m_IsLayoutDirty() && ctx->docking->RestoreLastClosedTab(panelId))
        {
            RebuildDockspace(ctx);
        }
        else
        {
            if (DockNode* root = ctx->docking->GetRoot())
            {
                DockNode* targetLeaf = nullptr;
                if (!preferredLeafPanelId.empty())
                    targetLeaf = FindLeafContainingPanel(root, preferredLeafPanelId);
                /* The caller's anchor can name a panel that was never docked in
                   this layout (a graph kind tab nobody opened yet). Fall back to
                   where this panel belongs by default before settling for the
                   first leaf in the tree, which is the Hierarchy corner. */
                if (!targetLeaf)
                    targetLeaf = FindLeafContainingPanel(root, DefaultPanelLeafAnchor(panelId));
                if (!targetLeaf)
                    targetLeaf = FindFirstLeaf(root);
                if (targetLeaf)
                {
                    targetLeaf->AddTab(panelId);
                    ctx->docking->ActivateTab(panelId);
                    RebuildDockspace(ctx);
                }
            }
        }
    }

    RefreshPanelUI(ctx, panelId);
}

// ---- Named panel activations (delegate to generic) ----

void EditorPanelManager::ShowOrActivateUIDemoPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::UiDemo, EditorPanelIds::SceneView);
}

void EditorPanelManager::ShowOrActivateSettingsPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::Settings, EditorPanelIds::Hierarchy);
}

void EditorPanelManager::ShowOrActivateSettingsPanelToCategory(EditorWindowContext* ctx,
                                                               SettingsCategory category)
{
    ShowOrActivateSettingsPanel(ctx);
    if (auto* panel = FindFirstPanelOfType<SettingsPanel>())
        panel->RequestShowCategory(category);
}

void EditorPanelManager::ShowOrActivateSettingsPanelToRegistryCategory(EditorWindowContext* ctx,
                                                                       std::string_view categoryId)
{
    ShowOrActivateSettingsPanel(ctx);
    if (auto* panel = FindFirstPanelOfType<SettingsPanel>())
        panel->RequestShowRegistryCategory(categoryId);
}

void EditorPanelManager::ShowOrActivateScriptEditorPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::ScriptEditor, EditorPanelIds::SceneView);
}

void EditorPanelManager::ShowOrActivateGraphPanel(EditorWindowContext* ctx)
{
    if (GraphPanel* last = GraphPanel::FindLiveLastOpened())
    {
        if (ctx && ctx->docking)
        {
            for (const auto& [id, element] : ctx->docking->GetPanels())
            {
                if (element == last)
                {
                    ShowOrActivatePanel(ctx, id, EditorPanelIds::SceneView);
                    return;
                }
            }
        }
    }
    ShowOrActivatePanel(ctx, EditorPanelIds::NodeGraph, EditorPanelIds::SceneView);
}

void EditorPanelManager::ShowOrActivateAnimationPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::Animation, EditorPanelIds::Assets);
}

void EditorPanelManager::ShowOrActivateTodoPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::Todo, EditorPanelIds::Hierarchy);
}

void EditorPanelManager::ShowOrActivateBookmarksPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::Bookmarks, EditorPanelIds::Hierarchy);
}

void EditorPanelManager::ShowOrActivateUndoHistoryPanel(EditorWindowContext* ctx)
{
    ShowOrActivatePanel(ctx, EditorPanelIds::UndoHistory, EditorPanelIds::Inspector);
}

void EditorPanelManager::ShowOrActivateBuildPanel(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->docking)
        return;

    if (ctx->docking->ActivateTab(EditorPanelIds::Build))
    {
        RebuildDockspace(ctx);
        RefreshPanelUI(ctx, EditorPanelIds::Build);
        // Defer version refresh so opening the Build tab doesn't stall the editor (avoids
        // synchronous settings I/O and RebuildRows in the same call stack).
        if (ctx->ui)
            if (UIElement* root = ctx->ui->GetRootElement())
                root->PostAction([this]() {
                    if (auto* buildPanel = FindFirstPanelOfType<BuildPanel>())
                        buildPanel->RefreshVersionsFromSettings();
                });
        return;
    }

    if (m_IsLayoutDirty && m_IsLayoutDirty() &&
        ctx->docking->RestoreLastClosedTab(EditorPanelIds::Build))
    {
        RebuildDockspace(ctx);
        RefreshPanelUI(ctx, EditorPanelIds::Build);
        return;
    }

    // When Build is not in the layout, dock it below Hierarchy (~70% / ~30% split).
    DockNode* root = ctx->docking->GetRoot();
    if (root)
    {
        std::string hierarchyPath;
        if (FindPathToLeafContainingPanel(root, "Hierarchy", "", hierarchyPath) &&
            !hierarchyPath.empty() &&
            ctx->docking->DockSplitSubtreeByPath(hierarchyPath, DockPosition::Bottom, EditorPanelIds::Build))
        {
            ctx->docking->SetSplitRatioByPath(hierarchyPath, 0.70f); // ~70% top (Hierarchy), ~30% bottom (Build)
            ctx->docking->ActivateTab(EditorPanelIds::Build);
            RebuildDockspace(ctx);
            RefreshPanelUI(ctx, EditorPanelIds::Build);
            if (ctx->ui)
                if (UIElement* rootEl = ctx->ui->GetRootElement())
                    rootEl->PostAction([this]() {
                        if (auto* buildPanel = FindFirstPanelOfType<BuildPanel>())
                            buildPanel->RefreshVersionsFromSettings();
                    });
            return;
        }
        // Fallback: add to first available leaf
        DockNode* targetLeaf = FindFirstLeaf(root);
        if (targetLeaf)
        {
            targetLeaf->AddTab(EditorPanelIds::Build);
            ctx->docking->ActivateTab(EditorPanelIds::Build);
            RebuildDockspace(ctx);
            RefreshPanelUI(ctx, EditorPanelIds::Build);
            if (ctx->ui)
                if (UIElement* rootEl = ctx->ui->GetRootElement())
                    rootEl->PostAction([this]() {
                        if (auto* buildPanel = FindFirstPanelOfType<BuildPanel>())
                            buildPanel->RefreshVersionsFromSettings();
                    });
            return;
        }
    }
}

void EditorPanelManager::ShowOrActivateMixerPanel(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->docking)
        return;

    if (ctx->docking->ActivateTab(EditorPanelIds::Mixer))
    {
        RebuildDockspace(ctx);
        RefreshPanelUI(ctx, EditorPanelIds::Mixer);
        return;
    }

    if (m_IsLayoutDirty && m_IsLayoutDirty() &&
        ctx->docking->RestoreLastClosedTab(EditorPanelIds::Mixer))
    {
        RebuildDockspace(ctx);
        RefreshPanelUI(ctx, EditorPanelIds::Mixer);
        return;
    }

    // When Mixer is not in the layout, open it in the same position as Build (below Hierarchy).
    DockNode* root = ctx->docking->GetRoot();
    if (root)
    {
        std::string hierarchyPath;
        if (FindPathToLeafContainingPanel(root, "Hierarchy", "", hierarchyPath) &&
            !hierarchyPath.empty() &&
            ctx->docking->DockSplitSubtreeByPath(hierarchyPath, DockPosition::Bottom, EditorPanelIds::Mixer))
        {
            // Smaller top ratio so Mixer opens ~30px taller than default (71.25% top / 28.75% bottom)
            ctx->docking->SetSplitRatioByPath(hierarchyPath, 0.7125f);
            ctx->docking->ActivateTab(EditorPanelIds::Mixer);
            RebuildDockspace(ctx);
            RefreshPanelUI(ctx, EditorPanelIds::Mixer);
            return;
        }
        // Fallback: add to first available leaf
        DockNode* targetLeaf = FindFirstLeaf(root);
        if (targetLeaf)
        {
            targetLeaf->AddTab(EditorPanelIds::Mixer);
            ctx->docking->ActivateTab(EditorPanelIds::Mixer);
            RebuildDockspace(ctx);
            RefreshPanelUI(ctx, EditorPanelIds::Mixer);
            return;
        }
    }
}

void EditorPanelManager::ShowOrActivateWebPanel(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->docking)
        return;

    // Open in center (same tab group as Scene/Game/NodeGraph view)
    ShowOrActivatePanel(ctx, EditorPanelIds::Web, EditorPanelIds::SceneView);

    if (auto* webPanel = FindFirstPanelOfType<WebPanel>())
    {
        if (ctx->window)
            webPanel->SetWindow(ctx->window.get());
    }
}

void EditorPanelManager::ShowOrActivateHelpPanel()
{
    if (!m_Windows || m_Windows->empty() || !m_Windows->front())
        return;

    EditorWindowContext* ctx = m_Windows->front().get();
    ShowOrActivateWebPanel(ctx);
    if (auto* webPanel = FindFirstPanelOfType<WebPanel>())
        webPanel->Navigate(WebPanel::kHelpDocsURL);
}

// ---- Panel closing ----

void EditorPanelManager::CloseActiveTabOrWindow(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->docking)
        return;

    auto* root = ctx->docking->GetRoot();
    if (!root)
        return;

    auto rebuildDockspace = [&]()
    {
        // Clearing focus is important: UIManager consumes keys whenever focusId is non-empty.
        // If the focused element lived inside a closed tab, focusId can become stale and "eat" keyboard input.
        if (ctx->ui)
        {
            ctx->ui->ClearFocus();
        }
        RebuildDockspace(ctx);
    };

    auto closePanelId = [&](const std::string& panelId) -> bool
    {
        if (panelId.empty())
            return false;
        if (!ctx->docking->RemoveTab(panelId))
            return false;

        rebuildDockspace();

        // If the docking tree is now empty and this is a floating window, close it.
        if (m_Windows && ctx->docking->IsEmpty() && !m_Windows->empty() && ctx != (*m_Windows)[0].get())
        {
            if (ctx->window)
                ctx->window->RequestClose();
        }
        return true;
    };

    // Prefer closing the tab that currently owns focus (e.g., focused TextField in Inspector).
    if (ctx->ui)
    {
        const std::string& focusId = ctx->ui->GetFocusedElementId();
        if (!focusId.empty())
        {
            // Fast-path: focus may be on a dock UI element id that encodes the panel id.
            auto stripPrefix = [&](const std::string& full, const char* prefix) -> std::string
            {
                const size_t n = std::strlen(prefix);
                if (full.size() >= n && full.rfind(prefix, 0) == 0)
                    return full.substr(n);
                return {};
            };

            std::string direct = stripPrefix(focusId, "tab:");
            if (direct.empty())
                direct = stripPrefix(focusId, "mount:");
            if (!direct.empty() && closePanelId(direct))
            {
                return;
            }

            // Otherwise, search the docking tree for a panel subtree containing the focused element id.
            std::function<bool(DockNode*, std::string&)> findFocusedPanel = [&](DockNode* n, std::string& outPanelId) -> bool
            {
                if (!n)
                    return false;
                if (n->IsLeaf())
                {
                    for (const auto& tabId : n->GetTabs())
                    {
                        if (tabId.empty())
                            continue;
                        UIElement* panel = ctx->docking->GetPanel(tabId);
                        if (panel && panel->FindById(focusId))
                        {
                            outPanelId = tabId;
                            return true;
                        }
                    }
                    return false;
                }
                return findFocusedPanel(n->First(), outPanelId) || findFocusedPanel(n->Second(), outPanelId);
            };

            std::string focusedPanelId;
            if (findFocusedPanel(root, focusedPanelId) && closePanelId(focusedPanelId))
            {
                return;
            }
        }
    }

    // Fallback: close the active tab in the first leaf.
    DockNode* leaf = FindFirstLeaf(root);
    if (!leaf)
        return;

    const std::string& activePanelId = leaf->GetActivePanelId();
    if (activePanelId.empty())
    {
        // No active tab to close; if this is a floating window, close the window itself.
        if (m_Windows && !m_Windows->empty() && ctx != (*m_Windows)[0].get()) // Don't close main window
        {
            if (ctx->window)
                ctx->window->RequestClose();
        }
        return;
    }

    (void)closePanelId(activePanelId);
}

void EditorPanelManager::RegisterUpdateCallback(UIElement* panel, UpdateCallback callback, bool alwaysUpdate)
{
    if (!panel || !callback)
        return;
    // Check the active list first regardless of whether we're iterating.
    for (auto& entry : m_UpdateCallbacks)
    {
        if (entry.panel == panel)
        {
            entry.callback = std::move(callback);
            entry.alwaysUpdate = alwaysUpdate;
            return;
        }
    }
    if (m_UpdatingPanels)
    {
        for (auto& entry : m_PendingCallbacks)
        {
            if (entry.panel == panel)
            {
                entry.callback = std::move(callback);
                entry.alwaysUpdate = alwaysUpdate;
                return;
            }
        }
        m_PendingCallbacks.push_back({panel, std::move(callback), alwaysUpdate});
        return;
    }
    m_UpdateCallbacks.push_back({panel, std::move(callback), alwaysUpdate});
}

void EditorPanelManager::UnregisterUpdateCallback(UIElement* panel)
{
    // If called during UpdatePanels, null the entry so the iterator stays valid;
    // UpdatePanels will skip nulled entries and compact the list afterwards.
    if (m_UpdatingPanels)
    {
        for (auto& entry : m_UpdateCallbacks)
            if (entry.panel == panel) { entry.panel = nullptr; entry.callback = nullptr; return; }
        auto pit = std::find_if(m_PendingCallbacks.begin(), m_PendingCallbacks.end(),
                                [panel](const UpdateEntry& e) { return e.panel == panel; });
        if (pit != m_PendingCallbacks.end())
            m_PendingCallbacks.erase(pit);
        return;
    }
    auto it = std::find_if(m_UpdateCallbacks.begin(), m_UpdateCallbacks.end(),
                           [panel](const UpdateEntry& e) { return e.panel == panel; });
    if (it != m_UpdateCallbacks.end())
        m_UpdateCallbacks.erase(it);
}

void EditorPanelManager::ClearUpdateCallbacks()
{
    m_UpdateCallbacks.clear();
    m_PendingCallbacks.clear();
}

void EditorPanelManager::UpdatePanels(Rendering::IDevice* device,
                                       RenderDocCapture* renderDoc,
                                       Rendering::RenderGraph::RGFrame* rg2Frame)
{
    GE_CPU_PROFILE_SCOPE("EditorPanelManager.UpdatePanels");
    const PanelUpdateContext ctx{device, renderDoc, rg2Frame};
    m_UpdatingPanels = true;

    // Per-panel breakdown: each callback gets its own CpuProfiler scope named by the
    // panel's RTTI class name. typeid(T).name() returns a NUL-terminated const char*
    // with static storage duration, which is exactly what ScopedCpuProfile requires
    // of a scope name (it is retained as a map key and handed to NVTX).
    // We also aggregate a worst-panel log with both class name and id for easy
    // identification of the heaviest single panel this frame.
    using Clock = std::chrono::steady_clock;
    double worstMs = 0.0;
    const char* worstPanelClass = "<null>";
    std::string worstPanelId;
    {
        GE_CPU_PROFILE_SCOPE("EditorPanelManager.UpdatePanels.Callbacks");
        for (const auto& entry : m_UpdateCallbacks)
        {
            if (!entry.callback)
                continue;
            // Visibility gate: skip panels with no layout extent (inactive
            // dock tab, display:none, not yet attached). Panels that need to
            // run regardless (e.g. metric sampling for chart history) opt out
            // via alwaysUpdate=true on RegisterUpdateCallback. Saves the
            // callback's per-frame work AND prevents downstream MarkDirty
            // pushes from invalidating the UI snapshot for hidden panels.
            if (!entry.alwaysUpdate && entry.panel &&
                (entry.panel->GetLayoutWidth() <= 0.0f ||
                 entry.panel->GetLayoutHeight() <= 0.0f))
                continue;
            const char* panelClass = entry.panel ? typeid(*entry.panel).name() : "<null>";
            const auto t0 = Clock::now();
            {
                ::GameEngine::Profiling::ScopedCpuProfile panelScope{panelClass};
                entry.callback(ctx);
            }
            const auto t1 = Clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms > worstMs)
            {
                worstMs = ms;
                worstPanelClass = panelClass;
                worstPanelId = entry.panel ? entry.panel->GetId() : std::string{"<null>"};
                if (worstPanelId.empty())
                    worstPanelId = "<no-id>";
            }
        }
    }
    if (worstMs >= 5.0)
    {
        Logger::Log::Info("[PanelPerf] Worst panel callback: {} id='{}' at {:.2f} ms",
                           worstPanelClass, worstPanelId, worstMs);
    }

    m_UpdatingPanels = false;
    // Compact entries nulled by UnregisterUpdateCallback re-entrancy.
    auto it = std::remove_if(m_UpdateCallbacks.begin(), m_UpdateCallbacks.end(),
                             [](const UpdateEntry& e) { return !e.panel; });
    m_UpdateCallbacks.erase(it, m_UpdateCallbacks.end());
    // Flush registrations deferred during iteration.
    for (auto& entry : m_PendingCallbacks)
        m_UpdateCallbacks.push_back(std::move(entry));
    m_PendingCallbacks.clear();
}

} // namespace GameEngine
