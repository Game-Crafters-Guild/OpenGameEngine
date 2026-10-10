#include "Assets/AssetsDataProviders.h"
#include "Assets/AssetPathKey.h"
#include "Assets/AssetRegistry.h"
#include "MissingAssetTracker.h"
#include "UI/SmartFolder/SmartFolderManager.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "Types/StringUtils.h"
#include "VCSIntegration/VCSFileStatus.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include "Logger/Logger.h"
#include <cctype>

#ifdef GE_HAVE_STB
#include <stb_image.h>
#endif



namespace GameEngine {

namespace {

// Get sort priority for VCS status (lower number = higher priority)
static int GetStatusPriority(VCSFileStatus status)
{
    switch (status)
    {
    case VCSFileStatus::Conflict:        return 0;  // Conflicts first
    case VCSFileStatus::Modified:        return 1;  // Modified files
    case VCSFileStatus::Added:           return 2;  // Added files
    case VCSFileStatus::Deleted:         return 3;  // Deleted files
    case VCSFileStatus::LockedByMe:      return 4;  // Locked by me
    case VCSFileStatus::LockedByOthers:  return 5;  // Locked by others
    case VCSFileStatus::ServerHasChanges: return 6; // Outdated
    case VCSFileStatus::Unversioned:     return 7;  // Untracked
    case VCSFileStatus::NotConfigured:   return 8;  // Not configured
    case VCSFileStatus::Ignored:         return 9;  // Ignored
    case VCSFileStatus::Clean:           return 10; // Clean files last
    default:                             return 11; // Unknown
    }
}

// Helper: Check if file is an image and get its dimensions
static bool GetImageDimensions(const std::filesystem::path& path, int& outWidth, int& outHeight)
{
#ifdef GE_HAVE_STB
    if (!path.has_extension())
        return false;
    
    std::string ext = path.extension().string();
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" && 
        ext != ".tga" && ext != ".gif" && ext != ".hdr" && ext != ".psd")
        return false;
    
    int w = 0, h = 0, comp = 0;
    if (stbi_info(path.string().c_str(), &w, &h, &comp))
    {
        outWidth = w;
        outHeight = h;
        return true;
    }
#else
    (void)path;
    (void)outWidth;
    (void)outHeight;
#endif
    return false;
}

static std::string FormatByteSize(uintmax_t bytes)
{
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    if (bytes >= (uintmax_t)gb)
    {
        oss << std::setprecision(1) << (bytes / gb) << " Gb";
    }
    else if (bytes >= (uintmax_t)mb)
    {
        oss << std::setprecision(1) << (bytes / mb) << " Mb";
    }
    else if (bytes >= (uintmax_t)kb)
    {
        oss << std::setprecision(0) << (bytes / kb) << " Kb";
    }
    else
    {
        oss << bytes << " B";
    }
    return oss.str();
}

static std::string FormatTime(const std::filesystem::file_time_type& ft)
{
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
    std::time_t tt = system_clock::to_time_t(sctp);
    std::tm tm{};
    auto localTimeOk = [&]() -> bool {
#if defined(_WIN32)
        return localtime_s(&tm, &tt) == 0;
#else
        return localtime_r(&tt, &tm) != nullptr;
#endif
    }();
    if (!localTimeOk)
        return std::string();
    std::ostringstream oss;
    oss << std::put_time(&tm, "%d %b %Y %H:%M");
    return oss.str();
}

static bool EqualsAsciiCaseInsensitive(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto ac = static_cast<unsigned char>(a[i]);
        const auto bc = static_cast<unsigned char>(b[i]);
        if (std::tolower(ac) != std::tolower(bc))
            return false;
    }
    return true;
}

static std::string DisplayNameForRootDirectory(const std::filesystem::path& dir)
{
    const std::string fallback = dir.filename().string().empty() ? dir.string() : dir.filename().string();
    if (dir.empty() || dir.filename().empty())
        return fallback;

    const std::filesystem::path parent = dir.parent_path();
    if (parent.empty())
        return fallback;

    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec))
        return fallback;

    const std::string requestedName = dir.filename().string();
    for (auto it = std::filesystem::directory_iterator(parent, ec); !ec && it != std::filesystem::end(it); ++it)
    {
        const std::string actualName = it->path().filename().string();
        if (EqualsAsciiCaseInsensitive(actualName, requestedName))
            return actualName;
    }
    return fallback;
}
} // namespace

// --- AssetsTreeDataProvider -------------------------------------------------
AssetsTreeDataProvider::AssetsTreeDataProvider() {
    // Start empty; roots will be populated via SetRootsFromDirectory()
}

int AssetsTreeDataProvider::GetRootCount() const { return static_cast<int>(m_Roots.size()); }
TreeId AssetsTreeDataProvider::GetRootId(int index) const { return m_Roots[index]; }
int AssetsTreeDataProvider::GetChildCount(TreeId parent) const {
    EnsureChildrenBuilt(parent);
    auto it = m_IndexById.find(parent);
    if (it == m_IndexById.end())
        return 0;
    return static_cast<int>(m_Nodes[it->second].children.size());
}
TreeId AssetsTreeDataProvider::GetChildId(TreeId parent, int index) const {
    EnsureChildrenBuilt(parent);
    auto it = m_IndexById.find(parent);
    if (it == m_IndexById.end())
        return 0;
    const auto& ch = m_Nodes[it->second].children;
    if (index < 0 || index >= (int)ch.size())
        return 0;
    return ch[(size_t)index];
}
const char* AssetsTreeDataProvider::GetLabel(TreeId id) const {
    auto it = m_IndexById.find(id);
    if (it != m_IndexById.end())
        return m_Nodes[it->second].label.c_str();
    static const char* kUnknown = ""; return kUnknown;
}
bool AssetsTreeDataProvider::IsExpandable(TreeId id) const {
    auto it = m_IndexById.find(id);
    if (it != m_IndexById.end())
        return m_Nodes[it->second].expandable;
    return false;
}

void AssetsTreeDataProvider::SetRootsFromDirectory(const std::filesystem::path& dir) {
    m_RootDir = dir;
    m_Nodes.clear();
    m_Roots.clear();
    m_Populated.clear();
    m_IndexById.clear();
    m_IdByPath.clear();
    m_NextId = 1;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        MarkAllChanged();
        return;
    }

    auto hasSubdirs = [](const std::filesystem::path& parent){
        std::error_code ec2;
        for (auto it = std::filesystem::directory_iterator(parent, ec2); !ec2 && it != std::filesystem::end(it); ++it)
        {
            if (it->is_directory(ec2))
                return true;
        }
        return false;
    };

    // Create a synthetic root node representing the provided directory
    Node root{};
    root.id = m_NextId++;
    root.label = DisplayNameForRootDirectory(dir);
    root.path = dir;
    root.key = AssetPathKey(dir);
    root.expandable = hasSubdirs(dir);
    m_Nodes.push_back(root);
    m_Roots.push_back(root.id);
    m_IndexById[root.id] = m_Nodes.size() - 1;
    if (!root.key.empty())
        m_IdByPath[root.key] = root.id;

    RefreshOnlineAssets();
    MarkAllChanged();
}

std::filesystem::path AssetsTreeDataProvider::GetPath(TreeId id) const {
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return {};
    return m_Nodes[it->second].path;
}

TreeId AssetsTreeDataProvider::FindIdForPath(const std::filesystem::path& path) const {
    const std::string key = AssetPathKey(path);
    if (key.empty())
        return 0;
    auto it = m_IdByPath.find(key);
    if (it == m_IdByPath.end())
        return 0;
    return it->second;
}

bool AssetsTreeDataProvider::OnDirectoryChanged(const std::filesystem::path& dir) {
    const TreeId id = FindIdForPath(dir);
    if (id == 0)
        return false;
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    Logger::Log::Debug("AssetsTreeDataProvider::OnDirectoryChanged dir='{}' id={}", dir.string(), (int)id);
    // Invalidate cached children for this node so the next query will rebuild
    // its children list from a fresh filesystem snapshot.
    m_Populated.erase(id);
    m_Nodes[it->second].expandable = true; // conservatively assume it may have children now
    MarkAllChanged();
    return true;
	}

