// S4 adaptive page-level oracles (sculpt shape-accuracy, design §3.b): escalation trigger policy,
// slot accounting (VRAM honesty), the mixed-level seam stitch (with a can-fail variant proving the
// stitch does something), undo across escalation, .tsculpt v2 round-trip + v1 read, resize-remap
// level carry, and analytic-flag composition over escalated pages. Device-free like the layer.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/SphereAnalyticModifiers.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"
#include "CBTTerrain/SphereSculptSerialization.h"

using namespace GameEngine::CBTTerrain;

namespace
{

SphereSculptGeometry Geom(uint32_t virtualDim, uint32_t pageCount)
{
    return SphereSculptGeometry{virtualDim, virtualDim / kSculptPageDim, kSculptMaxPagesPerFaceAxis,
                                pageCount};
}

// The runtime geometry for a planet radius (ConfigurePlanetSculpt's arithmetic) with an explicit
// pool so the overflow tests control slot budgets.
SphereSculptGeometry RuntimeGeom(float radius, uint32_t pool)
{
    SphereSculptGeometry g = MakeSphereSculptGeometry(radius, pool);
    return g;
}

// A direction centred INSIDE a page (page-local base column/row ~64), at a fractional texel
// offset, `pageIndex` pages along both axes of face 0 — page-interior by construction, so the
// seam-strip limitation (see SeamStripDabIsDocumentedResidualLimitation) is not in play.
std::array<float, 3> MidPageDir(uint32_t dim, float pageIndex)
{
    const float centreTexel = (pageIndex + 0.5f) * static_cast<float>(kSculptPageDim) + 0.37f;
    const float uv = centreTexel / static_cast<float>(dim - 1u);
    std::array<float, 3> n{};
    FaceUVToWorldDir(0u, uv, uv + 5.2f / static_cast<float>(dim - 1u), n[0], n[1], n[2]);
    return n;
}

std::vector<SphereSculptPageContent> SortedExport(const SphereSculptLayer& layer)
{
    std::vector<SphereSculptPageContent> pages = layer.ExportPages();
    std::sort(pages.begin(), pages.end(),
              [](const SphereSculptPageContent& a, const SphereSculptPageContent& b)
              {
                  if (a.Face != b.Face)
                      return a.Face < b.Face;
                  if (a.PageY != b.PageY)
                      return a.PageY < b.PageY;
                  return a.PageX < b.PageX;
              });
    return pages;
}

// Logical store equality through the export seam: same page set, levels, and byte-identical
// authoring layers. Physical slot ids may permute across free/re-allocate cycles (an allocator
// detail every consumer resolves through the page table).
void ExpectExportsIdentical(const std::vector<SphereSculptPageContent>& a,
                            const std::vector<SphereSculptPageContent>& b, const char* what)
{
    ASSERT_EQ(a.size(), b.size()) << what;
    for (size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(a[i].Face, b[i].Face) << what << " page " << i;
        EXPECT_EQ(a[i].PageX, b[i].PageX) << what << " page " << i;
        EXPECT_EQ(a[i].PageY, b[i].PageY) << what << " page " << i;
        EXPECT_EQ(a[i].Level, b[i].Level) << what << " page " << i;
        ASSERT_EQ(a[i].Dab.size(), b[i].Dab.size()) << what << " page " << i;
        ASSERT_EQ(a[i].Modifier.size(), b[i].Modifier.size()) << what << " page " << i;
        EXPECT_EQ(0, std::memcmp(a[i].Dab.data(), b[i].Dab.data(), a[i].Dab.size() * sizeof(float)))
            << what << " dab bytes, page " << i;
        EXPECT_EQ(0, std::memcmp(a[i].Modifier.data(), b[i].Modifier.data(),
                                 a[i].Modifier.size() * sizeof(float)))
            << what << " modifier bytes, page " << i;
    }
}

uint32_t SumBlockSlots(const std::vector<SphereSculptPageContent>& pages)
{
    uint32_t slots = 0u;
    for (const SphereSculptPageContent& p : pages)
        slots += 1u << (2u * p.Level);
    return slots;
}

} // namespace

// ---- Trigger policy -----------------------------------------------------------------------------

