#include "AssetDatabase/AssetStore_TextJsonl.h"

#include "AssetCore/SharedFileRead.h"

#include "AssetCore/PathNormalization.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/ScopedFileLock.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine::AssetDatabase
{

namespace
{
using ordered_json = nlohmann::ordered_json;

static ordered_json MakeAssetJsonCanonical(const AssetRecord& r)
{
    ordered_json j;
    j["guid"] = r.guid.ToString();
    j["path"] = r.path;
    // "type" is the persisted programmable type id string.
    // For built-in types this typically matches AssetTypeToString(r.type).
    j["type"] = !r.typeId.empty() ? r.typeId : AssetTypeToString(r.type);
    // Omit default fields to keep diffs small.
    if (r.missing)
    {
        j["missing"] = true;
    }

    if (!r.kv.empty())
    {
        ordered_json kv = ordered_json::object();
        std::vector<std::string> keys;
        keys.reserve(r.kv.size());
        for (const auto& kvp : r.kv)
        {
            keys.push_back(kvp.first);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys)
        {
            auto it = r.kv.find(k);
            if (it != r.kv.end())
            {
                kv[k] = it->second;
            }
        }
        j["kv"] = std::move(kv);
    }
    return j;
}

static ordered_json MakeRedirectJsonCanonical(const GUID& from, const GUID& to)
{
    ordered_json j;
    j["redirect_from"] = from.ToString();
    j["redirect_to"] = to.ToString();
    return j;
}

// Phase 3.5 v2: delete-record shapes. Asset delete carries only the guid;
// redirect delete carries an empty redirect_to. Both are replayed by
// LoadFromFile as erase ops.
static ordered_json MakeAssetDeleteJson(const GUID& guid)
{
    ordered_json j;
    j["guid"] = guid.ToString();
    j["deleted"] = true;
    return j;
}

static ordered_json MakeRedirectDeleteJson(const GUID& from)
{
    ordered_json j;
    j["redirect_from"] = from.ToString();
    j["redirect_to"] = "";
    return j;
}

// Header line for v2. First non-comment line of a v2 file.
static ordered_json MakeFormatHeaderJson()
{
    ordered_json j;
    j["format"] = "assetdb";
    j["version"] = 2;
    return j;
}

// Returns the format version declared by the json (2+) or 0 if not a header.
static int TryParseFormatHeader(const ordered_json& j)
{
    auto fmtIt = j.find("format");
    auto verIt = j.find("version");
    if (fmtIt == j.end() || !fmtIt->is_string())
        return 0;
    const std::string& fmt = fmtIt->get_ref<const std::string&>();
    // Accept both the project-DB ("assetdb") and Package-mount manifest
    // ("assetmanifest") format strings — they share the same line-format
    // shape (header + asset/redirect records). Phase 6 generators write
    // "assetmanifest"; the project-mount writer in this same TU still
    // emits "assetdb".
    if (fmt != "assetdb" && fmt != "assetmanifest")
        return 0;
    if (verIt == j.end() || !verIt->is_number_integer())
        return 0;
    return verIt->get<int>();
}

static bool ParseBool(const ordered_json& j, const char* key, bool& out)
{
    auto it = j.find(key);
    if (it == j.end())
        return false;
    if (it->is_boolean())
    {
        out = it->get<bool>();
        return true;
    }
    if (it->is_number_integer())
    {
        out = it->get<int>() != 0;
        return true;
    }
    return false;
}

static AssetType ParseAssetType(const ordered_json& j)
{
    auto it = j.find("type");
    if (it == j.end())
        return AssetType::Unknown;

    if (it->is_string())
    {
        // The writer emits AssetTypeToString(); parse with its exact inverse so
        // every type the store can write reads back as itself. A programmable
        // type id that is not a built-in name stays Unknown here — r.typeId
        // preserves the string for the type registry to resolve.
        return AssetTypeFromString(it->get<std::string>());
    }

    if (it->is_number_integer())
    {
        return static_cast<AssetType>(it->get<int>());
    }

    return AssetType::Unknown;
}

// Per-line parse output. Holds the parsed-but-not-merged classification
// of a single JSONL line so the JSON-parse step can be parallelized
// across chunks while the merge into m_Assets/m_PathToGuid stays
// sequential (preserving last-write-wins ordering).
struct ParsedLine
{
    enum class Kind
    {
        Skipped,        // empty / comment / unrecognized / parse-error
        FormatHeader,
        AssetUpsert,
        AssetDelete,
        RedirectUpsert,
        RedirectDelete,
    };
    Kind kind = Kind::Skipped;
    int formatVersion = 0;        // FormatHeader only
    AssetRecord asset;            // AssetUpsert / AssetDelete (Delete uses guid only)
    GUID redirectFrom;            // Redirect*
    GUID redirectTo;              // RedirectUpsert
    size_t lineNo = 0;
    std::string parseError;       // populated when JSON parse threw
};

// Trim trailing CR/LF and leading whitespace. Returns the trimmed view.
// Does NOT allocate — pure span manipulation on the input bytes.
static std::string_view TrimLineForParse(std::string_view line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    return line.substr(i);
}

// Parse one JSONL line into a ParsedLine. Pure function — no shared
// state, safe to call concurrently from multiple threads.
static ParsedLine ParseJsonlLine(std::string_view rawLine, size_t lineNo)
{
    ParsedLine out;
    out.lineNo = lineNo;

    const std::string_view trimmed = TrimLineForParse(rawLine);
    if (trimmed.empty() || trimmed.front() == '#')
        return out;  // Skipped

    try
    {
        ordered_json j = ordered_json::parse(trimmed.begin(), trimmed.end());

        // Format header (v2+).
        if (int v = TryParseFormatHeader(j); v > 0)
        {
            out.kind = ParsedLine::Kind::FormatHeader;
            out.formatVersion = v;
            return out;
        }

        // Redirect record (upsert or delete via empty redirect_to).
        if (j.contains("redirect_from") && j.contains("redirect_to"))
        {
            GUID from(j["redirect_from"].get<std::string>());
            if (from.IsNull())
                return out;
            const std::string toStr = j["redirect_to"].get<std::string>();
            out.redirectFrom = from;
            if (toStr.empty())
            {
                out.kind = ParsedLine::Kind::RedirectDelete;
                return out;
            }
            GUID to(toStr);
            if (to.IsNull())
                return out;
            out.redirectTo = to;
            out.kind = ParsedLine::Kind::RedirectUpsert;
            return out;
        }

        // Asset delete record (v2): {"guid": ..., "deleted": true}
        bool deletedFlag = false;
        if (j.contains("guid") && ParseBool(j, "deleted", deletedFlag) && deletedFlag)
        {
            GUID g(j["guid"].get<std::string>());
            if (g.IsNull())
                return out;
            out.asset.guid = g;
            out.kind = ParsedLine::Kind::AssetDelete;
            return out;
        }

        // Asset upsert (must have guid + path).
        if (!j.contains("guid") || !j.contains("path"))
            return out;

        AssetRecord r{};
        r.guid = GUID(j["guid"].get<std::string>());
        if (r.guid.IsNull())
            return out;
        r.path = AssetStore_TextJsonl::NormalizeCanonicalPath(j["path"].get<std::string>());
        auto itType = j.find("type");
        if (itType != j.end() && itType->is_string())
            r.typeId = itType->get<std::string>();
        r.type = ParseAssetType(j);
        if (r.typeId.empty())
            r.typeId = AssetTypeToString(r.type);
        (void)ParseBool(j, "missing", r.missing);

        auto itKv = j.find("kv");
        if (itKv != j.end() && itKv->is_object())
        {
            for (auto it = itKv->begin(); it != itKv->end(); ++it)
            {
                if (!it.key().empty() && it.value().is_string())
                    r.kv[it.key()] = it.value().get<std::string>();
            }
        }

        out.asset = std::move(r);
        out.kind = ParsedLine::Kind::AssetUpsert;
        return out;
    }
    catch (const std::exception& e)
    {
        out.parseError = e.what();
        return out;
    }
}

// Sequential merge of one parsed line into the in-memory store. Caller
// holds the AssetStore_TextJsonl mutex. detectedVersion is updated when
// a FormatHeader is seen and gates v1/v2 conflict semantics for
// subsequent records. Both single- and parallel-load paths use this so
// merge ordering is identical between them.
//
// Absolute-path upserts violate the canonical-relative contract and are
// quarantined instead of admitted: they never reach the live maps, so
// they can't hydrate the registry, be re-fingerprinted, or be journaled
// back (no tombstone churn in the on-disk file). Replay stays
// last-write-wins across the quarantine boundary — an absolute upsert
// supersedes an earlier relative row for the same GUID and vice versa,
// and a delete erases from both sides.
//
// Path keys are folded (AssetPaths::FoldStorePathKey), so a case-variant of a
// path already claimed is the SAME key. Replay arbitrates such a collision the
// way runtime writes do (CommitRecordLocked): the later line wins and the
// displaced record is evicted — but it is handed to `displaced` rather than
// dropped, because content may still reference the evicted GUID and the mount
// owes it a redirect.
static void ApplyParsedLine(
    const ParsedLine& pl,
    std::unordered_map<GUID, AssetRecord>& assets,
    std::unordered_map<std::string, GUID>& pathToGuid,
    std::unordered_map<GUID, GUID>& redirects,
    std::unordered_map<GUID, AssetRecord>& quarantined,
    std::unordered_map<GUID, AssetRecord>& displaced,
    std::vector<std::string>& loadConflicts,
    int& detectedVersion,
    const std::filesystem::path& filePath)
{
    if (!pl.parseError.empty())
    {
        Logger::Log::Warning("AssetStore_TextJsonl: failed to parse line {} in '{}': {}",
                             pl.lineNo, filePath.string(), pl.parseError);
        loadConflicts.push_back("Parse error at line " + std::to_string(pl.lineNo) + ": " + pl.parseError);
        return;
    }

    switch (pl.kind)
    {
    case ParsedLine::Kind::Skipped:
        return;

    case ParsedLine::Kind::FormatHeader:
        detectedVersion = pl.formatVersion;
        return;

    case ParsedLine::Kind::RedirectDelete:
        redirects.erase(pl.redirectFrom);
        return;

    case ParsedLine::Kind::RedirectUpsert:
    {
        // v1: each from-guid expected once → conflict on disagreement, keep first.
        // v2: last-write-wins.
        if (detectedVersion <= 1)
        {
            auto it = redirects.find(pl.redirectFrom);
            if (it != redirects.end() && it->second != pl.redirectTo)
            {
                loadConflicts.push_back("Redirect conflict at line " + std::to_string(pl.lineNo) +
                                        ": from " + pl.redirectFrom.ToString() + " maps to multiple targets");
                return;
            }
        }
        redirects[pl.redirectFrom] = pl.redirectTo;
        return;
    }

    case ParsedLine::Kind::AssetDelete:
    {
        quarantined.erase(pl.asset.guid);
        // A tombstone for a displaced GUID retires it for good: the journal
        // already carries whatever redirect a previous mount emitted.
        displaced.erase(pl.asset.guid);
        auto itA = assets.find(pl.asset.guid);
        if (itA != assets.end())
        {
            if (!itA->second.path.empty())
            {
                auto itP = pathToGuid.find(AssetPaths::FoldStorePathKey(itA->second.path));
                if (itP != pathToGuid.end() && itP->second == pl.asset.guid)
                    pathToGuid.erase(itP);
            }
            assets.erase(itA);
        }
        return;
    }

    case ParsedLine::Kind::AssetUpsert:
    {
        const AssetRecord& r = pl.asset;
        if (AssetPaths::IsAbsoluteStorePath(r.path))
        {
            // Contract violation: quarantine, and retire any live row this
            // write supersedes (last-write-wins).
            auto itLive = assets.find(r.guid);
            if (itLive != assets.end())
            {
                if (!itLive->second.path.empty())
                {
                    auto itOld = pathToGuid.find(AssetPaths::FoldStorePathKey(itLive->second.path));
                    if (itOld != pathToGuid.end() && itOld->second == r.guid)
                        pathToGuid.erase(itOld);
                }
                assets.erase(itLive);
            }
            quarantined[r.guid] = r;
            return;
        }
        // A relative upsert supersedes any quarantined row for this GUID
        // (the on-disk shape a mount-time repair appends).
        quarantined.erase(r.guid);
        // This GUID is being written, so it is not a displaced casualty —
        // whatever evicted it earlier in the journal has been superseded.
        displaced.erase(r.guid);
        const std::string pathKey = AssetPaths::FoldStorePathKey(r.path);
        if (detectedVersion >= 2)
        {
            // Last-write-wins. Drop the old path index entry if the key changed.
            auto itGuidV2 = assets.find(r.guid);
            if (itGuidV2 != assets.end() && !itGuidV2->second.path.empty())
            {
                const std::string oldKey = AssetPaths::FoldStorePathKey(itGuidV2->second.path);
                if (oldKey != pathKey)
                {
                    auto itOld = pathToGuid.find(oldKey);
                    if (itOld != pathToGuid.end() && itOld->second == r.guid)
                        pathToGuid.erase(itOld);
                }
            }
            // Path-key uniqueness: evict whoever held this key under another
            // GUID, and report it so the mount can redirect rather than orphan
            // its references.
            if (!pathKey.empty())
            {
                auto itP = pathToGuid.find(pathKey);
                if (itP != pathToGuid.end() && itP->second != r.guid)
                {
                    if (auto itOther = assets.find(itP->second); itOther != assets.end())
                    {
                        displaced[itOther->first] = itOther->second;
                        assets.erase(itOther);
                    }
                }
            }
            assets[r.guid] = r;
            if (!pathKey.empty())
                pathToGuid[pathKey] = r.guid;
            return;
        }

        // v1 conflict-warn semantics: each GUID/path expected once; keep first.
        auto itGuid = assets.find(r.guid);
        if (itGuid != assets.end())
        {
            if (!itGuid->second.path.empty() && !r.path.empty() && itGuid->second.path != r.path)
            {
                loadConflicts.push_back("GUID conflict at line " + std::to_string(pl.lineNo) +
                                        ": " + r.guid.ToString() + " maps to multiple paths ('" +
                                        itGuid->second.path + "' vs '" + r.path + "')");
            }
            for (const auto& kv : r.kv)
            {
                auto itExisting = itGuid->second.kv.find(kv.first);
                if (itExisting == itGuid->second.kv.end())
                    itGuid->second.kv[kv.first] = kv.second;
                else if (itExisting->second != kv.second)
                    loadConflicts.push_back("KV conflict for GUID " + r.guid.ToString() +
                                            " key '" + kv.first + "' at line " + std::to_string(pl.lineNo));
            }
            return;
        }

        if (!pathKey.empty())
        {
            auto itPath = pathToGuid.find(pathKey);
            if (itPath != pathToGuid.end() && itPath->second != r.guid)
            {
                loadConflicts.push_back("Path conflict at line " + std::to_string(pl.lineNo) +
                                        ": '" + r.path + "' maps to multiple GUIDs (" +
                                        itPath->second.ToString() + " vs " + r.guid.ToString() + ")");
                // v1 keeps the first claimant, so this row is the casualty —
                // report it so the mount redirects it onto the survivor.
                displaced[r.guid] = r;
                return;
            }
        }

        assets[r.guid] = r;
        if (!pathKey.empty())
            pathToGuid[pathKey] = r.guid;
        return;
    }
    }
}

} // namespace

std::string AssetStore_TextJsonl::NormalizeCanonicalPath(std::string s)
{
    // Forwards to the AssetCore helper. Kept as a static method for now to
    // avoid churning every call site (registry, reconciler, internal store
    // ops); new code should call AssetPaths::CanonicalizeStorePath directly.
    return AssetPaths::CanonicalizeStorePath(std::move(s));
}

AssetStore_TextJsonl::AssetStore_TextJsonl(JobSystem::WorkStealingThreadPool* parsePool) : m_ParsePool(parsePool)
{
}

bool AssetStore_TextJsonl::LoadFromFile(const std::filesystem::path& filePath, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Assets.clear();
    m_PathToGuid.clear();
    m_Redirects.clear();
    m_RedirectIndexStale = true;
    m_LoadConflicts.clear();
    m_LoadQuarantined.clear();
    m_DisplacedRecords.clear();
    m_DirtyAssets.clear();
    m_DirtyRedirects.clear();
    m_LastWrittenFormatVersion = 0;
    m_LastReadOrWrittenFile = filePath;
    m_LastReadFileHasLoadConflicts = false;
    m_AppendsSinceCompaction = 0;
    m_LastCompactionTime = std::chrono::steady_clock::time_point{};

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec))
    {
        return true; // empty store is ok
    }

    // Read the entire file into a single buffer in one shot. This wins
    // back the per-line allocations std::getline used to do, lets us
    // hand string_views to the JSON parser, and is a precondition for
    // the parallel parse path below.
    String buffer;
    if (!ReadFileTextShared(filePath, buffer))
    {
        if (outError)
            *outError = "Failed to open asset database: " + filePath.string();
        return false;
    }

    // Build a single pass of (begin, end) line offsets. Cheap — one
    // linear scan over the buffer, no allocations beyond the offsets
    // vector itself.
    std::vector<std::pair<size_t, size_t>> lineSpans;
    lineSpans.reserve(buffer.size() / 64);  // rough average line length guess
    {
        size_t lineBegin = 0;
        for (size_t i = 0; i < buffer.size(); ++i)
        {
            if (buffer[i] == '\n')
            {
                lineSpans.emplace_back(lineBegin, i);
                lineBegin = i + 1;
            }
        }
        if (lineBegin < buffer.size())
            lineSpans.emplace_back(lineBegin, buffer.size());
    }

    // F.7: parallelize the JSON-parse stage for large files on the parse pool.
    // ParseJsonlLine is pure, so lines parse concurrently. The merge into
    // m_Assets etc. stays sequential to preserve last-write-wins ordering. The
    // threshold keeps the fork's overhead off small projects.
    constexpr size_t kParallelLineThreshold = 4000;
    int detectedVersion = 1;

    // GUID-keyed during replay so last-write-wins holds across the
    // quarantine boundary; drained into m_LoadQuarantined below.
    std::unordered_map<GUID, AssetRecord> quarantined;

    // Records evicted by a later line claiming their path key. GUID-keyed for
    // the same reason: a record that is displaced and then rewritten later in
    // the journal is not a casualty. Drained into m_DisplacedRecords below.
    std::unordered_map<GUID, AssetRecord> displaced;

    // Journal debt already on disk. Replayed record lines minus the live state
    // they collapse to is what compaction would reclaim, and the threshold has
    // to see it or a session that appends a handful of lines and exits can never
    // reach it — which is how the file grows without bound across sessions.
    size_t journalRecordLines = 0;

    if (!m_ParsePool || lineSpans.size() < kParallelLineThreshold)
    {
        // Single-threaded path: parse + apply each line in order.
        for (size_t idx = 0; idx < lineSpans.size(); ++idx)
        {
            const auto [b, e] = lineSpans[idx];
            const std::string_view raw(buffer.data() + b, e - b);
            const ParsedLine pl = ParseJsonlLine(raw, idx + 1);
            if (pl.kind != ParsedLine::Kind::Skipped && pl.kind != ParsedLine::Kind::FormatHeader)
                ++journalRecordLines;
            ApplyParsedLine(pl, m_Assets, m_PathToGuid, m_Redirects,
                            quarantined, displaced, m_LoadConflicts, detectedVersion, filePath);
        }
    }
    else
    {
        // Parallel path: parse every line into its own slot, then apply the
        // slots in line order on this thread.
        std::vector<ParsedLine> parsed(lineSpans.size());
        JobSystem::ParallelFor(m_ParsePool, lineSpans.size(),
            [&buffer, &lineSpans, &parsed](size_t begin, size_t end) {
                for (size_t idx = begin; idx < end; ++idx)
                {
                    const auto [b, e] = lineSpans[idx];
                    parsed[idx] = ParseJsonlLine(std::string_view(buffer.data() + b, e - b), idx + 1);
                }
            },
            kParallelLineThreshold / 4);

        for (const ParsedLine& pl : parsed)
        {
            if (pl.kind != ParsedLine::Kind::Skipped && pl.kind != ParsedLine::Kind::FormatHeader)
                ++journalRecordLines;
            ApplyParsedLine(pl, m_Assets, m_PathToGuid, m_Redirects,
                            quarantined, displaced, m_LoadConflicts, detectedVersion, filePath);
        }
    }

    m_LoadQuarantined.reserve(quarantined.size());
    for (auto& entry : quarantined)
        m_LoadQuarantined.push_back(std::move(entry.second));

    // Journal-silent, exactly like the quarantine drain: the displaced rows
    // stay on disk until a compaction rewrites them away, so a store whose
    // mount never runs the merge pass (read-only manifest) is left
    // byte-identical. The redirect the merge pass emits is what dirties it.
    m_DisplacedRecords.reserve(displaced.size());
    for (auto& entry : displaced)
        m_DisplacedRecords.push_back(std::move(entry.second));

    m_LastWrittenFormatVersion = detectedVersion;
    m_LastReadFileHasLoadConflicts = !m_LoadConflicts.empty();
    // Debt is what a compaction would reclaim: replayed lines beyond the live
    // state they collapse to. A freshly compacted snapshot replays one line per
    // live record and so carries none.
    const size_t liveRecords = m_Assets.size() + m_Redirects.size();
    m_AppendsSinceCompaction =
        journalRecordLines > liveRecords ? journalRecordLines - liveRecords : 0;
    // Arm the wall-clock trigger from the load. It is guarded on a non-zero
    // last-compaction time, so leaving it default-constructed disables age-driven
    // compaction for the entire lifetime of any process that only ever appends.
    m_LastCompactionTime = std::chrono::steady_clock::now();
    return true;
}

