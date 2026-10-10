#pragma once

#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace GameEngine {

/*
 * DockTabBar: Tab bar with animated reordering and live insertion indicator.
 *
 * Insertion slot semantics:
 * - Inclusive [0..N], slot 0 = before first, slot N = after last
 * - Computed by counting child centers <= dragCenter
 * - Edge forcing: when the dragged tab is visually clamped to the bar edges, force slot to 0/N
 *
 * Magnetic snapping:
 * - When enabled, snaps dragged center to nearest neighbor within m_MagnetPx
 * - Optional hysteresis: when enabled, once snapped, stay snapped until leaving (m_MagnetPx * m_MagnetHysteresisRatio)
 *
 * Undock thresholds:
 * - Threshold and dominance are configured on the bar; DockTab reads them during interaction
 */

class DockTabBar : public UIElement {
public:
    DockTabBar();

    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void AddTabChild(std::unique_ptr<UIElement> tab);
    void EnsureTabVisible(const std::string& panelId);

    // Live insertion indicator API
    void SetInsertionIndicator(int index, float xLocalPx);
    void ClearInsertionIndicator();

    int  GetInsertionIndex() const { return m_InsertActive ? m_InsertIndex : -1; }
    bool IsInsertionActive() const { return m_InsertActive; }

    // Optional magnetic snapping toggle
    void SetMagneticSnappingEnabled(bool enabled) { m_MagneticSnapping = enabled; }
    bool IsMagneticSnappingEnabled() const { return m_MagneticSnapping; }
    void SetMagneticSnapDistance(float px) { m_MagnetPx = std::max(0.0f, px); }
    // Optional magnetic hysteresis (off by default)
    void SetMagneticHysteresisEnabled(bool enabled) { m_MagnetHysteresis = enabled; }
    bool IsMagneticHysteresisEnabled() const { return m_MagnetHysteresis; }
    void SetMagneticHysteresisRatio(float r) { m_MagnetHysteresisRatio = std::max(1.0f, r); }
    float GetMagneticHysteresisRatio() const { return m_MagnetHysteresisRatio; }

    float GetMagneticSnapDistance() const { return m_MagnetPx; }

    void SetUndockThresholdPx(float px) { m_UndockThresholdPx = std::max(0.0f, px); }
    float GetUndockThresholdPx() const { return m_UndockThresholdPx; }

    void SetUndockDominance(float r) { m_UndockDominance = std::max(0.0f, r); }
    float GetUndockDominance() const { return m_UndockDominance; }


    // Reorder animation state (called by DockTab during drag moves)
    void SetReorderVisual(int fromIdx, int toIdx, float draggedWidth, UIElement* draggedEl);
    void ClearReorderVisual();

    float GetAnimatedOffsetForChild(const UIElement* child) const;

    void SetAccentColor(uint32_t argb) { m_AccentColor = argb; }

    // Returns the base (non-animated) X position for a child, or its GetLayoutX() if not dragging.
    float GetBaseXForChild(const UIElement* child) const;

    // Live drag: the dragged tab follows the mouse, others animate into place.
    // Call BeginLiveDrag when drag starts, UpdateLiveDrag on every mouse move,
    // and EndLiveDrag on mouse up. grabOffsetX = mouseX - tab.layoutX at drag start.
    void BeginLiveDrag(UIElement* draggedTab, float grabOffsetX);
    void UpdateLiveDrag(float mouseX);
    void EndLiveDrag();
    bool IsLiveDragActive() const { return m_LiveDragTab != nullptr; }
    UIElement* GetLiveDragTab() const { return m_LiveDragTab; }

    // Called after children have been reordered to set up animation offsets
    // for non-dragged tabs (they animate from old position to new position).
    void NotifyReorderSwap();

    // Tick animations (call once per frame). Returns true if still animating.
    bool TickDragAnimations(float dt);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    float GetTabsWidth() const;
    float GetMaxScrollOffset() const;
    void SetScrollOffset(float offset);
    void ApplyScrollOffset();
    void ScrollByPage(int direction);
    bool IsOverflowing() const { return GetMaxScrollOffset() > 0.5f; }

    static constexpr float kTabGapPx = 4.0f;
    static constexpr float kScrollButtonWidthPx = 20.0f;
    static constexpr float kOverflowControlsWidthPx = kScrollButtonWidthPx * 2.0f;
    float m_ScrollOffset = 0.0f;
    bool m_MiddlePanning = false;
    float m_MiddlePanStartX = 0.0f;
    float m_MiddlePanStartOffset = 0.0f;
    UIElement* m_ScrollLeftButton = nullptr;
    UIElement* m_ScrollRightButton = nullptr;
    bool m_HadOverflow = false;

    bool m_InsertActive = false;
    int  m_InsertIndex = -1;
    float m_InsertX = 0.0f; // in local coordinates of this bar

    // Reorder animation state
    bool m_ReorderActive = false;
    int  m_FromIndex = -1;
    // Magnetic snapping config
    bool m_MagneticSnapping = true;
    float m_MagnetPx = 12.0f;

    // Magnetic hysteresis config
    bool m_MagnetHysteresis = false;
    float m_MagnetHysteresisRatio = 1.5f; // stay snapped until leaving magnetPx * ratio

    int  m_ToIndex = -1;
    float m_DraggedWidth = 0.0f;
    UIElement* m_Dragged = nullptr; // not owned


    // Undock behavior config (read by DockTab)
    float m_UndockThresholdPx = 60.0f;
    float m_UndockDominance = 1.25f;

    uint32_t m_AccentColor = 0xFF3A8FFF;

    // Live drag state
    UIElement* m_LiveDragTab = nullptr;
    float m_LiveGrabOffsetX = 0.0f; // mouseX - tab.layoutX at grab time

    // Per-child: base X (the Yoga/sequential position) and visual offset (animates toward 0)
    std::vector<float> m_BaseX;       // true layout X for each child
    std::vector<float> m_AnimOffsets;  // visual offset from base

    // Animated offsets for each child (current and target), index-based
    mutable std::vector<float> m_CurOffset;
    mutable std::vector<float> m_TargetOffset;
};

} // namespace GameEngine
