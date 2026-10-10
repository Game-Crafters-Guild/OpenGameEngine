#include "Engine/Rendering/WindVolumeResolver.h"

#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

struct ExtractedWindVolume
{
    Components::WindVolume Volume{};
    uint64 EntityId = 0;
    float Center[3]{};
    float AxisX[3]{1.0f, 0.0f, 0.0f};
    float AxisY[3]{0.0f, 1.0f, 0.0f};
    float AxisZ[3]{0.0f, 0.0f, 1.0f};
    float HalfExtents[3]{0.5f, 0.5f, 0.5f};
    bool ValidSpatial = true;
};

float Clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

void Normalize3(float& x, float& y, float& z)
{
    const float len2 = x * x + y * y + z * z;
    if (len2 <= 1e-10f)
    {
        x = 1.0f;
        y = 0.0f;
        z = 0.0f;
        return;
    }

    const float invLen = 1.0f / std::sqrt(len2);
    x *= invLen;
    y *= invLen;
    z *= invLen;
}

float SpatialWeight(const ExtractedWindVolume& vol, float worldX, float worldY, float worldZ)
{
    if (vol.Volume.IsGlobal)
        return 1.0f;
    if (!vol.ValidSpatial)
        return 0.0f;

    const float vx = worldX - vol.Center[0];
    const float vy = worldY - vol.Center[1];
    const float vz = worldZ - vol.Center[2];

    const float lx = vx * vol.AxisX[0] + vy * vol.AxisX[1] + vz * vol.AxisX[2];
    const float ly = vx * vol.AxisY[0] + vy * vol.AxisY[1] + vz * vol.AxisY[2];
    const float lz = vx * vol.AxisZ[0] + vy * vol.AxisZ[1] + vz * vol.AxisZ[2];

    const float hx = std::max(vol.HalfExtents[0], 0.001f);
    const float hy = std::max(vol.HalfExtents[1], 0.001f);
    const float hz = std::max(vol.HalfExtents[2], 0.001f);

    float dist = 0.0f;
    if (vol.Volume.Shape == Components::WindVolumeShape::Sphere)
    {
        const float k1x = lx / hx;
        const float k1y = ly / hy;
        const float k1z = lz / hz;
        const float k1 = std::sqrt(k1x * k1x + k1y * k1y + k1z * k1z);
        const float k2x = lx / (hx * hx);
        const float k2y = ly / (hy * hy);
        const float k2z = lz / (hz * hz);
        const float k2 = std::sqrt(k2x * k2x + k2y * k2y + k2z * k2z);
        dist = std::max(0.0f, k1 > 0.0f && k2 > 0.0f ? k1 * (k1 - 1.0f) / k2 : 0.0f);
    }
    else if (vol.Volume.Shape == Components::WindVolumeShape::Capsule ||
             vol.Volume.Shape == Components::WindVolumeShape::Cylinder)
    {
        const float radius = std::max(hx, hz);
        const float axisCore = std::max(0.0f, hy - (vol.Volume.Shape == Components::WindVolumeShape::Capsule ? radius : 0.0f));
        const float qy = std::fabs(ly) - axisCore;
        const float qLen = std::sqrt(lx * lx + lz * lz);
        if (vol.Volume.Shape == Components::WindVolumeShape::Capsule)
        {
            dist = std::max(0.0f, std::sqrt(qLen * qLen + std::max(qy, 0.0f) * std::max(qy, 0.0f)) - radius);
        }
        else
        {
            const float dx = qLen - radius;
            const float dy = std::max(qy, 0.0f);
            dist = std::max(dx, 0.0f);
            if (dy > 0.0f)
                dist = std::sqrt(std::max(0.0f, dx) * std::max(0.0f, dx) + dy * dy);
        }
    }
    else
    {
        const float dx = std::max(0.0f, std::fabs(lx) - hx);
        const float dy = std::max(0.0f, std::fabs(ly) - hy);
        const float dz = std::max(0.0f, std::fabs(lz) - hz);
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    if (dist <= 0.0f)
        return 1.0f;

    const float blend = std::max(vol.Volume.BlendDistance, 0.0f);
    if (blend <= 0.0f || dist >= blend)
        return 0.0f;

    const float t = 1.0f - (dist / blend);
    return t * t * (3.0f - 2.0f * t);
}

void HashBytes(uint64& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

int32 Quantize(float value, float scale)
{
    return static_cast<int32>(std::lround(value * scale));
}

// Extraction runs once per tick per caller; the scratch keeps its capacity across calls
// instead of allocating per frame. thread_local because ResolveAt is also called from
// package extraction systems on their own threads.
std::vector<ExtractedWindVolume>& ExtractVolumes(ECS::World& world, uint32 layerMask)
{
    static thread_local std::vector<ExtractedWindVolume> volumes;
    volumes.clear();
    world.Query<
             ECS::Read<Components::WindVolume>,
             ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::WindVolume& volume,
                  const Components::WorldTransform& xf)
              {
                  if ((volume.LayerMask & layerMask) == 0u)
                      return;

                  ExtractedWindVolume extracted{};
                  extracted.Volume = volume;
                  extracted.EntityId = entity.id;
                  extracted.Center[0] = xf.matrix[12];
                  extracted.Center[1] = xf.matrix[13];
                  extracted.Center[2] = xf.matrix[14];

                  const float* m = xf.matrix;
                  const float ax[3] = {m[0], m[1], m[2]};
                  const float ay[3] = {m[4], m[5], m[6]};
                  const float az[3] = {m[8], m[9], m[10]};
                  const float sx = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                  const float sy = std::sqrt(ay[0] * ay[0] + ay[1] * ay[1] + ay[2] * ay[2]);
                  const float sz = std::sqrt(az[0] * az[0] + az[1] * az[1] + az[2] * az[2]);

                  if (sx <= 0.0f || sy <= 0.0f || sz <= 0.0f)
                  {
                      extracted.ValidSpatial = false;
                  }
                  else
                  {
                      extracted.AxisX[0] = ax[0] / sx; extracted.AxisX[1] = ax[1] / sx; extracted.AxisX[2] = ax[2] / sx;
                      extracted.AxisY[0] = ay[0] / sy; extracted.AxisY[1] = ay[1] / sy; extracted.AxisY[2] = ay[2] / sy;
                      extracted.AxisZ[0] = az[0] / sz; extracted.AxisZ[1] = az[1] / sz; extracted.AxisZ[2] = az[2] / sz;
                      extracted.HalfExtents[0] = sx * 0.5f;
                      extracted.HalfExtents[1] = sy * 0.5f;
                      extracted.HalfExtents[2] = sz * 0.5f;
                  }

                  volumes.push_back(extracted);
              });

    std::sort(volumes.begin(), volumes.end(), [](const ExtractedWindVolume& a, const ExtractedWindVolume& b)
    {
        if (a.Volume.Priority != b.Volume.Priority)
            return a.Volume.Priority < b.Volume.Priority;
        return a.EntityId < b.EntityId;
    });

    return volumes;
}

} // namespace

