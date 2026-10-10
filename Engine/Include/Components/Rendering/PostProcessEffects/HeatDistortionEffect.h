#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Screen-space heat-wave refraction. Attach to the same entity as a PostProcessVolume.
struct HeatDistortionEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // 0 = bypass. Default is a visible mirage shimmer without heavy image swim.
    float32 Strength{8.0f};
    // Unitless shader speed for vertically rising thermal shimmer.
    float32 Speed{1.15f};
    float32 Scale{11.0f};
    float32 MaskStrength{1.0f};
    // Normalized camera depth; fades in over the far scene instead of affecting the foreground.
    float32 DistanceStart{0.02f};
    float32 DistanceEnd{0.14f};
    // 0 disables view-direction falloff. Higher values focus shimmer near the horizon.
    float32 DirectionalFalloff{4.0f};
    bool UseAbsoluteY{true};
    // Small full-resolution soft merge in pixels to hide shimmer edge artifacts.
    float32 Softness{1.0f};
};

} // namespace Components
} // namespace GameEngine
