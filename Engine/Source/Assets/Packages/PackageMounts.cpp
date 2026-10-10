#include "Assets/Packages/PackageMounts.h"

#include "Assets/AssetManager.h"
#include "Logger/Logger.h"

#include <cassert>
#include <filesystem>
#include <memory>
#include <string_view>

namespace GameEngine
{

AssetSourceDesc MakeEmbeddedPackageMount(const ResolvedPackage& package)
{
    AssetSourceDesc desc;
    desc.Alias = package.Alias;
    desc.Root = package.AssetsDir;
    desc.DerivedIdentity = true;
    desc.AuthoritativeDbFile = package.RootDir / "AssetDatabase.assetdb";
    desc.CacheRoot = package.RootDir / ".Cache" / "AssetDatabase";
    desc.Priority = package.Priority;
    desc.RejectOverlappingRoots = true;

    // Published package (SEAM 4): a local/embedded package that ships
    // <AssetsDir>/.assetmanifest — the extraction flow's output — mounts with
    // STORED identity while staying fully mutable (watched, scanned, its own
    // working DB + cache as above). The manifest's {guid, path} records are
    // merged into the working DB at mount and win identity conflicts, so a
    // scene that referenced an asset by its pre-extraction GUID keeps
    // resolving after the file physically moved into the package. Files added
    // later (Explorer copy, editor import) get deterministic DERIVED fallback
    // GUIDs until the manifest is republished — see the identity decision
    // table in PackageMounts.h.
    std::error_code ec;
    const std::filesystem::path manifestFile = package.AssetsDir / ".assetmanifest";
    if (std::filesystem::exists(manifestFile, ec))
    {
        desc.DerivedIdentity = false;
        desc.IdentityManifestFile = manifestFile;
    }
    return desc;
}

// A git or engine package's derived database under the host's asset cache root:
// <hostCacheRoot>/Packages/<alias>/AssetDatabase, cooked textures beside it in
// <hostCacheRoot>/Packages/<alias>/Tex and model LODs in the sibling Lod folder.
// The alias is the resolver's sanitized mount alias, one path segment,
// independent of where the package is installed.
static std::filesystem::path PackageAssetCacheRoot(const std::filesystem::path& hostCacheRoot,
                                                   std::string_view alias)
{
    // An empty root would leave a relative path the registry resolves against the working directory.
    assert(!hostCacheRoot.empty() && "package mounts need the host's asset cache root");
    return hostCacheRoot / "Packages" / alias / "AssetDatabase";
}

// Shape for a package that publishes no .assetmanifest: derived identity needs
// a scan, but the mount stays read-only (no persistence writes) and unwatched —
// it has no store to author into, so import settings are refused with that as
// the reason. IsImmutable stays false: the initial scan registers through the
// same mutation path it would gate.
static AssetSourceDesc MakeUnpublishedPackageMount(const ResolvedPackage& package)
{
    AssetSourceDesc desc;
    desc.Alias = package.Alias;
    desc.Root = package.AssetsDir;
    desc.DerivedIdentity = true;
    desc.Priority = package.Priority;
    desc.AcceptsMetadataWrites = false;
    desc.IsReadOnly = true;
    desc.TracksTombstones = false;
    desc.RegisterFileWatcher = false;
    desc.RejectOverlappingRoots = true;
    return desc;
}

AssetSourceDesc MakeGitPackageMount(const ResolvedPackage& package,
                                    const std::filesystem::path& cacheRoot)
{
    std::error_code ec;
    if (std::filesystem::exists(package.AssetsDir / ".assetmanifest", ec))
    {
        AssetSourceDesc desc = MakePackageMount(package.Alias, package.AssetsDir, package.Priority);
        // A development dependency may ship stored identity without cooked
        // artifacts, so this process cooks misses into its own derived
        // cache. Its settings stay authored: the cache entry takes no writes,
        // so an inferred tag would only be refused. Staged Player content uses
        // MakePackageMount directly and consumes the build's artifacts.
        desc.CooksDerivedArtifactsOnMiss = true;
        desc.CacheRoot = PackageAssetCacheRoot(cacheRoot, package.Alias);
        return desc;
    }
    return MakeUnpublishedPackageMount(package);
}

// The manifest an engine package's metadata belongs in: the one in the tree the
// package is AUTHORED in, never the staged copy. The build restages an engine
// package with copy_directory_if_different on every build of every target that
// consumes it, so an edit written to the staged manifest survives until the next
// build of anything and never reaches the repository. Writing through means the
// edit lands where the package is committed, shows up in `git status`, and comes
// back to the staged tree as part of the next build like every other change to
// package content.
//
// Empty when the staged tree names no authoring tree (a shipped or packaged
// tree, which has no marker), or when that tree publishes no manifest, or when
// the package's assets directory does not sit under its own root — all three
// mount read-only.
static std::filesystem::path AuthoringManifestFile(const ResolvedPackage& package)
{
    if (package.AuthoringRootDir.empty() || package.AssetsDir.empty())
        return {};
    const std::filesystem::path assetsRelative =
        package.AssetsDir.lexically_relative(package.RootDir);
    if (assetsRelative.empty() || *assetsRelative.begin() == "..")
        return {};
    const std::filesystem::path manifest =
        package.AuthoringRootDir / assetsRelative / ".assetmanifest";
    std::error_code ec;
    return std::filesystem::exists(manifest, ec) ? manifest.lexically_normal()
                                                 : std::filesystem::path{};
}

AssetSourceDesc MakeEnginePackageMount(const ResolvedPackage& package,
                                       const std::filesystem::path& cacheRoot)
{
    std::error_code ec;
    if (!std::filesystem::exists(package.AssetsDir / ".assetmanifest", ec))
        return MakeUnpublishedPackageMount(package);

    // MakePackageMount's InfersTextureImportSettings=false carries over to BOTH
    // shapes below, and that is load-bearing for the writable one: the render
    // path's first-material-bind tagger writes inferred colour-space and usage
    // rows through SetMetaValue whenever a mount infers import settings, which
    // on this mount would author a COMMITTED file with no authoring action —
    // opening a scene, or running a developer Player, would dirty the checkout.
    // A package's import settings are authored through the inspector and
    // committed with the package; the ones a bind would have inferred are rows
    // in its manifest already.
    //
    // Cooking is the opposite answer on both shapes. An engine package is only
    // ever mounted by a process built from the engine's own tree — an editor or
    // a developer Player, encoder-equipped, with a derived cache of its own
    // outside the staged package. Refusing the cook there would upload every one
    // of the package's textures raw and mip-less for the life of every session,
    // because nothing else ever fills that cache: the export bakes into the
    // packaged game's tree, not this one.
    AssetSourceDesc desc = MakePackageMount(package.Alias, package.AssetsDir, package.Priority);
    desc.CooksDerivedArtifactsOnMiss = true;
    // Derived artifacts belong to the running host, outside staged or signed content.
    desc.CacheRoot = PackageAssetCacheRoot(cacheRoot, package.Alias);

    const std::filesystem::path authoringManifest = AuthoringManifestFile(package);
    if (authoringManifest.empty())
        return desc; // Staged content with no authoring tree behind it.

    // An engine package in a developer build is a SOURCE package: the import
    // settings of the files its manifest declares are editable, and they are
    // written to that manifest in the tree the package is authored in. The asset
    // SET stays frozen — IsImmutable keeps registration, unregistration and
    // rename refused, which is what protects a stored-identity row that nothing
    // can recreate. Root stays the STAGED assets directory: the staged tree is
    // what runs (it carries the build's prebuilt module binaries); only the
    // store moves, and the derived cache stays with the running host.
    //
    // Every editor and developer Player built from this checkout resolves the
    // same authoring manifest, whatever staged tree each one runs from, so the
    // store is a cross-process writer and says so.
    desc.AcceptsMetadataWrites = true;
    desc.IsReadOnly = false;
    desc.AuthoritativeDbFile = authoringManifest;
    desc.StoreIsSharedAcrossProcesses = true;
    return desc;
}

std::vector<std::string> MountResolvedPackages(AssetManager& assetManager,
                                               const PackageResolution& resolution,
                                               const std::filesystem::path& cacheRoot)
{
    for (const auto& error : resolution.Errors)
        Logger::Log::Error("Packages: {}", error);
    for (const auto& warning : resolution.Warnings)
        Logger::Log::Warning("Packages: {}", warning);

    std::vector<std::string> mounted;
    mounted.reserve(resolution.MountOrder.size());
    for (const ResolvedPackage& package : resolution.MountOrder)
    {
        // Code-only package (manifest declared "assets": "") — no mount, by design.
        if (package.AssetsDir.empty())
            continue;

        std::error_code ec;
        if (!std::filesystem::is_directory(package.AssetsDir, ec))
        {
            Logger::Log::Warning("Packages: '{}' has no assets dir at '{}'; nothing to mount",
                                 package.Manifest.Name, package.AssetsDir.string());
            continue;
        }

        // Three shapes, by where the package's tree comes from: a git package
        // lives in the immutable content-addressed cache, an engine package is
        // staged from the engine's own source tree, everything else is a
        // directory the project owns.
        AssetSourceDesc desc;
        switch (package.SourceKind)
        {
        case PackageSourceKind::Git:    desc = MakeGitPackageMount(package, cacheRoot); break;
        case PackageSourceKind::Engine: desc = MakeEnginePackageMount(package, cacheRoot); break;
        default:                        desc = MakeEmbeddedPackageMount(package); break;
        }
        if (!assetManager.RegisterSource(desc))
        {
            Logger::Log::Error("Packages: failed to mount '{}' (alias '{}', root '{}') — see preceding "
                               "registry error; package assets unavailable",
                               package.Manifest.Name, desc.Alias, desc.Root.string());
            continue;
        }
        Logger::Log::Info("Packages: mounted '{}' {} as '{}' (priority {}) from '{}'",
                          package.Manifest.Name, package.Manifest.Version.ToString(), desc.Alias,
                          desc.Priority, desc.Root.string());
        mounted.push_back(desc.Alias);
    }

    for (const ResolvedPackage& package : resolution.Disabled)
    {
        Logger::Log::Info("Packages: '{}' is disabled; not mounted", package.Manifest.Name);
    }
    return mounted;
}

void UnmountPackageSources(AssetManager& assetManager, const std::vector<std::string>& aliases,
                           Function<void()> onUnmounted)
{
    // Each source finishes when the loads it still had in flight resolve, so
    // they land in any order — including inside their own Begin call. A shared
    // count is what makes "all of them" observable from whichever lands last;
    // the extra arrival at the end covers the case where every source finishes
    // inline before the loop is done.
    struct Remaining
    {
        size_t Count = 0;
        Function<void()> OnUnmounted;
    };
    auto remaining = std::make_shared<Remaining>();
    remaining->Count = aliases.size() + 1;
    remaining->OnUnmounted = std::move(onUnmounted);
    auto arrive = [remaining]()
    {
        if (--remaining->Count == 0 && remaining->OnUnmounted)
            remaining->OnUnmounted();
    };

    for (const std::string& alias : aliases)
    {
        const bool started = assetManager.BeginUnregisterSource(
            alias,
            [arrive, alias](bool ok)
            {
                if (!ok)
                    Logger::Log::Warning("Packages: unmounting source '{}' did not complete", alias);
                arrive();
            });
        if (!started)
        {
            Logger::Log::Warning("Packages: failed to unmount source '{}'", alias);
            arrive();
        }
    }
    arrive();
}

} // namespace GameEngine