std::vector<std::string> AssetStore_TextJsonl::GetLoadConflicts() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_LoadConflicts;
}

std::vector<AssetRecord> AssetStore_TextJsonl::GetLoadQuarantinedRecords() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_LoadQuarantined;
}

std::vector<AssetRecord> AssetStore_TextJsonl::TakeDisplacedRecords()
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<AssetRecord> out;
    out.swap(m_DisplacedRecords);
    return out;
}

bool AssetStore_TextJsonl::HasLoadConflicts() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return !m_LoadConflicts.empty();
}

void AssetStore_TextJsonl::SetCrossProcessWriteLockFile(std::filesystem::path lockFile)
{
    m_CrossProcessWriteLockFile = std::move(lockFile);
}

// The format version declared by the header of the file at filePath: its first
// line that is neither blank nor a '#' comment. 0 when the file is missing,
// empty or does not start with a format header.
static int ReadOnDiskFormatVersion(const std::filesystem::path& filePath)
{
    std::ifstream in(filePath, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        const std::string_view trimmed = TrimLineForParse(line);
        if (trimmed.empty() || trimmed.front() == '#')
            continue;
        const ordered_json header = ordered_json::parse(trimmed.begin(), trimmed.end(), nullptr, false);
        return header.is_discarded() ? 0 : TryParseFormatHeader(header);
    }
    return 0;
}

