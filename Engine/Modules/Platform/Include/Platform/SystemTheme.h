#pragma once

#include <cstdint>

// Forward declaration to avoid including Window.h in this header
namespace GameEngine
{
namespace Platform
{

class Window;

enum class SystemTheme : std::uint8_t
{
    Unknown = 0,
    Light   = 1,
    Dark    = 2,
};

// Query the current operating system preference for app theme.
// On Windows this uses the AppsUseLightTheme registry value. On
// other platforms this returns SystemTheme::Unknown for now.
SystemTheme GetSystemTheme();

// Apply a theme hint to a top-level window's non-client area
// (title bar, system-drawn chrome, native toolbars where supported).
// Implemented on Windows; no-op elsewhere.
void ApplyThemeToWindow(Window& window, SystemTheme theme);

// Apply process-wide native app/menu theme hints where the OS supports them.
// On Windows this opts native popup menus into the current app theme. No-op elsewhere.
void ApplyNativeAppTheme(SystemTheme theme);

// Convenience: apply the current system theme to the window.
inline void ApplySystemThemeToWindow(Window& window)
{
    ApplyThemeToWindow(window, GetSystemTheme());
}

inline void ApplySystemNativeAppTheme()
{
    ApplyNativeAppTheme(GetSystemTheme());
}

} // namespace Platform
} // namespace GameEngine
