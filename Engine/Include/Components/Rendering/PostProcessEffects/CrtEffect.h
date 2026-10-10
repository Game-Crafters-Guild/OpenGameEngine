#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// CRT-style post effect (barrel warp, scanlines, vignette, RGB split).
// Attach to the same entity as a PostProcessVolume.
struct CrtEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Relative execution order among reorderable LDR post FX.
    int32 StackOrder{2};
    // 0 = bypass (shader skips the look), 1 = full effect.
    float32 Intensity{1.0f};
    // Screen curvature / barrel warp (typical 0–0.35). 0 = flat (no barrel warp).
    float32 Curvature{0.0f};
    // Darkness of horizontal scanlines (0–1).
    float32 Scanlines{0.12f};
    // Edge darkening (0–1).
    float32 Vignette{0.25f};
    // Subtle red/blue channel separation in UV space (0–0.01 typical).
    float32 Aberration{0.0025f};
    // Pixel filtering softness (0 = sharper texel edges, 1 = softer phosphor blend).
    float32 Softness{0.0f};
    // Auto-lifts CRT output to compensate scanline darkening (derived from Scanlines amount).
    bool ExposureCompensation{true};
    // CRT emulated-resolution divisor (Lottes defaults to /6). 0 = shader uses 6.0.
    // Set equal to Scene View pixel-perfect scale (or game integer scale) so scan/emulated rows
    // line up with logical pixels.
    float32 EmulatedResolutionDiv{0.0f};
};

} // namespace Components
} // namespace GameEngine
