#include "UI/Controls/SearchDialog.h"

#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Interaction/FocusIsInside.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine
{

namespace
{
constexpr float kDefaultResultRowHeight = 36.0f;
constexpr float kMinResultIconSize = 20.0f;
constexpr float kMaxResultIconSize = 512.0f;
constexpr float kResultIconStep = 4.0f;
constexpr float kResultRowPadding = 8.0f;
// Pre-style-resolution fallbacks for GetChromeHeight(). Once the stylesheet is
// resolved the live CSS values are authoritative; these only cover the frames
// before that.
constexpr float kFallbackSearchFieldHeight = 36.0f; // margins + bar, `.search-dialog-search-bar`
constexpr float kFallbackStatusBarHeight = 28.0f;   // `.search-dialog-status`
constexpr float kFallbackDragHandleHeight = 18.0f;  // `.search-dialog-drag-handle`
constexpr float kViewportMargin    = 8.0f;
constexpr float kMinPanelWidth     = 200.0f;

/// The element's styled fixed height plus vertical margins, or `fallback` until
/// the stylesheet provides a px height.
float StyledOuterHeight(const UIElement* element, float fallback)
{
    if (!element)
        return fallback;
    const auto& layout = element->GetResolvedStyle().Layout;
    if (!layout.Height.IsPx())
        return fallback;
    return layout.Height.Value + layout.Margin.Top + layout.Margin.Bottom;
}

void CaptureRootForCurrentPress(UIElement& element, UIEvent& event)
{
    UIManager* owner = element.GetOwnerManager();
    UIElement* root = owner ? owner->GetRootElement() : nullptr;
    if (root)
        event.Capture(root);
}

void BindSearchResultIcon(UIElement& iconElement, const SearchIcon& icon, float size)
{
    iconElement.Overrides()
        .Set(Style::Width, StyleLength::Px(size))
        .Set(Style::Height, StyleLength::Px(size))
        .Set(Style::MinWidth, StyleLength::Px(size))
        .Set(Style::MinHeight, StyleLength::Px(size));

    std::vector<std::string> staleIconClasses;
    for (const auto& cls : iconElement.GetClasses())
    {
        if (cls == "inspector-section-icon" || cls.starts_with("icon-") ||
            cls.starts_with("inspector-section-icon-"))
            staleIconClasses.push_back(cls);
    }
    for (const std::string& cls : staleIconClasses)
        iconElement.RemoveClass(cls);

    if (icon.HasImage())
    {
        constexpr std::string_view kEnginePrefix = "engine:";
        if (icon.ImagePath.rfind(kEnginePrefix, 0) == 0)
            UI::Layout::SetBackgroundResourceName(iconElement, icon.ImagePath.substr(kEnginePrefix.size()));
        else
            UI::Layout::SetBackgroundPath(iconElement, icon.ImagePath);
        iconElement.RemoveClass("hidden");
        iconElement.AddClass("has-thumb");
    }
    else if (icon.HasCssClass())
    {
        UI::Layout::DisableBackgroundOverride(iconElement);
        iconElement.RemoveClass("has-thumb");
        if (icon.CssClass.starts_with("inspector-section-icon-"))
            iconElement.AddClass("inspector-section-icon");
        iconElement.AddClass(icon.CssClass);
        iconElement.RemoveClass("hidden");
    }
    else
    {
        UI::Layout::ClearBackgroundOverride(iconElement);
        iconElement.RemoveClass("has-thumb");
        iconElement.AddClass("hidden");
    }
}
} // namespace

// ---------------------------------------------------------------------------
// ResultsDataProvider — bridges m_Results vector to IListDataProvider
// ---------------------------------------------------------------------------
class SearchDialog::ResultsDataProvider : public ListChangeTrackingProvider
{
public:
    void SetItemHeight(float height)
    {
        if (m_ItemHeight == height)
            return;
        m_ItemHeight = height;
        MarkAllChanged();
    }

    void SetSource(const std::vector<SearchResultItem>* source)
    {
        m_Source = source;
        MarkAllChanged();
    }

    void NotifyAppended(int count)
    {
        if (!m_Source || count <= 0)
            return;
        const int total = static_cast<int>(m_Source->size());
        for (int i = total - count; i < total; ++i)
            MarkChanged((*m_Source)[i].Id);
    }

    int GetItemCount() const override
    {
        return m_Source ? static_cast<int>(m_Source->size()) : 0;
    }

    ListId GetItemId(int index) const override
    {
        if (!m_Source || index < 0 || index >= static_cast<int>(m_Source->size()))
            return 0;
        return (*m_Source)[index].Id;
    }

    float GetItemHeight(int /*index*/) const override { return m_ItemHeight; }

    const SearchResultItem* GetItemByIndex(int index) const
    {
        if (!m_Source || index < 0 || index >= static_cast<int>(m_Source->size()))
            return nullptr;
        return &(*m_Source)[index];
    }

private:
    const std::vector<SearchResultItem>* m_Source = nullptr;
    float m_ItemHeight = kDefaultResultRowHeight;
};

class SearchDialog::ResultsGridDataProvider : public GridChangeTrackingProvider
{
public:
    void SetSource(const std::vector<SearchResultItem>* source)
    {
        m_Source = source;
        MarkStructureChanged();
    }

    void NotifyStructureChanged() { MarkStructureChanged(); }

    int GetItemCount() const override
    {
        return m_Source ? static_cast<int>(m_Source->size()) : 0;
    }

    GridId GetItemId(int index) const override
    {
        if (!m_Source || index < 0 || index >= static_cast<int>(m_Source->size()))
            return 0;
        return (*m_Source)[index].Id;
    }

    const char* GetLabel(GridId id) const override
    {
        const SearchResultItem* item = GetItemById(id);
        return item ? item->Label.c_str() : "";
    }

    uint64_t GetIcon(GridId id) const override
    {
        const SearchResultItem* item = GetItemById(id);
        return item ? item->Icon.TextureHandle : 0;
    }

    const char* GetTypeKey(GridId id) const override
    {
        const SearchResultItem* item = GetItemById(id);
        return item ? item->TypeKey.c_str() : "";
    }

    void ApplySort(const SortDescriptor&) override {}
    void ApplyGrouping(const GroupDescriptor&) override {}

    const SearchResultItem* GetItemById(GridId id) const
    {
        if (!m_Source)
            return nullptr;
        const auto found = std::find_if(m_Source->begin(), m_Source->end(),
                                        [id](const SearchResultItem& item) { return item.Id == id; });
        return found == m_Source->end() ? nullptr : &*found;
    }

private:
    const std::vector<SearchResultItem>* m_Source = nullptr;
};

// ---------------------------------------------------------------------------
// SearchDialog
// ---------------------------------------------------------------------------
SearchDialog::~SearchDialog()
{
}

SearchDialog::SearchDialog()
    : DismissablePopup(this)
{
    AddClass("search-dialog");
    RequestSubtreeStyleAssetPath("UI/controls/SearchDialog.css", "editor");
    SetOverlayLayer(OverlayLayer::Modal);

    int dialogId = s_NextDialogId.fetch_add(1, std::memory_order_relaxed);
    m_FieldId = "search-dialog-field-" + std::to_string(dialogId);

    BuildStructure();
}

void SearchDialog::SetProvider(ISearchProvider* provider)
{
    m_Provider = provider;
    if (m_SearchBar)
        m_SearchBar->SetPlaceholder(provider ? provider->GetPlaceholderText() : std::string{});
}

void SearchDialog::SetFilterOptions(std::vector<SearchDialogFilterOption> options)
{
    m_FilterOptions = std::move(options);
    m_QueryPrefix.clear();

    if (!m_SearchBar || !m_SearchFilter || !m_SearchField)
        return;

    if (m_FilterOptions.empty())
    {
        m_SearchBar->SetFilterOptions({});
        return;
    }

    std::vector<Dropdown::Option> dropdownOptions;
    dropdownOptions.reserve(m_FilterOptions.size());
    for (const SearchDialogFilterOption& option : m_FilterOptions)
        dropdownOptions.push_back({option.Value, option.Label});

    m_QueryPrefix = m_FilterOptions.front().QueryPrefix;
    m_SearchBar->SetFilterOptions(dropdownOptions, 0);
    if (m_QueryPrefix.empty())
        m_SearchBar->RemoveClass("search-filter-active");
    else
        m_SearchBar->AddClass("search-filter-active");
}

void SearchDialog::SetPanelWidth(float width)
{
    m_PanelWidth = std::max(kMinPanelWidth, width);
    ApplyClampedPanelPosition();
}

void SearchDialog::SetMaxListHeight(float height)
{
    m_MaxListHeight = std::max(m_ResultRowHeight, height);
    UpdatePanelHeight();
}

void SearchDialog::SetResultIconSize(float size)
{
    const float quantized = std::round(std::clamp(size, kMinResultIconSize, kMaxResultIconSize) /
                                       kResultIconStep) * kResultIconStep;
    if (quantized == m_ResultIconSize)
        return;

    const int selectedIndex = m_ResultsList ? m_ResultsList->GetSelectedIndex() : -1;
    const float scrollOffset = m_ResultsList ? m_ResultsList->GetScrollOffset() : 0.0f;
    m_ResultIconSize = quantized;
    m_ResultRowHeight = std::max(kDefaultResultRowHeight, m_ResultIconSize + kResultRowPadding);
    m_ResultsProvider->SetItemHeight(m_ResultRowHeight);
    if (m_ResultsGrid)
        m_ResultsGrid->SetIconSize(std::max(32.0f, m_ResultIconSize));
    if (m_ResultsList)
    {
        m_ResultsList->RefreshFromProvider();
        PostSafeAction([this, selectedIndex, scrollOffset]()
        {
            if (!m_ResultsList)
                return;
            m_ResultsList->SetScrollOffset(scrollOffset);
            if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_Results.size()))
                m_ResultsList->SetSelectedIndex(selectedIndex, false);
        });
    }
    UpdatePanelHeight();
}