namespace
{
// The dirty GUIDs one save drained from a store. Unless the save marks them
// written, the destructor merges them back into the store's sets under its
// mutex, so a save that returns false or throws leaves its changes for the next
// one. merge() keeps an entry a mutation dirtied while the save ran.
class DrainedDirtySets
{
  public:
    DrainedDirtySets(std::mutex& storeMutex, std::unordered_set<GUID>& storeAssets,
                     std::unordered_set<GUID>& storeRedirects)
        : m_StoreMutex(storeMutex), m_StoreAssets(storeAssets), m_StoreRedirects(storeRedirects)
    {
    }
    DrainedDirtySets(const DrainedDirtySets&) = delete;
    DrainedDirtySets& operator=(const DrainedDirtySets&) = delete;

    ~DrainedDirtySets()
    {
        if (m_Written)
            return;
        std::lock_guard<std::mutex> lk(m_StoreMutex);
        m_StoreAssets.merge(Assets);
        m_StoreRedirects.merge(Redirects);
    }

    // Moves the store's sets in. The caller holds the store mutex.
    void Drain()
    {
        Assets.swap(m_StoreAssets);
        Redirects.swap(m_StoreRedirects);
    }

    void MarkWritten() { m_Written = true; }

    std::unordered_set<GUID> Assets;
    std::unordered_set<GUID> Redirects;

