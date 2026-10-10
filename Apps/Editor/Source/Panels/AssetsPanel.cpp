#include "Panels/AssetsPanel.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "Panels/InspectorPanel.h"
#include "Panels/AddTagModal.h"

#include "Core/Engine.h"
#include "Input/KeyCodes.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/SettingsPanel.h"
#include "UI/Controls/SplitView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Controls/TreeView.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/TableView.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ItemSizeSlider.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/PanelSearchBar.h"
#include "UI/StyleProperties.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "Automation/UiReplayCommandIds.h"
#include "Assets/AssetsBrowserController.h"
#include "Assets/AssetsDataProviders.h"
#include "Thumbnails/FolderBakeStatus.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UI/SmartFolder/SmartFolderManager.h"
#include "UI/SmartFolder/SmartFolderController.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VersionControl/EditorVersionControlService.h"
#include "Core/Engine.h"
#include "EditorContext.h"
#include "MissingAssetTracker.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/AxisLayoutSerializer.h"
#include "EditorContextMenu/EditorContextMenu.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine {

namespace {
constexpr const char* kListColumnWidthsKey = "ui.assets.list.columnWidths";

// Per-column visibility preference keys for the Assets list view.
// Name column is always visible; other columns can be toggled.
constexpr const char* kListColPref_Type       = "ui.assets.list.showType";
constexpr const char* kListColPref_Size       = "ui.assets.list.showSize";
constexpr const char* kListColPref_Dimensions = "ui.assets.list.showDimensions";
constexpr const char* kListColPref_Modified   = "ui.assets.list.showModified";
constexpr const char* kListColPref_Git        = "ui.assets.list.showGit";
constexpr const char* kListColPref_Tag        = "ui.assets.list.showTag";
constexpr const char* kListColPref_Referenced = "ui.assets.list.showReferenced";
constexpr const char* kListColPref_Custom     = "ui.assets.list.showCustom";
constexpr const char* kListColPref_Creator    = "ui.assets.list.showCreator";
bool IsImageExtension(const std::filesystem::path& path)
{
    if (!path.has_extension())
        return false;
    std::string ext = path.extension().string();
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
        ext == ".gif" || ext == ".hdr" || ext == ".ktx" || ext == ".ktx2" || ext == ".dds";
}

/// A–Z / 0–9 only; Finder-style match on first filename character (ASCII).
int TypeAheadKeyToMatchChar(int key)
{
    if (key >= Input::kKeyCode_A && key <= Input::kKeyCode_Z)
        return std::tolower(key);
    if (key >= Input::kKeyCode_0 && key <= Input::kKeyCode_9)
        return key;
    return -1;
}

bool LabelFirstCharMatches(const char* label, char matchLowerOrDigit)
{
    if (!label || label[0] == '\0')
        return false;
    const unsigned char first = static_cast<unsigned char>(label[0]);
    if (first >= 128u)
        return false;
    return static_cast<char>(std::tolower(first)) == matchLowerOrDigit;
}

/// Same key within this window cycles to the next matching item; after pause, same key starts from the first match again.
constexpr std::chrono::milliseconds kAssetsTypeAheadRepeatWindow(750);

// Narrows the tree column vs the default 1:3 flex split (space before the splitter).
constexpr float kAssetsPanelLeftPaneMarginRightPx = 10.0f;

const std::vector<Dropdown::Option>& AssetsSearchFilterOptions()
{
    static const std::vector<Dropdown::Option> options = {
        {"all", "All fields"},
        {"name", "Name"},
        {"type", "Type"},
        {"extension", "File extension"},
        {"path", "Path"},
        {"tag", "Tag"},
    };
    return options;
}

AssetsSearchField AssetsSearchFieldFromValue(const std::string& value)
{
    if (value == "name") return AssetsSearchField::Name;
    if (value == "type") return AssetsSearchField::Type;
    if (value == "extension") return AssetsSearchField::Extension;
    if (value == "path") return AssetsSearchField::Path;
    if (value == "tag") return AssetsSearchField::Tag;
    return AssetsSearchField::All;
}

int AssetsSearchFieldIndex(const std::string& value)
{
    const auto& options = AssetsSearchFilterOptions();
    const auto it = std::find_if(options.begin(), options.end(), [&value](const Dropdown::Option& option) {
        return option.value == value;
    });
    return it == options.end() ? 0 : static_cast<int>(std::distance(options.begin(), it));
}

// Matches `.assets-view-toggle-toolbar` height in core.css (toolbar is absolutely positioned).
constexpr float kAssetsBottomToolbarHeightPx = 28.0f;

// Grid toolbar slider uses 0..1 with log mapping (GridView::SetIconSize domain) so small icon
// sizes get more of the track than large sizes.
constexpr float kGridToolbarIconMinPx = 32.0f;
constexpr float kGridToolbarIconMaxPx = 4096.0f;
// Grid icon sizes land on 32 + n * 16 px, whether a drag, the wheel or a stored preference sets them.
constexpr float kGridIconSizeStepPx = 16.0f;

float GridToolbarSliderTFromIconPx(float px)
{
    px = std::clamp(px, kGridToolbarIconMinPx, kGridToolbarIconMaxPx);
    const float ratio = kGridToolbarIconMaxPx / kGridToolbarIconMinPx;
    return std::log(px / kGridToolbarIconMinPx) / std::log(ratio);
}

float IconPxFromGridToolbarSliderT(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float ratio = kGridToolbarIconMaxPx / kGridToolbarIconMinPx;
    return kGridToolbarIconMinPx * std::pow(ratio, t);
}

void SetAssetsVirtualizedViewActive(UIElement* view, bool active)
{
    if (!view)
        return;

    view->Overrides()
        .Set(Style::Display, active ? DisplayMode::Flex : DisplayMode::None)
        .Set(Style::FlexGrow, active ? 1.0f : 0.0f)
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(0.0f));

    UI::Layout::SetElementInvisible(*view, !active);
    if (active)
        UI::Layout::ClearForcedHeight(*view);
    else
        UI::Layout::SetForcedHeight(*view, 0);
}
}

