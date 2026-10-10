#pragma once

// SphereAnalyticModifiers — closed-form (analytic) sphere modifier evaluation for the height
// composition. A flatten's shape is a
// pure function of world direction; baking it through the sculpt store quantizes the exact circle
// to store texels (8 m/texel at R=50k, ~611 m/texel at Earth under the 128-page cap) — the measured
// shape-accuracy ceiling behind "a 100 m flatten is visibly polygonal / a ~1 km mound". Evaluating
// the same closed form AT SAMPLE TIME reproduces the exact shape at ANY planet radius.
//
// EvaluateSphereAnalyticFlatten is the exact twin of TerrainModifierSystem.cpp's
// EvaluateSpherePlacement flatten branch (tangent-plane metres + SphereShapeWeight smoothstep +
// relief-cancelling radial target), so a store-baked flatten and the analytic term agree wherever
// the store's texel budget can express the shape (the convergence oracle locks this).
//
// SampleSphereSculptComposed is the composition CHOKEPOINT: store bilinear + the analytic set.
// Every composed-height consumer (GPU VertexEval / crease / surface normal via the
// cbt_analytic.glsl twin, CPU physics collider / brush cursor) reads through one sampler pair, so
// routing the analytic term through the same seam keeps all of them consistent by construction.
// This header is device-free and inert by default: an empty set composes bit-identically to the
// plain store sample (the dark-ship oracle). The live wiring is gated on
// GE_TERRAIN_ANALYTIC_MODIFIERS (TerrainModifierSystem publishes the set; flag off = empty set).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "CBTTerrain/CBTPlanetShading.h"   // AnyTangent (the placement tangent frame)
#include "CBTTerrain/SphereSculptPaging.h" // SphereSculptSampler + SampleSphereSculptByDir