  private:
    std::mutex& m_StoreMutex;
    std::unordered_set<GUID>& m_StoreAssets;
    std::unordered_set<GUID>& m_StoreRedirects;
    bool m_Written = false;
};
} // namespace

bool AssetStore_TextJsonl::SaveToFile(const std::filesystem::path& filePath, std::string* outError,
                                      StoreSaveWait wait) const
{
    using Clock = std::chrono::high_resolution_clock;
    auto msElapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    const auto tTotal = Clock::now();

    // Ownership of a store file several processes can write: one writer at a
    // time, and whether the loser waits or retries is the caller's call. The
    // lock is taken BEFORE the dirty drain below, so a save that cannot take it
    // returns with every delta still dirty.
    std::optional<FileSystem::ScopedFileLock> crossProcessLock;
    if (!m_CrossProcessWriteLockFile.empty())
    {
        crossProcessLock.emplace(m_CrossProcessWriteLockFile,
                                 FileSystem::ScopedFileLock::Mode::Exclusive,
                                 wait == StoreSaveWait::WaitForPeers);
        if (!crossProcessLock->IsLocked())
        {
            if (outError)
                *outError = "Another process is writing the asset database: " + filePath.string() + " (" +
                            crossProcessLock->FailureReason() + ")";
            return false;
        }
    }

    // Read-merge-write, which is what the exclusive lock is for. This process
    // has held its whole view in memory since mount and nothing refreshes it —
    // an engine package's manifest has no file watcher and never reconciles —
    // so a compaction that snapshotted that view would rewrite away every row
    // another process committed in the meantime. The file is re-read here,
    // under the lock, and the deltas drained below replay on top of it: this
    // process's dirty set is its intent and wins for the rows it touched,
    // everything else is the file's.
    //
    // Only when the file already exists. A store creating its file has nothing
    // to merge with, and a file that has gone missing must be rewritten whole
    // rather than reduced to this flush's deltas.
    std::optional<AssetStore_TextJsonl> onDisk;
    if (crossProcessLock)
    {
        std::error_code existsError;
        if (std::filesystem::exists(filePath, existsError))
        {
            onDisk.emplace(m_ParsePool);
            std::string readError;
            if (!onDisk->LoadFromFile(filePath, &readError))
            {
                // Nothing has been drained yet, so the caller's retry still
                // holds every delta.
                if (outError)
                    *outError = "Failed to re-read the asset database under its write lock: " +
                                readError;
                return false;
            }
            // A file that parses but holds nothing, while this process holds
            // rows, was emptied by something that is not a store: every store
            // write publishes atomically and none publishes an empty database.
            // Merging against it would adopt the damage and reduce this whole
            // view to the flush below, so refuse and keep the deltas.
            if (onDisk->CountAssets() == 0 && CountAssets() != 0)
            {
                if (outError)
                    *outError = "The asset database re-read under its write lock with no "
                                "records and is damaged: " + filePath.string();
                return false;
            }
        }
    }

    // Appending is only right onto the file this store holds the view of: one
    // it loaded or last wrote as v2, and that still declares v2 now. A store
    // that never read the file (a fresh store publishing over an existing one)
    // would append its deltas to someone else's rows; a file deleted, emptied
    // or replaced under the store (a user, a sync tool, a cache wipe) would be
    // left holding only this flush's deltas. Either takes the compaction
    // branch, which rewrites the store's whole view. Under the cross-process
    // lock the re-read stands for the file, and its header was parsed there.
    int ownVersion = 0;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        ownVersion = m_LastWrittenFormatVersion;
    }
    int onDiskVersion = 0;
    if (onDisk)
        onDiskVersion = onDisk->m_LastWrittenFormatVersion;
    else if (ownVersion >= 2)
        onDiskVersion = ReadOnDiskFormatVersion(filePath);

    // Decide compact-vs-append. The append branch only fires when:
    //   - the file on disk starts with a v2 header
    //   - there are dirty deltas to write
    //   - the journal debt plus this flush stays under the compaction threshold
    //   - the time since the last compaction is under kCompactionMaxAge
    // With no deltas and the v2 file this store last read or wrote, loaded
    // without conflicts, there is nothing to do; otherwise we fall through to a
    // full v2 snapshot rewrite (the same path that handles first-time-write,
    // v1-upgrade, and a missing or replaced file).
    bool canAppend = false;
    size_t deltaCount = 0;
    size_t projectedAppends = 0; // journal debt + dirty-count
    // The file's own journal state when it was re-read above, which is what the
    // append/compact decision must be made against — another process's appends
    // are debt this one has to count toward the threshold.
    int fileVersion = 0;
    bool fileHasLoadConflicts = false;
    std::filesystem::path lastReadOrWrittenFile;
    std::vector<AssetRecord> upsertDeltas;     // assets present in m_Assets at flush time
    std::vector<GUID>        assetDeleteDeltas; // dirty assets absent from m_Assets at flush time
    std::vector<std::pair<GUID, GUID>> redirectUpsertDeltas;
    std::vector<GUID>        redirectDeleteDeltas;
    // What the drain took. Declared before the drain's lock so a throw inside
    // it unlocks first; restored on every exit that does not mark it written.
    DrainedDirtySets drained(m_Mutex, m_DirtyAssets, m_DirtyRedirects);
    {
        std::lock_guard<std::mutex> lk(m_Mutex);

        // Drain dirty sets. Whether we append or compact, dirty tracking
        // resets to "all clean" once the save succeeds — compaction emits the
        // entire live state, so it subsumes any pending deltas.
        drained.Drain();
        for (const GUID& g : drained.Assets)
        {
            auto it = m_Assets.find(g);
            if (it != m_Assets.end())
                upsertDeltas.push_back(it->second);
            else
                assetDeleteDeltas.push_back(g);
        }
        for (const GUID& f : drained.Redirects)
        {
            auto it = m_Redirects.find(f);
            if (it != m_Redirects.end())
                redirectUpsertDeltas.emplace_back(f, it->second);
            else
                redirectDeleteDeltas.push_back(f);
        }

        deltaCount = upsertDeltas.size() + assetDeleteDeltas.size()
                   + redirectUpsertDeltas.size() + redirectDeleteDeltas.size();
        fileVersion = onDiskVersion;
        fileHasLoadConflicts = onDisk ? onDisk->m_LastReadFileHasLoadConflicts
                                      : m_LastReadFileHasLoadConflicts;
        lastReadOrWrittenFile = m_LastReadOrWrittenFile;
        const size_t journalDebt = onDisk ? onDisk->m_AppendsSinceCompaction
                                          : m_AppendsSinceCompaction;
        projectedAppends = journalDebt + deltaCount;

        // Compaction triggers (any one fires → fall through to snapshot path):
        //   1. file is not yet v2 (must upgrade).
        //   2. projected appends would meet/exceed the threshold.
        //   3. wall-clock since last compaction is past kCompactionMaxAge.
        // With nothing dirty there is nothing to append; see below.
        const size_t threshold = m_CompactionThresholdOverride.value_or(kCompactionThreshold);
        const auto maxAge = m_CompactionMaxAgeOverride.value_or(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(kCompactionMaxAge));
        const auto now = std::chrono::steady_clock::now();
        const bool ageExceeded = (m_LastCompactionTime.time_since_epoch().count() != 0)
                              && (now - m_LastCompactionTime >= maxAge);

        if (fileVersion < 2 && m_LastWrittenFormatVersion >= 2)
        {
            Logger::Log::Warning("[AssetStore] '{}' was removed, emptied or replaced after this store "
                                 "last wrote it; rewriting it whole from the {} records held in memory",
                                 filePath.string(), m_Assets.size());
        }
        canAppend = (fileVersion >= 2)
                    && (deltaCount > 0)
                    && (projectedAppends < threshold)
                    && !ageExceeded;
    }

    // Nothing to write: the file this store last read or wrote already holds
    // every row it has. The registry marks its store dirty on paths that change
    // nothing, and a rewrite would still change a tracked file that holds
    // superseded lines or lines another writer formatted, in a session where no
    // asset changed. Only that file, while it exists: a save to any other path,
    // or to a file that went missing or predates v2, writes the file whole. A
    // file re-read under the cross-process lock is the file at this path. A
    // file that loaded with conflicts (git leaves conflict markers in a
    // .assetdb two branches appended to) holds lines that are not rows, and
    // only the rewrite removes them.
    if (deltaCount == 0 && fileVersion >= 2 && !fileHasLoadConflicts)
    {
        std::error_code sameFileError;
        if (onDisk || std::filesystem::equivalent(filePath, lastReadOrWrittenFile, sameFileError))
            return true;
    }

    if (canAppend)
    {
        const auto tAppend = Clock::now();

        // Build the entire delta block in memory first, then write it as
        // a single ofstream::write call. This shrinks the torn-append
        // window (where a mid-append failure could leave the on-disk file
        // with a partially-written last line) from "between any two line
        // writes" to "during the kernel-flush of the single write call".
        // Self-healing on next compaction is still in place; this just
        // reduces how often the heal path is needed.
        std::string buffer;
        // Rough size estimate: typical record ~120 bytes; reserve to avoid
        // mid-build reallocs.
        const size_t totalDeltas = upsertDeltas.size() + assetDeleteDeltas.size()
                                 + redirectUpsertDeltas.size() + redirectDeleteDeltas.size();
        buffer.reserve(totalDeltas * 128);
        // Section order is a crash-prefix invariant, not taste: replay
        // semantics are identical for any order (disjoint keyspaces), but a
        // torn append persists a PREFIX of this buffer. Redirect upserts
        // must precede asset deletes so a heal-carrying flush (new record +
        // redirect old->new + old record delete) can never durably lose the
        // redirect while keeping the ghost's delete — that would resolve
        // the old GUID to nothing, permanently. Losing a suffix here is
        // always recoverable: ghost record + redirect is the interrupted
        // heal shape the reconcile's completion sweep finishes.
        for (const AssetRecord& r : upsertDeltas)
        {
            buffer += MakeAssetJsonCanonical(r).dump();
            buffer += '\n';
        }
        for (const auto& rr : redirectUpsertDeltas)
        {
            buffer += MakeRedirectJsonCanonical(rr.first, rr.second).dump();
            buffer += '\n';
        }
        for (const GUID& g : assetDeleteDeltas)
        {
            buffer += MakeAssetDeleteJson(g).dump();
            buffer += '\n';
        }
        for (const GUID& f : redirectDeleteDeltas)
        {
            buffer += MakeRedirectDeleteJson(f).dump();
            buffer += '\n';
        }

        std::ofstream out(filePath, std::ios::binary | std::ios::app);
        if (!out.is_open())
        {
            if (outError)
                *outError = "Failed to open asset database for append: " + filePath.string();
            return false;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        out.flush();
        if (!out.good())
        {
            if (outError)
                *outError = "Failed while appending to asset database: " + filePath.string();
            return false;
        }
        out.close();
        drained.MarkWritten();

        const double appendMs = msElapsed(tAppend);
        const size_t nDeltas = upsertDeltas.size() + assetDeleteDeltas.size()
                             + redirectUpsertDeltas.size() + redirectDeleteDeltas.size();
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_AppendsSinceCompaction = projectedAppends;
            m_LastWrittenFormatVersion = fileVersion;
            m_LastReadOrWrittenFile = filePath;
        }
        if (appendMs > 50.0 || nDeltas > 100)
        {
            Logger::Log::Debug(
                "[AssetStore] SaveToFile (append) '{}' {} deltas: {:.1f}ms",
                filePath.filename().string(), nDeltas, appendMs);
        }
        return true;
    }

    // Fall through: full v2 snapshot rewrite. Used on first-time-write,
    // v1->v2 upgrade, the compaction triggers, and a save with nothing dirty to
    // any file but the one this store last read or wrote, after that file went
    // missing, or when it loaded with conflicts.
    const auto tSnapshot = Clock::now();
    std::vector<AssetRecord> records;
    std::vector<std::pair<GUID, GUID>> redirects;
    if (onDisk)
    {
        // The merge, not either side alone: the file as another process left
        // it, with this flush's deltas replayed over it. Delta order mirrors
        // the append branch's section order so both write the same sequence.
        for (const AssetRecord& r : upsertDeltas)
            onDisk->UpsertAsset(r, nullptr);
        for (const auto& rr : redirectUpsertDeltas)
            onDisk->AddRedirect(rr.first, rr.second, nullptr);
        for (const GUID& g : assetDeleteDeltas)
            onDisk->RemoveAsset(g, nullptr);
        for (const GUID& f : redirectDeleteDeltas)
            onDisk->RemoveRedirect(f, nullptr);
        records = onDisk->EnumerateAssets();
        const std::vector<RedirectRecord> mergedRedirects = onDisk->EnumerateRedirects();
        redirects.reserve(mergedRedirects.size());
        for (const RedirectRecord& rr : mergedRedirects)
            redirects.emplace_back(rr.from, rr.to);
    }
    else
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        records.reserve(m_Assets.size());
        for (const auto& kv : m_Assets)
            records.push_back(kv.second);
        redirects.reserve(m_Redirects.size());
        for (const auto& kv : m_Redirects)
            redirects.emplace_back(kv.first, kv.second);
    }
    const double snapshotMs = msElapsed(tSnapshot);

    // Ensure directory exists.
    std::error_code ec;
    std::filesystem::create_directories(filePath.parent_path(), ec);

    // Write to temp then replace for atomic-ish updates.
    const std::filesystem::path tmpPath = filePath.string() + ".tmp";

    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        if (outError)
            *outError = "Failed to open temp asset database for write: " + tmpPath.string() + " (" +
                        std::generic_category().message(errno) + ")";
        return false;
    }

    // Comment header. LoadFromFile() will ignore these.
    out << "# GameEngine AssetDatabase.assetdb (authoritative)\n";
    out << "# Format: JSON Lines (one JSON object per line). Lines starting with '#' are comments.\n";
    out << "# v2 (append-only journal + compaction): first non-comment line is a {format,version} record.\n";
    out << "# Asset upsert:    {\"guid\":\"...\",\"path\":\"...\",\"type\":\"Texture\",\"missing\":false,\"kv\":{...}}\n";
    out << "# Asset delete:    {\"guid\":\"...\",\"deleted\":true}\n";
    out << "# Redirect upsert: {\"redirect_from\":\"...\",\"redirect_to\":\"...\"}\n";
    out << "# Redirect delete: {\"redirect_from\":\"...\",\"redirect_to\":\"\"}\n";
    out << "#\n";

    // First non-comment line: v2 format header.
    out << MakeFormatHeaderJson().dump() << "\n";

    // Phase 2: Sort snapshot outside the mutex (no longer blocks RegisterAsset).
    // Use GUID::operator< (raw 16-byte compare) rather than comparing
    // guid.ToString() — the latter allocates two std::strings per comparison
    // and dominates the flush time in Debug builds (~2s for 1000 records).
    const auto tSort = Clock::now();
    std::sort(records.begin(), records.end(),
              [](const AssetRecord& a, const AssetRecord& b)
              { return a.guid < b.guid; });
    std::sort(redirects.begin(), redirects.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });
    const double sortMs = msElapsed(tSort);

    // Phase 3: Serialize + write.
    const auto tWrite = Clock::now();
    for (const AssetRecord& r : records)
    {
        ordered_json j = MakeAssetJsonCanonical(r);
        out << j.dump() << "\n";
    }
    for (const auto& rr : redirects)
    {
        ordered_json j = MakeRedirectJsonCanonical(rr.first, rr.second);
        out << j.dump() << "\n";
    }

    out.flush();
    if (!out.good())
    {
        if (outError)
            *outError = "Failed while writing asset database: " + tmpPath.string();
        return false;
    }
    out.close();
    const double writeMs = msElapsed(tWrite);

    // Phase 4: put the rewritten database in place.
    const auto tPublish = Clock::now();
    const bool published = FileSystem::PublishFile(tmpPath, filePath);
    const double publishMs = msElapsed(tPublish);

    if (!published)
    {
        if (outError)
            *outError = "Failed to replace asset database '" + filePath.string() + "'";
        return false;
    }

    // Successful compaction: the on-disk file is now v2 and the journal is reset.
    drained.MarkWritten();
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_LastWrittenFormatVersion = 2;
        m_LastReadOrWrittenFile = filePath;
        m_LastReadFileHasLoadConflicts = false;
        m_AppendsSinceCompaction = 0;
        m_LastCompactionTime = std::chrono::steady_clock::now();
    }

    const double totalMs = msElapsed(tTotal);
    if (totalMs > 100.0)
    {
        Logger::Log::Info(
            "[AssetStore] SaveToFile (compact) '{}' {} records: {:.1f}ms (snapshot {:.1f}ms, sort {:.1f}ms, write {:.1f}ms, publish {:.1f}ms)",
            filePath.filename().string(), records.size(),
            totalMs, snapshotMs, sortMs, writeMs, publishMs);
    }

    return true;
}