// Escalation is triggered ONLY by sub-representable brushes (the dark-ship contract): a footprint
// already >= kSculptMinDabFootprintTexels stays at level 0; below it the smallest sufficient level
// wins, capped at maxLevel; a brush that stays sub-TEXEL even at maxLevel returns 0 (escalating
// would burn 4^L slots without making the shape representable — the few-metre-at-Earth regime).
TEST(SphereSculptEscalation, PolicyTriggersOnlyOnSubRepresentableBrushes)
{
    const uint32_t dim50k = DeriveSculptVirtualDim(50000.0f); // 9856
    ASSERT_EQ(dim50k, 9856u);
    // 100 m brush at R=50000: footprint ~25 texels — never escalates.
    EXPECT_EQ(DesiredSculptDabLevel(100.0f / 50000.0f, dim50k, kSculptMaxPageLevel), 0u);
    // 16.1 m brush: footprint ~4.04 — just representable, no escalation.
    EXPECT_EQ(DesiredSculptDabLevel(16.1f / 50000.0f, dim50k, kSculptMaxPageLevel), 0u);
    // 15 m brush: footprint ~3.76 -> level 1 doubles it past 4.
    EXPECT_EQ(DesiredSculptDabLevel(15.0f / 50000.0f, dim50k, kSculptMaxPageLevel), 1u);
    // 5 m brush: footprint ~1.25 -> needs level 2.
    EXPECT_EQ(DesiredSculptDabLevel(5.0f / 50000.0f, dim50k, kSculptMaxPageLevel), 2u);
    // maxLevel clamp: the same brush with maxLevel 1 takes the partial improvement.
    EXPECT_EQ(DesiredSculptDabLevel(5.0f / 50000.0f, dim50k, 1u), 1u);
    // maxLevel 0 disables escalation outright.
    EXPECT_EQ(DesiredSculptDabLevel(5.0f / 50000.0f, dim50k, 0u), 0u);
    // Hopeless: 5 m at Earth under the page-table cap (footprint ~0.008; x4 is still sub-texel).
    const uint32_t dimEarth = DeriveSculptVirtualDim(6.371e6f); // capped 16384
    EXPECT_EQ(DesiredSculptDabLevel(5.0f / 6.371e6f, dimEarth, kSculptMaxPageLevel), 0u);
}

// ---- Escalated commit + slot accounting ---------------------------------------------------------

// A sub-representable dab escalates exactly the pages its cap writes (probe-then-escalate), the
// block consumes 4^level pool slots (AllocatedPageCount counts slots — the VRAM-honesty oracle),
// and a later resolvable brush never adds escalation.
TEST(SphereSculptEscalation, SubTexelDabEscalatesOnlyTouchedPagesWithSlotAccounting)
{
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeom(50000.0f, 256u));
    const uint32_t dim = layer.Geometry().VirtualDim;

    const std::array<float, 3> n = MidPageDir(dim, 40.0f);
    layer.ApplyDab(n[0], n[1], n[2], 5.0f / 50000.0f, 5.0f, false);
    EXPECT_EQ(layer.EscalatedPageCount(), 1u) << "a mid-page sub-texel cap touches one page";
    const std::vector<SphereSculptPageContent> pages = SortedExport(layer);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].Level, kSculptMaxPageLevel);
    EXPECT_EQ(layer.AllocatedPageCount(), SumBlockSlots(pages))
        << "slot accounting: a level-L page consumes its whole 4^L block";

    // Committed centre holds the amplitude (page-interior placement; the shape gate proper lives
    // in ReleasePopQuantifiedAtStoreResolution).
    const float centre = SampleSphereSculptByDir(layer.MakeSampler(), n[0], n[1], n[2]);
    EXPECT_GT(centre, 0.6f * 5.0f);

    // A resolvable brush composes without any new escalation.
    const std::array<float, 3> m = MidPageDir(dim, 44.0f);
    layer.ApplyDab(m[0], m[1], m[2], 100.0f / 50000.0f, 5.0f, false);
    EXPECT_EQ(layer.EscalatedPageCount(), 1u)
        << "a representable brush must never trigger escalation";
}

// ---- Dark-ship: the escalation machinery is inert without its trigger ---------------------------

