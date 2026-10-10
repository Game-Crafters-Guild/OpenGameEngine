#pragma once

// GE_DEBUG_INSTRUMENTATION selects whether engine classes carry their debug-only
// members, so it is part of the engine's binary interface rather than a local
// diagnostic switch. The engine's build defines it, the SDK's generated
// find_package config and NativeScripting manifest republish the value the
// staged Engine library was built with, and ApplicationConfig below carries the
// host's value to EngineCore::Initialize. An absent definition is the one case
// the runtime check cannot report honestly, so refuse it here.
#if !defined(GE_DEBUG_INSTRUMENTATION)
#error "GE_DEBUG_INSTRUMENTATION is not defined. Engine classes lay out differently under its two values, so a host that includes engine headers must be compiled with the value the Engine library it links was built with. Link the engine through find_package(GameEngine) (SDK consumers) or through the repository's own CMake (in-tree targets); both define it."
#endif

#include "Core/WindowInputRouter.h"
#include "Types/Types.h"
#include <cstddef>
#include <filesystem>
#include <memory>

namespace GameEngine {

namespace Input { class GamepadEdgeTracker; class InputSystem; }

/**
 * @brief Utility functions for path resolution
 */
class PathUtils {
public:
    /**
     * @brief Get the directory containing the current executable
     * @return Absolute path to the executable directory
     */
    static std::filesystem::path GetExecutableDirectory();

    /**
     * @brief Get the root of the engine assets staged for the executable: beside it, or in
     * Contents/Resources inside a macOS app bundle.
     *
     * This is where the build stages engine-owned runtime content (shaders, fonts,
     * textures), so it is the only correct anchor for loading it. Staged content must never
     * be resolved against the process working directory: a shipped or relocated build has
     * no repo to walk up into, and the working directory is whatever happened to launch the
     * process. The layout rule is InstallAssetsRootFor(), applied to GetExecutableDirectory()
     * once per process and cached: callers may resolve through it on every load.
     *
     * @return Absolute path to the staged engine asset root, empty if the executable
     *         directory cannot be determined
     */
    static std::filesystem::path GetInstallAssetsRoot();

    /**
     * @brief A cooked shader cache the build ships alongside the runtime, or empty
     * where there is none. A runtime with no shader compiler (the browser) reads
     * this instead of a project's own cache and seeds new projects from it.
     */
    static std::filesystem::path GetBundledShaderCacheRoot();

    /**
     * @brief The staged asset root for an executable that lives in @p executableDirectory:
     * `InstallContentRootFor(executableDirectory) / "Assets"`.
     *
     * @return Absolute path to the staged asset root for that executable directory; empty
     *         only when @p executableDirectory is empty
     */
    static std::filesystem::path InstallAssetsRootFor(const std::filesystem::path& executableDirectory);

    /**
     * @brief The directory that holds everything the build stages for an executable that
     * lives in @p executableDirectory, apart from code: `Assets/`, and in a packaged game
     * `game.config`, `Packages/` and `NativeScripts/`.
     *
     * The executable directory itself everywhere, with one exception: inside a macOS app
     * bundle (GetBundleResourcesDirectory() is non-empty) it is the bundle's
     * `Contents/Resources`, because `Contents/MacOS` holds code only. codesign seals a file
     * there as code, and refuses a directory whose name looks like a nested bundle. The rule
     * is structural, as GetBundleResourcesDirectory() is: nothing is probed for existence, so
     * a flat layout (test executables, CLI tools) never moves to a `Resources/` sibling that
     * happens to exist. A trailing separator on @p executableDirectory is ignored.
     *
     * @return The content root for that executable directory; empty only when
     *         @p executableDirectory is empty
     */
    static std::filesystem::path InstallContentRootFor(const std::filesystem::path& executableDirectory);

    /**
     * @brief Resources directory of the macOS app bundle containing an executable directory.
     *
     * A bundled executable lives at <App>.app/Contents/MacOS; the bundle's resources at
     * <App>.app/Contents/Resources. Structural only — nothing is probed for existence. A
     * trailing separator on @p executableDirectory is ignored.
     *
     * @return The bundle's Contents/Resources directory; empty when @p executableDirectory is
     *         not a bundle's Contents/MacOS, and always empty on platforms other than macOS,
     *         whose layouts are executable-relative
     */
    static std::filesystem::path GetBundleResourcesDirectory(const std::filesystem::path& executableDirectory);

    /**
     * @brief Get the scripts assembly output directory relative to executable
     * @return Path to where script assemblies should be built and loaded from
     */
    static std::filesystem::path GetScriptsAssemblyDirectory();

