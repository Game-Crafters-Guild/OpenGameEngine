#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier
#endif

#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSStatusPoller.h"
#include "VCSIntegration/VCSFileStatus.h"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

namespace GameEngine
{

// Backward compatibility: GitFileStatus is an alias to VCSFileStatus
using GitFileStatus = VCSFileStatus;

// Git integration manager
class GitIntegration : public IVCSIntegration
{
public:
    static GitIntegration& GetInstance();

    // IVCSIntegration interface implementation
    bool Initialize(const std::filesystem::path& projectRoot, 
                    const std::filesystem::path& executable = {}) override;
    void Shutdown() override;
    bool IsAvailable() const override { return m_IsAvailable; }
    bool IsRepository() const override { return m_IsRepository; }
    VCSFileStatus GetFileStatus(const std::filesystem::path& filePath) override;
    bool Add(const std::filesystem::path& filePath) override;
    bool Remove(const std::filesystem::path& filePath) override;
    bool Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) override;
    bool Commit(const std::string& message) override;
    bool Update() override;  // Maps to Pull() for Git
    bool Revert(const std::filesystem::path& filePath) override;
    bool IsIgnored(const std::filesystem::path& filePath) override;
    void RefreshStatus(const std::filesystem::path& path = {}) override;
    std::filesystem::path GetRepositoryRoot() const override { return m_RepositoryRoot; }
    std::string GetCurrentBranch() const override;
    std::filesystem::path GetExecutable() const override { return m_GitExecutable; }
    void SetStatusChangedCallback(StatusChangedCallback callback) override;

    // Git-specific operations (not in IVCSIntegration)
    bool Push();
    bool Pull();  // Git-specific, Update() calls this
    bool Fetch();

    // Git-specific operations (not in IVCSIntegration)
    // Open diff for a file using external diff tool or git difftool
    // If externalDiffTool is empty, uses git's configured difftool
    bool OpenDiff(const std::filesystem::path& filePath, 
                  const std::filesystem::path& externalDiffTool = {});

    // Periodic fetch settings
    void SetAutoFetch(bool enabled, int intervalSeconds = 300);
    bool IsAutoFetchEnabled() const { return m_AutoFetchEnabled; }
    int GetFetchInterval() const { return m_FetchIntervalSeconds; }
    void SetStatusRefreshInterval(int seconds) { m_StatusPoller.SetIntervalSeconds(seconds); }

    // How many fast-poll ticks between refreshes of the ignored-file set.
    // Ignored entries come from `git status --ignored`, which is ~35x slower
    // than a plain porcelain status because git walks the entire ignored tree.
    // The ignored set changes rarely (only when .gitignore edits), so polling
    // it on a much lower cadence than the fast status keeps the UI signal
    // without the steady-state CPU cost.
    void SetIgnoredRefreshRatio(int ticks) { m_IgnoredRefreshRatio = ticks > 0 ? ticks : 10; }

    // Check if file is locked by Git LFS
    bool IsLocked(const std::filesystem::path& filePath);
    bool LockFile(const std::filesystem::path& filePath);
    bool UnlockFile(const std::filesystem::path& filePath);
    VCSLockInfo GetLockInfo(const std::filesystem::path& filePath) override;
    bool AcquireLock(const std::filesystem::path& filePath) override { return LockFile(filePath); }
    bool ReleaseLock(const std::filesystem::path& filePath) override { return UnlockFile(filePath); }

    std::vector<VCSLogEntry> GetLog(const std::filesystem::path& filePath = {},
                                    int maxEntries = 100) override;

    // Backward compatibility: GetGitExecutable() maps to GetExecutable()
    std::filesystem::path GetGitExecutable() const { return GetExecutable(); }

private:
    GitIntegration() = default;
    ~GitIntegration() = default;
    GitIntegration(const GitIntegration&) = delete;
    GitIntegration& operator=(const GitIntegration&) = delete;

