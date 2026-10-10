#include "UI/Controls/Dropdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"

namespace GameEngine {

namespace {

// Returned for an option that declares no icon class and for an out-of-range
// index, so callers can hold a reference either way.
const std::string kNoIconClass;

static std::string TrimCopy(const std::string& s)
{
    size_t start = 0;
    size_t end   = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

// Parse #rrggbb or #rgb to ARGB (0xAARRGGBB). Returns 0 and false on failure.
static bool TryParseHexColor(const char* s, uint32_t& outArgb)
{
    if (!s || s[0] != '#' || std::strlen(s) < 4)
        return false;
    auto hexNibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
        return 0xFFu;
    };
    const size_t len = std::strlen(s);
    uint8_t r, g, b;
    if (len >= 7)
    {
        r = (uint8_t)((hexNibble(s[1]) << 4) | hexNibble(s[2]));
        g = (uint8_t)((hexNibble(s[3]) << 4) | hexNibble(s[4]));
        b = (uint8_t)((hexNibble(s[5]) << 4) | hexNibble(s[6]));
    }
    else if (len >= 4)
    {
        r = hexNibble(s[1]) * 17u;
        g = hexNibble(s[2]) * 17u;
        b = hexNibble(s[3]) * 17u;
    }
    else
        return false;
    outArgb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
}

// A row with an icon keeps its text in a child Label, so the row itself stays a
// flex container instead of a text-measure leaf. Either way this is the Label
// the option's color applies to.
static Label* ItemTextLabel(Label* row)
{
    for (const auto& child : row->GetChildren())
    {
        if (auto* text = dynamic_cast<Label*>(child.get()))
            return text;
    }
    return row;
}

static void ApplyOptionColorToItemLabel(Label* raw, const Dropdown::Option& opt, bool isSelected)
{
    if (isSelected)
    {
        raw->Overrides().Reset(Style::Color);
        return;
    }
    if (!opt.color.empty())
    {
        uint32_t argb = 0;
        if (TryParseHexColor(opt.color.c_str(), argb))
            raw->Overrides().Set(Style::Color, argb);
        else
            raw->Overrides().Reset(Style::Color);
    }
    else
        raw->Overrides().Reset(Style::Color);
}

} // namespace

Dropdown::~Dropdown()
{
}

Dropdown::Dropdown()
    : DismissablePopup(this)
{
    AddClass("dropdown");
    // Control styling is an Editor-shipped UIStyle asset.
    RequestSubtreeStyleAssetPath("UI/controls/Dropdown.css", "editor");
    SetFocusable(true);
    EnsureHeader();
    EnsureItemsContainer();
}

void Dropdown::SetMode(Mode mode)
{
    if (m_Mode == mode)
        return;
    m_Mode = mode;
}

void Dropdown::SetOptions(const std::vector<Option>& options, int selectedIndex)
{
    m_Options = options;
    m_HasOptionIcons = std::any_of(m_Options.begin(), m_Options.end(),
                                   [](const Option& o) { return !o.iconClass.empty(); });
    EnsureHeader();
    if (m_HasOptionIcons)
        EnsureHeaderIcon();
    else
        RemoveHeaderIcon();
    EnsureItemsContainer();
    UpdatePopupWidthClass();
    RebuildUiItems();

    if (!m_Options.empty())
    {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_Options.size()))
            selectedIndex = 0;
        SetSelectedIndex(selectedIndex);
        // SetSelectedIndex may early-return (same index) without updating the
        // header — force a refresh so the label reflects the new option set.
        UpdateHeaderLabel();
    }
    else
    {
        m_SelectedIndex = -1;
        Field<std::string>::SetValue(std::string());
        NotifyValueChanging();
        NotifyValueChanged();
        UpdateHeaderLabel();
    }
}

void Dropdown::SetOptionsFromLabels(const std::vector<std::string>& labels, int selectedIndex)
{
    std::vector<Option> options;
    options.reserve(labels.size());
    for (const auto& label : labels)
    {
        Option opt;
        opt.value = label;
        opt.label = label;
        options.push_back(std::move(opt));
    }
    SetOptions(options, selectedIndex);
}

