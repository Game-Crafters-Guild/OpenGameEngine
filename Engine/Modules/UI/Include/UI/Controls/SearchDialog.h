#pragma once

#include <any>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "UI/UIElement.h"
#include "UI/Controls/ChangeTrackingProvider.h"
#include "UI/Interaction/DismissablePopup.h"

namespace GameEngine
{

class TextField;
class ListView;
class GridView;
class Label;
class Button;
class Slider;
class Dropdown;
class SearchFieldWithFilter;

using SearchItemId = uint64_t;

/// One optional filter in a SearchDialog's query-field chevron menu.
/// QueryPrefix is prepended only for the provider; the user's visible query
/// remains unchanged.
struct SearchDialogFilterOption
{
    std::string Value;
    std::string Label;
    std::string QueryPrefix;
};

/// Icon descriptor for a search result row. Supports CSS class icons
/// (for asset-type differentiation) and/or texture handles (for thumbnails).
struct SearchIcon
{
    std::string CssClass;          // CSS class applied to the icon element
    std::string ImagePath;         // Background image: file path or "engine:<name>" for GPU textures
    uint64_t    TextureHandle = 0; // Raw texture handle for thumbnail (0 = none)

    bool HasImage() const { return !ImagePath.empty(); }
    bool HasTexture() const { return TextureHandle != 0; }
    bool HasCssClass() const { return !CssClass.empty(); }
    bool HasAny() const { return HasTexture() || HasCssClass() || HasImage(); }

    static SearchIcon FromClass(const std::string& cssClass) { return {cssClass, {}, 0}; }
    static SearchIcon FromTexture(uint64_t handle) { return {{}, {}, handle}; }
    static SearchIcon None() { return {}; }
};

/// A single search result. The provider populates display fields (Label, Detail, Icon)
/// and stores actionable data in UserData so result callbacks can act on selection
/// without parsing display text.
struct SearchResultItem
{
    SearchItemId Id = 0;
    std::string  Label;    // Primary display text (e.g., asset name)
    std::string  Detail;   // Secondary display text (e.g., path, category)
    std::string  TypeKey;  // Optional grouping key
    SearchIcon   Icon;     // Optional icon (CSS class and/or texture)
    std::any     UserData; // Opaque payload (e.g., GUID, command action)
};

/// Pluggable data source for SearchDialog. Supports both synchronous and
/// asynchronous result delivery via a streaming callback (ResultSink).
///
/// Simple providers call `sink(results, true)` synchronously from BeginSearch.
/// Async providers post batches from background threads; the SearchDialog
/// marshals them to the UI thread via UiDispatcher.
struct ISearchProvider
{
    virtual ~ISearchProvider() = default;

    /// Callback for delivering results. May be called multiple times (additive
    /// batches). Set isComplete=true on the final batch.
    using ResultSink = std::function<void(std::vector<SearchResultItem> batch, bool isComplete)>;

    /// Start a search. CancelSearch() is always called before a new BeginSearch.
    virtual void BeginSearch(const std::string& query, ResultSink sink) = 0;

    /// Cancel any in-flight search. Called before each new query and on dialog close.
    virtual void CancelSearch() = 0;

    /// Placeholder text shown in the search field when empty.
    virtual std::string GetPlaceholderText() const { return "Search..."; }

    /// Completed-search footer for the displayed rows. Providers with action rows
    /// can summarize their domain results without counting the actions.
    virtual std::string FormatResultSummary(std::span<const SearchResultItem> results) const
    {
        return results.empty() ? "No results found"
                               : std::to_string(results.size()) + (results.size() == 1 ? " result" : " results");
    }
};

/// Horizontal placement of the panel relative to `SetAnchorPosition` x.
enum class SearchDialogHorizontalAnchor : std::uint8_t
{
    /// `x` is the panel's **left** edge (default).
    LeadingLeft,
    /// `x` is the panel's **right** edge; the panel extends left (e.g. Inspector fields on the window edge).
    TrailingRight,
};

/// Modal search dialog with text input, virtualized result list, and
/// async provider support. Use OverlayLayer::Modal to escape parent
/// stacking contexts.
///
/// Usage:
///   auto dialog = std::make_unique<SearchDialog>();
///   dialog->SetProvider(myProvider);
///   dialog->SetOnResult([](const SearchResultItem& item) { ... });
///   root->AddChild(std::move(dialog));
///   dialog->Show();
class SearchDialog : public UIElement, public DismissablePopup
{
public:
    SearchDialog();
    ~SearchDialog() override;

