#include "Assets/AssetsBrowserController.h"
#include "Assets/AssetDeletePathGuard.h"
#include "Assets/AssetPathKey.h"
#include "Assets/AssetsDataProviders.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenService.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/StyleProperties.h"
#include "UI/Layout/ElementOverrideHelpers.h"

#include "EditorContext.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/EditorIcons.h"
#include "EditorContextMenu/EditorContextMenu.h"
#include "MissingAssetTracker.h"
#include "Platform/Clipboard.h"
#include "FileSystem/FileSystem.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"

#include "Assets/AssetManager.h"
#include "AssetCore/AssetTypes.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsStatusUi.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "Editor/Settings/SettingsStore.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/CreateTextFileCommand.h"
#include "UndoRedo/DeleteAssetPathsCommand.h"
#include "UndoRedo/VcsRevertCommand.h"
#include "AssetCore/GUID.h"

#include "Platform/Shell.h"
#include "Editor/Registries/EditorSceneCommands.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"

#include "Thumbnails/IThumbnailProvider.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/ThumbnailService.h"

#include "Types/StringUtils.h"
#include "UI/SmartFolder/SmartFolder.h"
#include "UI/SmartFolder/SmartFolderController.h"
#include "UI/SmartFolder/SmartFolderManager.h"
#include "Panels/InspectorPanel.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "UI/Interaction/Payload.h"
#include "Editor/DragDropPayloads.h"
#include "Automation/UiReplayCommandIds.h"
#include "Input/KeyCodes.h"
#include "UI/EditorTags.h"
#include "Assets/AssetCreation.h"
#include "Assets/CodeAssetTemplates.h"
#include "Assets/MaterialAsset.h"
#include "Graph/ShaderGraphTemplate.h"
#include "Rendering/Materials/SurfaceShaderTemplate.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <cstdlib>

#include <cassert>
#include <unordered_set>
#include <fstream>
#include <thread>
#include <vector>


namespace GameEngine
{

// Forward declaration of drag state (defined in BookmarksPanel.cpp)
struct DragState {
    bool active;
    std::filesystem::path assetPath;
    std::string scenePath;
    std::string entityId;
};
extern DragState g_DragState;

void SchedulePolyhavenCategoriesLoad(const EditorContext* context, AssetsTreeDataProvider* treeProvider, TreeView* tree,
                                     std::shared_ptr<std::atomic<bool>> alive);

namespace
{
    constexpr float kAssetDragStartThresholdPx = 10.0f;

    // Every Polyhaven network transfer (listing, category, thumbnail and download
    // fetches) is a job of the editor's one "Polyhaven" channel, reached through the
    // editor context, so the browser's fetches and the download manager's downloads
    // share its cap and none holds a compute worker. A transfer still queued at quit is
    // cancelled.
    template <typename F>
    void SubmitPolyhavenTransfer(const EditorContext* context, F&& transfer)
    {
        JobSystem::JobChannel* transfers =
            context && context->DownloadManager ? context->DownloadManager->Transfers() : nullptr;
        if (!transfers)
        {
            Logger::Log::Warning("Assets browser: no Polyhaven download manager in the editor context (or it "
                                 "has shut down); the Polyhaven request was not sent");
            return;
        }
        (void)transfers->Submit(std::forward<F>(transfer));
    }

    // Taken on the UI thread for a job, watcher or thumbnail callback to post back through
    // without touching the widget; empty, refusing every post, when there is no widget.
    UI::UiPostHandle PostHandleFor(UIElement* anchor)
    {
        return anchor ? anchor->GetPostHandle() : UI::UiPostHandle{};
    }

    static float ComputeSavedUiListFontSize()
    {
        constexpr float kDefaultBaseFontSize = 14.0f;
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        double storedBaseFontSize = kDefaultBaseFontSize;
        if (prefs.TryGetDouble("ui.fontSizeBase", storedBaseFontSize))
            storedBaseFontSize = std::clamp(storedBaseFontSize, 12.0, 16.0);

        const int basePx = static_cast<int>(std::round(static_cast<float>(storedBaseFontSize)));
        return static_cast<float>(std::max(8, basePx - 1));
    }

    // Mounted editor assets may carry different path casing than the registry
    // on case-insensitive filesystems (notably /Users vs /users on macOS).
    // AssetPathKey folds that difference away, so this stays lexical — no
    // filesystem query per row.
    static bool PathsReferToSameEntry(const std::filesystem::path& candidate,
                                      const std::string& targetKey)
    {
        return AssetPathKey(candidate) == targetKey;
    }

    static std::string NormalizeVirtualAssetPathString(const std::filesystem::path& p)
    {
        std::string s = p.generic_string();
        if (s.rfind("online:/", 0) == 0 && s.rfind("online://", 0) != 0)
            s.insert(7, "/");
        else if (s.rfind("polyhaven:/", 0) == 0 && s.rfind("polyhaven://", 0) != 0)
            s.insert(10, "/");
        return s;
    }

    // The Name column's label carries this class so a rename session can find it
    // under a pooled TableView row.
    constexpr const char* kListNameLabelClass = "col-name";
    Label* FindDescendantLabelWithClass(UIElement& root, const char* className)
    {
        for (const auto& child : root.GetChildren())
        {
            if (!child)
                continue;
            if (auto* label = dynamic_cast<Label*>(child.get()); label && label->HasClass(className))
                return label;
            if (Label* nested = FindDescendantLabelWithClass(*child, className))
                return nested;
        }
        return nullptr;
    }

    static bool IsAssetBrowserVirtualPath(const std::filesystem::path& p)
    {
        const std::string s = NormalizeVirtualAssetPathString(p);
        return s.rfind("online://", 0) == 0 || s.rfind("polyhaven://", 0) == 0;
    }

    static bool IsCopyDrop(int mods)
    {
        // Editor UX: holding the primary shortcut modifier indicates "copy" (Ctrl on Win/Linux, Cmd on macOS).
        return Input::IsPrimaryShortcutModifier(mods);
    }

    static bool IsBrowserThumbnailVisible(UIElement* view, const std::string& id)
    {
        UIElement* element = view ? view->FindById(id) : nullptr;
        if (!element)
            return false;
        // Parked pooled cells and the inactive grid/list retain their IDs and
        // are hidden through the visibility override (SetElementInvisible),
        // not display:none; a collapsed ancestor panel may use either.
        for (auto* parent = element; parent; parent = parent->GetDfsParent())
        {
            const auto& style = parent->GetResolvedStyle();
            if (style.Layout.DisplayMode == DisplayMode::None || !style.Visual.Visible)
                return false;
        }
        return true;
    }

    static std::filesystem::path MakeUniqueDestinationPath(const std::filesystem::path& destDir, const std::filesystem::path& srcPath)
    {
        if (destDir.empty())
            return {};

        const std::filesystem::path fileName = srcPath.filename();
        if (fileName.empty())
            return {};

        std::error_code ec;
        std::filesystem::path candidate = destDir / fileName;
        if (!std::filesystem::exists(candidate, ec))
            return candidate;

        const std::filesystem::path stem = fileName.stem();
        const std::filesystem::path ext = fileName.extension();

        // Try "Name (N).ext"
        for (int i = 1; i < 10000; ++i)
        {
            std::filesystem::path alt = destDir / (stem.string() + " (" + std::to_string(i) + ")" + ext.string());
            if (!std::filesystem::exists(alt, ec))
                return alt;
        }

        // Give up: return the original candidate and let caller log/handle.
        return candidate;
    }

    // Report every file that landed at `dst`. A directory is only knowable as a file
    // list once the copy has made it, which is why the announcement covers the subtree.
    static void ReportPlacedFiles(ExpectedAssetWrite& write, const std::filesystem::path& dst)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(dst, ec))
        {
            for (auto it = std::filesystem::recursive_directory_iterator(dst, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); ++it)
            {
                if (it->is_regular_file())
                    write.Report(it->path());
            }
            return;
        }
        write.Report(dst);
    }

    static bool CopyAssetPath(const std::filesystem::path& src, const std::filesystem::path& dst)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(src, ec))
            return ::GameEngine::FileSystem::CopyTree(src, dst);
        return ::GameEngine::FileSystem::CopyFileContents(src, dst);
    }

    // Put one payload entry at `dst`. `write` is the announcement covering `dst`, absent
    // only where there is no asset manager for the result to be reported to.
    static bool PlaceAssetPath(const std::filesystem::path& src, const std::filesystem::path& dst,
                               bool copy, AssetManager* assets, ExpectedAssetWrite* write)
    {
        if (copy)
        {
            if (!CopyAssetPath(src, dst))
            {
                LOG_WARNING("Assets: copy failed {} -> {}", src.string(), dst.string());
                return false;
            }
            if (write)
                ReportPlacedFiles(*write, dst);
            return true;
        }

        std::error_code ec;
        std::filesystem::rename(src, dst, ec);
        if (ec)
        {
            // Across volumes a move is a copy plus a delete; the source's registration
            // goes when its deletion is seen, as it does for any file that disappears.
            if (!CopyAssetPath(src, dst))
            {
                LOG_WARNING("Assets: move failed {} -> {} ({})", src.string(), dst.string(), ec.message());
                return false;
            }
            std::filesystem::remove_all(src, ec);
            if (write)
                ReportPlacedFiles(*write, dst);
            return true;
        }

        // A move keeps the asset's identity, and the code that performed the move is what
        // says so — the watcher's report of the same rename is this operation arriving and
        // is consumed. A directory is not a registered asset, so nothing moves for it: the
        // files under it are reported below and register at their new paths, and their old
        // registrations go when a rescan or their deletions are seen. Same outcome as the
        // watcher's own report of a directory rename, which never moved them either.
        if (assets)
            (void)assets->RenameAssetPath(src, dst);
        if (write)
            ReportPlacedFiles(*write, dst);
        return true;
    }

    static std::vector<std::filesystem::path> CopyOrMoveAssetPaths(const Editor::AssetPathsDragPayload& payload,
                                                                   const std::filesystem::path& dest,
                                                                   bool copy,
                                                                   AssetManager* assets)
    {
        std::vector<std::filesystem::path> written;
        written.reserve(payload.paths.size());
        for (const auto& src : payload.paths)
        {
            if (src.empty())
                continue;
            const std::filesystem::path dst = MakeUniqueDestinationPath(dest, src);
            if (dst.empty())
                continue;

            // Announced before anything lands at `dst`: a watcher can report a copied
            // file before this thread reports it, and that report would drive the change
            // a second time.
            bool placed = false;
            if (assets)
            {
                auto write = assets->ExpectWritesUnder(dst);
                placed = PlaceAssetPath(src, dst, copy, assets, &write);
            }
            else
            {
                placed = PlaceAssetPath(src, dst, copy, nullptr, nullptr);
            }

            if (placed)
                written.push_back(dst);
        }
        return written;
    }

    // OPFS (wasm) rejects std::filesystem::copy_file with EPERM because it has
    // no POSIX permission bits to preserve. Streaming bytes works on every mount.
    static bool CopyFileByStreaming(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        std::error_code ec;
        const std::filesystem::path parent = to.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                Logger::Log::Warning("Assets: mkdir '{}' failed ({})", parent.string(), ec.message());
                return false;
            }
        }

        std::ifstream in(from, std::ios::binary);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!in || !out)
        {
            Logger::Log::Warning("Assets: copy '{}' -> '{}' failed to open", from.string(), to.string());
            return false;
        }
        out << in.rdbuf();
        if (!out)
        {
            Logger::Log::Warning("Assets: copy '{}' -> '{}' failed to write", from.string(), to.string());
            return false;
        }
        return true;
    }
}

// Helper: Middle-truncate a filename to fit in limited space
// Shows beginning + "..." + end (preserving file extension)
// Example: "MyVeryLongFileName.png" -> "MyVery...me.png"
static std::string MiddleTruncate(const std::string& text, size_t maxChars)
{
    if (text.length() <= maxChars || maxChars < 8)
        return text;
    
    // Find the extension (if any)
    size_t dotPos = text.rfind('.');
    std::string ext;
    std::string base = text;
    if (dotPos != std::string::npos && dotPos > 0 && text.length() - dotPos <= 6)
    {
        ext = text.substr(dotPos);  // includes the dot
        base = text.substr(0, dotPos);
    }
    
    // Reserve space for "..." (3 chars) and extension
    size_t availableForBase = maxChars - 3 - ext.length();
    if (availableForBase < 4)
        availableForBase = 4;
    
    // Split available space: more at beginning, less at end
    size_t frontChars = (availableForBase * 2) / 3;
    size_t backChars = availableForBase - frontChars;
    if (backChars < 2)
        backChars = 2;
    if (frontChars + backChars > base.length())
        return text;  // No truncation needed for base
    
    return base.substr(0, frontChars) + "..." + base.substr(base.length() - backChars) + ext;
}

namespace
{
static uint8_t HexNibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
    return 0xFFu;
}

static bool TryParseHexColor(const char* s, uint32_t& outArgb)
{
    if (!s || s[0] != '#')
        return false;
    const size_t len = std::strlen(s);
    if (len != 7)
        return false;
    const uint8_t r1 = HexNibble(s[1]);
    const uint8_t r2 = HexNibble(s[2]);
    const uint8_t g1 = HexNibble(s[3]);
    const uint8_t g2 = HexNibble(s[4]);
    const uint8_t b1 = HexNibble(s[5]);
    const uint8_t b2 = HexNibble(s[6]);
    if ((r1 | r2 | g1 | g2 | b1 | b2) == 0xFFu)
        return false;
    const uint8_t r = (uint8_t)((r1 << 4) | r2);
    const uint8_t g = (uint8_t)((g1 << 4) | g2);
    const uint8_t b = (uint8_t)((b1 << 4) | b2);
    outArgb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
}

static uint32_t StatusColorToArgb(const char* color, uint32_t fallback = 0xFFFFFFFFu)
{
    uint32_t parsed = 0;
    return TryParseHexColor(color, parsed) ? parsed : fallback;
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

static std::string MakeVcsCacheKey(const std::filesystem::path& path)
{
    std::string key = path.generic_string();
    for (char& c : key)
    {
        if (c == '\\')
            c = '/';
    }
    return key;
}
} // namespace

AssetsBrowserController::~AssetsBrowserController()
{
    // Unsubscribe before any member the watch callback uses is destroyed. The
    // callback runs on a watcher thread and uses m_FsRefreshPosted.
    // Unsubscribing waits for a running callback, so none runs after this line.
    m_AssetsWatchSub.reset();
    // Turns away the refresh a callback posted to the UI thread: it can still be
    // queued there, and it checks ControllerAlive before it touches the controller.
    {
        std::lock_guard<std::mutex> lock(m_FsInbox->Mutex);
        m_FsInbox->ControllerAlive = false;
    }
    // Wait only for a post already in progress, never for the directory scan.
    // Workers retain the mutex and alive flag after this controller is gone.
    std::lock_guard<std::mutex> postLock(*m_ScanPostMutex);
    // Signal background threads that this controller is being destroyed.
    m_Alive->store(false, std::memory_order_release);
    // Bump generation so any in-flight Polyhaven loads become stale.
    ++m_PolyhavenLoadGeneration;
}

void AssetsBrowserController::NotifyVcsStatusDirty()
{
    if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
        vcs->RefreshStatus();
}

void AssetsBrowserController::RefreshCachedVcsUiSettings()
{
    const std::string currTypeId = Editor::EditorVcsProviderRegistry::Get().ActiveTypeId();
    if (!m_VcsUiDirty && currTypeId == m_CachedVcsTypeId)
        return;

    if (currTypeId != m_CachedVcsTypeId)
        InvalidateVcsStatusCache();

    m_CachedVcsTypeId = currTypeId;
    m_CachedVcsUiSettings = {};

    Editor::EditorVcsProviderDescriptor provider;
    if (!currTypeId.empty() &&
        Editor::EditorVcsProviderRegistry::Get().TryGet(currTypeId, provider) &&
        provider.BadgeUiSettings)
    {
        const Editor::VcsBadgeUiSettings badge = provider.BadgeUiSettings();
        m_CachedVcsUiSettings.showStatusIcons = badge.ShowStatusIcons;
        m_CachedVcsUiSettings.colorWholeText = badge.ColorWholeText;
        m_CachedVcsUiSettings.colorDotOnly = badge.ColorDotOnly;
        m_CachedVcsUiSettings.hideDotForClean = badge.HideDotForClean;
    }

    m_VcsUiDirty = false;
}

void AssetsBrowserController::RefreshCachedTruncationSettings()
{
    if (m_TruncationPrefsLoaded)
        return;

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    if (!m_TruncationEnabledOverride)
    {
        bool enabled = m_TruncationEnabled;
        if (prefs.TryGetBool("ui.textTruncation", enabled))
            m_TruncationEnabled = enabled;
    }

    if (!m_TruncationThresholdOverride)
    {
        double stored = static_cast<double>(m_TruncationThreshold);
        if (prefs.TryGetDouble("ui.gridTruncationThreshold", stored))
            m_TruncationThreshold = static_cast<float>(stored);
    }

    m_TruncationPrefsLoaded = true;
}

void AssetsBrowserController::RefreshCachedListFontSize()
{
    if (m_ListFontSizePrefsLoaded)
        return;

    m_ListFontSize = ComputeSavedUiListFontSize();
    m_ListFontSizePrefsLoaded = true;
}

void AssetsBrowserController::InvalidateThumbnailForPath(const std::filesystem::path& assetPath)
{
    if (!m_Context || !m_Context->Thumbnails || !m_Grid || !m_GridProvider)
        return;

    const GridId gid = m_GridProvider->FindIdForPath(assetPath);
    if (gid == 0)
        return;

    // Build the stable thumb element ID (mirrors the cell binder's naming convention).
    const std::string cellId = std::string("gridcell-")
        + std::to_string(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(m_Grid)))
        + "-" + std::to_string(static_cast<uint64_t>(gid));
    const std::string thumbId = "thumb-" + cellId;

    // Request the (now-updated) thumbnail and post an update to the cell element.
    const std::string immediate = m_Context->Thumbnails->GetOrRequestBrowser(
        assetPath, 128, thumbId,
        [this, alive = m_Alive, thumbId]()
        {
            return alive->load(std::memory_order_acquire) &&
                   IsBrowserThumbnailVisible(m_Grid, thumbId);
        },
        [this, alive = m_Alive, thumbId, post = PostHandleFor(m_Grid)](const std::string& rel)
        {
            if (rel.empty()) return;
            post.Post([this, alive, thumbId, rel]()
            {
                if (!alive->load(std::memory_order_acquire)) return;
                if (auto* el = m_Grid->FindById(thumbId))
                    UI::Layout::SetBackgroundPath(*el, rel);
            });
        });

    if (!immediate.empty())
    {
        if (auto* el = m_Grid->FindById(thumbId))
            UI::Layout::SetBackgroundPath(*el, immediate);
    }
}

void AssetsBrowserController::InvalidateVcsStatusCache()
{
    m_VcsStatusCache.clear();
    m_VcsStatusQueued.clear();
    m_VcsStatusQueue.clear();
    m_VcsStatusQueueIndex = 0;
    m_VcsStatusBatchPosted.store(false);
}

void AssetsBrowserController::InvalidatePolyhavenDownloadCache()
{
    if (m_Polyhaven)
        m_Polyhaven->ClearDownloadCache();
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List)
        m_List->RefreshFromProvider();
}

void AssetsBrowserController::InvalidateVcsUiSettings()
{
    m_VcsUiDirty = true;
}

void AssetsBrowserController::InvalidateCachedDisplaySettings()
{
    m_TruncationPrefsLoaded = false;
    m_ListFontSizePrefsLoaded = false;
}

bool AssetsBrowserController::TryGetCachedVcsStatus(const std::filesystem::path& path, VCSFileStatus& outStatus)
{
    if (path.empty())
        return false;

    const std::string key = MakeVcsCacheKey(path);
    auto it = m_VcsStatusCache.find(key);
    if (it != m_VcsStatusCache.end())
    {
        outStatus = it->second;
        return true;
    }

    QueueVcsStatusRequest(path, key);
    return false;
}

void AssetsBrowserController::QueueVcsStatusRequest(const std::filesystem::path& path, const std::string& key)
{
    if (path.empty())
        return;

    if (!m_VcsStatusQueued.insert(key).second)
        return;

    m_VcsStatusQueue.push_back(VcsStatusRequest{path, key});

    UIElement* anchor = m_List ? static_cast<UIElement*>(m_List)
        : (m_Grid ? static_cast<UIElement*>(m_Grid) : static_cast<UIElement*>(m_Tree));
    if (!anchor)
    {
        m_VcsStatusBatchPosted.store(false);
        return;
    }

    if (!m_VcsStatusBatchPosted.exchange(true))
    {
        anchor->PostAction([this]()
                           { ProcessPendingVcsStatus(); });
    }
}

void AssetsBrowserController::ProcessPendingVcsStatus()
{
    GE_CPU_PROFILE_SCOPE("AssetsBrowserController.ProcessPendingVcsStatus");
    m_VcsStatusBatchPosted.store(false);
    if (m_VcsStatusQueueIndex >= m_VcsStatusQueue.size())
        return;

    auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
    if (!vcs || !vcs->IsRepository())
    {
        InvalidateVcsStatusCache();
        if (m_TreeProvider)
            m_TreeProvider->MarkAllChanged();
        if (m_GridProvider)
            m_GridProvider->MarkAllChanged();
        if (m_Tree)
            m_Tree->RefreshFromProvider();
        if (m_Grid)
            m_Grid->RefreshFromProvider();
        if (m_List && m_ListProvider)
            m_List->RefreshFromProvider();
        return;
    }

    std::unordered_set<GridId> changedGridIds;
    std::unordered_set<TreeId> changedTreeIds;

    constexpr size_t kBatchSize = 64;
    size_t processed = 0;
    while (processed < kBatchSize && m_VcsStatusQueueIndex < m_VcsStatusQueue.size())
    {
        const VcsStatusRequest& req = m_VcsStatusQueue[m_VcsStatusQueueIndex++];
        const VCSFileStatus status = vcs->GetFileStatus(req.path);
        const auto it = m_VcsStatusCache.find(req.key);
        if (it == m_VcsStatusCache.end() || it->second != status)
        {
            if (m_GridProvider)
            {
                const GridId id = m_GridProvider->FindIdForPath(req.path);
                if (id != 0)
                    changedGridIds.insert(id);
            }
            if (m_TreeProvider)
            {
                TreeId id = m_TreeProvider->FindIdForPath(req.path);
                if (id == 0 && !req.path.empty())
                    id = m_TreeProvider->FindIdForPath(req.path.parent_path());
                if (id != 0)
                    changedTreeIds.insert(id);
            }
        }
        m_VcsStatusCache[req.key] = status;
        m_VcsStatusQueued.erase(req.key);
        ++processed;
    }

    if (m_VcsStatusQueueIndex >= m_VcsStatusQueue.size())
    {
        m_VcsStatusQueue.clear();
        m_VcsStatusQueueIndex = 0;
    }

    // Prevent the cache from growing without bound across many directory navigations.
    // When the cache exceeds a generous threshold, clear it and let entries be re-queried
    // on demand. This bounds memory to O(kMaxCacheEntries * avg_key_size).
    constexpr size_t kMaxCacheEntries = 8192;
    if (m_VcsStatusCache.size() > kMaxCacheEntries)
    {
        m_VcsStatusCache.clear();
    }

    if (processed > 0 && (!changedGridIds.empty() || !changedTreeIds.empty()))
    {
        if (m_GridProvider && !changedGridIds.empty())
        {
            std::vector<GridId> ids;
            ids.reserve(changedGridIds.size());
            for (const auto& id : changedGridIds)
                ids.push_back(id);
            m_GridProvider->MarkChangedBatch(ids);
        }
        if (m_TreeProvider && !changedTreeIds.empty())
        {
            std::vector<TreeId> ids;
            ids.reserve(changedTreeIds.size());
            for (const auto& id : changedTreeIds)
                ids.push_back(id);
            m_TreeProvider->MarkChangedBatch(ids);
        }
        if (m_Tree)
            m_Tree->RefreshFromProvider();
        if (m_Grid)
            m_Grid->RefreshFromProvider();
        if (m_List && m_ListProvider)
            m_List->RefreshFromProvider();
    }

    if (m_VcsStatusQueueIndex < m_VcsStatusQueue.size())
    {
        UIElement* anchor = m_List ? static_cast<UIElement*>(m_List)
            : (m_Grid ? static_cast<UIElement*>(m_Grid) : static_cast<UIElement*>(m_Tree));
        if (anchor && !m_VcsStatusBatchPosted.exchange(true))
        {
            anchor->PostAction([this]()
                               { ProcessPendingVcsStatus(); });
        }
    }
}

void AssetsBrowserController::DeferThumbnailsAfterNavigation()
{
    // Navigation and smart-folder replacement get the same settle window as
    // scrolling. Visible cells supply requests; do not import folder-order
    // candidates that may never appear on screen.
    ModelThumbnailHandler::NoteAssetListScrollActivity();
}

void AssetsBrowserController::ReloadCurrentDirectory()
{
    if (!m_GridProvider)
        return;
    const auto dir = m_GridProvider->GetCurrentDirectory();
    if (dir.empty())
        return; // Smart-folder / virtual-folder mode — nothing to re-enumerate.
    m_GridProvider->LoadDirectory(dir);
}

void AssetsBrowserController::OnPhantomRemoveFromScene(const GUID& guid)
{
    if (!m_Context || !m_Context->MissingAssets)
        return;
    const size_t cleared = m_Context->MissingAssets->ClearAllReferencesToActive(guid);
    if (cleared > 0 && m_Context->OnSceneDirty)
        m_Context->OnSceneDirty();
}


