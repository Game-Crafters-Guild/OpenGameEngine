#include "Assets/AssetManager.h"
#include "AssetPathSyntax.h"

#include "Assets/AssetDbProfiler.h"
#include "Assets/AssetDecodeGate.h"
#include "Assets/AssetIOService.h"
#include "Assets/AssetTasks.h"
#include "Assets/ShaderProgramAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCookWorkers.h"
#include "Assets/CoreAssetRegistrations.h"
#include "AssetCore/PathNormalization.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace GameEngine
{

namespace
{
using AssetPathSyntax::NormalizeAssetSourceAlias;
using AssetPathSyntax::IsValidAssetSourceAlias;
using AssetPathSyntax::NormalizeRelativeAssetPath;
using AssetPathSyntax::TrySplitAssetSourcePrefix;

constexpr auto kTimestampReloadCheckInterval = std::chrono::milliseconds(100);
constexpr size_t kMaxTimestampReloadChecksPerUpdate = 8;

// How long an eject may wait on loads that never complete before it says so.
// Long enough that a genuinely slow decode (a large texture cook) is not
// reported, short enough that a user who opened a project and is looking at a
// picker that has not gone away gets an answer in the log.
constexpr auto kEjectStallWarningDelay = std::chrono::seconds(5);
constexpr size_t kMaxNamedStalledLoads = 8;

void WarnLoadSuppressed(const GUID& guid, const AssetMetadata& metadata, const String& reason)
{
    Logger::Log::Warning(
        "AssetManager: suppressing repeated loads for asset {} ('{}'): {}",
        guid.ToString(),
        metadata.Path.string(),
        reason);
}

template <typename T>
inline std::pair<std::shared_ptr<std::promise<T>>, std::shared_future<T>> MakeSharedPromise()
{
    auto p = std::make_shared<std::promise<T>>();
    auto sf = p->get_future().share();
    return {std::move(p), sf};
}

static std::string NormalizePathForCompare(const std::filesystem::path& p)
{
    if (p.empty())
        return {};
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec).lexically_normal();
    std::string s = abs.generic_string();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
    return s;
}

static bool PathIsWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path)
{
    if (root.empty() || path.empty())
        return false;
    std::string normRoot = NormalizePathForCompare(root);
    std::string normPath = NormalizePathForCompare(path);
    if (normRoot.empty() || normPath.empty())
        return false;
    if (normPath == normRoot)
        return true;
    if (!normRoot.empty() && normRoot.back() != '/')
        normRoot.push_back('/');
    return normPath.rfind(normRoot, 0) == 0;
}

} // namespace

thread_local AssetManager* AssetManager::t_threadCurrent = nullptr;

// Out-of-line so all t_threadCurrent (thread_local) access stays inside Engine.dll;
// the SHARED library does not export TLS data symbols, so inlining these into
// external TUs would leave the symbol unresolved at link.
AssetManager::ScopedThreadAssetManager::ScopedThreadAssetManager(AssetManager* current)
    : m_Prev(t_threadCurrent)
{
    t_threadCurrent = current;
}

AssetManager::ScopedThreadAssetManager::~ScopedThreadAssetManager()
{
    t_threadCurrent = m_Prev;
}

AssetManager* AssetManager::GetThreadCurrent()
{
    return t_threadCurrent;
}

bool AssetManager::IsAssetSupportedForLoad(const AssetMetadata& metadata, String* outReason) const
{
    // Determine support based on either:
    // - an AssetTypeRegistry factory for the resolved type, OR
    // - a ParserRegistry parser for the file extension.
    const auto& typeRegistry = m_Registry.GetTypeRegistry();

    AssetType resolvedType = metadata.Type;
    if (resolvedType == AssetType::Unknown)
    {
        resolvedType = typeRegistry.GetAssetTypeFromExtension(metadata.Extension);
    }

    const bool hasFactory =
        (resolvedType != AssetType::Unknown) && typeRegistry.IsAssetTypeRegistered(resolvedType);

    const bool hasParser = m_ParserRegistry.HasParserForExtension(metadata.Extension);

    if (outReason)
    {
        if (hasFactory || hasParser)
        {
            *outReason = "";
        }
        else
        {
            *outReason =
                "Unsupported asset extension '" + metadata.Extension +
                "' (no registered type factory and no parser)";
        }
    }

    return hasFactory || hasParser;
}

bool AssetManager::IsLoadSuppressed(const GUID& guid, String* outReason) const
{
    if (guid.IsNull())
        return false;

    std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
    auto it = m_SuppressedLoads.find(guid);
    if (it == m_SuppressedLoads.end())
        return false;
    if (outReason)
        *outReason = it->second.reason;
    return true;
}

void AssetManager::SuppressLoadRetries(const GUID& guid, const AssetMetadata& metadata, const String& reason)
{
    MarkLoadSuppressed(guid, metadata, reason);
}

void AssetManager::MarkLoadSuppressed(const GUID& guid, const AssetMetadata& metadata, const String& reason)
{
    if (guid.IsNull())
        return;

    bool first = false;
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        auto it = m_SuppressedLoads.find(guid);
        if (it == m_SuppressedLoads.end())
        {
            SuppressedLoadInfo info{};
            info.extension = metadata.Extension;
            info.reason = reason;
            info.firstSeen = std::chrono::steady_clock::now();
            m_SuppressedLoads.emplace(guid, std::move(info));
            first = true;
        }
        else
        {
            // Keep the firstSeen timestamp stable; update details for diagnostics.
            it->second.extension = metadata.Extension;
            it->second.reason = reason;
        }
    }

    if (first)
        WarnLoadSuppressed(guid, metadata, reason);
}

void AssetManager::MarkLoadSuppressed(const GUID& guid, const String& reason)
{
    AssetMetadata metadata{};
    m_Registry.TryGetAssetMetadata(guid, metadata);
    MarkLoadSuppressed(guid, metadata, reason);
}

void AssetManager::MarkLoadFailed(const GUID& guid)
{
    static const String kReason = "Load failed";
    if (guid.IsNull())
        return;

    AssetMetadata metadata{};
    m_Registry.TryGetAssetMetadata(guid, metadata);
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        SuppressedLoadInfo info{};
        info.extension = metadata.Extension;
        info.reason = kReason;
        info.firstSeen = std::chrono::steady_clock::now();
        // The decode job records why a load failed before the load completes;
        // that reason stays.
        if (!m_SuppressedLoads.emplace(guid, std::move(info)).second)
            return;
    }
    WarnLoadSuppressed(guid, metadata, kReason);
}

void AssetManager::SuppressLoadsWhileEjecting(const std::vector<GUID>& guids)
{
    static const String kReason = "source eject in progress";
    size_t refused = 0;
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        for (const auto& guid : guids)
        {
            if (guid.IsNull())
                continue;
            SuppressedLoadInfo info{};
            info.reason = kReason;
            info.firstSeen = std::chrono::steady_clock::now();
            // An asset already suppressed for a real reason keeps that reason:
            // the eject clears the whole set at step 5 either way.
            if (m_SuppressedLoads.emplace(guid, std::move(info)).second)
                ++refused;
        }
    }
    if (refused > 0)
        Logger::Log::Debug("AssetManager: refusing loads for {} asset(s) while their source ejects", refused);
}

void AssetManager::ClearLoadSuppressed(const GUID& guid)
{
    if (guid.IsNull())
        return;
    std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
    m_SuppressedLoads.erase(guid);
}

// How long after a write is reported the file watcher's report of the same write can
// still arrive. It has to outlast the watcher's own latency: the polling backend looks
// once a second, holds a Deleted for 200 ms to see whether a replace-rename brings the
// path back, and defers a Modified for the 100 ms debounce — all of which stretch under
// load. Erring long costs nothing, because what protects an edit made inside the window
// is the reported file's identity rather than the clock; erring short brings back a
// second drive for one save.
static constexpr std::chrono::milliseconds kWriteEchoWindow{5000};

AssetManager::AssetManager()
    : m_Initialized(false), m_HotReloadEnabled(false), m_JobSystem(nullptr),
      m_ExpectedWrites(kWriteEchoWindow)
{
}

AssetManager::~AssetManager()
{
    if (m_Initialized)
    {
        Shutdown();
    }
}

// ------------------------------------------------------------------
// Infrastructure-only Initialize
// ------------------------------------------------------------------

bool AssetManager::Initialize(JobSystem::WorkStealingThreadPool* jobSystem)
{
    if (m_Initialized)
    {
        Logger::Log::Warning("AssetManager already initialized");
        return true;
    }

    Logger::Log::Info("Initializing Asset Manager (infrastructure) with {} job system",
                      jobSystem ? "async" : "sync");

    m_AssetSources.clear();
    m_SuppressedLoads.clear();
    m_JobSystem = jobSystem;

    // Initialize parser registry
    if (!m_ParserRegistry.Initialize())
    {
        Logger::Log::Error("Failed to initialize parser registry");
        return false;
    }

    // Set parser registry reference in asset registry
    m_Registry.SetParserRegistry(&m_ParserRegistry);

    // Initialize asset registry infrastructure (type registry, async coordinator).
    if (!m_Registry.Initialize(m_JobSystem))
    {
        Logger::Log::Error("Failed to initialize asset registry");
        return false;
    }

    // Ensure core engine asset types are registered.
    RegisterCoreAssetTypes(GetAssetTypeRegistry());

    // E5: consume registry GUID remaps (overlapping-source re-claims)
    // atomically, so loads started under the old GUID stay reachable under
    // the new one.
    m_Registry.SetGuidRemapCallback(
        [this](const GUID& oldGuid, const GUID& newGuid, const std::filesystem::path& path)
        { OnGuidRemapped(oldGuid, newGuid, path); });

    // Initialize the blocking-read front-end and the unified loading coordinator
    if (m_JobSystem)
    {
        m_IOService = std::make_unique<AssetIOService>();
        m_IOService->Start(*m_JobSystem);
        // Inline mode has no gate and no workers to spread to: cooks encode on their own thread.
        if (std::shared_ptr<AssetDecodeGate> gate = m_IOService->GetDecodeGate())
            m_TextureCookWorkers = std::make_unique<TextureCookWorkers>(*m_JobSystem, std::move(gate));

        m_BatchCoordinator = std::make_unique<AssetBatchCoordinator>(*this, *m_JobSystem, *m_IOService);
        Logger::Log::Info("Unified asset loading coordinator initialized");
    }

    m_Initialized = true;
    Logger::Log::Info("Asset Manager infrastructure initialized");
    return true;
}

// ------------------------------------------------------------------
// Convenience Initialize overload
// ------------------------------------------------------------------

namespace
{
bool IsPackagedPlayerEnvironment()
{
    const char* env = std::getenv("GE_PACKAGED_PLAYER");
    return env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
}

// One place decides what an inline reload outcome means for the log and for
// the event stream. Deferred is a save in progress rather than a failure, so
// it stays at Debug and raises nothing: an AssetLoadFailed on every ordinary
// non-atomic save would teach consumers to ignore the event.
void ReportInlineReloadOutcome(ReloadOutcome outcome, const Asset& asset, const GUID& guid,
                               double reloadMs, std::vector<AssetEvent>& pendingEvents)
{
    const String path = asset.GetPath().string();
    switch (outcome)
    {
    case ReloadOutcome::Reloaded:
        Logger::Log::Info("[AssetReload] '{}' reloaded in {:.2f} ms (inline on main thread)", path,
                          reloadMs);
        pendingEvents.push_back(AssetEvents::AssetReloaded(guid, asset.GetType(), path));
        break;
    case ReloadOutcome::Deferred:
        Logger::Log::Debug("[AssetReload] '{}' deferred in {:.2f} ms: empty read, save in progress",
                           path, reloadMs);
        break;
    case ReloadOutcome::Failed:
        Logger::Log::Info("[AssetReload] '{}' reload FAILED in {:.2f} ms (inline on main thread)",
                          path, reloadMs);
        pendingEvents.push_back(
            AssetEvents::AssetLoadFailed(guid, asset.GetType(), path, "Reload failed"));
        break;
    }
}
} // namespace

bool AssetManager::Initialize(const std::filesystem::path& assetRoot,
                              JobSystem::WorkStealingThreadPool* jobSystem,
                              const std::filesystem::path& authoritativeDbFile,
                              const std::filesystem::path& cacheRoot)
{
    if (!Initialize(jobSystem))
        return false;

    const std::filesystem::path normalizedRoot = AssetPaths::NormalizeMountRoot(assetRoot);

    // A packaged game ships a flat, read-only `.assetmanifest` (the authoritative
    // GUID<->path identity, JSONL) at the asset root instead of the editor's
    // writable AssetDatabase.assetdb. Mount it as an immutable, manifest-driven
    // package source: no filesystem scan, no DB write-back, no SQLite cache —
    // identity is resolved entirely from the manifest. This is the standalone
    // Player's path.
    std::error_code manifestEc;
    const std::filesystem::path manifestFile = normalizedRoot / ".assetmanifest";
    if (std::filesystem::exists(manifestFile, manifestEc))
    {
        AssetSourceDesc packageDesc = MakePackageMount("project", normalizedRoot, 100);
        if (!RegisterSource(packageDesc))
        {
            Logger::Log::Error("AssetManager: failed to register packaged project source at '{}'", assetRoot.string());
            return false;
        }
        Logger::Log::Info("AssetManager: mounted packaged content (read-only manifest) at '{}'", normalizedRoot.string());
        m_ContentIsPackaged = true;
        return true;
    }

    // Editor / dev: a scanning project source whose identity is DERIVED from the
    // canonical asset path. A file's GUID is Derive(namespace, normalize("project/"
    // + relPath)) — reproducible on every machine with no shipped GUID->path map.
    // The authoritative DB, when present, is kept purely as a derived cache:
    // PopulateHotCachesFromSource
    // re-derives each record from its path on load, so a project still holding
    // pre-flip random GUIDs resolves identically to one rebuilt from scratch.
    AssetSourceDesc projectDesc{};
    projectDesc.Alias = "project";
    projectDesc.Root = normalizedRoot;
    projectDesc.DerivedIdentity = true;
    projectDesc.AuthoritativeDbFile = authoritativeDbFile;
    projectDesc.CacheRoot = cacheRoot;
    projectDesc.Priority = 100;
    if (IsPackagedPlayerEnvironment())
        projectDesc.RegisterFileWatcher = false;

    if (!RegisterSource(projectDesc))
    {
        Logger::Log::Error("AssetManager: failed to register project source at '{}'", assetRoot.string());
        return false;
    }

    return true;
}

