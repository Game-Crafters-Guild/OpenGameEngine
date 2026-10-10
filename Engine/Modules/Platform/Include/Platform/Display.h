#pragma once

#include <string>
#include <vector>

struct GLFWwindow;

namespace GameEngine {
namespace Platform {

struct MonitorInfo
{
    int index = -1;
    std::string id;
    std::string name;
    bool primary = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int workX = 0;
    int workY = 0;
    int workWidth = 0;
    int workHeight = 0;
    int physicalWidthMm = 0;
    int physicalHeightMm = 0;
    double refreshRate = 0.0;
    bool hdrAvailable = false;
    bool hdrActive = false;
    bool supportsHDR10_PQ = false;
    bool supportsHLG = false;
    bool supportsScRGB = false;
    bool supportsHDR10Plus = false;
    float maxLuminance = 0.0f;
    float minLuminance = 0.0f;
    float maxFullFrameLuminance = 0.0f;
    // OS-reported SDR reference white in absolute nits (Windows:
    // DISPLAYCONFIG_SDR_WHITE_LEVEL; macOS: BT.2408 203-nit anchor, since macOS
    // exposes no absolute-nits API). 0 == unknown / not queried (Linux today)
    // -> consumers fall back to the device paper-white.
    float sdrWhiteLevelNits = 0.0f;
    std::vector<std::string> diagnosticHints;
};

// Monitor/display query utilities (requires GLFW to be initialized)

// Returns the usable work area of the primary monitor (excluding taskbar/dock/menu bar).
// outX, outY: position of the work area origin
// outWidth, outHeight: dimensions of the work area in screen coordinates
void GetPrimaryMonitorWorkArea(int& outX, int& outY, int& outWidth, int& outHeight);
std::vector<MonitorInfo> EnumerateMonitors();
int GetActiveMonitorIndexForWindow(GLFWwindow* window);
MonitorInfo GetMonitorInfoForWindow(GLFWwindow* window);
MonitorInfo GetPrimaryMonitorInfo();

// Live + potential EDR headroom multipliers (relative to SDR white) of the display the
// given window is on (falls back to the main display when the window is off-screen/null).
// The "current" value fluctuates with content/brightness/thermal and, on a swapchain
// recreate, can momentarily collapse toward 1.0 — the "toggle-dim" symptom. Diagnostic
// only (never use the live value for capability). Returns false off macOS or when no
// display is available; the outputs are left untouched in that case.
bool GetActiveDisplayLiveEdr(GLFWwindow* window, float& outCurrentEdr, float& outPotentialEdr);

bool IsGamescopeSession();
bool IsSteamDeckSession();
bool LooksLikeLGOledC7(const std::string& displayName);

} // namespace Platform
} // namespace GameEngine
