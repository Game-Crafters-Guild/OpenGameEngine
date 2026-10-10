#include "Panels/ColorPickerPopup.h"
#include "Panels/ColorPicker.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <memory>

namespace GameEngine
{
namespace
{
// Cursor offset and edge padding are ColorPickerWindow's, so the modal opens
// where the native tool window did for the same click.
constexpr float kCursorOffsetPx = 96.0f;
constexpr float kEdgePaddingPx = 8.0f;
// Stands in for the panel's extent before its first layout pass: the native
// picker window's own size, which is what this modal replaces.
constexpr float kNativePickerSizePx = 512.0f;
// How much of the panel must stay on screen while dragging. A title bar you
// cannot reach is a panel you cannot move back.
constexpr float kMinGrabbableTitlePx = 64.0f;
} // namespace

ColorPickerPopup::ColorPickerPopup()
    : DismissablePopup(this)
{
    Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f)).Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f)).Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::ZIndex, 10000)
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center);

    // Positions the panel — centered by default, pinned to an anchor by
    // ShowAt. Undimmed and non-interactive: dismissal is UIManager's job, so
    // this neither paints nor catches anything.
    auto panelHost = std::make_unique<UIElement>();
    panelHost->SetId("colorpicker-popup-panel-host");
    panelHost->AddClass("colorpicker-popup-panel-host");
    panelHost->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f)).Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f)).Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, false);
    m_PanelHost = panelHost.get();

    // Floating panel: contains the picker element + buttons.
    auto panel = std::make_unique<UIElement>();
    panel->SetId("colorpicker-popup-panel");
    panel->AddClass("colorpicker-popup-panel");
    panel->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(12.0f))
        .Set(Style::BackgroundColor, (uint32_t)0xFF252526)
        .Set(Style::BorderWidth, Box4{1, 1, 1, 1}).Set(Style::BorderColor, BorderColorsTRBL{0xFF3C3C3C, 0xFF3C3C3C, 0xFF3C3C3C, 0xFF3C3C3C})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{8, 8, 8, 8})
        .Set(Style::PaddingTop, StyleLength::Px(12.0f)).Set(Style::PaddingRight, StyleLength::Px(12.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(12.0f)).Set(Style::PaddingLeft, StyleLength::Px(12.0f))
        .Set(Style::PointerEvents, true);
    m_Panel = panel.get();

    // Title bar. Off desktop this popup stands in for a native tool window, so
    // it carries the one affordance that window had and a panel does not: a
    // grab handle. The handle is the title bar rather than the panel so that
    // the picker's own drags (wheel, sliders) are never mistaken for it.
    auto titleBar = std::make_unique<UIElement>();
    titleBar->SetId("colorpicker-popup-titlebar");
    titleBar->AddClass("colorpicker-popup-titlebar");
    titleBar->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::PaddingBottom, StyleLength::Px(4.0f))
        .Set(Style::Cursor, CursorStyle::Move)
        .Set(Style::PointerEvents, true);
    BindDragHandle(titleBar.get());
    auto titleLabel = std::make_unique<Label>();
    titleLabel->SetText("Color Picker");
    titleLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFCCCCCC)
        .Set(Style::FontSize, StyleLength::Px(13.0f))
        // The label is inert so a grab that lands on the text still drags.
        .Set(Style::PointerEvents, false);
    titleBar->AddChild(std::move(titleLabel));
    panel->AddChild(std::move(titleBar));

    // The color picker is a pure UI element; we just add it as a child.
    auto picker = std::make_unique<ColorPicker>();
    picker->SetId("colorpicker-popup-picker");
    m_Picker = picker.get();
    panel->AddChild(std::move(picker));

    // Button row: Apply, Cancel.
    auto buttonRow = std::make_unique<UIElement>();
    buttonRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::JustifyContent, JustifyContent::FlexEnd)
        .Set(Style::Gap, StyleLength::Px(8.0f));
    auto applyBtn = std::make_unique<Button>();
    applyBtn->SetId("colorpicker-apply");
    applyBtn->SetText("Apply");
    applyBtn->AddClass("primary");
    applyBtn->Overrides()
        .Set(Style::PaddingTop, StyleLength::Px(6.0f)).Set(Style::PaddingRight, StyleLength::Px(16.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(6.0f)).Set(Style::PaddingLeft, StyleLength::Px(16.0f))
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{0, 0, 0, 0})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4, 4, 4, 4})
        .Set(Style::FontSize, StyleLength::Px(14.0f));
    applyBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnApplyClicked(); });
    auto cancelBtn = std::make_unique<Button>();
    cancelBtn->SetId("colorpicker-cancel");
    cancelBtn->SetText("Cancel");
    cancelBtn->Overrides()
        .Set(Style::PaddingTop, StyleLength::Px(6.0f)).Set(Style::PaddingRight, StyleLength::Px(16.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(6.0f)).Set(Style::PaddingLeft, StyleLength::Px(16.0f))
        .Set(Style::BackgroundColor, (uint32_t)0xFF555555)
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{0, 0, 0, 0})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4, 4, 4, 4})
        .Set(Style::FontSize, StyleLength::Px(14.0f));
    cancelBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    buttonRow->AddChild(std::move(cancelBtn));
    buttonRow->AddChild(std::move(applyBtn));
    panel->AddChild(std::move(buttonRow));

    panelHost->AddChild(std::move(panel));
    AddChild(std::move(panelHost));
}