// ------------------------------------------------------------------
// GetAssetRoot
// ------------------------------------------------------------------

std::filesystem::path AssetManager::GetAssetRoot() const
{
    return m_Registry.GetAssetRoot();
}

void AssetManager::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    Logger::Log::Info("Shutting down Asset Manager");

    // Unsubscribe all source watchers (does not stop the shared service).
    for (auto& source : m_AssetSources)
    {
        source.WatchSubscription.reset();
    }
    m_AssetSources.clear();

    // Unload all assets. Locked: decode jobs still run until the drain below
    // and register/suppress concurrently under these same mutexes.
    {
        std::unique_lock<std::shared_mutex> lk(m_LoadedAssetsMutex);
        m_LoadedAssets.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        m_SuppressedLoads.clear();
    }
    InvalidateResolveMemo();
    m_Registry.SetGuidRemapCallback(nullptr);

    // Shutdown registry
    m_Registry.Shutdown();

    // Withdraw async reload pipelines before stopping the IO service: flags
    // first (claimed reads and queued/running decodes defuse themselves),
    // then queued reads. CancelRequest may fire a request's OnFailure inline
    // — no locks held here.
    {
        std::vector<std::pair<GUID, std::shared_ptr<std::atomic<bool>>>> activeReloads;
        {
            std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
            activeReloads.reserve(m_ActiveReloads.size());
            for (auto& [guid, entry] : m_ActiveReloads)
            {
                if (entry.CancelRequested)
                    entry.CancelRequested->store(true, std::memory_order_release);
                activeReloads.emplace_back(guid, entry.CancelRequested);
            }
        }
        for (auto& [guid, flag] : activeReloads)
        {
            if (m_IOService)
                m_IOService->CancelRequest(guid, flag);
        }
    }

    // Stop the blocking-read front-end BEFORE the engine's JobSystem pool
    // shuts down: reads still queued are abandoned with their promises
    // resolved, reads already claimed run to completion, and the reader
    // threads are joined here. Loads are handed to the coordinator inline on
    // the submitting caller's thread (no internal producer to stop first), so
    // this Stop() is the submission gate: a LoadAsset racing it fails its
    // request inline on the stopped service and resolves its promise on the
    // calling thread, never producing a decode job. The object itself stays
    // alive (stopped) so any straggling submission fails its request inline
    // instead of dereferencing a dead service.
    if (m_IOService)
    {
        m_IOService->Stop();
    }

    // Drain the decode jobs. They and their completion callbacks capture this
    // AssetManager raw, and the engine destroys the manager BEFORE the
    // JobSystem pool (Engine.cpp teardown order) — so nothing referencing the
    // manager may remain in the pool when Shutdown returns. The readers are
    // joined and only they submit decode jobs, so no new decode can appear:
    // cancel every in-flight decode (queued ones die on the spot and resolve
    // their waiters inline), then wait out the survivors — bounded by the few
    // decodes actually running. Completion callbacks resolve the shared
    // promise as their final touch of manager state, so a resolved future
    // means that load is done with `this`.
    {
        struct PendingLoad
        {
            JobSystem::TaskHandle DecodeHandle;
            std::shared_future<SharedPtr<Asset>> Future;
        };
        std::vector<PendingLoad> pending;
        {
            std::lock_guard<std::mutex> lock(m_InFlightMutex);
            pending.reserve(m_InFlight.size());
            for (auto& [guid, entry] : m_InFlight)
            {
                if (entry.CancelRequested)
                    entry.CancelRequested->store(true, std::memory_order_release);
                pending.push_back({entry.DecodeHandle, entry.Future});
            }
        }
        for (auto& load : pending)
        {
            if (load.DecodeHandle.IsValid())
                load.DecodeHandle.Cancel(); // won cancels fire the failure callbacks inline
        }
        for (const auto& load : pending)
        {
            if (load.Future.valid())
            {
                try { load.Future.wait(); } catch (...) {}
            }
        }
        if (!pending.empty())
        {
            Logger::Log::Info("AssetManager: drained {} in-flight load(s) during shutdown", pending.size());
        }
    }

    // Drop the pending ejects without finishing them and without running their
    // continuations. Everything an eject would still do is already done above —
    // the sources are gone, the loaded assets are released, the registry is
    // shut down — and a continuation here would run editor code (remounting
    // packages, touching a window that no longer exists) into a torn-down
    // application: the editor destroys its UI before the engine shuts down.
    m_PendingEjects.clear();

    // Wait out reload pipelines still in flight: their read/decode callbacks
    // capture this manager raw, and the engine destroys the manager before
    // the JobSystem pool. Bounded: the readers are already joined (each
    // queued reload read resolved through OnFailure), and every submitted
    // reload decode job's first act under its set cancel flag is to bail out.
    {
        std::unique_lock<std::mutex> lk(m_ReloadDrainMutex);
        m_ReloadDrainCv.wait(lk, [this]
                             { return m_ActiveReloadJobCount.load(std::memory_order_acquire) == 0; });
    }
    {
        std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
        m_ActiveReloads.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_CompletedReloadMutex);
        m_CompletedReloads.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_InFlightMutex);
        m_ReloadsRequestedDuringLoad.clear();
    }

    // Shutdown parser registry
    m_ParserRegistry.Shutdown();

    m_Initialized = false;
    AssetDbProfiler::EmitSummary("shutdown");
    Logger::Log::Info("Asset Manager shutdown complete");
}

void AssetManager::Update()
{
    if (!m_Initialized)
    {
        return;
    }

    GE_CPU_PROFILE_SCOPE("AssetManager.Update");
    AssetDbProfiler::LatchMainThread();

    {
        GE_CPU_PROFILE_SCOPE("AssetManager.Update.ProcessCompletedTasks");
        ProcessCompletedTasks();
    }

    {
        GE_CPU_PROFILE_SCOPE("AssetManager.Update.StartReloadsRequestedDuringLoad");
        StartReloadsRequestedDuringLoad();
    }

    {
        GE_CPU_PROFILE_SCOPE("AssetManager.Update.DrainPendingEjects");
        DrainPendingEjects();
    }

    {
        GE_CPU_PROFILE_SCOPE("AssetManager.Update.TickPersistence");
        m_Registry.TickPersistence();
    }

    if (m_HotReloadEnabled)
    {
        GE_CPU_PROFILE_SCOPE("AssetManager.Update.CheckForReloads");
        CheckForReloads();
    }

    AssetDbProfiler::TickMainThread();
}

bool AssetManager::RegisterSource(const AssetSourceDesc& source)
{
    if (!m_Initialized)
    {
        Logger::Log::Warning("AssetManager: cannot register source '{}' before initialization", source.Alias);
        return false;
    }

    if (source.Alias.empty())
    {
        Logger::Log::Warning("AssetManager: cannot register source with empty alias");
        return false;
    }

    AssetSourceDesc normalized = source;
    normalized.Alias = NormalizeAssetSourceAlias(source.Alias);
    if (!IsValidAssetSourceAlias(normalized.Alias))
    {
        Logger::Log::Warning("AssetManager: source alias '{}' is invalid (allowed: [a-zA-Z0-9_-])", source.Alias);
        return false;
    }
    // Note: "project" is now a valid alias (no longer reserved).
    normalized.Root = AssetPaths::NormalizeMountRoot(source.Root);
    if (normalized.Root.empty())
    {
        Logger::Log::Warning("AssetManager: source '{}' has empty root", normalized.Alias);
        return false;
    }

    // Check for duplicate alias in our local list.
    for (const auto& entry : m_AssetSources)
    {
        if (entry.Desc.Alias == normalized.Alias)
        {
            Logger::Log::Warning("AssetManager: source alias '{}' is already registered", normalized.Alias);
            return false;
        }
    }

    // Delegate to registry (which handles DB loading, reconciliation, scanning, etc.).
    if (!m_Registry.RegisterSource(normalized))
    {
        Logger::Log::Warning("AssetManager: failed to register source '{}' in AssetRegistry", normalized.Alias);
        return false;
    }

    // Subscribe to file watching for this source.
    // F.1: RegisterFileWatcher=false (Package mounts, read-only/remote
    // sources, transient/scratch mounts) skips the watcher subscription —
    // their content is either manifest-driven, network-mounted (where
    // watching produces spurious events), or short-lived.
    RegisteredAssetSource state{};
    state.Desc = normalized;
    if (normalized.RegisterFileWatcher)
    {
        FilePattern pattern(normalized.Root, ".*", {}, true);
        auto& service = FileWatchingService::GetInstance();
        state.WatchSubscription.emplace(service.Subscribe(pattern, [this, alias = normalized.Alias](const FileChangeEvent& event)
                                                          { OnWatchedFileChanged(event, alias); }));
    }
    else
    {
        Logger::Log::Trace("AssetManager: source '{}' has RegisterFileWatcher=false; skipping subscription",
                           normalized.Alias);
    }

    // Insert sorted by priority descending.
    auto insertPos = std::find_if(m_AssetSources.begin(), m_AssetSources.end(),
                                  [&](const RegisteredAssetSource& e) { return e.Desc.Priority < normalized.Priority; });
    m_AssetSources.insert(insertPos, std::move(state));
    m_SourceSetVersion.fetch_add(1, std::memory_order_release);
    InvalidateResolveMemo();
    Logger::Log::Info("AssetManager: mounted source '{}' at '{}'", normalized.Alias, normalized.Root.string());
    return true;
}

void AssetManager::WaitForStartupScan(std::string_view sourceAlias)
{
    m_Registry.WaitForStartupScan(sourceAlias);
}

bool AssetManager::BeginUnregisterSource(std::string_view sourceAlias, Function<void(bool)> onUnregistered)
{
    if (!m_Initialized)
        return false;
    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);
    if (normalizedAlias.empty())
        return false;

    const auto it = std::find_if(m_AssetSources.begin(), m_AssetSources.end(),
                                 [&normalizedAlias](const RegisteredAssetSource& entry)
                                 { return entry.Desc.Alias == normalizedAlias; });
    if (it == m_AssetSources.end())
        return false;

    if (IsEjectPendingForSource(normalizedAlias))
    {
        Logger::Log::Error("AssetManager::BeginUnregisterSource: an eject of source '{}' is still pending;"
                           " wait for the first one to complete",
                           normalizedAlias);
        return false;
    }

    // The source stays mounted and resolvable while its assets drain; removing
    // it here would let a load started in between resolve against a root the
    // eject is in the middle of abandoning.
    BeginEjectAssetsForSource(
        normalizedAlias,
        [this, normalizedAlias, onUnregistered = std::move(onUnregistered)]()
        {
            const auto entry = std::find_if(m_AssetSources.begin(), m_AssetSources.end(),
                                            [&normalizedAlias](const RegisteredAssetSource& source)
                                            { return source.Desc.Alias == normalizedAlias; });
            if (entry == m_AssetSources.end())
            {
                if (onUnregistered)
                    onUnregistered(false);
                return;
            }
            const std::filesystem::path removedRoot = entry->Desc.Root;
            entry->WatchSubscription.reset();
            m_AssetSources.erase(entry);
            m_SourceSetVersion.fetch_add(1, std::memory_order_release);
            if (!m_Registry.UnregisterSource(normalizedAlias))
            {
                Logger::Log::Warning("AssetManager: source '{}' removed locally, but registry unregister failed",
                                     normalizedAlias);
            }
            // After the registry drops the source: resolution reads the registry's
            // sources, so a resolve between the two steps could memoize a path
            // under the removed root.
            InvalidateResolveMemo();
            Logger::Log::Info("AssetManager: removed source '{}' mounted at '{}'", normalizedAlias,
                              removedRoot.string());
            if (onUnregistered)
                onUnregistered(true);
        });
    return true;
}

std::vector<AssetSourceDesc> AssetManager::GetRegisteredSources() const
{
    std::vector<AssetSourceDesc> out;
    out.reserve(m_AssetSources.size());
    for (const auto& entry : m_AssetSources)
    {
        out.push_back(entry.Desc);
    }
    return out;
}

std::filesystem::path AssetManager::GetSourceRoot(std::string_view sourceAlias) const
{
    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);

    for (const auto& entry : m_AssetSources)
    {
        if (entry.Desc.Alias == normalizedAlias)
            return entry.Desc.Root;
    }
    return {};
}

std::string AssetManager::GetSourceAliasForPath(const std::filesystem::path& assetPath) const
{
    if (assetPath.empty())
        return {};

    // Prefer registered sources first so callers can intentionally keep lookups in the
    // importer's namespace even when that source maps to the same physical root as project.
    for (const auto& entry : m_AssetSources)
    {
        if (PathIsWithinRoot(entry.Desc.Root, assetPath))
            return entry.Desc.Alias;
    }

    if (PathIsWithinRoot(GetAssetRoot(), assetPath))
        return "project";

    return {};
}

// ------------------------------------------------------------------
// BeginEjectAssetsForSource
// ------------------------------------------------------------------

