#include "AssetDatabase/AssetStoreReconciler.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"

#include "AssetCore/PathNormalization.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "AssetDatabase/AssetRecord.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/IAssetStore.h"
#include "AssetDatabase/RedirectChain.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace GameEngine::AssetDatabase
{

namespace
{

static std::filesystem::path NormalizePathForMap(const std::filesystem::path& p)
{
    if (p.empty())
        return p;
    // Match the platform's native fs case behavior so m_PathToGuid lookups
    // align with what the filesystem itself accepts:
    //   - Linux: case-sensitive fs → preserve real case (lex-normalize only).
    //   - Windows / macOS: case-insensitive fs → case-fold so "UI/theme.css"
    //     and "ui/theme.css" hash to the same key, matching how fs::open
    //     itself resolves them.
    //
    // Cross-platform GUID stability for path-derived identities is a
    // separate concern handled at the GUID::Derive callsites (which apply
    // NormalizeForRegistryKey explicitly). Splitting the two avoids the
    // Phase 0a regression where casefolding at this layer produced paths
    // that didn't exist on the case-sensitive Linux filesystem.
    //
    // F.8: skip the absolute() syscall when the input is already absolute.
    std::error_code ec;
    std::filesystem::path abs;
    if (p.is_absolute())
    {
        abs = p;
    }
    else
    {
        abs = std::filesystem::absolute(p, ec);
        if (ec)
            abs = p;
    }
    if (FileSystem::IsCaseSensitive())
        return abs.lexically_normal();
    // UTF-8 safety: NormalizeForRegistryKey returns a UTF-8 string;
    // construct the fs::path through char8_t* so Windows interprets the
    // bytes as UTF-8 (not ACP) for non-ASCII paths.
    const std::string s = AssetPaths::NormalizeForRegistryKey(abs);
    const auto* u8data = reinterpret_cast<const char8_t*>(s.data());
    return std::filesystem::path(u8data, u8data + s.size());
}

static std::filesystem::path ReconcilerTrimTrailingSeparators(std::filesystem::path p)
{
    // std::filesystem::path::filename() may be empty when the path ends with a
    // trailing separator. Trim those so leaf checks (".../Assets") behave
    // consistently for inputs like "Assets/" or "C:\Foo\Assets\".
    std::filesystem::path prev;
    while (!p.empty() && p.filename().empty())
    {
        prev = p;
        p = p.parent_path();
        if (p == prev)
            break; // root reached
    }
    return p;
}

static bool IsAssetsLeafDirectory(const std::filesystem::path& p)
{
    if (p.empty())
        return false;
    const std::filesystem::path trimmed = ReconcilerTrimTrailingSeparators(p);
    if (trimmed.empty())
        return false;
    return ToLowerAscii(trimmed.filename().string()) == "assets";
}

static bool StartsWithAssetsSegment(std::string canonicalPath)
{
    canonicalPath = AssetPaths::CanonicalizeStorePath(std::move(canonicalPath));

    if (canonicalPath.rfind("./", 0) == 0)
    {
        canonicalPath.erase(0, 2);
    }

    if (canonicalPath.size() < 6)
        return false;

    const std::string head = ToLowerAscii(canonicalPath.substr(0, 6));
    if (head != "assets")
        return false;

    if (canonicalPath.size() == 6)
        return true;

    return canonicalPath[6] == '/';
}

static std::string StripLeadingAssetsSegment(std::string canonicalPath)
{
    canonicalPath = AssetPaths::CanonicalizeStorePath(std::move(canonicalPath));

    while (canonicalPath.rfind("./", 0) == 0)
    {
        canonicalPath.erase(0, 2);
    }

    if (!StartsWithAssetsSegment(canonicalPath))
        return canonicalPath;

    if (canonicalPath.size() <= 6)
        return std::string{};

    if (canonicalPath[6] == '/')
        return canonicalPath.substr(7);

    return canonicalPath.substr(6);
}

static int64_t FileTimeToInt64(const std::filesystem::file_time_type& t)
{
    return static_cast<int64_t>(t.time_since_epoch().count());
}

static bool TryGetFileStats(const std::filesystem::path& path, int64_t& outSize, int64_t& outMtime)
{
    outSize = 0;
    outMtime = 0;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec))
    {
        return false;
    }
    std::error_code ec2;
    outMtime = FileTimeToInt64(std::filesystem::last_write_time(path, ec2));
    std::error_code ec3;
    outSize = static_cast<int64_t>(std::filesystem::file_size(path, ec3));
    return true;
}

static std::string GetFileIdString(const std::filesystem::path& path)
{
#ifdef PLATFORM_WINDOWS
    HANDLE h = CreateFileW(
        path.wstring().c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info))
    {
        CloseHandle(h);
        return {};
    }
    CloseHandle(h);

    const uint64_t fileIndex = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | static_cast<uint64_t>(info.nFileIndexLow);
    const uint32_t vol = info.dwVolumeSerialNumber;
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%08X-%016llX", vol, static_cast<unsigned long long>(fileIndex));
    return std::string(buf);
#else
    struct stat st;
    if (stat(path.string().c_str(), &st) != 0)
    {
        return {};
    }
    return std::to_string(static_cast<uint64_t>(st.st_dev)) + "-" + std::to_string(static_cast<uint64_t>(st.st_ino));
#endif
}

static std::string HashToHex(uint64_t h)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(h));
    return std::string(buf);
}

static std::string ComputePartialHash_Fnv1a64(const std::filesystem::path& path, size_t maxBytes, uint64_t sizeForMix)
{
    SharedFileReader in(path);
    if (!in.IsOpen())
        return {};

    uint64_t h = 14695981039346656037ULL;
    auto fnvStep = [&](uint8_t b)
    {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    };

    // Mix in file size (helps distinguish small collisions).
    for (int i = 0; i < 8; ++i)
    {
        fnvStep(static_cast<uint8_t>((sizeForMix >> (i * 8)) & 0xFF));
    }

    std::vector<char> buf;
    buf.resize(maxBytes);
    const int64 got = in.Read(buf.data(), buf.size());
    for (int64 i = 0; i < got; ++i)
    {
        fnvStep(static_cast<uint8_t>(buf[static_cast<size_t>(i)]));
    }

    return HashToHex(h);
}

/**
 * @brief Compute a sparse sampling hash for large files.
 *
 * Reads small samples from multiple positions in the file (start, 25%, 50%, 75%, end)
 * to create a distinctive fingerprint without reading the entire file.
 * Total I/O is ~20KB regardless of file size.
 *
 * @param path Path to the file
 * @param fileSize Size of the file in bytes
 * @return A hex string hash, or empty string on failure
 */
