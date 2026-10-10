#pragma once

#include "AssetCore/DepEdge.h"
#include "AssetDatabase/AssetRecord.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine::AssetDatabase
{

// Abstract interface for the derived asset-database cache (dependency graph,
// file fingerprints, KV mirror, provenance). This cache is ALWAYS safe to
// delete; it must never be required to preserve GUIDs or user metadata.
//
// The concrete implementation (AssetDbCache_Sqlite) links a SQL engine and
// lives in a separate library that only editor/tooling targets link. The
// engine + standalone Player depend on this interface only — they run fully
// cache-less (and SQL-free) when no implementation is injected. The cache is
// created through AssetRegistry's injected factory; a runtime with no factory
// (the Player) simply has a null cache, and every call site already degrades
// gracefully to "continuing without cache".
class IAssetDbCache
{
  public:
    struct FileFingerprint
    {
        int64_t mtime = 0;
        int64_t size = 0;
        std::string contentHash; // typically a fast/partial hash; may be empty
        std::string fileId;      // OS file identity (dev+inode / volume+fileindex); may be empty
        std::string path;        // canonical-relative row path; empty for fingerprint-only rows
        bool valid = false;
    };

    struct ProvenanceRow
    {
        GUID Produced;
        GUID Producer;
        std::string ImporterId;
    };

    // An advisory "these two are probably the same asset" pairing that the
    // reconcile pass found on evidence too weak to act on by itself. The
    // absentee stays a tombstone; nothing is journaled. A confirmation step
    // (see AssetStoreReconciler.h) is what turns one of these into a redirect.
    struct RenameSuggestion
    {
        /// The tombstoned record the suggestion is about; the row's key.
        GUID MissingGuid;
        /// Canonical-relative path the missing record still carries.
        std::string MissingPath;
        /// The candidate's registered GUID. Null under the Stored scheme,
        /// where new-file candidates come from a disk walk and have none yet.
        GUID CandidateGuid;
        /// Canonical-relative path of the candidate file.
        std::string CandidatePath;
        /// Which matcher produced it, e.g. kRenameEvidenceTierDirExt.
        std::string EvidenceTier;
    };

    // One ghost (missing-flagged asset row) as the retention pass sees it.
    struct GhostRow
    {
        GUID Guid;
        /// Session counter value stamped when the record went missing. 0 means
        /// the ghost predates retention stamping (it was marked before this
        /// cache had a missing_since column, or by a caller that passed 0) —
        /// its age is unknown, never "infinitely old".
        int64_t MissingSinceSession = 0;
        /// Whether the row carries any fingerprint column. Mirrors
        /// TryGetFileFingerprint's validity rule exactly, because that is the
        /// guard the rename matcher indexes absentees behind: a ghost with no
        /// fingerprint enters no tier — not even the advisory dir+ext one —
        /// and therefore can never be rename-healed.
        bool HasFingerprint = false;
    };

    // One asset row + optional fingerprint for UpsertAssetBatch. Batching
    // exists for the cold-import fan-in: per-call statement prepare plus
    // one implicit transaction per row costs ~17µs each (F.2 measurement);
    // a batch commits the whole shard in one transaction.
    struct AssetUpsertBatchEntry
    {
        AssetRecord Record;
        bool HasFingerprint = false;
        int64_t Mtime = 0;
        int64_t Size = 0;
        std::string ContentHash;
        std::string FileId;
    };

    virtual ~IAssetDbCache() = default;

    // Lifecycle.
    virtual bool Open(const std::filesystem::path& cachePath, std::string* outError = nullptr) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;
    virtual bool EnsureSchema(std::string* outError = nullptr) = 0;

    // True when this connection BUILT the schema rather than adopting one: a
    // cache file that did not exist, or one whose schema version forced a
    // rebuild. Either way every derived index starts empty, so a reader must
    // treat "no rows" as "nothing has been measured yet" rather than as an
    // observation about the project. Anything destructive gated on a derived
    // index MUST consult this first â dependency edges in particular are built
    // lazily on first query and are NOT repopulated by a scan, so an empty
    // dependents count is silence, not evidence of no references.
    virtual bool WasSchemaReset() const = 0;

    // Asset index. UpsertAsset writes the WHOLE row including `missing`, so it
    // is for callers that just looked at the file — a registration clearing a
    // ghost flag is exactly this call.
    virtual bool UpsertAsset(const AssetRecord& record, std::string* outError = nullptr) = 0;
    virtual bool UpsertAssetBatch(const std::vector<AssetUpsertBatchEntry>& entries, std::string* outError = nullptr) = 0;

    // Mirror a journal record's identity + type into the cache WITHOUT
    // claiming anything about disk. The journal holds identity and
    // user-authored metadata only; per-machine observations — missing,
    // missing_since, fingerprints — are the cache's own, so this call leaves
    // every one of them exactly as the row holds it (a new row starts
    // unflagged, which is the same "nothing observed yet" a fresh cache says).
    //
    // The distinction is load-bearing: a derived mount's tombstone rests ONLY
    // in this cache, and its journal record reads missing=false by design, so
    // a mirror that wrote `missing` would erase the ghost's one residence and
    // leave it flagged nowhere.
    virtual bool MirrorJournalRecord(const AssetRecord& record, std::string* outError = nullptr) = 0;
    virtual bool RemoveAsset(const GUID& guid, std::string* outError = nullptr) = 0;
    virtual bool TryGetAsset(const GUID& guid, AssetRecord& outRecord) const = 0;
    virtual std::optional<GUID> LookupGuidByPath(const std::string& canonicalPath) const = 0;
    // All GUIDs whose asset row carries missing=true. For derived-identity
    // mounts this cache is the missing flag's resting place (the git-shared
    // journal never carries per-machine disk observations); the reconcile
    // pass seeds its was-missing view from this one query instead of a
    // per-record point lookup.
    virtual std::vector<GUID> EnumerateMissingAssets() const = 0;

    // Set or clear the ghost flag on `record`'s row, creating the row from
    // `record` when the cache has never seen the GUID. Path and type follow
    // `record`; fingerprint columns are never touched.
    //
    // `missingSinceSession` is the age stamp GC measures against — write the
    // current GetSessionCounter() when marking, 0 when clearing. It is the
    // caller's job to stamp ONCE, at the false->true transition: re-stamping
    // a standing ghost every session would reset its age forever.
    virtual bool SetAssetMissing(const AssetRecord& record,
                                 bool missing,
                                 int64_t missingSinceSession,
                                 std::string* outError = nullptr) = 0;

    // The ghost set with the two facts a retention policy needs. Separate from
    // EnumerateMissingAssets because that one is on every reconcile pass's
    // critical path and is served by a covering partial index; this one reads
    // extra columns and runs only when a retention policy is armed.
    virtual std::vector<GhostRow> EnumerateGhostRows() const = 0;

    // Per-machine monotonic session counter, incremented once per cache open.
    // Ghost age is (current - MissingSinceSession) sessions. Session count,
    // not wall clock: it measures "how many times you opened this project
    // without the file coming back", which is what retention is really about
    // and what survives a machine sitting idle for a month.
    virtual int64_t GetSessionCounter() const = 0;

    // Fingerprints (derived).
    virtual bool UpdateFileFingerprint(const GUID& guid,
                                       int64_t mtime,
                                       int64_t size,
                                       const std::string& contentHash,
                                       const std::string& fileId,
                                       std::string* outError = nullptr) = 0;
    virtual bool TryGetFileFingerprint(const GUID& guid, FileFingerprint& outFingerprint) const = 0;
    virtual std::unordered_map<GUID, FileFingerprint> EnumerateAllFingerprints() const = 0;

    // Monotonic mutation counter (snapshot-skip optimization).
    virtual uint64_t GetMutationCount() const = 0;

    // KV mirror (derived convenience; authoritative kv is in the IAssetStore).
    virtual bool SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError = nullptr) = 0;
    virtual bool TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const = 0;
    virtual bool RemoveKeyValue(const GUID& guid, const std::string& key, std::string* outError = nullptr) = 0;

    // Rename suggestions (derived, advisory).
    //
    // Cache residence is the correct home, not a compromise: a suggestion is a
    // per-machine observation about local disk state, re-derived from scratch
    // by the next reconcile pass whenever the same weak-evidence pairing
    // recurs. Losing the table costs nothing but the re-derivation, which is
    // exactly the "always safe to delete" contract this interface opens with.
    // Journaling them would push machine-local guesses into git-shared
    // identity, which is what demoting the tier was meant to prevent.
    //
    // Keyed on MissingGuid: re-recording the same absentee overwrites, so a
    // re-derivation is idempotent rather than an accumulating pile of rows.
    virtual bool RecordRenameSuggestion(const RenameSuggestion& suggestion, std::string* outError = nullptr) = 0;
    virtual bool RemoveRenameSuggestion(const GUID& missingGuid, std::string* outError = nullptr) = 0;
    virtual std::vector<RenameSuggestion> EnumerateRenameSuggestions() const = 0;

    // Dependencies / reference index (derived).
    virtual bool ReplaceDependencies(const GUID& guid, const std::vector<GUID>& deps, std::string* outError = nullptr) = 0;
    virtual bool ReplaceDependencies(const GUID& guid, const std::vector<DepEdge>& edges, std::string* outError = nullptr) = 0;
    virtual std::vector<GUID> GetDependencies(const GUID& guid) const = 0;
    virtual void IterateDependents(const GUID& depGuid,
                                   const std::function<bool(const GUID&)>& visit) const = 0;
    virtual void IterateDependentsByPath(const std::string& canonicalPath,
                                         const std::function<bool(const GUID&)>& visit) const = 0;
    virtual size_t CountDependents(const GUID& depGuid) const = 0;
    virtual size_t CountDependentsByPath(const std::string& canonicalPath) const = 0;
    virtual std::vector<DepEdge> GetDependencyEdges(const GUID& guid) const = 0;

    // Provenance edges (importer-time relationships).
    virtual bool RegisterProvenance(const GUID& produced,
                                    const GUID& producer,
                                    std::string_view importerId,
                                    std::string* outError = nullptr) = 0;
    virtual bool UnregisterProvenance(const GUID& produced, std::string* outError = nullptr) = 0;
    virtual GUID GetProducer(const GUID& produced) const = 0;
    virtual std::vector<GUID> EnumerateProducedAssets(const GUID& producer) const = 0;
    virtual std::vector<ProvenanceRow> EnumerateAllProvenance() const = 0;
    virtual size_t UnregisterAllProducedBy(const GUID& producer, std::string* outError = nullptr) = 0;
    virtual size_t RedirectProvenance(const GUID& from, const GUID& to, std::string* outError = nullptr) = 0;
};

// Factory for the derived cache. Editor/tooling builds register one that
// returns the SQLite implementation; the engine library ships no factory, so
// the standalone Player runs cache-less and links no SQL engine.
using AssetDbCacheFactory = std::function<std::unique_ptr<IAssetDbCache>()>;

} // namespace GameEngine::AssetDatabase
