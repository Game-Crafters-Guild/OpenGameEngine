// Shared-memory radix-2 butterfly IFFT. Faithful port of the reference
// FFTCompute.compute::ButterflyPass + ComputeFFT (MIT). Processes the three
// channels (height, displaceX, displaceZ) together. The twiddle weights come
// from a precomputed butterfly table (binding 4), generated CPU-side exactly as
// the reference's InitializeButterfly.

#ifndef GE_OCEAN_FFT_BUTTERFLY_GLSL
#define GE_OCEAN_FFT_BUTTERFLY_GLSL

#include "Ocean/ocean_fft_common.glsl"

// Twiddle table: width = GE_FFT_SIZE, height = GE_FFT_PASSES. (cos, -sin) per
// (coord, pass). Bound as a read-only storage image (no filtering, integer load).
layout(set = 0, binding = 4, rgba32f) uniform readonly image2D uButterfly;

shared vec2 sIntH[GE_FFT_SIZE];
shared vec2 sScrH[GE_FFT_SIZE];
shared vec2 sIntX[GE_FFT_SIZE];
shared vec2 sScrX[GE_FFT_SIZE];
shared vec2 sIntZ[GE_FFT_SIZE];
shared vec2 sScrZ[GE_FFT_SIZE];

void OceanButterflyPass(vec2 butterfly, uint coord, uint passIndex)
{
    uint indexA;
    uint indexB;

    const uint offset = 1u << passIndex;
    if ((coord / offset) % 2u == 1u)
    {
        indexA = coord - offset;
        indexB = coord;
    }
    else
    {
        indexA = coord;
        indexB = coord + offset;
    }

    if (passIndex == 0u)
    {
        indexA = bitfieldReverse(indexA) >> (32 - GE_FFT_PASSES);
        indexB = bitfieldReverse(indexB) >> (32 - GE_FFT_PASSES);
    }

    const bool pingpong = (passIndex % 2u) == 0u;

    vec2 aH, bH, aX, bX, aZ, bZ;
    if (pingpong)
    {
        aH = sIntH[indexA]; bH = sIntH[indexB];
        aX = sIntX[indexA]; bX = sIntX[indexB];
        aZ = sIntZ[indexA]; bZ = sIntZ[indexB];
    }
    else
    {
        aH = sScrH[indexA]; bH = sScrH[indexB];
        aX = sScrX[indexA]; bX = sScrX[indexB];
        aZ = sScrZ[indexA]; bZ = sScrZ[indexB];
    }

    const vec2 rH = aH + OceanCMul(butterfly, bH);
    const vec2 rX = aX + OceanCMul(butterfly, bX);
    const vec2 rZ = aZ + OceanCMul(butterfly, bZ);

    if (pingpong)
    {
        sScrH[coord] = rH; sScrX[coord] = rX; sScrZ[coord] = rZ;
    }
    else
    {
        sIntH[coord] = rH; sIntX[coord] = rX; sIntZ[coord] = rZ;
    }
}

// Runs all butterfly passes; shared sInt* must be preloaded by the caller.
void OceanRunFFT(uint coord)
{
    for (uint passIndex = 0u; passIndex < uint(GE_FFT_PASSES); ++passIndex)
    {
        barrier();
        const vec2 bf = imageLoad(uButterfly, ivec2(int(coord), int(passIndex))).xy;
        OceanButterflyPass(bf, coord, passIndex);
    }
    barrier();
}

// After GE_FFT_PASSES passes, the result sits in sInt* if PASSES is even, else
// in sScr* (GE_FFT_SIZE=256 -> PASSES=8 even, but keep it general).
vec2 OceanFFTResultH(uint coord) { return ((GE_FFT_PASSES % 2) == 0) ? sIntH[coord] : sScrH[coord]; }
vec2 OceanFFTResultX(uint coord) { return ((GE_FFT_PASSES % 2) == 0) ? sIntX[coord] : sScrX[coord]; }
vec2 OceanFFTResultZ(uint coord) { return ((GE_FFT_PASSES % 2) == 0) ? sIntZ[coord] : sScrZ[coord]; }

#endif // GE_OCEAN_FFT_BUTTERFLY_GLSL