AssetsPanel::AssetsPanel()
    : DockPanel("Assets")
{
    // Load default view preference (list view = false, grid view = true)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("ui.assetsDefaultGridView", m_IsGridViewActive);
        m_ExtraBottomViewToolbarEnabled = true;
        prefs.TryGetBool("ui.assetsExtraBottomViewToolbar", m_ExtraBottomViewToolbarEnabled);
        m_SingleViewToggleIconEnabled = true;
        prefs.TryGetBool("ui.assetsSingleViewToggleIcon", m_SingleViewToggleIconEnabled);
        m_BottomToolbarZoomSliderVisible = true;
        prefs.TryGetBool("ui.assets.bottomToolbarShowZoomSlider", m_BottomToolbarZoomSliderVisible);
    }

    // Build: [pane(left: ScrollView(TreeView))][splitter][pane(right: GridView)] inside SplitView
    auto split = std::make_unique<SplitView>();
    split->AddClass("dock-split");
    split->AddClass("row");

    // Left pane with TreeView (TreeView has internal scroll). Folder column weight (~21.5%: between prior 25% and 18%).
    auto leftPane = std::make_unique<WeightedPane>(0.215f);
    m_LeftPane = leftPane.get();
    leftPane->AddClass("pane");
    leftPane->AddClass("assets-left-pane");
    leftPane->Overrides().Set(Style::MarginRight, StyleLength::Px(kAssetsPanelLeftPaneMarginRightPx));
    {
        auto tree = std::make_unique<TreeView>();
        m_TreeView = tree.get();
        m_TreeView->SetId("assets-tree");
        m_TreeView->AddClass("tree");
        m_TreeView->SetOnItemResizeGesture([this](float scrollY) {
            if (!m_OnTreeIconSizeWheelCommit)
                return;
            const float resized = EditorTreeIconSizeAfterResizeGesture(
                m_LastTreeIconSizePx, scrollY, kMaxEditorAssetsTreeIconSizePx);
            if (std::fabs(resized - m_LastTreeIconSizePx) > 0.1f)
                m_OnTreeIconSizeWheelCommit(resized);
        });
        leftPane->AddChild(std::move(tree));

        // Search bar placement and visibility are controlled via Settings.
        auto built = BuildPanelSearchBar(
            "assets-search-field",
            []() { return SettingsPanel::GetSearchBarsVisible(); },
            [this](const std::string& value)
            {
                if (m_ToolbarSearchField)
                    m_ToolbarSearchField->SetValueWithoutNotify(value);
                if (m_Controller)
                    m_Controller->SetSearchQuery(value);
            },
            [this](const std::string& value)
            {
                if (m_ToolbarSearchField)
                    m_ToolbarSearchField->SetValueWithoutNotify(value);
                if (m_Controller)
                    m_Controller->SetSearchQuery(value);
            },
            AssetsSearchFilterOptions(),
            [this](const std::string& value)
            {
                ApplySearchField(value, m_SearchFilter);
            });
        m_SearchBar = built.RootPtr;
        m_SearchField = built.FieldPtr;
        m_SearchFilter = built.FilterPtr;
        leftPane->AddChild(std::move(built.Root));

        SettingsPanel::RegisterSearchBar(m_SearchBar, built.IconPtr);
    }

    // Splitter between panes (needs a non-empty id for mouse capture to work)
    auto splitter = std::make_unique<Splitter>();
    splitter->SetId("assets-splitter"); // not "split:"-prefixed to avoid docking model updates
    splitter->AddClass("splitter");
    splitter->AddClass("row");

    // Right pane with GridView/ListView and toggle buttons
    auto rightPane = std::make_unique<WeightedPane>(0.785f);
    rightPane->AddClass("pane");
    rightPane->AddClass("assets-right-pane");
    {
        // Container for the views (GridView and ListView)
        auto viewsContainer = std::make_unique<UIElement>();
        viewsContainer->AddClass("assets-views-container");
        viewsContainer->SetId("assets-views-container");
        viewsContainer->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::Position, PositionType::Relative)
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f));

        // GridView
        auto grid = std::make_unique<GridView>();
        m_GridView = grid.get();
        m_GridView->SetId("assets-grid");
        m_GridView->AddClass("grid");
        m_GridView->SetIconSizeStep(kGridIconSizeStepPx);
        m_GridView->Overrides()
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f))
            .Set(Style::Display, DisplayMode::Flex);
        SetAssetsVirtualizedViewActive(m_GridView, m_IsGridViewActive);
        m_GridView->SetFocusable(true);
        m_GridView->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
                                         {
            if (Editor::MatchesCatalogShortcut("Assets", "Toggle Preview", e.Key, e.Mods))
            {
                m_AssetPreviewEnabled = !m_AssetPreviewEnabled;
                UpdateAssetPreview();
                e.Stop();
                return;
            }

            if (Editor::MatchesCatalogShortcut("Assets", "Rename", e.Key, e.Mods))
            {
                if (m_Controller)
                    m_Controller->BeginRenameOfSelection();
                e.Stop();
                return;
            }

            if (TryAssetsViewTypeAhead(e, true))
                return;

            if (e.Mods == 0 && e.Key == Input::kKeyCode_Enter)
            {
                if (m_GridView)
                    m_GridView->ActivateSelected();
                e.Stop();
                return;
            }
            // Arrow navigation is owned by GridView::OnEvent; handling it here
            // too would move the selection twice per keypress.
        });
        viewsContainer->AddChild(std::move(grid));

        // List view: a reusable TableView (header + virtualized body + column resize/sort).
        // Columns and the keyed cell binders are configured in ConfigureListTable() after the
        // controller (and its row provider) exist. m_ListView aliases the table's body so the
        // existing keyboard / type-ahead / selection code keeps working against the same ListView.
        auto table = std::make_unique<TableView>();
        m_Table = table.get();
        m_Table->Overrides()
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f))
            .Set(Style::Display, DisplayMode::Flex);
        SetAssetsVirtualizedViewActive(m_Table, !m_IsGridViewActive);
        m_ListView = &m_Table->Body();
        m_ListView->SetId("assets-list");
        // Keep the legacy "assets-list-header" id on the (now TableView-owned) header so the
        // UIReplay fixtures that probe the list header during horizontal scroll still resolve it.
        if (UIElement* header = m_ListView->GetHeader())
            header->SetId("assets-list-header");
        m_ListView->SetFocusable(true);

        m_ListView->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
                                         {
            if (Editor::MatchesCatalogShortcut("Assets", "Toggle Preview", e.Key, e.Mods))
            {
                m_AssetPreviewEnabled = !m_AssetPreviewEnabled;
                UpdateAssetPreview();
                e.Stop();
                return;
            }

            if (Editor::MatchesCatalogShortcut("Assets", "Rename", e.Key, e.Mods))
            {
                if (m_Controller)
                    m_Controller->BeginRenameOfSelection();
                e.Stop();
                return;
            }

            if (TryAssetsViewTypeAhead(e, false))
                return;

            if (e.Mods == 0 && e.Key == Input::kKeyCode_Enter)
            {
                if (m_ListView)
                    m_ListView->ActivateSelected();
                e.Stop();
                return;
            }

            const bool shift = (e.Mods & 0x0001) != 0; // Input::kModShift
            if ((e.Mods == 0 || (shift && (e.Mods & ~0x0001) == 0)) &&
                (e.Key == Input::kKeyCode_Up || e.Key == Input::kKeyCode_Down))
            {
                const int count = m_ListView ? m_ListView->GetItemCount() : 0;
                if (count <= 0 || !m_ListView)
                    return;
                int index = m_ListView->GetSelectedIndex();
                if (index < 0)
                    index = 0;
                index += (e.Key == Input::kKeyCode_Up) ? -1 : 1;
                index = std::clamp(index, 0, count - 1);
                m_ListView->SetSelectedIndexKeyboard(index, /*extendRange*/ shift, /*scrollIntoView*/ true);
                e.Stop();
            }
        });
        viewsContainer->AddChild(std::move(table));

        m_AssetsViewsContainer = viewsContainer.get();
        rightPane->AddChild(std::move(viewsContainer));

        // Toggle buttons container at bottom right
        auto buttonsContainer = std::make_unique<UIElement>();
        m_MainViewToggleContainer = buttonsContainer.get();
        buttonsContainer->AddClass("assets-view-toggle-container");
        buttonsContainer->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PositionBottom, StyleLength::Px(20.0f))
            .Set(Style::PositionRight, StyleLength::Px(24.0f))
            .Set(Style::Display, m_ExtraBottomViewToolbarEnabled ? DisplayMode::None : DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::Gap, StyleLength::Px(4.0f))
            .Set(Style::ZIndex, 100);

        // Grid view button
        auto gridButton = std::make_unique<Button>();
        m_GridViewButton = gridButton.get();
        m_GridViewButton->AddClass("small");
        m_GridViewButton->AddClass("secondary");
        m_GridViewButton->AddClass("icon-button");
        m_GridViewButton->AddClass("grid-view-icon");
        m_GridViewButton->SetTooltip("Grid View");
        if (m_IsGridViewActive)
            m_GridViewButton->AddClass("active");
        m_GridViewButton->SetText("");
        m_GridViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            SwitchToGridView();
        });
        buttonsContainer->AddChild(std::move(gridButton));

        // List view button
        auto listButton = std::make_unique<Button>();
        m_ListViewButton = listButton.get();
        m_ListViewButton->AddClass("small");
        m_ListViewButton->AddClass("secondary");
        m_ListViewButton->AddClass("icon-button");
        m_ListViewButton->AddClass("list-view-icon");
        m_ListViewButton->SetTooltip("List View");
        if (!m_IsGridViewActive)
            m_ListViewButton->AddClass("active");
        m_ListViewButton->SetText("");
        m_ListViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            SwitchToListView();
        });
        buttonsContainer->AddChild(std::move(listButton));

        auto singleToggleButton = std::make_unique<Button>();
        m_SingleViewToggleButton = singleToggleButton.get();
        m_SingleViewToggleButton->AddClass("small");
        m_SingleViewToggleButton->AddClass("secondary");
        m_SingleViewToggleButton->AddClass("icon-button");
        m_SingleViewToggleButton->SetText("");
        m_SingleViewToggleButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            if (m_IsGridViewActive)
                SwitchToListView();
            else
                SwitchToGridView();
        });
        buttonsContainer->AddChild(std::move(singleToggleButton));

        rightPane->AddChild(std::move(buttonsContainer));

        // Optional extra bottom toolbar with duplicate view toggle icons.
        auto bottomToolbar = std::make_unique<UIElement>();
        m_ExtraViewToolbar = bottomToolbar.get();
        m_ExtraViewToolbar->AddClass("assets-view-toggle-toolbar");
        m_ExtraViewToolbar->Overrides()
            .Set(Style::Display, m_ExtraBottomViewToolbarEnabled ? DisplayMode::Flex : DisplayMode::None);

        auto bottomToolbarRight = std::make_unique<UIElement>();
        m_ExtraViewToolbarRight = bottomToolbarRight.get();
        bottomToolbarRight->AddClass("assets-view-toggle-toolbar-right");

        auto extraGridButton = std::make_unique<Button>();
        m_ExtraGridViewButton = extraGridButton.get();
        m_ExtraGridViewButton->AddClass("small");
        m_ExtraGridViewButton->AddClass("secondary");
        m_ExtraGridViewButton->AddClass("icon-button");
        m_ExtraGridViewButton->AddClass("grid-view-icon");
        m_ExtraGridViewButton->SetTooltip("Grid View");
        if (m_IsGridViewActive)
            m_ExtraGridViewButton->AddClass("active");
        m_ExtraGridViewButton->SetText("");
        m_ExtraGridViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            SwitchToGridView();
        });
        bottomToolbarRight->AddChild(std::move(extraGridButton));

        auto extraListButton = std::make_unique<Button>();
        m_ExtraListViewButton = extraListButton.get();
        m_ExtraListViewButton->AddClass("small");
        m_ExtraListViewButton->AddClass("secondary");
        m_ExtraListViewButton->AddClass("icon-button");
        m_ExtraListViewButton->AddClass("list-view-icon");
        m_ExtraListViewButton->SetTooltip("List View");
        if (!m_IsGridViewActive)
            m_ExtraListViewButton->AddClass("active");
        m_ExtraListViewButton->SetText("");
        m_ExtraListViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            SwitchToListView();
        });
        bottomToolbarRight->AddChild(std::move(extraListButton));

        auto extraSingleToggleButton = std::make_unique<Button>();
        m_ExtraSingleViewToggleButton = extraSingleToggleButton.get();
        m_ExtraSingleViewToggleButton->AddClass("small");
        m_ExtraSingleViewToggleButton->AddClass("secondary");
        m_ExtraSingleViewToggleButton->AddClass("icon-button");
        m_ExtraSingleViewToggleButton->SetText("");
        m_ExtraSingleViewToggleButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            if (m_IsGridViewActive)
                SwitchToListView();
            else
                SwitchToGridView();
        });
        bottomToolbarRight->AddChild(std::move(extraSingleToggleButton));

        auto zoomSlider = std::make_unique<EditorUI::ItemSizeSlider>();
        m_ToolbarGridZoomSlider = zoomSlider.get();
        m_ToolbarGridZoomSlider->AddClass("assets-toolbar-zoom-slider");
        zoomSlider->SetOnResizeGesture([this](float scrollY) { HandleAssetsToolbarZoomSliderScroll(scrollY); });
        m_ToolbarGridZoomSlider->SetOnValueChanging([this](const float& value) {
            if (m_ToolbarGridZoomSliderUpdating)
                return;
            if (m_IsGridViewActive)
                SetGridIconSize(IconPxFromGridToolbarSliderT(value));
            else if (m_Controller)
                m_Controller->SetListRowHeight(value);
        });
        m_ToolbarGridZoomSlider->SetOnValueChanged([this](const float& value) {
            if (m_ToolbarGridZoomSliderUpdating)
                return;
            if (m_IsGridViewActive)
            {
                SetGridIconSize(IconPxFromGridToolbarSliderT(value));
                if (m_OnGridIconSizeChanged)
                    m_OnGridIconSizeChanged(GetGridIconSize());
            }
            else if (m_Controller)
                m_Controller->SetListRowHeight(value);
        });
        bottomToolbarRight->AddChild(std::move(zoomSlider));

        m_ExtraViewToolbar->AddChild(std::move(bottomToolbarRight));

        auto bottomToolbarSpacer = std::make_unique<UIElement>();
        bottomToolbarSpacer->AddClass("assets-view-toggle-toolbar-spacer");
        m_ExtraViewToolbar->AddChild(std::move(bottomToolbarSpacer));

        auto bottomToolbarSearchHost = std::make_unique<UIElement>();
        m_ExtraViewToolbarSearchHost = bottomToolbarSearchHost.get();
        m_ExtraViewToolbarSearchHost->AddClass("assets-view-toggle-toolbar-search");
        {
            auto built = BuildPanelSearchBar(
                "assets-toolbar-search-field",
                []() { return SettingsPanel::GetSearchBarsVisible(); },
                [this](const std::string& value)
                {
                    if (m_SearchField)
                        m_SearchField->SetValueWithoutNotify(value);
                    if (m_Controller)
                        m_Controller->SetSearchQuery(value);
                },
                [this](const std::string& value)
                {
                    if (m_SearchField)
                        m_SearchField->SetValueWithoutNotify(value);
                    if (m_Controller)
                        m_Controller->SetSearchQuery(value);
                },
                AssetsSearchFilterOptions(),
                [this](const std::string& value)
                {
                    ApplySearchField(value, m_ToolbarSearchFilter);
                });
            m_ToolbarSearchBar = built.RootPtr;
            m_ToolbarSearchField = built.FieldPtr;
            m_ToolbarSearchFilter = built.FilterPtr;
            if (m_ToolbarSearchBar)
            {
                SettingsPanel::RegisterSearchBar(m_ToolbarSearchBar, built.IconPtr);
            }
            m_ExtraViewToolbarSearchHost->AddChild(std::move(built.Root));
        }
        m_ExtraViewToolbar->AddChild(std::move(bottomToolbarSearchHost));

        rightPane->AddChild(std::move(bottomToolbar));
        UpdateSingleViewToggleButtons();
        UpdateViewToggleButtonsPresentation();
        UpdateSearchBarPlacement();
        ApplyAssetsViewsBottomInsetForToolbar();
        ApplyBottomToolbarZoomSliderVisibility();
    }

    // Assemble into split view
    split->AddChild(std::move(leftPane));
    split->AddChild(std::move(splitter));
    split->AddChild(std::move(rightPane));

    // Attach to this DockPanel
    this->AddChild(std::move(split));

    // Add Tag modal (overlay); wire to AssetRegistry for tag assignment
    {
        auto addTagModal = std::make_unique<AddTagModal>();
        m_AddTagModal = addTagModal.get();
        addTagModal->SetOnAssignTag([this](const std::vector<std::filesystem::path>& paths, const std::string& tagName) {
            if (paths.empty() || tagName.empty())
                return;
            // Defer registry writes so the modal can close and the UI stays responsive.
            std::vector<std::filesystem::path> pathsCopy = paths;
            std::string tagNameCopy = tagName;
            this->PostAction([this, pathsCopy, tagNameCopy]() {
                auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                const std::string key = "tags";
                for (const auto& p : pathsCopy)
                {
                    std::string existing;
                    reg.TryGetMetaValue(p, key, existing);
                    std::vector<std::string> parts;
                    for (size_t i = 0; i < existing.size(); )
                    {
                        size_t j = existing.find(',', i);
                        if (j == std::string::npos)
                            j = existing.size();
                        std::string part = existing.substr(i, j - i);
                        {
                            const size_t start = part.find_first_not_of(" \t");
                            if (start == std::string::npos) { part.clear(); }
                            else {
                                const size_t end = part.find_last_not_of(" \t");
                                part = part.substr(start, end == std::string::npos ? part.size() - start : end - start + 1);
                            }
                        }
                        if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
                            parts.push_back(part);
                        i = j + (j < existing.size() ? 1 : 0);
                    }
                    if (std::find(parts.begin(), parts.end(), tagNameCopy) == parts.end())
                        parts.push_back(tagNameCopy);
                    std::string combined;
                    for (size_t i = 0; i < parts.size(); ++i)
                        combined += (i ? "," : "") + parts[i];
                    reg.SetMetaValue(p, key, combined);
                }
            });
        });
        if (m_OnOpenSettingsToTags)
            addTagModal->SetOnOpenSettings(m_OnOpenSettingsToTags);
        this->AddChild(std::move(addTagModal));
    }

    // Create per-panel controller and wire views
    m_Controller = std::make_unique<AssetsBrowserController>();
    m_Controller->Initialize(m_TreeView, m_GridView, m_ListView);
    m_Controller->SetOnAddTag([this](const std::filesystem::path&, const std::vector<std::filesystem::path>& paths) {
        if (m_AddTagModal)
            m_AddTagModal->Show(paths);
    });
    // Tags submenu: assign directly from context menu (no modal)
    m_Controller->SetOnAssignTag([this](const std::vector<std::filesystem::path>& paths, const std::string& tagName) {
        if (paths.empty() || tagName.empty())
            return;
        std::vector<std::filesystem::path> pathsCopy = paths;
        std::string tagNameCopy = tagName;
        this->PostAction([this, pathsCopy, tagNameCopy]() {
            auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
            const std::string key = "tags";
            for (const auto& p : pathsCopy)
            {
                std::string existing;
                reg.TryGetMetaValue(p, key, existing);
                std::vector<std::string> parts;
                for (size_t i = 0; i < existing.size(); )
                {
                    size_t j = existing.find(',', i);
                    if (j == std::string::npos)
                        j = existing.size();
                    std::string part = existing.substr(i, j - i);
                    {
                        const size_t start = part.find_first_not_of(" \t");
                        if (start == std::string::npos) { part.clear(); }
                        else {
                            const size_t end = part.find_last_not_of(" \t");
                            part = part.substr(start, end == std::string::npos ? part.size() - start : end - start + 1);
                        }
                    }
                    if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
                        parts.push_back(part);
                    i = j + (j < existing.size() ? 1 : 0);
                }
                auto it = std::find(parts.begin(), parts.end(), tagNameCopy);
                if (it == parts.end())
                    parts.push_back(tagNameCopy);
                else
                    parts.erase(it);
                std::string combined;
                for (size_t i = 0; i < parts.size(); ++i)
                    combined += (i ? "," : "") + parts[i];
                reg.SetMetaValue(p, key, combined);
            }
            RefreshViews();
        });
    });
    if (m_OnOpenSettingsToTags)
        m_Controller->SetOnOpenSettingsToTags(m_OnOpenSettingsToTags);
    m_Controller->SetOnSelectAssets([this](const std::vector<std::filesystem::path>& paths)
                                    { HandleSelectionChanged(paths); });
    m_Controller->SetOnAssetsListScrolled([this]()
                                          {
        ModelThumbnailHandler::NoteAssetListScrollActivity();
        if (m_OnVideoPreviewBindingRefresh)
            m_OnVideoPreviewBindingRefresh();
    });
    m_Controller->SetOnRowHeightChanged([this](float height)
                                        { ApplyListRowHeight(height); });

    // Initialize smart folder manager
    m_SmartFolderController = std::make_unique<SmartFolderController>();
    m_SmartFolderController->GetManager().SetOnChanged([this]() {
        if (m_Controller) {
            m_Controller->RefreshSmartFolders();
        }
    });
    
    // Wire up smart folder manager to controller immediately
    // This ensures smart folders are displayed even before SetContext/SetAssetsRoot is called
    m_Controller->SetSmartFolderController(m_SmartFolderController.get());
    
    // Load smart folders early (global folders don't need project root)
    // Project folders will be loaded again when SetContext/SetAssetsRoot is called
    m_SmartFolderController->GetManager().Load();

    // Configure the list TableView now that the controller (and its row data provider) exist:
    // declare the columns, wire the keyed cell binders + sort + width persistence, restore the
    // saved per-project widths, and apply the column-visibility prefs.
    ConfigureListTable();

    // Initialize row height from controller (or use default).
    m_ListRowHeight = m_Controller ? m_Controller->GetListRowHeight() : 24.0f;
    ApplyListRowHeight(m_ListRowHeight);

    SyncAssetsToolbarZoomSlider();
}

