// Voronoi clumping: the second world lattice that turns an evenly-spread field into tufts.
//
// MIRROR OBLIGATION: byte-for-byte counterpart of
// Engine/Modules/TerrainGrass/Include/TerrainGrass/GrassClumpModel.h. GrassPlacementModelTests
// pins the CPU side against golden values, so a drift between the two reshapes the field silently
// unless both move together. Change one, change the other, and re-run that suite.
//
// Included by all THREE grass shaders, which is the point: the placement compute writes the clump
// code into the instance's Flags word, the vertex stage forwards it through custom0, and the
// surface reads it back. One file owns that bit layout so the three cannot disagree.
//
// DETERMINISM: every value here is a pure function of (worldX, worldZ, clumpSize, seed). Nothing
// reads gl_InstanceIndex, a slot, a frame counter or the camera — the compacted instance slot a
// blade lands in is reallocated by a subgroup atomicAdd every frame, so anything derived from it
// re-rolls per frame and reads as field-wide jitter on a static camera.

#ifndef GRASS_CLUMP_GLSL
#define GRASS_CLUMP_GLSL

// The site hash this lattice draws from. Only the placement compute calls the sampler below, so in
// the vertex and fragment stages every symbol this pulls in is unreferenced and dead-stripped
// before SPIR-V — the include costs those stages nothing, and paying it keeps ONE definition of
// the clump for all three rather than a copy per stage.
#include "TerrainGrass/grass_placement.glsl" // GrassHashSite / GrassHashNext / GrassHashToUnit

// The clump lattice is a SECOND lattice over the same world at a different cell size, so it must
// not draw from the placement lattice's hash namespace: without a salt a clump cell would share
// its jitter with whatever placement cell carries the same integer coordinates.
const uint kGrassClumpSeedSalt = 0x51ED2A17u;

// Below this the lattice is finer than a blade and every blade is its own clump — the unclumped
// field at nine times the hash cost.
const float kGrassMinClumpSize = 0.05;

// The hue knob is authored in [0,1] and means "up to this much of a full swing": 30 degrees either
// way at 1.0. Past that grass stops reading as grass and starts reading as a colour bug.
const float kGrassMaxHueShiftRadians = 0.5235988; // 30 degrees

// How much of the authored hue variation a blade gets against its own tuft-mates, as a fraction of
// what whole tufts get against each other. Two granularities, one dial: tufts carry most of the
// spread because that is what makes a field read as many plants, and the inside of a tuft breaks up
// with the remainder. The two sum, so the total swing at the maximum is 30 * (1 + this) degrees.
const float kGrassBladeHueShare = 0.4;

// GrassBladeInstance::Flags bit layout. THE definition — GrassClumpModel.h mirrors these exact
// constants and every producer and consumer goes through the helpers below.
//
//   bits 0-1   LOD index (written per cell by the placement compute)
//   bits 2-9   clump colour code (8 bits)
//   bits 10-25 minimum projected width, as a multiple of the blade's authored width (16 bits)
//   bits 26-31 unused
const uint kGrassFlagLodMask = 3u;
const uint kGrassClumpCodeShift = 2u;
const uint kGrassClumpCodeMask = 255u;
const uint kGrassMinWidthScaleShift = 10u;
const uint kGrassMinWidthScaleMask = 65535u;

// Ceiling on how far the minimum-projected-width floor may widen a blade — and, necessarily the
// same number, the top of the range that floor is quantized over. A blade turned exactly edge-on
// projects to zero width and would ask for an unbounded one; past this it is a sliver among slivers
// and further widening only smears it. The two uses are one constant on purpose: encoding past the
// ceiling would spend range the clamp discards, and encoding short of it would cap blades the clamp
// was still meant to widen.
const float kGrassMaxWidthExpansion = 6.0;

// The clump attributes a blade inherits from the site it belongs to. Randoms rather than finished
// values: the consumer scales each one by its own authored variance, so a variance of zero is
// exactly the unclumped field and not merely close to it.
struct GrassClumpSample
{
    vec2 Center;      // winning site, world metres
    float ColorRand;  // [0,1) clump tint / hue offset
    float HeightRand; // [0,1) clump height multiplier
    float FacingRand; // [0,1) clump common facing, as a fraction of a turn
};

