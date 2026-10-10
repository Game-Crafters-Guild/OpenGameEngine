#include "UI/Interaction/TooltipOverlay.h"

#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine::UI::Interaction
{

static float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Logical px.
constexpr float kGap = 1.0f;         // gap between source edge and arrow tip
constexpr float kEdgePad = 6.0f;     // the bubble's least distance from the window edge
constexpr float kArrowSize = 15.0f;  // arrow triangle height

// An explicit side the bubble does not fit on, between the source and the window
// edge, flips to the opposite side when that one fits. Auto picks its side itself.
static UIElement::TooltipPlacement FitPlacement(UIElement::TooltipPlacement placement, const UIElement& source,
                                                float tipW, float tipH, float vpW, float vpH)
{
    using Placement = UIElement::TooltipPlacement;
    const float reach = kGap + kArrowSize;
    const float left = source.GetLayoutX();
    const float top = source.GetLayoutY();
    const float right = left + source.GetLayoutWidth();
    const float bottom = top + source.GetLayoutHeight();
    const bool fitsRight = right + reach + tipW + kEdgePad <= vpW;
    const bool fitsLeft = left - reach - tipW >= kEdgePad;
    const bool fitsBelow = bottom + reach + tipH + kEdgePad <= vpH;
    const bool fitsAbove = top - reach - tipH >= kEdgePad;
    switch (placement)
    {
    case Placement::Right:
        return !fitsRight && fitsLeft ? Placement::Left : placement;
    case Placement::Left:
        return !fitsLeft && fitsRight ? Placement::Right : placement;
    case Placement::Below:
        return !fitsBelow && fitsAbove ? Placement::Above : placement;
    case Placement::Above:
        return !fitsAbove && fitsBelow ? Placement::Below : placement;
    default:
        return placement;
    }
}

static void ShiftLayoutSubtree(UIElement* el, float dx, float dy)
{
    if (!el)
        return;
    UILayoutAccess::SetLastLayoutRect(*el, el->GetLayoutX() + dx, el->GetLayoutY() + dy,
                          el->GetLayoutWidth(), el->GetLayoutHeight());
    for (const auto& child : el->GetChildren())
        ShiftLayoutSubtree(child.get(), dx, dy);
}

void TooltipOverlay::ResetIfDetached()
{
    if (m_Wrapper && m_Wrapper->GetParent() == nullptr)
    {
        m_Wrapper  = nullptr;
        m_Tooltip  = nullptr;
        m_Label    = nullptr;
        m_LastText.clear();
        m_Visible   = false;
        m_WarmingUp = false;
        m_HasShownTooltip = false;
        m_HasArrow  = false;
        m_Parked    = false;
        m_HasLastPlaced = false;
    }
}

void TooltipOverlay::EnsureElements(UIElement* root)
{
    if (!root)
        return;

    if (m_Wrapper)
        return;

    if (UIElement* existing = root->FindById("ui-tooltip-wrapper"))
    {
        m_Wrapper = existing;
        m_Tooltip = m_Wrapper->FindById("ui-tooltip");
        m_Label   = dynamic_cast<Label*>(m_Wrapper->FindById("ui-tooltip-text"));
    }
    else
    {
        auto wrapper = std::make_unique<UIElement>();
        wrapper->SetId("ui-tooltip-wrapper");
        wrapper->AddClass("tooltip-wrapper");

        auto tip = std::make_unique<UIElement>();
        tip->SetId("ui-tooltip");
        tip->AddClass("tooltip");

        auto lbl = std::make_unique<Label>();
        lbl->SetId("ui-tooltip-text");
        lbl->AddClass("tooltip-text");
        m_Label = lbl.get();
        tip->AddChild(std::move(lbl));

        m_Tooltip = tip.get();
        wrapper->AddChild(std::move(tip));

        m_Wrapper = wrapper.get();
        root->AddChild(std::move(wrapper));
    }

    // One-shot constant styles, applied on creation and adoption alike.
    // Re-applying these every frame would dirty the wrapper and force a
    // full primitive regen (the values never change during its lifetime).
    //
    // Overlay ordering, rather than local z-index, decides precedence once
    // a dropdown/modal escapes its panel stacking context, so hover help
    // lives on the final system layer.
    m_Wrapper->SetOverlayLayer(OverlayLayer::HoverTooltip);
    m_Wrapper->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PointerEvents, false)
        .Set(Style::ZIndex, 9600);

    // Cap the bubble width so long tooltip text wraps. Yoga clamps the
    // label's measure constraint against the cap, so the wrapped size lands
    // in a single layout pass. Engine-side (not editor CSS) so every
    // UIManager host gets sane wrapping. The label must NOT carry a percent
    // width: it would resolve against the pre-clamp available width and let
    // the text escape the bubble.
    if (m_Tooltip)
        m_Tooltip->Overrides().Set(Style::MaxWidth, StyleLength::Px(kMaxTooltipWidth));
}

