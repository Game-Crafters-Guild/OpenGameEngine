#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/AxisModel.h"  // StringId, SortDirection, TrackDef
#include "UI/Interaction/Selection.h"
#include "UI/UiCoalescedPost.h"
#include "MissingAssetTracker.h"
#include "VersionControl/EditorVersionControlService.h"

namespace GameEngine {

class TreeView;
class GridView;
class ListView;
class TableView;
class Button;
class Dropdown;
class Slider;
class AddTagModal;
class AssetsBrowserController;
class FolderBakeStatus;
class TextField;
class UIElement;
class SmartFolderManager;
class SmartFolderController;
struct EditorContext;
namespace Editor { class UndoRedoService; }

class AssetsPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-assets-icon"; }

    AssetsPanel();
    ~AssetsPanel() override;

    // Configure assets root for the grid (forwarded to controller)
    void SetAssetsRoot(const std::filesystem::path& dir);

    // Optional shared editor context (assets root, services)
    void SetContext(const EditorContext* ctx);

    // Set callbacks for VCS dialogs (commit dialog, log viewer)
    void SetVcsDialogCallbacks(
        std::function<void(const std::string&)> onShowCommitDialog,
        std::function<void(const std::filesystem::path&)> onShowVcsLog);
    // Set callback for diff panel
    void SetOnShowDiff(std::function<void(const std::filesystem::path&)> onShowDiff);

    // UI tweak: control the left TreeView row height (pixels).
    void SetTreeRowHeight(float px);
    float GetTreeRowHeight() const;
    // UI tweak: control the left TreeView child indentation (pixels).
    void SetTreeChildIndent(float px);
    // UI tweak: control the left TreeView icon size (pixels).
    void SetTreeIconSize(float px);

    /** The item resize gesture over the assets tree; editor wires to Settings + prefs. */
    void SetOnTreeIconSizeWheelCommit(std::function<void(float)> cb) { m_OnTreeIconSizeWheelCommit = std::move(cb); }
    void SetGridIconSize(float px);
    float GetGridIconSize() const;
    void SetOnGridIconSizeChanged(std::function<void(float)> cb);
    
    // Set live truncation threshold for grid view (used during slider drag)
    void SetTruncationThreshold(float threshold);
    
    // Set truncation enabled and refresh grid view
    void SetTruncationEnabled(bool enabled);
    
    // Refresh all views (tree, grid, list) - useful when display settings change
    void RefreshViews();

    // Forward selection/open callbacks to controller
    void SetOnSelectAssets(std::function<void(const std::vector<std::filesystem::path>&)> cb);
    void SetOnOpenAsset(std::function<void(const std::filesystem::path&)> cb);
    void SetOnEditAsset(std::function<void(const std::filesystem::path&)> cb);
    void SetOnAddToBookmarks(std::function<void(const std::vector<std::filesystem::path>&)> cb);
    void SetOnAssetPreviewChanged(std::function<void(const std::filesystem::path&, bool)> cb);
    void SetOnVideoPreviewBindingRefresh(std::function<void()> cb);
    
    // Smart folders support
    SmartFolderManager* GetSmartFolderManager();
    // Automation-safe selection that also works while the tree section is collapsed.
    bool SelectSmartFolderByName(const std::string& name);
    int GetVisibleAssetCount() const;
    std::filesystem::path GetVisibleAssetDirectory() const;
    void SetSmartFoldersAtTop(bool atTop);
    void SetSmartFoldersExpandedOnStartup(bool expanded);
    void SetOnlineAssetsEnabled(bool enabled);
    void SetPolyHavenEnabled(bool enabled);
    void SetFoldersFirst(bool foldersFirst);
    void SetExpandFoldersOnLoad(bool expand);
    void SetExtraBottomViewToolbarEnabled(bool enabled);
    void SetSingleViewToggleIconEnabled(bool enabled);
    void SetBottomToolbarZoomSliderVisible(bool visible);
    // Callback when a smart folder is selected (for inspector display)
    void SetOnSmartFolderSelected(std::function<void(const std::string&, SmartFolderManager*)> cb);
    
    // Get currently selected asset paths
    const std::vector<std::filesystem::path>& GetSelectedAssetPaths() const { return m_SelectedAssetPaths; }

    /// Clear the asset selection without firing OnSelectAssets (used for cross-panel sync).
    void SilentlyClearSelection();

    /// Callback invoked when the user clicks "All Tags…" in the Add Tag modal (e.g. open Settings → Tags).
    void SetOnOpenSettingsToTags(std::function<void()> cb);
    
    // Navigate to and select an asset by path
    void NavigateToAndSelectAsset(const std::filesystem::path& assetPath);
    // Variants that do not fire OnSelectAssets — used to restore a past selection from
    // inspector history without recording a new entry.
    void NavigateToAndSelectAssetSilent(const std::filesystem::path& assetPath);
    void NavigateToFolderSilent(const std::filesystem::path& folderPath);

    // Re-apply the thumbnail for a single asset path without rebuilding the grid.
    void InvalidateThumbnailForPath(const std::filesystem::path& assetPath);

    // Clear cached Polyhaven download status so grid cells re-check (e.g. after background download completes).
    void InvalidatePolyhavenDownloadCache();

    // UIReplay-only: deterministic commands (navigate to sandbox dirs, etc).
    // Returns true if the commandId was handled.
    bool HandleUiReplayCommand(std::uint32_t commandId, std::string* outError);

