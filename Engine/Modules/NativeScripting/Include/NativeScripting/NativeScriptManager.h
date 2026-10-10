#pragma once

// NativeScriptManager — owns the native user-script hot-reload lifecycle.
//
// Watches the project's native source files (.cpp/.h/.hpp), debounces edit bursts,
// and drives the async generate→compile→shadow-copy→load→register pipeline. The
// shape deliberately mirrors the C# ScriptManager (Initialize / Shutdown +
// play-mode-deferred reload) so the two read alike.

#include "NativeScripting/MsvcToolchain.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "Types/Types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
}

namespace GameEngine
{
// From Assets/FileWatchingService.h / FileWatcher/FileWatcher.h — forward-declared to
// keep those (and <regex>) out of this header.
class FileWatchSubscription;
struct FileChangeEvent;

namespace NativeScripting
{

class UserModuleHandle; // NativeScripting/UserModuleHandle.h (full include in the cpp)

class NativeScriptManager
{
public:
    NativeScriptManager();
    ~NativeScriptManager();

    NativeScriptManager(const NativeScriptManager&) = delete;
    NativeScriptManager& operator=(const NativeScriptManager&) = delete;

    // jobSystem may be null (unit tests / headless): builds then never start. With one,
    // module builds are jobs of the manager's "Native module builds" channel (cap 1) and
    // toolchain detection a job of its "MSVC detection" channel; Shutdown releases both,
    // so the job system must outlive Shutdown.
    bool Initialize(JobSystem::WorkStealingThreadPool* jobSystem = nullptr);
    void Shutdown();
    bool IsInitialized() const { return m_Initialized; }

    // (Re)watch a directory tree for native source edits. An empty path clears the
    // watch. The editor calls this when a project opens.
    void SetWatchDirectory(const std::filesystem::path& sourceDir);
    const std::filesystem::path& WatchDirectory() const { return m_WatchDirectory; }

    // Store the engine build interface + project paths used by the async build path
    // (loaded from the staged SDK manifest by the editor). Until this is set, file
    // edits + RequestRebuild() are no-ops. Safe to call again on project switch.
    void SetBuildConfig(const NativeBuildConfig& config);
    bool HasBuildConfig() const { return m_HaveConfig; }

    // P1 packages: one NativeBuildConfig per package Cpp module, in resolver
    // topo order (dependencies first). Each carries its own SourceDir (module
    // root), BuildDir/ActiveDir (under <packageRoot>/.Cache/NativeScripts/) and
    // ModuleName (package assembly name), so build-cache records and registry
    // module ids stay per-module. Built + loaded BEFORE the project module on
    // every kick. Replaces the previous set; call alongside SetBuildConfig.
    void SetPackageModuleConfigs(std::vector<NativeBuildConfig> configs);

    // True once every package module of the CURRENT config set has reached a
    // terminal first outcome — prebuilt/cached load, or a finished (successful
    // OR failed) source build. Trivially true with no package modules
    // configured. The editor gates the FIRST VCS workspace detection on this,
    // so package-provided providers (git/svn/…) all register before the first
    // claim and a mixed workspace never sees a transient wrong-provider claim.
    // Main thread only (every transition happens there).
    bool InitialPackageModulePassComplete() const { return m_InitialPackageOutcomesPending == 0; }

    // True while a native module of the current configuration, the project module or a package
    // module, may still load and register types: a requested or debounced build has not started,
    // a build is running, the first pass over the package modules has not finished, or a finished
    // build waits for play mode to end. The render pipeline waits on it before it refuses a pass
    // type no loaded module registers. Main thread only.
    bool AreModulesPending() const;

    // Whether this platform loads native modules at all (Platform::SupportsDynamicNativeModules).
    // Where it does not, no module is waited for: AreModulesPending is false and the first package
    // pass counts as complete. True until set; EngineCore sets it at initialization.
    void SetModuleLoadingSupported(bool supported);

    // True when the latest build of a module that ships with the game failed, so a type it would
    // register is missing for that reason. Main thread only.
    bool AnyShippedModuleBuildFailed() const;

    // Invoked on the MAIN thread after every async build completes (success or failure)
    // so the editor can surface a toast / route errors to the log. Optional.
    using BuildCompletedCallback = std::function<void(const NativeBuildResult&)>;
    void SetBuildCompletedCallback(BuildCompletedCallback callback) { m_OnBuildComplete = std::move(callback); }

