#include "PlayerApplication.h"

#include "Assets/AssetManager.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageNativeCache.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/Packages/PackagesIndex.h"
#include "Audio/AudioSystem.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/Hosting/RuntimeFrameReadback.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "UI/UITextureSpace.h"
#include "Engine/Rendering/AnimatorScenePlayback.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"

#include <stb_image_write.h>
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "NativeScripting/NativeScriptManager.h"  // load the editor-built user DLL
#include "NativeScripting/UserSystemRegistry.h"   // Start/Tick/StopUserSystems
#include "Platform/Capabilities.h"
#include "Platform/Window.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scripting/ManagedSystemBridge.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainGrass/Scene/TerrainGrassSceneSchemas.h"
#if GE_PLAYER_MOVIE_RECORDER
#include "Video/AsyncVideoRecorder.h"
#endif

#include <cstdlib>
#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine {

namespace
{
#if GE_PLAYER_MOVIE_RECORDER
constexpr size_t kMaxPendingMovieReadbacks = 4;
constexpr uint32_t kMovieStartupWarmupFrames = 4;
// ~20s of 48 kHz stereo float audio: enough to ride out an encoder stall without
// unbounded growth. Past this the oldest buffered audio is dropped.
constexpr size_t kMaxPendingMovieAudioBytes = 8 * 1024 * 1024;
#endif

// Engine-shader path resolution for the Player's own copy of Rendering::Utils.
//
// On Windows/Linux the Player links GameEngineRendering directly (see
// CMakeLists), so the Rendering::Utils shader-resolution globals exist in a
// second copy inside the Player executable, distinct from the one Engine::
// Initialize configures inside Engine. Passes compiled into that copy (e.g.
// SRGBEncodePass, scheduled by the movie capture) would read
// an unconfigured resolver and throw "no shader path resolver configured". We
// install the same hooks here so the Player's copy is configured too. (On macOS
// the Player links only Engine, so there is a single copy and this is a benign
// no-op.)
//
// This copy serves only the passes compiled into Player.exe, which ask for bare
// engine shader names. It prefers the 'editor' mount and otherwise falls back to
// the ordinary project-first resolve, so it does not carry Engine.cpp's stricter
// engine-shader policy (no project fallback once an editor mount exists) nor its
// authored-source-prefix handling. Custom rendergraph nodes live in Engine and
// resolve through Engine.dll's copy, which does.
std::filesystem::path PlayerResolveEngineShaderPath(const std::filesystem::path& relativePath)
{
    return EngineCore::GetInstance().GetAssetManager().ResolveAssetPathPreferringSource(
        relativePath, kAssetSourceAliasEditor, AssetPathKind::AnyEntry);
}

std::vector<uint8_t> PlayerShaderBytecodeLoader(const char* name)
{
    if (!name || !name[0])
        return {};
    const auto resolved = PlayerResolveEngineShaderPath(name);
    if (resolved.empty())
        return {};
    return Rendering::Utils::ReadFile(resolved.string());
}

// The fix for a render pipeline the Player could not resolve at startup, per
// failure; `configured` says whether game.config named the pipeline.
const char* RenderPipelineFix(Engine::Renderer::PipelineResolveFailure failure, bool configured)
{
    using Engine::Renderer::PipelineResolveFailure;
    switch (failure)
    {
    case PipelineResolveFailure::NotInMount:
        return configured ? "Correct the \"renderPipeline\" key in game.config, or add the file to the "
                            "project's Assets."
                          : "game.config has no \"renderPipeline\" key, so the Player looked for the engine's "
                            "default pipeline: add the key naming the project's .rendergraph.";
    case PipelineResolveFailure::LoadFailed:
        return "Repair the file, or replace it with a valid .rendergraph.";
    case PipelineResolveFailure::Rejected:
        return "Correct the graph as each compile error says; register each unknown pass type in a native "
               "module the Player loads (the project's or a package's), or remove its node.";
    case PipelineResolveFailure::None:
        break;
    }
    return "";
}
}

PlayerApplication::PlayerApplication(const ApplicationConfig& config, const GameConfig& gameConfig,
                                     bool tickManagedSystems,
#if GE_PLAYER_MOVIE_RECORDER
                                     std::optional<Video::VideoWriterOptions> movieOptions,
                                     uint64_t movieFrameLimit,
#endif
                                     bool allowRuntimeHdrToggle,
                                     std::string screenshotPath)
    : Application(config)
    , m_GameConfig(gameConfig)
    , m_TickManagedSystems(tickManagedSystems)
    , m_RuntimeHdrRequestedMode(gameConfig.hdrMode != Rendering::HdrOutputMode::Off
                                    ? gameConfig.hdrMode
                                    : Rendering::HdrOutputMode::Auto)
    , m_RuntimeHdrBitDepth(gameConfig.hdrSwapchainBitDepth)
    , m_AllowRuntimeHdrToggle(allowRuntimeHdrToggle)
#if GE_PLAYER_MOVIE_RECORDER
    , m_MovieOptions(std::move(movieOptions))
    , m_MovieFrameLimit(movieFrameLimit)
#endif
    , m_ScreenshotPath(std::move(screenshotPath))
{
    RuntimeHostHooks hooks;
    hooks.OnDeviceFailed = [this] { HandleFatalDeviceLoss(); };
    hooks.OnFrameComposited = [this](RuntimeFrameReadback& readback) { OnFrameComposited(readback); };
    m_Host = std::make_unique<RuntimeHost>(*this, MakeRuntimeHostDesc(), std::move(hooks));

    // Warm up a few frames before the one-shot capture: first-frame shader compiles
    // settle and the C++ ValidationSystem-style OnStart spawn has populated the world.
    // Material variants compile asynchronously, so a cold cache needs far more than
    // the default: GE_SCREENSHOT_WARMUP_FRAMES raises it for headless verification.
    if (!m_ScreenshotPath.empty())
    {
        constexpr uint32_t kDefaultScreenshotWarmupFrames = 12;
        m_ScreenshotWarmupFrames = kDefaultScreenshotWarmupFrames;
        if (const char* env = std::getenv("GE_SCREENSHOT_WARMUP_FRAMES"))
        {
            const long parsed = std::strtol(env, nullptr, 10);
            if (parsed > 0)
                m_ScreenshotWarmupFrames = static_cast<uint32_t>(parsed);
        }
    }
}