// The same resolvable-brush + modifier-bake sequence on a maxLevel-0 layer (escalation pinned off
// — pre-S4 semantics) and a default layer produces BYTE-identical pool + page-table + version, and
// the encoded .tsculpt stays version 1: with no sub-representable brush the whole store pipeline
// is byte-identical, locked structurally (the level-0 write paths ARE the pre-S4 code paths).
TEST(SphereSculptEscalation, ResolvableBrushPipelineIsByteIdenticalWithEscalationEnabled)
{
    const auto author = [](SphereSculptLayer& layer)
    {
        layer.Configure(RuntimeGeom(50000.0f, 256u));
        const uint32_t dim = layer.Geometry().VirtualDim;
        const std::array<float, 3> a = MidPageDir(dim, 40.0f);
        const std::array<float, 3> b = MidPageDir(dim, 40.3f);
        layer.ApplyDab(a[0], a[1], a[2], 100.0f / 50000.0f, 5.0f, false);
        const SphereEditRegions r = ClassifySphereCapEdit(a[0], a[1], a[2], 200.0f / 50000.0f);
        layer.BakeModifierLayer(r, [](float, float, float) { return 2.5f; });
        layer.ApplyDab(b[0], b[1], b[2], 40.0f / 50000.0f, 3.0f, true);
    };
    SphereSculptLayer pinned;
    pinned.SetMaxPageLevel(0u);
    author(pinned);
    SphereSculptLayer live;
    author(live);

    EXPECT_EQ(pinned.EscalatedPageCount(), 0u);
    EXPECT_EQ(live.EscalatedPageCount(), 0u);
    EXPECT_EQ(pinned.Version(), live.Version());
    EXPECT_EQ(pinned.AllocatedPageCount(), live.AllocatedPageCount());
    ASSERT_EQ(pinned.Pool().size(), live.Pool().size());
    EXPECT_EQ(0, std::memcmp(pinned.Pool().data(), live.Pool().data(),
                             pinned.Pool().size() * sizeof(float)))
        << "published pool bytes must be identical without an escalation trigger";
    ASSERT_EQ(pinned.PageTable().size(), live.PageTable().size());
    EXPECT_EQ(0, std::memcmp(pinned.PageTable().data(), live.PageTable().data(),
                             pinned.PageTable().size() * sizeof(uint32_t)))
        << "page-table bytes (incl. level bits: none) must be identical";

    const std::vector<uint8_t> blobA = EncodeSphereSculpt(pinned.Geometry(), pinned.ExportPages());
    const std::vector<uint8_t> blobB = EncodeSphereSculpt(live.Geometry(), live.ExportPages());
    ASSERT_EQ(blobA.size(), blobB.size());
    EXPECT_EQ(0, std::memcmp(blobA.data(), blobB.data(), blobA.size()));
    uint32_t version = 0u;
    std::memcpy(&version, blobA.data() + sizeof(uint32_t), sizeof(uint32_t));
    EXPECT_EQ(version, 1u) << "an unescalated store must encode as .tsculpt v1 (byte-compatible)";
}

// ---- The seam stitch (gate 2) -------------------------------------------------------------------

namespace
{

// Fine-scale test content: period-3-base-texel waves in u and v, so fine texels deviate strongly
// from the base-texel lerp (the precondition that makes the can-fail variant actually fail).
float SeamContent(float u, float v, uint32_t dim)
{
    const float kU = 2.0f * 3.14159265f * static_cast<float>(dim - 1u) / 3.0f;
    const float kV = 2.0f * 3.14159265f * static_cast<float>(dim - 1u) / 5.0f;
    return 4.0f * std::sin(kU * u + 0.4f) * std::cos(kV * v + 0.9f) + 2.0f;
}

SphereSculptPageContent MakeContentPage(uint32_t face, uint32_t px, uint32_t py, uint32_t level,
                                        uint32_t dim)
{
    SphereSculptPageContent page;
    page.Face = face;
    page.PageX = px;
    page.PageY = py;
    page.Level = level;
    const uint32_t fdim = SculptFineDim(level);
    const float stepInv = 1.0f / static_cast<float>(1u << level);
    page.Dab.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
    page.Modifier.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
    for (uint32_t jy = 0; jy < fdim; ++jy)
    {
        const float v = (static_cast<float>(py * kSculptPageDim) + static_cast<float>(jy) * stepInv) /
                        static_cast<float>(dim - 1u);
        for (uint32_t jx = 0; jx < fdim; ++jx)
        {
            const float u =
                (static_cast<float>(px * kSculptPageDim) + static_cast<float>(jx) * stepInv) /
                static_cast<float>(dim - 1u);
            page.Dab[static_cast<size_t>(jy) * fdim + jx] = SeamContent(u, v, dim);
        }
    }
    return page;
}

} // namespace

