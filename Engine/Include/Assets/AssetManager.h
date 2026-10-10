#pragma once

#include "Types/Types.h"
#include "AssetCore/Asset.h"
#include "AssetCore/Result.h"
#include "AssetCore/AssetRegistry.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ExpectedAssetWrite.h" // brings ExpectedWriteLedger with it
#include "FileWatcher/FileWatcher.h"
#include "Assets/FileWatchingService.h"
#include "Assets/ParserRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/TaskHandle.h"
#include <future>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <shared_mutex>

#include <unordered_set>
namespace GameEngine {

// Use fully qualified JobSystem types to avoid conflicts

// Forward declarations
class AssetManager;
class AssetBatchCoordinator;
class AssetIOService;
class TextureCookWorkers;

/**
 * @brief Asset loading result for async operations
 */
struct AssetLoadResult {
    SharedPtr<Asset> LoadedAsset;
    bool Success;
    String ErrorMessage;

    AssetLoadResult(SharedPtr<Asset> a = nullptr, bool s = false, const String& error = "")
        : LoadedAsset(a), Success(s), ErrorMessage(error) {}
};

/**
 * @brief Shared future of one asset load, as returned by LoadAssetAsync.
 *
 * The future resolves with the asset, or nullptr when the load failed, was
 * refused or was cancelled. OnComplete and OnError subscribe to the event
 * dispatcher's AssetLoaded / AssetLoadFailed events for this GUID; loads run on
 * the job system dispatch neither event, so completion is observed through
 * get() or wait_for(), or through LoadAsset's callback.
 */
class AssetFuture {
public:
    /**
     * @brief Constructor
     * @param future The underlying std::future for the asset loading operation
     * @param dispatcher Event dispatcher for callback registration
     * @param assetGuid GUID of the asset being loaded
     */
    AssetFuture(std::shared_future<SharedPtr<Asset>> future, AssetEventDispatcher* dispatcher, const GUID& assetGuid)
        : m_Future(std::move(future)), m_Dispatcher(dispatcher), m_AssetGuid(assetGuid) {}

    /**
     * @brief Get the loaded asset (blocking operation)
     * @return SharedPtr to the loaded asset, or nullptr if loading failed
     */
    SharedPtr<Asset> get() { return m_Future.get(); }

    /**


     * @brief Wait for the loading operation to complete with timeout
     * @param timeout Maximum time to wait
     * @return Status indicating whether the operation completed, timed out, or is deferred
     */
    std::future_status wait_for(const std::chrono::milliseconds& timeout) {
        return m_Future.wait_for(timeout);
    }

    /**
     * @brief Check if the future is valid and associated with a shared state
     * @return True if the future is valid
     */
    bool Valid() const { return m_Future.valid(); }

    /**
     * @brief Call @p callback on an AssetLoaded event for this GUID once the future is ready
     * @param callback Function to call with the loaded asset
     * @return Reference to this AssetFuture for method chaining
     *
     * The destructor unregisters the most recent OnComplete registration only.
     */
    AssetFuture& OnComplete(std::function<void(SharedPtr<Asset>)> callback) {
        if (m_Dispatcher && callback) {
            m_CompleteCallbackHandle = m_Dispatcher->AddCallback([this, callback](const AssetEvent& event) {
                if (event.AssetGuid == m_AssetGuid && event.EventType == AssetEventType::AssetLoaded) {
                    if (m_Future.valid() && m_Future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        callback(m_Future.get());
                    }
                }
            });
        }
        return *this;
    }

    /**
     * @brief Call @p callback on an AssetLoadFailed event for this GUID
     * @param callback Function to call with the error message
     * @return Reference to this AssetFuture for method chaining
     *
     * The destructor unregisters the most recent OnError registration only.
     */
    AssetFuture& OnError(std::function<void(const String&)> callback) {
        if (m_Dispatcher && callback) {
            m_ErrorCallbackHandle = m_Dispatcher->AddCallback([this, callback](const AssetEvent& event) {
                if (event.AssetGuid == m_AssetGuid && event.EventType == AssetEventType::AssetLoadFailed) {
                    callback(event.Message);
                }
            });
        }
        return *this;
    }

    /**
     * @brief Destructor - automatically unregisters any callbacks
     */
    ~AssetFuture() {
        if (m_Dispatcher) {
            if (m_CompleteCallbackHandle != 0) {
                m_Dispatcher->RemoveCallback(m_CompleteCallbackHandle);
            }
            if (m_ErrorCallbackHandle != 0) {
                m_Dispatcher->RemoveCallback(m_ErrorCallbackHandle);
            }
        }
    }

    // Move-only type for safe resource management
    AssetFuture(const AssetFuture&) = delete;
    AssetFuture& operator=(const AssetFuture&) = delete;
    AssetFuture(AssetFuture&&) = default;
    AssetFuture& operator=(AssetFuture&&) = default;

private:
    std::shared_future<SharedPtr<Asset>> m_Future;
    AssetEventDispatcher* m_Dispatcher;
    GUID m_AssetGuid;
    uint32 m_CompleteCallbackHandle = 0;
    uint32 m_ErrorCallbackHandle = 0;
};

/**
 * @brief Tracking handle for one asset load request.
 *
 * Holding the handle is optional: the completion callback passed to LoadAsset
 * is delivered whether or not the caller keeps it, and destroying the handle
 * withdraws nothing. A caller that must not be called back after it dies
 * withdraws the request explicitly with Cancel() while it is still alive.
 */
struct AssetLoadHandle {
    GUID AssetGuid;
    std::optional<std::shared_future<SharedPtr<Asset>>> Future;
    // Withdrawal token shared with the load's registered completion callback.
    // Set only by Cancel(); a set token means the callback is not invoked.
    std::shared_ptr<std::atomic_bool> Cancelled;

    AssetLoadHandle() = default;
    explicit AssetLoadHandle(const GUID& guid)
        : AssetGuid(guid), Cancelled(std::make_shared<std::atomic_bool>(false)) {}

    /// Withdraws the request: the completion callback will not be invoked.
    void Cancel() {
        // seq_cst on the store-load pair so the worker thread observing
        // Cancelled is also guaranteed to see prior writes by the cancelling
        // thread (typical: an owner tearing down the state the callback would
        // otherwise reference). The JobSystem queue mutex plus the
        // shared_ptr/weak_ptr control block do provide happens-before in
        // practice, but we don't want to depend on that implicitly.
        if (Cancelled)
            Cancelled->store(true, std::memory_order_seq_cst);
    }

