#include "Assets/AssetRegistry.h"
#include "FileSystem/FileSystem.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/AsyncRegistryTasks.h"
#include "Assets/AssetDependencyExtractor.h"
#include "FileWatcher/FileIdentity.h"
#include "Assets/ParserRegistry.h"
#include "AssetCore/FoldedPathIndex.h"
#include "AssetCore/PathNormalization.h"
#include "AssetCore/SharedFileRead.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "AssetDatabase/AssetSourceSnapshot.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "AssetDatabase/AssetStoreReconciler.h"
#include "AssetDatabase/RedirectChain.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <regex>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

namespace
{
// Releases a FlushInProgress claim on every exit path. A claim that outlived
// a throwing SaveToFile would stay set forever, and every later wait on it
// would spin.
struct FlushClaimRelease
{
    std::atomic<bool>& Flag;
    ~FlushClaimRelease() { Flag.store(false, std::memory_order_release); }
};

// The source and store must outlive their background save. A timeout cannot
// authorize replacing or destroying either object: the save retains raw
// pointers to them. Preserve the ownership barrier even when a save is slow.
static void WaitForFlushRelease(const std::atomic<bool>& flushInProgress,
                                const std::string& alias,
                                const char* site)
{
    constexpr auto kReportInterval = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(5);
    auto nextReport = std::chrono::steady_clock::now() + kReportInterval;
    while (flushInProgress.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= nextReport)
        {
            Logger::Log::Warning(
                "AssetRegistry::{}: still waiting for source '{}' to finish saving",
                site, alias);
            nextReport = std::chrono::steady_clock::now() + kReportInterval;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

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

// E4: UTF-8 map key for a path that is ALREADY normalized via
// NormalizePathForMap. Pure encoding conversion — no re-normalization, no
// syscalls. m_PathToGuid / m_AssetSourceOwner key on these strings so
// lookups hash a flat UTF-8 buffer instead of a wide fs::path.
static std::string PathMapKey(const std::filesystem::path& normPath)
{
    const std::u8string u8 = normPath.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// Reconstruct the fs::path form of a PathMapKey (UTF-8 → platform encoding).
static std::filesystem::path PathFromMapKey(std::string_view key)
{
    const auto* u8data = reinterpret_cast<const char8_t*>(key.data());
    return std::filesystem::path(u8data, u8data + key.size());
}

// True when `pathKey` denotes a path strictly inside the root whose
// precomputed prefix (PathMapKey(root) + '/') is `rootPrefix`.
static bool PathKeyHasRootPrefix(std::string_view pathKey, std::string_view rootPrefix)
{
    return !rootPrefix.empty() &&
           pathKey.size() > rootPrefix.size() &&
           pathKey.compare(0, rootPrefix.size(), rootPrefix) == 0;
}

// True when two root prefix keys name the same directory or one nests inside
// the other, in either direction.
static bool RootPrefixKeysNest(std::string_view a, std::string_view b)
{
    return a == b || PathKeyHasRootPrefix(a, b) || PathKeyHasRootPrefix(b, a);
}

// The key a source root is indexed under: the on-disk root run through the same
// normalizer as every asset path (NormalizePathForMap), plus the trailing '/'
// that turns a prefix test into a containment test. Compare-only — the root
// itself keeps its on-disk spelling because files are opened from it.
static std::string MakeRootPrefixKey(const std::filesystem::path& root)
{
    return PathMapKey(NormalizePathForMap(root)) + '/';
}

// The overlap policy's second key for a root: the prefix key of the path with
// every symlink resolved, so two spellings of one directory nest under
// RootPrefixKeysNest even when their lexical keys share no prefix. Compare-only
// — files are opened from Root, never from this. Touches the filesystem once
// per registration; falls back to the lexical key when the root cannot be
// resolved.
static std::string MakeCanonicalRootPrefixKey(const std::filesystem::path& root)
{
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(root, ec);
    return MakeRootPrefixKey(ec ? root : resolved);
}

static std::string NormalizeRegistryAssetSourceAlias(std::string_view alias)
{
    size_t begin = 0;
    size_t end = alias.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(alias[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(alias[end - 1])))
        --end;
    std::string out(alias.substr(begin, end - begin));
    return ToLowerAscii(std::move(out));
}

static bool IsValidRegistryAssetSourceAlias(std::string_view alias)
{
    if (alias.empty())
        return false;
    for (char c : alias)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static GUID BuildRegistryAssetSourceNamespaceGuid(std::string_view sourceAlias)
{
    return GUID::Derive(GUID::Null(), String("asset-source-namespace:") + std::string(sourceAlias));
}

// Forward declarations for the hash helpers defined later in this anon
// namespace (ComputePartialHash_Fnv1a64, ComputeSparseHash_Fnv1a64). Lets
// ComputeOrReuseHash live next to the snapshot-related code without a
// large hoist of the existing hash routines.
static std::string ComputePartialHash_Fnv1a64(const std::filesystem::path& path, size_t maxBytes, uint64_t sizeForMix);
static std::string ComputeSparseHash_Fnv1a64(const std::filesystem::path& path, int64_t fileSize);

// Compute the content hash for an asset, reusing a cached snapshot hash
// when the disk fingerprint provably hasn't changed since the snapshot was
// written.
//
// The stat-confirm contract from AssetSourceSnapshot.h is honored here:
// before reusing the cached hash we confirm (Mtime, Size, FileId) all
// match what was just stat'd from disk. If any of those differ — or if
// the cached hash is empty — fall through to the full hash compute.
//
// This is the dominant warm-start savings of Phase 5: RegisterAsset is
// called per-asset on scan, drag-drop, and file-watcher events; each call
// previously rehashed the file unconditionally. With a valid snapshot,
// rehashing is now skipped for unchanged files.
//
// Caller passes the snapshot lookup result already (canonicalRel + the
// pre-resolved entry) instead of the SourceEntry, so this helper has no
// dependency on the registry's private types.
static std::string ComputeOrReuseHash(
    const std::filesystem::path& absPath,
    int64_t currentSize,
    int64_t currentMtime,
    const std::string& currentFileId,
    const std::string& cachedHash,        // empty = none available
    int64_t cachedMtime,
    int64_t cachedSize,
    const std::string& cachedFileId)
{
    if (!cachedHash.empty() &&
        cachedMtime == currentMtime &&
        cachedSize == currentSize &&
        cachedFileId == currentFileId)
    {
        return cachedHash;
    }
    if (currentSize <= (256 * 1024))
    {
        return ComputePartialHash_Fnv1a64(absPath, 64 * 1024, static_cast<uint64_t>(currentSize));
    }
    return ComputeSparseHash_Fnv1a64(absPath, currentSize);
}

// Buffering sink for AssetParser::ExtractDependencies. Collects Emit()'d
// edges into a vector; the caller persists them via
// IAssetDbCache::ReplaceDependencies(GUID, vector<DepEdge>).
//
// One sink per call (per referrer, scoped to one ExtractDependencies
// invocation) — no thread-safety needed beyond what the caller's lock
// discipline already provides.
class BufferingDepEdgeSink final : public DepEdgeSink
{
public:
    void Emit(DepEdge edge) override { m_Edges.push_back(std::move(edge)); }
    std::vector<DepEdge>& Edges() { return m_Edges; }
    const std::vector<DepEdge>& Edges() const { return m_Edges; }

private:
    std::vector<DepEdge> m_Edges;
};

// Try parser-driven format-aware dep extraction.
// Returns:
//   - true if the parser implements ExtractDependencies (it claimed responsibility);
//     `outEdges` contains its emissions (possibly empty if the asset has no outgoing refs).
//   - false if the parser uses the default no-op base impl;
//     the caller should fall back to AssetDependencyExtractor's syntactic scan.
static bool TryExtractParserDepEdges(ParserRegistry* parserRegistry,
                                     const GUID& referrerGuid,
                                     const AssetMetadata& metadata,
                                     std::vector<DepEdge>& outEdges)
{
    outEdges.clear();
    if (!parserRegistry)
        return false;
    auto parser = parserRegistry->FindParser(metadata.Path);
    if (!parser)
        return false;
    BufferingDepEdgeSink sink;
    if (!parser->ExtractDependencies(referrerGuid, metadata, sink))
        return false;
    outEdges = std::move(sink.Edges());
    return true;
}

static std::string BuildRegistrySourceScopedKey(std::string_view sourceAlias, std::string_view canonicalRelativePath)
{
    if (sourceAlias.empty() || canonicalRelativePath.empty())
        return {};
    // Slash-normalize first (the platform-native canonicalRelativePath
    // may use real case on Linux/macOS and lowercase on Windows since
    // NormalizePathForMap is platform-aware).
    const std::string slashNormalized = AssetDatabase::AssetStore_TextJsonl::NormalizeCanonicalPath(
        std::string(sourceAlias) + "/" + std::string(canonicalRelativePath));
    // Apply NFC + Unicode case fold so the same logical asset on any
    // platform produces the same key — this is the input to GUID::Derive
    // for derived-identity sources, and we want cross-platform GUID
    // stability there. m_PathToGuid stays platform-native; the JSONL
    // store's index keys fold too (FoldStorePathKey), matching this.
    return AssetPaths::NormalizeForRegistryKey(slashNormalized);
}

// Deterministic GUID for a path under a derived-identity source. Mirrors the
// expression GetOrCreateAssetGUID / RegisterAsset(path, alias) use so every
// derive site produces an identical GUID. Returns Null on bad inputs so callers
// can fall back to their stored-identity behaviour.
static GUID DeriveSourceScopedGuid(const GUID& namespaceGuid,
                                   std::string_view sourceAlias,
                                   std::string_view canonicalRelativePath)
{
    if (namespaceGuid.IsNull())
        return GUID::Null();
    const std::string key = BuildRegistrySourceScopedKey(sourceAlias, canonicalRelativePath);
    if (key.empty())
        return GUID::Null();
    return GUID::Derive(namespaceGuid, key);
}

// SEAM 4 (stored-identity packages): deterministic fallback identity for a
// file that appears under a MUTABLE stored-identity source with no store row
// — e.g. a file Explorer-copied into an extracted package's Assets/ that has
// not been republished into the package's .assetmanifest yet. Deriving from
// the source-scoped key (instead of GUID::Generate) keeps the identity stable
// across sessions AND across a later republish: PublishPackageAssetManifest
// emits this same derived GUID verbatim, so references authored against the
// fallback remain valid once the file is folded into the published manifest.
// Full decision table: Engine/Include/Assets/Packages/PackageMounts.h.
static GUID MintStoredSourceFallbackGuid(AssetRegistry::SourceEntry& src,
                                         const std::string& canonicalRel)
{
    const GUID derived = DeriveSourceScopedGuid(src.NamespaceGuid, src.Alias, canonicalRel);
    if (derived.IsNull())
        return GUID::Generate();

    // Advisory throttle: one Info per source per session, Trace afterwards —
    // a bulk Explorer copy would otherwise flood the log with one line per file.
    const bool firstThisSession = !src.StoredFallbackLogged.exchange(true, std::memory_order_relaxed);
    const char* republishHint = src.IdentityManifestFile.empty()
        ? ""
        : " Republish the package's .assetmanifest to fold new files into the published identity.";
    if (firstThisSession)
    {
        Logger::Log::Info("AssetRegistry: '{}' is not in source '{}''s stored identity; registered with a "
                          "derived fallback GUID.{} (Further additions this session log at Trace.)",
                          canonicalRel, src.Alias, republishHint);
    }
    else
    {
        Logger::Log::Trace("AssetRegistry: '{}' not in source '{}''s stored identity; derived fallback GUID.{}",
                           canonicalRel, src.Alias, republishHint);
    }
    return derived;
}

// Resolve a source's DB + derived-cache paths from its descriptor (shared by
// RegisterSource and RebindSource). Absolute-izes relative inputs, defaults
// the cache dir next to the DB, and ensures the cache dir exists.
static void ResolveSourceDbPaths(const AssetSourceDesc& source, AssetRegistry::SourceEntry& entry)
{
    std::error_code ec;
    if (source.AuthoritativeDbFile.empty())
    {
        entry.DbFile.clear();
        entry.CacheDbFile.clear();
        return;
    }
    std::filesystem::path dbFile = source.AuthoritativeDbFile;
    if (!dbFile.is_absolute())
        dbFile = std::filesystem::absolute(dbFile, ec).lexically_normal();
    entry.DbFile = dbFile;

    std::filesystem::path cacheDir = source.CacheRoot;
    if (cacheDir.empty())
        cacheDir = dbFile.parent_path();
    if (!cacheDir.is_absolute())
        cacheDir = std::filesystem::absolute(cacheDir, ec).lexically_normal();
    entry.CacheDbFile = cacheDir / "AssetDbCache.sqlite";

    std::filesystem::create_directories(cacheDir, ec);
}

static int64_t FileTimeToInt64(const std::filesystem::file_time_type& t)
{
    return static_cast<int64_t>(t.time_since_epoch().count());
}

static std::filesystem::file_time_type Int64ToFileTime(int64_t t)
{
    using clock = std::filesystem::file_time_type;
    return clock(clock::duration(t));
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

    std::vector<uint8_t> buf;
    buf.resize(maxBytes);
    const int64_t got = in.Read(buf.data(), buf.size());
    for (int64_t i = 0; i < got; ++i)
    {
        fnvStep(buf[static_cast<size_t>(i)]);
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
    std::vector<uint8_t> buf(kSampleSize);
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

        const int64_t got = in.Read(buf.data(), kSampleSize);
        for (int64_t i = 0; i < got; ++i)
        {
            fnvStep(buf[static_cast<size_t>(i)]);
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


} // namespace

AssetDatabase::AssetDbCacheFactory AssetRegistry::s_CacheFactory;

void AssetRegistry::SetAssetDbCacheFactory(AssetDatabase::AssetDbCacheFactory factory)
{
    s_CacheFactory = std::move(factory);
}

AssetRegistry::AssetRegistry() = default;

AssetRegistry::~AssetRegistry()
{
    if (m_Initialized)
    {
        Shutdown();
    }
}

// ------------------------------------------------------------------
// Source lookup helpers
// ------------------------------------------------------------------

AssetRegistry::SourceEntry* AssetRegistry::FindSourceByAlias(std::string_view alias)
{
    const std::string normalized = NormalizeRegistryAssetSourceAlias(alias);
    for (auto& src : m_Sources)
    {
        if (src->Alias == normalized)
            return src.get();
    }
    return nullptr;
}

void AssetRegistry::SetGuidRemapCallback(GuidRemapCallback callback)
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    m_GuidRemapCallback = std::move(callback);
}

void AssetRegistry::RefreshProjectRedirectFlagLocked()
{
    AssetDatabase::IAssetStore* store = ProjectStore();
    m_ProjectHasRedirects.store(store && !store->EnumerateRedirects().empty(),
                                std::memory_order_relaxed);
}

SharedPtr<AssetRegistry::SourceEntry> AssetRegistry::ProjectSourcePinned() const
{
    // Block A.2 pin point. Copy the SharedPtr under shared_lock so the
    // local return value owns its own refcount independent of the
    // registry's m_ProjectSource. After this returns, the caller can
    // release any locks and keep using the SourceEntry — its lifetime is
    // bound to the returned SharedPtr's scope, not the registry's.
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    return m_ProjectSource;
}

SharedPtr<AssetRegistry::SourceEntry> AssetRegistry::PinSourceLocked(const SourceEntry* found) const
{
    if (!found)
        return nullptr;
    for (const auto& src : m_Sources)
    {
        if (src.get() == found)
            return src;
    }
    return nullptr;
}

const AssetRegistry::SourceEntry* AssetRegistry::FindSourceByAlias(std::string_view alias) const
{
    const std::string normalized = NormalizeRegistryAssetSourceAlias(alias);
    for (const auto& src : m_Sources)
    {
        if (src->Alias == normalized)
            return src.get();
    }
    return nullptr;
}

AssetRegistry::SourceEntry* AssetRegistry::FindSourceForPath(const std::filesystem::path& absPath)
{
    if (absPath.empty())
        return nullptr;
    // E4: one normalization of the query path, then a prefix compare per
    // source against the precomputed RootPrefixKey. No syscalls — the old
    // shape ran TryMakeCanonicalRelativePath (2× fs::absolute) per source.
    // m_Sources is sorted by priority descending; first match wins.
    const std::string pathKey = PathMapKey(NormalizePathForMap(absPath));
    for (auto& src : m_Sources)
    {
        if (PathKeyHasRootPrefix(pathKey, src->RootPrefixKey))
            return src.get();
    }
    return nullptr;
}

const AssetRegistry::SourceEntry* AssetRegistry::FindSourceForPath(const std::filesystem::path& absPath) const
{
    return const_cast<AssetRegistry*>(this)->FindSourceForPath(absPath);
}

bool AssetRegistry::TryBuildSourceScopedKeyForPath(const std::filesystem::path& absPath,
                                                    std::string& outKey,
                                                    GUID& outNamespaceGuid,
                                                    std::string* outCanonicalRel) const
{
    outKey.clear();
    outNamespaceGuid = GUID::Null();
    if (outCanonicalRel)
        outCanonicalRel->clear();
    if (absPath.empty())
        return false;

    // E4: prefix compare against the precomputed root keys; the source-
    // relative path is the suffix past the prefix (both sides are
    // NormalizePathForMap-normalized, so the suffix is already canonical).
    const std::string pathKey = PathMapKey(NormalizePathForMap(absPath));
    for (const auto& src : m_Sources)
    {
        if (!src->DerivedIdentity)
            continue;
        if (!PathKeyHasRootPrefix(pathKey, src->RootPrefixKey))
            continue;
        const std::string rel = pathKey.substr(src->RootPrefixKey.size());
        outKey = BuildRegistrySourceScopedKey(src->Alias, rel);
        outNamespaceGuid = src->NamespaceGuid;
        if (outCanonicalRel)
            *outCanonicalRel = rel;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------
// Source store helpers
// ------------------------------------------------------------------

namespace
{
// The warm-start snapshot lives beside the derived cache and shares its
// lifetime: both are safe to delete, and neither is meaningful without the
// other (see SetupSourceStore's fresh-cache handling).
std::filesystem::path SourceSnapshotFilePath(const std::filesystem::path& cacheDbFile)
{
    return cacheDbFile.parent_path() / "watcher.snapshot.bin";
}
} // namespace

bool AssetRegistry::SetupSourceStore(SourceEntry& entry)
{
    if (entry.DbFile.empty())
        return true; // No DB configured; derived-only source.

    std::error_code ec;
    if (!entry.DbFile.parent_path().empty())
    {
        std::filesystem::create_directories(entry.DbFile.parent_path(), ec);
        ec.clear();
    }
    if (!entry.CacheDbFile.parent_path().empty())
    {
        std::filesystem::create_directories(entry.CacheDbFile.parent_path(), ec);
        ec.clear();
    }

    const bool dbExisted = std::filesystem::exists(entry.DbFile, ec);

    auto store = GameEngine::MakeUnique<AssetDatabase::AssetStore_TextJsonl>(m_JobSystem);
    // A store file other processes can reach gets an advisory lock beside it
    // before anything can write through it (see AssetSourceDesc).
    if (entry.StoreIsSharedAcrossProcesses)
        store->SetCrossProcessWriteLockFile(entry.DbFile.string() + ".lock");
    {
        // F.1: time the JSONL load — at 50K records this is suspected to
        // dominate init residual.
        using fclock = std::chrono::high_resolution_clock;
        using fmsd = std::chrono::duration<double, std::milli>;
        const auto fT0 = fclock::now();
        std::string err;
        if (!store->LoadFromFile(entry.DbFile, &err))
        {
            Logger::Log::Error("AssetRegistry: failed to load asset database '{}': {}", entry.DbFile.string(), err);
            return false;
        }
        const auto fT1 = fclock::now();
        const size_t recCount = store->CountAssets();
        if (recCount >= 1000)
        {
            std::fprintf(stderr,
                "[F.1 init-prof] alias=%s jsonl-load=%.1fms records=%zu\n",
                entry.Alias.c_str(), fmsd(fT1 - fT0).count(), recCount);
        }

        const auto conflicts = store->GetLoadConflicts();
        if (!conflicts.empty())
        {
            Logger::Log::Error("AssetRegistry: detected {} conflicts while loading '{}'",
                               conflicts.size(), entry.DbFile.string());
            for (const auto& c : conflicts)
            {
                Logger::Log::Error("  {}", c);
            }
        }
    }

    // Merge the published stored-identity manifest, when configured (mutable
    // stored-identity package mounts — extracted packages). The manifest wins
    // {guid, path} identity conflicts: it is the package's published identity
    // contract, while the local working DB may hold stale pre-publish rows
    // (e.g. derived GUIDs from a session before the package was published).
    // kv/type of an agreeing local row are preserved — the working DB is
    // authoritative for everything except identity. The manifest file itself
    // is never written here; republishing is an explicit caller step.
    bool manifestMergeDirty = false;
    if (!entry.IdentityManifestFile.empty())
    {
        std::error_code manifestEc;
        if (std::filesystem::exists(entry.IdentityManifestFile, manifestEc))
        {
            AssetDatabase::AssetStore_TextJsonl manifest(m_JobSystem);
            std::string err;
            if (!manifest.LoadFromFile(entry.IdentityManifestFile, &err))
            {
                // A configured-but-unreadable identity contract must fail the
                // mount: falling back to derived identity would silently mint
                // different GUIDs and break every stored reference.
                Logger::Log::Error("AssetRegistry: failed to load identity manifest '{}' for source '{}': {}",
                                   entry.IdentityManifestFile.string(), entry.Alias, err);
                return false;
            }

            size_t merged = 0;
            size_t conflicts = 0;
            for (const AssetDatabase::AssetRecord& rec : manifest.EnumerateAssets())
            {
                if (rec.guid.IsNull() || rec.path.empty() || rec.missing)
                    continue;
                const std::optional<GUID> localGuid = store->LookupGuidByPath(rec.path);
                if (localGuid && *localGuid == rec.guid)
                    continue; // local row already carries the published identity
                if (localGuid)
                {
                    // Same path, different GUID: published identity wins. Carry
                    // the local row's kv/type onto the published GUID so
                    // user-authored metadata survives the identity flip.
                    AssetDatabase::AssetRecord localRec{};
                    AssetDatabase::AssetRecord replacement = rec;
                    if (store->TryGetAsset(*localGuid, localRec))
                    {
                        if (replacement.kv.empty())
                            replacement.kv = localRec.kv;
                        if (replacement.type == AssetType::Unknown)
                        {
                            replacement.type = localRec.type;
                            replacement.typeId = localRec.typeId;
                        }
                    }
                    (void)store->RemoveAsset(*localGuid, nullptr);
                    (void)store->UpsertAsset(replacement, nullptr);
                    ++conflicts;
                }
                else
                {
                    (void)store->UpsertAsset(rec, nullptr);
                }
                ++merged;
            }
            if (merged > 0)
            {
                // Applied to entry.StoreDirty AFTER the store move below —
                // the move is followed by an unconditional dirty reset.
                manifestMergeDirty = true;
                Logger::Log::Info("AssetRegistry: merged {} identity records from manifest '{}' into source '{}'",
                                  merged, entry.IdentityManifestFile.string(), entry.Alias);
            }
            if (conflicts > 0)
            {
                Logger::Log::Warning("AssetRegistry: source '{}' had {} local rows whose GUID diverged from "
                                     "the published manifest '{}'; published identity wins",
                                     entry.Alias, conflicts, entry.IdentityManifestFile.string());
            }
        }
        else
        {
            Logger::Log::Warning("AssetRegistry: identity manifest '{}' for source '{}' does not exist; "
                                 "mounting with the local store only",
                                 entry.IdentityManifestFile.string(), entry.Alias);
        }
    }

    // Captured from the concrete store before the IAssetStore move below;
    // feeds the E1 recovery pass once the cache is open.
    const std::vector<AssetDatabase::AssetRecord> quarantinedRecords =
        store->GetLoadQuarantinedRecords();

    // Read-only mounts (e.g. a shipped .assetmanifest package) never write back,
    // so they don't need the derived SQLite cache. Skipping the open means a
    // packaged Player initializes no SQL engine and drops no AssetDbCache.sqlite.
    UniquePtr<AssetDatabase::IAssetDbCache> cache;
    if (dbExisted && !entry.IsReadOnly && s_CacheFactory)
    {
        cache = s_CacheFactory();
        if (cache)
        {
            std::string err;
            if (!cache->Open(entry.CacheDbFile, &err))
            {
                Logger::Log::Warning("AssetDbCache: failed to open '{}': {} (continuing without cache)",
                                     entry.CacheDbFile.string(), err);
                cache.reset();
            }
        }
    }

    entry.Store = std::move(store);
    entry.Cache = std::move(cache);
    // Manifest-merged rows are pending working-DB writes; everything else
    // starts clean.
    entry.StoreDirty.store(manifestMergeDirty, std::memory_order_relaxed);

    // E1 recovery: the JSONL load quarantined absolute-path records
    // (canonical-relative contract violations — macOS and build-dir paths
    // leaked into the project .assetdb by legacy writers). They never
    // entered the live store, so nothing hydrates, re-fingerprints, or
    // journals them; this pass re-admits the ones that resolve under this
    // source's own root (path repaired, GUID + kv preserved) and scrubs
    // the rest from the derived cache. Dropping is journal-silent — the
    // polluted-but-tracked file stays byte-identical instead of gaining a
    // tombstone per row every session.
    if (!quarantinedRecords.empty())
    {
        if (entry.IsReadOnly)
        {
            // Read-only mounts are never rewritten; an absolute path in a
            // shipped manifest is a packaging bug to fix at build time. The
            // quarantined records simply don't resolve on this mount.
            Logger::Log::Warning("AssetRegistry: store for '{}' contains {} absolute-path records; "
                                 "quarantined (read-only mount, file left untouched)",
                                 entry.Alias, quarantinedRecords.size());
        }
        else
        {
            size_t repaired = 0;
            size_t dropped = 0;
            if (AssetDatabase::RecoverQuarantinedStoreRecords(entry.Root, quarantinedRecords,
                                                              *entry.Store, entry.Cache.get(),
                                                              repaired, dropped))
            {
                entry.StoreDirty.store(true, std::memory_order_relaxed);
            }
            Logger::Log::Info("AssetRegistry: quarantined {} leaked absolute-path records from '{}' "
                              "({} repaired to canonical-relative)",
                              dropped, entry.Alias, repaired);
        }
    }

    // Duplicate-identity migration for whatever the journal replay evicted.
    DrainDisplacedIdentities(entry);

    // (Conflict report is now computed on demand by GetAssetDatabaseConflicts —
    // no cached state on the registry.)

    // Phase 5 step 2b: load the warm-start snapshot if one exists. The
    // snapshot was written during the previous Shutdown (Phase 5 step 2a)
    // and lives next to the cache db. Records are stashed in
    // entry.SnapshotByPath so the reconcile / scan path can use them to
    // skip stat'ing files whose fingerprints already match disk.
    //
    // Snapshot misses, missing files, mount-root mismatches, and corrupt
    // headers are all logged at Trace and degrade gracefully — the worst
    // outcome is that this session does a full stat-every-file scan, the
    // same behavior as before Phase 5.
    entry.SnapshotByPath.clear();
    entry.SnapshotDirMtimeByPath.clear();
    if (!entry.CacheDbFile.empty())
    {
        const std::filesystem::path snapshotFile = SourceSnapshotFilePath(entry.CacheDbFile);
        std::error_code snapshotEc;

        // The snapshot and the cache are one unit of derived state, but they
        // are two files, so deleting only AssetDbCache.sqlite -- the documented
        // always-safe unit -- used to leave the snapshot behind. That snapshot
        // then told this session's scan to skip every unchanged file, and a
        // skipped file is never fingerprinted, so the fresh cache could never
        // refill: absentees became permanently unmatchable and the derived
        // indices stayed empty. Nothing self-invalidates it (the load checks
        // only mount root and ignore-rules signature; the cache mutation
        // counter gates snapshot WRITING at shutdown, not its validity), so a
        // rebuilt schema takes the snapshot with it and this session does one
        // honest full scan.
        if (entry.Cache && entry.Cache->WasSchemaReset() &&
            std::filesystem::exists(snapshotFile, snapshotEc))
        {
            std::error_code removeEc;
            std::filesystem::remove(snapshotFile, removeEc);
            Logger::Log::Info(
                "AssetRegistry: source '{}' rebuilt its asset cache, so the warm-start snapshot "
                "was discarded too; this session does a full scan to repopulate derived data",
                entry.Alias);
        }

        if (std::filesystem::exists(snapshotFile, snapshotEc))
        {
            std::filesystem::path snapshotMountRoot;
            uint64_t snapshotIgnoreSig = 0;
            std::vector<AssetDatabase::AssetSourceSnapshotRecord> records;
            std::vector<AssetDatabase::AssetSourceSnapshotDirectory> directories;
            std::string err;
            if (AssetDatabase::AssetSourceSnapshot::Load(snapshotFile, m_JobSystem, snapshotMountRoot,
                                                         snapshotIgnoreSig, records, directories, &err))
            {
                // Belt-and-braces: the format already encodes a hash of the
                // mount root, but a paranoid string compare guards against
                // hash collisions and is essentially free. Compared as keys,
                // not as paths: "is this snapshot for this mount" is an
                // identity question, and a snapshot written by another
                // machine (or an earlier spelling of the root) still names
                // the same mount.
                const bool mountMatches =
                    !snapshotMountRoot.empty() &&
                    AssetPaths::NormalizeForRegistryKey(snapshotMountRoot) ==
                        AssetPaths::NormalizeForRegistryKey(entry.Root);
                // Compare the snapshot's recorded ignore-rules signature
                // against the active rules. A mismatch means .assetignore
                // changed since the snapshot was written — paths that were
                // ignored may now be visible (and vice versa), so cached
                // fingerprints could be stale w.r.t. what scan will index.
                // Treat as invalid; a fresh scan replaces the snapshot.
                const uint64_t currentIgnoreSig = entry.IgnoreRules.Signature();
                const bool ignoreSigMatches = (snapshotIgnoreSig == 0) || (snapshotIgnoreSig == currentIgnoreSig);

                if (!mountMatches)
                {
                    Logger::Log::Trace("AssetRegistry: snapshot for '{}' had mismatched mount root ('{}' vs '{}')",
                                       entry.Alias, snapshotMountRoot.string(), entry.Root.string());
                }
                else if (!ignoreSigMatches)
                {
                    Logger::Log::Trace("AssetRegistry: snapshot for '{}' has stale ignore-rules signature "
                                       "(snapshot={}, current={}); discarding",
                                       entry.Alias, snapshotIgnoreSig, currentIgnoreSig);
                }
                else
                {
                    entry.SnapshotByPath.reserve(records.size());
                    for (auto& r : records)
                    {
                        if (r.CanonicalPath.empty())
                            continue;
                        SnapshotFingerprint fp;
                        fp.Mtime = r.Mtime;
                        fp.Size = r.Size;
                        fp.FileId = std::move(r.FileId);
                        fp.Hash = std::move(r.Hash);
                        entry.SnapshotByPath.emplace(
                            AssetPaths::FoldStorePathKey(r.CanonicalPath), std::move(fp));
                    }
                    entry.SnapshotDirMtimeByPath.reserve(directories.size());
                    for (auto& d : directories)
                    {
                        if (d.CanonicalPath.empty())
                            continue;
                        entry.SnapshotDirMtimeByPath.emplace(std::move(d.CanonicalPath), d.Mtime);
                    }
                    // F.5: capture the cache's mutation count baseline so the
                    // Shutdown writer can detect "nothing changed since load"
                    // and skip the rewrite.
                    if (entry.Cache)
                        entry.SnapshotLoadTimeMutationCount = entry.Cache->GetMutationCount();
                    Logger::Log::Trace("AssetRegistry: loaded snapshot for '{}' ({} records, {} dirs)",
                                       entry.Alias, entry.SnapshotByPath.size(),
                                       entry.SnapshotDirMtimeByPath.size());
                }
            }
            else
            {
                Logger::Log::Trace("AssetRegistry: skipping snapshot for '{}' at '{}': {}",
                                   entry.Alias, snapshotFile.string(), err);
            }
        }
    }

    return true;
}

size_t AssetRegistry::DrainDisplacedIdentities(SourceEntry& entry)
{
    // The store evicts a row when another GUID takes its path key, in any
    // spelling (AssetStore_TextJsonl::UpsertAsset, and journal replay). A
    // journal can hold two rows for one file: a pre-fold pair ("Models/X.gltf"
    // and "models/x.gltf"), or a row from before identity was derived from the
    // path. The evicted GUID other than the scheme's own is a retired identity
    // persisted content may still bind, so it is redirected onto the survivor
    // rather than dropped, cascading through its subasset derive keys.
    //
    // Drained at both the mount and the scan tail because the eviction happens
    // at both: replay sees a journal that already carries the pair, and the
    // scan's registration writes the row that displaces the other.
    if (!entry.HasStore())
        return 0;

    const std::vector<AssetDatabase::AssetRecord> displaced = entry.Store->TakeDisplacedRecords();
    if (displaced.empty())
        return 0;

    if (entry.IsReadOnly)
    {
        // A read-only mount is never rewritten; duplicate rows in a shipped
        // manifest are a packaging bug to fix where the manifest is generated.
        Logger::Log::Warning("AssetRegistry: store for '{}' holds {} duplicate-identity records; "
                             "left unmerged (read-only mount, file untouched)",
                             entry.Alias, displaced.size());
        return 0;
    }

    // A redirect is owed to every displaced identity except the one the
    // mount's scheme gives the path: one path key is one file, so any other
    // GUID that held it is a retired identity for that file. Spelling is not
    // the signal. On a derived-identity source the scheme's GUID is the path's
    // derived GUID, and a displaced row holding it is left to the reconcile,
    // which owns that identity; a stored-identity source has no scheme GUID to
    // compare, so every displaced identity is owed.
    std::vector<AssetDatabase::AssetRecord> owed;
    owed.reserve(displaced.size());
    for (const AssetDatabase::AssetRecord& old : displaced)
    {
        if (entry.DerivedIdentity &&
            old.guid == DeriveSourceScopedGuid(entry.NamespaceGuid, entry.Alias, old.path))
            continue;
        owed.push_back(old);
    }

    size_t redirected = 0;
    size_t subassetRedirects = 0;
    if (AssetDatabase::MergeDisplacedStoreRecords(owed, *entry.Store, entry.Cache.get(),
                                                  redirected, subassetRedirects))
    {
        entry.StoreDirty.store(true, std::memory_order_relaxed);
    }
    if (redirected > 0)
    {
        Logger::Log::Info("AssetRegistry: retired {} duplicate identities in '{}' onto the rows "
                          "that own their paths ({} subasset cascade redirects)",
                          redirected, entry.Alias, subassetRedirects);
    }
    return redirected;
}

void AssetRegistry::ReconcileDerivedSourceAfterScan(
    const std::filesystem::path& scanDirectory,
    const std::vector<AssetDatabase::ReconcileScanNewFile>* scanNewFiles)
{
    if (scanDirectory.empty())
        return;

    // Pin the source whose root IS the scanned directory. Partial
    // (sub-directory) scans match no source root, so these passes only ever
    // run against a full-mount scan — never a partial view of the tree.
    SharedPtr<SourceEntry> scanned;
    {
        const std::filesystem::path normDir = NormalizePathForMap(scanDirectory);
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        for (const auto& src : m_Sources)
        {
            if (src->HasStore() && NormalizePathForMap(src->Root) == normDir)
            {
                scanned = src;
                break;
            }
        }
    }
    if (!scanned)
        return;

    // The mount-time duplicate-identity migration's other half. The scan's
    // registration of a file whose journal row holds another GUID writes the
    // scheme's row over it and evicts the old one; retiring that identity here,
    // before anything resolves against it, is what keeps references to it
    // alive. Runs ahead of the Derived-only reconcile below.
    if (DrainDisplacedIdentities(*scanned) > 0)
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        // ResolveGuid chases the PROJECT store's redirects only, so only the
        // project source arms the seam.
        if (scanned.get() == m_ProjectSource.get())
            m_ProjectHasRedirects.store(true, std::memory_order_relaxed);
    }

    SharedPtr<SourceEntry> pinned;
    if (scanned->DerivedIdentity && !scanned->IsReadOnly && scanned->Cache)
        pinned = scanned;
    if (!pinned)
        return;

    // Missing observations land in the per-machine cache. Rename heals are
    // identity movement and DO write the journaled store: a redirect
    // old→new, migrated kv, and the ghost record's removal.
    AssetDatabase::ReconcileGhostRetention retention{};
    retention.MaxMissingSessions = pinned->GhostRetentionSessions;
    retention.CollectNeverHealable = pinned->CollectNeverHealableGhosts;

    AssetDatabase::ReconcileStats stats;
    std::vector<AssetDatabase::ReconcileHeal> heals;
    std::vector<GUID> collectedGhosts;
    const auto reconT0 = std::chrono::high_resolution_clock::now();
    const bool storeDirty = AssetDatabase::StartupReconcileAssetDatabase(
        pinned->Root, pinned->IgnoreRules, *pinned->Store, pinned->Cache.get(),
        AssetDatabase::ReconcileIdentityScheme::Derived,
        pinned->SnapshotByPath.empty() ? nullptr : &pinned->SnapshotByPath,
        pinned->SnapshotDirMtimeByPath.empty() ? nullptr : &pinned->SnapshotDirMtimeByPath,
        scanNewFiles, retention, stats, &heals, &collectedGhosts);
    const double reconMs =
        std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - reconT0).count();
    pinned->LastReconcileExistenceChecks = stats.ExistenceChecks;

    // F.3a-symmetric split for the derived pass: it runs at the scan tail,
    // outside the mount the init-prof line times, so it prints its own.
    // Threshold mirrors the F.1/F.3a 1000-record gate.
    if (const size_t reconRecords = pinned->Store->CountAssets(); reconRecords >= 1000)
    {
        std::fprintf(stderr,
            "[recon-prof] alias=%s scheme=derived records=%zu reconcile=%.1fms exist-checks=%zu "
            "redirected=%zu heals-completed=%zu subasset-redirects=%zu stale-removed=%zu "
            "ambiguous=%zu suggested=%zu\n",
            pinned->Alias.c_str(), reconRecords, reconMs, stats.ExistenceChecks,
            stats.Redirected, stats.HealsCompleted, stats.SubassetRedirects,
            stats.StaleRedirectsRemoved, stats.Ambiguous, stats.Suggested);
    }

    if (storeDirty)
        pinned->StoreDirty.store(true, std::memory_order_relaxed);

    if (!heals.empty())
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

        // Redirects landed in this source's store; arm the resolve seam.
        // ResolveGuid chases the PROJECT store's redirects only, so only the
        // project source flips the flag.
        if (pinned.get() == m_ProjectSource.get())
            m_ProjectHasRedirects.store(true, std::memory_order_relaxed);

        // Healed ghosts were resident in-memory: PopulateHotCachesFromSource
        // loads every journal record whose (cache-resting) missing flag it
        // cannot see. Drop them so lookups chase the redirect instead of a
        // stale record at a path that no longer exists.
        for (const auto& heal : heals)
        {
            if (const AssetMetadata* meta = FindAssetLocked(heal.From))
            {
                const std::string pathKey = PathMapKey(meta->Path);
                m_AssetSourceOwner.erase(pathKey);
                ErasePathMappingLocked(pathKey);
            }
            UnindexResidentGuidLocked(heal.From);
            if (EraseAssetEntryLocked(heal.From))
                ApplyAssetCountDelta(-1);
        }
    }

    if (!collectedGhosts.empty())
    {
        {
            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
            // A collected ghost's record is gone from the journal, but it was
            // resident: PopulateHotCachesFromSource loads every record whose
            // (cache-resting) missing flag it cannot see. Leaving the entry
            // would keep a lookup resolving a path that has neither a file nor
            // a record behind it.
            for (const GUID& ghost : collectedGhosts)
            {
                if (const AssetMetadata* meta = FindAssetLocked(ghost))
                {
                    const std::string pathKey = PathMapKey(meta->Path);
                    m_AssetSourceOwner.erase(pathKey);
                    ErasePathMappingLocked(pathKey);
                }
                UnindexResidentGuidLocked(ghost);
                if (EraseAssetEntryLocked(ghost))
                    ApplyAssetCountDelta(-1);
            }
        }

        // Outside the lock: UnregisterProvenance takes the writer itself. The
        // reconcile pass already dropped the cache row, so this is the
        // in-memory provenance maps catching up (the cache delete it repeats
        // is a no-op).
        for (const GUID& ghost : collectedGhosts)
            UnregisterProvenance(ghost);
    }

    // Stale-redirect removals may have emptied the table; re-derive the
    // fast-path flag (a removal-only run would otherwise leave it stale-true).
    if (stats.StaleRedirectsRemoved > 0 || stats.SubassetRedirectsRemoved > 0)
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (pinned.get() == m_ProjectSource.get())
            RefreshProjectRedirectFlagLocked();
    }

    if (stats.Redirected > 0 || stats.HealsCompleted > 0)
    {
        Logger::Log::Info(
            "AssetRegistry: derived reconcile on '{}' healed {} offline renames via redirects "
            "({} interrupted heals completed, {} subasset cascades)",
            pinned->Alias, stats.Redirected, stats.HealsCompleted, stats.SubassetRedirects);
    }
    if (stats.Ambiguous > 0)
    {
        Logger::Log::Warning(
            "AssetRegistry: {} ambiguous rename matches during derived reconcile on '{}' — "
            "left as missing",
            stats.Ambiguous, pinned->Alias);
    }
    if (stats.Suggested > 0)
    {
        Logger::Log::Warning(
            "AssetRegistry: {} rename suggestion(s) recorded during derived reconcile on '{}' — "
            "dir+ext evidence only, left unhealed pending confirmation",
            stats.Suggested, pinned->Alias);
    }

    // Ghost surfacing: the count a session would otherwise only discover by
    // opening a missing-assets view. What survives this run is also next
    // start's syscall bill — a standing ghost is exempt from the dir-mtime
    // gate by construction and gets re-checked every session.
    if (stats.MissingObserved > 0)
    {
        const size_t resolved = stats.Redirected + stats.HealsCompleted + stats.GhostsCollected;
        Logger::Log::Info(
            "AssetRegistry: '{}' saw {} missing asset(s) — {} healed, {} collected, {} retained "
            "(still referenced or awaiting a rename decision); {} remain and will be re-checked "
            "next start",
            pinned->Alias, stats.MissingObserved, stats.Redirected + stats.HealsCompleted,
            stats.GhostsCollected, stats.GhostsRetained,
            stats.MissingObserved > resolved ? stats.MissingObserved - resolved : 0);
    }
    if (stats.StaleSuggestionsPurged > 0)
    {
        Logger::Log::Info(
            "AssetRegistry: purged {} rename suggestion(s) on '{}' whose subject is no longer missing",
            stats.StaleSuggestionsPurged, pinned->Alias);
    }
}

bool AssetRegistry::ReconcileSourceStore(SourceEntry& entry)
{
    if (!entry.HasStore() || entry.DerivedIdentity)
        return true; // Only reconcile stored-identity sources.

    // A mount whose manifest IS its store treats that manifest as authoritative,
    // whether or not it accepts metadata writes. Reconcile-against-disk is both
    // contract-violating and meaningless there: the manifest is truth, not the
    // on-disk filesystem, and the prune / legacy-path / rename-detection passes
    // below would rewrite a published package's identity contract from a
    // directory walk. Skipping also avoids setting storeDirty=true permanently
    // (which would make every TickPersistence pass attempt + skip a flush
    // forever).
    if (entry.IsImmutable || entry.IsReadOnly)
        return true;

    bool anyDirty = false;

    // The reconciler operates through IAssetStore — no concrete-store assumptions.
    // Sources that don't need reconciliation are filtered upstream by the
    // HasStore / DerivedIdentity / IsReadOnly checks at the head of this function.
    AssetDatabase::IAssetStore& store = *entry.Store;

    // F.3b: warm-start skip for one-time migration / cleanup helpers.
    // PruneIgnoredStoreRecords + NormalizeLegacyStorePathsIfNeeded each
    // call EnumerateAssets and iterate every record (~75ms each at 50K).
    // When the snapshot was loaded successfully (entry.SnapshotByPath
    // non-empty), the previous session already ran these and persisted
    // a clean store; the ignore-rules signature also matched at snapshot
    // load (else SnapshotByPath would be empty), so no records have
    // become newly ignored. Skip the redundant work.
    //
    // The inner StartupReconcileAssetDatabase below still runs — that's
    // what catches between-session structural changes (moves, deletes,
    // adds in dirty subtrees).
    const bool isWarmStart = !entry.SnapshotByPath.empty();

    if (!isWarmStart)
    {
        // Prune ignored records.
        {
            size_t removed = 0;
            if (AssetDatabase::PruneIgnoredStoreRecords(entry.IgnoreRules, store, entry.Cache.get(), removed))
                anyDirty = true;
            if (removed > 0)
                Logger::Log::Info("AssetRegistry: pruned {} ignored records from '{}'", removed, entry.DbFile.string());
        }

        // Normalize legacy "Assets/..." prefixed paths.
        {
            size_t fixed = 0, conflicts = 0;
            if (AssetDatabase::NormalizeLegacyStorePathsIfNeeded(entry.Root, store, entry.Cache.get(), fixed, conflicts))
                anyDirty = true;
            if (fixed > 0)
                Logger::Log::Info("AssetRegistry: normalized {} legacy paths in '{}'", fixed, entry.DbFile.string());
        }

        // Re-prune after normalization.
        {
            size_t removed = 0;
            if (AssetDatabase::PruneIgnoredStoreRecords(entry.IgnoreRules, store, entry.Cache.get(), removed))
                anyDirty = true;
        }
    }

    // (Absolute-path handling lives in SetupSourceStore — the JSONL load
    // quarantines contract-violating records for every store, including
    // derived-identity sources this function never sees, and the recovery
    // pass is gated on the quarantine being non-empty rather than the
    // warm-start heuristic.)

    // Startup reconciliation (rename detection via fingerprints).
    {
        AssetDatabase::ReconcileStats stats;
        const auto reconT0 = std::chrono::high_resolution_clock::now();
        if (AssetDatabase::StartupReconcileAssetDatabase(entry.Root, entry.IgnoreRules, store,
                                          entry.Cache.get(),
                                          AssetDatabase::ReconcileIdentityScheme::Stored,
                                          entry.SnapshotByPath.empty() ? nullptr : &entry.SnapshotByPath,
                                          entry.SnapshotDirMtimeByPath.empty() ? nullptr : &entry.SnapshotDirMtimeByPath,
                                          /*scanNewFiles*/ nullptr,
                                          // Retention is Derived-only: a stored GUID is
                                          // authoritative and unrecreatable, so collecting
                                          // its record would destroy identity no path can
                                          // mint again.
                                          AssetDatabase::ReconcileGhostRetention{}, stats,
                                          /*outHeals*/ nullptr, /*outCollectedGhosts*/ nullptr))
            anyDirty = true;
        const double reconMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::high_resolution_clock::now() - reconT0)
                                   .count();
        entry.LastReconcileHashesReused = stats.HashesReused;
        entry.LastReconcileExistenceChecks = stats.ExistenceChecks;

        // recon-prof: isolates the StartupReconcile call from the F.3a
        // "reconcile=" split, which spans all of ReconcileSourceStore
        // (prune/normalize/type-fixup included on cold starts). Threshold
        // mirrors the F.1/F.3a 1000-record gate.
        if (const size_t reconRecords = store.CountAssets(); reconRecords >= 1000)
        {
            std::fprintf(stderr,
                "[recon-prof] alias=%s scheme=stored records=%zu reconcile=%.1fms exist-checks=%zu "
                "moved=%zu hashes-reused=%zu ambiguous=%zu suggested=%zu\n",
                entry.Alias.c_str(), reconRecords, reconMs, stats.ExistenceChecks,
                stats.Moved, stats.HashesReused, stats.Ambiguous, stats.Suggested);
        }
        if (stats.Moved > 0)
            Logger::Log::Info("AssetRegistry: reconciled {} moved assets on startup", stats.Moved);
        if (stats.Suggested > 0)
            Logger::Log::Warning(
                "AssetRegistry: {} rename suggestion(s) recorded on '{}' — dir+ext evidence only, "
                "left unhealed pending confirmation",
                stats.Suggested, entry.Alias);
        if (stats.Ambiguous > 0)
            Logger::Log::Warning("AssetRegistry: {} ambiguous move matches during startup reconciliation", stats.Ambiguous);
        if (stats.HashesReused > 0)
            Logger::Log::Info("AssetRegistry: warm-start reuse — skipped hash compute for {} files via snapshot",
                              stats.HashesReused);
    }

    // F.3b: Type fix-up — upgrades Unknown/invalid AssetTypes by parser
    // sniffing or extension inference. Once upgraded and persisted, the
    // store carries the correct type forever; warm-start can skip the
    // 50K-record sweep.
    if (!isWarmStart)
    {
        size_t upgraded = 0;
        const std::vector<AssetDatabase::AssetRecord> records = entry.Store->EnumerateAssets();
        for (const auto& rec : records)
        {
            if (rec.guid.IsNull() || rec.path.empty())
                continue;
            const bool storedValid = IsRecognizedAssetType(rec.type);
            const bool storedUnknown = (!storedValid) || (rec.type == AssetType::Unknown);
            if (!storedUnknown)
                continue;
            const std::string ext = GetCompoundExtensionFromPath(rec.path);
            AssetType inferred = AssetType::Unknown;
            if (m_ParserRegistry)
            {
                const std::filesystem::path abs = NormalizePathForMap(entry.Root / std::filesystem::path(rec.path));
                if (auto parser = m_ParserRegistry->FindParser(abs))
                    inferred = parser->GetAssetType();
            }
            if (inferred == AssetType::Unknown)
                inferred = m_TypeRegistry.GetAssetTypeFromExtension(ext);
            if (inferred == AssetType::Unknown)
                continue;
            const bool allowUpgrade =
                (rec.type == AssetType::Unknown) ||
                (!IsRecognizedAssetType(rec.type)) ||
                ((ToLowerAscii(ext) == ".xml" || ToLowerAscii(ext) == ".uxml" || ToLowerAscii(ext) == ".xaml") &&
                 rec.type == AssetType::XML && inferred == AssetType::UILayout);
            if (allowUpgrade && rec.type != inferred)
            {
                AssetDatabase::AssetRecord updated = rec;
                updated.type = inferred;
                updated.typeId = AssetTypeToString(inferred);
                (void)entry.Store->UpsertAsset(updated, nullptr);
                if (entry.Cache)
                    (void)entry.Cache->UpsertAsset(updated, nullptr);
                ++upgraded;
            }
        }
        if (upgraded > 0)
        {
            Logger::Log::Info("AssetRegistry: upgraded {} asset types from extension inference", upgraded);
            anyDirty = true;
        }
    }

    if (anyDirty)
        entry.StoreDirty.store(true, std::memory_order_relaxed);

    return true;
}

void AssetRegistry::PopulateHotCachesFromSource(SourceEntry& entry, Vector<AssetMetadata>& outRecords)
{
    if (!entry.HasStore())
        return;

    const std::vector<AssetDatabase::AssetRecord> storeRecords = entry.Store->EnumerateAssets();
    outRecords.reserve(outRecords.size() + storeRecords.size());

    // B.2 warm-replay fast path: when the snapshot is loaded and covers
    // this record, use snapshot fields for LastModified+FileSize and skip
    // both the per-record stat and the per-record cache UpsertAsset/
    // SetKeyValue. The cache file persists between sessions so its data
    // is already correct; re-writing it on every Initialize was pure
    // overhead. The slow path still runs for records not in the snapshot
    // (cache-blind first session, partially-stale snapshot).
    const bool haveSnapshot = !entry.SnapshotByPath.empty();

    // Built on the first stored-identity record whose manifest path names no
    // file — see AssetPaths::BuildFoldedPathIndex. A case-insensitive
    // filesystem resolves the folded path itself, so neither the index nor the
    // per-record stat that would trigger it runs there.
    const bool matchFoldedPaths = FileSystem::IsCaseSensitive();
    std::optional<std::unordered_map<std::string, std::filesystem::path>> foldedPaths;

    for (const auto& rec : storeRecords)
    {
        if (rec.guid.IsNull() || rec.path.empty() || rec.missing)
            continue;

        // Snapshot lookup gates the per-record stat / cache write later.
        const SnapshotFingerprint* snapFp = nullptr;
        if (haveSnapshot)
        {
            auto it = entry.SnapshotByPath.find(AssetPaths::FoldStorePathKey(rec.path));
            if (it != entry.SnapshotByPath.end())
                snapFp = &it->second;
        }

        AssetMetadata md{};
        md.Guid = rec.guid;
        // Derived-identity source: identity is a function of the canonical path,
        // not whatever GUID happens to be stored, so re-derive on load — the
        // .assetdb is a pure derived cache. Runs for the project source (now
        // DerivedIdentity=true); skipped for DerivedIdentity=false sources such
        // as the package mount, where the stored guid is loaded verbatim.
        if (entry.DerivedIdentity)
        {
            const GUID derived = DeriveSourceScopedGuid(entry.NamespaceGuid, entry.Alias, rec.path);
            if (!derived.IsNull())
                md.Guid = derived;
        }
        // The record's spelling, which names the asset; md.Path is its map-key form.
        std::filesystem::path spelledPath = entry.Root / std::filesystem::path(rec.path);
        // Stored identity only: a package manifest's paths are folded by the
        // publishing host, so on a case-sensitive filesystem they must be
        // matched back to the real on-disk spelling. A derived-identity source
        // owns its own paths at their real case — a miss there is a genuinely
        // absent file, and must stay one.
        if (matchFoldedPaths && !entry.DerivedIdentity)
        {
            std::error_code caseEc;
            if (!std::filesystem::exists(spelledPath, caseEc))
            {
                if (!foldedPaths)
                    foldedPaths = AssetPaths::BuildFoldedPathIndex(entry.Root);
                auto realIt = foldedPaths->find(ToLowerAscii(
                    std::filesystem::path(rec.path).generic_string()));
                if (realIt != foldedPaths->end())
                    spelledPath = realIt->second;
            }
        }
        md.Path = NormalizePathForMap(spelledPath);
        if (snapFp)
        {
            md.LastModified = Int64ToFileTime(snapFp->Mtime);
            md.FileSize = static_cast<size_t>(snapFp->Size);
        }

        md.Name = spelledPath.stem().string();
        md.Extension = GetCompoundExtensionFromPath(md.Path.string());
        md.Type = (IsRecognizedAssetType(rec.type) && rec.type != AssetType::Unknown)
                      ? rec.type
                      : m_TypeRegistry.GetAssetTypeFromExtension(md.Extension);
        md.TypeId = !rec.typeId.empty() ? rec.typeId : AssetTypeToString(md.Type);

        const bool snapshotCovered = (snapFp != nullptr);

        if (!snapshotCovered)
        {
            std::error_code ec;
            if (std::filesystem::exists(md.Path, ec) && std::filesystem::is_regular_file(md.Path, ec))
            {
                std::error_code ec2;
                md.LastModified = std::filesystem::last_write_time(md.Path, ec2);
                std::error_code ec3;
                md.FileSize = std::filesystem::file_size(md.Path, ec3);
            }
        }

        // Warm derived cache. Skipped on the snapshot-covered fast path:
        // the cache file was written at the previous Shutdown and persists
        // between sessions, so its rows already reflect the same record.
        if (entry.Cache && !snapshotCovered)
        {
            AssetDatabase::AssetRecord cacheRec = rec;
            // Key the warm cache by the resolved identity (== rec.guid for
            // stored-identity sources, == the re-derived GUID for derived ones)
            // so the derived cache and the hot caches agree.
            cacheRec.guid = md.Guid;
            cacheRec.type = md.Type;
            cacheRec.typeId = md.TypeId;
            // Mirror, not upsert: this loop reads the journal, and the journal
            // has no answer about this machine's disk. Under derived identity
            // a ghost's journal flag is false by design (§5-A), so writing it
            // would clear the cache tombstone that IS the ghost — and the
            // dir-mtime gate, which only re-checks records believed present,
            // would then keep the existence loop from ever re-marking it.
            (void)entry.Cache->MirrorJournalRecord(cacheRec, nullptr);
            for (const auto& kv : rec.kv)
            {
                (void)entry.Cache->SetKeyValue(md.Guid, kv.first, kv.second, nullptr);
            }
        }
        // E3: stage instead of writing the registry maps — the caller
        // splices under a short writer section (RegisterSource) or commits
        // in place under its already-held lock (RebindSource).
        outRecords.push_back(std::move(md));
    }
}

// ------------------------------------------------------------------
// Infrastructure-only Initialize
// ------------------------------------------------------------------

bool AssetRegistry::Initialize(JobSystem::WorkStealingThreadPool* jobSystem)
{
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (m_Initialized)
        {
            Logger::Log::Warning("AssetRegistry already initialized");
            return true;
        }
    }

    Logger::Log::Info("Initializing Asset Registry (infrastructure) with {} job system",
                      jobSystem ? "async" : "sync");

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        m_JobSystem = jobSystem;

        if (!m_TypeRegistry.Initialize())
        {
            Logger::Log::Error("Failed to initialize asset type registry");
            return false;
        }

        if (m_JobSystem)
        {
            m_AsyncCoordinator = GameEngine::MakeUnique<AsyncRegistryCoordinator>(*this, *m_JobSystem);
        }

        m_Initialized = true;
    }

    Logger::Log::Info("Asset Registry infrastructure initialized");
    return true;
}

// ------------------------------------------------------------------
// RegisterSource
// ------------------------------------------------------------------

bool AssetRegistry::RegisterSource(const AssetSourceDesc& source)
{
    ForgetSubassetContainers();
    const std::string normalizedAlias = NormalizeRegistryAssetSourceAlias(source.Alias);
    if (normalizedAlias.empty() || !IsValidRegistryAssetSourceAlias(normalizedAlias))
    {
        Logger::Log::Warning("AssetRegistry::RegisterSource: invalid alias '{}'", source.Alias);
        return false;
    }

    std::error_code ec;
    // The root is a path the registry OPENS files from, so it keeps the on-disk
    // spelling; only its key (RootPrefixKey, below) is folded.
    const std::filesystem::path root = AssetPaths::NormalizeMountRoot(source.Root);
    if (root.empty())
    {
        Logger::Log::Warning("AssetRegistry::RegisterSource: empty root for alias '{}'", normalizedAlias);
        return false;
    }
    const std::string rootPrefixKey = MakeRootPrefixKey(root);

    // Ensure root directory exists.
    if (!std::filesystem::exists(root, ec))
    {
        ec.clear();
        const bool created = std::filesystem::create_directories(root, ec);
        if (ec)
        {
            Logger::Log::Warning("AssetRegistry::RegisterSource: could not create source directory '{}' for alias '{}': {}",
                                 root.string(), normalizedAlias, ec.message());
        }
        else if (created)
        {
            Logger::Log::Info("AssetRegistry: created source directory: {}", root.string());
        }
    }

    // Validate DerivedIdentity=false requires DB.
    if (!source.DerivedIdentity && source.AuthoritativeDbFile.empty())
    {
        Logger::Log::Error("AssetRegistry::RegisterSource: DerivedIdentity=false requires AuthoritativeDbFile for alias '{}'",
                           normalizedAlias);
        return false;
    }

    // Resolved after the directory exists so the whole path canonicalizes.
    const std::string canonicalRootPrefixKey = MakeCanonicalRootPrefixKey(root);

    // E3: the writer lock used to be held across JSONL load + reconcile +
    // populate (~0.5-1.5s registry freeze per 100k-record mount). Now the
    // alias is RESERVED under a short writer section, the heavy staging
    // work runs off-lock into local structures, and a second short writer
    // section splices the result in.
    //
    // Concurrency semantics during the staging window: the source is not
    // yet visible, so registrations targeting paths under the new root
    // resolve against the pre-mount source set (exactly as they did before
    // the mount started). At splice time the staged records win —
    // CommitMetadataToMapsLocked evicts conflicting path mappings — and
    // the post-mount startup scan reconciles anything else.
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

        // Reject duplicate alias (registered or mid-staging).
        for (const auto& existing : m_Sources)
        {
            if (existing->Alias == normalizedAlias)
            {
                Logger::Log::Warning("AssetRegistry::RegisterSource: alias '{}' is already registered", normalizedAlias);
                return false;
            }
        }
        // E5 overlap policy: sources flagged RejectOverlappingRoots (all
        // package mounts) refuse to nest with any existing source root, in
        // either direction. Roots nest as spelled (the keys every path map
        // and FindSourceForPath use) or as resolved through symlinks (a root
        // reached through a link that lands inside another root). The legacy
        // project/editor same-root overlap keeps working — both sides
        // default to false.
        for (const auto& existing : m_Sources)
        {
            // An entry without its resolved key would silently opt out of the
            // symlink half of the check: PathKeyHasRootPrefix never matches an
            // empty prefix.
            assert(!existing->CanonicalRootPrefixKey.empty() &&
                   "every SourceEntry publishes CanonicalRootPrefixKey beside RootPrefixKey");
            const bool nestAsSpelled = RootPrefixKeysNest(rootPrefixKey, existing->RootPrefixKey);
            const bool nestResolved =
                RootPrefixKeysNest(canonicalRootPrefixKey, existing->CanonicalRootPrefixKey);
            if (!(nestAsSpelled || nestResolved) ||
                !(source.RejectOverlappingRoots || existing->RejectOverlappingRoots))
                continue;
            if (nestAsSpelled)
            {
                Logger::Log::Error(
                    "AssetRegistry::RegisterSource: root '{}' of source '{}' overlaps existing "
                    "source '{}' at '{}' and one of them rejects overlapping roots",
                    root.string(), normalizedAlias, existing->Alias, existing->Root.string());
            }
            else
            {
                Logger::Log::Error(
                    "AssetRegistry::RegisterSource: root '{}' of source '{}' resolves through a "
                    "symlink to '{}', which overlaps existing source '{}' at '{}' (resolves to '{}'), "
                    "and one of them rejects overlapping roots; mount a directory outside every "
                    "registered root",
                    root.string(), normalizedAlias, canonicalRootPrefixKey, existing->Alias,
                    existing->Root.string(), existing->CanonicalRootPrefixKey);
            }
            return false;
        }

        if (!m_PendingSourceAliases.insert(normalizedAlias).second)
        {
            Logger::Log::Warning("AssetRegistry::RegisterSource: alias '{}' is already being registered", normalizedAlias);
            return false;
        }
    }

