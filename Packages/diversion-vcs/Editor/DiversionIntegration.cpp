#include "DiversionIntegration.h"
#include "DiversionCommandExecutor.h"
#include "VCSIntegration/VCSPathKey.h"
#include "Platform/HttpClient.h"
#include "Logger/Logger.h"
#include "Platform/Thread.h"
#include "Platform/Shell.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <atomic>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace GameEngine
{
namespace
{
// The environment variable the dv CLI reads its integration token from.
constexpr const char* kCliTokenVariable = "DIVERSION_REFRESH_TOKEN";
} // namespace

DiversionIntegration& DiversionIntegration::GetInstance()
{
    static DiversionIntegration instance;
    return instance;
}

bool DiversionIntegration::Initialize(const std::filesystem::path& projectRoot, 
                                       const std::filesystem::path& dvExecutable)
{
    if (m_IsRepository)
    {
        return true; // Already initialized
    }

    try
    {
        m_ProjectRoot = projectRoot;

        // Use provided Diversion executable or find it
        if (!dvExecutable.empty() && std::filesystem::exists(dvExecutable))
        {
            m_DiversionExecutable = dvExecutable;
        }
        else
        {
            m_DiversionExecutable = DiversionCommandExecutor::FindDiversionExecutable();
        }

        if (m_DiversionExecutable.empty())
        {
            Logger::Log::Warning("Diversion executable not found. Diversion integration disabled.");
            m_IsAvailable = false;
            return false;
        }

        Logger::Log::Info("Diversion: Found executable at: {}", m_DiversionExecutable.string());
        m_IsAvailable = true;

        // Detect Diversion repository
        if (!DetectDiversionRepository(projectRoot))
        {
            Logger::Log::Info("No Diversion repository detected. Diversion integration disabled.");
            return false;
        }

        m_IsRepository = true;
        Logger::Log::Info("Diversion integration initialized. Repository: {}", m_RepositoryRoot.string());

        // Try to fetch repo and workspace IDs from CLI
        FetchRepoAndWorkspaceIds();
        
        // Try to exchange refresh token for access token if we have one
        {
            std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
            if (!m_RefreshToken.empty())
            {
                // Exchange in background (non-blocking)
                std::thread([this]() {
                    GameEngine::Platform::SetCurrentThreadName("Diversion Auth");
                    ExchangeRefreshTokenForAccessToken();
                }).detach();
            }
        }

        // Start status tracking (non-blocking): one poll at once, then one per interval.
        m_StatusPoller.Start("Diversion Status", [this] { PollStatus(); });

        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Diversion integration initialization exception: {}", e.what());
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
    catch (...)
    {
        Logger::Log::Warning("Diversion integration initialization failed with unknown error");
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
}

void DiversionIntegration::Shutdown()
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
    m_RepoId.clear();
    m_WorkspaceId.clear();
    m_AccessToken.clear();
    
    // Clear status cache - use try_lock to avoid blocking if thread is holding the mutex
    {
        std::unique_lock<std::mutex> lock(m_StatusCacheMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            m_StatusCache.clear();
        }
        // If we can't get the lock, the thread will exit soon and clear it's fine to skip
    }
}

bool DiversionIntegration::DetectDiversionRepository(const std::filesystem::path& projectRoot)
{
    try
    {
        std::filesystem::path current = projectRoot;
        
        while (!current.empty() && current != current.root_path())
        {
            std::error_code ec;
            std::filesystem::path diversionDir = current / ".diversion";
            if (std::filesystem::exists(diversionDir, ec) && std::filesystem::is_directory(diversionDir, ec))
            {
                m_RepositoryRoot = current;
                return true;
            }
            
            current = current.parent_path();
        }
    }
    catch (...)
    {
        // Silently fail - not a Diversion repository
    }

    return false;
}

std::filesystem::path DiversionIntegration::FindDiversionExecutable()
{
    return DiversionCommandExecutor::FindDiversionExecutable();
}

bool DiversionIntegration::FetchRepoAndWorkspaceIds()
{
    if (!m_IsAvailable || m_DiversionExecutable.empty())
    {
        return false;
    }

    // Try to read from .diversion/config file first
    std::filesystem::path configPath = m_RepositoryRoot / ".diversion" / "config";
    std::error_code ec;
    if (std::filesystem::exists(configPath, ec))
    {
        std::ifstream configFile(configPath);
        if (configFile.is_open())
        {
            std::string line;
            while (std::getline(configFile, line))
            {
                // Look for repo_id or workspace_id in config
                // Format could be: repo_id = dv.repo.xxx or repo = xxx
                size_t eqPos = line.find('=');
                if (eqPos != std::string::npos)
                {
                    std::string key = line.substr(0, eqPos);
                    std::string value = line.substr(eqPos + 1);
                    
                    // Trim whitespace
                    auto trimWs = [](std::string& s) {
                        size_t start = s.find_first_not_of(" \t\r\n");
                        size_t end = s.find_last_not_of(" \t\r\n");
                        if (start != std::string::npos && end != std::string::npos)
                            s = s.substr(start, end - start + 1);
                        else
                            s.clear();
                    };
                    trimWs(key);
                    trimWs(value);
                    
                    if (key == "repo_id" || key == "repo" || key == "repository")
                    {
                        m_RepoId = value;
                        Logger::Log::Debug("Diversion: Found repo ID from config: {}", m_RepoId);
                    }
                    else if (key == "workspace_id" || key == "workspace" || key == "ws")
                    {
                        m_WorkspaceId = value;
                        Logger::Log::Debug("Diversion: Found workspace ID from config: {}", m_WorkspaceId);
                    }
                }
            }
            configFile.close();
            
            if (!m_RepoId.empty() || !m_WorkspaceId.empty())
            {
                return true;
            }
        }
    }

    // Fallback: Run dv info to get repo and workspace IDs
    std::vector<std::string> args = {"info"};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        // Try dv status as another fallback
        args = {"status"};
        result = DiversionCommandExecutor::Execute(
            m_DiversionExecutable,
            m_RepositoryRoot,
            args,
            true);
            
        if (!result.success)
        {
            Logger::Log::Warning("Diversion: Failed to fetch repo/workspace IDs: {}", result.error);
            return false;
        }
    }

    // Parse output for repo and workspace info
    std::istringstream iss(result.output);
    std::string line;
    while (std::getline(iss, line))
    {
        // Look for dv.repo. pattern
        size_t repoPos = line.find("dv.repo.");
        if (repoPos != std::string::npos)
        {
            size_t start = repoPos;
            size_t end = line.find_first_of(" \t\n\r)", start + 8);
            if (end == std::string::npos)
                end = line.length();
            m_RepoId = line.substr(start, end - start);
            Logger::Log::Debug("Diversion: Found repo ID: {}", m_RepoId);
        }
        
        // Look for "Repository:" or "Repo:" prefix
        if (m_RepoId.empty())
        {
            size_t colonPos = line.find(':');
            if (colonPos != std::string::npos)
            {
                std::string key = line.substr(0, colonPos);
                std::string value = line.substr(colonPos + 1);
                // Trim
                size_t start = key.find_first_not_of(" \t");
                size_t end = key.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos)
                    key = key.substr(start, end - start + 1);
                start = value.find_first_not_of(" \t");
                end = value.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos)
                    value = value.substr(start, end - start + 1);
                
                if (key == "Repository" || key == "Repo" || key == "repository" || key == "repo")
                {
                    m_RepoId = value;
                    Logger::Log::Debug("Diversion: Found repo ID: {}", m_RepoId);
                }
                else if (key == "Workspace" || key == "workspace" || key == "ws")
                {
                    m_WorkspaceId = value;
                    Logger::Log::Debug("Diversion: Found workspace ID: {}", m_WorkspaceId);
                }
            }
        }

        // Look for dv.ws. pattern
        if (m_WorkspaceId.empty())
        {
            size_t wsPos = line.find("dv.ws.");
            if (wsPos != std::string::npos)
            {
                size_t start = wsPos;
                size_t end = line.find_first_of(" \t\n\r)", start + 6);
                if (end == std::string::npos)
                    end = line.length();
                m_WorkspaceId = line.substr(start, end - start);
                Logger::Log::Debug("Diversion: Found workspace ID: {}", m_WorkspaceId);
            }
        }
    }

    return !m_RepoId.empty() || !m_WorkspaceId.empty();
}

