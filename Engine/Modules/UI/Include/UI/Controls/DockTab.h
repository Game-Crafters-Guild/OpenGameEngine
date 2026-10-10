#pragma once

#include "UI/Controls/Label.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/DockTabBar.h"
#include "UI/UIEvents.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <limits>
#include <string_view>

namespace GameEngine {
/*
 * DockTab: Draggable tab supporting reorder (horizontal), undock (vertical),
 * and close/activate click behaviors.
 * - Reorder uses inclusive slot [0..N] based on dragged center with edge forcing.
 * - Magnetic snapping (if enabled on the bar) snaps within bar->GetMagneticSnapDistance().
 * - Undock requires dy > bar->GetUndockThresholdPx() and dy > dx * bar->GetUndockDominance().
 *
 * Hosts an inner Label for the title so optional controls (e.g. the Inspector lock icon)
 * can be inserted without using text measure on the tab root.
 */


class DockTab : public UIElement {
public:
    DockTab();
    void SetText(const std::string& t);
    // Prepends an icon element before the label. Pass an empty class to skip.
    void SetIconClass(std::string_view iconClass);

    void OnEvent(UIEvent& e) override;
    // Compute insertion slot and drag center from pointer X, with magnet and edge forcing
    struct SlotCalcResult { size_t fromIdx; size_t toIdx; float dragCenter; float clampedDrawX; float localX; float minDrawX; float maxDrawX; };
    SlotCalcResult ComputeInsertionSlot(class DockTabBar* bar, float pointerX) const;


private:
    bool IsPointInside(float x, float y) const;
    bool IsInCloseHitRect(float x, float y) const;
    UIElement* GetOwningLeaf() const;
    bool IsPointInLeaf(float x, float y) const;

    std::string GetPanelIdFromSelf() const;

    Label* m_Label = nullptr;

    bool m_Armed = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    float m_DragGrabOffsetX = 0.0f;         // fixed mouse-to-tab offset captured before live reordering
    mutable float m_DragCurOffsetX = 0.0f;  // eased value applied at draw time
    float m_DragTargetOffsetX = 0.0f;       // set by input; eased towards
    float m_DragCurrentX = 0.0f;
    float m_DragCurrentY = 0.0f;
    bool m_DraggingForDock = false;
    bool m_ShowingGhost = false;
    int m_LastLiveReorderSlot = -1; // tracks last committed reorder slot for live reorder

    // Magnet latch state for hysteresis
    mutable bool  m_MagnetSnapped = false;
    mutable float m_LastSnapCenter = 0.0f;
    
    // RMB tracking for context menu
    bool m_RmbCandidateMenu = false;
    float m_RmbDownX = 0.0f;
    float m_RmbDownY = 0.0f;

    // Double-click detection for maximize toggle
    std::chrono::steady_clock::time_point m_LastClickTime;
    float m_LastClickX = 0.0f;
    float m_LastClickY = 0.0f;
};


} // namespace GameEngine
