#include "EngineShaderPath.h"
#include "Core/Engine.h"

#include "EngineShaderHookState.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

// Include JobSystem implementation details locally (avoid exposing in Engine.h)
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Capabilities.h"


// Asset type includes
#include "Assets/AssetRegistry.h"
#include "Assets/AudioAsset.h"
#include "Assets/CoreAssetRegistrations.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/ScriptAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include "Assets/ShaderSourceAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/RenderPipelineAsset.h"

#include "NativeScripting/NativeScriptManager.h"
#include "UI/ModuleOwnedHandlers.h"

#include "Scripting/PathResolver.h"

// Rendering utilities (shader loader indirection)
#include "Rendering/Common/Utils.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"

// Rendering hot-reload bridge (Engine side)
#include "Engine/RenderingHotReloadBridge.h"

// Scene module (TLAS lifecycle release on World destruction)
#include "Scene/SceneTlas.h"
// Optional engine-owned rendering loop
#include "ECSModules/Rendering/RenderingLoop.h"

// Audio
#include "Audio/AudioSystem.h"
#include "ECSModules/Audio/Systems/RegisterAudioSystems.h"

// PhysicsECS (module)
#include "PhysicsECS/PhysicsWorldHolder.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "TerrainECS/TerrainModifierComponents.h" // lifecycle-event subscription (terrain modifier change gating)
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"
#include "TerrainGrass/RegisterTerrainGrass.h"
#include "Ocean/Systems/RegisterOceanSystems.h"
#include "MarkupECS/MarkupService.h"
#include "SplineECS/SplineService.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
// ECS integration
#include "Components/Components.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Hierarchy.h" // Components::Parent — lifecycle-event subscription (hierarchy incremental sync)
#include "ECS/ComponentFieldRegistry.h" // C12 load-abort purge
#include "ECS/ComponentRegistry.h"
#include "Particles/ParticleProcessorRegistry.h" // C12 load-abort purge, unload ledger
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/SystemScheduling.h"
#include "ECS/World.h"
#include "Scene/SceneSchemaRegistry.h" // built-in schema registration, C12 load-abort purge
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Systems/LensFlareExtractionSystem.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Engine/Video/VideoTextureSystem.h"
#include "PluginAPI/EnginePlugin.h"

#include <cstdlib>

namespace GameEngine
{

namespace
{
// Empty never matches: a drive root and a trailing separator both yield an empty
// filename, and treating those as equal would report every such path as nested
// inside itself.
bool EqualsIgnoringCase(const std::string& a, const std::string& b)
{
    return !a.empty() && a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char l, unsigned char r) {
               return std::tolower(l) == std::tolower(r);
           });
}

// An asset root that does not exist is either a brand-new project (fine — the
// registry creates it) or a mistyped root, and the one mistake worth naming is
// pointing a root AT an assets directory that already contains one. Both hosts
// can reach it from opposite directions: the Editor is given a PROJECT root and
// the engine appends the asset directory, so `--project <root>/Assets` mounts
// `<root>/Assets/Assets`; the Player resolves `--asset-root` against the launch
// cwd, so running it from inside `<root>/Assets` with `--asset-root Assets`
// lands on the same place. Either way the directory is created empty and every
// asset reference in every scene then fails with a per-entity parse error that
// names the entity rather than the mount.
//
// `appendedToWorkspaceRoot` is what separates the two, and only the suggested
// correction depends on it: the Editor's fix is to open the parent project, the
// Player's is to point the asset root one level up.
void DiagnoseAssetRootBeforeCreation(const std::filesystem::path& workspaceRoot,
                                     const std::filesystem::path& assetDirName,
                                     const std::filesystem::path& resolvedAssetRoot,
                                     bool appendedToWorkspaceRoot)
{
    std::error_code ec;
    if (std::filesystem::is_directory(resolvedAssetRoot, ec))
        return;

    // The signature is the same nesting in both cases — the directory's own name
    // repeated by its parent — but only the appended form can read it off the
    // workspace root, since an absolute asset root has no relation to it.
    const bool misNested =
        appendedToWorkspaceRoot
            ? EqualsIgnoringCase(workspaceRoot.filename().string(), assetDirName.string())
            : EqualsIgnoringCase(resolvedAssetRoot.parent_path().filename().string(),
                                 resolvedAssetRoot.filename().string());

    if (misNested && appendedToWorkspaceRoot)
    {
        Logger::Log::Error("Project root '{}' is itself an asset directory: the engine appends '{}' to the "
                           "project root, so assets are being mounted at '{}', which will be created empty. "
                           "Every asset reference will fail to resolve. Did you mean to open '{}'?",
                           workspaceRoot.string(), assetDirName.string(), resolvedAssetRoot.string(),
                           workspaceRoot.parent_path().string());
        return;
    }

    if (misNested)
    {
        Logger::Log::Error("Asset root '{}' is nested inside a directory of the same name and will be created "
                           "empty. Every asset reference will fail to resolve. Did you mean '{}'?",
                           resolvedAssetRoot.string(), resolvedAssetRoot.parent_path().string());
        return;
    }

    Logger::Log::Warning("No asset directory at '{}'; creating an empty one. "
                         "Asset references will not resolve until content is placed there.",
                         resolvedAssetRoot.string());
}

// Absolute, lexically normal workspace root. Empty means the executable directory; a
// relative directory is interpreted against the executable directory.
std::filesystem::path ResolveWorkspaceRoot(const std::filesystem::path& directory)
{
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    std::filesystem::path root = directory.empty() ? exeDir : directory;
    if (!root.is_absolute())
        root = exeDir / root;
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(root, ec);
    if (!ec)
        root = canonical;
    return root.lexically_normal();
}

std::filesystem::path ConfiguredOrDefault(const String& configured, const char* fallback)
{
    return configured.empty() ? std::filesystem::path(fallback) : std::filesystem::path(configured);
}

std::filesystem::path ResolveUnderWorkspace(const std::filesystem::path& workspaceRoot,
                                            const std::filesystem::path& path)
{
    return (path.is_absolute() ? path : workspaceRoot / path).lexically_normal();
}

struct ProjectPaths
{
    std::filesystem::path AssetRoot;
    std::filesystem::path AssetDatabaseFile;
    std::filesystem::path AssetDatabaseCacheRoot;
};

// The configured asset directory, asset database file and its derived cache, each relative
// to the workspace root unless configured absolute; an empty setting takes the default layout.
ProjectPaths ResolveProjectPaths(const std::filesystem::path& workspaceRoot, const ApplicationConfig& config)
{
    ProjectPaths paths;
    paths.AssetRoot =
        ResolveUnderWorkspace(workspaceRoot, ConfiguredOrDefault(config.AssetDirectory, kDefaultAssetDirectory));
    paths.AssetDatabaseFile = ResolveUnderWorkspace(
        workspaceRoot, ConfiguredOrDefault(config.AssetDatabaseFile, kDefaultAssetDatabaseFile));
    paths.AssetDatabaseCacheRoot = ResolveUnderWorkspace(
        workspaceRoot, ConfiguredOrDefault(config.AssetDatabaseCacheDirectory, kDefaultAssetDatabaseCacheDirectory));
    return paths;
}

static std::filesystem::path ResolveEngineShaderPath(const std::filesystem::path& relativePath)
{
    return Detail::ResolveEngineShaderPath(EngineCore::GetInstance().GetAssetManager(), relativePath);
}

static std::string ShaderPackageRebuildAction(const std::string& packageName)
{
    return Detail::ShaderPackageRebuildAction(packageName, PathUtils::GetExecutableDirectory());
}

