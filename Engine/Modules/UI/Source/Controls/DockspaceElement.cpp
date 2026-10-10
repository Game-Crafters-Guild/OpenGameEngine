#include "UI/Controls/DockspaceElement.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/SplitView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/DockLeaf.h"
#include "UI/Controls/DockTabBar.h"
#include "UI/Controls/DockTab.h"
#include "UI/Controls/DockPanel.h"
#include "UI/UIStyle.h"
#include "UI/StyleProperties.h"

#include "UI/Controls/DockOverlay.h"

#include <algorithm>

using namespace GameEngine;
using namespace GameEngine::Rendering::Geometry;

namespace
{
bool IsFloatingDockspace(const DockspaceElement* dockspace)
{
    const UIElement* cur = dockspace;
    int depth = 0;
    constexpr int kMaxDepth = 64;
    while (cur && depth++ < kMaxDepth)
    {
        if (cur->HasClass("floating"))
            return true;
        cur = cur->GetParent();
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void DockspaceElement::Fallback(DockPatchFallbackReason reason)
{
    ++m_Instrumentation.patchFallbacks;
    m_Instrumentation.lastFallbackReason = reason;
    RequestRebuildFromModel();
}

DockspaceElement::LeafView DockspaceElement::ResolveLeafByPanelId(const std::string& panelId)
{
    LeafView view{};

    std::function<void(UIElement*)> search = [&](UIElement* el)
    {
        if (!el || view.leaf)
            return;
        if (auto* leaf = dynamic_cast<DockLeaf*>(el))
        {
            for (auto& child : leaf->GetChildren())
            {
                if (child->HasClass("tabbar"))
                {
                    for (auto& tabChild : child->GetChildren())
                    {
                        if (tabChild->GetId() == std::string("tab:") + panelId)
                        {
                            view.leaf = leaf;
                            view.tabBar = child.get();

                            const std::string leafId = leaf->GetId();
                            const std::string prefix = "leaf:";
                            if (leafId.rfind(prefix, 0) == 0)
                                view.leafPath = leafId.substr(prefix.size());
                            break;
                        }
                    }
                }
                if (child->HasClass("dock-content"))
                    view.content = child.get();
            }
            if (view.leaf && view.content)
            {
                for (auto& contentChild : view.content->GetChildren())
                {
                    if (auto* m = dynamic_cast<Mount*>(contentChild.get()))
                    {
                        view.mount = m;
                        break;
                    }
                }
            }
            return;
        }
        for (auto& child : el->GetChildren())
            search(child.get());
    };
    search(this);
    return view;
}

DockspaceElement::LeafView DockspaceElement::ResolveLeafByPath(const std::string& leafPath)
{
    LeafView view{};
    const std::string leafId = "leaf:" + leafPath;

    std::function<void(UIElement*)> search = [&](UIElement* el)
    {
        if (!el || view.leaf)
            return;
        if (auto* leaf = dynamic_cast<DockLeaf*>(el))
        {
            if (leaf->GetId() == leafId)
            {
                view.leaf = leaf;
                view.leafPath = leafPath;
                for (auto& child : leaf->GetChildren())
                {
                    if (child->HasClass("tabbar"))
                        view.tabBar = child.get();
                    if (child->HasClass("dock-content"))
                        view.content = child.get();
                }
                if (view.content)
                {
                    for (auto& contentChild : view.content->GetChildren())
                    {
                        if (auto* m = dynamic_cast<Mount*>(contentChild.get()))
                        {
                            view.mount = m;
                            break;
                        }
                    }
                }
            }
            return;
        }
        for (auto& child : el->GetChildren())
            search(child.get());
    };
    search(this);
    return view;
}

void DockspaceElement::ApplyActiveTabVisuals(const LeafView& view, const std::string& activePanelId)
{
    if (!view.tabBar || !view.content || !view.leaf || !m_Model)
        return;

    for (auto& tabChild : view.tabBar->GetChildren())
    {
        bool isActive = (tabChild->GetId() == std::string("tab:") + activePanelId);
        if (isActive && !tabChild->HasClass("active"))
            tabChild->AddClass("active");
        else if (!isActive && tabChild->HasClass("active"))
            tabChild->RemoveClass("active");
    }
    if (auto* tabBar = dynamic_cast<DockTabBar*>(view.tabBar))
        tabBar->EnsureTabVisible(activePanelId);

    view.content->RemoveClass("overflow-visible");
    view.leaf->RemoveClass("overflow-visible");
    UIElement* panel = m_Model->GetPanel(activePanelId);
    if (panel)
    {
        if (auto* dp = dynamic_cast<DockPanel*>(panel))
        {
            if (dp->WantsDockContentOverflowVisible())
            {
                view.content->AddClass("overflow-visible");
                view.leaf->AddClass("overflow-visible");
            }
        }
    }

    if (view.mount && panel)
    {
        if (UIManager* owner = GetOwnerManager())
            panel->SetOwnerManager(owner);
        view.mount->SwapTargetForActivation(panel);
        view.mount->SetId(std::string("mount:") + activePanelId);
    }
}

// ---------------------------------------------------------------------------
// Patch methods
// ---------------------------------------------------------------------------

bool DockspaceElement::RequestActivationPatch(const std::string& panelId)
{
    if (!m_Model)
        return false;

    if (!m_Model->ActivateTab(panelId))
        return false;

    if (PatchActiveTabInLeaf(panelId))
    {
        ++m_Instrumentation.activationPatches;
        return true;
    }

    ++m_Instrumentation.patchFallbacks;
    RequestRebuildFromModel();
    return false;
}

bool DockspaceElement::PatchActiveTabInLeaf(const std::string& panelId)
{
    if (!m_Model)
        return false;

    UIElement* panel = m_Model->GetPanel(panelId);
    if (!panel)
        return false;

    // Find the Mount element whose content area contains this panel's leaf.
    // Mount IDs have the format "mount:<panelId>". The old mount has the
    // previously active panel's ID; we need to find it by searching leaf content areas.
    // Strategy: find all mount elements, locate the one in the same leaf as panelId.

    // Find the leaf containing this panel by checking all leaf elements
    UIElement* targetLeaf = nullptr;
    UIElement* targetTabBar = nullptr;
    UIElement* targetContent = nullptr;
    Mount* targetMount = nullptr;

    std::function<void(UIElement*)> findLeaf = [&](UIElement* el)
    {
        if (targetLeaf)
            return;
        if (!el)
            return;
        auto* leaf = dynamic_cast<DockLeaf*>(el);
        if (leaf)
        {
            // Check if this leaf's tab bar contains a tab for panelId
            for (auto& child : leaf->GetChildren())
            {
                if (child->HasClass("tabbar"))
                {
                    for (auto& tabChild : child->GetChildren())
                    {
                        const std::string& tabId = tabChild->GetId();
                        if (tabId == std::string("tab:") + panelId)
                        {
                            targetLeaf = leaf;
                            targetTabBar = child.get();
                            break;
                        }
                    }
                }
                if (child->HasClass("dock-content"))
                    targetContent = child.get();
            }
            if (targetLeaf)
            {
                // Find the existing mount in the content area
                if (targetContent)
                {
                    for (auto& contentChild : targetContent->GetChildren())
                    {
                        if (auto* m = dynamic_cast<Mount*>(contentChild.get()))
                        {
                            targetMount = m;
                            break;
                        }
                    }
                }
            }
            return;
        }
        for (auto& child : el->GetChildren())
            findLeaf(child.get());
    };
    findLeaf(this);

    if (!targetLeaf || !targetTabBar || !targetContent)
        return false;

    // Update tab "active" classes
    for (auto& tabChild : targetTabBar->GetChildren())
    {
        const std::string& tabId = tabChild->GetId();
        bool isActive = (tabId == std::string("tab:") + panelId);
        if (isActive && !tabChild->HasClass("active"))
            tabChild->AddClass("active");
        else if (!isActive && tabChild->HasClass("active"))
            tabChild->RemoveClass("active");
    }
    if (auto* tabBar = dynamic_cast<DockTabBar*>(targetTabBar))
        tabBar->EnsureTabVisible(panelId);

    // Update overflow-visible classes
    targetContent->RemoveClass("overflow-visible");
    targetLeaf->RemoveClass("overflow-visible");
    if (auto* dp = dynamic_cast<DockPanel*>(panel))
    {
        if (dp->WantsDockContentOverflowVisible())
        {
            targetContent->AddClass("overflow-visible");
            targetLeaf->AddClass("overflow-visible");
        }
    }

    // Swap mount target
    if (targetMount)
    {
        if (UIManager* owner = GetOwnerManager())
            panel->SetOwnerManager(owner);
        targetMount->SwapTargetForActivation(panel);
        targetMount->SetId(std::string("mount:") + panelId);
    }
    else
    {
        // No existing mount — create one (shouldn't normally happen for activation)
        auto mount = std::make_unique<Mount>();
        mount->SetId(std::string("mount:") + panelId);
        mount->AddClass("mount");
        if (UIManager* owner = GetOwnerManager())
            panel->SetOwnerManager(owner);
        mount->SetTarget(panel);
        targetContent->AddChild(std::move(mount));
    }

    return true;
}

static void ShiftTabSubtree(UIElement* el, float dx) {
    for (const auto& child : el->GetChildren()) {
        UIElement* c = child.get();
        UILayoutAccess::SetLastLayoutRect(*c, c->GetLayoutX() + dx, c->GetLayoutY(),
                             c->GetLayoutWidth(), c->GetLayoutHeight());
        ShiftTabSubtree(c, dx);
    }
}

bool DockspaceElement::RequestTabReorder(const std::string& leafPath)
{
    if (!m_Model)
        return false;

    LeafView view = ResolveLeafByPath(leafPath);
    if (!view.leaf || !view.tabBar)
    {
        Fallback(DockPatchFallbackReason::MissingUINodes);
        return false;
    }

    const DockNode* modelLeaf = m_Model->FindNodeByPath(leafPath);
    if (!modelLeaf || !modelLeaf->IsLeaf())
    {
        Fallback(DockPatchFallbackReason::TargetPathInvalidated);
        return false;
    }

    const auto& modelTabs = modelLeaf->GetTabs();
    auto& tabChildren = view.tabBar->GetMutableChildren();

    const size_t uiTabCount = static_cast<size_t>(std::count_if(
        tabChildren.begin(), tabChildren.end(), [](const auto& child) { return child && child->HasClass("tab"); }));
    if (uiTabCount != modelTabs.size())
    {
        Fallback(DockPatchFallbackReason::TabVectorMismatch);
        return false;
    }

    // Reorder existing DockTab children to match model order (move, don't recreate).
    for (size_t i = 0; i < modelTabs.size(); ++i)
    {
        const std::string expectedId = "tab:" + modelTabs[i];
        if (tabChildren[i]->GetId() == expectedId)
            continue;

        // Find the matching tab and swap it into position.
        bool found = false;
        for (size_t j = i + 1; j < modelTabs.size(); ++j)
        {
            if (tabChildren[j]->GetId() == expectedId)
            {
                // Swap layout X positions so hit testing is immediate (don't wait for Yoga)
                float xi = tabChildren[i]->GetLayoutX();
                float xj = tabChildren[j]->GetLayoutX();
                float wi = tabChildren[i]->GetLayoutWidth();
                float wj = tabChildren[j]->GetLayoutWidth();
                float yi = tabChildren[i]->GetLayoutY();
                float hi = tabChildren[i]->GetLayoutHeight();
                float yj = tabChildren[j]->GetLayoutY();
                float hj = tabChildren[j]->GetLayoutHeight();
                UILayoutAccess::SetLastLayoutRect(*tabChildren[i], xj, yi, wi, hi);
                UILayoutAccess::SetLastLayoutRect(*tabChildren[j], xi, yj, wj, hj);

                // Shift child subtrees so labels move with tabs
                ShiftTabSubtree(tabChildren[i].get(), xj - xi);
                ShiftTabSubtree(tabChildren[j].get(), xi - xj);

                std::swap(tabChildren[i], tabChildren[j]);
                found = true;
                break;
            }
        }
        if (!found)
        {
            Fallback(DockPatchFallbackReason::TabVectorMismatch);
            return false;
        }
    }

    // Reassign sequential X positions based on current widths to avoid gaps/overlaps
    {
        float curX = view.tabBar->GetLayoutX() + view.tabBar->GetLayoutPadding().Left;
        for (size_t i = 0; i < modelTabs.size(); ++i)
        {
            auto& child = tabChildren[i];
            float oldX = child->GetLayoutX();
            float w = child->GetLayoutWidth();
            float h = child->GetLayoutHeight();
            float y = child->GetLayoutY();
            UILayoutAccess::SetLastLayoutRect(*child, curX, y, w, h);
            float dx = curX - oldX;
            if (dx != 0.0f)
                ShiftTabSubtree(child.get(), dx);
            curX += w + 4.0f;
        }
    }

    view.tabBar->MarkDirty(ChildrenDirty | LayoutDirty);
    ++m_Instrumentation.reorderPatches;
    return true;
}

bool DockspaceElement::RequestTabRemoval(const std::string& panelId)
{
    if (!m_Model)
        return false;

    // Pre-mutation snapshot.
    LeafView view = ResolveLeafByPanelId(panelId);
    if (!view.leaf || !view.tabBar || !view.content)
    {
        Fallback(DockPatchFallbackReason::MissingUINodes);
        return false;
    }

    size_t tabCountBefore = 0;
    for (auto& ch : view.tabBar->GetChildren())
        if (ch->HasClass("tab")) ++tabCountBefore;

    bool wasActive = false;
    for (auto& ch : view.tabBar->GetChildren())
    {
        if (ch->GetId() == std::string("tab:") + panelId && ch->HasClass("active"))
        {
            wasActive = true;
            break;
        }
    }

    // Floating dockspaces with 2->1 tab transition need tabbar visibility change.
    const bool isFloating = IsFloatingDockspace(this);
    if (isFloating && tabCountBefore <= 2)
    {
        if (!m_Model->RemoveTab(panelId))
            return false;
        Fallback(DockPatchFallbackReason::FloatingTabbarTransition);
        return false;
    }

    // Mutate model.
    if (!m_Model->RemoveTab(panelId))
        return false;

    // Post-mutation: check if the leaf still exists.
    const DockNode* postLeaf = m_Model->FindNodeByPath(view.leafPath);
    if (!postLeaf || !postLeaf->IsLeaf() || postLeaf->GetTabs().empty())
    {
        Fallback(DockPatchFallbackReason::SourceLeafCollapsed);
        return false;
    }

    // Remove the DockTab element from the tab bar.
    UIElement* tabToRemove = nullptr;
    for (auto& ch : view.tabBar->GetChildren())
    {
        if (ch->GetId() == std::string("tab:") + panelId)
        {
            tabToRemove = ch.get();
            break;
        }
    }
    if (tabToRemove)
        view.tabBar->RemoveChild(tabToRemove);

    // If the closed tab was active, activate the model's new active tab.
    if (wasActive)
    {
        const std::string& newActive = postLeaf->GetActivePanelId();
        ApplyActiveTabVisuals(view, newActive);
    }

    ++m_Instrumentation.tabRemovalPatches;
    return true;
}

bool DockspaceElement::RequestTabAddition(const std::string& leafPath, const std::string& panelId)
{
    if (!m_Model)
        return false;

    // Floating dockspaces with 1->2 tab transition need tabbar creation.
    const bool isFloating = IsFloatingDockspace(this);
    if (isFloating)
    {
        LeafView preView = ResolveLeafByPath(leafPath);
        if (preView.leaf && preView.tabBar)
        {
            size_t tabCount = 0;
            for (auto& ch : preView.tabBar->GetChildren())
                if (ch->HasClass("tab")) ++tabCount;
            if (tabCount <= 1)
            {
                if (!m_Model->DockAsTabInLeafByPath(leafPath, panelId))
                    return false;
                m_Model->ActivateTab(panelId);
                Fallback(DockPatchFallbackReason::FloatingTabbarTransition);
                return false;
            }
        }
        else if (preView.leaf && !preView.tabBar)
        {
            // Single-tab floating leaf with hidden tabbar -- need full rebuild.
            if (!m_Model->DockAsTabInLeafByPath(leafPath, panelId))
                return false;
            m_Model->ActivateTab(panelId);
            Fallback(DockPatchFallbackReason::FloatingTabbarTransition);
            return false;
        }
    }

    // Mutate model.
    if (!m_Model->DockAsTabInLeafByPath(leafPath, panelId))
        return false;
    m_Model->ActivateTab(panelId);

    // Post-mutation: revalidate target path.
    const DockNode* postLeaf = m_Model->FindNodeByPath(leafPath);
    if (!postLeaf || !postLeaf->IsLeaf())
    {
        Fallback(DockPatchFallbackReason::TargetPathInvalidated);
        return false;
    }

    LeafView view = ResolveLeafByPath(leafPath);
    if (!view.leaf || !view.tabBar || !view.content)
    {
        Fallback(DockPatchFallbackReason::MissingUINodes);
        return false;
    }

    // Create new DockTab element.
    UIElement* panel = m_Model->GetPanel(panelId);
    std::string label = panelId;
    if (panel)
    {
        if (auto* dp = dynamic_cast<DockPanel*>(panel))
        {
            if (!dp->GetTitle().empty())
                label = dp->GetTitle();
        }
    }

    auto tab = std::make_unique<DockTab>();
    tab->AddClass("tab");
    tab->SetId(std::string("tab:") + panelId);
    tab->SetText(label);
    if (auto* dockTabBar = dynamic_cast<DockTabBar*>(view.tabBar))
        dockTabBar->AddTabChild(std::move(tab));
    else
        view.tabBar->AddChild(std::move(tab));

    ApplyActiveTabVisuals(view, panelId);
    view.tabBar->MarkDirty(ChildrenDirty | LayoutDirty);

    ++m_Instrumentation.tabAdditionPatches;
    return true;
}

void DockspaceElement::RebuildFromModel() {
    ++m_Instrumentation.fullRebuilds;

    m_DragGhost = nullptr; // About to be destroyed with children
    m_DragGhostLabel = nullptr;
    m_DragGhostUndockLabel = nullptr;
    m_DragGhostTab = nullptr;
    m_DragGhostBody = nullptr;

    // Clear existing children
    RemoveAllChildren();
    if (!m_Model) return;
    const DockNode* root = m_Model->GetRoot();
    if (!root) return;
    BuildFromNode(root, this, "");
    MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);

    // If debug is enabled, ensure overlay is present and updated immediately
    if (DockOverlay::IsDebugZonesEnabled()) {
        UIElement* overlayEl = FindById("dock-overlay");
        DockOverlay* ov = nullptr;
        if (!overlayEl) {
            auto ovNew = std::make_unique<DockOverlay>();
            ovNew->SetId("dock-overlay");
            ovNew->AddClass("overlay");
            ov = ovNew.get();
            AddChild(std::move(ovNew));
        } else {
            ov = dynamic_cast<DockOverlay*>(overlayEl);
        }
        if (ov) {
            UILayoutAccess::SetLastLayoutRect(*ov, GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight());
            DockDropTarget dt; dt.Kind = DockDropTarget::TargetKind::None;
            ov->SetTargetZones(dt, *m_Model, GetLayoutWidth(), GetLayoutHeight());
        }
    }

    if (m_OnPostRebuild)
        m_OnPostRebuild();
}

void DockspaceElement::BuildFromNode(const DockNode* n, UIElement* parent, const std::string& path) {
    if (!n || !parent) return;
    if (n->IsLeaf()) {
        // Leaf container: vertical stack -> [tabbar][content]
        auto leaf = std::make_unique<DockLeaf>();
        leaf->AddClass("dock-leaf");
        leaf->SetId(std::string("leaf:") + path);
        const auto& tabs = n->GetTabs();
        const size_t activeIdx = n->GetActiveIndex();
        const bool hideSingleTabBar = IsFloatingDockspace(this) && tabs.size() <= 1;

        // Tab bar row
        if (!hideSingleTabBar)
        {
            auto bar = std::make_unique<DockTabBar>();
            bar->AddClass("tabbar");
            bar->SetId(std::string("tabbar:") + path);
            bar->SetAccentColor(m_AccentColor);
            for (size_t i = 0; i < tabs.size(); ++i) {
                auto tab = std::make_unique<DockTab>();
                tab->AddClass("tab");
                // Encode the panel id into the element id for click handling
                tab->SetId(std::string("tab:") + tabs[i]);
                if (i == activeIdx) tab->AddClass("active");
                // Use the registered panel's title when available (DockPanel), otherwise fallback to id.
                std::string label = tabs[i];
                if (m_Model)
                {
                    if (UIElement* panel = m_Model->GetPanel(tabs[i]))
                    {
                        if (auto* dp = dynamic_cast<DockPanel*>(panel))
                        {
                            if (!dp->GetTitle().empty())
                                label = dp->GetTitle();
                        }
                    }
                }
                tab->SetText(label);
                if (m_Model)
                {
                    if (UIElement* panel = m_Model->GetPanel(tabs[i]))
                    {
                        if (auto* dp = dynamic_cast<DockPanel*>(panel))
                            tab->SetIconClass(dp->GetTabIcon());
                    }
                }
                bar->AddTabChild(std::move(tab));
            }
            leaf->AddChild(std::move(bar));
        }
        else
        {
            leaf->AddClass("single-tab-floating");
        }

        // Content placeholder (fills remaining space)
        auto content = std::make_unique<UIElement>();
        content->AddClass("dock-content");
        content->SetId(std::string("content:") + path);
        // Stable per-panel hook for the theme. The element id encodes the dock
        // path, which changes when panels are dragged, so it cannot serve.
        if (!tabs.empty())
        {
            const size_t activeForClass = n->GetActiveIndex();
            if (activeForClass < tabs.size())
                content->AddClass("dock-content-" + tabs[activeForClass]);
        }
        // Allow overflow when the mounted panel needs it (e.g. Scene View camera bookmark preview)
        if (!tabs.empty() && m_Model) {
            size_t idx = n->GetActiveIndex();
            if (idx < tabs.size()) {
                if (UIElement* panel = m_Model->GetPanel(tabs[idx])) {
                    if (auto* dp = dynamic_cast<DockPanel*>(panel)) {
                        if (dp->WantsDockContentOverflowVisible()) {
                            content->AddClass("overflow-visible");
                            leaf->AddClass("overflow-visible");
                        }
                    }
                }
            }
        }
        // Mount the active panel into the content area without transferring ownership.
        // Ensure the mounted panel's subtree is associated with this UIManager so that
        // pointer routing, font resolution, and focus logic work correctly.
        if (!tabs.empty() && m_Model) {
            size_t idx = n->GetActiveIndex();
            if (idx < tabs.size()) {
                if (UIElement* panel = m_Model->GetPanel(tabs[idx])) {
                    if (UIManager* owner = GetOwnerManager()) {
                        panel->SetOwnerManager(owner);
                    }
                    auto mount = std::make_unique<Mount>();
                    mount->SetId(std::string("mount:") + tabs[idx]);
                    mount->AddClass("mount");
                    mount->SetTarget(panel);
                    content->AddChild(std::move(mount));
                }
            }
        }
        leaf->AddChild(std::move(content));

        parent->AddChild(std::move(leaf));
    } else if (n->IsSplit()) {
        // Split container with two weighted panes
        auto split = std::make_unique<SplitView>();
        split->AddClass("dock-split");
        const auto dir = n->GetSplitDirection();
        if (dir == DockPosition::Left || dir == DockPosition::Right) {
            split->AddClass("row");
        } else {
            // Add both 'column' and 'col' to match CSS rules
            split->AddClass("column");
            split->AddClass("col");
        }

        float ratio = std::max(0.0f, std::min(1.0f, n->GetSplitRatio()));
        auto first = std::make_unique<WeightedPane>(ratio);
        auto* firstPtr = first.get();
        auto second = std::make_unique<WeightedPane>(std::max(0.0f, 1.0f - ratio));
        auto* secondPtr = second.get();
        // Mark weighted panes so CSS can target them (no combinators supported)
        firstPtr->AddClass("pane");
        secondPtr->AddClass("pane");

        split->AddChild(std::move(first));
        // Splitter handle sits between the panes; id encodes path to this split
        auto handle = std::make_unique<Splitter>();
        handle->AddClass("splitter");
        if (dir == DockPosition::Left || dir == DockPosition::Right) {
            handle->AddClass("row");
        } else {
            // Add both 'column' and 'col' to match CSS rules
            handle->AddClass("column");
            handle->AddClass("col");
        }
        handle->SetId(std::string("split:") + path);
        split->AddChild(std::move(handle));
        split->AddChild(std::move(second));

        UIElement* splitPtr = split.get();
        parent->AddChild(std::move(split));

        // Recurse into panes (append '0' for first, '1' for second)
        BuildFromNode(n->First(), firstPtr, path + "0");
        BuildFromNode(n->Second(), secondPtr, path + "1");

        // Ensure split container exists in the hierarchy before children are processed further
        (void)splitPtr;
    }
}

void DockspaceElement::ResetLayoutDefault() {
    if (!m_Model) return;
    // Default layout:
    // Left = Top(Hierarchy | SceneView), Bottom(Assets); Right = Inspector
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.8f);
    // Right: Inspector
    root->Second()->AddTab("Inspector");
    // Left: split Top/Bottom (Top 75%, Bottom 25%)
    root->First()->SetSplit(DockPosition::Top, 0.75f, DockNode::MakeLeaf(), DockNode::MakeLeaf());
    // Top: split Left/Right (Hierarchy | SceneView)
    root->First()->First()->SetSplit(DockPosition::Left, 0.25f, DockNode::MakeLeaf(), DockNode::MakeLeaf());
    // Assign panels
    root->First()->First()->First()->AddTab("Hierarchy");
    root->First()->First()->Second()->AddTab("SceneView");
    root->First()->Second()->AddTab("Assets");
    m_Model->SetRoot(std::move(root));
    RequestRebuildFromModel();
}

void DockspaceElement::SyncSplitRatiosFromUI()
{
    if (!m_Model)
        return;
    
    SyncSplitRatiosRecursive(this, "");
}

void DockspaceElement::SyncSplitRatiosRecursive(UIElement* el, const std::string& path)
{
    if (!el)
        return;
    
    // Check if this is a SplitView (contains WeightedPane children and a Splitter)
    // Structure: [WeightedPane (first)][Splitter][WeightedPane (second)]
    const auto& children = el->GetChildren();
    if (children.size() >= 3)
    {
        WeightedPane* paneA = dynamic_cast<WeightedPane*>(children[0].get());
        Splitter* splitter = dynamic_cast<Splitter*>(children[1].get());
        WeightedPane* paneB = dynamic_cast<WeightedPane*>(children[2].get());
        
        if (paneA && splitter && paneB)
        {
            // Extract split path from splitter ID (format: "split:<path>")
            const std::string& splitterId = splitter->GetId();
            const std::string prefix = "split:";
            if (splitterId.rfind(prefix, 0) == 0)
            {
                std::string splitPath = splitterId.substr(prefix.size());
                
                // Get the ratio from the first pane's flex weight
                float weightA = paneA->GetFlexWeight();
                float weightB = paneB->GetFlexWeight();
                float totalWeight = weightA + weightB;
                if (totalWeight > 1e-3f)
                {
                    float ratio = weightA / totalWeight;
                    ratio = std::max(0.0f, std::min(1.0f, ratio));
                    
                    // Update the model
                    m_Model->SetSplitRatioByPath(splitPath, ratio);
                }
            }
        }
    }
    
    // Recurse into children
    for (auto& child : children)
    {
        SyncSplitRatiosRecursive(child.get(), path);
    }
}

bool DockspaceElement::ToggleMaximizeLeaf(const std::string& panelId)
{
    if (!m_Model)
        return false;

    if (m_SavedRoot)
    {
        m_Model->SetRoot(std::move(m_SavedRoot));
        m_Model->ActivateTab(panelId);
        RequestRebuildFromModel();
        return true;
    }

    std::string leafPath;
    const DockNode* leaf = m_Model->FindLeafContaining(panelId, leafPath);
    if (!leaf || !leaf->IsLeaf())
        return false;

    SyncSplitRatiosFromUI();
    m_SavedRoot = m_Model->TakeRoot();

    auto soloLeaf = DockNode::MakeLeaf();
    for (const auto& tab : leaf->GetTabs())
        soloLeaf->AddTab(tab);
    soloLeaf->ActivateTab(panelId);

    m_Model->SetRoot(std::move(soloLeaf));
    RequestRebuildFromModel();
    return true;
}

void DockspaceElement::SetDropPreview(const DockDropTarget& target, float containerW, float containerH) {
    if (!m_Model) return;
    // Ensure overlay exists as a child
    UIElement* overlayEl = FindById("dock-overlay");
    DockOverlay* overlay = nullptr;
    if (!overlayEl) {
        auto ov = std::make_unique<DockOverlay>();
        ov->SetId("dock-overlay");
        ov->AddClass("overlay");
        overlay = ov.get();
        AddChild(std::move(ov));
    } else {
        overlay = dynamic_cast<DockOverlay*>(overlayEl);
    }
    if (!overlay) return;

    // The overlay may be added after the Yoga layout pass has already run for
    // this frame. Force its layout rect to cover the dockspace so primitive
    // generation doesn't skip it due to zero dimensions.
    UILayoutAccess::SetLastLayoutRect(*overlay, GetLayoutX(), GetLayoutY(), containerW, containerH);
    overlay->SetAccentColor(m_AccentColor);

    overlay->SetTargetZones(target, *m_Model, containerW, containerH);
}

void DockspaceElement::ClearDropPreview() {
    if (DockOverlay::IsDebugZonesEnabled() && m_Model) {
        // Ensure overlay exists and show debug-only zones
        UIElement* overlayEl = FindById("dock-overlay");
        DockOverlay* ov = nullptr;
        if (!overlayEl) {
            auto ovNew = std::make_unique<DockOverlay>();
            ovNew->SetId("dock-overlay");
            ovNew->AddClass("overlay");
            ov = ovNew.get();
            AddChild(std::move(ovNew));
        } else {
            ov = dynamic_cast<DockOverlay*>(overlayEl);
        }
        if (ov) {
            UILayoutAccess::SetLastLayoutRect(*ov, GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight());
            DockDropTarget dt; dt.Kind = DockDropTarget::TargetKind::None; // debug bands only
            ov->SetTargetZones(dt, *m_Model, GetLayoutWidth(), GetLayoutHeight());
        }
    } else {
        if (UIElement* overlayEl = FindById("dock-overlay")) {
            if (auto* ov = dynamic_cast<DockOverlay*>(overlayEl)) {
                ov->Clear();
            }
        }
    }
}

void DockspaceElement::ShowDockDragGhost(const std::string& title, float mouseX, float mouseY, float tabW, float tabH)
{
    if (!m_DragGhost)
    {
        constexpr float kGhostW = 360.0f;
        constexpr float kGhostH = 200.0f;
        constexpr float kBorder = 2.0f;

        // Use actual source tab dimensions if provided, otherwise sensible defaults
        const float actualTabW = (tabW > 1.0f) ? tabW : 120.0f;
        const float actualTabH = (tabH > 1.0f) ? tabH : 30.0f;

        // Outer container — transparent, no background, no border.
        // Only the tab and body have visible surfaces so the area beside the tab is see-through.
        auto ghost = std::make_unique<UIElement>();
        ghost->SetId("dock-drag-ghost");
        ghost->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9400)
            .Set(Style::Width, StyleLength::Px(kGhostW))
            .Set(Style::Height, StyleLength::Px(actualTabH + kGhostH))
            .Set(Style::Opacity, 0.8f);

        constexpr uint32_t kGrayBorder = 0xFF555555u;
        constexpr uint32_t kTransparent = 0x00000000u;

        // Tab element — flush with the left edge of the ghost, matching source tab size
        auto tab = std::make_unique<UIElement>();
        tab->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(0.f))
            .Set(Style::PositionTop, StyleLength::Px(0.f))
            .Set(Style::BackgroundColor, 0xFF272727u)
            .Set(Style::Width, StyleLength::Px(actualTabW))
            .Set(Style::Height, StyleLength::Px(actualTabH))
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.f, 4.f, 0.f, 0.f})
            .Set(Style::BorderWidth, Box4{kBorder, kBorder, 0.f, kBorder}) // top, right, bottom=0, left
            .Set(Style::BorderColor, BorderColorsTRBL{kGrayBorder, kGrayBorder, kTransparent, kGrayBorder});
        UILayoutAccess::SetLastLayoutRect(*tab, 0, 0, actualTabW, actualTabH);
        m_DragGhostTab = tab.get();

        auto lbl = std::make_unique<Label>();
        lbl->SetText(title);
        lbl->Overrides()
            .Set(Style::FontSize, StyleLength::Px(11.f))
            .Set(Style::Color, 0xFFFFFFFFu);
        UILayoutAccess::SetLastLayoutRect(*lbl, 0, 0, actualTabW, actualTabH);
        m_DragGhostLabel = lbl.get();
        tab->AddChild(std::move(lbl));
        ghost->AddChild(std::move(tab));

        // Body area — below the tab, with border on all sides including top
        auto body = std::make_unique<UIElement>();
        body->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(0.f))
            .Set(Style::PositionTop, StyleLength::Px(actualTabH))
            .Set(Style::Width, StyleLength::Px(kGhostW))
            .Set(Style::Height, StyleLength::Px(kGhostH))
            .Set(Style::BackgroundColor, 0xFF222222u)
            .Set(Style::BorderWidth, Box4{kBorder, kBorder, kBorder, kBorder})
            .Set(Style::BorderColor, BorderColorsTRBL{kGrayBorder, kGrayBorder, kGrayBorder, kGrayBorder})
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.f, 4.f, 4.f, 4.f})
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center);
        UILayoutAccess::SetLastLayoutRect(*body, 0, actualTabH, kGhostW, kGhostH);
        m_DragGhostBody = body.get();

        // "Undock" label centered in the body, hidden by default
        auto undockLbl = std::make_unique<Label>();
        undockLbl->SetText("Undock");
        undockLbl->Overrides()
            .Set(Style::FontSize, StyleLength::Px(16.f))
            .Set(Style::Color, 0xFFFFFFFFu)
            .Set(Style::Display, DisplayMode::None);
        UILayoutAccess::SetLastLayoutRect(*undockLbl, 0, 0, kGhostW, kGhostH);
        m_DragGhostUndockLabel = undockLbl.get();
        body->AddChild(std::move(undockLbl));

        ghost->AddChild(std::move(body));

        m_DragGhost = ghost.get();

        // Add to root element so the ghost isn't clipped by the dockspace
        UIElement* root = this;
        while (root->GetParent())
            root = root->GetParent();
        root->AddChild(std::move(ghost));
    }

    UpdateDockDragGhostPosition(mouseX, mouseY);
}