bool AssetsTreeDataProvider::OnPathRenamed(const std::filesystem::path& oldPath,
                                          const std::filesystem::path& newPath)
{
    if (oldPath.empty() || newPath.empty())
        return false;

    const std::string oldKey = AssetPathKey(oldPath);
    auto itId = m_IdByPath.find(oldKey);
    if (itId == m_IdByPath.end())
        return false;

    const TreeId id = itId->second;
    auto itIndex = m_IndexById.find(id);
    if (itIndex == m_IndexById.end())
        return false;

    // Update this node's path and label, preserving its TreeId so expansion state remains stable.
    Node& renamed = m_Nodes[itIndex->second];
    renamed.path = newPath;
    renamed.label = newPath.filename().string();
    renamed.key = AssetPathKey(newPath);

    // Best-effort: update any cached descendants' paths so future lookups work and IDs remain stable.
    // This is rare and bounded by the number of materialized nodes.
    for (Node& n : m_Nodes)
    {
        std::error_code ecRel;
        const auto rel = std::filesystem::relative(n.path, oldPath, ecRel);
        if (ecRel)
        {
            continue;
        }
        const std::string relStr = rel.generic_string();
        if (relStr.empty() || relStr == "." || relStr.rfind("..", 0) == 0)
        {
            continue; // not within the renamed subtree
        }
        n.path = newPath / rel;
        n.key = AssetPathKey(n.path);
    }

    // Rebuild path->id map (cheap relative to directory traversal; avoids subtle key conflicts).
    m_IdByPath.clear();
    for (const Node& n : m_Nodes)
    {
        if (!n.key.empty())
            m_IdByPath[n.key] = n.id;
    }

    // Invalidate the renamed directory and its parent so child lists are rebuilt on next query.
    m_Populated.erase(id);
    if (!renamed.key.empty())
        m_IdByPath[renamed.key] = id;
    MarkAllChanged();
    return true;
}

void AssetsTreeDataProvider::EnsureChildrenBuilt(TreeId parent) const {
    auto* self = const_cast<AssetsTreeDataProvider*>(this);
    auto itIndex = self->m_IndexById.find(parent);
    if (itIndex == self->m_IndexById.end())
    {
        return;
    }

    const size_t parentIndex = itIndex->second;
    
    // Skip smart folder nodes - they don't have filesystem children
    if (self->m_Nodes[parentIndex].isSmartFolder)
    {
        return;
    }
    // Skip online asset nodes - children are built in RefreshOnlineAssets
    if (self->m_Nodes[parentIndex].isOnlineAsset)
    {
        return;
    }

    bool needsRebuild = false;
    if (m_Populated.find(parent) == m_Populated.end())
    {
        needsRebuild = true;
    }
    else
    {
        // Verify cached children still exist as directories (best-effort).
        const auto& cachedChildren = self->m_Nodes[parentIndex].children;
        for (TreeId childId : cachedChildren)
        {
            auto itChild = self->m_IndexById.find(childId);
            if (itChild == self->m_IndexById.end())
            {
                needsRebuild = true;
                break;
            }
            const auto& childPath = self->m_Nodes[itChild->second].path;
            std::error_code ec;
            if (!std::filesystem::exists(childPath, ec) || !std::filesystem::is_directory(childPath, ec))
            {
                needsRebuild = true;
                break;
            }
        }
    }

    if (!needsRebuild)
    {
        return;
    }

    const std::filesystem::path parentPath = self->m_Nodes[parentIndex].path;

    std::error_code ecDir;
    if (!std::filesystem::exists(parentPath, ecDir) || !std::filesystem::is_directory(parentPath, ecDir))
    {
        self->m_Nodes[parentIndex].children.clear();
        self->m_Nodes[parentIndex].expandable = false;
        m_Populated.insert(parent);
        return;
    }

    auto hasSubdirs = [](const std::filesystem::path& parentPath) -> bool
    {
        std::error_code ec2;
        for (auto it = std::filesystem::directory_iterator(parentPath, ec2); !ec2 && it != std::filesystem::end(it); ++it)
        {
            if (it->is_directory(ec2))
                return true;
        }
        return false;
    };

    std::vector<std::filesystem::path> dirs;
    dirs.reserve(32);
    std::error_code ec2;
    for (auto it = std::filesystem::directory_iterator(parentPath, ec2); !ec2 && it != std::filesystem::end(it); ++it)
    {
        if (it->is_directory(ec2))
            dirs.push_back(it->path());
    }

    std::sort(dirs.begin(), dirs.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
              { return a.filename().string() < b.filename().string(); });

    std::vector<TreeId> newChildren;
    newChildren.reserve(dirs.size());

    // Directories first
    for (const auto& p : dirs)
    {
        const std::string key = AssetPathKey(p);
        TreeId childId = 0;
        auto itExisting = self->m_IdByPath.find(key);
        if (itExisting != self->m_IdByPath.end())
        {
            childId = itExisting->second;
            auto itChildIndex = self->m_IndexById.find(childId);
            if (itChildIndex != self->m_IndexById.end())
            {
                Node& childNode = self->m_Nodes[itChildIndex->second];
                childNode.label = p.filename().string();
                childNode.path = p;
                childNode.key = key;
                childNode.expandable = hasSubdirs(p);
            }
        }
        else
        {
            childId = self->m_NextId++;
            Node child{};
            child.id = childId;
            child.label = p.filename().string();
            child.path = p;
            child.key = key;
            child.expandable = hasSubdirs(p);
            self->m_Nodes.push_back(std::move(child));
            self->m_IndexById[childId] = self->m_Nodes.size() - 1;
            if (!key.empty())
                self->m_IdByPath[key] = childId;
        }

        if (childId != 0)
        {
            newChildren.push_back(childId);
        }
    }

    // IMPORTANT: m_Nodes may have reallocated while adding new nodes above, so
    // never keep references into m_Nodes across that loop. Assign back by index.
    self->m_Nodes[parentIndex].children = std::move(newChildren);
    self->m_Nodes[parentIndex].expandable = !self->m_Nodes[parentIndex].children.empty();
    m_Populated.insert(parent);
}

void AssetsTreeDataProvider::SetSmartFolderManager(SmartFolderManager* manager)
{
    m_SmartFolderManager = manager;
    // RefreshSmartFolders() is invoked by AssetsBrowserController::SetSmartFolderManager after wiring;
    // avoid rebuilding smart-folder nodes twice per update.
}

void AssetsTreeDataProvider::RefreshSmartFolders()
{
    // Remove smart folder IDs from populated set
    m_Populated.erase(kSmartFoldersSectionId);
    for (TreeId sfId : m_SmartFolderIds) {
        m_Populated.erase(sfId);
    }
    
    // Remove existing smart folder nodes from m_Roots
    m_Roots.erase(
        std::remove_if(m_Roots.begin(), m_Roots.end(),
            [this](TreeId id) { return IsSmartFolder(id) || IsSmartFoldersSection(id); }),
        m_Roots.end());
    
    // Clear smart folder ID tracking
    m_SmartFolderIds.clear();
    
    // Remove smart folder nodes from m_Nodes and rebuild index maps
    m_Nodes.erase(
        std::remove_if(m_Nodes.begin(), m_Nodes.end(),
            [](const Node& n) { return n.isSmartFolder; }),
        m_Nodes.end());
    
    // Rebuild index maps after removal
    m_IndexById.clear();
    for (size_t i = 0; i < m_Nodes.size(); ++i)
    {
        m_IndexById[m_Nodes[i].id] = i;
    }
    
    if (!m_SmartFolderManager)
    {
        MarkAllChanged();
        return;
    }
    
    const auto& smartFolders = m_SmartFolderManager->GetAll();
    if (smartFolders.empty())
    {
        MarkAllChanged();
        return;
    }
    
    // Add "Smart Folders" section header
    Node sectionNode{};
    sectionNode.id = kSmartFoldersSectionId;
    sectionNode.label = "Smart Folders";
    sectionNode.expandable = true;
    sectionNode.isSmartFolder = true;
    m_Nodes.push_back(std::move(sectionNode));
    m_IndexById[kSmartFoldersSectionId] = m_Nodes.size() - 1;
    
    // Add to roots at top or bottom based on setting
    if (m_SmartFoldersAtTop) {
        m_Roots.insert(m_Roots.begin(), kSmartFoldersSectionId);
    } else {
        m_Roots.push_back(kSmartFoldersSectionId);
    }
    
    // Add each smart folder as a child of the section
    TreeId nextSmartFolderId = kSmartFolderIdBase;
    for (const auto& sf : smartFolders)
    {
        Node sfNode{};
        sfNode.id = nextSmartFolderId++;
        sfNode.label = sf.Name;
        sfNode.expandable = false;
        sfNode.isSmartFolder = true;
        sfNode.smartFolderId = sf.Id;
        
        m_Nodes.push_back(std::move(sfNode));
        m_IndexById[m_Nodes.back().id] = m_Nodes.size() - 1;
        m_SmartFolderIds.push_back(m_Nodes.back().id);
        
        // Add as child of section node
        size_t sectionIdx = m_IndexById[kSmartFoldersSectionId];
        m_Nodes[sectionIdx].children.push_back(m_Nodes.back().id);
        
        // Mark smart folder node as populated (no filesystem children)
        m_Populated.insert(m_Nodes.back().id);
    }
    
    // Mark the section as populated so EnsureChildrenBuilt doesn't try to scan filesystem
    m_Populated.insert(kSmartFoldersSectionId);
    MarkAllChanged();
}

