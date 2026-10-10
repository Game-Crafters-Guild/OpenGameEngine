#pragma once

#include "Platform/Environment.h"

namespace GameEngine::Engine::Renderer
{

/// Whether an app requests the graphics debug layer (DeviceDesc::enableDebugLayer)
/// for the device it creates.
///
/// Debug: on by default. DebugFast and Release: off by default. GE_VK_NO_VALIDATION
/// forces it off and GE_VK_VALIDATION forces it on; either switch wins over the
/// config default, and both follow Platform::EnvironmentSwitchEnabled, so
/// GE_VK_NO_VALIDATION=0 (or false/off/no)
/// leaves validation at its default.
///
/// Inline so the config test resolves in the calling app's translation unit.
inline bool ShouldEnableVkValidation()
{
#if defined(GE_DEBUGFAST)
    constexpr bool kDefaultOn = false;
#elif defined(_DEBUG) || defined(DEBUG)
    constexpr bool kDefaultOn = true;
#else
    constexpr bool kDefaultOn = false;
#endif
    if (Platform::EnvironmentSwitchEnabled("GE_VK_NO_VALIDATION", false))
        return false;
    if (Platform::EnvironmentSwitchEnabled("GE_VK_VALIDATION", false))
        return true;
    return kDefaultOn;
}

} // namespace GameEngine::Engine::Renderer