void SearchDialog::SetGridMode(bool enabled)
{
    if (m_GridMode == enabled || !m_ResultsList || !m_ResultsGrid)
        return;

    const int selectedIndex = m_GridMode ? m_ResultsGrid->GetSelectedIndex()
                                         : m_ResultsList->GetSelectedIndex();
    m_GridMode = enabled;
    UpdateResultViewToggleButton();
    if (enabled)
    {
        m_ResultsList->AddClass("view-hidden");
        m_ResultsGrid->AddClass("active");
        m_ResultsGrid->InvalidateVirtualization();
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_Results.size()))
            m_ResultsGrid->SetSelectedIndex(selectedIndex, true);
    }
    else
    {
        m_ResultsGrid->RemoveClass("active");
        m_ResultsList->RemoveClass("view-hidden");
        m_ResultsList->InvalidateVirtualization();
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_Results.size()))
            m_ResultsList->SetSelectedIndex(selectedIndex, true);
    }

    FocusSearchField();
}

void SearchDialog::UpdateResultViewToggleButton()
{
    if (!m_ResultViewToggle)
        return;
    m_ResultViewToggle->RemoveClass("grid-view-icon");
    m_ResultViewToggle->RemoveClass("list-view-icon");
    if (m_GridMode)
    {
        m_ResultViewToggle->AddClass("list-view-icon");
        m_ResultViewToggle->AddClass("active");
        m_ResultViewToggle->SetTooltip("List View");
    }
    else
    {
        m_ResultViewToggle->AddClass("grid-view-icon");
        m_ResultViewToggle->RemoveClass("active");
        m_ResultViewToggle->SetTooltip("Grid View");
    }
}

