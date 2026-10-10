#include "Engine/Hosting/RuntimeHost.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECSModules/Rendering/Systems/RenderGraphBuildSystem.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/Hosting/HudPointer.h"
#include "Engine/Hosting/RuntimeFrameReadback.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAntiAliasing.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/CameraExposureHelpers.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Engine/Rendering/ViewFinalize.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Engine/Rendering/VkValidationRequest.h"
#include "Logger/Logger.h"
#include "Platform/Window.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/HzbCullingStrategy.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"
#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneIO.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>

namespace GameEngine {

namespace
{
class RuntimeSceneAssetResolver final : public Scene::ISceneAssetResolver
{
public:
    explicit RuntimeSceneAssetResolver(AssetManager& assetManager, std::filesystem::path assetRoot)
        : m_AssetManager(assetManager)
        , m_Registry(assetManager.GetRegistry())
        , m_AssetRoot(std::move(assetRoot))
    {
    }

    std::filesystem::path GetAssetRoot() const override { return m_AssetRoot; }
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const override
    {
        return m_AssetManager.ResolveAssetPath(authoredPath);
    }
    GUID ResolveGuid(const GUID& guid) const override { return m_Registry.ResolveGuid(guid); }
    GUID GetOrCreateAssetGuid(const std::filesystem::path& absolutePath) override
    {
        return m_Registry.GetOrCreateAssetGUID(absolutePath);
    }

    bool TryGetPathAndType(const GUID& guid, std::filesystem::path& outPath, AssetType& outType) const override
    {
        AssetMetadata metadata{};
        if (!m_Registry.TryGetAssetMetadata(guid, metadata))
            return false;
        outPath = metadata.Path;
        outType = metadata.Type;
        return true;
    }

    bool TryGetGuidAndType(const std::filesystem::path& absolutePath, GUID& outGuid, AssetType& outType) const override
    {
        AssetMetadata metadata{};
        if (!m_Registry.TryGetAssetMetadata(absolutePath, metadata))
            return false;
        outGuid = metadata.Guid;
        outType = metadata.Type;
        return true;
    }

private:
    AssetManager& m_AssetManager;
    AssetRegistry& m_Registry;
    std::filesystem::path m_AssetRoot;
};

// Applies a fullscreen window mode; the windowed mode is the window as created.
void ApplyFullscreenMode(Platform::Window& window, WindowMode mode)
{
    switch (mode)
    {
    case WindowMode::Windowed:
        break;

    case WindowMode::BorderlessFullscreen:
    {
        // Borderless fullscreen: windowed mode, no decorations, sized to monitor resolution.
        // This avoids exclusive fullscreen mode change (faster alt-tab, no resolution switch).
        if (window.IsFullscreen())
            window.SetFullscreen(false);
        window.SetDecorated(false);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* videoMode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (videoMode)
        {
            window.SetPosition(0, 0);
            window.SetWindowSize(videoMode->width, videoMode->height);
        }
        break;
    }

    case WindowMode::ExclusiveFullscreen:
        if (!window.IsFullscreen())
            window.SetFullscreen(true);
        break;
    }
}
} // namespace

using Rendering::TextureFormat;

RuntimeHost::RuntimeHost(Application& application, RuntimeHostDesc desc, RuntimeHostHooks hooks)
    : m_Application(application)
    , m_Desc(std::move(desc))
    , m_Hooks(std::move(hooks))
    , m_RenderGraphDiagnostics(std::getenv("GE_RG_DIAG") && std::getenv("GE_RG_DIAG")[0] == '1')
{
}

RuntimeHost::~RuntimeHost() = default;

bool RuntimeHost::InitWindow()
{
    // GLFW init lifecycle (including the macOS COCOA_CHDIR_RESOURCES /
    // COCOA_MENUBAR hints) is handled inside Platform::Window. See
    // Engine/Modules/Platform/Source/Window.cpp.
    m_Window = std::make_unique<Platform::Window>();
    Platform::WindowDesc desc{};
    desc.Title = m_Desc.Title;
    desc.Width = static_cast<int>(m_Desc.WindowWidth);
    desc.Height = static_cast<int>(m_Desc.WindowHeight);

    if (!m_Window->Create(desc))
    {
        Logger::Log::Error("Runtime: Failed to create window");
        return false;
    }

    // The game's InputSystem is the last stage of the chain here: with no chrome
    // layer above it (the host reserves that slot and binds nothing), whatever
    // the HUD declines is gameplay's. Both legs are queried lazily per event.
    //
    // A gamepad does not enter this config: Application::GamepadInputChain
    // answers with the application chain, whose only stage is the same
    // InputSystem, and nothing above it has a gamepad entry to offer a pad to.
    // The day the HUD gains one, the owning application overrides
    // GamepadInputChain with this config, or its pad passes the HUD unseen.
    WindowInputRouterConfig inputCfg{};
    inputCfg.window = m_Window.get();
    inputCfg.getInput = [this]() { return m_Application.GetInputSystem(); };
    inputCfg.getPlaySurface = [this]() { return HudPlaySurface(); };
    WindowInputRouter::BindBasicHandlers(inputCfg);
    m_Window->SetRefreshHandler([this] { m_Application.Tick(); });

    ApplyFullscreenMode(*m_Window, m_Desc.Mode);

    return true;
}

WindowInputRouterConfig::PlaySurface RuntimeHost::HudPlaySurface() const
{
    WindowInputRouterConfig::PlaySurface surface;
    if (!m_GameUI)
        return surface;
    surface.gameUi = m_GameUI.get();
    // This window IS the surface, so its pointer is the HUD's: binding the
    // mapping is what puts the HUD on the move, button and wheel chains, and the
    // mapping itself is the client-to-framebuffer scale the HUD lays out at.
    surface.mapGameUiPointer = [this](float clientX, float clientY, float& hudX, float& hudY)
    {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        if (m_Window)
            m_Window->GetContentScale(scaleX, scaleY);
        MapClientToHudPixels(clientX, clientY, scaleX, scaleY, hudX, hudY);
    };
    return surface;
}

void RuntimeHost::RecreateSwapchain(int width, int height)
{
    // A minimized window reports zero, which no swapchain can take.
    if (m_RenderCtx && width > 0 && height > 0)
        m_RenderCtx->RecreateWindowTargetSwapchain(static_cast<uint32>(width), static_cast<uint32>(height));
}

void RuntimeHost::FollowFramebufferSize(int width, int height)
{
    // A callback at the size the swapchain already has (a menu or toolbar update, a restore
    // from minimize) would only drain the queues and rebuild for nothing, and can flicker.
    Rendering::IDevice* device = m_RenderCtx ? m_RenderCtx->GetDevice() : nullptr;
    uint32_t currentWidth = 0, currentHeight = 0;
    if (device && device->GetSwapchainSize(currentWidth, currentHeight) &&
        currentWidth == static_cast<uint32_t>(width) && currentHeight == static_cast<uint32_t>(height))
        return;
    RecreateSwapchain(width, height);
}

bool RuntimeHost::InitRendering()
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Auto;
    desc.enableSwapchain = true;
    desc.applicationName = m_Desc.Title;
    desc.vsync = m_Desc.VSync;
    desc.hdrEnabled = m_Desc.HdrEnabled;
    desc.hdrMode = m_Desc.HdrMode;
    desc.hdrSwapchainBitDepth = m_Desc.HdrBitDepth;
    desc.hdrTargetDisplay = m_Desc.HdrTargetDisplay;
    desc.hdrStaticMetadata = m_Desc.HdrMetadata;
    if (Engine::Renderer::ShouldEnableVkValidation())
    {
        desc.enableDebugLayer = true;
    }
    m_RenderCtx = std::make_unique<Engine::Renderer::RenderDeviceContext>();
    Engine::Renderer::RenderDeviceContext::InitParams params{};
    params.deviceDesc = desc;
    params.windowHandle = m_Window->GetNativeHandle();