void AssetsBrowserController::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    m_VcsUiDirty = true;
    InvalidateVcsStatusCache();
    if (m_GridProvider)
        m_GridProvider->SetMissingAssetTracker(m_Context ? m_Context->MissingAssets : nullptr);
    if (m_Context && m_Context->MainWindow)
    {
        if (!m_ContextMenu)
        {
            m_ContextMenu = std::make_unique<EditorContextMenu>(m_Context->MainWindow);
        }
        else
        {
            m_ContextMenu->SetWindow(m_Context->MainWindow);
        }
        
        // Set up VCS revert callback with undo support
        m_ContextMenu->SetOnVcsRevert([this](const std::filesystem::path& path) {
            auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            if (!vcs || !vcs->IsRepository())
            {
                return;
            }

            // Undoable revert works through the provider-agnostic integration:
            // backup bytes, Revert(), restore on undo.
            if (m_Context && m_Context->UndoRedo)
            {
                m_Context->UndoRedo->Execute(std::make_unique<Editor::VcsRevertCommand>(path));
            }
            else if (vcs->Revert(path))
            {
                Logger::Log::Info("VCS: Reverted {}", path.filename().string());
            }
            else
            {
                Logger::Log::Warning("VCS: Failed to revert {}", path.filename().string());
            }
        });
        if (m_OnAddTag)
            m_ContextMenu->SetOnAddTag(m_OnAddTag);
        if (m_OnAssignTag)
            m_ContextMenu->SetOnAssignTag(m_OnAssignTag);
        if (m_OnOpenSettingsToTags)
            m_ContextMenu->SetOnOpenSettingsToTags(m_OnOpenSettingsToTags);
    }
    if (m_Context && !m_Context->AssetsRoot.empty())
    {
        SetAssetsRoot(m_Context->AssetsRoot);
    }
}

void AssetsBrowserController::SetOnAddTag(std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> cb)
{
    m_OnAddTag = std::move(cb);
    if (m_ContextMenu)
        m_ContextMenu->SetOnAddTag(m_OnAddTag);
}

void AssetsBrowserController::SetOnAssignTag(std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> cb)
{
    m_OnAssignTag = std::move(cb);
    if (m_ContextMenu)
        m_ContextMenu->SetOnAssignTag(m_OnAssignTag);
}

void AssetsBrowserController::SetOnOpenSettingsToTags(std::function<void()> cb)
{
    m_OnOpenSettingsToTags = std::move(cb);
    if (m_ContextMenu)
        m_ContextMenu->SetOnOpenSettingsToTags(m_OnOpenSettingsToTags);
}

void AssetsBrowserController::SetVcsDialogCallbacks(
    std::function<void(const std::string&)> onShowCommitDialog,
    std::function<void(const std::filesystem::path&)> onShowVcsLog)
{
    if (m_ContextMenu)
    {
        m_ContextMenu->SetOnShowCommitDialog(std::move(onShowCommitDialog));
        m_ContextMenu->SetOnShowVcsLog(std::move(onShowVcsLog));
    }
}

void AssetsBrowserController::SetOnShowDiff(std::function<void(const std::filesystem::path&)> onShowDiff)
{
    if (m_ContextMenu)
    {
        m_ContextMenu->SetOnShowDiff(std::move(onShowDiff));
    }
}

// Defined later in this file; forward-declared so the grid + list binders share one copy of the
// engine:-prefix vs filesystem-path thumbnail resolution.
static void ApplyThumbToElement(UIElement* thumb, const std::string& relOrEngine);