void SearchDialog::SetCentered(bool centered)
{
    m_Centered = centered;
    ApplyClampedPanelPosition();
}

void SearchDialog::SetManipulationEnabled(bool enabled)
{
    m_ManipulationEnabled = enabled;
    m_DraggingPanel = false;
    m_ResizingPanel = false;
    if (m_Panel)
    {
        if (enabled)
            m_Panel->AddClass("draggable-resizable");
        else
            m_Panel->RemoveClass("draggable-resizable");
    }
    UpdatePanelHeight();
}

float SearchDialog::GetChromeHeight() const
{
    // Chrome metrics come from the stylesheet so the panel-height math cannot
    // drift when the CSS changes.
    return StyledOuterHeight(m_SearchBar, kFallbackSearchFieldHeight) +
           StyledOuterHeight(m_StatusBar, kFallbackStatusBarHeight) +
           (m_ManipulationEnabled ? StyledOuterHeight(m_DragHandle, kFallbackDragHandleHeight) : 0.0f);
}

void SearchDialog::BeginPanelDrag(UIEvent& event)
{
    if (!m_ManipulationEnabled || event.Button != 0)
        return;
    m_DraggingPanel = true;
    m_ResizingPanel = false;
    m_Centered = false;
    m_HorizontalAnchor = SearchDialogHorizontalAnchor::LeadingLeft;
    m_AnchorHeight = 0.0f;
    m_GestureStartX = event.X;
    m_GestureStartY = event.Y;
    m_GesturePanelX = m_Panel->GetLayoutX();
    m_GesturePanelY = m_Panel->GetLayoutY();
    m_AnchorX = m_GesturePanelX;
    m_AnchorY = m_GesturePanelY;
    // Capture the element whose handler began the gesture. The top and bottom
    // bars own their full down/move/up sequences, matching controls such as the
    // resize grip and avoiding any dependency on ancestor bubbling while held.
    event.Capture(event.CurrentTarget ? event.CurrentTarget : m_Panel);
    event.Stop();
}

void SearchDialog::ContinuePanelDrag(UIEvent& event)
{
    if (!m_DraggingPanel)
        return;
    if (!event.ButtonDown)
    {
        m_DraggingPanel = false;
        event.Stop();
        return;
    }
    m_AnchorX = m_GesturePanelX + (event.X - m_GestureStartX);
    m_AnchorY = m_GesturePanelY + (event.Y - m_GestureStartY);
    ApplyClampedPanelPosition();
    event.Stop();
}

void SearchDialog::EndPanelDrag(UIEvent& event)
{
    if (!m_DraggingPanel)
        return;
    m_DraggingPanel = false;
    event.Stop();
}

void SearchDialog::SetAnchorPosition(float x, float y, SearchDialogHorizontalAnchor horizontalAnchor,
                                     float anchorHeight)
{
    m_AnchorX = x;
    m_AnchorY = y;
    m_HorizontalAnchor = horizontalAnchor;
    m_AnchorHeight = anchorHeight;
    ApplyClampedPanelPosition();
}

