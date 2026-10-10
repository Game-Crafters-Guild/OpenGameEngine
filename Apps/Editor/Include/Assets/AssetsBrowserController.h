#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "Assets/AssetsDataProviders.h" // brings in complete types for providers
#include "UI/Interaction/Selection.h"
#include "Assets/AssetRenameController.h"
#include "VCSIntegration/VCSFileStatus.h"
#include "Assets/FileWatchingService.h"
#include "EditorContextMenu/EditorContextMenu.h"
#include <optional>
#include "Assets/PolyhavenService.h"
#include "Types/StringId.h"

namespace GameEngine {

class UIElement;
class TreeView;
class GridView;
class ListView;
class Label;
class AssetManager;
class PreferencesStore;
class ThumbnailService; // or IconProvider; see docs naming discussion
class SmartFolderController;
struct EditorContext;
namespace Editor { struct CodeAssetDescriptor; }

// Stable per-column keys for the Assets list TableView, shared by the panel (which builds
// the TrackDef column model) and the controller (whose keyed cell binder dispatches on them).
// The persisted per-project column layout is keyed by these StringIds, so the strings are
// part of the on-disk settings format — don't rename them casually.
namespace AssetsListColumns
{
inline constexpr StringId kName       = "assets.list.name"_sid;
inline constexpr StringId kType       = "assets.list.type"_sid;
inline constexpr StringId kSize       = "assets.list.size"_sid;
inline constexpr StringId kDimensions = "assets.list.dimensions"_sid;
inline constexpr StringId kModified   = "assets.list.modified"_sid;
inline constexpr StringId kGit        = "assets.list.git"_sid;
inline constexpr StringId kTag        = "assets.list.tag"_sid;
inline constexpr StringId kReferenced = "assets.list.referenced"_sid;
inline constexpr StringId kCustom     = "assets.list.custom"_sid;
inline constexpr StringId kCreator    = "assets.list.creator"_sid;
} // namespace AssetsListColumns

class AssetsBrowserController {
public:
    AssetsBrowserController() = default;
    ~AssetsBrowserController();

    // For now, accept only views; services will be injected later via setters/ctor overloads
    void Initialize(TreeView* tree, GridView* grid, ListView* list = nullptr);

    // Optional shared editor context (assets root, services)
    void SetContext(const EditorContext* ctx);

    // Configure the root folder used by the grid provider
    void SetAssetsRoot(const std::filesystem::path& dir);

    // Reload the provider's current directory in place. Used after data
    // sources outside the filesystem change (e.g. the missing-asset
    // tracker rescans) so phantom rows are re-injected.
    void ReloadCurrentDirectory();

    // Optional: handle opening a non-directory asset from the grid
    void SetOnOpenAsset(std::function<void(const std::filesystem::path&)> cb) { m_OnOpenAsset = std::move(cb); }
    // Optional: handle "Edit" action for assets (e.g., open in internal editor)
    void SetOnEditAsset(std::function<void(const std::filesystem::path&)> cb) { m_OnEditAsset = std::move(cb); }
    // Optional: handle "Add to Bookmarks" action for assets (paths may be multi-selection)
    void SetOnAddToBookmarks(std::function<void(const std::vector<std::filesystem::path>&)> cb) { m_OnAddToBookmarks = std::move(cb); }
    // Optional: handle selection changes (paths may contain multiple assets)
    void SetOnSelectAssets(std::function<void(const std::vector<std::filesystem::path>&)> cb) { m_OnSelectAssets = std::move(cb); }

    // Set callbacks for VCS dialogs (commit dialog, log viewer)
    void SetVcsDialogCallbacks(
        std::function<void(const std::string&)> onShowCommitDialog,
        std::function<void(const std::filesystem::path&)> onShowVcsLog);
    // Set callback for diff panel
    void SetOnShowDiff(std::function<void(const std::filesystem::path&)> onShowDiff);

    // Set callback for "Add tag" context menu (primary path, selected paths); opens modal if no Tags submenu.
    void SetOnAddTag(std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> cb);
    /// Assign tag to paths from Tags submenu (no modal). When set, context menu shows Tags submenu.
    void SetOnAssignTag(std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> cb);
    /// Open Settings → Tags (e.g. "All Tags…" in Tags submenu).
    void SetOnOpenSettingsToTags(std::function<void()> cb);

    // Apply sort to the current assets views
    void ApplySort(const SortDescriptor& desc);
    void SetFoldersFirst(bool foldersFirst);
    void SetExpandFoldersOnLoad(bool expand);

