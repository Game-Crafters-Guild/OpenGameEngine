#include "Assets/Packages/PackageNativeCache.h"
#include "Assets/Packages/PackageGitSource.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/ScopedFileLock.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using FileSystem::ScopedFileLock;
namespace fs = std::filesystem;
constexpr size_t kRetainedGenerations = 4;
constexpr size_t kHashLength = 12;
constexpr const char* kCacheRootRecord = "cache-root";
constexpr const char* kScopeLock = "scope";
// Bounded metadata read, with room for the UTF-8 form of a maximum-length native path.
constexpr std::streamoff kMaxCacheRootBytes = 128 * 1024;

bool IsHash(const std::string& name)
{
    if (name.size() != kHashLength)
        return false;
    for (const char c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool IsPackageScopeName(const std::string& name)
{
    const auto separator = name.rfind('-');
    if (separator == std::string::npos || separator == 0 || separator > 8 ||
        !IsHash(name.substr(separator + 1)))
        return false;
    return name.find_first_of("/\\") == std::string::npos;
}

bool IsPlainDirectory(const fs::path& path)
{
    std::error_code error;
    return fs::symlink_status(path, error).type() == fs::file_type::directory && !error;
}

bool IsPlainFileOrMissing(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    return status.type() == fs::file_type::not_found ||
           (!error && status.type() == fs::file_type::regular);
}

std::optional<fs::path> ReadCacheRoot(const fs::path& record)
{
    if (!IsPlainFileOrMissing(record))
        return std::nullopt;
    std::ifstream input(record, std::ios::binary);
    if (!input)
        return std::nullopt;
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > kMaxCacheRootBytes)
        return std::nullopt;
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(length), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() || bytes.find('\0') != std::string::npos)
        return std::nullopt;
    try
    {
        const fs::path root(std::u8string(bytes.begin(), bytes.end()));
        if (root.is_absolute() && root == root.lexically_normal())
            return root;
    }
    catch (const fs::filesystem_error&)
    {
        // A damaged ownership record cannot authorize eviction.
    }
    return std::nullopt;
}

bool RecordCacheRoot(const fs::path& packageRoot, const fs::path& cacheRoot)
{
    const auto record = packageRoot / kCacheRootRecord;
    if (!IsPlainFileOrMissing(record))
        return false;
    std::error_code error;
    if (fs::exists(record, error))
        return ReadCacheRoot(record) == cacheRoot;
    if (error)
        return false;

    const auto temporary = FileSystem::MakeTemporarySiblingPath(record);
    const auto bytes = cacheRoot.generic_u8string();
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output)
    {
        fs::remove(temporary, error);
        return false;
    }
    // Concurrent acquisitions publish the same immutable scope identity. A
    // reader sees a complete record, including while another process starts.
    return FileSystem::PublishFile(temporary, record);
}

struct CacheEntry
{
    fs::path Path;
    fs::file_time_type LastUsed;
};

bool MoreRecent(const CacheEntry& left, const CacheEntry& right)
{
    if (left.LastUsed != right.LastUsed)
        return left.LastUsed > right.LastUsed;
    return left.Path < right.Path;
}

void RemoveInactiveGeneration(const fs::path& candidate, const fs::path& lockRoot)
{
    if (!IsHash(candidate.filename().string()) || !IsPlainDirectory(candidate) ||
        !IsPlainFileOrMissing(lockRoot / candidate.filename()))
        return;
    ScopedFileLock eviction(lockRoot / candidate.filename(), ScopedFileLock::Mode::Exclusive, false);
    if (!eviction.IsLocked() || !IsPlainDirectory(candidate))
        return;
    std::error_code error;
    fs::remove_all(candidate, error);
    if (error)
        Logger::Log::Warning("[Packages] cannot prune native cache '{}': {}", candidate.string(), error.message());
    // Sidecars stay in place: unlinking a lock inode would let two processes lock different files.
}

void Prune(const fs::path& packageRoot, const fs::path& lockRoot, const fs::path& current)
{
    std::error_code error;
    std::vector<CacheEntry> entries;
    for (fs::directory_iterator it(packageRoot, error), end; !error && it != end; it.increment(error))
    {
        const auto candidate = it->path();
        const auto candidateName = candidate.filename().string();
        if (candidate == current || !IsHash(candidateName) || !IsPlainDirectory(candidate))
            continue;
        const auto lastUsed = fs::last_write_time(lockRoot / candidateName, error);
        if (error)
        {
            error.clear();
            continue;
        }
        entries.push_back({candidate, lastUsed});
    }
    if (error)
        return;
    std::sort(entries.begin(), entries.end(), MoreRecent);
    for (size_t index = kRetainedGenerations - 1; index < entries.size(); ++index)
        RemoveInactiveGeneration(entries[index].Path, lockRoot);
}

