// Sculpt-store SHAPE ACCURACY probes + the analytic-modifier exact-circle oracle.
// Two suites:
//
//   * SphereSculptShapeAccuracy — the FAILS-BEFORE measurement probes for the report "brush strokes
//     and flatten modifiers cannot make accurate circular shapes at radius <= 100 m on planets".
//     A circular flatten (closed form) is baked through the REAL runtime store geometry
//     (radius-scaled Dv, 128-page cap) and the reconstructed edge is compared against the exact
//     circle. These assert properties of the STORE path (texel-scale error, sub-texel
//     non-representability at planet radii) that remain true after the analytic cure lands — the
//     regression baseline the cures must beat, with the numbers printed for the report.
//
//   * SphereAnalyticModifiers — the S1 prototype oracle: the SAME closed form evaluated analytically
//     through the composition chokepoint (SampleSphereSculptComposed) reconstructs the exact circle
//     at ANY radius (edge error bounded by the scan step, << 1 store texel), composes additively
//     with the dab layer, cancels relief exactly (the physics-composition parity property), and is
//     bit-identical to the plain store sample when the set is empty (dark-ship).
//
// Device-free like the layer itself; every measurement is deterministic fp math.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrain/SphereAnalyticModifiers.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"

using namespace GameEngine::CBTTerrain;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEarthRadius = 6.371e6f;
constexpr float kPlateauH = 10.0f; // plateau height (metres) all shape probes use

std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// The store geometry the RUNTIME derives for a planet radius (ConfigurePlanetSculpt's arithmetic):
// radius-scaled virtual dim at the 8 m/texel budget, clamped by the 128-page cap; default pool.
SphereSculptGeometry RuntimeGeometry(float planetRadius)
{
    return MakeSphereSculptGeometry(planetRadius,
                                    DeriveSculptPagePoolCount(kSculptPagePoolBudgetBytes));
}

// Bake a circular analytic flatten into the store's modifier layer exactly like
// TerrainModifierSystem::BakeSphereModifiers: dirty region from ClassifySphereCapEdit over the
// footprint (`inflate` 1.0 = the shipping classification; > 1 widens the rect to separate the
// classification's toward-edge undershoot from pure texel quantization), then BakeModifierLayer
// SETs evalOffset(worldDir) at every texel centre of the rect — the rasterization under test.
void BakeFlattenIntoStore(SphereSculptLayer& layer, const SphereAnalyticFlatten& f, float inflate)
{
    const float ang =
        std::min((f.Radius + std::max(f.Falloff, 0.0f)) / f.PlanetRadius * inflate, kPi);
    const SphereEditRegions regions = ClassifySphereCapEdit(f.N[0], f.N[1], f.N[2], ang);
    layer.BakeModifierLayer(regions, [&f](float dx, float dy, float dz)
                            { return EvaluateSphereAnalyticFlatten(f, dx, dy, dz, 0.0f); });
}

struct EdgeMeasurement
{
    float MaxErrM = 0.0f;      // max |reconstructed edge - exact circle| over azimuths, metres
    float MeanErrM = 0.0f;     // mean of the same
    float MinEdgeM = 1e30f;    // innermost reconstructed edge (a classification clip shows here)
    float MaxEdgeM = -1e30f;   // outermost reconstructed edge (the bilinear smear shows here)
    float PlateauH = 0.0f;     // reconstructed height at the disc centre
    bool Representable = false; // centre reaches at least half the authored plateau
};

// Reconstruct the disc edge along kAzimuths great-circle rays from the centre direction `n`:
// march from outside (brushR + scanPadM) inward at stepM and record the first crossing of the
// half-plateau height. err(azimuth) = |crossing - brushR|; a ray that never crosses scores the
// full brushR (the shape is absent along that ray).
template <typename SampleFn>
EdgeMeasurement MeasureCircleEdge(SampleFn&& sample, const std::array<float, 3>& n,
                                  float planetRadius, float brushR, float scanPadM, float stepM)
{
    const std::array<float, 3> t1 = AnyTangent(n);
    const std::array<float, 3> t2 = Cross(n, t1);

    EdgeMeasurement m{};
    m.PlateauH = sample(n[0], n[1], n[2]);
    m.Representable = m.PlateauH >= 0.5f * kPlateauH;

    constexpr int kAzimuths = 64;
    double errSum = 0.0;
    for (int az = 0; az < kAzimuths; ++az)
    {
        const float phi = (2.0f * kPi * static_cast<float>(az)) / static_cast<float>(kAzimuths);
        const float cp = std::cos(phi), sp = std::sin(phi);
        const std::array<float, 3> t = {cp * t1[0] + sp * t2[0], cp * t1[1] + sp * t2[1],
                                        cp * t1[2] + sp * t2[2]};
        float edgeM = 0.0f;
        const int steps = static_cast<int>(std::ceil((brushR + scanPadM) / stepM));
        for (int i = steps; i >= 0; --i)
        {
            const float distM = static_cast<float>(i) * stepM;
            const float a = distM / planetRadius;
            const float ca = std::cos(a), sa = std::sin(a);
            const float dx = ca * n[0] + sa * t[0];
            const float dy = ca * n[1] + sa * t[1];
            const float dz = ca * n[2] + sa * t[2];
            if (sample(dx, dy, dz) >= 0.5f * kPlateauH)
            {
                edgeM = distM;
                break;
            }
        }
        const float err = std::fabs(edgeM - brushR);
        m.MaxErrM = std::max(m.MaxErrM, err);
        m.MinEdgeM = std::min(m.MinEdgeM, edgeM);
        m.MaxEdgeM = std::max(m.MaxEdgeM, edgeM);
        errSum += err;
    }
    m.MeanErrM = static_cast<float>(errSum / kAzimuths);
    return m;
}

// Measure the STORE path: bake the flatten (shipping classification unless inflated), reconstruct
// through the published bilinear sample. `n` is the +X face centre so the disc is face-interior.
EdgeMeasurement MeasureStoreFlatten(float planetRadius, float brushR, float falloffM, float inflate)
{
    const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
    const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
        n[0] * planetRadius, 0.0f, 0.0f, brushR, falloffM, planetRadius + kPlateauH, planetRadius);
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    BakeFlattenIntoStore(layer, f, inflate);
    EXPECT_FALSE(layer.PoolExhausted());

    const float texelM = SculptMetersPerTexel(planetRadius, layer.Geometry().VirtualDim);
    const float stepM = std::min(brushR, texelM) / 256.0f;
    const float scanPadM = falloffM + 4.0f * texelM;
    return MeasureCircleEdge(
        [&layer](float dx, float dy, float dz)
        { return SampleSphereSculptByDir(layer.MakeSampler(), dx, dy, dz); },
        n, planetRadius, brushR, scanPadM, stepM);
}

void PrintMeasurement(const char* tag, float planetRadius, float brushR, float texelM,
                      const EdgeMeasurement& m)
{
    std::printf("[shape-accuracy] %s R=%.0f B=%.1f texel=%.3fm maxErr=%.3fm (%.1f%% of B) "
                "meanErr=%.3fm edge=[%.3f..%.3f]m plateau=%.3fm representable=%d\n",
                tag, planetRadius, brushR, texelM, m.MaxErrM, 100.0f * m.MaxErrM / brushR,
                m.MeanErrM, m.MinEdgeM, m.MaxEdgeM, m.PlateauH, m.Representable ? 1 : 0);
}

} // namespace

// ---- Suite 1: the fails-before probes (store path) ---------------------------------------------

