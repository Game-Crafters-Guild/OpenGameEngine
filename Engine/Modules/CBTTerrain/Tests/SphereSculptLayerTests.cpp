// CPU oracle for the sparse virtual sculpt PAGE-TABLE store (planet editing v2).
// Device-free: the authoring math
// (mirror mutation, region-proportional cost, cross-face consistency, quiescence) is verified here,
// plus the paging-specific oracles: parity vs a flat golden at
// Dv=256, page materialization counts, pool-exhaustion honesty, and radius-scaled m/texel.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"

using namespace GameEngine::CBTTerrain;

namespace
{
// A geometry with a chosen virtual dim + pool count (Dv must be a multiple of kSculptPageDim).
SphereSculptGeometry Geom(uint32_t virtualDim, uint32_t pageCount)
{
    return SphereSculptGeometry{virtualDim, virtualDim / kSculptPageDim, kSculptMaxPagesPerFaceAxis,
                                pageCount};
}
// The default parity geometry: Dv = 256 (== the pre-paging fixed atlas dim), generous pool.
SphereSculptGeometry Geom256() { return Geom(256u, 64u); }

// Exact stored value at virtual texel (face, x, y) — the paged twin of the old flat atlas index.
float Texel(const SphereSculptLayer& layer, uint32_t face, uint32_t x, uint32_t y)
{
    return SculptResolveTexel(layer.MakeSampler(), face, x, y);
}

// Bilinear sample at face-local (u,v) — the SAME sample the shader performs.
float SampleFace(const SphereSculptLayer& layer, uint32_t face, float u, float v)
{
    return SampleSculptFaceUV(layer.MakeSampler(), face, u, v);
}

double BandAbsSum(const SphereSculptLayer& layer, uint32_t face)
{
    const uint32_t dim = layer.Geometry().VirtualDim;
    double s = 0.0;
    for (uint32_t y = 0; y < dim; ++y)
        for (uint32_t x = 0; x < dim; ++x)
            s += std::fabs(Texel(layer, face, x, y));
    return s;
}

float BandPeak(const SphereSculptLayer& layer, uint32_t face)
{
    const uint32_t dim = layer.Geometry().VirtualDim;
    float peak = 0.0f;
    for (uint32_t y = 0; y < dim; ++y)
        for (uint32_t x = 0; x < dim; ++x)
            peak = std::fmax(peak, std::fabs(Texel(layer, face, x, y)));
    return peak;
}

float CentreTexel(const SphereSculptLayer& layer, uint32_t face)
{
    const uint32_t half = layer.Geometry().VirtualDim / 2u;
    return Texel(layer, face, half, half);
}

SphereEditRegions FaceRect(uint32_t face, float minU, float minV, float maxU, float maxV)
{
    SphereEditRegions r{};
    r.Rects[r.Count++] = SphereFaceUVRect{face, minU, minV, maxU, maxV};
    return r;
}
} // namespace

// A fresh (unconfigured) layer is quiescent: no edits, no allocation, no dirty region.
TEST(SphereSculptLayer, FreshIsQuiescent)
{
    SphereSculptLayer layer;
    EXPECT_EQ(layer.Version(), 0u);
    EXPECT_FALSE(layer.HasEdits());
    EXPECT_EQ(layer.AllocatedPageCount(), 0u);
    uint32_t f;
    float a, b, c, d;
    EXPECT_FALSE(layer.DirtyFaceRect(f, a, b, c, d));
    // A configured-but-unedited layer is also quiescent (zero pages).
    layer.Configure(Geom256());
    EXPECT_EQ(layer.Version(), 0u);
    EXPECT_EQ(layer.AllocatedPageCount(), 0u);
}

// A raise dab on the +X face centre raises that band (peak ~strength) and touches ONLY that band.
TEST(SphereSculptLayer, RaiseDabSingleFace)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    const float strength = 25.0f;
    const SphereEditRegions regions = layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, strength, false);

    EXPECT_EQ(regions.Count, 1u);
    EXPECT_EQ(regions.Rects[0].Face, 0u); // +X
    EXPECT_EQ(layer.Version(), 1u);
    EXPECT_TRUE(layer.HasEdits());

    const float peak = BandPeak(layer, 0u);
    EXPECT_GT(peak, strength * 0.7f) << "peak offset too weak";
    EXPECT_LE(peak, strength + 1e-3f) << "peak exceeds strength (falloff should cap at 1)";
    EXPECT_GT(CentreTexel(layer, 0u), 0.0f);
    EXPECT_GT(BandAbsSum(layer, 0u), 0.0);
    for (uint32_t f = 1; f < kCubeFaceCount; ++f)
    {
        EXPECT_EQ(BandAbsSum(layer, f), 0.0) << "interior dab bled onto face " << f;
        EXPECT_EQ(layer.AllocatedPageCountForFace(f), 0u) << "untouched face allocated a page";
    }

    uint32_t face;
    float minU, minV, maxU, maxV;
    ASSERT_TRUE(layer.DirtyFaceRect(face, minU, minV, maxU, maxV));
    EXPECT_EQ(face, 0u);
    EXPECT_LT(minU, 0.5f);
    EXPECT_GT(maxU, 0.5f);
}

// Lower subtracts.
TEST(SphereSculptLayer, LowerDabNegative)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(0.0f, 1.0f, 0.0f, 0.05f, 10.0f, true); // +Y face, lower
    EXPECT_LT(CentreTexel(layer, 2u), 0.0f) << "lower must push the band down";
}

// A dab straddling the +X/+Y cube edge edits BOTH face bands, and the dirty region reports the
// primary (centre) face.
TEST(SphereSculptLayer, CrossFaceDabEditsBothBands)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    const SphereEditRegions regions = layer.ApplyDab(1.0f, 1.0f, 0.0f, 0.08f, 15.0f, false);
    EXPECT_EQ(regions.Count, 2u);
    EXPECT_GT(BandAbsSum(layer, 0u), 0.0) << "+X band not edited";
    EXPECT_GT(BandAbsSum(layer, 2u), 0.0) << "+Y band not edited";
    for (uint32_t f : {1u, 3u, 4u, 5u})
        EXPECT_EQ(BandAbsSum(layer, f), 0.0) << "face " << f << " should be untouched";

    uint32_t face;
    float a, b, c, d;
    ASSERT_TRUE(layer.DirtyFaceRect(face, a, b, c, d));
    EXPECT_EQ(face, 0u); // (1,1,0) tie-breaks to +X in WorldDirToFaceUV
}

