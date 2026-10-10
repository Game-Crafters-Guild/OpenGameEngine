#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine {

class AssetManager;
struct ResolvedPackage;

/// A single entry in the build asset manifest.
struct AssetManifestEntry
{
    GUID guid;
    AssetType type = AssetType::Unknown;
    std::filesystem::path sourcePath;  // absolute path to editor-format file
    std::filesystem::path outputPath;  // relative path in output folder (e.g. "Assets/Textures/foo.png")
    uint64_t contentHash = 0;          // file size + mtime based (baking hooks may key off it)
    // Owning asset-source alias ("project", "editor", or a package alias).
    // Package-owned entries stage under Packages/<alias>/Assets/ and are written
    // into that package's own .assetmanifest — each mount stays an independent
    // GUID namespace in the shipped game. Empty is treated as "project".
    std::string sourceAlias;

    // Future baking fields (reserved, not populated yet):
    // std::string processorId;        // e.g. "TextureCompressor", "MeshOptimizer"
    // std::filesystem::path bakedPath; // populated by processor, replaces sourcePath during copy
};

/// Collection of assets to include in a build.
struct AssetManifest
{
    std::vector<AssetManifestEntry> entries;

    // GUIDs the dependency walk referenced but could not resolve to metadata
    // (typically assets of a disabled or unresolved package — their mounts are
    // absent). Build validation attributes these to their owning package and
    // fails loudly instead of shipping a scene with dangling references.
    std::vector<GUID> unresolvedDependencies;

    const AssetManifestEntry* FindByGuid(const GUID& guid) const;
    bool ContainsGuid(const GUID& guid) const;
};

/// Collects assets required for a game build by walking the dependency graph
/// transitively from scene roots.
///
/// Design notes:
/// - AssetRegistry::GetDependencies() returns only direct deps. This class
///   walks recursively with cycle detection to build the transitive closure.
/// - C# scripts and native code are NOT scanned for asset references (blind
///   spot): a package lists what its code loads under runtimeAssets
///   (CollectPackageRuntimeAssets); a project references it from a built scene.
/// - Engine shaders are not GUID-tracked and must be copied separately
///   by the BuildPipeline (not via AssetCollector).
/// - The manifest includes content hashes for future incremental build support.
///   A baking step registered at PreAssetCopy can rewrite manifest entries.
class AssetCollector
{
public:
    explicit AssetCollector(AssetManager& assetManager);

    /// Build a manifest by walking transitive dependencies from scene roots.
    AssetManifest CollectFromScenes(const std::vector<std::string>& scenePaths);

    /// Fallback: include all assets from the PROJECT source only (never the editor
    /// mount). Use when dependency tracking is incomplete (script-loaded assets, etc.).
    AssetManifest CollectAllAssets();

    // Runtime auto-import can need the default without an authored dependency
    // edge. Append it after project fallback collection, with its real identity.
    bool CollectRuntimeHumanoidProfile(AssetManifest& manifest);

    /// Append the default game UI font (GameUIHost::kDefaultFontPath). The Player reads it by
    /// path from its asset root with no authored dependency edge, and once its font resolver
    /// is installed there is no system-font fallback, so a game without it draws no text. It
    /// is the editor's font unless the project supplies its own copy at that path, so the path
    /// is resolved across the mounts like the render pipeline. Already collected is true; a
    /// path no mount supplies, or one that is not shippable content, is false.
    bool CollectDefaultUIFont(AssetManifest& manifest);

    /// Append the render pipeline game.config names, with the assets it references by
    /// GUID or by path, whichever way the rest of the manifest was collected. The Player
    /// resolves that asset path through the shipped .assetmanifest, and the pipeline is
    /// usually the editor's default rather than a project asset, so the path is resolved
    /// across the mounts (a project copy shadows the editor's). An empty path is nothing
    /// to collect (true); a path no mount supplies, or one that is not shippable content,
    /// is false.
    bool CollectRenderPipeline(const std::string& renderPipelinePath, AssetManifest& manifest);

    /// The components the manifest's scenes and blueprints assign fields of
    /// (SceneAssetParser::CollectComponentNames over each Scene entry's file).
    static std::unordered_set<std::string> CollectComponentNames(const AssetManifest& manifest);

    /// Append what one enabled package ships beyond the assets the scenes reach,
    /// each with its own dependency closure: every shippable asset under its
    /// Shaders/ folder (a material names its shaders by path under each source's
    /// Shaders/, which gives the walk no edge), and the files and folders its
    /// manifest's runtimeAssets lists for each component in `usedComponents` (what
    /// its code loads by path). Returns the listed paths no asset of the mounted
    /// package matched, as "<component>: <path>", for the build to warn about.
    std::vector<std::string> CollectPackageRuntimeAssets(const ResolvedPackage& package,
                                                         const std::unordered_set<std::string>& usedComponents,
                                                         AssetManifest& manifest);

    /// Output-layout mapping for a package-owned asset: Packages/<alias>/Assets/<rel>.
    /// Per-alias subdirectories are what make same-relative-path assets from
    /// different mounts collision-free in the staged output, and each package's
    /// Assets/ dir is a sibling of the project's Assets/ so the shipped package
    /// mounts never nest (RejectOverlappingRoots holds in the Player).
    static std::filesystem::path PackageAssetOutputPath(std::string_view alias,
                                                        const std::filesystem::path& relPath);

    /// True if a path may ship in a redistributable game bundle. Excludes C++/C#
    /// source, project/build files (.csproj/.sln/obj/bin), dotfiles (.DS_Store),
    /// and any path with an Editor/ segment — the editor-only asset folder
    /// convention (any folder named Editor/ under a project's or package's asset
    /// root is mounted in the editor but never staged). Shader sources are
    /// content and pass. Callers pass source-root-RELATIVE paths so the Editor/
    /// deny scopes to segments under the owning root. Shared by the collector
    /// and BuildPipeline's last-resort raw directory walk.
    static bool IsShippableContent(const std::filesystem::path& path);

private:
    struct SourceRootEntry
    {
        std::string Alias;
        std::filesystem::path Root;
    };

    void WalkDependencies(const GUID& root, AssetManifest& manifest);
    bool TryAddAsset(const GUID& guid, AssetManifest& manifest);
    static uint64_t ComputeContentHash(const AssetMetadata& meta);

    AssetManager& m_AssetManager;
    // Project-source root: anchor for relative metadata paths.
    std::filesystem::path m_ProjectAssetRoot;
    // All registered sources, so absolute paths from any mount (project,
    // packages, editor) relativize against their own root — the deepest
    // matching root wins — and the entry records its owning alias.
    std::vector<SourceRootEntry> m_SourceRoots;
};

} // namespace GameEngine
