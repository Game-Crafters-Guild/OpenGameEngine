#include "Platform/SystemTheme.h"
#include "Platform/Window.h"

// Windows-specific implementation of system theme helpers.
#if defined(_WIN32)

#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// DWM dark mode attribute is available on Windows 10 1809+ / 1903+
// but the numeric value has changed across SDKs. Define it here so
// we can call DwmSetWindowAttribute in a backwards-compatible way
// without including dwmapi.h directly (which can be sensitive to
// header ordering / SDK variants).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#endif

// Forward declaration of DwmSetWindowAttribute so we don't depend
// on dwmapi.h. The function lives in dwmapi.lib which we already
// link against via CMake.
extern "C" HRESULT WINAPI DwmSetWindowAttribute(
    HWND hwnd,
    DWORD dwAttribute,
    LPCVOID pvAttribute,
    DWORD cbAttribute);

namespace GameEngine
{
namespace Platform
{

namespace
{
enum class PreferredAppMode
{
    Default   = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode appMode);
using FlushMenuThemesFn = void(WINAPI*)();

static void ApplyPreferredAppMode(SystemTheme theme)
{
    static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxtheme)
        return;

    // These undocumented ordinals are the same dark-mode hooks used by
    // Win32 apps that want OS-themed popup menus on Windows 10 1903+.
    static auto setPreferredAppMode =
        reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    static auto flushMenuThemes =
        reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));

    if (!setPreferredAppMode)
        return;

    PreferredAppMode mode = PreferredAppMode::AllowDark;
    if (theme == SystemTheme::Light)
        mode = PreferredAppMode::ForceLight;
    else if (theme == SystemTheme::Dark)
        mode = PreferredAppMode::AllowDark;

    (void)setPreferredAppMode(mode);
    if (flushMenuThemes)
        flushMenuThemes();
}
} // namespace

static SystemTheme ReadAppsUseLightTheme()
{
    // Windows stores the app theme preference in HKCU. A value of 0
    // indicates dark theme; 1 indicates light theme.
    DWORD value = 0;
    DWORD valueSize = sizeof(value);

    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0,
        KEY_READ,
        &key);

    if (status != ERROR_SUCCESS)
    {
        return SystemTheme::Unknown;
    }

    DWORD type = 0;
    status = RegGetValueW(
        key,
        nullptr,
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        &type,
        &value,
        &valueSize);

    RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_DWORD)
    {
        return SystemTheme::Unknown;
    }

    return (value == 0) ? SystemTheme::Dark : SystemTheme::Light;
}

SystemTheme GetSystemTheme()
{
    return ReadAppsUseLightTheme();
}

void ApplyNativeAppTheme(SystemTheme theme)
{
    if (theme == SystemTheme::Unknown)
    {
        theme = GetSystemTheme();
    }

    ApplyPreferredAppMode(theme);
}

void ApplyThemeToWindow(Window& window, SystemTheme theme)
{
    // If caller passed Unknown, fall back to the system preference.
    if (theme == SystemTheme::Unknown)
    {
        theme = GetSystemTheme();
    }

    // If we still don't know, leave the OS defaults as-is.
    if (theme == SystemTheme::Unknown)
    {
        return;
    }

    ApplyNativeAppTheme(theme);

    GLFWwindow* gw = window.GetGLFWHandle();
    if (!gw)
    {
        return;
    }

    HWND hwnd = glfwGetWin32Window(gw);
    if (!hwnd)
    {
        return;
    }

    const BOOL useDark = (theme == SystemTheme::Dark) ? TRUE : FALSE;

    // Hint DWM to use immersive dark mode for the window's non-client
    // area when the user prefers a dark app theme. Older Windows
    // builds ignore unknown attributes, so it's safe to call
    // unconditionally and ignore failures.
    (void)DwmSetWindowAttribute(
        hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1,
        &useDark,
        sizeof(useDark));

    (void)DwmSetWindowAttribute(
        hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE,
        &useDark,
        sizeof(useDark));
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