// The texel-budget arithmetic behind the report, asserted exactly: the 8 m/texel budget holds to
// the 128-page cap (~83 km), beyond which m/texel grows linearly with radius — at Earth a 100 m
// brush radius is 0.16 store texels (sub-texel: not representable as a shape at all).
TEST(SphereSculptShapeAccuracy, TexelBudgetCurveMatchesDerivation)
{
    struct Row
    {
        float R;
        uint32_t ExpectDv;
        float ExpectMpt;     // metres per texel
        float ExpectB100Tex; // 100 m brush radius in texels
        float ExpectB5Tex;   // 5 m brush radius in texels
    };
    const Row rows[] = {
        {2000.0f, 512u, 6.1359f, 16.297f, 0.8149f},
        {50000.0f, 9856u, 7.9687f, 12.549f, 0.6275f},
        {kEarthRadius, 16384u, 610.81f, 0.16372f, 0.0081859f},
    };
    for (const Row& row : rows)
    {
        const uint32_t dv = DeriveSculptVirtualDim(row.R);
        EXPECT_EQ(dv, row.ExpectDv) << "R=" << row.R;
        const float mpt = SculptMetersPerTexel(row.R, dv);
        EXPECT_NEAR(mpt, row.ExpectMpt, row.ExpectMpt * 1e-3f) << "R=" << row.R;
        EXPECT_NEAR(100.0f / mpt, row.ExpectB100Tex, row.ExpectB100Tex * 1e-3f) << "R=" << row.R;
        EXPECT_NEAR(5.0f / mpt, row.ExpectB5Tex, row.ExpectB5Tex * 1e-3f) << "R=" << row.R;
        std::printf("[shape-accuracy] budget R=%.0f Dv=%u m/texel=%.3f B100=%.2ftex B5=%.3ftex\n",
                    row.R, dv, mpt, 100.0f / mpt, 5.0f / mpt);
    }
}

// A 100 m flatten where the budget holds (R=2000 / R=50000): the store CAN represent the disc, but
// the edge error is TEXEL-scale (6-8 m — 6-13% of the brush radius), never metre-exact. This is the
// polygonal-circle report quantified, and it is a property of the store path that stays true after
// the analytic cure (which bypasses the store, not fixes it).
TEST(SphereSculptShapeAccuracy, StoreFlatten100mEdgeErrorIsTexelScale)
{
    for (const float planetRadius : {2000.0f, 50000.0f})
    {
        const float brushR = 100.0f;
        const float texelM = SculptMetersPerTexel(planetRadius, DeriveSculptVirtualDim(planetRadius));
        const EdgeMeasurement m = MeasureStoreFlatten(planetRadius, brushR, 0.0f, 1.0f);
        PrintMeasurement("store", planetRadius, brushR, texelM, m);
        EXPECT_TRUE(m.Representable);
        // Bilinear reconstruction of a hard edge lands the half-height crossing within roughly a
        // texel; the shipping classification's toward-edge undershoot can clip up to ~0.13*B more.
        EXPECT_LT(m.MaxErrM, 2.0f * texelM + 0.15f * brushR);
        // The fails-before floor: the error is REAL (a substantial fraction of a texel), not
        // sub-centimetre. If the store path ever becomes shape-exact this flips and the probe
        // must be re-derived.
        EXPECT_GT(m.MaxErrM, 0.15f * texelM);
    }
}

// Decompose classification clip vs pure quantization: re-bake with an inflated dirty rect (the
// per-texel cap test decides writes, so inflation only removes the rect clip). The remaining error
// is pure texel quantization; the delta is ClassifySphereCapEdit's toward-edge undershoot.
TEST(SphereSculptShapeAccuracy, StoreFlatten100mClassificationClipVsQuantization)
{
    const float planetRadius = 50000.0f;
    const float brushR = 100.0f;
    const float texelM = SculptMetersPerTexel(planetRadius, DeriveSculptVirtualDim(planetRadius));
    const EdgeMeasurement shipping = MeasureStoreFlatten(planetRadius, brushR, 0.0f, 1.0f);
    const EdgeMeasurement inflated = MeasureStoreFlatten(planetRadius, brushR, 0.0f, 1.35f);
    PrintMeasurement("store-shipping", planetRadius, brushR, texelM, shipping);
    PrintMeasurement("store-inflated", planetRadius, brushR, texelM, inflated);
    // With the rect clip removed the error is bounded by bilinear texel quantization alone.
    EXPECT_TRUE(inflated.Representable);
    EXPECT_LT(inflated.MaxErrM, 2.0f * texelM);
    // And it is still texel-scale — quantization, not classification, is the accuracy floor.
    EXPECT_GT(inflated.MaxErrM, 0.15f * texelM);
}

// A 5 m flatten at R=50000: 0.63 texel radius — below one store texel the disc is not a shape any
// more (either absent, or a 1-2 texel blob whose edge is wrong by a large fraction of B).
TEST(SphereSculptShapeAccuracy, StoreFlatten5mAtR50000IsSubTexel)
{
    const float planetRadius = 50000.0f;
    const float brushR = 5.0f;
    const float texelM = SculptMetersPerTexel(planetRadius, DeriveSculptVirtualDim(planetRadius));
    ASSERT_LT(brushR, texelM); // the premise: sub-texel brush
    const EdgeMeasurement m = MeasureStoreFlatten(planetRadius, brushR, 0.0f, 1.0f);
    PrintMeasurement("store", planetRadius, brushR, texelM, m);
    EXPECT_TRUE(!m.Representable || m.MaxErrM > 0.4f * brushR)
        << "a sub-texel disc reconstructed accurately would falsify the store-quantization "
           "diagnosis";
}

// The Earth-radius headline: at 610.8 m/texel a 100 m flatten is 0.16 texels of radius. Whatever
// the bake writes (nothing, or one texel that bilinear-smears to a ~1.2 km-wide mound), the
// reconstruction is wrong by MANY multiples of the brush radius — her "100 m dab becomes a ~1 km
// mound" observation as a committed number.
TEST(SphereSculptShapeAccuracy, StoreFlatten100mAtEarthIsNotRepresentable)
{
    const float planetRadius = kEarthRadius;
    const float brushR = 100.0f;
    const float texelM = SculptMetersPerTexel(planetRadius, DeriveSculptVirtualDim(planetRadius));
    ASSERT_GT(texelM, brushR); // the premise: the whole brush fits inside a fraction of one texel
    const EdgeMeasurement m = MeasureStoreFlatten(planetRadius, brushR, 0.0f, 1.0f);
    PrintMeasurement("store", planetRadius, brushR, texelM, m);
    if (m.Representable)
    {
        // A texel centre landed inside the cap: the disc reconstructs as a texel-wide mound whose
        // edge sits hundreds of metres out — report the measured mound width.
        EXPECT_GT(m.MaxErrM, 2.0f * brushR);
        std::printf("[shape-accuracy] earth mound full width ~%.0f m for a %.0f m brush\n",
                    2.0f * m.MaxEdgeM, brushR);
    }
    else
    {
        // No texel centre inside the cap: the flatten wrote nothing the sampler can see.
        EXPECT_LT(m.PlateauH, 0.5f * kPlateauH);
    }
}

// The other Earth-radius failure mode: centre the same 100 m flatten exactly ON a texel-centre
// direction so the bake catches that texel. The disc then reconstructs as a bilinear mound one
// texel high and TWO texels wide at the base — a ~1.2 km-wide, 610 m-half-width feature for a
// 100 m brush ("a 100 m dab becomes a ~1 km mound"). Together with the vanishing case above these
// are the only two shapes a sub-texel brush can produce.
TEST(SphereSculptShapeAccuracy, StoreFlatten100mAtEarthOnTexelCentreBecomesKmMound)
{
    const float planetRadius = kEarthRadius;
    const float brushR = 100.0f;
    const uint32_t dv = DeriveSculptVirtualDim(planetRadius);
    const float texelM = SculptMetersPerTexel(planetRadius, dv);
    ASSERT_GT(texelM, brushR);

    // A texel-centre direction on face 0 (texel dv/2 of the (dv-1)-span grid).
    const float uv = static_cast<float>(dv / 2u) / static_cast<float>(dv - 1u);
    std::array<float, 3> n{};
    FaceUVToWorldDir(0u, uv, uv, n[0], n[1], n[2]);
    const SphereAnalyticFlatten f =
        MakeSphereAnalyticFlatten(n[0] * planetRadius, n[1] * planetRadius, n[2] * planetRadius,
                                  brushR, 0.0f, planetRadius + kPlateauH, planetRadius);
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    BakeFlattenIntoStore(layer, f, 1.0f);
    EXPECT_FALSE(layer.PoolExhausted());

    const EdgeMeasurement m = MeasureCircleEdge(
        [&layer](float dx, float dy, float dz)
        { return SampleSphereSculptByDir(layer.MakeSampler(), dx, dy, dz); },
        n, planetRadius, brushR, 4.0f * texelM, texelM / 256.0f);
    PrintMeasurement("store-texelhit", planetRadius, brushR, texelM, m);
    std::printf("[shape-accuracy] earth mound half-height full width ~%.0f m, base width ~%.0f m, "
                "for a %.0f m brush\n",
                2.0f * m.MaxEdgeM, 2.0f * (m.MaxEdgeM + texelM), brushR);
    EXPECT_TRUE(m.Representable); // the mound exists...
    EXPECT_GT(m.MaxErrM, 2.0f * brushR); // ...but its edge is off by multiples of the brush radius
}

