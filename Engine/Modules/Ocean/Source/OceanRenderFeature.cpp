#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanCollisionProvider.h"
#include "Ocean/OceanFootprintFrustum.h"
#include "Ocean/OceanForwardContributor.h"

#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector2.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/BufferHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace GameEngine::Ocean
{

using ::GameEngine::Rendering::BufferCreateFlags;
using ::GameEngine::Rendering::BufferDesc;
using ::GameEngine::Rendering::BufferMemoryUsage;
using ::GameEngine::Rendering::BufferUsage;
using ::GameEngine::Rendering::TextureCreateFlags;
using ::GameEngine::Rendering::TextureDesc;
using ::GameEngine::Rendering::TextureFormat;
using ::GameEngine::Rendering::TextureUsage;

namespace
{

float32 Lerp(float32 a, float32 b, float32 t);

float32 AuthoredInputWeight(const OceanInputDrawPacket& input, float32 worldX, float32 worldZ)
{
    const float32 dx = std::abs(worldX - input.CenterX);
    const float32 dz = std::abs(worldZ - input.CenterZ);
    if (input.ExtentX <= 0.0f || input.ExtentZ <= 0.0f ||
        dx > input.ExtentX || dz > input.ExtentZ)
    {
        return 0.0f;
    }
    if (input.Feather <= 1e-4f)
        return 1.0f;
    const float32 inside = std::min(input.ExtentX - dx, input.ExtentZ - dz);
    const float32 t = std::clamp(inside / input.Feather, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void ApplyTypedShapeInputs(const std::vector<OceanInputDrawPacket>& inputs,
                           OceanSurfaceSample& sample, const OceanParamsGPU& params,
                           float32 worldX, float32 worldZ)
{
    float32 slopeX = 0.0f;
    float32 slopeZ = 0.0f;
    for (const OceanInputDrawPacket& input : inputs)
    {
        if (input.Family != OceanInputFamily::AnimatedWaves &&
            input.Family != OceanInputFamily::Height)
        {
            continue;
        }
        const float32 weight = AuthoredInputWeight(input, worldX, worldZ);
        if (weight <= 0.0f)
            continue;

        if (input.Family == OceanInputFamily::Height)
        {
            const bool absolute = input.Value[1] > 0.5f;
            const float32 target = absolute ? input.Value[0] - params.SeaLevel : input.Value[0];
            const OceanInputBlendMode mode = absolute
                                                 ? OceanInputBlendMode::Replace
                                                 : input.Blend;
            sample.DisplacementWS[1] = OceanInputDrawRegistry::ApplyBlend(
                sample.DisplacementWS[1], target, weight, mode);
            continue;
        }

        const float32 amplitude = input.Value[0];
        const float32 wavelength = std::max(input.Value[1], 0.1f);
        float32 dirX = input.Value[2];
        float32 dirZ = input.Value[3];
        const float32 dirLength = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (dirLength <= 1e-5f)
            continue;
        dirX /= dirLength;
        dirZ /= dirLength;
        const float32 k = 6.28318530718f / wavelength;
        const float32 phaseX = worldX + params.WaveOriginOffsetX;
        const float32 phaseZ = worldZ + params.WaveOriginOffsetZ;
        const float32 phase = k * (dirX * phaseX + dirZ * phaseZ) +
                              std::sqrt(9.81f * k) * params.Time;
        const float32 sine = std::sin(phase);
        const float32 cosine = std::cos(phase);
        const float32 vertical = amplitude * sine;
        const float32 horizontal = amplitude * 0.65f * cosine;
        sample.DisplacementWS[0] = OceanInputDrawRegistry::ApplyBlend(
            sample.DisplacementWS[0], dirX * horizontal, weight, input.Blend);
        sample.DisplacementWS[1] = OceanInputDrawRegistry::ApplyBlend(
            sample.DisplacementWS[1], vertical, weight, input.Blend);
        sample.DisplacementWS[2] = OceanInputDrawRegistry::ApplyBlend(
            sample.DisplacementWS[2], dirZ * horizontal, weight, input.Blend);
        slopeX += amplitude * k * cosine * dirX * weight;
        slopeZ += amplitude * k * cosine * dirZ * weight;
    }

    sample.DisplacementWS[0] = std::clamp(sample.DisplacementWS[0],
                                          -params.MaxHorizontalDisplacement,
                                          params.MaxHorizontalDisplacement);
    sample.DisplacementWS[2] = std::clamp(sample.DisplacementWS[2],
                                          -params.MaxHorizontalDisplacement,
                                          params.MaxHorizontalDisplacement);
    sample.DisplacementWS[1] = std::clamp(sample.DisplacementWS[1],
                                          -params.MaxVerticalDisplacement,
                                          params.MaxVerticalDisplacement);
    sample.PositionWS[0] = worldX + sample.DisplacementWS[0];
    sample.PositionWS[1] = params.SeaLevel + sample.DisplacementWS[1];
    sample.PositionWS[2] = worldZ + sample.DisplacementWS[2];
    sample.Height = sample.PositionWS[1];
    if (std::abs(slopeX) > 1e-6f || std::abs(slopeZ) > 1e-6f)
    {
        float32 nx = sample.NormalWS[0] - slopeX;
        float32 ny = sample.NormalWS[1];
        float32 nz = sample.NormalWS[2] - slopeZ;
        const float32 length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 1e-6f)
        {
            sample.NormalWS[0] = nx / length;
            sample.NormalWS[1] = ny / length;
            sample.NormalWS[2] = nz / length;
        }
    }
}

void ApplyBakedHeightFieldSample(const OceanHeightField& heightField, OceanSurfaceSample& sample,
                                 [[maybe_unused]] const OceanParamsGPU& params, float32 worldX, float32 worldZ)
{
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float h = 0.0f;
    if (!heightField.SampleDisplacement(worldX, worldZ, dx, dy, dz, h))
        return;

    sample.Source = OceanQuerySource::BakedHeightField;
    sample.Height = h;
    sample.PositionWS[0] = worldX;
    sample.PositionWS[1] = h;
    sample.PositionWS[2] = worldZ;
    sample.DisplacementWS[0] = dx;
    sample.DisplacementWS[1] = dy;
    sample.DisplacementWS[2] = dz;

    constexpr float32 e = 0.5f;
    float hL = 0.0f;
    float hR = 0.0f;
    float hD = 0.0f;
    float hU = 0.0f;
    if (heightField.Sample(worldX - e, worldZ, hL) &&
        heightField.Sample(worldX + e, worldZ, hR) &&
        heightField.Sample(worldX, worldZ - e, hD) &&
        heightField.Sample(worldX, worldZ + e, hU))
    {
        float32 nx = hL - hR;
        float32 ny = 2.0f * e;
        float32 nz = hD - hU;
        const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-5f)
        {
            sample.NormalWS[0] = nx / len;
            sample.NormalWS[1] = ny / len;
            sample.NormalWS[2] = nz / len;
        }
    }
}

void BlendBakedHeightFieldSample(const OceanHeightField& heightField, OceanSurfaceSample& sample,
                                 float32 worldX, float32 worldZ, float32 blend)
{
    blend = std::clamp(blend, 0.0f, 1.0f);
    if (blend <= 1e-5f)
        return;

    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float h = 0.0f;
    if (!heightField.SampleDisplacement(worldX, worldZ, dx, dy, dz, h))
        return;

    sample.Source = OceanQuerySource::BakedHeightField;
    sample.Height = Lerp(sample.Height, h, blend);
    sample.PositionWS[0] = worldX;
    sample.PositionWS[1] = sample.Height;
    sample.PositionWS[2] = worldZ;
    sample.DisplacementWS[0] = Lerp(sample.DisplacementWS[0], dx, blend);
    sample.DisplacementWS[1] = Lerp(sample.DisplacementWS[1], dy, blend);
    sample.DisplacementWS[2] = Lerp(sample.DisplacementWS[2], dz, blend);

    constexpr float32 e = 0.5f;
    float hL = 0.0f;
    float hR = 0.0f;
    float hD = 0.0f;
    float hU = 0.0f;
    if (!(heightField.Sample(worldX - e, worldZ, hL) &&
          heightField.Sample(worldX + e, worldZ, hR) &&
          heightField.Sample(worldX, worldZ - e, hD) &&
          heightField.Sample(worldX, worldZ + e, hU)))
    {
        return;
    }

    float32 slopeX = 0.0f;
    float32 slopeZ = 0.0f;
    if (std::abs(sample.NormalWS[1]) > 1e-5f)
    {
        slopeX = -sample.NormalWS[0] / sample.NormalWS[1];
        slopeZ = -sample.NormalWS[2] / sample.NormalWS[1];
    }
    const float32 localSlopeX = (hR - hL) / (2.0f * e);
    const float32 localSlopeZ = (hU - hD) / (2.0f * e);
    slopeX = Lerp(slopeX, localSlopeX, blend);
    slopeZ = Lerp(slopeZ, localSlopeZ, blend);

    const float32 nx = -slopeX;
    const float32 ny = 1.0f;
    const float32 nz = -slopeZ;
    const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-5f)
    {
        sample.NormalWS[0] = nx / len;
        sample.NormalWS[1] = ny / len;
        sample.NormalWS[2] = nz / len;
    }
}

OceanSurfaceSample MakeInvalidSurfaceSample(const OceanParamsGPU& params, float32 worldX, float32 worldZ)
{
    OceanSurfaceSample sample =
        MakeOceanSurfaceSample(params, worldX, worldZ, params.SeaLevel, 0.0f, 1.0f, 0.0f,
                               OceanQuerySource::None);
    sample.Valid = false;
    return sample;
}

float32 SmoothStep(float32 edge0, float32 edge1, float32 x)
{
    const float32 denom = edge1 - edge0;
    if (std::abs(denom) < 1e-6f)
        return 0.0f;
    const float32 t = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float32 Lerp(float32 a, float32 b, float32 t)
{
    return a + (b - a) * t;
}

void ApplyWaterDepthValue(OceanSurfaceSample& sample, const OceanParamsGPU& params, float32 depth)
{
    sample.HasWaterDepth = true;
    sample.WaterDepth = depth;

    const float32 shallowBase =
        1.0f - std::clamp(depth / std::max(params.SubSurfaceDepthMax, 1e-3f), 0.0f, 1.0f);
    sample.ShallowWaterFactor =
        std::pow(shallowBase, std::max(params.SubSurfaceDepthPower, 0.0f));

    const float32 shoreMaxDepth = std::max(params.ShorelineFoamMaxDepth, 0.0f);
    const float32 contact =
        (shoreMaxDepth > 1e-5f) ? SmoothStep(shoreMaxDepth, 0.0f, depth) : 0.0f;
    sample.ShorelineContactFactor = contact;
    sample.ShorelineFoamFactor =
        contact * std::max(params.ShorelineFoamStrength, 0.0f) * std::max(params.FoamAmount, 0.0f);
}

void ApplyWaterDepthSample(const OceanSeabedDepth& seabedDepth, OceanSurfaceSample& sample,
                           const OceanParamsGPU& params, float32 worldX, float32 worldZ)
{
    float32 depth = 0.0f;
    if (seabedDepth.SampleDepth(worldX, worldZ, depth))
        ApplyWaterDepthValue(sample, params, depth);
}

void ApplyRasterDepthReadbackSample(const OceanRasterDepthCapture& rasterDepth,
                                    OceanSurfaceSample& sample,
                                    const OceanParamsGPU& params,
                                    float32 worldX, float32 worldZ)
{
    float32 depth = 0.0f;
    if (!rasterDepth.SampleDepth(worldX, worldZ, depth))
        return;
    if (sample.HasWaterDepth)
        depth = std::min(depth, sample.WaterDepth);
    ApplyWaterDepthValue(sample, params, depth);
}

void ApplyWaveWeight(OceanSurfaceSample& sample, const OceanParamsGPU& params,
                     float32 weight, float32 chop)
{
    weight = std::max(weight, 0.0f);
    chop = std::max(chop, 0.0f);
    if (std::abs(weight - 1.0f) < 1e-4f && std::abs(chop - 1.0f) < 1e-4f)
        return;
    const float32 baseX = sample.PositionWS[0] - sample.DisplacementWS[0];
    const float32 baseZ = sample.PositionWS[2] - sample.DisplacementWS[2];
    const float32 relativeHeight = sample.Height - params.SeaLevel;
    sample.Height = params.SeaLevel + relativeHeight * weight;
    sample.DisplacementWS[0] *= weight * chop;
    sample.PositionWS[1] = sample.Height;
    sample.DisplacementWS[1] *= weight;
    sample.DisplacementWS[2] *= weight * chop;
    sample.PositionWS[0] = baseX + sample.DisplacementWS[0];
    sample.PositionWS[2] = baseZ + sample.DisplacementWS[2];

    if (std::abs(sample.NormalWS[1]) > 1e-5f)
    {
        const float32 slopeX = (-sample.NormalWS[0] / sample.NormalWS[1]) * weight;
        const float32 slopeZ = (-sample.NormalWS[2] / sample.NormalWS[1]) * weight;
        const float32 nx = -slopeX;
        const float32 ny = 1.0f;
        const float32 nz = -slopeZ;
        const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-5f)
        {
            sample.NormalWS[0] = nx / len;
            sample.NormalWS[1] = ny / len;
            sample.NormalWS[2] = nz / len;
        }
    }
}

float32 EvaluateLocalWaterBodyWave(float32 worldX, float32 worldZ, float32 time,
                                   float32 amplitude, float32 wavelength,
                                   float32 dirX, float32 dirZ,
                                   float32& outSlopeX, float32& outSlopeZ)
{
    outSlopeX = 0.0f;
    outSlopeZ = 0.0f;
    amplitude = std::max(amplitude, 0.0f);
    wavelength = std::max(wavelength, 1e-3f);
    const float32 dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (amplitude <= 1e-5f || dirLen <= 1e-4f)
        return 0.0f;
    dirX /= dirLen;
    dirZ /= dirLen;
    constexpr float32 kTwoPi = 6.28318530718f;
    constexpr float32 kGravity = 9.81f;
    const float32 k = kTwoPi / wavelength;
    const float32 c = std::sqrt(kGravity / k);
    const float32 phase = k * (dirX * worldX + dirZ * worldZ) + time * k * c;
    const float32 slope = amplitude * std::cos(phase) * k;
    outSlopeX = slope * dirX;
    outSlopeZ = slope * dirZ;
    return amplitude * std::sin(phase);
}

float32 EvaluateLocalWaterBodyWavePacket(
    float32 worldX, float32 worldZ, float32 time, uint32 waveCount, float32 amplitude,
    float32 wavelength, float32 dirX, float32 dirZ,
    const float32 extraWaves[kMaxOceanWaveMaskLocalWaves - 1u][4],
    float32& outSlopeX, float32& outSlopeZ)
{
    outSlopeX = 0.0f;
    outSlopeZ = 0.0f;
    waveCount = std::min(waveCount, kMaxOceanWaveMaskLocalWaves);
    if (waveCount == 0u)
        return 0.0f;

    float32 slopeX = 0.0f;
    float32 slopeZ = 0.0f;
    float32 height = EvaluateLocalWaterBodyWave(
        worldX, worldZ, time, amplitude, wavelength, dirX, dirZ, slopeX, slopeZ);
    outSlopeX += slopeX;
    outSlopeZ += slopeZ;
    for (uint32 i = 1u; i < waveCount; ++i)
    {
        const float32* w = extraWaves[i - 1u];
        height += EvaluateLocalWaterBodyWave(
            worldX, worldZ, time, w[0], w[1], w[2], w[3], slopeX, slopeZ);
        outSlopeX += slopeX;
        outSlopeZ += slopeZ;
    }
    return height;
}

void ApplyLocalWaveSample(OceanSurfaceSample& sample, float32 localHeight,
                          float32 localSlopeX, float32 localSlopeZ)
{
    if (std::abs(localHeight) >= 1e-5f)
    {
        sample.Height += localHeight;
        sample.PositionWS[1] = sample.Height;
        sample.DisplacementWS[1] += localHeight;
    }

    if (std::abs(localSlopeX) < 1e-5f && std::abs(localSlopeZ) < 1e-5f)
        return;

    float32 slopeX = localSlopeX;
    float32 slopeZ = localSlopeZ;
    if (std::abs(sample.NormalWS[1]) > 1e-5f)
    {
        slopeX += -sample.NormalWS[0] / sample.NormalWS[1];
        slopeZ += -sample.NormalWS[2] / sample.NormalWS[1];
    }

    const float32 nx = -slopeX;
    const float32 ny = 1.0f;
    const float32 nz = -slopeZ;
    const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-5f)
    {
        sample.NormalWS[0] = nx / len;
        sample.NormalWS[1] = ny / len;
        sample.NormalWS[2] = nz / len;
    }
}