PlayerApplication::~PlayerApplication()
{
    // Keep shutdown virtual dispatch on PlayerApplication while render/device
    // members are still alive. The base Application destructor can only call
    // the base shutdown path once derived destruction has begun.
    Shutdown();
}

bool PlayerApplication::Initialize()
{
    if (!Application::Initialize())
        return false;

#if GE_PLAYER_MOVIE_RECORDER
    if (m_MovieOptions)
    {
        // Resolve Auto before the capability check so the refusal names the codec
        // the run would actually have used. Refusing here — before any recorder or
        // capture state exists — is what keeps an unsupported build from rendering,
        // encoding and reading back a frame every tick for a file it can never write.
        if (m_MovieOptions->codec == Video::VideoCodec::Auto)
            m_MovieOptions->codec = Video::GuessCodecForMoviePath(m_MovieOptions->path);
        if (!Video::VideoWriter::IsCodecSupported(m_MovieOptions->codec))
        {
            Logger::Log::Error(
                "Player: Cannot record '{}': codec {} is not supported by this platform/build. "
                "This build has no video encoder, so --write-movie cannot produce a file",
                m_MovieOptions->path, Video::ToString(m_MovieOptions->codec));
            m_MovieOptions.reset();
            return false;
        }

        SetFixedFrameRate(m_MovieOptions->fps);
        m_MovieWarmupFramesRemaining = kMovieStartupWarmupFrames;
        m_MovieAudioCaptureArmed = false;
        m_PendingMovieEncodeFrame.reset();
        ClearPendingMovieAudio();
        {
            std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
            m_MovieRecorder = std::make_shared<Video::AsyncVideoRecorder>();
            std::string error;
            if (!m_MovieRecorder->Start(*m_MovieOptions, {}, &error))
            {
                Logger::Log::Error("Player: Failed to start movie recorder '{}': {}", m_MovieOptions->path, error);
                m_MovieOptions.reset();
                return false;
            }
            m_MovieAudioChannels = m_MovieOptions->audioChannels;
            m_MovieAudioSampleRate = m_MovieOptions->audioSampleRate;
        }
        m_LastMovieFramePixels.clear();
        m_LastMovieFrameWidth = 0;
        m_LastMovieFrameHeight = 0;
        m_LastMovieFrameStrideBytes = 0;
        Logger::Log::Info(
            "Player: Movie maker mode enabled path='{}' fps={} codec={} audio={} hardware={}",
            m_MovieOptions->path,
            m_MovieOptions->fps,
            Video::ToString(m_MovieOptions->codec),
            m_MovieOptions->recordAudio ? "on" : "off",
            m_MovieOptions->requireHardwareAcceleration ? "required" : "allowed-software-fallback");
        if (m_MovieFrameLimit > 0)
            Logger::Log::Info("Player: Movie capture will quit after {} frames", m_MovieFrameLimit);
    }
#endif

    if (!m_Host->InitWindow())
        return false;

    // Mount staged runtime assets as 'editor'. BuildPlayerEditorSourceDesc
    // picks the mount shape per layout (distinct staged root, same-root dev
    // alias) and returns nothing for a packaged game, whose editor content is
    // fused into the manifest mount at build time.
    {
        auto& am = EngineCore::GetInstance().GetAssetManager();
        const auto projectAssetsRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
        const auto stagedAssetsRoot = PathUtils::GetInstallAssetsRoot();
        if (const auto editorSource = BuildPlayerEditorSourceDesc(projectAssetsRoot, stagedAssetsRoot))
        {
            am.RegisterSource(*editorSource);
        }
        else
        {
            Logger::Log::Info("Player: packaged layout — editor content is fused into the packaged "
                              "Assets mount at '{}'; no separate 'editor' source",
                              projectAssetsRoot.string());
        }
    }

    // Mount packages. Two layouts:
    //  - dev: the project's Packages/manifest.json resolves through the same
    //    resolver as the editor (P0 — local/embedded packages, mutable mounts);
    //  - packaged: the build pipeline staged Packages/packages.index + one
    //    immutable Packages/<alias>/Assets mount per package, each with its own
    //    .assetmanifest GUID namespace (P2). Nothing resolves at boot — the
    //    index IS the resolution, burned in at build time.
    // A game with neither file has no packages — the quiet path. Native module
    // load roots are collected here (per layout), loaded after InitRendering.
    struct PackageNativeModule
    {
        std::filesystem::path Root;        // build-cache / staged record root
        std::string Name;                  // module name (NativeBuildConfig::ModuleName)
        std::filesystem::path PrebuiltDir; // shipped binaries root (P3); empty when none
    };
    std::vector<PackageNativeModule> packageNativeModules;
    {
        // Both layouts resolve against the game CONTENT root — the parent of the
        // resolved Assets root, i.e. the staged content root in a packaged game
        // (the exe dir, or Contents/Resources in a mac bundle) and the project
        // dir in a dev run (--asset-root <project>/Assets). This is the
        // same base the staged Assets mount / .assetmanifest and the prebuilt
        // native-script record use. The workspace root is NOT it: in a packaged
        // game that is the per-user prefs dir (writable caches), which holds no
        // game content — resolving Packages/ there silently mounted nothing.
        const std::filesystem::path contentRoot =
            EngineCore::GetInstance().GetResolvedAssetRoot().parent_path();
        auto& am = EngineCore::GetInstance().GetAssetManager();
        std::error_code layoutEc;
        const std::filesystem::path indexFile =
            contentRoot / kPackagesStagingDirName / kPackagesIndexFileName;
        // packages.index is burned in by the build pipeline, so its presence
        // IS the packaged layout. Everything else is a dev run and resolves —
        // even without a Packages/manifest.json, because implicit engine
        // packages (<exe dir>/Packages) apply to every project.
        if (!std::filesystem::exists(indexFile, layoutEc))
        {
            // Code modules are collected in PLAYER context: Editor-kind modules
            // are excluded entirely (they never compile, load, or ship outside
            // the editor).
            const PackageResolution resolution = PackageResolver::Resolve(contentRoot);
            (void)MountResolvedPackages(am, resolution, EngineCore::GetInstance().GetAssetDbCacheRoot());
            for (const PackageCodeModule& module :
                 CollectPackageCodeModules(resolution, /*editorContext=*/false))
            {
                // PrebuiltDir rides along so a prebuilt-only package (P3) loads
                // here too — dev runs have no editor build-cache record for it.
                if (module.Lang == PackageModuleRecord::ModuleLang::Cpp)
                {
                    if (module.UsesManagedNativeCache && !RetainPackageNativeCache(module.CacheDir))
                    {
                        Logger::Log::Error("Player: cannot lease native cache '{}' for package '{}'; module skipped",
                                           module.CacheDir.string(), module.PackageName);
                        continue;
                    }
                    packageNativeModules.push_back(
                        {module.CacheDir, module.AssemblyName, module.PrebuiltDir});
                }
            }
        }
        else
        {
            PackagesIndex index;
            std::string indexError;
            if (!TryLoadPackagesIndex(indexFile, index, indexError))
                Logger::Log::Error("Player: {} — no packages will be mounted", indexError);
            size_t mounted = 0;
            for (const PackagesIndexEntry& entry : index.Packages)
            {
                const std::filesystem::path packageRoot =
                    contentRoot / kPackagesStagingDirName / entry.Alias;
                const std::filesystem::path assetsRoot = packageRoot / "Assets";
                std::error_code ec;
                // packages.index is burned in at build time: an entry whose
                // staged footprint is missing means the shipped game is
                // incomplete (partial copy, deleted folder) — say so loudly.
                if (!std::filesystem::is_directory(packageRoot, ec))
                {
                    Logger::Log::Error(
                        "Player: packages.index lists package '{}' but '{}' is missing — "
                        "the game package is incomplete; its content will not load",
                        entry.Name, packageRoot.generic_string());
                    continue;
                }
                if (std::filesystem::exists(assetsRoot / ".assetmanifest", ec))
                {
                    if (am.RegisterSource(MakePackageMount(entry.Alias, assetsRoot, entry.Priority)))
                        ++mounted;
                    else
                        Logger::Log::Error("Player: failed to mount staged package '{}' at '{}'",
                                           entry.Name, assetsRoot.generic_string());
                }
                else if (std::filesystem::is_directory(assetsRoot, ec))
                {
                    Logger::Log::Error(
                        "Player: staged package '{}' has assets at '{}' but no .assetmanifest — "
                        "its GUID identity cannot be mounted (repackage the game)",
                        entry.Name, assetsRoot.generic_string());
                }
                for (const std::string& moduleName : entry.NativeModules)
                    packageNativeModules.push_back({packageRoot, moduleName, {}});
            }
            if (!index.Packages.empty())
                Logger::Log::Info("Player: mounted {}/{} staged package(s) from packages.index",
                                  mounted, index.Packages.size());
        }
    }

    // Seed the LOD import default tier from game.config before any model loads.
    // The editor seeds this global from its SettingsStore; the Player has no
    // editor, so ModelAsset::PostLoad would otherwise see the engine default
    // (AutoGenerateOnImport=false) and UseGlobal assets would never generate.
    // Per-asset overrides still resolve from the staged .assetmanifest kv.
    {
        LODImportSettings lod;
        lod.AutoGenerateOnImport = m_GameConfig.lod.autoGenerate;
        lod.Config.LodCount = std::clamp<uint32_t>(m_GameConfig.lod.count, 1u, MeshLODConfig::kMaxLODs);
        // Out-of-range values keep the SeamPlanes default rather than clamping
        // to Free (the least-safe rule) — matches LodAssetSettings::Load.
        lod.Config.BorderRule =
            m_GameConfig.lod.borderRule <= static_cast<uint32_t>(MeshLODBorderRule::Free)
                ? static_cast<MeshLODBorderRule>(m_GameConfig.lod.borderRule)
                : MeshLODBorderRule::SeamPlanes;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            lod.Config.TargetRatios[i] = m_GameConfig.lod.ratios[i];
            lod.Config.TargetError[i]  = m_GameConfig.lod.errors[i];
        }
        SetLODImportSettings(lod);
    }

    // Configure the Player's copy of the Rendering::Utils shader hooks (see
    // PlayerResolveEngineShaderPath above). Must run after the 'editor' asset
    // source is mounted so resolution can find staged engine shaders, and before
    // any pass that loads engine shaders is scheduled.
    Rendering::Utils::SetShaderPathResolver(&PlayerResolveEngineShaderPath);
    Rendering::Utils::SetShaderFileLoader(&PlayerShaderBytecodeLoader);

    if (!m_Host->InitRendering())
        return false;

    // The Player has no PlayModeDriver (that is editor-side); ManagedSystemBridge
    // is the ONLY thing that ever calls GameSystemRunner.Initialize/Tick here.
    // Must run after InitRendering (the RenderingLoop owns the SystemManager);
    // the execution plan is already frozen by now, so the bridge lands in a
    // trailing wave (see SystemManager::AddSystem).
    if (m_TickManagedSystems)
        RegisterManagedSystemBridge();

    if (m_AllowRuntimeHdrToggle)
    {
        Logger::Log::Info("Player: Runtime HDR toggle enabled (F10 toggles SDR/HDR, restore mode={} bitDepth={})",
                          Rendering::HdrOutputModeToString(m_RuntimeHdrRequestedMode),
                          m_RuntimeHdrBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10);
    }

    // Register built-in primitive meshes/materials (must happen after editor source
    // is mounted so the material compiler can find shader adapters).
    if (auto* rs = m_Host->GetRenderDeviceContext()->GetRenderServices())
        Engine::Renderer::PrimitiveGenerator::RegisterAll(*rs);

    // Register the plugin scene schemas before any scene loads (EngineCore
    // registers the built-in ones at initialization), and the terrain modules'
    // schemas, whose static auto-registration may be stripped when linking
    // against Engine.lib from the SDK.
    Scene::EnsureBuiltInSchemasRegistered();
    Scene::EnsureTerrainSceneSchemasRegistered();
    Scene::EnsureTerrainGrassSceneSchemasRegistered();

    // Scene entities reference editor GUIDs from the authoritative asset DB. Wait for
    // the project source scan so registry lookups succeed before mesh/material setup.
    EngineCore::GetInstance().GetAssetManager().WaitForStartupScan(kAssetSourceAliasProject);

    // Load the editor-built native user scripts BEFORE the scene so entities carrying user
    // components deserialize against a populated registry. The project root is the parent of the
    // resolved Assets root (it holds .Cache/NativeScripts). No build — the runtime just loads.
    // Package Cpp modules first (topo/mount order): dev modules carry a NativeScripts/build
    // cache under their writable cache dir, staged roots a flat NativeScripts/ record —
    // LoadPrebuiltUserModule reads both layouts.
    if (!Platform::SupportsDynamicNativeModules())
    {
        // Single-static-binary platform (wasm): user native modules are linked
        // in at export time and self-register; there is nothing to load.
        Logger::Log::Info("Player: dynamic native modules unsupported on this platform; "
                          "using statically linked user systems");
    }
    else if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
    {
        for (const auto& module : packageNativeModules)
            nativeScripts->LoadPrebuiltUserModule(module.Root, module.Name, module.PrebuiltDir);
        const std::filesystem::path projectRoot =
            EngineCore::GetInstance().GetResolvedAssetRoot().parent_path();
        nativeScripts->LoadPrebuiltUserModule(projectRoot);
    }

    // After the native modules: loading one registers its pipeline node types,
    // and the pipeline is compiled here, before the scene and the first frame.
    if (const Engine::Renderer::PipelineResolveFailure failure = m_Host->LoadRenderPipeline();
        failure != Engine::Renderer::PipelineResolveFailure::None)
    {
        Logger::Log::Error("Player: cannot start. {}",
                           RenderPipelineFix(failure, !m_GameConfig.renderPipeline.empty()));
        return false;
    }

    if (m_GameConfig.startupScene.empty())
        Logger::Log::Warning("Player: No startup scene specified in game config");
    else if (!m_Host->OpenScene(m_GameConfig.startupScene))
        return false;

    m_StartupResolve = m_Host->GetSceneResolveBatch();
    m_StartupResolvePending = m_StartupResolve != Engine::Renderer::SceneResolveBatch::None;
    m_StartupResolveStart = std::chrono::steady_clock::now();

    if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
    {
        // An autoplay start applies only to entities whose skeleton is bound, which
        // follows their model's resolve: with the startup scene still resolving, the
        // animators start once its batch completes (FinishStartupResolve).
        if (!m_StartupResolvePending)
            Engine::Renderer::BootstrapSceneAnimators(*world);
        // The Player is always "playing": start user C++ systems now (OnStart), tick them each
        // frame (Update), stop them at Shutdown (OnDestroy) — same lifecycle the editor's
        // PlayModeManager drives, minus the edit/play toggle. What OnStart spawns resolves
        // beside the scene.
        GameEngine::NativeScripting::StartUserSystems(*world);
    }

    Logger::Log::Info("Player: Initialized successfully (game='{}')", m_GameConfig.gameName);
    return true;
}