// ---- Suite 2: the analytic prototype oracle -----------------------------------------------------

// Exact-circle oracle: the SAME closed form, evaluated analytically through the composition
// chokepoint over an empty store, reconstructs the disc edge to scan-step accuracy — far below one
// store texel and below 1% of the brush radius — at R=50000 AND at Earth radius, for both the 100 m
// and the sub-texel 5 m brush. This is the cure the store path cannot reach at any radius.
TEST(SphereAnalyticModifiers, AnalyticFlattenReconstructsExactCircleAtAnyRadius)
{
    struct Case
    {
        float R;
        float B;
    };
    const Case cases[] = {{50000.0f, 100.0f}, {50000.0f, 5.0f}, {kEarthRadius, 100.0f}};
    for (const Case& c : cases)
    {
        const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
        const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
            n[0] * c.R, 0.0f, 0.0f, c.B, 0.0f, c.R + kPlateauH, c.R);
        const SphereAnalyticModifierSet set{&f, 1u};

        SphereSculptLayer layer;
        layer.Configure(RuntimeGeometry(c.R)); // empty store: pages all unallocated
        const float texelM = SculptMetersPerTexel(c.R, layer.Geometry().VirtualDim);
        const float stepM = c.B / 512.0f;
        const EdgeMeasurement m = MeasureCircleEdge(
            [&layer, &set](float dx, float dy, float dz) {
                return SampleSphereSculptComposed(layer.MakeSampler(), set, dx, dy, dz, 0.0f);
            },
            n, c.R, c.B, 0.25f * c.B, stepM);
        PrintMeasurement("analytic", c.R, c.B, texelM, m);
        EXPECT_TRUE(m.Representable);
        EXPECT_LT(m.MaxErrM, 0.01f * c.B) << "R=" << c.R << " B=" << c.B;
        EXPECT_LT(m.MaxErrM, texelM) << "R=" << c.R << " B=" << c.B;
        EXPECT_NEAR(m.PlateauH, kPlateauH, 1e-3f);
    }
}

// The analytic term composes ADDITIVELY with the store's dab layer through the chokepoint: the
// flatten contributes only inside its footprint, the dab only where it was painted, and neither
// disturbs the other's contribution.
TEST(SphereAnalyticModifiers, AnalyticComposesWithDabLayer)
{
    const float planetRadius = 50000.0f;
    const float brushR = 100.0f;
    const std::array<float, 3> padDir = {1.0f, 0.0f, 0.0f};
    const SphereAnalyticFlatten f =
        MakeSphereAnalyticFlatten(padDir[0] * planetRadius, 0.0f, 0.0f, brushR, 0.0f,
                                  planetRadius + kPlateauH, planetRadius);
    const SphereAnalyticModifierSet set{&f, 1u};

    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    // A freehand dab well clear of the pad: centre 500 m away (0.01 rad), 200 m brush — a 200 m
    // gap between the dab's reach and the pad edge.
    const float dabAng = 0.01f;
    const std::array<float, 3> dabDir = {std::cos(dabAng), std::sin(dabAng), 0.0f};
    layer.ApplyDab(dabDir[0], dabDir[1], dabDir[2], 200.0f / planetRadius, 5.0f, false);

    const SphereSculptSampler s = layer.MakeSampler();
    // Pad centre: analytic only (the dab layer reads 0 there).
    EXPECT_EQ(SampleSphereSculptByDir(s, padDir[0], padDir[1], padDir[2]), 0.0f);
    EXPECT_NEAR(SampleSphereSculptComposed(s, set, padDir[0], padDir[1], padDir[2], 0.0f),
                kPlateauH, 1e-4f);
    // Dab centre: store only (outside the pad the analytic term is 0).
    const float dabStore = SampleSphereSculptByDir(s, dabDir[0], dabDir[1], dabDir[2]);
    EXPECT_GT(dabStore, 1.0f);
    EXPECT_EQ(SampleSphereSculptComposed(s, set, dabDir[0], dabDir[1], dabDir[2], 0.0f), dabStore);
}

// Physics-composition parity: through the collider's formula (surface radius = planetRadius +
// closed-form relief + sculpt sample) the analytic flatten cancels the relief and lands the pad
// EXACTLY on TargetRadius — the flat pad property, from the same closed form the GPU twin will
// compose (fp32-shared math is the S2 bit-lock statement). The store path cannot do this: it holds
// relief cancellation only at baked texel centres.
TEST(SphereAnalyticModifiers, AnalyticFlattenCancelsReliefInPhysicsComposition)
{
    const float planetRadius = 50000.0f;
    const float brushR = 100.0f;
    const float targetRadius = planetRadius + kPlateauH;
    const float amp = 5.0f, freq = 4.0f;
    const uint32_t oct = 3u;
    const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
    const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
        n[0] * planetRadius, 0.0f, 0.0f, brushR, 0.0f, targetRadius, planetRadius);
    const SphereAnalyticModifierSet set{&f, 1u};

    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    const SphereSculptSampler s = layer.MakeSampler();

    const std::array<float, 3> t1 = AnyTangent(n);
    const std::array<float, 3> t2 = Cross(n, t1);
    for (int az = 0; az < 8; ++az)
    {
        const float phi = (2.0f * kPi * static_cast<float>(az)) / 8.0f;
        for (const float frac : {0.0f, 0.35f, 0.7f, 0.95f})
        {
            const float a = frac * brushR / planetRadius; // strictly inside the hard edge
            const float ca = std::cos(a), sa = std::sin(a);
            const float dx = ca * n[0] + sa * (std::cos(phi) * t1[0] + std::sin(phi) * t2[0]);
            const float dy = ca * n[1] + sa * (std::cos(phi) * t1[1] + std::sin(phi) * t2[1]);
            const float dz = ca * n[2] + sa * (std::cos(phi) * t1[2] + std::sin(phi) * t2[2]);
            const float relief = PlanetRelief(dx, dy, dz, amp, freq, oct);
            const float surfaceRadius =
                planetRadius + relief + SampleSphereSculptComposed(s, set, dx, dy, dz, relief);
            EXPECT_NEAR(surfaceRadius, targetRadius, 0.02f)
                << "az=" << az << " frac=" << frac << " relief=" << relief;
        }
    }
}