bool AssetStore_TextJsonl::UpsertAsset(const AssetRecord& record, std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return CommitRecordLocked(record, outError) != StoreMergeResult::Failed;
}

StoreMergeResult AssetStore_TextJsonl::MergeObservation(const AssetObservation& observation,
                                                        AssetRecord& outMerged,
                                                        std::string* outError)
{
    std::lock_guard<std::mutex> lk(m_Mutex);

    AssetRecord merged{};
    if (auto it = m_Assets.find(observation.guid); it != m_Assets.end())
    {
        merged = it->second;
    }
    merged.guid = observation.guid;
    if (!observation.path.empty())
    {
        merged.path = observation.path;
    }
    if (!IsRecognizedAssetType(merged.type))
    {
        merged.type = AssetType::Unknown;
    }
    // The observation wins only where it has something to say. A registrar that
    // could not classify the asset reports Unknown; letting that land erases the
    // answer a registrar that could classify it already wrote.
    if (IsClassificationKnown(observation.type, observation.typeId))
    {
        merged.type = observation.type;
        merged.typeId = observation.typeId.empty() ? AssetTypeToString(observation.type)
                                                   : observation.typeId;
    }
    // Observing an asset is seeing its file.
    merged.missing = false;

    const StoreMergeResult result = CommitRecordLocked(merged, outError);
    if (result != StoreMergeResult::Failed)
    {
        if (auto it = m_Assets.find(observation.guid); it != m_Assets.end())
        {
            outMerged = it->second;
        }
    }
    return result;
}