static std::string ComputeSparseHash_Fnv1a64(const std::filesystem::path& path, int64_t fileSize)
{
    constexpr size_t kSampleSize = 4096;
    constexpr size_t kNumSamples = 5;

    if (fileSize <= 0)
        return {};

    SharedFileReader in(path);
    if (!in.IsOpen())
        return {};

    // Compute sample offsets: start, 25%, 50%, 75%, end
    const auto usize = static_cast<size_t>(fileSize);
    std::array<size_t, kNumSamples> offsets = {
        0,
        usize / 4,
        usize / 2,
        (usize * 3) / 4,
        usize > kSampleSize ? usize - kSampleSize : 0
    };

    uint64_t h = 14695981039346656037ULL;
    auto fnvStep = [&](uint8_t b)
    {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ULL;
    };

    // Mix in file size first.
    for (int i = 0; i < 8; ++i)
    {
        fnvStep(static_cast<uint8_t>((static_cast<uint64_t>(fileSize) >> (i * 8)) & 0xFF));
    }

    // Mix in offset marker to distinguish samples at different positions.
    std::vector<char> buf(kSampleSize);
    for (size_t sampleIdx = 0; sampleIdx < kNumSamples; ++sampleIdx)
    {
        const size_t offset = offsets[sampleIdx];

        // Mix in offset as a position marker.
        for (int i = 0; i < 8; ++i)
        {
            fnvStep(static_cast<uint8_t>((offset >> (i * 8)) & 0xFF));
        }

        if (!in.SeekTo(offset))
            continue;

        const int64 got = in.Read(buf.data(), kSampleSize);
        for (int64 i = 0; i < got; ++i)
        {
            fnvStep(static_cast<uint8_t>(buf[static_cast<size_t>(i)]));
        }
    }

    return HashToHex(h);
}

static std::vector<std::filesystem::path> EnumerateAssetFilesOnDisk(const std::filesystem::path& root,
                                                                    const AssetIgnoreRules& rules)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec))
        return out;

    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        const auto& entry = *it;
        const auto p = entry.path();

        if (entry.is_directory(ec))
        {
            if (rules.ShouldIgnoreDirectory(p, root))
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file(ec))
            continue;

        if (rules.ShouldIgnoreFile(p, root))
            continue;

        out.push_back(p);
    }

    return out;
}

/// A tier-4 pairing awaiting the end of the match loop (see the matcher).
struct PendingRenameSuggestion
{
    GUID MissingGuid;
    /// The candidate's registered GUID; null under the Stored scheme.
    GUID CandidateGuid;
    std::string CandidatePath;
};

// Enough log lines to see the shape of a bad reconcile without a 500-line wall
// when a whole tree moves; the count in the summary line carries the rest.
constexpr size_t kMaxSuggestionLogLines = 5;

// Persist the run's weak-evidence pairings and announce them. Rate limiting is
// per run rather than per wall-clock interval because this pass runs once per
// mount at startup: a clock-based gate would silently drop every suggestion
// after the first and report no total at all.
void RecordRenameSuggestions(const IAssetStore& store,
                             IAssetDbCache& cache,
                             const std::vector<PendingRenameSuggestion>& pending,
                             const std::unordered_set<std::string>& matchedGuids,
                             ReconcileStats& outStats)
{
    size_t logged = 0;
    for (const PendingRenameSuggestion& p : pending)
    {
        // A stronger tier claimed this absentee after the suggestion was
        // queued — it is healing, not waiting for review.
        if (matchedGuids.find(p.MissingGuid.ToString()) != matchedGuids.end())
            continue;

        AssetRecord missingRec{};
        if (!store.TryGetAsset(p.MissingGuid, missingRec))
            continue;

        IAssetDbCache::RenameSuggestion suggestion{};
        suggestion.MissingGuid = p.MissingGuid;
        suggestion.MissingPath = missingRec.path;
        suggestion.CandidateGuid = p.CandidateGuid;
        suggestion.CandidatePath = p.CandidatePath;
        suggestion.EvidenceTier = kRenameEvidenceTierDirExt;
        if (!cache.RecordRenameSuggestion(suggestion, nullptr))
            continue;

        ++outStats.Suggested;
        if (logged < kMaxSuggestionLogLines)
        {
            ++logged;
            Logger::Log::Warning(
                "AssetRegistry: possible rename '{}' ({}) -> '{}' — same folder and extension is "
                "the only evidence, so it was NOT healed and '{}' stays missing. Recorded as a "
                "suggestion; confirming it emits the redirect that 'Asset Database/Fix Up "
                "Redirects' then propagates to referencing assets.",
                missingRec.path, p.MissingGuid.ToString(), p.CandidatePath, missingRec.path);
        }
    }

    if (outStats.Suggested > logged)
    {
        Logger::Log::Warning(
            "AssetRegistry: {} further rename suggestion(s) recorded and not logged individually.",
            outStats.Suggested - logged);
    }
}

// Drop advisory pairings whose subject is no longer a missing record. A
// suggestion outlives its subject in ways neither of the two existing removal
// paths sees: the Stored scheme rebinds an absentee in a later session without
// routing through the cache's re-clear, and a candidate can itself vanish,
// leaving a row that points at nothing. `stillMissing` is this run's absentee
// set — scheme-neutral by construction, which a SQL predicate over the cache's
// own missing column would not be (Stored rests the flag in the journal).
void PurgeStaleRenameSuggestions(IAssetDbCache& cache,
                                 const std::vector<AssetRecord>& stillMissing,
                                 ReconcileStats& outStats)
{
    const std::vector<IAssetDbCache::RenameSuggestion> suggestions = cache.EnumerateRenameSuggestions();
    if (suggestions.empty())
        return;

    std::unordered_set<GUID> missingGuids;
    missingGuids.reserve(stillMissing.size());
    for (const AssetRecord& rec : stillMissing)
        missingGuids.insert(rec.guid);

    for (const IAssetDbCache::RenameSuggestion& s : suggestions)
    {
        if (missingGuids.count(s.MissingGuid) != 0)
            continue;
        if (cache.RemoveRenameSuggestion(s.MissingGuid, nullptr))
            ++outStats.StaleSuggestionsPurged;
    }
}

// Enough log lines to name the ghosts a run collected without a wall of them
// when a long-dead folder finally ages out; the summary carries the count.
constexpr size_t kMaxCollectedGhostLogLines = 5;

