#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class UIElement;
class Label;

namespace UI
{
struct UIPrimitive;
}

namespace UI::Interaction
{

// Hover tooltip overlay. Owned by UIManager and updated each frame.
// Shows a dark tooltip bubble with a dynamically-positioned arrow
// whenever the hovered element (or any ancestor) has tooltip text.
//
// The arrow is rendered as a GPU triangle primitive (no DOM element),
// so it works correctly for all placement directions.
class TooltipOverlay final
{
public:
    // Call once per frame after hover chain and events have been resolved.
    // root     - the UI root element (tooltip is appended here)
    // hovered  - the currently hovered element (may be nullptr)
    // mouseX/Y - current mouse position in root-space pixels
    // time     - current UIManager time in seconds (used for hover delay)
    void Update(UIElement* root, UIElement* hovered, float mouseX, float mouseY, float time);

    // Called when middle-mouse button is pressed — shows tooltip immediately if the
    // hovered element (or an ancestor) has tooltip text.
    void OnMiddleMousePressed(UIElement* hovered);

    // Force-hide the tooltip immediately (e.g. on mouse leave from window).
    void Hide();

    // Runtime configuration (called each frame by the editor from TooltipSettings).
    void SetHoverDelay(float seconds) { m_HoverDelaySeconds = seconds; }
    void SetHoverResetDelay(float seconds) { m_HoverResetDelaySeconds = seconds; }
    void SetArrowColor(uint32_t argb) { m_ArrowColorARGB = argb; }

    // Collect arrow triangle primitives for the current frame. Called by
    // UIManager during primitive generation, after the tooltip DOM has
    // been measured and placed.
    // contentScale converts the arrow's logical-px verts / rounding to physical
    // px for emission (arrow geometry is computed in logical space by PlaceTooltip).
    void EmitArrowPrimitive(std::vector<UI::UIPrimitive>& prims, float opacity, float contentScale) const;

private:
    void EnsureElements(UIElement* root);
    void ResetIfDetached();
    void ParkOffscreen();
    bool UpdateText(const std::string& text);

    // Walk up the element chain to find the nearest tooltip text.
    static const std::string* FindTooltipText(UIElement* element, UIElement** outSource = nullptr);

    void PlaceTooltip(UIElement* root, UIElement* source, float mouseX, float mouseY);

    UIElement* m_Wrapper  = nullptr; // outer container (position/z-index, no clipping)
    UIElement* m_Tooltip  = nullptr; // visible box (background, border-radius, shadow)
    Label*     m_Label    = nullptr;

    std::string m_LastText;
    bool        m_Visible   = false;
    bool        m_WarmingUp = false;
    bool        m_HasShownTooltip = false;
    // Track whether the wrapper is currently parked offscreen so subsequent
    // ParkOffscreen() calls during the hover delay become no-ops instead of
    // re-applying the same Display/Position values (which would dirty the
    // wrapper every frame and force a full primitive regen).
    bool        m_Parked    = false;
    // Last placed position so PlaceTooltip can short-circuit when the
    // tooltip stays put across frames (e.g. stationary cursor over a
    // tooltip-bearing button).
    float       m_LastPlacedX = 0.0f;
    float       m_LastPlacedY = 0.0f;
    bool        m_HasLastPlaced = false;

    // Arrow triangle: computed by PlaceTooltip, emitted by EmitArrowPrimitive.
    bool     m_HasArrow = false;
    float    m_ArrowVerts[6] = {}; // v0.xy, v1.xy, v2.xy
    uint32_t m_ResolvedArrowColorARGB = 0; // per-frame: read from tooltip bg

    // Hover delay tracking
    uint64_t    m_HoveredId                = 0;
    float       m_HoverStartTime           = 0.0f;
    float       m_LastVisibleTime          = 0.0f;
    float       m_HoverDelaySeconds        = 0.5f;
    float       m_HoverResetDelaySeconds   = 0.5f;
    // Max bubble width (logical px), applied as a Style::MaxWidth override on
    // the tooltip box so long text wraps; short text keeps its natural width.
    static constexpr float kMaxTooltipWidth = 512.0f;
    bool  m_MiddleMousePending = false;

    uint32_t m_ArrowColorARGB = 0xFF000000u; // black default
};

} // namespace UI::Interaction
} // namespace GameEngine