void PlayerApplication::Shutdown()
{
    // Stop user C++ systems (OnDestroy) while the world is still alive.
    if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
        GameEngine::NativeScripting::StopUserSystems(*world);
    GameUI::NotifyGameplayStopped();

    Application::Shutdown();
}

RuntimeHostDesc PlayerApplication::MakeRuntimeHostDesc() const
{
    RuntimeHostDesc desc;
    desc.Title = m_GameConfig.gameName;
    desc.Mode = m_GameConfig.windowMode;
    const ApplicationConfig& appConfig = GetConfig();
    desc.WindowWidth = appConfig.WindowWidth;
    desc.WindowHeight = appConfig.WindowHeight;
    desc.VSync = m_GameConfig.vsync;
    desc.HdrEnabled = m_GameConfig.hdrEnabled;
    desc.HdrMode = m_GameConfig.hdrMode;
    desc.HdrBitDepth = m_GameConfig.hdrSwapchainBitDepth;
    desc.HdrTargetDisplay = m_GameConfig.hdrTargetDisplay;
    desc.HdrMetadata = m_GameConfig.hdrStaticMetadata;
    desc.LodSelection = m_GameConfig.lodSelection;
    desc.RenderQuality = m_GameConfig.renderQuality;
    desc.RenderPipeline = m_GameConfig.renderPipeline;
    desc.UIScale = m_GameConfig.uiScale;
    return desc;
}

