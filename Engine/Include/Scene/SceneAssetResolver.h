#pragma once

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include <filesystem>
#include <string_view>

namespace GameEngine::Scene
{
// Scene IO can run in tools/tests without Engine. When available, callers can provide
// an asset resolver to make GUID/path resolution rename-safe and asset-root-aware.
class ISceneAssetResolver
{
  public:
    virtual ~ISceneAssetResolver() = default;

    // Return the project's asset root. Save relativizes paths against it; load anchors a
    // relative path to it only when ResolveAssetPath yields nothing. May be empty; callers
    // can also override per-call via options.
    virtual std::filesystem::path GetAssetRoot() const = 0;

    // Map an authored asset path — relative to a mount root, or pinned to one mount with an
    // `alias:` prefix — to the absolute file that supplies it, searching mounts in the
    // engine's order: project first, then the other registered sources. This is what lets a
    // scene shipped on the editor mount bind its editor-mount assets while a project asset at
    // the same relative path still wins. Absolute paths pass through.
    //
    // A non-empty answer is NOT a promise that the file exists: an implementation may name
    // its own primary-root candidate when no mount supplies the path, and the engine's
    // AssetManager does exactly that. Empty means the resolver could not name a candidate
    // at all — an unregistered `alias:` prefix, or no root to anchor to.
    virtual std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const = 0;

    // If the asset system supports GUID aliasing/redirects, normalize to the canonical GUID.
    // Default implementations may simply return the input.
    virtual GUID ResolveGuid(const GUID& guid) const = 0;

    // Derive or retrieve a stable GUID for a file on disk.
    virtual GUID GetOrCreateAssetGuid(const std::filesystem::path& absolutePath) = 0;

    // Query canonical on-disk path and asset type from a GUID.
    virtual bool TryGetPathAndType(const GUID& guid, std::filesystem::path& outPath, AssetType& outType) const = 0;

    // Query GUID and asset type from an absolute path.
    virtual bool TryGetGuidAndType(const std::filesystem::path& absolutePath, GUID& outGuid, AssetType& outType) const = 0;
};

} // namespace GameEngine::Scene