static std::vector<uint8_t> EngineShaderBytecodeLoaderFromAssets(const char* name)
{
    if (!name || !name[0])
        return {};
    auto resolved = ResolveEngineShaderPath(name);
    if (resolved.empty())
        return {};
    return Rendering::Utils::ReadFile(resolved.string());
}
// Frames named when Initialize is refused; the chain through CoreCLR into a GE_*
// entry point and back is deeper than the logger's default backtrace.
constexpr int kRefusedInitializeFrames = 32;

std::string DescribeCallers()
{
    std::string callers;
    for (const Logger::BacktraceFrame& frame : Logger::CaptureBacktrace(kRefusedInitializeFrames))
    {
        if (!callers.empty())
            callers += " <- ";
        if (!frame.Symbol.empty())
            callers += frame.Symbol;
        else
            callers += frame.Module.empty() ? "?" : frame.Module;
    }
    return callers;
}

void ReportRefusedInitialize(std::thread::id initializingThread)
{
    const bool sameThread = initializingThread == std::this_thread::get_id();
    Logger::Log::Error("EngineCore::Initialize refused: Initialize is already running {}; a second run would build "
                       "every subsystem twice. Callers: {}",
                       sameThread ? "on this thread (re-entrant call)" : "on another thread (concurrent call)",
                       DescribeCallers());
}


// The world services the engine owns. EnableRenderingLoop initializes them in
// this order; Shutdown releases them in reverse, after the worlds and systems
// that use them are gone.
struct WorldServiceLifecycle
{
    bool (*IsInitialized)();
    void (*Initialize)();
    void (*Shutdown)();
};

const WorldServiceLifecycle kWorldServices[] = {
    {&TerrainECS::TerrainService::IsInitialized, &TerrainECS::TerrainService::Initialize,
     &TerrainECS::TerrainService::Shutdown},
    {&SplineECS::SplineService::IsInitialized, &SplineECS::SplineService::Initialize,
     &SplineECS::SplineService::Shutdown},
    {&MarkupECS::MarkupService::IsInitialized, &MarkupECS::MarkupService::Initialize,
     &MarkupECS::MarkupService::Shutdown},
    {&PathfindingECS::NavigationService::IsInitialized, &PathfindingECS::NavigationService::Initialize,
     &PathfindingECS::NavigationService::Shutdown},
};

void InitializeWorldServices()
{
    for (const WorldServiceLifecycle& service : kWorldServices)
    {
        if (!service.IsInitialized())
            service.Initialize();
    }
}

void ShutdownWorldServices()
{
    for (auto it = std::rbegin(kWorldServices); it != std::rend(kWorldServices); ++it)
    {
        if (it->IsInitialized())
            it->Shutdown();
    }
}

// Opens the audio device. False (audio disabled) when the init fails or throws.
bool InitializeAudio(Audio::AudioSystem& audio)
{
    try
    {
        Audio::AudioSystemConfig audioCfg{};
        if (!audio.Initialize(audioCfg))
        {
            Logger::Log::Warning("AudioSystem init failed; audio disabled");
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("AudioSystem init threw: {}", e.what());
        return false;
    }
}

} // namespace

class EngineCore::InitializeTransaction
{
  public:
    explicit InitializeTransaction(EngineCore& engine) : m_Engine(engine) {}

    // Rollback: every early exit from Initialize, including an exception, lands
    // here. Nothing may stay outstanding, or the next Initialize would be refused
    // for the life of the process.
    ~InitializeTransaction()
    {
        if (m_Committed)
            return;
        m_Engine.DiscardAudioInitAfterFailedInitialize();
        m_Engine.m_StartupChannel.reset();
        m_Engine.m_ShaderHookState.reset();
        m_Engine.m_InitializingThread.store(std::thread::id{}, std::memory_order_release);
        m_Engine.m_Lifecycle.store(EngineLifecycleState::Uninitialized, std::memory_order_release);
    }

    void Commit()
    {
        m_Committed = true;
        m_Engine.m_InitializingThread.store(std::thread::id{}, std::memory_order_release);
        m_Engine.m_Lifecycle.store(EngineLifecycleState::Initialized, std::memory_order_release);
    }

  private:
    EngineCore& m_Engine;
    bool m_Committed = false;
};

EngineCore* EngineCore::s_Instance = nullptr;

EngineCore::EngineCore()
{
    if (s_Instance != nullptr)
    {
        Logger::Log::Critical("Engine instance already exists!");
    }
    s_Instance = this;
}

EngineCore::~EngineCore()
{
    if (IsInitialized())
    {
        Shutdown();
    }
    s_Instance = nullptr;
}

void EngineCore::SetScriptsConfig(const ScriptsConfig& config)
{
    m_ScriptsConfig = config;
}