// The falloff is smooth: the centre texel has the largest magnitude; a texel off-centre is smaller.
TEST(SphereSculptLayer, FalloffMonotoneFromCentre)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.10f, 40.0f, false);
    const float centre = CentreTexel(layer, 0u);
    const uint32_t half = layer.Geometry().VirtualDim / 2u;
    const float offCentre = Texel(layer, 0u, half + 6u, half);
    EXPECT_GT(centre, 0.0f);
    EXPECT_LT(offCentre, centre);
    EXPECT_GE(offCentre, 0.0f);
}

// A degenerate dab (zero direction or zero radius) changes nothing (quiescence guard).
TEST(SphereSculptLayer, DegenerateDabNoOp)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(0.0f, 0.0f, 0.0f, 0.05f, 10.0f, false);
    EXPECT_EQ(layer.Version(), 0u);
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.0f, 10.0f, false);
    EXPECT_EQ(layer.Version(), 0u);
    EXPECT_EQ(layer.AllocatedPageCount(), 0u);
}

// CRACK ORACLE (cross-face): an off-centre dab straddling the +X/+Y cube edge writes the shared
// edge column to the IDENTICAL value on both faces (sampled by direction from either face).
TEST(SphereSculptLayer, CrossFaceSharedEdgeTexelsAgree)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(1.0f, 0.85f, 0.0f, 0.12f, 20.0f, false);
    layer.ApplyDab(0.85f, 1.0f, 0.10f, 0.12f, 18.0f, false);
    layer.ApplyDab(1.0f, 0.90f, -0.15f, 0.10f, 12.0f, true);

    uint32_t edited = 0;
    for (int s = -5; s <= 5; ++s)
    {
        const float t = 0.08f * static_cast<float>(s);
        const float dir[3] = {1.0f, 1.0f, t};
        float uA, vA, uB, vB;
        ASSERT_TRUE(ProjectDirOntoFace(0u, dir[0], dir[1], dir[2], 1e-3f, uA, vA)) << "+X project";
        ASSERT_TRUE(ProjectDirOntoFace(2u, dir[0], dir[1], dir[2], 1e-3f, uB, vB)) << "+Y project";
        const float sA = SampleFace(layer, 0u, uA, vA);
        const float sB = SampleFace(layer, 2u, uB, vB);
        EXPECT_NEAR(sA, sB, 1e-3f) << "shared cube-edge value disagrees across faces (t=" << t << ")";
        if (std::fabs(sA) > 1e-3f)
            ++edited;
    }
    EXPECT_GT(edited, 0u) << "the test did not actually edit the shared edge";
}

// CRACK ORACLE (corner): a dab near cube corner 7 (+,+,+) writes the corner texel to the same value
// on all three faces (+X,+Y,+Z).
TEST(SphereSculptLayer, CornerTexelAgreesThreeFaces)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(0.92f, 0.90f, 0.94f, 0.14f, 20.0f, false);
    const float dir[3] = {1.0f, 1.0f, 1.0f};
    float u0, v0, u2, v2, u4, v4;
    ASSERT_TRUE(ProjectDirOntoFace(0u, dir[0], dir[1], dir[2], 1e-3f, u0, v0));
    ASSERT_TRUE(ProjectDirOntoFace(2u, dir[0], dir[1], dir[2], 1e-3f, u2, v2));
    ASSERT_TRUE(ProjectDirOntoFace(4u, dir[0], dir[1], dir[2], 1e-3f, u4, v4));
    const float s0 = SampleFace(layer, 0u, u0, v0);
    const float s2 = SampleFace(layer, 2u, u2, v2);
    const float s4 = SampleFace(layer, 4u, u4, v4);
    EXPECT_GT(std::fabs(s0), 1e-3f) << "corner not actually edited";
    EXPECT_NEAR(s0, s2, 1e-3f) << "corner texel disagrees +X vs +Y";
    EXPECT_NEAR(s0, s4, 1e-3f) << "corner texel disagrees +X vs +Z";
}

// MODIFIER LAYER: a constant-offset bake over one face sets that band (SET) and touches only it.
TEST(SphereSculptLayer, BakeModifierLayerSetsConstantOnOneFace)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    const SphereEditRegions baked =
        layer.BakeModifierLayer(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 7.0f; });
    EXPECT_EQ(baked.Count, 1u);
    EXPECT_EQ(layer.Version(), 1u);
    EXPECT_TRUE(layer.HasEdits());
    EXPECT_NEAR(CentreTexel(layer, 0u), 7.0f, 1e-4f);
    EXPECT_NEAR(BandPeak(layer, 0u), 7.0f, 1e-4f);
    for (uint32_t f = 1; f < kCubeFaceCount; ++f)
        EXPECT_EQ(BandAbsSum(layer, f), 0.0) << "modifier bake bled onto face " << f;
}

// The published pool COMPOSES the dab layer and the modifier layer: a dab + a modifier bake on the
// same face sum, so freehand strokes and baked modifiers coexist without erasing each other.
TEST(SphereSculptLayer, BakeAndDabCompose)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.BakeModifierLayer(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 3.0f; });
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 10.0f, false); // +X centre raise
    EXPECT_GT(CentreTexel(layer, 0u), 8.0f) << "pool must compose dab + modifier";
    // A corner texel (dab ~0 there) == the modifier value alone.
    EXPECT_NEAR(Texel(layer, 0u, 2u, 2u), 3.0f, 1e-3f);
}

// SET semantics: re-baking a region with an empty stack (0) clears any prior modifier value.
TEST(SphereSculptLayer, BakeReDeriveClearsStale)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.BakeModifierLayer(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 5.0f; });
    EXPECT_NEAR(CentreTexel(layer, 0u), 5.0f, 1e-4f);
    layer.BakeModifierLayer(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 0.0f; });
    EXPECT_NEAR(CentreTexel(layer, 0u), 0.0f, 1e-4f) << "SET must clear the vacated modifier region";
}

// REGION COST: re-baking a sub-rect touches ONLY that rect's texels; texels outside keep their bake.
TEST(SphereSculptLayer, BakeModifierRegionCost)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.BakeModifierLayer(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 4.0f; });
    layer.BakeModifierLayer(FaceRect(0u, 0.4f, 0.4f, 0.6f, 0.6f), [](float, float, float) { return 9.0f; });
    EXPECT_NEAR(CentreTexel(layer, 0u), 9.0f, 1e-4f) << "the sub-rect re-baked";
    EXPECT_NEAR(Texel(layer, 0u, 2u, 2u), 4.0f, 1e-4f)
        << "a texel outside the sub-rect must be untouched (region cost)";
}

