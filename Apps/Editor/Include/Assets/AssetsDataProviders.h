#pragma once

#include <functional>
#include <vector>
#include <string>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include "AssetCore/GUID.h"
#include "UI/Controls/TreeView.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ListView.h"

namespace GameEngine {

enum class AssetsSearchField
{
    All,
    Name,
    Type,
    Extension,
    Path,
    Tag
};

class SmartFolderManager;
class AssetRegistry;
struct SmartFolder;

// Simple providers; Tree still stubbed, Grid now reads real filesystem data

class AssetsTreeDataProvider : public TreeChangeTrackingProvider {
public:
    AssetsTreeDataProvider();
    ~AssetsTreeDataProvider() override = default;

    int GetRootCount() const override;
    TreeId GetRootId(int index) const override;
    int GetChildCount(TreeId parent) const override;
    TreeId GetChildId(TreeId parent, int index) const override;
    const char* GetLabel(TreeId id) const override;
    bool IsExpandable(TreeId id) const override;

    // Real data
    void SetRootsFromDirectory(const std::filesystem::path& dir);
    std::filesystem::path GetPath(TreeId id) const;
	    TreeId FindIdForPath(const std::filesystem::path& path) const;

    // Notify the provider that the direct children of a directory may have changed.
    // Returns true if a known node was invalidated.
    bool OnDirectoryChanged(const std::filesystem::path& dir);
    // Notify the provider that a directory path was renamed/moved.
    // Returns true if a known node was updated.
    bool OnPathRenamed(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);

    // Smart folders support
    void SetSmartFolderManager(SmartFolderManager* manager);
    void RefreshSmartFolders();
    bool IsSmartFolder(TreeId id) const;
    bool IsSmartFoldersSection(TreeId id) const;
    bool IsAssetsRoot(TreeId id) const; // true for the top-level Assets folder only
    std::string GetSmartFolderId(TreeId treeId) const; // Returns empty string if not a smart folder
    TreeId GetTreeIdForSmartFolder(const std::string& smartFolderId) const; // Returns 0 if not found
    void SetSmartFoldersAtTop(bool atTop); // true = show at top, false = show at bottom
    bool GetSmartFoldersAtTop() const { return m_SmartFoldersAtTop; }
    
    // Special IDs for smart folders section
    static constexpr TreeId kSmartFoldersSectionId = 0xF000000000000000ull;
    static constexpr TreeId kSmartFolderIdBase = 0xF000000000000001ull;

    // Online Assets section (e.g. Poly Haven)
    static constexpr TreeId kOnlineAssetsSectionId = 0xE000000000000000ull;
    static constexpr TreeId kPolyhavenId = 0xE000000000000001ull;
    static constexpr TreeId kPolyhavenCategoryIdBase = 0xE000000000000002ull;  // HDRIs, Textures, Models (type nodes)
    static constexpr TreeId kPolyhavenCategoryChildIdBase = 0xE000000001000000ull; // All, Aerial, Brick, ... (category dirs)

    bool IsOnlineAssetsSection(TreeId id) const;
    bool IsPolyhaven(TreeId id) const;
    bool IsPolyhavenCategory(TreeId id) const;  // true for type nodes: HDRIs, Textures, Models
    bool IsPolyhavenCategoryChild(TreeId id) const;  // true for category dirs: All, Aerial, Brick, ...
    std::string GetPolyhavenCategory(TreeId id) const; // "hdris" | "textures" | "models" or empty (for type nodes)
    void GetPolyhavenTypeAndCategory(TreeId id, std::string& outType, std::string& outCategorySlug) const; // for category child nodes
    void SetPolyhavenTypeCategories(const std::string& type, const std::vector<std::pair<std::string, std::string>>& slugAndLabel);

    void SetOnlineAssetsAtTop(bool atTop);
    bool GetOnlineAssetsAtTop() const { return m_OnlineAssetsAtTop; }

    void SetOnlineAssetsEnabled(bool enabled);
    bool GetOnlineAssetsEnabled() const { return m_OnlineAssetsEnabled; }