bool EngineCore::Initialize(const ApplicationConfig& config)
{
    // Layout agreement before anything reads a shared object. The host filled
    // HostDebugInstrumentation from its own compile of the engine headers; this
    // translation unit is inside the engine. When the two differ, every engine
    // class the host touches has members at offsets the engine does not use,
    // and the first symptom is plausible-looking garbage rather than a fault —
    // refuse here, where the cause is still nameable.
    if (config.HostDebugInstrumentation != GE_DEBUG_INSTRUMENTATION)
    {
        Logger::Log::Error(
            "EngineCore::Initialize refused: this program was compiled with "
            "GE_DEBUG_INSTRUMENTATION={} and the engine it loaded was built with {}. Engine "
            "classes declare debug-only members under that switch, so the two disagree on "
            "where every member after the first one lives. Rebuild this program against the "
            "engine it runs with: SDK consumers get the value from "
            "find_package(GameEngine), in-tree targets from the repository's CMake.",
            config.HostDebugInstrumentation, static_cast<uint32>(GE_DEBUG_INSTRUMENTATION));
        return false;
    }

    m_EverInitialized.store(true, std::memory_order_release);
    EngineLifecycleState expected = EngineLifecycleState::Uninitialized;
    if (!m_Lifecycle.compare_exchange_strong(expected, EngineLifecycleState::Initializing, std::memory_order_acq_rel))
    {
        if (expected == EngineLifecycleState::Initialized)
        {
            Logger::Log::Warning("Engine already initialized");
            return true;
        }
        ReportRefusedInitialize(m_InitializingThread.load(std::memory_order_acquire));
        return false;
    }
    m_InitializingThread.store(std::this_thread::get_id(), std::memory_order_release);
    InitializeTransaction transaction(*this);

    // Unreachable by construction: Shutdown() and the Initialize rollback both join
    // the audio init. Kept as the assert this invariant deserves, since breaking it
    // means destroying an AudioSystem under its own init thread.
    if (m_AudioInit.IsValid() || m_AudioInitDeferred)
    {
        Logger::Log::Error("EngineCore::Initialize refused: an AudioSystem init is outstanding while the "
                           "engine is Uninitialized; neither Shutdown() nor the Initialize rollback joined it");
        return false;
    }

    const auto tEngineStart = std::chrono::high_resolution_clock::now();
    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };

    Logger::Log::Info("Initializing Game Engine");
    Logger::Log::Info("Platform: {}", PLATFORM_NAME);


    m_Config = config;
    m_WorkspaceRootIsFallback = config.WorkspaceDirectoryIsFallback;

    // Resolve workspace root early so assets and scripts agree on where "workspace-relative" paths land.
    const std::filesystem::path resolvedWorkspaceRoot = ResolveWorkspaceRoot(config.WorkspaceDirectory);
    {
        // The shader-graph node library ships with the engine assets; the
        // Rendering module cannot locate the executable itself on every host.
        const std::filesystem::path installAssets = PathUtils::GetInstallAssetsRoot();
        if (!installAssets.empty())
            ShaderGraph::SetEngineGraphNodesRoot(installAssets / "Shaders" / "Graph" / "Nodes");
    }

    // Publish resolved workspace root for consumers (Editor, tools).
    m_WorkspaceRoot = resolvedWorkspaceRoot;

    // Ensure the workspace directory exists before setting the process working directory to it.
    // This avoids failures on first run (e.g. default project / user-writable workspace paths).
    {
        std::error_code ec;
        std::filesystem::create_directories(resolvedWorkspaceRoot, ec);
        if (ec)
        {
            Logger::Log::Warning("Failed to create workspace directory '{}': {}",
                                 resolvedWorkspaceRoot.string(),
                                 ec.message());
        }
    }

    // Ensure process working directory matches the resolved workspace root.
    // This keeps relative path usage project-centric and avoids accidental writes into app bundles.
    GameEngine::PathUtils::EnsureWorkingDirectoryMatches(resolvedWorkspaceRoot);

    // Resolve the configured asset directory and asset database locations against the
    // workspace root, and publish them for consumers (Editor, tools).
    const ProjectPaths projectPaths = ResolveProjectPaths(resolvedWorkspaceRoot, config);
    const std::filesystem::path& resolvedAssetRoot = projectPaths.AssetRoot;
    {
        const std::filesystem::path assetDirName = ConfiguredOrDefault(config.AssetDirectory, kDefaultAssetDirectory);
        // Unconditional: the Player hands this layer an already-absolute asset
        // root, so gating the diagnostic on the appended form would leave the
        // host that has no other asset-root diagnostic at all undiagnosed.
        DiagnoseAssetRootBeforeCreation(resolvedWorkspaceRoot, assetDirName, resolvedAssetRoot,
                                        !assetDirName.is_absolute());
    }
    m_ResolvedAssetRoot = projectPaths.AssetRoot;
    m_AuthoritativeAssetDbFile = projectPaths.AssetDatabaseFile;
    m_AssetDbCacheRoot = projectPaths.AssetDatabaseCacheRoot;

    // Initialize job system for async operations
    size_t numThreads = Platform::RecommendedWorkerCount();
    bool numThreadsFromEnv = false;
    if (const char* env = std::getenv("GE_JOB_THREADS"))
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(env, &end, 10);
        if (end != env && v > 0)
        {
            numThreads = static_cast<size_t>(v);
            numThreadsFromEnv = true;
        }
    }

    if (!numThreadsFromEnv)
    {
        // Creating a very large worker pool can dominate startup time on Windows (thread creation + scheduler warmup),
        // especially in headless tools/tests. Clamp by default (Editor can override via config/env later if needed).
        if (!config.EnableEditor)
        {
            numThreads = std::min<size_t>(numThreads, 8);
        }
    }
    auto tPhase = std::chrono::high_resolution_clock::now();
    m_JobSystem = MakeUnique<JobSystem::WorkStealingThreadPool>(numThreads, Platform::BlockingThreadBudget());
    Logger::Log::Info("[Startup] JobSystem: {:.1f}ms ({} threads)", MsSince(tPhase), numThreads);

    // Backfill the primary world's pool if the world was created FIRST.
    // EnsurePrimaryWorld is called from application code (editor panels,
    // play mode, debug handlers) that can run before Initialize reaches this
    // line — the world then constructs with a null pool and, without this,
    // stays deliberately-serial forever. This is not hypothetical: the
    // editor hit exactly that order, and the old engine-pool fallback
    // ladders masked it (found by the wiring arc's A/B bench — TLAS refit
    // silently serial, 3.4 -> 8.1 ms @ 10k movers).
    if (m_PrimaryWorld && !m_PrimaryWorld->GetJobSystem())
    {
        m_PrimaryWorld->SetJobSystem(m_JobSystem.get());
        Logger::Log::Info("[Startup] Primary world pool: backfilled (world predates JobSystem)");
    }

    // Create asset manager instance. We register core asset types *before*
    // initializing/scanning so startup indexing can assign correct types.
    tPhase = std::chrono::high_resolution_clock::now();
    m_AssetManager = MakeUnique<AssetManager>();

    // Register core engine asset types
    try
    {
        auto& typeRegistry = m_AssetManager->GetAssetTypeRegistry();
        RegisterCoreAssetTypes(typeRegistry);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Exception registering core asset types: {}", e.what());
    }

    // Initialize asset manager with job system
    if (!m_AssetManager->Initialize(resolvedAssetRoot, m_JobSystem.get(), projectPaths.AssetDatabaseFile,
                                    projectPaths.AssetDatabaseCacheRoot))
    {
        Logger::Log::Error("Failed to initialize asset manager");
        return false;
    }
    Logger::Log::Info("[Startup] AssetManager: {:.1f}ms", MsSince(tPhase));

    // Native C++ user-script manager (watches sources; build/load pipeline lands in C10+).
    // After AssetManager + FileWatchingService so its watch subscriptions are valid.
    tPhase = std::chrono::high_resolution_clock::now();
    m_NativeScriptManager = MakeUnique<NativeScripting::NativeScriptManager>();
    m_NativeScriptManager->SetModuleLoadingSupported(Platform::SupportsDynamicNativeModules());
    if (!m_NativeScriptManager->Initialize(m_JobSystem.get()))
    {
        Logger::Log::Error("Failed to initialize native script manager");
        return false;
    }
    // C12 load-abort purge: a module DLL refused by the load handshake already
    // ran its registrars; drop everything stamped with that load's generation
    // from the registries the manager itself cannot see, before the image is
    // unmapped. (Engine plugins + user systems are purged by the manager
    // directly; the schedule purge is belt-and-braces — the registration hold
    // keeps an aborted load from ever replaying systems into the model.)
    m_NativeScriptManager->SetModuleRegistrationPurgeHandler(
        [this](std::string_view moduleId, uint64 generation)
        {
            ECS::ComponentRegistry::PurgeModuleComponents(moduleId, generation);
            ECS::ComponentFieldRegistry::PurgeModuleFieldTables(moduleId, generation);
            Scene::SceneSchemaRegistry::PurgeModuleSchemas(moduleId, generation);
            Particles::ParticleProcessorRegistry::PurgeModuleProcessors(moduleId, generation);
            if (m_RenderServices)
                m_RenderServices->Spine().PurgeModulePipelineNodes(moduleId, generation);
            if (m_RenderingLoop)
            {
                auto& schedule = m_RenderingLoop->GetScheduleBuilder();
                if (auto* systemManager = m_RenderingLoop->GetSystemManager())
                    schedule.PurgeModuleRegistrations(*systemManager, moduleId, generation);
            }
        });

    // C12 unload quiesce ledger: after a successful reload, the superseded
    // image may be unmapped only when every registry re-owned (or dropped) its
    // entries — a non-empty answer names what still points into old code.
    m_NativeScriptManager->SetModuleQuiesceCheck(
        [this](std::string_view moduleId, uint64 currentGeneration) -> std::string
        {
            std::string blockers;
            const auto append = [&blockers](std::size_t n, const char* what)
            {
                if (n == 0)
                    return;
                if (!blockers.empty())
                    blockers += ", ";
                blockers += std::to_string(n);
                blockers += ' ';
                blockers += what;
            };
            append(Plugins::EnginePluginRegistry::Get().CountSupersededModulePlugins(moduleId,
                                                                                     currentGeneration),
                   "engine plugin(s)");
            append(ECS::ComponentRegistry::CountSupersededModuleComponents(moduleId, currentGeneration),
                   "component handler(s)");
            append(ECS::ComponentFieldRegistry::CountSupersededModuleFieldTables(moduleId,
                                                                                 currentGeneration),
                   "reflected field table(s)");
            append(Scene::SceneSchemaRegistry::CountSupersededModuleSchemas(moduleId, currentGeneration),
                   "scene schema(s)");
            append(Particles::ParticleProcessorRegistry::CountSupersededModuleProcessors(moduleId, currentGeneration),
                   "particle processor(s)");
            if (m_RenderServices)
                append(m_RenderServices->Spine().CountSupersededModulePipelineNodes(moduleId,
                                                                                    currentGeneration),
                       "render pipeline node factory(ies)");
            if (m_RenderingLoop)
            {
                append(m_RenderingLoop->GetScheduleBuilder().CountSupersededModuleRegistrations(
                           moduleId, currentGeneration),
                       "scheduled system(s)");
            }
            else if (m_RenderingLoopEnabled)
            {
                append(1, "unreachable schedule model");
            }
            return blockers;
        });

    // C12 module-image observer: what points into a superseded image by ADDRESS
    // rather than by registration id. This includes UI event handlers whose
    // callable a user module built — code and constant data inside the image,
    // stored in an engine-owned element that outlives it, and copied by every
    // dispatch before any liveness flag is read. Engine.dll and Editor.exe are
    // never in the set these ranges describe, so an engine-registered handler
    // cannot be revoked here; the discrimination is structural.
    m_NativeScriptManager->SetModuleImageObserver([] {
        NativeScripting::NativeScriptManager::ModuleImageObserver observer;
        observer.ImageMapBegin = [] { UI::OpenImageAttribution(); };
        observer.ImageMapEnd = [](uint64 base, uint64 size) {
            UI::CloseImageAttribution(base, size);
        };
        observer.DescribeImagePins = [](uint64 base, uint64 size) -> std::string {
            const std::size_t n = UI::CountHandlersOwnedByImage(base, size);
            std::string pins = n == 0 ? std::string{} : std::to_string(n) + " UI event handler(s)";
            // Component hooks may own live resources. Keep their code mapped
            // until the owner explicitly unregisters; silently revoking them
            // would strand those resources on later component destruction.
            const auto hooks = ECS::World::CountComponentHooksOwnedByImage(base, size);
            if (hooks != 0)
            {
                if (!pins.empty())
                    pins += ", ";
                pins += std::to_string(hooks) + " ECS component hook(s)";
            }
            return pins;
        };
        observer.ImageUnmapping = [](uint64 base, uint64 size) {
            UI::RevokeHandlersOwnedByImage(base, size);
        };
        observer.ImageUnmapped = [](uint64 base) { UI::RetractHotSwappableImage(base); };
        return observer;
    }());

    // C12 reload reconcile: after a successful reload's replay re-owned the
    // re-declared registrations, retire the pipeline-node factories the replay
    // stopped declaring and rebuild live pipeline instances from the newest
    // factories — before the unload decision reads the ledger above.
    m_NativeScriptManager->SetModuleReloadReconcileHandler(
        [this](std::string_view moduleId, uint64 generation)
        {
            if (m_RenderServices)
                m_RenderServices->Spine().ReconcileModuleNodeRegistrations(moduleId, generation);
        });
    Logger::Log::Info("[Startup] NativeScriptManager: {:.1f}ms", MsSince(tPhase));

    m_ShaderHookState = MakeUnique<ShaderHookState>();
    Rendering::Utils::SetShaderFileLoader(&EngineShaderBytecodeLoaderFromAssets);
    Rendering::Utils::SetShaderPathResolver(&ResolveEngineShaderPath);
    Rendering::SetShaderPackageRebuildAction(&ShaderPackageRebuildAction);
    {
        std::filesystem::path shaderCacheRoot = resolvedWorkspaceRoot / ".Cache" / "Shaders";
        // Runtimes without a compiler read the current bundled packages for
        // every project. A persisted project cache can contain older variants
        // or metadata and must not shadow that authoritative bundle.
        if (const auto bundleCache = PathUtils::GetBundledShaderCacheRoot(); !bundleCache.empty())
            shaderCacheRoot = bundleCache;
        ShaderProgramAsset::SetShaderCacheRoot(shaderCacheRoot);
    }

    // Initialize script manager (only when scripting is enabled at build time).
