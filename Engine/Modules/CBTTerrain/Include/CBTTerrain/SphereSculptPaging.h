#pragma once

// SphereSculptPaging.h — the sparse virtual sculpt page-table GEOMETRY (radius -> virtual dim ->
// pages) and the CPU-side bit-lock SAMPLER for it.
// Virtual resolution is radius-scaled from a metres-per-texel budget; physical storage
// is a fixed page pool addressed through a per-face page table. This header owns the arithmetic the
// GPU shaders MUST reproduce bit-for-bit: SculptResolveTexel here is the exact integer twin of
// cbt_sculpt.glsl CBT_SculptResolveTexel, and SampleSculptFaceUV mirrors CBT_SampleSculptFaceUV —
// the endpoint-exact shared-sampling house pattern (cbt_atlas.glsl <-> AtlasHeightSampler). Any edit
// here is edited in lockstep with cbt_sculpt.glsl.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "CBTTerrain/CBTLayout.h"        // kSculptPageDim / kSculptMaxPagesPerFaceAxis / kSculptNoPage / ring
#include "CBTTerrain/CBTSphereFaceMap.h" // WorldDirToFaceUV / SphereFaceUV

namespace GameEngine::CBTTerrain
{

// Quality budget: target metres per virtual sculpt texel. A cube face spans a quarter turn
// (pi/2 rad), so Dv = (pi/2 * R) / metersPerTexel; 8 m/texel makes a default ~128 m brush cover
// ~32 texels at ANY radius — the fix for the invisible sub-texel brush on large planets. This is
// the documented headline target (PR budget table): m/texel is radius-INDEPENDENT quality.
inline constexpr float kSculptMetersPerTexelTarget = 8.0f;

// Default physical page pool memory budget. This is host-visible SYSTEM memory for the ring (Upload
// buffers live in system RAM on discrete GPUs, not VRAM). DeriveSculptPagePoolCount fits the page
// count under it: 64 MiB / (ring 4 * 64 KiB/page) = 256 pages (2.1 M texels ~= 268 km^2 of authored
// 8 m/texel terrain at ANY radius). GE_SCULPT_PAGES overrides the derived count outright.
inline constexpr uint64_t kSculptPagePoolBudgetBytes = 64ull * 1024ull * 1024ull;

inline constexpr float kSculptHalfPi = 1.57079632679489661923f; // a cube face spans pi/2 rad

// Resolved per-planet page geometry. VirtualDim (Dv) is a multiple of kSculptPageDim so pages tile a
// face exactly; PagesPerAxis = Dv / kSculptPageDim. Cap is the fixed page-table row stride
// (kSculptMaxPagesPerFaceAxis) both CPU and GPU index with — independent of PagesPerAxis so the
// shader needs only Cap for addressing. PoolPageCount is the physical pool size (page ids 0..N-1).
struct SphereSculptGeometry
{
    uint32_t VirtualDim = kSculptPageDim;
    uint32_t PagesPerAxis = 1u;
    uint32_t Cap = kSculptMaxPagesPerFaceAxis;
    uint32_t PoolPageCount = 0u;
};

// metres per virtual texel for a radius + dim (pi/2 * R / Dv). The radius-scaling oracle asserts
// this stays <= the target budget at R = 2k/20k/50k.
inline float SculptMetersPerTexel(float radius, uint32_t virtualDim)
{
    return virtualDim > 0u ? (kSculptHalfPi * radius) / static_cast<float>(virtualDim) : 0.0f;
}

// Finite-difference step (radians) for the sculpt gradient in the shading normal: one virtual texel
// of arc at a face centre (a face spans pi/2 rad across Dv texels). The GPU twin is
// cbt_surface.glsl's `sculptStep = CBT_SCULPT_HALF_PI / Dv`, and the shading-normal oracle passes
// THIS so it validates the exact step the fragment uses. A full-texel step keeps the central
// difference inside one bilinear cell instead of the sub-texel 1/Dv step that creased at cell edges.
inline float SculptNormalAngularStep(uint32_t virtualDim)
{
    return virtualDim > 0u ? kSculptHalfPi / static_cast<float>(virtualDim) : 0.0f;
}

// Derive the virtual dim (a multiple of kSculptPageDim) for a planet radius at a metres/texel
// budget. pagesPerAxis is clamped to [1, kSculptMaxPagesPerFaceAxis]; beyond the cap the budget
// degrades gracefully (coarser m/texel) instead of overflowing the fixed page table. Rounding Dv
// UP to a whole page count only makes m/texel finer, so the delivered quality still meets the target.
inline uint32_t DeriveSculptVirtualDim(float radius, float metersPerTexel = kSculptMetersPerTexelTarget)
{
    const float mpt = metersPerTexel > 0.0f ? metersPerTexel : kSculptMetersPerTexelTarget;
    const float rawDv = (kSculptHalfPi * std::max(radius, 0.0f)) / mpt;
    uint32_t pagesPerAxis =
        static_cast<uint32_t>(std::ceil(rawDv / static_cast<float>(kSculptPageDim)));
    pagesPerAxis = std::clamp(pagesPerAxis, 1u, kSculptMaxPagesPerFaceAxis);
    return pagesPerAxis * kSculptPageDim;
}

// Physical page count under a memory budget (mirror DeriveAtlasSlotCount): the largest N with
// ring * N * pageBytes <= budget. >= 1, and capped to the page-id field of the table entry
// (the top byte carries the S4 page level). The caller applies any GE_SCULPT_PAGES override.
inline uint32_t DeriveSculptPagePoolCount(uint64_t budgetBytes)
{
    const uint64_t pageBytes = static_cast<uint64_t>(kSculptPageTexels) * sizeof(float);
    const uint64_t ringBytes = static_cast<uint64_t>(kCBTFrameParamsRing) * pageBytes;
    const uint64_t n = budgetBytes / std::max<uint64_t>(1ull, ringBytes);
    return static_cast<uint32_t>(
        std::min<uint64_t>(std::max<uint64_t>(1ull, n), kSculptPageIdMask));
}

// ---- Adaptive-level escalation policy (S4) --------------------------------------------------

// A dab is comfortably representable when its footprint (diameter) covers at least this many
// texels at the resolution it writes — below it the smoothstep cone degenerates toward the
// measured vanish/blob outcomes (S1 §2.2).
inline constexpr float kSculptMinDabFootprintTexels = 4.0f;

// The page level a dab of `angularRadius` wants: 0 for a footprint already representable at the
// base grid (escalation is triggered ONLY by sub-representable brushes — the dark-ship contract),
// else the smallest level that lifts the footprint to kSculptMinDabFootprintTexels, capped at
// `maxLevel`. A brush that stays sub-TEXEL even at maxLevel (footprint * 2^maxLevel < 1 — the
// few-metre-at-Earth regime under the 128-page Dv cap) returns 0: escalating would burn 4^L pool
// slots without making the shape representable, so the store keeps today's behaviour (and the
// sub-texel warn) instead of paying for nothing.
inline uint32_t DesiredSculptDabLevel(float angularRadius, uint32_t virtualDim, uint32_t maxLevel)
{
    const float footprint =
        (2.0f * std::max(angularRadius, 0.0f) / kSculptHalfPi) * static_cast<float>(virtualDim);
    if (footprint >= kSculptMinDabFootprintTexels)
        return 0u;
    uint32_t level = 0u;
    while (level < maxLevel &&
           footprint * static_cast<float>(1u << level) < kSculptMinDabFootprintTexels)
        ++level;
    if (footprint * static_cast<float>(1u << level) < 1.0f)
        return 0u;
    return level;
}

// Fill a SphereSculptGeometry from a radius (+ resolved pool count). One chokepoint so every
// consumer (store, render feature GPU params, physics) derives the SAME Dv/pages from the radius.
inline SphereSculptGeometry MakeSphereSculptGeometry(float radius, uint32_t poolPageCount,
                                                     float metersPerTexel = kSculptMetersPerTexelTarget)
{
    SphereSculptGeometry g{};
    g.VirtualDim = DeriveSculptVirtualDim(radius, metersPerTexel);
    g.PagesPerAxis = g.VirtualDim / kSculptPageDim;
    g.Cap = kSculptMaxPagesPerFaceAxis;
    g.PoolPageCount = poolPageCount;
    return g;
}

// SoA view of the CPU page store for the shared sampler. Pool is the published-height page pool
// (PoolPageCount * kSculptPageTexels floats, dab + modifier composed); PageTable is
// kSculptPageTableEntries uint entries (kSculptNoPage marks an unallocated virtual page).
struct SphereSculptSampler
{
    const float* Pool = nullptr;
    const uint32_t* PageTable = nullptr;
    SphereSculptGeometry Geom{};