void AssetsBrowserController::Initialize(TreeView* tree, GridView* grid, ListView* list)
{
    m_Tree = tree;
    m_Grid = grid;
    m_List = list;

    m_Polyhaven = std::make_unique<PolyhavenService>();
    m_TreeProvider = std::make_unique<AssetsTreeDataProvider>();
    m_GridProvider = std::make_unique<AssetsGridDataProvider>();
    m_GridProvider->SetAssetRegistry(&EngineCore::GetInstance().GetAssetManager().GetRegistry());
    m_GridProvider->SetTagSortKeyResolver([](const std::filesystem::path& p) {
        std::string tagMeta;
        auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        if (!reg.TryGetMetaValue(p, "tags", tagMeta) || tagMeta.empty())
            return std::string();
        std::vector<std::string> parts;
        for (size_t i = 0; i < tagMeta.size();)
        {
            size_t j = tagMeta.find(',', i);
            if (j == std::string::npos)
                j = tagMeta.size();
            std::string part = tagMeta.substr(i, j - i);
            const size_t start = part.find_first_not_of(" \t");
            if (start != std::string::npos)
            {
                const size_t end = part.find_last_not_of(" \t");
                part = part.substr(start, end == std::string::npos ? part.size() - start : end - start + 1);
            }
            else
                part.clear();
            if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
                parts.push_back(part);
            i = j + (j < tagMeta.size() ? 1 : 0);
        }
        std::sort(parts.begin(), parts.end());
        std::string key;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                key += ", ";
            key += parts[i];
        }
        return key;
    });
    m_TreeSelection = std::make_unique<UI::Interaction::SelectionModel>();
    m_ItemSelection = std::make_unique<UI::Interaction::SelectionModel>();

    // Unified selection notification for the asset item dataset (shared by grid + list).
    // This keeps selection handling centralized and avoids view-to-view coupling.
    m_ItemSelection->SetOnChanged([this]()
                                  {
        if (!m_GridProvider)
            return;
        if (m_InspectorAssetFireSuppressionDepth != 0)
            return;
        const std::vector<UI::Interaction::ItemId> ids = m_ItemSelection->GetSelection();
        std::vector<std::filesystem::path> paths;
        paths.reserve(ids.size());
        for (auto raw : ids)
        {
            std::filesystem::path p = m_GridProvider->GetPath(static_cast<GridId>(raw));
            if (IsAssetBrowserVirtualPath(p))
                continue;
            // For downloaded Polyhaven items, resolve the actual model file
            // so the asset preview panel shows a 3D rendered thumbnail.
            if (!m_PolyhavenTypeKey.empty() && !p.empty())
            {
                std::string slug = p.stem().string();
                if (!slug.empty() && slug != "loading")
                {
                    std::filesystem::path modelFile = PolyhavenService::FindDownloadedFile(slug, m_AssetsRoot);
                    if (!modelFile.empty())
                        p = modelFile;
                }
            }
            paths.push_back(std::move(p));
        }

        // Buffer the paths and defer firing so the inspector does not change
        // on mouse-press — leaves current inspector content (and its drop
        // targets) visible while the user drags an asset. The deferred check
        // polls until the mouse is released; if a drag occurs during the
        // press, the pending paths are discarded.
        m_PendingInspectorSelectPaths = std::move(paths);
        m_DragObservedDuringPress = false;
        CheckPendingInspectorSelect(); });
    
    if (m_List && m_GridProvider)
    {
        m_ListProvider = std::make_unique<AssetsListDataProvider>(m_GridProvider.get());
    }

    if (m_Tree)
    {
        m_Tree->SetDataProvider(m_TreeProvider.get());
        m_Tree->SetSelectionModel(m_TreeSelection.get());

        // Drag folders from the tree (to move into other folders).
        m_Tree->SetDragPayloadBuilder([this](TreeId dragSourceId) -> UI::Interaction::DragPayload {
            if (!m_TreeSelection || !m_TreeProvider || dragSourceId == 0)
                return {};
            // Drag the whole selection when the grabbed row is part of it; otherwise just that row.
            const std::vector<UI::Interaction::ItemId> ids = m_TreeSelection->IsSelected(dragSourceId)
                                                                 ? m_TreeSelection->GetSelection()
                                                                 : std::vector<UI::Interaction::ItemId>{ dragSourceId };
            if (ids.empty())
                return {};

            Editor::AssetPathsDragPayload p;
            p.paths.reserve(ids.size());
            for (auto raw : ids)
            {
                const TreeId tid = static_cast<TreeId>(raw);
                if (tid == 0)
                    continue;
                if (m_TreeProvider->IsSmartFolder(tid) || m_TreeProvider->IsSmartFoldersSection(tid))
                    continue;
                if (m_TreeProvider->IsOnlineAssetsSection(tid) || m_TreeProvider->IsPolyhaven(tid) || m_TreeProvider->IsPolyhavenCategory(tid) || m_TreeProvider->IsPolyhavenCategoryChild(tid))
                    continue;
                p.paths.push_back(m_TreeProvider->GetPath(tid));
            }
            if (p.paths.empty())
                return {};

            const std::string first = p.paths[0].filename().string();
            const std::string label = (p.paths.size() == 1) ? first : (first + " + " + std::to_string(p.paths.size() - 1));
            p.displayLabel = label;
            UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
            out.DisplayLabel = label;
            if (out.IsValid())
            {
                std::error_code ec;
                const auto* pp = out.TryGet<Editor::AssetPathsDragPayload>();
                if (pp && !pp->paths.empty())
                {
                    const bool isDir = std::filesystem::is_directory(pp->paths[0], ec);
                    out.GhostIconKind = isDir ? UI::Interaction::DragGhostIconKind::AssetFolder : UI::Interaction::DragGhostIconKind::AssetFile;
                    if (!isDir)
                    {
                        const std::string ext = pp->paths[0].extension().string();
                        if (ext == ".scene")
                            out.GhostThumbnailEngineName = "Icons/sceneicon.png";
                        else if (m_Context && m_Context->Thumbnails)
                        {
                            const std::string thumb = m_Context->Thumbnails->GetOrRequest(pp->paths[0], 128, [](const std::string&) {});
                            if (!thumb.empty())
                                out.GhostThumbnailEngineName = thumb;
                        }
                    }
                }
            }
            return out;
        });

        // Accept asset drags onto folders.
        m_Tree->SetAcceptsPayload([](UI::Interaction::PayloadTypeId tid) {
            return tid == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
        });
        m_Tree->SetOnCanDrop([this](const UI::Interaction::DropRequest& req) -> UI::Interaction::DropFeedback {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload)
                return {false, "Wrong payload"};
            if (!m_TreeProvider || !m_GridProvider)
                return {false, "No providers"};

            std::filesystem::path dest = (req.hit.TargetId != 0) ? m_TreeProvider->GetPath(req.hit.TargetId)
                                                                 : m_GridProvider->GetCurrentDirectory();
            if (dest.empty())
                return {false, "No destination"};
            if (req.hit.TargetId != 0)
            {
                if (m_TreeProvider->IsSmartFolder(req.hit.TargetId) || m_TreeProvider->IsSmartFoldersSection(req.hit.TargetId))
                    return {false, "Cannot drop onto smart folders"};
                if (m_TreeProvider->IsOnlineAssetsSection(req.hit.TargetId) || m_TreeProvider->IsPolyhaven(req.hit.TargetId) || m_TreeProvider->IsPolyhavenCategory(req.hit.TargetId) || m_TreeProvider->IsPolyhavenCategoryChild(req.hit.TargetId))
                    return {false, "Cannot drop onto online assets"};
            }

            const std::string destKey = AssetPathKey(dest);
            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            for (const auto& src : payload->paths)
            {
                if (src.empty())
                    continue;
                if (AssetPathKey(src) == destKey)
                    return {false, "Cannot move into itself"};
                if (std::filesystem::is_directory(src))
                {
                    if (copy)
                        return {false, "Copy folder not supported"};
                    // Prevent moving a folder into its own descendant.
                    if (IsUnderAssetDir(src, dest))
                        return {false, "Cannot move folder into its descendant"};
                }
                if (IsDirectChildOfAssetDir(dest, src))
                    return {false, "Already in destination"};
            }

            return {true, {}};
        });
        m_Tree->SetOnPerformDrop([this](const UI::Interaction::DropRequest& req) {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload || !m_TreeProvider || !m_GridProvider)
                return;

            std::filesystem::path dest = (req.hit.TargetId != 0) ? m_TreeProvider->GetPath(req.hit.TargetId)
                                                                 : m_GridProvider->GetCurrentDirectory();
            if (dest.empty())
                return;

            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            const auto written = CopyOrMoveAssetPaths(*payload, dest, copy, m_Context ? m_Context->Assets : nullptr);
            RevealImportedAssets(dest, written);
            NotifyVcsStatusDirty();
        });
        // Selecting a folder in the tree drives the grid's directory
        m_Tree->SetShowRoot(false);
        ApplyInitialTreeExpansion();

        m_Tree->SetOnSelectionChanged([this](TreeId id)
                                      {
            if (m_SuppressTreeSelectSideEffects) return;
            if (!m_TreeProvider || !m_GridProvider) return;
            
            // Check if this is a smart folder selection
            if (m_TreeProvider->IsSmartFolder(id)) {
                std::string sfId = m_TreeProvider->GetSmartFolderId(id);
                if (!sfId.empty()) {
                    SelectSmartFolder(sfId);
                    return;
                }
            }
            
            // Check if this is the smart folders section (don't navigate, but clear inspector)
            if (m_TreeProvider->IsSmartFoldersSection(id)) {
                if (m_SmartFolderController)
                    m_SmartFolderController->ClearSelection();
                return;
            }
            
            // Online Assets section: show child nodes (e.g. "Polyhaven") as virtual folders
            if (m_TreeProvider->IsOnlineAssetsSection(id)) {
                if (m_SmartFolderController)
                    m_SmartFolderController->ClearSelection();
                m_PolyhavenTypeKey.clear();
                std::vector<std::pair<std::string, std::filesystem::path>> folders;
                int childCount = m_TreeProvider->GetChildCount(id);
                for (int i = 0; i < childCount; ++i) {
                    TreeId childId = m_TreeProvider->GetChildId(id, i);
                    const char* label = m_TreeProvider->GetLabel(childId);
                    if (label)
                        folders.push_back({label, std::filesystem::path("online://polyhaven")});
                }
                m_GridProvider->LoadVirtualFolders(folders);
                if (m_Grid) m_Grid->RefreshFromProvider();
                if (m_List && m_ListProvider) m_List->RefreshFromProvider();
                return;
            }
            // Polyhaven root: show type nodes (HDRIs, Textures, Models) as virtual folders
            if (m_TreeProvider->IsPolyhaven(id)) {
                if (m_SmartFolderController)
                    m_SmartFolderController->ClearSelection();
                m_PolyhavenTypeKey.clear();
                std::vector<std::pair<std::string, std::filesystem::path>> folders;
                int childCount = m_TreeProvider->GetChildCount(id);
                for (int i = 0; i < childCount; ++i) {
                    TreeId childId = m_TreeProvider->GetChildId(id, i);
                    const char* label = m_TreeProvider->GetLabel(childId);
                    std::string cat = m_TreeProvider->GetPolyhavenCategory(childId);
                    if (label && !cat.empty())
                        folders.push_back({label, std::filesystem::path("polyhaven://" + cat)});
                }
                m_GridProvider->LoadVirtualFolders(folders, std::filesystem::path("online://"));
                if (m_Grid) m_Grid->RefreshFromProvider();
                if (m_List && m_ListProvider) m_List->RefreshFromProvider();
                return;
            }
            // Polyhaven type node (HDRIs, Textures, Models): show category children as virtual folders
            if (m_TreeProvider->IsPolyhavenCategory(id)) {
                if (m_SmartFolderController)
                    m_SmartFolderController->ClearSelection();
                m_PolyhavenTypeKey.clear();
                std::string typeSlug = m_TreeProvider->GetPolyhavenCategory(id);
                std::vector<std::pair<std::string, std::filesystem::path>> folders;
                int childCount = m_TreeProvider->GetChildCount(id);
                for (int i = 0; i < childCount; ++i) {
                    TreeId childId = m_TreeProvider->GetChildId(id, i);
                    const char* label = m_TreeProvider->GetLabel(childId);
                    std::string catType, catSlug;
                    m_TreeProvider->GetPolyhavenTypeAndCategory(childId, catType, catSlug);
                    if (label && !catSlug.empty())
                        folders.push_back({label, std::filesystem::path("polyhaven://" + catType + "/" + catSlug)});
                }
                m_GridProvider->LoadVirtualFolders(folders, std::filesystem::path("polyhaven://"));
                if (m_Grid) m_Grid->RefreshFromProvider();
                if (m_List && m_ListProvider) m_List->RefreshFromProvider();
                return;
            }
            // Polyhaven category directory (All, Aerial, Brick, ...): load assets for that type+category
            if (m_TreeProvider->IsPolyhavenCategoryChild(id)) {
                std::string phType, phSlug;
                m_TreeProvider->GetPolyhavenTypeAndCategory(id, phType, phSlug);
                if (!phType.empty() && phSlug != "loading") {
                    SelectPolyhavenCategoryFilter(phType, phSlug);
                    return;
                }
            }
            
            auto path = m_TreeProvider->GetPath(id);
            if (!path.empty() && m_Tree) {
                // Clear smart folder / Polyhaven selection
                if (m_SmartFolderController)
                    m_SmartFolderController->ClearSelection();
                m_PolyhavenTypeKey.clear();
                
                // Show the selected folder in inspector
                if (m_OnSelectAssets && m_InspectorAssetFireSuppressionDepth == 0) {
                    m_OnSelectAssets({path});
                }
                
                // Route folder selection through the canonical navigation path
                // so the grid's thumbnail settle window and orbit-focus clear
                // fire. Selecting a folder in the tree used to call LoadDirectory
                // directly and skip both, so the new folder's tiles stayed blank
                // until each was clicked; a grid double-click went through here
                // and worked. Same-dir skip + per-frame coalescing live inside.
                NavigateToDirectory(path);
            } });
        // Double-click in tree: navigate grid to that folder
        m_Tree->SetOnItemActivated([this](TreeId id)
                                   {
            if (!m_TreeProvider || !m_GridProvider || !m_Tree) return;
            auto path = m_TreeProvider->GetPath(id);
            if (path.empty()) return;
            // Same canonical path as tree selection (above) — fires the
            // thumbnail settle window + orbit-focus clear that a bare LoadDirectory
            // would skip.
            NavigateToDirectory(path); });

        // Right-click context menu for the directory tree.
        // - Item: show directory menu for that folder
        // - Empty space: show directory menu for current grid directory (if any)
        m_Tree->SetOnContextMenu([this](TreeId id, float x, float y)
                                 {
            if (!m_TreeProvider || !m_GridProvider || !m_ContextMenu || !m_Context) return;

            // Handle Smart Folders section header
            if (m_TreeProvider->IsSmartFoldersSection(id))
            {
                m_ContextMenu->ShowSmartFolderMenu(x, y,
                    [this](EditorContextMenu::SmartFolderAction action)
                    {
                        if (action == EditorContextMenu::SmartFolderAction::NewSmartFolder)
                            CreateNewSmartFolder();
                    });
                return;
            }

            // Individual smart folder item — show smart-folder-specific menu
            if (m_TreeProvider->IsSmartFolder(id))
            {
                std::string sfId = m_TreeProvider->GetSmartFolderId(id);
                m_ContextMenu->ShowSmartFolderMenu(x, y,
                    [this, sfId](EditorContextMenu::SmartFolderAction action)
                    {
                        switch (action)
                        {
                            case EditorContextMenu::SmartFolderAction::NewSmartFolder:
                                CreateNewSmartFolder();
                                break;
                            case EditorContextMenu::SmartFolderAction::Delete:
                                if (m_SmartFolderController)
                                {
                                    m_SmartFolderController->GetManager().Delete(sfId);
                                }
                                break;
                        }
                    });
                return;
            }

            std::filesystem::path dir;
            if (id == 0)
            {
                dir = m_GridProvider->GetCurrentDirectory();
            }
            else
            {
                dir = m_TreeProvider->GetPath(id);
            }

            if (dir.empty())
                return;

            m_ContextMenu->ShowDirectoryMenu(dir, x, y,
                [this](const std::filesystem::path& directory,
                       EditorContextMenu::DirectoryAction action)
                { HandleDirectoryAction(directory, action); },
                /*includeDelete=*/id != 0);
        });
        
        // Custom row binding to add VCS status dot to folder names
        m_Tree->SetOnRowBound([this](TreeId id, UIElement* row) {
            if (!row || !m_TreeProvider)
                return;

            // Smart folder row styling
            if (m_TreeProvider->IsSmartFolder(id))
            {
                row->AddClass("smart-folder");
                row->RemoveClass("smart-folders-section");
            }
            else if (m_TreeProvider->IsSmartFoldersSection(id))
            {
                row->AddClass("smart-folders-section");
                row->RemoveClass("smart-folder");
            }
            else
            {
                row->RemoveClass("smart-folder");
                row->RemoveClass("smart-folders-section");
            }

            RefreshCachedVcsUiSettings();
            
            // Find the toggle, title, folder icon, and dot elements
            Label* toggleLabel = nullptr;
            Label* titleLabel = nullptr;
            UIElement* dotElement = nullptr;
            UIElement* folderIconElement = nullptr;
            std::vector<UIElement*> extraFolderIcons;
            std::vector<UIElement*> extraDots;
            
            const auto& children = row->GetChildren();
            for (const auto& child : children)
            {
                if (child->HasClass("tree-git-dot"))
                {
                    if (!dotElement)
                        dotElement = child.get();
                    else
                        extraDots.push_back(child.get());
                    continue;
                }
                if (child->HasClass("tree-folder-icon"))
                {
                    if (!folderIconElement)
                        folderIconElement = child.get();
                    else
                        extraFolderIcons.push_back(child.get());
                    continue;
                }
                if (auto* label = dynamic_cast<Label*>(child.get()))
                {
                    if (toggleLabel == nullptr)
                    {
                        toggleLabel = label;
                        continue;
                    }
                    if (titleLabel == nullptr)
                    {
                        titleLabel = label;
                    }
                }
            }
            
            // Do NOT structurally remove pooled row children during binding.
            // Virtualized TreeView rows are reused and removal can lead to late Yoga rebuilds,
            // flicker, and duplicate/ghost visuals. Hide extras instead.
            for (auto* extra : extraDots)
            {
                if (extra)
                    extra->Overrides().Set(Style::Display, DisplayMode::None);
            }

            // Hide any extra folder icons (avoid structural removal in pooled rows).
            for (auto* extra : extraFolderIcons)
            {
                if (extra)
                    extra->Overrides().Set(Style::Display, DisplayMode::None);
            }

            if (!titleLabel)
                return;
            
            // Create folder icon element if it doesn't exist (separate from title's background).
            // Create folder icon element if it doesn't exist (separate from title's background).
            // IMPORTANT: Avoid removing children from pooled rows; but creating once is OK.
            if (!folderIconElement)
            {
                auto newIcon = std::make_unique<UIElement>();
                newIcon->AddClass("tree-folder-icon");
                folderIconElement = newIcon.get();
                row->AddChild(std::move(newIcon));
            }

            // Hide folder icon for top-level Assets and Smart Folders section
            if (folderIconElement)
            {
                if (m_TreeProvider->IsSmartFoldersSection(id) || m_TreeProvider->IsAssetsRoot(id) || m_TreeProvider->IsOnlineAssetsSection(id))
                    folderIconElement->AddClass("hidden");
                else
                    folderIconElement->RemoveClass("hidden");
            }
            
            // Apply current tree icon size to folder icon
            if (folderIconElement && m_Tree)
            {
                float iconSize = m_Tree->GetIconSize();
                folderIconElement->Overrides()
                    .Set(Style::Width, StyleLength::Px(iconSize))
                    .Set(Style::Height, StyleLength::Px(iconSize))
                    .Set(Style::MinWidth, StyleLength::Px(iconSize))
                    .Set(Style::MinHeight, StyleLength::Px(iconSize));
            }
            
            // Remove the default folder icon from title (we use separate element now)
            titleLabel->AddClass("tree-title-no-icon");
            
            // Get the original folder name from provider
            const char* folderName = m_TreeProvider->GetLabel(id);
            std::string displayName = folderName ? folderName : "";
            if (ToLowerAscii(displayName) == "materials")
                row->AddClass("asset-tree-materials-folder");
            else
                row->RemoveClass("asset-tree-materials-folder");

            const auto path = m_TreeProvider->GetPath(id);
            if (path.empty())
            {
                if (dotElement)
                    dotElement->Overrides().Set(Style::Display, DisplayMode::None);
                titleLabel->SetText(displayName);
                titleLabel->Overrides().Reset(Style::Color);
                return;
            }
            
            // Use VCS abstraction
            auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            if (!vcs || !vcs->IsRepository())
            {
                // No VCS repo - hide dot if exists, show plain name
                if (dotElement)
                    dotElement->Overrides().Set(Style::Display, DisplayMode::None);
                titleLabel->SetText(displayName);
                titleLabel->Overrides().Reset(Style::Color);
                return;
            }
            
            const bool showStatusIcons = m_CachedVcsUiSettings.showStatusIcons;
            const bool colorWholeText = m_CachedVcsUiSettings.colorWholeText;
            const bool colorDotOnly = m_CachedVcsUiSettings.colorDotOnly;

            VCSFileStatus status = VCSFileStatus::Clean;
            const char* statusColor = nullptr;
            uint32_t statusArgb = 0;
            if (showStatusIcons)
            {
                (void)TryGetCachedVcsStatus(path, status);
                statusColor = Editor::VcsStatusToColor(status);
                statusArgb = StatusColorToArgb(statusColor);
            }
            
            if (showStatusIcons && statusColor && status != VCSFileStatus::Clean)
            {
                // Create dot element if it doesn't exist
                // Create dot element if it doesn't exist
                if (!dotElement)
                {
                    auto newDot = std::make_unique<UIElement>();
                    newDot->AddClass("tree-git-dot");
                    dotElement = newDot.get();
                    row->AddChild(std::move(newDot));
                }
                
                // Show the dot with colored tint
                if (colorDotOnly || colorWholeText)
                {
                    dotElement->Overrides()
                        .Set(Style::BackgroundTint, statusArgb)
                        .Set(Style::Display, DisplayMode::Flex);
                }
                else
                {
                    dotElement->Overrides().Reset(Style::BackgroundTint);
                    dotElement->Overrides().Set(Style::Display, DisplayMode::Flex);
                }

                // Set title text (without dot) and optionally color it
                // Never color text when status is Clean or NotConfigured
                titleLabel->SetText(displayName);
                if (colorWholeText && status != VCSFileStatus::Clean && status != VCSFileStatus::NotConfigured)
                    titleLabel->Overrides().Set(Style::Color, (uint32_t)statusArgb);
                else
                    titleLabel->Overrides().Reset(Style::Color);
            }
            else
            {
                // No status - hide dot, show plain name
                if (dotElement)
                    dotElement->Overrides().Set(Style::Display, DisplayMode::None);
                titleLabel->SetText(displayName);
                titleLabel->Overrides().Reset(Style::Color);
            }
            
            // TreeView handles positioning of known extra elements (dot/icon) during bind.
        });
        
        m_Tree->RefreshFromProvider();
    }
    if (m_Grid)
    {
        m_Grid->SetDataProvider(m_GridProvider.get());
        m_Grid->SetSelectionModel(m_ItemSelection.get());
        m_Grid->SetOnScrollChanged([this](float scrollY, float contentHeight, float viewportHeight)
        {
            TryLoadMorePolyhavenBatch(scrollY, contentHeight, viewportHeight);
            if (m_OnAssetsListScrolled)
                m_OnAssetsListScrolled();
        });
        m_Grid->SetDragPayloadBuilder([this]() -> UI::Interaction::DragPayload {
            if (!m_ItemSelection || !m_GridProvider)
                return {};
            const auto ids = m_ItemSelection->GetSelection();
            if (ids.empty())
                return {};

            // When browsing Polyhaven online assets, support single and multi-select drag.
            // If ALL selected items are already downloaded, create an AssetPathsDragPayload
            // with their local model files. Otherwise, create an OnlineAssetDragPayload with
            // entries for each item (hierarchy handler resolves downloaded vs needs-download).
            if (!m_PolyhavenTypeKey.empty())
            {
                std::string phType;
                if (m_PolyhavenTypeKey.find("HDRI") != std::string::npos) phType = "hdris";
                else if (m_PolyhavenTypeKey.find("Texture") != std::string::npos) phType = "textures";
                else if (m_PolyhavenTypeKey.find("Model") != std::string::npos) phType = "models";

                // Collect local paths for downloaded items and entries for all items
                std::vector<std::filesystem::path> localPaths;
                std::vector<Editor::OnlineAssetEntry> entries;
                std::string firstName;
                std::filesystem::path firstItemPath; // thumbnail cache path for ghost

                for (auto id : ids)
                {
                    auto itemPath = m_GridProvider->GetPath(static_cast<GridId>(id));
                    std::string slug = itemPath.stem().string();
                    if (slug.empty() || slug == "loading")
                        continue;

                    if (firstItemPath.empty())
                        firstItemPath = itemPath;

                    std::string displayName = slug;
                    for (const auto& me : m_PolyhavenManifest)
                    {
                        if (me.slug == slug) { displayName = me.name; break; }
                    }
                    if (firstName.empty())
                        firstName = displayName;

                    entries.push_back({slug, displayName});

                    if (m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot))
                    {
                        std::filesystem::path downloadDir = m_AssetsRoot / "Polyhaven" / slug;
                        std::error_code ec;
                        for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
                        {
                            auto candidate = downloadDir / (slug + ext);
                            if (std::filesystem::exists(candidate, ec))
                            {
                                localPaths.push_back(std::move(candidate));
                                break;
                            }
                        }
                    }
                }

                if (entries.empty())
                    return {};

                // Ghost thumbnail: use the cached thumbnail PNG directly via file: prefix.
                std::string ghostThumb;
                if (!firstItemPath.empty())
                    ghostThumb = "file:" + firstItemPath.string();

                // If every item is fully downloaded, use AssetPathsDragPayload (direct scene import)
                if (localPaths.size() == entries.size())
                {
                    Editor::AssetPathsDragPayload p;
                    p.paths = std::move(localPaths);
                    std::string label = (entries.size() == 1)
                        ? firstName
                        : firstName + " + " + std::to_string(entries.size() - 1);
                    p.displayLabel = label;
                    UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
                    out.DisplayLabel = label;
                    out.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                    if (!ghostThumb.empty())
                        out.GhostThumbnailEngineName = ghostThumb;
                    return out;
                }

                // Mix of downloaded and not-downloaded: use OnlineAssetDragPayload with entries
                Editor::OnlineAssetDragPayload p;
                p.slug = entries[0].slug;
                p.name = entries[0].name;
                p.type = phType;
                p.entries = std::move(entries);
                size_t notDownloaded = p.entries.size() - localPaths.size();
                std::string label = firstName + " (" + std::to_string(notDownloaded) + " to download)";
                p.displayLabel = label;
                UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
                out.DisplayLabel = label;
                out.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                if (!ghostThumb.empty())
                    out.GhostThumbnailEngineName = ghostThumb;
                return out;
            }

            Editor::AssetPathsDragPayload p;
            p.paths.reserve(ids.size());
            for (auto id : ids)
                p.paths.push_back(m_GridProvider->GetPath(static_cast<GridId>(id)));
            std::string label;
            if (!p.paths.empty())
            {
                label = (p.paths.size() == 1) ? p.paths[0].filename().string()
                                              : (p.paths[0].filename().string() + " + " + std::to_string(p.paths.size() - 1));
                p.displayLabel = label;
            }
            UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
            out.DisplayLabel = label;
            if (out.IsValid())
            {
                std::error_code ec;
                const auto* pp = out.TryGet<Editor::AssetPathsDragPayload>();
                if (pp && !pp->paths.empty())
                {
                    const bool isDir = std::filesystem::is_directory(pp->paths[0], ec);
                    out.GhostIconKind = isDir ? UI::Interaction::DragGhostIconKind::AssetFolder : UI::Interaction::DragGhostIconKind::AssetFile;
                    if (!isDir)
                    {
                        const std::string ext = pp->paths[0].extension().string();
                        if (ext == ".scene")
                            out.GhostThumbnailEngineName = "Icons/sceneicon.png";
                        else if (m_Context && m_Context->Thumbnails)
                        {
                            const std::string thumb = m_Context->Thumbnails->GetOrRequest(pp->paths[0], 128, [](const std::string&) {});
                            if (!thumb.empty())
                                out.GhostThumbnailEngineName = thumb;
                        }
                    }
                }
            }
            return out;
        });

        // Accept asset drops onto folders in the grid (and onto grid empty space for current dir).
        m_Grid->SetAcceptsPayload([](UI::Interaction::PayloadTypeId tid) {
            return tid == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
        });
        m_Grid->SetOnCanDrop([this](const UI::Interaction::DropRequest& req) -> UI::Interaction::DropFeedback {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload)
                return {false, "Wrong payload"};
            if (!m_GridProvider)
                return {false, "No provider"};

            std::filesystem::path dest = m_GridProvider->GetCurrentDirectory();
            if (req.hit.Location == UI::Interaction::DropLocation::OnItem && req.hit.TargetId != 0)
            {
                const GridId gid = static_cast<GridId>(req.hit.TargetId);
                if (!m_GridProvider->IsDirectory(gid))
                    return {false, "Drop onto a folder"};
                dest = m_GridProvider->GetPath(gid);
            }
            if (dest.empty())
                return {false, "No destination"};

            const std::string destKey = AssetPathKey(dest);
            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            for (const auto& src : payload->paths)
            {
                if (src.empty())
                    continue;
                if (AssetPathKey(src) == destKey)
                    return {false, "Cannot move into itself"};
                if (std::filesystem::is_directory(src))
                {
                    if (copy)
                        return {false, "Copy folder not supported"};
                    if (IsUnderAssetDir(src, dest))
                        return {false, "Cannot move folder into its descendant"};
                }
                if (IsDirectChildOfAssetDir(dest, src))
                    return {false, "Already in destination"};
            }
            return {true, {}};
        });
        m_Grid->SetOnPerformDrop([this](const UI::Interaction::DropRequest& req) {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload || !m_GridProvider)
                return;

            std::filesystem::path dest = m_GridProvider->GetCurrentDirectory();
            if (req.hit.Location == UI::Interaction::DropLocation::OnItem && req.hit.TargetId != 0)
            {
                const GridId gid = static_cast<GridId>(req.hit.TargetId);
                if (!m_GridProvider->IsDirectory(gid))
                    return;
                dest = m_GridProvider->GetPath(gid);
            }
            if (dest.empty())
                return;

            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            const auto written = CopyOrMoveAssetPaths(*payload, dest, copy, m_Context ? m_Context->Assets : nullptr);
            RevealImportedAssets(dest, written);
            NotifyVcsStatusDirty();
        });

        // Virtualized binding for pooled cells: update folder style + thumbnail + label.
        // (ApplyThumbToElement resolves engine:-prefixed textures vs filesystem-path thumbnails.)
        m_Grid->SetItemBinder([this](UIElement* cell, GridId id, IGridDataProvider* prov)
                              {
            if (!cell || !m_GridProvider)
                return;

            const bool isDir = m_GridProvider->IsDirectory(id);
            if (isDir) cell->AddClass("folder"); else cell->RemoveClass("folder");
            
            // Check for scene/script files to apply CSS icon
            auto filePath = m_GridProvider->GetPath(id);
            StartPendingRenameIfBound(filePath);
            bool isScene = false;
            bool isScript = false;
            bool isVideo = false;
            if (!isDir && filePath.has_extension())
            {
                auto ext = filePath.extension().string();
                for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                isScene = (ext == ".scene");
                isScript = (ext == ".cs" || GetAssetTypeFromExtension(ext) == AssetType::NativeSource);
                isVideo = (ext == ".mp4" || ext == ".mov" || ext == ".avi" ||
                           ext == ".mkv" || ext == ".m4v" || ext == ".webm" || ext == ".wmv");
            }
            if (isScene) cell->AddClass("scene"); else cell->RemoveClass("scene");
            if (isScript) cell->AddClass("script"); else cell->RemoveClass("script");
            if (isVideo) cell->AddClass("video"); else cell->RemoveClass("video");

            // Set up drag handlers for bookmark creation
            // Only set up once per cell (check if already initialized)
            if (!cell->HasClass("asset-drag-initialized"))
            {
                cell->AddClass("asset-drag-initialized");
                
                auto path = m_GridProvider->GetPath(id);
                if (!path.empty() && !isDir)
                {
                    // Store path in cell for drag handlers
                    std::filesystem::path cellPath = path;
                    
                    // Use a simple approach: track mouse down position and detect drag on move
                    static float s_DragStartX = 0.0f;
                    static float s_DragStartY = 0.0f;
                    static bool s_MouseDown = false;
                    static std::filesystem::path s_DragPath;
                    
                    // Mouse down - start tracking potential drag
                    cell->RegisterEventHandler(kEventMouseDown, [cellPath](UIEvent& e)
                    {
                        if (e.Button == 0)
                        {
                            s_DragStartX = e.X;
                            s_DragStartY = e.Y;
                            s_MouseDown = true;
                            s_DragPath = cellPath;
                            // Clear any previous drag state
                            g_DragState.active = false;
                            g_DragState.assetPath.clear();
                        }
                    });

                    // Mouse move - detect drag and set global state
                    cell->RegisterEventHandler(kEventMouseMove, [cellPath](UIEvent& e)
                    {
                        if (s_MouseDown && s_DragPath == cellPath)
                        {
                            float dx = std::abs(e.X - s_DragStartX);
                            float dy = std::abs(e.Y - s_DragStartY);
                            if (dx > kAssetDragStartThresholdPx || dy > kAssetDragStartThresholdPx)
                            {
                                // Do not call WebPanel::TryStartNativeAssetDrag here: on success it stops
                                // propagation and starts an OS drag session, which prevents GridView's
                                // DragDropManager::BeginDrag from running so inspector/material drops never see
                                // an in-engine drag. GridView/ListView own AssetPathsDragPayload drags.

                                // Set global drag state for BookmarksPanel
                                g_DragState.active = true;
                                g_DragState.assetPath = cellPath;
                                g_DragState.scenePath.clear();
                                g_DragState.entityId.clear();
                            }
                        }
                    });

                    // Mouse up - clear drag tracking
                    cell->RegisterEventHandler(kEventMouseUp, [](UIEvent& e)
                    {
                        if (e.Button == 0)
                        {
                            s_MouseDown = false;
                            s_DragPath.clear();
                            // Don't clear g_DragState here - let BookmarksPanel handle it after drop
                        }
                    });
                }
            }

            // Default GridView structure: kids[0] = thumb, kids[1] = title (Label)
            UIElement* thumbEl = nullptr;
            Label* titleEl = nullptr;
            UIElement* gitDotEl = nullptr;
            const auto& kids = cell->GetChildren();
            if (!kids.empty())
                thumbEl = kids[0].get();
            if (kids.size() >= 2)
                titleEl = dynamic_cast<Label*>(kids[1].get());

            // Find or create VCS status dot element (added dynamically)
            for (const auto& child : kids)
            {
                if (child->HasClass("grid-git-dot"))
                {
                    gitDotEl = child.get();
                    break;
                }
            }

            auto p = m_GridProvider->GetPath(id);
            
            // Use VCS abstraction
            RefreshCachedVcsUiSettings();
            auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            const bool showStatusIcons = m_CachedVcsUiSettings.showStatusIcons;
            const bool colorDotOnly = m_CachedVcsUiSettings.colorDotOnly;
            const bool colorWholeText = m_CachedVcsUiSettings.colorWholeText;
            
            if (showStatusIcons && vcs && vcs->IsRepository() && !p.empty() && !isDir)
            {
                VCSFileStatus status = VCSFileStatus::Clean;
                (void)TryGetCachedVcsStatus(p, status);
                const char* statusColor = Editor::VcsStatusToColor(status);
                const uint32_t statusArgb = StatusColorToArgb(statusColor);
                
                if (statusColor && status != VCSFileStatus::Clean)
                {
                    
                    // Create dot if it doesn't exist
                    if (!gitDotEl)
                    {
                        auto newDot = std::make_unique<UIElement>();
                        newDot->AddClass("grid-git-dot");
                        gitDotEl = newDot.get();
                        cell->AddChild(std::move(newDot));
                    }
                    
                    // Show and color the dot
                    if (colorDotOnly || colorWholeText)
                    {
                        gitDotEl->Overrides()
                            .Set(Style::BackgroundTint, statusArgb)
                            .Set(Style::Display, DisplayMode::Flex);
                    }
                    else
                    {
                        gitDotEl->Overrides().Reset(Style::BackgroundTint);
                        gitDotEl->Overrides().Set(Style::Display, DisplayMode::Flex);
                    }

                    // Color title if colorWholeText is enabled
                    // Never color text when status is Clean or NotConfigured
                    if (titleEl && colorWholeText && status != VCSFileStatus::Clean && status != VCSFileStatus::NotConfigured)
                        titleEl->Overrides().Set(Style::Color, (uint32_t)statusArgb);
                    else if (titleEl)
                        titleEl->Overrides().Reset(Style::Color);
                }
                else
                {
                    // No status - hide dot
                    if (gitDotEl)
                        gitDotEl->Overrides().Set(Style::Display, DisplayMode::None);
                    if (titleEl)
                        titleEl->Overrides().Reset(Style::Color);
                }
            }
            else
            {
                // Not a file or no VCS or status icons disabled - hide dot
                if (gitDotEl)
                    gitDotEl->Overrides().Set(Style::Display, DisplayMode::None);
                if (titleEl)
                    titleEl->Overrides().Reset(Style::Color);
            }

            // Check if this Polyhaven item has been fully downloaded (model + textures)
            bool polyhavenDownloaded = false;
            if (!m_PolyhavenTypeKey.empty() && !isDir)
            {
                std::string slug = p.stem().string();
                if (!slug.empty() && slug != "loading")
                    polyhavenDownloaded = m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot);
                // Color the title blue for downloaded items (override any VCS color)
                if (polyhavenDownloaded && titleEl)
                {
                    constexpr uint32_t kDownloadedBlue = 0xFF4499FF;
                    titleEl->Overrides().Set(Style::Color, kDownloadedBlue);
                }
            }

            if (titleEl && prov)
            {
                const char* name = prov->GetLabel(id);
                std::string displayName = name ? std::string(name) : std::string("");

                RefreshCachedTruncationSettings();
                const bool truncationEnabled = m_TruncationEnabled;
                const float truncationThreshold = m_TruncationThreshold;
                
                float iconSize = m_Grid ? m_Grid->GetIconSize() : 80.0f;
                
                // Calculate max-width for text based on icon size (never larger than icon)
                // Subtract padding (8px each side) so total width stays within icon width
                float textMaxWidth = std::max(40.0f, iconSize - 16.0f);
                
                // Only truncate if enabled and icon size is below threshold
                if (truncationEnabled && iconSize < truncationThreshold)
                {
                    // Calculate max chars dynamically based on cell width
                    // Cell width = icon size + 16px padding, minus 4px for text padding
                    // Average char width at 14px font is ~7px
                    float cellWidth = iconSize + 16.0f - 4.0f;
                    size_t maxChars = std::max(10, (int)(cellWidth / 7.0f));
                    titleEl->SetText(MiddleTruncate(displayName, maxChars));
                    titleEl->Overrides().Set(Style::MaxWidth, StyleLength::Px(textMaxWidth));
                }
                else
                {
                    // No truncation - show full name with word wrap
                    titleEl->SetText(displayName);
                    titleEl->Overrides().Set(Style::MaxWidth, StyleLength::Px(textMaxWidth));
                }
            }

            if (!thumbEl)
                return;

            // Stable thumb id so async thumbnail updates always target the correct asset id.
            const std::string thumbId = std::string("thumb-") + cell->GetId();
            if (thumbEl->GetId() != thumbId)
                thumbEl->SetId(thumbId);

            UIElement* thumb = thumbEl;

            // Folder icon: rely on CSS (.grid-cell.folder .thumb) and disable any previous override.
            if (isDir)
            {
                if (thumb)
                    UI::Layout::DisableBackgroundOverride(*thumb);
                return;
            }
            
            // Script icon: rely on CSS (.grid-cell.script .thumb) like folders.
            if (isScript)
            {
                if (thumb)
                    UI::Layout::DisableBackgroundOverride(*thumb);
                return;
            }

            // File: clear any stale thumbnail first, then request/cache.
            if (thumb)
                UI::Layout::ClearBackgroundOverride(*thumb);

            // p was already retrieved above for VCS status
            if (p.empty())
                return;

            // All local asset types share the deferred browser path, including
            // saved scene images, material previews, and model thumbnails.
            if (m_Context && m_Context->Thumbnails)
            {
                const std::string cellId = cell->GetId();
                auto immediate = m_Context->Thumbnails->GetOrRequestBrowser(
                    p, 128, thumbId,
                    [this, alive = m_Alive, thumbId]()
                    {
                        return alive->load(std::memory_order_acquire) &&
                               IsBrowserThumbnailVisible(m_Grid, thumbId);
                    },
                    [this, alive = m_Alive, thumbId, cellId, post = PostHandleFor(m_Grid)](const std::string& rel)
                    {
                        if (rel.empty()) return;
                        post.Post([this, alive, thumbId, cellId, rel]()
                        {
                            if (!alive->load(std::memory_order_acquire)) return;
                            if (auto* el = m_Grid->FindById(thumbId))
                                ApplyThumbToElement(el, rel);
                            if (auto* c = m_Grid->FindById(cellId))
                                c->AddClass("has-thumbnail");
                        });
                    });

                if (!immediate.empty())
                {
                    ApplyThumbToElement(thumb, immediate);
                    cell->AddClass("has-thumbnail");
                }
                else
                {
                    cell->RemoveClass("has-thumbnail");
                    if (isScene)
                    {
                        // No thumbnail yet — fall back to the CSS scene icon.
                        UI::Layout::DisableBackgroundOverride(*thumb);
                    }
                }
            }
            else
            {
                cell->RemoveClass("has-thumbnail");
                if (isScene)
                {
                    UI::Layout::DisableBackgroundOverride(*thumb);
                }
            }

            // VCS status overlay icon (cached; non-blocking for UI thread).
            // Recycled cells carry the previous item's class, so the stale
            // ones must come off even when this item has no status.
            {
                const char* statusClass = nullptr;
                auto* activeVcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
                if (!isDir && m_CachedVcsUiSettings.showStatusIcons && activeVcs && activeVcs->IsRepository() && !p.empty())
                {
                    VCSFileStatus st = VCSFileStatus::Clean;
                    (void)TryGetCachedVcsStatus(p, st);
                    switch (st)
                    {
                    case VCSFileStatus::Modified:    statusClass = "git-status-modified"; break;
                    case VCSFileStatus::Added:       statusClass = "git-status-added"; break;
                    case VCSFileStatus::Deleted:     statusClass = "git-status-deleted"; break;
                    case VCSFileStatus::Conflict:    statusClass = "git-status-conflict"; break;
                    case VCSFileStatus::Unversioned: statusClass = "git-status-unversioned"; break;
                    default: break;
                    }
                }
                static constexpr const char* kAllStatusClasses[] = {
                    "git-status-modified", "git-status-added", "git-status-deleted",
                    "git-status-conflict", "git-status-unversioned"};
                for (const char* cls : kAllStatusClasses)
                {
                    if (statusClass && std::strcmp(cls, statusClass) == 0)
                        cell->AddClass(cls);
                    else
                        cell->RemoveClass(cls);
                }
            }

            // Phantom rows render ghosted via the .missing-asset class. Drag,
            // VCS, and thumbnail load are skipped — they don't apply to refs
            // whose target file isn't on disk.
            if (m_GridProvider->IsPhantom(id))
                cell->AddClass("missing-asset");
            else
                cell->RemoveClass("missing-asset"); });

        // Double-click to activate (enter folder or open asset)
        m_Grid->SetOnItemRenameRequested([this](GridId id)
                                         {
            if (m_GridProvider)
                BeginRename(m_GridProvider->GetPath(id));
        });
        m_Grid->SetOnItemActivated([this](GridId id)
                                   {
		            if (!m_GridProvider || (!m_Grid && !m_Tree)) return;
		            auto p = m_GridProvider->GetPath(id);
		            std::string ps = NormalizeVirtualAssetPathString(p);
		            Logger::Log::Info("Grid double-click: id={} path='{}' isDir={}", id, ps, m_GridProvider->IsDirectory(id));
		            if (ps == "polyhaven://loading")
		                return;
		            // Navigate virtual online-asset folders by selecting the tree node
		            if (ps == "online://polyhaven" && m_Tree && m_TreeSelection) {
		                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kPolyhavenId);
		                m_Tree->SetExpanded(AssetsTreeDataProvider::kPolyhavenId, true);
		                m_Tree->SyncSelectionVisuals();
		                return;
		            }
		            if (ps == "online://" && m_Tree && m_TreeSelection) {
		                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kOnlineAssetsSectionId);
		                m_Tree->SetExpanded(AssetsTreeDataProvider::kOnlineAssetsSectionId, true);
		                m_Tree->SyncSelectionVisuals();
		                return;
		            }
		            // polyhaven:// alone → go to Polyhaven root
		            if (ps == "polyhaven://" && m_Tree && m_TreeSelection) {
		                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kPolyhavenId);
		                m_Tree->SetExpanded(AssetsTreeDataProvider::kPolyhavenId, true);
		                m_Tree->SyncSelectionVisuals();
		                return;
		            }
		            // polyhaven://hdris, polyhaven://textures, polyhaven://models (no slash after type)
		            if (ps.size() > 12 && ps.compare(0, 12, "polyhaven://") == 0 && ps.find('/', 12) == std::string::npos) {
		                std::string typeSlug = ps.substr(12);
		                if (m_Tree && m_TreeProvider && m_TreeSelection) {
		                    TreeId phId = AssetsTreeDataProvider::kPolyhavenId;
		                    m_Tree->SetExpanded(phId, true);
		                    int n = m_TreeProvider->GetChildCount(phId);
		                    for (int i = 0; i < n; ++i) {
		                        TreeId cid = m_TreeProvider->GetChildId(phId, i);
		                        if (m_TreeProvider->GetPolyhavenCategory(cid) == typeSlug) {
		                            m_TreeSelection->SetSingle(cid);
		                            m_Tree->SetExpanded(cid, true);
		                            m_Tree->SyncSelectionVisuals();
		                            break;
		                        }
		                    }
		                }
		                return;
		            }
		            // polyhaven://type/category
		            std::string phType, phCategory;
		            if (PolyhavenService::ParseVirtualPath(p, phType, phCategory)) {
		                SyncTreeToPolyhavenCategory(phType, phCategory);
		                SelectPolyhavenCategoryFilter(phType, phCategory);
		                return;
		            }
		            bool isDir = m_GridProvider->IsDirectory(id);
		            if (isDir && !p.empty()) {
		                NavigateToDirectory(p);
		            } else if (!m_PolyhavenTypeKey.empty() && !p.empty()) {
		                // Double-click on Polyhaven item: download if not already downloaded
		                std::string slug = p.stem().string();
		                if (!slug.empty() && slug != "loading")
		                {
		                    if (m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot))
		                    {
		                        // Already downloaded — navigate to the imported asset
		                        std::filesystem::path downloadDir = m_AssetsRoot / "Polyhaven" / slug;
		                        std::filesystem::path mainFile;
		                        std::error_code ec;
		                        for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
		                        {
		                            auto candidate = downloadDir / (slug + ext);
		                            if (std::filesystem::exists(candidate, ec))
		                            {
		                                mainFile = candidate;
		                                break;
		                            }
		                        }
		                        if (!mainFile.empty())
		                            NavigateToAndSelectAsset(mainFile);
		                        else
		                            NavigateToDirectory(downloadDir);
		                    }
		                    else
		                    {
		                        // Not downloaded — trigger download
		                        // phType is set here before capture in the download thread below
		                        if (m_PolyhavenTypeKey.find("HDRI") != std::string::npos) phType = "hdris";
		                        else if (m_PolyhavenTypeKey.find("Texture") != std::string::npos) phType = "textures";
		                        else if (m_PolyhavenTypeKey.find("Model") != std::string::npos) phType = "models";

		                        std::filesystem::path assetsRoot = m_AssetsRoot;
		                        auto alive = m_Alive;
		                        AssetsBrowserController* controller = this;
		                        const UI::UiPostHandle post = PostHandleFor(m_Grid ? static_cast<UIElement*>(m_Grid)
		                                                                           : static_cast<UIElement*>(m_List));

		                        SubmitPolyhavenTransfer(m_Context, [slug, phType, assetsRoot, post, alive, controller]()
		                        {
		                            std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
		                            PolyhavenService::DownloadAsset(slug, phType, destDir);
		                            if (alive->load(std::memory_order_acquire))
		                            {
		                                post.Post([alive, controller]() {
		                                    if (!alive->load(std::memory_order_acquire))
		                                        return;
		                                    controller->m_Polyhaven->ClearDownloadCache();
		                                    if (controller->m_Grid)
		                                        controller->m_Grid->RefreshFromProvider();
		                                    if (controller->m_List)
		                                        controller->m_List->RefreshFromProvider();
		                                });
		                            }
		                        });
		                    }
		                }
		            } else {
		                if (m_OnOpenAsset) {
		                    m_OnOpenAsset(p);
		                } else {
	                    Platform::OpenPath(p);
		                }
		            } });

        // Right-click context menu for assets in the grid (files and folders).
        m_Grid->SetOnContextMenu([this](GridId id, float x, float y)
                                 {
	            if (!m_GridProvider || !m_Context) return;

                // Polyhaven remote items get a dedicated context menu (before m_ContextMenu guard).
                if (id != 0 && !m_PolyhavenTypeKey.empty())
                {
                    auto p = m_GridProvider->GetPath(id);
                    if (!p.empty())
                        ShowPolyhavenContextMenu(p, x, y);
                    return;
                }

                if (!m_ContextMenu) return;
                if (id == 0)
                {
                    const auto& dir = m_GridProvider->GetCurrentDirectory();
                    if (dir.empty())
                    {
                        // Empty space in smart folder view — show smart folder menu
                        if (m_SmartFolderController && m_SmartFolderController->HasSelection())
                        {
                            std::string sfId = m_SmartFolderController->GetSelectedId();
                            m_ContextMenu->ShowSmartFolderMenu(x, y,
                                [this, sfId](EditorContextMenu::SmartFolderAction action)
                                {
                                    switch (action)
                                    {
                                        case EditorContextMenu::SmartFolderAction::NewSmartFolder:
                                            CreateNewSmartFolder();
                                            break;
                                        case EditorContextMenu::SmartFolderAction::Delete:
                                            if (m_SmartFolderController)
                                                m_SmartFolderController->GetManager().Delete(sfId);
                                            break;
                                    }
                                });
                        }
                        return;
                    }
                    m_ContextMenu->ShowDirectoryMenu(dir, x, y,
                        [this](const std::filesystem::path& directory,
                               EditorContextMenu::DirectoryAction action)
                        { HandleDirectoryAction(directory, action); });
                    return;
                }
	            // Phantom rows aren't real files on disk; show recovery actions
	            // (Remove from scene, Copy GUID) instead of the standard asset
	            // menu. Pick-replacement is deferred to a follow-up.
	            if (m_GridProvider->IsPhantom(id))
	            {
	                const GUID phantomGuid = m_GridProvider->GetPhantomGuid(id);
	                const auto displayPath = m_GridProvider->GetPath(id);
	                m_ContextMenu->ShowPhantomAssetMenu(displayPath, x, y,
	                    [this, phantomGuid]() { OnPhantomRemoveFromScene(phantomGuid); },
	                    [phantomGuid]() { Platform::SetClipboardText(phantomGuid.ToString().c_str()); });
	                return;
	            }

	            auto p = m_GridProvider->GetPath(id);
	            if (p.empty()) return;

	            bool isDir = m_GridProvider->IsDirectory(id);
	            std::vector<std::filesystem::path> pathsForBookmarks;
	            {
	                const auto ids = m_ItemSelection->GetSelection();
	                bool idInSelection = false;
	                for (auto raw : ids)
	                    if (static_cast<GridId>(raw) == id) { idInSelection = true; break; }
	                if (idInSelection && !ids.empty())
	                {
	                    pathsForBookmarks.reserve(ids.size());
	                    for (auto raw : ids)
	                        pathsForBookmarks.push_back(m_GridProvider->GetPath(static_cast<GridId>(raw)));
	                }
	                else
	                    pathsForBookmarks = { p };
	            }
	            m_ContextMenu->ShowAssetMenu(p, isDir, x, y,
	                [this](const std::filesystem::path& path, bool isDirectory)
	                {
	                    if (isDirectory)
	                        NavigateToDirectory(path);
	                    else if (m_OnOpenAsset)
	                        m_OnOpenAsset(path);
	                    else
	                        Platform::OpenPath(path);
	                },
	                [this](const std::filesystem::path& path)
	                {
	                    if (m_OnEditAsset)
	                        m_OnEditAsset(path);
	                },
	                &pathsForBookmarks,
	                [this](const std::vector<std::filesystem::path>& paths)
	                {
	                    if (m_OnAddToBookmarks)
	                        m_OnAddToBookmarks(paths);
	                },
	                [this](const std::vector<std::filesystem::path>& paths)
	                {
	                    DeleteAssetPaths(paths);
	                },
	                isDir ? std::function<void(const std::filesystem::path&)>{}
	                      : std::function<void(const std::filesystem::path&)>(
	                            [this](const std::filesystem::path& path) { BeginRename(path); }));
	        });

        SortDescriptor sort;
        sort.field = SortDescriptor::Field::Name;
        sort.ascending = true;
        m_Grid->SetSort(sort);
        GroupDescriptor group;
        group.key = GroupDescriptor::Key::None;
        m_Grid->SetGrouping(group);
        m_Grid->RefreshFromProvider();
    }

    if (m_List && m_ListProvider)
    {
        // Drag assets from the list (same payload as grid).
        m_List->SetDragPayloadBuilder([this]() -> UI::Interaction::DragPayload {
            if (!m_ItemSelection || !m_ListProvider)
                return {};
            const auto ids = m_ItemSelection->GetSelection();
            if (ids.empty())
                return {};

            // When browsing Polyhaven online assets in list view, same multi-select
            // logic as grid: all-downloaded → AssetPathsDragPayload, otherwise → OnlineAssetDragPayload.
            if (!m_PolyhavenTypeKey.empty())
            {
                std::string phType;
                if (m_PolyhavenTypeKey.find("HDRI") != std::string::npos) phType = "hdris";
                else if (m_PolyhavenTypeKey.find("Texture") != std::string::npos) phType = "textures";
                else if (m_PolyhavenTypeKey.find("Model") != std::string::npos) phType = "models";

                std::vector<std::filesystem::path> localPaths;
                std::vector<Editor::OnlineAssetEntry> entries;
                std::string firstName;
                std::filesystem::path firstItemPath;

                for (auto id : ids)
                {
                    auto itemPath = m_ListProvider->GetPath(static_cast<ListId>(id));
                    std::string slug = itemPath.stem().string();
                    if (slug.empty() || slug == "loading")
                        continue;

                    if (firstItemPath.empty())
                        firstItemPath = itemPath;

                    std::string displayName = slug;
                    for (const auto& me : m_PolyhavenManifest)
                    {
                        if (me.slug == slug) { displayName = me.name; break; }
                    }
                    if (firstName.empty())
                        firstName = displayName;

                    entries.push_back({slug, displayName});

                    if (m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot))
                    {
                        std::filesystem::path downloadDir = m_AssetsRoot / "Polyhaven" / slug;
                        std::error_code ec;
                        for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
                        {
                            auto candidate = downloadDir / (slug + ext);
                            if (std::filesystem::exists(candidate, ec))
                            {
                                localPaths.push_back(std::move(candidate));
                                break;
                            }
                        }
                    }
                }

                if (!entries.empty())
                {
                    std::string label = (entries.size() == 1)
                        ? firstName
                        : firstName + " + " + std::to_string(entries.size() - 1);

                    std::string ghostThumb;
                    if (!firstItemPath.empty())
                        ghostThumb = "file:" + firstItemPath.string();

                    if (localPaths.size() == entries.size())
                    {
                        Editor::AssetPathsDragPayload ap;
                        ap.paths = std::move(localPaths);
                        ap.displayLabel = label;
                        UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(ap));
                        out.DisplayLabel = label;
                        out.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                        if (!ghostThumb.empty())
                            out.GhostThumbnailEngineName = ghostThumb;
                        return out;
                    }

                    Editor::OnlineAssetDragPayload op;
                    op.slug = entries[0].slug;
                    op.name = entries[0].name;
                    op.type = phType;
                    op.entries = std::move(entries);
                    size_t notDownloaded = op.entries.size() - localPaths.size();
                    std::string onlineLabel = firstName + " (" + std::to_string(notDownloaded) + " to download)";
                    op.displayLabel = onlineLabel;
                    UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(op));
                    out.DisplayLabel = onlineLabel;
                    out.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                    if (!ghostThumb.empty())
                        out.GhostThumbnailEngineName = ghostThumb;
                    return out;
                }
            }

            Editor::AssetPathsDragPayload p;
            p.paths.reserve(ids.size());
            for (auto id : ids)
                p.paths.push_back(m_ListProvider->GetPath(static_cast<ListId>(id)));
            std::string label;
            if (!p.paths.empty())
            {
                label = (p.paths.size() == 1) ? p.paths[0].filename().string()
                                              : (p.paths[0].filename().string() + " + " + std::to_string(p.paths.size() - 1));
                p.displayLabel = label;
            }
            UI::Interaction::DragPayload out = UI::Interaction::DragPayload::Create(std::move(p));
            out.DisplayLabel = label;
            if (out.IsValid())
            {
                std::error_code ec;
                const auto* pp = out.TryGet<Editor::AssetPathsDragPayload>();
                if (pp && !pp->paths.empty())
                {
                    const bool isDir = std::filesystem::is_directory(pp->paths[0], ec);
                    out.GhostIconKind = isDir ? UI::Interaction::DragGhostIconKind::AssetFolder : UI::Interaction::DragGhostIconKind::AssetFile;
                    if (!isDir)
                    {
                        const std::string ext = pp->paths[0].extension().string();
                        if (ext == ".scene")
                            out.GhostThumbnailEngineName = "Icons/sceneicon.png";
                        else if (m_Context && m_Context->Thumbnails)
                        {
                            const std::string thumb = m_Context->Thumbnails->GetOrRequest(pp->paths[0], 128, [](const std::string&) {});
                            if (!thumb.empty())
                                out.GhostThumbnailEngineName = thumb;
                        }
                    }
                }
            }
            return out;
        });

        // Accept asset drops onto folders in the list (and onto list empty space for current dir).
        m_List->SetAcceptsPayload([](UI::Interaction::PayloadTypeId tid) {
            return tid == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
        });
        m_List->SetOnCanDrop([this](const UI::Interaction::DropRequest& req) -> UI::Interaction::DropFeedback {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload)
                return {false, "Wrong payload"};
            if (!m_ListProvider || !m_GridProvider)
                return {false, "No provider"};

            std::filesystem::path dest = m_GridProvider->GetCurrentDirectory();
            if (req.hit.Location == UI::Interaction::DropLocation::OnItem && req.hit.TargetId != 0)
            {
                const ListId lid = static_cast<ListId>(req.hit.TargetId);
                if (!m_ListProvider->IsDirectory(lid))
                    return {false, "Drop onto a folder"};
                dest = m_ListProvider->GetPath(lid);
            }
            if (dest.empty())
                return {false, "No destination"};

            const std::string destKey = AssetPathKey(dest);
            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            for (const auto& src : payload->paths)
            {
                if (src.empty())
                    continue;
                if (AssetPathKey(src) == destKey)
                    return {false, "Cannot move into itself"};
                if (std::filesystem::is_directory(src))
                {
                    if (copy)
                        return {false, "Copy folder not supported"};
                    if (IsUnderAssetDir(src, dest))
                        return {false, "Cannot move folder into its descendant"};
                }
                if (IsDirectChildOfAssetDir(dest, src))
                    return {false, "Already in destination"};
            }
            return {true, {}};
        });
        m_List->SetOnPerformDrop([this](const UI::Interaction::DropRequest& req) {
            const auto* payload = req.payload.TryGet<Editor::AssetPathsDragPayload>();
            if (!payload || !m_ListProvider || !m_GridProvider)
                return;

            std::filesystem::path dest = m_GridProvider->GetCurrentDirectory();
            if (req.hit.Location == UI::Interaction::DropLocation::OnItem && req.hit.TargetId != 0)
            {
                const ListId lid = static_cast<ListId>(req.hit.TargetId);
                if (!m_ListProvider->IsDirectory(lid))
                    return;
                dest = m_ListProvider->GetPath(lid);
            }
            if (dest.empty())
                return;

            const bool copy = payload->copyOnly || IsCopyDrop(req.mods);
            const auto written = CopyOrMoveAssetPaths(*payload, dest, copy, m_Context ? m_Context->Assets : nullptr);
            RevealImportedAssets(dest, written);
            NotifyVcsStatusDirty();
        });
    }

    // Initialize ListView if provided
    if (m_List && m_ListProvider)
    {
        m_List->SetDataProvider(m_ListProvider.get());
        m_List->SetSelectionModel(m_ItemSelection.get());
        m_List->SetOnScrollChanged([this](float /*scrollX*/, float scrollY)
        {
            if (m_List)
            {
                TryLoadMorePolyhavenBatch(scrollY, m_List->GetTotalContentHeight(), m_List->GetViewportHeight());
            }
            if (m_OnAssetsListScrolled)
                m_OnAssetsListScrolled();
        });

        // Double-click to activate
        m_List->SetOnItemRenameRequested([this](ListId id)
                                         {
            if (m_ListProvider)
                BeginRename(m_ListProvider->GetPath(id));
        });
        m_List->SetOnItemActivated([this](ListId id)
        {
            if (!m_ListProvider) return;
            auto p = m_ListProvider->GetPath(id);
            std::string ps = NormalizeVirtualAssetPathString(p);
            if (ps == "polyhaven://loading")
                return;
            if (ps == "online://polyhaven" && m_Tree && m_TreeSelection) {
                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kPolyhavenId);
                m_Tree->SetExpanded(AssetsTreeDataProvider::kPolyhavenId, true);
                m_Tree->SyncSelectionVisuals();
                return;
            }
            if (ps == "online://" && m_Tree && m_TreeSelection) {
                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kOnlineAssetsSectionId);
                m_Tree->SetExpanded(AssetsTreeDataProvider::kOnlineAssetsSectionId, true);
                m_Tree->SyncSelectionVisuals();
                return;
            }
            if (ps == "polyhaven://" && m_Tree && m_TreeSelection) {
                m_TreeSelection->SetSingle(AssetsTreeDataProvider::kPolyhavenId);
                m_Tree->SetExpanded(AssetsTreeDataProvider::kPolyhavenId, true);
                m_Tree->SyncSelectionVisuals();
                return;
            }
            if (ps.size() > 12 && ps.compare(0, 12, "polyhaven://") == 0 && ps.find('/', 12) == std::string::npos) {
                std::string typeSlug = ps.substr(12);
                if (m_Tree && m_TreeProvider && m_TreeSelection) {
                    TreeId phId = AssetsTreeDataProvider::kPolyhavenId;
                    m_Tree->SetExpanded(phId, true);
                    int n = m_TreeProvider->GetChildCount(phId);
                    for (int i = 0; i < n; ++i) {
                        TreeId cid = m_TreeProvider->GetChildId(phId, i);
                        if (m_TreeProvider->GetPolyhavenCategory(cid) == typeSlug) {
                            m_TreeSelection->SetSingle(cid);
                            m_Tree->SetExpanded(cid, true);
                            m_Tree->SyncSelectionVisuals();
                            break;
                        }
                    }
                }
                return;
            }
            std::string phType, phCategory;
            if (PolyhavenService::ParseVirtualPath(p, phType, phCategory)) {
                SyncTreeToPolyhavenCategory(phType, phCategory);
                SelectPolyhavenCategoryFilter(phType, phCategory);
                return;
            }
            bool isDir = m_ListProvider->IsDirectory(id);
            if (isDir && !p.empty()) {
                NavigateToDirectory(p);
            } else {
                if (m_OnOpenAsset) {
                    m_OnOpenAsset(p);
                } else {
                    Platform::OpenPath(p);
                }
            }
        });

        // Context menu for list view
        m_List->SetOnContextMenu([this](ListId id, float x, float y)
        {
            if (!m_ListProvider || !m_Context) return;

            // Polyhaven remote items get a dedicated context menu.
            if (id != 0 && !m_PolyhavenTypeKey.empty())
            {
                auto p = m_ListProvider->GetPath(id);
                if (!p.empty())
                    ShowPolyhavenContextMenu(p, x, y);
                return;
            }

            if (!m_ContextMenu) return;
            if (id == 0)
            {
                if (!m_GridProvider) return;
                const auto& dir = m_GridProvider->GetCurrentDirectory();
                if (dir.empty()) return;
                m_ContextMenu->ShowDirectoryMenu(dir, x, y,
                    [this](const std::filesystem::path& directory,
                           EditorContextMenu::DirectoryAction action)
                    { HandleDirectoryAction(directory, action); });
                return;
            }
            if (m_GridProvider && m_GridProvider->IsPhantom(static_cast<GridId>(id)))
            {
                const GUID phantomGuid = m_GridProvider->GetPhantomGuid(static_cast<GridId>(id));
                const auto displayPath = m_GridProvider->GetPath(static_cast<GridId>(id));
                m_ContextMenu->ShowPhantomAssetMenu(displayPath, x, y,
                    [this, phantomGuid]() { OnPhantomRemoveFromScene(phantomGuid); },
                    [phantomGuid]() { Platform::SetClipboardText(phantomGuid.ToString().c_str()); });
                return;
            }

            bool isDir = m_ListProvider->IsDirectory(id);
            auto p = m_ListProvider->GetPath(id);
            if (p.empty()) return;
            std::vector<std::filesystem::path> pathsForBookmarks;
            {
                const auto ids = m_ItemSelection->GetSelection();
                bool idInSelection = false;
                for (auto raw : ids)
                    if (static_cast<ListId>(raw) == id) { idInSelection = true; break; }
                if (idInSelection && !ids.empty())
                {
                    pathsForBookmarks.reserve(ids.size());
                    for (auto raw : ids)
                        pathsForBookmarks.push_back(m_ListProvider->GetPath(static_cast<ListId>(raw)));
                }
                else
                    pathsForBookmarks = { p };
            }
            m_ContextMenu->ShowAssetMenu(p, isDir, x, y,
                [this](const std::filesystem::path& path, bool isDirectory)
                {
                    if (isDirectory)
                        NavigateToDirectory(path);
                    else if (m_OnOpenAsset)
                        m_OnOpenAsset(path);
                    else
                        Platform::OpenPath(path);
                },
                [this](const std::filesystem::path& path)
                {
                    if (m_OnEditAsset)
                        m_OnEditAsset(path);
                },
                &pathsForBookmarks,
                [this](const std::vector<std::filesystem::path>& paths)
                {
                    if (m_OnAddToBookmarks)
                        m_OnAddToBookmarks(paths);
                },
                [this](const std::vector<std::filesystem::path>& paths)
                {
                    DeleteAssetPaths(paths);
                },
                isDir ? std::function<void(const std::filesystem::path&)>{}
                      : std::function<void(const std::filesystem::path&)>(
                            [this](const std::filesystem::path& path) { BeginRename(path); }));
        });

        // The item resize gesture adjusts row height.
        m_List->SetOnItemResizeGesture([this](float scrollY) { ResizeListRowsFromScroll(scrollY); });
    }
}