#if GE_ENABLE_SCRIPTING
    // Build scripting configuration (workspace + script roots) and initialize ScriptManager.
    ScriptsConfig scriptsConfig;
    std::filesystem::path scriptsNativeDllPath;
    {
        using GameEngine::PathUtils;

        const std::filesystem::path& workspaceRoot = resolvedWorkspaceRoot;

        if (m_ScriptsConfig.has_value())
        {
            scriptsConfig = *m_ScriptsConfig;

            // Fill in any unset paths with sensible defaults derived from the resolved workspace root.
            if (scriptsConfig.workspaceRoot.empty())
            {
                scriptsConfig.workspaceRoot = workspaceRoot;
            }
            if (scriptsConfig.scriptsRoot.empty())
            {
                // Keep scripts root aligned with the engine asset root by default (typically <ProjectRoot>/Assets).
                scriptsConfig.scriptsRoot = resolvedAssetRoot;
            }
            if (scriptsConfig.assembliesRoot.empty())
            {
                scriptsConfig.assembliesRoot = scriptsConfig.workspaceRoot / "ScriptAssemblies";
            }
        }
        else
        {
            scriptsConfig.workspaceRoot = workspaceRoot;
            scriptsConfig.scriptsRoot = resolvedAssetRoot;
            scriptsConfig.assembliesRoot = workspaceRoot / "ScriptAssemblies";
            scriptsConfig.disableClr = false;
            scriptsConfig.enableHotReload = true;
            scriptsConfig.enableAsyncHotReload = false;
            scriptsConfig.enableAutoProjectGeneration = true;
        }

        // Explicitly set the native library before CoreBridge init to avoid CWD heuristics.
        scriptsNativeDllPath = ScriptingPaths::ResolveNativeLibraryPath();
        if (scriptsNativeDllPath.empty())
        {
            // Fallback for unusual dev layouts; prefer the platform-specific resolver above.
            scriptsNativeDllPath = PathUtils::GetExecutableDirectory() / "GameEngine.Native.dll";
        }
    }

    // If scripting is effectively disabled by config, skip ScriptManager initialization entirely.
    // This keeps headless tools/tests fast and avoids unnecessary file watcher + hot reload pipeline setup.
    const bool scriptingFullyDisabled =
        scriptsConfig.disableClr &&
        !scriptsConfig.enableHotReload &&
        !scriptsConfig.enableAsyncHotReload &&
        !scriptsConfig.enableAutoProjectGeneration;

    tPhase = std::chrono::high_resolution_clock::now();
    if (!scriptingFullyDisabled)
    {
        // GetScriptManager() may have constructed the manager before Initialize (a caller that
        // configures its CLR host ahead of startup); the constructor captures no configuration,
        // so that instance is the one initialized here rather than replaced.
        if (!m_ScriptManager)
        {
            m_ScriptManager = MakeUnique<ScriptManager>();
        }
        // Engine init and Update run on this thread; record it so off-main-thread
        // pumping of main-thread tasks gets diagnosed instead of passing silently.
        m_ScriptManager->MarkMainThread();

        if (!scriptsNativeDllPath.empty())
        {
            m_ScriptManager->SetNativeLibraryPath(scriptsNativeDllPath);
            Logger::Log::Debug("Scripting native library set: workspace={} native={}",
                               scriptsConfig.workspaceRoot.string(),
                               scriptsNativeDllPath.string());
        }

        if (!m_ScriptManager->Initialize(scriptsConfig, *m_JobSystem))
        {
            Logger::Log::Error("Failed to initialize script manager");
            return false;
        }
        Logger::Log::Info("Script manager initialized");

        // Enable asset/script hot reloading based on scripts configuration
        if (scriptsConfig.enableHotReload)
        {
            // Always enable AssetManager hot-reload when requested so that
            // non-script assets (e.g., UI layouts/styles, textures, shaders)
            // participate in the hot-reload pipeline.
            m_AssetManager->SetHotReloadEnabled(true);

            // ScriptManager hot-reload is only available when scripting is
            // compiled in (GE_ENABLE_SCRIPTING=1).
            m_ScriptManager->SetHotReloadEnabled(true);
            Logger::Log::Info("Hot reloading enabled");

            if (scriptsConfig.enableAsyncHotReload)
            {
                m_ScriptManager->SetAsyncHotReloadEnabled(true);
                Logger::Log::Info("Async hot-reload enabled - hot-reload operations will be non-blocking");
            }
            else
            {
                Logger::Log::Info("Async hot-reload disabled - using synchronous hot-reload");
            }
        }
    }
    else
    {
        Logger::Log::Info("ScriptManager disabled by config; skipping ScriptManager initialization");
    }