void PruneOrphanedScope(const fs::path& packageRoot, const fs::path& lockRoot)
{
    if (!IsPlainDirectory(packageRoot) || !IsPlainDirectory(lockRoot) ||
        !IsPlainFileOrMissing(lockRoot / kScopeLock))
        return;
    // Construction holds this lock shared until its entry and root record
    // exist. An empty scope cannot disappear underneath a new acquisition.
    ScopedFileLock scope(lockRoot / kScopeLock, ScopedFileLock::Mode::Exclusive, false);
    if (!scope.IsLocked())
        return;
    const auto cacheRoot = ReadCacheRoot(packageRoot / kCacheRootRecord);
    std::error_code error;
    if (!cacheRoot || fs::exists(*cacheRoot, error) || error)
        return;

    for (fs::directory_iterator it(packageRoot, error), end; !error && it != end; it.increment(error))
        RemoveInactiveGeneration(it->path(), lockRoot);
    if (error)
        return;
    // A leased generation or an unrecognized file keeps the scope intact.
    for (fs::directory_iterator it(packageRoot, error), end; !error && it != end; it.increment(error))
        if (it->path().filename() != kCacheRootRecord)
            return;
    if (error)
        return;
    fs::remove(packageRoot / kCacheRootRecord, error);
    if (!error)
        fs::remove(packageRoot, error);
    if (error)
        Logger::Log::Warning("[Packages] cannot remove empty native cache scope '{}': {}",
                             packageRoot.string(), error.message());
}

void PruneOrphanedScopes(const fs::path& root, const fs::path& currentPackageRoot)
{
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        const auto scope = it->path();
        if (scope != currentPackageRoot && IsPackageScopeName(scope.filename().string()))
            PruneOrphanedScope(scope, root / ".locks" / scope.filename());
    }
}
}

std::unique_ptr<FileSystem::ScopedFileLock> AcquirePackageNativeCache(
    const std::filesystem::path& entry, const std::filesystem::path& cacheRoot)
{
#if defined(__EMSCRIPTEN__)
    return {};
#else
    if (!entry.is_absolute() || entry != entry.lexically_normal() || cacheRoot.empty() ||
        entry.parent_path().parent_path().filename() != ".native" ||
        !IsPackageScopeName(entry.parent_path().filename().string()) ||
        !IsHash(entry.filename().string()))
        return {};
    const auto packageRoot = entry.parent_path();
    const auto root = packageRoot.parent_path();
    const auto lockRoot = root / ".locks" / packageRoot.filename();
    std::error_code error;
    const auto normalizedCacheRoot = fs::absolute(cacheRoot, error).lexically_normal();
    if (error)
        return {};
    fs::create_directories(root, error);
    if (error || !IsPlainDirectory(root))
        return {};
    fs::create_directories(lockRoot, error);
    if (error || !IsPlainDirectory(root / ".locks") || !IsPlainDirectory(lockRoot) ||
        !IsPlainFileOrMissing(root / ".locks" / "maintenance") ||
        !IsPlainFileOrMissing(lockRoot / kScopeLock) || !IsPlainFileOrMissing(lockRoot / entry.filename()))
        return {};
    std::unique_ptr<ScopedFileLock> lease;
    {
        ScopedFileLock construction(lockRoot / kScopeLock, ScopedFileLock::Mode::Shared, false);
        if (!construction.IsLocked())
            return {};
        fs::create_directories(normalizedCacheRoot, error);
        if (error)
            return {};
        fs::create_directories(packageRoot, error);
        if (error || !IsPlainDirectory(packageRoot))
            return {};
        // Lease first: an evictor holds this same sidecar exclusively through
        // remove_all, so a cache entry is safe even when maintenance is busy.
        lease = std::make_unique<ScopedFileLock>(lockRoot / entry.filename(),
                                                 ScopedFileLock::Mode::Shared, false);
        if (!lease->IsLocked())
            return {};
        fs::create_directories(entry, error);
        if (error || !IsPlainDirectory(entry) || !RecordCacheRoot(packageRoot, normalizedCacheRoot))
            return {};
        fs::last_write_time(lockRoot / entry.filename(), fs::file_time_type::clock::now(), error);
        if (error)
            return {};
    }

    // Maintenance coordinates recency and eviction only. Do not block editor
    // startup behind a paused peer: retain the safe lease and skip this pass.
    ScopedFileLock maintenance(root / ".locks" / "maintenance", ScopedFileLock::Mode::Exclusive, false);
    if (!maintenance.IsLocked())
    {
        Logger::Log::Warning("[Packages] native-cache maintenance is busy; skipping retention for '{}'",
                             packageRoot.filename().string());
        return lease;
    }
    Prune(packageRoot, lockRoot, entry);
    PruneOrphanedScopes(root, packageRoot);
    return lease;
#endif
}

bool RetainPackageNativeCache(const std::filesystem::path& entry)
{
    if (entry.empty())
        return false;
    struct ProcessLeases
    {
        std::mutex Mutex;
        std::map<std::filesystem::path, std::unique_ptr<FileSystem::ScopedFileLock>> Entries;
    };
    // Intentionally process-lifetime: static destruction must not release a lease before
    // another singleton finishes unloading module images. The OS closes these handles at exit.
    static auto* const s_Leases = new ProcessLeases;
    std::lock_guard guard(s_Leases->Mutex);
    if (s_Leases->Entries.contains(entry))
        return true;
    auto lease = AcquirePackageNativeCache(entry, GlobalPackageCacheRoot());
    if (!lease)
        return false;
    s_Leases->Entries.emplace(entry, std::move(lease));
    return true;
}
}
