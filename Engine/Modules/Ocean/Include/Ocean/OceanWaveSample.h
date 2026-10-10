#pragma once

#include "Ocean/OceanTypes.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{

// Legacy CPU evaluation of the analytic wave array. The runtime ocean surface uses
// the FFT-backed OceanRenderFeature query path instead.
inline float32 SampleOceanHeight(const OceanParamsGPU& p, float32 x, float32 z)
{
    x += p.WaveOriginOffsetX;
    z += p.WaveOriginOffsetZ;
    float32 h = p.SeaLevel;
    const uint32 count = std::min<uint32>(p.GerstnerWaveCount, kMaxGerstnerWaves);
    for (uint32 i = 0; i < count; ++i)
    {
        const GerstnerWave& w = p.Waves[i];
        float32 dx = w.DirectionX;
        float32 dz = w.DirectionZ;
        const float32 dlen = std::sqrt(dx * dx + dz * dz);
        if (dlen < 1e-4f)
            continue;
        dx /= dlen;
        dz /= dlen;

        const float32 k = 6.2831853f / std::max(w.Wavelength, 1e-3f);
        const float32 c = std::sqrt(9.81f / k) * w.Speed;
        const float32 wf = k * c;
        const float32 phase = k * (dx * x + dz * z) + p.Time * wf;
        h += w.Amplitude * std::sin(phase);
    }
    return h;
}

inline void SampleOceanHorizontalDisplacement(const OceanParamsGPU& p, float32 x, float32 z,
                                              float32& outDx, float32& outDz)
{
    x += p.WaveOriginOffsetX;
    z += p.WaveOriginOffsetZ;
    outDx = 0.0f;
    outDz = 0.0f;
    const uint32 count = std::min<uint32>(p.GerstnerWaveCount, kMaxGerstnerWaves);
    for (uint32 i = 0; i < count; ++i)
    {
        const GerstnerWave& w = p.Waves[i];
        float32 dx = w.DirectionX;
        float32 dz = w.DirectionZ;
        const float32 dlen = std::sqrt(dx * dx + dz * dz);
        if (dlen < 1e-4f)
            continue;
        dx /= dlen;
        dz /= dlen;

        const float32 k = 6.2831853f / std::max(w.Wavelength, 1e-3f);
        const float32 c = std::sqrt(9.81f / k) * w.Speed;
        const float32 wf = k * c;
        const float32 phase = k * (dx * x + dz * z) + p.Time * wf;
        const float32 q = w.Steepness * p.ChoppyScale;
        const float32 d = q * w.Amplitude * std::cos(phase);
        outDx += d * dx;
        outDz += d * dz;
    }
}

// Surface normal via finite differences of the height field. Good enough for
// aligning floating bodies to the wave slope.
inline void SampleOceanHeightAndNormal(const OceanParamsGPU& p, float32 x, float32 z,
                                       float32& outHeight, float32& outNx, float32& outNy,
                                       float32& outNz, float32 epsilon = 0.5f)
{
    const float32 e = std::max(epsilon, 1e-3f);
    const float32 h = SampleOceanHeight(p, x, z);
    const float32 hL = SampleOceanHeight(p, x - e, z);
    const float32 hR = SampleOceanHeight(p, x + e, z);
    const float32 hD = SampleOceanHeight(p, x, z - e);
    const float32 hU = SampleOceanHeight(p, x, z + e);

    float32 nx = (hL - hR);
    float32 ny = 2.0f * e;
    float32 nz = (hD - hU);
    const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-5f)
    {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    outHeight = h;
    outNx = nx;
    outNy = ny;
    outNz = nz;
}

} // namespace GameEngine::Ocean