#else
    // Even when scripting is compiled out, we still want asset hot
    // reloading (e.g., UI CSS/layout, textures, shaders) to work when
    // requested via ScriptsConfig. Honor the engine-level scripts config
    // for the AssetManager here.
    if (m_ScriptsConfig.has_value() && m_ScriptsConfig->enableHotReload)
    {
        m_AssetManager->SetHotReloadEnabled(true);
        Logger::Log::Info("Asset hot reloading enabled (scripting disabled at build time)");
    }
    Logger::Log::Info("Scripting disabled at build time (GE_ENABLE_SCRIPTING=0) - skipping ScriptManager initialization");
#endif
    Logger::Log::Info("[Startup] ScriptManager: {:.1f}ms", MsSince(tPhase));

    // Engine components register themselves: AutoComponentRegistrar<T> enters
    // each one in the ComponentRegistry at static init or on its first World use,
    // under its canonical type name. Only plugin components arrive through a hook.
    tPhase = std::chrono::high_resolution_clock::now();
    Plugins::EnginePluginRegistry::Get().RegisterEngineComponents();
    Logger::Log::Info("[Startup] Plugin components: {:.1f}ms", MsSince(tPhase));

    // Before any system or editor code can look up a scene schema.
    Scene::RegisterBuiltInSceneSchemas();

    // Initialize engine-owned audio system asynchronously (no downstream deps during startup).
    tPhase = std::chrono::high_resolution_clock::now();
    m_StartupChannel = MakeUnique<JobSystem::JobChannel>(
        *m_JobSystem, JobSystem::JobChannelDesc{.Name = "Engine startup", .MaxRunning = 2});
    m_AudioSystem = MakeUnique<Audio::AudioSystem>(*m_AssetManager);
    {
        // Opening the device waits on the OS, so it is a startup-channel job. Where the
        // joining thread cannot block (Platform::SupportsTransientThreads: the browser
        // main thread), GetAudioSystem() runs it inline on first use instead.
        if (Platform::SupportsTransientThreads())
        {
            Audio::AudioSystem* audioRaw = m_AudioSystem.get();
            m_AudioInit = m_StartupChannel->Submit([audioRaw]() -> bool { return InitializeAudio(*audioRaw); });
        }
        else
        {
            m_AudioInitDeferred = true;
        }
    }
    Logger::Log::Info("[Startup] AudioSystem: launched async ({:.1f}ms to dispatch)", MsSince(tPhase));

    // If engine-managed rendering loop is enabled, ensure world exists
    if (m_RenderingLoopEnabled)
    {
        (void)EnsurePrimaryWorld();
    }

    transaction.Commit();
    Logger::Log::Info("[Startup] EngineCore::Initialize TOTAL: {:.1f}ms", MsSince(tEngineStart));
    return true;
}

GameEngine::ECS::World* GameEngine::EngineCore::EnsurePrimaryWorld()
{
    if (!m_PrimaryWorld)
    {
        // World::GetJobSystem() is authoritative for parallel execution:
        // systems never resolve a pool through EngineCore. Non-null = parallel
        // on that pool; null = deliberately serial (thumbnail/preview bake
        // worlds, bare test worlds). The primary world is wired to the engine
        // pool here — the same pool the RenderingLoop SystemManager dispatches
        // on, which keeps participating Waits inside systems deadlock-free.
        // Lifetime invariant: Shutdown() destroys m_PrimaryWorld before
        // m_JobSystem resets ("job system last"), so this pointer outlives
        // every tick of the world. Keep that order.
        //
        // Creation-order invariant: application code may reach this BEFORE
        // Initialize creates the pool (the editor does). Pass the nullable
        // pointer — never &GetJobSystem(), which derefs a null unique_ptr in
        // that window (UB) — and let Initialize backfill post-pool-creation.
        m_PrimaryWorld = GameEngine::MakeUnique<ECS::World>(m_JobSystem.get());

        // Component teardown is registered by the module that owns the resource,
        // never enumerated here: each registrar releases its own component types
        // from World::Clear (scene close), World::DestroyEntity and a component
        // remove alike.
        Engine::Renderer::RegisterRenderWorldHooks(*m_PrimaryWorld);
        TerrainECS::RegisterTerrainWorldHooks(*m_PrimaryWorld);
        PhysicsECS::RegisterPhysicsWorldHooks(*m_PrimaryWorld);
        PathfindingECS::RegisterNavigationWorldHooks(*m_PrimaryWorld);

        // Per-entity dirty feed (change-signaling §5 P2): subscribe the
        // engine-driven world to WorldTransform. Only worlds whose swap is
        // driven by the engine tick (EngineCore::Update) enable the feed —
        // thumbnail/preview worlds stay unsubscribed so emission is a no-op
        // there and nothing accumulates without a consumer.
        m_PrimaryWorld->EnableComponentDirtyFeed(
            ECS::GetComponentTypeId<Components::WorldTransform>());

        // Lifecycle events (change-signaling §6 P3): subscribe the types
        // whose systems consume Added/Removed. Same engine-tick-driven-world
        // rule as the dirty feed — thumbnail/preview worlds stay
        // unsubscribed, so recording there is one empty signature check.
        // First consumer: AudioEmitterSystem (event-driven voice teardown).
        m_PrimaryWorld->EnableLifecycleEvents<Components::AudioEmitter>();
        // Consumer: editor HierarchyPanel incremental sync — Removed<Parent> is the
        // only detector for a parent-component REMOVE (the entity leaves the Parent
        // archetype, so the Changed<Parent> scan can't see it); Added<Parent> backs
        // the value-write scan for component ADDs.
        m_PrimaryWorld->EnableLifecycleEvents<Components::Parent>();
        // Consumer: TerrainModifierSystem change gating — Removed<T> is the only
        // detector for a modifier remove/destroy (the entity leaves the modifier
        // archetype, invisible to the Changed<T> scans); Added<T> covers spawns.
        // The type set is TerrainECS's to own, so it is folded out of the
        // canonical lists rather than restated here.
        TerrainECS::EnableTerrainModifierLifecycleEvents(*m_PrimaryWorld);

        Logger::Log::Info("Primary ECS world created");
    }
    return m_PrimaryWorld.get();
}

void EngineCore::DiscardAudioInitAfterFailedInitialize() noexcept
{
    std::lock_guard<std::mutex> lock(m_AudioInitMutex);
    const bool launched = m_AudioInit.IsValid();
    if (!launched && !m_AudioInitDeferred)
        return;

    Logger::Log::Error("EngineCore::Initialize failed after launching the AudioSystem init; joining that init and "
                       "discarding the AudioSystem so the engine can be initialized again");
    m_AudioInitDeferred = false;
    if (launched)
    {
        m_AudioInit.Wait();
        bool initialized = false;
        const bool completed = m_AudioInit.TryGetResult(initialized);
        m_AudioInit = JobSystem::TaskHandle();
        try
        {
            if (completed && initialized && m_AudioSystem)
                m_AudioSystem->Shutdown();
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("AudioSystem teardown after a failed Initialize threw: {}", e.what());
        }
        catch (...)
        {
            Logger::Log::Error("AudioSystem teardown after a failed Initialize threw a non-std exception");
        }
    }
    m_AudioSystem.reset();
}