void AssetManager::BeginEjectAssetsForSource(std::string_view sourceAlias, Function<void()> onDrained)
{
    // 1. Collect GUIDs belonging to this source.
    const std::vector<GUID> guids = m_Registry.GetAssetsForSource(sourceAlias);
    if (guids.empty())
    {
        // Nothing to eject, but the continuation still unregisters or rebinds
        // the source, which a save in progress forbids. Go through the same
        // readiness test rather than round it.
        PendingSourceEject storeOnly;
        storeOnly.SourceAlias = std::string(sourceAlias);
        storeOnly.OnDrained = std::move(onDrained);
        storeOnly.Started = std::chrono::steady_clock::now();
        if (IsEjectDrained(storeOnly))
        {
            if (storeOnly.OnDrained)
                storeOnly.OnDrained();
            return;
        }
        Logger::Log::Info("AssetManager: eject of source '{}' is waiting for its store to finish saving",
                          storeOnly.SourceAlias);
        m_PendingEjects.push_back(std::move(storeOnly));
        return;
    }

    // 2. Cancel the source's in-flight loads: set every entry's cancel flag
    //    first (any read/decode claimed from here on defuses itself), then
    //    withdraw queued reads and queued decode jobs. Won cancels fire the
    //    loads' failure callbacks inline on this thread — they acquire
    //    m_InFlightMutex, so no lock is held while cancelling. Waiters
    //    resolve with nullptr promptly instead of after a full-backlog load.
    //
    //    Shut the window first. Cancelling resolves the loads' waiters, and a
    //    waiter that reacts by asking for the asset again would be served from
    //    the outgoing root while the eject is still pending — a load this eject
    //    never recorded, whose callbacks the finish would drop and whose result
    //    would land as a resident under a GUID the incoming root owns. Step 5
    //    lifts the refusal, so the retry that follows the eject loads from the
    //    root that is live by then.
    SuppressLoadsWhileEjecting(guids);

    struct PendingLoad
    {
        GUID Guid;
        std::shared_ptr<std::atomic<bool>> CancelRequested;
        std::shared_ptr<std::atomic<bool>> Completed;
        JobSystem::TaskHandle DecodeHandle;
        std::shared_future<SharedPtr<Asset>> Future;
    };
    std::vector<PendingLoad> pending;
    {
        std::lock_guard<std::mutex> lk(m_InFlightMutex);
        for (const auto& guid : guids)
        {
            auto it = m_InFlight.find(guid);
            if (it == m_InFlight.end())
                continue;
            if (it->second.CancelRequested)
                it->second.CancelRequested->store(true, std::memory_order_release);
            pending.push_back({guid, it->second.CancelRequested, it->second.Completed,
                               it->second.DecodeHandle, it->second.Future});
        }
    }
    for (auto& load : pending)
    {
        if (m_IOService)
            m_IOService->CancelRequest(load.Guid, load.CancelRequested);
        if (load.DecodeHandle.IsValid())
            load.DecodeHandle.Cancel();
    }

    // 2b. Withdraw the source's async reload pipelines. Entries are erased
    //     here so any straggler completion is dropped at drain (no entry =
    //     stale); cancelled reads/decodes defuse themselves via the flag.
    //     CancelRequest may fire OnFailure inline — no locks held.
    {
        std::vector<std::pair<GUID, std::shared_ptr<std::atomic<bool>>>> activeReloads;
        {
            std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
            for (const auto& guid : guids)
            {
                auto it = m_ActiveReloads.find(guid);
                if (it == m_ActiveReloads.end())
                    continue;
                if (it->second.CancelRequested)
                    it->second.CancelRequested->store(true, std::memory_order_release);
                activeReloads.emplace_back(guid, it->second.CancelRequested);
                m_ActiveReloads.erase(it);
            }
        }
        for (auto& [guid, flag] : activeReloads)
        {
            if (m_IOService)
                m_IOService->CancelRequest(guid, flag);
        }
    }

    // 3. Record what the eject still has to outlast, and stop. A claimed read
    //    or a running decode may still register its asset (cancel is an
    //    optimization, F12), so steps 3b-7 must not run until every one of
    //    those has resolved — but waiting for them here would block whatever
    //    thread called this, which in the editor is the thread that runs the
    //    frame. The wait becomes a per-frame readiness test instead.
    PendingSourceEject eject;
    eject.SourceAlias = std::string(sourceAlias);
    eject.Guids = guids;
    eject.Outstanding.reserve(pending.size());
    for (const auto& load : pending)
        eject.Outstanding.push_back({load.Guid, load.Completed, load.Future});
    eject.OnDrained = std::move(onDrained);
    eject.Started = std::chrono::steady_clock::now();

    // Nothing genuinely in progress is the common case, and finishing it here
    // keeps that case a single synchronous step for every caller.
    if (IsEjectDrained(eject))
    {
        FinishEject(eject);
        if (eject.OnDrained)
            eject.OnDrained();
        return;
    }

    // Worth a line: from here the source's assets are unavailable and, for a
    // project switch, the switch itself is unfinished until this drains.
    Logger::Log::Info("AssetManager: eject of source '{}' is waiting on {} load(s) still running"
                      " and on any save in progress",
                      eject.SourceAlias, eject.Outstanding.size());
    m_PendingEjects.push_back(std::move(eject));
}

bool AssetManager::IsEjectPendingForSource(std::string_view normalizedAlias) const
{
    return std::any_of(m_PendingEjects.begin(), m_PendingEjects.end(),
                       [normalizedAlias](const PendingSourceEject& eject)
                       { return eject.SourceAlias == normalizedAlias; });
}

bool AssetManager::IsEjectDrained(const PendingSourceEject& eject) const
{
    for (const auto& load : eject.Outstanding)
    {
        if (load.Future.valid() && load.Future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
    }
    // The registry's own barrier: unregistering or rebinding a source destroys
    // objects an in-flight save holds raw pointers to, so neither may run while
    // one is writing. The registry enforces that by waiting; this thread runs
    // the frame and must not, so the eject waits here instead and the registry's
    // wait finds nothing to wait for.
    return !m_Registry.IsSourceFlushInProgress(eject.SourceAlias);
}

void AssetManager::FinishEject(const PendingSourceEject& eject)
{
    const std::vector<GUID>& guids = eject.Guids;

    // 3b. Remove the entries this eject recorded that their own callbacks did
    //     not already erase — and only those. A later load of the same GUID is
    //     a different generation with its own waiters; erasing it here would
    //     drop those waiters and strand its result.
    {
        std::lock_guard<std::mutex> lk(m_InFlightMutex);
        for (const auto& load : eject.Outstanding)
        {
            auto it = m_InFlight.find(load.Guid);
            if (it != m_InFlight.end() && it->second.Completed == load.Completed)
                m_InFlight.erase(it);
        }
    }

    // 4. Remove from m_LoadedAssets.
    {
        std::unique_lock<std::shared_mutex> lk(m_LoadedAssetsMutex);
        for (const auto& guid : guids)
            m_LoadedAssets.erase(guid);
    }

    // 5. Remove from m_SuppressedLoads.
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        for (const auto& guid : guids)
            m_SuppressedLoads.erase(guid);
    }

    // 6. Remove from m_PendingReloads.
    {
        std::lock_guard<std::mutex> lk(m_ReloadMutex);
        for (const auto& guid : guids)
            m_PendingReloads.erase(guid);
    }

    // 7. Dispatch AssetDestroyed events (no lock held).
    for (const auto& guid : guids)
    {
        m_EventDispatcher.DispatchEvent(AssetEvent(AssetEventType::AssetDestroyed, guid, AssetType::Unknown, String{}, String{}));
    }
}

void AssetManager::DrainPendingEjects()
{
    for (size_t i = 0; i < m_PendingEjects.size();)
    {
        if (!IsEjectDrained(m_PendingEjects[i]))
        {
            ReportStalledEject(m_PendingEjects[i]);
            ++i;
            continue;
        }
        // Move out and erase BEFORE the continuation runs: it re-enters this
        // manager (registering sources, starting loads, starting another
        // eject) and must not observe or invalidate the vector being walked.
        PendingSourceEject eject = std::move(m_PendingEjects[i]);
        m_PendingEjects.erase(m_PendingEjects.begin() + static_cast<std::ptrdiff_t>(i));
        FinishEject(eject);
        if (eject.OnDrained)
            eject.OnDrained();
    }
}

void AssetManager::ReportStalledEject(PendingSourceEject& eject)
{
    if (eject.Warned || std::chrono::steady_clock::now() - eject.Started < kEjectStallWarningDelay)
        return;
    eject.Warned = true;

    std::string outstanding;
    size_t named = 0;
    for (const auto& load : eject.Outstanding)
    {
        if (load.Future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            continue;
        if (named++ >= kMaxNamedStalledLoads)
            break;
        if (!outstanding.empty())
            outstanding += ", ";
        outstanding += load.Guid.ToString();
    }
    if (outstanding.empty())
    {
        Logger::Log::Warning(
            "AssetManager: ejecting source '{}' has waited {}s for its store to finish saving; "
            "the source stays mounted until it does",
            eject.SourceAlias,
            std::chrono::duration_cast<std::chrono::seconds>(kEjectStallWarningDelay).count());
        return;
    }
    Logger::Log::Warning(
        "AssetManager: ejecting source '{}' has waited {}s for loads that have not completed ({}); "
        "the source stays mounted until they do",
        eject.SourceAlias,
        std::chrono::duration_cast<std::chrono::seconds>(kEjectStallWarningDelay).count(), outstanding);
}

// ------------------------------------------------------------------
// BeginRebindSource
// ------------------------------------------------------------------

bool AssetManager::BeginRebindSource(std::string_view sourceAlias, const SourceRebindDesc& desc,
                                    Function<void(bool)> onRebound)
{
    if (!m_Initialized)
        return false;

    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);
    if (normalizedAlias.empty())
        return false;

    // Reject before anything changes: an unknown alias or an unusable root is
    // the caller's mistake, not a state the eject should be started for.
    const auto it = std::find_if(m_AssetSources.begin(), m_AssetSources.end(),
                                 [&normalizedAlias](const RegisteredAssetSource& entry)
                                 { return entry.Desc.Alias == normalizedAlias; });
    if (it == m_AssetSources.end())
    {
        Logger::Log::Error("AssetManager::BeginRebindSource: unknown alias '{}'", normalizedAlias);
        return false;
    }

    std::error_code ec;
    if (desc.NewRoot.empty() || !std::filesystem::exists(desc.NewRoot, ec) || !std::filesystem::is_directory(desc.NewRoot, ec))
    {
        Logger::Log::Error("AssetManager::BeginRebindSource: invalid new root '{}'", desc.NewRoot.string());
        return false;
    }

    if (IsEjectPendingForSource(normalizedAlias))
    {
        Logger::Log::Error("AssetManager::BeginRebindSource: an eject of source '{}' is still pending;"
                           " wait for the first one to complete",
                           normalizedAlias);
        return false;
    }

    BeginEjectAssetsForSource(normalizedAlias,
                              [this, normalizedAlias, desc, onRebound = std::move(onRebound)]()
                              {
                                  const bool ok = CompleteRebindSource(normalizedAlias, desc);
                                  if (onRebound)
                                      onRebound(ok);
                              });
    return true;
}

bool AssetManager::CompleteRebindSource(const std::string& normalizedAlias, const SourceRebindDesc& desc)
{
    const auto it = std::find_if(m_AssetSources.begin(), m_AssetSources.end(),
                                 [&normalizedAlias](const RegisteredAssetSource& entry)
                                 { return entry.Desc.Alias == normalizedAlias; });
    if (it == m_AssetSources.end())
    {
        Logger::Log::Error("AssetManager: source '{}' disappeared while its assets were ejected",
                           normalizedAlias);
        return false;
    }
    const std::filesystem::path oldRoot = it->Desc.Root;

    // Build new descriptor for the registry.
    AssetSourceDesc newDesc = it->Desc;
    newDesc.Root = desc.NewRoot;
    if (!desc.AuthoritativeDbFile.empty())
        newDesc.AuthoritativeDbFile = desc.AuthoritativeDbFile;
    if (!desc.CacheRoot.empty())
        newDesc.CacheRoot = desc.CacheRoot;

    // Delegate to registry (closes old store, loads new store, reconciles, populates, scans).
    if (!m_Registry.RebindSource(normalizedAlias, newDesc))
    {
        Logger::Log::Error("AssetManager: registry rebind failed for '{}'", normalizedAlias);
        return false;
    }

    // Swap the file watcher.
    it->WatchSubscription.reset();
    {
        FilePattern pattern(desc.NewRoot, ".*", {}, true);
        auto& service = FileWatchingService::GetInstance();
        it->WatchSubscription.emplace(service.Subscribe(pattern, [this, alias = normalizedAlias](const FileChangeEvent& event)
                                                        { OnWatchedFileChanged(event, alias); }));
    }

    // Update local descriptor.
    it->Desc.Root = desc.NewRoot;
    if (!desc.AuthoritativeDbFile.empty())
        it->Desc.AuthoritativeDbFile = desc.AuthoritativeDbFile;
    if (!desc.CacheRoot.empty())
        it->Desc.CacheRoot = desc.CacheRoot;
    m_SourceSetVersion.fetch_add(1, std::memory_order_release);
    InvalidateResolveMemo();

    Logger::Log::Info("AssetManager: remounted source '{}' from '{}' to '{}'",
                      normalizedAlias,
                      oldRoot.string(),
                      it->Desc.Root.string());
    return true;
}

// ------------------------------------------------------------------
// Resolution APIs
// ------------------------------------------------------------------

std::filesystem::path AssetManager::ResolveAssetPath(const std::filesystem::path& assetPath) const
{
    if (assetPath.empty())
        return {};
    if (assetPath.is_absolute())
        return assetPath;

    std::string prefixedAlias;
    std::filesystem::path prefixedRelativeAssetPath;
    if (TrySplitAssetSourcePrefix(assetPath, prefixedAlias, prefixedRelativeAssetPath))
    {
        return ResolveAssetPath(prefixedRelativeAssetPath, prefixedAlias);
    }

    // E4: memoized implicit resolution. The fallthrough below costs one
    // fs::exists per source per call; hot callers (CSS @import, texture
    // references) resolve the same relative paths repeatedly.
    const std::string memoKey = assetPath.generic_string();
    {
        std::shared_lock<std::shared_mutex> lk(m_ResolveMemoMutex);
        auto it = m_ResolveMemo.find(memoKey);
        if (it != m_ResolveMemo.end())
            return it->second;
    }

    const std::filesystem::path normalizedRelativeAssetPath = NormalizeRelativeAssetPath(assetPath);

    // The registry resolves it: the project root first, then the registered sources
    // by priority. Files and read-only manifest identities are memoized; a not-found fallback must keep
    // re-probing so a file created later resolves correctly even when no
    // watcher event reaches us.
    const auto memoize = [this, &memoKey](const std::filesystem::path& resolved)
    {
        std::unique_lock<std::shared_mutex> lk(m_ResolveMemoMutex);
        m_ResolveMemo[memoKey] = resolved;
    };

    const std::filesystem::path resolved = m_Registry.ResolveRelativeAssetPath(normalizedRelativeAssetPath);
    if (!resolved.empty())
    {
        memoize(resolved);
        return resolved;
    }
    return (GetAssetRoot() / normalizedRelativeAssetPath).lexically_normal();
}

void AssetManager::InvalidateResolveMemo()
{
    std::unique_lock<std::shared_mutex> lk(m_ResolveMemoMutex);
    m_ResolveMemo.clear();
}

std::filesystem::path AssetManager::ResolveAssetPathInSource(
    const std::filesystem::path& normalizedRelativeAssetPath, const std::string& normalizedAlias) const
{
    if (normalizedAlias == kAssetSourceAliasProject)
        return (GetAssetRoot() / normalizedRelativeAssetPath).lexically_normal();

    for (const auto& source : m_AssetSources)
    {
        if (source.Desc.Alias == normalizedAlias)
            return (source.Desc.Root / normalizedRelativeAssetPath).lexically_normal();
    }
    return {};
}

