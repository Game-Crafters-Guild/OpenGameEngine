#include "SVNIntegration.h"
#include "SVNCommandExecutor.h"
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

#ifndef _WIN32
#include <unistd.h>
#endif

namespace GameEngine
{
SVNIntegration& SVNIntegration::GetInstance()
{
    static SVNIntegration instance;
    return instance;
}

bool SVNIntegration::Initialize(const std::filesystem::path& projectRoot, 
                                 const std::filesystem::path& svnExecutable)
{
    if (m_IsRepository)
    {
        return true; // Already initialized
    }

    try
    {
        m_ProjectRoot = projectRoot;

        // Use provided SVN executable or find it
        if (!svnExecutable.empty() && std::filesystem::exists(svnExecutable))
        {
            m_SVNExecutable = svnExecutable;
        }
        else
        {
            m_SVNExecutable = SVNCommandExecutor::FindSVNExecutable();
        }

        if (m_SVNExecutable.empty())
        {
            Logger::Log::Warning("SVN executable not found. SVN integration disabled.");
            m_IsAvailable = false;
            return false;
        }

        m_IsAvailable = true;

        // Detect SVN repository
        if (!DetectSVNRepository(projectRoot))
        {
            Logger::Log::Info("No SVN repository detected. SVN integration disabled.");
            return false;
        }

        m_IsRepository = true;
        Logger::Log::Info("SVN integration initialized. Repository: {}", m_RepositoryRoot.string());

        // Start status tracking (non-blocking): one poll at once, then one per interval.
        m_StatusPoller.Start("SVN Status", [this] { PollStatus(); });

        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("SVN integration initialization exception: {}", e.what());
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
    catch (...)
    {
        Logger::Log::Warning("SVN integration initialization failed with unknown error");
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
}

void SVNIntegration::Shutdown()
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

bool SVNIntegration::DetectSVNRepository(const std::filesystem::path& projectRoot)
{
    try
    {
        std::filesystem::path current = projectRoot;
        
        while (!current.empty() && current != current.root_path())
        {
            std::error_code ec;
            std::filesystem::path svnDir = current / ".svn";
            if (std::filesystem::exists(svnDir, ec) && std::filesystem::is_directory(svnDir, ec))
            {
                m_RepositoryRoot = current;
                return true;
            }
            
            current = current.parent_path();
        }
    }
    catch (...)
    {
        // Silently fail - not an SVN repository
    }

    return false;
}

VCSFileStatus SVNIntegration::GetFileStatus(const std::filesystem::path& filePath)
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

    // Cache miss - return Clean immediately to avoid blocking
    // The background status thread will populate the cache and trigger a UI refresh
    // DO NOT query SVN synchronously here as it causes major UI stalls
    return VCSFileStatus::Clean;
}

bool SVNIntegration::Add(const std::filesystem::path& filePath)
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
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool SVNIntegration::Remove(const std::filesystem::path& filePath)
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
    std::vector<std::string> args = {"remove", relPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool SVNIntegration::Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::filesystem::path relOldPath = std::filesystem::relative(oldPath, m_RepositoryRoot);
    std::filesystem::path relNewPath = std::filesystem::relative(newPath, m_RepositoryRoot);

    std::vector<std::string> args = {"move", relOldPath.string(), relNewPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
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

bool SVNIntegration::Commit(const std::string& message)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"commit", "-m", message};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool SVNIntegration::Update()
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::vector<std::string> args = {"update"};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool SVNIntegration::Revert(const std::filesystem::path& filePath)
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
    std::vector<std::string> args = {"revert", relPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool SVNIntegration::IsIgnored(const std::filesystem::path& filePath)
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
    std::vector<std::string> args = {"status", "--no-ignore", relPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        return false;
    }

    // If the file appears with 'I' status, it's ignored
    return result.output.find("I") == 0;
}

void SVNIntegration::RefreshStatus(const std::filesystem::path& path)
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

std::string SVNIntegration::GetCurrentBranch() const
{
    if (!m_IsRepository)
    {
        return "";
    }

    // SVN doesn't have branches in the same way as Git
    // Return the current revision number
    std::vector<std::string> args = {"info", "--show-item", "revision"};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success && !result.output.empty())
    {
        std::string revision = result.output;
        // Remove trailing newline
        while (!revision.empty() && (revision.back() == '\n' || revision.back() == '\r'))
        {
            revision.pop_back();
        }
        return "r" + revision;
    }

    return "";
}

void SVNIntegration::SetStatusChangedCallback(StatusChangedCallback callback)
{
    m_StatusChangedCallback = callback;
}

bool SVNIntegration::Lock(const std::filesystem::path& filePath)
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
    std::vector<std::string> args = {"lock", relPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool SVNIntegration::Unlock(const std::filesystem::path& filePath)
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
    std::vector<std::string> args = {"unlock", relPath.string()};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool SVNIntegration::IsLocked(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    VCSFileStatus status = GetFileStatus(filePath);
    return status == VCSFileStatus::LockedByMe || status == VCSFileStatus::LockedByOthers;
}

VCSLockInfo SVNIntegration::GetLockInfo(const std::filesystem::path& filePath)
{
    VCSLockInfo info{};
    if (!m_IsRepository)
        return info;

    switch (GetFileStatus(filePath))
    {
    case VCSFileStatus::LockedByMe:
        info.State = VCSLockState::LockedByMe;
        info.CanRelease = true;
        break;
    case VCSFileStatus::LockedByOthers:
        info.State = VCSLockState::LockedByOthers;
        break;
    default:
        info.State = VCSLockState::Unlocked;
        info.CanAcquire = true;
        break;
    }
    return info;
}

std::vector<VCSLogEntry> SVNIntegration::GetLog(const std::filesystem::path& filePath, int maxEntries)
{
    std::vector<VCSLogEntry> entries;

    if (!m_IsRepository)
    {
        return entries;
    }

    std::vector<std::string> args = {
        "log",
        "--limit", std::to_string(maxEntries),
        "--xml"
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
                args.push_back(relPath.string());
            }
        }
        catch (...)
        {
            // Ignore errors
        }
    }

    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);