/// Retention pass (Derived only). `absentees` is this run's missing set;
/// `matchedGuids` names the ones a heal already consumed. Returns true if the
/// STORE changed.
bool CollectGhostGarbage(IAssetStore& store,
                         IAssetDbCache& cache,
                         const ReconcileGhostRetention& retention,
                         const std::vector<AssetRecord>& absentees,
                         const std::unordered_set<std::string>& matchedGuids,
                         ReconcileStats& outStats,
                         std::vector<GUID>* outCollected)
{
    if (!retention.Armed() || absentees.empty())
        return false;

    // A cache built from scratch this session has empty derived indices, and
    // the two facts this pass destroys a record on are read from them. Neither
    // recovers in time: fingerprints repopulate only for files the scan
    // actually processes, and dependency edges are built LAZILY on first query
    // â no scan path writes them at all. So every ghost would read
    // never-healable with zero dependents, and a referenced tombstone whose
    // references simply have not been queried yet would be collected. Wait for
    // a session whose indices mean something.
    if (cache.WasSchemaReset())
    {
        Logger::Log::Info(
            "AssetRegistry: ghost retention skipped this session - the asset cache was rebuilt "
            "from scratch, so dependency and fingerprint data carry no evidence yet");
        return false;
    }

    std::unordered_map<GUID, IAssetDbCache::GhostRow> ghostRows;
    for (const IAssetDbCache::GhostRow& row : cache.EnumerateGhostRows())
        ghostRows.emplace(row.Guid, row);
    if (ghostRows.empty())
        return false;

    // An outstanding suggestion is a rename hypothesis waiting on the user.
    // Collecting its subject would answer the question by destroying it, so a
    // suggested ghost is retained however old it is.
    std::unordered_set<GUID> suggested;
    for (const IAssetDbCache::RenameSuggestion& s : cache.EnumerateRenameSuggestions())
        suggested.insert(s.MissingGuid);

    const int64_t currentSession = cache.GetSessionCounter();
    bool dirty = false;
    size_t logged = 0;

    for (const AssetRecord& rec : absentees)
    {
        // Defence in depth rather than the ordering guarantee: a heal already
        // removed this ghost's cache row, so the lookup below would miss it
        // anyway. What actually keeps a healable ghost is that this pass runs
        // after the matcher.
        if (matchedGuids.count(rec.guid.ToString()) != 0)
            continue;
        auto rowIt = ghostRows.find(rec.guid);
        if (rowIt == ghostRows.end())
            continue;
        IAssetDbCache::GhostRow& row = rowIt->second;

        // Unstamped: the record was flagged before retention existed, or by a
        // path that had no session to stamp. Start its clock now — inventing
        // an age would collect a ghost this machine has never actually
        // watched sit missing.
        if (row.MissingSinceSession <= 0)
        {
            row.MissingSinceSession = currentSession;
            if (cache.SetAssetMissing(rec, true, currentSession, nullptr))
                ++outStats.GhostsStamped;
        }

        const int64_t age = currentSession - row.MissingSinceSession;
        const bool agedOut =
            retention.MaxMissingSessions > 0 &&
            age >= static_cast<int64_t>(retention.MaxMissingSessions);
        const bool neverHealable = retention.CollectNeverHealable && !row.HasFingerprint;
        if (!agedOut && !neverHealable)
            continue;

        // Her acceptance requirement is that a deleted asset's references
        // resolve to its tombstone rather than to nothing. Anything still
        // pointing at this ghost keeps it alive regardless of age or
        // healability — the same predicate the live-delete path uses to
        // choose tombstone over removal, minus its liveness refinement (the
        // registry's resident map is not reachable from here), so this errs
        // toward retaining.
        if (suggested.count(rec.guid) != 0 ||
            cache.CountDependents(rec.guid) > 0 ||
            (!rec.path.empty() && cache.CountDependentsByPath(rec.path) > 0) ||
            !cache.EnumerateProducedAssets(rec.guid).empty())
        {
            ++outStats.GhostsRetained;
            continue;
        }

        // Disposal is S2's ghost-removal shape: journal record, cache row
        // (which takes deps, kv mirror and any rename suggestion with it),
        // and the provenance row RemoveAsset deliberately leaves behind.
        (void)store.RemoveAsset(rec.guid, nullptr);
        (void)cache.RemoveAsset(rec.guid, nullptr);
        (void)cache.UnregisterProvenance(rec.guid, nullptr);
        dirty = true;
        ++outStats.GhostsCollected;
        if (outCollected)
            outCollected->push_back(rec.guid);

        if (logged < kMaxCollectedGhostLogLines)
        {
            ++logged;
            Logger::Log::Info(
                "AssetRegistry: collected ghost '{}' ({}) - missing for {} session(s){}, nothing "
                "references it",
                rec.path, rec.guid.ToString(), age,
                row.HasFingerprint ? "" : ", no fingerprint so no rename match was ever possible");
        }
    }

    if (outStats.GhostsCollected > logged)
    {
        Logger::Log::Info("AssetRegistry: {} further ghost(s) collected and not logged individually.",
                          outStats.GhostsCollected - logged);
    }

    return dirty;
}

} // namespace

bool PruneIgnoredStoreRecords(const AssetIgnoreRules& rules,
                              IAssetStore& store,
                              IAssetDbCache* cache,
                              size_t& outRemoved)
{
    outRemoved = 0;
    bool dirty = false;

    const std::vector<AssetRecord> records = store.EnumerateAssets();
    for (const auto& rec : records)
    {
        if (rec.guid.IsNull() || rec.path.empty())
            continue;

        if (!rules.ShouldIgnoreCanonicalRelativePath(rec.path))
            continue;

        (void)store.RemoveAsset(rec.guid, nullptr);
        if (cache)
        {
            (void)cache->RemoveAsset(rec.guid, nullptr);
        }
        ++outRemoved;
        dirty = true;
    }

    return dirty;
}

bool RecoverQuarantinedStoreRecords(const std::filesystem::path& assetRoot,
                                    const std::vector<AssetRecord>& quarantined,
                                    IAssetStore& store,
                                    IAssetDbCache* cache,
                                    size_t& outRepaired,
                                    size_t& outDropped)
{
    outRepaired = 0;
    outDropped = 0;

    for (const auto& rec : quarantined)
    {
        if (rec.guid.IsNull())
            continue;

        std::string canonicalRel;
        if (AssetPaths::TryMakeCanonicalRelativePath(assetRoot, std::filesystem::path(rec.path), canonicalRel) &&
            !canonicalRel.empty())
        {
            // Under this source's own root: re-admit with the path repaired
            // to canonical-relative, preserving the GUID and any
            // user-authored kv — unless the relative key already belongs to
            // another GUID, in which case that record is the healthy one and
            // the quarantined row is a stale duplicate.
            const auto relOwner = store.LookupGuidByPath(canonicalRel);
            if (!relOwner || *relOwner == rec.guid)
            {
                AssetRecord updated = rec;
                updated.path = canonicalRel;
                (void)store.UpsertAsset(updated, nullptr);
                if (cache)
                {
                    (void)cache->UpsertAsset(updated, nullptr);
                }
                ++outRepaired;
                continue;
            }
        }

        // Not recoverable under this root: the record never entered the
        // store, so only the derived cache needs scrubbing. RemoveAsset
        // drops the asset row (fingerprint columns included), the kv
        // mirror, and both directions of dep edges; provenance has its own
        // lifecycle API and is dropped explicitly.
        if (cache)
        {
            (void)cache->RemoveAsset(rec.guid, nullptr);
            (void)cache->UnregisterProvenance(rec.guid, nullptr);
        }
        ++outDropped;
    }

    return outRepaired > 0;
}

