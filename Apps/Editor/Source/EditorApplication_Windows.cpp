// Floating/native tool window lifecycle for EditorApplication: undocking a
// panel into its own OS window (platform window + render device context + UI
// bootstrap + controllers) and attaching externally created tool windows.
// Split from EditorApplication.cpp; shared bootstrap helpers live in
// EditorWindowBootstrap.h.
#include "EditorApplication.h"

#include "Core/Engine.h"
#include "Core/WindowInputRouter.h"
#include "Core/WindowUiBootstrap.h"
#include "Display/HdrOutputController.h"
#include "Docking/WindowDockingController.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "EditorPanelManager.h"
#include "EditorWindowBootstrap.h"
#include "Core/WindowPlatformApi.h"
#include "Editor/Settings/EditorHiDpiPlatformSettings.h"
#include "Editor/Settings/RenderPipelineSettings.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Logger/Logger.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VkValidationRequest.h"
#include "Input/KeyCodes.h"
#include "Panels/GameViewPanel.h"
#include "Panels/InspectorPanel.h"
#include "Rendering/Core/Device.h"
#include "Platform/Capabilities.h"
#include "Platform/Display.h"
#include "Platform/Window.h"
#include "GameViewController.h"
#include "SceneViewController.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/DockTab.h"
#include "UI/Layout/Docking.h"
#include "UI/UIManager.h"
#include "Input/InputSystem.h"
#include "Panels/SettingsPanel.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "UI/EditorUIFontSettings.h"
#include "UI/UICursorHelper.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{
using namespace Rendering;
using namespace UI;

void EditorApplication::QueueUndockPanel(std::string panelId)
{
    if (panelId.empty())
    {
        return;
    }
    // A floating panel is an OS window where the platform has more than one.
    // With a single window the docking layer presents it as an in-canvas frame.
    if (!Platform::SupportsMultipleWindows())
    {
        m_DockDnd->FloatingFrames().QueueTearOff(std::move(panelId));
        return;
    }
    const std::string key = std::string("editor.undock:") + panelId;
    m_DeferredPreUiActions.EnqueueUnique(key, [this, panelId = std::move(panelId)]()
                                         { this->UndockToFloatingWindow(panelId); });
}

void EditorApplication::QueueNativeToolWindow(std::unique_ptr<EditorWindowContext> ctx)
{
    if (!ctx)
    {
        return;
    }

    m_DeferredNativeToolWindows.emplace_back(std::move(ctx));
    m_DeferredPreUiActions.EnqueueUnique("editor.attach-native-tool-windows",
                                         [this]()
                                         { this->AttachDeferredNativeToolWindows(); });
}

void EditorApplication::DismissTornOffPanelsForLayoutRestore()
{
    m_DockDnd->FloatingFrames().DismissAll();

    for (size_t i = m_Windows.size(); i-- > 1;)
    {
        EditorWindowContext* ctx = m_Windows[i].get();
        if (!ctx || ctx->floatingPanelId.empty())
            continue;
        if (ctx->docking)
            ctx->docking->RemoveTab(ctx->floatingPanelId);
        if (ctx->ui)
        {
            ctx->ui->ClearHover();
            ctx->ui->ClearFocus();
        }
        EditorPanelManager::RebuildDockspaceNow(ctx, ctx->docking);
        if (ctx->window)
            ctx->window->RequestClose();
    }
}

void EditorApplication::AttachDeferredNativeToolWindows()
{
    GE_CPU_PROFILE_SCOPE("EditorApplication.Update.AttachPendingNativeToolWindows");

    auto pending = std::move(m_DeferredNativeToolWindows);
    m_DeferredNativeToolWindows.clear();

    for (auto& win : pending)
    {
        if (!win)
            continue;

        if (win->windowId == 0)
        {
            win->windowId = m_NextWindowId++;
        }
        if (win->ui)
        {
            if (win->uiRuntimeConfigOverride.has_value())
            {
                win->ui->ApplyRenderRuntimeConfig(*win->uiRuntimeConfigOverride);
            }
            else
            {
                ApplyUiRuntimeConfig(win->ui.get());
            }
        }
        m_Windows.emplace_back(std::move(win));
        EditorWindowContext* attached = m_Windows.back().get();
        // Match UndockToFloatingWindow: shared-device windows must activate their swapchain target
        // before UIManager can measure against the correct dimensions; without a prime Update,
        // the first presented frame can show cleared/uninitialized color or stale compositor content.
        if (attached && attached->renderCtx && attached->ui)
        {
            (void)attached->renderCtx->ActivateWindowTarget();
            attached->ui->Update(0.0f, /*interactive=*/false);
        }
    }
}

