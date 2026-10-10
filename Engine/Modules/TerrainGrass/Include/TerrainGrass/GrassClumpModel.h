#pragma once

#include "TerrainGrass/GrassPlacementModel.h" // GrassHashSite / GrassHashNext / GrassHashToUnit

#include <algorithm>
#include <cmath>

// Voronoi clumping: the second world lattice that turns an evenly-spread field into tufts.
//
// The placement lattice (GrassPlacementModel.h) is anti-clump BY CONSTRUCTION — Halton is
// low-discrepancy, so every cell fills as evenly as its slot count allows. That is what makes a
// field read as mown rather than grown. Clumping restores grouping WITHOUT touching that lattice:
// a second, coarser, world-anchored lattice assigns each blade to the nearest of nine jittered
// sites, and the winning site's hash drives the attributes that make a tuft read as one plant —
// common height, common facing, a shared colour, and a pull toward its centre.
//
// MIRROR OBLIGATION: byte-for-byte counterpart of
// Shaders/TerrainGrass/grass_clump.glsl. Every function below has a GLSL twin with the same name
// and the same arithmetic; GrassPlacementModelTests pins the CPU side against golden values, so a
// drift in either direction reds that suite rather than silently reshaping the field.
//
// DETERMINISM: every value below is a pure function of (worldX, worldZ, clumpSize, seed). Nothing
// reads an instance index, a slot, a frame counter or a camera. That is not a style preference —
// the compacted instance slot a blade lands in is reallocated by a subgroup atomicAdd every frame,
// so anything derived from it re-rolls per frame and reads as field-wide jitter on a static
// camera (the defect fixed in terrain_grass_vertex_modifier.glsl's bladeLean).
namespace GameEngine::TerrainGrass
{

// The clump lattice is a SECOND lattice over the same world, at a different cell size, so it must
// not draw from the placement lattice's hash namespace: without a salt a clump cell would share
// its jitter with whatever placement cell happens to carry the same integer coordinates, and the
// two structures would correlate wherever the sizes lined up. Any fixed odd constant that is not
// one of the placement mixer's multipliers does the job; this one is arbitrary and only has to
// stay stable.
inline constexpr uint32 kGrassClumpSeedSalt = 0x51ED2A17u;

// Floor on the authored clump size. Below this the lattice is finer than a blade and every blade
// is its own clump, which is just the unclumped field at nine times the hash cost.
inline constexpr float32 kGrassMinClumpSize = 0.05f;

// The hue jitter knob is authored in [0,1] and means "up to this much of a full swing". One turn of
// 30 degrees either way is the whole range a knob of 1.0 buys — beyond that grass stops reading as
// grass and starts reading as a colour bug.
inline constexpr float32 kGrassMaxHueShiftRadians = 0.5235988f; // 30 degrees


// GrassBladeInstance::Flags bit layout. THE definition — grass_clump.glsl mirrors these exact
// constants and every producer and consumer goes through the helpers below, so the layout cannot
// drift between the compute that writes it and the two stages that read it.
//
//   bits 0-1  LOD index (written per cell by the placement compute)
//   bits 2-9  clump colour code (8 bits, this file)
//   bits 10-25 minimum projected width, as a multiple of the blade's authored width (16 bits)
//   bits 26-31 unused
inline constexpr uint32 kGrassFlagLodMask = 3u;
inline constexpr uint32 kGrassClumpCodeShift = 2u;
inline constexpr uint32 kGrassClumpCodeMask = 255u;
inline constexpr uint32 kGrassMinWidthScaleShift = 10u;
inline constexpr uint32 kGrassMinWidthScaleMask = 65535u;

// Ceiling on how far the minimum-projected-width floor may widen a blade — and, necessarily the
// same number, the top of the range that floor is quantized over. A blade turned exactly edge-on
// projects to zero width and would ask for an unbounded one; past this it is a sliver among slivers
// and further widening only smears it. The two uses are one constant on purpose: encoding past the
// ceiling would spend range the clamp discards, and encoding short of it would cap blades the clamp
// was still meant to widen.
inline constexpr float32 kGrassMaxWidthExpansion = 6.0f;

// The clump attributes a blade inherits from the site it belongs to. Randoms rather than finished
// values: the consumer scales each one by its own authored variance, so a variance of zero is
// exactly the unclumped field and not merely close to it.
struct GrassClumpSample
{
    float32 CenterX = 0.0f;    // winning site, world metres
    float32 CenterZ = 0.0f;
    float32 ColorRand = 0.0f;  // [0,1) — clump tint / hue offset
    float32 HeightRand = 0.0f; // [0,1) — clump height multiplier
    float32 FacingRand = 0.0f; // [0,1) — clump common facing, as a fraction of a turn
    int32 CellX = 0;           // winning lattice cell, for tests and debugging
    int32 CellZ = 0;
};

// Nearest of the nine jittered sites around (worldX, worldZ).
//
// Nine, not the true nearest: with sites jittered across the whole cell a site two cells away can
// occasionally be closer, so this is the standard Worley approximation rather than an exact
// Voronoi partition (it is also exactly what the Ghost of Tsushima talk describes). The
// consequence is a handful of blades on a cell boundary joining the second-nearest tuft, which is
// invisible in a field and costs nothing to accept. What matters is that the assignment is a
// deterministic function of position, which it is.
//
// Distances are computed in CELL-LOCAL units — the fractional part of the position minus the
// site's offset in the same cell — so the subtraction never differences two world-scale floats.
// At 16 km the world-unit form would leave only millimetre resolution in the comparison; the local
// form keeps full mantissa regardless of how far from the origin the blade stands.
inline GrassClumpSample GrassSampleClump(float32 worldX, float32 worldZ, float32 clumpSize,
                                         uint32 seed)
{
    const float32 size = std::max(clumpSize, kGrassMinClumpSize);
    // Divide, exactly as grass_clump.glsl does: a reciprocal-multiply diverges from the shader
    // by 1 ulp on ~17% of positions, and this header exists to be bit-equal to the shader.
    const float32 fx = worldX / size;
    const float32 fz = worldZ / size;
    const int32 baseX = static_cast<int32>(std::floor(fx));
    const int32 baseZ = static_cast<int32>(std::floor(fz));
    const float32 localX = fx - static_cast<float32>(baseX);
    const float32 localZ = fz - static_cast<float32>(baseZ);

    const uint32 clumpSeed = seed ^ kGrassClumpSeedSalt;

    GrassClumpSample best;
    float32 bestDistanceSq = 3.4e38f;
    // Fixed iteration order with a strict less-than, so an exact tie always resolves to the same
    // neighbour on both mirrors rather than to whichever one the compiler evaluated last.
    for (int32 dz = -1; dz <= 1; ++dz)
    {
        for (int32 dx = -1; dx <= 1; ++dx)
        {
            const int32 cellX = baseX + dx;
            const int32 cellZ = baseZ + dz;
            const uint32 h0 = GrassHashSite(cellX, cellZ, 0u, clumpSeed);
            const uint32 h1 = GrassHashNext(h0);
            const float32 jitterX = GrassHashToUnit(h0);
            const float32 jitterZ = GrassHashToUnit(h1);

            const float32 offsetX = static_cast<float32>(dx) + jitterX - localX;
            const float32 offsetZ = static_cast<float32>(dz) + jitterZ - localZ;
            const float32 distanceSq = offsetX * offsetX + offsetZ * offsetZ;
            if (distanceSq < bestDistanceSq)
            {
                bestDistanceSq = distanceSq;
                const uint32 h2 = GrassHashNext(h1);
                const uint32 h3 = GrassHashNext(h2);
                const uint32 h4 = GrassHashNext(h3);
                best.CenterX = (static_cast<float32>(cellX) + jitterX) * size;
                best.CenterZ = (static_cast<float32>(cellZ) + jitterZ) * size;
                best.ColorRand = GrassHashToUnit(h2);
                best.HeightRand = GrassHashToUnit(h3);
                best.FacingRand = GrassHashToUnit(h4);
                best.CellX = cellX;
                best.CellZ = cellZ;
            }
        }
    }
    return best;
}

// The clump's colour random quantized to the eight bits the instance's Flags word carries.
inline uint32 GrassClumpCodeFromRand(float32 colorRand)
{
    const float32 clamped = std::clamp(colorRand, 0.0f, 0.99999994f);
    return static_cast<uint32>(clamped * 256.0f) & kGrassClumpCodeMask;
}

inline float32 GrassClumpRandFromCode(uint32 code)
{
    return static_cast<float32>(code & kGrassClumpCodeMask) * (1.0f / 255.0f);
}

// A blade's minimum projected width, carried as a MULTIPLE of the authored width the instance
// already holds. A ratio rather than a length so it needs no units and no range beyond the
// expansion ceiling, which is what lets it ride in 16 bits the instance was not using.
//
// The placement compute evaluates this once per blade; the vertex stage multiplies it back by the
// width it has already read. 0 means "no floor" and makes the vertex clamp a provable no-op — its
// lower bound then loses to the blade's own width and the original value is returned unchanged.
inline uint32 GrassMinWidthScaleToBits(float32 scale)
{
    const float32 q = std::clamp(scale, 0.0f, kGrassMaxWidthExpansion)
        * (static_cast<float32>(kGrassMinWidthScaleMask) / kGrassMaxWidthExpansion);
    return static_cast<uint32>(q + 0.5f) & kGrassMinWidthScaleMask;
}

inline float32 GrassMinWidthScaleFromFlags(uint32 flags)
{
    return static_cast<float32>((flags >> kGrassMinWidthScaleShift) & kGrassMinWidthScaleMask)
        * (kGrassMaxWidthExpansion / static_cast<float32>(kGrassMinWidthScaleMask));
}

inline uint32 GrassPackFlags(uint32 lod, uint32 clumpCode, float32 minWidthScale)
{
    return (lod & kGrassFlagLodMask)
        | ((clumpCode & kGrassClumpCodeMask) << kGrassClumpCodeShift)
        | (GrassMinWidthScaleToBits(minWidthScale) << kGrassMinWidthScaleShift);
}

inline uint32 GrassLodFromFlags(uint32 flags)
{
    return flags & kGrassFlagLodMask;
}

inline uint32 GrassClumpCodeFromFlags(uint32 flags)
{
    return (flags >> kGrassClumpCodeShift) & kGrassClumpCodeMask;
}

// The custom0 .x / .z / .w wire format the vertex stage writes and the surface reads, mirrored
// here so a test can pin it. See grass_clump.glsl for why the 1.0f exponent bias is load-bearing: without it the
// payload is a denormal float, and a denormal is precisely what a driver may flush to zero — which
// would deliver terrain row 0 and clump 0 rather than failing.
//
//   bits 0-14   terrain row
//   bits 15-22  clump colour code
//   bits 23-30  the 1.0f exponent
inline constexpr uint32 kGrassCustomPackBias = 0x3F800000u; // 1.0f
inline constexpr uint32 kGrassTerrainIndexMask = 0x7FFFu;
inline constexpr uint32 kGrassCustomClumpShift = 15u;

inline uint32 GrassPackTerrainAndClumpBits(uint32 terrainIndex, uint32 clumpCode)
{
    return kGrassCustomPackBias | (terrainIndex & kGrassTerrainIndexMask)
           | ((clumpCode & kGrassClumpCodeMask) << kGrassCustomClumpShift);
}

inline uint32 GrassUnpackTerrainIndexBits(uint32 packed)
{
    return packed & kGrassTerrainIndexMask;
}

inline uint32 GrassUnpackClumpCodeBits(uint32 packed)
{
    return (packed >> kGrassCustomClumpShift) & kGrassClumpCodeMask;
}

// The float the varying actually carries, so a test can assert it is a NORMAL float rather than
// trusting the arithmetic that says it should be.
inline float32 GrassPackedCustomAsFloat(uint32 packedBits)
{
    float32 f = 0.0f;
    std::memcpy(&f, &packedBits, sizeof(f));
    return f;
}

// Quantization is floor(x + 0.5), matching the shader: GLSL leaves round()'s direction at
// exactly 0.5 to the implementation, so round() here would not be the byte-for-byte twin it
// claims to be. Every argument is non-negative by construction.
//
// The other two packed lanes, mirrored for the same reason: they carry reinterpreted integers under
// the same exponent bias, and their field boundaries have to hold identically on both sides.
//
//   custom0.x  bits 0-11   far-LOD term          12-bit unorm
//              bits 12-22  gust (wind strength)  11-bit unorm
//   custom0.z  bits 0-10   terrain normal X      11-bit snorm
//              bits 11-21  terrain normal Z      11-bit snorm
// One constant per field: the mask IS the top code, so a widened field cannot keep a stale mask.
inline constexpr uint32 kGrassFarLodLevels = 4095u;
inline constexpr uint32 kGrassGustLevels = 2047u;
inline constexpr uint32 kGrassGustShift = 12u;

inline uint32 GrassPackFarLodAndGustBits(float32 farLodTerm, float32 gust)
{
    const uint32 lod = static_cast<uint32>(
        std::floor(std::clamp(farLodTerm, 0.0f, 1.0f) * static_cast<float32>(kGrassFarLodLevels) + 0.5f));
    const uint32 wind = static_cast<uint32>(
        std::floor(std::clamp(gust, 0.0f, 1.0f) * static_cast<float32>(kGrassGustLevels) + 0.5f));
    return kGrassCustomPackBias | lod | (wind << kGrassGustShift);
}

inline float32 GrassUnpackFarLodBits(uint32 packed)
{
    return static_cast<float32>(packed & kGrassFarLodLevels)
           / static_cast<float32>(kGrassFarLodLevels);
}

inline float32 GrassUnpackGustBits(uint32 packed)
{
    return static_cast<float32>((packed >> kGrassGustShift) & kGrassGustLevels)
           / static_cast<float32>(kGrassGustLevels);
}

// Signed about the MIDPOINT so -1, 0 and +1 all round-trip exactly. The exact 0 is the one that
// carries weight: flat terrain has to decode to exactly (0, 1, 0), or every blade standing on it
// settles onto a normal a thousandth off vertical.
inline constexpr uint32 kGrassNormalHalfLevels = 1023u;
// NOT derived from the half-levels: this is a BIT mask, and the field's top code is
// 2 * halfLevels = 2046, one short of the 11 bits it occupies. Masking with 2046 would clear
// bit 0 and corrupt every odd code.
inline constexpr uint32 kGrassNormalMask = 2047u;
inline constexpr uint32 kGrassNormalZShift = 11u;

inline uint32 GrassPackTerrainNormalBits(float32 normalX, float32 normalZ)
{
    const float32 halfLevels = static_cast<float32>(kGrassNormalHalfLevels);
    const uint32 x = static_cast<uint32>(std::floor(std::clamp(normalX, -1.0f, 1.0f) * halfLevels + halfLevels + 0.5f));
    const uint32 z = static_cast<uint32>(std::floor(std::clamp(normalZ, -1.0f, 1.0f) * halfLevels + halfLevels + 0.5f));
    return kGrassCustomPackBias | x | (z << kGrassNormalZShift);
}

inline float32 GrassUnpackTerrainNormalXBits(uint32 packed)
{
    const float32 halfLevels = static_cast<float32>(kGrassNormalHalfLevels);
    return (static_cast<float32>(packed & kGrassNormalMask) - halfLevels) / halfLevels;
}

inline float32 GrassUnpackTerrainNormalZBits(uint32 packed)
{
    const float32 halfLevels = static_cast<float32>(kGrassNormalHalfLevels);
    return (static_cast<float32>((packed >> kGrassNormalZShift) & kGrassNormalMask) - halfLevels) / halfLevels;
}

// Symmetric variance around 1: `variance` 0 returns exactly 1.0, so an unclumped field is
// bit-identical to one with the height multiplier applied rather than merely indistinguishable.
inline float32 GrassClumpHeightMultiplier(float32 heightRand, float32 variance)
{
    const float32 v = std::clamp(variance, 0.0f, 1.0f);
    return 1.0f + (heightRand * 2.0f - 1.0f) * v;
}

} // namespace GameEngine::TerrainGrass