    // Invoked on the MAIN thread when an async build is actually dispatched to a worker
    // (NOT on a cached no-op load, which fires only the completed callback) so the editor
    // can show a "building…" indicator. Optional.
    using BuildStartedCallback = std::function<void()>;
    void SetBuildStartedCallback(BuildStartedCallback callback) { m_OnBuildStarted = std::move(callback); }

    // Bracket a real module (re)load with two MAIN-THREAD callbacks. `beforeLoad` fires right
    // before LoadLibrary overwrites the reflection registries (so the editor can snapshot
    // component layouts); `afterLoad` fires right after a successful load+register (so the
    // editor can migrate placed instances whose component layout changed). The manager stays
    // engine-agnostic — all ECS work lives in the editor's hooks. Both optional.
    // `afterLoad` MUST be idempotent: an optimistic load fires it for the last-good DLL and then
    // again when the background rebuild swaps in the fresh DLL, so it can run twice per build.
    using ReloadHook = std::function<void()>;
    void SetReloadHooks(ReloadHook beforeLoad, ReloadHook afterLoad)
    {
        m_OnBeforeReload = std::move(beforeLoad);
        m_OnAfterReload = std::move(afterLoad);
    }

    // C12: purge fan-out for registries this module cannot see (engine plugins,
    // component handlers/field tables, scene schemas, schedule registrations).
    // LoadModule invokes it with the module's id + the aborted load's generation
    // when a DLL whose registrars already ran fails the load handshake and is
    // about to be unmapped — every registration stamped with that exact
    // generation must be dropped before the image goes away. EngineCore
    // installs the handler at startup.
    using ModuleRegistrationPurgeHandler =
        std::function<void(std::string_view moduleId, std::uint64_t generation)>;
    void SetModuleRegistrationPurgeHandler(ModuleRegistrationPurgeHandler handler)
    {
        m_ModulePurge = std::move(handler);
    }

    // Load generation of `moduleId` (bumped once per real (re)load attempt);
    // 0 = never loaded. Registrations are stamped with it (ECS/ModuleRegistration.h).
    std::uint64_t ModuleLoadGeneration(std::string_view moduleId) const;

    // C12 unload quiesce ledger: asked after a successful reload whether any
    // registry still holds an entry attributed to an OLDER generation of
    // `moduleId`. Returns an empty string when fully re-owned (the superseded
    // image may be unmapped) or a human-readable list of blockers (it stays
    // mapped, loudly). EngineCore installs the handler with visibility into
    // the plugin registry, component/field/schema registries and the live
    // schedule model.
    using ModuleQuiesceCheck =
        std::function<std::string(std::string_view moduleId, std::uint64_t currentGeneration)>;
    void SetModuleQuiesceCheck(ModuleQuiesceCheck check) { m_QuiesceCheck = std::move(check); }

    // C12 reload reconcile fan-out for registries whose entries are replaced
    // in place by a replay but whose UNDECLARED leftovers the manager cannot
    // retire itself (today: render pipeline node factories + the live pipeline
    // instances built from them). LoadModule invokes it after a successful
    // load's replay completed (registrations re-owned) and BEFORE the unload
    // quiesce decision, so retired entries and torn-down instances never block
    // — or dangle past — the superseded image's unmap. EngineCore installs it.
    using ModuleReloadReconcileHandler =
        std::function<void(std::string_view moduleId, std::uint64_t generation)>;
    void SetModuleReloadReconcileHandler(ModuleReloadReconcileHandler handler)
    {
        m_ModuleReconcile = std::move(handler);
    }

    // C12 module-image observer: the lifecycle of a mapped user image, expressed
    // as address ranges rather than module ids. Some things that point into a
    // superseded image are not registry entries keyed by (moduleId, generation)
    // but raw callables an engine-owned object happens to store — a UI event
    // handler built by the module is the live example. Those can only be found
    // by asking which image an address belongs to, so this seam speaks in
    // [base, base + size) and the manager stays ignorant of who is listening.
    // EngineCore installs it; hosts that never load a user module never call it.
    struct ModuleImageObserver
    {
        // Bracket the raw map. A module's static initializers run INSIDE
        // LoadLibrary, before its base exists, so ImageMapBegin lets a listener
        // attribute what they register once ImageMapEnd supplies the range.
        // ImageMapEnd ALWAYS follows a Begin, with {0, 0} when the map failed.
        std::function<void()> ImageMapBegin;
        std::function<void(std::uint64_t base, std::uint64_t size)> ImageMapEnd;