VCSFileStatus DiversionIntegration::GetFileStatus(const std::filesystem::path& filePath)
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

    std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
    
    std::string key = relPath.generic_string();
    std::replace(key.begin(), key.end(), '\\', '/');

    // No logging here: cache hits run per visible Assets-panel cell, and the
    // interactive editor keeps the logger at Debug.
    auto it = m_StatusCache.find(key);
    if (it != m_StatusCache.end())
    {
        return it->second;
    }

    // Also try without leading slash
    if (!key.empty() && key[0] == '/')
    {
        std::string keyNoSlash = key.substr(1);
        auto it2 = m_StatusCache.find(keyNoSlash);
        if (it2 != m_StatusCache.end())
        {
            return it2->second;
        }
    }

    // Cache miss - check whether status can be read at all before answering.
    // The CLI authenticates itself, so its presence alone is enough; a missing
    // integration token no longer means "not configured".
    bool canReadStatus = !m_DiversionExecutable.empty();
    if (!canReadStatus)
    {
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        // Check if we have either a valid access token or a refresh token to exchange
        if (!m_AccessToken.empty())
        {
            auto now = std::chrono::steady_clock::now();
            canReadStatus = (m_AccessTokenExpiry > now);
        }
        else if (!m_RefreshToken.empty() && m_RefreshToken.length() >= 10)
        {
            // We have a refresh token that might be valid
            canReadStatus = true;
        }
    }

    if (!canReadStatus)
    {
        // No CLI and no token - return NotConfigured instead of Unversioned
        Logger::Log::Debug("Diversion GetFileStatus: Cache miss for '{}', no status source - returning NotConfigured", key);
        return VCSFileStatus::NotConfigured;
    }

    // Cache miss but can authenticate - return Clean (not Unversioned)
    // The background thread will update via API and only mark as Unversioned if API confirms it
    Logger::Log::Debug("Diversion GetFileStatus: Cache miss for '{}', returning Clean (will be updated by background thread via API)", key);
    return VCSFileStatus::Clean;
}