// CRACK ORACLE (modifier bake, cross-face): a modifier baked over two faces sharing a cube edge
// writes the shared edge to the identical value on both.
TEST(SphereSculptLayer, BakeModifierCrossFaceSeamFree)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    SphereEditRegions r{};
    r.Rects[r.Count++] = SphereFaceUVRect{0u, 0.0f, 0.0f, 1.0f, 1.0f}; // +X
    r.Rects[r.Count++] = SphereFaceUVRect{2u, 0.0f, 0.0f, 1.0f, 1.0f}; // +Y
    auto eval = [](float dx, float dy, float dz) { return 10.0f * dx + 5.0f * dy + 3.0f * dz; };
    layer.BakeModifierLayer(r, eval);
    for (int s = -4; s <= 4; ++s)
    {
        const float t = 0.1f * static_cast<float>(s);
        const float dir[3] = {1.0f, 1.0f, t};
        float uA, vA, uB, vB;
        ASSERT_TRUE(ProjectDirOntoFace(0u, dir[0], dir[1], dir[2], 1e-3f, uA, vA));
        ASSERT_TRUE(ProjectDirOntoFace(2u, dir[0], dir[1], dir[2], 1e-3f, uB, vB));
        EXPECT_NEAR(SampleFace(layer, 0u, uA, vA), SampleFace(layer, 2u, uB, vB), 1e-2f)
            << "modifier bake cracks at the shared cube edge (t=" << t << ")";
    }
}

// ---------------------------------------------------------------------------
// Paging oracles (the merge gates)
// ---------------------------------------------------------------------------

// PARITY: at Dv=256 the paged store's sampled heights are BYTE-IDENTICAL to an independent flat
// 256^2 computation of the same dab sequence — the page grid reproduces the pre-paging texel
// positions exactly, so migrating storage changed no sampled value. (Documented: byte-identical at
// Dv=256 because the page texel grid matches the old 256^2 grid.)
TEST(SphereSculptLayer, ParitySampledHeightsMatchFlatGoldenAtDim256)
{
    constexpr uint32_t kDim = 256u;
    SphereSculptLayer layer;
    layer.Configure(Geom(kDim, 64u));

    struct Dab { float cx, cy, cz, ar, strength; bool lower; };
    const Dab seq[] = {
        {1.0f, 0.0f, 0.0f, 0.06f, 20.0f, false}, {1.0f, 0.30f, 0.0f, 0.05f, 12.0f, false},
        {1.0f, 0.0f, 0.20f, 0.04f, 8.0f, true},  {1.0f, 1.0f, 0.0f, 0.09f, 15.0f, false},
    };

    // Independent flat golden: replay the EXACT ApplyDab falloff into a flat 256^2/face atlas.
    std::vector<float> golden(static_cast<size_t>(kDim) * kDim * kCubeFaceCount, 0.0f);
    const int32_t dimI = static_cast<int32_t>(kDim);
    const float invDimMinus1 = 1.0f / static_cast<float>(dimI - 1);
    for (const Dab& d : seq)
    {
        layer.ApplyDab(d.cx, d.cy, d.cz, d.ar, d.strength, d.lower);
        const float clen = std::sqrt(d.cx * d.cx + d.cy * d.cy + d.cz * d.cz);
        const float nx = d.cx / clen, ny = d.cy / clen, nz = d.cz / clen;
        const SphereEditRegions regions = ClassifySphereCapEdit(nx, ny, nz, d.ar);
        const float sign = d.lower ? -1.0f : 1.0f;
        const float cosR = std::cos(d.ar);
        const int32_t margin =
            static_cast<int32_t>(std::ceil(0.3f * d.ar * static_cast<float>(dimI - 1))) + 2;
        auto clampT = [&](int32_t v) { return std::clamp(v, 0, dimI - 1); };
        for (uint32_t ri = 0; ri < regions.Count; ++ri)
        {
            const SphereFaceUVRect& r = regions.Rects[ri];
            if (r.IsEmpty())
                continue;
            const int32_t minTx = clampT(static_cast<int32_t>(std::floor(r.MinU * (dimI - 1))) - margin);
            const int32_t maxTx = clampT(static_cast<int32_t>(std::ceil(r.MaxU * (dimI - 1))) + margin);
            const int32_t minTz = clampT(static_cast<int32_t>(std::floor(r.MinV * (dimI - 1))) - margin);
            const int32_t maxTz = clampT(static_cast<int32_t>(std::ceil(r.MaxV * (dimI - 1))) + margin);
            for (int32_t tz = minTz; tz <= maxTz; ++tz)
                for (int32_t tx = minTx; tx <= maxTx; ++tx)
                {
                    float gx, gy, gz;
                    FaceUVToWorldDir(r.Face, tx * invDimMinus1, tz * invDimMinus1, gx, gy, gz);
                    const float cosd = gx * nx + gy * ny + gz * nz;
                    if (cosd < cosR)
                        continue;
                    const float ang = std::acos(std::clamp(cosd, -1.0f, 1.0f));
                    const float t = std::clamp(1.0f - ang / d.ar, 0.0f, 1.0f);
                    const float falloff = t * t * (3.0f - 2.0f * t);
                    golden[(r.Face * kDim + tz) * kDim + tx] += sign * d.strength * falloff;
                }
        }
    }

    // Byte-identical: every virtual texel equals the golden flat texel.
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
        for (uint32_t y = 0; y < kDim; ++y)
            for (uint32_t x = 0; x < kDim; ++x)
                ASSERT_EQ(Texel(layer, f, x, y), golden[(f * kDim + y) * kDim + x])
                    << "paged vs flat mismatch at face " << f << " (" << x << "," << y << ")";
}