        // Anything still pointing into [base, base + size), as a human-readable
        // list, or empty when the image is clean. Asked AFTER ImageUnmapping ran,
        // so a non-empty answer means a callable owner remains (possibly one
        // deliberately retained for resource teardown). Keep the image mapped.
        std::function<std::string(std::uint64_t base, std::uint64_t size)> DescribeImagePins;

        // Release safely revocable callables while the image is STILL MAPPED.
        // Resource-owning hooks may need to stay registered; include those in
        // DescribeImagePins rather than silently discarding their teardown.
        std::function<void(std::uint64_t base, std::uint64_t size)> ImageUnmapping;

        // The image is gone; drop its range.
        std::function<void(std::uint64_t base)> ImageUnmapped;
    };
    void SetModuleImageObserver(ModuleImageObserver observer)
    {
        m_ImageObserver = std::move(observer);
    }

    // Editor-kind unload refusal diagnostics: returns a human-readable list of
    // the editor-side registrations pinning `moduleId`'s code (panels, traits,
    // inspectors, pick/VCS providers, plugins). The editor host installs it;
    // UnloadSupersededModuleVersions folds the answer into the loud refusal so
    // the developer sees exactly WHY the superseded image stays mapped.
    using EditorModulePinReport = std::function<std::string(std::string_view moduleId)>;
    void SetEditorModulePinReport(EditorModulePinReport report)
    {
        m_EditorModulePinReport = std::move(report);
    }

    // Mapped module images (all generations still in memory). Superseded
    // generations linger only when the quiesce ledger refused their unload.
    std::size_t LoadedModuleImageCount() const { return m_LoadedModules.size(); }

    // Request an (async) rebuild-and-load of the configured project. Coalesced: a
    // request during an in-flight build, or within the throttle window, is deferred and
    // serviced by Tick(). No-op without a build config / JobSystem. The editor calls
    // this on project-open; the file watcher calls it after edits settle.
    void RequestRebuild();

    // Per-frame (main thread): drains completed-build results, fires the debounced
    // "source settled" rebuild request, and kicks a deferred rebuild once the build is
    // free and past the throttle window.
    void Tick();

    // Called with the module names of the build being waited on (empty while a
    // requested build has not started yet).
    using ModuleBuildWaitReport = std::function<void(const std::vector<std::string>& moduleNames)>;

    // How WaitForModuleBuilds ended.
    enum class ModuleBuildWaitOutcome
    {
        // Nothing pending. Every build record is the latest build's, except for the
        // modules FailedModules names: their latest build failed, so their record
        // is an older build.
        Settled,
        // A build finished, or sources changed, during play mode. The load, the
        // record write and the rebuild wait for play exit (HasDeferredHotReloadPending).
        DeferredByPlayMode,
        Cancelled,
        // Shutdown() ran: the builds will never finish.
        ShutDown,
        // Called from the thread that runs Tick(), where the wait could never end.
        CalledFromTickThread,
    };
    struct ModuleBuildWaitResult
    {
        ModuleBuildWaitOutcome Outcome = ModuleBuildWaitOutcome::Settled;
        // Settled: modules whose latest build failed. Editor-only modules (they
        // never ship in a packaged game) are not listed.
        std::vector<std::string> FailedModules;
        // DeferredByPlayMode: modules whose finished build waits for play exit.
        // Empty when only source changes wait for it.
        std::vector<std::string> DeferredModules;
    };

    // Blocks the CALLING thread until no module build is pending: no source edit in
    // its debounce, no requested build waiting for its kick, and no dispatched build
    // of a shipped module (the project module or a runtime package module; editor-only
    // modules never ship) whose load and build record the main thread has not applied
    // yet. A packaged
    // export calls it before it reads any build record, so it never reads a record
    // that a running build is about to write. `isCancelled` and `report` run on the
    // calling thread without the manager's lock; `report` runs each time the
    // waited-on module set changes and never when nothing is pending. Returns at
    // once with DeferredByPlayMode when play mode holds a finished build or a
    // source change, because only play exit releases them. Must run on a thread
    // other than the one that runs Tick(): the builds it waits for finish there.
    ModuleBuildWaitResult WaitForModuleBuilds(const std::function<bool()>& isCancelled,
                                              const ModuleBuildWaitReport& report);