namespace GameEngine::CBTTerrain
{

// The bounded set size (kMaxSphereAnalyticModifiers) and the 12-float GPU packing stride live in
// CBTLayout.h with the rest of the GPU layout contract (they size the CBTFrameParams /
// CBTSurfaceParams uploads); this header pulls them in via SphereSculptPaging.h -> CBTLayout.h.

// One analytic circular flatten resolved onto the sphere (mirror of MakeSpherePlacement's flatten
// fields): centre direction + tangent frame + circle shape + radial target, all in world metres.
struct SphereAnalyticFlatten
{
    std::array<float, 3> N{};  // unit centre direction (planet centred at the world origin)
    std::array<float, 3> E1{}; // tangent frame at N (AnyTangent) — local X, metres via R * dot
    std::array<float, 3> E2{}; // = cross(N, E1) — local Z
    float Radius = 0.0f;       // circle radius, world metres
    float Falloff = 0.0f;      // smoothstep skirt beyond the edge, metres (0 = hard edge)
    float TargetRadius = 0.0f; // metres from planet centre the pad levels to
    float PlanetRadius = 0.0f;
    bool Valid = false; // false for a degenerate centre (world origin) — evaluates to 0
};

// Resolve a circular flatten at world position (px,py,pz) — the entity position, exactly like
// MakeSpherePlacement: N = normalize(pos), E1 = AnyTangent(N), E2 = N x E1.
inline SphereAnalyticFlatten MakeSphereAnalyticFlatten(float px, float py, float pz, float radius,
                                                       float falloff, float targetRadius,
                                                       float planetRadius)
{
    SphereAnalyticFlatten f{};
    const float len = std::sqrt(px * px + py * py + pz * pz);
    if (len <= 0.0f)
        return f; // Valid stays false — a modifier at the planet centre has no direction
    f.N = {px / len, py / len, pz / len};
    f.E1 = AnyTangent(f.N);
    f.E2 = {f.N[1] * f.E1[2] - f.N[2] * f.E1[1], f.N[2] * f.E1[0] - f.N[0] * f.E1[2],
            f.N[0] * f.E1[1] - f.N[1] * f.E1[0]};
    f.Radius = radius;
    f.Falloff = falloff;
    f.TargetRadius = targetRadius;
    f.PlanetRadius = planetRadius;
    f.Valid = true;
    return f;
}

// Height offset (metres) the flatten contributes at UNIT world direction (dx,dy,dz). reliefAtDir is
// the closed-form base relief (metres) at this direction — cancelled so the pad lands flat, the
// radial analogue of the planar h' = lerp(h, target, weight). Exact twin of EvaluateSpherePlacement
// (TerrainModifierSystem.cpp): far-hemisphere reject, tangent-plane metres, smoothstep edge weight.
inline float EvaluateSphereAnalyticFlatten(const SphereAnalyticFlatten& f, float dx, float dy,
                                           float dz, float reliefAtDir)
{
    if (!f.Valid)
        return 0.0f;
    if (dx * f.N[0] + dy * f.N[1] + dz * f.N[2] <= 0.0f)
        return 0.0f; // far hemisphere
    const float l0 = f.PlanetRadius * (dx * f.E1[0] + dy * f.E1[1] + dz * f.E1[2]);
    const float l1 = f.PlanetRadius * (dx * f.E2[0] + dy * f.E2[1] + dz * f.E2[2]);
    const float distFromEdge = f.Radius - std::sqrt(l0 * l0 + l1 * l1);
    float weight;
    if (distFromEdge <= 0.0f)
    {
        if (f.Falloff <= 0.0f)
            return 0.0f;
        const float t = std::fmin(std::fmax(1.0f + distFromEdge / f.Falloff, 0.0f), 1.0f);
        weight = t * t * (3.0f - 2.0f * t);
    }
    else
    {
        weight = 1.0f;
    }
    return (f.TargetRadius - f.PlanetRadius - reliefAtDir) * weight;
}

// One analytic brush DAB (sculpt shape-accuracy S3 — analytic-while-stroking): the interactive
// sphere brush's primitive falloff as a closed form of world direction. While a stroke is held
// the accumulating dabs are represented by a TRANSIENT set of these (exact circles at any
// radius); on mouse-up the queued dabs are committed to the store through the normal ApplyDab
// path and the transient set clears — the transient is never persisted, never undone.
//
// NUMERICAL FORM (deliberate): ApplyDab's per-texel falloff is smoothstep in TRUE ANGULAR
// distance (acos of a dot). That expression is fp32-degenerate exactly in S3's regime — for a
// few-metre brush at planet radius cos(angularRadius) rounds to 1.0f (cos(1e-4) = 1 - 5e-9,
// under one ULP), so a dot-then-acos eval cannot resolve the cap at all. The analytic dab
// therefore uses the SAME tangent-plane-metres form the proven flatten uses (l0/l1 dots against
// an E1/E2 frame — well-conditioned to Earth radius, §2.3 0.195 m): its falloff argument is
// sin(ang)/sin(angularRadius) instead of ang/angularRadius, identical at the centre and the cap
// edge and within angularRadius^2/6 (< 1e-5 for any sane brush/planet pair) everywhere between.
// The dab-vs-store oracle bounds the discrepancy against real ApplyDab texel writes.
struct SphereAnalyticDab
{
    std::array<float, 3> N{};   // unit dab centre direction
    std::array<float, 3> E1{};  // tangent frame at N (AnyTangent) — metres via PlanetRadius * dot
    std::array<float, 3> E2{};  // = cross(N, E1)
    float SinRadiusM = 0.0f;    // PlanetRadius * sin(angularRadius) — tangent-plane cap radius, m
    float Amplitude = 0.0f;     // signed centre height, metres (sign*strength, merged dabs sum)
    float PlanetRadius = 0.0f;
    float AngularRadius = 0.0f; // ApplyDab's cap radius, radians (merge key + mask support cone;
                                // not packed — the GPU consumes SinRadiusM)
    bool Valid = false;
};

// Resolve a dab at world centre direction (cx,cy,cz) — the brush hit direction, exactly like
// ApplySphereSculptDab: normalized centre, angular cap radius, signed amplitude.
inline SphereAnalyticDab MakeSphereAnalyticDab(float cx, float cy, float cz, float angularRadius,
                                               float amplitude, float planetRadius)
{
    SphereAnalyticDab d{};
    const float len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (len <= 0.0f || angularRadius <= 0.0f || planetRadius <= 0.0f)
        return d; // Valid stays false — mirrors ApplyDab's degenerate-input no-op
    const float sinR = std::sin(std::fmin(angularRadius, 0.5f * 3.14159265f));
    if (sinR <= 0.0f)
        return d;
    d.N = {cx / len, cy / len, cz / len};
    d.E1 = AnyTangent(d.N);
    d.E2 = {d.N[1] * d.E1[2] - d.N[2] * d.E1[1], d.N[2] * d.E1[0] - d.N[0] * d.E1[2],
            d.N[0] * d.E1[1] - d.N[1] * d.E1[0]};
    d.SinRadiusM = planetRadius * sinR;
    d.Amplitude = amplitude;
    d.PlanetRadius = planetRadius;
    d.AngularRadius = angularRadius;
    d.Valid = true;
    return d;
}

// Height offset (metres) the dab contributes at UNIT world direction (dx,dy,dz): smoothstep
// falloff of the tangent-plane distance over the cap radius (see the numerical-form note above).
inline float EvaluateSphereAnalyticDab(const SphereAnalyticDab& d, float dx, float dy, float dz)
{
    if (!d.Valid)
        return 0.0f;
    if (dx * d.N[0] + dy * d.N[1] + dz * d.N[2] <= 0.0f)
        return 0.0f; // far hemisphere
    const float l0 = d.PlanetRadius * (dx * d.E1[0] + dy * d.E1[1] + dz * d.E1[2]);
    const float l1 = d.PlanetRadius * (dx * d.E2[0] + dy * d.E2[1] + dz * d.E2[2]);
    const float t =
        std::fmin(std::fmax(1.0f - std::sqrt(l0 * l0 + l1 * l1) / d.SinRadiusM, 0.0f), 1.0f);
    return d.Amplitude * (t * t * (3.0f - 2.0f * t));
}

// True when unit (dx,dy,dz) lies inside the dab's cap — the crease-residency twin (a transient
// dab allocates no pages; the smoothstep support ends exactly at the cap edge, no skirt).
inline bool SphereAnalyticDabCovers(const SphereAnalyticDab& d, float dx, float dy, float dz)
{
    if (!d.Valid || dx * d.N[0] + dy * d.N[1] + dz * d.N[2] <= 0.0f)
        return false;
    const float l0 = d.PlanetRadius * (dx * d.E1[0] + dy * d.E1[1] + dz * d.E1[2]);
    const float l1 = d.PlanetRadius * (dx * d.E2[0] + dy * d.E2[1] + dz * d.E2[2]);
    return std::sqrt(l0 * l0 + l1 * l1) <= d.SinRadiusM;
}

// Pack one dab into the same 12-float (3 x vec4) slot layout the flattens ride (the GPU array is
// shared: flattens at [0, Count), dabs at [Count, Count+DabCount)):
// [N.xyz | SinRadiusM], [E1.xyz | Amplitude], [E2.xyz | 0]. PlanetRadius rides the param block's
// own radius field, exactly like the flatten packing. Locked by the GLSL parse test against
// cbt_analytic.glsl's CBT_EvalSphereAnalyticDab field consumption.
inline void PackSphereAnalyticDab(const SphereAnalyticDab& d, float* out12)
{
    out12[0] = d.N[0];
    out12[1] = d.N[1];
    out12[2] = d.N[2];
    out12[3] = d.SinRadiusM;
    out12[4] = d.E1[0];
    out12[5] = d.E1[1];
    out12[6] = d.E1[2];
    out12[7] = d.Amplitude;
    out12[8] = d.E2[0];
    out12[9] = d.E2[1];
    out12[10] = d.E2[2];
    out12[11] = 0.0f;
}

// A bounded view over the analytic modifiers active on a planet. Empty (Count 0 and DabCount 0)
// composes bit-identically to the plain store sample — the inert default every existing path
// keeps. Dabs (S3) are appended members so S2 positional initializers stay valid.
struct SphereAnalyticModifierSet
{
    const SphereAnalyticFlatten* Items = nullptr;
    uint32_t Count = 0u;
    const SphereAnalyticDab* Dabs = nullptr; // S3 transient brush dabs (mid-stroke only)
    uint32_t DabCount = 0u;
};

// Pack one flatten into the 12-float (3 x vec4) GPU layout both param blocks carry:
// [N.xyz | Radius], [E1.xyz | Falloff], [E2.xyz | TargetRadius]. PlanetRadius rides the param
// block's own radius field (PlanetParams.x / Radius), so it is not packed per modifier. The
// layout is locked by SphereSculptShapeAccuracyTests + the GLSL parse test against
// cbt_analytic.glsl's CBTSphereAnalyticFlatten field consumption.
inline void PackSphereAnalyticFlatten(const SphereAnalyticFlatten& f, float* out12)
{
    out12[0] = f.N[0];
    out12[1] = f.N[1];
    out12[2] = f.N[2];
    out12[3] = f.Radius;
    out12[4] = f.E1[0];
    out12[5] = f.E1[1];
    out12[6] = f.E1[2];
    out12[7] = f.Falloff;
    out12[8] = f.E2[0];
    out12[9] = f.E2[1];
    out12[10] = f.E2[2];
    out12[11] = f.TargetRadius;
}

// THE composition chokepoint: published store sample (dab + baked-modifier layers, paged bilinear)
// plus the analytic terms, at world direction (dx,dy,dz) — normalized here so store sampling (which
// tolerates non-unit dirs) and the analytic dot products agree on one direction. reliefAtDir is the
// closed-form base relief at that direction (only flatten consumes it; pass 0 when the caller has
// no relief). The future GLSL twin composes the same two terms inside CBT_SampleSculptDirSurf.
inline float SampleSphereSculptComposed(const SphereSculptSampler& s,
                                        const SphereAnalyticModifierSet& set, float dx, float dy,
                                        float dz, float reliefAtDir)
{
    float h = SampleSphereSculptByDir(s, dx, dy, dz);
    const bool anyFlattens = set.Count != 0u && set.Items != nullptr;
    const bool anyDabs = set.DabCount != 0u && set.Dabs != nullptr;
    if (!anyFlattens && !anyDabs)
        return h;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0f)
        return h;
    const float ux = dx / len, uy = dy / len, uz = dz / len;
    if (anyFlattens)
        for (uint32_t i = 0; i < set.Count; ++i)
            h += EvaluateSphereAnalyticFlatten(set.Items[i], ux, uy, uz, reliefAtDir);
    if (anyDabs)
        for (uint32_t i = 0; i < set.DabCount; ++i)
            h += EvaluateSphereAnalyticDab(set.Dabs[i], ux, uy, uz);
    return h;
}

