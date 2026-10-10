#pragma once

#include "AssetCore/GUID.h"
#include "Core/Application.h"
#include "Core/DeferredActionQueue.h"
#include "Core/WindowInputRouter.h"
#include "Core/WindowUiBootstrap.h"
#include "DeviceLossSurfacer.h"
#include "EditorContext.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGWindowFrame.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Controls/DockOverlay.h"
#include "UI/Interaction/Payload.h"
#include "UI/Layout/Docking.h"
#include "UI/UIManager.h"
#include "CssInspector/CssInspector.h"
#include "Panels/ProjectFolderPickerModal.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Panels/PlayModeChangeReviewModal.h"
#include "Editor/RenderGraphTickRegistry.h"
#include "EditorVideoPlayerController.h"
#include "Startup/EditorSessionDescriptor.h"
#include "JobSystem/TaskHandle.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Logger { class RingBufferSink; }

namespace GameEngine
{

class SettingsPanel;      // fwd
class EditorToolbar;      // fwd (native menu bar)
class EditorTopToolbar; // fwd (in-UI toolbar buttons)
class EditorPanelManager; // fwd (panel show/activate/close)
class LayoutPresetToolbar; // fwd (Editor UI control)
class SaveSceneChangesModal; // fwd (reused for layout switch prompt)
class RenameLayoutModal;     // fwd (rename layout preset prompt)
class UiReplayRunner; // fwd (automation)
class Button; // fwd
class AssetViewPanel; // fwd
class AssetsPanel; // fwd
class GraphPanel; // fwd
class HierarchyPanel; // fwd
class EditorVersionControlService; // fwd
class EditorVersionControlUi; // fwd
class EditorDebugServer; // fwd
class RenderDocCapture;  // fwd
class NsightGraphicsCapture; // fwd
namespace Editor
{
class ToolbarDragDrop; // fwd
namespace Startup
{
struct EditorCommandLineArgs;
} // namespace Startup
class UndoRedoService;
class EditorChangeNotifications;
class MarkupEditorBridge;
struct EditorPanelDescriptor;        // fwd (EditorSDK panel registry)
struct EditorStyleSheetContribution; // fwd (EditorSDK panel registry)
struct EditorOverlayDescriptor;      // fwd (EditorSDK panel registry)
class HdrOutputController;
class SceneEditorController;
class WindowDockingController;
class ColorPickerPresenter;
class SceneThumbnailCapture;
class PackageManagerController;
class MovieRecorderController;
class PlayModeManager;
class RecipeControllers;
class UniversalSearchController;
enum class PlayModeState : std::uint8_t;
namespace Telemetry
{
class RGCsvSidecar;
} // namespace Telemetry
} // namespace Editor
namespace Engine::Renderer
{
class RenderDeviceContext;
}
namespace Input
{
class InputSystem;
}

class MissingAssetTracker;

struct SceneViewCameraPose;

class EditorApplication : public Application
{
    // Allow MCP debug handler registration to read internal editor state.
    friend void RegisterDebugHandlers(EditorDebugServer& server,
                                      EditorApplication& app,
                                      Logger::RingBufferSink* ringBufferSink);
    friend void RegisterTerrainDebugHandlers(EditorDebugServer& server, EditorApplication& app);
    friend void RegisterGpuToolingHandlers(EditorDebugServer& server, EditorApplication& app);
    friend bool ApplyMainSceneViewPose(EditorApplication& app, const SceneViewCameraPose& pose);
    friend void RegisterMarkupDebugHandlers(EditorDebugServer& server, EditorApplication& app);

  public:
    enum class WindowRole : uint8_t
    {
        Main,
        Tool,
        Dialog
    };
    struct WindowCapabilities
    {
        bool worldRender = true;
        bool uiOnly = false;
        bool thumbnails = true;
        bool dockingHost = false;
    };
    struct WindowDescriptor
    {
        WindowRole role = WindowRole::Tool;
        WindowCapabilities capabilities{};
        std::string title;
        std::string panelId;
    };

    explicit EditorApplication(const ApplicationConfig& config);
    ~EditorApplication() override;

    // Configure Editor-specific command line options after construction but before Initialize().
    void ConfigureFromCommandLine(const Editor::Startup::EditorCommandLineArgs& args);

    // Raw argv[1..] captured by main() so a Save-and-Restart (after an unrecoverable
    // device loss) can relaunch this editor with the same command line.
    void SetRelaunchArgs(std::vector<std::string> args) { m_RelaunchArgs = std::move(args); }

    bool Initialize() override;
    void Update(float64 deltaTime) override;
    void Render() override;
    void OnShutdown() override;

    // Settings
    void SetDockEdgeFraction(float frac);
    void SyncHiDpiPlatformSettingsFromPreferences();

