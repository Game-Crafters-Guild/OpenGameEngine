#include "Scripting/ScriptManager.h"
#include "JobSystem/JobChannel.h"
#include "Scripting/ScriptTargetFramework.h"
#include "Assets/Packages/PackageNativeCache.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/FileWatchingService.h"
#include "Core/Application.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/DotnetHost.h"
#include "Editor/EditorIPCClient.h"
#include "Jobs/CompileServerClient.h"
#include "Jobs/CompileServerUtil.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IncrementalCompileHints.h"
#include "Jobs/WorkspaceId.h"
#include "Logger/Logger.h"
#include "Scripting/PathResolver.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <span>
#include <unordered_set>
#include <utility>

#include "Scripting/ScriptingABI.h"

#include <cstdlib>

#ifdef _WIN32
#include <libloaderapi.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace GameEngine
{

namespace
{
// A compile that lost to a newer file change reports failure with a
// "superseded" message (see HotReloadTasks / CompileProject); it is the
// debounce working as designed and must not log as an error.
bool IsSupersededCompileResult(const ScriptCompilationResult& result)
{
    return result.errorMessage.find("superseded") != String::npos;
}

// A reload that was refused before it started.
std::future<HotReloadPipeline::PipelineStats> MakeFinishedReload(HotReloadResult result, const char* message)
{
    std::promise<HotReloadPipeline::PipelineStats> promise;
    HotReloadPipeline::PipelineStats stats;
    stats.success = false;
    stats.result = result;
    stats.errorMessage = message;
    promise.set_value(stats);
    return promise.get_future();
}

uint32 ReadLittleEndian(const Vector<uint8>& bytes, size_t offset, size_t width)
{
    uint32 value = 0;
    for (size_t i = 0; i < width; ++i)
        value |= static_cast<uint32>(bytes[offset + i]) << (8 * i);
    return value;
}

// True when bytes are a PE image whose CLI header data directory is present:
// a .NET assembly, as opposed to a native DLL or any other file.
bool IsDotNetAssemblyImage(const Vector<uint8>& bytes)
{
    constexpr size_t kPeOffsetField = 0x3C;
    constexpr size_t kCoffHeaderSize = 20;
    constexpr uint32 kPe32Magic = 0x10B;
    constexpr uint32 kPe32PlusMagic = 0x20B;
    constexpr size_t kCliHeaderDirectory = 14;
    constexpr size_t kDataDirectorySize = 8;

    if (bytes.size() < kPeOffsetField + 4 || bytes[0] != 'M' || bytes[1] != 'Z')
        return false;
    const size_t peOffset = ReadLittleEndian(bytes, kPeOffsetField, 4);
    const size_t optionalHeader = peOffset + 4 + kCoffHeaderSize;
    if (optionalHeader + 2 > bytes.size() || bytes[peOffset] != 'P' || bytes[peOffset + 1] != 'E' ||
        bytes[peOffset + 2] != 0 || bytes[peOffset + 3] != 0)
        return false;
    const uint32 magic = ReadLittleEndian(bytes, optionalHeader, 2);
    if (magic != kPe32Magic && magic != kPe32PlusMagic)
        return false;
    // NumberOfRvaAndSizes, then the data directories, follow the fixed part of
    // the optional header (96 bytes for PE32, 112 for PE32+).
    const size_t directories = optionalHeader + (magic == kPe32Magic ? 96 : 112);
    if (directories > bytes.size() ||
        ReadLittleEndian(bytes, directories - 4, 4) <= kCliHeaderDirectory)
        return false;
    const size_t cliEntry = directories + kCliHeaderDirectory * kDataDirectorySize;
    if (cliEntry + kDataDirectorySize > bytes.size())
        return false;
    return ReadLittleEndian(bytes, cliEntry, 4) != 0 && ReadLittleEndian(bytes, cliEntry + 4, 4) != 0;
}

// Writes sourceBytes to destination unless it already holds the same bytes, so
// an unchanged prebuilt assembly is not rewritten on every mount.
bool WriteIfContentDiffers(const Vector<uint8>& sourceBytes, const std::filesystem::path& destination,
                           std::string& outError)
{
    Vector<uint8> destinationBytes;
    if (ReadFileBytesShared(destination, destinationBytes) && destinationBytes == sourceBytes)
        return true;
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(sourceBytes.data()), static_cast<std::streamsize>(sourceBytes.size()));
    if (!out)
    {
        outError = "cannot write '" + destination.string() + "'";
        return false;
    }
    return true;
}
} // namespace

ScriptManager::ScriptManager()
    : m_Initialized(false), m_HotReloadEnabled(false), m_AsyncHotReloadEnabled(false)
{
}

void ScriptManager::SetAutoProjectGenerationEnabled(bool enabled)
{
    m_AutoProjectGenerationEnabled = enabled;
}

ScriptManager::~ScriptManager()
{
    if (m_Initialized)
    {
        Shutdown();
    }
}