void AssetsBrowserController::SetAssetsRoot(const std::filesystem::path& dir)
{
    m_AssetsRoot = dir;
    if (m_SmartFolderController)
        m_SmartFolderController->SetAssetsRoot(dir);
    InvalidateVcsStatusCache();

    // Switching projects/roots should drop any pending navigation/refresh work.
    // Otherwise we can apply file events or queued directory loads from the
    // previous root after the providers have been repointed. The listing below
    // supersedes every scan still in flight.
    //
    // Clearing the queue alone does not drop every old-root event: the previous
    // subscription stays live until it is released below, so an old-root callback
    // can still run after the clear. The generation bump, under the queue's mutex,
    // makes every previous-root callback that pushes after this point drop its event.
    m_PendingNavigation = {};
    uint64_t rootGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_FsInbox->Mutex);
        rootGeneration = ++m_FsInbox->RootGeneration;
        m_FsInbox->Events.clear();
    }
    m_FsRefreshPosted.store(false);

    if (m_TreeProvider)
    {
        m_TreeProvider->SetRootsFromDirectory(dir);
        if (m_Tree)
        {
            ApplyInitialTreeExpansion();
        }
    }

    if (m_GridProvider)
    {
        m_GridProvider->SetRootLimit(dir);
        m_GridProvider->SetAssetsRoot(dir);
        m_GridProvider->LoadDirectory(dir);
        if (m_Grid)
            m_Grid->RefreshFromProvider();
        if (m_List && m_ListProvider)
            m_List->RefreshFromProvider();

        // Defer search-index warming until the project-folder loading UI can dismiss.
        if (UIElement* searchAnchor = m_Grid ? static_cast<UIElement*>(m_Grid)
                                               : (m_Tree ? static_cast<UIElement*>(m_Tree)
                                                         : (m_List ? static_cast<UIElement*>(m_List) : nullptr)))
        {
            searchAnchor->PostAction([this]() {
                WarmSearchIndexInBackground();
                DeferThumbnailsAfterNavigation();
            });
        }
        else
        {
            WarmSearchIndexInBackground();
            DeferThumbnailsAfterNavigation();
        }
    }

    // Apply online assets settings from editor preferences.
    if (m_TreeProvider)
    {
        auto prefs = Editor::OpenEditorPreferences();
        prefs.Load();
        bool onlineEnabled = true;
        prefs.TryGetBool("onlineAssets.enabled", onlineEnabled);
        m_TreeProvider->SetOnlineAssetsEnabled(onlineEnabled);

        bool polyHavenEnabled = true;
        prefs.TryGetBool("onlineAssets.polyHaven", polyHavenEnabled);
        m_TreeProvider->SetPolyHavenEnabled(polyHavenEnabled);

        std::string posStr;
        bool atTop = false;
        if (prefs.TryGetString("onlineAssets.position", posStr))
            atTop = (posStr == "top");
        m_TreeProvider->SetOnlineAssetsAtTop(atTop);
    }

    // Load Poly Haven categories into the tree as directories (All, Aerial, Brick, ... under HDRIs/Textures/Models)
    if (m_TreeProvider && m_Tree && m_TreeProvider->GetOnlineAssetsEnabled())
        SchedulePolyhavenCategoriesLoad(m_Context, m_TreeProvider.get(), m_Tree, m_Alive);

    // Subscribe to file watching for auto-refresh. The logic is the same for
    // the Assets root and any subfolder: when a change happens directly in the
    // directory that the grid is currently showing, we reload that directory.
    // "Directly in" here means the parent directory of the changed path keys
    // the same as the current directory under AssetPathKey, which keeps
    // behaviour consistent for root and nested folders without any special
    // cases. The key must be the registry's: the current directory arrives
    // case-folded from the registry while watch events carry on-disk case.
    m_AssetsWatchSub.reset();
    FilePattern pattern(dir, ".*", {}, true);
    FileWatchingService* watcher =
        m_Context && m_Context->FileWatcher ? m_Context->FileWatcher : &FileWatchingService::GetInstance();
    const UI::UiPostHandle post =
        PostHandleFor(m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_Grid));
    auto sub = watcher->Subscribe(pattern, [this, post, inbox = m_FsInbox, rootGeneration](const FileChangeEvent& ev)
                                  {
        // Ignore dotfile churn (e.g. .thumbcache_*, .DS_Store) for UI refresh
        // purposes. The grid provider already filters dotfiles from display,
        // so changes to them should never trigger a directory reload.
        auto isDotfile = [](const std::filesystem::path& p) {
            const auto name = p.filename().string();
            return !name.empty() && name[0] == '.';
        };
        if (isDotfile(ev.Path) || (!ev.OldPath.empty() && isDotfile(ev.OldPath)))
            return;

        // Held to the end of the callback: the destructor takes it to clear
        // ControllerAlive, so the controller stays alive while this reads and posts
        // through it.
        std::lock_guard<std::mutex> lock(inbox->Mutex);
        if (!inbox->ControllerAlive || rootGeneration != inbox->RootGeneration)
            return;
        inbox->Events.push_back(ev);

        // Batch/coalesce into one UI refresh per frame. A refused post never runs to
        // release the latch, so release it here and let the next event post again.
        if (!m_FsRefreshPosted.exchange(true))
        {
            if (!post.Post([this, inbox]()
                           {
                               // The anchor can outlive the controller.
                               if (inbox->ControllerAlive)
                                   ProcessPendingFsEvents();
                           }))
                m_FsRefreshPosted.store(false);
        } });
    m_AssetsWatchSub = std::move(sub);
}

void AssetsBrowserController::ApplySort(const SortDescriptor& desc)
{
    if (m_Grid)
    {
        m_Grid->SetSort(desc);
    }
    else if (m_GridProvider)
    {
        m_GridProvider->ApplySort(desc);
    }

    if (m_List && m_ListProvider)
    {
        m_List->RefreshFromProvider();
    }
}

void AssetsBrowserController::ApplyInitialTreeExpansion()
{
    if (!m_Tree)
        return;

    if (m_ExpandFoldersOnLoad)
    {
        m_Tree->ExpandAll();
    }
    else
    {
        std::vector<TreeId> expanded;
        m_Tree->ForEachExpanded([&expanded](TreeId id) { expanded.push_back(id); });
        for (TreeId id : expanded)
            m_Tree->SetExpanded(id, false);
        m_Tree->RefreshFromProvider();
    }

    // Keep remote catalogs collapsed at startup so they don't dominate the tree.
    m_Tree->SetExpanded(AssetsTreeDataProvider::kOnlineAssetsSectionId, false);
    m_Tree->SetExpanded(AssetsTreeDataProvider::kPolyhavenId, false);
    for (TreeId cid = AssetsTreeDataProvider::kPolyhavenCategoryIdBase;
         cid < AssetsTreeDataProvider::kPolyhavenCategoryIdBase + 3; ++cid)
        m_Tree->SetExpanded(cid, false);
}

void AssetsBrowserController::SetFoldersFirst(bool foldersFirst)
{
    if (!m_GridProvider)
        return;

    m_GridProvider->SetFoldersFirst(foldersFirst);

    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List)
        m_List->RefreshFromProvider();
    DeferThumbnailsAfterNavigation();
}

void AssetsBrowserController::SetExpandFoldersOnLoad(bool expand)
{
    if (m_ExpandFoldersOnLoad == expand)
        return;

    m_ExpandFoldersOnLoad = expand;
    ApplyInitialTreeExpansion();
}

// Apply a thumbnail background to an icon element. An "engine:" prefix names a staged
// engine resource; otherwise the string is a filesystem path. Empty clears the override.
static void ApplyThumbToElement(UIElement* thumb, const std::string& relOrEngine)
{
    if (!thumb)
        return;
    if (relOrEngine.empty())
    {
        UI::Layout::ClearBackgroundOverride(*thumb);
        return;
    }
    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t      kEnginePrefixLen = 7;
    if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
    {
        UI::Layout::SetBackgroundResourceName(*thumb, relOrEngine.substr(kEnginePrefixLen));
        return;
    }
    UI::Layout::SetBackgroundPath(*thumb, relOrEngine);
}

// Which CSS row-icon class an asset's extension maps to. Lower-cases once; directories pass
// isDir separately. Shared by the row classes (BindListRow) and the Name-cell icon choice.
namespace {
struct AssetExtensionKind { bool IsScene = false; bool IsScript = false; bool IsVideo = false; };
AssetExtensionKind ClassifyAssetExtension(const std::filesystem::path& path)
{
    AssetExtensionKind kind;
    if (!path.has_extension())
        return kind;
    std::string ext = path.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    kind.IsScene  = (ext == ".scene");
    kind.IsScript = (ext == ".cs" || GetAssetTypeFromExtension(ext) == AssetType::NativeSource);
    kind.IsVideo  = (ext == ".mp4" || ext == ".mov" || ext == ".avi" ||
                     ext == ".mkv" || ext == ".m4v" || ext == ".webm" || ext == ".wmv");
    return kind;
}
} // namespace