bool AssetsTreeDataProvider::IsSmartFolder(TreeId id) const
{
    if (id == kSmartFoldersSectionId)
        return false; // The section header is not a smart folder itself
    
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    
    const Node& node = m_Nodes[it->second];
    return node.isSmartFolder && !node.smartFolderId.empty();
}

bool AssetsTreeDataProvider::IsSmartFoldersSection(TreeId id) const
{
    return id == kSmartFoldersSectionId;
}

bool AssetsTreeDataProvider::IsAssetsRoot(TreeId id) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    const Node& n = m_Nodes[it->second];
    if (n.path.empty() || n.isSmartFolder)
        return false;
    return std::find(m_Roots.begin(), m_Roots.end(), id) != m_Roots.end();
}

std::string AssetsTreeDataProvider::GetSmartFolderId(TreeId treeId) const
{
    auto it = m_IndexById.find(treeId);
    if (it == m_IndexById.end())
        return {};
    return m_Nodes[it->second].smartFolderId;
}

TreeId AssetsTreeDataProvider::GetTreeIdForSmartFolder(const std::string& smartFolderId) const
{
    for (const auto& node : m_Nodes) {
        if (node.isSmartFolder && node.smartFolderId == smartFolderId) {
            return node.id;
        }
    }
    return 0;
}

void AssetsTreeDataProvider::SetSmartFoldersAtTop(bool atTop)
{
    if (m_SmartFoldersAtTop != atTop) {
        m_SmartFoldersAtTop = atTop;
        RefreshSmartFolders();
    }
}

void AssetsTreeDataProvider::SetOnlineAssetsAtTop(bool atTop)
{
    if (m_OnlineAssetsAtTop != atTop)
    {
        m_OnlineAssetsAtTop = atTop;
        RefreshOnlineAssets();
        MarkAllChanged();
    }
}

void AssetsTreeDataProvider::SetOnlineAssetsEnabled(bool enabled)
{
    if (m_OnlineAssetsEnabled != enabled)
    {
        m_OnlineAssetsEnabled = enabled;
        RefreshOnlineAssets();
        MarkAllChanged();
    }
}

void AssetsTreeDataProvider::SetPolyHavenEnabled(bool enabled)
{
    if (m_PolyHavenEnabled != enabled)
    {
        m_PolyHavenEnabled = enabled;
        RefreshOnlineAssets();
        MarkAllChanged();
    }
}

void AssetsTreeDataProvider::RefreshOnlineAssets()
{
    m_Populated.erase(kOnlineAssetsSectionId);
    m_Populated.erase(kPolyhavenId);
    for (TreeId cid = kPolyhavenCategoryIdBase; cid < kPolyhavenCategoryIdBase + 3; ++cid)
        m_Populated.erase(cid);

    m_Roots.erase(
        std::remove_if(m_Roots.begin(), m_Roots.end(),
            [this](TreeId id) { return IsOnlineAssetsSection(id) || IsPolyhaven(id) || IsPolyhavenCategory(id); }),
        m_Roots.end());

    m_Nodes.erase(
        std::remove_if(m_Nodes.begin(), m_Nodes.end(),
            [](const Node& n) { return n.isOnlineAsset; }),
        m_Nodes.end());

    m_IndexById.clear();
    for (size_t i = 0; i < m_Nodes.size(); ++i)
        m_IndexById[m_Nodes[i].id] = i;

    if (!m_OnlineAssetsEnabled)
        return;

    // Online Assets section
    Node sectionNode{};
    sectionNode.id = kOnlineAssetsSectionId;
    sectionNode.label = "Online Assets";
    sectionNode.expandable = true;
    sectionNode.isOnlineAsset = true;
    m_Nodes.push_back(std::move(sectionNode));
    m_IndexById[kOnlineAssetsSectionId] = m_Nodes.size() - 1;

    // Polyhaven child (only if enabled)
    if (m_PolyHavenEnabled)
    {
        Node polyNode{};
        polyNode.id = kPolyhavenId;
        polyNode.label = "Polyhaven";
        polyNode.expandable = true;
        polyNode.isOnlineAsset = true;
        m_Nodes.push_back(std::move(polyNode));
        m_IndexById[kPolyhavenId] = m_Nodes.size() - 1;

        size_t sectionIdx = m_IndexById[kOnlineAssetsSectionId];
        m_Nodes[sectionIdx].children.push_back(kPolyhavenId);

        // Type nodes: HDRIs, Textures, Models (expandable; children = category directories, added by SetPolyhavenTypeCategories)
        static const struct { const char* label; const char* category; } kCategories[] = {
            { "HDRIs", "hdris" },
            { "Textures", "textures" },
            { "Models", "models" },
        };
        for (size_t i = 0; i < 3; ++i)
        {
            Node catNode{};
            catNode.id = kPolyhavenCategoryIdBase + static_cast<TreeId>(i);
            catNode.label = kCategories[i].label;
            catNode.expandable = true;
            catNode.isOnlineAsset = true;
            catNode.onlineAssetCategory = kCategories[i].category;
            m_Nodes.push_back(std::move(catNode));
            m_IndexById[m_Nodes.back().id] = m_Nodes.size() - 1;
            size_t polyIdx = m_IndexById[kPolyhavenId];
            m_Nodes[polyIdx].children.push_back(m_Nodes.back().id);

            // Placeholder "Loading..." child until SetPolyhavenTypeCategories fills real categories
            Node loadingNode{};
            loadingNode.id = m_NextPolyhavenCategoryChildId++;
            loadingNode.label = "Loading...";
            loadingNode.expandable = false;
            loadingNode.isOnlineAsset = true;
            loadingNode.onlineAssetCategory = kCategories[i].category;
            loadingNode.polyhavenCategorySlug = "loading";
            m_Nodes.push_back(std::move(loadingNode));
            m_IndexById[m_Nodes.back().id] = m_Nodes.size() - 1;
            size_t typeNodeIdx = m_IndexById[kPolyhavenCategoryIdBase + static_cast<TreeId>(i)];
            m_Nodes[typeNodeIdx].children.push_back(m_Nodes.back().id);
            m_Populated.insert(m_Nodes.back().id);
        }
        m_Populated.insert(kPolyhavenId);
        for (TreeId cid = kPolyhavenCategoryIdBase; cid < kPolyhavenCategoryIdBase + 3; ++cid)
            m_Populated.insert(cid);
    }

    if (m_OnlineAssetsAtTop)
        m_Roots.insert(m_Roots.begin(), kOnlineAssetsSectionId);
    else
        m_Roots.push_back(kOnlineAssetsSectionId);
    m_Populated.insert(kOnlineAssetsSectionId);
}

