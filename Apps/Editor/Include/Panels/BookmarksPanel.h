#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/Interaction/DropTarget.h"
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <functional>
#include <cstddef>
#include <filesystem>
#include <optional>

namespace GameEngine {

namespace Editor { class UndoRedoService; }

// Constants for bookmarks persistence (file name and SettingsStore keys).
// Single place for names used in SaveBookmarks/LoadBookmarks/SaveRowHeight/LoadRowHeight.
namespace BookmarksKeys
{
    constexpr const char* kFileName = "Bookmarks.json";
    constexpr const char* kBookmarksKey = "bookmarks";
    constexpr const char* kRowHeightKey = "rowHeight";
}

class UIElement;
class ScrollView;
class Label;
class Button;
class TextField;
class ConfirmActionModal;
struct EditorContext;
class AssetsPanel;
class HierarchyPanel;

enum class BookmarkType {
    Asset,
    Scene,
    Entity
};

struct Bookmark {
    BookmarkType Type;
    std::string Reference;  // GUID for assets/scenes, "scenePath|entityId" for entities
    std::string Name;
    std::string IconPath;   // Relative path to icon or icon class name
    int Order = 0;

    bool operator==(const Bookmark& other) const {
        return Type == other.Type && Reference == other.Reference;
    }
};

class BookmarksPanel : public DockPanel, public UI::Interaction::IDropTarget {
public:
    std::string_view DeclaredTabIconClass() const override { return "book-icon"; }

    BookmarksPanel();
    ~BookmarksPanel() override;
    
    // Static callback for adding bookmarks from other panels
    static void SetOnAddBookmark(std::function<void(const Bookmark&)> cb);
    static void AddBookmarkStatic(const Bookmark& bookmark);
    
    // Set editor context for accessing asset registry, thumbnails, etc.
    void SetEditorContext(EditorContext* context);

    // Set undo service for undoable remove/reorder (set by EditorApplication).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    // Set panel references for navigation (set by EditorApplication)
    void SetAssetsPanel(AssetsPanel* panel) { m_AssetsPanel = panel; }
    void SetHierarchyPanel(HierarchyPanel* panel) { m_HierarchyPanel = panel; }

    // Callbacks for entity bookmark navigation (wired by EditorApplication)
    void SetGetCurrentScenePath(std::function<std::optional<std::filesystem::path>()> fn) { m_GetCurrentScenePath = std::move(fn); }
    void SetOnOpenScene(std::function<void(const std::filesystem::path&)> fn) { m_OnOpenScene = std::move(fn); }
    
    // Add a bookmark (called from drag-and-drop or other sources)
    void AddBookmark(const Bookmark& bookmark);
    // Insert a bookmark at a specific index (0 = top, size = after last)
    void AddBookmarkAt(const Bookmark& bookmark, size_t insertIndex);
    
    // Remove a bookmark by index
    void RemoveBookmark(size_t index);

    /** First index in m_Bookmarks that matches bookmark (type + normalized reference), or size() if not found. */
    size_t FindBookmarkIndex(const Bookmark& bookmark) const;
    /** Same as above but prefers preferredIndex when in range and that slot matches (so duplicates remove the clicked row). */
    size_t FindBookmarkIndex(const Bookmark& bookmark, size_t preferredIndex) const;
    /** Number of bookmarks (for bounds checks). */
    size_t GetBookmarkCount() const { return m_Bookmarks.size(); }

    // Reorder bookmarks (used by undo/redo and drag-and-drop).
    void ReorderBookmarks(int fromIndex, int toIndex);
    
    // Navigate to a bookmark (called when clicking a bookmark or dropping on Hierarchy)
    void NavigateToBookmark(size_t index);
    void NavigateToBookmark(const Bookmark& bookmark);
    
    // Set panel references for drag-and-drop (set by EditorApplication)
    void SetAssetsPanelForDrag(AssetsPanel* panel) { m_AssetsPanelForDrag = panel; }
    void SetHierarchyPanelForDrag(HierarchyPanel* panel) { m_HierarchyPanelForDrag = panel; }
    