    bool IsComplete() const {
        return Future.has_value() &&
               Future->valid() &&
               Future->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    SharedPtr<Asset> GetResult() const {
        if (IsComplete()) {
            return Future->get();
        }
        return nullptr;
    }

    // Move-only type
    AssetLoadHandle(const AssetLoadHandle&) = delete;
    AssetLoadHandle& operator=(const AssetLoadHandle&) = delete;
    AssetLoadHandle(AssetLoadHandle&&) noexcept = default;
    AssetLoadHandle& operator=(AssetLoadHandle&&) noexcept = default;
};

using AssetProgressCallback = std::function<void(float progress)>;

/**
 * @brief Asset load completion callback.
 *
 * Receives a Result<SharedPtr<Asset>, AssetError>. Success carries the
 * loaded asset. Failure carries AssetError::Missing when a path overload
 * resolves no GUID, otherwise AssetError::ImportFailed (the load failed, was
 * refused, or was cancelled by an unload). A request withdrawn with
 * AssetLoadHandle::Cancel() is not called back. Use ToString(err) for
 * diagnostics.
 *
 * Example:
 *   manager.LoadAsset(guid, [](Result<SharedPtr<Asset>, AssetError> r) {
 *       if (r.IsOk()) { Use(r.Value()); }
 *       else          { Logger::Warning("load failed: {}", ToString(r.Error())); }
 *   });
 */
using AssetLoadResultCallback = std::function<void(Result<SharedPtr<Asset>, AssetError>)>;

/**
 * @brief How long a load keeps its asset in memory.
 */
enum class AssetResidency : uint8
{
    /// Resident until an explicit unload. GetAsset, LoadAssetAsync and every
    /// LoadAsset without a residency keep an asset this way, and one of them
    /// reaching a transient asset pins it.
    Pinned,
    /// Resident for the requester's work only: ReleaseTransientAsset unloads it
    /// again unless a pinning access reached it since it loaded. For work that
    /// reads an asset once and keeps a derived result, such as a thumbnail bake.
    Transient,
};

struct AssetLoadRequest {
    GUID AssetGuid;
    AssetLoadPriority Priority = AssetLoadPriority::Normal;
    AssetResidency Residency = AssetResidency::Pinned;
    AssetLoadResultCallback OnComplete;
    AssetProgressCallback OnProgress;

    AssetLoadRequest() = default;
    AssetLoadRequest(const GUID& guid, AssetLoadResultCallback callback, AssetLoadPriority prio = AssetLoadPriority::Normal)
        : AssetGuid(guid), Priority(prio), OnComplete(std::move(callback)) {}
};

/**
 * @brief What must be present at a candidate path for it to count as a hit.
 *
 * Selects the filesystem probe ResolveAssetPathPreferringSource applies to the
 * alias-pinned candidate before preferring it over implicit priority.
 */
enum class AssetPathKind {
    /// Any entry counts. For assets that are files.
    AnyEntry,
    /// Only a directory counts. For assets that ARE a directory tree (an
    /// adapter-shader root); a same-named file must fall through, not be
    /// handed to a consumer that will walk into it.
    Directory,
};

/**
 * @brief Manages all assets in the engine
 */
class AssetManager {
public:
    // ========================================
    // THREAD-LOCAL LOADING CONTEXT
    // ========================================
    //
    // Some asset types (e.g. UIStyleAsset) need access to the AssetRegistry/AssetRoot
    // of the AssetManager that is currently performing the load/reload. Using the
    // Engine singleton here is incorrect in tests/tools that construct their own
    // AssetManager instance.
    //
    // AssetManager establishes this context while executing asset load work.
    // ctor/dtor and GetThreadCurrent are defined out-of-line in AssetManager.cpp
    // on purpose: t_threadCurrent is a thread_local static, and a SHARED Engine.dll
    // does not export TLS symbols (WINDOWS_EXPORT_ALL_SYMBOLS skips them). Keeping
    // every t_threadCurrent access inside the .cpp means external TUs (e.g.
    // UIStyleAsset, test targets) link against the exported accessor functions
    // instead of an unresolved TLS data symbol.
    class ScopedThreadAssetManager
    {
      public:
        explicit ScopedThreadAssetManager(AssetManager* current);
        ~ScopedThreadAssetManager();

        ScopedThreadAssetManager(const ScopedThreadAssetManager&) = delete;
        ScopedThreadAssetManager& operator=(const ScopedThreadAssetManager&) = delete;

      private:
        AssetManager* m_Prev = nullptr;
    };

    // Returns the AssetManager currently performing asset load/reload work on this thread.
    // May be null if called outside an AssetManager loading context.
    static AssetManager* GetThreadCurrent();

    AssetManager();
    ~AssetManager();

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /**
     * @brief Infrastructure-only init: parser registry, type registry, batch coordinator.
     * Does NOT register any sources, load any DB, or subscribe any watchers.
     */
    bool Initialize(JobSystem::WorkStealingThreadPool* jobSystem = nullptr);

    /**
     * @brief Convenience: initialize infrastructure + register a "project" source.
     */
    bool Initialize(const std::filesystem::path& assetRoot,
                    JobSystem::WorkStealingThreadPool* jobSystem = nullptr,
                    const std::filesystem::path& authoritativeDbFile = {},
                    const std::filesystem::path& cacheRoot = {});

    /**
     * @brief Shutdown asset manager
     */
    void Shutdown();

    /**
     * @brief Update asset manager (check for changes, process completed tasks)
     */
    void Update();

    // ------------------------------------------------------------------
    // Source management
    // ------------------------------------------------------------------

    /**
     * @brief Get the configured asset root directory (convenience for project source root).
     *
     * Returns by value because the underlying registry holds the SourceEntry
     * via SharedPtr; returning a reference would race Shutdown / source
     * rebind.
     */
    std::filesystem::path GetAssetRoot() const;

    /**
     * @brief Register an asset source. "project" is a valid alias.
     */
    bool RegisterSource(const AssetSourceDesc& source);

    /**
     * @brief Block until a source's startup directory scan finishes (see AssetRegistry::WaitForStartupScan).
     */
    void WaitForStartupScan(std::string_view sourceAlias = std::string_view(kAssetSourceAliasProject));

    /**
     * @brief Begin removing a previously registered asset source. Returns false
     * when the alias names no source, in which case nothing changed and
     * `onUnregistered` never runs.
     *
     * Otherwise the source's assets are ejected and `onUnregistered(ok)` runs
     * when that finishes — **possibly before this call returns**, otherwise from
     * a later AssetManager::Update. The source stays mounted until then, but
     * loads of its assets are refused for the duration, so a waiter that reacts
     * to a cancellation by asking again is answered rather than served from a
     * root that is being abandoned.
     *
     * Main thread only, like Update, which is where the completion runs. A
     * completion never runs once Shutdown has begun.
     */
    bool BeginUnregisterSource(std::string_view sourceAlias, Function<void(bool)> onUnregistered = {});

    /**
     * @brief Rebind a source to a new root (and optionally a new DB location).
     * Ejects loaded assets for the old root, swaps root/DB/watcher, scans the
     * new root. All other sources and their loaded assets are preserved.
     */
    struct SourceRebindDesc
    {
        std::filesystem::path NewRoot;
        std::filesystem::path AuthoritativeDbFile;  // empty = derive from NewRoot
        std::filesystem::path CacheRoot;            // empty = derive from NewRoot
    };