void AssetsTreeDataProvider::SetPolyhavenTypeCategories(const std::string& type,
    const std::vector<std::pair<std::string, std::string>>& slugAndLabel)
{
    TreeId typeNodeId = 0;
    for (TreeId tid = kPolyhavenCategoryIdBase; tid < kPolyhavenCategoryIdBase + 3; ++tid)
    {
        auto it = m_IndexById.find(tid);
        if (it != m_IndexById.end() && m_Nodes[it->second].onlineAssetCategory == type)
        {
            typeNodeId = tid;
            break;
        }
    }
    if (typeNodeId == 0)
        return;
    size_t typeIdx = m_IndexById[typeNodeId];

    // Remove existing category child nodes for this type only
    m_Nodes.erase(
        std::remove_if(m_Nodes.begin(), m_Nodes.end(),
            [&type](const Node& n) {
                if (n.id < kPolyhavenCategoryChildIdBase || n.polyhavenCategorySlug.empty())
                    return false;
                return n.onlineAssetCategory == type;
            }),
        m_Nodes.end());
    m_IndexById.clear();
    for (size_t i = 0; i < m_Nodes.size(); ++i)
        m_IndexById[m_Nodes[i].id] = i;

    typeIdx = m_IndexById[typeNodeId];
    m_Nodes[typeIdx].children.clear();
    // Build ordered list with "All" first (slug "all" or label starting with "All")
    std::vector<std::pair<std::string, std::string>> ordered;
    ordered.reserve(slugAndLabel.size());
    for (const auto& pair : slugAndLabel)
    {
        bool isAll = (pair.first == "all" || (pair.second.size() >= 3 && pair.second.compare(0, 3, "All") == 0));
        if (isAll)
            ordered.insert(ordered.begin(), pair);
        else
            ordered.push_back(pair);
    }
    // Collect child IDs separately — m_Nodes.push_back() can reallocate,
    // invalidating any reference into m_Nodes (including typeChildren).
    std::vector<TreeId> newChildIds;
    newChildIds.reserve(ordered.size());
    for (const auto& pair : ordered)
    {
        const std::string& slug = pair.first;
        const std::string& label = pair.second;
        Node sub{};
        sub.id = m_NextPolyhavenCategoryChildId++;
        sub.label = label;
        sub.expandable = false;
        sub.isOnlineAsset = true;
        sub.onlineAssetCategory = type;
        sub.polyhavenCategorySlug = slug;
        TreeId childId = sub.id;
        m_Nodes.push_back(std::move(sub));
        m_IndexById[m_Nodes.back().id] = m_Nodes.size() - 1;
        newChildIds.push_back(childId);
        m_Populated.insert(childId);
    }
    // Assign children after all push_backs are done (typeIdx is still valid
    // because we only appended — indices don't shift).
    m_Nodes[m_IndexById[typeNodeId]].children = std::move(newChildIds);
    MarkAllChanged();
}

bool AssetsTreeDataProvider::IsPolyhavenCategoryChild(TreeId id) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    const Node& n = m_Nodes[it->second];
    return n.isOnlineAsset && !n.polyhavenCategorySlug.empty();
}

void AssetsTreeDataProvider::GetPolyhavenTypeAndCategory(TreeId id, std::string& outType, std::string& outCategorySlug) const
{
    outType.clear();
    outCategorySlug.clear();
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return;
    const Node& n = m_Nodes[it->second];
    if (!n.isOnlineAsset || n.polyhavenCategorySlug.empty())
        return;
    outType = n.onlineAssetCategory;
    outCategorySlug = n.polyhavenCategorySlug;
}

bool AssetsTreeDataProvider::IsOnlineAssetsSection(TreeId id) const
{
    return id == kOnlineAssetsSectionId;
}

bool AssetsTreeDataProvider::IsPolyhaven(TreeId id) const
{
    return id == kPolyhavenId;
}

bool AssetsTreeDataProvider::IsPolyhavenCategory(TreeId id) const
{
    if (id < kPolyhavenCategoryIdBase || id >= kPolyhavenCategoryIdBase + 3)
        return false;
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    return m_Nodes[it->second].isOnlineAsset && !m_Nodes[it->second].onlineAssetCategory.empty();
}

std::string AssetsTreeDataProvider::GetPolyhavenCategory(TreeId id) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return {};
    const Node& n = m_Nodes[it->second];
    if (!n.isOnlineAsset || n.onlineAssetCategory.empty())
        return {};
    return n.onlineAssetCategory;
}


// --- AssetsGridDataProvider -------------------------------------------------
AssetsGridDataProvider::AssetsGridDataProvider() {
	// Defer loading until controller provides the assets root via SetRootLimit/LoadDirectory.
}

namespace
{
static uint64_t Fnv1a64(const char* data, size_t len)
{
    // 64-bit FNV-1a
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < len; ++i)
    {
        hash ^= (uint64_t)(unsigned char)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t StableHashPath(const std::filesystem::path& p)
{
    std::error_code ecAbs;
    std::filesystem::path abs = std::filesystem::absolute(p, ecAbs);
    if (ecAbs)
    {
        abs = p;
    }
    const std::string key = abs.lexically_normal().generic_string();
    return Fnv1a64(key.data(), key.size());
}
} // namespace

void AssetsGridDataProvider::RebuildIndexById()
{
    m_IndexById.clear();
    m_IndexById.reserve(m_Items.size());
    m_IdByPath.clear();
    m_IdByPath.reserve(m_Items.size());
    for (size_t i = 0; i < m_Items.size(); ++i)
    {
        m_IndexById[m_Items[i].id] = i;
        if (!m_Items[i].isUp && !m_Items[i].path.empty())
        {
            const std::string key = AssetPathKey(m_Items[i].path);
            if (!key.empty())
                m_IdByPath[key] = m_Items[i].id;
        }
    }
}

int AssetsGridDataProvider::GetItemCount() const { return static_cast<int>(m_Items.size()); }
GridId AssetsGridDataProvider::GetItemId(int index) const { return m_Items[index].id; }
const char* AssetsGridDataProvider::GetLabel(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it != m_IndexById.end())
        return m_Items[it->second].label.c_str();
    static const char* kEmpty = "";
    return kEmpty;
}
	uint64_t AssetsGridDataProvider::GetIcon(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it != m_IndexById.end())
        return m_Items[it->second].icon;
    return 0;
	}
	
	const char* AssetsGridDataProvider::GetTypeKey(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it != m_IndexById.end())
        return m_Items[it->second].type.c_str();
    static const char* kUnknown = ""; return kUnknown;
}

bool AssetsGridDataProvider::TryGetImageDimensions(GridId id, int& outWidth, int& outHeight) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    const auto& item = m_Items[it->second];
    if (!item.hasImageDimensions || item.imgWidth <= 0 || item.imgHeight <= 0)
        return false;
    outWidth = item.imgWidth;
    outHeight = item.imgHeight;
    return true;
}

bool AssetsGridDataProvider::TryGetSizeBytes(GridId id, uintmax_t& outSizeBytes) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    const auto& item = m_Items[it->second];
    if (!item.hasSizeBytes)
        return false;
    outSizeBytes = item.sizeBytes;
    return true;
}

bool AssetsGridDataProvider::TryGetLastWriteTime(GridId id, std::filesystem::file_time_type& outLastWriteTime) const
{
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    const auto& item = m_Items[it->second];
    if (!item.hasLastWriteTime)
        return false;
    outLastWriteTime = item.lastWriteTime;
    return true;
}

