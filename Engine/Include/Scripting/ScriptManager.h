#pragma once

#include "Scripting/CoreCLRHost.h"
#include "Scripting/PackageScriptCompile.h"
#include "Scripting/ScriptsConfig.h"
#include "Types/Types.h"

#include "FileWatcher/FileWatcher.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadTasks.h"
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>

namespace JobSystem
{
class JobChannel;
}

namespace GameEngine
{

class FileWatchSubscription; // Assets/FileWatchingService.h (full include in the cpp)

// Use fully qualified JobSystem types to avoid conflicts

/**
 * @brief Script compilation result
 */
struct ScriptCompilationResult
{
    bool success;
    String outputPath;
    String errorMessage;
    Vector<String> warnings;
};

// ScriptsConfig is now in Scripting/ScriptsConfig.h (lightweight, no heavy deps).

/**
 * @brief Manages C# scripts and their compilation
 */
class ScriptManager
{
  public:
    ScriptManager();
    ~ScriptManager();

    /**
     * @brief Initialize script manager
     */
    bool Initialize(const ScriptsConfig& config, JobSystem::WorkStealingThreadPool& jobSystem);

    /**
     * @brief Shutdown script manager
     */
    void Shutdown();

    /**
     * @brief Check if initialized
     */
    bool IsInitialized() const { return m_Initialized; }

    /**
     * @brief Rebind project-specific script paths and watchers.
     *
     * Editor scripts, CLR host, and managed state are preserved.
     * Stops the project file watcher, updates paths, restarts watcher on new directory.
     * Unloads old project assemblies and kicks off compilation of the new project's scripts.
     * Reads only the path fields of newConfig; the hot-reload and project-generation policy
     * stays as Initialize set it.
     */
    bool RebindProjectScripts(const ScriptsConfig& newConfig);

    /**
     * @brief Compile all scripts in the scripts directory
     * @param cancelRequested Per-run supersede token (may be null); threaded
     * into each CompileProject so a superseded compile never writes its DLL
     * (see Jobs::ICompiler::compile).
     */
    ScriptCompilationResult CompileScripts(const std::atomic<bool>* cancelRequested);

    /**
     * @brief Compile a specific script project
     * @param cancelRequested Per-run supersede token (may be null). Consulted
     * before the compile starts and — under Jobs::AssemblyOutputWriteMutex —
     * between the server round-trip and the output-DLL write, so a compile
     * superseded mid-flight never clobbers the newer compile's output.
     */
    ScriptCompilationResult CompileProject(const std::filesystem::path& projectPath, const std::atomic<bool>* cancelRequested);

    /**
     * @brief Load compiled script assembly
     */
    bool LoadScriptAssembly(const std::filesystem::path& assemblyPath);

    /**
     * @brief Load compiled script assembly from path (preload + swap)
     */
    bool LoadScriptAssemblyFromPath(const std::filesystem::path& assemblyPath);

    /**
     * @brief Reload all script assemblies
     */
    bool ReloadScriptAssemblies();

    /**
     * @brief Recompile and reload script assemblies, blocking the calling thread until done
     */
    bool RecompileAndReload();

    /**
     * @brief Asynchronously recompile and reload script assemblies
     * @param assemblyPath Path to the assembly to reload
     * @return Future that resolves when hot-reload completes
     */
    std::future<HotReloadPipeline::PipelineStats> RecompileAndReloadAsync(const String& assemblyPath);

    /**
     * @brief Cancel any in-progress async hot-reload operation
     */
    void CancelAsyncHotReload();

    /**
     * @brief Check if async hot-reload is currently in progress
     */
    bool IsAsyncHotReloadInProgress() const;

    /**
     * @brief Get progress of current async hot-reload operation (0.0 to 1.0)
     */
    float GetAsyncHotReloadProgress() const;

    /**
     * @brief Get status message for current async hot-reload operation
     */
    String GetAsyncHotReloadStatus() const;