    {
        // Build SourceEntry. Block A.2: SharedPtr so callers can pin
        // lifetime across lock-release windows via ProjectSourcePinned().
        auto entry = std::make_shared<SourceEntry>();
        entry->Alias = normalizedAlias;
        entry->Root = root;
        entry->RootPrefixKey = rootPrefixKey;
        entry->CanonicalRootPrefixKey = canonicalRootPrefixKey;
        entry->NamespaceGuid = BuildRegistryAssetSourceNamespaceGuid(normalizedAlias);
        entry->Priority = source.Priority;
        entry->DerivedIdentity = source.DerivedIdentity;
        // Copy the lifecycle flags onto the entry; each is wired to runtime
        // behaviour as documented on the AssetSourceDesc fields (the package
        // mount uses them for the read-only/immutable packaged-Player shape).
        // (RegisterFileWatcher is intentionally NOT copied — the registry
        // doesn't act on it; AssetManager reads it directly from the desc.)
        entry->RequiresScan = source.RequiresScan;
        entry->IsImmutable = source.IsImmutable;
        entry->AcceptsMetadataWrites = source.AcceptsMetadataWrites;
        entry->StoreIsSharedAcrossProcesses = source.StoreIsSharedAcrossProcesses;
        entry->InfersTextureImportSettings = source.InfersTextureImportSettings;
        entry->CooksDerivedArtifactsOnMiss = source.CooksDerivedArtifactsOnMiss;
        entry->IsReadOnly = source.IsReadOnly;
        entry->TracksTombstones = source.TracksTombstones;
        entry->AggressiveTombstoneCleanup = source.AggressiveTombstoneCleanup;
        entry->RejectOverlappingRoots = source.RejectOverlappingRoots;
        entry->GhostRetentionSessions = source.GhostRetentionSessions;
        entry->CollectNeverHealableGhosts = source.CollectNeverHealableGhosts;

        // Compute DB paths.
        ResolveSourceDbPaths(source, *entry);
        if (!source.IdentityManifestFile.empty())
        {
            std::filesystem::path manifestFile = source.IdentityManifestFile;
            if (!manifestFile.is_absolute())
                manifestFile = std::filesystem::absolute(manifestFile, ec).lexically_normal();
            entry->IdentityManifestFile = manifestFile;
        }

        // Load ignore rules.
        entry->IgnoreRules = AssetIgnoreRules::LoadForAssetRoot(root);

        // F.3a: time the three init helpers separately so we can attack
        // whichever dominates 50K's residual ~778ms.
        using fclock = std::chrono::high_resolution_clock;
        using fmsd = std::chrono::duration<double, std::milli>;
        const auto fT0 = fclock::now();

        // E3 staging, all OFF the registry lock: load DB, reconcile, and
        // build the hot-cache records into a local vector. GUID derives use
        // the new entry's own namespace — self-contained.
        const bool setupOk = SetupSourceStore(*entry);
        const auto fT1 = fclock::now();

        if (setupOk && !entry->DerivedIdentity && entry->HasStore())
        {
            ReconcileSourceStore(*entry);
        }
        const auto fT2 = fclock::now();

        Vector<AssetMetadata> stagedRecords;
        if (setupOk)
        {
            PopulateHotCachesFromSource(*entry, stagedRecords);
        }
        const auto fT3 = fclock::now();

        // F.3a: log only when there's enough work to be interesting.
        // Threshold mirrors the F.1 instrumentation's 1000-record gate.
        if (setupOk && entry->Store)
        {
            const size_t recCount = entry->Store->CountAssets();
            if (recCount >= 1000)
            {
                std::fprintf(stderr,
                    "[F.3a init-prof] alias=%s records=%zu setup-store=%.1fms reconcile=%.1fms populate=%.1fms\n",
                    normalizedAlias.c_str(), recCount,
                    fmsd(fT1 - fT0).count(),
                    fmsd(fT2 - fT1).count(),
                    fmsd(fT3 - fT2).count());
            }
        }

        // S10: build the mount overlay OFF-lock — the source's complete
        // record set in its own maps — so the writer section below only has
        // to publish a pointer. Readers stall for O(1) instead of O(records).
        const size_t stagedCount = stagedRecords.size();
        auto overlay = MakeUnique<MountOverlay>();
        overlay->SourceAlias = normalizedAlias;
        overlay->Assets.reserve(stagedCount);
        overlay->PathToGuid.reserve(stagedCount);
        for (AssetMetadata& md : stagedRecords)
        {
            // Duplicate paths within one store keep last-wins order, matching
            // the order the old splice committed them.
            overlay->PathToGuid[PathMapKey(md.Path)] = md.Guid;
            overlay->Assets[md.Guid] = std::move(md);
        }
        stagedRecords.clear();

        // Publish under one short writer section: the source entry and its
        // full record set become visible to readers ATOMICALLY here.
        double publishMs = 0.0;
        {
            const auto sT0 = fclock::now();
            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
            m_PendingSourceAliases.erase(normalizedAlias);

            if (!setupOk)
            {
                Logger::Log::Error("AssetRegistry::RegisterSource: failed to set up store for alias '{}'", normalizedAlias);
                return false;
            }
            if (!m_Initialized)
            {
                // Shutdown raced the staging window; drop everything.
                return false;
            }

            if (!overlay->Assets.empty())
            {
                // S12: published overlay records enter the counted union.
                ApplyAssetCountDelta(static_cast<ptrdiff_t>(overlay->Assets.size()));
                m_MountOverlays.push_back(std::move(overlay));
            }

            // Insert sorted by priority descending.
            auto insertPos = std::find_if(m_Sources.begin(), m_Sources.end(),
                                           [&](const SharedPtr<SourceEntry>& s) { return s->Priority < source.Priority; });
            m_Sources.insert(insertPos, entry);

            // Update project source cache.
            if (normalizedAlias == "project")
            {
                m_ProjectSource = entry;
                RefreshProjectRedirectFlagLocked();
            }
            publishMs = fmsd(fclock::now() - sT0).count();
        }

        // Drain the overlay into the global maps in time-boxed writer
        // sections. Complete before this function returns, so post-return
        // state is exactly the pre-S10 splice's; readers interleave between
        // sections instead of stalling for the whole commit.
        const auto dT0 = fclock::now();
        const DrainStats drainStats = DrainMountOverlay(normalizedAlias);
        const double drainMs = fmsd(fclock::now() - dT0).count();

        if (stagedCount >= 1000)
        {
            std::fprintf(stderr,
                         "[S10 splice-prof] alias=%s records=%zu publish=%.2fms drain=%.2fms "
                         "sections=%zu max-section=%.2fms sections>1ms=%zu\n",
                         normalizedAlias.c_str(), stagedCount, publishMs, drainMs,
                         drainStats.Sections, drainStats.MaxSectionMs, drainStats.SectionsOverMs);
        }
    }

    // If the DB didn't exist yet, write initial file. SetupSourceStore decided
    // not to open the SQLite cache for the same reason (the .assetdb wasn't
    // there at SetupSourceStore time). Now that the file exists, open the
    // cache too — otherwise the first session pays a "cache-blind" penalty
    // where every UpsertAsset / UpdateFileFingerprint / ReplaceDependencies
    // call silently no-ops, including the dep-graph edges parsers emit.
    //
    // This must happen BEFORE the startup scan is submitted: the scan's
    // RegistryUpdateTask reads entry->Cache when it persists fingerprints,
    // and on a fresh project a scan that outruns the lazy open registers
    // every found asset with no fingerprint at all — the warm-start
    // snapshot written at Shutdown then comes out empty and the next
    // session pays a full cold rehash.
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        auto* entry = FindSourceByAlias(normalizedAlias);
        // F.1: skip the write-initial-DB path for read-only sources
        // (Package mount). They're loaded read-only from a manifest;
        // creating the file when missing would clobber the contract.
        if (entry && entry->HasStore() && !entry->DerivedIdentity && !entry->IsReadOnly)
        {
            std::error_code ec2;
            if (!std::filesystem::exists(entry->DbFile, ec2))
            {
                (void)entry->Store->SaveToFile(entry->DbFile, nullptr);
            }
            // Lazily open the cache now that the .assetdb is present, even on
            // first-session/fresh-project flows.
            if (!entry->Cache && !entry->CacheDbFile.empty() && s_CacheFactory)
            {
                auto cache = s_CacheFactory();
                if (cache)
                {
                    std::string err;
                    if (cache->Open(entry->CacheDbFile, &err) && cache->EnsureSchema(&err))
                    {
                        entry->Cache = std::move(cache);
                    }
                    else if (!err.empty())
                    {
                        Logger::Log::Warning("AssetDbCache: lazy open of '{}' failed: {} (continuing without cache)",
                                             entry->CacheDbFile.string(), err);
                    }
                }
            }
        }
    }

    // Start directory scan (outside lock).
    // F.1: skip the auto-scan for sources with RequiresScan=false (e.g.
    // Package mounts, which enumerate from a manifest rather than walking
    // the filesystem).
    if (!source.RequiresScan)
    {
        Logger::Log::Trace("AssetRegistry: source '{}' has RequiresScan=false; skipping startup scan",
                           normalizedAlias);
    }
    else if (m_JobSystem)
    {
        auto scanFuture = ScanDirectoryAsync(root, true);
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        auto* entry = FindSourceByAlias(normalizedAlias);
        if (entry)
            entry->StartupScanFuture = std::move(scanFuture);
    }
    else
    {
        ScanDirectory(root, true);
    }

    Logger::Log::Info("AssetRegistry: mounted source '{}' at '{}'", normalizedAlias, root.string());
    return true;
}

AssetRegistry::DrainStats AssetRegistry::DrainMountOverlay(std::string_view alias)
{
    // Bound each writer section so the worst reader stall a drain can cause
    // is the budget plus one record's commit (including any shard-bounded
    // map-growth event), independent of how many records the mount staged.
    // 300µs leaves headroom under the ~1ms bar for scheduling noise.
    constexpr auto kSpliceDrainBudget = std::chrono::microseconds(300);

    DrainStats stats;
    for (;;)
    {
        {
            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
            if (!m_Initialized)
                return stats; // Shutdown discarded the overlays.

            // Re-find each section: UnregisterSource/RebindSource may have
            // discarded the overlay (and the vector may have reallocated)
            // between sections.
            MountOverlay* overlay = nullptr;
            for (const auto& candidate : m_MountOverlays)
            {
                if (candidate->SourceAlias == alias)
                {
                    overlay = candidate.get();
                    break;
                }
            }
            if (!overlay)
                return stats;

            const auto sectionStart = std::chrono::steady_clock::now();
            const auto deadline = sectionStart + kSpliceDrainBudget;
            ptrdiff_t sectionDelta = 0;
            while (!overlay->Assets.empty())
            {
                auto it = overlay->Assets.begin();
                AssetMetadata md = std::move(it->second);
                const GUID guid = it->first;
                const std::string mdKey = PathMapKey(md.Path);
                if (auto pIt = overlay->PathToGuid.find(mdKey);
                    pIt != overlay->PathToGuid.end() && pIt->second == guid)
                {
                    overlay->PathToGuid.erase(pIt);
                }
                overlay->Assets.erase(guid);
                // Republishes store-loaded records, so no
                // RetireStaleAliasesOnRegistrationLocked here: an interrupted
                // heal's ghost record still carries the redirect the completion
                // sweep finishes the heal with (InterruptedHealIsCompletedNextSession).
                sectionDelta += CommitToGlobalMapsLocked(std::move(md)) - 1; // -1: left the overlay
                if (!m_PathClaims.empty())
                    m_PathClaims.erase(mdKey);
                if (std::chrono::steady_clock::now() >= deadline)
                    break;
            }
            ApplyAssetCountDelta(sectionDelta);

            ++stats.Sections;
            const double sectionMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sectionStart)
                    .count();
            stats.MaxSectionMs = std::max(stats.MaxSectionMs, sectionMs);
            if (sectionMs > 1.0)
                ++stats.SectionsOverMs;

            if (overlay->Assets.empty())
            {
                // Residual PathToGuid entries (bindings whose asset was
                // erased mid-window) carry no backing record; drop them with
                // the overlay.
                DiscardOverlayForAliasLocked(alias);
                return stats;
            }
        }
        // Lock released: give readers a guaranteed window before the next
        // section. A single bare yield is NOT enough — SRWLOCK blocks new
        // shared acquisitions while a writer waits, and this loop
        // re-requesting the writer lock back-to-back starves readers for the
        // whole drain (measured: reader stall == total drain time). Pause
        // for one budget length (~50% duty cycle) so any waiting reader gets
        // through; yield-spin rather than sleep_for because the default
        // Windows timer granularity (15.6 ms) would turn a 500 µs pause into
        // milliseconds per section and balloon mount wall time.
        const auto resumeAt = std::chrono::steady_clock::now() + kSpliceDrainBudget;
        while (std::chrono::steady_clock::now() < resumeAt)
            std::this_thread::yield();
    }
}

bool AssetRegistry::IsStartupScanRunning(std::string_view sourceAlias) const
{
    const std::string normalizedAlias = NormalizeRegistryAssetSourceAlias(sourceAlias);
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const SourceEntry* source = FindSourceByAlias(normalizedAlias);
    return source && source->StartupScanFuture.valid() &&
           source->StartupScanFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void AssetRegistry::WaitForStartupScan(std::string_view sourceAlias)
{
    std::vector<std::future<size_t>> scans;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (!m_Initialized)
            return;

        const std::string normalizedAlias =
            sourceAlias.empty() ? std::string{} : NormalizeRegistryAssetSourceAlias(sourceAlias);

        for (auto& src : m_Sources)
        {
            if (!normalizedAlias.empty() && src->Alias != normalizedAlias)
                continue;
            if (src->StartupScanFuture.valid())
                scans.push_back(std::move(src->StartupScanFuture));
        }
    }

    for (auto& scan : scans)
    {
        try
        {
            (void)scan.get();
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("AssetRegistry: startup scan ended with exception: {}", e.what());
        }
        catch (...)
        {
            Logger::Log::Warning("AssetRegistry: startup scan ended with unknown exception");
        }
    }
}