    void SetPolyHavenEnabled(bool enabled);
    bool GetPolyHavenEnabled() const { return m_PolyHavenEnabled; }

private:
    void EnsureChildrenBuilt(TreeId parent) const; // lazy child population
    void RefreshOnlineAssets();
    TreeId m_NextPolyhavenCategoryChildId = kPolyhavenCategoryChildIdBase;
    struct Node { TreeId id; std::string label; std::filesystem::path path; std::string key; std::vector<TreeId> children; bool expandable = false; bool isSmartFolder = false; std::string smartFolderId; bool isOnlineAsset = false; std::string onlineAssetCategory; std::string polyhavenCategorySlug; };
    std::vector<Node> m_Nodes;
    std::vector<TreeId> m_Roots;
    std::filesystem::path m_RootDir;
    TreeId m_NextId = 1;
    mutable std::unordered_set<TreeId> m_Populated; // which nodes have had children materialized
    // Fast lookup maps
    std::unordered_map<TreeId, size_t> m_IndexById;
    std::unordered_map<std::string, TreeId> m_IdByPath;
    
    // Smart folders
    SmartFolderManager* m_SmartFolderManager = nullptr;
    std::vector<TreeId> m_SmartFolderIds; // TreeIds for smart folder nodes
    bool m_SmartFoldersAtTop = true; // true = show at top of tree, false = at bottom

    // Online Assets (e.g. Poly Haven) - nodes stored in m_Nodes with isOnlineAsset = true
    bool m_OnlineAssetsAtTop = false; // false = show at bottom of tree (default)
    bool m_OnlineAssetsEnabled = true; // true = show online assets section
    bool m_PolyHavenEnabled = true; // true = show Poly Haven under online assets
};

class AssetsGridDataProvider : public GridChangeTrackingProvider {
public:
    using TagSortKeyResolver = std::function<std::string(const std::filesystem::path&)>;

    AssetsGridDataProvider();
    ~AssetsGridDataProvider() override = default;

    // IGridDataProvider
    int GetItemCount() const override;
    GridId GetItemId(int index) const override;
    const char* GetLabel(GridId id) const override;
    uint64_t GetIcon(GridId id) const override;
    const char* GetTypeKey(GridId id) const override;
    void ApplySort(const SortDescriptor& desc) override;
    void ApplyGrouping(const GroupDescriptor& desc) override;

    // Load items from a directory and apply the active sort/display preferences.
    // Synchronous; a scan still in flight (ScanDirectory) does not apply after it.
    void LoadDirectory(const std::filesystem::path& dir);
    
    // Load items from a custom list of file paths (for smart folders)
    void LoadFiles(const std::vector<std::filesystem::path>& files,
                   AssetRegistry* registry = nullptr);

    // Load remote items with custom display labels (e.g. Poly Haven API results). Path = local cache path for thumbnail.
    void LoadRemoteItems(const std::vector<std::pair<std::filesystem::path, std::string>>& pathAndLabelPairs,
                         const std::string& typeKey);

    // Append more remote items (e.g. next batch when scrolling). Only valid when already showing remote items (no directory).
    void AppendRemoteItems(const std::vector<std::pair<std::filesystem::path, std::string>>& pathAndLabelPairs,
                           const std::string& typeKey);

    // Load virtual folder entries for online asset navigation (no real filesystem).
    // Each entry is {label, virtualPath}. If parentVirtualPath is non-empty, a ".." entry is added.
    void LoadVirtualFolders(const std::vector<std::pair<std::string, std::filesystem::path>>& folders,
                            const std::filesystem::path& parentVirtualPath = {});

    // Limit navigation so "go up" does not traverse above this root
    void SetRootLimit(const std::filesystem::path& root) { m_RootLimit = root; }

    // Set the project assets root for project-wide search
    void SetAssetsRoot(const std::filesystem::path& root);
    void SetAssetRegistry(AssetRegistry* registry);