    int fbW = 0, fbH = 0;
    m_Window->GetFramebufferSize(fbW, fbH);
    params.width = static_cast<uint32>(fbW > 0 ? fbW : m_Desc.WindowWidth);
    params.height = static_cast<uint32>(fbH > 0 ? fbH : m_Desc.WindowHeight);
    params.createRenderServices = true;

    if (!m_RenderCtx->Initialize(params))
    {
        Logger::Log::Error("Runtime: Failed to initialize RenderDeviceContext");
        return false;
    }
    // A new monitor can change the swapchain's format and color space at the same size.
    m_Window->SetMonitorChangedHandler([this](int) {
        int width = 0, height = 0;
        m_Window->GetFramebufferSize(width, height);
        RecreateSwapchain(width, height);
    });
    // The swapchain follows the framebuffer. A Vulkan surface may report the resize at acquire
    // or at present, or not until later, and a browser canvas never does; until then the host
    // would render at the old size into the resized window.
    m_Window->SetFramebufferSizeHandler([this](int width, int height) { FollowFramebufferSize(width, height); });

    auto* rs = m_RenderCtx->GetRenderServices();
    if (!rs)
    {
        Logger::Log::Error("Runtime: RenderServices was not created");
        return false;
    }

    // Enable the engine-managed rendering loop (ECS extraction, culling, draw list build).
    auto& engine = EngineCore::GetInstance();
    if (!engine.EnableRenderingLoop(rs))
    {
        Logger::Log::Error("Runtime: Failed to enable engine-managed rendering loop");
        return false;
    }
    engine.SetRenderingLoopAutoDrive(true);

