#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Editor::Startup
{
// Counters for one seeding/refresh pass over the user-writable editor assets.
struct BuiltinSyncStats
{
    size_t Copied = 0;           // engine-owned files written (new or changed)
    size_t SkippedIdentical = 0; // engine-owned files already byte-identical in dst
    size_t RemovedStale = 0;     // previously shipped files pruned from dst
};

// An engine-owned subtree of the install assets that is force-refreshed into the
// user copy (the user copy is what the runtime reads on platforms with a
// writable mirror, so stale engine files there shadow shipped updates).
struct OwnedTreeSpec
{
    std::filesystem::path RelativeRoot;  // e.g. "Shaders"
    const char* AssetKind = "asset";     // for error messages
    bool Recursive = true;
    std::vector<std::string> Extensions; // empty = all files; matched case-insensitively
};

// Copy files present under srcRoot but missing under dstRoot. Never overwrites,
// so user edits to seeded files survive.
bool CopyMissingFilesRecursive(const std::filesystem::path& srcRoot,
                               const std::filesystem::path& dstRoot,
                               std::string* outError);

// Mirror the shipped files of an engine-owned subtree into dst, overwriting stale
// copies. Byte-identical files are skipped so unchanged assets keep their mtimes
// (rewriting them would invalidate downstream caches every launch). Files that
// exist only in dst are left alone. Each shipped file's dst-relative generic path
// is appended to outShippedFiles for manifest tracking.
bool RefreshOwnedTree(const std::filesystem::path& srcRoot,
                      const std::filesystem::path& dstRoot,
                      const OwnedTreeSpec& spec,
                      std::vector<std::string>& outShippedFiles,
                      BuiltinSyncStats& stats,
                      std::string* outError);

// The prune manifest of the mirror at mirrorRoot: <mirrorRoot>.builtin-manifest,
// beside the mirror. A manifest records what its mirror received, so each mirror
// keeps its own: editors pointed at different mirrors under one user data root
// (GE_EDITOR_ASSETS_ROOT) never prune a mirror against another mirror's history.
std::filesystem::path BuiltinManifestPathFor(const std::filesystem::path& mirrorRoot);

// Delete files recorded in the previous manifest that are no longer shipped, then
// rewrite the manifest with currentShippedFiles. Only manifest-recorded paths are
// ever deleted, so user-created files are never touched; a missing or corrupt
// manifest prunes nothing. Returns false only when the new manifest cannot be
// written.
bool PruneStaleOwnedFiles(const std::filesystem::path& dstRoot,
                          const std::filesystem::path& manifestPath,
                          const std::vector<std::string>& currentShippedFiles,
                          BuiltinSyncStats& stats,
                          std::string* outError);
} // namespace GameEngine::Editor::Startup