// ------------------------------------------------------------------
// UnregisterSource
// ------------------------------------------------------------------

bool AssetRegistry::IsSourceFlushInProgress(std::string_view sourceAlias) const
{
    const std::string normalized = NormalizeRegistryAssetSourceAlias(sourceAlias);
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    if (m_ProjectSource && m_ProjectSource->Alias == normalized)
        return m_ProjectSource->FlushInProgress.load(std::memory_order_acquire);
    for (const auto& src : m_Sources)
    {
        if (src->Alias == normalized)
            return src->FlushInProgress.load(std::memory_order_acquire);
    }
    return false;
}

bool AssetRegistry::UnregisterSource(std::string_view sourceAlias)
{
    ForgetSubassetContainers();
    const std::string normalized = NormalizeRegistryAssetSourceAlias(sourceAlias);
    if (normalized.empty())
        return false;

    // S11 shape (mirrors RegisterSource): the scan wait and the store flush
    // — the I/O — run OFF the registry lock via a pinned entry; the source
    // itself is removed in one short writer section (atomic to readers);
    // its records are evicted afterwards in paced, budget-boxed sections.
    // By return, everything is gone. During the eviction window the source
    // no longer resolves while its remaining records drain out — the same
    // transient a file-deleted-but-not-yet-unregistered asset already
    // presents, and the shape a streaming unmount needs (no frame hitch).
    SharedPtr<SourceEntry> pinned;
    std::future<size_t> scanFuture;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        auto it = std::find_if(m_Sources.begin(), m_Sources.end(),
                               [&](const SharedPtr<SourceEntry>& s) { return s->Alias == normalized; });
        if (it == m_Sources.end())
            return false;
        // Guard the alias against a concurrent RegisterSource/RebindSource
        // while we work off-lock.
        if (!m_PendingSourceAliases.insert(normalized).second)
        {
            Logger::Log::Warning("AssetRegistry::UnregisterSource: alias '{}' has a mount operation in flight",
                                 normalized);
            return false;
        }
        pinned = *it;
        if (pinned->StartupScanFuture.valid())
            scanFuture = std::move(pinned->StartupScanFuture);
    }

    // Off-lock: wait for the startup scan, then flush the store (same
    // FlushInProgress claim protocol as TickPersistence, so an in-flight
    // async flush is waited out rather than raced).
    if (scanFuture.valid())
    {
        try { (void)scanFuture.get(); } catch (...) {}
    }
    WaitForFlushRelease(pinned->FlushInProgress, pinned->Alias, "UnregisterSource");
    // IsReadOnly gate for consistency with Shutdown's flush: read-only
    // mounts skip persistent writes. Moot in practice — immutable/read-only
    // mounts reject mutations and never go dirty — but unmount must not be
    // the one path that could write to a read-only mount's DB.
    if (pinned->HasStore() && pinned->StoreDirty.load(std::memory_order_relaxed) &&
        !pinned->IsReadOnly)
    {
        bool expected = false;
        if (pinned->FlushInProgress.compare_exchange_strong(expected, true))
        {
            FlushClaimRelease release{pinned->FlushInProgress};
            pinned->StoreDirty.store(false, std::memory_order_relaxed);
            std::string err;
            // This is the mount's last flush: there is no later tick to retry
            // on, so it waits out another process holding the store's write
            // lock rather than dropping the edit. Main thread, bounded by that
            // process's own save.
            if (!pinned->Store->SaveToFile(pinned->DbFile, &err,
                                           AssetDatabase::StoreSaveWait::WaitForPeers))
            {
                Logger::Log::Warning("AssetRegistry: failed to flush '{}' on unmount: {}",
                                     pinned->DbFile.string(), err);
                pinned->StoreDirty.store(true, std::memory_order_relaxed);
            }
        }
        else
        {
            WaitForFlushRelease(pinned->FlushInProgress, pinned->Alias, "UnregisterSource");
        }
    }

    // Short writer section: remove the source atomically. A mount still
    // draining its overlay is discarded wholesale (its undrained records
    // never reach the global maps); pending path claims drop with it.
    FastHashSet<GUID> ownedGuids;
    std::filesystem::path removedRoot;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        auto it = std::find_if(m_Sources.begin(), m_Sources.end(),
                               [&](const SharedPtr<SourceEntry>& s) { return s.get() == pinned.get(); });
        m_PendingSourceAliases.erase(normalized);
        if (it == m_Sources.end())
            return false; // raced a Shutdown; nothing left to do
        removedRoot = pinned->Root;

        DiscardOverlayForAliasLocked(normalized);
        PurgeClaimsUnderPrefixLocked(pinned->RootPrefixKey);
        ownedGuids = std::move(pinned->ResidentGuids);
        pinned->ResidentGuids.clear();

        // Resetting our SharedPtr drops the registry's co-ownership; any
        // caller that pinned the entry via ProjectSourcePinned() keeps it
        // alive until they release.
        if (normalized == "project")
        {
            m_ProjectSource.reset();
            m_ProjectHasRedirects.store(false, std::memory_order_relaxed);
        }
        m_Sources.erase(it);
    }

    const size_t removedAssetCount = ownedGuids.size();
    const DrainStats evictStats = EvictSourceRecordsPaced(std::move(ownedGuids), {});
    if (removedAssetCount >= 1000)
    {
        std::fprintf(stderr,
                     "[S11 unmount-prof] alias=%s records=%zu evict-sections=%zu evict-max-section=%.2fms evict>1ms=%zu\n",
                     normalized.c_str(), removedAssetCount,
                     evictStats.Sections, evictStats.MaxSectionMs, evictStats.SectionsOverMs);
    }

    Logger::Log::Info("AssetRegistry: removed source '{}' mounted at '{}' (evicted {} hot-cache entries)",
                      normalized,
                      removedRoot.string(),
                      removedAssetCount);
    return true;
}

// ------------------------------------------------------------------
// RebindSource
// ------------------------------------------------------------------

bool AssetRegistry::RebindSource(std::string_view sourceAlias, const AssetSourceDesc& newDesc)
{
    ForgetSubassetContainers();
    const std::string normalized = NormalizeRegistryAssetSourceAlias(sourceAlias);
    if (normalized.empty())
        return false;

    // S11 shape (mirrors RegisterSource): the heavy I/O — old-store flush,
    // new JSONL/manifest load, reconcile, per-record populate — runs OFF the
    // registry lock; the source flips to its replacement incarnation in ONE
    // short writer section (old entry swapped out of m_Sources, new entry +
    // its complete record set published atomically via the overlay); records
    // the new DB no longer contains are evicted in paced sections and the
    // overlay drains before return. Readers never stall longer than one
    // budget-boxed section. [was: the entire rebind under the writer lock —
    // 23.6 s reader stall at 100k records.]
    SharedPtr<SourceEntry> oldEntry;
    std::future<size_t> scanFuture;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        SourceEntry* entry = FindSourceByAlias(normalized);
        if (!entry)
        {
            Logger::Log::Error("AssetRegistry::RebindSource: unknown alias '{}'", normalized);
            return false;
        }
        // Guard the alias against concurrent Register/Unregister/Rebind
        // while the staging work runs off-lock.
        if (!m_PendingSourceAliases.insert(normalized).second)
        {
            Logger::Log::Warning("AssetRegistry::RebindSource: alias '{}' has a mount operation in flight",
                                 normalized);
            return false;
        }
        for (const auto& src : m_Sources)
        {
            if (src.get() == entry)
            {
                oldEntry = src;
                break;
            }
        }
        if (oldEntry->StartupScanFuture.valid())
            scanFuture = std::move(oldEntry->StartupScanFuture);
    }

    // Off-lock: wait for the startup scan, then flush the old store (same
    // FlushInProgress claim protocol as TickPersistence). The old store
    // stays OPEN and serving until the flip below, so identity resolution
    // never degrades during the staging window.
    if (scanFuture.valid())
    {
        try { (void)scanFuture.get(); } catch (...) {}
    }
    WaitForFlushRelease(oldEntry->FlushInProgress, oldEntry->Alias, "RebindSource");
    // Same IsReadOnly gate as UnregisterSource/Shutdown: read-only mounts
    // skip persistent writes (moot in practice — they never go dirty).
    if (oldEntry->HasStore() && oldEntry->StoreDirty.load(std::memory_order_relaxed) &&
        !oldEntry->IsReadOnly)
    {
        bool expected = false;
        if (oldEntry->FlushInProgress.compare_exchange_strong(expected, true))
        {
            FlushClaimRelease release{oldEntry->FlushInProgress};
            oldEntry->StoreDirty.store(false, std::memory_order_relaxed);
            std::string err;
            // The old incarnation is about to be replaced and never ticks
            // again, so this save waits out a peer rather than dropping it.
            if (!oldEntry->Store->SaveToFile(oldEntry->DbFile, &err,
                                             AssetDatabase::StoreSaveWait::WaitForPeers))
            {
                Logger::Log::Warning("AssetRegistry: failed to flush '{}' on rebind: {}",
                                     oldEntry->DbFile.string(), err);
                oldEntry->StoreDirty.store(true, std::memory_order_relaxed);
            }
        }
        else
        {
            WaitForFlushRelease(oldEntry->FlushInProgress, oldEntry->Alias, "RebindSource");
        }
    }

    const std::filesystem::path oldRoot = oldEntry->Root;
    const std::string oldRootPrefixKey = oldEntry->RootPrefixKey;

    // Off-lock: build the replacement incarnation. Identity model, priority,
    // namespace, and lifecycle flags carry over from the original
    // registration (rebind moves the root/DB only); root-derived state comes
    // from the new descriptor.
    auto newEntry = std::make_shared<SourceEntry>();
    newEntry->Alias = normalized;
    newEntry->Root = AssetPaths::NormalizeMountRoot(newDesc.Root);
    newEntry->RootPrefixKey = MakeRootPrefixKey(newEntry->Root);
    newEntry->NamespaceGuid = oldEntry->NamespaceGuid;
    newEntry->Priority = oldEntry->Priority;
    newEntry->DerivedIdentity = oldEntry->DerivedIdentity;
    newEntry->RequiresScan = oldEntry->RequiresScan;
    newEntry->IsImmutable = oldEntry->IsImmutable;
    newEntry->AcceptsMetadataWrites = oldEntry->AcceptsMetadataWrites;
    newEntry->StoreIsSharedAcrossProcesses = oldEntry->StoreIsSharedAcrossProcesses;
    newEntry->InfersTextureImportSettings = oldEntry->InfersTextureImportSettings;
    newEntry->CooksDerivedArtifactsOnMiss = oldEntry->CooksDerivedArtifactsOnMiss;
    newEntry->IsReadOnly = oldEntry->IsReadOnly;
    newEntry->TracksTombstones = oldEntry->TracksTombstones;
    newEntry->AggressiveTombstoneCleanup = oldEntry->AggressiveTombstoneCleanup;
    newEntry->RejectOverlappingRoots = oldEntry->RejectOverlappingRoots;
    newEntry->GhostRetentionSessions = oldEntry->GhostRetentionSessions;
    newEntry->CollectNeverHealableGhosts = oldEntry->CollectNeverHealableGhosts;
    newEntry->IdentityManifestFile = oldEntry->IdentityManifestFile;

    std::error_code ec;
    if (!std::filesystem::exists(newEntry->Root, ec))
    {
        std::filesystem::create_directories(newEntry->Root, ec);
    }
    newEntry->CanonicalRootPrefixKey = MakeCanonicalRootPrefixKey(newEntry->Root);
    ResolveSourceDbPaths(newDesc, *newEntry);
    newEntry->IgnoreRules = AssetIgnoreRules::LoadForAssetRoot(newEntry->Root);

    const bool setupOk = SetupSourceStore(*newEntry);
    if (setupOk && !newEntry->DerivedIdentity && newEntry->HasStore())
    {
        ReconcileSourceStore(*newEntry);
    }
    Vector<AssetMetadata> stagedRecords;
    if (setupOk)
    {
        PopulateHotCachesFromSource(*newEntry, stagedRecords);
    }

    auto overlay = MakeUnique<MountOverlay>();
    overlay->SourceAlias = normalized;
    overlay->Assets.reserve(stagedRecords.size());
    overlay->PathToGuid.reserve(stagedRecords.size());
    for (AssetMetadata& md : stagedRecords)
    {
        overlay->PathToGuid[PathMapKey(md.Path)] = md.Guid;
        overlay->Assets[md.Guid] = std::move(md);
    }
    stagedRecords.clear();

    // Atomic flip: swap the entry and publish the new record set in one
    // short writer section.
    FastHashSet<GUID> oldGuids;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        m_PendingSourceAliases.erase(normalized);

        if (!setupOk)
        {
            // Old incarnation stays mounted and fully functional (its store
            // was flushed but never closed) — a strictly better failure mode
            // than the old half-rebound entry.
            Logger::Log::Error("AssetRegistry::RebindSource: failed to set up store for alias '{}'", normalized);
            return false;
        }
        if (!m_Initialized)
            return false;

        auto it = std::find_if(m_Sources.begin(), m_Sources.end(),
                               [&](const SharedPtr<SourceEntry>& s) { return s.get() == oldEntry.get(); });
        if (it == m_Sources.end())
            return false; // concurrently unregistered / shutdown

        *it = newEntry; // same priority — sort order preserved

        DiscardOverlayForAliasLocked(normalized); // defensive; none can be pending
        oldGuids = std::move(oldEntry->ResidentGuids);
        oldEntry->ResidentGuids.clear();
        if (!overlay->Assets.empty())
        {
            // S12: published overlay records enter the counted union.
            ApplyAssetCountDelta(static_cast<ptrdiff_t>(overlay->Assets.size()));
            m_MountOverlays.push_back(std::move(overlay));
        }
        PurgeClaimsUnderPrefixLocked(oldRootPrefixKey);

        if (normalized == "project")
        {
            m_ProjectSource = newEntry;
            RefreshProjectRedirectFlagLocked();
        }
    }

    // Paced: evict records the new DB no longer carries (GUIDs present in
    // the published overlay are skipped — the drain refreshes those in
    // place, so shared-identity rebinds never expose a resolution gap),
    // then drain the new set into the global maps.
    const size_t evictCandidates = oldGuids.size();
    const DrainStats evictStats = EvictSourceRecordsPaced(std::move(oldGuids), normalized);
    const DrainStats drainStats = DrainMountOverlay(normalized);
    if (evictCandidates >= 1000)
    {
        std::fprintf(stderr,
                     "[S11 rebind-prof] alias=%s old-records=%zu evict-sections=%zu evict-max-section=%.2fms "
                     "evict>1ms=%zu drain-sections=%zu drain-max-section=%.2fms drain>1ms=%zu\n",
                     normalized.c_str(), evictCandidates,
                     evictStats.Sections, evictStats.MaxSectionMs, evictStats.SectionsOverMs,
                     drainStats.Sections, drainStats.MaxSectionMs, drainStats.SectionsOverMs);
    }

    // The old incarnation's store/cache close when the last pin releases
    // (the Shutdown in-place keep-alive pattern); content was flushed above.
    const std::filesystem::path reboundRoot = newEntry->Root;

    // Start directory scan, mirroring RegisterSource's RequiresScan gate.
    // Package mounts and other manifest-driven sources skip the scan on
    // rebind too — the new root's manifest is read by SetupSourceStore.
    bool reboundRequiresScan = true;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const SourceEntry* reboundEntry = FindSourceByAlias(normalized);
        if (reboundEntry)
            reboundRequiresScan = reboundEntry->RequiresScan;
    }

    if (!reboundRequiresScan)
    {
        Logger::Log::Trace("AssetRegistry: source '{}' has RequiresScan=false; skipping rebind scan",
                           normalized);
    }
    else if (m_JobSystem)
    {
        auto rebindScanFuture = ScanDirectoryAsync(reboundRoot, true);
        std::unique_lock<std::shared_mutex> wl(m_RegistryMutex);
        if (SourceEntry* rebound = FindSourceByAlias(normalized))
            rebound->StartupScanFuture = std::move(rebindScanFuture);
    }
    else
    {
        ScanDirectory(reboundRoot, true);
    }

    Logger::Log::Info("AssetRegistry: remounted source '{}' from '{}' to '{}'",
                      normalized,
                      oldRoot.string(),
                      reboundRoot.string());
    return true;
}

// ------------------------------------------------------------------
// TryGetCacheRoot (guid + path overloads)
// ------------------------------------------------------------------
//
// cacheDbFile lives at <ProjectRoot>/.Cache/AssetDatabase/AssetDbCache.sqlite
// by editor convention. Two parent_path()'s climbs from the .sqlite file
// up to the .Cache/ parent — the project-wide root that AssetDatabase/,
// Shaders/, MeshBvh/, etc. all sit under as siblings.

namespace
{
std::optional<std::filesystem::path> CacheRootFromDbFile(const std::filesystem::path& cacheDbFile)
{
    if (cacheDbFile.empty())
        return std::nullopt;
    auto parent = cacheDbFile.parent_path();          // .Cache/AssetDatabase/
    auto grandparent = parent.parent_path();          // .Cache/
    return grandparent.empty() ? parent : grandparent;
}
}

std::optional<std::filesystem::path>
AssetRegistry::TryGetCacheRoot(const GUID& guid) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const AssetMetadata* meta = FindAssetLocked(guid);
    if (!meta || meta->Path.empty())
        return std::nullopt;

    const auto owner = m_AssetSourceOwner.find(PathMapKey(meta->Path));
    const SourceEntry* src = owner != m_AssetSourceOwner.end()
        ? FindSourceByAlias(owner->second) : FindSourceForPath(meta->Path);
    if (!src)
        return std::nullopt;
    return CacheRootFromDbFile(src->CacheDbFile);
}

std::optional<std::filesystem::path>
AssetRegistry::TryGetCacheRoot(const std::filesystem::path& absPath) const
{
    if (absPath.empty())
        return std::nullopt;

    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const auto owner = m_AssetSourceOwner.find(PathMapKey(NormalizePathForMap(absPath)));
    const SourceEntry* src = owner != m_AssetSourceOwner.end()
        ? FindSourceByAlias(owner->second) : FindSourceForPath(absPath);
    if (!src)
        return std::nullopt;
    return CacheRootFromDbFile(src->CacheDbFile);
}

bool AssetRegistry::AcceptsDerivedRecords(const std::filesystem::path& absPath) const
{
    if (absPath.empty())
        return false;

    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const auto owner = m_AssetSourceOwner.find(PathMapKey(NormalizePathForMap(absPath)));
    const SourceEntry* src = owner != m_AssetSourceOwner.end()
        ? FindSourceByAlias(owner->second) : FindSourceForPath(absPath);
    return src && !src->IsImmutable && !src->IsReadOnly;
}

DerivedArtifactPolicy AssetRegistry::GetDerivedArtifactPolicy(const GUID& guid) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const AssetMetadata* meta = FindAssetLocked(guid);
    if (!meta || meta->Path.empty())
        return {};

    const auto owner = m_AssetSourceOwner.find(PathMapKey(meta->Path));
    const SourceEntry* source = owner != m_AssetSourceOwner.end()
        ? FindSourceByAlias(owner->second)
        : FindSourceForPath(meta->Path);
    if (!source)
        return {};
    return {source->InfersTextureImportSettings, source->CooksDerivedArtifactsOnMiss};
}

// ------------------------------------------------------------------
// GetAssetsForSource
// ------------------------------------------------------------------

std::vector<GUID> AssetRegistry::GetAssetsForSource(std::string_view sourceAlias) const
{
    const std::string normalized = NormalizeRegistryAssetSourceAlias(sourceAlias);
    std::vector<GUID> result;

    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const SourceEntry* src = FindSourceByAlias(normalized);
    if (!src)
        return result;

    ForEachAssetLocked([&](const GUID& guid, const AssetMetadata& md)
    {
        const std::string pathKey = PathMapKey(md.Path);
        // Ownership-aware: if an asset has explicit source ownership, only
        // include it when the owner matches.  This prevents project rebind
        // from ejecting editor-owned assets when both mounts share a root.
        auto ownerIt = m_AssetSourceOwner.find(pathKey);
        if (ownerIt != m_AssetSourceOwner.end())
        {
            if (ownerIt->second == normalized)
                result.push_back(guid);
            return;
        }

        // No explicit owner -- fall back to path membership.
        if (PathKeyHasRootPrefix(pathKey, src->RootPrefixKey))
            result.push_back(guid);
    });
    return result;
}

// ------------------------------------------------------------------
// GetRegisteredSources
// ------------------------------------------------------------------

std::vector<AssetSourceDesc> AssetRegistry::GetRegisteredSources() const
{
    std::vector<AssetSourceDesc> out;
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    out.reserve(m_Sources.size());
    for (const auto& src : m_Sources)
    {
        AssetSourceDesc desc{};
        desc.Alias = src->Alias;
        desc.Root = src->Root;
        desc.DerivedIdentity = src->DerivedIdentity;
        desc.AuthoritativeDbFile = src->DbFile;
        desc.CacheRoot = src->CacheDbFile.empty() ? std::filesystem::path{} : src->CacheDbFile.parent_path();
        desc.IdentityManifestFile = src->IdentityManifestFile;
        desc.Priority = src->Priority;
        // Lifecycle flags round-trip so callers (e.g. package publishing) can
        // reason about mutability. RegisterFileWatcher is not stored on the
        // entry (AssetManager-only concern) and stays at its default here.
        desc.RequiresScan = src->RequiresScan;
        desc.IsImmutable = src->IsImmutable;
        desc.AcceptsMetadataWrites = src->AcceptsMetadataWrites;
        desc.StoreIsSharedAcrossProcesses = src->StoreIsSharedAcrossProcesses;
        desc.InfersTextureImportSettings = src->InfersTextureImportSettings;
        desc.CooksDerivedArtifactsOnMiss = src->CooksDerivedArtifactsOnMiss;
        desc.IsReadOnly = src->IsReadOnly;
        desc.TracksTombstones = src->TracksTombstones;
        desc.AggressiveTombstoneCleanup = src->AggressiveTombstoneCleanup;
        desc.RejectOverlappingRoots = src->RejectOverlappingRoots;
        desc.GhostRetentionSessions = src->GhostRetentionSessions;
        desc.CollectNeverHealableGhosts = src->CollectNeverHealableGhosts;
        out.push_back(std::move(desc));
    }
    return out;
}

bool AssetRegistry::VisitSourceStoreRecords(
    std::string_view sourceAlias,
    const std::function<void(const AssetDatabase::AssetRecord&)>& visitor) const
{
    if (!visitor)
        return false;

    // Copy the records out under the shared lock (EnumerateAssets snapshots
    // internally under the store's own mutex), then run the visitor lock-free
    // so it may call back into the registry.
    std::vector<AssetDatabase::AssetRecord> records;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const SourceEntry* src = FindSourceByAlias(sourceAlias);
        if (!src || !src->HasStore())
            return false;
        records = src->Store->EnumerateAssets();
    }
    for (const AssetDatabase::AssetRecord& rec : records)
        visitor(rec);
    return true;
}

GUID AssetRegistry::DeriveGuidForSourcePath(std::string_view sourceAlias,
                                            std::string_view relativePath)
{
    if (sourceAlias.empty() || relativePath.empty())
        return GUID::Null();
    return DeriveSourceScopedGuid(BuildRegistryAssetSourceNamespaceGuid(sourceAlias),
                                  sourceAlias, relativePath);
}

// ------------------------------------------------------------------
// GetAssetRoot / GetSourceRoot
// ------------------------------------------------------------------

bool AssetRegistry::IsAssetPathAvailable(const std::filesystem::path& absolutePath) const
{
    std::error_code error;
    if (std::filesystem::exists(absolutePath, error))
        return true;
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const auto normalized = NormalizePathForMap(absolutePath);
    const auto* guid = FindPathGuidLocked(PathMapKey(normalized));
    const auto* source = FindSourceForPath(normalized);
    return guid && source && source->IsReadOnly;
}

std::filesystem::path AssetRegistry::ResolveRelativeAssetPath(const std::filesystem::path& relativePath) const
{
    // Copy the roots under the lock and probe the filesystem outside it.
    std::vector<std::filesystem::path> roots;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        roots.reserve(m_Sources.size() + 1);
        if (m_ProjectSource)
            roots.push_back(m_ProjectSource->Root);
        for (const auto& source : m_Sources)
        {
            if (source != m_ProjectSource)
                roots.push_back(source->Root);
        }
    }

    for (const std::filesystem::path& root : roots)
    {
        if (root.empty())
            continue;
        std::filesystem::path candidate = (root / relativePath).lexically_normal();
        if (IsAssetPathAvailable(candidate))
            return candidate;
    }
    return {};
}

std::filesystem::path AssetRegistry::GetAssetRoot() const
{
    // Reads m_ProjectSource under shared_lock and copies the path before
    // releasing. Returning by value means the caller's path is independent
    // of the SourceEntry's lifetime — no UAF if the registry is shut down
    // or the project source rebinds while the caller still holds the path.
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    return m_ProjectSource ? m_ProjectSource->Root : std::filesystem::path{};
}

std::filesystem::path AssetRegistry::GetSourceRoot(std::string_view sourceAlias) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const SourceEntry* src = FindSourceByAlias(sourceAlias);
    return src ? src->Root : std::filesystem::path{};
}

std::string AssetRegistry::GetAssetSourceOwnerAlias(const std::filesystem::path& path) const
{
    if (path.empty())
        return {};

    // Lock for the entire ProjectRoot() deref + map lookup.
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (ProjectRoot() / path) : path);
    auto ownerIt = m_AssetSourceOwner.find(PathMapKey(normPath));
    if (ownerIt == m_AssetSourceOwner.end())
        return {};
    return ownerIt->second;
}

// ------------------------------------------------------------------
// GetIgnoreRulesSnapshot (updated for per-source rules)
// ------------------------------------------------------------------

AssetIgnoreRules AssetRegistry::GetIgnoreRulesSnapshot() const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    // Return the project source's ignore rules for backward compatibility.
    if (m_ProjectSource)
        return m_ProjectSource->IgnoreRules;
    // Fallback: return first source's rules or default.
    if (!m_Sources.empty())
        return m_Sources.front()->IgnoreRules;
    return AssetIgnoreRules{};
}

// ------------------------------------------------------------------
// Convenience Initialize overload
// ------------------------------------------------------------------

bool AssetRegistry::Initialize(const std::filesystem::path& assetRoot,
                               JobSystem::WorkStealingThreadPool* jobSystem,
                               const std::filesystem::path& authoritativeDbFile,
                               const std::filesystem::path& cacheRoot)
{
    // Convenience overload: infrastructure init + register a "project" source.
    if (!Initialize(jobSystem))
        return false;

    // Compute default DB paths for the project source.
    const AssetDatabase::AssetDatabasePaths dbPaths =
        AssetDatabase::GetDefaultPathsForAssetRoot(assetRoot, authoritativeDbFile, cacheRoot);

    AssetSourceDesc projectDesc{};
    projectDesc.Alias = "project";
    projectDesc.Root = assetRoot;
    projectDesc.DerivedIdentity = false;
    projectDesc.AuthoritativeDbFile = dbPaths.authoritativeFile;
    projectDesc.CacheRoot = dbPaths.cacheRoot;
    projectDesc.Priority = 100;

    if (!RegisterSource(projectDesc))
    {
        Logger::Log::Error("AssetRegistry: failed to register project source at '{}'", assetRoot.string());
        return false;
    }

    return true;
}

void AssetRegistry::Shutdown()
{
    // Stop the in-flight startup scans, then join them before tearing down.
    // A cancelled scan returns at its next file or chunk boundary instead of
    // hashing and registering the rest of its tree; batches it registered
    // stay registered and are flushed below.
    std::vector<SharedPtr<SourceEntry>> cancelledScanSources;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (!m_Initialized)
            return;

        if (m_AsyncCoordinator)
            m_AsyncCoordinator->CancelScans();

        // Collect all scan futures so we can wait outside the lock.
        std::vector<std::pair<SharedPtr<SourceEntry>, std::future<size_t>>> scans;
        for (auto& src : m_Sources)
        {
            if (src->StartupScanFuture.valid())
                scans.emplace_back(src, std::move(src->StartupScanFuture));
        }
        writeLock.unlock();

        for (auto& [source, scan] : scans)
        {
            try { (void)scan.get(); }
            catch (const AssetScanCancelled&)
            {
                Logger::Log::Info("AssetRegistry: shutdown stopped the startup scan of '{}' before it finished",
                                  source->Alias);
                cancelledScanSources.push_back(source);
            }
            catch (const std::exception& e)
            {
                Logger::Log::Warning("AssetRegistry: startup scan ended with exception: {}", e.what());
            }
            catch (...)
            {
                Logger::Log::Warning("AssetRegistry: startup scan ended with unknown exception");
            }
        }
    }

    // Wait for any in-flight async flushes to complete before destroying sources.
    // Bounded spin so a wedged job (e.g. JobSystem already torn down, unrecoverable
    // disk error on the worker) can't deadlock shutdown. Individual flushes are
    // seconds at most; 30s is a generous ceiling that still beats a hang.
    {
        constexpr auto kFlushWaitTimeout = std::chrono::seconds(30);
        constexpr auto kPollInterval = std::chrono::milliseconds(5);

        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        for (auto& src : m_Sources)
        {
            if (!src->FlushInProgress.load(std::memory_order_acquire))
                continue;

            const auto deadline = std::chrono::steady_clock::now() + kFlushWaitTimeout;
            while (src->FlushInProgress.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    Logger::Log::Warning(
                        "AssetRegistry: flush for source '{}' did not complete within {}s; "
                        "proceeding with shutdown (in-flight write may leave a stale .tmp file)",
                        src->Alias,
                        std::chrono::duration_cast<std::chrono::seconds>(kFlushWaitTimeout).count());
                    break;
                }
                std::this_thread::sleep_for(kPollInterval);
            }
        }
    }

    // Block A.2: collect SharedPtr<SourceEntry> co-owners so the entries
    // outlive any pinned callers that grabbed them via ProjectSourcePinned()
    // before Shutdown ran. The actual flush + snapshot write happens
    // below — IN-PLACE on each SourceEntry, holding the registry's
    // unique_lock for the duration. This matters because moving store/
    // cache out of the SourceEntry (the previous pattern) would null the
    // UniquePtrs that pinned callers still expect to dereference.
    std::vector<SharedPtr<SourceEntry>> toFlush;

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (!m_Initialized)
            return;

        Logger::Log::Info("Shutting down Asset Registry");

        // Capture each source's SharedPtr (co-ownership) so its store +
        // cache survive at least until our in-place flush completes,
        // even if pinned callers are still holding their own SharedPtr.
        for (auto& src : m_Sources)
        {
            if (src->Store)
                toFlush.push_back(src);
        }

        // Flush + write snapshot for each captured source under the same
        // unique_lock. Holding the lock blocks new callers from acquiring
        // shared_lock to call ProjectSourcePinned(), but pinned callers
        // who already returned can keep using their SharedPtr — they go
        // through the cache's own internal mutex, which is independent.
        for (auto& sfPtr : toFlush)
        {
            auto& entry = *sfPtr;
            // Read-only mounts skip persistent writes (no flush, no snapshot).
            const bool dirty = entry.StoreDirty.load(std::memory_order_relaxed);
            if (dirty && entry.Store && !entry.DbFile.empty() && !entry.IsReadOnly)
            {
                std::string err;
                // The last write this process will make: a refused save here
                // is the edit gone, so it waits out a peer holding the store's
                // write lock instead of warning and dropping it.
                if (!entry.Store->SaveToFile(entry.DbFile, &err,
                                             AssetDatabase::StoreSaveWait::WaitForPeers))
                {
                    Logger::Log::Warning("AssetRegistry: failed to flush '{}' on shutdown: {}",
                                         entry.DbFile.string(), err);
                }
            }

            // F.5: skip snapshot write when nothing has changed since the
            // snapshot was loaded. Conditions:
            //   - Snapshot was successfully loaded at Initialize
            //     (entry.SnapshotByPath non-empty → there's a current
            //     on-disk snapshot to keep using).
            //   - Store hasn't been mutated (storeDirty == false).
            //   - Cache hasn't been mutated since Initialize (mutation
            //     count matches the load-time baseline).
            //
            // The dir-mtime trailer can drift slightly under this gate
            // (e.g. someone touches a project dir mid-session via the
            // file system without changing records), but the worst-case
            // outcome is a no-op skip in next session's warm-start scan.
            // Saves ~400ms at 50K shutdown.
            const bool snapshotCanSkip =
                !entry.SnapshotByPath.empty() &&
                !entry.StoreDirty.load(std::memory_order_relaxed) &&
                entry.Cache &&
                entry.Cache->GetMutationCount() == entry.SnapshotLoadTimeMutationCount;

            if (!snapshotCanSkip &&
                entry.Cache && entry.Store && !entry.CacheDbFile.empty() && !entry.IsReadOnly)
            {
                const std::filesystem::path snapshotFile = SourceSnapshotFilePath(entry.CacheDbFile);

                // F.1: fine-grained timing — find which sub-step of the
                // snapshot write dominates 50K shutdown (Phase C measured
                // 731ms total). Logs at Info for the bench to pick up.
                using fclock = std::chrono::high_resolution_clock;
                using fmsd = std::chrono::duration<double, std::milli>;
                const auto fT0 = fclock::now();

                std::vector<AssetDatabase::AssetSourceSnapshotRecord> records;
                const auto assetRecords = entry.Store->EnumerateAssets();
                records.reserve(assetRecords.size());
                // F.2: batch SQLite fingerprint lookup. Per-record
                // TryGetFileFingerprint cost ~17μs of prepare/finalize
                // overhead; at 50K records that was 854ms of shutdown.
                // Single SELECT + in-memory map is ~50ms for the same
                // data.
                const auto fingerprintMap = entry.Cache->EnumerateAllFingerprints();
                const auto fT1 = fclock::now();

                // Fingerprints are joined to store rows by GUID first, then
                // by path. The path fallback covers identity churn: when two
                // racing first-registrations of one path minted different
                // GUIDs, the surviving store row and the cache's fingerprint
                // row can disagree on GUID while still describing the same
                // file — and the snapshot itself is path-keyed, so a
                // path-matched fingerprint is exactly as trustworthy (the
                // stat-confirm contract re-validates it on load either way).
                std::unordered_map<std::string_view, const AssetDatabase::IAssetDbCache::FileFingerprint*>
                    fingerprintByPath;
                fingerprintByPath.reserve(fingerprintMap.size());
                for (const auto& [fpGuid, fp] : fingerprintMap)
                {
                    if (fp.valid && !fp.path.empty())
                        fingerprintByPath.emplace(fp.path, &fp);
                }

                for (const auto& ar : assetRecords)
                {
                    if (ar.guid.IsNull() || ar.path.empty())
                        continue;
                    const AssetDatabase::IAssetDbCache::FileFingerprint* fpPtr = nullptr;
                    if (auto fpIt = fingerprintMap.find(ar.guid);
                        fpIt != fingerprintMap.end() && fpIt->second.valid)
                    {
                        fpPtr = &fpIt->second;
                    }
                    else if (auto pathIt = fingerprintByPath.find(ar.path);
                             pathIt != fingerprintByPath.end())
                    {
                        fpPtr = pathIt->second;
                    }
                    if (!fpPtr)
                        continue;
                    const auto& fp = *fpPtr;
                    AssetDatabase::AssetSourceSnapshotRecord r;
                    r.CanonicalPath = ar.path;
                    r.Mtime = fp.mtime;
                    r.Size = fp.size;
                    r.FileId = fp.fileId;
                    r.Hash = fp.contentHash;
                    r.Guid = ar.guid.ToString();
                    r.TypeId = ar.typeId;
                    records.push_back(std::move(r));
                }
                const auto fT2 = fclock::now();

                // C.2: capture per-subdirectory mtimes under the mount root.
                // The warm-path scan + reconcile use these to skip subtrees
                // whose dir mtime is unchanged since this Shutdown — Windows
                // updates a directory's mtime when files are added/removed/
                // renamed within it, so an unchanged mtime is strong proof
                // the subtree is structurally untouched.
                //
                // Tradeoff: files whose CONTENT changed offline (without a
                // dir-mtime bump) go undetected at warm-start. Mid-session
                // file watchers + load-time validation cover that path.
                //
                // Implementation note: derive the set of dirs to stat from
                // the records we're about to write rather than walking the
                // filesystem. A 50K-file project has ~16-50 unique parent
                // dirs, so this is O(unique-dirs) stats instead of O(files).
                //
                // A startup scan cancelled by this Shutdown registered only
                // part of the tree, so a directory's unchanged mtime does not
                // prove its files are registered: that snapshot records no
                // directories, and the next scan walks the whole tree while
                // still skipping the files recorded here.
                const bool scanCancelled =
                    std::find(cancelledScanSources.begin(), cancelledScanSources.end(), sfPtr) !=
                    cancelledScanSources.end();
                std::vector<AssetDatabase::AssetSourceSnapshotDirectory> directories;
                if (!scanCancelled)
                {
                    std::unordered_set<std::string> dirSet;
                    for (const auto& r : records)
                    {
                        std::filesystem::path p(r.CanonicalPath);
                        while (p.has_parent_path())
                        {
                            std::filesystem::path parent = p.parent_path();
                            if (parent.empty() || parent == p)
                                break;
                            std::string parentStr = parent.generic_string();
                            if (parentStr.empty() || parentStr == ".")
                                break;
                            dirSet.insert(parentStr);
                            p = std::move(parent);
                        }
                    }

                    directories.reserve(dirSet.size());
                    for (const auto& canonicalRel : dirSet)
                    {
                        const std::filesystem::path absDir =
                            entry.Root / std::filesystem::path(canonicalRel);
                        std::error_code mtEc;
                        const auto mtime = std::filesystem::last_write_time(absDir, mtEc);
                        if (mtEc)
                            continue;
                        AssetDatabase::AssetSourceSnapshotDirectory d;
                        d.CanonicalPath = canonicalRel;
                        d.Mtime = FileTimeToInt64(mtime);
                        directories.push_back(std::move(d));
                    }
                }

                const auto fT3 = fclock::now();
                std::string err;
                const bool saveOk = AssetDatabase::AssetSourceSnapshot::Save(snapshotFile, entry.Root,
                                                              entry.IgnoreRules.Signature(),
                                                              records, directories, &err);
                const auto fT4 = fclock::now();

                if (records.size() >= 1000)
                {
                    std::fprintf(stderr,
                        "[F.1 shutdown-prof] alias=%s records=%zu enumerate=%.1fms fingerprint-loop=%.1fms dir-stats=%.1fms binary-save=%.1fms\n",
                        entry.Alias.c_str(), records.size(),
                        fmsd(fT1 - fT0).count(),
                        fmsd(fT2 - fT1).count(),
                        fmsd(fT3 - fT2).count(),
                        fmsd(fT4 - fT3).count());
                }

                if (!saveOk)
                {
                    Logger::Log::Warning("AssetRegistry: failed to write snapshot for '{}' at '{}': {}",
                                         entry.Alias, snapshotFile.string(), err);
                }
                else
                {
                    Logger::Log::Trace("AssetRegistry: wrote snapshot for '{}' ({} records)",
                                       entry.Alias, records.size());
                }
            }
        }

        m_Sources.clear();
        m_ProjectSource.reset();
        m_ProjectHasRedirects.store(false, std::memory_order_relaxed);
        m_Initialized = false;
        m_AsyncCoordinator.reset();
        m_JobSystem = nullptr;
        m_TypeRegistry.Shutdown();
        for (auto& shard : m_AssetShards)
            shard.clear();
        for (auto& shard : m_PathShards)
            shard.clear();
        m_AssetSourceOwner.clear();
        m_MountOverlays.clear();
        m_PathClaims.clear();
        m_GuidRemaps.clear();
        m_AssetCount.store(0, std::memory_order_relaxed);
    }

    // Drop our co-ownership of the captured entries. If any pinned callers
    // still hold SharedPtrs from ProjectSourcePinned(), the SourceEntry
    // (and its store/cache) survives until they release.
    toFlush.clear();
}