    // Keyed bind hooks for the Assets list TableView (the panel wires these onto the control's
    // row + cell binders). BindListRow does whole-row state (folder/scene/script classes, the
    // row→path map for the bookmark micro-drag); BindListCell fills one column's content,
    // dispatched by the column's stable AssetsListColumns key.
    void BindListRow(UIElement* row, ListId id, int rowIndex);
    void BindListCell(UIElement* cell, StringId colKey, ListId id, int rowIndex);

    // Refresh the list view (called when switching to list view)
    void RefreshListView();

    // The list-view row data provider (created in Initialize). The panel wires this into the
    // TableView via SetRowProvider; null until Initialize runs with a non-null list.
    IListDataProvider* GetListProvider() const { return m_ListProvider.get(); }

    // Filter assets by search query (applies to grid + list). When an explicit
    // reveal is about to navigate elsewhere, callers can skip restoring the
    // selected search result while clearing the query.
    void SetSearchQuery(const std::string& query, bool restoreSelectedResultOnClear = true);
    void SetSearchField(AssetsSearchField field);

    // Re-apply the thumbnail for a single asset path without rebuilding the grid.
    // Safe to call from the main thread after an on-disk thumbnail file changes.
    void InvalidateThumbnailForPath(const std::filesystem::path& assetPath);

    // Clear cached VCS status so subsequent binds re-fetch.
    void InvalidateVcsStatusCache();

    // Clear cached download status so grid cells re-check and update blue text.
    void InvalidatePolyhavenDownloadCache();

    // Mark VCS UI settings (color whole text, show icons, etc.) dirty so next bind re-reads from Settings.
    void InvalidateVcsUiSettings();

    // Mark cached asset-view display preferences (text truncation, list font size) dirty.
    void InvalidateCachedDisplaySettings();
    
    // Smart folders support
    void SetSmartFolderController(SmartFolderController* controller);
    void RefreshSmartFolders();
    void SelectSmartFolder(const std::string& smartFolderId);
    int GetVisibleItemCount() const;
    std::filesystem::path GetVisibleDirectory() const;
    void SetSmartFoldersAtTop(bool atTop);
    void SetSmartFoldersExpandedOnStartup(bool expanded);
    void SetOnlineAssetsEnabled(bool enabled);
    void SetPolyHavenEnabled(bool enabled);

    // Navigate to directory and select an asset by path
    void NavigateToAndSelectAsset(const std::filesystem::path& assetPath,
                                  std::function<void()> onSelected = {});
    // Live selection from the shared item model (survives list/grid scroll rebind).
    std::filesystem::path GetPrimarySelectedAssetPath() const;
    // Same as NavigateToAndSelectAsset but does not fire m_OnSelectAssets — used when
    // restoring asset selection from inspector history so it does not record a new entry.
    void NavigateToAndSelectAssetSilent(const std::filesystem::path& assetPath);
    // Navigate tree + grid to a folder without firing m_OnSelectAssets.
    void NavigateToFolderSilent(const std::filesystem::path& folderPath);
    // Directly set grid/list selection from a set of asset paths (used by undo/redo).
    void SetSelectionFromPaths(const std::vector<std::filesystem::path>& paths);

    // Inline rename of the file at `assetPath` in whichever view is showing: its
    // name becomes an editor (Enter commits, Escape cancels). Renaming a surface
    // shader renames its paired material too. Folders and virtual entries are ignored.
    void BeginRename(const std::filesystem::path& assetPath);
    // BeginRename for the single selected file, if there is one.
    void BeginRenameOfSelection();
    
    // Text truncation settings for asset browser (grid/list).
    // These are typically driven live from SettingsPanel.
    void SetTruncationThreshold(float threshold);
    void SetTruncationEnabled(bool enabled);

    // Callback when list row height changes (for the item resize gesture)
    void SetOnRowHeightChanged(std::function<void(float)> cb) { m_OnRowHeightChanged = std::move(cb); }

    // Fired when the assets list scroll offset changes (virtualized rebind).
    void SetOnAssetsListScrolled(std::function<void()> cb) { m_OnAssetsListScrolled = std::move(cb); }
    
    // Get current list row height
    float GetListRowHeight() const;

    /// Set list row height (same clamp/refresh path as the item resize gesture on the list).
    void SetListRowHeight(float heightPx);

    /// One item resize step on the list rows: wheel up (negative scrollY) makes them taller.
    void ResizeListRowsFromScroll(float scrollY);

    /// Display name for the item at `index` in the current grid/list (shared ordering). Null if out of range.
    const char* GetItemDisplayName(int index) const;