std::filesystem::path AssetManager::ResolveAssetPath(const std::filesystem::path& assetPath,
                                                     std::string_view sourceAlias) const
{
    if (assetPath.empty())
        return {};
    if (assetPath.is_absolute())
        return assetPath;
    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);
    if (normalizedAlias.empty())
    {
        Logger::Log::Warning("AssetManager: explicit ResolveAssetPath called with empty source alias");
        return {};
    }
    if (!IsValidAssetSourceAlias(normalizedAlias))
    {
        Logger::Log::Warning("AssetManager: explicit ResolveAssetPath called with invalid source alias '{}'",
                             std::string(sourceAlias));
        return {};
    }

    std::filesystem::path resolved =
        ResolveAssetPathInSource(NormalizeRelativeAssetPath(assetPath), normalizedAlias);
    if (!resolved.empty())
        return resolved;

    // Once per alias: explicit-alias resolution sits on per-frame paths
    // (shader lookups), so an unregistered alias must not flood the log.
    bool firstMiss = false;
    {
        std::lock_guard<std::mutex> lock(m_UnknownAliasWarnMutex);
        firstMiss = m_UnknownAliasesWarned.insert(normalizedAlias).second;
    }
    if (firstMiss)
    {
        Logger::Log::Warning("AssetManager: unknown asset source alias '{}' for '{}' "
                             "(further misses for this alias are not logged)",
                             normalizedAlias,
                             assetPath.string());
    }
    return {};
}

std::filesystem::path AssetManager::ResolveAssetPathPreferringSource(const std::filesystem::path& assetPath,
                                                                     std::string_view sourceAlias,
                                                                     AssetPathKind candidateKind) const
{
    if (assetPath.empty())
        return {};
    if (assetPath.is_absolute())
        return assetPath;

    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);
    if (!normalizedAlias.empty() && IsValidAssetSourceAlias(normalizedAlias))
    {
        const std::filesystem::path preferred =
            ResolveAssetPathInSource(NormalizeRelativeAssetPath(assetPath), normalizedAlias);
        if (!preferred.empty())
        {
            std::error_code ec;
            const bool present = candidateKind == AssetPathKind::Directory
                                     ? std::filesystem::is_directory(preferred, ec)
                                     : m_Registry.IsAssetPathAvailable(preferred);
            if (present)
                return preferred;
        }
    }

    return ResolveAssetPath(assetPath);
}

std::filesystem::path AssetManager::ResolveAssetPathFromReference(
    const std::filesystem::path& assetPath, const std::filesystem::path& referrerAssetPath) const
{
    if (assetPath.empty())
        return {};
    if (assetPath.is_absolute())
        return assetPath;

    std::string referrerAlias = m_Registry.GetAssetSourceOwnerAlias(referrerAssetPath);
    if (referrerAlias.empty())
        referrerAlias = GetSourceAliasForPath(referrerAssetPath);

    // A referrer under no registered source yields no alias, which the
    // preferring resolve reads as "nothing to prefer" — the same fallthrough
    // as a referrer whose own source simply lacks the file.
    return ResolveAssetPathPreferringSource(assetPath, referrerAlias, AssetPathKind::AnyEntry);
}

GUID AssetManager::ResolveAssetGuid(const std::filesystem::path& assetPath)
{
    const std::filesystem::path resolvedAssetPath = ResolveAssetPath(assetPath);
    if (resolvedAssetPath.empty())
        return GUID::Null();

    if (!m_Registry.IsAssetRegistered(resolvedAssetPath))
    {
        (void)m_Registry.RegisterAsset(resolvedAssetPath);
    }
    return m_Registry.GetAssetGUID(resolvedAssetPath);
}

GUID AssetManager::ResolveAssetGuid(const std::filesystem::path& assetPath, std::string_view sourceAlias)
{
    const std::filesystem::path resolvedAssetPath = ResolveAssetPath(assetPath, sourceAlias);
    if (resolvedAssetPath.empty())
        return GUID::Null();

    // Use the source-aware overload so that when project and editor share the
    // same root, the asset gets the correct derived-identity GUID for the
    // requested source rather than the project's stored GUID.
    const std::string normalizedAlias = NormalizeAssetSourceAlias(sourceAlias);
    (void)m_Registry.RegisterAsset(resolvedAssetPath, normalizedAlias);
    return m_Registry.GetAssetGUID(resolvedAssetPath);
}

GUID AssetManager::ResolveAssetGuidFromReference(const std::filesystem::path& assetPath,
                                                 const std::filesystem::path& referrerAssetPath)
{
    const std::filesystem::path resolvedAssetPath = ResolveAssetPathFromReference(assetPath, referrerAssetPath);
    if (resolvedAssetPath.empty())
        return GUID::Null();

    // Prefer explicit source ownership of the referrer when available.
    std::string preferredAlias = m_Registry.GetAssetSourceOwnerAlias(referrerAssetPath);
    if (preferredAlias.empty())
        preferredAlias = GetSourceAliasForPath(referrerAssetPath);
    if (preferredAlias.empty())
        preferredAlias = GetSourceAliasForPath(resolvedAssetPath);

    if (!preferredAlias.empty())
    {
        (void)m_Registry.RegisterAsset(resolvedAssetPath, preferredAlias);
    }
    else if (!m_Registry.IsAssetRegistered(resolvedAssetPath))
    {
        (void)m_Registry.RegisterAsset(resolvedAssetPath);
    }

    return m_Registry.GetAssetGUID(resolvedAssetPath);
}

void AssetManager::ReportAssetNotInBuild(std::string_view assetId)
{
    if (!m_ContentIsPackaged)
        return;
    {
        std::lock_guard<std::mutex> lock(m_NotInBuildWarnMutex);
        if (!m_NotInBuildWarned.emplace(assetId).second)
            return;
    }
    Logger::Log::Warning("asset {} is not in this build: reference it from a scene or material in the build, "
                         "or list it under runtimeAssets in the package's package.json",
                         assetId);
}

AssetFuture AssetManager::LoadAssetAsync(const GUID& requestedGuid, AssetLoadPriority priority)
{
    const GUID guid = m_Registry.ResolveGuid(requestedGuid);

    // Fast path: already loaded. Point lookups of m_LoadedAssets go through
    // GetLoadedAsset so the map lock stays inside it and can never span the
    // handle/future construction that follows.
    if (SharedPtr<Asset> cached = GetLoadedAsset(guid, true))
    {
        std::promise<SharedPtr<Asset>> p;
        p.set_value(std::move(cached));
        return CreateEnhancedFuture(p.get_future().share(), guid);
    }

    // Short-circuit if this GUID already failed or is unsupported.
    if (IsLoadSuppressed(guid))
    {
        std::promise<SharedPtr<Asset>> p;
        p.set_value(nullptr);
        return CreateEnhancedFuture(p.get_future().share(), guid);
    }

    // Dedupe: join or create a new in-flight entry
    std::shared_future<SharedPtr<Asset>> sharedFut;
    bool created = false;
    {
        std::lock_guard<std::mutex> inflightLock(m_InFlightMutex);
        auto it = m_InFlight.find(guid);
        if (it != m_InFlight.end())
        {
            // Update priority if not yet submitted
            if (!it->second.Submitted && priority > it->second.Priority)
            {
                it->second.Priority = priority;
            }
            it->second.Transient = false;
            sharedFut = it->second.Future;
        }
        else
        {
            // Create new in-flight entry
            InFlightEntry entry;
            {
                auto pair = MakeSharedPromise<SharedPtr<Asset>>();
                entry.Promise = pair.first;
                entry.Future = pair.second;
            }
            entry.Priority = priority;
            entry.Submitted = false;
            entry.Completed = std::make_shared<std::atomic<bool>>(false);
            entry.CancelRequested = std::make_shared<std::atomic<bool>>(false);
            sharedFut = entry.Future;
            m_InFlight.emplace(guid, std::move(entry));
            created = true;
        }
    }

    // If we just created it, submit the actual load now
    if (created)
    {
        AssetMetadata metadata{};
        if (!m_Registry.TryGetAssetMetadata(guid, metadata))
        {
            if (!guid.IsNull())
                ReportAssetNotInBuild(guid.ToString());
            // Fail + erase the entry, firing any dedupe joiner's callbacks.
            ResolveAndEraseInFlight(guid, nullptr);
            std::promise<SharedPtr<Asset>> p;
            p.set_value(nullptr);
            return CreateEnhancedFuture(p.get_future().share(), guid);
        }

        // Avoid building a full pipeline when we know the asset cannot be created.
        // This commonly happens for unknown file types that are still tracked by the AssetDatabase.
        String unsupportedReason;
        if (!IsAssetSupportedForLoad(metadata, &unsupportedReason))
        {
            MarkLoadSuppressed(guid, metadata, unsupportedReason);
            ResolveAndEraseInFlight(guid, nullptr);

            std::promise<SharedPtr<Asset>> p;
            p.set_value(nullptr);
            return CreateEnhancedFuture(p.get_future().share(), guid);
        }

        // Mark as submitted (and read final priority)
        AssetLoadPriority finalPriority;
        std::shared_ptr<std::promise<SharedPtr<Asset>>> promise;
        std::shared_ptr<std::atomic<bool>> doneFlag;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        {
            std::lock_guard<std::mutex> inflightLock(m_InFlightMutex);
            auto& e = m_InFlight[guid];
            e.Submitted = true;
            finalPriority = e.Priority;
            promise = e.Promise;
            doneFlag = e.Completed;
            cancelFlag = e.CancelRequested;
        }

        if (m_JobSystem)
        {
            SubmitInFlightLoad(metadata, finalPriority, promise, doneFlag, cancelFlag);
        }
        else
        {
            // No job system: run the load synchronously and fulfill the promise
            // immediately (no extra thread)
            SharedPtr<Asset> result;
            try
            {
                result = LoadAssetSynchronous(metadata).get();
            }
            catch (...)
            {
                result = nullptr;
            }
            if (!result)
            {
                MarkLoadFailed(guid);
            }
            // Resolve + erase the entry (firing any dedupe joiner's callbacks);
            // uses the entry's own promise/Completed (same shared_ptrs grabbed above).
            ResolveAndEraseInFlight(guid, result);
        }
    }

    // Return an enhanced future for callers without scheduling a JobSystem worker
    // that blocks on completion. Blocking the same worker pool that the asset
    // pipeline uses can deadlock small pools (common in unit tests).
    return CreateEnhancedFuture(sharedFut, guid);
}

AssetLoadHandle AssetManager::LoadAsset(const GUID& guid,
                                              AssetLoadResultCallback callback,
                                              AssetLoadPriority priority)
{
    AssetLoadRequest request(guid, std::move(callback), priority);
    return LoadAsset(request);
}

AssetLoadHandle AssetManager::LoadAsset(const std::filesystem::path& assetPath,
                                              AssetLoadResultCallback callback,
                                              AssetLoadPriority priority)
{
    const GUID guid = ResolveAssetGuid(assetPath);
    if (guid.IsNull())
    {
        ReportAssetNotInBuild(assetPath.generic_string());
        if (callback)
        {
            callback(Result<SharedPtr<Asset>, AssetError>(AssetError::Missing));
        }
        return {};
    }
    return LoadAsset(guid, std::move(callback), priority);
}

AssetLoadHandle AssetManager::LoadAsset(const std::filesystem::path& assetPath,
                                              std::string_view sourceAlias,
                                              AssetLoadResultCallback callback,
                                              AssetLoadPriority priority)
{
    const GUID guid = ResolveAssetGuid(assetPath, sourceAlias);
    if (guid.IsNull())
    {
        ReportAssetNotInBuild(assetPath.generic_string());
        if (callback)
        {
            callback(Result<SharedPtr<Asset>, AssetError>(AssetError::Missing));
        }
        return {};
    }
    return LoadAsset(guid, std::move(callback), priority);
}

AssetLoadHandle AssetManager::LoadAsset(const AssetLoadRequest& request)
{
    // Fast path: already loaded. The lookup owns the map lock and releases it
    // before CreateLoadHandle runs: that path re-reads the map (GetLoadedAsset)
    // and invokes the caller's callback, and m_LoadedAssetsMutex must be held
    // across neither. std::shared_mutex is not recursive (a nested shared
    // acquire is UB) and SRW locks are writer-priority, so a second shared
    // acquire behind a decode thread queued for exclusive in RegisterLoadedAsset
    // never clears. Firing the callback outside the lock also matches the
    // completion path (FirePendingLoadCallbacks), so a callback behaves the same
    // whether or not the asset was already cached.
    if (SharedPtr<Asset> cached = GetLoadedAsset(request.AssetGuid, request.Residency == AssetResidency::Pinned))
    {
        // Provide a ready shared_future
        std::promise<SharedPtr<Asset>> p;
        p.set_value(std::move(cached));
        return CreateLoadHandle(request.AssetGuid, request.OnComplete, p.get_future().share());
    }

    // Short-circuit if this GUID already failed or is unsupported.
    if (IsLoadSuppressed(request.AssetGuid))
    {
        std::promise<SharedPtr<Asset>> p;
        p.set_value(nullptr);
        return CreateLoadHandle(request.AssetGuid, request.OnComplete, p.get_future().share());
    }

    // Dedupe: join or create a new in-flight entry
    std::shared_future<SharedPtr<Asset>> sharedFut;
    bool created = false;
    {
        std::lock_guard<std::mutex> inflightLock(m_InFlightMutex);
        auto it = m_InFlight.find(request.AssetGuid);
        if (it != m_InFlight.end())
        {
            // Escalate priority if possible
            if (!it->second.Submitted && request.Priority > it->second.Priority)
            {
                it->second.Priority = request.Priority;
            }
            if (request.Residency == AssetResidency::Pinned)
                it->second.Transient = false;
            sharedFut = it->second.Future;
        }
        else
        {
            InFlightEntry entry;
            {
                auto pair = MakeSharedPromise<SharedPtr<Asset>>();
                entry.Promise = pair.first;
                entry.Future = pair.second;
            }
            entry.Priority = request.Priority;
            entry.Submitted = false;
            entry.Completed = std::make_shared<std::atomic<bool>>(false);
            entry.CancelRequested = std::make_shared<std::atomic<bool>>(false);
            entry.Transient = request.Residency == AssetResidency::Transient;
            sharedFut = entry.Future;
            m_InFlight.emplace(request.AssetGuid, std::move(entry));
            created = true;
        }
    }

    // If we created the entry, kick off the actual load
    if (created)
    {
        AssetMetadata metadata{};
        if (!m_Registry.TryGetAssetMetadata(request.AssetGuid, metadata))
        {
            if (!request.AssetGuid.IsNull())
                ReportAssetNotInBuild(request.AssetGuid.ToString());
            // Fail + erase the entry, firing any dedupe joiner's callbacks.
            ResolveAndEraseInFlight(request.AssetGuid, nullptr);
            // Return handle with a ready future for error
            std::promise<SharedPtr<Asset>> p;
            p.set_value(nullptr);
            return CreateLoadHandle(request.AssetGuid, request.OnComplete, p.get_future().share());
        }

        // Avoid building a full pipeline when we know the asset cannot be created.
        String unsupportedReason;
        if (!IsAssetSupportedForLoad(metadata, &unsupportedReason))
        {
            MarkLoadSuppressed(request.AssetGuid, metadata, unsupportedReason);
            ResolveAndEraseInFlight(request.AssetGuid, nullptr);

            std::promise<SharedPtr<Asset>> p;
            p.set_value(nullptr);
            return CreateLoadHandle(request.AssetGuid, request.OnComplete, p.get_future().share());
        }

        // Read final priority and promise, mark submitted
        AssetLoadPriority finalPriority;
        std::shared_ptr<std::promise<SharedPtr<Asset>>> promise;
        std::shared_ptr<std::atomic<bool>> doneFlag;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        {
            std::lock_guard<std::mutex> inflightLock(m_InFlightMutex);
            auto& e = m_InFlight[request.AssetGuid];
            e.Submitted = true;
            finalPriority = e.Priority;
            promise = e.Promise;
            doneFlag = e.Completed;
            cancelFlag = e.CancelRequested;
        }

        if (m_JobSystem)
        {
            SubmitInFlightLoad(metadata, finalPriority, promise, doneFlag, cancelFlag);
        }
        else
        {
            // No job system: run the load synchronously and fulfill the promise
            // immediately (no extra thread)
            SharedPtr<Asset> result;
            try
            {
                result = LoadAssetSynchronous(metadata).get();
            }
            catch (...)
            {
                result = nullptr;
            }
            if (!result)
            {
                MarkLoadFailed(request.AssetGuid);
            }
            // Resolve + erase the entry (firing any dedupe joiner's callbacks);
            // uses the entry's own promise/Completed (same shared_ptrs grabbed above).
            ResolveAndEraseInFlight(request.AssetGuid, result);
        }
    }

    // Return a handle that shares the same future and wires the callback appropriately
    return CreateLoadHandle(request.AssetGuid, request.OnComplete, sharedFut);
}