void PlayerApplication::RegisterManagedSystemBridge()
{
    auto& engine = EngineCore::GetInstance();
    auto* loop = engine.GetRenderingLoop();
    if (!loop)
    {
        Logger::Log::Warning("Player: Cannot register ManagedSystemBridge — no RenderingLoop");
        return;
    }

    auto* sm = loop->GetSystemManager();
    if (!sm)
    {
        Logger::Log::Warning("Player: Cannot register ManagedSystemBridge — no SystemManager");
        return;
    }

    sm->AddSystem<ManagedSystemBridge>();
    Logger::Log::Info("Player: Registered ManagedSystemBridge (C# GameSystems tick each frame)");
}

void PlayerApplication::PollRuntimeDisplayControls()
{
    if (!m_AllowRuntimeHdrToggle)
        return;

    Input::InputSystem* input = GetInputSystem();
    if (!input)
        return;

    if (input->WasKeyPressed(Input::kKeyCode_F10))
        (void)ToggleRuntimeHdrOutput();
}

bool PlayerApplication::ToggleRuntimeHdrOutput()
{
    Engine::Renderer::RenderDeviceContext* renderCtx = m_Host->GetRenderDeviceContext();
    if (!renderCtx)
        return false;

    Rendering::IDevice* device = renderCtx->GetDevice();
    if (!device)
        return false;

    const bool hdrRequested = m_GameConfig.hdrMode != Rendering::HdrOutputMode::Off;
    const Rendering::HdrOutputMode nextMode = hdrRequested ? Rendering::HdrOutputMode::Off : m_RuntimeHdrRequestedMode;
    return ApplyRuntimeHdrOutput(nextMode);
}

bool PlayerApplication::ApplyRuntimeHdrOutput(Rendering::HdrOutputMode mode)
{
    Engine::Renderer::RenderDeviceContext* renderCtx = m_Host->GetRenderDeviceContext();
    if (!renderCtx)
        return false;

    Rendering::IDevice* device = renderCtx->GetDevice();
    if (!device)
        return false;

    if (!renderCtx->ActivateWindowTarget())
        return false;

    // No device drain around the switch: the swapchain recreate inside
    // SetHdrOutputMode idles every queue the engine submits to before it
    // destroys the images and semaphores, and nothing else can reference them.
    const bool applied = device->SetHdrOutputMode(mode, &m_GameConfig.hdrStaticMetadata, m_RuntimeHdrBitDepth);
    if (!applied)
    {
        Logger::Log::Warning("Player: Runtime HDR switch to {} failed", Rendering::HdrOutputModeToString(mode));
        return false;
    }

    m_GameConfig.hdrEnabled = mode != Rendering::HdrOutputMode::Off;
    m_GameConfig.hdrMode = mode;

    const Rendering::HdrOutputMode activeMode = device->GetActiveHdrOutputMode();
    Logger::Log::Info("Player: Runtime HDR switch requested={} active={} swapchain={} bitDepth={}",
                      Rendering::HdrOutputModeToString(mode),
                      Rendering::HdrOutputModeToString(activeMode),
                      Rendering::ToString(device->GetSwapchainTextureFormat()),
                      m_RuntimeHdrBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10);
    return true;
}