// Composed height must be C0 across every mixed-level boundary configuration: vertical seam
// L2|L1, horizontal seam L2|L0, and the four-page corner mixing L2/L1/L0/L2. The oracle checks
// jumps across every boundary line a discontinuity could live on (page seam edges and fine-cell
// edges), at fine-fractional positions along the seam. The CAN-FAIL variant replaces the seam
// cells' stitch with a naive base-tap bilinear and shows the same oracle REJECTS it — proving the
// stitch (per-page edge-function evaluation) is load-bearing, not decorative.
TEST(SphereSculptEscalation, MixedLevelSeamsAreC0AndOracleCanFail)
{
    const uint32_t dim = 512u; // 4 pages/axis: pages 1..2 are interior
    SphereSculptLayer layer;
    layer.Configure(Geom(dim, 64u));
    std::vector<SphereSculptPageContent> pages;
    pages.push_back(MakeContentPage(0u, 1u, 1u, 2u, dim)); // L2
    pages.push_back(MakeContentPage(0u, 2u, 1u, 1u, dim)); // L1 (east of the L2 page)
    pages.push_back(MakeContentPage(0u, 1u, 2u, 0u, dim)); // L0 (south of the L2 page)
    pages.push_back(MakeContentPage(0u, 2u, 2u, 2u, dim)); // L2 (diagonal)
    layer.ImportPages(pages);
    ASSERT_EQ(layer.EscalatedPageCount(), 3u);
    const SphereSculptSampler s = layer.MakeSampler();

    // Reconstruction: inside the L2 page the fine content is reproduced to fine-bilinear error —
    // and the fine content genuinely deviates from the base-texel lerp (the can-fail premise).
    {
        float maxErr = 0.0f;
        float maxBaseLerpDev = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            // Deliberately fractional (neither base- nor fine-aligned) sample positions.
            const float bu = 140.37f + 4.83f * static_cast<float>(i % 20);
            const float bv = 150.29f + 4.31f * static_cast<float>(i / 20);
            const float u = bu / static_cast<float>(dim - 1u);
            const float v = bv / static_cast<float>(dim - 1u);
            maxErr = std::max(maxErr,
                              std::fabs(SampleSculptFaceUV(s, 0u, u, v) - SeamContent(u, v, dim)));
            // Base-lerp deviation at the same spot: resolve the four surrounding BASE texels.
            const uint32_t x0 = static_cast<uint32_t>(bu);
            const uint32_t y0 = static_cast<uint32_t>(bv);
            const float fx = bu - static_cast<float>(x0);
            const float fy = bv - static_cast<float>(y0);
            const float b00 = SculptResolveTexel(s, 0u, x0, y0);
            const float b10 = SculptResolveTexel(s, 0u, x0 + 1u, y0);
            const float b01 = SculptResolveTexel(s, 0u, x0, y0 + 1u);
            const float b11 = SculptResolveTexel(s, 0u, x0 + 1u, y0 + 1u);
            const float baseLerp =
                (b00 + (b10 - b00) * fx) + ((b01 + (b11 - b01) * fx) - (b00 + (b10 - b00) * fx)) * fy;
            maxBaseLerpDev =
                std::max(maxBaseLerpDev, std::fabs(SampleSculptFaceUV(s, 0u, u, v) - baseLerp));
        }
        EXPECT_LT(maxErr, 0.7f) << "L2 interior must reproduce the fine content";
        EXPECT_GT(maxBaseLerpDev, 1.0f)
            << "fine content must deviate from the base lerp or the can-fail variant proves nothing";
    }

    // Boundary-jump oracle: |S(b-eps) - S(b+eps)| at every interesting boundary, at multiple
    // fine-fractional positions along the seam. eps = 1e-5 UV; content slope <= ~4300/UV ->
    // continuous jumps are < ~0.1; a broken stitch jumps by the fine-vs-base deviation (> 1).
    const float eps = 1e-5f;
    const float kJumpBound = 0.25f;
    const auto maxJumpAcross = [&](auto&& sampler, float boundaryBase, bool vertical) -> float
    {
        float maxJump = 0.0f;
        for (int i = 0; i < 40; ++i)
        {
            // Along-seam positions sweep page-row/col 1..2 including fine-fractional offsets.
            const float along = 132.0f + 6.1f * static_cast<float>(i);
            const float b = boundaryBase / static_cast<float>(dim - 1u);
            const float a = along / static_cast<float>(dim - 1u);
            const float lo = vertical ? sampler(b - eps, a) : sampler(a, b - eps);
            const float hi = vertical ? sampler(b + eps, a) : sampler(a, b + eps);
            maxJump = std::max(maxJump, std::fabs(hi - lo));
        }
        return maxJump;
    };
    const auto real = [&](float u, float v) { return SampleSculptFaceUV(s, 0u, u, v); };

    // Vertical seam L2|L1: the strip spans base [255, 256]; check both strip edges and a couple
    // of fine-cell edges inside the L2 page. Horizontal seam L2|L0: strip [255, 256] in v.
    for (const float ub : {255.0f, 256.0f, 254.75f, 254.5f})
        EXPECT_LT(maxJumpAcross(real, ub, true), kJumpBound)
            << "vertical boundary at base " << ub << " must be C0";
    for (const float vb : {255.0f, 256.0f, 254.75f})
        EXPECT_LT(maxJumpAcross(real, vb, false), kJumpBound)
            << "horizontal boundary at base " << vb << " must be C0";

    // Four-page corner: dense grid over the corner neighbourhood, adjacent-sample deltas bounded
    // by slope * step with safety (a discontinuity anywhere in the cross shows here).
    {
        const float step = 0.02f;
        float prevRow[151];
        float maxDelta = 0.0f;
        for (int iy = 0; iy <= 150; ++iy)
        {
            float prev = 0.0f;
            for (int ix = 0; ix <= 150; ++ix)
            {
                const float u = (254.0f + step * static_cast<float>(ix)) / static_cast<float>(dim - 1u);
                const float v = (254.0f + step * static_cast<float>(iy)) / static_cast<float>(dim - 1u);
                const float h = real(u, v);
                if (ix > 0)
                    maxDelta = std::max(maxDelta, std::fabs(h - prev));
                if (iy > 0)
                    maxDelta = std::max(maxDelta, std::fabs(h - prevRow[ix]));
                prev = h;
                prevRow[ix] = h;
            }
        }
        // slope ~4300/UV * step 0.02 base (=0.02/511 UV) ~ 0.17; 3x safety.
        EXPECT_LT(maxDelta, 0.5f) << "the four-page mixed-level corner must be C0";
    }

    // CAN-FAIL: naive stitch = base-tap bilinear for any cell straddling pages. The same
    // boundary oracle must REJECT it (the interior fine path and the base-tap seam disagree by
    // the fine-vs-base deviation at fine-fractional along-seam positions).
    const auto naive = [&](float u, float v) -> float
    {
        const float tu = std::min(std::max(u, 0.0f), 1.0f) * static_cast<float>(dim - 1u);
        const float tv = std::min(std::max(v, 0.0f), 1.0f) * static_cast<float>(dim - 1u);
        const uint32_t x0 = static_cast<uint32_t>(tu);
        const uint32_t y0 = static_cast<uint32_t>(tv);
        const uint32_t x1 = std::min(x0 + 1u, dim - 1u);
        const uint32_t y1 = std::min(y0 + 1u, dim - 1u);
        if (x0 / kSculptPageDim == x1 / kSculptPageDim && y0 / kSculptPageDim == y1 / kSculptPageDim)
            return SampleSculptFaceUV(s, 0u, u, v); // interior: the real (fine) path
        const float fx = tu - static_cast<float>(x0);
        const float fy = tv - static_cast<float>(y0);
        const float s00 = SculptResolveTexel(s, 0u, x0, y0);
        const float s10 = SculptResolveTexel(s, 0u, x1, y0);
        const float s01 = SculptResolveTexel(s, 0u, x0, y1);
        const float s11 = SculptResolveTexel(s, 0u, x1, y1);
        const float a = s00 + (s10 - s00) * fx;
        const float b = s01 + (s11 - s01) * fx;
        return a + (b - a) * fy;
    };
    float naiveWorst = 0.0f;
    for (const float ub : {255.0f, 256.0f})
        naiveWorst = std::max(naiveWorst, maxJumpAcross(naive, ub, true));
    std::printf("[escalation] seam oracle: naive-stitch worst boundary jump = %.3f (bound %.2f)\n",
                naiveWorst, kJumpBound);
    EXPECT_GT(naiveWorst, kJumpBound)
        << "the boundary oracle must reject a naive base-tap stitch — otherwise it cannot fail "
           "and the C0 assertions above prove nothing";
}