// Nearest of the nine jittered sites around worldXZ.
//
// Nine, not the true nearest: with sites jittered across the whole cell a site two cells away can
// occasionally be closer, so this is the standard Worley approximation rather than an exact
// Voronoi partition (and exactly what the Ghost of Tsushima talk describes). A handful of blades
// on a cell boundary join the second-nearest tuft, which is invisible in a field. What matters is
// that the assignment is a deterministic function of position.
//
// Distances are computed in CELL-LOCAL units — the fractional part of the position minus the
// site's offset in the same cell — so the subtraction never differences two world-scale floats. At
// 16 km the world-unit form would leave only millimetre resolution in the comparison; the local
// form keeps full mantissa however far from the origin the blade stands.
GrassClumpSample GrassSampleClump(vec2 worldXZ, float clumpSize, uint seed)
{
    float size = max(clumpSize, kGrassMinClumpSize);
    vec2 f = worldXZ / size;
    vec2 baseCell = floor(f);
    vec2 local = f - baseCell;
    int baseX = int(baseCell.x);
    int baseZ = int(baseCell.y);

    uint clumpSeed = seed ^ kGrassClumpSeedSalt;

    GrassClumpSample best;
    best.Center = vec2(0.0);
    best.ColorRand = 0.0;
    best.HeightRand = 0.0;
    best.FacingRand = 0.0;
    float bestDistanceSq = 3.4e38;
    // Fixed iteration order with a strict less-than, so an exact tie always resolves to the same
    // neighbour on both mirrors rather than to whichever one the compiler evaluated last.
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int cellX = baseX + dx;
            int cellZ = baseZ + dz;
            uint h0 = GrassHashSite(cellX, cellZ, 0u, clumpSeed);
            uint h1 = GrassHashNext(h0);
            float jitterX = GrassHashToUnit(h0);
            float jitterZ = GrassHashToUnit(h1);

            float offsetX = float(dx) + jitterX - local.x;
            float offsetZ = float(dz) + jitterZ - local.y;
            float distanceSq = offsetX * offsetX + offsetZ * offsetZ;
            if (distanceSq < bestDistanceSq)
            {
                bestDistanceSq = distanceSq;
                uint h2 = GrassHashNext(h1);
                uint h3 = GrassHashNext(h2);
                uint h4 = GrassHashNext(h3);
                best.Center = (vec2(float(cellX), float(cellZ)) + vec2(jitterX, jitterZ)) * size;
                best.ColorRand = GrassHashToUnit(h2);
                best.HeightRand = GrassHashToUnit(h3);
                best.FacingRand = GrassHashToUnit(h4);
            }
        }
    }
    return best;
}

// The clump's colour random quantized to the eight bits the instance's Flags word carries.
uint GrassClumpCodeFromRand(float colorRand)
{
    return uint(clamp(colorRand, 0.0, 0.99999994) * 256.0) & kGrassClumpCodeMask;
}

float GrassClumpRandFromCode(uint code)
{
    return float(code & kGrassClumpCodeMask) * (1.0 / 255.0);
}

// A blade's minimum projected width, carried as a MULTIPLE of the authored width the instance
// already holds. A ratio rather than a length so it needs no units and no range beyond the
// expansion ceiling, which is what lets it ride in 16 bits the instance was not using.
//
// The placement compute evaluates this once per blade; the vertex stage multiplies it back by the
// width it has already read. 0 means "no floor" and makes the vertex clamp a provable no-op — its
// lower bound then loses to the blade's own width and the original value is returned unchanged.
uint GrassMinWidthScaleToBits(float scale)
{
    float q = clamp(scale, 0.0, kGrassMaxWidthExpansion)
        * (float(kGrassMinWidthScaleMask) / kGrassMaxWidthExpansion);
    return uint(q + 0.5) & kGrassMinWidthScaleMask;
}

float GrassMinWidthScaleFromFlags(uint flags)
{
    return float((flags >> kGrassMinWidthScaleShift) & kGrassMinWidthScaleMask)
        * (kGrassMaxWidthExpansion / float(kGrassMinWidthScaleMask));
}

uint GrassPackFlags(uint lod, uint clumpCode, float minWidthScale)
{
    return (lod & kGrassFlagLodMask)
        | ((clumpCode & kGrassClumpCodeMask) << kGrassClumpCodeShift)
        | (GrassMinWidthScaleToBits(minWidthScale) << kGrassMinWidthScaleShift);
}

