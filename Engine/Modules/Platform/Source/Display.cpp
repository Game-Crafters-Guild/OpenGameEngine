#include "Platform/Display.h"
#include "Platform/Window.h"
#include "Logger/Logger.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <map>
#elif defined(__APPLE__)
#include <cstdint>
// GE_MacOS_ScreenInfo + the GE_MacOS_* prototypes (implemented in Display_mac.mm).
#include "MacScreenBridge.h"
// Map GLFW handles to their Cocoa objects. Declared by hand rather than via
// <GLFW/glfw3native.h>, which references the Objective-C `id` type and won't compile in
// this C++ translation unit. glfwGetCocoaWindow returns an NSWindow* (an `id`/pointer).
extern "C" uint32_t glfwGetCocoaMonitor(GLFWmonitor* monitor);
extern "C" void* glfwGetCocoaWindow(GLFWwindow* window);
#endif

namespace GameEngine {
namespace Platform {

namespace {

// BT.2408 SDR reference white in nits. macOS exposes no absolute-nits SDR-white
// API, so this is the anchor used for EDR displays there; it is also the value
// the GE_FORCE_HDR_DISPLAY diagnostic reports.
constexpr float kBT2408SdrWhiteNits = 203.0f;

bool HasEnv(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0';
}

long long RectIntersectionArea(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    const int x0 = std::max(ax, bx);
    const int y0 = std::max(ay, by);
    const int x1 = std::min(ax + aw, bx + bw);
    const int y1 = std::min(ay + ah, by + bh);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    return static_cast<long long>(x1 - x0) * static_cast<long long>(y1 - y0);
}

long long RectCenterDistanceSquared(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    const long long acx = static_cast<long long>(ax) * 2 + aw;
    const long long acy = static_cast<long long>(ay) * 2 + ah;
    const long long bcx = static_cast<long long>(bx) * 2 + bw;
    const long long bcy = static_cast<long long>(by) * 2 + bh;
    const long long dx = acx - bcx;
    const long long dy = acy - bcy;
    return dx * dx + dy * dy;
}

void AddSessionDiagnostics(MonitorInfo& info)
{
    if (IsGamescopeSession())
    {
        info.diagnosticHints.push_back("Gamescope session detected; Vulkan surface HDR formats decide Steam HDR availability.");
        if (!HasEnv("GAMESCOPE_HDR_OUTPUT") && !HasEnv("ENABLE_HDR_WSI"))
            info.diagnosticHints.push_back("Gamescope HDR environment flag is not visible; compositor may not expose HDR.");
    }

    if (LooksLikeLGOledC7(info.name))
    {
        info.diagnosticHints.push_back("LG OLED C7 detected; prefer HLG/HDR10+ if plain HDR10 PQ does not trigger TV HDR mode.");
        if (!info.hdrAvailable)
        {
            info.diagnosticHints.push_back("If HDR is missing, check HDMI Ultra HD Deep Color, HDMI 2.0 bandwidth, GPU/cable 10-bit support, and AVR/capture EDID passthrough.");
        }
    }
}

#if defined(_WIN32)
// Query the OS "SDR content brightness" (SDR white level, in nits) for each
// active display path, keyed by the GDI device name (e.g. "\\.\DISPLAY1"). The
// map is empty when the driver/OS does not report it (older Win10, query
// failure) -> MonitorInfo.sdrWhiteLevelNits stays 0 (unknown).
std::map<std::string, float> QuerySdrWhiteLevelsByDeviceName()
{
    std::map<std::string, float> out;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                           &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return out;
    paths.resize(pathCount);
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
            continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId; // SDR white is keyed on the TARGET
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS || white.SDRWhiteLevel == 0)
            continue;