    // UIReplay-only: deterministic navigation without relying on brittle UI hit testing.
    // Returns true if the commandId was handled.
    bool HandleUiReplayCommand(std::uint32_t commandId, std::string* outError);
    
private:
    struct VcsUiSettings
    {
        bool showStatusIcons = true;
        bool colorWholeText = false;
        bool colorDotOnly = true;
        bool hideDotForClean = true;
    };
    void RefreshCachedVcsUiSettings();
    void RefreshCachedTruncationSettings();
    void RefreshCachedListFontSize();
    bool TryGetCachedVcsStatus(const std::filesystem::path& path, VCSFileStatus& outStatus);
    void QueueVcsStatusRequest(const std::filesystem::path& path, const std::string& key);
    void ProcessPendingVcsStatus();
    // Wake the active VCS integration's poll thread after an editor-initiated
    // file operation (drag-drop move/rename) so the panel reflects the new
    // state on the next tick instead of waiting for the next polling interval.
    static void NotifyVcsStatusDirty();

    /// Let navigation settle before visible cells start thumbnail work.
    void DeferThumbnailsAfterNavigation();

    // Show context menu for a Polyhaven online asset (shared by grid and list views).
    void ShowPolyhavenContextMenu(const std::filesystem::path& itemPath, float x, float y);

    // Reload the file's directory, then select and scroll to it; `onSelected` runs
    // once the selection landed. For files newer than the provider's listing.
    void RevealAsset(const std::filesystem::path& assetPath, std::function<void()> onSelected);
    // Select, reveal and start renaming a file the user just created — the
    // file-manager convention for "name it now".
    void RevealCreatedAsset(const std::filesystem::path& createdPath);
    // A rename requested before the item's cell exists (fresh directory load,
    // scroll-into-view) starts when the grid or list binds that item.
    void StartPendingRenameIfBound(const std::filesystem::path& boundPath);
    Label* FindNameLabelForPath(const std::filesystem::path& assetPath) const;
    bool IsGridViewShowing() const;
    void OnAssetRenamed(const std::filesystem::path& renamedTo);

    TreeView* m_Tree = nullptr; // not owned
    GridView* m_Grid = nullptr; // not owned
    ListView* m_List = nullptr; // not owned

    std::unique_ptr<AssetsTreeDataProvider> m_TreeProvider;
    std::unique_ptr<AssetsGridDataProvider> m_GridProvider;
    std::unique_ptr<AssetsListDataProvider> m_ListProvider;
    AssetRenameController m_Rename;
    std::filesystem::path m_PendingRenamePath;

    // Selection models:
    // - Tree (folders) has its own selection.
    // - Grid/List share selection for the same dataset (same folder view).
    std::unique_ptr<UI::Interaction::SelectionModel> m_TreeSelection;
    std::unique_ptr<UI::Interaction::SelectionModel> m_ItemSelection;

    // Watch for changes and refresh grid automatically. Its callback uses
    // members declared after it, so the destructor resets it first.
    std::optional<FileWatchSubscription> m_AssetsWatchSub;
    std::filesystem::path m_AssetsRoot;

    // A folder navigation whose listing a worker still builds. It is pending
    // while its listing generation is the grid provider's newest: any later
    // listing (another navigation, a refresh, a project switch) supersedes it
    // and drops its callbacks.
    struct PendingNavigation
    {
        std::filesystem::path Dir;
        uint64_t Generation = 0;
        std::vector<std::function<void()>> OnNavigated;
    };
    PendingNavigation m_PendingNavigation;
    bool IsNavigationPending() const;
    // Lists `dir` on a worker and returns the listing generation. The listing
    // applies on the UI thread unless a newer listing superseded it.
    uint64_t ScanDirectoryInBackground(const std::filesystem::path& dir);
    void ApplyScannedDirectory(AssetsGridDataProvider::DirectoryScan scan, uint64_t generation);
    // Builds the project-wide search snapshot on a worker, so the first search
    // does not build it on the UI thread.
    void WarmSearchIndexInBackground();

    // When true, the tree's OnSelectionChanged handler suppresses its side effects
    // (grid load + inspector fire). Used to distinguish user clicks from programmatic
    // selection syncs (SyncTreeSelectionToDirectory, NavigateToDirectory).
    bool m_SuppressTreeSelectSideEffects = false;

    // While non-zero, m_OnSelectAssets is not fired from grid selection changes or
    // explicit callsites. The depth keeps overlapping history-driven navigations
    // suppressed until every deferred selection has completed.
    std::size_t m_InspectorAssetFireSuppressionDepth = 0;

