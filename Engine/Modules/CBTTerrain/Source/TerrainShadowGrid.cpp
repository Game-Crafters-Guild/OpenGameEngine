#include "CBTTerrain/TerrainShadowGrid.h"

#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::CBTTerrain
{

namespace
{
// The capped texel is grown by this factor so float rounding never makes the diagonal ask for one
// sample more than the cap holds.
constexpr float kCappedTexelMargin = 1.0001f;

float LatticeCell(const TerrainShadowExtent& extent)
{
    return std::min(extent.SizeX / static_cast<float>(extent.CellsX),
                    extent.SizeZ / static_cast<float>(extent.CellsZ));
}

bool IsEmpty(const TerrainShadowExtent& extent)
{
    return extent.SizeX <= 0.0f || extent.SizeZ <= 0.0f || extent.CellsX == 0u || extent.CellsZ == 0u;
}

float Diagonal(const TerrainShadowExtent& extent)
{
    return std::sqrt(extent.SizeX * extent.SizeX + extent.SizeZ * extent.SizeZ);
}

// Samples needed to span `length` metres at `texel`, both ends included.
uint32_t SamplesFor(float length, float texel)
{
    return static_cast<uint32_t>(std::ceil(length / texel)) + 1u;
}
} // namespace

uint32_t TerrainShadowMapSide(const TerrainShadowExtent& extent)
{
    if (IsEmpty(extent))
        return 0u;
    return std::min(SamplesFor(Diagonal(extent), LatticeCell(extent)), kTerrainShadowMaxSide);
}

float TerrainShadowTexel(const TerrainShadowExtent& extent)
{
    if (IsEmpty(extent))
        return 0.0f;
    const float cell = LatticeCell(extent);
    if (SamplesFor(Diagonal(extent), cell) <= kTerrainShadowMaxSide)
        return cell;
    // The cap's samples span the diagonal.
    return Diagonal(extent) / static_cast<float>(kTerrainShadowMaxSide - 1u) * kCappedTexelMargin;
}

std::optional<TerrainShadowGrid> ComputeTerrainShadowGrid(const TerrainShadowExtent& extent,
                                                          const float towardSun[3], float tanHalfAngle)
{
    if (IsEmpty(extent))
        return std::nullopt;
    const float horizontal = std::sqrt(towardSun[0] * towardSun[0] + towardSun[2] * towardSun[2]);
    const float up = towardSun[1];
    if (up <= 0.0f)
        return std::nullopt;
    const float length = std::sqrt(horizontal * horizontal + up * up);
    if (horizontal <= length * std::sin(kTerrainShadowZenithDegrees * Mathematics::Pi / 180.0f))
        return std::nullopt;

    TerrainShadowGrid grid;
    grid.SunX = towardSun[0] / horizontal;
    grid.SunZ = towardSun[2] / horizontal;
    grid.TanElevation = up / horizontal;
    grid.TanLowerEdge = grid.TanElevation;
    if (tanHalfAngle > 0.0f)
        grid.TanLowerEdge = std::tan(std::max(std::atan(grid.TanElevation) - std::atan(tanHalfAngle), 0.0f));
    grid.Texel = TerrainShadowTexel(extent);
    // Half extents of the terrain's rectangle along u and v.
    const float halfU = 0.5f * (std::fabs(grid.SunX) * extent.SizeX + std::fabs(grid.SunZ) * extent.SizeZ);
    const float halfV = 0.5f * (std::fabs(grid.SunZ) * extent.SizeX + std::fabs(grid.SunX) * extent.SizeZ);
    const uint32_t side = TerrainShadowMapSide(extent);
    grid.UMin = -halfU;
    grid.VMin = -halfV;
    grid.SamplesU = std::min(SamplesFor(2.0f * halfU, grid.Texel), side);
    grid.SamplesV = std::min(SamplesFor(2.0f * halfV, grid.Texel), side);
    return grid;
}

TerrainShadowLines TerrainShadowDirtyLines(const TerrainShadowGrid& grid, const TerrainShadowExtent& extent,
                                           float minU, float minV, float maxU, float maxV)
{
    if (maxU <= minU || maxV <= minV || grid.SamplesV == 0u || IsEmpty(extent))
        return {};
    // The rectangle's corners relative to the terrain's centre, projected on v.
    const float xs[2] = {(minU - 0.5f) * extent.SizeX, (maxU - 0.5f) * extent.SizeX};
    const float zs[2] = {(minV - 0.5f) * extent.SizeZ, (maxV - 0.5f) * extent.SizeZ};
    float lowV = 0.0f;
    float highV = 0.0f;
    bool first = true;
    for (float x : xs)
    {
        for (float z : zs)
        {
            const float v = z * grid.SunX - x * grid.SunZ;
            lowV = first ? v : std::min(lowV, v);
            highV = first ? v : std::max(highV, v);
            first = false;
        }
    }
    const float reach = LatticeCell(extent);
    const float lowLine = std::floor((lowV - reach - grid.VMin) / grid.Texel);
    const float highLine = std::ceil((highV + reach - grid.VMin) / grid.Texel);
    const float lastLine = static_cast<float>(grid.SamplesV - 1u);
    if (highLine < 0.0f || lowLine > lastLine)
        return {};
    const uint32_t firstLine = static_cast<uint32_t>(std::max(lowLine, 0.0f));
    const uint32_t endLine = static_cast<uint32_t>(std::min(highLine, lastLine)) + 1u;
    return {firstLine, endLine - firstLine};
}

} // namespace GameEngine::CBTTerrain
