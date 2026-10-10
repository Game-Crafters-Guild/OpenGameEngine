#include "ColorPicker/ColorPickerWindow.h"
#include "ColorPicker/ColorPickerEditorIntegration.h"
#include "ColorPicker/ColorPickerContext.h"
#include "ColorPicker/ColorPickerWindowInput.h"
#include "Panels/ColorPicker.h"
#include "Editor/Settings/EditorHiDpiPlatformSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/UIAccentStyleHelper.h"
#include "UI/UICursorHelper.h"
#include "Core/WindowInputRouter.h"
#include "Core/WindowPlatformApi.h"
#include "AssetCore/AssetTypes.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Platform/Display.h"
#include "Platform/Environment.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VkValidationRequest.h"
#include "GameViewController.h"
#include "SceneViewController.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{
uint64_t NextStandaloneWindowId()
{
    // Keep standalone-created ids in a high range to avoid collisions with
    // EditorApplication-assigned ids during attach.
    static std::atomic<uint64_t> s_Next{1ull << 48};
    return s_Next.fetch_add(1, std::memory_order_relaxed);
}

ColorPicker* FindDialogPicker(UIManager* ui)
{
    if (!ui)
        return nullptr;
    UIElement* root = ui->GetRootElement();
    UIElement* pickerEl = root ? root->FindById("colorpicker-dialog-picker") : nullptr;
    return pickerEl ? dynamic_cast<ColorPicker*>(pickerEl) : nullptr;
}

void StopEyedropper(ColorPickerContext* cpCtx)
{
    if (!cpCtx)
        return;
    cpCtx->eyedropperActive = false;
    if (cpCtx->eyedropperMonitorsArmed)
    {
        Platform::Window::StopGlobalMoveMonitor();
        Platform::Window::StopGlobalClickMonitor();
        Platform::Window::PopGlobalCursor();
        cpCtx->eyedropperMonitorsArmed = false;
    }
}
} // namespace