void PlayerApplication::FinishStartupResolve(ECS::World& world)
{
    if (!m_StartupResolvePending)
        return;
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    if (!loop)
        return;
    const Engine::Renderer::SceneResolveService& resolves = loop->GetSceneResolveService();
    ++m_StartupResolveFrames;
    if (!resolves.IsBatchComplete(m_StartupResolve))
        return;
    m_StartupResolvePending = false;

    const Engine::Renderer::SceneResolveBatchProgress progress = resolves.BatchProgress(m_StartupResolve);
    const Engine::Renderer::SceneResolveCounts counts = resolves.Counts();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartupResolveStart).count();
    Logger::Log::Info("Player: startup scene resolved: {} item(s) in {:.2f} s over {} frame(s); session totals: "
                      "{} entities bound, {} missed, {} models registered, {} standalone materials registered, "
                      "{} missed",
                      progress.Total, seconds, m_StartupResolveFrames, counts.ResolvedEntities,
                      counts.MissedEntities, counts.RegisteredModels, counts.RegisteredMaterials,
                      counts.MissedMaterials);
    // The skeleton binder bound the batch's skinned entities on the frame each
    // completed, before this tick, so the autoplay starts find their skeletons.
    Engine::Renderer::BootstrapSceneAnimators(world);
}

void PlayerApplication::Update(float64 deltaTime)
{
    m_LastRenderDeltaTime = static_cast<float>(deltaTime);

    if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
        FinishStartupResolve(*world);

    // Tick user C++ systems each frame (the Player runs them for the whole session).
    if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
    {
        GameEngine::NativeScripting::TickUserSystems(*world, static_cast<float>(deltaTime));
    }

    // Check for window close request (GLFW polling model).
    if (Platform::Window* window = m_Host->GetWindow(); window && window->ShouldClose())
        RequestExit();

    PollRuntimeDisplayControls();

    // Engine::Update() is called by the base Application loop and drives the
    // rendering loop (ECS systems) automatically via SetRenderingLoopAutoDrive(true).
}

void PlayerApplication::HandleFatalDeviceLoss()
{
    if (m_FatalDeviceLossHandled)
        return;
    m_FatalDeviceLossHandled = true;
    Logger::Log::Error(
        "Player: the graphics device was reset and could not be recovered — exiting. "
        "Relaunch to continue (a fresh process gets a working device).");

    // Save-state hook (stub): a shipping game persists checkpoint / autosave state here
    // before the process exits, so a relaunch can resume. The Player has no scene-author
    // save (that is Editor-only); this is where a game's save callback would run.
    // TODO(gameplay): route to the game's autosave when the scripting save API lands.

    RequestExit();
}

void PlayerApplication::Render()
{
#if GE_PLAYER_MOVIE_RECORDER
    // Drained before the frame declares its own capture, so the bounded readback
    // queue the declaration checks reflects what the GPU finished.
    if (!m_FatalDeviceLossHandled)
        PollMovieFrames();
#endif

    m_Host->Render(m_LastRenderDeltaTime);

    if (!m_FatalDeviceLossHandled)
        PollScreenshot();
}

void PlayerApplication::OnFrameComposited(RuntimeFrameReadback& readback)
{
#if GE_PLAYER_MOVIE_RECORDER
    DeclareMovieFrame(readback);
#endif
    DeclareScreenshot(readback);
}

void PlayerApplication::DeclareScreenshot(RuntimeFrameReadback& readback)
{
    // --screenshot: declare a one-shot readback of the composited frame after a
    // short warmup, counted from the frame the startup scene finished resolving.
    // Declared pre-Execute; the host stamps the frame's readbacks after submit.
    if (m_ScreenshotPath.empty() || m_ScreenshotWritten || m_ScreenshotTicket || m_StartupResolvePending)
        return;
    if (m_ScreenshotWarmupFrames > 0)
    {
        --m_ScreenshotWarmupFrames;
        return;
    }
    const RuntimeFrameReadback::Composite& composite = readback.GetComposite();
    if (!composite.PresentSrc.IsValid())
        return;
    m_ScreenshotTicket = Rendering::RequestTextureReadbackRG(&readback.GetDevice(), readback.GetFrame(),
                                                             composite.PresentSrc, "PlayerScreenshot");
    // The space is the declaring frame's stamp, held with the ticket: the SDR HUD
    // chain's composite holds encoded bytes at rest, so the PNG conversion must
    // not apply a second OETF.
    if (m_ScreenshotTicket)
        m_ScreenshotSpace = composite.PresentSpace;
}

void PlayerApplication::PollScreenshot()
{
    // Write the PNG once the one-shot screenshot readback lands, then exit.
    if (!m_ScreenshotTicket)
        return;
    Rendering::ViewReadbackResult shot;
    if (m_ScreenshotTicket->IsConsumed())
    {
        // The readback died before resolving (declaring frame abandoned, or
        // the device was rebuilt): it can never complete. Drop the ticket so
        // the declare re-requests next frame instead of polling forever.
        m_ScreenshotTicket.reset();
        m_ScreenshotSpace.reset();
    }
    else if (m_ScreenshotTicket->TryGet(shot))
    {
        // The declaring frame's stamp for FinalColor, not a guess from `shot.format`.
        std::vector<uint8_t> rgba =
            m_ScreenshotSpace ? Rendering::ReadbackToRgba8Srgb(shot, *m_ScreenshotSpace)
                              : std::vector<uint8_t>{};
        if (!rgba.empty() &&
            stbi_write_png(m_ScreenshotPath.c_str(), static_cast<int>(shot.width),
                           static_cast<int>(shot.height), 4, rgba.data(),
                           static_cast<int>(shot.width * 4u)) != 0)
        {
            Logger::Log::Info("Player: wrote screenshot '{}' ({}x{})", m_ScreenshotPath, shot.width, shot.height);
        }
        else
        {
            Logger::Log::Error("Player: screenshot write/readback failed (format {})",
                               static_cast<uint32_t>(shot.format));
        }
        m_ScreenshotTicket.reset();
        m_ScreenshotSpace.reset();
        m_ScreenshotWritten = true;
        RequestExit();
    }
}

