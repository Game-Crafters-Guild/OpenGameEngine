#include "LoreIntegration.h"
#include "LoreCommandExecutor.h"
#include "VCSIntegration/VCSPathKey.h"
#include "LoreJsonEvents.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <random>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace GameEngine
{
namespace
{
// Workspace marker directories the CLI itself opens (`.urc` is the pre-rename
// name; existing working trees still carry it).
constexpr const char* kMetadataDirNames[] = {".lore", ".urc"};

constexpr const char* kEventStatusFile = "repositoryStatusFile";
constexpr const char* kEventStatusRevision = "repositoryStatusRevision";

std::string TrimCopy(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Strip surrounding quotes from a TOML string value.
std::string Unquote(std::string s)
{
    s = TrimCopy(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
    {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::string FormatBranchLabel(const LoreRevisionHeader& header)
{
    return header.BranchName + " (revision " + std::to_string(header.RevisionNumber) + ")";
}

std::filesystem::path FindMetadataDir(const std::filesystem::path& root)
{
    std::error_code ec;
    for (const char* name : kMetadataDirNames)
    {
        const std::filesystem::path candidate = root / name;
        if (std::filesystem::is_directory(candidate, ec))
            return candidate;
    }
    return {};
}

std::filesystem::path MakeScratchFilePath(const std::filesystem::path& source)
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        dir = std::filesystem::current_path();
    std::random_device device;
    return dir / ("lore-base-" + std::to_string(device()) + source.extension().string());
}
} // namespace

LoreIntegration& LoreIntegration::GetInstance()
{
    static LoreIntegration instance;
    return instance;
}

bool LoreIntegration::Initialize(const std::filesystem::path& projectRoot,
                                 const std::filesystem::path& loreExecutable)
{
    if (m_IsRepository)
    {
        return true; // Already initialized
    }

    try
    {
        m_ProjectRoot = projectRoot;

        // Use provided Lore executable or find it
        if (!loreExecutable.empty() && std::filesystem::exists(loreExecutable))
        {
            m_LoreExecutable = loreExecutable;
        }
        else
        {
            m_LoreExecutable = LoreCommandExecutor::FindLoreExecutable();
        }

        if (m_LoreExecutable.empty())
        {
            Logger::Log::Warning("Lore executable not found. Lore integration disabled.");
            m_IsAvailable = false;
            return false;
        }

        Logger::Log::Info("Lore: Found executable at: {}", m_LoreExecutable.string());
        m_IsAvailable = true;

        if (!DetectLoreRepository(projectRoot))
        {
            Logger::Log::Info("No Lore repository detected. Lore integration disabled.");
            return false;
        }

        m_IsRepository = true;

        // Read remote_url / identity from config.toml for display.
        ReadConfig();

        Logger::Log::Info("Lore integration initialized. Repository: {}", m_RepositoryRoot.string());

        // Start status tracking (non-blocking): one poll at once, then one per interval.
        m_StatusPoller.Start("Lore Status", [this] { PollStatus(); });

        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Lore integration initialization exception: {}", e.what());
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
    catch (...)
    {
        Logger::Log::Warning("Lore integration initialization failed with unknown error");
        m_IsAvailable = false;
        m_IsRepository = false;
        return false;
    }
}

void LoreIntegration::Shutdown()
{
    // The poll thread is joined before the callback is cleared, so no poll
    // calls into a listener after Shutdown returns.
    m_IsRepository = false;
    m_StatusPoller.Stop();
    m_StatusChangedCallback = nullptr;

    // Clear all state to allow proper reinitialization
    m_IsAvailable = false;
    m_RepositoryRoot.clear();
    m_MetadataDir.clear();
    m_ProjectRoot.clear();
    m_RemoteUrl.clear();
    m_Identity.clear();

    // The poll thread is joined, so the lock is uncontended.
    {
        std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
        m_StatusCache.clear();
        m_BranchLabel.clear();
    }
}

bool LoreIntegration::DetectLoreRepository(const std::filesystem::path& projectRoot)
{
    try
    {
        std::filesystem::path current = projectRoot;

        while (!current.empty() && current != current.root_path())
        {
            const std::filesystem::path metadataDir = FindMetadataDir(current);
            if (!metadataDir.empty())
            {
                m_RepositoryRoot = current;
                m_MetadataDir = metadataDir;
                return true;
            }

            current = current.parent_path();
        }
    }
    catch (...)
    {
        // Silently fail - not a Lore repository
    }

    return false;
}

void LoreIntegration::ReadConfig()
{
    m_RemoteUrl.clear();
    m_Identity.clear();

    std::error_code ec;
    std::filesystem::path configPath = m_MetadataDir / "config.toml";
    if (!std::filesystem::exists(configPath, ec))
    {
        return;
    }

    std::ifstream configFile(configPath);
    if (!configFile.is_open())
    {
        return;
    }

    // remote_url and identity are top-level keys; stop at the first table
    // header so a same-named key inside [store] or [file] cannot shadow them.
    std::string line;
    while (std::getline(configFile, line))
    {
        std::string trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        if (trimmed[0] == '[')
            break;

        size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string key = TrimCopy(trimmed.substr(0, eqPos));
        std::string value = Unquote(trimmed.substr(eqPos + 1));

        if (key == "remote_url")
        {
            m_RemoteUrl = value;
        }
        else if (key == "identity")
        {
            m_Identity = value;
        }
    }
}

bool LoreIntegration::TryGetRepositoryRelativePath(const std::filesystem::path& filePath,
                                                   std::string& outRelativePath) const
{
    std::error_code ec;
    const std::filesystem::path relPath = std::filesystem::relative(filePath, m_RepositoryRoot, ec);
    if (ec || relPath.empty())
        return false;

    std::string key = relPath.generic_string();
    if (key == "." || key.rfind("..", 0) == 0)
        return false;

    outRelativePath = std::move(key);
    return true;
}

VCSFileStatus LoreIntegration::GetFileStatus(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return VCSFileStatus::Unversioned;
    }

    // Only status-cache reads use the lexical fast path. Commands retain
    // canonical path resolution in TryGetRepositoryRelativePath.
    const auto relative = VCS::RepoRelative(filePath, m_RepositoryRoot);
    std::string key = relative.generic_string();
    if (key.empty() || key == "." || key.rfind("..", 0) == 0)
    {
        return VCSFileStatus::Unversioned;
    }

    std::lock_guard<std::mutex> lock(m_StatusCacheMutex);

    auto it = m_StatusCache.find(key);
    if (it != m_StatusCache.end())
    {
        return it->second;
    }

    // Cache miss - return Clean immediately to avoid blocking.
    // The background status thread populates the cache and triggers a UI refresh.
    return VCSFileStatus::Clean;
}

bool LoreIntegration::Add(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::string relPath;
    if (!TryGetRepositoryRelativePath(filePath, relPath))
    {
        return false;
    }

    // A single file path is checked against the filesystem and staged when it
    // differs from the current revision, whatever its dirty flag.
    std::vector<std::string> args = {"stage", relPath};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool LoreIntegration::Remove(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::string relPath;
    if (!TryGetRepositoryRelativePath(filePath, relPath))
    {
        return false;
    }

    // Lore has no separate remove verb: staging a path that is gone from disk
    // records the deletion in the next revision.
    std::vector<std::string> args = {"stage", relPath};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool LoreIntegration::Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::string relOldPath;
    std::string relNewPath;
    if (!TryGetRepositoryRelativePath(oldPath, relOldPath) ||
        !TryGetRepositoryRelativePath(newPath, relNewPath))
    {
        return false;
    }

    std::vector<std::string> args = {"stage", "move", relOldPath, relNewPath};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus(oldPath);
        RefreshStatus(newPath);
    }

    return result.success;
}

bool LoreIntegration::Commit(const std::string& message)
{
    if (!m_IsRepository)
    {
        return false;
    }

    // `lore commit <MESSAGE>` takes the message as one positional argument.
    std::vector<std::string> args = {"commit", message};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool LoreIntegration::Update()
{
    if (!m_IsRepository)
    {
        return false;
    }

    // `lore sync` synchronizes the working tree with the branch head.
    std::vector<std::string> args = {"sync"};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus();
    }

    return result.success;
}

bool LoreIntegration::Revert(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::string relPath;
    if (!TryGetRepositoryRelativePath(filePath, relPath))
    {
        return false;
    }

    // `lore reset <path>` restores the path to the current revision and drops
    // the staged change, which is what Revert means for every other provider.
    std::vector<std::string> args = {"reset", relPath};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    if (result.success)
    {
        RefreshStatus(filePath);
    }

    return result.success;
}

bool LoreIntegration::IsIgnored(const std::filesystem::path& filePath)
{
    // The CLI exposes no ignore-query command; ignored paths simply never
    // appear in the status stream.
    (void)filePath;
    return false;
}

void LoreIntegration::RefreshStatus(const std::filesystem::path& path)
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
            std::string key;
            if (TryGetRepositoryRelativePath(path, key))
                m_StatusCache.erase(key);
        }
    }

    m_StatusPoller.Wake();
}

std::string LoreIntegration::GetCurrentBranch() const
{
    if (!m_IsRepository)
    {
        return "";
    }

    {
        std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
        if (!m_BranchLabel.empty())
            return m_BranchLabel;
    }

    // No poll has completed yet: read just the revision header, no tree walk.
    std::vector<std::string> args = {"status", "--revision-only"};
    auto result = LoreCommandExecutor::ExecuteJson(m_LoreExecutable, m_RepositoryRoot, args);
    if (!result.success)
        return "";

    for (const auto& event : ParseLoreJsonEvents(result.output))
    {
        if (event.value("tagName", std::string{}) != kEventStatusRevision)
            continue;
        LoreRevisionHeader header;
        if (ParseLoreRevisionHeader(event.value("data", nlohmann::json::object()), header))
            return FormatBranchLabel(header);
    }
    return "";
}

void LoreIntegration::SetStatusChangedCallback(StatusChangedCallback callback)
{
    m_StatusChangedCallback = callback;
}

bool LoreIntegration::Login()
{
    if (!m_IsAvailable || m_LoreExecutable.empty())
    {
        return false;
    }

    // `lore login [remote-url]` authenticates the CLI against the remote. Lore
    // owns its own credential store; we just launch the flow.
    std::vector<std::string> args = {"login"};
    if (!m_RemoteUrl.empty())
    {
        args.push_back(m_RemoteUrl);
    }

    std::filesystem::path wd = m_IsRepository ? m_RepositoryRoot : m_ProjectRoot;
    if (wd.empty())
    {
        wd = std::filesystem::current_path();
    }

    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, wd, args, true);
    if (result.success)
    {
        RefreshStatus();
    }
    return result.success;
}