    // Public methods for adding bookmarks (used by context menu and drag-and-drop)
    void HandleAssetDrop(const std::filesystem::path& assetPath);
    void HandleAssetDrop(const std::filesystem::path& assetPath, int insertIndex);
    /** Add multiple assets to bookmarks (e.g. multi-selection or drag multiple). Appends at end. */
    void HandleAssetDrops(const std::vector<std::filesystem::path>& assetPaths);
    /** Add multiple assets at a specific insert index (first at insertIndex, then insertIndex+1, ...). */
    void HandleAssetDrops(const std::vector<std::filesystem::path>& assetPaths, int insertIndex);
    void HandleEntityDrop(const std::string& scenePath, const std::string& entityId);
    
    // Refresh bookmarks (reload from disk and rebuild UI)
    // Call after asset scanning completes to validate bookmarks against updated registry
    void RefreshBookmarks();
    
    // Override to refresh bookmarks on first layout
    void OnPostLayout() override;

    // IDropTarget (bookmark reorder)
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

private:
    void BuildUI();
    void RebuildBookmarkRows();
    void CreateBookmarkRow(size_t index, UIElement* container);
    void SetupDragAndDrop(UIElement* row, UIElement* dragHandle, size_t index);
    void SetupDropHandlers(UIElement* container);
    void SaveBookmarks();
    void LoadBookmarks();
    void SaveRowHeight();
    void LoadRowHeight();
    
    // Navigation helpers
    void NavigateToAsset(const std::string& guid);
    void NavigateToScene(const std::string& guid);
    void NavigateToEntity(const std::string& scenePath, const std::string& entityId);
    void SelectEntityById(const std::string& entityId);

    // Drop handler for hierarchy entity drags
    void HandleEntityPayloadDrop(const std::vector<std::uint64_t>& treeIds, int insertIndex);
    
    // Icon helpers
    void UpdateBookmarkIcon(UIElement* iconEl, const Bookmark& bookmark);
    std::string GetDefaultIconForType(BookmarkType type) const;
    /** Resolves icon class for a bookmark; for Asset type, looks up metadata to pick script/scene/asset icon. */
    std::string GetDefaultIconForBookmark(const Bookmark& bookmark) const;

    // Drag-and-drop state
    UIElement* m_ListContainer = nullptr;
    ScrollView* m_ScrollView = nullptr;
    EditorContext* m_Context = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    
    // Track current workspace root to detect project changes
    std::filesystem::path m_CurrentWorkspaceRoot;
    
    // Search
    UIElement* m_SearchBar = nullptr;
    TextField* m_SearchField = nullptr;
    std::string m_SearchQuery;
    std::string m_SearchFieldScope{"all"};
    
    std::vector<Bookmark> m_Bookmarks;
    
    // Panel references for navigation and drag-and-drop
    AssetsPanel* m_AssetsPanel = nullptr;
    HierarchyPanel* m_HierarchyPanel = nullptr;
    AssetsPanel* m_AssetsPanelForDrag = nullptr;
    HierarchyPanel* m_HierarchyPanelForDrag = nullptr;

    // Entity navigation callbacks (wired by EditorApplication)
    std::function<std::optional<std::filesystem::path>()> m_GetCurrentScenePath;
    std::function<void(const std::filesystem::path&)> m_OnOpenScene;

    // Confirm dialog for opening a scene when navigating to an entity
    ConfirmActionModal* m_OpenSceneConfirm = nullptr; // not owned (child UIElement)
    std::filesystem::path m_PendingEntityNavScenePath;
    std::string m_PendingEntityNavEntityId;
    
    // Static callback for adding bookmarks
    static std::function<void(const Bookmark&)> s_OnAddBookmark;
    
    // Drag state (DragDropManager drives hover/commit; we only track pending + source row for class/preview)
    bool m_DragPending = false;
    size_t m_DragPendingIndex = 0;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    int m_DragPendingMods = 0; // mods at mouse down, used for click (selection/navigate) on mouse up when no drag
    UIElement* m_DragSourceRow = nullptr;
    UIElement* m_InsertionIndicator = nullptr;
    int m_AssetDropInsertIndex = -1; // Insert position when dragging asset over panel (-1 = append)
    
    // Selection state (multi-select: Ctrl/Cmd+click add/toggle, Shift+click range)
    std::set<size_t> m_SelectedIndices;
    size_t m_SelectionAnchor = 0;
    void ApplySelectionVisuals();
    
    // Row height (adjustable via Ctrl+Scroll)
    float m_RowHeight = 32.0f;
    
    // Track if initial refresh has been done (on first layout)
    bool m_InitialRefreshDone = false;
};

} // namespace GameEngine
