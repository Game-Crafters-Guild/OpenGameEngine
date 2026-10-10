#include "Display/HdrOutputController.h"

#include "Core/Engine.h"
#include "Display/HdrOutputMatch.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Logger/Logger.h"
#include "Platform/Display.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
struct EditorHdrOutputRequest
{
    bool enabled = false;
    Rendering::HdrOutputMode mode = Rendering::HdrOutputMode::Auto;
    Rendering::HdrSwapchainBitDepth swapchainBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
    int targetDisplay = -1;
    Rendering::HdrStaticMetadata metadata{};
    bool metadataExplicit = false;
    bool paperWhiteExplicit = false;
    bool autoTuneForDisplay = true;
};

static constexpr int kHdrTargetActiveWindowMonitor = -1;
static constexpr int kHdrTargetPrimaryMonitor = -2;

static bool LooksLikeLivingRoomHdrDisplay(std::string_view displayName)
{
    std::string lower;
    lower.reserve(displayName.size());
    for (const char c : displayName)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.find("tv") != std::string::npos ||
           lower.find("oled") != std::string::npos ||
           lower.find("hdr") != std::string::npos ||
           lower.find("hdmi") != std::string::npos;
}

static const char* HdrDisplayProfileName(const Platform::MonitorInfo& monitor)
{
    if (Platform::LooksLikeLGOledC7(monitor.name))
        return "LG OLED C7 HLG/HDR10+";
    if (LooksLikeLivingRoomHdrDisplay(monitor.name))
        return "TV HDR10";
    if (monitor.maxLuminance > 0.0f || monitor.maxFullFrameLuminance > 0.0f || monitor.minLuminance > 0.0f)
        return "display-reported HDR";
    return "project defaults";
}

static void TuneHdrMetadataForMonitor(Rendering::HdrStaticMetadata& metadata,
                                      const Platform::MonitorInfo& monitor,
                                      bool metadataExplicit,
                                      bool paperWhiteExplicit,
                                      bool autoTuneForDisplay)
{
    // Autotune only fills in what the settings did NOT set: explicit values
    // always win, and autoTuneForDisplay=false disables tuning entirely.
    if (!autoTuneForDisplay)
        return;

    const bool lgC7 = Platform::LooksLikeLGOledC7(monitor.name);
    const bool tvLike = lgC7 || LooksLikeLivingRoomHdrDisplay(monitor.name);

    if (!metadataExplicit)
    {
        if (monitor.maxLuminance > 0.0f)
        {
            metadata.maxMasteringLuminance = monitor.maxLuminance;
            metadata.maxContentLightLevel = monitor.maxLuminance;
        }
        else if (tvLike)
        {
            metadata.maxMasteringLuminance = 1000.0f;
            metadata.maxContentLightLevel = 1000.0f;
        }

        if (monitor.maxFullFrameLuminance > 0.0f)
            metadata.maxFrameAverageLightLevel = monitor.maxFullFrameLuminance;
        else if (tvLike)
            metadata.maxFrameAverageLightLevel = 400.0f;

        if (monitor.minLuminance > 0.0f)
            metadata.minMasteringLuminance = monitor.minLuminance;
        else if (tvLike)
            metadata.minMasteringLuminance = 0.001f;
    }

    if (!paperWhiteExplicit)
    {
        if (lgC7 || tvLike)
            metadata.paperWhiteNits = 203.0f;
        else if (monitor.sdrWhiteLevelNits > 0.0f)
            // Match the OS "SDR content brightness" so scene white reads at the same level as the
            // desktop. This is the correct paper-white for a desktop HDR monitor.
            metadata.paperWhiteNits = std::clamp(monitor.sdrWhiteLevelNits, Rendering::kSdrWhiteToPaperWhiteMinNits, Rendering::kSdrWhiteToPaperWhiteMaxNits);
        else if (monitor.maxFullFrameLuminance > 0.0f)
            metadata.paperWhiteNits = std::clamp(monitor.maxFullFrameLuminance * 0.5f, 200.0f, 350.0f);
    }
}