uint GrassLodFromFlags(uint flags)
{
    return flags & kGrassFlagLodMask;
}

uint GrassClumpCodeFromFlags(uint flags)
{
    return (flags >> kGrassClumpCodeShift) & kGrassClumpCodeMask;
}

// Symmetric variance around 1: `variance` 0 returns exactly 1.0, so an unclumped field is
// bit-identical to one with the height multiplier applied rather than merely indistinguishable.
float GrassClumpHeightMultiplier(float heightRand, float variance)
{
    float v = clamp(variance, 0.0, 1.0);
    return 1.0 + (heightRand * 2.0 - 1.0) * v;
}

// THE custom0 CHANNEL LAYOUT. One file owns all four components so the vertex stage and the
// surface cannot disagree about any of them.
//
// custom0 is the per-vertex channel the surface contract gives a user shader (surface_io.glsl).
// Grass also claims the adapter's tangent location (3) for vGrassWindStrength because procedural
// blades never set HAS_TANGENT, but that is a single float and the only other free slot below the
// 16-location hardware floor adapter_vertex.glsl spends. Grass needs six more per-blade quantities
// through these four components, so three of them carry packed payloads and only the per-blade
// random stays a plain float. The alternative — binding the instance buffer a second time in the
// fragment stage — costs a descriptor and a dependent load per fragment.
//
// The varying is `flat` (adapter_vertex.glsl declares vCustom0 flat), so these are bit patterns
// and not interpolants: every quantity here is constant across a blade, and a packed lane that
// interpolated would decode to noise between its vertices.
//
// THE BIAS IS LOAD-BEARING, not decoration. uintBitsToFloat of a small integer is a DENORMAL
// float: every payload below 0x00800000 has a zero exponent field. Denormals are exactly the
// values a driver is permitted to flush to zero, and a flushed payload does not degrade — it
// delivers terrain row 0 and clump 0, silently, on whatever hardware does it. Forcing the 1.0f
// exponent puts every payload in [1.0, 2.0), which is always normal, and costs one OR.
//
// The layout follows from that: bits 23-30 belong to the exponent, so every payload below lives in
// the 23 bits under it. Terrain rows are per-view terrain entities and 32767 is orders of magnitude
// more than a view can hold.
//
//   custom0.x  bits 0-11   far-LOD term          12-bit unorm
//              bits 12-22  gust (wind strength)  11-bit unorm
//   custom0.y              per-blade random, a plain float and the only unpacked component
//   custom0.z  bits 0-10   terrain normal X      11-bit snorm
//              bits 11-21  terrain normal Z      11-bit snorm
//   custom0.w  bits 0-14   terrain row
//              bits 15-22  clump colour code
//   (every lane) bits 23-30  the 1.0f exponent (never touched by a payload)
const uint kGrassCustomPackBias = 0x3F800000u; // 1.0f
const uint kGrassTerrainIndexMask = 0x7FFFu;
const uint kGrassCustomClumpShift = 15u;

uint GrassPackTerrainAndClumpBits(uint terrainIndex, uint clumpCode)
{
    return kGrassCustomPackBias | (terrainIndex & kGrassTerrainIndexMask)
           | ((clumpCode & kGrassClumpCodeMask) << kGrassCustomClumpShift);
}

float GrassPackTerrainAndClump(uint terrainIndex, uint clumpCode)
{
    return uintBitsToFloat(GrassPackTerrainAndClumpBits(terrainIndex, clumpCode));
}

uint GrassUnpackTerrainIndex(float packed)
{
    return floatBitsToUint(packed) & kGrassTerrainIndexMask;
}

float GrassUnpackClumpRand(float packed)
{
    return GrassClumpRandFromCode((floatBitsToUint(packed) >> kGrassCustomClumpShift)
                                  & kGrassClumpCodeMask);
}

