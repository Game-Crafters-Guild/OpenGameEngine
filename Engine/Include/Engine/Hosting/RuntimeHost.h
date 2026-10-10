#pragma once

#include "Core/WindowInputRouter.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/AntiAliasingProjectSettings.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGWindowFrame.h"
#include "UI/UIScaleSettings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine {

namespace Platform { class Window; }
namespace Engine::Renderer {
class RenderDeviceContext;
class RenderServices;
enum class PipelineResolveFailure : uint8_t;
enum class SceneResolveBatch : uint32_t;
} // namespace Engine::Renderer
class Application;
class GameUIHost;
class RuntimeFrameReadback;

/// The settings a RuntimeHost reads to bring up its window, its render device and its pipeline.
struct RuntimeHostDesc
{
    /// The window title, also the application name the render device reports.
    std::string Title;
    /// Applied to the window after it is created; Windowed leaves it as created.
    WindowMode Mode = WindowMode::Windowed;
    /// The window's client size at creation, in screen coordinates, and the render size the
    /// device starts at when the window reports no framebuffer yet.
    uint32_t WindowWidth = 1920;
    uint32_t WindowHeight = 1080;
    /// Presents in step with the display's refresh when true.
    bool VSync = true;
    /// Requests an HDR swapchain; the device falls back to SDR when the display has none.
    bool HdrEnabled = false;
    /// The HDR output encoding requested when HdrEnabled is set.
    Rendering::HdrOutputMode HdrMode = Rendering::HdrOutputMode::HDR10_PQ;
    /// The bit depth of an HDR swapchain.
    Rendering::HdrSwapchainBitDepth HdrBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
    /// The display the HDR output targets; -1 is the window's own monitor.
    int HdrTargetDisplay = -1;
    /// The mastering metadata an HDR swapchain reports to the display.
    Rendering::HdrStaticMetadata HdrMetadata{};
    /// Applied to the render services before any mesh registers.
    Rendering::LodProjectSettings LodSelection;
    /// The engine-wide anti-aliasing and render scale; per-camera overrides still win per view.
    Rendering::AntiAliasingProjectSettings RenderQuality;
    /// Asset-relative path of the .rendergraph to render with; empty selects the engine's
    /// default pipeline.
    std::string RenderPipeline;
    /// The HUD's scale policy.
    UI::UIScaleSettings UIScale;
};

/// The two points where a RuntimeHost hands control back to its owner during a frame.
struct RuntimeHostHooks
{
    /// Called instead of rendering, every frame the render device is in the terminal Failed
    /// state. What a failed device means for the session (exit, save, report) is the owner's.
    std::function<void()> OnDeviceFailed;
    /// Called every rendered frame after the HUD composite and before the terminal encode to
    /// the window, with the composited image. Readbacks declared on the frame are stamped once
    /// the frame is submitted, like every readback the frame declares.
    std::function<void(RuntimeFrameReadback&)> OnFrameComposited;
};

/// The runtime's window and render frame: one window with the game's HUD as its play surface,
/// the render device context and the engine-managed rendering loop on it, the render pipeline,
/// the scene opened into the primary world, and the render-graph frame that draws the primary
/// world's active camera, composites the HUD and presents.
///
/// The owner brings it up in order (InitWindow, InitRendering, LoadRenderPipeline, OpenScene),
/// mounting assets and loading native modules between the steps as it needs, calls Render once
/// per application frame, and Shutdown before the engine shuts down.
class RuntimeHost
{
public:
    /// `application` drives the frames: its input system receives the window's input and its
    /// Tick runs when the window asks for a refresh. It must outlive the host.
    RuntimeHost(Application& application, RuntimeHostDesc desc, RuntimeHostHooks hooks);
    ~RuntimeHost();
    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