void ApplyDynamicWaveReadbackSample(const OceanDynWavesReadback& readback,
                                    OceanSurfaceSample& sample, float32 worldX, float32 worldZ,
                                    float32 scale, float32 horizontalDisplacement,
                                    float32 displacementClamp)
{
    if (std::abs(scale) < 1e-5f)
        return;

    float32 h = 0.0f;
    if (!readback.Sample(worldX, worldZ, h))
        return;

    const float32 dynamicHeight = std::clamp(h * scale, -std::max(displacementClamp, 0.01f),
                                             std::max(displacementClamp, 0.01f));
    if (std::abs(dynamicHeight) < 1e-5f)
        return;

    sample.Height += dynamicHeight;
    sample.PositionWS[1] = sample.Height;
    sample.DisplacementWS[1] += dynamicHeight;

    constexpr float32 e = 0.5f;
    float32 hL = 0.0f;
    float32 hR = 0.0f;
    float32 hD = 0.0f;
    float32 hU = 0.0f;
    if (!(readback.Sample(worldX - e, worldZ, hL) &&
          readback.Sample(worldX + e, worldZ, hR) &&
          readback.Sample(worldX, worldZ - e, hD) &&
          readback.Sample(worldX, worldZ + e, hU)))
    {
        return;
    }

    float32 slopeX = 0.0f;
    float32 slopeZ = 0.0f;
    if (std::abs(sample.NormalWS[1]) > 1e-5f)
    {
        slopeX = -sample.NormalWS[0] / sample.NormalWS[1];
        slopeZ = -sample.NormalWS[2] / sample.NormalWS[1];
    }
    slopeX += ((hR - hL) / (2.0f * e)) * scale;
    slopeZ += ((hU - hD) / (2.0f * e)) * scale;
    const float32 dynamicSlopeX = ((hR - hL) / (2.0f * e)) * scale;
    const float32 dynamicSlopeZ = ((hU - hD) / (2.0f * e)) * scale;
    const float32 horizontalX = -dynamicSlopeX * std::max(horizontalDisplacement, 0.0f);
    const float32 horizontalZ = -dynamicSlopeZ * std::max(horizontalDisplacement, 0.0f);
    sample.PositionWS[0] += horizontalX;
    sample.PositionWS[2] += horizontalZ;
    sample.DisplacementWS[0] += horizontalX;
    sample.DisplacementWS[2] += horizontalZ;

    float32 nx = -slopeX;
    float32 ny = 1.0f;
    float32 nz = -slopeZ;
    const float32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-5f)
    {
        sample.NormalWS[0] = nx / len;
        sample.NormalWS[1] = ny / len;
        sample.NormalWS[2] = nz / len;
    }
}

using PolygonPointsXZ = std::array<Mathematics::Vector2, kMaxOceanClipPolygonPoints>;

// Gathers an ocean polygon's parallel X/Z arrays into XZ points (x in .x, z in .y) for the
// Mathematics polygon tests; at most kMaxOceanClipPolygonPoints are read.
std::span<const Mathematics::Vector2> GatherPolygonPointsXZ(uint32 pointCount, const float* xs, const float* zs,
                                                            PolygonPointsXZ& storage)
{
    const uint32 count = std::min(pointCount, kMaxOceanClipPolygonPoints);
    for (uint32 i = 0; i < count; ++i)
        storage[i] = Mathematics::Vector2(xs[i], zs[i]);
    return std::span<const Mathematics::Vector2>(storage.data(), count);
}

float32 SegmentDistanceXZ(float32 x, float32 z, float32 ax, float32 az, float32 bx, float32 bz)
{
    const float32 abx = bx - ax;
    const float32 abz = bz - az;
    const float32 denom = std::max(abx * abx + abz * abz, 1e-12f);
    const float32 t = std::clamp(((x - ax) * abx + (z - az) * abz) / denom, 0.0f, 1.0f);
    const float32 px = ax + abx * t;
    const float32 pz = az + abz * t;
    const float32 dx = x - px;
    const float32 dz = z - pz;
    return std::sqrt(dx * dx + dz * dz);
}

float32 PolygonInteriorWeightXZ(float32 x, float32 z, uint32 pointCount, const float* xs,
                                const float* zs, float32 feather)
{
    PolygonPointsXZ storage;
    const std::span<const Mathematics::Vector2> points = GatherPolygonPointsXZ(pointCount, xs, zs, storage);
    if (!Mathematics::PointInPolygon(Mathematics::Vector2(x, z), points))
        return 0.0f;

    feather = std::max(feather, 0.0f);
    if (feather <= 1e-3f)
        return 1.0f;
    float32 minEdgeDist = 1e20f;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        minEdgeDist = std::min(minEdgeDist, SegmentDistanceXZ(x, z, points[i].x, points[i].y, points[j].x, points[j].y));
    return SmoothStep(0.0f, feather, minEdgeDist);
}