bool EngineCore::JoinAudioInitLocked()
{
    if (m_AudioInitDeferred)
    {
        m_AudioInitDeferred = false;
        return m_AudioSystem && InitializeAudio(*m_AudioSystem);
    }
    if (!m_AudioInit.IsValid())
        return true;
    // A job the shutdown drain cancelled never ran: no result, so not initialized.
    m_AudioInit.Wait();
    bool initialized = false;
    const bool completed = m_AudioInit.TryGetResult(initialized);
    m_AudioInit = JobSystem::TaskHandle();
    return completed && initialized;
}

Audio::AudioSystem* EngineCore::GetAudioSystem()
{
    // Joins this lifetime's init exactly once; later calls and the next
    // Initialize/Shutdown cycle find nothing outstanding.
    std::lock_guard<std::mutex> lock(m_AudioInitMutex);
    if (!JoinAudioInitLocked())
        m_AudioSystem.reset();
    return m_AudioSystem.get();
}

void EngineCore::Shutdown()
{
    if (!IsInitialized())
    {
        return;
    }

    Logger::Log::Info("Shutting down Game Engine");

    // Tear down engine-managed rendering loop and primary ECS world while the
    // process is still in a well-defined state (before static destruction).
    // This avoids lifetime issues during CRT atexit when the Engine singleton
    // and thread-local ECS buffers are being torn down.

    // Disable the optional engine-owned rendering loop first so no systems
    // continue to reference the primary world during or after its destruction.
    if (m_RenderingLoopEnabled || m_RenderingLoop)
    {
        DisableRenderingLoop();
    }

    // Destroy the primary ECS world explicitly during EngineCore::Shutdown so that
    // World::~World() and its internal cleanup (including thread-local
    // command-buffer mappings) run before the Engine singleton is destroyed at
    // process exit.
    if (m_PrimaryWorld)
    {
        // Release the per-World Scene TLAS before the World goes away: the
        // TLAS lives in the Scene module's registry keyed by the World id, so
        // dropping the World without releasing leaks the TLAS (~14 MB at 10⁵
        // entities).
        GameEngine::Scene::ReleaseSceneTlas(*m_PrimaryWorld);
        m_PrimaryWorld.reset();
        Logger::Log::Info("Primary ECS world destroyed");
    }

    ShutdownWorldServices();

    // Shutdown the shared physics world after worlds/systems are torn down.
    if (PhysicsECS::PhysicsWorldService::IsInitialized())
    {
        PhysicsECS::PhysicsWorldService::Shutdown();
    }

    // Shutdown subsystems in reverse order
    if (m_ScriptManager)
    {
        m_ScriptManager->Shutdown();
        m_ScriptManager.reset();
        Logger::Log::Info("Script manager shutdown");
    }

    // Native script manager: before AssetManager so its file-watch subscriptions
    // tear down while the watching service is still alive.
    if (m_NativeScriptManager)
    {
        m_NativeScriptManager->Shutdown();
        m_NativeScriptManager.reset();
        Logger::Log::Info("Native script manager shutdown");
    }

    // Join the async audio init before tearing audio down: the init job must
    // not outlive the AudioSystem it is initializing.
    (void)GetAudioSystem();
    // The startup jobs are joined (the audio init above, an application's
    // preloads by the application before this Shutdown).
    m_StartupChannel.reset();
    // Shutdown audio before AssetManager so any clip resolution/handles are no longer used.
    if (m_AudioSystem)
    {
        m_AudioSystem->Shutdown();
        m_AudioSystem.reset();
        Logger::Log::Info("Audio system shutdown");
    }

    if (m_AssetManager)
    {
        // If we registered the rendering hot-reload bridge, unregister it before tearing down assets
        if (m_ShaderHotReloadHandle != 0)
        {
            try
            {
                EngineIntegration::UnregisterShaderHotReloadBridge(m_AssetManager->GetEventDispatcher(), m_ShaderHotReloadHandle);
            }
            catch (const std::exception& e)
            {
                Logger::Log::Warning("Failed to unregister shader hot-reload bridge: {}", e.what());
            }
            m_ShaderHotReloadHandle = 0;
        }

        // Unregister core engine asset types
        try
        {
            auto& typeRegistry = m_AssetManager->GetAssetTypeRegistry();
            typeRegistry.UnregisterAssetType(AssetType::Audio);
            typeRegistry.UnregisterAssetType(AssetType::Model);
            Logger::Log::Info("Core asset types unregistered");
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("Exception unregistering core asset types: {}", e.what());
        }

        m_AssetManager->Shutdown();
        m_AssetManager.reset();

        Logger::Log::Info("Asset manager shutdown");
    }

    // Shutdown job system last (after all subsystems that use it). The primary
    // world (wired to this pool at EnsurePrimaryWorld) was destroyed above —
    // keep that order. The lifecycle returns to Uninitialized only below, so there
    // is a window where IsInitialized() still reads true with a dead pool; it is
    // unreachable because systems take pools from World::GetJobSystem() (never
    // EngineCore) and everything ticking worlds stopped at DisableRenderingLoop().
    if (m_JobSystem)
    {
        m_JobSystem.reset();
        Logger::Log::Info("Job system shutdown");
    }

    m_ShaderHookState.reset();

    m_WorkspaceRoot.clear();
    m_ResolvedAssetRoot.clear();
    m_AuthoritativeAssetDbFile.clear();
    m_AssetDbCacheRoot.clear();

    // Host application may outlive Engine shutdown in test harnesses; clear non-owning pointers.
    m_InputSystem = nullptr;
    m_RuntimeInput = nullptr;

    m_Lifecycle.store(EngineLifecycleState::Uninitialized, std::memory_order_release);
    Logger::Log::Info("Game Engine shutdown complete");
}

void EngineCore::SetWorkspaceRoot(const std::filesystem::path& newRoot)
{
    if (newRoot.empty())
    {
        Logger::Log::Warning("SetWorkspaceRoot called with empty path");
        return;
    }

    // Update all workspace-dependent paths
    m_WorkspaceRoot = ResolveWorkspaceRoot(newRoot);
    m_WorkspaceRootIsFallback = false;
    const ProjectPaths projectPaths = ResolveProjectPaths(m_WorkspaceRoot, m_Config);
    m_ResolvedAssetRoot = projectPaths.AssetRoot;
    m_AuthoritativeAssetDbFile = projectPaths.AssetDatabaseFile;
    m_AssetDbCacheRoot = projectPaths.AssetDatabaseCacheRoot;

    // Propagate to ScriptManager so it watches the new project's script directory.
    if (m_ScriptManager && m_ScriptManager->IsInitialized())
    {
        ScriptsConfig newScriptsConfig;
        newScriptsConfig.workspaceRoot = m_WorkspaceRoot;
        newScriptsConfig.scriptsRoot = m_ResolvedAssetRoot;
        newScriptsConfig.assembliesRoot = m_WorkspaceRoot / "ScriptAssemblies";
        m_ScriptManager->RebindProjectScripts(newScriptsConfig);
    }

    ApplyTerrainBakeCacheLocation();
    Logger::Log::Info("Engine workspace root updated to: {}", m_WorkspaceRoot.string());
}