void ColorPickerPopup::Show(const ColorPickerValue& initialValue)
{
    m_Visible = true;
    if (m_Picker)
    {
        m_Picker->SetValue(initialValue);
        m_Picker->SetOnChange([this](const ColorPickerValue& v)
                              {
                                  if (m_OnValueChanging)
                                      m_OnValueChanging(v);
                              });
    }
    Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::PointerEvents, true);
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

float ColorPickerPopup::GetPanelWidth() const
{
    const float measured = m_Panel ? m_Panel->GetLayoutWidth() : 0.0f;
    return measured > 0.0f ? measured : kNativePickerSizePx;
}

float ColorPickerPopup::GetPanelHeight() const
{
    const float measured = m_Panel ? m_Panel->GetLayoutHeight() : 0.0f;
    return measured > 0.0f ? measured : kNativePickerSizePx;
}

void ColorPickerPopup::GetWindowOrigin(float& x, float& y) const
{
    x = m_Panel ? m_Panel->GetLayoutX() : 0.0f;
    y = m_Panel ? m_Panel->GetLayoutY() : 0.0f;
}

void ColorPickerPopup::PlaceWindow(float x, float y)
{
    if (!m_Panel)
        return;

    // This element spans the host window, so its layout box is the area a
    // native tool window would have had the monitor work area for.
    const float hostW = GetLayoutWidth();
    const float hostH = GetLayoutHeight();
    if (hostW > 0.0f && hostH > 0.0f)
    {
        // A window manager keeps a title bar grabbable no matter where you drop
        // a window; the same floor applies here, or a panel dragged past an
        // edge could not be dragged back.
        x = std::clamp(x, kEdgePaddingPx - GetPanelWidth() + kMinGrabbableTitlePx,
                       std::max(kEdgePaddingPx, hostW - kMinGrabbableTitlePx));
        y = std::clamp(y, kEdgePaddingPx, std::max(kEdgePaddingPx, hostH - kMinGrabbableTitlePx));
    }

    m_Panel->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(x))
        .Set(Style::PositionTop, StyleLength::Px(y));
    // The host centres the panel by default; an explicit position needs it to
    // stop doing that or the two fight over the same axis. Hide() puts the
    // centring back.
    if (m_PanelHost)
    {
        m_PanelHost->Overrides()
            .Set(Style::AlignItems, AlignItems::FlexStart)
            .Set(Style::JustifyContent, JustifyContent::FlexStart);
    }
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ColorPickerPopup::ShowAt(const ColorPickerValue& initialValue, float cursorX, float cursorY)
{
    Show(initialValue);

    // Mirrors ColorPickerWindow's placement so the picker lands where the
    // native tool window used to: offset from the cursor, on the side the
    // cursor leaves most room on, so it never covers the swatch that opened it.
    const float hostW = GetLayoutWidth();
    const float hostH = GetLayoutHeight();
    const float panelW = GetPanelWidth();
    const float panelH = GetPanelHeight();

    float x = (cursorX >= hostW * 0.5f) ? cursorX - panelW - kCursorOffsetPx
                                        : cursorX + kCursorOffsetPx;
    float y = (cursorY >= hostH * 0.5f) ? cursorY - panelH - kCursorOffsetPx
                                        : cursorY + kCursorOffsetPx;

    // Opening is the one moment the whole panel should be on screen, so this
    // clamp is tighter than the drag clamp above, which only keeps the title
    // bar reachable.
    if (hostW > 0.0f && hostH > 0.0f)
    {
        x = std::clamp(x, kEdgePaddingPx, std::max(kEdgePaddingPx, hostW - panelW - kEdgePaddingPx));
        y = std::clamp(y, kEdgePaddingPx, std::max(kEdgePaddingPx, hostH - panelH - kEdgePaddingPx));
    }
    PlaceWindow(x, y);
}

void ColorPickerPopup::Hide()
{
    m_Visible = false;
    CancelWindowDrag();
    // Reset panel to centered position for future Show() calls.
    if (m_Panel)
    {
        m_Panel->Overrides()
            .Reset(Style::Position)
            .Reset(Style::PositionLeft)
            .Reset(Style::PositionTop);
    }
    if (m_PanelHost)
    {
        m_PanelHost->Overrides()
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center);
    }
    Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false);
}

void ColorPickerPopup::OnApplyClicked()
{
    if (m_Picker && m_OnApply)
        m_OnApply(m_Picker->GetValue());
    Hide();
}

void ColorPickerPopup::OnCancelClicked()
{
    if (m_OnCancel)
        m_OnCancel();
    Hide();
}

} // namespace GameEngine