float32 RectInteriorWeight(float32 x, float32 z,
                           const OceanRenderFeature::WaterBodyWaveBox& box)
{
    const float32 halfX = std::max(box.HalfX, 0.0f);
    const float32 halfZ = std::max(box.HalfZ, 0.0f);
    const float32 dx = std::abs(x - box.CenterX);
    const float32 dz = std::abs(z - box.CenterZ);
    if (dx > halfX || dz > halfZ)
        return 0.0f;

    const float32 feather = std::max(box.Feather, 0.0f);
    if (feather > 1e-3f)
    {
        const float32 insideDist = std::min(halfX - dx, halfZ - dz);
        return SmoothStep(0.0f, feather, insideDist);
    }

    const float32 edgeX = 1.0f - SmoothStep(0.7f, 1.0f, dx / std::max(halfX, 1e-3f));
    const float32 edgeZ = 1.0f - SmoothStep(0.7f, 1.0f, dz / std::max(halfZ, 1e-3f));
    return std::min(edgeX, edgeZ);
}

float32 RectInteriorWeight(float32 x, float32 z,
                           const OceanRenderFeature::WaterBodyWaveTextureBox& box)
{
    const float32 halfX = std::max(box.HalfX, 0.0f);
    const float32 halfZ = std::max(box.HalfZ, 0.0f);
    const float32 dx = std::abs(x - box.CenterX);
    const float32 dz = std::abs(z - box.CenterZ);
    if (dx > halfX || dz > halfZ)
        return 0.0f;

    const float32 feather = std::max(box.Feather, 0.0f);
    if (feather <= 1e-3f)
        return 1.0f;
    return SmoothStep(0.0f, feather, std::min(halfX - dx, halfZ - dz));
}

} // anonymous namespace

OceanRenderFeature::OceanRenderFeature()
{
    m_OriginShiftNotifier.AddListener(this);
}

OceanRenderFeature::~OceanRenderFeature()
{
    m_Caustics.Destroy(m_Device);
}

bool OceanRenderFeature::Initialize(::GameEngine::Rendering::IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;
    // Sticky: the caller retries a false Initialize every frame; remember a
    // decline so the retry costs a branch, not a rebuild.
    if (m_InitDeclined)
        return false;
    m_Device = device;
    CreateGridMesh(device, kDefaultOceanGeometryGridSize);

    // FFT wave simulation. A failure leaves rendering and collision on the
    // deterministic Gerstner representation built by extraction.
    m_FFTReady = m_FFT.Initialize(device);
    if (!m_FFTReady)
        Logger::Log::Warning("OceanRenderFeature: FFT init failed; using Gerstner fallback");

    // Foam simulation depends on the FFT displacement (the Jacobian source). If
    // either is unavailable the surface degrades to its per-pixel Jacobian foam.
    if (m_FFTReady)
    {
        m_FoamReady = m_FoamSim.Initialize(device);
        if (!m_FoamReady)
            Logger::Log::Warning("OceanRenderFeature: foam sim init failed; using per-pixel foam");

        // CPU-readable height field for buoyancy (reads the FFT surface). Depends
        // on the FFT displacement; queries use Gerstner until readback is ready.
        m_HeightFieldReady = m_HeightField.Initialize(device);
        if (!m_HeightFieldReady)
            Logger::Log::Warning("OceanRenderFeature: height field init failed; CPU queries use Gerstner");

        m_GPUQueryReady = m_GPUQuery.Initialize(device);
        if (!m_GPUQueryReady)
            Logger::Log::Warning(
                "OceanRenderFeature: arbitrary GPU collision queries unavailable; using fallback providers");

        // Displacement combine cascade. Storage follows the camera, while the
        // compute evaluates absolute world coordinates so wave phase stays stable.
        m_CombineReady = m_CombineSim.Initialize(device);
        if (!m_CombineReady)
            Logger::Log::Warning("OceanRenderFeature: combine init failed; using direct 16-cascade sum");
    }

    // Sea-floor depth cascade — independent of the FFT (it only needs the device).
    // Drives the shallow-water colour + shoreline foam; degrades to deep water on
    // failure (the surface's SeabedDepthAvailable flag stays 0).
    m_SeabedDepthReady = m_SeabedDepth.Initialize(device);
    if (!m_SeabedDepthReady)
        Logger::Log::Warning("OceanRenderFeature: seabed depth init failed; shallows/shoreline off");

    // Flow cascade — independent of the FFT. Advects the foam + scrolls the
    // detail-normal UVs; degrades to "no flow" on failure (FlowAvailable stays 0).
    m_FlowReady = m_Flow.Initialize(device);
    if (!m_FlowReady)
        Logger::Log::Warning("OceanRenderFeature: flow init failed; foam advection off");

    // Dynamic (interactive) wave sim — independent of the FFT. Spreading ripples
    // from impulses; degrades to "spectrum waves only" on failure (the surface's
    // DynamicWavesAvailable flag stays 0).
    m_DynWavesReady = m_DynWaves.Initialize(device);
    if (!m_DynWavesReady)
        Logger::Log::Warning("OceanRenderFeature: dynamic waves init failed; interactive ripples off");
    if (m_DynWavesReady)
    {
        m_DynWavesReadbackReady = m_DynWavesReadback.Initialize(device);
        if (!m_DynWavesReadbackReady)
            Logger::Log::Warning(
                "OceanRenderFeature: dynamic-wave readback init failed; queries omit ripples");
    }

    // Local wave override mask — independent of FFT. Multiplies the spectrum and
    // dynamic waves by water-body-authored wave/chop factors.
    m_WaveMaskReady = m_WaveMask.Initialize(device);
    if (!m_WaveMaskReady)
        Logger::Log::Warning("OceanRenderFeature: wave mask init failed; per-body waves off");

    // Procedural underwater caustics — a one-time CPU-generated tileable texture,
    // independent of every sim. Degrades to "no caustics" on failure (the
    // surface's CausticsAvailable flag stays 0).
    m_CausticsReady = m_Caustics.Initialize(device);
    if (!m_CausticsReady)
        Logger::Log::Warning("OceanRenderFeature: caustics init failed; underwater caustics off");

    // Clip cascade — independent of the FFT. Cuts holes in the surface (harbors,
    // hulls); degrades to "no clip" on failure (the surface's ClipAvailable flag
    // stays 0, so the surface stays solid).
    m_ClipReady = m_Clip.Initialize(device);
    if (!m_ClipReady)
        Logger::Log::Warning("OceanRenderFeature: clip init failed; surface clipping off");

    // Albedo cascade — independent of the FFT. Paints decals onto the surface
    // albedo; degrades to "no paint" on failure (the surface's AlbedoAvailable flag
    // stays 0).
    m_AlbedoReady = m_Albedo.Initialize(device);
    if (!m_AlbedoReady)
        Logger::Log::Warning("OceanRenderFeature: albedo init failed; surface albedo off");

    // Shared Linear/Repeat sampler for the user foam + caustics texture overrides
    // (both tile). Created once; the override textures are resolved per frame by the
    // extraction system and bound through this sampler by the contributor.
    m_UserTextureSampler = device->CreateSampler(
        Rendering::SamplerDesc::MaterialLinearRepeat("Ocean_UserTexture_Sampler"));

    m_Initialized = true;
    return true;
}

void OceanRenderFeature::SetLocalFFTParams(const OceanFFTParamsGPU& params, bool spectrumDirty,
                                           bool enabled)
{
    SetLocalFFTParams(0u, params, spectrumDirty, enabled);
}

void OceanRenderFeature::SetLocalFFTParams(uint32 streamIndex, const OceanFFTParamsGPU& params,
                                           bool spectrumDirty, bool enabled)
{
    if (streamIndex >= kMaxOceanLocalFFTStreams)
        return;

    m_LatestDepthInputs.LocalFFT[streamIndex] = params;
    m_LatestDepthInputs.LocalFFTEnabled[streamIndex] = enabled ? 1u : 0u;
    m_LocalFFTEnabled[streamIndex].store(false, std::memory_order_release);
    if (!enabled || !m_Device || !m_FFTReady)
        return;

    if (!m_LocalFFTReady[streamIndex])
    {
        m_LocalFFTReady[streamIndex] = m_LocalFFT[streamIndex].Initialize(m_Device);
        if (!m_LocalFFTReady[streamIndex])
        {
            Logger::Log::Warning("OceanRenderFeature: local FFT init failed; local bodies use wave packets");
            return;
        }
    }

    if (!m_LocalHeightFieldReady[streamIndex] &&
        !m_LocalHeightFieldInitAttempted[streamIndex])
    {
        m_LocalHeightFieldInitAttempted[streamIndex] = true;
        m_LocalHeightFieldReady[streamIndex] = m_LocalHeightField[streamIndex].Initialize(m_Device);
        if (!m_LocalHeightFieldReady[streamIndex])
            Logger::Log::Warning(
                "OceanRenderFeature: local height-field init failed; local FFT queries use global readback");
    }

    m_LocalFFT[streamIndex].SetParams(params, spectrumDirty);
    m_LocalFFTEnabled[streamIndex].store(true, std::memory_order_release);
}

OceanCascadeLayoutGPU OceanRenderFeature::ClampCascadeLayout(
    const OceanCascadeLayoutGPU& layout) const
{
    // Clamp the LOD count DOWN to the authored limit (the renderer's LodCount /
    // the quality tier), so the surface samples fewer snapped cascade layers. Only
    // shrinks — never expands past what the sim allocated (which would read
    // uninitialized layers). 0 = no limit.
    OceanCascadeLayoutGPU clamped = layout;
    const uint32 limit = m_LodCountLimit.load(std::memory_order_acquire);
    if (limit != 0 && clamped.LodCount > limit)
        clamped.LodCount = limit;
    return clamped;
}

OceanSampledCascadeLayoutsGPU OceanRenderFeature::SampledCascadeLayouts(
    const SampledCascadeAvailability& available) const
{
    const OceanCascadeLayoutGPU* sources[static_cast<uint32>(OceanSampledCascade::Count)] = {};
    sources[static_cast<uint32>(OceanSampledCascade::Foam)] = &m_FoamSim.GetFoamLayout();
    sources[static_cast<uint32>(OceanSampledCascade::SeabedDepth)] = &m_SeabedDepth.GetLayout();
    sources[static_cast<uint32>(OceanSampledCascade::Flow)] = &m_Flow.GetLayout();
    sources[static_cast<uint32>(OceanSampledCascade::DynWaves)] = &m_DynWaves.GetLayout();
    sources[static_cast<uint32>(OceanSampledCascade::WaveMask)] = &m_WaveMask.GetLayout();
    sources[static_cast<uint32>(OceanSampledCascade::Clip)] = &m_Clip.GetLayout();
    sources[static_cast<uint32>(OceanSampledCascade::Albedo)] = &m_Albedo.GetLayout();
    // The clip keeps every layer: its coarsest one is anchored over all authored
    // water, so trimming it would cut the water on a camera-relative line again.
    constexpr uint32 kClip = static_cast<uint32>(OceanSampledCascade::Clip);
    OceanSampledCascadeLayoutsGPU layouts{};
    for (uint32 index = 0; index < static_cast<uint32>(OceanSampledCascade::Count); ++index)
    {
        if (available.Available[index])
            layouts.Layouts[index] = index == kClip ? *sources[index] : ClampCascadeLayout(*sources[index]);
    }
    return layouts;
}