    if (!result.success)
    {
        return entries;
    }

    // Parse XML log output
    // Simplified parsing - in a full implementation, use an XML parser
    std::istringstream iss(result.output);
    std::string line;
    VCSLogEntry currentEntry;
    bool inLogEntry = false;
    bool inAuthor = false;
    bool inDate = false;
    bool inMsg = false;

    while (std::getline(iss, line))
    {
        // Simple XML parsing (for production, use a proper XML parser)
        if (line.find("<logentry") != std::string::npos)
        {
            inLogEntry = true;
            currentEntry = VCSLogEntry();
            // Extract revision from logentry tag
            size_t revPos = line.find("revision=\"");
            if (revPos != std::string::npos)
            {
                revPos += 10;
                size_t revEnd = line.find("\"", revPos);
                if (revEnd != std::string::npos)
                {
                    currentEntry.revision = line.substr(revPos, revEnd - revPos);
                }
            }
        }
        else if (line.find("<author>") != std::string::npos)
        {
            inAuthor = true;
            size_t start = line.find(">") + 1;
            size_t end = line.find("<", start);
            if (end != std::string::npos)
            {
                currentEntry.author = line.substr(start, end - start);
            }
        }
        else if (line.find("<date>") != std::string::npos)
        {
            inDate = true;
            size_t start = line.find(">") + 1;
            size_t end = line.find("<", start);
            if (end != std::string::npos)
            {
                currentEntry.date = line.substr(start, end - start);
            }
        }
        else if (line.find("<msg>") != std::string::npos)
        {
            inMsg = true;
            size_t start = line.find(">") + 1;
            size_t end = line.find("<", start);
            if (end != std::string::npos)
            {
                currentEntry.message = line.substr(start, end - start);
            }
        }
        else if (line.find("</logentry>") != std::string::npos)
        {
            if (inLogEntry)
            {
                entries.push_back(currentEntry);
                inLogEntry = false;
            }
        }
    }

    return entries;
}

void SVNIntegration::PollStatus()
{
    if (!m_IsRepository)
        return;

    UpdateStatusCache();
    if (m_StatusChangedCallback)
        m_StatusChangedCallback();
}