    // Actions callable from native menu handlers
    void ResetLayoutToDefault();
    /// Show or activate the named panel in the main editor window.
    /// preferredLeafId optionally hints which existing leaf the new tab should join.
    void OpenPanel(const std::string& panelId, const std::string& preferredLeafId = {});

    // Undo/redo actions (used by hotkeys and native menu items)
    bool CanUndo() const;
    bool CanRedo() const;
    const char* GetUndoActionName() const; // nullptr if none
    const char* GetRedoActionName() const; // nullptr if none
    void Undo();
    void Redo();
    // Editor toolbar accessor
    EditorToolbar* GetToolbar() const { return m_EditorToolbar.get(); }

    // Update project folder (called when user selects a folder)
    void SetProjectFolder(const std::filesystem::path& projectPath);
    void RequestHdrOutputRefreshForAllWindows();
    // Re-apply the project settings file's renderer knobs (MSAA, AA mode, TAAU
    // render scale, render-scale mode + dynamic target, mesh LOD) to every
    // window's RenderServices — Editor::ApplyProjectRenderSettings per window,
    // read from the current workspace root. Each window owns a RenderServices,
    // so the change must fan out; targets re-spec on the next declared frame.
    // Callers persist the edited key FIRST: the apply is total, so afterwards
    // every window's state for these knobs is a function of the file alone.
    void ApplyProjectRenderSettingsToAllWindows();

    // Copy the main window's live dynamic-resolution state — mode plus
    // controller tuning — onto every other window's RenderServices.
    //
    // Separate from the total re-apply above because DRS is only partly a
    // project-file knob: rendering.drsMode and rendering.drsTargetFps are
    // persisted, but a controller's target-ms, min-scale and max-scale are not,
    // so the file cannot carry them. The debug server's set_dynamic_resolution
    // is a live, non-persisting same-session A/B knob and needs this one; the
    // settings page persists its two keys and uses the file-backed re-apply.
    void ApplyDynamicResolutionFromMainWindowToAllWindows();

    // Multi-window scaffolding (shared device, per-window render graph/UI manager).
    // Public so ColorPickerWindow utility can return unique_ptr<EditorWindowContext>.
    struct EditorWindowContext
    {
        ~EditorWindowContext();

        uint64_t windowId = 0;
        WindowRole role = WindowRole::Tool;
        WindowCapabilities capabilities{};
        std::unique_ptr<Platform::Window> window; // platform window wrapper
        int width = 0;
        int height = 0;
        std::string title;
        std::unique_ptr<Engine::Renderer::RenderDeviceContext> renderCtx;
        std::unique_ptr<UIManager> ui;
        // The routing policy bound to this window's platform callbacks. Retained
        // so synthesized input (debug server) enters the same WindowInputRouter
        // entry points real input does, instead of reaching UIManager alone and
        // silently skipping the editor InputSystem and the play-pointer remap.
        WindowInputRouterConfig inputConfig;
        // Optional per-window UI runtime policy that should be preserved when this
        // context is attached into the editor window list.
        std::optional<UIManager::RenderRuntimeConfig> uiRuntimeConfigOverride;
        // Async UI bootstrap state (used by non-blocking floating window bootstrap).
        std::optional<WindowUiBootstrapAsyncState> uiBootstrapAsync;
        Rendering::CameraId modelThumbnailCameraId = 0;
        Rendering::ViewId modelThumbnailViewId = 0;
        std::unique_ptr<UI::IPlatformApi> uiPlatform;
        std::unique_ptr<class SceneViewController> scene;
        std::array<std::unique_ptr<class SceneViewController>, 3> sceneQuadViews;
        std::unique_ptr<class GameViewController> gameView;
        DockingManager* docking = nullptr;
        std::unique_ptr<DockingManager> dockingOwned;
        std::string floatingPanelId;
        int lastPosX = 0;
        int lastPosY = 0;
        bool hasLastPos = false;
        bool pendingShow = false; // show window after startup render warm-up
        uint32_t startupPresentedFrames = 0;
        uint32_t startupSceneReadyPresentedFrames = 0;
        bool pendingSwapchainResize = false;
        int pendingWidth = 0;
        int pendingHeight = 0;
        float lastNativeContentScaleX = 0.0f;
        float lastNativeContentScaleY = 0.0f;
        struct PendingHdrOutputUpdate
        {
            bool pending = false;
            int monitorIndex = -1;
            bool forceSwapchainRefresh = false;
            std::chrono::steady_clock::time_point requestedAt{};
        };
        PendingHdrOutputUpdate pendingHdrOutput{};
        std::chrono::steady_clock::time_point lastWindowMoveAt{};
        uint32_t skipRenderFramesAfterHdrSwitch = 0;
        double backgroundUiUpdateAccumulator = 0.0;
        // This window's render-graph frame stream: its frame, upload ring and
        // pools. Pools are per-window because RGFrame::Execute ticks pool aging
        // with the stream-local frame index; sharing pools across streams with
        // independent clocks destroys live entries (unsigned idle math wraps).
        // RGWindowFrame's declaration order fixes its destruction order (Frame,
        // UploadRing, TransientPool, PersistentPool); the explicit teardown
        // paths reset the members in that same order.
        Rendering::RenderGraph::RGWindowFrame RenderGraphStream;
        uint64_t rg2FrameIndex = 0;
        // Per-frame capture context for RenderGraph-capable debug-server callbacks
        // (MCP take_screenshot, 8c-4): filled by the frame driver right
        // before RunRenderCallbacks. RAW frame-local resource ids (the 8b
        // publish-map convention — typed handles never cross headers), valid
        // ONLY when frameIndex matches the frame's current incarnation.
        struct RGCaptureContext
        {
            Rendering::RenderGraph::RGFrame* frame = nullptr;
            uint32_t finalLinearId = 0xFFFFFFFFu; // kInvalidId
            uint32_t backbufferId = 0xFFFFFFFFu;
            // The space the composite's bytes hold under the frame's declared
            // UI target space — what a readback of finalLinearId must state
            // (P5): SrgbAuthored on SDR frames (#767 encoded blend),
            // DisplayLinearSdr on HDR-display capture frames.
            UI::UITextureSpace compositeSpace = UI::UITextureSpace::DisplayLinearSdr();
            bool pure = false;
            uint64_t frameIndex = 0;
        };
        RGCaptureContext rg2Capture;
        // Pairing tripwire: every RGFrame::BeginFrame must ride a fresh
        // IDevice::BeginFrame fence-wait (the ring slot rewind is only safe
        // behind it). Stamped in BeginWindowRenderFrame, checked at the RenderGraph
        // BeginFrame site.
        uint64_t deviceFrameStamp = 0;
        uint64_t rg2LastBeginDeviceStamp = 0;
    };
    std::vector<std::unique_ptr<EditorWindowContext>> m_Windows;
    // The main render services (EngineCore RS == window[0]'s renderCtx RS).
    // 8e: floating windows have PRIVATE per-window RS objects, but every
    // RenderGraph declaration path (spine, pipeline outputs, submit stamps) must use
    // the MAIN RS — the controllers were constructed on it.
    Engine::Renderer::RenderServices* MainRenderServices() const;
    // Framebuffer size callback (used by ColorPickerWindow and main window). Public so utilities can wire it.
    void OnFramebufferResized(EditorWindowContext* ctx, int width, int height);
    void OnFilesDropped(EditorWindowContext* ctx, const std::vector<std::filesystem::path>& paths);

