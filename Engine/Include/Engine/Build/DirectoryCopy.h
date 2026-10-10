#pragma once

#include <filesystem>
#include <functional>
#include <system_error>

namespace GameEngine {

/// Copies the directory tree at `from` into `to`, creating `to`.
///
/// Symbolic links are copied as links with their stored target and are never
/// followed, so a macOS framework (`Versions/Current -> A`) or a versioned
/// shared-library chain (`libfoo.so -> libfoo.so.1`) keeps its layout, and a
/// link pointing outside `from` never causes a write outside `to`.
/// `shouldCancel` is polled before each entry. `shouldSkip` receives its lexical
/// path relative to `from`; skipping a directory skips its whole subtree.
///
/// Returns false on the first failure, with the error in `ec`, or with `ec`
/// clear when cancelled. A partial copy is left for the caller to remove.
bool CopyDirectoryKeepingLinks(const std::filesystem::path& from,
                               const std::filesystem::path& to,
                               std::error_code& ec,
                               const std::function<bool()>& shouldCancel = {},
                               const std::function<bool(const std::filesystem::path&)>& shouldSkip = {});

} // namespace GameEngine
