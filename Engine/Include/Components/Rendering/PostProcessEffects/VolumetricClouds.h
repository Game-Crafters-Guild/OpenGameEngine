#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Raymarched volumetric clouds (SebLague/Clouds port). Attach to the same
// entity as a PostProcessVolume. The cloud volume is a disc centered on the
// viewer — Radius wide, spanning [Altitude, Altitude + Thickness] — so the
// layer has no corners to look at and no edge to walk to. Cloud shapes are
// sampled in world space, so they stay put while the disc follows the camera.
// Named skies, each a set of the parameters below. Applying one overwrites
// them; editing any afterwards is what Custom means.
enum class VolumetricCloudsPreset : int32 {
    Custom = 0,
    FairWeatherCumulus,
    Altocumulus,
    StratusOvercast,
    Thunderhead,
    CirrusVeil,
    BrokenCeiling,
};

struct VolumetricClouds {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    VolumetricCloudsPreset Preset{VolumetricCloudsPreset::FairWeatherCumulus};

    // Volume (authored here, not derived from the entity transform)
    float32 Radius{9000.0f};
    float32 Altitude{900.0f};
    float32 Thickness{1400.0f};

    // March
    int32 NumStepsLight{8};
    float32 StepSize{11.0f};
    float32 RayOffsetStrength{10.0f};

    // Base shape
    float32 CloudScale{0.18f};
    float32 DensityMultiplier{1.0f};
    // Coverage threshold: the shape noise must exceed -DensityOffset*0.1 to
    // form cloud, so 0 (upstream's code default) is total overcast — upstream
    // ships its real value in a serialized scene, not in the source. Defaults
    // to broken cumulus, the shape the effect is for.
    float32 DensityOffset{-5.2f};
    float32 ShapeOffset[3]{0.0f, 0.0f, 0.0f};
    float32 ShapeNoiseWeights[4]{1.0f, 0.48f, 0.15f, 0.0f};

    // Detail erosion
    float32 DetailNoiseScale{10.0f};
    float32 DetailNoiseWeight{0.1f};
    float32 DetailNoiseWeights[3]{1.0f, 0.5f, 0.25f};
    float32 DetailOffset[3]{0.0f, 0.0f, 0.0f};

    // Lighting
    float32 LightAbsorptionThroughCloud{1.0f};
    float32 LightAbsorptionTowardSun{1.0f};
    float32 DarknessThreshold{0.2f};
    float32 ForwardScattering{0.83f};
    float32 BackScattering{0.3f};
    float32 BaseBrightness{0.8f};
    float32 PhaseFactor{0.15f};

    // Animation
    float32 TimeScale{1.0f};
    float32 BaseSpeed{0.25f};
    float32 DetailSpeed{0.5f};

    // Temporal accumulation (port addition; 0 = no history reuse)
    float32 HistoryWeight{0.85f};
};

} // namespace Components
} // namespace GameEngine