void TooltipOverlay::ParkOffscreen()
{
    if (!m_Wrapper)
        return;
    m_HasArrow = false;
    if (m_Parked)
        return;
    m_Wrapper->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PositionLeft, StyleLength::Px(-9999.0f))
        .Set(Style::PositionTop,  StyleLength::Px(-9999.0f));
    m_Parked = true;
}

// Swap in new tooltip text. Returns true when the text changed, in which
// case the caller must park for a frame: the label re-measures against the
// max-width cap during the next layout pass, and showing before that would
// place the bubble with the previous string's dimensions.
bool TooltipOverlay::UpdateText(const std::string& text)
{
    if (!m_Label || m_LastText == text)
        return false;

    m_LastText = text;
    m_Label->SetText(m_LastText);
    m_WarmingUp = false;
    m_Visible   = false;
    m_HasArrow  = false;
    return true;
}

const std::string* TooltipOverlay::FindTooltipText(UIElement* element, UIElement** outSource)
{
    // An authored tooltip anywhere on the ancestor chain is the answer. Only
    // when the whole chain has none does a label truncated by
    // text-overflow: ellipsis stand in as its own tooltip, so hover reveals
    // the full text without per-call-site wiring.
    UIElement*         truncated     = nullptr;
    const std::string* truncatedText = nullptr;

    for (UIElement* el = element; el; el = el->GetParent())
    {
        const std::string& t = el->GetTooltip();
        if (!t.empty())
        {
            if (outSource)
                *outSource = el;
            return &t;
        }
        if (truncatedText)
            continue;
        if (const ITextMeasurable* tm = el->GetTextMeasurable();
            tm && tm->WasLastRunEllipsized())
        {
            // GetTextContent is the same accessor emission truncated, and it
            // returns stable storage, so the pointer outlives this walk.
            const std::string& content = el->GetTextContent();
            if (!content.empty())
            {
                truncated     = el;
                truncatedText = &content;
            }
        }
    }

    if (outSource)
        *outSource = truncated;
    return truncatedText;
}

