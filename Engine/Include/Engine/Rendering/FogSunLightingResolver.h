#pragma once

#include "Rendering/CameraTypes.h"

namespace GameEngine::Engine::Renderer {

class RenderServices;

struct ResolvedFogSun
{
    float direction[3]{0.35f, -0.65f, 0.68f};
    float color[3]{1.0f, 0.86f, 0.62f};
    float intensity = 1.0f;
};

ResolvedFogSun ResolveHeightFogSunLighting(
    RenderServices& services,
    ::GameEngine::Rendering::ViewId viewId,
    bool trackDirectionalLight,
    float sunIntensityScale,
    const float fallbackDirection[3],
    const float fallbackColor[3],
    float fallbackIntensity);

} // namespace GameEngine::Engine::Renderer