// PAGE MATERIALIZATION: a dab allocates EXACTLY the pages its written texels fall into, untouched
// faces allocate ZERO, and an idle (no-edit) layer allocates ZERO.
TEST(SphereSculptLayer, DabAllocatesExactlyTouchedPages)
{
    constexpr uint32_t kDim = 512u; // 4x4 pages/face
    SphereSculptLayer layer;
    layer.Configure(Geom(kDim, 128u));
    EXPECT_EQ(layer.AllocatedPageCount(), 0u) << "idle configured layer allocates nothing";

    const float cx = 1.0f, cy = 0.12f, cz = 0.08f, ar = 0.05f, strength = 20.0f;
    layer.ApplyDab(cx, cy, cz, ar, strength, false);

    // Independently compute which pages the dab's non-zero writes touch (replay the falloff test).
    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float nx = cx / clen, ny = cy / clen, nz = cz / clen;
    const SphereEditRegions regions = ClassifySphereCapEdit(nx, ny, nz, ar);
    const float cosR = std::cos(ar);
    const int32_t dimI = static_cast<int32_t>(kDim);
    const float invDimMinus1 = 1.0f / static_cast<float>(dimI - 1);
    const int32_t margin =
        static_cast<int32_t>(std::ceil(0.3f * ar * static_cast<float>(dimI - 1))) + 2;
    std::vector<uint8_t> touchedPage(static_cast<size_t>(kCubeFaceCount) * kSculptMaxPagesPerFaceAxis *
                                         kSculptMaxPagesPerFaceAxis,
                                     0u);
    uint32_t expectedPages = 0u;
    auto clampT = [&](int32_t v) { return std::clamp(v, 0, dimI - 1); };
    for (uint32_t ri = 0; ri < regions.Count; ++ri)
    {
        const SphereFaceUVRect& r = regions.Rects[ri];
        if (r.IsEmpty())
            continue;
        const int32_t minTx = clampT(static_cast<int32_t>(std::floor(r.MinU * (dimI - 1))) - margin);
        const int32_t maxTx = clampT(static_cast<int32_t>(std::ceil(r.MaxU * (dimI - 1))) + margin);
        const int32_t minTz = clampT(static_cast<int32_t>(std::floor(r.MinV * (dimI - 1))) - margin);
        const int32_t maxTz = clampT(static_cast<int32_t>(std::ceil(r.MaxV * (dimI - 1))) + margin);
        for (int32_t tz = minTz; tz <= maxTz; ++tz)
            for (int32_t tx = minTx; tx <= maxTx; ++tx)
            {
                float gx, gy, gz;
                FaceUVToWorldDir(r.Face, tx * invDimMinus1, tz * invDimMinus1, gx, gy, gz);
                if (gx * nx + gy * ny + gz * nz < cosR)
                    continue; // outside the cap -> no write -> no page
                const uint32_t pageX = static_cast<uint32_t>(tx) / kSculptPageDim;
                const uint32_t pageY = static_cast<uint32_t>(tz) / kSculptPageDim;
                const size_t key = (r.Face * kSculptMaxPagesPerFaceAxis + pageY) *
                                       kSculptMaxPagesPerFaceAxis +
                                   pageX;
                if (!touchedPage[key])
                {
                    touchedPage[key] = 1u;
                    ++expectedPages;
                }
            }
    }
    EXPECT_GT(expectedPages, 0u) << "the dab wrote no texels (nothing to prove)";
    EXPECT_EQ(layer.AllocatedPageCount(), expectedPages)
        << "the dab must allocate exactly the pages its non-zero texels touch";
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
        if (f != regions.Rects[0].Face && regions.Count == 1u)
            EXPECT_EQ(layer.AllocatedPageCountForFace(f), 0u) << "untouched face allocated a page";
}

// POOL EXHAUSTION HONESTY: fill a tiny pool, then the next allocating write is REFUSED (one-shot
// warning flag set) and existing content stays INTACT (hash unchanged).
TEST(SphereSculptLayer, PoolExhaustionRefusesAndPreservesContent)
{
    constexpr uint32_t kDim = 128u; // 1 page/face, so a face-centre dab lands in exactly one page
    SphereSculptLayer layer;
    layer.Configure(Geom(kDim, 3u)); // only 3 physical pages

    // Fill the pool: three separate face-centre dabs on distinct faces each land in one page.
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.03f, 10.0f, false); // +X
    layer.ApplyDab(0.0f, 1.0f, 0.0f, 0.03f, 10.0f, false); // +Y
    layer.ApplyDab(0.0f, 0.0f, 1.0f, 0.03f, 10.0f, false); // +Z
    ASSERT_EQ(layer.AllocatedPageCount(), 3u) << "the three dabs did not fill the 3-page pool";
    EXPECT_FALSE(layer.PoolExhausted());

    // Content hash before the over-budget edit.
    auto hash = [&] {
        double h = 0.0;
        for (uint32_t f = 0; f < kCubeFaceCount; ++f)
            for (uint32_t y = 0; y < kDim; ++y)
                for (uint32_t x = 0; x < kDim; ++x)
                    h += (Texel(layer, f, x, y) * 1.000001) * static_cast<double>((f + 1) * (x + 1) + y);
        return h;
    };
    const double before = hash();

    // A dab on a fourth face needs a new page — the pool is full, so it is refused.
    layer.ApplyDab(0.0f, -1.0f, 0.0f, 0.03f, 10.0f, false); // -Y (fresh page needed)
    EXPECT_TRUE(layer.PoolExhausted()) << "an over-budget allocation must set the exhausted flag";
    EXPECT_EQ(layer.AllocatedPageCount(), 3u) << "no new page was allocated (refused)";
    EXPECT_EQ(hash(), before) << "existing authored content must be intact after a refused edit";
    EXPECT_EQ(BandAbsSum(layer, 3u), 0.0) << "the refused -Y edit wrote nothing";
}

// FULL BAKE bounding + reclamation: BakeModifierFull clears the whole modifier layer (bounded by
// the pool) and reclaims pages that end up holding no content — repeated modifier moves do not leak.
TEST(SphereSculptLayer, FullBakeClearsAndReclaimsVacatedPages)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(256u, 64u));
    // Bake a modifier onto +X; it allocates pages.
    layer.BakeModifierFull(FaceRect(0u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 6.0f; });
    const uint32_t afterFirst = layer.AllocatedPageCount();
    EXPECT_GT(afterFirst, 0u);
    // A full re-bake that moves the modifier onto +Y (empty on +X) must clear +X's pages and
    // reclaim them — the allocated count tracks +Y only, not +X + +Y.
    layer.BakeModifierFull(FaceRect(2u, 0.0f, 0.0f, 1.0f, 1.0f), [](float, float, float) { return 6.0f; });
    EXPECT_EQ(BandAbsSum(layer, 0u), 0.0) << "the vacated +X face must be cleared";
    EXPECT_GT(BandAbsSum(layer, 2u), 0.0) << "+Y must hold the moved modifier";
    EXPECT_EQ(layer.AllocatedPageCount(), afterFirst)
        << "moved-modifier pages must be reclaimed, not leaked";
}

