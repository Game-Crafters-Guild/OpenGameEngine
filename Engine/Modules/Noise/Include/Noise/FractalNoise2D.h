#pragma once

#include "Noise/GradientNoise2D.h"

#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::Noise
{

/// @brief Upper bound on the octave count any fractal sum here will run.
///
/// Callers are expected to clamp authored counts themselves; this is the
/// backstop. The loops run per texel, so an unclamped count from a hand-edited
/// or corrupt source would otherwise turn a bake into a hang.
inline constexpr uint32 kMaxOctaves = 8u;

/// @brief Seed step between octaves, so each octave hashes a decorrelated
/// lattice instead of repeating the previous one at a finer scale.
inline constexpr uint32 kOctaveSeedStride = 31u;

/// @brief Fractional Brownian motion over the gradient-noise lattice.
///
/// Each octave multiplies frequency by @p lacunarity and amplitude by
/// @p persistence. The octave count is clamped to kMaxOctaves.
inline float32 FBMNoise2D(float32 x, float32 z, float32 frequency, float32 amplitude,
                          uint32 octaves, uint32 seed, float32 lacunarity, float32 persistence)
{
    float32 value = 0.0f;
    float32 freq = frequency;
    float32 amp = amplitude;
    const uint32 count = std::min(octaves, kMaxOctaves);
    for (uint32 i = 0; i < count; ++i)
    {
        value += GradientNoise2D(x * freq, z * freq, seed + i * kOctaveSeedStride) * amp;
        freq *= lacunarity;
        amp *= persistence;
    }
    return value;
}

/// @brief FBMNoise2D carrying the analytic gradient of the sum.
///
/// Value reproduces FBMNoise2D expression for expression, so the two agree bit
/// for bit; the derivatives accumulate through the same octave loop.
inline NoiseSample FBMNoise2DWithDerivatives(float32 x, float32 z, float32 frequency,
                                             float32 amplitude, uint32 octaves, uint32 seed,
                                             float32 lacunarity, float32 persistence)
{
    NoiseSample out{};
    float32 freq = frequency;
    float32 amp = amplitude;
    const uint32 count = std::min(octaves, kMaxOctaves);
    for (uint32 i = 0; i < count; ++i)
    {
        const NoiseSample n =
            GradientNoise2DWithDerivatives(x * freq, z * freq, seed + i * kOctaveSeedStride);
        out.Value += n.Value * amp;
        // Chain rule through the octave's frequency scaling of x and z.
        out.DValueDX += n.DValueDX * freq * amp;
        out.DValueDZ += n.DValueDZ * freq * amp;
        freq *= lacunarity;
        amp *= persistence;
    }
    return out;
}

} // namespace GameEngine::Noise
