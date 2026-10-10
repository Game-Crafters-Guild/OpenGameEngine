#include "Platform/SystemTheme.h"

namespace GameEngine
{
namespace Platform
{

SystemTheme GetSystemTheme()
{
    // For non-Windows platforms we currently don't query a system
    // preference. Callers can still opt into a specific theme in
    // the future via ApplyThemeToWindow if desired.
    return SystemTheme::Unknown;
}

void ApplyThemeToWindow(Window& /*window*/, SystemTheme /*theme*/)
{
    // No-op on non-Windows platforms for now.
}

void ApplyNativeAppTheme(SystemTheme /*theme*/)
{
    // No-op on non-Windows platforms for now.
}

} // namespace Platform
} // namespace GameEngine