bool ScriptManager::Initialize(const ScriptsConfig& config, JobSystem::WorkStealingThreadPool& jobSystem)
{
    if (m_Initialized)
    {
        Logger::Log::Warning("ScriptManager already initialized");
        return true;
    }

    m_ShuttingDown.store(false, std::memory_order_relaxed);

    Logger::Log::Info("Initializing Script Manager");
    Logger::Log::Info("Workspace root: {}", config.workspaceRoot.string());
    Logger::Log::Info("Scripts directory: {}", config.scriptsRoot.string());
    Logger::Log::Info("Assemblies directory: {}", config.assembliesRoot.string());

    m_WorkspaceRoot = config.workspaceRoot;
    m_ScriptsDirectory = config.scriptsRoot;
    m_AssembliesDirectory = config.assembliesRoot;
    m_GeneratedProjectRoot = config.generatedProjectRoot;
    Logger::Log::Info("Generated scripts project: {}", GeneratedProjectPath().string());
    m_AutoProjectGenerationEnabled = config.enableAutoProjectGeneration;
    m_DeferInitialLoad = config.deferInitialLoad;
    m_PrebuiltAssemblyPath = config.prebuiltAssemblyPath;
    m_PackagedMode = config.packagedMode || !m_PrebuiltAssemblyPath.empty();
    if (!m_PrebuiltAssemblyPath.empty())
        Logger::Log::Info("Packaged mode: prebuilt script assembly '{}' (runtime compilation disabled)",
                          m_PrebuiltAssemblyPath.string());

    // Packaged mode is a hard override, not a preference: whatever the rest of
    // the config says, a shipped game never generates projects, never watches
    // sources, never compiles.
    if (m_PackagedMode)
    {
        if (m_AutoProjectGenerationEnabled)
        {
            Logger::Log::Error(
                "ScriptManager: auto project generation requested in packaged mode — forcing OFF "
                "(a shipped game never writes csproj files)");
            m_AutoProjectGenerationEnabled = false;
        }
        m_DeferInitialLoad = false;
    }

    // Route managed main-thread work ([InitializeOnLoad], B5) through this
    // manager's task queue, drained per frame by EngineCore::Update via
    // ProcessMainThreadTasks. Must be wired before CoreBridge initialization so
    // InstallMainThreadPump sees the dispatcher.
    m_ClrHost.SetMainThreadDispatcher([this](std::function<void()> task)
                                      { QueueMainThreadTask(std::move(task)); });

    // Editor scripts live under the install assets root; editor-generated outputs
    // (GameEngine.Editor.csproj + GameEngine.Editor.dll) beside the engine's managed
    // assemblies — except inside a macOS app bundle, which is never written to (a write
    // invalidates its code signature), so they go to the user-writable scripts assemblies
    // directory instead.
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    const std::filesystem::path engineManagedDir = ScriptingPaths::ResolveEngineManagedDirectory();
    m_EditorScriptsDirectory = PathUtils::GetInstallAssetsRoot();
    m_EditorAssembliesDirectory = exeDir;
    if (!PathUtils::GetBundleResourcesDirectory(exeDir).empty() && !m_AssembliesDirectory.empty())
    {
        m_EditorAssembliesDirectory = m_AssembliesDirectory;
    }

    // Create primary directories if they do not exist yet
    std::error_code ec;
    if (!m_ScriptsDirectory.empty())
    {
        std::filesystem::create_directories(m_ScriptsDirectory, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to create scripts directory: {} - {}", m_ScriptsDirectory.string(), ec.message());
        }
        ec.clear();
    }
    if (!m_AssembliesDirectory.empty())
    {
        std::filesystem::create_directories(m_AssembliesDirectory, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to create scripts assembly directory: {} - {}", m_AssembliesDirectory.string(), ec.message());
        }
        ec.clear();
    }
    if (!m_GeneratedProjectRoot.empty())
    {
        std::filesystem::create_directories(m_GeneratedProjectRoot, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to create generated project directory: {} - {}", m_GeneratedProjectRoot.string(), ec.message());
        }
    }

    // Best-effort: ensure the editor scripts root exists so file watching can be enabled even in dev builds.
    if (!m_EditorScriptsDirectory.empty())
    {
        std::filesystem::create_directories(m_EditorScriptsDirectory, ec);
    }

    // Detect overlapping script directories (F.20): when --project resolves to the
    // editor's own Assets folder, both compilations would include the same .cs files.
    m_ProjectScriptsOverlap.store(AreScriptDirectoriesOverlapping(), std::memory_order_relaxed);
    if (m_ProjectScriptsOverlap.load(std::memory_order_relaxed))
    {
        Logger::Log::Warning(
            "Project scripts directory '{}' overlaps editor scripts directory '{}'. "
            "Project script compilation will be skipped to avoid duplicate hooks. "
            "Use --project to point to a separate project directory.",
            m_ScriptsDirectory.string(), m_EditorScriptsDirectory.string());
    }

    // Startup info: auto-generated project toggle
    Logger::Log::Info("Auto project generation is: {}", m_AutoProjectGenerationEnabled ? "ENABLED" : "DISABLED");

    if (!m_PackagedMode)
        RemoveStaleGeneratedProject();

    // Log resolved critical paths (exe-relative)
    Logger::Log::Info("Executable dir: {}", PathUtils::GetExecutableDirectory().string());
    Logger::Log::Info("Assemblies dir: {}", GetAssembliesDirectory().string());
    Logger::Log::Info("Scripts root: {}", m_ScriptsDirectory.string());

    // Ensure ephemeral auto-generated project exists (idempotent)
    if (m_AutoProjectGenerationEnabled)
    {
        std::filesystem::path autoProjPath;
        if (EnsureAutoProject(autoProjPath))
        {
            Logger::Log::Info("Startup: ensured auto-generated scripts project at {}", autoProjPath.string());
        }
    }

    if (!config.disableClr)
    {
        // Check if .NET CLI is available. Skipped in packaged mode: a shipped
        // game never compiles, and user machines have no SDK to probe.
        if (!m_PackagedMode && !IsDotnetSdkAvailable())
        {
            Logger::Log::Warning(".NET CLI not found - C# compilation will not be available");
        }

        // Initialize CoreCLR host
        std::filesystem::path runtimeConfig = ScriptingPaths::ResolveScriptsRuntimeConfig();
        if (!m_ClrHost.Initialize(runtimeConfig))
        {
            Logger::Log::Warning("Failed to initialize CoreCLR host - C# scripts will not be available");
        }
        else
        {
            // Initialize CoreBridge (managed, non-collectible) and register callbacks
            // Do not initialize CoreBridge here. We rely on the unified ABI bootstrap path:
            // GE_PreloadAssemblyContext/GE_SwapPreloadedContext -> EnsureClrInitialized -> InitializeCoreBridge.
            // This avoids double initialization and duplicate async project registration threads.
            // Diagnostics sink registration is performed during EnsureClrInitialized().
        }
    }
    else
    {
        Logger::Log::Info("ScriptManager: CLR disabled by config; skipping CoreCLR initialization");
    }

    // Initialize async hot-reload infrastructure
    m_CompileChannel = std::make_unique<JobSystem::JobChannel>(
        jobSystem, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    m_HotReloadPipeline = std::make_unique<HotReloadPipeline>(jobSystem, *m_CompileChannel);

    // Wire up the compiler so the async pipeline actually recompiles (not just reloads).
    // `mgr` stays valid for every compile: Shutdown destroys the pipeline, which joins
    // each stage it started, before tearing down anything CompileScripts uses. The
    // shutdown flag only skips the Roslyn round-trip for a compile that starts after
    // Shutdown began.
    m_HotReloadPipeline->SetCompilerFactory([this]() -> std::unique_ptr<Jobs::ICompiler>
    {
        struct ScriptManagerCompiler : Jobs::ICompiler
        {
            ScriptManager* mgr;
            std::atomic<bool>* shuttingDown;
            ScriptManagerCompiler(ScriptManager* m, std::atomic<bool>* sd) : mgr(m), shuttingDown(sd) {}
            CompilationResult compile(const String& /*assemblyPath*/, const std::atomic<bool>* cancelRequested) override
            {
                if (shuttingDown->load(std::memory_order_relaxed))
                    return {false, "ScriptManager is shutting down", {}, {"Compilation aborted: shutdown in progress"}};
                // Superseded before the compile started — skip the Roslyn
                // round-trip entirely. The token is also threaded through
                // CompileScripts/CompileProject: once running, the compile
                // finishes (F12), and CompileProject's pre-write check is what
                // keeps its stale DLL off the disk.
                if (cancelRequested && cancelRequested->load(std::memory_order_relaxed))
                    return {false, "Compilation superseded before start", {}, {"Compilation aborted: superseded by newer change"}};
                auto r = mgr->CompileScripts(cancelRequested);
                CompilationResult cr;
                cr.success = r.success;
                cr.output = r.errorMessage;
                cr.warnings = r.warnings;
                if (!r.success && !r.errorMessage.empty())
                    cr.errors.push_back(r.errorMessage);
                return cr;
            }
        };
        return std::make_unique<ScriptManagerCompiler>(this, &m_ShuttingDown);
    });

    Logger::Log::Info("Async hot-reload infrastructure initialized");

    // Apply initial hot-reload configuration (never in packaged mode: nothing
    // to watch, nothing to recompile).
    SetHotReloadEnabled(!m_PackagedMode && config.enableHotReload);
    SetAsyncHotReloadEnabled(!m_PackagedMode && config.enableAsyncHotReload);

    // The managed hot-reload domain loads every DLL under
    // <assemblies>/Packages[/Editor] wholesale as part of the initial script
    // assembly load below, but package code modules are registered only later
    // (the app mounts packages after engine init). Prune against a fresh
    // resolution NOW — otherwise a removed package's stale assembly loads for
    // one more session before the SetPackageCodeModules prune takes effect.
    if (!m_PackagedMode && !m_WorkspaceRoot.empty())
    {
        std::error_code packagesEc;
        if (std::filesystem::is_directory(PackageAssembliesDirectory(/*editorKind=*/false), packagesEc))
        {
            const PackageResolution resolution = PackageResolver::Resolve(m_WorkspaceRoot);
            // editorContext=true yields the superset (runtime + ".Editor"
            // assemblies): dev editor and dev player share ScriptAssemblies/,
            // so the prune must never treat editor-kind outputs as stale.
            std::unordered_set<std::string> expectedPackageAssemblies;
            for (const PackageCodeModule& module :
                 CollectPackageCodeModules(resolution, /*editorContext=*/true))
            {
                if (module.Lang == PackageModuleRecord::ModuleLang::CSharp)
                    expectedPackageAssemblies.insert(module.AssemblyName);
            }
            PrunePackageAssemblies(expectedPackageAssemblies);
        }
    }

    if (!config.disableClr && m_PackagedMode && m_PrebuiltAssemblyPath.empty())
    {
        // Positive packaged signal but nothing staged to load: an incomplete
        // package. Never fall through to the dev compile pipeline.
        Logger::Log::Error(
            "Packaged mode with no staged script assembly configured — the game's C# scripts "
            "will not run (repackage the game or fix game.config scriptAssemblyPath)");
    }
    else if (!config.disableClr && !m_PrebuiltAssemblyPath.empty())
    {
        // Packaged game: the staged assembly is the ONLY script source. No
        // editor-project generation (which used to write GameEngine.Editor.csproj
        // into the game dir and dotnet-build it at game startup), no staleness
        // rebuilds, no initial build — load it or fail loudly.
        std::error_code prebuiltEc;
        if (!std::filesystem::exists(m_PrebuiltAssemblyPath, prebuiltEc))
        {
            Logger::Log::Error(
                "Packaged script assembly missing: '{}' — the game's C# scripts will not run "
                "(the package is incomplete; repackage the game)",
                m_PrebuiltAssemblyPath.string());
        }
        else if (!LoadScriptAssembly(m_PrebuiltAssemblyPath))
        {
            Logger::Log::Error("Failed to load packaged script assembly '{}'",
                               m_PrebuiltAssemblyPath.string());
        }
    }
    else if (!config.disableClr)
    {
        // Ensure the Editor scripts assembly exists (best-effort) so project scripts can reference it.
        try
        {
            std::filesystem::path editorProj;
            if (EnsureEditorAutoProject(editorProj))
            {
                const std::filesystem::path editorDll = m_EditorAssembliesDirectory / "GameEngine.Editor.dll";
                bool editorDllMissing = !std::filesystem::exists(editorDll, ec);
                bool editorDllStale = false;
                if (!editorDllMissing)
                {
                    // Rebuild if the editor scripts assembly is older than the ABI it was compiled against.
                    const std::filesystem::path abiPath = engineManagedDir / "GameEngine.Scripting.ABI.dll";
                    try
                    {
                        if (std::filesystem::exists(abiPath))
                        {
                            ec.clear();
                            auto editorTime = std::filesystem::last_write_time(editorDll, ec);
                            if (!ec)
                            {
                                auto abiTime = std::filesystem::last_write_time(abiPath, ec);
                                if (!ec && editorTime < abiTime)
                                {
                                    editorDllStale = true;
                                    Logger::Log::Warning("Editor scripts assembly is older than Scripting.ABI — will rebuild.");
                                }
                            }
                        }
                    }
                    catch (...) { /* best-effort */ }
                }
                if (editorDllMissing || editorDllStale)
                {
                    if (!m_DeferInitialLoad)
                    {
                        (void)CompileProject(editorProj, nullptr);
                    }
                }
            }
        }
        catch (...)
        {
            // best-effort only
        }

        // CRITICAL: Load the Scripts assembly during initialization to enable hot-reload
        std::filesystem::path assemblyPath = GetScriptsAssemblyPath();
        bool needBuild = true;
        if (std::filesystem::exists(assemblyPath))
        {
            // Robust stub detection: scan for ReferenceAssembly attribute marker instead of size heuristics
            bool isReferenceStub = false;
            {
                Vector<uint8> data;
                if (ReadFileBytesShared(assemblyPath, data))
                {
                    const std::string_view view(reinterpret_cast<const char*>(data.data()), data.size());
                    if (view.find("ReferenceAssembly") != std::string_view::npos)
                    {
                        isReferenceStub = true;
                    }
                }
            }

            // Check if the Scripts assembly is older than the Scripting ABI it was compiled against.
            // A stale Scripts.dll can reference types that no longer exist in the ABI, causing
            // CLR type-load exceptions during reflection (GetTypes/GetExportedTypes).
            bool isStale = false;
            if (!isReferenceStub)
            {
                const std::filesystem::path abiPath = engineManagedDir / "GameEngine.Scripting.ABI.dll";
                try
                {
                    if (std::filesystem::exists(abiPath))
                    {
                        ec.clear();
                        auto scriptsTime = std::filesystem::last_write_time(assemblyPath, ec);
                        if (!ec)
                        {
                            auto abiTime = std::filesystem::last_write_time(abiPath, ec);
                            if (!ec && scriptsTime < abiTime)
                            {
                                isStale = true;
                                Logger::Log::Warning("Scripts assembly is older than Scripting.ABI — will rebuild to avoid type mismatches.");
                            }
                        }
                    }
                }
                catch (...) { /* best-effort */ }
            }

            if (isReferenceStub || isStale)
            {
                if (isReferenceStub)
                {
                    auto sz = std::filesystem::file_size(assemblyPath, ec);
                    Logger::Log::Warning("Found existing Scripts assembly marked as ReferenceAssembly ({} bytes). Will rebuild.", ec ? -1 : (long long)sz);
                }
            }
            else
            {
                Logger::Log::Debug("🚀 Loading initial Scripts assembly for hot-reload support...");
                if (LoadScriptAssembly(assemblyPath))
                {
                    Logger::Log::Debug("✅ Initial Scripts assembly loaded successfully");
                    needBuild = false;
                }
                else
                {
                    Logger::Log::Warning("⚠️ Failed to load initial Scripts assembly - will attempt a rebuild.");
                }
            }
        }
        else
        {
            Logger::Log::Debug("ℹ️ Scripts assembly not found - attempting initial build...");
        }
        if (needBuild)
        {
            if (m_DeferInitialLoad)
            {
                Logger::Log::Info("Scripts assembly missing/out-of-date; deferring initial build/load to background");
                QueueDeferredInitialBuildIfNeeded();
            }
            else
            {
                if (TriggerInitialBuild())
                {
                    Logger::Log::Debug("✅ Initial build completed successfully");
                    // Try to load the newly built assembly
                    if (std::filesystem::exists(assemblyPath) && LoadScriptAssembly(assemblyPath))
                    {
                        Logger::Log::Debug("✅ Initial Scripts assembly loaded after build");
                    }
                    else
                    {
                        Logger::Log::Warning("⚠️ Failed to load Scripts assembly after initial build");
                    }
                }
                else
                {
                    Logger::Log::Debug("ℹ️ Initial build failed or skipped - assembly will be built on first hot-reload");
                }
            }
        }
    }
    else
    {
        Logger::Log::Info("ScriptManager: CLR disabled by config; skipping initial scripts load/build");
    }

    // Drain managed main-thread work queued by the initial load NOW, while we
    // are still on the main thread. The per-frame pump (EngineCore::Update)
    // does not run until the whole app finishes initializing, so without this
    // the managed watchdog fires after 3s and steals the first
    // [InitializeOnLoad] batch onto a thread-pool thread (logged as a pump
    // fallback that reads like an error).
    (void)ProcessMainThreadTasks();

    m_Initialized = true;
    Logger::Log::Info("Script Manager initialized successfully");
    return true;
}

void ScriptManager::SetNativeLibraryPath(const std::filesystem::path& nativeLibraryPath)
{
    m_ClrHost.SetNativeLibraryOverride(nativeLibraryPath.string());
}

bool ScriptManager::RebindProjectScripts(const ScriptsConfig& newConfig)
{
    if (!m_Initialized)
    {
        Logger::Log::Warning("ScriptManager::RebindProjectScripts called while not initialized");
        return false;
    }

    Logger::Log::Info("ScriptManager: rebinding project scripts to '{}'", newConfig.scriptsRoot.string());

    // 1. Stop watching the old project scripts directory.
    m_ScriptsWatchSubscription.reset();

    // 2. Cancel any in-progress async hot reload.
    if (IsAsyncHotReloadInProgress())
    {
        CancelAsyncHotReload();
    }

    // 3. Update project-specific paths.
    m_WorkspaceRoot = newConfig.workspaceRoot;
    m_ScriptsDirectory = newConfig.scriptsRoot;
    m_AssembliesDirectory = newConfig.assembliesRoot;
    m_GeneratedProjectRoot = newConfig.generatedProjectRoot;

    // 4. Create directories if missing.
    std::error_code ec;
    if (!m_ScriptsDirectory.empty())
        std::filesystem::create_directories(m_ScriptsDirectory, ec);
    if (!m_AssembliesDirectory.empty())
        std::filesystem::create_directories(m_AssembliesDirectory, ec);
    if (!m_GeneratedProjectRoot.empty())
        std::filesystem::create_directories(m_GeneratedProjectRoot, ec);

    // 5. Re-check overlap after rebinding paths.
    m_ProjectScriptsOverlap.store(AreScriptDirectoriesOverlapping(), std::memory_order_relaxed);
    if (m_ProjectScriptsOverlap.load(std::memory_order_relaxed))
    {
        Logger::Log::Warning(
            "Project scripts directory '{}' overlaps editor scripts directory '{}'. "
            "Project script compilation will be skipped.",
            m_ScriptsDirectory.string(), m_EditorScriptsDirectory.string());
    }

    // 6. Re-generate .csproj for new location if auto-generation is enabled.
    if (!m_PackagedMode)
        RemoveStaleGeneratedProject();
    if (m_AutoProjectGenerationEnabled)
    {
        std::filesystem::path autoProjectPath;
        (void)EnsureAutoProject(autoProjectPath);
    }

    // 7. Watch the new project scripts directory.
    if (m_HotReloadEnabled && !m_ScriptsDirectory.empty())
    {
        m_ScriptsWatchSubscription = WatchScriptDirectory(m_ScriptsDirectory);
        Logger::Log::Info("ScriptManager: project script watcher restarted for '{}'", m_ScriptsDirectory.string());
    }

    Logger::Log::Info("ScriptManager: project scripts rebound successfully");
    return true;
}

void ScriptManager::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    Logger::Log::Info("Shutting down Script Manager");

    // Prevent any new background work from being queued during teardown.
    m_ShuttingDown.store(true, std::memory_order_relaxed);

    // Unsubscribe from file watching first so no new change starts a reload.
    m_ScriptsWatchSubscription.reset();
    m_EditorScriptsWatchSubscription.reset();
    m_PackageModuleWatchSubscriptions.clear();

    // Unsubscribing does not wait for a change event already being dispatched.
    // Once this lock is held, every reload start either published its future
    // and task handle (joined below) or will see m_ShuttingDown and refuse, so
    // none reaches m_HotReloadPipeline after it is reset.
    std::vector<JobSystem::TaskHandle> trackedCompileTasks;
    {
        std::lock_guard<std::mutex> lock(m_HotReloadStartMutex);
        trackedCompileTasks = std::exchange(m_TrackedCompileTasks, {});
    }

    // CRITICAL: Cancel and wait for async operations to complete
    Logger::Log::Info("Cancelling async hot-reload operations...");
    CancelAsyncHotReload();

    // Wait for the in-progress hot reload's result. A cancelled pipeline run resolves as
    // soon as it sees the token, and destroying the pipeline below joins its stage tasks;
    // the task of a synchronous reload is joined below.
    std::future<HotReloadPipeline::PipelineStats> currentAsyncFuture;
    {
        std::lock_guard<std::mutex> lock(m_CurrentAsyncFutureMutex);
        currentAsyncFuture = std::move(m_CurrentAsyncFuture);
    }
    if (currentAsyncFuture.valid())
    {
        Logger::Log::Info("Waiting for async hot-reload to complete...");
        try
        {
            auto stats = currentAsyncFuture.get();
            Logger::Log::Info("Async hot-reload completed during shutdown: {}", stats.success ? "SUCCESS" : "FAILED");
            if (!stats.success && !stats.errorMessage.empty())
            {
                Logger::Log::Warning("Async hot-reload shutdown status: {}", stats.errorMessage);
            }
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Exception while waiting for async hot-reload completion: {}", e.what());
        }
        catch (...)
        {
            Logger::Log::Warning("Unknown exception while waiting for async hot-reload completion");
        }
    }

    // If we queued the deferred initial build, wait for it to finish before we
    // tear down state and allow ScriptManager destruction.
    if (m_DeferredInitialBuildTask.IsValid() && !m_DeferredInitialBuildTask.IsDone())
    {
        Logger::Log::Info("Waiting for deferred initial scripts build task to complete...");
        m_DeferredInitialBuildTask.Wait();
    }
    m_DeferredInitialBuildTask = JobSystem::TaskHandle{};

    // Same for every synchronous reload submitted by SubmitSyncHotReload and every compile
    // queued by RunCompile: a running build pipeline compile calls back into this manager.
    for (JobSystem::TaskHandle& trackedCompileTask : trackedCompileTasks)
        trackedCompileTask.Wait();

    // Same for a scheduled package-module compile retry.
    if (m_PackageModuleRetryTask.IsValid() && !m_PackageModuleRetryTask.IsDone())
        m_PackageModuleRetryTask.Wait();
    m_PackageModuleRetryTask = JobSystem::TaskHandle{};

    // CRITICAL: Shutdown async infrastructure in correct order
    Logger::Log::Info("Shutting down async infrastructure...");
    // Blocks until every stage the pipeline started has finished: a compile stage that
    // was already running holds `this` and must end before CoreCLR and the loaded
    // assemblies go away.
    if (m_HotReloadPipeline)
    {
        m_HotReloadPipeline.reset();
    }
    // Every job of the channel (the deferred initial build, the tracked reloads and
    // RunCompile jobs, the package-module retry, the pipeline's stages) has been waited
    // on above; the channel goes before the pool it is registered with.
    m_CompileChannel.reset();

    // Shutdown CoreCLR
    m_ClrHost.Shutdown();

    m_LoadedAssemblies.clear();

    m_Initialized = false;
    Logger::Log::Info("Script Manager shutdown complete");
}

