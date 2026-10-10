#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace GameEngine::AssetPaths
{

// Map every file under `root` from its ASCII-folded relative path to its real
// on-disk path.
//
// A package manifest is a git-committed artifact whose record paths are
// platform-folded by whichever host published it (lowercase on Windows/macOS),
// while the files it names keep the author's case. On a case-sensitive
// filesystem the folded path then names nothing on disk. This index maps the
// folded spelling back to the real one.
//
// Keys are the relative path in generic form, ASCII-lowercased — the same fold
// the publisher applied. Two files differing only in case collide; the first
// one the directory walk reaches wins, which matches the manifest's own
// one-record-per-folded-path shape. An unreadable root yields an empty index.
std::unordered_map<std::string, std::filesystem::path> BuildFoldedPathIndex(
    const std::filesystem::path& root);

} // namespace GameEngine::AssetPaths