  private:
    // When the scene document is dirty, shows the save modal; returns true when the quit was intercepted.
    bool TryInterceptQuitForDirtyScene();
    // UI replay command invoker: used by UiReplayRunner's invokeCommand event.
    bool InvokeUiReplayCommand(std::uint32_t commandId, std::string* outError);
    // UI replay window resize hook: used by UiReplayRunner's windowResize event.
    bool ResizeUiReplayWindow(std::uint32_t windowIndex, std::uint32_t width, std::uint32_t height, std::string* outError);

    // Helper for debug/profiling hotkeys (F2): format a perf line similar to the SceneView overlay.
    std::string BuildPerfDumpLine() const;
    struct FramePerfBreakdown
    {
        double pollMs = 0.0;
        double inputMs = 0.0;
        double appUpdateMs = 0.0;
        double engineUpdateMs = 0.0;
        double beginFrameMs = 0.0;
        double worldViewsMs = 0.0;     // RecordWindowWorldViews (entity / scene rendering record)
        double thumbnailsMs = 0.0;
        double uiRecordMs = 0.0;
        double terminalPassesMs = 0.0; // BuildWindowTerminalPasses + debug callbacks
        double rgCompileMs = 0.0;
        double rgExecuteMs = 0.0;
        // First-use SPIR-V variant compile time. Front-loaded by
        // RenderServices::PrewarmActiveVariants before rg->Compile so it no
        // longer inflates rgExecuteMs; reported here so the spike is still
        // visible in render_stats.
        double variantCompileMs = 0.0;
        double prewarmMs = 0.0;
        double presentMs = 0.0;
    };

    // Shared editor context (services, assets root)
    std::unique_ptr<EditorContext> m_EditorContext;
    std::unique_ptr<IThumbnailProvider> m_ThumbnailProvider; // lifetime owner; exposed via EditorContext

    // Q6 slice 6: surfaces device-loss health (native toast / save-and-restart dialog),
    // polled from the render-loop tick. Actions are wired in Initialize().
    Editor::DeviceLossSurfacer m_DeviceLossSurfacer;
    Editor::DeviceLossSurfacer::Actions m_DeviceLossActions;
    std::vector<std::string> m_RelaunchArgs; // argv[1..] replayed on Save-and-Restart