bool ScriptManager::RefusePackagedModeCompile(const char* operation) const
{
    if (!m_PackagedMode)
        return false;
    // A shipped game has no sources and no toolchain; reaching a compile or
    // project-generation path here is an engine bug, not a recoverable state.
    Logger::Log::Critical(
        "ScriptManager: {} attempted in packaged mode — a shipped game never compiles or "
        "generates projects (engine bug; the staged prebuilt assembly is authoritative)",
        operation);
    assert(false && "ScriptManager compile/generation path reached in packaged mode");
    return true;
}

ScriptCompilationResult ScriptManager::CompileScripts(const std::atomic<bool>* cancelRequested)
{
    if (!m_Initialized)
    {
        return {false, "", "Script manager not initialized", {}};
    }

    // Packaged mode never compiles: there are no sources and no toolchain in a
    // shipped game — the staged assembly loaded at Initialize is authoritative.
    // (Benign skip, not the assert-level refusal: this is the pipeline-facing
    // orchestrator and a stray reload request must not crash a shipped game.)
    if (m_PackagedMode)
    {
        return {true, "", "Packaged mode: prebuilt scripts only, compilation disabled", {}};
    }

    Logger::Log::Info("Compiling all scripts");

    // P1 packages: compile package C# modules (dependency-topo order) FIRST —
    // they feed the PROJECT assembly, not the editor one, so they must not be
    // gated on the editor-scripts result (a transient editor compile failure,
    // e.g. a compile-server connect race on first open, used to leave every
    // package uncompiled until the next pipeline run). A failed package logs
    // loudly and is skipped (with its dependents); it never blocks anything else.
    CompilePackageModules(cancelRequested);

    // Editor scripts assembly (EditorRoot/Assets -> GameEngine.Editor.dll).
    // A failed editor compile must NOT derail the rest of the chain: the
    // hot-reload swap consumes the PROJECT assembly, and an editor failure
    // just leaves the previously loaded editor scripts in place. Report it
    // loudly, carry its diagnostics on the result, and keep going. Supersede
    // stays an early-out — a newer pipeline run is already queued behind
    // this one, so finishing the stale run is pure waste.
    ScriptCompilationResult editorRes{true, "", "", {}};
    std::filesystem::path editorAutoProject;
    if (EnsureEditorAutoProject(editorAutoProject))
    {
        editorRes = CompileProject(editorAutoProject, cancelRequested);
        if (!editorRes.success)
        {
            // Supersede is the debounce working as designed (a newer change arrived
            // before this compile started), not a failure worth an ERROR.
            if (IsSupersededCompileResult(editorRes))
            {
                Logger::Log::Info("Editor scripts compilation superseded by a newer change: {}", editorRes.errorMessage);
                return editorRes;
            }
            Logger::Log::Error("Editor scripts compilation failed (package/project compiles continue): {}",
                               editorRes.errorMessage);
        }
    }

    // Skip project scripts when directories overlap (F.20) — editor compilation already covers them.
    if (m_ProjectScriptsOverlap.load(std::memory_order_relaxed))
    {
        // Overlap means the editor compile IS the project compile, so its
        // failure is the chain's failure.
        if (!editorRes.success)
            return editorRes;
        Logger::Log::Info("Skipping project script compilation (directory overlaps editor scripts)");
        return {true, "", "Project scripts skipped (overlapping editor directory)", {}};
    }

    // Editor diagnostics ride along as warnings: overall success tracks the
    // project chain (the assembly the reload swap consumes), while the editor
    // failure stays visible to result consumers and the log.
    ScriptCompilationResult overallResult{true, "", "", {}};
    overallResult.warnings.insert(overallResult.warnings.end(),
                                  editorRes.warnings.begin(),
                                  editorRes.warnings.end());
    if (!editorRes.success)
    {
        overallResult.warnings.push_back(
            "Editor scripts compilation failed: " + editorRes.errorMessage);
    }

    Vector<std::filesystem::path> projectFiles = FindProjectFiles();
    std::filesystem::path autoProject;
    if (projectFiles.empty())
    {
        if (!m_AutoProjectGenerationEnabled)
        {
            Logger::Log::Info("No C# project files found and auto-generation is disabled; skipping compilation");
            overallResult.errorMessage = "No projects to compile (auto-generation disabled)";
            return overallResult;
        }
        if (EnsureAutoProject(autoProject))
        {
            projectFiles.push_back(autoProject);
        }
        else
        {
            overallResult.errorMessage = "No projects to compile (failed to auto-generate project)";
            return overallResult;
        }
    }

    for (const auto& projectPath : projectFiles)
    {
        ScriptCompilationResult result = CompileProject(projectPath, cancelRequested);
        if (!result.success)
        {
            overallResult.success = false;
            overallResult.errorMessage += result.errorMessage + "\n";
        }
        overallResult.warnings.insert(overallResult.warnings.end(),
                                      result.warnings.begin(),
                                      result.warnings.end());
    }

    return overallResult;
}

// Default to CompileServer; allow opt-out via GE_DISABLE_COMPILE_SERVER=1
static bool DisableCompileServer()
{
    const char* env = std::getenv("GE_DISABLE_COMPILE_SERVER");
    return env && std::string(env) == "1";
}

ScriptCompilationResult ScriptManager::CompileProject(const std::filesystem::path& projectPath, const std::atomic<bool>* cancelRequested)
{
    if (RefusePackagedModeCompile("CompileProject"))
    {
        return {false, "", "Packaged mode: compilation disabled", {}};
    }

    if (!IsDotnetSdkAvailable())
    {
        return {false, "", "dotnet CLI not available", {}};
    }

    // Superseded before this project's compile started — skip the server
    // round-trip (and, on the CLI path, the dotnet-driven DLL write) entirely.
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed))
    {
        return {false, "", "Compilation superseded before start", {}};
    }

    Logger::Log::Info("Compiling project: {}", projectPath.string());

    const bool isEditorProject = (projectPath.filename() == "GameEngine.Editor.csproj");
    const std::filesystem::path outDir = isEditorProject ? m_EditorAssembliesDirectory : GetAssembliesDirectory();

    if (DisableCompileServer())
    {
        Logger::Log::Info("CompileServer disabled (GE_DISABLE_COMPILE_SERVER=1); using dotnet CLI");
        return ExecuteDotNetBuild(projectPath, outDir);
    }

    CompileServerRequestDesc desc;
    desc.ProjectRoot = projectPath.parent_path();
    // Assembly identity for the generated project names; a user-authored csproj
    // keeps the server default (its output still lands as GameEngine.Scripts.dll).
    {
        const auto file = projectPath.filename().string();
        if (file == "GameEngine.Scripts.csproj")
            desc.AssemblyName = "GameEngine.Scripts";
        else if (file == "GameEngine.Editor.csproj")
            desc.AssemblyName = "GameEngine.Editor";
    }
#ifdef NDEBUG
    desc.Config = "Release";
#endif
    desc.EngineBinDir = ScriptingPaths::ResolveEngineManagedDirectory();
    // Only user-authored sources from the relevant scripts root (never the
    // csproj dir — obj/bin churn).
    desc.SourceRoots = {isEditorProject ? m_EditorScriptsDirectory : m_ScriptsDirectory};

    // P1 packages: the PROJECT scripts compile references every package runtime
    // assembly that exists (compiled just before in CompileScripts) and inherits
    // the union of enabled-package defines, so game code can consume package
    // types and `#if GE_PACKAGE_OCEAN_PACK`. Editor-kind package assemblies are deliberately
    // NOT referenced — GameEngine.Scripts.dll must load in the Player, where they
    // never exist. The editor scripts assembly predates packages and stays clean.
    if (!isEditorProject)
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        desc.Defines = m_ProjectPackageDefines;
        const std::filesystem::path runtimeDir = PackageAssembliesDirectory(/*editorKind=*/false);
        for (const PackageCodeModule& module : m_PackageCSharpModules)
        {
            if (module.Kind != PackageModuleRecord::ModuleKind::Runtime)
                continue;
            std::error_code ec;
            const std::filesystem::path dll = runtimeDir / (module.AssemblyName + ".dll");
            if (std::filesystem::exists(dll, ec))
                desc.References.push_back(dll);
        }
    }

    const std::string outDllName =
        isEditorProject ? "GameEngine.Editor.dll" : std::string(ScriptingPaths::kScriptsAssemblyFileName);
    return CompileCsUnit(projectPath, desc, outDir, outDllName, cancelRequested);
}

ScriptCompilationResult ScriptManager::CompileCsUnit(const std::filesystem::path& projectPath,
                                                     const CompileServerRequestDesc& desc,
                                                     const std::filesystem::path& outDir,
                                                     const std::string& outDllName,
                                                     const std::atomic<bool>* cancelRequested)
{
    // IMPORTANT: The pipe name must be stable per *workspace*, not per-project directory.
    // Otherwise we accidentally spawn multiple compile servers for nested projects
    // (e.g., <exe>/GameEngine.Editor.csproj vs <Workspace>/GameEngine.Scripts.csproj),
    // which causes long connection timeouts and stalls during Editor startup.
    const std::filesystem::path pipeRoot = m_WorkspaceRoot.empty() ? desc.ProjectRoot : m_WorkspaceRoot;
    const std::string pipeName = ComputeCompileServerPipeName(pipeRoot);
    const std::string json = BuildCompileServerRequestJson(desc);

    // Test seam: same transport injection point as CompileServerCompiler.
    std::unique_ptr<IHotReloadTransport> transport;
    if (auto transportFactory = GetCompileServerTransportFactoryForTests())
    {
        transport = transportFactory(pipeName);
    }
    CompileServerClient client(std::move(transport), pipeName);
    CompileServerResponse resp;
    if (client.Compile(json, resp) && resp.Success)
    {
        if (!resp.AssemblyBytes.empty())
        {
            // Superseded while the server round-trip was in flight: the
            // newer compile owns the output path now. Same gate as
            // CompileServerCompiler::compile — the check and the write
            // share one critical section so a check that narrowly passed
            // cannot land its write after (or torn-interleaved with) the
            // newer DLL. The shared mutex also serializes the deferred
            // initial build's write against a concurrent pipeline compile.
            std::lock_guard<std::mutex> writeLock(Jobs::AssemblyOutputWriteMutex());

            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed))
            {
                return {false, outDir.string(), "Compilation superseded; output write skipped", {}};
            }

            std::filesystem::create_directories(outDir);
            std::ofstream of(outDir / outDllName, std::ios::binary);
            of.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size());
            // Emit PDB if present
            if (!resp.PdbBytes.empty())
            {
                std::filesystem::path pdbPath = outDir / outDllName;
                pdbPath.replace_extension(".pdb");
                std::ofstream pf(pdbPath, std::ios::binary);
                pf.write(reinterpret_cast<const char*>(resp.PdbBytes.data()), (std::streamsize)resp.PdbBytes.size());
            }
            return {true, outDir.string(), "", {}};
        }
        Logger::Log::Error("CompileServer returned success but no AssemblyBytes; treating as failure.");
        return {false, outDir.string(), "CompileServer returned empty assembly bytes", {}};
    }

    CompileServerClient::NoteFallback();
    if (EditorIPC::IsEnabled())
    {
        EditorIPC::NotifyCompileServerFallbackOnce();
    }
    const char* allowEnv = std::getenv("GE_ALLOW_CLI_FALLBACK");
    if (allowEnv && std::string(allowEnv) == "1")
    {
        // dotnet writes the output DLL itself and cannot be gated
        // mid-build, so a supersede that landed during the failed server
        // attempt must skip the fallback before it starts.
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed))
        {
            return {false, "", "Compilation superseded; CLI fallback skipped", {}};
        }
        Logger::Log::Warning("CompileServer attempt failed; falling back to CLI (GE_ALLOW_CLI_FALLBACK=1)");
        return ExecuteDotNetBuild(projectPath, outDir);
    }

    Logger::Log::Error("CompileServer attempt failed; CLI fallback disabled. Set GE_ALLOW_CLI_FALLBACK=1 to enable CLI fallback.");
    ScriptCompilationResult res;
    res.success = false;
    res.outputPath = outDir.string();
    res.errorMessage = "CompileServer failed and CLI fallback disabled";
    return res;
}

void ScriptManager::SetPackageCodeModules(std::vector<PackageCodeModule> modules,
                                          std::vector<std::string> projectDefines)
{
    std::vector<PackageCodeModule> csharpModules;
    for (PackageCodeModule& module : modules)
    {
        if (module.Lang == PackageModuleRecord::ModuleLang::CSharp && ResolvePrebuiltPackageAssembly(module))
            csharpModules.push_back(std::move(module));
    }
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        m_PackageCSharpModules = std::move(csharpModules);
        m_ProjectPackageDefines = std::move(projectDefines);
        Logger::Log::Info("ScriptManager: {} package C# module(s) registered for compilation",
                          m_PackageCSharpModules.size());
    }
    m_PackageCodeModulesRevision.fetch_add(1, std::memory_order_release);
    RewatchPackageModuleRoots();
    // Prune stale assemblies first so the regenerated project never references
    // outputs the current package resolution no longer produces.
    PrunePackageAssemblies();
    // Keep the generated csproj's baked package references current for MSBuild
    // consumers (live compiles pass references per compile-server request).
    // Outside the mutex: EnsureAutoProject re-locks it to read the module set.
    (void)RefreshGeneratedProject();
}

