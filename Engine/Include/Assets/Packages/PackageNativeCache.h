#pragma once

#include <filesystem>
#include <memory>

namespace GameEngine::FileSystem
{
class ScopedFileLock;
}

namespace GameEngine
{
/// Pin a managed native cache entry, recording the original package-cache root that owns its scope.
/// Prune inactive generations and scopes whose original roots have been removed.
/// The returned lease must outlive every build and loaded module using the entry.
/// Unsupported platforms, unsafe paths and lock failures return null without deleting data.
std::unique_ptr<FileSystem::ScopedFileLock> AcquirePackageNativeCache(
    const std::filesystem::path& entry, const std::filesystem::path& cacheRoot);

/// Keep an entry owned by GlobalPackageCacheRoot pinned until process termination, including project switches.
bool RetainPackageNativeCache(const std::filesystem::path& entry);
}