// True when (dx,dy,dz) lies inside any analytic modifier's footprint (shape + falloff skirt) — the
// crease-residency twin. The edit-driven retess residency gate today reads "sculpt page allocated";
// an analytic modifier allocates no pages, so the live wiring extends the gate to
// page-resident OR analytic-footprint (design §3.a) — without it the pad rim never crease-refines.
inline bool SphereAnalyticFootprintCovers(const SphereAnalyticModifierSet& set, float dx, float dy,
                                          float dz)
{
    const bool anyFlattens = set.Count != 0u && set.Items != nullptr;
    const bool anyDabs = set.DabCount != 0u && set.Dabs != nullptr;
    if (!anyFlattens && !anyDabs)
        return false;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0f)
        return false;
    const float ux = dx / len, uy = dy / len, uz = dz / len;
    if (anyFlattens)
        for (uint32_t i = 0; i < set.Count; ++i)
        {
            const SphereAnalyticFlatten& f = set.Items[i];
            if (!f.Valid || ux * f.N[0] + uy * f.N[1] + uz * f.N[2] <= 0.0f)
                continue;
            const float l0 = f.PlanetRadius * (ux * f.E1[0] + uy * f.E1[1] + uz * f.E1[2]);
            const float l1 = f.PlanetRadius * (ux * f.E2[0] + uy * f.E2[1] + uz * f.E2[2]);
            if (std::sqrt(l0 * l0 + l1 * l1) <= f.Radius + std::fmax(f.Falloff, 0.0f))
                return true;
        }
    if (anyDabs)
        for (uint32_t i = 0; i < set.DabCount; ++i)
            if (SphereAnalyticDabCovers(set.Dabs[i], ux, uy, uz))
                return true;
    return false;
}