    /**
     * @brief Get CoreCLR host
     */
    CoreCLRHost& GetCLRHost() { return m_ClrHost; }

    // Explicitly set the native engine library managed code binds to, avoiding CWD heuristics. Call before InitializeCoreBridge.
    void SetNativeLibraryPath(const std::filesystem::path& nativeLibraryPath);

    /**
     * @brief Enable/disable auto-generation of a C# project when none is found
     */
    void SetAutoProjectGenerationEnabled(bool enabled);

    /**
     * @brief Enable/disable hot reloading of scripts
     */
    void SetHotReloadEnabled(bool enabled);

    /**
     * @brief Check if hot reload is enabled
     */
    bool IsHotReloadEnabled() const { return m_HotReloadEnabled; }

    /**
     * @brief Enable/disable async hot reloading
     */
    void SetAsyncHotReloadEnabled(bool enabled);

    /**
     * @brief Check if async hot reload is enabled
     */
    bool IsAsyncHotReloadEnabled() const { return m_AsyncHotReloadEnabled; }

    /**
     * @brief Process pending main thread tasks (call from main thread)
     * @return Number of tasks processed; 0 immediately if another thread is
     *         already pumping (the owner drains the queue)
     */
    size_t ProcessMainThreadTasks();

    /**
     * @brief Record the calling thread as the main thread. Used to diagnose
     *        main-thread tasks executing on a fallback thread.
     */
    void MarkMainThread();

    /**
     * @brief True once MarkMainThread ran, i.e. a live frame loop owns the
     *        main-thread task queue. Standalone hosts (gtest, dotnet-test)
     *        never mark one and must pump the queue themselves.
     */
    bool HasMarkedMainThread() const { return m_MainThreadId.load(std::memory_order_relaxed) != std::thread::id{}; }

    /**
     * @brief True when the calling thread is the marked main thread. False both
     *        off-main and when no main thread has been marked at all, so a
     *        caller that treats "no frame loop" as permissive must ask
     *        HasMarkedMainThread() as well.
     */
    bool IsMainThread() const
    {
        const std::thread::id marked = m_MainThreadId.load(std::memory_order_relaxed);
        return marked != std::thread::id{} && marked == std::this_thread::get_id();
    }

    /**
     * @brief Queue a task for execution on the main thread
     * @param task Function to execute on main thread
     */
    void QueueMainThreadTask(std::function<void()> task);

    /**
     * @brief Get the number of pending main thread tasks
     * @return Number of tasks in the main thread queue
     */
    size_t GetMainThreadTaskCount() const;

    /**
     * @brief RACE CONDITION FIX: Check if hot-reload is in critical section
     */
    bool IsHotReloadCriticalSection() const { return m_HotReloadCriticalSection.load(); }

    /**
     * @brief RACE CONDITION FIX: Enter hot-reload critical section
     */
    void EnterHotReloadCriticalSection();

    /**
     * @brief RACE CONDITION FIX: Exit hot-reload critical section
     */
    void ExitHotReloadCriticalSection();

    /**
     * @brief Get scripts directory
     */
    const std::filesystem::path& GetScriptsDirectory() const { return m_ScriptsDirectory; }

    /**
     * @brief Get compiled assemblies directory
     */
    std::filesystem::path GetAssembliesDirectory() const;

    /**
     * @brief Path of the compiled project-script assembly inside GetAssembliesDirectory()
     */
    std::filesystem::path GetScriptsAssemblyPath() const;

    // P1 packages: the resolved package code modules (dependency-topo order,
    // from CollectPackageCodeModules) whose CSharp entries this manager
    // compiles, plus the union of enabled-package defines the PROJECT scripts
    // compile receives. Called by the application layer whenever the project's
    // package set (re)mounts; the next compile pass picks it up (whole-graph
    // swap — no per-package reload in P1). Thread-safe against in-flight
    // background compiles. While hot reload is enabled, a .cs/.csproj change
    // under the C# module root of an Embedded or File package starts the same
    // whole-graph reload a project script edit does. A module with no sources
    // and a declared `prebuilt` directory uses <PrebuiltDir>/<AssemblyName>.dll
    // (and its .pdb) instead of compiling: it is copied into the package
    // assemblies directory here, and refused with an error when it is missing
    // or not a .NET assembly.
    // Call on the owning thread.
    void SetPackageCodeModules(std::vector<PackageCodeModule> modules,
                               std::vector<std::string> projectDefines);

