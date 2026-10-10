#pragma once

#include "Components/Rendering/PostProcessEffects/FogGlowQuality.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class VolumetricFogDensityMode : int32 {
    Additive = 0,
    Subtractive = 1,
    Override = 2,
};

enum class VolumetricFogGradientMode : int32 {
    None = 0,
    LocalX = 1,
    LocalY = 2,
    LocalZ = 3,
    ScreenY = 4,
    MainLight = 5,
};

// Froxel-grid volumetric fog post effect.
// Attach to the same entity as a PostProcessVolume.
struct VolumetricFogEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{1.0f};
    float32 MaxDistance{260.0f};
    int32 XYCellSizePixels{8};
    int32 ZSliceCount{128};
    float32 DepthDistribution{1.6f};
    float32 Density{0.035f};
    float32 BaseHeight{0.0f};
    float32 HeightFalloff{24.0f};
    float32 SkyFade{0.5f}; // softness of the distant fog->sky join (0 = hard edge, 1 = very gradual)
    float32 Albedo[3]{0.82f, 0.78f, 0.72f};
    float32 Emission[3]{0.0f, 0.0f, 0.0f};
    float32 Anisotropy{0.55f};
    bool TrackDirectionalLight{true};
    float32 SunIntensityScale{1.0f};
    float32 SunScatteringTint[3]{1.0f, 0.72f, 0.42f};
    float32 AmbientScatteringTint[3]{0.32f, 0.38f, 0.48f};
    bool NoiseEnabled{true};
    float32 NoiseScale{80.0f};
    float32 NoiseStrength{0.28f};
    float32 NoiseVelocity[3]{0.0f, 0.0f, 0.0f};
    float32 NoiseContrast{1.25f};
    float32 NoiseChannelWeights[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float32 DensityThreshold{0.0f};
    float32 DensityThresholdSoftness{0.15f};
    VolumetricFogDensityMode DensityMode{VolumetricFogDensityMode::Additive};
    VolumetricFogGradientMode GradientMode{VolumetricFogGradientMode::None};
    float32 GradientStrength{0.0f};
    float32 GradientLowTint[3]{1.0f, 1.0f, 1.0f};
    float32 GradientHighTint[3]{1.0f, 1.0f, 1.0f};
    bool TemporalEnabled{true};
    float32 TemporalBlend{0.92f};
    float32 JitterStrength{0.45f};
    bool JitterMotion{false};
    float32 CompositeDepthBias{0.0f};
    float32 ShadowBias{0.0f};
    bool FogGlowEnabled{false};
    FogGlowQuality FogGlowQualityLevel{FogGlowQuality::High};
    float32 FogGlowIntensity{0.0f};
    float32 FogGlowRadius{3.0f};
    int32 FogGlowOctaves{6};
    float32 FogGlowScatter{0.7f};
    float32 FogGlowThreshold{0.0f};
    float32 FogGlowKnee{0.5f};
    float32 FogGlowFadeStart{0.05f};
    float32 FogGlowFadeEnd{0.8f};
    float32 FogGlowTint[3]{1.0f, 1.0f, 1.0f};
    bool FogGlowAntiFlicker{true};
};

} // namespace Components
} // namespace GameEngine