#if GE_PLAYER_MOVIE_RECORDER
void PlayerApplication::DeclareMovieFrame(RuntimeFrameReadback& readback)
{
    // Movie capture must read back the post-process output after the game UI is
    // composited, but before the terminal swapchain encode. Keep the queue
    // bounded so a slow encoder cannot make the render loop allocate without
    // limit; PollMovieFrames drains tickets in capture order and the frame
    // limit is checked against scheduled and written frames.
    const RuntimeFrameReadback::Composite& composite = readback.GetComposite();
    const Rendering::RenderGraph::RGTexture presentSrc = composite.PresentSrc;
    Rendering::RenderGraph::RGFrame& frame = readback.GetFrame();
    if (!m_MovieOptions || !presentSrc.IsValid() || DelayMovieCaptureFrame() ||
        (m_MovieFrameLimit != 0 && m_MovieFramesScheduled >= m_MovieFrameLimit) ||
        m_MovieReadbacks.size() >= kMaxPendingMovieReadbacks ||
        !AcceptMovieSourceExtent(frame.Graph().ResourceDesc(presentSrc.Id)))
    {
        return;
    }

    // The capture target takes the SOURCE's extent, never the requested output
    // resolution. On the encoded arm presentSrc arrives already encoded and
    // dithered, and this pass samples it through a linear sampler: a differing
    // destination extent would resample the dither at a step sized to the
    // source and re-round the recovered detail undithered. Resizing belongs
    // outside the dithered domain, in an encoder-side scaler that does not
    // exist yet, so AcceptMovieSourceExtent refuses a differing request
    // rather than honouring it silently at the wrong resolution.
    const Rendering::RenderGraph::RGResourceDesc& movieSrcDesc = frame.Graph().ResourceDesc(presentSrc.Id);
    Rendering::TextureDesc movieDesc{};
    movieDesc.width = movieSrcDesc.Width;
    movieDesc.height = movieSrcDesc.Height;
    movieDesc.depth = 1;
    movieDesc.mipLevels = 1;
    movieDesc.arrayLayers = 1;
    movieDesc.sampleCount = 1;
    movieDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::BGRA8_UNORM);
    movieDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    movieDesc.debugName = "Player.MovieReadbackBGRA8";
    const auto movieDst = frame.CreateTexture("Player.MovieReadbackBGRA8", movieDesc);
    // The movie target is 8-bit whatever the swapchain runs at, and who owns
    // its quantization step is the shared decision in SRGBEncodePass.h — the
    // editor game view's recording takes the same one, which is what keeps
    // the two hosts' movies from diverging. Whenever this pass owns the step
    // it owns the deband at that step too.
    const auto movieQuantizer = Rendering::Passes::SelectTransferQuantizer(
        composite.PresentInput == Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
        composite.PresentedFormat, Rendering::TextureFormat::BGRA8_UNORM);
    const bool movieOwnsStep =
        movieQuantizer != Rendering::Passes::FinalizeQuantizer::None;
    if (!movieDst.IsValid() ||
        !Rendering::Passes::AddSRGBEncodePassRG(
             frame, presentSrc, movieDst,
             {.InputSpace = composite.PresentInput,
              .Quantizer = movieQuantizer,
              .VolumeDebandThresholdLsb = movieOwnsStep ? composite.DebandThresholdLsb : 0.0f,
              // Seeded on the ACCEPTED-capture count, never the render frame
              // index, so movie frame N carries realisation N in this host
              // and in the editor's game view alike (TemporalDither.h).
              .DitherPhase = Rendering::Passes::MovieDitherPhase(m_MovieFramesScheduled)},
             "Player.MovieSRGBEncode")
             .IsValid())
    {
        return;
    }
    if (auto ticket = Rendering::RequestTextureReadbackRG(&readback.GetDevice(), frame, movieDst,
                                                          "PlayerMovieFrame"))
    {
        // movieDst is the encode pass's output in both chains — an OETF
        // of the linear FinalColor, or a requantize of the already-
        // encoded composite: encoded bytes at rest either way.
        m_MovieReadbacks.push_back({std::move(ticket), UI::UITextureSpace::SrgbAuthored()});
        ++m_MovieFramesScheduled;
    }
}

Video::SubmitResult PlayerApplication::TrySubmitMovieFrame(Video::OwnedVideoFrame& frame)
{
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    if (!recorder)
        return Video::SubmitResult::Rejected;
    return recorder->SubmitVideoFrame(frame);
}

// A rejected submit is terminal; the recorder's failure text carries the cause
// and the fix (e.g. which encoder refused and why). Surfacing it is what makes
// the refusal actionable — "encoder stopped" alone is not.
std::string PlayerApplication::MovieEncoderFailureReason()
{
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    std::string lastError = recorder ? recorder->GetStats().LastError : std::string();
    if (lastError.empty())
        return "encoder stopped";
    // The writer speaks API-level; translate the hardware-encoder advice into
    // this host's own control.
    if (lastError.find("requireHardwareAcceleration") != std::string::npos)
        lastError += " (remove --require-hardware-encoder to use the software encoder)";
    return lastError;
}

