#include "Startup/EditorPathPolicy.h"

#include "Startup/BuiltinAssetSync.h"

#include "Core/Application.h" // PathUtils
#include "Editor/EditorPaths.h"

#include <vector>

namespace GameEngine::Editor::Startup
{
std::filesystem::path GetEditorInstallAssetsRoot()
{
    return GameEngine::Editor::GetEditorGlobalPaths().installAssetsRoot;
}

std::filesystem::path GetEditorUserCacheRoot()
{
    return GameEngine::Editor::GetEditorGlobalPaths().userCacheRoot;
}

std::filesystem::path GetDefaultProjectRoot()
{
    return GameEngine::Editor::GetEditorGlobalPaths().defaultProjectRoot;
}

std::filesystem::path GetEditorUserAssetsRoot()
{
    return GameEngine::Editor::GetEditorGlobalPaths().userAssetsRoot;
}

bool EnsureEditorUserAssetsSeeded(std::string* outError, BuiltinSyncStats* outStats)
{
    const auto src = GetEditorInstallAssetsRoot();
    const auto dst = GetEditorUserAssetsRoot();

    // If the user assets root already exists, copy new defaults without removing
    // custom extra files. Shipped render pipelines and shaders are engine-owned and
    // force-refreshed (byte-identical copies are skipped so unchanged assets keep
    // their mtimes). On macOS, UI is refreshed as well because the runtime mounts a
    // writable mirror of the signed bundle; leaving an older built-in there makes an
    // upgraded editor execute stale UI. Files a previous install shipped that the
    // current one no longer does are pruned via the manifest beside the mirror, so
    // removed builtins cannot linger in the mirror and shadow current behavior.
    BuiltinSyncStats stats;
    std::vector<std::string> shipped;
    bool ok = CopyMissingFilesRecursive(src, dst, outError) &&
              RefreshOwnedTree(src, dst,
                               {.RelativeRoot = "RenderPipelines",
                                .AssetKind = "render pipeline",
                                .Recursive = false,
                                .Extensions = {".rendergraph", ".renderpipeline"}},
                               shipped, stats, outError) &&
              RefreshOwnedTree(src, dst,
                               {.RelativeRoot = "Shaders", .AssetKind = "shader"},
                               shipped, stats, outError);
#if defined(__APPLE__)
    if (ok)
    {
        /* The whole mirror is engine-owned: every launch byte-compares it
           against the freshly built bundle, so a build is enough to make the
           next editor session current — no "delete Application Support"
           hammer needed. Files the user added that the bundle never shipped
           are untouched (the prune removes only manifest-listed builtins). */
        ok = RefreshOwnedTree(src, dst,
                              {.RelativeRoot = "", .AssetKind = "editor asset"},
                              shipped, stats, outError);
    }
#endif
    if (ok)
    {
        ok = PruneStaleOwnedFiles(dst, BuiltinManifestPathFor(dst), shipped, stats, outError);
    }
    if (outStats)
    {
        *outStats = stats;
    }
    return ok;
}
} // namespace GameEngine::Editor::Startup