    /**
     * @brief Begin rebinding a source. Returns false when the request is
     * rejected outright — unknown alias, or a new root that is not a directory
     * — in which case nothing changed and `onRebound` never runs.
     *
     * Otherwise the source's in-flight loads are cancelled and `onRebound(ok)`
     * runs once the last of them reports done. **It may run before this call
     * returns** (the usual case: nothing of that source was in flight), or from
     * a later AssetManager::Update. Callers must express what happens next
     * inside `onRebound`, never in statements after this call. Loads of the
     * source's assets are refused until the rebind lands.
     *
     * Main thread only, like Update, which is where the completion runs. A
     * completion never runs once Shutdown has begun.
     */
    bool BeginRebindSource(std::string_view sourceAlias, const SourceRebindDesc& desc,
                           Function<void(bool)> onRebound = {});

    /**
     * @brief Snapshot registered asset sources (in priority order).
     */
    std::vector<AssetSourceDesc> GetRegisteredSources() const;

    /**
     * @brief Monotonic counter bumped whenever the SOURCE SET changes
     * (register / unregister / rebind) — never on mere file events. Lets
     * consumers holding a snapshot derived from the mounts (e.g. the material
     * build context's package shader roots) re-derive only when the mount
     * table actually moved.
     */
    uint64_t GetSourceSetVersion() const { return m_SourceSetVersion.load(std::memory_order_acquire); }

    /**
     * @brief Resolve root directory for a source alias. Returns empty path when alias is unknown.
     */
    std::filesystem::path GetSourceRoot(std::string_view sourceAlias) const;

    /**
     * @brief Infer source alias for an absolute asset path.
     *
     * Returns the first matching registered source alias when the path is under a registered source root,
     * otherwise returns "project" when under the project asset root, or empty when no source matches.
     */
    std::string GetSourceAliasForPath(const std::filesystem::path& assetPath) const;

    /**
     * @brief Resolve an asset path using implicit source priority (project first, then registered sources).
     */
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& assetPath) const;

    /**
     * @brief Resolve an asset path from a specific source alias (no implicit fallback).
     *
     * For AUTHORED aliases — an `editor:Icons/foo.png` URL, an asset reference
     * carrying a source prefix — where an unregistered alias is a mistake worth
     * reporting: this overload logs it (once per alias) and returns empty. Code
     * that pins a hard-coded alias as a *preference* wants
     * ResolveAssetPathPreferringSource instead, where an absent source is a
     * legitimate deployment shape rather than an error.
     */
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& assetPath,
                                           std::string_view sourceAlias) const;

    /**
     * @brief Resolve preferring one source, falling back to implicit priority.
     *
     * The engine's standard "prefer this mount when it actually supplies the
     * asset" resolution: take the candidate pinned to @p sourceAlias when that
     * source is registered AND something of @p candidateKind is there, else
     * resolve through implicit priority ('project' is preferred without being
     * registered — the implicit chain special-cases it). An unregistered
     * @p sourceAlias is NOT an error here and is never logged — the whole point
     * is that the preferred mount is optional (a shipped game has no 'editor'
     * source; a referrer may sit outside every registered source). A PREFIXED
     * @p path ("alias:...") still routes through the explicit overload, which
     * warns once and can return empty for an unknown authored prefix.
     *
     * The result is only as strong as the implicit overload's: it is empty for
     * an empty or unknown-prefixed input and otherwise NEVER empty, because
     * implicit resolution yields the project candidate whether or not it
     * exists. Callers must
     * therefore treat EXISTENCE, not emptiness, as "resolved", and must
     * re-resolve when GetSourceSetVersion() moves — a call made before the
     * preferred source registers returns a fallback that no later call revisits
     * on its own.
     */
    std::filesystem::path ResolveAssetPathPreferringSource(const std::filesystem::path& assetPath,
                                                           std::string_view sourceAlias,
                                                           AssetPathKind candidateKind) const;

    /**
     * @brief Resolve an asset path using the referrer's source first, then implicit priority.
     *
     * Useful for nested references like CSS @import where source-local lookup should win.
     */
    std::filesystem::path ResolveAssetPathFromReference(const std::filesystem::path& assetPath,
                                                        const std::filesystem::path& referrerAssetPath) const;

    /**
     * @brief Resolve and register a GUID for an asset path using implicit source priority.
     */
    GUID ResolveAssetGuid(const std::filesystem::path& assetPath);

    /**
     * @brief Resolve and register a GUID for an asset path from a specific source alias.
     */
    GUID ResolveAssetGuid(const std::filesystem::path& assetPath, std::string_view sourceAlias);

    /**
     * @brief Resolve and register a GUID using referrer source context first.
     *
     * Useful for dependency paths (for example CSS @import) where the dependency
     * should stay in the same source namespace as the importer when possible.
     */
    GUID ResolveAssetGuidFromReference(const std::filesystem::path& assetPath,
                                       const std::filesystem::path& referrerAssetPath);

    /**
     * @brief Report that a GUID or asset path a packaged game asked for is not in it.
     *
     * A packaged game mounts its content from shipped manifests, and an export ships
     * only what its scenes reference plus each package's Shaders/ folder and listed
     * runtimeAssets, so an asset a script or a package's code loads with no reference
     * is missing there. Logs one warning per id that states the fix; does nothing
     * outside a packaged game, where a miss has other causes. LoadAssetAsync, LoadAsset
     * and the path overloads of LoadAsset report their misses themselves; code that
     * resolves an asset path to a GUID itself calls this when the path resolves nowhere.
     *
     * @param assetId The GUID string or asset path that resolved to nothing
     */
    void ReportAssetNotInBuild(std::string_view assetId);

    // ========================================
    // ASSET LOADING API
    // ========================================

    /**
     * @brief Load a single asset and return its shared future.
     *
     * The future is ready on return when the asset is resident, its loads are
     * suppressed or its GUID has no metadata; a request for a GUID already in
     * flight joins that load. Without a job system the load runs on the
     * calling thread before this returns.
     *
     * @param guid Asset GUID to load
     * @param priority Orders the read in the AssetIOService queue
     * @return AssetFuture sharing the load's result
     */
    AssetFuture LoadAssetAsync(const GUID& guid,
                              AssetLoadPriority priority = AssetLoadPriority::Normal);