void SVNIntegration::UpdateStatusCache()
{
    // Check if we should stop before doing any work
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("SVN: UpdateStatusCache aborted - stop tracking requested");
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_StatusCacheMutex);

    // Check again after acquiring lock
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("SVN: UpdateStatusCache aborted - stop tracking requested after lock");
        return;
    }

    // Run svn status (without -v for speed, like git status --porcelain)
    // Only shows modified/added/deleted files, not all files with version info
    std::vector<std::string> args = {"status"};
    auto result = SVNCommandExecutor::Execute(
        m_SVNExecutable,
        m_RepositoryRoot,
        args,
        true);
    
    // Check again after potentially long SVN command
    if (m_StatusPoller.IsStopping())
    {
        Logger::Log::Debug("SVN: UpdateStatusCache aborted - stop tracking requested after svn status");
        return;
    }

    if (!result.success)
    {
        Logger::Log::Warning("SVN: Failed to get status: {}", result.error);
        return;
    }
    
    if (result.output.empty())
    {
        Logger::Log::Debug("SVN: Status output is empty (repository is clean)");
        m_StatusCache.clear();
        m_LastStatusUpdate = std::chrono::steady_clock::now();
        return;
    }

    // Clear and rebuild cache
    m_StatusCache.clear();

    // svn status output format (without -v):
    // Column 1: file status (M, A, D, ?, C, etc.)
    // Column 2: property status
    // Column 3: lock status (L, etc.)
    // Column 4: history scheduled with commit
    // Column 5: switched
    // Column 6: lock info
    // Column 7: tree conflict
    // Column 8+: filename (starts after 7 columns of status)
    
    std::istringstream iss(result.output);
    std::string line;
    while (std::getline(iss, line))
    {
        if (line.length() < 8)
        {
            continue;
        }

        char fileStatus = line[0];
        char lockStatus = (line.length() > 5) ? line[5] : ' ';

        // Filename starts at column 8 (index 8) in standard svn status output
        std::string filePath = line.substr(8);
        
        // Trim leading whitespace
        size_t start = filePath.find_first_not_of(" \t");
        if (start != std::string::npos)
        {
            filePath = filePath.substr(start);
        }
        
        if (filePath.empty())
        {
            continue;
        }
        
        std::replace(filePath.begin(), filePath.end(), '\\', '/');
        if (!filePath.empty() && filePath.front() == '/')
        {
            filePath = filePath.substr(1);
        }

        VCSFileStatus vcsStatus = VCSFileStatus::Clean;

        switch (fileStatus)
        {
        case '?':
            vcsStatus = VCSFileStatus::Unversioned;
            break;
        case 'A':
            vcsStatus = VCSFileStatus::Added;
            break;
        case 'D':
            vcsStatus = VCSFileStatus::Deleted;
            break;
        case 'M':
            vcsStatus = VCSFileStatus::Modified;
            break;
        case 'C':
        case '~':
            vcsStatus = VCSFileStatus::Conflict;
            break;
        case 'I':
            vcsStatus = VCSFileStatus::Ignored;
            break;
        default:
            vcsStatus = VCSFileStatus::Clean;
            break;
        }

        // Check lock status (column 6, index 5)
        if (lockStatus == 'K')
        {
            vcsStatus = VCSFileStatus::LockedByMe;
        }
        else if (lockStatus == 'O' || lockStatus == 'T' || lockStatus == 'B')
        {
            vcsStatus = VCSFileStatus::LockedByOthers;
        }

        m_StatusCache[filePath] = vcsStatus;
        Logger::Log::Debug("SVN: Cached status for '{}' -> {}", filePath, static_cast<int>(vcsStatus));
    }

    m_LastStatusUpdate = std::chrono::steady_clock::now();
    Logger::Log::Debug("SVN: Status cache populated with {} files", m_StatusCache.size());
    
    // Log all cached entries for debugging
    for (const auto& [path, status] : m_StatusCache)
    {
        Logger::Log::Debug("SVN cache entry: '{}' = {}", path, static_cast<int>(status));
    }

    // NOTE: Don't call callback here - it's called once after UpdateStatusCache in the tracking thread
    // This avoids multiple refreshes (one per file)
}

std::filesystem::path SVNIntegration::FindSVNExecutable()
{
    return SVNCommandExecutor::FindSVNExecutable();
}

bool SVNIntegration::OpenDiff(const std::filesystem::path& filePath,
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
            Logger::Log::Warning("SVN OpenDiff: Failed to get relative path for {}", filePath.string());
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
        // First, get the BASE version (repository version) from SVN
        std::vector<std::string> catArgs = {"cat", "-r", "BASE", relPath.generic_string()};
        auto catResult = SVNCommandExecutor::Execute(
            m_SVNExecutable,
            m_RepositoryRoot,
            catArgs,
            true);

        if (!catResult.success)
        {
            Logger::Log::Warning("SVN OpenDiff: Failed to get BASE version for {}", relPath.string());
            // File might be new/untracked, just open the diff tool anyway
        }

        // Create a temp file with the BASE content
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
        // Use svn diff
        std::vector<std::string> args = {"diff", relPath.string()};
        auto result = SVNCommandExecutor::Execute(
            m_SVNExecutable,
            m_RepositoryRoot,
            args,
            false); // Don't capture output - let it open the tool

        return result.success;
    }
}

} // namespace GameEngine