    // Shared editor services (document-scoped for now).
    std::unique_ptr<Editor::UndoRedoService> m_UndoRedo;
    std::unique_ptr<Editor::EditorChangeNotifications> m_ChangeNotifications;
    std::unique_ptr<Editor::MarkupEditorBridge> m_MarkupBridge;
    std::unique_ptr<Editor::SceneEditorController> m_SceneEditor;
    std::unique_ptr<Editor::SceneThumbnailCapture> m_SceneThumbnailCapture;
    std::unique_ptr<Editor::PlayModeManager> m_PlayMode;
    std::unique_ptr<Editor::MovieRecorderController> m_MovieRecorder;
    std::unique_ptr<Editor::RecipeControllers> m_RecipeControllers;
    std::unique_ptr<MissingAssetTracker> m_MissingAssetTracker;

    // Separate InputSystem for gameplay code — only receives events when
    // play mode is active and a Game View tab is showing. Mouse position is
    // Game View local (viewport origin), not window client pixels.
    std::unique_ptr<Input::InputSystem> m_RuntimeInput;
    // Cached once per frame: play mode + Game View is the active tab of some
    // window. Session-scoped (context enable, stuck-key reset), not per-window.
    bool m_RuntimeInputActive = false;
    bool m_WasRuntimeInputActive = false;
    bool m_WasPlayModeActive = false;
    // The play surface events arriving on `window` belong to — its HUD host, the
    // gameplay sink and the viewport's coordinate map — or an empty surface when
    // that window is not showing the Game View. Per-window: a torn-off Game View
    // must not consume keys typed into the main window. One answer for all three
    // legs so they cannot disagree about whether play is live here.
    WindowInputRouterConfig::PlaySurface GetPlaySurface(Platform::Window* window) const;
    // The chain a gamepad enters. A pad belongs to the session rather than to a
    // window, so the window running the game answers for it wherever editor
    // focus sits — which also keeps the destination still while a button is
    // held, since the edge that moves it resets the runtime sink, pad state
    // included.
    // With nothing playing, every editor window's chain ends at the same
    // application InputSystem, so the focused window's is used, and the main
    // window's when the editor holds no focus at all.
    const WindowInputRouterConfig& GamepadInputChain() const override;
    // True while play mode holds the world away from its authored state — the window in which
    // nothing may write the .scene file. Both write inhibits (the autosave tick and the manual
    // save probe) read THIS, so they cannot drift apart into two different definitions of
    // "playing". Every non-Edit state counts, ChangeReview included: its pending changes are
    // undecided, so the world is not yet the scene the user means to keep.
    bool IsPlayModeBlockingSceneWrites() const;
    // The window whose docking currently hosts `panel` and owns a Game View
    // controller, or null when the panel is not docked in a live window. Resolved
    // per call: a tear-off or redock moves the panel between windows, so no
    // controller may be retained across events.
    EditorWindowContext* FindWindowHostingGameView(const UIElement* panel) const;
    
    // Editor-owned VCS glue.
    std::unique_ptr<EditorVersionControlService> m_VcsService;
    std::unique_ptr<EditorVersionControlUi> m_VcsUi;
    bool m_PendingVcsInit = false;
    // One-shot: NotifyProvidersReady was sent — the package-module load pass
    // finished (or can never produce providers), so VCS detection may run.
    bool m_VcsProvidersReadySignaled = false;
    bool m_PendingPostInit = false;
    // GE_EDITOR_STARTUP_SCENE is opened from Update after the world path has
    // settled — the same boot-open class the wasm last-project skip avoids.
    std::filesystem::path m_DeferredStartupScene;
    std::filesystem::path m_DeferredStartupSceneWorkspace;
    bool m_PendingWindowShow = true;
    // Last project the per-project services (native C++ hot-reload, LOD import policy)
    // were wired to; Update() re-wires when the engine workspace root changes
    // (project open/switch).
    std::filesystem::path m_WiredProjectRoot;
    uint32_t m_StartupFrameCount = 0;
    // The startup preloads (a compute job for the UI assets, an "Engine startup" channel
    // job for the shader packages); OnShutdown waits for both before teardown.
    JobSystem::TaskHandle m_UiPreload;
    JobSystem::TaskHandle m_ShaderPreload;

    // In-UI toolbar (play/pause/stop, panel toggles, etc.)
    EditorTopToolbar* m_TopToolbar = nullptr; // owned by the main UI tree

    // Toolbar button drag-and-drop helpers (one per toolbar)
    std::unique_ptr<Editor::ToolbarDragDrop> m_ToolbarDragDrop;
    std::unique_ptr<Editor::ToolbarDragDrop> m_SceneViewToolbarDragDrop;

    // Panel lifecycle management (show, activate, close)
    std::unique_ptr<EditorPanelManager> m_PanelManager;

    // Global search/command palette (provider, dialog, registrations).
    std::unique_ptr<Editor::UniversalSearchController> m_UniversalSearch;

    std::filesystem::path m_AssetsDirectory;