void OceanRenderFeature::CreateGridMesh(::GameEngine::Rendering::IDevice* device, uint32 gridSize)
{
    m_GridSize = gridSize;

    // Concentric patch topology adapted from Wave Harmonic's Crest OceanBuilder
    // (MIT, Copyright (c) 2019 Wave Harmonic and contributors; see Ocean/NOTICE).
    // The implementation is packed into one indexed mesh/draw for this engine.
    // LOD 0 is a dense 4x4 tile square; every coarser LOD is a 12-tile ring.
    // Fat/slim patch borders weld adjacent resolutions without cracks.
    enum class PatchType : uint8
    {
        Interior,
        Fat,
        FatX,
        FatXSlimZ,
        FatXOuter,
        FatXZ,
        FatXZOuter,
        SlimX,
        SlimXZ,
        SlimXFatZ,
    };

    struct PatchPlacement
    {
        float32 X = 0.0f;
        float32 Z = 0.0f;
        PatchType Type = PatchType::Interior;
    };

    constexpr uint32 kGeometryLodCount = 8u;
    constexpr uint32 kTileResolutionDivisor = 10u;
    constexpr uint32 kMinTileResolution = 16u;
    constexpr float32 kHorizonSkirtMultiplier = 4.0f;

    // The old authoring value described one monolithic grid. Preserve its budget
    // meaning by mapping it to a per-tile density: default 512 -> 50 cells/tile,
    // close to Crest's proven default of 48 while keeping total vertices near the
    // previous single-grid count. Keep it even so 2x/4x snapping stays exact.
    uint32 tileResolution = std::max(gridSize / kTileResolutionDivisor,
                                     kMinTileResolution);
    tileResolution &= ~1u;

    std::vector<float32> vertices; // float4: local XZ, LOD index, skirt footprint scale
    std::vector<uint32> indices;
    const size_t estimatedPatches = 16u + 12u * (kGeometryLodCount - 1u);
    vertices.reserve(estimatedPatches * static_cast<size_t>(tileResolution + 2u) *
                     static_cast<size_t>(tileResolution + 2u) * 4u);
    indices.reserve(estimatedPatches * static_cast<size_t>(tileResolution + 1u) *
                    static_cast<size_t>(tileResolution + 1u) * 6u);

    const auto isSidePatch = [](PatchType type)
    {
        return type == PatchType::FatX || type == PatchType::FatXOuter ||
               type == PatchType::SlimX || type == PatchType::SlimXFatZ;
    };
    const auto isCornerPatch = [](PatchType type)
    {
        return type == PatchType::FatXZ || type == PatchType::SlimXZ ||
               type == PatchType::FatXSlimZ || type == PatchType::FatXZOuter;
    };

    // Returns the rotated point in the tile's local ground plane: .x is X, .y is Z.
    const auto rotateOutward = [&](float32 x, float32 z, const PatchPlacement& placement)
    {
        // Patch variants put their skirt on local +X (and, for corners, +Z).
        // Rotate those directions toward this tile's side/corner of the ring.
        int quarterTurns = 0;
        if (isSidePatch(placement.Type))
        {
            if (std::abs(placement.Z) >= std::abs(placement.X))
                quarterTurns = placement.Z > 0.0f ? 3 : 1;
            else if (placement.X < 0.0f)
                quarterTurns = 2;
        }
        else if (isCornerPatch(placement.Type))
        {
            if (placement.X < 0.0f && placement.Z > 0.0f)
                quarterTurns = 3;
            else if (placement.X < 0.0f && placement.Z < 0.0f)
                quarterTurns = 2;
            else if (placement.X > 0.0f && placement.Z < 0.0f)
                quarterTurns = 1;
        }

        switch (quarterTurns)
        {
        case 1: return Mathematics::Vector2(z, -x);
        case 2: return Mathematics::Vector2(-x, -z);
        case 3: return Mathematics::Vector2(-z, x);
        default: return Mathematics::Vector2(x, z);
        }
    };

    const auto appendPatch = [&](const PatchPlacement& placement, uint32 lodIndex)
    {
        int32 skirtXMinus = 0;
        int32 skirtXPlus = 0;
        int32 skirtZMinus = 0;
        int32 skirtZPlus = 0;
        switch (placement.Type)
        {
        case PatchType::Fat:
            skirtXMinus = skirtXPlus = skirtZMinus = skirtZPlus = 1;
            break;
        case PatchType::FatX:
        case PatchType::FatXOuter:
            skirtXPlus = 1;
            break;
        case PatchType::FatXZ:
        case PatchType::FatXZOuter:
            skirtXPlus = skirtZPlus = 1;
            break;
        case PatchType::FatXSlimZ:
            skirtXPlus = 1;
            skirtZPlus = -1;
            break;
        case PatchType::SlimX:
            skirtXPlus = -1;
            break;
        case PatchType::SlimXZ:
            skirtXPlus = skirtZPlus = -1;
            break;
        case PatchType::SlimXFatZ:
            skirtXPlus = -1;
            skirtZPlus = 1;
            break;
        default:
            break;
        }

        const uint32 vertsX = static_cast<uint32>(
            static_cast<int32>(tileResolution + 1u) + skirtXMinus + skirtXPlus);
        const uint32 vertsZ = static_cast<uint32>(
            static_cast<int32>(tileResolution + 1u) + skirtZMinus + skirtZPlus);
        const float32 cell = 1.0f / static_cast<float32>(tileResolution);
        const float32 startX = -0.5f - static_cast<float32>(skirtXMinus) * cell;
        const float32 endX = 0.5f + static_cast<float32>(skirtXPlus) * cell;
        const float32 startZ = -0.5f - static_cast<float32>(skirtZMinus) * cell;
        const float32 endZ = 0.5f + static_cast<float32>(skirtZPlus) * cell;
        const float32 lodScale = static_cast<float32>(1u << lodIndex);
        const uint32 vertexBase = static_cast<uint32>(vertices.size() / 4u);

        for (uint32 z = 0; z < vertsZ; ++z)
        {
            float32 localZ = Lerp(startZ, endZ,
                                  static_cast<float32>(z) /
                                      static_cast<float32>(vertsZ - 1u));
            const bool horizonZ = placement.Type == PatchType::FatXZOuter &&
                                  z == vertsZ - 1u;
            if (horizonZ)
                localZ *= kHorizonSkirtMultiplier;

            for (uint32 x = 0; x < vertsX; ++x)
            {
                float32 localX = Lerp(startX, endX,
                                      static_cast<float32>(x) /
                                          static_cast<float32>(vertsX - 1u));
                const bool horizonX =
                    x == vertsX - 1u &&
                    (placement.Type == PatchType::FatXOuter ||
                     placement.Type == PatchType::FatXZOuter);
                if (horizonX)
                    localX *= kHorizonSkirtMultiplier;

                const Mathematics::Vector2 rotated = rotateOutward(localX, localZ, placement);
                vertices.push_back((placement.X + rotated.x) * lodScale);
                vertices.push_back((placement.Z + rotated.y) * lodScale);
                vertices.push_back(static_cast<float32>(lodIndex));
                vertices.push_back(horizonX || horizonZ ? kHorizonSkirtMultiplier : 1.0f);
            }
        }

        for (uint32 z = 0; z + 1u < vertsZ; ++z)
        {
            for (uint32 x = 0; x + 1u < vertsX; ++x)
            {
                const uint32 tl = vertexBase + z * vertsX + x;
                const uint32 tr = tl + 1u;
                const uint32 bl = vertexBase + (z + 1u) * vertsX + x;
                const uint32 br = bl + 1u;
                if (((x ^ z) & 1u) == 0u)
                {
                    indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
                }
                else
                {
                    indices.insert(indices.end(), {tl, bl, tr, tr, bl, br});
                }
            }
        }
    };

    for (uint32 lod = 0; lod < kGeometryLodCount; ++lod)
    {
        const bool outermost = lod + 1u == kGeometryLodCount;
        const PatchType leadSide = outermost ? PatchType::FatXOuter : PatchType::SlimX;
        const PatchType trailSide = outermost ? PatchType::FatXOuter : PatchType::FatX;
        const PatchType leadCorner = outermost ? PatchType::FatXZOuter : PatchType::SlimXZ;
        const PatchType trailCorner = outermost ? PatchType::FatXZOuter : PatchType::FatXZ;
        const PatchType topLeftCorner =
            outermost ? PatchType::FatXZOuter : PatchType::SlimXFatZ;
        const PatchType bottomRightCorner =
            outermost ? PatchType::FatXZOuter : PatchType::FatXSlimZ;

        std::vector<PatchPlacement> placements;
        placements.reserve(lod == 0u ? 16u : 12u);
        if (lod == 0u)
        {
            placements = {
                {-1.5f,  1.5f, topLeftCorner}, {-0.5f,  1.5f, leadSide},
                { 0.5f,  1.5f, leadSide},      { 1.5f,  1.5f, leadCorner},
                {-1.5f,  0.5f, trailSide},     {-0.5f,  0.5f, PatchType::Interior},
                { 0.5f,  0.5f, PatchType::Interior}, {1.5f, 0.5f, leadSide},
                {-1.5f, -0.5f, trailSide},     {-0.5f, -0.5f, PatchType::Interior},
                { 0.5f, -0.5f, PatchType::Interior}, {1.5f, -0.5f, leadSide},
                {-1.5f, -1.5f, trailCorner},   {-0.5f, -1.5f, trailSide},
                { 0.5f, -1.5f, trailSide},     { 1.5f, -1.5f, bottomRightCorner},
            };
        }
        else
        {
            placements = {
                {-1.5f,  1.5f, topLeftCorner}, {-0.5f,  1.5f, leadSide},
                { 0.5f,  1.5f, leadSide},      { 1.5f,  1.5f, leadCorner},
                {-1.5f,  0.5f, trailSide},     { 1.5f,  0.5f, leadSide},
                {-1.5f, -0.5f, trailSide},     { 1.5f, -0.5f, leadSide},
                {-1.5f, -1.5f, trailCorner},   {-0.5f, -1.5f, trailSide},
                { 0.5f, -1.5f, trailSide},     { 1.5f, -1.5f, bottomRightCorner},
            };
        }

        for (const PatchPlacement& placement : placements)
            appendPatch(placement, lod);
    }

    m_GridVertexCount = static_cast<uint32>(vertices.size() / 4u);
    m_GridIndexCount = static_cast<uint32>(indices.size());

    m_GridVB = ::GameEngine::Rendering::CreateVertexBuffer(
        device, vertices.data(), vertices.size() * sizeof(float32), "Ocean_Grid_VBO");
    m_GridIB = ::GameEngine::Rendering::CreateIndexBuffer(
        device, indices.data(), indices.size() * sizeof(uint32), "Ocean_Grid_IBO");
}