void GameEngine::AssetsPanel::SetAssetsRoot(const std::filesystem::path& dir) {
    if (m_Controller) {
        m_Controller->SetAssetsRoot(dir);
    }
    // Reload smart folders when project changes (e.g., project picker)
    if (m_SmartFolderController) {
        // SmartFolderManager now uses GetCurrentEditorProjectPaths() internally,
        // so we just need to reload and refresh
        m_SmartFolderController->GetManager().Load();
        
        // Always refresh smart folders after the tree is set up
        if (m_Controller) {
            m_Controller->RefreshSmartFolders();
        }
    }
}

GameEngine::SmartFolderManager* GameEngine::AssetsPanel::GetSmartFolderManager()
{
    return m_SmartFolderController ? &m_SmartFolderController->GetManager() : nullptr;
}

bool GameEngine::AssetsPanel::SelectSmartFolderByName(const std::string& name)
{
    if (!m_SmartFolderController || !m_Controller)
        return false;
    for (const auto& folder : m_SmartFolderController->GetManager().GetAll())
    {
        if (folder.Name == name)
        {
            m_Controller->SelectSmartFolder(folder.Id);
            return true;
        }
    }
    return false;
}

int GameEngine::AssetsPanel::GetVisibleAssetCount() const
{
    return m_Controller ? m_Controller->GetVisibleItemCount() : 0;
}