void TooltipOverlay::PlaceTooltip(UIElement* root, UIElement* source, float mouseX, float mouseY)
{
    if (!m_Wrapper || !m_Tooltip || !root)
        return;

    const float vpW = root->GetLayoutWidth();
    const float vpH = root->GetLayoutHeight();
    const float tipW = m_Tooltip->GetLayoutWidth();
    const float tipH = m_Tooltip->GetLayoutHeight();

    constexpr float kGapAbove  = 10.0f;   // extra gap above cursor when no source element
    constexpr float kArrowHalf = 11.0f;   // half the arrow base width
    constexpr float kOverlap = 4.0f;      // arrow base extends 4px into tooltip

    m_ResolvedArrowColorARGB = m_ArrowColorARGB;

    auto placement = source ? FitPlacement(source->GetTooltipPlacement(), *source, tipW, tipH, vpW, vpH)
                            : UIElement::TooltipPlacement::Auto;

    float x, y;
    m_HasArrow = false;

    if (placement == UIElement::TooltipPlacement::Right && source)
    {
        const float srcRight = source->GetLayoutX() + source->GetLayoutWidth();
        const float srcMidY  = source->GetLayoutY() + source->GetLayoutHeight() * 0.5f;
        x = ClampF(srcRight + kGap + kArrowSize, kEdgePad, vpW - tipW - kEdgePad);
        y = srcMidY - tipH * 0.5f;
        y = ClampF(y, kEdgePad, vpH - tipH - kEdgePad);

        // Left-pointing arrow at the left edge of the tooltip (base overlaps tooltip).
        float arrowTipX = srcRight + kGap;
        float arrowBaseX = arrowTipX + kArrowSize + kOverlap;
        float arrowMidY = ClampF(srcMidY, y + kArrowHalf, y + tipH - kArrowHalf);
        m_HasArrow = true;
        m_ArrowVerts[0] = arrowTipX;           m_ArrowVerts[1] = arrowMidY;
        m_ArrowVerts[2] = arrowBaseX;          m_ArrowVerts[3] = arrowMidY - kArrowHalf;
        m_ArrowVerts[4] = arrowBaseX;          m_ArrowVerts[5] = arrowMidY + kArrowHalf;
    }
    else if (placement == UIElement::TooltipPlacement::Left && source)
    {
        const float srcLeft = source->GetLayoutX();
        const float srcMidY = source->GetLayoutY() + source->GetLayoutHeight() * 0.5f;
        x = ClampF(srcLeft - kGap - kArrowSize - tipW, kEdgePad, vpW - tipW - kEdgePad);
        y = srcMidY - tipH * 0.5f;
        y = ClampF(y, kEdgePad, vpH - tipH - kEdgePad);

        // Right-pointing arrow at the right edge of the tooltip (base overlaps tooltip).
        float arrowTipX = srcLeft - kGap;
        float arrowBaseX = arrowTipX - kArrowSize - kOverlap;
        float arrowMidY = ClampF(srcMidY, y + kArrowHalf, y + tipH - kArrowHalf);
        m_HasArrow = true;
        m_ArrowVerts[0] = arrowTipX;           m_ArrowVerts[1] = arrowMidY;
        m_ArrowVerts[2] = arrowBaseX;          m_ArrowVerts[3] = arrowMidY - kArrowHalf;
        m_ArrowVerts[4] = arrowBaseX;          m_ArrowVerts[5] = arrowMidY + kArrowHalf;
    }
    else if (placement == UIElement::TooltipPlacement::Below && source)
    {
        const float srcBot  = source->GetLayoutY() + source->GetLayoutHeight();
        const float srcMidX = source->GetLayoutX() + source->GetLayoutWidth() * 0.5f;
        x = srcMidX - tipW * 0.5f;
        x = ClampF(x, kEdgePad, vpW - tipW - kEdgePad);
        y = ClampF(srcBot + kGap + kArrowSize, kEdgePad, vpH - tipH - kEdgePad);

        // Up-pointing arrow above the tooltip (base overlaps tooltip).
        float arrowTipY = srcBot + kGap;
        float arrowBaseY = arrowTipY + kArrowSize + kOverlap;
        float arrowMidX = ClampF(srcMidX, x + kArrowHalf, x + tipW - kArrowHalf);
        m_HasArrow = true;
        m_ArrowVerts[0] = arrowMidX;           m_ArrowVerts[1] = arrowTipY;
        m_ArrowVerts[2] = arrowMidX - kArrowHalf; m_ArrowVerts[3] = arrowBaseY;
        m_ArrowVerts[4] = arrowMidX + kArrowHalf; m_ArrowVerts[5] = arrowBaseY;
    }
    else if (placement == UIElement::TooltipPlacement::Above && source)
    {
        const float srcTop  = source->GetLayoutY();
        const float srcMidX = source->GetLayoutX() + source->GetLayoutWidth() * 0.5f;
        x = srcMidX - tipW * 0.5f;
        x = ClampF(x, kEdgePad, vpW - tipW - kEdgePad);
        y = ClampF(srcTop - kGap - kArrowSize - tipH, kEdgePad, vpH - tipH - kEdgePad);

        // Down-pointing arrow below the tooltip (base overlaps tooltip).
        float arrowTipY = srcTop - kGap;
        float arrowBaseY = arrowTipY - kArrowSize - kOverlap;
        float arrowMidX = ClampF(srcMidX, x + kArrowHalf, x + tipW - kArrowHalf);
        m_HasArrow = true;
        m_ArrowVerts[0] = arrowMidX;           m_ArrowVerts[1] = arrowTipY;
        m_ArrowVerts[2] = arrowMidX - kArrowHalf; m_ArrowVerts[3] = arrowBaseY;
        m_ArrowVerts[4] = arrowMidX + kArrowHalf; m_ArrowVerts[5] = arrowBaseY;
    }
    else
    {
        // Auto: anchor to the source element if available, otherwise cursor.
        // Canvas-style controls draw many virtual items inside one large UIElement
        // (for example node graph nodes/wires), so anchoring to the element rect
        // puts the tooltip at the canvas edge instead of the hovered item.
        const bool sourceUsesCursorAnchor = source && source->HasClass("node-canvas");
        // Prefer below the element; fall back to above.
        const bool hasSource = source != nullptr && !sourceUsesCursorAnchor;
        const float anchorX = hasSource
            ? source->GetLayoutX() + source->GetLayoutWidth() * 0.5f
            : mouseX;
        const float anchorTop = hasSource
            ? source->GetLayoutY()
            : mouseY - kGapAbove;
        const float anchorBot = hasSource
            ? source->GetLayoutY() + source->GetLayoutHeight()
            : mouseY;

        const float yBelow = anchorBot + kGap + kArrowSize;
        const float yAbove = anchorTop - kGap - kArrowSize - tipH;
        bool below = false;

        if (yBelow + tipH + kEdgePad <= vpH)
        {
            y = yBelow;
            below = true;
        }
        else if (yAbove >= kEdgePad)
            y = yAbove;
        else
        {
            // Neither fits perfectly — pick the side with more room.
            const float roomBelow = vpH - anchorBot;
            const float roomAbove = anchorTop;
            if (roomBelow >= roomAbove)
            {
                y = std::min(vpH - tipH - kEdgePad, yBelow);
                below = true;
            }
            else
                y = std::max(kEdgePad, yAbove);
        }
        x = anchorX - tipW * 0.5f;
        x = ClampF(x, kEdgePad, vpW - tipW - kEdgePad);

        // Arrow pointing at the source element.
        float arrowMidX = ClampF(anchorX, x + kArrowHalf, x + tipW - kArrowHalf);
        m_HasArrow = true;
        if (below)
        {
            float arrowTipY = anchorBot + kGap;
            float arrowBaseY = arrowTipY + kArrowSize + kOverlap;
            m_ArrowVerts[0] = arrowMidX;               m_ArrowVerts[1] = arrowTipY;
            m_ArrowVerts[2] = arrowMidX - kArrowHalf;  m_ArrowVerts[3] = arrowBaseY;
            m_ArrowVerts[4] = arrowMidX + kArrowHalf;  m_ArrowVerts[5] = arrowBaseY;
        }
        else
        {
            float arrowTipY = anchorTop - kGap;
            float arrowBaseY = arrowTipY - kArrowSize - kOverlap;
            m_ArrowVerts[0] = arrowMidX;               m_ArrowVerts[1] = arrowTipY;
            m_ArrowVerts[2] = arrowMidX - kArrowHalf;  m_ArrowVerts[3] = arrowBaseY;
            m_ArrowVerts[4] = arrowMidX + kArrowHalf;  m_ArrowVerts[5] = arrowBaseY;
        }
    }

    const float finalX = std::round(x);
    const float finalY = std::round(y);

    // Skip the Set chain when nothing actually moved. Re-applying the same
    // Display/Position values would dirty the wrapper every frame.
    if (!m_Parked && m_HasLastPlaced && finalX == m_LastPlacedX && finalY == m_LastPlacedY)
        return;

    m_Wrapper->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PositionLeft, StyleLength::Px(finalX))
        .Set(Style::PositionTop,  StyleLength::Px(finalY));
    m_Parked = false;
    m_LastPlacedX = finalX;
    m_LastPlacedY = finalY;
    m_HasLastPlaced = true;

    const float curX = m_Wrapper->GetLayoutX();
    const float curY = m_Wrapper->GetLayoutY();
    ShiftLayoutSubtree(m_Wrapper, finalX - curX, finalY - curY);
}

