#include "AssetDatabase/AssetSourceSnapshot.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "JobSystem/ParallelAlgorithms.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <system_error>

namespace GameEngine::AssetDatabase
{

namespace
{

// Phase 5.2: multi-process advisory lock on the snapshot file. Sidecar
// `<snapshotFile>.lock` carries a unix-time-seconds value. A second writer
// attempting to Save() while the lock is fresh (< kLockStaleSeconds) backs
// off; on stale locks it takes over (recovers from a crashed prior writer).
//
// "Advisory" here means: a writer that bypasses the lock (or a Load() call)
// can still corrupt or read torn data. The lock's only job is to prevent
// two well-behaved Save() callers from colliding. Reads of the snapshot
// don't take the lock at all — they're idempotent and the temp+rename in
// the writer makes them see either the old or new file, never half-written.
constexpr int64_t kLockStaleSeconds = 60;
constexpr const char* kLockSuffix = ".lock";

int64_t SnapshotNowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ReadLockTimestamp(const std::filesystem::path& lockFile, int64_t& outTimestamp)
{
    String text;
    if (!ReadFileTextShared(lockFile, text))
        return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    while (begin != end && (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n'))
        ++begin;
    return std::from_chars(begin, end, outTimestamp).ec == std::errc{};
}

bool WriteLockFile(const std::filesystem::path& lockFile)
{
    std::ofstream os(lockFile, std::ios::trunc);
    if (!os)
        return false;
    os << SnapshotNowSeconds() << "\n";
    return os.good();
}

void ReleaseLockFile(const std::filesystem::path& lockFile)
{
    std::error_code ec;
    std::filesystem::remove(lockFile, ec);
}

// Releases the lock file when the guard goes out of scope unless dismissed.
// Used so any failure path in Save() (open / write / rename) drops the lock.
struct LockReleaseGuard
{
    std::filesystem::path lockFile;
    bool active = true;
    ~LockReleaseGuard()
    {
        if (active)
            ReleaseLockFile(lockFile);
    }
    void Dismiss() { active = false; }
};

template <typename T>
void WriteScalar(std::ostream& os, T value)
{
    static_assert(std::is_trivially_copyable_v<T>, "WriteScalar requires trivially-copyable");
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadScalar(std::istream& is, T& outValue)
{
    static_assert(std::is_trivially_copyable_v<T>, "ReadScalar requires trivially-copyable");
    is.read(reinterpret_cast<char*>(&outValue), sizeof(T));
    return static_cast<size_t>(is.gcount()) == sizeof(T);
}

void WriteString(std::ostream& os, std::string_view s)
{
    // u16 length — paths longer than 65535 bytes are vanishingly rare and
    // the rest of the system already assumes path lengths fit in MAX_PATH-ish.
    const auto len = static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFFu));
    WriteScalar(os, len);
    if (len > 0)
        os.write(s.data(), len);
}

bool ReadString(std::istream& is, std::string& out)
{
    uint16_t len = 0;
    if (!ReadScalar(is, len))
        return false;
    out.assign(len, '\0');
    if (len > 0)
    {
        is.read(out.data(), len);
        if (static_cast<size_t>(is.gcount()) != len)
            return false;
    }
    return true;
}

// F.9: byte-buffer-based readers used by the parallel Load path.
// Caller maintains a cursor into a contiguous std::string buffer holding
// the post-header records section. Returning false signals EOF/truncated.
template <typename T>
bool ReadScalarFromBuffer(const char* data, size_t size, size_t& cursor, T& outValue)
{
    static_assert(std::is_trivially_copyable_v<T>, "ReadScalarFromBuffer requires trivially-copyable");
    if (cursor + sizeof(T) > size)
        return false;
    std::memcpy(&outValue, data + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

bool ReadStringFromBuffer(const char* data, size_t size, size_t& cursor, std::string& out)
{
    uint16_t len = 0;
    if (!ReadScalarFromBuffer(data, size, cursor, len))
        return false;
    if (cursor + len > size)
        return false;
    out.assign(data + cursor, len);
    cursor += len;
    return true;
}

// Skip a length-prefixed string. Used in the pre-scan pass that
// computes per-record byte offsets without materializing strings.
bool SkipStringInBuffer(const char* data, size_t size, size_t& cursor)
{
    uint16_t len = 0;
    if (!ReadScalarFromBuffer(data, size, cursor, len))
        return false;
    if (cursor + len > size)
        return false;
    cursor += len;
    return true;
}

uint64_t HashMountRoot(const std::filesystem::path& mountRoot)
{
    // Use generic_u8string so the hash is identical regardless of host
    // path-separator conventions. This is just a sanity check — collisions
    // would only matter if two different mount roots happened to hash
    // identically AND a snapshot got swapped between them, which is a
    // misuse pattern the caller already guards against.
    const std::u8string u8 = mountRoot.lexically_normal().generic_u8string();
    std::string_view sv(reinterpret_cast<const char*>(u8.data()), u8.size());
    return std::hash<std::string_view>{}(sv);
}

} // namespace

bool AssetSourceSnapshot::Save(const std::filesystem::path& snapshotFile,
                               const std::filesystem::path& mountRoot,
                               uint64_t ignoreRulesSignature,
                               const std::vector<AssetSourceSnapshotRecord>& records,
                               const std::vector<AssetSourceSnapshotDirectory>& directories,
                               std::string* outError)
{
    if (snapshotFile.empty())
    {
        if (outError)
            *outError = "Snapshot file path is empty";
        return false;
    }

    // Phase 5.2: acquire the advisory lock before any I/O. A fresh lock
    // means another process is mid-write; back off so the two writers don't
    // step on each other's temp files / rename. A stale lock is treated as
    // a crashed prior writer and we take over.
    const std::filesystem::path lockFile = snapshotFile.string() + kLockSuffix;
    {
        std::error_code lockEc;
        if (std::filesystem::exists(lockFile, lockEc))
        {
            int64_t holdSec = 0;
            if (ReadLockTimestamp(lockFile, holdSec))
            {
                const int64_t age = SnapshotNowSeconds() - holdSec;
                if (age >= 0 && age < kLockStaleSeconds)
                {
                    if (outError)
                        *outError = "Snapshot is locked by another process (lock age " +
                                    std::to_string(age) + "s)";
                    return false;
                }
                // Stale lock — fall through and override.
            }
            // Unparseable lock file — treat as stale.
        }
    }
    if (!WriteLockFile(lockFile))
    {
        if (outError)
            *outError = "Failed to acquire snapshot lock file: " + lockFile.string();
        return false;
    }
    LockReleaseGuard releaseLock{lockFile};

    // Atomic write via temp + rename: a crash mid-write must not corrupt the
    // existing snapshot. The temp file lives next to the target so the rename
    // is on the same volume.
    std::error_code ec;
    if (!snapshotFile.parent_path().empty())
    {
        std::filesystem::create_directories(snapshotFile.parent_path(), ec);
        ec.clear();
    }
    const std::filesystem::path tmp = snapshotFile.string() + ".tmp";

    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os)
        {
            if (outError)
                *outError = "Failed to open snapshot temp file for writing: " + tmp.string();
            return false;
        }

        WriteScalar<uint32_t>(os, kMagic);
        WriteScalar<uint32_t>(os, kVersion);
        WriteScalar<uint64_t>(os, HashMountRoot(mountRoot));
        WriteScalar<uint64_t>(os, ignoreRulesSignature);

        // Filter out invalid records (empty paths) before writing the count
        // so the count and the actual write match. Cheaper than a second pass.
        std::vector<const AssetSourceSnapshotRecord*> valid;
        valid.reserve(records.size());
        for (const auto& r : records)
        {
            if (!r.CanonicalPath.empty())
                valid.push_back(&r);
        }
        WriteScalar<uint64_t>(os, static_cast<uint64_t>(valid.size()));

        const std::u8string mountU8 = mountRoot.lexically_normal().generic_u8string();
        WriteString(os, std::string_view(reinterpret_cast<const char*>(mountU8.data()), mountU8.size()));

        for (const auto* r : valid)
        {
            WriteString(os, r->CanonicalPath);
            WriteScalar<int64_t>(os, r->Mtime);
            WriteScalar<int64_t>(os, r->Size);
            WriteString(os, r->FileId);
            WriteString(os, r->Hash);
            WriteString(os, r->Guid);
            WriteString(os, r->TypeId);
        }

        // v5 trailer: per-directory mtime entries. Filter out empty paths
        // for symmetry with the records section (empty dir paths are
        // ambiguous: caller might mean "mount root itself" but the loader
        // can't tell that from "missing"; producers should use a sentinel
        // like "." instead).
        std::vector<const AssetSourceSnapshotDirectory*> validDirs;
        validDirs.reserve(directories.size());
        for (const auto& d : directories)
        {
            if (!d.CanonicalPath.empty())
                validDirs.push_back(&d);
        }
        WriteScalar<uint64_t>(os, static_cast<uint64_t>(validDirs.size()));
        for (const auto* d : validDirs)
        {
            WriteString(os, d->CanonicalPath);
            WriteScalar<int64_t>(os, d->Mtime);
        }

        if (!os)
        {
            if (outError)
                *outError = "I/O error while writing snapshot temp file";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    if (!FileSystem::PublishFile(tmp, snapshotFile))
    {
        if (outError)
            *outError = "Failed to put the temp snapshot in place";
        return false;
    }
    return true;
}

bool AssetSourceSnapshot::Load(const std::filesystem::path& snapshotFile,
                               JobSystem::WorkStealingThreadPool* parsePool,
                               std::filesystem::path& outMountRoot,
                               uint64_t& outIgnoreRulesSignature,
                               std::vector<AssetSourceSnapshotRecord>& outRecords,
                               std::vector<AssetSourceSnapshotDirectory>& outDirectories,
                               std::string* outError)
{
    outRecords.clear();
    outDirectories.clear();
    outIgnoreRulesSignature = 0;

    // F.9: read whole file into a buffer up front. One I/O instead of
    // per-field stream reads, and the buffer enables the parallel
    // record-parse path below for large snapshots.
    String buffer;
    if (!ReadFileTextShared(snapshotFile, buffer))
    {
        if (outError)
            *outError = "Snapshot file not found or unreadable: " + snapshotFile.string();
        return false;
    }

    const char* data = buffer.data();
    const size_t size = buffer.size();
    size_t cursor = 0;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t mountPathHash = 0;
    uint64_t recordCount = 0;

    if (!ReadScalarFromBuffer(data, size, cursor, magic) || magic != kMagic)
    {
        if (outError)
            *outError = "Snapshot file has wrong magic (corrupt or not a GESS snapshot)";
        return false;
    }
    if (!ReadScalarFromBuffer(data, size, cursor, version) || version != kVersion)
    {
        if (outError)
            *outError = "Snapshot file has unsupported version " + std::to_string(version);
        return false;
    }
    if (!ReadScalarFromBuffer(data, size, cursor, mountPathHash) ||
        !ReadScalarFromBuffer(data, size, cursor, outIgnoreRulesSignature) ||
        !ReadScalarFromBuffer(data, size, cursor, recordCount))
    {
        if (outError)
            *outError = "Snapshot header truncated";
        return false;
    }

    std::string mountStr;
    if (!ReadStringFromBuffer(data, size, cursor, mountStr))
    {
        if (outError)
            *outError = "Snapshot mount path truncated";
        return false;
    }
    outMountRoot = std::filesystem::path(reinterpret_cast<const char8_t*>(mountStr.data()),
                                          reinterpret_cast<const char8_t*>(mountStr.data()) + mountStr.size());

    if (HashMountRoot(outMountRoot) != mountPathHash)
    {
        if (outError)
            *outError = "Snapshot mount-path hash mismatch (snapshot belongs to a different mount root)";
        return false;
    }

    // F.9: pre-scan to compute each record's start offset. Records have
    // variable-length string fields, so we can't index directly — but
    // a single linear pass through the records section is cheap (just
    // reads the u16 length prefixes and skips). Storing offsets lets
    // parallel workers parse non-overlapping ranges.
    std::vector<size_t> recordOffsets;
    recordOffsets.reserve(static_cast<size_t>(recordCount));
    for (uint64_t i = 0; i < recordCount; ++i)
    {
        recordOffsets.push_back(cursor);
        // path, fileId, hash, guid, typeId are length-prefixed strings.
        // Mtime + Size are int64 scalars between path and fileId.
        if (!SkipStringInBuffer(data, size, cursor))            // CanonicalPath
        {
            if (outError) *outError = "Snapshot record " + std::to_string(i) + " truncated (path)";
            outRecords.clear(); return false;
        }
        if (cursor + sizeof(int64_t) * 2 > size)
        {
            if (outError) *outError = "Snapshot record " + std::to_string(i) + " truncated (mtime/size)";
            outRecords.clear(); return false;
        }
        cursor += sizeof(int64_t) * 2;                          // Mtime + Size
        if (!SkipStringInBuffer(data, size, cursor) ||          // FileId
            !SkipStringInBuffer(data, size, cursor) ||          // Hash
            !SkipStringInBuffer(data, size, cursor) ||          // Guid
            !SkipStringInBuffer(data, size, cursor))            // TypeId
        {
            if (outError) *outError = "Snapshot record " + std::to_string(i) + " truncated";
            outRecords.clear(); return false;
        }
    }

    outRecords.resize(static_cast<size_t>(recordCount));

    // Lambda: parse one record at the given offset. Pure function except
    // for writing to outRec — safe to call concurrently when each worker
    // writes to a non-overlapping outRecords slot.
    auto parseRecordAt = [data, size](size_t recOffset, AssetSourceSnapshotRecord& outRec) -> bool {
        size_t c = recOffset;
        return ReadStringFromBuffer(data, size, c, outRec.CanonicalPath) &&
               ReadScalarFromBuffer(data, size, c, outRec.Mtime) &&
               ReadScalarFromBuffer(data, size, c, outRec.Size) &&
               ReadStringFromBuffer(data, size, c, outRec.FileId) &&
               ReadStringFromBuffer(data, size, c, outRec.Hash) &&
               ReadStringFromBuffer(data, size, c, outRec.Guid) &&
               ReadStringFromBuffer(data, size, c, outRec.TypeId);
    };

    // Large snapshots parse their records on the parse pool; each record
    // writes only its own slot.
    constexpr size_t kParallelRecordThreshold = 4000;
    if (!parsePool || recordCount < kParallelRecordThreshold)
    {
        for (size_t i = 0; i < recordOffsets.size(); ++i)
        {
            if (!parseRecordAt(recordOffsets[i], outRecords[i]))
            {
                if (outError) *outError = "Snapshot record " + std::to_string(i) + " parse failed";
                outRecords.clear();
                return false;
            }
        }
    }
    else
    {
        std::atomic<bool> ok{true};
        JobSystem::ParallelFor(parsePool, recordOffsets.size(),
            [&recordOffsets, &outRecords, &parseRecordAt, &ok](size_t chunkBegin, size_t chunkEnd) {
                for (size_t i = chunkBegin; i < chunkEnd; ++i)
                {
                    if (!parseRecordAt(recordOffsets[i], outRecords[i]))
                    {
                        ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            },
            kParallelRecordThreshold / 4);
        if (!ok.load(std::memory_order_relaxed))
        {
            if (outError) *outError = "Snapshot record parse failed (parallel path)";
            outRecords.clear();
            return false;
        }
    }

    // v5 trailer: per-directory mtimes. Small (~16-50 entries even at
    // 50K records); stay sequential.
    uint64_t directoryCount = 0;
    if (!ReadScalarFromBuffer(data, size, cursor, directoryCount))
    {
        if (outError)
            *outError = "Snapshot directory count truncated";
        outRecords.clear();
        return false;
    }

    outDirectories.reserve(static_cast<size_t>(directoryCount));
    for (uint64_t i = 0; i < directoryCount; ++i)
    {
        AssetSourceSnapshotDirectory d;
        if (!ReadStringFromBuffer(data, size, cursor, d.CanonicalPath) ||
            !ReadScalarFromBuffer(data, size, cursor, d.Mtime))
        {
            if (outError)
                *outError = "Snapshot directory entry " + std::to_string(i) + " truncated";
            outRecords.clear();
            outDirectories.clear();
            return false;
        }
        outDirectories.push_back(std::move(d));
    }

    return true;
}

} // namespace GameEngine::AssetDatabase
