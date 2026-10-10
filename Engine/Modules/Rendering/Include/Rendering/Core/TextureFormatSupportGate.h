#pragma once

// TextureFormatSupportGate.h — the one place a backend's CreateTexture asks
// its own IsTextureFormatSupported before allocating.
//
// A backend that answers NO to the query but creates the texture anyway hands
// the caller a resource the API will reject at first use, far from the site
// that declared it. In developer configs (GE_DEV_DIAG) the create is refused
// and the error names the texture, format and usage; in Release it is logged
// once and left to the API, because a shipped build gains nothing from a
// missing texture it could have drawn with.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Logger/Logger.h"

namespace GameEngine::Rendering
{

/// True when `device` must refuse to create `desc`. Logs the reason either way.
inline bool TextureFormatSupportGateRefuses(const IDevice& device, const TextureDesc& desc)
{
    const auto format = static_cast<TextureFormat>(desc.format);
    if (device.IsTextureFormatSupported(format, desc.usage))
        return false;

    const char* name = desc.debugName ? desc.debugName : "<unnamed>";
    Logger::Log::Error("CreateTexture: '{}' asks for {} with usage {}, which this device reports as "
                       "unsupported; fix the declaration or pick the format through "
                       "IsTextureFormatSupported",
                       name, ToString(format), FormatTextureUsage(desc.usage));
#if defined(GE_DEV_DIAG)
    return true;
#else
    return false;
#endif
}

} // namespace GameEngine::Rendering