void Dropdown::SetOptionsFromString(const std::string& csv)
{
    std::vector<std::string> labels;
    std::stringstream        ss(csv);
    std::string              item;
    while (std::getline(ss, item, ','))
    {
        labels.push_back(TrimCopy(item));
    }
    SetOptionsFromLabels(labels);
}

const Dropdown::Option* Dropdown::SelectedOption() const
{
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Options.size()))
    {
        return nullptr;
    }
    return &m_Options[static_cast<size_t>(m_SelectedIndex)];
}

const std::string& Dropdown::GetSelectedValue() const
{
    static const std::string kEmpty;
    const Option* selected = SelectedOption();
    return selected ? selected->value : kEmpty;
}

const std::string& Dropdown::GetSelectedLabel() const
{
    static const std::string kEmpty;
    const Option* selected = SelectedOption();
    return selected ? selected->label : kEmpty;
}

float Dropdown::WidthForLabel(std::string_view label) const
{
    UIManager* ui = GetOwnerManager();
    if (!ui || !m_HeaderContainer || !m_HeaderLabel)
        return 0.0f;
    const float text = ui->MeasureTextWidth(*m_HeaderLabel, label);
    if (text <= 0.0f)
        return 0.0f;
    const auto& labelLayout = m_HeaderLabel->GetResolvedStyle().Layout;
    float width = text + UI::Layout::HorizontalInsetPx(*this) +
                  UI::Layout::HorizontalInsetPx(*m_HeaderContainer) + UI::Layout::HorizontalInsetPx(*m_HeaderLabel) +
                  labelLayout.Margin.Left + labelLayout.Margin.Right;
    const float gap = m_HeaderContainer->GetResolvedStyle().Layout.ColumnGap;
    for (const UIElement* part : {static_cast<const UIElement*>(m_HeaderIcon), static_cast<const UIElement*>(m_Chevron)})
    {
        if (!part || part->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            continue;
        const auto& layout = part->GetResolvedStyle().Layout;
        width += part->GetLayoutWidth() + layout.Margin.Left + layout.Margin.Right + gap;
    }
    return width;
}

void Dropdown::SetSelectedValue(const std::string& value)
{
    for (size_t i = 0; i < m_Options.size(); ++i)
    {
        if (m_Options[i].value == value)
        {
            SetSelectedIndex(static_cast<int>(i));
            return;
        }
    }
}

void Dropdown::SetSelectedIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Options.size()))
        index = -1;

    if (m_SelectedIndex == index)
        return;

    m_SelectedIndex = index;

    if (m_SelectedIndex >= 0)
    {
        const auto& opt = m_Options[static_cast<size_t>(m_SelectedIndex)];
        if (GetValue() != opt.value)
        {
            Field<std::string>::SetValue(opt.value);
            NotifyValueChanging();
            NotifyValueChanged();
        }
    }
    else
    {
        if (!GetValue().empty())
        {
            Field<std::string>::SetValue(std::string());
            NotifyValueChanging();
            NotifyValueChanged();
        }
    }

    UpdateHeaderLabel();
    SyncItemSelectionClasses();
}

void Dropdown::SetSelectedIndexWithoutNotify(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Options.size()))
        index = -1;

    if (m_SelectedIndex == index)
        return;

    m_SelectedIndex = index;

    // Mirror SetSelectedIndex's backing-value update, but via the field's
    // without-notify path so OnValueChanged (which commits an edit) does not
    // fire. SetValueWithoutNotify already short-circuits an unchanged value.
    if (m_SelectedIndex >= 0)
        Field<std::string>::SetValueWithoutNotify(m_Options[static_cast<size_t>(m_SelectedIndex)].value);
    else
        Field<std::string>::SetValueWithoutNotify(std::string());

    UpdateHeaderLabel();
    SyncItemSelectionClasses();
}

void Dropdown::SyncItemSelectionClasses()
{
    if (!m_ItemsContainer)
        return;
    const auto& children = m_ItemsContainer->GetChildren();
    for (size_t i = 0; i < children.size() && i < m_Options.size(); ++i)
    {
        auto* raw = dynamic_cast<Label*>(children[i].get());
        if (!raw)
            continue;
        const bool isSelected = (static_cast<int>(i) == m_SelectedIndex);
        if (isSelected)
            raw->AddClass("selected");
        else
            raw->RemoveClass("selected");
        ApplyOptionColorToItemLabel(ItemTextLabel(raw), m_Options[i], isSelected);
    }
}