Vector<String> AssetRegistry::GetAssetDatabaseConflicts() const
{
    // Computed on demand from the project source's store. Walks the
    // pinned project source through IAssetStore::GetLoadConflicts —
    // editor-facing diagnostic surface that doesn't need cached state
    // on the registry. JSONL is the only backend that emits conflicts
    // today; other backends return an empty list via the default vtable.
    auto pinned = ProjectSourcePinned();
    if (!pinned || !pinned->Store)
        return {};
    const auto conflicts = pinned->Store->GetLoadConflicts();
    Vector<String> out;
    out.reserve(conflicts.size());
    for (const auto& c : conflicts)
        out.push_back(c);
    return out;
}

void AssetRegistry::SetParserRegistry(ParserRegistry* parserRegistry)
{
    m_ParserRegistry = parserRegistry;
}

AssetType AssetRegistry::ClassifyAssetType(const std::filesystem::path& path) const
{
    if (path.empty())
    {
        return AssetType::Unknown;
    }

    ParserRegistry* parserRegistry = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        parserRegistry = m_ParserRegistry;
        assetRoot = ProjectRoot();
    }

    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (assetRoot / path) : path);

    // Prefer parser-based classification when available (supports sniffing and multiple parsers per extension).
    if (parserRegistry)
    {
        if (auto parser = parserRegistry->FindParser(normPath))
        {
            return parser->GetAssetType();
        }
    }

    // Fallback: extension-based mapping (fast and deterministic).
    // Use compound-extension lookup so .humanoidrig.json / .profile.json /
    // .retargetmap.json / .anim.json resolve to their registered types
    // instead of collapsing to .json (Phase 16).
    return m_TypeRegistry.GetAssetTypeFromExtension(
        GetCompoundExtensionFromPath(normPath.string()));
}

bool AssetRegistry::RegisterAsset(const std::filesystem::path& path)
{
    // GetAssetRoot() takes a brief shared_lock + returns a copy, so the
    // path is safe to use without holding the registry lock.
    const std::filesystem::path projectRoot = GetAssetRoot();
    // normPath is the map-key form, case-folded on a case-insensitive file
    // system; spelledPath keeps the caller's spelling for the store record and
    // the name (the store folds at comparison, AssetPaths::FoldStorePathKey).
    const std::filesystem::path spelledPath = path.is_relative() ? (projectRoot / path) : path;
    const std::filesystem::path normPath = NormalizePathForMap(spelledPath);
    const std::string normKey = PathMapKey(normPath);

    // Fast path: if the asset is already registered at this path, skip all
    // filesystem syscalls (exists/is_directory/canonical-relative/mtime/hash)
    // and the full idempotent-update flow. Callers routinely re-register
    // known assets (during drag-drop, thumbnail preview, file-watcher events)
    // where this repeats per-call work that's a no-op in practice. In Debug
    // builds the filesystem syscalls alone dominate RegisterAsset latency.
    //
    // Edge cases where the slow path still matters (asset previously missing,
    // type changed, store/cache out of sync) are now handled when the file
    // watcher or explicit invalidation signals the change.
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (FindPathGuidLocked(normKey) != nullptr)
            return true;
        // F.1: reject mutations to immutable sources (e.g. Package mounts).
        // Trace (not Warning) — RegisterAsset is called speculatively by
        // probe-style code (drag-drop hover, "is this path an asset?"
        // lookups, file-watcher fan-out). A user-visible warning per probe
        // would flood the log; a Trace lets investigators find it without
        // cluttering normal sessions.
        if (const SourceEntry* src = FindSourceForPath(normPath); src && src->IsImmutable)
        {
            Logger::Log::Trace("AssetRegistry::RegisterAsset: source '{}' is immutable; "
                               "rejecting registration of '{}'",
                               src->Alias, normPath.string());
            return false;
        }
    }

    if (!std::filesystem::exists(normPath))
    {
        // Episodic: renderers and retry loops re-probe the same missing path
        // every frame; the first miss is the actionable ERROR, repeats are
        // noise. The episode clears when the file appears (below).
        std::lock_guard<std::mutex> logLock(m_MissingAssetLogMutex);
        if (m_MissingAssetLogEpisodes.insert(normKey).second)
        {
            Logger::Log::Error("Asset file does not exist: {} (repeat misses "
                               "for this path are suppressed until it appears)",
                               normPath.string());
        }
        return false;
    }

    // Recovery transition: the path sat in a missing-file episode and just
    // appeared — close the episode so a later disappearance logs again.
    {
        std::lock_guard<std::mutex> logLock(m_MissingAssetLogMutex);
        if (m_MissingAssetLogEpisodes.erase(normKey) > 0)
        {
            Logger::Log::Info("Asset file appeared: {} (missing-file episode cleared)",
                              normPath.string());
        }
    }

    // Directories are not assets; skip to avoid file_size() and other file-only operations.
    if (std::filesystem::is_directory(normPath))
    {
        Logger::Log::Trace("Skipping directory (not an asset): {}", normPath.string());
        return false;
    }

    // Skip ignored assets (derived/tool/build folders, etc.) to avoid polluting the project database.
    // Only apply ignore rules for assets that live under the configured project asset root.
    {
        std::string canonicalRel;
        if (AssetPaths::TryMakeCanonicalRelativePath(projectRoot, normPath, canonicalRel) && !canonicalRel.empty())
        {
            AssetIgnoreRules rules;
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                rules = ProjectIgnoreRules();
            }
            if (rules.ShouldIgnoreCanonicalRelativePath(canonicalRel))
            {
                Logger::Log::Trace("Skipping ignored asset file: {}", normPath.string());
                return false;
            }
        }
    }

    // Check if already registered
    if (IsAssetRegistered(normPath))
    {
        // Idempotent update: ensure store/cache are kept in sync (e.g. asset was previously missing
        // and reappeared, or type/extension changed).
        const GUID existingGuid = GetAssetGUID(normPath);
        (void)TryUpdateFilesystemMetadata(normPath);

        // Only persist assets that live under the configured asset root (project assets).
        // Mounted editor assets are tracked in-memory only to avoid polluting the project database.
        std::string canonicalRel;
        const bool persistent = AssetPaths::TryMakeCanonicalRelativePath(projectRoot, spelledPath, canonicalRel);
        if (!persistent)
        {
            // Best-effort: ensure the non-persistent store/cache sees this record when configured.
            SourceEntry* npSrc = nullptr;
            AssetDatabase::IAssetStore* npStore = nullptr;
            AssetDatabase::IAssetDbCache* npCache = nullptr;
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                npSrc = FindSourceForPath(normPath);
                if (npSrc && npSrc != m_ProjectSource.get() && npSrc->HasStore())
                {
                    npStore = npSrc->Store.get();
                    npCache = npSrc->Cache.get();
                }
                else
                {
                    npSrc = nullptr;
                }
            }
            if (npStore)
            {
                std::string key;
                GUID nsGuid;
                std::string sourceCanonicalRel;
                bool storedOwnerRefresh = false;
                const bool haveKey = [&]() {
                    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                    if (TryBuildSourceScopedKeyForPath(normPath, key, nsGuid, &sourceCanonicalRel))
                        return true;
                    // SEAM 4: mutable stored-identity owner (extracted package)
                    // — its rows refresh through the same path as derived mounts.
                    if (npSrc && !npSrc->DerivedIdentity && !npSrc->IsImmutable && !npSrc->IsReadOnly)
                    {
                        const std::string pathKey = PathMapKey(normPath);
                        if (PathKeyHasRootPrefix(pathKey, npSrc->RootPrefixKey))
                        {
                            sourceCanonicalRel = pathKey.substr(npSrc->RootPrefixKey.size());
                            storedOwnerRefresh = !sourceCanonicalRel.empty();
                        }
                    }
                    return false;
                }();

                if ((haveKey && !key.empty() && !sourceCanonicalRel.empty()) || storedOwnerRefresh)
                {
                    AssetMetadata md{};
                    if (TryGetAssetMetadata(existingGuid, md))
                    {
                        AssetDatabase::AssetObservation obs{};
                        obs.guid = existingGuid;
                        obs.path = sourceCanonicalRel;
                        obs.type = md.Type;
                        obs.typeId = md.TypeId;

                        // Skip the dirty-mark when nothing actually changed.
                        AssetDatabase::AssetRecord rec{};
                        if (npStore->MergeObservation(obs, rec, nullptr) ==
                            AssetDatabase::StoreMergeResult::Changed)
                        {
                            npSrc->StoreDirty.store(true, std::memory_order_relaxed);
                            if (npCache)
                            {
                                (void)npCache->UpsertAsset(rec, nullptr);
                            }
                        }
                    }
                }
            }

            Logger::Log::Trace("Asset already registered (non-persistent): {}", normPath.string());
            return true;
        }

        // canonicalRel already computed above when persistent==true.
        //
        // Block A.2/A.3 audit follow-up: pin the project source for the
        // duration of this idempotent-update block. Previously this code
        // called ProjectStore()/ProjectCache() without any lock — those raw
        // accessors deref m_ProjectSource and have an implicit "caller holds
        // the registry lock" contract. Pinning replaces the lock-based
        // lifetime guarantee with the SharedPtr's refcount; the store and
        // cache have their own internal mutexes for concurrent operations.
        SharedPtr<SourceEntry> pinned = ProjectSourcePinned();
        AssetDatabase::AssetRecord rec{};
        AssetDatabase::IAssetStore* pinnedStore = pinned ? pinned->Store.get() : nullptr;
        AssetDatabase::IAssetDbCache* pinnedCache = pinned ? pinned->Cache.get() : nullptr;

        if (pinnedStore)
        {
            AssetDatabase::AssetObservation obs{};
            obs.guid = existingGuid;
            obs.path = canonicalRel;
            // typeId left empty: the store derives it from the type it accepts,
            // so the two can't disagree.
            obs.type = ClassifyAssetType(normPath);

            // Only mark dirty if something actually changed. Without this, every
            // re-registration (file watcher events, repeated RegisterAsset calls
            // during previews/thumbnails) marks the store dirty and triggers an
            // O(N) flush on the throttle interval — even though no user data
            // changed.
            if (pinnedStore->MergeObservation(obs, rec, nullptr) == AssetDatabase::StoreMergeResult::Changed)
            {
                pinned->StoreDirty.store(true, std::memory_order_relaxed);
            }
        }

        if (pinnedCache)
        {
            (void)pinnedCache->UpsertAsset(rec, nullptr);

            int64_t size = 0;
            int64_t mtime = 0;
            if (TryGetFileStats(normPath, size, mtime))
            {
                const std::string fileId = ReadFileIdentity(normPath).ToString();

                // Phase 5 step 3: try snapshot-based hash reuse before
                // recomputing. snapshotByPath is mutated only by
                // SetupSourceStore under m_RegistryMutex writer lock; the
                // pin keeps the SourceEntry alive but doesn't synchronize
                // with that writer. Take a shared lock to read snapshotByPath
                // safely.
                //
                // Note: re-registration of an already-known asset takes the
                // fast-path at line ~2205 (m_PathToGuid hit) and never
                // reaches this code. The reuse path therefore only fires on
                // the FIRST registration of a path within a given session,
                // which is exactly when the snapshot's cached hash is most
                // valuable (avoids the cold rehash of the full asset tree
                // after a fresh editor launch).
                std::string cachedHash;
                int64_t cachedMtime = 0;
                int64_t cachedSize = 0;
                std::string cachedFileId;
                if (!canonicalRel.empty())
                {
                    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                    auto it = pinned->SnapshotByPath.find(AssetPaths::FoldStorePathKey(canonicalRel));
                    if (it != pinned->SnapshotByPath.end())
                    {
                        cachedHash = it->second.Hash;
                        cachedMtime = it->second.Mtime;
                        cachedSize = it->second.Size;
                        cachedFileId = it->second.FileId;
                    }
                }

                const std::string hash = ComputeOrReuseHash(
                    normPath, size, mtime, fileId,
                    cachedHash, cachedMtime, cachedSize, cachedFileId);
                (void)pinnedCache->UpdateFileFingerprint(existingGuid, mtime, size, hash, fileId, nullptr);
            }
        }

        Logger::Log::Trace("Asset already registered: {}", normPath.string());
        return true;
    }

    SourceEntry* npSrc = nullptr;
    AssetDatabase::IAssetStore* npStore = nullptr;
    AssetDatabase::IAssetDbCache* npCache = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        npSrc = FindSourceForPath(normPath);
        if (npSrc && npSrc != m_ProjectSource.get() && npSrc->HasStore())
        {
            npStore = npSrc->Store.get();
            npCache = npSrc->Cache.get();
        }
        else
        {
            npSrc = nullptr;
        }
    }

    AssetMetadata metadata;
    metadata.Path = normPath;

    // Classify asset type (parser sniffing when available, otherwise extension mapping).
    metadata.Type = ClassifyAssetType(normPath);
    metadata.TypeId = AssetTypeToString(metadata.Type);

    metadata.Name = spelledPath.stem().string();
    metadata.Extension = GetCompoundExtensionFromPath(normPath.string());
    metadata.LastModified = std::filesystem::last_write_time(normPath);
    metadata.FileSize = std::filesystem::file_size(normPath);

    // Resolve or create the GUID from the authoritative store / derived path id.
    std::string canonicalRel;
    const bool persistent = AssetPaths::TryMakeCanonicalRelativePath(projectRoot, spelledPath, canonicalRel);

    GUID guid = GUID::Null();
    AssetDatabase::AssetRecord rec{};

    std::string nonPersistentKey;
    GUID nonPersistentNs = GUID::Null();
    std::string nonPersistentCanonicalRel;
    // SEAM 4: true when the owning source is a MUTABLE stored-identity mount
    // (extracted package) — identity then comes from its store with a derived
    // fallback, instead of the derived-source key below.
    bool storedMutableOwner = false;
    const bool haveNonPersistentKey = [&]() {
        if (persistent)
            return false;
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (TryBuildSourceScopedKeyForPath(normPath, nonPersistentKey, nonPersistentNs, &nonPersistentCanonicalRel))
            return true;
        if (npSrc && !npSrc->DerivedIdentity && !npSrc->IsImmutable && !npSrc->IsReadOnly)
        {
            const std::string pathKey = PathMapKey(normPath);
            if (PathKeyHasRootPrefix(pathKey, npSrc->RootPrefixKey))
            {
                nonPersistentCanonicalRel = pathKey.substr(npSrc->RootPrefixKey.size());
                storedMutableOwner = !nonPersistentCanonicalRel.empty();
            }
        }
        return false;
    }();

    // Pin the project source for the entire persistent-write block. Without
    // this pin, ProjectStore() / ProjectCache() return raw pointers that race
    // a concurrent Shutdown — Shutdown's m_ProjectSource.reset() can null
    // the SourceEntry while we're mid-call into store/cache, dereferencing a
    // dangling mutex. Pinning bumps the SourceEntry's refcount so the
    // store/cache UniquePtrs (and their mutexes) stay alive for the scope.
    SharedPtr<SourceEntry> pinnedSource;
    AssetDatabase::IAssetStore* pinnedStore = nullptr;
    AssetDatabase::IAssetDbCache* pinnedCache = nullptr;
    if (persistent)
    {
        pinnedSource = ProjectSourcePinned();
        if (pinnedSource)
        {
            pinnedStore = pinnedSource->Store.get();
            pinnedCache = pinnedSource->Cache.get();
        }
    }

    // Persistent (project) assets: use authoritative store to preserve identity.
    if (persistent && pinnedStore && !canonicalRel.empty())
    {
        if (auto g = pinnedStore->LookupGuidByPath(canonicalRel))
        {
            guid = *g;
        }
    }

    bool guidWasMinted = false;
    if (guid.IsNull())
    {
        if (persistent)
        {
            // Derived-identity project source: the GUID is a deterministic
            // function of the canonical path, not a random mint, so it
            // reproduces the same GUID on every machine with no DB. Runs for the
            // project source (DerivedIdentity=true); the package mount is
            // DerivedIdentity=false and resolves stored GUIDs from the manifest.
            GUID derived = GUID::Null();
            if (pinnedSource && pinnedSource->DerivedIdentity && !canonicalRel.empty())
            {
                derived = DeriveSourceScopedGuid(pinnedSource->NamespaceGuid,
                                                 pinnedSource->Alias, canonicalRel);
            }
            guidWasMinted = derived.IsNull();
            guid = derived.IsNull() ? GUID::Generate() : derived;
        }
        else
        {
            // Non-persistent (mounted) assets: deterministically derive identity from a stable root-relative key.
            if (haveNonPersistentKey && !nonPersistentNs.IsNull() && !nonPersistentKey.empty())
            {
                guid = GUID::Derive(nonPersistentNs, nonPersistentKey);
            }
            else if (storedMutableOwner && npStore)
            {
                // SEAM 4 mutable stored-identity source (extracted package):
                // adopt the store row's GUID when the path is published or
                // previously persisted; otherwise mint the deterministic
                // derived fallback (decision table in PackageMounts.h).
                if (auto g = npStore->LookupGuidByPath(nonPersistentCanonicalRel); g && !g->IsNull())
                    guid = *g;
                else
                    guid = MintStoredSourceFallbackGuid(*npSrc, nonPersistentCanonicalRel);
            }
            else
            {
                // Fallback: still register in-memory, but do not persist outside-root assets.
                guid = GUID::Generate();
                guidWasMinted = true;
            }
        }
    }

    metadata.Guid = guid;

    // S10 atomic claim: a randomly-minted GUID is arbitrated through the
    // claim table BEFORE any store/cache write. Racing first registrations
    // of this path (the startup scan's GetOrCreateAssetGUID staging, another
    // RegisterAsset) converge on one winner here, so the store only ever
    // sees a single GUID per path and its last-writer-wins arbitration never
    // drops a row the maps still reference — the dual-mint window behind the
    // PinnedSource flake family.
    if (guidWasMinted)
    {
        std::unique_lock<std::shared_mutex> claimLock(m_RegistryMutex);
        if (!m_Initialized)
            return false;
        const GUID winner = ClaimPathGuidLocked(normKey, guid);
        if (winner != guid)
        {
            guid = winner;
            metadata.Guid = winner;
        }
    }

    AssetDatabase::AssetObservation obs{};
    obs.guid = guid;
    obs.path = canonicalRel;
    obs.type = metadata.Type;
    obs.typeId = metadata.TypeId;

    // Stands in for the merged row where there is no store to merge against;
    // the cache writes below mirror whichever this ends up holding.
    rec.guid = guid;
    rec.path = canonicalRel;
    rec.type = metadata.Type;
    rec.typeId = metadata.TypeId;
    rec.missing = false;

    if (persistent && pinnedStore)
    {
        // Merged, not overwritten: this path reaches a file another registrar
        // may already have classified, and a scan that cannot classify it must
        // not erase that answer. The merged row is what the cache mirrors.
        (void)pinnedStore->MergeObservation(obs, rec, nullptr);
        pinnedSource->StoreDirty.store(true, std::memory_order_relaxed);

        // Write-time identity confirm, mirroring RegisterAssetMetadataBatch's
        // pass-4 re-check: a concurrent registrar of the same path (startup
        // scan batch) can land between the LookupGuidByPath above and this
        // upsert. The store arbitrates path ownership last-writer-wins and
        // drops the loser's row, so re-read the surviving GUID and adopt it —
        // the cache writes below and the map commit must key the row that
        // actually exists in the store.
        if (!pinnedSource->DerivedIdentity && !canonicalRel.empty())
        {
            if (auto winner = pinnedStore->LookupGuidByPath(canonicalRel);
                winner && !winner->IsNull() && *winner != guid)
            {
                guid = *winner;
                metadata.Guid = guid;
                rec.guid = guid;
            }
        }
    }

    if (persistent && pinnedCache)
    {
        (void)pinnedCache->UpsertAsset(rec, nullptr);

        int64_t size = static_cast<int64_t>(metadata.FileSize);
        const int64_t mtime = FileTimeToInt64(metadata.LastModified);
        const std::string fileId = ReadFileIdentity(normPath).ToString();

        // Phase 5 step 3: snapshot hash reuse on the new-asset path too.
        // snapshotByPath is mutated only under m_RegistryMutex writer lock,
        // so we still need the shared lock to read it safely (the pin keeps
        // the SourceEntry alive but doesn't synchronize with that writer).
        std::string cachedHash;
        int64_t cachedMtime = 0;
        int64_t cachedSize = 0;
        std::string cachedFileId;
        if (!canonicalRel.empty())
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            auto it = pinnedSource->SnapshotByPath.find(AssetPaths::FoldStorePathKey(canonicalRel));
            if (it != pinnedSource->SnapshotByPath.end())
            {
                cachedHash = it->second.Hash;
                cachedMtime = it->second.Mtime;
                cachedSize = it->second.Size;
                cachedFileId = it->second.FileId;
            }
        }

        const std::string hash = ComputeOrReuseHash(
            normPath, size, mtime, fileId,
            cachedHash, cachedMtime, cachedSize, cachedFileId);
        (void)pinnedCache->UpdateFileFingerprint(guid, mtime, size, hash, fileId, nullptr);
    }

    // Optional: persist non-persistent assets into the separate local store/cache (Editor/local), never into the project DB.
    if (!persistent && npStore &&
        ((haveNonPersistentKey && !nonPersistentKey.empty()) || storedMutableOwner) &&
        !nonPersistentCanonicalRel.empty())
    {
        AssetDatabase::AssetObservation npObs = obs;
        npObs.path = nonPersistentCanonicalRel;

        AssetDatabase::AssetRecord nprec{};
        (void)npStore->MergeObservation(npObs, nprec, nullptr);
        npSrc->StoreDirty.store(true, std::memory_order_relaxed);
        if (npCache)
        {
            (void)npCache->UpsertAsset(nprec, nullptr);
        }
    }

    GUID healedRemapTo = GUID::Null();
    std::filesystem::path healedRemapPath;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        // Another thread may have registered this asset while we were parsing
        // metadata. If it committed the same GUID we adopted, this is a plain
        // idempotent re-register. Otherwise converge the maps on our resolved
        // GUID (with the S10 claim, a disagreement here means a deliberate
        // rebind — relink/reconcile — landed mid-flight; racing fresh mints
        // already converged at the claim).
        if (const GUID* existingBinding = FindPathGuidLocked(normKey))
        {
            m_PathClaims.erase(normKey);
            if (*existingBinding == metadata.Guid)
                return true;
            ptrdiff_t occupancyDelta = 0;
            const GUID staleGuid = *existingBinding;
            if (const AssetMetadata* stale = FindAssetLocked(staleGuid);
                stale && NormalizePathForMap(stale->Path) == normPath)
            {
                UnindexResidentGuidLocked(staleGuid);
                if (EraseAssetEntryLocked(staleGuid))
                    --occupancyDelta;
            }
            if (AssetShard(metadata.Guid).insert_or_assign(metadata.Guid, metadata).second)
                ++occupancyDelta;
            PathShard(normKey)[normKey] = metadata.Guid;
            IndexResidentGuidLocked(metadata.Guid, normPath);
            ApplyAssetCountDelta(occupancyDelta);
            RetireStaleAliasesOnRegistrationLocked(metadata.Guid);
            return true;
        }

        // Derive-collision guard (DerivedIdentity sources only). Under derived
        // identity the GUID is a hash of the path, so an occupied GUID for a
        // DISTINCT canonical path means one of two things. A file re-created at
        // a path whose derived GUID rode along with an earlier rename is the
        // benign shape — healed below by migrating the renamed asset onto its
        // own path-derived GUID. Anything else (two live paths whose canonical
        // forms hash equal) is a hard correctness failure — proceeding would
        // silently overwrite the first's metadata. (The same-path
        // re-registration case is an update and already returned above via the
        // m_PathToGuid hit, so this never fires on a legitimate re-register.)
        // Active for the project source (DerivedIdentity=true).
        //
        // Refuse the genuine collision loudly; never assert here. The condition
        // is data-driven (on-disk state and registration history), and this
        // runs on the file-watcher thread while m_RegistryMutex is held
        // exclusive — a debug abort's modal dialog would park every thread
        // that touches the registry on this lock forever.
        if (persistent && pinnedSource && pinnedSource->DerivedIdentity)
        {
            // Primary probe: a GUID whose only reachability is an S10c alias
            // (the E5 reassign already migrated the renamed asset) is free —
            // this registration shadows the alias, exactly as the alias
            // machinery intends; an alias-chasing lookup would report that
            // shape as occupied and refuse a healthy registration.
            if (const AssetMetadata* existing = FindAssetPrimaryLocked(metadata.Guid);
                existing &&
                !existing->Path.empty() &&
                existing->Path != normPath)
            {
                // Rename-kept identity: a live rename left this GUID on the
                // renamed asset (now at another path), so the path that
                // actually derives it can't register. Migrate that asset onto
                // its own path-derived GUID and continue; the
                // RetireStaleAliasesOnRegistrationLocked below then retires
                // the rename redirect — the path speaks for itself again.
                healedRemapPath = existing->Path;
                if (!TryHealRenameKeptIdentityLocked(metadata.Guid, *existing, *pinnedSource,
                                                     projectRoot, healedRemapTo))
                {
                    healedRemapPath.clear();
                    Logger::Log::Error(
                        "AssetRegistry: derive collision under derived-identity source '{}' — "
                        "distinct paths derive the same GUID {}: existing='{}' new='{}'. "
                        "Asset identity is corrupt; rename one of the files.",
                        pinnedSource->Alias,
                        metadata.Guid.ToString(),
                        existing->Path.string(),
                        normPath.string());
                    return false;
                }
            }
        }

        if (AssetShard(metadata.Guid).insert_or_assign(metadata.Guid, metadata).second)
            ApplyAssetCountDelta(1);
        PathShard(normKey)[normKey] = metadata.Guid;
        IndexResidentGuidLocked(metadata.Guid, normPath);
        m_PathClaims.erase(normKey);
        RetireStaleAliasesOnRegistrationLocked(metadata.Guid);
    }

    // E5-symmetric: notify the manager about the heal's remap AFTER the maps
    // are consistent and OUTSIDE the writer lock, so it can atomically re-key
    // its loaded/in-flight maps before anything loads the registrant's GUID.
    if (!healedRemapTo.IsNull())
    {
        GuidRemapCallback callback;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            callback = m_GuidRemapCallback;
        }
        if (callback)
        {
            callback(metadata.Guid, healedRemapTo, healedRemapPath);
        }
    }

    Logger::Log::Trace("Registered asset: {} ({})", metadata.Name, metadata.Guid.ToString());
    EnsureFbxMirrorAxesMeta(normPath);
    return true;
}

// ------------------------------------------------------------------
// RegisterAsset with explicit source ownership
// ------------------------------------------------------------------

bool AssetRegistry::RegisterAsset(const std::filesystem::path& path, std::string_view preferredSourceAlias)
{
    const std::string normalizedAlias = NormalizeRegistryAssetSourceAlias(preferredSourceAlias);

    // Empty or invalid alias: fall back to the base overload.
    if (normalizedAlias.empty() || !IsValidRegistryAssetSourceAlias(normalizedAlias))
    {
        return RegisterAsset(path);
    }

    // Ensure the asset is registered (may create it with a project GUID first).
    if (!RegisterAsset(path))
        return false;

    const std::filesystem::path projectRoot = GetAssetRoot();
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (projectRoot / path) : path);
    const std::string normKey = PathMapKey(normPath);

    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

    // Find the preferred source.
    SourceEntry* src = FindSourceByAlias(normalizedAlias);
    if (!src)
    {
        // Unknown source -- just record ownership with current GUID.
        m_AssetSourceOwner[normKey] = normalizedAlias;
        if (const GUID* boundForOwner = FindPathGuidLocked(normKey))
            MoveResidentGuidToOwnerLocked(*boundForOwner, normalizedAlias);
        return true;
    }

    // For stored-identity sources (e.g. "project"), keep the current GUID and
    // just record ownership.
    if (!src->DerivedIdentity)
    {
        m_AssetSourceOwner[normKey] = normalizedAlias;
        if (const GUID* boundForOwner = FindPathGuidLocked(normKey))
            MoveResidentGuidToOwnerLocked(*boundForOwner, normalizedAlias);
        return true;
    }

    // Derived-identity source: compute the deterministic GUID.
    std::string canonicalRel;
    if (!AssetPaths::TryMakeCanonicalRelativePath(src->Root, normPath, canonicalRel))
    {
        // Path not under this source's root -- keep current GUID.
        m_AssetSourceOwner[normKey] = normalizedAlias;
        if (const GUID* boundForOwner = FindPathGuidLocked(normKey))
            MoveResidentGuidToOwnerLocked(*boundForOwner, normalizedAlias);
        return true;
    }

    const std::string key = BuildRegistrySourceScopedKey(normalizedAlias, canonicalRel);
    if (key.empty() || src->NamespaceGuid.IsNull())
    {
        m_AssetSourceOwner[normKey] = normalizedAlias;
        if (const GUID* boundForOwner = FindPathGuidLocked(normKey))
            MoveResidentGuidToOwnerLocked(*boundForOwner, normalizedAlias);
        return true;
    }

    const GUID derivedGuid = GUID::Derive(src->NamespaceGuid, key);
    GUID remapOld = GUID::Null();

    // Case-only-different file collision (Linux only — case-insensitive
    // filesystems on Windows/macOS can't host both files in the first
    // place). Two files like "Assets/Knight.fbx" and "Assets/knight.fbx"
    // produce different m_PathToGuid keys (real case preserved on Linux)
    // but the same derived GUID (NormalizeForRegistryKey case-folds at
    // the derive site for cross-platform stability). The second
    // registration would silently overwrite the first; warn so the
    // author can rename before the project hits a case-insensitive fs.
    if (const AssetMetadata* mdCheck = FindAssetLocked(derivedGuid);
        mdCheck && !mdCheck->Path.empty() && mdCheck->Path != normPath)
    {
        Logger::Log::Warning(
            "AssetRegistry: case-only-different files derive the same GUID under "
            "derived-identity source '{}': existing='{}' new='{}'. Rename one — "
            "this project won't open correctly on case-insensitive filesystems "
            "(Windows/macOS).",
            normalizedAlias,
            mdCheck->Path.string(),
            normPath.string());
    }

    if (const GUID* boundGuid = FindPathGuidLocked(normKey))
    {
        const GUID existingGuid = *boundGuid;

        if (existingGuid == derivedGuid)
        {
            // Already the correct derived GUID.
            m_AssetSourceOwner[normKey] = normalizedAlias;
            MoveResidentGuidToOwnerLocked(existingGuid, normalizedAlias);
            return true;
        }

        // Remap: the asset was registered under a different GUID (typically the
        // project's stored identity).  Move the metadata to the derived GUID so
        // every downstream consumer (loaded-asset cache, UIHotReload bindings,
        // file-watcher events) agrees on a single stable identity.
        if (const AssetMetadata* existingMd = FindAssetLocked(existingGuid))
        {
            AssetMetadata md = *existingMd;
            md.Guid = derivedGuid;
            ptrdiff_t occupancyDelta = 0;
            UnindexResidentGuidLocked(existingGuid);
            if (EraseAssetEntryLocked(existingGuid))
                --occupancyDelta;
            ErasePathMappingLocked(normKey);
            if (AssetShard(derivedGuid).insert_or_assign(derivedGuid, md).second)
                ++occupancyDelta;
            ApplyAssetCountDelta(occupancyDelta);
            PathShard(normKey)[normKey] = derivedGuid;
            // S10c: record the alias in the SAME writer section as the map
            // rewrite, so a caller holding the pre-remap GUID never observes
            // an unresolvable window (the overlapping-source load-null race).
            // Drop any inverse alias first — alternating re-claims between
            // two overlapping sources must not form a lookup cycle.
            m_GuidRemaps.erase(derivedGuid);
            m_GuidRemaps[existingGuid] = derivedGuid;

            // Persist into the source's local store / cache if configured.
            if (src->HasStore())
            {
                AssetDatabase::AssetRecord rec{};
                rec.guid = derivedGuid;
                rec.path = canonicalRel;
                rec.type = md.Type;
                rec.typeId = md.TypeId;
                rec.missing = false;
                (void)src->Store->UpsertAsset(rec, nullptr);
                src->StoreDirty.store(true, std::memory_order_relaxed);
                if (src->Cache)
                {
                    (void)src->Cache->UpsertAsset(rec, nullptr);
                }
            }

            Logger::Log::Info(
                "AssetRegistry: reassigned '{}' from {} to derived {} (source '{}')",
                normPath.string(),
                existingGuid.ToString(),
                derivedGuid.ToString(),
                normalizedAlias);
            remapOld = existingGuid;
        }
    }

    m_AssetSourceOwner[normKey] = normalizedAlias;
    if (const GUID* boundForOwner = FindPathGuidLocked(normKey))
        MoveResidentGuidToOwnerLocked(*boundForOwner, normalizedAlias);

    // E5: notify the manager AFTER the maps are consistent and OUTSIDE the
    // writer lock, so it can atomically re-key its loaded/in-flight maps.
    if (!remapOld.IsNull())
    {
        writeLock.unlock();
        GuidRemapCallback callback;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            callback = m_GuidRemapCallback;
        }
        if (callback)
        {
            callback(remapOld, derivedGuid, normPath);
        }
    }
    return true;
}

AssetRegistry::LockedRef<AssetDatabase::IAssetStore>
AssetRegistry::ProjectStoreLocked() const
{
    std::shared_lock<std::shared_mutex> lock(m_RegistryMutex);
    AssetDatabase::IAssetStore* ptr = m_ProjectSource ? m_ProjectSource->Store.get() : nullptr;
    return LockedRef<AssetDatabase::IAssetStore>(std::move(lock), ptr);
}

AssetRegistry::LockedRef<AssetDatabase::IAssetDbCache>
AssetRegistry::ProjectCacheLocked() const
{
    std::shared_lock<std::shared_mutex> lock(m_RegistryMutex);
    AssetDatabase::IAssetDbCache* ptr = m_ProjectSource ? m_ProjectSource->Cache.get() : nullptr;
    return LockedRef<AssetDatabase::IAssetDbCache>(std::move(lock), ptr);
}

size_t AssetRegistry::GetSnapshotRecordCount(std::string_view sourceAlias) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const auto* entry = FindSourceByAlias(sourceAlias);
    if (!entry)
        return 0;
    return entry->SnapshotByPath.size();
}

size_t AssetRegistry::GetLastReconcileHashesReused(std::string_view sourceAlias) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const auto* entry = FindSourceByAlias(sourceAlias);
    if (!entry)
        return 0;
    return entry->LastReconcileHashesReused;
}

bool AssetRegistry::TryComputeCanonicalRelativePath(const std::filesystem::path& assetRoot,
                                                    const std::filesystem::path& assetPath,
                                                    std::string& outCanonical)
{
    return AssetPaths::TryMakeCanonicalRelativePath(assetRoot, assetPath, outCanonical);
}

std::unordered_map<std::string, AssetRegistry::SnapshotFingerprint>
AssetRegistry::CopySnapshotByPathForRoot(const std::filesystem::path& directory) const
{
    const std::filesystem::path normDir = NormalizePathForMap(directory);
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    for (const auto& src : m_Sources)
    {
        if (NormalizePathForMap(src->Root) == normDir && !src->SnapshotByPath.empty())
        {
            // Single copy under shared lock; callers can use it lock-free.
            return src->SnapshotByPath;
        }
    }
    return {};
}

std::unordered_map<std::string, int64_t>
AssetRegistry::CopyDirectoryMtimesByPathForRoot(const std::filesystem::path& directory) const
{
    const std::filesystem::path normDir = NormalizePathForMap(directory);
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    for (const auto& src : m_Sources)
    {
        if (NormalizePathForMap(src->Root) == normDir && !src->SnapshotDirMtimeByPath.empty())
        {
            return src->SnapshotDirMtimeByPath;
        }
    }
    return {};
}