    // The host owns BeginFrame / BuildFrameGraph / blit / Present in Render().
    // Keep ECS housekeeping (extraction, batch keys, buffer finalize) in the loop
    // but skip the system's own BuildFrameGraph so we don't double-schedule passes.
    if (auto* loop = engine.GetRenderingLoop())
    {
        if (auto* sm = loop->GetSystemManager())
        {
            if (auto* rgBuild = sm->GetSystem<Engine::Renderer::RenderGraphBuildSystem>())
                rgBuild->SetSkipFrameGraphBuild(true);
        }
    }

    // The project's authored LOD selection settings, cooked into game.config
    // (a runtime has no project settings file to read). Applied before any
    // mesh registers, so every row derives under the shipped mode rather than
    // being re-derived by a later switch. The editor performs the same total
    // apply on project open.
    m_Desc.LodSelection.ApplyTo(*rs);
    // Read the renderer back rather than echoing the config: echoing would
    // print the same line if the apply were a no-op.
    Logger::Log::Info(
        "Runtime: Applied LOD selection settings (mode={}, errorBudgetPx={}, skinnedBudgetScale={})",
        Rendering::ToString(rs->GetMeshGPURegistry().GetLodSelectionMode()),
        rs->GetLODErrorBudgetPx(), rs->GetLODSkinnedBudgetScale());

    // Engine-wide AA + render scale, cooked into game.config the same way.
    // Cameras with mode Default inherit these; per-camera overrides still win.
    m_Desc.RenderQuality.ApplyTo(*rs);
    Logger::Log::Info(
        "Runtime: Applied render quality settings (aaMode={}, msaa={} sample(s), renderScale={})",
        static_cast<uint32_t>(rs->GetDefaultAntiAliasingMode()), rs->GetDefaultMSAASampleCount(),
        rs->GetDefaultRenderScale());

    return true;
}

Engine::Renderer::PipelineResolveFailure RuntimeHost::LoadRenderPipeline()
{
    auto* rs = m_RenderCtx->GetRenderServices();
    if (!rs)
        return Engine::Renderer::PipelineResolveFailure::LoadFailed;

    // Without the key the host renders with the engine's default pipeline,
    // the one the editor renders with.
    const std::filesystem::path requested = !m_Desc.RenderPipeline.empty()
                                                ? std::filesystem::path(m_Desc.RenderPipeline)
                                                : rs->Spine().GetActiveRenderPipelinePath();
    const Engine::Renderer::PipelineResolveResult result = rs->Spine().ResolveActiveRenderPipelineNow(requested);
    if (result.Failure != Engine::Renderer::PipelineResolveFailure::None)
    {
        Logger::Log::Error("Runtime: cannot start: the {}.", result.Message);
        return result.Failure;
    }
    Logger::Log::Info("Runtime: render pipeline '{}' applied", requested.generic_string());
    return Engine::Renderer::PipelineResolveFailure::None;
}

bool RuntimeHost::OpenScene(const std::filesystem::path& scenePath)
{
    auto& engine = EngineCore::GetInstance();
    auto* world = engine.EnsurePrimaryWorld();
    if (!world)
    {
        Logger::Log::Error("Runtime: Failed to create primary ECS world");
        return false;
    }

    const auto& assetRoot = engine.GetResolvedAssetRoot();
    const auto sceneFile = assetRoot / scenePath;

    RuntimeSceneAssetResolver assetResolver(engine.GetAssetManager(), assetRoot);

    Scene::LoadOptions opts{};
    opts.mode = Scene::LoadMode::Replace;
    opts.assetResolver = &assetResolver;
    opts.assetRootOverride = assetRoot;
    if (!Scene::LoadSceneFromFile(*world, sceneFile, opts))
    {
        // The loader reports through its error record and does not log the
        // reason itself; this is the one line that carries it.
        const auto& err = Scene::GetLastSceneIOError();
        Logger::Log::Error("Runtime: failed to load scene '{}' at {}:{}: {}", sceneFile.string(),
                           err.file.string(), err.line, err.message);
        return false;
    }

    // The scene's models and materials resolve through the engine's resolve
    // service: their loads start now, and each entity binds on the frame its model
    // has landed, so no frame waits for a load. Skeletons bind as entities complete.
    m_SceneResolve = Engine::Renderer::SceneResolveBatch::None;
    auto* rs = m_RenderCtx ? m_RenderCtx->GetRenderServices() : nullptr;
    if (auto* loop = engine.GetRenderingLoop(); rs && loop)
        m_SceneResolve = loop->GetSceneResolveService().EnqueueWorld(*world, *rs);

    Logger::Log::Info("Runtime: Loaded scene '{}'", sceneFile.string());

    // If the scene has no camera, create a default one so the host can render.
    auto existingCam = Engine::Renderer::FindActiveCamera(*world);
    if (!existingCam)
    {
        // Projection settings come from Components::Camera's own defaults so the
        // fallback cannot drift from them; only the placement is chosen here.
        Components::Camera cam{};

        Components::Transform xf{};
        // Default camera at (0, 2, -5) looking forward (+Z)
        xf.matrix[12] = 0.0f;
        xf.matrix[13] = 2.0f;
        xf.matrix[14] = -5.0f;

        world->Create(cam, xf);
        Logger::Log::Info("Runtime: Created default camera (no camera found in scene)");
    }

    return true;
}