    /**
     * @brief Ensure working directory matches executable directory
     * This fixes path resolution issues when the executable is launched from different directories
     */
    static void EnsureWorkingDirectoryMatchesExecutable();

    /**
     * @brief Ensure working directory matches the provided directory.
     *
     * Callers should typically pass an absolute path. When the directory does not exist,
     * changing the working directory will fail; callers may create it first if appropriate.
     */
    static void EnsureWorkingDirectoryMatches(const std::filesystem::path& directory);

    /**
     * @brief Get a user-writable cache directory suitable for derived data, caches, thumbnails, etc.
     *
     * Windows: %LOCALAPPDATA% (fallback: %APPDATA%)
     * macOS:   ~/Library/Caches
     * Linux:   $XDG_CACHE_HOME (fallback: ~/.cache)
     */
    static std::filesystem::path GetUserCacheDirectory();

    /**
     * @brief Get a user-writable data directory suitable for save data and user settings.
     *
     * Windows: %APPDATA% (fallback: %LOCALAPPDATA%)
     * macOS:   ~/Library/Application Support
     * Linux:   $XDG_DATA_HOME (fallback: ~/.local/share)
     */
    static std::filesystem::path GetUserDataDirectory();

    /**
     * @brief Get a user-writable Documents directory suitable for user-facing save data.
     *
     * Windows: Known Folder Documents (fallback: %USERPROFILE%\\Documents)
     * macOS:   ~/Documents
     * Linux:   $XDG_DOCUMENTS_DIR via user-dirs.dirs when available (fallback: ~/Documents)
     *
     * NOTE: This is distinct from GetUserDataDirectory(), which is intended for application
     * support data/config rather than user-facing documents.
     */
    static std::filesystem::path GetUserDocumentsDirectory();

    /**
     * @brief Sanitize an identifier for safe use as a single folder name.
     *
     * Replaces characters illegal or problematic on common filesystems with '_',
     * trims trailing spaces/dots (Windows), and collapses empty to "App".
     */
    static std::string SanitizeForFolderName(std::string_view name);
};

// Default project layout, relative to the workspace root.
inline constexpr const char* kDefaultAssetDirectory = "Assets";
inline constexpr const char* kDefaultAssetDatabaseFile = "AssetDatabase.assetdb";
inline constexpr const char* kDefaultAssetDatabaseCacheDirectory = ".Cache/AssetDatabase";

/**
 * @brief Application configuration
 */
struct ApplicationConfig {
    /// The value of GE_DEBUG_INSTRUMENTATION the HOST was compiled with.
    ///
    /// Public engine headers declare data members under that switch, so a host
    /// compiled with the other value lays out engine classes differently from
    /// the Engine library it loads and reads every member past the first
    /// difference at the wrong offset — garbage values, then a crash far from
    /// the cause. The default initializer is evaluated in whichever translation
    /// unit constructs the config, so every host reports its own value without
    /// having to remember to; EngineCore::Initialize refuses a host that
    /// disagrees with the engine. First member deliberately: a layout
    /// disagreement cannot move offset zero, so the check that detects one is
    /// itself readable across the boundary.
    uint32 HostDebugInstrumentation = GE_DEBUG_INSTRUMENTATION;