void SearchDialog::ApplyClampedPanelPosition()
{
    if (!m_Panel)
        return;

    float vw = 0.0f;
    float vh = 0.0f;
    if (auto* mgr = GetOwnerManager())
    {
        if (UIElement* root = mgr->GetRootElement())
        {
            vw = root->GetLayoutWidth();
            vh = root->GetLayoutHeight();
        }
    }
    if (vw <= 1.0f || vh <= 1.0f)
    {
        vw = GetLayoutWidth();
        vh = GetLayoutHeight();
    }
    if (vw <= 1.0f || vh <= 1.0f)
    {
        float left = m_AnchorX;
        if (m_HorizontalAnchor == SearchDialogHorizontalAnchor::TrailingRight)
            left = m_AnchorX - m_PanelWidth;
        m_Panel->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionLeft, StyleLength::Px(left))
            .Set(Style::PositionTop, StyleLength::Px(m_AnchorY))
            .Set(Style::PositionRight, StyleLength::Auto())
            .Set(Style::PositionBottom, StyleLength::Auto())
            .Set(Style::Width, StyleLength::Px(m_PanelWidth));
        m_Panel->MarkDirty(LayoutDirty | VisualDirty);
        return;
    }

    const float desiredListHeight = m_FixedHeight
        ? m_MaxListHeight
        : std::min(static_cast<float>(m_Results.size()) * m_ResultRowHeight, m_MaxListHeight);
    // Reserve the search and status bars first. Only the scrollable results area
    // shrinks on short viewports, so the query field can never be pushed or clipped
    // off-screen.
    const float chromeHeight = GetChromeHeight();
    const float availablePanelHeight = std::max(chromeHeight, vh - 2.0f * kViewportMargin);
    const float listHeight = std::min(desiredListHeight, std::max(0.0f, availablePanelHeight - chromeHeight));
    const float panelH = chromeHeight + listHeight;
    UI::Layout::SetForcedHeight(*m_Panel, static_cast<int>(panelH));

    const float maxW = std::max(kMinPanelWidth, vw - 2.0f * kViewportMargin);
    const float panelW = std::min(m_PanelWidth, maxW);

    float x = m_Centered ? (vw - panelW) * 0.5f : m_AnchorX;
    if (!m_Centered && m_HorizontalAnchor == SearchDialogHorizontalAnchor::TrailingRight)
        x = m_AnchorX - panelW;

    // Decide below-vs-above ONCE per open, using the panel's MAX height so that
    // filtering (which changes the live height) never flips the chosen side. An
    // above-placed panel pins its bottom to the field's top and grows upward.
    if (m_AnchorHeight > 0.0f && m_AnchorDecisionPending)
    {
        const float maxPanelH = GetChromeHeight() + m_MaxListHeight;
        m_PlaceAbove = (m_AnchorY + maxPanelH > vh - kViewportMargin);
        m_AnchorDecisionPending = false;
    }

    float y = m_Centered
        ? (vh - panelH) * 0.5f
        : ((m_AnchorHeight > 0.0f && m_PlaceAbove)
               ? (m_AnchorY - m_AnchorHeight - panelH)  // bottom pinned at the field's top
               : m_AnchorY);

    if (x + panelW > vw - kViewportMargin)
        x = vw - kViewportMargin - panelW;
    if (x < kViewportMargin)
        x = kViewportMargin;

    if (y + panelH > vh - kViewportMargin)
        y = vh - kViewportMargin - panelH;
    if (y < kViewportMargin)
        y = kViewportMargin;

    m_Panel->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(x))
        .Set(Style::PositionTop, StyleLength::Px(y))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(panelW));
    m_Panel->MarkDirty(LayoutDirty | VisualDirty);
}

void SearchDialog::UpdatePanelHeight()
{
    const float listHeight = m_FixedHeight
        ? m_MaxListHeight
        : std::min(static_cast<float>(m_Results.size()) * m_ResultRowHeight, m_MaxListHeight);
    const int panelHeight = static_cast<int>(GetChromeHeight() + listHeight);
    UI::Layout::SetForcedHeight(*m_Panel, panelHeight);
    ApplyClampedPanelPosition();
}