    bool Valid() const { return Pool != nullptr && PageTable != nullptr && Geom.PoolPageCount > 0u; }
};

// ---- Adaptive page levels (sculpt shape-accuracy S4, design §3.b) --------------------------
//
// A page-table entry packs a resolution LEVEL into its top byte (CBTLayout.h kSculptPageIdMask /
// kSculptPageLevelShift): a level-L page hosts a (127*2^L + 1)^2 fine grid over the SAME angular
// rect its 128 base texels cover, endpoint-aligned (fine texel j sits at base position
// bx + j/2^L, so j = 0 and j = 127*2^L coincide with the page's first/last base texels). The
// block occupies 4^L contiguous pool slots starting at the entry's page id. Level 0 is the
// pre-S4 layout bit-for-bit (fine dim 128 == kSculptPageDim).

inline uint32_t SculptEntryPageId(uint32_t entry)
{
    return entry & kSculptPageIdMask;
}

// Level of an entry; the caller must have rejected kSculptNoPage first (the sentinel's top byte
// is not a level).
inline uint32_t SculptEntryLevel(uint32_t entry)
{
    return entry >> kSculptPageLevelShift;
}

inline uint32_t SculptFineDim(uint32_t level)
{
    return 127u * (1u << level) + 1u;
}

// Fine texel (jx, jy) of the block `entry` decodes to. At level 0 the index expression reduces
// exactly to the pre-S4 pool index (page * texels + y * 128 + x) — the dark-ship byte-lock.
// Mirror of cbt_sculpt.glsl CBT_SculptPoolIndexFine.
inline float SculptFineTexel(const float* pool, uint32_t entry, uint32_t jx, uint32_t jy)
{
    const uint32_t fdim = SculptFineDim(SculptEntryLevel(entry));
    return pool[static_cast<size_t>(SculptEntryPageId(entry)) * kSculptPageTexels +
                static_cast<size_t>(jy) * fdim + jx];
}

// Resolve one virtual texel (face, integer vtx/vty already clamped to [0, Dv-1]) to its additive
// height in metres. pageX/pageY select the page; an unallocated page (kSculptNoPage) reads 0 — the
// "no edits" fast path. On an escalated page the base-aligned fine texel (local << level) is read —
// base positions exist at every level by endpoint alignment. This is the EXACT integer arithmetic
// cbt_sculpt.glsl / cbt_layout.glsl CBT_SculptResolveTexel performs; the two must stay
// bit-identical (the CPU/GPU bit-lock).
inline float SculptResolveTexel(const SphereSculptSampler& s, uint32_t face, uint32_t vtx, uint32_t vty)
{
    const uint32_t pageX = vtx / kSculptPageDim;
    const uint32_t pageY = vty / kSculptPageDim;
    const uint32_t entry =
        s.PageTable[face * kSculptPageTableFaceStride + pageY * s.Geom.Cap + pageX];
    if (entry == kSculptNoPage)
        return 0.0f;
    const uint32_t level = SculptEntryLevel(entry);
    const uint32_t localX = vtx - pageX * kSculptPageDim;
    const uint32_t localY = vty - pageY * kSculptPageDim;
    return SculptFineTexel(s.Pool, entry, localX << level, localY << level);
}

// Piecewise-linear evaluation of one page's edge COLUMN (fixed fine jx) at page-local base-texel
// position tLocal in [0, 127], at the page's OWN level — the seam-stitch primitive: a boundary
// cell interpolates between the two flanking pages' edge functions, each evaluated at its own
// resolution, which is what keeps the composed height C0 across mixed-level page seams (the S4
// seam oracle). An unallocated entry evaluates to 0 (the additive-zero contract).
inline float SculptEdgeColEval(const float* pool, uint32_t entry, uint32_t jx, float tLocal)
{
    if (entry == kSculptNoPage)
        return 0.0f;
    const uint32_t step = 1u << SculptEntryLevel(entry);
    const uint32_t jmax = 127u * step;
    const float sv = tLocal * static_cast<float>(step);
    uint32_t k0 = static_cast<uint32_t>(sv); // sv >= 0
    if (k0 > jmax)
        k0 = jmax;
    const uint32_t k1 = k0 + 1u <= jmax ? k0 + 1u : jmax;
    const float g = sv - static_cast<float>(k0);
    const float a = SculptFineTexel(pool, entry, jx, k0);
    const float b = SculptFineTexel(pool, entry, jx, k1);
    return a + (b - a) * g;
}

// Row twin (fixed fine jy, u varying).
inline float SculptEdgeRowEval(const float* pool, uint32_t entry, uint32_t jy, float tLocal)
{
    if (entry == kSculptNoPage)
        return 0.0f;
    const uint32_t step = 1u << SculptEntryLevel(entry);
    const uint32_t jmax = 127u * step;
    const float su = tLocal * static_cast<float>(step);
    uint32_t k0 = static_cast<uint32_t>(su);
    if (k0 > jmax)
        k0 = jmax;
    const uint32_t k1 = k0 + 1u <= jmax ? k0 + 1u : jmax;
    const float g = su - static_cast<float>(k0);
    const float a = SculptFineTexel(pool, entry, k0, jy);
    const float b = SculptFineTexel(pool, entry, k1, jy);
    return a + (b - a) * g;
}

// Level-aware paged bilinear at face-local uv, generic over the page store (`entryAt(face,px,py)`
// -> table entry, `pool` -> the published float pool). The pool chokepoint below instantiates it
// for SphereSculptSampler; SphereSculptLayer's resize remap instantiates it per AUTHORING layer
// (dab / modifier kept separate through a resample). Structure (mirrored in cbt_layout.glsl
// CBT_SampleSphereSculpt and cbt_surface.glsl CBT_SampleSculptDirSurf — the CPU/GPU bit-lock):
//
//   * all four tap pages at level 0  -> the pre-S4 four-tap bilinear, bit-identical (dark-ship:
//     a store with no escalated pages samples the exact pre-S4 bytes AND arithmetic);
//   * cell interior to ONE page      -> fine bilinear at that page's level (the shape payoff);
//   * cell straddling a page seam    -> lerp across the base cell between the two pages' edge
//     functions, each evaluated at its OWN level (C0 with both neighbours' interiors: at the
//     cell borders the lerp degenerates to exactly the flanking page's own edge evaluation);
//   * four-page corner cell          -> plain bilinear of the four corner texels (single points
//     at any level, endpoint-aligned), C0 with the adjacent seam cells by the same argument.
//
// Endpoint-exact per level: uv in {0,1} lands on texel {0, Dv-1}, so a shared cube-edge direction
// samples the identical value from both faces (crack-free at level 0; escalation of face-border
// pages is refused by the store — see SphereSculptLayer — so cross-face columns stay base-level).
template <typename EntryAt>
inline float SampleSculptFaceUVWith(EntryAt&& entryAt, const float* pool,
                                    const SphereSculptGeometry& geom, uint32_t face, float u,
                                    float v)
{
    const float dim = static_cast<float>(geom.VirtualDim);
    const float tu = std::min(std::max(u, 0.0f), 1.0f) * (dim - 1.0f);
    const float tv = std::min(std::max(v, 0.0f), 1.0f) * (dim - 1.0f);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(tu));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(tv));
    const uint32_t x1 = x0 + 1u < geom.VirtualDim ? x0 + 1u : geom.VirtualDim - 1u;
    const uint32_t y1 = y0 + 1u < geom.VirtualDim ? y0 + 1u : geom.VirtualDim - 1u;
    const float fx = tu - static_cast<float>(x0);
    const float fy = tv - static_cast<float>(y0);
    const uint32_t px0 = x0 / kSculptPageDim;
    const uint32_t py0 = y0 / kSculptPageDim;
    const uint32_t px1 = x1 / kSculptPageDim;
    const uint32_t py1 = y1 / kSculptPageDim;
    const uint32_t e00 = entryAt(face, px0, py0);
    const uint32_t e10 = entryAt(face, px1, py0);
    const uint32_t e01 = entryAt(face, px0, py1);
    const uint32_t e11 = entryAt(face, px1, py1);
    const uint32_t lv00 = e00 == kSculptNoPage ? 0u : SculptEntryLevel(e00);
    const uint32_t lv10 = e10 == kSculptNoPage ? 0u : SculptEntryLevel(e10);
    const uint32_t lv01 = e01 == kSculptNoPage ? 0u : SculptEntryLevel(e01);
    const uint32_t lv11 = e11 == kSculptNoPage ? 0u : SculptEntryLevel(e11);
    if ((lv00 | lv10 | lv01 | lv11) == 0u)
    {
        // Pre-S4 four-tap bilinear, expression-identical (the level-0 byte-lock).
        const float s00 = e00 == kSculptNoPage
                              ? 0.0f
                              : SculptFineTexel(pool, e00, x0 - px0 * kSculptPageDim,
                                                y0 - py0 * kSculptPageDim);
        const float s10 = e10 == kSculptNoPage
                              ? 0.0f
                              : SculptFineTexel(pool, e10, x1 - px1 * kSculptPageDim,
                                                y0 - py0 * kSculptPageDim);
        const float s01 = e01 == kSculptNoPage
                              ? 0.0f
                              : SculptFineTexel(pool, e01, x0 - px0 * kSculptPageDim,
                                                y1 - py1 * kSculptPageDim);
        const float s11 = e11 == kSculptNoPage
                              ? 0.0f
                              : SculptFineTexel(pool, e11, x1 - px1 * kSculptPageDim,
                                                y1 - py1 * kSculptPageDim);
        const float a = s00 + (s10 - s00) * fx;
        const float b = s01 + (s11 - s01) * fx;
        return a + (b - a) * fy;
    }
    if (px0 == px1 && py0 == py1)
    {
        // Interior cell of one (escalated) page: fine bilinear at the page's own level. The
        // page is allocated (an unallocated page has level 0, which the fast path caught).
        const uint32_t step = 1u << lv00;
        const uint32_t jmax = 127u * step;
        const float sx = (tu - static_cast<float>(px0 * kSculptPageDim)) * static_cast<float>(step);
        const float sy = (tv - static_cast<float>(py0 * kSculptPageDim)) * static_cast<float>(step);
        uint32_t jx0 = static_cast<uint32_t>(sx);
        uint32_t jy0 = static_cast<uint32_t>(sy);
        if (jx0 > jmax)
            jx0 = jmax;
        if (jy0 > jmax)
            jy0 = jmax;
        const uint32_t jx1 = jx0 + 1u <= jmax ? jx0 + 1u : jmax;
        const uint32_t jy1 = jy0 + 1u <= jmax ? jy0 + 1u : jmax;
        const float gx = sx - static_cast<float>(jx0);
        const float gy = sy - static_cast<float>(jy0);
        const float s00 = SculptFineTexel(pool, e00, jx0, jy0);
        const float s10 = SculptFineTexel(pool, e00, jx1, jy0);
        const float s01 = SculptFineTexel(pool, e00, jx0, jy1);
        const float s11 = SculptFineTexel(pool, e00, jx1, jy1);
        const float a = s00 + (s10 - s00) * gx;
        const float b = s01 + (s11 - s01) * gx;
        return a + (b - a) * gy;
    }
    if (py0 == py1)
    {
        // Vertical seam: x0 is the left page's last base column, x1 the right page's first.
        const float tLoc = tv - static_cast<float>(py0 * kSculptPageDim);
        const float colL =
            SculptEdgeColEval(pool, e00, e00 == kSculptNoPage ? 0u : 127u * (1u << lv00), tLoc);
        const float colR = SculptEdgeColEval(pool, e10, 0u, tLoc);
        return colL + (colR - colL) * fx;
    }
    if (px0 == px1)
    {
        // Horizontal seam: y0 is the bottom page's last base row, y1 the top page's first.
        const float tLoc = tu - static_cast<float>(px0 * kSculptPageDim);
        const float rowB =
            SculptEdgeRowEval(pool, e00, e00 == kSculptNoPage ? 0u : 127u * (1u << lv00), tLoc);
        const float rowT = SculptEdgeRowEval(pool, e01, 0u, tLoc);
        return rowB + (rowT - rowB) * fy;
    }
    // Four-page corner cell: each page contributes its corner texel adjacent to the cell.
    const float c00 =
        e00 == kSculptNoPage ? 0.0f : SculptFineTexel(pool, e00, 127u * (1u << lv00), 127u * (1u << lv00));
    const float c10 = e10 == kSculptNoPage ? 0.0f : SculptFineTexel(pool, e10, 0u, 127u * (1u << lv10));
    const float c01 = e01 == kSculptNoPage ? 0.0f : SculptFineTexel(pool, e01, 127u * (1u << lv01), 0u);
    const float c11 = e11 == kSculptNoPage ? 0.0f : SculptFineTexel(pool, e11, 0u, 0u);
    const float a = c00 + (c10 - c00) * fx;
    const float b = c01 + (c11 - c01) * fx;
    return a + (b - a) * fy;
}