void AssetsGridDataProvider::ApplySort(const SortDescriptor& desc) {
    m_Sort = desc;
    // Keep special ".." entry at the very front
    auto firstNonUp = std::stable_partition(m_Items.begin(), m_Items.end(), [](const Item& it){ return it.isUp; });

    // Precompute VCS status priorities once per sort (avoid per-compare cache lookups).
    // GetFileStatus() is expected to be non-blocking (cached) for UI safety.
    std::unordered_map<GridId, int> vcsPriorityById;
    if (m_Sort.field == SortDescriptor::Field::Git)
    {
        if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration(); vcs && vcs->IsRepository())
        {
            vcsPriorityById.reserve(m_Items.size());
            for (const auto& it : m_Items)
            {
                const VCSFileStatus st = it.path.empty() ? VCSFileStatus::Clean : vcs->GetFileStatus(it.path);
                vcsPriorityById[it.id] = GetStatusPriority(st);
            }
        }
    }

    // Re-sort remaining items by selected field, optionally keeping folders before files.
    std::stable_sort(firstNonUp, m_Items.end(), [&](const Item& a, const Item& b){
        if (m_FoldersFirst && a.isDir != b.isDir)
            return a.isDir && !b.isDir;

        const bool asc = m_Sort.ascending;

        switch (m_Sort.field)
        {
        case SortDescriptor::Field::Type:
        {
            const std::string& ta = a.type;
            const std::string& tb = b.type;
            if (ta != tb) return asc ? (ta < tb) : (ta > tb);
            break;
        }
        case SortDescriptor::Field::Size:
        {
            const uintmax_t sa = (!a.isDir && a.hasSizeBytes) ? a.sizeBytes : 0;
            const uintmax_t sb = (!b.isDir && b.hasSizeBytes) ? b.sizeBytes : 0;
            if (sa != sb) return asc ? (sa < sb) : (sa > sb);
            break;
        }
        case SortDescriptor::Field::Modified:
        {
            const auto ta = (!a.isDir && a.hasLastWriteTime) ? a.lastWriteTime : std::filesystem::file_time_type{};
            const auto tb = (!b.isDir && b.hasLastWriteTime) ? b.lastWriteTime : std::filesystem::file_time_type{};
            if (ta != tb) return asc ? (ta < tb) : (ta > tb);
            break;
        }
        case SortDescriptor::Field::Dimensions:
        {
            // Sort by total pixels (width * height), with non-images at the end
            const int64_t pixelsA =
                (a.hasImageDimensions && a.imgWidth > 0 && a.imgHeight > 0) ? (int64_t)a.imgWidth * a.imgHeight : -1;
            const int64_t pixelsB =
                (b.hasImageDimensions && b.imgWidth > 0 && b.imgHeight > 0) ? (int64_t)b.imgWidth * b.imgHeight : -1;
            if (pixelsA != pixelsB)
            {
                // Non-images (-1) go to the end
                if (pixelsA < 0) return false; // a is non-image, goes after b
                if (pixelsB < 0) return true;  // b is non-image, a goes before
                return asc ? (pixelsA < pixelsB) : (pixelsA > pixelsB);
            }
            break;
        }
        case SortDescriptor::Field::Git:
        {
            // Sort by VCS status
            if (!vcsPriorityById.empty())
            {
                const int priorityA = vcsPriorityById[a.id];
                const int priorityB = vcsPriorityById[b.id];
                if (priorityA != priorityB)
                    return asc ? (priorityA < priorityB) : (priorityA > priorityB);
            }
            // If no VCS or same status, fall through to name sorting
            break;
        }
        case SortDescriptor::Field::Tag:
        {
            if (m_TagSortKeyResolver)
            {
                const std::string keyA = a.isDir ? std::string() : m_TagSortKeyResolver(a.path);
                const std::string keyB = b.isDir ? std::string() : m_TagSortKeyResolver(b.path);
                const bool emptyA = keyA.empty();
                const bool emptyB = keyB.empty();
                if (emptyA != emptyB)
                    return asc ? emptyA : emptyB; // no-tag first when ascending, last when descending
                if (keyA != keyB)
                    return asc ? (keyA < keyB) : (keyA > keyB);
            }
            break;
        }
        case SortDescriptor::Field::Referenced:
        case SortDescriptor::Field::Custom:
        case SortDescriptor::Field::Creator:
            // No data yet; fall through to name for stable ordering
            break;
        case SortDescriptor::Field::Name:
        default:
            break;
        }

        // Fallback to name for stable ordering
        if (a.label == b.label) return false;
        return asc ? (a.label < b.label) : (a.label > b.label);
    });
    RebuildIndexById();
    MarkAllChanged();
}
void AssetsGridDataProvider::ApplyGrouping(const GroupDescriptor& desc) {
    m_Group = desc; // grouping headers can be added later
    MarkAllChanged();
}

void AssetsGridDataProvider::SetSearchQuery(const std::string& query)
{
    const std::string next = ToLowerAscii(query);
    if (next == m_FilterLower)
        return;
    m_FilterLower = next;

    // Universal Search keeps an immutable, normalized snapshot and evaluates
    // each query against memory. Do the same here: the first project-wide
    // query builds the recursive snapshot, while subsequent keystrokes only
    // filter it. This also preserves m_CurrentDir so clearing search returns to
    // the directory that was open before searching.
    if (!next.empty())
        BuildProjectSearchSnapshot();
    ApplyFilter();
    ApplySort(m_Sort);
}

void AssetsGridDataProvider::SetSearchField(AssetsSearchField field)
{
    if (m_SearchField == field)
        return;
    m_SearchField = field;
    ApplyFilter();
    ApplySort(m_Sort);
}

void AssetsGridDataProvider::RefreshSearchIndex()
{
    InvalidateProjectSearchSnapshot();
    if (m_FilterLower.empty())
        return;
    BuildProjectSearchSnapshot();
    ApplyFilter();
    ApplySort(m_Sort);
}

void AssetsGridDataProvider::SetFoldersFirst(bool foldersFirst)
{
    if (m_FoldersFirst == foldersFirst)
        return;

    m_FoldersFirst = foldersFirst;
    ApplySort(m_Sort);
}

void AssetsGridDataProvider::SetTagSortKeyResolver(TagSortKeyResolver resolver)
{
    m_TagSortKeyResolver = std::move(resolver);
    for (auto& item : m_AllItems)
        IndexItemForSearch(item, m_TagSortKeyResolver);
    InvalidateProjectSearchSnapshot();
}

void AssetsGridDataProvider::SetAssetsRoot(const std::filesystem::path& root)
{
    if (m_AssetsRoot == root)
        return;
    m_AssetsRoot = root;
    InvalidateProjectSearchSnapshot();
}

void AssetsGridDataProvider::SetAssetRegistry(AssetRegistry* registry)
{
    if (m_AssetRegistry == registry)
        return;
    m_AssetRegistry = registry;
    InvalidateProjectSearchSnapshot();
}

void AssetsGridDataProvider::InvalidateProjectSearchSnapshot()
{
    ++m_ProjectSearchGeneration;
    m_ProjectSearchItems.clear();
    m_ProjectSearchItemsValid = false;
}

void AssetsGridDataProvider::IndexItemForSearch(Item& item, const TagSortKeyResolver& tags)
{
    item.searchName = ToLowerAscii(item.label);
    item.searchType = ToLowerAscii(item.type);
    item.searchPath = ToLowerAscii(item.path.generic_string());

    std::string extension = item.path.extension().string();
    if (!extension.empty() && extension.front() == '.')
        extension += " " + extension.substr(1);
    item.searchExtension = ToLowerAscii(extension);

    item.searchTag.clear();
    if (!item.isDir && tags)
        item.searchTag = ToLowerAscii(tags(item.path));

    item.searchAll = item.searchName + " " + item.searchType + " " + item.searchPath;
    if (!item.isDir)
    {
        if (item.hasSizeBytes)
        {
            item.searchAll += " " + std::to_string(item.sizeBytes);
            item.searchAll += " " + ToLowerAscii(FormatByteSize(item.sizeBytes));
        }
        if (item.hasLastWriteTime)
            item.searchAll += " " + ToLowerAscii(FormatTime(item.lastWriteTime));
        if (!item.searchTag.empty())
            item.searchAll += " " + item.searchTag;
    }
}

void AssetsGridDataProvider::BuildProjectSearchSnapshot()
{
    if (m_ProjectSearchItemsValid || m_AssetsRoot.empty())
        return;
    m_ProjectSearchItems = ScanProjectForSearch(m_AssetsRoot, m_AssetRegistry, m_TagSortKeyResolver).Items;
    m_ProjectSearchItemsValid = true;
}

bool AssetsGridDataProvider::ApplyProjectSearchScan(ProjectSearchScan scan, uint64_t generation)
{
    if (generation != m_ProjectSearchGeneration || m_ProjectSearchItemsValid)
        return false;
    m_ProjectSearchItems = std::move(scan.Items);
    m_ProjectSearchItemsValid = true;
    if (m_FilterLower.empty())
        return false;
    ApplyFilter();
    ApplySort(m_Sort);
    return true;
}

