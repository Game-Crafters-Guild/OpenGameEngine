#pragma once

#include "AssetCore/PathNormalization.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace GameEngine {

// Identity key for a filesystem path, used to compare paths that reach the
// asset browser in different spellings: registry asset paths are folded by
// NormalizeForRegistryKey, while mount roots and file-watch events name a
// directory or file in its on-disk case. Keying both sides through the
// registry's own normalizer is what makes them meet — an unfolded comparison
// silently never matches, and the browser stops reacting to file changes.
//
// Case folding applies on every platform: this key answers "same asset?", which
// the registry defines case-insensitively everywhere, and the result is only
// ever compared, never used to open a file.
inline std::string AssetPathKey(const std::filesystem::path& path)
{
    if (path.empty())
        return std::string();

    std::string key;
    if (path.is_absolute())
    {
        // An already-absolute path needs no current-directory resolution.
        key = AssetPaths::NormalizeForRegistryKey(path);
    }
    else
    {
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(path, ec);
        key = AssetPaths::NormalizeForRegistryKey(ec ? path : abs);
    }

    // A directory spelled with a trailing separator keys the same as the same
    // directory without one — lexically_normal keeps that separator, and the
    // two spellings otherwise compare unequal. Roots ("c:/", "/") are left
    // alone: there the separator is the path, not a spelling of one.
    while (key.size() > 1 && key.back() == '/' && key[key.size() - 2] != ':')
        key.pop_back();
    return key;
}

// True when `item` sits directly inside `dir` — one level down, not `dir`
// itself. Purely lexical: create and rename events name paths that may not
// exist yet, so this must never consult the filesystem.
inline bool IsDirectChildOfAssetDir(const std::filesystem::path& dir,
                                    const std::filesystem::path& item)
{
    if (dir.empty() || item.empty())
        return false;
    const std::filesystem::path parent = item.parent_path();
    if (parent.empty())
        return false;
    return AssetPathKey(parent) == AssetPathKey(dir);
}

// True when `descendantKey` lies beneath `dirKey` at any depth — not `dirKey`
// itself. Whole path segments are compared, so ".../Rock" does not contain
// ".../Rocks". Takes keys so a caller testing many paths against one directory
// keys the directory once.
inline bool IsUnderAssetDirKey(std::string_view dirKey, std::string_view descendantKey)
{
    if (dirKey.empty() || descendantKey.size() <= dirKey.size())
        return false;
    if (descendantKey.compare(0, dirKey.size(), dirKey) != 0)
        return false;
    return dirKey.back() == '/' || descendantKey[dirKey.size()] == '/';
}

// True when `descendant` lies beneath `dir` at any depth — not `dir` itself.
inline bool IsUnderAssetDir(const std::filesystem::path& dir,
                            const std::filesystem::path& descendant)
{
    if (dir.empty() || descendant.empty())
        return false;
    return IsUnderAssetDirKey(AssetPathKey(dir), AssetPathKey(descendant));
}

// Follow a browsed directory through a rename of itself or one of its
// ancestors, returning where that directory now lives, or nullopt when the
// rename did not move it. Lexical for a sharper reason than the above: once
// the rename has landed `oldPath` no longer exists, so equivalent() and
// relative() report failure rather than an answer. The tail is rebuilt from
// `dir`'s own spelling, which is what the browser navigates with.
inline std::optional<std::filesystem::path> RebaseRenamedAssetDir(
    const std::filesystem::path& dir,
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath)
{
    if (dir.empty() || oldPath.empty() || newPath.empty())
        return std::nullopt;

    const std::string oldKey = AssetPathKey(oldPath);
    if (oldKey.empty())
        return std::nullopt;

    std::filesystem::path tail;
    for (std::filesystem::path probe = dir; !probe.empty(); probe = probe.parent_path())
    {
        if (AssetPathKey(probe) == oldKey)
            return tail.empty() ? newPath : newPath / tail;
        if (probe == probe.parent_path())
            break;
        tail = tail.empty() ? probe.filename() : probe.filename() / tail;
    }
    return std::nullopt;
}

} // namespace GameEngine