// Paged bilinear read of a face at face-local uv — the SAME bilinear as the flat sampler, only each
// texel fetch goes through the page table (level-aware since S4; see SampleSculptFaceUVWith).
// Endpoint-exact: uv in {0,1} lands on texel {0, Dv-1}, so a shared cube-edge direction samples the
// identical value from both faces (crack-free). Mirror of cbt_layout.glsl CBT_SampleSphereSculpt.
inline float SampleSculptFaceUV(const SphereSculptSampler& s, uint32_t face, float u, float v)
{
    if (!s.Valid())
        return 0.0f;
    return SampleSculptFaceUVWith(
        [&s](uint32_t f, uint32_t px, uint32_t py)
        { return s.PageTable[f * kSculptPageTableFaceStride + py * s.Geom.Cap + px]; },
        s.Pool, s.Geom, face, u, v);
}

// By-direction sampler (physics colliders, brush cursor, CPU analytic normal). dir need not be
// unit; the planet is centred at the world origin. Mirror of cbt_surface.glsl CBT_SampleSculptDirSurf.
inline float SampleSphereSculptByDir(const SphereSculptSampler& s, float dx, float dy, float dz)
{
    if (!s.Valid())
        return 0.0f;
    const SphereFaceUV f = WorldDirToFaceUV(dx, dy, dz);
    return SampleSculptFaceUV(s, f.Face, f.U, f.V);
}

} // namespace GameEngine::CBTTerrain