void Dropdown::SetNativeMenuInvoker(NativeMenuInvoker invoker)
{
    m_NativeMenuInvoker = std::move(invoker);
}

void Dropdown::SetAutoWidthPopup(bool enabled)
{
    if (m_AutoWidthPopup == enabled)
        return;
    m_AutoWidthPopup = enabled;
    EnsureItemsContainer();
    UpdatePopupWidthClass();
}

void Dropdown::UpdatePopupWidthClass()
{
    // The header sizes the popup, except when the popup needs more: a caller
    // that asked for auto width, or the icon column the control adds, which
    // would otherwise be taken out of the labels and wrap them.
    if (m_AutoWidthPopup || m_HasOptionIcons)
        m_ItemsContainer->AddClass("fit-content");
    else
        m_ItemsContainer->RemoveClass("fit-content");
}

// An open popup re-places itself after every layout pass: its own width is not
// known until it has been measured, and both the flip and the clamp below need it.
void Dropdown::OnPostLayout()
{
    if (IsMenuOpen())
        UpdatePopupPlacement();
}

// Where the popup sits relative to its header. Vertically it opens above when
// there is not enough room below and the upper side offers more; horizontally it
// is pulled back inside its panel, and capped, when it would otherwise run past
// the edge. Both decisions are re-made on every converge pass, so each one is
// applied only when it changes: a class flipped off and on again costs the whole
// tree another post-layout dispatch and solve.
void Dropdown::UpdatePopupPlacement()
{
    const auto setUpward = [this](bool upward)
    {
        if (upward)
            AddClass("open-upward");
        else
            RemoveClass("open-upward");
    };

    if (!m_ItemsContainer)
    {
        setUpward(false);
        return;
    }

    UIManager* manager = GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!root)
    {
        setUpward(false);
        return;
    }

    // A popup may be well inside the window yet immediately cross a clipped
    // dock/panel boundary. Use the intersection of genuine viewport ancestors
    // as the available space so bottom-edge dropdowns flip upward before
    // their rows overlap another dock and lose pointer hit testing, and so a
    // popup in a narrow panel is measured against that panel.
    //
    // Only a ScrollView's clip viewport or a dock panel box count: either one
    // imposes a real limit independent of its content. A plain overflow:hidden
    // wrapper (e.g. one used only to force ellipsis text truncation) sizes
    // itself to fit its own content and clips nothing under normal layout, so
    // treating it as a boundary here starves the room estimate and flips the
    // popup upward for no visible reason.
    float visibleTop = root->GetLayoutY();
    float visibleBottom = visibleTop + root->GetLayoutHeight();
    float visibleLeft = root->GetLayoutX();
    float visibleRight = visibleLeft + root->GetLayoutWidth();
    for (UIElement* ancestor = GetParent(); ancestor; ancestor = ancestor->GetParent())
    {
        if (!ancestor->HasClass("scroll-viewport") && !ancestor->HasClass("panel"))
            continue;
        visibleTop = std::max(visibleTop, ancestor->GetLayoutY());
        visibleBottom = std::min(visibleBottom,
                                 ancestor->GetLayoutY() + ancestor->GetLayoutHeight());
        visibleLeft = std::max(visibleLeft, ancestor->GetLayoutX());
        visibleRight = std::min(visibleRight,
                                ancestor->GetLayoutX() + ancestor->GetLayoutWidth());
    }

    constexpr float kPopupEdgeClearance = 2.0f;

    const float headerTop = GetLayoutY();
    const float headerBottom = headerTop + GetLayoutHeight();
    const float roomAbove = std::max(0.0f, headerTop - visibleTop);
    const float roomBelow = std::max(0.0f, visibleBottom - headerBottom);

    // A closed popup has no current layout height. Use the standard dropdown
    // row height for its first opening, then prefer the measured height on
    // subsequent openings.
    constexpr float kEstimatedRowHeight = 24.0f;
    constexpr float kEstimatedPopupPadding = 4.0f;
    const float measuredHeight = m_ItemsContainer->GetLayoutHeight();
    const float estimatedHeight =
        kEstimatedPopupPadding + static_cast<float>(m_Options.size()) * kEstimatedRowHeight;
    const float requiredHeight =
        measuredHeight > 1.0f ? measuredHeight + kPopupEdgeClearance : estimatedHeight;
    setUpward(roomBelow < requiredHeight && roomAbove > roomBelow);

    const float available = visibleRight - visibleLeft - 2.0f * kPopupEdgeClearance;
    const float headerLeft = GetLayoutX();
    // Before the first layout the popup has no width of its own; it is at least
    // as wide as the header it matches.
    const float popupWidth = std::max(m_ItemsContainer->GetLayoutWidth(), GetLayoutWidth());
    const float overflowRight = (headerLeft + popupWidth) - (visibleRight - kPopupEdgeClearance);
    float offset = overflowRight > 0.0f ? -overflowRight : 0.0f;
    const float leftLimit = (visibleLeft + kPopupEdgeClearance) - headerLeft;
    offset = std::max(offset, std::min(0.0f, leftLimit));
    if (offset < 0.0f)
    {
        m_ItemsContainer->Overrides().Set(Style::PositionLeft, StyleLength::Px(offset));
        // A popup that has to be pulled back does not fit where it was anchored,
        // and it was sized against the header, not the panel: give it the panel's
        // width so its rows use the space that is there instead of wrapping
        // beside it.
        if (available > 0.0f)
            m_ItemsContainer->Overrides().Set(Style::MinWidth, StyleLength::Px(available));
    }
    else
    {
        m_ItemsContainer->Overrides().Reset(Style::PositionLeft);
        m_ItemsContainer->Overrides().Reset(Style::MinWidth);
    }
}