Result<GUID, AssetError> AssetRegistry::RegisterAssetByPath(const std::filesystem::path& path)
{
    // Wrapper around the bool RegisterAsset + GUID resolution sequence so
    // callers handle the failure shape via Result instead of having to
    // inspect a bool plus a possibly-null GUID. Used by paste-path / drag-drop /
    // CLI cook flows where the caller wants a single Try-step API.
    if (!RegisterAsset(path))
        return AssetError::MountUnavailable;

    const GUID guid = GetAssetGUID(path);
    if (guid.IsNull())
        return AssetError::Missing;

    return guid;
}

// ------------------------------------------------------------------
// S10 overlay-aware accessors (contracts in AssetRegistry.h).
// ------------------------------------------------------------------

// Chain cap for S10c remap aliases: overlap re-claims are one hop in
// practice (project GUID -> derived editor GUID); the cap only guards a
// pathological alias cycle from ever spinning a lookup.
static constexpr int kGuidRemapMaxChase = 4;

const GUID* AssetRegistry::FindPathGuidLocked(std::string_view pathKey) const
{
    // Overlays win path conflicts: a just-published mount's binding shadows
    // any pre-mount binding for the same path, matching the eviction order
    // its drain will commit.
    if (!m_MountOverlays.empty())
    {
        for (const auto& overlay : m_MountOverlays)
        {
            if (auto it = overlay->PathToGuid.find(pathKey); it != overlay->PathToGuid.end())
                return &it->second;
        }
    }
    const PathShardMap& shard = PathShard(pathKey);
    auto it = shard.find(pathKey);
    return (it != shard.end()) ? &it->second : nullptr;
}

const AssetMetadata* AssetRegistry::FindAssetLocked(const GUID& guid) const
{
    // Primary probes first; the S10c remap-alias chase below runs only on a
    // total miss, so direct hits never pay for it.
    GUID current = guid;
    for (int depth = 0; depth < kGuidRemapMaxChase; ++depth)
    {
        const AssetShardMap& shard = AssetShard(current);
        if (auto it = shard.find(current); it != shard.end())
            return &it->second;
        for (const auto& overlay : m_MountOverlays)
        {
            if (auto ovIt = overlay->Assets.find(current); ovIt != overlay->Assets.end())
                return &ovIt->second;
        }
        if (m_GuidRemaps.empty())
            return nullptr;
        auto remapIt = m_GuidRemaps.find(current);
        if (remapIt == m_GuidRemaps.end() || remapIt->second == current)
            return nullptr;
        current = remapIt->second;
    }
    return nullptr;
}

const AssetMetadata* AssetRegistry::FindAssetPrimaryLocked(const GUID& guid) const
{
    const AssetShardMap& shard = AssetShard(guid);
    if (auto it = shard.find(guid); it != shard.end())
        return &it->second;
    for (const auto& overlay : m_MountOverlays)
    {
        if (auto ovIt = overlay->Assets.find(guid); ovIt != overlay->Assets.end())
            return &ovIt->second;
    }
    return nullptr;
}

AssetMetadata* AssetRegistry::FindAssetMutableLocked(const GUID& guid)
{
    GUID current = guid;
    for (int depth = 0; depth < kGuidRemapMaxChase; ++depth)
    {
        AssetShardMap& shard = AssetShard(current);
        if (auto it = shard.find(current); it != shard.end())
            return &it->second;
        for (const auto& overlay : m_MountOverlays)
        {
            if (auto ovIt = overlay->Assets.find(current); ovIt != overlay->Assets.end())
                return &ovIt->second;
        }
        if (m_GuidRemaps.empty())
            return nullptr;
        auto remapIt = m_GuidRemaps.find(current);
        if (remapIt == m_GuidRemaps.end() || remapIt->second == current)
            return nullptr;
        current = remapIt->second;
    }
    return nullptr;
}

bool AssetRegistry::ErasePathMappingLocked(std::string_view pathKey)
{
    // Erase from every layer: a conflicted path can be bound in an overlay
    // AND (shadowed) in the global map; unregistering it must not let the
    // shadowed binding resurface.
    bool erased = false;
    for (const auto& overlay : m_MountOverlays)
    {
        if (auto it = overlay->PathToGuid.find(pathKey); it != overlay->PathToGuid.end())
        {
            overlay->PathToGuid.erase(it);
            erased = true;
        }
    }
    PathShardMap& shard = PathShard(pathKey);
    if (auto it = shard.find(pathKey); it != shard.end())
    {
        shard.erase(it);
        erased = true;
    }
    return erased;
}

bool AssetRegistry::EraseAssetEntryLocked(const GUID& guid)
{
    if (AssetShard(guid).erase(guid) > 0)
        return true;
    for (const auto& overlay : m_MountOverlays)
    {
        if (overlay->Assets.erase(guid) > 0)
            return true;
    }
    return false;
}

GUID AssetRegistry::ClaimPathGuidLocked(const std::string& pathKey, const GUID& candidate)
{
    if (const GUID* registered = FindPathGuidLocked(pathKey))
        return *registered;
    if (auto it = m_PathClaims.find(pathKey); it != m_PathClaims.end())
        return it->second;
    m_PathClaims.emplace(pathKey, candidate);
    return candidate;
}

void AssetRegistry::DiscardOverlayForAliasLocked(std::string_view alias)
{
    std::erase_if(m_MountOverlays, [&](const UniquePtr<MountOverlay>& overlay)
                  {
                      if (overlay->SourceAlias != alias)
                          return false;
                      // S12: dropped overlay records leave the counted union.
                      ApplyAssetCountDelta(-static_cast<ptrdiff_t>(overlay->Assets.size()));
                      return true;
                  });
}

// ------------------------------------------------------------------
// S11 per-source resident-GUID index (contracts in AssetRegistry.h).
// ------------------------------------------------------------------

void AssetRegistry::IndexResidentGuidLocked(const GUID& guid, const std::filesystem::path& path)
{
    // Attribution mirrors the eviction rule exactly: an explicit owner alias
    // wins; otherwise the highest-priority source whose root contains the
    // path. Records outside every source (and ownerless) are not indexed —
    // no unmount ever targeted them.
    SourceEntry* attributed = nullptr;
    auto ownerIt = m_AssetSourceOwner.find(PathMapKey(path));
    if (ownerIt != m_AssetSourceOwner.end())
        attributed = FindSourceByAlias(ownerIt->second);
    else
        attributed = FindSourceForPath(path);
    if (attributed)
        attributed->ResidentGuids.insert(guid);
}

void AssetRegistry::UnindexResidentGuidLocked(const GUID& guid)
{
    // Search the (few) sources' sets directly instead of re-deriving
    // attribution: a source mounted/unmounted or an owner recorded between
    // insert and erase would otherwise strand a stale membership.
    for (const auto& src : m_Sources)
    {
        if (src->ResidentGuids.erase(guid) > 0)
            return;
    }
}

void AssetRegistry::MoveResidentGuidToOwnerLocked(const GUID& guid, std::string_view ownerAlias)
{
    UnindexResidentGuidLocked(guid);
    if (SourceEntry* owner = FindSourceByAlias(ownerAlias))
        owner->ResidentGuids.insert(guid);
}

AssetRegistry::DrainStats AssetRegistry::EvictSourceRecordsPaced(FastHashSet<GUID>&& guids,
                                                                 std::string_view overlaySkipAlias)
{
    DrainStats stats;
    if (guids.empty())
        return stats;

    constexpr auto kEvictSectionBudget = std::chrono::microseconds(300);

    auto it = guids.begin();
    for (;;)
    {
        {
            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
            if (!m_Initialized)
                return stats; // Shutdown cleared the maps wholesale.

            // Re-find the skip overlay each section (drains/discards move it).
            const MountOverlay* skipOverlay = nullptr;
            if (!overlaySkipAlias.empty())
            {
                for (const auto& candidate : m_MountOverlays)
                {
                    if (candidate->SourceAlias == overlaySkipAlias)
                    {
                        skipOverlay = candidate.get();
                        break;
                    }
                }
            }

            const auto sectionStart = std::chrono::steady_clock::now();
            const auto deadline = sectionStart + kEvictSectionBudget;
            ptrdiff_t sectionDelta = 0;
            while (it != guids.end())
            {
                // Deadline FIRST, so skip-heavy passes (a shared-identity
                // rebind skips every overlay-resident GUID) stay budget-boxed
                // too — a loop-bottom check after `continue` paths measured
                // as one unbounded 26 ms section at 100k.
                if (std::chrono::steady_clock::now() >= deadline)
                    break;

                const GUID guid = *it;
                ++it;

                // Shared-identity rebind: the replacement record is already
                // published in the overlay; its drain refreshes the global
                // record in place. Evicting here would open a resolution gap
                // for a GUID that never actually left the source.
                if (skipOverlay && skipOverlay->Assets.find(guid) != skipOverlay->Assets.end())
                    continue;

                AssetShardMap& shard = AssetShard(guid);
                auto recIt = shard.find(guid);
                if (recIt == shard.end())
                    continue; // already gone (renamed away, unregistered)

                const std::string pathKey = PathMapKey(recIt->second.Path);
                m_AssetSourceOwner.erase(pathKey);
                PathShardMap& pathShard = PathShard(pathKey);
                if (auto bindIt = pathShard.find(pathKey);
                    bindIt != pathShard.end() && bindIt->second == guid)
                {
                    pathShard.erase(bindIt);
                }
                shard.erase(guid);
                --sectionDelta;
                UnindexResidentGuidLocked(guid); // no-op for moved-out sets; safety for stragglers
            }
            ApplyAssetCountDelta(sectionDelta);

            ++stats.Sections;
            const double sectionMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sectionStart)
                    .count();
            stats.MaxSectionMs = std::max(stats.MaxSectionMs, sectionMs);
            if (sectionMs > 1.0)
                ++stats.SectionsOverMs;

            if (it == guids.end())
                return stats;
        }
        // Same reader-window pacing as DrainMountOverlay: SRWLOCK parks new
        // shared acquisitions while a writer waits, so back-to-back
        // re-acquisition would starve readers for the whole eviction.
        const auto resumeAt = std::chrono::steady_clock::now() + kEvictSectionBudget;
        while (std::chrono::steady_clock::now() < resumeAt)
            std::this_thread::yield();
    }
}

void AssetRegistry::PurgeClaimsUnderPrefixLocked(std::string_view rootPrefixKey)
{
    if (rootPrefixKey.empty() || m_PathClaims.empty())
        return;
    std::vector<std::string> toErase;
    for (const auto& [key, guid] : m_PathClaims)
    {
        if (PathKeyHasRootPrefix(key, rootPrefixKey))
            toErase.push_back(key);
    }
    for (const auto& key : toErase)
        m_PathClaims.erase(key);
}

void AssetRegistry::RetireStaleAliasesOnRegistrationLocked(const GUID& guid)
{
    if (guid.IsNull())
        return;
    // The session alias is consulted only after the primary probe misses, so
    // this registration shadows it already; erasing it keeps the alias from
    // resurfacing — and answering with the migrated asset — once the
    // registration is unregistered again.
    if (!m_GuidRemaps.empty())
        m_GuidRemaps.erase(guid);

    // TryGetAssetMetadata prefers a redirect's chased target over a direct
    // record whenever the target record is live, so a redirect whose source
    // GUID just (re)registered would permanently shadow the new file.
    // Registration means the path speaks for itself again — remove the
    // source's outgoing redirect. When the chased target has NO live record
    // the redirect is kept: that shape is load-bearing (live-rename residue
    // and derived→stable identity redirects resolve old references forward),
    // and the lookup falls through to the live source record anyway.
    if (!m_ProjectHasRedirects.load(std::memory_order_relaxed))
        return;
    AssetDatabase::IAssetStore* store = ProjectStore();
    if (!store)
        return;
    const AssetDatabase::RedirectChain chain = AssetDatabase::ChaseRedirectChain(
        *store, guid, AssetDatabase::RedirectTargetCheck::ResidentRecord,
        [this](const GUID& target) { return FindAssetLocked(target) != nullptr; });
    if (!chain.TargetAccepted)
        return;

    // The container's cascades die with it: the chain-final target record
    // carries the container's journaled derive keys (kv-migrated by the heal),
    // and each Derive(guid, key) redirect shadows the re-registered container's
    // own subasset identities exactly like the container redirect shadowed its
    // file.
    std::vector<GUID> victims{guid};
    AssetDatabase::AssetRecord targetRec{};
    if (store->TryGetAsset(chain.Final, targetRec))
    {
        if (auto it = targetRec.kv.find(kSubassetDeriveKeysKvKey); it != targetRec.kv.end())
        {
            for (const String& key : SplitSubassetDeriveKeys(it->second))
                victims.push_back(GUID::Derive(guid, key));
        }
    }

    // The rename direction outlives the removal: references that reached this
    // GUID inherit its chain-final target rather than binding to the unrelated
    // file that just claimed the GUID.
    const size_t retargeted = AssetDatabase::RetargetIncomingRedirects(*store, victims);
    size_t removed = 0;
    for (const GUID& victim : victims)
    {
        if (store->RemoveRedirect(victim, nullptr))
            ++removed;
    }
    if (retargeted == 0 && removed == 0)
        return;

    RefreshProjectRedirectFlagLocked();
    if (m_ProjectSource)
        m_ProjectSource->StoreDirty.store(true, std::memory_order_relaxed);
    Logger::Log::Info(
        "AssetRegistry: removed stale redirect from {} - the GUID re-registered and speaks for "
        "itself again; {} incoming hop(s) retargeted to {}",
        guid.ToString(), retargeted, chain.Final.ToString());
}

bool AssetRegistry::TryHealRenameKeptIdentityLocked(const GUID& collidingGuid,
                                                    const AssetMetadata& holder,
                                                    SourceEntry& source,
                                                    const std::filesystem::path& projectRoot,
                                                    GUID& outMigratedTo)
{
    // Copy first: erasing the colliding entry below invalidates `holder`.
    AssetMetadata md = holder;

    std::string canonicalHolder;
    if (!AssetPaths::TryMakeCanonicalRelativePath(projectRoot, md.Path, canonicalHolder))
        return false;
    const GUID holderDerived =
        DeriveSourceScopedGuid(source.NamespaceGuid, source.Alias, canonicalHolder);
    // Equal derives for distinct paths (case-only siblings on Linux) or an
    // occupied target identity are genuine collisions — refuse upstream.
    // Primary probe: an alias at the target is dropped below, not occupancy.
    if (holderDerived.IsNull() || holderDerived == collidingGuid ||
        FindAssetPrimaryLocked(holderDerived))
        return false;

    md.Guid = holderDerived;
    const std::string holderKey = PathMapKey(md.Path);

    ptrdiff_t occupancyDelta = 0;
    UnindexResidentGuidLocked(collidingGuid);
    if (EraseAssetEntryLocked(collidingGuid))
        --occupancyDelta;
    if (AssetShard(holderDerived).insert_or_assign(holderDerived, md).second)
        ++occupancyDelta;
    ApplyAssetCountDelta(occupancyDelta);
    PathShard(holderKey)[holderKey] = holderDerived;
    IndexResidentGuidLocked(holderDerived, md.Path);

    // No session alias from the colliding GUID: the registrant claims it as
    // a primary in the same writer section, which retires any alias it
    // carries. An alias FROM the migrated asset's new identity would be
    // stale the moment it becomes a primary.
    m_GuidRemaps.erase(holderDerived);

    // Store: the migrated asset's row moves to its path-derived identity,
    // carrying its kv whole (subasset derive keys included — the stale-redirect
    // clearing reads them from the chain-final record, and the offline heal's
    // completion sweep expects them there). The colliding GUID's row already
    // describes the registrant's path (MergeObservation ran before the writer
    // section); the migrated asset's kv leaves it.
    if (source.HasStore())
    {
        AssetDatabase::AssetRecord oldRec{};
        const bool hadOldRec = source.Store->TryGetAsset(collidingGuid, oldRec);

        AssetDatabase::AssetRecord holderRec{};
        holderRec.guid = holderDerived;
        holderRec.path = canonicalHolder;
        holderRec.type = md.Type;
        holderRec.typeId = md.TypeId;
        holderRec.missing = false;
        if (hadOldRec)
            holderRec.kv = oldRec.kv;
        (void)source.Store->UpsertAsset(holderRec, nullptr);
        if (source.Cache)
            (void)source.Cache->UpsertAsset(holderRec, nullptr);

        if (hadOldRec && !oldRec.kv.empty())
        {
            oldRec.kv.clear();
            (void)source.Store->UpsertAsset(oldRec, nullptr);
            if (source.Cache)
                (void)source.Cache->UpsertAsset(oldRec, nullptr);
        }
        source.StoreDirty.store(true, std::memory_order_relaxed);
    }

    Logger::Log::Info(
        "AssetRegistry: rename-kept identity {} released for re-registration; the renamed "
        "asset at '{}' now owns its path-derived GUID {}",
        collidingGuid.ToString(), md.Path.string(), holderDerived.ToString());
    outMigratedTo = holderDerived;
    return true;
}

void AssetRegistry::CommitMetadataToMapsLocked(const AssetMetadata& md)
{
    // S10: a record touched while it still sits in a published-but-undrained
    // overlay is drained early — extract the overlay's version, land it in
    // the global maps, then apply the incoming update on top so the refresh
    // semantics below see it as an existing record. S12: the overlay erase
    // and both commits fold into ONE counter application so lock-free
    // readers never see an intra-record dip.
    ptrdiff_t occupancyDelta = 0;
    if (!m_MountOverlays.empty())
    {
        for (const auto& overlay : m_MountOverlays)
        {
            auto ovIt = overlay->Assets.find(md.Guid);
            if (ovIt == overlay->Assets.end())
                continue;
            AssetMetadata promoted = std::move(ovIt->second);
            const std::string promotedKey = PathMapKey(promoted.Path);
            if (auto pIt = overlay->PathToGuid.find(promotedKey);
                pIt != overlay->PathToGuid.end() && pIt->second == md.Guid)
            {
                overlay->PathToGuid.erase(pIt);
            }
            overlay->Assets.erase(md.Guid);
            --occupancyDelta;
            occupancyDelta += CommitToGlobalMapsLocked(std::move(promoted));
            break;
        }
    }
    occupancyDelta += CommitToGlobalMapsLocked(md);
    ApplyAssetCountDelta(occupancyDelta);
    // S10 claim consumption: the path's identity is now committed; any
    // pending first-mint claim for it is settled.
    if (!m_PathClaims.empty())
        m_PathClaims.erase(PathMapKey(md.Path));
    RetireStaleAliasesOnRegistrationLocked(md.Guid);
}

ptrdiff_t AssetRegistry::CommitToGlobalMapsLocked(AssetMetadata md)
{
    // Update in-memory maps. If the asset is already registered, refresh its
    // metadata (especially derived dependencies computed by async tasks).
    //
    // A registration can arrive carrying a GUID this registry has already
    // superseded: the async scan resolves identities OUTSIDE the writer lock,
    // so a chunk resolved before a derived-identity reassignment still holds
    // the old snapshot GUID when it commits (cold-boot TOCTOU). If the remap
    // table says the incoming GUID was superseded by the GUID currently bound
    // to the SAME path, this is a stale-identity metadata refresh, not a new
    // binding — re-key it to the superseding identity. Without this, the
    // path-eviction below destroys the derived mapping that consumers (e.g.
    // the UI stylesheet cascade) captured moments earlier, and their GUIDs
    // resolve to nothing for the rest of the session.
    if (!m_GuidRemaps.empty())
    {
        auto remapIt = m_GuidRemaps.find(md.Guid);
        if (remapIt != m_GuidRemaps.end() && remapIt->second != md.Guid)
        {
            const std::string staleKey = PathMapKey(md.Path);
            PathShardMap& staleShard = PathShard(staleKey);
            auto itStale = staleShard.find(staleKey);
            if (itStale != staleShard.end() && itStale->second == remapIt->second)
                md.Guid = remapIt->second;
        }
    }

    // Mutation ordering note: the shards are FastHashMap (open addressing) —
    // every insert/erase invalidates ALL iterators and references into that
    // shard, not just the affected element. Do all preliminary erases first,
    // then re-find the target before reading through it.
    ptrdiff_t occupancyDelta = 0;
    const GUID mdGuid = md.Guid;
    const std::string mdKey = PathMapKey(md.Path);
    AssetShardMap& guidShard = AssetShard(mdGuid);
    auto itExisting = guidShard.find(mdGuid);
    const bool wasExisting = (itExisting != guidShard.end());

    // Remove old path mapping if it's changing for an existing GUID.
    bool pathChanged = false;
    if (wasExisting)
    {
        const std::filesystem::path oldPath = itExisting->second.Path;
        if (!oldPath.empty() && oldPath != md.Path)
        {
            pathChanged = true;
            const std::string oldKey = PathMapKey(oldPath);
            PathShardMap& oldShard = PathShard(oldKey);
            auto itP = oldShard.find(oldKey);
            if (itP != oldShard.end() && itP->second == md.Guid)
            {
                oldShard.erase(itP);
            }
        }
    }

    // If destination path is already mapped to a DIFFERENT GUID, evict it.
    {
        PathShardMap& destShard = PathShard(mdKey);
        auto itPath = destShard.find(mdKey);
        if (itPath != destShard.end() && itPath->second != md.Guid)
        {
            const GUID other = itPath->second;
            Logger::Log::Warning("AssetRegistry: RegisterAssetMetadata path '{}' is already registered ({}); overriding mapping",
                                 md.Path.string(), other.ToString());
            UnindexResidentGuidLocked(other);
            if (AssetShard(other).erase(other) > 0) // may invalidate itExisting (same shard case)
                --occupancyDelta;
            destShard.erase(itPath);
        }
    }

    // S11: index attribution follows the (possibly new) path. Resolve before
    // md's fields are moved out below.
    if (!wasExisting || pathChanged)
    {
        if (pathChanged)
            UnindexResidentGuidLocked(mdGuid);
        IndexResidentGuidLocked(mdGuid, md.Path);
    }

    // Apply the metadata write last, after all erases settle.
    if (wasExisting)
    {
        // Re-find: the evicted GUID's erase above may have invalidated the
        // iterator we captured at the top of the function.
        auto it = guidShard.find(mdGuid);
        if (it != guidShard.end())
        {
            AssetMetadata& existing = it->second;
            existing.Path = std::move(md.Path);
            existing.Name = std::move(md.Name);
            existing.Extension = std::move(md.Extension);
            // Same rule the store merges by: a registrar that could not classify
            // the asset reports no answer, and must not erase one that could.
            // A scan chunk landing after a typed RegisterAssetMetadata otherwise
            // downgrades the resident record to Unknown.
            if (IsClassificationKnown(md.Type, md.TypeId) ||
                !IsClassificationKnown(existing.Type, existing.TypeId))
            {
                existing.Type = md.Type;
                existing.TypeId = std::move(md.TypeId);
            }
            existing.LastModified = md.LastModified;
            existing.FileSize = md.FileSize;
            if (md.DependenciesExtracted)
            {
                existing.Dependencies = std::move(md.Dependencies);
                existing.DependencyPaths = std::move(md.DependencyPaths);
                existing.DependenciesExtracted = true;
            }
        }
        else
        {
            // Defensive: should not happen since we never erased md.Guid itself.
            guidShard[mdGuid] = std::move(md);
            ++occupancyDelta;
        }
    }
    else
    {
        guidShard[mdGuid] = std::move(md);
        ++occupancyDelta;
    }
    PathShard(mdKey)[mdKey] = mdGuid;
    return occupancyDelta;
}

bool AssetRegistry::RegisterAssetMetadata(const AssetMetadata& metadata)
{
    if (metadata.Guid.IsNull())
    {
        Logger::Log::Error("Cannot register asset with null GUID: {}", metadata.Path.string());
        return false;
    }

    const std::filesystem::path projectRoot = GetAssetRoot();
    // The caller's spelling feeds the store record and the name; normPath is
    // the map-key form (see RegisterAsset).
    const std::filesystem::path spelledPath =
        metadata.Path.is_relative() ? (projectRoot / metadata.Path) : metadata.Path;
    const std::filesystem::path normPath = NormalizePathForMap(spelledPath);

    // F.1 (defense-in-depth): if the path falls under an immutable source,
    // refuse the registration. RegisterAsset's gate already covers the
    // direct path; this gate covers the scan-completion fan-in
    // (RegistryUpdateTask) which currently can't reach Package mounts due
    // to RequiresScan=false but should fail safe if someone wires a
    // direct-feed path later.
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (const SourceEntry* src = FindSourceForPath(normPath); src && src->IsImmutable)
        {
            Logger::Log::Trace("AssetRegistry::RegisterAssetMetadata: source '{}' is immutable; "
                               "rejecting metadata registration of '{}'",
                               src->Alias, normPath.string());
            return false;
        }
    }

    // IMPORTANT: Avoid calling ClassifyAssetType() while holding m_RegistryMutex.
    // ClassifyAssetType() takes a shared lock on the same mutex, and std::shared_mutex is not re-entrant.
    AssetType inferredTypeForMetadata = metadata.Type;
    if (!IsRecognizedAssetType(inferredTypeForMetadata))
    {
        inferredTypeForMetadata = AssetType::Unknown;
    }
    if (inferredTypeForMetadata == AssetType::Unknown)
    {
        inferredTypeForMetadata = ClassifyAssetType(normPath);
    }

    // Upsert into the authoritative store/cache. Resolve the persisted type BEFORE taking the
    // registry lock because ClassifyAssetType acquires the same shared_mutex (not re-entrant).
    //
    // Ownership routing contract (mirrors RegisterAsset): only paths under the
    // project root persist into the project store. Anything else belongs to
    // the source that owns it — or to no store at all. Falling back to the raw
    // absolute path here is what used to leak editor-mount and foreign-machine
    // records into the project .assetdb.
    std::string canonicalRel;
    const bool persistent = AssetPaths::TryMakeCanonicalRelativePath(projectRoot, spelledPath, canonicalRel);

    AssetType typeToPersist = metadata.Type;
    if (!IsRecognizedAssetType(typeToPersist))
    {
        typeToPersist = AssetType::Unknown;
    }
    if (typeToPersist == AssetType::Unknown)
    {
        const AssetType inferred = ClassifyAssetType(normPath);
        if (inferred != AssetType::Unknown)
        {
            typeToPersist = inferred;
        }
    }

    // Hold a shared lock across ProjectStore()/ProjectCache() use so the project source cannot
    // be destroyed out from under this worker (UnregisterSource/RebindSource take the unique lock
    // before resetting m_ProjectSource or the store unique_ptr). The store has its own mutex for
    // concurrent upserts; the shared lock only guarantees the store pointer stays valid.
    if (persistent)
    {
        std::shared_lock<std::shared_mutex> sourceLock(m_RegistryMutex);
        SourceEntry* projectSrc = m_ProjectSource.get();
        AssetDatabase::IAssetStore* store = projectSrc ? projectSrc->Store.get() : nullptr;
        AssetDatabase::IAssetDbCache* cache = projectSrc ? projectSrc->Cache.get() : nullptr;

        if (store)
        {
            AssetDatabase::AssetObservation obs{};
            obs.guid = metadata.Guid;
            obs.path = canonicalRel;
            obs.type = typeToPersist;
            obs.typeId = metadata.TypeId;

            // The scan re-registers every project asset the warm snapshot
            // doesn't cover; a merge that changed nothing must not dirty the
            // source, or an untouched project forces a full store flush every
            // cold session. The cache below still refreshes the fingerprint.
            AssetDatabase::AssetRecord rec{};
            if (store->MergeObservation(obs, rec, nullptr) == AssetDatabase::StoreMergeResult::Changed)
            {
                projectSrc->StoreDirty.store(true, std::memory_order_relaxed);
            }

            if (cache)
            {
                (void)cache->UpsertAsset(rec, nullptr);

                // Populate the file fingerprint here too. RegisterAsset has
                // a parallel block that does this for the test-thread / hot
                // path, but the scan-driven RegistryUpdateTask comes through
                // here, and without fingerprint persistence cross-session
                // reconciliation (rename detection, move-by-hash) can't find
                // the asset's prior identity. Fixed bug surfaced by
                // SparseHashReconciliationForLargeFiles: scan would win the
                // race vs the test's direct RegisterAsset, leaving the cache
                // with the asset record but no fingerprint, so session 2's
                // missingByFileId/missingByHashSize maps were empty and the
                // moved file got re-GUID'd as brand-new.
                int64_t size = static_cast<int64_t>(metadata.FileSize);
                int64_t mtime = FileTimeToInt64(metadata.LastModified);
                if (size == 0 || mtime == 0)
                {
                    // Scan path passes us metadata that may have been
                    // computed long before this task ran. Re-stat now so
                    // the fingerprint matches current disk state.
                    int64_t curSize = 0;
                    int64_t curMtime = 0;
                    if (TryGetFileStats(normPath, curSize, curMtime))
                    {
                        if (size == 0)  size  = curSize;
                        if (mtime == 0) mtime = curMtime;
                    }
                }
                const std::string fileId = ReadFileIdentity(normPath).ToString();

                std::string cachedHash;
                int64_t cachedMtime = 0;
                int64_t cachedSize = 0;
                std::string cachedFileId;
                if (!canonicalRel.empty())
                {
                    auto it = projectSrc->SnapshotByPath.find(AssetPaths::FoldStorePathKey(canonicalRel));
                    if (it != projectSrc->SnapshotByPath.end())
                    {
                        cachedHash = it->second.Hash;
                        cachedMtime = it->second.Mtime;
                        cachedSize = it->second.Size;
                        cachedFileId = it->second.FileId;
                    }
                }
                const std::string hash = ComputeOrReuseHash(
                    normPath, size, mtime, fileId,
                    cachedHash, cachedMtime, cachedSize, cachedFileId);
                (void)cache->UpdateFileFingerprint(metadata.Guid, mtime, size, hash, fileId, nullptr);
            }
        }
    }
    else
    {
        // Route to the owning source's store. Same shared-lock-for-pointer-
        // validity pattern as the project branch above: the lock keeps the
        // SourceEntry (and its store/cache) alive; the store's own mutex
        // handles concurrent upserts. No fingerprint write here — this
        // mirrors RegisterAsset's non-persistent path, where mounted-source
        // records carry identity + type only and are never reconciled by
        // fingerprint.
        std::shared_lock<std::shared_mutex> sourceLock(m_RegistryMutex);
        SourceEntry* owner = FindSourceForPath(normPath);
        // Re-check immutability: the gate at the top of this function ran
        // under an earlier lock scope, and a source registered in between
        // could now own this path.
        const bool ownerRoutable = owner && owner != m_ProjectSource.get() &&
                                   owner->HasStore() && !owner->IsImmutable && !owner->IsReadOnly;
        std::string ownerRel;
        if (ownerRoutable && AssetPaths::TryMakeCanonicalRelativePath(owner->Root, normPath, ownerRel) && !ownerRel.empty())
        {
            AssetDatabase::AssetObservation obs{};
            obs.guid = metadata.Guid;
            obs.path = ownerRel;
            obs.type = typeToPersist;
            obs.typeId = metadata.TypeId;

            // The scan re-registers every mounted asset each session; a merge
            // that changed nothing must not dirty the source, or an untouched
            // mount forces a full store flush every startup.
            AssetDatabase::AssetRecord rec{};
            if (owner->Store->MergeObservation(obs, rec, nullptr) == AssetDatabase::StoreMergeResult::Changed)
            {
                owner->StoreDirty.store(true, std::memory_order_relaxed);
                if (owner->Cache)
                {
                    (void)owner->Cache->UpsertAsset(rec, nullptr);
                }
            }
        }
        else
        {
            Logger::Log::Trace("AssetRegistry::RegisterAssetMetadata: '{}' is outside the project root "
                               "and has no writable owning store; registering in-memory only",
                               normPath.string());
        }
    }

    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

    if (!m_Initialized)
    {
        // Prevent late async tasks from repopulating the registry during/after shutdown.
        return false;
    }

    AssetMetadata md = metadata;
    md.Path = normPath;
    if (md.Name.empty())
    {
        md.Name = spelledPath.stem().string();
    }
    if (md.Extension.empty())
    {
        md.Extension = GetCompoundExtensionFromPath(normPath.string());
    }
    if (!IsRecognizedAssetType(md.Type) || md.Type == AssetType::Unknown)
    {
        if (inferredTypeForMetadata != AssetType::Unknown)
        {
            md.Type = inferredTypeForMetadata;
        }
    }
    if (md.TypeId.empty())
    {
        md.TypeId = AssetTypeToString(md.Type);
    }

    CommitMetadataToMapsLocked(md);

    // Persist derived dependencies into the OWNING source's cache DB for
    // fast reference queries. E6d: the old unconditional ProjectCache()
    // write leaked dep edges for non-project GUIDs (editor mount, packages)
    // into the project SQLite cache; route to the owner or skip.
    if (md.DependenciesExtracted)
    {
        AssetDatabase::IAssetDbCache* depCache = nullptr;
        if (persistent)
        {
            depCache = ProjectCache();
        }
        else if (SourceEntry* owner = FindSourceForPath(md.Path);
                 owner && owner != m_ProjectSource.get())
        {
            depCache = owner->Cache.get();
        }
        if (depCache)
        {
            (void)depCache->ReplaceDependencies(md.Guid, md.Dependencies, nullptr);
        }
    }

    Logger::Log::Trace("Registered asset metadata: {} ({})", metadata.Name, metadata.Guid.ToString());
    return true;
}

