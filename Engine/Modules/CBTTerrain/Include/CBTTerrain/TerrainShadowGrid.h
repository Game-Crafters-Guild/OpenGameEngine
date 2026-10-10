#pragma once

#include <cstdint>
#include <optional>

namespace GameEngine::CBTTerrain
{

// The geometry of a terrain's sun-space clearance map (terrain_shadow.glsl): a grid whose axis u
// runs along the sun's horizontal direction and v across it, centred on the terrain, covering the
// terrain's rectangle at any sun azimuth. Mirrors GE_TerrainShadowGrid without its positions, which
// the publisher supplies in each consumer's frame.

// The largest map side in texels. A terrain whose diagonal needs more samples gets a coarser texel
// rather than a larger map (R4: 4096 m at 1 m cells, 1.41 m texels, 64 MiB).
inline constexpr uint32_t kTerrainShadowMaxSide = 4096u;
// Samples per bake line the kernel covers (256 threads x 16 samples): the cap above.
inline constexpr uint32_t kTerrainShadowLineCapacity = 4096u;
// Within this angle of the zenith (degrees) the sun casts no terrain shadow worth a bake.
inline constexpr float kTerrainShadowZenithDegrees = 0.5f;

struct TerrainShadowExtent
{
    // The terrain's extent in metres and its lattice cells per axis (height texture size - 1).
    float SizeX = 0.0f;
    float SizeZ = 0.0f;
    uint32_t CellsX = 0;
    uint32_t CellsZ = 0;

    bool operator==(const TerrainShadowExtent&) const = default;
};

struct TerrainShadowGrid
{
    // The sun's horizontal direction (unit length) and tan of its elevation.
    float SunX = 0.0f;
    float SunZ = 0.0f;
    float TanElevation = 0.0f;
    // tan of the elevation of the sun disc's lower edge, not below the horizon: the second ray the
    // bake follows, for the occluder of a lit receiver's penumbra. TanElevation for a point sun.
    float TanLowerEdge = 0.0f;
    // Metres between samples, the first sample's u and v relative to the terrain's centre, and the
    // samples per line and lines in use (both at most the map side).
    float Texel = 0.0f;
    float UMin = 0.0f;
    float VMin = 0.0f;
    uint32_t SamplesU = 0;
    uint32_t SamplesV = 0;

    // Exact: a grid differs whenever the sun moved at all (the sky writes it only when it does).
    bool operator==(const TerrainShadowGrid&) const = default;
};

// The lines of a bake dispatch.
struct TerrainShadowLines
{
    uint32_t First = 0;
    uint32_t Count = 0;
};

// The map's side in texels and its texel in metres for `extent`: one lattice cell, enlarged so the
// terrain's diagonal (its widest extent at any azimuth) fits within kTerrainShadowMaxSide. The map
// is allocated at this side once, so a turning sun never reallocates it. Zero for an empty extent.
uint32_t TerrainShadowMapSide(const TerrainShadowExtent& extent);
float TerrainShadowTexel(const TerrainShadowExtent& extent);

// The grid for the sun along `towardSun` (unit or not) whose disc has the angular radius
// atan(tanHalfAngle) (0: a point sun), or none when the sun casts no terrain shadow: at or below the
// horizon, or within kTerrainShadowZenithDegrees of the zenith.
std::optional<TerrainShadowGrid> ComputeTerrainShadowGrid(const TerrainShadowExtent& extent,
                                                          const float towardSun[3], float tanHalfAngle);

// The lines whose samples read heights inside the terrain-UV rectangle [minU, maxU] x [minV, maxV]
// (an edit's dirty rect): a sample's bilinear height reaches one lattice cell around it. Whole lines
// are re-baked, since a caster shadows everything downwind of it on its line. Empty for an empty
// rectangle.
TerrainShadowLines TerrainShadowDirtyLines(const TerrainShadowGrid& grid, const TerrainShadowExtent& extent,
                                           float minU, float minV, float maxU, float maxV);

} // namespace GameEngine::CBTTerrain