void Dropdown::EnsureHeader()
{
    if (m_HeaderLabel)
        return;

    // Header container (the clickable row)
    auto container = std::make_unique<UIElement>();
    m_HeaderContainer = container.get();
    m_HeaderContainer->AddClass("dropdown-header");
    /* Same shared class TextFieldBase puts on its inner editor: a dropdown
       header is a field editor for styling purposes, so one selector can reach
       every field's editable surface instead of enumerating control types. */
    m_HeaderContainer->AddClass("field-editor");
    // Delegate focus for mouse/tap interactions on the header to the
    // Dropdown itself so keyboard focus and UI.FocusIn/Out are expressed in
    // terms of the control, not its internal label.
    m_HeaderContainer->SetFocusProxy(this);

    m_HeaderContainer->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        // Treat primary mouse button as the activator for opening the menu.
        if (e.Button != 0 && e.Button != 1)
            return;
        if (!IsEnabled())
            return;

        if (m_Mode == Mode::Native)
            OpenMenuNative();
        else
            OpenMenuUi();

        e.Stop();
    });

    // Text label (fills available space) — proxy focus to the Dropdown so
    // clicking the label (or chevron) registers focus on the outer control
    // and FocusOut fires correctly when the user clicks elsewhere.
    auto lbl = std::make_unique<Label>();
    m_HeaderLabel = lbl.get();
    m_HeaderLabel->AddClass("dropdown-header-label");
    m_HeaderLabel->SetFocusProxy(this);
    m_HeaderContainer->AddChild(std::move(lbl));

    // Chevron indicator (right-aligned)
    auto chevron = std::make_unique<UIElement>();
    m_Chevron = chevron.get();
    m_Chevron->AddClass("dropdown-chevron");
    m_Chevron->SetFocusProxy(this);
    m_HeaderContainer->AddChild(std::move(chevron));

    AddChild(std::move(container));
}

void Dropdown::EnsureHeaderIcon()
{
    if (m_HeaderIcon || !m_HeaderContainer)
        return;

    auto icon = std::make_unique<UIElement>();
    m_HeaderIcon = icon.get();
    m_HeaderIcon->AddClass("dropdown-header-icon");
    m_HeaderIcon->SetFocusProxy(this);
    // Leading slot: the header reads icon, label, chevron.
    m_HeaderContainer->InsertChild(0, std::move(icon));
}