size_t AssetRegistry::RegisterAssetMetadataBatch(
    Vector<AssetMetadata>&& batch,
    std::vector<AssetDatabase::ReconcileScanNewFile>* outDerivedNewFiles)
{
    if (batch.empty())
        return 0;

    const std::filesystem::path projectRoot = GetAssetRoot();

    struct StagedEntry
    {
        AssetMetadata Md;                 // Path replaced with the normalized absolute path
        std::string CanonicalRel;         // project-relative when Persistent
        bool Persistent = false;
        bool Skip = false;
        AssetType InferredType = AssetType::Unknown;
        // Snapshot fingerprint copy for hash reuse (persistent entries only).
        std::string CachedHash;
        std::string CachedFileId;
        int64_t CachedMtime = 0;
        int64_t CachedSize = 0;
        // S5 ownership routing: pinned owning source for non-project entries.
        SharedPtr<SourceEntry> Owner;
        std::string OwnerRel;
    };

    std::vector<StagedEntry> staged;
    staged.reserve(batch.size());

    // Pass 1 (no lock): normalize paths + lexical project membership.
    for (AssetMetadata& metadata : batch)
    {
        if (metadata.Guid.IsNull() || metadata.Path.empty())
            continue;
        StagedEntry e;
        e.Md = std::move(metadata);
        // The store record keeps the scanned spelling; Md.Path becomes the
        // map-key form (see RegisterAsset).
        const std::filesystem::path spelledPath =
            e.Md.Path.is_relative() ? (projectRoot / e.Md.Path) : e.Md.Path;
        e.Md.Path = NormalizePathForMap(spelledPath);
        e.Persistent = AssetPaths::TryMakeCanonicalRelativePath(projectRoot, spelledPath, e.CanonicalRel);
        staged.push_back(std::move(e));
    }
    if (staged.empty())
        return 0;

    // Pass 2 (no lock): type classification. ClassifyAssetType takes its own
    // shared lock and may sniff file contents — keep it out of ours.
    for (StagedEntry& e : staged)
    {
        AssetType t = e.Md.Type;
        if (!IsRecognizedAssetType(t))
            t = AssetType::Unknown;
        if (t == AssetType::Unknown)
            t = ClassifyAssetType(e.Md.Path);
        e.InferredType = t;
    }

    // Pass 3 (one shared lock): immutable-source gate, snapshot fingerprint
    // copies, and owner pinning. Pinned SharedPtrs keep each source's store
    // and cache alive through the off-lock persist pass below.
    SharedPtr<SourceEntry> projectPin;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        projectPin = m_ProjectSource;
        for (StagedEntry& e : staged)
        {
            if (const SourceEntry* src = FindSourceForPath(e.Md.Path); src && src->IsImmutable)
            {
                Logger::Log::Trace("AssetRegistry::RegisterAssetMetadataBatch: source '{}' is immutable; "
                                   "rejecting metadata registration of '{}'",
                                   src->Alias, e.Md.Path.string());
                e.Skip = true;
                continue;
            }

            if (e.Persistent)
            {
                if (projectPin && !e.CanonicalRel.empty())
                {
                    auto it = projectPin->SnapshotByPath.find(AssetPaths::FoldStorePathKey(e.CanonicalRel));
                    if (it != projectPin->SnapshotByPath.end())
                    {
                        e.CachedHash = it->second.Hash;
                        e.CachedMtime = it->second.Mtime;
                        e.CachedSize = it->second.Size;
                        e.CachedFileId = it->second.FileId;
                    }
                }
                continue;
            }

            // Owner routing mirrors RegisterAssetMetadata: the first source
            // containing the path (priority order) owns it; route only when
            // it is a writable non-project store.
            const std::string pathKey = PathMapKey(e.Md.Path);
            for (const auto& srcPtr : m_Sources)
            {
                if (!PathKeyHasRootPrefix(pathKey, srcPtr->RootPrefixKey))
                    continue;
                if (srcPtr.get() != m_ProjectSource.get() && srcPtr->HasStore() &&
                    !srcPtr->IsImmutable && !srcPtr->IsReadOnly)
                {
                    e.Owner = srcPtr;
                    e.OwnerRel = pathKey.substr(srcPtr->RootPrefixKey.size());
                }
                break; // first containing source decides, routable or not
            }
        }
    }

    // What this scan chunk observed about an entry, for the store to merge onto
    // its row. Same shape as the single-asset path.
    const auto observe = [](const StagedEntry& e, const std::string& rel)
    {
        AssetDatabase::AssetObservation obs{};
        obs.guid = e.Md.Guid;
        obs.path = rel;
        obs.type = e.InferredType;
        obs.typeId = e.Md.TypeId;
        return obs;
    };

    // Pass 4 (no registry lock): store upserts + fingerprint hashing, with
    // the derived-cache rows collected into one transaction per cache.
    if (projectPin && projectPin->Store)
    {
        AssetDatabase::IAssetStore& store = *projectPin->Store;
        std::vector<AssetDatabase::IAssetDbCache::AssetUpsertBatchEntry> cacheBatch;
        bool anyStoreWrite = false;

        for (StagedEntry& e : staged)
        {
            if (e.Skip || !e.Persistent)
                continue;

            // Identity re-check at write time (stored-identity sources).
            // The GUID was resolved when the chunk was staged; another
            // pipeline (startup scan vs explicit scan vs a direct
            // RegisterAsset) may have claimed this path in the store since —
            // both racers would otherwise mint different random GUIDs and
            // write duplicate rows for one path. Adopt the store's identity
            // so concurrent registrars converge, shrinking the race back to
            // the old serial fan-in's per-file window. Derived-identity
            // sources are immune (the GUID is a function of the path).
            if (!projectPin->DerivedIdentity && !e.CanonicalRel.empty())
            {
                if (auto g = store.LookupGuidByPath(e.CanonicalRel); g && !g->IsNull())
                    e.Md.Guid = *g;
            }

            // Advisory only, for the rename-heal candidate list below — the
            // write decision is the store's, taken under its own lock.
            const bool hadExisting = store.LookupGuidByPath(e.CanonicalRel).has_value();

            // The scan re-registers every project asset the warm snapshot
            // doesn't cover; a merge that changed nothing must not dirty the
            // source, or an untouched project forces a full store flush every
            // cold session. The cache batch below still refreshes fingerprints.
            AssetDatabase::AssetRecord rec{};
            if (store.MergeObservation(observe(e, e.CanonicalRel), rec, nullptr) ==
                AssetDatabase::StoreMergeResult::Changed)
            {
                anyStoreWrite = true;
            }

            if (projectPin->Cache)
            {
                int64_t size = static_cast<int64_t>(e.Md.FileSize);
                int64_t mtime = FileTimeToInt64(e.Md.LastModified);
                if (size == 0 || mtime == 0)
                {
                    int64_t curSize = 0;
                    int64_t curMtime = 0;
                    if (TryGetFileStats(e.Md.Path, curSize, curMtime))
                    {
                        if (size == 0)
                            size = curSize;
                        if (mtime == 0)
                            mtime = curMtime;
                    }
                }
                const std::string fileId = ReadFileIdentity(e.Md.Path).ToString();

                AssetDatabase::IAssetDbCache::AssetUpsertBatchEntry ce;
                ce.Record = rec;
                ce.HasFingerprint = true;
                ce.Mtime = mtime;
                ce.Size = size;
                ce.ContentHash = ComputeOrReuseHash(e.Md.Path, size, mtime, fileId,
                                                    e.CachedHash, e.CachedMtime, e.CachedSize, e.CachedFileId);
                ce.FileId = fileId;

                // Derived rename-heal candidate: a path the store had no
                // record for before this batch. Collected with the cache
                // batch's fingerprints so the scan-tail reconcile matches
                // absentees against it without re-reading the file.
                if (outDerivedNewFiles && projectPin->DerivedIdentity && !hadExisting)
                {
                    AssetDatabase::ReconcileScanNewFile snf;
                    snf.Guid = e.Md.Guid;
                    snf.CanonicalRel = e.CanonicalRel;
                    snf.Size = size;
                    snf.Mtime = mtime;
                    snf.FileId = ce.FileId;
                    snf.ContentHash = ce.ContentHash;
                    outDerivedNewFiles->push_back(std::move(snf));
                }

                cacheBatch.push_back(std::move(ce));
            }
        }

        if (anyStoreWrite)
            projectPin->StoreDirty.store(true, std::memory_order_relaxed);
        if (projectPin->Cache && !cacheBatch.empty())
        {
            // A failed batch rolls back every row's fingerprint in the
            // transaction, which silently degrades the next session's
            // warm-start snapshot — worth a warning, never worth a throw.
            std::string upsertErr;
            if (!projectPin->Cache->UpsertAssetBatch(cacheBatch, &upsertErr))
            {
                Logger::Log::Warning("AssetRegistry: derived-cache batch upsert of {} rows failed: {}",
                                     cacheBatch.size(), upsertErr);
            }
        }
    }

    // Owner-routed entries: store upserts per entry (change-gated, matching
    // the single-asset path), cache rows batched per owning source.
    {
        std::vector<std::pair<SourceEntry*, std::vector<AssetDatabase::IAssetDbCache::AssetUpsertBatchEntry>>>
            ownerCacheBatches;
        for (StagedEntry& e : staged)
        {
            if (e.Skip || e.Persistent || !e.Owner || e.OwnerRel.empty())
                continue;

            AssetDatabase::IAssetStore& store = *e.Owner->Store;

            // Same write-time identity re-check as the project branch above
            // (only meaningful for stored-identity owners; derived GUIDs are
            // deterministic).
            if (!e.Owner->DerivedIdentity)
            {
                if (auto g = store.LookupGuidByPath(e.OwnerRel); g && !g->IsNull())
                    e.Md.Guid = *g;
            }
            // The scan re-registers every mounted asset each session; a merge
            // that changed nothing must not dirty the source, or an untouched
            // mount forces a full store flush every startup.
            AssetDatabase::AssetRecord rec{};
            if (store.MergeObservation(observe(e, e.OwnerRel), rec, nullptr) !=
                AssetDatabase::StoreMergeResult::Changed)
                continue;

            e.Owner->StoreDirty.store(true, std::memory_order_relaxed);
            if (e.Owner->Cache)
            {
                auto groupIt = std::find_if(ownerCacheBatches.begin(), ownerCacheBatches.end(),
                                            [&](const auto& g) { return g.first == e.Owner.get(); });
                if (groupIt == ownerCacheBatches.end())
                {
                    ownerCacheBatches.push_back({e.Owner.get(), {}});
                    groupIt = ownerCacheBatches.end() - 1;
                }
                AssetDatabase::IAssetDbCache::AssetUpsertBatchEntry ce;
                ce.Record = rec;
                groupIt->second.push_back(std::move(ce));
            }
        }
        for (auto& [owner, entries] : ownerCacheBatches)
        {
            (void)owner->Cache->UpsertAssetBatch(entries, nullptr);
        }
    }

    // Pass 5 (one writer lock): commit the in-memory maps for the whole batch.
    size_t registered = 0;
    std::vector<std::tuple<GUID, Vector<GUID>, SharedPtr<SourceEntry>>> depWrites;
    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (!m_Initialized)
        {
            // Late async batch racing shutdown — mirror RegisterAssetMetadata.
            return 0;
        }

        for (StagedEntry& e : staged)
        {
            if (e.Skip)
                continue;

            AssetMetadata md = std::move(e.Md);
            // Same write-time identity confirm as pass 4, re-checked at map
            // commit time: a RegisterAsset on another thread can re-claim
            // this path in the store between our pass-4 upsert and this
            // commit. The store's surviving row is authoritative — committing
            // a dropped GUID would leave the maps pointing at a row that no
            // longer exists.
            if (e.Persistent && !e.CanonicalRel.empty() && projectPin &&
                projectPin->Store && !projectPin->DerivedIdentity)
            {
                if (auto winner = projectPin->Store->LookupGuidByPath(e.CanonicalRel);
                    winner && !winner->IsNull())
                {
                    md.Guid = *winner;
                }
            }
            if (md.Name.empty())
                md.Name = (e.CanonicalRel.empty() ? md.Path : std::filesystem::path(e.CanonicalRel)).stem().string();
            if (md.Extension.empty())
                md.Extension = GetCompoundExtensionFromPath(md.Path.string());
            if ((!IsRecognizedAssetType(md.Type) || md.Type == AssetType::Unknown) &&
                e.InferredType != AssetType::Unknown)
            {
                md.Type = e.InferredType;
            }
            if (md.TypeId.empty())
                md.TypeId = AssetTypeToString(md.Type);

            if (md.DependenciesExtracted)
            {
                // E6d routing: project entries → project cache; owner-routed
                // entries → the owning source's cache; otherwise skip.
                const SharedPtr<SourceEntry>& target = e.Persistent ? projectPin : e.Owner;
                if (target && target->Cache)
                    depWrites.emplace_back(md.Guid, md.Dependencies, target);
            }

            CommitMetadataToMapsLocked(md);
            ++registered;
        }
    }

    // Dependency edges land in the owning source's derived cache outside
    // the writer lock (SQLite I/O must not block registry readers). The
    // SharedPtr pin keeps each cache alive without the registry lock.
    for (const auto& [guid, deps, pin] : depWrites)
    {
        (void)pin->Cache->ReplaceDependencies(guid, deps, nullptr);
    }

    return registered;
}

void AssetRegistry::UnregisterAsset(const GUID& guid)
{
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    const AssetMetadata* md = FindAssetLocked(guid);
    if (!md)
        return;

    // F.1: reject if the asset belongs to an immutable source (Package mount).
    if (const SourceEntry* src = FindSourceForPath(md->Path); src && src->IsImmutable)
    {
        Logger::Log::Warning("AssetRegistry::UnregisterAsset: source '{}' is immutable; "
                             "rejecting unregister of {}", src->Alias, guid.ToString());
        return;
    }

    // F.1: TracksTombstones=false sources just drop the entry without
    // marking-missing in the store. TracksTombstones=true (default)
    // preserves the in-memory entry being removed but lets MarkMissing
    // bookkeeping fire elsewhere as needed.
    {
        const std::string pathKey = PathMapKey(md->Path);
        m_AssetSourceOwner.erase(pathKey);
        ErasePathMappingLocked(pathKey);
    }
    UnindexResidentGuidLocked(guid);
    if (EraseAssetEntryLocked(guid))
        ApplyAssetCountDelta(-1);
    Logger::Log::Debug("Unregistered asset: {}", guid.ToString());
}

bool AssetRegistry::TryGetAssetMetadata(const GUID& guid, AssetMetadata& outMetadata) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    // Redirect-aware resolution (Unreal-style). If a redirect exists for this GUID,
    // always prefer the target GUID's metadata.
    // E6a: skip the up-to-8 store probes entirely when no redirects exist —
    // the overwhelmingly common case for every metadata lookup.
    AssetDatabase::IAssetStore* store = ProjectStore();
    if (store && m_ProjectHasRedirects.load(std::memory_order_relaxed) && !guid.IsNull())
    {
        const AssetMetadata* target = nullptr;
        const AssetDatabase::RedirectChain chain = AssetDatabase::ChaseRedirectChain(
            *store, guid, AssetDatabase::RedirectTargetCheck::ResidentRecord,
            [&](const GUID& resolved)
            {
                target = FindAssetLocked(resolved);
                return target != nullptr;
            });
        if (chain.TargetAccepted)
        {
            outMetadata = *target;
            return true;
        }
    }

    if (const AssetMetadata* md = FindAssetLocked(guid))
    {
        outMetadata = *md; // copy to stable output
        return true;
    }

    // Reverse-redirect fallback. GetAssetGUID hands out the STABLE identity for
    // a freshly registered asset (GUID-in-store), recording `derived -> stable`
    // in the store; that stable GUID lands in scenes (e.g. a terrain zone's
    // PayloadRef). But a store reload re-derives metadata keys from the
    // canonical path (derived identity), so the stable GUID has no direct
    // record of its own in the next session — every metadata lookup through it
    // silently fails until the redirect SOURCE's record is surfaced here.
    // Only runs when both direct lookups missed, so the enumeration cost stays
    // off the hot path.
    if (!guid.IsNull())
    {
        const auto tryReverseLookup = [&](const AssetDatabase::IAssetStore* store) -> bool {
            if (!store)
                return false;
            for (const auto& redirect : store->EnumerateRedirects())
            {
                if (redirect.to != guid || redirect.from.IsNull())
                    continue;
                if (const AssetMetadata* md = FindAssetLocked(redirect.from))
                {
                    outMetadata = *md;
                    return true;
                }
            }
            return false;
        };

        // The project store may exist without a m_Sources entry (legacy
        // Initialize() convenience path), so consult it explicitly first.
        const AssetDatabase::IAssetStore* projectStore = ProjectStore();
        if (tryReverseLookup(projectStore))
            return true;
        for (const auto& source : m_Sources)
        {
            const AssetDatabase::IAssetStore* sourceStore = source ? source->Store.get() : nullptr;
            if (sourceStore == projectStore)
                continue;
            if (tryReverseLookup(sourceStore))
                return true;
        }
    }
    return false;
}

bool AssetRegistry::TryGetAssetMetadata(const std::filesystem::path& path, AssetMetadata& outMetadata) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (ProjectRoot() / path) : path);
    const GUID* guid = FindPathGuidLocked(PathMapKey(normPath));
    if (!guid)
    {
        return false;
    }
    const AssetMetadata* md = FindAssetLocked(*guid);
    if (!md)
    {
        return false;
    }
    outMetadata = *md; // copy to stable output
    return true;
}

GUID AssetRegistry::GetAssetGUID(const std::filesystem::path& path) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (ProjectRoot() / path) : path);
    const GUID* guid = FindPathGuidLocked(PathMapKey(normPath));
    return guid ? *guid : GUID::Null();
}

GUID AssetRegistry::GetOrCreateAssetGUID(const std::filesystem::path& path)
{
    if (path.empty())
        return GUID::Null();

    // Fast path: already registered in-memory.
    const GUID existing = GetAssetGUID(path);
    if (!existing.IsNull())
        return existing;

    std::string pathKey;
    {
        // Hold the registry lock for the whole iteration so source entries (and their stores)
        // cannot be destroyed while we dereference them. The store's own mutex is independent
        // and lock order is always registry -> store, so no deadlock.
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (!m_Initialized)
            return GUID::Null();

        const std::filesystem::path projectRoot = m_ProjectSource ? m_ProjectSource->Root : std::filesystem::path{};
        const std::filesystem::path normPath = NormalizePathForMap(
            path.is_relative() ? (projectRoot.empty() ? path : (projectRoot / path)) : path);
        // E4: normalize once, prefix-compare per source (no per-source syscalls).
        pathKey = PathMapKey(normPath);

        // S10: a pending claim from a racing first registration IS this
        // path's identity — adopt it instead of minting a second GUID.
        // Safe to check before the source walk: claims are only ever minted
        // for paths whose durable lookups missed, and the store write that
        // follows a claim always writes the claim winner, so a claim can
        // never disagree with a store row. Early check also covers
        // outside-root paths.
        if (auto it = m_PathClaims.find(pathKey); it != m_PathClaims.end())
            return it->second;

        // Iterate sources in priority order (m_Sources is already sorted).
        for (const auto& src : m_Sources)
        {
            if (!PathKeyHasRootPrefix(pathKey, src->RootPrefixKey))
                continue;
            const std::string canonicalRel = pathKey.substr(src->RootPrefixKey.size());

            if (src->DerivedIdentity)
            {
                // Derived identity: deterministic GUID from namespace + key.
                const std::string key = BuildRegistrySourceScopedKey(src->Alias, canonicalRel);
                if (!key.empty() && !src->NamespaceGuid.IsNull())
                    return GUID::Derive(src->NamespaceGuid, key);
            }
            else
            {
                // Stored identity: lookup in authoritative store.
                AssetDatabase::IAssetStore* store = src->Store.get();
                if (store && !canonicalRel.empty())
                {
                    if (auto g = store->LookupGuidByPath(canonicalRel))
                        return *g;
                }
                // SEAM 4: mutable stored-identity packages derive a deterministic
                // fallback for store-missing files (decision table in
                // Assets/Packages/PackageMounts.h). The project source's random
                // mint (and the immutable-mount probe mint) drop to the claim
                // below.
                if (src.get() != m_ProjectSource.get() && !src->IsImmutable && !src->IsReadOnly &&
                    !canonicalRel.empty())
                {
                    return MintStoredSourceFallbackGuid(*src, canonicalRel);
                }
                break;
            }
        }
    }

    // S10 atomic claim: every random mint is arbitrated under the writer
    // lock, so concurrent first registrations of one path (direct
    // RegisterAsset, scan staging workers, dependency extraction) converge
    // on a single GUID — the store only ever sees the winner. Covers the
    // stored-identity project mint and the outside-root fallback.
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    if (!m_Initialized)
        return GUID::Null();
    return ClaimPathGuidLocked(pathKey, GUID::Generate());
}

Vector<GUID> AssetRegistry::GetAssetsByType(AssetType type) const
{
    Vector<GUID> result;
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    ForEachAssetLocked([&](const GUID& guid, const AssetMetadata& metadata)
    {
        if (metadata.Type == type)
        {
            result.push_back(guid);
        }
    });
    return result;
}

AssetIndexRecord ToAssetIndexRecord(const AssetMetadata& metadata)
{
    AssetIndexRecord record;
    record.Guid = metadata.Guid;
    record.Type = metadata.Type;
    record.TypeId = metadata.TypeId;
    record.Path = metadata.Path;
    record.Name = metadata.Name;
    record.Extension = metadata.Extension;
    record.LastModified = metadata.LastModified;
    record.FileSize = metadata.FileSize;
    return record;
}

Vector<AssetIndexRecord> AssetRegistry::GetAssetIndexSnapshot() const
{
    Vector<AssetIndexRecord> result;
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    result.reserve(m_AssetCount.load(std::memory_order_relaxed));
    ForEachAssetLocked([&](const GUID& guid, const AssetMetadata& metadata)
    {
        AssetIndexRecord record = ToAssetIndexRecord(metadata);
        // The map key is the identity; metadata.Guid is a copy of it that a
        // partially-populated row could leave null.
        record.Guid = guid;
        result.push_back(std::move(record));
    });
    return result;
}

// GetIgnoreRulesSnapshot -- see new implementation above (near legacy wrappers)

bool AssetRegistry::IsAssetRegistered(const GUID& guid) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    return FindAssetLocked(guid) != nullptr;
}

bool AssetRegistry::IsAssetRegistered(const std::filesystem::path& path) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (ProjectRoot() / path) : path);
    return FindPathGuidLocked(PathMapKey(normPath)) != nullptr;
}

size_t AssetRegistry::GetAssetCount() const
{
    // S12: lock-free — see m_AssetCount's contract in the header. Exact at
    // every operation boundary; never blocks behind mount-family writers.
    return m_AssetCount.load(std::memory_order_relaxed);
}

std::vector<std::filesystem::path> AssetRegistry::GetRegisteredAssetPaths() const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    std::vector<std::filesystem::path> out;
    out.reserve(m_AssetCount.load(std::memory_order_relaxed));
    ForEachPathGuidLocked([&](const std::string& key, const GUID&)
    {
        out.push_back(PathFromMapKey(key));
    });
    return out;
}

GUID AssetRegistry::ResolveSessionAlias(const GUID& guid) const
{
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    if (m_GuidRemaps.empty())
        return guid;
    GUID current = guid;
    for (int depth = 0; depth < kGuidRemapMaxChase; ++depth)
    {
        if (FindAssetPrimaryLocked(current))
            return current;
        auto remapIt = m_GuidRemaps.find(current);
        if (remapIt == m_GuidRemaps.end() || remapIt->second == current)
            return current;
        current = remapIt->second;
    }
    return current;
}

GUID AssetRegistry::ResolveGuid(const GUID& guid) const
{
    if (guid.IsNull())
        return guid;

    // E6a: redirect-free registries resolve to identity without touching
    // the store.
    if (!m_ProjectHasRedirects.load(std::memory_order_relaxed))
        return guid;

    // ProjectStore() == m_ProjectSource->Store. A packaged Player mounts the
    // .assetmanifest via MakePackageMount("project", ...) (AssetManager.cpp),
    // and RegisterSource assigns m_ProjectSource to whatever registers under the
    // "project" alias regardless of mount kind — so this store IS the manifest
    // store in the Player, and the manifest's redirect records resolve here. No
    // package-specific branch is needed.
    AssetDatabase::IAssetStore* store = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
    }

    if (!store)
        return guid;

    return AssetDatabase::ChaseRedirectChain(*store, guid,
                                             AssetDatabase::RedirectTargetCheck::None, {})
        .Final;
}

std::vector<GUID> AssetRegistry::FindRedirectsTo(const GUID& resolvedGuid) const
{
    if (resolvedGuid.IsNull() || !m_ProjectHasRedirects.load(std::memory_order_relaxed))
        return {};

    AssetDatabase::IAssetStore* store = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
    }
    if (!store)
        return {};
    return store->FindRedirectSourcesTo(resolvedGuid);
}

GUID AssetRegistry::ResolveByAlias(std::string_view aliasUrl) const
{
    const auto colonPos = aliasUrl.find(':');
    if (colonPos == std::string_view::npos || colonPos == 0)
        return GUID{};

    const std::string_view alias = aliasUrl.substr(0, colonPos);
    const std::string_view relPath = aliasUrl.substr(colonPos + 1);
    if (relPath.empty())
        return GUID{};

    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    const SourceEntry* source = FindSourceByAlias(alias);
    if (!source)
        return GUID{};

    // E4: probe with the same normalized key the maps are built from.
    // The old lexically_normal-only form bypassed casefolding, so uppercase
    // alias URLs could never match on Windows.
    const std::string pathKey = PathMapKey(
        NormalizePathForMap(source->Root / std::filesystem::path(relPath)));

    const GUID* bound = FindPathGuidLocked(pathKey);
    if (!bound)
        return GUID{};

    // Verify the asset is actually owned by the requested source. Prevents
    // returning a project-mount asset for a `editor:` URL when the same path
    // exists under both mounts.
    auto ownerIt = m_AssetSourceOwner.find(pathKey);
    if (ownerIt != m_AssetSourceOwner.end() && ownerIt->second != alias)
        return GUID{};

    return *bound;
}

Vector<GUID> AssetRegistry::RefreshDependencies(const GUID& assetGuid)
{
    // Strategy: try parser-driven format-aware extraction first (richer
    // edge_kind + field_locator metadata). Fall back to the syntactic
    // GUID/path-text scanner if no parser opts in. Phase 3 staged migration:
    // parsers individually adopt the format-aware path; until they do, the
    // syntactic fallback is what populates the dep graph.
    AssetMetadata md;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const AssetMetadata* found = FindAssetLocked(assetGuid);
        if (!found)
            return {};
        md = *found;
    }

    std::vector<DepEdge> parserEdges;
    const bool parserHandled = TryExtractParserDepEdges(m_ParserRegistry, assetGuid, md, parserEdges);

    AssetDependencyInfo info(assetGuid, md.Type);
    if (parserHandled)
    {
        // Resolve path-form parser edges to GUIDs. The dep graph's walkers
        // (GetDependencies, the build collector) consume GUID-form rows only —
        // an edge left as a bare TargetPath would persist but never
        // materialize downstream, silently dropping scene→material /
        // material→shader chains authored by path.
        const std::filesystem::path assetRoot = GetAssetRoot();
        for (DepEdge& e : parserEdges)
        {
            if (!e.Target.IsNull() || e.TargetPath.empty())
                continue;
            std::filesystem::path abs(e.TargetPath);
            std::error_code ec;
            if (abs.is_relative())
            {
                // The project root, then the referrer's own folder (a glTF image URI
                // is relative to the .gltf), then the other mounts (an editor-mount
                // pipeline names editor-mount textures by path).
                std::filesystem::path candidate =
                    assetRoot.empty() ? std::filesystem::path{} : (assetRoot / abs).lexically_normal();
                if (candidate.empty() || !std::filesystem::exists(candidate, ec))
                {
                    candidate = (md.Path.parent_path() / abs).lexically_normal();
                    if (!std::filesystem::exists(candidate, ec))
                    {
                        std::filesystem::path acrossMounts = ResolveRelativeAssetPath(abs);
                        if (!acrossMounts.empty())
                            candidate = std::move(acrossMounts);
                    }
                }
                abs = std::move(candidate);
            }
            if (!std::filesystem::exists(abs, ec))
            {
                Logger::Log::Debug(
                    "AssetRegistry: dependency path '{}' of '{}' did not resolve to a file",
                    e.TargetPath, md.Path.string());
                continue;
            }
            const GUID g = GetOrCreateAssetGUID(abs);
            if (!g.IsNull())
                e.Target = ResolveGuid(g);
        }

        // Hydrate AssetDependencyInfo from parser edges so the
        // in-memory metadata cache stays consistent (DependencyPaths
        // is left empty: parser path refs were resolved above).
        info.Dependencies.reserve(parserEdges.size());
        for (const auto& e : parserEdges)
            info.Dependencies.push_back(e.Target);
    }
    else
    {
        info = AssetDependencyExtractor::ExtractDependencies(md);
        AssetDependencyExtractor::ResolveDependencyPathsToGuids(info, md, *this);
    }

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (AssetMetadata* target = FindAssetMutableLocked(assetGuid))
        {
            target->Dependencies = info.Dependencies;
            target->DependencyPaths.clear();
            for (const auto& p : info.DependencyPaths)
                target->DependencyPaths.push_back(p);
            target->DependenciesExtracted = true;
        }
    }
    // Persist to SQLite outside the exclusive lock to avoid blocking readers during I/O.
    {
        std::shared_lock<std::shared_mutex> cacheLock(m_RegistryMutex);
        if (auto* cache = ProjectCache())
        {
            if (parserHandled)
                (void)cache->ReplaceDependencies(assetGuid, parserEdges, nullptr);
            else
                (void)cache->ReplaceDependencies(assetGuid, info.Dependencies, nullptr);
        }
    }

    // Return the freshly extracted targets directly (null targets are path-form
    // edges that resolved to nothing — not dependencies).
    Vector<GUID> out;
    out.reserve(info.Dependencies.size());
    for (const GUID& dep : info.Dependencies)
        if (!dep.IsNull())
            out.push_back(dep);
    return out;
}

Vector<GUID> AssetRegistry::GetDependencies(const GUID& assetGuid)
{
    // Lazy dependency extraction: if the asset's dependencies were not extracted
    // during the startup scan (skipped for performance), extract them on first
    // query — and return that fresh result directly, so the answer never depends
    // on whether a SQLite cache is configured to round-trip it.
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const AssetMetadata* found = FindAssetLocked(assetGuid);
        if (found && !found->DependenciesExtracted)
        {
            readLock.unlock();
            return RefreshDependencies(assetGuid);
        }
    }

    AssetDatabase::IAssetDbCache* cache = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        cache = ProjectCache();
    }
    if (!cache)
    {
        // No cache DB (in-memory registries, some tests, cacheless mounts):
        // serve the extraction recorded on the metadata itself.
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const AssetMetadata* found = FindAssetLocked(assetGuid);
        if (!found)
            return {};
        Vector<GUID> out;
        out.reserve(found->Dependencies.size());
        for (const GUID& dep : found->Dependencies)
            if (!dep.IsNull())
                out.push_back(dep);
        return out;
    }
    return cache->GetDependencies(assetGuid);
}

Vector<GUID> AssetRegistry::GetDependents(const GUID& assetGuid) const
{
    AssetDatabase::IAssetDbCache* cache = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        cache = ProjectCache();
    }
    if (!cache)
    {
        return {};
    }
    Vector<GUID> out;
    cache->IterateDependents(assetGuid, [&out](const GUID& referrer) {
        out.push_back(referrer);
        return true;
    });
    return out;
}

GUID AssetRegistry::ResolvePathTarget(std::string_view canonicalRel) const
{
    if (canonicalRel.empty())
        return GUID::Null();

    const std::filesystem::path rel(canonicalRel);

    // Iterate registered sources in priority order; first mount that has the
    // path registered wins. Mirrors GetOrCreateAssetGUID so dep edges authored
    // against either project or editor mount resolve correctly.
    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
    if (!m_Initialized)
        return GUID::Null();
    for (const auto& src : m_Sources)
    {
        if (!src)
            continue;
        const std::filesystem::path absPath = NormalizePathForMap(src->Root / rel);
        if (const GUID* bound = FindPathGuidLocked(PathMapKey(absPath)))
            return *bound;
    }
    return GUID::Null();
}

std::vector<DepEdge> AssetRegistry::GetResolvedDependencyEdges(const GUID& assetGuid) const
{
    AssetDatabase::IAssetDbCache* cache = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        cache = ProjectCache();
    }
    if (!cache)
        return {};

    std::vector<DepEdge> edges = cache->GetDependencyEdges(assetGuid);
    // Upgrade path-form edges to GUID-form where the path resolves. Edges
    // that don't resolve are left unchanged — caller treats those as missing
    // references.
    for (DepEdge& edge : edges)
    {
        if (edge.Target.IsNull() && !edge.TargetPath.empty())
        {
            const GUID resolved = ResolvePathTarget(edge.TargetPath);
            if (!resolved.IsNull())
                edge.Target = resolved;
        }
    }
    return edges;
}

Vector<AssetRegistry::MissingAssetInfo> AssetRegistry::GetMissingAssets() const
{
    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        cache = ProjectCache();
        assetRoot = ProjectRoot();
    }

    Vector<MissingAssetInfo> out;
    if (!store)
        return out;

    // A tombstone rests in one of two places depending on the mount's identity
    // scheme: the journaled record flag under Stored identity, the per-machine
    // cache's ghost set under Derived, where the git-shared journal carries no
    // disk observations at all. Read both — a caller asking what is missing
    // means the question, not one residence's answer to it.
    //
    // Dedup is structural rather than a filter: the store record is the unit of
    // the result, so a record flagged in both residences yields exactly one
    // entry. A cache ghost row with no store record behind it is deliberately
    // not surfaced — it has no path or type to report and is residue from an
    // interrupted removal, not an asset the user lost.
    std::unordered_set<GUID> cacheMissing;
    if (cache)
    {
        const std::vector<GUID> ghosts = cache->EnumerateMissingAssets();
        cacheMissing.insert(ghosts.begin(), ghosts.end());
    }

    // AssetStore_TextJsonl implements EnumerateAssets; use interface.
    const std::vector<AssetDatabase::AssetRecord> records = store->EnumerateAssets();
    out.reserve(records.size());

    for (const auto& rec : records)
    {
        if (rec.guid.IsNull())
            continue;
        if (!rec.missing && cacheMissing.count(rec.guid) == 0)
            continue;

        MissingAssetInfo mi{};
        mi.guid = rec.guid;
        mi.type = rec.type;
        mi.lastKnownPath = NormalizePathForMap(assetRoot / std::filesystem::path(rec.path));
        mi.dependentCount = cache ? cache->CountDependents(rec.guid) : 0;
        out.push_back(std::move(mi));
    }

    return out;
}

bool AssetRegistry::TryRelinkAssetPath(const GUID& guid, const std::filesystem::path& newPath)
{
    if (guid.IsNull() || newPath.empty())
    {
        return false;
    }

    const std::filesystem::path projectRoot = GetAssetRoot();
    const std::filesystem::path normNew = NormalizePathForMap(
        newPath.is_relative() ? (projectRoot / newPath) : newPath);

    std::error_code ec;
    if (!std::filesystem::exists(normNew, ec) || !std::filesystem::is_regular_file(normNew, ec))
    {
        return false;
    }

    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        cache = ProjectCache();
        assetRoot = ProjectRoot();
    }

    if (!store)
    {
        return false;
    }

    std::string canonicalRel;
    if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normNew, canonicalRel))
    {
        canonicalRel = AssetDatabase::AssetStore_TextJsonl::NormalizeCanonicalPath(normNew.generic_string());
    }

    AssetDatabase::AssetRecord rec{};
    if (!store->TryGetAsset(guid, rec))
    {
        rec.guid = guid;
    }
    rec.path = canonicalRel;
    if (!IsRecognizedAssetType(rec.type))
    {
        rec.type = AssetType::Unknown;
    }
    const AssetType inferredType = ClassifyAssetType(normNew);
    if (inferredType != AssetType::Unknown)
    {
        rec.type = inferredType;
        rec.typeId = AssetTypeToString(inferredType);
    }
    rec.missing = false;

    (void)store->UpsertAsset(rec, nullptr);
    (void)store->MarkMissing(guid, false, nullptr);
    if (auto pinnedDirty = ProjectSourcePinned())
        pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);

    if (cache)
    {
        (void)cache->UpsertAsset(rec, nullptr);

        int64_t size = 0;
        int64_t mtime = 0;
        if (TryGetFileStats(normNew, size, mtime))
        {
            const std::string fileId = ReadFileIdentity(normNew).ToString();
            std::string hash;
            if (size <= (256 * 1024))
            {
                hash = ComputePartialHash_Fnv1a64(normNew, 64 * 1024, static_cast<uint64_t>(size));
            }
            else
            {
                // For large files, use sparse sampling for future reconciliation.
                hash = ComputeSparseHash_Fnv1a64(normNew, size);
            }
            (void)cache->UpdateFileFingerprint(guid, mtime, size, hash, fileId, nullptr);
        }
    }

    // Register/update in-memory metadata entry for the relinked asset.
    (void)RegisterAsset(normNew);
    return true;
}

// ============================================================================
// Provenance: importer-time produced -> producer relationships.
// ============================================================================

namespace
{

// Lazy-populate the in-memory provenance cache from the project source's
// SQLite cache. Caller must hold m_RegistryMutex (writer lock for the
// populate, since both maps are mutated).
//
// Populates once per registry lifetime. Subsequent calls under the same
// lock are no-ops.
void PopulateProvenanceCacheLocked(
    AssetDatabase::IAssetDbCache* cache,
    HashMap<GUID, GUID>& producer,
    HashMap<GUID, Vector<GUID>>& produced,
    bool& populatedFlag)
{
    if (populatedFlag)
        return;
    populatedFlag = true; // Set early so a re-entrant call short-circuits.

    if (!cache)
        return;

    // EnumerateAllProvenance returns flat (produced, producer, importer_id)
    // rows. We need to populate two maps: 1:1 produced->producer and 1:N
    // producer->produced[]. The grouping is one pass over the row vector.
    auto rows = cache->EnumerateAllProvenance();
    producer.reserve(rows.size());
    for (const auto& row : rows)
    {
        if (row.Produced.IsNull() || row.Producer.IsNull())
            continue;
        producer.emplace(row.Produced, row.Producer);
        produced[row.Producer].push_back(row.Produced);
    }
}

// Remove `produced` from m_ProvenanceProduced[producer] vector. Used on
// re-register (when produced is reassigned to a new producer) and on
// unregister.
void EraseProducedFromProducerListLocked(
    HashMap<GUID, Vector<GUID>>& produced,
    const GUID& producer,
    const GUID& target)
{
    auto it = produced.find(producer);
    if (it == produced.end())
        return;
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), target), vec.end());
    if (vec.empty())
        produced.erase(it);
}

} // namespace

bool AssetRegistry::RegisterProvenance(const GUID& produced,
                                       const GUID& producer,
                                       std::string_view importerId)
{
    if (produced.IsNull() || producer.IsNull() || produced == producer)
        return false;

    auto pinned = ProjectSourcePinned();
    if (!pinned || !pinned->Cache)
    {
        Logger::Log::Warning("AssetRegistry::RegisterProvenance: project source has no cache; "
                             "skipping provenance for {} <- {}",
                             produced.ToString(), producer.ToString());
        return false;
    }

    // Hold writer lock across both the SQLite mutation and the in-memory
    // map update so a concurrent reader can't observe a half-applied state.
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

    // Validate both GUIDs are registered in this session.
    if (!FindAssetLocked(produced) || !FindAssetLocked(producer))
    {
        Logger::Log::Warning("AssetRegistry::RegisterProvenance: produced ({}) or producer ({}) "
                             "is not registered; rejecting",
                             produced.ToString(), producer.ToString());
        return false;
    }

    PopulateProvenanceCacheLocked(pinned->Cache.get(),
                                  m_ProvenanceProducer,
                                  m_ProvenanceProduced,
                                  m_ProvenanceCachePopulated);

    std::string err;
    if (!pinned->Cache->RegisterProvenance(produced, producer, importerId, &err))
    {
        Logger::Log::Warning("AssetRegistry::RegisterProvenance: SQLite write failed: {}", err);
        return false;
    }

    // Update in-memory maps. If `produced` was previously assigned to a
    // different producer, remove it from that producer's list first.
    auto existing = m_ProvenanceProducer.find(produced);
    if (existing != m_ProvenanceProducer.end() && existing->second != producer)
    {
        EraseProducedFromProducerListLocked(m_ProvenanceProduced, existing->second, produced);
    }
    m_ProvenanceProducer[produced] = producer;
    auto& vec = m_ProvenanceProduced[producer];
    if (std::find(vec.begin(), vec.end(), produced) == vec.end())
        vec.push_back(produced);

    return true;
}

void AssetRegistry::UnregisterProvenance(const GUID& produced)
{
    if (produced.IsNull())
        return;

    auto pinned = ProjectSourcePinned();
    if (!pinned || !pinned->Cache)
        return;

    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    PopulateProvenanceCacheLocked(pinned->Cache.get(),
                                  m_ProvenanceProducer,
                                  m_ProvenanceProduced,
                                  m_ProvenanceCachePopulated);

    (void)pinned->Cache->UnregisterProvenance(produced, nullptr);

    auto it = m_ProvenanceProducer.find(produced);
    if (it == m_ProvenanceProducer.end())
        return;
    const GUID producer = it->second;
    m_ProvenanceProducer.erase(it);
    EraseProducedFromProducerListLocked(m_ProvenanceProduced, producer, produced);
}

Vector<GUID> AssetRegistry::GetProducedAssets(const GUID& producer) const
{
    if (producer.IsNull())
        return {};

    auto pinned = ProjectSourcePinned();
    if (!pinned || !pinned->Cache)
        return {};

    // Lazy populate requires a writer lock — drop the const briefly so the
    // populate can run, then read under the same lock.
    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    PopulateProvenanceCacheLocked(pinned->Cache.get(),
                                  const_cast<HashMap<GUID, GUID>&>(m_ProvenanceProducer),
                                  const_cast<HashMap<GUID, Vector<GUID>>&>(m_ProvenanceProduced),
                                  const_cast<bool&>(m_ProvenanceCachePopulated));

    auto it = m_ProvenanceProduced.find(producer);
    if (it == m_ProvenanceProduced.end())
        return {};
    return Vector<GUID>(it->second.begin(), it->second.end());
}

