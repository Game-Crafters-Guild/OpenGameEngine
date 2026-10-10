#pragma once
#include "Assets/AssetManager.h"
#include "../Assets/AssetPathSyntax.h"
#include "Logger/Logger.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

namespace GameEngine::Detail
{
inline std::filesystem::path ResolveEngineShaderPath(AssetManager& am, const std::filesystem::path& relativePath)
{
    // Authored source URLs select that source exactly. Parse before pinning
    // ordinary engine names, and never fall back from a missing/unknown alias.
    std::string sourceAlias;
    std::filesystem::path sourcePath;
    if (AssetPathSyntax::TrySplitAssetSourcePrefix(relativePath, sourceAlias, sourcePath))
    {
        const auto resolved = am.ResolveAssetPath(sourcePath, sourceAlias);
        std::error_code ec;
        return !resolved.empty() && std::filesystem::exists(resolved, ec) ? resolved : std::filesystem::path{};
    }

    // Engine shaders ship with the editor / runtime executable, not with the
    // user's project content. Resolving via the unscoped path falls back to
    // the project root first, which silently picks up stale shaders staged
    // in a sibling project tree (e.g. when the editor is launched from a
    // worktree but auto-detects a different project root). Pin the resolution
    // to the "editor" mount so engine shaders always come from the same
    // executable's staged Assets directory.
    auto resolved = am.ResolveAssetPath(relativePath, kAssetSourceAliasEditor);
    if (!resolved.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec))
            return resolved;
    }
    // When an editor mount exists, a shader it does not hold is MISSING — the
    // project-first fallback would probe the project root, which is both the
    // stale-sibling-shader hazard above and, on web, an OPFS touch: a sync
    // filesystem call on the main thread proxies to the WasmFS worker, and the
    // browser's no-Atomics.wait rule turns that wait into a busy-spin. With
    // the OPFS worker contended at boot, that spin never returns — this exact
    // fallthrough (probed every frame by shader-less retry loops) was the
    // 1-in-3 web boot wedge. This is why the resolution is spelled out here
    // rather than through ResolveAssetPathPreferringSource, whose fallback is
    // exactly the probe being avoided.
    if (!am.GetSourceRoot(kAssetSourceAliasEditor).empty())
    {
        // Returning empty is the whole answer the caller gets, and most callers degrade in
        // silence — a feature just never appears. Name the shader once so a missing staged
        // file is a line in the log rather than an absence to be inferred. Once per name,
        // not per call: the render-graph declare path asks again every frame.
        //
        // Thread safety: shader paths resolve on the render thread and on material-compile
        // workers, so the seen-set takes its own mutex. It is function-local and never
        // cleared — the set is bounded by the number of distinct engine shader names, and a
        // name that starts resolving again simply stops reaching this branch.
        static std::mutex reportedMutex;
        static std::unordered_set<std::string> reported;
        const std::string name = relativePath.generic_string();
        bool firstTime = false;
        {
            std::lock_guard<std::mutex> lock(reportedMutex);
            firstTime = reported.insert(name).second;
        }
        if (firstTime)
            Logger::Log::Warning(
                "Engine shader '{}' does not resolve in the editor mount at '{}'. A caller "
                "with no fallback of its own gets nothing back; check that the build staged "
                "it under that root and that the name is spelled relative to it.",
                name, am.GetSourceRoot(kAssetSourceAliasEditor).string());
        return {};
    }
    // Setups without an "editor" mount still resolve project-first: unit tests,
    // Player builds before they register their own editor source, and packaged
    // games, which fuse the engine shaders into the project mount instead.
    return am.ResolveAssetPath(relativePath);
}

// ShaderReflect where the build stages it: in the Tools folder next to the Apps
// folder that holds the executable (bin/<Config>/Tools beside bin/<Config>/Apps,
// through an app bundle too). Empty when no such file exists, as for a packaged
// game, which ships no tools.
inline std::filesystem::path FindStagedShaderReflect(const std::filesystem::path& executableDirectory)
{
#if defined(_WIN32)
    constexpr const char* kShaderReflectFile = "ShaderReflect.exe";
#else
    constexpr const char* kShaderReflectFile = "ShaderReflect";
#endif
    for (std::filesystem::path directory = executableDirectory; directory.has_relative_path();
         directory = directory.parent_path())
    {
        if (directory.filename() != "Apps")
            continue;
        const std::filesystem::path tool = directory.parent_path() / "Tools" / kShaderReflectFile;
        std::error_code ec;
        return std::filesystem::is_regular_file(tool, ec) ? tool : std::filesystem::path{};
    }
    return {};
}

// What to rebuild when a shader package of another format version is refused,
// chosen by the source its name selects, parsed as ResolveEngineShaderPath parses
// it: unprefixed names and the editor source are the build's staged packages,
// rebuilt by the CompileShaderPkgs target; a project or other source, or an
// absolute path, is a package its owner cooks with ShaderReflect, named by the
// path the build staged it at beside `executableDirectory` when it is there.
// EngineCore installs it, with the running executable's directory, as the
// package loaders' rebuild action (Rendering::SetShaderPackageRebuildAction).
inline std::string ShaderPackageRebuildAction(const std::string& packageName,
                                              const std::filesystem::path& executableDirectory)
{
    std::string sourceAlias;
    std::filesystem::path sourcePath;
    const bool hasSource = AssetPathSyntax::TrySplitAssetSourcePrefix(packageName, sourceAlias, sourcePath);
    const bool enginePackage = hasSource ? sourceAlias == kAssetSourceAliasEditor
                                        : !std::filesystem::path(packageName).is_absolute();
    const std::string resolved =
        std::filesystem::path(Rendering::ResolveShaderPkgPath(packageName)).generic_string();
    if (enginePackage)
        return "Rebuild the staged shader packages with build target CompileShaderPkgs "
               "to replace \"" + resolved + "\".";
    const std::filesystem::path shaderReflect = FindStagedShaderReflect(executableDirectory);
    const std::string tool = shaderReflect.empty()
                                 ? std::string("this SDK's ShaderReflect")
                                 : "this SDK's ShaderReflect \"" + shaderReflect.generic_string() + "\"";
    return "Rebuild this package from its shader sources with " + tool +
           " (--vs/--fs/--cs <compiled-stage-files> --out \"" + resolved + "\").";
}

} // namespace GameEngine::Detail