AssetsGridDataProvider::ProjectSearchScan AssetsGridDataProvider::ScanProjectForSearch(
    const std::filesystem::path& assetsRoot, const AssetRegistry* registry, const TagSortKeyResolver& tags)
{
    std::vector<Item> dirs;
    std::vector<Item> files;
    std::unordered_set<std::string> registeredPaths;
    std::unordered_set<GridId> usedIds;
    const size_t expectedAssets = registry ? registry->GetAssetCount() : 0;
    registeredPaths.reserve(expectedAssets);
    usedIds.reserve(expectedAssets + 128);
    usedIds.insert(kUpEntryId);

    // The registry's hot cache is populated from the authoritative asset DB
    // and startup scan. Take it once under one shared lock, then adapt the
    // lightweight rows without any per-file filesystem work.
    if (registry)
    {
        const Vector<AssetIndexRecord> snapshot = registry->GetAssetIndexSnapshot();
        files.reserve(snapshot.size());
        // The root arrives in its on-disk spelling and registry paths arrive
        // folded, so "is this record under the root" is a key question, not a
        // lexical one. Keyed once here and once per record below.
        const std::string rootKey = AssetPathKey(assetsRoot);
        for (const AssetIndexRecord& record : snapshot)
        {
            if (record.Path.empty())
                continue;

            // The one site that scales with project size: once per registry
            // record on a project-wide search rebuild.
            const std::string pathKey = AssetPathKey(record.Path);
            if (pathKey.empty() || !IsUnderAssetDirKey(rootKey, pathKey))
                continue;
            if (!registeredPaths.insert(pathKey).second)
                continue;

            Item item{};
            uint64_t hash = StableHashPath(record.Path) & 0x7FFFFFFFFFFFFFFFull;
            if (hash == 0)
                hash = 1;
            GridId id = static_cast<GridId>(hash);
            while (usedIds.contains(id))
            {
                id = (id + 1ull) & 0x7FFFFFFFFFFFFFFFull;
                if (id == 0)
                    id = 1;
            }
            usedIds.insert(id);

            item.id = id;
            item.label = record.Path.filename().string();
            item.path = record.Path;
            item.type = record.Extension.empty() ? std::string("File") : ToLowerAscii(record.Extension);
            item.sizeBytes = static_cast<uintmax_t>(record.FileSize);
            item.hasSizeBytes = true;
            item.lastWriteTime = record.LastModified;
            item.hasLastWriteTime = record.LastModified != std::filesystem::file_time_type{};
            IndexItemForSearch(item, tags);
            files.push_back(std::move(item));
        }
    }

    // Folders are not asset records, and a just-created file can briefly
    // precede registry ingestion. Walk directory names only to add folders
    // and those unregistered files; registered files avoid stat/image reads.
    LoadSearchFilesystemSupplement(assetsRoot, registeredPaths, tags, dirs, files, usedIds);

    ProjectSearchScan scan;
    scan.Items.reserve(dirs.size() + files.size());
    scan.Items.insert(scan.Items.end(), std::make_move_iterator(dirs.begin()), std::make_move_iterator(dirs.end()));
    scan.Items.insert(scan.Items.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
    return scan;
}

void AssetsGridDataProvider::ApplyFilter()
{
    m_Items.clear();
    const std::vector<Item>& source =
        !m_FilterLower.empty() && m_ProjectSearchItemsValid
            ? m_ProjectSearchItems
            : m_AllItems;
    if (source.empty())
        return;

    if (m_FilterLower.empty())
    {
        m_Items = source;
        return;
    }

    for (const auto& item : source)
    {
        if (item.isUp)
        {
            m_Items.push_back(item);
            continue;
        }

        const std::string* blob = nullptr;
        switch (m_SearchField)
        {
            case AssetsSearchField::Name:
                blob = &item.searchName;
                break;
            case AssetsSearchField::Type:
                blob = &item.searchType;
                break;
            case AssetsSearchField::Extension:
                blob = &item.searchExtension;
                break;
            case AssetsSearchField::Path:
                blob = &item.searchPath;
                break;
            case AssetsSearchField::Tag:
                blob = &item.searchTag;
                break;
            case AssetsSearchField::All:
                blob = &item.searchAll;
                break;
        }

        if (blob && blob->find(m_FilterLower) != std::string::npos)
        {
            m_Items.push_back(item);
        }
    }
}


void AssetsGridDataProvider::LoadSearchFilesystemSupplement(
    const std::filesystem::path& dir,
    const std::unordered_set<std::string>& registeredPaths,
    const TagSortKeyResolver& tags,
    std::vector<Item>& outDirs,
    std::vector<Item>& outFiles,
    std::unordered_set<GridId>& usedIds)
{
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::end(it); ++it) {
        const auto& p = it->path();
        const bool isDir = it->is_directory(ec);

        std::string filename = p.filename().string();
        if (!filename.empty() && filename[0] == '.') {
            continue;
        }

        Item item{};
        uint64_t h = StableHashPath(p) & 0x7FFFFFFFFFFFFFFFull;
        if (h == 0)
            h = 1;
        GridId gid = (GridId)h;
        while (usedIds.find(gid) != usedIds.end()) {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0)
                gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = filename;
        item.isDir = isDir;
        item.path = p;

        if (isDir) {
            item.type = std::string("Folder");
            IndexItemForSearch(item, tags);
            outDirs.emplace_back(item);
            LoadSearchFilesystemSupplement(p, registeredPaths, tags, outDirs, outFiles, usedIds);
        } else {
            if (registeredPaths.contains(AssetPathKey(p)))
                continue;

            std::string ext = p.has_extension() ? p.extension().string() : std::string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            item.type = !ext.empty() ? ext : std::string("File");

            // Preserve complete metadata for the small supplement set (and
            // for callers without a registry). Registered assets never enter
            // this path, which removes the project-wide stat/image probe.
            std::error_code ecSize;
            const auto sz = it->file_size(ecSize);
            if (!ecSize) {
                item.sizeBytes = sz;
                item.hasSizeBytes = true;
            }

            std::error_code ecWt;
            const auto wt = it->last_write_time(ecWt);
            if (!ecWt) {
                item.lastWriteTime = wt;
                item.hasLastWriteTime = true;
            }

            int imgW = 0, imgH = 0;
            if (GetImageDimensions(p, imgW, imgH)) {
                item.imgWidth = imgW;
                item.imgHeight = imgH;
                item.hasImageDimensions = true;
            }
            IndexItemForSearch(item, tags);
            outFiles.emplace_back(std::move(item));
        }
    }
}

void AssetsGridDataProvider::LoadDirectory(const std::filesystem::path& dir) {
    ApplyDirectoryScan(ScanDirectory(dir, m_TagSortKeyResolver), NextListingGeneration());
}

AssetsGridDataProvider::DirectoryScan AssetsGridDataProvider::ScanDirectory(const std::filesystem::path& dir,
                                                                            const TagSortKeyResolver& tags) {
    DirectoryScan scan;
    scan.Dir = dir;
    std::error_code ec;
    scan.Exists = std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
    if (!scan.Exists)
        return scan;

    std::unordered_set<GridId> usedIds;
    usedIds.reserve(128);
    usedIds.insert(kUpEntryId);

    // Keep the current-directory snapshot separate from the project-wide
    // search snapshot so navigation does not get replaced by search state.
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::end(it); ++it) {
        const auto& p = it->path();
        const bool isDir = it->is_directory(ec);

        // Skip hidden/special files and directories (dotfiles) so the Assets panel
        // matches the AssetRegistry/AsyncRegistryTasks behaviour and does not show
        // entries like .DS_Store or .gitkeep.
        std::string filename = p.filename().string();
        if (!filename.empty() && filename[0] == '.') {
            continue;
        }

        Item item{};
        // Stable id derived from normalized absolute path (within this run).
        uint64_t h = StableHashPath(p) & 0x7FFFFFFFFFFFFFFFull; // keep top bit clear for user entries
        if (h == 0)
            h = 1;
        GridId gid = (GridId)h;
        // Collision-resolve within this directory snapshot.
        while (usedIds.find(gid) != usedIds.end())
        {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0)
                gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = filename;
        item.isDir = isDir;
        item.path = p;
        if (isDir) {
            item.type = std::string("Folder");
            IndexItemForSearch(item, tags);
            scan.Dirs.emplace_back(std::move(item));
        } else {
            std::string ext = p.has_extension() ? p.extension().string() : std::string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            item.type = !ext.empty() ? ext : std::string("File");

            // Cache file metadata once per directory snapshot (avoid per-sort/per-bind disk queries).
            std::error_code ecSize;
            const auto sz = it->file_size(ecSize);
            if (!ecSize)
            {
                item.sizeBytes = sz;
                item.hasSizeBytes = true;
            }

            std::error_code ecWt;
            const auto wt = it->last_write_time(ecWt);
            if (!ecWt)
            {
                item.lastWriteTime = wt;
                item.hasLastWriteTime = true;
            }

            // Get image dimensions if it's an image file
            int imgW = 0, imgH = 0;
            if (GetImageDimensions(p, imgW, imgH))
            {
                item.imgWidth = imgW;
                item.imgHeight = imgH;
                item.hasImageDimensions = true;
            }
            IndexItemForSearch(item, tags);
            scan.Files.emplace_back(std::move(item));
        }
    }

    auto byNameAsc = [](const Item& a, const Item& b){ return a.label < b.label; };
    std::sort(scan.Dirs.begin(), scan.Dirs.end(), byNameAsc);
    std::sort(scan.Files.begin(), scan.Files.end(), byNameAsc);
    return scan;
}

