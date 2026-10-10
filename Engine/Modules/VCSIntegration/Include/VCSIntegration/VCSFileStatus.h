#pragma once

namespace GameEngine
{

// Unified file status enum for all version control systems
enum class VCSFileStatus
{
    Unversioned,      // Not tracked by VCS
    Modified,         // Modified but not committed
    Added,            // Added to staging/version control
    Deleted,          // Deleted
    Conflict,         // Merge conflict
    LockedByMe,       // Locked by current user
    LockedByOthers,   // Locked by another user
    ServerHasChanges, // Remote has changes (needs update)
    Ignored,          // Ignored by VCS ignore rules
    Clean,            // No changes
    NotConfigured     // VCS not configured (e.g., no API token for Diversion)
};

} // namespace GameEngine