void AssetsBrowserController::BindListRow(UIElement* row, ListId id, int /*rowIndex*/)
{
    if (!row || !m_ListProvider || !m_GridProvider)
        return;
    StartPendingRenameIfBound(m_ListProvider->GetPath(id));

    // Inherit the legacy list-item / assets-list-row styling (colours, hover, selected, icons).
    // The TableView's own .table-view-row class drives layout; the override CSS reconciles the two.
    row->AddClass("list-item");
    row->AddClass("assets-list-row");

    const bool isDir = m_ListProvider->IsDirectory(id);
    if (isDir) row->AddClass("folder"); else row->RemoveClass("folder");

    const auto filePath = m_ListProvider->GetPath(id);
    const AssetExtensionKind ext = isDir ? AssetExtensionKind{} : ClassifyAssetExtension(filePath);
    if (ext.IsScene)  row->AddClass("scene");  else row->RemoveClass("scene");
    if (ext.IsScript) row->AddClass("script"); else row->RemoveClass("script");
    if (ext.IsVideo)  row->AddClass("video");  else row->RemoveClass("video");

    if (m_GridProvider->IsPhantom(static_cast<GridId>(id)))
        row->AddClass("missing-asset");
    else
        row->RemoveClass("missing-asset");

    // Store the row's path so the per-row bookmark micro-drag (below) can recover it.
    m_ListRowPathMap[row] = filePath.string();

    // One-time per-row drag-to-bookmarks gesture. This is distinct from the ListView's asset
    // DnD (SetDragPayloadBuilder): it drives the global g_DragState that BookmarksPanel reads
    // on drop. The handlers are registered once per pooled row and recover the current path
    // from m_ListRowPathMap (re-keyed every bind), so reuse across items stays correct.
    if (!row->HasClass("asset-list-drag-initialized"))
    {
        row->AddClass("asset-list-drag-initialized");
        static float                 s_DragStartX = 0.0f;
        static float                 s_DragStartY = 0.0f;
        static bool                  s_MouseDown  = false;
        static std::filesystem::path s_DragPath;

        AssetsBrowserController* controller = this;
        UIElement*               rowEl      = row;
        row->RegisterEventHandler(kEventMouseDown, [controller, rowEl](UIEvent& e)
        {
            if (e.Button != 0)
                return;
            auto it = controller->m_ListRowPathMap.find(rowEl);
            if (it == controller->m_ListRowPathMap.end() || it->second.empty())
                return;
            s_DragStartX = e.X;
            s_DragStartY = e.Y;
            s_MouseDown  = true;
            s_DragPath   = std::filesystem::path(it->second);
            g_DragState.active = false;
            g_DragState.assetPath.clear();
        });
        row->RegisterEventHandler(kEventMouseMove, [](UIEvent& e)
        {
            if (!s_MouseDown || s_DragPath.empty())
                return;
            const float dx = std::abs(e.X - s_DragStartX);
            const float dy = std::abs(e.Y - s_DragStartY);
            if (dx > kAssetDragStartThresholdPx || dy > kAssetDragStartThresholdPx)
            {
                g_DragState.active = true;
                g_DragState.assetPath = s_DragPath;
                g_DragState.scenePath.clear();
                g_DragState.entityId.clear();
            }
        });
        row->RegisterEventHandler(kEventMouseUp, [](UIEvent& e)
        {
            if (e.Button == 0) { s_MouseDown = false; s_DragPath.clear(); }
        });
    }
}

void AssetsBrowserController::BindListCell(UIElement* cell, StringId colKey, ListId id, int /*rowIndex*/)
{
    if (!cell || !m_ListProvider || !m_GridProvider)
        return;

    RefreshCachedListFontSize();
    const float fontSize = m_ListFontSize;
    // Create the cell's single text child once, reuse it on every rebind (the column a cell
    // belongs to is fixed for its lifetime), and keep the recycled label's font size in sync.
    auto ensureLabel = [cell, fontSize](const char* cls1, const char* cls2) -> Label*
    {
        Label* lbl = nullptr;
        if (cell->GetChildren().empty())
        {
            auto l = std::make_unique<Label>();
            l->AddClass(cls1);
            if (cls2) l->AddClass(cls2);
            lbl = l.get();
            cell->AddChild(std::move(l));
        }
        else
        {
            lbl = dynamic_cast<Label*>(cell->GetChildren()[0].get());
        }
        if (lbl)
            lbl->Overrides().Set(Style::FontSize, StyleLength::Px(fontSize));
        return lbl;
    };

    const bool isDir = m_ListProvider->IsDirectory(id);
    const auto p     = m_ListProvider->GetPath(id);

    // ---- Name (icon + label composite) ----
    if (colKey == AssetsListColumns::kName)
    {
        cell->AddClass("assets-name-cell");
        UIElement* icon      = nullptr;
        Label*     nameLabel = nullptr;
        if (cell->GetChildren().size() < 2)
        {
            auto ic = std::make_unique<UIElement>();
            ic->AddClass("list-item-icon");
            icon = ic.get();
            cell->AddChild(std::move(ic));
            auto lbl = std::make_unique<Label>();
            lbl->AddClass("list-label");
            lbl->AddClass(kListNameLabelClass);
            nameLabel = lbl.get();
            cell->AddChild(std::move(lbl));
        }
        else
        {
            icon      = cell->GetChildren()[0].get();
            nameLabel = dynamic_cast<Label*>(cell->GetChildren()[1].get());
        }

        const AssetExtensionKind extKind = isDir ? AssetExtensionKind{} : ClassifyAssetExtension(p);
        const bool isScene  = extKind.IsScene;
        const bool isScript = extKind.IsScript;

        bool downloaded = false;
        if (!m_PolyhavenTypeKey.empty() && !isDir && m_Polyhaven)
        {
            std::string slug = p.stem().string();
            if (!slug.empty() && slug != "loading")
                downloaded = m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot);
        }

        if (nameLabel)
        {
            nameLabel->Overrides().Set(Style::FontSize, StyleLength::Px(fontSize));
            const char* nm = m_ListProvider->GetLabel(id);
            std::string displayName = nm ? std::string(nm) : std::string();
            if (!isDir)
            {
                const size_t dot = displayName.rfind('.');
                if (dot != std::string::npos && dot > 0)
                    displayName = displayName.substr(0, dot);
            }
            RefreshCachedTruncationSettings();
            nameLabel->SetText(m_TruncationEnabled ? MiddleTruncate(displayName, 40) : displayName);

            // Name colour: dim by default; colorWholeText paints it with the VCS status colour
            // (this column owns that, since the Git cell can no longer reach the name label);
            // a downloaded Polyhaven asset wins with blue.
            nameLabel->Overrides().Reset(Style::Color);
            RefreshCachedVcsUiSettings();
            // Gate on showStatusIcons too: the old binder only tinted the name when status icons
            // were enabled (the colour came from inside the icon block), and the grid view still
            // does. Without this, names tint with icons off but colorWholeText on.
            if (m_CachedVcsUiSettings.showStatusIcons && m_CachedVcsUiSettings.colorWholeText && !p.empty())
            {
                auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
                if (vcs && vcs->IsRepository())
                {
                    VCSFileStatus status = VCSFileStatus::Clean;
                    (void)TryGetCachedVcsStatus(p, status);
                    if (status != VCSFileStatus::Clean && status != VCSFileStatus::NotConfigured)
                    {
                        const char* statusColor = Editor::VcsStatusToColor(status);
                        if (statusColor)
                            nameLabel->Overrides().Set(Style::Color, (uint32_t)StatusColorToArgb(statusColor));
                    }
                }
            }
            if (downloaded)
                nameLabel->Overrides().Set(Style::Color, 0xFF4499FFu);
        }

        if (icon)
        {
            const std::string iconId = std::string("list-thumb-") +
                std::to_string(reinterpret_cast<std::uintptr_t>(m_List)) + "-" + std::to_string(id);
            if (icon->GetId() != iconId)
                icon->SetId(iconId);

            const float rowHeight  = m_ListProvider->GetItemHeight();
            const int   gapPerSide = std::max(1, std::min(2, (int)(rowHeight / 24.0f)));
            const int   totalGap   = gapPerSide * 2;
            const int   iconSize   = std::max(16, (int)(rowHeight - totalGap));
            icon->Overrides()
                .Set(Style::Width, StyleLength::Px((float)iconSize))
                .Set(Style::Height, StyleLength::Px((float)iconSize))
                .Set(Style::MinWidth, StyleLength::Px((float)iconSize))
                .Set(Style::MinHeight, StyleLength::Px((float)iconSize))
                .Set(Style::MarginTop, StyleLength::Px((float)gapPerSide))
                .Set(Style::MarginRight, StyleLength::Px(4.0f))
                .Set(Style::MarginBottom, StyleLength::Px((float)gapPerSide))
                .Set(Style::MarginLeft, StyleLength::Px(0.0f));

            if (isDir || isScript)
            {
                // Folder + script icons come from CSS (the row carries the folder/script class).
                icon->RemoveClass("has-thumbnail");
                UI::Layout::DisableBackgroundOverride(*icon);
            }
            else
            {
                icon->RemoveClass("has-thumbnail");
                UI::Layout::ClearBackgroundOverride(*icon);
                if (!p.empty() && m_Context && m_Context->Thumbnails)
                {
                    const std::string iconIdCopy = iconId;
                    auto immediate = m_Context->Thumbnails->GetOrRequestBrowser(
                        p, 32, iconIdCopy,
                        [this, alive = m_Alive, iconIdCopy]()
                        {
                            return alive->load(std::memory_order_acquire) &&
                                   IsBrowserThumbnailVisible(m_List, iconIdCopy);
                        },
                        [this, alive = m_Alive, iconIdCopy, post = PostHandleFor(m_List)](const std::string& rel)
                        {
                            if (rel.empty())
                                return;
                            post.Post([this, alive, iconIdCopy, rel]()
                            {
                                if (!alive->load(std::memory_order_acquire)) return;
                                if (auto* el = m_List->FindById(iconIdCopy))
                                {
                                    ApplyThumbToElement(el, rel);
                                    el->AddClass("has-thumbnail");
                                }
                            });
                        });
                    if (!immediate.empty())
                    {
                        ApplyThumbToElement(icon, immediate);
                        icon->AddClass("has-thumbnail");
                    }
                    else if (isScene)
                    {
                        UI::Layout::DisableBackgroundOverride(*icon);
                    }
                }
                else if (isScene)
                {
                    UI::Layout::DisableBackgroundOverride(*icon);
                }
            }
        }
        return;
    }

    // ---- Type ----
    if (colKey == AssetsListColumns::kType)
    {
        Label* lbl = ensureLabel("list-type", "col-type");
        if (lbl)
        {
            if (!m_PolyhavenTypeKey.empty() && !isDir)
            {
                if (m_PolyhavenTypeKey.find("Model") != std::string::npos)        lbl->SetText("PH Model");
                else if (m_PolyhavenTypeKey.find("Texture") != std::string::npos) lbl->SetText("PH Texture");
                else if (m_PolyhavenTypeKey.find("HDRI") != std::string::npos)    lbl->SetText("PH HDRI");
                else                                                              lbl->SetText("PH Asset");
            }
            else
            {
                const char* type = isDir ? "Folder" : m_ListProvider->GetTypeKey(id);
                if (!type || type[0] == '\0')
                    type = "File";
                lbl->SetText(type);
            }
        }
        return;
    }

    // ---- Size ----
    if (colKey == AssetsListColumns::kSize)
    {
        Label* lbl = ensureLabel("list-size", "col-size");
        if (lbl)
        {
            uintmax_t sizeBytes = 0;
            if (!isDir && m_GridProvider->TryGetSizeBytes(static_cast<GridId>(id), sizeBytes))
                lbl->SetText(FormatByteSize(sizeBytes));
            else
                lbl->SetText("-");
        }
        return;
    }

    // ---- Dimensions ----
    if (colKey == AssetsListColumns::kDimensions)
    {
        Label* lbl = ensureLabel("list-dimensions", "col-dimensions");
        if (lbl)
        {
            int imgW = 0, imgH = 0;
            if (!isDir && m_GridProvider->TryGetImageDimensions(static_cast<GridId>(id), imgW, imgH))
                lbl->SetText(std::to_string(imgW) + " x " + std::to_string(imgH));
            else
                lbl->SetText("-");
        }
        return;
    }

    // ---- Modified ----
    if (colKey == AssetsListColumns::kModified)
    {
        Label* lbl = ensureLabel("list-modified", "col-modified");
        if (lbl)
        {
            std::filesystem::file_time_type wt{};
            if (!isDir && m_GridProvider->TryGetLastWriteTime(static_cast<GridId>(id), wt))
                lbl->SetText(FormatTime(wt));
            else
                lbl->SetText("-");
        }
        return;
    }

    // ---- Git / VCS status (dot + text) ----
    if (colKey == AssetsListColumns::kGit)
    {
        UIElement* container = nullptr;
        UIElement* dot       = nullptr;
        Label*     text      = nullptr;
        if (cell->GetChildren().empty())
        {
            auto c = std::make_unique<UIElement>();
            c->AddClass("list-git");
            c->AddClass("col-git");
            c->AddClass("git-container");
            auto d = std::make_unique<UIElement>();
            d->AddClass("git-dot");
            dot = d.get();
            c->AddChild(std::move(d));
            auto t = std::make_unique<Label>();
            t->AddClass("git-text");
            text = t.get();
            c->AddChild(std::move(t));
            container = c.get();
            cell->AddChild(std::move(c));
        }
        else
        {
            container = cell->GetChildren()[0].get();
            if (container)
            {
                const auto& gk = container->GetChildren();
                if (gk.size() >= 1) dot  = gk[0].get();
                if (gk.size() >= 2) text = dynamic_cast<Label*>(gk[1].get());
            }
        }
        if (text)
            text->Overrides().Set(Style::FontSize, StyleLength::Px(fontSize));

        if (container && dot && text)
        {
            auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            RefreshCachedVcsUiSettings();
            const bool showStatusIcons = m_CachedVcsUiSettings.showStatusIcons;
            const bool colorDotOnly    = m_CachedVcsUiSettings.colorDotOnly;
            const bool hideDotForClean = m_CachedVcsUiSettings.hideDotForClean;

            if (showStatusIcons && vcs && vcs->IsRepository() && !p.empty())
            {
                VCSFileStatus  status     = VCSFileStatus::Clean;
                (void)TryGetCachedVcsStatus(p, status);
                const char*    statusStr   = Editor::VcsStatusToDisplayString(status);
                const char*    statusColor = Editor::VcsStatusToColor(status);
                const uint32_t statusArgb  = StatusColorToArgb(statusColor);
                const bool     shouldHideDot = (hideDotForClean && status == VCSFileStatus::Clean);

                if (statusColor && statusStr && statusStr[0] != '\0')
                {
                    if (shouldHideDot)
                        dot->Overrides().Set(Style::Display, DisplayMode::None);
                    else
                        dot->Overrides().Set(Style::BackgroundTint, statusArgb).Set(Style::Display, DisplayMode::Flex);
                    text->SetText(statusStr);
                    if (status == VCSFileStatus::Clean || status == VCSFileStatus::NotConfigured || colorDotOnly)
                        text->Overrides().Reset(Style::Color);
                    else
                        text->Overrides().Set(Style::Color, (uint32_t)statusArgb);
                }
                else if (statusStr && statusStr[0] != '\0')
                {
                    dot->Overrides().Set(Style::Display, DisplayMode::None);
                    text->SetText(statusStr);
                    text->Overrides().Reset(Style::Color);
                }
                else
                {
                    dot->Overrides().Set(Style::Display, DisplayMode::None);
                    text->SetText("");
                    text->Overrides().Reset(Style::Color);
                }

                container->RemoveClass("git-modified");
                container->RemoveClass("git-added");
                container->RemoveClass("git-deleted");
                container->RemoveClass("git-conflict");
                container->RemoveClass("git-untracked");
                switch (status)
                {
                case VCSFileStatus::Modified:    container->AddClass("git-modified"); break;
                case VCSFileStatus::Added:       container->AddClass("git-added"); break;
                case VCSFileStatus::Deleted:     container->AddClass("git-deleted"); break;
                case VCSFileStatus::Conflict:    container->AddClass("git-conflict"); break;
                case VCSFileStatus::Unversioned: container->AddClass("git-untracked"); break;
                default: break;
                }
            }
            else
            {
                dot->Overrides().Set(Style::Display, DisplayMode::None);
                if (!vcs || !vcs->IsRepository())
                    text->SetText("not active");
                else
                    text->SetText("-");
                text->Overrides().Reset(Style::Color);
            }
        }
        return;
    }

    // ---- Tag (coloured dots + text) ----
    if (colKey == AssetsListColumns::kTag)
    {
        UIElement* container = nullptr;
        UIElement* dots      = nullptr;
        Label*     text      = nullptr;
        if (cell->GetChildren().empty())
        {
            auto c = std::make_unique<UIElement>("div");
            c->AddClass("list-tag");
            c->AddClass("col-tag");
            c->AddClass("tag-container");
            auto dd = std::make_unique<UIElement>("div");
            dd->AddClass("tag-dots");
            dots = dd.get();
            c->AddChild(std::move(dd));
            auto t = std::make_unique<Label>();
            t->AddClass("tag-text");
            text = t.get();
            c->AddChild(std::move(t));
            container = c.get();
            cell->AddChild(std::move(c));
        }
        else
        {
            container = cell->GetChildren()[0].get();
            if (container)
            {
                const auto& tk = container->GetChildren();
                if (tk.size() >= 1) dots = tk[0].get();
                if (tk.size() >= 2) text = dynamic_cast<Label*>(tk[1].get());
            }
        }
        if (text)
            text->Overrides().Set(Style::FontSize, StyleLength::Px(fontSize));

        if (dots && text)
        {
            auto clearDots = [dots]()
            {
                dots->RemoveAllChildren();
            };
            std::string tagMeta;
            if (isDir)
            {
                clearDots();
                text->SetText("-");
            }
            else if (auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                     !reg.TryGetMetaValue(p, "tags", tagMeta) || tagMeta.empty())
            {
                clearDots();
                text->SetText("-");
            }
            else
            {
                std::vector<std::string> tagParts;
                for (size_t i = 0; i < tagMeta.size();)
                {
                    size_t j = tagMeta.find(',', i);
                    if (j == std::string::npos)
                        j = tagMeta.size();
                    std::string part = tagMeta.substr(i, j - i);
                    const size_t start = part.find_first_not_of(" \t");
                    if (start != std::string::npos)
                    {
                        const size_t end = part.find_last_not_of(" \t");
                        part = part.substr(start, end == std::string::npos ? part.size() - start : end - start + 1);
                    }
                    else
                        part.clear();
                    if (!part.empty() && std::find(tagParts.begin(), tagParts.end(), part) == tagParts.end())
                        tagParts.push_back(part);
                    i = j + (j < tagMeta.size() ? 1 : 0);
                }
                if (tagParts.empty())
                {
                    clearDots();
                    text->SetText("-");
                }
                else
                {
                    std::string displayTags;
                    for (size_t i = 0; i < tagParts.size(); ++i)
                    {
                        if (i) displayTags += ", ";
                        displayTags += tagParts[i];
                    }
                    text->SetText(displayTags);
                    const std::vector<EditorTagDefinition> definitions = EditorTags::Load();
                    clearDots();
                    for (const std::string& tagName : tagParts)
                    {
                        std::string color;
                        for (const auto& def : definitions)
                        {
                            if (def.Name == tagName) { color = def.Color; break; }
                        }
                        auto d = std::make_unique<UIElement>("div");
                        d->AddClass("tag-dot");
                        d->Overrides()
                            .Set(Style::Width, StyleLength::Px(8.0f))
                            .Set(Style::Height, StyleLength::Px(8.0f))
                            .Set(Style::MinWidth, StyleLength::Px(8.0f))
                            .Set(Style::MinHeight, StyleLength::Px(8.0f))
                            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
                            .Set(Style::FlexShrink, 0.0f)
                            .Set(Style::MarginRight, StyleLength::Px(2.0f));
                        if (color.empty())
                        {
                            d->Overrides()
                                .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
                                .Set(Style::BorderColor, BorderColorsTRBL{0xFF666666u, 0xFF666666u, 0xFF666666u, 0xFF666666u});
                        }
                        else
                        {
                            d->Overrides().Set(Style::BackgroundColor, (uint32_t)StatusColorToArgb(color.c_str(), 0xFF888888u));
                        }
                        dots->AddChild(std::move(d));
                    }
                }
            }
        }
        return;
    }

    // ---- Referenced / Custom / Creator (no data yet) ----
    if (colKey == AssetsListColumns::kReferenced)
    {
        if (Label* l = ensureLabel("list-referenced", "col-referenced")) l->SetText("-");
        return;
    }
    if (colKey == AssetsListColumns::kCustom)
    {
        if (Label* l = ensureLabel("list-custom", "col-custom")) l->SetText("-");
        return;
    }
    if (colKey == AssetsListColumns::kCreator)
    {
        if (Label* l = ensureLabel("list-creator", "col-creator")) l->SetText("-");
        return;
    }
}

float AssetsBrowserController::GetListRowHeight() const
{
    return m_ListProvider ? m_ListProvider->GetItemHeight() : 24.0f;
}

void AssetsBrowserController::SetListRowHeight(float heightPx)
{
    if (!m_List || !m_ListProvider)
        return;

    constexpr float kMinRow = 16.0f;
    constexpr float kMaxRow = 64.0f;
    const float newHeight = std::clamp(heightPx, kMinRow, kMaxRow);
    if (std::abs(newHeight - m_ListProvider->GetItemHeight()) <= 0.1f)
        return;

    m_ListProvider->SetItemHeight(newHeight);
    if (m_OnRowHeightChanged)
        m_OnRowHeightChanged(newHeight);
    m_List->RefreshFromProvider();
}

void AssetsBrowserController::ResizeListRowsFromScroll(float scrollY)
{
    if (!m_ListProvider)
        return;

    // scrollY is the UI's pixel scroll (~30 per wheel detent); rows change linearly per detent.
    constexpr float kPixelsPerWheelDetent = 30.0f;
    constexpr float kRowHeightPxPerDetent = 2.0f;
    const float detents = -scrollY / kPixelsPerWheelDetent;
    SetListRowHeight(m_ListProvider->GetItemHeight() + detents * kRowHeightPxPerDetent);
}

const char* AssetsBrowserController::GetItemDisplayName(int index) const
{
    if (!m_GridProvider || index < 0 || index >= m_GridProvider->GetItemCount())
        return nullptr;
    return m_GridProvider->GetLabel(m_GridProvider->GetItemId(index));
}

void AssetsBrowserController::SetSearchQuery(const std::string& query,
                                             bool restoreSelectedResultOnClear)
{
    if (m_SearchQuery == query)
        return;

    // A project-wide search can show an asset outside the directory that was
    // open before searching. Keep hold of a single selected result so clearing
    // the query can return to its folder without dropping the selection.
    const bool isClearingSearch = !m_SearchQuery.empty() && query.empty();
    const std::filesystem::path selectedSearchResult =
        isClearingSearch && restoreSelectedResultOnClear
            ? GetPrimarySelectedAssetPath()
            : std::filesystem::path{};

    m_SearchQuery = query;
    if (m_GridProvider)
        m_GridProvider->SetSearchQuery(m_SearchQuery);
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List)
        m_List->RefreshFromProvider();
    DeferThumbnailsAfterNavigation();

    if (!selectedSearchResult.empty())
        NavigateToAndSelectAssetSilent(selectedSearchResult);
}

void AssetsBrowserController::SetSearchField(AssetsSearchField field)
{
    if (!m_GridProvider)
        return;
    m_GridProvider->SetSearchField(field);
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List)
        m_List->RefreshFromProvider();
    DeferThumbnailsAfterNavigation();
}