    /**
     * @brief Move an asset's file binding: the registry keeps its GUID under the new
     * path and a resident instance follows, so a later reload reads the new file.
     * The caller has already moved the file. Owner thread only. False when the old
     * path was not a registered asset (or the rename crossed a source root).
     */
    bool RenameAssetPath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);

    /**
     * @brief Reload a loaded asset while establishing this AssetManager as the thread-local context.
     *
     * Owner thread only. Publishes no event: the caller owns what the reload
     * means (a material caller picks its own compile mode, a UI layout reload
     * announces itself to the other UI managers), so it dispatches whatever
     * event its consumers need. Observers must not reload in response.
     *
     * Failed when the GUID names no resident asset, otherwise the asset's own
     * Reload() outcome — Deferred means the file read back empty and the live
     * payload was kept, so the caller may ask again once the save completes.
     */
    ReloadOutcome ReloadAssetNow(const GUID& guid);

    /**
     * @brief Queue an async reload of a loaded asset (decode on a worker, adopt
     * on the main thread — the same pipeline file-watch reloads use). For
     * settings-driven payload rebuilds (e.g. texture cook meta changes) where a
     * synchronous ReloadAssetNow would hitch the caller. While the asset's first
     * load is in flight the request is kept and the reload starts once that load
     * lands (Update), because the load may already have read the settings the
     * request is about. No-op when the asset is neither resident nor loading.
     * Returns true when a reload was queued or kept for the load.
     */
    bool RequestAsyncReload(const GUID& guid);

    /**
     * @brief True while an async reload of @p guid is reading and decoding — from
     * the moment it is queued until its payload is adopted. The AssetReloaded
     * event is dispatched after the entry is cleared, so a handler of that event
     * reads false. A request RequestAsyncReload kept for an in-flight first load
     * reads false until that load lands and the reload starts. For a panel that
     * has to say the payload it shows is being rebuilt.
     */
    bool IsReloadInFlight(const GUID& guid) const;

    /**
     * @brief Announce a write of @p file that is about to happen.
     *
     * Every writer of a file this manager tracks announces it, writes it, then reports
     * it through the returned handle. Reporting is what drives the change: registry
     * metadata sync, asset-changed dispatch, dependent fan-out and the pending-reload
     * mark, on the reporting thread, on every host. A file watcher, where one runs,
     * reports the same write a moment later; that report matches the announcement and
     * is dropped, so one save drives the pipeline once whether or not the host watches
     * files at all.
     *
     * The announcement must precede the write: on a watched host the watcher can report
     * a new file appearing before its writer does.
     *
     * **Threading**: announce, write and report on the writer's own thread; the only
     * work the report does inline is the registry sync and the event dispatch, exactly
     * as a watcher-thread change does. The reload it marks is drained on the main
     * thread by Update() as before, so reporting a save does not import inside the save.
     */
    [[nodiscard]] ExpectedAssetWrite ExpectWrite(const std::filesystem::path& file);

    /**
     * @brief Announce a write of every file under @p root.
     *
     * For writes whose files cannot be named in advance because producing them is what
     * discovers them — copying a directory into the project. Otherwise identical to
     * ExpectWrite: report each file the write produced through the returned handle.
     */
    [[nodiscard]] ExpectedAssetWrite ExpectWritesUnder(const std::filesystem::path& root);

    /**
     * @brief Unload an asset on a job-system worker, withdrawing its in-flight load first
     * @param guid Asset GUID to unload
     * @return TaskHandle of the unload task; invalid when the manager is not initialized or has no job system
     */
    JobSystem::TaskHandle UnloadAssetWithDependencies(const GUID& guid);

    /**
     * @brief Load a single asset with a Result<>-based completion callback.
     *
     * Fire-and-forget asset loading. The callback receives a typed
     * Result<SharedPtr<Asset>, AssetError> — success carries the asset,
     * failure carries an AssetError enum.
     *
     * @param guid Asset GUID to load
     * @param callback Function called when loading completes (success or failure)
     * @param priority Orders the read in the AssetIOService queue
     * @return AssetLoadHandle for tracking and optional cancellation
     *
     * **Delivery**: The callback is invoked once, whether the asset is already
     * resident, is loaded by this request, or joins a load another caller
     * started. Keeping the returned handle is optional and does not affect
     * delivery.
     * **Threading**: The callback runs on the thread that completes the load —
     * a JobSystem worker, or the calling thread when the result is already
     * available on entry. UI code should post to the UI thread before touching
     * UI state.
     * **Cancellation**: Explicit. Call Cancel() on the handle to withdraw the
     * request before it completes; destroying the handle does not.
     *
     * **Example Usage**:
     * ```cpp
     * auto handle = assetManager.LoadAsset(modelGuid,
     *     [](Result<SharedPtr<Asset>, AssetError> r) {
     *         if (r.IsOk()) Logger::Log::Info("Model loaded: {}", r.Value()->GetName());
     *         else          Logger::Log::Error("Loading failed: {}", ToString(r.Error()));
     *     }, AssetLoadPriority::High);
     * ```
     */
    AssetLoadHandle LoadAsset(const GUID& guid,
                                    AssetLoadResultCallback callback,
                                    AssetLoadPriority priority = AssetLoadPriority::Normal);

    /**
     * @brief Load an asset by path using implicit source priority.
     */
    AssetLoadHandle LoadAsset(const std::filesystem::path& assetPath,
                                    AssetLoadResultCallback callback,
                                    AssetLoadPriority priority = AssetLoadPriority::Normal);

    /**
     * @brief Load an asset by path from a specific source alias.
     */
    AssetLoadHandle LoadAsset(const std::filesystem::path& assetPath,
                                    std::string_view sourceAlias,
                                    AssetLoadResultCallback callback,
                                    AssetLoadPriority priority = AssetLoadPriority::Normal);

    /**
     * @brief Load a single asset from a request structure (GUID, priority,
     * completion callback). The request's OnProgress is not invoked.
     */
    AssetLoadHandle LoadAsset(const AssetLoadRequest& request);

    /**
     * @brief Unload an asset by GUID, withdrawing its in-flight load first.
     * @param guid Asset GUID to unload
     * @return Without a job system, a ready future: the unload ran on the
     *         calling thread. With one, the unload runs as a worker task the
     *         future is not tied to: the future is ready on return and get()
     *         throws std::future_error (broken_promise), so waiting on it does
     *         not wait for the unload.
     */
    std::future<void> UnloadAssetAsync(const GUID& guid);

    /**
     * @brief Explicitly release an asset that only transient requests have reached.
     *
     * Called by the thumbnail owner after its work completes. This is not an
     * LRU, age-based eviction, or memory-pressure sweep.
     *
     * The asset leaves memory when AssetResidency::Transient requests loaded it
     * and no pinning access (GetAsset, LoadAssetAsync, a pinned LoadAsset)
     * reached it since. The manager cannot see who else still works with it:
     * its own finished load tasks keep references for a while, so a reference
     * count says nothing. The caller drops its own references first, and
     * transient requesters of one asset agree among themselves on the last one
     * to release it. The unload raises AssetUnloaded, so the caches that mirror
     * the asset (GPU meshes, textures) drop their copies.
     *
     * @return true when the asset was unloaded.
     */
    bool ReleaseTransientAsset(const GUID& guid);

    /**
     * @brief Get loaded asset by GUID (returns nullptr if not loaded)
     * @param guid Asset GUID
     * @return Loaded asset or nullptr. The asset stays pinned (AssetResidency).
     */
    SharedPtr<Asset> GetAsset(const GUID& guid) const;

    /**
     * @brief Check if asset is currently loaded
     * @param guid Asset GUID
     * @return True if asset is loaded in memory
     */
    bool IsAssetLoaded(const GUID& guid) const;

    /**
     * @brief Get asset registry
     */
    AssetRegistry& GetRegistry() { return m_Registry; }
    const AssetRegistry& GetRegistry() const { return m_Registry; }

    /**
     * @brief Get parser registry
     */
    ParserRegistry& GetParserRegistry() { return m_ParserRegistry; }
    const ParserRegistry& GetParserRegistry() const { return m_ParserRegistry; }

    /**
     * @brief Get asset type registry (from AssetCore)
     */
    AssetTypeRegistry& GetAssetTypeRegistry() { return m_Registry.GetTypeRegistry(); }
    const AssetTypeRegistry& GetAssetTypeRegistry() const { return m_Registry.GetTypeRegistry(); }

    /**
     * @brief Enable/disable hot reloading
     */
    void SetHotReloadEnabled(bool enabled);

    /**
     * @brief Check if hot reload is enabled
     */
    bool IsHotReloadEnabled() const { return m_HotReloadEnabled; }

    /**
     * @brief Memory usage statistics of the resident assets
     */
    struct MemoryStats {
        uint64 TotalMemoryUsed;
        uint64 LoadedAssetCount;
        HashMap<AssetType, uint64> MemoryByType;
        std::chrono::steady_clock::time_point LastUpdated;
    };

    /**
     * @brief Get the cached memory statistics. RegisterLoadedAsset and
     * UnregisterLoadedAsset update the cache incrementally; the per-type map is
     * copied under m_StatsCacheMutex.
     */
    MemoryStats GetMemoryStats() const;

    /**
     * @brief Force recalculation of memory statistics (O(n), use sparingly)
     * Only needed for debugging or when cache coherency is suspected
     */
    MemoryStats RecalculateMemoryStats() const;

    /**
     * @brief Get the asset event dispatcher for advanced usage
     */
    AssetEventDispatcher& GetEventDispatcher() { return m_EventDispatcher; }

    /**
     * @brief Get the job system for task submission
     */
    JobSystem::WorkStealingThreadPool& GetJobSystem() { return *m_JobSystem; }

    /**
     * @brief The workers texture cooks spread their encode across (CookTexture's
     * `workers`), within the asset decode gate's texture share; null when the job
     * system runs inline.
     */
    TextureCookWorkers* GetTextureCookWorkers() { return m_TextureCookWorkers.get(); }

    /**
     * @brief Number of loads currently tracked as in flight (dedupe map size).
     * Diagnostic surface; also used by tests to assert load conservation.
     */
    size_t GetInFlightLoadCount() const;