std::future<void> AssetManager::UnloadAssetAsync(const GUID& guid)
{
    // An in-flight load of this GUID is moot once the caller decided to
    // unload: withdraw it (queued work dies, waiters resolve with nullptr,
    // no suppression is recorded).
    CancelInFlightLoad(guid);

    // If no job system available, unload synchronously
    if (!m_JobSystem)
    {
        std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
        auto it = m_LoadedAssets.find(guid);
        if (it != m_LoadedAssets.end())
        {
            it->second.Instance->Unload();
            m_LoadedAssets.erase(it);
            Logger::Log::Debug("Unloaded asset: {}", guid.ToString());
        }

        std::promise<void> promise;
        promise.set_value();
        return promise.get_future();
    }

    // Start async unload
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    auto task = std::make_unique<AssetUnloadTask>(m_JobSystem, guid, *this);
    m_JobSystem->Submit(std::move(task));

    return future;
}

bool AssetManager::ReleaseTransientAsset(const GUID& requestedGuid)
{
    const GUID guid = m_Registry.ResolveGuid(requestedGuid);
    // A reload in flight still adopts its payload into this instance.
    if (IsReloadInFlight(guid))
        return false;
    SharedPtr<Asset> released;
    {
        // The exclusive lock orders this check after every GetAsset that
        // pinned the asset.
        std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
        auto it = m_LoadedAssets.find(guid);
        if (it == m_LoadedAssets.end() || !it->second.Transient.load())
            return false;
        released = std::move(it->second.Instance);
        m_LoadedAssets.erase(it);
    }

    const AssetType type = released->GetType();
    const std::string path = released->GetPath().string();
    released->Unload();
    UpdateMemoryStatsIncremental(released, false);
    released.reset();
    DispatchWithRedirectSources(AssetEvents::AssetUnloaded(guid, type, path));
    return true;
}

SharedPtr<Asset> AssetManager::GetAsset(const GUID& requestedGuid) const
{
    const GUID guid = m_Registry.ResolveGuid(requestedGuid);
    AssetDbProfiler::LockScope profileLock(AssetDbProfiler::IsOnMainThread()
        ? AssetDbProfiler::Bucket::LockLoadedAssetsMain
        : AssetDbProfiler::Bucket::LockLoadedAssetsWorker);
    std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
    profileLock.OnAcquired();
    auto it = m_LoadedAssets.find(guid);
    if (it == m_LoadedAssets.end())
        return nullptr;
    it->second.Pin();
    return it->second.Instance;
}

bool AssetManager::IsAssetLoaded(const GUID& requestedGuid) const
{
    const GUID guid = m_Registry.ResolveGuid(requestedGuid);
    AssetDbProfiler::LockScope profileLock(AssetDbProfiler::IsOnMainThread()
        ? AssetDbProfiler::Bucket::LockLoadedAssetsMain
        : AssetDbProfiler::Bucket::LockLoadedAssetsWorker);
    std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
    profileLock.OnAcquired();
    return m_LoadedAssets.find(guid) != m_LoadedAssets.end();
}

void AssetManager::SetHotReloadEnabled(bool enabled)
{
    m_HotReloadEnabled = enabled;
    if (enabled)
    {
        m_LastTimestampReloadCheck = {};
        m_TimestampReloadScanList.clear();
        m_TimestampReloadScanIndex = 0;
    }
    // File watching is provided by FileWatchingService. We keep the subscription active
    // and only toggle whether change events schedule reloads of loaded assets.
    Logger::Log::Info("Asset hot reloading {}", enabled ? "enabled" : "disabled");
}

AssetManager::MemoryStats AssetManager::GetMemoryStats() const
{
    // Update cache if needed (lock-free check first)
    if (m_StatsNeedUpdate.load(std::memory_order_acquire))
    {
        UpdateMemoryStatsCache();
    }

    MemoryStats stats{};
    stats.LoadedAssetCount = m_CachedAssetCount.load(std::memory_order_relaxed);
    stats.TotalMemoryUsed = m_CachedTotalMemory.load(std::memory_order_relaxed);
    stats.LastUpdated = std::chrono::steady_clock::now();

    // Copy cached memory by type (this is the only part that needs a brief lock)
    {
        std::lock_guard<std::mutex> lock(m_StatsCacheMutex);
        for (const auto& [type, memory] : m_CachedMemoryByType)
        {
            stats.MemoryByType[type] = memory.load(std::memory_order_relaxed);
        }
    }

    return stats;
}

AssetManager::MemoryStats AssetManager::RecalculateMemoryStats() const
{
    MemoryStats stats{};
    std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);

    stats.LoadedAssetCount = m_LoadedAssets.size();
    stats.TotalMemoryUsed = 0;
    stats.LastUpdated = std::chrono::steady_clock::now();

    // Calculate actual memory usage from loaded assets
    for (const auto& [guid, entry] : m_LoadedAssets)
    {
        const auto& asset = entry.Instance;
        if (asset)
        {
            uint64 assetMemory = asset->GetMemoryUsage();
            stats.TotalMemoryUsed += assetMemory;

            // Track by asset type
            AssetType type = asset->GetType();
            stats.MemoryByType[type] += assetMemory;
        }
    }

    // Update cache with fresh values
    m_CachedAssetCount.store(stats.LoadedAssetCount, std::memory_order_relaxed);
    m_CachedTotalMemory.store(stats.TotalMemoryUsed, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> cacheLock(m_StatsCacheMutex);
        m_CachedMemoryByType.clear();
        for (const auto& [type, memory] : stats.MemoryByType)
        {
            m_CachedMemoryByType[type].store(memory, std::memory_order_relaxed);
        }
    }

    m_StatsNeedUpdate.store(false, std::memory_order_release);

    return stats;
}

SharedPtr<Asset> AssetManager::CreateAsset(const AssetMetadata& metadata)
{
    // First try to use asset type registry for direct creation
    auto& typeRegistry = m_Registry.GetTypeRegistry();
    AssetType resolvedType = metadata.Type;
    if (resolvedType == AssetType::Unknown)
    {
        resolvedType = typeRegistry.GetAssetTypeFromExtension(metadata.Extension);
    }

    if (resolvedType != AssetType::Unknown && typeRegistry.IsAssetTypeRegistered(resolvedType))
    {
        AssetMetadata md = metadata;
        md.Type = resolvedType;
        SharedPtr<Asset> asset;
        String failureReason = "factory returned null";
        try
        {
            asset = typeRegistry.CreateAsset(md);
        }
        catch (const std::exception& e)
        {
            failureReason = e.what();
        }
        catch (...)
        {
            failureReason = "factory threw a non-standard exception";
        }

        if (asset)
        {
            Logger::Log::Debug("Created asset using asset type registry: {}", metadata.Name);
            return asset;
        }
        Logger::Log::Warning("Asset type registry failed to create asset: {} (type: {}): {}",
                             metadata.Name, AssetTypeToString(resolvedType), failureReason);
    }

    // Fallback to parser registry for backward compatibility
    AssetParseResult result = m_ParserRegistry.ParseAsset(metadata, *this);

    if (result.Success && result.ParsedAsset)
    {
        Logger::Log::Debug("Created asset using parser registry: {}", metadata.Name);
        return result.ParsedAsset;
    }

    // Log detailed error information
    Logger::Log::Error("Failed to create asset: {} - Type: {}, Extension: {}, Error: {}",
                       metadata.Name, AssetTypeToString(metadata.Type), metadata.Extension, result.ErrorMessage);

    return nullptr;
}

void AssetManager::OnWatchedFileChanged(const FileChangeEvent& event, std::string_view sourceAliasHint)
{
    // A change that carries a write this process announced has already been delivered by
    // the writer's own report, and applying it again would re-sync the registry, dispatch
    // a second asset-changed event and re-mark the reload for a change nobody made.
    //
    // Only the event types that say "the file has these bytes now" can repeat a write.
    // A Deleted is left alone: the watcher already cancels the one a replace-rename
    // produces, and swallowing a real deletion would strand the asset in the registry.
    const bool carriesContent = event.Type == FileChangeType::Created ||
                                event.Type == FileChangeType::Modified ||
                                event.Type == FileChangeType::Renamed;
    if (carriesContent && m_ExpectedWrites.IsEchoOfOurWrite(event.Path))
    {
        Logger::Log::Debug("Already delivered this write: {}", event.Path.string());
        return;
    }

    ApplyFileChange(event, sourceAliasHint);
}