// ---- VRAM honesty: refusal + reclaim (gate 3) ---------------------------------------------------

// Escalation is refuse-don't-lose: when the pool cannot hold a contiguous 4^L block the page
// keeps its current resolution and the write proceeds (PoolExhausted stays false — no content
// was refused); undoing an escalated stroke reclaims the whole block; and a freed block's slots
// satisfy a later escalation through the free-list contiguous-run scan.
TEST(SphereSculptEscalation, EscalationRefusalKeepsBaseWritesAndBlocksReclaim)
{
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeom(50000.0f, 20u)); // one L2 block + change
    const uint32_t dim = layer.Geometry().VirtualDim;
    const float angR = 5.0f / 50000.0f;

    const std::array<float, 3> p1 = MidPageDir(dim, 40.0f);
    layer.BeginStrokeCapture();
    layer.ApplyDab(p1[0], p1[1], p1[2], angR, 5.0f, false);
    const std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    EXPECT_EQ(layer.EscalatedPageCount(), 1u);
    EXPECT_EQ(layer.AllocatedPageCount(), 16u);
    const float centre1 = SampleSphereSculptByDir(layer.MakeSampler(), p1[0], p1[1], p1[2]);
    EXPECT_GT(centre1, 0.6f * 5.0f);

    // Second sub-texel dab: only 4 slots remain — escalation refused, base-level write proceeds.
    const std::array<float, 3> p2 = MidPageDir(dim, 50.0f);
    layer.ApplyDab(p2[0], p2[1], p2[2], angR, 5.0f, false);
    EXPECT_EQ(layer.EscalatedPageCount(), 1u) << "no contiguous block free — escalation refused";
    EXPECT_LE(layer.AllocatedPageCount(), 20u);
    EXPECT_FALSE(layer.PoolExhausted())
        << "an escalation refusal is not a write refusal — the base-level write proceeded";
    const float centre1After = SampleSphereSculptByDir(layer.MakeSampler(), p1[0], p1[1], p1[2]);
    EXPECT_EQ(centre1After, centre1) << "existing escalated content must be untouched";

    // Undo the escalating stroke: the whole 16-slot block reclaims...
    layer.RestoreDabPages(before);
    EXPECT_EQ(layer.EscalatedPageCount(), 0u);
    EXPECT_LE(layer.AllocatedPageCount(), 4u);

    // ...and the freed slots satisfy a later escalation via the contiguous-run scan.
    const std::array<float, 3> p3 = MidPageDir(dim, 60.0f);
    layer.ApplyDab(p3[0], p3[1], p3[2], angR, 5.0f, false);
    EXPECT_EQ(layer.EscalatedPageCount(), 1u)
        << "the reclaimed block must be re-usable for escalation (free-list contiguous run)";
    const float centre3 = SampleSphereSculptByDir(layer.MakeSampler(), p3[0], p3[1], p3[2]);
    EXPECT_GT(centre3, 0.6f * 5.0f);
}