void EngineCore::ApplyTerrainBakeCacheLocation()
{
    if (!TerrainECS::TerrainService::IsInitialized())
        return;
    // A packaged game reads the bakes its build staged beside the asset manifest
    // and never writes beside its install; a run on a project workspace reads and
    // fills that workspace's derived-data cache; a run without one always bakes.
    TerrainECS::TerrainBakeCacheConfig cache;
    if (m_ScriptsConfig && m_ScriptsConfig->packagedMode && m_AssetManager)
        cache = {m_AssetManager->GetAssetRoot() / ".terrainbake", false};
    else if (!m_WorkspaceRoot.empty() && !m_WorkspaceRootIsFallback)
        cache = {m_WorkspaceRoot / ".Cache" / "TerrainBake", true};
    TerrainECS::TerrainService::Get().SetBakeCache(std::move(cache));

    // The cooked height stores the paged terrains read: the build's terrain containers in a
    // packaged game (never cooked on load), the workspace's derived-data cache otherwise (cooked
    // there in the background when missing), none without a workspace.
    TerrainECS::HeightPageStoreLocation pages;
    if (m_ScriptsConfig && m_ScriptsConfig->packagedMode && m_AssetManager)
        pages.PackagedDirectory = m_AssetManager->GetAssetRoot() / "Cooked" / "Terrain";
    else if (!m_WorkspaceRoot.empty() && !m_WorkspaceRootIsFallback)
        pages.CacheDirectory = m_WorkspaceRoot / ".Cache" / "TerrainPages";
    TerrainECS::TerrainService::Get().SetHeightPageStoreLocation(std::move(pages));
}

void EngineCore::Update(float64 deltaTime)
{
    if (!IsInitialized())
    {
        return;
    }

    // Update asset manager (process loading queue, check for file changes)
    if (m_AssetManager)
    {
        m_AssetManager->Update();
    }

    // Update audio system once per frame on the main thread.
    if (m_AudioSystem)
    {
        m_AudioSystem->Update(static_cast<float>(deltaTime));
    }

    // For engine-managed worlds, ensure deferred ECS commands are flushed
    // once per frame before any systems run so that frame N's systems see a
    // consistent view of commands enqueued during N (or late in N-1).
    if (m_PrimaryWorld)
    {
        m_PrimaryWorld->ProcessCommands();

        // Dirty-feed frame boundary (change-signaling §5 P2, C6 precedent):
        // exactly once per frame, next to — never inside — the deferred
        // flush (ProcessCommands has mid-frame call sites that would destroy
        // entries before late consumers run). After the flush so entries
        // from just-played-back deferred SETs are promoted immediately.
        // Consumers Snapshot() both feed buffers, so correctness does not
        // depend on where in the frame this lands — entries survive at
        // least one full frame regardless.
        m_PrimaryWorld->SwapComponentDirtyFeed();

        // Lifecycle-event frame boundary (change-signaling §6 P3, same C6
        // discipline). After the flush, so structural ops played back just
        // above are promoted and visible to THIS frame's systems; anything
        // recorded later in the frame (editor tools, managed sets) surfaces
        // next frame (the documented visibility inversion). Unlike the feed,
        // consumers read only the current window — an event is delivered for
        // exactly one frame, so event consumers must run every tick (no poll
        // fallback exists; see LifecycleEvents.h on consumer cadence).
        m_PrimaryWorld->SwapLifecycleEvents();
    }

    // Drive engine-managed rendering loop if enabled and configured to be
    // stepped automatically from EngineCore::Update. Tools like the Editor can
    // disable this via SetRenderingLoopAutoDrive(false) and call
    // StepRenderingLoop() manually after their own per-frame updates so that
    // editor-driven Transform changes are reflected in the same frame.
    if (m_RenderingLoopAutoDrive && m_RenderingLoopEnabled && m_RenderingLoop && m_PrimaryWorld)
    {
        m_RenderingLoop->Update(*m_PrimaryWorld, (float32)deltaTime);
    }

    // CRITICAL: Process main thread tasks for hot reload system
    if (m_ScriptManager)
    {
        size_t tasksProcessed = m_ScriptManager->ProcessMainThreadTasks();
        if (tasksProcessed > 0)
        {
            Logger::Log::Debug("Processed {} main thread tasks", tasksProcessed);
        }
    }

    // Native script manager: fire the debounced "source settled" action once edits quiesce.
    if (m_NativeScriptManager)
    {
        m_NativeScriptManager->Tick();
    }
}

EngineCore& EngineCore::GetInstance()
{
    if (s_Instance == nullptr)
    {
        Logger::Log::Debug("No engine instance bound — binding the default instance (expected during DLL bootstrap)");
        // Create a default instance to prevent crashes.
        //
        // NOTE: This instance is intentionally leaky. Some test harnesses and embedding
        // scenarios call EngineCore::GetInstance() without ever creating a host-owned Engine.
        // If we return a function-local static `Engine` by value, its destructor runs during
        // process shutdown and can trigger shutdown-order crashes (static/TLS teardown ordering).
        static EngineCore* defaultInstance = new EngineCore();
        // s_Instance is the one record of which engine is current: GetInstance returns it and
        // the constructor's duplicate check reads it. The constructor binds it the first time;
        // after a host-owned engine's destructor cleared it, the default is bound again here.
        s_Instance = defaultInstance;
    }
    return *s_Instance;
}

void EngineCore::EnableRenderingHotReload(Rendering::IDevice& device)
{
    if (!m_AssetManager)
    {
        Logger::Log::Warning("EnableRenderingHotReload called but AssetManager is not initialized");
        return;
    }
    if (m_ShaderHotReloadHandle != 0)
    {
        Logger::Log::Info("Rendering hot-reload bridge already registered (handle={})", m_ShaderHotReloadHandle);
        return;
    }
    try
    {
        m_ShaderHotReloadHandle = EngineIntegration::RegisterShaderHotReloadBridge(device, m_AssetManager->GetEventDispatcher());
        Logger::Log::Info("Rendering hot-reload bridge registered (handle={})", m_ShaderHotReloadHandle);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Failed to register rendering hot-reload bridge: {}", e.what());
        m_ShaderHotReloadHandle = 0;
    }
}

void EngineCore::DisableRenderingHotReload()
{
    if (!m_AssetManager)
    {
        return;
    }

    if (m_ShaderHotReloadHandle != 0)
    {
        try
        {
            EngineIntegration::UnregisterShaderHotReloadBridge(m_AssetManager->GetEventDispatcher(), m_ShaderHotReloadHandle);
            Logger::Log::Info("Rendering hot-reload bridge unregistered (handle={})", m_ShaderHotReloadHandle);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Warning("Failed to unregister rendering hot-reload bridge: {}", e.what());
        }
        m_ShaderHotReloadHandle = 0;
    }
}

namespace
{
void AddOwnedPluginSystems(ECS::SystemScheduleBuilder& schedule,
                          Plugins::IEnginePlugin& plugin,
                          Plugins::EnginePluginContext& context)
{
    schedule.BeginOwnedRegistrations(plugin.GetDescriptor().Id);
    plugin.AddSystemsToSchedule(schedule, context);
    schedule.EndOwnedRegistrations();
}

void RegisterLateRenderingPlugin(Engine::Renderer::RenderingLoop& renderingLoop,
                                 Plugins::IEnginePlugin& plugin,
                                 Plugins::EnginePluginContext& context)
{
    const char* pluginIdentifier = plugin.GetDescriptor().Id;
    plugin.RegisterEngineComponents();
    plugin.RegisterSceneSchemas();
    plugin.RegisterRenderPipelineNodes(context);
    auto& schedule = renderingLoop.GetScheduleBuilder();
    const size_t registrationsBefore = schedule.GetRegistrationCount();
    AddOwnedPluginSystems(schedule, plugin, context);
    if (schedule.GetRegistrationCount() != registrationsBefore ||
        schedule.HasPendingRetirements())
        renderingLoop.IntegrateLateSystems();
    plugin.OnRuntimeInitialized(context);
    Logger::Log::Info(
        "Engine plugin '{}' registered after runtime init — hooks replayed; "
        "its systems run at their declared phase/order (schedule re-solved)",
        pluginIdentifier ? pluginIdentifier : "<null>");
}
} // namespace