// RADIUS SCALING: at R=20000 the derived m/texel meets the 8 m target and a default ~128 m brush
// dab covers many virtual texels (>1) — the invisible-brush fix.
TEST(SphereSculptLayer, RadiusScalingMeetsTargetAndBrushCoversTexels)
{
    struct Case { float radius; };
    for (float radius : {2000.0f, 20000.0f, 50000.0f})
    {
        const uint32_t dim = DeriveSculptVirtualDim(radius);
        const float mpt = SculptMetersPerTexel(radius, dim);
        EXPECT_LE(mpt, kSculptMetersPerTexelTarget + 1e-3f)
            << "R=" << radius << " m/texel " << mpt << " exceeds the " << kSculptMetersPerTexelTarget
            << " m budget";
        // A 128 m-radius brush: angularRadius = worldRadius / planetRadius. Footprint diameter in
        // texels = (2*ar)/(pi/2)*dim.
        const float ar = 128.0f / radius;
        const float footprintTexels = (2.0f * ar) / kSculptHalfPi * static_cast<float>(dim);
        EXPECT_GT(footprintTexels, 1.0f)
            << "R=" << radius << " a 128 m brush is sub-texel (footprint " << footprintTexels << ")";
    }
    // Specifically kill the 20 km invisible-brush case: >>1 texel, not the old ~2.
    const uint32_t dim20k = DeriveSculptVirtualDim(20000.0f);
    const float ar = 128.0f / 20000.0f;
    EXPECT_GT((2.0f * ar) / kSculptHalfPi * static_cast<float>(dim20k), 20.0f)
        << "a 128 m brush on a 20 km planet must cover many texels now";
}

// RESET ORACLE (create->edit->delete->recreate): Reset() (a planet delete / domain-switch-away)
// returns the store to unconfigured so the NEXT Configure at a DIFFERENT radius re-derives fresh —
// the new planet must not inherit the old planet's Dv or stale content. This is the exact
// judge-session flow that resurrected "brush does nothing at 20k".
//
// Expectation change (content-remap slice): reconfiguring WHILE edits exist used to FREEZE the
// geometry ("geometry must freeze while a live planet has edits"); a live radius edit now REMAPS
// the authored content onto the new grid instead (the SphereSculptRemap oracles below), so this
// test asserts the remap adopts the new Dv rather than the old freeze.
TEST(SphereSculptLayer, ResetReDerivesForNewPlanet)
{
    SphereSculptLayer layer;
    // Small planet: Dv derives from R=2000.
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    const uint32_t smallDim = layer.Geometry().VirtualDim;
    EXPECT_EQ(smallDim, DeriveSculptVirtualDim(2000.0f));
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 20.0f, false);
    ASSERT_TRUE(layer.HasEdits());
    ASSERT_GT(layer.AllocatedPageCount(), 0u);

    // A live-resize reconfigure REMAPS: the store adopts the 20 km grid and keeps the content.
    layer.Configure(MakeSphereSculptGeometry(20000.0f, 64u));
    EXPECT_EQ(layer.Geometry().VirtualDim, DeriveSculptVirtualDim(20000.0f))
        << "a live radius edit must re-derive Dv (remap), not freeze the old resolution";
    EXPECT_TRUE(layer.HasEdits()) << "the remap must carry the authored content, not reset it";

    // Planet deleted -> Reset -> recreate at 20 km: geometry re-derives, pool empty, sample 0.
    layer.Reset();
    EXPECT_FALSE(layer.HasEdits());
    EXPECT_EQ(layer.AllocatedPageCount(), 0u);
    layer.Configure(MakeSphereSculptGeometry(20000.0f, 64u));
    const uint32_t bigDim = layer.Geometry().VirtualDim;
    EXPECT_EQ(bigDim, DeriveSculptVirtualDim(20000.0f)) << "the recreated planet must re-derive its own Dv";
    EXPECT_NE(bigDim, smallDim) << "20 km must not inherit the 2 km planet's frozen dim";
    EXPECT_EQ(layer.AllocatedPageCount(), 0u) << "recreated planet starts with an empty page pool";
    // Sampling anywhere returns 0 (no stale content from the deleted planet).
    EXPECT_EQ(SampleFace(layer, 0u, 0.5f, 0.5f), 0.0f);
    EXPECT_EQ(SampleFace(layer, 2u, 0.3f, 0.7f), 0.0f);
    EXPECT_EQ(BandAbsSum(layer, 0u), 0.0);
}

// UPLOAD QUIESCENCE ORACLE: after an edit each ring slot uploads ONCE, then idle frames upload
// NOTHING. A new edit re-arms one refresh per slot.
TEST(SphereSculptLayer, UploadGateRefreshesEachSlotOnceThenIdle)
{
    SphereSculptUploadGate gate;
    for (uint32_t f = 0; f < kCBTFrameParamsRing * 3u; ++f)
        EXPECT_FALSE(gate.ShouldUpload(f, 0u));

    uint32_t uploads = 0u;
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
        if (gate.ShouldUpload(f, 1u))
            ++uploads;
    EXPECT_EQ(uploads, kCBTFrameParamsRing);

    uint32_t idle = 0u;
    for (uint32_t f = 0; f < 200u; ++f)
        if (gate.ShouldUpload(f, 1u))
            ++idle;
    EXPECT_EQ(idle, 0u) << "an idle edited planet uploaded the store again";

    uint32_t reupload = 0u;
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
        if (gate.ShouldUpload(f, 2u))
            ++reupload;
    EXPECT_EQ(reupload, kCBTFrameParamsRing);
}

// DEVICE-REBUILD ORACLE: a device rebuild re-creates the sculpt ring buffers ZEROED, so the gate's
// per-slot "already holds version N" record is stale. Without the reset the quiescence gate above
// suppresses the re-upload at the SAME version and an edited planet renders its unedited base
// shape. Reset must re-arm exactly one upload per slot, at the unchanged version.
TEST(SphereSculptLayer, UploadGateResetReArmsEverySlotAfterDeviceRebuild)
{
    SphereSculptUploadGate gate;
    constexpr uint64_t kVersion = 7u;
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
        EXPECT_TRUE(gate.ShouldUpload(f, kVersion));

    // Quiescent: the store is already resident in every slot.
    for (uint32_t f = 0; f < kCBTFrameParamsRing * 4u; ++f)
        ASSERT_FALSE(gate.ShouldUpload(f, kVersion));

    gate.Reset();

    // The version has NOT changed — only the buffers died. Every slot must upload again.
    uint32_t reArmed = 0u;
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
        if (gate.ShouldUpload(f, kVersion))
            ++reArmed;
    EXPECT_EQ(reArmed, kCBTFrameParamsRing)
        << "post-rebuild slots did not re-upload; an edited planet would render unedited";

    // And the gate settles back to quiescent rather than uploading every frame forever.
    uint32_t idle = 0u;
    for (uint32_t f = 0; f < 200u; ++f)
        if (gate.ShouldUpload(f, kVersion))
            ++idle;
    EXPECT_EQ(idle, 0u) << "Reset left the gate permanently re-uploading";
}