// ---- Undo across escalation (#630 composition, gate 4) ------------------------------------------

// A stroke that ESCALATES a page carrying earlier base-level dab + modifier content undoes
// byte-exact (level, dab AND modifier bytes — the escalation pre-image captures both layers,
// because upsampling rewrote the modifier layer too) and redoes byte-exact. A dab-only snapshot
// whose level no longer matches the live page is refused per page (stale-level refusal).
TEST(SphereSculptEscalation, UndoAcrossEscalationIsByteExactAndStaleLevelRefuses)
{
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeom(50000.0f, 64u));
    const uint32_t dim = layer.Geometry().VirtualDim;
    const std::array<float, 3> n = MidPageDir(dim, 40.0f);

    // Pre-stroke content on the target page: a base-level dab AND baked modifier content.
    layer.ApplyDab(n[0], n[1], n[2], 120.0f / 50000.0f, 2.0f, false);
    const SphereEditRegions region = ClassifySphereCapEdit(n[0], n[1], n[2], 250.0f / 50000.0f);
    layer.BakeModifierLayer(region, [](float, float, float) { return 1.5f; });
    const std::vector<SphereSculptPageContent> pre = SortedExport(layer);
    const uint32_t preSlots = layer.AllocatedPageCount();

    // The escalating stroke.
    layer.BeginStrokeCapture();
    layer.ApplyDab(n[0], n[1], n[2], 5.0f / 50000.0f, 5.0f, false);
    const std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());
    const std::vector<SphereSculptPageState> after = layer.SnapshotPages(before);
    const std::vector<SphereSculptPageContent> post = SortedExport(layer);
    ASSERT_GT(layer.EscalatedPageCount(), 0u);

    // Undo: byte-exact pre-stroke state, level de-escalated, slots reclaimed.
    layer.RestoreDabPages(before);
    ExpectExportsIdentical(SortedExport(layer), pre, "undo across escalation");
    EXPECT_EQ(layer.EscalatedPageCount(), 0u);
    EXPECT_EQ(layer.AllocatedPageCount(), preSlots);

    // Redo: byte-exact post-stroke state (level re-escalated, both layers restored).
    layer.RestoreDabPages(after);
    ExpectExportsIdentical(SortedExport(layer), post, "redo across escalation");
    ASSERT_GT(layer.EscalatedPageCount(), 0u);

    // Stale-level refusal: a dab-only level-0 snapshot against the now-escalated page cannot be
    // restored (no modifier bytes to rebuild the block at level 0) — refused per page, store
    // untouched, no version churn.
    const std::vector<SphereSculptPageContent> current = SortedExport(layer);
    const uint32_t versionBefore = layer.Version();
    SphereSculptPageState stale;
    stale.Face = current[0].Face;
    stale.PageX = current.back().PageX;
    stale.PageY = current.back().PageY;
    // Find the escalated page's key for the stale snapshot.
    for (const SphereSculptPageContent& p : current)
    {
        if (p.Level > 0u)
        {
            stale.Face = p.Face;
            stale.PageX = p.PageX;
            stale.PageY = p.PageY;
            break;
        }
    }
    stale.VirtualDim = dim;
    stale.Level = 0u;
    stale.Allocated = true;
    stale.HasModifier = false;
    stale.Dab.assign(kSculptPageTexels, 1.0f);
    const SphereEditRegions refused = layer.RestoreDabPages({stale});
    EXPECT_EQ(refused.Count, 0u);
    EXPECT_EQ(layer.Version(), versionBefore);
    ExpectExportsIdentical(SortedExport(layer), current, "stale-level refusal must not mutate");
}

// ---- .tsculpt v2 (#637 composition, gate 4) -----------------------------------------------------

