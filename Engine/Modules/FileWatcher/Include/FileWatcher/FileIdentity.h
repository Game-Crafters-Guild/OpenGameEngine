#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
/**
 * @brief Filesystem identity of a file: the volume it lives on and its index
 *        within that volume (NTFS file index, POSIX device + inode).
 *
 * The identity survives renames and in-place rewrites of the same file and
 * changes when a path is replaced by a different file (a safe-save renames a
 * temp file over the target), so it tells a renamed file from a
 * deleted-then-created one where neither the path nor the timestamp can.
 */
struct FileIdentity
{
    uint64 Volume = 0;
    uint64 Index = 0;
    bool Valid = false;

    bool operator==(const FileIdentity& other) const = default;

    /// Persistent text form; asset fingerprints record and compare this string.
    std::string ToString() const;
};

/// Identity of the file at `path`; invalid when the file cannot be opened.
FileIdentity ReadFileIdentity(const std::filesystem::path& path);
} // namespace GameEngine

template <>
struct std::hash<GameEngine::FileIdentity>
{
    std::size_t operator()(const GameEngine::FileIdentity& identity) const noexcept
    {
        // Volume counts are tiny and indices dense; the odd multiplier spreads
        // the volume across the word so two volumes' index runs do not collide.
        constexpr std::uint64_t kVolumeSpread = 0x9E3779B97F4A7C15ull;
        return std::hash<std::uint64_t>{}(identity.Index ^ (identity.Volume * kVolumeSpread));
    }
};