    // Changes whenever SetPackageCodeModules hands over a module set, so a
    // consumer of GetCompiledAssemblyNames can poll it cheaply.
    uint64 GetPackageCodeModulesRevision() const;

    // Every assembly a reload compiles: each package C# module's that has
    // sources, then GameEngine.Editor and GameEngine.Scripts.
    std::vector<std::string> GetCompiledAssemblyNames() const;

    // Output directory for compiled package assemblies:
    // <assemblies>/Packages (runtime kind) or <assemblies>/Packages/Editor
    // (editor kind — loaded only when the editor scripts assembly is present).
    std::filesystem::path PackageAssembliesDirectory(bool editorKind) const;

    // Where the auto-generated project scripts csproj lives (EnsureAutoProject
    // writes it here): the generated-project root when configured, otherwise the
    // workspace (project) root, so an IDE opening it defaults new files to the
    // project rather than into Assets/. The build pipeline resolves the project's
    // csproj through this SAME accessor so editor and packager can never disagree
    // about the location.
    std::filesystem::path GeneratedProjectPath() const;

    // The generated scripts csproj inside a project root; GeneratedProjectPath()
    // and the build pipeline's no-live-session fallback both resolve through it.
    static std::filesystem::path GeneratedProjectPathFor(const std::filesystem::path& projectRoot);

    // Regenerate the auto-generated scripts csproj (content-compared write) so
    // its baked package references and paths match the CURRENT session state.
    // The packaged build calls this before `dotnet build` — the csproj is the
    // only reference carrier on that path (live editor compiles pass
    // references per compile-server request). No-op (false) in packaged mode,
    // when auto-generation is off, or before Initialize.
    bool RefreshGeneratedProject();

    // Runs `compile` as a job of the "Script compiles" channel, the one serial queue of every
    // C# compile in the process, and waits for it: a compile outside this manager that reads
    // what its compiles write (the build pipeline's csproj refresh and managed build) never
    // runs beside a live compile. Shutdown waits for a compile this queued before Shutdown
    // began. Before Initialize and after Shutdown, where nothing else compiles, `compile`
    // runs on the calling thread. Once Shutdown has started the call is refused, and a job
    // the job system's shutdown cancels never starts; `compile` then never runs. Returns true
    // when `compile` ran. Callable from any thread except a job of the "Script compiles"
    // channel (cap 1: the job would wait for itself), never concurrently with Initialize.
    bool RunCompile(const std::function<void()>& compile);

  private:
    // Delete <assemblies>/Packages[/Editor] dll/pdb files no resolved package
    // module produces. The hot-reload domain loads that directory wholesale, so
    // a removed package's assembly would otherwise keep loading (and keep
    // registering its components) every session. Runs during Initialize (from
    // a fresh resolution, BEFORE the initial domain load) and on each
    // package-set (re)mount; no-op in packaged mode.
    void PrunePackageAssemblies();
    void PrunePackageAssemblies(const std::unordered_set<std::string>& expectedAssemblyNames);

    // The prebuilt rule for one C# module: with sources it compiles (a declared
    // DLL is ignored, and the log says so); without sources its declared
    // <PrebuiltDir>/<AssemblyName>.dll and .pdb are copied, content-compared,
    // into PackageAssembliesDirectory. Returns false, with an error that states
    // the fix, when the declared DLL is missing, is not a .NET assembly or
    // cannot be copied: the module is then not registered.
    bool ResolvePrebuiltPackageAssembly(const PackageCodeModule& module) const;

    /**
     * @brief Handle file change events for hot reloading
     */
    void OnFileChanged(const FileChangeEvent& event);