bool EngineCore::EnableRenderingLoop(Engine::Renderer::RenderServices* renderServices)
{
    if (m_RenderingLoopEnabled && m_RenderingLoop)
        return true;
    try
    {
        m_RenderServices = renderServices;
        m_RenderingLoop = MakeUnique<Engine::Renderer::RenderingLoop>();
        if (!m_RenderingLoop->Initialize(renderServices))
        {
            Logger::Log::Error("EnableRenderingLoop: initialization failed");
            m_RenderingLoop.reset();
            m_RenderingLoopEnabled = false;
            m_RenderServices = nullptr;
            return false;
        }
        m_RenderingLoopEnabled = true;
        (void)EnsurePrimaryWorld();

        // Initialize world services and register module pipeline node types.
        InitializeWorldServices();
        ApplyTerrainBakeCacheLocation();
        if (auto* nodeTypes = renderServices->Spine().GetPipelineNodeRegistry())
        {
            TerrainECS::RegisterTerrainPipelineNodes(*nodeTypes);
            CBTTerrainECS::RegisterCBTPipelineNodes(*nodeTypes);
            TerrainGrass::RegisterTerrainGrassPipelineNodes(*nodeTypes);
            Ocean::RegisterOceanPipelineNodes(*nodeTypes);
        }
        else
        {
            Logger::Log::Error("EnableRenderingLoop: RenderServices is not initialized; the engine "
                               "modules' pipeline node types were not registered");
        }
        Plugins::EnginePluginContext pluginContext{};
        pluginContext.RenderServices = renderServices;
        Plugins::EnginePluginRegistry::Get().RegisterRenderPipelineNodes(pluginContext);

        auto& schedule = m_RenderingLoop->GetScheduleBuilder();
        if (m_AudioSystem)
        {
            Engine::Audio::AddAudioSystemsToSchedule(schedule, m_AudioSystem.get());
        }
        PhysicsECS::AddPhysicsSystemsToSchedule(schedule);
        TerrainECS::AddTerrainSystemsToSchedule(schedule, renderServices);
        CBTTerrainECS::AddCBTSystemsToSchedule(schedule, renderServices);
        Ocean::AddOceanSystemsToSchedule(schedule, renderServices);
        schedule.Add<Engine::Renderer::LensFlareExtractionSystem>(
            "LensFlareExtraction", ECS::SystemPhase::Extraction, 7,
            {"TransformHierarchy", "Camera"}, renderServices);
        // Strictly after both extraction systems: VideoTexture
        // writes material + TextureService binding state, which is
        // not synchronized against the extraction-wave workers
        // that read it (MaterialRegistry is single-threaded by
        // contract). An empty dependency list would drop it into
        // wave 0 alongside every other dependency-free system.
        schedule.Add<Video::VideoTextureSystem>(
            "VideoTexture", ECS::SystemPhase::Extraction, 8,
            {"RenderExtraction", "TerrainExtraction"},
            renderServices->GetDevice(), renderServices);
        SplineECS::AddSplineSystemsToSchedule(schedule);
        PathfindingECS::AddPathfindingSystemsToSchedule(schedule);
        // Associate contributions with their plugin so reload reconciliation
        // can replace or retire that plugin's systems.
        for (Plugins::IEnginePlugin* plugin :
             Plugins::EnginePluginRegistry::Get().GetEnabledPlugins())
        {
            AddOwnedPluginSystems(schedule, *plugin, pluginContext);
        }
        m_RenderingLoop->BuildSchedule();

        Plugins::EnginePluginRegistry::Get().OnRuntimeInitialized(pluginContext);

        // Late plugins contribute to the persistent schedule and re-solve its
        // wave plan so their declared dependencies retain their ordering.
        Plugins::EnginePluginRegistry::Get().SetLateRegistrationHandler(
            [this, pluginContext](Plugins::IEnginePlugin& plugin) mutable
            {
                RegisterLateRenderingPlugin(*m_RenderingLoop, plugin, pluginContext);
            });

        Logger::Log::Info("Engine-managed rendering loop enabled");
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("EnableRenderingLoop exception: {}", e.what());
        Plugins::EnginePluginRegistry::Get().SetLateRegistrationHandler({});
        m_RenderingLoop.reset();
        m_RenderingLoopEnabled = false;
        m_RenderServices = nullptr;
        return false;
    }
    catch (...)
    {
        Logger::Log::Error("EnableRenderingLoop unknown exception");
        Plugins::EnginePluginRegistry::Get().SetLateRegistrationHandler({});
        m_RenderingLoop.reset();
        m_RenderingLoopEnabled = false;
        m_RenderServices = nullptr;
        return false;
    }
}

void EngineCore::SetRenderingLoopAutoDrive(bool enabled)
{
    m_RenderingLoopAutoDrive = enabled;
}

void EngineCore::StepRenderingLoop(float32 deltaTime)
{
    if (!m_RenderingLoopEnabled || !m_RenderingLoop || !m_PrimaryWorld)
    {
        return;
    }
    m_RenderingLoop->Update(*m_PrimaryWorld, deltaTime);
}

void EngineCore::DisableRenderingLoop()
{
    if (!m_RenderingLoopEnabled)
        return;
    Plugins::EnginePluginRegistry::Get().SetLateRegistrationHandler({});
    Plugins::EnginePluginRegistry::Get().OnRuntimeShutdown();
    m_RenderingLoopEnabled = false;
    m_RenderingLoop.reset();
    m_RenderServices = nullptr;
    Logger::Log::Info("Engine-managed rendering loop disabled");
}

bool EngineCore::ConfigureRenderingSystemEveryNFrames(const std::string& systemName, uint32 n)
{
    if (!m_RenderingLoopEnabled || !m_RenderingLoop)
        return false;
    auto* sm = m_RenderingLoop->GetSystemManager();
    if (!sm)
        return false;
    bool ok = sm->SetSystemEveryNFramesByName(systemName, n);
    if (ok)
    {
        Logger::Log::Info("Rendering system '{}' configured to run every {} frame(s)", systemName, n);
    }
    else
    {
        Logger::Log::Warning("ConfigureRenderingSystemEveryNFrames: system '{}' not found", systemName);
    }
    return ok;
}

bool EngineCore::SetRenderingSystemEnabled(const std::string& systemName, bool enabled)
{
    if (!m_RenderingLoopEnabled || !m_RenderingLoop)
        return false;
    auto* sm = m_RenderingLoop->GetSystemManager();
    if (!sm)
        return false;
    const bool ok = sm->SetSystemEnabledByName(systemName, enabled);
    if (!ok)
    {
        Logger::Log::Warning("SetRenderingSystemEnabled: system '{}' not found", systemName);
    }
    return ok;
}

Engine::Renderer::RenderingLoop* EngineCore::GetRenderingLoop() const
{
    if (!m_RenderingLoopEnabled)
        return nullptr;
    return m_RenderingLoop.get();
}

bool EngineCore::AreNativeModulesPending() const
{
    return m_NativeScriptManager && m_NativeScriptManager->AreModulesPending();
}

bool EngineCore::HasFailedNativeModuleBuild() const
{
    return m_NativeScriptManager && m_NativeScriptManager->AnyShippedModuleBuildFailed();
}

} // namespace GameEngine
