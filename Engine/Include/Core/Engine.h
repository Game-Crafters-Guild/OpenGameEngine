#pragma once

#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "JobSystem/TaskHandle.h"
#include "Scripting/ScriptManager.h"
#include "Types/Types.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
// Forward declare JobSystem to avoid heavy headers in Engine public API
namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
}

// Forward declare ECS World to avoid heavy includes in header
namespace GameEngine
{
namespace Input
{
class InputSystem;
}
namespace ECS
{
class World;
}
namespace NativeScripting
{
// Forward-declared: NativeScriptManager.h is a module-local header, not on the
// include path of every module that pulls in Core/Engine.h. The full include lives
// in Engine.cpp, where EngineCore's out-of-line destructor needs the complete type.
class NativeScriptManager;
}
} // namespace GameEngine
// Forward declare Rendering device to avoid heavy includes
namespace GameEngine
{
namespace Rendering
{
class IDevice;
}
} // namespace GameEngine
// Forward declare RenderingLoop to avoid heavy includes
namespace GameEngine
{
namespace Engine
{
namespace Renderer
{
class RenderingLoop;
}
} // namespace Engine
} // namespace GameEngine
// Forward declare RenderServices to avoid heavy includes
namespace GameEngine
{
namespace Engine
{
namespace Renderer
{
class RenderServices;
}
} // namespace Engine
} // namespace GameEngine
// Forward declare AudioSystem to avoid heavy includes
namespace GameEngine
{
namespace Audio
{
class AudioSystem;
}
} // namespace GameEngine

namespace GameEngine
{

/**
 * @brief Lifecycle of the EngineCore singleton.
 *
 * Initialize() moves Uninitialized -> Initializing -> Initialized; Shutdown() returns
 * to Uninitialized. The distinct Initializing state is what lets a re-entrant or
 * concurrent Initialize be refused instead of building every subsystem twice.
 */
enum class EngineLifecycleState : uint8_t
{
    Uninitialized,
    Initializing,
    Initialized,
};

/**
 * @brief Main engine class that manages all subsystems
 */
class EngineCore
{
  public:
    EngineCore();
    ~EngineCore();

    /**
     * @brief Initialize the engine with given configuration
     */
    bool Initialize(const ApplicationConfig& config);

    // Optional: override default scripting configuration before Initialize().
    // Tests and tools (e.g. Editor) can call this to control workspace/scripts roots.
    void SetScriptsConfig(const ScriptsConfig& config);

    /**
     * @brief Shutdown the engine
     */
    void Shutdown();

    /**
     * @brief Update all engine subsystems
     */
    void Update(float64 deltaTime);

    /**
     * @brief Check if engine is initialized
     */
    bool IsInitialized() const
    {
        return m_Lifecycle.load(std::memory_order_acquire) == EngineLifecycleState::Initialized;
    }

    /**
     * @brief True while Initialize() is running on some thread.
     */
    bool IsInitializing() const
    {
        return m_Lifecycle.load(std::memory_order_acquire) == EngineLifecycleState::Initializing;
    }