void OceanRenderFeature::RebuildGridMesh(::GameEngine::Rendering::IDevice* device, uint32 gridSize)
{
    if (!device)
        return;
    if (m_GridVB.IsValid())
        device->DestroyBuffer(m_GridVB);
    if (m_GridIB.IsValid())
        device->DestroyBuffer(m_GridIB);
    m_GridVB = {};
    m_GridIB = {};
    CreateGridMesh(device, gridSize);
}

void OceanRenderFeature::ApplyCascadeConfig(::GameEngine::Rendering::IDevice* device)
{
    if (!device)
        return;

    const float baseScale = m_CascadeBaseScale.load(std::memory_order_acquire);
    const uint32 resolution = m_CascadeResolution.load(std::memory_order_acquire);

    // Base scale takes effect on each sim's next snap (cheap, applied every frame);
    // a resolution change resizes the cascade textures. ConfigureCascades early-
    // returns when the resolution already matches, so steady frames are no-ops.
    if (baseScale > 0.0f || resolution > 0u)
    {
        if (m_FoamReady)
            m_FoamSim.ConfigureCascades(device, baseScale, resolution);
        if (m_SeabedDepthReady)
            m_SeabedDepth.ConfigureCascades(device, baseScale, resolution);
        if (m_FlowReady)
            m_Flow.ConfigureCascades(device, baseScale, resolution);
        if (m_DynWavesReady)
            m_DynWaves.ConfigureCascades(device, baseScale, resolution);
        if (m_WaveMaskReady)
            m_WaveMask.ConfigureCascades(device, baseScale, resolution);
        if (m_ClipReady)
            m_Clip.ConfigureCascades(device, baseScale, resolution);
        if (m_AlbedoReady)
            m_Albedo.ConfigureCascades(device, baseScale, resolution);
        if (m_CombineReady)
            m_CombineSim.ConfigureCascades(device, baseScale, resolution);
    }

    // Grid geometry density: rebuild only when the effective cell count changes.
    // 0 means the authored default, which also lets scenes without an
    // OceanRenderer revert after one is removed.
    const uint32 requestedGridSize = m_GeometryGridSize.load(std::memory_order_acquire);
    const uint32 gridSize = requestedGridSize == 0u
        ? kDefaultOceanGeometryGridSize
        : ClampOceanGeometryGridSize(requestedGridSize);
    if (gridSize != m_GridSize)
    {
        RebuildGridMesh(device, gridSize);
    }
}

void OceanRenderFeature::SetParams(const OceanParamsGPU& params)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Params = params;
    m_Params.WaveOriginOffsetX = m_WaveOriginOffsetX.load(std::memory_order_acquire);
    m_Params.WaveOriginOffsetZ = m_WaveOriginOffsetZ.load(std::memory_order_acquire);
    // Stash the authored underwater toggle; the per-frame submersion test ANDs
    // onto it in SetUnderwaterActive. The stored params' Underwater flag starts
    // at 0 (not submerged until the render node proves it).
    m_UnderwaterEnabled.store(params.Underwater != 0u, std::memory_order_release);
    m_Params.Underwater = 0u;
    m_UnderwaterByView.clear();
}

void OceanRenderFeature::OnOceanOriginShift(const OceanOriginShiftEvent& event)
{
    m_WaveOriginOffsetX.fetch_add(event.ShiftX, std::memory_order_acq_rel);
    m_WaveOriginOffsetZ.fetch_add(event.ShiftZ, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Params.WaveOriginOffsetX = m_WaveOriginOffsetX.load(std::memory_order_acquire);
        m_Params.WaveOriginOffsetZ = m_WaveOriginOffsetZ.load(std::memory_order_acquire);
        m_Params.SeaLevel -= event.ShiftY;
        for (OceanInputDrawPacket& packet : m_TypedInputPackets)
        {
            packet.CenterX -= event.ShiftX;
            packet.CenterZ -= event.ShiftZ;
        }
        for (UnderwaterVolumeBox& box : m_UnderwaterVolumes)
        {
            box.CenterX -= event.ShiftX;
            box.CenterY -= event.ShiftY;
            box.CenterZ -= event.ShiftZ;
        }
        for (UnderwaterVolumePolygon& polygon : m_UnderwaterVolumePolygons)
        {
            polygon.SurfaceY -= event.ShiftY;
            for (uint32 i = 0u; i < polygon.PointCount; ++i)
            {
                polygon.X[i] -= event.ShiftX;
                polygon.Z[i] -= event.ShiftZ;
            }
        }
        const auto shiftBoxes = [&](std::vector<UnderwaterVolumeBox>& boxes) {
            for (UnderwaterVolumeBox& box : boxes)
            {
                box.CenterX -= event.ShiftX;
                box.CenterY -= event.ShiftY;
                box.CenterZ -= event.ShiftZ;
            }
        };
        shiftBoxes(m_UnderwaterExclusionVolumes);
        shiftBoxes(m_UnderwaterPortalOccluders);
        for (WaterBodyBox& box : m_WaterBodies)
        {
            box.CenterX -= event.ShiftX;
            box.CenterZ -= event.ShiftZ;
        }
        for (WaterBodyStamp& stamp : m_WaterBodyStamps)
        {
            stamp.CenterX -= event.ShiftX;
            stamp.CenterZ -= event.ShiftZ;
        }
        for (WaterBodyPolygon& polygon : m_WaterBodyPolygons)
            for (uint32 i = 0u; i < polygon.PointCount; ++i)
            {
                polygon.X[i] -= event.ShiftX;
                polygon.Z[i] -= event.ShiftZ;
            }
        for (WaterBodyWaveBox& box : m_WaterBodyWaveBoxes)
        {
            box.CenterX -= event.ShiftX;
            box.CenterZ -= event.ShiftZ;
        }
        for (WaterBodyWavePolygon& polygon : m_WaterBodyWavePolygons)
            for (uint32 i = 0u; i < polygon.PointCount; ++i)
            {
                polygon.X[i] -= event.ShiftX;
                polygon.Z[i] -= event.ShiftZ;
            }
        for (WaterBodyWaveTextureBox& box : m_WaterBodyWaveTextureBoxes)
        {
            box.CenterX -= event.ShiftX;
            box.CenterZ -= event.ShiftZ;
        }
        for (auto &material : m_WaterMaterials)
        {
            material.Bounds[0] -= event.ShiftX;
            material.Bounds[2] -= event.ShiftX;
            material.Bounds[1] -= event.ShiftZ;
            material.Bounds[3] -= event.ShiftZ;
            for (auto &point : material.Points)
            {
                point[0] -= event.ShiftX;
                point[1] -= event.ShiftZ;
            }
        }
    }

    m_HeightField.OnOriginShift(event.ShiftX, event.ShiftZ, event.Teleport);
    for (OceanHeightField& heightField : m_LocalHeightField)
        heightField.OnOriginShift(event.ShiftX, event.ShiftZ, event.Teleport);
    m_FoamSim.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_Shadows.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_SprayGPU.RebaseOrigin(event.ShiftX, event.ShiftY, event.ShiftZ);
    m_SplineRaster.RebaseOrigin(event.ShiftX, event.ShiftZ, event.ShiftY);
    m_CombineSim.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_SeabedDepth.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_Flow.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_DynWaves.RebaseOrigin(event.ShiftX, event.ShiftZ);
    if (event.Teleport)
    {
        m_FoamSim.InvalidateHistory();
        m_DynWaves.InvalidateHistory();
    }
    m_DynWavesReadback.OnOriginShift(event.ShiftX, event.ShiftZ, event.Teleport);
    m_WaveMask.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_Clip.RebaseOrigin(event.ShiftX, event.ShiftZ);
    m_Albedo.RebaseOrigin(event.ShiftX, event.ShiftZ);
    if (m_CollisionProvider)
        m_CollisionProvider->Clear();
    m_GPUQuery.Clear();
}

OceanParamsGPU OceanRenderFeature::GetParams() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Params;
}

void OceanRenderFeature::PublishDepthClaim()
{
    m_LatestDepthInputs.Params = GetParams();
    const bool inputsChanged =
        std::memcmp(&m_LatestDepthInputs, &m_ClaimedDepthInputs, sizeof(SurfaceDepthInputs)) != 0;
    m_ClaimedDepthInputs = m_LatestDepthInputs;
    const bool waterInView = m_WaterInViewSincePublish.exchange(false, std::memory_order_acq_rel);
    m_SurfaceDepthChanging.store(waterInView && inputsChanged, std::memory_order_release);
}

void OceanRenderFeature::SetInputStats(const OceanInputFrameStats& stats)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_InputStats = stats;
}

OceanInputFrameStats OceanRenderFeature::GetInputStats() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_InputStats;
}

void OceanRenderFeature::SetTypedInputPackets(std::vector<OceanInputDrawPacket> packets)
{
    OceanInputDrawRegistry::Sort(packets);
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_TypedInputPackets = std::move(packets);
}

std::vector<OceanInputDrawPacket> OceanRenderFeature::GetTypedInputPackets() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_TypedInputPackets;
}

OceanSurfaceSample OceanRenderFeature::SampleSurface(float worldX, float worldZ) const
{
    m_SimulationDemand.NoteSurfaceQuery();
    return SampleSurfaceForRendering(worldX, worldZ);
}

