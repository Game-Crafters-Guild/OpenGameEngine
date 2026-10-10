#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/AssetRegistry.h"
#include "AssetCore/DepEdge.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Result.h"
#include "AssetCore/SnapshotFingerprint.h"
#include "AssetCore/AssetIgnoreRules.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Types/Types.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

// Forward declarations
class ParserRegistry;
class AsyncRegistryCoordinator;

namespace AssetDatabase
{
class IAssetStore;
class IAssetDbCache;
struct AssetRecord;
struct ReconcileScanNewFile;
struct ReconcileHeal;

// Mirror of the alias in AssetDatabase/IAssetDbCache.h. Declared here (rather
// than including that header) so this Engine public header stays free of the
// AssetDatabase include dir — modules like UI/Scene that pull in
// Assets/AssetRegistry.h don't link AssetDatabase and would otherwise fail to
// resolve the include. std::function only needs IAssetDbCache forward-declared.
using AssetDbCacheFactory = std::function<std::unique_ptr<IAssetDbCache>()>;
} // namespace AssetDatabase

/// Well-known source alias constants.
/// Use collision-safe names to avoid shadowing local constants in editor modules.
inline constexpr std::string_view kAssetSourceAliasProject = "project";
inline constexpr std::string_view kAssetSourceAliasEditor  = "editor";

struct AssetSourceDesc
{
    // Stable logical source name used by callers (for example: "project", "editor", "package", "mod").
    std::string Alias;
    // Absolute or relative filesystem root for this source.
    std::filesystem::path Root;

    // GUID assignment strategy.
    // true (default): GUIDs are derived deterministically from alias + relative path.
    //   No DB is needed for identity. Suitable for: editor, packages, mods.
    // false: GUIDs are stored in the authoritative DB. New assets get GUID::Generate().
    //   Requires AuthoritativeDbFile to be set. Suitable for: project.
    bool DerivedIdentity = true;

    // Optional authoritative metadata store.
    // For DerivedIdentity=false: required. Stores GUIDs and metadata. Source of truth.
    // For DerivedIdentity=true: optional. Stores type info and KV metadata alongside
    //   derived GUIDs. If empty, metadata is in-memory only.
    std::filesystem::path AuthoritativeDbFile;
    // Directory of the derived SQLite cache for a source with an
    // AuthoritativeDbFile; its parent holds cooked textures (TryGetCacheRoot).
    // Unused without a store. Empty means the store's own directory. For a
    // read-only store such as a shipped .assetmanifest that is what it should
    // be: the export publishes its texture bakes beside the content, the mount
    // reads them from there and never writes. A mount that cooks on a miss needs
    // a writable directory of the host's here.
    std::filesystem::path CacheRoot;

    // Optional published stored-identity manifest (.assetmanifest, same JSONL
    // schema as the store). When set and the file exists, SetupSourceStore
    // loads it and merges its records into the authoritative store at mount,
    // with the MANIFEST winning identity conflicts ({guid, path} bindings) —
    // the manifest is the package's published identity contract; the local DB
    // is the working copy. The registry never writes this file; refreshing it
    // is the explicit publish step (PublishPackageAssetManifest). Used by the
    // mutable stored-identity package mount (extracted packages): set together
    // with DerivedIdentity=false and a writable AuthoritativeDbFile.
    std::filesystem::path IdentityManifestFile;

    // Implicit resolution priority. Higher values are checked first during implicit path/GUID resolution.
    // Convention: project ~100, editor ~50, mods ~25.
    int32_t Priority = 0;

    // ------------------------------------------------------------------
    // Mount lifecycle flags. Defaults preserve standard project-mount
    // behaviour; MakePackageMount sets the read-only/immutable shape used by
    // the packaged Player. Each flag's "Wired:" note below says where it takes
    // effect at runtime.
    // ------------------------------------------------------------------

    // false for Package mounts (manifest-driven, no fs walk needed).
    // Wired: RegisterSource skips the auto ScanDirectoryAsync when false.
    bool RequiresScan = true;

    // true for Package mounts: treats the mount as read-only metadata where
    // assets cannot be added or removed at runtime. Wired: RegisterAsset,
    // UnregisterAsset, TryRenameAssetPath, and TryUnregisterAssetByPath
    // reject + warn when the affected path falls under an immutable source.
    bool IsImmutable = false;

    // Whether per-asset key/value metadata (import settings) may be written
    // into this source's store. Deliberately independent of IsImmutable: a
    // published package's ASSET SET is fixed by its manifest — nothing may be
    // added, removed or renamed — while the import settings of the files that
    // manifest already declares are precisely what the package's author edits.
    // Wired: SetMetaValue and GetImportSettingsOrigin (SourceAcceptsMetaWrites), which
    // also require a store and a writable mount.
    bool AcceptsMetadataWrites = true;

    // true when this source's store file is reachable by other PROCESSES — an
    // engine package's manifest is written by every editor and developer Player
    // built from the same checkout. Wired: SetupSourceStore gives the store an
    // advisory lock beside its file, so two processes' writes cannot land inside
    // one another and each write re-reads the file under that lock and replays
    // its own deltas on top, so neither process rewrites away the other's rows.
    bool StoreIsSharedAcrossProcesses = false;

    // Whether the render path may INFER this source's texture import settings —
    // the colour space a data-slot bind writes into the source's store when none
    // is set, and the cook usage every bind widens to serve its slot
    // (WidenTextureCookUsage). False for every published package: its settings
    // are authored and committed with it, so drawing one of its meshes must never
    // author them. Wired: TextureService's EnsureTextureColorSpaceTagged and
    // EnsureTextureCookUsageTagged, through GetDerivedArtifactPolicy.
    bool InfersTextureImportSettings = true;

    // Whether loaders may generate missing derived artifacts in the owning
    // source's cache. Independent of metadata and asset-record mutability:
    // editors and developer Players own writable host caches even for immutable
    // packages. Shipped Player mounts consume their baked artifacts without
    // writing to the install folder. TextureAsset and ModelAsset query this
    // through GetDerivedArtifactPolicy.
    bool CooksDerivedArtifactsOnMiss = true;

    // true for CI / headless / shared-volume scenarios. When true, the registry
    // skips persistent writes (no flush, no snapshot, no file-watcher subscription).
    // Already wired: TickPersistence + Shutdown snapshot write skip when IsReadOnly.
    bool IsReadOnly = false;

    // false for transient/scratch mounts: don't preserve identity for missing
    // assets; just remove the entry instead of marking missing. Wired:
    // TryUnregisterAssetByPath drops to RemoveAsset when the affected
    // source has this flag false (both npStore and project paths).
    bool TracksTombstones = true;

    // false for read-only/remote mounts where we don't want active fs watching
    // (avoids spurious events on networked or virtual filesystems). Wired:
    // AssetManager::RegisterSource skips the FileWatchingService subscription
    // when this flag is false.
    bool RegisterFileWatcher = true;

    // E5 overlap policy: when true, RegisterSource fails loudly if this
    // source's root nests with an existing source root (either direction),
    // as spelled or once symlinks are resolved.
    // Package mounts default to true via MakePackageMount — overlapping
    // mounts remap GUIDs in place, which the packaged Player must never
    // encounter. The legacy project/editor same-root overlap keeps working
    // (both default to false) and is covered by the GuidRemapped event.
    bool RejectOverlappingRoots = false;

    // When TracksTombstones is true, deleting an asset normally tombstones it
    // (MarkMissing=true) to preserve GUID identity in case anything still
    // references it. AggressiveTombstoneCleanup=true upgrades that to: tombstone
    // ONLY if something still references the asset; otherwise actually remove
    // the row from the store. Hygiene over time — long-lived projects accumulate
    // throwaway test assets that nothing references, and they don't need to
    // linger as tombstones. Includes a cascade: when an asset is removed, its
    // outgoing dep edges are re-checked and any orphans (zero live refs, zero
    // path refs, zero produced sidecars) get cleaned up too. Ignored when
    // TracksTombstones=false (which always removes anyway).
    bool AggressiveTombstoneCleanup = true;

    // Ghost retention for derived-identity mounts, applied by the startup
    // reconcile pass (see AssetDatabase::ReconcileGhostRetention for the
    // invariants that hold whatever these say). Both defaults keep every ghost
    // forever: a mount collects nothing until it opts in.
    //
    // Sessions a ghost must have been continuously missing before it is
    // collected; 0 disables age-based collection.
    uint32_t GhostRetentionSessions = 0;
    // Collect ghosts the rename matcher can never bind — records the cache
    // holds no fingerprint for, so no tier indexes them and no amount of
    // waiting can heal them.
    bool CollectNeverHealableGhosts = false;
};

/// Source policy for inferred texture settings and derived-artifact generation.
/// Resolved together under one lock. Unknown or unmounted assets permit neither
/// inferred metadata writes nor persistent artifact generation.
struct DerivedArtifactPolicy
{
    bool InfersTextureImportSettings = false;
    bool CooksOnMiss = false;
};

/// Where an asset's import settings come from and what this editor may do with
/// them. Resolved from one source entry under one lock take: an editor asking
/// whether it may write has already paid for the name to put in the refusal.
struct AssetImportSettingsOrigin
{
    /// The owning source's alias ("project", "editor", a package's name). Empty
    /// when no mounted source owns the path.
    std::string SourceAlias;
    /// Whether SetMetaValue on this asset would persist.
    bool Writable = false;
};

/// Lightweight immutable row used by editor indexes that need to enumerate
/// every live asset. Unlike AssetMetadata, this intentionally excludes
/// dependency and subasset vectors so taking a project-wide snapshot does not
/// copy unrelated graph data.
struct AssetIndexRecord
{
    GUID Guid;
    AssetType Type = AssetType::Unknown;
    String TypeId;
    std::filesystem::path Path;
    String Name;
    String Extension;
    std::filesystem::file_time_type LastModified{};
    size_t FileSize = 0;
};