// Honest-falsification check: where the texel budget is fine relative to the brush (R=2000,
// 6.14 m/texel, 100 m brush, 20 m falloff skirt), the STORE bake reproduces the ANALYTIC closed
// form to bilinear-interpolation error (< 0.2 * plateau). The shape error at planet scale is store
// QUANTIZATION, not a falloff-function or bake-rasterization discrepancy — if the two paths
// diverged at fine texels, the analytic cure would be changing the shape, not curing the store.
TEST(SphereAnalyticModifiers, StoreBakeConvergesToAnalyticAtFineTexels)
{
    const float planetRadius = 2000.0f;
    const float brushR = 100.0f;
    const float falloffM = 20.0f;
    const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
    const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
        n[0] * planetRadius, 0.0f, 0.0f, brushR, falloffM, planetRadius + kPlateauH, planetRadius);
    const SphereAnalyticModifierSet set{&f, 1u};

    SphereSculptLayer baked;
    baked.Configure(RuntimeGeometry(planetRadius));
    BakeFlattenIntoStore(baked, f, 1.35f); // inflated rect: isolate quantization from the rect clip
    SphereSculptLayer empty;
    empty.Configure(RuntimeGeometry(planetRadius));

    const float texelM = SculptMetersPerTexel(planetRadius, baked.Geometry().VirtualDim);
    const std::array<float, 3> t1 = AnyTangent(n);
    const std::array<float, 3> t2 = Cross(n, t1);
    float maxDiff = 0.0f;
    for (int az = 0; az < 64; ++az)
    {
        const float phi = (2.0f * kPi * static_cast<float>(az)) / 64.0f;
        const float reachM = brushR + falloffM + 3.0f * texelM;
        for (float distM = 0.0f; distM <= reachM; distM += 0.25f * texelM)
        {
            const float a = distM / planetRadius;
            const float ca = std::cos(a), sa = std::sin(a);
            const float dx = ca * n[0] + sa * (std::cos(phi) * t1[0] + std::sin(phi) * t2[0]);
            const float dy = ca * n[1] + sa * (std::cos(phi) * t1[1] + std::sin(phi) * t2[1]);
            const float dz = ca * n[2] + sa * (std::cos(phi) * t1[2] + std::sin(phi) * t2[2]);
            const float store = SampleSphereSculptByDir(baked.MakeSampler(), dx, dy, dz);
            const float analytic =
                SampleSphereSculptComposed(empty.MakeSampler(), set, dx, dy, dz, 0.0f);
            maxDiff = std::max(maxDiff, std::fabs(store - analytic));
        }
    }
    std::printf("[shape-accuracy] convergence R=%.0f B=%.1f falloff=%.1f texel=%.3fm "
                "max|store-analytic|=%.3fm\n",
                planetRadius, brushR, falloffM, texelM, maxDiff);
    EXPECT_LT(maxDiff, 0.2f * kPlateauH);
}

// Dark-ship: an EMPTY analytic set composes bit-identically to the plain store sample — the inert
// default every existing consumer keeps until the live flag wiring (S2) opts in.
TEST(SphereAnalyticModifiers, EmptySetIsBitIdenticalToStoreSample)
{
    const float planetRadius = 50000.0f;
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    layer.ApplyDab(1.0f, 0.02f, 0.01f, 0.004f, 7.0f, false);
    layer.ApplyDab(0.0f, 1.0f, 0.0f, 0.002f, 3.0f, true);

    const SphereSculptSampler s = layer.MakeSampler();
    const SphereAnalyticModifierSet empty{};
    for (int i = 0; i < 400; ++i)
    {
        // Deterministic direction sweep covering edited and untouched faces.
        const float u = 0.05f + 0.9f * static_cast<float>(i % 20) / 19.0f;
        const float v = 0.05f + 0.9f * static_cast<float>(i / 20) / 19.0f;
        float dx, dy, dz;
        FaceUVToWorldDir(static_cast<uint32_t>(i) % 6u, u, v, dx, dy, dz);
        EXPECT_EQ(SampleSphereSculptComposed(s, empty, dx, dy, dz, 123.0f),
                  SampleSphereSculptByDir(s, dx, dy, dz));
    }
}

// The crease-residency twin: the retess residency gate must extend to "page-resident OR inside an
// analytic footprint" (design §3.a) — an analytic modifier allocates no pages, so without this the
// pad rim never crease-refines. Covers inside, the falloff skirt, and outside (with margin).
TEST(SphereAnalyticModifiers, FootprintCoversFootprintAndSkirtOnly)
{
    const float planetRadius = 50000.0f;
    const float brushR = 100.0f;
    const float falloffM = 30.0f;
    const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
    const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
        n[0] * planetRadius, 0.0f, 0.0f, brushR, falloffM, planetRadius + kPlateauH, planetRadius);
    const SphereAnalyticModifierSet set{&f, 1u};

    const std::array<float, 3> t1 = AnyTangent(n);
    auto dirAt = [&](float distM) {
        const float a = distM / planetRadius;
        return std::array<float, 3>{std::cos(a) * n[0] + std::sin(a) * t1[0],
                                    std::cos(a) * n[1] + std::sin(a) * t1[1],
                                    std::cos(a) * n[2] + std::sin(a) * t1[2]};
    };
    const std::array<float, 3> inside = dirAt(0.5f * brushR);
    const std::array<float, 3> skirt = dirAt(brushR + 0.5f * falloffM);
    const std::array<float, 3> outside = dirAt(brushR + falloffM + 25.0f);
    EXPECT_TRUE(SphereAnalyticFootprintCovers(set, inside[0], inside[1], inside[2]));
    EXPECT_TRUE(SphereAnalyticFootprintCovers(set, skirt[0], skirt[1], skirt[2]));
    EXPECT_FALSE(SphereAnalyticFootprintCovers(set, outside[0], outside[1], outside[2]));
    EXPECT_FALSE(SphereAnalyticFootprintCovers(SphereAnalyticModifierSet{}, inside[0], inside[1],
                                               inside[2]));
}

// ---------------------------------------------------------------------------------------------
// S2 live-wiring locks: the GLSL twin (cbt_analytic.glsl) and the 12-float GPU packing.
// ---------------------------------------------------------------------------------------------