// GE_FORCE_HDR_OUTPUT is an operator/launch override: set explicitly, it beats
// the project setting so a launch recipe can pin the output mode without editing
// project files. Unset or empty leaves the project setting governing. An
// unrecognized spelling is refused rather than parsed as Off, because silently
// disabling HDR on a typo is exactly the failure this override exists to avoid.
static std::optional<Rendering::HdrOutputMode> ReadForcedHdrOutputMode()
{
    const char* forced = std::getenv("GE_FORCE_HDR_OUTPUT");
    if (!forced || forced[0] == '\0')
        return std::nullopt;

    const std::optional<Rendering::HdrOutputMode> mode = Rendering::TryParseHdrOutputMode(forced);
    if (!mode)
    {
        Logger::Log::Warning(
            "Editor: GE_FORCE_HDR_OUTPUT='{}' is not a recognized HDR output mode; ignoring it and using the "
            "project setting. Recognized values: Off, Auto, HDR10_PQ, HLG, scRGB, HDR10+.",
            forced);
        return std::nullopt;
    }
    return mode;
}

static EditorHdrOutputRequest LoadEditorHdrOutputRequest(const std::filesystem::path& workspaceRoot)
{
    EditorHdrOutputRequest request{};
    bool enabledExplicit = false;
    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
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
                if (hdr.contains("enabled") && hdr["enabled"].is_boolean())
                {
                    request.enabled = hdr["enabled"].get<bool>();
                    enabledExplicit = true;
                }
                if (hdr.contains("mode") && hdr["mode"].is_string())
                    request.mode = Rendering::HdrOutputModeFromString(hdr["mode"].get<std::string>());
                if (hdr.contains("swapchainBitDepth"))
                {
                    if (hdr["swapchainBitDepth"].is_number_integer() && hdr["swapchainBitDepth"].get<int>() == 16)
                        request.swapchainBitDepth = Rendering::HdrSwapchainBitDepth::Float16;
                    else if (hdr["swapchainBitDepth"].is_string() && hdr["swapchainBitDepth"].get<std::string>() == "16")
                        request.swapchainBitDepth = Rendering::HdrSwapchainBitDepth::Float16;
                    else
                        request.swapchainBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
                }
                if (hdr.contains("targetDisplay") && hdr["targetDisplay"].is_number_integer())
                    request.targetDisplay = hdr["targetDisplay"].get<int>();
                if (hdr.contains("autoTuneForDisplay") && hdr["autoTuneForDisplay"].is_boolean())
                    request.autoTuneForDisplay = hdr["autoTuneForDisplay"].get<bool>();
                if (const auto itMetadata = hdr.find("metadata"); itMetadata != hdr.end() && itMetadata->is_object())
                {
                    request.metadataExplicit = true;
                    const auto& metadata = *itMetadata;
                    if (metadata.contains("maxMasteringLuminance") && metadata["maxMasteringLuminance"].is_number())
                        request.metadata.maxMasteringLuminance = metadata["maxMasteringLuminance"].get<float>();
                    if (metadata.contains("minMasteringLuminance") && metadata["minMasteringLuminance"].is_number())
                        request.metadata.minMasteringLuminance = metadata["minMasteringLuminance"].get<float>();
                    if (metadata.contains("maxContentLightLevel") && metadata["maxContentLightLevel"].is_number())
                        request.metadata.maxContentLightLevel = metadata["maxContentLightLevel"].get<float>();
                    if (metadata.contains("maxFrameAverageLightLevel") && metadata["maxFrameAverageLightLevel"].is_number())
                        request.metadata.maxFrameAverageLightLevel = metadata["maxFrameAverageLightLevel"].get<float>();
                    if (metadata.contains("paperWhiteNits") && metadata["paperWhiteNits"].is_number())
                    {
                        request.metadata.paperWhiteNits = metadata["paperWhiteNits"].get<float>();
                        request.paperWhiteExplicit = true;
                    }
                }
                else
                {
                    if (hdr.contains("maxLuminanceNits") && hdr["maxLuminanceNits"].is_number())
                    {
                        const float maxLuminance = hdr["maxLuminanceNits"].get<float>();
                        request.metadata.maxMasteringLuminance = maxLuminance;
                        request.metadata.maxContentLightLevel = maxLuminance;
                    }
                    if (hdr.contains("referenceLuminanceNits") && hdr["referenceLuminanceNits"].is_number())
                    {
                        request.metadata.paperWhiteNits = hdr["referenceLuminanceNits"].get<float>();
                        request.paperWhiteExplicit = true;
                    }
                }
            }
        }
    }
    const std::optional<Rendering::HdrOutputMode> forcedMode = ReadForcedHdrOutputMode();
    if (forcedMode)
        Logger::Log::Info(
            "Editor: GE_FORCE_HDR_OUTPUT={} overrides the project HDR setting (project: enabled={} mode={}{})",
            Rendering::HdrOutputModeToString(*forcedMode),
            request.enabled ? "true" : "false",
            Rendering::HdrOutputModeToString(request.mode),
            enabledExplicit ? "" : ", not set explicitly");

    const Editor::HdrOutputRequestResolution resolved =
        Editor::ResolveHdrOutputRequest(request.enabled, request.mode, forcedMode);
    request.enabled = resolved.enabled;
    request.mode = resolved.mode;
    return request;
}

