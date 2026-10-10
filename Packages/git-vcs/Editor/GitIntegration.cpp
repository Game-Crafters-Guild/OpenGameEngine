#include "GitIntegration.h"
#include "GitCommandExecutor.h"
#include "VCSIntegration/VCSPathKey.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace GameEngine
{
GitIntegration& GitIntegration::GetInstance()
{
    static GitIntegration instance;
    return instance;
}

bool GitIntegration::Initialize(const std::filesystem::path& projectRoot, 
                                 const std::filesystem::path& gitExecutable)
{
    if (m_IsRepository)
    {
        return true; // Already initialized
    }

    try
    {
        m_ProjectRoot = projectRoot;

        // Use provided git executable or find it
        if (!gitExecutable.empty() && std::filesystem::exists(gitExecutable))
        {
            m_GitExecutable = gitExecutable;
        }
        else
        {
            m_GitExecutable = GitCommandExecutor::FindGitExecutable();
        }

        if (m_GitExecutable.empty())
        {
            Logger::Log::Warning("Git executable not found. Git integration disabled.");
            m_IsAvailable = false;
            return false;
        }

        m_IsAvailable = true;

        // Detect git repository
        if (!DetectGitRepository(projectRoot))
        {
            Logger::Log::Info("No git repository detected. Git integration disabled.");
            return false;
        }

        m_IsRepository = true;
#if defined(__APPLE__)
        m_StatusCacheSlots[0] = std::make_shared<StatusCache>();
        m_StatusCacheSlotIndex.store(0, std::memory_order_release);
#else
        m_StatusCacheSnapshot.store(std::make_shared<StatusCache>(), std::memory_order_release);
#endif
        Logger::Log::Info("Git integration initialized. Repository: {}", m_RepositoryRoot.string());

        // Start status tracking (non-blocking): one poll at once, then one per interval.
        m_StatusPoller.Start("Git Status", [this] { PollStatus(); });

        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Git integration initialization exception: {}", e.what());
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
    catch (...)
    {
        Logger::Log::Warning("Git integration initialization failed with unknown error");
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
}

void GitIntegration::Shutdown()
{
    // The poll thread is joined before the callback is cleared, so no poll
    // calls into a listener after Shutdown returns.
    m_IsRepository = false;
    m_StatusPoller.Stop();
    m_StatusChangedCallback = nullptr;
    
    // Clear all state to allow proper reinitialization
    m_IsAvailable = false;
    m_RepositoryRoot.clear();
    m_ProjectRoot.clear();
    
    // Clear status cache snapshot (reader may still hold a copy; that's fine).
#if defined(__APPLE__)
    m_StatusCacheSlots[0].reset();
    m_StatusCacheSlots[1].reset();
#else
    m_StatusCacheSnapshot.store({}, std::memory_order_release);
#endif
}

bool GitIntegration::DetectGitRepository(const std::filesystem::path& projectRoot)
{
    try
    {
        std::filesystem::path current = projectRoot;
        
        while (!current.empty() && current != current.root_path())
        {
            std::error_code ec;
            std::filesystem::path gitDir = current / ".git";
            if (std::filesystem::exists(gitDir, ec) && std::filesystem::is_directory(gitDir, ec))
            {
                m_RepositoryRoot = current;
                return true;
            }
            
            current = current.parent_path();
        }
    }
    catch (...)
    {
        // Silently fail - not a git repository
    }

    return false;
}

VCSFileStatus GitIntegration::GetFileStatus(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return VCSFileStatus::Unversioned;
    }

    // Lexical: this runs per newly visible Assets-panel cell, so no syscalls.
    const std::filesystem::path relPath = VCS::RepoRelative(filePath, m_RepositoryRoot);
    if (relPath.empty())
    {
        return VCSFileStatus::Unversioned;
    }

    std::string key = relPath.generic_string();
    std::replace(key.begin(), key.end(), '\\', '/');

#if defined(__APPLE__)
    const size_t idx = m_StatusCacheSlotIndex.load(std::memory_order_acquire);
    std::shared_ptr<const StatusCache> snapshot = m_StatusCacheSlots[idx];
#else
    std::shared_ptr<const StatusCache> snapshot = m_StatusCacheSnapshot.load(std::memory_order_acquire);
#endif
    if (!snapshot)
        return VCSFileStatus::Clean;

    const auto* entry = FindInSnapshot(*snapshot, key);
    if (entry)
    {
        return entry->second;
    }

    // Cache miss - return Clean immediately to avoid blocking the caller (often UI thread).
    // The background status thread periodically refreshes the cache.
    return VCSFileStatus::Clean;
}

bool GitIntegration::Add(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    std::vector<std::string> args = {"add", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool GitIntegration::Remove(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    std::vector<std::string> args = {"rm", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool GitIntegration::Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relOldPath = std::filesystem::relative(oldPath, m_RepositoryRoot);
    std::filesystem::path relNewPath = std::filesystem::relative(newPath, m_RepositoryRoot);

    std::vector<std::string> args = {"mv", relOldPath.string(), relNewPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(oldPath);
        RefreshStatus(newPath);
    }

    return result.success;
}

bool GitIntegration::Commit(const std::string& message)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"commit", "-m", message};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool GitIntegration::Push()
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"push"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    return result.success;
}

bool GitIntegration::Update()
{
    // IVCSIntegration::Update() maps to Pull() for Git
    return Pull();
}

bool GitIntegration::Pull()
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"pull"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool GitIntegration::Revert(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    std::vector<std::string> args = {"checkout", "--", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool GitIntegration::Fetch()
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"fetch"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool GitIntegration::OpenDiff(const std::filesystem::path& filePath,
                               const std::filesystem::path& externalDiffTool)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            Logger::Log::Warning("OpenDiff: Failed to get relative path for {}", filePath.string());
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    if (!externalDiffTool.empty() && std::filesystem::exists(externalDiffTool))
    {
        // Use external diff tool directly
        // First, get the original file content from git
        std::vector<std::string> showArgs = {"show", "HEAD:" + relPath.generic_string()};
        auto showResult = GitCommandExecutor::Execute(
            m_GitExecutable,
            m_RepositoryRoot,
            showArgs,
            true);

        if (!showResult.success)
        {
            Logger::Log::Warning("OpenDiff: Failed to get original file content for {}", relPath.string());
            // File might be new/untracked, just open the diff tool anyway
        }

        // Create a temp file with the original content
        std::filesystem::path tempDir = std::filesystem::temp_directory_path();
        std::filesystem::path originalFile = tempDir / ("original_" + filePath.filename().string());
        
        {
            std::ofstream ofs(originalFile);
            if (ofs.is_open())
            {
                ofs << showResult.output;
                ofs.close();
            }
        }

        // Launch the external diff tool without a shell; arguments are passed
        // verbatim, so paths containing spaces or shell metacharacters are safe.
        Platform::LaunchDetached(externalDiffTool, {originalFile.string(), filePath.string()});
        return true;
    }
    else
    {
        // Use git difftool
        std::vector<std::string> args = {"difftool", "--no-prompt", relPath.string()};
        auto result = GitCommandExecutor::Execute(
            m_GitExecutable,
            m_RepositoryRoot,
            args,
            false); // Don't capture output - let it open the tool

        return result.success;
    }
}

bool GitIntegration::IsIgnored(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    std::vector<std::string> args = {"check-ignore", "-q", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    // check-ignore returns 0 if file is ignored, 1 if not ignored
    return result.exitCode == 0;
}

std::string GitIntegration::GetCurrentBranch() const
{
    if (!m_IsRepository)
    {
        Logger::Log::Debug("GetCurrentBranch: Not a repository");
        return "";
    }

    if (m_RepositoryRoot.empty())
    {
        Logger::Log::Debug("GetCurrentBranch: Repository root is empty");
        return "";
    }

    Logger::Log::Debug("GetCurrentBranch: Querying branch for repo at {}", m_RepositoryRoot.string());

    // Use git symbolic-ref to get current branch name
    std::vector<std::string> args = {"symbolic-ref", "--short", "HEAD"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    Logger::Log::Debug("GetCurrentBranch: symbolic-ref result: success={}, exitCode={}, output='{}'", 
                       result.success, result.exitCode, result.output);

    if (result.success && !result.output.empty())
    {
        // Remove trailing newline
        std::string branch = result.output;
        while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r'))
        {
            branch.pop_back();
        }
        return branch;
    }

    // Fallback: Try to get detached HEAD state (commit hash)
    args = {"rev-parse", "--short", "HEAD"};
    result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    Logger::Log::Debug("GetCurrentBranch: rev-parse result: success={}, exitCode={}, output='{}'", 
                       result.success, result.exitCode, result.output);

    if (result.success && !result.output.empty())
    {
        std::string hash = result.output;
        while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r'))
        {
            hash.pop_back();
        }
        return "HEAD detached at " + hash;
    }

    return "";
}

void GitIntegration::RefreshStatus(const std::filesystem::path& path)
{
    if (path.empty())
    {
        // Refresh entire cache. Use the pool so the old snapshot can be
        // recycled instead of deleted, avoiding steady-state allocations.
        StatusCache* vec = AcquireSnapshotVec();
        vec->clear(); // empty snapshot, retains capacity
#if defined(__APPLE__)
        const size_t inactive = 1 - m_StatusCacheSlotIndex.load(std::memory_order_relaxed);
        m_StatusCacheSlots[inactive] = WrapSnapshotVec(vec);
        m_StatusCacheSlotIndex.store(inactive, std::memory_order_release);
#else
        m_StatusCacheSnapshot.store(WrapSnapshotVec(vec), std::memory_order_release);
#endif
        m_LastStatusUpdate = std::chrono::steady_clock::now();
        m_StatusPoller.Wake();
        return;
    }

#if defined(__APPLE__)
    const size_t idx = m_StatusCacheSlotIndex.load(std::memory_order_acquire);
    std::shared_ptr<const StatusCache> snapshot = m_StatusCacheSlots[idx];
#else
    std::shared_ptr<const StatusCache> snapshot = m_StatusCacheSnapshot.load(std::memory_order_acquire);
#endif
    if (snapshot && !snapshot->empty())
    {
        // Remove specific path from cache
        std::filesystem::path relPath = std::filesystem::relative(path, m_RepositoryRoot);
        std::string key = relPath.generic_string();
        std::replace(key.begin(), key.end(), '\\', '/');

        // Build a new snapshot excluding the erased key. Use the pool so the
        // replaced snapshot's memory can be recycled.
        StatusCache* next = AcquireSnapshotVec();
        next->clear();
        next->reserve(snapshot->size());
        for (const auto& e : *snapshot)
        {
            if (e.first != key)
                next->push_back(e);
        }
#if defined(__APPLE__)
        const size_t inactive = 1 - m_StatusCacheSlotIndex.load(std::memory_order_relaxed);
        m_StatusCacheSlots[inactive] = WrapSnapshotVec(next);
        m_StatusCacheSlotIndex.store(inactive, std::memory_order_release);
#else
        m_StatusCacheSnapshot.store(WrapSnapshotVec(next), std::memory_order_release);
#endif
    }

    m_StatusPoller.Wake();
}

void GitIntegration::PollStatus()
{
    if (!m_IsRepository)
        return;

    RefreshIgnoredSetWhenDue();
    UpdateStatusCache();
    FetchWhenDue();
}

void GitIntegration::RefreshIgnoredSetWhenDue()
{
    // The first poll always refreshes, so the UI has accurate Ignored state at
    // once (about 2 s of work during project init, on the poll thread).
    const bool dueByCadence = m_IgnoredTickCounter >= m_IgnoredRefreshRatio;
    if (m_IgnoredNeedsInitialRefresh || dueByCadence)
    {
        try
        {
            RefreshIgnoredSet();
        }
        catch (...)
        {
            // Leave the previous ignored set in place on error.
        }
        m_IgnoredNeedsInitialRefresh = false;
        m_IgnoredTickCounter = 0;
    }
    ++m_IgnoredTickCounter;
}

void GitIntegration::FetchWhenDue()
{
    if (!m_AutoFetchEnabled || m_StatusPoller.IsStopping())
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_LastFetch).count();
    if (elapsed >= m_FetchIntervalSeconds)
    {
        Fetch();
        m_LastFetch = now;
    }
}

void GitIntegration::UpdateStatusCache()
{
    // Check if we should stop before doing any work
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Git: UpdateStatusCache aborted - stop tracking requested");
        return;
    }

    // Fast path: `git status --porcelain` without --ignored. The --ignored
    // flag forces git to walk every ignored path (build/, .vcpkg/, caches)
    // and is ~35x slower (~2s vs ~60ms on a typical engine checkout). The
    // ignored set is refreshed separately at a much lower cadence by
    // RefreshIgnoredSet and merged into the snapshot below.
    std::vector<std::string> args = {"status", "--porcelain"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    // Check again after potentially long Git command
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Git: UpdateStatusCache aborted - stop tracking requested after git status");
        return;
    }

    if (!result.success)
    {
        return;
    }

    // Reuse the scratch vector to avoid per-cycle heap allocations. We write
    // into existing elements via assignment (reuses string heap storage) and
    // only push_back if we exceed the previous cycle's count.
    m_ScratchCount = 0;

    // Estimate entry count from output (one entry per line, ~40 chars avg).
    // Pre-reserve so push_back at the end doesn't trigger repeated growth.
    const size_t estimatedEntries = result.output.size() / 30 + 8;
    if (m_StatusCacheScratch.capacity() < estimatedEntries)
        m_StatusCacheScratch.reserve(estimatedEntries);

    std::istringstream iss(result.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.length() < 3)
        {
            continue;
        }

        std::string status = line.substr(0, 2);
        std::string filePath = line.substr(3);

        // Remove quotes if present
        if (filePath.front() == '"' && filePath.back() == '"')
        {
            filePath = filePath.substr(1, filePath.length() - 2);
        }

        // Normalize path
        std::replace(filePath.begin(), filePath.end(), '\\', '/');
        if (filePath.front() == '/')
        {
            filePath = filePath.substr(1);
        }

        VCSFileStatus fileStatus = VCSFileStatus::Clean;

        if (status == "!!")
        {
            fileStatus = VCSFileStatus::Ignored;
        }
        else if (status == "??")
        {
            fileStatus = VCSFileStatus::Unversioned;
        }
        else if (status[0] == 'M' || status[1] == 'M')
        {
            fileStatus = VCSFileStatus::Modified;
        }
        else if (status[0] == 'A')
        {
            fileStatus = VCSFileStatus::Added;
        }
        else if (status[0] == 'D' || status[1] == 'D')
        {
            fileStatus = VCSFileStatus::Deleted;
        }
        else if (status[0] == 'U' || status[1] == 'U')
        {
            fileStatus = VCSFileStatus::Conflict;
        }

        // Reuse existing element storage when possible (string::operator=
        // reuses heap allocation if capacity is sufficient).
        if (m_ScratchCount < m_StatusCacheScratch.size())
        {
            m_StatusCacheScratch[m_ScratchCount].first = filePath;
            m_StatusCacheScratch[m_ScratchCount].second = fileStatus;
        }
        else
        {
            m_StatusCacheScratch.push_back({std::move(filePath), fileStatus});
        }
        ++m_ScratchCount;
    }
    // Trim excess entries from previous cycle (destroys their strings, but
    // the vector capacity is retained).
    m_StatusCacheScratch.resize(m_ScratchCount);

    // Sort + deduplicate so the snapshot is binary-searchable.
    std::sort(m_StatusCacheScratch.begin(), m_StatusCacheScratch.end(),
        [](const StatusEntry& a, const StatusEntry& b) { return a.first < b.first; });
    auto last = std::unique(m_StatusCacheScratch.begin(), m_StatusCacheScratch.end(),
        [](const StatusEntry& a, const StatusEntry& b) { return a.first == b.first; });
    m_StatusCacheScratch.erase(last, m_StatusCacheScratch.end());

    // Merge the ignored set (refreshed on its own slower cadence) into the
    // fast-status snapshot. Fast-status entries take precedence — a file
    // that's both tracked-modified and matches .gitignore keeps the
    // Modified status. Only pure-unknown paths become Ignored.
    //
    // Both inputs are sorted by path. We produce the merged output in a
    // second scratch vector via a single linear pass (two-pointer merge).
    // Both scratch vectors are single-threaded (poll thread only), so the
    // reserve + swap pattern is safe.
    if (!m_IgnoredScratch.empty())
    {
        const size_t fastSize = m_StatusCacheScratch.size();
        m_MergeScratch.clear();
        m_MergeScratch.reserve(fastSize + m_IgnoredScratch.size());

        size_t fastIdx = 0;
        size_t ignIdx = 0;
        while (fastIdx < fastSize && ignIdx < m_IgnoredScratch.size())
        {
            const StatusEntry& fastEntry = m_StatusCacheScratch[fastIdx];
            const std::string& ignPath = m_IgnoredScratch[ignIdx];
            if (fastEntry.first < ignPath)
            {
                m_MergeScratch.push_back(fastEntry);
                ++fastIdx;
            }
            else if (ignPath < fastEntry.first)
            {
                m_MergeScratch.emplace_back(ignPath, VCSFileStatus::Ignored);
                ++ignIdx;
            }
            else
            {
                // Same path in both — fast-status wins.
                m_MergeScratch.push_back(fastEntry);
                ++fastIdx;
                ++ignIdx;
            }
        }
        for (; fastIdx < fastSize; ++fastIdx)
            m_MergeScratch.push_back(m_StatusCacheScratch[fastIdx]);
        for (; ignIdx < m_IgnoredScratch.size(); ++ignIdx)
            m_MergeScratch.emplace_back(m_IgnoredScratch[ignIdx], VCSFileStatus::Ignored);

        m_StatusCacheScratch.swap(m_MergeScratch);
    }

    // Publish an immutable snapshot. Acquire a recycled vector from the pool
    // (or allocate a new one on the first cycle). Pre-reserve to match
    // scratch capacity, then assign (reuses element storage when sufficient).
    StatusCache* vec = AcquireSnapshotVec();
    if (vec->capacity() < m_StatusCacheScratch.size())
        vec->reserve(m_StatusCacheScratch.size());
    *vec = m_StatusCacheScratch; // reuses capacity + string storage
    auto newSnapshot = WrapSnapshotVec(vec);
#if defined(__APPLE__)
    const size_t inactive = 1 - m_StatusCacheSlotIndex.load(std::memory_order_relaxed);
    m_StatusCacheSlots[inactive] = newSnapshot;
    m_StatusCacheSlotIndex.store(inactive, std::memory_order_release);
#else
    m_StatusCacheSnapshot.store(newSnapshot, std::memory_order_release);
#endif
    m_LastStatusUpdate = std::chrono::steady_clock::now();

    // Notify callbacks. Fire exactly once per refresh (matching SVN/Diversion);
    // listeners should treat this as "snapshot updated, re-query what you need".
    // Previously we iterated every entry and fired per file, which on a repo
    // with a large ignored set produced hundreds of listener invocations per
    // poll and dominated VCS-panel CPU usage.
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Git: UpdateStatusCache aborted - stop tracking requested after cache update");
        return;
    }

    const auto callback = m_StatusChangedCallback;
    if (callback)
    {
        callback();
    }
}

void GitIntegration::RefreshIgnoredSet()
{
    if (m_StatusPoller.IsStopping() || !m_IsRepository)
        return;

    // `git status --porcelain --ignored` walks every ignored path on disk.
    // On a typical engine checkout this costs ~2s wall-time vs ~60ms for a
    // plain porcelain status — so we only run it on the ignored cadence,
    // not the fast status cadence.
    std::vector<std::string> args = {"status", "--porcelain", "--ignored"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (m_StatusPoller.IsStopping() || !result.success)
        return;

    m_IgnoredScratch.clear();

    std::istringstream iss(result.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.length() < 3)
            continue;
        if (!(line[0] == '!' && line[1] == '!'))
            continue; // not an ignored entry — ignore modified/untracked here

        std::string filePath = line.substr(3);
        if (!filePath.empty() && filePath.front() == '"' && filePath.back() == '"')
        {
            filePath = filePath.substr(1, filePath.length() - 2);
        }
        std::replace(filePath.begin(), filePath.end(), '\\', '/');
        if (!filePath.empty() && filePath.front() == '/')
            filePath = filePath.substr(1);

        m_IgnoredScratch.push_back(std::move(filePath));
    }

    std::sort(m_IgnoredScratch.begin(), m_IgnoredScratch.end());
    m_IgnoredScratch.erase(std::unique(m_IgnoredScratch.begin(), m_IgnoredScratch.end()),
                           m_IgnoredScratch.end());
}

void GitIntegration::SetStatusChangedCallback(StatusChangedCallback callback)
{
    m_StatusChangedCallback = callback;
}

void GitIntegration::SetAutoFetch(bool enabled, int intervalSeconds)
{
    m_AutoFetchEnabled = enabled;
    m_FetchIntervalSeconds = std::max(60, intervalSeconds); // Minimum 60 seconds
    if (enabled)
    {
        m_LastFetch = std::chrono::steady_clock::now();
    }
}

bool GitIntegration::IsLocked(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    // Check Git LFS locks
    std::vector<std::string> args = {"lfs", "locks", "--json"};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        // Git LFS might not be installed or file might not be tracked by LFS
        return false;
    }

    // Simple check: if the file path appears in the locks output, it's locked
    // In a full implementation, we'd parse JSON and check lock owner
    return result.output.find(relPath.string()) != std::string::npos;
}

VCSLockInfo GitIntegration::GetLockInfo(const std::filesystem::path& filePath)
{
    VCSLockInfo info{};
    if (!m_IsRepository)
        return info;

    std::error_code ec;
    auto relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
    if (ec)
        return info;
    const std::string wanted = relPath.generic_string();

    auto result = GitCommandExecutor::Execute(
        m_GitExecutable, m_RepositoryRoot, {"lfs", "locks", "--json"}, true);
    if (!result.success)
        return info;

    info.State = VCSLockState::Unlocked;
    info.CanAcquire = true;

    auto userResult = GitCommandExecutor::Execute(
        m_GitExecutable, m_RepositoryRoot,
        std::vector<std::string>{"config", "user.name"}, true);
    std::string currentUser = userResult.success ? userResult.output : std::string{};
    while (!currentUser.empty() && (currentUser.back() == '\n' || currentUser.back() == '\r'))
        currentUser.pop_back();

    const auto json = nlohmann::json::parse(result.output, nullptr, false);
    if (json.is_discarded())
        return info;
    const nlohmann::json* locks = &json;
    if (json.is_object() && json.contains("locks"))
        locks = &json["locks"];
    if (!locks->is_array())
        return info;

    for (const auto& lock : *locks)
    {
        if (!lock.is_object() || lock.value("path", std::string{}) != wanted)
            continue;
        if (lock.contains("owner") && lock["owner"].is_object())
            info.Owner = lock["owner"].value("name", std::string{});
        const bool mine = !currentUser.empty() && info.Owner == currentUser;
        info.State = mine ? VCSLockState::LockedByMe : VCSLockState::LockedByOthers;
        info.CanAcquire = false;
        info.CanRelease = mine;
        break;
    }
    return info;
}

bool GitIntegration::LockFile(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    std::vector<std::string> args = {"lfs", "lock", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    return result.success;
}

bool GitIntegration::UnlockFile(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relPath;
    try
    {
        std::error_code ec;
        relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
        if (ec)
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    std::vector<std::string> args = {"lfs", "unlock", relPath.string()};
    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    return result.success;
}

std::vector<VCSLogEntry> GitIntegration::GetLog(const std::filesystem::path& filePath, int maxEntries)
{
    std::vector<VCSLogEntry> entries;

    if (!m_IsRepository)
    {
        return entries;
    }

    std::vector<std::string> args = {
        "log",
        "--max-count=" + std::to_string(maxEntries),
        "--pretty=format:%H|%an|%ad|%s",
        "--date=iso"
    };

    if (!filePath.empty())
    {
        std::filesystem::path relPath;
        try
        {
            std::error_code ec;
            relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
            if (!ec)
            {
                args.push_back("--");
                // Panel paths come from a case-insensitive filesystem and can
                // differ in case from the committed tree; :(icase) matches the
                // history regardless. generic_string(): git pathspecs use
                // forward slashes on every platform.
                args.push_back(":(icase)" + relPath.generic_string());
            }
        }
        catch (...)
        {
            // Ignore errors
        }
    }

    auto result = GitCommandExecutor::Execute(
        m_GitExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        return entries;
    }

    // Parse log output
    std::istringstream iss(result.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;

        VCSLogEntry entry;
        size_t pos1 = line.find('|');
        if (pos1 == std::string::npos)
            continue;
        entry.revision = line.substr(0, pos1);

        size_t pos2 = line.find('|', pos1 + 1);
        if (pos2 == std::string::npos)
            continue;
        entry.author = line.substr(pos1 + 1, pos2 - pos1 - 1);

        size_t pos3 = line.find('|', pos2 + 1);
        if (pos3 == std::string::npos)
            continue;
        entry.date = line.substr(pos2 + 1, pos3 - pos2 - 1);
        entry.message = line.substr(pos3 + 1);

        entries.push_back(entry);
    }

    return entries;
}

GitIntegration::StatusCache* GitIntegration::AcquireSnapshotVec()
{
    return new StatusCache();
}

std::shared_ptr<const GitIntegration::StatusCache> GitIntegration::WrapSnapshotVec(StatusCache* vec)
{
    return std::shared_ptr<const StatusCache>(vec, [](const StatusCache* p) { delete const_cast<StatusCache*>(p); });
}

} // namespace GameEngine