// Quantization here is floor(x + 0.5) and never round(): GLSL leaves round()'s direction at
// exactly 0.5 to the implementation, and this pack has a byte-for-byte CPU mirror to agree
// with. Every argument below is non-negative by construction, which is what makes the two
// forms equivalent everywhere except at that one ambiguous point.
//
// custom0.x — the far-LOD term and the gust share one lane. Both are [0,1] scalars feeding smooth
// blends (a distance fade and a wind-shade multiplier spanning 0.66..1.08), so their quantization
// steps land far below an 8-bit output level. The per-blade random keeps its own full-precision
// component instead, because it also drives a DISCRETE choice — the atlas tile index through a
// floor() — where quantization would bias the tile distribution rather than dither a gradient.
// One constant per field: the mask IS the top code, so a widened field cannot keep a stale
// mask that silently truncates it.
const uint kGrassFarLodLevels = 4095u; // 12 bits
const uint kGrassGustLevels = 2047u; // 11 bits
const uint kGrassGustShift = 12u;

float GrassPackFarLodAndGust(float farLodTerm, float gust)
{
    uint lod = uint(floor(clamp(farLodTerm, 0.0, 1.0) * float(kGrassFarLodLevels) + 0.5));
    uint wind = uint(floor(clamp(gust, 0.0, 1.0) * float(kGrassGustLevels) + 0.5));
    return uintBitsToFloat(kGrassCustomPackBias | lod | (wind << kGrassGustShift));
}

float GrassUnpackFarLod(float packed)
{
    return float(floatBitsToUint(packed) & kGrassFarLodLevels) / float(kGrassFarLodLevels);
}

float GrassUnpackGust(float packed)
{
    return float((floatBitsToUint(packed) >> kGrassGustShift) & kGrassGustLevels)
           / float(kGrassGustLevels);
}

// custom0.z — the terrain normal at the blade's foot, XZ only with Y rebuilt from them.
//
// Two components rather than three is the form this normal already has everywhere it is stored:
// the terrain normal map keeps rg and the placement compute bakes NormalX/NormalZ, both rebuilding
// Y as sqrt(1 - x^2 - z^2). A terrain normal is upper-hemisphere by construction, so the sign of Y
// is never in question and nothing is lost.
//
// The encoding is signed about the MIDPOINT of the range, which makes -1, 0 and +1 round-trip
// exactly. That matters at 0: flat terrain has to decode to exactly (0, 1, 0) so a blade standing
// on it settles onto true vertical, not onto a normal a thousandth off it.
const uint kGrassNormalHalfLevels = 1023u; // 11 bits per component, signed about the midpoint
// NOT derived from the half-levels: this is a BIT mask, and the field's top code is
// 2 * halfLevels = 2046, one short of the 11 bits it occupies. Masking with 2046 would clear
// bit 0 and corrupt every odd code. The unorm fields above can share one constant because a
// 12- and an 11-bit unorm really do top out at all-ones; a snorm about the midpoint does not.
const uint kGrassNormalMask = 2047u;
const uint kGrassNormalZShift = 11u;

float GrassPackTerrainNormal(vec3 terrainNormal)
{
    vec2 nXZ = clamp(terrainNormal.xz, vec2(-1.0), vec2(1.0));
    float halfLevels = float(kGrassNormalHalfLevels);
    uint x = uint(floor(nXZ.x * halfLevels + halfLevels + 0.5));
    uint z = uint(floor(nXZ.y * halfLevels + halfLevels + 0.5));
    return uintBitsToFloat(kGrassCustomPackBias | x | (z << kGrassNormalZShift));
}

vec3 GrassUnpackTerrainNormal(float packed)
{
    uint bits = floatBitsToUint(packed);
    float halfLevels = float(kGrassNormalHalfLevels);
    float x = (float(bits & kGrassNormalMask) - halfLevels) / halfLevels;
    float z = (float((bits >> kGrassNormalZShift) & kGrassNormalMask) - halfLevels) / halfLevels;
    float y = sqrt(max(1.0 - x * x - z * z, 0.0));
    return normalize(vec3(x, y, z));
}

// Hue rotation about the achromatic axis (Rodrigues on the grey diagonal). Branch-free, preserves
// r+g+b, and needs no RGB/HSV round trip — the right shape for a jitter that must stay subtle and
// cost nothing per fragment.
vec3 GrassHueShift(vec3 color, float radians)
{
    const vec3 axis = vec3(0.57735026); // 1/sqrt(3)
    float c = cos(radians);
    float s = sin(radians);
    return color * c + cross(axis, color) * s + axis * dot(axis, color) * (1.0 - c);
}

#endif // GRASS_CLUMP_GLSL