void SearchDialog::BuildStructure()
{
    m_ResultsProvider = std::make_unique<ResultsDataProvider>();
    m_ResultsGridProvider = std::make_unique<ResultsGridDataProvider>();

    // ---- Backdrop: fullscreen, click-to-dismiss ----
    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("search-dialog-backdrop");
    m_Backdrop->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        CaptureRootForCurrentPress(*this, e);
        Close();
        if (m_OnCancel)
            m_OnCancel();
        e.Stop();
    });
    UIElement::AddChild(std::move(backdrop));

    // ---- Panel: centered dialog container ----
    auto panel = std::make_unique<UIElement>();
    m_Panel = panel.get();
    m_Panel->AddClass("search-dialog-panel");
    // Drag gestures are owned entirely by the drag handle (top bar) and the
    // status bar (bottom bar); both capture their full down/move/up sequence.

    // Optional move handle. It is part of the panel's chrome but remains hidden
    // for anchored picker dialogs.
    auto dragHandle = std::make_unique<Label>();
    m_DragHandle = dragHandle.get();
    m_DragHandle->SetText("");
    m_DragHandle->AddClass("search-dialog-drag-handle");
    m_DragHandle->SetTooltip("Drag search panel");
    m_DragHandle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) { BeginPanelDrag(e); });
    m_DragHandle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { ContinuePanelDrag(e); });
    m_DragHandle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) { EndPanelDrag(e); });

    m_Panel->AddChild(std::move(dragHandle));

    // ---- Shared search field chrome ----
    // Keep the field, filter, icon, and clear action as siblings. This matches
    // panel searches and lets the dropdown use the panel viewport for popup
    // placement instead of treating the text field as a clipping ancestor.
    auto searchBar = std::make_unique<SearchFieldWithFilter>();
    m_SearchBar = searchBar.get();
    m_SearchBar->AddClass("search-dialog-search-bar");
    m_SearchBar->SetFieldId(m_FieldId);
    m_SearchBar->SetRefocusAfterClear(true);

    m_SearchField = m_SearchBar->GetField();
    m_SearchField->AddClass("search-dialog-query-field");
    m_SearchField->RegisterEventHandler(kEventMouseDown, [this](UIEvent&) {
        if (auto* owner = GetOwnerManager())
            owner->SetFocusById(m_FieldId);
    });
    m_SearchBar->SetOnQueryChanging([this](const std::string& query) {
        m_InitialSelectionPending = false;
        OnQueryChanged(query);
    });

    m_SearchField->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (!m_Open)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            SelectHighlighted();
            e.Stop();
        }
        else if (e.Key == Input::kKeyCode_Down)
        {
            MoveHighlight(1);
            e.Stop();
        }
        else if (e.Key == Input::kKeyCode_Up)
        {
            MoveHighlight(-1);
            e.Stop();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            Close();
            if (m_OnCancel)
                m_OnCancel();
            e.Stop();
        }
    });

    m_SearchIcon = m_SearchBar->GetSearchIcon();
    m_SearchFilter = m_SearchBar->GetFilter();
    m_SearchFilter->AddClass("search-dialog-filter");
    m_SearchBar->SetOnFilterChanged([this](const std::string& value) {
        const auto selected = std::find_if(
            m_FilterOptions.begin(), m_FilterOptions.end(),
            [&value](const SearchDialogFilterOption& option) { return option.Value == value; });
        if (selected == m_FilterOptions.end())
            return;

        m_QueryPrefix = selected->QueryPrefix;
        if (m_QueryPrefix.empty())
            m_SearchBar->RemoveClass("search-filter-active");
        else
            m_SearchBar->AddClass("search-filter-active");

        if (m_Open)
        {
            FocusSearchField();
            ClearResults();
            OnQueryChanged(m_SearchField->GetValue());
        }
    });

    m_ClearButton = m_SearchBar->GetClearButton();
    m_ClearButton->AddClass("search-dialog-clear");
    m_Panel->AddChild(std::move(searchBar));

    // ---- Results list ----
    auto list = std::make_unique<ListView>();
    m_ResultsList = list.get();
    m_ResultsList->SetId("search-dialog-results");
    m_ResultsList->AddClass("search-dialog-results");
    m_ResultsList->SetDataProvider(m_ResultsProvider.get());
    m_ResultsList->SetPickerActivation(true);

    m_ResultsList->SetItemFactory([](ListId /*id*/, IListDataProvider* /*provider*/) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("search-result-row");

        // Icon
        auto icon = std::make_unique<UIElement>();
        icon->AddClass("search-result-icon");
        row->AddChild(std::move(icon));

        // Text wrapper (label + detail stacked vertically)
        auto textWrapper = std::make_unique<UIElement>();
        textWrapper->AddClass("search-result-text");

        auto label = std::make_unique<Label>();
        label->AddClass("search-result-label");
        textWrapper->AddChild(std::move(label));

        auto detail = std::make_unique<Label>();
        detail->AddClass("search-result-detail");
        textWrapper->AddChild(std::move(detail));

        row->AddChild(std::move(textWrapper));
        return row;
    });

    m_ResultsList->SetItemBinder([this](UIElement* cell, ListId /*id*/,
                                        int index, IListDataProvider* /*provider*/) {
        const SearchResultItem* item = m_ResultsProvider->GetItemByIndex(index);
        if (!item)
            return;

        auto& children = cell->GetChildren();
        if (children.size() < 2)
            return;

        // Icon element — strip stale icon classes/backgrounds from cell reuse.
        UIElement* iconEl = children[0].get();
        BindSearchResultIcon(*iconEl, item->Icon, m_ResultIconSize);

        // Text wrapper -> label + detail
        UIElement* textWrapper = children[1].get();
        auto& textChildren = textWrapper->GetChildren();
        if (textChildren.size() >= 2)
        {
            static_cast<Label*>(textChildren[0].get())->SetText(item->Label);
            static_cast<Label*>(textChildren[1].get())->SetText(item->Detail);
        }
    });

    m_ResultsList->SetOnItemActivated([this](ListId id) {
        OnItemActivated(id);
    });
    m_Panel->AddChild(std::move(list));

    auto grid = std::make_unique<GridView>();
    m_ResultsGrid = grid.get();
    m_ResultsGrid->AddClass("search-dialog-grid-results");
    m_ResultsGrid->SetDataProvider(m_ResultsGridProvider.get());
    m_ResultsGrid->SetPickerActivation(true);
    m_ResultsGrid->SetIconSize(std::max(32.0f, m_ResultIconSize));
    m_ResultsGrid->SetItemBinder([this](UIElement* cell, GridId id, IGridDataProvider*)
    {
        const SearchResultItem* item = m_ResultsGridProvider->GetItemById(id);
        if (!item)
            return;
        cell->AddClass("search-result-grid-card");
        cell->SetTooltip(item->Detail);
        auto& children = cell->GetChildren();
        if (!children.empty())
        {
            UIElement* icon = children[0].get();
            icon->AddClass("search-result-icon");
            icon->AddClass("search-result-grid-thumbnail");
            BindSearchResultIcon(*icon, item->Icon, std::max(32.0f, m_ResultIconSize));
        }
        if (children.size() >= 2)
        {
            auto* title = static_cast<Label*>(children[1].get());
            title->AddClass("search-result-grid-label");
            title->SetText(item->Label);
        }
    });
    m_ResultsGrid->SetOnItemActivated([this](GridId id) { OnItemActivated(id); });
    m_Panel->AddChild(std::move(grid));

    // ---- Bottom toolbar / status ----
    auto status = std::make_unique<Label>();
    m_StatusLabel = status.get();
    m_StatusLabel->AddClass("search-dialog-status-text");

    auto statusBar = std::make_unique<UIElement>();
    m_StatusBar = statusBar.get();
    statusBar->AddClass("search-dialog-status");
    statusBar->SetTooltip("Drag search panel");
    statusBar->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) { BeginPanelDrag(e); });
    statusBar->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) { ContinuePanelDrag(e); });
    statusBar->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) { EndPanelDrag(e); });

    auto resultSizeControls = std::make_unique<UIElement>();
    resultSizeControls->AddClass("search-dialog-result-size-controls");

    auto resultViewToggle = std::make_unique<Button>();
    m_ResultViewToggle = resultViewToggle.get();
    m_ResultViewToggle->AddClass("search-dialog-result-view-toggle");
    UpdateResultViewToggleButton();
    m_ResultViewToggle->SetText("");
    m_ResultViewToggle->SetFocusable(false);
    m_ResultViewToggle->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetGridMode(!m_GridMode); });
    resultSizeControls->AddChild(std::move(resultViewToggle));

    auto resultSizeSlider = std::make_unique<Slider>();
    m_ResultSizeSlider = resultSizeSlider.get();
    m_ResultSizeSlider->AddClass("search-dialog-result-size-slider");
    m_ResultSizeSlider->SetTooltip("Result thumbnail size");
    m_ResultSizeSlider->SetMin(kMinResultIconSize);
    m_ResultSizeSlider->SetMax(kMaxResultIconSize);
    m_ResultSizeSlider->SetStep(kResultIconStep);
    m_ResultSizeSlider->SetShowValueBubble(false);
    m_ResultSizeSlider->SetValueWithoutNotify(m_ResultIconSize);
    m_ResultSizeSlider->SetOnValueChanging([this](const float& value) { SetResultIconSize(value); });
    m_ResultSizeSlider->SetOnValueChanged([this](const float& value) { SetResultIconSize(value); });
    resultSizeControls->AddChild(std::move(resultSizeSlider));

    statusBar->AddChild(std::move(resultSizeControls));
    statusBar->AddChild(std::move(status));
    m_Panel->AddChild(std::move(statusBar));

    auto resizeHandle = std::make_unique<UIElement>();
    m_ResizeHandle = resizeHandle.get();
    m_ResizeHandle->AddClass("search-dialog-resize-handle");
    m_ResizeHandle->SetTooltip("Resize search panel");
    m_ResizeHandle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (!m_ManipulationEnabled || e.Button != 0)
            return;
        m_ResizingPanel = true;
        m_DraggingPanel = false;
        m_Centered = false;
        m_HorizontalAnchor = SearchDialogHorizontalAnchor::LeadingLeft;
        m_AnchorHeight = 0.0f;
        m_GestureStartX = e.X;
        m_GestureStartY = e.Y;
        m_GesturePanelX = m_Panel->GetLayoutX();
        m_GesturePanelY = m_Panel->GetLayoutY();
        m_GesturePanelWidth = m_Panel->GetLayoutWidth();
        m_GestureListHeight = std::max(m_ResultRowHeight, m_Panel->GetLayoutHeight() - GetChromeHeight());
        m_AnchorX = m_GesturePanelX;
        m_AnchorY = m_GesturePanelY;
        e.Capture(m_ResizeHandle);
        e.Stop();
    });
    m_ResizeHandle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (!m_ResizingPanel)
            return;
        if (!e.ButtonDown)
        {
            m_ResizingPanel = false;
            e.Stop();
            return;
        }
        m_PanelWidth = std::max(kMinPanelWidth, m_GesturePanelWidth + (e.X - m_GestureStartX));
        m_MaxListHeight = std::max(m_ResultRowHeight, m_GestureListHeight + (e.Y - m_GestureStartY));
        UpdatePanelHeight();
        e.Stop();
    });
    m_ResizeHandle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (!m_ResizingPanel)
            return;
        m_ResizingPanel = false;
        e.Stop();
    });
    m_Panel->AddChild(std::move(resizeHandle));

    UIElement::AddChild(std::move(panel));
}