    // Defer firing m_OnSelectAssets while the mouse is held or a drag is in
    // progress so the inspector content stays visible (with its drop targets)
    // during an asset drag. The deferred check re-schedules itself each frame
    // while IsMouseDown() is true; if a drag is observed it discards the
    // pending paths, otherwise it fires them once the mouse releases.
    std::optional<std::vector<std::filesystem::path>> m_PendingInspectorSelectPaths;
    bool m_DragObservedDuringPress = false;

    void FirePendingInspectorSelect();
    void CheckPendingInspectorSelect();
    void ClearAssetItemSelectionForDatasetSwitch();
    std::shared_ptr<void> HoldInspectorAssetFireSuppression();

    // Phantom-row context-menu callback. Delegates the actual world walk +
    // schema-driven mutation to MissingAssetTracker (where ECS knowledge
    // already lives); this method just adapts the EditorContext + dirty hook.
    void OnPhantomRemoveFromScene(const GUID& guid);

    // Coalesce file-system driven refreshes into one UI refresh per frame.
    //
    // The watch callback runs on the watcher thread. It holds this state, not the
    // controller, and touches the controller only under Mutex while ControllerAlive is
    // set; the destructor clears ControllerAlive under Mutex. The refresh the callback
    // posts to the UI thread can outlive the controller and checks ControllerAlive there.
    struct FsEventInbox
    {
        std::mutex Mutex;
        std::vector<FileChangeEvent> Events;
        // SetAssetsRoot increments it, and each watch callback carries the value current
        // when it subscribed, so a callback of a previous root queues nothing.
        uint64_t RootGeneration = 0;
        // Written by the destructor on the UI thread; the refresh posted to the UI thread
        // reads it there without the lock.
        bool ControllerAlive = true;
    };
    std::shared_ptr<FsEventInbox> m_FsInbox = std::make_shared<FsEventInbox>();
    std::atomic<bool> m_FsRefreshPosted{false};
    void ProcessPendingFsEvents();

    // Weak reference to context; lifetime owned by EditorApplication
    const EditorContext* m_Context = nullptr;

    std::function<void(const std::filesystem::path&)> m_OnOpenAsset;
    std::function<void(const std::filesystem::path&)> m_OnEditAsset;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnAddToBookmarks;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnSelectAssets;
    std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> m_OnAddTag;
    std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> m_OnAssignTag;
    std::function<void()> m_OnOpenSettingsToTags;
    std::function<void(float)> m_OnRowHeightChanged;
    std::function<void()> m_OnAssetsListScrolled;
    std::string m_SearchQuery;
    
    // Cached truncation settings for live updates
    float m_TruncationThreshold = 128.0f;
    bool m_TruncationThresholdOverride = false; // true when using live value instead of prefs
    bool m_TruncationEnabled = true;
    bool m_TruncationEnabledOverride = false;
    bool m_TruncationPrefsLoaded = false;
    float m_ListFontSize = 13.0f;
    bool m_ListFontSizePrefsLoaded = false;
    bool m_SmartFoldersExpandedOnStartup = false;
    bool m_SmartFoldersStartupExpansionApplied = false;

    // Editor-level helper for native context menus in the Assets grid.
    std::unique_ptr<EditorContextMenu> m_ContextMenu;

    // Cached VCS UI settings to avoid per-row SettingsStore disk reads.
    std::string m_CachedVcsTypeId; // active provider TypeId ("" = none)
    VcsUiSettings m_CachedVcsUiSettings{};
    bool m_VcsUiDirty = true;

    struct VcsStatusRequest
    {
        std::filesystem::path path;
        std::string key;
    };
    std::unordered_map<std::string, VCSFileStatus> m_VcsStatusCache;
    std::unordered_set<std::string> m_VcsStatusQueued;
    std::vector<VcsStatusRequest> m_VcsStatusQueue;
    size_t m_VcsStatusQueueIndex = 0;
    std::atomic<bool> m_VcsStatusBatchPosted{false};

    // Expand tree ancestors and select the folder node matching dir.
    void SyncTreeSelectionToDirectory(const std::filesystem::path& dir);
    void ApplyInitialTreeExpansion();

    // Shared implementation for NavigateToAndSelectAsset / NavigateToAndSelectAssetSilent.
    void NavigateToAndSelectAssetImpl(const std::filesystem::path& assetPath,
                                      bool silent,
                                      std::function<void()> onSelected = {});
    void SelectAssetAfterNavigation(const std::filesystem::path& assetPath,
                                    std::shared_ptr<void> silentGuard,
                                    int attemptsRemaining,
                                    std::function<void()> onSelected);

