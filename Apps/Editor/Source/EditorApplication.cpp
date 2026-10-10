#include "EditorApplication.h"
#include "Editor/BuildExporters/SteamDeckBuildExporter.h"
#include "Editor/BuildExporters/WebBuildExporter.h"
#include "Editor/Application/BuildConfigStatus.h"
#include "Editor/Application/EditorVersion.h"
#include "EditorPanelIds.h"
#include "EditorPanelManager.h"
#include "Mathematics/Vector2.h"
#include "UI/Controls/EditorTopToolbar.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "UI/BuiltInViewOverlays.h"
#include "UI/ViewOverlayHost.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "UI/Layout/DockingHitTest.h"
#include "UI/Layout/EditorDockConfigParser.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Assets/UIStyleAsset.h"
#include "Editor/DragDropPayloads.h"

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetPipelineCommands.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/EditorAssetActionsInstall.h"
#include "Assets/EditorAssetRegistrations.h"
#include "Assets/CoreAssetRegistrations.h"
#include "Diagnostics/MainThreadHangWatchdog.h"
#include "Display/HdrOutputController.h"
#include "EditorWindowBootstrap.h"
#include "Docking/WindowDockingController.h"
#include "Packages/PackageManagerController.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageNativeCache.h"
#include "Core/Application.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"
#include "Core/Time.h"
#include "Core/WindowInputRouter.h"
// A2.4-P0-R: full JobSystem headers to build the RGFrame fork-join primitive
// (Core/Engine.h only forward-declares WorkStealingThreadPool).
#include "JobSystem/JobChannel.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Core/WindowUiBootstrap.h"
#include "Editor/Settings/BuildSettingsPage.h"
#include "Editor/Settings/EditorHiDpiPlatformSettings.h"
#include "Editor/EditorPaths.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "Editor/Settings/ScriptEditorSettings.h"
#include "Editor/Assets/AssetOpenRouting.h"
#include "Editor/Assets/ShaderGlslOpen.h"
#include "Editor/Settings/PhysicsProjectSettings.h"
#include "Editor/Settings/RenderPipelineSettings.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/SvgRasterSettings.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/TooltipSettings.h"
#include "UI/ToolbarDragDrop.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/UI/FontResolver.h"
#include "PhysicsECS/PhysicsWorldSettingsHelpers.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/EditorVersionControlUi.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "ScriptEditor/SourceLocation.h"
#include "ScriptEditor/SourceLocationOpener.h"
#include "Startup/BuiltinAssetSync.h"
#include "Startup/EditorCommandLine.h"
#include "Startup/EditorPathPolicy.h"
#include <filesystem>
#include <fstream>
#include <iomanip>

#include "ExternalScriptEditorLauncher.h"
#include "Scene/DefaultSceneEntities.h"
#include "Platform/Capabilities.h"
#include "Platform/Shell.h"

// Rendering & windowing
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECSModules/Rendering/Systems/RenderGraphBuildSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VkValidationRequest.h"
#include "Engine/Rendering/ViewFinalize.h"
#include "Input/InputSystem.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGWindowFrame.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CGEventSource.h>
#endif
#include "Input/KeyCodes.h"

// Editor panel stubs
#include "Automation/EditorSmokeHooks.h"
#include "Automation/UiReplayRunner.h"
#include "Telemetry/RGCsvSidecar.h"
#include "Automation/UiReplayCommandIds.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/DebugHandlers.h"
#include "DebugServer/NsightGraphicsCapture.h"
#include "DebugServer/RenderDocCapture.h"
#include "Logger/RingBufferSink.h"
#include "EditorInputActions.h"
#include "GameViewController.h"
#include "MovieRecorderController.h"
#include "MovieRecorderToolbarMenu.h"
#include "Panels/AssetsPanel.h"
#include "Panels/TodoPanel.h"
#include "Panels/BookmarksPanel.h"
#include "Panels/UndoHistoryPanel.h"
#include "Panels/GameViewPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/ScriptErrorsPanel.h"
#include "Panels/ShaderErrorsPanel.h"
#include "NativeScripting/NativeScriptManager.h" // native C++ user-script hot-reload
#include "NativeScripting/NativeSourceTree.h"    // does the project carry native sources at all
#include "NativeScripting/SdkManifest.h"         // staged-SDK build config for user DLLs
#include "Panels/MonitorsPanel.h"
#include "Panels/RenderGraphPanel.h"
#include "Panels/CpuProfilerPanel.h"
#include "Panels/VisualProfilerPanel.h"
#include "Panels/VramPanel.h"
#include "EditorDebugMetrics.h"
#include "Panels/PlayModeChangeReviewModal.h"
#include "EditorVideoPlayerController.h"
#include "Panels/ProjectFolderPickerModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "EditorDockNodeJson.h"
#include "Panels/SceneViewPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/GraphPanel.h"
#include "Graph/GraphKindChrome.h"
#include "Graph/GraphPanelFactory.h"
#include "ColorPicker/ColorPickerContext.h"
#include "ColorPicker/ColorPickerEditorIntegration.h"
#include "ColorPicker/ColorPickerPresenter.h"
#include "UI/EditorIcons.h"
#include "UI/UIAccentStyleHelper.h"
#include "UI/EditorUIFontSettings.h"
#include "UI/UICursorHelper.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/SettingsPanel.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "Panels/UIDemoPanel.h"
#include "Panels/MissingAssetsPanel.h"
#include "Panels/PackageManagerPanel.h"
#include "MissingAssetTracker.h"
#include "Panels/DiffPanel.h"
#include "Panels/WebPanel.h"
#include "Panels/AnimationWindowPanel.h"
#include "Panels/MixerPanel.h"
#include "Panels/BuildPanel.h"
#include "Panels/BuiltInPanelTypes.h"
#include "SceneView/GizmoSettingsWiring.h"
#include "SceneView/EditorSceneViewDropWiring.h"
#include "SceneView/SceneViewEntityLinks.h"
#include "SceneView/SceneViewRenderCoordinator.h"
#include "SceneViewController.h"

#include "Assets/PolyhavenDownloadManager.h"
#include "Panels/DownloadInProgressModal.h"
#include "EditorChangeNotifications.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupEditorSurfaces.h"
#include "SceneView/BuiltInSceneViewTools.h"
#include "Placement/RecipeControllers.h"
#include "PlayMode/PlayModeManager.h"
#include "PlayMode/RuntimeInputConnection.h"
#include "Scene/SceneEditorController.h"
#include "Scene/SceneThumbnailCapture.h"
#include "Scene/SceneSchemaRegistry.h"
#include "UndoRedo/UndoRedoService.h"
#include "Scripting/ScriptingService.h"

#include "EditorToolbar/EditorToolbar.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Registration/ElementRegistration.h"

#include "Thumbnails/ModelThumbnailHandler.h"
#include "Thumbnails/TextureThumbnailHandler.h"
#include "Thumbnails/ThumbnailMenuItems.h"
#include "Thumbnails/ThumbnailService.h"
#include "Thumbnails/VideoThumbnailHandler.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/TextArea.h"
#include "Search/UniversalSearchController.h"
#include "UI/Controls/DockOverlay.h"
#include "UI/Controls/LayoutPresetToolbar.h"
#include "UI/Controls/Mount.h"
#include "Core/WindowPlatformApi.h"
#include "UI/UiContext.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/DragDropGhostRenderers.h"
#include "UI/Interaction/Types.h"

// Linux runtime icon loading via stb_image. On Windows we use the embedded
// .ico resource (set via Win32 APIs) and on macOS the app bundle's .icns.
#if !defined(_WIN32) && !defined(__APPLE__) && defined(GE_HAVE_STB)
#include <stb_image.h>
#endif

#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Vector3Field.h"

#include "EditorContextMenu/MenuIconPreload.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Editor/Registries/EditorSceneCommands.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Picking/EditorPickProviders.h"
#include "Platform/ContextMenu.h"
#include "Platform/Display.h"
#include "Platform/Environment.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/ComponentLayoutReloadMigrator.h" // hot-reload component migration
#include "ECS/UnresolvedComponentStore.h"
#include "InspectorRegistry.h"
#include "Inspectors/BuiltInInspectorRegistration.h"
#include "Inspectors/AnimationPreviewManager.h"
#include "Inspectors/PhysicsColliderInspector.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/ModelEntityFactory.h"

namespace GameEngine
{

using namespace Rendering;
using namespace UI;

EditorApplication::EditorWindowContext::~EditorWindowContext()
{
#if !defined(_WIN32) && !defined(__APPLE__)
    if (window)
        UIContextMenu::Unregister(window.get());
#endif
}

namespace
{

// MCP debug-server port used when neither --debug-port nor GE_EDITOR_DEBUG_PORT
// selects one. Tooling defaults to this port, so a second editor on the same
// machine must be given its own rather than inheriting this.
constexpr uint16_t kDefaultEditorDebugPort = 9999;

} // namespace

// Reads the saved "renderer.graphicsApi" preference (auto|vulkan|metal) set in
// Settings > Performance. Unsupported/unknown values fall back to Auto; the
// GE_GFX_API environment override in DeviceFactory still wins over this.
Rendering::GraphicsAPI LoadPreferredGraphicsApi()
{
    std::string requested = "auto";
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString("renderer.graphicsApi", requested);
    }
    Rendering::GraphicsAPI api = Rendering::GraphicsAPI::Auto;
    if (requested == "vulkan")
        api = Rendering::GraphicsAPI::Vulkan;
    else if (requested == "metal")
        api = Rendering::GraphicsAPI::Metal;
    if (api != Rendering::GraphicsAPI::Auto && !Rendering::DeviceFactory::IsAPISupported(api))
    {
        Logger::Log::Warning("Graphics API preference '{}' is not supported in this build; using Auto", requested);
        return Rendering::GraphicsAPI::Auto;
    }
    if (api != Rendering::GraphicsAPI::Auto)
        Logger::Log::Info("Graphics API preference: {}", requested);
    return api;
}


// True if the current UI focus is in a TextField or TextArea (so Ctrl+I is not treated as the CSS inspector toggle).
bool IsFocusInTextField(UIManager* ui)
{
    if (!ui) return false;
    const std::string& focusId = ui->GetFocusedElementId();
    if (focusId.empty()) return false;
    UIElement* root = ui->GetRootElement();
    if (!root) return false;
    for (UIElement* el = root->FindById(focusId); el; el = el->GetParent())
    {
        if (dynamic_cast<TextField*>(el) || dynamic_cast<TextArea*>(el))
            return true;
    }
    return false;
}

namespace
{

// Optional correctness-mode override for debugging invalidation issues.
// Disabled by default; enable with GE_EDITOR_FORCE_UI_CORRECTNESS=1.
static bool IsUiCorrectnessModeForcedForAllWindows()
{
    return Platform::EnvironmentSwitchEnabled("GE_EDITOR_FORCE_UI_CORRECTNESS", false);
}

} // namespace

/// The project an ordinary launch opens by itself: the last project from
/// preferences, when the host policy and the user's auto-load preference both
/// allow it and the path still exists. Nullopt means the launch will show the
/// picker instead.
static std::optional<std::filesystem::path> ResolveAutoLoadProjectRoot(const GameEngine::ApplicationConfig& config)
{
    if (!config.AutoOpenLastProjectOnBoot)
        return std::nullopt;

    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool autoLoad = true; // default on
    prefs.TryGetBool("startup.autoLoadLastProject", autoLoad);
    if (!autoLoad)
        return std::nullopt;

    std::string lastPath;
    if (!prefs.TryGetString("lastProjectPath", lastPath) || lastPath.empty())
        return std::nullopt;

    std::filesystem::path lastProject(lastPath);
    std::error_code ec;
    if (!std::filesystem::exists(lastProject, ec) || ec)
        return std::nullopt;
    return lastProject;
}

/// Project root whose settings configure the device before it exists — startup
/// HDR mode, dynamic resolution, TAA scale.
///
/// A fallback WorkspaceDirectory is not the project that will open: with no
/// --project, EditorStartup points it at DefaultProject and flags it, and the
/// auto-load a few phases later replaces it with the last project. Resolving the
/// same root here is what lets the swapchain be created in that project's HDR
/// mode instead of being recreated into it once the project lands.
static std::filesystem::path ResolveStartupProjectSettingsRoot(const GameEngine::ApplicationConfig& config)
{
    if (config.WorkspaceDirectoryIsFallback)
    {
        if (const std::optional<std::filesystem::path> autoLoadRoot = ResolveAutoLoadProjectRoot(config))
            return *autoLoadRoot;
    }
    if (!config.WorkspaceDirectory.empty())
        return std::filesystem::path(config.WorkspaceDirectory);
    return EngineCore::GetInstance().GetWorkspaceRoot();
}




namespace
{

static GameEngine::UIManager::RenderRuntimeConfig BuildEditorUiRenderRuntimeConfig()
{
    GameEngine::UIManager::RenderRuntimeConfig cfg{};
    Editor::SettingsStore store = Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
    std::string err;
    (void)store.Load(&err);
    const auto& root = store.Json();
    if (root.is_object())
    {
        const auto itRendering = root.find("rendering");
        if (itRendering != root.end() && itRendering->is_object())
        {
            const auto itHdr = itRendering->find("hdr");
            if (itHdr != itRendering->end() && itHdr->is_object())
            {
                const auto& hdr = *itHdr;
                bool hdrEnabled = false;
                if (hdr.contains("enabled") && hdr["enabled"].is_boolean())
                    hdrEnabled = hdr["enabled"].get<bool>();
                if (hdrEnabled)
                {
                    // Default UI white to the OS "SDR content brightness" (SDR white
                    // level) so SDR chrome matches its SDR-mode brightness in HDR; 0
                    // (unknown) lets the shader fall back to the device paper-white.
                    // A saved uiPaperWhiteNits override below still wins.
                    const float osSdrWhiteNits = Platform::GetPrimaryMonitorInfo().sdrWhiteLevelNits;
                    cfg.HdrUiPaperWhiteNits = osSdrWhiteNits > 0.0f
                        ? std::clamp(osSdrWhiteNits, Rendering::kSdrWhiteToPaperWhiteMinNits, Rendering::kSdrWhiteToPaperWhiteMaxNits)
                        : 0.0f;
                    // UI black-floor LIFT: auto (< 0) derives from the display's
                    // black floor each frame (~0 on OLED, the backlight floor on
                    // LCD). A saved uiBlackLiftNits override below still wins;
                    // an explicit 0 stays a faithful no-lift.
                    cfg.HdrUiBlackLiftNits = -1.0f;
                    if (hdr.contains("uiPaperWhiteNits") && hdr["uiPaperWhiteNits"].is_number())
                        cfg.HdrUiPaperWhiteNits = std::clamp(hdr["uiPaperWhiteNits"].get<float>(), 40.0f, 350.0f);
                    if (hdr.contains("uiBlackLiftNits") && hdr["uiBlackLiftNits"].is_number())
                        cfg.HdrUiBlackLiftNits = std::clamp(hdr["uiBlackLiftNits"].get<float>(),
                                                            GameEngine::UIManager::kHdrUiBlackLiftMinNits,
                                                            GameEngine::UIManager::kHdrUiBlackLiftMaxNits);
                }
            }
        }
    }
    if (IsUiCorrectnessModeForcedForAllWindows())
    {
        cfg.CorrectnessModeEnabled = true;
        cfg.DisableFastPathNoOp = true;
    }
    return cfg;
}

// Window-bootstrap helpers below have external linkage: they are shared with
// EditorApplication_Windows.cpp (see EditorWindowBootstrap.h).
} // namespace

void ApplyUiRuntimeConfig(GameEngine::UIManager* ui)
{
    if (!ui)
        return;
    // Editor UIManagers historically never received a JobSystem, leaving
    // every UI fork-join site (MT-1 pre-measure, MT-3 parallel drain)
    // silently sequential in the editor — only GameUIHost wired one. This
    // is the one config point every editor window's UI passes through.
    ui->SetJobSystem(&EngineCore::GetInstance().GetJobSystem());
    const auto cfg = BuildEditorUiRenderRuntimeConfig();
    Logger::Log::Info("Editor: UI HDR mapping paperWhite={} blackLift={}",
                      cfg.HdrUiPaperWhiteNits,
                      cfg.HdrUiBlackLiftNits);
    ui->ApplyRenderRuntimeConfig(cfg);
}

struct BackgroundToolWindowUiPolicy
{
    bool enabled = false;
    double targetFps = 0.0;
    double minIntervalSeconds = 0.0;
};

static const BackgroundToolWindowUiPolicy& GetBackgroundToolWindowUiPolicy()
{
    static const BackgroundToolWindowUiPolicy policy = []()
    {
        BackgroundToolWindowUiPolicy p{};
        const char* fpsValue = std::getenv("GE_EDITOR_BACKGROUND_TOOL_WINDOW_UI_FPS");
        if (!fpsValue || !*fpsValue)
        {
            return p;
        }

        char* parseEnd = nullptr;
        const double fps = std::strtod(fpsValue, &parseEnd);
        if (parseEnd == fpsValue || fps <= 0.0)
        {
            Logger::Log::Warning(
                "Editor: ignoring GE_EDITOR_BACKGROUND_TOOL_WINDOW_UI_FPS='{}' (expected positive number)",
                fpsValue);
            return p;
        }

        p.enabled = true;
        p.targetFps = fps;
        p.minIntervalSeconds = 1.0 / fps;
        Logger::Log::Info("Editor: background tool-window UI throttle enabled at {:.1f} fps", p.targetFps);
        return p;
    }();
    return policy;
}

static bool IsUiSlotLoggingEnabled()
{
    return Platform::EnvironmentSwitchEnabled("GE_EDITOR_LOG_UI_SLOTS", false);
}

static void MaybeLogUiSlotDiagnostics(const EditorApplication::EditorWindowContext* ctx)
{
    if (!ctx || !ctx->ui || !IsUiSlotLoggingEnabled())
        return;
    static uint64_t s_LogCounter = 0;
    if ((s_LogCounter++ % 120ull) != 0ull)
        return;
    const auto d = ctx->ui->GetRenderSlotDiagnostics();
    Logger::Log::Info(
        "[UI Slots] window={} target={} devFrame={} targetFrame={} uiSlot={} rgSlot={}",
        ctx->windowId,
        d.ActiveWindowTargetId,
        d.DeviceFrameIndex,
        d.TargetFrameIndex,
        d.UiFrameSlot,
        d.RgPoolSlot);
}

void ApplyWindowDescriptor(EditorApplication::EditorWindowContext& ctx,
                                  const EditorApplication::WindowDescriptor& desc,
                                  uint64_t windowId)
{
    ctx.windowId = windowId;
    ctx.role = desc.role;
    ctx.capabilities = desc.capabilities;
    if (!desc.title.empty())
    {
        ctx.title = desc.title;
    }
    if (!desc.panelId.empty())
    {
        ctx.floatingPanelId = desc.panelId;
    }
}

static bool BuildFallbackDockRoot(UIManager& ui)
{
    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    root->AddClass("dock-root");

    auto dock = std::make_unique<DockspaceElement>();
    dock->SetId("dock");
    dock->AddClass("dockspace");
    root->AddChild(std::move(dock));

    ui.SetRoot(std::move(root));
    return ui.GetRootElement() != nullptr;
}

static uint32_t GetEditorAccentColor()
{
    uint32_t accentColor = UI::AccentStyleHelper::kDefaultAccentColor;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    int64_t stored = static_cast<int64_t>(accentColor);
    if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
        accentColor = static_cast<uint32_t>(stored);
    return accentColor;
}

void ApplySavedAccentStyle(UIManager* ui)
{
    if (!ui)
        return;
    uint32_t accentColor = GetEditorAccentColor();
    if (auto accentSheet = UI::AccentStyleHelper::BuildAccentColorStylesheet(accentColor))
        ui->AddStylesheet(accentSheet);

    // Forward accent color to dock overlays
    if (auto* root = ui->GetRootElement())
    {
        if (auto* ds = dynamic_cast<DockspaceElement*>(root->FindById("dock")))
            ds->SetAccentColor(accentColor);
    }
}

void ConfigureEditorUiFonts(UIManager* ui, AssetManager* assetManager, const std::filesystem::path& assetsDirectory)
{
    if (!ui || !assetManager)
        return;

    auto& reg = assetManager->GetRegistry();
    auto* js = &EngineCore::GetInstance().GetJobSystem();

    // Register every shipped UI face before the resolver builds its index.
    // Bold/italic/mono text resolves against these (not the default-font
    // install below); relying on the async asset scan left them out of the
    // resolver's first index build, so e.g. bold fell back to Regular.
    const std::filesystem::path fontsDir = assetsDirectory / "Fonts";
    std::error_code fontsEc;
    for (const auto& entry : std::filesystem::directory_iterator(fontsDir, fontsEc))
    {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".ttf" && ext != ".otf")
            continue;
        if (!reg.IsAssetRegistered(entry.path()))
            reg.RegisterAsset(entry.path());
    }

    // Install the default UI font so first-frame text has a stable
    // proportional fallback while async font loads are still in flight.
    const std::filesystem::path fontPath = fontsDir / "Roboto-Regular.ttf";
    const GUID defaultFontGuid = reg.GetAssetGUID(fontPath);
    const std::vector<uint8_t> defaultFontBytes = Rendering::Utils::ReadFile(fontPath.string().c_str());
    if (!ui->SetDefaultFontBytes(defaultFontBytes, defaultFontGuid))
    {
        Logger::Log::Warning("Editor UI font: failed to install default font '{}'", fontPath.string());
    }

    auto fontResolver = std::make_shared<GameEngine::Engine::UI::FontResolver>(reg, js);
    ui->SetFontResolver(
        [fontResolver](const std::string& family,
                       int weight,
                       FontStyle style,
                       FontVariant variant,
                       UIManager::FontResolveCallback onReady)
        {
            if (!fontResolver || !onReady)
                return;
            fontResolver->ResolveAsync(
                family,
                weight,
                (style == FontStyle::Italic)
                    ? GameEngine::Engine::UI::FontStyle::Italic
                : (style == FontStyle::Oblique) ? GameEngine::Engine::UI::FontStyle::Oblique
                                                : GameEngine::Engine::UI::FontStyle::Normal,
                (variant == FontVariant::SmallCaps) ? GameEngine::Engine::UI::FontVariant::SmallCaps
                                                    : GameEngine::Engine::UI::FontVariant::Normal,
                [onReady = std::move(onReady)](GameEngine::Engine::UI::FontBytes bytes) mutable
                {
                    UIManager::FontResolveResult r{};
                    r.Bytes = std::move(bytes.bytes);
                    r.FaceIndex = bytes.faceIndex;
                    r.DebugName = std::move(bytes.debugName);
                    r.AssetGuid = bytes.assetGuid;
                    onReady(std::move(r));
                });
        });

    // Prefetch theme fonts to avoid first-frame layout shifts.
    ui->RequestFontFamily("Roboto");
}

static bool RegisterEditorAssetSource(AssetManager& assetManager,
                                      const std::filesystem::path& editorAssetsRoot,
                                      const char* contextLabel)
{
    // Editor mount uses derived identity (default true on AssetSourceDesc):
    // GUID = hash(canonical relative path under editorAssetsRoot). No DB,
    // no per-user cache, no tombstones — editor-shipped assets are
    // version-controlled and recreated by the build, so persisting registry
    // state across sessions is unnecessary and historically caused stale
    // entries to accumulate under %LOCALAPPDATA% (see asset-system-rewrite
    // Phase 2). Leaving AuthoritativeDbFile + CacheRoot empty selects this
    // mode.
    AssetSourceDesc editorSource{};
    editorSource.Alias = "editor";
    editorSource.Root = editorAssetsRoot;
    if (!assetManager.RegisterSource(editorSource))
    {
        Logger::Log::Warning("Editor: {} failed to register editor asset source '{}'",
                             contextLabel ? contextLabel : "startup",
                             editorAssetsRoot.string());
        return false;
    }
    return true;
}

static void RefreshEditorUiAssetRefs(AssetManager& assetManager,
                                     const std::filesystem::path& layoutAssetPath,
                                     const std::filesystem::path& styleAssetPath,
                                     GUID& layoutGuid,
                                     GUID& styleGuid,
                                     const char* contextLabel)
{
    const std::filesystem::path resolvedLayoutPath =
        layoutAssetPath.empty() ? std::filesystem::path{} : assetManager.ResolveAssetPath(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    const std::filesystem::path resolvedStylePath =
        styleAssetPath.empty() ? std::filesystem::path{} : assetManager.ResolveAssetPath(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    layoutGuid = layoutAssetPath.empty() ? GUID::Null() : assetManager.ResolveAssetGuid(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    styleGuid = styleAssetPath.empty() ? GUID::Null() : assetManager.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    if (layoutGuid.IsNull() || styleGuid.IsNull())
    {
        Logger::Log::Warning("Editor: {} UI asset refs unresolved (alias='{}', layout='{}' -> '{}' guid={}, style='{}' -> '{}' guid={})",
                             contextLabel ? contextLabel : "refresh-ui-asset-refs",
                             std::string(GameEngine::kAssetSourceAliasEditor),
                             layoutAssetPath.string(),
                             resolvedLayoutPath.string(),
                             layoutGuid.ToString(),
                             styleAssetPath.string(),
                             resolvedStylePath.string(),
                             styleGuid.ToString());
    }
}

bool EnsureEditorThemeStyleAttached(UIManager* ui,
                                           AssetManager* assetManager,
                                           const std::filesystem::path& styleAssetPath,
                                           GUID styleGuid,
                                           const std::string& contextLabel,
                                           std::string_view sourceAlias)
{
    if (!ui || !assetManager)
        return false;

    if (sourceAlias.empty())
    {
        Logger::Log::Warning("Editor: {} requires explicit source alias when attaching theme style",
                             contextLabel.empty() ? std::string("theme-attach") : contextLabel);
        return false;
    }

    GUID resolvedStyleGuid = GUID::Null();
    if (!styleAssetPath.empty())
    {
        resolvedStyleGuid = assetManager->ResolveAssetGuid(styleAssetPath, sourceAlias);
    }

    // Style GUIDs can become stale across project switches (for example when source root
    // ownership changes between project and named source). Always prefer a fresh resolve
    // from the current source selection when available.
    if (!resolvedStyleGuid.IsNull())
    {
        styleGuid = resolvedStyleGuid;
    }

    const std::filesystem::path resolvedStylePath = assetManager->ResolveAssetPath(styleAssetPath, sourceAlias);

    if (styleGuid.IsNull())
    {
        Logger::Log::Warning("Editor: {} cannot attach theme style (null style guid, alias='{}', asset='{}', resolved='{}')",
                             contextLabel,
                             std::string(sourceAlias),
                             styleAssetPath.string(),
                             resolvedStylePath.string());
        return false;
    }

    if (!assetManager->IsAssetLoaded(styleGuid))
    {
        auto h = assetManager->LoadAsset(styleGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);
        if (h.Future.has_value())
        {
            h.Future->wait();
        }
    }

    SharedPtr<Asset> a = assetManager->GetAsset(styleGuid);
    if (!a || a->GetType() != AssetType::UIStyle || !a->IsLoaded() || a->HasFailed())
    {
        Logger::Log::Warning("Editor: {} failed to load theme style asset {} (alias='{}', asset='{}', resolved='{}')",
                             contextLabel,
                             styleGuid.ToString(),
                             std::string(sourceAlias),
                             styleAssetPath.string(),
                             resolvedStylePath.string());
        return false;
    }

    const bool attached = ui->AttachStyleFromAsset(*static_cast<UIStyleAsset*>(a.get()));
    if (!attached)
    {
        Logger::Log::Warning("Editor: {} failed to attach theme style asset {} (alias='{}', asset='{}', resolved='{}')",
                             contextLabel,
                             styleGuid.ToString(),
                             std::string(sourceAlias),
                             styleAssetPath.string(),
                             resolvedStylePath.string());
    }
    else
    {
        Logger::Log::Info("Editor: {} attached theme style asset {} from alias='{}' resolved='{}'",
                          contextLabel,
                          styleGuid.ToString(),
                          std::string(sourceAlias),
                          resolvedStylePath.string());
    }
    return attached;
}

static WindowUiBootstrapRequest BuildEditorWindowUiBootstrapRequest(UIManager* ui,
                                                                    AssetManager* assets,
                                                                    const GUID& layoutGuid,
                                                                    const GUID& styleGuid,
                                                                    const std::string& contextLabel)
{
    WindowUiBootstrapRequest req{};
    req.ui = ui;
    req.assetManager = assets;
    req.layoutGuid = layoutGuid;
    req.styleGuid = styleGuid;
    req.contextLabel = contextLabel;
    req.fallbackRootBuilder = [](UIManager& m) { return BuildFallbackDockRoot(m); };
    return req;
}

WindowUiBootstrapResult BootstrapEditorWindowUi(UIManager* ui,
                                                       AssetManager* assets,
                                                       const GUID& layoutGuid,
                                                       const GUID& styleGuid,
                                                       const std::string& contextLabel)
{
    return WindowUiBootstrap::Run(
        BuildEditorWindowUiBootstrapRequest(ui, assets, layoutGuid, styleGuid, contextLabel));
}

namespace
{

static void PumpEditorWindowUiBootstrap(EditorApplication::EditorWindowContext* ctx)
{
    if (!ctx || !ctx->ui || !ctx->uiBootstrapAsync.has_value())
    {
        return;
    }

    const WindowUiBootstrapResult result = WindowUiBootstrap::TickAsync(*ctx->uiBootstrapAsync);
    if (WindowUiBootstrap::IsAsyncComplete(*ctx->uiBootstrapAsync))
    {
        if (!result.hasRoot)
        {
            Logger::Log::Warning("Editor: window {} completed async UI bootstrap without a root", ctx->windowId);
        }
        ctx->uiBootstrapAsync.reset();
    }
}

static bool EnsureUiRootReadyForRender(const EditorApplication::EditorWindowContext* ctx, const char* phaseLabel)
{
    if (!ctx || !ctx->ui)
        return false;
    if (ctx->ui->GetRootElement())
        return true;

    static std::unordered_set<uint64_t> s_WarnedWindowIds;
    if (s_WarnedWindowIds.insert(ctx->windowId).second)
    {
        Logger::Log::Warning("Editor: window {} has no UI root during {}. UI render skipped.",
                             ctx->windowId,
                             phaseLabel ? phaseLabel : "render");
    }
    return false;
}

static bool BeginWindowRenderFrame(EditorApplication::EditorWindowContext* ctx,
                                   Rendering::IDevice* dev)
{
    if (!ctx || !ctx->renderCtx || !dev)
        return false;

    if (!ctx->renderCtx->ActivateWindowTarget())
    {
        return false;
    }

    if (!dev->BeginFrame())
    {
        // BeginFrame returns false on three independent acquire conditions:
        //   1. VK_TIMEOUT (16ms) — no swapchain image was free in time (e.g.
        //      compositor briefly held all 3 images for vsync). Transient;
        //      next frame's acquire will succeed.
        //   2. VK_ERROR_OUT_OF_DATE_KHR — swapchain is stale. The device layer
        //      (AcquireNextImage in VulkanDevice.cpp) already recreates the
        //      swapchain internally before returning false.
        //   3. VK_SUBOPTIMAL_KHR — same: device layer already recreated.
        //
        // Previously this site unconditionally recreated the swapchain. On (1)
        // that was a destructive false-positive: ~70ms recreate work made the
        // next acquire ALSO time out (new images not yet enqueued for
        // compositor display), and the editor wedged at ~12 FPS in a recreate
        // loop. On (2)/(3) the device had already recreated, so the editor's
        // recreate was a redundant second pass.
        //
        // Right behavior: skip this frame; trust the device's internal
        // OUT_OF_DATE/SUBOPTIMAL handling to have recreated when warranted.
        return false;
    }

    ++ctx->deviceFrameStamp; // RenderGraph BeginFrame pairing tripwire (see header)
    return true;
}

template <typename TPanel>
static TPanel* ResolveMountedPanelForWindow(EditorApplication::EditorWindowContext* ctx, const char* mountId)
{
    if (!ctx || !ctx->ui || !mountId)
        return nullptr;

    UIElement* root = ctx->ui->GetRootElement();
    if (!root)
        return nullptr;

    if (auto* mount = dynamic_cast<Mount*>(root->FindById(mountId)))
    {
        return dynamic_cast<TPanel*>(mount->GetTarget());
    }
    return nullptr;
}

// 7f post-spine bind: re-point "game_main" + the game viewport element at the
// game view's pipeline-output physical — every frame, never cached (FinalCopy
// elision moves the physical with PP toggles). The no-camera frame binds the
// clear pass's stashed pooled physical instead.
static void BindGameViewRG(EditorApplication::EditorWindowContext* ctx,
                            Rendering::RenderGraph::RGFrame& frame, GameViewPanel* gvPanel,
                            bool bindViewportBackground, bool pureRGFrame)
{
    if (!ctx || !ctx->ui || !ctx->gameView || !gvPanel)
        return;
    // MAIN RS (8e): floating windows carry private RS objects; the pipeline
    // outputs live on the RS the controllers were constructed on.
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* device = rs->GetDevice();
    if (!device)
        return;

    UIElement* viewport = gvPanel->GetViewportElement();

    Rendering::TextureHandle physical{};
    Rendering::RenderGraph::RGTexture rg2Tex{};
    uint32_t texW = 0, texH = 0;
    // Every arm below assigns the space its producer stamped; the initializer
    // is unread (the invalid-physical arm unbinds without stating a space).
    UI::UITextureSpace space = UI::UITextureSpace::DisplayLinearSdr();
    if (ctx->gameView->IsWaitingForExtraction())
    {
        // Invalid across a device rebuild: this arm never reaches the snapshot's
        // Update, so the accessor's generation compare is the only thing between
        // a freed id and ImportExternalTexture.
        physical = ctx->gameView->GetLastPresentedTexture(*device);
        texW = ctx->gameView->GetLastPresentedWidth();
        texH = ctx->gameView->GetLastPresentedHeight();
        // The cached copy keeps the space of the frame that rendered it —
        // never the current mode.
        space = ctx->gameView->GetLastPresentedSpace();
        if (physical.IsValid())
        {
            rg2Tex = frame.ImportExternalTexture(
                "Editor.GameView.LastPresented", physical,
                Rendering::ResourceState::ShaderResource,
                device->GetTextureFormat(physical));
        }
    }
    else if (!ctx->gameView->HadCameraThisFrame())
    {
        physical = ctx->gameView->GetNoCameraPhysicalRG();
        rg2Tex = ctx->gameView->GetNoCameraColorRG(frame);
        texW = ctx->gameView->GetNoCameraWidthRG();
        texH = ctx->gameView->GetNoCameraHeightRG();
        // The clear writes a display-referred grey constant; that value never
        // varies with the output mode.
        space = UI::UITextureSpace::DisplayLinearSdr();
    }
    else if (ctx->gameView->GetPixelPerfectOutputRG(frame, rg2Tex, physical, texW, texH, space))
    {
        // Smooth pixel-perfect (8e-5): the UI samples the full-panel upscale
        // output, not the padded reference RT the pipeline rendered. The
        // upscale carries whatever its source held — the finalized view on the
        // encoded chain, linear light on the declining one — and `space` says
        // which, so nothing here has to infer it from the F16 format.
    }
    else
    {
        const auto out = rs->GetPipelineOutputRG(frame, ctx->gameView->GetViewId());
        if (out.IsValid())
        {
            const auto& desc = frame.Graph().ResourceDesc(out.Out.Id);
            if (desc.SampleCount > 1)
            {
                static bool sWarnedGameMsaa = false;
                if (!sWarnedGameMsaa)
                {
                    sWarnedGameMsaa = true;
                    Logger::Log::Warning(
                        "Editor RenderGraph: game view pipeline output is multisampled — "
                        "'game_main' skipped (no resolve in the chain?)");
                }
            }
            else
            {
                physical = out.Physical;
                rg2Tex = out.Out;
                texW = desc.Width;
                texH = desc.Height;
                space = rs->GetPipelineOutputSpaceRG();
                // The controller's post-pipeline chain finished this view: its
                // own finalize with the HUD composited on the encoded bytes, or
                // the pipeline output it started from when the finalize
                // declined. `physical` deliberately stays the PIPELINE's — the
                // finalize writes a transient, which has no handle until
                // Execute, and the arms below only need one to prove the view
                // rendered this frame.
                //
                // Pure frames only, as the scene view gates its own finalize
                // (SceneViewRenderCoordinator::BindRG): a hybrid frame binds
                // `physical`, which is the UNfinalized pipeline output, so
                // taking the finished view's SPACE here would stamp encoded
                // bytes onto an image that holds linear light and the composite
                // would treat the curve as light.
                if (pureRGFrame)
                    ctx->gameView->GetFinishedViewRG(frame, rg2Tex, space);
            }
        }
        else
        {
            static bool sWarnedGameNoOutput = false;
            if (!sWarnedGameNoOutput)
            {
                sWarnedGameNoOutput = true;
                Logger::Log::Warning(
                    "Editor RenderGraph: GetPipelineOutputRG returned nothing for the game view — "
                    "'game_main' will not render this frame");
            }
        }
    }

    if (!physical.IsValid())
    {
        ctx->ui->RemoveExternalTexture("game_main");
        if (viewport)
            ctx->ui->ClearElementBackgroundTexture(*viewport);
        gvPanel->SetNoCameraOverlayVisible(!ctx->gameView->HadCameraThisFrame());
        return;
    }

    if (!ctx->gameView->IsWaitingForExtraction() && rg2Tex.IsValid())
        ctx->gameView->UpdatePresentedSnapshotRG(frame, rg2Tex, space);

    if (pureRGFrame && rg2Tex.IsValid())
    {
        // Pure frame: the UI pass declares the read in the same graph. The
        // registration's format is the one the resource was declared (or
        // imported) with, whichever of the four arms above produced rg2Tex.
        ctx->ui->SetExternalTextureRG(
            "game_main", texW, texH, space,
            static_cast<Rendering::TextureFormat>(frame.Graph().ResourceDesc(rg2Tex.Id).Format));
        ctx->ui->PublishExternalTextureRG("game_main", frame, rg2Tex);
    }
    else
    {
        ctx->ui->SetExternalTexture("game_main", physical, texW, texH, space);
    }
    if (bindViewportBackground && viewport)
    {
        // Rebind only when the binding actually changes — the unconditional
        // Reset/Set cycle dirtied the viewport element every rendered frame
        // (StyleOverrides::Reset erases a present override), keeping the UI
        // tree perpetually dirty via sibling-combinator invalidation. Same
        // fix as SceneViewRenderCoordinator::BindRG.
        const auto currentBg = viewport->Overrides().Get(Style::BackgroundImage);
        const bool alreadyBound = currentBg.has_value() &&
                                  currentBg->Kind == BackgroundImageSource::SourceKind::ResourceName &&
                                  currentBg->Value == "game_main" &&
                                  !viewport->HasBackgroundImageTextureOverride();
        if (!alreadyBound)
        {
            ctx->ui->ClearElementBackgroundTexture(*viewport);
            viewport->Styles()
                .SetBackgroundResourceName("game_main")
                .SetBackgroundRepeat(BackgroundRepeat::NoRepeat)
                .SetBackgroundSizeCover()
                .SetBackgroundPositionPercent(50.0f, 50.0f)
                .ResetBackgroundTint();
        }
    }
    gvPanel->SetNoCameraOverlayVisible(!ctx->gameView->HadCameraThisFrame());
}

// Frame routing, computed ONCE per window frame in RenderWindowFrame: which
// views declare into the RenderGraph frame this frame, and their render sizes.
struct WindowFrameRouting
{
    // Scene/game views declare into the RenderGraph frame (slices 7d/7f semantics).
    bool rg2Scene = false;
    bool rg2Game = false;
    GameViewPanel* gvPanel = nullptr;
    bool forceGameViewRecord = false;
    // D5 (8e-8): mounted game tab whose viewport is degenerate (collapsed /
    // zero-px) — the RenderGraph arm quiesces the view instead of recording it.
    bool gameDegenerate = false;
    uint32_t gvW = 1;
    uint32_t gvH = 1;
    // The editor is RenderGraph-only: every frame imports the backbuffer, declares the
    // UI composite, and runs one terminal encode. Always true; retained so the
    // scene/game bind paths can branch on the pure-frame publish convention.
    bool pure = false;
};

static bool RecordWindowWorldViews(EditorApplication::EditorWindowContext* ctx,
                                   bool bindViewportBackground,
                                   const WindowFrameRouting& routing,
                                   float deltaTimeSeconds,
                                   Editor::SceneThumbnailCapture* thumbnailCapture = nullptr,
                                   Rendering::RenderGraph::RGFrame* rg2Frame = nullptr)
{
    if (!ctx || !ctx->ui || !ctx->capabilities.worldRender)
        return false;

    bool sceneViewportReady = false;
    // RenderGraph arm (slices 7d + 7f; quad since 8e-3; pixel-perfect since 8e-5;
    // movie tickets since 8e-6): scene panes and the game view declare into
    // the RenderGraph frame and the DRIVER makes the single spine call carrying the
    // frame's COMPLETE view set; the UI re-points at each view's
    // pipeline-output value after it.
    const bool useRGScene = rg2Frame && routing.rg2Scene;
    GameViewPanel* gvPanel = routing.gvPanel;
    const uint32_t gvW = routing.gvW;
    const uint32_t gvH = routing.gvH;
    const bool useRGGame = useRGScene && routing.rg2Game;

    bool gameRGHandled = false;
    if (useRGScene)
    {
        std::vector<Engine::Renderer::Pipeline::ViewTargetsRG> targets;
        // Scene first: targets[0] is the scene entry (DeclareOverlaysRG
        // indexes it).
        const bool sceneDeclared =
            Editor::SceneViewRenderCoordinator::CollectRG(ctx, *rg2Frame, targets);
        bool gameDeclared = false;
        if (useRGGame)
        {
            Engine::Renderer::Pipeline::ViewTargetsRG gameVt{};
            ECS::World* gameWorld = EngineCore::GetInstance().EnsurePrimaryWorld();
            gameDeclared = ctx->gameView->DeclareTargetsRG(*rg2Frame, gvW, gvH, ctx->windowId,
                                                            gameWorld, gameVt);
            // No-camera frames declare a clear pass but push NO view.
            if (gameDeclared && gameVt.View != 0)
                targets.push_back(gameVt);
            gameRGHandled = true;
        }
        else if (routing.gameDegenerate && ctx->gameView)
        {
            // D5 (8e-8): mounted-but-degenerate game viewport — stop declaring
            // instead of recording. The view is NOT released (D9/A1.6): it stops
            // being re-armed, so its OnDemand participation lapses to zero within
            // two frames (extraction/culling/shadows gate off at no per-frame
            // cost) while the viewId + HZB history survive for an instant warm
            // reopen; release happens only when the controller is destroyed.
            // Keep the RG registration: publications are frame-stamped, and
            // removing it here makes the first reactivated UI frame generate an
            // unresolved background before the new game frame can publish.
            gameRGHandled = true;
        }
        // 8e: the spine is declared on EVERY RenderGraph-arm frame, even with an
        // empty targets span — BuildFrameGraph stamps FrameRG() for this
        // frame stream regardless, which is what lets a panel-less tool
        // window's thumbnails dispatch. Always the MAIN RS: floating windows
        // carry private RS objects the controllers were never built on.
        {
            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                Engine::Renderer::RenderServices::FrameGraphBuildParamsRG params{};
                params.ViewTargets = std::span<const Engine::Renderer::Pipeline::ViewTargetsRG>(
                    targets.data(), targets.size());
                params.BuildWorldDrawListsIfReady = true;
                params.DeltaTime = deltaTimeSeconds;
                rs->Spine().BuildFrameGraph(*rg2Frame, params);
                if (sceneDeclared)
                {
                    Editor::SceneViewRenderCoordinator::DeclareOverlaysRG(ctx, *rg2Frame,
                                                                           params.ViewTargets);
                    Editor::SceneViewRenderCoordinator::BindRG(ctx, *rg2Frame, routing.pure);
                    // Scene-save thumbnail (8c-4): ticket readback of the scene
                    // view's pipeline output — overlays included, the view's own
                    // finalize not, so the PNG writer applies the curve and its
                    // own 8-bit filters rather than inheriting the screen's
                    // 10-bit ones. Same image content, filtered for the file's
                    // depth instead of the display's. targets[0] is the MAIN
                    // scene entry when it declared (8e-3 quad contract); if the
                    // perspective pane is collapsed in quad, capture frames
                    // produce nothing — accepted parity, do NOT "fix" this by
                    // capturing the active pane.
                    if (thumbnailCapture && thumbnailCapture->WantsCapture() && !targets.empty() &&
                        ctx->scene && targets[0].View == ctx->scene->GetViewId())
                        thumbnailCapture->BeginReadbackRG(
                            *rg2Frame, static_cast<uint32_t>(targets[0].View), rs);
                    sceneViewportReady = true;
                }
                if (gameDeclared)
                {
                    // Post-pipeline declares (8e-5 PP upscale, 8e-6 movie) —
                    // consume GetPipelineOutputRG, so AFTER the spine and
                    // BEFORE the bind; panel-less movie frames run this too.
                    // The presented format is resolved from THIS window's own
                    // target handle, per frame — never from the device's
                    // active-target ambient and never cached across frames
                    // (swapchain recreates happen between frames).
                    if (!ctx->gameView->IsWaitingForExtraction())
                    {
                        Rendering::IDevice* dev = ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr;
                        const Rendering::TextureFormat presentedFormat =
                            dev ? dev->GetWindowTargetSwapchainFormat(ctx->renderCtx->GetWindowTarget())
                                : Rendering::TextureFormat::Unknown;
                        ctx->gameView->DeclarePostPipelinePassesRG(*rg2Frame, ctx->windowId,
                                                                   presentedFormat);
                    }
                    if (gvPanel)
                        BindGameViewRG(ctx, *rg2Frame, gvPanel, bindViewportBackground,
                                        routing.pure);
                }
            }
        }
    }

    if (ctx->gameView && !gameRGHandled)
    {
        // No RenderGraph game declare this frame (no panel, not movie-recording,
        // view not renderable): stop declaring. The view is
        // NOT released (D9/A1.6) — it stops being re-armed, so its OnDemand
        // participation lapses to zero within two frames while the viewId + HZB
        // history survive for an instant warm reopen; release happens only when
        // the controller is destroyed. Preserve the external RG registration so
        // a later tab activation generates a textured primitive in the same frame;
        // its old publication cannot be sampled because publications are stamped
        // with the producing RG frame and frame index.
    }

    return sceneViewportReady;
}

static void EnsureWorldHasPhysicsSettingsFromProject(GameEngine::ECS::World& world, const std::filesystem::path& workspaceRoot)
{
    using namespace GameEngine;
    const Physics::PhysicsWorldSettings s = Editor::PhysicsProjectSettings::Load(workspaceRoot);
    (void)PhysicsECS::UpsertPhysicsWorldSettingsComponent(world, s, /*createIfMissing=*/true);
}
} // namespace

std::unique_ptr<DockNode> CloneDockNode(const DockNode* src)
{
    if (!src)
        return nullptr;

    if (src->IsLeaf())
    {
        auto out = DockNode::MakeLeaf();
        for (const auto& tab : src->GetTabs())
        {
            out->AddTab(tab);
        }
        const std::string& active = src->GetActivePanelId();
        if (!active.empty())
        {
            (void)out->ActivateTab(active);
        }
        return out;
    }

    if (src->IsSplit())
    {
        auto out = std::make_unique<DockNode>();
        auto first = CloneDockNode(src->First());
        auto second = CloneDockNode(src->Second());
        out->SetSplit(src->GetSplitDirection(), src->GetSplitRatio(), std::move(first), std::move(second));
        out->SetMinChildSizes(src->GetMinFirstPx(), src->GetMinSecondPx());
        return out;
    }

    return nullptr;
}

std::unique_ptr<DockNode> EditorApplication::CloneCurrentDockLayout() const
{
    return m_Docking && m_Docking->GetRoot() ? CloneDockNode(m_Docking->GetRoot()) : nullptr;
}

void EditorApplication::SetDockLayoutFromClone(const DockNode* src)
{
    if (m_Docking && src)
        m_Docking->SetRoot(CloneDockNode(src));
}

static AnimationWindowPanel* FindAnimationPanelOfKind(const std::vector<std::unique_ptr<UIElement>>& panels,
                                                      AnimationWindowPanel::PanelKind kind)
{
    for (const auto& p : panels)
    {
        if (!p)
            continue;
        if (auto* casted = dynamic_cast<AnimationWindowPanel*>(p.get()))
        {
            if (casted->GetPanelKind() == kind)
                return casted;
        }
    }
    return nullptr;
}

template <typename Fn>
static void ForEachAnimationPanel(const std::vector<std::unique_ptr<UIElement>>& panels, Fn&& fn)
{
    for (const auto& p : panels)
    {
        if (!p)
            continue;
        if (auto* casted = dynamic_cast<AnimationWindowPanel*>(p.get()))
            fn(casted);
    }
}

template <typename T>
static T* FindFirstPanelOfType(const std::vector<std::unique_ptr<UIElement>>& panels)
{
    for (const auto& p : panels)
    {
        if (!p)
            continue;
        if (auto* casted = dynamic_cast<T*>(p.get()))
            return casted;
    }
    return nullptr;
}

static void BootstrapEditorWorld(ECS::World& world, Engine::Renderer::RenderServices* rs)
{
    using namespace Components;

    if (!rs)
        return;

    Engine::Renderer::PrimitiveGenerator::RegisterAll(*rs);

    const GUID cubeGuid = Engine::Renderer::PrimitiveGenerator::CubeGuid();
    const GUID matGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
    const auto meshHandle = rs->GetMeshGPURegistry().FindHandle(
        Rendering::MeshGPUKey{cubeGuid, 0});

    // Simple root entity with a transform and mesh renderer
    ECS::Entity rootEntity = world.Create();
    ECS::EntityHandle rootHandle = rootEntity.GetHandle();

    Mathematics::Vector3 rootPos(0.0f, 0.0f, 0.0f);
    Mathematics::Quaternion rootRot{};
    Mathematics::Vector3 rootScale(1.0f, 1.0f, 1.0f);
    Transform rootTransform = Transform::FromTRS(rootPos, rootRot, rootScale);
    rootEntity.Set(rootTransform);
    {
        MeshRenderer renderer{};
        renderer.meshGpuHandleId = meshHandle.IsValid() ? static_cast<uint64>(meshHandle) : 0;
        renderer.materialAssetGuid.Set(matGuid);
        renderer.renderLayerMask = 1u; // Scene View layer only
        rootEntity.Set(renderer);
    }

    Name rootName{};
    const char rootLabel[] = "Root";
    for (size_t i = 0; i < sizeof(rootLabel) && i < sizeof(rootName.value); ++i)
    {
        rootName.value[i] = rootLabel[i];
    }
    rootEntity.Set(rootName);

    // Child entity 1
    ECS::Entity child1 = world.Create();
    Mathematics::Vector3 child1Pos(2.0f, 0.0f, 0.0f);
    Transform child1Transform = Transform::FromTRS(child1Pos, rootRot, rootScale);
    child1.Set(child1Transform);
    {
        MeshRenderer renderer{};
        renderer.meshGpuHandleId = meshHandle.IsValid() ? static_cast<uint64>(meshHandle) : 0;
        renderer.materialAssetGuid.Set(matGuid);
        renderer.renderLayerMask = 1u; // Scene View layer only
        child1.Set(renderer);
    }
    child1.Set(Parent{rootHandle});

    Name child1Name{};
    const char child1Label[] = "Child 1";
    for (size_t i = 0; i < sizeof(child1Label) && i < sizeof(child1Name.value); ++i)
    {
        child1Name.value[i] = child1Label[i];
    }
    child1.Set(child1Name);

    // Child entity 2
    ECS::Entity child2 = world.Create();
    Mathematics::Vector3 child2Pos(0.0f, 2.0f, 0.0f);
    Transform child2Transform = Transform::FromTRS(child2Pos, rootRot, rootScale);
    child2.Set(child2Transform);
    {
        MeshRenderer renderer{};
        renderer.meshGpuHandleId = meshHandle.IsValid() ? static_cast<uint64>(meshHandle) : 0;
        renderer.materialAssetGuid.Set(matGuid);
        renderer.renderLayerMask = 1u; // Scene View layer only
        child2.Set(renderer);
    }
    child2.Set(Parent{rootHandle});

    Name child2Name{};
    const char child2Label[] = "Child 2";
    for (size_t i = 0; i < sizeof(child2Label) && i < sizeof(child2Name.value); ++i)
    {
        child2Name.value[i] = child2Label[i];
    }
    child2.Set(child2Name);

    world.ProcessCommands();
}

static bool ResolveNativePluginEnabled(std::string_view pluginId, bool defaultEnabled)
{
    if (pluginId.empty())
        return false;

    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return defaultEnabled;

    Editor::SettingsStore projectSettings = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)projectSettings.Load(&err);

    bool enabled = defaultEnabled;
    const std::string baseKey = std::string("editor.features.") + std::string(pluginId);
    (void)projectSettings.TryGetBool(baseKey, enabled);
    (void)projectSettings.TryGetBool(baseKey + ".enabled", enabled);
    return enabled;
}

static void ConfigureNativePluginResolvers()
{
    // Editor plugins now arrive exclusively through Editor-kind package
    // modules (EditorSDK); they self-register at DLL load.
    Plugins::EnginePluginRegistry::Get().SetEnabledResolver(ResolveNativePluginEnabled);
    Editor::EditorPluginRegistry::Get().SetEnabledResolver(ResolveNativePluginEnabled);
}

Engine::Renderer::RenderServices* EditorApplication::MainRenderServices() const
{
    // EngineCore's RS is the one passed to EnableRenderingLoop — the main
    // window's renderCtx RS. Floating windows carry PRIVATE RS objects; the
    // RenderGraph declaration paths must never resolve those (controllers were
    // constructed on the main RS).
    auto* rs = EngineCore::GetInstance().GetRenderServices();
#if defined(_DEBUG)
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->renderCtx)
        assert(rs == m_Windows[0]->renderCtx->GetRenderServices() &&
               "EngineCore RS must be window[0]'s RS");
#endif
    return rs;
}

EditorApplication::EditorApplication(const ApplicationConfig& config)
    : Application(config)
{
    // Core editor services (single-document for now).
    m_UndoRedo = std::make_unique<GameEngine::Editor::UndoRedoService>();
    m_ChangeNotifications = std::make_unique<GameEngine::Editor::EditorChangeNotifications>();
    // The editor's own Scene View tools come first in the tool strip.
    Editor::RegisterBuiltInSceneViewTools();
    m_MarkupBridge = Editor::CreateMarkupEditorBridge(*m_ChangeNotifications,
        [this]() { return m_SceneEditor ? m_SceneEditor->GetActiveScenePath() : std::nullopt; });
    m_MarkupBridge->SetSceneViewProvider([this]() -> SceneViewController* {
        return (!m_Windows.empty() && m_Windows[0]->scene) ? m_Windows[0]->scene.get() : nullptr;
    });
    m_MarkupBridge->SetPlayModeProvider([this]() { return m_PlayMode && m_PlayMode->IsPlayingOrPaused(); });
    Editor::InstallSceneViewEntityLinks([this]() -> SceneViewController* {
        return !m_Windows.empty() ? m_Windows[0]->scene.get() : nullptr;
    });
    m_SceneEditor = std::make_unique<GameEngine::Editor::SceneEditorController>();
    // Manual save reads the same write inhibit the autosave tick does
    // (IsPlayModeBlockingSceneWrites), because both protect the authored .scene file from the
    // in-place simulated world. Wired at construction; the controller holds the probe until
    // AttachToRoot builds the document.
    m_SceneEditor->SetPlayModeProbe([this]() { return IsPlayModeBlockingSceneWrites(); });
    m_SceneEditor->SetEagerScenePersistence(GetConfig().PersistScenesEagerly);
    // The untitled-scene save prompt waits for the picker to close and for a
    // queued startup scene to be requested.
    m_SceneEditor->SetBootFlowActiveProbe(
        [this]() { return ShouldIdleWorldPathForProjectPicker() || !m_DeferredStartupScene.empty(); });
    // The recipe controllers' generated entities are scoped to the open scene
    // (RecipeControllers::ReleaseSceneScoped). Release them at the swap, while the
    // outgoing world is still the one they describe. Wired here rather than at the
    // panel-construction site because a scene can be swapped with no panel in
    // play (--scene, MCP open_scene).
    m_SceneEditor->SetSceneScopedGeneratorRelease([this]()
    {
        auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (!world)
            return;
        if (m_RecipeControllers)
            m_RecipeControllers->ReleaseSceneScoped(*world, MainRenderServices());
    });
    Editor::RegisterAssetPipelineCommands([this]() -> std::optional<std::filesystem::path>
    {
        return m_SceneEditor ? m_SceneEditor->GetActiveScenePath() : std::nullopt;
    });
    m_SceneThumbnailCapture = std::make_unique<GameEngine::Editor::SceneThumbnailCapture>();
    m_VcsUi = std::make_unique<EditorVersionControlUi>();
    m_VcsUi->SetScenePathProvider([this]() -> std::optional<std::filesystem::path>
    {
        if (!m_SceneEditor)
            return std::nullopt;
        const auto scenePath = m_SceneEditor->GetActiveScenePath();
        if (!scenePath)
            return std::nullopt;
        // The active scene path comes from the registry, which case-folds
        // asset paths on Windows/macOS, while version control resolves against
        // the real repository root and std::filesystem::relative compares
        // lexically. A folded path therefore relativizes to ../../.. outside
        // the repository and every provider query fails, silently and
        // completely. canonical() restores the on-disk spelling.
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::canonical(*scenePath, ec);
        if (ec)
            return scenePath;
        return resolved;
    });

    // Q6 slice 6: wire the device-loss surfacing actions (invoked from the native
    // save-and-restart dialog on an unrecoverable device loss; all run on the main
    // thread). SaveAll is synchronous for a titled scene (file I/O, no GPU); an
    // untitled scene cannot prompt Save-As on a dead device, so it is skipped.
    m_DeviceLossActions.SaveAll = [this]()
    {
        if (m_SceneEditor && m_SceneEditor->IsSceneDirty())
            m_SceneEditor->RequestSaveScene();
    };
    m_DeviceLossActions.RestartAndExit = [this]()
    {
        const std::filesystem::path exe = Platform::GetExecutablePath();
        if (!exe.empty())
            Platform::LaunchDetached(exe, m_RelaunchArgs);
        else
            Logger::Log::Error("DeviceLossSurfacer: could not resolve executable path; cannot restart");
        RequestExit();
    };
    m_DeviceLossActions.ExitApp = [this]() { RequestExit(); };
    m_SceneEditor->SetOnAfterSave([this](const std::filesystem::path& scenePath)
    {
        if (m_SceneThumbnailCapture)
            m_SceneThumbnailCapture->RequestCapture(scenePath);
        if (m_VcsUi)
            m_VcsUi->RefreshSceneDecorations();
        if (const ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            m_MarkupBridge->OnSceneSaved(*world);
    });
    m_PlayMode = std::make_unique<GameEngine::Editor::PlayModeManager>();
    m_DownloadManager = std::make_unique<PolyhavenDownloadManager>();
    m_DownloadManager->SetOnDownloadFinished([this]()
    {
        for (auto& panel : m_PanelStorage)
            if (auto* ap = dynamic_cast<AssetsPanel*>(panel.get()))
                ap->InvalidatePolyhavenDownloadCache();
    });
    m_DownloadManager->SetOnEntityCreated([this](ECS::EntityHandle entity)
    {
        if (!entity.IsValid())
            return;
        // Select the new model root so the Inspector shows mesh/materials. Use
        // SceneViewController::OnEntityPicked — not HierarchyPanel::SelectEntity alone:
        // hierarchy rebuild is deferred on WorldStructureChanged (PostAction), so a tree
        // search can run before the provider updates and never fires the selection callback,
        // leaving the Inspector on the destroyed placeholder UI.
        SceneViewController* scene =
            (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->scene) ? m_Windows[0]->scene.get() : nullptr;
        if (scene)
            scene->OnEntityPicked(entity);
        else if (InspectorPanel* inspectorPanel = FindFirstPanelOfType<InspectorPanel>(m_PanelStorage))
        {
            ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
            inspectorPanel->ShowEntity(world, entity, /*force=*/true, /*keepMultiSelection=*/false);
        }
    });
    if (m_UndoRedo)
    {
        // Keep native menu labels (Undo/Redo <name>) up to date.
        m_UndoRedo->SetOnHistoryChanged([this]()
                                        {
                                            if (m_EditorToolbar)
                                            {
	                                                m_EditorToolbar->UpdateUndoRedoTitles();
                                            } });
        // Resolve any in-flight deferred scene build before ANY undoable edit,
        // undo, or redo captures/applies a world snapshot — a mid-pump snapshot
        // would bake unresolved (meshGpuHandleId==0) entities into history and a
        // later undo/redo would re-hide them permanently.
        m_UndoRedo->SetBeforeMutation([this]()
                                      {
                                          if (m_SceneEditor && m_SceneEditor->IsSceneBuildInProgress())
                                              m_SceneEditor->DrainSceneBuild();
                                      });
    }
    if (m_PlayMode)
    {
        m_PlayMode->SetWorld(EngineCore::GetInstance().EnsurePrimaryWorld());
        m_PlayMode->SetSceneEditor(m_SceneEditor.get());
        m_PlayMode->SetUndoRedo(m_UndoRedo.get());
        m_PlayMode->SetChangeNotifications(m_ChangeNotifications.get());

        // If the managed scripting domain unloads/swaps while Play Mode is active (or paused),
        // invalidate cached managed export pointers immediately to avoid calling stale pointers.
        // Registered through the GE_ ABI so it reaches GameEngine.Native.dll's
        // ScriptingService — the module where GE_ScriptsDomainUnload actually fires
        // the notification. Setting it on the EXE's inline-static ScriptingService
        // copy never fired (per-module singletons since Engine went into the DLL).
#if GE_ENABLE_SCRIPTING
        GE_SetOnDomainWillUnload(
            [](GE_DomainHandle /*domain*/, void* user)
            {
                auto* self = static_cast<EditorApplication*>(user);
                if (self->m_PlayMode)
                    self->m_PlayMode->InvalidateManagedPlayModeBindings();
            },
            this);
#endif

        m_PlayMode->SetOnStateChanged([this](GameEngine::Editor::PlayModeState s)
                                      {
                                          this->UpdatePlayModeToolbar(s);

                                          if (auto* hierarchyPanel = FindFirstPanelOfType<HierarchyPanel>(m_PanelStorage))
                                          {
                                              if (s == GameEngine::Editor::PlayModeState::EnteringPlay ||
                                                  s == GameEngine::Editor::PlayModeState::ExitingPlay)
                                              {
                                                  hierarchyPanel->StashSelectionForPlayModeTransition();
                                              }
                                              else if (s == GameEngine::Editor::PlayModeState::Play ||
                                                       s == GameEngine::Editor::PlayModeState::Edit ||
                                                       s == GameEngine::Editor::PlayModeState::ChangeReview)
                                              {
                                                  // Defer until after WorldStructureChanged refresh PostActions.
                                                  hierarchyPanel->PostAction([hierarchyPanel]()
                                                                             {
                                                                                 hierarchyPanel->RestoreStashedSelectionAfterPlayModeTransition();
                                                                             });
                                              }
                                          }

                                          // When leaving play mode, the snapshot restore writes directly to ECS
                                          // without emitting per-component notifications. Force the inspector to
                                          // rebuild so it picks up the restored values.
                                          if (s == GameEngine::Editor::PlayModeState::Edit ||
                                              s == GameEngine::Editor::PlayModeState::ChangeReview)
                                          {
                                              if (auto* ip = FindFirstPanelOfType<InspectorPanel>(m_PanelStorage))
                                              {
                                                  auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                                                  ip->ShowEntity(world, ip->GetInspectedEntity(), /*force=*/true);
                                              }
                                          }
                                      });
    }

    // Allow disabling the native toolbar to bisect shutdown issues
    const char* env = std::getenv("EDITOR_DISABLE_NATIVE_TOOLBAR");
    if (env)
    {
        std::string v(env);
        for (auto& c : v)
            c = (char)std::tolower((unsigned char)c);
        if (v == "1" || v == "true" || v == "yes" || v == "on")
        {
            m_EnableNativeToolbar = false;
        }
    }

    // Optional perf dump for headless/RelWithDebInfo runs (no hotkeys required).
    // Usage: set GE_PERF_DUMP_EVERY_N=<N> to log once every N frames.
    // Set GE_PERF_DUMP_SYNC=1 to also log per-frame sync timings (acquire/wait/present).
    static const int s_PerfDumpEveryN = []()
    {
        if (const char* e = std::getenv("GE_PERF_DUMP_EVERY_N"))
        {
            char* end = nullptr;
            long v = std::strtol(e, &end, 10);
            if (end != e && v > 0)
                return (int)v;
        }
        return 0;
    }();
    static const bool s_PerfDumpSync = []()
    {
        if (const char* e = std::getenv("GE_PERF_DUMP_SYNC"))
            return (e && e[0] == '1');
        return false;
    }();
    static std::uint64_t s_PerfDumpCounter = 0;
    if (s_PerfDumpEveryN > 0)
    {
        if ((++s_PerfDumpCounter % (std::uint64_t)s_PerfDumpEveryN) == 0)
        {
            Logger::Log::Info("{}", BuildPerfDumpLine());
            if (s_PerfDumpSync)
            {
                Rendering::IDevice::FrameSyncTimings sync{};
                bool haveSync = false;
                for (auto& win : m_Windows)
                {
                    if (win && win->renderCtx)
                    {
                        auto* dev = win->renderCtx->GetDevice();
                        if (dev && dev->GetLastFrameSyncTimings(sync))
                        {
                            haveSync = true;
                            break;
                        }
                    }
                }
                if (haveSync)
                {
                    Logger::Log::Info(
                        "[PerfSync] frame={} wait {:.2f}ms (G{} C{} T{}) acquire {:.2f}ms{} presentSubmit {:.2f}ms present {:.2f}ms",
                        sync.frameIndex,
                        sync.beginFrameWaitMs,
                        sync.waitedGraphicsFence ? 1 : 0,
                        sync.waitedComputeFence ? 1 : 0,
                        sync.waitedTransferFence ? 1 : 0,
                        sync.acquireMs,
                        sync.acquireTimedOut ? " timeout" : "",
                        sync.presentTransitionSubmitMs,
                        sync.presentMs);
                }
            }
        }
    }
}

EditorApplication::~EditorApplication()
{
    // The entity links' and the asset actions reach this editor's panels.
    Editor::UninstallSceneViewEntityLinks();
    Editor::UninstallEditorAssetActions();
    // Ensure Application::Shutdown (and thus EditorApplication::OnShutdown)
    // run while EditorApplication members (windows, devices, UI, etc.) are
    // still alive. Shutdown() is idempotent and guarded by m_Initialized,
    // so this is safe even if it was called earlier.
    Shutdown();
}

void EditorApplication::ConfigureFromCommandLine(const GameEngine::Editor::Startup::EditorCommandLineArgs& args)
{
    // Store automation config for use after Initialize() when windows/UI are created.
    m_UiReplayScenarioPath = args.uiReplayScenario;
    m_UiReplayLogPath = args.uiReplayLogFile;
    m_UiReplayExitAfterFrames = args.exitAfterFrames;
    // Also support --exit-after-frames for normal (non-replay) runs.
    m_ExitAfterFrames = args.exitAfterFrames;
    m_ExitFrameCounter = 0;
    m_BenchRgCsvPath = args.benchRgCsv;
    m_DebugPortOverride = args.debugPort;

    // Stamp session provenance here, before Initialize(), so the branch recorded is the
    // one this binary was built from. The worktree comes from the executable directory,
    // which cannot drift; the branch is read once and deliberately never refreshed.
    m_SessionDescriptor = Editor::Startup::DeriveEditorSessionDescriptor(PathUtils::GetExecutableDirectory(),
                                                                        args.sessionLabel);
}

bool EditorApplication::Initialize()
{
    const auto tEditorStart = std::chrono::high_resolution_clock::now();
    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };

    // Create the MCP ring-buffer log sink early so it captures all startup
    // phase timings. The debug server itself is wired up at the end of init
    // (it needs RenderDoc and other late-init state), but the sink must exist
    // before any [Startup] messages are logged.
    {
        auto ringBuffer = std::make_unique<Logger::RingBufferSink>(2000);
        m_DebugLogSink = ringBuffer.get();
        Logger::Log::AddSink(std::move(ringBuffer));
    }

    auto tPhase = tEditorStart;
    ConfigureNativePluginResolvers();
    // VCS providers all live in engine-package modules (git-vcs, svn-vcs,
    // diversion-vcs, lore-vcs); the first workspace detection is gated on
    // their load pass (EditorVersionControlService::NotifyProvidersReady).
    if (!Application::Initialize())
    {
        return false;
    }
    Logger::Log::Info("[Startup] Application::Initialize (Engine): {:.1f}ms", MsSince(tPhase));

    // The Settings panel is lazy. Apply both SVG defaults before any editor UI
    // or project SVG texture can be loaded, rather than waiting for its page.
    Editor::SvgRasterSettings::ApplySavedDefaults(
        EngineCore::GetInstance().GetWorkspaceRoot());
    // The engine now knows the project and runs MarkupService; a later project switch
    // reloads in FinishProjectFolderSwitch.
    m_MarkupBridge->LoadProjectState(EngineCore::GetInstance().GetWorkspaceRoot());

    // Register Editor-specific drag ghost renderers (payload-specific UI polish).
    GameEngine::Editor::RegisterEditorDragDropGhostRenderers();

    // Split Project assets vs Editor assets:
    // - Project assets root is driven by Engine/AssetManager (derived from --project workspace + assetDirectory).
    // - Editor assets live next to the executable and are mounted for load/hot-reload without being persisted into the project DB.
    const std::filesystem::path projectAssetsRoot = EngineCore::GetInstance().GetAssetManager().GetAssetRoot();

    // UI replay scenarios expect the Assets tree to be scrollable. The repo's default
    // `Assets/` folder can be too small (folders-only view), so create a temporary
    // directory subtree under Assets for the duration of the UI replay run.
    // It is removed during shutdown to avoid leaving the working tree dirty.
    if (m_UiReplayScenarioPath.has_value())
    {
        const std::filesystem::path fixtureRoot = projectAssetsRoot / "__ui_replay_tree_fixtures__";
        std::error_code ec;
        std::filesystem::remove_all(fixtureRoot, ec);
        ec.clear();
        std::filesystem::create_directories(fixtureRoot, ec);
        if (!ec)
        {
            // Create enough folders to force scroll, but keep the scrollbar thumb
            // large enough for the replay scenario's fixed relative click point.
            for (int i = 0; i < 40; ++i)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Folder_%03d", i);
                std::filesystem::create_directories(fixtureRoot / buf, ec);
                if (ec)
                {
                    ec.clear();
                    break;
                }
            }
        }
    }
    const auto editorGlobalPaths = GameEngine::Editor::GetEditorGlobalPaths();
    std::filesystem::path editorAssetsRoot = editorGlobalPaths.installAssetsRoot;
#if defined(__APPLE__)
    {
        namespace fs = std::filesystem;

        const auto hasRequiredEditorAssets = [](const fs::path& root) -> bool
        {
            if (root.empty())
                return false;

            std::error_code ec;
            const bool hasTheme = fs::exists(root / "UI" / "theme.css", ec);
            ec.clear();
            const bool hasLayout = fs::exists(root / "UI" / "layout.uxml", ec);
            ec.clear();
            const bool hasCoreTheme = fs::exists(root / "UI" / "theme" / "core.css", ec);
            return hasTheme && hasLayout && hasCoreTheme;
        };

        // Prefer a user-writable editor assets root for hot-reload without touching the app bundle.
        // Seed it from install assets on first run / when new defaults are added.
        std::string seedErr;
        GameEngine::Editor::Startup::BuiltinSyncStats seedStats;
        if (!GameEngine::Editor::Startup::EnsureEditorUserAssetsSeeded(&seedErr, &seedStats))
        {
            if (!seedErr.empty())
            {
                Logger::Log::Warning("Editor: Failed to seed user editor assets: {}", seedErr);
            }
        }
        else if (seedStats.Copied > 0 || seedStats.RemovedStale > 0)
        {
            Logger::Log::Info("Editor: engine-owned asset refresh: {} updated, {} identical, {} stale pruned",
                              seedStats.Copied, seedStats.SkippedIdentical, seedStats.RemovedStale);
        }

        std::error_code ec;
        const fs::path userRoot = GameEngine::Editor::Startup::GetEditorUserAssetsRoot();
        const fs::path installRoot = GameEngine::Editor::Startup::GetEditorInstallAssetsRoot();
        if (hasRequiredEditorAssets(userRoot))
        {
            editorAssetsRoot = userRoot;
        }
        else if (hasRequiredEditorAssets(installRoot))
        {
            if (!userRoot.empty() && fs::exists(userRoot, ec))
            {
                Logger::Log::Warning("Editor: user editor assets at '{}' are incomplete; using install assets '{}'",
                                     userRoot.string(),
                                     installRoot.string());
            }
            editorAssetsRoot = installRoot;
        }
        else
        {
            Logger::Log::Warning("Editor: required UI assets are missing from user root '{}' and install root '{}'",
                                 userRoot.string(),
                                 installRoot.string());
        }
    }
#endif

    // Track editor assets directory for UI resources (layout/theme, icons, etc.)
    m_AssetsDirectory = editorAssetsRoot;

    // File watching for asset hot reload is handled centrally by AssetManager
    // (via its own FileWatchingService subscription). The Editor does not need
    // an additional UI-specific watcher.

    // Build shared EditorContext
    m_EditorContext = std::make_unique<EditorContext>();
    m_EditorContext->AssetsRoot = projectAssetsRoot;
    m_EditorContext->FileWatcher = &FileWatchingService::GetInstance();
    m_EditorContext->Assets = &EngineCore::GetInstance().GetAssetManager();
    m_EditorContext->UndoRedo = m_UndoRedo.get();
    m_EditorContext->PlayMode = m_PlayMode.get();
    m_EditorContext->DownloadManager = m_DownloadManager.get();
    m_EditorContext->RenderGraphTicks = &m_RenderGraphTicks;
    m_DownloadManager->SetContext(m_EditorContext.get(), m_ChangeNotifications.get());
    // Pin the active world on the context so consumers (MissingAssetTracker
    // rescan, etc.) read it from one place instead of each reaching into
    // EngineCore. EnsurePrimaryWorld is idempotent.
    m_EditorContext->World = EngineCore::GetInstance().EnsurePrimaryWorld();
    m_MissingAssetTracker = std::make_unique<MissingAssetTracker>();
    m_EditorContext->MissingAssets = m_MissingAssetTracker.get();
    m_MissingAssetTracker->Bind(m_EditorContext.get());

    // Rescan when the scene structure changes (load, mass create/destroy).
    // Notifications fire synchronously after the world commit, so it's safe
    // to walk the world here. RescanActive resolves world+registry from the
    // bound EditorContext.
    if (m_ChangeNotifications)
    {
        m_ChangeNotifications->SubscribeWorldStructureChanged(
            [this](const Editor::EditorChangeNotifications::WorldStructureChangedEvent& e)
            {
                if (!e.world || !m_MissingAssetTracker)
                    return;
                m_MissingAssetTracker->RescanActive();
            });
    }

    m_EditorContext->OnSceneDirty = [this]() { if (m_SceneEditor) m_SceneEditor->MarkSceneDirty(); };
    m_EditorContext->SceneBuildProgress = [this](uint64_t& processed, uint64_t& total) {
        return m_SceneEditor && m_SceneEditor->GetSceneBuildProgress(processed, total);
    };
    Editor::RegisterBuiltInViewOverlays(*m_EditorContext);
    m_EditorContext->UIReplayActive = m_UiReplayScenarioPath.has_value();
    // Under UI replay, pin the global UI animation clock to a fixed step so golden
    // screenshots of animated HUDs are reproducible (GameUIHost reads Time::GetDeltaTime).
    if (m_EditorContext->UIReplayActive)
    {
        constexpr float kUiReplayStepSeconds = 1.0f / 60.0f;
        GameEngine::Time::SetDeterministicStep(kUiReplayStepSeconds);
    }

    // Register Editor assets as a named source so they are addressable via `editor:<path>`.
    // Editor mount uses derived identity (GUID derived deterministically from the
    // canonical path under the editor root) — no persistent DB, no per-user cache,
    // no tombstones. Editor-shipped assets are version-controlled and recreated by
    // the build pipeline; persisting registry state across sessions to track them
    // produces a global-DB-with-stale-entries problem that bit users when assets
    // were copied in and then removed (see Phase 0a/2 in the rewrite plan).
    tPhase = std::chrono::high_resolution_clock::now();
    {
        auto& am = EngineCore::GetInstance().GetAssetManager();

        // One-time migration: if a previous version's global editor AssetDatabase
        // is left behind under <userCacheRoot>/Editor, clear it. The entries it
        // contains are now stale (GUIDs are derived from canonical paths under the
        // editor mount and don't need persisting).
        //
        // Guarded by a sentinel file so we don't repeatedly fight a future engine
        // version that legitimately writes new state to the same directory.
        // After a successful clear (or if the dir already didn't exist), drop a
        // sentinel and skip the cleanup forever after.
        {
            namespace fs = std::filesystem;
            const auto global = GameEngine::Editor::GetEditorGlobalPaths();
            if (!global.userCacheRoot.empty())
            {
                const fs::path editorCacheRoot = (global.userCacheRoot / "Editor").lexically_normal();
                const fs::path sentinel = (editorCacheRoot / ".legacy-cleared").lexically_normal();
                std::error_code ec;
                if (!fs::exists(sentinel, ec))
                {
                    if (fs::exists(editorCacheRoot, ec))
                    {
                        const auto removed = fs::remove_all(editorCacheRoot, ec);
                        if (ec)
                        {
                            Logger::Log::Warning("Editor: failed to clear legacy editor AssetDatabase at '{}': {}",
                                                 editorCacheRoot.string(), ec.message());
                        }
                        else if (removed > 0)
                        {
                            Logger::Log::Info("Editor: cleared legacy editor AssetDatabase at '{}' ({} entries)",
                                              editorCacheRoot.string(), removed);
                        }
                    }
                    // Drop sentinel so future startups skip this entire block.
                    fs::create_directories(editorCacheRoot, ec);
                    std::ofstream(sentinel) << "Editor mount migrated to derived identity. Safe to delete.\n";
                }
            }
        }

        (void)RegisterEditorAssetSource(am, editorAssetsRoot, "startup");

        // Mount packages declared by the startup project (project source is
        // already registered by EngineCore::Initialize). Must precede
        // FileWatchingService::StartWatching below so package watcher
        // subscriptions are in place when watching begins.
        {
            Editor::PackageManagerController::Dependencies packageDeps;
            packageDeps.GetPackageManagerPanels = [this]()
            {
                std::vector<PackageManagerPanel*> panels;
                for (auto& panel : m_PanelStorage)
                {
                    if (auto* packagesPanel = dynamic_cast<PackageManagerPanel*>(panel.get()))
                        panels.push_back(packagesPanel);
                }
                return panels;
            };
            m_Packages = std::make_unique<Editor::PackageManagerController>();
            m_Packages->Initialize(std::move(packageDeps));
        }
        m_Packages->MountForProject(EngineCore::GetInstance().GetWorkspaceRoot());
    }
    Logger::Log::Info("[Startup] EditorAssetSource: {:.1f}ms", MsSince(tPhase));

    // Start centralized file watching once mounts/subscriptions are established.
    {
        // UI replay automation should be deterministic and fast:
        // avoid background file watching/hot-reload storms caused by external file writes
        // (e.g., build/staging touching UI theme files) which can keep the process busy
        // long after the window closes.
        if (!m_UiReplayScenarioPath.has_value())
        {
            auto& fws = FileWatchingService::GetInstance();
            if (!fws.IsWatching())
            {
                if (!fws.StartWatching())
                {
                    // Partial success still arms the service; only the
                    // directories named in the errors above are unwatched.
                    Logger::Log::Warning("Editor: some directories failed to start watching (hot-reload is off for those)");
                }
            }
        }
    }

    // Thumbnail provider is initialized once the main RenderServices is available
    // (after the main window render context has been created).

    // Configure editor input actions on the shared Application-level InputSystem
    tPhase = std::chrono::high_resolution_clock::now();
    {
        Input::InputSystem* inputSystem = GetInputSystem();
        if (!inputSystem)
        {
            Logger::Log::Error("Editor: Application InputSystem is not available");
            return false;
        }
        EditorInput::RegisterEditorInputActions(*inputSystem);
    }

    // Runtime InputSystem for gameplay code — only receives events when game
    // view is focused and play mode is active. Separate from the editor's
    // InputSystem so scripts don't see raw key state while editing.
    // HDR/display-output controller must exist before the first window's
    // monitor-changed handler is registered (the handler lambdas deref it).
    {
        Editor::HdrOutputController::Dependencies hdrDeps;
        hdrDeps.Windows = &m_Windows;
        hdrDeps.ApplyUiRuntimeConfig = [](UIManager* ui) { ApplyUiRuntimeConfig(ui); };
        hdrDeps.RefreshWorldRenderForPassiveFrame =
            [this](EditorWindowContext* ctx) { RefreshWorldRenderForPassiveFrame(ctx); };
        m_HdrOutput = std::make_unique<Editor::HdrOutputController>();
        m_HdrOutput->Initialize(std::move(hdrDeps));
    }

    // Docking drag-drop policy controller (deps are lazy; the docking model and
    // replay runner are created later in Initialize).
    {
        Editor::WindowDockingController::Dependencies dndDeps;
        dndDeps.Windows = &m_Windows;
        dndDeps.GetMainDocking = [this]() { return m_Docking.get(); };
        dndDeps.IsUiReplayActive = [this]() { return m_UiReplay && m_UiReplay->IsEnabled(); };
        dndDeps.RenderWindowNow = [this](EditorWindowContext* ctx) { RenderSingle(ctx); };
        dndDeps.RebuildDockspaceNow = [](EditorWindowContext* ctx, DockingManager* docking)
        {
            EditorPanelManager::RebuildDockspaceNow(ctx, docking);
        };
        dndDeps.PreUiActions = &m_DeferredPreUiActions;
        m_DockDnd = std::make_unique<Editor::WindowDockingController>();
        m_DockDnd->Initialize(std::move(dndDeps));
    }

    {
        Editor::ColorPickerPresenter::Dependencies pickerDeps;
        pickerDeps.GetMainWindow = [this]() { return m_Windows.empty() ? nullptr : m_Windows[0].get(); };
        pickerDeps.QueueNativeToolWindow = [this](std::unique_ptr<EditorWindowContext> ctx)
        { QueueNativeToolWindow(std::move(ctx)); };
        pickerDeps.IsUiReplayActive = [this]() { return m_UiReplayScenarioPath.has_value(); };
        m_ColorPicker = std::make_unique<Editor::ColorPickerPresenter>();
        m_ColorPicker->Initialize(std::move(pickerDeps));
    }

    // The game's sink sits ahead of editor focus navigation and editor actions in
    // the routing chain, so it takes only the input the game claims and lets the
    // rest keep travelling to the editor.
    m_RuntimeInput = std::make_unique<Input::InputSystem>(Input::SinkRole::Gameplay);
    EngineCore::GetInstance().SetRuntimeInput(m_RuntimeInput.get());
    Logger::Log::Info("[Startup] InputSystem config: {:.1f}ms", MsSince(tPhase));

    // Create main window (using Platform::Window)
    tPhase = std::chrono::high_resolution_clock::now();
    auto mainCtx = std::make_unique<EditorWindowContext>();
    WindowDescriptor mainDesc{};
    mainDesc.role = WindowRole::Main;
    mainDesc.capabilities.worldRender = true;
    mainDesc.capabilities.uiOnly = false;
    mainDesc.capabilities.thumbnails = true;
    mainDesc.capabilities.dockingHost = true;
    mainDesc.title = GetConfig().Name;
    ApplyWindowDescriptor(*mainCtx, mainDesc, m_NextWindowId++);

    // Determine window size: if config has default 1280x720, use 80% of the primary monitor's
    // work area instead. This makes the Editor open at a more usable size on high-res displays
    // (especially Mac Retina screens) while respecting explicit user overrides.
    int windowWidth = static_cast<int>(GetConfig().WindowWidth);
    int windowHeight = static_cast<int>(GetConfig().WindowHeight);

    // Only the work-area size feeds the initial window bounds; the window opens
    // maximized on the default monitor, so its origin is the OS's to pick.
    int workW = 0;
    int workH = 0;
    {
        int workOriginX = 0;
        int workOriginY = 0;
        Platform::GetPrimaryMonitorWorkArea(workOriginX, workOriginY, workW, workH);
    }
    const std::filesystem::path startupSettingsRoot = ResolveStartupProjectSettingsRoot(GetConfig());
    // The main window always opens on the default monitor: rendering.hdr.targetDisplay
    // gates whether HDR engages there, it does not place the window. The saved HDR mode
    // itself reaches the swapchain below, via ConfigureStartupDeviceDesc.
    // Use 80% of the work area, clamped to a reasonable minimum
    windowWidth = std::max(windowWidth, static_cast<int>(workW));
    windowHeight = std::max(windowHeight, static_cast<int>(workH));

    mainCtx->width = windowWidth;
    mainCtx->height = windowHeight;
    mainCtx->title = GetConfig().Name;
    mainCtx->window = std::make_unique<Platform::Window>();
    // Create hidden; show after first rendered frame to avoid white-screen flash.
    // The first platform event poll triggers ~1.8s of OS/driver window-show processing.
    // By keeping the window hidden until after the first render, the user sees a
    // fully rendered editor instead of a blank window.
    if (!mainCtx->window->Create({.Title = mainCtx->title,
                                  .Width = mainCtx->width,
                                  .Height = mainCtx->height,
                                  .StartHidden = true,
                                  .StartMaximized = true}))
    {
        Logger::Log::Error("Editor: Failed to create main window");
        return false;
    }
    {
        const auto monitors = Platform::EnumerateMonitors();
        Logger::Log::Info("Editor: detected {} monitor(s)", monitors.size());
        for (const auto& monitor : monitors)
        {
            Logger::Log::Info("Editor: monitor[{}] '{}' primary={} pos={}x{} size={}x{} work={}x{}+{}+{} refresh={:.1f}Hz hdrAvailable={} hdrActive={}",
                              monitor.index,
                              monitor.name,
                              monitor.primary ? "true" : "false",
                              monitor.x,
                              monitor.y,
                              monitor.width,
                              monitor.height,
                              monitor.workWidth,
                              monitor.workHeight,
                              monitor.workX,
                              monitor.workY,
                              monitor.refreshRate,
                              monitor.hdrAvailable ? "true" : "false",
                              monitor.hdrActive ? "true" : "false");
        }
    }

    // Linux: load window icon at runtime via GLFW. Windows/macOS rely on
    // embedded resources or bundle icons.
#if !defined(_WIN32) && !defined(__APPLE__) && defined(GE_HAVE_STB)
    {
        auto iconPath = PathUtils::GetExecutableDirectory() / "AppIcon.png";
        int iconW = 0, iconH = 0, iconChannels = 0;
        unsigned char* iconPixels = stbi_load(iconPath.string().c_str(), &iconW, &iconH, &iconChannels, 4);
        if (iconPixels)
        {
            mainCtx->window->SetIcon(iconPixels, iconW, iconH);
            stbi_image_free(iconPixels);
        }
    }
#endif

    // Expose the main editor window through EditorContext so panels/controllers
    // (e.g., the Assets browser) can show native context menus at cursor position.
    if (m_EditorContext)
    {
        m_EditorContext->MainWindow = mainCtx->window.get();
        m_EditorContext->ThumbnailHostWindowId = mainCtx->windowId;
    }

    Logger::Log::Info("[Startup] Window creation: {:.1f}ms", MsSince(tPhase));

    // Per-window input routing to UIManager and the shared Application InputSystem
    EditorWindowContext* mainCtxRaw = mainCtx.get();
    WindowInputRouterConfig mainInput{};
    mainInput.window = mainCtx->window.get();
    mainInput.getUi = [this, mainCtxRaw]() -> UIManager*
    {
        // A UI replay drives this UIManager with synthetic input; real OS events
        // would contaminate the deterministic stream (a stray cursor-leave parks
        // the mouse at the unknown sentinel and kills hover for the rest of the
        // run). Route nothing while a replay scenario is active.
        if (m_UiReplayScenarioPath.has_value())
            return nullptr;
        return mainCtxRaw ? mainCtxRaw->ui.get() : nullptr;
    };
    mainInput.getInput = [this]() -> Input::InputSystem*
    {
        return GetInputSystem();
    };
    mainInput.onScrollPre = [this](float /*dx*/, float dy) -> bool
    {
        // When CSS inspector is on, scroll cycles through layers instead of UI/camera.
        if (m_CssInspector.IsEnabled())
        {
            m_CssInspector.OnScrollWheel(dy);
            return true;
        }
        return false;
    };
    mainInput.onKeyPre = [this, mainCtxRaw](int key, int action, int mods) -> bool
    {
        if (key == Input::kKeyCode_Escape && action == Input::kKeyActionPress && m_PlayMode)
        {
            const auto state = m_PlayMode->GetState();
            // Change Review is post-play: no Game View sink, so Escape discard
            // has to be intercepted here.
            if (state == GameEngine::Editor::PlayModeState::ChangeReview)
            {
                m_PlayMode->DiscardPendingChanges();
                return true;
            }
            // Fullscreen play hides the top toolbar (.play-fullscreen-gameview
            // in EditorTopToolbar.css), so Escape is its only visible exit and
            // is intercepted ahead of the runtime forward. Windowed play leaves
            // Escape to the game: exit with Ctrl/Cmd+P or the toolbar Stop.
            if (m_PlayFullscreenActive && state != GameEngine::Editor::PlayModeState::Edit)
            {
                m_PlayMode->ExitPlayMode();
                ExitPlayFullscreen();
                if (m_TopToolbar)
                    m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
                return true;
            }
        }

        // The configured play toggle exits before routing so the shortcut
        // works regardless of which panel is focused or whether game-view input
        // is active. Entering play from Edit still goes through the normal
        // Editor.Global.TogglePlayMode action path below.
        if (action == Input::kKeyActionPress && m_PlayMode)
        {
            const auto* input = GetInputSystem();
            if (input && input->MatchesKeyBinding(EditorInput::kEditorGlobalContext,
                                                 EditorInput::kEditorTogglePlayMode, key, mods))
            {
                const auto state = m_PlayMode->GetState();
                if (state == GameEngine::Editor::PlayModeState::ChangeReview)
                {
                    m_PlayMode->DiscardPendingChanges();
                    return true;
                }
                if (state != GameEngine::Editor::PlayModeState::Edit)
                {
                    m_PlayMode->ExitPlayMode();
                    ExitPlayFullscreen();
                    if (m_TopToolbar)
                        m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
                    return true;
                }
            }
        }

        // Global editor hotkey: F2 controls CPU profiler (opt-in diagnostics).
        // - Press F2 once to enable.
        // - Press F2 again to dump the last frame's scope summary to the console.
        // - Press Shift+F2 to disable.
#if GE_ENABLE_CPU_PROFILING
        if (key == Input::kKeyCode_F2 && action == Input::kKeyActionPress)
        {
            auto& prof = GameEngine::Profiling::CpuProfiler::Get();
            if ((mods & Input::kModShift) != 0)
            {
                prof.SetEnabled(false);
                Logger::Log::Info("[CPU Profiler] OFF");
                return true;
            }
            if (!prof.IsEnabled())
            {
                prof.SetEnabled(true);
                Logger::Log::Info("[CPU Profiler] ON (press F2 again to dump)");
            }
            else
            {
                // Include the current perf breakdown so dumps are self-contained.
                prof.RequestDump(this->BuildPerfDumpLine());
            }
            return true;
        }
#endif

        // Global editor hotkey: F3 toggles UI Demo panel.
        if (key == Input::kKeyCode_F3 && action == Input::kKeyActionPress)
        {
            if (m_PanelManager)
                m_PanelManager->ShowOrActivateUIDemoPanel(mainCtxRaw);
            return true;
        }

        // Global editor hotkey: F11 toggles dock debug overlay regardless of UI focus.
        if (key == Input::kKeyCode_F11 && action == Input::kKeyActionPress)
        {
            this->ToggleDockDebugZones();
            return true;
        }

        if (this->TryHandleCatalogGlobalKeyPre(mainCtxRaw, key, action, mods, true))
            return true;

        // Copy hovered element selector + style to clipboard. Key: C (no modifiers), only while CSS inspector is active.
        if (key == Input::kKeyCode_C && action == Input::kKeyActionPress && (mods & (Input::kModControl | Input::kModShift | Input::kModAlt | Input::kModSuper)) == 0 &&
            mainCtxRaw->ui && m_CssInspector.IsEnabled())
        {
            if (UIElement* displayed = m_CssInspector.GetDisplayedElement(mainCtxRaw->ui.get()))
            {
                std::string s = m_CssInspector.GetSummaryIncludingPathForElement(mainCtxRaw->ui.get(), displayed);
                if (!s.empty() && mainCtxRaw->ui->GetPlatform())
                    mainCtxRaw->ui->GetPlatform()->SetClipboardText(s.c_str());
            }
            return true;
        }

#if defined(_DEBUG)
	        // Debug-only: export the current UI layout/styles for this window.
	        if (key == Input::kKeyCode_F12 && action == Input::kKeyActionPress)
	        {
	            this->DebugExportCurrentUILayout(mainCtxRaw);
	            return true;
	        }
#endif

        // Global editor shortcuts: macOS Cmd+Q quits.
        if (action == Input::kKeyActionPress)
        {
#if defined(__APPLE__)
            if (((mods & Input::kModSuper) != 0) && key == Input::kKeyCode_Q)
            {
                // RequestClose() does not run the GLFW close callback; mirror the title-bar flow.
                if (TryInterceptQuitForDirtyScene())
                    return true;
                for (auto& w : m_Windows)
                {
                    if (w && w->window)
                        w->window->RequestClose();
                }
                return true;
            }
#endif
        }
        return false;
    };
    mainInput.getPlaySurface = [this, mainWindow = mainCtx->window.get()]()
    { return GetPlaySurface(mainWindow); };
    mainCtxRaw->inputConfig = mainInput;
    WindowInputRouter::BindBasicHandlers(mainCtxRaw->inputConfig);
    mainCtx->window->SetRefreshHandler([this] { Tick(); });
    mainCtx->window->SetPositionHandler([this, mainCtxRaw](int x, int y)
                                        { this->OnWindowMoved(mainCtxRaw, x, y); });
    mainCtx->window->SetFramebufferSizeHandler([this, mainCtxRaw](int width, int height)
                                               { this->OnFramebufferResized(mainCtxRaw, width, height); });
    mainCtx->window->SetMonitorChangedHandler([this, mainCtxRaw](int monitorIndex)
                                              { m_HdrOutput->OnWindowMonitorChanged(mainCtxRaw, monitorIndex); });
    mainCtx->window->SetFileDropHandler([this, mainCtxRaw](const std::vector<std::filesystem::path>& paths)
                                        { this->OnFilesDropped(mainCtxRaw, paths); });
    BindPlayFullscreenChangedHandler(*mainCtx->window);
    mainCtx->window->GetContentScale(mainCtxRaw->lastNativeContentScaleX,
                                     mainCtxRaw->lastNativeContentScaleY);

    Logger::Log::Info("[Startup] Input routing setup: {:.1f}ms", MsSince(tPhase));

    // Pre-load UI assets (CSS + layout + all CSS @imports) on a background thread while
    // the main thread proceeds to Vulkan device creation. Both are independent: CSS/XML
    // parsing is CPU-bound, Vulkan init is GPU-driver-bound.
    m_LayoutPath = std::filesystem::path("UI") / "layout.uxml";
    m_StylePath = std::filesystem::path("UI") / "theme.css";
    {
        auto tPreload = std::chrono::high_resolution_clock::now();
        auto& am = EngineCore::GetInstance().GetAssetManager();
        // Resolve the 2 primary GUIDs on the main thread (fast, needed before Vulkan).
        RefreshEditorUiAssetRefs(am, m_LayoutPath, m_StylePath, m_LayoutGuid, m_StyleGuid, "early-ui-preload");

        // Dispatch all GUID resolution + LoadAsset calls as a compute job (CPU work: the
        // lambda only DISPATCHES async loads) so the main thread can proceed immediately
        // to Vulkan device creation. On web the lambda runs inline instead, as it does
        // where no thread of its own may be started (Platform::SupportsTransientThreads):
        // inline costs microseconds there, and the browser main thread must not wait on
        // a job at shutdown.
        GUID layoutGuid = m_LayoutGuid;
        GUID styleGuid = m_StyleGuid;
        AssetManager* amPtr = &am;
        const auto uiPreload = [amPtr, layoutGuid, styleGuid]()
        {
            if (!layoutGuid.IsNull())
                amPtr->LoadAsset(layoutGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);
            if (!styleGuid.IsNull())
                amPtr->LoadAsset(styleGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);

            // All CSS @imports from theme.css + panel-specific UXML/CSS.
            // Pre-loading here means panels find cached assets when their OnPostLayout fires.
            static constexpr const char* kEditorAssetPreloads[] = {
                // theme.css @imports
                "UI/theme/tokens.css",     "UI/theme/core.css",
                "UI/theme/views.css",      "UI/theme/inspector.css",
                "UI/theme/widgets.css",    "UI/theme/version-control.css",
                "UI/theme/smartfolders.css",
                "UI/theme/toggle.css",     "UI/theme/slider.css",
                "UI/theme/settings.css",   "UI/theme/todos.css",
                "UI/theme/bookmarks.css",  "UI/panels/DiffPanel.css",
                "UI/panels/BuildPanel.css","UI/panels/MovieRecorderPanel.css",
                "UI/theme/node-graph.css",
                "UI/controls/EditorTopToolbar.uxml", "UI/controls/EditorTopToolbar.css",
                "UI/controls/ProjectPicker.uxml", "UI/controls/ProjectPicker.css",
                // Panel UXML + CSS that actually exist on disk.
                // Only panels with separate .uxml/.css files are listed here.
                "UI/panels/SceneViewPanel.uxml", "UI/panels/SceneViewPanel.css",
                "UI/theme/scene-view.css",
                "UI/panels/GameViewPanel.uxml",  "UI/panels/GameViewPanel.css",
                "UI/panels/AssetViewPanel.css",
                "UI/panels/SettingsPanel.css",
                "UI/panels/AnimationWindowPanel.uxml", "UI/panels/AnimationWindowPanel.css",
                "UI/panels/WebPanel.uxml",  "UI/panels/WebPanel.css",
                "UI/panels/MixerPanel.css",
                "UI/panels/ColorPickerPanel.css",
                "UI/panels/MonitorsPanel.css",
                "UI/panels/RenderGraphPanel.css",
                "UI/panels/PackageManagerPanel.css",
                "UI/panels/CpuProfilerPanel.css",
                "UI/panels/VisualProfilerPanel.css",
                "UI/panels/VramPanel.css",
                "UI/panels/MissingAssetsPanel.css",
            };
            for (const char* assetPath : kEditorAssetPreloads)
            {
                GUID guid = amPtr->ResolveAssetGuid(
                    std::filesystem::path(assetPath), kAssetSourceAliasEditor);
                if (!guid.IsNull())
                    amPtr->LoadAsset(guid, AssetLoadResultCallback{}, AssetLoadPriority::High);
            }
            Logger::Log::Info("[Startup] UI pre-load thread completed ({} assets)", std::size(kEditorAssetPreloads));
        };
        if (Platform::SupportsTransientThreads())
            m_UiPreload = EngineCore::GetInstance().GetJobSystem().Submit(uiPreload);
        else
            uiPreload();
        Logger::Log::Info("[Startup] UI pre-load dispatched to background: {:.1f}ms", MsSince(tPreload));
    }

    // Create a device and swapchain for this window (PoC: one device per window).
    // Ownership is centralized in RenderDeviceContext so shutdown ordering is consistent.
    tPhase = std::chrono::high_resolution_clock::now();
    DeviceDesc desc{};
    desc.preferredAPI = LoadPreferredGraphicsApi();
    desc.enableSwapchain = true;
    desc.applicationName = GetConfig().Name;
    if (Engine::Renderer::ShouldEnableVkValidation())
    {
        desc.enableDebugLayer = true;
    }
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        bool vsync = true;
        prefs.TryGetBool("performance.vsync", vsync);
        // GE_VSYNC overrides the saved preference (diagnostics / forced runs).
        if (const char* vsyncEnv = std::getenv("GE_VSYNC"))
        {
            if (vsyncEnv[0] != '\0')
                vsync = (vsyncEnv[0] != '0');
        }
        desc.vsync = vsync;
    }
    const int initialMonitorIndex = mainCtx->window ? mainCtx->window->GetActiveMonitorIndex() : -1;
    Editor::HdrOutputController::ConfigureStartupDeviceDesc(desc, startupSettingsRoot, initialMonitorIndex);

    mainCtx->renderCtx = std::make_unique<GameEngine::Engine::Renderer::RenderDeviceContext>();
    if (!mainCtx->renderCtx)
    {
        Logger::Log::Error("Editor: Failed to allocate RenderDeviceContext (main window)");
        return false;
    }

    // The swapchain starts at the window's real framebuffer size, not at the
    // size that was requested: a maximized or HiDPI window is larger than the
    // request, and a canvas-backed window sizes itself to the viewport. A
    // surface configured to a stale size writes that size back into the
    // backing store.
    {
        int fbW = 0;
        int fbH = 0;
        mainCtx->window->GetFramebufferSize(fbW, fbH);
        if (fbW > 0 && fbH > 0)
        {
            mainCtx->width = fbW;
            mainCtx->height = fbH;
        }
    }

    GameEngine::Engine::Renderer::RenderDeviceContext::InitParams renderInit{};
    renderInit.deviceDesc = desc;
    renderInit.windowHandle = mainCtx->window->GetNativeHandle();
    renderInit.width = static_cast<uint32>(mainCtx->width);
    renderInit.height = static_cast<uint32>(mainCtx->height);
    renderInit.createRenderServices = true;

    if (!mainCtx->renderCtx->Initialize(renderInit))
    {
        Logger::Log::Error("Editor: Failed to initialize RenderDeviceContext (main window)");
        return false;
    }

    Logger::Log::Info("[Startup] RenderDeviceContext (Vulkan+RenderServices): {:.1f}ms", MsSince(tPhase));
    tPhase = std::chrono::high_resolution_clock::now();

    // Enable the engine-managed rendering loop so the ECS rendering systems
    // (extraction, culling, RenderGraphBuildSystem) execute every frame.
    {
        auto& engine = EngineCore::GetInstance();
        auto* rs = mainCtx->renderCtx ? mainCtx->renderCtx->GetRenderServices() : nullptr;
        if (!rs)
        {
            Logger::Log::Error("Editor: RenderServices was not created for the main window render context");
            return false;
        }
        // Primitive creation commands in HierarchyPanel resolve mesh/material handles
        // from these registries. Register them at startup so entity creation works
        // even when world bootstrapping is disabled.
        Engine::Renderer::PrimitiveGenerator::RegisterAll(*rs);
        engine.EnableRenderingLoop(rs);

        // The Editor owns the render-graph lifecycle (BeginFrame / Compile /
        // Execute) and schedules pipeline + present passes itself during
        // its render phase.  Tell the ECS-driven RenderGraphBuildSystem to
        // skip BuildFrameGraph so per-frame housekeeping (write-pool,
        // material upload, draw-list build) still runs without conflicting
        // with the Editor's own pass scheduling.
        if (auto* loop = engine.GetRenderingLoop())
        {
            if (auto* sm = loop->GetSystemManager())
            {
                if (auto* rgBuild = sm->GetSystem<Engine::Renderer::RenderGraphBuildSystem>())
                    rgBuild->SetSkipFrameGraphBuild(true);
            }
        }

        // Expose RenderServices to editor panels via EditorContext.
        if (m_EditorContext)
            m_EditorContext->RenderServices = rs;

        // Project-shared physics settings:
        // Load from <ProjectRoot>/.Editor/ProjectSettings.json and apply to the ECS world
        // as PhysicsWorldSettingsComponent. PhysicsECS will consume this via its bootstrap system.
        if (ECS::World* world = engine.EnsurePrimaryWorld())
        {
            EnsureWorldHasPhysicsSettingsFromProject(*world, engine.GetWorkspaceRoot());
        }

        // Project-shared rendering settings:
        // Load active render pipeline path from <ProjectRoot>/.Editor/ProjectSettings.json (if present).
        {
            const auto& wsRoot = engine.GetWorkspaceRoot();
            Logger::Log::Info("Editor: loading render pipeline settings (workspace root: '{}')", wsRoot.string());
            const std::filesystem::path pipelinePath = Editor::LoadActiveRenderPipelinePathFromProjectSettings(wsRoot);
            Editor::ApplyProjectRenderSettings(*rs, wsRoot);
            if (!pipelinePath.empty())
            {
                Logger::Log::Info("Editor: loaded active render pipeline from settings: {}", pipelinePath.generic_string());
                rs->Spine().SetActiveRenderPipelinePath(pipelinePath);
            }
            else
            {
                Logger::Log::Info("Editor: no active render pipeline in project settings (using default)");
            }
        }

        // Pre-load the render pipeline asset so EnsureActiveRenderPipelineBuilt()
        // finds it on Frame 0 instead of falling back to builtin (which causes a 370ms
        // rebuild on Frame 1 when the real asset becomes available).
        // Kick off async load — it will complete during Vulkan-driven Frame 0 rendering.
        {
            auto& am = EngineCore::GetInstance().GetAssetManager();
            const auto pipelineAssetPath = rs->Spine().GetActiveRenderPipelinePath();
            if (!pipelineAssetPath.empty())
            {
                const auto absPath = am.ResolveAssetPath(pipelineAssetPath);
                std::error_code ec;
                if (!absPath.empty() &&
                    std::filesystem::exists(absPath, ec) &&
                    !std::filesystem::is_directory(absPath, ec))
                {
                    GUID pipelineGuid = am.ResolveAssetGuid(pipelineAssetPath);
                    if (pipelineGuid.IsNull())
                        pipelineGuid = am.GetRegistry().GetAssetGUID(absPath);
                    if (!pipelineGuid.IsNull())
                        am.LoadAsset(pipelineGuid, AssetLoadResultCallback{}, AssetLoadPriority::High);
                }
            }
        }

        // Pre-load shader packages used by the ForwardPlus pipeline as a job of the engine's
        // "Engine startup" channel (it waits on the disk, so it holds no compute worker).
        // These .shaderpkg files are loaded from disk during EnsureCachedResources() on the
        // first pipeline build (Frame 1). By pre-loading them here, the disk I/O overlaps
        // with Frame 0's UI work and the shader data is available from cache instantly.
        {
            static constexpr const char* kPipelineShaderPkgs[] = {
                "Shaders/clustered_light_cull.shaderpkg",
                "Shaders/bloom_threshold.shaderpkg",
                "Shaders/bloom_downsample.shaderpkg",
                "Shaders/bloom_octave_gather.shaderpkg",
                "Shaders/halation_prefilter.shaderpkg",
                "Shaders/halation_blur_h.shaderpkg",
                "Shaders/halation_composite.shaderpkg",
                "Shaders/bloom_combine.shaderpkg",
                "Shaders/tonemap.shaderpkg",
            };
            auto* rsCapture = rs;
            const GameEngine::Rendering::ShaderSourceKind sourceKind =
                rs->GetDevice()->PreferredShaderSource();
            const auto shaderPreload = [rsCapture, sourceKind]() {
                for (const char* pkg : kPipelineShaderPkgs)
                    rsCapture->Spine().PreLoadShaderPackage(pkg, sourceKind);
                Logger::Log::Info("[Startup] Shader packages pre-loaded on a startup job");
            };
            if (Platform::SupportsTransientThreads())
                m_ShaderPreload = EngineCore::GetInstance().GetStartupChannel().Submit(shaderPreload);
            else
                shaderPreload();
        }

        // Default Editor mode: runtime simulation systems OFF until entering Play Mode.
        if (m_PlayMode)
        {
            m_PlayMode->ApplyEditModeSystemGating();
        }
    }

    // Store as the first window
    // Create minimal SceneViewController for this window
    mainCtx->scene = std::make_unique<SceneViewController>(
        mainCtx->renderCtx ? mainCtx->renderCtx->GetRenderServices() : nullptr,
        *EngineCore::GetInstance().EnsurePrimaryWorld());
    if (mainCtx->scene)
    {
        mainCtx->scene->SetUndoRedoService(m_UndoRedo.get());
        if (auto* tool = mainCtx->scene->GetTransformTool())
        {
            tool->SetUndoRedoService(m_UndoRedo.get());
            tool->SetChangeNotifications(m_ChangeNotifications.get());
        }
        mainCtx->scene->SetChangeNotifications(m_ChangeNotifications.get());
    }
    {
        auto* rs = mainCtx->renderCtx ? mainCtx->renderCtx->GetRenderServices() : nullptr;
        const SceneViewController::FixedViewOrientation orientations[] = {
            SceneViewController::FixedViewOrientation::Top,
            SceneViewController::FixedViewOrientation::Front,
            SceneViewController::FixedViewOrientation::Side,
        };
        const char* prefixes[] = {"SceneView.Top", "SceneView.Front", "SceneView.Side"};
        for (size_t i = 0; i < mainCtx->sceneQuadViews.size(); ++i)
        {
            mainCtx->sceneQuadViews[i] = std::make_unique<SceneViewController>(rs, *EngineCore::GetInstance().EnsurePrimaryWorld());
            mainCtx->sceneQuadViews[i]->SetRenderNamePrefix(prefixes[i]);
            mainCtx->sceneQuadViews[i]->SetFixedViewOrientation(orientations[i]);
            mainCtx->sceneQuadViews[i]->SetUndoRedoService(m_UndoRedo.get());
            mainCtx->sceneQuadViews[i]->SetChangeNotifications(m_ChangeNotifications.get());
            if (auto* tool = mainCtx->sceneQuadViews[i]->GetTransformTool())
            {
                tool->SetUndoRedoService(m_UndoRedo.get());
                tool->SetChangeNotifications(m_ChangeNotifications.get());
            }
        }
    }

    // Expose the primary scene view's 2D-mode state to panels (Hierarchy uses
    // this to orient sprite drops upright toward the 2D camera).
    if (m_EditorContext)
    {
        SceneViewController* primaryScene = mainCtx->scene.get();
        m_EditorContext->IsSceneView2D = [primaryScene]() {
            return primaryScene && primaryScene->Is2DMode();
        };
    }

    // Create minimal GameViewController for this window (ECS camera -> engine("game_main")).
    mainCtx->gameView = std::make_unique<GameViewController>(
        mainCtx->renderCtx ? mainCtx->renderCtx->GetRenderServices() : nullptr,
        m_AssetsDirectory);

    // Reserve capacity for the main window + a few tool windows (color picker, tear-offs).
    // This reduces the chance of m_Windows reallocating during iteration when a tool window
    // is added from a UI event handler (e.g. opening the color picker from Settings).
    m_Windows.reserve(8);
    m_Windows.emplace_back(std::move(mainCtx));

    // ThumbnailService-backed provider: dispatches to per-asset handlers and
    // preserves existing behavior for images/models via a fallback path.
    {
        auto* assetManager = &EngineCore::GetInstance().GetAssetManager();
        auto* renderServices =
            (m_Windows[0] && m_Windows[0]->renderCtx) ? m_Windows[0]->renderCtx->GetRenderServices() : nullptr;

        if (!assetManager || !renderServices)
        {
            Logger::Log::Warning("Editor: ThumbnailService disabled (missing AssetManager or RenderServices)");
        }
        else
        {
            auto thumbnailService = std::make_unique<ThumbnailService>(assetManager);
            ThumbnailService* thumbnailServiceRaw = thumbnailService.get();
            m_ThumbnailProvider = std::move(thumbnailService);
            Editor::RegisterThumbnailMenuItems(*m_ThumbnailProvider);

            // Register model thumbnails handler. When the handler cannot produce a
            // thumbnail, ThumbnailService falls back to the static model icon.
            auto modelHandler = std::make_unique<ModelThumbnailHandler>(assetManager, renderServices);
            thumbnailServiceRaw->RegisterHandler(AssetType::Model, std::move(modelHandler));

            // Register video thumbnail handler (extracts representative frame).
            auto videoHandler = std::make_unique<VideoThumbnailHandler>();
            thumbnailServiceRaw->RegisterHandler(AssetType::Video, std::move(videoHandler));

            // Register texture thumbnails: without it, every image asset serves
            // its full-resolution source as its own thumbnail.
            auto textureHandler =
                std::make_unique<TextureThumbnailHandler>(EngineCore::GetInstance().GetJobSystem());
            thumbnailServiceRaw->RegisterHandler(AssetType::Texture, std::move(textureHandler));

            // Thumbnails should browse project assets, not editor assets.
            m_ThumbnailProvider->SetAssetsRoot(EngineCore::GetInstance().GetAssetManager().GetAssetRoot());

            // Configure the persistent thumbnail cache root under the project
            // directory, not under Assets/. Design default:
            //   <ProjectRoot>/.Editor/Thumbnails
            //
            // Skip when no project is loaded yet (workspace == exeDir), which on
            // macOS would resolve to inside the .app bundle and corrupt its
            // codesign seal. ApplyProjectFolderUiAsync re-applies the cache root
            // once the user picks a project.
            {
                const auto p = GameEngine::Editor::GetCurrentEditorProjectPaths();
                const auto& cfg = GetConfig();
                const bool workspaceIsProject = !cfg.WorkspaceDirectory.empty() &&
                                                !cfg.WorkspaceDirectoryIsFallback;
                if (workspaceIsProject && !p.thumbnailsRoot.empty())
                {
                    thumbnailServiceRaw->SetCacheRoot(p.thumbnailsRoot);
                }
            }

            if (m_EditorContext)
            {
                m_EditorContext->Thumbnails = m_ThumbnailProvider.get();
            }
        }
    }

    // Attach native OS menu to the main window (does not consume UI content space)
    if (m_EnableNativeToolbar)
    {
        auto* main = m_Windows[0].get();
        if (main && main->window)
        {
            m_EditorToolbar = std::make_unique<EditorToolbar>();
            m_EditorToolbar->Install(main->window.get(), this);
        }
    }

    Logger::Log::Info("[Startup] RenderLoop+Thumbnails+Toolbar setup: {:.1f}ms", MsSince(tPhase));

    // Stage a data-driven UI layout for the Editor (now with device wired)
    tPhase = std::chrono::high_resolution_clock::now();
    try
    {
        auto* main = m_Windows[0].get();
        if (!main)
        {
            Logger::Log::Error("Editor: Main window context not available");
            return false;
        }
        Logger::Log::Info("Editor: UI stage begin");
        auto tUiSub = std::chrono::high_resolution_clock::now();
        main->ui = std::make_unique<UIManager>(main->renderCtx ? main->renderCtx->GetDevice() : nullptr,
                                               &EngineCore::GetInstance().GetAssetManager());
        // Unconditional: native menus need this association too, to resolve
        // their row icons against the window's stylesheet cascade.
        UIContextMenu::Register(main->window.get(), main->ui.get());
        // Menu-owning manipulators create their menus through this: the configured
        // backend, wrapped in the automation interceptor — never CreateNativeContextMenu,
        // which would drop both.
        ContextMenuManipulator::SetMenuFactory([] { return CreateContextMenu(); });
        ApplyUiRuntimeConfig(main->ui.get());
        Logger::Log::Info("[Startup]   UIManager ctor: {:.1f}ms", MsSince(tUiSub));
        if (main->ui)
        {
            // Host-provided font loading: resolve CSS font-family through AssetRegistry first,
            // then fall back to OS system fonts. This keeps UIManager generic and supports async loads.
            tUiSub = std::chrono::high_resolution_clock::now();
            ConfigureEditorUiFonts(main->ui.get(), &EngineCore::GetInstance().GetAssetManager(), m_AssetsDirectory);
            Logger::Log::Info("[Startup]   ConfigureEditorUiFonts: {:.1f}ms", MsSince(tUiSub));
            // Font prefetch (13 families) is deferred to Update() after the first few frames
            // to avoid draining 914ms of PostActions all on frame 0. See m_PendingFontPrefetch.
        }
        // Wire OS clipboard into UI via a per-window platform adapter so
        // TextInput/TextArea controls can use copy/paste.
        if (main->window)
        {
            main->uiPlatform = std::make_unique<WindowPlatformApi>(main->window.get());
            main->ui->SetPlatform(main->uiPlatform.get());
            Editor::ApplySavedHiDpiPlatformSettings(main->uiPlatform.get());

            // Set cursor callback to change OS cursor based on hovered UI element's CSS cursor property.
            SetupUICursorCallback(main->ui.get(), main->window.get());
        }

        // Wire tooltip settings into UIManager so it respects user preferences.
        if (main->ui)
        {
            main->ui->SetTooltipConfigProvider([]() -> UIManager::TooltipConfig
            {
                const auto& s = Editor::TooltipSettings::Get();
                UIManager::TooltipConfig cfg;
                cfg.Enabled                = s.GetEnabled();
                cfg.HoverDelaySeconds      = s.GetHoverDelaySeconds();
                cfg.HoverResetDelaySeconds = s.GetHoverResetDelaySeconds();
                cfg.MiddleMouseShow        = s.GetMiddleMouseShow();
                cfg.ArrowColor             = s.GetArrowColor();
                return cfg;
            });
        }
        // Layout/style paths and GUIDs were resolved earlier (before Vulkan init) to allow
        // background CSS/XML pre-parsing. Just pick up the pre-loaded assets here.
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& reg = am.GetRegistry();

        // Ensure core asset types are registered (defensive for editor builds/configs).
        RegisterCoreAssetTypes(am.GetAssetTypeRegistry());

        // Ensure editor-layer asset types are registered (factories are per type)
        RegisterEditorAssetTypes(am.GetAssetTypeRegistry());

        // Ensure the default UI font is registered so font-family resolution can hit the AssetRegistry fast.
        {
            const std::filesystem::path fontPath = m_AssetsDirectory / "Fonts" / "Roboto-Regular.ttf";
            if (!reg.IsAssetRegistered(fontPath))
            {
                reg.RegisterAsset(fontPath);
            }
        }

        tUiSub = std::chrono::high_resolution_clock::now();
        const WindowUiBootstrapResult mainBootstrap =
            BootstrapEditorWindowUi(main->ui.get(), &am, m_LayoutGuid, m_StyleGuid, "main");
        Logger::Log::Info("[Startup]   BootstrapEditorWindowUi: {:.1f}ms (layout={}, style={})",
                         MsSince(tUiSub), mainBootstrap.layoutLoaded, mainBootstrap.styleLoaded);

        // Deferred to the first update, not run here: an asset request issued
        // this early in bootstrap never completes. By the first update the
        // asset pipeline answers, and the icons load ahead of the first menu.
        m_DeferredPreUiActions.EnqueueUnique(
            "editor.warm-menu-icons",
            []() { MenuIcons::PreloadEditorIconAssets(); });

        // Register default inspectors (currently Transform)
        tUiSub = std::chrono::high_resolution_clock::now();
        RegisterBuiltInInspectors();

        // Load and apply accent color from preferences before UI is shown.
        ApplySavedAccentStyle(main->ui.get());
        SettingsPanel::ApplySavedCompactComponentHeadersStyle(main->ui.get());
        SettingsPanel::ApplySavedHierarchyIconsColoredStyle(main->ui.get());
        SettingsPanel::ApplySavedHierarchyModelThumbsAlwaysColoredStyle(main->ui.get());
        SettingsPanel::ApplySavedGraySlidersStyle(main->ui.get());
        SettingsPanel::ApplySavedToggleStyle(main->ui.get());
        SettingsPanel::ApplySavedTabIconsStyle(main->ui.get());
        SettingsPanel::ApplySavedPopupShadowStyle(main->ui.get());
        Editor::InfoCardAppearanceSettings::Get().ApplyTo(main->ui.get());
        SettingsPanel::ApplySavedRowGapStyles(main->ui.get());
        SettingsPanel::ApplySavedValueBoxHeightStyle(main->ui.get());
        SettingsPanel::ApplySavedNodeGraphHeaderAlignmentStyle(main->ui.get());
        SettingsPanel::ApplySavedTextSubpixelAA(main->ui.get());
        SettingsPanel::ApplySavedTextContrast(main->ui.get());
        SettingsPanel::ApplySavedTextSmoothingGamma(main->ui.get());
        SettingsPanel::ApplySavedInspectorBigNumberSpacing();
        Editor::ApplySavedEditorFontPreferences(main->ui.get());
        // After the font preferences: the line height resolves against the
        // script face those select.
        SettingsPanel::ApplySavedScriptLineHeightStyle(main->ui.get());
        Logger::Log::Info("[Startup]   Inspectors+ApplySaved: {:.1f}ms", MsSince(tUiSub));

        // Create project folder picker modal (always available, shown on demand)
        tUiSub = std::chrono::high_resolution_clock::now();
        {
            if (main->ui)
            {
                if (auto* rootEl = main->ui->GetRootElement())
                {
                    auto modal = std::make_unique<ProjectFolderPickerModal>();
                    modal->SetOnFolderSelected([this](const std::filesystem::path& path)
                                               {
                        m_SelectedProjectPath = path;
                        m_ProjectFolderSelected = true;
                        Logger::Log::Info("Editor: Project folder selected: {}", path.string());
                        
                        // Update the project root by reinitializing AssetManager
                        SetProjectFolder(path); });
                    modal->SetOnCancelled([this]()
                                          {
                        Logger::Log::Info("Editor: Project folder selection cancelled");
                        // For now, continue with default workspace
                        m_ProjectFolderSelected = false; });

                    m_ProjectFolderModal = modal.get();
                    rootEl->AddChild(std::move(modal));

                    const auto& config = GetConfig();
                    if (config.WorkspaceDirectory.empty() || config.WorkspaceDirectoryIsFallback)
                    {
                        // Shift held at launch always forces the project picker regardless of settings.
                        // Platform key state requires an event poll first, so use native APIs where possible.
                        bool shiftHeld = false;
#if defined(_WIN32)
                        shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#elif defined(__APPLE__)
                        {
                            CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
                            shiftHeld = (flags & kCGEventFlagMaskShift) != 0;
                        }
#else
                        {
                            // X11/Wayland: query platform key state after a brief poll
                            Platform::Window::PollEvents();
                            shiftHeld = (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->window) &&
                                (m_Windows[0]->window->IsKeyPressed(Input::kKeyCode_LeftShift) ||
                                 m_Windows[0]->window->IsKeyPressed(Input::kKeyCode_RightShift));
                        }
#endif

                        bool autoLoaded = false;
                        if (!shiftHeld)
                        {
                            // Same resolver ConfigureStartupDeviceDesc read for the
                            // startup device settings, so the swapchain was already
                            // built for this project.
                            if (const std::optional<std::filesystem::path> lastProject =
                                    ResolveAutoLoadProjectRoot(config))
                            {
                                Logger::Log::Info("Editor: Auto-loading last project: {}",
                                                  lastProject->string());
                                m_SelectedProjectPath = *lastProject;
                                m_ProjectFolderSelected = true;
                                if (m_ProjectFolderModal)
                                    m_ProjectFolderModal->SetPath(*lastProject);
                                SetProjectFolder(*lastProject);
                                autoLoaded = true;
                            }
                            else if (!config.AutoOpenLastProjectOnBoot)
                            {
                                Logger::Log::Info(
                                    "Editor: this host does not open the last project on boot; "
                                    "showing the picker");
                            }
                        }

                        if (!autoLoaded)
                        {
                            Logger::Log::Info("Editor: Showing project folder picker");
                            m_ProjectFolderModal->Show();

#if defined(__APPLE__)
                            try
                            {
                                const auto global = GameEngine::Editor::GetEditorGlobalPaths();
                                m_ProjectFolderModal->SetPath(global.defaultProjectRoot);
                            }
                            catch (...) {}
#endif
                        }
                    }
                    else
                    {
                        Logger::Log::Info("Editor: Using explicit project from command line: {}", config.WorkspaceDirectory);
                        std::filesystem::path explicitProject = std::filesystem::path(config.WorkspaceDirectory);
                        m_SelectedProjectPath = explicitProject;
                        m_ProjectFolderSelected = true;
                        if (m_ProjectFolderModal)
                        {
                            m_ProjectFolderModal->SetPath(explicitProject);
                        }
                        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
                        std::string err;
                        (void)prefs.Load(&err);
                        prefs.SetString("lastProjectPath", explicitProject.string());
                        (void)prefs.Save(&err);
                    }
                    SyncWorldPathForProjectPicker();
                }
            }
        }

        // Create play mode change review modal (always available, shown on demand)
        {
            if (main->ui)
            {
                if (auto* rootEl = main->ui->GetRootElement())
                {
                    auto modal = std::make_unique<PlayModeChangeReviewModal>();
                    modal->SetOnApply([this](const std::vector<bool>& keep)
                                      {
                                          if (m_PlayMode)
                                              m_PlayMode->ApplyPendingChanges(keep); });
                    modal->SetOnDiscard([this]()
                                        {
                                            if (m_PlayMode)
                                                m_PlayMode->DiscardPendingChanges(); });
                    m_PlayModeChangeReviewModal = modal.get();
                    rootEl->AddChild(std::move(modal));
                }
            }
        }

        // Mount download-in-progress modal.
        if (main->ui)
        {
            if (auto* rootEl = main->ui->GetRootElement())
            {
                auto dlModal = std::make_unique<DownloadInProgressModal>();
                m_DownloadModal = dlModal.get();
                rootEl->AddChild(std::move(dlModal));
            }
        }

        // Mount the intro video player modal (shown when user clicks the info toolbar button).
        if (main->ui)
        {
            if (auto* rootEl = main->ui->GetRootElement())
            {
                auto* device = main->renderCtx ? main->renderCtx->GetDevice() : nullptr;
                m_VideoPlayer.Setup(rootEl, device, main->ui.get());
            }
        }
        Logger::Log::Info("[Startup]   Modals+ProjectPicker: {:.1f}ms", MsSince(tUiSub));

        Logger::Log::Info("[Startup] UIManager + bootstrap: {:.1f}ms", MsSince(tPhase));

        // Seed the primary world with default scene entities (lighting, sky, geometry)
        // so the editor is immediately usable.
        tPhase = std::chrono::high_resolution_clock::now();
        if (ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld())
        {
            auto* rs = (m_Windows.size() > 0 && m_Windows[0]->renderCtx)
                          ? m_Windows[0]->renderCtx->GetRenderServices()
                          : nullptr;
            Editor::SeedDefaultSceneEntities(*world, rs);

            // Optional: additional test cubes for debugging (GE_EDITOR_BOOTSTRAP_WORLD=1).
            const char* env = std::getenv("GE_EDITOR_BOOTSTRAP_WORLD");
            if (env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0'))
                BootstrapEditorWorld(*world, rs);
        }

        Logger::Log::Info("[Startup] SceneSeeding: {:.1f}ms", MsSince(tPhase));

        // Build docking model and register editor panels.
        // Prefer the dock-config embedded in the loaded layout. If it is missing/invalid,
        // fall back to the fixed panel inventory (Editor::FallbackDockPanels) + default dock tree.
        tPhase = std::chrono::high_resolution_clock::now();
        auto tDockSub = tPhase;
        m_Docking = std::make_unique<DockingManager>();
        m_DefaultDockLayout.reset();
        m_PanelStorage.clear();

        EditorUI::EditorDockConfig dockCfg{};
        DockspaceElement* dockspaceForConfig = nullptr;
        if (auto* rootEl = main->ui->GetRootElement())
        {
            if (auto* el = rootEl->FindById("dock"))
            {
                dockspaceForConfig = dynamic_cast<DockspaceElement*>(el);
            }
        }

        Editor::RegisterBuiltInPanelTypes();

        auto buildFallbackDocking = [&]()
        {
            m_Docking = std::make_unique<DockingManager>();
            m_DefaultDockLayout.reset();
            m_PanelStorage.clear();

            for (const Editor::FallbackDockPanel& entry : Editor::FallbackDockPanels())
            {
                std::unique_ptr<UIElement> panel =
                    Editor::EditorPanelRegistry::Get().CreatePanelOfType(entry.TypeKey);
                if (!panel)
                {
                    Logger::Log::Error("Editor: fallback dock inventory names unregistered panel type '{}' (id '{}')",
                                       entry.TypeKey, entry.PanelId);
                    continue;
                }
                UIElement* raw = panel.get();
                m_PanelStorage.emplace_back(std::move(panel));
                m_Docking->RegisterPanel(entry.PanelId, raw);
                // Tell each Inspector which dock-tab id owns it so the lock icon mounts on the right tab.
                if (auto* ip = dynamic_cast<InspectorPanel*>(raw))
                    ip->SetDockTabPanelId(entry.PanelId);
            }

            // Default layout (legacy fallback): matches layout.uxml (Build stays in inventory only;
            // opening it uses EditorPanelManager::ShowOrActivateBuildPanel → split below Hierarchy).
            // Root: [ Workspace (left ~80%) | Inspector (right ~20%) ]
            // Workspace: vertical split [ Top (Hierarchy | SceneView) ~75% / Bottom (Assets…) ~25% ]
            auto root = DockNode::MakeSplit(DockPosition::Left, 0.80f);
            root->Second()->AddTab(EditorPanelIds::Inspector);
            root->First()->SetSplit(DockPosition::Top, 0.75f, DockNode::MakeLeaf(), DockNode::MakeLeaf());
            root->First()->Second()->AddTab(EditorPanelIds::Assets);
            root->First()->Second()->AddTab(EditorPanelIds::Log);
            root->First()->Second()->AddTab(EditorPanelIds::ScriptErrors);
            root->First()->Second()->AddTab(EditorPanelIds::ShaderErrors);
            root->First()->Second()->AddTab(EditorPanelIds::UiDemo);
            root->First()->First()->SetSplit(DockPosition::Left, 0.25f, DockNode::MakeLeaf(), DockNode::MakeLeaf());
            root->First()->First()->First()->AddTab(EditorPanelIds::Hierarchy);
            root->First()->First()->Second()->AddTab(EditorPanelIds::SceneView);
            root->First()->First()->Second()->AddTab(EditorPanelIds::GameView);

            m_DefaultDockLayout = CloneDockNode(root.get());
            m_Docking->SetRoot(std::move(root));
        };

        auto buildDockingFromLayoutConfig = [&]() -> bool
        {
            if (!dockspaceForConfig)
                return false;
            if (!EditorUI::TryParseEditorDockConfigFromDockspace(dockspaceForConfig, dockCfg))
                return false;

            // Instantiate panels from inventory.
            for (const auto& def : dockCfg.panels)
            {
                std::unique_ptr<UIElement> panel = Editor::EditorPanelRegistry::Get().CreatePanelOfType(def.type);
                if (!panel)
                {
                    Logger::Log::Warning("Editor: Dock-config references unknown panel type '{}' (id '{}')", def.type, def.id);
                    return false;
                }

                UIElement* raw = panel.get();
                // Install the override only when the markup authored one. An
                // authored icon="" is engaged-but-empty and must still be
                // installed: that is how a layout suppresses a declared icon.
                if (auto* dp = dynamic_cast<DockPanel*>(raw))
                {
                    if (def.icon)
                        dp->SetTabIcon(*def.icon);
                    dp->SetListedInPanelMenus(def.showInMenu);
                }
                m_PanelStorage.emplace_back(std::move(panel));
                m_Docking->RegisterPanel(def.id, raw);
                if (auto* ip = dynamic_cast<InspectorPanel*>(raw))
                    ip->SetDockTabPanelId(def.id);
                if (auto* mp = dynamic_cast<MissingAssetsPanel*>(raw); mp && m_MissingAssetTracker)
                    mp->SetTracker(m_MissingAssetTracker.get());
            }

            // Store a clone for ResetLayoutToDefault, then apply authored default.
            m_DefaultDockLayout = CloneDockNode(dockCfg.defaultLayout.root.get());
            m_Docking->SetRoot(std::move(dockCfg.defaultLayout.root));
            return m_Docking->GetRoot() != nullptr;
        };

        // In UI replay mode we prefer a stable, fully-known docking layout so scripted tests can
        // reliably target tabs/panels by id across machines and asset changes.
        if (m_UiReplayScenarioPath.has_value())
        {
            Logger::Log::Info("Editor: UI replay active; using fallback docking layout");
            buildFallbackDocking();
        }
        else if (buildDockingFromLayoutConfig())
        {
            Logger::Log::Info("Editor: Docking model built from layout.uxml");
        }
        else
        {
            Logger::Log::Info("Editor: Dock-config missing/invalid; using fallback docking layout");
            buildFallbackDocking();
        }

        // Panels exist now; give the Package Manager its data source (covers
        // the startup order where packages mounted before the dock was built).
        m_Packages->WirePanels();

        // Docking model exists: start consuming package panel / editor-chrome
        // stylesheet registrations (EditorSDK phase 2). Package Editor modules
        // load at project open, later than this; the registry replays anything
        // that somehow registered earlier.
        AttachPackagePanelConsumers();

        // Seed in-memory layout presets from the captured default layout.
        // In UI replay mode, do not load persisted/user presets: they make the UI tree
        // non-deterministic across machines and can remove panels needed by tests.
        InitializeLayoutPresets();
        if (!m_UiReplayScenarioPath.has_value())
        {
            LoadLayoutPresetsFromPreferences();
        }

        auto* mainWindowCtx = !m_Windows.empty() ? m_Windows[0].get() : nullptr;

        // Initialize panel manager with references to shared state.
        Logger::Log::Info("[Startup]   Panel creation + layout config: {:.1f}ms", MsSince(tPhase));
        tDockSub = std::chrono::high_resolution_clock::now();
        m_PanelManager = std::make_unique<EditorPanelManager>();
        m_PanelManager->SetDocking(m_Docking.get());
        m_PanelManager->SetPanelStorage(&m_PanelStorage);
        m_PanelManager->SetWindows(&m_Windows);
        m_PanelManager->SetIsLayoutDirtyCallback([this]() { return IsActiveLayoutDirty(); });
        m_PanelManager->SetFloatingFrames(&m_DockDnd->FloatingFrames());
        if (m_EditorToolbar)
            m_EditorToolbar->SetPanelManager(m_PanelManager.get());

        // Register per-frame update callbacks for panels that need them.
        // Panels stay decoupled from EditorPanelManager; wiring lives here.
        for (auto& p : m_PanelStorage)
        {
            if (auto* logPanel = dynamic_cast<LogPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(logPanel, [logPanel](const EditorPanelManager::PanelUpdateContext&) { logPanel->Update(); });
            else if (auto* scriptErrorsPanel = dynamic_cast<ScriptErrorsPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(scriptErrorsPanel, [scriptErrorsPanel](const EditorPanelManager::PanelUpdateContext&) { scriptErrorsPanel->Update(); });
            else if (auto* shaderErrorsPanel = dynamic_cast<ShaderErrorsPanel*>(p.get()))
                // alwaysUpdate=true: the version poll must run while the panel is
                // hidden — it is what reveals the panel on a failed shader compile.
                m_PanelManager->RegisterUpdateCallback(shaderErrorsPanel, [shaderErrorsPanel](const EditorPanelManager::PanelUpdateContext&) { shaderErrorsPanel->Update(); }, /*alwaysUpdate=*/true);
            else if (auto* monitorsPanel = dynamic_cast<MonitorsPanel*>(p.get()))
                // alwaysUpdate=true: PushBuiltInSamples (chart history) must accumulate
                // even when the panel is hidden so reopening it shows continuous data.
                m_PanelManager->RegisterUpdateCallback(monitorsPanel, [monitorsPanel](const EditorPanelManager::PanelUpdateContext&) { monitorsPanel->Update(); }, /*alwaysUpdate=*/true);
            else if (auto* rgPanel = dynamic_cast<RenderGraphPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(rgPanel, [rgPanel](const EditorPanelManager::PanelUpdateContext& ctx) { rgPanel->Update(ctx.renderDoc, ctx.rg2Frame); });
            else if (auto* cpuPanel = dynamic_cast<CpuProfilerPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(cpuPanel, [cpuPanel](const EditorPanelManager::PanelUpdateContext&) { cpuPanel->Update(); });
            else if (auto* visPanel = dynamic_cast<VisualProfilerPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(visPanel, [visPanel](const EditorPanelManager::PanelUpdateContext& ctx) { visPanel->Update(ctx.rg2Frame); });
            else if (auto* vramPanel = dynamic_cast<VramPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(vramPanel, [vramPanel](const EditorPanelManager::PanelUpdateContext& ctx) { vramPanel->Update(ctx.device, ctx.rg2Frame); });
            else if (auto* nodeGraphPanel = dynamic_cast<GraphPanel*>(p.get()))
                m_PanelManager->RegisterUpdateCallback(nodeGraphPanel, [nodeGraphPanel](const EditorPanelManager::PanelUpdateContext&) { nodeGraphPanel->Update(); });
            else if (auto* hierarchyPanelUpd = dynamic_cast<HierarchyPanel*>(p.get()))
                // Version-poll backstop: rebuilds when the world's entity set changes via a path
                // that doesn't emit EditorChangeNotifications (IPC bulk spawn, scripts, gameplay).
                // alwaysUpdate stays false — the poll is a cheap atomic read gated to the visible
                // panel, and a hidden hierarchy catches up the frame it is shown.
                m_PanelManager->RegisterUpdateCallback(hierarchyPanelUpd, [hierarchyPanelUpd](const EditorPanelManager::PanelUpdateContext&) { hierarchyPanelUpd->Update(); });
        }

        // Locate panels by type for editor wiring (avoid hardcoded indices).
        auto* assetsPanel = FindFirstPanelOfType<AssetsPanel>(m_PanelStorage);
        auto* assetViewPanel = FindFirstPanelOfType<AssetViewPanel>(m_PanelStorage);
        auto* todoPanel = FindFirstPanelOfType<TodoPanel>(m_PanelStorage);
        auto* bookmarksPanel = FindFirstPanelOfType<BookmarksPanel>(m_PanelStorage);
        auto* undoHistoryPanel = FindFirstPanelOfType<UndoHistoryPanel>(m_PanelStorage);
        auto* hierarchyPanel = FindFirstPanelOfType<HierarchyPanel>(m_PanelStorage);
        auto* inspectorPanel = FindFirstPanelOfType<InspectorPanel>(m_PanelStorage);
        auto* animationPanel = FindAnimationPanelOfKind(m_PanelStorage, AnimationWindowPanel::PanelKind::Animation);
        auto* timelinePanel = FindAnimationPanelOfKind(m_PanelStorage, AnimationWindowPanel::PanelKind::Timeline);
        auto* clipEditorPanel = FindAnimationPanelOfKind(m_PanelStorage, AnimationWindowPanel::PanelKind::ClipEditor);
        auto* sceneViewPanel = FindFirstPanelOfType<SceneViewPanel>(m_PanelStorage);
        auto* gameViewPanel = m_Docking
            ? dynamic_cast<GameViewPanel*>(m_Docking->GetPanel(EditorPanelIds::GameView))
            : FindFirstPanelOfType<GameViewPanel>(m_PanelStorage);
        auto* settingsPanel = FindFirstPanelOfType<SettingsPanel>(m_PanelStorage);
        auto* uiDemoPanel = FindFirstPanelOfType<UIDemoPanel>(m_PanelStorage);
        auto* logPanel = FindFirstPanelOfType<LogPanel>(m_PanelStorage);
        auto* webPanel = FindFirstPanelOfType<WebPanel>(m_PanelStorage);
        auto* nodeGraphPanel = FindFirstPanelOfType<GraphPanel>(m_PanelStorage);
        auto* monitorsPanel = FindFirstPanelOfType<MonitorsPanel>(m_PanelStorage);

        // Expose VCS service to panels via EditorContext (each panel can subscribe/unsubscribe).
        // Defer the expensive InitializeForProject (git detection, status) to first Update().
        if (!m_VcsService)
        {
            m_VcsService = std::make_unique<EditorVersionControlService>();
            if (m_EditorContext)
                m_EditorContext->VcsService = m_VcsService.get();
            m_PendingVcsInit = true;
        }

        // Connect the primary Scene View panel to the primary SceneViewController.
        if (sceneViewPanel && mainWindowCtx && mainWindowCtx->scene)
        {
            sceneViewPanel->SetSceneController(mainWindowCtx->scene.get());
        }

        if (gameViewPanel)
        {
            if (mainWindowCtx && mainWindowCtx->ui)
                gameViewPanel->PrepareForFirstMount(mainWindowCtx->ui.get());

            // pointerWindowId remembers which window received the last delivered
            // event. An off-surface end resolves no window — the panel may have
            // been undocked meanwhile — so it is the only handle on the controller
            // that still holds the gesture and has to be told the surface ended.
            gameViewPanel->SetPointerCallback(
                [this, gameViewPanel, pointerWindowId = uint64_t{0}]
                (bool over, float x, float y, bool down, int mods) mutable
                {
                    EditorWindowContext* destination = over ? FindWindowHostingGameView(gameViewPanel) : nullptr;
                    const uint64_t nextId = destination ? destination->windowId : 0;
                    if (pointerWindowId != nextId)
                        for (auto& win : m_Windows)
                            if (win && win->windowId == pointerWindowId && win->gameView)
                                win->gameView->HandleGameUiPointer(false, 0.0f, 0.0f, false, 0);
                    pointerWindowId = nextId;
                    if (destination)
                        destination->gameView->HandleGameUiPointer(true, x, y, down, mods);
                });

            gameViewPanel->SetActivationArmedCallback(
                [this, gameViewPanel](float contentWidth, float contentHeight)
                {
                    EditorWindowContext* host = FindWindowHostingGameView(gameViewPanel);
                    if (!host)
                        return;

                    // 1:1 with the panel rect. Render scale is per-view
                    // pipeline state, applied inside the pipeline.
                    const float contentScale = host->ui ? host->ui->GetContentScale() : 1.0f;
                    const uint32_t width =
                        static_cast<uint32_t>(std::max(1.0f, contentWidth * contentScale));
                    const uint32_t height =
                        static_cast<uint32_t>(std::max(1.0f, contentHeight * contentScale));
                    host->gameView->PrepareForActivation(
                        EngineCore::GetInstance().EnsurePrimaryWorld(), width, height);
                });
        }

        // Install a native menu invoker for the UI Demo panel's OS dropdown so it
        // can exercise platform-hosted menus when available.
        if (uiDemoPanel && mainWindowCtx && mainWindowCtx->window)
        {
            if (auto* nativeDropdown = uiDemoPanel->GetNativeDropdown())
            {
                Platform::Window* window = mainWindowCtx->window.get();
                nativeDropdown->SetNativeMenuInvoker(
                    [window](Dropdown& dropdown,
                             const std::vector<std::string>& labels,
                             int selectedIndex,
                             std::function<void(int)> onSelected)
                    {
                        if (!window || labels.empty())
                        {
                            return;
                        }

                        auto menu = CreateContextMenu();
                        if (!menu)
                        {
                            return;
                        }

                        constexpr uint32_t kBaseCommandId = 0x4000;
                        ContextMenuBuilder builder;
                        for (size_t i = 0; i < labels.size(); ++i)
                        {
                            const uint32_t cmd = kBaseCommandId + static_cast<uint32_t>(i);
                            uint32_t flags = MenuItemFlag_None;
                            if (selectedIndex >= 0 && static_cast<size_t>(selectedIndex) == i)
                            {
                                flags |= MenuItemFlag_Checked;
                            }
                            builder.AddItem(labels[i], cmd, flags);
                        }
                        builder.Build(menu.get());

                        menu->SetCommandHandler([labels,
                                                 onSelected = std::move(onSelected)](uint32_t cmd) mutable
                                                {
								if (!onSelected)
									return;
								if (cmd < kBaseCommandId)
									return;
								const uint32_t index = cmd - kBaseCommandId;
								if (index >= labels.size())
									return;
								onSelected(static_cast<int>(index)); });

                        // ContextMenu::Show converts UI-logical pixels to native points internally
                        // (uiContentScale / nativeContentScale). Pass GetLayoutX/Y directly.
                        float menuX = 0.0f;
                        float menuY = 0.0f;
                        Label* header = dropdown.GetHeaderLabel();
                        if (header)
                        {
                            menuX = header->GetLayoutX();
                            menuY = header->GetLayoutY() + header->GetLayoutHeight();
                        }

                        menu->Show(window, static_cast<int>(menuX), static_cast<int>(menuY));
                    });
            }
        }

        // Wire selections to Inspector
        if (inspectorPanel)
        {
            inspectorPanel->SetUndoRedoService(m_UndoRedo.get());
            inspectorPanel->SetChangeNotifications(m_ChangeNotifications.get());
            if (m_VcsUi)
                m_VcsUi->BindInspectorPanel(*inspectorPanel);
            inspectorPanel->SetOnHistoryEntityNavigate(
                [mainWindowCtx, inspectorPanel](ECS::World* world, ECS::EntityHandle entity)
                {
                    if (!world || !entity.IsValid() || !world->IsValid(entity))
                        return;
                    if (mainWindowCtx && mainWindowCtx->scene)
                        mainWindowCtx->scene->OnEntityPicked(entity);
                    else if (inspectorPanel)
                        inspectorPanel->ShowEntity(world, entity, true, /*keepMultiSelection=*/false);
                });
            inspectorPanel->SetOnHistoryEntitiesNavigate(
                [mainWindowCtx, inspectorPanel](ECS::World* world,
                                                const std::vector<ECS::EntityHandle>& entities)
                {
                    if (!world || entities.empty())
                        return;
                    if (mainWindowCtx && mainWindowCtx->scene)
                        mainWindowCtx->scene->OnEntitiesMarqueeSelected(entities, false);
                    if (inspectorPanel)
                        inspectorPanel->ShowEntities(world, entities);
                });
            inspectorPanel->SetOpenColorPickerWindow(m_ColorPicker->AsOpenCallback());
        }

        // Wire ScriptEditorPanel to InspectorPanel for variable inspection
        ScriptEditorPanel* scriptEditorPanel = FindFirstPanelOfType<ScriptEditorPanel>(m_PanelStorage);
        if (scriptEditorPanel)
        {
            scriptEditorPanel->SetUndoRedoService(m_UndoRedo.get());
        }
        const Editor::SourceLocationOpener sourceOpener(*m_PanelManager, mainWindowCtx, scriptEditorPanel);
        if (scriptEditorPanel && inspectorPanel)
        {
            // When a C# script is opened, show its variables and methods in the Inspector
            scriptEditorPanel->SetOnCSharpScriptOpened(
                [inspectorPanel, scriptEditorPanel](const std::filesystem::path& path, const std::vector<ScriptVariable>& vars)
                {
                    inspectorPanel->ShowScriptVariables(path, vars, scriptEditorPanel->GetScriptMethods());
                });

            inspectorPanel->SetOnHistoryScriptNavigate(
                [scriptEditorPanel](const std::filesystem::path& path)
                {
                    scriptEditorPanel->OpenScript(path);
                });

            // When a variable is edited in the Inspector, update it in the script
            inspectorPanel->SetOnVariableEdited(
                [scriptEditorPanel](const std::string& varName, const std::string& newValue)
                {
                    scriptEditorPanel->UpdateVariableValue(varName, newValue);
                });

            // When a variable field is focused in Inspector, highlight it in the script
            inspectorPanel->SetOnVariableFocused(
                [scriptEditorPanel](const std::string& varName)
                {
                    scriptEditorPanel->HighlightVariable(varName);
                });

            // When a variable field loses focus, clear the highlight
            inspectorPanel->SetOnVariableUnfocused(
                [scriptEditorPanel]()
                {
                    scriptEditorPanel->ClearVariableHighlight();
                });

            // When script variables change (user edits script), update Inspector
            scriptEditorPanel->SetOnScriptVariablesChanged(
                [inspectorPanel](const std::vector<ScriptVariable>& vars)
                {
                    inspectorPanel->UpdateScriptVariableValues(vars);
                });

            // When script methods change (user edits script), rebuild Inspector members outline
            scriptEditorPanel->SetOnScriptMembersChanged(
                [inspectorPanel, scriptEditorPanel](const std::vector<ScriptMethod>& methods)
                {
                    const auto& path = scriptEditorPanel->GetCurrentScriptPath();
                    const auto& vars = scriptEditorPanel->GetScriptVariables();
                    if (!path.empty())
                        inspectorPanel->ShowScriptVariables(path, vars, methods);
                });

            // Clicking a function in the members outline jumps the script editor to that line
            inspectorPanel->SetOnScriptMethodNavigate(
                [scriptEditorPanel](size_t lineNumber, const std::string& name)
                {
                    scriptEditorPanel->ScrollToLine(lineNumber, name);
                });

            // When caret/selection in script is in a serialized value, highlight the
            // matching Inspector field. Never steal focus — that would break mid-drag
            // text selection in the script editor by pulling focus to the Inspector.
            scriptEditorPanel->SetOnCaretPositionChanged(
                [inspectorPanel](const std::string& varName, bool /*focusInspectorField*/)
                {
                    if (varName.empty())
                        inspectorPanel->ClearVariableFieldHighlight();
                    else
                        inspectorPanel->HighlightVariableField(varName);
                });
        }

        // Hot-reload C# scripts on save (works in both edit and play mode)
        if (scriptEditorPanel)
        {
            scriptEditorPanel->SetOnCSharpScriptSaved(
                [](const std::filesystem::path&)
                {
                    auto& sm = EngineCore::GetInstance().GetScriptManager();
                    if (!sm.IsAsyncHotReloadInProgress())
                        sm.RecompileAndReload();
                });
        }

        // Wire ScriptErrorsPanel double-click to the user's script-open preference.
        if (auto* scriptErrorsPanel = FindFirstPanelOfType<ScriptErrorsPanel>(m_PanelStorage))
        {
            if (mainWindowCtx && mainWindowCtx->window)
                scriptErrorsPanel->SetWindow(mainWindowCtx->window.get());

            scriptErrorsPanel->SetOnOpenInEditor(
                [sourceOpener](const std::filesystem::path& path, size_t line, size_t column)
                { sourceOpener.Open({path, line, column}); });
            scriptErrorsPanel->SetOnOpenInScriptEditor(
                [sourceOpener](const std::filesystem::path& path, size_t line, size_t column)
                { sourceOpener.OpenInScriptEditor({path, line, column}); });
        }

        // Shader Errors: same open-in-editor routing as Script Errors, plus
        // auto-reveal — a failed material shader compile activates the tab so
        // a broken surface save is seen without selecting the material.
        if (auto* shaderErrorsPanel = FindFirstPanelOfType<ShaderErrorsPanel>(m_PanelStorage))
        {
            if (mainWindowCtx && mainWindowCtx->window)
                shaderErrorsPanel->SetWindow(mainWindowCtx->window.get());

            shaderErrorsPanel->SetOnOpenInEditor(
                [sourceOpener](const std::filesystem::path& path, size_t line, size_t column)
                { sourceOpener.Open({path, line, column}); });
            shaderErrorsPanel->SetOnOpenInScriptEditor(
                [sourceOpener](const std::filesystem::path& path, size_t line, size_t column)
                { sourceOpener.OpenInScriptEditor({path, line, column}); });
            shaderErrorsPanel->SetOnRequestReveal(
                [this]()
                {
                    if (m_PanelManager)
                        OpenPanel(EditorPanelIds::ShaderErrors);
                });
        }

        GraphPanel::ForEachLive([this](GraphPanel& panel) { BindGraphPanel(panel); });
        if (inspectorPanel)
        {
            Logger::Log::Info("EditorApplication: Wiring GraphPanel -> InspectorPanel callback");
            inspectorPanel->SetOnGraphNodeParameterChanged(
                [](const std::string& nodeId, const std::string& key,
                   const std::string& value, bool commitUndo) {
                    if (GraphPanel* panel = GraphPanel::InspectorTarget())
                        panel->SetNodeParameter(nodeId, key, value, commitUndo);
                });
            inspectorPanel->SetOnGraphVariableValueChanged(
                [](const std::string& variableName, const std::string& value, bool commitUndo) {
                    if (GraphPanel* panel = GraphPanel::InspectorTarget())
                        panel->EditGraphVariableFromInspector(variableName, value, commitUndo);
                });
            inspectorPanel->SetOnGraphTransitionChanged(
                [](const std::string& linkId, const GraphTransitionDesc& desc, bool commitUndo) {
                    if (GraphPanel* panel = GraphPanel::InspectorTarget())
                        panel->SetTransitionFromInspector(linkId, desc, commitUndo);
                });
        }
        else if (nodeGraphPanel)
        {
            Logger::Log::Error("EditorApplication: graph panel exists but inspectorPanel is NULL!");
        }

        if (monitorsPanel)
        {
            monitorsPanel->SetUndoRedoService(m_UndoRedo.get());
        }
        if (hierarchyPanel)
        {
            hierarchyPanel->SetUndoRedoService(m_UndoRedo.get());
            hierarchyPanel->SetChangeNotifications(m_ChangeNotifications.get());

            // Synchronous refresh so the panel picks up entities seeded before
            // it was created. PostAction doesn't work here — the UI dispatcher
            // isn't wired until the dock-bound block below (UiContextScope).
            hierarchyPanel->Refresh();
        }
        ForEachAnimationPanel(m_PanelStorage, [this, main, sceneViewPanel, assetViewPanel, assetsPanel, inspectorPanel](AnimationWindowPanel* panel)
        {
            panel->SetUndoRedoService(m_UndoRedo.get());
            panel->SetOpenColorPickerWindow(m_ColorPicker->AsOpenCallback());
            panel->SetOnOpenSettings([this, main]()
            {
                if (m_PanelManager && main)
                    m_PanelManager->ShowOrActivateSettingsPanelToCategory(main, SettingsCategory::Animation);
            });
            panel->SetOnOpenTimelinePanel([this, main]()
            {
                if (m_PanelManager && main)
                    m_PanelManager->ShowOrActivatePanel(main, EditorPanelIds::Timeline, EditorPanelIds::Animation);
            });
            panel->SetOnOpenAnimationPanel([this, main]()
            {
                if (m_PanelManager && main)
                    m_PanelManager->ShowOrActivatePanel(main, EditorPanelIds::Animation, EditorPanelIds::Timeline);
            });
            panel->SetOnOpenClipEditorPanel([this, main]()
            {
                if (m_PanelManager && main)
                    m_PanelManager->ShowOrActivatePanel(main, EditorPanelIds::ClipEditor, EditorPanelIds::Animation);
            });
            panel->SetOnPreviewToggled([this, panel, sceneViewPanel, assetViewPanel](bool enabled)
            {
                const std::filesystem::path& path = panel->GetPreviewModelPath();
                if (sceneViewPanel)
                    sceneViewPanel->SetAssetPreview(path, enabled && !path.empty());
                if (assetViewPanel)
                    assetViewPanel->SetAssetPreview(path, enabled && !path.empty());
                m_CurrentAssetPreviewEnabled = enabled;
                if (AssetViewPanel* sceneAssetViewPanel =
                        EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy))
                    sceneAssetViewPanel->SetAssetPreview(path, enabled && !path.empty());
            });
            panel->SetOnClipSourcePreview([assetViewPanel](const std::filesystem::path& path)
            {
                if (assetViewPanel)
                    assetViewPanel->SetAssetPreview(path, !path.empty());
            });
            if (assetsPanel)
            {
                panel->SetOnRevealInAssetsPanel([assetsPanel](const std::filesystem::path& path)
                {
                    if (!path.empty())
                        assetsPanel->NavigateToAndSelectAsset(path);
                });
            }
            if (inspectorPanel)
            {
                panel->SetOnTimelineInspectorRequested(
                    [inspectorPanel](const std::string& title, std::function<void(UIElement*)> buildContent)
                    {
                        inspectorPanel->ShowCustomInspector(title, "inspector-header-kind-node", std::move(buildContent));
                    });
            }
        });
        if (inspectorPanel && hierarchyPanel)
            inspectorPanel->SetSelectEntity([hierarchyPanel](ECS::EntityHandle entity)
                                            { hierarchyPanel->SelectEntity(entity); });
        if (assetsPanel && inspectorPanel)
        {
            // Wire undo/redo for asset selection (list/grid) and selection → inspector.
            assetsPanel->SetUndoRedoService(m_UndoRedo.get());

            inspectorPanel->SetGetSelectedAssetPaths([assetsPanel]() -> std::vector<std::filesystem::path>
                                                     { return assetsPanel->GetSelectedAssetPaths(); });
            inspectorPanel->SetPingAsset([assetsPanel](const std::filesystem::path& path)
                                         { assetsPanel->NavigateToAndSelectAsset(path); });
            inspectorPanel->SetPingAssetPreserveInspector([assetsPanel](const std::filesystem::path& path)
                                                          { assetsPanel->NavigateToAndSelectAssetSilent(path); });
            inspectorPanel->SetOpenScript([sourceOpener](const std::filesystem::path& path)
                                          {
                                              if (!path.empty())
                                                  sourceOpener.OpenInScriptEditor({path});
                                          });
            inspectorPanel->SetOnHistoryAssetNavigate(
                [assetsPanel](const std::vector<std::filesystem::path>& paths)
                {
                    if (paths.empty())
                        return;
                    // Navigate to the item's parent directory and select it, whether it's a
                    // file or a folder. For folders this means the folder is highlighted
                    // inside its parent's grid view (selected) rather than opened.
                    assetsPanel->NavigateToAndSelectAssetSilent(paths.front());
                });
            if (hierarchyPanel)
            {
                hierarchyPanel->SetPingAsset([assetsPanel](const std::filesystem::path& path)
                                             { assetsPanel->NavigateToAndSelectAsset(path); });
            }
            assetsPanel->SetOnSelectAssets([inspectorPanel](const std::vector<std::filesystem::path>& paths)
                                           {
                                               inspectorPanel->ShowSelectedAssets(paths);
                                           });
            // Wire smart folder selection to inspector
            assetsPanel->SetOnSmartFolderSelected([inspectorPanel](const std::string& id, SmartFolderManager* mgr)
                                           {
                                               if (id.empty() || !mgr) {
                                                   inspectorPanel->ShowSelectedAssets({});
                                               } else {
                                                   inspectorPanel->ShowSmartFolder(id, mgr);
                                               }
                                           });
        }
        if (assetsPanel && (sceneViewPanel || assetViewPanel || m_Docking))
        {
            assetsPanel->SetOnAssetPreviewChanged([this, sceneViewPanel, assetViewPanel](const std::filesystem::path& path, bool enabled)
                                                  {
                m_CurrentAssetPreviewPath = path;
                m_CurrentAssetPreviewEnabled = enabled;
                if (sceneViewPanel)
                    sceneViewPanel->SetAssetPreview(path, enabled);
                if (assetViewPanel)
                    assetViewPanel->SetAssetPreview(path, enabled);
                if (AssetViewPanel* sceneAssetViewPanel = EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy))
                    sceneAssetViewPanel->SetAssetPreview(path, enabled);
            });
            assetsPanel->SetOnVideoPreviewBindingRefresh([assetViewPanel, docking = m_Docking.get()]()
                                                         {
                if (assetViewPanel)
                    assetViewPanel->RefreshVideoPreviewBindingIfActive();
                if (AssetViewPanel* sceneAssetViewPanel =
                        EditorApplication::FindAssetViewPanelById(docking, EditorPanelIds::AssetViewSceneCopy))
                    sceneAssetViewPanel->RefreshVideoPreviewBindingIfActive();
            });
        }
        const std::function<void(const std::filesystem::path&)> openInMaterialGraph =
            [this](const std::filesystem::path& path)
        {
            if (path.empty())
                return;
            (void)OpenGraphAsset(path);
        };

        // One open-asset policy for every entry point — Assets browser double-click and
        // context-menu Open, inspector "Open ..." buttons. ResolveAssetOpenTarget decides;
        // this lambda only executes the decision.
        const std::function<void(const std::filesystem::path&)> openAsset =
            [this, assetsPanel, sourceOpener, main, assetViewPanel,
             animationPanel, timelinePanel, clipEditorPanel](const std::filesystem::path& path)
        {
            if (path.empty())
                return;

            std::string ext = path.extension().string();
            for (auto& ch : ext)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            Editor::AssetOpenRoutingInputs inputs;
            inputs.OpenCSharpInScriptEditor = Editor::ScriptEditorSettings::Get().GetOpenInScriptInspector();
            inputs.OpenNativeSourceInScriptEditor =
                Editor::ScriptEditorSettings::Get().GetOpenNativeSourceInScriptInspector();
            inputs.GlslOpensInMaterialGraph = ext == ".glsl" && Editor::ShouldOpenGlslInMaterialGraph(path);

            const Editor::AssetOpenTarget target = Editor::ResolveAssetOpenTarget(ext, inputs);
            switch (target)
            {
            case Editor::AssetOpenTarget::Scene:
                if (m_SceneEditor && m_SceneEditor->HandleAssetOpen(path))
                    return;
                break;
            case Editor::AssetOpenTarget::ScriptEditor:
                if (sourceOpener.OpenInScriptEditor({path}))
                    return;
                if (ext == ".cs" && ExternalScriptEditorLauncher::OpenScript(path))
                    return;
                break;
            case Editor::AssetOpenTarget::ExternalScriptEditor:
                if (ExternalScriptEditorLauncher::OpenScript(path))
                    return;
                break;
            case Editor::AssetOpenTarget::ExternalIde:
            {
                // The C++-aware launcher reuses the running IDE instance and focuses the
                // file (VS Code also opens the project folder).
                std::filesystem::path projectDir;
                if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
                    projectDir = nativeScripts->WatchDirectory();
                if (Platform::OpenSourceWithProject(path, projectDir))
                    return;
                break;
            }
            case Editor::AssetOpenTarget::NodeGraph:
                if (OpenGraphAsset(path))
                    return;
                break;
            case Editor::AssetOpenTarget::Inspector:
                // A material is edited in the Inspector, so opening it means selecting
                // it and bringing that panel forward.
                if (assetsPanel)
                {
                    const std::vector<std::filesystem::path>& selected = assetsPanel->GetSelectedAssetPaths();
                    if (selected.size() != 1 || selected.front() != path)
                        assetsPanel->NavigateToAndSelectAsset(path);
                }
                if (m_PanelManager)
                    m_PanelManager->ShowOrActivatePanel(main, EditorPanelIds::Inspector);
                return;
            case Editor::AssetOpenTarget::Animation:
            case Editor::AssetOpenTarget::Timeline:
            case Editor::AssetOpenTarget::ClipEditor:
            {
                AnimationWindowPanel* targetPanel = animationPanel;
                const char* targetPanelId = EditorPanelIds::Animation;
                if (target == Editor::AssetOpenTarget::Timeline)
                {
                    targetPanel = timelinePanel;
                    targetPanelId = EditorPanelIds::Timeline;
                }
                else if (target == Editor::AssetOpenTarget::ClipEditor)
                {
                    targetPanel = clipEditorPanel;
                    targetPanelId = EditorPanelIds::ClipEditor;
                }
                if (!targetPanel)
                    targetPanel = animationPanel ? animationPanel
                                                 : FindFirstPanelOfType<AnimationWindowPanel>(m_PanelStorage);
                if (!targetPanel)
                    break;
                if (m_PanelManager)
                    m_PanelManager->ShowOrActivatePanel(main, targetPanelId, EditorPanelIds::SceneView);
                if (!targetPanel->OpenAnimationAssetPath(path))
                    break;
                const std::filesystem::path& previewPath = targetPanel->GetPreviewModelPath();
                if (!previewPath.empty())
                {
                    if (assetViewPanel)
                        assetViewPanel->SetAssetPreview(previewPath, true);
                    m_CurrentAssetPreviewPath = previewPath;
                    m_CurrentAssetPreviewEnabled = true;
                    if (AssetViewPanel* sceneAssetViewPanel = EditorApplication::FindAssetViewPanelById(
                            m_Docking.get(), EditorPanelIds::AssetViewSceneCopy))
                    {
                        sceneAssetViewPanel->SetAssetPreview(previewPath, true);
                    }
                }
                return;
            }
            case Editor::AssetOpenTarget::OperatingSystem:
                break;
            }

            // Nothing in the editor took the asset (or its launcher failed): the OS
            // default application is the last resort.
            Platform::OpenPath(path);
        };
        if (assetsPanel)
            assetsPanel->SetOnOpenAsset(openAsset);
        if (inspectorPanel)
        {
            inspectorPanel->SetOpenAsset(openAsset);
            inspectorPanel->SetOpenMaterialGraph(openInMaterialGraph);
        }
        Editor::InstallEditorAssetActions(
            {m_ThumbnailProvider.get(), m_EditorContext->ThumbnailHostWindowId, openAsset, m_PanelManager.get(), main,
             assetViewPanel});

        // Log double-click and stack-frame clicks open the named source location.
        if (logPanel)
        {
            logPanel->SetOnOpenInIDE([sourceOpener](const std::string& lineText)
                                     { sourceOpener.OpenLogLine(lineText); });
            logPanel->SetOnOpenSourceFile([sourceOpener](const std::string& file, int line)
                                          { sourceOpener.OpenLogSourceFile(file, line); });
        }

        // Context-menu "Edit (Internal)" for text-ish assets:
        // Defer to Update() so we never rebuild dockspace during native/menu callbacks.
        if (assetsPanel)
        {
            assetsPanel->SetOnAddToBookmarks([this, bookmarksPanel](const std::vector<std::filesystem::path>& paths)
            {
                if (!bookmarksPanel || paths.empty())
                    return;
                bookmarksPanel->HandleAssetDrops(paths);
            });
            
            assetsPanel->SetOnEditAsset([this](const std::filesystem::path& path)
                                        {
                                            if (path.empty())
                                                return;
                                            std::string ext = path.extension().string();
                                            for (auto& ch : ext)
                                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                            const bool scriptOrPipeline =
                                                ext == ".cs" || ext == ".rendergraph" ||
                                                ext == ".renderpipeline" || ext == ".hlsl";
                                            if (!scriptOrPipeline && !ExtensionOpensInGraphPanel(ext))
                                                return;
                                            this->QueueOpenInternalFile(path); });
        }
        // Keep Hierarchy selection in sync with the primary Scene View selection.
        // This ensures global shortcuts (e.g. Frame Selection) operate on the
        // same selection regardless of whether the user clicked in the viewport
        // or in the hierarchy tree.
        if (hierarchyPanel)
        {
            hierarchyPanel->SetOnSelectEntity(
                [mainWindowCtx, inspectorPanel](ECS::EntityHandle entity)
                {
                    if (mainWindowCtx && mainWindowCtx->scene)
                    {
                        // Route through SceneViewController so it updates the
                        // selection gizmo + transform tool target, and then
                        // propagates selection to the Inspector via its callback.
                        mainWindowCtx->scene->OnEntityPicked(entity);
                        return;
                    }

                    // Fallback: if no Scene View exists yet, at least update the Inspector.
                    if (inspectorPanel)
                    {
                        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                        inspectorPanel->ShowEntity(world, entity, /*force=*/false, /*keepMultiSelection=*/false);
                    }
                });

            // Multi-entity selection → inspector multi-edit + scene view gizmo /
            // transform-tool pivot. Routing through OnEntitiesMarqueeSelected
            // replaces the controller's internal selection set with the full
            // list; the single-entity OnEntityPicked path would only push the
            // anchor and race the hierarchy back to single on the next sync.
            hierarchyPanel->SetOnSelectEntities(
                [mainWindowCtx, inspectorPanel](const std::vector<ECS::EntityHandle>& entities)
                {
                    if (mainWindowCtx && mainWindowCtx->scene)
                    {
                        mainWindowCtx->scene->OnEntitiesMarqueeSelected(entities, false);
                    }
                    if (inspectorPanel)
                    {
                        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                        inspectorPanel->ShowEntities(world, entities);
                    }
                });

            // Hover preview (Hierarchy -> Scene View outline).
            hierarchyPanel->SetOnHoverEntity(
                [mainWindowCtx](ECS::EntityHandle entity)
                {
                    if (mainWindowCtx && mainWindowCtx->scene)
                    {
                        mainWindowCtx->scene->SetHoverEntity(
                            entity,
                            false,
                            SceneViewController::HoverEntitySource::Panel);
                    }
                });

            // Double-click in hierarchy: frame the entity selected by the row click.
            // Do not route the selection through OnEntityPicked again: that callback treats the
            // pick as Scene View-originated and reveals it by scrolling the hierarchy tree.
            hierarchyPanel->SetOnHierarchyItemActivated(
                [mainWindowCtx](ECS::EntityHandle entity)
                {
                    if (!entity.IsValid() || !mainWindowCtx || !mainWindowCtx->scene)
                        return;
                    mainWindowCtx->scene->FrameOrigin();
                });

            hierarchyPanel->SetOnNewScene([this]()
            {
                if (m_SceneEditor)
                    m_SceneEditor->RequestNewScene();
            });
            hierarchyPanel->SetRecentScenesProvider([this]() -> std::vector<std::filesystem::path>
            {
                if (m_SceneEditor)
                    return m_SceneEditor->GetRecentScenePaths();
                return {};
            });
            hierarchyPanel->SetOnOpenRecentScene([this](const std::filesystem::path& path)
            {
                if (m_SceneEditor)
                    m_SceneEditor->RequestOpenScene(path, /*additive=*/false);
            });

            if (mainWindowCtx)
            {
                Editor::SceneViewRenderCoordinator::ForEachController(
                    mainWindowCtx,
                    [hierarchyPanel](SceneViewController* sceneController)
                    {
                        sceneController->SetIsEntityPickable(
                            [hierarchyPanel](ECS::EntityHandle entity) -> bool
                            {
                                return !hierarchyPanel->IsEntityLocked(entity);
                            });
                    });
            }

            // Note: We no longer auto-frame newly created entities.
            // The camera stays in place when creating cubes, spheres, etc.
        }
        Logger::Log::Info("[Startup]     SceneEditor+Hierarchy wiring: {:.1f}ms", MsSince(tDockSub));
        tDockSub = std::chrono::high_resolution_clock::now();
        // Scene View selection uses the same Inspector wiring via SceneViewController.
        if (inspectorPanel && mainWindowCtx)
        {
            ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
            Editor::SceneViewRenderCoordinator::ForEachController(
                mainWindowCtx,
                [inspectorPanel, world, hierarchyPanel, mainWindowCtx](SceneViewController* sceneController)
                {
                    sceneController->SetOnSelectEntityCallback(
                        [inspectorPanel, world, hierarchyPanel, mainWindowCtx, sceneController](ECS::EntityHandle entity)
                        {
                            Editor::SceneViewRenderCoordinator::SyncSelectionAcrossControllers(
                                mainWindowCtx, sceneController);
                            if (hierarchyPanel)
                            {
                                const auto& selected = sceneController->GetSelectedEntities();
                                if (selected.empty())
                                    hierarchyPanel->SyncSelectionWithSceneViewPick({});
                                else if (selected.size() == 1)
                                    hierarchyPanel->SyncSelectionWithSceneViewPick(selected.front());
                                else
                                    hierarchyPanel->SelectEntities(selected);
                            }
                            if (!entity.IsValid())
                                inspectorPanel->ShowSelectedAssets({});
                            else
                                inspectorPanel->ShowEntity(world, entity, /*force=*/false, /*keepMultiSelection=*/false);
                        });
                    // Multi-entity selection from scene view (marquee/lasso) -> hierarchy
                    // multi-select + inspector multi-edit. Empty list means the marquee
                    // cleared the selection; both panels reset in that case.
                    sceneController->SetOnSelectEntitiesCallback(
                        [inspectorPanel, hierarchyPanel, mainWindowCtx, sceneController](const std::vector<ECS::EntityHandle>& entities)
                        {
                            Editor::SceneViewRenderCoordinator::SyncSelectionAcrossControllers(
                                mainWindowCtx, sceneController);
                            if (hierarchyPanel)
                            {
                                if (entities.empty())
                                    hierarchyPanel->SyncSelectionWithSceneViewPick({});
                                else
                                    hierarchyPanel->SelectEntities(entities);
                            }
                            if (inspectorPanel)
                            {
                                ECS::World* w = EngineCore::GetInstance().EnsurePrimaryWorld();
                                if (entities.empty())
                                    inspectorPanel->ShowSelectedAssets({});
                                else
                                    inspectorPanel->ShowEntities(w, entities);
                            }
                        });
                });
        }

        // Bind docking model to <dockspace id="dock"> in the loaded layout and build.
        // On first run or missing layout, "dock" may be null; we still pass context to panels so they don't crash when used.
        EditorContext* const editorCtx = m_EditorContext.get();
        bool dockBound = false;
        if (auto* rootEl = main->ui->GetRootElement())
        {
            if (auto* el = rootEl->FindById("dock"))
            {
                if (auto* dock = dynamic_cast<DockspaceElement*>(el))
                {
                    dock->SetOnPostRebuild([this]() {
                        // Re-mount the lock icon on every inspector tab — both Inspector and Inspector 2
                        // (and any future instances) need their tab chrome restored after a rebuild.
                        for (const auto& panel : m_PanelStorage)
                        {
                            if (auto* ip = dynamic_cast<InspectorPanel*>(panel.get()))
                                ip->PostAction([ip]() { ip->EnsureInspectorTabLockMounted(); });
                        }
                    });
                    dock->SetOnTabContextMenu([this, main](const std::string& panelId, float x, float y) {
                        ShowTabContextMenu(panelId, x, y, main->window.get());
                    });
                    dock->BindModel(m_Docking.get());
                    UI::UiContextScope uiScope(main->ui->GetDispatcher(), main->ui->GetScheduler());
                    tDockSub = std::chrono::high_resolution_clock::now();
                    dock->RebuildFromModel();
                    Logger::Log::Info("[Startup]   RebuildFromModel: {:.1f}ms", MsSince(tDockSub));
                    dockBound = true;
                    tDockSub = std::chrono::high_resolution_clock::now();
                    MigrateLegacySharedTreePreferences();

                    // Load preferences once for all panel wiring (avoids 4+ redundant file reads + JSON parses).
                    auto cachedPrefs = Editor::OpenEditorPreferences();
                    { std::string err; cachedPrefs.Load(&err); }

                    Logger::Log::Info("[Startup]     Prefs+Migrate: {:.1f}ms", MsSince(tDockSub));
                    tDockSub = std::chrono::high_resolution_clock::now();

                    // Now that panels are attached, provide shared EditorContext to Assets panel
                    if (assetsPanel && editorCtx)
                    {
                        assetsPanel->SetContext(editorCtx);
                        assetsPanel->SetOnOpenSettingsToTags([this, main]() {
                            if (main && m_PanelManager)
                                m_PanelManager->ShowOrActivateSettingsPanelToCategory(main, SettingsCategory::Tags);
                        });
                        // Assets folder tree row height (separate from Hierarchy).
                        {
                            double stored = 20.0;
                            cachedPrefs.TryGetDouble("ui.assetsTreeRowHeight", stored);
                            const float rowH = std::clamp(static_cast<float>(stored), kMinEditorTreeRowHeightPx,
                                                          kMaxEditorTreeRowHeightPx);
                            assetsPanel->SetTreeRowHeight(rowH);
                        }

                        // Version control UI callbacks (commit/log dialogs and diff panel).
                        if (m_VcsUi && main && main->ui && m_Docking)
                        {
                            auto* diffPanel = FindFirstPanelOfType<DiffPanel>(m_PanelStorage);
                            m_VcsUi->BindAssetsPanel(*assetsPanel, *main->ui, diffPanel, m_Docking.get());
                        }
                    }
                    Logger::Log::Info("[Startup]     Assets+VCS+Bookmarks wiring: {:.1f}ms", MsSince(tDockSub));
                    tDockSub = std::chrono::high_resolution_clock::now();
                    // Attach scene document UX modals to the root once the UI tree exists.
                    if (m_SceneEditor)
                    {
                        if (auto* world = EngineCore::GetInstance().EnsurePrimaryWorld())
                        {
                            m_SceneEditor->AttachToRoot(*rootEl, *world, *m_ChangeNotifications, m_UndoRedo.get());
                        }
                    }
                    // The "Import Unity Package" modal ships in the
                    // unity-import engine package: its module registers the
                    // modal overlay + Tools-menu item, and the editor's
                    // overlay consumer (ConsumePackageOverlay) attaches it to
                    // this root when the package loads at project open.
                    if (main && main->window && m_SceneEditor)
                    {
                        main->window->SetCloseRequestedHandler([this]() {
                            return TryInterceptQuitForDirtyScene();
                        });
                    }
        if (hierarchyPanel && m_SceneEditor)
        {
            if (m_VcsUi)
                m_VcsUi->BindHierarchyPanel(*hierarchyPanel);
            hierarchyPanel->SetPendingHierarchyUiProvider([this]() -> std::optional<Scene::SceneHierarchyUiFromFile> {
                            if (!m_SceneEditor)
                                return std::nullopt;
                            return m_SceneEditor->ConsumeHierarchyUiFromLastDocumentLoad();
                        });
                        m_SceneEditor->SetHierarchyUiCaptureForSave([hierarchyPanel]() {
                            return hierarchyPanel->CaptureHierarchyUiForSceneSave();
                        });
                    }
                    if (m_SceneEditor)
                    {
                        m_SceneEditor->SetEditorCameraCaptureForSave([this]() -> Scene::SceneEditorCamera {
                            Scene::SceneEditorCamera out{};
                            SceneViewController* sceneController = nullptr;
                            for (const auto& win : m_Windows)
                            {
                                if (win && win->scene)
                                {
                                    sceneController = win->scene.get();
                                    break;
                                }
                            }
                            if (!sceneController)
                                return out;
                            const SceneViewCameraPose pose = sceneController->GetCameraPose();
                            out.PosX = pose.Pos[0];
                            out.PosY = pose.Pos[1];
                            out.PosZ = pose.Pos[2];
                            out.YawDeg = pose.YawDeg;
                            out.PitchDeg = pose.PitchDeg;
                            out.Distance = pose.Distance;
                            out.Is2D = pose.Is2D;
                            return out;
                        });
                    }
                    if (hierarchyPanel && editorCtx)
                    {
                        hierarchyPanel->SetContext(editorCtx);
                        
                        // Wire BookmarksPanel
                        if (bookmarksPanel && editorCtx)
                        {
                            bookmarksPanel->SetEditorContext(editorCtx);
                            bookmarksPanel->SetUndoRedoService(m_UndoRedo.get());
                            bookmarksPanel->SetAssetsPanel(assetsPanel);
                            bookmarksPanel->SetHierarchyPanel(hierarchyPanel);
                            bookmarksPanel->SetAssetsPanelForDrag(assetsPanel);
                            bookmarksPanel->SetHierarchyPanelForDrag(hierarchyPanel);
                            // Entity bookmark navigation: scene path and open-scene callbacks
                            bookmarksPanel->SetGetCurrentScenePath([this]() -> std::optional<std::filesystem::path> {
                                if (m_SceneEditor)
                                    return m_SceneEditor->GetActiveScenePath();
                                return std::nullopt;
                            });
                            bookmarksPanel->SetOnOpenScene([this](const std::filesystem::path& path) {
                                if (m_SceneEditor)
                                    m_SceneEditor->RequestOpenScene(path, /*additive=*/false);
                            });
                        }
                        // Wire Hierarchy to navigate when a bookmark is dropped on it
                        if (hierarchyPanel && bookmarksPanel)
                        {
                            hierarchyPanel->SetOnNavigateToBookmark([bookmarksPanel](const Bookmark& b) {
                                if (bookmarksPanel)
                                    bookmarksPanel->NavigateToBookmark(b);
                            });
                            // Wire "Add to Bookmarks" context menu → BookmarksPanel entity drop handler
                            hierarchyPanel->SetOnAddEntitiesToBookmarks([this, bookmarksPanel](const std::vector<ECS::EntityHandle>& entities) {
                                if (!bookmarksPanel)
                                    return;
                                ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                                if (!world)
                                    return;
                                std::string scenePath;
                                if (m_SceneEditor)
                                {
                                    auto opt = m_SceneEditor->GetActiveScenePath();
                                    if (opt.has_value())
                                        scenePath = opt->string();
                                }
                                for (const ECS::EntityHandle& handle : entities)
                                {
                                    if (!world->IsValid(handle))
                                        continue;
                                    std::string entityName = "Entity";
                                    if (auto* n = world->GetComponent<Components::Name>(handle))
                                        if (!n->View().empty())
                                            entityName = std::string(n->View());
                                    std::string entityId = std::to_string(handle.index) + ":" + std::to_string(handle.version);
                                    Bookmark bm;
                                    bm.Type = BookmarkType::Entity;
                                    bm.Reference = scenePath + "|" + entityId;
                                    bm.Name = entityName;
                                    bookmarksPanel->AddBookmark(bm);
                                }
                            });
                        }
                        if (bookmarksPanel)
                        {
                            // Defer a refresh to re-validate bookmarks after asset registry is populated.
                            // This ensures bookmarks are validated against the complete asset list.
                            BookmarksPanel* bmPanel = bookmarksPanel;
                            bookmarksPanel->PostAction([bmPanel]()
                            {
                                if (bmPanel)
                                    bmPanel->RefreshBookmarks();
                            });
                        }
                        // Hierarchy tree row height (separate from Assets browser tree).
                        {
                            double stored = 20.0;
                            cachedPrefs.TryGetDouble("ui.hierarchyTreeRowHeight", stored);
                            const float rowH = std::clamp(static_cast<float>(stored), kMinEditorTreeRowHeightPx,
                                                          kMaxEditorTreeRowHeightPx);
                            hierarchyPanel->SetTreeRowHeight(rowH);
                        }
                    }
                    // Wire TodoPanel
                    if (todoPanel && editorCtx)
                    {
                        todoPanel->SetEditorContext(editorCtx);
                        todoPanel->SetUndoRedoService(m_UndoRedo.get());
                    }
                    if (undoHistoryPanel)
                    {
                        undoHistoryPanel->SetUndoRedoService(m_UndoRedo.get());
                        m_UndoRedo->SetOnHistoryChanged([this, undoHistoryPanel]() {
                            if (m_EditorToolbar) m_EditorToolbar->UpdateUndoRedoTitles();
                            undoHistoryPanel->Rebuild();
                        });
                    }
                    GraphPanel::ForEachLive([this, editorCtx](GraphPanel& panel) {
                        panel.SetUndoRedoService(m_UndoRedo.get());
                        if (editorCtx)
                            panel.SetContext(editorCtx);
                    });
                    if (inspectorPanel && editorCtx)
                    {
                        inspectorPanel->SetContext(editorCtx);
                    }
                    if (editorCtx)
                        ForEachAnimationPanel(m_PanelStorage, [editorCtx](AnimationWindowPanel* panel)
                        {
                            panel->SetContext(editorCtx);
                        });
                    if (sceneViewPanel && editorCtx)
                    {
                        sceneViewPanel->SetContext(editorCtx);
                        sceneViewPanel->SetChangeNotifications(m_ChangeNotifications.get());
                        sceneViewPanel->SetOpenColorPickerWindow(m_ColorPicker->AsOpenCallback());
                    }
                    if (sceneViewPanel && hierarchyPanel)
                    {
                        Editor::WireSceneViewAssetDropUndo(sceneViewPanel,
                                                           hierarchyPanel,
                                                           m_UndoRedo.get(),
                                                           m_ChangeNotifications.get());
                    }
                    if (editorCtx)
                    {
                        auto configureAssetViewPanel = [&](AssetViewPanel* panel)
                        {
                            if (!panel)
                                return;
                            panel->SetContext(editorCtx);
                            panel->SetDevice(main->renderCtx ? main->renderCtx->GetDevice() : nullptr);
                            panel->SetUIManager(main->ui ? main->ui.get() : nullptr);
                        };

                        for (const auto& panel : m_PanelStorage)
                            configureAssetViewPanel(dynamic_cast<AssetViewPanel*>(panel.get()));
                        configureAssetViewPanel(EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy));
                    }
                    if (settingsPanel && editorCtx)
                    {
                        settingsPanel->SetContext(editorCtx);
                        settingsPanel->SetOpenColorPickerWindow(m_ColorPicker->AsOpenCallback());
                    }
                    // The scene-diff indicator preference is owned by
                    // EditorVersionControlUi and registered with
                    // Editor::EditorSettingsRegistry; the SettingsPanel renders
                    // it from that descriptor and needs no wiring here.
                    if (webPanel && main && main->window)
                    {
                        webPanel->SetWindow(main->window.get());
                    }
                    // Wire SettingsPanel to UIManager for AA strength control
                    if (settingsPanel && main && main->ui)
                    {
                        settingsPanel->SetUIManager(main->ui.get());
                        settingsPanel->SetDevice(main->renderCtx ? main->renderCtx->GetDevice() : nullptr);
                        settingsPanel->SetOnEditorFontPreferencesChanged([this]()
                                                                        {
                                                                            for (auto& w : m_Windows)
                                                                            {
                                                                                if (!w || !w->ui)
                                                                                    continue;
                                                                                Editor::ApplySavedEditorFontPreferences(w->ui.get());
                                                                                // The line height is a multiple of the
                                                                                // script face's own line, so a new face
                                                                                // needs a new multiplier.
                                                                                SettingsPanel::ApplySavedScriptLineHeightStyle(w->ui.get());
                                                                            }
                                                                        });
                        settingsPanel->SetOnToggleStyleChanged([this](bool useCheckmarks)
                                                               {
                                                                   for (auto& w : m_Windows)
                                                                   {
                                                                       if (w && w->ui)
                                                                           SettingsPanel::ApplyToggleStyle(w->ui.get(), useCheckmarks);
                                                                   }
                                                               });
                        settingsPanel->SetOnTextContrastChanged([this](float contrast)
                                                                {
                                                                    for (auto& w : m_Windows)
                                                                    {
                                                                        if (w && w->ui)
                                                                            w->ui->SetTextContrast(contrast);
                                                                    }
                                                                });
                        settingsPanel->SetOnTextSubpixelAAChanged([this](bool enabled)
                                                                  {
                                                                      for (auto& w : m_Windows)
                                                                      {
                                                                          if (w && w->ui)
                                                                              w->ui->SetTextSubpixelAA(enabled);
                                                                      }
                                                                  });
                        settingsPanel->SetOnHdrOutputSettingsChanged([this]()
                                                                      {
                                                                          RequestHdrOutputRefreshForAllWindows();
                                                                      });
                        settingsPanel->SetOnTextSmoothingGammaChanged([this](float gamma)
                                                                      {
                                                                          for (auto& w : m_Windows)
                                                                          {
                                                                              if (w && w->ui)
                                                                                  w->ui->SetTextSmoothingGamma(gamma);
                                                                          }
                                                                      });
                    }
                    Logger::Log::Info("[Startup]     Selection+Inspector+SceneView wiring: {:.1f}ms", MsSince(tDockSub));
                    tDockSub = std::chrono::high_resolution_clock::now();
                    // Defer Settings panel initialization to post-first-frame.
                    // SettingsPanel is never the active tab on startup, so its wiring
                    // (including 9+ preference file loads) can run after first render.
                    {
                        SettingsPanel* sp = settingsPanel;
                        AssetsPanel* ap = assetsPanel;
                        HierarchyPanel* hp = hierarchyPanel;
                        SceneViewController* sc = (main && main->scene) ? main->scene.get() : nullptr;
                        if (auto* uiRoot = main->ui->GetRootElement())
                        {
                            uiRoot->PostAction([this, sp, ap, hp, sc]()
                            {
                                InitializeTreeAndListSettings(sp, ap, hp);
                                InitializeAssetsGridAndSmartFolderSettings(sp, ap);
                                if (sp && ap)
                                    sp->SetPingAsset([ap](const std::filesystem::path& path)
                                                     { ap->NavigateToAndSelectAsset(path); });
                                InitializeInspectorSettings(sp);
                                if (sp && sc)
                                    Editor::SceneTools::GizmoSettingsWiring::WireGizmoSettings(sp, sc);
                            });
                        }
                    }
                    // The toolbar is declared in layout.uxml and instantiated by the
                    // UI factory; it loads its own UXML/CSS. User-mirrored layouts on
                    // Windows/Linux only receive missing files, so a stale layout.uxml
                    // may predate the tag — insert the control ourselves in that case.
                    if (!m_TopToolbar)
                    {
                        m_TopToolbar =
                            dynamic_cast<EditorTopToolbar*>(rootEl->FindById("EditorTopToolbar"));
                        if (!m_TopToolbar)
                        {
                            auto toolbar = std::make_unique<EditorTopToolbar>();
                            m_TopToolbar = toolbar.get();
                            rootEl->InsertChild(0, std::move(toolbar));
                        }
                        m_TopToolbar->SetPanelManager(m_PanelManager.get());
                        (void)m_TopToolbar->LoadAssets();
                    }
                    {
                        EditorTopToolbar::Callbacks toolbarCb;
                        toolbarCb.getPanelMenuEntries = [this]() {
                            std::vector<EditorTopToolbar::PanelMenuEntry> entries;
                            if (!m_PanelManager)
                                return entries;
                            const auto panels = m_PanelManager->GetPanelMenuEntries();
                            entries.reserve(panels.size());
                            for (const auto& panel : panels)
                            {
                                entries.push_back({
                                    panel.panelId,
                                    panel.title,
                                    panel.iconClass,
                                });
                            }
                            return entries;
                        };
                        toolbarCb.onPanelMenuEntryClicked = [this](const std::string& panelId) {
                            OpenPanel(panelId);
                        };
                        toolbarCb.onUniversalSearchClicked = [this]() {
                            if (m_UniversalSearch) m_UniversalSearch->Toggle();
                        };
                        toolbarCb.onSettingsClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateSettingsPanel(main);
                        };
                        toolbarCb.onNodeGraphClicked = [this, main]() {
                            if (main && m_PanelManager)
                                m_PanelManager->ShowOrActivateGraphPanel(main);
                        };
                        toolbarCb.onBookmarksClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateBookmarksPanel(main);
                        };
                        toolbarCb.onWebClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateWebPanel(main);
                        };
                        toolbarCb.onScriptEditorClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateScriptEditorPanel(main);
                        };
                        toolbarCb.onTodosClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateTodoPanel(main);
                        };
                        toolbarCb.onUndoHistoryClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateUndoHistoryPanel(main);
                        };
                        toolbarCb.onPackagesClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::PackageManager);
                        };
                        toolbarCb.onBuildClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateBuildPanel(main);
                        };
                        toolbarCb.onRecordClicked = [this, main]() {
                            if (!main || !m_PanelManager || !m_MovieRecorder)
                                return;
                            // Surface the Recording page so the session is
                            // visible from its first frame.
                            m_PanelManager->ShowOrActivateSettingsPanelToRegistryCategory(main, "recording");
                            m_MovieRecorder->ToggleRecording();
                        };
                        toolbarCb.onMixerClicked = [this, main]() {
                            if (main && m_PanelManager) m_PanelManager->ShowOrActivateMixerPanel(main);
                        };
                        toolbarCb.onMonitorsClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::Monitors);
                        };
                        toolbarCb.onVisualProfilerClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::VisualProfiler);
                        };
                        toolbarCb.onRenderGraphClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::RenderGraph);
                        };
                        toolbarCb.onAnimationClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::Animation);
                        };
                        toolbarCb.onTimelineClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::Timeline);
                        };
                        toolbarCb.onLogClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::Log);
                        };
                        toolbarCb.onVramClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::Vram);
                        };
                        toolbarCb.onCpuProfilerClicked = [this, main]() {
                            if (main && m_PanelManager) OpenPanel(EditorPanelIds::CpuProfiler);
                        };
                        if (auto* buildPanel = m_PanelManager->FindFirstPanelOfType<BuildPanel>())
                            buildPanel->SetOnOpenBuildSettings([this, main]() {
                                if (main && m_PanelManager) m_PanelManager->ShowOrActivateSettingsPanelToRegistryCategory(main, "build");
                            });
                        m_MovieRecorder = std::make_unique<Editor::MovieRecorderController>();
                        m_MovieRecorder->SetPlayMode(m_PlayMode.get());
                        m_MovieRecorder->SetOnPlayModeRecordingStarted([this]() {
                            m_DeferredPostUiActions.EnqueueUnique("editor.recording-game-input", [this]() {
                                OpenPanel(EditorPanelIds::GameView);
                                for (const auto& win : m_Windows)
                                    if (win && win->ui && win->docking &&
                                        win->docking->IsPanelActiveTab(EditorPanelIds::GameView))
                                        win->ui->SetFocusById(EditorPanelIds::GameViewViewport);
                            });
                        });
                        m_MovieRecorder->SetGetGameView([this]() -> GameViewController* {
                            for (const auto& win : m_Windows)
                            {
                                if (win && win->role == WindowRole::Main && win->gameView)
                                    return win->gameView.get();
                            }
                            for (const auto& win : m_Windows)
                            {
                                if (win && win->gameView)
                                    return win->gameView.get();
                            }
                            return nullptr;
                        });
                        m_MovieRecorder->SetForEachGameView([this](const std::function<void(GameViewController&)>& fn) {
                            for (const auto& win : m_Windows)
                            {
                                if (win && win->gameView)
                                    fn(*win->gameView);
                            }
                        });
                        m_MovieRecorder->SetGetCurrentScenePath([this]() -> std::optional<std::filesystem::path> {
                            if (m_SceneEditor)
                                return m_SceneEditor->GetActiveScenePath();
                            return std::nullopt;
                        });
                        m_MovieRecorder->SetFixedFrameRateHooks(
                            [this](double fps) { SetFixedFrameRate(fps); },
                            [this]() { ClearFixedFrameRate(); });
                        m_MovieRecorder->SetOnRecordingFinished([this, main, assetsPanel](const std::filesystem::path&) {
                            if (assetsPanel)
                                assetsPanel->RefreshViews();
                            // Surface the Recording page; its view pulls the
                            // finished session from the controller.
                            if (main && m_PanelManager)
                                m_PanelManager->ShowOrActivateSettingsPanelToRegistryCategory(main, "recording");
                        });
                        m_MovieRecorder->SetOnOutputPathMissing([this, main]() {
                            if (main && m_PanelManager)
                                m_PanelManager->ShowOrActivateSettingsPanelToRegistryCategory(main, "recording");
                        });
                        m_MovieRecorder->RegisterSettingsCategory(
                            m_UndoRedo.get(),
                            [this](const std::filesystem::path& path,
                                   const std::string& title)
                            {
                                m_VideoPlayer.Open(path, title);
                            });
                        // The Build pages own their descriptors; the shell only
                        // supplies what they cannot reach from the registry.
                        {
                            Editor::BuildSettingsPageHooks buildHooks;
                            buildHooks.OpenBuildPanel = [this, main]() {
                                if (main && m_PanelManager) m_PanelManager->ShowOrActivateBuildPanel(main);
                            };
                            buildHooks.IsBuildPanelActive = [main]() -> bool {
                                return main && main->docking && main->docking->IsPanelActiveTab(EditorPanelIds::Build);
                            };
                            buildHooks.Defer = [this](std::function<void()> action) {
                                m_DeferredPreUiActions.Enqueue(std::move(action));
                            };
                            Editor::RegisterBuildSettingsCategories(std::move(buildHooks));
                            // Build exporters register before any build can be triggered (panel or debug server).
                            Editor::RegisterWebBuildExporter();
                            Editor::RegisterSteamDeckBuildExporter();
                        }
                        // Load play fullscreen preference from cached prefs.
                        cachedPrefs.TryGetBool("ui.playFullscreen", m_PlayFullscreenOnEnter);

                        toolbarCb.onSaveSceneClicked = [this](int mods) {
                            const bool saveAs = ((mods & Input::kModShift) != 0);
                            m_DeferredPreUiActions.EnqueueUnique(
                                "editor.toolbar-save-scene",
                                [this, saveAs]()
                                {
                                    if (!m_SceneEditor)
                                        return;
                                    if (saveAs)
                                        m_SceneEditor->RequestSaveSceneAs();
                                    else
                                        m_SceneEditor->RequestSaveScene();
                                });
                        };
                        toolbarCb.onRevertSceneClicked = [this]() {
                            m_DeferredPreUiActions.EnqueueUnique("editor.toolbar-revert-scene", [this]() {
                                if (m_SceneEditor)
                                    m_SceneEditor->RequestRevertScene();
                            });
                        };
                        toolbarCb.onProjectFolderClicked = [this]() {
                            if (m_ProjectFolderModal)
                            {
                                auto& engine = EngineCore::GetInstance();
                                std::filesystem::path currentWorkspace = engine.GetWorkspaceRoot();
                                if (!currentWorkspace.empty())
                                    m_ProjectFolderModal->SetPath(currentWorkspace);
                                m_ProjectFolderModal->Show();
                                // The toolbar opens the picker over a usable editor
                                // session, so the user must always be able to dismiss it.
                                m_ProjectFolderModal->SetShowButtons(true);
                            }
                        };
                        toolbarCb.onFullscreenToggleClicked = [this]() {
                            if (!m_PlayMode)
                                return;

                            const auto state = m_PlayMode->GetState();
                            if (state != GameEngine::Editor::PlayModeState::Edit)
                            {
                                // While play mode is running (or paused/change-review), toggle
                                // the fullscreen game-view layout via a deferred action so that
                                // window/docking changes happen outside the immediate UI callback.
                                m_DeferredPreUiActions.EnqueueUnique(
                                    "editor.toggle-play-fullscreen",
                                    [this]()
                                    {
                                        if (!m_PlayMode)
                                            return;
                                        const auto curState = m_PlayMode->GetState();
                                        if (curState == GameEngine::Editor::PlayModeState::Edit || m_Windows.empty())
                                            return;

                                        if (!m_PlayFullscreenActive)
                                        {
                                            EnterPlayFullscreen();
                                        }
                                        else
                                        {
                                            ExitPlayFullscreen();
                                        }

                                        if (m_TopToolbar)
                                            m_TopToolbar->SetFullscreenActive(m_PlayFullscreenActive);
                                    });
                                return;
                            }

                            // In Edit mode, toggle the preference that controls whether the
                            // next time we enter play mode we should go fullscreen.
                            auto prefs = Editor::OpenEditorPreferences();
                            std::string err;
                            prefs.Load(&err);
                            m_PlayFullscreenOnEnter = !m_PlayFullscreenOnEnter;
                            prefs.SetBool("ui.playFullscreen", m_PlayFullscreenOnEnter);
                            prefs.Save(&err);
                            if (m_TopToolbar)
                                m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
                        };

                        toolbarCb.onPlayClicked = [this]() {
                            if (!m_PlayMode) return;
                            if (m_PlayMode->GetState() != GameEngine::Editor::PlayModeState::Edit)
                                return;

                            auto doPlay = [this]() {
                                if (CanEnterPlayFullscreen())
                                    RequestEnterPlayFullscreenAndPlay();
                                else
                                    m_PlayMode->EnterPlayMode();
                            };

                            if (m_DownloadManager && m_DownloadManager->HasActiveDownloads() && m_DownloadModal)
                            {
                                m_DownloadModal->Show("Play Mode", m_DownloadManager.get(), std::move(doPlay));
                                return;
                            }
                            doPlay();
                        };
                        toolbarCb.onPauseClicked = [this]() {
                            if (!m_PlayMode) return;
                            if (m_PlayMode->IsPlayingOrPaused())
                                m_PlayMode->TogglePause();
                        };
                        toolbarCb.onStopClicked = [this]() {
                            if (!m_PlayMode) return;
                            if (m_PlayMode->GetState() == GameEngine::Editor::PlayModeState::ChangeReview)
                            {
                                m_PlayMode->DiscardPendingChanges();
                                return;
                            }
                            if (m_PlayMode->GetState() != GameEngine::Editor::PlayModeState::Edit)
                            {
                                m_PlayMode->ExitPlayMode();
                                ExitPlayFullscreen();
                                if (m_TopToolbar)
                                    m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
                            }
                        };
                        Logger::Log::Info("[Startup]   Panel wiring (pre-toolbar): {:.1f}ms", MsSince(tDockSub));
                        tDockSub = std::chrono::high_resolution_clock::now();
                        m_TopToolbar->WireControls(rootEl, main->window.get(), toolbarCb);
                        // Initialize fullscreen toggle visual state.
                        m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);

                        if (auto* recordBtn = m_TopToolbar->GetRecordButton())
                        {
                            // Provider rather than a fixed set only for the guard: the
                            // recorder controller may not exist yet when the button wires.
                            recordBtn->AddManipulator(ContextMenuManipulator::Create(
                                [this, main]() -> std::vector<ContextMenuManipulator::Item> {
                                    if (!main || !m_MovieRecorder)
                                        return {};
                                    return Editor::MovieRecorderMenuItems(
                                        *m_MovieRecorder,
                                        [this, main]() {
                                            if (main && m_PanelManager)
                                                m_PanelManager->ShowOrActivateSettingsPanelToRegistryCategory(main, "recording");
                                        });
                                }));
                            recordBtn->SetTooltip("Record. Right-click for options.");
                        }

                        // Wire TopToolbarRight2 (info icon) to open the intro video.
                        if (auto* infoBtn = dynamic_cast<Button*>(rootEl->FindById("TopToolbarRight2")))
                        {
                            infoBtn->SetTooltip("Info");
                            infoBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                            {
                                m_VideoPlayer.Open(m_AssetsDirectory / "Sample" / "logo.mp4",
                                                   "Open Engine " + Editor::EditorVersionText());
                            });
                        }

                        m_TopToolbar->ApplyButtonVisibilityFromPrefs(rootEl);
                        m_TopToolbar->WireRightClickToggles(rootEl, main->window.get());
                    }

                    // Sync play/pause/stop button visuals to current Play Mode state.
                    if (m_PlayMode)
                    {
                        UpdatePlayModeToolbar(m_PlayMode->GetState());
                    }

                    // Set up drag-and-drop for toolbar icon buttons
                    if (!m_ToolbarDragDrop)
                        m_ToolbarDragDrop = std::make_unique<Editor::ToolbarDragDrop>();
                    m_ToolbarDragDrop->SetAccentColor(GetEditorAccentColor());
                    m_TopToolbar->SetupDragDrop(rootEl, m_ToolbarDragDrop.get());

                    // Set up drag-and-drop for scene view toolbar buttons.
                    // The SceneViewPanel loads its UXML asynchronously, so SceneViewToolbar is not yet in
                    // the DOM when the dock is built. Use SetOnToolbarReady to wire drag-drop once it exists.
                    if (sceneViewPanel)
                    {
                        if (!m_SceneViewToolbarDragDrop)
                            m_SceneViewToolbarDragDrop = std::make_unique<Editor::ToolbarDragDrop>();
                        m_SceneViewToolbarDragDrop->SetAccentColor(GetEditorAccentColor());
                        auto* dragDrop = m_SceneViewToolbarDragDrop.get();
                        auto* panelMgr = m_PanelManager.get();
                        auto* sceneViewPanelPtr = sceneViewPanel;
                        // The toolbar owns its right-click menus (option B on PR #1107);
                        // only the actions that leave the toolbar are wired here.
                        sceneViewPanel->SetOnToolbarReady([main, dragDrop, panelMgr, sceneViewPanelPtr](SceneViewToolbar* toolbar) {
                            if (!main || !main->ui)
                                return;
                            if (UIElement* root = main->ui->GetRootElement())
                                toolbar->SetupDragDrop(root, dragDrop);
                            // "Open Grid Settings..." / "Open Snap Settings..." rows.
                            toolbar->SetOnOpenGridSettings([panelMgr, main]() {
                                if (panelMgr && main)
                                    panelMgr->ShowOrActivateSettingsPanelToCategory(main, SettingsCategory::GridAndSnapping);
                            });
                            // The auto-exposure toggle seeds Fixed EV100 (AE-lock); an open
                            // camera popup must pick up the seeded value or its stale field
                            // would clobber the seed.
                            toolbar->SetOnPostFxAutoExposureToggled([sceneViewPanelPtr]() {
                                if (sceneViewPanelPtr)
                                    sceneViewPanelPtr->SyncCameraSettingsPopupFromSettings();
                            });
                            // Left-click on the camera button opens the scene camera quick-settings popup.
                            toolbar->SetOnOpenCameraSettings([sceneViewPanelPtr](float x, float y) {
                                if (sceneViewPanelPtr)
                                    sceneViewPanelPtr->ShowCameraSettingsPopup(x, y);
                            });
                        });
                    }

                    // Wire layout preset toolbar (bottom toolbar).
                    SyncLayoutPresetToolbar();

                    // Track docking model bound to this window
                    Logger::Log::Info("[Startup]   Toolbar + SceneView drag-drop + presets: {:.1f}ms", MsSince(tDockSub));
                    main->docking = m_Docking.get();
                }
                else
                {
                    Logger::Log::Warning("Editor: Element with id 'dock' is not a DockspaceElement");
                }
            }
            else
            {
                Logger::Log::Warning("Editor: Dockspace element with id 'dock' not found in layout");
            }
        }
        // When dock was not bound (first run or missing layout), still give panels context so they don't crash when used.
        if (!dockBound && editorCtx)
        {
            if (assetsPanel)
                assetsPanel->SetContext(editorCtx);
            if (hierarchyPanel)
                hierarchyPanel->SetContext(editorCtx);
            if (inspectorPanel)
                inspectorPanel->SetContext(editorCtx);
            ForEachAnimationPanel(m_PanelStorage, [editorCtx](AnimationWindowPanel* panel)
            {
                panel->SetContext(editorCtx);
            });
            if (sceneViewPanel)
            {
                sceneViewPanel->SetContext(editorCtx);
                sceneViewPanel->SetChangeNotifications(m_ChangeNotifications.get());
            }
            if (sceneViewPanel && hierarchyPanel)
            {
                Editor::WireSceneViewAssetDropUndo(sceneViewPanel,
                                                   hierarchyPanel,
                                                   m_UndoRedo.get(),
                                                   m_ChangeNotifications.get());
            }
            {
                auto configureAssetViewPanel = [&](AssetViewPanel* panel)
                {
                    if (!panel)
                        return;
                    panel->SetContext(editorCtx);
                    panel->SetDevice(main->renderCtx ? main->renderCtx->GetDevice() : nullptr);
                    panel->SetUIManager(main->ui ? main->ui.get() : nullptr);
                };

                for (const auto& panel : m_PanelStorage)
                    configureAssetViewPanel(dynamic_cast<AssetViewPanel*>(panel.get()));
                configureAssetViewPanel(EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy));
            }
            if (settingsPanel)
                settingsPanel->SetContext(editorCtx);
            GraphPanel::ForEachLive([this, editorCtx](GraphPanel& panel) {
                panel.SetUndoRedoService(m_UndoRedo.get());
                panel.SetContext(editorCtx);
            });
            if (todoPanel)
            {
                todoPanel->SetEditorContext(editorCtx);
                todoPanel->SetUndoRedoService(m_UndoRedo.get());
            }
            if (undoHistoryPanel)
            {
                undoHistoryPanel->SetUndoRedoService(m_UndoRedo.get());
                m_UndoRedo->SetOnHistoryChanged([this, undoHistoryPanel]() {
                    if (m_EditorToolbar) m_EditorToolbar->UpdateUndoRedoTitles();
                    undoHistoryPanel->Rebuild();
                });
            }
            if (bookmarksPanel)
            {
                bookmarksPanel->SetEditorContext(editorCtx);
                bookmarksPanel->SetUndoRedoService(m_UndoRedo.get());
                bookmarksPanel->SetAssetsPanel(assetsPanel);
                bookmarksPanel->SetHierarchyPanel(hierarchyPanel);
                bookmarksPanel->SetAssetsPanelForDrag(assetsPanel);
                bookmarksPanel->SetHierarchyPanelForDrag(hierarchyPanel);
                bookmarksPanel->SetGetCurrentScenePath([this]() -> std::optional<std::filesystem::path> {
                    if (m_SceneEditor)
                        return m_SceneEditor->GetActiveScenePath();
                    return std::nullopt;
                });
                bookmarksPanel->SetOnOpenScene([this](const std::filesystem::path& path) {
                    if (m_SceneEditor)
                        m_SceneEditor->RequestOpenScene(path, /*additive=*/false);
                });
            }
            if (hierarchyPanel && bookmarksPanel)
            {
                hierarchyPanel->SetOnAddEntitiesToBookmarks([this, bookmarksPanel](const std::vector<ECS::EntityHandle>& entities) {
                    if (!bookmarksPanel)
                        return;
                    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                    if (!world)
                        return;
                    std::string scenePath;
                    if (m_SceneEditor)
                    {
                        auto opt = m_SceneEditor->GetActiveScenePath();
                        if (opt.has_value())
                            scenePath = opt->string();
                    }
                    for (const ECS::EntityHandle& handle : entities)
                    {
                        if (!world->IsValid(handle))
                            continue;
                        std::string entityName = "Entity";
                        if (auto* n = world->GetComponent<Components::Name>(handle))
                            if (!n->View().empty())
                                entityName = std::string(n->View());
                        std::string entityId = std::to_string(handle.index) + ":" + std::to_string(handle.version);
                        Bookmark bm;
                        bm.Type = BookmarkType::Entity;
                        bm.Reference = scenePath + "|" + entityId;
                        bm.Name = entityName;
                        bookmarksPanel->AddBookmark(bm);
                    }
                });
            }
        }
        {
            Editor::UniversalSearchController::Dependencies searchDeps;
            searchDeps.MainWindow = mainWindowCtx;
            searchDeps.PanelManager = m_PanelManager.get();
            searchDeps.Docking = m_Docking.get();
            searchDeps.Assets = assetsPanel;
            searchDeps.Hierarchy = hierarchyPanel;
            searchDeps.Settings = settingsPanel;
            searchDeps.ScriptEditor = scriptEditorPanel;
            searchDeps.SceneEditor = m_SceneEditor.get();
            searchDeps.PlayMode = m_PlayMode.get();
            searchDeps.UndoRedo = m_UndoRedo.get();
            searchDeps.ProjectFolderModal = m_ProjectFolderModal;
            searchDeps.GetWorld = [this]() { return m_EditorContext ? m_EditorContext->World : nullptr; };
            searchDeps.GetProjectRoot = []() { return EngineCore::GetInstance().GetWorkspaceRoot(); };
            searchDeps.ResetLayout = [this]() { ResetLayoutToDefault(); };
            m_UniversalSearch = std::make_unique<Editor::UniversalSearchController>();
            m_UniversalSearch->Initialize(searchDeps);
        }
        if (m_EditorToolbar)
            m_EditorToolbar->Refresh();
        // Wire undock callback (drag-out from DockTab) to create floating window
        m_Docking->SetUndockCallback([this](const std::string& id)
                                     {
                                         // NOTE: This callback is invoked from UI event dispatch, which itself is
                                         // driven by Platform::Window::PollEvents(). Creating a new OS window and
                                         // render device inside that callback can deadlock/freeze on Windows/GLFW.
                                         // Defer the heavy undock work to the next EditorApplication::Update().
                                         this->QueueUndockPanel(id); });
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("Editor: UI staging failed: {}", e.what());
    }

    Logger::Log::Info("[Startup] Docking + panel wiring: {:.1f}ms", MsSince(tPhase));
    tPhase = std::chrono::high_resolution_clock::now();

    Logger::Log::Info("Editor: Project Assets root: '{}'", EngineCore::GetInstance().GetAssetManager().GetAssetRoot().string());
    Logger::Log::Info("Editor: Editor Assets root: '{}'", m_AssetsDirectory.string());

    // Start MCP debug server eagerly (needed for AI agent IPC).
    {
        // Precedence: --debug-port (explicit, per-launch) > GE_EDITOR_DEBUG_PORT
        // (ambient, often inherited by a whole shell) > the default. The flag
        // must win, or a lane that asks for its own port lands on the default —
        // which is where another developer's editor is already listening.
        uint16_t debugPort = kDefaultEditorDebugPort;
        const char* portSource = "default";
        if (const char* portEnv = std::getenv("GE_EDITOR_DEBUG_PORT"))
        {
            char* end = nullptr;
            const long parsed = std::strtol(portEnv, &end, 10);
            if (end != portEnv && parsed > 0 && parsed <= 65535)
            {
                debugPort = static_cast<uint16_t>(parsed);
                portSource = "GE_EDITOR_DEBUG_PORT";
            }
        }
        if (m_DebugPortOverride.has_value())
        {
            debugPort = *m_DebugPortOverride;
            portSource = "--debug-port";
        }
        m_DebugServer = std::make_unique<EditorDebugServer>(Editor::DebugRequestGateRegistry::Get());
        m_DebugServer->SetUndoRedoService(m_UndoRedo.get());
        RegisterDebugHandlers(*m_DebugServer, *this, m_DebugLogSink);
        if (m_DebugServer->Start(debugPort))
        {
            Editor::SetEditorDebugPort(debugPort);
            Logger::Log::Info("Editor: MCP debug server started on port {} (from {})", debugPort,
                              portSource);
        }
        else
            Logger::Log::Warning("Editor: Failed to start MCP debug server on port {} (from {})",
                                 debugPort, portSource);
    }

    // Q6 slice 2: first OnDeviceRebuilt consumer. An in-place device rebuild frees
    // the per-window RenderGraph frame rings, so any in-flight readback ticket
    // (IPC screenshot, movie recording, thumbnail, debug capture) would reference a
    // dead ring and never resolve — hanging the IPC poller that awaits it. Cancel
    // them on rebuild, mirroring the window-close cleanup. The callback fires on the
    // render thread inside RebuildDevice — the same thread that owns m_Windows — so
    // the iteration is race-free.
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        if (Rendering::IDevice* device = rs->GetDevice())
        {
            device->RegisterDeviceRebuiltCallback(
                "EditorCancelPendingReadbacks",
                [this](Rendering::IDevice*)
                {
                    for (auto& win : m_Windows)
                    {
                        if (win && win->RenderGraphStream.Frame)
                            Rendering::CancelPendingReadbacksRG(win->RenderGraphStream.Frame.get());
                    }
                });
        }
    }

    // Defer ScriptMenuRegistry + capture tools to first Update() (not needed for first frame).
    m_PendingPostInit = true;

    Logger::Log::Info("[Startup] Post-init (ScriptMenu+Capture+MCP): {:.1f}ms", MsSince(tPhase));
    Logger::Log::Info("[Startup] EditorApplication::Initialize TOTAL: {:.1f}ms", MsSince(tEditorStart));

    QueueDeferredStartupScene();

    return true;
}

void EditorApplication::AttachPackagePanelConsumers()
{
    Editor::EditorPanelRegistry::Consumers consumers;
    consumers.Panel = [this](const Editor::EditorPanelDescriptor& descriptor) {
        ConsumePackagePanelDescriptor(descriptor);
    };
    consumers.StyleSheet = [this](const Editor::EditorStyleSheetContribution& contribution) {
        ConsumePackageEditorStyleSheet(contribution);
    };
    consumers.Overlay = [this](const Editor::EditorOverlayDescriptor& descriptor) {
        ConsumePackageOverlay(descriptor);
    };
    Editor::EditorPanelRegistry::Get().SetPanelOpener(
        [this](const std::string& panelId) { OpenPanel(panelId); });
    Editor::EditorPanelRegistry::Get().SetConsumers(std::move(consumers));

    // Installed scene/asset services package modules reach through
    // EditorSceneCommands (open-scene flow; imported-asset registration so
    // path-form references resolve without racing the FileWatcher debounce).
    Editor::EditorSceneCommands::Get().SetOpenSceneHandler(
        [this](const std::filesystem::path& scenePath) {
            if (m_SceneEditor)
                m_SceneEditor->RequestOpenScene(scenePath, /*additive=*/false);
            else
                Logger::Log::Warning("Editor: OpenScene('{}') before the scene editor exists; ignored",
                                     scenePath.string());
        });
    // Runs on the calling module's worker thread; the registry is internally
    // synchronized and registration is idempotent against concurrent watcher
    // events.
    Editor::EditorSceneCommands::Get().SetRegisterAssetsHandler(
        [](const std::vector<std::filesystem::path>& files) -> size_t {
            auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
            size_t failed = 0;
            for (const auto& file : files)
            {
                if (!registry.RegisterAssetByPath(file).IsOk())
                    ++failed;
            }
            return failed;
        });
}

void EditorApplication::ConsumePackageOverlay(const Editor::EditorOverlayDescriptor& descriptor)
{
    if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->ui)
    {
        Logger::Log::Error("Editor: package overlay '{}' registered before the main UI exists; skipped",
                           descriptor.OverlayId);
        return;
    }
    UIElement* rootEl = m_Windows[0]->ui->GetRootElement();
    if (!rootEl)
    {
        Logger::Log::Error("Editor: package overlay '{}' found no UI root; skipped",
                           descriptor.OverlayId);
        return;
    }
    // Attach-once per id: a module reload re-registers, but the attached
    // element carries live state (an in-flight import) the reload must not
    // tear down. The registry's replace-forward still updates the descriptor
    // for future consumers.
    if (!m_AttachedOverlayIds.insert(descriptor.OverlayId).second)
    {
        Logger::Log::Info("Editor: package overlay '{}' re-registered; keeping the existing attach",
                          descriptor.OverlayId);
        return;
    }
    std::unique_ptr<UIElement> overlay = descriptor.Factory ? descriptor.Factory() : nullptr;
    if (!overlay)
    {
        Logger::Log::Error("Editor: package overlay '{}' factory returned null; overlay skipped",
                           descriptor.OverlayId);
        m_AttachedOverlayIds.erase(descriptor.OverlayId);
        return;
    }
    rootEl->AddChild(std::move(overlay));
    Logger::Log::Info("Editor: package overlay '{}' attached to the UI root", descriptor.OverlayId);
}

void EditorApplication::ConsumePackagePanelDescriptor(const Editor::EditorPanelDescriptor& descriptor)
{
    if (!m_Docking)
        return;

    std::unique_ptr<UIElement> panel = descriptor.Factory ? descriptor.Factory() : nullptr;
    if (!panel)
    {
        Logger::Log::Error("Editor: package panel '{}' factory returned null; panel skipped",
                           descriptor.PanelId);
        return;
    }
    if (auto* dockPanel = dynamic_cast<DockPanel*>(panel.get()))
    {
        if (!descriptor.Title.empty())
            dockPanel->SetTitle(descriptor.Title);
        // Unlike a uxml attribute, a descriptor field cannot distinguish "left
        // alone" from "set to empty" — a package that never touches TabIconClass
        // gets the same empty string as one that clears it. Empty therefore means
        // "keep the panel type's declaration", so package panels that rely on
        // their own DeclaredTabIconClass() keep it.
        if (!descriptor.TabIconClass.empty())
            dockPanel->SetTabIcon(descriptor.TabIconClass);
    }

    // Replace-forward on module reload: swap the docking registration to the
    // fresh instance, then drop the old one (its DLL stays mapped, so the
    // destructor is safe to run; the dockspace rebuild below unmounts it).
    UIElement* previous = m_Docking->GetPanel(descriptor.PanelId);
    UIElement* raw = panel.get();
    m_PanelStorage.emplace_back(std::move(panel));
    m_Docking->RegisterPanel(descriptor.PanelId, raw);
    Logger::Log::Info("Editor: package panel '{}' registered{}", descriptor.PanelId,
                      previous ? " (replacing forward)" : "");

    // A saved layout (or the pre-reload session) may already show this id as
    // an empty tab; rebuild so the mount binds the live element.
    const bool inCurrentLayout =
        m_Docking->GetRoot() &&
        EditorPanelManager::FindLeafContainingPanel(m_Docking->GetRoot(), descriptor.PanelId) != nullptr;
    if ((previous || inCurrentLayout) && !m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
    {
        if (UIElement* rootEl = m_Windows[0]->ui->GetRootElement())
            if (auto* dockspace = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock")))
                dockspace->RequestRebuildFromModel();
    }

    if (previous)
    {
        if (m_PanelManager)
            m_PanelManager->UnregisterUpdateCallback(previous);
        std::erase_if(m_PanelStorage,
                      [previous](const std::unique_ptr<UIElement>& p) { return p.get() == previous; });
    }

    if (m_EditorToolbar)
        m_EditorToolbar->Refresh();
}

void EditorApplication::ConsumePackageEditorStyleSheet(
    const Editor::EditorStyleSheetContribution& contribution)
{
    if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->ui)
        return;

    auto& am = EngineCore::GetInstance().GetAssetManager();
    const GUID guid = am.ResolveAssetGuid(std::filesystem::path(contribution.StyleAssetPath),
                                          contribution.AssetSourceAlias);
    if (guid.IsNull())
    {
        Logger::Log::Error("Editor: package editor stylesheet '{}' did not resolve from source '{}'",
                           contribution.StyleAssetPath, contribution.AssetSourceAlias);
        return;
    }
    // Waits on purpose: a package's editor stylesheet, a small .css with no cook step.
    auto asset = am.LoadAssetAsync(guid, AssetLoadPriority::High).get();
    if (!asset || asset->GetType() != AssetType::UIStyle)
    {
        Logger::Log::Error("Editor: package editor stylesheet '{}:{}' failed to load as UIStyle",
                           contribution.AssetSourceAlias, contribution.StyleAssetPath);
        return;
    }
    // Idempotent global attach: package chrome rules append after the editor
    // theme (the cascade is file order), so packages extend rather than
    // preempt it.
    m_Windows[0]->ui->AttachStyleFromAsset(*static_cast<UIStyleAsset*>(asset.get()));
    if (m_EditorToolbar)
        m_EditorToolbar->Refresh();
}


void EditorApplication::SetProjectFolder(const std::filesystem::path& projectPath)
{
    if (projectPath.empty())
    {
        Logger::Log::Warning("Editor: Cannot set empty project folder");
        return;
    }

    // A switch runs across several frames now, and the second half of one and
    // the first half of another would fight over the same source.
    if (m_ProjectSwitchPending)
    {
        Logger::Log::Warning("Editor: a project switch is still finishing; ignoring the request to open '{}'",
                             projectPath.string());
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(projectPath, ec) || !std::filesystem::is_directory(projectPath, ec))
    {
        Logger::Log::Error("Editor: Project folder does not exist or is not a directory: {}", projectPath.string());
        // Hide loading indicator on error
        if (m_ProjectFolderModal)
        {
            m_ProjectFolderModal->HideLoading();
        }
        return;
    }

    Logger::Log::Info("Editor: Setting project folder to: {}", projectPath.string());

    // Get current AssetManager configuration before shutdown
    auto& engine = EngineCore::GetInstance();
    auto& am = engine.GetAssetManager();
    std::filesystem::path currentAssetRoot = am.GetAssetRoot();
    std::filesystem::path currentDbFile = engine.GetAuthoritativeAssetDbFile();
    std::filesystem::path currentCacheRoot = engine.GetAssetDbCacheRoot();

    // Calculate new asset root (projectPath / "Assets")
    std::filesystem::path newAssetRoot = projectPath / "Assets";

    // Ensure Assets directory exists
    std::filesystem::create_directories(newAssetRoot, ec);
    if (ec)
    {
        Logger::Log::Warning("Editor: Failed to create Assets directory: {}", ec.message());
    }

    // Calculate new DB file and cache root
    std::filesystem::path newDbFile = projectPath / "AssetDatabase.assetdb";
    std::filesystem::path newCacheRoot = projectPath / ".Cache" / "AssetDatabase";

    // Apply the new project's LOD import policy before the source rebinds — model
    // loads kick off on background workers as soon as the new source registers.
    ApplyProjectLodImportPolicy(projectPath);

    ProjectSwitch pending;
    pending.ProjectPath = projectPath;
    pending.NewAssetRoot = newAssetRoot;
    pending.OldAssetRoot = currentAssetRoot;
    pending.OldDbFile = currentDbFile;
    pending.OldCacheRoot = currentCacheRoot;

    // Rebind just the project source. Editor source + loaded editor assets are preserved.
    AssetManager::SourceRebindDesc rebindDesc;
    rebindDesc.NewRoot = newAssetRoot;
    rebindDesc.AuthoritativeDbFile = newDbFile;
    rebindDesc.CacheRoot = newCacheRoot;

    m_ProjectSwitchPending = true;

    // The outgoing project's package mounts belong to that project, so they are
    // unmounted before the project source rebinds. Both steps eject assets, and
    // an eject finishes only once the loads it could not withdraw have resolved
    // — which can be several frames later — so each step continues from the
    // previous one's completion rather than from the statement after it.
    m_Packages->UnmountAll(
        [this, pending, rebindDesc]()
        {
            AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
            const bool started = assets.BeginRebindSource(
                "project", rebindDesc,
                [this, pending](bool rebound)
                {
                    if (rebound)
                        FinishProjectFolderSwitch(pending);
                    else
                        RollBackProjectFolderSwitch(pending);
                });
            if (!started)
            {
                Logger::Log::Error("Editor: Failed to rebind project source to new folder");
                RollBackProjectFolderSwitch(pending);
            }
        });
}

void EditorApplication::RollBackProjectFolderSwitch(const ProjectSwitch& pending)
{
    AssetManager& am = EngineCore::GetInstance().GetAssetManager();
    AssetManager::SourceRebindDesc rollback;
    rollback.NewRoot = pending.OldAssetRoot;
    rollback.AuthoritativeDbFile = pending.OldDbFile;
    rollback.CacheRoot = pending.OldCacheRoot;
    const bool started = am.BeginRebindSource("project", rollback,
                                              [this](bool)
                                              {
                                                  // The workspace root was never moved, so the old
                                                  // project's packages remount against it.
                                                  m_Packages->MountForProject(
                                                      EngineCore::GetInstance().GetWorkspaceRoot());
                                                  if (m_ProjectFolderModal)
                                                      m_ProjectFolderModal->HideLoading();
                                                  m_ProjectSwitchPending = false;
                                              });
    if (started)
        return;

    Logger::Log::Error("Editor: could not roll the project source back to '{}'",
                       pending.OldAssetRoot.string());
    m_Packages->MountForProject(EngineCore::GetInstance().GetWorkspaceRoot());
    if (m_ProjectFolderModal)
        m_ProjectFolderModal->HideLoading();
    m_ProjectSwitchPending = false;
}

void EditorApplication::FinishProjectFolderSwitch(const ProjectSwitch& pending)
{
    // Editor source is still registered. No need to re-register or re-resolve editor UI refs.
    const std::filesystem::path& projectPath = pending.ProjectPath;
    const std::filesystem::path& newAssetRoot = pending.NewAssetRoot;
    EngineCore& engine = EngineCore::GetInstance();
    AssetManager& am = engine.GetAssetManager();

    // Update Engine's workspace root so that GetCurrentEditorProjectPaths() returns correct paths
    engine.SetWorkspaceRoot(projectPath);
    if (m_MarkupBridge)
        m_MarkupBridge->LoadProjectState(projectPath);
    Editor::SvgRasterSettings::ApplySavedDefaults(projectPath);
    // Plugin enablement lives in the project's settings, so the workspace
    // switch changes the answers: drop the memoized states before package
    // modules mount and re-resolve against the new project.
    Editor::EditorPluginRegistry::Get().InvalidateEnabledCache();
    if (m_UniversalSearch)
        m_UniversalSearch->OnProjectRootChanged(projectPath);

    // Mount the new project's packages now that the project source points at it.
    m_Packages->MountForProject(projectPath);

    // Reload layout presets from the newly-opened project. The initial load during Initialize()
    // ran against the startup/default workspace root; without this reload, saves go to the new
    // project but loads still reflect the old in-memory list.
    InitializeLayoutPresets();
    LoadLayoutPresetsFromPreferences();
    // The dockspace is already built at this point, so SetRoot() alone (called inside
    // LoadLayoutPresetsFromPreferences) does not visually apply the layout. Recall the
    // active preset explicitly so the live dockspace is rebuilt from the loaded model.
    if (m_ActiveLayoutPresetIndex >= 0)
        RecallLayoutPreset(m_ActiveLayoutPresetIndex);
    SyncLayoutPresetToolbar();

    // Re-apply project-scoped runtime settings after workspace/project switch.
    if (ECS::World* world = engine.EnsurePrimaryWorld())
    {
        EnsureWorldHasPhysicsSettingsFromProject(*world, projectPath);
    }
    if (auto* rs = m_EditorContext ? m_EditorContext->RenderServices : nullptr)
    {
        // Drop the material-side caches keyed on the previous project's paths:
        // build context, compiled SPIR-V and the inspector's compile cache.
        // MaterialSystem owns the per-cache reasons, including which one is
        // deliberately kept.
        rs->Materials().OnProjectSwitched();

        // Re-register primitive meshes and default material after the project
        // switch.  The asset rebind may have invalidated registry entries that
        // were populated during initial startup.
        Engine::Renderer::PrimitiveGenerator::RegisterAll(*rs);

        const std::filesystem::path pipelinePath = Editor::LoadActiveRenderPipelinePathFromProjectSettings(projectPath);
        if (!pipelinePath.empty())
        {
            Logger::Log::Info("Editor: loaded active render pipeline from project settings after switch: {}",
                              pipelinePath.generic_string());
            rs->Spine().SetActiveRenderPipelinePath(pipelinePath);
        }
        else
        {
            Logger::Log::Info("Editor: project settings contain no active render pipeline after switch; using default");
            rs->Spine().SetActiveRenderPipelinePath(std::filesystem::path("RenderPipelines/ForwardPlus.rendergraph"));
        }
    }

    // The project's renderer knobs — MSAA, AA mode, TAAU render scale,
    // render-scale mode, mesh LOD — all live on a RenderServices that this
    // switch does not tear down, so they keep the OUTGOING project's values
    // unless re-applied. The settings page reads its displayed values back from
    // the incoming project's file, so skipping this shows the user a value the
    // renderer is not using, and re-selecting the shown value fires no callback
    // for the page to heal itself with.
    //
    // Per window, not just m_EditorContext->RenderServices: a floating window
    // owns a private RenderServices, and two panes running different AA modes in
    // one session is exactly what that would produce. Ordered after
    // PrimitiveGenerator::RegisterAll above, which the LOD apply re-derives
    // over, and after SetWorkspaceRoot, which the fan-out reads projectPath
    // back from.
    ApplyProjectRenderSettingsToAllWindows();

    // rendering.hdr is the one project-scoped renderer setting outside that
    // apply: HDR output is per-window swapchain state owned by
    // HdrOutputController, which re-reads the request from the workspace root
    // when the queued (settle-gated) refresh lands. When the incoming project
    // resolves to the mode already active, the swapchain is left untouched.
    RequestHdrOutputRefreshForAllWindows();

    // Update EditorContext
    if (m_EditorContext)
    {
        m_EditorContext->AssetsRoot = newAssetRoot;
        m_EditorContext->Assets = &am;
    }

    // Dismiss the project picker before scanning Assets/ and updating thumbnails. LoadDirectory and
    // grid refresh can take noticeable time on large projects; those are not required to close the modal.
    if (m_ProjectFolderModal)
    {
        m_ProjectFolderModal->SetPath(projectPath);
        m_ProjectFolderModal->AddToRecentProjects(projectPath);
        m_ProjectFolderModal->Hide();
    }

    const std::filesystem::path deferredProjectPath = projectPath;
    const std::filesystem::path deferredAssetRoot = newAssetRoot;
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui && m_Windows[0]->ui->GetDispatcher())
    {
        m_Windows[0]->ui->PostToUI([this, deferredProjectPath, deferredAssetRoot]()
        { ApplyProjectFolderUiAsync(deferredProjectPath, deferredAssetRoot); });
    }
    else
    {
        ApplyProjectFolderUiAsync(deferredProjectPath, deferredAssetRoot);
    }
    m_ProjectSwitchPending = false;
}

void EditorApplication::ApplyProjectFolderUiAsync(const std::filesystem::path& projectPath,
                                                    const std::filesystem::path& newAssetRoot)
{
    if (m_IsShuttingDown)
        return;

    // Update AssetsPanel
    for (auto& panel : m_PanelStorage)
    {
        if (auto* assetsPanel = dynamic_cast<AssetsPanel*>(panel.get()))
        {
            assetsPanel->SetAssetsRoot(newAssetRoot);
        }

        // Update BookmarksPanel to reload bookmarks for new project
        if (auto* bookmarksPanel = dynamic_cast<BookmarksPanel*>(panel.get()))
        {
            if (m_EditorContext)
            {
                bookmarksPanel->SetEditorContext(m_EditorContext.get());
                // Defer a refresh to ensure bookmarks are validated after asset registry is updated
                bookmarksPanel->PostAction([bookmarksPanel]()
                {
                    if (bookmarksPanel)
                        bookmarksPanel->RefreshBookmarks();
                });
            }
        }
    }

    // Update thumbnail provider and preload thumbnails in background
    if (m_ThumbnailProvider)
    {
        m_ThumbnailProvider->SetAssetsRoot(newAssetRoot);
        if (auto* svc = dynamic_cast<ThumbnailService*>(m_ThumbnailProvider.get()))
        {
            const auto p = GameEngine::Editor::GetCurrentEditorProjectPaths();
            if (!p.thumbnailsRoot.empty())
                svc->SetCacheRoot(p.thumbnailsRoot);
        }
    }

    Logger::Log::Info("Editor: Project folder updated successfully. New asset root: {}", newAssetRoot.string());

    // Save the project path to preferences for next launch
    {
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err); // Load existing preferences
        prefs.SetString("lastProjectPath", projectPath.string());
        (void)prefs.Save(&err);
        if (!err.empty())
        {
            Logger::Log::Warning("Editor: Failed to save last project path to preferences: {}", err);
        }
    }

    // Editor theme style assets survive the rebind (they're from the editor source, not project).
    // Refresh the live tree now that panel/project UI state has been updated so the first rendered
    // project frame does not use stale style/layout data from the startup/default workspace.
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
    {
        if (UIElement* root = m_Windows[0]->ui->GetRootElement())
        {
            m_Windows[0]->ui->MarkStyleDirtySubtree(root);
            m_Windows[0]->ui->RequestRelayout();
        }
    }
    m_PendingProjectFolderStyleRelayout = false;

    if (SceneViewPanel* sceneViewPanel = FindFirstPanelOfType<SceneViewPanel>(m_PanelStorage))
        sceneViewPanel->FocusSceneViewViewport();

    // Reinitialize VCS integration for the new project folder (quiet; UI will reflect status).
    if (m_VcsService)
    {
        if (m_VcsUi)
            m_VcsUi->InitializeForProject(
                *m_VcsService, projectPath, m_EditorContext.get());
        else
            m_VcsService->InitializeForProject(projectPath);
    }

    // Native C++ hot-reload is wired from Update() on workspace-root change (covers this
    // UI-switch path plus explicit --project / default exe-dir / auto-load-last), so it is
    // not duplicated here.

    // Main window title (project + active scene) and focus the editor
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->window)
    {
        RefreshMainWindowTitle();
        m_Windows[0]->window->Focus();
    }
}

void EditorApplication::ApplyProjectLodImportPolicy(const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return;

    // Push the project's auto-LOD import policy down to the engine import path
    // (which has no editor dependency). Project-scoped so the whole team gets
    // uniform import behavior; mirrors the physics-settings-on-open pattern.
    Editor::SettingsStore projectSettings = Editor::OpenProjectSettings(projectRoot);
    std::string err;
    (void)projectSettings.Load(&err);
    bool autoLODs = true; // default-on for static meshes when the project has no explicit key
    projectSettings.TryGetBool("import.autoGenerateLODs", autoLODs);
    GameEngine::LODImportSettings lod = GameEngine::GetLODImportSettings();
    lod.AutoGenerateOnImport = autoLODs;
    GameEngine::SetLODImportSettings(lod);
}

void EditorApplication::WireNativeScriptingForProject(const std::filesystem::path& projectRoot)
{
    auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager();
    if (!nativeScripts || projectRoot.empty())
        return;

    const std::filesystem::path sdkRoot = PathUtils::GetExecutableDirectory() / "SDK";
    GameEngine::NativeScripting::NativeBuildConfig cfg;
    std::string manifestErr;
    if (!GameEngine::NativeScripting::LoadSdkManifest(sdkRoot, cfg, manifestErr))
    {
        Logger::Log::Info("Editor: native C++ scripting unavailable ({}); user scripts disabled", manifestErr);
        return;
    }

    // Native sources live anywhere under <project>/Assets — the same tree the AssetManager
    // already watches, so the NativeScriptManager subscription consolidates onto that one
    // OS watcher. The user DLL builds in <project>/.Cache so it never touches the repo.
    const std::filesystem::path assetsDir = projectRoot / "Assets";
    cfg.SourceDir = assetsDir;
    cfg.BuildDir = projectRoot / ".Cache" / "NativeScripts" / "build";
    cfg.ActiveDir = projectRoot / ".Cache" / "NativeScripts" / "active";
    // P1 package Cpp modules: one build config per module, sharing the project's
    // engine build interface (SDK-manifest fields in cfg) but rooted at the
    // module's own sources and the package's own writable cache (its .Cache, or
    // the package cache's .derived sibling for immutable git entries) — its
    // build cache and shadow copies never collide with the project's. Topo
    // order preserved from the resolution. Each module compiles with its
    // effective defines (own + transitive dependencies'), never with defines
    // of packages it can't see.
    {
        std::vector<GameEngine::NativeScripting::NativeBuildConfig> packageConfigs;
        for (const GameEngine::PackageCodeModule& module : m_Packages->GetCodeModules())
        {
            if (module.Lang != GameEngine::PackageModuleRecord::ModuleLang::Cpp)
                continue;
            const bool isEditorModule =
                module.Kind == GameEngine::PackageModuleRecord::ModuleKind::Editor;
            if (isEditorModule && cfg.EditorImportLib.empty())
            {
                Logger::Log::Error("Editor: package '{}' has a native Editor-kind module but the staged "
                                   "SDK carries no EditorSDK interface (editorimportlib); module skipped",
                                   module.PackageName);
                continue;
            }
            if (module.UsesManagedNativeCache && !RetainPackageNativeCache(module.CacheDir))
            {
                Logger::Log::Error("Editor: cannot lease native cache '{}' for package '{}'; module skipped",
                                   module.CacheDir.string(), module.PackageName);
                continue;
            }
            GameEngine::NativeScripting::NativeBuildConfig pkgCfg = cfg;
            pkgCfg.SourceDir = module.RootDir;
            pkgCfg.PackageRootDir = module.PackageRootDir;
            // One generated-project/build/active triple per module half, all under
            // the package's writable cache: package sources can sit in read-only or
            // signed installs (generated files must never land there), and the
            // runtime and editor halves of one package share a CacheDir — two
            // generated projects must never share a CMake build tree.
            const auto cacheSubdir = [&module, isEditorModule](const char* leaf) {
                return module.CacheDir / "NativeScripts" /
                       (isEditorModule ? std::string(leaf) + "-editor" : std::string(leaf));
            };
            pkgCfg.ProjectDir = cacheSubdir("project");
            pkgCfg.BuildDir = cacheSubdir("build");
            pkgCfg.ActiveDir = cacheSubdir("active");
            pkgCfg.ModuleName = module.AssemblyName;
            pkgCfg.PrebuiltDir = module.PrebuiltDir; // P3: shipped binaries beat source builds
            if (isEditorModule)
            {
                // The editor half of a package extends its runtime half: give it
                // the sibling Runtime Cpp module's headers (one package, one
                // source of truth for shared option/component types).
                for (const GameEngine::PackageCodeModule& peer : m_Packages->GetCodeModules())
                {
                    if (peer.PackageName == module.PackageName &&
                        peer.Lang == GameEngine::PackageModuleRecord::ModuleLang::Cpp &&
                        peer.Kind == GameEngine::PackageModuleRecord::ModuleKind::Runtime)
                        pkgCfg.IncludeDirs.push_back(peer.RootDir);
                }
            }
            else
            {
                // Runtime modules must not fold the editor surface into their
                // ABI digest (they never link it) — keep their digests
                // editor-independent and Player-identical.
                pkgCfg.EditorImportLib.clear();
                pkgCfg.EditorIncludeDirs.clear();
            }
            GameEngine::NativeScripting::AppendPackageDefines(pkgCfg, module.Defines);
            packageConfigs.push_back(std::move(pkgCfg));
        }
        nativeScripts->SetPackageModuleConfigs(std::move(packageConfigs));
    }

    // The project module compiles with the union of enabled-package defines, so
    // game C++ can `#if GE_PACKAGE_OCEAN_PACK` (defines propagation). Shared
    // helper: BuildPipeline recomputes the ABI digest from the same input.
    // Project user scripts are runtime code — they never see the EditorSDK,
    // and their ABI digest must stay editor-independent (BuildPipeline
    // recomputes it without any editor interface).
    cfg.EditorImportLib.clear();
    cfg.EditorIncludeDirs.clear();
    GameEngine::NativeScripting::AppendPackageDefines(cfg, m_Packages->GetPackageDefines());
    nativeScripts->SetBuildConfig(cfg);

    // Bottom-bar status indicator: set the status Label's text + state-color class. Looked
    // up by id per call (cheap — only fires on build edges) so a UI/layout rebuild can never
    // leave a dangling pointer. State colors live in core.css (.is-building/.is-ok/.is-fail).
    auto setNativeScriptStatus = [this](const std::string& text, const char* stateClass) {
        if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->ui)
            return;
        UIElement* root = m_Windows[0]->ui->GetRootElement();
        if (!root)
            return;
        auto* label = dynamic_cast<Label*>(root->FindById("NativeScriptStatus"));
        if (!label)
            return;
        label->RemoveClass("is-building");
        label->RemoveClass("is-ok");
        label->RemoveClass("is-fail");
        label->AddClass(stateClass);
        label->SetText(text);
    };

    // Build STARTED (main thread): only fires for a real compile, not a cached no-op load.
    nativeScripts->SetBuildStartedCallback([setNativeScriptStatus]() {
        setNativeScriptStatus("Building scripts...", "is-building");
    });

    // Build-complete callback (main thread): on failure bring the log forward (the error
    // lines are already routed there) and flag the status bar. A successful module load
    // refreshes only panels currently showing a custom inspector owned by that module.
    // This covers inspectors first registered by a late package load as well as replaced
    // inspector code, without rebuilding unrelated selections for every startup package.
    // Component layout changes use the more precise InspectorRebuild notifications below.
    nativeScripts->SetBuildCompletedCallback([this, setNativeScriptStatus](const GameEngine::NativeScripting::NativeBuildResult& result) {
        if (!result.Success())
        {
            setNativeScriptStatus("Build failed - using last good build", "is-fail");
            if (m_PanelManager && !m_Windows.empty() && m_Windows[0])
                m_PanelManager->ShowOrActivatePanel(m_Windows[0].get(), "Log");
            return;
        }
        setNativeScriptStatus("Scripts loaded", "is-ok");
        if (result.RegistrationsChanged && !result.ModuleId.empty())
        {
            for (const auto& panel : m_PanelStorage)
            {
                if (auto* inspectorPanel = dynamic_cast<InspectorPanel*>(panel.get()))
                    inspectorPanel->RequestRefreshForInspectorModule(result.ModuleId);
            }
        }
    });

    // Hot-reload component migration: bracket each real (re)load so a component whose fields
    // changed (added/removed/reordered/retyped) while already placed on entities is migrated
    // rather than reading stale/aliased bytes. The migrator owns the whole S8 policy: the
    // before-hook snapshots each placeable component's field table + byte size with OWNED name
    // copies (the registry's default tables are non-owning views into module rdata, which the
    // C12 unload may unmap inside the load this brackets); the after-hook re-packs placed
    // instances of any component whose layout changed — same-named fields carry, new fields
    // default, no-reflection components reset loudly.
    // Editor-kind unload refusal diagnostics: when the C12 unload refuses to
    // unmap a superseded editor-kind image, this names the editor-side
    // registrations pinning it (the registries stamp entries from the
    // loader's active-module bracket).
    nativeScripts->SetEditorModulePinReport(
        [](std::string_view moduleId) -> std::string
        {
            std::vector<std::string> pins;
            Editor::EditorPluginRegistry::Get().AppendModulePins(moduleId, pins);
            Editor::EditorPanelRegistry::Get().AppendModulePins(moduleId, pins);
            Editor::EditorComponentTraitsRegistry::Get().AppendModulePins(moduleId, pins);
            Editor::Picking::EditorPickProviderRegistry::Get().AppendModulePins(moduleId, pins);
            Editor::EditorVcsProviderRegistry::Get().AppendModulePins(moduleId, pins);
            InspectorRegistry::Get().AppendModulePins(moduleId, pins);
            Editor::DebugRequestGateRegistry::Get().AppendModulePins(moduleId, pins);
            if (pins.empty())
                return "no stamped editor-registry entries (module code may still be referenced "
                       "by live UI)";
            std::string joined;
            for (const std::string& pin : pins)
            {
                if (!joined.empty())
                    joined += ", ";
                joined += pin;
            }
            return joined;
        });

    auto layoutMigrator = std::make_shared<ECS::ComponentLayoutReloadMigrator>();
    nativeScripts->SetReloadHooks(
        [layoutMigrator]() { layoutMigrator->SnapshotLayouts(); },
        [this, layoutMigrator]() {
            ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
            if (!world)
                return;
            const std::vector<ECS::ComponentTypeId> migratedIds =
                layoutMigrator->MigrateChangedLayouts(*world);

            // Re-apply preserved unknown components whose type just became registered (e.g. a user
            // native module finished compiling after the scene had already loaded, so the loader
            // stashed the component verbatim instead of dropping it). Runs AFTER layout migration so
            // a freshly-instantiated component lands at the current layout. Resolved entries are
            // removed (so the next save emits them once, via the real reflected path); still-unknown
            // entries stay for a later reload.
            auto& unresolved = world->GetUnresolvedComponents();
            if (!unresolved.Empty())
            {
                GameEngine::Scene::SceneLoadContext loadCtx{};
                loadCtx.TargetWorld = world;
                for (auto entIt = unresolved.Map().begin(); entIt != unresolved.Map().end();)
                {
                    const ECS::EntityHandle e = entIt->first;
                    if (!world->IsValid(e))
                    {
                        entIt = unresolved.Map().erase(entIt); // entity destroyed before module loaded
                        continue;
                    }
                    auto& preserved = entIt->second;
                    for (auto compIt = preserved.begin(); compIt != preserved.end();)
                    {
                        const GameEngine::Scene::ISceneComponentSchema* schema =
                            GameEngine::Scene::SceneSchemaRegistry::Find(compIt->Name);
                        if (!schema)
                        {
                            ++compIt; // type still not registered — keep it preserved
                            continue;
                        }
                        bool ok = true;
                        std::string applyErr;
                        if (compIt->Props.empty())
                        {
                            ok = schema->AddDefault(*world, e, &applyErr);
                        }
                        else
                        {
                            for (const auto& [prop, value] : compIt->Props)
                            {
                                if (!schema->ApplyProperty(*world, e, loadCtx, prop, value, &applyErr))
                                {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (ok)
                        {
                            Logger::Log::Info("[NativeScripting] re-applied preserved component '{}'", compIt->Name);
                            compIt = preserved.erase(compIt);
                        }
                        else
                        {
                            Logger::Log::Warning("[NativeScripting] re-apply of preserved component '{}' failed: {}",
                                                 compIt->Name, applyErr);
                            ++compIt;
                        }
                    }
                    if (preserved.empty())
                        entIt = unresolved.Map().erase(entIt);
                    else
                        ++entIt;
                }
                world->ProcessCommands();
            }

            // A migration re-packs bytes under the SAME ComponentTypeId, so panels
            // watching component-ID signatures (the inspector's rebuild poll) cannot
            // see it — a stale field plan would keep displaying, and WireEdit keep
            // committing, pre-migration offsets. Broadcast InspectorRebuild for every
            // entity holding a migrated component so every observing panel rebuilds
            // from the new field table. Fired after the preserved-component re-apply
            // above so a triggered rebuild reads fully settled state. Idempotent
            // (the after-hook contract): a re-fire just forces a redundant rebuild.
            if (!migratedIds.empty() && m_ChangeNotifications)
            {
                std::vector<ECS::EntityHandle> alive;
                world->GetAliveEntitiesSnapshot(alive);
                for (const ECS::EntityHandle e : alive)
                {
                    ECS::Archetype* archetype = world->GetEntityArchetype(e);
                    if (!archetype)
                        continue;
                    for (const ECS::ComponentTypeId id : migratedIds)
                    {
                        if (archetype->GetSignature().Contains(id))
                            m_ChangeNotifications->NotifyComponentChanged(
                                {world, e, id,
                                 Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild});
                    }
                }
            }
        });

    // SetWatchDirectory arms the GLOBAL FileWatchingService (StartWatching),
    // which would re-enable every subscription's callbacks during UI replay
    // and defeat the replay determinism gate applied at startup. Skip the
    // watch in replay mode; builds still run below.
    if (!m_UiReplayScenarioPath.has_value())
        nativeScripts->SetWatchDirectory(assetsDir);

    // A platform that cannot load native modules has no consumer for a build,
    // and the recursive walk below is the kind Platform::SupportsSynchronousDirectoryWalk
    // forbids there; skip the whole native-source probe.
    if (!Platform::SupportsDynamicNativeModules())
        return;

    // Only kick a build if the project actually has native sources, so an asset-only
    // project doesn't spin cmake to produce an empty module.
    const bool hasNativeSources = GameEngine::NativeScripting::HasWatchedNativeSource(assetsDir);
    // Package Cpp modules kick the build machinery even when the project itself
    // has no native sources — their DLLs must load on project open.
    const bool hasPackageNativeModules =
        std::any_of(m_Packages->GetCodeModules().begin(), m_Packages->GetCodeModules().end(),
                    [](const GameEngine::PackageCodeModule& m) {
                        return m.Lang == GameEngine::PackageModuleRecord::ModuleLang::Cpp;
                    });
    if (hasNativeSources || hasPackageNativeModules)
        nativeScripts->RequestRebuild();
}

void EditorApplication::RefreshMainWindowTitle()
{
    if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->window)
        return;

    std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!workspaceRoot.empty())
        workspaceRoot = workspaceRoot.lexically_normal();

    std::optional<std::filesystem::path> scenePath;
    std::optional<std::string> sceneDisplayName;
    if (m_SceneEditor)
    {
        scenePath = m_SceneEditor->GetActiveScenePath();
        if (scenePath.has_value() && !scenePath->empty())
            scenePath = scenePath->lexically_normal();
        sceneDisplayName = m_SceneEditor->GetActiveSceneDisplayName();
    }

    std::string title = "Open Engine Editor " + Editor::EditorVersionText();
    if (!workspaceRoot.empty())
    {
        std::string folderName = workspaceRoot.filename().string();
        if (folderName.empty())
            folderName = workspaceRoot.string();
        title += " - ";
        title += folderName;
    }
    if (m_SceneEditor)
    {
        title += " - ";
        if (sceneDisplayName.has_value() && !sceneDisplayName->empty())
            title += *sceneDisplayName;
        else if (scenePath.has_value() && !scenePath->empty())
            title += scenePath->filename().string();
        else
            title += "Untitled";
        if (m_SceneEditor->IsSceneDirty())
            title += " *";
    }

    // Session provenance, appended after the scene so it can never truncate or displace
    // it. Compact by design — the full descriptor is on get_editor_state. Worktree, not
    // branch, is the identity here: a `git switch` after launch leaves this binary built
    // from the branch named, not the one checked out now. Build configuration is
    // deliberately absent — the bottom-bar build tag owns that display.
    if (const std::optional<std::string> session = Editor::Startup::FormatSessionChromeSuffix(m_SessionDescriptor))
    {
        title += "  [";
        title += *session;
        title += ']';
    }

    if (title != m_LastMainWindowTitle)
    {
        m_LastMainWindowTitle = std::move(title);
        m_Windows[0]->window->SetTitle(m_LastMainWindowTitle);
    }

    if (m_TopToolbar)
    {
        const bool dirty = m_SceneEditor && m_SceneEditor->IsSceneDirty();
        if (dirty != m_LastSaveDirty)
        {
            m_LastSaveDirty = dirty;
            m_TopToolbar->SetSaveDirty(dirty);
        }
    }
}


void EditorApplication::QueueOpenInternalFile(std::filesystem::path path)
{
    if (path.empty())
    {
        return;
    }

    m_DeferredOpenInternalFile = std::move(path);
    m_DeferredPostUiActions.EnqueueUnique("editor.open-internal-file",
                                          [this]()
                                          { this->OpenDeferredInternalFile(); });
}

void EditorApplication::FlushPreUiDeferredActions()
{
    GE_CPU_PROFILE_SCOPE("EditorApplication.Update.FlushPreUiDeferredActions");
    m_DeferredPreUiActions.FlushOnce();
}

void EditorApplication::FlushPostUiDeferredActions()
{
    GE_CPU_PROFILE_SCOPE("EditorApplication.Update.FlushPostUiDeferredActions");
    m_DeferredPostUiActions.FlushOnce();
}


void EditorApplication::RequestHdrOutputRefreshForAllWindows()
{
    m_HdrOutput->RequestRefreshForAllWindows();
}

void EditorApplication::ApplyProjectRenderSettingsToAllWindows()
{
    const auto& wsRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Editor::SettingsStore store = Editor::OpenProjectSettings(wsRoot);
    std::string error;
    (void)store.Load(&error);
    // A project that has never chosen an anti-aliasing mode gets one written
    // here, once, against this machine's capabilities — before the snapshot is
    // fanned out, so every window applies the value that reached the file.
    // Every window's device is the same physical GPU, so the main one answers
    // for all of them.
    if (auto* mainRs = MainRenderServices())
        (void)Editor::MaterializeDefaultAntiAliasing(store, mainRs->GetAntiAliasingDeviceCaps());
    for (auto& win : m_Windows)
    {
        if (win && win->renderCtx)
        {
            if (auto* rs = win->renderCtx->GetRenderServices())
                Editor::ApplyProjectRenderSettings(*rs, store);
        }
    }
}

void EditorApplication::ApplyDynamicResolutionFromMainWindowToAllWindows()
{
    auto* mainRs = MainRenderServices();
    if (mainRs == nullptr)
        return;

    // Config before mode, matching Editor::ApplyProjectRenderSettings: entering
    // Dynamic resets the controller from the scale standing at that moment, so
    // the tuning has to be in place first. The main window is a no-op pass —
    // its config is already this value and the mode call takes the same-mode
    // branch — which keeps the caller's own apply the only one that transitions.
    //
    // A copy, not a reference: the loop feeds main's own config back through
    // main's setter, and the getter hands out a reference to it.
    const Engine::Renderer::DynamicResolutionConfig config =
        mainRs->GetDynamicResolutionConfig();
    const auto mode = mainRs->GetDynamicResolutionMode();
    size_t windowIndex = 0;
    for (auto& win : m_Windows)
    {
        if (win && win->renderCtx)
        {
            if (auto* rs = win->renderCtx->GetRenderServices())
            {
                rs->SetDynamicResolutionConfig(config);
                rs->SetDynamicResolutionMode(mode);
                // One line per window reached, like the file-backed fan-out.
                // A live knob that never persists leaves no other trace of how
                // far it got.
                Logger::Log::Info(
                    "Editor: applied dynamic resolution to window {}: mode {}, target {:.3f} ms",
                    windowIndex, static_cast<uint32_t>(mode), config.TargetGpuMs);
            }
        }
        ++windowIndex;
    }
}


void EditorApplication::BindGraphPanel(GraphPanel& panel)
{
    panel.SetUndoRedoService(m_UndoRedo.get());
    panel.SetRequestOpenGraphAsset([this](const std::filesystem::path& openPath) {
        return OpenGraphAsset(openPath);
    });
    panel.SetOpenColorPickerWindow(m_ColorPicker->AsOpenCallback());
    if (m_EditorContext)
        panel.SetContext(m_EditorContext.get());
    if (auto* inspectorPanel = FindFirstPanelOfType<InspectorPanel>(m_PanelStorage))
    {
        panel.SetOnNodeSelected(
            [inspectorPanel](const std::string& nodeId, const std::string& nodeTypeId,
                             const std::string& displayName,
                             const std::unordered_map<std::string, std::string>& parameters,
                             const std::string& kindId) {
                inspectorPanel->ShowGraphNode(nodeId, nodeTypeId, displayName, parameters, kindId);
            });
        panel.SetOnNodeDeselected([inspectorPanel]() { inspectorPanel->ClearGraphNodeIfShown(); });
        panel.SetOnTransitionSelected(
            [inspectorPanel](const std::string& linkId, const std::string& fromName,
                             const std::string& toName, const GraphTransitionDesc& desc) {
                inspectorPanel->ShowGraphTransition(linkId, fromName, toName, desc);
            });
    }
    if (m_PanelManager)
    {
        GraphPanel* raw = &panel;
        m_PanelManager->RegisterUpdateCallback(raw, [raw](const EditorPanelManager::PanelUpdateContext&) {
            raw->Update();
        });
    }
}

void EditorApplication::ActivateGraphDock(EditorWindowContext* ctx, GraphPanel& panel)
{
    if (!m_PanelManager || !ctx || !ctx->docking)
        return;
    std::string dockId;
    for (const auto& [id, element] : ctx->docking->GetPanels())
    {
        if (element == &panel)
        {
            dockId = id;
            break;
        }
    }
    if (dockId.empty())
    {
        if (const char* kindDock = GraphDockPanelIdForKind(panel.PanelKindId()))
            dockId = kindDock;
        else
            dockId = EditorPanelIds::NodeGraph;
    }
    const char* kindDock = GraphDockPanelIdForKind(panel.PanelKindId());
    m_PanelManager->ShowOrActivatePanel(ctx, dockId,
                                        kindDock ? kindDock : EditorPanelIds::SceneView);
}

bool EditorApplication::OpenGraphAsset(const std::filesystem::path& path)
{
    if (path.empty() || m_Windows.empty())
        return false;

    EditorWindowContext* ctx = m_Windows[0].get();
    std::string ext = path.extension().string();
    for (char& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    std::string kind;
    if (!GraphPanel::TryReadKindId(path, kind) || kind.empty())
        kind = KindIdFromGraphExtension(ext);
    if (kind.empty() || !IsKnownGraphKindId(kind))
        return false;

    if (GraphPanel* live = GraphPanel::FindLiveByPath(path))
    {
        ActivateGraphDock(ctx, *live);
        live->NoteAsLastOpened();
        return true;
    }

    GUID assetGuid{};
    if (m_EditorContext && m_EditorContext->Assets)
        assetGuid = m_EditorContext->Assets->ResolveAssetGuid(path);
    const std::string assetDockId = MakeGraphDockIdForAsset(kind, assetGuid);
    if (ctx && ctx->docking)
    {
        if (auto* hashed = dynamic_cast<GraphPanel*>(ctx->docking->GetPanel(assetDockId)))
        {
            if (hashed->GetCurrentGraphPath() == path)
            {
                ActivateGraphDock(ctx, *hashed);
                hashed->NoteAsLastOpened();
                return true;
            }
            if (hashed->GetCurrentGraphPath().empty() && !hashed->HasUnsavedChanges())
            {
                ActivateGraphDock(ctx, *hashed);
                return hashed->OpenGraph(path);
            }
        }
    }

    if (GraphPanel* empty = GraphPanel::FindEmptyLiveByKind(kind))
    {
        ActivateGraphDock(ctx, *empty);
        return empty->OpenGraph(path);
    }

    std::unique_ptr<GraphPanel> created = GraphPanel::CreateForKind(kind);
    if (!created || !ctx || !ctx->docking)
        return false;

    std::string dockId = assetDockId;
    for (int suffix = 2; ctx->docking->GetPanel(dockId); ++suffix)
        dockId = assetDockId + "-" + std::to_string(suffix);

    created->SetId(dockId);
    created->SetListedInPanelMenus(false);
    GraphPanel* panel = created.get();
    m_PanelStorage.emplace_back(std::move(created));
    ctx->docking->RegisterPanel(dockId, panel);
    BindGraphPanel(*panel);
    ActivateGraphDock(ctx, *panel);
    return panel->OpenGraph(path);
}

void EditorApplication::OpenDeferredInternalFile()
{
    if (!m_DeferredOpenInternalFile.has_value())
    {
        return;
    }

    if (m_Windows.empty())
    {
        // Keep request pending until a docking host exists.
        m_DeferredPostUiActions.EnqueueUnique("editor.open-internal-file",
                                              [this]()
                                              { this->OpenDeferredInternalFile(); });
        return;
    }

    EditorWindowContext* ctx = m_Windows[0].get();
    if (!ctx || !ctx->docking)
    {
        m_DeferredPostUiActions.EnqueueUnique("editor.open-internal-file",
                                              [this]()
                                              { this->OpenDeferredInternalFile(); });
        return;
    }

    const std::filesystem::path path = *m_DeferredOpenInternalFile;
    m_DeferredOpenInternalFile.reset();

    std::string ext = path.extension().string();
    for (auto& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ExtensionOpensInGraphPanel(ext))
    {
        const bool openAsScriptGlsl =
            ext == ".glsl" && !Editor::ShouldOpenGlslInMaterialGraph(path);
        if (!openAsScriptGlsl)
        {
            (void)OpenGraphAsset(path);
        }
        else
        {
            if (m_PanelManager)
                m_PanelManager->ShowOrActivateScriptEditorPanel(ctx);
            UIElement* panelEl = ctx->docking->GetPanel(EditorPanelIds::ScriptEditor);
            if (auto* scriptPanel = dynamic_cast<ScriptEditorPanel*>(panelEl))
            {
                (void)scriptPanel->OpenScript(path);
            }
        }
    }
    else
    {
        if (m_PanelManager)
            m_PanelManager->ShowOrActivateScriptEditorPanel(ctx);
        UIElement* panelEl = ctx->docking->GetPanel(EditorPanelIds::ScriptEditor);
        if (auto* scriptPanel = dynamic_cast<ScriptEditorPanel*>(panelEl))
        {
            (void)scriptPanel->OpenScript(path);
        }
    }
}

void EditorApplication::Update(float64 deltaTime)
{
    // Heartbeat for the hang watchdog: reaching here each frame means the
    // message pump (PollEvents, immediately before Update) is being serviced.
    Editor::MainThreadHangWatchdog::NotifyAlive();

    SyncWorldPathForProjectPicker();

    // Menus displaced after Show() returned (drawn row handler, macOS deferred
    // popup) wait until the handler stack has unwound. PollEvents just ran, so
    // that stack is gone.
    DrainShowingContextMenus();

    const auto tAppUpdate0 = std::chrono::high_resolution_clock::now();


    if (m_PendingProjectFolderStyleRelayout)
    {
        if (!m_IsShuttingDown && !m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
        {
            if (UIElement* root = m_Windows[0]->ui->GetRootElement())
            {
                m_Windows[0]->ui->MarkStyleDirtySubtree(root);
                m_Windows[0]->ui->RequestRelayout();
            }
        }
        m_PendingProjectFolderStyleRelayout = false;
    }

    // Wire per-project services (native C++ hot-reload, LOD import policy) whenever the
    // workspace root changes. This single check covers every project-open path — explicit
    // --project, default exe-dir, auto-load-last, and runtime project switch — without
    // depending on which startup branch ran. Cheap (a path compare); both wires are
    // idempotent per path.
    if (!m_IsShuttingDown)
    {
        const std::filesystem::path& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        if (!workspaceRoot.empty() && workspaceRoot != m_WiredProjectRoot)
        {
            m_WiredProjectRoot = workspaceRoot;
            ApplyProjectLodImportPolicy(workspaceRoot);
            WireNativeScriptingForProject(workspaceRoot);
        }
    }

    // First-detect gate: open VCS workspace detection once every package
    // module reached its first terminal outcome — all VCS providers live in
    // package modules, and detecting earlier would let a later-ordered
    // provider transiently claim a workspace whose real provider (git,
    // DetectionOrder 0) is still loading. With staged prebuilt binaries the
    // pass completes within the first frames, so the frame-2 init below
    // detects immediately; a cold source build holds detection until done.
    if (m_VcsService && !m_VcsProvidersReadySignaled && !m_IsShuttingDown &&
        !m_WiredProjectRoot.empty())
    {
        auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager();
        if (!nativeScripts || !nativeScripts->IsInitialized() ||
            nativeScripts->InitialPackageModulePassComplete())
        {
            m_VcsProvidersReadySignaled = true;
            m_VcsService->NotifyProvidersReady();
        }
    }

    // Provider (re)registrations defer their heavy work (integration
    // shutdown joins poll threads; Initialize spawns processes) because they
    // fire from module static init under the loader lock. Run it here.
    if (m_VcsService && !m_IsShuttingDown)
        m_VcsService->PumpDeferredWork();

    // Panel construction can finish after the second startup frame (notably
    // while package editor modules are loading).  Keep the deferred request
    // armed until the service actually exists instead of permanently missing
    // VCS initialization when that frame has already passed.
    if (m_PendingVcsInit && m_VcsService && m_StartupFrameCount >= 2)
    {
        m_PendingVcsInit = false;
        const auto& projectRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        if (m_VcsUi)
            m_VcsUi->InitializeForProject(
                *m_VcsService, projectRoot, m_EditorContext.get());
        else
            m_VcsService->InitializeForProject(projectRoot);
    }

    // Spread deferred startup work across multiple frames to avoid a single-frame spike.
    // Frame 0: nothing extra (just the essential UI PostActions from Initialize)
    // Frame 2: VCS init + capture tools (RenderDoc, Nsight) + ScriptMenu (non-visual)
    // Frame 4: Font prefetch (13 families — only needed if user opens Settings)
    if (m_StartupFrameCount < 10)
    {
        ++m_StartupFrameCount;

        if (m_StartupFrameCount == 2)
        {
            // Post-init (capture tools, ScriptMenu). VCS initialization is kept
            // independently pending above because the service may arrive late.
            if (m_PendingPostInit)
            {
                m_PendingPostInit = false;
                m_RenderDoc = std::make_unique<RenderDocCapture>();
                m_RenderDoc->Initialize();
                // Nsight binds to its interception library, which attaches when
                // the Vulkan device is hooked -- so this must run after device
                // creation, not at startup.
                m_NsightCapture = std::make_unique<NsightGraphicsCapture>();
                m_NsightCapture->Initialize();
                auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
                if (clr.IsInitialized())
                    (void)Editor::ScriptMenuRegistry::Get().TryRegisterManagedBridge(clr);
                if (m_DownloadManager && m_EditorContext)
                    Editor::Automation::RunStartupSmokeHooks(*m_DownloadManager, m_EditorContext->AssetsRoot);
            }
        }

        if (m_StartupFrameCount == 4)
        {
            // Font prefetch (13 families for Settings dropdown)
            if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
                Editor::PrefetchEditorFontOptions(m_Windows[0]->ui.get());
        }
    }

    // Update project folder picker modal to process deferred folder loading
    if (m_ProjectFolderModal)
    {
        m_ProjectFolderModal->Update();
    }

    TryOpenDeferredStartupScene();

    // Poll scene dirty state every frame so the save-button indicator stays in sync.
    // Refresh the title on the first tick and thereafter only when something the title
    // is built from changed. The first tick matters on its own: an editor that opens no
    // scene would otherwise keep the placeholder window title for its whole life, naming
    // neither the project nor the session.
    //
    // The display name is watched alongside the path because it is what the title
    // actually prints, and the two move independently: consecutive failed opens all
    // leave the path empty while the name follows the file that failed, so watching
    // the path alone pins the title to the FIRST failure's file name forever.
    {
        const bool dirty = m_SceneEditor && m_SceneEditor->IsSceneDirty();
        std::optional<std::filesystem::path> activeScenePath;
        std::optional<std::string> sceneDisplayName;
        if (m_SceneEditor)
        {
            activeScenePath = m_SceneEditor->GetActiveScenePath();
            if (activeScenePath.has_value() && !activeScenePath->empty())
                activeScenePath = activeScenePath->lexically_normal();
            sceneDisplayName = m_SceneEditor->GetActiveSceneDisplayName();
        }

        // An empty last-title is the "never computed" state, since RefreshMainWindowTitle
        // always stores a non-empty string.
        bool shouldRefreshTitle = m_LastMainWindowTitle.empty();
        if (activeScenePath != m_LastTitleScenePath)
        {
            m_LastTitleScenePath = activeScenePath;
            shouldRefreshTitle = true;
        }

        if (sceneDisplayName != m_LastTitleSceneDisplayName)
        {
            m_LastTitleSceneDisplayName = std::move(sceneDisplayName);
            shouldRefreshTitle = true;
        }

        if (dirty != m_LastSaveDirty)
        {
            m_LastSaveDirty = dirty;
            if (m_TopToolbar)
                m_TopToolbar->SetSaveDirty(dirty);
            shouldRefreshTitle = true;
        }
        if (shouldRefreshTitle)
            RefreshMainWindowTitle();
    }

    // Bottom-bar status refresh, 1 Hz. Both labels are looked up by id here rather than cached
    // so a UI rebuild can never leave a dangling pointer.
    m_FooterStatusAccum += deltaTime;
    if (m_FooterStatusAccum >= 1.0 && !m_IsShuttingDown)
    {
        m_FooterStatusAccum = 0.0;
        uint64_t errors = 0;
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = rs->GetDevice())
                errors = device->GetValidationStats().ErrorCount;
        }
        if (errors != m_ValidationBadgeErrors && !m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
        {
            // Vulkan validation badge: empty (invisible) at zero errors, so only count edges matter.
            if (UIElement* root = m_Windows[0]->ui->GetRootElement())
            {
                if (auto* label = dynamic_cast<Label*>(root->FindById("VkValidationStatus")))
                {
                    m_ValidationBadgeErrors = errors;
                    label->SetText(errors > 0 ? "VK validation: " + std::to_string(errors) : "");
                }
            }
        }

        // Build-configuration tag. The name comes from the build (GE_BUILD_CONFIG) and never
        // changes, so the id lookup is gated on the tree-structure generation: only a rebuild
        // that recreated the label (layout hot reload, window bootstrap) can have dropped the
        // text, and any such rebuild bumps the generation. A quiet tree costs one integer
        // compare here, not a tree walk.
        if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
        {
            const uint64_t treeGen = m_Windows[0]->ui->GetTreeStructureGeneration();
            if (treeGen != m_BuildConfigTagTreeGeneration)
            {
                if (UIElement* root = m_Windows[0]->ui->GetRootElement())
                {
                    if (auto* label = dynamic_cast<Label*>(root->FindById("BuildConfigStatus")))
                        label->SetText(Editor::BuildConfigStatusText());
                    m_BuildConfigTagTreeGeneration = treeGen;
                }
            }
        }
    }

    // Advance video player modal frame decode/upload.
    m_VideoPlayer.Update();

    // Advance asset view panel video previews.
    {
        std::unordered_set<AssetViewPanel*> updatedAssetViews;
        for (const auto& panel : m_PanelStorage)
        {
            if (auto* avp = dynamic_cast<AssetViewPanel*>(panel.get()))
            {
                avp->Update(static_cast<float>(deltaTime));
                updatedAssetViews.insert(avp);
            }
        }

        if (auto* sceneAssetViewPanel = FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy);
            sceneAssetViewPanel && updatedAssetViews.find(sceneAssetViewPanel) == updatedAssetViews.end())
        {
            sceneAssetViewPanel->Update(static_cast<float>(deltaTime));
        }
    }

    // Poll download-in-progress modal (auto-dismiss when all downloads finish).
    if (m_DownloadModal)
        m_DownloadModal->Poll();

    // Poll completed Polyhaven downloads and replace placeholder entities with full models.
    if (m_DownloadManager)
        m_DownloadManager->PollAndProcess();

    Editor::ViewOverlayHost::Get().Update();

    // If a drag-preview billboard is active and the download completed, swap to 3D model.
    // This may move files from temp cache to project assets, so invalidate the download
    // cache afterwards so blue text updates in the asset browser.
    {
        bool anySwapped = false;
        for (auto& panel : m_PanelStorage)
            if (auto* sv = dynamic_cast<SceneViewPanel*>(panel.get()))
            {
                anySwapped |= sv->PollDragPreviewDownload();
                }
        if (anySwapped)
            for (auto& panel : m_PanelStorage)
                if (auto* ap = dynamic_cast<AssetsPanel*>(panel.get()))
                    ap->InvalidatePolyhavenDownloadCache();
    }

    // Apply a persisted [editor_camera] pose once after a scene load.
    if (m_SceneEditor)
    {
        EditorWindowContext* targetWindow = nullptr;
        for (const auto& win : m_Windows)
        {
            if (win && win->scene)
            {
                targetWindow = win.get();
                break;
            }
        }

        if (targetWindow && targetWindow->scene)
        {
            if (auto stored = m_SceneEditor->ConsumeEditorCameraFromLastDocumentLoad())
            {
                if (stored->hadSection)
                {
                    SceneViewCameraPose pose{};
                    pose.Pos[0] = stored->camera.PosX;
                    pose.Pos[1] = stored->camera.PosY;
                    pose.Pos[2] = stored->camera.PosZ;
                    pose.YawDeg = stored->camera.YawDeg;
                    pose.PitchDeg = stored->camera.PitchDeg;
                    pose.Distance = stored->camera.Distance;
                    pose.Is2D = stored->camera.Is2D;
                    targetWindow->scene->SetCameraPose(pose);

                    // Keep the panel's temporary yaw/pitch in sync so the next
                    // input tick does not overwrite the restored camera orientation.
                    if (SceneViewPanel* svPanel = ResolveMountedPanelForWindow<SceneViewPanel>(
                            targetWindow,
                            EditorPanelIds::MountSceneView))
                    {
                        svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
                    }
                }
            }
        }
    }

    // Poll scene thumbnail capture: readback → background PNG write → UI refresh.
    if (m_SceneThumbnailCapture)
    {
        m_SceneThumbnailCapture->Poll(
            [this](const std::string& cachePath, const std::string& scenePath)
            {
                auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                const GUID thumbGuid = assetManager.ResolveAssetGuid(std::filesystem::path(cachePath));
                if (!thumbGuid.IsNull())
                {
                    const std::string eventPath = std::filesystem::path(cachePath).string();
                    const bool reloaded =
                        assetManager.ReloadAssetNow(thumbGuid) == ReloadOutcome::Reloaded;
                    assetManager.GetEventDispatcher().DispatchEvent(
                        reloaded
                            ? AssetEvents::AssetReloaded(thumbGuid, AssetType::Texture, eventPath)
                            : AssetEvents::AssetModified(thumbGuid, AssetType::Texture, eventPath));
                }

                for (auto& win : m_Windows)
                {
                    if (win && win->ui)
                        win->ui->EvictBackgroundTexture(cachePath);
                }
                if (!scenePath.empty())
                {
                    if (auto* assetsPanel = FindFirstPanelOfType<AssetsPanel>(m_PanelStorage))
                        assetsPanel->InvalidateThumbnailForPath(std::filesystem::path(scenePath));
                }
            });
    }

    Application::Update(deltaTime);
    GE_CPU_PROFILE_SCOPE("EditorApplication.Update");

    // Flush MCP debug server requests (executes queued handlers on main thread).
    if (m_DebugServer)
        m_DebugServer->FlushPendingRequests();

    // Global exit-after-frames (works even without UI replay).
    if (m_ExitAfterFrames.has_value())
    {
        if (m_ExitFrameCounter++ >= *m_ExitAfterFrames)
        {
            Logger::Log::Info("Editor: exiting after {} frames (--exit-after-frames).", *m_ExitAfterFrames);
            RequestExit();
            return;
        }
    }

    // UI replay automation: initialize lazily once per run.
    if (m_UiReplayScenarioPath.has_value() && !m_UiReplay)
    {
        UiReplayRunner::Config cfg{};
        cfg.scenarioPath = *m_UiReplayScenarioPath;
        cfg.outputJsonlPath = m_UiReplayLogPath.has_value()
                                  ? *m_UiReplayLogPath
                                  : std::filesystem::path("ui_replay.jsonl");
        // Safety: always enforce a max frame count unless the user explicitly disables it.
        cfg.exitAfterFrames = m_UiReplayExitAfterFrames.value_or(1200ull);
        cfg.windowIndex = 0;
        cfg.invokeCommandCtx = this;
        cfg.invokeCommand = [](void* userCtx, std::uint32_t commandId, std::string* outError) -> bool
        {
            if (!userCtx)
            {
                if (outError)
                    *outError = "null userCtx";
                return false;
            }
            return static_cast<EditorApplication*>(userCtx)->InvokeUiReplayCommand(commandId, outError);
        };
        cfg.resizeWindowCtx = this;
        cfg.resizeWindow = [](void* userCtx, std::uint32_t windowIndex, std::uint32_t width, std::uint32_t height, std::string* outError) -> bool
        {
            if (!userCtx)
            {
                if (outError)
                    *outError = "null userCtx";
                return false;
            }
            return static_cast<EditorApplication*>(userCtx)->ResizeUiReplayWindow(windowIndex, width, height, outError);
        };
        cfg.queryWindowCountCtx = this;
        cfg.queryWindowCount = [](void* userCtx) -> std::uint32_t
        {
            if (!userCtx)
                return 0u;
            auto* app = static_cast<EditorApplication*>(userCtx);
            return static_cast<std::uint32_t>(app->m_Windows.size());
        };

        m_UiReplay = std::make_unique<UiReplayRunner>();
        std::string err;
        if (!m_UiReplay->Initialize(cfg, &err))
        {
            Logger::Log::Error("UIReplay: failed to initialize: {}", err);
            SetExitCode(2);
            RequestExit();
            return;
        }
    }

    // Flush any pending ECS deferred commands before running editor tools/UI so
    // reads during this update see the latest world state.
    {
        GE_CPU_PROFILE_SCOPE("EditorApplication.Update.World.ProcessCommands");
        if (auto* world = EngineCore::GetInstance().EnsurePrimaryWorld())
        {
            world->ProcessCommands();
        }
    }

    // Maintain what the recipe components on splines generate, and the terrain
    // effects under a generated run (Placement/RecipeControllers.h).
    {
        GE_CPU_PROFILE_SCOPE("EditorApplication.Update.RecipeControllers");
        if (auto* world = EngineCore::GetInstance().EnsurePrimaryWorld())
        {
            if (!m_RecipeControllers)
                m_RecipeControllers = std::make_unique<Editor::RecipeControllers>();
            m_RecipeControllers->Update(*world, MainRenderServices(), m_UndoRedo.get(),
                                        static_cast<float>(deltaTime));
        }
    }

    // Drive scene document UX (Ctrl+S save/save-as, dirty prompt, scene open requests).
    if (m_SceneEditor)
    {
        m_SceneEditor->Update(GetInputSystem());
        m_SceneEditor->TickAutoSave(static_cast<float>(deltaTime), IsPlayModeBlockingSceneWrites());
    }

    // Keep PlayModeManager wired to the latest primary world pointer (best-effort).
    if (m_PlayMode)
    {
        m_PlayMode->SetWorld(EngineCore::GetInstance().EnsurePrimaryWorld());
    }

    // Process callback-deferred editor actions at a stable pre-UI point.
    FlushPreUiDeferredActions();
    m_HdrOutput->Tick();

    // Ensure editor managed menu bridge is registered (best-effort retry).
    {
        auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
        if (clr.IsInitialized())
        {
            (void)Editor::ScriptMenuRegistry::Get().TryRegisterManagedBridge(clr);
        }
    }

    // Toolbar items refresh: script items (managed snapshot updates) and
    // native package items (EditorMenuRegistry — packages load at project
    // open, after the toolbar first builds). Consume BOTH dirty flags even
    // when one already triggered the rebuild.
    {
        const bool scriptDirty = Editor::ScriptMenuRegistry::Get().ConsumeToolbarDirty();
        const bool nativeDirty = Editor::EditorMenuRegistry::Get().ConsumeDirty();
        if (m_EditorToolbar && (scriptDirty || nativeDirty))
            m_EditorToolbar->Refresh();
    }

    // UI hot reload is handled by UIManager/UIHotReload via AssetManager-driven
    // asset reload events (UILayout/UIStyle). No EditorApplication-specific
    // debounce/refresh logic is needed here.

    const auto tPreUiEnd = std::chrono::high_resolution_clock::now();

    // Flush pending log messages BEFORE UIManager::Update so the same frame's UI update
    // can consume the mutations (avoids 1-frame latency and reduces reliance on input-driven invalidation).
    {
        auto* mainWin = !m_Windows.empty() ? m_Windows[0].get() : nullptr;
        Rendering::IDevice* mainDevice = (mainWin && mainWin->renderCtx) ? mainWin->renderCtx->GetDevice() : nullptr;

        Rendering::RenderGraph::RGFrame* mainFrame = mainWin ? mainWin->RenderGraphStream.Frame.get() : nullptr;
        CollectEditorDebugMetrics(mainDevice, mainFrame);

        GE_CPU_PROFILE_SCOPE("EditorApplication.Update.LogPanel.Update");
        if (m_PanelManager)
            m_PanelManager->UpdatePanels(mainDevice, m_RenderDoc.get(), mainFrame);
    }

    const auto tDebugPanelsEnd = std::chrono::high_resolution_clock::now();

    // Drive data model updates (input/hotkeys integration to come)
    {
        GE_CPU_PROFILE_SCOPE("EditorApplication.Update.UI.UpdateAllWindows");
        // Tick animation-family panels when playing (before UI update so playheads repaint same frame).
        ForEachAnimationPanel(m_PanelStorage, [deltaTime](AnimationWindowPanel* panel)
        {
            panel->TickTimeline(static_cast<float>(deltaTime));
        });
        const std::uint32_t replayWindowIndex =
            (m_UiReplay && m_UiReplay->IsEnabled()) ? m_UiReplay->GetTargetWindowIndex() : 0xFFFFFFFFu;
        const BackgroundToolWindowUiPolicy& backgroundPolicy = GetBackgroundToolWindowUiPolicy();
        for (size_t wi = 0; wi < m_Windows.size(); ++wi)
        {
            // Use a raw pointer instead of a reference to m_Windows[wi].
            // UI event handlers inside Update() can call m_AddNativeToolWindow (e.g. opening
            // the ColorPicker), which does m_Windows.emplace_back(). If that reallocation
            // moves the vector storage, a reference to the old element is dangling.
            // The heap-allocated EditorWindowContext itself is stable (unique_ptr).
            EditorWindowContext* win = m_Windows[wi].get();
            if (win && win->ui)
            {
                GE_CPU_PROFILE_SCOPE("EditorApplication.Update.UI.UpdateWindow");

                // UIManager::Update queries swapchain size from the device for layout.
                // In shared-device mode we must activate this window's target first;
                // otherwise a previous window's target can leak dimensions (e.g. main
                // viewport adopting the color picker size).
                if (win->renderCtx && !win->renderCtx->ActivateWindowTarget())
                {
                    continue;
                }

                // Non-blocking style/layout bootstrap for windows that opted into async setup.
                PumpEditorWindowUiBootstrap(win);

                // Keep thumbnail external texture bindings in sync before we
                // build this frame's UI draw lists.
                if (win->capabilities.thumbnails && win->ui)
                {
                    if (auto* svc = dynamic_cast<ThumbnailService*>(m_ThumbnailProvider.get()))
                    {
                        svc->RegisterReadyThumbnails(win->windowId, win->ui.get());
                    }
                }

                const bool isReplayWindow = (m_UiReplay && m_UiReplay->IsEnabled() && wi == (size_t)replayWindowIndex);

                // When running a deterministic UI replay we must also make the frame delta deterministic,
                // otherwise dt-driven UI behaviors (smooth scrolling, caret blink, animations, etc.) will
                // diverge between correctness-mode and cached-mode runs.
                const double uiDeltaTime = isReplayWindow ? (1.0 / 60.0) : deltaTime;

                // Scripted UI replay injects input *before* UIManager::Update.
                if (isReplayWindow)
                {
                    m_UiReplay->TickBeforeUiUpdate(*win->ui);
                }

                // IMPORTANT:
                // Update FPS/perf overlays using the *mounted* SceneViewPanel instance for this
                // window. Layout switching can leave multiple SceneViewPanel instances in
                // m_PanelStorage; updating "the first" one makes overlays appear to update only
                // when input forces a rebuild.
                if (auto* svPanel = Editor::SceneViewRenderCoordinator::SyncMountedPanelControllers(win))
                {
                    SceneViewPanel::PerfBreakdown perf{};
                    const auto& phases = GetLastFramePhaseTimings();
                    perf.pollMs = phases.PollEventsMs;
                    perf.inputMs = phases.InputMs;
                    perf.appUpdateMs = phases.AppUpdateMs;
                    perf.engineUpdateMs = phases.EngineUpdateMs;
                    perf.beginFrameMs = m_LastSceneViewPerf.beginFrameMs;
                    perf.thumbnailsMs = m_LastSceneViewPerf.thumbnailsMs;
                    perf.uiRecordMs = m_LastSceneViewPerf.uiRecordMs;
                    perf.rgCompileMs = m_LastSceneViewPerf.rgCompileMs;
                    perf.rgExecuteMs = m_LastSceneViewPerf.rgExecuteMs;
                    perf.presentMs = m_LastSceneViewPerf.presentMs;
                    svPanel->UpdatePerformanceOverlay(uiDeltaTime, perf);
                    svPanel->UpdateFPS(static_cast<float>(uiDeltaTime));
                }

                bool interactiveUiUpdate = true;
                float uiUpdateDelta = static_cast<float>(uiDeltaTime);
                if (backgroundPolicy.enabled && !isReplayWindow && wi > 0 && win->role == WindowRole::Tool &&
                    win->floatingPanelId.empty())
                {
                    const bool focused = win->window && win->window->IsFocused();
                    if (!focused)
                    {
                        win->backgroundUiUpdateAccumulator += uiDeltaTime;
                        if (win->backgroundUiUpdateAccumulator < backgroundPolicy.minIntervalSeconds)
                        {
                            interactiveUiUpdate = false;
                            uiUpdateDelta = 0.0f;
                        }
                        else
                        {
                            win->backgroundUiUpdateAccumulator = 0.0;
                        }
                    }
                    else
                    {
                        win->backgroundUiUpdateAccumulator = 0.0;
                    }
                }
                else
                {
                    win->backgroundUiUpdateAccumulator = 0.0;
                }

                m_CssInspector.Update(win->ui.get(), win->width, win->height);
                win->ui->Update(uiUpdateDelta, interactiveUiUpdate);

                if (isReplayWindow)
                {
                    m_UiReplay->TickAfterUiUpdate(*win->ui, uiDeltaTime);
                    if (m_UiReplay->ShouldExitNow())
                    {
                        SetExitCode(m_UiReplay->ExitCode());
                        RequestExit();
                        return;
                    }
                }
            }
        }
    }

    const auto tUiWindowsEnd = std::chrono::high_resolution_clock::now();

    // Process post-UI deferred actions (e.g. opening script/nodegraph editors).
    FlushPostUiDeferredActions();

    TickPlayFullscreenFocus();

    // Session-wide runtime-input state: playing and the Game View is the active
    // tab of some window. Which window's events reach the game is decided
    // per-event by GetPlaySurface.
    bool resetFired = false;
    if (Input::InputSystem* inputSystem = GetInputSystem())
    {
        // The window whose Game View is the active tab is the one feeding the
        // game its input, so its focus is the game's.
        const EditorWindowContext* gameViewHost = nullptr;
        for (auto& win : m_Windows)
        {
            if (win && win->docking && win->docking->IsPanelActiveTab(EditorPanelIds::GameView))
            {
                gameViewHost = win.get();
                break;
            }
        }
        const bool gameViewActive = gameViewHost != nullptr;

        const bool playingOrPaused = (m_PlayMode && m_PlayMode->IsPlayingOrPaused());
        inputSystem->SetContextEnabled(EditorInput::kGameViewContext, playingOrPaused && gameViewActive);

        m_RuntimeInputActive = playingOrPaused && gameViewActive;

        // A paused game acts on nothing, so its sink claims nothing and every key
        // reaches the editor until play resumes. The sink stays wired rather than
        // being unhooked, so releases still land and no key is stranded down
        // across the pause.
        if (m_RuntimeInput)
            m_RuntimeInput->SetClaimsSuspended(m_PlayMode &&
                                               m_PlayMode->GetState() == GameEngine::Editor::PlayModeState::Paused);

        if (m_RuntimeInputActive && !m_WasRuntimeInputActive && m_RuntimeInput)
            Editor::ConnectRuntimeInput(*m_RuntimeInput,
                                        gameViewHost->window && gameViewHost->window->IsFocused());

        // Once game input switches off (play stops, or the Game View stops being
        // the active tab) the game's input system hears nothing more from the
        // window: disconnecting it releases what it holds, so no key stays stuck
        // down, and leaves the pointer outside and the window unfocused. The HUD
        // stops receiving keys on the same edge and would otherwise keep holding
        // a modifier whose release it never saw.
        resetFired = !m_RuntimeInputActive && m_WasRuntimeInputActive && m_RuntimeInput;
        if (resetFired)
        {
            Editor::DisconnectRuntimeInput(*m_RuntimeInput);
            for (auto& win : m_Windows)
                if (win && win->gameView)
                    if (GameUIHost* gameUi = win->gameView->GetGameUI())
                        gameUi->ResetKeyboardState();
        }
        m_WasRuntimeInputActive = m_RuntimeInputActive;

        // Clear action/listener registrations when play mode exits to prevent
        // stale callbacks from accumulating across play sessions.
        if (!playingOrPaused && m_WasPlayModeActive && m_RuntimeInput)
            m_RuntimeInput->ClearRegistrations();
        m_WasPlayModeActive = playingOrPaused;
    }

    // Update runtime input before gameplay scripts tick.
    // Also run on the frame ResetState() fired so synthetic key releases
    // are snapshotted into frame state for WasKeyReleased().
    if (m_RuntimeInput && (m_RuntimeInputActive || resetFired))
        m_RuntimeInput->Update(static_cast<float>(deltaTime));

    // Drive managed runtime tick (V1: editor-driven; paused state is handled inside PlayModeManager).
    if (m_PlayMode)
    {
        m_PlayMode->Tick(static_cast<float>(deltaTime));
    }
    // Recording session upkeep: play-exit stop, finalization events, view
    // ticks, and the capture fixed frame rate all live in the controller.
    if (m_MovieRecorder)
    {
        m_MovieRecorder->Tick();
    }
    // Session-active (not just capturing) so the button lights the moment
    // record is pressed, while play mode is still entering.
    if (m_TopToolbar)
        m_TopToolbar->SetRecordActive(m_MovieRecorder && m_MovieRecorder->IsSessionActive());

    // Refresh inspector fields from live component data. Tiny visual callbacks run every frame;
    // heavier synchronization is throttled internally to ~10 Hz.
    if (auto* inspectorPanel = FindFirstPanelOfType<InspectorPanel>(m_PanelStorage))
        inspectorPanel->TickSimulationRefresh();

    // Retry animation previews whose model was still loading (model loads are
    // non-blocking, so a preview can be pending until the asset is resident).
    if (ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld())
        Editor::AnimationPreviewManager::TickPending(*world);

    // Drive Scene View camera updates from each window's mounted SceneView panel.
    {
        Input::InputSystem* inputSystem = GetInputSystem();
        SceneViewController* firstSceneController = nullptr;
        SceneViewController* activeInputSceneController = nullptr;
        bool consumedSceneInput = false;

        for (auto& win : m_Windows)
        {
            if (!win || !win->scene)
            {
                continue;
            }

            SceneViewPanel* svPanel =
                Editor::SceneViewRenderCoordinator::SyncMountedPanelControllers(win.get());
            if (!firstSceneController)
            {
                firstSceneController = svPanel
                    ? svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot())
                    : win->scene.get();
                if (!firstSceneController)
                    firstSceneController = win->scene.get();
            }

            bool looking = false;
            bool orbiting = false;
            bool panning = false;
            bool dollying = false;

            bool moveF = false;
            bool moveB = false;
            bool moveL = false;
            bool moveR = false;
            bool moveU = false;
            bool moveD = false;
            bool moveFaster = false;

            float lookYawDelta = 0.0f;
            float lookPitchDelta = 0.0f;
            float dollyDeltaY = 0.0f;

            if (svPanel)
            {
                GE_CPU_PROFILE_SCOPE("EditorApplication.Update.SceneView");

                SceneViewController* panelSceneController =
                    svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot());
                if (!panelSceneController)
                    panelSceneController = win->scene.get();

                looking = svPanel->IsLooking();
                orbiting = svPanel->IsOrbiting();
                panning = svPanel->IsPanning();
                dollying = svPanel->IsDollying();
                const bool interacting = looking || orbiting || panning || dollying;

                if (!activeInputSceneController && interacting)
                {
                    activeInputSceneController = panelSceneController;
                }

                // Only one mounted SceneView panel should consume frame input deltas.
                if (interacting && inputSystem && !consumedSceneInput)
                {
                    auto yawState = inputSystem->GetActionState(EditorInput::kSceneViewLookYaw);
                    auto pitchState = inputSystem->GetActionState(EditorInput::kSceneViewLookPitch);

                    const float kSens = Editor::SceneViewSettings::Get().GetLookSensitivity();
                    const bool kInvertY = Editor::SceneViewSettings::Get().GetInvertY();

                    if (looking || orbiting)
                    {
                        lookYawDelta = yawState.value * -kSens;
                        lookPitchDelta = pitchState.value * (kInvertY ? -kSens : kSens);
                    }

                    if (dollying)
                    {
                        // Dolly: horizontal mouse delta for zoom.
                        dollyDeltaY = -yawState.value;
                    }

                    if (looking)
                    {
                        moveF = inputSystem->IsActionActive(EditorInput::kSceneViewMoveForward);
                        moveB = inputSystem->IsActionActive(EditorInput::kSceneViewMoveBackward);
                        moveL = inputSystem->IsActionActive(EditorInput::kSceneViewMoveLeft);
                        moveR = inputSystem->IsActionActive(EditorInput::kSceneViewMoveRight);
                        moveU = inputSystem->IsActionActive(EditorInput::kSceneViewMoveUp);
                        moveD = inputSystem->IsActionActive(EditorInput::kSceneViewMoveDown);
                        moveFaster = inputSystem->IsActionActive(EditorInput::kSceneViewMoveFaster);
                    }

                    consumedSceneInput = true;
                }

                // Skip look delta on the first orbit frame to avoid orbit pivot drift.
                const bool firstOrbitFrame = svPanel->IsFirstOrbitFrame();
                if ((looking || orbiting) && (lookYawDelta != 0.0f || lookPitchDelta != 0.0f) && !firstOrbitFrame)
                {
                    svPanel->ApplyLookDelta(lookYawDelta, lookPitchDelta);
                }
            }

            float speedMultiplier = 1.0f;
            {
                using GameEngine::Editor::SceneViewSettings;
                const float fastMult = SceneViewSettings::Get().GetFastMoveMultiplier();
                speedMultiplier = moveFaster ? fastMult : 1.0f;
            }
            // Update cached viewport dimensions BEFORE UpdateCamera so the
            // frustum culling projection uses the current layout size, not
            // the previous frame's. Without this, resizing the scene view
            // panel causes entities to be culled against a stale frustum for
            // one frame, producing a visible clipping line.
            if (svPanel)
            {
                uint32_t curW = 1, curH = 1;
                SceneViewController* panelSceneController =
                    svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot());
                if (!panelSceneController)
                    panelSceneController = win->scene.get();
                UIElement* panelViewport =
                    svPanel->GetViewportElementForSlot(svPanel->GetActiveViewportSlot());
                if (!panelViewport)
                    panelViewport = svPanel->GetViewportElement();

                Editor::SceneViewRenderCoordinator::TryResolveRenderableViewportSize(
                    win.get(), panelViewport, curW, curH);
                panelSceneController->SetLastViewportSize(curW, curH);

                // Push panel yaw/pitch and apply orbit/pan/dolly BEFORE UpdateCamera
                // so that m_CamPos and angles are fully settled before any viewProj is
                // built. This ensures the world pass VP and the grid's invVP (recomputed
                // in Record) see the exact same camera state — preventing jitter.
                panelSceneController->SetCameraAnglesDeg(svPanel->GetYawDeg(), svPanel->GetPitchDeg());
                panelSceneController->UpdateOrbit(svPanel->IsOrbiting());

                if (dollying && dollyDeltaY != 0.0f)
                {
                    panelSceneController->UpdateDolly(dollyDeltaY);
                }
                panelSceneController->Update(static_cast<float>(deltaTime), moveF, moveB, moveL, moveR, moveU, moveD,
                                             speedMultiplier);
                panelSceneController->UpdateCamera(static_cast<float>(deltaTime), moveF, moveB, moveL, moveR, moveU,
                                                   moveD, speedMultiplier);
                if (auto* rs = panelSceneController->GetRenderServices())
                    rs->Views().SetViewpointCamera(panelSceneController->GetCameraId());

                // Keep panel yaw/pitch in sync with controller (tweens update angles internally).
                const auto pose = panelSceneController->GetCameraPose();
                svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
            }
            else
            {
                win->scene->Update(static_cast<float>(deltaTime), moveF, moveB, moveL, moveR, moveU, moveD,
                                   speedMultiplier);
                win->scene->UpdateCamera(static_cast<float>(deltaTime), moveF, moveB, moveL, moveR, moveU, moveD,
                                         speedMultiplier);
                if (auto* rs = win->scene->GetRenderServices())
                    rs->Views().SetViewpointCamera(win->scene->GetCameraId());
            }
        }

        if (inputSystem)
        {
            using namespace EditorInput;

            if (Editor::AllowShortcutActionsThisFrame())
            {
            if (inputSystem->WasActionTriggered(kEditorUniversalSearch) && m_UniversalSearch)
                m_UniversalSearch->Toggle();

            // Cmd/Ctrl+P: toggle play mode (global, works even without Scene View focus).
            if (inputSystem->WasActionTriggered(kEditorTogglePlayMode) && m_PlayMode)
            {
                const auto state = m_PlayMode->GetState();
                if (state == GameEngine::Editor::PlayModeState::Edit)
                {
                    auto doPlay = [this]() {
                        if (CanEnterPlayFullscreen())
                            RequestEnterPlayFullscreenAndPlay();
                        else
                            m_PlayMode->EnterPlayMode();
                    };

                    if (m_DownloadManager && m_DownloadManager->HasActiveDownloads() && m_DownloadModal)
                    {
                        m_DownloadModal->Show("Play Mode", m_DownloadManager.get(), std::move(doPlay));
                    }
                    else
                    {
                        doPlay();
                    }
                }
                else if (state == GameEngine::Editor::PlayModeState::ChangeReview)
                {
                    m_PlayMode->DiscardPendingChanges();
                }
                else if (state != GameEngine::Editor::PlayModeState::Edit)
                {
                    m_PlayMode->ExitPlayMode();
                    ExitPlayFullscreen();
                    if (m_TopToolbar)
                        m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
                }
            }

            // Escape → Change Review discard only. Leave play with Ctrl/Cmd+P,
            // the toolbar Stop button, or Escape while play is fullscreen.
            // Only floating windows reach this: the main window's onKeyPre
            // intercepts Escape in Change Review before routing. Known
            // limitation of that asymmetry: in a floating window a focused
            // TextField consumes Escape first (TextInput::CancelEditing), so
            // discard needs the field blurred there.
            if (inputSystem->WasActionTriggered(kEditorDiscardPlayReview) && m_PlayMode)
            {
                if (m_PlayMode->GetState() == GameEngine::Editor::PlayModeState::ChangeReview)
                    m_PlayMode->DiscardPendingChanges();
            }

            SceneViewController* controller =
                activeInputSceneController ? activeInputSceneController : firstSceneController;
            if (controller)
            {
                if (inputSystem->WasActionTriggered(kEditorToggleGizmos))
                {
                    controller->ToggleGizmos();
                }

                // Suppress frame commands while camera is actively controlled
                // (RMB look, orbit, pan, dolly) to avoid conflicts with WASD keys.
                if (!activeInputSceneController)
                {
                    if (inputSystem->WasActionTriggered(kEditorFrameSelection))
                    {
                        controller->FrameOrigin();
                    }

                    if (inputSystem->WasActionTriggered(kEditorFrameAll))
                    {
                        controller->FrameAll();
                    }
                }

                // Cmd/Ctrl+F: toggle editor window fullscreen without entering Play Mode.
                if (inputSystem->WasActionTriggered(kEditorToggleWindowFullscreen))
                {
                    if ((!m_PlayMode || m_PlayMode->GetState() == GameEngine::Editor::PlayModeState::Edit) &&
                        !m_Windows.empty())
                        DeferToggleWindowFullscreen();
                }
            }
            } // AllowShortcutActionsThisFrame
        }
    }

    const auto tAppUpdate1 = std::chrono::high_resolution_clock::now();
    auto dMs = [](std::chrono::high_resolution_clock::time_point a, std::chrono::high_resolution_clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    m_LastAppUpdateSubPhases.preUiMs       = dMs(tAppUpdate0, tPreUiEnd);
    m_LastAppUpdateSubPhases.debugPanelsMs = dMs(tPreUiEnd, tDebugPanelsEnd);
    m_LastAppUpdateSubPhases.uiWindowsMs   = dMs(tDebugPanelsEnd, tUiWindowsEnd);
    m_LastAppUpdateSubPhases.tailMs        = dMs(tUiWindowsEnd, tAppUpdate1);
}

bool EditorApplication::TryInterceptQuitForDirtyScene()
{
    const bool sceneDirty   = m_SceneEditor && m_SceneEditor->IsSceneDirty();
    std::vector<GraphPanel*> dirtyGraphs;
    GraphPanel::ForEachLive([&dirtyGraphs](GraphPanel& panel) {
        if (panel.HasUnsavedChanges())
            dirtyGraphs.push_back(&panel);
    });
    ScriptEditorPanel* sePanel = FindFirstPanelOfType<ScriptEditorPanel>(m_PanelStorage);
    const bool graphDirty  = !dirtyGraphs.empty();
    const bool scriptDirty = sePanel && sePanel->HasUnsavedChanges();

    if (!sceneDirty && !graphDirty && !scriptDirty)
        return false;

    auto focusMainEditorWindow = [this]()
    {
        if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->window)
            m_Windows[0]->window->Focus();
    };

    if (m_SceneEditor && m_SceneEditor->IsAnyModalOpen())
    {
        focusMainEditorWindow();
        return true;
    }

    focusMainEditorWindow();

    /* Build a chain: scene → node graph → script editor → quit.
       Each step calls the next only when the user saves or discards. */
    auto doQuit = [this]() { RequestExit(); };

    auto checkScript = [this, sePanel, scriptDirty, doQuit]()
    {
        if (scriptDirty && sePanel)
            sePanel->PromptSaveBeforeQuit(doQuit);
        else
            doQuit();
    };

    std::function<void()> checkGraph = checkScript;
    for (auto it = dirtyGraphs.rbegin(); it != dirtyGraphs.rend(); ++it)
    {
        GraphPanel* panel = *it;
        auto next = checkGraph;
        checkGraph = [panel, next]() { panel->PromptSaveBeforeQuit(next); };
    }

    if (sceneDirty && m_SceneEditor)
        m_SceneEditor->PromptSaveBeforeQuit(checkGraph);
    else
        checkGraph();

    return true;
}

bool EditorApplication::InvokeUiReplayCommand(std::uint32_t commandId, std::string* outError)
{
    if (commandId == 0u)
    {
        if (outError)
            *outError = "commandId=0";
        return false;
    }

    // Native package menu items (EditorMenuRegistry, 0x6000-0x6FFF — e.g.
    // Tools > Import Unity Package...): exposed here so UI-replay /
    // execute_command can drive them by command id.
    if (Editor::EditorMenuRegistry::Get().TryInvoke(commandId))
        return true;

    // Fixed-id commands their owners registered (e.g. the asset pipeline's
    // Reimport LODs and Bake HLOD).
    switch (Editor::EditorMenuRegistry::Get().InvokeCommand(commandId, outError))
    {
    case Editor::EditorCommandResult::Succeeded:
        return true;
    case Editor::EditorCommandResult::Failed:
        return false;
    case Editor::EditorCommandResult::NotFound:
        break;
    }

    if (commandId == UiReplayCommandIds::SaveScene)
    {
        if (!m_SceneEditor)
        {
            if (outError)
                *outError = "scene editor unavailable";
            return false;
        }
        return m_SceneEditor->SaveActiveScene(outError);
    }

    const auto playModeStateName = [](Editor::PlayModeState state) -> const char*
    {
        using State = Editor::PlayModeState;
        switch (state)
        {
        case State::Edit:
            return "Edit";
        case State::EnteringPlay:
            return "EnteringPlay";
        case State::Play:
            return "Play";
        case State::Paused:
            return "Paused";
        case State::ExitingPlay:
            return "ExitingPlay";
        case State::ChangeReview:
            return "ChangeReview";
        default:
            return "Unknown";
        }
    };

    if (commandId == UiReplayCommandIds::UndockHierarchy || commandId == UiReplayCommandIds::UndockSceneView ||
        commandId == UiReplayCommandIds::UndockGameView || commandId == UiReplayCommandIds::UndockInspector ||
        commandId == UiReplayCommandIds::UndockNodeGraph || commandId == UiReplayCommandIds::UndockAssets)
    {
        if (!m_Docking)
        {
            if (outError)
                *outError = "docking unavailable";
            return false;
        }

        const char* panelId = EditorPanelIds::Hierarchy;
        if (commandId == UiReplayCommandIds::UndockSceneView)
            panelId = EditorPanelIds::SceneView;
        else if (commandId == UiReplayCommandIds::UndockGameView)
            panelId = EditorPanelIds::GameView;
        else if (commandId == UiReplayCommandIds::UndockInspector)
            panelId = EditorPanelIds::Inspector;
        else if (commandId == UiReplayCommandIds::UndockNodeGraph)
            panelId = EditorPanelIds::NodeGraph;
        else if (commandId == UiReplayCommandIds::UndockAssets)
            panelId = EditorPanelIds::Assets;

        this->QueueUndockPanel(panelId);
        return true;
    }

    if (commandId == UiReplayCommandIds::CloseLastFloatingWindow)
    {
        if (m_Windows.size() <= 1)
        {
            if (outError)
                *outError = "no floating windows";
            return false;
        }

        for (size_t i = m_Windows.size(); i-- > 1;)
        {
            EditorWindowContext* win = m_Windows[i].get();
            if (win && win->window)
            {
                win->window->RequestClose();
                return true;
            }
        }

        if (outError)
            *outError = "no closeable floating window";
        return false;
    }

    if (commandId == UiReplayCommandIds::ReattachLastFloatingWindow)
    {
        if (m_Windows.size() <= 1)
        {
            if (outError)
                *outError = "no floating windows";
            return false;
        }
        EditorWindowContext* source = nullptr;
        for (size_t i = m_Windows.size(); i-- > 1;)
        {
            EditorWindowContext* win = m_Windows[i].get();
            if (win && win->window && !win->floatingPanelId.empty())
            {
                source = win;
                break;
            }
        }
        EditorWindowContext* target = m_Windows.empty() ? nullptr : m_Windows[0].get();
        if (!source || !target)
        {
            if (outError)
                *outError = "source/target window unavailable";
            return false;
        }

        Editor::WindowDockingController::DropCandidate cand{};
        cand.source = source;
        cand.target = target;
        cand.kind = Editor::WindowDockingController::DropKind::None;
        cand.edge = DockPosition::Center;
        m_DockDnd->PerformDockDrop(cand);
        return true;
    }

    if (commandId == UiReplayCommandIds::PlayEnter)
    {
        if (!m_PlayMode)
        {
            if (outError)
                *outError = "play mode unavailable";
            return false;
        }

        const Editor::PlayModeState state = m_PlayMode->GetState();
        if (state != Editor::PlayModeState::Edit)
        {
            if (outError)
                *outError = std::string("expected Edit before PlayEnter, got ") + playModeStateName(state);
            return false;
        }
        m_PlayMode->EnterPlayMode();
        return true;
    }

    if (commandId == UiReplayCommandIds::PlayTogglePause)
    {
        if (!m_PlayMode)
        {
            if (outError)
                *outError = "play mode unavailable";
            return false;
        }
        if (!m_PlayMode->IsPlayingOrPaused())
        {
            if (outError)
                *outError = "PlayTogglePause requires Play or Paused state";
            return false;
        }
        m_PlayMode->TogglePause();
        return true;
    }

    if (commandId == UiReplayCommandIds::PlayStop)
    {
        if (!m_PlayMode)
        {
            if (outError)
                *outError = "play mode unavailable";
            return false;
        }
        const Editor::PlayModeState state = m_PlayMode->GetState();
        if (state == Editor::PlayModeState::ChangeReview)
        {
            m_PlayMode->DiscardPendingChanges();
            return true;
        }
        if (state == Editor::PlayModeState::Edit)
            return true;

        m_PlayMode->ExitPlayMode();
        ExitPlayFullscreen();
        if (m_TopToolbar)
            m_TopToolbar->SetFullscreenActive(m_PlayFullscreenOnEnter);
        return true;
    }

    if (commandId == UiReplayCommandIds::AssertPlayModeEdit || commandId == UiReplayCommandIds::AssertPlayModePlay ||
        commandId == UiReplayCommandIds::AssertPlayModePaused ||
        commandId == UiReplayCommandIds::AssertPlayModeNotRunning)
    {
        if (!m_PlayMode)
        {
            if (outError)
                *outError = "play mode unavailable";
            return false;
        }

        const Editor::PlayModeState state = m_PlayMode->GetState();
        bool ok = false;
        const char* expected = "unknown";

        if (commandId == UiReplayCommandIds::AssertPlayModeEdit)
        {
            expected = "Edit";
            ok = (state == Editor::PlayModeState::Edit);
        }
        else if (commandId == UiReplayCommandIds::AssertPlayModePlay)
        {
            expected = "Play";
            ok = (state == Editor::PlayModeState::Play);
        }
        else if (commandId == UiReplayCommandIds::AssertPlayModePaused)
        {
            expected = "Paused";
            ok = (state == Editor::PlayModeState::Paused);
        }
        else
        {
            expected = "not-running (Edit or ChangeReview)";
            ok = (state == Editor::PlayModeState::Edit || state == Editor::PlayModeState::ChangeReview);
        }

        if (!ok)
        {
            if (outError)
                *outError = std::string("expected ") + expected + ", got " + playModeStateName(state);
            return false;
        }
        return true;
    }

    if (commandId == UiReplayCommandIds::AssertGameViewNoCameraOverlayVisible)
    {
        if (auto* gameViewPanel = FindFirstPanelOfType<GameViewPanel>(m_PanelStorage))
        {
            if (gameViewPanel->IsNoCameraOverlayVisible())
                return true;
            if (outError)
                *outError = "game view no-camera overlay is not visible";
            return false;
        }

        if (outError)
            *outError = "game view panel unavailable";
        return false;
    }

    if (commandId == UiReplayCommandIds::AssertFloatingHierarchyWindow ||
        commandId == UiReplayCommandIds::AssertFloatingSceneViewWindow ||
        commandId == UiReplayCommandIds::AssertFloatingGameViewWindow ||
        commandId == UiReplayCommandIds::AssertFloatingInspectorWindow ||
        commandId == UiReplayCommandIds::AssertFloatingAssetsWindow)
    {
        const char* expectedPanelId = EditorPanelIds::Hierarchy;
        if (commandId == UiReplayCommandIds::AssertFloatingSceneViewWindow)
            expectedPanelId = EditorPanelIds::SceneView;
        else if (commandId == UiReplayCommandIds::AssertFloatingGameViewWindow)
            expectedPanelId = EditorPanelIds::GameView;
        else if (commandId == UiReplayCommandIds::AssertFloatingInspectorWindow)
            expectedPanelId = EditorPanelIds::Inspector;
        else if (commandId == UiReplayCommandIds::AssertFloatingAssetsWindow)
            expectedPanelId = EditorPanelIds::Assets;

        auto subtreeHasClass = [](UIElement* root, const char* className) -> bool
        {
            if (!root || !className || !*className)
                return false;
            std::vector<UIElement*> stack;
            stack.push_back(root);
            while (!stack.empty())
            {
                UIElement* cur = stack.back();
                stack.pop_back();
                if (!cur)
                    continue;
                if (cur->HasClass(className))
                    return true;
                for (const auto& ch : cur->GetChildren())
                {
                    stack.push_back(ch.get());
                }
                if (auto* m = dynamic_cast<Mount*>(cur))
                {
                    if (UIElement* target = m->GetTarget())
                        stack.push_back(target);
                }
            }
            return false;
        };

        auto floatingContainsPanel = [&](const EditorWindowContext* win, const char* panelId, std::string* outDetail) -> bool
        {
            if (!win || !win->docking || !panelId)
            {
                if (outDetail)
                    *outDetail = "missing window/docking/panel id";
                return false;
            }
            const DockNode* root = win->docking->GetRoot();
            if (!root || !root->IsLeaf())
            {
                if (outDetail)
                    *outDetail = "floating dock root is not a leaf";
                return false;
            }
            const auto& tabs = root->GetTabs();
            if (std::find(tabs.begin(), tabs.end(), panelId) == tabs.end())
            {
                if (outDetail)
                    *outDetail = "floating dock leaf does not include panel tab";
                return false;
            }

            if (!win->ui)
            {
                if (outDetail)
                    *outDetail = "floating window has no UI manager";
                return false;
            }
            UIElement* rootEl = win->ui->GetRootElement();
            if (!rootEl)
            {
                if (outDetail)
                    *outDetail = "floating window has no UI root";
                return false;
            }
            const std::string mountId = std::string("mount:") + panelId;
            auto* mount = dynamic_cast<Mount*>(rootEl->FindById(mountId));
            if (!mount || !mount->GetTarget())
            {
                if (outDetail)
                    *outDetail = std::string("missing mount target '") + mountId + "'";
                return false;
            }
            UIElement* panelRoot = mount->GetTarget();
            if (panelRoot->GetOwnerManager() != win->ui.get())
            {
                if (outDetail)
                    *outDetail = "mounted panel owner manager mismatch";
                return false;
            }

            if (std::strcmp(panelId, EditorPanelIds::Hierarchy) == 0)
            {
                if (!subtreeHasClass(panelRoot, "hierarchy-panel"))
                {
                    if (outDetail)
                        *outDetail = "hierarchy panel class missing";
                    return false;
                }
                if (!subtreeHasClass(panelRoot, "tree"))
                {
                    if (outDetail)
                        *outDetail = "hierarchy tree class missing";
                    return false;
                }
            }
            else if (std::strcmp(panelId, EditorPanelIds::Inspector) == 0)
            {
                if (!subtreeHasClass(panelRoot, "inspector-panel"))
                {
                    if (outDetail)
                        *outDetail = "inspector panel class missing";
                    return false;
                }
                if (!subtreeHasClass(panelRoot, "inspector-scrollview"))
                {
                    if (outDetail)
                        *outDetail = "inspector scrollview class missing";
                    return false;
                }
            }
            else if (std::strcmp(panelId, EditorPanelIds::Assets) == 0)
            {
                if (!panelRoot->FindById("assets-tree"))
                {
                    if (outDetail)
                        *outDetail = "assets tree element missing";
                    return false;
                }
                if (!panelRoot->FindById("assets-views-container"))
                {
                    if (outDetail)
                        *outDetail = "assets views container missing";
                    return false;
                }
            }

            return true;
        };

        for (size_t i = 1; i < m_Windows.size(); ++i)
        {
            const EditorWindowContext* win = m_Windows[i].get();
            if (!win || !win->window)
                continue;
            std::string detail;
            if (win->floatingPanelId == expectedPanelId && floatingContainsPanel(win, expectedPanelId, &detail))
                return true;
        }

        if (outError)
            *outError = std::string("expected floating window hosting panel '") + expectedPanelId + "' with mounted content";
        return false;
    }

    // Prefer built-in Hierarchy commands (create primitives, etc.).
    if (auto* hierarchyPanel = FindFirstPanelOfType<HierarchyPanel>(m_PanelStorage))
    {
        if (hierarchyPanel->HandleCommand(commandId))
        {
            return true;
        }
    }

    // Assets UIReplay automation commands (sandbox navigation, etc.)
    if (auto* assetsPanel = FindFirstPanelOfType<AssetsPanel>(m_PanelStorage))
    {
        std::string tmpErr;
        std::string* err = outError ? outError : &tmpErr;
        if (err)
            err->clear();

        if (assetsPanel->HandleUiReplayCommand(commandId, err))
        {
            // Treat "handled but errored" as a hard failure so UIReplay doesn't silently
            // keep running and fail later with unrelated timeouts.
            if (err && !err->empty())
                return false;
            return true;
        }
    }

    // Fallback: script-provided commands.
    std::uint64_t dom = 0;
    std::string method;
    if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(commandId, dom, method) && !method.empty())
    {
        // A hot-reload since registration retires the domain the command was
        // bound to; dispatching into an unloaded ALC is a synchronous managed
        // call that can hang the main thread. Reject the stale binding with
        // context instead of wedging the editor.
        const std::uint64_t currentDom = Editor::ScriptMenuRegistry::Get().GetCurrentDomainId();
        if (dom != currentDom)
        {
            if (outError)
                *outError = "script command " + std::to_string(commandId)
                          + " is bound to unloaded script domain " + std::to_string(dom)
                          + " (current " + std::to_string(currentDom)
                          + ") — stale after hot-reload; re-resolve the command";
            return false;
        }
        try
        {
            auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
            std::int32_t out = 0;
            (void)clr.InvokeInDomain(dom, method.c_str(), (std::uint32_t)method.size(), &out);
            return true;
        }
        catch (...)
        {
            if (outError)
                *outError = "script command threw";
            return false;
        }
    }

    if (outError)
        *outError = "unhandled commandId";
    return false;
}

bool EditorApplication::ResizeUiReplayWindow(std::uint32_t windowIndex,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             std::string* outError)
{
    if (width == 0u || height == 0u)
    {
        if (outError)
            *outError = "invalid size";
        return false;
    }
    if (windowIndex >= m_Windows.size() || !m_Windows[windowIndex] || !m_Windows[windowIndex]->window)
    {
        if (outError)
            *outError = "invalid windowIndex";
        return false;
    }

    // Best-effort resize. The framebuffer-size callback (OnFramebufferResized) drives swapchain recreation.
    if (!m_Windows[windowIndex]->window->GetGLFWHandle())
    {
        if (outError)
            *outError = "null GLFW handle";
        return false;
    }
    m_Windows[windowIndex]->window->SetWindowSize(static_cast<int>(width), static_cast<int>(height));
    return true;
}

std::string EditorApplication::BuildPerfDumpLine() const
{
    const double dt = GetDeltaTime();
    const double dtSafe = (dt > 1e-9) ? dt : 1e-9;
    const double ms = dtSafe * 1000.0;
    const double fps = 1.0 / dtSafe;

    const auto& phases = GetLastFramePhaseTimings();
    const auto& sp = m_LastSceneViewPerf;

    char buf[320];
    std::snprintf(
        buf,
        sizeof(buf),
        "[PerfDump] %.1f ms (%.0f fps) | poll %.2f in %.2f | upd %.2f+%.2f | ren %.2f sl %.2f | bf %.2f th %.2f ui %.2f rg %.2f+%.2f pr %.2f",
        ms, fps,
        phases.PollEventsMs,
        phases.InputMs,
        phases.AppUpdateMs,
        phases.EngineUpdateMs,
        phases.RenderMs,
        phases.SleepMs,
        sp.beginFrameMs,
        sp.thumbnailsMs,
        sp.uiRecordMs,
        sp.rgCompileMs,
        sp.rgExecuteMs,
        sp.presentMs);
    return std::string(buf);
}

void EditorApplication::Render()
{
    if (m_Windows.empty())
        return;

    if (m_HdrOutput->ShouldSuppressRender())
        return;

    // Time the cross-window/floating-window/drag-drop pre-loop work so we
    // can attribute it (it sits inside Render() but outside the per-window
    // BeginFrame/UI/RG/Present sub-phase timers). m_LastRenderPreloopMs
    // is exposed via the debug server for frame-cost diagnostics.
    const auto _tPreloop0 = std::chrono::high_resolution_clock::now();

    m_DockDnd->Tick();

    // Collect floating windows that requested close
    std::vector<uint64_t> toCloseWindowIds;

    // Best-effort per-frame perf breakdown for the primary Scene View window.
    bool capturedScenePerf = false;
    FramePerfBreakdown scenePerf{};
    auto MsBetween = [](const auto& a, const auto& b) -> double
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    {
        const auto _tPreloop1 = std::chrono::high_resolution_clock::now();
        m_LastRenderPreloopMs = std::chrono::duration<double, std::milli>(_tPreloop1 - _tPreloop0).count();
    }

    if (auto* thumbSvc = dynamic_cast<ThumbnailService*>(m_ThumbnailProvider.get()))
    {
        thumbSvc->BeginFrame();
    }

    for (size_t i = 0; i < m_Windows.size(); ++i)
    {
        // Use a raw pointer to the context to avoid invalidation if m_Windows reallocates during UI->Render()
        EditorWindowContext* ctx = m_Windows[i].get();
        Rendering::IDevice* dev = (ctx && ctx->renderCtx) ? ctx->renderCtx->GetDevice() : nullptr;
        if (!ctx || !ctx->window || !dev || !ctx->ui)
        {
            continue;
        }

        // Q6: drive the device-loss rebuild retry from this unconditional per-tick
        // poll — BEFORE the render-skip gates below. A failed rebuild suppresses
        // rendering (BeginWindowRenderFrame returns false), so a retry driven off the
        // render/submit path would never fire again; the timed-backoff schedule inside
        // also lets a real-TDR adapter reset settle between attempts.
        dev->TickDeviceRecovery();

        // Q6 slice 6: surface device health natively (the GPU-rendered editor UI is
        // frozen while unhealthy). Poll only the primary window's device — the
        // save-and-restart dialog is process-global. The poll returns promptly: the
        // terminal modal is presented on a worker thread, because blocking this thread
        // would stop the frame loop, input, autosave and the debug-server IPC drain.
        if (i == 0)
            m_DeviceLossSurfacer.Poll(dev->GetDeviceHealth(), ctx->window.get(),
                                      m_DeviceLossActions);

        // Commit dock-on-drop on mouse release (global)
        m_DockDnd->TickDropCommitOnRelease();

        if (ctx->window->ShouldClose())
        {
            if (i == 0)
            {
                // Main window closed: exit the app
                RequestExit();
                return;
            }
            else
            {
                toCloseWindowIds.push_back(ctx->windowId);
                continue;
            }
        }

        // Ensure framebuffer is valid before rendering; resize handling is driven by
        // Platform::Window framebuffer-size callback (OnFramebufferResized).
        int fbW = 0, fbH = 0;
        ctx->window->GetFramebufferSize(fbW, fbH);
        if (fbW <= 0 || fbH <= 0)
        {
            continue; // skip rendering until valid size
        }

        // When Web panel is not the active tab, move its web view off-screen so it doesn't draw on top of other panels.
        // Native WKWebView/WebView2 is a subview above GPU UI, so also hide it while any modal is visible
        // (or fading out) — otherwise the modal renders underneath.
        // We keep the view "alive" (not hidden) so the page and audio keep playing in the background.
        if (ctx->docking)
        {
            if (auto* webPanel = FindFirstPanelOfType<WebPanel>(m_PanelStorage))
            {
                const bool webTabInactive = !ctx->docking->IsPanelActiveTab(EditorPanelIds::Web);
                const bool projectPickerBlocking =
                    m_ProjectFolderModal != nullptr && m_ProjectFolderModal->ShouldSuppressNativeWebView();
                const bool sceneModalBlocking =
                    m_SceneEditor && m_SceneEditor->IsAnyModalOpen();
                const bool videoModalBlocking = m_VideoPlayer.IsOpen();
                webPanel->SetWebViewOffScreen(webTabInactive || projectPickerBlocking
                    || sceneModalBlocking || videoModalBlocking);
            }
        }

        // Keep debug zones up-to-date even when not hovering/dragging
        if (DockOverlay::IsDebugZonesEnabled())
        {
            if (UIElement* root = ctx->ui->GetRootElement())
            {
                if (auto* dsEl = dynamic_cast<DockspaceElement*>(root->FindById("dock")))
                {
                    DockDropTarget dt;
                    dt.Kind = DockDropTarget::TargetKind::None; // debug only
                    dsEl->SetDropPreview(dt, dsEl->GetLayoutWidth(), dsEl->GetLayoutHeight());
                }
            }
        }

        const bool capturePerfThisWindow = (!capturedScenePerf && ctx->scene != nullptr);

        if (ctx->skipRenderFramesAfterHdrSwitch > 0)
        {
            --ctx->skipRenderFramesAfterHdrSwitch;
            continue;
        }

        const auto tBeginFrame0 = std::chrono::high_resolution_clock::now();
        const bool beginOk = BeginWindowRenderFrame(ctx, dev);
        const auto tBeginFrame1 = std::chrono::high_resolution_clock::now();
        if (capturePerfThisWindow)
        {
            scenePerf.beginFrameMs = MsBetween(tBeginFrame0, tBeginFrame1);
        }
        if (!beginOk)
        {
            continue;
        }

        auto frameResult = RenderWindowFrame(ctx, dev, /*passiveUi=*/false);

        // Show the main window after the startup Scene View has survived a small
        // warm-up. The first presented frame can still contain a dark placeholder
        // while render targets and image-backed UI finish settling, so keep the
        // hidden window hidden until two Scene View-ready presents have completed.
        if (m_PendingWindowShow && ctx == m_Windows[0].get())
        {
            ++ctx->startupPresentedFrames;
            if (frameResult.sceneViewportReady)
                ++ctx->startupSceneReadyPresentedFrames;
            else
                ctx->startupSceneReadyPresentedFrames = 0;

            constexpr uint32_t kStartupVisibleSceneReadyPresents = 2;
            constexpr uint32_t kStartupVisibleFallbackPresents = 5;
            const bool sceneWarm = ctx->startupSceneReadyPresentedFrames >= kStartupVisibleSceneReadyPresents;
            const bool fallbackWarm = ctx->startupPresentedFrames >= kStartupVisibleFallbackPresents;
            if ((sceneWarm || fallbackWarm) && ctx->window)
            {
                m_PendingWindowShow = false;
                ctx->window->Show();
            }
        }

        if (capturePerfThisWindow)
        {
            scenePerf.worldViewsMs    = frameResult.worldViewsMs;
            scenePerf.thumbnailsMs    = frameResult.thumbnailsMs;
            scenePerf.uiRecordMs      = frameResult.uiRecordMs;
            scenePerf.terminalPassesMs = frameResult.terminalPassesMs;
            scenePerf.rgCompileMs     = frameResult.rgCompileMs;
            scenePerf.rgExecuteMs     = frameResult.rgExecuteMs;
            scenePerf.variantCompileMs = frameResult.variantCompileMs;
            scenePerf.prewarmMs       = frameResult.prewarmMs;
            scenePerf.presentMs       = frameResult.presentMs;
            // Include Application-level phase timings so the MCP debug handler
            // can report the full frame cost (not just the rendering phases).
            const auto& phases = GetLastFramePhaseTimings();
            scenePerf.pollMs       = phases.PollEventsMs;
            scenePerf.inputMs      = phases.InputMs;
            scenePerf.appUpdateMs  = phases.AppUpdateMs;
            scenePerf.engineUpdateMs = phases.EngineUpdateMs;
            m_LastSceneViewPerf    = scenePerf;
            capturedScenePerf      = true;
        }
    }

    // macOS: show windows that were created hidden (StartHidden) now that all
    // windows have finished rendering — avoids a gray/empty flash.
#if defined(__APPLE__)
    for (size_t i = 0; i < m_Windows.size(); ++i)
    {
        auto* ctx = m_Windows[i].get();
        if (ctx && ctx->pendingShow && ctx->window)
        {
            ctx->window->Show();
            ctx->pendingShow = false;
        }
    }
#endif

    // Tear down any floating windows that were closed
    if (!toCloseWindowIds.empty())
    {
        for (uint64_t windowId : toCloseWindowIds)
        {
            size_t idx = m_Windows.size();
            for (size_t wi = 1; wi < m_Windows.size(); ++wi)
            {
                if (m_Windows[wi] && m_Windows[wi]->windowId == windowId)
                {
                    idx = wi;
                    break;
                }
            }
            if (idx >= m_Windows.size() || idx == 0)
            {
                continue;
            }

            auto& win = m_Windows[idx];
            // Drop tear-off tracking and any drag-state pointers into this window
            if (win)
                m_DockDnd->OnFloatingWindowClosing(win.get());
            // Fire cancel callback before cleanup so the opener can revert
            // (e.g. restore original light/material color when closing via X button).
            if (auto* cpCtx = ColorPickerEditorIntegration::GetContext(win.get()))
            {
                if (cpCtx->onCancel)
                    cpCtx->onCancel();
            }
            ColorPickerEditorIntegration::Cleanup(win.get());

            // Drawn menus CloseAll() through this window's UIManager. Release
            // before ui.reset() — Unregister runs from ~EditorWindowContext,
            // which is after that, and is compiled out on Win32/macOS.
            if (win && win->window)
                ReleaseShowingContextMenuForWindow(win->window.get());

            // Destroy UI first, then rendering, then the platform window (order matters)
            if (win && win->ui)
            {
                win->ui.reset();
            }
            // Scene controller owns RenderServices-backed view/camera resources.
            // Destroy it BEFORE shutting down the per-window RenderDeviceContext.
            if (win && win->scene)
            {
                win->scene.reset();
            }
            if (win)
            {
                for (auto& quadScene : win->sceneQuadViews)
                    quadScene.reset();
            }
            // Game controller also owns RenderServices-backed view/camera resources.
            // Destroy it BEFORE shutting down the per-window RenderDeviceContext.
            if (win && win->gameView)
            {
                win->gameView.reset();
            }
            if (win && win->renderCtx)
            {
                // This window's Scene/Game controllers were bound to the MAIN
                // window's RenderServices, which created a per-stream pipeline
                // instance keyed by this window's RenderGraph frame. Reap this window's
                // frame-stream slot (pipeline instance + declare-seq record)
                // before the RGFrame dies so a future frame at the recycled
                // address can't pick up a stale slot.
                if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->renderCtx)
                {
                    if (auto* mainRs = m_Windows[0]->renderCtx->GetRenderServices())
                    {
                        if (win->RenderGraphStream.Frame)
                            mainRs->Spine().RemovePipelineInstanceForFrame(win->RenderGraphStream.Frame.get());
                    }
                }
                // RenderGraph arm: cancel pendings whose tickets outlive this frame
                // stream (thumbnail disk cache, debug captures) — a dangling
                // Frame* in the stamp list could mis-stamp a heap-recycled
                // RGFrame at the same address.
                if (win->RenderGraphStream.Frame)
                    Rendering::CancelPendingReadbacksRG(win->RenderGraphStream.Frame.get());
                // The frame stream dies BEFORE the device context — ~RGFrame
                // drains its own timelines (graph-scoped; destroying a
                // semaphore the GPU still signals is device-loss class).
                // Pools die after the frame (8e: per-window), device after.
                win->RenderGraphStream.Frame.reset();
                win->RenderGraphStream.UploadRing.reset();
                win->RenderGraphStream.TransientPool.reset();
                win->RenderGraphStream.PersistentPool.reset();
                win->renderCtx->Shutdown();
                win->renderCtx.reset();
            }
            if (win && win->window)
            {
                win->window->Destroy();
                win->window.reset();
            }
            // Finally remove the context
            m_Windows.erase(m_Windows.begin() + static_cast<long long>(idx));
        }
    }
}

bool EditorApplication::IsPlayModeBlockingSceneWrites() const
{
    return m_PlayMode && m_PlayMode->GetState() != GameEngine::Editor::PlayModeState::Edit;
}

// Window client pixels to Game View viewport-local pixels, for a window context
// the caller has already resolved. File-local: everything the mapping needs hangs
// off the context, so the pointer legs do one window walk instead of two and the
// application's own surface grows nothing.
static void MapClientToPlaySurface(const EditorApplication::EditorWindowContext& win, float clientX,
                                   float clientY, float& playX, float& playY)
{
    playX = clientX;
    playY = clientY;
    if (!win.window || !win.ui || !win.docking)
        return;
    auto* gameView = dynamic_cast<GameViewPanel*>(win.docking->GetPanel(EditorPanelIds::GameView));
    if (!gameView)
        return;
    UIElement* viewport = gameView->GetViewportElement();
    if (!viewport)
        return;
    WindowInputRouter::ClientToSurfaceLocal(win.window.get(), win.ui.get(), viewport->GetLayoutX(),
                                            viewport->GetLayoutY(), clientX, clientY, playX, playY);
}

WindowInputRouterConfig::PlaySurface EditorApplication::GetPlaySurface(Platform::Window* window) const
{
    WindowInputRouterConfig::PlaySurface surface;
    if (!m_RuntimeInput || !m_PlayMode || !m_PlayMode->IsPlayingOrPaused())
        return surface;
    for (auto& win : m_Windows)
    {
        if (!win || win->window.get() != window || !win->docking)
            continue;
        if (!win->docking->IsPanelActiveTab(EditorPanelIds::GameView))
            return surface;
        surface.gameplaySink = m_RuntimeInput.get();
        surface.gameUi = win->gameView ? win->gameView->GetGameUI() : nullptr;
        // The mapper maps from the context resolved here instead of finding the
        // window again. Holding it raw is safe because a surface is built and
        // spent inside one routing call (WindowInputRouter::RouteMouseMove), with
        // no chance for the window list to change in between.
        surface.mapGameplayPointer = [ctx = win.get()](float clientX, float clientY, float& playX, float& playY)
        { MapClientToPlaySurface(*ctx, clientX, clientY, playX, playY); };
        return surface;
    }
    return surface;
}

const WindowInputRouterConfig& EditorApplication::GamepadInputChain() const
{
    const WindowInputRouterConfig* focused = nullptr;
    const WindowInputRouterConfig* main = nullptr;

    for (const auto& win : m_Windows)
    {
        if (!win || !win->window)
            continue;
        // The window running the game wins, wherever editor focus sits: a pad is
        // not typed into a window, and a player holding a button has no hand on
        // the mouse. It also keeps a held button from being stranded — the
        // destination moves only when play or the Game View tab ends, and
        // ResetState on that edge releases the buttons the runtime sink held.
        if (GetPlaySurface(win->window.get()).gameplaySink)
            return win->inputConfig;
        if (!focused && win->window->IsFocused())
            focused = &win->inputConfig;
        if (!main && win->role == WindowRole::Main)
            main = &win->inputConfig;
    }

    if (focused)
        return *focused;
    if (main)
        return *main;
    return Application::GamepadInputChain();
}

EditorApplication::EditorWindowContext* EditorApplication::FindWindowHostingGameView(const UIElement* panel) const
{
    for (auto& win : m_Windows)
    {
        if (win && win->docking && win->gameView &&
            win->docking->GetPanel(EditorPanelIds::GameView) == panel)
            return win.get();
    }
    return nullptr;
}

void EditorApplication::OnShutdown()
{
    // Stop input while every host is alive, then revoke deferred viewport
    // callbacks before their UI dispatchers and this application are destroyed.
    for (auto& win : m_Windows)
        if (win && win->gameView)
            win->gameView->HandleGameUiPointer(false, 0.0f, 0.0f, false, 0);
    if (auto* gameView = FindFirstPanelOfType<GameViewPanel>(m_PanelStorage))
        gameView->ClearPointerCallback();

    // Wait for the startup preload jobs before tearing down the systems they reference.
    if (m_UiPreload.IsValid())
        m_UiPreload.Wait();
    if (m_ShaderPreload.IsValid())
        m_ShaderPreload.Wait();

    // If UI replay created temporary Assets fixtures, remove them first to avoid
    // leaving the working tree dirty after tests.
    if (m_UiReplay)
    {
        // Pixel-probe readbacks must cancel BEFORE render teardown: ticket
        // buffer destroys are timeline-deferred and need the device alive
        // (m_UiReplay itself is destroyed after the windows).
        m_UiReplay->CancelPendingReadbacks();
        const std::filesystem::path assetsRoot = EngineCore::GetInstance().GetAssetManager().GetAssetRoot();
        const std::filesystem::path fixtureRoot = assetsRoot / "__ui_replay_tree_fixtures__";
        std::error_code ec;
        std::filesystem::remove_all(fixtureRoot, ec);
    }

    // Shut down Polyhaven download threads before tearing down subsystems.
    if (m_DownloadManager)
        m_DownloadManager->Shutdown();

    // Stop MCP debug server before tearing down subsystems.
    if (m_DebugServer)
    {
        m_DebugServer->Stop();
        Editor::SetEditorDebugPort(0);
    }

    // Detach runtime input before tearing down subsystems.
    EngineCore::GetInstance().SetRuntimeInput(nullptr);
    m_RuntimeInput.reset();

    // Mark teardown in progress *first* so any OS callbacks (resize/DPI changes)
    // that fire during shutdown cannot re-enter rendering/UI.
    m_IsShuttingDown = true;
    m_DeferredPreUiActions.Clear();
    m_DeferredPostUiActions.Clear();
    // A tool window never attached still registered its ColorPicker context,
    // which points at the window this clear destroys.
    for (const auto& pendingWindow : m_DeferredNativeToolWindows)
        ColorPickerEditorIntegration::Cleanup(pendingWindow.get());
    m_DeferredNativeToolWindows.clear();
    m_DeferredOpenInternalFile.reset();

    // The editor UI holds a raw pointer to the thumbnail provider via EditorContext.
    // Null it immediately so any late UI bind/update will safely skip thumbnails.
    if (m_EditorContext)
    {
        m_EditorContext->Thumbnails = nullptr;
    }

    // Stop centralized file watching BEFORE any teardown at all. Watcher
    // threads dispatch subscription callbacks that post into UI elements (the
    // Assets browser posts grid refreshes from file events) and into ECS
    // systems (NavigationBuildSystem marks navigation assets dirty);
    // StopWatching joins those threads, so after this point no callback can
    // land on state the teardown below destroys. Debounced events make this
    // window real: files the editor itself writes at exit (.Editor state,
    // asset DB, thumbcache) arrive as change events right as teardown begins.
    //
    // This must precede DisableRenderingLoop: that destroys the SystemManager
    // and with it every ECS system holding a subscription.
    FileWatchingService::GetInstance().StopWatching();

    // Disable engine-managed rendering loop early so ECS rendering systems
    // can no longer submit GPU work during teardown.
    EngineCore::GetInstance().DisableRenderingLoop();

    // Unregister native scripting domain-unload hooks early to avoid callbacks into partially torn down editor state.
#if GE_ENABLE_SCRIPTING
    GE_SetOnDomainWillUnload(nullptr, nullptr);
#endif

    // Ensure any in-flight drag/preview state is cleared before tearing down windows
    if (m_DockDnd)
        m_DockDnd->ResetDragStateForShutdown();

    // RenderServices are owned by the main RenderDeviceContext and will be
    // released during per-window RenderDeviceContext::Shutdown() below.

    // Quiesce editor-integration callbacks to avoid late reentry during teardown
    if (m_Docking)
    {
        m_Docking->SetUndockCallback({});
    }
    // Drain any UI deferred actions once before destroying windows
    for (auto& w : m_Windows)
    {
        if (w && w->ui)
        {
            w->ui->DrainDeferredActionsOnce();
        }
    }

    if (m_DockDnd)
        m_DockDnd->ClearAllDockOverlays();

    // IMPORTANT: Uninstalling the native toolbar can trigger OS window messages
    // (including framebuffer resize callbacks). Detach platform callbacks that
    // capture 'this' *before* uninstalling the toolbar.
    for (auto& w : m_Windows)
    {
        if (w && w->window)
        {
            w->window->SetMouseMoveHandler({});
            w->window->SetMouseButtonHandler({});
            w->window->SetCursorEnterHandler({});
            w->window->SetCharHandler({});
            w->window->SetKeyHandler({});
            w->window->SetRefreshHandler({});
            w->window->SetPositionHandler({});
            w->window->SetFramebufferSizeHandler({});
            w->window->SetScrollHandler({});
        }
    }

    // Uninstall native toolbar before destroying any windows it hooks into
    if (m_EditorToolbar)
    {
        m_EditorToolbar->Uninstall();
        m_EditorToolbar.reset();
    }

    // Now that callbacks are detached and the toolbar has been removed, it is safe
    // to destroy the thumbnail provider without risking re-entry during resize events.
    m_ThumbnailProvider.reset();

    // Shutdown VCS glue before destroying UI -- VCS background threads
    // invoke callbacks that post to UI dispatchers.
    if (m_VcsService)
    {
        m_VcsService->Shutdown();
        if (m_EditorContext)
            m_EditorContext->VcsService = nullptr;
        m_VcsService.reset();
    }
    m_VcsUi.reset();

    // Null out PlayModeManager reference before implicit member destruction
    // (m_PlayMode may be destroyed before m_EditorContext due to declaration order).
    if (m_EditorContext)
        m_EditorContext->PlayMode = nullptr;

    // Render-graph participants (panel-owned atlases and the like) hold views,
    // cameras and textures in RenderServices. Release those handles before the
    // frame streams below die, while RenderServices and the device are alive;
    // the same rule that puts win->scene and win->gameView ahead of the device
    // context.
    m_RenderGraphTicks.ReleaseAll();
    // The view overlays read the editor context, which dies with the application.
    Editor::ViewOverlayHost::Get().Reset();

    // RenderGraph arm: every frame stream drains + dies first (~RGFrame waits on its
    // own timelines), then the device-shared pools — all before any window's
    // device context shuts down. Reap each stream's RenderServices slot
    // before its frame dies (dangling stream keys in a still-alive RS would
    // alias a future allocation at the recycled address).
    for (auto& win : m_Windows)
    {
        if (!win)
            continue;
        if (win->RenderGraphStream.Frame && !m_Windows.empty() && m_Windows[0] && m_Windows[0]->renderCtx)
        {
            if (auto* mainRs = m_Windows[0]->renderCtx->GetRenderServices())
                mainRs->Spine().RemovePipelineInstanceForFrame(win->RenderGraphStream.Frame.get());
        }
        if (win->RenderGraphStream.Frame)
            Rendering::CancelPendingReadbacksRG(win->RenderGraphStream.Frame.get());
        win->RenderGraphStream.Frame.reset();
        win->RenderGraphStream.UploadRing.reset();
        // 8e: per-window pools die with their frame stream (device is alive —
        // the device-owning main window shuts down last in the loop below).
        win->RenderGraphStream.TransientPool.reset();
        win->RenderGraphStream.PersistentPool.reset();
    }

    // A scene-thumbnail readback ticket holds its buffer and the main device's
    // pointer, and its destructor destroys the buffer through that pointer. Release
    // it once the frame streams have drained, while the main device is still alive.
    m_SceneThumbnailCapture.reset();

    // Drawn menus CloseAll() through a UIManager this loop is about to
    // destroy, and native ones tear down an OS menu. Unregister is compiled
    // out on Win32/macOS and otherwise runs from ~EditorWindowContext after
    // ui.reset(), so the slot has to go now, while every UIManager is alive.
    ReleaseShowingContextMenu();

    // Panels are Mount targets owned here, not by any UI tree, and their
    // destructors reach their window's UIManager, dialogs and overlays parented
    // under that manager's root, their own context menus, and the device. End
    // their lifetime while all of those are alive: announce the detach their
    // subscribers are owed, then destroy them. A destroyed Mount target clears
    // its Mount (MountRegistry), so the docking trees torn down below no longer
    // reach them.
    if (m_PanelManager)
        m_PanelManager->ClearUpdateCallbacks();
    for (auto& panel : m_PanelStorage)
        UI::DispatchDestructionDetach(panel.get());
    m_PanelStorage.clear();

    // Tear down all windows (UI then rendering then GLFW)
    for (auto it = m_Windows.rbegin(); it != m_Windows.rend(); ++it)
    {
        auto& win = *it;
        if (!win)
            continue;
        // Quiesce ColorPicker integration first so no global callbacks survive
        // after window/UI teardown starts.
        ColorPickerEditorIntegration::Cleanup(win.get());
        // UI and docking
        win->ui.reset();
        win->docking = nullptr;
        win->dockingOwned.reset();
        // Scene controller owns RenderServices-backed view/camera resources.
        // Destroy it BEFORE shutting down the per-window RenderDeviceContext so
        // SceneViewController::~SceneViewController can safely call RenderServices::ReleaseView.
        win->scene.reset();
        for (auto& quadScene : win->sceneQuadViews)
            quadScene.reset();
        // Game controller also owns RenderServices-backed view/camera resources.
        // Destroy it BEFORE shutting down the per-window RenderDeviceContext so
        // GameViewController::~GameViewController can safely call RenderServices::ReleaseView.
        win->gameView.reset();
        // Render resources
        if (win->renderCtx)
        {
            win->renderCtx->Shutdown();
            win->renderCtx.reset();
        }
        // Window
        if (win->window)
        {
            win->window->Destroy();
            win->window.reset();
        }
    }
    m_DispatchingWindow = nullptr;
    m_Windows.clear();

    // Editor data models
    m_Docking.reset();

    Platform::Window::Terminate();
}


void EditorApplication::SetDockEdgeFraction(float frac)
{
    m_DockDnd->SetDockEdgeFraction(frac);
}

void EditorApplication::SyncHiDpiPlatformSettingsFromPreferences()
{
    for (auto& win : m_Windows)
    {
        if (!win)
            continue;
        if (win->uiPlatform)
            Editor::ApplySavedHiDpiPlatformSettings(win->uiPlatform.get());
        if (win->window && win->ui && !m_UiReplayScenarioPath.has_value())
            WindowInputRouter::RefreshUiMouseCoordinates(win->window.get(), win->ui.get());
    }
}

EditorApplication::RenderWindowFrameResult EditorApplication::RenderWindowFrame(
    EditorWindowContext* ctx, Rendering::IDevice* dev, bool passiveUi)
{
    RenderWindowFrameResult result{};
    if (ctx && ctx->skipRenderFramesAfterHdrSwitch > 0)
    {
        --ctx->skipRenderFramesAfterHdrSwitch;
        return result;
    }
    // RenderGraph arm: EVERY window runs an RenderGraph frame (8e — was window[0]-only through
    // 8c). Frames/rings/pools are all per-window; the device is
    // process-shared, and all RenderGraph declaration paths resolve the MAIN
    // RenderServices (controllers were constructed on it).
    const bool useRGFrame = dev != nullptr;
    if (useRGFrame)
    {
        if (!ctx->RenderGraphStream.Frame)
        {
            // 8e: pools are per-window (per frame stream) — RGFrame::Execute
            // ticks pool aging with the stream-local index.
            ctx->RenderGraphStream = Rendering::RenderGraph::MakeWindowRGFrame(*dev);
        }
        // RGFrame::BeginFrame rotates+rewinds the upload ring slot, which is
        // safe ONLY behind the fence-wait of a fresh IDevice::BeginFrame (the
        // one BeginWindowRenderFrame ran for this call). Two RenderGraph BeginFrames
        // off one device frame would rewind a slot the GPU may still read.
        assert(ctx->deviceFrameStamp != ctx->rg2LastBeginDeviceStamp &&
               "RGFrame::BeginFrame requires a fresh IDevice::BeginFrame fence-wait");
        ctx->rg2LastBeginDeviceStamp = ctx->deviceFrameStamp;
        ctx->RenderGraphStream.Frame->BeginFrame(ctx->rg2FrameIndex++);
    }
    auto MsNow = []() { return std::chrono::high_resolution_clock::now(); };
    auto MsBetween = [](auto a, auto b) -> double
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // In the passive-UI path (live resize, drag preview) we must run the Yoga
    // layout solve BEFORE recording world views so that viewport elements report
    // their new dimensions (the 8b routing below also needs viewport sizes). In
    // the normal main-loop path the UIManager was already updated during
    // EditorApplication::Update().
    if (passiveUi && ctx->ui)
        ctx->ui->Update(0.0f, /*interactive=*/false);

    // Thumbnail capture is only driven from the primary window's scene view.
    Editor::SceneThumbnailCapture* thumbnailCapture =
        (ctx == (m_Windows.empty() ? nullptr : m_Windows[0].get()))
        ? m_SceneThumbnailCapture.get() : nullptr;

    // ── RenderGraph frame routing: the editor is RenderGraph-only. Every renderable producer
    // declares into the per-window RenderGraph frame; the scene/game views push their
    // view targets onto the single spine, the UI declares the composite, and
    // one terminal encode owns the OETF. ──
    WindowFrameRouting routing{};
    // Match the world-update decision latched at the start of Update. A project
    // switch can hide the picker later in that update; rendering immediately
    // would reuse the paused world's retired transient bindings. The next
    // Update resumes extraction and resets its caches before world recording.
    const bool idleWorldForPicker = m_WorldPathIdledForProjectPicker;
    routing.rg2Scene = useRGFrame && ctx->RenderGraphStream.Frame && ctx->scene != nullptr && !idleWorldForPicker;
    routing.gvPanel =
        ctx->gameView ? ResolveMountedPanelForWindow<GameViewPanel>(ctx, EditorPanelIds::MountGameView)
                      : nullptr;
    // An inactive dock tab's panel is unmounted, but its viewport element keeps
    // the layout box from its last visible frame — the mounted resolve, not the
    // size probe, is what says the tab is actually on screen. The unmounted
    // fallback below exists only for pre-mount warmup; letting it feed the
    // size-driven declare kept a hidden Game View rendering the full pipeline
    // behind the Scene View every frame.
    const bool gvPanelMounted = routing.gvPanel != nullptr;
    if (!routing.gvPanel && ctx->gameView && ctx->docking)
    {
        routing.gvPanel = dynamic_cast<GameViewPanel*>(
            ctx->docking->GetPanel(EditorPanelIds::GameView));
    }
    if (routing.gvPanel && ctx->ui)
        routing.gvPanel->PrepareForFirstMount(ctx->ui.get());
    routing.forceGameViewRecord = ctx->gameView && ctx->gameView->GetMovieCapture().IsRecording();
    if (routing.rg2Scene && ctx->gameView)
    {
        if (routing.forceGameViewRecord)
        {
            // Movie overrides the RENDER resolution and runs panel-less —
            // tickets are RenderGraph-native since 8e-6.
            routing.gvW = std::max(1u, ctx->gameView->GetMovieCapture().GetWidth());
            routing.gvH = std::max(1u, ctx->gameView->GetMovieCapture().GetHeight());
            routing.rg2Game = true;
        }
        else if (gvPanelMounted &&
                 Editor::SceneViewRenderCoordinator::TryResolveRenderableViewportSize(
                     ctx, routing.gvPanel->GetViewportElement(), routing.gvW, routing.gvH))
        {
            routing.rg2Game = true;
        }
        else if (routing.gvPanel && ctx->gameView->NeedsPresentationWarmup())
        {
            // Cold-start the hidden Game View once so its first tab activation
            // already has a complete frame to present if input arrives after
            // extraction. The inactive Game panel has no measured viewport yet;
            // the visible Scene viewport is the closest matching content extent.
            if (auto* scenePanel = ResolveMountedPanelForWindow<SceneViewPanel>(
                    ctx, EditorPanelIds::MountSceneView))
            {
                (void)Editor::SceneViewRenderCoordinator::TryResolveRenderableViewportSize(
                    ctx, scenePanel->GetViewportElement(), routing.gvW, routing.gvH);
            }
            if (routing.gvW <= 1u || routing.gvH <= 1u)
            {
                uint32_t swapW = 1280u;
                uint32_t swapH = 720u;
                (void)dev->GetSwapchainSize(swapW, swapH);
                routing.gvW = std::max(2u, swapW);
                routing.gvH = std::max(2u, swapH);
            }
            routing.rg2Game = true;
        }
        else if (routing.gvPanel)
        {
            // D5 (8e-8): mounted-but-degenerate game viewport — nothing can
            // render it, so it counts as ABSENT. RecordWindowWorldViews
            // quiesces the view.
            routing.gameDegenerate = true;
        }
    }
    // The editor has no hybrid/old-graph arm: every frame is RenderGraph-pure. UI-root
    // warm-up frames still run the frame, just without the UI declare.
    const bool uiRootReady =
        EnsureUiRootReadyForRender(ctx, passiveUi ? "single-window render" : "main-loop render");
    routing.pure = true;

    if (ctx->ui)
        ctx->ui->ClearRenderTargetOverride();

    auto tRenderSub = MsNow();
    // World delta for the scene/culling record; the game UI's animation clock is now
    // sourced from Time::GetDeltaTime() inside GameUIHost (clamp + replay live there).
    const float worldDelta = static_cast<float>(std::max(0.0, GetDeltaTime()));
    result.sceneViewportReady = RecordWindowWorldViews(
        ctx, /*bindViewportBackground=*/true, routing, worldDelta, thumbnailCapture,
        useRGFrame ? ctx->RenderGraphStream.Frame.get() : nullptr);
    const double worldViewsMs = MsBetween(tRenderSub, MsNow());
    result.worldViewsMs = worldViewsMs;

    {
        // ── Pure RenderGraph frame: UI declares into the frame, single terminal
        // encode, the old graph is never begun. ──

        // HDR-switch recheck BEFORE the backbuffer import — an imported
        // backbuffer must never be abandoned (the hybrid arm's mid-frame
        // skip sits after its terminal passes instead).
        if (ctx->skipRenderFramesAfterHdrSwitch > 0)
        {
            --ctx->skipRenderFramesAfterHdrSwitch;
            return result;
        }

        auto& frame = *ctx->RenderGraphStream.Frame;
        const Rendering::RenderGraph::RGTexture backbuffer = frame.ImportBackbuffer();
        if (!backbuffer.IsValid())
            return result; // no swapchain image this frame — skip

        // Thumbnails (8c-2b): declare pending renders into the frame, then
        // register + publish every ready slot — publish is a per-frame value
        // RenderRG consumes below. Sits AFTER the abandon points above so an
        // un-executed frame never advances slot bookkeeping. Dispatching new
        // renders requires the spine to have stamped THIS frame incarnation
        // (the bucketer twin needs its frame-local visibility value and
        // ordered skin-palette producers); a pure frame without a scene/game
        // declare has no spine — renders defer, ready slots still publish.
        if (ctx->capabilities.thumbnails && !idleWorldForPicker)
        {
            const auto tThumb0 = MsNow();
            auto* rs = MainRenderServices();
            if (auto* svc = dynamic_cast<ThumbnailService*>(m_ThumbnailProvider.get()))
            {
                // On-demand: render engine thumbnails the UI requested but could
                // not resolve (hierarchy/inspector/asset-field entity icons use
                // the list variant the asset browser's cell requests never produce).
                if (ctx->ui)
                    for (const std::string& name : ctx->ui->ConsumeUnresolvedExternalTextureRequests())
                        svc->EnsureEngineThumbnailRequested(ctx->windowId, name);
                if (rs && rs->FrameRG().For.IsFor(frame))
                    svc->TickThumbnailsForWindowRG(ctx->windowId, frame);
                if (ctx->ui)
                    svc->RegisterReadyThumbnailsRG(ctx->windowId, ctx->ui.get(), &frame);
            }
            result.thumbnailsMs = MsBetween(tThumb0, MsNow());
        }

        // Video player modal (main window only): declare the staged-frame copy
        // into this frame and publish the texture for the UI to sample. Routing
        // the copy through the graph orders it before the UI pass via the same
        // sampled-read dependency the scene viewport and thumbnails rely on.
        if (ctx == m_Windows[0].get())
            m_VideoPlayer.TickRenderRG(frame);

        // Panel-owned render-graph participants (registered through
        // EditorContext). Same spine gate as the thumbnails above: a participant
        // may bind material pipelines and the shared material SSBO, which only
        // exist once the frame spine has stamped THIS frame incarnation.
        if (auto* rs = MainRenderServices(); rs && rs->FrameRG().For.IsFor(frame))
            m_RenderGraphTicks.TickAll(ctx->windowId, ctx->ui.get(), frame);

        Rendering::RenderGraph::RGTexture finalLinear{};
        Rendering::RenderGraph::RGTexture uiTarget = backbuffer;
        // 8c-4: a pending debug capture (MCP screenshot) reads the composite
        // via tickets — the backbuffer import is discard-contract and may not
        // support readback.
        const bool captureWantsComposite =
            m_DebugServer && m_DebugServer->HasAnyPendingRenderCallbacks();
        // 8e-7: a pixel-probing replay reads the composite the same way.
        const bool uiReplayWantsComposite =
            !passiveUi && m_UiReplay && m_UiReplay->WantsPixelReadbackComposite();

        // The attachment's declared blend space (#767 flip slice ii + #784).
        // SDR frames — display-bound and capture-bound alike — declare
        // EncodedSrgb: the UI blends on raw sRGB-encoded bytes (the browser
        // compositing model every stylesheet in this repo was authored
        // against), and the screen and every SDR capture consume the same
        // encoded artifact, so the #784 capture tie-break has no SDR arm
        // left. Under an active HDR output mode the tie-break survives: an
        // attached SDR capture consumer (MCP screenshot readback /
        // pixel-probing replay) wins the dual-consumer composite and
        // declares LinearSdr — never EncodedSrgb, whose presentation the
        // Finalize contract refuses under HDR (FinalizeContract.h) — so an
        // HDR-display capture keeps the linear blend and presents un-lifted
        // for that frame; a dual-path render is the escape hatch if either
        // consequence ever matters. Display-bound HDR frames follow the
        // active output mode.
        //
        // The SDR arm reads the SAME predicate the world views finalize on
        // (Engine::Renderer::ViewFinalizeEligible), and it has to: a finalized
        // view hands the composite sRGB code values, which only the encoded blend
        // space leaves alone. Splitting the two decisions would let one flip
        // without the other and either wash every viewport out or double-encode it.
        const UI::UITargetSpace uiTargetSpace =
            Engine::Renderer::ViewFinalizeEligible(dev) ? UI::UITargetSpace::EncodedSrgb()
            : (captureWantsComposite || uiReplayWantsComposite)
                ? UI::UITargetSpace::LinearSdr()
                : UI::UITargetSpace::ForDisplay(dev->GetActiveHdrOutputMode());
        const bool uiTargetEncoded = uiTargetSpace == UI::UITargetSpace::EncodedSrgb();
        // What the composite's bytes HOLD under that declaration — published
        // to the capture context below and declared at the replay probe.
        const UI::UITextureSpace compositeSpace = uiTargetSpace.ReadbackSpace();

        {
            // Every editor frame routes through the RGBA16F composite:
            // - SDR frames blend encoded (#767) — the bytes must never meet a
            //   potentially-_SRGB backbuffer ROP, whose write-side OETF would
            //   re-encode them; the terminal pass requantizes instead;
            // - HDR frames composite scene-linear for the PQ/HLG/scRGB
            //   terminal encode (the single OETF);
            // - capture consumers read it via tickets (8c-4 / 8e-7 above).
            const auto& bbDesc = frame.Graph().ResourceDesc(backbuffer.Id);
            Rendering::TextureDesc td{};
            td.width = bbDesc.Width > 0 ? bbDesc.Width : 1u;
            td.height = bbDesc.Height > 0 ? bbDesc.Height : 1u;
            td.mipLevels = 1;
            td.arrayLayers = 1;
            td.sampleCount = 1;
            td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                       static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
            td.debugName = "Editor.FinalLinear";
            finalLinear = frame.CreateTexture("Editor.FinalLinear", td);
            if (finalLinear.IsValid())
                uiTarget = finalLinear;
        }

        m_DispatchingWindow = ctx;
        const auto tUI0 = MsNow();
        // UI-root warm-up frames declare no UI pass (the window is still
        // hidden); a frame that nothing cleared must never be encoded/presented.
        const bool uiDeclared = uiRootReady && ctx->ui->RenderRG(frame, uiTarget, uiTargetSpace);
        result.uiRecordMs = MsBetween(tUI0, MsNow());
        m_DispatchingWindow = nullptr;

        if (!uiDeclared)
            return result; // no UI declared (warm-up / invalid target) — never
                           // encode/present a target nothing cleared

        if (finalLinear.IsValid() && uiTarget.Id == finalLinear.Id)
        {
            // finalLinear is the composite the UI blended into under the
            // uiTargetSpace declared above, and the quantizer this terminal
            // takes is the shared transfer decision, at the degenerate point
            // where the source's step and the destination ARE the same surface:
            //
            // Encoded (SDR frames): the composite already carries the output
            // curve, so its bytes sit on the presented step — chrome from CSS
            // values that land on code values on their own, world pixels from a
            // finalize (encode+deband+dither at this swapchain's step) or, on a
            // hybrid frame, from the UI applying the OETF at sample time. The
            // decision answers None either way, because it keys on the bytes
            // being on the presented step, not on how they got there: round the
            // composite onto the backbuffer's step and add nothing (raw into a
            // UNORM backbuffer, D(c) into an _SRGB one). This is what stops the
            // terminal graining flat chrome — it no longer has to tell chrome
            // from world, because it filters neither.
            //
            // The hybrid frame is the honest caveat, not a second decision: it
            // reaches here correct but UNFILTERED, and no pass downstream can
            // give it a dither (R7, landing 4). No editor frame is hybrid today.
            //
            // Linear (HDR modes, and HDR-display capture frames): the views did
            // NOT finalize, so this pass still owns the OETF and its quantizer,
            // full-frame — a linear source always answers Destination. The
            // 10-bit HDR step puts that grain at ~0.1%.
            const Rendering::TextureFormat presentedFormat =
                dev && ctx->renderCtx
                    ? dev->GetWindowTargetSwapchainFormat(ctx->renderCtx->GetWindowTarget())
                    : Rendering::TextureFormat::Unknown;
            Rendering::Passes::AddSRGBEncodePassRG(
                frame, finalLinear, backbuffer,
                {.InputSpace = uiTargetEncoded
                                   ? Rendering::Passes::FinalizeInputSpace::EncodedSrgb
                                   : Rendering::Passes::FinalizeInputSpace::Linear,
                 .Quantizer = Rendering::Passes::SelectTransferQuantizer(
                     uiTargetEncoded, presentedFormat, presentedFormat),
                 .VolumeDebandThresholdLsb =
                     Engine::Renderer::ResolveViewDebandThresholdLsb(MainRenderServices()),
                 // Inert on SDR frames — the decision above answers None there
                 // and the pass zeroes the phase with the dither. It reaches the
                 // image on the linear arms (HDR modes, HDR-display capture),
                 // where this pass is the only quantizer on the frame.
                 .DitherPhase = Rendering::Passes::ScreenDitherPhase(frame.FrameIndex())});
        }

        // 8e-7: the pixel-probing replay declares its region readback of the
        // FinalLinear composite NOW (after the UI pass, before Execute);
        // tickets are stamped by OnFrameSubmittedReadbacksRG below — no
        // abandon point exists between this seam and Execute.
        if (uiReplayWantsComposite && finalLinear.IsValid() && ctx->ui)
        {
            // The probe reads the composite's bytes as declared this frame:
            // SrgbAuthored (encoded at rest) on SDR frames, DisplayLinearSdr
            // on HDR-display capture frames. Stated here, never inferred from
            // the RGBA16F format at the far end (P5).
            m_UiReplay->TickBeforeRenderRG(*ctx->ui, frame, finalLinear.Id, compositeSpace, dev);
        }

        // 8c-4: RenderGraph-capable debug callbacks (MCP screenshots) declare their
        // readbacks NOW — after all normal passes, before Execute (the RenderGraph
        // mirror of the hybrid pre-compile seam). The capture context hands
        // them this frame's raw ids, validity-stamped by incarnation.
        if (m_DebugServer)
        {
            ctx->rg2Capture.frame = &frame;
            ctx->rg2Capture.finalLinearId = finalLinear.IsValid() ? finalLinear.Id : 0xFFFFFFFFu;
            ctx->rg2Capture.compositeSpace = compositeSpace;
            ctx->rg2Capture.backbufferId = backbuffer.Id;
            ctx->rg2Capture.pure = true;
            ctx->rg2Capture.frameIndex = frame.FrameIndex();
            m_DebugServer->RunRenderCallbacks();
        }

        // A2.4-P0-R (§D6.6): inject a fork-join primitive so RGFrame can record the
        // RecordInSecondary pass interiors on job-system workers. Type-erased at
        // the RGFrame boundary so the render-graph module stays decoupled from
        // JobSystem; here (engine layer) it is backed by WorkStealingThreadPool::Run
        // + a participating Wait (never ParallelFor — §A2.4-D3). Inert unless
        // GE_PARALLEL_RECORD is set (RecordAndSubmit gates on the env kill switch).
        // Native external waiters park while workers record the interiors;
        // a host thread that cannot block participates in this tagged counter.
        frame.SetRecordParallel(
            [](uint32_t count, const std::function<void(uint32_t)>& body)
            {
                auto& pool = EngineCore::GetInstance().GetJobSystem();
                JobSystem::JobCounter counter;
                for (uint32_t i = 0; i < count; ++i)
                    pool.Run([&body, i]() { body(i); }, counter);
                pool.Wait(counter);
            });

        const auto tExec0 = MsNow();
        frame.Execute();
        result.rgExecuteMs = MsBetween(tExec0, MsNow());
        result.rgCompileMs = 0.0; // RenderGraph has no compile phase — declaration IS the build
        if (auto* rs = MainRenderServices())
            rs->Spine().OnFrameSubmittedRG(frame, frame.SubmissionToken());
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());

        // Phase C RG CSV sidecar (--bench-rg-csv): lazy-open on first frame,
        // then one row per frame. rgCompileMs is 0 in RenderGraph (declaration IS the
        // build); rgExecuteMs is the per-frame Execute time.
        if (m_BenchRgCsvPath.has_value())
        {
            if (!m_RgCsvSidecar)
            {
                m_RgCsvSidecar = GameEngine::Editor::Telemetry::RGCsvSidecar::Open(
                    *m_BenchRgCsvPath, "unknown", "editor");
                if (!m_RgCsvSidecar)
                    m_BenchRgCsvPath.reset(); // failed open — don't retry every frame
            }
            if (m_RgCsvSidecar)
            {
                m_RgCsvSidecar->SetAppUpdateSubPhases(
                    m_LastAppUpdateSubPhases.preUiMs, m_LastAppUpdateSubPhases.debugPanelsMs,
                    m_LastAppUpdateSubPhases.uiWindowsMs, m_LastAppUpdateSubPhases.tailMs);
                m_RgCsvSidecar->Append(m_RgCsvFrameCounter++, result.rgCompileMs, result.rgExecuteMs);
            }
        }

        static const bool kEditorRg2DiagPure = Platform::EnvironmentSwitchEnabled("GE_EDITOR_RG_DIAG", false);
        if (kEditorRg2DiagPure && ctx == m_Windows[0].get() && ctx->rg2FrameIndex == 3)
            frame.SetProfilingEnabled(true);
        if (kEditorRg2DiagPure && (ctx->rg2FrameIndex % 120) == 10)
        {
            const auto& g = frame.Graph();
            fprintf(stderr, "[EdRGDiag] frame=%llu passes=%zu live=%zu\n",
                    static_cast<unsigned long long>(ctx->rg2FrameIndex), g.PassCount(),
                    g.LivePassCount());
            for (size_t p = 0; p < g.PassCount(); ++p)
                fprintf(stderr, "[EdRGDiag]   pass[%zu] '%s'%s\n", p,
                        g.PassName(static_cast<Rendering::RenderGraph::RGPassId>(p)),
                        g.IsCulled(static_cast<Rendering::RenderGraph::RGPassId>(p)) ? " CULLED" : "");
            fflush(stderr);
        }

        MaybeLogUiSlotDiagnostics(ctx);
        const auto tPresent0 = MsNow();
        dev->Present();
        result.presentMs = MsBetween(tPresent0, MsNow());

        // First-frame render sub-phase diagnostics.
        if (result.rgExecuteMs > 50.0 || worldViewsMs > 50.0 || result.uiRecordMs > 50.0)
        {
            Logger::Log::Info(
                "[FrameRender] WorldViews={:.1f}ms UIRecord={:.1f}ms RGExecute={:.1f}ms Present={:.1f}ms",
                worldViewsMs, result.uiRecordMs, result.rgExecuteMs, result.presentMs);
        }
        return result;
    }
}

void EditorApplication::RenderSingle(EditorWindowContext* ctx)
{
    Rendering::IDevice* dev = (ctx && ctx->renderCtx) ? ctx->renderCtx->GetDevice() : nullptr;
    if (!ctx || !ctx->window || !dev || !ctx->ui)
        return;
    int fbW = 0, fbH = 0;
    ctx->window->GetFramebufferSize(fbW, fbH);
    if (fbW <= 0 || fbH <= 0)
        return;
    if (ctx->skipRenderFramesAfterHdrSwitch > 0)
        return;
    if (!BeginWindowRenderFrame(ctx, dev))
        return;

    RenderWindowFrame(ctx, dev, /*passiveUi=*/true);
}

void EditorApplication::RefreshWorldRenderForPassiveFrame(EditorWindowContext* ctx)
{
    if (!ctx || !ctx->capabilities.worldRender)
        return;

    auto& engine = EngineCore::GetInstance();
    if (!engine.GetRenderingLoop())
        return;

    engine.StepRenderingLoop(0.0f);
}

void EditorApplication::ToggleDockDebugZones()
{
    DockOverlay::SetDebugZonesEnabled(!DockOverlay::IsDebugZonesEnabled());
    // Refresh overlays across windows so debug zones appear/disappear immediately
    m_DockDnd->ClearAllDockOverlays();
}

#if defined(_DEBUG)

void EditorApplication::DebugExportCurrentUILayout(EditorWindowContext* ctx)
{
    // Prefer the context passed in from the window that triggered the hotkey.
    EditorWindowContext* target = ctx;
    if (!target)
    {
        // Fallback to the dispatching window (if any) or the main window.
        target = m_DispatchingWindow;
        if (!target && !m_Windows.empty())
        {
            target = m_Windows[0].get();
        }
    }
    if (!target || !target->ui)
    {
        return;
    }

    // Export into a deterministic directory next to the working directory so it
    // is easy to locate from external tools.
    std::filesystem::path outDir = std::filesystem::current_path() / "UIExport";
    target->ui->DebugExportLayoutAndStyles(outDir.string());
}

#endif // _DEBUG




void EditorApplication::OnWindowMoved(EditorWindowContext* moving, int /*x*/, int /*y*/)
{
    if (moving)
        moving->lastWindowMoveAt = std::chrono::steady_clock::now();
    m_DockDnd->OnWindowMoved(moving);
}

void EditorApplication::OnFramebufferResized(EditorWindowContext* ctx, int width, int height)
{
    // During teardown OS callbacks (resize/DPI changes) can still fire while
    // we are destroying subsystems. Never re-enter rendering once shutdown begins.
    if (m_IsShuttingDown || ShouldExit())
        return;
    if (!ctx || !ctx->window)
        return;

    const int prevW = ctx->width;
    const int prevH = ctx->height;

    // Track latest framebuffer size for this window so UI/layout fallbacks remain accurate.
    ctx->width = width;
    ctx->height = height;
    ctx->lastWindowMoveAt = std::chrono::steady_clock::now();

    // Ignore zero/negative sizes (minimized or not yet visible); keep the last valid swapchain.
    if (width <= 0 || height <= 0)
        return;

    // Some OS operations (e.g., native menu/toolbar updates) can trigger framebuffer callbacks
    // even when the framebuffer dimensions are unchanged. Avoid recreating the swapchain and
    // forcing an immediate render in that case, which can cause visible flicker.
    if (width == prevW && height == prevH)
        return;

    const int resolvedMonitor = Platform::GetActiveMonitorIndexForWindow(ctx->window->GetGLFWHandle());
    const bool monitorChanged = resolvedMonitor >= 0 && resolvedMonitor != ctx->window->GetActiveMonitorIndex();
    if (monitorChanged)
        m_HdrOutput->OnWindowMonitorChanged(ctx, resolvedMonitor);

#if defined(__APPLE__)
    {
        const int targetMonitor = resolvedMonitor >= 0 ? resolvedMonitor : ctx->window->GetActiveMonitorIndex();

        float nativeScaleX = 1.0f;
        float nativeScaleY = 1.0f;
        ctx->window->GetContentScale(nativeScaleX, nativeScaleY);
        const bool havePreviousNativeScale = ctx->lastNativeContentScaleX > 0.0f &&
                                             ctx->lastNativeContentScaleY > 0.0f;
        const bool nativeScaleChanged = havePreviousNativeScale &&
                                        (std::fabs(nativeScaleX - ctx->lastNativeContentScaleX) > 0.01f ||
                                         std::fabs(nativeScaleY - ctx->lastNativeContentScaleY) > 0.01f);
        ctx->lastNativeContentScaleX = nativeScaleX;
        ctx->lastNativeContentScaleY = nativeScaleY;

        const bool monitorRefreshPending = ctx->pendingHdrOutput.pending &&
                                           ctx->pendingHdrOutput.forceSwapchainRefresh;
        if (monitorChanged || monitorRefreshPending || nativeScaleChanged)
        {
            // macOS can emit several large transient framebuffer sizes while a window
            // is crossing displays, before GLFW has settled on the new active monitor.
            // On MoltenVK, rendering into one of those transient swapchains can poison
            // the in-flight frame and report VK_ERROR_DEVICE_LOST. Keep deferring monitor
            // and content-scale transitions, but let ordinary same-monitor resize fall
            // through to the immediate render path below to avoid stretched UI.
            m_HdrOutput->QueueUpdate(ctx, targetMonitor, true, true);
            ctx->pendingSwapchainResize = true;
            ctx->pendingWidth = width;
            ctx->pendingHeight = height;
            m_HdrOutput->BeginMonitorSettleSuppression(ctx);
            return;
        }
    }
#endif

    // Recreate swapchain and render immediately to prevent content stretching during live resize.
    // Without this, macOS displays the old framebuffer content scaled to the new window
    // size until the next frame is rendered in the main loop.
    Rendering::IDevice* dev = (ctx->renderCtx) ? ctx->renderCtx->GetDevice() : nullptr;
    if (dev)
    {
        if (ctx->renderCtx)
        {
            ctx->renderCtx->RecreateWindowTargetSwapchain(static_cast<uint32_t>(width),
                                                          static_cast<uint32_t>(height));
        }
    }

    RefreshWorldRenderForPassiveFrame(ctx);

    // Immediately render a frame to prevent content stretching during live resize.
    RenderSingle(ctx);
}

void EditorApplication::OnFilesDropped(EditorWindowContext* ctx, const std::vector<std::filesystem::path>& paths)
{
    if (m_IsShuttingDown || ShouldExit() || !ctx || !ctx->window || !ctx->ui || paths.empty())
        return;
    // Native drops push real cursor coordinates into the UI; a replay's synthetic
    // input stream must stay uncontaminated.
    if (m_UiReplayScenarioPath.has_value())
        return;

    std::vector<std::filesystem::path> existingPaths;
    existingPaths.reserve(paths.size());
    for (const auto& path : paths)
    {
        if (path.empty())
            continue;
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(path, ec);
        if (ec)
            abs = path;
        if (std::filesystem::exists(abs, ec))
            existingPaths.push_back(abs.lexically_normal());
    }
    if (existingPaths.empty())
        return;

    Logger::Log::Info("Editor: received native file drop with {} path(s)", existingPaths.size());

    // A platform that staged the drop in transient storage releases it once the
    // Assets panel has copied the files; Platform::ReleaseTransientFiles is a
    // no-op where the paths are the user's own files.
    const std::vector<std::filesystem::path> droppedPaths = existingPaths;

    Editor::AssetPathsDragPayload payload;
    payload.paths = std::move(existingPaths);
    payload.copyOnly = true; // Native OS drops should import/copy, never move user files.
    const auto& first = payload.paths.front();
    payload.displayLabel = payload.paths.size() == 1
        ? first.filename().string()
        : first.filename().string() + " + " + std::to_string(payload.paths.size() - 1);

    UI::Interaction::DragPayload dragPayload = UI::Interaction::DragPayload::Create(std::move(payload));
    dragPayload.DisplayLabel = dragPayload.TryGet<Editor::AssetPathsDragPayload>()
        ? dragPayload.TryGet<Editor::AssetPathsDragPayload>()->displayLabel
        : std::string{};
    dragPayload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;

    WindowInputRouter::RefreshUiMouseCoordinates(ctx->window.get(), ctx->ui.get());
    // Native OS drop callbacks can arrive without a preceding interactive UI
    // update on this frame; refresh hover/hit-test state before resolving the
    // drop target so GetHoveredElement() reflects the actual cursor location.
    ctx->ui->Update(0.0f, /*interactive=*/true);
    auto* dd = ctx->ui->GetDragDropManager();

    if (!dd)
    {
        Platform::ReleaseTransientFiles(droppedPaths);
        return;
    }

    const Mathematics::Vector2 cursor = ctx->ui->GetMousePosition();
    UIElement* hovered = ctx->ui->GetHoveredElement();
    dd->BeginDrag(std::move(dragPayload));
    dd->UpdateHover(hovered, cursor.x, cursor.y, /*mods=*/0);
    const bool targetAcceptedDrop = dd->GetCurrentFeedback().Allowed;
    dd->CommitDrop(/*mods=*/0);

    if (!targetAcceptedDrop)
        Logger::Log::Info("Editor: native file drop ignored because no asset drop target accepted it");

    Platform::ReleaseTransientFiles(droppedPaths);
}

void EditorApplication::ApplyInspectorToggleAlign(const std::string& value)
{
    for (auto& p : m_PanelStorage)
        if (auto* ip = dynamic_cast<InspectorPanel*>(p.get()))
            InspectorPanel::ApplyToggleAlign(ip, value);
}

void EditorApplication::ShowTabContextMenu(const std::string& panelId, float x, float y, Platform::Window* window)
{
    enum : uint32_t
    {
        kCloseTab = 1,
        kPinOnTop = 2
    };

    constexpr const char* kPrefKeyTabRightClickContextMenu = "ui.tabRightClickContextMenu";
    bool contextMenuEnabled = true;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool(kPrefKeyTabRightClickContextMenu, contextMenuEnabled);
    }

    DockspaceElement* dockspace = nullptr;
    EditorWindowContext* floatingCtx = nullptr;
    if (window)
    {
        for (auto& w : m_Windows)
        {
            if (!w || w->window.get() != window || !w->ui)
                continue;
            if (!w->floatingPanelId.empty())
                floatingCtx = w.get();
            if (UIElement* rootEl = w->ui->GetRootElement())
                dockspace = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock"));
            break;
        }
    }

    auto countTabs = [](const DockNode* node) -> size_t {
        if (!node)
            return 0;
        if (node->IsLeaf())
            return node->GetTabs().size();
        size_t total = 0;
        std::function<void(const DockNode*)> visit = [&](const DockNode* n) {
            if (!n)
                return;
            if (n->IsLeaf())
            {
                total += n->GetTabs().size();
                return;
            }
            visit(n->First());
            visit(n->Second());
        };
        visit(node);
        return total;
    };

    auto closeTabOrFloatingWindow = [dockspace, panelId, floatingCtx, countTabs]() {
        if (floatingCtx && floatingCtx->docking && floatingCtx->window &&
            countTabs(floatingCtx->docking->GetRoot()) <= 1)
        {
            floatingCtx->window->RequestClose();
            return;
        }

        if (dockspace)
            (void)dockspace->RequestTabRemoval(panelId);
        if (floatingCtx && floatingCtx->docking && floatingCtx->docking->IsEmpty() && floatingCtx->window)
            floatingCtx->window->RequestClose();
    };

    if (!contextMenuEnabled)
    {
        closeTabOrFloatingWindow();
        return;
    }

    m_TabContextMenu = CreateContextMenu();
    auto* menu = m_TabContextMenu.get();
    if (!menu)
        return;

    menu->AddItem(0, "Close", kCloseTab, MenuItemFlag_None);
    menu->SetItemIcon(kCloseTab, EditorIcons::kClose);

    if (floatingCtx)
    {
        const bool pinned = floatingCtx->window && floatingCtx->window->IsAlwaysOnTop();
        menu->AddSeparator(0);
        menu->AddItem(0, "Pin on Top", kPinOnTop, pinned ? MenuItemFlag_Checked : MenuItemFlag_None);
    }

    menu->SetCommandHandler([closeTabOrFloatingWindow, floatingCtx](uint32_t cmd) {
        if (cmd == kCloseTab)
        {
            closeTabOrFloatingWindow();
        }
        else if (cmd == kPinOnTop && floatingCtx && floatingCtx->window)
        {
            bool current = floatingCtx->window->IsAlwaysOnTop();
            floatingCtx->window->SetAlwaysOnTop(!current);
        }
    });

    menu->Show(window, static_cast<int>(x), static_cast<int>(y));
}

void EditorApplication::ResetLayoutToDefault()
{
    if (m_Windows.empty())
        return;
    auto& main = m_Windows[0];
    if (!main || !main->ui)
        return;

    // If we have a default preset, recall it so the toolbar highlight stays in sync.
    if (!m_LayoutPresets.empty() && m_LayoutPresets[0].layout)
    {
        RecallLayoutPreset(0);
        return;
    }

    // Fallback: restore the editor-authored default docking tree (parsed from layout.uxml at startup).
    // If no default was captured, defer to the generic DockspaceElement default.
    if (m_Docking && m_DefaultDockLayout)
    {
        m_Docking->SetRoot(CloneDockNode(m_DefaultDockLayout.get()));
        m_ActiveLayoutPresetIndex = 0;
        SyncLayoutPresetToolbar();
    }

    if (auto* rootEl = main->ui->GetRootElement())
    {
        if (auto* el = rootEl->FindById("dock"))
        {
            if (auto* ds = dynamic_cast<DockspaceElement*>(el))
            {
                if (!m_Docking || !m_DefaultDockLayout)
                {
                    // Best-effort fallback when no editor-specific default is available.
                    ds->ResetLayoutDefault();
                }
                ds->RequestRebuildFromModel();
            }
        }
    }
}

void EditorApplication::OpenPanel(const std::string& panelId, const std::string& preferredLeafId)
{
    if (m_Windows.empty() || !m_PanelManager)
        return;
    auto* main = m_Windows[0].get();
    if (!main)
        return;

    if (preferredLeafId.empty())
        m_PanelManager->ShowOrActivatePanelAtDefaultPlacement(main, panelId);
    else
        m_PanelManager->ShowOrActivatePanel(main, panelId, preferredLeafId);
}

bool EditorApplication::CanUndo() const
{
    return m_UndoRedo && m_UndoRedo->CanUndo();
}

bool EditorApplication::CanRedo() const
{
    return m_UndoRedo && m_UndoRedo->CanRedo();
}

const char* EditorApplication::GetUndoActionName() const
{
    return m_UndoRedo ? m_UndoRedo->PeekUndoName() : nullptr;
}

const char* EditorApplication::GetRedoActionName() const
{
    return m_UndoRedo ? m_UndoRedo->PeekRedoName() : nullptr;
}

void EditorApplication::Undo()
{
    if (m_UndoRedo)
    {
        m_UndoRedo->Undo();
    }
}

void EditorApplication::Redo()
{
    if (m_UndoRedo)
    {
        m_UndoRedo->Redo();
    }
}

// Toolbar drag-and-drop implementation moved to ToolbarDragDrop helper class

void EditorApplication::UpdatePlayModeToolbar(GameEngine::Editor::PlayModeState s)
{
    // Delegate button visual updates to the EditorTopToolbar helper.
    if (m_TopToolbar)
        m_TopToolbar->UpdatePlayModeState(s);

    // Show change review modal when leaving play with pending changes.
    if (s == GameEngine::Editor::PlayModeState::ChangeReview)
    {
        if (m_PlayModeChangeReviewModal && m_PlayMode)
        {
            auto names = m_PlayMode->GetPendingChangeNames();
            m_PlayModeChangeReviewModal->Show(names);
        }
    }
    else if (m_PlayModeChangeReviewModal)
    {
        // Toolbar, shortcut and IPC exits bypass the modal's button callbacks.
        m_PlayModeChangeReviewModal->Hide();
    }
}

// OnAssetChanged removed: UI hot reload is handled by AssetManager + UIHotReload.

} // namespace GameEngine