std::filesystem::path GameEngine::AssetsPanel::GetVisibleAssetDirectory() const
{
    return m_Controller ? m_Controller->GetVisibleDirectory() : std::filesystem::path{};
}

void GameEngine::AssetsPanel::SetOnSelectAssets(std::function<void(const std::vector<std::filesystem::path>&)> cb) {
    m_OnSelectAssets = std::move(cb);
}

void GameEngine::AssetsPanel::SetOnAssetPreviewChanged(std::function<void(const std::filesystem::path&, bool)> cb)
{
    m_OnAssetPreviewChanged = std::move(cb);
    UpdateAssetPreview();
}

void GameEngine::AssetsPanel::SetOnVideoPreviewBindingRefresh(std::function<void()> cb)
{
    m_OnVideoPreviewBindingRefresh = std::move(cb);
}

void GameEngine::AssetsPanel::SetOnOpenAsset(std::function<void(const std::filesystem::path&)> cb) {
    if (m_Controller) m_Controller->SetOnOpenAsset(std::move(cb));
}

void GameEngine::AssetsPanel::SetOnEditAsset(std::function<void(const std::filesystem::path&)> cb) {
    if (m_Controller) m_Controller->SetOnEditAsset(std::move(cb));
}

void GameEngine::AssetsPanel::SetOnAddToBookmarks(std::function<void(const std::vector<std::filesystem::path>&)> cb) {
    if (m_Controller) m_Controller->SetOnAddToBookmarks(std::move(cb));
}

void GameEngine::AssetsPanel::NavigateToAndSelectAsset(const std::filesystem::path& assetPath)
{
    if (!m_Controller || assetPath.empty())
        return;

    // A reveal must make the target visible even when the Assets panel had a
    // local filter left over from earlier browsing.
    m_Controller->SetSearchQuery("", /*restoreSelectedResultOnClear=*/false);
    if (m_SearchField)
        m_SearchField->SetValue("");
    if (m_ToolbarSearchField)
        m_ToolbarSearchField->SetValue("");

    m_Controller->NavigateToAndSelectAsset(assetPath, [this]()
    {
        UIElement* activeView = m_IsGridViewActive
            ? static_cast<UIElement*>(m_GridView)
            : static_cast<UIElement*>(m_ListView);
        if (activeView)
        {
            if (UIManager* ui = activeView->GetOwnerManager())
                ui->FocusElement(activeView);
        }
    });
}

void GameEngine::AssetsPanel::NavigateToAndSelectAssetSilent(const std::filesystem::path& assetPath)
{
    if (!m_Controller || assetPath.empty())
        return;

    // Preserve the Inspector target, but otherwise reveal exactly like the
    // normal path: a filtered-out asset cannot be selected or scrolled into
    // view by the controller's post-navigation retry.
    m_Controller->SetSearchQuery("", /*restoreSelectedResultOnClear=*/false);
    if (m_SearchField)
        m_SearchField->SetValue("");
    if (m_ToolbarSearchField)
        m_ToolbarSearchField->SetValue("");

    m_Controller->NavigateToAndSelectAssetSilent(assetPath);
}

void GameEngine::AssetsPanel::NavigateToFolderSilent(const std::filesystem::path& folderPath)
{
    if (!m_Controller || folderPath.empty())
        return;
    m_Controller->NavigateToFolderSilent(folderPath);
}

void GameEngine::AssetsPanel::InvalidateThumbnailForPath(const std::filesystem::path& assetPath)
{
    if (m_Controller)
        m_Controller->InvalidateThumbnailForPath(assetPath);
}