// CPU <-> GLSL expression lockstep for the analytic eval (the cbt_sculpt.glsl constants-lock
// pattern, extended to expressions): parse cbt_analytic.glsl and assert the load-bearing lines of
// CBT_EvalSphereAnalyticFlatten / CBT_SphereAnalyticFlattenCovers are the EXACT expression twins
// of EvaluateSphereAnalyticFlatten / SphereAnalyticFootprintCovers, field-mapped through the
// PackSphereAnalyticFlatten layout (N/Radius -> NRadius, E1/Falloff -> E1Falloff,
// E2/TargetRadius -> E2Target). An edit to either side without the other fails here.
TEST(SphereAnalyticModifiers, GlslAnalyticEvalMatchesCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_analytic.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    auto expectLine = [&](const char* fragment) {
        EXPECT_NE(src.find(fragment), std::string::npos)
            << "cbt_analytic.glsl lost its CPU-lockstep expression: " << fragment;
    };

    // Set bound mirrors CBTLayout.h kMaxSphereAnalyticModifiers.
    std::smatch m;
    ASSERT_TRUE(std::regex_search(
        src, m, std::regex(R"(const\s+uint\s+CBT_MAX_SPHERE_ANALYTIC\s*=\s*([0-9]+)u?\s*;)")));
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kMaxSphereAnalyticModifiers);

    // EvaluateSphereAnalyticFlatten twins (SphereAnalyticModifiers.h:76-99):
    expectLine("if (dot(dir, f.NRadius.xyz) <= 0.0)");                      // far-hemisphere reject
    expectLine("float l0 = planetRadius * dot(dir, f.E1Falloff.xyz);");     // tangent-plane metres
    expectLine("float l1 = planetRadius * dot(dir, f.E2Target.xyz);");
    expectLine("float distFromEdge = f.NRadius.w - sqrt(l0 * l0 + l1 * l1);");
    expectLine("if (f.E1Falloff.w <= 0.0)");                                // hard edge
    expectLine("float t = clamp(1.0 + distFromEdge / f.E1Falloff.w, 0.0, 1.0);");
    expectLine("weight = t * t * (3.0 - 2.0 * t);");                        // smoothstep
    expectLine("return (f.E2Target.w - planetRadius - reliefAtDir) * weight;"); // relief-cancel

    // SphereAnalyticFootprintCovers twin (footprint + skirt):
    expectLine("return sqrt(l0 * l0 + l1 * l1) <= f.NRadius.w + max(f.E1Falloff.w, 0.0);");
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The 12-float packing both param blocks upload (CBTFrameParams.SphereAnalytic /
// CBTSurfaceParams.SphereAnalytic) must place each field exactly where the GLSL struct reads it:
// [N.xyz | Radius][E1.xyz | Falloff][E2.xyz | TargetRadius]. Locks PackSphereAnalyticFlatten
// against both the GLSL field consumption above and the CBTRenderFeature fill.
TEST(SphereAnalyticModifiers, PackedLayoutMatchesGlslFieldOrder)
{
    const SphereAnalyticFlatten f =
        MakeSphereAnalyticFlatten(1000.0f, 2000.0f, 3000.0f, 150.0f, 25.0f, 2050.0f, 2000.0f);
    ASSERT_TRUE(f.Valid);
    float packed[kSphereAnalyticFloatsPerModifier] = {};
    PackSphereAnalyticFlatten(f, packed);
    EXPECT_EQ(packed[0], f.N[0]);
    EXPECT_EQ(packed[1], f.N[1]);
    EXPECT_EQ(packed[2], f.N[2]);
    EXPECT_EQ(packed[3], f.Radius);
    EXPECT_EQ(packed[4], f.E1[0]);
    EXPECT_EQ(packed[5], f.E1[1]);
    EXPECT_EQ(packed[6], f.E1[2]);
    EXPECT_EQ(packed[7], f.Falloff);
    EXPECT_EQ(packed[8], f.E2[0]);
    EXPECT_EQ(packed[9], f.E2[1]);
    EXPECT_EQ(packed[10], f.E2[2]);
    EXPECT_EQ(packed[11], f.TargetRadius);
}

// ---------------------------------------------------------------------------------------------
// S3: analytic-while-stroking (transient brush dabs) + per-cell placement culling.
// ---------------------------------------------------------------------------------------------

// Exact-shape-during-stroke oracle, part 1: the transient analytic dab converges to ApplyDab's
// store writes. Bake one dab and evaluate the analytic twin at every texel-centre direction of
// its footprint (texel written into an empty layer == exactly ApplyDab's delta). Two regimes:
//   * R=2000 (angR=0.05 — acos well-conditioned): the store IS the smoothstep cone; the analytic
//     tangent-plane form matches to its documented sin(ang)/ang discrepancy (angR^2/6 ~ 4e-4)
//     -> tight bound. The "falloff math is the same function" honest-falsification check.
//   * R=50000 (angR=0.002): ApplyDab's own acos quantizes the falloff argument in fp32 steps of
//     ULP(1)/sin(ang)/angR (~6% of the slope mid-cap) — the STORE's values carry ~0.1*strength
//     noise rings that the analytic (well-conditioned) preview does not have. Bounded here and
//     folded into the release-pop honesty below; the store's own artifact, not the preview's.
TEST(SphereAnalyticDabs, AnalyticDabConvergesToApplyDabStoreWrites)
{
    struct Case
    {
        float R;
        float tolFrac; // max |store - analytic| as a fraction of strength
    };
    const Case cases[] = {{2000.0f, 0.01f}, {50000.0f, 0.25f}};
    const float brushR = 100.0f;
    const float strength = 5.0f;
    for (const Case& c : cases)
    {
        const float angR = brushR / c.R;
        const std::array<float, 3> n = {1.0f, 0.02f, 0.01f};
        const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const std::array<float, 3> un = {n[0] / nlen, n[1] / nlen, n[2] / nlen};

        SphereSculptLayer layer;
        layer.Configure(RuntimeGeometry(c.R));
        layer.ApplyDab(n[0], n[1], n[2], angR, strength, false);

        const SphereAnalyticDab dab = MakeSphereAnalyticDab(n[0], n[1], n[2], angR, strength, c.R);
        ASSERT_TRUE(dab.Valid);

        // Scan the footprint's texel grid on the primary face and compare texel values.
        const uint32_t dim = layer.Geometry().VirtualDim;
        const SphereFaceUV fc = WorldDirToFaceUV(un[0], un[1], un[2]);
        const float span = 2.0f * angR / (0.5f * kPi); // footprint span in face-UV units (approx)
        const int32_t centerTx = static_cast<int32_t>(fc.U * static_cast<float>(dim - 1));
        const int32_t centerTz = static_cast<int32_t>(fc.V * static_cast<float>(dim - 1));
        const int32_t radiusT = static_cast<int32_t>(span * static_cast<float>(dim - 1)) + 2;
        const SphereSculptSampler s = layer.MakeSampler();
        int compared = 0;
        float maxDiff = 0.0f;
        for (int32_t tz = centerTz - radiusT; tz <= centerTz + radiusT; ++tz)
            for (int32_t tx = centerTx - radiusT; tx <= centerTx + radiusT; ++tx)
            {
                if (tx < 0 || tz < 0 || tx >= static_cast<int32_t>(dim) ||
                    tz >= static_cast<int32_t>(dim))
                    continue;
                const float u = static_cast<float>(tx) / static_cast<float>(dim - 1);
                const float v = static_cast<float>(tz) / static_cast<float>(dim - 1);
                float dx, dy, dz;
                FaceUVToWorldDir(fc.Face, u, v, dx, dy, dz);
                const float store = SampleSphereSculptByDir(s, dx, dy, dz);
                const float analytic = EvaluateSphereAnalyticDab(dab, dx, dy, dz);
                maxDiff = std::max(maxDiff, std::fabs(store - analytic));
                if (analytic != 0.0f)
                    ++compared;
            }
        std::printf(
            "[shape-accuracy] dab-vs-store R=%.0f B=%.0f texels-compared=%d max|diff|=%.6g m\n",
            c.R, brushR, compared, maxDiff);
        ASSERT_GT(compared, 20); // the footprint must actually hold texels
        EXPECT_LT(maxDiff, c.tolFrac * strength) << "R=" << c.R;
    }
}

// Exact-shape-during-stroke oracle, part 2 (the S2 oracle standard): the mid-stroke composed
// sample (empty store + one transient dab) reconstructs the dab's half-amplitude circle to
// <= 0.2% of the brush radius at R=50000 AND Earth — including the sub-texel 5 m brush the store
// path loses entirely. The half-amplitude ring of the smoothstep falloff sits exactly at
// angR/2 (t = 0.5 -> w = 0.5), so the expected circle radius is brushR/2.
TEST(SphereAnalyticDabs, TransientDabReconstructsExactCircleAtAnyRadius)
{
    struct Case
    {
        float R;
        float B;
    };
    const Case cases[] = {{50000.0f, 100.0f}, {50000.0f, 5.0f}, {kEarthRadius, 100.0f}};
    for (const Case& c : cases)
    {
        const std::array<float, 3> n = {1.0f, 0.0f, 0.0f};
        // Centre amplitude = kPlateauH, so MeasureCircleEdge's half-plateau crossing (kPlateauH/2)
        // sits at smoothstep weight 0.5 -> t = 0.5 -> exactly angR/2: the expected ring is B/2.
        const float strength = kPlateauH;
        const SphereAnalyticDab dab =
            MakeSphereAnalyticDab(n[0], n[1], n[2], c.B / c.R, strength, c.R);
        const SphereAnalyticModifierSet set{nullptr, 0u, &dab, 1u};

        SphereSculptLayer layer;
        layer.Configure(RuntimeGeometry(c.R)); // empty store
        const float texelM = SculptMetersPerTexel(c.R, layer.Geometry().VirtualDim);
        const float halfRing = 0.5f * c.B;
        const EdgeMeasurement m = MeasureCircleEdge(
            [&layer, &set](float dx, float dy, float dz) {
                return SampleSphereSculptComposed(layer.MakeSampler(), set, dx, dy, dz, 0.0f);
            },
            n, c.R, halfRing, 0.75f * c.B, c.B / 1024.0f);
        std::printf("[shape-accuracy] transient-dab R=%.0f B=%.1f texel=%.3fm maxErr=%.4fm "
                    "(%.3f%% of B)\n",
                    c.R, c.B, texelM, m.MaxErrM, 100.0f * m.MaxErrM / c.B);
        EXPECT_TRUE(m.Representable);
        EXPECT_LT(m.MaxErrM, 0.002f * c.B) << "R=" << c.R << " B=" << c.B;
    }
}

// The transient's covers twin gates the crease-retess residency: inside the cap covers, outside
// does not (the smoothstep support ends at the cap edge), and an empty dab set stays inert.
TEST(SphereAnalyticDabs, DabCoversCapOnly)
{
    const float planetRadius = 50000.0f;
    const float brushR = 100.0f;
    const std::array<float, 3> n = {0.0f, 1.0f, 0.0f};
    const SphereAnalyticDab dab =
        MakeSphereAnalyticDab(n[0], n[1], n[2], brushR / planetRadius, 3.0f, planetRadius);
    const SphereAnalyticModifierSet set{nullptr, 0u, &dab, 1u};

    const std::array<float, 3> t1 = AnyTangent(n);
    auto dirAt = [&](float distM) {
        const float a = distM / planetRadius;
        return std::array<float, 3>{std::cos(a) * n[0] + std::sin(a) * t1[0],
                                    std::cos(a) * n[1] + std::sin(a) * t1[1],
                                    std::cos(a) * n[2] + std::sin(a) * t1[2]};
    };
    const std::array<float, 3> inside = dirAt(0.6f * brushR);
    const std::array<float, 3> outside = dirAt(brushR + 15.0f);
    EXPECT_TRUE(SphereAnalyticFootprintCovers(set, inside[0], inside[1], inside[2]));
    EXPECT_FALSE(SphereAnalyticFootprintCovers(set, outside[0], outside[1], outside[2]));
    EXPECT_EQ(SampleSphereSculptComposed(SphereSculptSampler{}, set, outside[0], outside[1],
                                         outside[2], 0.0f),
              0.0f);
}

// Release-pop, before/after (S4 EXPECTATION UPDATE, justified): pre-S4 this test PINNED the pop
// (a sub-texel commit lost the shape -> pop ~= full amplitude) as the store's documented ceiling.
// S4's adaptive page escalation is the cure at the root, so the expectation flips: the DEFAULT
// store now commits the shape (pop bounded by fine-texel bilinear error), and the old fails-before
// is preserved as a live can-fail oracle by pinning MaxPageLevel(0) — proving the closure comes
// from escalation, not from a measurement change. The resolvable 100 m brush keeps its legacy
// (acos falloff) bound, and additionally locks the GE_TERRAIN_TANGENT_DAB rider: the tangent-
// plane committed falloff matches the preview to pure bilinear error (the acos noise term is
// gone), the "11% -> bilinear-only" half of the S4 pop gate.
TEST(SphereAnalyticDabs, ReleasePopQuantifiedAtStoreResolution)
{
    const float planetRadius = 50000.0f;
    const float strength = 5.0f;
    struct Case
    {
        float B;
        bool subTexel; // sub-representable at the base grid (the escalation trigger regime)
    };
    const Case cases[] = {{100.0f, false}, {5.0f, true}};
    for (const Case& c : cases)
    {
        // CONSTRUCT a page-INTERIOR centre (mid-page 40, fractional texel offset) instead of a
        // lucky direction: escalated fine texels exist only inside pages, so a dab centred in
        // the one-base-texel SEAM STRIP between two pages commits only its flanking-column
        // sliver — the S4 design's documented residual limitation, pinned separately by
        // SeamStripDabIsDocumentedResidualLimitation. The closure gate here is the page-interior
        // case (the overwhelming majority of placements).
        const uint32_t dimForCentre = RuntimeGeometry(planetRadius).VirtualDim;
        const float centreTexel = 40.5f * static_cast<float>(kSculptPageDim) + 0.37f;
        const float centreUV = centreTexel / static_cast<float>(dimForCentre - 1u);
        std::array<float, 3> n{};
        FaceUVToWorldDir(0u, centreUV, centreUV + 7.3f / static_cast<float>(dimForCentre - 1u),
                         n[0], n[1], n[2]);
        const float angR = c.B / planetRadius;
        const SphereAnalyticDab dab =
            MakeSphereAnalyticDab(n[0], n[1], n[2], angR, strength, planetRadius);
        const SphereAnalyticModifierSet transient{nullptr, 0u, &dab, 1u};

        SphereSculptLayer layer; // empty store: the held-preview side of the pop
        layer.Configure(RuntimeGeometry(planetRadius));
        const float texelM = SculptMetersPerTexel(planetRadius, layer.Geometry().VirtualDim);
        const float texelAng = texelM / planetRadius;

        // The pop is a SHAPE-level truth, not a single point's: a lost sub-texel dab either
        // vanishes (pop = amplitude at the centre) or blobs into a 2-texel-wide mound (pop =
        // amplitude at 1-2 texels out, where the preview is already 0 but the blob is not).
        // Scan a tangent-plane grid covering both the preview footprint and the possible blob.
        const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const std::array<float, 3> un = {n[0] / nlen, n[1] / nlen, n[2] / nlen};
        const std::array<float, 3> t1 = AnyTangent(un);
        const std::array<float, 3> t2 = Cross(un, t1);
        const float scanAng = std::max(angR, 3.0f * texelAng);
        const auto measurePop = [&](const SphereSculptLayer& committedLayer) -> float
        {
            float pop = 0.0f;
            constexpr int kScan = 24;
            for (int iy = -kScan; iy <= kScan; ++iy)
                for (int ix = -kScan; ix <= kScan; ++ix)
                {
                    const float a1 = scanAng * static_cast<float>(ix) / kScan;
                    const float a2 = scanAng * static_cast<float>(iy) / kScan;
                    const float dx = un[0] + a1 * t1[0] + a2 * t2[0];
                    const float dy = un[1] + a1 * t1[1] + a2 * t2[1];
                    const float dz = un[2] + a1 * t1[2] + a2 * t2[2];
                    const float preview =
                        SampleSphereSculptComposed(layer.MakeSampler(), transient, dx, dy, dz, 0.0f);
                    const float committed =
                        SampleSphereSculptByDir(committedLayer.MakeSampler(), dx, dy, dz);
                    pop = std::max(pop, std::fabs(preview - committed));
                }
            return pop;
        };
        const auto commit = [&](uint32_t maxLevel, bool tangent) -> SphereSculptLayer
        {
            SphereSculptLayer committed;
            committed.Configure(RuntimeGeometry(planetRadius));
            committed.SetMaxPageLevel(maxLevel);
            committed.SetTangentDabFalloff(tangent);
            committed.ApplyDab(n[0], n[1], n[2], angR, strength, false); // the mouse-up commit
            return committed;
        };

        if (c.subTexel)
        {
            // Fails-before preserved: escalation OFF loses the shape (the S3-documented pop).
            const SphereSculptLayer before = commit(0u, false);
            const float popBefore = measurePop(before);
            EXPECT_EQ(before.EscalatedPageCount(), 0u);
            EXPECT_GT(popBefore, 0.5f * strength)
                << "with escalation pinned off a sub-texel commit must still visibly pop — if this "
                   "fails the fails-before is gone and the after-numbers prove nothing";

            // THE S4 GATE, two metrics:
            //   * pointwise pop = max|preview - committed| — bounded by 2D fine-texel bilinear
            //     error (h^2/8 * (|f_xx|+|f_yy|), h ~ 2.54 m local fine texel at this near-
            //     centre gnomonic position -> ~0.38*A worst-case; the apex offset here is
            //     deliberately mid-cell, near the worst alignment). This is PEAK CLIPPING of
            //     the cone between fine texels, not shape loss.
            //   * half-height ring error — the SHAPE metric: the committed circle's radius must
            //     land within ONE local fine texel of the exact B/2 ring (vs "vanish or 2-base-
            //     texel blob" before).
            const SphereSculptLayer after = commit(kSculptMaxPageLevel, false);
            const float popAfter = measurePop(after);
            std::printf("[shape-accuracy] release-pop R=%.0f B=%.1f texel=%.3fm before=%.3fm "
                        "(%.1f%% of amplitude) after=%.3fm (%.1f%%) escalatedPages=%u\n",
                        planetRadius, c.B, texelM, popBefore, 100.0f * popBefore / strength,
                        popAfter, 100.0f * popAfter / strength, after.EscalatedPageCount());
            EXPECT_GT(after.EscalatedPageCount(), 0u)
                << "the sub-representable brush must have escalated its pages";
            EXPECT_LT(popAfter, 0.4f * strength)
                << "the committed escalated shape must match the preview to 2D fine-texel "
                   "bilinear error — the pop is closed";

            // Shape metric on a kPlateauH-amplitude commit (MeasureCircleEdge crosses at
            // kPlateauH/2, i.e. the smoothstep's exact B/2 ring).
            SphereSculptLayer shape;
            shape.Configure(RuntimeGeometry(planetRadius));
            shape.ApplyDab(n[0], n[1], n[2], angR, kPlateauH, false);
            const float localFineTexelM = 2.0f / kSculptHalfPi * texelM /
                                          static_cast<float>(1u << kSculptMaxPageLevel);
            const EdgeMeasurement em = MeasureCircleEdge(
                [&shape](float dx, float dy, float dz)
                { return SampleSphereSculptByDir(shape.MakeSampler(), dx, dy, dz); },
                n, planetRadius, 0.5f * c.B, 0.75f * c.B, c.B / 1024.0f);
            std::printf("[shape-accuracy] committed-ring R=%.0f B=%.1f fineTexel=%.3fm "
                        "maxErr=%.3fm (%.2f fine texels) representable=%d\n",
                        planetRadius, c.B, localFineTexelM, em.MaxErrM,
                        em.MaxErrM / localFineTexelM, em.Representable ? 1 : 0);
            EXPECT_TRUE(em.Representable)
                << "the committed sub-texel brush must hold at least half its amplitude";
            EXPECT_LT(em.MaxErrM, localFineTexelM)
                << "committed half-height ring must land within one local fine texel of B/2";
        }
        else
        {
            // Resolvable brush, legacy falloff (default flags): never escalates; pop bounded by
            // bilinear/texel error PLUS the store's fp32 acos falloff quantization (~0.1*A).
            const SphereSculptLayer legacy = commit(kSculptMaxPageLevel, false);
            const float popLegacy = measurePop(legacy);
            EXPECT_EQ(legacy.EscalatedPageCount(), 0u)
                << "a resolvable brush must never escalate (the dark-ship trigger)";
            EXPECT_LT(popLegacy, 0.2f * strength);

            // GE_TERRAIN_TANGENT_DAB rider: committed bytes use the preview's own closed form, so
            // the acos noise term vanishes and only pure bilinear error remains.
            const SphereSculptLayer tangent = commit(kSculptMaxPageLevel, true);
            const float popTangent = measurePop(tangent);
            std::printf("[shape-accuracy] release-pop R=%.0f B=%.1f texel=%.3fm acos=%.3fm "
                        "(%.1f%% of amplitude) tangent=%.3fm (%.2f%%)\n",
                        planetRadius, c.B, texelM, popLegacy, 100.0f * popLegacy / strength,
                        popTangent, 100.0f * popTangent / strength);
            EXPECT_EQ(tangent.EscalatedPageCount(), 0u);
            EXPECT_LT(popTangent, 0.05f * strength)
                << "tangent-falloff committed bytes must match the preview to pure bilinear error";
            EXPECT_LT(popTangent, popLegacy)
                << "the tangent falloff must strictly beat the acos form's noise";
        }
    }
}

// The S4 design's documented RESIDUAL limitation, pinned so it cannot silently regress OR
// silently heal without a test update: escalated fine texels exist only INSIDE pages (the
// endpoint-aligned (127*2^L+1)^2 grid), so the one-base-texel seam strip BETWEEN pages holds no
// fine storage — the sampler lerps the flanking pages' edge columns across it (that lerp is what
// keeps mixed-level seams C0). An isolated sub-texel dab whose centre lands INSIDE a strip
// (~1-3% of placements: within ~1 base texel of a page seam line) therefore commits only its
// flanking-column sliver — the release pop persists there, bounded by the pre-S4 behaviour
// (never worse). This direction measurably lands mid-strip at R=50000 (base x ~4991.56, strip
// [4991, 4992]); the S5 candidate is strip-owning page grids (full 128*2^L storage with
// overflow-aware edge evals). Strokes are largely unaffected: dabs on either side fill the
// flanking columns and the strip interpolates between them.
TEST(SphereAnalyticDabs, SeamStripDabIsDocumentedResidualLimitation)
{
    const float planetRadius = 50000.0f;
    const float strength = 5.0f;
    const float brushR = 5.0f;
    const std::array<float, 3> n = {1.0f, 0.013f, 0.007f}; // mid-strip at this radius (see above)
    SphereSculptLayer committed;
    committed.Configure(RuntimeGeometry(planetRadius));
    const uint32_t dim = committed.Geometry().VirtualDim;
    {
        // Verify the premise: the centre's base x must actually be inside a seam strip
        // (fractional page-local column in (126.5, 127.9) -> between the two pages' texels).
        const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const SphereFaceUV fc = WorldDirToFaceUV(n[0] / nlen, n[1] / nlen, n[2] / nlen);
        const float baseX = fc.U * static_cast<float>(dim - 1u);
        const float local = baseX - std::floor(baseX / kSculptPageDim) * kSculptPageDim;
        ASSERT_GT(local, 126.5f) << "test premise drifted: centre no longer in a seam strip";
        ASSERT_LT(local, 127.9f) << "test premise drifted: centre no longer in a seam strip";
    }
    committed.ApplyDab(n[0], n[1], n[2], brushR / planetRadius, strength, false);
    const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float centre =
        SampleSphereSculptByDir(committed.MakeSampler(), n[0] / nlen, n[1] / nlen, n[2] / nlen);
    std::printf("[shape-accuracy] strip-dab R=%.0f B=%.1f committed-centre=%.3fm of %.1fm "
                "(escalatedPages=%u)\n",
                planetRadius, brushR, centre, strength, committed.EscalatedPageCount());
    EXPECT_LT(centre, 0.5f * strength)
        << "a mid-strip sub-texel dab commits only the flanking-column sliver — if this starts "
           "holding the full shape, the strip limitation was fixed: move this case into the "
           "closure gate";
    EXPECT_GE(centre, 0.0f);
}

// Per-cell culling conservativeness (the S3 cost fix's correctness contract): for ANY direction
// where a placement contributes (eval != 0 or covers true), the mask of the cell containing that
// direction's face-UV must have the placement's bit set — masked evaluation is exact, never a
// lossy approximation. Sweep a dense direction grid over a mixed flatten+dab set (face-interior,
// face-edge and cell-spanning placements).
TEST(SphereAnalyticDabs, CellMasksAreConservativeForEvalAndCovers)
{
    const float planetRadius = 50000.0f;
    std::array<SphereAnalyticFlatten, 3> flattens{};
    flattens[0] = MakeSphereAnalyticFlatten(planetRadius, 0.0f, 0.0f, 120.0f, 40.0f,
                                            planetRadius + 10.0f, planetRadius);
    // Face-edge straddler (between +X and +Y faces).
    flattens[1] = MakeSphereAnalyticFlatten(planetRadius, planetRadius, 0.0f, 300.0f, 60.0f,
                                            planetRadius + 5.0f, planetRadius);
    // A footprint wider than a cell (cell ~ 9.8 km at R=50k with the 8x8 grid).
    flattens[2] = MakeSphereAnalyticFlatten(0.0f, 0.0f, planetRadius, 15000.0f, 2000.0f,
                                            planetRadius + 3.0f, planetRadius);
    std::array<SphereAnalyticDab, 2> dabs{};
    dabs[0] = MakeSphereAnalyticDab(0.0f, planetRadius, 0.0f, 100.0f / planetRadius, 4.0f,
                                    planetRadius);
    dabs[1] = MakeSphereAnalyticDab(-planetRadius, 0.2f * planetRadius, 0.1f * planetRadius,
                                    5.0f / planetRadius, 2.0f, planetRadius);

    std::array<uint32_t, kSphereAnalyticCellMaskWords> words{};
    BuildSphereAnalyticCellMasks(flattens.data(), static_cast<uint32_t>(flattens.size()),
                                 dabs.data(), static_cast<uint32_t>(dabs.size()), words.data());

    auto cellMask = [&](uint32_t face, float u, float v) {
        const uint32_t cell = SphereAnalyticCellIndex(face, u, v);
        const uint32_t word = words[cell >> 1u];
        return (cell & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    };

    int contributing = 0;
    int culledSamples = 0;
    constexpr int kGridSamples = 96;
    for (uint32_t face = 0; face < 6u; ++face)
        for (int iy = 0; iy < kGridSamples; ++iy)
            for (int ix = 0; ix < kGridSamples; ++ix)
            {
                const float u = (static_cast<float>(ix) + 0.5f) / kGridSamples;
                const float v = (static_cast<float>(iy) + 0.5f) / kGridSamples;
                float dx, dy, dz;
                FaceUVToWorldDir(face, u, v, dx, dy, dz);
                const uint32_t mask = cellMask(face, u, v);
                for (uint32_t i = 0; i < flattens.size(); ++i)
                {
                    const bool contributes =
                        EvaluateSphereAnalyticFlatten(flattens[i], dx, dy, dz, 1.0f) != 0.0f ||
                        SphereAnalyticFootprintCovers(
                            SphereAnalyticModifierSet{&flattens[i], 1u}, dx, dy, dz);
                    if (contributes)
                    {
                        ++contributing;
                        EXPECT_NE(mask & (1u << i), 0u)
                            << "flatten " << i << " contributes at face=" << face << " uv=(" << u
                            << "," << v << ") but its cell bit is clear (lossy cull!)";
                    }
                }
                for (uint32_t i = 0; i < dabs.size(); ++i)
                {
                    if (SphereAnalyticDabCovers(dabs[i], dx, dy, dz))
                    {
                        ++contributing;
                        EXPECT_NE(mask & (1u << (flattens.size() + i)), 0u)
                            << "dab " << i << " covers face=" << face << " uv=(" << u << "," << v
                            << ") but its cell bit is clear (lossy cull!)";
                    }
                }
                if (mask == 0u)
                    ++culledSamples;
            }
    std::printf("[shape-accuracy] cell-mask sweep: contributing-hits=%d culled-samples=%d/%d\n",
                contributing, culledSamples, 6 * kGridSamples * kGridSamples);
    ASSERT_GT(contributing, 100); // the sweep must actually exercise the footprints
    EXPECT_GT(culledSamples, 6 * kGridSamples * kGridSamples / 2)
        << "the mask must actually cull: most of the planet is far from every placement";
}

// The GLSL dab twin + the cell-mask lookup stay in expression-lockstep with the CPU authority
// (the S2 parse-lock pattern extended to S3's additions).
TEST(SphereAnalyticDabs, GlslDabEvalAndCellMaskMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    auto readFile = [](const std::string& path) {
        std::ifstream file(path);
        EXPECT_TRUE(file.is_open()) << "cannot open " << path;
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    };
    const std::string analytic = readFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_analytic.glsl");
    auto expectIn = [](const std::string& src, const char* fragment, const char* file) {
        EXPECT_NE(src.find(fragment), std::string::npos)
            << file << " lost its CPU-lockstep expression: " << fragment;
    };

    // EvaluateSphereAnalyticDab twins (SphereAnalyticModifiers.h): tangent-plane metres (the
    // numerically-robust form — see the header's note on the acos collapse), smoothstep of
    // dist/SinRadiusM, signed amplitude in E1Falloff.w.
    expectIn(analytic, "float t = clamp(1.0 - sqrt(l0 * l0 + l1 * l1) / f.NRadius.w, 0.0, 1.0);",
             "cbt_analytic.glsl");
    expectIn(analytic, "return f.E1Falloff.w * (t * t * (3.0 - 2.0 * t));", "cbt_analytic.glsl");
    // SphereAnalyticDabCovers twin (support ends exactly at the cap edge — no skirt term).
    expectIn(analytic, "return sqrt(l0 * l0 + l1 * l1) <= f.NRadius.w;", "cbt_analytic.glsl");

    // The cell-mask lookup (cbt_layout.glsl) mirrors SphereAnalyticCellIndex: 8x8 grid per face,
    // 2 cells per word, low half = even cell.
    const std::string layout = readFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl");
    std::smatch m;
    ASSERT_TRUE(std::regex_search(layout, m,
                                  std::regex(R"(const\s+uint\s+kGrid\s*=\s*([0-9]+)u?\s*;)")));
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kSphereAnalyticCellGrid);
    expectIn(layout, "uint cell = face * kGrid * kGrid + c.y * kGrid + c.x;", "cbt_layout.glsl");
    expectIn(layout, ".analyticCellMask[cell >> 3u][(cell >> 1u) & 3u];", "cbt_layout.glsl");
    expectIn(layout, "return (cell & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);",
             "cbt_layout.glsl");
    // The mask rides the UBO slot BESIDE the frame struct (never inside CBT_Frame()'s by-value
    // copy — the 1792 B per-thread copy measured ~7x on CBT.Update).
    expectIn(layout, "uvec4 analyticCellMask[48];", "cbt_layout.glsl");
    expectIn(layout, "CBTFrameParams frame;", "cbt_layout.glsl");
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The dab's 12-float packing places each field exactly where CBT_EvalSphereAnalyticDab reads it:
// [N.xyz | SinRadiusM][E1.xyz | Amplitude][E2.xyz | 0].
TEST(SphereAnalyticDabs, PackedDabLayoutMatchesGlslFieldOrder)
{
    const SphereAnalyticDab d = MakeSphereAnalyticDab(100.0f, 220.0f, -75.0f, 0.004f, -3.5f, 2000.0f);
    ASSERT_TRUE(d.Valid);
    float packed[kSphereAnalyticFloatsPerModifier] = {};
    PackSphereAnalyticDab(d, packed);
    EXPECT_EQ(packed[0], d.N[0]);
    EXPECT_EQ(packed[1], d.N[1]);
    EXPECT_EQ(packed[2], d.N[2]);
    EXPECT_EQ(packed[3], d.SinRadiusM);
    EXPECT_EQ(packed[4], d.E1[0]);
    EXPECT_EQ(packed[5], d.E1[1]);
    EXPECT_EQ(packed[6], d.E1[2]);
    EXPECT_EQ(packed[7], d.Amplitude);
    EXPECT_EQ(packed[8], d.E2[0]);
    EXPECT_EQ(packed[9], d.E2[1]);
    EXPECT_EQ(packed[10], d.E2[2]);
    EXPECT_EQ(packed[11], 0.0f);
}

// Dark-ship extension: a set with flattens but NO dabs (the S2 shape) composes identically to S2,
// and the defaulted dab fields keep every S2 positional initializer inert.
TEST(SphereAnalyticDabs, EmptyDabSetKeepsS2Composition)
{
    const float planetRadius = 50000.0f;
    SphereSculptLayer layer;
    layer.Configure(RuntimeGeometry(planetRadius));
    layer.ApplyDab(1.0f, 0.05f, 0.02f, 0.003f, 4.0f, false);

    const SphereAnalyticFlatten f = MakeSphereAnalyticFlatten(
        planetRadius, 0.0f, 0.0f, 100.0f, 0.0f, planetRadius + kPlateauH, planetRadius);
    const SphereAnalyticModifierSet s2Shape{&f, 1u};              // dab fields defaulted
    const SphereAnalyticModifierSet s3Shape{&f, 1u, nullptr, 0u}; // explicit empty dabs
    const SphereSculptSampler s = layer.MakeSampler();
    for (int i = 0; i < 60; ++i)
    {
        const float u = 0.05f + 0.9f * static_cast<float>(i % 10) / 9.0f;
        const float v = 0.05f + 0.9f * static_cast<float>(i / 10) / 5.0f;
        float dx, dy, dz;
        FaceUVToWorldDir(static_cast<uint32_t>(i) % 6u, u, v, dx, dy, dz);
        EXPECT_EQ(SampleSphereSculptComposed(s, s2Shape, dx, dy, dz, 7.0f),
                  SampleSphereSculptComposed(s, s3Shape, dx, dy, dz, 7.0f));
    }
}