void AssetsBrowserController::ProcessPendingFsEvents()
{
    GE_CPU_PROFILE_SCOPE("AssetsBrowserController.ProcessPendingFsEvents");
    // Clear the latch BEFORE taking the batch. The watcher thread pushes an event and then
    // tests the latch; cleared after the swap, an event pushed between the swap and the
    // clear would find the latch still set, post nothing, and wait for the next file event.
    // Cleared first, the worst case is one extra post that finds the queue empty.
    m_FsRefreshPosted.store(false);

    std::vector<FileChangeEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_FsInbox->Mutex);
        events.swap(m_FsInbox->Events);
    }

    if (events.empty())
        return;

    if (m_SmartFolderController)
        m_SmartFolderController->InvalidateResults();

    // While a navigation is pending, its folder is the one to keep current:
    // the folder on screen is about to be replaced.
    const bool navigationPending = IsNavigationPending();
    std::filesystem::path currentDir = navigationPending ? m_PendingNavigation.Dir
                                     : m_GridProvider ? m_GridProvider->GetCurrentDirectory()
                                                      : std::filesystem::path{};
    std::filesystem::path desiredDir = currentDir;
    bool currentDirRenamed = false;
    bool needsGridRefresh = false;

    bool needsTreeRefresh = false;
    std::unordered_set<std::string> treeCandidateKeys;
    std::vector<std::filesystem::path> treeCandidates;
    std::vector<TreeId> toExpand;

    for (const auto& ev : events)
    {
        // Filter dotfiles again (defensive).
        {
            const auto name = ev.Path.filename().string();
            if (!name.empty() && name[0] == '.')
                continue;
        }
        if (!ev.OldPath.empty())
        {
            const auto name = ev.OldPath.filename().string();
            if (!name.empty() && name[0] == '.')
                continue;
        }

        // If the current directory itself (or an ancestor) was renamed, keep browsing within the moved subtree.
        if (ev.Type == FileChangeType::Renamed && !ev.OldPath.empty() && !desiredDir.empty())
        {
            if (auto rebased = RebaseRenamedAssetDir(desiredDir, ev.OldPath, ev.Path))
            {
                desiredDir = *rebased;
                currentDirRenamed = true;
            }
        }

        // Grid refresh: if anything directly in the current directory changed (including moved out via oldPath).
        if (!desiredDir.empty())
        {
            const bool directFromPath = !ev.Path.empty() && IsDirectChildOfAssetDir(desiredDir, ev.Path);
            const bool directFromOldPath = !ev.OldPath.empty() && IsDirectChildOfAssetDir(desiredDir, ev.OldPath);
            if (directFromPath || directFromOldPath)
            {
                needsGridRefresh = true;
            }
        }

        // Tree updates are only needed for directory-structure affecting events (ignore Modified).
        if (m_TreeProvider && m_Tree && ev.Type != FileChangeType::Modified)
        {
            // Preserve TreeId across directory renames when possible.
            if (ev.Type == FileChangeType::Renamed && !ev.OldPath.empty())
            {
                std::error_code ec;
                if (!ev.Path.empty() && std::filesystem::exists(ev.Path, ec) && std::filesystem::is_directory(ev.Path, ec))
                {
                    if (m_TreeProvider->OnPathRenamed(ev.OldPath, ev.Path))
                    {
                        needsTreeRefresh = true;
                    }
                }
            }

            auto addCandidate = [&](const std::filesystem::path& c)
            {
                if (c.empty())
                    return;
                const std::string key = AssetPathKey(c);
                if (treeCandidateKeys.insert(key).second)
                {
                    treeCandidates.push_back(c);
                }
            };

            if (!ev.Path.empty())
                addCandidate(ev.Path.parent_path());
            if (!ev.OldPath.empty())
                addCandidate(ev.OldPath.parent_path());
        }
    }

    if (currentDirRenamed)
    {
        needsGridRefresh = true;
    }

    // Apply grid refresh at most once. A pending navigation rescans its folder
    // and keeps its callbacks.
    if (needsGridRefresh && m_GridProvider)
    {
        const std::filesystem::path loadDir = desiredDir.empty() ? currentDir : desiredDir;
        if (!loadDir.empty())
        {
            const uint64_t generation = ScanDirectoryInBackground(loadDir);
            if (navigationPending)
            {
                m_PendingNavigation.Dir = loadDir;
                m_PendingNavigation.Generation = generation;
            }
        }
    }

    // A project-wide search snapshot includes descendants outside the current
    // folder. Refresh it for any filesystem-event batch that did not already
    // reload the current directory, so cached search results stay current.
    if (!needsGridRefresh && !m_SearchQuery.empty() && m_GridProvider)
    {
        m_GridProvider->RefreshSearchIndex();
        if (m_Grid)
            m_Grid->RefreshFromProvider();
        if (m_List && m_ListProvider)
            m_List->RefreshFromProvider();
        DeferThumbnailsAfterNavigation();
    }

    // Apply tree invalidations and auto-expand newly non-leaf folders.
    if (m_TreeProvider && m_Tree && !treeCandidates.empty())
    {
        for (const auto& c : treeCandidates)
        {
            TreeId id = m_TreeProvider->FindIdForPath(c);
            int oldChildCount = -1;
            if (id != 0)
            {
                oldChildCount = m_TreeProvider->GetChildCount(id);
            }
            if (m_TreeProvider->OnDirectoryChanged(c))
            {
                needsTreeRefresh = true;
                if (id != 0 && oldChildCount == 0)
                {
                    toExpand.push_back(id);
                }
            }
        }
    }

    if (needsTreeRefresh && m_Tree)
    {
        for (TreeId id : toExpand)
        {
            m_Tree->SetExpanded(id, true);
        }
        m_Tree->RefreshFromProvider();
    }

    // Smart-folder result sets span directories, so any coalesced filesystem
    // change can affect the selected set even when the current directory is empty.
    if (m_SmartFolderController && m_SmartFolderController->HasSelection())
        ShowSmartFolderInGrid(m_SmartFolderController->GetSelectedId());
}

void AssetsBrowserController::SyncTreeSelectionToDirectory(const std::filesystem::path& dir)
{
    if (!m_TreeProvider || !m_Tree || !m_TreeSelection || dir.empty() || m_AssetsRoot.empty())
        return;

    std::error_code ec;
    std::filesystem::path normDir = std::filesystem::absolute(dir, ec).lexically_normal();
    std::filesystem::path normRoot = std::filesystem::absolute(m_AssetsRoot, ec).lexically_normal();
    if (normDir.empty() || normRoot.empty())
        return;

    // Skip entirely if the target is outside the assets root — nothing in the tree can match.
    {
        std::error_code ecRel;
        std::filesystem::path rel = std::filesystem::relative(normDir, normRoot, ecRel);
        if (ecRel || rel.empty() || (!rel.native().empty() && rel.native()[0] == '.' && rel.native().size() >= 2 && rel.native()[1] == '.'))
            return;
    }

    // Walk from target up to the assets root, collecting ancestors to expand.
    std::vector<std::filesystem::path> chain;
    for (auto p = normDir; !p.empty() && p != p.root_path(); p = p.parent_path())
    {
        chain.push_back(p);
        if (p == normRoot)
            break;
        std::error_code ecEq;
        if (std::filesystem::exists(p, ecEq) && std::filesystem::exists(normRoot, ecEq) &&
            std::filesystem::equivalent(p, normRoot, ecEq))
            break;
    }
    std::reverse(chain.begin(), chain.end());

    bool didChange = false;
    for (auto& seg : chain)
    {
        TreeId tid = m_TreeProvider->FindIdForPath(seg);
        if (tid != 0)
        {
            m_TreeProvider->GetChildCount(tid); // ensure populated
            if (!m_Tree->IsExpanded(tid))
            {
                m_Tree->SetExpanded(tid, true);
                didChange = true;
            }
        }
    }

    // Select the target folder node. Suppress the tree's OnSelectionChanged side effects
    // so that programmatic syncs don't fire m_OnSelectAssets (which would push the folder
    // into inspector selection history for a folder the user never clicked).
    TreeId targetId = m_TreeProvider->FindIdForPath(normDir);
    if (targetId != 0)
    {
        const auto existing = m_TreeSelection->GetSelection();
        const bool alreadySelected = existing.size() == 1 && existing.front() == targetId;
        if (!alreadySelected)
        {
            m_SuppressTreeSelectSideEffects = true;
            m_TreeSelection->SetSingle(targetId);
            m_SuppressTreeSelectSideEffects = false;
            m_Tree->SyncSelectionVisuals();
            didChange = true;
        }
    }

    if (didChange)
        m_Tree->RefreshFromProvider();
}

void AssetsBrowserController::NavigateToDirectory(const std::filesystem::path& dir)
{
    NavigateToDirectory(dir, nullptr);
}

void AssetsBrowserController::NavigateToDirectory(const std::filesystem::path& dir, std::function<void()> onNavigated)
{
    if (!m_GridProvider || (!m_Grid && !m_Tree))
    {
        if (onNavigated)
            onNavigated();
        return;
    }
    if (dir.empty())
    {
        if (onNavigated)
            onNavigated();
        return;
    }

    // Skip grid reload if same directory as current, but still sync tree selection.
    std::error_code eq;
    const auto& curr = m_GridProvider->GetCurrentDirectory();
    bool same = !curr.empty() && std::filesystem::exists(dir, eq) && std::filesystem::exists(curr, eq) && std::filesystem::equivalent(curr, dir, eq);
    if (same)
    {
        // The folder on screen is the newest request: a navigation still in
        // flight must not replace it when its scan lands.
        if (IsNavigationPending())
        {
            m_GridProvider->NextListingGeneration();
            m_PendingNavigation = {};
        }
        SyncTreeSelectionToDirectory(dir);
        if (onNavigated)
            onNavigated();
        return;
    }

    // Coalesce repeated requests for the folder a worker already lists.
    if (IsNavigationPending())
    {
        std::error_code e2;
        const auto& pendingDir = m_PendingNavigation.Dir;
        bool eqv = !pendingDir.empty() && std::filesystem::exists(pendingDir, e2) && std::filesystem::exists(dir, e2) && std::filesystem::equivalent(pendingDir, dir, e2);
        if (eqv)
        {
            if (onNavigated)
                m_PendingNavigation.OnNavigated.push_back(std::move(onNavigated));
            return;
        }
    }

    // A pending rename waits for its cell to bind, which can only happen while
    // its directory is the one on screen: navigating away expires it, so it
    // cannot fire on a much later revisit of that path.
    m_PendingRenamePath.clear();
    // A Poly Haven listing still in flight must not replace the folder.
    ++m_PolyhavenLoadGeneration;

    m_PendingNavigation = {};
    m_PendingNavigation.Dir = dir;
    if (onNavigated)
        m_PendingNavigation.OnNavigated.push_back(std::move(onNavigated));
    m_PendingNavigation.Generation = ScanDirectoryInBackground(dir);
}

bool AssetsBrowserController::IsNavigationPending() const
{
    return m_PendingNavigation.Generation != 0 && m_GridProvider &&
           m_PendingNavigation.Generation == m_GridProvider->GetListingGeneration();
}

uint64_t AssetsBrowserController::ScanDirectoryInBackground(const std::filesystem::path& dir)
{
    const uint64_t generation = m_GridProvider->NextListingGeneration();
    UIElement* postAnchor = m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_Grid);
    EngineCore::GetInstance().GetJobSystem().EnqueueWork(
        [this, dir, generation, postAnchor, postMutex = m_ScanPostMutex, alive = m_Alive, tags = m_GridProvider->GetTagSortKeyResolver()]()
        {
            auto scan = std::make_shared<AssetsGridDataProvider::DirectoryScan>(
                AssetsGridDataProvider::ScanDirectory(dir, tags));
            std::lock_guard<std::mutex> postLock(*postMutex);
            if (!alive->load(std::memory_order_acquire))
                return;
            postAnchor->PostAction([this, scan, generation, alive]()
            {
                if (alive->load(std::memory_order_acquire))
                    ApplyScannedDirectory(std::move(*scan), generation);
            });
        });
    return generation;
}

void AssetsBrowserController::ApplyScannedDirectory(AssetsGridDataProvider::DirectoryScan scan, uint64_t generation)
{
    const std::filesystem::path dir = scan.Dir;
    if (!m_GridProvider->ApplyDirectoryScan(std::move(scan), generation))
    {
        // Superseded: release what the callbacks hold (suppression guards).
        if (generation == m_PendingNavigation.Generation)
            m_PendingNavigation = {};
        return;
    }
    // The navigation is complete once its listing is on the provider. The tree
    // sync below re-enters NavigateToDirectory for this folder, which must not
    // find it pending.
    const bool navigated = generation == m_PendingNavigation.Generation;
    std::vector<std::function<void()>> callbacks;
    if (navigated)
    {
        callbacks = std::move(m_PendingNavigation.OnNavigated);
        m_PendingNavigation = {};
    }
    if (navigated && !m_PolyhavenTypeKey.empty())
    {
        // Leaving the Polyhaven online view — reset state so the normal
        // context menu and cell binder are used for real filesystem items.
        m_PolyhavenTypeKey.clear();
        m_PolyhavenManifest.clear();
        m_PolyhavenManifestFetchedEnd = 0;
    }
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List && m_ListProvider)
        m_List->RefreshFromProvider();
    DeferThumbnailsAfterNavigation();
    if (!navigated)
        return;
    SyncTreeSelectionToDirectory(dir);
    for (std::function<void()>& callback : callbacks)
        callback();
}

void AssetsBrowserController::WarmSearchIndexInBackground()
{
    UIElement* postAnchor = m_Grid ? static_cast<UIElement*>(m_Grid)
                                   : (m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_List));
    if (!m_GridProvider || !postAnchor || m_AssetsRoot.empty() || m_GridProvider->HasProjectSearchSnapshot())
        return;
    const uint64_t generation = m_GridProvider->GetProjectSearchGeneration();
    const AssetRegistry* registry = &EngineCore::GetInstance().GetAssetManager().GetRegistry();
    EngineCore::GetInstance().GetJobSystem().EnqueueWork(
        [this, root = m_AssetsRoot, registry, tags = m_GridProvider->GetTagSortKeyResolver(), generation, postAnchor,
         alive = m_Alive, postMutex = m_ScanPostMutex]()
        {
            auto scan = std::make_shared<AssetsGridDataProvider::ProjectSearchScan>(
                AssetsGridDataProvider::ScanProjectForSearch(root, registry, tags));
            std::lock_guard<std::mutex> postLock(*postMutex);
            if (!alive->load(std::memory_order_acquire))
                return;
            postAnchor->PostAction([this, scan, generation, alive]()
            {
                if (!alive->load(std::memory_order_acquire))
                    return;
                if (!m_GridProvider->ApplyProjectSearchScan(std::move(*scan), generation))
                    return;
                if (m_Grid)
                    m_Grid->RefreshFromProvider();
                if (m_List && m_ListProvider)
                    m_List->RefreshFromProvider();
            });
        },
        JobSystem::JobPriority::Background);
}

bool AssetsBrowserController::HandleUiReplayCommand(std::uint32_t commandId, std::string* outError)
{
    if (commandId != UiReplayCommandIds::AssetsNavSandboxRoot &&
        commandId != UiReplayCommandIds::AssetsNavSandboxSrc &&
        commandId != UiReplayCommandIds::AssetsNavSandboxDst)
        return false;

    if (m_AssetsRoot.empty())
    {
        if (outError)
            *outError = "AssetsBrowserController: assets root not set";
        return true;
    }

    const char* nameC = std::getenv("GE_UIREPLAY_ASSETS_SANDBOX");
    std::string name = nameC ? std::string(nameC) : std::string();
    if (name.empty())
    {
        if (outError)
            *outError = "AssetsBrowserController: GE_UIREPLAY_ASSETS_SANDBOX not set";
        return true;
    }

    std::filesystem::path dir = m_AssetsRoot / "UIReplaySandbox" / name;
    if (commandId == UiReplayCommandIds::AssetsNavSandboxSrc)
        dir /= "Src";
    if (commandId == UiReplayCommandIds::AssetsNavSandboxDst)
        dir /= "Dst";

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
    {
        if (outError)
            *outError = "AssetsBrowserController: sandbox dir missing: " + dir.string();
        return true;
    }

    NavigateToDirectory(dir);
    return true;
}

void AssetsBrowserController::RefreshDirectory(const std::filesystem::path& dir)
{
    if (!m_GridProvider || dir.empty())
        return;

    m_GridProvider->LoadDirectory(dir);
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List && m_ListProvider)
        m_List->RefreshFromProvider();
    DeferThumbnailsAfterNavigation();
}

void AssetsBrowserController::RevealImportedAssets(const std::filesystem::path& dest,
                                                   const std::vector<std::filesystem::path>& imported)
{
    if (dest.empty())
        return;

    if (m_TreeProvider && m_Tree && m_TreeProvider->OnDirectoryChanged(dest))
        m_Tree->RefreshFromProvider();

    RefreshDirectory(dest);
    if (!imported.empty())
        SetSelectionFromPaths(imported);
}

void AssetsBrowserController::ImportFilesIntoDirectory(const std::filesystem::path& dir)
{
    if (dir.empty() || IsAssetBrowserVirtualPath(dir))
        return;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
        return;

    const std::string destKey = AssetPathKey(dir);
    std::vector<std::filesystem::path> imported;
    const std::vector<std::filesystem::path> picked =
        Platform::SelectFiles(dir, "All Files", "*.*");
    if (picked.empty())
        return;
    // A platform that staged the pick in transient storage releases it once
    // the copies below are done; on desktop this is a no-op.
    struct StagingRelease
    {
        const std::vector<std::filesystem::path>& Paths;
        ~StagingRelease() { Platform::ReleaseTransientFiles(Paths); }
    } stagingRelease{picked};

    imported.reserve(picked.size());
    for (const auto& src : picked)
    {
        if (src.empty())
            continue;
        if (std::filesystem::is_directory(src, ec))
        {
            Logger::Log::Warning("Assets: import folder not supported {}", src.string());
            continue;
        }
        if (IsDirectChildOfAssetDir(dir, src))
        {
            Logger::Log::Info("Assets: skipped import, already in destination {}", src.string());
            continue;
        }

        const std::filesystem::path dst = MakeUniqueDestinationPath(dir, src);
        if (dst.empty())
            continue;

        if (!CopyFileByStreaming(src, dst))
            continue;
        imported.push_back(dst);
    }

    if (imported.empty())
        return;

    Editor::EditorSceneCommands::Get().RegisterImportedAssets(imported);

    if (m_TreeProvider && m_Tree && m_TreeProvider->OnDirectoryChanged(dir))
        m_Tree->RefreshFromProvider();
    RefreshDirectory(dir);
    SetSelectionFromPaths(imported);

    NotifyVcsStatusDirty();
    Logger::Log::Info("Assets: imported {} file(s) into {}", imported.size(), dir.string());
}

void AssetsBrowserController::DeleteAssetPaths(const std::vector<std::filesystem::path>& paths)
{
    if (!m_Context || paths.empty())
        return;

    auto NormalizePath = [](const std::filesystem::path& p) -> std::filesystem::path
    {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(p, ec);
        return ec ? p.lexically_normal() : canonical;
    };

    const std::filesystem::path assetsRoot = m_Context->AssetsRoot.empty()
                                                 ? std::filesystem::path{}
                                                 : NormalizePath(m_Context->AssetsRoot);

    // Lexical prune: if both a folder and something inside it are selected, keep
    // only the outer path so we do not stage the child twice.
    std::vector<std::filesystem::path> unique;
    unique.reserve(paths.size());
    for (const auto& p : paths)
    {
        if (p.empty())
            continue;
        const auto use = NormalizePath(p);
        if (!assetsRoot.empty() && !IsAssetPathStrictlyUnder(use, assetsRoot))
        {
            Logger::Log::Warning("Assets: refusing to delete '{}' (not under assets root)",
                                 use.string());
            continue;
        }
        unique.push_back(use);
    }
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    std::vector<std::filesystem::path> pruned;
    pruned.reserve(unique.size());
    for (const auto& p : unique)
    {
        bool nested = false;
        for (const auto& kept : pruned)
        {
            if (IsAssetPathStrictlyUnder(p, kept))
            {
                nested = true;
                break;
            }
        }
        if (!nested)
            pruned.push_back(p);
    }

    if (pruned.empty())
        return;

    std::vector<std::filesystem::path> parents;
    parents.reserve(pruned.size());
    for (const auto& p : pruned)
        parents.push_back(p.parent_path());

    auto onChanged = [this, parents]()
    {
        if (m_ItemSelection)
            m_ItemSelection->Clear();
        if (m_Tree)
            m_Tree->RefreshFromProvider();
        for (const auto& parent : parents)
        {
            if (!parent.empty())
                RefreshDirectory(parent);
        }
        if (m_GridProvider)
        {
            const auto& cur = m_GridProvider->GetCurrentDirectory();
            if (!cur.empty())
                RefreshDirectory(cur);
        }
        NotifyVcsStatusDirty();
    };

    if (m_Context->UndoRedo)
    {
        std::filesystem::path stagingRoot;
        if (!m_Context->AssetsRoot.empty())
            stagingRoot = m_Context->AssetsRoot.parent_path() / ".Editor" / "UndoTrash" /
                          GUID::Generate().ToString();
        else
            stagingRoot = std::filesystem::temp_directory_path() / "GameEngineUndoTrash" /
                          GUID::Generate().ToString();

        m_Context->UndoRedo->Execute(std::make_unique<Editor::DeleteAssetPathsCommand>(
            std::move(pruned), m_Context->Assets, std::move(stagingRoot), std::move(onChanged)));
        return;
    }

    // No undo stack: still prefer OS trash so the user can recover outside the editor.
    for (const auto& p : pruned)
    {
        if (m_Context->Assets)
        {
            auto& registry = m_Context->Assets->GetRegistry();
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec))
                (void)registry.TryUnregisterAssetByPath(p);
            else if (std::filesystem::is_directory(p, ec))
            {
                for (std::filesystem::recursive_directory_iterator it(p, ec), end;
                     !ec && it != end; it.increment(ec))
                {
                    if (it->is_regular_file(ec))
                        (void)registry.TryUnregisterAssetByPath(it->path());
                }
            }
        }
        if (Platform::MoveToTrash(p))
            continue;
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        if (ec)
            Logger::Log::Warning("Assets: failed to delete '{}': {}", p.string(), ec.message());
        else
            Logger::Log::Warning("Assets: OS trash unavailable; permanently removed '{}'", p.string());
    }
    onChanged();
}

void AssetsBrowserController::CreateNewFolderInDirectory(const std::filesystem::path& dir)
{
    if (dir.empty())
    {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
    {
        return;
    }

    auto MakeCandidate = [&dir](int index) -> std::filesystem::path
    {
        if (index <= 1)
        {
            return dir / "New Folder";
        }
        return dir / ("New Folder (" + std::to_string(index) + ")");
    };

    std::filesystem::path target;
    for (int i = 1; i < 1024; ++i)
    {
        auto candidate = MakeCandidate(i);
        if (!std::filesystem::exists(candidate, ec))
        {
            target = candidate;
            break;
        }
    }

    if (target.empty())
    {
        return;
    }

    // For the tree, capture whether this directory was a leaf *before* we
    // create the new folder. GetChildCount() will lazily populate children
    // from the current filesystem snapshot, which at this point does not
    // yet include the new folder.
    TreeId parentId = 0;
    int oldChildCount = -1;
    if (m_TreeProvider && m_Tree)
    {
        parentId = m_TreeProvider->FindIdForPath(dir);
        if (parentId != 0)
        {
            oldChildCount = m_TreeProvider->GetChildCount(parentId);
        }
    }

    if (!std::filesystem::create_directory(target, ec))
    {
        LOG_WARNING("AssetsBrowserController: Failed to create folder '{}': {}", target.string(), ec.message());
        return;
    }

    // Immediately refresh the grid for the directory where the folder was created.
    RefreshDirectory(dir);

    // Also invalidate the corresponding tree node so the new folder appears in
    // the tree. If this directory used to be a leaf and now has a child,
    // expand it so the newly created folder is visible immediately.
    if (m_TreeProvider && m_Tree)
    {
        if (m_TreeProvider->OnDirectoryChanged(dir))
        {
            const bool shouldExpand = (parentId != 0 && oldChildCount == 0);
            m_Tree->PostAction([this, parentId, shouldExpand]()
                               {
		                if (!m_Tree)
		                {
		                    return;
		                }
		                if (shouldExpand && parentId != 0)
		                {
		                    m_Tree->SetExpanded(parentId, true);
		                }
		                m_Tree->RefreshFromProvider(); });
        }
    }
}

namespace
{
static std::string SanitizeCSharpIdentifier(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_')
            out.push_back(ch);
    }
    if (out.empty())
        out = "NewScript";
    // C# identifiers cannot start with a digit.
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front())))
        out.insert(out.begin(), '_');
    return out;
}

// The six code-asset templates. Bodies are kept verbatim from the original per-type handlers;
// the descriptor table below points at them so the menu/dispatch/create paths stay table-driven.
static std::string MakeCSharpGameSystemBody(const std::string& name)
{
    const std::string cls = SanitizeCSharpIdentifier(name);
    return "using GameEngine.Scripting;\n"
           "using GameEngine.ECS;\n"
           "\n"
           "public class " + cls + " : GameSystem\n"
           "{\n"
           "    public override int Order => 0;\n"
           "\n"
           "    public override void OnCreate()\n"
           "    {\n"
           "    }\n"
           "\n"
           "    public override void OnUpdate(float deltaTime)\n"
           "    {\n"
           "    }\n"
           "\n"
           "    public override void OnDestroy()\n"
           "    {\n"
           "    }\n"
           "}\n";
}

static std::string MakeCSharpEntitySystemBody(const std::string& name)
{
    const std::string cls = SanitizeCSharpIdentifier(name);
    return "using System.Runtime.InteropServices;\n"
           "using GameEngine.Scripting;\n"
           "\n"
           "[StructLayout(LayoutKind.Sequential)]\n"
           "public struct " + cls + "Component : IComponent\n"
           "{\n"
           "    public float Value;\n"
           "}\n"
           "\n"
           "public partial struct " + cls + " : IEntitySystem\n"
           "{\n"
           "    public int Order => 0;\n"
           "\n"
           "    void Execute(ref " + cls + "Component component, float deltaTime)\n"
           "    {\n"
           "        // Process each entity with " + cls + "Component\n"
           "    }\n"
           "}\n";
}

static std::string MakeCSharpComponentBody(const std::string& name)
{
    const std::string cls = SanitizeCSharpIdentifier(name);
    return "using System.Runtime.InteropServices;\n"
           "using GameEngine.Scripting;\n"
           "\n"
           "[StructLayout(LayoutKind.Sequential)]\n"
           "public struct " + cls + " : IComponent\n"
           "{\n"
           "    public float Value;\n"
           "}\n";
}

static std::string MakeCppComponentBody(const std::string& name)
{
    const std::string id = SanitizeCSharpIdentifier(name);
    return "#pragma once\n"
           "\n"
           "// Native C++ ECS component. Inheriting ECS::ComponentBase makes the build-time\n"
           "// scanner auto-register it (no macros). Fields must be plain data (POD).\n"
           "#include <GameSDK/Component.h>\n"
           "\n"
           "struct " + id + " : ECS::ComponentBase\n"
           "{\n"
           "    float Value = 0.0f;\n"
           "};\n";
}

static std::string MakeCppGameSystemBody(const std::string& name)
{
    const std::string id = SanitizeCSharpIdentifier(name);
    return "#pragma once\n"
           "\n"
           "// Native C++ system: OnUpdate runs ONCE PER FRAME during play. Inheriting\n"
           "// ECS::SystemBase makes the build-time scanner auto-register it.\n"
           "#include <GameSDK/System.h>\n"
           "\n"
           "struct " + id + " : ECS::SystemBase\n"
           "{\n"
           "    void OnUpdate(ECS::World& /*world*/, float /*dt*/)\n"
           "    {\n"
           "        // e.g. world.Query<MyComponent>().Each([dt](MyComponent& c){ /* ... */ });\n"
           "    }\n"
           "};\n";
}

static std::string MakeCppEntitySystemBody(const std::string& name)
{
    const std::string id = SanitizeCSharpIdentifier(name);
    return "#pragma once\n"
           "\n"
           "// Native C++ PER-ENTITY system: inherit ECS::EntitySystem<Self> and write\n"
           "// ForEach(comps..., float dt). The queried components are INFERRED from the\n"
           "// parameters; ForEach runs once per matching entity, each frame during play.\n"
           "// Replace Transform with your own component(s).\n"
           "#include <GameSDK/System.h>\n"
           "#include \"Components/Transform.h\"\n"
           "\n"
           "struct " + id + " : ECS::EntitySystem<" + id + ">\n"
           "{\n"
           "    void ForEach(GameEngine::Components::Transform& /*transform*/, float /*dt*/)\n"
           "    {\n"
           "        // Runs per entity with a Transform. e.g. transform.matrix[13] += dt; (Y)\n"
           "    }\n"
           "};\n";
}