public:
    /**
     * @brief Create asset instance based on type (made public for async tasks)
     */
    SharedPtr<Asset> CreateAsset(const AssetMetadata& metadata);

    /**
     * @brief Query whether load attempts are currently suppressed for a GUID.
     *
     * This is a read-only view into the AssetManager's suppression cache (used to avoid
     * hammering unsupported or repeatedly failing assets). External systems that poll
     * assets (e.g. render-pipeline selection) can use this to avoid retrying every frame
     * and to surface the suppression reason.
     */
    bool IsLoadSuppressed(const GUID& guid, String* outReason = nullptr) const;

    /**
     * @brief Suppress repeated load attempts for a GUID until the source file changes.
     *
     * This is used by background decode jobs to prevent "error loops"
     * where the host keeps requesting the same asset every frame and it keeps failing.
     * The suppression is automatically cleared when the underlying asset file changes.
     */
    void SuppressLoadRetries(const GUID& guid, const AssetMetadata& metadata, const String& reason);

    /**
     * @brief Clear load suppression for a GUID (e.g. after registry path sync or explicit user retry).
     */
    void ClearLoadSuppressed(const GUID& guid);

    /**
     * @brief Asset registration helpers (thread-safe, made public for async tasks)
     */
    void RegisterLoadedAsset(const GUID& guid, SharedPtr<Asset> asset);
    void UnregisterLoadedAsset(const GUID& guid);

    /**
     * @brief Record the merged decode job's TaskHandle on the in-flight entry
     * (made public for AssetBatchCoordinator; called from the I/O thread right
     * after the continuation is submitted). The handle is the load's
     * cancellation surface. No-op when the load already resolved.
     */
    void SetInFlightDecodeHandle(const GUID& guid, const JobSystem::TaskHandle& handle);