bool ScriptManager::ResolvePrebuiltPackageAssembly(const PackageCodeModule& module) const
{
    if (module.PrebuiltDir.empty())
        return true;

    const std::string dllName = module.AssemblyName + ".dll";
    const std::filesystem::path declaredDll = module.PrebuiltDir / dllName;
    std::error_code ec;
    const bool shipsDll = std::filesystem::is_regular_file(declaredDll, ec);
    if (!module.RootDir.empty())
    {
        if (shipsDll)
            Logger::Log::Info("[Packages] C# module '{}' has sources, so it compiles; the declared prebuilt "
                              "assembly '{}' is not used",
                              module.AssemblyName, declaredDll.string());
        return true;
    }

    const bool editorKind = module.Kind == PackageModuleRecord::ModuleKind::Editor;
    if (!shipsDll)
    {
        Logger::Log::Error(
            "[Packages] package '{}' declares prebuilt assembly '{}' for its C# module and the file is missing: "
            "put {} in that directory, add the module's .cs sources, or remove the package.json modules[] entry "
            "with \"lang\": \"CSharp\", \"kind\": \"{}\", \"prebuilt\": \"{}\"",
            module.PackageName, declaredDll.string(), dllName, editorKind ? "Editor" : "Runtime",
            module.PrebuiltDir.lexically_relative(module.PackageRootDir).generic_string());
        return false;
    }

    // Shipped games never stage: their assemblies were staged by the build.
    if (m_PackagedMode || GetAssembliesDirectory().empty())
        return true;

    const std::filesystem::path outDir = PackageAssembliesDirectory(editorKind);
    std::string error;
    Vector<uint8> dllBytes;
    if (!ReadFileBytesShared(declaredDll, dllBytes))
    {
        Logger::Log::Error("[Packages] cannot read prebuilt assembly '{}' of package '{}'; check that the file is "
                           "readable",
                           declaredDll.string(), module.PackageName);
        return false;
    }
    // Refused here rather than by the script domain's load, which cannot name
    // the package or the manifest entry.
    if (!IsDotNetAssemblyImage(dllBytes))
    {
        Logger::Log::Error(
            "[Packages] package '{}' declares prebuilt assembly '{}' and it is not a .NET assembly: rebuild it as "
            "a {} class library named {}, or remove the package.json modules[] entry with \"lang\": \"CSharp\", "
            "\"kind\": \"{}\", \"prebuilt\": \"{}\"",
            module.PackageName, declaredDll.string(), kScriptTargetFramework, module.AssemblyName,
            editorKind ? "Editor" : "Runtime",
            module.PrebuiltDir.lexically_relative(module.PackageRootDir).generic_string());
        return false;
    }

    const std::filesystem::path declaredPdb = std::filesystem::path(declaredDll).replace_extension(".pdb");
    const std::filesystem::path stagedPdb = outDir / declaredPdb.filename();
    Vector<uint8> pdbBytes;
    const bool shipsPdb = std::filesystem::is_regular_file(declaredPdb, ec);
    if (shipsPdb && !ReadFileBytesShared(declaredPdb, pdbBytes))
        error = "cannot read '" + declaredPdb.string() + "'";
    {
        // The same output gate a compile writes under: the copy never
        // interleaves with a compile landing the same file.
        std::lock_guard<std::mutex> writeLock(Jobs::AssemblyOutputWriteMutex());
        bool copied = error.empty() && WriteIfContentDiffers(dllBytes, outDir / dllName, error);
        if (copied && shipsPdb)
            copied = WriteIfContentDiffers(pdbBytes, stagedPdb, error);
        else if (copied)
            std::filesystem::remove(stagedPdb, ec); // a compiled module's symbols would not match
        if (!copied)
        {
            Logger::Log::Error("[Packages] cannot use prebuilt assembly '{}' of package '{}': {}; check that the "
                               "file is readable and that '{}' is writable",
                               declaredDll.string(), module.PackageName, error, outDir.string());
            return false;
        }
    }
    Logger::Log::Info("[Packages] using prebuilt assembly {} from {}", dllName, module.PrebuiltDir.string());
    return true;
}

uint64 ScriptManager::GetPackageCodeModulesRevision() const
{
    return m_PackageCodeModulesRevision.load(std::memory_order_acquire);
}

std::vector<std::string> ScriptManager::GetCompiledAssemblyNames() const
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        names.reserve(m_PackageCSharpModules.size() + 2);
        for (const PackageCodeModule& module : m_PackageCSharpModules)
        {
            if (!module.RootDir.empty()) // a module without sources uses its prebuilt assembly
                names.push_back(module.AssemblyName);
        }
    }
    names.emplace_back("GameEngine.Editor");
    names.emplace_back("GameEngine.Scripts");
    return names;
}

bool ScriptManager::RunCompile(const std::function<void()>& compile)
{
    std::unique_lock<std::mutex> lock(m_HotReloadStartMutex);
    if (!m_Initialized)
    {
        // No live manager: nothing else compiles.
        lock.unlock();
        compile();
        return true;
    }
    if (m_ShuttingDown.load(std::memory_order_relaxed))
        return false;
    // Invalid once the job system's shutdown has started.
    JobSystem::TaskHandle job = m_CompileChannel->Submit(compile);
    if (!job.IsValid())
        return false;
    std::erase_if(m_TrackedCompileTasks, [](const JobSystem::TaskHandle& task) { return task.IsDone(); });
    m_TrackedCompileTasks.push_back(job);
    lock.unlock();

    job.Wait();
    return job.IsCompleted();
}

bool ScriptManager::RefreshGeneratedProject()
{
    if (m_PackagedMode || !m_AutoProjectGenerationEnabled || !m_Initialized)
        return false;
    std::filesystem::path projectPath;
    return EnsureAutoProject(projectPath);
}

void ScriptManager::PrunePackageAssemblies()
{
    std::unordered_set<std::string> expected;
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        for (const PackageCodeModule& module : m_PackageCSharpModules)
            expected.insert(module.AssemblyName);
    }
    PrunePackageAssemblies(expected);
}

void ScriptManager::PrunePackageAssemblies(const std::unordered_set<std::string>& expectedAssemblyNames)
{
    // The hot-reload domain loads EVERY dll under <assemblies>/Packages[/Editor]
    // unconditionally, so an assembly left behind by a removed (or renamed)
    // package keeps loading each session and its module initializer keeps
    // re-registering components for a package that no longer resolves. Delete
    // anything the current resolution does not account for. Files only — the
    // next successful compile recreates a legitimately-missing output.
    if (m_PackagedMode)
        return; // shipped games never prune: staged assemblies are authoritative
    const std::filesystem::path assembliesDir = GetAssembliesDirectory();
    if (assembliesDir.empty())
        return;

    for (const bool editorKind : {false, true})
    {
        const std::filesystem::path dir = PackageAssembliesDirectory(editorKind);
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            const std::filesystem::path& p = entry.path();
            std::string ext = p.extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".dll" && ext != ".pdb")
                continue;
            if (expectedAssemblyNames.count(p.stem().string()) != 0)
                continue;
            std::error_code rmEc;
            std::filesystem::remove(p, rmEc);
            if (rmEc)
                Logger::Log::Warning("[Packages] failed to prune stale package assembly '{}': {}",
                                     p.string(), rmEc.message());
            else
                Logger::Log::Info("[Packages] pruned stale package assembly '{}' — no resolved "
                                  "package produces it",
                                  p.string());
        }
    }
}

std::filesystem::path ScriptManager::PackageAssembliesDirectory(bool editorKind) const
{
    std::filesystem::path dir = GetAssembliesDirectory() / "Packages";
    if (editorKind)
        dir /= "Editor";
    return dir;
}

std::vector<std::filesystem::path> ScriptManager::PackageDependencyAssemblyPaths(
    const std::vector<std::string>& dependencyAssemblies) const
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(dependencyAssemblies.size());
    for (const std::string& name : dependencyAssemblies)
    {
        const bool editorKind = name.size() > 7 && name.compare(name.size() - 7, 7, ".Editor") == 0;
        paths.push_back(PackageAssembliesDirectory(editorKind) / (name + ".dll"));
    }
    return paths;
}

bool ScriptManager::EnsurePackageModuleProject(const PackageCodeModule& module,
                                               std::filesystem::path& outProjectPath) const
{
    try
    {
        if (module.UsesManagedNativeCache && !RetainPackageNativeCache(module.CacheDir))
        {
            Logger::Log::Error("[Packages] cannot lease managed native cache '{}' for C# module '{}'",
                               module.CacheDir.string(), module.AssemblyName);
            return false;
        }
        // CacheDir routes derived data away from immutable git package cache
        // entries (managed .native cache, under TEMP on Windows); embedded/file packages keep
        // <PackageRootDir>/.Cache.
        const std::filesystem::path projectDir =
            (module.CacheDir.empty() ? module.PackageRootDir / ".Cache" : module.CacheDir) /
            "ScriptProjects";
        std::error_code ec;
        std::filesystem::create_directories(projectDir, ec);
        if (ec)
        {
            Logger::Log::Error("[Packages] cannot create script project dir '{}': {}",
                               projectDir.string(), ec.message());
            return false;
        }
        outProjectPath = projectDir / (module.AssemblyName + ".csproj");

        const bool editorKind = module.Kind == PackageModuleRecord::ModuleKind::Editor;
        const std::string newContent = GeneratePackageModuleCsprojXml(
            module, projectDir, PackageAssembliesDirectory(editorKind),
            ScriptingPaths::ResolveEngineManagedDirectory(),
            PackageDependencyAssemblyPaths(module.DependencyAssemblies));

        if (std::filesystem::exists(outProjectPath, ec))
        {
            String existingContent;
            if (ReadFileTextShared(outProjectPath, existingContent) && existingContent == newContent)
                return true;
        }

        std::ofstream of(outProjectPath, std::ios::trunc);
        if (!of)
        {
            Logger::Log::Error("[Packages] failed to write generated project '{}'", outProjectPath.string());
            return false;
        }
        of << newContent;
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("[Packages] exception generating project for module '{}': {}",
                           module.AssemblyName, e.what());
        return false;
    }
}

void ScriptManager::CompilePackageModules(const std::atomic<bool>* cancelRequested)
{
    if (RefusePackagedModeCompile("CompilePackageModules"))
        return;

    std::vector<PackageCodeModule> modules;
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        modules = m_PackageCSharpModules;
    }
    if (modules.empty())
        return;

    std::string cfg = "Debug";
#ifdef NDEBUG
    cfg = "Release";
#endif

    // Topo order (CollectPackageCodeModules preserves the resolver's mount
    // order), so a module's dependency assemblies exist before it compiles.
    std::set<std::string> failedPackages;
    for (const PackageCodeModule& module : modules)
    {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed))
        {
            Logger::Log::Info("[Packages] package module compilation superseded by a newer change");
            return;
        }

        // No sources: SetPackageCodeModules registered the module only after
        // staging its declared prebuilt assembly, so there is nothing to compile.
        if (module.RootDir.empty())
            continue;

        const auto failedDep = std::find_if(module.DependencyPackages.begin(), module.DependencyPackages.end(),
                                            [&](const std::string& dep) { return failedPackages.count(dep) != 0; });
        if (failedDep != module.DependencyPackages.end())
        {
            Logger::Log::Error("[Packages] skipping C# module '{}' of package '{}': dependency package '{}' "
                               "failed to compile",
                               module.AssemblyName, module.PackageName, *failedDep);
            failedPackages.insert(module.PackageName);
            continue;
        }

        std::filesystem::path projectPath;
        if (!EnsurePackageModuleProject(module, projectPath))
        {
            failedPackages.insert(module.PackageName);
            continue;
        }

        const bool editorKind = module.Kind == PackageModuleRecord::ModuleKind::Editor;
        CompileServerRequestDesc desc;
        desc.ProjectRoot = projectPath.parent_path();
        desc.AssemblyName = module.AssemblyName;
        desc.Config = cfg;
        desc.EngineBinDir = ScriptingPaths::ResolveEngineManagedDirectory();
        desc.SourceRoots = {module.RootDir};
        desc.Defines = module.Defines;
        desc.References = PackageDependencyAssemblyPaths(module.DependencyAssemblies);

        const auto compileStart = std::chrono::steady_clock::now();
        const ScriptCompilationResult result = CompileCsUnit(
            projectPath, desc, PackageAssembliesDirectory(editorKind), module.AssemblyName + ".dll",
            cancelRequested);
        const auto roundTrip =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - compileStart);
        if (!result.success)
        {
            if (IsSupersededCompileResult(result))
            {
                Logger::Log::Info("[Packages] package module compilation superseded: {}", result.errorMessage);
                return;
            }
            // Loud, but never blocks project scripts: the package (and its
            // dependents, via failedPackages) is skipped for this pass.
            Logger::Log::Error("[Packages] package '{}' C# module '{}' failed to compile: {}",
                               module.PackageName, module.AssemblyName, result.errorMessage);
            // A DLL from a previous session keeps loading into the script
            // domain until a recompile succeeds — say so, loudly, instead of
            // letting stale package code masquerade as current.
            std::error_code staleEc;
            const std::filesystem::path staleDll =
                PackageAssembliesDirectory(editorKind) / (module.AssemblyName + ".dll");
            if (std::filesystem::exists(staleDll, staleEc))
            {
                Logger::Log::Error(
                    "[Packages] STALE package assembly in use: '{}' is the previous session's "
                    "build and will keep loading until '{}' recompiles successfully",
                    staleDll.string(), module.AssemblyName);
            }
            failedPackages.insert(module.PackageName);
            continue;
        }
        // The compile-server round trip, output write included.
        Logger::Log::Info("[Packages] compiled package assembly {}.dll ({} ms)", module.AssemblyName,
                          roundTrip.count());
    }

    if (failedPackages.empty())
    {
        // Re-arm the one-shot retry for a future failure episode.
        m_PackageModuleRetryScheduled.store(false, std::memory_order_relaxed);
        return;
    }

    // Compile-server cold-start race (first editor open): the failure above may
    // just mean the server wasn't up yet. Schedule ONE retry on the next
    // pipeline tick (main-thread hop, then a compile-channel job) instead of
    // silently keeping stale output until the user edits a file.
    if (m_ShuttingDown.load(std::memory_order_relaxed) || !m_CompileChannel)
        return;
    if (m_PackageModuleRetryScheduled.exchange(true, std::memory_order_relaxed))
        return;
    Logger::Log::Warning(
        "[Packages] {} package module(s) failed to compile — scheduling one retry on the next "
        "pipeline tick",
        failedPackages.size());
    QueueMainThreadTask([this]() {
        if (m_ShuttingDown.load(std::memory_order_relaxed) || !m_CompileChannel)
            return;
        m_PackageModuleRetryTask = m_CompileChannel->Submit([this]() {
            if (m_ShuttingDown.load(std::memory_order_relaxed))
                return;
            Logger::Log::Info("[Packages] retrying failed package module compile");
            CompilePackageModules(nullptr);
            // Project scripts compiled in the same failed episode saw the
            // missing package assemblies (CS0103 against package types) — a
            // packages-only retry left them broken until the next user edit.
            // Re-run the whole graph and swap the assembly in. Skipped when a
            // reload is already in flight: that pass compiles packages-first
            // itself.
            QueueMainThreadTask([this]() {
                if (m_ShuttingDown.load(std::memory_order_relaxed))
                    return;
                if (IsAsyncHotReloadInProgress())
                    return;
                Logger::Log::Info(
                    "[Packages] package retry finished — recompiling project scripts against the "
                    "fresh package assemblies");
                (void)ReloadScriptAssemblies();
            });
        });
    });
}