ResolvedWind WindVolumeResolver::ResolveAt(ECS::World& world,
                                           float32 worldX,
                                           float32 worldY,
                                           float32 worldZ,
                                           const ResolvedWind& baseWind,
                                           uint32 layerMask)
{
    const auto& volumes = ExtractVolumes(world, layerMask);

    ResolvedWind result = baseWind;
    for (const ExtractedWindVolume& volume : volumes)
    {
        const float influence = Clamp01(volume.Volume.Weight) * SpatialWeight(volume, worldX, worldY, worldZ);
        if (influence <= 0.0f)
            continue;

        float dirX = volume.Volume.DirectionX;
        float dirY = volume.Volume.DirectionY;
        float dirZ = volume.Volume.DirectionZ;
        Normalize3(dirX, dirY, dirZ);

        const ResolvedWind target{
            dirX * std::max(volume.Volume.Speed, 0.0f),
            dirY * std::max(volume.Volume.Speed, 0.0f),
            dirZ * std::max(volume.Volume.Speed, 0.0f),
            std::max(volume.Volume.Turbulence, 0.0f),
            std::max(volume.Volume.GustFrequency, 0.0f),
            std::max(volume.Volume.GustScale, 0.001f),
            influence};

        if (volume.Volume.BlendMode == Components::WindVolumeBlendMode::Override)
        {
            result.VelocityX += (target.VelocityX - result.VelocityX) * influence;
            result.VelocityY += (target.VelocityY - result.VelocityY) * influence;
            result.VelocityZ += (target.VelocityZ - result.VelocityZ) * influence;
            result.Turbulence += (target.Turbulence - result.Turbulence) * influence;
            result.GustFrequency += (target.GustFrequency - result.GustFrequency) * influence;
            result.GustScale += (target.GustScale - result.GustScale) * influence;
        }
        else if (target.VelocityX == 0.0f && target.VelocityY == 0.0f && target.VelocityZ == 0.0f)
        {
            result.VelocityX *= (1.0f - influence);
            result.VelocityY *= (1.0f - influence);
            result.VelocityZ *= (1.0f - influence);
            result.Turbulence *= (1.0f - influence);
            result.GustFrequency += (target.GustFrequency - result.GustFrequency) * influence;
            result.GustScale += (target.GustScale - result.GustScale) * influence;
        }
        else
        {
            result.VelocityX += target.VelocityX * influence;
            result.VelocityY += target.VelocityY * influence;
            result.VelocityZ += target.VelocityZ * influence;
            result.Turbulence += target.Turbulence * influence;
            result.GustFrequency += (target.GustFrequency - result.GustFrequency) * influence;
            result.GustScale += (target.GustScale - result.GustScale) * influence;
        }

        result.Weight = std::max(result.Weight, influence);
    }

    result.Turbulence = std::max(result.Turbulence, 0.0f);
    result.GustFrequency = std::max(result.GustFrequency, 0.0f);
    result.GustScale = std::max(result.GustScale, 0.001f);
    return result;
}