static void ShiftGhostSubtree(UIElement* el, float dx, float dy)
{
    if (!el)
        return;
    UILayoutAccess::SetLastLayoutRect(*el, el->GetLayoutX() + dx, el->GetLayoutY() + dy,
                          el->GetLayoutWidth(), el->GetLayoutHeight());
    for (const auto& child : el->GetChildren())
        ShiftGhostSubtree(child.get(), dx, dy);
}

void DockspaceElement::UpdateDockDragGhostPosition(float mouseX, float mouseY)
{
    if (!m_DragGhost)
        return;

    // Ghost is a child of root, so use absolute coordinates directly
    float x = mouseX + 12.0f;
    float y = mouseY + 8.0f;

    m_DragGhost->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(x))
        .Set(Style::PositionTop, StyleLength::Px(y));

    // Shift the layout subtree from current position to desired position so the
    // immediate-mode render uses the correct coordinates this frame.  Do NOT use
    // a raw SetLastLayoutRect on the ghost alone — that desynchronises children
    // and fights with the Yoga solve on the next frame, causing flickering.
    const float dx = x - m_DragGhost->GetLayoutX();
    const float dy = y - m_DragGhost->GetLayoutY();
    ShiftGhostSubtree(m_DragGhost, dx, dy);
}

void DockspaceElement::SetDockDragGhostUndockMode(bool undocking)
{
    if (!m_DragGhost)
        return;

    constexpr uint32_t kGrayBorder = 0xFF555555u;
    const uint32_t accentBorder = m_AccentColor | 0xFF000000u; // ensure full alpha
    const uint32_t borderColor = undocking ? accentBorder : kGrayBorder;

    constexpr uint32_t kTransparent = 0x00000000u;

    m_DragGhost->Overrides()
        .Set(Style::Opacity, undocking ? 1.0f : 0.8f);
    if (m_DragGhostTab)
        m_DragGhostTab->Overrides()
            .Set(Style::BorderColor, BorderColorsTRBL{borderColor, borderColor, kTransparent, borderColor});
    if (m_DragGhostBody)
        m_DragGhostBody->Overrides()
            .Set(Style::BorderColor, BorderColorsTRBL{borderColor, borderColor, borderColor, borderColor});
    if (m_DragGhostUndockLabel)
        m_DragGhostUndockLabel->Overrides()
            .Set(Style::Display, undocking ? DisplayMode::Flex : DisplayMode::None);
}

void DockspaceElement::HideDockDragGhost()
{
    if (m_DragGhost)
    {
        if (UIElement* parent = m_DragGhost->GetParent())
            parent->RemoveChild(m_DragGhost);
        m_DragGhost = nullptr;
        m_DragGhostLabel = nullptr;
        m_DragGhostUndockLabel = nullptr;
        m_DragGhostTab = nullptr;
        m_DragGhostBody = nullptr;
    }
}