void TooltipOverlay::EmitArrowPrimitive(std::vector<UI::UIPrimitive>& prims, float opacity, float contentScale) const
{
    if (!m_HasArrow || !m_Visible || !m_Tooltip)
        return;
    // Don't emit the arrow if the tooltip body hasn't been measured yet.
    if (m_Tooltip->GetLayoutWidth() <= 0.0f || m_Tooltip->GetLayoutHeight() <= 0.0f)
        return;

    constexpr float kRoundingLogical = 3.5f; // curve on the arrow edges (logical px)
    const float cs = (contentScale > 0.0f) ? contentScale : 1.0f;
    uint32_t color = UI::PackFromARGB(m_ResolvedArrowColorARGB);
    auto tri = UI::MakeTriangle(
        m_ArrowVerts[0] * cs, m_ArrowVerts[1] * cs,
        m_ArrowVerts[2] * cs, m_ArrowVerts[3] * cs,
        m_ArrowVerts[4] * cs, m_ArrowVerts[5] * cs,
        color, kRoundingLogical * cs);
    UI::SetOpacity(tri, opacity);
    prims.push_back(tri);
}

void TooltipOverlay::Update(UIElement* root, UIElement* hovered, float mouseX, float mouseY, float time)
{
    ResetIfDetached();

    if (!root)
        return;

    UIElement* tooltipSource = nullptr;
    const std::string* tooltipText = hovered ? FindTooltipText(hovered, &tooltipSource) : nullptr;

    if (!tooltipText || tooltipText->empty())
    {
        m_HoveredId      = 0;
        m_HoverStartTime = 0.0f;
        m_WarmingUp      = false;
        if (m_Visible)
        {
            m_LastVisibleTime = time;
            m_Visible = false;
            m_HasArrow = false;
            // Park offscreen (keep Display::Flex) so Yoga continues to measure
            // the wrapper. This ensures dimensions are valid when the next
            // tooltip needs to hop-show immediately.
            ParkOffscreen();
        }
        return;
    }

    // Use the tooltip source element's ID (not the hovered element) so that
    // moving the mouse between a button and its child text label doesn't
    // restart the hover delay.
    const uint64_t currentId = tooltipSource ? tooltipSource->GetInstanceId() : 0;

    if (currentId != m_HoveredId)
    {
        const bool wasVisible = m_Visible;
        if (wasVisible)
            m_LastVisibleTime = time;

        const bool hop = wasVisible ||
            (m_HasShownTooltip && (time - m_LastVisibleTime) < m_HoverResetDelaySeconds);

        m_HoveredId      = currentId;
        m_HoverStartTime = hop ? 0.0f : time;

        EnsureElements(root);
        if (UpdateText(*tooltipText))
        {
            ParkOffscreen();
            return;
        }

        if (hop)
        {
            // Show immediately during hop if the wrapper has valid dimensions.
            // If not (was Display::None), park for 1 frame so Yoga measures.
            const float cw = m_Tooltip ? m_Tooltip->GetLayoutWidth() : 0.0f;
            const float ch = m_Tooltip ? m_Tooltip->GetLayoutHeight() : 0.0f;
            if (cw > 0.0f && ch > 0.0f)
            {
                m_Visible = true;
                m_HasShownTooltip = true;
                m_WarmingUp = false;
                PlaceTooltip(root, tooltipSource, mouseX, mouseY);
                return;
            }
            // Dimensions stale — warm up for 1 frame.
            m_WarmingUp = true;
            ParkOffscreen();
            return;
        }

        m_WarmingUp      = false;
        if (m_Visible)
        {
            m_Visible = false;
            m_HasArrow = false;
        }
        ParkOffscreen();
        return;
    }

    if (!m_MiddleMousePending && (time - m_HoverStartTime) < m_HoverDelaySeconds)
    {
        EnsureElements(root);
        UpdateText(*tooltipText);
        ParkOffscreen();
        return;
    }

    m_MiddleMousePending = false;

    EnsureElements(root);
    if (UpdateText(*tooltipText))
    {
        ParkOffscreen();
        return;
    }

    const float curW = m_Wrapper ? m_Wrapper->GetLayoutWidth()  : 0.0f;
    const float curH = m_Wrapper ? m_Wrapper->GetLayoutHeight() : 0.0f;

    if (curW <= 0.0f || curH <= 0.0f || m_WarmingUp)
    {
        m_WarmingUp = false;
        ParkOffscreen();
        return;
    }

    m_WarmingUp = false;
    m_Visible   = true;
    m_HasShownTooltip = true;
    PlaceTooltip(root, tooltipSource, mouseX, mouseY);
}

void TooltipOverlay::OnMiddleMousePressed(UIElement* hovered)
{
    if (!hovered)
        return;
    const std::string* text = FindTooltipText(hovered);
    if (!text || text->empty())
        return;
    m_MiddleMousePending = true;
    m_HoveredId = hovered->GetInstanceId();
}

void TooltipOverlay::Hide()
{
    ResetIfDetached();
    m_HoveredId          = 0;
    m_HoverStartTime     = 0.0f;
    m_Visible            = false;
    m_WarmingUp          = false;
    m_MiddleMousePending = false;
    m_HasArrow           = false;
    ParkOffscreen();
}

} // namespace GameEngine::UI::Interaction
