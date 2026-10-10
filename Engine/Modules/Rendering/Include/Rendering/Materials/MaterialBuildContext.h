#pragma once

// MaterialBuildContext: the pre-resolved filesystem environment for one
// material compile. The Rendering module never talks to the asset system —
// the caller (Engine/Editor/Player) resolves every directory below via
// AssetManager and hands the context in as a VALUE snapshot, so compiles on
// worker threads never touch live mount state.

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

struct MaterialBuildContext
{
    // The engine's built-in shader tree (the editor mount's Shaders/ dir).
    // Home of the adapter templates — adapters always come from here and are
    // never package-overridable — and the FINAL fallback root for surface /
    // vertex-modifier references.
    std::filesystem::path AdapterShaderDir;

    // Source ROOT(s) of the open project mount (not Shaders/ subdirs — a
    // project's Shaders/ dir already rides PackageShaderDirs). Consulted for
    // surface / vertex-modifier references right after the material's own
    // directory, so a project-relative reference still resolves against the
    // LIVE project root when the material's asset path could not be derived
    // (cache-driven compiles) instead of silently collapsing to the engine
    // tree. Derived from the asset mount table by CollectProjectRoots.
    std::vector<std::filesystem::path> ProjectRoots;

    // One mounted asset source: its alias ("project", "editor", a package's
    // alias) and its root directory.
    struct AssetSourceRoot
    {
        std::string Alias;
        std::filesystem::path Root;
    };

    // Every mounted asset source: the project, the editor mount and each
    // package. Derived data named after a material (a graph material's
    // generated surface) uses "<alias>/<path under the root>" from the deepest
    // root holding the material, which stays the same when the tree moves.
    // Derived from the asset mount table by CollectAssetSourceRoots; an
    // offline cook lists its --project and --scan roots.
    std::vector<AssetSourceRoot> AssetSourceRoots;

    // Shaders/ directories of mounted packages, highest mount priority first.
    // Shader references ("Surfaces/x.glsl", "VertexModifiers/y.glsl") resolve
    // materialDir -> ProjectRoots -> PackageShaderDirs (in order) ->
    // AdapterShaderDir, so a package can ship shaders under
    // <assetsDir>/Shaders/ without materials knowing where the file physically
    // lives. Derived from the asset mount table (the single authority) by
    // CollectPackageShaderDirs.
    std::vector<std::filesystem::path> PackageShaderDirs;

    std::filesystem::path CacheRoot;
    std::vector<std::filesystem::path> IncludeDirs;
};

} // namespace Rendering
} // namespace GameEngine