void PlayerApplication::PollMovieFrames()
{
    if (!m_MovieOptions)
        return;

    // Resubmit a frame the encoder couldn't accept last poll before draining new
    // readbacks, so encode order matches capture order.
    if (m_PendingMovieEncodeFrame)
    {
        switch (TrySubmitMovieFrame(*m_PendingMovieEncodeFrame))
        {
        case Video::SubmitResult::Accepted:
            ++m_MovieFramesWritten;
            m_PendingMovieEncodeFrame.reset();
            break;
        case Video::SubmitResult::QueueFull:
            return; // still backed up; retry next poll
        case Video::SubmitResult::Rejected:
            Logger::Log::Error("Player: Movie capture ended: {}", MovieEncoderFailureReason());
            m_PendingMovieEncodeFrame.reset();
            m_MovieOptions.reset();
            return;
        }
    }

    if (m_MovieReadbacks.empty())
        return;

    for (auto it = m_MovieReadbacks.begin(); it != m_MovieReadbacks.end();)
    {
        if (!it->Ticket)
        {
            it = m_MovieReadbacks.erase(it);
            continue;
        }

        Rendering::ViewReadbackResult result;
        if (!it->Ticket->TryGet(result))
        {
            if (it->Ticket->IsConsumed())
            {
                // The readback died (declaring frame abandoned, or the device
                // was rebuilt): it can never resolve. Drop the frame instead of
                // wedging the in-order drain at the head forever.
                it = m_MovieReadbacks.erase(it);
                continue;
            }
            // GPU readbacks do not guarantee completion order. Preserve the
            // rendered frame order instead of encoding a later frame first.
            break;
        }

        if (!EnsureMovieRecorder(result.width, result.height))
        {
            it = m_MovieReadbacks.erase(it);
            continue;
        }

        // The declaration stamped SrgbAuthored (the encode pass's output), so the
        // converter only swizzles BGRA->RGBA — no second encode.
        std::vector<uint8_t> rgba8 = Rendering::ReadbackToRgba8Srgb(result, it->Space);
        if (rgba8.empty())
        {
            Logger::Log::Error("Player: Movie capture got unsupported readback format {}", static_cast<uint32_t>(result.format));
            m_MovieOptions.reset();
            it = m_MovieReadbacks.erase(it);
            // Return, not continue: the reset above ended the recording, and a
            // later readback resolving in this same drain would dereference the
            // null options below.
            return;
        }

        Video::OwnedVideoFrame frame{};
        frame.Pixels = std::move(rgba8);
        frame.Width = result.width;
        frame.Height = result.height;
        frame.StrideBytes = result.width * 4u;
        frame.Format = Video::VideoPixelFormat::RGBA8;

        if (m_MovieOptions->fadeOutEnabled)
            m_LastMovieFramePixels = frame.Pixels;
        else
            m_LastMovieFramePixels.clear();
        m_LastMovieFrameWidth = frame.Width;
        m_LastMovieFrameHeight = frame.Height;
        m_LastMovieFrameStrideBytes = frame.StrideBytes;
        m_LastMovieFrameFormat = frame.Format;

        switch (TrySubmitMovieFrame(frame))
        {
        case Video::SubmitResult::Accepted:
            ++m_MovieFramesWritten;
            it = m_MovieReadbacks.erase(it);
            break;
        case Video::SubmitResult::QueueFull:
            // Encoder is behind: hold this frame, stop draining, retry next poll.
            // Backpressure stalls scheduling rather than ending the recording.
            m_PendingMovieEncodeFrame = std::move(frame);
            it = m_MovieReadbacks.erase(it);
            return;
        case Video::SubmitResult::Rejected:
            Logger::Log::Error("Player: Movie capture ended at frame {}: {}", m_MovieFramesWritten,
                               MovieEncoderFailureReason());
            m_MovieOptions.reset();
            it = m_MovieReadbacks.erase(it);
            return;
        }
    }

    if (m_MovieFrameLimit > 0 &&
        m_MovieFramesScheduled >= m_MovieFrameLimit &&
        m_MovieFramesWritten >= m_MovieFrameLimit &&
        m_MovieReadbacks.empty())
    {
        RequestExit();
    }
}

bool PlayerApplication::AcceptMovieSourceExtent(const Rendering::RenderGraph::RGResourceDesc& sourceDesc)
{
    const bool widthMatches =
        m_MovieOptions->width == 0 || m_MovieOptions->width == sourceDesc.Width;
    const bool heightMatches =
        m_MovieOptions->height == 0 || m_MovieOptions->height == sourceDesc.Height;
    if (widthMatches && heightMatches)
        return true;

    // Refused rather than resampled: capture reads an already-dithered image, so
    // rescaling it here destroys the dither instead of resizing the movie. The
    // resize belongs in an encoder-side scaler, which this build does not have.
    Logger::Log::Error(
        "Player: Refusing to record '{}' at {}x{}: the frame is rendered at {}x{} and movie "
        "resize is not implemented. Resizing a captured frame here would resample an "
        "already-dithered image and band the result, so it must happen in the encoder, which "
        "has no scaler yet. Drop --movie-width/--movie-height to record at {}x{}, or size the "
        "window to the resolution you want",
        m_MovieOptions->path, m_MovieOptions->width, m_MovieOptions->height, sourceDesc.Width,
        sourceDesc.Height, sourceDesc.Width, sourceDesc.Height);
    m_MovieOptions.reset();
    RequestExit();
    return false;
}

bool PlayerApplication::EnsureMovieRecorder(uint32_t width, uint32_t height)
{
    if (!m_MovieOptions)
        return false;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        if (m_MovieRecorder)
        {
            m_MovieOptions->width = m_MovieOptions->width > 0 ? m_MovieOptions->width : width;
            m_MovieOptions->height = m_MovieOptions->height > 0 ? m_MovieOptions->height : height;
            return true;
        }
    }

    Video::VideoWriterOptions options = *m_MovieOptions;
    options.width = options.width > 0 ? options.width : (m_GameConfig.windowWidth > 0 ? m_GameConfig.windowWidth : width);
    options.height = options.height > 0 ? options.height : (m_GameConfig.windowHeight > 0 ? m_GameConfig.windowHeight : height);
    if (options.codec == Video::VideoCodec::Auto)
        options.codec = Video::GuessCodecForMoviePath(options.path);

    if (!Video::VideoWriter::IsCodecSupported(options.codec))
    {
        Logger::Log::Error("Player: Movie codec '{}' is not supported by this platform/build", Video::ToString(options.codec));
        m_MovieOptions.reset();
        return false;
    }

    auto recorder = std::make_shared<Video::AsyncVideoRecorder>();
    std::string error;
    if (!recorder->Start(options, {}, &error))
    {
        Logger::Log::Error("Player: Failed to open movie recorder '{}': {}", options.path, error);
        m_MovieOptions.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        m_MovieRecorder = recorder;
    }

    *m_MovieOptions = options;
    const bool bitrateCodec = options.codec == Video::VideoCodec::H264 || options.codec == Video::VideoCodec::HEVC;
    const std::string bitrateText = bitrateCodec
                                        ? (options.bitrateKbps > 0 ? std::to_string(options.bitrateKbps) + " Kbps" : "encoder-default")
                                        : "profile-driven";
    Logger::Log::Info(
        "Player: Writing movie '{}' ({}x{} @ {} fps, codec={}, bitrate={}, audio={}, hardware={})",
        options.path,
        options.width,
        options.height,
        options.fps,
        Video::ToString(options.codec),
        bitrateText,
        options.recordAudio ? "on" : "off",
        options.requireHardwareAcceleration ? "required" : "allowed-software-fallback");
    return true;
}