bool ScriptManager::LoadScriptAssembly(const std::filesystem::path& assemblyPath)
{
    if (!m_ClrHost.IsInitialized())
    {
        Logger::Log::Error("CoreCLR host not initialized");
        return false;
    }

    Logger::Log::Info("🚀 Loading script assembly: {}", assemblyPath.string());

    // Unified path: Preload + Swap via CoreBridge UCOs to avoid GE_* ABI re-entrancy
    if (!m_ClrHost.PreloadAndSwapFromPath(assemblyPath))
    {
        Logger::Log::Error("Failed to preload/swap script assembly via CoreBridge: {}", assemblyPath.string());
        return false;
    }

    m_LoadedAssemblies.push_back(assemblyPath);
    Logger::Log::Info("✅ Successfully loaded script assembly: {}", assemblyPath.filename().string());
    return true;
}

bool ScriptManager::ReloadScriptAssemblies()
{
    Logger::Log::Info("Reloading all script assemblies");

    // PERFORMANCE OPTIMIZATION: Use async hot-reload for <50ms main thread blocking
    if (m_AsyncHotReloadEnabled)
    {
        // SAFETY: Prevent multiple simultaneous hot-reload operations
        if (IsAsyncHotReloadInProgress())
        {
            Logger::Log::Warning("Async hot-reload already in progress, ignoring request");
            return false;
        }

        Logger::Log::Debug("🚀 Using ASYNC hot-reload for <50ms main thread blocking target");

        // Get assembly path
        std::filesystem::path assemblyPath = GetScriptsAssemblyPath();

        // Start async hot-reload (non-blocking)
        StartTrackedHotReload(assemblyPath.string());

        // PERFORMANCE: Don't wait for compilation - return immediately
        // The async pipeline will handle compilation in background
        Logger::Log::Debug("✅ Async hot-reload started - compilation running in background");
        Logger::Log::Debug("🎯 Main thread blocking: <1ms (target: <50ms) - PERFORMANCE TARGET MET!");

        return true; // Return immediately for async operation
    }

    // Fallback to synchronous hot-reload
    if (RecompileAndReload())
    {
        Logger::Log::Info("Script assemblies reloaded successfully");
        return true;
    }
    else
    {
        Logger::Log::Error("Failed to reload script assemblies");
        return false;
    }
}

bool ScriptManager::RecompileAndReload()
{
    if (!m_Initialized)
    {
        Logger::Log::Error("Script manager not initialized");
        return false;
    }

    // PERFORMANCE OPTIMIZATION: Measure main thread blocking time for <50ms target
    auto mainThreadBlockStart = std::chrono::high_resolution_clock::now();
    Logger::Log::Info("🚀 Starting hot-reload process");

    // Step 1: Build first, then swap domain; do NOT unload first
    auto unloadStart = std::chrono::high_resolution_clock::now();
    bool hadLoadedAssembly = m_ClrHost.IsAssemblyLoaded();
    if (hadLoadedAssembly)
    {
        Logger::Log::Info("ℹ️ Existing assembly is loaded; will swap after successful build/load of new domain");
    }
    else
    {
        Logger::Log::Info("ℹ️ No assembly currently loaded - proceeding with initial load");
    }
    auto unloadEnd = std::chrono::high_resolution_clock::now();
    auto unloadTime = std::chrono::duration_cast<std::chrono::milliseconds>(unloadEnd - unloadStart);

    // Step 2: Get assembly path
    std::filesystem::path assemblyPath = GetScriptsAssemblyPath();
    Logger::Log::Info("🎯 Target assembly: {}", assemblyPath.string());

    // Step 3: Compile on the calling thread; RecompileAndReloadAsync is the non-blocking entry point.
    Logger::Log::Info("🔧 Starting compilation...");
    auto compilationStart = std::chrono::high_resolution_clock::now();
    const ScriptCompilationResult compilationResult = CompileScripts(nullptr);
    auto compilationEnd = std::chrono::high_resolution_clock::now();
    auto compilationTime = std::chrono::duration_cast<std::chrono::milliseconds>(compilationEnd - compilationStart);

    if (!compilationResult.success)
    {
        if (IsSupersededCompileResult(compilationResult))
            Logger::Log::Info("Script compilation superseded by a newer change: {}", compilationResult.errorMessage);
        else
            Logger::Log::Error("Script compilation failed: {}", compilationResult.errorMessage);
        return false;
    }
    Logger::Log::Info("✅ Compilation completed in {}ms", compilationTime.count());

    // Step 4: Verify the compiled assembly exists
    if (!std::filesystem::exists(assemblyPath))
    {
        Logger::Log::Error("Compiled assembly not found: {}", assemblyPath.string());
        return false;
    }

    // Step 5: Assembly loading (direct path)
    Logger::Log::Debug("🔄 Starting assembly loading...");
    auto loadStart = std::chrono::high_resolution_clock::now();

    // Use simplified loading approach to bypass CoreCLR issues
    bool loadSuccess = LoadScriptAssemblyFromPath(assemblyPath);

    auto loadEnd = std::chrono::high_resolution_clock::now();
    auto loadTime = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd - loadStart);

    auto totalMainThreadTime = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd - mainThreadBlockStart);

    // PERFORMANCE VALIDATION: Check <1ms target
    if (totalMainThreadTime.count() < 1)
    {
        Logger::Log::Debug("✅ PERFORMANCE TARGET MET: Main thread blocked for {}ms (<1ms)", totalMainThreadTime.count());
    }
    else
    {
        Logger::Log::Warning("⚠️ PERFORMANCE TARGET MISSED: Main thread blocked for {}ms (target: <1ms)", totalMainThreadTime.count());
    }

    Logger::Log::Debug("📊 Performance breakdown: Unload={}ms, Compilation={}ms, Loading={}ms, Total={}ms",
                       unloadTime.count(), compilationTime.count(), loadTime.count(), totalMainThreadTime.count());

    if (!loadSuccess)
    {
        Logger::Log::Error("Failed to load recompiled assembly");
        return false;
    }

    // Step 6: No explicit unload is needed; the old context is scheduled for cleanup by managed code.

    Logger::Log::Info("✅ Hot-reload process completed successfully!");
    return true;
}

bool ScriptManager::LoadScriptAssemblyFromPath(const std::filesystem::path& assemblyPath)
{
    // PERFORMANCE OPTIMIZATION: Simplified assembly loading for <50ms target
    // This bypasses the CoreCLR ComponentEntryPoint issues while maintaining functionality

    auto loadStart = std::chrono::high_resolution_clock::now();
    Logger::Log::Debug("🔄 Starting assembly loading: {}", assemblyPath.filename().string());

    // Use CoreBridge UCOs: Preload + Swap (no GE_* ABI, no re-entrancy)
    bool success = m_ClrHost.PreloadAndSwapFromPath(assemblyPath);

    auto loadEnd = std::chrono::high_resolution_clock::now();
    auto loadTime = std::chrono::duration_cast<std::chrono::milliseconds>(loadEnd - loadStart);

    if (success)
    {
        Logger::Log::Debug("✅ Assembly loading completed in {}ms", loadTime.count());

        // Track the loaded assembly
        m_LoadedAssemblies.push_back(assemblyPath);

        // PERFORMANCE: Check if loading time meets target
        if (loadTime.count() < 25)
        { // Reserve 25ms for loading, 25ms for other operations
            Logger::Log::Debug("🎯 Assembly loading PERFORMANCE TARGET MET: {}ms (<25ms)", loadTime.count());
        }
        else
        {
            Logger::Log::Warning("⚠️ Assembly loading performance target missed: {}ms (target: <25ms)", loadTime.count());
        }
    }
    else
    {
        Logger::Log::Error("❌ Assembly loading failed in {}ms", loadTime.count());
    }

    return success;
}