void WindVolumeResolver::ExtractGPU(ECS::World& world, std::vector<Rendering::WindVolumeGPU>& out,
                                    uint32 layerMask)
{
    const auto& volumes = ExtractVolumes(world, layerMask);
    out.clear();
    out.reserve(volumes.size());
    for (const auto& v : volumes)
    {
        Rendering::WindVolumeGPU gpu{};
        for (int i = 0; i < 3; ++i)
        {
            gpu.CenterShape[i] = v.Center[i];
            gpu.AxisXExtent[i] = v.AxisX[i];
            gpu.AxisYExtent[i] = v.AxisY[i];
            gpu.AxisZExtent[i] = v.AxisZ[i];
        }
        gpu.CenterShape[3] = v.Volume.IsGlobal ? -1.0f
            : !v.ValidSpatial ? -2.0f : static_cast<float>(v.Volume.Shape);
        gpu.AxisXExtent[3] = std::max(v.HalfExtents[0], 0.001f);
        gpu.AxisYExtent[3] = std::max(v.HalfExtents[1], 0.001f);
        gpu.AxisZExtent[3] = std::max(v.HalfExtents[2], 0.001f);
        float x = v.Volume.DirectionX, y = v.Volume.DirectionY, z = v.Volume.DirectionZ;
        Normalize3(x, y, z);
        const float speed = std::max(v.Volume.Speed, 0.0f);
        gpu.VelocityWeight[0] = x * speed;
        gpu.VelocityWeight[1] = y * speed;
        gpu.VelocityWeight[2] = z * speed;
        gpu.VelocityWeight[3] = Clamp01(v.Volume.Weight);
        gpu.Gust[0] = std::max(v.Volume.Turbulence, 0.0f);
        gpu.Gust[1] = std::max(v.Volume.GustFrequency, 0.0f);
        gpu.Gust[2] = std::max(v.Volume.GustScale, 0.001f);
        gpu.Gust[3] = std::max(v.Volume.BlendDistance, 0.0f);
        gpu.Mode[0] = static_cast<float>(v.Volume.BlendMode);
        out.push_back(gpu);
    }
}

uint64 WindVolumeResolver::QuantizedHash(const ResolvedWind& wind)
{
    uint64 hash = 1469598103934665603ull;
    const int32 values[] = {
        Quantize(wind.VelocityX, 1000.0f),
        Quantize(wind.VelocityY, 1000.0f),
        Quantize(wind.VelocityZ, 1000.0f),
        Quantize(wind.Turbulence, 1000.0f),
        Quantize(wind.GustFrequency, 1000.0f),
        Quantize(wind.GustScale, 1000.0f),
        Quantize(wind.Weight, 1000.0f),
    };
    HashBytes(hash, values, sizeof(values));
    return hash;
}

} // namespace GameEngine::Engine::Renderer