GUID AssetRegistry::GetProducer(const GUID& produced) const
{
    if (produced.IsNull())
        return GUID::Null();

    auto pinned = ProjectSourcePinned();
    if (!pinned || !pinned->Cache)
        return GUID::Null();

    std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
    PopulateProvenanceCacheLocked(pinned->Cache.get(),
                                  const_cast<HashMap<GUID, GUID>&>(m_ProvenanceProducer),
                                  const_cast<HashMap<GUID, Vector<GUID>>&>(m_ProvenanceProduced),
                                  const_cast<bool&>(m_ProvenanceCachePopulated));

    auto it = m_ProvenanceProducer.find(produced);
    if (it == m_ProvenanceProducer.end())
        return GUID::Null();
    return it->second;
}

// ----------------------------------------------------------------------------
// Aggressive-tombstone-cleanup cascade.
//
// Given a recently-removed (or about-to-be-removed) GUID, walks its outgoing
// dep edges and recursively removes any newly-orphaned downstream assets. An
// asset is eligible for cleanup when:
//   - IterateDependents (GUID-form) finds no referrer in m_Assets
//     (i.e. every referrer is either tombstoned or never registered)
//   - IterateDependentsByPath (path-form) finds no referrer at all
//   - EnumerateProducedAssets is empty (no provenance children)
//
// Iterative + visited set so cycles are bounded. Acquires the writer lock
// per-iteration to update in-memory state alongside the SQLite mutation;
// the cache mutex is taken transiently inside each cache call.
//
// Caller invariant: must NOT hold m_RegistryMutex on entry. The cleanup
// path itself does not call any registry method that would re-acquire it.
// ----------------------------------------------------------------------------
void AssetRegistry::CleanupOrphanedDependenciesCascade(const GUID& startGuid,
                                                       AssetDatabase::IAssetStore* store,
                                                       AssetDatabase::IAssetDbCache* cache)
{
    if (startGuid.IsNull() || !store || !cache)
        return;

    // Visited set bounds the walk on cycles.
    std::unordered_set<GUID> visited;
    std::vector<GUID> queue;
    queue.push_back(startGuid);

    while (!queue.empty())
    {
        const GUID guid = queue.back();
        queue.pop_back();
        if (!visited.insert(guid).second)
            continue;

        // Acquire writer lock for each step; eligibility check + mutation
        // atomically under the same lock so concurrent RegisterAsset can't
        // race with us.
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

        // Skip if not in the in-memory store. Could be either: already cleaned
        // up, never registered, or currently tombstoned (tombstones live in
        // the SQLite store with missing=true but are filtered out of m_Assets
        // during PopulateHotCachesFromSource). We rely on SQLite as the source
        // of truth from here on.
        AssetDatabase::AssetRecord rec{};
        const bool inMemory = (FindAssetLocked(guid) != nullptr);
        if (!store->TryGetAsset(guid, rec))
            continue; // asset doesn't exist in store either — nothing to clean

        // Eligibility check 1: GUID-form referrers. Stream + early-exit on
        // first live referrer (in m_Assets). Tombstones / orphan edges have
        // referrers that aren't in m_Assets, so they don't count.
        bool hasLiveReferrer = false;
        cache->IterateDependents(guid, [&](const GUID& referrer) {
            if (FindAssetLocked(referrer) != nullptr)
            {
                hasLiveReferrer = true;
                return false; // stop
            }
            return true;
        });
        if (hasLiveReferrer)
            continue;

        // Eligibility check 2: path-form referrers. Any match disqualifies.
        if (!rec.path.empty())
        {
            bool hasPathReferrer = false;
            cache->IterateDependentsByPath(rec.path, [&](const GUID&) {
                hasPathReferrer = true;
                return false; // stop on first hit
            });
            if (hasPathReferrer)
                continue;
        }

        // Eligibility check 3: provenance children. Don't auto-remove a
        // producer that still has produced sidecars.
        if (!cache->EnumerateProducedAssets(guid).empty())
            continue;

        // Eligible. Capture outgoing edges BEFORE removal so we can recurse
        // on them. cache->RemoveAsset clears both edge directions in the
        // deps table atomically, so the cleared outgoing list is gone after
        // the call returns — the captured copy is what we recurse on.
        std::vector<GUID> outgoing = cache->GetDependencies(guid);

        // Remove from authoritative store + cache (deps both directions
        // drop atomically inside cache->RemoveAsset) + provenance.
        (void)store->RemoveAsset(guid, nullptr);
        (void)cache->RemoveAsset(guid, nullptr);

        // Provenance row drops alongside the SQLite asset row (mirrors the
        // existing deregistration hook). Inline rather than calling
        // UnregisterProvenance — we already hold the writer lock, and the
        // wrapper would try to re-acquire it. The in-memory map cleanup
        // skips PopulateProvenanceCacheLocked: if the cache wasn't yet
        // populated, the maps are empty (no-op find), and the SQLite delete
        // alone is correct — next Populate reads the post-deletion state.
        auto producerIt = m_ProvenanceProducer.find(guid);
        if (producerIt != m_ProvenanceProducer.end())
        {
            const GUID producerOfRemoved = producerIt->second;
            m_ProvenanceProducer.erase(producerIt);
            auto producedListIt = m_ProvenanceProduced.find(producerOfRemoved);
            if (producedListIt != m_ProvenanceProduced.end())
            {
                auto& vec = producedListIt->second;
                vec.erase(std::remove(vec.begin(), vec.end(), guid), vec.end());
                if (vec.empty())
                    m_ProvenanceProduced.erase(producedListIt);
            }
        }
        (void)cache->UnregisterProvenance(guid, nullptr);

        // Drop in-memory registry state if present (live asset). Tombstones
        // aren't in m_Assets so this is a no-op for them.
        if (inMemory)
        {
            if (const AssetMetadata* meta = FindAssetLocked(guid))
            {
                const std::string removedKey = PathMapKey(meta->Path);
                m_AssetSourceOwner.erase(removedKey);
                ErasePathMappingLocked(removedKey);
                UnindexResidentGuidLocked(guid);
                if (EraseAssetEntryLocked(guid))
                    ApplyAssetCountDelta(-1);
            }
        }

        writeLock.unlock();

        // Queue outgoing GUIDs for re-evaluation. Each is a potential orphan
        // now that one of its referrers (us) is gone.
        for (const GUID& o : outgoing)
        {
            if (!o.IsNull() && visited.find(o) == visited.end())
                queue.push_back(o);
        }
    }
}

bool AssetRegistry::AddRedirect(const GUID& from, const GUID& to)
{
    if (from.IsNull() || to.IsNull() || from == to)
    {
        return false;
    }

    AssetDatabase::IAssetStore* store = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
    }
    if (!store)
        return false;
    if (!store->AddRedirect(from, to, nullptr))
        return false;
    m_ProjectHasRedirects.store(true, std::memory_order_relaxed);
    if (auto pinnedDirty = ProjectSourcePinned())
        pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
    return true;
}

bool AssetRegistry::RemoveRedirect(const GUID& from)
{
    if (from.IsNull())
        return false;
    AssetDatabase::IAssetStore* store = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
    }
    if (!store)
        return false;
    const bool ok = store->RemoveRedirect(from, nullptr);
    if (ok)
    {
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            RefreshProjectRedirectFlagLocked();
        }
        if (auto pinnedDirty = ProjectSourcePinned())
            pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
    }
    return ok;
}

namespace
{
static bool FixupTextFileReplaceGuid(const std::filesystem::path& path,
                                    const GUID& from,
                                    const GUID& to,
                                    bool& outChanged)
{
    outChanged = false;

    std::string content;
    if (!ReadFileTextShared(path, content))
        return false;

    // GUID pattern: 8-4-4-4-12 with optional dashes; case-insensitive.
    static const std::regex guidPattern(R"(\b[0-9a-fA-F]{8}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{12}\b)");

    std::string out;
    out.reserve(content.size());

    size_t last = 0;
    for (std::sregex_iterator it(content.begin(), content.end(), guidPattern), end; it != end; ++it)
    {
        const auto& m = *it;
        const size_t pos = static_cast<size_t>(m.position());
        const size_t len = static_cast<size_t>(m.length());

        out.append(content, last, pos - last);

        const std::string matched = m.str();
        GUID g(matched);
        if (g == from)
        {
            const bool hadDash = matched.find('-') != std::string::npos;
            bool anyUpper = false;
            for (char c : matched)
            {
                if (c >= 'A' && c <= 'F')
                {
                    anyUpper = true;
                    break;
                }
            }

            std::string repl = hadDash ? to.ToString() : to.ToCompactString();
            if (anyUpper)
            {
                std::transform(repl.begin(), repl.end(), repl.begin(), [](unsigned char c)
                               { return static_cast<char>(std::toupper(c)); });
            }

            out.append(repl);
            outChanged = true;
        }
        else
        {
            out.append(matched);
        }

        last = pos + len;
    }

    out.append(content, last, std::string::npos);

    if (!outChanged)
    {
        return true;
    }

    std::ofstream outFile(path, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        return false;
    outFile.write(out.data(), static_cast<std::streamsize>(out.size()));
    return outFile.good();
}
} // namespace

size_t AssetRegistry::ExtractPendingDependencies()
{
    std::vector<GUID> pending;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        const SourceEntry* project = m_ProjectSource.get();
        if (!project)
            return 0;

        // Project-owned assets only. RefreshDependencies writes its edges to
        // the PROJECT cache whatever mount the asset came from, so extracting
        // an editor-mount or package asset here would file its edges under a
        // GUID that cache does not own — the same leak the batch registration
        // path routes around at RegisterAssetMetadataBatch. That is a
        // pre-existing hole in RefreshDependencies and not this call's to fix;
        // what this call must not do is widen it to every mounted asset. The
        // project cache is also the only index the redirect sweep reads, so
        // these are exactly the assets whose edges it needs.
        ForEachAssetLocked([this, project, &pending](const GUID& guid, const AssetMetadata& md)
                           {
                               if (guid.IsNull() || md.DependenciesExtracted)
                                   return;
                               if (FindSourceForPath(md.Path) != project)
                                   return;
                               pending.push_back(guid);
                           });
    }

    // Collect first, extract second: RefreshDependencies takes the registry
    // lock itself (and the writer lock, to stamp the record), so it cannot run
    // inside the walk.
    for (const GUID& guid : pending)
        (void)RefreshDependencies(guid);
    return pending.size();
}

size_t AssetRegistry::FixUpRedirects()
{
    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        cache = ProjectCache();
    }
    if (!store || !cache)
    {
        return 0;
    }

    const std::vector<AssetDatabase::RedirectRecord> redirects = store->EnumerateRedirects();
    if (redirects.empty())
    {
        return 0;
    }

    // #1008: an empty index is not the answer "nothing references this". The
    // reverse dependency index has no lazy-populate path of its own — only a
    // forward query extracts, and the startup scan writes no edges at all — so
    // on a project nobody has run a reference query against, every source
    // below reads unreferenced and every hop retires without a referrer being
    // rewritten. Warm the index first and the emptiness means what the branch
    // assumes. This is a menu action over a project the user is looking at, so
    // paying the extraction here is affordable; the startup retention pass,
    // the class's other member, cannot pay it and keeps its own cold-cache
    // guard instead.
    if (const size_t extracted = ExtractPendingDependencies(); extracted > 0)
    {
        Logger::Log::Info("AssetRegistry::FixUpRedirects: extracted dependencies for {} asset(s) "
                          "before sweeping — the reverse index is built on demand, so an empty "
                          "referrer set is only evidence once every asset has been read",
                          extracted);
    }

    // Every target is captured before the first mutation. The sweep removes
    // hops as it goes, so a target resolved from inside the loop would chase a
    // graph this same loop has already shortened: once A -> B is dropped, a
    // later X -> A resolves to A, and X's referrers get rewritten on disk to a
    // GUID that no longer forwards anywhere. Which redirects land on that
    // outcome depends only on enumeration order.
    //
    // The chain-final target must have a store record. This sweep reads and
    // writes the project store, and the store is also the only liveness signal
    // that covers GUIDs which are not currently registered, so the store's
    // records are the right authority here rather than the resident map. A
    // recordless chain-final target means the redirect is load-bearing residue
    // (live-rename residue, derived -> stable identity): rewriting referrers to
    // it and then dropping the hop would strand exactly the references the hop
    // exists to forward. Skipping leaves such a redirect untouched. A cyclic
    // chain reports no redirection and is skipped for the same reason - cycles
    // are reported, never repaired.
    struct RedirectFixUp
    {
        GUID From;
        GUID ChainFinal;
    };
    std::vector<RedirectFixUp> fixUps;
    fixUps.reserve(redirects.size());
    std::vector<GUID> sources;
    sources.reserve(redirects.size());
    for (const auto& rr : redirects)
    {
        if (rr.from.IsNull() || rr.to.IsNull() || rr.from == rr.to)
            continue;

        const AssetDatabase::RedirectChain chain = AssetDatabase::ChaseRedirectChain(
            *store, rr.from, AssetDatabase::RedirectTargetCheck::StoreRecord, {});
        if (!chain.TargetAccepted)
            continue;

        fixUps.push_back(RedirectFixUp{rr.from, chain.Final});
        sources.push_back(rr.from);
    }

    // Collapse every chain to a single hop before the first removal. A hop this
    // sweep leaves in place (a referrer it could not rewrite) would otherwise be
    // left pointing at a GUID whose own hop the sweep removed, which turns a
    // reference that resolved correctly before the sweep into a dangling one.
    // Retargeting only shortens chains, so it cannot move a target captured
    // above.
    if (AssetDatabase::RetargetIncomingRedirects(*store, sources) > 0)
    {
        // A sweep that retargets but removes nothing still changed the store,
        // and StoreDirty is what gates the flush.
        if (auto pinnedDirty = ProjectSourcePinned())
            pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
    }

    size_t modifiedFiles = 0;

    for (const auto& [from, chainFinal] : fixUps)
    {
        // Provenance redirect runs UNCONDITIONALLY before the dep-dependents
        // check, because provenance is independent of the dep graph. A
        // redirect that no dep edges reference could still have provenance
        // rows pointing at the source (the produced asset's parent was renamed,
        // or vice versa). Rewriting here ensures the provenance row tracks
        // the redirect target; the in-memory cache reshuffle below mirrors
        // it. Empty result is fine — RedirectProvenance returns 0 when no
        // rows match, no harm.
        //
        // Hold the writer lock across BOTH the SQLite rewrite and the
        // in-memory reshuffle — a concurrent RegisterProvenance between
        // the two would observe inconsistent state otherwise. (Cache
        // mutex order is registry-then-cache; RedirectProvenance takes
        // the cache mutex internally, no deadlock concern.)
        {
            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
            const size_t provRowsChanged = cache->RedirectProvenance(from, chainFinal, nullptr);
            if (provRowsChanged > 0 && m_ProvenanceCachePopulated)
            {
                // Produced-side: move the (source, producer) entry to key chainFinal,
                // and rewrite vectors in m_ProvenanceProduced that name the source.
                auto producedIt = m_ProvenanceProducer.find(from);
                if (producedIt != m_ProvenanceProducer.end())
                {
                    const GUID producer = producedIt->second;
                    m_ProvenanceProducer.erase(producedIt);
                    m_ProvenanceProducer[chainFinal] = producer;
                    auto vecIt = m_ProvenanceProduced.find(producer);
                    if (vecIt != m_ProvenanceProduced.end())
                    {
                        for (GUID& g : vecIt->second)
                        {
                            if (g == from)
                                g = chainFinal;
                        }
                    }
                }

                // Producer-side: rewrite values in m_ProvenanceProducer matching the
                // source and rename the key in m_ProvenanceProduced.
                for (auto& [produced, producer] : m_ProvenanceProducer)
                {
                    if (producer == from)
                        producer = chainFinal;
                }
                auto producerKeyIt = m_ProvenanceProduced.find(from);
                if (producerKeyIt != m_ProvenanceProduced.end())
                {
                    // Extract the vector and erase from m_ProvenanceProduced
                    // BEFORE touching m_ProvenanceProduced[chainFinal]. operator[]
                    // can trigger a rehash on insertion, which would invalidate
                    // producerKeyIt; dereferencing or erasing through the
                    // invalidated iterator is UB. Move-then-erase first; insert
                    // at the new key second.
                    Vector<GUID> producedList = std::move(producerKeyIt->second);
                    m_ProvenanceProduced.erase(producerKeyIt);
                    auto& mergeTarget = m_ProvenanceProduced[chainFinal];
                    for (const GUID& g : producedList)
                    {
                        if (std::find(mergeTarget.begin(), mergeTarget.end(), g) == mergeTarget.end())
                            mergeTarget.push_back(g);
                    }
                }
            }
        }

        // This sweep's removal discriminator is the dep graph: a hop goes when
        // nothing references the source any more. It is deliberately not the
        // discriminator the registration and reconciler paths use - those
        // remove a hop when its SOURCE has a live record again, which says
        // nothing about who still points at it. The two are separate contracts
        // and are not interchangeable.
        //
        // The discriminator is only sound because the index was warmed above
        // (#1008): the reverse index is built on demand, so without that an
        // empty result here means "nobody asked", not "nobody refers".
        //
        // Collect every dependent in one pass; the rewrite loop needs the
        // full set so we can flag anySkipped accurately.
        std::vector<GUID> dependents;
        cache->IterateDependents(from, [&dependents](const GUID& g) {
            dependents.push_back(g);
            return true;
        });
        if (dependents.empty())
        {
            (void)store->RemoveRedirect(from, nullptr);
            if (auto pinnedDirty = ProjectSourcePinned())
                pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
            continue;
        }

        bool anySkipped = false;
        for (const GUID& dep : dependents)
        {
            AssetMetadata md{};
            if (!TryGetAssetMetadata(dep, md))
            {
                anySkipped = true;
                continue;
            }

            // Only attempt fix-up for text-based assets (best-effort).
            std::string ext = md.Extension;
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });

            if (!IsTextBasedAssetType(md.Type) || ext == ".spv")
            {
                anySkipped = true;
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::exists(md.Path, ec) || !std::filesystem::is_regular_file(md.Path, ec))
            {
                anySkipped = true;
                continue;
            }

            bool changed = false;
            if (!FixupTextFileReplaceGuid(md.Path, from, chainFinal, changed))
            {
                anySkipped = true;
                continue;
            }

            if (changed)
            {
                ++modifiedFiles;

                // Refresh derived deps for this asset so reverse lookups stay
                // correct. Parser-driven format-aware path preferred; syntactic
                // fallback if no parser implements ExtractDependencies for
                // this asset's type yet.
                std::vector<DepEdge> parserEdges;
                if (TryExtractParserDepEdges(m_ParserRegistry, md.Guid, md, parserEdges))
                {
                    (void)cache->ReplaceDependencies(md.Guid, parserEdges, nullptr);
                }
                else
                {
                    AssetDependencyInfo info = AssetDependencyExtractor::ExtractDependencies(md);
                    AssetDependencyExtractor::ResolveDependencyPathsToGuids(info, md, *this);
                    (void)cache->ReplaceDependencies(md.Guid, info.Dependencies, nullptr);
                }
            }
        }

        // If we successfully updated all applicable dependents, remove redirect when no longer referenced.
        if (cache->CountDependents(from) == 0 && !anySkipped)
        {
            (void)store->RemoveRedirect(from, nullptr);
            if (auto pinnedDirty = ProjectSourcePinned())
                pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
        }
    }

    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        RefreshProjectRedirectFlagLocked();
    }
    return modifiedFiles;
}

bool AssetRegistry::TryRenameAssetPath(const std::filesystem::path& oldPath,
                                       const std::filesystem::path& newPath)
{
    if (oldPath.empty() || newPath.empty())
    {
        return false;
    }
    if (oldPath == newPath)
    {
        return true;
    }

    const std::filesystem::path projectRoot = GetAssetRoot();
    const std::filesystem::path normOld = NormalizePathForMap(
        oldPath.is_relative() ? (projectRoot / oldPath) : oldPath);
    const std::filesystem::path normNew = NormalizePathForMap(
        newPath.is_relative() ? (projectRoot / newPath) : newPath);

    // F.1: reject if either endpoint falls under an immutable source.
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (const SourceEntry* srcOld = FindSourceForPath(normOld); srcOld && srcOld->IsImmutable)
        {
            Logger::Log::Warning("AssetRegistry::TryRenameAssetPath: source '{}' is immutable; "
                                 "rejecting rename from '{}'", srcOld->Alias, normOld.string());
            return false;
        }
        if (const SourceEntry* srcNew = FindSourceForPath(normNew); srcNew && srcNew->IsImmutable)
        {
            Logger::Log::Warning("AssetRegistry::TryRenameAssetPath: source '{}' is immutable; "
                                 "rejecting rename to '{}'", srcNew->Alias, normNew.string());
            return false;
        }
    }

    // Only perform project-store updates for assets that live under the configured asset root.
    // If a file moves between persistent and non-persistent source roots, let the caller handle it as unregister+register.
    std::string canonicalOld;
    std::string canonicalNew;
    const bool oldPersistent = AssetPaths::TryMakeCanonicalRelativePath(projectRoot, normOld, canonicalOld);
    // The store record keeps the caller's spelling of the new path.
    const bool newPersistent = AssetPaths::TryMakeCanonicalRelativePath(
        projectRoot, newPath.is_relative() ? (projectRoot / newPath) : newPath, canonicalNew);
    if (oldPersistent != newPersistent)
    {
        return false;
    }

    // Derived-identity rename: the GUID stays attached to the asset across the
    // rename (so in-memory consumers don't churn), but under derived identity
    // the new path hashes to a DIFFERENT GUID — so anything that re-derives from
    // the new path (a fresh DB build, the Player) must be able to chase the kept
    // GUID forward. Capture old/new-derived here and append a redirect after the
    // write lock is released (AddRedirect takes its own locks). Active for the
    // project source (DerivedIdentity=true).
    GUID renameRedirectFrom = GUID::Null();
    GUID renameRedirectTo = GUID::Null();

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

        const GUID* oldBinding = FindPathGuidLocked(PathMapKey(normOld));
        if (!oldBinding)
        {
            return false;
        }

        const GUID guid = *oldBinding;

        // Rename onto an already-registered destination (safe-save: write a
        // temp file, then rename it over the target) is a content replacement,
        // not an identity move. The destination keeps its GUID — consumers and
        // the reload path see the same asset with new bytes — and the source
        // entry is unregistered: its path just ceased to exist. Rebinding the
        // source entry onto the destination path instead would strand a
        // derived-identity entry at a path that derives a different GUID, so
        // the next registration of the source path (the following safe-save
        // cycle) would hit the derive-collision guard and be refused.
        if (const GUID* newBinding = FindPathGuidLocked(PathMapKey(normNew));
            newBinding && *newBinding != guid)
        {
            const GUID targetGuid = *newBinding;
            writeLock.unlock();
            Logger::Log::Debug(
                "AssetRegistry: rename '{}' -> '{}' replaces registered asset {}; "
                "keeping the target's identity and unregistering source entry {}",
                normOld.string(), normNew.string(), targetGuid.ToString(), guid.ToString());
            // Takes registry locks itself. Watcher events are delivered
            // serially, so the source path can't be re-registered between the
            // check above and this call in practice; a lost race self-heals on
            // the path's next Created/Modified event.
            (void)TryUnregisterAssetByPath(normOld);
            return true;
        }

        // The destination is not registered right now, but the authoritative
        // store may still own the destination path's derived GUID — e.g. an
        // external safe-save whose temp lingered after a failed replace: the
        // real file's Deleted outlived the watcher hold and consumed the
        // registration before the rename finally landed. Riding the source
        // GUID here would migrate the asset onto a temp-derived identity
        // (fresh GUID for the real path, original GUID dangling). Restore the
        // destination's own identity instead: a fresh registration re-derives
        // it deterministically and resurrects the store record.
        if (newPersistent && m_ProjectSource && m_ProjectSource->DerivedIdentity && ProjectStore())
        {
            const GUID destDerived = DeriveSourceScopedGuid(
                m_ProjectSource->NamespaceGuid, m_ProjectSource->Alias, canonicalNew);
            AssetDatabase::AssetRecord destRec{};
            if (!destDerived.IsNull() && destDerived != guid &&
                ProjectStore()->TryGetAsset(destDerived, destRec) &&
                AssetPaths::FoldStorePathKey(destRec.path) == AssetPaths::FoldStorePathKey(canonicalNew))
            {
                writeLock.unlock();
                Logger::Log::Debug(
                    "AssetRegistry: rename '{}' -> '{}' lands on a path whose identity {} the "
                    "store still owns; restoring it and unregistering source entry {}",
                    normOld.string(), normNew.string(), destDerived.ToString(), guid.ToString());
                (void)TryUnregisterAssetByPath(normOld);
                return RegisterAsset(newPath);
            }
        }

        if (newPersistent && m_ProjectSource && m_ProjectSource->DerivedIdentity)
        {
            const GUID newDerived = DeriveSourceScopedGuid(
                m_ProjectSource->NamespaceGuid, m_ProjectSource->Alias, canonicalNew);
            if (!newDerived.IsNull() && newDerived != guid)
            {
                renameRedirectFrom = guid;
                renameRedirectTo = newDerived;
            }
        }

        // Remove old mapping first to avoid transient duplicate mapping to the same GUID.
        ErasePathMappingLocked(PathMapKey(normOld));

        const std::string newKey = PathMapKey(normNew);
        PathShard(newKey)[newKey] = guid;

        // S11: a rename can cross source roots; re-attribute the index
        // membership from the old path's source to the new path's.
        UnindexResidentGuidLocked(guid);
        IndexResidentGuidLocked(guid, normNew);

        AssetMetadata* metaPtr = FindAssetMutableLocked(guid);
        if (!metaPtr)
        {
            return false;
        }

        AssetMetadata& metadata = *metaPtr;
        metadata.Path = normNew;
        metadata.Name = newPath.stem().string();
        metadata.Extension = GetCompoundExtensionFromPath(normNew.string());
        // Avoid calling ClassifyAssetType() under the registry lock (it also locks). Inline classification.
        AssetType newType = AssetType::Unknown;
        if (m_ParserRegistry)
        {
            if (auto parser = m_ParserRegistry->FindParser(normNew))
            {
                newType = parser->GetAssetType();
            }
        }
        if (newType == AssetType::Unknown)
        {
            newType = m_TypeRegistry.GetAssetTypeFromExtension(metadata.Extension);
        }
        metadata.Type = newType;
        metadata.TypeId = AssetTypeToString(newType);

        // Best-effort filesystem stats update (may fail for non-regular files).
        std::error_code ec;
        if (std::filesystem::exists(normNew, ec) && std::filesystem::is_regular_file(normNew, ec))
        {
            std::error_code ec2;
            metadata.LastModified = std::filesystem::last_write_time(normNew, ec2);
            std::error_code ec3;
            metadata.FileSize = std::filesystem::file_size(normNew, ec3);
        }

        // Update the authoritative record for the renamed path.
        if (newPersistent && ProjectStore())
        {
            AssetDatabase::AssetRecord rec{};
            if (!ProjectStore()->TryGetAsset(guid, rec))
            {
                rec.guid = guid;
            }
            rec.path = canonicalNew;
            rec.type = metadata.Type;
            rec.typeId = metadata.TypeId;
            rec.missing = false;

            (void)ProjectStore()->UpsertAsset(rec, nullptr);
            // writeLock above keeps m_ProjectSource stable; calling
            // ProjectSourcePinned() here would recurse on the shared_mutex.
            m_ProjectSource->StoreDirty.store(true, std::memory_order_relaxed);

            if (ProjectCache())
            {
                (void)ProjectCache()->UpsertAsset(rec, nullptr);
            }
        }
    }

    // If this is a non-persistent rename, best-effort update the non-persistent store/cache too.
    if (!newPersistent)
    {
        SourceEntry* npSrc = nullptr;
        AssetDatabase::IAssetStore* npStore = nullptr;
        AssetDatabase::IAssetDbCache* npCache = nullptr;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            npSrc = FindSourceForPath(normNew);
            if (npSrc && npSrc != m_ProjectSource.get() && npSrc->HasStore())
            {
                npStore = npSrc->Store.get();
                npCache = npSrc->Cache.get();
            }
            else
            {
                npSrc = nullptr;
            }
        }
        if (npStore)
        {
            const GUID guid = GetAssetGUID(normNew);
            std::string keyNew;
            GUID nsGuid;
            std::string canonicalRelNew;
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                if (!guid.IsNull() &&
                    TryBuildSourceScopedKeyForPath(normNew, keyNew, nsGuid, &canonicalRelNew) &&
                    !keyNew.empty() &&
                    !canonicalRelNew.empty())
                {
                    AssetMetadata md{};
                    if (TryGetAssetMetadata(guid, md))
                    {
                        AssetDatabase::AssetRecord rec{};
                        if (!npStore->TryGetAsset(guid, rec))
                        {
                            rec.guid = guid;
                        }
                        rec.path = canonicalRelNew;
                        rec.type = md.Type;
                        rec.typeId = md.TypeId;
                        rec.missing = false;
                        (void)npStore->UpsertAsset(rec, nullptr);
                        npSrc->StoreDirty.store(true, std::memory_order_relaxed);
                        if (npCache)
                        {
                            (void)npCache->UpsertAsset(rec, nullptr);
                        }
                    }
                }
            }
        }
    }

    // Derived-identity rename: persist the old->new-derived redirect so the kept
    // GUID resolves forward once the project re-derives from the new path.
    // AddRedirect locks internally; safe now that the write lock is released.
    if (!renameRedirectFrom.IsNull() && !renameRedirectTo.IsNull())
    {
        (void)AddRedirect(renameRedirectFrom, renameRedirectTo);

        // Cascade the container's journaled subasset derive keys (same
        // mechanism as the offline heal, AssetStoreReconciler): a fresh
        // re-derivation of the new path mints Derive(new, key) while persisted
        // references hold Derive(old, key). The record kept the old GUID, so
        // its kv row is read under the old identity.
        AssetDatabase::IAssetStore* store = nullptr;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            store = ProjectStore();
        }
        AssetDatabase::AssetRecord rec{};
        if (store && store->TryGetAsset(renameRedirectFrom, rec))
        {
            if (auto it = rec.kv.find(kSubassetDeriveKeysKvKey); it != rec.kv.end())
            {
                for (const String& key : SplitSubassetDeriveKeys(it->second))
                {
                    (void)AddRedirect(GUID::Derive(renameRedirectFrom, key),
                                      GUID::Derive(renameRedirectTo, key));
                }
            }
        }
    }

    return true;
}

bool AssetRegistry::TryUnregisterAssetByPath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    const std::filesystem::path projectRoot = GetAssetRoot();
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (projectRoot / path) : path);

    GUID guid = GUID::Null();
    AssetMetadata metadataCopy{};
    bool haveMetadata = false;

    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    std::filesystem::path assetRoot;

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        // F.1: reject if path is in an immutable source.
        if (const SourceEntry* src = FindSourceForPath(normPath); src && src->IsImmutable)
        {
            Logger::Log::Warning("AssetRegistry::TryUnregisterAssetByPath: source '{}' is "
                                 "immutable; rejecting unregister of '{}'",
                                 src->Alias, normPath.string());
            return false;
        }
        const GUID* bound = FindPathGuidLocked(PathMapKey(normPath));
        if (!bound)
        {
            return false;
        }

        guid = *bound;
        if (guid.IsNull())
        {
            return false;
        }

        if (const AssetMetadata* meta = FindAssetLocked(guid))
        {
            metadataCopy = *meta;
            haveMetadata = true;
        }

        // Remove from in-memory registry maps.
        m_AssetSourceOwner.erase(PathMapKey(normPath));
        ErasePathMappingLocked(PathMapKey(normPath));
        UnindexResidentGuidLocked(guid);
        if (EraseAssetEntryLocked(guid))
            ApplyAssetCountDelta(-1);

        // Store/cache pointers for post-unlock durable update.
        store = ProjectStore();
        cache = ProjectCache();
        assetRoot = ProjectRoot();
    }

    // Only project assets (under assetRoot) participate in authoritative tombstones.
    // Mounted editor assets are tracked in-memory only to avoid polluting the project database.
    std::string canonicalRel;
    const bool persistent = AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normPath, canonicalRel);
    if (!persistent)
    {
        // Best-effort: mark missing in non-persistent store to preserve metadata/identity (Editor/local DB).
        SourceEntry* npSrc = nullptr;
        AssetDatabase::IAssetStore* npStore = nullptr;
        AssetDatabase::IAssetDbCache* npCache = nullptr;
        bool npTracksTombstones = true;
        bool npDerivedIdentity = false;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            npSrc = FindSourceForPath(normPath);
            if (npSrc && npSrc != m_ProjectSource.get() && npSrc->HasStore())
            {
                npStore = npSrc->Store.get();
                npCache = npSrc->Cache.get();
                npTracksTombstones = npSrc->TracksTombstones;
                npDerivedIdentity = npSrc->DerivedIdentity;
            }
            else
            {
                npSrc = nullptr;
            }
        }
        // F.1: TracksTombstones=false → just remove the record outright
        // (transient/scratch mounts don't preserve identity for missing).
        if (npStore && !npTracksTombstones)
        {
            (void)npStore->RemoveAsset(guid, nullptr);
            if (npCache)
                (void)npCache->RemoveAsset(guid, nullptr);
            // Provenance lives only in the project source's cache by
            // design (H1 simplification — non-project sources are typically
            // read-only Package mounts and can't host writable rows). This
            // is the non-project deregistration path, so a provenance row
            // for `guid` shouldn't exist. Still call UnregisterProvenance
            // defensively — it's a no-op when no row exists and a one-time
            // map-clean if some future code path lands a project-source row
            // referencing a non-project GUID.
            UnregisterProvenance(guid);
            if (npSrc)
                npSrc->StoreDirty.store(true, std::memory_order_relaxed);
            return true;
        }
        if (npStore)
        {
            std::string key;
            GUID nsGuid;
            std::string sourceCanonicalRel;
            AssetDatabase::AssetRecord rec{};
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                if (TryBuildSourceScopedKeyForPath(normPath, key, nsGuid, &sourceCanonicalRel) &&
                    !key.empty() &&
                    !sourceCanonicalRel.empty())
                {
                    if (!npStore->TryGetAsset(guid, rec))
                    {
                        rec.guid = guid;
                        rec.path = sourceCanonicalRel;
                        rec.type = haveMetadata ? metadataCopy.Type : AssetType::Unknown;
                        rec.typeId = haveMetadata && !metadataCopy.TypeId.empty() ? metadataCopy.TypeId : AssetTypeToString(rec.type);
                    }
                    rec.path = sourceCanonicalRel;
                    // Residence follows the identity scheme, exactly as the
                    // reconcile pass does it: a derived mount's journal holds
                    // identity and user metadata only, so the tombstone rests
                    // in the per-machine cache and this delete leaves the
                    // git-shared file alone.
                    rec.missing = !npDerivedIdentity;
                    if (npDerivedIdentity)
                    {
                        // Keep the identity record (creating it if the delete
                        // beat registration) and clear any flag a pre-residence
                        // session journaled. Both calls suppress themselves
                        // when nothing changes.
                        (void)npStore->UpsertAsset(rec, nullptr);
                        (void)npStore->MarkMissing(guid, false, nullptr);
                    }
                    else if (!npStore->MarkMissing(guid, true, nullptr))
                    {
                        (void)npStore->UpsertAsset(rec, nullptr);
                    }
                    npSrc->StoreDirty.store(true, std::memory_order_relaxed);
                }
            }
            if (npCache && !sourceCanonicalRel.empty())
            {
                (void)npCache->SetAssetMissing(rec, true, npCache->GetSessionCounter(), nullptr);
            }
        }
        return true;
    }

    // Mark missing in the authoritative store so GUID identity is preserved (tombstone semantics).
    if (store)
    {
        // F.1: TracksTombstones=false → remove the record instead of marking
        // missing. The default project source has TracksTombstones=true so
        // this branch typically only fires on unusual configurations.
        bool projectTracksTombstones = true;
        bool projectAggressiveCleanup = true;
        bool projectDerivedIdentity = false;
        {
            auto pinned = ProjectSourcePinned();
            if (pinned)
            {
                projectTracksTombstones = pinned->TracksTombstones;
                projectAggressiveCleanup = pinned->AggressiveTombstoneCleanup;
                projectDerivedIdentity = pinned->DerivedIdentity;
            }
        }

        if (!projectTracksTombstones)
        {
            (void)store->RemoveAsset(guid, nullptr);
            if (cache)
                (void)cache->RemoveAsset(guid, nullptr);
            // Provenance row drops alongside the SQLite asset row. Use the
            // registry wrapper (not direct cache) so the in-memory cache
            // maps stay consistent. No registry lock is held in this path,
            // so the wrapper's own lock acquisition is safe.
            UnregisterProvenance(guid);
            if (auto pinnedDirty = ProjectSourcePinned())
                pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);
            // Cascade: this asset's outgoing deps may now be orphaned.
            if (cache && projectAggressiveCleanup)
            {
                auto outgoing = cache->GetDependencies(guid);
                for (const GUID& o : outgoing)
                    CleanupOrphanedDependenciesCascade(o, store, cache);
            }
            return true;
        }

        // AggressiveTombstoneCleanup: when nothing live depends on this asset
        // and it has no path-form referrers / no produced sidecars, real
        // removal beats tombstoning — hygiene over time. Falls through to
        // the tombstone path if any check says "still referenced".
        //
        // The eligibility check + cleanup mutation are wrapped in a single
        // writer-lock acquisition to close a TOCTOU race: a concurrent
        // RegisterAsset between check and mutation could install a new
        // referrer we'd then silently miss. Matches the cascade's invariant.
        //
        // Pin the project source BEFORE the writer lock so we can mark it
        // dirty after the lock is released — ProjectSourcePinned() itself
        // takes a shared_lock on the same mutex, which would self-deadlock
        // if called while we hold the writer.
        if (projectAggressiveCleanup && cache)
        {
            auto projectPin = ProjectSourcePinned();

            std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);

            // Stream GUID-form referrers; stop on first live one (in m_Assets).
            bool eligible = true;
            cache->IterateDependents(guid, [&](const GUID& referrer) {
                if (FindAssetLocked(referrer) != nullptr)
                {
                    eligible = false;
                    return false; // stop
                }
                return true;
            });
            if (eligible && !canonicalRel.empty())
            {
                // Any path-form referrer disqualifies.
                cache->IterateDependentsByPath(canonicalRel, [&](const GUID&) {
                    eligible = false;
                    return false; // stop on first hit
                });
            }
            if (eligible && !cache->EnumerateProducedAssets(guid).empty())
                eligible = false;

            if (eligible)
            {
                // Capture outgoing for cascade BEFORE removal. cache->RemoveAsset
                // clears both edge directions in the deps table atomically.
                auto outgoing = cache->GetDependencies(guid);
                (void)store->RemoveAsset(guid, nullptr);
                (void)cache->RemoveAsset(guid, nullptr);

                // Inline provenance cleanup (mirror of the cascade): we hold
                // the writer lock and can't call UnregisterProvenance, which
                // would try to re-acquire it. Skipping PopulateProvenanceCache-
                // Locked is safe — see cascade for the rationale.
                auto producerIt = m_ProvenanceProducer.find(guid);
                if (producerIt != m_ProvenanceProducer.end())
                {
                    const GUID producerOfRemoved = producerIt->second;
                    m_ProvenanceProducer.erase(producerIt);
                    auto producedListIt = m_ProvenanceProduced.find(producerOfRemoved);
                    if (producedListIt != m_ProvenanceProduced.end())
                    {
                        auto& vec = producedListIt->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), guid), vec.end());
                        if (vec.empty())
                            m_ProvenanceProduced.erase(producedListIt);
                    }
                }
                (void)cache->UnregisterProvenance(guid, nullptr);

                writeLock.unlock();

                // Mark store dirty AFTER releasing the registry lock — the
                // pre-acquired pin lets us touch the atomic safely.
                if (projectPin)
                    projectPin->StoreDirty.store(true, std::memory_order_relaxed);

                for (const GUID& o : outgoing)
                    CleanupOrphanedDependenciesCascade(o, store, cache);
                return true;
            }
        }

        AssetDatabase::AssetRecord rec{};
        if (!store->TryGetAsset(guid, rec))
        {
            rec.guid = guid;
            rec.path = canonicalRel;
            rec.type = haveMetadata ? metadataCopy.Type : AssetType::Unknown;
            rec.typeId = haveMetadata && !metadataCopy.TypeId.empty() ? metadataCopy.TypeId : AssetTypeToString(rec.type);
        }

        // Ensure the canonical path is up to date; where the missing flag
        // then rests follows the identity scheme, exactly as the reconcile
        // pass does it. A derived mount's journal holds identity and
        // user-authored metadata only — a per-machine disk observation like
        // "this file is gone" belongs in the per-machine cache, so a delete
        // leaves the git-shared .assetdb alone.
        rec.path = canonicalRel;
        rec.missing = !projectDerivedIdentity;

        if (projectDerivedIdentity)
        {
            // Keep the identity record (creating it if the delete beat
            // registration) and clear any flag a pre-residence session
            // journaled. Both calls suppress themselves when nothing changes,
            // so the steady state writes nothing.
            (void)store->UpsertAsset(rec, nullptr);
            (void)store->MarkMissing(guid, false, nullptr);
        }
        // Prefer MarkMissing if record exists; otherwise upsert a tombstone record.
        else if (!store->MarkMissing(guid, true, nullptr))
        {
            (void)store->UpsertAsset(rec, nullptr);
        }

        if (auto pinnedDirty = ProjectSourcePinned())
            pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);

        if (cache)
        {
            // The stamp starts the ghost's retention clock at the session the
            // delete happened, so a live delete and an offline one age alike.
            (void)cache->SetAssetMissing(rec, true, cache->GetSessionCounter(), nullptr);
        }
        // No cache on a derived mount (the SQLite cache opens only once the
        // .assetdb exists) means the observation has nowhere to rest, so the
        // record keeps its identity but carries no flag until a session that
        // HAS a cache runs the reconcile pass and re-derives it from disk.
        // Same degradation the derived reconcile pass takes on a null cache,
        // and the same reason: a flag whose residence depended on whether a
        // cache happened to be open would land per-machine state in the
        // git-shared journal exactly on a fresh clone.
    }

    return true;
}