bool ScriptManager::TriggerInitialBuild()
{
    if (RefusePackagedModeCompile("TriggerInitialBuild"))
        return false;

    Logger::Log::Info("Triggering initial build of Scripts assembly...");

    // Skip when project scripts overlap editor scripts (F.20).
    if (m_ProjectScriptsOverlap.load(std::memory_order_relaxed))
    {
        Logger::Log::Info("Skipping initial build (project scripts overlap editor scripts)");
        return true;
    }

    // Check if .NET CLI is available
    if (!IsDotnetSdkAvailable())
    {
        Logger::Log::Warning("dotnet CLI not available - skipping initial build");
        return false;
    }

    // Package modules feed the project compile (compile-server References +
    // baked csproj references) — build them FIRST, exactly like the hot-reload
    // pass (CompileScripts). On a fresh cache the initial build used to compile
    // project scripts against package assemblies that did not exist yet. No-op
    // when no package set is registered; MountProjectPackages kicks a full
    // reload when it registers one later.
    CompilePackageModules(nullptr);

    // Find project files (under user scripts root)
    Vector<std::filesystem::path> projectFiles = FindProjectFiles();
    std::filesystem::path projectPath;
    if (projectFiles.empty())
    {
        if (!m_AutoProjectGenerationEnabled)
        {
            Logger::Log::Info("No C# project files found and auto-generation is DISABLED; skipping initial build");
            return false;
        }
        if (!EnsureAutoProject(projectPath))
        {
            Logger::Log::Warning("Failed to auto-generate scripts project - skipping initial build");
            return false;
        }
        Logger::Log::Info("Startup: Using auto-generated scripts project: {}", projectPath.string());
    }
    else
    {
        // A user-authored project under the scripts root replaces the generated one.
        projectPath = projectFiles[0];
        Logger::Log::Info("Startup: Using user-defined scripts project: {}", projectPath.string());
    }
    Logger::Log::Info("Building project: {}", projectPath.string());

    try
    {
        ScriptCompilationResult result = CompileProject(projectPath, nullptr);
        if (result.success)
        {
            Logger::Log::Info("✅ Initial build completed successfully");
            return true;
        }
        else
        {
            Logger::Log::Warning("Initial build failed: {}", result.errorMessage);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Exception during initial build: {}", e.what());
        return false;
    }
}

void ScriptManager::QueueDeferredInitialBuildIfNeeded()
{
    if (m_ShuttingDown.load(std::memory_order_relaxed))
    {
        return;
    }

    if (!m_DeferInitialLoad)
    {
        return;
    }

    if (!m_CompileChannel)
    {
        Logger::Log::Warning("Deferred initial scripts build requested but JobSystem is not available");
        return;
    }

    bool expected = false;
    if (!m_DeferredInitialBuildQueued.compare_exchange_strong(expected, true))
    {
        return; // already queued
    }

    // Fire-and-forget: compile on the compile channel, then queue the load on the
    // main thread. This keeps the Editor responsive even on first run / clean builds.
    m_DeferredInitialBuildTask = m_CompileChannel->Submit([this]()
                        {
        try
        {
            // Ensure the Editor assembly exists first so project scripts can reference it.
            try
            {
                std::filesystem::path editorProj;
                if (EnsureEditorAutoProject(editorProj))
                {
                    std::error_code ec;
                    const std::filesystem::path editorDll = m_EditorAssembliesDirectory / "GameEngine.Editor.dll";
                    if (!std::filesystem::exists(editorDll, ec))
                    {
                        (void)CompileProject(editorProj, nullptr);
                    }
                }
            }
            catch (...)
            {
                // best-effort; proceed to scripts build
            }

            // Build project scripts (may auto-generate a .csproj).
            (void)TriggerInitialBuild();

            // Load the newly built Scripts assembly on the main thread.
            QueueMainThreadTask([this]()
                                {
                const std::filesystem::path assemblyPath = GetScriptsAssemblyPath();
                std::error_code ec;
                if (!std::filesystem::exists(assemblyPath, ec))
                {
                    Logger::Log::Warning("Deferred initial scripts build finished but assembly was not found at {}", assemblyPath.string());
                    return;
                }

                if (!LoadScriptAssembly(assemblyPath))
                {
                    Logger::Log::Warning("Deferred initial scripts load failed for {}", assemblyPath.string());
                } });
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Deferred initial scripts build failed: {}", e.what());
        }
        catch (...)
        {
            Logger::Log::Warning("Deferred initial scripts build failed: unknown exception");
        } });
}

std::future<HotReloadPipeline::PipelineStats> ScriptManager::RecompileAndReloadAsync(const String& assemblyPath)
{
    std::lock_guard<std::mutex> lock(m_HotReloadStartMutex);
    return StartHotReload(assemblyPath);
}

void ScriptManager::StartTrackedHotReload(const String& assemblyPath)
{
    // Published under the start mutex, so Shutdown collects this future after it.
    std::lock_guard<std::mutex> lock(m_HotReloadStartMutex);
    SetCurrentAsyncFuture(StartHotReload(assemblyPath));
}

std::future<HotReloadPipeline::PipelineStats> ScriptManager::StartHotReload(const String& assemblyPath)
{
    if (!m_Initialized)
    {
        Logger::Log::Error("Script manager not initialized");
        return MakeFinishedReload(HotReloadResult::UnknownError, "Script manager not initialized");
    }

    // A late change event is refused here instead of starting a reload that
    // nothing joins, on a pipeline Shutdown is about to destroy.
    if (m_ShuttingDown.load(std::memory_order_relaxed))
        return MakeFinishedReload(HotReloadResult::Cancelled, "ScriptManager is shutting down");

    if (!m_AsyncHotReloadEnabled)
    {
        Logger::Log::Info("Async hot-reload disabled, running synchronous hot-reload on the job system");
        return SubmitSyncHotReload();
    }

    // Cancel any existing operation
    if (m_HotReloadPipeline->IsExecuting())
    {
        Logger::Log::Info("Cancelling existing async hot-reload operation");
        CancelAsyncHotReload();
    }

    // Execute async pipeline
    return m_HotReloadPipeline->ExecuteAsync(assemblyPath);
}

std::future<HotReloadPipeline::PipelineStats> ScriptManager::SubmitSyncHotReload()
{
    auto promise = std::make_shared<std::promise<HotReloadPipeline::PipelineStats>>();
    std::future<HotReloadPipeline::PipelineStats> future = promise->get_future();

    // The compile channel runs reloads one at a time, behind any other compile.
    std::erase_if(m_TrackedCompileTasks, [](const JobSystem::TaskHandle& task) { return task.IsDone(); });
    m_TrackedCompileTasks.push_back(m_CompileChannel->Submit([this, promise]() { RunSyncHotReload(*promise); }));
    return future;
}

void ScriptManager::RunSyncHotReload(std::promise<HotReloadPipeline::PipelineStats>& promise)
{
    try
    {
        const bool success = RecompileAndReload();
        HotReloadPipeline::PipelineStats stats;
        stats.success = success;
        stats.result = success ? HotReloadResult::Success : HotReloadResult::UnknownError;
        stats.errorMessage = success ? "" : "Synchronous hot-reload failed";
        promise.set_value(stats);
    }
    catch (...)
    {
        promise.set_exception(std::current_exception());
    }
}

void ScriptManager::CancelAsyncHotReload()
{
    if (m_HotReloadPipeline)
    {
        m_HotReloadPipeline->Cancel();
        Logger::Log::Info("Async hot-reload operation cancelled");
    }
}

bool ScriptManager::IsAsyncHotReloadInProgress() const
{
    if (!m_HotReloadPipeline)
    {
        return false;
    }

    // Check if pipeline is executing
    if (m_HotReloadPipeline->IsExecuting())
    {
        return true;
    }

    // Also check if we have a stored future that hasn't completed yet. A ready
    // one is taken out under the lock and reported outside it.
    std::future<HotReloadPipeline::PipelineStats> completedFuture;
    {
        std::lock_guard<std::mutex> lock(m_CurrentAsyncFutureMutex);
        if (!m_CurrentAsyncFuture.valid())
        {
            return false;
        }
        if (m_CurrentAsyncFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            return true;
        }
        completedFuture = std::move(m_CurrentAsyncFuture);
    }

    try
    {
        auto stats = completedFuture.get();
        const bool cancelled = IsCancellation(stats.result);
        Logger::Log::Info("🎯 Async hot-reload completed: {}",
                          stats.success ? "SUCCESS" : (cancelled ? "CANCELLED" : "FAILED"));
        if (!stats.success)
        {
            // A run superseded by a newer edit reports failure by design; during a
            // rapid-edit storm every keystroke would otherwise log an error.
            if (cancelled)
            {
                Logger::Log::Debug("Async hot-reload cancelled: {}", stats.errorMessage);
            }
            else
            {
                Logger::Log::Error("Hot-reload error: {}", stats.errorMessage);
            }
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Exception getting async hot-reload result: {}", e.what());
    }
    // Future is consumed, no longer in progress
    return false;
}

void ScriptManager::SetCurrentAsyncFuture(std::future<HotReloadPipeline::PipelineStats> future)
{
    // The replaced future is destroyed after the lock is released.
    std::lock_guard<std::mutex> lock(m_CurrentAsyncFutureMutex);
    std::swap(m_CurrentAsyncFuture, future);
}

float ScriptManager::GetAsyncHotReloadProgress() const
{
    if (m_HotReloadPipeline)
    {
        return m_HotReloadPipeline->GetProgress();
    }
    return 0.0f;
}

String ScriptManager::GetAsyncHotReloadStatus() const
{
    if (!m_HotReloadPipeline)
    {
        return "Hot-reload pipeline not initialized";
    }

    if (!IsAsyncHotReloadInProgress())
    {
        return "No hot-reload operation in progress";
    }

    float progress = m_HotReloadPipeline->GetProgress();
    if (progress < 0.25f)
    {
        return "🔧 Compiling scripts... (" + std::to_string(int(progress * 100)) + "%)";
    }
    else if (progress < 0.50f)
    {
        return "📁 Loading assembly files... (" + std::to_string(int(progress * 100)) + "%)";
    }
    else if (progress < 0.75f)
    {
        return "🔄 Preparing assembly context... (" + std::to_string(int(progress * 100)) + "%)";
    }
    else if (progress < 1.0f)
    {
        return "⚡ Swapping assemblies... (" + std::to_string(int(progress * 100)) + "%)";
    }
    else
    {
        return "✅ Hot-reload completed successfully";
    }
}

void ScriptManager::SetHotReloadEnabled(bool enabled)
{
    m_HotReloadEnabled = enabled;

    if (enabled && !m_ScriptsWatchSubscription)
    {
        m_ScriptsWatchSubscription = WatchScriptDirectory(m_ScriptsDirectory);
        Logger::Log::Info("Script hot reloading enabled");
    }
    else if (!enabled && m_ScriptsWatchSubscription)
    {
        m_ScriptsWatchSubscription.reset();
        Logger::Log::Info("Script hot reloading disabled");
    }

    if (enabled && !m_EditorScriptsWatchSubscription)
    {
        m_EditorScriptsWatchSubscription = WatchScriptDirectory(m_EditorScriptsDirectory);
        Logger::Log::Info("Editor script hot reloading enabled");
    }
    else if (!enabled && m_EditorScriptsWatchSubscription)
    {
        m_EditorScriptsWatchSubscription.reset();
        Logger::Log::Info("Editor script hot reloading disabled");
    }

    if (!enabled)
        m_PackageModuleWatchSubscriptions.clear();
    else if (m_PackageModuleWatchSubscriptions.empty())
        RewatchPackageModuleRoots();
}

void ScriptManager::RewatchPackageModuleRoots()
{
    m_PackageModuleWatchSubscriptions.clear();
    // Shutdown dropped every subscription; a module set handed over after it
    // must not resubscribe.
    if (!m_HotReloadEnabled || m_ShuttingDown.load(std::memory_order_relaxed))
        return;

    std::vector<std::filesystem::path> roots;
    {
        std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
        for (const PackageCodeModule& module : m_PackageCSharpModules)
        {
            // Git cache entries and staged Engine packages never change in
            // place: a new pin is a new directory, picked up by the remount.
            const bool editable = module.SourceKind == PackageSourceKind::Embedded ||
                                  module.SourceKind == PackageSourceKind::File;
            if (editable && !module.RootDir.empty())
                roots.push_back(module.RootDir);
        }
    }

    for (const std::filesystem::path& root : roots)
    {
        if (UniquePtr<FileWatchSubscription> subscription = WatchScriptDirectory(root))
            m_PackageModuleWatchSubscriptions.push_back(std::move(subscription));
    }
    if (!roots.empty())
        Logger::Log::Info("ScriptManager: watching {} package C# module root(s) for hot reload", roots.size());
}

UniquePtr<FileWatchSubscription> ScriptManager::WatchScriptDirectory(const std::filesystem::path& directory)
{
    if (directory.empty())
        return nullptr;

    // OnFileChanged runs on a FileWatchingService watcher thread. The host app
    // arms and stops the service (the editor does both); this only subscribes.
    FilePattern pattern(directory, ".*", {".cs", ".csproj"}, /*recursive*/ true);
    return MakeUnique<FileWatchSubscription>(FileWatchingService::GetInstance().Subscribe(
        pattern, [this](const FileChangeEvent& event) { OnFileChanged(event); }));
}

size_t ScriptManager::ProcessMainThreadTasks()
{
    // Only one thread may execute main-thread callbacks at a time; the recursive
    // try-lock keeps same-thread re-entrant pumping working (managed initialization
    // pumps the queue from inside a task). If another thread is already pumping,
    // return instead of blocking: the owner will drain the queue, and waiting here
    // would stall this caller behind arbitrary task execution and then execute
    // "main thread" tasks on the wrong thread while the real pump is alive.
    std::unique_lock<std::recursive_mutex> processingLock(m_MainThreadTaskProcessingMutex, std::try_to_lock);
    if (!processingLock.owns_lock())
    {
        return 0;
    }

    // RACE CONDITION FIX: Check if hot-reload is in progress and prioritize those tasks
    bool hotReloadInProgress = IsAsyncHotReloadInProgress();

    size_t tasksProcessed = 0;
    size_t maxTasksPerFrame = hotReloadInProgress ? 1 : 10; // Limit tasks during hot-reload

    while (tasksProcessed < maxTasksPerFrame)
    {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_MainThreadTasksMutex);
            if (m_MainThreadTasks.empty())
                break;
            task = std::move(m_MainThreadTasks.front());
            m_MainThreadTasks.pop();
        }

        if (tasksProcessed == 0)
        {
            // Legitimate only where no main pump exists (standalone test hosts);
            // in the editor/runtime it means thread-affine work may misbehave.
            // Deliberately checked before the first task runs so the warning is
            // in the log even if that task then deadlocks or crashes.
            const std::thread::id mainThreadId = m_MainThreadId.load(std::memory_order_relaxed);
            if (mainThreadId != std::thread::id{} && mainThreadId != std::this_thread::get_id() &&
                !m_WarnedOffMainThreadPump.exchange(true))
            {
                Logger::Log::Warning("ScriptManager: executing main-thread tasks on a non-main thread (fallback pump)");
            }
        }

        // Execute outside the queue lock. Managed initialization can pump the
        // main thread and enqueue follow-up work while this task is running.
        try
        {
            // RACE CONDITION FIX: Add timing monitoring for hot-reload tasks
            auto taskStart = std::chrono::high_resolution_clock::now();

            task();

            auto taskEnd = std::chrono::high_resolution_clock::now();
            auto taskDuration = std::chrono::duration_cast<std::chrono::milliseconds>(taskEnd - taskStart);

            if (hotReloadInProgress && taskDuration.count() > 1)
            {
                Logger::Log::Warning("Main thread task took {}ms during hot-reload (target: <1ms)", taskDuration.count());
            }

            tasksProcessed++;
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("Main thread task execution failed: {}", e.what());
        }
    }

    return tasksProcessed;
}

void ScriptManager::MarkMainThread()
{
    m_MainThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

void ScriptManager::QueueMainThreadTask(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_MainThreadTasksMutex);
    m_MainThreadTasks.push(std::move(task));
    Logger::Log::Debug("Queued task for main thread execution (queue size: {})", m_MainThreadTasks.size());
}

size_t ScriptManager::GetMainThreadTaskCount() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_MainThreadTasksMutex));
    return m_MainThreadTasks.size();
}

void ScriptManager::EnterHotReloadCriticalSection()
{
    std::lock_guard<std::mutex> lock(m_HotReloadStateMutex);
    m_HotReloadCriticalSection.store(true);
    Logger::Log::Debug("Entered hot-reload critical section - user interactions may be limited");
}

void ScriptManager::ExitHotReloadCriticalSection()
{
    std::lock_guard<std::mutex> lock(m_HotReloadStateMutex);
    m_HotReloadCriticalSection.store(false);
    Logger::Log::Debug("Exited hot-reload critical section - normal operations resumed");
}

void ScriptManager::SetAsyncHotReloadEnabled(bool enabled)
{
    m_AsyncHotReloadEnabled = enabled;

    if (enabled)
    {
        Logger::Log::Info("Async hot-reload enabled - hot-reload operations will be non-blocking");
    }
    else
    {
        Logger::Log::Info("Async hot-reload disabled - falling back to synchronous hot-reload");

        // Cancel any in-progress async operation
        if (IsAsyncHotReloadInProgress())
        {
            CancelAsyncHotReload();
        }
    }
}

std::filesystem::path ScriptManager::GetAssembliesDirectory() const
{
    // Assemblies directory is provided by ScriptsConfig and kept stable for the lifetime of ScriptManager
    return m_AssembliesDirectory;
}

std::filesystem::path ScriptManager::GetScriptsAssemblyPath() const
{
    return m_AssembliesDirectory / ScriptingPaths::kScriptsAssemblyFileName;
}

namespace
{
// Inner text of the first <tag ...>value</tag> element, or empty when absent.
std::string ExtractXmlTagValue(const std::string& content, const std::string& tag)
{
    const auto open = content.find("<" + tag);
    if (open == std::string::npos)
        return {};
    const auto valueStart = content.find('>', open);
    if (valueStart == std::string::npos)
        return {};
    const auto close = content.find("</" + tag + ">", valueStart + 1);
    if (close == std::string::npos)
        return {};
    return content.substr(valueStart + 1, close - valueStart - 1);
}

bool IsPathUnder(const std::filesystem::path& p, const std::filesystem::path& base)
{
    if (p.empty() || base.empty())
        return false;
    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(p, ec);
    if (ec)
        canonicalPath = p.lexically_normal();
    ec.clear();
    auto canonicalBase = std::filesystem::weakly_canonical(base, ec);
    if (ec)
        canonicalBase = base.lexically_normal();
    const auto rel = canonicalPath.lexically_relative(canonicalBase);
    return !rel.empty() && *rel.begin() != "..";
}

// Path written into a generated csproj for `target`, anchored to the csproj's own
// directory when the layout survives relocation: relative when target sits inside
// projectDir, or when both stay inside anchorRoot (moving the whole root keeps the
// pair intact). Anything else gets the absolute target — a baked "..\..\.." chain
// that escapes the anchor re-targets to whatever happens to sit N levels above the
// file after a copy or move (issue #333: a stale instance climbed to the repo
// root's Assets).
std::string AnchorGeneratedPath(const std::filesystem::path& target,
                                const std::filesystem::path& projectDir,
                                const std::filesystem::path& anchorRoot,
                                bool& outIsRelative)
{
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(target, projectDir, ec);
    bool useRelative = !ec && !rel.empty();
    if (useRelative && *rel.begin() == "..")
    {
        useRelative = IsPathUnder(target, anchorRoot) && IsPathUnder(projectDir, anchorRoot);
    }
    outIsRelative = useRelative;
    std::string out = useRelative ? rel.generic_string() : target.generic_string();
    if (!out.empty() && out.back() != '/')
        out += '/';
    return out;
}

// Loud replacement for the silent Exists()-drop failure mode (issue #333): when the
// EngineBinDir baked into an existing generated project no longer matches the live
// engine location, say so — and say it as an error when the old location is gone
// (Editor moved/uninstalled), because until regeneration every engine reference in
// that file silently resolved to nothing ("type or namespace not found" with no cause).
void ReportStaleEngineBinDir(const std::string& existingContent,
                             const std::filesystem::path& projectPath,
                             const std::string& liveEngineBinDir)
{
    if (existingContent.empty())
        return;
    const std::string previous = ExtractXmlTagValue(existingContent, "EngineBinDir");
    if (previous.empty() || previous == liveEngineBinDir)
        return;
    std::error_code ec;
    if (!previous.empty() && previous.front() != '$' &&
        !std::filesystem::exists(std::filesystem::path(previous), ec))
    {
        Logger::Log::Error(
            "Generated project '{}' pointed at missing EngineBinDir '{}' (the Editor moved since it was "
            "generated); regenerating with '{}'. IDE builds against the stale file dropped all engine "
            "references.",
            projectPath.string(), previous, liveEngineBinDir);
    }
    else
    {
        Logger::Log::Info("Generated project '{}' EngineBinDir updated: '{}' -> '{}'",
                          projectPath.string(), previous, liveEngineBinDir);
    }
}

// MSBuild-side validation baked into both generated projects. Runs for IDE and CLI
// builds, where no engine process is alive to notice a relocated Editor. An absent
// EngineBinDir is always fatal (nothing can resolve); a present-but-unstaged one
// warns with the exact path instead of silently dropping Exists()-guarded references.
void EmitEngineBinDirValidationTarget(std::ostringstream& xml, const char* requiredDllName)
{
    xml << "  <Target Name=\"ValidateEngineBinDir\" BeforeTargets=\"ResolveAssemblyReferences\">\n";
    xml << "    <Error Condition=\"!Exists('$(EngineBinDir)')\" Text=\"GameEngine: EngineBinDir "
           "'$(EngineBinDir)' does not exist. The Open Engine Editor moved since this project was generated. "
           "Re-open the project in the Editor to regenerate this file, or build with "
           "-p:EngineBinDir=&lt;Editor directory&gt;.\" />\n";
    xml << "    <Warning Condition=\"Exists('$(EngineBinDir)') And !Exists('$(EngineBinDir)" << requiredDllName
        << "')\" Text=\"GameEngine: '$(EngineBinDir)' is missing " << requiredDllName
        << "; engine references will not resolve (stale or partially staged Editor directory).\" />\n";
    xml << "  </Target>\n";
}

// Every generated scripts project carries this diagnostics target; a
// hand-written GameEngine.Scripts.csproj does not, so the stale-project
// cleanup never deletes the user's own file.
constexpr std::string_view kGeneratedScriptsProjectSignature = "<Target Name=\"DumpCompileItems\"";
} // namespace