private:
    static thread_local AssetManager* t_threadCurrent;

    friend class ExpectedAssetWrite;

    /**
     * @brief Handle a file change the watcher reported.
     *
     * The one place the two sources of change meet: a change that repeats a write this
     * process announced has already been delivered by the writer's own report and stops
     * here. Everything else is an edit nobody in this process made and is applied.
     */
    void OnWatchedFileChanged(const FileChangeEvent& event, std::string_view sourceAliasHint = {});

    /**
     * @brief Apply a file change: registry sync, asset event, dependent fan-out, reload mark.
     *
     * Runs on whichever thread reported the change — the file watcher's for an external
     * edit, the writer's for a save. Only the reload is deferred (m_PendingReloads,
     * drained on the main thread by Update); everything here runs inline.
     */
    void ApplyFileChange(const FileChangeEvent& event, std::string_view sourceAliasHint = {});

    /// Drive the pipeline for a file a writer just produced, and remember the bytes it
    /// left so the watcher's report of the same write is recognisable. See ExpectWrite.
    void ReportExpectedWrite(const std::filesystem::path& written);

    /// Stop expecting an announced write. See ExpectedAssetWrite's destructor.
    void RetireExpectedWrite(ExpectedWriteLedger::Registration id);

    /**
     * @brief Check whether an asset can be created/loaded with the currently registered factories/parsers.
     * Used to avoid repeatedly retrying unsupported extensions (e.g., no parser and no registered type factory).
     */
    bool IsAssetSupportedForLoad(const AssetMetadata& metadata, String* outReason = nullptr) const;

    /**
     * @brief Suppress repeated load attempts for assets that failed or are unsupported.
     */
    void MarkLoadSuppressed(const GUID& guid, const AssetMetadata& metadata, const String& reason);
    void MarkLoadSuppressed(const GUID& guid, const String& reason);

    /**
     * @brief Suppress repeated load attempts for a load that produced no asset and
     * reported no reason. A reason already recorded for the GUID stays.
     */
    void MarkLoadFailed(const GUID& guid);

    /**
     * @brief Process completed async tasks (called from Update)
     */
    void ProcessCompletedTasks();

    /**
     * @brief Check for assets that need reloading
     */
    void CheckForReloads();

    /**
     * @brief Dispatch an asset event with its RedirectSources filled, so an invalidator reaches a
     * cache that holds the asset under a GUID redirected to it. Safe from any thread.
     */
    void DispatchWithRedirectSources(AssetEvent event);

    /**
     * @brief Start the reloads RequestAsyncReload kept for first loads that
     * have since landed; drop those whose load failed or was cancelled.
     * Main thread only (called from Update).
     */
    void StartReloadsRequestedDuringLoad();

    /**
     * @brief Kick the async reload pipeline for one changed loaded asset:
     * read on the AssetIOService, decode into a staged instance on a
     * Background pool job, then adopt in place on the main thread via
     * DrainCompletedReloads. A newer change for the same GUID supersedes the
     * in-flight generation (drop/restart) instead of queueing a duplicate.
     * File-watch and timestamp-poll marks whose mtime hasn't advanced past
     * the in-flight generation are dropped (the poll re-notices the same
     * version every tick until the swap refreshes the recorded mtime).
     * RequestAsyncReload passes restartSameMtime: inspector kv edits do
     * not bump mtime and must still restart. Main thread only.
     */
    void StartAsyncReload(const GUID& guid, const SharedPtr<Asset>& asset, bool watchReason,
                          bool restartSameMtime = false);

    /**
     * @brief Adopt staged async-reload payloads into their live assets (main
     * thread, called from CheckForReloads). Stale generations and completions
     * for since-unloaded assets are dropped. Appends the resulting
     * AssetReloaded/AssetLoadFailed events to pendingEvents; the caller
     * dispatches them after all reload work, matching the inline path.
     */
    void DrainCompletedReloads(std::vector<AssetEvent>& pendingEvents);

    /**
     * @brief Balance one StartAsyncReload pipeline (exactly-once per
     * pipeline, from its decode job or its read-failure callback). Shutdown
     * waits for the count to reach zero so no reload work can touch a dead
     * manager.
     */
    void FinishReloadJob();

    /**
     * @brief Fallback synchronous load for managers without a job system.
     */
    std::future<SharedPtr<Asset>> LoadAssetSynchronous(const AssetMetadata& metadata);
    std::future<Vector<SharedPtr<Asset>>> LoadAssetsSequential(const Vector<GUID>& guids, AssetLoadPriority priority);

    // A per-request completion callback awaiting an in-flight load, paired with
    // the originating handle's cancel flag. These are fired as a CONTINUATION
    // from the load-completion path (FirePendingLoadCallbacks, driven by
    // EraseInFlightLoadGeneration) rather than by a pool task that blocks on the
    // decode future: a worker parked on a future whose producer (the decode job)
    // runs on the SAME pool starves the pool -- the CreateLoadHandle
    // futureWaitTask deadlock.
    struct PendingLoadCallback
    {
        AssetLoadResultCallback Callback;
        std::shared_ptr<std::atomic_bool> Cancelled;
    };

    // Invoke drained continuation callbacks with the completed asset (or an
    // ImportFailed error when null), skipping any whose handle was cancelled.
    // Static (touches no manager state) so the "resolved future =>
    // manager done with its state" shutdown invariant holds even though callers
    // fire AFTER resolving the promise; always called outside m_InFlightMutex.
    // Fire-after is deliberate: a callback that (transitively) blocks on its own
    // asset's future must not wedge the completion thread before resolution.
    static void FirePendingLoadCallbacks(std::vector<PendingLoadCallback>& pending, const SharedPtr<Asset>& asset);

    // Unconditionally erase the in-flight entry for `guid`, resolve its promise
    // with `result`, then drain and fire its pending continuation callbacks
    // (fire-after, outside the lock). Used by the synchronous teardown paths
    // (metadata miss / unsupported / no-job-system) where a concurrent dedupe
    // joiner may have registered a callback that must not be dropped.
    void ResolveAndEraseInFlight(const GUID& guid, const SharedPtr<Asset>& result);

    /**
     * @brief The single submission route: hand a freshly-created in-flight
     * load to the AssetBatchCoordinator on the calling thread and wire its
     * completion callbacks to the entry's shared promise. The coordinator
     * hand-off is a cancel check plus an AssetIOService queue push and does
     * not block, so by the time this returns the load is in the IO priority
     * queue (or already resolved); no "accepted but unsubmitted" state exists.
     */
    void SubmitInFlightLoad(const AssetMetadata& metadata, AssetLoadPriority priority,
                            std::shared_ptr<std::promise<SharedPtr<Asset>>> promise,
                            std::shared_ptr<std::atomic<bool>> completed,
                            std::shared_ptr<std::atomic<bool>> cancelRequested);

    // What an erased in-flight entry hands to its completion: the continuation
    // callbacks, and whether every request that reached the load was transient.
    struct ErasedInFlightLoad
    {
        std::vector<PendingLoadCallback> Callbacks;
        bool Transient = false;
    };

    /**
     * @brief Erase the in-flight entry for a GUID only if it still belongs to
     * this load generation (identified by its Completed flag). Keeps a stale
     * completion callback — e.g. a cancelled load whose claimed read finishes
     * after an eject already erased the entry and a fresh load re-created it —
     * from erasing the newer generation out from under its own pipeline and
     * the shutdown drain. Returns the entry's pending continuation callbacks
     * (moved out under the lock) so the caller can fire them after publishing
     * the result, and its residency; returns empty and pinned if the generation
     * no longer matches.
     */
    ErasedInFlightLoad EraseInFlightLoadGeneration(const GUID& guid, const std::shared_ptr<std::atomic<bool>>& completed);

    // True while every request that reached the in-flight load of `guid` was
    // AssetResidency::Transient; false when no load of it is in flight.
    bool IsInFlightLoadTransient(const GUID& guid) const;

    // A pinning access reached `asset` (see AssetResidency).
    void PinResidency(const GUID& guid, const SharedPtr<Asset>& asset);

    /**
     * @brief Internal helper for creating enhanced futures
     */
    AssetFuture CreateEnhancedFuture(std::shared_future<SharedPtr<Asset>> future, const GUID& assetGuid);

    /**
     * @brief Wrap a load's shared future in a handle and attach @p callback.
     *
     * While the load is in flight the callback joins the entry's continuation
     * list (FirePendingLoadCallbacks); otherwise it runs at once on the calling
     * thread. A null result is delivered as AssetError::ImportFailed.
     */
    AssetLoadHandle CreateLoadHandle(const GUID& assetGuid, AssetLoadResultCallback callback,
                                   std::shared_future<SharedPtr<Asset>> future);

    /**
     * @brief Update cached memory statistics (called when cache is stale)
     */
    void UpdateMemoryStatsCache() const;

    /**
     * @brief Incrementally update memory stats when assets are added/removed
     */
    void UpdateMemoryStatsIncremental(const SharedPtr<Asset>& asset, bool added) const;

    /**
     * @brief Get loaded asset by GUID (thread-safe, optimized for callbacks)
     * @param guid Asset GUID to retrieve
     * @return SharedPtr to asset if loaded, nullptr otherwise
     */
    SharedPtr<Asset> GetLoadedAsset(const GUID& guid, bool pinResidency = false) const;





    /**
     * @brief Cancel an in-flight load: set the entry's CancelRequested flag,
     * remove any still-queued read from the AssetIOService, and Cancel the
     * decode handle (F12: wins only while queued). Work already running
     * completes and its result is discarded by the flag checks; waiters
     * resolve with nullptr through the existing failure path, without
     * poisoning m_SuppressedLoads. Idempotent.
     * @return True when an in-flight entry existed for the GUID.
     */
    bool CancelInFlightLoad(const GUID& guid);

    /**
     * @brief E5: consume a registry GUID remap — atomically re-key the
     * loaded-asset and in-flight maps (plus pending reloads / suppression)
     * from oldGuid to newGuid, then dispatch a GuidRemapped event. The
     * registry stays the only owner of the alias itself: a pipeline that
     * captured oldGuid before the remap resolves it through
     * AssetRegistry::ResolveSessionAlias at completion, which reaches the
     * re-keyed entry only while oldGuid has no primary record. A load that
     * spans this remap AND a new file claiming oldGuid's old path completes
     * against oldGuid as a primary: its result is stored under the re-claimed
     * GUID, the entry re-keyed to newGuid leaks with the callbacks joined on
     * it, and an async reload posted under oldGuid is dropped at drain.
     * The fix is for the entry's key to travel with the pipeline instead of
     * being re-resolved at completion; it is a recorded follow-up, not done.
     */
    void OnGuidRemapped(const GUID& oldGuid, const GUID& newGuid, const std::filesystem::path& path);

    /**
     * @brief Drop every memoized implicit path resolution (E4).
     *
     * Called on any source-set change (register/unregister/rebind) and on
     * every applied file change (ApplyFileChange). Conservative whole-memo invalidation:
     * correctness over cleverness — the memo refills in a handful of
     * resolutions.
     */
    void InvalidateResolveMemo();

    /**
     * @brief Build the candidate path for one source, without judging the alias.
     *
     * Empty return means the alias names no source; 'project' always resolves
     * (it is the asset root, whether or not it is also registered), and a
     * registered source always has a non-empty root, so the caller can read an
     * empty result as exactly "unknown alias" and decide for itself whether
     * that is worth logging. Takes pre-normalized inputs.
     */
    std::filesystem::path ResolveAssetPathInSource(const std::filesystem::path& normalizedRelativeAssetPath,
                                                   const std::string& normalizedAlias) const;

    /**
     * @brief Begin ejecting all loaded assets belonging to a source.
     *
     * Cancels the source's in-flight loads without waiting: queued reads and
     * queued decode jobs die immediately and their waiters resolve with
     * nullptr. Loads already claimed by a reader or running as a decode job
     * cannot be withdrawn, and the eject must not finish before they stop
     * touching the source's assets, so the rest — dropping the assets, the
     * suppressed loads and the pending reloads, and dispatching AssetDestroyed
     * — runs when the last of those futures reports ready. That is inside this
     * call when nothing was in flight, and from AssetManager::Update otherwise.
     * No thread is ever blocked on another thread's progress.
     */
    void BeginEjectAssetsForSource(std::string_view sourceAlias, Function<void()> onDrained = {});

