#pragma once

#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSStatusPoller.h"
#include "VCSIntegration/VCSFileStatus.h"
#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

namespace GameEngine
{

// Lore integration manager.
// Lore is a centralized, content-addressed VCS aimed at code plus large binary
// assets. Integration is pure CLI shell-out via the `lore` binary; status,
// branch and history are read from its `--json` event stream. Authentication
// is owned by the CLI itself (`lore login` / `lore auth`), so this backend
// stores no credentials.
class LoreIntegration : public IVCSIntegration
{
public:
    static LoreIntegration& GetInstance();

    // IVCSIntegration interface implementation
    bool Initialize(const std::filesystem::path& projectRoot,
                    const std::filesystem::path& executable = {}) override;
    void Shutdown() override;
    bool IsAvailable() const override { return m_IsAvailable; }
    bool IsRepository() const override { return m_IsRepository; }
    VCSFileStatus GetFileStatus(const std::filesystem::path& filePath) override;
    bool Add(const std::filesystem::path& filePath) override;        // lore stage
    bool Remove(const std::filesystem::path& filePath) override;     // lore stage (a missing file stages as delete)
    bool Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) override; // lore stage move
    bool Commit(const std::string& message) override;                // lore commit
    bool Update() override;                                          // lore sync
    bool Revert(const std::filesystem::path& filePath) override;     // lore reset
    bool IsIgnored(const std::filesystem::path& filePath) override;
    void RefreshStatus(const std::filesystem::path& path = {}) override;
    std::filesystem::path GetRepositoryRoot() const override { return m_RepositoryRoot; }
    // "<branch> (revision N)" from the last status poll; polls the CLI once
    // when no poll has completed yet.
    std::string GetCurrentBranch() const override;
    std::filesystem::path GetExecutable() const override { return m_LoreExecutable; }
    void SetStatusChangedCallback(StatusChangedCallback callback) override;

    // Set status refresh interval (in seconds)
    void SetStatusRefreshInterval(int seconds) { m_StatusPoller.SetIntervalSeconds(seconds); }

    // Lore-specific accessors (read from the workspace's config.toml)
    std::string GetRemoteUrl() const { return m_RemoteUrl; }
    std::string GetIdentity() const { return m_Identity; }

    // Authenticate the CLI against the repository's remote (`lore login <url>`).
    // Lore manages its own credential store; this just kicks off the CLI flow.
    bool Login();

    // Open diff for a file using an external diff tool, or `lore diff`.
    bool OpenDiff(const std::filesystem::path& filePath,
                  const std::filesystem::path& externalDiffTool = {});

    // Content of the file at the current synced revision (`lore file write`).
    // Empty when the file is not tracked or the CLI fails.
    std::string ReadCommittedContent(const std::filesystem::path& filePath);

    // Revision history entries (`lore --json history`).
    std::vector<VCSLogEntry> GetLog(const std::filesystem::path& filePath = {},
                                    int maxEntries = 100) override;

private:
    LoreIntegration() = default;
    ~LoreIntegration() = default;
    LoreIntegration(const LoreIntegration&) = delete;
    LoreIntegration& operator=(const LoreIntegration&) = delete;

    bool DetectLoreRepository(const std::filesystem::path& projectRoot);
    void ReadConfig();  // Parse <metadata dir>/config.toml for remote_url + identity
    bool TryGetRepositoryRelativePath(const std::filesystem::path& filePath,
                                      std::string& outRelativePath) const;
    void PollStatus();
    void UpdateStatusCache();

    bool m_IsAvailable = false;
    // Atomic: written by Initialize/Shutdown, read by the poll thread and any
    // public-API caller.
    std::atomic<bool> m_IsRepository{false};
    std::filesystem::path m_RepositoryRoot;
    std::filesystem::path m_MetadataDir; // <root>/.lore (or the pre-rename .urc)
    std::filesystem::path m_LoreExecutable;
    std::filesystem::path m_ProjectRoot;

    // Parsed from config.toml
    std::string m_RemoteUrl;
    std::string m_Identity;

    mutable std::mutex m_StatusCacheMutex;
    std::unordered_map<std::string, VCSFileStatus> m_StatusCache;
    std::string m_BranchLabel; // guarded by m_StatusCacheMutex
    std::chrono::steady_clock::time_point m_LastStatusUpdate;

    StatusChangedCallback m_StatusChangedCallback;

    // Declared last: destroyed first, so the poll thread is joined while the
    // state it reads is still alive.
    VCSStatusPoller m_StatusPoller;
};

} // namespace GameEngine
