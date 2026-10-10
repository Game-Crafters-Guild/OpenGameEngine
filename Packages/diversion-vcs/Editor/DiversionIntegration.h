#pragma once

#include "VCSIntegration/IVCSIntegration.h"
#include "VCSIntegration/VCSStatusPoller.h"
#include "VCSIntegration/VCSCommandExecutor.h"
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

// Diversion integration manager
// Diversion is a cloud-based VCS with local CLI support
class DiversionIntegration : public IVCSIntegration
{
public:
    static DiversionIntegration& GetInstance();

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
    bool Update() override;  // Diversion sync
    bool Revert(const std::filesystem::path& filePath) override;
    bool IsIgnored(const std::filesystem::path& filePath) override;
    void RefreshStatus(const std::filesystem::path& path = {}) override;
    std::filesystem::path GetRepositoryRoot() const override { return m_RepositoryRoot; }
    std::string GetCurrentBranch() const override;  // Returns current workspace info
    std::filesystem::path GetExecutable() const override { return m_DiversionExecutable; }
    void SetStatusChangedCallback(StatusChangedCallback callback) override;
    // Trigger immediate status update and callback (for manual refresh)
    void TriggerStatusUpdate();

    // Set status refresh interval (in seconds)
    void SetStatusRefreshInterval(int seconds) { m_StatusPoller.SetIntervalSeconds(seconds); }

    // Diversion-specific operations
    
    // Get repository ID (dv.repo.xxx)
    std::string GetRepoId() const { return m_RepoId; }
    void SetRepoId(const std::string& repoId) { m_RepoId = repoId; }
    
    // Get workspace ID (dv.ws.xxx)
    std::string GetWorkspaceId() const { return m_WorkspaceId; }
    void SetWorkspaceId(const std::string& workspaceId) { m_WorkspaceId = workspaceId; }
    
    // Authentication
    // The "refresh token" is a CLI integration token that can be exchanged for an OAuth2 access token
    // for API calls. It can also be used directly with CLI commands via DIVERSION_REFRESH_TOKEN.
    void SetRefreshToken(const std::string& token) { 
        std::lock_guard<std::mutex> lock(m_TokenMutex);
        m_RefreshToken = token;
        // Clear access token when refresh token changes to force re-exchange
        m_AccessToken.clear();
    }
    std::string GetRefreshToken() const { 
        std::lock_guard<std::mutex> lock(m_TokenMutex);
        return m_RefreshToken; 
    }
    std::string GetAccessToken() const {
        std::lock_guard<std::mutex> lock(m_TokenMutex);
        return m_AccessToken;
    }
    bool RefreshAccessToken(); // Exchange refresh token for OAuth2 access token
    bool IsAuthenticated() const { 
        std::lock_guard<std::mutex> lock(m_TokenMutex);
        // Check if we have either a valid access token or a refresh token to exchange
        if (!m_AccessToken.empty())
        {
            // Check if access token is still valid (not expired)
            auto now = std::chrono::steady_clock::now();
            return m_AccessTokenExpiry > now;
        }
        return !m_RefreshToken.empty(); // Can authenticate if we have refresh token
    }
    bool ValidateRefreshToken(const std::string& token = "");

    // Open diff for a file using external diff tool or dv diff
    // If externalDiffTool is empty, uses dv diff
    bool OpenDiff(const std::filesystem::path& filePath, 
                  const std::filesystem::path& externalDiffTool = {});

    std::vector<VCSLogEntry> GetLog(const std::filesystem::path& filePath = {},
                                    int maxEntries = 100) override;

private:
    DiversionIntegration() = default;
    ~DiversionIntegration() = default;
    DiversionIntegration(const DiversionIntegration&) = delete;
    DiversionIntegration& operator=(const DiversionIntegration&) = delete;

    bool DetectDiversionRepository(const std::filesystem::path& projectRoot);
    std::filesystem::path FindDiversionExecutable();
    void PollStatus();
    void UpdateStatusCache();
    // Fills the cache from `dv diff --name-status`. Returns false when the CLI
    // call fails, leaving the REST path to try instead.
    bool UpdateStatusCacheFromCli();
    bool FetchRepoAndWorkspaceIds();
    // The CLI integration token as the dv child's environment; empty when no
    // token is set, so the CLI falls back to its own login.
    VCSCommandExecutor::Environment CliTokenEnvironment() const;

    bool m_IsAvailable = false;
    // Atomic: written by Initialize/Shutdown, read by the poll thread and any
    // public-API caller.
    std::atomic<bool> m_IsRepository{false};
    std::filesystem::path m_RepositoryRoot;
    std::filesystem::path m_DiversionExecutable;
    std::filesystem::path m_ProjectRoot;

    // Diversion-specific
    std::string m_RepoId;
    std::string m_WorkspaceId;
    std::string m_RefreshToken; // CLI integration token (can be exchanged for OAuth2 access token)
    std::string m_AccessToken; // OAuth2 access token for API calls
    std::chrono::steady_clock::time_point m_AccessTokenExpiry; // When the access token expires
    mutable std::mutex m_TokenMutex; // Protect token access (mutable for const methods)
    
    // OAuth2 token exchange
    bool ExchangeRefreshTokenForAccessToken();
    bool EnsureValidAccessToken(); // Check and refresh access token if needed

    std::mutex m_StatusCacheMutex;
    std::unordered_map<std::string, VCSFileStatus> m_StatusCache;
    std::chrono::steady_clock::time_point m_LastStatusUpdate;

    StatusChangedCallback m_StatusChangedCallback;

    // Declared last: destroyed first, so the poll thread is joined while the
    // state it reads is still alive.
    VCSStatusPoller m_StatusPoller;
};

} // namespace GameEngine
