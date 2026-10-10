#pragma once

#include "Ocean/OceanWaveSample.h"

namespace GameEngine::Ocean
{

enum class OceanQuerySource : uint32
{
    None = 0,
    // Legacy source value retained for callers that still use SampleOceanSurfaceAnalytic().
    AnalyticGerstner = 1,
    BakedHeightField = 2,
    BakedFFTCPU = 3,
    GPUQuery = 4,
    CombinedCascade = 5,
};

using OceanSurfaceQueryHandle = uint64;

struct OceanSurfaceSample
{
    bool Valid = false;
    OceanQuerySource Source = OceanQuerySource::None;

    float32 PositionWS[3] = {0.0f, 0.0f, 0.0f};
    float32 NormalWS[3] = {0.0f, 1.0f, 0.0f};
    float32 DisplacementWS[3] = {0.0f, 0.0f, 0.0f};
    // Surface point velocity returned by provider-backed queries. Legacy direct
    // samples leave this at zero when no temporal history is available.
    float32 VelocityWS[3] = {0.0f, 0.0f, 0.0f};

    float32 Height = 0.0f;
    float32 SeaLevel = 0.0f;
    float32 WaveWeight = 1.0f;
    float32 WaveChop = 1.0f;
    bool TextureWaveMaskContributes = false;
    uint32 TextureWaveMaskCount = 0u;
    float32 LocalFFTBlend = 0.0f;
    uint32 LocalFFTStream = 0u;
    float32 LocalWaveHeight = 0.0f;
    float32 LocalWaveSlopeX = 0.0f;
    float32 LocalWaveSlopeZ = 0.0f;
    bool HasWaterDepth = false;
    float32 WaterDepth = 0.0f;
    float32 ShallowWaterFactor = 0.0f;
    float32 ShorelineContactFactor = 0.0f;
    float32 ShorelineFoamFactor = 0.0f;
};

struct OceanSurfaceQueryPoint
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
};

struct OceanCurrentSample
{
    bool Valid = false;
    float32 FlowX = 0.0f;
    float32 FlowZ = 0.0f;
    uint32 RectSourceCount = 0;
    uint32 PolygonSourceCount = 0;

    // True when a sampled flow-map footprint overlaps the query point. FlowX/Z
    // include the decoded texture-vector sample when the CPU texture copy is loaded.
    bool TextureFlowMapContributes = false;
    uint32 TextureFlowMapCount = 0;
};

inline OceanSurfaceSample MakeOceanSurfaceSample(const OceanParamsGPU& params, float32 x, float32 z,
                                                 float32 height, float32 nx, float32 ny, float32 nz,
                                                 OceanQuerySource source)
{
    OceanSurfaceSample sample;
    sample.Valid = true;
    sample.Source = source;
    sample.PositionWS[0] = x;
    sample.PositionWS[1] = height;
    sample.PositionWS[2] = z;
    sample.NormalWS[0] = nx;
    sample.NormalWS[1] = ny;
    sample.NormalWS[2] = nz;
    sample.DisplacementWS[1] = height - params.SeaLevel;
    sample.Height = height;
    sample.SeaLevel = params.SeaLevel;
    return sample;
}

inline OceanSurfaceSample SampleOceanSurfaceAnalytic(const OceanParamsGPU& params, float32 x, float32 z,
                                                     float32 normalEpsilon = 0.5f)
{
    float32 height = params.SeaLevel;
    float32 nx = 0.0f;
    float32 ny = 1.0f;
    float32 nz = 0.0f;
    SampleOceanHeightAndNormal(params, x, z, height, nx, ny, nz, normalEpsilon);
    OceanSurfaceSample sample =
        MakeOceanSurfaceSample(params, x, z, height, nx, ny, nz,
                               OceanQuerySource::AnalyticGerstner);
    SampleOceanHorizontalDisplacement(params, x, z,
                                      sample.DisplacementWS[0], sample.DisplacementWS[2]);
    sample.PositionWS[0] = x + sample.DisplacementWS[0];
    sample.PositionWS[2] = z + sample.DisplacementWS[2];
    return sample;
}

inline void SampleOceanSurfaceAnalyticBatch(const OceanParamsGPU& params,
                                            const OceanSurfaceQueryPoint* points,
                                            uint32 count, OceanSurfaceSample* outSamples,
                                            float32 normalEpsilon = 0.5f)
{
    if (!points || !outSamples)
        return;
    for (uint32 i = 0; i < count; ++i)
        outSamples[i] = SampleOceanSurfaceAnalytic(params, points[i].X, points[i].Z, normalEpsilon);
}

} // namespace GameEngine::Ocean
