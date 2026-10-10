#include "UI/Controls/DockTab.h"
#include "UI/Controls/DockTabBar.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/DockingHitTest.h"
#include "Platform/SystemMetrics.h"
#include "Logger/Logger.h"

namespace GameEngine {

DockTab::DockTab()
{
    auto lbl = std::make_unique<Label>();
    m_Label = lbl.get();
    lbl->AddClass("dock-tab-label");
    AddChild(std::move(lbl));
}

void DockTab::SetText(const std::string& t)
{
    if (m_Label)
        m_Label->SetText(t);
}

void DockTab::SetIconClass(std::string_view iconClass)
{
    if (iconClass.empty())
        return;
    auto icon = std::make_unique<UIElement>();
    icon->AddClass("dock-tab-icon");
    icon->AddClass(std::string(iconClass));
    InsertChild(0, std::move(icon));
}

UIElement* DockTab::GetOwningLeaf() const {
    UIElement* p = GetParent(); // Start with parent (should be tabbar)
    while (p) {
        if (p->HasClass("dock-leaf")) return p;
        p = p->GetParent();
    }
    return nullptr;
}

bool DockTab::IsPointInLeaf(float x, float y) const {
    UIElement* leaf = GetOwningLeaf();
    if (!leaf) return false;
    float lx = leaf->GetLayoutX();
    float ly = leaf->GetLayoutY();
    float lw = leaf->GetLayoutWidth();
    float lh = leaf->GetLayoutHeight();
    return (x >= lx && y >= ly && x < lx + lw && y < ly + lh);
}

void DockTab::OnEvent(UIEvent& e) {
    // Middle-button dragging pans an overflowing tab strip; it must not arm
    // the normal tab reorder/undock gesture.
    if (e.Id == kEventMouseDown && e.Button == 2)
    {
        if (auto* bar = dynamic_cast<DockTabBar*>(GetParent()))
            bar->OnEvent(e);
        return;
    }

    // Handle right-click for tab actions.
    if (e.Id == kEventMouseDown && e.Button == 1) // RMB
    {
        if (IsPointInside(e.X, e.Y))
        {
            m_RmbCandidateMenu = true;
            m_RmbDownX = e.X;
            m_RmbDownY = e.Y;
            e.Capture(this);
            e.Stop();
            return;
        }
    }
    else if (e.Id == kEventMouseUp && e.Button == 1) // RMB release
    {
        if (m_RmbCandidateMenu)
        {
            m_RmbCandidateMenu = false;
            
            // Check if it was a click (not a drag)
            const float dx = e.X - m_RmbDownX;
            const float dy = e.Y - m_RmbDownY;
            const float dist2 = dx * dx + dy * dy;
            constexpr float kThresholdPx = 4.0f;
            
            if (dist2 <= (kThresholdPx * kThresholdPx) && IsPointInside(e.X, e.Y))
            {
                // Right-click on tab: let the host decide whether this opens a menu or closes immediately.
                std::string panelId = GetPanelIdFromSelf();
                bool foundDockspace = false;
                for (UIElement* p = this; p; p = p->GetParent())
                {
                    if (auto* ds = dynamic_cast<DockspaceElement*>(p))
                    {
                        foundDockspace = true;
                        if (!ds->FireTabContextMenu(panelId, e.X, e.Y))
                            ds->RequestTabRemoval(panelId);
                        break;
                    }
                }
                // A tab outside any dockspace neither opens a menu nor closes:
                // the click is swallowed with nothing to show for it.
                if (!foundDockspace)
                {
                    Logger::Log::Warning("[DockTab] tab '{}' has no DockspaceElement ancestor; its "
                                         "right-click does nothing.", panelId);
                }
                e.Stop();
                return;
            }
        }
    }
    
    constexpr float kDragThreshold = 8.0f; // Distance to start drag (ghost + dock mode)
    
    if (e.Id == kEventMouseDown && e.Button == 0) {
        // Clicks on interactive tab chrome (e.g. Inspector lock) must not capture: mouse-up is routed to
        // the capture element, so children would never receive MouseUp for toggling.
        if (e.Button == 0)
        {
            for (const auto& ch : GetChildren())
            {
                UIElement* c = ch.get();
                if (!c || c == m_Label)
                    continue;
                if (c->ContainsPoint(e.X, e.Y))
                    return;
            }
        }

        // Give an inactive render-backed panel the press->release frame to
        // prepare its on-demand view. Activation itself still happens on
        // mouse-up, preserving drag/reorder behavior.
        std::string panelId = GetPanelIdFromSelf();
        for (UIElement* p = this; p; p = p->GetParent())
        {
            auto* dockspace = dynamic_cast<DockspaceElement*>(p);
            if (!dockspace || !dockspace->GetModel())
                continue;
            auto* model = dockspace->GetModel();
            if (!model->IsPanelActiveTab(panelId))
            {
                if (auto* panel = dynamic_cast<DockPanel*>(model->GetPanel(panelId)))
                {
                    float contentWidth = 0.0f;
                    float contentHeight = 0.0f;
                    if (UIElement* leaf = GetOwningLeaf())
                    {
                        for (const auto& child : leaf->GetChildren())
                        {
                            if (child && child->HasClass("dock-content"))
                            {
                                contentWidth = child->GetLayoutWidth();
                                contentHeight = child->GetLayoutHeight();
                                break;
                            }
                        }
                    }
                    panel->OnDockTabActivationArmed(contentWidth, contentHeight);
                }
            }
            break;
        }
        m_Armed = true;
        m_DragStartX = e.X; m_DragStartY = e.Y;
        m_DragGrabOffsetX = e.X - GetLayoutX();
        m_DragCurrentX = e.X; m_DragCurrentY = e.Y;
        m_MagnetSnapped = false; m_LastSnapCenter = 0.0f;
        m_DraggingForDock = false;
        m_ShowingGhost = false;
        m_LastLiveReorderSlot = -1;
        e.Capture(this);
        e.Stop();
    } else if (e.Id == kEventMouseMove) {
        // Add visual feedback class when starting significant drag
        if (!m_Armed) return;

        m_DragCurrentX = e.X;
        m_DragCurrentY = e.Y;

        const float dx = std::abs(e.X - m_DragStartX);
        const float dy = std::abs(e.Y - m_DragStartY);
        const float totalDrag = std::sqrt(dx * dx + dy * dy);

        if (totalDrag < kDragThreshold) return;

        UIElement* barEl = GetParent();
        DockTabBar* bar = dynamic_cast<DockTabBar*>(barEl);

        // Find dockspace for preview updates
        DockspaceElement* dockspace = nullptr;
        for (UIElement* p = this; p; p = p->GetParent()) {
            if (auto* ds = dynamic_cast<DockspaceElement*>(p)) { dockspace = ds; break; }
        }
        if (!dockspace) return;

        if (!HasClass("dragging"))
            const_cast<DockTab*>(this)->AddClass("dragging");

        std::string ghostTitle = m_Label ? m_Label->GetText() : GetPanelIdFromSelf();

        // Show/update ghost
        if (!m_ShowingGhost) {
            m_ShowingGhost = true;
            dockspace->ShowDockDragGhost(ghostTitle, e.X, e.Y, GetLayoutWidth(), GetLayoutHeight());
            // Start live drag on the tab bar so the dragged tab follows the mouse
            if (bar)
                bar->BeginLiveDrag(this, m_DragGrabOffsetX);
        } else {
            dockspace->UpdateDockDragGhostPosition(e.X, e.Y);
        }

        m_DraggingForDock = true;
        m_DragTargetOffsetX = 0.0f;

        // Current leaf path for this tab
        std::string currentLeafPath;
        if (barEl) {
            const std::string& barId = barEl->GetId();
            const std::string prefix = "tabbar:";
            if (barId.rfind(prefix, 0) == 0)
                currentLeafPath = barId.substr(prefix.size());
        }

        if (auto* model = dockspace->GetModel()) {
            float dsX = dockspace->GetLayoutX();
            float dsY = dockspace->GetLayoutY();
            float dsW = dockspace->GetLayoutWidth();
            float dsH = dockspace->GetLayoutHeight();

            float localX = e.X - dsX;
            float localY = e.Y - dsY;

            if (localX >= 0 && localY >= 0 && localX < dsW && localY < dsH) {
                DockDropTarget target = DockingHitTest::Compute(*model, dsW, dsH, localX, localY);

                bool sameLeafTabBar = (target.Kind == DockDropTarget::TargetKind::TabBar && target.Path == currentLeafPath);

                if (sameLeafTabBar && bar) {
                    // Own tab bar — live reorder with dragged tab following mouse
                    dockspace->ClearDropPreview();
                    dockspace->SetDockDragGhostUndockMode(false);
                    bar->UpdateLiveDrag(e.X);
                    auto r = ComputeInsertionSlot(bar, e.X);
                    int toSlot = static_cast<int>(r.toIdx);
                    if (toSlot != m_LastLiveReorderSlot && r.fromIdx != r.toIdx) {
                        m_LastLiveReorderSlot = toSlot;
                        if (model->ReorderTabInLeafByPath(currentLeafPath, r.fromIdx, r.toIdx)) {
                            bar->NotifyReorderSwap();
                            if (!dockspace->RequestTabReorder(currentLeafPath))
                                dockspace->RequestRebuildFromModel();
                        }
                    }
                } else {
                    // Leaving own tab bar — end live drag
                    if (bar && bar->IsLiveDragActive())
                        bar->EndLiveDrag();
                    m_LastLiveReorderSlot = -1;
                    bool isUndock = (target.Kind == DockDropTarget::TargetKind::Tab);
                    if (isUndock) {
                        dockspace->ClearDropPreview();
                    } else {
                        dockspace->SetDropPreview(target, dsW, dsH);
                    }
                    dockspace->SetDockDragGhostUndockMode(isUndock);
                }
            } else {
                m_LastLiveReorderSlot = -1;
                dockspace->ClearDropPreview();
                dockspace->SetDockDragGhostUndockMode(true);
            }
        }
        e.Stop();
    } else if (e.Id == kEventMouseUp) {
        if (!m_Armed) return;
        m_Armed = false;
        m_MagnetSnapped = false;
        
        // Remove visual feedback class
        if (HasClass("dragging")) {
            const_cast<DockTab*>(this)->RemoveClass("dragging");
        }

        std::string panelId = GetPanelIdFromSelf();

        // Find owning tabbar and dockspace/model
        DockspaceElement* dockspace = nullptr;
        UIElement* barEl = GetParent();
        DockTabBar* bar = dynamic_cast<DockTabBar*>(barEl);
        for (UIElement* p = this; p; p = p->GetParent()) {
            if (auto* ds = dynamic_cast<DockspaceElement*>(p)) { dockspace = ds; break; }
        }
        
        // Clear any visuals
        if (bar) { bar->ClearInsertionIndicator(); bar->ClearReorderVisual(); bar->EndLiveDrag(); }
        if (dockspace) { dockspace->ClearDropPreview(); dockspace->HideDockDragGhost(); }
        m_ShowingGhost = false;

        if (!dockspace) { m_DragTargetOffsetX = 0.0f; m_DraggingForDock = false; e.Stop(); return; }
        auto* model = dockspace->GetModel();
        if (!model) { m_DragTargetOffsetX = 0.0f; m_DraggingForDock = false; e.Stop(); return; }

        // If we were dragging for dock (outside current leaf), perform the dock operation
        if (m_DraggingForDock) {
            float dsX = dockspace->GetLayoutX();
            float dsY = dockspace->GetLayoutY();
            float dsW = dockspace->GetLayoutWidth();
            float dsH = dockspace->GetLayoutHeight();

            float localX = e.X - dsX;
            float localY = e.Y - dsY;

            // Check if cursor is inside the main editor window (root element bounds)
            UIElement* root = dockspace;
            while (root->GetParent())
                root = root->GetParent();
            float rootW = root->GetLayoutWidth();
            float rootH = root->GetLayoutHeight();
            bool outsideWindow = (e.X < 0 || e.Y < 0 || e.X >= rootW || e.Y >= rootH);

            if (outsideWindow) {
                // Dropped outside the editor window — undock to floating window
                model->RequestUndock(panelId);
                dockspace->RequestRebuildFromModel();
            } else if (localX >= 0 && localY >= 0 && localX < dsW && localY < dsH) {
                DockDropTarget target = DockingHitTest::Compute(*model, dsW, dsH, localX, localY);

                // Get the current leaf path for this tab
                std::string currentLeafPath;
                if (barEl) {
                    const std::string& barId = barEl->GetId();
                    const std::string prefix = "tabbar:";
                    if (barId.rfind(prefix, 0) == 0) {
                        currentLeafPath = barId.substr(prefix.size());
                    }
                }

                bool sameLeaf = ((target.Kind == DockDropTarget::TargetKind::Tab || target.Kind == DockDropTarget::TargetKind::TabBar)
                                 && target.Path == currentLeafPath);

                if (target.Kind == DockDropTarget::TargetKind::None) {
                    // Invalid zone — cancel
                } else if (target.Kind == DockDropTarget::TargetKind::Tab) {
                    // Center of any panel (including own) — undock to floating window
                    model->RequestUndock(panelId);
                    dockspace->RequestRebuildFromModel();
                } else if (sameLeaf && target.Kind == DockDropTarget::TargetKind::TabBar) {
                    // Own tab bar — reorder already applied live during drag, nothing to do
                } else if (target.Kind == DockDropTarget::TargetKind::TabBar) {
                    // Tab bar of another panel — dock as tab
                    model->RemoveTabForDockMove(panelId);
                    model->DockAsTabInLeafByPath(target.Path, panelId);
                    model->ActivateTab(panelId);
                    dockspace->RequestRebuildFromModel();
                } else {
                    // Side zones — dock/split into target panel
                    model->RemoveTabForDockMove(panelId);

                    switch (target.Kind) {
                        case DockDropTarget::TargetKind::LeafSplit:
                        case DockDropTarget::TargetKind::RegionSplit:
                            model->DockSplitSubtreeByPath(target.Path, target.Edge, panelId);
                            break;
                        case DockDropTarget::TargetKind::RootSplit:
                            model->DockToRoot(target.Edge, panelId);
                            break;
                        default:
                            break;
                    }

                    model->ActivateTab(panelId);
                    dockspace->RequestRebuildFromModel();
                }
            }
            // If dropped on toolbar area (inside window but outside dockspace), just cancel
            
            m_DragTargetOffsetX = 0.0f;
            m_DraggingForDock = false;
            e.Stop();
            return;
        }

        // Otherwise treat as click: activate tab if pointer is still on the tab
        if (!IsPointInside(e.X, e.Y)) { m_DragTargetOffsetX = 0.0f; m_DraggingForDock = false; e.Stop(); return; }

        constexpr float kDoubleClickTolerancePx = 8.0f;
        auto now = std::chrono::steady_clock::now();
        bool isDoubleClick = ((now - m_LastClickTime) < Platform::GetDoubleClickInterval()) &&
                             (std::abs(e.X - m_LastClickX) <= kDoubleClickTolerancePx) &&
                             (std::abs(e.Y - m_LastClickY) <= kDoubleClickTolerancePx);
        m_LastClickTime = now;
        m_LastClickX = e.X;
        m_LastClickY = e.Y;

        if (isDoubleClick) {
            dockspace->ToggleMaximizeLeaf(panelId);
            m_LastClickTime = {};
            m_DragTargetOffsetX = 0.0f;
            m_DraggingForDock = false;
            e.Stop();
            return;
        }

        if (!dockspace->RequestActivationPatch(panelId)) {
            if (model->ActivateTab(panelId))
                dockspace->RequestRebuildFromModel();
        }
        m_DragTargetOffsetX = 0.0f;
        m_DraggingForDock = false;
        e.Stop();
    }
}

DockTab::SlotCalcResult DockTab::ComputeInsertionSlot(DockTabBar* bar, float pointerX) const {
    SlotCalcResult r{0,0,0,0,0,0,0};
    if (!bar) return r;

    const float barX = bar->GetLayoutX();
    const float barW = bar->GetLayoutWidth();
    const float usedX = std::max(barX, std::min(barX + barW, pointerX));

    const float wSelf = GetLayoutWidth();
    // Keep the original grab point stable. GetLayoutX() changes after a live
    // reorder, so deriving this offset again would move the computed center
    // under a stationary pointer and could immediately reorder the tab back.
    const float desiredDrawX = usedX - m_DragGrabOffsetX;
    const float minDrawX = barX;
    const float maxDrawX = barX + barW - wSelf;
    const float clampedDrawX = std::max(minDrawX, std::min(maxDrawX, desiredDrawX));
    float dragCenter = clampedDrawX + wSelf * 0.5f;

    const auto& kids = bar->GetChildren();
    size_t fromIdx = 0;
    size_t idx = 0;
    for (const auto& ch : kids) {
        if (!ch->HasClass("tab")) continue;
        if (ch.get() == this) { fromIdx = idx; }
        idx++;
    }

    // Nearest neighbor center (excluding self), using base positions during live drag
    float nearestCx = dragCenter;
    float nearestDist = std::numeric_limits<float>::infinity();
    for (const auto& ch : kids) {
        if (!ch->HasClass("tab") || ch.get() == this) continue;
        float cx = bar->GetBaseXForChild(ch.get()) + ch->GetLayoutWidth() * 0.5f;
        float d  = std::abs(dragCenter - cx);
        if (d < nearestDist) { nearestDist = d; nearestCx = cx; }
    }

    // Magnetic snapping with optional hysteresis
    if (bar->IsMagneticSnappingEnabled()) {
        const float magnetPx = bar->GetMagneticSnapDistance();
        if (bar->IsMagneticHysteresisEnabled()) {
            if (m_MagnetSnapped) {
                const float holdPx = magnetPx * bar->GetMagneticHysteresisRatio();
                if (std::abs(dragCenter - m_LastSnapCenter) <= holdPx) {
                    dragCenter = m_LastSnapCenter; // stay snapped
                } else {
                    m_MagnetSnapped = false; // release
                }
            }
            if (!m_MagnetSnapped) {
                if (nearestDist <= magnetPx) {
                    dragCenter = nearestCx;
                    m_MagnetSnapped = true;
                    m_LastSnapCenter = nearestCx;
                }
            }
        } else {
            if (nearestDist <= magnetPx) {
                dragCenter = nearestCx;
                m_MagnetSnapped = true;
                m_LastSnapCenter = nearestCx;
            } else {
                m_MagnetSnapped = false;
            }
        }
    } else {
        m_MagnetSnapped = false;
    }

    // Inclusive insertion slot [0..N], using base positions during live drag.
    // A small dead zone around each neighbor center prevents a relayout or tiny
    // pointer movement from repeatedly swapping the same two tabs. The dragged
    // center must clear the neighbor by this margin in the direction of travel.
    constexpr float kReorderHysteresisPx = 6.0f;
    size_t toIdx = 0; idx = 0;
    for (const auto& ch : kids) {
        if (!ch->HasClass("tab")) continue;
        if (ch.get() == this) {
            idx++;
            continue;
        }
        float cx = bar->GetBaseXForChild(ch.get()) + ch->GetLayoutWidth() * 0.5f;
        const float crossing = idx < fromIdx
            ? cx - kReorderHysteresisPx
            : cx + kReorderHysteresisPx;
        if (dragCenter >= crossing) toIdx = idx + 1;
        idx++;
    }

    // Force extremes when visually clamped
    constexpr float kEdgeEps = 0.5f;
    if (clampedDrawX <= minDrawX + kEdgeEps) toIdx = 0;
    else if (clampedDrawX >= maxDrawX - kEdgeEps) toIdx = idx;

    r.fromIdx = fromIdx;
    r.toIdx = toIdx;
    r.dragCenter = dragCenter;
    r.clampedDrawX = clampedDrawX;
    r.localX = dragCenter - barX;
    r.minDrawX = minDrawX;
    r.maxDrawX = maxDrawX;
    return r;
}

bool DockTab::IsPointInside(float x, float y) const {
    float lx = GetLayoutX();
    float ly = GetLayoutY();
    float w  = GetLayoutWidth();
    float h  = GetLayoutHeight();
    return (x >= lx && y >= ly && x < (lx + w) && y < ly + h);
}

bool DockTab::IsInCloseHitRect(float x, float y) const {
    // Treat rightmost 16px square as the close button hit area for now
    constexpr float kClosePx = 16.0f;
    float lx = GetLayoutX();
    float ly = GetLayoutY();
    float w  = GetLayoutWidth();
    float h  = GetLayoutHeight();
    return (x >= lx + std::max(0.0f, w - kClosePx) && x < lx + w && y >= ly && y < ly + h);
}

std::string DockTab::GetPanelIdFromSelf() const {
    const std::string& fullId = GetId();
    std::string panelId = fullId;
    const std::string prefix = "tab:";
    if (panelId.rfind(prefix, 0) == 0) panelId = panelId.substr(prefix.size());
    return panelId;
}

} // namespace GameEngine
