#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Deterministic camera-relative grass placement: the arithmetic shared by the CPU (budget fit,
// dispatch shape, instrumentation) and the GPU (terrain_grass_place.comp).
//
// MIRROR OBLIGATION: every function below has a byte-for-byte counterpart in
// Shaders/TerrainGrass/grass_placement.glsl. The two must agree exactly — the CPU derives the
// dispatch extent and the instance budget from the same slot counts the GPU spawns, so a drift
// silently mis-sizes the buffer the GPU writes. GrassPlacementModelTests pins the CPU side against
// golden values; a GPU-side drift shows up as a placed-vs-planned gap in the placement stats.
//
// Placement is anchored to a WORLD cell lattice, not to the camera: cell (cx, cz) always covers
// [cx*CellSize, (cx+1)*CellSize) in x and z, and a blade's position, yaw and scale are a pure
// function of (cellX, cellZ, slot, Seed). The camera only decides which cells are dispatched and
// how many slots each one fills, so a blade never moves when the camera does, and two views of the
// same ground place identical blades.
namespace GameEngine::TerrainGrass
{

// Cell edge in metres. One compute workgroup per cell, so this trades per-cell overhead (frustum
// test + terrain resolve) against how finely density tracks distance. A cell holds 1344 slots at
// the shipped 21 blades/m². Density is constant across a cell, so the near field steps by
// 1-(1-CellSize/FarRadius)^Falloff per cell. FarRadius here is the FITTED radius the compute runs
// on, not the authored one: at the shipped defaults that is 369 m and a 4.3 % step (it would be
// 3.2 % against the authored 500). Either way below the noise floor of the random scale.
inline constexpr float32 kGrassCellSize = 8.0f;
inline constexpr float32 kGrassCellArea = kGrassCellSize * kGrassCellSize;

// Hard per-cell slot ceiling; bounds the compute's inner loop and the worst-case cell cost.
// 4096 slots over a 64 m² cell is 64 blades/m², above any density this system is meant to serve.
inline constexpr uint32 kGrassMaxSlotsPerCell = 4096u;

// Slots below this growth are skipped. A blade at 2 % of its authored height is sub-pixel wherever
// the growth ramp puts it (the outermost metres of the fade radius), so dropping it is invisible
// and saves the whole vertex+raster cost.
inline constexpr float32 kGrassMinGrowth = 0.02f;

// Fraction of a cell's slots that are mid-growth at any moment. The topmost slots scale in as the
// camera approaches instead of popping into existence; spreading it over 15 % of the slots turns
// what would be a half-metre pop into tens of metres of camera travel. It also costs ~7 % of the
// nominal blade volume, which reads as size variation rather than as missing grass.
inline constexpr float32 kGrassGrowthSlotFraction = 0.15f;

// LOD 1 (a reduced blade, animated by the same wind field as LOD 0) starts at this fraction of
// the placement radius, so the split scales with the range instead of needing its own absolute
// knob. It is applied to the FITTED radius on both sides — GrassPlacementPlan::Lod1StartDistance
// and the vertex stage's distance band — because that is where the field actually ends. There is
// no separate fade radius: density reaches zero at the placement radius and blades leave by not
// being spawned.
inline constexpr float32 kGrassLod1RadiusFraction = 0.18f;

inline constexpr uint32 kGrassLodCount = 2u;

// Vulkan guarantees only 65535 workgroups in a dimension, and placement dispatches one per cell of
// a CellSpan x CellSpan window. 255 cells a side is 65025 workgroups, the largest square inside the
// guarantee; a longer authored range is shortened to fit by the same policy the instance budget
// uses. Without this an authored 2000 m range at a low density asks for 251001 workgroups.
inline constexpr uint32 kGrassMaxCellHalfSpan = 127u;

// Cells in the largest window, and so the length of the cell-plan array both kernels index.
inline constexpr uint32 kGrassMaxCells =
    (kGrassMaxCellHalfSpan * 2u + 1u) * (kGrassMaxCellHalfSpan * 2u + 1u);

// One cell's share of the instance pool. The plan kernel decides it, the emit kernel executes it:
// slot `s` of a cell always lands at `Base + s`, so the placed set and its order are a pure
// function of the inputs rather than of which workgroup reached an atomic first.
struct GrassCellPlanGPU
{
    uint32 Slots = 0;           // slots this cell reserves; 0 means the cell places nothing
    uint32 SlotsWantedBits = 0; // bit pattern of the unrounded count — the growth ramp reads it
    uint32 LodAndTerrain = 0;   // valid bit | LOD bit | terrain row
    uint32 Base = 0;            // first reserved slot, counted from this LOD's end of the pool
};
static_assert(sizeof(GrassCellPlanGPU) == 16);

// Distance rings the GPU budget fit histograms over, and the padded word count that histogram
// occupies ahead of the cell array in the same buffer (padded so the struct array stays 16-byte
// aligned). The CPU clears the histogram each frame, which is why it needs the size.
inline constexpr uint32 kGrassMaxRings = kGrassMaxCellHalfSpan + 2u;
inline constexpr uint32 kGrassRingHistogramWords = 132u;
static_assert(kGrassRingHistogramWords >= kGrassMaxRings);
static_assert(kGrassRingHistogramWords * sizeof(uint32) % 16u == 0u,
              "the cell array that follows the histogram must stay 16-byte aligned");
inline constexpr uint32 kGrassCellPlanBytes =
    kGrassRingHistogramWords * sizeof(uint32) + kGrassMaxCells * sizeof(GrassCellPlanGPU);

inline constexpr uint32 kGrassCellValidBit = 0x80000000u;
inline constexpr uint32 kGrassCellLodShift = 30u;
inline constexpr uint32 kGrassCellTerrainMask = 0x3FFFFFFFu;

// Assumed share of the placement disc that survives the per-cell frustum test. It sizes the
// DISPATCH WINDOW and the vertex stage's detail-fade band, and nothing else: the plan kernel
// re-fits the density radius on the GPU against the cells this view can actually see, so an
// assumption that is too low costs dispatch extent and one that is too high is corrected before a
// single slot is handed out. A 60 deg horizontal FOV covers 1/6 of the disc; the rest of this is
// near-field cells (always partly visible) and cells straddling the frustum edge.
inline constexpr float32 kGrassPlannedVisibleFraction = 0.35f;

// Authored placement inputs, resolved per terrain per view.
struct GrassPlacementParams
{
    // Blades per square metre AT THE CAMERA. This is the spec unit: it does not change with terrain
    // size, and the same value produces the same grass on a 512 m terrain and a 16 km one.
    //
    // NOT the authoring default. Components::TerrainGrass::BladesPerSquareMeter owns that; every
    // production path assigns this field before the fit reads it. The value below only mirrors the
    // component so the header does not read as a second authority, and
    // GrassGpuParamDefaultsMatchTheComponent pins the two together.
    float32 NearDensity = 21.0f;
    // Distance at which density reaches zero. Blades leave by not being spawned, so this is the
    // hard visibility range and there is no separate fade band.
    float32 FarRadius = 500.0f;
    // Shape of the falloff between the two. 1 = linear, higher = more of the budget spent near the
    // camera.
    float32 Falloff = 2.0f;
    float32 Seed = 3.0f;
};

// Blades per square metre at distance `distanceMeters` from the camera.
inline float32 GrassDensityAtDistance(const GrassPlacementParams& params, float32 distanceMeters)
{
    if (params.FarRadius <= 0.0f)
        return 0.0f;
    const float32 t = std::clamp(1.0f - distanceMeters / params.FarRadius, 0.0f, 1.0f);
    return std::max(params.NearDensity, 0.0f) * std::pow(t, std::max(params.Falloff, 0.0f));
}

// Unrounded slot count a cell at this distance wants. Kept fractional because the fraction IS the
// growth ramp: slot i is fully grown once this passes i + growth width, so the count changes
// continuously with camera distance and no blade ever appears at full size.
inline float32 GrassSlotsWantedForCell(const GrassPlacementParams& params, float32 distanceMeters)
{
    const float32 wanted = GrassDensityAtDistance(params, distanceMeters) * kGrassCellArea;
    return std::min(wanted, static_cast<float32>(kGrassMaxSlotsPerCell));
}

// Scale in [0,1] applied to slot `slot`'s height and width.
inline float32 GrassGrowthForSlot(float32 slotsWanted, uint32 slot)
{
    const float32 growWidth = std::max(1.0f, slotsWanted * kGrassGrowthSlotFraction);
    return std::clamp((slotsWanted - static_cast<float32>(slot)) / growWidth, 0.0f, 1.0f);
}

// Slots the compute iterates for a cell: every slot that could clear kGrassMinGrowth.
inline uint32 GrassSlotsToIterate(float32 slotsWanted)
{
    if (slotsWanted <= 0.0f)
        return 0u;
    return std::min(static_cast<uint32>(std::ceil(slotsWanted)), kGrassMaxSlotsPerCell);
}

// Half-width of the dispatched cell window, in cells.
inline uint32 GrassCellHalfSpan(float32 farRadius)
{
    if (farRadius <= 0.0f)
        return 0u;
    return static_cast<uint32>(std::ceil(farRadius / kGrassCellSize));
}

inline uint32 GrassCellSpan(float32 farRadius)
{
    return GrassCellHalfSpan(farRadius) * 2u + 1u;
}

// 32-bit integer hash of a placement site. Bijective mixing (a Wang-style finalizer over the three
// coordinates) rather than a float hash chain: the float hash the old lattice used aliased badly at
// large indices, and this has to stay stable for every (cell, slot) in a 16 km world.
inline uint32 GrassHashSite(int32 cellX, int32 cellZ, uint32 slot, uint32 seed)
{
    uint32 h = static_cast<uint32>(cellX) * 0x9E3779B1u;
    h ^= static_cast<uint32>(cellZ) * 0x85EBCA77u;
    h ^= slot * 0xC2B2AE3Du;
    h ^= seed * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

// The authored seed is a float on the component and a uint in the hash; the shader reinterprets it
// with floatBitsToUint, so this is the CPU mirror of that exact reinterpretation.
inline uint32 GrassSeedBits(float32 seed)
{
    uint32 bits = 0;
    std::memcpy(&bits, &seed, sizeof(bits));
    return bits;
}

// Successive decorrelated draws from one site hash.
inline uint32 GrassHashNext(uint32 h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline float32 GrassHashToUnit(uint32 h)
{
    return static_cast<float32>(h >> 8) * (1.0f / 16777216.0f);
}

// Radical inverse base 2 / base 3 — the Halton pair that places slots inside a cell.
//
// Why not a plain per-slot random offset: the slot count a cell spawns changes with camera
// distance, so ANY placement that depends on the count moves every blade whenever the count moves.
// Halton is prefix-uniform — slots 0..N-1 cover the cell evenly for every N — so a cell can grow
// from 25 slots to 512 without a single existing blade shifting, and there is no lattice structure
// to read as a grid (the pre-registered R2 risk).
inline float32 GrassRadicalInverse2(uint32 index)
{
    uint32 bits = index;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return static_cast<float32>(bits) * 2.3283064365386963e-10f;
}

inline float32 GrassRadicalInverse3(uint32 index)
{
    float32 result = 0.0f;
    float32 denominator = 1.0f / 3.0f;
    uint32 n = index;
    while (n > 0u)
    {
        result += static_cast<float32>(n % 3u) * denominator;
        n /= 3u;
        denominator *= 1.0f / 3.0f;
    }
    return result;
}

// A blade site: where it stands inside its cell and the decorrelated draws the compute turns into
// yaw, scale and colour. Pure in (cellX, cellZ, slot, seed) — this is the determinism contract.
struct GrassSlotSample
{
    float32 CellU = 0.0f; // [0,1) offset inside the cell, x
    float32 CellV = 0.0f; // [0,1) offset inside the cell, z
    float32 Yaw01 = 0.0f;
    float32 ScaleRand = 0.0f;
    float32 WidthRand = 0.0f;
    float32 MaskRand = 0.0f; // Bernoulli draw for the splat-mask feather
};

inline GrassSlotSample GrassSampleSlot(int32 cellX, int32 cellZ, uint32 slot, uint32 seed)
{
    // Cranley-Patterson rotation: a per-cell offset on the Halton pair decorrelates neighbouring
    // cells without disturbing the low-discrepancy property inside one.
    const uint32 cellHash = GrassHashSite(cellX, cellZ, 0u, seed);
    const uint32 cellHash2 = GrassHashNext(cellHash);
    const float32 rotU = GrassHashToUnit(cellHash);
    const float32 rotV = GrassHashToUnit(cellHash2);

    GrassSlotSample s;
    s.CellU = GrassRadicalInverse2(slot) + rotU;
    s.CellU -= std::floor(s.CellU);
    s.CellV = GrassRadicalInverse3(slot) + rotV;
    s.CellV -= std::floor(s.CellV);

    uint32 h = GrassHashSite(cellX, cellZ, slot + 1u, seed);
    s.Yaw01 = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.ScaleRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.WidthRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.MaskRand = GrassHashToUnit(h);
    return s;
}

// Candidate slots over the whole placement disc, before any culling.
//
// Closed form: integrating NearDensity*(1-r/R)^p over the disc gives
// 2*pi*D*R^2 / ((p+1)(p+2)), which is what makes the budget fit a single expression instead of a
// per-cell walk on the render thread.
inline double GrassPlannedCandidatesForDisc(const GrassPlacementParams& params)
{
    if (params.FarRadius <= 0.0f || params.NearDensity <= 0.0f)
        return 0.0;
    const double p = std::max(params.Falloff, 0.0f);
    const double r = params.FarRadius;
    constexpr double kTwoPi = 6.283185307179586;
    return kTwoPi * params.NearDensity * r * r / ((p + 1.0) * (p + 2.0));
}

// Result of fitting the authored placement to a per-view instance budget.
struct GrassPlacementPlan
{
    GrassPlacementParams Params{};   // Params after the fit (FarRadius may be shortened)
    uint32 CellSpan = 0;             // cells per side of the dispatched window
    uint32 CellCount = 0;            // CellSpan^2
    uint32 PlannedCandidates = 0;    // expected candidates after the assumed frustum share
    uint32 Capacity = 0;             // instance slots in the pool both LODs draw from
    float32 Lod1StartDistance = 0.0f;
    bool RangeReduced = false;       // the budget shortened FarRadius
};

// Fit authored placement to a per-view instance cap.
//
// The budget is spent NEAR FIRST: over-subscription shortens FarRadius rather than thinning
// NearDensity, because blades per m² at the camera is what the author specified and what they can
// see. Total candidates scale with R^2, so the fit is one square root.
inline GrassPlacementPlan GrassFitPlacementToBudget(const GrassPlacementParams& params,
                                                    uint32 instanceBudget)
{
    GrassPlacementPlan plan;
    plan.Params = params;
    if (instanceBudget == 0u || params.NearDensity <= 0.0f || params.FarRadius <= 0.0f)
        return plan;

    // The near field must survive any budget: without a floor a tiny cap would collapse the radius
    // to nothing and the author would see no grass at all rather than less range.
    constexpr float32 kMinFarRadius = 32.0f;

    const double planned = GrassPlannedCandidatesForDisc(params) * kGrassPlannedVisibleFraction;
    if (planned > static_cast<double>(instanceBudget))
    {
        const double shrink = std::sqrt(static_cast<double>(instanceBudget) / planned);
        const float32 fitted = static_cast<float32>(params.FarRadius * shrink);
        plan.Params.FarRadius = std::max(fitted, kMinFarRadius);
        plan.RangeReduced = true;
    }

    // Clamp the dispatch to what the device guarantees before deriving the window.
    plan.Params.FarRadius = std::min(plan.Params.FarRadius,
                                     static_cast<float32>(kGrassMaxCellHalfSpan) * kGrassCellSize);
    plan.CellSpan = GrassCellSpan(plan.Params.FarRadius);
    plan.CellCount = plan.CellSpan * plan.CellSpan;
    plan.Lod1StartDistance = plan.Params.FarRadius * kGrassLod1RadiusFraction;

    const double plannedFitted = GrassPlannedCandidatesForDisc(plan.Params) * kGrassPlannedVisibleFraction;
    plan.PlannedCandidates = static_cast<uint32>(
        std::min(plannedFitted, static_cast<double>(instanceBudget)));

    // One pool for both LOD ranges. A fixed split had to guess how much of each range the
    // frustum keeps, and no guess fits both a camera looking down (everything visible is the
    // near range) and one looking out (almost everything is the far range): on real content
    // the near range saturated its share and refused blades while the far range left most of
    // its own idle. The placement fills one pool from both ends instead, so each range gets
    // whatever the other does not use.
    plan.Capacity = instanceBudget;
    return plan;
}

} // namespace GameEngine::TerrainGrass