void AssetManager::ApplyFileChange(const FileChangeEvent& event, std::string_view sourceAliasHint)
{
    Logger::Log::Debug("File changed: {} ({})", event.Path.string(), static_cast<int>(event.Type));

    // E4: any watched change may flip which mount a relative path resolves
    // to (create/delete under an overlapping root) — drop the whole memo.
    InvalidateResolveMemo();

    const std::string normalizedHint = NormalizeAssetSourceAlias(sourceAliasHint);

    // Skip the authoritative DB and internal/cache files to prevent recursive processing / feedback loops.
    const auto IsIgnoredPath = [](const std::filesystem::path& p) -> bool
    {
        if (p.empty())
            return true;

        // Authoritative DB file (should not be treated as an asset).
        if (p.filename() == "AssetDatabase.assetdb")
            return true;

        // Published stored-identity manifest: lives inside a package's
        // Assets/, written by the explicit publish step — never an asset.
        if (p.filename() == ".assetmanifest")
            return true;

        // Derived/cache folders (best-effort; note that FileWatcher also excludes these).
        std::string s = p.generic_string();
        std::replace(s.begin(), s.end(), '\\', '/');
        if (s.find("/.Cache/") != std::string::npos)
            return true;
        if (s.find("/.MyEngine/") != std::string::npos)
            return true;
        if (s.find("/.Editor/") != std::string::npos)
            return true;

        return false;
    };

    if (IsIgnoredPath(event.Path) || (!event.OldPath.empty() && IsIgnoredPath(event.OldPath)))
    {
        Logger::Log::Debug("Ignoring internal file change: {}", event.Path.string());
        return;
    }

    // Helper: test whether a path currently points to a regular file.
    auto IsRegularFile = [](const std::filesystem::path& p) -> bool
    {
        if (p.empty())
            return false;
        std::error_code ec;
        return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
    };

    // Keep AssetRegistry in sync with on-disk changes.
    // Note: this runs on whichever thread reported the change — the file watcher's for an
    // external edit, the writer's own for a save — so keep work minimal and avoid
    // touching UI. The reload it marks is drained on the main thread by Update().
    //
    // Ownership handling:
    // - preserve explicit ownership when present (editor/project overlap safety),
    // - avoid assigning ownership in ambiguous overlap cases,
    // - otherwise use source hint or unique root membership when available.
    auto inferRegistrationAlias = [&](const std::filesystem::path& p) -> std::string
    {
        // Explicit ownership always wins.
        std::string ownerAlias = m_Registry.GetAssetSourceOwnerAlias(p);
        if (!ownerAlias.empty())
            return ownerAlias;

        // Determine unique containing source by root membership.
        std::string uniqueAlias;
        int matchCount = 0;
        const auto sources = m_Registry.GetRegisteredSources();
        for (const auto& source : sources)
        {
            if (!PathIsWithinRoot(source.Root, p))
                continue;
            ++matchCount;
            uniqueAlias = source.Alias;
            if (matchCount > 1)
            {
                uniqueAlias.clear();
                break; // ambiguous overlap, avoid assigning new owner
            }
        }
        if (!uniqueAlias.empty())
            return uniqueAlias;

        // Last resort: use watcher hint only when we had no ambiguity signal.
        if (matchCount == 0 && !normalizedHint.empty())
            return normalizedHint;

        return {};
    };

    auto registerWithBestAlias = [&](const std::filesystem::path& p) -> GUID
    {
        if (p.empty())
            return GUID::Null();

        const std::string alias = inferRegistrationAlias(p);
        if (!alias.empty())
        {
            (void)m_Registry.RegisterAsset(p, alias);
        }
        else
        {
            (void)m_Registry.RegisterAsset(p);
        }
        return m_Registry.GetAssetGUID(p);
    };

    GUID guid = GUID::Null();
    switch (event.Type)
    {
    case FileChangeType::Created:
    {
        if (IsRegularFile(event.Path))
        {
            guid = registerWithBestAlias(event.Path);
        }
        break;
    }
    case FileChangeType::Modified:
    {
        if (IsRegularFile(event.Path))
        {
            guid = registerWithBestAlias(event.Path);
            (void)m_Registry.TryUpdateFilesystemMetadata(event.Path);
        }
        break;
    }
    case FileChangeType::Deleted:
    {
        guid = m_Registry.GetAssetGUID(event.Path);
        (void)m_Registry.TryUnregisterAssetByPath(event.Path);
        break;
    }
    case FileChangeType::Renamed:
    {
        // GUID is tracked by oldPath when available.
        if (!event.OldPath.empty())
        {
            guid = m_Registry.GetAssetGUID(event.OldPath);
            // RenameAssetPath (not the bare registry call) so a loaded
            // instance's recorded path moves with the file on external
            // renames too.
            if (RenameAssetPath(event.OldPath, event.Path))
            {
                // Replace-style renames (safe-save temp -> target) keep the
                // destination's identity, so re-query to dispatch the event
                // with the surviving GUID. Plain renames resolve to the same
                // GUID that rode along with the path.
                guid = m_Registry.GetAssetGUID(event.Path);
            }
            else
            {
                // If the old path was not registered, attempt to register the new path so
                // future loads have identity in the asset database.
                if (IsRegularFile(event.Path))
                {
                    guid = registerWithBestAlias(event.Path);
                }
            }
        }
        else
        {
            // Best-effort fallback: treat as create+delete without GUID preservation.
            if (IsRegularFile(event.Path))
            {
                guid = registerWithBestAlias(event.Path);
            }
        }
        break;
    }
    }

    // Dispatch an event for the changed asset itself (if registered).
    if (!guid.IsNull())
    {
        // Any on-disk change can make a previously unsupported asset become supported
        // (e.g., rename changes extension), so clear the suppression cache.
        ClearLoadSuppressed(guid);

        // Determine asset type for this change.
        //
        // IMPORTANT: do not rely solely on the registry's stored metadata.Type here.
        // In practice, files can be registered before parsers/types are available,
        // and some update paths only refresh mtime/size (not type). UI hot reload
        // routes off AssetEvent.EventAssetType, so ensure we have a correct classification
        // even in those cases.
        AssetMetadata metadata{};
        AssetType assetType = AssetType::Unknown;
        if (m_Registry.TryGetAssetMetadata(guid, metadata))
        {
            assetType = metadata.Type;
        }
        if (assetType == AssetType::Unknown)
        {
            assetType = m_Registry.ClassifyAssetType(event.Path);
        }
        if (assetType == AssetType::Unknown)
        {
            assetType = GetAssetTypeFromExtension(event.Path.extension().string());
        }

        // Create and dispatch asset event
        AssetEventType eventType = AssetEventType::AssetModified; // Default to modified
        switch (event.Type)
        {
        case FileChangeType::Created:
            eventType = AssetEventType::AssetCreated;
            break;
        case FileChangeType::Modified:
            eventType = AssetEventType::AssetModified;
            break;
        case FileChangeType::Deleted:
            eventType = AssetEventType::AssetDestroyed;
            break;
        case FileChangeType::Renamed:
            eventType = AssetEventType::AssetModified;
            break; // Map renamed to modified
        }

        DispatchWithRedirectSources(AssetEvent(eventType, guid, assetType, event.Path.string()));
    }

    // If a shader source file changed, schedule reload of any loaded shader programs that reference it.
    std::vector<GUID> dependentShaderPrograms;
    {
        std::string ext = event.Path.extension().string();
        for (auto& c : ext)
            c = (char)std::tolower((unsigned char)c);
        const bool isShaderSource =
            (ext == ".glsl" || ext == ".hlsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".geom");

        if (isShaderSource)
        {
            std::shared_lock<std::shared_mutex> lk(m_LoadedAssetsMutex);
            std::error_code ec;
            for (const auto& [aguid, entry] : m_LoadedAssets)
            {
                const auto& a = entry.Instance;
                if (!a)
                    continue;
                auto* prog = dynamic_cast<ShaderProgramAsset*>(a.get());
                if (!prog)
                    continue;

                const auto& doc = prog->GetDocument();
                const std::filesystem::path progDir = prog->GetPath().parent_path();
                for (const auto& kv : doc.stages)
                {
                    const std::filesystem::path srcRel(kv.second.source);
                    const std::filesystem::path srcAbs = srcRel.is_absolute() ? srcRel : (progDir / srcRel);
                    if (std::filesystem::equivalent(srcAbs, event.Path, ec) || srcAbs == event.Path)
                    {
                        dependentShaderPrograms.push_back(aguid);
                        break;
                    }
                }
            }
        }
    }

    // Marked, not performed: a reload runs on the main thread, so it is never done on the
    // thread that reported the change — the file watcher's for an external edit, the
    // writer's own for a save, where performing it here would import inside the save.
    if (m_HotReloadEnabled && (!guid.IsNull() || !dependentShaderPrograms.empty()))
    {
        std::lock_guard<std::mutex> rl(m_ReloadMutex);
        if (!guid.IsNull())
        {
            m_PendingReloads.insert(guid);
        }
        for (const auto& g : dependentShaderPrograms)
        {
            m_PendingReloads.insert(g);
        }
    }
}

void AssetManager::ProcessCompletedTasks()
{
    std::lock_guard<std::mutex> lock(m_PendingTasksMutex);

    // Clean up completed futures to prevent memory leaks
    auto it = m_PendingTasks.begin();
    while (it != m_PendingTasks.end())
    {
        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                it->get(); // Get result to handle any exceptions
            }
            catch (const std::exception& e)
            {
                Logger::Log::Error("Async task failed: {}", e.what());
            }
            it = m_PendingTasks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void AssetManager::CheckForReloads()
{
    struct ReloadCandidate
    {
        GUID Guid;
        SharedPtr<Asset> AssetPtr;
        const char* Reason = "watch";
    };

    // Ensure the thread-local AssetManager context is set so that asset Load()
    // implementations (e.g. UIStyleAsset CSS @import resolution) can reach this
    // manager via AssetManager::GetThreadCurrent().
    ScopedThreadAssetManager ctx(this);

    // Event dispatches are pure notifications: subscribers commonly re-enter
    // LoadAsset to schedule fresh loads for invalidated textures / materials
    // (see RenderServices::InstallAssetReloadInvalidators). Collect first,
    // then fire after all reload work completes.
    std::vector<AssetEvent> pendingEvents;

    // Adopt async-reload payloads decoded since the last update BEFORE
    // draining new marks: a mark for the same GUID processed below then sees
    // the finished pipeline's entry already gone and starts a fresh one.
    DrainCompletedReloads(pendingEvents);

    // Drain pending reloads posted by FileWatcher (main thread)
    std::vector<GUID> toReload;
    {
        std::lock_guard<std::mutex> lk(m_ReloadMutex);
        if (!m_PendingReloads.empty())
        {
            toReload.reserve(m_PendingReloads.size());
            for (const auto& g : m_PendingReloads)
                toReload.push_back(g);
            m_PendingReloads.clear();
        }
    }

    // Stale-generation guard: a change event for a GUID whose load is still
    // in flight must not be dropped — the in-flight read/decode may already
    // carry the pre-change bytes. Keep the mark pending until the load
    // resolves, then reload in place with the fresh bytes. Checked BEFORE the
    // loaded map below: a load that completes between the two checks is then
    // found loaded and reloads this frame; the reverse order would drop it.
    if (!toReload.empty())
    {
        std::vector<GUID> deferred;
        {
            std::lock_guard<std::mutex> lk(m_InFlightMutex);
            for (const auto& guid : toReload)
            {
                if (m_InFlight.find(guid) != m_InFlight.end())
                    deferred.push_back(guid);
            }
        }
        if (!deferred.empty())
        {
            {
                std::lock_guard<std::mutex> rl(m_ReloadMutex);
                for (const auto& guid : deferred)
                    m_PendingReloads.insert(guid);
            }
            std::unordered_set<GUID> deferredSet(deferred.begin(), deferred.end());
            toReload.erase(std::remove_if(toReload.begin(), toReload.end(),
                                          [&deferredSet](const GUID& g)
                                          { return deferredSet.find(g) != deferredSet.end(); }),
                           toReload.end());
        }
    }

    std::vector<GUID> timestampCandidates;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_LastTimestampReloadCheck >= kTimestampReloadCheckInterval)
    {
        m_LastTimestampReloadCheck = now;

        if (m_TimestampReloadScanIndex >= m_TimestampReloadScanList.size())
        {
            m_TimestampReloadScanList.clear();
            m_TimestampReloadScanIndex = 0;

            std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
            m_TimestampReloadScanList.reserve(m_LoadedAssets.size());
            for (const auto& [guid, entry] : m_LoadedAssets)
            {
                const auto& asset = entry.Instance;
                if (asset)
                    m_TimestampReloadScanList.push_back(guid);
            }
        }

        const size_t remaining = m_TimestampReloadScanList.size() - m_TimestampReloadScanIndex;
        const size_t batchSize = std::min(kMaxTimestampReloadChecksPerUpdate, remaining);
        timestampCandidates.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            timestampCandidates.push_back(m_TimestampReloadScanList[m_TimestampReloadScanIndex++]);
        }
    }

    // Snapshot assets under the map lock, then run NeedsReload()/Reload() with
    // no AssetManager lock held. Those paths touch the filesystem and execute
    // asset-specific code, so keeping them outside the lock prevents the hot
    // reload poll from blocking normal asset lookups and callback re-entry.
    std::vector<ReloadCandidate> reloadCandidates;
    std::unordered_set<GUID> scheduled;
    scheduled.reserve(toReload.size() + timestampCandidates.size());
    {
        std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);

        for (const auto& guid : toReload)
        {
            auto it = m_LoadedAssets.find(guid);
            if (it != m_LoadedAssets.end() && it->second.Instance && scheduled.insert(guid).second)
                reloadCandidates.push_back({guid, it->second.Instance, "watch"});
        }

        std::vector<ReloadCandidate> needsReloadChecks;
        needsReloadChecks.reserve(timestampCandidates.size());
        for (const auto& guid : timestampCandidates)
        {
            if (scheduled.find(guid) != scheduled.end())
                continue;

            auto it = m_LoadedAssets.find(guid);
            if (it != m_LoadedAssets.end() && it->second.Instance)
                needsReloadChecks.push_back({guid, it->second.Instance, "timestamp"});
        }

        lock.unlock();

        for (auto& candidate : needsReloadChecks)
        {
            if (candidate.AssetPtr->NeedsReload() && scheduled.insert(candidate.Guid).second)
                reloadCandidates.push_back(std::move(candidate));
        }
    }

    for (const auto& candidate : reloadCandidates)
    {
        const auto& guid = candidate.Guid;
        const auto& asset = candidate.AssetPtr;
        const bool watchReason = std::string_view(candidate.Reason) == "watch";

        // Assets that support it reload through the async pipeline: read +
        // decode on workers, in-place payload adopt on the main thread (next
        // DrainCompletedReloads). Everything else keeps the inline path.
        if (m_IOService && asset->SupportsAsyncReload())
        {
            StartAsyncReload(guid, asset, watchReason);
            continue;
        }

        Logger::Log::Info("{} asset: {}", watchReason ? "Hot-reloading" : "Auto-reloading",
                          asset->GetPath().string());
        // Reload() runs inline on this (main) thread: file re-read + decode + GPU
        // re-upload all land in this frame. One log line per reload event so the
        // stall each reload causes is visible in the editor log.
        const auto reloadStart = std::chrono::steady_clock::now();
        const ReloadOutcome outcome = asset->Reload();
        const double reloadMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reloadStart).count();
        ReportInlineReloadOutcome(outcome, *asset, guid, reloadMs, pendingEvents);
    }

    // Lock released. Safe for handlers to re-enter LoadAsset etc.
    for (auto& evt : pendingEvents)
        DispatchWithRedirectSources(std::move(evt));
}

void AssetManager::DispatchWithRedirectSources(AssetEvent event)
{
    // GetAsset resolves redirects, so a consumer may hold an asset under a redirect source while
    // every event names it by the GUID it resolved to. The sources ride the event so an
    // invalidator reaches them without knowing redirects exist. Empty, at the cost of one atomic
    // load, in a project without redirects.
    event.RedirectSources = m_Registry.FindRedirectsTo(event.AssetGuid);
    m_EventDispatcher.DispatchEvent(event);
}

void AssetManager::StartAsyncReload(const GUID& guid, const SharedPtr<Asset>& asset, bool watchReason,
                                    bool restartSameMtime)
{
    AssetMetadata metadata{};
    if (!m_Registry.TryGetAssetMetadata(guid, metadata) || metadata.Path.empty())
    {
        // Registry no longer resolves the GUID (rename/eject race): stay on
        // the inline path's source of truth, the asset's own recorded path.
        metadata = AssetMetadata(guid, asset->GetType(), asset->GetPath());
    }

    const auto currentMtime = asset->GetLastModified();
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> supersededFlag;
    uint64 generation = 0;
    {
        std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
        auto [it, inserted] = m_ActiveReloads.try_emplace(guid);
        if (!inserted)
        {
            // A reload is already in flight. Until its swap lands (which is
            // what refreshes the asset's recorded mtime), the timestamp poll
            // re-notices the same content version every tick — restarting on
            // those marks would cancel the in-flight decode forever. Only
            // genuinely newer bytes supersede a watch/timestamp mark. Explicit
            // RequestAsyncReload (inspector kv, cook settings) must restart
            // even when the file mtime is unchanged.
            if (!restartSameMtime && currentMtime <= it->second.SubmittedMtime)
                return;
            supersededFlag = it->second.CancelRequested;
        }
        it->second.Generation++;
        it->second.CancelRequested = cancelFlag;
        it->second.SubmittedMtime = currentMtime;
        generation = it->second.Generation;
    }
    if (supersededFlag)
    {
        // Drop/restart: defuse the superseded generation (a claimed read or
        // running decode discards its result), then withdraw its queued read.
        // CancelRequest may fire that request's OnFailure inline — no locks held.
        supersededFlag->store(true, std::memory_order_release);
        m_IOService->CancelRequest(guid, supersededFlag);
    }

    Logger::Log::Info("{} asset (async): {}", watchReason ? "Hot-reloading" : "Auto-reloading",
                      asset->GetPath().string());

    m_ActiveReloadJobCount.fetch_add(1, std::memory_order_acq_rel);
    const auto submitTime = std::chrono::steady_clock::now();

    AssetIOService::ReadRequest request;
    request.AssetGuid = guid;
    request.Metadata = metadata;
    request.Metadata.Path = m_ParserRegistry.ResolveReadPath(metadata, *this);
    request.Priority = AssetLoadPriority::High; // the user just saved this file
    request.CancelRequested = cancelFlag;
    request.ProcessData = [this, guid, metadata, cancelFlag, generation,
                           submitTime](Vector<uint8> data) -> SharedPtr<Asset>
    {
        SharedPtr<Asset> staged;
        if (!cancelFlag->load(std::memory_order_acquire))
        {
            staged = DecodeAssetPayload(*this, guid, metadata, data, cancelFlag);
        }
        const double offThreadMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitTime).count();
        if (!cancelFlag->load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lk(m_CompletedReloadMutex);
            m_CompletedReloads.push_back(
                {guid, generation, staged, staged ? String{} : String("decode failed"), offThreadMs});
        }
        FinishReloadJob();
        return staged;
    };
    request.OnFailure = [this, guid, cancelFlag, generation, submitTime](const String& error)
    {
        if (!cancelFlag->load(std::memory_order_acquire))
        {
            const double offThreadMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitTime).count();
            std::lock_guard<std::mutex> lk(m_CompletedReloadMutex);
            m_CompletedReloads.push_back({guid, generation, nullptr, error, offThreadMs});
        }
        FinishReloadJob();
    };
    m_IOService->SubmitRead(std::move(request));
}