void EditorApplication::UndockToFloatingWindow(const std::string& panelId)
{
    // Find panel content in editor storage
    UIElement* panel = m_Docking ? m_Docking->GetPanel(panelId) : nullptr;
    if (!panel)
    {
        Logger::Log::Warning("Editor: UndockToFloatingWindow - unknown panel '{}'; aborting", panelId);
        return;
    }

    // Size the new floating window to match the current docked leaf size (tabbar + content),
    // so tearing off feels like "lifting" the panel out of the layout.
    // GetLayoutWidth/Height return CSS-logical pixels (Yoga's space, shrunk by the
    // Additional UI Scale multiplier). Platform::Window::Create expects client-area
    // size in screen coords (DIP), which equals UI logical * multiplier. We convert
    // via UiLogicalToClientScale so the floating window opens at the same on-screen
    // size as the docked leaf at every UI scale.
    int desiredLogicalW = 960;
    int desiredLogicalH = 600;
    int desiredWinW = 960;
    int desiredWinH = 600;
    if (!m_Windows.empty())
    {
        EditorWindowContext* mainCtx = m_Windows[0].get();
        if (mainCtx && mainCtx->ui && mainCtx->window)
        {
            if (UIElement* rootEl = mainCtx->ui->GetRootElement())
            {
                // Prefer the leaf size that the tab lived in. This works even when
                // the tab is not currently active (so no mount exists).
                UIElement* leaf = nullptr;
                if (UIElement* tabEl = rootEl->FindById(std::string("tab:") + panelId))
                {
                    UIElement* p = tabEl;
                    while (p && !p->HasClass("dock-leaf"))
                        p = p->GetParent();
                    leaf = p;
                }
                if (!leaf)
                {
                    // Fallback to the active mount path (works when the panel is active).
                    if (UIElement* mount = rootEl->FindById(std::string("mount:") + panelId))
                    {
                        UIElement* content = mount->GetParent();
                        leaf = content ? content->GetParent() : nullptr;
                    }
                }

                if (leaf)
                {
                    const float lw = leaf->GetLayoutWidth();
                    const float lh = leaf->GetLayoutHeight();
                    if (lw > 50.0f && lh > 50.0f)
                    {
                        desiredLogicalW = (int)std::lround(lw);
                        desiredLogicalH = (int)std::lround(lh);
                    }
                }
                else
                {
                    // Last resort: use the panel's last layout rect (content-only).
                    const float pw = panel->GetLayoutWidth();
                    const float ph = panel->GetLayoutHeight();
                    if (pw > 50.0f && ph > 50.0f)
                    {
                        desiredLogicalW = (int)std::lround(pw);
                        desiredLogicalH = (int)std::lround(ph);
                    }
                }
            }

            const float scale = WindowInputRouter::UiLogicalToClientScale(mainCtx->window.get(), mainCtx->ui.get());
            desiredWinW = (int)std::lround((double)desiredLogicalW * (double)scale);
            desiredWinH = (int)std::lround((double)desiredLogicalH * (double)scale);
        }
    }
    // Clamp to sane minimums so tiny tabs don't create unusable windows.
    desiredWinW = std::max(320, desiredWinW);
    desiredWinH = std::max(240, desiredWinH);

    // The main dock still owns the tab here, and keeps owning it until the
    // floating window is fully built. Everything from here to that point can
    // fail — window creation, the render device, the swapchain — and a tab
    // removed ahead of a failure would belong to no window at all, with no way
    // to get it back from the UI.
    auto detachPanelFromMainDock = [this, &panelId]()
    {
        if (!m_Docking)
            return;
        m_Docking->RemoveTab(panelId);
        // IMPORTANT: ensure the panel is detached from the main window's UI tree
        // *before* we mount it into a floating window. UIElements store per-frame
        // layout/style state; mounting the same subtree in two UIManagers in the
        // same frame can cause visual corruption (especially text).
        if (!m_Windows.empty())
        {
            auto& main = m_Windows[0];
            if (main && main->ui)
            {
                EditorPanelManager::RebuildDockspaceNow(main.get(), m_Docking.get());
                // Clear hover/capture/focus so we don't keep pointers/ids to a removed tab.
                main->ui->ClearHover();
                main->ui->ClearFocus();
            }
        }
    };

    // Create a dedicated docking model for the floating window with this single tab
    auto floatDock = std::make_unique<DockingManager>();
    floatDock->RegisterPanel(panelId, panel);
    if (auto* ip = dynamic_cast<InspectorPanel*>(panel))
        ip->SetDockTabPanelId(panelId);
    auto root = DockNode::MakeLeaf();
    root->AddTab(panelId);
    floatDock->SetRoot(std::move(root));

    // Build a new window context
    auto ctx = std::make_unique<EditorWindowContext>();
    WindowDescriptor toolDesc{};
    toolDesc.role = WindowRole::Tool;
    toolDesc.capabilities.worldRender = true;
    toolDesc.capabilities.uiOnly = false;
    toolDesc.capabilities.thumbnails = true;
    toolDesc.capabilities.dockingHost = false;
    std::string floatingPanelTitle = panelId;
    if (auto* dockPanel = dynamic_cast<DockPanel*>(panel); dockPanel && !dockPanel->GetTitle().empty())
        floatingPanelTitle = dockPanel->GetTitle();
#if defined(__APPLE__)
    constexpr const char* kRedockTitleHint = "Option-drag onto Editor to dock again";
#else
    constexpr const char* kRedockTitleHint = "Alt-drag onto Editor to dock again";
#endif
    toolDesc.title = std::string("Editor - ") + floatingPanelTitle + " — " + kRedockTitleHint;
    toolDesc.panelId = panelId;
    ApplyWindowDescriptor(*ctx, toolDesc, m_NextWindowId++);
    ctx->width = 0;
    ctx->height = 0;
    ctx->window = std::make_unique<Platform::Window>();
    Platform::Window* ownerWindow = (!m_Windows.empty() && m_Windows[0]) ? m_Windows[0]->window.get() : nullptr;
    Platform::MonitorInfo ownerMonitor = ownerWindow
        ? Platform::GetMonitorInfoForWindow(ownerWindow->GetGLFWHandle())
        : Platform::GetPrimaryMonitorInfo();
    int toolX = ownerMonitor.workX + (ownerMonitor.workWidth - desiredWinW) / 2;
    int toolY = ownerMonitor.workY + (ownerMonitor.workHeight - desiredWinH) / 2;
    if (ownerMonitor.workWidth > 0 && ownerMonitor.workHeight > 0)
    {
        toolX = std::clamp(toolX, ownerMonitor.workX, ownerMonitor.workX + std::max(0, ownerMonitor.workWidth - desiredWinW));
        toolY = std::clamp(toolY, ownerMonitor.workY, ownerMonitor.workY + std::max(0, ownerMonitor.workHeight - desiredWinH));
    }
    if (!ctx->window->Create({.Title = ctx->title, .Width = desiredWinW, .Height = desiredWinH, .ToolWindow = true}))
    {
        Logger::Log::Error("Editor: Failed to create floating window for '{}'; panel stays docked",
                           panelId);
        return;
    }
    ctx->window->SetPosition(toolX, toolY);
    if (ownerWindow)
    {
        ctx->window->SetOwnedBy(ownerWindow);
        ctx->window->SetShowInTaskbar(m_FloatingShowInTaskbar);
    }
    if (m_FloatingFullCustomChrome)
    {
        ctx->window->SetDecorated(false);
    }

    // Capture actual framebuffer size (pixels) for swapchain creation.
    // This avoids creating a swapchain with DIP sizes on HiDPI monitors.
    {
        int fbW = 0, fbH = 0;
        ctx->window->GetFramebufferSize(fbW, fbH);
        ctx->width = (fbW > 0) ? fbW : std::max(1, desiredLogicalW);
        ctx->height = (fbH > 0) ? fbH : std::max(1, desiredLogicalH);
    }

    // Device & swapchain
    DeviceDesc desc{};
    desc.preferredAPI = LoadPreferredGraphicsApi();
    desc.enableSwapchain = true;
    desc.applicationName = GetConfig().Name;
    if (Engine::Renderer::ShouldEnableVkValidation())
    {
        desc.enableDebugLayer = true;
    }
    ctx->renderCtx = std::make_unique<GameEngine::Engine::Renderer::RenderDeviceContext>();
    if (!ctx->renderCtx)
    {
        Logger::Log::Error("Editor: Failed to allocate RenderDeviceContext for floating window '{}'; "
                           "panel stays docked", panelId);
        return;
    }

    GameEngine::Engine::Renderer::RenderDeviceContext::InitParams renderInit{};
    renderInit.deviceDesc = desc;
    auto* sharedRs = EngineCore::GetInstance().GetRenderServices();
    if (!sharedRs || !sharedRs->GetDevice())
    {
        Logger::Log::Error(
            "Editor: Cannot create floating window '{}' without shared render device root; "
            "panel stays docked",
            panelId);
        return;
    }
    else
    {
        renderInit.sharedDevice = sharedRs->GetDevice();
    }
    renderInit.windowHandle = ctx->window->GetNativeHandle();
    renderInit.width = static_cast<uint32>(ctx->width);
    renderInit.height = static_cast<uint32>(ctx->height);
    renderInit.createRenderServices = true;

    if (!ctx->renderCtx->Initialize(renderInit))
    {
        Logger::Log::Error("Editor: Failed to init RenderDeviceContext for floating window '{}'; "
                           "panel stays docked", panelId);
        return;
    }

    // Past every failure that would have stranded the panel: the floating window
    // owns a device and a swapchain, so the main dock can let the tab go.
    detachPanelFromMainDock();

    // Set the same render pipeline on the floating window's RenderServices so that
    // GPU-rendered panels (Game View, Scene View) draw with the project's pipeline.
    if (auto* floatRs = ctx->renderCtx->GetRenderServices())
    {
        const auto& wsRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        // A floating window owns its own RenderServices, so it needs the
        // project's renderer knobs applied to it directly — otherwise it would,
        // for instance, auto-select mesh LODs while the main window had
        // selection off.
        Editor::ApplyProjectRenderSettings(*floatRs, wsRoot);
        const std::filesystem::path pipelinePath = Editor::LoadActiveRenderPipelinePathFromProjectSettings(wsRoot);
        if (!pipelinePath.empty())
        {
            floatRs->Spine().SetActiveRenderPipelinePath(pipelinePath);
        }
        else
        {
            floatRs->Spine().SetActiveRenderPipelinePath(std::filesystem::path("RenderPipelines/ForwardPlus.rendergraph"));
        }
    }

    // UIManager and assets
    auto& am = EngineCore::GetInstance().GetAssetManager();
    ctx->ui = std::make_unique<UIManager>(ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr, &am);
    // Unconditional: native menus resolve their row icons through this too.
    UIContextMenu::Register(ctx->window.get(), ctx->ui.get());
    if (ctx->ui)
    {
        // Match main-window runtime configuration so torn-out windows render the
        // same geometry/text pipeline (instancing/correctness toggles, etc.).
        ApplyUiRuntimeConfig(ctx->ui.get());
        ConfigureEditorUiFonts(ctx->ui.get(), &am, m_AssetsDirectory);
        Editor::PrefetchEditorFontOptions(ctx->ui.get());
    }
    // Connect clipboard/platform hooks for this floating window.
    if (ctx->window)
    {
        ctx->uiPlatform = std::make_unique<WindowPlatformApi>(ctx->window.get());
        ctx->ui->SetPlatform(ctx->uiPlatform.get());
        Editor::ApplySavedHiDpiPlatformSettings(ctx->uiPlatform.get());

        // Set cursor callback for floating windows.
        SetupUICursorCallback(ctx->ui.get(), ctx->window.get());
    }
    // Floating window styling must be fully ready immediately; async-only style load can
    // leave torn windows visually blank/uninteractive for some users/build configs.
    GUID floatingStyleGuid = !m_StylePath.empty()
                                 ? am.ResolveAssetGuid(m_StylePath, GameEngine::kAssetSourceAliasEditor)
                                 : GUID::Null();
    if (floatingStyleGuid.IsNull())
    {
        floatingStyleGuid = m_StyleGuid;
        if (floatingStyleGuid.IsNull())
        {
            Logger::Log::Warning("Editor: floating '{}' failed to resolve style guid (alias='{}', asset='{}', resolved='{}')",
                                 panelId,
                                 std::string(GameEngine::kAssetSourceAliasEditor),
                                 m_StylePath.string(),
                                 am.ResolveAssetPath(m_StylePath, GameEngine::kAssetSourceAliasEditor).string());
        }
    }
    else
    {
        m_StyleGuid = floatingStyleGuid;
    }

    const std::string floatingLabel = std::string("floating:") + panelId;
    const WindowUiBootstrapResult floatingBootstrap = BootstrapEditorWindowUi(ctx->ui.get(),
                                                                               &am,
                                                                               GUID{},
                                                                               floatingStyleGuid,
                                                                               floatingLabel);
    Logger::Log::Info("Editor: floating '{}' bootstrap styleLoaded={} hasRoot={}",
                      panelId,
                      floatingBootstrap.styleLoaded,
                      floatingBootstrap.hasRoot);
    if (!floatingBootstrap.hasRoot)
    {
        Logger::Log::Error("Editor: floating window '{}' has no UI root after bootstrap", panelId);
    }

    // Defensive fallback: make sure the main editor theme stylesheet is attached even if the
    // bootstrap style load was skipped/failed for any reason.
    if (!floatingBootstrap.styleLoaded)
    {
        (void)EnsureEditorThemeStyleAttached(ctx->ui.get(),
                                             &am,
                                             m_StylePath,
                                             floatingStyleGuid,
                                             floatingLabel,
                                             GameEngine::kAssetSourceAliasEditor);
    }

    // Keep floating windows on the same accent palette as the main editor UI.
    // Apply after theme attach so accent overrides win by precedence.
    ApplySavedAccentStyle(ctx->ui.get());
    SettingsPanel::ApplySavedCompactComponentHeadersStyle(ctx->ui.get());
    SettingsPanel::ApplySavedHierarchyIconsColoredStyle(ctx->ui.get());
    SettingsPanel::ApplySavedHierarchyModelThumbsAlwaysColoredStyle(ctx->ui.get());
    SettingsPanel::ApplySavedGraySlidersStyle(ctx->ui.get());
    SettingsPanel::ApplySavedToggleStyle(ctx->ui.get());
    SettingsPanel::ApplySavedTabIconsStyle(ctx->ui.get());
    SettingsPanel::ApplySavedPopupShadowStyle(ctx->ui.get());
    Editor::InfoCardAppearanceSettings::Get().ApplyTo(ctx->ui.get());
    SettingsPanel::ApplySavedValueBoxHeightStyle(ctx->ui.get());
    SettingsPanel::ApplySavedNodeGraphHeaderAlignmentStyle(ctx->ui.get());
    SettingsPanel::ApplySavedTextSubpixelAA(ctx->ui.get());
    SettingsPanel::ApplySavedTextContrast(ctx->ui.get());
    SettingsPanel::ApplySavedTextSmoothingGamma(ctx->ui.get());
    Editor::ApplySavedEditorFontPreferences(ctx->ui.get());
    // After the font preferences: the line height resolves against the script
    // face those select.
    SettingsPanel::ApplySavedScriptLineHeightStyle(ctx->ui.get());
    if (UIElement* floatingRoot = ctx->ui ? ctx->ui->GetRootElement() : nullptr)
    {
        ctx->ui->MarkStyleDirtySubtree(floatingRoot);
        ctx->ui->RequestRelayout();
    }

    // Scene/Game view controllers for floating window use the MAIN RenderServices
    // so they share views, cameras, and draw submissions from ECS world rendering.
    auto* mainRs = (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->renderCtx)
                       ? m_Windows[0]->renderCtx->GetRenderServices()
                       : nullptr;

    ctx->scene = std::make_unique<SceneViewController>(mainRs, *EngineCore::GetInstance().EnsurePrimaryWorld());
    if (ctx->scene)
    {
        ctx->scene->SetUndoRedoService(m_UndoRedo.get());
        if (auto* tool = ctx->scene->GetTransformTool())
        {
            tool->SetUndoRedoService(m_UndoRedo.get());
            tool->SetChangeNotifications(m_ChangeNotifications.get());
        }
        ctx->scene->SetChangeNotifications(m_ChangeNotifications.get());
    }
    {
        const SceneViewController::FixedViewOrientation orientations[] = {
            SceneViewController::FixedViewOrientation::Top,
            SceneViewController::FixedViewOrientation::Front,
            SceneViewController::FixedViewOrientation::Side,
        };
        const char* prefixes[] = {"SceneView.Top", "SceneView.Front", "SceneView.Side"};
        for (size_t i = 0; i < ctx->sceneQuadViews.size(); ++i)
        {
            ctx->sceneQuadViews[i] = std::make_unique<SceneViewController>(mainRs, *EngineCore::GetInstance().EnsurePrimaryWorld());
            ctx->sceneQuadViews[i]->SetRenderNamePrefix(prefixes[i]);
            ctx->sceneQuadViews[i]->SetFixedViewOrientation(orientations[i]);
            ctx->sceneQuadViews[i]->SetUndoRedoService(m_UndoRedo.get());
            ctx->sceneQuadViews[i]->SetChangeNotifications(m_ChangeNotifications.get());
            if (auto* tool = ctx->sceneQuadViews[i]->GetTransformTool())
            {
                tool->SetUndoRedoService(m_UndoRedo.get());
                tool->SetChangeNotifications(m_ChangeNotifications.get());
            }
        }
    }

    ctx->gameView = std::make_unique<GameViewController>(mainRs, m_AssetsDirectory);

    // Mark this window as floating for styling, then bind its dockspace
    if (auto* rootEl = ctx->ui->GetRootElement())
    {
        rootEl->AddClass("floating");
        if (auto* el = rootEl->FindById("dock"))
        {
            if (auto* ds = dynamic_cast<DockspaceElement*>(el))
            {
                ds->SetOnPostRebuild([this]() {
                    for (const auto& panel : m_PanelStorage)
                    {
                        if (auto* ip = dynamic_cast<InspectorPanel*>(panel.get()))
                            ip->PostAction([ip]() { ip->EnsureInspectorTabLockMounted(); });
                    }
                });
                ds->SetOnTabContextMenu([this, rawCtx = ctx.get()](const std::string& panelId, float x, float y) {
                    ShowTabContextMenu(panelId, x, y, rawCtx->window.get());
                });
                EditorPanelManager::RebuildDockspaceNow(ctx.get(), floatDock.get());
            }
        }
    }

    // Floating windows are pinned on top by default
    if (ctx->window)
    {
        ctx->window->SetAlwaysOnTop(true);

        // Right-click on the native title bar shows a context menu with Pin on Top
        auto* rawCtx = ctx.get();
        ctx->window->SetTitleBarRightClickHandler([this, rawCtx](int screenX, int /*screenY*/) {
            if (!rawCtx->window)
                return;
            // Convert screen X to client-local X; Y=0 places menu at the top edge
            int wx = 0, wy = 0;
            rawCtx->window->GetPosition(wx, wy);
            ShowTabContextMenu(rawCtx->floatingPanelId,
                               static_cast<float>(screenX - wx), 0,
                               rawCtx->window.get());
        });
    }

    // Prime initial geometry for newly torn-out windows immediately so the first presented
    // frame is fully populated even if the very next main-loop UI update is skipped.
    if (ctx->renderCtx && ctx->ui)
    {
        (void)ctx->renderCtx->ActivateWindowTarget();
        ctx->ui->Update(0.0f, /*interactive=*/false);
    }

    ctx->dockingOwned = std::move(floatDock);
    ctx->docking = ctx->dockingOwned.get();

    // Route input for this window to UIManager and the shared Application InputSystem
    EditorWindowContext* ctxRaw = ctx.get();
    ctxRaw->floatingPanelId = panelId;
    WindowInputRouterConfig floatingInput{};
    floatingInput.window = ctx->window.get();
    floatingInput.getUi = [this, ctxRaw]() -> UIManager*
    {
        // Same replay gate as the main window: synthetic replay input only.
        if (m_UiReplayScenarioPath.has_value())
            return nullptr;
        return ctxRaw ? ctxRaw->ui.get() : nullptr;
    };
    floatingInput.getInput = [this]() -> Input::InputSystem*
    {
        return GetInputSystem();
    };
    floatingInput.onScrollPre = [this](float /*dx*/, float dy) -> bool
    {
        if (m_CssInspector.IsEnabled())
        {
            m_CssInspector.OnScrollWheel(dy);
            return true;
        }
        return false;
    };
    floatingInput.onKeyPre = [this, ctxRaw](int k, int a, int m) -> bool
    {
#if GE_ENABLE_CPU_PROFILING
        if (k == Input::kKeyCode_F2 && a == Input::kKeyActionPress)
        {
            auto& prof = GameEngine::Profiling::CpuProfiler::Get();
            if ((m & Input::kModShift) != 0)
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
                prof.RequestDump(this->BuildPerfDumpLine());
            }
            return true;
        }
#endif

        // Global editor hotkey: F11 toggles dock debug overlay regardless of UI focus.
        if (k == Input::kKeyCode_F11 && a == Input::kKeyActionPress)
        {
            this->ToggleDockDebugZones();
            return true;
        }

        if (this->TryHandleCatalogGlobalKeyPre(ctxRaw, k, a, m, false))
            return true;

        // Copy hovered element selector + style to clipboard. Key: C (no modifiers), only while CSS inspector is active.
        if (k == Input::kKeyCode_C && a == Input::kKeyActionPress && (m & (Input::kModControl | Input::kModShift | Input::kModAlt | Input::kModSuper)) == 0 &&
            ctxRaw->ui && m_CssInspector.IsEnabled())
        {
            if (UIElement* displayed = m_CssInspector.GetDisplayedElement(ctxRaw->ui.get()))
            {
                std::string s = m_CssInspector.GetSummaryIncludingPathForElement(ctxRaw->ui.get(), displayed);
                if (!s.empty() && ctxRaw->ui->GetPlatform())
                    ctxRaw->ui->GetPlatform()->SetClipboardText(s.c_str());
            }
            return true;
        }

#if defined(_DEBUG)
	        // Debug-only: export the current UI layout/styles for this floating window.
	        if (k == Input::kKeyCode_F12 && a == Input::kKeyActionPress)
	        {
	            this->DebugExportCurrentUILayout(ctxRaw);
	            return true;
	        }
#endif

        return false;
    };
    floatingInput.getPlaySurface = [this, floatWindow = ctx->window.get()]()
    { return GetPlaySurface(floatWindow); };
    ctxRaw->inputConfig = floatingInput;
    WindowInputRouter::BindBasicHandlers(ctxRaw->inputConfig);
    ctx->window->SetRefreshHandler([this] { Tick(); });
    ctx->window->SetPositionHandler([this, ctxRaw](int x, int y)
                                    { this->OnWindowMoved(ctxRaw, x, y); });
    ctx->window->SetFramebufferSizeHandler([this, ctxRaw](int width, int height)
                                           { this->OnFramebufferResized(ctxRaw, width, height); });
    ctx->window->SetMonitorChangedHandler([this, ctxRaw](int monitorIndex)
                                          { m_HdrOutput->OnWindowMonitorChanged(ctxRaw, monitorIndex); });
    ctx->window->SetFileDropHandler([this, ctxRaw](const std::vector<std::filesystem::path>& paths)
                                    { this->OnFilesDropped(ctxRaw, paths); });
    ctx->window->GetContentScale(ctxRaw->lastNativeContentScaleX,
                                 ctxRaw->lastNativeContentScaleY);

    // Add to windows list
    m_Windows.emplace_back(std::move(ctx));

    // The window target was born with the HDR request of whatever target was
    // active; evaluate this window's own monitor now that it is in the list the
    // controller ticks.
    m_HdrOutput->OnWindowCreated(ctxRaw);

    // Begin smooth tear-off drag: position new window under global cursor and follow until mouse released
    EditorWindowContext* sourceCtx = m_DispatchingWindow ? m_DispatchingWindow : (!m_Windows.empty() ? m_Windows[0].get() : nullptr);
    if (sourceCtx && sourceCtx->window)
    {
        EditorWindowContext* newCtx = m_Windows.back().get();
        int fl = 0, ft = 0, fr = 0, fb = 0;
        newCtx->window->GetFrameSize(fl, ft, fr, fb);
        int cursorX = 0, cursorY = 0;
        Platform::Window::GetCursorScreenPosition(cursorX, cursorY);
        // Place the new window so the cursor sits near the title area (~16,12 offset)
        float anchorX = 16.0f, anchorY = 12.0f;
        int targetX = static_cast<int>(cursorX - fl - anchorX);
        int targetY = static_cast<int>(cursorY - ft - anchorY);
        newCtx->window->SetPosition(targetX, targetY);
        // Activate tear-off tracking
        m_DockDnd->BeginTearOff(sourceCtx->window.get(), newCtx->window.get(), anchorX, anchorY);
    }
}

} // namespace GameEngine