OceanSurfaceSample OceanRenderFeature::SampleSurfaceForRendering(float worldX, float worldZ) const
{
    OceanParamsGPU params = GetParams();
    const std::vector<OceanInputDrawPacket> typedInputs = GetTypedInputPackets();
    if (!IsSurfaceQueryAllowed(worldX, worldZ))
        return MakeInvalidSurfaceSample(params, worldX, worldZ);

    OceanSurfaceSample sample = SampleOceanSurfaceAnalytic(params, worldX, worldZ);

    // Prefer the baked FFT height field: it carries the full spectrum, so gameplay
    // can query the same waves the camera renders. If neighbour reads miss near the
    // tile edge, keep the current normal and only replace the height.
    if (m_HeightFieldReady)
        ApplyBakedHeightFieldSample(m_HeightField, sample, params, worldX, worldZ);
    const WaterBodyWaveOverrideSample waveOverride =
        SampleWaterBodyWaveOverride(worldX, worldZ, params.Time);
    if (IsLocalHeightFieldReady(waveOverride.LocalFFTStream))
        BlendBakedHeightFieldSample(m_LocalHeightField[waveOverride.LocalFFTStream], sample,
                                    worldX, worldZ, waveOverride.LocalFFTBlend);
    sample.WaveWeight = waveOverride.Weight;
    sample.WaveChop = waveOverride.Chop;
    sample.TextureWaveMaskContributes = waveOverride.TextureWaveMaskContributes;
    sample.TextureWaveMaskCount = waveOverride.TextureWaveMaskCount;
    sample.LocalFFTBlend = waveOverride.LocalFFTBlend;
    sample.LocalFFTStream = waveOverride.LocalFFTStream;
    sample.LocalWaveHeight = waveOverride.LocalHeight;
    sample.LocalWaveSlopeX = waveOverride.LocalSlopeX;
    sample.LocalWaveSlopeZ = waveOverride.LocalSlopeZ;
    ApplyWaveWeight(sample, params, waveOverride.Weight, waveOverride.Chop);
    ApplyLocalWaveSample(sample, waveOverride.LocalHeight,
                         waveOverride.LocalSlopeX, waveOverride.LocalSlopeZ);
    ApplyTypedShapeInputs(typedInputs, sample, params, worldX, worldZ);
    if (m_DynWavesReadbackReady && IsDynWavesEnabled() && !m_DynWaves.IsQuiescent())
        ApplyDynamicWaveReadbackSample(m_DynWavesReadback, sample, worldX, worldZ,
                                       params.DynWavesAmplitude *
                                           std::max(waveOverride.Weight, 0.0f),
                                       params.DynWavesHorizontalDisplacement,
                                       params.DynWavesDisplacementClamp);
    ApplyWaterDepthSample(m_SeabedDepth, sample, params, worldX, worldZ);
    float ribbonDepth = 0;
    if (m_SplineRaster.SampleDepth(worldX, worldZ, ribbonDepth))
        ApplyWaterDepthValue(sample, params,
                             sample.HasWaterDepth ? std::min(sample.WaterDepth, ribbonDepth) : ribbonDepth);
    ApplyRasterDepthReadbackSample(m_RasterDepthCapture, sample, params, worldX, worldZ);

    return sample;
}

void OceanRenderFeature::SampleSurfaces(const OceanSurfaceQueryPoint* points, uint32 count,
                                        OceanSurfaceSample* outSamples) const
{
    if (!points || !outSamples || count == 0)
        return;

    m_SimulationDemand.NoteSurfaceQuery();
    const OceanParamsGPU params = GetParams();
    const std::vector<OceanInputDrawPacket> typedInputs = GetTypedInputPackets();
    for (uint32 i = 0; i < count; ++i)
    {
        if (!IsSurfaceQueryAllowed(points[i].X, points[i].Z))
        {
            outSamples[i] = MakeInvalidSurfaceSample(params, points[i].X, points[i].Z);
            continue;
        }

        OceanSurfaceSample sample =
            SampleOceanSurfaceAnalytic(params, points[i].X, points[i].Z);
        if (m_HeightFieldReady)
            ApplyBakedHeightFieldSample(m_HeightField, sample, params, points[i].X, points[i].Z);
        const WaterBodyWaveOverrideSample waveOverride =
            SampleWaterBodyWaveOverride(points[i].X, points[i].Z, params.Time);
        if (IsLocalHeightFieldReady(waveOverride.LocalFFTStream))
            BlendBakedHeightFieldSample(m_LocalHeightField[waveOverride.LocalFFTStream], sample,
                                        points[i].X, points[i].Z, waveOverride.LocalFFTBlend);
        sample.WaveWeight = waveOverride.Weight;
        sample.WaveChop = waveOverride.Chop;
        sample.TextureWaveMaskContributes = waveOverride.TextureWaveMaskContributes;
        sample.TextureWaveMaskCount = waveOverride.TextureWaveMaskCount;
        sample.LocalFFTBlend = waveOverride.LocalFFTBlend;
        sample.LocalFFTStream = waveOverride.LocalFFTStream;
        sample.LocalWaveHeight = waveOverride.LocalHeight;
        sample.LocalWaveSlopeX = waveOverride.LocalSlopeX;
        sample.LocalWaveSlopeZ = waveOverride.LocalSlopeZ;
        ApplyWaveWeight(sample, params, waveOverride.Weight, waveOverride.Chop);
        ApplyLocalWaveSample(sample, waveOverride.LocalHeight,
                             waveOverride.LocalSlopeX, waveOverride.LocalSlopeZ);
        ApplyTypedShapeInputs(typedInputs, sample, params, points[i].X, points[i].Z);
        if (m_DynWavesReadbackReady && IsDynWavesEnabled() && !m_DynWaves.IsQuiescent())
            ApplyDynamicWaveReadbackSample(m_DynWavesReadback, sample, points[i].X, points[i].Z,
                                           params.DynWavesAmplitude *
                                               std::max(waveOverride.Weight, 0.0f),
                                           params.DynWavesHorizontalDisplacement,
                                           params.DynWavesDisplacementClamp);
        ApplyWaterDepthSample(m_SeabedDepth, sample, params, points[i].X, points[i].Z);
        float ribbonDepth = 0;
        if (m_SplineRaster.SampleDepth(points[i].X, points[i].Z, ribbonDepth))
            ApplyWaterDepthValue(sample, params,
                                 sample.HasWaterDepth ? std::min(sample.WaterDepth, ribbonDepth)
                                                      : ribbonDepth);
        ApplyRasterDepthReadbackSample(m_RasterDepthCapture, sample, params,
                                       points[i].X, points[i].Z);
        outSamples[i] = sample;
    }
}

OceanCurrentSample OceanRenderFeature::SampleFlow(float worldX, float worldZ) const
{
    OceanCurrentSample sample{};
    if (!HasOcean() || !IsSurfaceQueryAllowed(worldX, worldZ))
        return sample;

    sample.Valid = true;
    if (m_FlowReady && IsFlowEnabled())
        sample = m_Flow.SampleFlow(worldX, worldZ);
    m_SplineRaster.ApplyFlow(worldX, worldZ, sample);
    return sample;
}

void OceanRenderFeature::BeginSimulationFrame()
{
    const OceanSimulationDemand::Clock::time_point now = OceanSimulationDemand::Clock::now();
    m_SimulationDemand.Update(now);
    if (!m_SimulationDemand.IsWaveDataStale(now))
        return;
    if (m_HeightFieldReady)
        m_HeightField.DiscardCpuData();
    for (uint32 stream = 0u; stream < kMaxOceanLocalFFTStreams; ++stream)
        if (IsLocalHeightFieldReady(stream))
            m_LocalHeightField[stream].DiscardCpuData();
}

void OceanRenderFeature::ForgetSimulationClaimsBefore(uint32 frameIndex)
{
    constexpr uint32 kNoClaim = 0xFFFFFFFFu;
    for (uint32* claim : {&m_LastFFTDispatchFrame, &m_LastSeabedDispatchFrame, &m_LastFlowDispatchFrame,
                          &m_LastWaveMaskDispatchFrame, &m_LastFoamDispatchFrame, &m_LastClipDispatchFrame,
                          &m_LastAlbedoDispatchFrame, &m_LastReflectionDispatchFrame,
                          &m_LastCombineDispatchFrame})
    {
        if (*claim != frameIndex)
            *claim = kNoClaim;
    }
    for (uint32& claim : m_LastLocalFFTDispatchFrame)
    {
        if (claim != frameIndex)
            claim = kNoClaim;
    }
}

OceanSurfaceQueryHandle OceanRenderFeature::EnqueueSurfaceQueries(
    const OceanSurfaceQueryPoint* points, uint32 count)
{
    if (!points || count == 0)
        return 0;

    PendingSurfaceQueryBatch batch{};
    batch.Handle = m_NextSurfaceQueryHandle.fetch_add(1, std::memory_order_relaxed);
    if (batch.Handle == 0)
        batch.Handle = m_NextSurfaceQueryHandle.fetch_add(1, std::memory_order_relaxed);
    batch.Points.assign(points, points + count);

    std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
    m_SurfaceQueryBatches.push_back(std::move(batch));
    return m_SurfaceQueryBatches.back().Handle;
}

void OceanRenderFeature::ProcessQueuedSurfaceQueries(uint32 maxBatches)
{
    struct WorkBatch
    {
        OceanSurfaceQueryHandle Handle = 0;
        std::vector<OceanSurfaceQueryPoint> Points;
    };

    std::vector<WorkBatch> work;
    {
        std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
        for (PendingSurfaceQueryBatch& batch : m_SurfaceQueryBatches)
        {
            if (batch.Ready || batch.InProgress)
                continue;
            batch.InProgress = true;
            WorkBatch w{};
            w.Handle = batch.Handle;
            w.Points = batch.Points;
            work.push_back(std::move(w));
            if (maxBatches != 0u && static_cast<uint32>(work.size()) >= maxBatches)
                break;
        }
    }

    for (WorkBatch& w : work)
    {
        std::vector<OceanSurfaceSample> samples(w.Points.size());
        SampleSurfaces(w.Points.data(), static_cast<uint32>(w.Points.size()), samples.data());

        std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
        auto it = std::find_if(m_SurfaceQueryBatches.begin(), m_SurfaceQueryBatches.end(),
                               [&](const PendingSurfaceQueryBatch& batch) {
                                   return batch.Handle == w.Handle;
                               });
        if (it == m_SurfaceQueryBatches.end())
            continue;
        it->Samples = std::move(samples);
        it->Ready = true;
        it->InProgress = false;
    }

    // Advance the owner-keyed provider from the same extraction-thread pump as
    // the legacy deferred batches. This keeps both APIs non-blocking and gives
    // them identical frame cadence.
    if (m_CollisionProvider)
        m_CollisionProvider->Update(maxBatches);
}

IOceanCollisionProvider& OceanRenderFeature::GetCollisionProvider()
{
    std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
    if (!m_CollisionProvider)
        m_CollisionProvider = std::make_unique<OceanFeatureCollisionProvider>(*this);
    return *m_CollisionProvider;
}

const IOceanCollisionProvider& OceanRenderFeature::GetCollisionProvider() const
{
    std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
    if (!m_CollisionProvider)
        m_CollisionProvider = std::make_unique<OceanFeatureCollisionProvider>(
            const_cast<OceanRenderFeature&>(*this));
    return *m_CollisionProvider;
}

