#pragma once

#include "Components/Rendering/PostProcessEffects/FogGlowQuality.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class HeightFogAxisMode : int32 {
    WorldY = 0,
    WorldX = 1,
    WorldZ = 2,
    Custom = 3,
};

enum class HeightFogGradientMode : int32 {
    None = 0,
    Distance = 1,
    Height = 2,
    ScreenY = 3,
    MainLight = 4,
};

enum class HeightFogLayerMode : int32 {
    Dominant = 0,
    Additive = 1,
};

enum class HeightFogPreset : int32 {
    Custom = 0,
    MorningHaze = 1,
    GroundMist = 2,
    MountainValley = 3,
    HorizonPollution = 4,
    Day = 5,
    Night = 6,
};

// Analytical screen-space height fog. Attach to the same entity as a PostProcessVolume.
struct HeightFogEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    HeightFogPreset Preset{HeightFogPreset::Custom};

    // 0 = bypass, 1 = full contribution.
    float32 Intensity{0.65f};
    // Density is intentionally artist-scaled in [0, 1]; the shader remaps it
    // to extinction over MaxDistance.
    float32 Density{0.28f};
    // Caps the final fog opacity so dense fog cannot wash the whole frame to white.
    float32 MaxOpacity{0.85f};

    bool DistanceFogEnabled{true};
    float32 MinDistance{5.0f};
    float32 SmoothLength{80.0f};
    float32 MaxDistance{1000.0f};
    HeightFogLayerMode LayerMode{HeightFogLayerMode::Dominant};

    bool HeightFogEnabled{true};
    float32 BaseHeight{0.0f};
    float32 TransitionLength{120.0f};
    float32 HorizonHeightOffset{0.0f};
    float32 HorizonHeightBlendStart{0.0f};
    float32 HorizonHeightBlendEnd{0.0f};
    HeightFogAxisMode AxisMode{HeightFogAxisMode::WorldY};
    float32 CustomAxis[3]{0.0f, 1.0f, 0.0f};

    float32 Emissive[3]{0.48f, 0.54f, 0.60f};

    HeightFogGradientMode GradientMode{HeightFogGradientMode::None};
    float32 GradientStrength{0.0f};
    float32 GradientLowColor[3]{0.48f, 0.54f, 0.60f};
    float32 GradientHighColor[3]{0.72f, 0.78f, 0.85f};

    bool TrackDirectionalLight{true};
    float32 SunDirection[3]{0.35f, -0.65f, 0.68f};
    float32 SunColor[3]{1.0f, 0.82f, 0.58f};
    float32 SunIntensity{1.0f};
    float32 SunIntensityScale{1.0f};
    float32 Phase{-0.5f};
    float32 PhaseWeight0{1.0f};
    float32 PhaseWeight1{0.0f};

    bool NoiseEnabled{false};
    float32 NoiseScale{80.0f};
    float32 NoiseStrength{0.28f};
    float32 NoiseVelocity[3]{0.0f, 0.0f, 0.0f};
    float32 NoiseContrast{1.25f};
    float32 NoiseMin{0.0f};
    float32 NoiseMax{1.0f};
    float32 NoiseFadeStart{0.0f};
    float32 NoiseFadeEnd{0.0f};

    bool UseTimeOfDay{false};

    bool SkyEnabled{true};
    float32 SkyPower{1.0f};
    float32 SkyFillStart{0.0f};
    float32 SkyFillEnd{1.0f};
    float32 SkyHorizonOffset{0.0f};
    float32 SkyBottomStrength{0.0f};

    // Energy-normalized screen-space approximation of higher-order fog
    // scattering. The shared renderer combines height and volumetric fog into
    // one pyramid, so enabling both effects does not blur the frame twice.
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
