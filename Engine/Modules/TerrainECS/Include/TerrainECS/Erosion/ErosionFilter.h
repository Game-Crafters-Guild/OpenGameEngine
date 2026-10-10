// SPDX-License-Identifier: MPL-2.0
//
// Erosion filter, from the technique "Fast and Gorgeous Erosion Filter" by Rune
// Skovbo Johansen (https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html),
// lineage Clay John 2018 / Felix Westin 2023 / Johansen 2026.
// Adapted here as an additive offset over the engine's own fBM basis, with an
// analytic gradient and an authorable edge-rounding knob; the modifications are
// listed in ThirdParty/RunevisionErosionFilter/UPSTREAM.md.
//
// MPL v2 covers this file and ErosionFilter.cpp and nothing else — every
// modification to the technique stays inside the pair. Including this header
// does not place the including file under MPL. The fBM basis this filter carves
// is the engine's own (Engine/Modules/Noise) and stays under the engine's
// licence; calling it from here does not extend MPL to it.

#pragma once

#include "Types/Types.h"

namespace GameEngine::TerrainECS::Erosion
{

// Erosion block parameters, mirroring TerrainNoiseEffect's Erosion* fields one
// for one and named so they lift onto a standalone erosion component without a
// rename. TerrainNoiseEffect owns the authoring defaults; this struct carries
// none deliberately, because it is stored inside a union whose other members
// would leave it indeterminate — a value-initialized ErosionParams must mean
// "disarmed", and all-zero Strength is exactly that.
struct ErosionParams
{
    // 0 disables the filter. Callers take the untouched fBM path in that case,
    // which is what makes strength 0 byte-identical to a pre-erosion bake.
    float32 Strength;

    uint32 Octaves;

    // Multiplier on the base noise frequency: where the first gully octave sits
    // relative to the terrain it carves.
    float32 Frequency;

    // How tightly the finer octaves stay confined to already-steep ground.
    // Below 1 tightens the confinement; 1 is none; above 1 opens it up.
    float32 Detail;

    // How strongly a gully's own slope steers the next octave's flow direction.
    // This is what makes gullies branch rather than repeat.
    float32 GullyWeight;

    // 0 = triangle-wave gullies, constant slope from ridge to crease, which
    // branch at sharp angles. 1 = pure sine, rounded ridges and gentle creases.
    float32 EdgeRounding;

    // How completely gullies fade out at peaks and valleys, where the flow
    // direction is undefined and unfaded stripes read as noise.
    float32 Fade;
};

// fBM with the erosion filter applied, as a single point-local pure function of
// (x, z). Returns the plain fBM value when params.Strength <= 0, but callers
// should skip the call entirely in that case rather than rely on it.
float32 ErodedFBMNoise2D(float32 x, float32 z, float32 frequency, float32 amplitude,
                         uint32 octaves, uint32 seed, float32 lacunarity, float32 persistence,
                         const ErosionParams& params);

} // namespace GameEngine::TerrainECS::Erosion