void SearchDialog::ClearResults()
{
    m_Results.clear();
    m_ResultsProvider->SetSource(&m_Results);
    m_ResultsList->RefreshFromProvider();
    m_ResultsGridProvider->SetSource(&m_Results);
    m_ResultsGrid->RefreshFromProvider();
    UpdatePanelHeight();
}

void SearchDialog::FocusSearchField()
{
    // Focus is safe during dispatch: SetFocusById prevents the pointer release
    // from overriding it. Deferring this would let a closed dialog reclaim it.
    if (m_Open)
        if (auto* owner = GetOwnerManager())
            owner->SetFocusById(m_FieldId);
}

void SearchDialog::Show()
{
    if (m_Open)
        return;

    m_Open = true;
    m_UserInteracting = false;
    AddClass("open");
    FocusSearchField();

    // Reopen in the previous working state. Position and dimensions already
    // live on the dialog; retain the query as well and refresh its results so
    // runtime-backed entries are current.
    const std::string query = m_SearchField ? m_SearchField->GetValue() : std::string{};
    if (m_SearchBar)
        m_SearchBar->RefreshVisualState();
    m_InitialSelectionPending = m_InitialSelectionId != 0;
    OnQueryChanged(query);

    MarkDirty(StyleDirty | LayoutDirty | VisualDirty);

    ApplyClampedPanelPosition();
    PostSafeAction([this]() {
        if (m_Open)
            ApplyClampedPanelPosition();
    });
}