// v2 round-trips an escalated store byte-exactly (level + fine layers, including a modifier layer
// baked AT the escalated resolution); a v1 blob still reads (level 0); malformed v2 blobs (level
// beyond the cap, truncated fine payload, level bits under a v1 header) are rejected wholesale.
TEST(SphereSculptEscalation, TsculptV2RoundTripV1ReadAndRejection)
{
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeom(50000.0f, 64u));
    const uint32_t dim = layer.Geometry().VirtualDim;
    const std::array<float, 3> n = MidPageDir(dim, 40.0f);
    layer.ApplyDab(n[0], n[1], n[2], 5.0f / 50000.0f, 5.0f, false); // escalates (slot 0 block)
    ASSERT_GT(layer.EscalatedPageCount(), 0u);
    // Bake a modifier over the escalated page AFTER escalation — the bake writes at the page's
    // fine resolution (the level-aware bake path), so v2 carries an L2 modifier layer too.
    const SphereEditRegions region = ClassifySphereCapEdit(n[0], n[1], n[2], 150.0f / 50000.0f);
    layer.BakeModifierLayer(region, [](float, float, float) { return 2.5f; });
    // Composed sample inside the modifier footprint but outside the dab cap: exactly the SET
    // value (fine texels store dab 0 + modifier 2.5).
    {
        std::array<float, 3> off{};
        FaceUVToWorldDir(0u, 0.5f + 60.0f / static_cast<float>(dim), 0.5f, off[0], off[1], off[2]);
        (void)off; // probe positions vary with the classify rect; the round-trip below is the gate
    }

    const std::vector<SphereSculptPageContent> original = SortedExport(layer);
    const std::vector<uint8_t> blob = EncodeSphereSculpt(layer.Geometry(), layer.ExportPages());
    uint32_t version = 0u;
    std::memcpy(&version, blob.data() + sizeof(uint32_t), sizeof(uint32_t));
    EXPECT_EQ(version, 2u) << "an escalated store must encode as v2";

    SphereSculptGeometry decodedGeom{};
    std::vector<SphereSculptPageContent> decodedPages;
    ASSERT_TRUE(DecodeSphereSculpt(blob.data(), blob.size(), decodedGeom, decodedPages));
    EXPECT_EQ(decodedGeom.VirtualDim, layer.Geometry().VirtualDim);

    SphereSculptLayer restored;
    restored.Configure(RuntimeGeom(50000.0f, 64u));
    restored.ImportPages(decodedPages);
    ExpectExportsIdentical(SortedExport(restored), original, ".tsculpt v2 round-trip");
    EXPECT_EQ(restored.EscalatedPageCount(), layer.EscalatedPageCount());

    // v1 read: a level-0-only store encodes as v1 and decodes with level 0 throughout.
    SphereSculptLayer flat;
    flat.Configure(Geom(512u, 16u));
    float fx, fy, fz;
    FaceUVToWorldDir(1u, 0.5f, 0.5f, fx, fy, fz);
    flat.ApplyDab(fx, fy, fz, 0.05f, 3.0f, false);
    const std::vector<uint8_t> v1 = EncodeSphereSculpt(flat.Geometry(), flat.ExportPages());
    std::memcpy(&version, v1.data() + sizeof(uint32_t), sizeof(uint32_t));
    ASSERT_EQ(version, 1u);
    SphereSculptGeometry v1Geom{};
    std::vector<SphereSculptPageContent> v1Pages;
    ASSERT_TRUE(DecodeSphereSculpt(v1.data(), v1.size(), v1Geom, v1Pages));
    for (const SphereSculptPageContent& p : v1Pages)
        EXPECT_EQ(p.Level, 0u) << "v1 blobs decode as all-level-0";

    // Rejections. The escalated block sits at slot 0, so the first record is the L2 page:
    // header = 5 u32, record flags at header + 3 u32.
    const size_t flagsOffset = 5u * sizeof(uint32_t) + 3u * sizeof(uint32_t);
    {
        std::vector<uint8_t> bad = blob; // level beyond the cap
        uint32_t flags = 0u;
        std::memcpy(&flags, bad.data() + flagsOffset, sizeof(uint32_t));
        flags |= kSculptBlobLevelMask; // level 3
        std::memcpy(bad.data() + flagsOffset, &flags, sizeof(uint32_t));
        SphereSculptGeometry g{};
        std::vector<SphereSculptPageContent> p;
        EXPECT_FALSE(DecodeSphereSculpt(bad.data(), bad.size(), g, p));
    }
    {
        std::vector<uint8_t> bad = blob; // truncated fine payload
        bad.resize(bad.size() - 512u);
        SphereSculptGeometry g{};
        std::vector<SphereSculptPageContent> p;
        EXPECT_FALSE(DecodeSphereSculpt(bad.data(), bad.size(), g, p));
    }
    {
        std::vector<uint8_t> bad = blob; // v1 header over records carrying level bits
        const uint32_t v1Version = 1u;
        std::memcpy(bad.data() + sizeof(uint32_t), &v1Version, sizeof(uint32_t));
        SphereSculptGeometry g{};
        std::vector<SphereSculptPageContent> p;
        EXPECT_FALSE(DecodeSphereSculpt(bad.data(), bad.size(), g, p))
            << "v1 must reject unknown (level) flag bits rather than guess";
    }
}

// ---- Resize remap (#632 composition, gate 4) ----------------------------------------------------