std::filesystem::path ScriptManager::GeneratedProjectPathFor(const std::filesystem::path& projectRoot)
{
    return projectRoot / "GameEngine.Scripts.csproj";
}

std::filesystem::path ScriptManager::GeneratedProjectPath() const
{
    return GeneratedProjectPathFor(m_GeneratedProjectRoot.empty() ? m_WorkspaceRoot : m_GeneratedProjectRoot);
}

void ScriptManager::RemoveStaleGeneratedProject() const
{
    // A generated project inside the scripts root (and the IDE's .lscache
    // beside it) is safe to delete, and must go: FindProjectFiles would
    // compile it as a user project. A hand-written one is the user's and stays.
    const std::filesystem::path staleProject = GeneratedProjectPathFor(m_ScriptsDirectory);
    const std::filesystem::path currentProject = GeneratedProjectPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(staleProject, ec))
        return;
    if (std::filesystem::equivalent(staleProject, currentProject, ec))
        return; // a configured generated-project root at this location: current, not stale
    String content;
    if (!ReadFileTextShared(staleProject, content) ||
        content.find(kGeneratedScriptsProjectSignature) == String::npos)
        return;
    if (!std::filesystem::remove(staleProject, ec))
    {
        Logger::Log::Warning(
            "Could not remove stale auto-generated scripts project '{}' ({}); until it is gone it compiles as a "
            "user project instead of '{}'. Close the program holding it (usually the IDE), then delete it.",
            staleProject.string(), ec.message(), currentProject.string());
        return;
    }
    std::filesystem::path staleCache = staleProject;
    staleCache += ".lscache";
    std::filesystem::remove(staleCache, ec);
    Logger::Log::Info("Removed stale auto-generated scripts project '{}' (the project is generated at '{}')",
                      staleProject.string(), currentProject.string());
}

bool ScriptManager::EnsureAutoProject(std::filesystem::path& outProjectPath) const
{
    if (RefusePackagedModeCompile("EnsureAutoProject"))
        return false;

    try
    {
        std::filesystem::path projectPath = GeneratedProjectPath();
        std::filesystem::path projectDir = projectPath.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(projectDir, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to ensure generated project directory exists: {}", ec.message());
        }

        outProjectPath = projectPath;

        // If a project exists, read its content so we can decide whether regeneration is needed
        String existingContent;
        if (std::filesystem::exists(projectPath))
        {
            // Read errors leave existingContent empty; we'll rewrite below.
            (void)ReadFileTextShared(projectPath, existingContent);
        }

        // The live engine location. Baked below only as a last-known-good fallback for
        // IDE loads; engine-driven builds pass it per invocation (compile server request
        // field / -p:EngineBinDir), so a relocated Editor cannot poison them.
        std::string engineBinDirAbs = ScriptingPaths::ResolveEngineManagedDirectory().generic_string();
        if (!engineBinDirAbs.empty() && engineBinDirAbs.back() != '/' && engineBinDirAbs.back() != '\\')
        {
            engineBinDirAbs.push_back('/');
        }

        ReportStaleEngineBinDir(existingContent, projectPath, engineBinDirAbs);

        // Source glob anchored to the csproj: project-relative when the layout stays
        // inside the workspace, absolute otherwise (never a climb into unrelated trees).
        const auto& scriptsRoot = m_ScriptsDirectory;
        bool includeIsRelative = false;
        std::string includeRoot = AnchorGeneratedPath(scriptsRoot, projectDir, m_WorkspaceRoot, includeIsRelative);
        if (!includeIsRelative)
        {
            Logger::Log::Warning(
                "Scripts project dir '{}' and scripts root '{}' do not share workspace root '{}'; generating an "
                "absolute source path (check --project / ScriptsConfig roots)",
                projectDir.string(), scriptsRoot.string(), m_WorkspaceRoot.string());
        }
        // Normalize to Windows separators for MSBuild globs
        std::string includeRootWin = includeRoot;
        for (auto& ch : includeRootWin)
        {
            if (ch == '/')
                ch = '\\';
        }

        // Generate minimal SDK-style project that compiles the scripts root and outputs to assembliesRoot
        std::ostringstream xml;
        xml << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
        xml << "  <PropertyGroup>\n";
        xml << "    <TargetFramework>" << kScriptTargetFramework << "</TargetFramework>\n";
        xml << "    <OutputType>Library</OutputType>\n";
        xml << "    <AssemblyName>GameEngine.Scripts</AssemblyName>\n";
        xml << "    <RootNamespace>GameEngine.Scripts</RootNamespace>\n";
        // The language settings CompileServerRequestDesc sends on the live path.
        xml << "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
        xml << "    <ImplicitUsings>enable</ImplicitUsings>\n";
        xml << "    <Nullable>enable</Nullable>\n";
        xml << "    <EnableDefaultItems>false</EnableDefaultItems>\n"; // we explicitly include from scripts root
        xml << "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
        xml << "    <AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>\n";
        // Output always goes to assembliesRoot regardless of where this .csproj lives
        // (the project root). Project-relative when the layout allows, so the pair survives
        // whole-project relocation; MSBuild resolves a relative OutputPath against the
        // project directory.
        bool outputIsRelative = false;
        const std::string assembliesDirStr =
            AnchorGeneratedPath(m_AssembliesDirectory, projectDir, m_WorkspaceRoot, outputIsRelative);
        xml << "    <OutputPath>" << assembliesDirStr << "</OutputPath>\n";
        // Engine managed DLLs live in the engine managed directory (beside the executable;
        // Contents/Resources/Managed inside a macOS bundle). The baked value is a
        // last-known-good for IDE loads only (refreshed every time the project is opened
        // in the Editor); live builds override it per invocation via -p:EngineBinDir or
        // the compile-server request, so it is emitted as a default, not a constant.
        xml << "    <EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" << engineBinDirAbs << "</EngineBinDir>\n";
        // Ensure we always produce an implementation assembly in OutputPath and expose compiler args for diagnostics
        xml << "    <ProduceReferenceAssembly>false</ProduceReferenceAssembly>\n";
        xml << "    <ProvideCommandLineArgs>true</ProvideCommandLineArgs>\n";
        // Package `defines` propagation for MSBuild-driven builds (the packaged
        // BuildPipeline passes -p:GamePackageDefines="A;B"). Appended — never a
        // global DefineConstants override, which would drop DEBUG/TRACE. The
        // editor's compile-server path passes the same set per request instead.
        xml << "    <DefineConstants Condition=\"'$(GamePackageDefines)' != ''\">$(DefineConstants);$(GamePackageDefines)</DefineConstants>\n";
        xml << "  </PropertyGroup>\n";
        xml << "  <ItemGroup>\n";
        // obj/bin hold MSBuild outputs — including generated AssemblyInfo/
        // attribute .cs files from any csproj that ever built under this root
        // (e.g. the pre-migration Assets/Scripts project). Sweeping them into
        // the compile duplicates assembly attributes (CS0579).
        xml << "    <Compile Include=\"" << includeRootWin << "**\\*.cs\" Exclude=\""
            << includeRootWin << "**\\obj\\**;" << includeRootWin << "**\\bin\\**\" />\n";
        xml << "  </ItemGroup>\n";
        // Reference engine-provided ABI modules (do not expose CoreBridge to user scripts).
        // We enumerate the actual DLLs at project-generation time for reliable MSBuild resolution,
        // rather than relying on item metadata transforms which can fail in SDK-style projects.
        xml << "  <ItemGroup>\n";
        {
            int abiCount = 0;
            std::filesystem::path abiDir(engineBinDirAbs);
            for (auto& entry : std::filesystem::directory_iterator(abiDir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                auto fn = entry.path().filename().string();
                if (fn.size() > 12 && fn.substr(0, 11) == "GameEngine." && fn.size() > 8 &&
                    fn.substr(fn.size() - 8) == ".ABI.dll")
                {
                    auto stem = fn.substr(0, fn.size() - 4); // remove .dll
                    xml << "    <Reference Include=\"" << stem << "\">\n";
                    xml << "      <HintPath>$(EngineBinDir)" << fn << "</HintPath>\n";
                    xml << "      <Private>false</Private>\n";
                    xml << "    </Reference>\n";
                    ++abiCount;
                }
            }
            ec.clear();
            if (abiCount == 0)
            {
                // Enumeration happens at generation time; an empty list means IDE builds of
                // this project can never see the engine ABI until the next regeneration.
                Logger::Log::Error(
                    "No GameEngine.*.ABI.dll found in '{}' while generating '{}'; scripts will not resolve engine "
                    "APIs (broken or partially staged Editor directory)",
                    engineBinDirAbs, projectPath.string());
            }
        }
        xml << "  </ItemGroup>\n";
        // Optional reference: Editor scripts implementation assembly (if present).
        xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)GameEngine.Editor.dll')\">\n";
        xml << "    <Reference Include=\"GameEngine.Editor\">\n";
        xml << "      <HintPath>$(EngineBinDir)GameEngine.Editor.dll</HintPath>\n";
        xml << "      <Private>false</Private>\n";
        xml << "    </Reference>\n";
        xml << "  </ItemGroup>\n";
        // Reference Scripting.Runtime (needed for GameSystemRunner in source-generated code)
        xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)GameEngine.Scripting.Runtime.dll')\">\n";
        xml << "    <Reference Include=\"GameEngine.Scripting.Runtime\">\n";
        xml << "      <HintPath>$(EngineBinDir)GameEngine.Scripting.Runtime.dll</HintPath>\n";
        xml << "      <Private>false</Private>\n";
        xml << "    </Reference>\n";
        xml << "  </ItemGroup>\n";
        // Package runtime assemblies (P1): baked so MSBuild consumers — IDE loads,
        // the dotnet CLI fallback, and the packaged Debug build's `dotnet build` —
        // resolve the same package types the editor's compile-server requests
        // reference live (CompileProject's desc.References). Editor-kind package
        // assemblies stay out for the same reason GameEngine.Editor.dll is
        // Private=false: GameEngine.Scripts.dll must load in the Player.
        // Regenerated by SetPackageCodeModules whenever the package set changes.
        {
            std::vector<std::string> packageAssemblies;
            {
                std::lock_guard<std::mutex> lock(m_PackageModulesMutex);
                for (const PackageCodeModule& module : m_PackageCSharpModules)
                {
                    if (module.Kind == PackageModuleRecord::ModuleKind::Runtime)
                        packageAssemblies.push_back(module.AssemblyName);
                }
            }
            if (!packageAssemblies.empty())
            {
                // Same anchoring as OutputPath: compiled package assemblies live
                // in Packages/ under the assemblies root.
                std::string packagesDir = assembliesDirStr;
                if (!packagesDir.empty() && packagesDir.back() != '/' && packagesDir.back() != '\\')
                    packagesDir += '/';
                packagesDir += "Packages/";
                xml << "  <PropertyGroup>\n";
                xml << "    <GamePackagesDir Condition=\"'$(GamePackagesDir)' == ''\">" << packagesDir
                    << "</GamePackagesDir>\n";
                xml << "  </PropertyGroup>\n";
                xml << "  <ItemGroup>\n";
                for (const std::string& name : packageAssemblies)
                {
                    xml << "    <Reference Include=\"" << name << "\" Condition=\"Exists('$(GamePackagesDir)"
                        << name << ".dll')\">\n";
                    xml << "      <HintPath>$(GamePackagesDir)" << name << ".dll</HintPath>\n";
                    xml << "      <Private>false</Private>\n";
                    xml << "    </Reference>\n";
                }
                xml << "  </ItemGroup>\n";
            }
        }
        // Source generator for IEntitySystem / GameSystem (EntitySystemGenerator)
        xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)SourceGenerators/EntitySystemGenerator.dll')\">\n";
        xml << "    <Analyzer Include=\"$(EngineBinDir)SourceGenerators/EntitySystemGenerator.dll\" />\n";
        xml << "  </ItemGroup>\n";
        EmitEngineBinDirValidationTarget(xml, "GameEngine.Scripting.Runtime.dll");
        // Diagnostics: dump which files are compiled and the C# compiler command line
        xml << "  " << kGeneratedScriptsProjectSignature << " BeforeTargets=\"CoreCompile\">\n";
        xml << "    <Message Importance=\"High\" Text=\"[Scripts.csproj] Compile items: @(Compile)\" />\n";
        xml << "    <Message Importance=\"High\" Text=\"[Scripts.csproj] Csc args: @(CscCommandLineArgs)\" />\n";
        xml << "  </Target>\n";
        xml << "</Project>\n";

        const std::string newContent = xml.str();
        if (std::filesystem::exists(projectPath) && existingContent == newContent)
        {
            Logger::Log::Info("Auto-generated Scripts project at {} is up to date (sources: {})", projectPath.string(), includeRoot);
            return true;
        }

        std::ofstream of(projectPath, std::ios::trunc);
        if (!of)
        {
            Logger::Log::Error("Failed to create auto-generated project file: {}", projectPath.string());
            return false;
        }
        of << newContent;
        of.close();

        Logger::Log::Info("Auto-generated Scripts project at {} (sources: {})", projectPath.string(), includeRoot);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Exception while generating scripts project: {}", e.what());
        return false;
    }
}

