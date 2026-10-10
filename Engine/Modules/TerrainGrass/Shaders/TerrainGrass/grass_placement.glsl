// Deterministic camera-relative grass placement arithmetic.
//
// MIRROR OBLIGATION: byte-for-byte counterpart of
// Engine/Modules/TerrainGrass/Include/TerrainGrass/GrassPlacementModel.h. The CPU sizes the
// dispatch and the instance budget from these same slot counts, so a drift mis-sizes the buffer
// this shader writes into. Change one, change the other, and re-run GrassPlacementModelTests.

#ifndef GRASS_PLACEMENT_GLSL
#define GRASS_PLACEMENT_GLSL

const float kGrassCellSize = 8.0;
const float kGrassCellArea = kGrassCellSize * kGrassCellSize;
const uint kGrassMaxSlotsPerCell = 4096u;
const float kGrassMinGrowth = 0.02;
const float kGrassGrowthSlotFraction = 0.15;
const uint kGrassLodCount = 2u;

// Largest cell window the dispatch can cover, and the cell-plan array both kernels index.
const uint kGrassMaxCellHalfSpan = 127u;
const uint kGrassMaxCells = (kGrassMaxCellHalfSpan * 2u + 1u) * (kGrassMaxCellHalfSpan * 2u + 1u);

// One cell's share of the instance pool, decided by the plan kernel and executed by the emit
// kernel: slot `s` of this cell always lands at `Base + s`. Nothing else assigns an index, which
// is what makes the placed set AND its order a pure function of the inputs.
struct GrassCellPlan
{
    uint Slots;           // slots this cell reserves; 0 means the cell places nothing
    uint SlotsWantedBits; // floatBitsToUint of the unrounded count — the growth ramp reads it
    uint LodAndTerrain;   // valid bit | LOD bit | terrain row
    uint Base;            // first reserved slot, counted from this LOD's end of the pool
};

// Distance rings the budget fit histograms over. A cell is culled past Bounds.w, which the CPU
// caps at the largest fitted radius plus one cell, so ring 128 is the last one reachable.
const uint kGrassMaxRings = kGrassMaxCellHalfSpan + 2u;
// The histogram sits ahead of the cell array in the same buffer, padded so the struct array that
// follows keeps its 16-byte alignment.
const uint kGrassRingHistogramWords = 132u;

const uint kGrassCellValidBit = 0x80000000u;
const uint kGrassCellLodShift = 30u;
const uint kGrassCellTerrainMask = 0x3FFFFFFFu;

// Blades per square metre at `distanceMeters` from the camera. nearDensity is the authored spec
// unit (blades/m² AT the camera) and is independent of terrain size.
float GrassDensityAtDistance(float nearDensity, float farRadius, float falloff, float distanceMeters)
{
    if (farRadius <= 0.0)
        return 0.0;
    float t = clamp(1.0 - distanceMeters / farRadius, 0.0, 1.0);
    return max(nearDensity, 0.0) * pow(t, max(falloff, 0.0));
}

// Unrounded slot count for a cell at this distance. The fraction IS the growth ramp.
float GrassSlotsWantedForCell(float nearDensity, float farRadius, float falloff, float distanceMeters)
{
    float wanted = GrassDensityAtDistance(nearDensity, farRadius, falloff, distanceMeters) * kGrassCellArea;
    return min(wanted, float(kGrassMaxSlotsPerCell));
}

// Scale in [0,1] applied to slot `slot`'s height and width, so the topmost slots of a cell scale in
// as the camera approaches instead of popping.
float GrassGrowthForSlot(float slotsWanted, uint slot)
{
    float growWidth = max(1.0, slotsWanted * kGrassGrowthSlotFraction);
    return clamp((slotsWanted - float(slot)) / growWidth, 0.0, 1.0);
}

uint GrassSlotsToIterate(float slotsWanted)
{
    if (slotsWanted <= 0.0)
        return 0u;
    return min(uint(ceil(slotsWanted)), kGrassMaxSlotsPerCell);
}

uint GrassHashSite(int cellX, int cellZ, uint slot, uint seed)
{
    uint h = uint(cellX) * 0x9E3779B1u;
    h ^= uint(cellZ) * 0x85EBCA77u;
    h ^= slot * 0xC2B2AE3Du;
    h ^= seed * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

uint GrassHashNext(uint h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float GrassHashToUnit(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Halton(2,3) inside the cell. Prefix-uniform for EVERY slot count, which is what lets a cell grow
// from 25 slots to 512 without moving a single existing blade.
float GrassRadicalInverse2(uint index)
{
    uint bits = index;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

float GrassRadicalInverse3(uint index)
{
    float result = 0.0;
    float denominator = 1.0 / 3.0;
    uint n = index;
    while (n > 0u)
    {
        result += float(n % 3u) * denominator;
        n /= 3u;
        denominator *= 1.0 / 3.0;
    }
    return result;
}

struct GrassSlotSample
{
    float CellU;
    float CellV;
    float Yaw01;
    float ScaleRand;
    float WidthRand;
    float MaskRand;
};

GrassSlotSample GrassSampleSlot(int cellX, int cellZ, uint slot, uint seed)
{
    // Cranley-Patterson rotation per cell: decorrelates neighbours without disturbing the
    // low-discrepancy property inside a cell.
    uint cellHash = GrassHashSite(cellX, cellZ, 0u, seed);
    uint cellHash2 = GrassHashNext(cellHash);
    float rotU = GrassHashToUnit(cellHash);
    float rotV = GrassHashToUnit(cellHash2);

    GrassSlotSample s;
    s.CellU = fract(GrassRadicalInverse2(slot) + rotU);
    s.CellV = fract(GrassRadicalInverse3(slot) + rotV);

    uint h = GrassHashSite(cellX, cellZ, slot + 1u, seed);
    s.Yaw01 = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.ScaleRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.WidthRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.MaskRand = GrassHashToUnit(h);
    return s;
}

#endif // GRASS_PLACEMENT_GLSL