static EditorHdrOutputRequest TuneHdrRequestForMonitor(EditorHdrOutputRequest request, int monitorIndex)
{
    const auto monitors = Platform::EnumerateMonitors();
    const auto it = std::find_if(monitors.begin(), monitors.end(), [monitorIndex](const Platform::MonitorInfo& monitor) {
        return monitor.index == monitorIndex;
    });
    if (it != monitors.end())
    {
        TuneHdrMetadataForMonitor(request.metadata, *it, request.metadataExplicit,
                                  request.paperWhiteExplicit, request.autoTuneForDisplay);
        // The C7 quirk only decides for the user under Auto; an explicit
        // HDR10_PQ choice in the settings panel stands.
        if (Platform::LooksLikeLGOledC7(it->name) && request.mode == Rendering::HdrOutputMode::Auto)
            request.mode = Rendering::HdrOutputMode::HLG;
    }
    return request;
}

static bool IsHdrRequestEnabledForMonitor(const EditorHdrOutputRequest& request, int monitorIndex)
{
    if (!request.enabled)
        return false;

    const auto monitors = Platform::EnumerateMonitors();
    const auto it = std::find_if(monitors.begin(), monitors.end(), [monitorIndex](const Platform::MonitorInfo& monitor) {
        return monitor.index == monitorIndex;
    });
    if (it == monitors.end())
        return false;

    if (request.targetDisplay == kHdrTargetPrimaryMonitor && !it->primary)
        return false;
    if (request.targetDisplay >= 0 && request.targetDisplay != monitorIndex)
        return false;

    const bool backendReportedHdr = it->hdrAvailable || it->supportsHDR10_PQ || it->supportsHLG || it->supportsScRGB || it->supportsHDR10Plus;
    if (backendReportedHdr)
        return true;

    if (Platform::LooksLikeLGOledC7(it->name))
        return true;

    // Load-bearing: do NOT "clean this up" by making the checks above decide.
    //
    // Platform::MonitorInfo's HDR fields are never assigned on Win32 or Linux —
    // the only writers are Display.cpp:219 (Apple-only) and Display.cpp:235
    // (the GE_FORCE_HDR_DISPLAY override), with supportsScRGB additionally gated
    // on GE_FORCE_SCRGB_HDR at Display.cpp:332/378. So on Windows every term of
    // backendReportedHdr is false no matter what the display is doing, and this
    // return is the only reason HDR output works there at all. Deleting it, or
    // turning the OR into an AND, disables HDR on every Windows machine while
    // looking correct on an SDR panel.
    //
    // The real capability signal is Rendering::HdrDisplayInfo, which the backend
    // fills from the live Vulkan surface formats; ResolveHdrOutputMode already
    // gates on it, and swapchain creation falls back to SDR with
    // HdrOutputState.activeMode == Off when no HDR format is exposed. Deferring
    // to it is the decision here, not an oversight.
    return true;
}