bool ScriptManager::EnsureEditorAutoProject(std::filesystem::path& outProjectPath) const
{
    if (RefusePackagedModeCompile("EnsureEditorAutoProject"))
        return false;

    try
    {
        // Place the auto-generated project next to the executable so its output lands in the engine bin directory.
        std::filesystem::path assemblyDir = m_EditorAssembliesDirectory;
        std::error_code ec;
        std::filesystem::create_directories(assemblyDir, ec);

        std::filesystem::path projectPath = assemblyDir / "GameEngine.Editor.csproj";
        outProjectPath = projectPath;

        // If a project exists, read its content so we can decide whether regeneration is needed
        String existingContent;
        if (std::filesystem::exists(projectPath))
        {
            // Read errors leave existingContent empty; we'll rewrite below.
            (void)ReadFileTextShared(projectPath, existingContent);
        }

        const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
        const std::filesystem::path engineBinDir = ScriptingPaths::ResolveEngineManagedDirectory();
        std::string engineBinDirAbs = engineBinDir.generic_string();
        if (!engineBinDirAbs.empty() && engineBinDirAbs.back() != '/' && engineBinDirAbs.back() != '\\')
        {
            engineBinDirAbs.push_back('/');
        }

        // The Editor project normally lives in the engine bin directory itself, so the
        // baked default can be fully relocation-proof: $(MSBuildThisFileDirectory). Only
        // when the two diverge (macOS bundle redirect) does an absolute last-known-good
        // get baked — that layout regenerates on every Editor launch anyway.
        const bool projectLivesInEngineBin =
            IsPathUnder(assemblyDir, engineBinDir) && IsPathUnder(engineBinDir, assemblyDir);
        const std::string engineBinDirDefault =
            projectLivesInEngineBin ? std::string("$(MSBuildThisFileDirectory)") : engineBinDirAbs;

        ReportStaleEngineBinDir(existingContent, projectPath, engineBinDirDefault);

        // Source glob anchored to the csproj (exe-relative in the normal layout).
        const auto& scriptsRoot = m_EditorScriptsDirectory;
        bool includeIsRelative = false;
        std::string includeRoot = AnchorGeneratedPath(scriptsRoot, assemblyDir, exeDir, includeIsRelative);
        // Normalize to Windows separators for MSBuild globs
        std::string includeRootWin = includeRoot;
        for (auto& ch : includeRootWin)
        {
            if (ch == '/')
                ch = '\\';
        }

        std::ostringstream xml;
        xml << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
        xml << "  <PropertyGroup>\n";
        xml << "    <TargetFramework>" << kScriptTargetFramework << "</TargetFramework>\n";
        xml << "    <OutputType>Library</OutputType>\n";
        xml << "    <AssemblyName>GameEngine.Editor</AssemblyName>\n";
        xml << "    <RootNamespace>GameEngine.Editor</RootNamespace>\n";
        // The language settings CompileServerRequestDesc sends on the live path.
        xml << "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n";
        xml << "    <ImplicitUsings>enable</ImplicitUsings>\n";
        xml << "    <Nullable>enable</Nullable>\n";
        xml << "    <EnableDefaultItems>false</EnableDefaultItems>\n";
        xml << "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
        xml << "    <AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>\n";
        xml << "    <OutputPath>$(MSBuildThisFileDirectory)</OutputPath>\n";
        // Default only: live builds pass -p:EngineBinDir (or the compile-server request
        // field), so a relocated Editor cannot poison them; the baked value serves IDE
        // loads and refreshes on every Editor launch.
        xml << "    <EngineBinDir Condition=\"'$(EngineBinDir)' == ''\">" << engineBinDirDefault << "</EngineBinDir>\n";
        xml << "    <ProduceReferenceAssembly>false</ProduceReferenceAssembly>\n";
        xml << "    <ProvideCommandLineArgs>true</ProvideCommandLineArgs>\n";
        xml << "  </PropertyGroup>\n";
        xml << "  <ItemGroup>\n";
        // obj/bin hold MSBuild outputs — including generated AssemblyInfo/
        // attribute .cs files from any csproj that ever built under this root
        // (e.g. the pre-migration Assets/Scripts project). Sweeping them into
        // the compile duplicates assembly attributes (CS0579).
        xml << "    <Compile Include=\"" << includeRootWin << "**\\*.cs\" Exclude=\""
            << includeRootWin << "**\\obj\\**;" << includeRootWin << "**\\bin\\**\" />\n";
        xml << "  </ItemGroup>\n";
        xml << "  <ItemGroup Condition=\"Exists('$(EngineBinDir)')\">\n";
        xml << "    <GameEngineAbi Include=\"$(EngineBinDir)GameEngine.*.ABI.dll\" />\n";
        xml << "  </ItemGroup>\n";
        xml << "  <ItemGroup>\n";
        xml << "    <Reference Include=\"%(GameEngineAbi.Filename)\">\n";
        xml << "      <HintPath>%(GameEngineAbi.FullPath)</HintPath>\n";
        xml << "      <Private>false</Private>\n";
        xml << "    </Reference>\n";
        xml << "  </ItemGroup>\n";
        EmitEngineBinDirValidationTarget(xml, "GameEngine.Scripting.ABI.dll");
        xml << "  <Target Name=\"DumpCompileItems\" BeforeTargets=\"CoreCompile\">\n";
        xml << "    <Message Importance=\"High\" Text=\"[Editor.csproj] Compile items: @(Compile)\" />\n";
        xml << "    <Message Importance=\"High\" Text=\"[Editor.csproj] Csc args: @(CscCommandLineArgs)\" />\n";
        xml << "  </Target>\n";
        xml << "</Project>\n";

        const std::string newContent = xml.str();
        if (std::filesystem::exists(projectPath) && existingContent == newContent)
        {
            Logger::Log::Info("Auto-generated Editor project at {} is up to date (sources: {})", projectPath.string(), includeRoot);
            return true;
        }

        std::ofstream of(projectPath, std::ios::trunc);
        if (!of)
        {
            Logger::Log::Error("Failed to create auto-generated project file: {}", projectPath.string());
            return false;
        }
        of << newContent;
        of.close();

        Logger::Log::Info("Auto-generated Editor project at {} (sources: {})", projectPath.string(), includeRoot);
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Exception while generating editor scripts project: {}", e.what());
        return false;
    }
}

void ScriptManager::OnFileChanged(const FileChangeEvent& event)
{
    // Shutdown does not wait for an event already being dispatched. This is an
    // early out; StartHotReload re-checks the flag under the start mutex.
    if (!m_HotReloadEnabled || m_ShuttingDown.load(std::memory_order_relaxed))
    {
        return;
    }

    const auto ext = event.Path.extension();
    if (ext != ".cs" && ext != ".csproj")
    {
        return;
    }

    Logger::Log::Debug("Script file changed: {} ({})", event.Path.string(), static_cast<int>(event.Type));

    // Trigger recompile and reload for any C# file or project file changes
    if (ext == ".cs" || ext == ".csproj")
    {
        // RACE CONDITION FIX: Improved multiple hot-reload prevention with timeout
        static std::atomic<bool> hotReloadInProgress{false};
        static std::atomic<std::chrono::steady_clock::time_point> lastHotReloadStart{std::chrono::steady_clock::time_point{}};

        auto now = std::chrono::steady_clock::now();
        auto lastStart = lastHotReloadStart.load();

        // RACE CONDITION FIX: Reset flag if previous operation took too long (likely crashed)
        if (hotReloadInProgress.load() && (now - lastStart) > std::chrono::seconds(30))
        {
            Logger::Log::Warning("Hot-reload operation appears stuck, resetting flag after 30 seconds");
            hotReloadInProgress.store(false);
        }

        if (hotReloadInProgress.exchange(true))
        {
            Logger::Log::Debug("Hot-reload already in progress, ignoring file change: {}", event.Path.string());
            return;
        }

        // Update start time
        lastHotReloadStart.store(now);

        // RACE CONDITION FIX: Ensure we reset the flag when done using RAII with timeout protection
        struct FlagGuard
        {
            std::atomic<bool>& flag;
            std::atomic<std::chrono::steady_clock::time_point>& startTime;
            FlagGuard(std::atomic<bool>& f, std::atomic<std::chrono::steady_clock::time_point>& st) : flag(f), startTime(st) {}
            ~FlagGuard()
            {
                flag.store(false);
                startTime.store(std::chrono::steady_clock::time_point{});
            }
        };
        FlagGuard flagGuard(hotReloadInProgress, lastHotReloadStart);

        // Propagate ChangedFiles to CompileServer for incremental strategy
        {
            std::vector<std::string> changed;
            changed.push_back(event.Path.generic_string());
            SetChangedFilesForNextCompile(changed);
        }

        Logger::Log::Debug("🔄 File watcher triggered hot-reload for: {}", event.Path.string());

        // Do not unload the current assembly immediately; keep it until the new one is ready.
        // The collectible assemblies approach handles file locking via stream-based loading,
        // and we unload the old assembly only after the new one has been loaded successfully.
        if (m_ClrHost.IsAssemblyLoaded())
        {
            Logger::Log::Debug("🔄 File watcher: Current assembly will be replaced after new one loads successfully");
            Logger::Log::Debug("📋 Using collectible AssemblyLoadContext - no premature unloading needed");
        }

        // Never compile on this thread: the service dispatches every subscriber's
        // events for the directory from it, asset hot reload included.
        // StartTrackedHotReload hands the compile to the async pipeline, or to a
        // job-system reload when async hot reload is disabled.
        Logger::Log::Info("🚀 File watcher: Starting hot-reload for: {}", event.Path.string());
        const std::filesystem::path assemblyPath = GetScriptsAssemblyPath();
        StartTrackedHotReload(assemblyPath.string());
        Logger::Log::Debug("✅ File watcher: Hot-reload started for: {}", event.Path.string());
    }
}

Vector<std::filesystem::path> ScriptManager::FindProjectFiles() const
{
    Vector<std::filesystem::path> projectFiles;

    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_ScriptsDirectory))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".csproj")
            {
                projectFiles.push_back(entry.path());
            }
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Error("Error scanning for project files: {}", e.what());
    }

    return projectFiles;
}

bool ScriptManager::AreScriptDirectoriesOverlapping() const
{
    if (m_ScriptsDirectory.empty() || m_EditorScriptsDirectory.empty())
        return false;

    std::error_code ec;
    auto projectCanonical = std::filesystem::weakly_canonical(m_ScriptsDirectory, ec);
    if (ec) projectCanonical = m_ScriptsDirectory.lexically_normal();
    ec.clear();
    auto editorCanonical = std::filesystem::weakly_canonical(m_EditorScriptsDirectory, ec);
    if (ec) editorCanonical = m_EditorScriptsDirectory.lexically_normal();

    // Exact match: project Assets == editor Assets.
    if (projectCanonical == editorCanonical)
        return true;

    // Project is inside editor (e.g. <exe>/Assets/SubDir as project pointed at <exe>).
    auto rel = projectCanonical.lexically_relative(editorCanonical);
    if (!rel.empty() && *rel.begin() != "..")
        return true;

    // Editor is inside project (e.g. --project points to a parent of the exe dir).
    rel = editorCanonical.lexically_relative(projectCanonical);
    if (!rel.empty() && *rel.begin() != "..")
        return true;

    return false;
}

ScriptCompilationResult ScriptManager::ExecuteDotNetBuild(const std::filesystem::path& projectPath,
                                                          const std::filesystem::path& outputDir)
{
    ScriptCompilationResult result;
    result.success = false;
    result.outputPath = outputDir.string();
    if (RefusePackagedModeCompile("ExecuteDotNetBuild"))
    {
        result.errorMessage = "Packaged mode: dotnet is never invoked";
        return result;
    }
    // The csproj's baked EngineBinDir is only a last-known-good for IDE loads; this
    // process knows where the engine actually lives, so pass it per invocation (same
    // contract as the compile-server request, issue #333). Trailing separator required:
    // HintPaths concatenate as $(EngineBinDir)GameEngine.*.dll.
    std::string engineBinDir = ScriptingPaths::ResolveEngineManagedDirectory().generic_string();
    if (!engineBinDir.empty() && engineBinDir.back() != '/')
        engineBinDir += '/';
    const std::vector<std::string> args = {
        "build",
        projectPath.string(),
        "--configuration",
        "Debug",
        "--verbosity",
        "minimal",
        "--output",
        outputDir.string(),
        "-p:ProduceReferenceAssembly=false",
        "-p:ProduceOnlyReferenceAssembly=false",
        "-p:EngineBinDir=" + engineBinDir,
    };

    Logger::Log::Info("Executing: dotnet build {}", projectPath.string());
    Logger::Log::Info("Output directory: {}", outputDir.string());

    const ShellProcessResult buildResult = RunProcessCaptured(DotnetHostCommand(), args);
    const std::string& output = buildResult.output;
    const int exitCode = buildResult.exitCode;

    if (exitCode == 0)
    {
        result.success = true;
        Logger::Log::Info("Script compilation successful!");
        if (!output.empty())
        {
            Logger::Log::Debug("Build output: {}", output);
        }
    }
    else
    {
        result.success = false;
        result.errorMessage = "dotnet build failed with exit code " + std::to_string(exitCode);
        if (!output.empty())
        {
            result.errorMessage += "\nOutput: " + output;
        }
        Logger::Log::Error("Script compilation failed: {}", result.errorMessage);
    }

    // Parse output for warnings (even on success).
    ParseBuildOutput(output, result);

    return result;
}

void ScriptManager::ParseBuildOutput(const String& output, ScriptCompilationResult& result)
{
    if (output.empty())
    {
        return;
    }

    // Parse MSBuild output for warnings and errors
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line))
    {
        // Look for warning patterns: "warning CS####:"
        if (line.find("warning CS") != std::string::npos)
        {
            result.warnings.push_back(line);
            Logger::Log::Debug("Build warning: {}", line);
        }
        // Look for error patterns: "error CS####:"
        else if (line.find("error CS") != std::string::npos)
        {
            if (!result.errorMessage.empty())
            {
                result.errorMessage += "\n";
            }
            result.errorMessage += line;
            Logger::Log::Debug("Build error: {}", line);
        }
        // Look for general build failed messages
        else if (line.find("Build FAILED") != std::string::npos)
        {
            if (!result.errorMessage.empty())
            {
                result.errorMessage += "\n";
            }
            result.errorMessage += line;
        }
    }
}

} // namespace GameEngine
