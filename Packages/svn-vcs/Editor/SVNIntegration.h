#pragma once

#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSStatusPoller.h"
#include "VCSIntegration/VCSFileStatus.h"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

namespace GameEngine
{

// SVN integration manager
class SVNIntegration : public IVCSIntegration
{
public:
    static SVNIntegration& GetInstance();

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
    bool Update() override;  // SVN update
    bool Revert(const std::filesystem::path& filePath) override;
    bool IsIgnored(const std::filesystem::path& filePath) override;
    void RefreshStatus(const std::filesystem::path& path = {}) override;
    std::filesystem::path GetRepositoryRoot() const override { return m_RepositoryRoot; }
    std::string GetCurrentBranch() const override;  // Returns current revision/branch
    std::filesystem::path GetExecutable() const override { return m_SVNExecutable; }
    void SetStatusChangedCallback(StatusChangedCallback callback) override;

    // Set status refresh interval (in seconds)
    void SetStatusRefreshInterval(int seconds) { m_StatusPoller.SetIntervalSeconds(seconds); }

    // SVN-specific operations
    bool Lock(const std::filesystem::path& filePath);
    bool Unlock(const std::filesystem::path& filePath);
    bool IsLocked(const std::filesystem::path& filePath);
    VCSLockInfo GetLockInfo(const std::filesystem::path& filePath) override;
    bool AcquireLock(const std::filesystem::path& filePath) override { return Lock(filePath); }
    bool ReleaseLock(const std::filesystem::path& filePath) override { return Unlock(filePath); }

    // Open diff for a file using external diff tool or svn diff
    // If externalDiffTool is empty, uses svn diff
    bool OpenDiff(const std::filesystem::path& filePath, 
                  const std::filesystem::path& externalDiffTool = {});

    std::vector<VCSLogEntry> GetLog(const std::filesystem::path& filePath = {},
                                    int maxEntries = 100) override;

private:
    SVNIntegration() = default;
    ~SVNIntegration() = default;
    SVNIntegration(const SVNIntegration&) = delete;
    SVNIntegration& operator=(const SVNIntegration&) = delete;

    bool DetectSVNRepository(const std::filesystem::path& projectRoot);
    std::filesystem::path FindSVNExecutable();
    void PollStatus();
    void UpdateStatusCache();

    bool m_IsAvailable = false;
    // Atomic: written by Initialize/Shutdown, read by the poll thread and any
    // public-API caller.
    std::atomic<bool> m_IsRepository{false};
    std::filesystem::path m_RepositoryRoot;
    std::filesystem::path m_SVNExecutable;
    std::filesystem::path m_ProjectRoot;

    std::mutex m_StatusCacheMutex;
    std::unordered_map<std::string, VCSFileStatus> m_StatusCache;
    std::chrono::steady_clock::time_point m_LastStatusUpdate;

    StatusChangedCallback m_StatusChangedCallback;

    // Declared last: destroyed first, so the poll thread is joined while the
    // state it reads is still alive.
    VCSStatusPoller m_StatusPoller;
};

} // namespace GameEngine
