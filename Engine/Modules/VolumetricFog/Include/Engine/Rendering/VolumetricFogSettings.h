#pragma once

#include "Engine/Rendering/VolumetricFogTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{

struct VolumetricFogSettings
{
    bool enabled = false;
    bool isGlobal = true;
    bool localVolumeValid = false;
    int32_t localVolumeShape = 0;
    float localVolumeBlendDistance = 0.0f;
    float localVolumeCenter[3] = {0.0f, 0.0f, 0.0f};
    float localVolumeAxisX[3] = {1.0f, 0.0f, 0.0f};
    float localVolumeAxisY[3] = {0.0f, 1.0f, 0.0f};
    float localVolumeAxisZ[3] = {0.0f, 0.0f, 1.0f};
    float localVolumeHalfExtents[3] = {0.5f, 0.5f, 0.5f};

    float maxDistance = 160.0f;
    uint32_t xyCellSizePixels = 8;
    uint32_t zSliceCount = 128;
    float depthDistribution = 1.6f;

    float density = 0.02f;
    float baseHeight = 0.0f;
    float heightFalloff = 30.0f;
    float skyFade = 0.5f;
    float albedo[3] = {0.82f, 0.78f, 0.72f};
    float emission[3] = {0.0f, 0.0f, 0.0f};

    float anisotropy = 0.35f;
    bool trackDirectionalLight = true;
    float sunIntensityScale = 1.0f;
    float sunScatteringTint[3] = {1.0f, 0.78f, 0.52f};
    float ambientScatteringTint[3] = {0.32f, 0.38f, 0.48f};

    bool noiseEnabled = true;
    float noiseScale = 65.0f;
    float noiseStrength = 0.35f;
    float noiseVelocity[3] = {0.0f, 0.0f, 0.0f};
    float noiseContrast = 1.25f;
    float noiseChannelWeights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float densityThreshold = 0.0f;
    float densityThresholdSoftness = 0.15f;

    bool filterEnabled = true;
    bool temporalEnabled = true;
    float temporalBlend = 0.92f;
    float jitterStrength = 0.45f;
    bool jitterMotion = false;
    float compositeDepthBias = 0.0f;
    float shadowBias = 0.0f;
    std::vector<VolumetricFogLocalVolume> localVolumes;
};

struct VolumetricFogGrid
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t slices = 1;
};

inline VolumetricFogGrid ComputeVolumetricFogGrid(
    uint32_t renderWidth,
    uint32_t renderHeight,
    const VolumetricFogSettings& settings)
{
    const uint32_t cell = std::clamp(settings.xyCellSizePixels, 1u, 64u);
    VolumetricFogGrid grid{};
    grid.width = std::max(1u, (renderWidth + cell - 1u) / cell);
    grid.height = std::max(1u, (renderHeight + cell - 1u) / cell);
    grid.slices = std::clamp(settings.zSliceCount, 8u, 256u);
    return grid;
}

inline float VolumetricFogSliceToDistance(float normalizedSlice, const VolumetricFogSettings& settings)
{
    const float s = std::clamp(normalizedSlice, 0.0f, 1.0f);
    const float exponent = std::max(settings.depthDistribution, 0.05f);
    return std::max(settings.maxDistance, 0.01f) * std::pow(s, exponent);
}

inline float VolumetricFogDistanceToSlice(float distance, const VolumetricFogSettings& settings)
{
    const float maxDistance = std::max(settings.maxDistance, 0.01f);
    const float exponent = std::max(settings.depthDistribution, 0.05f);
    return std::pow(std::clamp(distance / maxDistance, 0.0f, 1.0f), 1.0f / exponent);
}

} // namespace GameEngine::Engine::Renderer