bool MergeDisplacedStoreRecords(const std::vector<AssetRecord>& displaced,
                                IAssetStore& store,
                                IAssetDbCache* cache,
                                size_t& outRedirected,
                                size_t& outSubassetRedirects)
{
    outRedirected = 0;
    outSubassetRedirects = 0;

    bool dirty = false;
    for (const auto& old : displaced)
    {
        if (old.guid.IsNull() || old.path.empty())
            continue;

        // The survivor is whoever holds the path key now. LookupGuidByPath
        // folds, so the displaced row's own spelling finds it.
        const auto survivor = store.LookupGuidByPath(old.path);
        if (!survivor || survivor->IsNull() || *survivor == old.guid)
            continue;

        AssetRecord newRec{};
        if (!store.TryGetAsset(*survivor, newRec))
            continue;

        // Copy-if-absent: the survivor's own metadata always wins, but keys
        // only the retired row carried (derive keys, importer settings) must
        // not be lost with it.
        for (const auto& [key, value] : old.kv)
        {
            if (newRec.kv.find(key) != newRec.kv.end())
                continue;
            (void)store.SetKeyValue(*survivor, key, value, nullptr);
            if (cache)
                (void)cache->SetKeyValue(*survivor, key, value, nullptr);
        }

        // Cascade before the container redirect — same ordering rationale as
        // the offline rename heal.
        if (auto itKeys = old.kv.find(kSubassetDeriveKeysKvKey); itKeys != old.kv.end())
        {
            for (const std::string& key : SplitSubassetDeriveKeys(itKeys->second))
            {
                if (store.AddRedirect(GUID::Derive(old.guid, key),
                                      GUID::Derive(*survivor, key), nullptr))
                {
                    ++outSubassetRedirects;
                }
            }
        }

        const bool redirected = store.AddRedirect(old.guid, *survivor, nullptr);
        if (cache)
        {
            (void)cache->RedirectProvenance(old.guid, *survivor, nullptr);
            (void)cache->RemoveAsset(old.guid, nullptr);
        }

        // Dirty is unconditional on purpose: the displaced rows stay in the
        // journal until a flush compacts them, so a re-drain that adds no new
        // redirects must still request the flush that scrubs the dead rows.
        dirty = true;
        if (redirected)
        {
            ++outRedirected;
            Logger::Log::Info(
                "AssetDatabase: retired duplicate identity {} onto {} — '{}' and '{}' are one asset",
                old.guid.ToString(), survivor->ToString(), old.path, newRec.path);
        }
    }

    return dirty;
}