void GameEngine::AssetsPanel::InvalidatePolyhavenDownloadCache()
{
    if (m_Controller)
        m_Controller->InvalidatePolyhavenDownloadCache();
}

void GameEngine::AssetsPanel::SetOnOpenSettingsToTags(std::function<void()> cb)
{
    m_OnOpenSettingsToTags = std::move(cb);
    if (m_AddTagModal)
        m_AddTagModal->SetOnOpenSettings(m_OnOpenSettingsToTags);
    if (m_Controller)
        m_Controller->SetOnOpenSettingsToTags(m_OnOpenSettingsToTags);
}

void GameEngine::AssetsPanel::DetachContextSubscriptions()
{
    // Neither unsubscribe joins a callback already running: both services copy
    // their listener list under the lock and invoke it outside, on whichever
    // thread broadcast. A running callback only touches its own copy of the
    // coalesced post, and Cancel keeps any refresh it queued from running.
    m_VcsRefresh.Cancel();
    m_MissingAssetsRefresh.Cancel();

    // Both handles detach themselves, and detaching from a service that died
    // first is inert — neither reads m_Context, which the shutdown order that
    // used to make this safe no longer has to guarantee.
    m_VcsSubscription.Reset();
    m_MissingAssetsSubscription.Reset();
}

void GameEngine::AssetsPanel::SetContext(const EditorContext* ctx) {
    // Unsubscribe from the previous context's notifications (if any).
    DetachContextSubscriptions();

    m_Context = ctx;
    if (m_Controller)
        m_Controller->SetContext(ctx);
    // Reports the "Generate Thumbnails" folder command under the views.
    if (m_Context && m_Context->Thumbnails && m_AssetsViewsContainer && !m_FolderBakeStatus)
    {
        auto status = std::make_unique<FolderBakeStatus>(*m_Context->Thumbnails);
        m_FolderBakeStatus = status.get();
        m_AssetsViewsContainer->AddChild(std::move(status));
    }
    // null ctx is allowed (e.g. first run before layout); all uses below guard on m_Context

    // Subscribe this panel to VCS status changes. Coalesce refresh to once per frame.
    // Skip in UI replay mode to keep deterministic fast-path behavior. The listener
    // fires on the provider's status thread.
    if (m_Context && !m_Context->UIReplayActive && m_Context->VcsService)
    {
        m_VcsRefresh = UI::UiCoalescedPost(GetPostHandle(), [this]() { RefreshViews(); });
        m_VcsSubscription = m_Context->VcsService->AddListener([refresh = m_VcsRefresh]() { refresh.Request(); });
    }

    // Subscribe to missing-asset rescan events so phantom rows appear/refresh
    // without a manual panel reopen. Coalesce identically to the VCS path.
    if (m_Context && !m_Context->UIReplayActive && m_Context->MissingAssets)
    {
        m_MissingAssetsRefresh = UI::UiCoalescedPost(GetPostHandle(), [this]() {
            if (m_Controller)
                m_Controller->ReloadCurrentDirectory();
            RefreshViews();
        });
        m_MissingAssetsSubscription =
            m_Context->MissingAssets->AddListener([refresh = m_MissingAssetsRefresh]() { refresh.Request(); });
    }
    
    // Defer smart folder loading to post-first-frame (JSON file I/O + tree rebuild).
    if (m_SmartFolderController)
    {
        SmartFolderManager* mgr = &m_SmartFolderController->GetManager();
        AssetsBrowserController* ctrl = m_Controller.get();
        PostAction([mgr, ctrl]()
        {
            mgr->Load();
            if (ctrl)
                ctrl->RefreshSmartFolders();
        });
    }
}

void GameEngine::AssetsPanel::SetVcsDialogCallbacks(
    std::function<void(const std::string&)> onShowCommitDialog,
    std::function<void(const std::filesystem::path&)> onShowVcsLog)
{
    if (m_Controller)
    {
        m_Controller->SetVcsDialogCallbacks(std::move(onShowCommitDialog), std::move(onShowVcsLog));
    }
}

void GameEngine::AssetsPanel::SetOnShowDiff(std::function<void(const std::filesystem::path&)> onShowDiff)
{
    if (m_Controller)
    {
        m_Controller->SetOnShowDiff(std::move(onShowDiff));
    }
}

void AssetsPanel::SetTreeRowHeight(float px)
{
    if (m_TreeView)
    {
        m_TreeView->SetRowHeight(px);
    }
}

float AssetsPanel::GetTreeRowHeight() const
{
    return m_TreeView ? m_TreeView->GetRowHeight() : 20.0f;
}

void AssetsPanel::SetTreeChildIndent(float px)
{
    if (m_TreeView)
    {
        m_TreeView->SetChildIndent(px);
    }
}

void AssetsPanel::SetTreeIconSize(float px)
{
    m_LastTreeIconSizePx = std::clamp(px, kMinEditorTreeIconSizePx, kMaxEditorAssetsTreeIconSizePx);
    if (m_TreeView)
    {
        ApplyTreeTitleIconLayoutVars(m_TreeView, m_LastTreeIconSizePx);
        m_TreeView->SetIconSize(m_LastTreeIconSizePx);

        // Also update separate .tree-folder-icon elements used for VCS status display
        std::function<void(UIElement*)> updateFolderIcons = [&](UIElement* el) {
            if (!el) return;
            if (el->HasClass("tree-folder-icon"))
            {
                el->Overrides()
                    .Set(Style::Width, StyleLength::Px(m_LastTreeIconSizePx))
                    .Set(Style::Height, StyleLength::Px(m_LastTreeIconSizePx))
                    .Set(Style::MinWidth, StyleLength::Px(m_LastTreeIconSizePx))
                    .Set(Style::MinHeight, StyleLength::Px(m_LastTreeIconSizePx));
            }
            for (const auto& child : el->GetChildren())
                updateFolderIcons(child.get());
        };
        updateFolderIcons(m_TreeView);
        m_TreeView->RefreshFromProvider();
    }
}

void AssetsPanel::SetGridIconSize(float px)
{
    if (m_GridView)
        m_GridView->SetIconSize(px);
    if (m_IsGridViewActive)
        SetToolbarGridZoomSliderValue(GetGridIconSize());
}

float AssetsPanel::GetGridIconSize() const
{
    return m_GridView ? m_GridView->GetIconSize() : 80.0f;
}

void AssetsPanel::SetTruncationThreshold(float threshold)
{
    if (m_Controller)
        m_Controller->SetTruncationThreshold(threshold);
}

void AssetsPanel::SetTruncationEnabled(bool enabled)
{
    if (m_Controller)
        m_Controller->SetTruncationEnabled(enabled);
}