    // Editor-specific helpers
    const std::filesystem::path& GetCurrentDirectory() const { return m_CurrentDir; }
    std::filesystem::path GetPath(GridId id) const;
    GridId FindIdForPath(const std::filesystem::path& path) const;
    bool IsDirectory(GridId id) const;
    // True when the entry is a phantom row representing a scene asset reference
    // whose target file doesn't exist on disk. Phantom rows are injected into
    // the visible listing by the missing-assets workflow; they should render
    // ghosted, suppress drag, and offer a context menu of recovery actions.
    bool IsPhantom(GridId id) const;
    // Return the GUID the phantom row represents (Null for non-phantom rows).
    GUID GetPhantomGuid(GridId id) const;

    // Provide the missing-asset tracker used to inject phantom rows. Pointer
    // is borrowed; lifetime managed by EditorApplication. Pass nullptr to
    // disable phantom injection for this provider.
    void SetMissingAssetTracker(class MissingAssetTracker* tracker);
    bool TryGetImageDimensions(GridId id, int& outWidth, int& outHeight) const;
    bool TryGetSizeBytes(GridId id, uintmax_t& outSizeBytes) const;
    bool TryGetLastWriteTime(GridId id, std::filesystem::file_time_type& outLastWriteTime) const;
    void SetSearchQuery(const std::string& query);
    void SetSearchField(AssetsSearchField field);
    void RefreshSearchIndex();
    void SetFoldersFirst(bool foldersFirst);

    // Optional resolver for sort-by-tag: returns a sort key string for the asset path (empty = no tags).
    // It runs on the worker that scans a folder, so it must be thread-safe.
    void SetTagSortKeyResolver(TagSortKeyResolver resolver);
    const TagSortKeyResolver& GetTagSortKeyResolver() const { return m_TagSortKeyResolver; }

private:
    struct Item
    {
        GridId id;
        std::string label;
        std::string type;
        uint64_t icon = 0;
        std::filesystem::path path;
        bool isDir = false;
        bool isUp = false;
        bool isPhantom = false;
        GUID  phantomGuid;

        // Cached metadata (loaded once per directory snapshot).
        int imgWidth = 0;
        int imgHeight = 0;
        bool hasImageDimensions = false;

        uintmax_t sizeBytes = 0;
        bool hasSizeBytes = false;

        std::filesystem::file_time_type lastWriteTime{};
        bool hasLastWriteTime = false;

        // Search keys are built once with the directory snapshot. Keeping the
        // normalized fields here avoids rebuilding path/metadata strings for
        // every item on every keystroke.
        std::string searchName;
        std::string searchType;
        std::string searchExtension;
        std::string searchPath;
        std::string searchTag;
        std::string searchAll;
    };

public:
    // A folder listing built off the main thread: the file-system walk and each
    // entry's metadata and search keys, but none of the provider's state.
    struct DirectoryScan
    {
        std::filesystem::path Dir;
        bool Exists = false;
        std::vector<Item> Dirs;  // sorted by name
        std::vector<Item> Files; // sorted by name
    };
    // Walks `dir` for ApplyDirectoryScan. Reads only the file system and `tags`,
    // so a worker can run it while the provider serves the current listing.
    static DirectoryScan ScanDirectory(const std::filesystem::path& dir, const TagSortKeyResolver& tags);
    // Every request that replaces the listing takes a new generation; a scan
    // applies only if no newer request was made since it started.
    uint64_t NextListingGeneration() { return ++m_ListingGeneration; }
    uint64_t GetListingGeneration() const { return m_ListingGeneration; }
    // Replaces the listing with a finished scan and applies the active filter
    // and sort. Returns false, and changes nothing, for a superseded scan.
    bool ApplyDirectoryScan(DirectoryScan scan, uint64_t generation);