static constexpr auto kHdrMonitorSwitchSettleTime = std::chrono::milliseconds(2000);
} // namespace

namespace Editor
{

void HdrOutputController::Initialize(Dependencies deps)
{
    m_Deps = std::move(deps);
}

void HdrOutputController::QueueUpdate(WindowContext* ctx, int monitorIndex, bool waitForMonitorSettle, bool forceSwapchainRefresh)
{
    if (!ctx || !ctx->window || !ctx->renderCtx)
        return;

    ctx->pendingHdrOutput.pending = true;
    ctx->pendingHdrOutput.monitorIndex = monitorIndex;
    ctx->pendingHdrOutput.forceSwapchainRefresh = ctx->pendingHdrOutput.forceSwapchainRefresh || forceSwapchainRefresh;
    const auto now = std::chrono::steady_clock::now();
    ctx->pendingHdrOutput.requestedAt = waitForMonitorSettle ? now : now - kHdrMonitorSwitchSettleTime;
}

void HdrOutputController::OnWindowCreated(WindowContext* ctx)
{
    if (!ctx || !ctx->window || !ctx->renderCtx)
        return;

    // No display transition is in flight at birth — only the window's own
    // placement — so skip the settle wait; Tick's lastWindowMoveAt term still
    // holds the apply until a tear-off drag comes to rest. forceSwapchainRefresh
    // stays false so a window whose own evaluation matches the inherited request
    // keeps the swapchain it was born with.
    QueueUpdate(ctx, ctx->window->GetActiveMonitorIndex(), /*waitForMonitorSettle=*/false);
}

void HdrOutputController::OnWindowMonitorChanged(WindowContext* ctx, int monitorIndex)
{
    if (!ctx || !ctx->renderCtx || !ctx->window)
        return;

    const auto monitors = Platform::EnumerateMonitors();
    const auto it = std::find_if(monitors.begin(), monitors.end(), [monitorIndex](const Platform::MonitorInfo& monitor) {
        return monitor.index == monitorIndex;
    });
    if (it != monitors.end())
    {
        Logger::Log::Info("Editor: window {} active monitor changed to [{}] '{}' {}x{} @ {:.1f}Hz",
                          ctx->windowId,
                          it->index,
                          it->name,
                          it->width,
                          it->height,
                          it->refreshRate);
    }
    else
    {
        Logger::Log::Info("Editor: window {} active monitor changed to [{}]", ctx->windowId, monitorIndex);
    }

    QueueUpdate(ctx, monitorIndex, true, true);
    m_SuppressRenderUntil = std::chrono::steady_clock::now() + kHdrMonitorSwitchSettleTime;
    ctx->skipRenderFramesAfterHdrSwitch = std::max(ctx->skipRenderFramesAfterHdrSwitch, 2u);
    Logger::Log::Info("Editor: monitor changed; queued HDR output refresh after display settle");
    if (ctx->ui)
        m_Deps.ApplyUiRuntimeConfig(ctx->ui.get());
}

bool HdrOutputController::ApplyForMonitor(WindowContext* ctx, int monitorIndex, bool forceSwapchainRefresh)
{
    if (!ctx || !ctx->window || !ctx->renderCtx)
        return false;

    const EditorHdrOutputRequest rawHdrRequest = LoadEditorHdrOutputRequest(EngineCore::GetInstance().GetWorkspaceRoot());
    const EditorHdrOutputRequest hdrRequest = TuneHdrRequestForMonitor(rawHdrRequest, monitorIndex);
    const bool hdrEnabledForMonitor = IsHdrRequestEnabledForMonitor(hdrRequest, monitorIndex);
    const Rendering::HdrOutputMode mode = hdrEnabledForMonitor ? hdrRequest.mode : Rendering::HdrOutputMode::Off;
    std::string monitorName = "<unknown>";
    const char* displayProfile = "project defaults";
    for (const Platform::MonitorInfo& monitor : Platform::EnumerateMonitors())
    {
        if (monitor.index == monitorIndex)
        {
            monitorName = monitor.name;
            displayProfile = HdrDisplayProfileName(monitor);
            break;
        }
    }
    Logger::Log::Info("Editor: applying HDR mode {} bitDepth={} for window {} on monitor {} '{}' (requestEnabled={} targetDisplay={} profile={} paperWhite={} max={} fall={})",
                      Rendering::HdrOutputModeToString(mode),
                      hdrRequest.swapchainBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10,
                      ctx->windowId,
                      monitorIndex,
                      monitorName,
                      hdrRequest.enabled ? "true" : "false",
                      hdrRequest.targetDisplay,
                      displayProfile,
                      hdrRequest.metadata.paperWhiteNits,
                      hdrRequest.metadata.maxMasteringLuminance,
                      hdrRequest.metadata.maxFrameAverageLightLevel);

    if (!ctx->renderCtx->ActivateWindowTarget())
        return false;

    Rendering::IDevice* device = ctx->renderCtx->GetDevice();
    if (!device)
        return false;

    const Rendering::HdrOutputState currentHdrState = device->GetHdrOutputState();
    const bool alreadySatisfied = HdrOutputStateSatisfiesRequest(currentHdrState, mode,
                                                                 hdrRequest.swapchainBitDepth,
                                                                 hdrRequest.metadata);
    if (alreadySatisfied && !forceSwapchainRefresh)
    {
        Logger::Log::Info("Editor: HDR mode {} already active for window {}; leaving swapchain unchanged",
                          Rendering::HdrOutputModeToString(mode),
                          ctx->windowId);
        if (ctx->ui)
            m_Deps.ApplyUiRuntimeConfig(ctx->ui.get());
        return true;
    }

    const bool forceUnchangedSwapchainRefresh = forceSwapchainRefresh && alreadySatisfied;
    constexpr auto kHdrSwitchRenderSuppressDuration = std::chrono::milliseconds(1500);
    m_SuppressRenderUntil = std::chrono::steady_clock::now() + kHdrSwitchRenderSuppressDuration;
    ctx->skipRenderFramesAfterHdrSwitch = 30;
    // RenderGraph arm: drain this window's frame stream (graph-scoped, never
    // device-wide). Nothing surface-dependent is retained in RenderGraph.
    if (ctx->RenderGraphStream.Frame)
        ctx->RenderGraphStream.Frame->WaitForPendingWork();
    if (ctx->ui)
        ctx->ui->InvalidateRenderPassStateAfterTargetChange();
    // No device drain around the re-spec: the swapchain recreate both arms
    // below reach idles every queue the engine submits to before it destroys
    // the images and semaphores, which is every submission that can reference
    // them.
    bool applied = false;
    if (forceUnchangedSwapchainRefresh)
    {
        Logger::Log::Info("Editor: refreshing swapchain for monitor change with HDR mode {} unchanged for window {}",
                          Rendering::HdrOutputModeToString(mode),
                          ctx->windowId);
        const int refreshWidth = ctx->pendingSwapchainResize ? ctx->pendingWidth : ctx->width;
        const int refreshHeight = ctx->pendingSwapchainResize ? ctx->pendingHeight : ctx->height;
        ctx->pendingSwapchainResize = false;
        ctx->pendingWidth = 0;
        ctx->pendingHeight = 0;
        applied = refreshWidth > 0 && refreshHeight > 0 &&
                  ctx->renderCtx->RecreateWindowTargetSwapchain(static_cast<uint32_t>(refreshWidth),
                                                                static_cast<uint32_t>(refreshHeight));
    }
    else
    {
        applied = device->SetHdrOutputMode(mode, &hdrRequest.metadata, hdrRequest.swapchainBitDepth);
    }
    if (applied)
    {
        if (ctx->ui)
            ctx->ui->InvalidateRenderPassStateAfterTargetChange();
        m_SuppressRenderUntil = std::chrono::steady_clock::now() + kHdrSwitchRenderSuppressDuration;
        ctx->skipRenderFramesAfterHdrSwitch = 30;
        if (ctx->ui)
            m_Deps.ApplyUiRuntimeConfig(ctx->ui.get());
        // The discard above drops the imported draw-stream/visibility buffer
        // rebindings the persistent bucketer passes rely on; without a world
        // re-record the color pass replays stale indirect records (ghost
        // geometry flickering until the next interaction re-records views).
        // Same recovery the resize path performs.
        m_Deps.RefreshWorldRenderForPassiveFrame(ctx);
        // Surface what the device decided at the moment of the switch (e.g. whether
        // vkSetHdrMetadataEXT actually fired, or that no HDR surface format exists).
        const Rendering::HdrOutputState appliedHdrState = device->GetHdrOutputState();
        for (const std::string& hint : appliedHdrState.display.diagnosticHints)
            Logger::Log::Info("Editor: HDR diagnostic — {}", hint);

        // Reading the probe: liveEdr collapsing toward 1.0 while deviceMaxLinear
        // stays high is the OS-side EDR-headroom collapse, not a device downgrade.
        float liveEdr = 0.0f;
        float potentialEdr = 0.0f;
        if (Platform::GetActiveDisplayLiveEdr(ctx->window->GetGLFWHandle(), liveEdr, potentialEdr))
            Logger::Log::Info(
                "Editor: EDR probe (post-apply) window {} activeMode={} liveEdr={:.2f} potentialEdr={:.2f} deviceMaxLinear={:.2f}",
                ctx->windowId,
                Rendering::HdrOutputModeToString(appliedHdrState.activeMode),
                liveEdr,
                potentialEdr,
                appliedHdrState.display.outputMaxLinearValue);
    }
    else if (forceUnchangedSwapchainRefresh)
    {
        ctx->pendingHdrOutput.pending = true;
        ctx->pendingHdrOutput.monitorIndex = monitorIndex;
        ctx->pendingHdrOutput.forceSwapchainRefresh = true;
        ctx->pendingHdrOutput.requestedAt = std::chrono::steady_clock::now();
        m_SuppressRenderUntil = std::chrono::steady_clock::now() + kHdrMonitorSwitchSettleTime;
        ctx->skipRenderFramesAfterHdrSwitch = std::max(ctx->skipRenderFramesAfterHdrSwitch, 2u);
    }
    return applied;
}

void HdrOutputController::Tick()
{
    const auto now = std::chrono::steady_clock::now();

    // Live-track the OS "SDR content brightness" so the auto UI white follows
    // the Windows slider mid-session. Config-only re-derive (no swapchain
    // churn); windows with an explicit uiRuntimeConfigOverride keep it.
    static constexpr auto kSdrWhitePollInterval = std::chrono::seconds(2);
    if (now - m_LastSdrWhitePollAt >= kSdrWhitePollInterval)
    {
        m_LastSdrWhitePollAt = now;
        const float osSdrWhiteNits = Platform::GetPrimaryMonitorInfo().sdrWhiteLevelNits;
        if (osSdrWhiteNits > 0.0f && m_LastSdrWhiteLevelNits > 0.0f &&
            std::abs(osSdrWhiteNits - m_LastSdrWhiteLevelNits) > 0.5f)
        {
            Logger::Log::Info(
                "Editor: OS SDR white level changed {} -> {} nits; re-deriving UI HDR mapping",
                m_LastSdrWhiteLevelNits, osSdrWhiteNits);
            for (const auto& win : (*m_Deps.Windows))
                if (win && win->ui && !win->uiRuntimeConfigOverride.has_value())
                    m_Deps.ApplyUiRuntimeConfig(win->ui.get());
        }
        m_LastSdrWhiteLevelNits = osSdrWhiteNits;
    }

    for (const auto& win : (*m_Deps.Windows))
    {
        WindowContext* ctx = win.get();
        if (!ctx || !ctx->pendingHdrOutput.pending || !ctx->window)
            continue;

        const int currentMonitor = ctx->window->GetActiveMonitorIndex();
        if (currentMonitor != ctx->pendingHdrOutput.monitorIndex)
            continue;

        const auto referenceTime = std::max(ctx->pendingHdrOutput.requestedAt, ctx->lastWindowMoveAt);
        if (now - referenceTime < kHdrMonitorSwitchSettleTime)
            continue;

        if (ctx->width <= 0 || ctx->height <= 0)
            continue;

        const int monitorIndex = ctx->pendingHdrOutput.monitorIndex;
        const bool forceSwapchainRefresh = ctx->pendingHdrOutput.forceSwapchainRefresh;
        ctx->pendingHdrOutput.pending = false;
        ctx->pendingHdrOutput.forceSwapchainRefresh = false;
        (void)ApplyForMonitor(ctx, monitorIndex, forceSwapchainRefresh);
    }
}

void HdrOutputController::RequestRefreshForAllWindows()
{
    for (const auto& win : (*m_Deps.Windows))
    {
        if (!win || !win->window || !win->renderCtx)
            continue;
        if (win->ui)
            m_Deps.ApplyUiRuntimeConfig(win->ui.get());
        QueueUpdate(win.get(), win->window->GetActiveMonitorIndex(), false);
    }
}

bool HdrOutputController::ShouldSuppressRender()
{
    if (m_SuppressRenderUntil.time_since_epoch().count() != 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_SuppressRenderUntil)
            return true;
        m_SuppressRenderUntil = {};
    }
    return false;
}