    /**
     * @brief True while Initialize() is running on the calling thread.
     *
     * ABI entry points reached from inside that call (ScriptManager -> CoreCLR ->
     * GE_*) use this to avoid initializing the engine a second time. A caller on
     * another thread does not qualify: for it the engine is half-built.
     */
    bool IsInitializingOnCurrentThread() const
    {
        return IsInitializing() &&
               m_InitializingThread.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    /**
     * @brief True once Initialize() has been called in this process, whatever it returned.
     *
     * Never cleared: not by a failed run, not by Shutdown(). It records that some
     * caller drives this engine's lifecycle, which is how the scripting ABI tells an
     * engine nobody has started (standalone hosting initializes it) from one it must
     * leave alone.
     */
    bool HasEverInitialized() const { return m_EverInitialized.load(std::memory_order_acquire); }

    /**
     * @brief Get asset manager.
     *
     * Precondition: Initialize() has constructed the manager. A host that drives
     * engine code without a full Initialize — a bare test harness, a headless or
     * embedding host — has none, and the assert names that misuse at the accessor
     * instead of letting it surface as a null-page fault at a small offset inside
     * whichever subsystem touches the manager first. Code that legitimately runs
     * in such a host calls TryGetAssetManager() and handles the null.
     */
    AssetManager& GetAssetManager()
    {
        assert(m_AssetManager != nullptr
               && "EngineCore has no AssetManager: initialize the engine (EngineCore::Initialize) "
                  "before using assets, or call TryGetAssetManager() and handle the null");
        return *m_AssetManager;
    }

    // The asset manager, or null until Initialize() constructs it. Lets code that may
    // run before/without a full engine init (e.g. loading a model from a bare test
    // harness) fall back to defaults instead of asserting on a manager it never had.
    AssetManager* TryGetAssetManager() noexcept { return m_AssetManager.get(); }

    /**
     * @brief Get script manager (lazily constructs if missing to support C-ABI test scenarios)
     */
    ScriptManager& GetScriptManager()
    {
        if (!m_ScriptManager)
        {
            m_ScriptManager = MakeUnique<ScriptManager>();
        }
        return *m_ScriptManager;
    }

    /**
     * @brief Get engine-owned AudioSystem (may be null if audio init failed or disabled).
     * Resolves async init on first call if still pending.
     */
    Audio::AudioSystem* GetAudioSystem();

    /**
     * @brief Get primary ECS world (opaque to external systems for now)
     */
    ECS::World* GetPrimaryWorld() const { return m_PrimaryWorld.get(); }

    /**
     * @brief Ensure a primary ECS world exists and return it (creates lazily if needed)
     */
    ECS::World* EnsurePrimaryWorld();

    /**
     * @brief Get job system.
     *
     * Precondition: the pool exists only between Initialize and Shutdown.
     * Callers that can run without an initialized engine (standalone test
     * hosts) must check IsInitialized() before calling — dereferencing the
     * returned reference from an uninitialized engine is undefined behavior.
     */
    JobSystem::WorkStealingThreadPool& GetJobSystem() { return *m_JobSystem; }

    /**
     * @brief The "Engine startup" channel (cap 2) on the job system's blocking
     * threads: one-time startup loads that wait on the disk or a device (the
     * audio device open, an application's shader package preload) run as its
     * jobs, beside each other and never on a compute worker.
     *
     * Precondition: exists between Initialize and Shutdown, like the job system.
     * A job still running when Shutdown starts must be waited for by its owner
     * before the systems it reads are torn down.
     */
    JobSystem::JobChannel& GetStartupChannel() { return *m_StartupChannel; }

    // Native C++ user-script hot-reload manager. Null until EngineCore::Initialize runs.
    // The editor uses it to point the watcher at a project and surface build results.
    NativeScripting::NativeScriptManager* GetNativeScriptManager() { return m_NativeScriptManager.get(); }

    // True while a native module (the project's or a package's) may still load and register types
    // (NativeScriptManager::AreModulesPending); false before Initialize. Main thread only.
    bool AreNativeModulesPending() const;

    // True when the latest build of a native module that ships with the game failed
    // (NativeScriptManager::AnyShippedModuleBuildFailed); false before Initialize. Main thread only.
    bool HasFailedNativeModuleBuild() const;

    /**
     * @brief Get the resolved workspace root (absolute, normalized).
     *
     * This is the canonical project/workspace directory the engine uses as the
     * base for workspace-relative paths and as the process working directory.
     */
    const std::filesystem::path& GetWorkspaceRoot() const { return m_WorkspaceRoot; }

    /**
     * @brief True while the workspace root is the startup staging fallback
     * (ApplicationConfig::WorkspaceDirectoryIsFallback) rather than a project
     * the user opened. SetWorkspaceRoot clears it.
     */
    bool IsWorkspaceRootFallback() const { return m_WorkspaceRootIsFallback; }

    // ---------------------------------------------------------------------
    // Application-owned input (non-owning)
    //
    // The base Application owns the concrete InputSystem instance; Engine keeps
    // a non-owning pointer so subsystems and the scripting ABI can query input
    // without needing a handle to the host Application type.
    // ---------------------------------------------------------------------
    void SetInputSystem(Input::InputSystem* input) { m_InputSystem = input; }
    Input::InputSystem* GetInputSystem() const { return m_InputSystem; }

    // Runtime input for gameplay code. In the editor, a separate InputSystem
    // that only receives events when the game view is focused. Falls back to
    // the main InputSystem when no runtime input is set (Player builds).
    // Thread safety: must only be called from the main thread (window input
    // callbacks, Update tick, and shutdown are all main-thread).
    void SetRuntimeInput(Input::InputSystem* input) { m_RuntimeInput = input; }
    Input::InputSystem* GetRuntimeInput() const { return m_RuntimeInput ? m_RuntimeInput : m_InputSystem; }

    /**
     * @brief Update the workspace root when switching projects.
     *
     * This updates all internal paths that depend on the workspace root.
     * Called by the Editor when the user opens a different project folder.
     */
    void SetWorkspaceRoot(const std::filesystem::path& newRoot);

    /**
     * @brief Get the resolved asset root (absolute, normalized).
     *
     * Typically <WorkspaceRoot>/<assetDirectory> (default: Assets).
     */
    const std::filesystem::path& GetResolvedAssetRoot() const { return m_ResolvedAssetRoot; }

    /**
     * @brief Get the authoritative AssetDatabase file location (absolute, normalized).
     */
    const std::filesystem::path& GetAuthoritativeAssetDbFile() const { return m_AuthoritativeAssetDbFile; }

    /**
     * @brief Get the derived AssetDatabase cache root (absolute, normalized).
     */
    const std::filesystem::path& GetAssetDbCacheRoot() const { return m_AssetDbCacheRoot; }

    /**
     * @brief Get singleton instance
     */
    static EngineCore& GetInstance();

    /// Enable the engine-owned rendering loop and its shared system schedule.
    bool EnableRenderingLoop(Engine::Renderer::RenderServices* renderServices);
    void DisableRenderingLoop();
    // Non-owning access to the RenderServices instance currently driving the engine-managed rendering loop (if enabled).
    Engine::Renderer::RenderServices* GetRenderServices() const { return m_RenderServices; }
    // Control whether Engine::Update automatically steps the rendering loop.
    // Default is true; tools like the Editor can disable this and drive the
    // rendering loop explicitly at a more appropriate point in their frame.
    void SetRenderingLoopAutoDrive(bool enabled);
    // Explicitly step the engine-owned rendering loop for the primary world
    // (no-op if the loop is not enabled or no world exists).
    void StepRenderingLoop(float32 deltaTime);
    // Convenience: set per-system frequency by system name (Every N frames).
    //
    // CADENCE-GAP WARNING: throttling a system that consumes the per-frame
    // change-signaling buffers (ECS lifecycle events GetAdded/GetRemoved, or
    // the ComponentDirtyFeed) makes it skip swap windows — the engine tick
    // keeps swapping every frame, so skipped windows are discarded unseen.
    // Such consumers must implement the swap-generation gap recovery
    // (ECS/SwapGenerationGuard.h over World::GetLifecycleSwapGeneration or
    // ComponentDirtyFeed::SwapGeneration; AudioEmitterSystem is the
    // reference consumer) or they will silently miss events (frozen state,
    // orphaned resources).
    bool ConfigureRenderingSystemEveryNFrames(const std::string& systemName, uint32 n);
    // Enable/disable a rendering-loop ECS system by name. Returns true if found.
    //
    // CADENCE-GAP WARNING: same hazard as ConfigureRenderingSystemEveryNFrames
    // above, and this is the one the editor's play-mode pause gating actually
    // exercises on gameplay systems — a disabled lifecycle-event consumer
    // misses every window swapped during the disable. Consumers must carry
    // the swap-generation gap recovery before this is safe to call on them.
    bool SetRenderingSystemEnabled(const std::string& systemName, bool enabled);
    // Access to the engine-managed rendering loop (nullptr if not enabled).
    Engine::Renderer::RenderingLoop* GetRenderingLoop() const;

    // invalidates pipeline variants on shader/material reload.
    void EnableRenderingHotReload(Rendering::IDevice& device);
    void DisableRenderingHotReload();

  private:
    // Points the terrain service's bake cache at this run's location (packaged
    // staged copy, the project workspace's .Cache, or off). Called when the world
    // services start and whenever the workspace root changes.
    void ApplyTerrainBakeCacheLocation();

    // Scopes one Initialize(): Commit() publishes Initialized; destruction without a
    // commit rolls the engine back to Uninitialized with nothing outstanding.
    class InitializeTransaction;
    struct ShaderHookState;
    // Captured before this engine installs process-wide shader services; restored after its jobs stop.
    UniquePtr<ShaderHookState> m_ShaderHookState;

    // Rollback half of a failed Initialize(): joins an AudioSystem init that this
    // Initialize launched and discards that AudioSystem, so the next Initialize()
    // starts clean instead of being refused.
    void DiscardAudioInitAfterFailedInitialize() noexcept;
    // Caller holds m_AudioInitMutex. Runs or joins this lifetime's audio init if one is
    // outstanding; false when it ran and failed.
    bool JoinAudioInitLocked();

    std::atomic<EngineLifecycleState> m_Lifecycle{EngineLifecycleState::Uninitialized};
    // Thread running Initialize(); default id whenever no Initialize is in flight.
    std::atomic<std::thread::id> m_InitializingThread{};
    // Set on entry to Initialize(), before the lifecycle is claimed, so a reader that
    // sees Initializing or Initialized also sees this. Never cleared.
    std::atomic<bool> m_EverInitialized{false};
    ApplicationConfig m_Config;
    std::optional<ScriptsConfig> m_ScriptsConfig;

    // Resolved key roots (computed during Initialize()).
    std::filesystem::path m_WorkspaceRoot;
    bool m_WorkspaceRootIsFallback = false;

    // Non-owning pointer to the current Application's InputSystem.
    Input::InputSystem* m_InputSystem = nullptr;
    Input::InputSystem* m_RuntimeInput = nullptr;
    std::filesystem::path m_ResolvedAssetRoot;
    std::filesystem::path m_AuthoritativeAssetDbFile;
    std::filesystem::path m_AssetDbCacheRoot;

    // Subsystems
    UniquePtr<JobSystem::WorkStealingThreadPool> m_JobSystem;
    UniquePtr<AssetManager> m_AssetManager;
    // Engine-managed rendering loop state
    UniquePtr<Engine::Renderer::RenderingLoop> m_RenderingLoop;
    bool m_RenderingLoopEnabled = false;
    // Non-owning pointer to RenderServices used by the rendering loop.
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    // When true (default), Engine::Update drives the rendering loop. Tools
    // like the Editor can disable this and call StepRenderingLoop() manually
    // after their own per-frame updates to avoid 1-frame latency between
    // editor-driven Transform changes and world rendering.
    bool m_RenderingLoopAutoDrive = true;

    UniquePtr<ScriptManager> m_ScriptManager;

    // Native C++ user-script hot-reload manager (watches .cpp/.h/.hpp; builds land in C10+).
    UniquePtr<NativeScripting::NativeScriptManager> m_NativeScriptManager;

    // The "Engine startup" channel (GetStartupChannel); created by Initialize after the
    // job system, destroyed by Shutdown (and a failed Initialize) before it.
    UniquePtr<JobSystem::JobChannel> m_StartupChannel;

    // Engine-owned AudioSystem/backend (may remain null if init fails).
    UniquePtr<Audio::AudioSystem> m_AudioSystem;
    // This lifetime's audio init, joined once (JoinAudioInitLocked): a job of the startup
    // channel where the joining thread may block, else deferred to run inline at the join.
    JobSystem::TaskHandle m_AudioInit;
    bool m_AudioInitDeferred = false;
    // Serializes the one-time join of the audio init.
    std::mutex m_AudioInitMutex;

    // ECS primary world (Phase 1 façade target)
    UniquePtr<ECS::World> m_PrimaryWorld;

    // Hot-reload bridge registration state
    uint32_t m_ShaderHotReloadHandle = 0; // 0 = not registered

    // Singleton instance
    static EngineCore* s_Instance;

    DISALLOW_COPY_AND_ASSIGN(EngineCore);
};

} // namespace GameEngine