    // Project package mounts, resolution, and Package Manager panel wiring
    // (mounts change only on project open/close; see the controller).
    std::unique_ptr<Editor::PackageManagerController> m_Packages;

    // HDR/display-output policy (swapchain re-spec, settle windows, SDR-white
    // poll). Never reset during shutdown: monitor-changed callbacks can fire
    // while windows tear down and deref this through the handler lambdas.
    std::unique_ptr<Editor::HdrOutputController> m_HdrOutput;

    // EditorSDK phase 2: consume dockable-panel descriptors, editor-chrome
    // stylesheets, and UI-root overlays that package Editor modules register
    // (packages load at project open; the registry replays anything
    // registered earlier).
    void AttachPackagePanelConsumers();
    void ConsumePackagePanelDescriptor(const Editor::EditorPanelDescriptor& descriptor);
    void ConsumePackageEditorStyleSheet(const Editor::EditorStyleSheetContribution& contribution);
    void ConsumePackageOverlay(const Editor::EditorOverlayDescriptor& descriptor);
    // Overlay ids already attached to the UI root: attach-once per id — a
    // module reload's re-registration must not stack a second overlay.
    std::set<std::string> m_AttachedOverlayIds;

    // UI replay automation configuration (set via ConfigureFromCommandLine).
    std::optional<std::filesystem::path> m_UiReplayScenarioPath;
    std::optional<std::filesystem::path> m_UiReplayLogPath;
    std::optional<std::uint64_t> m_UiReplayExitAfterFrames;
    std::unique_ptr<UiReplayRunner> m_UiReplay;
    uint64_t m_NextWindowId = 1;

    // MCP debug server for AI agent introspection.
    std::unique_ptr<EditorDebugServer> m_DebugServer;
    Logger::RingBufferSink* m_DebugLogSink = nullptr; // non-owning; owned by Logger
    // Port override from --debug-port; outranks GE_EDITOR_DEBUG_PORT.
    std::optional<std::uint16_t> m_DebugPortOverride;

    // Who this session is, stamped in ConfigureFromCommandLine (before Initialize) and
    // never refreshed. Surfaced in the window title and over get_editor_state so that
    // "may I touch this editor?" is one query rather than process forensics.
    Editor::Startup::EditorSessionDescriptor m_SessionDescriptor;

    // RenderDoc in-app capture (only active when launched under RenderDoc).
    std::unique_ptr<RenderDocCapture> m_RenderDoc;

    // Nsight Graphics in-app capture (only active when launched under ngfx-capture).
    std::unique_ptr<NsightGraphicsCapture> m_NsightCapture;

    // Polyhaven background download manager (placeholder entity → full model replacement).
    std::unique_ptr<PolyhavenDownloadManager> m_DownloadManager;

    // Modal shown when play/build/save is triggered while downloads are active.
    class DownloadInProgressModal* m_DownloadModal = nullptr; // owned by UI tree

    // Modal shown after exiting Play Mode to review/apply recorded editor changes.
    // Owned by the UI tree (mounted under the main window root).
    PlayModeChangeReviewModal* m_PlayModeChangeReviewModal = nullptr;

    // Intro video player (modal mounted on the main window root; opened via toolbar info button).
    EditorVideoPlayerController m_VideoPlayer;

    // Per-frame render-graph participants registered by panels through
    // EditorContext. Drained once per window per frame; the application never
    // learns what is in it.
    Editor::RenderGraphTickRegistry m_RenderGraphTicks;

    // Generic "exit after N frames" (applies even without UI replay).
    std::optional<std::uint64_t> m_ExitAfterFrames;
    std::uint64_t m_ExitFrameCounter = 0;

    // Phase C sidecar: emit one CSV row per RenderGraph frame Execute.
    // Path set via --bench-rg-csv; sidecar is lazily opened on the first frame.
    std::optional<std::filesystem::path> m_BenchRgCsvPath;
    std::unique_ptr<Editor::Telemetry::RGCsvSidecar> m_RgCsvSidecar;
    int m_RgCsvFrameCounter = 0;

    // Editor panels (owned by Editor); DockingManagers store non-owning pointers
    std::unique_ptr<DockingManager> m_Docking;              // main window docking model
    std::unique_ptr<DockNode> m_DefaultDockLayout;          // cloned default dock tree (from UXML or fallback)
    std::vector<std::unique_ptr<UIElement>> m_PanelStorage; // owns panel UI elements

    // Layout presets (in-memory; editor UI bottom toolbar)
    struct LayoutPreset
    {
        std::string name;
        std::unique_ptr<DockNode> layout;
    };
    std::vector<LayoutPreset> m_LayoutPresets;
    int m_ActiveLayoutPresetIndex = 0; // 0 = Default preset

