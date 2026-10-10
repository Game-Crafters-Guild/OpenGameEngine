#include "NativeScripting/NativeScriptManager.h"

#include "FileWatcher/FileWatcher.h"          // FileChangeEvent
#include "Assets/FileWatchingService.h"  // FileWatchingService, FilePattern, FileWatchSubscription
#include "ECS/ModuleRegistration.h"      // per-load registration stamp (C12)
#include "JobSystem/JobChannel.h" // async build dispatch
#include "Logger/Logger.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/CMakeInvoker.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeScriptingABI.h"
#include "NativeScripting/NativeSourceTree.h"
#include "NativeScripting/PrebuiltModuleBinaries.h"
#include "NativeScripting/ToolchainFingerprint.h"
#include "NativeScripting/UserModuleHandle.h"
#include "NativeScripting/UserProjectGenerator.h"
#include "NativeScripting/UserSystemRegistry.h" // ClearUserSystems on reload
#include "PluginAPI/EnginePlugin.h"             // registration hold + abort purge (C12)

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

namespace GameEngine
{
namespace NativeScripting
{

namespace
{
// User-edit debounce. Distinct from FileWatcher's 100ms Modified debounce: this
// waits for a burst of saves to quiesce before treating the sources as settled.
constexpr std::chrono::milliseconds kSourceDebounce{300};

// How often a WaitForModuleBuilds waiter re-checks its cancel predicate. The
// caller's cancel flag cannot notify the manager's condition variable, so the
// wait wakes on this period to look; a Tick that settles the builds wakes it
// at once.
constexpr std::chrono::milliseconds kSettleCancelCheckPeriod{100};

// Content-addressed shadow copy: copy `src` into `activeDir` under a name suffixed
// with an 8-hex FNV-1a hash of its bytes, so a rebuilt-but-identical DLL reuses the
// same file and a changed one gets a fresh name (never overwriting a loaded DLL).
// Returns the destination path, or empty on failure. A simple stand-in until the
// C11 GUID stamper / PDB content-hash naming.
std::filesystem::path ShadowCopyVersioned(const std::filesystem::path& src,
                                          const std::filesystem::path& activeDir)
{
    std::ifstream in(src, std::ios::binary);
    if (!in)
        return {};
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string hex = ToHexDigest(Fnv1aHash(bytes)).substr(0, 8);

    std::error_code ec;
    std::filesystem::create_directories(activeDir, ec);
    const std::filesystem::path dest =
        activeDir / (src.stem().generic_string() + "_" + hex + src.extension().generic_string());
    if (!std::filesystem::exists(dest, ec))
    {
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return {};
    }
    return dest;
}

// Set the result's one-line failure summary and log it. Returns false so callers can
// `return LogFail(...)` from bool paths.
bool LogFail(NativeBuildResult& result, std::string msg)
{
    result.Error = std::move(msg);
    Logger::Log::Error("[NativeScripting] {}", result.Error);
    return false;
}

// Pull the actual compiler/linker error lines out of the captured cmake build output and
// log each, so the build console shows WHY the build failed (not just "exit 1"). Matches
// MSVC ("file(line,col): error C…" / "error LNK"), clang/gcc ("file:line:col: error:"),
// and "fatal error". Capped so a pathological build can't flood the log.
void LogCompilerErrors(const std::string& buildOutput)
{
    constexpr int kMaxErrorLines = 40;
    std::istringstream stream(buildOutput);
    std::string line;
    int logged = 0;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const bool isError = line.find(": error") != std::string::npos ||
                             line.find("error C") != std::string::npos ||
                             line.find("fatal error") != std::string::npos;
        if (!isError)
            continue;
        if (logged >= kMaxErrorLines)
        {
            Logger::Log::Error("[NativeScripting]   … more errors truncated; see the project's .Cache/NativeScripts/build output");
            break;
        }
        Logger::Log::Error("[NativeScripting]   {}", line);
        ++logged;
    }
}
} // namespace

NativeScriptManager::NativeScriptManager() = default;

NativeScriptManager::~NativeScriptManager()
{
    Shutdown();
}

bool NativeScriptManager::Initialize(JobSystem::WorkStealingThreadPool* jobSystem)
{
    if (m_Initialized)
        return true;
    if (jobSystem)
    {
        m_BuildChannel = std::make_unique<JobSystem::JobChannel>(
            *jobSystem, JobSystem::JobChannelDesc{.Name = "Native module builds", .MaxRunning = 1});
    }
    {
        std::lock_guard<std::mutex> lock(m_SettleMutex);
        m_Initialized = true;
        m_ShutDown = false;
    }
    // Warm the fast-build toolchain (Ninja + cached MSVC env) as a job so it's ready
    // before the first user build. No-op without a JobSystem (builds use the default generator).
    m_Toolchain.WarmAsync(jobSystem);
    Logger::Log::Info("[NativeScripting] NativeScriptManager initialized");
    return true;
}

void NativeScriptManager::Shutdown()
{
    if (!m_Initialized)
        return;
    // Cancel + join any dispatched build before tearing down: the JobSystem is reset only
    // AFTER this manager is destroyed, so a running build still holds `this`. Tick starts a
    // build only while none is in flight, so m_BuildTask is the only job of the channel.
    m_CancelBuild.store(true, std::memory_order_relaxed);
    m_BuildTask.Wait();
    m_BuildTask = JobSystem::TaskHandle{};
    m_BuildChannel.reset();

    // Same lifetime rule for the toolchain warm job: Detect() touches m_Toolchain, so a
    // shutdown that lands before the one-time vswhere/vcvars probes finish must join it
    // here or the job dereferences a destroyed toolchain (UAF that presents as a
    // deadlocked pool join on the freed mutex).
    m_Toolchain.JoinWarm();

    m_WatchSubscription.reset(); // RAII unsubscribe
    m_PendingChange.store(false, std::memory_order_relaxed);
    m_RebuildRequested.store(false, std::memory_order_relaxed);
    m_WatchDirectory.clear();

    // The completion task of a build still in flight will never run, so a thread in
    // WaitForModuleBuilds is woken to return ShutDown, and the manager stays alive
    // until the last one has left.
    {
        std::unique_lock<std::mutex> lock(m_SettleMutex);
        m_Initialized = false;
        m_ShutDown = true;
        m_SettleCondition.notify_all();
        m_SettleCondition.wait(lock, [this] { return m_SettleWaiters == 0; });
    }
    Logger::Log::Info("[NativeScripting] NativeScriptManager shutdown");
}

void NativeScriptManager::SetWatchDirectory(const std::filesystem::path& sourceDir)
{
    m_WatchSubscription.reset();
    m_WatchDirectory = sourceDir;
    if (sourceDir.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(sourceDir, ec) || ec)
    {
        Logger::Log::Warning("[NativeScripting] watch directory does not exist: {}", sourceDir.string());
        return;
    }

    auto& service = FileWatchingService::GetInstance();
    FilePattern pattern(sourceDir, ".*", {".cpp", ".h", ".hpp", ".hxx", ".hh"}, /*recursive*/ true);
    m_WatchSubscription = MakeUnique<FileWatchSubscription>(
        service.Subscribe(pattern, [this](const FileChangeEvent& e) { OnFileChanged(e); }));
    service.StartWatching();
    Logger::Log::Info("[NativeScripting] watching native sources under {}", sourceDir.string());
}

void NativeScriptManager::OnFileChanged(const FileChangeEvent& event)
{
    if (!IsWatchedNativeSourceExtension(event.Path))
        return;
    {
        std::lock_guard<std::mutex> lock(m_DirtyMutex);
        m_PendingChange.store(true, std::memory_order_relaxed);
        m_LastChangeTime = std::chrono::steady_clock::now();
    }
    Logger::Log::Debug("[NativeScripting] native source changed: {}", event.Path.string());
}

void NativeScriptManager::Tick()
{
    if (!m_Initialized)
        return;

    // Held for the whole body: WaitForModuleBuilds observes the build state only
    // between Ticks (see m_SettleMutex). Uncontended on every frame in which no export waits.
    std::lock_guard<std::mutex> settleLock(m_SettleMutex);
    m_TickThread = std::this_thread::get_id();
    if (m_SettleWaiters > 0)
        m_SettleCondition.notify_all(); // waiters re-check after this Tick releases the lock

    // Drain completed-build results (load/register + the editor callback run here, on
    // the main thread). Cheap when empty.
    ProcessMainThreadTasks();

    // Settle debounce: coalesce a burst of saves into one rebuild request.
    if (m_PendingChange.load(std::memory_order_relaxed))
    {
        bool settled = false;
        {
            std::lock_guard<std::mutex> lock(m_DirtyMutex);
            if (m_PendingChange.load(std::memory_order_relaxed) &&
                (std::chrono::steady_clock::now() - m_LastChangeTime) >= kSourceDebounce)
            {
                m_PendingChange.store(false, std::memory_order_relaxed);
                settled = true;
            }
        }
        if (settled)
            ProcessSettledChange();
    }

    // Kick a requested rebuild once the previous build is free and the throttle window
    // since its COMPLETION has elapsed (m_LastBuildCompleteTime starts at the epoch, so
    // the first kick is immediate).
    if (m_RebuildRequested.load(std::memory_order_relaxed) && !m_BuildInFlight.load(std::memory_order_relaxed) &&
        (std::chrono::steady_clock::now() - m_LastBuildCompleteTime) >= kBuildThrottle)
    {
        m_RebuildRequested.store(false, std::memory_order_relaxed);
        KickBuild();
    }
}

bool NativeScriptManager::IsModuleBuildPendingLocked() const
{
    // A requested build's stale set is unknown until its kick, so a request counts;
    // a dispatched build counts only while it holds a shipped module (one completion
    // task applies the whole batch, so a mixed batch is waited out in full).
    return m_PendingChange.load(std::memory_order_relaxed) ||
           m_RebuildRequested.load(std::memory_order_relaxed) ||
           (m_BuildInFlight.load(std::memory_order_relaxed) && !m_ShippedModulesBuilding.empty());
}

bool NativeScriptManager::IsShippedBuildDeferredLocked() const
{
    return m_DeferredReloadPending.load(std::memory_order_relaxed) ||
           std::any_of(m_DeferredBuildLoads.begin(), m_DeferredBuildLoads.end(),
                       [](const DeferredBuildLoad& deferred) { return deferred.Shipped; });
}

void NativeScriptManager::RecordModuleOutcomeLocked(const std::string& moduleId, bool succeeded)
{
    const auto it = std::find(m_FailedShippedModules.begin(), m_FailedShippedModules.end(), moduleId);
    if (succeeded)
    {
        if (it != m_FailedShippedModules.end())
            m_FailedShippedModules.erase(it);
        return;
    }
    if (it == m_FailedShippedModules.end() && m_EditorModuleIds.count(moduleId) == 0)
        m_FailedShippedModules.push_back(moduleId);
}

NativeScriptManager::ModuleBuildWaitResult NativeScriptManager::WaitForModuleBuilds(
    const std::function<bool()>& isCancelled, const ModuleBuildWaitReport& report)
{
    ModuleBuildWaitResult result;
    std::unique_lock<std::mutex> lock(m_SettleMutex);
    if (m_TickThread == std::this_thread::get_id())
    {
        Logger::Log::Error("[NativeScripting] WaitForModuleBuilds ran on the thread that runs Tick, where "
                           "the builds it waits for can never finish; call it from a worker thread");
        result.Outcome = ModuleBuildWaitOutcome::CalledFromTickThread;
        return result;
    }
    ++m_SettleWaiters;
    std::optional<std::vector<std::string>> reportedModules;
    for (;;)
    {
        if (m_ShutDown)
        {
            result.Outcome = ModuleBuildWaitOutcome::ShutDown;
            break;
        }
        if (!IsModuleBuildPendingLocked())
        {
            if (IsShippedBuildDeferredLocked())
            {
                result.Outcome = ModuleBuildWaitOutcome::DeferredByPlayMode;
                for (const DeferredBuildLoad& deferred : m_DeferredBuildLoads)
                {
                    if (deferred.Shipped)
                        result.DeferredModules.push_back(deferred.ModuleId);
                }
                break;
            }
            result.Outcome = ModuleBuildWaitOutcome::Settled;
            result.FailedModules = m_FailedShippedModules;
            break;
        }
        const bool reportDue = !reportedModules || *reportedModules != m_ShippedModulesBuilding;
        if (reportDue)
            reportedModules = m_ShippedModulesBuilding;
        lock.unlock();
        const bool cancelled = isCancelled();
        if (!cancelled && reportDue)
            report(*reportedModules);
        lock.lock();
        if (cancelled)
        {
            result.Outcome = ModuleBuildWaitOutcome::Cancelled;
            break;
        }
        if (!reportDue)
            m_SettleCondition.wait_for(lock, kSettleCancelCheckPeriod);
    }
    --m_SettleWaiters;
    m_SettleCondition.notify_all(); // Shutdown waits for the last waiter to leave
    return result;
}

void NativeScriptManager::ProcessSettledChange()
{
    if (m_HotReloadDeferred.load(std::memory_order_relaxed))
    {
        m_DeferredReloadPending.store(true, std::memory_order_relaxed);
        Logger::Log::Info("[NativeScripting] source change deferred until play-mode exits");
        return;
    }
    RequestRebuild();
}

void NativeScriptManager::SetHotReloadDeferred(bool deferred)
{
    m_HotReloadDeferred.store(deferred, std::memory_order_relaxed);
}

bool NativeScriptManager::TriggerDeferredHotReloadIfPending()
{
    // Held for the whole flush, as Tick holds it: the record writes below must not
    // race an export that reads the records (WaitForModuleBuilds).
    std::lock_guard<std::mutex> settleLock(m_SettleMutex);
    const bool sourcePending = m_DeferredReloadPending.exchange(false, std::memory_order_relaxed);
    const bool buildPending = !m_DeferredBuildLoads.empty();

    // Apply builds that completed during play first: their DLLs are newer than the
    // last-good caches, and a source edit made during play rebuilds on top of them
    // below. This is the completion task's load path (in the same module order),
    // replayed now that play has exited.
    if (buildPending)
    {
        std::vector<DeferredBuildLoad> deferredLoads = std::move(m_DeferredBuildLoads);
        m_DeferredBuildLoads.clear();
        for (DeferredBuildLoad& deferred : deferredLoads)
        {
            Logger::Log::Info("[NativeScripting] applying module build deferred during play-mode: {}",
                              deferred.Dll.string());
            if (m_OnBeforeReload)
                m_OnBeforeReload();
            LoadModule(deferred.Dll, deferred.ModuleId, deferred.Result);
            if (deferred.Result.Registered)
            {
                WriteBuildCache(deferred.BuildDir, deferred.FullDigest, deferred.AbiDigest, deferred.Dll);
                if (m_OnAfterReload)
                    m_OnAfterReload();
            }
            RecordModuleOutcomeLocked(deferred.ModuleId, deferred.Result.Registered);
            if (m_OnBuildComplete)
                m_OnBuildComplete(deferred.Result);
        }
    }

    if (sourcePending)
    {
        Logger::Log::Info("[NativeScripting] flushing deferred native source rebuild");
        RequestRebuild();
    }
    return sourcePending || buildPending;
}

NativeBuildResult NativeScriptManager::BuildAndLoad(const NativeBuildConfig& config)
{
    NativeBuildResult result;
    if (!config.PrebuiltDir.empty() && TryLoadPrebuiltPackageModule(config, result))
        return result;
    result = NativeBuildResult{};
    const std::filesystem::path dll = BuildOnly(config, result);
    if (!dll.empty())
        LoadModule(dll, config.ModuleName, result);
    return result;
}

bool NativeScriptManager::LoadPrebuiltUserModule(const std::filesystem::path& projectRoot,
                                                 const std::string& moduleId,
                                                 const std::filesystem::path& prebuiltDir)
{
    // Shipped package binaries beat cached builds, mirroring BuildAndLoad. A
    // prebuilt-only package (P3) has no editor build-cache record at all in a
    // dev-layout run, so without this probe it loaded in the editor and in
    // packaged games but never in a dev Player.
    if (!prebuiltDir.empty())
    {
        NativeBuildConfig prebuiltConfig;
        prebuiltConfig.ModuleName = moduleId;
        prebuiltConfig.PrebuiltDir = prebuiltDir;
        NativeBuildResult prebuiltResult;
        if (TryLoadPrebuiltPackageModule(prebuiltConfig, prebuiltResult))
            return true;
    }

    if (projectRoot.empty())
        return false;

    // The editor records its last successful build here (see WriteBuildCache). The runtime
    // does not re-check the SOURCE digest for that dev cache — there are no sources to hash in
    // a shipped game, and no toolchain to rebuild with — but it does check the engine identity
    // below, because the dev cache dir can be machine-global (package modules build into the
    // shared PackageCache) and therefore written by an engine other than this one.
    std::filesystem::path buildDir = projectRoot / ".Cache" / "NativeScripts" / "build";
    auto cache = ReadBuildCacheRecord(buildDir);
    if (!cache)
    {
        // Package-module callers pass the module's writable cache dir instead
        // of a project root (git packages build outside their immutable cache
        // entry); the editor's record then lives at <root>/NativeScripts/build.
        buildDir = projectRoot / "NativeScripts" / "build";
        cache = ReadBuildCacheRecord(buildDir);
    }
    if (!cache)
    {
        // Packaged game: BuildPipeline stages the record into a flat NativeScripts/ directory
        // in the game's content root (= the project root): beside the executable on
        // Windows/Linux, with the DLL; Contents/Resources in a macOS bundle, whose dylib is in
        // Contents/Frameworks and recorded relative to it.
        buildDir = projectRoot / "NativeScripts";
        cache = ReadBuildCacheRecord(buildDir);
    }
    if (!cache)
    {
        Logger::Log::Info("[NativeScripting] no prebuilt user module for project {} (no native scripts)",
                          projectRoot.string());
        return false;
    }

    // Engine-identity validation, BEFORE the DLL is mapped. It has to happen here: a module
    // built against a different engine typically faults inside DLL_PROCESS_ATTACH, so the
    // exported AbiVersion/toolchain handshakes in LoadModule — which only run once the image
    // is mapped — never get the chance to refuse it.
    //
    // Two disjoint cases, distinguished by whether an engine_abi marker was staged:
    //  - Packaged game (marker present): the record's build-time abiDigest must equal the
    //    marker the packager wrote for the engine it ships.
    //  - Dev cache (no marker): nothing built the marker, and the runtime cannot recompute an
    //    abiDigest (no import lib, no toolchain). Compare the engine that BUILT the DLL
    //    against the engine about to load it, both read off the running image.
    const std::string expectedEngineAbi = ReadEngineAbiMarker(buildDir);
    if (!expectedEngineAbi.empty())
    {
        if (cache->AbiDigest.empty())
        {
            Logger::Log::Error("[NativeScripting] REFUSING prebuilt user module {}: its record carries no "
                               "engine-ABI digest, so compatibility with this game's engine (abi={}) cannot be "
                               "verified. Rebuild the scripts and repackage the game.",
                               cache->DllPath, expectedEngineAbi);
            return false;
        }
        if (cache->AbiDigest != expectedEngineAbi)
        {
            Logger::Log::Error("[NativeScripting] REFUSING prebuilt user module {}: it was built against a "
                               "different engine (module abi={}, shipped engine abi={}). Rebuild the scripts "
                               "and repackage the game.",
                               cache->DllPath, cache->AbiDigest, expectedEngineAbi);
            return false;
        }
    }
    else if (const std::string& hostEngineBuild = EngineBuildIdentity(); hostEngineBuild.empty())
    {
        // The platform has no linker stamp to read (see EngineBuildIdentity). Unknown is not
        // mismatch: refusing every module here would break native scripting outright, so keep
        // the pre-guard behaviour and say why the guard is inactive.
        Logger::Log::Warning("[NativeScripting] loading prebuilt user module {} UNVERIFIED: this engine build "
                             "cannot identify itself on this platform, so the engine it was built against "
                             "cannot be checked.",
                             cache->DllPath);
    }
    else if (cache->EngineBuildId.empty())
    {
        // Written before this record line existed, or by a writer that does not stamp it.
        // The cache slot is machine-global and shared by every worktree, so "unknown" is far
        // more likely to be another engine's build than this one's — refuse and rebuild.
        Logger::Log::Warning("[NativeScripting] skipping prebuilt user module {}: its build record predates "
                             "engine-identity stamping, so the engine it was built against (this one is {}) "
                             "is unknown. It will be rebuilt.",
                             cache->DllPath, hostEngineBuild);
        return false;
    }
    else if (cache->EngineBuildId != hostEngineBuild)
    {
        Logger::Log::Warning("[NativeScripting] skipping prebuilt user module {}: it was built against engine "
                             "build {}, this engine is build {}. Loading it would run its static initializers "
                             "against a different Engine binary. It will be rebuilt.",
                             cache->DllPath, cache->EngineBuildId, hostEngineBuild);
        return false;
    }

    // Resolve a relative DLL path against the project root. A game packaged by the
    // Build pipeline writes a relocatable RELATIVE path (e.g.
    // "NativeScripts/UserScripts.dll") so the package keeps working after it is
    // moved or copied to another machine; the editor's own live cache records an
    // absolute path, which passes through here unchanged.
    std::filesystem::path dllPath = cache->DllPath;
    if (dllPath.is_relative())
        dllPath = projectRoot / dllPath;
    const std::string dllPathStr = dllPath.generic_string();

    std::error_code ec;
    if (!std::filesystem::exists(dllPath, ec))
    {
        Logger::Log::Warning("[NativeScripting] prebuilt user module missing on disk: {}", dllPathStr);
        return false;
    }

    NativeBuildResult result;
    LoadModule(dllPathStr, moduleId, result);
    if (result.Registered)
        Logger::Log::Info("[NativeScripting] loaded prebuilt user module: {}", dllPathStr);
    else
        Logger::Log::Warning("[NativeScripting] failed to load prebuilt user module {}: {}", dllPathStr, result.Error);
    return result.Registered;
}

std::filesystem::path NativeScriptManager::BuildOnly(const NativeBuildConfig& config, NativeBuildResult& result)
{
    // Local copy: the toolchain step below fills NinjaExe/MsvcEnvBatch.
    NativeBuildConfig cfg = config;

    // Use the fast-build toolchain (Ninja + cached MSVC env) when it's warmed up; otherwise
    // leave these empty so CMakeInvoker falls back to the default generator. The accessors
    // are thread-safe, so this is fine on the build worker. We're already off the UI thread
    // here, so wait out the one-time async detection rather than letting the FIRST build race
    // it onto the slow VS/MSBuild generator (the warm only takes a couple seconds for vcvars).
    if (m_Toolchain.WaitUntilReady(std::chrono::seconds(20)))
    {
        cfg.NinjaExe = m_Toolchain.NinjaExe();
        cfg.MsvcEnvBatch = m_Toolchain.EnvBatch();
    }

    // Scan SourceDir for ECS::ComponentBase / ECS::SystemBase inheritors (optional) →
    // generated registration TUs written into BuildDir as *.gen.cpp, picked up by the
    // generated project's glob. Skipped when the scanner isn't configured (macros-only mode).
    const bool scannerConfigured = !cfg.DotnetExe.empty() && !cfg.ComponentScannerDll.empty();
    if (scannerConfigured)
    {
        std::string scanError;
        ComponentScanStats stats;
        const std::filesystem::path gen = CMakeInvoker::RunComponentScanner(cfg, result.Output, scanError, stats);
        if (gen.empty())
        {
            LogFail(result, scanError.empty() ? "component scan failed" : scanError);
            return {};
        }

        // Surface silent scan misses: an empty result, a conservatively-skipped struct, or
        // a macro-free component stranded in a .cpp all build green but register nothing.
        if (stats.Reflected == 0)
            Logger::Log::Warning("[NativeScripting] component scan of {} found 0 components — "
                                 "check that components inherit {} and are declared in a header",
                                 cfg.SourceDir.string(), cfg.DetectBase);
        if (stats.Skipped > 0)
            Logger::Log::Warning("[NativeScripting] component scan skipped {} struct(s) with "
                                 "non-POD/virtual/macro-hidden fields (see scan output)", stats.Skipped);
        if (stats.SourceFileWarnings > 0)
            Logger::Log::Warning("[NativeScripting] {} component(s) inheriting {} are in .cpp files and "
                                 "will NOT be registered — move them to a header", stats.SourceFileWarnings, cfg.DetectBase);

        // The user-system registration TU: wraps each detected system in a
        // UserSystemAdapter<T>; the engine's bridge ticks them during play mode.
        std::string sysError;
        const std::filesystem::path sysGen = CMakeInvoker::RunSystemScanner(cfg, result.Output, sysError);
        if (sysGen.empty())
        {
            LogFail(result, sysError.empty() ? "system scan failed" : sysError);
            return {};
        }
    }
    else
    {
        // Macros-only mode: drop stale scanner outputs so the generated project's
        // *.gen.cpp glob doesn't compile registrations from a scanner that no longer
        // runs (they may reference user types that have since been deleted).
        std::error_code iterEc;
        for (std::filesystem::directory_iterator it(cfg.BuildDir, iterEc), end; !iterEc && it != end;
             it.increment(iterEc))
        {
            if (it->path().extension() == ".cpp" && it->path().stem().extension() == ".gen")
            {
                std::error_code removeEc;
                std::filesystem::remove(it->path(), removeEc);
            }
        }
    }

    // 1. Generate the standalone user-project CMakeLists (content-compared).
    bool changed = false;
    std::string genError;
    const std::filesystem::path cmakeLists = UserProjectGenerator::Generate(cfg, changed, genError);
    if (cmakeLists.empty())
    {
        LogFail(result, "project generation failed: " + genError);
        return {};
    }

    // 2. Configure (once) + build. Re-configure when the project file changed. The
    //    cancel hook lets Shutdown abort a long build instead of blocking on it. Ninja is only
    //    selected when MsvcToolchain validated a COMPLETE env (UCRT include + LIB), so a build
    //    failure here is a real user-code/compile error — reported directly (no slow VS retry
    //    that would double every iteration's compile-error turnaround).
    std::string buildError;
    const std::filesystem::path dll = CMakeInvoker::ConfigureAndBuild(
        cfg, /*forceConfigure=*/changed, result.Output, buildError,
        [this] { return m_CancelBuild.load(std::memory_order_relaxed); });
    result.Configured = true;
    if (dll.empty())
    {
        LogCompilerErrors(result.Output); // surface the actual compiler/linker errors
        LogFail(result, buildError.empty() ? "compile failed" : buildError);
        return {};
    }
    result.Compiled = true;

    // 3. Shadow-copy to a versioned name so a future rebuild never fights this load.
    const std::filesystem::path activeDir =
        cfg.ActiveDir.empty() ? (cfg.BuildDir / "active") : cfg.ActiveDir;
    const std::filesystem::path versioned = ShadowCopyVersioned(dll, activeDir);
    if (versioned.empty())
    {
        LogFail(result, "shadow-copy of " + dll.string() + " failed");
        return {};
    }
    result.LoadedModulePath = versioned;
    return versioned;
}

void NativeScriptManager::LoadModule(const std::filesystem::path& dll, const std::string& moduleId,
                                     NativeBuildResult& result)
{
    result.LoadedModulePath = dll;
    result.ModuleId = moduleId;

    // Already mapped this process (e.g. a repeated cached load with no source change):
    // the registrars ran on the first map, so don't open a duplicate handle or re-register.
    if (std::any_of(m_LoadedModules.begin(), m_LoadedModules.end(),
                    [&dll](const LoadedModule& m) { return m.Path == dll; }))
    {
        result.Loaded = true;
        result.Registered = true;
        return;
    }

    // We are committed to running this module's registrars below (static init + Register_v1), so
    // drop THIS module's previously-registered user systems HERE — pairing the clear with the
    // actual re-register rather than the caller, scoped to the owning module so a package module
    // reload never wipes the project module's systems (or vice versa). An edit-then-revert that
    // resolves to an already-mapped DLL hits the early-return above (systems kept, not wiped).
    // Components self-clear (keyed registry); the user-system registry is append-only, so it
    // needs the explicit per-module drop.
    ClearUserSystems(moduleId);

    // Everything the module registers while it loads (static init at LoadLibrary + Register_v1)
    // is stamped with its module id + this load's generation (C12): the user-system registry
    // reads the id, and every other registry reads the ECS-side stamp. Reset on every exit path.
    const std::uint64_t generation = ++m_ModuleGenerations[moduleId];
    SetActiveRegistrationModule(moduleId);
    ECS::SetActiveRegistrationModule(moduleId, generation);
    struct ActiveModuleScope
    {
        ~ActiveModuleScope()
        {
            SetActiveRegistrationModule({});
            ECS::ClearActiveRegistrationModule();
        }
    } activeModuleScope;

    // Loader-lock contract: engine-plugin registrations made during static init
    // mutate the registry immediately, but their late-registration replays
    // (component/schema/system hooks + schedule re-solve) queue behind this
    // hold and fire only after the handshake below passed — outside the loader
    // lock, and never for a DLL that is about to be refused and unmapped.
    Plugins::EnginePluginRegistry::Get().BeginRegistrationHold();
    struct RegistrationHoldScope
    {
        ~RegistrationHoldScope() { Plugins::EnginePluginRegistry::Get().EndRegistrationHold(); }
    } registrationHoldScope;

    // A handshake abort below unmaps a DLL whose registrars already ran: every
    // registration stamped with this load's generation must be dropped first,
    // or the registries dangle into the dead image (plugins, component
    // handlers, reflected field tables, schemas, schedule registrations).
    // Filled in once the image below is mapped; read by the abort purge, which is
    // declared first because every handshake failure funnels through it.
    std::uint64_t mappedImageBase = 0;
    std::uint64_t mappedImageSize = 0;

    // The handle below is a local, so every abort return unmaps the image.
    // Anything holding a raw callable out of it must be released here too, while
    // it is still mapped — the refused module's static init already ran and may
    // have handed one to an engine-owned object.
    const auto purgeAbortedRegistrations = [&] {
        const std::size_t purgedPlugins =
            Plugins::EnginePluginRegistry::Get().PurgeModulePlugins(moduleId, generation);
        if (m_ModulePurge)
            m_ModulePurge(moduleId, generation);
        ClearUserSystems(moduleId);
        if (mappedImageBase && mappedImageSize && m_ImageObserver.ImageUnmapping)
            m_ImageObserver.ImageUnmapping(mappedImageBase, mappedImageSize);
        Logger::Log::Warning("[NativeScripting] module '{}' load aborted: purged its generation-{} "
                             "registrations ({} engine plugin(s)) before unmapping",
                             moduleId, generation, purgedPlugins);
    };

    // Load + pre-flight checks — plain-integer exports, safe to call across a
    // mismatched toolchain — before Register_v1 runs any of the module's real C++.
    // (C10 caveat: component static initializers already ran at LoadLibrary; the
    // checks still gate registration/system wiring, and the purge above drops
    // what static init managed to register when a check refuses the module.)
    // Bracket the raw map: the image's static initializers run inside this
    // constructor, before its base is knowable, so anything they hand to an
    // engine-owned object can only be attributed once the range below exists.
    // The close is a scope guard, not a plain call: a window left open would make
    // every LATER registration in the process look provisionally module-owned,
    // and nothing would ever resolve it. Closing with {0,0} on an exception is
    // the same "the map produced no image" answer a failed LoadLibrary gives.
    struct ImageMapWindow
    {
        const ModuleImageObserver& Observer;
        const std::uint64_t& Base;
        const std::uint64_t& Size;
        bool Closed = false;
        void Close()
        {
            if (Closed)
                return;
            Closed = true;
            if (Observer.ImageMapEnd)
                Observer.ImageMapEnd(Base, Size);
        }
        ~ImageMapWindow()
        {
            if (!Closed && Observer.ImageMapEnd)
                Observer.ImageMapEnd(0, 0);
        }
    } imageMapWindow{m_ImageObserver, mappedImageBase, mappedImageSize};
    if (m_ImageObserver.ImageMapBegin)
        m_ImageObserver.ImageMapBegin();
    auto handle = MakeUnique<UserModuleHandle>(dll);
    if (handle->IsValid())
    {
        mappedImageBase = handle->ImageBase();
        mappedImageSize = handle->ImageSize();
    }
    imageMapWindow.Close();
    if (!handle->IsValid())
        return (void)LogFail(result, "LoadLibrary failed: " + handle->LastError());

    const auto rejectModule = [&](std::string reason) {
        // Cleanup can invoke module code. Record the reason and enqueue its
        // diagnostic before any of those callbacks run.
        LogFail(result, std::move(reason));
        purgeAbortedRegistrations();
        handle.reset();
        // Match the successful-unload path: this notification means the handle
        // has been released, including the module's detach/static destructors.
        if (mappedImageBase && m_ImageObserver.ImageUnmapped)
            m_ImageObserver.ImageUnmapped(mappedImageBase);
    };

    auto abiFn = handle->GetFunction<std::uint32_t (*)()>("GE_UserModule_AbiVersion_v1");
    if (!abiFn)
    {
        return rejectModule("user module missing GE_UserModule_AbiVersion_v1 export");
    }
    const std::uint32_t moduleAbi = abiFn();
    if (moduleAbi != GE_USERMODULE_ABI_VERSION)
    {
        return rejectModule("ABI version mismatch: module=" + std::to_string(moduleAbi) + " host=" +
                            std::to_string(GE_USERMODULE_ABI_VERSION) + " — rebuild user scripts against this engine");
    }

    // Toolchain handshake: same engine ABI version does not imply a compatible
    // build — a different MSVC update, CRT flavor, or _ITERATOR_DEBUG_LEVEL changes
    // STL layouts and corrupts memory far from the cause. Compare field-by-field
    // and refuse with the specific difference. A module exporting all zeroes was
    // built by a pre-fingerprint SDK and is refused the same way (rebuild fixes it).
    auto fingerprintFn =
        handle->GetFunction<void (*)(GE_ToolchainFingerprint*)>("GE_UserModule_ToolchainFingerprint_v1");
    if (!fingerprintFn)
    {
        return rejectModule("user module missing GE_UserModule_ToolchainFingerprint_v1 export");
    }
    GE_ToolchainFingerprint moduleFingerprint{};
    fingerprintFn(&moduleFingerprint);
    const std::string fingerprintMismatch =
        DescribeToolchainFingerprintMismatch(HostToolchainFingerprint(), moduleFingerprint);
    if (!fingerprintMismatch.empty())
    {
        return rejectModule("toolchain fingerprint mismatch — rebuild user scripts against this engine: " +
                            fingerprintMismatch);
    }
    result.Loaded = true;

    // Register. The host API carries the version + (empty for now) service registry.
    // Components already self-registered via static init at load.
    GE_UserModuleHostApi hostApi{};
    hostApi.StructSize = static_cast<std::uint32_t>(sizeof(GE_UserModuleHostApi));
    hostApi.AbiVersion = GE_USERMODULE_ABI_VERSION;
    hostApi.Services = nullptr;
    auto regFn = handle->GetFunction<int (*)(const GE_UserModuleHostApi*)>("GE_UserModule_Register_v1");
    if (regFn)
    {
        const int rc = regFn(&hostApi);
        if (rc != 0)
        {
            return rejectModule("GE_UserModule_Register_v1 returned " + std::to_string(rc));
        }
    }

    // Handshake passed: release the held late-registration replays now, inside
    // the module-stamp bracket (the replay's schedule contributions must carry
    // this load's stamp) and outside the loader lock.
    Plugins::EnginePluginRegistry::Get().EndRegistrationHold();
    result.Registered = true;
    result.RegistrationsChanged = true;

    // Reload reconcile fan-out: the replay above re-owned re-declared
    // registrations in place; this drops what the replay STOPPED declaring
    // (and rebuilds anything instantiated from the old image's factories)
    // while both images are still mapped — before the unload decision below.
    if (m_ModuleReconcile)
        m_ModuleReconcile(moduleId, generation);

    // Keep the module mapped (no FreeLibrary here): the engine holds registrations
    // into it. When this load REPLACED an older mapped version of the same module,
    // the replay above re-owned those registrations — UnloadSupersededModuleVersions
    // decides, per the quiesce ledger, whether the old image can actually be unmapped.
    m_LoadedModules.push_back(LoadedModule{handle.release(), dll, moduleId, generation});
    Logger::Log::Info("[NativeScripting] loaded user module: {} (module '{}' generation {})",
                      dll.string(), moduleId, generation);
    UnloadSupersededModuleVersions(moduleId, generation);
}

void NativeScriptManager::UnloadSupersededModuleVersions(const std::string& moduleId,
                                                         std::uint64_t currentGeneration)
{
    const bool anySuperseded =
        std::any_of(m_LoadedModules.begin(), m_LoadedModules.end(), [&](const LoadedModule& m) {
            return m.ModuleId == moduleId && m.Generation < currentGeneration;
        });
    if (!anySuperseded)
        return;

    if (m_EditorModuleIds.count(moduleId))
    {
        // First-class refusal, not a whisper: name the module, what pins it,
        // and the remedy. The NEW image already serves all dispatch (the
        // replay swapped registrations in place) — the superseded image is
        // only unreclaimed address space until the editor restarts.
        const std::size_t supersededImages = static_cast<std::size_t>(
            std::count_if(m_LoadedModules.begin(), m_LoadedModules.end(), [&](const LoadedModule& m) {
                return m.ModuleId == moduleId && m.Generation < currentGeneration;
            }));
        std::string pins = m_EditorModulePinReport ? m_EditorModulePinReport(moduleId) : std::string{};
        if (pins.empty())
            pins = "editor registries/UI hold module code (no pin report installed in this host)";
        Logger::Log::Warning(
            "[NativeScripting] module '{}' is editor-kind: REFUSING to unmap {} superseded image(s). "
            "New code is live (registrations replaced in place); the old image(s) stay mapped because "
            "editor-held references cannot be quiesced: {}. Plus any live panel UI, inspector rows and "
            "undo entries built by the old code. Restart the editor to unmap superseded images.",
            moduleId, supersededImages, pins);
        return;
    }

    // Safety valve for the wild: force the pre-C12 keep-everything-mapped behavior.
    static const bool unloadDisabled = std::getenv("GE_NATIVE_MODULE_NO_UNLOAD") != nullptr;
    if (unloadDisabled)
    {
        Logger::Log::Info("[NativeScripting] module '{}': superseded image(s) stay mapped "
                          "(GE_NATIVE_MODULE_NO_UNLOAD)",
                          moduleId);
        return;
    }

    // Release safely revocable callables (such as retired UI handlers) BEFORE
    // reading the ledger, while their image is still mapped. Other callables,
    // such as ECS resource-removal hooks, must survive until their owner tears
    // down and unregisters them; DescribeImagePins keeps those images mapped.
    // The observer decides what can be revoked. A nonzero residual count is a
    // refusal to unload, not permission to discard live resource ownership.
    for (const auto& m : m_LoadedModules)
    {
        if (m.ModuleId != moduleId || m.Generation >= currentGeneration || !m.Handle)
            continue;
        const std::uint64_t base = m.Handle->ImageBase();
        const std::uint64_t size = m.Handle->ImageSize();
        if (base && size && m_ImageObserver.ImageUnmapping)
            m_ImageObserver.ImageUnmapping(base, size);
    }

    std::string blockers =
        m_QuiesceCheck ? m_QuiesceCheck(moduleId, currentGeneration)
                       : std::string("no quiesce ledger installed");

    // Remaining callable owners, including deliberately retained hooks, per image.
    if (m_ImageObserver.DescribeImagePins)
    {
        for (const auto& m : m_LoadedModules)
        {
            if (m.ModuleId != moduleId || m.Generation >= currentGeneration || !m.Handle)
                continue;
            const std::uint64_t base = m.Handle->ImageBase();
            const std::uint64_t size = m.Handle->ImageSize();
            if (!base || !size)
                continue;
            const std::string pins = m_ImageObserver.DescribeImagePins(base, size);
            if (pins.empty())
                continue;
            if (!blockers.empty())
                blockers += ", ";
            blockers += pins;
        }
    }

    if (!blockers.empty())
    {
        Logger::Log::Warning("[NativeScripting] module '{}': superseded image(s) stay MAPPED — quiesce "
                             "ledger reports registrations still owned by older generations: {}",
                             moduleId, blockers);
        return;
    }

    for (auto it = m_LoadedModules.begin(); it != m_LoadedModules.end();)
    {
        if (it->ModuleId != moduleId || it->Generation >= currentGeneration)
        {
            ++it;
            continue;
        }
        Logger::Log::Info("[NativeScripting] unloading superseded module image: {} ('{}' generation {} -> "
                          "{}); its static destructors run now and its shadow copy becomes deletable",
                          it->Path.string(), moduleId, it->Generation, currentGeneration);
        const std::uint64_t base = it->Handle->ImageBase();
        it->Handle->Reset(); // FreeLibrary / dlclose — DLL_PROCESS_DETACH runs the image's static dtors
        // Only now: while the image was mapped its range still had to attribute
        // addresses (a callable released above is destroyed inside it).
        if (base && m_ImageObserver.ImageUnmapped)
            m_ImageObserver.ImageUnmapped(base);
        delete it->Handle;
        it = m_LoadedModules.erase(it);
    }
}

std::uint64_t NativeScriptManager::ModuleLoadGeneration(std::string_view moduleId) const
{
    const auto it = m_ModuleGenerations.find(std::string(moduleId));
    return it != m_ModuleGenerations.end() ? it->second : 0;
}

void NativeScriptManager::SetBuildConfig(const NativeBuildConfig& config)
{
    m_BuildConfig = config;
    m_HaveConfig = true;
    PruneStaleShadowCopies();
}

void NativeScriptManager::SetPackageModuleConfigs(std::vector<NativeBuildConfig> configs)
{
    m_PackageConfigs = std::move(configs);
    // Editor-kind modules (they link the EditorSDK) are excluded from the
    // superseded-image unload: their live editor UI/factories are not covered
    // by the quiesce ledger. Accumulated, not reset — a module once loaded as
    // editor-kind stays excluded even across a project switch that drops it
    // from the config set (its old image may still be mapped).
    for (const NativeBuildConfig& config : m_PackageConfigs)
    {
        if (!config.EditorImportLib.empty())
            m_EditorModuleIds.insert(config.ModuleName);
    }
    // Arm the initial-pass signal for the new set: complete only after the
    // first KickBuild pass gives every package module a terminal outcome.
    m_InitialPackageOutcomesPending = (m_PackageConfigs.empty() || !m_ModuleLoadingSupported) ? 0 : kInitialPassNotRun;
    if (!m_PackageConfigs.empty())
        Logger::Log::Info("[NativeScripting] {} package native module(s) registered", m_PackageConfigs.size());
}

std::vector<NativeBuildConfig> NativeScriptManager::ModuleConfigs() const
{
    std::vector<NativeBuildConfig> configs = m_PackageConfigs;
    if (m_HaveConfig)
        configs.push_back(m_BuildConfig);
    return configs;
}

void NativeScriptManager::PruneStaleShadowCopies()
{
    // Same fallback as the shadow-copy step in BuildOnly.
    const std::filesystem::path activeDir =
        m_BuildConfig.ActiveDir.empty() ? (m_BuildConfig.BuildDir / "active") : m_BuildConfig.ActiveDir;
    std::error_code ec;
    if (!std::filesystem::exists(activeDir, ec) || ec)
        return;

    // The one shadow copy the build cache still names (what a cached/optimistic load
    // would map next); every other file in active/ is a dead build. equivalent() is
    // stat-based, so mixed separators/case can't defeat the match.
    std::filesystem::path referenced;
    if (const std::optional<BuildCacheRecord> cache = ReadBuildCacheRecord(m_BuildConfig.BuildDir))
        referenced = cache->DllPath;

    int pruned = 0;
    for (std::filesystem::directory_iterator it(activeDir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || entryEc)
            continue;
        const std::filesystem::path& path = it->path();
        const auto sameFile = [&path](const std::filesystem::path& other) {
            std::error_code eqEc;
            return std::filesystem::equivalent(path, other, eqEc);
        };
        if (!referenced.empty() && sameFile(referenced))
            continue;
        // A module still mapped this session (project switch-and-back) stays on disk;
        // the OS locks mapped images anyway, this skip just avoids the failed delete.
        // Unloaded superseded images leave m_LoadedModules, so their shadow copies
        // ARE pruned here once the build cache stops referencing them.
        if (std::any_of(m_LoadedModules.begin(), m_LoadedModules.end(),
                        [&sameFile](const LoadedModule& m) { return sameFile(m.Path); }))
            continue;
        std::filesystem::remove(path, entryEc);
        if (!entryEc)
            ++pruned;
    }
    if (pruned > 0)
        Logger::Log::Info("[NativeScripting] pruned {} stale shadow-copied module(s) from {}",
                          pruned, activeDir.string());
}

bool NativeScriptManager::AreModulesPending() const
{
    if (!m_ModuleLoadingSupported)
        return false;
    return m_PendingChange.load(std::memory_order_relaxed) || m_RebuildRequested.load(std::memory_order_relaxed) ||
           m_BuildInFlight.load(std::memory_order_relaxed) || m_InitialPackageOutcomesPending != 0 ||
           HasDeferredHotReloadPending();
}

void NativeScriptManager::SetModuleLoadingSupported(bool supported)
{
    m_ModuleLoadingSupported = supported;
    if (!supported)
        m_InitialPackageOutcomesPending = 0;
}

bool NativeScriptManager::AnyShippedModuleBuildFailed() const
{
    // The main thread is the list's only writer (RecordModuleOutcomeLocked), so its own read needs
    // no lock; the lock is for WaitForModuleBuilds' readers on other threads.
    return !m_FailedShippedModules.empty();
}

void NativeScriptManager::RequestRebuild()
{
    if (!m_HaveConfig && m_PackageConfigs.empty())
        return;
    m_RebuildRequested.store(true, std::memory_order_relaxed);
}

void NativeScriptManager::KickBuild()
{
    // All modules in load order: package modules (topo) then the project module.
    const std::vector<NativeBuildConfig> configs = ModuleConfigs();
    if (configs.empty())
        return;

    // Per-module staleness: compute both digests (full = sources+ABI, abi = ABI only) and read
    // the cached build once. The cached/no-op paths below DON'T stamp the throttle, so a genuine
    // edit right after a cached open kicks immediately. Up-to-date modules load directly (main
    // thread, no cmake); stale ones are batched onto one worker.
    struct StaleModule
    {
        NativeBuildConfig Config;
        std::string FullDigest;
        std::string AbiDigest;
        bool IsPackageModule = false;
    };
    std::vector<StaleModule> staleModules;

    // Whether THIS pass is the first for the current package config set — it
    // owns the InitialPackageModulePassComplete transition (later passes are
    // edit-driven rebuilds and must not touch the signal).
    const bool initialPackagePass = m_InitialPackageOutcomesPending == kInitialPassNotRun;
    std::size_t configIndex = 0;

    for (const NativeBuildConfig& config : configs)
    {
        const bool isPackageModule = configIndex < m_PackageConfigs.size();
        ++configIndex;
        // Package-only / script-less projects: with ZERO native sources under
        // the project's Assets tree there is nothing to compile — generating
        // and building an empty UserScripts DLL on every project open is pure
        // waste (cmake + MSVC for a module that registers nothing). Skip
        // loudly; the source watcher's rebuild request re-enters here the
        // moment the first source file appears. Headers count as sources: a
        // header-only component project builds through the scanner.
        if (!isPackageModule && !HasWatchedNativeSource(config.SourceDir))
        {
            if (!m_EmptyProjectSkipLogged)
            {
                Logger::Log::Info(
                    "[NativeScripting] project has no native C++ sources under '{}' — skipping the "
                    "user-script module build (adding a .cpp/.h under Assets enables it)",
                    config.SourceDir.string());
                m_EmptyProjectSkipLogged = true;
            }
            // A build record without sources would read as "stale native
            // module" at package time — no sources means no module, so drop it.
            std::error_code recordEc;
            if (std::filesystem::remove(config.BuildDir / "last_build.txt", recordEc))
                Logger::Log::Info(
                    "[NativeScripting] removed stale native build record for the now-empty project");
            continue;
        }
        if (!isPackageModule)
            m_EmptyProjectSkipLogged = false;
        // P3 prebuilt packages: a usable shipped binary replaces the whole
        // digest/build path for this module (nothing to be stale against —
        // the package payload is immutable).
        if (!config.PrebuiltDir.empty())
        {
            NativeBuildResult prebuilt;
            if (TryLoadPrebuiltPackageModule(config, prebuilt))
            {
                RecordModuleOutcomeLocked(config.ModuleName, true);
                if (m_OnAfterReload)
                    m_OnAfterReload();
                if (m_OnBuildComplete)
                    m_OnBuildComplete(prebuilt);
                continue;
            }
        }

        std::string fullDigest;
        std::string abiDigest;
        ComputeDigests(config, fullDigest, abiDigest);
        const std::optional<BuildCacheRecord> cache = ReadBuildCacheRecord(config.BuildDir);

        // Fully up to date: source set + engine ABI unchanged and the previously-built DLL is
        // present -> load it directly, skip cmake entirely.
        NativeBuildResult cached;
        if (cache && cache->Digest == fullDigest && LoadCachedDll(cache->DllPath, config.ModuleName, cached))
        {
            Logger::Log::Info("[NativeScripting] module '{}' up to date — loaded cached DLL (no rebuild)",
                              config.ModuleName);
            // The cached path skips BuildOnly, and with it the project regeneration — but the
            // persisted CMakeLists doubles as the IDE entry point and carries a baked
            // last-known-good SDK path. Refresh it on every engine touch of the project
            // (content-compared no-op when nothing moved; Generate reports a dead baked SDK
            // dir as an ERROR naming it before rewriting).
            bool projectChanged = false;
            std::string projectError;
            if (UserProjectGenerator::Generate(config, projectChanged, projectError).empty())
                Logger::Log::Warning("[NativeScripting] could not refresh user CMakeLists: {}", projectError);
            // The cached path registers the module just like a fresh build, so fire the after-reload
            // hook here too — otherwise a scene that loaded before the cached DLL mapped (the common
            // project-open race) never gets its preserved unknown components re-applied. There are no
            // pre-existing placed instances to migrate on a cold cached open, so the before-hook
            // (layout snapshot) is intentionally not fired.
            RecordModuleOutcomeLocked(config.ModuleName, true);
            if (m_OnAfterReload)
                m_OnAfterReload();
            if (m_OnBuildComplete)
                m_OnBuildComplete(cached);
            continue;
        }

        // The full digest is stale, so a rebuild is needed. If only the SOURCES changed (the cached
        // build's ABI digest still matches — same engine), optimistically load the last-good DLL
        // right now so the module, and any scene components depending on it, is available
        // immediately instead of waiting out the cold cmake+compile; the background build below
        // hot-swaps it when ready. On an in-session edit the last-good DLL is already mapped, so
        // LoadModule no-ops. Skipped when the engine ABI changed (loading a DLL built against a
        // different engine could crash) or there is no usable cached DLL — the preserve/re-apply
        // path covers data safety in those cases.
        NativeBuildResult optimistic;
        if (cache && !cache->AbiDigest.empty() && cache->AbiDigest == abiDigest &&
            LoadCachedDll(cache->DllPath, config.ModuleName, optimistic))
        {
            Logger::Log::Info("[NativeScripting] loaded last-good module '{}' optimistically; "
                              "rebuilding in background",
                              config.ModuleName);
            if (m_OnAfterReload)
                m_OnAfterReload();
        }
        staleModules.push_back(StaleModule{config, fullDigest, abiDigest, isPackageModule});
    }

    // The initial pass owes the signal a count: package modules that loaded
    // synchronously above (prebuilt/cached) are already terminal; the rest
    // complete when the worker batch's completion task processes them.
    const int stalePackageModules = static_cast<int>(
        std::count_if(staleModules.begin(), staleModules.end(),
                      [](const StaleModule& m) { return m.IsPackageModule; }));
    if (initialPackagePass)
        m_InitialPackageOutcomesPending = stalePackageModules;

    if (staleModules.empty())
        return;

    if (!m_BuildChannel)
    {
        Logger::Log::Warning("[NativeScripting] no JobSystem — cannot build user scripts asynchronously");
        if (initialPackagePass)
            m_InitialPackageOutcomesPending = 0; // no worker will ever report; don't wedge the signal
        return;
    }

    // Dispatch the batch as one build job; loads + registration land back on the main thread
    // (registries are main-thread-only) in module order. Shutdown waits on m_BuildTask.
    m_BuildInFlight.store(true, std::memory_order_relaxed);
    m_ShippedModulesBuilding.clear();
    for (const StaleModule& stale : staleModules)
    {
        if (m_EditorModuleIds.count(stale.Config.ModuleName) == 0)
            m_ShippedModulesBuilding.push_back(stale.Config.ModuleName);
    }

    // Signal the editor (we're already on the main thread — KickBuild runs from Tick).
    if (m_OnBuildStarted)
        m_OnBuildStarted();

    m_BuildTask = m_BuildChannel->Submit([this, staleModules, initialPackagePass]() {
        struct ModuleOutcome
        {
            std::filesystem::path Dll; // empty on build failure
            std::string ModuleId;
            std::filesystem::path BuildDir;
            NativeBuildResult Result;
            std::string FullDigest;
            std::string AbiDigest;
            bool IsPackageModule = false;
        };
        std::vector<ModuleOutcome> outcomes;
        outcomes.reserve(staleModules.size());
        for (const StaleModule& stale : staleModules)
        {
            ModuleOutcome outcome;
            outcome.ModuleId = stale.Config.ModuleName;
            outcome.BuildDir = stale.Config.BuildDir;
            outcome.FullDigest = stale.FullDigest;
            outcome.AbiDigest = stale.AbiDigest;
            outcome.IsPackageModule = stale.IsPackageModule;
            try
            {
                outcome.Dll = BuildOnly(stale.Config, outcome.Result);
            }
            catch (const std::exception& e)
            {
                LogFail(outcome.Result, std::string("native build threw: ") + e.what());
            }
            catch (...)
            {
                LogFail(outcome.Result, "native build threw a non-standard exception");
            }
            outcomes.push_back(std::move(outcome));
            if (m_CancelBuild.load(std::memory_order_relaxed))
                break; // shutdown: skip the remaining modules
        }

        // Always queue the completion (clears m_BuildInFlight + fires the callback), even
        // on failure, so the build pipeline never wedges.
        QueueMainThreadTask([this, outcomes, initialPackagePass]() mutable {
            // Throttle runs from completion; failures stamp too, so a broken build
            // can't retry in a tight loop.
            m_LastBuildCompleteTime = std::chrono::steady_clock::now();
            m_BuildInFlight.store(false, std::memory_order_relaxed);
            m_ShippedModulesBuilding.clear();

            for (ModuleOutcome& outcome : outcomes)
            {
                // Every processed package outcome of the initial pass is terminal
                // for InitialPackageModulePassComplete — loaded, failed, or
                // stashed for play-exit (play can't be active during the project
                // open the signal gates, and a wedged signal would hold VCS
                // detection forever).
                if (initialPackagePass && outcome.IsPackageModule && m_InitialPackageOutcomesPending > 0)
                    --m_InitialPackageOutcomesPending;
                // A build dispatched before play-mode began completes on the worker
                // regardless of play state; loading it mid-play would clear +
                // re-register live systems without OnDestroy/OnStart. Stash the built
                // DLL (and everything the load needs) for the play-exit flush instead.
                // Failures carry no DLL and touch no engine state, so they report
                // immediately.
                if (!outcome.Dll.empty() && m_HotReloadDeferred.load(std::memory_order_relaxed))
                {
                    Logger::Log::Info(
                        "[NativeScripting] build finished during play-mode; module swap deferred until exit");
                    m_DeferredBuildLoads.push_back(DeferredBuildLoad{outcome.Dll, outcome.ModuleId,
                                                                     outcome.BuildDir, std::move(outcome.Result),
                                                                     outcome.FullDigest, outcome.AbiDigest,
                                                                     m_EditorModuleIds.count(outcome.ModuleId) == 0});
                    continue;
                }

                if (!outcome.Dll.empty())
                {
                    // This block is the whole main-thread hitch of a native module swap:
                    // layout snapshot (before-hook), LoadLibrary + registrars, then placed-
                    // instance migration (after-hook). One log line per swap event.
                    const auto swapStart = std::chrono::steady_clock::now();
                    // Snapshot component layouts before LoadLibrary overwrites the reflection
                    // registries, so the editor can migrate placed instances whose layout changed.
                    if (m_OnBeforeReload)
                        m_OnBeforeReload();
                    const auto beforeHookEnd = std::chrono::steady_clock::now();
                    LoadModule(outcome.Dll, outcome.ModuleId, outcome.Result); // per-module clear + re-register
                    const auto loadEnd = std::chrono::steady_clock::now();
                    if (outcome.Result.Registered)
                    {
                        WriteBuildCache(outcome.BuildDir, outcome.FullDigest, outcome.AbiDigest, outcome.Dll);
                        if (m_OnAfterReload)
                            m_OnAfterReload();
                    }
                    const auto swapEnd = std::chrono::steady_clock::now();
                    using SwapMs = std::chrono::duration<double, std::milli>;
                    Logger::Log::Info("[NativeScripting] module swap '{}' main-thread hitch: {:.2f} ms "
                                      "(layout snapshot {:.2f}, load+register {:.2f}, cache+migrate {:.2f})",
                                      outcome.ModuleId, SwapMs(swapEnd - swapStart).count(),
                                      SwapMs(beforeHookEnd - swapStart).count(),
                                      SwapMs(loadEnd - beforeHookEnd).count(), SwapMs(swapEnd - loadEnd).count());
                }
                RecordModuleOutcomeLocked(outcome.ModuleId, !outcome.Dll.empty() && outcome.Result.Registered);
                if (m_OnBuildComplete)
                    m_OnBuildComplete(outcome.Result);
            }
        });
    });
}

void NativeScriptManager::QueueMainThreadTask(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_MainThreadMutex);
    m_MainThreadTasks.push(std::move(task));
}