    // Synchronous generate→compile→shadow-copy→load→register. Returns with the user's
    // components visible in the engine registries. Used by tests and as the building
    // block of the async path. On success the module stays loaded for the process
    // lifetime (kept in m_LoadedModules; not unloaded — the unload protocol is C12).
    NativeBuildResult BuildAndLoad(const NativeBuildConfig& config);

    // Player/runtime entry: load the EDITOR-built user module for `projectRoot` WITHOUT building
    // (a shipped runtime has no compile toolchain). Reads <projectRoot>/.Cache/NativeScripts/build/
    // last_build.txt (written by the editor's build) and loads the recorded DLL — so the user's
    // components register before scene load and their systems are available to tick. Returns false
    // (project simply has no native scripts) if no prebuilt module is present. Call once at startup.
    // Also serves package Cpp modules: the Player calls this once per package with the module's
    // writable cache dir (PackageCodeModule::CacheDir), whose editor-built record lives at
    // <cacheDir>/NativeScripts/build — one of the probed layouts. `prebuiltDir` is the package's
    // shipped-binaries root (PackageCodeModule::PrebuiltDir, P3): when set it is probed FIRST via
    // the same fingerprint-keyed lookup BuildAndLoad uses, so a prebuilt-only package loads in a
    // dev-layout Player even though no editor build cache exists for it.
    bool LoadPrebuiltUserModule(const std::filesystem::path& projectRoot,
                                const std::string& moduleId = "UserScripts",
                                const std::filesystem::path& prebuiltDir = {});

    // Play-mode deferral: hold reloads while the game is running, then flush.
    void SetHotReloadDeferred(bool deferred);
    bool IsHotReloadDeferred() const { return m_HotReloadDeferred.load(std::memory_order_relaxed); }

    // True when play-exit has native-script work to flush: a source change that settled
    // during play, or a finished build whose load was stashed (main thread only).
    bool HasDeferredHotReloadPending() const
    {
        return m_DeferredReloadPending.load(std::memory_order_relaxed) || !m_DeferredBuildLoads.empty();
    }

    // Play-exit flush: load a build that completed (and was stashed) during play, then
    // resume any rebuild for source changes that settled during play. Returns whether
    // either was pending.
    bool TriggerDeferredHotReloadIfPending();

    // Captured build toolchain (Ninja + MSVC env batch). The BuildPipeline reuses the
    // cached environment for spawned tool processes that need the platform linker
    // (NativeAOT dotnet publish).
    const MsvcToolchain& Toolchain() const { return m_Toolchain; }

private:
    void OnFileChanged(const FileChangeEvent& event); // watcher callback (any thread)
    void ProcessSettledChange();                      // post-debounce: requests a rebuild

    // Async build pipeline. BuildOnly runs on a worker (scan/generate/cmake/shadow-copy,
    // no engine state); LoadModule runs on the main thread (LoadLibrary triggers the
    // user DLL's component registrars, which mutate the shared registries). KickBuild
    // (main thread, from Tick) gates on the source-digest staleness check, then either
    // loads the cached DLL directly or dispatches BuildOnly to the JobSystem.
    void KickBuild();
    std::filesystem::path BuildOnly(const NativeBuildConfig& config, NativeBuildResult& result);
    // moduleId = the owning module (NativeBuildConfig::ModuleName); stamps every registration the
    // module makes while loading and scopes the pre-register ClearUserSystems to that module.
    void LoadModule(const std::filesystem::path& dll, const std::string& moduleId, NativeBuildResult& result);

    // Main-thread result marshalling (workers push, Tick drains).
    void QueueMainThreadTask(std::function<void()> task);
    void ProcessMainThreadTasks();

    // P3 prebuilt packages: when config.PrebuiltDir names a usable DLL for the
    // host toolchain + engine ABI, load it and skip the source build entirely.
    // Returns whether the module was loaded+registered; a declared-but-unusable
    // prebuilt logs the reason loudly and returns false (source fallback).
    bool TryLoadPrebuiltPackageModule(const NativeBuildConfig& config, NativeBuildResult& outResult);

