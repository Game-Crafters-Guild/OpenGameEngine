#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::AssetDatabase
{

// One on-disk file's fingerprint as captured at the time the snapshot was
// written. Fields mirror what IAssetDbCache::FileFingerprint stores
// — but the snapshot itself lives outside the SQLite cache so it can be
// loaded faster on warm-start without touching the cache db.
//
// CONSUMER CONTRACT: snapshot records are ADVISORY. A crash mid-Shutdown
// leaves .assetdb newer than the snapshot, so any file modified in the
// gap looks "unchanged" here but might actually differ on disk. Consumers
// (delta-scan in ReconcileSourceStore, etc.) MUST stat the file and
// confirm (Mtime, Size) match before reusing the cached Hash. If any of
// those fields differ, the snapshot record for that path is stale and
// must be discarded, not patched.
struct AssetSourceSnapshotRecord
{
    // Canonical relative path under the source root, with forward slashes.
    // Empty paths are invalid and ignored at write time.
    std::string CanonicalPath;
    int64_t Mtime = 0;       // last_write_time stored as
                              // file_time_type::time_since_epoch().count() —
                              // i.e. 100-nanosecond ticks on Windows
                              // (FILETIME-equivalent). Producer + consumer
                              // both use the same encoding so it round-trips
                              // exactly via FileTimeToInt64 / Int64ToFileTime.
    int64_t Size = 0;        // file size in bytes.
    std::string FileId;      // OS file identity (NTFS file ID / inode); may be empty.
    std::string Hash;        // Cached content hash (FNV1a64 hex string typically).
                              // Lets the consumer reuse the hash instead of
                              // recomputing when the stat-confirm above passes.
                              // May be empty for records written before the
                              // hash was available.
    // v4: identity payload reserved for a future "snapshot replay
    // bypasses JSONL store enumeration" optimization. As of Phase B/C,
    // PopulateHotCachesFromSource still walks the JSONL store
    // (missing/kv records aren't in the snapshot), so these fields are
    // currently written-but-unused. Kept in the format because: (a)
    // skipping the store walk is the next logical optimization once
    // the JSONL parse becomes the dominant init cost, (b) bumping the
    // format version just to drop them would force another cold-start
    // rebuild for users without a corresponding perf win.
    std::string Guid;        // GUID::ToString() form; empty means unknown.
    std::string TypeId;      // Programmable type id string (matches
                              // AssetRecord::typeId); empty means unknown.
};

// v5: per-directory mtime, captured at the previous Shutdown. Used by the
// warm-path scan + reconcile to skip subtrees whose mtime hasn't changed
// since the snapshot was written — Windows updates a directory's mtime
// when files are added/removed/renamed within it, so this gates the
// per-file walk + per-record exists-check on actual structural change.
struct AssetSourceSnapshotDirectory
{
    std::string CanonicalPath;  // canonical relative dir path, forward slashes.
                                 // Empty means the mount root itself.
    int64_t Mtime = 0;           // dir last_write_time as a file_time_type tick count.
};

// Persistable snapshot of an asset source's on-disk state. Used by the
// registry's startup reconcile path to drive a delta scan instead of a full
// stat-every-file walk: at shutdown the registry writes the snapshot, and on
// next mount the snapshot is replayed. Records present in the snapshot whose
// (path, mtime, size, fileId) match disk are skipped; the rest get re-fingerprinted.
//
// The snapshot is treated as a CACHE, not a source of truth. Strict version
// equality is enforced — a version mismatch means "delete and rebuild on next
// shutdown", not "best-effort load". This keeps the format-evolution contract
// simple: bumping the version is a deliberate breaking change and the file
// regenerates itself naturally.
//
// Binary on-disk format (little-endian):
//
//   u32 magic                  = 0x47455353  ("GESS" — GameEngine Source Snapshot)
//   u32 version                = 1
//   u64 mountPathHash          = 64-bit hash of the mount root path (sanity check
//                                so a snapshot file isn't accidentally loaded for
//                                a different mount). Hash function is std::hash
//                                over canonical UTF-8.
//   u64 ignoreRulesSignature   = 64-bit hash of the AssetIgnoreRules in effect
//                                when the snapshot was written. Reader compares
//                                against the current ignore-rule signature; on
//                                mismatch, the snapshot is treated as invalid
//                                so a re-scan picks up whatever the new rules
//                                show or hide. Zero means "no signature
//                                recorded" — old caller didn't compute one.
//   u64 recordCount            = number of records that follow.
//   u16 mountPathLen ; u8[mountPathLen] mountPath  (UTF-8, no terminator)
//   { record }*recordCount
//
//   record:
//     u16 canonicalPathLen ; u8[canonicalPathLen] canonicalPath
//     i64 mtime
//     i64 size
//     u16 fileIdLen ; u8[fileIdLen] fileId
//     u16 hashLen ; u8[hashLen] hash
//     u16 guidLen ; u8[guidLen] guid       (GUID::ToString form, may be empty)
//     u16 typeIdLen ; u8[typeIdLen] typeId (programmable type id, may be empty)
//
//   trailer (after the recordCount records):
//     u64 directoryCount
//     { directory }*directoryCount
//   directory:
//     u16 canonicalPathLen ; u8[canonicalPathLen] canonicalPath
//     i64 mtime
//
// Zero-length strings are valid (just len=0). Reader stops when recordCount
// records have been consumed; trailing bytes are ignored.
class AssetSourceSnapshot
{
public:
    static constexpr uint32_t kMagic = 0x47455353u;  // "GESS"
    static constexpr uint32_t kVersion = 1;

    // Save the snapshot to `snapshotFile`. The file is written atomically via
    // a temp+rename so a crash mid-write doesn't corrupt the existing file.
    // `ignoreRulesSignature` is a u64 hash of the active ignore rules; pass 0
    // to skip the staleness gate (legacy behavior; the loader will accept
    // any signature). `directories` is the per-subdir mtime payload added
    // in v5; pass an empty vector to write a v5 file with no directory
    // entries (loader handles that case as "no gating possible"). Returns
    // true on success; on failure, outError (if non-null) gets a
    // human-readable explanation and the existing file is left untouched.
    static bool Save(const std::filesystem::path& snapshotFile,
                     const std::filesystem::path& mountRoot,
                     uint64_t ignoreRulesSignature,
                     const std::vector<AssetSourceSnapshotRecord>& records,
                     const std::vector<AssetSourceSnapshotDirectory>& directories,
                     std::string* outError = nullptr);

    // Load a snapshot from `snapshotFile`. Returns false (and populates
    // outError) if the file is missing, has a wrong magic, has an
    // unsupported version, or is truncated. The mountRoot stored in the
    // header is returned via outMountRoot so callers can verify it matches
    // their expected mount root before trusting the records. The
    // `outIgnoreRulesSignature` is the u64 from the header — callers compare
    // it against the current ignore-rules signature and treat a mismatch
    // as a snapshot-invalidation event (re-scan from scratch).
    // `parsePool` parses a large snapshot's records in parallel (null parses
    // on the calling thread).
    static bool Load(const std::filesystem::path& snapshotFile,
                     JobSystem::WorkStealingThreadPool* parsePool,
                     std::filesystem::path& outMountRoot,
                     uint64_t& outIgnoreRulesSignature,
                     std::vector<AssetSourceSnapshotRecord>& outRecords,
                     std::vector<AssetSourceSnapshotDirectory>& outDirectories,
                     std::string* outError = nullptr);
};

} // namespace GameEngine::AssetDatabase
