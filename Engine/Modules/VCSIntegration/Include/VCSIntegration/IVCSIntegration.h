#pragma once

#include "VCSIntegration/VCSFileStatus.h"
#include <filesystem>
#include <string>
#include <vector>
#include <functional>

namespace GameEngine
{

// One revision-history entry, shared by every provider's log output.
struct VCSLogEntry
{
    std::string revision;
    std::string author;
    std::string date;
    std::string message;
};

enum class VCSLockState
{
    Unsupported,
    Unlocked,
    LockedByMe,
    LockedByOthers,
};

// Provider-neutral file-lock capabilities and current state. Providers that
// do not implement file locking retain the default Unsupported result.
struct VCSLockInfo
{
    VCSLockState State = VCSLockState::Unsupported;
    std::string Owner;
    bool CanAcquire = false;
    bool CanRelease = false;
    bool CanRequest = false;
};

// Base interface for version control system integration
class IVCSIntegration
{
public:
    virtual ~IVCSIntegration() = default;

    // Initialize VCS integration (detect repo, start status tracking)
    // If executable is provided, use it; otherwise auto-detect
    virtual bool Initialize(const std::filesystem::path& projectRoot, 
                           const std::filesystem::path& executable = {}) = 0;

    // Shutdown and cleanup
    virtual void Shutdown() = 0;

    // Check if VCS is available and repository is detected
    virtual bool IsAvailable() const = 0;
    virtual bool IsRepository() const = 0;

    // Get file status
    virtual VCSFileStatus GetFileStatus(const std::filesystem::path& filePath) = 0;

    // VCS operations
    virtual bool Add(const std::filesystem::path& filePath) = 0;
    virtual bool Remove(const std::filesystem::path& filePath) = 0;
    virtual bool Move(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) = 0;
    virtual bool Commit(const std::string& message) = 0;
    virtual bool Update() = 0;  // Update from remote (git pull / svn update)
    virtual bool Revert(const std::filesystem::path& filePath) = 0;

    // File locking. A provider without it answers Unsupported from GetLockInfo,
    // which is what the UI reads before offering any of the three actions — so
    // the refusals below are unreachable rather than a silent failure mode.
    virtual VCSLockInfo GetLockInfo(const std::filesystem::path& filePath) { (void)filePath; return {}; }
    virtual bool AcquireLock(const std::filesystem::path& filePath) { (void)filePath; return false; }
    virtual bool ReleaseLock(const std::filesystem::path& filePath) { (void)filePath; return false; }
    virtual bool RequestLock(const std::filesystem::path& filePath) { (void)filePath; return false; }

    // Check if file is ignored
    virtual bool IsIgnored(const std::filesystem::path& filePath) = 0;

    // Invalidate the status cache and request a poll refresh ASAP, bypassing
    // the normal polling interval.
    // - `path` empty: invalidate the whole cache.
    // - `path` non-empty: invalidate that single entry.
    // In both cases, wake the background poll thread so the snapshot
    // repopulates in ~one git-status duration rather than up to one polling
    // interval. Safe to call from any thread.
    virtual void RefreshStatus(const std::filesystem::path& path = {}) = 0;

    // Get repository root
    virtual std::filesystem::path GetRepositoryRoot() const = 0;

    // Get current branch/revision identifier
    virtual std::string GetCurrentBranch() const = 0;

    // Revision history (whole repository when filePath is empty). Providers
    // without a history primitive return an empty list.
    virtual std::vector<VCSLogEntry> GetLog(const std::filesystem::path& filePath = {},
                                            int maxEntries = 100) = 0;

    // Get the VCS executable path currently in use
    virtual std::filesystem::path GetExecutable() const = 0;

    // Notifies listeners that the status snapshot has changed. Fires once
    // per refresh (not per file) — the listener should treat this as
    // "re-query anything you care about" and fetch current statuses via
    // GetFileStatus. No arguments: the per-file path+status were vestigial
    // after the callback-fanout fix and were always ignored by listeners.
    using StatusChangedCallback = std::function<void()>;
    virtual void SetStatusChangedCallback(StatusChangedCallback callback) = 0;
};

} // namespace GameEngine