/// The one projection from full metadata down to an index row. Callers that
/// hold an AssetMetadata but must satisfy a record-shaped interface — a
/// single-asset validation against the same filter a bulk index applies — use
/// this rather than re-listing the fields, so the two can never drift.
AssetIndexRecord ToAssetIndexRecord(const AssetMetadata& metadata);

// Immutable manifest-driven mount (Package mount) — the packaged Player's
// asset source. No filesystem walk (assets enumerated from
// <root>/.assetmanifest), no writes, no file watcher. Identity is stored in
// the manifest (DerivedIdentity=false) — the editor derives every GUID at
// build time and bakes {guid, path} into the manifest, so the Player reads
// stable GUIDs verbatim rather than re-deriving. The manifest is the
// authoritative store, like a project DB but read-only.
//
// SetupSourceStore loads <root>/.assetmanifest as the source's JSONL store;
// the lifecycle flags (RequiresScan/IsImmutable/AcceptsMetadataWrites/
// IsReadOnly/TracksTombstones/RegisterFileWatcher) handle the
// read-only-immutable behaviour.
inline AssetSourceDesc MakePackageMount(std::string alias,
                                        std::filesystem::path root,
                                        int32_t priority = 25)
{
    AssetSourceDesc d;
    d.Alias = std::move(alias);
    d.Root = root;
    d.DerivedIdentity = false;
    d.AuthoritativeDbFile = root / ".assetmanifest";
    d.Priority = priority;
    d.RequiresScan = false;
    d.IsImmutable = true;
    d.AcceptsMetadataWrites = false;
    d.InfersTextureImportSettings = false;
    d.CooksDerivedArtifactsOnMiss = false;
    d.IsReadOnly = true;
    d.TracksTombstones = false;
    d.RegisterFileWatcher = false;
    d.RejectOverlappingRoots = true;
    return d;
}

// Use fully qualified JobSystem types to avoid conflicts

// AssetMetadata is now defined in AssetCore/Asset.h

/**
 * @brief Central registry for all assets in a project: maps filesystem paths to
 *        stable asset GUIDs and the metadata/dependency graph built around them.
 *
 * Identity model (DB-derived-from-path, no .meta sidecars):
 *  - An asset's GUID is DERIVED deterministically from its source alias + canonical
 *    relative path (GUID::Derive) for derived-identity mounts (editor, packages,
 *    mods); for the project mount the same derived id is the default while the
 *    AssetDatabase (AssetDatabase.assetdb) acts as a git-friendly CACHE of
 *    type/KV metadata, NOT the source of truth for identity. No asset has a
 *    ".meta" sidecar file beside it carrying its GUID or settings. The
 *    ".meta" extension is filtered in exactly one authoritative place —
 *    AssetIgnoreRules (AssetCore/AssetIgnoreRules.cpp), which every scan /
 *    registration enumeration path consults — so .meta files are never registered.
 *  - Per-asset key/value metadata (e.g. the texture color-space and swizzle
 *    overrides, keys "assets.texture.colorSpace"/"assets.texture.swizzle") lives
 *    in the AssetDatabase KV store keyed by GUID, reached via TryGetMetaValue /
 *    SetMetaValue. "Meta" in those names refers to DB metadata, not a file.
 *
 * The REAL sidecars that DO exist are a separate, unrelated concept: the
 * humanoid-rig JSON written next to a model (ModelAsset SidecarPathForModel) and
 * per-action animation clips (ModelAssetLoadBlend, <stem>__<action>.anim.json).
 * Those are authored/derived content products, not identity sidecars.
 */
class AssetRegistry
{
  public:
    AssetRegistry();
    ~AssetRegistry();

    // ------------------------------------------------------------------
    // Derived-cache factory injection.
    //
    // The concrete derived cache (SQLite) lives in a separate library that
    // only editor/tooling/test targets link, so the engine + standalone
    // Player ship no SQL engine. Those targets call SetAssetDbCacheFactory
    // once at startup to register the implementation; runtimes that never
    // register one (the Player) run fully cache-less — every cache call site
    // already degrades gracefully to "continuing without cache".
    // ------------------------------------------------------------------
    static void SetAssetDbCacheFactory(AssetDatabase::AssetDbCacheFactory factory);

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /**
     * @brief Infrastructure-only initialization (type registry, async coordinator).
     *
     * Does NOT register any sources, load any DB, or start any scans.
     * Call RegisterSource() afterwards to add sources.
     */
    bool Initialize(JobSystem::WorkStealingThreadPool* jobSystem = nullptr);

    /**
     * @brief Convenience: initialize infrastructure + register a "project" source.
     *
     * Equivalent to Initialize(jobSystem) followed by
     * RegisterSource({Alias="project", DerivedIdentity=false, Root=assetRoot,
     *                  AuthoritativeDbFile=dbFile, CacheRoot=cacheRoot, Priority=100}).
     */
    bool Initialize(const std::filesystem::path& assetRoot,
                    JobSystem::WorkStealingThreadPool* jobSystem = nullptr,
                    const std::filesystem::path& authoritativeDbFile = {},
                    const std::filesystem::path& cacheRoot = {});

    /**
     * @brief Shutdown registry (flushes all source stores, clears all state).
     */
    void Shutdown();

    // ------------------------------------------------------------------
    // Source management (unified model)
    // ------------------------------------------------------------------

    /**
     * @brief Register a new asset source.
     *
     * Validates alias/root, loads DB (if configured via AuthoritativeDbFile),
     * runs reconciliation for stored-identity sources, populates hot caches, and starts a directory scan.
     * "project" is a valid alias (no longer reserved).
     */
    bool RegisterSource(const AssetSourceDesc& source);

    /**
     * @brief Block until a source's startup directory scan finishes.
     *
     * No-op when the alias is unknown, the source has RequiresScan=false, or no
     * scan was started (for example synchronous scan without a job system).
     * Empty alias waits for all sources that have an in-flight scan.
     */
    void WaitForStartupScan(std::string_view sourceAlias = std::string_view(kAssetSourceAliasProject));

    /**
     * @brief Non-blocking: true while the source's startup (or rebind) directory
     * scan is still registering files. False when it finished, was waited on,
     * or never started.
     */
    bool IsStartupScanRunning(std::string_view sourceAlias) const;

    /**
     * @brief Post-scan reconcile for a derived-identity source.
     *
     * Called by the scan pipeline's registry-update tail with the scanned
     * directory, before the scan's count promise resolves — anything waiting
     * on StartupScanFuture observes a fully reconciled registry. Matches the
     * derived source whose root equals scanDirectory (partial-directory scans
     * match nothing and no-op). Missing observations are written to the
     * source's per-machine derived cache. Offline renames are healed by
     * matching absentees against `scanNewFiles` — the scan's !hadExisting
     * registrations, collected by RegisterAssetMetadataBatch — and heals DO
     * write the journaled store: a redirect old→new, migrated kv, and the
     * ghost record's removal. Healed ghosts are also dropped from the
     * in-memory registry so lookups chase the redirect instead of a stale
     * resident record. Null/empty scanNewFiles means missing-marking only.
     */
    void ReconcileDerivedSourceAfterScan(
        const std::filesystem::path& scanDirectory,
        const std::vector<AssetDatabase::ReconcileScanNewFile>* scanNewFiles);

    /**
     * @brief Unregister a source by alias.
     *
     * Flushes its store, clears its hot-cache entries from m_Assets/m_PathToGuid,
     * and removes it from the sources list.
     */
    bool UnregisterSource(std::string_view sourceAlias);

    /**
     * @brief Whether a background save still owns this source's store.
     *
     * Unregistering or rebinding a source destroys objects an in-flight save
     * holds raw pointers to, so those calls must not run while this is true.
     * They enforce that themselves by waiting; a caller on a thread that must
     * not block — the one running the frame — polls this instead and defers
     * its own work until it clears.
     */
    bool IsSourceFlushInProgress(std::string_view sourceAlias) const;

    /**
     * @brief Rebind a source to a new root (and optionally new DB location).
     *
     * Flushes old store, clears old hot-cache entries, loads new store, reconciles,
     * populates hot caches, and starts a new scan. Other sources are untouched.
     */
    bool RebindSource(std::string_view sourceAlias, const AssetSourceDesc& newDesc);

    /**
     * @brief Collect all GUIDs belonging to a source (for ejection during rebind/unregister).
     */
    std::vector<GUID> GetAssetsForSource(std::string_view sourceAlias) const;

    /**
     * @brief Snapshot all currently registered sources.
     */
    std::vector<AssetSourceDesc> GetRegisteredSources() const;

    /**
     * @brief Visit the authoritative store records (identity + kv) of one
     * source, tombstones included (rec.missing == true). Returns false when
     * the alias is unknown or the source has no store configured.
     *
     * Used by package publishing (PublishPackageAssetManifest) to snapshot a
     * mounted package's identity without exposing the store in this header.
     * The visitor runs outside the registry lock against a copied record
     * vector, so it may safely call back into the registry.
     */
    bool VisitSourceStoreRecords(
        std::string_view sourceAlias,
        const std::function<void(const AssetDatabase::AssetRecord&)>& visitor) const;

    /**
     * @brief Deterministic GUID for a path under a derived-identity source,
     * computed WITHOUT the source being registered. Exactly the derivation a
     * mounted derived-identity source (project, dev packages) performs, so a
     * build can attribute a dangling scene reference to a disabled package by
     * deriving over that package's (unmounted) asset tree. Null on bad inputs.
     */
    static GUID DeriveGuidForSourcePath(std::string_view sourceAlias,
                                        std::string_view relativePath);