    void InitializeLayoutPresets();
    void RecallLayoutPreset(int index);
    void SaveCurrentLayoutAsPreset(const std::string& name);
    void RemoveLayoutPreset(int index);
    void OverwriteLayoutPreset(int index);
    void RenameLayoutPreset(int index, const std::string& newName);
    void PromptRenameLayoutPreset(int index);
    bool ExportLayoutPresetToFile(int index, const std::filesystem::path& path) const;
    bool ExportAllLayoutPresetsToFile(const std::filesystem::path& path) const;
    int  ImportLayoutPresetsFromFile(const std::filesystem::path& path);
    // The file-dialog halves of the preset buttons' menu actions; the menus themselves
    // are declared on the buttons in LayoutPresetToolbar.
    void ExportLayoutPresetInteractive(int index);
    void ExportAllLayoutPresetsInteractive();
    void ImportLayoutPresetsInteractive();
    void SyncLayoutPresetToolbar();
    void ShowTabContextMenu(const std::string& panelId, float x, float y, Platform::Window* window);

    // SettingsPanel wiring helpers
    void InitializeTreeAndListSettings(SettingsPanel* settingsPanel, AssetsPanel* assetsPanel, HierarchyPanel* hierarchyPanel);
    void MigrateLegacySharedTreePreferences();
    void InitializeAssetsGridAndSmartFolderSettings(SettingsPanel* settingsPanel, AssetsPanel* assetsPanel);
    void InitializeInspectorSettings(SettingsPanel* settingsPanel);
    void ApplyInspectorToggleAlign(const std::string& value);

    // Sync Play/Pause/Stop button visuals with Play Mode state.
    void UpdatePlayModeToolbar(Editor::PlayModeState s);

    // Play mode presentation flags (Game View fullscreen play).
    bool m_PlayFullscreenOnEnter = false; // controlled by top toolbar fullscreen toggle
    bool m_PlayFullscreenActive = false;  // true while play mode has expanded layout
    int m_PlayFullscreenFocusFrames = 0;
    std::unique_ptr<DockNode> m_PlayPrevDockLayout; // saved dock layout for fullscreen play
    std::filesystem::path m_CurrentAssetPreviewPath;
    bool m_CurrentAssetPreviewEnabled = false;

    // Fullscreen helpers (implemented in EditorApplication_Fullscreen.cpp).
    std::unique_ptr<DockNode> CloneCurrentDockLayout() const;
    void SetDockLayoutFromClone(const DockNode* src);
    void EnterPlayFullscreen();
    void ExitPlayFullscreen();
    void RequestEnterPlayFullscreenAndPlay();
    bool CanEnterPlayFullscreen() const;
    void DeferToggleWindowFullscreen();
    void TickPlayFullscreenFocus();
    void BindPlayFullscreenChangedHandler(Platform::Window& window);

    // Persisted layout presets (Editor-global per-user settings).
    void LoadLayoutPresetsFromPreferences();
    void SaveLayoutPresetsToPreferences() const;
    void OnLayoutPresetRestoreFailed();

    // Generic deferred action runners used to move callback-triggered work to
    // stable points in the frame. Payload-specific handling stays in editor code.
    DeferredActionQueue m_DeferredPreUiActions;
    DeferredActionQueue m_DeferredPostUiActions;
    std::vector<std::unique_ptr<EditorWindowContext>> m_DeferredNativeToolWindows;
    std::optional<std::filesystem::path> m_DeferredOpenInternalFile;

    // Tracked editor UI asset refs (source-relative paths + resolved GUIDs).
    GUID m_LayoutGuid;
    GUID m_StyleGuid;
    std::filesystem::path m_LayoutPath;
    std::filesystem::path m_StylePath;

    // Floating window behavior flags
    bool m_FloatingFullCustomChrome = false; // borderless + custom toolbar (not enabled yet)
    bool m_FloatingShowInTaskbar = false;    // default off when owned

    // The window currently dispatching UI (set around UIManager::Render)
    EditorWindowContext* m_DispatchingWindow = nullptr;

    // Last measured perf breakdown for the primary Scene View window (best-effort).
    FramePerfBreakdown m_LastSceneViewPerf{};

    // Sub-phase breakdown of AppUpdateMs (diagnostic for --bench-rg-csv captures).
    // Sums roughly equal the outer AppUpdateMs minus small untimed fillers.
    struct AppUpdateSubPhases
    {
        double preUiMs = 0.0;         // startup + modals + polls + base Update + ECS ProcessCommands + SceneEditor + flushes + pre-UI deferred
        double debugPanelsMs = 0.0;   // CollectEditorDebugMetrics + PanelManager->UpdatePanels (log flush + debug panels)
        double uiWindowsMs = 0.0;     // UI UpdateAllWindows loop (per-window UIManager::Update)
        double tailMs = 0.0;          // post-UI deferred actions + TickPlayFullscreenFocus + input routing + hotkeys
    };
    AppUpdateSubPhases m_LastAppUpdateSubPhases{};