// Single source of truth. Order is menu order (priorities ascending) == dispatch order.
const Editor::CodeAssetDescriptor kCodeAssetDescriptors[] = {
    { EditorContextMenu::DirectoryAction::CreateComponent,      Editor::kCmdCreateComponent,      "Create/C#/Component",      -959, "NewComponent",    ".cs", "Create Component",          &MakeCSharpComponentBody    },
    { EditorContextMenu::DirectoryAction::CreateGameSystem,     Editor::kCmdCreateGameSystem,     "Create/C#/Game System",    -958, "NewGameSystem",   ".cs", "Create Game System",        &MakeCSharpGameSystemBody   },
    { EditorContextMenu::DirectoryAction::CreateEntitySystem,   Editor::kCmdCreateEntitySystem,   "Create/C#/Entity System",  -957, "NewEntitySystem", ".cs", "Create Entity System",      &MakeCSharpEntitySystemBody },
    { EditorContextMenu::DirectoryAction::CreateCppComponent,   Editor::kCmdCreateCppComponent,   "Create/C++/Component",     -956, "NewComponent",    ".h",  "Create C++ Component",      &MakeCppComponentBody       },
    { EditorContextMenu::DirectoryAction::CreateCppGameSystem,  Editor::kCmdCreateCppGameSystem,  "Create/C++/Game System",   -955, "NewGameSystem",   ".h",  "Create C++ Game System",    &MakeCppGameSystemBody      },
    { EditorContextMenu::DirectoryAction::CreateCppEntitySystem, Editor::kCmdCreateCppEntitySystem, "Create/C++/Entity System", -954, "NewEntitySystem", ".h",  "Create C++ Entity System",  &MakeCppEntitySystemBody    },
};
} // namespace

namespace Editor
{
const CodeAssetDescriptor* GetCodeAssetDescriptors(std::size_t& outCount)
{
    outCount = std::size(kCodeAssetDescriptors);
    return kCodeAssetDescriptors;
}
} // namespace Editor

void AssetsBrowserController::HandleDirectoryAction(const std::filesystem::path& directory,
                                                     EditorContextMenu::DirectoryAction action)
{
    switch (action)
    {
    case EditorContextMenu::DirectoryAction::NewFolder:
        CreateNewFolderInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::NewSmartFolder:
        CreateNewSmartFolder();
        break;
    case EditorContextMenu::DirectoryAction::Refresh:
        RefreshDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateScene:
        CreateNewSceneInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateCSharpScript:
        CreateNewCSharpScriptInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateMaterial:
        CreateNewMaterialInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateAnimationLibrary:
        CreateNewAnimationLibraryInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateAnimationController:
        CreateNewAnimationControllerInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateTimeline:
        CreateNewTimelineInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateClipSet:
        CreateNewClipSetInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateSpriteFrames:
        CreateNewSpriteFramesInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateShaderGraph:
        CreateNewShaderGraphInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateSurfaceShader:
        CreateNewSurfaceShaderInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateNavGrid:
        CreateNewNavGridInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateNavMesh:
        CreateNewNavMeshInDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::CreateGameSystem:
    case EditorContextMenu::DirectoryAction::CreateEntitySystem:
    case EditorContextMenu::DirectoryAction::CreateComponent:
    case EditorContextMenu::DirectoryAction::CreateCppComponent:
    case EditorContextMenu::DirectoryAction::CreateCppGameSystem:
    case EditorContextMenu::DirectoryAction::CreateCppEntitySystem:
    {
        std::size_t count = 0;
        const auto* table = Editor::GetCodeAssetDescriptors(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (table[i].Action == action)
            {
                CreateCodeAssetInDirectory(directory, table[i]);
                break;
            }
        }
        break;
    }
    case EditorContextMenu::DirectoryAction::Import:
        ImportFilesIntoDirectory(directory);
        break;
    case EditorContextMenu::DirectoryAction::Delete:
        DeleteAssetPaths({directory});
        break;
    }
}

void AssetsBrowserController::CreateNewSceneInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewScene", ".scene",
        [](const std::string& name) { return "[scene name=\"" + name + "\" version=1]\n\n"; },
        "Create Scene", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewCSharpScriptInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewScript", ".cs",
        [](const std::string& name) {
            const std::string cls = SanitizeCSharpIdentifier(name);
            return "using System;\n"
                   "using GameEngine.Scripting;\n"
                   "\n"
                   "public class " + cls + "\n"
                   "{\n"
                   "    [InitializeOnLoad]\n"
                   "    public static void OnInitScript()\n"
                   "    {\n"
                   "        Console.WriteLine($\"{nameof(" + cls + ")} Initialized.\");\n"
                   "    }\n"
                   "}\n";
        },
        "Create C# Script", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewMaterialInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateDefaultMaterialFile(dir, "NewMaterial",
                                                     m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewAnimationLibraryInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewAnimationLibrary", ".animlib",
        [](const std::string&) {
            return "{\n"
                   "  \"schemaVersion\": 1,\n"
                   "  \"assetType\": \"AnimationLibrary\",\n"
                   "  \"animations\": []\n"
                   "}\n";
        },
        "Create Animation Library", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewAnimationControllerInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewAnimationController", ".animcontroller",
        [](const std::string&) {
            return "{\n"
                   "  \"schemaVersion\": 1,\n"
                   "  \"assetType\": \"AnimationController\",\n"
                   "  \"entryState\": \"\",\n"
                   "  \"parameters\": [],\n"
                   "  \"states\": [],\n"
                   "  \"transitions\": []\n"
                   "}\n";
        },
        "Create Animation Controller", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewTimelineInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewTimeline", ".timeline",
        [](const std::string&) {
            return "{\n"
                   "  \"schemaVersion\": 1,\n"
                   "  \"assetType\": \"Timeline\",\n"
                   "  \"tracks\": []\n"
                   "}\n";
        },
        "Create Timeline", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewClipSetInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewClipSet", ".clipset",
        [](const std::string&) {
            return "{\n"
                   "  \"schemaVersion\": 1,\n"
                   "  \"assetType\": \"ClipSet\",\n"
                   "  \"lanes\": []\n"
                   "}\n";
        },
        "Create Clip Set", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewSpriteFramesInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewSpriteFrames", ".spriteframes",
        [](const std::string&) {
            return "{\n"
                   "  \"schemaVersion\": 1,\n"
                   "  \"assetType\": \"SpriteFrames\",\n"
                   "  \"animations\": []\n"
                   "}\n";
        },
        "Create Sprite Frames", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewShaderGraphInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewShaderGraph", ".glsl",
        [](const std::string& stem) {
            return Graph::MakeShaderGraphTemplateSource(stem);
        },
        "Create Shader Graph", m_Context->UndoRedo, m_Context->Assets);
    if (result.path.empty())
        return;

    // Companion material pre-wired to the new graph (path + GUID), matching
    // Create → Surface Shader: the pair is assignable to a mesh immediately.
    MaterialDocument doc = Graph::MakeShaderGraphTemplateMaterial(result.path.stem().string());
    if (m_Context->Assets)
    {
        const GUID graphGuid = m_Context->Assets->GetRegistry().GetAssetGUID(result.path);
        if (!graphGuid.IsNull())
            doc.surfaceShaderGuid = graphGuid.ToString();
    }
    Editor::CreateAssetFile(dir, result.path.stem().string(), ".material",
        [&doc](const std::string& stem) {
            doc.materialName = stem;
            return SerializeMaterialDocument(doc).dump(2) + "\n";
        },
        "Create Shader Graph Material", m_Context->UndoRedo, m_Context->Assets);
    RefreshDirectory(dir);

    // The graph already carries its master node, so opening it lands the author
    // on a compiling graph rather than an empty canvas.
    NavigateToAndSelectAsset(result.path);
    if (m_OnOpenAsset)
        m_OnOpenAsset(result.path);
}

void AssetsBrowserController::CreateNewSurfaceShaderInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, "NewSurface", ".glsl",
        [](const std::string&) {
            return Rendering::MakeSurfaceShaderTemplateSource();
        },
        "Create Surface Shader", m_Context->UndoRedo, m_Context->Assets);
    if (result.path.empty())
        return;

    // Companion material pre-wired to the new shader (path + GUID), so the
    // pair is assignable the moment it exists — no separate Create → Material
    // plus a drag onto the Surface Shader field.
    MaterialDocument doc =
        Rendering::MakeSurfaceShaderTemplateMaterial(result.path.stem().string());
    if (m_Context->Assets)
    {
        // Resolve through AssetManager, which registers the just-created file when
        // nothing has yet: a raw registry lookup finds nothing for an unregistered
        // path, and the companion material would be written without its shader GUID.
        const GUID shaderGuid = m_Context->Assets->ResolveAssetGuid(result.path);
        if (!shaderGuid.IsNull())
            doc.surfaceShaderGuid = shaderGuid.ToString();
    }
    Editor::CreateAssetFile(dir, result.path.stem().string(), ".material",
        [&doc](const std::string& stem) {
            doc.materialName = stem;
            return SerializeMaterialDocument(doc).dump(2) + "\n";
        },
        "Create Surface Shader Material", m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewNavGridInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateDefaultNavGridFile(dir, "NewNavGrid",
                                                    m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateNewNavMeshInDirectory(const std::filesystem::path& dir)
{
    if (!m_Context) return;
    auto result = Editor::CreateDefaultNavMeshFile(dir, "NewNavMesh",
                                                    m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::CreateCodeAssetInDirectory(const std::filesystem::path& dir,
                                                          const Editor::CodeAssetDescriptor& descriptor)
{
    if (!m_Context) return;
    auto result = Editor::CreateAssetFile(dir, descriptor.BaseName, descriptor.Extension,
        descriptor.MakeBody, descriptor.UndoLabel, m_Context->UndoRedo, m_Context->Assets);
    RevealCreatedAsset(result.path);
}

void AssetsBrowserController::RevealAsset(const std::filesystem::path& assetPath, std::function<void()> onSelected)
{
    // The file is newer than the provider's listing and the watcher is up to a
    // second away: reload the directory now so the selection can find it.
    RefreshDirectory(assetPath.parent_path());
    NavigateToAndSelectAsset(assetPath, std::move(onSelected));
}

void AssetsBrowserController::RevealCreatedAsset(const std::filesystem::path& createdPath)
{
    if (createdPath.empty())
        return;
    RevealAsset(createdPath, [this, createdPath]() { BeginRename(createdPath); });
}

void AssetsBrowserController::BeginRenameOfSelection()
{
    BeginRename(GetPrimarySelectedAssetPath());
}

void AssetsBrowserController::BeginRename(const std::filesystem::path& assetPath)
{
    if (!m_Context || assetPath.empty() || IsAssetBrowserVirtualPath(assetPath))
        return;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(assetPath, ec))
        return;

    Label* nameLabel = FindNameLabelForPath(assetPath);
    if (nameLabel)
    {
        m_PendingRenamePath.clear();
        UIElement* view = IsGridViewShowing() ? static_cast<UIElement*>(m_Grid) : static_cast<UIElement*>(m_List);
        m_Rename.Begin(nameLabel, view, assetPath, m_Context->UndoRedo, m_Context->Assets,
                       [this](const std::filesystem::path& renamedTo) { OnAssetRenamed(renamedTo); });
        return;
    }
    // Not bound yet: the view binds it on its next virtualization pass.
    m_PendingRenamePath = assetPath;
}

void AssetsBrowserController::StartPendingRenameIfBound(const std::filesystem::path& boundPath)
{
    if (m_PendingRenamePath.empty())
        return;
    if (!PathsReferToSameEntry(boundPath, AssetPathKey(m_PendingRenamePath)))
        return;

    const std::filesystem::path path = std::move(m_PendingRenamePath);
    m_PendingRenamePath.clear();
    // Binding runs inside the view's virtualization pass; the editor field goes
    // into the cell once that pass has finished.
    UIElement* anchor = m_Grid ? static_cast<UIElement*>(m_Grid) : static_cast<UIElement*>(m_List);
    if (anchor)
        anchor->PostAction([this, path]() { BeginRename(path); });
}

bool AssetsBrowserController::IsGridViewShowing() const
{
    if (!m_Grid)
        return false;
    if (!m_List)
        return true;
    // AssetsPanel toggles the views through their Display override.
    const std::optional<DisplayMode> display = m_Grid->Overrides().Get(Style::Display);
    return !display.has_value() || *display != DisplayMode::None;
}

Label* AssetsBrowserController::FindNameLabelForPath(const std::filesystem::path& assetPath) const
{
    const std::string targetKey = AssetPathKey(assetPath);

    if (IsGridViewShowing())
    {
        if (!m_GridProvider)
            return nullptr;
        const int count = m_GridProvider->GetItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (PathsReferToSameEntry(m_GridProvider->GetPath(m_GridProvider->GetItemId(i)), targetKey))
                return m_Grid->GetTitleLabelForIndex(i);
        }
        return nullptr;
    }

    if (!m_List || !m_ListProvider)
        return nullptr;
    const int count = m_ListProvider->GetItemCount();
    for (int i = 0; i < count; ++i)
    {
        if (!PathsReferToSameEntry(m_ListProvider->GetPath(m_ListProvider->GetItemId(i)), targetKey))
            continue;
        UIElement* row = m_List->GetCellForIndex(i);
        return row ? FindDescendantLabelWithClass(*row, kListNameLabelClass) : nullptr;
    }
    return nullptr;
}

void AssetsBrowserController::OnAssetRenamed(const std::filesystem::path& renamedTo)
{
    NotifyVcsStatusDirty();
    // Follow the file under its new name — silently. The selection is still the
    // same asset, so recording it as an undoable "Assets Selection" would land a
    // NEW command while the rename itself is being undone or redone (the reveal
    // completes over deferred UI turns) and clear the redo branch: undoing a
    // rename must leave the rename redoable.
    RefreshDirectory(renamedTo.parent_path());
    NavigateToAndSelectAssetSilent(renamedTo);
}

void AssetsBrowserController::RefreshListView()
{
    if (m_List && m_ListProvider)
    {
        m_List->RefreshFromProvider();
    }
}

void AssetsBrowserController::SetSmartFolderController(SmartFolderController* controller)
{
    m_SmartFolderController = controller;
    if (controller)
        controller->SetAssetsRoot(m_AssetsRoot);
    if (m_TreeProvider) {
        m_TreeProvider->SetSmartFolderManager(controller ? &controller->GetManager() : nullptr);
    }
    RefreshSmartFolders();
}

void AssetsBrowserController::RefreshSmartFolders()
{
    if (m_SmartFolderController)
        m_SmartFolderController->InvalidateResults();
    if (m_TreeProvider) {
        m_TreeProvider->RefreshSmartFolders();
        Logger::Log::Debug("[AssetsBrowserController] RefreshSmartFolders: provider has {} smart folders",
            m_SmartFolderController ? m_SmartFolderController->GetManager().GetAll().size() : 0);
    }
    if (m_Tree) {
        m_Tree->RefreshFromProvider();
        if (!m_SmartFoldersStartupExpansionApplied)
        {
            m_Tree->SetExpanded(AssetsTreeDataProvider::kSmartFoldersSectionId, m_SmartFoldersExpandedOnStartup);
            m_SmartFoldersStartupExpansionApplied = true;
            m_Tree->RefreshFromProvider();
        }
    }
    
    // If a smart folder is currently selected, refresh its grid content
    if (m_SmartFolderController && m_SmartFolderController->HasSelection()) {
        ShowSmartFolderInGrid(m_SmartFolderController->GetSelectedId());
    }
}

void AssetsBrowserController::SetSmartFoldersAtTop(bool atTop)
{
    if (m_TreeProvider) {
        m_TreeProvider->SetSmartFoldersAtTop(atTop);
    }
    if (m_Tree) {
        m_Tree->RefreshFromProvider();
    }
}

void AssetsBrowserController::SetSmartFoldersExpandedOnStartup(bool expanded)
{
    m_SmartFoldersExpandedOnStartup = expanded;
    m_SmartFoldersStartupExpansionApplied = true;
    if (m_Tree)
    {
        m_Tree->SetExpanded(AssetsTreeDataProvider::kSmartFoldersSectionId, expanded);
        m_Tree->RefreshFromProvider();
    }
}

void AssetsBrowserController::SetOnlineAssetsEnabled(bool enabled)
{
    if (m_TreeProvider) {
        m_TreeProvider->SetOnlineAssetsEnabled(enabled);
    }
    if (m_Tree) {
        m_Tree->RefreshFromProvider();
    }
}

void AssetsBrowserController::SetPolyHavenEnabled(bool enabled)
{
    if (m_TreeProvider) {
        m_TreeProvider->SetPolyHavenEnabled(enabled);
    }
    if (m_Tree) {
        m_Tree->RefreshFromProvider();
    }
}

void AssetsBrowserController::SelectSmartFolder(const std::string& smartFolderId)
{
    if (m_Tree && m_TreeProvider && m_TreeSelection)
    {
        const bool wasSuppressingTreeSelect = m_SuppressTreeSelectSideEffects;
        m_SuppressTreeSelectSideEffects = true;
        const TreeId treeId = m_TreeProvider->GetTreeIdForSmartFolder(smartFolderId);
        if (treeId != 0)
        {
            bool treeChanged = false;
            if (!m_Tree->IsExpanded(AssetsTreeDataProvider::kSmartFoldersSectionId))
            {
                m_Tree->SetExpanded(AssetsTreeDataProvider::kSmartFoldersSectionId, true);
                treeChanged = true;
            }

            const auto existing = m_TreeSelection->GetSelection();
            if (existing.size() != 1 || existing.front() != treeId)
            {
                m_TreeSelection->SetSingle(treeId);
                treeChanged = true;
            }
            if (treeChanged)
                m_Tree->RefreshFromProvider();
            m_Tree->SyncSelectionVisuals();
        }
        m_SuppressTreeSelectSideEffects = wasSuppressingTreeSelect;
    }

    m_PolyhavenTypeKey.clear();
    ClearAssetItemSelectionForDatasetSwitch();

    if (m_SmartFolderController)
        m_SmartFolderController->SetSelected(smartFolderId);

    ShowSmartFolderInGrid(smartFolderId);
}

int AssetsBrowserController::GetVisibleItemCount() const
{
    return m_GridProvider ? m_GridProvider->GetItemCount() : 0;
}

std::filesystem::path AssetsBrowserController::GetVisibleDirectory() const
{
    return m_GridProvider ? m_GridProvider->GetCurrentDirectory() : std::filesystem::path{};
}

void AssetsBrowserController::CreateNewSmartFolder()
{
    if (!m_SmartFolderController) {
        return;
    }

    std::string newId = m_SmartFolderController->GetManager().Create("New Smart Folder", false);
    RefreshSmartFolders();
    
    // Expand the Smart Folders section to show the new folder
    if (m_Tree && m_TreeProvider) {
        m_Tree->SetExpanded(AssetsTreeDataProvider::kSmartFoldersSectionId, true);
        
        // Select the new smart folder in the tree
        TreeId treeId = m_TreeProvider->GetTreeIdForSmartFolder(newId);
        if (treeId != 0 && m_TreeSelection) {
            m_TreeSelection->SetSingle(treeId);
            m_Tree->RefreshFromProvider(); // Update visual selection
        }
    }
    
    // Select the new smart folder to show its configuration in inspector
    SelectSmartFolder(newId);
}

void AssetsBrowserController::ShowSmartFolderInGrid(const std::string& smartFolderId)
{
    if (!m_SmartFolderController || smartFolderId.empty() || !m_GridProvider) {
        return;
    }
    
    const SmartFolder* folder = m_SmartFolderController->GetManager().GetById(smartFolderId);
    if (!folder) {
        return;
    }
    ClearAssetItemSelectionForDatasetSwitch();
    
    // Don't evaluate if no filters are defined — an unconfigured smart folder
    // would match every asset, flooding the grid and stalling the editor.
    if (folder->Filters.empty())
    {
        m_GridProvider->LoadFiles({});
        if (m_Grid) m_Grid->RefreshFromProvider();
        if (m_List) m_List->RefreshFromProvider();
        return;
    }

    // Query through the dedicated controller so evaluation and cache lifetime
    // stay independent of the Assets browser UI.
    AssetRegistry* registry = &EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const std::vector<std::filesystem::path>* filesToLoad =
        m_SmartFolderController->Evaluate(smartFolderId, registry);
    if (!filesToLoad)
        return;
    
    // Clear search query since we're using direct file list
    m_SearchQuery.clear();
    if (m_GridProvider) {
        m_GridProvider->SetSearchQuery("");
    }
    
    // Load the matching files directly into the grid
    m_GridProvider->LoadFiles(*filesToLoad, registry);
    
    if (m_Grid) {
        m_Grid->RefreshFromProvider();
    }
    if (m_List) {
        m_List->RefreshFromProvider();
    }
    DeferThumbnailsAfterNavigation();
}

namespace
{
    constexpr size_t kPolyhavenBatchSize = 50;

    constexpr int kScheduleCategoriesMaxRounds = 10;
    constexpr int kScheduleCategoriesRoundDelaySeconds = 3;
}

void SchedulePolyhavenCategoriesLoad(const EditorContext* context, AssetsTreeDataProvider* treeProvider, TreeView* tree,
                                     std::shared_ptr<std::atomic<bool>> alive)
{
    if (!treeProvider || !tree)
        return;
    const UI::UiPostHandle post = tree->GetPostHandle();
    SubmitPolyhavenTransfer(context, [treeProvider, tree, post, alive]()
    {
        auto toSlugAndLabel = [](const PolyhavenService::CategoriesResult& r) {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& p : r.pathAndLabels)
            {
                std::string pathStr = p.first.generic_string();
                size_t lastSlash = pathStr.rfind('/');
                std::string slug = lastSlash != std::string::npos ? pathStr.substr(lastSlash + 1) : pathStr;
                out.emplace_back(slug, p.second);
            }
            return out;
        };
        std::vector<std::pair<std::string, std::string>> hdrisCats, texCats, modelCats;
        for (int round = 0; round < kScheduleCategoriesMaxRounds; ++round)
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            if (round > 0)
                std::this_thread::sleep_for(std::chrono::seconds(kScheduleCategoriesRoundDelaySeconds));
            if (hdrisCats.empty())
                hdrisCats = toSlugAndLabel(PolyhavenService::FetchCategories("hdris"));
            if (texCats.empty())
                texCats = toSlugAndLabel(PolyhavenService::FetchCategories("textures"));
            if (modelCats.empty())
                modelCats = toSlugAndLabel(PolyhavenService::FetchCategories("models"));
            if (!hdrisCats.empty() && !texCats.empty() && !modelCats.empty())
                break;
        }
        if (!alive->load(std::memory_order_acquire))
            return;
        post.Post([treeProvider, tree, hdrisCats, texCats, modelCats, alive]()
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            treeProvider->SetPolyhavenTypeCategories("hdris", hdrisCats);
            treeProvider->SetPolyhavenTypeCategories("textures", texCats);
            treeProvider->SetPolyhavenTypeCategories("models", modelCats);
            tree->RefreshFromProvider();
        });
    });
}

void AssetsBrowserController::SelectPolyhavenCategory(const std::string& type)
{
    if (type.empty() || !m_GridProvider)
        return;
    if (m_SmartFolderController)
        m_SmartFolderController->ClearSelection();
    m_GridProvider->SetSearchQuery("");

    std::string typeKeyPlaceholder = PolyhavenService::TypeKeyForType(type);
    m_GridProvider->LoadRemoteItems({}, typeKeyPlaceholder);
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List && m_ListProvider)
        m_List->RefreshFromProvider();

    const uint32_t loadGen = ++m_PolyhavenLoadGeneration;
    std::atomic<uint32_t>* pLoadGeneration = &m_PolyhavenLoadGeneration;
    AssetsGridDataProvider* gridProvider = m_GridProvider.get();
    GridView* grid = m_Grid;
    ListView* list = m_List;
    AssetsListDataProvider* listProvider = m_ListProvider.get();
    const UI::UiPostHandle post =
        PostHandleFor(m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_Grid));

    auto alive = m_Alive;

    SubmitPolyhavenTransfer(m_Context, [type, loadGen, pLoadGeneration, gridProvider, grid, list, listProvider, post, alive]()
    {
        auto result = PolyhavenService::FetchCategories(type);
        if (!alive->load(std::memory_order_acquire))
            return;
        post.Post([result, loadGen, pLoadGeneration, gridProvider, grid, list, listProvider, alive]()
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            if (!pLoadGeneration || pLoadGeneration->load(std::memory_order_relaxed) != loadGen)
                return;
            if (!gridProvider)
                return;
            gridProvider->LoadRemoteItems(result.pathAndLabels, result.typeKey);
            if (grid)
                grid->RefreshFromProvider();
            if (list && listProvider)
                list->RefreshFromProvider();
        });
    });
}