    // The project-wide search snapshot (every asset and folder under the
    // assets root), built off the main thread ahead of the first search.
    struct ProjectSearchScan
    {
        std::vector<Item> Items;
    };
    // Reads only the registry (under its own lock), the file system and `tags`.
    static ProjectSearchScan ScanProjectForSearch(const std::filesystem::path& assetsRoot,
                                                  const AssetRegistry* registry,
                                                  const TagSortKeyResolver& tags);
    bool HasProjectSearchSnapshot() const { return m_ProjectSearchItemsValid; }
    // Every invalidation of the snapshot takes a new generation.
    uint64_t GetProjectSearchGeneration() const { return m_ProjectSearchGeneration; }
    // Installs a finished scan unless the snapshot was invalidated or built
    // since the scan started. Returns true when the shown items changed.
    bool ApplyProjectSearchScan(ProjectSearchScan scan, uint64_t generation);

private:
    // Append phantom rows for missing-asset references whose authored path's
    // parent directory matches m_CurrentDir. Called from ApplyDirectoryScan after
    // the filesystem walk; safe to call with no tracker bound (no-op).
    void InjectPhantomsForCurrentDir(std::unordered_set<GridId>& usedIds);
    void ApplyFilter();
    static void IndexItemForSearch(Item& item, const TagSortKeyResolver& tags);
    void BuildProjectSearchSnapshot();
    void InvalidateProjectSearchSnapshot();
    static void LoadSearchFilesystemSupplement(const std::filesystem::path& dir,
                                               const std::unordered_set<std::string>& registeredPaths,
                                               const TagSortKeyResolver& tags,
                                               std::vector<Item>& outDirs,
                                               std::vector<Item>& outFiles,
                                               std::unordered_set<GridId>& usedIds);
    std::vector<Item> m_Items;
    std::vector<Item> m_AllItems;
    std::vector<Item> m_ProjectSearchItems;
    std::unordered_map<GridId, size_t> m_IndexById;
    std::unordered_map<std::string, GridId> m_IdByPath;
    SortDescriptor m_Sort{}; GroupDescriptor m_Group{};
    std::filesystem::path m_CurrentDir;
    std::filesystem::path m_RootLimit;
    std::filesystem::path m_AssetsRoot;
    std::string m_FilterLower;
    bool m_ProjectSearchItemsValid = false;
    uint64_t m_ProjectSearchGeneration = 0;
    AssetsSearchField m_SearchField = AssetsSearchField::All;
    bool m_FoldersFirst = true;
    TagSortKeyResolver m_TagSortKeyResolver;
    uint64_t m_ListingGeneration = 0;
    AssetRegistry* m_AssetRegistry = nullptr;
    class MissingAssetTracker* m_MissingAssetTracker = nullptr;
    // Phantom IDs come from this base + a counter so they don't collide with
    // hashed real-file IDs (which clear the high bit) or the reserved sentinels.
    static constexpr GridId kPhantomEntryIdBase = 0x9000000000000000ull;
    static constexpr GridId kUpEntryId = 0x8000000000000001ull;
    static constexpr GridId kLoadingPlaceholderId = 0x8000000000000002ull;
    void RebuildIndexById();
};

// Adapter to make AssetsGridDataProvider work with ListView
class AssetsListDataProvider : public IListDataProvider {
public:
    AssetsListDataProvider(AssetsGridDataProvider* gridProvider);
    ~AssetsListDataProvider() override = default;

    // IListDataProvider
    int GetItemCount() const override;
    ListId GetItemId(int index) const override;
    float GetItemHeight(int index) const override;
    void ConsumeChanges(uint64_t sinceVersion, ListChangeSet& out) const override;
    // Forward the change-tracking version from the backing grid provider so the
    // Assets panel's list mode participates in the UIManager change pump (C-8).
    uint64_t GetChangeVersion() const override;

    // Helper methods to access underlying grid provider data
    std::filesystem::path GetPath(ListId id) const;
    bool IsDirectory(ListId id) const;
    const char* GetLabel(ListId id) const;
    const char* GetTypeKey(ListId id) const;

    // Row height control (for the item resize gesture)
    void SetItemHeight(float height) { m_ItemHeight = height; }
    float GetItemHeight() const { return m_ItemHeight; }

private:
    AssetsGridDataProvider* m_GridProvider; // not owned
    float m_ItemHeight = 24.0f; // Default row height for list items
};

} // namespace GameEngine
