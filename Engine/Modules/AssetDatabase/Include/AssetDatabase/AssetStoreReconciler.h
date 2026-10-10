#pragma once

#include "AssetCore/AssetIgnoreRules.h"
#include "AssetDatabase/AssetRecord.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::AssetDatabase
{

/**
 * Remove records from `store` (and `cache`, if non-null) whose canonical
 * relative path matches the ignore rules. Returns true if any record was
 * removed and writes the count to `outRemoved`.
 */
bool PruneIgnoredStoreRecords(const AssetIgnoreRules& rules,
                              IAssetStore& store,
                              IAssetDbCache* cache,
                              size_t& outRemoved);

/**
 * Some builds/tools previously wrote canonical paths relative to the project root
 * (e.g. "Assets/Icons/Foo.png") into the authoritative DB. When the runtime is
 * configured with assetRoot="<ProjectRoot>/Assets", joining these produces a
 * duplicated "Assets/Assets/..." path. We normalize these legacy paths by
 * stripping a leading "Assets/" segment when the current assetRoot is itself an
 * "Assets" directory (and the legacy "<assetRoot>/Assets/..." resolution does
 * not actually exist on disk).
 *
 * Operates through the IAssetStore interface — no JSONL-specific calls.
 */
bool NormalizeLegacyStorePathsIfNeeded(const std::filesystem::path& assetRoot,
                                       IAssetStore& store,
                                       IAssetDbCache* cache,
                                       size_t& outFixed,
                                       size_t& outConflicts);

/**
 * Mount-time pass over the records the JSONL load quarantined for violating
 * the canonical-relative path contract (absolute paths — cross-mount leaks
 * and legacy absolute writes; see AssetStore_TextJsonl).
 *
 * A quarantined record whose absolute path resolves under `assetRoot` is
 * re-admitted to the store with its path rewritten to canonical-relative
 * (GUID and kv preserved) unless the relative key already belongs to a
 * different GUID — then the quarantined row is a stale duplicate. Every
 * other quarantined record is dropped: it never entered the store, so the
 * only cleanup is scrubbing its derived-cache rows (asset row incl.
 * fingerprint, kv mirror, dep edges, provenance) so nothing
 * re-fingerprints it in later sessions. Dropped records are never
 * journaled — the on-disk file keeps its historical lines untouched until
 * the next compaction rewrites them away.
 *
 * Operates through the IAssetStore interface — no JSONL-specific calls.
 * Returns true if the store changed (i.e. any record was repaired).
 */
bool RecoverQuarantinedStoreRecords(const std::filesystem::path& assetRoot,
                                    const std::vector<AssetRecord>& quarantined,
                                    IAssetStore& store,
                                    IAssetDbCache* cache,
                                    size_t& outRepaired,
                                    size_t& outDropped);

/**
 * Mount-time pass over the records the JSONL load displaced: rows whose path
 * key was claimed by a later journal line under a different GUID. In practice
 * these are case-shadows — the same file registered twice under two spellings
 * of its path, back when the store keyed rows by spelling rather than by the
 * folded identity key.
 *
 * The surviving row is the identity; the displaced GUID is retired onto it by
 * the same protocol a rename heal uses: per-key copy-if-absent kv migration
 * onto the survivor, then the subasset cascade (the displaced record's
 * journaled derive keys re-derive Derive(old, key) -> Derive(new, key), so
 * scenes binding embedded materials or clips keep resolving), then the
 * container redirect old->new, then the derived-cache scrub. Order matches the
 * rename heal: an interruption at any point leaves cascade-without-container
 * residue at worst, and the next mount re-runs the whole pass idempotently.
 *
 * Journals only what identity movement requires. Nothing is emitted for a
 * displaced GUID whose path key no longer resolves, or that resolves to
 * itself.
 *
 * Operates through the IAssetStore interface — no JSONL-specific calls.
 * Returns true if any displaced row was processed — including re-drains that
 * insert no new redirects, because the caller's flush is what compacts the
 * dead displaced rows out of the journal. Counters report actual insertions.
 */
bool MergeDisplacedStoreRecords(const std::vector<AssetRecord>& displaced,
                                IAssetStore& store,
                                IAssetDbCache* cache,
                                size_t& outRedirected,
                                size_t& outSubassetRedirects);

/**
 * GUID assignment strategy of the mount being reconciled. Selects where the
 * missing flag rests and what a rename match produces:
 * - Stored: GUIDs are db-authoritative. The missing flag is a journaled store
 *   field, and a rename match rebinds the record to its new path — the GUID
 *   is preserved. New-file candidates come from a full disk walk.
 * - Derived: GUIDs are path hashes, so rebinding is impossible by construction
 *   (a new path IS a new GUID); a rename match instead emits a journaled
 *   redirect old→new, migrates the old record's kv, and removes the ghost
 *   record — references heal at resolve time by chasing the redirect. The
 *   missing flag rests in the per-machine derived cache — the git-shared
 *   journal holds identity and user-authored metadata only, never per-machine
 *   disk observations; redirects ARE identity movement and are journaled.
 *   New-file candidates are the scan's !hadExisting registrations (never a
 *   disk walk). Requires a non-null cache; with none the pass is skipped
 *   entirely.
 */
enum class ReconcileIdentityScheme
{
    Stored,
    Derived,
};

/**
 * A file the scan registered that had no store record before this session
 * (the scan's !hadExisting set) — the Derived scheme's rename-candidate side.
 * Carries the fingerprint already computed for the cache batch; the reconcile
 * pass never re-reads the file.
 */
struct ReconcileScanNewFile
{
    /// Derived (path-hash) GUID the file registered under.
    GUID Guid;
    /// Canonical path relative to the mount root.
    std::string CanonicalRel;
    int64_t Size = 0;
    int64_t Mtime = 0;
    /// OS file identity (volume+index / dev+inode); empty when unavailable.
    std::string FileId;
    /// Partial/sparse content hash; empty when the compute failed.
    std::string ContentHash;
};

/// One Derived-scheme heal: references to From now resolve to To via the
/// journaled redirect. Reported so the registry can drop From's resident
/// in-memory entry.
struct ReconcileHeal
{
    GUID From;
    GUID To;
};

/**
 * Ghost retention policy for the Derived scheme (S3).
 *
 * A ghost is a record whose file is gone. Keeping it is what makes a dangling
 * reference resolve to a known-missing identity instead of to nothing, so
 * collecting one is never free — it is traded against the cost of carrying it,
 * which is one exists() syscall per ghost per session forever (a standing
 * ghost is exempt from the dir-mtime gate by construction) plus a permanent
 * entry in every missing-assets surface.
 *
 * Default-constructed, this disarms retention completely: no ghost is ever
 * collected. Nothing collects without a mount opting in.
 *
 * Three invariants hold whatever the policy says, so an armed mount cannot
 * lose a reference target or a heal:
 *  - A ghost with any dependent, any path-form referrer, or any produced asset
 *    is never collected. Same predicate the live-delete path already uses to
 *    decide tombstone-vs-remove (AggressiveTombstoneCleanup).
 *  - Collection runs AFTER the rename matcher, so a ghost that heals this
 *    session heals; only survivors are candidates.
 *  - Age is counted from the session the record was first observed missing,
 *    never inferred. A ghost the cache has no stamp for is stamped on sight
 *    and starts at age zero.
 */
struct ReconcileGhostRetention
{
    /// Sessions a ghost must have been continuously missing to be collected.
    /// 0 keeps aged ghosts forever.
    uint32_t MaxMissingSessions = 0;

    /// Collect ghosts that no matcher tier can ever bind, regardless of age:
    /// the cache holds no fingerprint for them, so tiers 1-3 cannot compare
    /// bytes and even the advisory dir+ext tier never indexes them. Waiting
    /// cannot change their outcome — only a manual relink can, and that path
    /// is unaffected by the record's absence.
    bool CollectNeverHealable = false;

    bool Armed() const { return MaxMissingSessions > 0 || CollectNeverHealable; }
};

/// Evidence label written to IAssetDbCache::RenameSuggestion::EvidenceTier by
/// the per-(dir, ext) 1:1 uniqueness matcher — the only tier that suggests
/// rather than heals.
inline constexpr const char* kRenameEvidenceTierDirExt = "dir-ext-1:1";

/// Counters reported by StartupReconcileAssetDatabase.
struct ReconcileStats
{
    /// Records rebound to a new path with GUID preserved (Stored only).
    size_t Moved = 0;
    /// Absentees healed by redirect emission (Derived only).
    size_t Redirected = 0;
    /// Interrupted heals completed: absentees whose redirect landed in a
    /// previous run but whose record removal was lost (Derived only).
    size_t HealsCompleted = 0;
    /// New files whose fingerprint matched more than one missing record.
    size_t Ambiguous = 0;
    /// Weak-evidence pairings recorded as advisory cache suggestions instead
    /// of healed — the per-(dir, ext) 1:1 tier's only consequence. The
    /// absentee stays a tombstone; nothing is journaled.
    size_t Suggested = 0;
    /// Stale redirects removed: the source record is live and file-backed
    /// while the chain-final target record exists, so the redirect would
    /// shadow the live source (Derived only).
    size_t StaleRedirectsRemoved = 0;
    /// Subasset cascade redirects emitted alongside container heals: the
    /// container's journaled derive keys re-derive
    /// Derive(old, key) -> Derive(new, key) per key (Derived only).
    size_t SubassetRedirects = 0;
    /// Subasset cascade redirects removed alongside a stale container
    /// redirect — their sources shadowed the reborn container's own derived
    /// subasset identities (Derived only).
    size_t SubassetRedirectsRemoved = 0;
    /// Records the existence loop found absent from disk this run, before any
    /// heal or collection consumed them. Also the number of records exempt
    /// from the dir-mtime gate next session, since a standing ghost is
    /// re-checked every start.
    size_t MissingObserved = 0;
    /// Ghosts collected by the retention policy: record, cache row and
    /// derived rows all removed (Derived only, and only when armed).
    size_t GhostsCollected = 0;
    /// Ghosts that met the policy but were held back anyway: something still
    /// references them, or an unresolved rename suggestion still names them.
    /// Reported so an armed mount that collects nothing says why.
    size_t GhostsRetained = 0;
    /// Standing ghosts given a missing-since stamp for the first time. Ages
    /// start now rather than being invented for records that predate the
    /// stamp (Derived only, and only when armed).
    size_t GhostsStamped = 0;
    /// Rename suggestions dropped because their subject is no longer missing.
    size_t StaleSuggestionsPurged = 0;
    /// New-file hashes reused from the warm-start snapshot instead of recomputed.
    size_t HashesReused = 0;
    /// Per-record exists() checks performed — the dir-mtime gate elides them
    /// for records whose parent directory is unchanged since the snapshot.
    size_t ExistenceChecks = 0;
};

/**
 * Startup reconciliation pass. For every store record, verifies on-disk
 * existence (dir-mtime-gated: records in directories whose mtime matches the
 * warm-start snapshot are proven-existing with zero syscalls) and maintains
 * the missing flag — set when the file is absent, re-cleared when it returns.
 * The flag's resting place follows the identity scheme (see
 * ReconcileIdentityScheme).
 *
 * Both schemes then match missing records against new-file candidates via
 * cached fingerprints. Three tiers carry evidence strong enough to act on and
 * heal automatically: file_id, content hash + size, and (for large files the
 * cache never hashed) size + extension. A fourth tier — exactly one missing
 * record and one new file sharing a (parent dir, extension) bucket — carries
 * no fingerprint evidence at all, only co-location, so it never heals: it
 * records an advisory IAssetDbCache::RenameSuggestion and leaves the absentee
 * a tombstone. Confirming a suggestion is what emits the redirect; nothing in
 * this pass journals one on dir+ext evidence.
 * - Stored discovers candidates with a full disk walk (reusing snapshot
 *   fingerprints when (mtime, size, fileId) match the previous session) and
 *   rebinds matches in place, preserving GUIDs.
 * - Derived takes candidates from `scanNewFiles` — the scan's !hadExisting
 *   registrations — and heals matches by redirect: per-key copy-if-absent kv
 *   migration onto the new record, then AddRedirect(old→new), then ghost
 *   record removal, in that order so an interruption at any point either
 *   re-matches next startup or is finished by the completion sweep (an
 *   absentee whose redirect already points away gets its removal completed).
 *   Heals are appended to `outHeals` when non-null.
 *
 * Finally, both schemes drop rename suggestions whose subject is no longer
 * missing, and the Derived scheme applies `retention` to whatever ghosts
 * remain — see ReconcileGhostRetention for the invariants that hold whatever
 * the policy says. Collected GUIDs are appended to `outCollectedGhosts` when
 * non-null so the caller can drop their resident in-memory entries.
 *
 * Operates through the IAssetStore interface — no JSONL-specific calls.
 * Returns true if the STORE changed. Derived runs keep missing observations
 * cache-only but DO journal identity movement: redirects, migrated kv, and
 * ghost-record removals (whether from a heal or from retention).
 */
bool StartupReconcileAssetDatabase(const std::filesystem::path& assetRoot,
                                   const AssetIgnoreRules& rules,
                                   IAssetStore& store,
                                   IAssetDbCache* cache,
                                   ReconcileIdentityScheme scheme,
                                   const std::unordered_map<std::string, SnapshotFingerprint>* snapshotByPath,
                                   const std::unordered_map<std::string, int64_t>* snapshotDirMtimeByPath,
                                   const std::vector<ReconcileScanNewFile>* scanNewFiles,
                                   const ReconcileGhostRetention& retention,
                                   ReconcileStats& outStats,
                                   std::vector<ReconcileHeal>* outHeals,
                                   std::vector<GUID>* outCollectedGhosts);

} // namespace GameEngine::AssetDatabase
