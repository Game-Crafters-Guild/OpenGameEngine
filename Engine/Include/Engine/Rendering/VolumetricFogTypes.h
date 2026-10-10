#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

inline constexpr uint32_t kMaxVolumetricFogLocalVolumes = 64;

enum class VolumetricFogDensityMode : int32_t
{
    Additive = 0,
    Subtractive = 1,
    Override = 2,
};

enum class VolumetricFogGradientMode : int32_t
{
    None = 0,
    LocalX = 1,
    LocalY = 2,
    LocalZ = 3,
    ScreenY = 4,
    MainLight = 5,
};

struct VolumetricFogLocalVolume
{
    bool enabled = true;
    int32_t shape = 0;
    VolumetricFogDensityMode densityMode = VolumetricFogDensityMode::Additive;
    VolumetricFogGradientMode gradientMode = VolumetricFogGradientMode::None;
    float weight = 1.0f;
    float density = 0.02f;
    float blendDistance = 0.0f;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float axisX[3] = {1.0f, 0.0f, 0.0f};
    float axisY[3] = {0.0f, 1.0f, 0.0f};
    float axisZ[3] = {0.0f, 0.0f, 1.0f};
    float halfExtents[3] = {0.5f, 0.5f, 0.5f};
    float albedo[3] = {0.82f, 0.78f, 0.72f};
    float emission[3] = {0.0f, 0.0f, 0.0f};
    float gradientLowTint[3] = {1.0f, 1.0f, 1.0f};
    float gradientHighTint[3] = {1.0f, 1.0f, 1.0f};
    float gradientStrength = 0.0f;
    float densityThreshold = 0.0f;
    float densityThresholdSoftness = 0.15f;
};

} // namespace GameEngine::Engine::Renderer
