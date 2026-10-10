#pragma once

#include "Types/Types.h"

namespace GameEngine::Components {

// Lens chromatic aberrations, screen-space approximations:
// - Lateral CA: wavelength-dependent radial magnification, grows toward the edge.
// - Longitudinal (axial) CA: wavelength-dependent focus — red/blue defocus while
//   green stays sharp. The approximation follows camera focus and resolved depth;
//   a fully physical per-wavelength circle of confusion belongs in the DoF model.
// - Coma: off-axis highlight smear into radial comet tails, grows toward the edge.
struct ChromaticAberrationEffect {
    static constexpr float32 kIntensityMax = 12.0f;
    static constexpr float32 kLongitudinalMax = 8.0f;
    static constexpr float32 kComaMax = 64.0f;

    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{1.0f}; // lateral: maximum red/blue separation in pixels
    float32 StartOffset{0.25f}; // normalized radius where lateral/coma begin
    float32 Saturation{1.0f};
    float32 LongitudinalIntensity{0.0f}; // axial: red/blue defocus radius in pixels
    float32 ComaIntensity{0.0f}; // radial highlight tail length in pixels at the corner
};

} // namespace GameEngine::Components