bool AssetsGridDataProvider::ApplyDirectoryScan(DirectoryScan scan, uint64_t generation) {
    if (generation != m_ListingGeneration)
        return false;
    m_CurrentDir = scan.Dir;
    InvalidateProjectSearchSnapshot();
    m_Items.clear();
    m_IndexById.clear();
    m_AllItems.clear();

    if (!scan.Exists) {
        LOG_WARNING("[GridData] LoadDirectory: path '{}' missing or not a directory", scan.Dir.string());
        MarkAllChanged();
        return true;
    }

    // Optional "go up" entry ("..") if above root limit
    if (!m_RootLimit.empty()) {
        std::error_code eq;
        bool atRoot = std::filesystem::equivalent(m_CurrentDir, m_RootLimit, eq);
        if (!atRoot) {
            Item up{};
            up.id = kUpEntryId;
            up.label = "..";
            up.type = "Up";
            up.isDir = true;
            up.isUp = true;
            up.path = m_CurrentDir.parent_path();
            m_AllItems.emplace_back(std::move(up));
        }
    }

    std::unordered_set<GridId> usedIds;
    usedIds.reserve(scan.Dirs.size() + scan.Files.size() + 1);
    usedIds.insert(kUpEntryId);
    for (const Item& item : scan.Dirs)
        usedIds.insert(item.id);
    for (const Item& item : scan.Files)
        usedIds.insert(item.id);

    // Merge: optional "up" entry, then folders, then files
    m_AllItems.reserve(m_AllItems.size() + scan.Dirs.size() + scan.Files.size());
    std::move(scan.Dirs.begin(), scan.Dirs.end(), std::back_inserter(m_AllItems));
    std::move(scan.Files.begin(), scan.Files.end(), std::back_inserter(m_AllItems));

    InjectPhantomsForCurrentDir(usedIds);

    if (!m_FilterLower.empty())
        BuildProjectSearchSnapshot();
    ApplyFilter();
    // Apply current sort setting if any (e.g., descending), keeping ".." at front
    ApplySort(m_Sort);
    RebuildIndexById();
    return true;
}

void AssetsGridDataProvider::LoadFiles(const std::vector<std::filesystem::path>& files,
                                       AssetRegistry* registry) {
    ++m_ListingGeneration;
    m_CurrentDir.clear(); // No single directory for smart folders
    m_Items.clear();
    m_IndexById.clear();
    m_AllItems.clear();

    std::unordered_set<GridId> usedIds;
    usedIds.reserve(files.size() + 1);
    usedIds.insert(kUpEntryId);

    for (const auto& p : files) {
        AssetMetadata metadata;
        const bool hasRegistryMetadata = registry && registry->TryGetAssetMetadata(p, metadata);

        // Registry snapshots are intentionally allowed to lag filesystem
        // events. Use them for the expensive size/time fields below, but keep
        // the filesystem authoritative for membership so deleted or renamed
        // assets cannot remain as stale smart-folder rows.
        std::error_code statusEc;
        const auto fileStatus = std::filesystem::status(p, statusEc);
        if (statusEc || !std::filesystem::exists(fileStatus) ||
            std::filesystem::is_directory(fileStatus)) {
            continue;
        }

        // Skip hidden files (dotfiles)
        std::string filename = p.filename().string();
        if (!filename.empty() && filename[0] == '.') {
            continue;
        }

        Item item{};
        // Stable id derived from normalized absolute path
        uint64_t h = StableHashPath(p) & 0x7FFFFFFFFFFFFFFFull;
        if (h == 0)
            h = 1;
        GridId gid = (GridId)h;
        // Collision-resolve
        while (usedIds.find(gid) != usedIds.end()) {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0)
                gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = filename;
        item.isDir = false;
        item.path = p;

        std::string ext = p.has_extension() ? p.extension().string() : std::string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        item.type = !ext.empty() ? ext : std::string("File");

        // Cache file metadata (same as LoadDirectory) so list view shows size, dimensions, modified
        if (hasRegistryMetadata) {
            item.sizeBytes = metadata.FileSize;
            item.hasSizeBytes = true;
            item.lastWriteTime = metadata.LastModified;
            item.hasLastWriteTime = true;
        } else {
            std::error_code ecSize;
            const auto sz = std::filesystem::file_size(p, ecSize);
            if (!ecSize) {
                item.sizeBytes = sz;
                item.hasSizeBytes = true;
            }
            std::error_code ecWt;
            const auto wt = std::filesystem::last_write_time(p, ecWt);
            if (!ecWt) {
                item.lastWriteTime = wt;
                item.hasLastWriteTime = true;
            }
        }
        int imgW = 0, imgH = 0;
        if (GetImageDimensions(p, imgW, imgH)) {
            item.imgWidth = imgW;
            item.imgHeight = imgH;
            item.hasImageDimensions = true;
        }
        IndexItemForSearch(item, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(item));
    }

    auto byNameAsc = [](const Item& a, const Item& b){ return a.label < b.label; };
    std::sort(m_AllItems.begin(), m_AllItems.end(), byNameAsc);

    ApplyFilter();
    ApplySort(m_Sort);
    RebuildIndexById();
    MarkAllChanged();
}

void AssetsGridDataProvider::LoadRemoteItems(const std::vector<std::pair<std::filesystem::path, std::string>>& pathAndLabelPairs,
                                              const std::string& typeKey)
{
    ++m_ListingGeneration;
    m_CurrentDir.clear();
    m_Items.clear();
    m_IndexById.clear();
    m_AllItems.clear();

    std::unordered_set<GridId> usedIds;
    usedIds.insert(kUpEntryId);

    if (pathAndLabelPairs.empty() && !typeKey.empty())
    {
        Item loadingItem{};
        loadingItem.id = kLoadingPlaceholderId;
        loadingItem.label = "Loading...";
        loadingItem.isDir = false;
        loadingItem.path = std::filesystem::path("polyhaven://loading");
        loadingItem.type = typeKey;
        IndexItemForSearch(loadingItem, m_TagSortKeyResolver);
        m_AllItems.push_back(std::move(loadingItem));
        ApplyFilter();
        ApplySort(m_Sort);
        RebuildIndexById();
        return;
    }

    for (const auto& pair : pathAndLabelPairs)
    {
        const std::filesystem::path& p = pair.first;
        const std::string& label = pair.second;
        const std::string pathStr = p.generic_string();
        const bool isVirtualPath = pathStr.rfind("polyhaven://", 0) == 0;

        Item item{};
        uint64_t h = StableHashPath(p) & 0x7FFFFFFFFFFFFFFFull;
        if (h == 0)
            h = 1;
        GridId gid = (GridId)h;
        while (usedIds.find(gid) != usedIds.end())
        {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0)
                gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = label.empty() ? p.filename().string() : label;
        item.isDir = false;
        item.path = p;
        item.type = typeKey;

        if (!isVirtualPath)
        {
            std::error_code ecSize;
            const auto sz = std::filesystem::file_size(p, ecSize);
            if (!ecSize)
            {
                item.sizeBytes = sz;
                item.hasSizeBytes = true;
            }
            int imgW = 0, imgH = 0;
            if (GetImageDimensions(p, imgW, imgH))
            {
                item.imgWidth = imgW;
                item.imgHeight = imgH;
                item.hasImageDimensions = true;
            }
        }
        IndexItemForSearch(item, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(item));
    }

    auto byLabelAsc = [](const Item& a, const Item& b) { return a.label < b.label; };
    std::sort(m_AllItems.begin(), m_AllItems.end(), byLabelAsc);

    ApplyFilter();
    ApplySort(m_Sort);
    RebuildIndexById();
}