// Requires m_Mutex. Shared by every write that lands a whole record, so the
// path-uniqueness arbitration and the re-journal suppression have one
// definition.
StoreMergeResult AssetStore_TextJsonl::CommitRecordLocked(const AssetRecord& record, std::string* outError)
{
    if (record.guid.IsNull())
        return StoreMergeResult::Failed;

    AssetRecord r = record;
    r.path = NormalizeCanonicalPath(r.path);
    if (AssetPaths::IsAbsoluteStorePath(r.path))
    {
        // Canonical-relative contract: absolute paths never enter the
        // journal. Callers own routing (project-relative vs mount-local);
        // an absolute path reaching the store is a caller bug.
        Logger::Log::Warning("AssetStore_TextJsonl: rejecting absolute-path upsert for {} ('{}')",
                             r.guid.ToString(), r.path);
        if (outError)
            *outError = "absolute path violates the canonical-relative store contract: " + r.path;
        return StoreMergeResult::Failed;
    }
    if (r.typeId.empty())
    {
        r.typeId = AssetTypeToString(r.type);
    }

    // A re-registration that changes nothing must not dirty the record: project
    // rescans re-upsert every unchanged asset, and journaling those appends a
    // byte-identical line per flush, per session, forever.
    if (auto itSame = m_Assets.find(r.guid);
        itSame != m_Assets.end() && itSame->second == r)
    {
        return StoreMergeResult::Unchanged;
    }

    // Identity is the folded key: a re-spelling of the same path updates the
    // row in place instead of forking a second identity for one file.
    const std::string pathKey = AssetPaths::FoldStorePathKey(r.path);

    // Remove old path mapping if changing.
    auto itExisting = m_Assets.find(r.guid);
    if (itExisting != m_Assets.end())
    {
        const std::string oldKey = AssetPaths::FoldStorePathKey(itExisting->second.path);
        if (!oldKey.empty() && oldKey != pathKey)
        {
            auto itP = m_PathToGuid.find(oldKey);
            if (itP != m_PathToGuid.end() && itP->second == r.guid)
            {
                m_PathToGuid.erase(itP);
            }
        }
    }

    // Enforce unique path -> guid mapping.
    if (!pathKey.empty())
    {
        auto itP = m_PathToGuid.find(pathKey);
        if (itP != m_PathToGuid.end() && itP->second != r.guid)
        {
            const GUID other = itP->second;
            // Mirror runtime AssetRegistry behaviour: override the mapping and drop the other record.
            // This prevents SaveToFile() from emitting duplicate path lines with different GUIDs.
            auto itOther = m_Assets.find(other);
            AssetRecord displacedRec;
            if (itOther != m_Assets.end())
            {
                displacedRec = itOther->second;
                m_Assets.erase(itOther);
            }
            m_PathToGuid.erase(itP);
            // The displaced guid needs a delete record on next flush.
            m_DirtyAssets.insert(other);

            // One path key is one file, so the displaced GUID is a second
            // identity for it, whatever spelling either row carries: it is
            // reported, as journal replay reports it, and the mount decides
            // whether a redirect is owed (AssetRegistry::DrainDisplacedIdentities).
            // Spelling is not the signal; the mount's identity scheme is.
            // Reported, not emitted here: the mount owns redirect emission
            // because it also mirrors the derived cache and re-arms the
            // registry's resolve seam.
            if (!displacedRec.guid.IsNull())
                m_DisplacedRecords.push_back(displacedRec);
        }
    }

    m_Assets[r.guid] = r;
    if (!pathKey.empty())
    {
        m_PathToGuid[pathKey] = r.guid;
    }
    m_DirtyAssets.insert(r.guid);
    return StoreMergeResult::Changed;
}