void RuntimeHost::Render(float deltaTime)
{
    if (!m_Window || !m_RenderCtx)
        return;
    auto* rs = m_RenderCtx->GetRenderServices();
    auto* device = m_RenderCtx->GetDevice();
    if (!rs || !device)
        return;

    // Drive the device-loss rebuild retry every tick (the editor does this in its
    // render loop; the host must too, or the retry starves). On the terminal Failed
    // state the frame is not rendered and the owner's OnDeviceFailed decides what
    // the session does. On this driver a real TDR is unrecoverable in-process — a
    // relaunched process gets a fresh, working device.
    device->TickDeviceRecovery();
    if (device->GetDeviceHealth() == Rendering::DeviceHealth::Failed)
    {
        if (m_Hooks.OnDeviceFailed)
            m_Hooks.OnDeviceFailed();
        return;
    }

    auto& engine = EngineCore::GetInstance();
    auto* world = engine.GetPrimaryWorld();

    // Activate the window's swapchain target and begin the frame.
    if (!m_RenderCtx->ActivateWindowTarget())
        return;
    if (!device->BeginFrame())
        return;

    int fbW = 0, fbH = 0;
    m_Window->GetFramebufferSize(fbW, fbH);
    const uint32_t w = fbW > 0 ? static_cast<uint32_t>(fbW) : 1u;
    const uint32_t h = fbH > 0 ? static_cast<uint32_t>(fbH) : 1u;

    // Find the active ECS camera and set up the view BEFORE allocating render
    // targets: a smooth pixel-perfect camera renders into an offscreen RT sized
    // to its (reference + 2-texel border) resolution rather than the window, and
    // a dedicated upscale pass blits that to the swapchain. ApplyActiveCameraAspect
    // publishes that per-view state, which we read back to size the targets.
    if (world)
    {
        auto activeCam = Engine::Renderer::FindActiveCamera(*world);

        if (activeCam)
        {
            if (m_CameraId == 0)
                m_CameraId = rs->Views().AllocateCamera("RuntimeHost.Camera");
            if (m_ViewId == 0)
                m_ViewId = rs->Views().AllocateView("RuntimeHost View", m_CameraId);
            else
                rs->Views().SetViewCamera(m_ViewId, m_CameraId);

            if (m_ViewId != 0)
            {
                rs->Views().SetViewRenderLayerMask(m_ViewId, 1u);
                rs->Views().SetViewWorldId(m_ViewId, world->GetWorldId());
                // Two-phase HZB occlusion for the shipped player's main view,
                // matching the editor game view + free scene view (#253):
                // default-on, GE_HZB_OCCLUSION=0 reverts to frustum-only.
                rs->Views().SetViewCullingStrategy(
                    m_ViewId, Rendering::MakeDefaultOcclusionStrategyOrNull());
            }

            // Per-camera AA policy — the same one the editor's Game View
            // applies (Engine/Rendering/CameraAntiAliasing.h owns it).
            const Engine::Renderer::CameraAspectResolution aspectResolution =
                Engine::Renderer::ResolveCameraAspect(activeCam->params, w, h);
            m_ViewSampleCount =
                Engine::Renderer::ApplyCameraAntiAliasing(*rs, m_ViewId, *activeCam,
                                                          aspectResolution)
                    .SampleCount;

            Engine::Renderer::ApplyActiveCameraAspect(*rs, m_ViewId, m_CameraId, *activeCam, w, h);
            rs->Views().SetCameraPostProcessMask(m_CameraId, activeCam->params.PostProcessMask);
            rs->Views().SetCameraExposure(m_CameraId, Engine::Renderer::ToCameraExposure(activeCam->params));
            // Per-camera render scale: an explicit value pins this view to a
            // fixed scale (opting out of the engine-wide dynamic controller);
            // 0 inherits the engine default, Dynamic included.
            rs->Views().SetViewRenderScale(
                m_ViewId, activeCam->params.RenderScale > 0.0f
                              ? std::optional<float>(activeCam->params.RenderScale)
                              : std::nullopt);
        }
        else
        {
            Logger::Log::Warning("Runtime: no enabled Camera in the scene — nothing will be drawn");
        }
    }

    // Letterboxed cameras clear to black so the bars read as bars, not as a
    // dim scene background. Shared by both arms.
    bool useBlackClear = false;
    if (world)
    {
        if (auto activeCam = Engine::Renderer::FindActiveCamera(*world))
            useBlackClear =
                Engine::Renderer::ResolveCameraAspect(activeCam->params, w, h).letterbox.active;
    }

    // RenderGraph immediate-mode frame driver. The host declares no
    // pixel-perfect upscale — only the editor Game View does. A port that adds
    // one must state its source's space to AddPixelPerfectUpscalePassRG: on the
    // SDR HUD chain below that source holds sRGB-encoded bytes, not linear.
    if (m_ViewId != 0)
        RenderFrameRG(*rs, *device, w, h, useBlackClear, deltaTime);

    // Present MUST follow a successful BeginFrame to return the swapchain image.
    device->Present();
}

