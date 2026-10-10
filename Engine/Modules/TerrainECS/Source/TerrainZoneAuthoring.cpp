#include "TerrainECS/TerrainZoneAuthoring.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

ZoneFootprint FitZoneToDab(float32 hitX, float32 hitZ, float32 brushRadius,
                           const ZoneAuthoringSettings& settings)
{
    const float32 half = std::min(brushRadius + settings.Padding, settings.MaxExtent);
    ZoneFootprint fp;
    fp.CenterX = hitX;
    fp.CenterZ = hitZ;
    fp.ExtentX = half;
    fp.ExtentZ = half;
    return fp;
}

bool GrowZoneToDab(ZoneFootprint& footprint, float32 hitX, float32 hitZ,
                   float32 brushRadius, const ZoneAuthoringSettings& settings)
{
    // Union the existing footprint with the dab's padded AABB, then re-center.
    const float32 dabHalf = brushRadius + settings.Padding;
    const float32 curMinX = footprint.CenterX - footprint.ExtentX;
    const float32 curMaxX = footprint.CenterX + footprint.ExtentX;
    const float32 curMinZ = footprint.CenterZ - footprint.ExtentZ;
    const float32 curMaxZ = footprint.CenterZ + footprint.ExtentZ;

    const float32 minX = std::min(curMinX, hitX - dabHalf);
    const float32 maxX = std::max(curMaxX, hitX + dabHalf);
    const float32 minZ = std::min(curMinZ, hitZ - dabHalf);
    const float32 maxZ = std::max(curMaxZ, hitZ + dabHalf);

    const float32 newExtentX = (maxX - minX) * 0.5f;
    const float32 newExtentZ = (maxZ - minZ) * 0.5f;
    if (newExtentX > settings.MaxExtent || newExtentZ > settings.MaxExtent)
        return false;

    footprint.CenterX = (minX + maxX) * 0.5f;
    footprint.CenterZ = (minZ + maxZ) * 0.5f;
    footprint.ExtentX = newExtentX;
    footprint.ExtentZ = newExtentZ;
    return true;
}

uint32 PayloadDimForExtent(float32 extent, const ZoneAuthoringSettings& settings)
{
    // Full width = 2*extent meters; +1 so both edges land on a texel center.
    const float32 span = 2.0f * std::max(extent, 0.0f) * settings.TexelsPerMeter;
    const auto dim = static_cast<uint32>(std::lround(span)) + 1u;
    return std::clamp(dim, settings.MinDim, settings.MaxDim);
}

void WorldToPayloadTexel(const ZoneFootprint& footprint, uint32 width, uint32 height,
                         float32 worldX, float32 worldZ, float32& outTx, float32& outTz)
{
    // Local in [-Extent, +Extent] -> UV [0,1] -> texel [0, dim-1].
    const float32 u = footprint.ExtentX > 0.0f
        ? (worldX - footprint.CenterX) / (2.0f * footprint.ExtentX) + 0.5f : 0.5f;
    const float32 v = footprint.ExtentZ > 0.0f
        ? (worldZ - footprint.CenterZ) / (2.0f * footprint.ExtentZ) + 0.5f : 0.5f;
    outTx = u * static_cast<float32>(width - 1);
    outTz = v * static_cast<float32>(height - 1);
}

TerrainZonePayload RegrowPayload(const TerrainZonePayload& old,
                                 const ZoneFootprint& oldFootprint,
                                 const ZoneFootprint& newFootprint, uint32 newWidth,
                                 uint32 newHeight)
{
    TerrainZonePayload grown;
    grown.Allocate(old.Format, newWidth, newHeight);

    if (old.Width < 1 || old.Height < 1 || newWidth < 1 || newHeight < 1)
        return grown;

    // Place each old texel's world center into the new payload by nearest
    // texel. Constant density makes this an integer offset copy.
    for (uint32 oz = 0; oz < old.Height; ++oz)
    {
        const float32 v = old.Height > 1 ? static_cast<float32>(oz) / static_cast<float32>(old.Height - 1) : 0.5f;
        const float32 worldZ = oldFootprint.CenterZ + (v - 0.5f) * 2.0f * oldFootprint.ExtentZ;
        for (uint32 ox = 0; ox < old.Width; ++ox)
        {
            const float32 u = old.Width > 1 ? static_cast<float32>(ox) / static_cast<float32>(old.Width - 1) : 0.5f;
            const float32 worldX = oldFootprint.CenterX + (u - 0.5f) * 2.0f * oldFootprint.ExtentX;

            float32 tx = 0.0f, tz = 0.0f;
            WorldToPayloadTexel(newFootprint, newWidth, newHeight, worldX, worldZ, tx, tz);
            const int32 nx = static_cast<int32>(std::lround(tx));
            const int32 nz = static_cast<int32>(std::lround(tz));
            if (nx < 0 || nz < 0 || nx >= static_cast<int32>(newWidth) || nz >= static_cast<int32>(newHeight))
                continue;

            const std::size_t src = static_cast<std::size_t>(oz) * old.Width + ox;
            const std::size_t dst = static_cast<std::size_t>(nz) * newWidth + nx;
            if (old.IsSculpt())
                grown.Offsets[dst] = old.Offsets[src];
            else
                grown.Mask[dst] = old.Mask[src];
        }
    }
    return grown;
}

} // namespace GameEngine::TerrainECS