    /**
     * @brief Subscribe OnFileChanged to .cs/.csproj changes under a directory
     * @return The subscription, or null when the directory is empty
     */
    UniquePtr<FileWatchSubscription> WatchScriptDirectory(const std::filesystem::path& directory);

    /**
     * @brief Replace the package module subscriptions with one per C# module
     *        root of an Embedded or File package, or none while hot reload is
     *        disabled. Call on the owning thread, never from a watch callback.
     */
    void RewatchPackageModuleRoots();

    /**
     * @brief Start a reload and make it the current one, which Shutdown joins
     */
    void StartTrackedHotReload(const String& assemblyPath);

    /**
     * @brief Start a reload on the async pipeline or the job system; the caller holds m_HotReloadStartMutex
     * @return Future of the reload; already Cancelled once Shutdown has started
     */
    std::future<HotReloadPipeline::PipelineStats> StartHotReload(const String& assemblyPath);

    /**
     * @brief Run RecompileAndReload on the job system (async hot reload disabled);
     *        the caller holds m_HotReloadStartMutex
     * @return Future fulfilled with the reload outcome, or the exception it threw
     */
    std::future<HotReloadPipeline::PipelineStats> SubmitSyncHotReload();
    void RunSyncHotReload(std::promise<HotReloadPipeline::PipelineStats>& promise);
    void SetCurrentAsyncFuture(std::future<HotReloadPipeline::PipelineStats> future);

    /**
     * @brief Find all C# project files in scripts directory
     */
    Vector<std::filesystem::path> FindProjectFiles() const;

    /**
     * @brief Execute dotnet build command (CLI fallback), writing to outputDir.
     */
    ScriptCompilationResult ExecuteDotNetBuild(const std::filesystem::path& projectPath,
                                               const std::filesystem::path& outputDir);

    // Shared C# compile unit: compile-server round-trip for `desc`, DLL/PDB
    // write to outDir/outDllName (supersede-gated), CLI fallback on
    // projectPath when the server path fails and GE_ALLOW_CLI_FALLBACK=1.
    ScriptCompilationResult CompileCsUnit(const std::filesystem::path& projectPath,
                                          const CompileServerRequestDesc& desc,
                                          const std::filesystem::path& outDir,
                                          const std::string& outDllName,
                                          const std::atomic<bool>* cancelRequested);

    // Compile every package C# module (topo order); a failed package logs
    // loudly and its dependents are skipped, but project scripts still
    // compile. Failures never flip the overall compile result.
    void CompilePackageModules(const std::atomic<bool>* cancelRequested);

    // Assert-level refusal for compile/generation entry points in packaged
    // mode (a shipped game must never spawn dotnet or write csproj files).
    // Returns true when the call must not proceed.
    bool RefusePackagedModeCompile(const char* operation) const;

    // Delete a generated scripts project (+ its .lscache) left inside the scripts
    // root, unless the scripts root is the configured generated-project root;
    // FindProjectFiles would otherwise compile it as a user project. Logs once
    // per removal.
    void RemoveStaleGeneratedProject() const;

    // Content-compared write of the generated csproj for one package module at
    // <packageRoot>/.Cache/ScriptProjects/<AssemblyName>.csproj.
    bool EnsurePackageModuleProject(const PackageCodeModule& module,
                                    std::filesystem::path& outProjectPath) const;

    // Absolute paths of a module's dependency package assemblies (Runtime dir,
    // .Editor-suffixed names in the Editor dir).
    std::vector<std::filesystem::path> PackageDependencyAssemblyPaths(
        const std::vector<std::string>& dependencyAssemblies) const;

    /**
     * @brief Parse MSBuild output for errors and warnings
     */
    void ParseBuildOutput(const String& output, ScriptCompilationResult& result);