// ----------------------------------------------------------------------------------------------
// PAGE-BOUNDARY CONTINUITY ORACLES (the #548 sparse virtual page-table contract).
//
// The sculpt resolve is per-TAP paged: SampleSculptFaceUV's four bilinear taps each call
// SculptResolveTexel, which recomputes (pageX,pageY,localX,localY) from the ABSOLUTE virtual texel.
// So a tap landing one texel past a page boundary reads the NEIGHBOUR page's texel — the sampled
// field stays C0 across every page edge with NO apron/gutter. These lock that invariant: the classic
// sparse-atlas seam bug (clamp a tap to its own page, or bake a per-page apron the writer forgets to
// refresh) would fail one of them. cbt_layout.glsl CBT_SampleSphereSculpt and cbt_surface.glsl
// CBT_SampleSculptDirSurf run byte-identical index math (CBTLayoutTests.GlslSculptPageConstantsMatchCpp
// locks the shared constants), so this CPU oracle guards the GPU resolve too.
namespace
{
// The exact additive height ApplyDab writes at virtual texel (face,x,y): a smoothstep of the TRUE
// angular distance to the (already normalised) dab direction — page-agnostic, so comparing it to the
// paged read independently verifies the store holds the right value at each texel across a boundary.
float AnalyticDabFalloff(uint32_t face, uint32_t x, uint32_t y, uint32_t dim, float ndx, float ndy,
                         float ndz, float angularRadius, float strength)
{
    const float invDimM1 = 1.0f / static_cast<float>(dim - 1);
    float dx, dy, dz;
    FaceUVToWorldDir(face, static_cast<float>(x) * invDimM1, static_cast<float>(y) * invDimM1, dx, dy,
                     dz);
    const float cosd = dx * ndx + dy * ndy + dz * ndz;
    if (cosd < std::cos(angularRadius))
        return 0.0f;
    const float ang = std::acos(std::clamp(cosd, -1.0f, 1.0f));
    const float t = std::clamp(1.0f - ang / angularRadius, 0.0f, 1.0f);
    return strength * (t * t * (3.0f - 2.0f * t));
}

// An R=2000 m planet: Dv=512 -> 4 pages/axis, page boundaries at texel 128/256/384.
SphereSculptGeometry GeomR2000()
{
    return MakeSphereSculptGeometry(2000.0f, ResolveSculptPagePoolCount());
}

// Max adjacent |dh| of a sub-texel scan of one face row across [txMin,txMax] (0.02-texel steps).
float MaxAdjacentJumpAcrossRow(const SphereSculptLayer& layer, uint32_t face, uint32_t dim, float txMin,
                              float txMax, float rowTexel)
{
    const float invDimM1 = 1.0f / static_cast<float>(dim - 1);
    const float v = rowTexel * invDimM1;
    float prev = 0.0f, maxJump = 0.0f;
    bool have = false;
    for (float tx = txMin; tx <= txMax; tx += 0.02f)
    {
        const float h = SampleFace(layer, face, tx * invDimM1, v);
        if (have)
            maxJump = std::fmax(maxJump, std::fabs(h - prev));
        prev = h;
        have = true;
    }
    return maxJump;
}
} // namespace

// Per-texel paged read == page-agnostic analytic falloff for every texel of a band straddling the
// texel-256 boundary — so SculptResolveTexel maps each texel to the right page id + local offset. A
// wrong page id or a clamped local index would diverge from the analytic value on one side of the edge.
TEST(SphereSculptPageBoundary, PagedResolveMatchesAnalyticFalloffAcrossBoundary)
{
    SphereSculptLayer layer;
    const SphereSculptGeometry g = GeomR2000();
    ASSERT_EQ(g.VirtualDim, 512u); // 4 pages/axis -> boundaries at 128/256/384
    layer.Configure(g);
    const uint32_t dim = g.VirtualDim;
    // Centre the dab OFF the boundary (texel ~300) so u=256 sits on the falloff SLOPE, not the
    // radially-symmetric peak (where both sides read ~equal and could hide a page-resolve bug).
    float cx, cy, cz;
    FaceUVToWorldDir(4u, 300.0f / (dim - 1), 256.0f / (dim - 1), cx, cy, cz);
    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float ndx = cx / clen, ndy = cy / clen, ndz = cz / clen;
    const float angR = 0.22f, strength = 20.0f;
    layer.ApplyDab(cx, cy, cz, angR, strength, false);
    const SphereFaceUV c = WorldDirToFaceUV(ndx, ndy, ndz);
    uint32_t nonzero = 0;
    for (uint32_t x = 248; x <= 264; ++x) // texels straddling the page boundary at 256, on the slope
    {
        const float paged = Texel(layer, c.Face, x, 256u);
        const float analytic = AnalyticDabFalloff(c.Face, x, 256u, dim, ndx, ndy, ndz, angR, strength);
        EXPECT_NEAR(paged, analytic, 1e-3f) << "texel " << x << " (page boundary at 256)";
        if (analytic > 0.01f)
            ++nonzero;
    }
    ASSERT_GT(nonzero, 8u) << "the band must lie on the non-zero slope for the oracle to bite";
}

// The bilinear midpoint between the last texel of page-col 1 (255) and the first of page-col 2 (256)
// equals their mean — proving the +u tap CROSSED into the neighbour page. A clamp-to-page resolve
// would read texel 255 for both taps and return h255, a seam of 0.5*(h256-h255).
TEST(SphereSculptPageBoundary, BilinearMidpointMixesBothPages)
{
    SphereSculptLayer layer;
    const SphereSculptGeometry g = GeomR2000();
    layer.Configure(g);
    const uint32_t dim = g.VirtualDim;
    float cx, cy, cz;
    FaceUVToWorldDir(4u, 300.0f / (dim - 1), 256.0f / (dim - 1), cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.22f, 20.0f, false);
    const SphereFaceUV c = WorldDirToFaceUV(cx, cy, cz);
    const float h255 = Texel(layer, c.Face, 255u, 256u); // page col 1
    const float h256 = Texel(layer, c.Face, 256u, 256u); // page col 2
    ASSERT_GT(std::fabs(h256 - h255), 1e-2f) << "boundary must sit on the slope (both pages non-flat)";
    const float uMid = 255.5f / static_cast<float>(dim - 1);
    const float vRow = 256.0f / static_cast<float>(dim - 1); // fy = 0 -> pure u interpolation
    const float sampled = SampleFace(layer, c.Face, uMid, vRow);
    EXPECT_NEAR(sampled, 0.5f * (h255 + h256), 1e-3f)
        << "cross-page bilinear must mix page1[255] and page2[256]; a clamp would give " << h255;
}