bool AssetRegistry::TryUpdateFilesystemMetadata(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    const std::filesystem::path projectRoot = GetAssetRoot();
    const std::filesystem::path normPath = NormalizePathForMap(
        path.is_relative() ? (projectRoot / path) : path);

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        const GUID* bound = FindPathGuidLocked(PathMapKey(normPath));
        if (!bound)
        {
            return false;
        }

        AssetMetadata* metaPtr = FindAssetMutableLocked(*bound);
        if (!metaPtr)
        {
            return false;
        }

        AssetMetadata& metadata = *metaPtr;

        std::error_code ec;
        if (!std::filesystem::exists(normPath, ec))
        {
            return false;
        }
        if (!std::filesystem::is_regular_file(normPath, ec))
        {
            return false;
        }

        std::error_code ec2;
        const auto newMtime = std::filesystem::last_write_time(normPath, ec2);
        std::error_code ec3;
        const auto newSize = std::filesystem::file_size(normPath, ec3);

        // Content changed since the last stat → any lazily-extracted dependency
        // edges are stale. Drop the extracted flag so the next GetDependencies
        // re-extracts; without this, a same-session edit (editor save of a
        // .material/.scene) kept serving the first extraction's edge set forever.
        if (!ec2 && !ec3 &&
            (metadata.LastModified != newMtime || metadata.FileSize != static_cast<size_t>(newSize)))
        {
            metadata.DependenciesExtracted = false;
        }
        if (!ec2)
            metadata.LastModified = newMtime;
        if (!ec3)
            metadata.FileSize = static_cast<size_t>(newSize);
    }

    return true;
}

void AssetRegistry::ScanDirectory(const std::filesystem::path& directory, bool recursive)
{
    if (!std::filesystem::exists(directory))
    {
        return;
    }

    Logger::Log::Debug("Scanning directory: {}", directory.string());

    // Use ignore rules rooted at the directory being scanned. If scanning the primary asset root,
    // include any <AssetRoot>/.assetignore overrides; otherwise fall back to defaults.
    AssetIgnoreRules rules = AssetIgnoreRules::CreateDefault();
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (NormalizePathForMap(directory) == NormalizePathForMap(ProjectRoot()))
        {
            rules = ProjectIgnoreRules();
        }
    }

    try
    {
        if (recursive)
        {
            // Apply the central AssetIgnoreRules (same as startup reconciliation):
            // hidden files, the authoritative DB, derived cache folders, and the
            // default ignored extensions.
            const auto files = EnumerateAssetFilesOnDisk(directory, rules);
            for (const auto& p : files)
            {
                (void)RegisterAsset(p);
            }
        }
        else
        {
            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                if (entry.is_regular_file())
                {
                    const auto p = entry.path();
                    if (rules.ShouldIgnoreFile(p, directory))
                        continue;
                    (void)RegisterAsset(p);
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Error("Error scanning directory {}: {}", directory.string(), e.what());
    }

    // Same tail contract as the async pipeline (RegistryUpdateTask): a
    // full-mount scan of a derived source reconciles before control returns.
    // This synchronous path registers per-file (RegisterAsset), so it
    // collects no rename-heal candidates — missing-marking only; heals run
    // on the batch pipeline (startup scan, ScanDirectoryAsync).
    ReconcileDerivedSourceAfterScan(directory, nullptr);
}

std::future<size_t> AssetRegistry::ScanDirectoryAsync(const std::filesystem::path& directory, bool recursive)
{
    AsyncRegistryCoordinator* coordinator = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        coordinator = m_AsyncCoordinator.get();
    }

    if (!coordinator)
    {
        Logger::Log::Warning("No async coordinator available, falling back to synchronous scan");

        // Fallback to synchronous operation
        std::promise<size_t> promise;
        ScanDirectory(directory, recursive);
        promise.set_value(GetAssetCount());
        return promise.get_future();
    }

    return coordinator->ScanDirectoryAsync(directory, recursive);
}

std::future<bool> AssetRegistry::LoadAssetMetadataAsync(const std::filesystem::path& assetPath)
{
    // Metadata lives in the authoritative store + derived cache (no sidecar files),
    // so a metadata "load" is just ensuring the asset is registered and present
    // in the store.
    std::promise<bool> promise;
    promise.set_value(RegisterAsset(assetPath));
    return promise.get_future();
}

std::future<bool> AssetRegistry::SaveAssetMetadataAsync(const AssetMetadata& metadata)
{
    // Saving metadata is now a store upsert; the on-disk flush happens later (or on shutdown).
    std::promise<bool> promise;
    promise.set_value(SaveAssetMetadata(metadata));
    return promise.get_future();
}

bool AssetRegistry::SaveToFile(const std::filesystem::path& path)
{
    AssetDatabase::IAssetStore* store = nullptr;
    std::filesystem::path filePath;
    std::filesystem::path defaultPath;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        defaultPath = ProjectDbFile();
        filePath = path.empty() ? defaultPath : path;
    }

    if (!store)
    {
        Logger::Log::Warning("AssetRegistry::SaveToFile: no asset store configured");
        return false;
    }

    std::string err;
    if (!store->SaveToFile(filePath, &err))
    {
        Logger::Log::Error("AssetRegistry::SaveToFile: failed to write '{}': {}", filePath.string(), err);
        return false;
    }

    // Only clear dirty when we wrote the authoritative file.
    if (path.empty() || filePath == defaultPath)
    {
        if (auto pinned = ProjectSourcePinned())
        {
            pinned->StoreDirty.store(false, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(pinned->StoreFlushMutex);
            pinned->LastStoreFlush = std::chrono::steady_clock::now();
        }
    }

    return true;
}

void AssetRegistry::TickPersistence()
{
    bool anyDirty = false;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (m_ProjectSource && m_ProjectSource->StoreDirty.load(std::memory_order_relaxed))
        {
            anyDirty = true;
        }
    }
    if (!anyDirty)
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        for (const auto& src : m_Sources)
        {
            if (src->StoreDirty.load(std::memory_order_relaxed))
            {
                anyDirty = true;
                break;
            }
        }
    }
    if (!anyDirty)
        return;

    // Avoid flushing the full authoritative DB while the startup scan is still in-flight.
    // The flush is O(N) over the entire store and can cause multi-second stalls on the main thread,
    // which defeats the goal of async startup scanning.
    //
    // Additionally, even when the scan just finished, skip ONE tick so the first frame after completion
    // doesn't pay the full flush cost (tests measure worst-case Update() time during scanning).
    std::future<size_t> finishedStartupScan;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (m_ProjectSource && m_ProjectSource->StartupScanFuture.valid())
        {
            using namespace std::chrono_literals;
            const auto status = m_ProjectSource->StartupScanFuture.wait_for(0ms);
            if (status != std::future_status::ready)
            {
                return;
            }
        }
    }
    {
        // If the scan is finished, consume and clear the future so subsequent ticks can flush normally.
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (m_ProjectSource && m_ProjectSource->StartupScanFuture.valid())
        {
            finishedStartupScan = std::move(m_ProjectSource->StartupScanFuture);
        }
    }
    if (finishedStartupScan.valid())
    {
        try
        {
            (void)finishedStartupScan.get();
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("AssetRegistry: startup scan ended with exception: {}", e.what());
        }
        catch (...)
        {
            Logger::Log::Warning("AssetRegistry: startup scan ended with unknown exception");
        }
        // Skip flushing on the completion tick to keep main thread responsive.
        return;
    }

    // Throttle flush frequency; SaveToFile() is deterministic but can be expensive on large projects.
    constexpr auto kMinFlushInterval = std::chrono::milliseconds(750);
    const auto now = std::chrono::steady_clock::now();

    // Helper: submit an async flush for a source. Returns immediately; the
    // actual SaveToFile runs on a JobSystem worker so the main thread never
    // stalls on database writes. Uses flushInProgress to prevent concurrent
    // flushes on the same source.
    auto submitAsyncFlush = [this](SourceEntry* src, AssetDatabase::IAssetStore* store,
                                    std::filesystem::path dbFile, bool isProject) {
        // Atomic claim: only one thread proceeds if multiple ticks see dirty=true
        // before any flush starts.
        bool expected = false;
        if (!src->FlushInProgress.compare_exchange_strong(expected, true))
            return;

        // Clear dirty BEFORE submitting. If RegisterAsset happens during the flush,
        // it'll set dirty=true again, and the next tick will pick it up.
        src->StoreDirty.store(false, std::memory_order_relaxed);
        src->LastStoreFlush = std::chrono::steady_clock::now();

        std::string alias = src->Alias;

        if (m_JobSystem)
        {
            m_JobSystem->Submit([src, store, dbFile = std::move(dbFile), alias, isProject]() {
                FlushClaimRelease release{src->FlushInProgress};
                std::string err;
                // Never park a job worker on another process's write lock: the
                // retry below is what this path has instead of waiting.
                if (!store->SaveToFile(dbFile, &err,
                                       AssetDatabase::StoreSaveWait::NonBlocking))
                {
                    Logger::Log::Warning("AssetRegistry: async flush failed for '{}': {}",
                                         dbFile.string(), err);
                    // Restore dirty flag so a future tick retries the write.
                    src->StoreDirty.store(true, std::memory_order_relaxed);
                }
            });
        }
        else
        {
            // No job system (e.g. unit tests) — do it synchronously.
            FlushClaimRelease release{src->FlushInProgress};
            std::string err;
            if (!store->SaveToFile(dbFile, &err, AssetDatabase::StoreSaveWait::NonBlocking))
            {
                Logger::Log::Warning("AssetRegistry: sync flush failed for '{}': {}",
                                     dbFile.string(), err);
                src->StoreDirty.store(true, std::memory_order_relaxed);
            }
        }
    };

    // Project store flush. IsReadOnly sources skip persistent writes
    // entirely (CI / headless / shared-volume scenarios).
    if (m_ProjectSource && m_ProjectSource->StoreDirty.load(std::memory_order_relaxed)
        && !m_ProjectSource->FlushInProgress.load(std::memory_order_acquire)
        && !m_ProjectSource->IsReadOnly)
    {
        std::lock_guard<std::mutex> lk(m_ProjectSource->StoreFlushMutex);
        if (m_ProjectSource->LastStoreFlush.time_since_epoch().count() == 0 || (now - m_ProjectSource->LastStoreFlush) >= kMinFlushInterval)
        {
            AssetDatabase::IAssetStore* store = nullptr;
            std::filesystem::path dbFile;
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                store = ProjectStore();
                dbFile = ProjectDbFile();
            }

            if (store && !dbFile.empty())
            {
                submitAsyncFlush(m_ProjectSource.get(), store, std::move(dbFile), true);
            }
        }
    }

    // Flush all non-project sources that are dirty.
    std::vector<std::string> dirtyNonProjectAliases;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        dirtyNonProjectAliases.reserve(m_Sources.size());
        for (const auto& src : m_Sources)
        {
            if (src.get() == m_ProjectSource.get())
                continue;
            if (!src->StoreDirty.load(std::memory_order_relaxed))
                continue;
            if (src->FlushInProgress.load(std::memory_order_acquire))
                continue;
            if (src->IsReadOnly)
                continue; // Read-only mounts skip persistent writes.
            // E6b: suspend flushes while this source's startup scan is in
            // flight — the project store is already scan-gated above, but a
            // bulk import into a non-project mount used to trigger a full
            // sorted store rewrite every 750ms once appends exceeded the
            // compaction threshold.
            if (src->StartupScanFuture.valid() &&
                src->StartupScanFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            dirtyNonProjectAliases.push_back(src->Alias);
        }
    }
    for (const std::string& alias : dirtyNonProjectAliases)
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        SourceEntry* src = FindSourceByAlias(alias);
        if (!src || src == m_ProjectSource.get())
            continue;
        if (!src->StoreDirty.load(std::memory_order_relaxed))
            continue;

        std::lock_guard<std::mutex> lk(src->StoreFlushMutex);
        if (src->LastStoreFlush.time_since_epoch().count() == 0 || (now - src->LastStoreFlush) >= kMinFlushInterval)
        {
            AssetDatabase::IAssetStore* store = src->Store.get();
            std::filesystem::path dbFile = src->DbFile;
            if (store && !dbFile.empty())
            {
                submitAsyncFlush(src, store, std::move(dbFile), false);
            }
        }
    }
}

bool AssetRegistry::LoadFromFile(const std::filesystem::path& path)
{
    const std::filesystem::path filePath = path.empty() ? ProjectDbFile() : path;

    auto newStore = GameEngine::MakeUnique<AssetDatabase::AssetStore_TextJsonl>(m_JobSystem);
    std::string err;
    if (!newStore->LoadFromFile(filePath, &err))
    {
        Logger::Log::Error("AssetRegistry::LoadFromFile: failed to read '{}': {}", filePath.string(), err);
        return false;
    }

    bool legacyPathsDirty = false;
    {
        std::filesystem::path assetRoot;
        AssetDatabase::IAssetDbCache* cache = nullptr;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            assetRoot = ProjectRoot();
            cache = ProjectCache();
        }

        size_t fixed = 0;
        size_t conflicts = 0;
        legacyPathsDirty = AssetDatabase::NormalizeLegacyStorePathsIfNeeded(assetRoot, *newStore, cache, fixed, conflicts);
        if (fixed > 0)
        {
            Logger::Log::Info("AssetRegistry: normalized {} legacy 'Assets/'-prefixed paths while loading '{}'",
                              fixed, filePath.string());
        }
        if (conflicts > 0)
        {
            Logger::Log::Warning("AssetRegistry: skipped {} legacy path normalizations due to existing path collisions while loading '{}'",
                                 conflicts, filePath.string());
        }
    }

    // Type fix-up on load: upgrade Unknown/invalid persisted types using extension inference.
    bool typesDirty = false;
    {
        size_t upgraded = 0;
        AssetDatabase::IAssetDbCache* cachePtr = nullptr;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            cachePtr = ProjectCache();
        }

        const std::vector<AssetDatabase::AssetRecord> recs = newStore->EnumerateAssets();
        for (const auto& rec : recs)
        {
            if (rec.guid.IsNull() || rec.path.empty())
                continue;

            // Only upgrade rows whose stored type is Unknown or unrecognized
            // (parser-/extension-derived classification is more authoritative
            // than a stale legacy value). Recognized types are skipped
            // unconditionally — including XML — so the parser's claim never
            // overrides a deliberate user-set type.
            const bool storedValid = IsRecognizedAssetType(rec.type);
            const bool storedUnknown = (!storedValid) || (rec.type == AssetType::Unknown);
            if (!storedUnknown)
                continue;

            const std::string ext = GetCompoundExtensionFromPath(rec.path);

            // Prefer parser-based classification when possible (enables .xml sniffing for UI layouts).
            AssetType inferred = AssetType::Unknown;
            if (m_ParserRegistry)
            {
                std::filesystem::path assetRoot;
                {
                    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                    assetRoot = ProjectRoot();
                }
                const std::filesystem::path abs = NormalizePathForMap(assetRoot / std::filesystem::path(rec.path));
                if (auto parser = m_ParserRegistry->FindParser(abs))
                {
                    inferred = parser->GetAssetType();
                }
            }
            if (inferred == AssetType::Unknown)
            {
                inferred = m_TypeRegistry.GetAssetTypeFromExtension(ext);
            }
            if (inferred == AssetType::Unknown)
                continue;

            if (rec.type != inferred)
            {
                AssetDatabase::AssetRecord updated = rec;
                updated.type = inferred;
                updated.typeId = AssetTypeToString(inferred);
                (void)newStore->UpsertAsset(updated, nullptr);
                if (cachePtr)
                {
                    (void)cachePtr->UpsertAsset(updated, nullptr);
                }
                ++upgraded;
            }
        }
        if (upgraded > 0)
        {
            Logger::Log::Info("AssetRegistry: upgraded {} asset types while loading '{}'", upgraded, filePath.string());
            typesDirty = true;
        }
    }

    // Prune ignored records (defaults + <AssetRoot>/.assetignore) so a previously polluted DB
    // can't keep reloading junk records after ignore rules change.
    bool ignoredRecordsDirty = false;
    {
        AssetIgnoreRules rules;
        AssetDatabase::IAssetDbCache* cachePtr = nullptr;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            rules = ProjectIgnoreRules();
            cachePtr = ProjectCache();
        }

        size_t removed = 0;
        ignoredRecordsDirty = AssetDatabase::PruneIgnoredStoreRecords(rules, *newStore, cachePtr, removed);
        if (removed > 0)
        {
            Logger::Log::Info("AssetRegistry: pruned {} ignored records while loading '{}'", removed, filePath.string());
        }
    }

    const std::vector<AssetDatabase::AssetRecord> records = newStore->EnumerateAssets();

    {
        std::unique_lock<std::shared_mutex> writeLock(m_RegistryMutex);
        if (!m_Initialized)
        {
            Logger::Log::Warning("AssetRegistry::LoadFromFile called while not initialized");
            return false;
        }

        // Update the project source entry if it exists.
        if (m_ProjectSource)
        {
            m_ProjectSource->Store = std::move(newStore);
            m_ProjectSource->DbFile = filePath;
            m_ProjectSource->StoreDirty.store(legacyPathsDirty || typesDirty || ignoredRecordsDirty, std::memory_order_relaxed);
        }

        // Legacy full-reload replaces the registry state wholesale; pending
        // overlays, claims, remap aliases, the per-source resident indices,
        // and the occupancy counter go with it (rebuilt by the insert loop
        // below).
        for (auto& shard : m_AssetShards)
            shard.clear();
        for (auto& shard : m_PathShards)
            shard.clear();
        m_MountOverlays.clear();
        m_PathClaims.clear();
        m_GuidRemaps.clear();
        for (const auto& src : m_Sources)
            src->ResidentGuids.clear();
        m_AssetCount.store(0, std::memory_order_relaxed);

        for (const auto& rec : records)
        {
            if (rec.guid.IsNull() || rec.path.empty() || rec.missing)
                continue;

            AssetMetadata md{};
            md.Guid = rec.guid;
            md.Path = NormalizePathForMap(ProjectRoot() / std::filesystem::path(rec.path));
            md.Name = std::filesystem::path(rec.path).stem().string();
            md.Extension = GetCompoundExtensionFromPath(md.Path.string());
            md.Type = (IsRecognizedAssetType(rec.type) && rec.type != AssetType::Unknown)
                          ? rec.type
                          : m_TypeRegistry.GetAssetTypeFromExtension(md.Extension);

            std::error_code ec;
            if (std::filesystem::exists(md.Path, ec) && std::filesystem::is_regular_file(md.Path, ec))
            {
                std::error_code ec2;
                md.LastModified = std::filesystem::last_write_time(md.Path, ec2);
                std::error_code ec3;
                md.FileSize = std::filesystem::file_size(md.Path, ec3);
            }

            const std::string mdKey = PathMapKey(md.Path);
            if (AssetShard(md.Guid).insert_or_assign(md.Guid, md).second)
                ApplyAssetCountDelta(1);
            PathShard(mdKey)[mdKey] = md.Guid;
            IndexResidentGuidLocked(md.Guid, md.Path);

            if (ProjectCache())
            {
                AssetDatabase::AssetRecord cacheRec = rec;
                cacheRec.type = md.Type;
                cacheRec.typeId = md.TypeId;
                (void)ProjectCache()->UpsertAsset(cacheRec, nullptr);
                for (const auto& kv : rec.kv)
                {
                    (void)ProjectCache()->SetKeyValue(rec.guid, kv.first, kv.second, nullptr);
                }
            }
        }
    }

    return true;
}

// kv rows are store state, so a mount states whether it takes them with its own
// flag rather than borrowing the structural one: a published package freezes its
// asset set (IsImmutable) and still owns the import settings of the files that
// set contains. A cache entry and shipped content take neither, and a mount with
// no store has nowhere to write.
static bool SourceAcceptsMetaWrites(const AssetRegistry::SourceEntry* source)
{
    return source && source->HasStore() && source->AcceptsMetadataWrites && !source->IsReadOnly;
}

// A refused metadata write is a dead end the caller cannot fix by retrying, so
// say why and how, once per source: the material-bind auto-tagger reaches this
// on every bind of a texture it cannot tag. Call with the registry lock held.
static void LogMetaWriteRefusalLocked(AssetRegistry::SourceEntry* source,
                                      const std::filesystem::path& assetPath)
{
    if (!source || source->MetaWriteRefusalLogged.exchange(true, std::memory_order_relaxed))
        return;
    if (!source->HasStore())
    {
        Logger::Log::Warning(
            "AssetRegistry: source '{}' publishes no .assetmanifest, so it has no metadata store; "
            "import settings for '{}' cannot be saved. Publish the package's manifest to author them.",
            source->Alias, assetPath.string());
        return;
    }
    Logger::Log::Warning(
        "AssetRegistry: source '{}' is mounted read-only (a package cache entry or shipped content); "
        "import settings for '{}' cannot be saved here. Edit them in the package's own repository "
        "and republish it.",
        source->Alias, assetPath.string());
}

AssetImportSettingsOrigin AssetRegistry::GetImportSettingsOrigin(
    const std::filesystem::path& assetPath) const
{
    AssetImportSettingsOrigin origin;
    if (assetPath.empty())
        return origin;

    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        if (!ProjectStore())
            return origin;
        assetRoot = ProjectRoot();
    }

    const std::filesystem::path normPath = NormalizePathForMap(
        assetPath.is_relative() ? (assetRoot / assetPath) : assetPath);

    std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);

    // A path under the project root is the project's whatever else overlaps it,
    // and the project store is writable.
    std::string canonicalRel;
    if (AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normPath, canonicalRel))
    {
        origin.SourceAlias = m_ProjectSource ? m_ProjectSource->Alias
                                             : std::string(kAssetSourceAliasProject);
        origin.Writable = true;
        return origin;
    }

    const SourceEntry* found = FindSourceForPath(normPath);
    if (!found)
        return origin;

    origin.SourceAlias = found->Alias;
    origin.Writable = found != m_ProjectSource.get() && SourceAcceptsMetaWrites(found);
    return origin;
}

bool AssetRegistry::TryGetMetaValue(const std::filesystem::path& assetPath,
                                    const std::string& key,
                                    std::string& outValue) const
{
    outValue.clear();
    if (assetPath.empty() || key.empty())
    {
        return false;
    }

    AssetDatabase::IAssetStore* store = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        assetRoot = ProjectRoot();
    }

    if (!store)
    {
        return false;
    }

    const std::filesystem::path normPath = NormalizePathForMap(
        assetPath.is_relative() ? (assetRoot / assetPath) : assetPath);

    std::string canonicalRel;
    if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normPath, canonicalRel))
    {
        // Non-project assets may have kv persistence in the non-persistent store (Editor/local).
        const SourceEntry* npSrc = nullptr;
        AssetDatabase::IAssetStore* npStore = nullptr;
        std::filesystem::path npRoot;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            npSrc = FindSourceForPath(normPath);
            if (npSrc && npSrc != m_ProjectSource.get() && npSrc->HasStore())
            {
                npStore = npSrc->Store.get();
                npRoot = npSrc->Root;
            }
            else
            {
                npSrc = nullptr;
            }
        }
        if (!npStore)
            return false;
        std::string npCanonicalRel;
        if (npRoot.empty() || !AssetPaths::TryMakeCanonicalRelativePath(npRoot, normPath, npCanonicalRel) || npCanonicalRel.empty())
            return false;

        const std::optional<GUID> guid = npStore->LookupGuidByPath(npCanonicalRel);
        if (!guid)
            return false;
        return npStore->TryGetKeyValue(*guid, key, outValue);
    }

    const std::optional<GUID> guid = store->LookupGuidByPath(canonicalRel);
    if (!guid)
    {
        return false;
    }

    return store->TryGetKeyValue(*guid, key, outValue);
}

bool AssetRegistry::SetMetaValue(const std::filesystem::path& assetPath,
                                 const std::string& key,
                                 const std::string& value)
{
    if (assetPath.empty() || key.empty())
    {
        return false;
    }

    // Ensure the asset has an identity in the store.
    if (!RegisterAsset(assetPath))
    {
        return false;
    }

    const GUID guid = GetAssetGUID(assetPath);
    if (guid.IsNull())
    {
        return false;
    }

    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        cache = ProjectCache();
        assetRoot = ProjectRoot();
    }

    if (!store)
    {
        return false;
    }

    // Route kv persistence to the project store for project assets, otherwise to the non-persistent store when configured.
    // The absolute spelling is kept beside the map key: the key is normalized
    // for lookup (case-folded where the filesystem is), and a refusal quotes the
    // asset's path as the caller spelled it, not a lookup key.
    const std::filesystem::path absolutePath =
        (assetPath.is_relative() ? (assetRoot / assetPath) : assetPath).lexically_normal();
    const std::filesystem::path normPath = NormalizePathForMap(absolutePath);
    std::string canonicalRel;
    if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normPath, canonicalRel))
    {
        // Pinned: the entry is used after the lock releases, so hold its
        // SharedPtr co-owner — a concurrent unmount must not destroy it.
        SharedPtr<SourceEntry> npSrc;
        AssetDatabase::IAssetStore* npStore = nullptr;
        AssetDatabase::IAssetDbCache* npCache = nullptr;
        std::filesystem::path npRoot;
        {
            std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
            SourceEntry* found = FindSourceForPath(normPath);
            if (found && found != m_ProjectSource.get() && SourceAcceptsMetaWrites(found))
            {
                npSrc = PinSourceLocked(found);
            }
            if (!npSrc)
            {
                LogMetaWriteRefusalLocked(found, absolutePath);
                return false;
            }
            npStore = npSrc->Store.get();
            npCache = npSrc->Cache.get();
            npRoot = npSrc->Root;
        }
        if (!npStore)
            return false;
        std::string npCanonicalRel;
        if (npRoot.empty() || !AssetPaths::TryMakeCanonicalRelativePath(npRoot, normPath, npCanonicalRel) || npCanonicalRel.empty())
            return false;

        // Ensure the record exists in the non-persistent store before setting kv.
        AssetMetadata md{};
        if (!TryGetAssetMetadata(guid, md))
            return false;

        AssetDatabase::AssetRecord rec{};
        if (!npStore->TryGetAsset(guid, rec))
        {
            rec.guid = guid;
        }
        rec.path = npCanonicalRel;
        rec.type = md.Type;
        rec.typeId = md.TypeId;
        rec.missing = false;
        (void)npStore->UpsertAsset(rec, nullptr);

        if (!npStore->SetKeyValue(guid, key, value, nullptr))
            return false;
        npSrc->StoreDirty.store(true, std::memory_order_relaxed);
        if (npCache)
        {
            (void)npCache->SetKeyValue(guid, key, value, nullptr);
        }
        return true;
    }

    if (!store->SetKeyValue(guid, key, value, nullptr))
    {
        return false;
    }
    if (auto pinnedDirty = ProjectSourcePinned())
        pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);

    if (cache)
    {
        (void)cache->SetKeyValue(guid, key, value, nullptr);
    }

    return true;
}

bool AssetRegistry::RegisterSubassetDeriveKeys(const std::filesystem::path& assetPath,
                                               const Vector<String>& keys)
{
    if (assetPath.empty())
        return false;
    for (const String& key : keys)
    {
        if (key.empty() || key.find('\n') != String::npos)
        {
            Logger::Log::Warning(
                "AssetRegistry::RegisterSubassetDeriveKeys: rejecting malformed derive key for '{}'",
                assetPath.string());
            return false;
        }
    }

    const std::filesystem::path assetRoot = GetAssetRoot();
    const std::filesystem::path normPath = NormalizePathForMap(
        assetPath.is_relative() ? (assetRoot / assetPath) : assetPath);

    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    // Block A.2 pin: the entry is used after the lock releases, so hold its
    // SharedPtr co-owner — a concurrent unmount must not destroy it under us.
    SharedPtr<SourceEntry> src;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        SourceEntry* found = FindSourceForPath(normPath);
        // A shipped package mount must never be journaled into; loaders run in
        // the Player too, so the writability gate lives here, not per caller.
        if (!found || !found->HasStore() || found->IsImmutable || found->IsReadOnly)
            return false;
        src = PinSourceLocked(found);
        if (!src)
            return false;
        store = src->Store.get();
        cache = src->Cache.get();
    }

    // Loader context: the container is already registered. A GUID-less path
    // (memory-only load, foreign mount) journals nothing.
    const GUID guid = GetAssetGUID(normPath);
    if (guid.IsNull())
        return false;

    const String joined = JoinSubassetDeriveKeys(keys);
    String previous;
    const bool hadRow = store->TryGetKeyValue(guid, kSubassetDeriveKeysKvKey, previous);
    if (!hadRow && keys.empty())
        return true; // no subassets, no row: nothing to journal or clear.

    if (!hadRow || previous != joined)
    {
        if (!store->SetKeyValue(guid, kSubassetDeriveKeysKvKey, joined, nullptr))
            return false;
        if (cache)
            (void)cache->SetKeyValue(guid, kSubassetDeriveKeysKvKey, joined, nullptr);
        src->StoreDirty.store(true, std::memory_order_relaxed);

        std::lock_guard indexLock(m_SubassetContainerMutex);
        if (m_SubassetContainersBuilt)
        {
            if (hadRow)
            {
                for (const String& key : SplitSubassetDeriveKeys(previous))
                    m_SubassetContainers.erase(GUID::Derive(guid, key));
            }
            for (const String& key : keys)
                m_SubassetContainers[GUID::Derive(guid, key)] = guid;
        }
    }

    // Subasset analog of the stale-redirect-on-registration rule: when this
    // container GUID speaks for itself (no outgoing container redirect), a
    // redirect sourced at one of ITS derived subasset identities is a stale
    // shadow from an earlier heal of this GUID — remove it. A live-renamed
    // container keeps its outgoing redirect, so its cascades stay load-bearing.
    if (!store->ResolveRedirect(guid).has_value())
    {
        std::vector<GUID> victims;
        victims.reserve(keys.size());
        for (const String& key : keys)
            victims.push_back(GUID::Derive(guid, key));
        // Keys that vanished since the last registration can still source
        // stale cascades; sweep the previous list too.
        if (hadRow && previous != joined)
        {
            for (const String& key : SplitSubassetDeriveKeys(previous))
                victims.push_back(GUID::Derive(guid, key));
        }

        // Cascade redirects are ordinary redirects: hops that reached a
        // subasset identity inherit its chain-final target so the rename
        // direction outlives the removal, exactly as at container level.
        const size_t retargeted = AssetDatabase::RetargetIncomingRedirects(*store, victims);
        bool removedAny = false;
        for (const GUID& victim : victims)
        {
            if (store->RemoveRedirect(victim, nullptr))
                removedAny = true;
        }
        if (removedAny || retargeted > 0)
        {
            src->StoreDirty.store(true, std::memory_order_relaxed);
            {
                std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
                if (src.get() == m_ProjectSource.get())
                    RefreshProjectRedirectFlagLocked();
            }
            Logger::Log::Info(
                "AssetRegistry: cleared stale subasset redirects for reborn container {} ('{}'); "
                "{} incoming hop(s) retargeted",
                guid.ToString(), normPath.string(), retargeted);
        }
    }

    return true;
}

GUID AssetRegistry::FindSubassetContainer(const GUID& subasset) const
{
    if (subasset.IsNull())
        return GUID::Null();
    std::lock_guard indexLock(m_SubassetContainerMutex);
    if (!m_SubassetContainersBuilt)
    {
        // Containers journal only into the project store (RegisterSubassetDeriveKeys refuses
        // immutable and read-only mounts), so that is the one journal to read.
        if (SharedPtr<SourceEntry> project = ProjectSourcePinned(); project && project->HasStore())
        {
            project->Store->IterateAssets([this](const AssetDatabase::AssetRecord& rec)
            {
                if (auto it = rec.kv.find(kSubassetDeriveKeysKvKey); it != rec.kv.end())
                {
                    for (const String& key : SplitSubassetDeriveKeys(it->second))
                        m_SubassetContainers[GUID::Derive(rec.guid, key)] = rec.guid;
                }
            });
        }
        m_SubassetContainersBuilt = true;
    }
    const auto it = m_SubassetContainers.find(subasset);
    return it != m_SubassetContainers.end() ? it->second : GUID::Null();
}

void AssetRegistry::ForgetSubassetContainers()
{
    std::lock_guard indexLock(m_SubassetContainerMutex);
    m_SubassetContainers.clear();
    m_SubassetContainersBuilt = false;
}

bool AssetRegistry::SaveAssetMetadata(const AssetMetadata& metadata)
{
    if (metadata.Guid.IsNull())
    {
        return false;
    }

    AssetDatabase::IAssetStore* store = nullptr;
    AssetDatabase::IAssetDbCache* cache = nullptr;
    std::filesystem::path assetRoot;
    {
        std::shared_lock<std::shared_mutex> readLock(m_RegistryMutex);
        store = ProjectStore();
        cache = ProjectCache();
        assetRoot = ProjectRoot();
    }

    if (!store)
    {
        // Registry can still function in-memory, but cannot persist without a store.
        return false;
    }

    const std::filesystem::path normPath = NormalizePathForMap(
        metadata.Path.is_relative() ? (assetRoot / metadata.Path) : metadata.Path);

    std::string canonicalRel;
    if (!AssetPaths::TryMakeCanonicalRelativePath(assetRoot, normPath, canonicalRel))
    {
        // Non-project assets are not persisted via this path.
        return false;
    }

    // Durable identity-ish fields only; kv (user-authored metadata) is the
    // store's to keep, as is a classification this caller does not carry.
    AssetDatabase::AssetObservation obs{};
    obs.guid = metadata.Guid;
    obs.path = canonicalRel;
    obs.type = metadata.Type;
    obs.typeId = metadata.TypeId;

    AssetDatabase::AssetRecord rec{};
    if (store->MergeObservation(obs, rec, nullptr) == AssetDatabase::StoreMergeResult::Failed)
    {
        return false;
    }
    if (auto pinnedDirty = ProjectSourcePinned())
        pinnedDirty->StoreDirty.store(true, std::memory_order_relaxed);

    if (cache)
    {
        (void)cache->UpsertAsset(rec, nullptr);
    }

    return true;
}

} // namespace GameEngine