bool DiversionIntegration::Add(const std::filesystem::path& filePath)
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

    // Diversion uses 'dv add' command
    std::vector<std::string> args = {"add", relPath.string()};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool DiversionIntegration::Remove(const std::filesystem::path& filePath)
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

    // Diversion uses 'dv rm' command
    std::vector<std::string> args = {"rm", relPath.string()};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool DiversionIntegration::Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relOldPath = std::filesystem::relative(oldPath, m_RepositoryRoot);
    std::filesystem::path relNewPath = std::filesystem::relative(newPath, m_RepositoryRoot);

    // Diversion uses 'dv mv' command
    std::vector<std::string> args = {"mv", relOldPath.string(), relNewPath.string()};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
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

bool DiversionIntegration::Commit(const std::string& message)
{
    if (!m_IsRepository)
    {
        return false;
    }

    // Diversion uses 'dv commit' command
    std::vector<std::string> args = {"commit", "-m", message};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool DiversionIntegration::Update()
{
    if (!m_IsRepository)
    {
        return false;
    }

    // Diversion uses 'dv sync' command to sync with remote
    std::vector<std::string> args = {"sync"};
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool DiversionIntegration::Revert(const std::filesystem::path& filePath)
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

    // Normalize path separators for Diversion
    std::string normalizedPath = relPath.generic_string();
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    if (!normalizedPath.empty() && normalizedPath.front() == '/')
    {
        normalizedPath = normalizedPath.substr(1);
    }

    // Diversion uses 'dv reset' command to discard changes
    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        {"reset", normalizedPath},
        true,
        CliTokenEnvironment());

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool DiversionIntegration::IsIgnored(const std::filesystem::path& filePath)
{
    // Diversion respects .gitignore-style patterns in .diversionignore
    // For now, return false as we don't have a direct API to check this
    (void)filePath;
    return false;
}

void DiversionIntegration::RefreshStatus(const std::filesystem::path& path)
{
    {
        std::lock_guard<std::mutex> lock(m_StatusCacheMutex);

        if (path.empty())
        {
            // Refresh entire cache
            m_StatusCache.clear();
            m_LastStatusUpdate = std::chrono::steady_clock::now();
        }
        else
        {
            // Remove specific path from cache
            std::filesystem::path relPath = std::filesystem::relative(path, m_RepositoryRoot);
            std::string key = relPath.generic_string();
            std::replace(key.begin(), key.end(), '\\', '/');
            m_StatusCache.erase(key);
        }
    }

    m_StatusPoller.Wake();
}

std::string DiversionIntegration::GetCurrentBranch() const
{
    if (!m_IsRepository)
    {
        return "";
    }

    // Return workspace ID as the "branch" equivalent
    // No need to run status command - workspace ID is already fetched during initialization
    if (!m_WorkspaceId.empty())
    {
        return m_WorkspaceId;
    }

    return "workspace";
}

void DiversionIntegration::SetStatusChangedCallback(StatusChangedCallback callback)
{
    m_StatusChangedCallback = callback;
}

void DiversionIntegration::TriggerStatusUpdate()
{
    // Clear cache and trigger immediate update
    RefreshStatus();
    
    // Trigger status update in background (non-blocking)
    // This will populate the cache and trigger the callback
    if (m_IsRepository && !m_StatusPoller.IsStopping())
    {
        // Trigger callback immediately to refresh UI
        // The background thread will update the cache asynchronously
        if (m_StatusChangedCallback)
        {
            m_StatusChangedCallback();
        }
        
        // Also trigger an immediate cache update if we have a valid token
        // Do this in a separate thread to avoid blocking
        std::thread([this]() {
            GameEngine::Platform::SetCurrentThreadName("Diversion Cache");
            if (!m_StatusPoller.IsStopping() && m_IsRepository)
            {
                UpdateStatusCache();
                // Trigger callback again after cache is updated
                if (m_StatusChangedCallback)
                {
                    m_StatusChangedCallback();
                }
            }
        }).detach();
    }
}

VCSCommandExecutor::Environment DiversionIntegration::CliTokenEnvironment() const
{
    std::lock_guard<std::mutex> lock(m_TokenMutex);
    if (m_RefreshToken.empty())
        return {};
    return {{kCliTokenVariable, m_RefreshToken}};
}

bool DiversionIntegration::RefreshAccessToken()
{
    return ExchangeRefreshTokenForAccessToken();
}

bool DiversionIntegration::ExchangeRefreshTokenForAccessToken()
{
    std::string refreshToken;
    {
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        refreshToken = m_RefreshToken;
    }
    
    if (refreshToken.empty())
    {
        Logger::Log::Warning("Diversion: Cannot exchange token - refresh token is empty");
        return false;
    }
    
    // Exchange refresh token for access token via OAuth2
    std::string url = "https://auth.diversion.dev/oauth2/token";
    std::unordered_map<std::string, std::string> formData = {
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
        {"client_id", "j084768v4hd6j1pf8df4h4c47"}
    };
    
    Logger::Log::Debug("Diversion: Exchanging refresh token for access token...");
    auto response = HttpClient::PostForm(url, formData);
    
    if (!response.success || response.statusCode != 200)
    {
        Logger::Log::Warning("Diversion: Failed to exchange refresh token for access token (status: {}): {}", 
                            response.statusCode, response.error.empty() ? response.body : response.error);
        return false;
    }
    
    try
    {
        auto json = nlohmann::json::parse(response.body);
        std::string accessToken = json.value("access_token", "");
        
        if (accessToken.empty() || accessToken.length() < 10)
        {
            Logger::Log::Warning("Diversion: Invalid access token received (length: {})", accessToken.length());
            return false;
        }
        
        // Get expiry time (default to 1 hour if not provided)
        int expiresIn = json.value("expires_in", 3600);
        
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        m_AccessToken = accessToken;
        auto now = std::chrono::steady_clock::now();
        m_AccessTokenExpiry = now + std::chrono::seconds(expiresIn - 60); // Refresh 1 minute before expiry
        
        Logger::Log::Debug("Diversion: Successfully exchanged refresh token for access token (expires in {} seconds)", expiresIn);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Diversion: Failed to parse access token response: {}", e.what());
        return false;
    }
}

bool DiversionIntegration::EnsureValidAccessToken()
{
    {
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        
        auto now = std::chrono::steady_clock::now();
        
        // If we have a valid access token that's not expired, we're good
        if (!m_AccessToken.empty() && m_AccessTokenExpiry > now)
        {
            return true;
        }
    }
    
    // Need to refresh - mutex is unlocked, call ExchangeRefreshTokenForAccessToken
    return ExchangeRefreshTokenForAccessToken();
}

bool DiversionIntegration::ValidateRefreshToken(const std::string& token)
{
    std::string tokenToValidate = token.empty() ? m_RefreshToken : token;
    
    if (tokenToValidate.empty())
    {
        Logger::Log::Debug("Diversion: Token validation failed - token is empty");
        return false;
    }
    
    // Basic format validation: check length and non-whitespace
    // Diversion tokens are typically JWT tokens or similar, should be reasonably long
    if (tokenToValidate.length() < 10)
    {
        Logger::Log::Debug("Diversion: Token validation failed - token too short");
        return false;
    }
    
    // Check if token is all whitespace
    bool hasNonWhitespace = false;
    for (char c : tokenToValidate)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            hasNonWhitespace = true;
            break;
        }
    }
    
    if (!hasNonWhitespace)
    {
        Logger::Log::Debug("Diversion: Token validation failed - token is all whitespace");
        return false;
    }
    
    // Validate via CLI by testing with a command that requires authentication
    if (!m_DiversionExecutable.empty())
    {
        std::filesystem::path testDir = m_IsRepository ? m_RepositoryRoot : m_ProjectRoot;
        if (testDir.empty())
        {
            testDir = std::filesystem::current_path();
        }
        
        // Set the CLI integration token as environment variable
        // Note: This is a CLI integration token, not an access token - it's used directly by the CLI
        const VCSCommandExecutor::Environment environment = {{kCliTokenVariable, tokenToValidate}};

        // Try running a simple authenticated command
        auto result = DiversionCommandExecutor::Execute(
            m_DiversionExecutable,
            testDir,
            {"info"},
            true,
            environment);
        
        if (result.success)
        {
            Logger::Log::Debug("Diversion: Token validation passed - CLI command succeeded");
            return true;
        }
        else
        {
            // Check if error is authentication-related
            std::string errorLower = result.error;
            std::transform(errorLower.begin(), errorLower.end(), errorLower.begin(), ::tolower);
            if (errorLower.find("auth") != std::string::npos || 
                errorLower.find("unauthorized") != std::string::npos ||
                errorLower.find("token") != std::string::npos ||
                errorLower.find("invalid") != std::string::npos)
            {
                Logger::Log::Debug("Diversion: Token validation failed - authentication error: {}", result.error);
                return false;
            }
            
            // If it's not an auth error, token format might be valid but command failed for other reasons
            // Still consider format valid
            Logger::Log::Debug("Diversion: Token format validation passed, but CLI command failed (non-auth): {}", result.error);
            return true; // Format is valid, even if command failed
        }
    }
    
    Logger::Log::Debug("Diversion: Token format validation passed (length: {})", tokenToValidate.length());
    return true;
}