void RuntimeHost::RenderFrameRG(Engine::Renderer::RenderServices& rs,
                                Rendering::IDevice& device, uint32_t w, uint32_t h,
                                bool useBlackClear, float deltaTime)
{
    using namespace Rendering;

    if (!m_RenderGraphStream.Frame)
        m_RenderGraphStream = RenderGraph::MakeWindowRGFrame(device);
    // Contract: AFTER IDevice::BeginFrame (the profiling resolve reads the
    // slot BeginFrame just fence-waited).
    m_RenderGraphStream.Frame->BeginFrame(m_RGFrameIndex++);

    // Caller view targets: pool-backed, window-sized. HDR-float color because the
    // ForwardPlus post chain runs in RGBA16F intermediates; the pipeline's
    // SceneColor redirect owns the resolve, so no caller resolve target is
    // passed. A window resize is just a new desc: the pool reallocs and the old
    // physical defer-destroys.
    // The PER-VIEW resolved sample count (camera override folded against the
    // engine default) — not the raw engine default, which a camera's explicit
    // per-camera MSAA choice can differ from.
    const uint32_t msaaSamples = m_ViewSampleCount;
    TextureDesc colorDesc{};
    colorDesc.width = w;
    colorDesc.height = h;
    colorDesc.depth = 1;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = msaaSamples;
    colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    colorDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(TextureUsage::ShaderResource);
    colorDesc.debugName = "RuntimeHost.Color";
    TextureDesc depthDesc{};
    depthDesc.width = w;
    depthDesc.height = h;
    depthDesc.depth = 1;
    depthDesc.mipLevels = 1;
    depthDesc.arrayLayers = 1;
    depthDesc.sampleCount = msaaSamples;
    depthDesc.format =
        static_cast<uint32_t>(device.GetCapabilities().preferredDepthAndStencilFormat);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil) |
                      static_cast<uint32_t>(TextureUsage::ShaderResource);
    depthDesc.debugName = "RuntimeHost.Depth";
    const RenderGraph::RGTexture color = m_RenderGraphStream.Frame->ImportPersistentTexture("RuntimeHost.Color", colorDesc);
    const RenderGraph::RGTexture depth = m_RenderGraphStream.Frame->ImportPersistentTexture("RuntimeHost.Depth", depthDesc);

    // MSAA: the caller provides the single-sample resolve, so EVERY blueprint —
    // including one with no SceneColor redirect — hands FinalColor to the
    // terminal encode as a sampleable 1-sample texture. Under ForwardPlus the
    // pre-pass prefers the pipeline's SceneColor and this import simply goes
    // unused (the pool ages it out).
    RenderGraph::RGTexture resolve{};
    if (msaaSamples > 1)
    {
        TextureDesc resolveDesc = colorDesc;
        resolveDesc.sampleCount = 1;
        resolveDesc.debugName = "RuntimeHost.Resolve";
        resolve = m_RenderGraphStream.Frame->ImportPersistentTexture("RuntimeHost.Resolve", resolveDesc);
    }

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    if (useBlackClear)
    {
        clear.clearColorValue[0] = 0.0f;
        clear.clearColorValue[1] = 0.0f;
        clear.clearColorValue[2] = 0.0f;
        clear.clearColorValue[3] = 1.0f;
    }
    else
    {
        clear.clearColorValue[0] = 0.02f;
        clear.clearColorValue[1] = 0.02f;
        clear.clearColorValue[2] = 0.02f;
        clear.clearColorValue[3] = 1.0f;
    }
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs.Views().SetViewClearConfig(m_ViewId, clear);

    // Forward+ directional/ambient lighting reads the per-view LightUBO —
    // populate it before the spine declares the passes that read it.
    rs.WriteViewLightBuffer(m_ViewId);

    const RenderGraph::RGTexture backbuffer = m_RenderGraphStream.Frame->ImportBackbuffer();

    // Game UI: reconcile the UIDocument set BEFORE the spine declares. Two of this
    // frame's decisions read the result — whether a ready Fullscreen menu already owns
    // the target (below) and whether the frame's blend-space topology needs the HUD
    // chain (#767 slice iii) — and both have to be known before any pass exists.
    if (!m_GameUI)
    {
        m_GameUI = GameUIHost::CreateForHost(
            &device, &EngineCore::GetInstance().GetAssetManager(),
            &EngineCore::GetInstance().GetJobSystem(),
            // Default HUD font from the project asset root (Assets/Fonts/Roboto-Regular.ttf),
            // staged beside a development runtime and shipped with every export. The host
            // owns its FinalColor target.
            EngineCore::GetInstance().GetResolvedAssetRoot(), /*neverClearTarget=*/false);
        m_GameUI->GetUIManager()->SetScaleSettings(m_Desc.UIScale);
    }
    GameUI::SetHost(m_GameUI.get());
    const bool hasHudDocuments =
        m_GameUI->SyncDocuments(EngineCore::GetInstance().GetPrimaryWorld());
    const bool sceneCoveredByUI = m_GameUI->CoversTargetOpaque();

    // A ready Fullscreen menu clears the whole target, so every per-view pass behind it
    // draws pixels nothing will ever see. Keeping the view out of the spine's
    // ViewTargets span drops all of them — the pipeline declares per-view passes from
    // the caller's targets, so with none it declares none — and the composite takes the
    // raw view target in place of a pipeline output. No clear pass replaces the scene:
    // the menu composites with RGLoadOp::Clear (the same CoversTargetOpaque verdict
    // drives it), so the UI's own clear IS the background.
    //
    // The GPU-driven prologue (skinning, view culling, the bucketer) is NOT skipped:
    // the spine schedules it from the view registry, not from this span. Dropping the
    // per-view scene and post work is the win here; deactivating the view itself is a
    // larger change with its own history and is not part of it.
    //
    // The editor Game View deliberately does NOT do this: its viewport is a WYSIWYG
    // preview of the scene being authored, and a menu-covered frame there must still
    // show what is behind the menu when the user hides or moves it.
    const Engine::Renderer::Pipeline::ViewTargetsRG vt{m_ViewId, color, depth, resolve};
    Engine::Renderer::RenderServices::FrameGraphBuildParamsRG params{};
    if (!sceneCoveredByUI)
        params.ViewTargets = std::span<const Engine::Renderer::Pipeline::ViewTargetsRG>(&vt, 1);
    params.BuildWorldDrawListsIfReady = false; // the ECS loop builds keys during Update
    params.DeltaTime = deltaTime;
    rs.Spine().BuildFrameGraph(*m_RenderGraphStream.Frame, params);

    // Covered: the single-sample view target the menu owns (the resolve when MSAA is
    // on, else the colour target) — the pipeline produced no output for a view it
    // never rendered, and must not be asked to fake one. The finalize below still runs
    // on it: it encodes whatever the target held last, the menu's Clear then overwrites
    // every pixel of that, and in exchange a covered frame reaches the swapchain
    // through the exact same encoded-blend chain — and the same dither and deband — as
    // every other SDR frame.
    Engine::Renderer::RenderServices::PipelineOutputRG out{};
    if (sceneCoveredByUI)
    {
        out.Out = resolve.IsValid() ? resolve : color;
        out.Physical = m_RenderGraphStream.Frame->PhysicalTexture(out.Out);
    }
    else
    {
        out = rs.GetPipelineOutputRG(*m_RenderGraphStream.Frame, m_ViewId);
    }

    // Volume lever for the deband, resolved before the finalize declares: the
    // game world's resolved post settings own the gate and GE_DEBAND env
    // overrides are applied inside the pass. Shared by the screen chain and the
    // composite hook's consumers so recordings keep matching the display.
    const float debandLsb = Engine::Renderer::ResolveViewDebandThresholdLsb(&rs);
    // The swapchain format this frame presents into, resolved from the host's
    // own window target once per frame — never cached (a swapchain recreate
    // between frames would leave a cached value sizing to a dead surface).
    // Shared by the finalize and the composite hook, so a consumer's
    // requantize decision and the screen's step are one dataflow.
    const Rendering::TextureFormat presentedFormat =
        device.GetWindowTargetSwapchainFormat(m_RenderCtx->GetWindowTarget());

    // What the terminal pass and the composite hook's readbacks consume, and the
    // space it holds.
    //
    // Linear chain (no HUD, or any active HDR output mode): the pipeline's
    // FinalColor, with the terminal pass owning the single OETF and the single
    // quantization step for every output config.
    //
    // SDR HUD chain: the world FINALIZES FIRST — OETF, deband and TPDF dither
    // at the swapchain's real step — and the HUD then blends on top of encoded
    // values. No dither reaches the HUD, so chrome lands on the screen through
    // a single ROP rounding; the terminal pass degenerates to a transfer that
    // applies neither a transfer function nor a filter, storing raw into a
    // UNORM swapchain and D(c) into an _SRGB one so its ROP round-trips
    // (FinalizeContract.h).
    //
    // The editor's game view runs this same order: its world finalizes at its
    // own resolve and its HUD composites onto the encoded bytes, so a runtime HUD
    // scrim and an editor game-view HUD scrim over the same sky blend against
    // the same kind of dst value (#767 slice iv).
    RenderGraph::RGTexture presentSrc = out.Out;
    auto finalizeInput = Rendering::Passes::FinalizeInputSpace::Linear;

    // Game UI: composite UIDocument entities onto the frame BEFORE the terminal
    // pass and the composite hook — the runtime HUD.
    if (out.Out.IsValid())
    {
        const auto& uiDesc = m_RenderGraphStream.Frame->Graph().ResourceDesc(out.Out.Id);
        {
            // The HUD's declared blend space. SDR frames declare EncodedSrgb:
            // the world is FINALIZED first — sRGB OETF, deband, and a TPDF
            // dither sized to the SWAPCHAIN's step, not this composite's — and
            // the HUD then blends on those encoded values. That is the browser
            // compositing model every stylesheet was authored against, so
            // HUD-over-scene lands where Chrome lands.
            //
            // What the reorder buys is where the dither ends up: it is applied
            // strictly UNDER the HUD, so a flat chrome fill reaches the screen
            // through one ROP rounding and nothing else. A fullscreen filter
            // over the composited frame cannot tell chrome from world content
            // and grains both.
            //
            // The composite stays FP16 on purpose. At 8 bits every overlapping
            // translucent HUD layer would take its own ROP rounding and the
            // error would accumulate as sqrt(N) — repeated 8-bit blending can
            // GENERATE banding in a HUD gradient that the FP16 path never had.
            // FP16 carries the dithered encoded values losslessly, and the
            // single quantization of the frame is the terminal transfer's ROP
            // store into the swapchain.
            //
            // Under an active HDR output mode nothing changes: the HUD keeps the
            // PRODUCER's stamped space (#784) and the terminal pass keeps the
            // single OETF, because the Finalize contract refuses encoded
            // presentation under HDR. HDR's quantizer is 1/1023 (or absent, on
            // scRGB), which is where the chrome-grain argument stops biting.
            // The shared policy declares it, so the engine carries one
            // implementation of the encode decision for every host.
            //
            // No host refusal: every SDR frame finalizes, HUD or not.
            // A document-less frame has nothing to composite, but that buys it
            // nothing to decline FOR — it would reach the terminal with a LINEAR
            // source, which selects the passthrough arm, which forces the dither
            // and the deband to zero. On a hardware-sRGB swapchain that frame
            // would present un-dithered and un-debanded. One fullscreen encode
            // buys both filters in the encoded domain, the only domain either is
            // correct in.
            const Engine::Renderer::ViewFinalizeResult view =
                Engine::Renderer::DeclareViewFinalize(*m_RenderGraphStream.Frame, out.Out,
                                                      rs.GetPipelineOutputSpaceRG(), "RuntimeHost",
                                                      debandLsb, presentedFormat,
                                                      /*hostRefusal=*/false);
            // Engaged exactly when a pass was declared, so it answers "did the
            // finalize run" without inferring it from the image or the space.
            if (view.Step.has_value())
            {
                presentSrc = view.Image;
                finalizeInput = Rendering::Passes::FinalizeInputSpace::EncodedSrgb;
            }
            if (hasHudDocuments)
            {
                // The HUD's target is whatever the chain finished with, and its
                // blend space is that image's PRODUCER stamp — never the display's.
                // The pointer arrived at the window callbacks, through the router's
                // play-surface leg, so rendering neither samples nor synthesises it.
                m_GameUI->UpdateAndRender(*m_RenderGraphStream.Frame, view.Image,
                                          UI::UITargetSpace::ForPipelineOutput(
                                              view.Space, device.GetActiveHdrOutputMode()),
                                          uiDesc.Width, uiDesc.Height);
            }
        }
    }

    // The composite hook: the owner reads the composited image back, or derives its
    // own image from it, before the terminal encode. A readback of presentSrc holds
    // encoded bytes on the SDR HUD chain and the pipeline's stamped space otherwise —
    // stated, never inferred from the RGBA16F format, so a PNG conversion does not
    // apply a second OETF.
    RuntimeFrameReadback readback(
        *m_RenderGraphStream.Frame, device,
        {.PresentSrc = presentSrc,
         .PresentInput = finalizeInput,
         .PresentSpace = finalizeInput == Rendering::Passes::FinalizeInputSpace::EncodedSrgb
                             ? UI::UITextureSpace::SrgbAuthored()
                             : rs.GetPipelineOutputSpaceRG(),
         .PresentedFormat = presentedFormat,
         .DebandThresholdLsb = debandLsb});
    if (m_Hooks.OnFrameComposited)
        m_Hooks.OnFrameComposited(readback);

    if (presentSrc.IsValid() && backbuffer.IsValid())
    {
        // presentSrc carries the HUD, blended under GameUIHost's declared target
        // space; finalizeInput states what its bytes hold (encoded on the SDR
        // HUD chain, linear otherwise) so the terminal never applies a second
        // OETF. The quantizer is the shared transfer decision at its degenerate
        // point — source step and destination are the same swapchain — so on the
        // HUD chain it answers None (the values already sit on the swapchain's
        // step; the pass degenerates to a transfer and the HUD reaches the
        // screen byte-clean) and on the linear chain Destination (this pass
        // still owns the OETF and the step).
        const auto terminalQuantizer = Rendering::Passes::SelectTransferQuantizer(
            finalizeInput == Rendering::Passes::FinalizeInputSpace::EncodedSrgb, presentedFormat,
            presentedFormat);
        const bool terminalOwnsStep =
            terminalQuantizer != Rendering::Passes::FinalizeQuantizer::None;
        Rendering::Passes::AddSRGBEncodePassRG(
            *m_RenderGraphStream.Frame, presentSrc, backbuffer,
            {.InputSpace = finalizeInput,
             .Quantizer = terminalQuantizer,
             .VolumeDebandThresholdLsb = terminalOwnsStep ? debandLsb : 0.0f,
             // Inert on the SDR HUD chain (None filters nothing); live on the
             // linear chain, where this pass owns the screen's only quantize.
             .DitherPhase = Rendering::Passes::ScreenDitherPhase(m_RenderGraphStream.Frame->FrameIndex())});
    }
    else
    {
        if (!m_WarnedNoOutput)
        {
            Logger::Log::Error(
                "Runtime(RenderGraph): no FinalColor output for view {} — nothing reaches the swapchain",
                m_ViewId);
            m_WarnedNoOutput = true;
        }
    }

    m_RenderGraphStream.Frame->Execute();

    // Stamp the frame's graphics token onto the CPU-readback pendings
    // (GPU-culling visibility + SDSM bounds) queued during declaration.
    rs.Spine().OnFrameSubmittedRG(*m_RenderGraphStream.Frame, m_RenderGraphStream.Frame->SubmissionToken());

    // Every readback declared on the frame, the composite hook's and any pass's,
    // becomes pollable only once it carries this frame's submission token.
    Rendering::OnFrameSubmittedReadbacksRG(*m_RenderGraphStream.Frame, m_RenderGraphStream.Frame->SubmissionToken());

    // GE_RG_DIAG=1: a pass dump and per-pass GPU timings.
    if (m_RenderGraphDiagnostics && m_RGFrameIndex == 2)
        m_RenderGraphStream.Frame->SetProfilingEnabled(true);
    if (m_RenderGraphDiagnostics && (m_RGFrameIndex % 120) == 10)
    {
        const auto& g = m_RenderGraphStream.Frame->Graph();
        fprintf(stderr, "[RGDiag] frame=%llu passes=%zu live=%zu finalColor=%u bb=%u\n",
                static_cast<unsigned long long>(m_RGFrameIndex), g.PassCount(),
                g.LivePassCount(), out.Out.IsValid() ? out.Out.Id : 0xFFFFFFFFu,
                backbuffer.IsValid() ? backbuffer.Id : 0xFFFFFFFFu);
        for (const auto& t : m_RenderGraphStream.Frame->LastFrameTimings())
            fprintf(stderr, "[RGDiag]   gpu span '%s' = %.4fms%s (cpu %.4fms)\n", t.Name,
                    static_cast<double>(t.GpuSpanMs), t.SpanShared ? " (shared encoder)" : "",
                    static_cast<double>(t.CpuMs));
        fflush(stderr);
    }
}