    // Shared helpers used by grid interactions and context menus.
    void NavigateToDirectory(const std::filesystem::path& dir);
    /// `onNavigated` runs once the listing of `dir` is on screen: at once when `dir`
    /// is already listed, else after a worker scanned it. It does not run when a
    /// newer listing supersedes the navigation.
    void NavigateToDirectory(const std::filesystem::path& dir, std::function<void()> onNavigated);
    // Rescan `dir` from disk and rebuild grid/list now. Web has no file watcher,
    // so a RefreshFromProvider-only redraw leaves the imported file invisible.
    void RefreshDirectory(const std::filesystem::path& dir);
    void RevealImportedAssets(const std::filesystem::path& dest,
                              const std::vector<std::filesystem::path>& imported);
    void HandleDirectoryAction(const std::filesystem::path& dir,
                               EditorContextMenu::DirectoryAction action);
    void ImportFilesIntoDirectory(const std::filesystem::path& dir);
    void DeleteAssetPaths(const std::vector<std::filesystem::path>& paths);
    void CreateNewFolderInDirectory(const std::filesystem::path& dir);
    void CreateNewSceneInDirectory(const std::filesystem::path& dir);
    void CreateNewCSharpScriptInDirectory(const std::filesystem::path& dir);
    void CreateNewMaterialInDirectory(const std::filesystem::path& dir);
    void CreateNewAnimationLibraryInDirectory(const std::filesystem::path& dir);
    void CreateNewAnimationControllerInDirectory(const std::filesystem::path& dir);
    void CreateNewTimelineInDirectory(const std::filesystem::path& dir);
    void CreateNewClipSetInDirectory(const std::filesystem::path& dir);
    void CreateNewSpriteFramesInDirectory(const std::filesystem::path& dir);
    void CreateNewShaderGraphInDirectory(const std::filesystem::path& dir);
    void CreateNewSurfaceShaderInDirectory(const std::filesystem::path& dir);
    void CreateNewNavGridInDirectory(const std::filesystem::path& dir);
    void CreateNewNavMeshInDirectory(const std::filesystem::path& dir);
    // Shared scaffold for all "Create code asset" actions (C# + C++ component/system); the
    // per-type templates live in the CodeAssetDescriptor table (see CodeAssetTemplates.h).
    void CreateCodeAssetInDirectory(const std::filesystem::path& dir,
                                    const Editor::CodeAssetDescriptor& descriptor);

    void CreateNewSmartFolder();
    void ShowSmartFolderInGrid(const std::string& smartFolderId);
    void SelectPolyhavenCategory(const std::string& type); // "hdris" | "textures" | "models" -> shows subcategories in grid
    void SyncTreeToPolyhavenCategory(const std::string& type, const std::string& category);
    void SelectPolyhavenCategoryFilter(const std::string& type, const std::string& categoryFilter); // load assets for type+category

    // Smart-folder session state (selection, query cache, manager) lives in the
    // panel-owned SmartFolderController; the browser only presents its results.
    SmartFolderController* m_SmartFolderController = nullptr; // not owned
    bool m_ExpandFoldersOnLoad = false;

    // Polyhaven load-more: called from scroll callback when user is near bottom
    void TryLoadMorePolyhavenBatch(float scrollY, float contentHeight, float viewportHeight);
    void StartNextPolyhavenBatch();

    std::unique_ptr<PolyhavenService> m_Polyhaven;
    std::atomic<uint32_t> m_PolyhavenLoadGeneration{0};
    std::vector<PolyhavenManifestEntry> m_PolyhavenManifest;
    std::string m_PolyhavenTypeKey;
    /// Exclusive end index into m_PolyhavenManifest: slices [0, fetchedEnd) have been passed to MaterializeBatch.
    /// Tracks manifest progress, not row count (thumbnail downloads can temporarily skip rows).
    size_t m_PolyhavenManifestFetchedEnd = 0;
    std::atomic<bool> m_PolyhavenLoadMoreInProgress{false};

    // Shared flag so detached background threads can detect controller destruction.
    // Serializes worker posting with destruction of the controller/UI tree.
    std::shared_ptr<std::mutex> m_ScanPostMutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::atomic<bool>> m_Alive = std::make_shared<std::atomic<bool>>(true);

    // Helper to apply column widths to a list item cell
    // List view: store asset path per row for drag (replaces data-asset-path attribute)
    std::unordered_map<UIElement*, std::string> m_ListRowPathMap;
};

} // namespace GameEngine