std::vector<VCSLogEntry> DiversionIntegration::GetLog(
    const std::filesystem::path& filePath, 
    int maxEntries)
{
    std::vector<VCSLogEntry> entries;

    if (!m_IsRepository)
    {
        return entries;
    }

    // Diversion log command
    std::vector<std::string> args = {"log", "--limit", std::to_string(maxEntries)};

    if (!filePath.empty())
    {
        std::filesystem::path relPath;
        try
        {
            std::error_code ec;
            relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
            if (!ec)
            {
                args.push_back(relPath.string());
            }
        }
        catch (...)
        {
            // Ignore errors
        }
    }

    auto result = DiversionCommandExecutor::Execute(
        m_DiversionExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        return entries;
    }

    // Parse log output - format depends on Diversion CLI output
    // This is a simplified implementation
    std::istringstream iss(result.output);
    std::string line;
    VCSLogEntry currentEntry;

    while (std::getline(iss, line))
    {
        if (line.find("commit ") == 0)
        {
            if (!currentEntry.revision.empty())
            {
                entries.push_back(currentEntry);
            }
            currentEntry = VCSLogEntry();
            currentEntry.revision = line.substr(7);
        }
        else if (line.find("Author: ") == 0)
        {
            currentEntry.author = line.substr(8);
        }
        else if (line.find("Date: ") == 0)
        {
            currentEntry.date = line.substr(6);
        }
        else if (!line.empty() && line[0] == ' ')
        {
            // Message lines are indented
            if (!currentEntry.message.empty())
            {
                currentEntry.message += "\n";
            }
            currentEntry.message += line.substr(4);
        }
    }

    if (!currentEntry.revision.empty())
    {
        entries.push_back(currentEntry);
    }

    return entries;
}