bool PlayerApplication::DelayMovieCaptureFrame()
{
    if (!m_MovieOptions || m_MovieWarmupFramesRemaining == 0)
        return false;
    --m_MovieWarmupFramesRemaining;
    return true;
}

void PlayerApplication::StartMovieAudioCapture()
{
    if (m_MovieAudioCaptureArmed || !m_MovieOptions || !m_MovieOptions->recordAudio)
        return;

    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
    {
        audio->SetOutputCaptureCallback([this](const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate) {
            AppendMovieAudio(samples, frameCount, channels, sampleRate);
        });
        m_MovieAudioCaptureArmed = true;
    }
}

void PlayerApplication::StopMovieAudioCapture()
{
    if (!m_MovieAudioCaptureArmed)
        return;
    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
        audio->SetOutputCaptureCallback(nullptr);
    m_MovieAudioCaptureArmed = false;
}

void PlayerApplication::AppendMovieAudio(const float* samples, uint32_t frameCount, uint32_t channels, uint32_t sampleRate)
{
    if (!samples || frameCount == 0 || channels == 0 || sampleRate == 0)
        return;
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        if (m_MovieAudioChannels == 0 || m_MovieAudioSampleRate == 0)
        {
            m_MovieAudioChannels = channels;
            m_MovieAudioSampleRate = sampleRate;
        }
        if (channels != m_MovieAudioChannels || sampleRate != m_MovieAudioSampleRate)
            return;
        recorder = m_MovieRecorder;
    }

    const size_t sampleCount = static_cast<size_t>(frameCount) * channels;
    Video::OwnedAudioSamples audio{};
    audio.Samples.assign(samples, samples + sampleCount);
    audio.FrameCount = frameCount;
    audio.Channels = channels;
    audio.SampleRate = sampleRate;
    if (recorder)
    {
        EnqueueMovieAudio(recorder, std::move(audio));
    }
}

void PlayerApplication::EnqueueMovieAudio(const std::shared_ptr<Video::AsyncVideoRecorder>& recorder,
                                          Video::OwnedAudioSamples&& audio)
{
    const auto chunkBytes = [](const Video::OwnedAudioSamples& a) { return a.Samples.size() * sizeof(float); };

    std::lock_guard<std::mutex> lock(m_MovieAudioBufferMutex);

    // Drain anything held from earlier backpressure first, in capture order.
    while (!m_PendingMovieAudio.empty())
    {
        const Video::SubmitResult result = recorder->SubmitAudioSamples(m_PendingMovieAudio.front());
        if (result == Video::SubmitResult::QueueFull)
        {
            break; // still backed up; keep the rest buffered for the next callback
        }
        // Accepted (consumed) or Rejected (recorder stopped) — either way it leaves.
        m_PendingMovieAudioBytes -= std::min(m_PendingMovieAudioBytes, chunkBytes(m_PendingMovieAudio.front()));
        m_PendingMovieAudio.pop_front();
    }

    // Submit the new chunk directly only if nothing is buffered ahead of it; otherwise
    // buffer it to preserve order (audio PTS is monotonic, so reordering corrupts sync).
    if (m_PendingMovieAudio.empty())
    {
        if (recorder->SubmitAudioSamples(audio) != Video::SubmitResult::QueueFull)
        {
            return;
        }
    }
    m_PendingMovieAudioBytes += chunkBytes(audio);
    m_PendingMovieAudio.push_back(std::move(audio));

    // Bound the buffer so a wedged encoder can't grow it without limit. Dropping the
    // oldest is unavoidable audio loss, but only after a multi-second encoder stall.
    while (m_PendingMovieAudioBytes > kMaxPendingMovieAudioBytes && !m_PendingMovieAudio.empty())
    {
        m_PendingMovieAudioBytes -= std::min(m_PendingMovieAudioBytes, chunkBytes(m_PendingMovieAudio.front()));
        m_PendingMovieAudio.pop_front();
    }
}

void PlayerApplication::ClearPendingMovieAudio()
{
    std::lock_guard<std::mutex> lock(m_MovieAudioBufferMutex);
    m_PendingMovieAudio.clear();
    m_PendingMovieAudioBytes = 0;
}

void PlayerApplication::FinishMovieCapture()
{
    PollMovieFrames();
    std::shared_ptr<Video::AsyncVideoRecorder> recorder;
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        recorder = m_MovieRecorder;
    }
    if (recorder)
    {
        Video::OwnedVideoFrame frame{};
        frame.Pixels = std::move(m_LastMovieFramePixels);
        frame.Width = m_LastMovieFrameWidth;
        frame.Height = m_LastMovieFrameHeight;
        frame.StrideBytes = m_LastMovieFrameStrideBytes;
        frame.Format = m_LastMovieFrameFormat;
        const std::string path = m_MovieOptions ? m_MovieOptions->path : std::string{};
        const uint64_t framesWritten = m_MovieFramesWritten;
        recorder->Finish(
            std::move(frame),
            "Recording finished.",
            [path, framesWritten](bool ok, const std::string& message) {
                if (ok)
                    Logger::Log::Info("Player: Finished movie capture '{}' ({} frames)", path, framesWritten);
                else
                    Logger::Log::Error("Player: Failed to finish movie '{}': {}", path, message);
            },
            true);
    }
    m_MovieReadbacks.clear();
    m_PendingMovieEncodeFrame.reset();
    ClearPendingMovieAudio();
    {
        std::lock_guard<std::mutex> lock(m_MovieRecorderMutex);
        m_MovieRecorder.reset();
        m_MovieAudioChannels = 0;
        m_MovieAudioSampleRate = 0;
    }
    m_LastMovieFramePixels.clear();
    m_LastMovieFrameWidth = 0;
    m_LastMovieFrameHeight = 0;
    m_LastMovieFrameStrideBytes = 0;
}
#endif // GE_PLAYER_MOVIE_RECORDER

void PlayerApplication::OnShutdown()
{
#if GE_PLAYER_MOVIE_RECORDER
    StopMovieAudioCapture();
    // Idle first, so every frame the movie declared has finished and its capture
    // resolves in the drain, the last rendered frame included.
    if (auto* renderCtx = m_Host->GetRenderDeviceContext())
    {
        if (auto* device = renderCtx->GetDevice())
            device->WaitForIdle();
    }
    FinishMovieCapture();
#endif
    // A pending screenshot readback releases against the device, which the host
    // shutdown frees.
    m_ScreenshotTicket.reset();
    m_ScreenshotSpace.reset();
    m_Host->Shutdown();
}

} // namespace GameEngine
