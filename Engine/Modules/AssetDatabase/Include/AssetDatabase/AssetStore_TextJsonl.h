#pragma once

#include "AssetDatabase/IAssetStore.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::AssetDatabase
{

// Authoritative store backed by a single JSON Lines file (one record per line).
//
// File format v2 (current):
//   First non-comment line is a header record:
//     {"format":"assetdb","version":2}
//   Followed by asset and redirect records, in any mix of upsert and delete
//   shapes (replayed in file order; last write wins per guid / per
//   redirect_from):
//     Asset upsert:    {"guid":"...","path":"...","type":"Texture","missing":false,"kv":{...}}
//     Asset delete:    {"guid":"...","deleted":true}
//     Redirect upsert: {"redirect_from":"...","redirect_to":"..."}
//     Redirect delete: {"redirect_from":"...","redirect_to":""}
//
// SaveToFile flushes the dirty-set as delta-append records when the file
// is already v2 and the post-write append count is under the compaction
// threshold. Otherwise it rewrites the whole file as a fresh v2 snapshot
// (sorted by GUID, canonical key order). A save with nothing dirty leaves a v2
// file that loaded without conflicts untouched: the file the store last read
// or wrote or, under the cross-process lock, any file at the saved path (the
// save re-reads it). LoadFromFile reads either v1 (pre-version-header) or v2
// files transparently.
//
// Record paths are canonical-relative to the store's mount root — never
// absolute. UpsertAsset rejects absolute paths; LoadFromFile quarantines
// them (GetLoadQuarantinedRecords) instead of admitting them to the live
// maps, so the contract holds on both the write and read boundaries.
//
// Path IDENTITY is case-folded (AssetPaths::FoldStorePathKey): a row keeps the
// spelling it was written with, but one file is one row and one GUID whatever
// case a query or a historical journal line uses. A row displaced by another
// GUID taking its path key, in any spelling, is reported through
// TakeDisplacedRecords so the mount can redirect its GUID forward instead of
// dropping it.
class AssetStore_TextJsonl final : public IAssetStore
{
  public:
    /// @param parsePool The pool a load parses a large file's lines on
    ///        (ParallelFor; the replay into the maps stays on the calling
    ///        thread): LoadFromFile and the re-read of a cross-process store
    ///        under its write lock. Null parses on the calling thread; pass the
    ///        engine's pool for any store that loads a file a project can grow.
    explicit AssetStore_TextJsonl(JobSystem::WorkStealingThreadPool* parsePool);
    ~AssetStore_TextJsonl() override = default;

    bool LoadFromFile(const std::filesystem::path& filePath, std::string* outError) override;
    // The wait default repeats the interface's deliberately: a caller holding
    // this type concretely would not see the base declaration's, and the two
    // must never disagree.
    bool SaveToFile(const std::filesystem::path& filePath, std::string* outError,
                    StoreSaveWait wait = StoreSaveWait::NonBlocking) const override;

    /// Serialize every write of this store against other PROCESSES on an
    /// advisory lock at @p lockFile, for a store file more than one process can
    /// reach — an engine package's manifest is written by every editor and
    /// developer Player built from the same checkout. Without it two appends can
    /// land inside one another; a torn line is file damage, not a lost update.
    ///
    /// Whether a contended save waits is the caller's call (StoreSaveWait): the
    /// flush on a job worker fails rather than park it and retries next tick,
    /// while shutdown, unmount and rebind — which have no next tick — wait the
    /// peer out. Either way nothing is drained before the lock is held, so a
    /// refused save keeps its deltas.
    ///
    /// Holding it, every write becomes a read-merge-write: SaveToFile re-reads
    /// the file and replays this process's dirty deltas over it, so a compaction
    /// publishes the merge rather than the view this process has held since
    /// mount. Without that the first compaction past kCompactionMaxAge rewrites
    /// away every row another process committed in the meantime. Rows this
    /// process did not touch stay the file's; the ones it did are its own.
    ///
    /// Set once at setup, before the store is handed to anything that can write
    /// it: it is read unsynchronized on every save.
    void SetCrossProcessWriteLockFile(std::filesystem::path lockFile);

    bool UpsertAsset(const AssetRecord& record, std::string* outError) override;
    bool RemoveAsset(const GUID& guid, std::string* outError) override;
    bool MarkMissing(const GUID& guid, bool missing, std::string* outError) override;
    StoreMergeResult MergeObservation(const AssetObservation& observation,
                                      AssetRecord& outMerged,
                                      std::string* outError) override;

    bool TryGetAsset(const GUID& guid, AssetRecord& outRecord) const override;
    std::optional<GUID> LookupGuidByPath(const std::string& canonicalPath) const override;
    std::vector<AssetRecord> EnumerateAssets() const override;
    void IterateAssets(const std::function<void(const AssetRecord&)>& visit) const override;
    size_t CountAssets() const override;

    bool SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError) override;
    bool TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const override;

    bool AddRedirect(const GUID& from, const GUID& to, std::string* outError) override;
    bool RemoveRedirect(const GUID& from, std::string* outError) override;
    std::optional<GUID> ResolveRedirect(const GUID& from) const override;
    std::vector<RedirectRecord> EnumerateRedirects() const override;
    std::vector<GUID> FindRedirectSourcesTo(const GUID& target) const override;

    // Conflict / diagnostics
    std::vector<std::string> GetLoadConflicts() const override;
    bool HasLoadConflicts() const;

    // Records whose path violated the canonical-relative store contract
    // (absolute path) in the last LoadFromFile. Quarantined at load: they
    // never enter the live maps (not enumerable, not path-resolvable) and
    // are never journaled — no tombstones, so a polluted-but-tracked file
    // stays byte-identical across sessions. They remain in the on-disk
    // journal until the next compaction rewrites it. The mount path
    // (AssetRegistry::SetupSourceStore) consumes this list to repair
    // under-root rows and scrub stale derived-cache entries.
    std::vector<AssetRecord> GetLoadQuarantinedRecords() const;

    // Drains the records evicted since the last drain because another GUID
    // claimed their path key under a different SPELLING of it — "Models/X.gltf"
    // and "models/x.gltf" are one file, and one row is now its identity.
    // Accumulated by both journal replay and runtime writes.
    //
    // The evicted GUID may still be referenced by persisted content, so the
    // mount consumes this list (MergeDisplacedStoreRecords) to journal a
    // redirect onto the surviving identity — cascading through the record's
    // subassets.deriveKeys — rather than leaving the reference dangling.
    // Draining rather than peeking keeps the pass idempotent across the
    // several points a mount drains it.
    std::vector<AssetRecord> TakeDisplacedRecords() override;

    // Utility: normalize canonical relative paths stored in the authoritative file.
    static std::string NormalizeCanonicalPath(std::string s);

  private:
    // Lands a whole record in the live maps: path-uniqueness arbitration,
    // re-journal suppression, dirty marking. Requires m_Mutex, which is what
    // lets MergeObservation read and write a row without a concurrent writer
    // slipping between the two.
    StoreMergeResult CommitRecordLocked(const AssetRecord& record, std::string* outError);

    // Mutable for thread-safe const queries with a shared mutex would be ideal, but
    // we keep it simple for now: AssetRegistry is the high-level concurrency boundary.
    mutable std::mutex m_Mutex;

    std::unordered_map<GUID, AssetRecord> m_Assets;
    // Keyed by AssetPaths::FoldStorePathKey(record.path), not by the stored
    // spelling — see the class comment.
    std::unordered_map<std::string, GUID> m_PathToGuid;
    std::unordered_map<GUID, GUID> m_Redirects;
    // The reverse of m_Redirects (target -> the GUIDs redirected straight to it). Rebuilt on the
    // first FindRedirectSourcesTo after any redirect change, so a project whose redirects do not
    // change pays the O(redirects) build once. Guarded by m_Mutex like m_Redirects.
    mutable std::unordered_map<GUID, std::vector<GUID>> m_RedirectSourcesByTarget;
    mutable bool m_RedirectIndexStale = true;

    std::vector<std::string> m_LoadConflicts;
    std::vector<AssetRecord> m_LoadQuarantined;
    std::vector<AssetRecord> m_DisplacedRecords;

    // Dirty-set tracking for delta-append flushes (Phase 3.5).
    //
    // Each mutating op (UpsertAsset, RemoveAsset, MarkMissing, SetKeyValue,
    // AddRedirect, RemoveRedirect) adds the touched GUID to one of these
    // sets. SaveToFile drains them: for each dirty asset GUID it emits
    // either an upsert record (if still in m_Assets) or a delete record
    // (if absent), and likewise for redirects. Once drained, the file
    // record count grows by len(deltas). The drain is held by a scope object
    // (DrainedDirtySets in the .cpp) that merges the GUIDs back on every exit
    // but a successful write, a throw included, so the next save still writes
    // them.
    //
    // The sets are protected by m_Mutex; mutated under the same lock that
    // guards the in-memory maps.
    mutable std::unordered_set<GUID> m_DirtyAssets;
    mutable std::unordered_set<GUID> m_DirtyRedirects; // by redirect_from guid

    // Snapshot-or-append decision: the in-memory format version of the
    // last file we read or wrote. v1 (pre-header) means we must compact on
    // the next SaveToFile (to upgrade the on-disk file to v2). v2 means we
    // can append.
    mutable int m_LastWrittenFormatVersion = 0; // 0 = "no file written/read yet"
    // The file that version describes. A save with nothing dirty skips only
    // this file, the one known to hold every row the store has. Guarded by
    // m_Mutex.
    mutable std::filesystem::path m_LastReadOrWrittenFile;
    // That file loaded with conflicts (m_LoadConflicts; in a v2 file, lines
    // that do not parse, such as git conflict markers). Only a rewrite removes
    // them, so a save with nothing dirty still writes it whole. Cleared when a
    // snapshot is published. Guarded by m_Mutex.
    mutable bool m_LastReadFileHasLoadConflicts = false;

    // Compaction triggers — when either fires, the next SaveToFile rewrites
    // the file as a fresh sorted snapshot instead of appending. Keeps the
    // on-disk file from growing unbounded between compactions, and restores
    // the canonical GUID-sorted ordering for VCS-friendly diffs.
    static constexpr size_t kCompactionThreshold = 1000;
    static constexpr std::chrono::seconds kCompactionMaxAge{300}; // 5 minutes

    mutable size_t m_AppendsSinceCompaction = 0;
    mutable std::chrono::steady_clock::time_point m_LastCompactionTime{};

  public:
    // Test seam: lets tests reduce kCompactionThreshold to a small value so
    // they don't have to write 1000+ records to exercise the compaction
    // path. nullopt restores the default constant. Thread-safe; takes the
    // store mutex.
    void SetCompactionThresholdForTesting(std::optional<size_t> threshold);
    // Test seam mirroring the threshold override for the wall-clock trigger, so
    // a test can exercise age-driven compaction without sleeping for
    // kCompactionMaxAge. nullopt restores the default constant.
    void SetCompactionMaxAgeForTesting(std::optional<std::chrono::steady_clock::duration> maxAge);
    // Read-only diagnostic: how many appends have happened since the last
    // compaction (or since open if no compaction has happened yet). Useful
    // in tests to assert the dirty bookkeeping.
    size_t GetAppendsSinceCompactionForTesting() const;

  private:
    // Empty when this store's file is this process's alone (the common case).
    JobSystem::WorkStealingThreadPool* m_ParsePool = nullptr;
    std::filesystem::path m_CrossProcessWriteLockFile;
    // When set, overrides kCompactionThreshold for this store instance only.
    mutable std::optional<size_t> m_CompactionThresholdOverride;
    // When set, overrides kCompactionMaxAge for this store instance only.
    mutable std::optional<std::chrono::steady_clock::duration> m_CompactionMaxAgeOverride;
};

} // namespace GameEngine::AssetDatabase


