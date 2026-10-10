// SPDX-License-Identifier: MPL-2.0
//
// Erosion filter, from the technique "Fast and Gorgeous Erosion Filter" by Rune
// Skovbo Johansen (https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html),
// lineage Clay John 2018 / Felix Westin 2023 / Johansen 2026.
// Adapted here as an additive offset over the engine's own fBM basis, with an
// analytic gradient and an authorable edge-rounding knob; the modifications are
// listed in ThirdParty/RunevisionErosionFilter/UPSTREAM.md.
//
// MPL v2 covers this file and ErosionFilter.h and nothing else — every
// modification to the technique stays inside the pair. The fBM basis this
// filter carves is the engine's own and stays under the engine's licence;
// calling it from here does not extend MPL to it.

#include "TerrainECS/Erosion/ErosionFilter.h"

#include "Mathematics/Interpolation.h"
#include "Noise/FractalNoise2D.h"
#include "Noise/GradientNoise2D.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS::Erosion
{
namespace
{

constexpr float32 kTwoPi = 6.28318530717958647692f;

// Stripe wavelength is one cell, so the pivot jitter is a fraction of a cell.
// At full jitter pivots from adjacent cells can coincide, which collapses the
// blend back to a single-pivot rotation and brings back the distortion the cell
// grid exists to avoid.
constexpr float32 kCellJitter = 0.7f;

// Partial normalisation gain. Every blended magnitude is scaled by this, then
// capped at 1: magnitudes at or above 1/gain reach full amplitude, and smaller
// ones are scaled by the same gain rather than all the way to 1. Normalising
// them fully would amplify waves that nearly cancel into spikes.
constexpr float32 kGullyNormalizeGain = 2.0f;

constexpr float32 kErosionLacunarity = 2.0f;
constexpr float32 kErosionPersistence = 0.5f;

// Base-noise gradient magnitude that reads as "fully steep" for the slope mask,
// in units of the fBM's own amplitude envelope times its frequency.
constexpr float32 kSlopeReference = 2.0f;

// First erosion octave's amplitude as a fraction of the base fBM envelope, so
// ErosionStrength 1 is a strong carve rather than an unusable one.
constexpr float32 kErosionAmplitudeScale = 0.35f;

constexpr float32 kMinStripeMagnitude = 1e-6f;
constexpr float32 kMinGradientLength = 1e-8f;

// Decorrelates each gully octave's cell lattice. Serves the same purpose as the
// base fBM's own octave stride, at a different value: the two lattices are
// independent, so sharing a stride would correlate the gullies with the terrain
// they carve.
constexpr uint32 kOctaveSeedStride = 131u;

// Upstream helper. Starts gently instead of vertically — sqrt's shape without
// sqrt's infinite slope at 0, which is what keeps peaks and valleys from
// showing a hard erosion onset.
float32 EaseOut(float32 t)
{
    const float32 inv = 1.0f - Math::Clamp01(t);
    return 1.0f - inv * inv;
}

// Upstream helper. A power below 1 pulls the result DOWN — (1-t)^p exceeds
// (1-t) there, so 1-(1-t)^p falls below t — which is how a finer octave is
// confined to ground the coarser one already steepened. Power 1 is identity;
// above 1 the mask opens up instead.
float32 PowInv(float32 t, float32 power)
{
    return 1.0f - std::pow(1.0f - Math::Clamp01(t), power);
}

// Cell jitter in [-0.5, 0.5] before the kCellJitter scale.
float32 JitterFromHash(uint32 h)
{
    return static_cast<float32>(h & 0xFFFFFFu) * (1.0f / 16777216.0f) - 0.5f;
}

// The stripe wave from the four cells whose jittered pivots bracket the sample,
// blended by the same smoothstep weights the noise lattice uses. Unaligned
// waves of one frequency average to a lower-amplitude wave of that frequency,
// which is what hides the cell seams; the amplitude loss is what
// PartialNormalize then undoes.
//
// All four cells share one wave axis, taken from the gradient at the sample.
// The pivots differ, so what varies between cells is the phase, not the
// direction — that is the point of the cell grid: it keeps the rotation arm
// (sample minus pivot) short, instead of rotating the whole plane about one
// distant origin.
void BlendedStripes(float32 px, float32 pz, float32 axisX, float32 axisZ, uint32 seed,
                    float32& outCos, float32& outSin)
{
    const float32 gx = px - 0.5f;
    const float32 gz = pz - 0.5f;
    const int32 cx = static_cast<int32>(std::floor(gx));
    const int32 cz = static_cast<int32>(std::floor(gz));
    const float32 fx = gx - static_cast<float32>(cx);
    const float32 fz = gz - static_cast<float32>(cz);
    const float32 wx = fx * fx * (3.0f - 2.0f * fx);
    const float32 wz = fz * fz * (3.0f - 2.0f * fz);

    outCos = 0.0f;
    outSin = 0.0f;
    for (int32 dz = 0; dz <= 1; ++dz)
    {
        for (int32 dx = 0; dx <= 1; ++dx)
        {
            const uint32 h = Noise::LatticeHash(cx + dx, cz + dz, seed);
            const float32 pivotX = static_cast<float32>(cx + dx) + 0.5f +
                                   JitterFromHash(h) * kCellJitter;
            const float32 pivotZ = static_cast<float32>(cz + dz) + 0.5f +
                                   JitterFromHash(h * 2654435761u) * kCellJitter;
            const float32 phase = kTwoPi * ((px - pivotX) * axisX + (pz - pivotZ) * axisZ);
            const float32 w = (dx != 0 ? wx : 1.0f - wx) * (dz != 0 ? wz : 1.0f - wz);
            outCos += w * std::cos(phase);
            outSin += w * std::sin(phase);
        }
    }
}

// Upstream's partial normalisation: treat the blended pair as a point that
// should sit on the unit circle and push it back out by a bounded gain rather
// than all the way, so that where the waves nearly cancel the result stays
// small instead of becoming a spike.
void PartialNormalize(float32& waveCos, float32& waveSin)
{
    const float32 mag = std::sqrt(waveCos * waveCos + waveSin * waveSin);
    if (mag <= kMinStripeMagnitude)
        return;
    const float32 target = std::min(mag * kGullyNormalizeGain, 1.0f);
    const float32 scale = target / mag;
    waveCos *= scale;
    waveSin *= scale;
}

} // namespace

float32 ErodedFBMNoise2D(float32 x, float32 z, float32 frequency, float32 amplitude,
                         uint32 octaves, uint32 seed, float32 lacunarity, float32 persistence,
                         const ErosionParams& params)
{
    const Noise::NoiseSample base = Noise::FBMNoise2DWithDerivatives(
        x, z, frequency, amplitude, octaves, seed, lacunarity, persistence);
    if (params.Strength <= 0.0f || params.Octaves == 0u || frequency <= 0.0f)
        return base.Value;

    // Peak envelope of the base fBM — the sum of its octave amplitudes. Every
    // erosion quantity is measured against it, so one set of erosion parameters
    // reads the same way at any terrain height scale or world size.
    float32 envelope = 0.0f;
    {
        float32 amp = amplitude;
        // Same bound the value loop above used, or the envelope would describe
        // an fBM with more octaves than was actually summed.
        const uint32 count = std::min(octaves, Noise::kMaxOctaves);
        for (uint32 i = 0; i < count; ++i)
        {
            envelope += std::abs(amp);
            amp *= persistence;
        }
    }
    if (envelope <= 0.0f)
        return base.Value;

    // Altitude in [-1, 1]. Gullies fade toward both extremes: at a peak or a
    // valley floor the gradient vanishes, so the flow direction the stripes are
    // rotated by is undefined and unfaded stripes would read as pure noise.
    const float32 altitude = std::clamp(base.Value / envelope, -1.0f, 1.0f);
    const float32 altitudeFade = 1.0f - Math::Clamp01(params.Fade) * std::abs(altitude);

    // Slope made dimensionless. A gradient of this basis runs about
    // envelope * frequency, so dividing by that puts ordinary ground near 1 and
    // lets one kSlopeReference serve every terrain scale.
    const float32 slopeNorm = 1.0f / (envelope * frequency * kSlopeReference);
    const float32 slope = std::sqrt(base.DValueDX * base.DValueDX +
                                    base.DValueDZ * base.DValueDZ) * slopeNorm;
    const float32 slopeMask = EaseOut(slope);

    const float32 detail = params.Detail > 0.0f ? params.Detail : 1.0f;
    const float32 rounding = Math::Clamp01(params.EdgeRounding);

    float32 flowGX = base.DValueDX;
    float32 flowGZ = base.DValueDZ;
    float32 combiMask = slopeMask * altitudeFade;
    float32 eFreq = frequency * (params.Frequency > 0.0f ? params.Frequency : 1.0f);
    float32 eAmp = envelope * kErosionAmplitudeScale;
    float32 total = 0.0f;

    const uint32 erosionOctaves = std::min(params.Octaves, Noise::kMaxOctaves);
    for (uint32 i = 0; i < erosionOctaves; ++i)
    {
        // Water runs down the gradient, and a gully is a channel that follows it
        // — so the height varies ACROSS the flow, not along it, and the wave
        // axis is the flow's perpendicular.
        const float32 len = std::sqrt(flowGX * flowGX + flowGZ * flowGZ);
        float32 axisX = 1.0f;
        float32 axisZ = 0.0f;
        if (len > kMinGradientLength)
        {
            axisX = -flowGZ / len;
            axisZ = flowGX / len;
        }

        float32 waveCos = 0.0f;
        float32 waveSin = 0.0f;
        BlendedStripes(x * eFreq, z * eFreq, axisX, axisZ, seed + i * kOctaveSeedStride,
                       waveCos, waveSin);
        PartialNormalize(waveCos, waveSin);

        total += eAmp * waveCos * combiMask;

        // Feed this gully's own slope back into the flow so the next, finer
        // octave branches off it instead of repeating it. Upstream's "straight
        // gullies" uses sign(sin) rather than sin: a constant slope from ridge
        // to crease makes fine gullies meet at sharp angles instead of curling.
        // EdgeRounding blends back toward the plain sine for softer ground.
        const float32 straight = waveSin < 0.0f ? -1.0f : 1.0f;
        const float32 slopeTerm = straight * (1.0f - rounding) + waveSin * rounding;
        const float32 gullyGrad = -kTwoPi * eFreq * eAmp * combiMask *
                                  params.GullyWeight * slopeTerm;
        flowGX += gullyGrad * axisX;
        flowGZ += gullyGrad * axisZ;

        combiMask = PowInv(combiMask, detail) * slopeMask * altitudeFade;
        eAmp *= kErosionPersistence;
        eFreq *= kErosionLacunarity;
    }

    return base.Value + params.Strength * total;
}

} // namespace GameEngine::TerrainECS::Erosion
