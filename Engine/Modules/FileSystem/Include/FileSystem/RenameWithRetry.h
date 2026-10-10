#pragma once

#include <filesystem>
#include <functional>
#include <system_error>

namespace GameEngine::FileSystem
{
/// Rename a file or directory, retrying only permission/busy failures that a
/// briefly held handle can cause. At most five attempts, with 200 ms between
/// attempts. Persistent permission/busy failures block the calling thread for
/// about 0.8 s. Leaves the last failure in error; cancellation stops the rename.
bool RenameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to,
                     const std::function<bool()>& shouldCancel, std::error_code& error);
} // namespace GameEngine::FileSystem