    // Maintains UIManager's dismissable-popup registry, which drives the
    // pointer gate and the outside-press / Escape dismissal.
    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }

    // DismissablePopup. The dialog is its own visible surface, so the default
    // popup root (this) is correct. Dismissal is a cancel, matching the
    // dialog's own Escape handling rather than a silent Close().
    bool IsPopupOpen() const override { return m_Open; }
    void DismissPopup() override
    {
        Close();
        if (m_OnCancel)
            m_OnCancel();
    }

    /// Set the data source (non-owning). Must be set before Show().
    void SetProvider(ISearchProvider* provider);

    /// Fired when the user selects an item (Enter, click). Receives the full
    /// SearchResultItem including UserData for the consumer to act on.
    using ResultCallback = std::function<void(const SearchResultItem&)>;
    void SetOnResult(ResultCallback cb) { m_OnResult = std::move(cb); }

    /// Controls whether activating a result dismisses the dialog. The
    /// predicate is evaluated for every activation so callers can back it
    /// with a live preference.
    using ShouldCloseOnResultCallback = std::function<bool()>;
    void SetShouldCloseOnResult(ShouldCloseOnResultCallback cb) { m_ShouldCloseOnResult = std::move(cb); }

    /// Fired when the user dismisses without selecting (Escape, click backdrop).
    using CancelCallback = std::function<void()>;
    void SetOnCancel(CancelCallback cb) { m_OnCancel = std::move(cb); }

    /// Position the panel at (x, y) in root-absolute coordinates.
    /// For `LeadingLeft`, `x` is the panel's left edge; for `TrailingRight`, `x` is the panel's right edge.
    /// When `anchorHeight > 0`, (x, y) is treated as the bottom-left of an anchor element of that
    /// height: the panel opens below it, or flips above it (bottom at the element's top) when there
    /// is no room below. `anchorHeight == 0` keeps the legacy behavior (y is the panel's top).
    void SetAnchorPosition(float x, float y,
                           SearchDialogHorizontalAnchor horizontalAnchor = SearchDialogHorizontalAnchor::LeadingLeft,
                           float anchorHeight = 0.0f);

    /// Highlight and scroll to the result with this id when Show() delivers
    /// results, instead of the first row. 0 disables it. Typing a new query
    /// returns the highlight to the first row.
    void SetInitialSelection(SearchItemId id) { m_InitialSelectionId = id; }

    /// Open the dialog, clear query, focus the text field.
    void Show();

    /// Close the dialog and cancel any in-flight search.
    void Close();

    bool IsOpen() const { return m_Open; }

    /// When true, the panel always uses its maximum height regardless of result count.
    void SetFixedHeight(bool fixed) { m_FixedHeight = fixed; }

    /// Override the picker-sized panel width. Existing callers keep the 360px default.
    void SetPanelWidth(float width);

    /// Add a compact filter chevron to the query field. The first option is
    /// selected by default; pass an empty vector to hide the filter.
    void SetFilterOptions(std::vector<SearchDialogFilterOption> options);

    /// Override the scrollable result area height. Existing callers keep the 300px default.
    void SetMaxListHeight(float height);

    /// Center the panel on both viewport axes. Passing false restores
    /// anchor-based placement configured by SetAnchorPosition().
    /// The panel is clamped to the viewport so its search and status bars stay visible.
    void SetCentered(bool centered);

    /// Enable the drag bars and bottom-right resize grip.
    void SetManipulationEnabled(bool enabled);