void NativeScriptManager::ProcessMainThreadTasks()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_MainThreadMutex);
            if (m_MainThreadTasks.empty())
                return;
            task = std::move(m_MainThreadTasks.front());
            m_MainThreadTasks.pop();
        }
        task(); // run outside the lock — the task may load/register and re-enter
    }
}

void NativeScriptManager::ComputeDigests(const NativeBuildConfig& config, std::string& outFull,
                                         std::string& outAbi) const
{
    // ABI inputs first — config + the engine import-lib (size+mtime, which any relink bumps) +
    // compile-definitions. The hash after just these IS the ABI digest: a cached DLL whose ABI
    // digest still matches was built against the same engine, so it is safe to optimistic-load.
    std::uint64_t hash = HashEngineAbiInputs(config);
    outAbi = ToHexDigest(hash);

    // Continue the SAME running hash over the watched sources -> the full digest, which
    // additionally differs on any source edit/add/remove. Keeping the ABI inputs as the
    // prefix is what makes the ABI digest a snapshot of the full digest mid-stream.
    outFull = ToHexDigest(HashWatchedNativeSources(config.SourceDir, config.PackageRootDir, hash));
}

bool NativeScriptManager::TryLoadPrebuiltPackageModule(const NativeBuildConfig& config,
                                                       NativeBuildResult& outResult)
{
    if (config.PrebuiltDir.empty())
        return false;

    const PrebuiltModuleLookup lookup =
        FindPrebuiltModule(config.PrebuiltDir, HostToolchainFingerprint(), config.ModuleName,
                           ComputeEngineAbiDigest(config));
    if (lookup.Dll.empty())
    {
        Logger::Log::Info("[NativeScripting] module '{}': prebuilt binaries unusable — {}; "
                          "building from source",
                          config.ModuleName, lookup.Reason);
        return false;
    }
    if (!LoadCachedDll(lookup.Dll, config.ModuleName, outResult))
    {
        // The DLL's own exported fingerprint handshake (LoadModule) is the
        // final gate; a refusal there means the shipped marker lied.
        Logger::Log::Warning("[NativeScripting] module '{}': prebuilt '{}' failed to load ({}); "
                             "building from source",
                             config.ModuleName, lookup.Dll.string(), outResult.Error);
        outResult = NativeBuildResult{};
        return false;
    }
    Logger::Log::Info("[NativeScripting] module '{}': loaded prebuilt binary '{}' (no build)",
                      config.ModuleName, lookup.Dll.string());
    return true;
}

bool NativeScriptManager::LoadCachedDll(const std::filesystem::path& dll, const std::string& moduleId,
                                        NativeBuildResult& result)
{
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec))
        return false;
    LoadModule(dll, moduleId, result);
    return result.Registered; // a stale/invalid cached DLL falls through to a fresh build
}

void NativeScriptManager::WriteBuildCache(const std::filesystem::path& buildDir, const std::string& digest,
                                          const std::string& abiDigest, const std::filesystem::path& dll) const
{
    WriteBuildCacheRecord(buildDir,
                          BuildCacheRecord{digest, dll.generic_string(), abiDigest, EngineBuildIdentity()});
}

} // namespace NativeScripting
} // namespace GameEngine
