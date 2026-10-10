#pragma once

#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackageResolver.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine
{

class AssetManager;

// ---------------------------------------------------------------------------
// Package identity decision table (SEAM 4 — stored-identity publishing).
//
// A package is "published" when <AssetsDir>/.assetmanifest exists (written by
// PublishPackageAssetManifest, or staged by the build pipeline). The manifest
// carries {guid, path} verbatim; for an extracted package those are the
// ORIGINAL project-derived GUIDs of the moved files, which is what keeps
// pre-extraction scene references resolving.
//
//   Mount shape                | Identity for      | File added to Assets/
//                              | manifest files    | without a manifest entry
//   ---------------------------+-------------------+--------------------------
//   Embedded, no manifest      | derived           | derived (normal dev flow),
//     (plain dev package)      |  <alias>/<rel>    |  persisted to pkg .assetdb
//   Embedded, manifest         | STORED (manifest  | DERIVED FALLBACK
//     (published/extracted;    |  merged into pkg  |  <alias>/<rel> + advisory
//     MUTABLE stored identity) |  .assetdb, wins   |  Info advising republish;
//                              |  conflicts)       |  persisted to pkg .assetdb
//   Engine, manifest           | STORED (manifest  | REJECTED (IsImmutable) —
//     (MakeEnginePackageMount) |  IS the store)    |  republish to add files
//   Engine, no manifest        | derived, scanned  | derived, not persisted
//     (MakeEnginePackageMount) |                   |  (no store to write)
//   Git cache, manifest        | STORED (manifest  | REJECTED (IsImmutable)
//     (MakePackageMount)       |  is the store)    |
//   Git cache, no manifest     | derived, no scan  | not persisted (read-only
//                              |  persistence      |  cache; derived in-memory)
//   Shipped Player content     | STORED            | REJECTED (IsImmutable)
//     (MakePackageMount)       |                   |
//
// Why derived fallback (not random, not reject) for mutable published
// packages: GUID::Derive(<alias>/<rel>) is reproducible across sessions and
// machines, equals what a plain dev mount of the same package would mint, and
// equals what the NEXT republish bakes into the manifest — so references
// authored against a fallback GUID survive the republish unchanged. Rejecting
// would break the local dev loop the mutable mount exists for; random minting
// would re-identify the file every session. For a mount whose working DB is a
// separate file, the manifest is only ever written by the explicit publish step;
// where the manifest IS the store (the engine-package shape), the registry
// writes it like any other store and publishing is not involved.
//
// Per-asset IMPORT SETTINGS (the store's kv rows) follow a second axis, because
// what a package's author edits is the settings of files the package already
// declares, never the set of files itself:
//
//   Embedded (either shape)  kv written to the package's own .assetdb
//   Engine, manifest         kv written to the manifest in the tree the package
//     (developer build)      is AUTHORED in, which is the mount's store — the
//                            one shape with a FROZEN asset set and writable
//                            settings (AcceptsMetadataWrites)
//   Engine, manifest         refused: staged content with no authoring tree
//     (shipped / packaged)   behind it
//   Engine, no manifest      refused: nothing published, so no store
//   Git cache / shipped      refused: a cache entry and shipped content are
//                            never written to (AcceptsMetadataWrites=false)
//
// A refused write is loud and names the fix; it is never silently dropped.
//
// A third axis is deliberately NOT granted with the settings: the subasset
// DERIVE-KEY journal a loader writes when it opens a container (a glTF's
// meshes and materials). RegisterSubassetDeriveKeys gates on IsImmutable,
// which every published package shape keeps, so it is refused on all of them —
// including the writable engine shape. The keys are loader output, not
// authored content, and a published package's subasset identities are already
// fixed by its manifest. eztree publishes no container asset, so nothing
// reaches the gate today; a first-party package that ships models is what
// would make this a decision rather than a default.
//
//   Embedded (either shape)  derive keys journaled (mutable mount)
//   Every published package  refused (IsImmutable)
//   Package with no manifest refused (no store to journal into)
//
// DERIVED ARTIFACTS and texture inference form a fourth axis: may the render
// path INFER a texture's import settings into the mount's store, and may a
// loader COOK missing textures or model LODs into the mount's derived cache.
// These separate because a package's settings are authored and committed with
// it, while its derived artifacts belong to whichever process is running — an
// editor and a developer Player own a writable host cache. Shipped games
// consume the baked artifacts without writing their install folders. A missing
// texture cook is diagnosed as a damaged package; missing model LODs generate
// in memory.
//
//   Mount shape              | Infers settings   | Cooks a missing artifact
//                            |  on a bind        |
//   -------------------------+-------------------+--------------------------
//   Embedded (either shape)  | yes               | yes
//   Engine, manifest         | no — the manifest | yes, into
//     (either shape)         |  is committed     |  <host cache>/Packages/<alias>
//   Engine, no manifest      | yes (refused: no  | yes
//                            |  store to write)  |
//   Git cache, manifest      | no — a cache entry| yes, into the host-owned
//                            |  takes no writes  |  derived root
//   Git cache, no manifest   | yes (refused: no  | yes
//                            |  store to write)  |
//   Shipped Player content   | no                | NO — a miss means a damaged
//     (MakePackageMount)     |                   |  package: textures load raw
//                            |                   |  with an error naming the
//                            |                   |  fix; model LODs generate
//                            |                   |  only in memory
// ---------------------------------------------------------------------------

// Build the asset-source descriptor for a resolved local/embedded dev
// package (the P0 mount shape). Unlike MakePackageMount (immutable, shipped,
// stored-identity), dev packages are MUTABLE and watched like project assets:
// their own authoritative DB (<pkgroot>/AssetDatabase.assetdb) + derived
// cache (<pkgroot>/.Cache/AssetDatabase) so their records never pollute the
// project DB, and RejectOverlappingRoots because package mounts must never
// nest with another source root (overlap remaps GUIDs in place).
//
// Identity is derived from "<alias>/<rel-path>" — unless the package ships
// <AssetsDir>/.assetmanifest, in which case the mount switches to MUTABLE
// STORED identity (manifest GUIDs verbatim, merged into the working DB; see
// the decision table above).
AssetSourceDesc MakeEmbeddedPackageMount(const ResolvedPackage& package);

// Read-only, unwatched git mount. Manifest-backed packages keep derived
// data under the supplied host cache root. Manifest-less packages scan into
// memory and have no persistent derived database.
AssetSourceDesc MakeGitPackageMount(const ResolvedPackage& package,
                                    const std::filesystem::path& cacheRoot);

// Mount shape for a first-party engine package (Packages/<name>, staged
// next to the executable). In a DEVELOPER build it is a SOURCE package, not a
// cache entry: the staged tree names the repository tree it was staged from
// (ResolvedPackage::AuthoringRootDir), and the published .assetmanifest in THAT
// tree is the mount's authoritative store — per-asset import settings edited in
// the inspector land where the package is committed, and return to the staged
// tree with the next build. Identity, priority, the absent filesystem scan, the
// frozen asset set and the overlap rule are all MakePackageMount's; only the
// import settings become writable. Derived caches use the supplied host root.
//
// With no authoring tree named (a shipped or packaged tree) the shape is
// MakePackageMount exactly: read-only staged content. A package with no
// published manifest has no store to author into and mounts read-only with
// derived identity, like the manifest-less git shape.
AssetSourceDesc MakeEnginePackageMount(const ResolvedPackage& package,
                                       const std::filesystem::path& cacheRoot);

// Mount every enabled package of a resolution (in resolver order) and log the
// resolution's errors/warnings loudly. Packages whose assets dir does not
// exist, or whose registration is rejected (e.g. overlapping roots), are
// skipped with a loud log line; everything else still mounts. Returns the
// aliases actually mounted — keep them to unmount on project switch.
std::vector<std::string> MountResolvedPackages(AssetManager& assetManager,
                                               const PackageResolution& resolution,
                                               const std::filesystem::path& cacheRoot);

// Unregister previously mounted package sources. Each one ejects its loaded
// assets first, which finishes when the loads it still had in flight resolve,
// so `onUnmounted` runs when the last source is gone — possibly inside this
// call, possibly several frames later. Nothing here blocks a thread.
void UnmountPackageSources(AssetManager& assetManager, const std::vector<std::string>& aliases,
                           Function<void()> onUnmounted = {});

} // namespace GameEngine