        char gdiName[CCHDEVICENAME] = {};
        WideCharToMultiByte(CP_ACP, 0, source.viewGdiDeviceName, -1, gdiName, sizeof(gdiName), nullptr, nullptr);
        // SDRWhiteLevel is encoded as (nits / 80) * 1000.
        out[gdiName] = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    return out;
}

struct DxgiLuminance
{
    float maxLuminance = 0.0f;          // peak (small-window) nits
    float minLuminance = 0.0f;          // black floor nits
    float maxFullFrameLuminance = 0.0f; // sustained full-field nits
};

// Per-monitor HDR luminance reported by DXGI, keyed by the GDI device name
// ("\\.\DISPLAY1") so it matches glfwGetWin32Adapter. Only outputs currently in the
// HDR (PQ) colour space report meaningful values; SDR outputs are skipped so
// MonitorInfo luminance stays 0 (unknown) and consumers keep the device-default
// 1000/400 fallbacks. The map is empty when DXGI is unavailable or no output is HDR.
std::map<std::string, DxgiLuminance> QueryDxgiLuminanceByDeviceName()
{
    using Microsoft::WRL::ComPtr;
    // This runs inside the per-frame monitor poll (and once per OS window), but display luminance only
    // changes on HDR-mode / hotplug events. Cache the result and refresh at a low rate rather than
    // recreating a DXGI factory and re-enumerating every adapter/output every frame.
    static std::map<std::string, DxgiLuminance> cached;
    static std::chrono::steady_clock::time_point lastQuery{};
    static bool haveCached = false;
    constexpr auto kRefreshInterval = std::chrono::milliseconds(500);
    const auto nowTime = std::chrono::steady_clock::now();
    if (haveCached && (nowTime - lastQuery) < kRefreshInterval)
        return cached;
    lastQuery = nowTime;
    haveCached = true;

    std::map<std::string, DxgiLuminance> out;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            // Break on ANY failure (not just NOT_FOUND) so the body never runs with a null adapter.
            if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter)))
                break;
            for (UINT outputIndex = 0;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                if (FAILED(adapter->EnumOutputs(outputIndex, &output)))
                    break;
                ComPtr<IDXGIOutput6> output6;
                if (FAILED(output.As(&output6)))
                    continue;
                DXGI_OUTPUT_DESC1 desc{};
                if (FAILED(output6->GetDesc1(&desc)))
                    continue;
                // Luminance is only meaningful while the output is in an HDR colour space.
                if (desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                    continue;

                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (!GetMonitorInfoW(desc.Monitor, &mi))
                    continue;
                char gdiName[CCHDEVICENAME] = {};
                WideCharToMultiByte(CP_ACP, 0, mi.szDevice, -1, gdiName, sizeof(gdiName), nullptr, nullptr);

                DxgiLuminance lum;
                lum.maxLuminance = desc.MaxLuminance;
                lum.minLuminance = desc.MinLuminance;
                lum.maxFullFrameLuminance = desc.MaxFullFrameLuminance;
                out[gdiName] = lum;
            }
        }
    }
    cached = out;
    return cached;
}
#endif

#if defined(__APPLE__)
// Map an NSScreen EDR headroom pair onto a MonitorInfo's HDR capability fields.
// Capability is driven by the STABLE potential headroom; the live "current" value
// fluctuates with brightness/content/thermal and only says whether EDR is engaged
// right now (used for hdrActive, never for capability).
void ApplyMacEdrCapabilities(MonitorInfo& info, float currentEdr, float potentialEdr)
{
    if (potentialEdr > 1.0f)
    {
        info.hdrAvailable = true;
        info.hdrActive = currentEdr > 1.0f;
        info.supportsScRGB = true; // MoltenVK only exposes EXTENDED_SRGB_LINEAR
        info.sdrWhiteLevelNits = kBT2408SdrWhiteNits;
        info.maxLuminance = kBT2408SdrWhiteNits * potentialEdr;
    }
}
#endif

// Apply the GE_FORCE_HDR_DISPLAY diagnostic override (forces a synthetic HDR profile
// for testing) and append session diagnostics. Shared finalization for every
// enumerated monitor, regardless of how it was discovered.
void ApplyForcedHdrAndDiagnostics(MonitorInfo& info)
{
    if (HasEnv("GE_FORCE_HDR_DISPLAY"))
    {
        info.hdrAvailable = true;
        info.hdrActive = true;
        info.supportsHDR10_PQ = true;
        info.supportsHLG = true;
        info.supportsScRGB = true;
        info.maxLuminance = 1000.0f;
        info.minLuminance = 0.001f;
        info.maxFullFrameLuminance = 400.0f;
        info.sdrWhiteLevelNits = kBT2408SdrWhiteNits;
        info.diagnosticHints.push_back("HDR forced by GE_FORCE_HDR_DISPLAY for diagnostics.");
    }
    AddSessionDiagnostics(info);
}