void AssetsPanel::RefreshViews()
{
    // Re-label the VCS column and re-apply column visibility for the current settings/VCS.
    UpdateVcsColumnTitle();
    ApplyListColumnVisibilityFromPrefs();
    if (m_Controller)
    {
        m_Controller->InvalidateVcsStatusCache();
        m_Controller->InvalidateVcsUiSettings();
        m_Controller->InvalidateCachedDisplaySettings();
    }
    if (m_TreeView)
        m_TreeView->RefreshFromProvider();
    if (m_GridView)
        m_GridView->RefreshFromProvider();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void AssetsPanel::SetOnGridIconSizeChanged(std::function<void(float)> cb)
{
    m_OnGridIconSizeChanged = std::move(cb);
    if (m_GridView)
    {
        m_GridView->SetOnIconSizeChanged([this](float px)
        {
            if (m_IsGridViewActive)
                SetToolbarGridZoomSliderValue(px);
            if (m_OnGridIconSizeChanged)
                m_OnGridIconSizeChanged(px);
        });
    }
}


void AssetsPanel::SwitchToGridView()
{
    if (m_IsGridViewActive)
        return;

    m_IsGridViewActive = true;
    if (m_GridView)
    {
        SetAssetsVirtualizedViewActive(m_GridView, true);
        m_GridView->InvalidateVirtualization();
    }
    if (m_Table)
    {
        SetAssetsVirtualizedViewActive(m_Table, false);
    }
    if (m_GridViewButton)
    {
        m_GridViewButton->AddClass("active");
    }
    if (m_ExtraGridViewButton)
    {
        m_ExtraGridViewButton->AddClass("active");
    }
    if (m_ListViewButton)
    {
        m_ListViewButton->RemoveClass("active");
    }
    if (m_ExtraListViewButton)
    {
        m_ExtraListViewButton->RemoveClass("active");
    }
    UpdateSingleViewToggleButtons();
    ResetAssetsTypeAheadState();
    SyncAssetsToolbarZoomSlider();
}

void AssetsPanel::SwitchToListView()
{
    if (!m_IsGridViewActive)
        return;

    m_IsGridViewActive = false;
    if (m_GridView)
    {
        SetAssetsVirtualizedViewActive(m_GridView, false);
    }
    if (m_Table)
    {
        SetAssetsVirtualizedViewActive(m_Table, true);
        if (m_ListView)
            m_ListView->InvalidateVirtualization();
    }
    if (m_GridViewButton)
    {
        m_GridViewButton->RemoveClass("active");
    }
    if (m_ExtraGridViewButton)
    {
        m_ExtraGridViewButton->RemoveClass("active");
    }
    if (m_ListViewButton)
    {
        m_ListViewButton->AddClass("active");
    }
    if (m_ExtraListViewButton)
    {
        m_ExtraListViewButton->AddClass("active");
    }
    UpdateSingleViewToggleButtons();

    // Re-label the VCS column for the active VCS.
    UpdateVcsColumnTitle();

    // Refresh list view when switching to it (the TableView re-derives its content width on layout).
    if (m_Controller && m_ListView)
    {
        m_Controller->RefreshListView();
    }

    ResetAssetsTypeAheadState();
    SyncAssetsToolbarZoomSlider();
}

void AssetsPanel::ConfigureListTable()
{
    if (!m_Table || !m_Controller)
        return;

    // Fixed-width, resizable, sortable columns. The icon folds into the Name cell (icon + label),
    // and a trailing fill track absorbs slack. The Name min (100) only needs the in-cell icon plus
    // a short name fragment — not a full-label floor.
    std::vector<TrackDef> columns;
    auto addCol = [&columns](StringId key, const char* title, float size, float minSize)
    {
        TrackDef t;
        t.Key       = key;
        t.Title     = title;
        t.Size      = size;
        t.MinSize   = minSize;
        t.Sortable  = true;
        t.Resizable = true;
        columns.push_back(std::move(t));
    };
    addCol(AssetsListColumns::kName,       "Name",            300.0f, 100.0f);
    addCol(AssetsListColumns::kType,       "Type",             70.0f,  60.0f);
    addCol(AssetsListColumns::kSize,       "Size",             60.0f,  60.0f);
    addCol(AssetsListColumns::kDimensions, "Dimensions",      120.0f, 110.0f);
    addCol(AssetsListColumns::kModified,   "Modified",        150.0f,  80.0f);
    addCol(AssetsListColumns::kGit,        VcsColumnTitle(),  100.0f,  80.0f);
    addCol(AssetsListColumns::kTag,        "Tag",             140.0f,  60.0f);
    addCol(AssetsListColumns::kReferenced, "Referenced",      100.0f,  60.0f);
    addCol(AssetsListColumns::kCustom,     "Custom property", 130.0f,  60.0f);
    addCol(AssetsListColumns::kCreator,    "Creator",         100.0f,  60.0f);
    columns.push_back(TrackDef::MakeFill());
    m_Table->SetColumns(std::move(columns));

    // Provider + keyed binders: the controller owns the per-row + per-column content.
    m_Table->SetRowProvider(m_Controller->GetListProvider());
    m_Table->SetRowBinder([this](UIElement* row, ListId id, int rowIndex)
                          { if (m_Controller) m_Controller->BindListRow(row, id, rowIndex); });
    m_Table->SetCellBinder([this](UIElement* cell, int /*colIndex*/, const TrackDef& track,
                                  ListId id, int rowIndex, IListDataProvider*)
                           { if (m_Controller) m_Controller->BindListCell(cell, track.Key, id, rowIndex); });

    // Header click → existing grid sort. The TableView owns the sort glyph + asc/desc toggle.
    m_Table->SetOnSort([this](StringId key, SortDirection dir) { ApplyListSort(key, dir); });
    m_Table->SetSortIndicator(AssetsListColumns::kName, SortDirection::Ascending);

    // Persist the per-project column widths on resize commit.
    m_Table->SetOnLayoutChanged([this]() { SaveListColumnLayout(); });

    // Restore saved widths (per-project) first, then apply visibility (editor prefs win); the
    // latter refreshes the column layout.
    LoadListColumnLayout();
    ApplyListColumnVisibilityFromPrefs();
}

void AssetsPanel::ApplyListSort(StringId key, SortDirection dir)
{
    if (!m_Controller)
        return;
    SortDescriptor sort;
    if (key == AssetsListColumns::kType)            sort.field = SortDescriptor::Field::Type;
    else if (key == AssetsListColumns::kSize)        sort.field = SortDescriptor::Field::Size;
    else if (key == AssetsListColumns::kDimensions)  sort.field = SortDescriptor::Field::Dimensions;
    else if (key == AssetsListColumns::kModified)    sort.field = SortDescriptor::Field::Modified;
    else if (key == AssetsListColumns::kGit)         sort.field = SortDescriptor::Field::Git;
    else if (key == AssetsListColumns::kTag)         sort.field = SortDescriptor::Field::Tag;
    else if (key == AssetsListColumns::kReferenced)  sort.field = SortDescriptor::Field::Referenced;
    else if (key == AssetsListColumns::kCustom)      sort.field = SortDescriptor::Field::Custom;
    else if (key == AssetsListColumns::kCreator)     sort.field = SortDescriptor::Field::Creator;
    else                                             sort.field = SortDescriptor::Field::Name;
    sort.ascending = (dir != SortDirection::Descending);
    m_Controller->ApplySort(sort);
}

const char* AssetsPanel::VcsColumnTitle()
{
    Editor::EditorVcsProviderDescriptor provider;
    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider))
    {
        static std::string s_ColumnTitle;
        s_ColumnTitle = provider.StatusColumnTitle.empty() ? provider.DisplayName + " Status"
                                                           : provider.StatusColumnTitle;
        return s_ColumnTitle.c_str();
    }
    return "VCS Status";
}

void AssetsPanel::UpdateVcsColumnTitle()
{
    if (!m_Table)
        return;
    if (TrackDef* git = m_Table->Columns().Find(AssetsListColumns::kGit))
    {
        const char* title = VcsColumnTitle();
        if (git->Title != title)
        {
            git->Title = title;
            m_Table->RefreshColumnLayout();
        }
    }
}

void AssetsPanel::ApplyListColumnVisibilityFromPrefs()
{
    if (!m_Table)
        return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    auto isVisible = [&prefs](const char* key, bool def) { bool v = def; prefs.TryGetBool(key, v); return v; };

    // Name (column 0) is always visible; the other nine are individually toggleable.
    // Referenced / Custom / Creator default hidden (matching the previous behaviour).
    struct ColVis { StringId key; const char* prefKey; bool def; };
    const ColVis settings[] = {
        { AssetsListColumns::kType,       kListColPref_Type,       true },
        { AssetsListColumns::kSize,       kListColPref_Size,       true },
        { AssetsListColumns::kDimensions, kListColPref_Dimensions, true },
        { AssetsListColumns::kModified,   kListColPref_Modified,   true },
        { AssetsListColumns::kGit,        kListColPref_Git,        true },
        { AssetsListColumns::kTag,        kListColPref_Tag,        true },
        { AssetsListColumns::kReferenced, kListColPref_Referenced, false },
        { AssetsListColumns::kCustom,     kListColPref_Custom,     false },
        { AssetsListColumns::kCreator,    kListColPref_Creator,    false },
    };
    // Name is structurally always-on: force it visible so a stale/hand-edited persisted layout
    // (whose Hidden flag round-trips through AxisLayoutSerializer) can never hide it with no UI to recover.
    m_Table->Columns().SetHidden(AssetsListColumns::kName, false);
    for (const ColVis& cs : settings)
        m_Table->Columns().SetHidden(cs.key, !isVisible(cs.prefKey, cs.def));
    m_Table->RefreshColumnLayout();
}

void AssetsPanel::LoadListColumnLayout()
{
    if (!m_Table)
        return;
    const auto& workspaceRoot = GameEngine::EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;
    GameEngine::Editor::SettingsStore store = GameEngine::Editor::OpenUserProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);
    GameEngine::Editor::LoadAxisLayout(store, kListColumnWidthsKey, m_Table->Columns());
}