// ---- Per-cell placement culling (sculpt shape-accuracy S3) ---------------------------------
//
// The S2 crease-residency gate ran the covers loop over ALL placements per live facet on a page
// miss — the measured CBT.Update driver (0.52 -> 0.87 ms from N=1 -> 8 at the 150 m pose). The
// cure: each cube face is split into a kSphereAnalyticCellGrid^2 cell grid and the CPU publishes,
// with the placement set, a 16-bit mask per cell of which placements' supports intersect that
// cell (conservative bounding-cone test). The GPU reads the mask for the cell containing its
// sample point and loops only the set bits — a facet outside every footprint pays one mask read.
// Correctness: a placement contributing at a direction implies its support contains that
// direction, which lies in the queried cell, so its bit is set — masked evaluation is EXACT,
// never a lossy approximation. Cell mapping is locked to CBT_AnalyticCellMask by the parse test.

// Cell index for a face-local UV: cell = face*Grid^2 + cellY*Grid + cellX, cellX = floor(u*Grid)
// clamped. Mask word w = cell >> 1; low 16 bits = even cell, high 16 = odd cell.
inline uint32_t SphereAnalyticCellIndex(uint32_t face, float u, float v)
{
    const float g = static_cast<float>(kSphereAnalyticCellGrid);
    const auto clampCell = [](float t) {
        const int32_t c = static_cast<int32_t>(t);
        return static_cast<uint32_t>(
            std::min(std::max(c, 0), static_cast<int32_t>(kSphereAnalyticCellGrid) - 1));
    };
    return face * kSphereAnalyticCellGrid * kSphereAnalyticCellGrid +
           clampCell(v * g) * kSphereAnalyticCellGrid + clampCell(u * g);
}