    // Optional editor undo service (not owned).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

private:
    // Drop the VCS and missing-asset subscriptions bound to the current context
    // and disarm any callback already in flight.
    void DetachContextSubscriptions();

    // UI elements (owned by UI tree via DockPanel::AddChild)
    TreeView* m_TreeView = nullptr;
    UIElement* m_LeftPane = nullptr;
    UIElement* m_AssetsViewsContainer = nullptr;
    FolderBakeStatus* m_FolderBakeStatus = nullptr;
    float m_LastTreeIconSizePx = 20.0f;
    std::function<void(float)> m_OnTreeIconSizeWheelCommit;
    GridView* m_GridView = nullptr;
    TableView* m_Table = nullptr;    // owns the list header + virtualized body + column resize/sort
    ListView* m_ListView = nullptr;  // alias of m_Table->Body(); keyboard / selection / type-ahead
    Button* m_GridViewButton = nullptr;
    Button* m_ListViewButton = nullptr;
    Slider* m_ToolbarGridZoomSlider = nullptr;
    bool m_ToolbarGridZoomSliderUpdating = false;
    Button* m_SingleViewToggleButton = nullptr;
    UIElement* m_MainViewToggleContainer = nullptr;
    Button* m_ExtraGridViewButton = nullptr;
    Button* m_ExtraListViewButton = nullptr;
    Button* m_ExtraSingleViewToggleButton = nullptr;
    UIElement* m_ExtraViewToolbar = nullptr;
    UIElement* m_ExtraViewToolbarSearchHost = nullptr;
    UIElement* m_ExtraViewToolbarRight = nullptr;
    UIElement* m_SearchBar = nullptr;
    TextField* m_SearchField = nullptr;
    Dropdown* m_SearchFilter = nullptr;
    UIElement* m_ToolbarSearchBar = nullptr;
    TextField* m_ToolbarSearchField = nullptr;
    Dropdown* m_ToolbarSearchFilter = nullptr;

    // View state (loaded from preferences, defaults to list view)
    bool m_IsGridViewActive = false;
    bool m_ExtraBottomViewToolbarEnabled = false;
    bool m_SingleViewToggleIconEnabled = false;
    bool m_BottomToolbarZoomSliderVisible = true;
    float m_ListRowHeight = 24.0f;

    // Helper methods
    void ApplyListRowHeight(float height);
    void SwitchToGridView();
    void SwitchToListView();
    // List TableView wiring: column model + keyed binders + sort + per-project width persistence.
    void ConfigureListTable();
    void ApplyListSort(StringId key, SortDirection dir);
    const char* VcsColumnTitle();
    void UpdateVcsColumnTitle();
    void ApplyListColumnVisibilityFromPrefs();
    void UpdateSearchBarPlacement();
    void ApplySearchField(const std::string& value, Dropdown* source);
    void ApplyAssetsViewsBottomInsetForToolbar();
    void ApplyBottomToolbarZoomSliderVisibility();
    void HandleAssetsToolbarZoomSliderScroll(float scrollY);
    void UpdateViewToggleButtonsPresentation();
    void UpdateSingleViewToggleButtons();
    void SetToolbarGridZoomSliderValue(float value);
    void SyncAssetsToolbarZoomSlider();
    void LoadListColumnLayout();
    void SaveListColumnLayout();
    void HandleSelectionChanged(const std::vector<std::filesystem::path>& paths);
    void ApplySelectionFromUndo(const std::vector<std::filesystem::path>& paths);
    void UpdateAssetPreview();
    bool IsImageAssetPath(const std::filesystem::path& path) const;

    /// Finder-style: letter/digit jumps to next item whose name starts with that character.
    bool TryAssetsViewTypeAhead(UIEvent& e, bool useGrid);
    void ResetAssetsTypeAheadState();

    // Per-panel controller instance
    std::unique_ptr<AssetsBrowserController> m_Controller;

    AddTagModal* m_AddTagModal = nullptr;
    std::function<void()> m_OnOpenSettingsToTags;

    const EditorContext* m_Context = nullptr; // not owned
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnSelectAssets;
    std::function<void(const std::filesystem::path&, bool)> m_OnAssetPreviewChanged;
    std::function<void()> m_OnVideoPreviewBindingRefresh;
    std::vector<std::filesystem::path> m_SelectedAssetPaths;
    bool m_AssetPreviewEnabled = false;

    // VCS refresh subscription (supports multiple Assets panels). The listener
    // fires on the provider's status thread and only calls m_VcsRefresh.Request.
    UI::UiCoalescedPost m_VcsRefresh;
    EditorVersionControlService::Subscription m_VcsSubscription;

    // Missing-asset refresh subscription. Re-runs RefreshViews when the
    // tracker rescans, so phantom rows appear/disappear without a reopen.
    UI::UiCoalescedPost m_MissingAssetsRefresh;
    MissingAssetTracker::Subscription m_MissingAssetsSubscription;
    
    // Smart folders
    std::unique_ptr<SmartFolderController> m_SmartFolderController;

    // Selection undo support: track last asset selection so we can create
    // undoable commands for grid/list selection changes.
    Editor::UndoRedoService* m_Undo = nullptr; // not owned
    std::vector<std::filesystem::path> m_LastSelectionPaths;
    bool m_SuppressSelectionUndo = false;

    std::chrono::steady_clock::time_point m_AssetsTypeAheadLastKeyTime{};
    int m_AssetsTypeAheadRepeatKey = -1;
    std::function<void(float)> m_OnGridIconSizeChanged;
};

} // namespace GameEngine