bool OceanRenderFeature::IsSurfaceQueryReady(OceanSurfaceQueryHandle handle) const
{
    if (handle == 0)
        return false;
    std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
    auto it = std::find_if(m_SurfaceQueryBatches.begin(), m_SurfaceQueryBatches.end(),
                           [&](const PendingSurfaceQueryBatch& batch) {
                               return batch.Handle == handle;
                           });
    return it != m_SurfaceQueryBatches.end() && it->Ready;
}

bool OceanRenderFeature::CopySurfaceQueryResults(OceanSurfaceQueryHandle handle,
                                                 OceanSurfaceSample* outSamples,
                                                 uint32 maxCount, uint32* outCount,
                                                 bool consume)
{
    if (outCount)
        *outCount = 0;
    if (handle == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_SurfaceQueryMutex);
    auto it = std::find_if(m_SurfaceQueryBatches.begin(), m_SurfaceQueryBatches.end(),
                           [&](const PendingSurfaceQueryBatch& batch) {
                               return batch.Handle == handle;
                           });
    if (it == m_SurfaceQueryBatches.end() || !it->Ready)
        return false;

    const uint32 sampleCount = static_cast<uint32>(it->Samples.size());
    if (outCount)
        *outCount = sampleCount;
    if (outSamples)
    {
        if (maxCount < sampleCount)
            return false;
        std::copy(it->Samples.begin(), it->Samples.end(), outSamples);
    }

    if (consume && outSamples)
        m_SurfaceQueryBatches.erase(it);
    return true;
}

OceanParamsGPU OceanRenderFeature::GetParamsForView(::GameEngine::Rendering::ViewId viewId) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    OceanParamsGPU params = m_Params;
    if (auto it = m_UnderwaterByView.find(viewId); it != m_UnderwaterByView.end())
        params.Underwater = it->second.Underwater;
    return params;
}

float OceanRenderFeature::GetSubmergedDepthForView(::GameEngine::Rendering::ViewId viewId) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (auto it = m_UnderwaterByView.find(viewId); it != m_UnderwaterByView.end())
        return it->second.SubmergedDepth;
    return 0.0f;
}

void OceanRenderFeature::SetUnderwaterActive(::GameEngine::Rendering::ViewId viewId, bool active,
                                             float submergedDepth)
{
    const bool enabled = m_UnderwaterEnabled.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(m_Mutex);
    UnderwaterViewState& state = m_UnderwaterByView[viewId];
    state.Underwater = (enabled && active) ? 1u : 0u;
    state.SubmergedDepth = submergedDepth;
}

void OceanRenderFeature::SetUnderwaterVolumes(const std::vector<UnderwaterVolumeBox>& boxes)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_UnderwaterVolumes = boxes;
}

void OceanRenderFeature::SetUnderwaterVolumePolygons(
    const std::vector<UnderwaterVolumePolygon>& polygons)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_UnderwaterVolumePolygons = polygons;
}

void OceanRenderFeature::SetUnderwaterExclusionVolumes(const std::vector<UnderwaterVolumeBox>& boxes)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_UnderwaterExclusionVolumes = boxes;
}

void OceanRenderFeature::SetUnderwaterPortalOccluders(const std::vector<UnderwaterVolumeBox>& boxes)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_UnderwaterPortalOccluders = boxes;
}

bool OceanRenderFeature::HasUnderwaterVolumes() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_SplineRaster.HasUnderwater() || !m_UnderwaterVolumes.empty() ||
           !m_UnderwaterVolumePolygons.empty();
}

bool OceanRenderFeature::IsUnderwaterExcluded(float x, float y, float z) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const UnderwaterVolumeBox& b : m_UnderwaterExclusionVolumes)
    {
        if (x < b.CenterX - b.HalfX || x > b.CenterX + b.HalfX || y < b.CenterY - b.HalfY ||
            y > b.CenterY + b.HalfY || z < b.CenterZ - b.HalfZ || z > b.CenterZ + b.HalfZ)
            continue;
        return true;
    }
    return false;
}

bool OceanRenderFeature::TestUnderwaterVolume(float x, float y, float z, float& outDepth) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const UnderwaterVolumeBox& b : m_UnderwaterExclusionVolumes)
    {
        if (x < b.CenterX - b.HalfX || x > b.CenterX + b.HalfX || y < b.CenterY - b.HalfY ||
            y > b.CenterY + b.HalfY || z < b.CenterZ - b.HalfZ || z > b.CenterZ + b.HalfZ)
            continue;
        return false;
    }
    if (m_SplineRaster.UnderwaterDepth(x, y, z, outDepth))
        return true;
    for (const UnderwaterVolumeBox& b : m_UnderwaterVolumes)
    {
        if (x < b.CenterX - b.HalfX || x > b.CenterX + b.HalfX || y < b.CenterY - b.HalfY ||
            y > b.CenterY + b.HalfY || z < b.CenterZ - b.HalfZ || z > b.CenterZ + b.HalfZ)
            continue;
        outDepth = (b.CenterY + b.HalfY) - y; // depth below the box top (>= 0 inside)
        return true;
    }
    PolygonPointsXZ points;
    for (const UnderwaterVolumePolygon& p : m_UnderwaterVolumePolygons)
    {
        const float depth = std::max(p.Depth, 0.0f);
        if (depth <= 0.0f || y > p.SurfaceY || y < p.SurfaceY - depth)
            continue;
        if (!Mathematics::PointInPolygon(Mathematics::Vector2(x, z), GatherPolygonPointsXZ(p.PointCount, p.X, p.Z, points)))
            continue;
        outDepth = p.SurfaceY - y;
        return true;
    }
    return false;
}

Rendering::RenderGraph::RGUploadRing::Alloc OceanRenderFeature::UploadWaterMaterials(
    Rendering::RenderGraph::RGFrame& frame, uint64& outBytes) const
{
    constexpr uint64 kCountLaneBytes = 16u;
    std::lock_guard<std::mutex> lock(m_Mutex);
    outBytes = kCountLaneBytes + m_WaterMaterials.size() * sizeof(OceanWaterMaterialGPU);
    const auto upload = frame.AllocUpload(outBytes);
    if (!upload.Valid())
        return upload;
    std::memset(upload.Ptr, 0, kCountLaneBytes);
    static_cast<uint32*>(upload.Ptr)[0] = static_cast<uint32>(m_WaterMaterials.size());
    if (!m_WaterMaterials.empty())
        std::memcpy(static_cast<uint8*>(upload.Ptr) + kCountLaneBytes, m_WaterMaterials.data(),
                    outBytes - kCountLaneBytes);
    return upload;
}

void OceanRenderFeature::FillUnderwaterPortalData(OceanUnderwaterPortalData& data, uint32 maxVolumeBoxes,
                                                  uint32 maxExclusionBoxes, uint32 maxPolygons,
                                                  uint32 maxOccluderBoxes) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    data.Enabled = false;
    data.Materials.assign(m_WaterMaterials.begin(), m_WaterMaterials.end());
    data.Ribbons.clear();
    data.Volumes.clear();
    data.Polygons.clear();
    data.Exclusions.clear();
    data.Occluders.clear();
    if (!m_UnderwaterEnabled.load(std::memory_order_acquire))
        return;

    auto appendBox = [](std::vector<OceanUnderwaterPortalBox>& dst,
                        const UnderwaterVolumeBox& src, uint32 maxCount)
    {
        if (dst.size() >= static_cast<size_t>(maxCount) ||
            src.HalfX <= 0.0f || src.HalfY <= 0.0f || src.HalfZ <= 0.0f)
        {
            return;
        }
        OceanUnderwaterPortalBox b{};
        b.CenterX = src.CenterX;
        b.CenterY = src.CenterY;
        b.CenterZ = src.CenterZ;
        b.HalfX = src.HalfX;
        b.HalfY = src.HalfY;
        b.HalfZ = src.HalfZ;
        dst.push_back(b);
    };

    data.Volumes.reserve(std::min<uint32>(maxVolumeBoxes,
                                          static_cast<uint32>(m_UnderwaterVolumes.size())));
    data.Polygons.reserve(std::min<uint32>(maxPolygons,
                                           static_cast<uint32>(m_UnderwaterVolumePolygons.size())));
    data.Exclusions.reserve(std::min<uint32>(maxExclusionBoxes,
                                             static_cast<uint32>(m_UnderwaterExclusionVolumes.size())));
    data.Occluders.reserve(std::min<uint32>(maxOccluderBoxes,
                                            static_cast<uint32>(m_UnderwaterPortalOccluders.size())));

    for (const UnderwaterVolumeBox& b : m_UnderwaterVolumes)
        appendBox(data.Volumes, b, maxVolumeBoxes);

    for (const UnderwaterVolumePolygon& p : m_UnderwaterVolumePolygons)
    {
        if (data.Polygons.size() >= static_cast<size_t>(maxPolygons) ||
            p.PointCount < 3u || p.Depth <= 0.0f)
        {
            continue;
        }

        OceanUnderwaterPortalPolygon polygon{};
        polygon.SurfaceY = p.SurfaceY;
        polygon.Depth = p.Depth;
        polygon.PointCount = std::min<uint32>(
            p.PointCount, kMaxOceanUnderwaterPortalPolygonPoints);
        for (uint32 i = 0; i < polygon.PointCount; ++i)
        {
            polygon.X[i] = p.X[i];
            polygon.Z[i] = p.Z[i];
        }
        data.Polygons.push_back(polygon);
    }

    for (const UnderwaterVolumeBox& b : m_UnderwaterExclusionVolumes)
        appendBox(data.Exclusions, b, maxExclusionBoxes);

    for (const UnderwaterVolumeBox& b : m_UnderwaterPortalOccluders)
        appendBox(data.Occluders, b, maxOccluderBoxes);

    m_SplineRaster.CopyUnderwaterTree(data.Ribbons);
    data.Enabled = !data.Ribbons.empty() || !data.Volumes.empty() || !data.Polygons.empty();
}

void OceanRenderFeature::SetWaterBodies(const std::vector<WaterBodyBox>& boxes, bool constrainSurface)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_WaterBodies = boxes;
    m_WaterBodiesConstrainSurface = constrainSurface;
}

void OceanRenderFeature::SetWaterBodyStamps(const std::vector<WaterBodyStamp>& stamps)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_WaterBodyStamps = stamps;
}

void OceanRenderFeature::SetWaterBodyPolygons(const std::vector<WaterBodyPolygon>& polygons)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_WaterBodyPolygons = polygons;
}

void OceanRenderFeature::SetWaterBodyWaveOverrides(
    const std::vector<WaterBodyWaveBox>& boxes, const std::vector<WaterBodyWavePolygon>& polygons,
    const std::vector<WaterBodyWaveTextureBox>& textures)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_WaterBodyWaveBoxes = boxes;
    m_WaterBodyWavePolygons = polygons;
    m_WaterBodyWaveTextureBoxes = textures;
}