private:
    struct RegisteredAssetSource
    {
        AssetSourceDesc Desc;
        std::optional<FileWatchSubscription> WatchSubscription;
    };

    // One entry per eject that could not finish inside its own call. Main
    // thread only: every API that reaches BeginEjectAssetsForSource is a
    // main-thread API, and Update drains these from the main thread.
    struct PendingSourceEject
    {
        // One load the eject has to outlast, kept with its GUID so a stall can
        // name what it is waiting for and with its completion flag so the
        // finish can tell this generation from a later load of the same GUID.
        struct OutstandingLoad
        {
            GUID Guid;
            std::shared_ptr<std::atomic<bool>> Completed;
            std::shared_future<SharedPtr<Asset>> Future;
        };

        std::string SourceAlias;
        std::vector<GUID> Guids;
        std::vector<OutstandingLoad> Outstanding;
        Function<void()> OnDrained;
        std::chrono::steady_clock::time_point Started{};
        bool Warned = false;
    };

    /// True while an eject of this source is recorded and has not drained.
    /// A second one would share the first's suppression window and leave the
    /// order of the two continuations undefined, so the Begin* entry points
    /// refuse it.
    bool IsEjectPendingForSource(std::string_view normalizedAlias) const;

    /// True once every recorded future reports ready, tested without blocking.
    bool IsEjectDrained(const PendingSourceEject& eject) const;

    /// Steps that must not run while a claimed read or a running decode can
    /// still register one of the source's assets: drop the in-flight entries,
    /// the loaded assets, the suppressed loads and the pending reloads, then
    /// dispatch AssetDestroyed.
    void FinishEject(const PendingSourceEject& eject);

    /// Finish every drained eject. Called from Update, and once from Shutdown
    /// after its blocking drain so no eject is left half-applied.
    void DrainPendingEjects();

    /// Say once, in the log, that an eject is outlasting loads that are not
    /// finishing. Nothing forces it to complete: a running decode may still
    /// register the asset the eject is about to drop, which is the whole
    /// reason the wait exists.
    void ReportStalledEject(PendingSourceEject& eject);

    /// Refuse loads of a source's assets for as long as its eject is pending.
    /// Without this a request that arrives inside the window resolves against
    /// the outgoing root, and the eject's finish would then drop its callbacks
    /// and leave its result resident under a GUID the incoming root owns. One
    /// log line for the set, not one per asset: this is bookkeeping, not the
    /// repeated-failure suppression MarkLoadSuppressed reports.
    void SuppressLoadsWhileEjecting(const std::vector<GUID>& guids);

    /// The registry rebind, watcher swap and descriptor update, run once the
    /// old root's assets are gone.
    bool CompleteRebindSource(const std::string& normalizedAlias, const SourceRebindDesc& desc);

    std::vector<PendingSourceEject> m_PendingEjects;

    bool m_Initialized = false;
    std::vector<RegisteredAssetSource> m_AssetSources;
    std::atomic<uint64_t> m_SourceSetVersion{0};
    AssetRegistry m_Registry;
    ParserRegistry m_ParserRegistry;
    bool m_HotReloadEnabled = false;

    // Event system
    AssetEventDispatcher m_EventDispatcher;

    // Job system integration
    JobSystem::WorkStealingThreadPool* m_JobSystem;

    // Asset storage — read-heavy (main-thread GetAsset/IsAssetLoaded); writers are
    // worker-thread inserts from RegisterLoadedAsset plus erases from Unload paths.
    struct LoadedAssetEntry
    {
        SharedPtr<Asset> Instance;
        mutable std::atomic<bool> Transient{false};

        LoadedAssetEntry() = default;
        LoadedAssetEntry(SharedPtr<Asset> asset, bool transient)
            : Instance(std::move(asset)), Transient(transient) {}
        LoadedAssetEntry(LoadedAssetEntry&& other) noexcept
            : Instance(std::move(other.Instance)), Transient(other.Transient.load()) {}
        LoadedAssetEntry& operator=(LoadedAssetEntry&& other) noexcept
        {
            Instance = std::move(other.Instance);
            Transient.store(other.Transient.load());
            return *this;
        }
        void Pin() const
        {
            // Shared map readers can promote concurrently; avoid dirtying the
            // entry on the normal already-pinned lookup path.
            if (Transient.load(std::memory_order_relaxed))
                Transient.store(false);
        }
    };
    HashMap<GUID, LoadedAssetEntry> m_LoadedAssets;
    mutable std::shared_mutex m_LoadedAssetsMutex;
    // Centralized in-flight dedupe shared across all APIs
    struct InFlightEntry {
        std::shared_ptr<std::promise<SharedPtr<Asset>>> Promise; // underlying promise fulfilled by the loader
        std::shared_future<SharedPtr<Asset>> Future;             // shared among all requesters
        AssetLoadPriority Priority = AssetLoadPriority::Normal;  // highest requested priority before submission
        bool Submitted = false;                                   // whether a load has been submitted
        std::shared_ptr<std::atomic<bool>> Completed;            // guards against double fulfillment
        JobSystem::TaskHandle DecodeHandle;                      // merged decode job; the load's cancellation surface
        std::shared_ptr<std::atomic<bool>> CancelRequested;      // set by cancel triggers; read/decode results are discarded once set
        std::vector<PendingLoadCallback> Callbacks;              // continuation callbacks fired on completion (no parked pool waiter)
        bool Transient = false;                                  // every request that reached this load was AssetResidency::Transient
    };
    HashMap<GUID, InFlightEntry> m_InFlight;
    mutable std::mutex m_InFlightMutex;
    // RequestAsyncReload calls that arrived while the asset's first load was in flight
    // (StartReloadsRequestedDuringLoad). Guarded by m_InFlightMutex, like the table it is
    // checked against.
    std::unordered_set<GUID> m_ReloadsRequestedDuringLoad;
    // Writes announced by this process, so a watcher's report of one is recognisable as
    // that write arriving rather than as a second change. See ExpectWrite.
    ExpectedWriteLedger m_ExpectedWrites;
    // Pending hot-reloads marked by the change pipeline; drained on main thread in CheckForReloads
    std::unordered_set<GUID> m_PendingReloads;
    std::mutex m_ReloadMutex;
    std::chrono::steady_clock::time_point m_LastTimestampReloadCheck{};
    std::vector<GUID> m_TimestampReloadScanList;
    size_t m_TimestampReloadScanIndex = 0;

    // Async reload pipeline (SupportsAsyncReload asset types). An entry lives
    // while a reload read/decode is in flight for a loaded asset; a newer
    // change bumps Generation and cancels the old flag (supersede) rather
    // than queueing a duplicate. Entries are erased when their generation's
    // completion is drained (or the asset is ejected/shut down).
    struct ActiveReloadEntry
    {
        uint64 Generation = 0;
        std::shared_ptr<std::atomic<bool>> CancelRequested;
        std::filesystem::file_time_type SubmittedMtime{};
    };
    HashMap<GUID, ActiveReloadEntry> m_ActiveReloads; // guarded by m_ActiveReloadMutex
    mutable std::mutex m_ActiveReloadMutex;

    // Payloads decoded off-thread, awaiting the main-thread in-place adopt in
    // DrainCompletedReloads. Null Staged = read/decode failure (the live
    // asset keeps its previous payload).
    struct CompletedReloadEntry
    {
        GUID Guid;
        uint64 Generation = 0;
        SharedPtr<Asset> Staged;
        String Error;
        double OffThreadMs = 0.0;
    };
    std::vector<CompletedReloadEntry> m_CompletedReloads; // guarded by m_CompletedReloadMutex
    std::mutex m_CompletedReloadMutex;

    // Count of reload pipelines whose read/decode may still touch this
    // manager. Shutdown waits for zero after stopping the IO service — the
    // engine destroys the manager before the JobSystem pool.
    std::atomic<size_t> m_ActiveReloadJobCount{0};
    std::mutex m_ReloadDrainMutex;
    std::condition_variable m_ReloadDrainCv;

    // E4: memo for ResolveAssetPath's implicit fallthrough, which probes
    // fs::exists once per source per call. Keyed by the caller's relative
    // path string; only resolutions whose candidate EXISTED are cached (a
    // miss must re-probe so newly created files are found). Invalidated
    // wholesale on source-set changes and watched file events.
    mutable std::shared_mutex m_ResolveMemoMutex;
    mutable FastHashMap<std::string, std::filesystem::path> m_ResolveMemo;

    // Aliases whose explicit resolution already warned about being
    // unregistered — the miss is logged once per alias, not per call.
    mutable std::mutex m_UnknownAliasWarnMutex;
    mutable std::unordered_set<std::string> m_UnknownAliasesWarned;

    // True when the project mounted from a shipped .assetmanifest (a packaged game):
    // the condition under which ReportAssetNotInBuild warns. Set once in Initialize.
    bool m_ContentIsPackaged = false;
    // Ids ReportAssetNotInBuild already warned about — one warning per id.
    std::mutex m_NotInBuildWarnMutex;
    std::unordered_set<std::string> m_NotInBuildWarned;

    // Assets whose loads are suppressed (unsupported type, or a previous load attempt failed).
    // Cleared automatically when the underlying file changes on disk.
    struct SuppressedLoadInfo
    {
        String extension;
        String reason;
        std::chrono::steady_clock::time_point firstSeen{};
    };
    mutable std::mutex m_SuppressedLoadMutex;
    HashMap<GUID, SuppressedLoadInfo> m_SuppressedLoads;


    // Cached memory statistics: the totals are atomics, the per-type map is
    // read and written under m_StatsCacheMutex.
    mutable std::atomic<uint64> m_CachedTotalMemory{0};
    mutable std::atomic<uint64> m_CachedAssetCount{0};
    mutable std::atomic<bool> m_StatsNeedUpdate{true};
    mutable HashMap<AssetType, std::atomic<uint64>> m_CachedMemoryByType;
    mutable std::mutex m_StatsCacheMutex;

    // Async task tracking
    Vector<std::future<void>> m_PendingTasks;
    std::mutex m_PendingTasksMutex;

    // Dedicated blocking-read front-end. Started with the job system; stopped
    // in Shutdown() BEFORE the engine pool shuts down (its reader threads are
    // Submit producers).
    std::unique_ptr<AssetIOService> m_IOService;

    // The pool workers texture cooks spread their bands across: helpers take
    // their slots from m_IOService's decode gate, as texture decodes do.
    std::unique_ptr<TextureCookWorkers> m_TextureCookWorkers;

    // Unified asset loading coordinator
    std::unique_ptr<AssetBatchCoordinator> m_BatchCoordinator;

    // Disable copy constructor and assignment operator
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
};

} // namespace GameEngine