bool AssetStore_TextJsonl::RemoveAsset(const GUID& guid, std::string* outError)
{
    (void)outError;
    if (guid.IsNull())
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end())
        return false;
    if (!it->second.path.empty())
    {
        auto itP = m_PathToGuid.find(AssetPaths::FoldStorePathKey(it->second.path));
        if (itP != m_PathToGuid.end() && itP->second == guid)
            m_PathToGuid.erase(itP);
    }
    m_Assets.erase(it);
    m_DirtyAssets.insert(guid); // delete record on next flush
    return true;
}

bool AssetStore_TextJsonl::MarkMissing(const GUID& guid, bool missing, std::string* outError)
{
    (void)outError;
    if (guid.IsNull())
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end())
        return false;
    // Value-equality no-op: journaling an unchanged flag appends a
    // byte-identical line per flush (same class as the UpsertAsset guard).
    if (it->second.missing == missing)
        return true;
    it->second.missing = missing;
    m_DirtyAssets.insert(guid);
    return true;
}

bool AssetStore_TextJsonl::TryGetAsset(const GUID& guid, AssetRecord& outRecord) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end())
        return false;
    outRecord = it->second;
    return true;
}

std::optional<GUID> AssetStore_TextJsonl::LookupGuidByPath(const std::string& canonicalPath) const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    const std::string key = AssetPaths::FoldStorePathKey(NormalizeCanonicalPath(canonicalPath));
    auto it = m_PathToGuid.find(key);
    if (it == m_PathToGuid.end())
        return std::nullopt;
    return it->second;
}