void DiversionIntegration::PollStatus()
{
    if (!m_IsRepository)
        return;

    UpdateStatusCache();
    if (m_StatusChangedCallback)
        m_StatusChangedCallback();
}

bool DiversionIntegration::UpdateStatusCacheFromCli()
{
    // `dv diff --name-status` prints one "<code>\t<path>" line per change, the
    // same shape git uses. The CLI carries its own credentials, so this works
    // with no integration token at all - which matters because a Diversion API
    // key (dvk_...) is not an OAuth refresh-token grant and can never satisfy
    // the REST path's token exchange.
    const DiversionCommandExecutor::Result execResult = DiversionCommandExecutor::Execute(
        m_DiversionExecutable, m_RepositoryRoot, {"diff", "--name-status"}, true);
    if (!execResult.success)
        return false;

    // The CLI exits 0 while logged out and simply prints that it is logged out,
    // so the exit code alone would cache "no changes" for every file. Treat a
    // logged-out reply as a failed read and let the REST path answer instead.
    if (execResult.output.find("not logged in") != std::string::npos ||
        execResult.output.find("no credentials found") != std::string::npos)
    {
        Logger::Log::Debug("Diversion: CLI reports logged out; leaving status to the API path");
        return false;
    }

    std::unordered_map<std::string, VCSFileStatus> fresh;
    std::istringstream stream(execResult.output);
    std::string line;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
            continue;

        VCSFileStatus status;
        switch (line[0])
        {
        case 'A': status = VCSFileStatus::Added; break;
        case 'D': status = VCSFileStatus::Deleted; break;
        case 'M':
        case 'R': status = VCSFileStatus::Modified; break;
        default: continue; // unknown code: leave the path to the Clean default
        }
        fresh[line.substr(tab + 1)] = status;
    }

    const size_t count = fresh.size();
    {
        std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
        m_StatusCache = std::move(fresh);
        m_LastStatusUpdate = std::chrono::steady_clock::now();
    }
    Logger::Log::Debug("Diversion: Status cache updated from CLI, {} files in cache", count);
    return true;
}