    /// Creates the window, binds its input to the application's input system and the HUD, and
    /// applies the window mode. False when the window cannot be created.
    bool InitWindow();
    /// Creates the render device and its render services on the window, enables the
    /// engine-managed rendering loop and applies the LOD selection and render quality settings.
    /// Requires InitWindow.
    bool InitRendering();
    /// Resolves and compiles the render pipeline. Call after every native module that registers
    /// pipeline node types is loaded. Returns PipelineResolveFailure::None on success; otherwise
    /// the reason, logged with the resolver's message, for the owner to state its own fix.
    Engine::Renderer::PipelineResolveFailure LoadRenderPipeline();
    /// Replaces the primary world's content with the scene at `scenePath`, relative to the
    /// resolved asset root, hands its models and standalone materials to the rendering loop's
    /// resolve service as one batch (GetSceneResolveBatch), and adds a default camera when the
    /// scene has none. No frame waits for the batch: each entity binds on the frame its model
    /// has landed, and its skeleton binds after. False, with the reason logged, when it cannot load.
    bool OpenScene(const std::filesystem::path& scenePath);
    /// The resolve batch of the scene the last OpenScene loaded, for the owner to ask the
    /// rendering loop's resolve service when the scene has resolved. None before OpenScene, or
    /// when it ran before InitRendering.
    Engine::Renderer::SceneResolveBatch GetSceneResolveBatch() const { return m_SceneResolve; }
    /// Renders one frame of the primary world's active camera and presents it. `deltaTime` is
    /// the frame time the pipeline's temporal passes advance by, in seconds.
    void Render(float deltaTime);
    /// Waits for the device, then releases the frame stream, the view, the HUD, the rendering
    /// loop, the render device and the window. Readbacks the owner still holds must be read or
    /// dropped before it: they cannot resolve after it.
    void Shutdown();

    /// Null before InitWindow and after Shutdown.
    Platform::Window* GetWindow() const { return m_Window.get(); }
    /// Null before InitRendering and after Shutdown.
    Engine::Renderer::RenderDeviceContext* GetRenderDeviceContext() const { return m_RenderCtx.get(); }

private:
    // The window's play surface, resolved per event: the HUD host, and the
    // mapping that makes this window's pointer the HUD's. Empty until the first
    // rendered frame builds the host. No gameplay sink — with no chrome above
    // it, the game's own InputSystem ends the chain instead of sitting inside it.
    WindowInputRouterConfig::PlaySurface HudPlaySurface() const;
    // The immediate-mode frame for the window — pool imports for the view targets,
    // ImportBackbuffer, the spine, the HUD composite, the composite hook, the
    // terminal encode reading GetPipelineOutputRG, Execute, token stamps.
    void RenderFrameRG(Engine::Renderer::RenderServices& rs, Rendering::IDevice& device,
                       uint32_t w, uint32_t h, bool useBlackClear, float deltaTime);
    // Recreates the window's swapchain at a positive framebuffer size (the monitor-changed handler).
    void RecreateSwapchain(int width, int height);
    // Recreates it only when the framebuffer size differs from the swapchain's (the
    // framebuffer-size handler).
    void FollowFramebufferSize(int width, int height);

    Application& m_Application;
    RuntimeHostDesc m_Desc;
    RuntimeHostHooks m_Hooks;

    std::unique_ptr<Platform::Window> m_Window;
    std::unique_ptr<Engine::Renderer::RenderDeviceContext> m_RenderCtx;
    // The batch OpenScene handed to the resolve service; zero is SceneResolveBatch::None.
    Engine::Renderer::SceneResolveBatch m_SceneResolve{};

    // Camera / view state
    Rendering::CameraId m_CameraId{0};
    Rendering::ViewId m_ViewId{0};
    // Per-view resolved AA sample count (camera override folded against the
    // engine default), published by the camera block for target creation.
    uint32_t m_ViewSampleCount = 1;

    // The render-graph frame stream of the window, built on first render.
    Rendering::RenderGraph::RGWindowFrame m_RenderGraphStream;
    uint64_t m_RGFrameIndex = 0;
    // GE_RG_DIAG=1: a pass dump and per-pass GPU timings on stderr. Read once per host.
    bool m_RenderGraphDiagnostics = false;
    // The missing-output error is logged once per host.
    bool m_WarnedNoOutput = false;

    // Game UI host: composites UIDocument entities over FinalColor before the
    // terminal encode — the runtime HUD. Lazy-created on first render.
    std::unique_ptr<GameUIHost> m_GameUI;
};

} // namespace GameEngine