void AssetsBrowserController::ShowPolyhavenContextMenu(const std::filesystem::path& itemPath, float x, float y)
{
    std::string slug = itemPath.stem().string();
    if (slug.empty() || slug == "loading" || !m_Context) return;

    std::string url = "https://polyhaven.com/a/" + slug;
    std::string phType;
    if (m_PolyhavenTypeKey.find("HDRI") != std::string::npos) phType = "hdris";
    else if (m_PolyhavenTypeKey.find("Texture") != std::string::npos) phType = "textures";
    else if (m_PolyhavenTypeKey.find("Model") != std::string::npos) phType = "models";

    // Collect all selected slugs for multi-select download.
    std::vector<std::string> selectedSlugs;
    if (m_ItemSelection)
    {
        for (auto id : m_ItemSelection->GetSelection())
        {
            std::filesystem::path selPath;
            if (m_GridProvider)
                selPath = m_GridProvider->GetPath(static_cast<GridId>(id));
            else if (m_ListProvider)
                selPath = m_ListProvider->GetPath(static_cast<ListId>(id));
            if (!selPath.empty())
            {
                std::string s = selPath.stem().string();
                if (!s.empty() && s != "loading")
                    selectedSlugs.push_back(std::move(s));
            }
        }
    }
    // Ensure the right-clicked item is included
    if (selectedSlugs.empty())
        selectedSlugs.push_back(slug);

    auto menu = CreateContextMenu();

    constexpr uint32_t kCmdDownload = 1;
    [[maybe_unused]] constexpr uint32_t kCmdShowOriginal = 2;
    constexpr uint32_t kCmdAddToBookmarks = 3;
    constexpr uint32_t kCmdGoToImported = 4;
    constexpr uint32_t kCmdDownloadDiffuse = 10;
    constexpr uint32_t kCmdDownloadNormal = 11;
    constexpr uint32_t kCmdDownloadRoughness = 12;
    constexpr uint32_t kCmdDownloadMetallic = 13;
    constexpr uint32_t kCmdDownloadAO = 14;
    constexpr uint32_t kCmdDownloadDisplacement = 15;
    constexpr uint32_t kCmdDownloadARM = 16;
    constexpr uint32_t kCmdDownloadAllMaps = 17;
    [[maybe_unused]] constexpr uint32_t kCmdTagBase = 100;
    [[maybe_unused]] constexpr uint32_t kCmdTagMax = 200;

    // Check if this asset has been fully downloaded (model + all textures)
    std::filesystem::path downloadDir = m_AssetsRoot / "Polyhaven" / slug;
    bool isDownloaded = m_Polyhaven->IsFullyDownloaded(slug, m_AssetsRoot);

    // Show count in label if multi-selected
    if (selectedSlugs.size() > 1)
        menu->AddItem(0, "Download " + std::to_string(selectedSlugs.size()) + " Assets", kCmdDownload);
    else
        menu->AddItem(0, "Download", kCmdDownload);
    menu->SetItemIcon(kCmdDownload, EditorIcons::kCloud);

    // Add individual PBR map download options for textures
    if (phType == "textures" && selectedSlugs.size() == 1)
    {
        menu->AddSeparator(0);
        menu->AddItem(0, "Download Diffuse Map", kCmdDownloadDiffuse);
        menu->SetItemIcon(kCmdDownloadDiffuse, EditorIcons::kMaterial);
        menu->AddItem(0, "Download Normal Map", kCmdDownloadNormal);
        menu->SetItemIcon(kCmdDownloadNormal, EditorIcons::kMaterial);
        menu->AddItem(0, "Download Roughness Map", kCmdDownloadRoughness);
        menu->SetItemIcon(kCmdDownloadRoughness, EditorIcons::kMaterial);
        menu->AddItem(0, "Download Metallic Map", kCmdDownloadMetallic);
        menu->SetItemIcon(kCmdDownloadMetallic, EditorIcons::kMaterial);
        menu->AddItem(0, "Download AO Map", kCmdDownloadAO);
        menu->SetItemIcon(kCmdDownloadAO, EditorIcons::kMaterial);
        menu->AddItem(0, "Download Displacement Map", kCmdDownloadDisplacement);
        menu->SetItemIcon(kCmdDownloadDisplacement, EditorIcons::kMaterial);
        menu->AddItem(0, "Download ARM Map", kCmdDownloadARM);
        menu->SetItemIcon(kCmdDownloadARM, EditorIcons::kMaterial);
        menu->AddSeparator(0);
        menu->AddItem(0, "Download All Maps", kCmdDownloadAllMaps);
        menu->SetItemIcon(kCmdDownloadAllMaps, EditorIcons::kCloud);
    }
    
    if (isDownloaded)
    {
        menu->AddItem(0, "Show Imported Asset", kCmdGoToImported);
        menu->SetItemIcon(kCmdGoToImported, EditorIcons::kEye);
    }

    // Add to Bookmarks
    if (m_OnAddToBookmarks)
    {
        menu->AddSeparator(0);
        menu->AddItem(0, "Add to Bookmarks", kCmdAddToBookmarks);
        menu->SetItemIcon(kCmdAddToBookmarks, EditorIcons::kPlus);
    }

    std::filesystem::path assetsRoot = m_AssetsRoot;
    std::filesystem::path capturedPath = itemPath;
    const UI::UiPostHandle post = PostHandleFor(m_Grid ? static_cast<UIElement*>(m_Grid) : static_cast<UIElement*>(m_List));
    auto onAddToBookmarks = m_OnAddToBookmarks;
    auto alive = m_Alive;
    AssetsBrowserController* controller = this;
    const EditorContext* context = m_Context;

    menu->SetCommandHandler([slug, phType, assetsRoot, post, capturedPath, downloadDir,
                             onAddToBookmarks, alive, controller, context,
                             selectedSlugs = std::move(selectedSlugs)](uint32_t cmd)
    {
        if (cmd == kCmdGoToImported && alive->load(std::memory_order_acquire))
        {
            std::filesystem::path mainFile;
            for (const char* ext : {".gltf", ".glb", ".hdr", ".png", ".jpg"})
            {
                auto candidate = downloadDir / (slug + ext);
                std::error_code ec2;
                if (std::filesystem::exists(candidate, ec2))
                {
                    mainFile = candidate;
                    break;
                }
            }
            if (!mainFile.empty())
                controller->NavigateToAndSelectAsset(mainFile);
            else
                controller->NavigateToDirectory(downloadDir);
        }
        else if (cmd == kCmdAddToBookmarks && onAddToBookmarks)
        {
            onAddToBookmarks({capturedPath});
        }
        else if (cmd == kCmdDownload)
        {
            SubmitPolyhavenTransfer(context, [selectedSlugs, phType, assetsRoot, post, alive, controller]()
            {
                for (const auto& itemSlug : selectedSlugs)
                {
                    std::filesystem::path destDir = assetsRoot / "Polyhaven" / itemSlug;
                    std::error_code ec;
                    if (std::filesystem::is_directory(destDir, ec))
                    {
                        Logger::Log::Info("Polyhaven: '{}' already downloaded, skipping", itemSlug);
                        continue;
                    }
                    PolyhavenService::DownloadAsset(itemSlug, phType, destDir);
                }

                if (alive->load(std::memory_order_acquire))
                {
                    post.Post([alive, controller]() {
                        if (!alive->load(std::memory_order_acquire))
                            return;
                        controller->m_Polyhaven->ClearDownloadCache();
                        if (controller->m_Grid)
                            controller->m_Grid->RefreshFromProvider();
                        if (controller->m_List)
                            controller->m_List->RefreshFromProvider();
                    });
                }
            });
        }
        else if (phType == "textures" && selectedSlugs.size() == 1 && 
                 cmd >= kCmdDownloadDiffuse && cmd <= kCmdDownloadAllMaps)
        {
            // Individual PBR map download commands
            std::string mapType;
            switch (cmd)
            {
                case kCmdDownloadDiffuse: mapType = "Diffuse"; break;
                case kCmdDownloadNormal: mapType = "nor_gl"; break;
                case kCmdDownloadRoughness: mapType = "Rough"; break;
                case kCmdDownloadMetallic: mapType = "Metallic"; break;
                case kCmdDownloadAO: mapType = "AO"; break;
                case kCmdDownloadDisplacement: mapType = "Displacement"; break;
                case kCmdDownloadARM: mapType = "arm"; break;
                case kCmdDownloadAllMaps: mapType = "all"; break;
            }

            SubmitPolyhavenTransfer(context, [slug, assetsRoot, post, alive, controller, mapType]()
            {
                std::filesystem::path destDir = assetsRoot / "Polyhaven" / slug;
                std::error_code ec;
                std::filesystem::create_directories(destDir, ec);

                if (mapType == "all")
                {
                    // Download all maps
                    PolyhavenService::DownloadTextureMaps(slug, destDir, {});
                }
                else
                {
                    // Download specific map
                    PolyhavenService::DownloadTextureMaps(slug, destDir, mapType);
                }

                if (alive->load(std::memory_order_acquire))
                {
                    post.Post([alive, controller]() {
                        if (!alive->load(std::memory_order_acquire))
                            return;
                        controller->m_Polyhaven->ClearDownloadCache();
                        if (controller->m_Grid)
                            controller->m_Grid->RefreshFromProvider();
                        if (controller->m_List)
                            controller->m_List->RefreshFromProvider();
                    });
                }
            });
        }
    });
    menu->Show(m_Context->MainWindow, static_cast<int>(x), static_cast<int>(y));
}

void AssetsBrowserController::SyncTreeToPolyhavenCategory(const std::string& type, const std::string& category)
{
    if (!m_Tree || !m_TreeProvider || !m_TreeSelection)
        return;

    // Expand Online Assets → Polyhaven.
    m_Tree->SetExpanded(AssetsTreeDataProvider::kOnlineAssetsSectionId, true);
    TreeId phId = AssetsTreeDataProvider::kPolyhavenId;
    m_Tree->SetExpanded(phId, true);

    // Find and expand the type node (e.g., "Models").
    int typeCount = m_TreeProvider->GetChildCount(phId);
    for (int i = 0; i < typeCount; ++i)
    {
        TreeId typeId = m_TreeProvider->GetChildId(phId, i);
        if (m_TreeProvider->GetPolyhavenCategory(typeId) != type)
            continue;
        m_Tree->SetExpanded(typeId, true);

        // Find the category child node (e.g., "Furniture").
        int catCount = m_TreeProvider->GetChildCount(typeId);
        for (int c = 0; c < catCount; ++c)
        {
            TreeId catId = m_TreeProvider->GetChildId(typeId, c);
            std::string catType, catSlug;
            m_TreeProvider->GetPolyhavenTypeAndCategory(catId, catType, catSlug);
            if (catSlug == category)
            {
                m_TreeSelection->SetSingle(catId);
                m_Tree->SyncSelectionVisuals();
                return;
            }
        }

        // Category not found (e.g., "All") — select the type node itself.
        m_TreeSelection->SetSingle(typeId);
        m_Tree->SyncSelectionVisuals();
        return;
    }
}

void AssetsBrowserController::SelectPolyhavenCategoryFilter(const std::string& type, const std::string& categoryFilter)
{
    if (type.empty() || !m_GridProvider)
        return;
    m_PolyhavenManifest.clear();
    m_PolyhavenManifestFetchedEnd = 0;
    m_PolyhavenTypeKey.clear();
    m_PolyhavenLoadMoreInProgress = false;
    m_Polyhaven->ClearDownloadCache();

    m_GridProvider->SetSearchQuery("");
    std::string typeKeyPlaceholder = PolyhavenService::TypeKeyForType(type);
    m_GridProvider->LoadRemoteItems({}, typeKeyPlaceholder);
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List && m_ListProvider)
        m_List->RefreshFromProvider();

    const uint32_t loadGen = ++m_PolyhavenLoadGeneration;
    std::atomic<uint32_t>* pLoadGeneration = &m_PolyhavenLoadGeneration;
    AssetsGridDataProvider* gridProvider = m_GridProvider.get();
    GridView* grid = m_Grid;
    ListView* list = m_List;
    AssetsListDataProvider* listProvider = m_ListProvider.get();
    UIElement* postAnchor = m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_Grid);
    if (!postAnchor)
        return;
    const UI::UiPostHandle post = postAnchor->GetPostHandle();
    AssetsBrowserController* controller = this;
    auto alive = m_Alive;

    SubmitPolyhavenTransfer(m_Context, [type, categoryFilter, loadGen, pLoadGeneration, gridProvider, grid, list, listProvider, post, controller, alive]()
    {
        auto manifestResult = PolyhavenService::FetchManifest(type, categoryFilter);
        post.Post([manifestResult, loadGen, pLoadGeneration, gridProvider, grid, list, listProvider, post, controller, alive]()
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            if (!pLoadGeneration || pLoadGeneration->load(std::memory_order_relaxed) != loadGen || !gridProvider)
                return;
            controller->m_PolyhavenManifest = manifestResult.manifest;
            controller->m_PolyhavenTypeKey = manifestResult.typeKey;
            controller->m_PolyhavenManifestFetchedEnd = 0;

            if (controller->m_PolyhavenManifest.empty())
            {
                gridProvider->LoadRemoteItems({}, controller->m_PolyhavenTypeKey);
                if (grid)
                    grid->RefreshFromProvider();
                if (list && listProvider)
                    list->RefreshFromProvider();
                return;
            }

            const size_t firstBatchEnd = std::min(kPolyhavenBatchSize, controller->m_PolyhavenManifest.size());
            controller->m_PolyhavenLoadMoreInProgress = true;
            // The worker gets its own copy of the batch: the controller's manifest is
            // reassigned on the UI thread by the next category selection, and the
            // controller may be gone before the download finishes.
            std::vector<PolyhavenManifestEntry> firstBatch(
                controller->m_PolyhavenManifest.begin(),
                controller->m_PolyhavenManifest.begin() + static_cast<std::ptrdiff_t>(firstBatchEnd));
            SubmitPolyhavenTransfer(controller->m_Context, [controller, loadGen, firstBatchEnd, gridProvider, grid, list, listProvider, post, pLoadGeneration, alive,
                                                                  firstBatch = std::move(firstBatch), typeKey = controller->m_PolyhavenTypeKey]()
            {
                PolyhavenLoadResult batch = PolyhavenService::MaterializeBatch(firstBatch, 0, firstBatch.size(), typeKey);
                post.Post([batch, loadGen, firstBatchEnd, controller, gridProvider, grid, list, listProvider, pLoadGeneration, alive]()
                {
                    if (!alive->load(std::memory_order_acquire))
                        return;
                    if (!pLoadGeneration || pLoadGeneration->load(std::memory_order_relaxed) != loadGen || !gridProvider)
                        return;
                    controller->m_PolyhavenLoadMoreInProgress = false;
                    controller->m_PolyhavenManifestFetchedEnd = firstBatchEnd;
                    gridProvider->LoadRemoteItems(batch.pathAndLabels, batch.typeKey);
                    if (grid)
                        grid->RefreshFromProvider();
                    if (list && listProvider)
                        list->RefreshFromProvider();
                    controller->DeferThumbnailsAfterNavigation();
                    if (controller->m_PolyhavenManifestFetchedEnd < controller->m_PolyhavenManifest.size())
                        controller->StartNextPolyhavenBatch();
                });
            });
        });
    });
}

void AssetsBrowserController::NavigateToAndSelectAsset(const std::filesystem::path& assetPath,
                                                       std::function<void()> onSelected)
{
    if (assetPath.empty() || !m_GridProvider)
        return;

    NavigateToAndSelectAssetImpl(assetPath, /*silent=*/false, std::move(onSelected));
}

void AssetsBrowserController::NavigateToAndSelectAssetSilent(const std::filesystem::path& assetPath)
{
    if (assetPath.empty() || !m_GridProvider)
        return;
    NavigateToAndSelectAssetImpl(assetPath, /*silent=*/true);
}

void AssetsBrowserController::NavigateToFolderSilent(const std::filesystem::path& folderPath)
{
    if (folderPath.empty() || !m_GridProvider)
        return;

    // See NavigateToAndSelectAssetImpl: the shared guard keeps suppression alive
    // until every captured PostAction (dir load + tree sync + later refreshes)
    // has run, so deferred OnChanged fires from grid/list selection mutations
    // cannot leak m_OnSelectAssets back into the inspector history recorder.
    auto silentGuard = HoldInspectorAssetFireSuppression();

    NavigateToDirectory(folderPath, [silentGuard]()
    {
        // Release the captured guard at navigation completion. Any further
        // PostActions queued during NavigateToDirectory that captured the same
        // guard will hold suppression alive until they too have run.
        (void)silentGuard;
    });
}

void AssetsBrowserController::NavigateToAndSelectAssetImpl(const std::filesystem::path& assetPath,
                                                           bool silent,
                                                           std::function<void()> onSelected)
{
    std::filesystem::path assetDir = assetPath.parent_path();

    // Shared suppression token: the suppression depth stays non-zero while any
    // PostAction (grid + list) that touches the shared selection model is still
    // pending. Its holder releases only when ALL captured copies of `silentGuard`
    // are destroyed. Without this, the list PostAction's SetSelectedIndex could
    // fire OnChanged on the shared m_ItemSelection after an earlier navigation
    // had already released its holder, leaking an unwanted m_OnSelectAssets fire.
    std::shared_ptr<void> silentGuard;
    if (silent)
    {
        silentGuard = HoldInspectorAssetFireSuppression();
    }

    NavigateToDirectory(assetDir, [this, assetPath, silentGuard, onSelected = std::move(onSelected)]() mutable
    {
        UIElement* anchor = m_Grid ? static_cast<UIElement*>(m_Grid)
                                   : static_cast<UIElement*>(m_List);
        if (!anchor)
            return;
        // Directory loading and virtualized view refreshes are posted. Select on
        // the following UI turn so the new provider contents are available.
        anchor->PostAction([this, assetPath, silentGuard, onSelected = std::move(onSelected)]() mutable
        {
            SelectAssetAfterNavigation(assetPath, silentGuard, 3, std::move(onSelected));
        });
    });
}

void AssetsBrowserController::SelectAssetAfterNavigation(const std::filesystem::path& assetPath,
                                                          std::shared_ptr<void> silentGuard,
                                                          int attemptsRemaining,
                                                          std::function<void()> onSelected)
{
    int gridIndex = -1;
    int listIndex = -1;
    const std::string assetKey = AssetPathKey(assetPath);

    if (m_Grid && m_GridProvider)
    {
        const int count = m_GridProvider->GetItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (PathsReferToSameEntry(m_GridProvider->GetPath(m_GridProvider->GetItemId(i)), assetKey))
            {
                gridIndex = i;
                break;
            }
        }
    }
    if (m_List && m_ListProvider)
    {
        const int count = m_ListProvider->GetItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (PathsReferToSameEntry(m_ListProvider->GetPath(m_ListProvider->GetItemId(i)), assetKey))
            {
                listIndex = i;
                break;
            }
        }
    }

    if (gridIndex >= 0 || listIndex >= 0)
    {
        if (gridIndex >= 0)
            m_Grid->SetSelectedIndex(gridIndex, true);
        if (listIndex >= 0)
            m_List->SetSelectedIndex(listIndex, true);

        // Grid and list share one selection model; SetSelectedIndex drives the
        // normal centralized selection notification after visual selection.
        if (onSelected)
            onSelected();
        return;
    }

    if (attemptsRemaining <= 0)
        return;
    UIElement* anchor = m_Grid ? static_cast<UIElement*>(m_Grid)
                               : static_cast<UIElement*>(m_List);
    if (anchor)
    {
        anchor->PostAction([this, assetPath, silentGuard, attemptsRemaining,
                            onSelected = std::move(onSelected)]() mutable
        {
            SelectAssetAfterNavigation(assetPath, silentGuard, attemptsRemaining - 1,
                                       std::move(onSelected));
        });
    }
}

std::filesystem::path AssetsBrowserController::GetPrimarySelectedAssetPath() const
{
    if (!m_ItemSelection || !m_GridProvider)
        return {};

    const std::vector<UI::Interaction::ItemId> ids = m_ItemSelection->GetSelection();
    if (ids.size() != 1)
        return {};

    const std::filesystem::path path = m_GridProvider->GetPath(static_cast<GridId>(ids.front()));
    if (path.empty() || IsAssetBrowserVirtualPath(path))
        return {};

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return {};

    return path;
}

void AssetsBrowserController::FirePendingInspectorSelect()
{
    if (!m_PendingInspectorSelectPaths.has_value())
        return;
    auto paths = std::move(*m_PendingInspectorSelectPaths);
    m_PendingInspectorSelectPaths.reset();
    m_DragObservedDuringPress = false;
    if (m_OnSelectAssets)
        m_OnSelectAssets(paths);
}

void AssetsBrowserController::ClearAssetItemSelectionForDatasetSwitch()
{
    m_PendingInspectorSelectPaths.reset();
    m_DragObservedDuringPress = false;

    if (!m_ItemSelection)
        return;

    auto suppressionGuard = HoldInspectorAssetFireSuppression();
    m_ItemSelection->Clear();
}

std::shared_ptr<void> AssetsBrowserController::HoldInspectorAssetFireSuppression()
{
    ++m_InspectorAssetFireSuppressionDepth;
    auto alive = m_Alive;
    return std::shared_ptr<void>(nullptr, [this, alive](void*)
    {
        if (alive && alive->load() && m_InspectorAssetFireSuppressionDepth != 0)
            --m_InspectorAssetFireSuppressionDepth;
    });
}

void AssetsBrowserController::CheckPendingInspectorSelect()
{
    if (!m_PendingInspectorSelectPaths.has_value())
        return;

    UIElement* anchor = m_Grid ? static_cast<UIElement*>(m_Grid)
                               : (m_List ? static_cast<UIElement*>(m_List) : nullptr);
    UIManager* ui = anchor ? anchor->GetOwnerManager() : nullptr;

    if (ui)
    {
        if (auto* dd = ui->GetDragDropManager(); dd && dd->IsDragging())
            m_DragObservedDuringPress = true;
    }

    if (m_DragObservedDuringPress)
    {
        m_PendingInspectorSelectPaths.reset();
        m_DragObservedDuringPress = false;
        return;
    }

    if (ui && ui->IsMouseDown())
    {
        if (anchor)
            anchor->PostAction([this]() { CheckPendingInspectorSelect(); });
        return;
    }

    FirePendingInspectorSelect();
}

void AssetsBrowserController::SetSelectionFromPaths(const std::vector<std::filesystem::path>& paths)
{
    if (!m_GridProvider || !m_ItemSelection)
        return;

    if (paths.empty())
    {
        m_ItemSelection->Clear();
        return;
    }

    // Build a mapping from path -> item id for the current grid dataset.
    std::unordered_map<std::filesystem::path, UI::Interaction::ItemId> pathToId;
    const int count = m_GridProvider->GetItemCount();
    pathToId.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        GridId gid = m_GridProvider->GetItemId(i);
        if (gid == 0)
            continue;
        std::filesystem::path p = m_GridProvider->GetPath(gid);
        pathToId.emplace(std::move(p), static_cast<UI::Interaction::ItemId>(gid));
    }

    std::vector<UI::Interaction::ItemId> ids;
    ids.reserve(paths.size());
    for (const auto& p : paths)
    {
        auto it = pathToId.find(p);
        if (it != pathToId.end())
            ids.push_back(it->second);
    }

    if (ids.empty())
    {
        m_ItemSelection->Clear();
        return;
    }

    UI::Interaction::ItemId anchor = ids.front();
    m_ItemSelection->SetSelection(ids, anchor);

    // Force views to rebind so their visual selection (highlight/outline) matches
    // the updated selection model.
    if (m_Grid)
        m_Grid->RefreshFromProvider();
    if (m_List && m_ListProvider)
        m_List->RefreshFromProvider();
}

void AssetsBrowserController::SetTruncationThreshold(float threshold)
{
    m_TruncationThreshold = threshold;
    m_TruncationThresholdOverride = true;

    // Refresh grid to show updated truncation
    if (m_Grid && m_GridProvider)
    {
        m_Grid->RefreshFromProvider();
    }
}

void AssetsBrowserController::SetTruncationEnabled(bool enabled)
{
    m_TruncationEnabled = enabled;
    m_TruncationEnabledOverride = true;

    // Refresh visible asset rows to show updated truncation.
    if (m_Grid && m_GridProvider)
    {
        m_Grid->RefreshFromProvider();
    }
    if (m_List && m_ListProvider)
    {
        m_List->RefreshFromProvider();
    }
}

void AssetsBrowserController::StartNextPolyhavenBatch()
{
    if (m_PolyhavenManifest.empty() || m_PolyhavenLoadMoreInProgress.load(std::memory_order_relaxed))
        return;
    if (m_PolyhavenManifestFetchedEnd >= m_PolyhavenManifest.size())
        return;

    m_PolyhavenLoadMoreInProgress = true;
    const uint32_t loadGen = m_PolyhavenLoadGeneration.load(std::memory_order_relaxed);
    const size_t start = m_PolyhavenManifestFetchedEnd;
    constexpr size_t kBatchSize = 50;
    const size_t end = std::min(start + kBatchSize, m_PolyhavenManifest.size());
    std::vector<PolyhavenManifestEntry> manifestCopy = m_PolyhavenManifest;
    std::string typeKeyCopy = m_PolyhavenTypeKey;
    AssetsGridDataProvider* gridProvider = m_GridProvider.get();
    GridView* grid = m_Grid;
    ListView* list = m_List;
    AssetsListDataProvider* listProvider = m_ListProvider.get();
    UIElement* postAnchor = m_Tree ? static_cast<UIElement*>(m_Tree) : static_cast<UIElement*>(m_Grid);
    if (!postAnchor)
        return;
    const UI::UiPostHandle post = postAnchor->GetPostHandle();
    auto alive = m_Alive;
    SubmitPolyhavenTransfer(m_Context, [this, loadGen, start, end, manifestCopy = std::move(manifestCopy), typeKeyCopy, gridProvider, grid, list, listProvider, post, alive]()
    {
        PolyhavenLoadResult batch = PolyhavenService::MaterializeBatch(manifestCopy, start, end, typeKeyCopy);
        post.Post([this, batch, loadGen, end, gridProvider, grid, list, listProvider, alive]()
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            if (m_PolyhavenLoadGeneration.load(std::memory_order_relaxed) != loadGen || !gridProvider)
                return;
            m_PolyhavenLoadMoreInProgress = false;
            m_PolyhavenManifestFetchedEnd = end;
            gridProvider->AppendRemoteItems(batch.pathAndLabels, batch.typeKey);
            if (grid)
                grid->RefreshFromProvider();
            if (list && listProvider)
                list->RefreshFromProvider();
            DeferThumbnailsAfterNavigation();
            if (m_PolyhavenManifestFetchedEnd < m_PolyhavenManifest.size())
                StartNextPolyhavenBatch();
        });
    });
}

void AssetsBrowserController::TryLoadMorePolyhavenBatch(float scrollY, float contentHeight, float viewportHeight)
{
    if (m_PolyhavenManifest.empty() || m_PolyhavenLoadMoreInProgress.load(std::memory_order_relaxed))
        return;
    if (m_PolyhavenManifestFetchedEnd >= m_PolyhavenManifest.size())
        return;
    if (viewportHeight <= 0)
        return;
    // Load more when: near bottom (within 200px), content fits in view, user scrolled at all, or at/near max scroll
    const float thresholdPx = 200.f;
    const bool nearBottom = (contentHeight > 0.f && (scrollY + viewportHeight >= contentHeight - thresholdPx));
    const bool contentFitsInView = (contentHeight > 0.f && contentHeight <= viewportHeight + thresholdPx);
    const bool userScrolled = (scrollY > 0.5f);
    const float maxScrollY = std::max(0.f, contentHeight - viewportHeight);
    const bool atMaxScroll = (contentHeight > 0.f && viewportHeight > 0.f && scrollY >= maxScrollY - 0.5f);
    if (!nearBottom && !contentFitsInView && !userScrolled && !atMaxScroll)
        return;

    StartNextPolyhavenBatch();
}

} // namespace GameEngine