// Build a MonitorInfo for each monitor GLFW currently knows about. Returns empty when
// glfwGetMonitors() reports nothing (GLFW only refreshes its list at init / on display
// reconfiguration, so a bad initial poll can leave it empty).
std::vector<MonitorInfo> EnumerateMonitorsFromGlfw()
{
    // Logged once per empty-streak (not per call — this runs ~per frame) so the rare
    // "GLFW reports no monitors" condition is directly attributable to glfwGetMonitors
    // (not Window::Initialize()), and we can see when it recovers. Plain static bool:
    // this path is main-thread-only (see EnumerateMonitors).
    static bool loggedEmpty = false;

    std::vector<MonitorInfo> result;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    if (!monitors || count <= 0)
    {
        if (!loggedEmpty)
        {
            loggedEmpty = true;
            Logger::Log::Warning(
                "Platform: glfwGetMonitors() returned count={} (ptr {}); GLFW display enumeration empty — falling back",
                count,
                monitors ? "non-null" : "null");
        }
        return result;
    }
    if (loggedEmpty)
    {
        loggedEmpty = false;
        Logger::Log::Info("Platform: glfwGetMonitors() recovered ({} monitor(s))", count);
    }

    result.reserve(static_cast<size_t>(count));
#if defined(_WIN32)
    const auto sdrWhiteByDevice = QuerySdrWhiteLevelsByDeviceName();
    const auto dxgiLumByDevice = QueryDxgiLuminanceByDeviceName();
#endif
    for (int i = 0; i < count; ++i)
    {
        GLFWmonitor* monitor = monitors[i];
        MonitorInfo info{};
        info.index = i;
        info.primary = (monitor == primary);
        info.id = std::to_string(i);
        const char* name = glfwGetMonitorName(monitor);
        info.name = name ? name : ("Monitor " + std::to_string(i));
        glfwGetMonitorPos(monitor, &info.x, &info.y);
        glfwGetMonitorWorkarea(monitor, &info.workX, &info.workY, &info.workWidth, &info.workHeight);
        glfwGetMonitorPhysicalSize(monitor, &info.physicalWidthMm, &info.physicalHeightMm);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (mode)
        {
            info.width = mode->width;
            info.height = mode->height;
            info.refreshRate = static_cast<double>(mode->refreshRate);
        }
        else
        {
            info.width = info.workWidth;
            info.height = info.workHeight;
        }

#if defined(_WIN32)
        // glfwGetWin32Adapter returns the GDI adapter name ("\\.\DISPLAY1"), which
        // matches DISPLAYCONFIG_SOURCE_DEVICE_NAME.viewGdiDeviceName used to key the
        // SDR-white map queried above.
        if (const char* adapterName = glfwGetWin32Adapter(monitor))
        {
            const auto it = sdrWhiteByDevice.find(adapterName);
            if (it != sdrWhiteByDevice.end())
                info.sdrWhiteLevelNits = it->second;
            const auto lumIt = dxgiLumByDevice.find(adapterName);
            if (lumIt != dxgiLumByDevice.end())
            {
                info.maxLuminance = lumIt->second.maxLuminance;
                info.minLuminance = lumIt->second.minLuminance;
                info.maxFullFrameLuminance = lumIt->second.maxFullFrameLuminance;
            }
        }
#endif

        // GLFW does not expose HDR/EDID metadata. Backends fill precise support from swapchain APIs.
        info.supportsScRGB = HasEnv("GE_FORCE_SCRGB_HDR");

#if defined(__APPLE__)
        // macOS exposes no SDR-white-in-nits API, only EDR headroom multipliers via
        // NSScreen. Runs after the supportsScRGB assignment above (which it may
        // override) and before the forced-HDR override inside the finalizer.
        float currentEdr = 1.0f;
        float potentialEdr = 1.0f;
        if (GE_MacOS_GetScreenEdrInfo(glfwGetCocoaMonitor(monitor), &currentEdr, &potentialEdr))
            ApplyMacEdrCapabilities(info, currentEdr, potentialEdr);
#endif

        ApplyForcedHdrAndDiagnostics(info);
        result.push_back(std::move(info));
    }
    return result;
}

#if defined(__APPLE__)
// Enumerate displays straight from NSScreen, bypassing GLFW's monitor cache. Used as a
// fallback when EnumerateMonitorsFromGlfw() comes back empty, so window->monitor
// resolution (and HDR gating) don't collapse to -1 for the whole session.
std::vector<MonitorInfo> EnumerateMonitorsFromNSScreen()
{
    std::vector<MonitorInfo> result;
    constexpr int kMaxScreens = 16;
    GE_MacOS_ScreenInfo screens[kMaxScreens];
    const int count = GE_MacOS_EnumerateScreens(screens, kMaxScreens);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const GE_MacOS_ScreenInfo& s = screens[i];
        MonitorInfo info{};
        info.index = i;
        info.primary = s.isPrimary != 0;
        info.id = std::to_string(i);
        info.name = (s.name[0] != '\0') ? std::string(s.name) : ("Monitor " + std::to_string(i));
        info.x = s.x;
        info.y = s.y;
        info.width = s.width;
        info.height = s.height;
        info.workX = s.workX;
        info.workY = s.workY;
        info.workWidth = s.workWidth;
        info.workHeight = s.workHeight;
        info.refreshRate = s.refreshRate;
        info.supportsScRGB = HasEnv("GE_FORCE_SCRGB_HDR");
        ApplyMacEdrCapabilities(info, s.currentEdr, s.potentialEdr);
        ApplyForcedHdrAndDiagnostics(info);
        result.push_back(std::move(info));
    }
    return result;
}
#endif