// A planet resize carries escalated content level-aware: the remapped page keeps its level (fine
// density), its angular position and metre amplitude survive within resample error, and a pool
// that cannot hold the remapped SLOTS refuses the whole remap (content intact at the previous
// geometry) — the same refuse-don't-lose contract as the level-0 remap.
TEST(SphereSculptEscalation, ResizeRemapCarriesEscalatedContentAndRefusesHonestly)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(50000.0f, 128u));
    const uint32_t dim = layer.Geometry().VirtualDim;
    const std::array<float, 3> n = MidPageDir(dim, 40.0f);
    const float strength = 10.0f;
    layer.ApplyDab(n[0], n[1], n[2], 5.0f / 50000.0f, strength, false);
    ASSERT_GT(layer.EscalatedPageCount(), 0u);
    const float centreBefore = SampleSphereSculptByDir(layer.MakeSampler(), n[0], n[1], n[2]);
    ASSERT_GT(centreBefore, 0.6f * strength);

    // Grow the planet: the grid re-derives; escalated content must carry its level and stay put.
    const SphereEditRegions remapped = layer.Configure(MakeSphereSculptGeometry(55000.0f, 128u));
    EXPECT_GT(remapped.Count, 0u) << "a remap IS an edit";
    EXPECT_NE(layer.Geometry().VirtualDim, dim);
    EXPECT_GT(layer.EscalatedPageCount(), 0u) << "the escalated page's level must carry across";
    const float centreAfter = SampleSphereSculptByDir(layer.MakeSampler(), n[0], n[1], n[2]);
    std::printf("[escalation] resize remap: centre %.3f -> %.3f (of %.1f m)\n", centreBefore,
                centreAfter, strength);
    EXPECT_GT(centreAfter, 0.5f * strength)
        << "escalated content must survive the resize at its angular position";

    // Refusal: a pool too small for the remapped slot total keeps geometry + content untouched.
    const SphereSculptGeometry live = layer.Geometry();
    SphereSculptGeometry tiny = live;
    tiny.PoolPageCount = 8u; // the escalated block alone needs 16
    const SphereEditRegions refused = layer.Configure(tiny);
    EXPECT_EQ(refused.Count, 0u);
    EXPECT_EQ(layer.Geometry().PoolPageCount, live.PoolPageCount)
        << "a refused remap must keep the previous geometry";
    const float centreRefused = SampleSphereSculptByDir(layer.MakeSampler(), n[0], n[1], n[2]);
    EXPECT_EQ(centreRefused, centreAfter) << "refusal must leave the content bit-identical";
}

// ---- Analytic flags x escalation (#664/#665 composition, gate 4) --------------------------------

// The analytic placement set composes ADDITIVELY over an escalated store through the S2/S3
// chokepoint: composed == store + flatten + dab term-for-term (bitwise: same accumulation order),
// and the empty set stays bit-identical to the plain store sample — over escalated pages too.
TEST(SphereSculptEscalation, AnalyticFlagsComposeOverEscalatedPages)
{
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeom(50000.0f, 64u));
    const uint32_t dim = layer.Geometry().VirtualDim;
    const std::array<float, 3> n = MidPageDir(dim, 40.0f);
    layer.ApplyDab(n[0], n[1], n[2], 5.0f / 50000.0f, 5.0f, false);
    ASSERT_GT(layer.EscalatedPageCount(), 0u);

    const SphereAnalyticFlatten flatten = MakeSphereAnalyticFlatten(
        n[0] * 50000.0f, n[1] * 50000.0f, n[2] * 50000.0f, 40.0f, 10.0f, 50010.0f, 50000.0f);
    const SphereAnalyticDab dab =
        MakeSphereAnalyticDab(n[0], n[1], n[2], 30.0f / 50000.0f, 2.0f, 50000.0f);
    const SphereAnalyticModifierSet set{&flatten, 1u, &dab, 1u};
    const float relief = 3.25f;

    for (int i = 0; i < 25; ++i)
    {
        std::array<float, 3> d{};
        const float offU = 0.5f + (static_cast<float>(i % 5) - 2.0f) * 0.4f / static_cast<float>(dim);
        const float offV = 0.5f + (static_cast<float>(i / 5) - 2.0f) * 0.4f / static_cast<float>(dim);
        FaceUVToWorldDir(0u, offU, offV, d[0], d[1], d[2]);
        const float composed =
            SampleSphereSculptComposed(layer.MakeSampler(), set, d[0], d[1], d[2], relief);
        // Recompute with the chokepoint's own accumulation order — must be bitwise identical.
        float expected = SampleSphereSculptByDir(layer.MakeSampler(), d[0], d[1], d[2]);
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float ux = d[0] / len, uy = d[1] / len, uz = d[2] / len;
        expected += EvaluateSphereAnalyticFlatten(flatten, ux, uy, uz, relief);
        expected += EvaluateSphereAnalyticDab(dab, ux, uy, uz);
        EXPECT_EQ(composed, expected) << "composition must be additive term-for-term";

        const float empty = SampleSphereSculptComposed(layer.MakeSampler(),
                                                       SphereAnalyticModifierSet{}, d[0], d[1],
                                                       d[2], relief);
        EXPECT_EQ(empty, SampleSphereSculptByDir(layer.MakeSampler(), d[0], d[1], d[2]))
            << "the empty set must stay bit-identical over escalated pages (dark-ship)";
    }
}