void HdrOutputController::BeginMonitorSettleSuppression(WindowContext* ctx)
{
    m_SuppressRenderUntil = std::chrono::steady_clock::now() + kHdrMonitorSwitchSettleTime;
    if (ctx)
        ctx->skipRenderFramesAfterHdrSwitch = std::max(ctx->skipRenderFramesAfterHdrSwitch, 2u);
}

void HdrOutputController::ConfigureStartupDeviceDesc(Rendering::DeviceDesc& desc,
                                                     const std::filesystem::path& settingsRoot,
                                                     int activeMonitorIndex)
{
    const EditorHdrOutputRequest rawHdrRequest = LoadEditorHdrOutputRequest(settingsRoot);
    const EditorHdrOutputRequest hdrRequest = TuneHdrRequestForMonitor(rawHdrRequest, activeMonitorIndex);
    const bool savedHdrEnabledForMonitor = IsHdrRequestEnabledForMonitor(hdrRequest, activeMonitorIndex);
    // Create the swapchain already in the saved HDR mode rather than building an
    // SDR one and immediately recreating it. The backend falls back to SDR on its
    // own when the surface exposes no HDR colorspace (e.g. Windows HDR off).
    desc.hdrEnabled = savedHdrEnabledForMonitor;
    desc.hdrMode = savedHdrEnabledForMonitor ? hdrRequest.mode : Rendering::HdrOutputMode::Off;
    desc.hdrSwapchainBitDepth = hdrRequest.swapchainBitDepth;
    desc.hdrTargetDisplay = kHdrTargetActiveWindowMonitor;
    desc.hdrStaticMetadata = hdrRequest.metadata;
    const char* startupHdrProfile = "project defaults";
    std::string startupMonitorName = "<unknown>";
    for (const Platform::MonitorInfo& monitor : Platform::EnumerateMonitors())
    {
        if (monitor.index == activeMonitorIndex)
        {
            startupHdrProfile = HdrDisplayProfileName(monitor);
            startupMonitorName = monitor.name;
            break;
        }
    }
    Logger::Log::Info("Editor: startup HDR; saved request enabled={} mode={} bitDepth={} targetDisplay={} activeMonitor={} '{}' deviceHdrEnabled={} profile={} paperWhite={} max={} fall={}",
                      hdrRequest.enabled ? "true" : "false",
                      Rendering::HdrOutputModeToString(hdrRequest.mode),
                      hdrRequest.swapchainBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10,
                      hdrRequest.targetDisplay,
                      activeMonitorIndex,
                      startupMonitorName,
                      savedHdrEnabledForMonitor ? "true" : "false",
                      startupHdrProfile,
                      hdrRequest.metadata.paperWhiteNits,
                      hdrRequest.metadata.maxMasteringLuminance,
                      hdrRequest.metadata.maxFrameAverageLightLevel);
}

} // namespace Editor
} // namespace GameEngine