void DiversionIntegration::UpdateStatusCache()
{
    // Check if we should stop before doing any work
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested");
        return;
    }

    // The CLI is already a hard requirement for the integration to start, and it
    // authenticates itself, so it is the dependable source. The REST path below
    // stays as the fallback for a session that has a working access token.
    if (UpdateStatusCacheFromCli())
        return;

    // Ensure we have a valid access token for API calls
    if (!EnsureValidAccessToken())
    {
        Logger::Log::Debug("Diversion: Cannot update status - no valid access token");
        std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
        m_StatusCache.clear();
        m_LastStatusUpdate = std::chrono::steady_clock::now();
        return;
    }
    
    // Check again after potentially blocking token refresh
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested after token refresh");
        return;
    }

    std::string accessToken;
    std::string repoId;
    std::string workspaceId;
    {
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        accessToken = m_AccessToken;
        repoId = m_RepoId;
        workspaceId = m_WorkspaceId;
    }

    // Ensure repo and workspace IDs have proper prefixes
    if (!repoId.empty() && repoId.find("dv.repo.") != 0)
    {
        repoId = "dv.repo." + repoId;
    }
    if (!workspaceId.empty() && workspaceId.find("dv.ws.") != 0)
    {
        workspaceId = "dv.ws." + workspaceId;
    }

    if (repoId.empty() || workspaceId.empty())
    {
        // Check if we should stop before CLI call
        if (m_StatusPoller.IsStopping())
        {
            Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested before FetchRepoAndWorkspaceIds");
            return;
        }
        
        Logger::Log::Debug("Diversion: Missing repo ID or workspace ID - cannot fetch status via API");
        // Fall back to CLI to get repo/workspace IDs
        FetchRepoAndWorkspaceIds();
        
        // Check again after potentially long CLI call
        if (m_StatusPoller.IsStopping())
        {
            Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested after FetchRepoAndWorkspaceIds");
            return;
        }
        
        {
            std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
            repoId = m_RepoId;
            workspaceId = m_WorkspaceId;
        }
        if (!repoId.empty() && repoId.find("dv.repo.") != 0)
            repoId = "dv.repo." + repoId;
        if (!workspaceId.empty() && workspaceId.find("dv.ws.") != 0)
            workspaceId = "dv.ws." + workspaceId;
        
        if (repoId.empty() || workspaceId.empty())
        {
            Logger::Log::Debug("Diversion: Still missing repo ID or workspace ID after CLI fetch");
            std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
            m_StatusCache.clear();
            m_LastStatusUpdate = std::chrono::steady_clock::now();
            return;
        }
    }

    std::lock_guard<std::mutex> lock(m_StatusCacheMutex);

    // Clear cache first
    m_StatusCache.clear();

    // Use Workspace Status API to get file statuses
    // This is more reliable than parsing CLI output
    Logger::Log::Debug("Diversion: Fetching workspace status via API for repo={}, workspace={}", repoId, workspaceId);
    
    int limit = 1000;
    int skip = 0;
    bool more = true;
    
    while (more && !m_StatusPoller.IsStopping())
    {
        // Check if we should stop before making API call
        if (m_StatusPoller.IsStopping())
        {
            Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested during API calls");
            break;
        }
        
        std::string apiUrl = "https://api.diversion.dev/v0/repos/" + repoId + "/workspaces/" + workspaceId + 
                            "/status?detail_items=true&recurse=true&limit=" + std::to_string(limit) + 
                            "&skip=" + std::to_string(skip);
        
        std::unordered_map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + accessToken}
        };
        
        auto response = HttpClient::Get(apiUrl, headers);
        
        // Check again after potentially long HTTP call
        if (m_StatusPoller.IsStopping())
        {
            Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested after API call");
            break;
        }
        
        if (!response.success || response.statusCode != 200)
        {
            Logger::Log::Warning("Diversion: Workspace Status API request failed (status: {}): {}", 
                                response.statusCode, response.error.empty() ? response.body : response.error);
            break;
        }
        
        try
        {
            auto json = nlohmann::json::parse(response.body);
            
            // Parse items from response
            if (json.contains("items") && json["items"].is_object())
            {
                auto items = json["items"];
                int itemsAddedThisPage = 0;
                
                // Process new files
                if (items.contains("new") && items["new"].is_array())
                {
                    for (const auto& item : items["new"])
                    {
                        if (item.contains("path") && item["path"].is_string())
                        {
                            std::string path = item["path"];
                            // Normalize path
                            std::replace(path.begin(), path.end(), '\\', '/');
                            if (!path.empty() && path.front() == '/')
                                path = path.substr(1);
                            
                            if (!path.empty())
                            {
                                m_StatusCache[path] = VCSFileStatus::Added;
                                itemsAddedThisPage++;
                            }
                        }
                    }
                }
                
                // Process modified files
                if (items.contains("modified") && items["modified"].is_array())
                {
                    for (const auto& item : items["modified"])
                    {
                        if (item.contains("path") && item["path"].is_string())
                        {
                            std::string path = item["path"];
                            
                            // Normalize path
                            std::replace(path.begin(), path.end(), '\\', '/');
                            if (!path.empty() && path.front() == '/')
                                path = path.substr(1);
                            
                            if (!path.empty())
                            {
                                // Mark as Modified (moved files are also shown as modified)
                                m_StatusCache[path] = VCSFileStatus::Modified;
                                itemsAddedThisPage++;
                            }
                        }
                    }
                }
                
                // Process deleted files
                if (items.contains("deleted") && items["deleted"].is_array())
                {
                    for (const auto& item : items["deleted"])
                    {
                        if (item.contains("path") && item["path"].is_string())
                        {
                            std::string path = item["path"];
                            // Normalize path
                            std::replace(path.begin(), path.end(), '\\', '/');
                            if (!path.empty() && path.front() == '/')
                                path = path.substr(1);
                            
                            if (!path.empty())
                            {
                                m_StatusCache[path] = VCSFileStatus::Deleted;
                                itemsAddedThisPage++;
                            }
                        }
                    }
                }
                
                // Process conflicted files
                if (items.contains("conflicted") && items["conflicted"].is_array())
                {
                    for (const auto& item : items["conflicted"])
                    {
                        if (item.contains("path") && item["path"].is_string())
                        {
                            std::string path = item["path"];
                            // Normalize path
                            std::replace(path.begin(), path.end(), '\\', '/');
                            if (!path.empty() && path.front() == '/')
                                path = path.substr(1);
                            
                            if (!path.empty())
                            {
                                m_StatusCache[path] = VCSFileStatus::Conflict;
                                itemsAddedThisPage++;
                            }
                        }
                    }
                }
                
                // Process moved files (if separate from modified)
                if (items.contains("moved") && items["moved"].is_array())
                {
                    for (const auto& item : items["moved"])
                    {
                        if (item.contains("path") && item["path"].is_string())
                        {
                            std::string path = item["path"];
                            // Normalize path
                            std::replace(path.begin(), path.end(), '\\', '/');
                            if (!path.empty() && path.front() == '/')
                                path = path.substr(1);
                            
                            if (!path.empty())
                            {
                                // Mark as Modified (moved files are essentially modified)
                                m_StatusCache[path] = VCSFileStatus::Modified;
                                itemsAddedThisPage++;
                            }
                        }
                    }
                }
                
                // Check if there are more pages
                if (itemsAddedThisPage < limit)
                {
                    more = false;
                }
                else
                {
                    skip += limit;
                }
            }
            else
            {
                more = false;
            }
            
            // Also process top-level conflicts array (if present)
            if (json.contains("conflicts") && json["conflicts"].is_array())
            {
                for (const auto& conflictPath : json["conflicts"])
                {
                    if (conflictPath.is_string())
                    {
                        std::string path = conflictPath;
                        // Normalize path
                        std::replace(path.begin(), path.end(), '\\', '/');
                        if (!path.empty() && path.front() == '/')
                            path = path.substr(1);
                        
                        if (!path.empty())
                        {
                            m_StatusCache[path] = VCSFileStatus::Conflict;
                        }
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Diversion: Failed to parse Workspace Status API response: {}", e.what());
            break;
        }
    }
    
    // Also get all tracked files via CLI to mark them as Clean if not in API response
    // This ensures we show Clean status for files that haven't changed
    std::string refreshToken;
    {
        std::lock_guard<std::mutex> tokenLock(m_TokenMutex);
        refreshToken = m_RefreshToken;
    }
    
    // Check if we should stop before CLI call
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested before CLI call");
        return;
    }
    
    if (!refreshToken.empty())
    {
        Logger::Log::Debug("Diversion: Running 'dv ls' to get all tracked files for Clean status");
        const VCSCommandExecutor::Environment environment = {{kCliTokenVariable, refreshToken}};
        auto lsResult = DiversionCommandExecutor::Execute(
            m_DiversionExecutable,
            m_RepositoryRoot,
            {"ls"},
            true,
            environment);
        
        // Check again after potentially long CLI call
        if (m_StatusPoller.IsStopping())
        {
            Logger::Log::Debug("Diversion: UpdateStatusCache aborted - stop tracking requested after CLI call");
            return;
        }
        
        if (lsResult.success && !lsResult.output.empty())
        {
            std::istringstream lsStream(lsResult.output);
            std::string line;
            int trackedCount = 0;
            while (std::getline(lsStream, line))
            {
                if (line.empty())
                    continue;
                
                if (line.find("Will list") != std::string::npos || 
                    line.find("list contents") != std::string::npos)
                {
                    continue;
                }
                
                std::string filePath;
                
                // Parse file path from ls output (similar to before)
                if (line.length() >= 3 && line[0] == '(' && (line[1] == 'F' || line[1] == 'D') && line[2] == ')')
                {
                    size_t pathStart = 3;
                    while (pathStart < line.length() && (line[pathStart] == ' ' || line[pathStart] == '\t'))
                        pathStart++;
                    
                    if (pathStart < line.length())
                    {
                        std::vector<std::string> tokens;
                        std::istringstream tokenStream(line.substr(pathStart));
                        std::string token;
                        while (tokenStream >> token)
                            tokens.push_back(token);
                        
                        if (!tokens.empty())
                        {
                            bool hasPermissions = false;
                            if (!tokens.back().empty() && (tokens.back()[0] == '-' || tokens.back()[0] == 'd'))
                                hasPermissions = true;
                            
                            bool hasSize = false;
                            if (tokens.size() >= 2)
                            {
                                const std::string& secondLast = tokens[tokens.size() - 2];
                                if (secondLast.find("kB") != std::string::npos || 
                                    secondLast.find("MB") != std::string::npos ||
                                    secondLast.find("GB") != std::string::npos)
                                    hasSize = true;
                            }
                            
                            size_t pathTokenCount = tokens.size();
                            if (hasPermissions) pathTokenCount--;
                            if (hasSize) pathTokenCount--;
                            
                            if (pathTokenCount > 0)
                            {
                                filePath = tokens[0];
                                for (size_t i = 1; i < pathTokenCount; i++)
                                    filePath += " " + tokens[i];
                            }
                        }
                    }
                }
                else
                {
                    filePath = line;
                }
                
                if (filePath.empty())
                    continue;
                
                std::replace(filePath.begin(), filePath.end(), '\\', '/');
                size_t start = filePath.find_first_not_of(" \t\r\n");
                if (start != std::string::npos)
                    filePath = filePath.substr(start);
                size_t end = filePath.find_last_not_of(" \t\r\n");
                if (end != std::string::npos && end < filePath.length() - 1)
                    filePath = filePath.substr(0, end + 1);
                
                if (!filePath.empty())
                {
                    if (filePath.front() == '/')
                        filePath = filePath.substr(1);
                    
                    // Only mark as Clean if not already in cache (API response takes precedence)
                    if (m_StatusCache.find(filePath) == m_StatusCache.end())
                    {
                        m_StatusCache[filePath] = VCSFileStatus::Clean;
                        trackedCount++;
                    }
                }
            }
            Logger::Log::Debug("Diversion: Found {} additional tracked files via 'dv ls' (marked as Clean)", trackedCount);
        }
    }
    
    m_LastStatusUpdate = std::chrono::steady_clock::now();
    Logger::Log::Debug("Diversion: Status cache updated, {} files in cache", m_StatusCache.size());
    
    // Log all cached entries for debugging
    for (const auto& [path, status] : m_StatusCache)
    {
        Logger::Log::Debug("Diversion cache entry: '{}' = {}", path, static_cast<int>(status));
    }
}

