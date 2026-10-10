#include "Engine/Rendering/FogSunLightingResolver.h"

#include "Components/Rendering/Light.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace GameEngine::Engine::Renderer {
namespace {

void Normalize3(float v[3])
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float invLen = 1.0f / std::max(len, 0.0001f);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
}

} // namespace

ResolvedFogSun ResolveHeightFogSunLighting(
    RenderServices& services,
    ::GameEngine::Rendering::ViewId viewId,
    bool trackDirectionalLight,
    float sunIntensityScale,
    const float fallbackDirection[3],
    const float fallbackColor[3],
    float fallbackIntensity)
{
    ResolvedFogSun sun{};
    sun.direction[0] = fallbackDirection[0];
    sun.direction[1] = fallbackDirection[1];
    sun.direction[2] = fallbackDirection[2];
    sun.color[0] = fallbackColor[0];
    sun.color[1] = fallbackColor[1];
    sun.color[2] = fallbackColor[2];
    sun.intensity = fallbackIntensity;

    bool foundDirectional = false;
    if (trackDirectionalLight)
    {
        const ViewDesc* view = services.Views().FindViewDesc(viewId);
        const auto lights = view ? services.GetWorldLights(view->worldId) : std::span<const ExtractedLight>{};
        // Fog tracks THE primary directional (the light the world shades
        // with — SelectPrimaryDirectional), never a secondary one. If the
        // primary is non-emitting the sky-anchor fallback below applies;
        // scanning on to a weaker directional would aim the fog sun at a
        // light the surface shading is not using. "Non-emitting" is what it
        // delivers, not its intensity alone: a sky-driven sun at night with
        // the moon hidden is black at its authored intensity.
        const ExtractedLight* light = SelectLitPrimaryDirectional(lights);
        if (light)
        {
            sun.direction[0] = -light->directionWS[0];
            sun.direction[1] = -light->directionWS[1];
            sun.direction[2] = -light->directionWS[2];
            sun.color[0] = std::max(light->color[0], 0.0f);
            sun.color[1] = std::max(light->color[1], 0.0f);
            sun.color[2] = std::max(light->color[2], 0.0f);
            sun.intensity = std::max(light->intensity, 0.0f);
            foundDirectional = true;
        }
    }

    if (auto* skyFeature = services.GetFeature<SkyRenderFeature>(); skyFeature && skyFeature->HasActiveSettings())
    {
        const auto& sky = skyFeature->GetSettings();
        if (trackDirectionalLight && !foundDirectional)
        {
            sun.direction[0] = sky.primarySunDir[0];
            sun.direction[1] = sky.primarySunDir[1];
            sun.direction[2] = sky.primarySunDir[2];
            // Ground level: with no directional light to read the sky is the only source, and fog
            // sits under the atmosphere, so it takes the extinguished colour rather than the
            // above-atmosphere one.
            sun.color[0] = std::max(sky.primarySunGroundColor[0], 0.0f);
            sun.color[1] = std::max(sky.primarySunGroundColor[1], 0.0f);
            sun.color[2] = std::max(sky.primarySunGroundColor[2], 0.0f);
            sun.intensity = std::max(sky.primarySunIntensity * 0.1f, 0.0f);
        }
        // A tracked light needs nothing from the sky: its colour already IS the ground-level sun
        // colour whenever the sky drives it, so scaling it by the source's chromaticity would apply
        // the same hue twice — the moon's at night, an authored sun tint squared.
    }

    Normalize3(sun.direction);
    sun.intensity *= std::max(sunIntensityScale, 0.0f);
    return sun;
}

} // namespace GameEngine::Engine::Renderer