// No cliff at a page edge on the slope: a dense sub-texel scan across texel 256 never jumps more than a
// small fraction of the local per-texel slope (a clamp cliff spikes to ~0.5*slope).
TEST(SphereSculptPageBoundary, C0ContinuousScanAcrossBoundary)
{
    SphereSculptLayer layer;
    const SphereSculptGeometry g = GeomR2000();
    layer.Configure(g);
    const uint32_t dim = g.VirtualDim;
    float cx, cy, cz;
    FaceUVToWorldDir(4u, 300.0f / (dim - 1), 256.0f / (dim - 1), cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.22f, 20.0f, false);
    const SphereFaceUV c = WorldDirToFaceUV(cx, cy, cz);
    const float slope = std::fabs(Texel(layer, c.Face, 256u, 256u) - Texel(layer, c.Face, 255u, 256u));
    ASSERT_GT(slope, 1e-2f);
    const float maxJump = MaxAdjacentJumpAcrossRow(layer, c.Face, dim, 250.0f, 262.0f, 256.0f);
    // Adjacent 0.02-texel steps move ~0.02*slope; a page-edge cliff jumps ~0.5*slope. 0.1*slope sits
    // an order of magnitude under the cliff and comfortably above the smooth step.
    EXPECT_LT(maxJump, 0.1f * slope) << "page-boundary cliff: adjacent jump " << maxJump;
}

// A dab whose falloff decays to 0 before the texel-256 boundary leaves the neighbour page unallocated
// (reads additive 0). The allocated->unallocated transition must not cliff — the falloff is ~0 there.
TEST(SphereSculptPageBoundary, UnallocatedNeighbourEdgeNoCliff)
{
    SphereSculptLayer layer;
    const SphereSculptGeometry g = GeomR2000();
    layer.Configure(g);
    const uint32_t dim = g.VirtualDim;
    float cx, cy, cz; // centre near texel 160, small radius -> falloff dies well before 256
    FaceUVToWorldDir(4u, 160.0f / (dim - 1), 256.0f / (dim - 1), cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.05f, 20.0f, false);
    const SphereFaceUV c = WorldDirToFaceUV(cx, cy, cz);
    ASSERT_LT(layer.AllocatedPageCountForFace(c.Face), 4u) << "small dab must leave neighbour pages free";
    const float maxJump = MaxAdjacentJumpAcrossRow(layer, c.Face, dim, 240.0f, 300.0f, 256.0f);
    EXPECT_LT(maxJump, 1e-3f) << "allocated->unallocated page edge cliffed by " << maxJump;
}

// A dab centred EXACTLY on the u=256|v=256 four-page corner materialises all straddled pages and stays
// C0 through the shared edges — the writer never leaves the far side of a straddled boundary unallocated.
TEST(SphereSculptPageBoundary, DabStraddlingFourPageCornerIsContinuous)
{
    SphereSculptLayer layer;
    const SphereSculptGeometry g = GeomR2000();
    layer.Configure(g);
    const uint32_t dim = g.VirtualDim;
    float cx, cy, cz;
    FaceUVToWorldDir(4u, 256.0f / (dim - 1), 256.0f / (dim - 1), cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.12f, 20.0f, false);
    const SphereFaceUV c = WorldDirToFaceUV(cx, cy, cz);
    // Texels on both sides of both boundaries near the peak are non-zero -> all four pages are live.
    EXPECT_GT(Texel(layer, c.Face, 255u, 255u), 1.0f);
    EXPECT_GT(Texel(layer, c.Face, 256u, 255u), 1.0f);
    EXPECT_GT(Texel(layer, c.Face, 255u, 256u), 1.0f);
    EXPECT_GT(Texel(layer, c.Face, 256u, 256u), 1.0f);
    const float maxJumpU = MaxAdjacentJumpAcrossRow(layer, c.Face, dim, 250.0f, 262.0f, 256.0f);
    EXPECT_LT(maxJumpU, 0.05f) << "straddling dab cliffed across its own centre boundary: " << maxJumpU;
}

// ---- Planet-resize content REMAP oracles (sculpt-content remap slice) ----
// A live radius edit re-derives the radius-scaled Dv while authored content exists; Configure
// REMAPS the content onto the new grid: angular position preserved (the sculpt is a function of
// world direction), amplitudes preserved in absolute METRES (heights are authored in metres —
// resizing a planet must not rescale a mountain's height, matching the planar heightfield whose
// heights don't scale with terrain size). Refused only when the resampled content cannot fit the
// physical page pool — then the old geometry + content stay byte-identical (no loss, ever).

// GROW (2 km -> 20 km): content stays at its angular position at metre amplitude; the resample
// error of upsampling a smoothstep dab is far under the brush's authored scale.
TEST(SphereSculptRemap, GrowKeepsAngularContentAndMetreAmplitude)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    const float strength = 20.0f;
    const float ang = 0.05f;
    float cx, cy, cz;
    FaceUVToWorldDir(0u, 0.35f, 0.40f, cx, cy, cz); // centred well inside +X, away from page edges
    layer.ApplyDab(cx, cy, cz, ang, strength, false);

    // Probe directions: dab centre, half-radius ring, just inside the rim, and outside the cap.
    auto probe = [&](float offU, float offV) {
        float dx, dy, dz;
        FaceUVToWorldDir(0u, 0.35f + offU, 0.40f + offV, dx, dy, dz);
        return SampleSphereSculptByDir(layer.MakeSampler(), dx, dy, dz);
    };
    const float centreBefore = probe(0.0f, 0.0f);
    const float midBefore = probe(0.015f, 0.0f);
    const float rimBefore = probe(0.0f, 0.028f);
    ASSERT_GT(centreBefore, 0.9f * strength) << "dab did not land";
    ASSERT_GT(midBefore, 0.0f);
    const uint32_t versionBefore = layer.Version();

    const SphereEditRegions remapped = layer.Configure(MakeSphereSculptGeometry(20000.0f, 64u));
    EXPECT_EQ(layer.Geometry().VirtualDim, DeriveSculptVirtualDim(20000.0f));
    EXPECT_EQ(layer.Version(), versionBefore + 1u) << "a remap IS an edit (upload gate + re-tess key on it)";
    ASSERT_GT(remapped.Count, 0u) << "remap must publish covering regions";
    EXPECT_EQ(remapped.Rects[0].Face, 0u);

    EXPECT_NEAR(probe(0.0f, 0.0f), centreBefore, 0.02f * strength)
        << "dab peak must stay at its angular position at its metre amplitude";
    EXPECT_NEAR(probe(0.015f, 0.0f), midBefore, 0.05f * strength);
    EXPECT_NEAR(probe(0.0f, 0.028f), rimBefore, 0.05f * strength);
    EXPECT_EQ(probe(0.25f, 0.25f), 0.0f) << "outside the cap must stay exactly 0 (sparse)";
}

