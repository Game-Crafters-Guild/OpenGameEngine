#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

inline constexpr float32 kAtmosphericCloudColorMaxIntensity = 8.0f;

// World-space atmospheric cloud volume. Attach to the same entity as a PostProcessVolume.
struct AtmosphericCloudLayer {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 SkyFill{0.48f};
    float32 VaporMass{0.62f};
    float32 CloudColor[3]{1.0f, 1.0f, 1.0f};
    float32 Opacity{1.0f};
    float32 FloorHeight{900.0f};
    float32 LayerDepth{1800.0f};
    float32 BodyFrequency{0.72f};
    float32 EdgeFrequency{3.2f};
    float32 EdgeBreakup{0.58f};
    float32 DriftAngle{24.0f};
    float32 DriftRate{0.08f};
    float32 SunFade{0.55f};
    float32 SkyBounce{0.32f};
    float32 RimBoost{0.42f};
    float32 Occlusion{0.55f};
    float32 HistoryWeight{0.90f};
    float32 PixelScale{0.75f};
};

} // namespace Components
} // namespace GameEngine
