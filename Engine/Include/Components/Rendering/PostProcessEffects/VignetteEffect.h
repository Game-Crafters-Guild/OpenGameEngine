#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-volume optical vignette (LDR stack, applied post-tonemap next to the CAS/LUT stage).
// Attach to the same entity as a PostProcessVolume. URP-exact falloff:
//   color *= lerp(Color, 1, pow(saturate(1 - dot(d, d)), Smoothness * 5))
//   d = |uv - 0.5| * (Intensity * 3);  d.x *= Rounded ? aspect : 1
// Intensity == 0 means pass-through (effect disabled in the shader).
struct VignetteEffect {
    bool    Enabled  {true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Relative execution order among reorderable LDR post FX.
    int32   StackOrder{0};
    // Corner darkening amount (0 = pass-through, higher = darker/larger falloff).
    float32 Intensity{0.0f};
    // Falloff sharpness. Larger values push the transition tighter into the corners.
    float32 Smoothness{0.2f};
    // Aspect-correct the falloff to a circle; otherwise it follows the screen aspect (elliptical).
    bool    Rounded  {false};
    // Display-referred RGB the corners fade toward (default black = plain darkening).
    float32 Color[3] {0.0f, 0.0f, 0.0f};
};

} // namespace Components
} // namespace GameEngine