    /**
     * @brief Resolve the project-wide derived-artifact cache root (typically
     * <ProjectRoot>/.Cache/) for the source that owns the given GUID.
     * Each derived-artifact subsystem appends its own subdir name under
     * this root — AssetDatabase/, Shaders/, MeshBvh/, etc. — so they all
     * sit as siblings instead of nesting inside AssetDatabase's folder.
     *
     * Returns nullopt for derived-identity sources whose AssetSourceDesc::
     * CacheRoot is empty, or for unknown GUIDs.
     */
    std::optional<std::filesystem::path> TryGetCacheRoot(const GUID& guid) const;

    // The owning source's texture inference and derived-artifact policy under one
    // lock take. Unknown or unmounted assets refuse both; explicit source
    // ownership wins over root overlap.
    DerivedArtifactPolicy GetDerivedArtifactPolicy(const GUID& guid) const;

    /**
     * @brief Path-based variant of TryGetCacheRoot. Used by derived-artifact
     * stores during AssetDestroyed handling: by the time the event fires
     * the GUID has been erased from m_Assets, but AssetEvent carries the
     * path so the cache file location can still be computed.
     */
    std::optional<std::filesystem::path> TryGetCacheRoot(const std::filesystem::path& absPath) const;

    /**
     * @brief Whether loader output describing the asset at absPath (a model's
     * imported materials) may be recorded in its source's derived cache.
     *
     * False when the owning source's content is fixed (IsImmutable or
     * IsReadOnly: a published package, or a packaged game whose cache root is
     * its install directory) and when no source owns the path. Ask this, not
     * whether TryGetCacheRoot answers: a manifest mount has a cache root too.
     */
    bool AcceptsDerivedRecords(const std::filesystem::path& absPath) const;

    /**
     * @brief Set parser registry for asset type detection
     */
    void SetParserRegistry(ParserRegistry* parserRegistry);

    /**
     * @brief Register an asset
     */
    bool RegisterAsset(const std::filesystem::path& path);

    /**
     * @brief Register an asset with explicit source ownership.
     *
     * When @p preferredSourceAlias names a derived-identity source whose root
     * contains @p path, the asset is assigned a deterministic derived GUID from
     * that source's namespace instead of the project's stored identity.  If the
     * asset was already registered under a different GUID (e.g. from the project
     * scan), the in-memory maps are remapped to the derived GUID so that
     * UIHotReload bindings, loaded-asset caches, and file-watcher events all
     * agree on a single stable identity.
     *
     * Ownership is recorded so that `GetAssetsForSource` / `RebindSource` /
     * `UnregisterSource` only eject assets belonging to the correct source,
     * even when multiple sources share the same physical root.
     */
    bool RegisterAsset(const std::filesystem::path& path, std::string_view preferredSourceAlias);

    /**
     * @brief Notification for in-place GUID remaps (E5).
     *
     * Fired when an overlapping source re-claims an already-registered path
     * (RegisterAsset(path, preferredSourceAlias) reassigning the derived
     * GUID), and when registering a path releases a rename-kept identity
     * (TryHealRenameKeptIdentityLocked migrating the renamed asset onto its
     * own path-derived GUID). Invoked AFTER the registry maps are consistent
     * and OUTSIDE the registry lock. AssetManager installs one at Initialize
     * to atomically re-key its loaded-asset / in-flight maps — the fix for
     * the historical "overlapping-source load flake".
     */
    using GuidRemapCallback = std::function<void(const GUID& oldGuid,
                                                 const GUID& newGuid,
                                                 const std::filesystem::path& path)>;
    void SetGuidRemapCallback(GuidRemapCallback callback);

    /**
     * @brief Register asset with pre-computed metadata (for async operations)
     */
    bool RegisterAssetMetadata(const AssetMetadata& metadata);

    /**
     * @brief Batched RegisterAssetMetadata for the scan fan-in (E2).
     *
     * Same per-asset semantics as RegisterAssetMetadata (ownership routing,
     * immutable-source gate, fingerprint persistence), restructured for
     * throughput: expensive per-file work (classification, hashing, store
     * upserts) runs without the registry lock, derived-cache rows land in one
     * SQLite transaction per batch, and the in-memory maps are committed
     * under a single writer-lock acquisition for the whole batch.
     *
     * Entries must carry a resolved (non-null) Guid. Returns the number of
     * assets registered into the in-memory maps.
     *
     * When outDerivedNewFiles is non-null and the project source is
     * derived-identity, entries that had no store record before this batch
     * (the !hadExisting set) are appended with the fingerprints computed for
     * the cache batch — the scan tail feeds them to
     * ReconcileDerivedSourceAfterScan as rename-heal candidates.
     */
    size_t RegisterAssetMetadataBatch(
        Vector<AssetMetadata>&& batch,
        std::vector<AssetDatabase::ReconcileScanNewFile>* outDerivedNewFiles);

    /**
     * @brief Number of warm-start snapshot records loaded for a source.
     *
     * Returns the count of (path, mtime, size, fileId) records loaded from
     * <cacheRoot>/watcher.snapshot.bin at SetupSourceStore time. 0 if no
     * snapshot was found, the source has no cache, or the source alias
     * doesn't match a registered source. Used by tests + diagnostics to
     * verify the warm-start data flow (Phase 5 step 2a writes, step 2b
     * loads). A non-zero count after Initialize on a project that's been
     * shut down at least once means the data round-trip is working; the
     * actual reconcile-path use of those records is a future optimization.
     */
    size_t GetSnapshotRecordCount(std::string_view sourceAlias) const;

    /**
     * @brief Compute the canonical relative path used as a key in the registry
     *        and the snapshot map. Public so the async scan path can match
     *        snapshot keys without duplicating the normalization logic.
     *
     * Containment and spelling are decided separately. Containment compares
     * the two paths element by element in the identity domain, so it holds
     * however either side is spelled — a source root carries the caller's
     * spelling while a NormalizePathForMap'd asset path is folded on
     * case-insensitive platforms, so pairing them lexically would answer
     * "outside the root" for every asset. The tail is then emitted verbatim
     * from `assetPath`, because this string is joined back onto a root to
     * reopen the file. Callers needing a spelling-blind key run the result
     * through AssetPaths::FoldStorePathKey, as the store does with its rows.
     *
     * Returns false when the path is not strictly inside the asset root.
     */
    static bool TryComputeCanonicalRelativePath(const std::filesystem::path& assetRoot,
                                                const std::filesystem::path& assetPath,
                                                std::string& outCanonical);

    /**
     * @brief Return how many files the most recent reconcile pass reused a
     *        hash for from the warm-start snapshot. Test-only diagnostic for
     *        Block D — confirms the delta-scan code path actually fires on
     *        round-trip rather than silently no-op'ing.
     *
     * Returns 0 if no reconcile has run yet, the source alias doesn't match,
     * or the source had no snapshot. The counter is per-source (not
     * cumulative across multiple registrations).
     */
    size_t GetLastReconcileHashesReused(std::string_view sourceAlias) const;

    /**
     * @brief Explicitly register an asset by path and return its GUID.
     *
     * The caller-driven discovery API for paste-path / drag-drop / CLI cook
     * flows where the file may not have been observed by a scan or
     * FileWatcher event yet. Combines RegisterAsset + GetAssetGUID into a
     * single call returning a typed Result so callers handle the failure
     * shape explicitly instead of "did register succeed?" + "is GUID null?".
     *
     * On success the returned Result holds the (newly-derived or
     * already-registered) GUID. On failure:
     *   AssetError::MountUnavailable — no source contains @p path (path
     *      is outside any registered mount root or registry not Initialized).
     *   AssetError::Missing — RegisterAsset succeeded but GUID resolution
     *      came back null (should not normally happen; defensive).
     *
     * Idempotent for paths already registered (returns the existing GUID).
     */
    Result<GUID, AssetError> RegisterAssetByPath(const std::filesystem::path& path);

    /**
     * @brief Unregister an asset
     */
    void UnregisterAsset(const GUID& guid);

    /**
     * @brief Get asset metadata by GUID
     */
    bool TryGetAssetMetadata(const GUID& guid, AssetMetadata& outMetadata) const;

    /**
     * @brief Get asset metadata by path
     */
    bool TryGetAssetMetadata(const std::filesystem::path& path, AssetMetadata& outMetadata) const;

    /**
     * @brief Get GUID for asset path
     */
    GUID GetAssetGUID(const std::filesystem::path& path) const;

    /**
     * @brief Get an existing GUID for an on-disk asset path, or create one if absent.
     *
     * Intended for high-throughput scanners that already validated ignore rules and file existence.
     * This avoids doing expensive per-file work (hashing/fingerprinting) during bulk scans.
     */
    GUID GetOrCreateAssetGUID(const std::filesystem::path& path);

    /**
     * @brief Get all assets of a specific type
     */
    Vector<GUID> GetAssetsByType(AssetType type) const;

    /**
     * @brief Get one lock-consistent, lightweight snapshot of every live asset.
     *
     * This is the bulk read path for search and browsing indexes. It replaces
     * repeated GetAssetsByType + TryGetAssetMetadata calls, which otherwise
     * walk the registry and acquire its shared lock once per asset.
     */
    Vector<AssetIndexRecord> GetAssetIndexSnapshot() const;

    /**
     * @brief Get a snapshot of the active ignore rules (defaults + <AssetRoot>/.assetignore).
     * This is used by background scanners to avoid indexing tool/build/generated folders.
     */
    AssetIgnoreRules GetIgnoreRulesSnapshot() const;

    /**
     * @brief Get the number of registered assets (thread-safe)
     */
    size_t GetAssetCount() const;