    void OnEvent(UIEvent& e) override;

private:
    void BuildStructure();
    void FocusSearchField();
    void ClearResults();
    void UpdatePanelHeight();
    float GetChromeHeight() const;
    void BeginPanelDrag(UIEvent& event);
    void ContinuePanelDrag(UIEvent& event);
    void EndPanelDrag(UIEvent& event);
    /// Keeps the panel within the root viewport (position and width when narrow).
    void ApplyClampedPanelPosition();
    void OnQueryChanged(const std::string& query);
    void AppendResults(std::vector<SearchResultItem> batch);
    void SelectHighlighted();
    void MoveHighlight(int delta);
    void OnItemActivated(uint64_t id);
    void SetResultIconSize(float size);
    void SetGridMode(bool enabled);
    void UpdateResultViewToggleButton();

    ISearchProvider* m_Provider = nullptr;
    ResultCallback   m_OnResult;
    ShouldCloseOnResultCallback m_ShouldCloseOnResult;
    CancelCallback   m_OnCancel;
    bool             m_Open = false;

    // Child elements (raw pointers; owned by UIElement children vector)
    UIElement* m_Backdrop = nullptr;
    UIElement* m_Panel = nullptr;
    SearchFieldWithFilter* m_SearchBar = nullptr;
    UIElement* m_SearchIcon = nullptr;
    TextField* m_SearchField = nullptr;
    Dropdown*  m_SearchFilter = nullptr;
    Button*    m_ClearButton = nullptr;
    ListView*  m_ResultsList = nullptr;
    GridView*  m_ResultsGrid = nullptr;
    Label*     m_StatusLabel = nullptr;
    UIElement* m_StatusBar = nullptr;
    Label*     m_DragHandle = nullptr;
    Button*    m_ResultViewToggle = nullptr;
    Slider*    m_ResultSizeSlider = nullptr;
    UIElement* m_ResizeHandle = nullptr;

    // Async generation counter: incremented on each new query.
    // Stale batches (from a previous generation) are silently discarded.
    uint64_t m_SearchGeneration = 0;
    bool m_SearchPending = false;
    std::string m_LastQuery;
    std::string m_QueryPrefix;
    std::vector<SearchDialogFilterOption> m_FilterOptions;

    // Cached results for the current query
    std::vector<SearchResultItem> m_Results;

    SearchItemId m_InitialSelectionId = 0;
    bool m_InitialSelectionPending = false;

    // Internal list data provider bridging m_Results → IListDataProvider
    class ResultsDataProvider;
    class ResultsGridDataProvider;
    std::unique_ptr<ResultsDataProvider> m_ResultsProvider;
    std::unique_ptr<ResultsGridDataProvider> m_ResultsGridProvider;

    inline static std::atomic<int> s_NextDialogId{0};
    std::string m_FieldId; // stable id for focus management

    float m_AnchorX = 0.0f;
    float m_AnchorY = 0.0f;
    float m_AnchorHeight = 0.0f;  // > 0 enables below/above flipping around the anchor element
    // Below-vs-above is decided on the first placement (using max panel height) and
    // latched for the panel's lifetime, so filtering the list never flips sides.
    bool m_PlaceAbove = false;
    bool m_AnchorDecisionPending = true;
    SearchDialogHorizontalAnchor m_HorizontalAnchor = SearchDialogHorizontalAnchor::LeadingLeft;
    bool m_FixedHeight = false;
    bool m_UserInteracting = false;
    float m_PanelWidth = 360.0f;
    float m_MaxListHeight = 300.0f;
    float m_ResultIconSize = 24.0f;
    float m_ResultRowHeight = 36.0f;
    bool m_GridMode = false;
    bool m_Centered = false;
    bool m_ManipulationEnabled = false;
    bool m_DraggingPanel = false;
    bool m_ResizingPanel = false;
    float m_GestureStartX = 0.0f;
    float m_GestureStartY = 0.0f;
    float m_GesturePanelX = 0.0f;
    float m_GesturePanelY = 0.0f;
    float m_GesturePanelWidth = 0.0f;
    float m_GestureListHeight = 0.0f;
};

} // namespace GameEngine