    String Name = "Game Engine Application";
    uint32 WindowWidth = 1280;
    uint32 WindowHeight = 720;
    bool Fullscreen = false;
    String AssetDirectory = kDefaultAssetDirectory;
    // Workspace root for scripts/projects/assemblies; interpreted relative to the executable when not absolute.
    // Empty means "use the executable directory".
    String WorkspaceDirectory;
    // True when WorkspaceDirectory is only a safe writable staging root, not a
    // project explicitly selected by the user. Editor startup uses this to avoid
    // persisting project state for the staging workspace.
    bool WorkspaceDirectoryIsFallback = false;
    // Optional override: authoritative asset database file location.
    // When not absolute, interpreted relative to the resolved workspace root.
    String AssetDatabaseFile = kDefaultAssetDatabaseFile;
    // Optional override: derived asset database cache directory (SQLite, diagnostics, etc.).
    // When not absolute, interpreted relative to the resolved workspace root.
    String AssetDatabaseCacheDirectory = kDefaultAssetDatabaseCacheDirectory;
    bool EnableEditor = false;
    // Editor: give every scene a file as soon as it exists — name a new scene on
    // creation, and prompt once to save an untitled scene — so auto-save has a
    // target. Set by hosts whose only durable storage is the project tree
    // (browser storage); desktop keeps the frictionless untitled scene.
    bool PersistScenesEagerly = false;
    // Editor: a boot with no explicit project reopens the last project from
    // preferences (subject to the user's own startup preference). A host that
    // cannot open a project during its first frames sets this false and boots
    // to the project picker instead.
    bool AutoOpenLastProjectOnBoot = true;
    // Editor: the first frame on which a deferred startup scene may open. A host
    // whose project storage mounts asynchronously during boot sets this past
    // the frame that mount lands on; desktop storage is ready from frame 1.
    uint32_t StartupSceneOpenFrame = 1;
};

/**
 * @brief Base application class
 */
class Application {
public:
    /**
     * @brief Singleton accessor for the running application.
     *
     * The base Application sets this in its constructor and clears it in
     * the destructor. Returns nullptr if no application has been
     * constructed (e.g., during unit tests that don't instantiate one).
     *
     * Use sparingly — most subsystems should receive what they need
     * through their own initialization rather than reaching for the
     * global. This exists mainly so editor UI can read shared frame
     * timing (FPS) without duplicating the computation per panel.
     */
    // Defined out-of-line (Application.cpp) so consumers across the
    // Engine.dll boundary don't inline a direct read of the s_Instance DATA
    // symbol — DATA imports require dllimport on the declaration, while
    // function calls are auto-importable on MSVC.
    static Application* Get();

public:
    Application(const ApplicationConfig& config);
    virtual ~Application();

    /**
     * @brief Initialize the application
     */
    virtual bool Initialize();

    /**
     * @brief Run the main application loop
     */
    int Run();

    /**
     * @brief Advance the application by exactly one frame.
     *
     * The frame PollEventsAndTick() runs: timing, input, Update(), engine
     * update, Render(), pacing. Returns false once exit has been requested,
     * so tests can call it directly without owning a loop. Native refresh
     * callbacks call it while the OS holds the event pump.
     */
    bool Tick();

    /**
     * @brief One frame-driver step: pump window events, then Tick() unless a
     * native refresh inside the pump already ran this step's frame.
     *
     * RunPlatformLoop() repeats it; the browser's requestAnimationFrame
     * callback calls it once per callback. Returns false once exit has been
     * requested.
     */
    bool PollEventsAndTick();

    /**
     * @brief Shutdown the application
     */
    virtual void Shutdown();

    /**
     * @brief Get application configuration
     */
    const ApplicationConfig& GetConfig() const { return m_Config; }

    /**
     * @brief Check if application should exit
     */
    bool ShouldExit() const { return m_ShouldExit; }

    /**
     * @brief Request application exit
     */
    void RequestExit() { m_ShouldExit = true; }

    /**
     * @brief Set process exit code returned by Run()
     */
    void SetExitCode(int code) { m_ExitCode = code; }
    int GetExitCode() const { return m_ExitCode; }

    /**
     * @brief Get delta time for current frame
     */
    float64 GetDeltaTime() const { return m_DeltaTime; }

    /**
     * @brief Force the main loop to advance by a fixed delta each rendered frame.
     *
     * This is intended for offline/movie rendering where simulation time should
     * match the output video frame rate instead of wall-clock performance.
     */
    void SetFixedFrameRate(double fps);
    void ClearFixedFrameRate();
    bool IsFixedFrameRateEnabled() const { return m_FixedDeltaTime > 0.0; }
    double GetFixedFrameRate() const { return m_FixedDeltaTime > 0.0 ? (1.0 / m_FixedDeltaTime) : 0.0; }

    // Coarse per-frame phase timings (CPU), captured once per frame from the main loop.
    struct FramePhaseTimings
    {
        double PollEventsMs = 0.0;
        double InputMs = 0.0;
        double AppUpdateMs = 0.0;
        double EngineUpdateMs = 0.0;
        double RenderMs = 0.0;
        double SleepMs = 0.0; // only when frame cap is enabled
    };
    const FramePhaseTimings& GetLastFramePhaseTimings() const { return m_LastFramePhases; }

    /**
     * @brief Get total running time
     */
    float64 GetTotalTime() const { return m_TotalTime; }

    /**
     * @brief Get current frame count
     */
    uint64 GetFrameCount() const { return m_FrameCount; }