bool DiversionIntegration::OpenDiff(const std::filesystem::path& filePath,
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
            Logger::Log::Warning("Diversion OpenDiff: Failed to get relative path for {}", filePath.string());
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    // Normalize path separators for Diversion
    std::string normalizedPath = relPath.generic_string();
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    if (!normalizedPath.empty() && normalizedPath.front() == '/')
    {
        normalizedPath = normalizedPath.substr(1);
    }

    if (!externalDiffTool.empty() && std::filesystem::exists(externalDiffTool))
    {
        // Use external diff tool directly
        // First, get the original file content from Diversion using 'dv cat'
        auto catResult = DiversionCommandExecutor::Execute(
            m_DiversionExecutable,
            m_RepositoryRoot,
            {"cat", normalizedPath},
            true,
            CliTokenEnvironment());

        if (!catResult.success)
        {
            Logger::Log::Warning("Diversion OpenDiff: Failed to get original file content for {}", normalizedPath);
            // File might be new/untracked, just open the diff tool anyway
        }

        // Create a temp file with the original content
        std::filesystem::path tempDir = std::filesystem::temp_directory_path();
        std::filesystem::path originalFile = tempDir / ("original_" + filePath.filename().string());
        
        {
            std::ofstream ofs(originalFile);
            if (ofs.is_open())
            {
                ofs << catResult.output;
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
        // Use dv diff
        auto result = DiversionCommandExecutor::Execute(
            m_DiversionExecutable,
            m_RepositoryRoot,
            {"diff", normalizedPath},
            false, // Don't capture output - let it open the tool
            CliTokenEnvironment());

        return result.success;
    }
}

} // namespace GameEngine