    // Build-staleness digests, computed together in one pass. outFull = ABI inputs + source files
    // (drives "fully up to date — skip the build"); outAbi = ABI inputs only (config + engine
    // import-lib + compile-defs) and is the prefix of outFull. A cached DLL whose ABI digest still
    // matches was built against the same engine, so it is safe to optimistic-load while a
    // source-only rebuild runs in the background — avoiding the cold-compile availability gap.
    void ComputeDigests(const NativeBuildConfig& config, std::string& outFull, std::string& outAbi) const;
    // exists() + LoadModule the cached DLL; returns whether it registered. The caller decides via the
    // digests whether the DLL is current (full match) or only ABI-compatible (optimistic load).
    bool LoadCachedDll(const std::filesystem::path& dll, const std::string& moduleId, NativeBuildResult& result);
    void WriteBuildCache(const std::filesystem::path& buildDir, const std::string& digest,
                         const std::string& abiDigest, const std::filesystem::path& dll) const;

    // All build configs in load order: package modules (topo) then the project
    // module when configured.
    std::vector<NativeBuildConfig> ModuleConfigs() const;

    // Delete active/ shadow copies (files only) not referenced by the build cache.
    // Runs from SetBuildConfig — project open, which precedes any LoadModule for that
    // project in the session — so it never touches a module this config could map;
    // modules still mapped from a previously-open project are skipped explicitly.
    void PruneStaleShadowCopies();

    // WaitForModuleBuilds' predicates. Caller holds m_SettleMutex. Only modules a
    // packaged game ships count: an editor-only module's build never blocks an export.
    bool IsModuleBuildPendingLocked() const;
    bool IsShippedBuildDeferredLocked() const;
    // Keeps m_FailedShippedModules current with a module's latest load outcome.
    // Main thread, caller holds m_SettleMutex.
    void RecordModuleOutcomeLocked(const std::string& moduleId, bool succeeded);

    // Written under m_SettleMutex (WaitForModuleBuilds reads it from other threads).
    bool m_Initialized = false;

    // Fast-build toolchain (Ninja + cached MSVC env), warmed async at Initialize so the
    // first build can use it. BuildOnly applies it to the build config when ready.
    MsvcToolchain m_Toolchain;

    UniquePtr<FileWatchSubscription> m_WatchSubscription;
    std::filesystem::path m_WatchDirectory;

    // Engine build interface + project paths for the async build (set by the editor).
    NativeBuildConfig m_BuildConfig;
    bool m_HaveConfig = false;

    // Package Cpp module configs (topo order), built + loaded before the project module.
    std::vector<NativeBuildConfig> m_PackageConfigs;

    // InitialPackageModulePassComplete state (main thread only): -1 = configs
    // set but the first KickBuild pass hasn't run; >0 = package modules still
    // building on the worker (the completion task decrements); 0 = complete.
    static constexpr int kInitialPassNotRun = -1;
    int m_InitialPackageOutcomesPending = 0;

    // Async build state. m_BuildInFlight serializes builds (one at a time);
    // m_RebuildRequested coalesces edits/opens; Tick kicks once free + past the throttle.
    // The throttle window runs from the last build's COMPLETION (stamped by its
    // main-thread completion task), not its kick — an edit made while a build runs
    // waits only the residual window, never <build duration> + window.
    std::atomic<bool> m_BuildInFlight{false};
    std::atomic<bool> m_RebuildRequested{false};
    std::chrono::steady_clock::time_point m_LastBuildCompleteTime{};
    static constexpr std::chrono::milliseconds kBuildThrottle{2000};
    // One Info per empty state: the project-module skip (zero native sources)
    // logs on the first pass that skips, then stays quiet until sources appear.
    bool m_EmptyProjectSkipLogged = false;

    // "Native module builds" (cap 1): a build runs cmake and the compiler, so its thread
    // waits on other processes for the whole build; as a channel job it holds no compute
    // worker. Created by Initialize (with a job system), released by Shutdown.
    std::unique_ptr<JobSystem::JobChannel> m_BuildChannel;
    // The dispatched build. The JobSystem outlives this manager at shutdown (EngineCore
    // destroys the manager before resetting the pool) and the build captures `this`, so
    // Shutdown sets m_CancelBuild and waits on this handle before tearing anything down.
    // m_CancelBuild is polled by the in-flight cmake build to bail out fast. Main thread.
    JobSystem::TaskHandle m_BuildTask;
    std::atomic<bool> m_CancelBuild{false};