void AssetsPanel::SaveListColumnLayout()
{
    if (!m_Table)
        return;
    const auto& workspaceRoot = GameEngine::EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;
    GameEngine::Editor::SettingsStore store = GameEngine::Editor::OpenUserProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);
    GameEngine::Editor::SaveAxisLayout(store, kListColumnWidthsKey, m_Table->Columns());
    (void)store.Save(&err);
}

void AssetsPanel::ApplyListRowHeight(float height)
{
    m_ListRowHeight = height;
    // The provider owns the actual row height (the controller's zoom path sets it and refreshes
    // the body); the TableView re-sizes the in-cell icon from it on rebind. Here we only keep the
    // toolbar zoom slider in sync.
    if (!m_IsGridViewActive)
        SyncAssetsToolbarZoomSlider();
}

void AssetsPanel::HandleSelectionChanged(const std::vector<std::filesystem::path>& paths)
{
    if (m_Undo && !m_SuppressSelectionUndo)
    {
        const std::vector<std::filesystem::path> before = m_LastSelectionPaths;
        const std::vector<std::filesystem::path> after = paths;

        if (before != after)
        {
            class AssetsSelectionCommand final : public Editor::IEditorCommand
            {
              public:
                AssetsSelectionCommand(AssetsPanel* panel,
                                       std::vector<std::filesystem::path> beforePaths,
                                       std::vector<std::filesystem::path> afterPaths,
                                       bool* suppressFlag)
                    : m_Panel(panel)
                    , m_BeforePaths(std::move(beforePaths))
                    , m_AfterPaths(std::move(afterPaths))
                    , m_SuppressFlag(suppressFlag)
                {
                }

                const char* GetName() const override { return "Assets Selection"; }

                void Do() override { Redo(); }

                void Undo() override
                {
                    if (!m_Panel || !m_SuppressFlag)
                        return;
                    *m_SuppressFlag = true;
                    m_Panel->ApplySelectionFromUndo(m_BeforePaths);
                    *m_SuppressFlag = false;
                }

                void Redo() override
                {
                    if (!m_Panel || !m_SuppressFlag)
                        return;
                    *m_SuppressFlag = true;
                    m_Panel->ApplySelectionFromUndo(m_AfterPaths);
                    *m_SuppressFlag = false;
                }

              private:
                AssetsPanel* m_Panel = nullptr; // not owned
                std::vector<std::filesystem::path> m_BeforePaths;
                std::vector<std::filesystem::path> m_AfterPaths;
                bool* m_SuppressFlag = nullptr; // not owned
            };

            auto cmd = std::make_unique<AssetsSelectionCommand>(
                this,
                before,
                after,
                &m_SuppressSelectionUndo);
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }

    m_LastSelectionPaths = paths;

    m_SelectedAssetPaths = paths;
    if (m_OnSelectAssets)
        m_OnSelectAssets(paths);
    UpdateAssetPreview();
}

void AssetsPanel::SilentlyClearSelection()
{
    if (m_SelectedAssetPaths.empty() && m_LastSelectionPaths.empty())
        return;
    m_SelectedAssetPaths.clear();
    m_LastSelectionPaths.clear();
    if (m_Controller)
        m_Controller->SetSelectionFromPaths({});
    UpdateAssetPreview();
}

void AssetsPanel::ApplySelectionFromUndo(const std::vector<std::filesystem::path>& paths)
{
    m_LastSelectionPaths = paths;
    m_SelectedAssetPaths = paths;
    if (m_Controller)
        m_Controller->SetSelectionFromPaths(paths);
    if (m_OnSelectAssets)
        m_OnSelectAssets(paths);
    UpdateAssetPreview();
}

void AssetsPanel::UpdateAssetPreview()
{
    if (!m_OnAssetPreviewChanged)
        return;

    // Multi-select has no single preview target — keep the current Asset View
    // content instead of pushing an empty update that tears down video.
    if (m_SelectedAssetPaths.size() > 1)
        return;

    std::filesystem::path previewPath;
    if (m_SelectedAssetPaths.size() == 1)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(m_SelectedAssetPaths[0], ec))
            previewPath = m_SelectedAssetPaths[0];
    }

    // Panel path cache can lag behind the live selection model during list/grid
    // scroll virtualization — fall back so Asset View is not cleared mid-scroll.
    if (previewPath.empty() && m_Controller)
        previewPath = m_Controller->GetPrimarySelectedAssetPath();

    if (previewPath.empty() && m_LastSelectionPaths.size() == 1)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(m_LastSelectionPaths[0], ec))
            previewPath = m_LastSelectionPaths[0];
    }

    const bool enabled = m_AssetPreviewEnabled && !previewPath.empty();
    m_OnAssetPreviewChanged(previewPath, enabled);
}

void AssetsPanel::ResetAssetsTypeAheadState()
{
    m_AssetsTypeAheadRepeatKey = -1;
    m_AssetsTypeAheadLastKeyTime = {};
}

bool AssetsPanel::TryAssetsViewTypeAhead(UIEvent& e, bool useGrid)
{
    if (e.Mods != 0)
        return false;
    const int matchCh = TypeAheadKeyToMatchChar(e.Key);
    if (matchCh < 0 || !m_Controller)
        return false;

    GridView* grid = m_GridView;
    ListView* list = m_ListView;
    const int count =
        useGrid ? (grid ? grid->GetItemCount() : 0) : (list ? list->GetItemCount() : 0);
    if (count <= 0)
        return false;

    const auto now = std::chrono::steady_clock::now();
    const bool keyChanged = (e.Key != m_AssetsTypeAheadRepeatKey);
    const bool timedOut =
        (m_AssetsTypeAheadRepeatKey < 0) || (now - m_AssetsTypeAheadLastKeyTime > kAssetsTypeAheadRepeatWindow);
    const bool sameKeyRepeat = !keyChanged && !timedOut;

    m_AssetsTypeAheadLastKeyTime = now;
    m_AssetsTypeAheadRepeatKey = e.Key;

    const char matchChar = static_cast<char>(matchCh);
    const int selectedIndex =
        useGrid ? (grid ? grid->GetSelectedIndex() : -1) : (list ? list->GetSelectedIndex() : -1);

    int found = -1;
    if (!sameKeyRepeat)
    {
        for (int i = 0; i < count; ++i)
        {
            const char* lab = m_Controller->GetItemDisplayName(i);
            if (LabelFirstCharMatches(lab, matchChar))
            {
                found = i;
                break;
            }
        }
    }
    else
    {
        int start = selectedIndex + 1;
        if (start < 0)
            start = 0;
        for (int i = start; i < count; ++i)
        {
            const char* lab = m_Controller->GetItemDisplayName(i);
            if (LabelFirstCharMatches(lab, matchChar))
            {
                found = i;
                break;
            }
        }
        if (found < 0)
        {
            for (int i = 0; i < start && i < count; ++i)
            {
                const char* lab = m_Controller->GetItemDisplayName(i);
                if (LabelFirstCharMatches(lab, matchChar))
                {
                    found = i;
                    break;
                }
            }
        }
    }

    if (found < 0)
        return false;

    if (useGrid && grid)
        grid->SetSelectedIndexKeyboard(found, false, true);
    else if (!useGrid && list)
        list->SetSelectedIndexKeyboard(found, false, true);
    else
        return false;

    e.Stop();
    return true;
}

void AssetsPanel::SetOnSmartFolderSelected(std::function<void(const std::string&, SmartFolderManager*)> cb)
{
    if (m_SmartFolderController) {
        m_SmartFolderController->SetOnSelectionChanged(std::move(cb));
    }
}

void AssetsPanel::SetSmartFoldersAtTop(bool atTop)
{
    if (m_Controller) {
        m_Controller->SetSmartFoldersAtTop(atTop);
    }
}

void AssetsPanel::SetSmartFoldersExpandedOnStartup(bool expanded)
{
    if (m_Controller) {
        m_Controller->SetSmartFoldersExpandedOnStartup(expanded);
    }
}

void AssetsPanel::SetOnlineAssetsEnabled(bool enabled)
{
    if (m_Controller) {
        m_Controller->SetOnlineAssetsEnabled(enabled);
    }
}

void AssetsPanel::SetPolyHavenEnabled(bool enabled)
{
    if (m_Controller) {
        m_Controller->SetPolyHavenEnabled(enabled);
    }
}

