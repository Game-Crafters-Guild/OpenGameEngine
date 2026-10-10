#include "VersionControl/SceneDiff.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSFileStatus.h"

#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace GameEngine::Editor
{
namespace
{
// Reading a base revision costs a provider process, while the editor asks for
// the diff on every selection change and every property commit. Memoize on what
// actually invalidates a result: the working file's identity (size + write
// time) and, via InvalidateVersionControlledSceneDiffCache, provider status
// changes that can move the base revision under an unchanged file.
struct CachedSceneDiff
{
    std::uintmax_t Size = 0;
    std::filesystem::file_time_type WriteTime{};
    // The file status this entry was built under. A commit leaves the working
    // file untouched but moves the base revision, and it is exactly what turns
    // Modified into Clean — so a status change is the signal that a memo whose
    // file identity still matches has nevertheless gone stale.
    VCSFileStatus Status = VCSFileStatus::Clean;
    std::vector<SceneObjectDiff> Objects;
};

std::mutex g_CacheMutex;
std::unordered_map<std::string, CachedSceneDiff> g_Cache;

bool TryGetFileIdentity(const std::filesystem::path& path, std::uintmax_t& outSize,
                        std::filesystem::file_time_type& outWriteTime)
{
    std::error_code ec;
    outSize = std::filesystem::file_size(path, ec);
    if (ec)
        return false;
    outWriteTime = std::filesystem::last_write_time(path, ec);
    return !ec;
}

bool TryGetCurrentStatus(const std::filesystem::path& scenePath, VCSFileStatus& outStatus)
{
    EditorVcsProviderDescriptor provider;
    if (!EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) ||
        !provider.Integration)
        return false;
    outStatus = provider.Integration().GetFileStatus(scenePath);
    return true;
}

std::vector<SceneObjectDiff> BuildFromProvider(const std::filesystem::path& scenePath)
{
    EditorVcsProviderDescriptor provider;
    if (!EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) ||
        !provider.Integration)
        return {};

    IVCSIntegration& integration = provider.Integration();
    const VCSFileStatus status = integration.GetFileStatus(scenePath);
    if (status == VCSFileStatus::Ignored || status == VCSFileStatus::NotConfigured)
        return {};

    std::ifstream file(scenePath, std::ios::binary);
    if (!file.is_open())
        return {};
    std::ostringstream current;
    current << file.rdbuf();
    // Added/unversioned files have an empty baseline for every provider, even
    // one whose CLI has no committed-content primitive.
    // Tracked files still require the provider-neutral baseline capability;
    // guessing here would turn an unavailable base into a false full-file add.
    const bool hasEmptyBaseline = status == VCSFileStatus::Added ||
                                  status == VCSFileStatus::Unversioned;
    if (!hasEmptyBaseline && !provider.GetBaseContent)
        return {};
    const std::string original = hasEmptyBaseline ? std::string{}
                                                   : provider.GetBaseContent(scenePath);
    if (original.empty() && !hasEmptyBaseline)
        return {};
    return BuildSceneDiff(original, current.str());
}
} // namespace

std::vector<SceneObjectDiff> LoadVersionControlledSceneDiff(const std::filesystem::path& scenePath)
{
    if (scenePath.empty() || scenePath.extension() != ".scene")
        return {};

    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
    const bool hasIdentity = TryGetFileIdentity(scenePath, size, writeTime);
    const std::string key = scenePath.generic_string();

    if (hasIdentity)
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        const auto it = g_Cache.find(key);
        if (it != g_Cache.end() && it->second.Size == size &&
            it->second.WriteTime == writeTime)
            return it->second.Objects;
    }

    std::vector<SceneObjectDiff> objects = BuildFromProvider(scenePath);
    // An empty result means "no provider, no baseline, or not tracked" — an
    // answer that can change without the file changing, so it is never cached.
    // A scene that really is under version control always yields its objects.
    if (!hasIdentity || objects.empty())
        return objects;

    VCSFileStatus status = VCSFileStatus::Clean;
    (void)TryGetCurrentStatus(scenePath, status);

    std::lock_guard<std::mutex> lock(g_CacheMutex);
    CachedSceneDiff& entry = g_Cache[key];
    entry.Size = size;
    entry.WriteTime = writeTime;
    entry.Status = status;
    entry.Objects = objects;
    return objects;
}

void InvalidateVersionControlledSceneDiffCache()
{
    std::lock_guard<std::mutex> lock(g_CacheMutex);
    g_Cache.clear();
}

bool InvalidateVersionControlledSceneDiffsWhoseStatusChanged()
{
    // Status comes from the provider's cached snapshot, so an idle poll — the
    // common case, since the broadcast fires whether or not anything changed —
    // costs no process here and no refresh downstream.
    std::vector<std::pair<std::string, VCSFileStatus>> current;
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        current.reserve(g_Cache.size());
        for (const auto& [path, entry] : g_Cache)
            current.emplace_back(path, entry.Status);
    }

    std::vector<std::string> stale;
    for (const auto& [path, cachedStatus] : current)
    {
        VCSFileStatus status = VCSFileStatus::Clean;
        if (TryGetCurrentStatus(path, status) && status != cachedStatus)
            stale.push_back(path);
    }
    if (stale.empty())
        return false;

    std::lock_guard<std::mutex> lock(g_CacheMutex);
    for (const std::string& path : stale)
        g_Cache.erase(path);
    return true;
}

} // namespace GameEngine::Editor