bool AssetManager::RequestAsyncReload(const GUID& guid)
{
    // A first load still in flight may already have read the settings this request is
    // about, and there is nothing resident to reload yet, so the request waits for the load
    // (StartReloadsRequestedDuringLoad). Checked before the loaded map: a load registers its
    // asset before it leaves the in-flight table, so one that completes between the two
    // checks is found loaded below.
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        if (m_InFlight.find(guid) != m_InFlight.end())
        {
            m_ReloadsRequestedDuringLoad.insert(guid);
            return true;
        }
    }
    SharedPtr<Asset> asset = GetLoadedAsset(guid);
    if (!asset)
        return false;
    StartAsyncReload(guid, asset, /*watchReason=*/false, /*restartSameMtime=*/true);
    return true;
}

void AssetManager::StartReloadsRequestedDuringLoad()
{
    std::vector<GUID> landed;
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        for (auto it = m_ReloadsRequestedDuringLoad.begin(); it != m_ReloadsRequestedDuringLoad.end();)
        {
            if (m_InFlight.find(*it) != m_InFlight.end())
            {
                ++it;
                continue;
            }
            landed.push_back(*it);
            it = m_ReloadsRequestedDuringLoad.erase(it);
        }
    }
    // A load that failed or was cancelled left nothing resident; its request goes with it.
    for (const GUID& guid : landed)
    {
        if (SharedPtr<Asset> asset = GetLoadedAsset(guid))
            StartAsyncReload(guid, asset, /*watchReason=*/false, /*restartSameMtime=*/true);
    }
}

bool AssetManager::IsReloadInFlight(const GUID& guid) const
{
    if (guid.IsNull())
        return false;
    std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
    return m_ActiveReloads.find(guid) != m_ActiveReloads.end();
}

ExpectedAssetWrite AssetManager::ExpectWrite(const std::filesystem::path& file)
{
    return ExpectedAssetWrite(*this, m_ExpectedWrites.Expect(file, /*subtree=*/false));
}

ExpectedAssetWrite AssetManager::ExpectWritesUnder(const std::filesystem::path& root)
{
    return ExpectedAssetWrite(*this, m_ExpectedWrites.Expect(root, /*subtree=*/true));
}

void AssetManager::ReportExpectedWrite(const std::filesystem::path& written)
{
    if (written.empty())
        return;

    // Recorded before the drive, not after: on a watched host the watcher's report of
    // the same write can arrive while this one is still running.
    m_ExpectedWrites.Reported(written);

    // Created vs Modified is decided by what is on disk now rather than by what
    // the caller believes: a save that replaced a file and a save that created
    // one reach here identically, and ApplyFileChange treats the two differently
    // (register a new asset vs refresh an existing one).
    std::error_code ec;
    const bool known = !m_Registry.GetAssetGUID(written).IsNull();

    FileChangeEvent event;
    event.Path = written;
    event.Type = known ? FileChangeType::Modified : FileChangeType::Created;
    event.Timestamp = std::filesystem::last_write_time(written, ec);
    if (ec)
        event.Timestamp = std::filesystem::file_time_type::clock::now();

    ApplyFileChange(event);
}

void AssetManager::RetireExpectedWrite(ExpectedWriteLedger::Registration id)
{
    m_ExpectedWrites.Retire(id);
}

void AssetManager::DrainCompletedReloads(std::vector<AssetEvent>& pendingEvents)
{
    std::vector<CompletedReloadEntry> completed;
    {
        std::lock_guard<std::mutex> lk(m_CompletedReloadMutex);
        completed.swap(m_CompletedReloads);
    }

    for (auto& done : completed)
    {
        // E5: the identity may have been re-keyed while the reload was in flight.
        // A completion posted under a GUID that a new file has since claimed
        // resolves to that primary and is dropped below (see OnGuidRemapped).
        const GUID guid = m_Registry.ResolveSessionAlias(done.Guid);
        {
            std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
            auto it = m_ActiveReloads.find(guid);
            if (it == m_ActiveReloads.end() || it->second.Generation != done.Generation)
                continue; // superseded by a newer change — its pipeline owns the entry
            m_ActiveReloads.erase(it);
        }

        SharedPtr<Asset> target = GetLoadedAsset(guid);
        if (!target)
            continue; // unloaded/ejected while the reload was in flight

        if (!done.Staged)
        {
            Logger::Log::Error(
                "[AssetReload] '{}' reload FAILED off-thread after {:.2f} ms — keeping previous payload ({})",
                target->GetPath().string(), done.OffThreadMs,
                done.Error.empty() ? String("decode failed") : done.Error);
            pendingEvents.push_back(
                AssetEvents::AssetLoadFailed(guid, target->GetType(), target->GetPath().string(), "Reload failed"));
            continue;
        }

        const auto swapStart = std::chrono::steady_clock::now();
        const bool adopted = target->AdoptReload(*done.Staged);
        const double swapMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapStart).count();
        if (!adopted)
        {
            // The staged instance was not adoptable (factory produced a
            // different concrete type than the live asset). Fall back to the
            // inline path so the change still lands.
            const auto reloadStart = std::chrono::steady_clock::now();
            const ReloadOutcome outcome = target->Reload();
            const double reloadMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reloadStart).count();
            ReportInlineReloadOutcome(outcome, *target, guid, reloadMs, pendingEvents);
            continue;
        }

        Logger::Log::Info(
            "[AssetReload] '{}' reloaded in {:.2f} ms on main thread (async swap; read+decode {:.2f} ms off-thread)",
            target->GetPath().string(), swapMs, done.OffThreadMs);
        pendingEvents.push_back(
            AssetEvents::AssetReloaded(guid, target->GetType(), target->GetPath().string()));
    }
}

void AssetManager::FinishReloadJob()
{
    if (m_ActiveReloadJobCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        std::lock_guard<std::mutex> lk(m_ReloadDrainMutex);
        m_ReloadDrainCv.notify_all();
    }
}

// Helper methods for async loading (IMPROVED - no redundant GUID parameter)
std::future<SharedPtr<Asset>> AssetManager::LoadAssetSynchronous(const AssetMetadata& metadata)
{
    std::promise<SharedPtr<Asset>> promise;

    try
    {
        // Establish AssetManager thread-local context so asset PostLoad /
        // Load implementations (humanoid auto-import, UI @import resolution)
        // can reach this manager via AssetManager::GetThreadCurrent().
        ScopedThreadAssetManager ctx(this);

        SharedPtr<Asset> asset = CreateAsset(metadata);
        if (!asset)
        {
            throw std::runtime_error("Failed to create asset instance");
        }

        if (!asset->Load())
        {
            throw std::runtime_error("Failed to load asset");
        }

        asset->PostLoad();

        const bool transient = IsInFlightLoadTransient(metadata.Guid);
        {
            const GUID key = m_Registry.ResolveSessionAlias(metadata.Guid);
            std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
            m_LoadedAssets.insert_or_assign(key, LoadedAssetEntry{asset, transient});
        }

        Logger::Log::Debug("Loaded asset synchronously: {}", metadata.Name);

        // Dispatch success event
        m_EventDispatcher.DispatchEvent(AssetEvents::AssetLoaded(metadata.Guid, metadata.Type, metadata.Path.string()));

        promise.set_value(asset);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Synchronous asset loading failed for {}: {}", metadata.Name, e.what());

        // Dispatch failure event
        m_EventDispatcher.DispatchEvent(AssetEvents::AssetLoadFailed(metadata.Guid, metadata.Type, metadata.Path.string(), e.what()));

        promise.set_value(nullptr);
    }

    return promise.get_future();
}

std::future<Vector<SharedPtr<Asset>>> AssetManager::LoadAssetsSequential(const Vector<GUID>& guids, AssetLoadPriority priority)
{
    std::promise<Vector<SharedPtr<Asset>>> promise;
    Vector<SharedPtr<Asset>> results;
    results.reserve(guids.size());

    for (const auto& guid : guids)
    {
        auto future = LoadAssetAsync(guid, priority);
        results.push_back(future.get());
    }

    promise.set_value(std::move(results));
    return promise.get_future();
}

void AssetManager::RegisterLoadedAsset(const GUID& guid, SharedPtr<Asset> asset)
{
    // E5: a decode pipeline launched before a remap registers with the GUID
    // it captured; resolve so the asset lands under the current identity.
    // Once a new file has claimed the captured GUID this stores the old
    // document under the re-claimed primary (see OnGuidRemapped).
    const GUID key = m_Registry.ResolveSessionAlias(guid);
    // Residency is decided before the asset becomes visible; after this it can
    // only be pinned (a pinned request that joins the load later is pinned by
    // the load's completion).
    const bool transient = IsInFlightLoadTransient(guid);
    {
        AssetDbProfiler::LockScope profileLock(AssetDbProfiler::IsOnMainThread()
            ? AssetDbProfiler::Bucket::LockLoadedAssetsMain
            : AssetDbProfiler::Bucket::LockLoadedAssetsWorker);
        std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
        profileLock.OnAcquired();
        m_LoadedAssets.insert_or_assign(key, LoadedAssetEntry{asset, transient});
    }

    // Update memory statistics incrementally (lock-free)
    UpdateMemoryStatsIncremental(asset, true);
}

void AssetManager::UnregisterLoadedAsset(const GUID& guid)
{
    SharedPtr<Asset> removedAsset;

    {
        std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
        auto it = m_LoadedAssets.find(guid);
        if (it != m_LoadedAssets.end())
        {
            removedAsset = it->second.Instance;
            removedAsset->Unload();
            m_LoadedAssets.erase(it);
        }
    }

    // Update memory statistics incrementally (lock-free)
    if (removedAsset)
    {
        UpdateMemoryStatsIncremental(removedAsset, false);
    }
}

// ========================================
// HELPER METHOD IMPLEMENTATIONS
// ========================================

AssetFuture AssetManager::CreateEnhancedFuture(std::shared_future<SharedPtr<Asset>> future, const GUID& assetGuid)
{
    return AssetFuture(std::move(future), &m_EventDispatcher, assetGuid);
}

bool AssetManager::RenameAssetPath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    const GUID guid = m_Registry.GetAssetGUID(oldPath);
    if (!m_Registry.TryRenameAssetPath(oldPath, newPath))
        return false;
    // The instance carries the registry's normalized form of the path, as it did
    // when it was loaded.
    AssetMetadata metadata;
    if (SharedPtr<Asset> asset = GetLoadedAsset(guid); asset && m_Registry.TryGetAssetMetadata(guid, metadata))
        asset->SetPath(metadata.Path);
    return true;
}

ReloadOutcome AssetManager::ReloadAssetNow(const GUID& guid)
{
    if (guid.IsNull())
        return ReloadOutcome::Failed;

    SharedPtr<Asset> asset;
    {
        std::shared_lock<std::shared_mutex> lk(m_LoadedAssetsMutex);
        auto it = m_LoadedAssets.find(guid);
        if (it == m_LoadedAssets.end())
            return ReloadOutcome::Failed;
        asset = it->second.Instance;
    }

    if (!asset)
        return ReloadOutcome::Failed;

    ScopedThreadAssetManager ctx(this);
    return asset->Reload();
}

AssetLoadHandle AssetManager::CreateLoadHandle(const GUID& assetGuid, AssetLoadResultCallback callback, std::shared_future<SharedPtr<Asset>> future)
{
    AssetLoadHandle handle(assetGuid);
    handle.Future = std::move(future);

    if (callback)
    {
        // Fire the callback as a CONTINUATION on load completion, never from a
        // pool task that parks on the decode future. A task blocked on fut.get()
        // here occupies a pool worker while the decode that fulfills that future
        // is queued on the SAME pool; enough concurrent callback-loads then park
        // every worker and the decodes starve -- the deadlock that wedged
        // ElvenRealm/Demo_unity.scene on a cold-cache open (fresh appdata ->
        // thumbnail/FBX import storm flooding LoadAsset(guid, callback)).
        //
        // Register on the in-flight entry so the completion path
        // (EraseInFlightLoadGeneration -> FirePendingLoadCallbacks) invokes it.
        // If there is no in-flight entry the load already finished (cache/fast
        // path, suppressed, or the no-job-system synchronous path erased it), so
        // resolve immediately on the calling thread.
        std::unique_lock<std::mutex> lock(m_InFlightMutex);
        auto it = m_InFlight.find(assetGuid);
        if (it != m_InFlight.end())
        {
            it->second.Callbacks.push_back(PendingLoadCallback{std::move(callback), handle.Cancelled});
        }
        else
        {
            lock.unlock(); // never invoke user code while holding m_InFlightMutex
            // Prefer the handle's own future once it has settled: the handle
            // and its callback then report the same asset by construction,
            // with no second map lookup a concurrent unload could make
            // disagree. The map read remains the fallback for the case this
            // branch exists to serve -- an in-flight entry that retired
            // before its promise was fulfilled.
            SharedPtr<Asset> asset = handle.IsComplete() ? handle.GetResult() : GetLoadedAsset(assetGuid);
            callback(asset ? Result<SharedPtr<Asset>, AssetError>(std::move(asset))
                           : Result<SharedPtr<Asset>, AssetError>(AssetError::ImportFailed));
        }
    }

    return handle;
}