bool NormalizeLegacyStorePathsIfNeeded(const std::filesystem::path& assetRoot,
                                       IAssetStore& store,
                                       IAssetDbCache* cache,
                                       size_t& outFixed,
                                       size_t& outConflicts)
{
    outFixed = 0;
    outConflicts = 0;

    const std::filesystem::path root = ReconcilerTrimTrailingSeparators(assetRoot);
    if (!IsAssetsLeafDirectory(root))
        return false;

    bool dirty = false;
    const std::vector<AssetRecord> records = store.EnumerateAssets();

    for (const auto& rec : records)
    {
        if (rec.guid.IsNull() || rec.path.empty())
            continue;

        const std::string norm = AssetPaths::CanonicalizeStorePath(rec.path);
        if (!StartsWithAssetsSegment(norm))
            continue;

        const std::string stripped = StripLeadingAssetsSegment(norm);
        if (stripped.empty())
            continue;

        // If the legacy path would resolve to "<assetRoot>/Assets/..." under the current
        // assetRoot, normalize it unless that legacy resolution actually exists on disk.
        const std::filesystem::path absOld = NormalizePathForMap(root / std::filesystem::path(norm));

        std::error_code ec;
        const bool oldExists = std::filesystem::exists(absOld, ec) && std::filesystem::is_regular_file(absOld, ec);

        // If the "old" path exists, do not rewrite; it may be a real nested
        // Assets/Assets/... layout (rare, but we should not break it).
        if (oldExists)
            continue;

        // Avoid collisions: don't rewrite if the stripped path already maps to a different GUID.
        if (auto other = store.LookupGuidByPath(stripped))
        {
            if (*other != rec.guid)
            {
                ++outConflicts;
                continue;
            }
        }

        AssetRecord updated = rec;
        updated.path = stripped;
        (void)store.UpsertAsset(updated, nullptr);
        if (cache)
        {
            (void)cache->UpsertAsset(updated, nullptr);
        }

        dirty = true;
        ++outFixed;
    }

    return dirty;
}

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
                                   std::vector<GUID>* outCollectedGhosts)
{
    outStats = ReconcileStats{};
    bool dirty = false;

    const bool derived = (scheme == ReconcileIdentityScheme::Derived);

    // Derived identity keeps its missing observations in the per-machine
    // cache; with no cache there is nowhere to read or write them (the
    // Player runs cache-less by design) — degrade to no marking at all,
    // never wrong marking.
    if (derived && !cache)
        return false;

    // Was-missing view: journaled record flag for stored identity, the
    // cache's ghost set for derived. One bulk query, O(ghosts) — served by
    // the cache's partial ghost index, not a scan of every record.
    std::unordered_set<GUID> cacheMissing;
    if (derived)
    {
        const std::vector<GUID> missingGuids = cache->EnumerateMissingAssets();
        cacheMissing.insert(missingGuids.begin(), missingGuids.end());
    }

    auto wasMissing = [&](const AssetRecord& rec) {
        return derived ? (cacheMissing.count(rec.guid) != 0) : rec.missing;
    };

    // C.3: per-record exists-walk gating via dir mtime. The snapshot's
    // recorded dir mtime is compared against the on-disk dir mtime; when
    // they match, the directory's contents haven't been added/removed/
    // renamed since the snapshot was written. Records in such dirs are
    // proven-existing without a per-record `std::filesystem::exists`
    // syscall.
    //
    // Move detection still works: a move (file from dir A to dir B)
    // changes BOTH A's and B's mtimes, so neither side's gate matches
    // and we fall through to the existing exists-check + missing-list
    // path that drives the move-matching logic later.
    //
    // Tradeoff (same as C.2): a between-session file-CONTENT edit that
    // doesn't change the parent dir's mtime goes undetected here. But
    // the existing behaviour of this loop only sets the missing flag
    // based on existence, not content — content-divergence isn't
    // detected here either way.
    const bool dirGatingEnabled = snapshotDirMtimeByPath && !snapshotDirMtimeByPath->empty();
    // canonical-rel dir -> mtime-matches-snapshot. Transparent hashing so the
    // per-record probe keys on a view into the record's path; only a memo miss
    // materializes the key string, once per directory.
    struct DirKeyHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, bool, DirKeyHash, std::equal_to<>> dirIsClean;

    auto isDirClean = [&](std::string_view dirRel) -> bool {
        if (!dirGatingEnabled || dirRel.empty())
            return false;
        auto cacheIt = dirIsClean.find(dirRel);
        if (cacheIt != dirIsClean.end())
            return cacheIt->second;

        std::string dirKey(dirRel);
        bool clean = false;
        auto snapIt = snapshotDirMtimeByPath->find(dirKey);
        if (snapIt != snapshotDirMtimeByPath->end())
        {
            const std::filesystem::path absDir =
                assetRoot / std::filesystem::path(dirKey);
            std::error_code mtEc;
            const auto mtime = std::filesystem::last_write_time(absDir, mtEc);
            if (!mtEc)
            {
                const int64_t curTicks = static_cast<int64_t>(mtime.time_since_epoch().count());
                clean = (snapIt->second == curTicks);
            }
        }
        dirIsClean.emplace(std::move(dirKey), clean);
        return clean;
    };

    // Pass 1 — select the records that still need a disk check, walking the
    // store's records in place. The dir-gate answers for the rest from the
    // snapshot and copies nothing, so a warm pass over clean directories
    // materializes no records at all.
    //
    // The visitor runs under the store's lock and must not re-enter it
    // (IAssetStore::IterateAssets), which is why the existence checks and
    // every store/cache mutation wait for pass 2. Those mutations are
    // per-GUID independent and the loop's other outputs are a count and an
    // OR, so deferring them changes no result.
    //
    // The dir-mtime stats inside isDirClean deliberately stay in the visitor:
    // memoization bounds them to one stat per unique parent directory, and
    // even the branch-switch worst case (every directory dirty, so every
    // lookup stats) holds the lock for less time than the full-store copy
    // this pass replaced.
    std::vector<AssetRecord> needsDiskCheck;
    if (!dirGatingEnabled)
    {
        // No snapshot to gate on: every record reaches the disk check, so
        // size the vector once rather than growing it per record.
        needsDiskCheck.reserve(store.CountAssets());
    }
    store.IterateAssets([&](const AssetRecord& rec) {
        if (rec.guid.IsNull() || rec.path.empty())
            return;

        // Dir-gating fast path: if the record's parent dir is clean and the
        // record was non-missing (store flag for stored identity, cache flag
        // for derived), trust the snapshot. Gating is tested first so a run
        // without a snapshot does no per-record work here at all.
        if (dirGatingEnabled && !wasMissing(rec) &&
            isDirClean(AssetPaths::StorePathParentDir(rec.path)))
        {
            return;
        }

        needsDiskCheck.push_back(rec);
    });

    // Pass 2 — the disk checks, and the missing-flag transitions they imply.
    std::vector<AssetRecord> missing;
    missing.reserve(needsDiskCheck.size());

    for (const AssetRecord& rec : needsDiskCheck)
    {
        const std::filesystem::path abs = NormalizePathForMap(assetRoot / std::filesystem::path(rec.path));
        std::error_code ec;
        const bool exists = std::filesystem::exists(abs, ec) && std::filesystem::is_regular_file(abs, ec);
        ++outStats.ExistenceChecks;

        if (!exists)
        {
            missing.push_back(rec);
            if (!wasMissing(rec))
            {
                if (derived)
                {
                    // Stamped once, here, at the false->true transition: this
                    // is the only place the Derived scheme learns a record
                    // just went missing, and re-stamping a standing ghost
                    // every session would keep its age at zero forever.
                    (void)cache->SetAssetMissing(rec, true, cache->GetSessionCounter(), nullptr);
                }
                else
                {
                    (void)store.MarkMissing(rec.guid, true, nullptr);
                    dirty = true;
                }
            }
        }
        else if (wasMissing(rec))
        {
            if (derived)
            {
                (void)cache->SetAssetMissing(rec, false, 0, nullptr);
            }
            else
            {
                (void)store.MarkMissing(rec.guid, false, nullptr);
                dirty = true;
            }
            // The file came back at its own path, so any weak-evidence guess
            // about where it went is now provably wrong. (The other way a
            // suggestion dies is the record itself being removed, which
            // IAssetDbCache::RemoveAsset covers.)
            if (cache)
                (void)cache->RemoveRenameSuggestion(rec.guid, nullptr);
        }
    }

    // Stale-redirect sweep (derived): a redirect whose SOURCE record is live
    // and file-backed shadows that record — the registry's redirect-aware
    // lookups prefer the chased target whenever the target record exists.
    // Registration clears such a redirect the moment the source GUID
    // re-registers; this sweep clears residue those paths never revisit (a
    // snapshot-current file re-upserts nothing, so a pre-existing stale
    // redirect would otherwise shadow it forever). The target-record-exists
    // requirement is the completion sweep's discriminator, reused: a
    // recordless-target redirect is load-bearing (live-rename residue,
    // derived→stable identity) and is kept, and an absentee source is left
    // for the completion sweep / tier matcher below. Incoming hops are
    // retargeted to the chain-final target before a removal, so a reference
    // that reached the source through an older rename keeps following the
    // rename direction instead of binding to whatever reclaimed the GUID.
    if (derived)
    {
        const std::vector<RedirectRecord> redirects = store.EnumerateRedirects();
        if (!redirects.empty())
        {
            std::unordered_set<GUID> missingGuids;
            missingGuids.reserve(missing.size());
            for (const auto& rec : missing)
                missingGuids.insert(rec.guid);

            for (const auto& rr : redirects)
            {
                AssetRecord sourceRec{};
                if (rr.from.IsNull() || !store.TryGetAsset(rr.from, sourceRec) ||
                    missingGuids.count(rr.from) != 0)
                {
                    continue;
                }

                // At startup the registry's resident map is not populated yet,
                // so store records are the only liveness signal available.
                const RedirectChain chain =
                    ChaseRedirectChain(store, rr.from, RedirectTargetCheck::StoreRecord, {});
                if (!chain.TargetAccepted)
                    continue;
                AssetRecord targetRec{};
                if (!store.TryGetAsset(chain.Final, targetRec))
                    continue;

                // The container's cascades die with it: the chain-final target
                // record carries the journaled derive keys (kv-migrated at
                // heal time), and each Derive(from, key) redirect shadows the
                // live source's own derived subasset identities exactly like
                // the container redirect shadowed its file. Cascade entries
                // themselves are never candidates here — their sources have no
                // records, so the source-liveness guard above skips them.
                std::vector<GUID> victims{rr.from};
                if (auto itKeys = targetRec.kv.find(kSubassetDeriveKeysKvKey);
                    itKeys != targetRec.kv.end())
                {
                    for (const std::string& key : SplitSubassetDeriveKeys(itKeys->second))
                        victims.push_back(GUID::Derive(rr.from, key));
                }

                // The rename direction outlives the removal: references that
                // reached this GUID inherit its chain-final target rather than
                // binding to the unrelated file that reclaimed the GUID.
                // Retargeting only shortens chains, so entries this run has
                // already rewritten still chase to the same place.
                const size_t retargeted = RetargetIncomingRedirects(store, victims);

                (void)store.RemoveRedirect(rr.from, nullptr);
                dirty = true;
                ++outStats.StaleRedirectsRemoved;
                for (size_t i = 1; i < victims.size(); ++i)
                {
                    if (store.RemoveRedirect(victims[i], nullptr))
                        ++outStats.SubassetRedirectsRemoved;
                }
                Logger::Log::Info(
                    "AssetRegistry: removed stale redirect {} -> {} - its source is live again at "
                    "'{}'; {} incoming hop(s) retargeted",
                    rr.from.ToString(), chain.Final.ToString(), sourceRec.path, retargeted);
            }
        }
    }

    outStats.MissingObserved = missing.size();

    // Suggestion hygiene runs ahead of the absentee fast-path on purpose: the
    // run that finds NOTHING missing is exactly the run where every standing
    // suggestion has lost its subject. Heals and collections drop their own
    // subjects' rows via IAssetDbCache::RemoveAsset, so this run's absentee
    // set is the right reference even though it is taken before both.
    if (cache)
        PurgeStaleRenameSuggestions(*cache, missing, outStats);

    // Fast-path: if there are no missing records (or no derived cache available),
    // there is nothing to reconcile. Avoid scanning/hash-computing every file on disk.
    //
    // IMPORTANT: This keeps Editor/Engine startup non-blocking for new projects where the
    // authoritative DB is empty (or has no missing tombstones).
    if (missing.empty() || !cache)
    {
        return dirty;
    }

    // Interrupted-heal completion (derived): an absentee whose redirect
    // points at an EXISTING record is a heal whose final step — ghost record
    // removal — was lost (the redirect commits before the removal). Finish
    // the removal and drop the ghost from this run's match set; its rename
    // target has been a registered record since the heal, so it can never
    // re-appear as a candidate.
    //
    // The target-record check discriminates against live-rename residue:
    // TryRenameAssetPath leaves {record old-guid @ new-path} plus a redirect
    // old→new-derived until the next re-registration displaces the record,
    // so that redirect's target has NO record by construction. In a genuine
    // interrupted heal the target record precedes the redirect in
    // store-mutation order, so any flushed state containing the redirect
    // contains the target record. Residue absentees fall through instead: a
    // deleted rename-target keeps its tombstone record, and a re-renamed one
    // stays matchable by the tier matcher below.
    if (derived)
    {
        std::vector<AssetRecord> unhealed;
        unhealed.reserve(missing.size());
        for (const auto& rec : missing)
        {
            const std::optional<GUID> target = store.ResolveRedirect(rec.guid);
            AssetRecord targetRec{};
            if (!target || target->IsNull() || *target == rec.guid ||
                !store.TryGetAsset(*target, targetRec))
            {
                unhealed.push_back(rec);
                continue;
            }
            // Torn-append recovery for cascades: within one flush the redirect
            // section drains from an unordered set, so a torn append can
            // persist the container redirect while losing cascade lines. The
            // target record precedes every redirect in the flush buffer and
            // carries the container's kv-migrated derive keys, so any state
            // that contains the container redirect can rebuild its cascades
            // here. Skip-if-identical keeps the common uninterrupted case
            // journal-quiet; a divergent cascade is re-pointed at the
            // container redirect's target (the source of truth).
            if (auto itKeys = targetRec.kv.find(kSubassetDeriveKeysKvKey);
                itKeys != targetRec.kv.end())
            {
                for (const std::string& key : SplitSubassetDeriveKeys(itKeys->second))
                {
                    const GUID from = GUID::Derive(rec.guid, key);
                    const GUID to = GUID::Derive(*target, key);
                    if (store.ResolveRedirect(from) == std::optional<GUID>(to))
                        continue;
                    if (store.AddRedirect(from, to, nullptr))
                    {
                        dirty = true;
                        ++outStats.SubassetRedirects;
                    }
                }
            }
            (void)cache->RedirectProvenance(rec.guid, *target, nullptr);
            (void)store.RemoveAsset(rec.guid, nullptr);
            (void)cache->RemoveAsset(rec.guid, nullptr);
            dirty = true;
            ++outStats.HealsCompleted;
            if (outHeals)
                outHeals->push_back(ReconcileHeal{rec.guid, *target});
            Logger::Log::Info(
                "AssetRegistry: completed interrupted rename heal {} -> {} (ghost record removed)",
                rec.guid.ToString(), target->ToString());
        }
        missing = std::move(unhealed);
        if (missing.empty())
        {
            return dirty;
        }
    }

    // New-file candidates for the rename matcher.
    struct NewFile
    {
        GUID guid; // Derived scheme only: the candidate's registered path-hash GUID.
        std::string canonicalRel;
        int64_t size = 0;
        int64_t mtime = 0;
        std::string fileId;
        std::string hash;
    };

    // During reconciliation, we use a higher size limit for hash computation since this is
    // a one-time startup cost and we want to maximize GUID preservation for moved files.
    // Regular fingerprint updates still use the lower 256KB limit for performance.
    constexpr int64_t kReconcileHashSizeLimit = 16 * 1024 * 1024; // 16MB

    std::vector<NewFile> newFiles;
    if (derived)
    {
        // Derived identity never walks the disk: candidates are the scan's
        // !hadExisting registrations, whose fingerprints were already
        // computed for the cache batch. No scan candidates, no matching —
        // the absentees stay ghosts (missing-marked in the cache) until a
        // later scan surfaces their rename targets.
        if (scanNewFiles)
        {
            newFiles.reserve(scanNewFiles->size());
            for (const ReconcileScanNewFile& snf : *scanNewFiles)
            {
                if (snf.Guid.IsNull() || snf.CanonicalRel.empty())
                    continue;
                NewFile nf{};
                nf.guid = snf.Guid;
                nf.canonicalRel = snf.CanonicalRel;
                nf.size = snf.Size;
                nf.mtime = snf.Mtime;
                nf.fileId = snf.FileId;
                nf.hash = snf.ContentHash;
                newFiles.push_back(std::move(nf));
            }
        }
    }
    else
    {
        const std::vector<std::filesystem::path> diskFiles = EnumerateAssetFilesOnDisk(assetRoot, rules);
        newFiles.reserve(diskFiles.size());
        for (const auto& p : diskFiles)
        {
            std::string canonicalRel;
            if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, p, canonicalRel))
                continue;

            if (auto existingGuid = store.LookupGuidByPath(canonicalRel))
            {
                // File exists on disk at this canonical path — it's not missing.
                // Clear a stale missing flag if the exists check incorrectly marked it absent
                // (e.g. case-sensitive filesystem: stored lowercase path != real mixed-case path).
                AssetRecord existingRec{};
                if (store.TryGetAsset(*existingGuid, existingRec) && existingRec.missing)
                {
                    (void)store.MarkMissing(*existingGuid, false, nullptr);
                    dirty = true;
                }
                continue;
            }

            NewFile nf{};
            nf.canonicalRel = canonicalRel;
            (void)TryGetFileStats(p, nf.size, nf.mtime);
            nf.fileId = GetFileIdString(p);

            // Block D: warm-start delta scan. If the snapshot has an entry
            // for this canonical path AND its (mtime, size, fileId) matches
            // what we just stat'd, reuse the cached hash from the previous
            // session — saves the partial/sparse-hash I/O. Stat-confirm
            // contract documented in AssetSourceSnapshot.h.
            bool hashFromSnapshot = false;
            if (snapshotByPath && !snapshotByPath->empty())
            {
                auto it = snapshotByPath->find(AssetPaths::FoldStorePathKey(canonicalRel));
                if (it != snapshotByPath->end())
                {
                    const auto& snap = it->second;
                    const bool statMatches = (snap.Mtime == nf.mtime) &&
                                             (snap.Size  == nf.size)  &&
                                             (snap.FileId == nf.fileId);
                    if (statMatches && !snap.Hash.empty())
                    {
                        nf.hash = snap.Hash;
                        hashFromSnapshot = true;
                        ++outStats.HashesReused;
                    }
                }
            }

            if (!hashFromSnapshot)
            {
                // Compute hash - use partial hash for smaller files, sparse sampling for larger.
                if (nf.size <= kReconcileHashSizeLimit)
                {
                    nf.hash = ComputePartialHash_Fnv1a64(p, 64 * 1024, static_cast<uint64_t>(nf.size));
                }
                else
                {
                    // For large files, use sparse sampling (~20KB I/O regardless of file size).
                    nf.hash = ComputeSparseHash_Fnv1a64(p, nf.size);
                }
            }

            newFiles.push_back(std::move(nf));
        }
    }

    if (newFiles.empty())
    {
        // No candidates means no heal was possible this session, so every
        // surviving absentee is already a retention candidate. This is the
        // path a project full of standing ghosts takes every warm start.
        if (derived && CollectGhostGarbage(store, *cache, retention, missing, {}, outStats,
                                           outCollectedGhosts))
        {
            dirty = true;
        }
        return dirty;
    }

    // Build lookup maps from cached fingerprints for missing assets.
    std::unordered_map<std::string, std::vector<GUID>> missingByFileId;
    std::unordered_map<std::string, std::vector<GUID>> missingByHashSize;
    // Legacy fallback: size + extension for old cache entries without sparse hashes.
    // New registrations always have hashes (sparse for large files), but this handles
    // cache entries created before sparse hashing was introduced.
    std::unordered_map<std::string, std::vector<GUID>> missingBySizeExt;

    // Per-directory uniqueness tier for the "rename + edit within same folder"
    // case. file_id breaks across `git pull` (git creates fresh files, not
    // renames), and hash+size breaks when content is edited, so co-location is
    // all that is left: exactly one missing record and exactly one new file
    // sharing a parent dir and extension.
    //
    // That is a guess, not evidence — two unrelated .png edits in one folder
    // produce the same 1:1 shape as a genuine rename — so this tier SUGGESTS
    // and never heals. The tiers above it match bytes or OS file identity and
    // stay automatic.
    auto buildDirExtKey = [](std::string_view canonicalRel) -> std::string {
        std::filesystem::path p((std::string(canonicalRel)));
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string parent;
        if (p.has_parent_path())
            parent = p.parent_path().generic_string();
        return parent + "|" + ext;
    };
    std::unordered_map<std::string, std::vector<GUID>> missingByDirExt;

    for (const auto& rec : missing)
    {
        IAssetDbCache::FileFingerprint fp;
        if (!cache->TryGetFileFingerprint(rec.guid, fp))
            continue;

        if (!fp.fileId.empty())
        {
            missingByFileId[fp.fileId].push_back(rec.guid);
        }

        if (!fp.contentHash.empty())
        {
            const std::string key = fp.contentHash + "|" + std::to_string(fp.size);
            missingByHashSize[key].push_back(rec.guid);
        }

        // For large files without hash, use size + extension as fallback.
        if (fp.contentHash.empty() && fp.size > kReconcileHashSizeLimit)
        {
            std::filesystem::path recPath(rec.path);
            std::string ext = recPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string key = std::to_string(fp.size) + "|" + ext;
            missingBySizeExt[key].push_back(rec.guid);
        }

        // Per-dir-ext index: same canonical-relative parent directory + same
        // file extension. Only used as the last fallback; populated for every
        // missing record that has a cache fingerprint row (the loop's guard
        // above skips records the cache never fingerprinted).
        missingByDirExt[buildDirExtKey(rec.path)].push_back(rec.guid);
    }

    // Pre-count new files per (dir, ext) — symmetrical to missingByDirExt.
    // Only a (dir, ext) bucket with exactly one missing AND one new is
    // safe to bind via this fallback.
    std::unordered_map<std::string, size_t> newFilesByDirExt;
    for (const auto& nf : newFiles)
        ++newFilesByDirExt[buildDirExtKey(nf.canonicalRel)];

    std::unordered_set<std::string> matchedGuids;
    matchedGuids.reserve(missing.size());

    // Tier-4 pairings, held until the loop ends: a stronger tier later in the
    // same run may still claim the absentee, and a suggestion about a record
    // that just healed is noise. Filtered against matchedGuids below.
    std::vector<PendingRenameSuggestion> pendingSuggestions;

    for (const auto& nf : newFiles)
    {
        GUID match = GUID::Null();
        bool ambiguous = false;

        if (!nf.fileId.empty())
        {
            auto it = missingByFileId.find(nf.fileId);
            if (it != missingByFileId.end())
            {
                std::vector<GUID> candidates;
                candidates.reserve(it->second.size());
                for (const GUID& g : it->second)
                {
                    if (matchedGuids.find(g.ToString()) == matchedGuids.end())
                        candidates.push_back(g);
                }
                if (candidates.size() == 1)
                {
                    match = candidates[0];
                }
                else if (candidates.size() > 1)
                {
                    ambiguous = true;
                }
            }
        }

        if (match.IsNull() && !nf.hash.empty())
        {
            const std::string key = nf.hash + "|" + std::to_string(nf.size);
            auto it = missingByHashSize.find(key);
            if (it != missingByHashSize.end())
            {
                std::vector<GUID> candidates;
                candidates.reserve(it->second.size());
                for (const GUID& g : it->second)
                {
                    if (matchedGuids.find(g.ToString()) == matchedGuids.end())
                        candidates.push_back(g);
                }
                if (candidates.size() == 1)
                {
                    match = candidates[0];
                    ambiguous = false;
                }
                else if (candidates.size() > 1)
                {
                    ambiguous = true;
                }
            }
        }

        // Legacy fallback for large files: match by size + extension.
        // This handles old cache entries without sparse hashes, or rare cases where
        // hash computation failed (I/O errors). New files always have hashes.
        if (match.IsNull() && !ambiguous && nf.hash.empty() && nf.size > kReconcileHashSizeLimit)
        {
            std::filesystem::path nfPath(nf.canonicalRel);
            std::string ext = nfPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string key = std::to_string(nf.size) + "|" + ext;
            auto it = missingBySizeExt.find(key);
            if (it != missingBySizeExt.end())
            {
                std::vector<GUID> candidates;
                candidates.reserve(it->second.size());
                for (const GUID& g : it->second)
                {
                    if (matchedGuids.find(g.ToString()) == matchedGuids.end())
                        candidates.push_back(g);
                }
                if (candidates.size() == 1)
                {
                    match = candidates[0];
                }
                else if (candidates.size() > 1)
                {
                    ambiguous = true;
                }
            }
        }

        // Tier 4, suggestion-only: exactly one missing record and exactly one
        // new file in the same (parent dir, extension) bucket. `match` stays
        // null so no heal follows; the pairing is queued for the cache. The
        // absentee is deliberately NOT added to matchedGuids — it was never
        // consumed, so a later new file in this same run can still claim it on
        // file_id or hash evidence.
        if (match.IsNull() && !ambiguous)
        {
            const std::string dirExtKey = buildDirExtKey(nf.canonicalRel);
            auto missingIt = missingByDirExt.find(dirExtKey);
            auto newCountIt = newFilesByDirExt.find(dirExtKey);
            if (missingIt != missingByDirExt.end() &&
                newCountIt != newFilesByDirExt.end() &&
                newCountIt->second == 1)
            {
                std::vector<GUID> candidates;
                candidates.reserve(missingIt->second.size());
                for (const GUID& g : missingIt->second)
                {
                    if (matchedGuids.find(g.ToString()) == matchedGuids.end())
                        candidates.push_back(g);
                }
                if (candidates.size() == 1)
                {
                    pendingSuggestions.push_back(
                        PendingRenameSuggestion{candidates[0], nf.guid, nf.canonicalRel});
                }
                // candidates.size() > 1 → not even a unique guess; suggest
                // nothing. The ambiguous counter stays reserved for tiers whose
                // evidence WAS strong enough to act on but hit several records;
                // this tier never had a claim to be blocked on.
            }
        }

        if (ambiguous)
        {
            ++outStats.Ambiguous;
            continue;
        }

        if (match.IsNull())
            continue;

        if (derived)
        {
            // Derived heal: the GUID cannot follow the file (it is the path
            // hash), so identity movement is a journaled redirect. Order is
            // load-bearing for interruption safety: (1) kv migration is
            // per-key copy-if-absent — re-entrant, existing keys on the new
            // record win; (2) the redirect commits after the kv copy, so a
            // run cut before it leaves the absentee unmatched and metadata
            // un-stranded; (3) ghost record removal is last — a run cut
            // between (2) and (3) is finished by the completion sweep above.
            //
            // Accepted residual: if a journal flush lands between the scan's
            // registration batch and this heal, and the process dies before
            // any later flush, the heal is lost for good — the candidate is
            // hadExisting next session, so the absentee stays a cache-marked
            // tombstone (as pre-S2) rather than healing. No corruption.
            AssetRecord oldRec{};
            AssetRecord newRec{};
            if (!store.TryGetAsset(match, oldRec) ||
                nf.guid.IsNull() || !store.TryGetAsset(nf.guid, newRec))
            {
                continue;
            }

            for (const auto& [key, value] : oldRec.kv)
            {
                if (newRec.kv.find(key) != newRec.kv.end())
                    continue;
                (void)store.SetKeyValue(nf.guid, key, value, nullptr);
                (void)cache->SetKeyValue(nf.guid, key, value, nullptr);
            }
            // Subasset cascade: identities derived from the container GUID
            // (embedded clips, bridge materials) break with it and have no
            // files of their own for any fingerprint to match. The container's
            // journaled derive keys name every derived identity; re-derive
            // each against both GUIDs and redirect. Emitted before the
            // container redirect: a racing flush cut between the two strands
            // at worst cascade-without-container residue — subasset references
            // still heal (the new container exists) while the container stays
            // a tombstone (the accepted residual above). Note the flush's
            // section order only guarantees redirects-before-deletes; WITHIN
            // the redirect section, lines drain from an unordered set in
            // arbitrary order, so a torn append can keep the container
            // redirect and lose cascade lines — the completion sweep re-emits
            // them from the target record's kv-migrated keys, which precede
            // every redirect in the flush buffer.
            if (auto itKeys = oldRec.kv.find(kSubassetDeriveKeysKvKey); itKeys != oldRec.kv.end())
            {
                for (const std::string& key : SplitSubassetDeriveKeys(itKeys->second))
                {
                    if (store.AddRedirect(GUID::Derive(match, key), GUID::Derive(nf.guid, key), nullptr))
                        ++outStats.SubassetRedirects;
                }
            }
            (void)store.AddRedirect(match, nf.guid, nullptr);
            (void)cache->RedirectProvenance(match, nf.guid, nullptr);
            (void)store.RemoveAsset(match, nullptr);
            (void)cache->RemoveAsset(match, nullptr);
            dirty = true;

            matchedGuids.insert(match.ToString());
            ++outStats.Redirected;
            if (outHeals)
                outHeals->push_back(ReconcileHeal{match, nf.guid});
            Logger::Log::Info(
                "AssetRegistry: offline rename healed via redirect {} -> {} ('{}' -> '{}')",
                match.ToString(), nf.guid.ToString(), oldRec.path, nf.canonicalRel);
            continue;
        }

        AssetRecord rec{};
        if (!store.TryGetAsset(match, rec))
        {
            rec.guid = match;
        }

        rec.path = nf.canonicalRel;
        rec.missing = false;
        (void)store.UpsertAsset(rec, nullptr);
        (void)store.MarkMissing(match, false, nullptr);
        dirty = true;

        // Update cache to reflect the new canonical path and fingerprint.
        (void)cache->UpsertAsset(rec, nullptr);
        (void)cache->UpdateFileFingerprint(match, nf.mtime, nf.size, nf.hash, nf.fileId, nullptr);

        matchedGuids.insert(match.ToString());
        ++outStats.Moved;
    }

    RecordRenameSuggestions(store, *cache, pendingSuggestions, matchedGuids, outStats);

    // Retention runs last: an absentee that healed this session is in
    // matchedGuids and is no longer a candidate, so a ghost is only ever
    // collected after its chance to heal has been taken.
    if (derived &&
        CollectGhostGarbage(store, *cache, retention, missing, matchedGuids, outStats,
                            outCollectedGhosts))
    {
        dirty = true;
    }

    return dirty;
}

} // namespace GameEngine::AssetDatabase
