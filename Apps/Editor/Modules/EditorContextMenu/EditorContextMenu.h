#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine {

class INativeContextMenu;
namespace Platform { class Window; }

// EditorContextMenu: small helper around INativeContextMenu/ContextMenuBuilder
// to provide common editor popup menus (initially for the Assets browser).
	class EditorContextMenu {
	public:
	    enum class DirectoryAction
	    {
	        NewFolder,
	        NewSmartFolder,
	        Refresh,
            CreateScene,
            CreateCSharpScript,
            CreateMaterial,
            CreateAnimationLibrary,
            CreateAnimationController,
            CreateTimeline,
            CreateClipSet,
            CreateSpriteFrames,
            CreateShaderGraph,
            CreateSurfaceShader,
            CreateNavGrid,
            CreateNavMesh,
            CreateGameSystem,
            CreateEntitySystem,
            CreateComponent,
            CreateCppComponent,
            CreateCppGameSystem,
            CreateCppEntitySystem,
            Import,
            Delete
	    };
	
	    enum class SmartFolderAction
	    {
	        NewSmartFolder,
	        Delete
	    };

		explicit EditorContextMenu(Platform::Window* window);
	    ~EditorContextMenu();
	
	    void SetWindow(Platform::Window* window);
	
	    // Show a context menu for a single asset in the Assets grid.
	    // pathsForBookmarks: when "Add to Bookmarks" is chosen, this list is passed to onAddToBookmarks (e.g. current selection or single item).
	    void ShowAssetMenu(const std::filesystem::path& assetPath,
	                       bool isDirectory,
	                       float x, float y,
	                       const std::function<void(const std::filesystem::path&, bool /*isDirectory*/)>& onOpenAction,
	                       const std::function<void(const std::filesystem::path&)>& onEditAction = {},
	                       const std::vector<std::filesystem::path>* pathsForBookmarks = nullptr,
	                       const std::function<void(const std::vector<std::filesystem::path>&)>& onAddToBookmarks = {},
	                       const std::function<void(const std::vector<std::filesystem::path>&)>& onDelete = {},
	                       const std::function<void(const std::filesystem::path&)>& onRename = {});

	    void SetOnAddTag(std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> cb);
	    /// Assign a tag to paths directly (no modal). Used by Tags submenu items.
	    void SetOnAssignTag(std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> cb);
	    /// Open Settings → Tags (e.g. "All Tags…" in Tags submenu).
	    void SetOnOpenSettingsToTags(std::function<void()> cb);

	    // Show a context menu for the current directory (e.g., when right-clicking
	    // empty space in the grid). This exposes folder-oriented actions like
	    // "Show in Explorer/Finder", "Copy Path" and editor-specific actions such
	    // as "New Folder" or "Refresh".
	    void ShowDirectoryMenu(const std::filesystem::path& directoryPath,
	                           float x, float y,
	                           const std::function<void(const std::filesystem::path&, DirectoryAction)>& onDirectoryAction,
	                           bool includeDelete = false);

	    void ShowSmartFolderMenu(float x, float y,
	                             const std::function<void(SmartFolderAction)>& onAction);

	    // Show a context menu for a phantom row (a missing-asset reference
	    // injected into the AssetsPanel). The asset doesn't exist on disk, so
	    // this menu offers recovery actions instead of the normal Open/Edit
	    // entries: clear the broken reference from the scene and copy the GUID
	    // for diagnostic use. The displayPath is informational (e.g. shown as
	    // a non-clickable header item to remind the user what's missing).
	    void ShowPhantomAssetMenu(const std::filesystem::path& displayPath,
	                              float x, float y,
	                              const std::function<void()>& onRemoveFromScene,
	                              const std::function<void()>& onCopyGuid);

    // Set callbacks for git operations that require UI dialogs
    void SetOnShowCommitDialog(std::function<void(const std::string&)> callback);
    void SetOnShowVcsLog(std::function<void(const std::filesystem::path&)> callback);
    void SetOnVcsRevert(std::function<void(const std::filesystem::path&)> callback);
    void SetOnShowDiff(std::function<void(const std::filesystem::path&)> callback);
	
    // Shows `path` (an asset path, resolved through the asset manager when relative) selected
    // in the operating system's file manager: the asset menu's Show in Explorer.
    static void ShowInFileManager(const std::filesystem::path& path);
    // Puts `path` on the clipboard: the asset menu's Copy Full Path.
    static void CopyPathToClipboard(const std::filesystem::path& path);

	private:
    void EnsureMenu();
    void OpenAsset(const std::filesystem::path& path);

    Platform::Window* m_Window = nullptr; // not owned
    std::unique_ptr<INativeContextMenu> m_Menu;
    std::filesystem::path m_AssetPath;
    bool m_IsDirectory = false;
    std::vector<std::filesystem::path> m_PathsForBookmarks;
    std::function<void(const std::filesystem::path&, bool)> m_OnOpenAction;
    std::function<void(const std::filesystem::path&)> m_OnEditAction;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnAddToBookmarks;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnDelete;
    std::function<void(const std::filesystem::path&)> m_OnRename;
    std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> m_OnAddTag;
    std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> m_OnAssignTag;
    std::function<void()> m_OnOpenSettingsToTags;
    std::function<void(const std::filesystem::path&, DirectoryAction)> m_OnDirectoryAction;
    std::function<void(SmartFolderAction)> m_OnSmartFolderAction;
    std::function<void()> m_OnPhantomRemoveFromScene;
    std::function<void()> m_OnPhantomCopyGuid;
    
    // VCS UI hooks (implemented by the Editor UI layer)
    std::function<void(const std::string&)> m_OnShowCommitDialog;
    std::function<void(const std::filesystem::path&)> m_OnShowVcsLog;
    std::function<void(const std::filesystem::path&)> m_OnVcsRevert;
    std::function<void(const std::filesystem::path&)> m_OnShowDiff;
};

} // namespace GameEngine