void Dropdown::RemoveHeaderIcon()
{
    if (!m_HeaderIcon)
        return;
    m_HeaderContainer->RemoveChild(m_HeaderIcon);
    m_HeaderIcon = nullptr;
    m_HeaderIconClass.clear();
}

void Dropdown::EnsureItemsContainer()
{
    if (m_ItemsContainer)
    {
        // Give the popup container a stable, discoverable id for automation/debugging.
        // This is intentionally derived from the dropdown id so UI replay scenarios can
        // target/probe the popup without needing extra theme/layout authoring.
        if (!GetId().empty())
            m_ItemsContainer->SetId(GetId() + ":items");
        return;
    }

    auto container = std::make_unique<UIElement>();
    m_ItemsContainer = container.get();
    m_ItemsContainer->AddClass("dropdown-items");
    if (!GetId().empty())
        m_ItemsContainer->SetId(GetId() + ":items");
    // Keep focus ownership on the Dropdown when interacting with the popup
    // list so that losing focus (clicking elsewhere) can reliably close it.
    m_ItemsContainer->SetFocusProxy(this);
    m_ItemsContainer->SetOverlayLayer(OverlayLayer::Dropdown);
    m_ItemsContainer->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        // The popup is an input shield: clicks on its background must never
        // fall through to controls visually underneath it.
        e.Capture(this);
        e.Stop();
    });
    AddChild(std::move(container));
}

void Dropdown::RebuildUiItems()
{
    // If this is called from within an event dispatch (e.g. the header's
    // mouse-down handler), UIElement::RemoveChild will defer actual child
    // removal via PostAction(). A synchronous while-loop that expects
    // GetChildren() to shrink would then spin forever. Mirror the TreeView
    // pattern and defer the entire rebuild to a safe point after dispatch.
    if (UIElement::IsInEventDispatch())
    {
        this->PostSafeAction([this]() { this->RebuildUiItems(); });
        return;
    }

    if (!m_ItemsContainer)
        return;

    // Clear any existing items.
    m_ItemsContainer->RemoveAllChildren();

    for (size_t i = 0; i < m_Options.size(); ++i)
    {
        auto  lbl = std::make_unique<Label>();
        Label* raw = lbl.get();
        raw->AddClass("dropdown-item");
        raw->SetFocusProxy(this);
        const int index = static_cast<int>(i);
        const bool isSelected = (m_SelectedIndex == index);
        if (isSelected)
            raw->AddClass("selected");
        if (m_HasOptionIcons)
        {
            // Every row of an annotated control gets the column, so a row whose
            // option declares no icon still lines its text up with the rest.
            auto icon = std::make_unique<UIElement>();
            icon->AddClass("dropdown-item-icon");
            if (!m_Options[i].iconClass.empty())
                icon->AddClass(m_Options[i].iconClass);
            icon->SetFocusProxy(this);
            raw->AddChild(std::move(icon));
            // A Label with children is no longer a Yoga text-measure leaf. Keep
            // the option text in its own flow child so both row height and
            // fit-content popup width still include the glyph metrics.
            auto text = std::make_unique<Label>();
            text->AddClass("dropdown-item-label");
            text->SetText(m_Options[i].label);
            text->SetFocusProxy(this);
            raw->AddChild(std::move(text));
        }
        else
        {
            raw->SetText(m_Options[i].label);
        }
        ApplyOptionColorToItemLabel(ItemTextLabel(raw), m_Options[i], isSelected);

        raw->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) {
            if (e.Button != 0 && e.Button != 1)
                return;
            e.Capture(this);
            SetSelectedIndex(index);
            // Close the popup list; mirror whatever OpenMenuUi() does so CSS
            // rules can rely either on the root .dropdown or the items
            // container carrying an "open" class.
            RemoveClass("open");
            if (m_ItemsContainer)
                m_ItemsContainer->RemoveClass("open");
            e.Stop();
        });

        m_ItemsContainer->AddChild(std::move(lbl));
    }
}