void SearchDialog::Close()
{
    if (!m_Open)
        return;

    m_Open = false;
    RemoveClass("open");

    // A retained, hidden field must not consume the next key. Leave an
    // intentional transfer outside the dialog alone, and mark an explicit
    // focus change so result-row mouse release cannot refocus the hidden list.
    if (UI::FocusIsInside(*this))
        GetOwnerManager()->SetFocusById({});

    // Cancel any in-flight search
    if (m_Provider)
        m_Provider->CancelSearch();

    MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
}

void SearchDialog::OnEvent(UIEvent& e)
{
    if (!m_Open)
        return;

    if (e.Id == kEventMouseDown)
    {
        // Interactive children must retain their own pointer sequence. Capturing the
        // root for a query-field press steals focus/caret placement; doing it for a
        // result row prevents the list's down->up activation gesture.
        bool pressInInteractiveChild = false;
        for (UIElement* el = e.Target; el; el = el->GetParent())
        {
            if (el == m_ResultsList || el == m_ResultsGrid || el == m_SearchField ||
                el == m_SearchFilter || el == m_ClearButton || el == m_ResultViewToggle ||
                el == m_ResultSizeSlider)
            {
                pressInInteractiveChild = true;
                break;
            }
        }
        if (!pressInInteractiveChild)
            CaptureRootForCurrentPress(*this, e);
        m_UserInteracting = true;
    }
    else if (e.Id == kEventMouseUp)
        m_UserInteracting = false;

    if (e.Id == kEventKeyDown)
    {
        if (e.Key == Input::kKeyCode_Escape)
        {
            Close();
            if (m_OnCancel)
                m_OnCancel();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Down)
        {
            MoveHighlight(1);
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Up)
        {
            MoveHighlight(-1);
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            SelectHighlighted();
            e.Stop();
            return;
        }
    }
}