    // Time spent in EditorApplication::Render() before the per-window
    // render loop begins (cross-window DnD, tear-off, OS-drag overlays).
    // Surfaced in get_render_stats so the gap between Application::RenderMs
    // and the per-window sub-phase total can be attributed.
    double m_LastRenderPreloopMs = 0.0;

    // Cross-window drag-drop / tear-off / dock-preview policy and state live
    // in the controller; window/GPU creation stays here (UndockToFloatingWindow).
    std::unique_ptr<Editor::WindowDockingController> m_DockDnd;

    // Presents a panel swatch's colour picker: native tool window or in-engine
    // modal, decided by the platform per open. Panels hold its callback.
    std::unique_ptr<Editor::ColorPickerPresenter> m_ColorPicker;

    // Helpers
    void RenderSingle(EditorWindowContext* ctx);
    void RefreshWorldRenderForPassiveFrame(EditorWindowContext* ctx);

    struct RenderWindowFrameResult
    {
        double worldViewsMs    = 0.0; // RecordWindowWorldViews
        double thumbnailsMs    = 0.0;
        double uiRecordMs      = 0.0;
        double terminalPassesMs = 0.0; // BuildWindowTerminalPasses + debug callbacks
        double rgCompileMs     = 0.0;
        double rgExecuteMs     = 0.0;
        double variantCompileMs = 0.0;
        double prewarmMs       = 0.0;
        double presentMs       = 0.0;
        bool sceneViewportReady = false;
    };
    // Shared per-window RenderGraph render sequence: world views → thumbnails → UI
    // composite → terminal encode → execute → present.
    // passiveUi=true skips interactive input dispatch and UI replay (used by
    // RenderSingle during OS window moves).
    RenderWindowFrameResult RenderWindowFrame(EditorWindowContext* ctx,
                                              Rendering::IDevice* dev,
                                              bool passiveUi);
    void QueueUndockPanel(std::string panelId);
    void QueueNativeToolWindow(std::unique_ptr<EditorWindowContext> ctx);

    /// Unmount torn-off panels without restoring tabs. Layout apply places them.
    void DismissTornOffPanelsForLayoutRestore();

    void QueueOpenInternalFile(std::filesystem::path path);
    void FlushPreUiDeferredActions();
    void FlushPostUiDeferredActions();
    void AttachDeferredNativeToolWindows();
    void OpenDeferredInternalFile();
    bool OpenGraphAsset(const std::filesystem::path& path);
    void BindGraphPanel(GraphPanel& panel);
    void ActivateGraphDock(EditorWindowContext* ctx, GraphPanel& panel);
    void ShowOrFocusAssetViewInSceneTabs(EditorWindowContext* ctx);
    // Catalog-backed global keys that must fire in onKeyPre (before UI routing).
    // includeWorkspaceShortcuts also handles Toggle Preview and Close Tab.
    bool TryHandleCatalogGlobalKeyPre(EditorWindowContext* ctx, int key, int action, int mods,
                                      bool includeWorkspaceShortcuts);
    static AssetViewPanel* FindAssetViewPanelById(DockingManager* docking, const char* panelId);
    void ToggleDockDebugZones();
    CssInspector m_CssInspector;

#if defined(_DEBUG)
    // Debug-only helper that asks the active editor UI to export its current
    // layout and resolved styles into an XML/HTML-compatible representation
    // for external inspection/import. Compiled out in non-debug builds.
    void DebugExportCurrentUILayout(EditorWindowContext* ctx);
#endif

    // Window move callback from Platform::Window (fires during native OS move)
    void OnWindowMoved(EditorWindowContext* moving, int x, int y);

    // Undock a panel from the main window into a new floating window (PoC)
    void UndockToFloatingWindow(const std::string& panelId);

    // Heavy UI/asset-browser refresh after project open; queued so SetProjectFolder returns quickly
    // (avoids stalling the modal callback and nested UI work on the same stack).
    void ApplyProjectFolderUiAsync(const std::filesystem::path& projectPath,
                                   const std::filesystem::path& newAssetRoot);

    // What a project switch still needs once its asset sources have finished
    // ejecting. Unmounting the old packages and rebinding the project source
    // both complete asynchronously, so the roots the second half works from are
    // carried in a value rather than read back off members that have moved on.
    struct ProjectSwitch
    {
        std::filesystem::path ProjectPath;
        std::filesystem::path NewAssetRoot;
        std::filesystem::path OldAssetRoot;
        std::filesystem::path OldDbFile;
        std::filesystem::path OldCacheRoot;
    };

    // The second half of SetProjectFolder: everything that may only run once
    // the project source points at the new root.
    void FinishProjectFolderSwitch(const ProjectSwitch& pending);

    // The failure half: put the project source back on the old root, remount
    // the old project's packages, and let the picker go.
    void RollBackProjectFolderSwitch(const ProjectSwitch& pending);