std::unique_ptr<EditorApplication::EditorWindowContext> ColorPickerWindow::CreateWindowContext(
    uint32_t initialArgb,
    float initialIntensity,
    Callbacks callbacks,
    const Params& params)
{
    if (!params.assetManager)
    {
        Logger::Log::Error("ColorPickerWindow: assetManager required");
        return nullptr;
    }

    // Picker layout has fixed CSS-logical dimensions; when Additional UI Scale > 100%
    // the effective logical viewport (window_logical / multiplier) shrinks below
    // the picker's natural width and the layout overflows. Scale the window up
    // by the multiplier so the logical viewport stays at the picker's natural size.
    const float hidpiMult = Editor::GetSavedHiDpiContentScaleMultiplier();
    const int kWidth = static_cast<int>(std::lround(512.0f * hidpiMult));
    const int kHeight = static_cast<int>(std::lround(512.0f * hidpiMult));

    // Place tool windows on the same display as their owner/main editor window.
    Platform::MonitorInfo ownerMonitor = params.ownerWindow
        ? Platform::GetMonitorInfoForWindow(params.ownerWindow->GetGLFWHandle())
        : Platform::GetPrimaryMonitorInfo();
    int workX = ownerMonitor.workX;
    int workY = ownerMonitor.workY;
    int workW = ownerMonitor.workWidth;
    int workH = ownerMonitor.workHeight;
    if (workW <= 0 || workH <= 0)
        Platform::GetPrimaryMonitorWorkArea(workX, workY, workW, workH);
    int centerX = workX + (workW - kWidth) / 2;
    int centerY = workY + (workH - kHeight) / 2;
    if (params.openNearCursor)
    {
        int cursorX = centerX;
        int cursorY = centerY;
        Platform::Window::GetCursorScreenPosition(cursorX, cursorY);
        constexpr int kCursorOffsetPx = 96;

        const int workMidX = workX + (workW / 2);
        const int workMidY = workY + (workH / 2);

        // Prefer opening away from the source region: controls on the right
        // (e.g. Inspector) open the picker to the left of the cursor.
        if (cursorX >= workMidX)
            centerX = cursorX - kWidth - kCursorOffsetPx;
        else
            centerX = cursorX + kCursorOffsetPx;

        // Place below cursor by default; if near bottom edge, place above.
        if (cursorY >= workMidY)
            centerY = cursorY - kHeight - kCursorOffsetPx;
        else
            centerY = cursorY + kCursorOffsetPx;

        centerX = std::clamp(centerX, workX, workX + std::max(0, workW - kWidth));
        centerY = std::clamp(centerY, workY, workY + std::max(0, workH - kHeight));
    }

    auto ctx = std::make_unique<EditorApplication::EditorWindowContext>();
    ctx->windowId = NextStandaloneWindowId();
    ctx->role = EditorApplication::WindowRole::Dialog;
    ctx->capabilities.worldRender = false;
    ctx->capabilities.uiOnly = true;
    ctx->capabilities.thumbnails = false;
    ctx->capabilities.dockingHost = false;
    ctx->width = kWidth;
    ctx->height = kHeight;
    ctx->title = "Color Picker";
    ctx->window = std::make_unique<Platform::Window>();
    // On macOS, create hidden and show after the first frame to avoid a gray flash.
    constexpr bool kStartHidden =
#if defined(__APPLE__)
        true;
#else
        false;
#endif
    if (!ctx->window->Create({.Title = ctx->title, .Width = kWidth, .Height = kHeight, .ToolWindow = true, .StartHidden = kStartHidden}))
    {
        Logger::Log::Error("Editor: Failed to create color picker window");
        return nullptr;
    }
    ctx->window->SetPosition(centerX, centerY);
    ctx->window->SetResizable(false);
    ctx->window->SetAlwaysOnTop(true);
    if (params.ownerWindow)
    {
        ctx->window->SetOwnedBy(params.ownerWindow);
        ctx->window->SetShowInTaskbar(false);
    }

    int fbW = 0, fbH = 0;
    ctx->window->GetFramebufferSize(fbW, fbH);
    ctx->width = (fbW > 0) ? fbW : kWidth;
    ctx->height = (fbH > 0) ? fbH : kHeight;

    // Consumed only if this context ever creates its own device; today it always
    // attaches the shared one below, so these values never reach vkCreateInstance.
    // If that path is revived, applicationName alone puts this window on a second
    // VkInstance (it is part of SharedInstanceKey) — match the editor's name unless
    // a separate instance is what is wanted.
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Auto;
    desc.enableSwapchain = true;
    desc.applicationName = "Color Picker";
    if (Engine::Renderer::ShouldEnableVkValidation())
    {
        desc.enableDebugLayer = true;
    }
    ctx->renderCtx = std::make_unique<Engine::Renderer::RenderDeviceContext>();
    if (!ctx->renderCtx)
    {
        Logger::Log::Error("Editor: Failed to allocate RenderDeviceContext for color picker window");
        return nullptr;
    }
    Engine::Renderer::RenderDeviceContext::InitParams renderInit{};
    renderInit.deviceDesc = desc;
    auto* sharedRs = EngineCore::GetInstance().GetRenderServices();
    if (!sharedRs || !sharedRs->GetDevice())
    {
        Logger::Log::Error("Editor: Cannot create color picker window without shared render device root");
        return nullptr;
    }
    else
    {
        renderInit.sharedDevice = sharedRs->GetDevice();
    }
    renderInit.windowHandle = ctx->window->GetNativeHandle();
    renderInit.width = static_cast<uint32>(ctx->width);
    renderInit.height = static_cast<uint32>(ctx->height);
    if (!ctx->renderCtx->Initialize(renderInit))
    {
        Logger::Log::Error("Editor: Failed to init RenderDeviceContext for color picker window");
        return nullptr;
    }

    ctx->ui = std::make_unique<UIManager>(
        ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr, params.assetManager);
    if (!ctx->ui)
    {
        Logger::Log::Error("Editor: Failed to create UIManager for color picker window");
        return nullptr;
    }
    ctx->ui->SetJobSystem(&EngineCore::GetInstance().GetJobSystem());
    EditorApplication::EditorWindowContext* ctxRaw = ctx.get();

    if (ctx->window)
    {
        ctx->uiPlatform = std::make_unique<WindowPlatformApi>(ctx->window.get());
        ctx->ui->SetPlatform(ctx->uiPlatform.get());
        Editor::ApplySavedHiDpiPlatformSettings(ctx->uiPlatform.get());

        // Set cursor callback for color picker window.
        SetupUICursorCallback(ctx->ui.get(), ctx->window.get());
    }

    UIManager::RenderRuntimeConfig uiRuntimeConfig = params.uiRenderConfig.has_value()
                                                         ? *params.uiRenderConfig
                                                         : ctx->ui->GetRenderRuntimeConfig();
    // Native picker windows default to a safer runtime profile unless the caller explicitly provides one.
    // This isolates the picker from editor-wide fast-path policy toggles while still allowing overrides.
    const bool forcePickerCorrectness = Platform::EnvironmentSwitchEnabled("GE_COLOR_PICKER_FORCE_UI_CORRECTNESS", true);
    if (!params.uiRenderConfig.has_value() && forcePickerCorrectness)
    {
        uiRuntimeConfig.CorrectnessModeEnabled = true;
        uiRuntimeConfig.DisableFastPathNoOp = true;
    }

    ctx->ui->ApplyRenderRuntimeConfig(uiRuntimeConfig);
    ctx->uiRuntimeConfigOverride = uiRuntimeConfig;
    Logger::Log::Info("[ColorPicker] UI runtime: correctness={} fastNoOp={}",
                      uiRuntimeConfig.CorrectnessModeEnabled ? "ON" : "OFF",
                      uiRuntimeConfig.DisableFastPathNoOp ? "OFF" : "ON");

    if (Platform::EnvironmentSwitchEnabled("GE_COLOR_PICKER_UI_DEBUG_CAPTURE", false))
    {
        if (!ctx->ui->IsDebugCaptureEnabled())
            ctx->ui->ToggleDebugCapture();
        Logger::Log::Info("[ColorPicker] UI debug capture enabled (GE_COLOR_PICKER_UI_DEBUG_CAPTURE)");
    }

    GUID styleGuid = params.styleGuid;
    if (styleGuid.IsNull() && params.assetManager)
    {
        // Fallback to the editor theme so native picker controls (sliders/buttons) are styled
        // the same way as in-window popup pickers.
            styleGuid = params.assetManager->ResolveAssetGuid(std::filesystem::path("UI") / "theme.css",
                                                          GameEngine::kAssetSourceAliasEditor);
    }

    if (!styleGuid.IsNull())
    {
        // Waits on purpose: the editor's own theme .css, small and never cooked.
        auto f = params.assetManager->LoadAssetAsync(styleGuid);
        auto a = f.get();
        if (a && a->GetType() == AssetType::UIStyle)
        {
            ctx->ui->AttachStyleFromAsset(*static_cast<UIStyleAsset*>(a.get()));
        }
    }
    
    // Load ColorPickerWindow-specific styles
    {
        static const char* kColorPickerWindowCSS =
            ".colorpicker-window-root { position: absolute; left: 0; top: 0; width: 100%; height: 100%; background-color: #252526; } "
            ".colorpicker-window-panel { display: flex; flex-direction: column; justify-content: space-between; align-items: stretch; gap: 12px; padding: 16px; background-color: #252526; width: 100%; height: 100%; } "
            ".colorpicker-picker-container { flex: 0 0 auto; overflow: visible; } "
            ".colorpicker-button-row { display: flex; flex-direction: row; justify-content: flex-end; align-items: center; gap: 12px; margin-bottom: 8px; margin-left: 8px; margin-right: 8px; } "
            ".colorpicker-button-group { display: flex; flex-direction: row; justify-content: flex-end; gap: 12px; } "
            ".colorpicker-apply-btn { min-width: 130px; padding: 8px 20px; color: #ffffff; border: none; border-radius: 6px; font-size: 14px; text-align: center; } "
            ".colorpicker-cancel-btn { min-width: 130px; padding: 8px 20px; background-color: #555; color: #ffffff; border: none; border-radius: 6px; font-size: 14px; text-align: center; } "
            ".colorpicker-cancel-btn:hover { background-color: #666; } "
            ".colorpicker-cancel-btn:active { background-color: #444; }";
        
        auto windowSheet = std::make_shared<Stylesheet>();
        if (UIParsing::CSSParser::ParseStylesFromString(kColorPickerWindowCSS, *windowSheet))
        {
            ctx->ui->AddStylesheet(windowSheet);
        }
    }

    // Apply accent color from preferences using the shared helper
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        uint32_t accentColor = UI::AccentStyleHelper::kDefaultAccentColor;
        int64_t stored = static_cast<int64_t>(accentColor);
        if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
            accentColor = static_cast<uint32_t>(stored);

        if (auto accentSheet = UI::AccentStyleHelper::BuildAccentColorStylesheet(accentColor))
            ctx->ui->AddStylesheet(accentSheet);
    }

    if (auto* cpCtx = ColorPickerEditorIntegration::GetOrCreateContext(ctxRaw))
    {
        cpCtx->onApply = std::move(callbacks.onApply);
        cpCtx->onCancel = std::move(callbacks.onCancel);
        cpCtx->onChange = std::move(callbacks.onValueChanging);
        cpCtx->ownerUi = ctxRaw ? ctxRaw->ui.get() : nullptr;
        cpCtx->window = ctxRaw ? ctxRaw->window.get() : nullptr;
    }

    ColorPickerValue initial = ColorPickerValueFromArgb(initialArgb);
    initial.intensity = initialIntensity;
    auto picker = std::make_unique<ColorPicker>();
    picker->SetId("colorpicker-dialog-picker");
    picker->SetValue(initial);
    picker->SetOnChange([ctxRaw](const ColorPickerValue& v)
                        {
                            ColorPickerContext* cpCtx = ctxRaw ? ColorPickerEditorIntegration::GetContext(ctxRaw) : nullptr;
                            if (cpCtx && cpCtx->onChange)
                            {
                                cpCtx->onChange(ColorPickerValueToArgb(v), v.intensity);
                            }
                        });

    // Set eyedropper handler: when clicked, enter eyedropper mode
    picker->SetEyedropperHandler([ctxRaw]()
    {
        if (!ctxRaw || !ctxRaw->window)
            return;
        const uint64_t windowId = ctxRaw->windowId;
        auto* cpCtx = ColorPickerEditorIntegration::GetOrCreateContext(ctxRaw);
        if (!cpCtx)
            return;
        cpCtx->ownerUi = ctxRaw->ui.get();

        // Save the current picker color so Escape can revert it.
        if (ColorPicker* pickerControl = FindDialogPicker(cpCtx->ownerUi))
            cpCtx->eyedropperStartArgb = ColorPickerValueToArgb(pickerControl->GetValue());
        cpCtx->eyedropperActive = true;
        cpCtx->eyedropperMonitorsArmed = true;
        
        // Use global cursor so it persists even when mouse is outside our window
        Platform::Window::PushGlobalCrosshairCursor();
        
        // Start global move monitoring for live color preview
        Platform::Window::StartGlobalMoveMonitor([windowId](int screenX, int screenY)
        {
            auto* cpState = ColorPickerEditorIntegration::GetContext(windowId);
            if (!cpState || !cpState->eyedropperActive || !cpState->ownerUi)
                return;
            
            // Sample screen color for live preview
            uint32_t color = Platform::Window::GetScreenPixelColor(screenX, screenY);
            
            // Update picker preview (without committing)
            if (ColorPicker* pickerControl = FindDialogPicker(cpState->ownerUi))
                pickerControl->SetColorFromArgb(color);
        });
        
        // Start global click monitoring (needed for clicking outside our window on macOS)
        Platform::Window::StartGlobalClickMonitor([windowId](int screenX, int screenY)
        {
            auto* cpState = ColorPickerEditorIntegration::GetContext(windowId);
            if (!cpState || !cpState->eyedropperActive || !cpState->ownerUi)
                return;
            
            // Sample screen color
            uint32_t color = Platform::Window::GetScreenPixelColor(screenX, screenY);
            
            // Deactivate eyedropper
            StopEyedropper(cpState);
            
            // Apply final color to picker
            if (ColorPicker* pickerControl = FindDialogPicker(cpState->ownerUi))
                pickerControl->SetColorFromArgb(color);
        });
    });

    // SV gradient and hue bar are rendered via OnGeneratePrimitives,
    // so no texture update callbacks are needed.

    auto buttonRow = std::make_unique<UIElement>();
    buttonRow->AddClass("colorpicker-button-row");
    auto buttonGroup = std::make_unique<UIElement>();
    buttonGroup->AddClass("colorpicker-button-group");
    auto applyBtn = std::make_unique<Button>();
    applyBtn->SetText("Apply");
    applyBtn->AddClass("primary");
    applyBtn->AddClass("colorpicker-apply-btn");
    applyBtn->RegisterEventHandler(kEventButtonClick, [ctxRaw](UIEvent&)
                        {
                            if (!ctxRaw || !ctxRaw->ui)
                                return;
                            UIElement* root = ctxRaw->ui->GetRootElement();
                            UIElement* pickerEl = root ? root->FindById("colorpicker-dialog-picker") : nullptr;
                            ColorPicker* pickerControl = pickerEl ? dynamic_cast<ColorPicker*>(pickerEl) : nullptr;
                            ColorPickerContext* cpCtx = ctxRaw ? ColorPickerEditorIntegration::GetContext(ctxRaw) : nullptr;
                            if (pickerControl && cpCtx && cpCtx->onApply)
                            {
                                const ColorPickerValue pv = pickerControl->GetValue();
                                cpCtx->onApply(ColorPickerValueToArgb(pv), pv.intensity);
                                cpCtx->onApply = nullptr;
                                cpCtx->onCancel = nullptr;
                                cpCtx->onChange = nullptr;
                            }
                            if (ctxRaw->window)
                                ctxRaw->window->RequestClose();
                        });
    auto cancelBtn = std::make_unique<Button>();
    cancelBtn->SetText("Cancel");
    cancelBtn->AddClass("colorpicker-cancel-btn");
    cancelBtn->RegisterEventHandler(kEventButtonClick, [ctxRaw](UIEvent&)
                         {
                             ColorPickerContext* cpCtx = ctxRaw ? ColorPickerEditorIntegration::GetContext(ctxRaw) : nullptr;
                             if (cpCtx && cpCtx->onCancel)
                             {
                                 cpCtx->onCancel();
                                 cpCtx->onApply = nullptr;
                                 cpCtx->onCancel = nullptr;
                                 cpCtx->onChange = nullptr;
                             }
                             if (ctxRaw && ctxRaw->window)
                                 ctxRaw->window->RequestClose();
                         });
    buttonGroup->AddChild(std::move(cancelBtn));
    buttonGroup->AddChild(std::move(applyBtn));
    buttonRow->AddChild(std::move(buttonGroup));

    auto panel = std::make_unique<UIElement>();
    panel->AddClass("colorpicker-window-panel");
    panel->AddChild(std::move(picker));
    panel->AddChild(std::move(buttonRow));

    auto root = std::make_unique<UIElement>();
    root->AddClass("colorpicker-window-root");
    root->AddChild(std::move(panel));
    ctx->ui->SetRoot(std::move(root));

    // On macOS the window was created hidden; show after first rendered frame.
    ctx->pendingShow = kStartHidden;

    // One router per window: this dialog walks the same chain every other editor
    // window does, so it also gets the focus-loss reset that clears modifiers no
    // release will ever arrive for. Its chain is one stage long — the dialog's
    // own UI — because a colour picker hosts no gameplay and no editor actions.
    const uint64_t windowId = ctxRaw->windowId;
    ColorPickerWindowInputSources inputSources;
    inputSources.getUi = [ctxRaw]() -> UIManager* { return ctxRaw ? ctxRaw->ui.get() : nullptr; };
    inputSources.isUiReplayActive = params.isUiReplayActive;
    inputSources.isEyedropperActive = [windowId]()
    {
        const ColorPickerContext* cpState = ColorPickerEditorIntegration::GetContext(windowId);
        return cpState && cpState->eyedropperActive;
    };
    inputSources.cancelEyedropper = [windowId]()
    {
        ColorPickerContext* cpState = ColorPickerEditorIntegration::GetContext(windowId);
        if (!cpState)
            return;
        StopEyedropper(cpState);
        // The sample was previewed into the picker as the cursor moved, so
        // cancelling puts back the value it held when sampling started; the
        // control's change callback carries that revert on to whoever is
        // previewing the colour.
        if (ColorPicker* pickerControl = FindDialogPicker(cpState->ownerUi))
            pickerControl->SetColorFromArgb(cpState->eyedropperStartArgb);
    };
    ctxRaw->inputConfig = MakeColorPickerWindowInputConfig(ctx->window.get(), std::move(inputSources));
    WindowInputRouter::BindBasicHandlers(ctxRaw->inputConfig);
    ctx->window->SetFramebufferSizeHandler([ctxRaw](int width, int height)
                                           {
                                               if (!ctxRaw)
                                                   return;
                                               const int prevW = ctxRaw->width;
                                               const int prevH = ctxRaw->height;
                                               ctxRaw->width = width;
                                               ctxRaw->height = height;
                                               if (width <= 0 || height <= 0)
                                                   return;
                                               if (width == prevW && height == prevH)
                                                   return;
                                               if (ctxRaw->renderCtx)
                                               {
                                                   ctxRaw->renderCtx->RecreateWindowTargetSwapchain(
                                                       static_cast<uint32_t>(width),
                                                       static_cast<uint32_t>(height));
                                               }
                                           });

    return ctx;
}

void ColorPickerWindow::Close(uint64_t windowId)
{
    const ColorPickerContext* cpCtx = ColorPickerEditorIntegration::GetContext(windowId);
    if (cpCtx && cpCtx->window)
        cpCtx->window->RequestClose();
}

} // namespace GameEngine