void SearchDialog::OnQueryChanged(const std::string& query)
{
    const bool queryWasShortened = query.size() < m_LastQuery.size();
    m_LastQuery = query;

    if (!m_Provider)
    {
        m_SearchPending = false;
        ClearResults();
        m_StatusLabel->SetText("");
        return;
    }

    // Cancel previous search and bump generation
    m_Provider->CancelSearch();
    uint64_t generation = ++m_SearchGeneration;
    m_SearchPending = true;

    // A shorter query normally broadens the result set. Do not leave the
    // narrower, now-stale rows on screen while the worker supplies that set,
    // or Backspace appears to do nothing until the async search lands.
    if (queryWasShortened)
        ClearResults();

    m_StatusLabel->SetText(query.empty() ? "" : "Searching...");

    // Create a UI-thread-safe sink that posts results back via the dispatcher.
    // Captures generation to discard stale results from previous queries.
    auto* owner = GetOwnerManager();
    uint64_t instanceId = GetInstanceId();

    auto firstBatch = std::make_shared<bool>(true);
    auto applyBatch = [this, generation, firstBatch](std::vector<SearchResultItem> batch, bool isComplete) {
        if (generation != m_SearchGeneration)
            return;
        if (*firstBatch)
        {
            ClearResults();
            m_SearchPending = false;
            *firstBatch = false;
        }
        AppendResults(std::move(batch));
        if (isComplete && m_Provider)
            m_StatusLabel->SetText(m_Provider->FormatResultSummary(m_Results));
    };

    ISearchProvider::ResultSink sink;
    if (owner)
    {
        // Shared, so a provider thread that delivers after the owner is gone posts into a closed
        // dispatcher, which refuses it. `owner` is dereferenced only by work that dispatcher
        // accepted, which runs from the owner's own drain.
        std::shared_ptr<UI::UiDispatcher> dispatcher = UIManagerSharedDispatcher(*owner);
        sink = [applyBatch, dispatcher, instanceId, owner](
                   std::vector<SearchResultItem> batch, bool isComplete) mutable {
            dispatcher->Post([applyBatch, batch = std::move(batch), isComplete,
                              instanceId, owner]() mutable {
                if (!owner->FindElementByInstanceId(instanceId))
                    return;
                applyBatch(std::move(batch), isComplete);
            });
        };
    }
    else
    {
        // No UIManager -- direct (synchronous) path for headless/testing
        sink = [applyBatch](std::vector<SearchResultItem> batch, bool isComplete) mutable {
            applyBatch(std::move(batch), isComplete);
        };
    }

    m_Provider->BeginSearch(m_QueryPrefix + query, std::move(sink));
}

void SearchDialog::AppendResults(std::vector<SearchResultItem> batch)
{
    if (batch.empty())
        return;

    bool wasEmpty = m_Results.empty();
    int count = static_cast<int>(batch.size());
    m_Results.insert(m_Results.end(),
                     std::make_move_iterator(batch.begin()),
                     std::make_move_iterator(batch.end()));

    m_ResultsProvider->NotifyAppended(count);
    m_ResultsList->NotifyItemsAppended(count);
    m_ResultsGridProvider->NotifyStructureChanged();
    m_ResultsGrid->RefreshFromProvider();
    UpdatePanelHeight();

    // Auto-highlight first result
    if (wasEmpty && !m_Results.empty())
    {
        m_ResultsList->SetSelectedIndex(0, /*scrollIntoView=*/true);
        m_ResultsGrid->SetSelectedIndex(0, /*scrollIntoView=*/true);
    }

    if (!m_InitialSelectionPending)
        return;
    const int firstNew = static_cast<int>(m_Results.size()) - count;
    for (int i = firstNew; i < static_cast<int>(m_Results.size()); ++i)
    {
        if (m_Results[static_cast<size_t>(i)].Id != m_InitialSelectionId)
            continue;
        m_InitialSelectionPending = false;
        m_ResultsList->SetSelectedIndex(i, /*scrollIntoView=*/true);
        m_ResultsGrid->SetSelectedIndex(i, /*scrollIntoView=*/true);
        return;
    }
}

void SearchDialog::MoveHighlight(int delta)
{
    if (m_SearchPending)
        return;
    int count = static_cast<int>(m_Results.size());
    if (count == 0)
        return;

    const int selectedIndex = m_GridMode ? m_ResultsGrid->GetSelectedIndex()
                                         : m_ResultsList->GetSelectedIndex();
    // In grid mode, vertical navigation advances by one visual row instead of
    // walking through adjacent cells as list navigation does.
    if (m_GridMode && (delta == -1 || delta == 1))
        delta *= std::max(1, m_ResultsGrid->GetColumnCount());

    int index = std::clamp(selectedIndex + delta, 0, count - 1);
    if (m_GridMode)
        m_ResultsGrid->SetSelectedIndex(index, /*scrollIntoView=*/true);
    else
        m_ResultsList->SetSelectedIndex(index, /*scrollIntoView=*/true);
}

void SearchDialog::SelectHighlighted()
{
    if (m_SearchPending)
        return;
    int index = m_GridMode ? m_ResultsGrid->GetSelectedIndex()
                           : m_ResultsList->GetSelectedIndex();
    if (index >= 0 && index < static_cast<int>(m_Results.size()))
    {
        const auto& item = m_Results[index];
        if (!m_ShouldCloseOnResult || m_ShouldCloseOnResult())
            Close();
        if (m_OnResult)
            m_OnResult(item);
    }
}

void SearchDialog::OnItemActivated(uint64_t id)
{
    if (m_SearchPending)
        return;
    for (const auto& item : m_Results)
    {
        if (item.Id == id)
        {
            if (!m_ShouldCloseOnResult || m_ShouldCloseOnResult())
                Close();
            if (m_OnResult)
                m_OnResult(item);
            return;
        }
    }
}

} // namespace GameEngine
