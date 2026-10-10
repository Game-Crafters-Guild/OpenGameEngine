#pragma once

#include "AssetDatabase/AssetRecord.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::AssetDatabase
{

// What a save should do when another PROCESS holds this store's write lock.
// Only a store that has one (AssetStore_TextJsonl::SetCrossProcessWriteLockFile)
// can be contended; for every other store both answers are the same.
enum class StoreSaveWait : uint8
{
    // Fail with an error rather than park the calling thread. For the flush that
    // runs on a job worker: the caller keeps the deltas and retries next tick.
    NonBlocking,
    // Wait the peer out. For a caller that has no next tick — shutdown, unmount,
    // rebind — where a refused save is an edit thrown away. Bounded by the
    // peer's own save, and a dead peer's lock is released by the OS.
    WaitForPeers,
};

// Authoritative persistence for asset identity + user-authored metadata.
// This is designed to be git-friendly (text format, deterministic output).
class IAssetStore
{
  public:
    virtual ~IAssetStore() = default;

    virtual bool LoadFromFile(const std::filesystem::path& filePath, std::string* outError = nullptr) = 0;
    virtual bool SaveToFile(const std::filesystem::path& filePath, std::string* outError = nullptr,
                            StoreSaveWait wait = StoreSaveWait::NonBlocking) const = 0;

    // Upsert/remove records. UpsertAsset writes the record whole — use it only
    // where the caller owns every field (replay, reconciliation, tools).
    virtual bool UpsertAsset(const AssetRecord& record, std::string* outError = nullptr) = 0;
    virtual bool RemoveAsset(const GUID& guid, std::string* outError = nullptr) = 0;
    virtual bool MarkMissing(const GUID& guid, bool missing, std::string* outError = nullptr) = 0;

    // Fold one registrar's observation onto the stored row under this store's
    // lock, clear the missing tombstone, and report the row as it now stands.
    // Registration paths use this rather than read-merge-upsert: concurrent
    // registrars of one asset (a directory scan and a caller registering the
    // same file) otherwise resolve last-writer-wins on stale snapshots, and the
    // one that could not classify the asset wins as often as the one that
    // could.
    virtual StoreMergeResult MergeObservation(const AssetObservation& observation,
                                              AssetRecord& outMerged,
                                              std::string* outError = nullptr) = 0;

    // Queries.
    virtual bool TryGetAsset(const GUID& guid, AssetRecord& outRecord) const = 0;
    // Path identity is case-insensitive (AssetPaths::FoldStorePathKey), so a
    // query resolves the one row for that file whatever case it is spelled in
    // — matching the identity policy GUID::Derive callsites already apply.
    virtual std::optional<GUID> LookupGuidByPath(const std::string& canonicalPath) const = 0;
    virtual std::vector<AssetRecord> EnumerateAssets() const = 0;
    // Visit every record in place. Preferred over EnumerateAssets for a
    // caller that walks the records once and discards them: no record is
    // copied, so a 50K-record store costs a traversal instead of a deep
    // copy of every path string and kv map.
    //
    // Reentrancy: `visit` runs under the lock guarding the store's records
    // (a non-recursive mutex in AssetStore_TextJsonl), so it must not call
    // back into the store — mutations self-deadlock, and reads do too.
    // Capture what is needed and act after IterateAssets returns. Keep the
    // visitor cheap for the same reason: it extends the lock hold.
    //
    // Callers that mutate the store while walking it, or that need the
    // records to outlive the call, keep EnumerateAssets.
    virtual void IterateAssets(const std::function<void(const AssetRecord&)>& visit) const = 0;
    // Record count without materializing the full record vector (E6c —
    // EnumerateAssets copies every record just to be .size()'d otherwise).
    virtual size_t CountAssets() const = 0;

    // Key/value metadata helpers.
    virtual bool SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError = nullptr) = 0;
    virtual bool TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const = 0;

    // Redirects (optional, Unreal-style).
    virtual bool AddRedirect(const GUID& from, const GUID& to, std::string* outError = nullptr) = 0;
    virtual bool RemoveRedirect(const GUID& from, std::string* outError = nullptr) = 0;
    virtual std::optional<GUID> ResolveRedirect(const GUID& from) const = 0;
    virtual std::vector<RedirectRecord> EnumerateRedirects() const = 0;
    // Every GUID whose redirect chain leads to `target`, directly or through other redirects;
    // `target` itself is never included, even when it sits in a redirect cycle. Proportional to
    // the result, not to the redirect table, so it is cheap enough for every asset event.
    virtual std::vector<GUID> FindRedirectSourcesTo(const GUID& target) const = 0;

    // Conflicts encountered when this store last loaded from disk (e.g.
    // duplicate GUIDs across rows, malformed records). Default-empty for
    // stores that don't track parse-time conflicts; the JSONL backend
    // overrides to return the conflicts it accumulated during LoadFromFile.
    virtual std::vector<std::string> GetLoadConflicts() const { return {}; }

    // Drains the records this store evicted because another GUID claimed their
    // path key, in any spelling: one file that ended up with two rows. The
    // mount decides which are owed a redirect (AssetRegistry::
    // DrainDisplacedIdentities) and consumes those (MergeDisplacedStoreRecords)
    // to redirect each retired GUID onto the row that owns the path now, so
    // references authored against it keep resolving. Default-empty for stores
    // that do not arbitrate path keys.
    virtual std::vector<AssetRecord> TakeDisplacedRecords() { return {}; }
};

} // namespace GameEngine::AssetDatabase


