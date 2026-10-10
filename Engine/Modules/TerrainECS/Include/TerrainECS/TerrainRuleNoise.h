#pragma once

#include "Noise/GradientNoise2D.h"
#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

// The field a surface rule's Noise condition bands: the engine's gradient-noise basis at
// unit amplitude, remapped from its roughly [-1,1] range into [0,1] so a band can be
// authored against a fixed domain. ONE octave — a condition wants broad blotches to break
// up a material edge, not a heightfield's detail; a second scale is a second condition.
//
// This calls the lattice DIRECTLY where the bake calls FBMNoise2D(..., 1.0f, 1, seed, ...).
// The two are bit-identical rather than merely equivalent: at octaves = 1 the fBM loop runs
// exactly one iteration, so it reduces to `GradientNoise2D(x*frequency, z*frequency, seed)`
// scaled by 1.0f — and multiplying a float by 1.0f is the identity for every value, NaN and
// the infinities included.
//
// The basis itself is shared (Engine/Modules/Noise) because three things must agree on it to
// the bit: this bake, the GPU height kernel's mirror in terrain_height_bake.comp, and the GPU
// splat kernel's mirror in terrain_surface_rules.glsl. Swept against the GLSL mirror by
// TerrainSurfaceRuleGpuParity.NoiseFieldMatchesExactly.
inline float32 SurfaceRuleNoiseSample(float32 worldX, float32 worldZ, float32 frequency,
                                      uint32 seed)
{
    return std::clamp(
        0.5f + 0.5f * Noise::GradientNoise2D(worldX * frequency, worldZ * frequency, seed),
        0.0f, 1.0f);
}

} // namespace GameEngine::TerrainECS