void RuntimeHost::Shutdown()
{
    if (m_RenderCtx)
    {
        if (auto* device = m_RenderCtx->GetDevice())
            device->WaitForIdle();
    }
    // RenderGraph frame objects die BEFORE the device context: ~RGFrame drains its
    // own queue timelines, then the ring/pools free their physicals against
    // the still-live device. Reap the RenderServices stream slot first so no
    // dangling key survives in a still-alive RS.
    if (m_RenderGraphStream.Frame && m_RenderCtx)
    {
        if (auto* rs = m_RenderCtx->GetRenderServices())
            rs->Spine().RemovePipelineInstanceForFrame(m_RenderGraphStream.Frame.get());
    }
    m_RenderGraphStream.Frame.reset();
    m_RenderGraphStream.UploadRing.reset();
    m_RenderGraphStream.TransientPool.reset();
    m_RenderGraphStream.PersistentPool.reset();

    auto* rs = m_RenderCtx ? m_RenderCtx->GetRenderServices() : nullptr;
    if (rs)
    {
        if (m_ViewId != 0)
        {
            rs->Views().ReleaseView(m_ViewId);
            m_ViewId = 0;
        }
        if (m_CameraId != 0)
        {
            rs->Views().ReleaseCamera(m_CameraId);
            m_CameraId = 0;
        }
    }

    // GameUIHost owns UIManager, whose element teardown can touch the asset
    // manager and rendering device. Destroy it before the render context and
    // EngineCore are torn down, rather than leaving it until the host's
    // destructor after those services are gone.
    m_GameUI.reset();

    EngineCore::GetInstance().DisableRenderingLoop();

    if (m_RenderCtx)
    {
        m_RenderCtx->Shutdown();
        m_RenderCtx.reset();
    }

    if (m_Window)
    {
        m_Window->Destroy();
        m_Window.reset();
    }

    Platform::Window::Terminate();
}

} // namespace GameEngine