    /**
     * @brief Resolve a GUID through any configured redirects.
     * If no redirect exists, returns the input GUID.
     */
    GUID ResolveGuid(const GUID& guid) const;

    /**
     * @brief Every GUID whose redirect chain leads to `resolvedGuid`, directly or through other
     * redirects; `resolvedGuid` itself is never included, even inside a redirect cycle. Empty, at
     * the cost of one atomic load, when the project has no redirects. Served from the project
     * store's reverse index, so the cost follows the result, not the size of the redirect table.
     */
    std::vector<GUID> FindRedirectsTo(const GUID& resolvedGuid) const;

    /**
     * @brief The identity a GUID carries through this session's remap
     * aliases — the E5 reassign and the rename-kept-identity heal, which
     * migrate a record to another GUID and alias the old one to it.
     *
     * A GUID with a primary record is its own identity: a registration
     * retires the alias the GUID left behind, so only alias-only GUIDs chase.
     * Identity for GUIDs never remapped. The registry is the single owner of
     * this table; a load pipeline that captured a GUID before a remap resolves
     * it here at completion, which lands the result under the current key only
     * while the captured GUID has no primary record — once a new file claims
     * it, the captured GUID resolves to that primary (AssetManager::OnGuidRemapped
     * states the consequence).
     */
    GUID ResolveSessionAlias(const GUID& guid) const;

    /**
     * @brief Resolve a `<alias>:<relativePath>` URL to a registered asset GUID.
     *
     * Used by code paths that want to point at an asset shipped under a
     * specific source mount without hard-coding the absolute path. For
     * example: `editor:SkeletonProfiles/HumanoidStandard.profile.json`
     * resolves through the editor mount.
     *
     * The leading `<alias>:` is required; without it the function returns
     * a default-constructed GUID. If the alias matches no registered source,
     * or the file isn't registered under that source, returns a
     * default-constructed GUID.
     *
     * Resolution is alias-scoped: a path that exists under both the project
     * mount and the editor mount but is requested via `editor:` returns the
     * editor-mount asset, NOT a fall-through to project.
     */
    GUID ResolveByAlias(std::string_view aliasUrl) const;

    /**
     * @brief Get the assets that this asset directly depends on (derived reference index).
     * Returns an empty list if the derived cache is unavailable.
     */
    Vector<GUID> GetDependencies(const GUID& assetGuid);

    /**
     * @brief Re-extract this asset's dependencies from its file NOW and return them.
     *
     * GetDependencies is lazy AND never invalidated on file edits: an asset
     * whose edges were extracted before a reference was authored keeps the
     * stale (possibly empty) edge set. Correctness-critical consumers — the
     * build pipeline's asset collector walking the scenes being shipped —
     * re-extract here instead of trusting the cache. Persists the fresh edges
     * to the derived cache (same as the lazy path) and returns the extracted
     * targets directly, so the result does not depend on cache availability.
     */
    Vector<GUID> RefreshDependencies(const GUID& assetGuid);

    /**
     * @brief Get the assets that reference this asset (reverse edge query).
     * Returns an empty list if the derived cache is unavailable.
     */
    Vector<GUID> GetDependents(const GUID& assetGuid) const;

    /**
     * @brief Get the full dep-edge list (kind + locator + target) for an asset,
     * with path-form edges resolved to GUIDs where the path-target is registered.
     *
     * Edges with `Target.IsNull() && !TargetPath.empty()` after this call are
     * unresolved — the path-target wasn't found in any mount. Phase 4 retarget
     * UX uses this signal to show "missing reference" rows.
     */
    std::vector<DepEdge> GetResolvedDependencyEdges(const GUID& assetGuid) const;

    /**
     * @brief Resolve a canonical mount-relative path to its registered GUID.
     *
     * Looks up the path through the same map AssetMetadata::Path uses. Returns
     * GUID::Null() if no asset is registered at that path. Pure read; takes a
     * shared lock on the registry.
     */
    GUID ResolvePathTarget(std::string_view canonicalRel) const;

    // -------------------------------------------------------------------------
    // Provenance: importer-time "produced_by" relationships.
    //
    // Distinct from the runtime dep graph (GetDependencies / GetDependents).
    // Provenance answers "what asset was this produced from during import?" —
    // e.g. ExportModelMaterials writes Materials/X.material from a glb;
    // RegisterProvenance(materialGuid, modelGuid, "ExportModelMaterials")
    // records that lineage so the editor can surface it (delete-confirm
    // dialog, reimport flow, asset panel "produced from X" badge).
    //
    // Backed by the v6 provenance table on the project source's SQLite
    // cache; mirrored by an in-memory cache populated lazily on first
    // touch (no warm-start cost when the feature is unused).
    // -------------------------------------------------------------------------

    /**
     * @brief Register an importer-time produced->producer relationship.
     *
     * Both GUIDs must be registered with the registry. Idempotent: re-registering
     * the same produced GUID with a different producer overwrites (the schema's
     * 1:1 produced->producer cardinality is enforced by the SQLite PK).
     *
     * @return true on success; false on null GUIDs or unregistered inputs.
     */
    bool RegisterProvenance(const GUID& produced,
                            const GUID& producer,
                            std::string_view importerId);

    /**
     * @brief Drop the provenance row for the produced GUID.
     *
     * Called from the SQLite-asset-row removal paths (not from MarkMissing
     * tombstoning — provenance survives tombstones). Idempotent — no-op if
     * no row exists.
     */
    void UnregisterProvenance(const GUID& produced);

    /**
     * @brief List the assets this producer wrote during import.
     *
     * Returns an empty list if the producer has no recorded provenance.
     */
    Vector<GUID> GetProducedAssets(const GUID& producer) const;

    /**
     * @brief Find the producer that wrote this produced asset, if any.
     *
     * Returns GUID::Null() if the asset has no recorded producer
     * (i.e. it was authored, not produced).
     */
    GUID GetProducer(const GUID& produced) const;

    /**
     * @brief List missing (tombstoned) assets of the project mount.
     *
     * Covers both residences of the missing flag: the journaled record field
     * (stored-identity mounts) and the per-machine cache's ghost set
     * (derived-identity mounts, whose git-shared journal never carries disk
     * observations). One entry per store record, so a record flagged in both
     * appears once; a cache ghost row with no record behind it is not
     * surfaced, having no path or type to report.
     */
    struct MissingAssetInfo
    {
        GUID guid;
        AssetType type = AssetType::Unknown;
        std::filesystem::path lastKnownPath; // absolute (may not exist)
        size_t dependentCount = 0;
    };
    Vector<MissingAssetInfo> GetMissingAssets() const;

    /**
     * @brief Relink an existing GUID to a new on-disk path (manual missing-asset resolution).
     * This preserves the GUID identity and updates the authoritative store.
     */
    bool TryRelinkAssetPath(const GUID& guid, const std::filesystem::path& newPath);

    /**
     * @brief Add/remove a redirect: a project-store row that forwards the `from` GUID
     *        to `to`, so references to an asset's old GUID keep resolving after it is
     *        renamed or re-identified.
     */
    bool AddRedirect(const GUID& from, const GUID& to);
    bool RemoveRedirect(const GUID& from);

    /**
     * @brief Rewrite references in text-based assets according to redirects, then remove redirects that were fully fixed.
     * Returns number of asset files modified.
     */
    size_t FixUpRedirects();

    /**
     * @brief Extract dependencies for every resident asset that has not been
     * extracted yet, so the reverse index answers about the project rather
     * than about what happens to have been queried. Returns the number
     * extracted.
     *
     * Nothing else populates that index: the startup scan writes no edges
     * (ShouldExtractDependenciesDuringScan is unconditionally false) and only
     * a forward query extracts, so an empty IterateDependents means "nobody
     * asked yet". Any sweep whose decision to DESTROY something reads the
     * index must either call this first or carry its own cold-index guard, as
     * the startup retention pass does — this re-reads and re-parses every
     * resident text asset and costs seconds on a real project, so it belongs
     * to deliberate user-initiated operations only, never startup or per-frame
     * paths.
     */
    size_t ExtractPendingDependencies();

    /**
     * @brief Returns any load/merge conflicts detected when reading the authoritative asset database file.
     */
    Vector<String> GetAssetDatabaseConflicts() const;