void AssetManager::UpdateMemoryStatsCache() const
{
    // Double-checked locking pattern for cache updates
    if (!m_StatsNeedUpdate.load(std::memory_order_acquire))
    {
        return; // Another thread already updated
    }

    std::lock_guard<std::mutex> lock(m_StatsCacheMutex);

    // Check again after acquiring lock
    if (!m_StatsNeedUpdate.load(std::memory_order_relaxed))
    {
        return;
    }

    // Force recalculation (this will update the cache)
    RecalculateMemoryStats();
}

void AssetManager::UpdateMemoryStatsIncremental(const SharedPtr<Asset>& asset, bool added) const
{
    if (!asset)
        return;

    uint64 assetMemory = asset->GetMemoryUsage();
    AssetType assetType = asset->GetType();

    if (added)
    {
        // Asset was added
        m_CachedAssetCount.fetch_add(1, std::memory_order_relaxed);
        m_CachedTotalMemory.fetch_add(assetMemory, std::memory_order_relaxed);

        // Update type-specific counter
        std::lock_guard<std::mutex> lock(m_StatsCacheMutex);
        if (m_CachedMemoryByType.find(assetType) == m_CachedMemoryByType.end())
        {
            m_CachedMemoryByType[assetType].store(assetMemory, std::memory_order_relaxed);
        }
        else
        {
            m_CachedMemoryByType[assetType].fetch_add(assetMemory, std::memory_order_relaxed);
        }
    }
    else
    {
        // Asset was removed
        m_CachedAssetCount.fetch_sub(1, std::memory_order_relaxed);
        m_CachedTotalMemory.fetch_sub(assetMemory, std::memory_order_relaxed);

        // Update type-specific counter
        std::lock_guard<std::mutex> lock(m_StatsCacheMutex);
        auto it = m_CachedMemoryByType.find(assetType);
        if (it != m_CachedMemoryByType.end())
        {
            it->second.fetch_sub(assetMemory, std::memory_order_relaxed);
        }
    }
}

SharedPtr<Asset> AssetManager::GetLoadedAsset(const GUID& guid, bool pinResidency) const
{
    std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
    auto it = m_LoadedAssets.find(guid);
    if (it == m_LoadedAssets.end())
        return nullptr;
    // Promotion must precede any concurrent transient eviction. Do not keep
    // this lock across handle construction or user callbacks.
    if (pinResidency)
        it->second.Pin();
    return it->second.Instance;
}

// ========================================
// IN-FLIGHT TRACKING HELPERS
// ========================================

void AssetManager::SubmitInFlightLoad(const AssetMetadata& metadata, AssetLoadPriority priority,
                                      std::shared_ptr<std::promise<SharedPtr<Asset>>> promise,
                                      std::shared_ptr<std::atomic<bool>> completed,
                                      std::shared_ptr<std::atomic<bool>> cancelRequested)
{
    const GUID guid = metadata.Guid;

    // The entry's CancelRequested flag is both the pre-submission early-out
    // (checked first thing by the coordinator) and the suppression
    // discriminator in the completion callbacks: a cancelled load is a
    // withdrawn request, not a failure, and must not poison
    // m_SuppressedLoads.
    //
    // Exactly one of the two callbacks fires per pipeline (coordinator
    // contract), including read failures, decode-job cancellation at pool
    // shutdown, and AssetIOService teardown — so the promise cannot strand.
    // In both callbacks the promise resolution is deliberately LAST:
    // AssetManager::Shutdown waits on the in-flight futures as its
    // no-decode-touches-a-dead-manager guarantee, so a resolved future must
    // mean the callback is done with manager state.
    m_BatchCoordinator->LoadAsset(
        metadata, priority, cancelRequested,
        [this, guid, promise, completed, cancelRequested](const JobSystem::TaskHandle& completedHandle)
        {
            SharedPtr<Asset> asset;
            const bool got = completedHandle.TryGetResult(asset);
            if ((!got || !asset) && !cancelRequested->load(std::memory_order_acquire))
            {
                MarkLoadFailed(guid);
            }
            ErasedInFlightLoad erased = EraseInFlightLoadGeneration(guid, completed);
            // A pinned request that joined after registration pins the asset
            // before any requester receives it.
            if (got && asset && !erased.Transient)
                PinResidency(guid, asset);
            const bool setNow = (completed ? !completed->exchange(true) : true);
            if (setNow && promise)
                promise->set_value(got ? asset : nullptr);
            // Continuation: notify LoadAsset(guid, callback) waiters (never from a
            // parked pool worker). Fired AFTER set_value: a downstream callback
            // that (transitively) blocks on this asset's OWN future would wedge
            // this completion thread if fired before resolution.
            // FirePendingLoadCallbacks is static (touches no manager
            // state), so the "resolved future => manager done" invariant still holds.
            FirePendingLoadCallbacks(erased.Callbacks, got ? asset : SharedPtr<Asset>{});
        },
        [this, guid, promise, completed, cancelRequested](const String& error)
        {
            if (!cancelRequested->load(std::memory_order_acquire))
            {
                if (error.empty())
                    MarkLoadFailed(guid);
                else
                    MarkLoadSuppressed(guid, error);
            }
            ErasedInFlightLoad erased = EraseInFlightLoadGeneration(guid, completed);
            const bool setNow = (completed ? !completed->exchange(true) : true);
            if (setNow && promise)
                promise->set_value(nullptr);
            FirePendingLoadCallbacks(erased.Callbacks, SharedPtr<Asset>{}); // fire after set_value (see success path)
        });
}

AssetManager::ErasedInFlightLoad AssetManager::EraseInFlightLoadGeneration(
    const GUID& guid, const std::shared_ptr<std::atomic<bool>>& completed)
{
    ErasedInFlightLoad erased;
    std::lock_guard<std::mutex> lock(m_InFlightMutex);
    auto it = m_InFlight.find(guid);
    if (it == m_InFlight.end() || it->second.Completed != completed)
    {
        // A remap can move the entry, and a new file can reclaim its old GUID.
        // The generation token stays with the load across both identity changes.
        it = completed ? std::find_if(m_InFlight.begin(), m_InFlight.end(),
                                     [&completed](const auto& entry)
                                     { return entry.second.Completed == completed; })
                       : m_InFlight.end();
    }
    if (it != m_InFlight.end())
    {
        // Return callbacks to the caller for delivery after the promise settles,
        // outside this lock. A replacement generation keeps its own callbacks.
        erased.Callbacks = std::move(it->second.Callbacks);
        erased.Transient = it->second.Transient;
        m_InFlight.erase(it);
    }
    return erased;
}

bool AssetManager::IsInFlightLoadTransient(const GUID& guid) const
{
    std::lock_guard<std::mutex> lock(m_InFlightMutex);
    const auto it = m_InFlight.find(guid);
    return it != m_InFlight.end() && it->second.Transient;
}

void AssetManager::PinResidency(const GUID& guid, const SharedPtr<Asset>& asset)
{
    const GUID key = m_Registry.ResolveSessionAlias(guid);
    std::shared_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
    const auto it = m_LoadedAssets.find(key);
    // A late completion must not promote a replacement loaded under this GUID.
    if (it != m_LoadedAssets.end() && it->second.Instance == asset)
        it->second.Pin();
}

void AssetManager::FirePendingLoadCallbacks(std::vector<PendingLoadCallback>& pending, const SharedPtr<Asset>& asset)
{
    for (auto& pc : pending)
    {
        if (!pc.Callback)
            continue;
        // Skip callbacks whose request was withdrawn with AssetLoadHandle::Cancel().
        if (!pc.Cancelled || pc.Cancelled->load(std::memory_order_seq_cst))
            continue;
        if (asset)
            pc.Callback(Result<SharedPtr<Asset>, AssetError>(asset));
        else
            pc.Callback(Result<SharedPtr<Asset>, AssetError>(AssetError::ImportFailed));
    }
}

void AssetManager::ResolveAndEraseInFlight(const GUID& guid, const SharedPtr<Asset>& result)
{
    std::shared_ptr<std::promise<SharedPtr<Asset>>> promiseToResolve;
    std::vector<PendingLoadCallback> callbacks;
    bool transient = false;
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        auto it = m_InFlight.find(guid);
        if (it != m_InFlight.end())
        {
            bool setNow = true;
            if (it->second.Completed)
                setNow = !it->second.Completed->exchange(true);
            if (setNow)
                promiseToResolve = it->second.Promise;
            callbacks = std::move(it->second.Callbacks);
            transient = it->second.Transient;
            m_InFlight.erase(it);
        }
    }
    if (result && !transient)
        PinResidency(guid, result);
    // Resolve first, then fire (fire-after): a downstream callback that blocks on
    // this asset's own future must not wedge us before resolution. Firing is
    // static (touches no manager state), so the shutdown invariant holds.
    if (promiseToResolve)
        promiseToResolve->set_value(result);
    FirePendingLoadCallbacks(callbacks, result);
}

void AssetManager::SetInFlightDecodeHandle(const GUID& guid, const JobSystem::TaskHandle& handle)
{
    // E5: follow a remap that re-keyed this load's entry. Resolved before
    // taking the in-flight lock so the registry lock never nests inside it.
    const GUID translated = m_Registry.ResolveSessionAlias(guid);
    std::lock_guard<std::mutex> lock(m_InFlightMutex);
    auto it = m_InFlight.find(guid);
    if (it == m_InFlight.end() && translated != guid)
        it = m_InFlight.find(translated);
    if (it != m_InFlight.end())
    {
        it->second.DecodeHandle = handle;
    }
}

void AssetManager::OnGuidRemapped(const GUID& oldGuid, const GUID& newGuid, const std::filesystem::path& path)
{
    if (oldGuid.IsNull() || newGuid.IsNull() || oldGuid == newGuid)
        return;

    // Re-key the loaded-asset map under its own lock.
    {
        std::unique_lock<std::shared_mutex> lock(m_LoadedAssetsMutex);
        auto it = m_LoadedAssets.find(oldGuid);
        if (it != m_LoadedAssets.end())
        {
            LoadedAssetEntry asset = std::move(it->second);
            m_LoadedAssets.erase(it);
            m_LoadedAssets[newGuid] = std::move(asset);
        }
    }

    // Re-key the in-flight entry when the new key is free; if a fresh load
    // already started under the new GUID, the old entry stays and erases
    // itself through the completion callback's generation match. A reload kept
    // for the load follows the identity either way: the load registers its
    // asset under the new GUID.
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        auto it = m_InFlight.find(oldGuid);
        if (it != m_InFlight.end() && m_InFlight.find(newGuid) == m_InFlight.end())
        {
            InFlightEntry entry = std::move(it->second);
            m_InFlight.erase(it);
            m_InFlight.emplace(newGuid, std::move(entry));
        }
        if (m_ReloadsRequestedDuringLoad.erase(oldGuid) > 0)
            m_ReloadsRequestedDuringLoad.insert(newGuid);
    }

    // Pending reloads + suppression follow the identity.
    {
        std::lock_guard<std::mutex> lk(m_ReloadMutex);
        if (m_PendingReloads.erase(oldGuid) > 0)
            m_PendingReloads.insert(newGuid);
    }
    // Active async reloads too (completions posted under the old GUID reach
    // the re-keyed entry through ResolveSessionAlias at drain).
    {
        std::lock_guard<std::mutex> lk(m_ActiveReloadMutex);
        auto it = m_ActiveReloads.find(oldGuid);
        if (it != m_ActiveReloads.end() && m_ActiveReloads.find(newGuid) == m_ActiveReloads.end())
        {
            ActiveReloadEntry entry = std::move(it->second);
            m_ActiveReloads.erase(it);
            m_ActiveReloads.emplace(newGuid, std::move(entry));
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_SuppressedLoadMutex);
        auto it = m_SuppressedLoads.find(oldGuid);
        if (it != m_SuppressedLoads.end())
        {
            SuppressedLoadInfo info = std::move(it->second);
            m_SuppressedLoads.erase(it);
            m_SuppressedLoads.emplace(newGuid, std::move(info));
        }
    }

    m_EventDispatcher.DispatchEvent(AssetEvent(
        AssetEventType::GuidRemapped, newGuid, AssetType::Unknown,
        path.string(), oldGuid.ToString()));
}

size_t AssetManager::GetInFlightLoadCount() const
{
    std::lock_guard<std::mutex> lock(m_InFlightMutex);
    return m_InFlight.size();
}

bool AssetManager::CancelInFlightLoad(const GUID& guid)
{
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    JobSystem::TaskHandle decodeHandle;
    {
        std::lock_guard<std::mutex> lock(m_InFlightMutex);
        auto it = m_InFlight.find(guid);
        if (it == m_InFlight.end())
            return false;
        cancelFlag = it->second.CancelRequested;
        decodeHandle = it->second.DecodeHandle;
    }

    // Flag first: any read/decode claimed from here on defuses itself. Then
    // withdraw queued work — matched by the flag's identity, so a newer load
    // of the same GUID (fresh flag) is never withdrawn by a stale canceller.
    // Both cancels may fire the load's failure callbacks inline on this
    // thread (they acquire m_InFlightMutex), so no lock is held.
    if (cancelFlag)
        cancelFlag->store(true, std::memory_order_release);
    if (m_IOService)
        m_IOService->CancelRequest(guid, cancelFlag);
    if (decodeHandle.IsValid())
        decodeHandle.Cancel();
    return true;
}

JobSystem::TaskHandle AssetManager::UnloadAssetWithDependencies(const GUID& guid)
{
    if (!m_Initialized || !m_JobSystem || !m_BatchCoordinator)
    {
        Logger::Log::Error("AssetManager not properly initialized for unified asset unloading");
        return JobSystem::TaskHandle();
    }

    // An in-flight load of this GUID is moot once the caller decided to
    // unload: withdraw it before scheduling the unload work.
    CancelInFlightLoad(guid);

    try
    {
        // Use unified batch coordinator for unloading
        auto handle = m_BatchCoordinator->UnloadAsset(guid);

        Logger::Log::Debug("Started unified asset unloading for asset {}", guid.ToString());
        return handle;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Failed to create unified unload task for {}: {}", guid.ToString(), e.what());
        return JobSystem::TaskHandle();
    }
}

} // namespace GameEngine