bool OceanRenderFeature::IsSurfaceQueryAllowed(float x, float z) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_WaterBodiesConstrainSurface)
        return true;
    if (m_SplineRaster.Contains(x, z))
        return true;
    for (const WaterBodyBox& b : m_WaterBodies)
    {
        if (std::abs(x - b.CenterX) <= b.HalfX && std::abs(z - b.CenterZ) <= b.HalfZ)
            return true;
    }
    for (const WaterBodyStamp& s : m_WaterBodyStamps)
    {
        const float halfWidth = std::max(s.HalfWidth, 0.0f);
        if (std::abs(x - s.CenterX) <= halfWidth && std::abs(z - s.CenterZ) <= halfWidth)
            return true;
    }
    PolygonPointsXZ points;
    for (const WaterBodyPolygon& p : m_WaterBodyPolygons)
    {
        if (Mathematics::PointInPolygon(Mathematics::Vector2(x, z), GatherPolygonPointsXZ(p.PointCount, p.X, p.Z, points)))
            return true;
    }
    return false;
}

bool OceanRenderFeature::IsWaterFootprintInView(const float* viewProj, float minY, float maxY,
                                                 float horizontalMargin) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const OceanFootprintFrustum frustum(viewProj);
    if (!m_WaterBodiesConstrainSurface)
        return frustum.IntersectsHorizontalBand(minY, maxY);
    const auto footprintInView = [&](float minX, float minZ, float maxX, float maxZ)
    {
        return frustum.Intersects(minX - horizontalMargin, minY, minZ - horizontalMargin,
                                  maxX + horizontalMargin, maxY, maxZ + horizontalMargin);
    };
    float ribbonMinX = 0.0f, ribbonMinZ = 0.0f, ribbonMaxX = 0.0f, ribbonMaxZ = 0.0f;
    if (m_SplineRaster.SurfaceBoundsXZ(ribbonMinX, ribbonMinZ, ribbonMaxX, ribbonMaxZ) &&
        footprintInView(ribbonMinX, ribbonMinZ, ribbonMaxX, ribbonMaxZ))
        return true;
    for (const WaterBodyBox& b : m_WaterBodies)
    {
        if (footprintInView(b.CenterX - b.HalfX, b.CenterZ - b.HalfZ, b.CenterX + b.HalfX,
                            b.CenterZ + b.HalfZ))
            return true;
    }
    for (const WaterBodyStamp& s : m_WaterBodyStamps)
    {
        const float halfWidth = std::max(s.HalfWidth, 0.0f);
        if (footprintInView(s.CenterX - halfWidth, s.CenterZ - halfWidth, s.CenterX + halfWidth,
                            s.CenterZ + halfWidth))
            return true;
    }
    for (const WaterBodyPolygon& p : m_WaterBodyPolygons)
    {
        if (frustum.IntersectsPolygon(p.X, p.Z, std::min(p.PointCount, kMaxOceanClipPolygonPoints),
                                      minY, maxY, horizontalMargin))
            return true;
    }
    return false;
}

float OceanRenderFeature::SampleWaterBodyWaveWeight(float x, float z) const
{
    return SampleWaterBodyWaveOverride(x, z, GetParams().Time).Weight;
}

OceanRenderFeature::WaterBodyWaveOverrideSample OceanRenderFeature::SampleWaterBodyWaveOverride(
    float x, float z, float time) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    WaterBodyWaveOverrideSample sample{};
    const float32 phaseX = x + m_Params.WaveOriginOffsetX;
    const float32 phaseZ = z + m_Params.WaveOriginOffsetZ;
    for (const WaterBodyWaveBox& b : m_WaterBodyWaveBoxes)
    {
        const float32 blend = RectInteriorWeight(x, z, b);
        if (blend <= 0.0f)
            continue;
        float32 localSlopeX = 0.0f;
        float32 localSlopeZ = 0.0f;
        const float32 localHeight = EvaluateLocalWaterBodyWavePacket(
            phaseX, phaseZ, time, b.LocalWaveCount, b.LocalAmplitude, b.LocalWavelength,
            b.LocalDirX, b.LocalDirZ, b.LocalWaveExtra, localSlopeX, localSlopeZ);
        sample.Weight = Lerp(sample.Weight, std::max(b.Weight, 0.0f), blend);
        sample.Chop = Lerp(sample.Chop, std::max(b.Chop, 0.0f), blend);
        const float32 sourceLocalFFTBlend = std::clamp(b.LocalFFTBlend, 0.0f, 1.0f);
        sample.LocalFFTBlend = Lerp(sample.LocalFFTBlend, sourceLocalFFTBlend, blend);
        if (sourceLocalFFTBlend > 1e-5f && blend > 1e-5f)
            sample.LocalFFTStream = std::min<uint32>(b.LocalFFTStream, kMaxOceanLocalFFTStreams - 1u);
        sample.LocalHeight = Lerp(sample.LocalHeight, localHeight, blend);
        sample.LocalSlopeX = Lerp(sample.LocalSlopeX, localSlopeX, blend);
        sample.LocalSlopeZ = Lerp(sample.LocalSlopeZ, localSlopeZ, blend);
    }
    for (const WaterBodyWavePolygon& p : m_WaterBodyWavePolygons)
    {
        const float32 blend = PolygonInteriorWeightXZ(x, z, p.PointCount, p.X, p.Z, p.Feather);
        if (blend <= 0.0f)
            continue;
        float32 localSlopeX = 0.0f;
        float32 localSlopeZ = 0.0f;
        const float32 localHeight = EvaluateLocalWaterBodyWavePacket(
            phaseX, phaseZ, time, p.LocalWaveCount, p.LocalAmplitude, p.LocalWavelength,
            p.LocalDirX, p.LocalDirZ, p.LocalWaveExtra, localSlopeX, localSlopeZ);
        sample.Weight = Lerp(sample.Weight, std::max(p.Weight, 0.0f), blend);
        sample.Chop = Lerp(sample.Chop, std::max(p.Chop, 0.0f), blend);
        const float32 sourceLocalFFTBlend = std::clamp(p.LocalFFTBlend, 0.0f, 1.0f);
        sample.LocalFFTBlend = Lerp(sample.LocalFFTBlend, sourceLocalFFTBlend, blend);
        if (sourceLocalFFTBlend > 1e-5f && blend > 1e-5f)
            sample.LocalFFTStream = std::min<uint32>(p.LocalFFTStream, kMaxOceanLocalFFTStreams - 1u);
        sample.LocalHeight = Lerp(sample.LocalHeight, localHeight, blend);
        sample.LocalSlopeX = Lerp(sample.LocalSlopeX, localSlopeX, blend);
        sample.LocalSlopeZ = Lerp(sample.LocalSlopeZ, localSlopeZ, blend);
    }
    for (const WaterBodyWaveTextureBox& t : m_WaterBodyWaveTextureBoxes)
    {
        float32 blend = RectInteriorWeight(x, z, t);
        if (blend <= 0.0f)
            continue;
        blend *= std::clamp(t.Coverage, 0.0f, 1.0f);
        if (blend <= 0.0f)
            continue;

        const float32 halfX = std::max(t.HalfX, 1e-3f);
        const float32 halfZ = std::max(t.HalfZ, 1e-3f);
        const float32 u = (x - (t.CenterX - halfX)) / (2.0f * halfX);
        const float32 v = (z - (t.CenterZ - halfZ)) / (2.0f * halfZ);
        float32 rgX = 1.0f;
        float32 rgY = 1.0f;
        t.Texture.SampleLinear(u, v, rgX, rgY);
        const float32 targetWeight = std::max(rgX * t.WeightScale + t.WeightBias, 0.0f);
        const float32 targetChop = std::max(rgY * t.ChopScale + t.ChopBias, 0.0f);
        sample.Weight = Lerp(sample.Weight, targetWeight, blend);
        sample.Chop = Lerp(sample.Chop, targetChop, blend);
        sample.TextureWaveMaskContributes = true;
        ++sample.TextureWaveMaskCount;
    }
    return sample;
}

void OceanRenderFeature::FillViewParams(
    OceanParamsGPU& out, ::GameEngine::Rendering::ViewId viewId, uint32 frameIndex,
    bool refractionAvailable, bool seabedDepthAvailable, bool causticsAvailable, bool flowAvailable,
    bool dynWavesAvailable, bool waveMaskAvailable, bool clipAvailable, bool albedoAvailable,
    bool foamTextureAvailable, bool causticsTextureAvailable, bool planarReflectionAvailable)
{
    OceanParamsGPU params = GetParamsForView(viewId);
    params.RefractionAvailable = refractionAvailable ? 1u : 0u;
    params.SeabedDepthAvailable = seabedDepthAvailable ? 1u : 0u;
    params.CausticsAvailable = causticsAvailable ? 1u : 0u;
    params.FlowAvailable = flowAvailable ? 1u : 0u;
    params.DynamicWavesAvailable = dynWavesAvailable ? 1u : 0u;
    params.ClipAvailable = clipAvailable ? 1u : 0u;
    params.AlbedoAvailable = albedoAvailable ? 1u : 0u;
    params.FoamTextureAvailable = foamTextureAvailable ? 1u : 0u;
    params.CausticsTextureAvailable = causticsTextureAvailable ? 1u : 0u;
    params.PlanarReflectionAvailable = planarReflectionAvailable ? 1u : 0u;
    params.CombineWavesAvailable = IsCombineReadyForFrame(frameIndex) ? 1u : 0u;
    params.WaveMaskAvailable = waveMaskAvailable ? 1u : 0u;
    params.LocalFFTAvailable = GetLocalFFTReadyMaskForFrame(frameIndex);
    out = params;
}

void OceanRenderFeature::EnsureForwardContributor(Engine::Renderer::RenderServices& rs)
{
    if (m_ForwardContributor)
        return;
    m_ForwardContributor = std::make_unique<OceanForwardContributor>(*this, rs);
}

bool OceanRenderFeature::BuildSurfaceCommandForView(
    Engine::Renderer::RenderServices& rs,
    Rendering::RenderGraph::RGFrame& frame,
    ::GameEngine::Rendering::ViewId viewId,
    uint32 frameIndex,
    Engine::Renderer::DrawCommand& outCommand,
    std::span<const Rendering::RenderGraph::RGTexture>* outSampledCascades)
{
    EnsureForwardContributor(rs);
    if (!m_ForwardContributor)
        return false;

    Engine::Renderer::ForwardEmitContext ctx{};
    ctx.Device = m_Device;
    ctx.Services = &rs;
    ctx.Frame = &frame;
    ctx.FrameIndex = frameIndex;
    ctx.ViewId = viewId;
    return m_ForwardContributor->BuildForwardCommand(ctx, outCommand, outSampledCascades);
}

} // namespace GameEngine::Ocean