    // Set while a switch is between its two halves. A second request in that
    // window would race the first over the same source, so it is refused with a
    // log line. If the switch never completes — the only way being an eject
    // whose loads never resolve, which the asset manager reports once after a
    // few seconds — the flag stays set and the picker keeps its loading state:
    // the project does not open, and nothing else in the editor stops.
    bool m_ProjectSwitchPending = false;

    // Point the native C++ hot-reload manager at a project (loads the staged SDK manifest,
    // sets the build config + Assets watch dir, kicks an initial build if sources exist).
    // Driven from Update() on workspace-root change so it covers every project-open path
    // (explicit --project, default exe-dir, auto-load-last, runtime switch).
    void WireNativeScriptingForProject(const std::filesystem::path& projectRoot);

    // Push the project's auto-LOD import policy (import.autoGenerateLODs, default on)
    // down to the engine import path. Driven from the same workspace-root-change hook
    // as native scripting, plus eagerly from SetProjectFolder before the project source
    // rebinds so the policy is live before the new project's models start loading.
    void ApplyProjectLodImportPolicy(const std::filesystem::path& projectRoot);

  private:
    /// Main window title: "Open Engine Editor <version> - <project folder> - <scene file or Untitled>",
    /// plus "  [<session>]" when FormatSessionChromeSuffix knows who this session is.
    /// Skips the native SetTitle call when the string is unchanged (see Update()).
    void RefreshMainWindowTitle();

    std::string m_LastMainWindowTitle;
    bool m_LastSaveDirty = false;
    std::optional<std::filesystem::path> m_LastTitleScenePath;
    // Watched separately from the path: a document with no path still has a name
    // (an untitled scene, or the file a failed open names), and consecutive
    // failures move only this one.
    std::optional<std::string> m_LastTitleSceneDisplayName;

    // Bottom-bar status refresh (1 Hz tick in Update): Vulkan validation badge, which stays
    // an empty label at zero errors, and the build-configuration tag.
    double m_FooterStatusAccum = 0.0;
    uint64_t m_ValidationBadgeErrors = 0;
    // Tree-structure generation at which the build-config tag was last asserted; a UI
    // rebuild that recreates the label bumps it and re-triggers the id lookup.
    uint64_t m_BuildConfigTagTreeGeneration = 0;

    // Destroy toolbar early during teardown by placing it last so it is released first
    std::unique_ptr<EditorToolbar> m_EditorToolbar;
#if defined(__APPLE__)
    bool m_EnableNativeToolbar = true;
#else
    bool m_EnableNativeToolbar = false;
#endif
    
    // Project folder picker modal (shown on launch if no project specified)
    // Note: owned by UI tree, stored as raw pointer for access
    ProjectFolderPickerModal* m_ProjectFolderModal = nullptr;

    // While the picker covers the editor on a host that drives the frame loop
    // (Platform::HostDrivesFrameLoop), skip world extract/cull/scene RG: there a
    // cache-cold world frame compiles pipelines on the main thread and Tick()
    // does not return, so the painted picker cannot hover or click until that
    // stall drains. Elsewhere the world keeps rendering behind the picker.
    // Implemented in EditorApplication_Boot.cpp.
    bool ShouldIdleWorldPathForProjectPicker() const;
    void SyncWorldPathForProjectPicker();
    void QueueDeferredStartupScene();
    void TryOpenDeferredStartupScene();
    bool m_WorldPathIdledForProjectPicker = false;

    // Modal for prompting when switching layouts with unsaved changes.
    SaveSceneChangesModal* m_LayoutChangesModal = nullptr;

    // Modal for prompting layout preset rename (lazy-created).
    RenameLayoutModal* m_RenameLayoutModal = nullptr;

    // Native context menu for layout preset buttons (lazy-created, reused).
    std::unique_ptr<INativeContextMenu> m_LayoutPresetContextMenu;
    int m_LayoutPresetContextMenuIndex = -1;

    // Dock tab right-click menu. Held here, not on the stack: the editor-drawn
    // backend's Show() returns as soon as the menu is built, so a local would
    // destruct — and close the menu — in the frame it opened. Only the native
    // backends block inside Show().
    std::unique_ptr<INativeContextMenu> m_TabContextMenu;

    void RequestSwitchLayoutPreset(int index);
    bool IsActiveLayoutDirty() const;
    bool m_ProjectFolderSelected = false;
    std::filesystem::path m_SelectedProjectPath;
    // Full-root style+layout pass is deferred to the frame after ApplyProjectFolderUiAsync so the
    // first render after the project picker closes is not composited mid-invalidation (avoids a
    // brief wrong background / "flash" in the scene view region).
    bool m_PendingProjectFolderStyleRelayout = false;

    // Set true as soon as teardown begins to prevent late callback re-entry
    // (e.g. framebuffer resize events firing during toolbar uninstall).
    bool m_IsShuttingDown = false;
};

} // namespace GameEngine