// Last non-empty monitor enumeration. A transient empty result (display sleep/wake,
// fast swapchain recreation) would otherwise collapse active-monitor resolution to -1
// and disable HDR; returning the last-good set rides over the gap. Plain file-static:
// every caller runs on the main thread (the frame loop and GLFW window callbacks), so
// no synchronization is needed.
std::vector<MonitorInfo> g_LastGoodMonitors;

} // namespace

void GetPrimaryMonitorWorkArea(int& outX, int& outY, int& outWidth, int& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth = 1280;  // Fallback defaults
    outHeight = 720;

    if (!Window::Initialize())
        return;

    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    if (primary)
    {
        glfwGetMonitorWorkarea(primary, &outX, &outY, &outWidth, &outHeight);
    }
}

std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> result;
    if (!Window::Initialize())
        return result;

    result = EnumerateMonitorsFromGlfw();
#if defined(__APPLE__)
    // GLFW's monitor list can come back empty for the whole session; fall back to a
    // direct NSScreen query so HDR can still resolve a target display.
    if (result.empty())
        result = EnumerateMonitorsFromNSScreen();
#endif

    if (!result.empty())
    {
        g_LastGoodMonitors = result;
        return result;
    }
    // Nothing enumerated this call — hand back the last known-good set rather than
    // collapsing every caller (window->monitor resolution, HDR gating) to "no displays".
    return g_LastGoodMonitors;
}

bool GetActiveDisplayLiveEdr(GLFWwindow* window, float& outCurrentEdr, float& outPotentialEdr)
{
#if defined(__APPLE__)
    // Query the screen the window is actually on (falls back to the main screen inside
    // the bridge) so the probe reports the recreated window's display, not the key one.
    return GE_MacOS_GetWindowScreenEdrInfo(window ? glfwGetCocoaWindow(window) : nullptr,
                                           &outCurrentEdr, &outPotentialEdr);
#else
    (void)window;
    (void)outCurrentEdr;
    (void)outPotentialEdr;
    return false;
#endif
}

int GetActiveMonitorIndexForWindow(GLFWwindow* window)
{
    if (!window)
        return -1;

    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window, &wx, &wy);
    glfwGetWindowSize(window, &ww, &wh);
    const auto monitors = EnumerateMonitors();
    int bestIndex = -1;
    long long bestArea = -1;
    long long bestDistance = std::numeric_limits<long long>::max();
    for (const MonitorInfo& monitor : monitors)
    {
        const long long area = RectIntersectionArea(wx, wy, ww, wh, monitor.x, monitor.y, monitor.width, monitor.height);
        const long long distance = RectCenterDistanceSquared(wx, wy, ww, wh, monitor.x, monitor.y, monitor.width, monitor.height);
        if (area > bestArea || (area == bestArea && distance < bestDistance))
        {
            bestArea = area;
            bestDistance = distance;
            bestIndex = monitor.index;
        }
    }
    return bestIndex;
}

MonitorInfo GetMonitorInfoForWindow(GLFWwindow* window)
{
    const int monitorIndex = GetActiveMonitorIndexForWindow(window);
    auto monitors = EnumerateMonitors();
    for (const MonitorInfo& monitor : monitors)
    {
        if (monitor.index == monitorIndex)
            return monitor;
    }
    return GetPrimaryMonitorInfo();
}

MonitorInfo GetPrimaryMonitorInfo()
{
    auto monitors = EnumerateMonitors();
    for (const MonitorInfo& monitor : monitors)
    {
        if (monitor.primary)
            return monitor;
    }
    return monitors.empty() ? MonitorInfo{} : monitors.front();
}

bool IsGamescopeSession()
{
    return HasEnv("GAMESCOPE_WAYLAND_DISPLAY") || HasEnv("ENABLE_GAMESCOPE_WSI") ||
           HasEnv("GAMESCOPE_HDR_OUTPUT") || HasEnv("SteamDeck");
}

bool IsSteamDeckSession()
{
#if defined(PLATFORM_STEAMDECK) || defined(GE_STEAMDECK_BUILD)
    return true;
#else
    return HasEnv("SteamDeck") || HasEnv("STEAM_DECK") || HasEnv("SteamOS");
#endif
}

bool LooksLikeLGOledC7(const std::string& displayName)
{
    std::string name = displayName;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find("lg oled") != std::string::npos ||
           name == "lg tv" ||
           name.find("lg electronics tv") != std::string::npos ||
           name.find("oled55c7") != std::string::npos ||
           name.find("oled65c7") != std::string::npos ||
           name.find("c7p") != std::string::npos ||
           name.find("c7v") != std::string::npos ||
           name.find("c7t") != std::string::npos;
}

} // namespace Platform
} // namespace GameEngine