    /**
     * @brief Trigger initial build if no assemblies are found
     */
    bool TriggerInitialBuild();
    void QueueDeferredInitialBuildIfNeeded();
    /**
     * @brief Ensure an auto-generated C# project exists in the assemblies directory
     *        that references sources under the scripts root. Returns its path via outProjectPath.
     */
    bool EnsureAutoProject(std::filesystem::path& outProjectPath) const;

    /**
     * @brief Ensure an auto-generated C# project exists for editor scripts (sources under EditorRoot/Assets).
     *        Returns its path via outProjectPath.
     */
    bool EnsureEditorAutoProject(std::filesystem::path& outProjectPath) const;

    /**
     * @brief Check whether the project scripts directory overlaps the editor scripts directory.
     *
     * Returns true when m_ScriptsDirectory is equal to or a subdirectory of m_EditorScriptsDirectory.
     * When true, project script compilation should be skipped to avoid compiling the same .cs files
     * into both GameEngine.Editor.dll and GameEngine.Scripts.dll.
     */
    bool AreScriptDirectoriesOverlapping() const;

  private:
    // Controls whether an ephemeral .csproj is auto-generated if none are found under the scripts root
    bool m_AutoProjectGenerationEnabled = true;

    // Written by Initialize and Shutdown on the owning thread. Atomic because the
    // watch subscriptions exist before Initialize sets it, so a reload start on
    // a watcher thread reads it concurrently.
    std::atomic<bool> m_Initialized;
    std::filesystem::path m_WorkspaceRoot;
    std::filesystem::path m_ScriptsDirectory;
    std::filesystem::path m_AssembliesDirectory;
    std::filesystem::path m_GeneratedProjectRoot;
    // Packaged-game mode (ScriptsConfig::prebuiltAssemblyPath): load exactly
    // this staged assembly; every compile path is disabled while set.
    std::filesystem::path m_PrebuiltAssemblyPath;
    // Positive packaged signal (ScriptsConfig::packagedMode, or a prebuilt
    // assembly configured): compile/generation entry points refuse at
    // assert-level while set — a shipped game must never spawn dotnet or
    // write csproj files into the game directory.
    bool m_PackagedMode = false;
    std::filesystem::path m_EditorScriptsDirectory;      // EditorRoot/Assets
    std::filesystem::path m_EditorAssembliesDirectory;   // EditorRoot (next to Editor.exe)
    std::atomic<bool> m_ProjectScriptsOverlap{false};     // True when project dir overlaps editor dir
    CoreCLRHost m_ClrHost;
    // FileWatchingService subscriptions for .cs/.csproj changes; held only while hot reload is enabled.
    UniquePtr<FileWatchSubscription> m_ScriptsWatchSubscription;
    UniquePtr<FileWatchSubscription> m_EditorScriptsWatchSubscription;
    // One per watched package C# module root, rebuilt from the module set by
    // SetPackageCodeModules; same lifetime rule as the two handles above.
    std::vector<UniquePtr<FileWatchSubscription>> m_PackageModuleWatchSubscriptions;

    // Written by the setters on the owning thread. Atomic because the watch
    // subscriptions can already exist when they are written (Initialize sets the
    // async flag after SetHotReloadEnabled subscribes, and EngineCore calls the
    // setters again after Initialize), so a change event on a watcher thread
    // reads them concurrently.
    std::atomic<bool> m_HotReloadEnabled;
    std::atomic<bool> m_AsyncHotReloadEnabled;
    bool m_DeferInitialLoad = false;
    Vector<std::filesystem::path> m_LoadedAssemblies;