    // Module-build settle state for WaitForModuleBuilds. Tick() and
    // TriggerDeferredHotReloadIfPending() hold m_SettleMutex for their whole body,
    // so a waiter reads the build state only between them: never in the gap
    // between a settled edit and its rebuild request, between a request and its
    // kick, or between a build's completion and its record write. Guarded by it:
    // m_ShippedModulesBuilding names the dispatched build's modules a packaged game
    // ships (the project module and runtime package modules; editor-only modules,
    // m_EditorModuleIds, never ship), empty when none;
    // m_FailedShippedModules the non-editor modules whose latest build failed;
    // m_TickThread the thread that last ran Tick(); m_SettleWaiters the threads
    // inside WaitForModuleBuilds, which Shutdown waits out before the manager can
    // be destroyed; m_ShutDown whether Shutdown ran since the last Initialize.
    // m_DeferredBuildLoads is written under it too.
    std::mutex m_SettleMutex;
    std::condition_variable m_SettleCondition;
    std::vector<std::string> m_ShippedModulesBuilding;
    std::vector<std::string> m_FailedShippedModules;
    bool m_ModuleLoadingSupported = true;
    std::thread::id m_TickThread;
    int m_SettleWaiters = 0;
    bool m_ShutDown = false;

    std::mutex m_MainThreadMutex;
    std::queue<std::function<void()>> m_MainThreadTasks;
    BuildCompletedCallback m_OnBuildComplete;
    BuildStartedCallback m_OnBuildStarted;
    ReloadHook m_OnBeforeReload;
    ReloadHook m_OnAfterReload;
    ModuleRegistrationPurgeHandler m_ModulePurge;
    ModuleReloadReconcileHandler m_ModuleReconcile;
    EditorModulePinReport m_EditorModulePinReport;
    ModuleImageObserver m_ImageObserver;

    // Per-module load generation (main thread): bumped at the top of every real
    // LoadModule attempt, stamped onto everything the module registers.
    std::unordered_map<std::string, std::uint64_t> m_ModuleGenerations;

    // Trailing-edge debounce: coalesce a burst of saves into one settled event.
    // m_PendingChange is atomic so the per-frame Tick() can early-out on the common
    // (no edits) path without taking m_DirtyMutex; the mutex still guards the
    // (m_PendingChange, m_LastChangeTime) pair when an edit is actually pending.
    std::mutex m_DirtyMutex;
    std::atomic<bool> m_PendingChange{false};
    std::chrono::steady_clock::time_point m_LastChangeTime;

    std::atomic<bool> m_HotReloadDeferred{false};
    std::atomic<bool> m_DeferredReloadPending{false};

    // A build dispatched before play-mode began can complete mid-play; loading it then
    // would clear + re-register live systems without OnDestroy/OnStart. The completion
    // task stashes the built DLLs here instead (one entry per module, load order), and
    // TriggerDeferredHotReloadIfPending applies them on play exit. Written on the main
    // thread only (completion task + play-exit flush), under m_SettleMutex, because
    // WaitForModuleBuilds reads it from the export thread.
    struct DeferredBuildLoad
    {
        std::filesystem::path Dll;
        std::string ModuleId;
        std::filesystem::path BuildDir;
        NativeBuildResult Result;
        std::string FullDigest;
        std::string AbiDigest;
        bool Shipped = true; // false for an editor-only module (m_EditorModuleIds)
    };
    std::vector<DeferredBuildLoad> m_DeferredBuildLoads;

    // One mapped module image. The newest generation of each module serves all
    // dispatch; superseded generations are unmapped by
    // UnloadSupersededModuleVersions once the quiesce ledger proves no registry
    // still points into them — otherwise they stay mapped for the process
    // lifetime (loud no-unload, the pre-C12 behavior). Handles are raw because
    // a kept image is intentionally leaked.
    struct LoadedModule
    {
        UserModuleHandle* Handle = nullptr;
        std::filesystem::path Path;
        std::string ModuleId;
        std::uint64_t Generation = 0;
    };

    // After a successful (re)load of `moduleId`, unmap every older-generation
    // image of it whose registrations were fully re-owned (quiesce ledger
    // empty). Editor-kind modules are excluded: live editor UI/factories
    // (panels, VCS integrations) are not covered by the ledger. Set
    // GE_NATIVE_MODULE_NO_UNLOAD=1 to keep every image mapped (safety valve).
    void UnloadSupersededModuleVersions(const std::string& moduleId, std::uint64_t currentGeneration);

    std::vector<LoadedModule> m_LoadedModules;
    ModuleQuiesceCheck m_QuiesceCheck;
    // Module ids whose config carries the EditorSDK interface (editor-kind
    // package modules) — excluded from unload.
    std::unordered_set<std::string> m_EditorModuleIds;
};

} // namespace NativeScripting
} // namespace GameEngine
