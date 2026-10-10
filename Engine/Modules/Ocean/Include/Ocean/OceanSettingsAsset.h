#pragma once

#include "Ocean/OceanCollisionProvider.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>

namespace GameEngine::Ocean
{

enum class OceanSettingsKind : uint32
{
    AnimatedWavesCollision = 0u,
    DynamicWaves = 1u,
    Foam = 2u,
    Shadows = 3u,
};

struct OceanAnimatedWaveCollisionSettings
{
    OceanCollisionProviderMode Provider = OceanCollisionProviderMode::Auto;
    uint32 MaximumQueryPoints = 8192u;
    float32 DefaultMinimumSpatialLength = 0.0f;
    bool AllowGPUQueries = true;
    bool AllowBakedFFT = true;
    bool AllowGerstnerFallback = true;
};

struct OceanDynamicWaveSettings
{
    float32 SimulationFrequency = 60.0f;
    uint32 MaximumSubsteps = 4u;
    float32 Damping = 0.2f;
    float32 CourantNumber = 0.7f;
    float32 Gravity = 9.81f;
    float32 ShallowAttenuation = 1.0f;
    float32 HorizontalDisplacement = 0.0f;
    float32 DisplacementClamp = 4.0f;
    uint32 MinimumCascade = 0u;
    uint32 MaximumCascade = 6u;
};

struct OceanFoamSettings
{
    float32 SimulationFrequency = 30.0f;
    uint32 MaximumSubsteps = 2u;
    float32 FadeRate = 0.8f;
    float32 WaveCoverage = 0.55f;
    float32 WaveStrength = 1.0f;
    float32 ShorelineStrength = 2.0f;
    float32 FlowAdvection = 1.0f;
};

struct OceanShadowSettings
{
    bool Enabled = false;
    float32 SimulationFrequency = 30.0f;
    float32 TemporalWeight = 0.92f;
    float32 JitterDiameter = 15.0f;
    float32 HardJitterDiameter = 0.6f;
    float32 HardChannelScale = 1.0f;
    float32 SoftChannelScale = 1.0f;
    uint32 CascadeCount = 4u;
    uint32 Resolution = 256u;
};

class OceanSettingsAsset
{
public:
    static constexpr uint32 kFormatVersion = 1u;

    OceanSettingsKind Kind = OceanSettingsKind::AnimatedWavesCollision;
    OceanAnimatedWaveCollisionSettings AnimatedWavesCollision{};
    OceanDynamicWaveSettings DynamicWaves{};
    OceanFoamSettings Foam{};
    OceanShadowSettings Shadows{};

    bool Validate(std::string* error = nullptr) const;
    bool SaveJson(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool LoadJson(const std::filesystem::path& path, std::string* error = nullptr);
};

} // namespace GameEngine::Ocean