    /**
     * @brief Current frames-per-second computed as a sliding 250 ms
     * frame-interval window. Spike-resistant: a single slow frame doesn't
     * sticky-drag the displayed value — the timestamp simply ages out of
     * the window within 250 ms and the estimate recovers as fast frames return.
     *
     * Use this for any user-facing FPS display so all in-editor counters
     * agree. Don't compute FPS as 1/dt for a UI — one slow frame ruins
     * the next averaging window.
     */
    float GetFps() const { return m_CurrentFps; }

    /**
     * @brief Frame time over the same 250 ms window as GetFps(), in
     * milliseconds. Reports the WORST single-frame ms in the window so
     * spikes remain visible (mean ms hides them).
     */
    float GetFrameMsWorst() const { return m_FrameMsWorst; }

    /**
     * @brief Mean frame time over the same 250 ms window, in ms.
     */
    float GetFrameMsMean() const { return m_FrameMsMean; }

	    /**
	     * @brief Access the shared high-level InputSystem, if present.
	     *
	     * The base Application owns a single InputSystem instance that can be
	     * configured by derived applications (e.g., Editor, games) and is
	     * updated once per frame from the main loop before Update/Engine::Update.
	     */
	    Input::InputSystem* GetInputSystem() const { return m_InputSystem.get(); }

protected:
    /**
     * @brief Called once per frame for updates
     */
    virtual void Update(float64 deltaTime) { (void)deltaTime; }

    /**
     * @brief Called once per frame for rendering
     */
    virtual void Render() {}

    /**
     * @brief Called when application is being shut down
     */
    virtual void OnShutdown() {}

    /**
     * @brief Drive Tick() until exit is requested.
     *
     * The default implementation is the blocking desktop loop. A platform
     * whose environment owns the frame cadence (the browser: Run() must
     * return while requestAnimationFrame keeps calling Tick()) overrides
     * this to register the callback and return immediately.
     */
    virtual void RunPlatformLoop();

    /**
     * @brief The window input chain a gamepad's state and button edges enter.
     *
     * A gamepad is not typed into a window the way a keyboard is — the platform
     * reports it as state belonging to the process — so the application that
     * owns the windows names the chain that receives it. The base answers with
     * the chain an application with one window and no play surface has: its own
     * InputSystem, as the last and only stage.
     */
    virtual const WindowInputRouterConfig& GamepadInputChain() const { return m_ApplicationInputChain; }

private:
    /**
     * @brief Read the gamepads once and route what changed through the chain.
     */
    void RouteGamepadPoll();

    /**
     * @brief Update timing information
     */
    void UpdateTiming();

	private:
	    ApplicationConfig m_Config;
	    bool m_Initialized;
	    bool m_Ticking = false;
	    bool m_ShouldExit;
    int m_ExitCode = 0;
	    
	    // Timing
	    TimePoint m_StartTime;
	    TimePoint m_LastFrameTime;
	    float64 m_DeltaTime;
	    float64 m_TotalTime;
	    uint64 m_FrameCount;
        float64 m_FixedDeltaTime = 0.0;

	    // Sliding-window FPS state. Fixed-size ring instead of std::deque to
	    // avoid per-block bookkeeping in Debug builds and per-frame heap
	    // touches in any build. Capacity is sized for ~8000 FPS over a
	    // 250 ms window (above any realistic frame rate). Old entries are
	    // overwritten by the head pointer; aging-out is just a walk from
	    // tail while (now - timestamp) is outside the display window.
	    static constexpr std::size_t kFrameWindowCapacity = 2048;
	    TimePoint   m_FrameTimes[kFrameWindowCapacity]{};
	    float       m_FrameMs[kFrameWindowCapacity]{};
	    std::size_t m_FrameRingHead {0}; // next write position
	    std::size_t m_FrameRingCount {0}; // valid entries in [head - count, head)
	    float       m_CurrentFps    {0.0f};
	    float       m_FrameMsWorst  {0.0f};
	    float       m_FrameMsMean   {0.0f};

        FramePhaseTimings m_LastFramePhases{};

	    // Singleton storage — set by ctor, cleared by dtor. See Application::Get().
	    static Application* s_Instance;

	    // Shared high-level input system (optional, lazily created).
	    std::unique_ptr<Input::InputSystem> m_InputSystem;

	    // The chain GamepadInputChain() answers with unless a derived application
	    // names another: the shared InputSystem and nothing above it.
	    WindowInputRouterConfig m_ApplicationInputChain;

	    // Holds the previous poll so the buttons that changed since it become
	    // edges, and everything else stays state.
	    std::unique_ptr<Input::GamepadEdgeTracker> m_GamepadEdgeTracker;

    DISALLOW_COPY_AND_ASSIGN(Application);
};

} // namespace GameEngine
