#pragma once

#include "AssetCore/DepEdge.h"
#include "AssetDatabase/AssetRecord.h"
#include "AssetDatabase/IAssetDbCache.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace GameEngine::AssetDatabase
{

// Derived cache stored in a local SQLite database.
// This cache is always safe to delete; it must never be required to preserve GUIDs or user metadata.
class AssetDbCache_Sqlite final : public IAssetDbCache
{
  public:
    using FileFingerprint = IAssetDbCache::FileFingerprint;
    using ProvenanceRow = IAssetDbCache::ProvenanceRow;
    using RenameSuggestion = IAssetDbCache::RenameSuggestion;

    AssetDbCache_Sqlite() = default;
    ~AssetDbCache_Sqlite() override;

    AssetDbCache_Sqlite(const AssetDbCache_Sqlite&) = delete;
    AssetDbCache_Sqlite& operator=(const AssetDbCache_Sqlite&) = delete;

    bool Open(const std::filesystem::path& sqlitePath, std::string* outError = nullptr) override;
    void Close() override;
    bool IsOpen() const override;

    bool EnsureSchema(std::string* outError = nullptr) override;
    bool WasSchemaReset() const override { return m_SchemaWasReset; }

    // Asset index
    bool UpsertAsset(const AssetRecord& record, std::string* outError = nullptr) override;
    // One transaction + cached prepared statements for the whole batch —
    // the cold-import fan-in path. ~17µs/row of prepare + implicit-txn
    // overhead drops to one COMMIT per shard.
    bool UpsertAssetBatch(const std::vector<AssetUpsertBatchEntry>& entries, std::string* outError = nullptr) override;
    bool MirrorJournalRecord(const AssetRecord& record, std::string* outError = nullptr) override;
    bool RemoveAsset(const GUID& guid, std::string* outError = nullptr) override;
    bool TryGetAsset(const GUID& guid, AssetRecord& outRecord) const override;
    std::optional<GUID> LookupGuidByPath(const std::string& canonicalPath) const override;
    // The ghost-set query EnumerateMissingAssets runs. Exported so the
    // plan-pin test EXPLAINs this exact text rather than a copy of it: the
    // partial index idx_assets_missing serves the query only while the two
    // WHERE clauses stay textually identical, and a divergence restores the
    // full table scan with no functional symptom to notice.
    static constexpr const char* kEnumerateMissingAssetsSql =
        "SELECT guid FROM assets WHERE missing<>0;";

    std::vector<GUID> EnumerateMissingAssets() const override;
    bool SetAssetMissing(const AssetRecord& record,
                         bool missing,
                         int64_t missingSinceSession,
                         std::string* outError = nullptr) override;
    std::vector<GhostRow> EnumerateGhostRows() const override;
    int64_t GetSessionCounter() const override { return m_SessionCounter; }

    // Fingerprints (derived): used for startup reconciliation and fast change detection.
    bool UpdateFileFingerprint(const GUID& guid,
                               int64_t mtime,
                               int64_t size,
                               const std::string& contentHash,
                               const std::string& fileId,
                               std::string* outError = nullptr) override;
    bool TryGetFileFingerprint(const GUID& guid, FileFingerprint& outFingerprint) const override;

    // Batch fingerprint accessor — returns ALL non-null fingerprints in one
    // SQLite query. Per-record TryGetFileFingerprint pays per-call prepare/
    // finalize overhead (~17μs each), so calling it in a 50K-record loop
    // costs ~850ms; this single call is ~50ms for the same data.
    // Used by AssetRegistry::Shutdown's snapshot writer.
    std::unordered_map<GUID, FileFingerprint> EnumerateAllFingerprints() const override;

    // F.5: monotonic counter incremented by UpsertAsset / UpdateFileFingerprint /
    // RemoveAsset / KV / dep mutations. Snapshot writer reads this at Shutdown:
    // if the value is unchanged from when the snapshot was loaded, the
    // on-disk snapshot is still current and the write can be skipped
    // entirely (saves ~400ms at 50K). Atomic so concurrent mutations from
    // background tasks are observed correctly.
    uint64_t GetMutationCount() const override { return m_MutationCount.load(std::memory_order_relaxed); }

    // KV mirror (derived convenience; authoritative kv is in the IAssetStore).
    bool SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError = nullptr) override;
    bool TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const override;
    bool RemoveKeyValue(const GUID& guid, const std::string& key, std::string* outError = nullptr) override;

    // Rename suggestions (derived, advisory). INSERT OR REPLACE on
    // missing_guid, so a re-derived suggestion overwrites its predecessor.
    // RemoveAsset drops a GUID's suggestion alongside its other derived rows.
    bool RecordRenameSuggestion(const RenameSuggestion& suggestion, std::string* outError = nullptr) override;
    bool RemoveRenameSuggestion(const GUID& missingGuid, std::string* outError = nullptr) override;
    std::vector<RenameSuggestion> EnumerateRenameSuggestions() const override;

    // Dependencies / reference index (derived).
    //
    // Two overloads:
    //   - The GUID-only overload writes simple (referrer, target) edges with
    //     edge_kind = Other, empty field_locator, ordinal = 0. Kept for
    //     callers that don't yet have richer edge metadata (most existing
    //     code paths). Internally stored alongside richer edges in the same
    //     deps table — no separate code path.
    //   - The DepEdge overload writes the full row including kind, locator,
    //     and ordinal. Phase 4 retarget UX queries (which need to know
    //     "which scene/material/field references this asset?") rely on the
    //     locator and kind being present, so importers should migrate to
    //     this overload as their ExtractDependencies implementations land.
    //
    // Both replace ALL existing dep rows for `guid`. Don't mix overloads
    // for the same referrer in successive calls — the second call drops
    // edges from the first.
    bool ReplaceDependencies(const GUID& guid, const std::vector<GUID>& deps, std::string* outError = nullptr) override;
    bool ReplaceDependencies(const GUID& guid, const std::vector<DepEdge>& edges, std::string* outError = nullptr) override;

    std::vector<GUID> GetDependencies(const GUID& guid) const override;

    // Stream every GUID-form referrer of depGuid through `visit`. Callback
    // returns true to continue, false to stop early. Memory is O(1) regardless
    // of fan-out — backed by a sqlite3_stmt cursor. Use cases:
    //   - Eligibility checks: stop on the first match (return false).
    //   - Full walks (FixUpRedirects, redirect rewrite): always return true.
    // Empty/null depGuid is a no-op.
    void IterateDependents(const GUID& depGuid,
                           const std::function<bool(const GUID&)>& visit) const override;

    // Path-form referrers: who points at this canonical path? Path-form dep
    // edges exist when a parser saw a path reference before the target was
    // registered. Same cursor + early-exit shape as IterateDependents.
    void IterateDependentsByPath(const std::string& canonicalPath,
                                  const std::function<bool(const GUID&)>& visit) const override;

    // Count-only variants. SELECT COUNT(*) — no row fetch, no vector alloc.
    // Use when only the cardinality matters (missing-asset reports, post-rewrite
    // remainder check).
    size_t CountDependents(const GUID& depGuid) const override;
    size_t CountDependentsByPath(const std::string& canonicalPath) const override;

    std::vector<DepEdge> GetDependencyEdges(const GUID& guid) const override;

    // Provenance edges (importer-time relationships). Tracks "asset
    // produced was written from asset producer during import" — distinct
    // from the deps table's runtime "asset A's content references B"
    // semantic.
    //
    // Cardinality: 1:1 produced -> producer (PK on produced_guid).
    // Idempotent: re-registering with a different producer overwrites
    // (INSERT OR REPLACE), so reimport works without an explicit clear.
    bool RegisterProvenance(const GUID& produced,
                            const GUID& producer,
                            std::string_view importerId,
                            std::string* outError = nullptr) override;

    bool UnregisterProvenance(const GUID& produced,
                              std::string* outError = nullptr) override;

    GUID GetProducer(const GUID& produced) const override;

    std::vector<GUID> EnumerateProducedAssets(const GUID& producer) const override;

    // Bulk read for warm-start: returns a flat row vector. Caller groups
    // by producer to build the producer -> produced map (the in-memory
    // cache on AssetRegistry does this once on first touch).
    std::vector<ProvenanceRow> EnumerateAllProvenance() const override;

    // Bulk delete: drop every row whose producer_guid matches. Used by
    // the orphan-cleanup scan path (after a producer is fully removed)
    // and tools that want to wipe provenance for a specific producer.
    // Returns number of rows changed.
    size_t UnregisterAllProducedBy(const GUID& producer,
                                   std::string* outError = nullptr) override;

    // Redirect-time bulk update: rewrite produced and/or producer
    // columns matching `from` to `to`. Both updates wrapped in a single
    // transaction. Returns number of rows changed across both columns.
    size_t RedirectProvenance(const GUID& from,
                              const GUID& to,
                              std::string* outError = nullptr) override;

  private:
    bool Exec(const char* sql, std::string* outError) const;
    static std::string SqliteError(sqlite3* db);

    // Lazily prepare `sql` into `slot` (once per connection) and return it
    // reset + cleared for a fresh bind. Caller must hold m_Mutex. Returns
    // nullptr on prepare failure. Cached statements are finalized in Close().
    sqlite3_stmt* GetCachedStmt(sqlite3_stmt*& slot, const char* sql) const;
    void FinalizeCachedStmts();

    // Bind + step one asset upsert / fingerprint via the cached statements.
    // Caller must hold m_Mutex.
    bool StepEvictPathRowLocked(const AssetRecord& record, std::string* outError);
    bool StepUpsertAssetLocked(const AssetRecord& record, std::string* outError);
    bool StepUpdateFingerprintLocked(const GUID& guid, int64_t mtime, int64_t size,
                                     const std::string& contentHash, const std::string& fileId,
                                     std::string* outError);

    // True when `table` already declares `column`. Additive schema changes go
    // through this instead of a version bump: bumping drops every table, and
    // the fingerprint columns it would take with it are exactly what lets an
    // absentee still rename-heal. Caller must hold m_Mutex.
    bool HasColumnLocked(const char* table, const char* column) const;

    // Read, increment and persist the per-machine session counter. Runs once
    // per process on the first EnsureSchema; caller must hold m_Mutex.
    void AdvanceSessionCounterLocked();

    mutable std::mutex m_Mutex;
    sqlite3* m_Db = nullptr;
    std::atomic<uint64_t> m_MutationCount{0};
    int64_t m_SessionCounter = 0;
    bool m_SchemaWasReset = false;

    // Cached prepared statements for the hot import path (F.2 / S9-E2).
    // Guarded by m_Mutex; owned by the connection, finalized before close.
    mutable sqlite3_stmt* m_StmtUpsertAsset = nullptr;
    mutable sqlite3_stmt* m_StmtMirrorJournalRecord = nullptr;
    mutable sqlite3_stmt* m_StmtEvictPathRow = nullptr;
    mutable sqlite3_stmt* m_StmtUpdateFingerprint = nullptr;
    mutable sqlite3_stmt* m_StmtTryGetAsset = nullptr;
    mutable sqlite3_stmt* m_StmtLookupGuidByPath = nullptr;
    mutable sqlite3_stmt* m_StmtSetKeyValue = nullptr;
    mutable sqlite3_stmt* m_StmtTryGetFingerprint = nullptr;
};

} // namespace GameEngine::AssetDatabase