    /**
     * @brief Rename/move an asset path while preserving its GUID.
     *
     * Updates internal path->GUID mapping and the stored AssetMetadata::path/name/extension/type.
     * Intended to be called by file-watching driven systems (editor/runtime).
     */
    bool TryRenameAssetPath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);

    /**
     * @brief Unregister an asset by its on-disk path.
     */
    bool TryUnregisterAssetByPath(const std::filesystem::path& path);

    /**
     * @brief Update cached file stats for a registered asset (size/mtime) from disk.
     * Best-effort; returns false if the asset is not registered.
     */
    bool TryUpdateFilesystemMetadata(const std::filesystem::path& path);

    /**
     * @brief Get all registered assets
     */
    // Intentionally not exposed by reference: callers must use thread-safe snapshot APIs above.

    /**
     * @brief Check if asset is registered
     */
    bool IsAssetRegistered(const GUID& guid) const;
    bool IsAssetRegistered(const std::filesystem::path& path) const;

    /**
     * @brief Scan directory for assets (synchronous)
     */
    void ScanDirectory(const std::filesystem::path& directory, bool recursive = true);

    /**
     * @brief Scan directory for assets asynchronously
     */
    std::future<size_t> ScanDirectoryAsync(const std::filesystem::path& directory, bool recursive = true);

    /**
     * @brief Load asset metadata asynchronously
     */
    std::future<bool> LoadAssetMetadataAsync(const std::filesystem::path& assetPath);

    /**
     * @brief Save asset metadata asynchronously
     */
    std::future<bool> SaveAssetMetadataAsync(const AssetMetadata& metadata);

    /**
     * @brief Read an arbitrary key/value from the authoritative asset database (kv map).
     * @return true if the key was found and outValue was set.
     */
    bool TryGetMetaValue(const std::filesystem::path& assetPath,
                         const std::string& key,
                         std::string& outValue) const;

    /**
     * @brief Where this asset's import settings come from and what may be done
     * with them here.
     * Writable is true for project assets and for any mount that owns a writable
     * store (a local package, an engine package); false for a package cache
     * entry, for shipped content, and for a mount that publishes no store at
     * all. Editors ask before offering a field whose value they could not save,
     * and use the alias to name the package the author has to edit instead.
     */
    AssetImportSettingsOrigin GetImportSettingsOrigin(const std::filesystem::path& assetPath) const;

    /**
     * @brief Set or add an arbitrary key/value in the authoritative asset database (kv map).
     * Ensures the asset is registered first so it has a stable GUID identity.
     * Returns false for an asset owned by an immutable or read-only mount:
     * kv rows are store state, and those mounts are never written to.
     */
    bool SetMetaValue(const std::filesystem::path& assetPath,
                      const std::string& key,
                      const std::string& value);

    /**
     * @brief Journal a container's subasset derive keys (kSubassetDeriveKeysKvKey
     * kv row) so a container rename can cascade redirects for the identities
     * derived from its GUID, and clear any redirect that shadows one of THIS
     * container's own derived subasset identities (the subasset analog of the
     * stale-redirect-on-registration rule) — unless the container itself still
     * carries an outgoing redirect (live-rename residue), in which case its
     * cascades stay load-bearing. Called by container loaders after a
     * successful load; rejects immutable/read-only sources. Keys must be
     * non-empty and newline-free.
     */
    bool RegisterSubassetDeriveKeys(const std::filesystem::path& assetPath,
                                    const Vector<String>& keys);

    /**
     * @brief The container whose journaled derive keys (RegisterSubassetDeriveKeys) derive
     * `subasset`: for an embedded clip's GUID, the model that holds it. Null when no journal
     * names it, which includes every subasset of a container never loaded in this project,
     * since containers journal their keys as they load. The lookup is a map built from the
     * project store's journal on first use and kept current as containers journal.
     */
    GUID FindSubassetContainer(const GUID& subasset) const;

    /**
     * @brief Save the authoritative asset database to disk.
     */
    bool SaveToFile(const std::filesystem::path& path);

    /**
     * @brief Debounced persistence tick (flushes the authoritative store occasionally when dirty).
     * Called by AssetManager::Update().
     */
    void TickPersistence();

    /**
     * @brief Load registry from file
     */
    bool LoadFromFile(const std::filesystem::path& path);

    /**
     * @brief Get asset root directory (convenience for the "project" source root).
     */
    // Returns a copy of the project source's root. By value (not by const&)
    // because returning a reference into the SharedPtr-owned SourceEntry
    // would race Shutdown — the entry can be destroyed between return and
    // first use. Path copies are ~32 bytes; cost is negligible.
    std::filesystem::path GetAssetRoot() const;

    /**
     * @brief Get root directory for a specific source alias.
     */
    std::filesystem::path GetSourceRoot(std::string_view sourceAlias) const;

    /**
     * @brief The file a relative asset path names under the implicit mount priority.
     *
     * The project root first, then the other registered sources by priority; empty
     * when no mount has the file. AssetManager::ResolveAssetPath resolves through this,
     * and dependency extraction falls back to it for a path reference the project root
     * (and, for parser edges, the referrer's own folder) does not hold, so the reference
     * becomes an edge to the file the runtime loads for it, whichever mount holds it.
     */
    std::filesystem::path ResolveRelativeAssetPath(const std::filesystem::path& relativePath) const;

    /// Existing filesystem entry or logical identity in a read-only manifest mount.
    bool IsAssetPathAvailable(const std::filesystem::path& absolutePath) const;

    /**
     * @brief Get explicit source ownership alias for an asset path.
     *
     * Returns the alias recorded by RegisterAsset(path, preferredSourceAlias),
     * or empty when no explicit ownership has been recorded for this path.
     */
    std::string GetAssetSourceOwnerAlias(const std::filesystem::path& path) const;

    /**
     * @brief Get asset type registry
     */
    GameEngine::AssetTypeRegistry& GetTypeRegistry() { return m_TypeRegistry; }
    const GameEngine::AssetTypeRegistry& GetTypeRegistry() const { return m_TypeRegistry; }

    /**
     * @brief Classify an asset type for a path, using ParserRegistry sniffing when available.
     * Falls back to extension-based inference when no parser matches.
     */
    AssetType ClassifyAssetType(const std::filesystem::path& path) const;

    /**
     * @brief Get a snapshot of all registered asset paths (for bulk meta updates, e.g. removing a tag from all assets).
     */
    std::vector<std::filesystem::path> GetRegisteredAssetPaths() const;

    // Per-source snapshot record fingerprint, loaded from
    // <cacheRoot>/watcher.snapshot.bin at mount setup time. Used by the
    // SnapshotFingerprint lives in the AssetDatabase module; this alias keeps
    // existing references like `AssetRegistry::SnapshotFingerprint` working
    // while making the canonical name available elsewhere.
    using SnapshotFingerprint = AssetCore::SnapshotFingerprint;

    /**
     * @brief Copy the warm-start snapshot map for the source whose root is
     *        `directory` (or the project source if directory is the project root).
     *
     * Returns an empty map when no source covers the directory or the source
     * has no snapshot loaded. Used by the async scan path to short-circuit
     * per-file processing for files whose (mtime, size) match the snapshot
     * — those files are already populated in the registry maps from
     * PopulateHotCachesFromSource at Initialize time, so there's nothing for
     * the scan to do. Lock-once cost up front beats per-file lookups under
     * the registry mutex.
     */
    std::unordered_map<std::string, SnapshotFingerprint>
        CopySnapshotByPathForRoot(const std::filesystem::path& directory) const;

    /**
     * @brief Copy the warm-start per-directory mtime map for the source
     *        whose root is `directory`. Used by the scan path to gate
     *        subtree recursion: when the on-disk dir's mtime matches the
     *        snapshot's recorded mtime, the subtree is skipped.
     *
     * Returns an empty map when no source covers the directory or the
     * source has no snapshot loaded (cold-start).
     */
    std::unordered_map<std::string, int64_t>
        CopyDirectoryMtimesByPathForRoot(const std::filesystem::path& directory) const;

    // ------------------------------------------------------------------
    // Per-source entry. Replaces the old split between project-only
    // (m_store/m_Cache/m_assetRoot) and non-persistent (m_npStore/m_npCache/NonPersistentRoot).
    //
    // Public because ProjectSourcePinned() returns a SharedPtr<SourceEntry>;
    // callers (stress tests, future tools) need to name the type. The
    // fields are exposed by design — this struct is the unit of source
    // ownership in the registry. Callers MUST hold the registry mutex (or
    // a pin from ProjectSourcePinned()) to access non-atomic members.
    // ------------------------------------------------------------------
    struct SourceEntry
    {
        std::string Alias;
        // Absolute, lexically normal, ON-DISK spelling: the path the registry
        // opens and enumerates files from, so it must name a real directory.
        // Never case-folded — a full Unicode fold is 1:N ("Straße" → "strasse"),
        // and creating that folded path makes a phantom root beside the real
        // one. Fold Root only to compare it (RootPrefixKey).
        std::filesystem::path Root;
        // E4: the folded, compare-only key for Root plus a trailing '/', so
        // path→source resolution is a prefix compare (no syscalls, no
        // per-source lexically_relative) on the hot path. Built by
        // MakeRootPrefixKey, which puts it in the same domain as every
        // NormalizePathForMap'd asset path in m_PathToGuid.
        std::string RootPrefixKey;
        // RootPrefixKey built from Root with every symlink resolved. Read by
        // the RejectOverlappingRoots check only, so a root spelled through a
        // link cannot land inside another root unnoticed. Built once by
        // MakeCanonicalRootPrefixKey at registration and rebind.
        std::string CanonicalRootPrefixKey;
        GUID NamespaceGuid;                 // = GUID::Derive(Null, "asset-source-namespace:" + alias)
        int32_t Priority = 0;
        bool DerivedIdentity = true;

        // Lifecycle flags — copies of the AssetSourceDesc flags for runtime
        // decision-making; defaults match the standard project mount.
        // RegisterFileWatcher is intentionally absent: it's a mount-time
        // concern read by AssetManager directly from the desc; the registry
        // has no use for it after RegisterSource returns.
        bool RequiresScan = true;
        bool IsImmutable = false;
        bool AcceptsMetadataWrites = true;
        bool StoreIsSharedAcrossProcesses = false;
        bool InfersTextureImportSettings = true;
        bool CooksDerivedArtifactsOnMiss = true;
        bool IsReadOnly = false;
        bool TracksTombstones = true;
        bool AggressiveTombstoneCleanup = true;
        bool RejectOverlappingRoots = false;
        uint32_t GhostRetentionSessions = 0;
        bool CollectNeverHealableGhosts = false;

        // Persistent store (null when no DB is configured for this source)
        UniquePtr<AssetDatabase::IAssetStore> Store;
        UniquePtr<AssetDatabase::IAssetDbCache> Cache;
        std::filesystem::path DbFile;
        std::filesystem::path CacheDbFile;
        // Published stored-identity manifest merged into Store at setup
        // (see AssetSourceDesc::IdentityManifestFile). Empty when unused.
        std::filesystem::path IdentityManifestFile;
        // Throttle for the "not in published identity; derived fallback GUID"
        // advisory: full Info once per source per session, Trace afterwards.
        std::atomic<bool> StoredFallbackLogged{false};
        // Throttle for the refused-metadata-write warning: the material-bind
        // auto-tagger retries on every bind, so the reason is stated once.
        std::atomic<bool> MetaWriteRefusalLogged{false};
        std::atomic<bool> StoreDirty{false};
        // True while an async flush is in flight; prevents concurrent flushes
        // and keeps main thread non-blocking in TickPersistence.
        std::atomic<bool> FlushInProgress{false};
        std::mutex StoreFlushMutex;
        std::chrono::steady_clock::time_point LastStoreFlush{};

        AssetIgnoreRules IgnoreRules;
        std::future<size_t> StartupScanFuture;

        // Phase 5: warm-start snapshot loaded at mount setup. Keyed by the
        // FOLDED canonical relative path, so a record and the disk file it
        // describes meet whatever spelling each side carries. Safe to fold
        // because nothing rebuilds a path from these keys — the value holds
        // everything a lookup needs. Consumed by the reconcile path (delta
        // scan) so files whose fingerprints match disk are skipped instead of
        // re-stat'd. Empty when no snapshot existed or it failed to load.
        std::unordered_map<std::string, SnapshotFingerprint> SnapshotByPath;

        // C.1: per-directory mtimes from the warm-start snapshot. Keyed by the
        // canonical-relative path of the directory under `Root` in the spelling
        // the snapshot recorded — NOT folded, because the scan joins these keys
        // back onto `Root` to stat the directory, and on a case-sensitive
        // filesystem only the recorded spelling names it. Empty key
        // (the mount root itself) is excluded by the snapshot writer. Used
        // by the warm-path scan/reconcile to skip subtrees whose dir mtime
        // is unchanged since the snapshot was written.
        std::unordered_map<std::string, int64_t> SnapshotDirMtimeByPath;

        // F.5: cache mutation count captured immediately after the snapshot
        // was loaded. At Shutdown, if Cache->GetMutationCount() still equals
        // this baseline AND StoreDirty == false, the on-disk snapshot is
        // current and the write can be skipped (saves ~400ms at 50K).
        // 0 means "no baseline captured" (cold start) — the gate doesn't
        // fire and the snapshot is written normally.
        uint64_t SnapshotLoadTimeMutationCount = 0;

        // Block D diagnostic: how many files the most recent reconcile pass
        // skipped re-hashing for because the snapshot's stat matched disk.
        // Reset to 0 at the start of each reconcile.
        size_t LastReconcileHashesReused = 0;

        // Diagnostic: per-record exists() checks the most recent reconcile
        // pass performed. The dir-mtime gate elides them for records in
        // clean directories, so a warm start over an unchanged tree reads 0.
        size_t LastReconcileExistenceChecks = 0;

        // S11: GUIDs of records currently in the GLOBAL maps attributed to
        // this source (owner-alias override first, else path-prefix — the
        // same rule the old unmount/rebind eviction walks applied). Lets
        // unmount/rebind evict O(source) instead of walking the whole
        // registry (~50 ms at 100k) under the writer lock. Maintained by
        // the Index/Unindex helpers at every global-map insert/erase
        // (O(1) amortized per record); overlay-resident records are NOT
        // indexed until their drain commits them. Guarded by
        // m_RegistryMutex (writer).
        FastHashSet<GUID> ResidentGuids;

        bool HasStore() const { return Store != nullptr; }
    };

    // Block A.2 pin point: copies m_ProjectSource under shared_lock and
    // returns the SharedPtr to the caller. See declaration in private
    // section below — moved here so the public stress test can name it.
    SharedPtr<SourceEntry> ProjectSourcePinned() const;

  private:
    // Persist durable identity-ish fields to the authoritative store (not a sidecar file).
    bool SaveAssetMetadata(const AssetMetadata& metadata);

    // Find the source whose root contains the given absolute path.
    // Returns nullptr if no source matches. Iterates by priority (descending).
    SourceEntry* FindSourceForPath(const std::filesystem::path& absPath);
    const SourceEntry* FindSourceForPath(const std::filesystem::path& absPath) const;

    // Find source entry by alias. Returns nullptr if not found.
    SourceEntry* FindSourceByAlias(std::string_view alias);
    const SourceEntry* FindSourceByAlias(std::string_view alias) const;

    // Block A.2 pin point for non-project sources: returns the m_Sources
    // SharedPtr co-owning `found`, so a caller that keeps using the entry
    // after releasing m_RegistryMutex survives a concurrent unmount.
    // Caller must hold m_RegistryMutex; returns null when `found` is null or
    // is no longer a member of m_Sources.
    SharedPtr<SourceEntry> PinSourceLocked(const SourceEntry* found) const;

    // Helper: load authoritative store + derived cache into a SourceEntry.
    bool SetupSourceStore(SourceEntry& entry);

    // Helper: run startup reconciliation, prune, legacy-path normalization, type fix-up.
    bool ReconcileSourceStore(SourceEntry& entry);

    // Helper: build the hot-cache records for a source's store into
    // outRecords (E3: staging output — the caller commits them under the
    // writer lock via CommitMetadataToMapsLocked). Safe to run without the
    // registry lock: reads only the entry, the parser registry pointer, and
    // the type registry (both immutable after Initialize).
    void PopulateHotCachesFromSource(SourceEntry& entry, Vector<AssetMetadata>& outRecords);

    // Retires the identities `entry`'s store evicted from their path keys by a
    // different SPELLING of the same path, redirecting each onto the row that
    // owns the path now (cascaded through its subasset derive keys). Returns
    // how many were retired; callers arm the resolve seam on a non-zero count.
    size_t DrainDisplacedIdentities(SourceEntry& entry);

    // Apply one fully-prepared metadata record to m_Assets / m_PathToGuid
    // (path-eviction, GUID-move, and refresh semantics of
    // RegisterAssetMetadata's tail). Caller must hold the writer lock and
    // have resolved Path/Name/Extension/Type/TypeId already.
    void CommitMetadataToMapsLocked(const AssetMetadata& md);

  private:
    // Unified source list, sorted by priority descending.
    //
    // Block A.2: SharedPtr (not UniquePtr) so callers that release the
    // registry lock can pin a source's lifetime via ProjectSourcePinned()
    // and keep using it while the registry's mutex is unheld. The atomic
    // refcount cost is dwarfed by the existing m_RegistryMutex traffic.
    std::vector<SharedPtr<SourceEntry>> m_Sources;

    // Cached SharedPtr to the "project" source for O(1) GetAssetRoot().
    // Updated when "project" is registered/unregistered/rebound. The
    // SharedPtr is co-owned with m_Sources; clearing m_Sources at Shutdown
    // does NOT immediately destroy the entry while a caller has a pinned
    // copy outstanding — the entry destructs when the last refcount drops.
    SharedPtr<SourceEntry> m_ProjectSource;

    // FindSubassetContainer's map from a derived subasset GUID to its container, built from the
    // project store's subasset journal on first use, updated by RegisterSubassetDeriveKeys and
    // dropped when a source is registered, unregistered or rebound. A container deleted since the
    // map was built keeps its entries until then; that is harmless, since a lookup that names it
    // leads to a load that fails, which playback reports and does not retry. Its own mutex, taken
    // before m_RegistryMutex and never while holding it.
    mutable std::mutex m_SubassetContainerMutex;
    mutable std::unordered_map<GUID, GUID> m_SubassetContainers;
    mutable bool m_SubassetContainersBuilt = false;
    void ForgetSubassetContainers();

    // ------------------------------------------------------------------
    // Project-source inline accessors.
    // These read directly from m_ProjectSource. Caller MUST hold
    // m_RegistryMutex (shared or exclusive) for the entire dereference
    // window — the SharedPtr's pointee is co-owned with m_Sources and
    // can be reassigned to nullptr by Shutdown/UnregisterSource (which
    // takes the unique_lock).
    //
    // BLOCK A.2 PIN PATTERN: callers that need to release the lock and
    // keep using the source MUST go through ProjectSourcePinned(), which
    // copies the SharedPtr under shared_lock so the local copy keeps the
    // SourceEntry alive even if the registry's m_ProjectSource is later
    // cleared. The raw accessors below stay valid only inside the lock.
    // ------------------------------------------------------------------
    const std::filesystem::path& ProjectRoot() const { static const std::filesystem::path empty; return m_ProjectSource ? m_ProjectSource->Root : empty; }
    AssetDatabase::IAssetStore* ProjectStore() const { return m_ProjectSource ? m_ProjectSource->Store.get() : nullptr; }
    AssetDatabase::IAssetDbCache* ProjectCache() const { return m_ProjectSource ? m_ProjectSource->Cache.get() : nullptr; }
    const std::filesystem::path& ProjectDbFile() const { static const std::filesystem::path empty; return m_ProjectSource ? m_ProjectSource->DbFile : empty; }
    AssetIgnoreRules ProjectIgnoreRules() const { return m_ProjectSource ? m_ProjectSource->IgnoreRules : AssetIgnoreRules{}; }

    // ------------------------------------------------------------------
    // Block A: lock-discipline foundation.
    //
    // LockedRef<T> bundles a shared_lock + raw pointer in one move-only
    // RAII handle so callers can't accidentally use a project-source
    // pointer outside its lock. Returned by the *Locked() accessors below.
    //
    // Usage:
    //   if (auto cache = reg.ProjectCacheLocked()) {
    //       cache->UpsertAsset(...);   // lock held for whole scope
    //   }
    //
    // The handle is null when no project source is registered or the
    // source has no store/cache configured. Default lock is *shared*
    // (reads dominate registry usage); use the helper for exclusive
    // mutations of SourceEntry contents.
    // ------------------------------------------------------------------
    template <typename T>
    class LockedRef
    {
    public:
        LockedRef() = default;
        LockedRef(std::shared_lock<std::shared_mutex> lock, T* ptr) noexcept
            : m_Lock(std::move(lock)), m_Ptr(ptr) {}

        LockedRef(LockedRef&&) noexcept = default;
        LockedRef& operator=(LockedRef&&) noexcept = default;
        LockedRef(const LockedRef&) = delete;
        LockedRef& operator=(const LockedRef&) = delete;

        T* operator->() const noexcept { return m_Ptr; }
        T& operator*() const noexcept { return *m_Ptr; }
        T* get() const noexcept { return m_Ptr; }
        explicit operator bool() const noexcept { return m_Ptr != nullptr; }

    private:
        std::shared_lock<std::shared_mutex> m_Lock;
        T* m_Ptr = nullptr;
    };

    LockedRef<AssetDatabase::IAssetStore> ProjectStoreLocked() const;
    LockedRef<AssetDatabase::IAssetDbCache> ProjectCacheLocked() const;

    // Run `f(SourceEntry&)` under a shared lock if the project source is
    // registered. No-op otherwise. The closure receives a reference (not
    // pointer) so the body can't accidentally smuggle the pointer out.
    // For mutations that require an exclusive lock, use
    // WithProjectSourceExclusive instead.
    template <typename F>
    void WithProjectSource(F&& f) const
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (m_ProjectSource)
            f(*m_ProjectSource);
    }

    template <typename F>
    void WithProjectSourceExclusive(F&& f)
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (m_ProjectSource)
            f(*m_ProjectSource);
    }

    // ------------------------------------------------------------------
    // Non-persistent (derived-identity) source helpers.
    // These replace the old m_npStore / m_npCache / m_nonPersistentRoots
    // shadow fields by computing the result from m_Sources on the fly.
    // Caller must hold m_RegistryMutex.
    // ------------------------------------------------------------------

    // Find the first derived-identity source that has a store configured.
    SourceEntry* FindFirstDerivedSourceWithStore() const
    {
        for (const auto& src : m_Sources)
        {
            if (src->DerivedIdentity && src->HasStore())
                return src.get();
        }
        return nullptr;
    }

    // Resolve derived-source identity context for a non-project asset path.
    // Iterates all derived-identity sources to find the matching root.
    // Returns false if the path doesn't belong to any derived source.
    // outCanonicalRel (when provided) receives the canonical source-relative path.
    bool TryBuildSourceScopedKeyForPath(const std::filesystem::path& absPath,
                                        std::string& outKey,
                                        GUID& outNamespaceGuid,
                                        std::string* outCanonicalRel = nullptr) const;

    // Aggressive-tombstone-cleanup helper. Given a GUID that's about to be
    // removed (or has been removed) from the SQLite store, walks its outgoing
    // dep edges and recursively cleans up any newly-orphaned downstream
    // assets. An asset is "orphaned" when:
    //   - It has zero GUID-form referrers (excluding tombstones)
    //   - It has zero path-form referrers
    //   - It has zero produced provenance children
    //
    // Operates iteratively with a visited set to handle cycles. Acquires the
    // registry writer lock internally; caller must NOT hold it.
    void CleanupOrphanedDependenciesCascade(const GUID& startGuid,
                                            AssetDatabase::IAssetStore* store,
                                            AssetDatabase::IAssetDbCache* cache);

    // E4: transparent hasher so the path maps can be probed with
    // string_view keys (no per-lookup std::string construction where the
    // caller already holds a view).
    struct PathKeyHash
    {
        using is_transparent = void;
        using is_avalanching = void;
        uint64_t operator()(std::string_view v) const noexcept
        {
            return ankerl::unordered_dense::hash<std::string_view>{}(v);
        }
        uint64_t operator()(const std::string& v) const noexcept
        {
            return operator()(std::string_view(v));
        }
    };

    // ------------------------------------------------------------------
    // S10b: the global maps are hash-sharded into fixed sub-tables so any
    // single container growth event (values-vector realloc, bucket-array
    // rehash at a capacity crossing) is bounded by shard size (~1/32 of the
    // registry) instead of the whole table — the residual O(N)-under-lock
    // wall that time-boxed drain sections cannot split, measured at
    // 26-47 ms per crossing at 100k records unsharded. Routing is a byte
    // extract for GUIDs (uniform: GUIDs are random/hashed) and one wyhash
    // for path keys — noise against the path-normalization cost that
    // dominates every path lookup. This is storage partitioning only: one
    // mutex, one lock discipline, and all access flows through the routers
    // + the overlay-aware *Locked helpers below (grep-audited; the arrays
    // are never touched directly outside them).
    //
    // Path shards are keyed by the canonical UTF-8 string form of the
    // normalized path (PathMapKey in AssetRegistry.cpp) — see E4.
    // ------------------------------------------------------------------
    static constexpr size_t kMapShardCount = 32; // power of two
    using AssetShardMap = FastHashMap<GUID, AssetMetadata>;
    using PathShardMap = FastHashMap<std::string, GUID, PathKeyHash, std::equal_to<>>;
    std::array<AssetShardMap, kMapShardCount> m_AssetShards;
    std::array<PathShardMap, kMapShardCount> m_PathShards;

    AssetShardMap& AssetShard(const GUID& guid)
    {
        return m_AssetShards[guid.GetData()[0] & (kMapShardCount - 1)];
    }
    const AssetShardMap& AssetShard(const GUID& guid) const
    {
        return m_AssetShards[guid.GetData()[0] & (kMapShardCount - 1)];
    }
    PathShardMap& PathShard(std::string_view pathKey)
    {
        return m_PathShards[PathKeyHash{}(pathKey) & (kMapShardCount - 1)];
    }
    const PathShardMap& PathShard(std::string_view pathKey) const
    {
        return m_PathShards[PathKeyHash{}(pathKey) & (kMapShardCount - 1)];
    }

    // ------------------------------------------------------------------
    // S10 mount overlay: a mounting source's staged records, built entirely
    // off-lock by RegisterSource and published under one O(1) writer section
    // — the atomic visibility point that keeps mounts atomic to readers —
    // then drained into m_Assets/m_PathToGuid in time-boxed writer sections
    // so the maximum reader stall is bounded by the drain budget instead of
    // the source's record count. Steady state has zero overlays; every
    // access goes through the overlay-aware *Locked helpers below, which
    // reduce to a plain global-map probe when m_MountOverlays is empty.
    //
    // Read semantics during a drain window: overlays win path conflicts
    // (matching splice-evict order), GUIDs are unique across layers, and
    // aggregate queries see the union. Guarded by m_RegistryMutex.
    // ------------------------------------------------------------------
    struct MountOverlay
    {
        std::string SourceAlias;
        FastHashMap<GUID, AssetMetadata> Assets;
        FastHashMap<std::string, GUID, PathKeyHash, std::equal_to<>> PathToGuid;
    };
    std::vector<UniquePtr<MountOverlay>> m_MountOverlays;

    // S10c overlap remap aliases: when an overlapping source re-claims an
    // already-registered path (RegisterAsset(path, preferredSourceAlias)
    // reassigning the derived GUID, the E5 flow), the OLD GUID's map entry
    // is erased and the record re-keyed — but callers that resolved the old
    // GUID before the remap (scene refs mid-parse, queued loads) still hold
    // it. The GuidRemapCallback re-keys AssetManager's maps, yet between
    // the map rewrite and any such re-key the registry itself answered
    // "unknown GUID" for the old identity — the overlapping-source
    // load-null window. This table records old→new IN THE SAME writer
    // section as the rewrite, and FindAsset*Locked chase it ONLY on a
    // primary miss, so old-GUID lookups stay reachable with zero window and
    // zero hot-path cost for direct hits. Entries are session-scoped
    // (remaps re-derive deterministically every session). A registration of
    // an aliased GUID retires its alias (RetireStaleAliasesOnRegistrationLocked):
    // the path speaks for itself again, and the alias must not resurface
    // once that registration is gone. Guarded by m_RegistryMutex.
    FastHashMap<GUID, GUID> m_GuidRemaps;

    // S10 atomic path→GUID claim table: pending first-registration identity
    // reservations, arbitrated under the writer lock BEFORE any store write.
    // Both random-mint sites (RegisterAsset and GetOrCreateAssetGUID) claim
    // here, so racing first registrations of one path converge on a single
    // GUID and the store only ever sees the winner — closing the dual-mint
    // window behind the PinnedSource flake family. Claims are consumed when
    // the registration commits to the maps; a stale claim (aborted
    // registration) merely pins path→GUID determinism until its source is
    // unregistered. Guarded by m_RegistryMutex.
    FastHashMap<std::string, GUID, PathKeyHash, std::equal_to<>> m_PathClaims;

    // Overlay-aware map accessors. Caller must hold m_RegistryMutex (shared
    // for finds/iteration/count, exclusive for mutation and claims).
    const GUID* FindPathGuidLocked(std::string_view pathKey) const;
    const AssetMetadata* FindAssetLocked(const GUID& guid) const;
    // Primary maps only — no S10c alias chase. Occupancy questions (the
    // derive-collision guard, the heal's migration target) must see the maps
    // as they are: an alias is shadowed by a real registration, so a GUID
    // reachable only through m_GuidRemaps is free, not occupied.
    const AssetMetadata* FindAssetPrimaryLocked(const GUID& guid) const;
    AssetMetadata* FindAssetMutableLocked(const GUID& guid);
    bool ErasePathMappingLocked(std::string_view pathKey);
    bool EraseAssetEntryLocked(const GUID& guid);
    // S12: exact registry occupancy — global shard records + published
    // overlay records (the same union GetAssetCount always reported).
    // Mutated ONLY under the writer lock at the same compile-enforced choke
    // points that maintain ResidentGuids: every insert/erase applies its net
    // delta exactly once (commit paths batch a whole record's delta so
    // lock-free readers never see an intra-record dip), overlay publish
    // adds the set size, drain/evict sections apply their per-section sums,
    // discard subtracts what it drops. Exact at every operation boundary;
    // transient by at most one in-flight section's records.
    //
    // std::atomic with RELAXED ordering throughout: the count is a
    // standalone scalar with no ordering dependency on the maps — a reader
    // acting on it never dereferences map state "as of" the count, so no
    // acquire/release pairing is needed. This makes GetAssetCount a
    // lock-free load; the old shared-lock walk summed 32 shards + overlays
    // and measured 3-14 ms max latency while mount-family writers held the
    // lock under machine load.
    std::atomic<size_t> m_AssetCount{0};

    void ApplyAssetCountDelta(ptrdiff_t delta)
    {
        if (delta > 0)
            m_AssetCount.fetch_add(static_cast<size_t>(delta), std::memory_order_relaxed);
        else if (delta < 0)
            m_AssetCount.fetch_sub(static_cast<size_t>(-delta), std::memory_order_relaxed);
    }

    template <typename F>
    void ForEachAssetLocked(F&& f) const
    {
        for (const auto& shard : m_AssetShards)
            for (const auto& [guid, md] : shard)
                f(guid, md);
        for (const auto& overlay : m_MountOverlays)
            for (const auto& [guid, md] : overlay->Assets)
                f(guid, md);
    }

    template <typename F>
    void ForEachPathGuidLocked(F&& f) const
    {
        for (const auto& shard : m_PathShards)
            for (const auto& [key, guid] : shard)
                f(key, guid);
        for (const auto& overlay : m_MountOverlays)
            for (const auto& [key, guid] : overlay->PathToGuid)
                f(key, guid);
    }

    // Atomic first-mint arbitration (see m_PathClaims). Returns the winning
    // GUID for the path: an already-registered binding, a pending claim, or
    // — when neither exists — @p candidate, recorded as the new claim.
    GUID ClaimPathGuidLocked(const std::string& pathKey, const GUID& candidate);

    // Commit one record into the GLOBAL maps only (the pre-S10
    // CommitMetadataToMapsLocked body). By value: the drain loop moves
    // records straight from its overlay into the shard (no deep
    // AssetMetadata copy under the lock); lvalue callers pay the same one
    // copy they always did, at the call site.
    //
    // S12: returns the net occupancy delta (+1 fresh insert, 0 refresh,
    // -1 when the destination-path evict removed another record and this
    // one refreshed, etc.). [[nodiscard]] so every caller must fold it into
    // m_AssetCount via ApplyAssetCountDelta — batched with any paired
    // overlay erase so the counter never transiently dips mid-record.
    [[nodiscard]] ptrdiff_t CommitToGlobalMapsLocked(AssetMetadata md);

    // Drain the published overlay for @p alias into the global maps in
    // writer sections bounded by kSpliceDrainBudget. Takes its own locks;
    // returns when the overlay is gone (fully drained, discarded by an
    // unregister/rebind, or shutdown).
    struct DrainStats
    {
        size_t Sections = 0;
        double MaxSectionMs = 0.0;
        size_t SectionsOverMs = 0; // sections that ran past 1 ms wall
    };
    DrainStats DrainMountOverlay(std::string_view alias);

    // S11: evict a captured set of global-resident records in paced,
    // budget-boxed writer sections (the unmount/rebind counterpart of
    // DrainMountOverlay). Records whose GUID is present in @p
    // overlaySkipAlias's published overlay are skipped — the drain refreshes
    // them in place, so shared-identity rebinds never expose a resolution
    // gap. Takes its own locks; returns DrainStats-shaped section timings.
    DrainStats EvictSourceRecordsPaced(FastHashSet<GUID>&& guids, std::string_view overlaySkipAlias);

    // Discard a source's pending overlay + purge its path claims. Caller
    // holds the writer lock. Used by UnregisterSource/RebindSource/Shutdown.
    void DiscardOverlayForAliasLocked(std::string_view alias);
    void PurgeClaimsUnderPrefixLocked(std::string_view rootPrefixKey);

    // ------------------------------------------------------------------
    // S11 per-source resident-GUID index maintenance. Caller holds the
    // writer lock. Insert attributes by the eviction rule (owner-alias
    // override, else priority-ordered path prefix); erase searches the
    // (few) sources' sets directly so attribution-time skew can never
    // strand a stale membership.
    // ------------------------------------------------------------------
    void IndexResidentGuidLocked(const GUID& guid, const std::filesystem::path& path);
    void UnindexResidentGuidLocked(const GUID& guid);
    // Owner-alias recorded/changed for a bound path: move its GUID's index
    // membership to the owning source (matches the eviction rule's
    // owner-first branch).
    void MoveResidentGuidToOwnerLocked(const GUID& guid, std::string_view ownerAlias);


    // Provenance in-memory cache. Mirrors the project source's SQLite
    // provenance table for O(1) GetProducer / GetProducedAssets lookups.
    // Populated lazily on first touch (PopulateProvenanceCacheLocked) so
    // sessions that never query provenance pay no warm-start cost.
    // Both maps protected by m_RegistryMutex.
    HashMap<GUID, GUID> m_ProvenanceProducer;             // produced -> producer
    HashMap<GUID, Vector<GUID>> m_ProvenanceProduced;     // producer -> produced[]
    bool m_ProvenanceCachePopulated = false;

    // Explicit source-ownership map (PathMapKey(normPath) -> owning source
    // alias). Populated by RegisterAsset(path, preferredSourceAlias).
    // Consulted by GetAssetsForSource / RebindSource / UnregisterSource
    // to avoid ejecting assets that belong to a different overlapping source.
    FastHashMap<std::string, std::string, PathKeyHash, std::equal_to<>> m_AssetSourceOwner;

    // E3: aliases reserved by an in-flight RegisterSource whose staging work
    // runs off-lock. Guarded by m_RegistryMutex.
    FastHashSet<std::string> m_PendingSourceAliases;

    // Missing-file log episodes (PathMapKey'd). Hot callers (per-frame texture
    // resolution, asset retry loops) re-probe the same missing path every
    // frame; RegisterAsset logs the ERROR once per episode and stays silent
    // until the file appears, which clears the episode (transition-logged) so
    // a later disappearance is a fresh episode. Own mutex — the miss path
    // runs outside m_RegistryMutex and must not contend with registry reads.
    mutable std::mutex m_MissingAssetLogMutex;
    FastHashSet<std::string> m_MissingAssetLogEpisodes;

    // E5: guarded by m_RegistryMutex; copied out before invocation so the
    // callback runs lock-free.
    GuidRemapCallback m_GuidRemapCallback;

    bool m_Initialized = false;
    ParserRegistry* m_ParserRegistry = nullptr;
    GameEngine::AssetTypeRegistry m_TypeRegistry;

    // E6a: redirect empty fast-path. TryGetAssetMetadata used to pay up to
    // 8 ResolveRedirect store probes per lookup even with zero redirects.
    // Maintained on redirect add/remove/load; read relaxed on the lookup
    // hot path. False negatives are impossible (every mutation path
    // refreshes it); a stale `true` only costs the old probe behaviour.
    std::atomic<bool> m_ProjectHasRedirects{false};

    // Recompute m_ProjectHasRedirects from the project store. Caller must
    // hold m_RegistryMutex (shared or exclusive) for the ProjectStore()
    // dereference.
    void RefreshProjectRedirectFlagLocked();

    // Registration-time stale-redirect clearing (#965): a (re)registered GUID
    // that is a redirect SOURCE is shadowed by that redirect whenever the
    // chased chain-final target has a live record — remove the source's
    // outgoing redirect then; keep it when the target is recordless (that
    // shape resolves old references forward: live-rename residue and
    // derived→stable identity redirects). The session remap alias for the
    // GUID goes unconditionally. Caller must hold the writer lock.
    void RetireStaleAliasesOnRegistrationLocked(const GUID& guid);

    // Rename-kept identity heal. A live rename keeps the renamed asset's
    // session GUID attached to its NEW path (TryRenameAssetPath), so the first
    // registration of the OLD path finds its own derived identity occupied by
    // an asset that no longer lives there. Migrate that asset onto the GUID its
    // current path derives — the identity a fresh session would give it — via
    // the E5 remap machinery, freeing `collidingGuid` for the registrant.
    // Returns false when the occupancy is NOT the rename-kept shape (a genuine
    // derive collision), leaving all state untouched. Caller must hold the
    // writer lock, copy any `holder` fields needed afterwards BEFORE the call
    // (the entry is erased), and fire the GuidRemapCallback with
    // (collidingGuid, outMigratedTo) after releasing the lock.
    bool TryHealRenameKeptIdentityLocked(const GUID& collidingGuid,
                                         const AssetMetadata& holder,
                                         SourceEntry& source,
                                         const std::filesystem::path& projectRoot,
                                         GUID& outMigratedTo);

    // Guards all registry state below (maps, init flags, and async coordinator).
    // Shared lock for reads; unique lock for mutations.
    mutable std::shared_mutex m_RegistryMutex;

    // Job system integration
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
    UniquePtr<AsyncRegistryCoordinator> m_AsyncCoordinator;

    // Injected derived-cache factory (see SetAssetDbCacheFactory). Null in
    // runtimes that don't link a cache implementation (the Player), which is
    // what makes them cache-less and SQL-free.
    static AssetDatabase::AssetDbCacheFactory s_CacheFactory;
};

} // namespace GameEngine
