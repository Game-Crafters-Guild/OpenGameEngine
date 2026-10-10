#pragma once

#include "Types/Types.h"

namespace GameEngine::Engine::Renderer {

// std140 layout shared by height_fog.frag and HeightFogParamsUploadNode.
struct HeightFogParamsUBO
{
    float intensity = 0.0f;
    float density = 0.0f;
    float minDistance = 0.0f;
    float smoothLength = 1.0f;
    float baseHeight = 0.0f;
    float transitionLength = 120.0f;
    float maxDistance = 1000.0f;
    float gradientStrength = 0.0f;
    float emissiveR = 0.48f;
    float emissiveG = 0.54f;
    float emissiveB = 0.60f;
    float sunIntensity = 1.0f;
    float sunDirX = 0.35f;
    float sunDirY = -0.65f;
    float sunDirZ = 0.68f;
    float phase = -0.5f;
    float sunColorR = 1.0f;
    float sunColorG = 0.82f;
    float sunColorB = 0.58f;
    float phaseWeight0 = 1.0f;
    float phaseWeight1 = 0.0f;
    float skyPower = 1.0f;
    float skyFillStart = 0.0f;
    float skyFillEnd = 1.0f;
    float axisX = 0.0f;
    float axisY = 1.0f;
    float axisZ = 0.0f;
    float noiseScale = 80.0f;
    float gradientLowR = 0.48f;
    float gradientLowG = 0.54f;
    float gradientLowB = 0.60f;
    float noiseStrength = 0.0f;
    float gradientHighR = 0.72f;
    float gradientHighG = 0.78f;
    float gradientHighB = 0.85f;
    float noiseContrast = 1.25f;
    float noiseVelX = 0.0f;
    float noiseVelY = 0.0f;
    float noiseVelZ = 0.0f;
    float noiseTime = 0.0f;
    int32 flags = 0;
    int32 axisMode = 0;
    int32 gradientMode = 0;
    float maxOpacity = 0.85f;
    int32 layerMode = 0;
    float horizonHeightOffset = 0.0f;
    float horizonHeightBlendStart = 0.0f;
    float horizonHeightBlendEnd = 0.0f;
    float noiseMin = 0.0f;
    float noiseMax = 1.0f;
    float noiseFadeStart = 0.0f;
    float noiseFadeEnd = 0.0f;
    float skyHorizonOffset = 0.0f;
    float skyBottomStrength = 0.0f;
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
};

static_assert(sizeof(HeightFogParamsUBO) == 224, "HeightFogParamsUBO must be 224 bytes");

enum HeightFogFlags : int32
{
    HeightFogFlagDistanceEnabled = 1 << 0,
    HeightFogFlagHeightEnabled = 1 << 1,
    HeightFogFlagNoiseEnabled = 1 << 2,
    HeightFogFlagSkyEnabled = 1 << 3,
};

void FillHeightFogParamsUBO(const struct PostProcessSettings& settings, HeightFogParamsUBO& out);

} // namespace GameEngine::Engine::Renderer
