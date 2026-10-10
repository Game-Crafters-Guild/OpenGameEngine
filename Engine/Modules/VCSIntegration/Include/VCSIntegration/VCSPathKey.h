#pragma once

#include <filesystem>

namespace GameEngine::VCS
{

// Repo-relative path for status-cache keys, computed lexically.
//
// GetFileStatus implementations run on UI bind paths in batches (the Assets
// panel queues one lookup per newly visible cell), and
// std::filesystem::relative canonicalizes BOTH operands with per-component
// stat syscalls. The inputs here are absolute paths produced by directory
// scans under the repository root, so a lexical computation matches the
// syscall result for them; anything the lexical form cannot express
// (different root, empty result) falls back to the filesystem call to keep
// the old behavior for oddball inputs. Returns an empty path on failure —
// callers treat that as Unversioned.
inline std::filesystem::path RepoRelative(const std::filesystem::path& filePath,
                                          const std::filesystem::path& repoRoot)
{
    if (filePath.empty() || repoRoot.empty())
        return {};

    const std::filesystem::path lexical = filePath.lexically_relative(repoRoot);
    if (!lexical.empty() && *lexical.begin() != "..")
        return lexical.lexically_normal();

    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(filePath, repoRoot, ec);
    if (ec)
        return {};
    return rel;
}

} // namespace GameEngine::VCS