void AssetsGridDataProvider::AppendRemoteItems(
    const std::vector<std::pair<std::filesystem::path, std::string>>& pathAndLabelPairs,
    const std::string& typeKey)
{
    if (!m_CurrentDir.empty())
        return;
    if (m_AllItems.empty())
        return;
    if (m_AllItems.size() == 1u && m_AllItems[0].id == kLoadingPlaceholderId)
        return;

    std::unordered_set<GridId> usedIds;
    for (const Item& it : m_AllItems)
        usedIds.insert(it.id);

    for (const auto& pair : pathAndLabelPairs)
    {
        const std::filesystem::path& p = pair.first;
        const std::string& label = pair.second;
        const std::string pathStr = p.generic_string();
        const bool isVirtualPath = pathStr.rfind("polyhaven://", 0) == 0;

        Item item{};
        uint64_t h = StableHashPath(p) & 0x7FFFFFFFFFFFFFFFull;
        if (h == 0)
            h = 1;
        GridId gid = (GridId)h;
        while (usedIds.find(gid) != usedIds.end())
        {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0)
                gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = label.empty() ? p.filename().string() : label;
        item.isDir = false;
        item.path = p;
        item.type = typeKey;

        if (!isVirtualPath)
        {
            std::error_code ecSize;
            const auto sz = std::filesystem::file_size(p, ecSize);
            if (!ecSize)
            {
                item.sizeBytes = sz;
                item.hasSizeBytes = true;
            }
            int imgW = 0, imgH = 0;
            if (GetImageDimensions(p, imgW, imgH))
            {
                item.imgWidth = imgW;
                item.imgHeight = imgH;
                item.hasImageDimensions = true;
            }
        }
        IndexItemForSearch(item, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(item));
    }

    auto byLabelAsc = [](const Item& a, const Item& b) { return a.label < b.label; };
    std::sort(m_AllItems.begin(), m_AllItems.end(), byLabelAsc);

    ApplyFilter();
    ApplySort(m_Sort);
    RebuildIndexById();
    MarkAllChanged();
}

void AssetsGridDataProvider::LoadVirtualFolders(
    const std::vector<std::pair<std::string, std::filesystem::path>>& folders,
    const std::filesystem::path& parentVirtualPath)
{
    ++m_ListingGeneration;
    m_CurrentDir.clear();
    m_Items.clear();
    m_IndexById.clear();
    m_AllItems.clear();

    std::unordered_set<GridId> usedIds;
    usedIds.insert(kUpEntryId);

    // ".." entry if a parent path is given
    if (!parentVirtualPath.empty())
    {
        Item up{};
        up.id = kUpEntryId;
        up.label = "..";
        up.type = "Up";
        up.isDir = true;
        up.isUp = true;
        up.path = parentVirtualPath;
        IndexItemForSearch(up, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(up));
    }

    for (const auto& [label, vpath] : folders)
    {
        Item item{};
        uint64_t h = StableHashPath(vpath) & 0x7FFFFFFFFFFFFFFFull;
        if (h == 0) h = 1;
        GridId gid = static_cast<GridId>(h);
        while (usedIds.find(gid) != usedIds.end())
        {
            gid = (gid + 1ull) & 0x7FFFFFFFFFFFFFFFull;
            if (gid == 0) gid = 1;
        }
        usedIds.insert(gid);
        item.id = gid;
        item.label = label;
        item.type = "Folder";
        item.isDir = true;
        item.path = vpath;
        IndexItemForSearch(item, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(item));
    }

    ApplyFilter();
    ApplySort(m_Sort);
    RebuildIndexById();
}

std::filesystem::path AssetsGridDataProvider::GetPath(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return {};
    return m_Items[it->second].path;
}

GridId AssetsGridDataProvider::FindIdForPath(const std::filesystem::path& path) const
{
    const std::string key = AssetPathKey(path);
    if (key.empty())
        return 0;
    auto it = m_IdByPath.find(key);
    if (it == m_IdByPath.end())
        return 0;
    return it->second;
}

bool AssetsGridDataProvider::IsDirectory(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    return m_Items[it->second].isDir;
}

bool AssetsGridDataProvider::IsPhantom(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return false;
    return m_Items[it->second].isPhantom;
}

GUID AssetsGridDataProvider::GetPhantomGuid(GridId id) const {
    auto it = m_IndexById.find(id);
    if (it == m_IndexById.end())
        return GUID::Null();
    return m_Items[it->second].phantomGuid;
}

void AssetsGridDataProvider::SetMissingAssetTracker(MissingAssetTracker* tracker) {
    m_MissingAssetTracker = tracker;
}

void AssetsGridDataProvider::InjectPhantomsForCurrentDir(std::unordered_set<GridId>& usedIds) {
    if (!m_MissingAssetTracker || m_CurrentDir.empty())
        return;

    const auto entries = m_MissingAssetTracker->GetMissing();
    if (entries.empty())
        return;

    const std::filesystem::path currentNorm = m_CurrentDir.lexically_normal();

    GridId nextId = kPhantomEntryIdBase;
    for (const auto& e : entries) {
        if (e.AuthoredPath.empty())
            continue; // No path to anchor the phantom to.

        std::filesystem::path abs(e.AuthoredPath);
        if (abs.is_relative() && !m_AssetsRoot.empty())
            abs = (m_AssetsRoot / abs).lexically_normal();
        else
            abs = abs.lexically_normal();

        const auto parent = abs.parent_path();
        std::error_code eq;
        // Match the current dir (lexical equality is enough — both are normalized).
        if (parent != currentNorm)
            continue;

        // Allocate a unique phantom id distinct from real-file hashes.
        GridId pid = nextId++;
        while (usedIds.find(pid) != usedIds.end())
            pid = nextId++;
        usedIds.insert(pid);

        Item item{};
        item.id = pid;
        item.label = abs.filename().string();
        std::string ext = abs.has_extension() ? abs.extension().string() : std::string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        item.type = !ext.empty() ? ext : std::string("MissingAsset");
        item.isDir = false;
        item.isUp = false;
        item.isPhantom = true;
        item.phantomGuid = e.Guid;
        item.path = abs;
        IndexItemForSearch(item, m_TagSortKeyResolver);
        m_AllItems.emplace_back(std::move(item));
    }
}

// --- AssetsListDataProvider -------------------------------------------------
AssetsListDataProvider::AssetsListDataProvider(AssetsGridDataProvider* gridProvider)
    : m_GridProvider(gridProvider)
{
}

int AssetsListDataProvider::GetItemCount() const {
    if (!m_GridProvider)
        return 0;
    return m_GridProvider->GetItemCount();
}

ListId AssetsListDataProvider::GetItemId(int index) const {
    if (!m_GridProvider)
        return 0;
    // Convert GridId to ListId (they're both uint64_t, so this is safe)
    GridId gridId = m_GridProvider->GetItemId(index);
    return static_cast<ListId>(gridId);
}

float AssetsListDataProvider::GetItemHeight(int /*index*/) const {
    return m_ItemHeight;
}

void AssetsListDataProvider::ConsumeChanges(uint64_t sinceVersion, ListChangeSet& out) const
{
    if (!m_GridProvider)
    {
        out.Kind = ChangeSetKind::None;
        out.Ids.clear();
        out.Version = sinceVersion;
        return;
    }

    GridChangeSet gridChanges{};
    m_GridProvider->ConsumeChanges(sinceVersion, gridChanges);
    out.Version = gridChanges.Version;
    out.StructureVersion = gridChanges.StructureVersion;
    out.Kind = gridChanges.Kind;
    if (gridChanges.Kind == ChangeSetKind::Subset)
    {
        out.Ids.assign(gridChanges.Ids.begin(), gridChanges.Ids.end());
    }
    else
    {
        out.Ids.clear();
    }
}

uint64_t AssetsListDataProvider::GetChangeVersion() const
{
    return m_GridProvider ? m_GridProvider->GetChangeVersion() : 0;
}

std::filesystem::path AssetsListDataProvider::GetPath(ListId id) const {
    if (!m_GridProvider)
        return {};
    GridId gridId = static_cast<GridId>(id);
    return m_GridProvider->GetPath(gridId);
}

bool AssetsListDataProvider::IsDirectory(ListId id) const {
    if (!m_GridProvider)
        return false;
    GridId gridId = static_cast<GridId>(id);
    return m_GridProvider->IsDirectory(gridId);
}

const char* AssetsListDataProvider::GetLabel(ListId id) const {
    if (!m_GridProvider)
        return "";
    GridId gridId = static_cast<GridId>(id);
    return m_GridProvider->GetLabel(gridId);
}

const char* AssetsListDataProvider::GetTypeKey(ListId id) const {
    if (!m_GridProvider)
        return "";
    GridId gridId = static_cast<GridId>(id);
    return m_GridProvider->GetTypeKey(gridId);
}

} // namespace GameEngine