std::vector<AssetRecord> AssetStore_TextJsonl::EnumerateAssets() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<AssetRecord> out;
    out.reserve(m_Assets.size());
    for (const auto& kv : m_Assets)
        out.push_back(kv.second);
    return out;
}

void AssetStore_TextJsonl::IterateAssets(const std::function<void(const AssetRecord&)>& visit) const
{
    if (!visit)
        return;
    // m_Mutex is not recursive — the IAssetStore contract forbids the
    // visitor from re-entering the store for exactly this reason.
    std::lock_guard<std::mutex> lk(m_Mutex);
    for (const auto& kv : m_Assets)
        visit(kv.second);
}

size_t AssetStore_TextJsonl::CountAssets() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_Assets.size();
}

bool AssetStore_TextJsonl::SetKeyValue(const GUID& guid, const std::string& key, const std::string& value, std::string* outError)
{
    (void)outError;
    if (guid.IsNull() || key.empty())
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end())
        return false;
    // Value-equality no-op: an unchanged key/value must not dirty the record.
    if (auto itKv = it->second.kv.find(key);
        itKv != it->second.kv.end() && itKv->second == value)
    {
        return true;
    }
    it->second.kv[key] = value;
    m_DirtyAssets.insert(guid);
    return true;
}

bool AssetStore_TextJsonl::TryGetKeyValue(const GUID& guid, const std::string& key, std::string& outValue) const
{
    if (guid.IsNull() || key.empty())
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end())
        return false;
    auto it2 = it->second.kv.find(key);
    if (it2 == it->second.kv.end())
        return false;
    outValue = it2->second;
    return true;
}

bool AssetStore_TextJsonl::AddRedirect(const GUID& from, const GUID& to, std::string* outError)
{
    (void)outError;
    if (from.IsNull() || to.IsNull() || from == to)
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    // Value-equality no-op: re-emitting an existing redirect must not dirty
    // the store.
    if (auto itSame = m_Redirects.find(from);
        itSame != m_Redirects.end() && itSame->second == to)
    {
        return true;
    }
    m_Redirects[from] = to;
    m_RedirectIndexStale = true;
    m_DirtyRedirects.insert(from);
    return true;
}

bool AssetStore_TextJsonl::RemoveRedirect(const GUID& from, std::string* outError)
{
    (void)outError;
    if (from.IsNull())
        return false;
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (m_Redirects.erase(from) == 0)
        return false;
    m_RedirectIndexStale = true;
    m_DirtyRedirects.insert(from);
    return true;
}

std::optional<GUID> AssetStore_TextJsonl::ResolveRedirect(const GUID& from) const
{
    if (from.IsNull())
        return std::nullopt;
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Redirects.find(from);
    if (it == m_Redirects.end())
        return std::nullopt;
    return it->second;
}

std::vector<RedirectRecord> AssetStore_TextJsonl::EnumerateRedirects() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<RedirectRecord> out;
    out.reserve(m_Redirects.size());
    for (const auto& kv : m_Redirects)
    {
        out.push_back(RedirectRecord{kv.first, kv.second});
    }
    return out;
}

std::vector<GUID> AssetStore_TextJsonl::FindRedirectSourcesTo(const GUID& target) const
{
    std::vector<GUID> sources;
    if (target.IsNull())
        return sources;
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (m_Redirects.empty())
        return sources;

    if (m_RedirectIndexStale)
    {
        m_RedirectSourcesByTarget.clear();
        for (const auto& [from, to] : m_Redirects)
            m_RedirectSourcesByTarget[to].push_back(from);
        m_RedirectIndexStale = false;
    }

    // Breadth-first over the reverse edges: the GUIDs redirected straight to `target`, then the
    // ones redirected to those, which is every chain head ResolveRedirect chasing would bring to
    // `target`. The visited set ends a cycle and keeps `target` itself out of the result.
    std::unordered_set<GUID> visited{target};
    std::vector<GUID> frontier{target};
    while (!frontier.empty())
    {
        const GUID current = frontier.back();
        frontier.pop_back();
        const auto it = m_RedirectSourcesByTarget.find(current);
        if (it == m_RedirectSourcesByTarget.end())
            continue;
        for (const GUID& source : it->second)
        {
            if (!visited.insert(source).second)
                continue;
            sources.push_back(source);
            frontier.push_back(source);
        }
    }
    return sources;
}

// Test seams (Phase 3.5 step 3) — let tests exercise the compaction path
// without writing 1000+ records. Production code should not call these.

void AssetStore_TextJsonl::SetCompactionThresholdForTesting(std::optional<size_t> threshold)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_CompactionThresholdOverride = threshold;
}

void AssetStore_TextJsonl::SetCompactionMaxAgeForTesting(
    std::optional<std::chrono::steady_clock::duration> maxAge)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_CompactionMaxAgeOverride = maxAge;
}

size_t AssetStore_TextJsonl::GetAppendsSinceCompactionForTesting() const
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_AppendsSinceCompaction;
}

} // namespace GameEngine::AssetDatabase