// Build the per-cell placement masks for a combined set (flattens at bits [0, flattenCount),
// dabs at bits [flattenCount, flattenCount+dabCount) — the GPU slot order). `outWords` is
// kSphereAnalyticCellMaskWords uint32s (2 cells per word), fully rewritten. Conservative: a
// placement's support cone (cap + skirt) is tested against each cell's bounding cone (centre
// direction + half-angle, precomputed once); overlap sets the bit. Cost is cells x placements
// on publish/upload — microseconds, never per-vertex.
inline void BuildSphereAnalyticCellMasks(const SphereAnalyticFlatten* flattens,
                                         uint32_t flattenCount, const SphereAnalyticDab* dabs,
                                         uint32_t dabCount, uint32_t* outWords)
{
    struct CellGeom
    {
        std::array<float, 3> Center{};
        float Cos = 1.0f, Sin = 0.0f; // cos/sin of the cell bounding-cone half-angle (+ margin)
    };
    static const std::array<CellGeom, kSphereAnalyticCellCount> kCells = [] {
        std::array<CellGeom, kSphereAnalyticCellCount> cells{};
        constexpr float kMarginRad = 1e-3f; // fp conservatism on the cone test
        const float g = static_cast<float>(kSphereAnalyticCellGrid);
        for (uint32_t face = 0; face < 6u; ++face)
            for (uint32_t cy = 0; cy < kSphereAnalyticCellGrid; ++cy)
                for (uint32_t cx = 0; cx < kSphereAnalyticCellGrid; ++cx)
                {
                    CellGeom& cell =
                        cells[face * kSphereAnalyticCellGrid * kSphereAnalyticCellGrid +
                              cy * kSphereAnalyticCellGrid + cx];
                    float cxd, cyd, czd;
                    FaceUVToWorldDir(face, (static_cast<float>(cx) + 0.5f) / g,
                                     (static_cast<float>(cy) + 0.5f) / g, cxd, cyd, czd);
                    cell.Center = {cxd, cyd, czd};
                    float half = 0.0f;
                    for (uint32_t corner = 0; corner < 4u; ++corner)
                    {
                        float dx, dy, dz;
                        FaceUVToWorldDir(face,
                                         (static_cast<float>(cx) + static_cast<float>(corner & 1u)) / g,
                                         (static_cast<float>(cy) + static_cast<float>(corner >> 1u)) / g,
                                         dx, dy, dz);
                        const float d = dx * cxd + dy * cyd + dz * czd;
                        half = std::fmax(half,
                                         std::acos(std::fmin(std::fmax(d, -1.0f), 1.0f)));
                    }
                    cell.Cos = std::cos(half + kMarginRad);
                    cell.Sin = std::sin(half + kMarginRad);
                }
        return cells;
    }();

    // Per-placement support cone: cos/sin of the support's angular radius about N.
    struct SupportCone
    {
        std::array<float, 3> N{};
        float Cos = 1.0f, Sin = 0.0f;
        bool Valid = false;
    };
    std::array<SupportCone, kMaxSphereAnalyticModifiers> cones{};
    const uint32_t total = std::min(flattenCount + dabCount, kMaxSphereAnalyticModifiers);
    for (uint32_t i = 0; i < total; ++i)
    {
        SupportCone& c = cones[i];
        if (i < flattenCount)
        {
            const SphereAnalyticFlatten& f = flattens[i];
            if (!f.Valid || f.PlanetRadius <= 0.0f)
                continue;
            const float reach = (f.Radius + std::fmax(f.Falloff, 0.0f)) / f.PlanetRadius;
            // Support = { near hemisphere, R*sin(ang) <= radius+falloff } -> ang <= asin(reach),
            // or the whole near hemisphere when the reach exceeds the planet.
            const float ang = reach >= 1.0f ? 0.5f * 3.14159265f
                                            : std::asin(std::fmax(reach, 0.0f));
            c.N = f.N;
            c.Cos = std::cos(ang);
            c.Sin = std::sin(ang);
            c.Valid = true;
        }
        else
        {
            const SphereAnalyticDab& d = dabs[i - flattenCount];
            if (!d.Valid)
                continue;
            const float ang = std::fmin(d.AngularRadius, 3.14159265f);
            c.N = d.N;
            c.Cos = std::cos(ang);
            c.Sin = std::sin(ang);
            c.Valid = true;
        }
    }

    for (uint32_t w = 0; w < kSphereAnalyticCellMaskWords; ++w)
        outWords[w] = 0u;
    for (uint32_t cell = 0; cell < kSphereAnalyticCellCount; ++cell)
    {
        uint32_t mask = 0u;
        const CellGeom& cg = kCells[cell];
        for (uint32_t i = 0; i < total; ++i)
        {
            const SupportCone& c = cones[i];
            if (!c.Valid)
                continue;
            // Cones overlap iff angle(cellCenter, N) <= cellHalf + supportAng, i.e.
            // dot >= cos(cellHalf + supportAng) = cellCos*supCos - cellSin*supSin.
            const float d =
                cg.Center[0] * c.N[0] + cg.Center[1] * c.N[1] + cg.Center[2] * c.N[2];
            if (d >= cg.Cos * c.Cos - cg.Sin * c.Sin)
                mask |= 1u << i;
        }
        outWords[cell >> 1u] |= (cell & 1u) != 0u ? (mask << 16) : mask;
    }
}

} // namespace GameEngine::CBTTerrain