    // Async hot-reload infrastructure
    // "Script compiles" (cap 1): every C# compile this manager starts off the
    // calling thread is a job of it: the pipeline's compile stage, the deferred
    // initial build, the package-module retry and the synchronous reload. The
    // compile server serves one connection per workspace at a time, so a
    // second concurrent compile would only wait at its pipe (on Windows, fail
    // there or launch a second host); here it waits in the FIFO instead, and
    // no compile holds a compute worker. Created by Initialize, destroyed by
    // Shutdown after everything that submits to it.
    std::unique_ptr<JobSystem::JobChannel> m_CompileChannel;
    // Created by Initialize before the watch subscriptions, destroyed by Shutdown.
    // A reload start can run on a watcher thread, so StartHotReload reads it only
    // under m_HotReloadStartMutex and after checking m_ShuttingDown, and calls
    // CancelAsyncHotReload from there. Every other reader (CancelAsyncHotReload,
    // IsAsyncHotReloadInProgress, the progress and status getters) takes no
    // lock: it runs on the thread that calls Initialize and Shutdown.
    std::unique_ptr<HotReloadPipeline> m_HotReloadPipeline;
    // Newest reload's future. Written from watcher threads and the main thread and
    // consumed by IsAsyncHotReloadInProgress and Shutdown, so every access holds m_CurrentAsyncFutureMutex.
    mutable std::future<HotReloadPipeline::PipelineStats> m_CurrentAsyncFuture;
    mutable std::mutex m_CurrentAsyncFutureMutex;

    // Shutdown/lifetime: prevents work from being queued during teardown and allows
    // us to join fire-and-forget tasks that capture `this`.
    std::atomic<bool> m_ShuttingDown{false};

    // When deferInitialLoad is enabled we queue a one-shot background build.
    // This TaskHandle is used to wait for completion during shutdown so worker
    // threads can't execute lambdas that captured `this` after destruction.
    JobSystem::TaskHandle m_DeferredInitialBuildTask{};

    // Compile-channel jobs of SubmitSyncHotReload and RunCompile not yet
    // terminal; Shutdown waits on each one (a queued one can be cancelled while
    // an older one still runs, so the newest alone does not join them all).
    // Guarded by m_HotReloadStartMutex.
    std::vector<JobSystem::TaskHandle> m_TrackedCompileTasks;
    // Held by every reload start and every RunCompile from its m_ShuttingDown
    // check until its job is submitted and tracked and, for a tracked reload,
    // its future published. Shutdown sets m_ShuttingDown and then takes this
    // mutex, so every start either finished publishing before Shutdown collects
    // what it joins, or refuses.
    std::mutex m_HotReloadStartMutex;

    // Main thread execution queue
    std::queue<std::function<void()>> m_MainThreadTasks;
    std::mutex m_MainThreadTasksMutex;
    // Serialize consumers while allowing a callback to pump this queue again on
    // the same thread; consumers try-lock and bail if another thread is pumping
    // (rationale in ProcessMainThreadTasks). Producers use the queue mutex above.
    std::recursive_mutex m_MainThreadTaskProcessingMutex;
    // Set once by MarkMainThread; empty id means unknown (standalone test hosts).
    std::atomic<std::thread::id> m_MainThreadId{};
    std::atomic<bool> m_WarnedOffMainThreadPump{false};

    // One-shot deferred initial build scheduling (when startup must not block)
    std::atomic<bool> m_DeferredInitialBuildQueued{false};

    // One-shot package-module compile retry (compile-server cold-start race):
    // queued through the main thread onto the compile channel (the next pipeline tick)
    // instead of silently keeping the previous session's stale package DLLs.
    // Re-armed by a fully successful package compile pass.
    std::atomic<bool> m_PackageModuleRetryScheduled{false};
    JobSystem::TaskHandle m_PackageModuleRetryTask{};

    // RACE CONDITION FIX: Hot-reload state protection
    std::atomic<bool> m_HotReloadCriticalSection{false};
    mutable std::mutex m_HotReloadStateMutex;

    // P1 packages: C# code modules to compile (topo order) + the defines the
    // project scripts compile inherits. Guarded — set from the main thread,
    // read from background compile workers.
    mutable std::mutex m_PackageModulesMutex;
    std::vector<PackageCodeModule> m_PackageCSharpModules;
    std::vector<std::string> m_ProjectPackageDefines;
    std::atomic<uint64> m_PackageCodeModulesRevision{0};

    DISALLOW_COPY_AND_ASSIGN(ScriptManager);
};

} // namespace GameEngine