std::vector<VCSLogEntry> LoreIntegration::GetLog(const std::filesystem::path& filePath,
                                                 int maxEntries)
{
    if (!m_IsRepository)
    {
        return {};
    }

    // `lore history [LENGTH]` is repository-wide; `lore file history <PATH>
    // [LENGTH]` narrows it to one file. Both stream the same event shapes.
    std::vector<std::string> args;
    std::string relPath;
    if (!filePath.empty() && TryGetRepositoryRelativePath(filePath, relPath))
    {
        args = {"file", "history", relPath, std::to_string(maxEntries)};
    }
    else
    {
        args = {"history", std::to_string(maxEntries)};
    }

    auto result = LoreCommandExecutor::ExecuteJson(m_LoreExecutable, m_RepositoryRoot, args);
    if (!result.success)
    {
        Logger::Log::Warning("Lore: history failed: {}", result.error);
        return {};
    }

    return ParseLoreHistoryEvents(ParseLoreJsonEvents(result.output));
}

void LoreIntegration::PollStatus()
{
    if (!m_IsRepository)
        return;

    UpdateStatusCache();
    if (m_StatusChangedCallback)
        m_StatusChangedCallback();
}

void LoreIntegration::UpdateStatusCache()
{
    if (m_StatusPoller.IsStopping())
    {
        return;
    }

    // `lore status --scan` walks the working tree and reconciles dirty flags;
    // the JSON stream carries the revision header plus one event per dirty,
    // staged or conflicted node. The CLI runs outside the lock so UI-thread
    // GetFileStatus lookups keep answering from the previous snapshot.
    std::vector<std::string> args = {"status", "--scan"};
    auto result = LoreCommandExecutor::ExecuteJson(m_LoreExecutable, m_RepositoryRoot, args);

    if (m_StatusPoller.IsStopping())
    {
        return;
    }

    if (!result.success)
    {
        Logger::Log::Warning("Lore: Failed to get status: {}", result.error);
        return;
    }

    std::unordered_map<std::string, VCSFileStatus> statuses;
    std::string branchLabel;
    for (const auto& event : ParseLoreJsonEvents(result.output))
    {
        const std::string tag = event.value("tagName", std::string{});
        const auto dataIt = event.find("data");
        if (dataIt == event.end())
            continue;

        if (tag == kEventStatusFile)
        {
            LoreFileStatusEntry entry;
            if (ParseLoreStatusFileEvent(*dataIt, entry))
                statuses[entry.Path] = entry.Status;
        }
        else if (tag == kEventStatusRevision)
        {
            LoreRevisionHeader header;
            if (ParseLoreRevisionHeader(*dataIt, header))
                branchLabel = FormatBranchLabel(header);
        }
    }

    std::lock_guard<std::mutex> lock(m_StatusCacheMutex);
    if (m_StatusPoller.IsStopping())
    {
        return;
    }
    m_StatusCache = std::move(statuses);
    if (!branchLabel.empty())
        m_BranchLabel = std::move(branchLabel);
    m_LastStatusUpdate = std::chrono::steady_clock::now();
    Logger::Log::Debug("Lore: Status cache populated with {} files", m_StatusCache.size());
}