    bool DetectGitRepository(const std::filesystem::path& projectRoot);
    std::filesystem::path FindGitExecutable();
    void PollStatus();
    void RefreshIgnoredSetWhenDue();
    void FetchWhenDue();
    void UpdateStatusCache();
    // Runs `git status --porcelain --ignored`, extracts just the `!!` entries,
    // and stores them in m_IgnoredScratch. Called much less often than
    // UpdateStatusCache because --ignored is ~35x slower than plain porcelain.
    void RefreshIgnoredSet();

    bool m_IsAvailable = false;
    // Written by Initialize/Shutdown on one thread, read by the poll thread
    // and by any thread that calls the public API (GetFileStatus,
    // RefreshStatus, etc.). Atomic to keep reads/writes defined under the memory model.
    std::atomic<bool> m_IsRepository{false};
    bool m_AutoFetchEnabled = false;
    int m_FetchIntervalSeconds = 300;
    int m_IgnoredRefreshRatio = 10; // Default: ignored polled every 10 fast ticks (~5min at 30s poll)
    int m_IgnoredTickCounter = 0;   // Counts fast ticks since last ignored refresh.
    bool m_IgnoredNeedsInitialRefresh = true; // Force ignored refresh on the first tick.
    std::filesystem::path m_RepositoryRoot;
    std::filesystem::path m_GitExecutable;
    std::filesystem::path m_ProjectRoot;

    // Flat sorted vector for O(log N) lookup without per-element heap nodes.
    // vector::clear() keeps capacity, and element-wise assignment reuses
    // string heap storage, so steady-state cycles allocate nothing.
    using StatusEntry = std::pair<std::string, VCSFileStatus>;
    using StatusCache = std::vector<StatusEntry>;

    // Binary search helper for sorted StatusCache.
    static const StatusEntry* FindInSnapshot(const StatusCache& cache, const std::string& key)
    {
        auto it = std::lower_bound(cache.begin(), cache.end(), key,
            [](const StatusEntry& e, const std::string& k) { return e.first < k; });
        if (it != cache.end() && it->first == key)
            return &*it;
        return nullptr;
    }

#if defined(__APPLE__)
    // macOS/libc++: std::atomic<std::shared_ptr<...>> not reliably supported (C++20 P0718).
    // Lock-free double buffer: writer updates inactive slot then flips index.
    static constexpr size_t kStatusCacheSlots = 2;
    std::shared_ptr<const StatusCache> m_StatusCacheSlots[kStatusCacheSlots];
    std::atomic<size_t> m_StatusCacheSlotIndex{0};
#else
    std::atomic<std::shared_ptr<const StatusCache>> m_StatusCacheSnapshot;
#endif
    // Scratch vector reused across UpdateStatusCache() calls. Only accessed
    // from the background status thread. We track a write index so that
    // existing elements' string storage is reused via assignment.
    StatusCache m_StatusCacheScratch;
    size_t m_ScratchCount = 0; // number of valid entries in scratch this cycle

    // Destination for the two-pointer merge of fast status + ignored set.
    // Swapped back into m_StatusCacheScratch each cycle; retains capacity
    // across cycles for allocation-free steady state.
    StatusCache m_MergeScratch;

    // Ignored-file set. Refreshed at a much lower cadence than the main
    // status cache (see m_IgnoredRefreshRatio) because git --ignored is
    // expensive. Merged into the published snapshot on every publish so
    // GetFileStatus returns Ignored for these paths.
    // Stored as a sorted vector of paths (same key format as StatusCache).
    std::vector<std::string> m_IgnoredScratch;
    std::chrono::steady_clock::time_point m_LastStatusUpdate;
    std::chrono::steady_clock::time_point m_LastFetch;

    StatusCache* AcquireSnapshotVec();
    std::shared_ptr<const StatusCache> WrapSnapshotVec(StatusCache* vec);

    StatusChangedCallback m_StatusChangedCallback;

    // Declared last: destroyed first, so the poll thread is joined while the
    // state it reads is still alive.
    VCSStatusPoller m_StatusPoller;
};

} // namespace GameEngine

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