void Dropdown::UpdateHeaderLabel()
{
    EnsureHeader();
    if (!m_HeaderLabel)
        return;

    UpdateHeaderIcon();

    if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Options.size()))
    {
        const Option& opt = m_Options[static_cast<size_t>(m_SelectedIndex)];
        m_HeaderLabel->SetText(opt.label);
        if (!opt.color.empty())
        {
            uint32_t argb = 0;
            if (TryParseHexColor(opt.color.c_str(), argb))
                m_HeaderLabel->Overrides().Set(Style::Color, argb);
            else
                m_HeaderLabel->Overrides().Reset(Style::Color);
        }
        else
            m_HeaderLabel->Overrides().Reset(Style::Color);
    }
    else
    {
        m_HeaderLabel->SetText(std::string());
        m_HeaderLabel->Overrides().Reset(Style::Color);
    }
}

void Dropdown::UpdateHeaderIcon()
{
    if (!m_HeaderIcon)
        return;

    const Option* selected = SelectedOption();
    const std::string& iconClass = selected ? selected->iconClass : kNoIconClass;
    if (iconClass == m_HeaderIconClass)
        return;

    if (!m_HeaderIconClass.empty())
        m_HeaderIcon->RemoveClass(m_HeaderIconClass);
    m_HeaderIconClass = iconClass;
    if (!m_HeaderIconClass.empty())
        m_HeaderIcon->AddClass(m_HeaderIconClass);
}

void Dropdown::OpenMenuUi()
{
    EnsureItemsContainer();
    RebuildUiItems();

    // Toggle an "open" class on both the dropdown root and the items
    // container. The Editor theme styles dropdowns primarily via the items
    // container, so mirroring the state there makes it easy to use simple
    // selectors like ".dropdown-items.open" in addition to descendant-based
    // rules.
    const bool isOpen = m_ItemsContainer && m_ItemsContainer->HasClass("open");
    if (isOpen)
    {
        RemoveClass("open");
        if (m_ItemsContainer)
            m_ItemsContainer->RemoveClass("open");
        UpdateHeaderLabel();
    }
    else
    {
        UpdatePopupPlacement();
        AddClass("open");
        if (m_ItemsContainer)
            m_ItemsContainer->AddClass("open");
        if (m_HeaderLabel)
            m_HeaderLabel->Overrides().Reset(Style::Color);
    }
    // Opening/closing the popup changes style/layout (display:none <-> flex). We rely on the
    // dirty marks from AddClass/RemoveClass and UIManager's "late dirty" rebuild to reflect
    // the change in the same frame (for both mouse and keyboard activation) without forcing
    // a redundant YogaSolve-only pass.
}

void Dropdown::CloseMenuUi()
{
    RemoveClass("open");
    if (m_ItemsContainer)
        m_ItemsContainer->RemoveClass("open");
    UpdateHeaderLabel();
}

bool Dropdown::IsMenuOpen() const
{
    return HasClass("open") || (m_ItemsContainer && m_ItemsContainer->HasClass("open"));
}

void Dropdown::OpenMenuNative()
{
    if (!m_NativeMenuInvoker)
    {
        // Fallback to UI-mode behavior if no native invoker is installed.
        OpenMenuUi();
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(m_Options.size());
    for (const auto& opt : m_Options)
        labels.push_back(opt.label);

    int current = m_SelectedIndex;
    Dropdown* self = this;
    auto       onSelected = [self](int index) {
        if (!self)
            return;
        self->SetSelectedIndex(index);
    };

    m_NativeMenuInvoker(*this, labels, current, std::move(onSelected));
}

void Dropdown::OnEvent(UIEvent& e)
{
    if (e.Id == kEventFocusOut)
    {
        // Auto-close the UI dropdown when focus is lost.
        CloseMenuUi();
    }

    // A disabled control never activates. Focus and the pointer already skip a
    // disabled element and everything inside it; this gate is the control's own
    // guarantee, so a key that arrives anyway cannot open the menu and show a
    // picked value its owner refuses to store.
    if (e.Id == kEventKeyDown && IsEnabled())
    {
        if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
        {
            if (m_Mode == Mode::Native)
                OpenMenuNative();
            else
                OpenMenuUi();
            e.Stop();
            return;
        }
    }

    // Defer all other events to the base Field bridge so focus/text behavior
    // remains consistent with other field-like controls.
    Field<std::string>::OnEvent(e);
}

} // namespace GameEngine