std::string LoreIntegration::ReadCommittedContent(const std::filesystem::path& filePath)
{
    if (!m_IsRepository)
    {
        return {};
    }

    std::string relPath;
    if (!TryGetRepositoryRelativePath(filePath, relPath))
    {
        return {};
    }

    // `lore file write --path <repo path> --output <file>` extracts the file
    // as recorded in the current revision; it only writes to disk, so a
    // scratch file carries the bytes back.
    const std::filesystem::path scratch = MakeScratchFilePath(filePath);
    std::vector<std::string> args = {"file", "write", "--path", relPath, "--output", scratch.string()};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, true);

    std::string content;
    if (result.success)
    {
        std::ifstream file(scratch, std::ios::binary);
        if (file.is_open())
        {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            content = buffer.str();
        }
    }
    else
    {
        Logger::Log::Warning("Lore: file write failed for {}: {}", relPath, result.error);
    }

    std::error_code ec;
    std::filesystem::remove(scratch, ec);
    return content;
}

bool LoreIntegration::OpenDiff(const std::filesystem::path& filePath,
                               const std::filesystem::path& externalDiffTool)
{
    if (!m_IsRepository)
    {
        return false;
    }

    std::string relPath;
    if (!TryGetRepositoryRelativePath(filePath, relPath))
    {
        Logger::Log::Warning("Lore OpenDiff: Failed to get relative path for {}", filePath.string());
        return false;
    }

    if (!externalDiffTool.empty() && std::filesystem::exists(externalDiffTool))
    {
        // Two-pane compare: the committed content goes to a scratch file that
        // the tool keeps open; the OS reclaims the temp directory.
        const std::string base = ReadCommittedContent(filePath);
        const std::filesystem::path basePath = MakeScratchFilePath(filePath);
        {
            std::ofstream baseFile(basePath, std::ios::binary);
            baseFile << base;
        }
#ifdef _WIN32
        std::string command = "start \"\" \"" + externalDiffTool.string() + "\" \"" +
                              basePath.string() + "\" \"" + filePath.string() + "\"";
        system(command.c_str());
#else
        pid_t pid = fork();
        if (pid == 0)
        {
            execlp(externalDiffTool.string().c_str(),
                   externalDiffTool.filename().string().c_str(),
                   basePath.string().c_str(),
                   filePath.string().c_str(),
                   nullptr);
            _exit(1);
        }
#endif
        return true;
    }

    // `lore diff <path>`: current revision against the working file.
    std::vector<std::string> args = {"diff", relPath};
    auto result = LoreCommandExecutor::Execute(m_LoreExecutable, m_RepositoryRoot, args, false);
    return result.success;
}

} // namespace GameEngine