// SHRINK (20 km -> 2 km): downsampling keeps the dab at its angular position and metre amplitude
// within the coarser grid's resample error.
TEST(SphereSculptRemap, ShrinkKeepsContentWithinResampleError)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(20000.0f, 64u));
    const float strength = 20.0f;
    float cx, cy, cz;
    FaceUVToWorldDir(2u, 0.60f, 0.55f, cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.05f, strength, false);
    auto probe = [&](float offU, float offV) {
        float dx, dy, dz;
        FaceUVToWorldDir(2u, 0.60f + offU, 0.55f + offV, dx, dy, dz);
        return SampleSphereSculptByDir(layer.MakeSampler(), dx, dy, dz);
    };
    const float centreBefore = probe(0.0f, 0.0f);
    ASSERT_GT(centreBefore, 0.9f * strength);

    const SphereEditRegions remapped = layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    EXPECT_EQ(layer.Geometry().VirtualDim, DeriveSculptVirtualDim(2000.0f));
    ASSERT_GT(remapped.Count, 0u);
    EXPECT_NEAR(probe(0.0f, 0.0f), centreBefore, 0.05f * strength);
    EXPECT_EQ(probe(0.3f, 0.3f), 0.0f);
}

// CRACK ORACLE through a remap: a dab straddling the +X/+Y cube edge stays seam-free after the
// resize — both faces resample their own bands from shared-edge columns that agreed, and the
// endpoint-exact texel<->UV mapping keeps the shared directions sampling identical values.
TEST(SphereSculptRemap, CrossFaceSharedEdgeStaysCrackFree)
{
    SphereSculptLayer layer;
    // Production-sized pool (256): two wide cross-edge dabs upsampled x7.8 span ~90 new pages.
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 256u));
    layer.ApplyDab(1.0f, 0.85f, 0.0f, 0.12f, 20.0f, false);
    layer.ApplyDab(0.85f, 1.0f, 0.10f, 0.12f, 18.0f, false);

    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(20000.0f, 256u)).Count, 0u);

    uint32_t edited = 0;
    for (int s = -5; s <= 5; ++s)
    {
        const float t = 0.08f * static_cast<float>(s);
        const float dir[3] = {1.0f, 1.0f, t};
        float uA, vA, uB, vB;
        ASSERT_TRUE(ProjectDirOntoFace(0u, dir[0], dir[1], dir[2], 1e-3f, uA, vA));
        ASSERT_TRUE(ProjectDirOntoFace(2u, dir[0], dir[1], dir[2], 1e-3f, uB, vB));
        const float sA = SampleFace(layer, 0u, uA, vA);
        const float sB = SampleFace(layer, 2u, uB, vB);
        EXPECT_NEAR(sA, sB, 1e-3f) << "remapped shared cube-edge value disagrees (t=" << t << ")";
        if (std::fabs(sA) > 1e-3f)
            ++edited;
    }
    EXPECT_GT(edited, 0u) << "the dabs did not actually cover the shared edge";
}

// REFUSAL ORACLE: when the resampled content would need more physical pages than the pool holds
// (a large radius growth), the remap is refused and the store is byte-identical to before — no
// authored height is ever lost to a resize.
TEST(SphereSculptRemap, RefusedWhenPoolCannotHoldRemappedContent)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(256u, 4u)); // Dv 256 (2 pages/axis), pool of only 4 pages
    layer.ApplyDab(1.0f, 0.05f, 0.05f, 0.30f, 20.0f, false); // wide cap: fills the +X face's pages
    ASSERT_GT(layer.AllocatedPageCount(), 0u);
    const std::vector<float> poolBefore = layer.Pool();
    const std::vector<uint32_t> tableBefore = layer.PageTable();
    const uint32_t versionBefore = layer.Version();

    // x16 the virtual area on the same 4-page pool: the remap cannot fit and must refuse.
    const SphereEditRegions remapped = layer.Configure(Geom(1024u, 4u));
    EXPECT_EQ(remapped.Count, 0u);
    EXPECT_EQ(layer.Geometry().VirtualDim, 256u) << "refused remap keeps the authored resolution";
    EXPECT_EQ(layer.Version(), versionBefore) << "refused remap is not an edit";
    EXPECT_EQ(layer.Pool(), poolBefore) << "refused remap must leave the pool byte-identical";
    EXPECT_EQ(layer.PageTable(), tableBefore);
}

// LAYER-SPLIT ORACLE: dab and modifier remap into their own layers — after the resize a full
// modifier re-bake (SET semantics, clear-first) still composes against the REMAPPED dab, exactly
// as it would against an un-remapped one.
TEST(SphereSculptRemap, DabAndModifierLayersRemapIndependently)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(256u, 16u));
    float cx, cy, cz;
    FaceUVToWorldDir(0u, 0.50f, 0.50f, cx, cy, cz);
    layer.ApplyDab(cx, cy, cz, 0.05f, 10.0f, false);
    layer.BakeModifierLayer(FaceRect(0u, 0.30f, 0.30f, 0.70f, 0.70f),
                            [](float, float, float) { return 5.0f; });
    auto centre = [&] {
        return SampleSphereSculptByDir(layer.MakeSampler(), cx, cy, cz);
    };
    const float composedBefore = centre(); // dab peak + 5
    ASSERT_GT(composedBefore, 14.0f);

    ASSERT_GT(layer.Configure(Geom(512u, 64u)).Count, 0u);
    EXPECT_NEAR(centre(), composedBefore, 0.5f) << "published = dab + modifier must survive the remap";

    // A full re-bake with an empty footprint clears the ENTIRE modifier layer; the remapped dab
    // must remain untouched underneath.
    layer.BakeModifierFull(SphereEditRegions{}, [](float, float, float) { return 0.0f; });
    EXPECT_NEAR(centre(), composedBefore - 5.0f, 0.5f)
        << "clearing the modifier layer after a remap must leave exactly the remapped dab";
}

// Reconfiguring to the SAME derived geometry is a no-op (the per-frame ConfigurePlanetSculpt call
// pattern): no version advance, no regions, no content churn.
TEST(SphereSculptRemap, SameGeometryIsNoOp)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 20.0f, false);
    const uint32_t versionBefore = layer.Version();
    const SphereEditRegions r = layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    EXPECT_EQ(r.Count, 0u);
    EXPECT_EQ(layer.Version(), versionBefore);
}