void AssetsPanel::SetFoldersFirst(bool foldersFirst)
{
    if (m_Controller)
        m_Controller->SetFoldersFirst(foldersFirst);
}

void AssetsPanel::SetExpandFoldersOnLoad(bool expand)
{
    if (m_Controller)
        m_Controller->SetExpandFoldersOnLoad(expand);
}

void AssetsPanel::SetExtraBottomViewToolbarEnabled(bool enabled)
{
    m_ExtraBottomViewToolbarEnabled = enabled;
    if (m_MainViewToggleContainer)
    {
        m_MainViewToggleContainer->Overrides()
            .Set(Style::Display, enabled ? DisplayMode::None : DisplayMode::Flex);
    }
    if (m_ExtraViewToolbar)
        m_ExtraViewToolbar->Overrides().Set(Style::Display, enabled ? DisplayMode::Flex : DisplayMode::None);
    UpdateViewToggleButtonsPresentation();
    UpdateSearchBarPlacement();
    ApplyAssetsViewsBottomInsetForToolbar();
    ApplyBottomToolbarZoomSliderVisibility();
}

void AssetsPanel::SetBottomToolbarZoomSliderVisible(bool visible)
{
    m_BottomToolbarZoomSliderVisible = visible;
    ApplyBottomToolbarZoomSliderVisibility();
}

void AssetsPanel::ApplyBottomToolbarZoomSliderVisibility()
{
    if (!m_ToolbarGridZoomSlider)
        return;
    const bool show = m_ExtraBottomViewToolbarEnabled && m_BottomToolbarZoomSliderVisible;
    m_ToolbarGridZoomSlider->Overrides().Set(Style::Display, show ? DisplayMode::Flex : DisplayMode::None);
}

void AssetsPanel::HandleAssetsToolbarZoomSliderScroll(float scrollY)
{
    // The same step the gesture takes over the view the slider sizes.
    if (m_IsGridViewActive)
    {
        if (m_GridView)
            m_GridView->AdjustIconSizeFromScroll(scrollY);
    }
    else if (m_Controller)
    {
        m_Controller->ResizeListRowsFromScroll(scrollY);
    }
    SyncAssetsToolbarZoomSlider();
}

void AssetsPanel::SetSingleViewToggleIconEnabled(bool enabled)
{
    m_SingleViewToggleIconEnabled = enabled;
    UpdateSingleViewToggleButtons();
    UpdateViewToggleButtonsPresentation();
}

void AssetsPanel::ApplySearchField(const std::string& value, Dropdown* source)
{
    const int index = AssetsSearchFieldIndex(value);
    if (m_SearchFilter && m_SearchFilter != source)
        m_SearchFilter->SetSelectedIndexWithoutNotify(index);
    if (m_ToolbarSearchFilter && m_ToolbarSearchFilter != source)
        m_ToolbarSearchFilter->SetSelectedIndexWithoutNotify(index);

    const bool filteredField = index != 0;
    for (UIElement* bar : {m_SearchBar, m_ToolbarSearchBar})
    {
        if (!bar)
            continue;
        if (filteredField)
            bar->AddClass("search-filter-active");
        else
            bar->RemoveClass("search-filter-active");
    }

    if (m_Controller)
        m_Controller->SetSearchField(AssetsSearchFieldFromValue(value));
}

void AssetsPanel::UpdateSearchBarPlacement()
{
    auto applyExtraToolbarClass = [this](UIElement* searchBar)
    {
        if (!searchBar)
            return;
        if (m_ExtraBottomViewToolbarEnabled)
            searchBar->AddClass("assets-extra-toolbar-enabled");
        else
            searchBar->RemoveClass("assets-extra-toolbar-enabled");
    };
    applyExtraToolbarClass(m_SearchBar);
    applyExtraToolbarClass(m_ToolbarSearchBar);

    // Keep search text consistent when switching between left and toolbar bars.
    if (m_ExtraBottomViewToolbarEnabled)
    {
        if (m_SearchField && m_ToolbarSearchField)
            m_ToolbarSearchField->SetValue(m_SearchField->GetValue());
    }
    else
    {
        if (m_SearchField && m_ToolbarSearchField)
            m_SearchField->SetValue(m_ToolbarSearchField->GetValue());
    }
}

void AssetsPanel::ApplyAssetsViewsBottomInsetForToolbar()
{
    if (!m_AssetsViewsContainer)
        return;
    if (m_ExtraBottomViewToolbarEnabled)
    {
        m_AssetsViewsContainer->Overrides()
            .Set(Style::PaddingBottom, StyleLength::Px(kAssetsBottomToolbarHeightPx));
    }
    else
    {
        m_AssetsViewsContainer->Overrides().Set(Style::PaddingBottom, StyleLength::Px(0.0f));
    }
}

void AssetsPanel::UpdateViewToggleButtonsPresentation()
{
    if (m_GridViewButton)
        m_GridViewButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::None : DisplayMode::Flex);
    if (m_ListViewButton)
        m_ListViewButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::None : DisplayMode::Flex);
    if (m_SingleViewToggleButton)
        m_SingleViewToggleButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::Flex : DisplayMode::None);

    if (m_ExtraGridViewButton)
        m_ExtraGridViewButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::None : DisplayMode::Flex);
    if (m_ExtraListViewButton)
        m_ExtraListViewButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::None : DisplayMode::Flex);
    if (m_ExtraSingleViewToggleButton)
        m_ExtraSingleViewToggleButton->Overrides().Set(Style::Display, m_SingleViewToggleIconEnabled ? DisplayMode::Flex : DisplayMode::None);
}

void AssetsPanel::UpdateSingleViewToggleButtons()
{
    auto updateButton = [this](Button* button)
    {
        if (!button)
            return;
        button->RemoveClass("grid-view-icon");
        button->RemoveClass("list-view-icon");
        if (m_IsGridViewActive)
        {
            button->AddClass("list-view-icon");
            button->SetTooltip("List View");
        }
        else
        {
            button->AddClass("grid-view-icon");
            button->SetTooltip("Grid View");
        }
    };
    updateButton(m_SingleViewToggleButton);
    updateButton(m_ExtraSingleViewToggleButton);
}

void AssetsPanel::SyncAssetsToolbarZoomSlider()
{
    if (!m_ToolbarGridZoomSlider)
        return;
    m_ToolbarGridZoomSliderUpdating = true;
    if (m_IsGridViewActive)
    {
        m_ToolbarGridZoomSlider->SetMin(0.0f);
        m_ToolbarGridZoomSlider->SetMax(1.0f);
        m_ToolbarGridZoomSlider->SetStep(0.0f);
        m_ToolbarGridZoomSlider->SetValue(GridToolbarSliderTFromIconPx(GetGridIconSize()));
    }
    else
    {
        m_ToolbarGridZoomSlider->SetMin(16.0f);
        m_ToolbarGridZoomSlider->SetMax(64.0f);
        m_ToolbarGridZoomSlider->SetStep(1.0f);
        const float h = m_Controller ? m_Controller->GetListRowHeight() : m_ListRowHeight;
        m_ToolbarGridZoomSlider->SetValue(h);
    }
    m_ToolbarGridZoomSliderUpdating = false;
}

void AssetsPanel::SetToolbarGridZoomSliderValue(float gridIconPx)
{
    if (!m_ToolbarGridZoomSlider)
        return;
    m_ToolbarGridZoomSliderUpdating = true;
    m_ToolbarGridZoomSlider->SetValue(GridToolbarSliderTFromIconPx(gridIconPx));
    m_ToolbarGridZoomSliderUpdating = false;
}

bool AssetsPanel::HandleUiReplayCommand(std::uint32_t commandId, std::string* outError)
{
    // UIReplay view mode control (keeps scenarios deterministic regardless of last-used view).
    if (commandId == UiReplayCommandIds::AssetsViewGrid)
    {
        SwitchToGridView();
        return true;
    }
    if (commandId == UiReplayCommandIds::AssetsViewList)
    {
        SwitchToListView();
        return true;
    }

    if (!m_Controller)
        return false;
    return m_Controller->HandleUiReplayCommand(commandId, outError);
}

AssetsPanel::~AssetsPanel()
{
    DetachContextSubscriptions();
    // Close the scan-post gate while the panel still owns its UI children.
    m_Controller.reset();
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);
    if (m_ToolbarSearchBar)
        SettingsPanel::UnregisterSearchBar(m_ToolbarSearchBar);
}

} // namespace GameEngine
