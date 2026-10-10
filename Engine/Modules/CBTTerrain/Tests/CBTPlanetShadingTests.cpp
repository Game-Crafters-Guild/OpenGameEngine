// CPU oracles for planet shading quality (plan §planet-shading). Pure math — no Vulkan
// device — so they always run, and each discriminates a real failure mode of the sphere
// shading path (they FAIL against the pre-change behaviour):
//   * Analytic relief gradient == central finite difference (the gradient is correct — a
//     facet/flat normal has NO gradient and fails).
//   * Analytic shading normal == the geometric normal of the displaced surface (the whole
//     N = normalize(dir - gradTangential(h)/(R+h)) formula is correct, and it is NOT the
//     radial normal where relief has slope).
//   * The normal is a function of world DIRECTION only, so it is bit-for-bit identical from
//     either cube face at a shared edge/corner — seam-free (the C4/C7 cross-face law).
//   * The editable sculpt layer, sampled by direction, gives a normal continuous across a
//     cube edge even for a dab that straddles the edge.
//   * The slope+altitude splat weights are continuous across a cube edge (no UV seam), and
//     respond to the radial slope (a globe's "up" is dir, not world Y).
//   * The multi-octave relief keeps the +/- 1.5*amplitude envelope for any octave count
//     (so CBTSphereDomainTests' on-shell bound stays valid) while adding real variation.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTerrain/SphereSculptLayer.h"

using namespace GameEngine::CBTTerrain;

namespace
{
using Vec3 = std::array<float, 3>;

Vec3 Normalize(const Vec3& v)
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

float Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 Add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 Scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

// The displaced sphere surface point for a (not-necessarily-unit) direction — normalize,
// displace by the multi-octave relief along it. The geometric-normal oracle differences it.
Vec3 SurfacePoint(const Vec3& d, float radius, float amp, float freq, uint32_t oct)
{
    const Vec3 n = Normalize(d);
    const float h = PlanetRelief(n[0], n[1], n[2], amp, freq, oct);
    return Scale(n, radius + h);
}

float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

// Independent brute-force occlusion: does the segment C->P pass through the floor-sphere of
// radius r (centred at the origin) before reaching P? The entry intersection parameter t0 must
// lie strictly inside (0,1). This is the geometric ground truth the cone predicate must match.
bool BruteForceOccluded(const Vec3& P, const Vec3& C, float r)
{
    const Vec3 d = Sub(P, C);
    const float a = Dot(d, d);
    if (a <= 0.0f)
        return false;
    const float b = 2.0f * Dot(C, d);
    const float c = Dot(C, C) - r * r;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return false;
    const float t0 = (-b - std::sqrt(disc)) / (2.0f * a);
    return t0 > 1e-4f && t0 < 1.0f - 1e-4f;
}

// Signed distance to the cone-test occlusion boundary (< 0 => the predicate says occluded).
// Samples within a small |margin| of 0 are grazing (fp-ambiguous) and skipped in the oracle.
float ConeMargin(const Vec3& P, const Vec3& C, float r)
{
    const float pLen = Length(P), cLen = Length(C);
    const float cosTheta = Dot(P, C) / (pLen * cLen);
    const float cosC = r / cLen, sinC = std::sqrt(std::fmax(1.0f - cosC * cosC, 0.0f));
    const float cosP = std::fmin(std::fmax(r / pLen, -1.0f), 1.0f);
    const float sinP = std::sqrt(std::fmax(1.0f - cosP * cosP, 0.0f));
    return cosTheta - (cosC * cosP - sinC * sinP);
}

// A deterministic spread of unit directions across all six faces (avoids the axes so the
// tangent frame + dominant-face pick are unambiguous).
std::vector<Vec3> SampleDirections()
{
    std::vector<Vec3> dirs;
    for (int i = -5; i <= 5; ++i)
        for (int j = -5; j <= 5; ++j)
            for (int k = -5; k <= 5; ++k)
            {
                if (i == 0 && j == 0 && k == 0)
                    continue;
                dirs.push_back(Normalize({static_cast<float>(i) + 0.13f,
                                          static_cast<float>(j) + 0.29f,
                                          static_cast<float>(k) + 0.47f}));
            }
    return dirs;
}
} // namespace

// The analytic 3D relief gradient agrees with a central finite difference of PlanetRelief at
// random directions. This is the core correctness of the "differentiate CBT_PlanetRelief
// analytically" requirement — a wrong partial (or a zero/facet gradient) fails.
TEST(CBTPlanetShading, ReliefGradientMatchesFiniteDifference)
{
    const float amp = 60.0f, freq = 6.0f;
    const float eps = 1e-4f;
    for (uint32_t oct = 1; oct <= 4; ++oct)
    {
        for (const Vec3& d : SampleDirections())
        {
            const std::array<float, 3> g = PlanetReliefGradient(d[0], d[1], d[2], amp, freq, oct);
            for (int axis = 0; axis < 3; ++axis)
            {
                Vec3 dp = d, dm = d;
                dp[axis] += eps;
                dm[axis] -= eps;
                const float fd = (PlanetRelief(dp[0], dp[1], dp[2], amp, freq, oct) -
                                  PlanetRelief(dm[0], dm[1], dm[2], amp, freq, oct)) /
                                 (2.0f * eps);
                // Tolerance scales with the frequency stack magnitude (higher octaves have
                // larger derivatives, hence larger absolute FD truncation error).
                EXPECT_NEAR(g[axis], fd, 0.5f + 0.02f * std::fabs(g[axis]))
                    << "oct=" << oct << " axis=" << axis;
            }
        }
    }
}

// The analytic shading normal matches the geometric normal from a finite-difference of the
// displaced surface. Validates the full N formula (gradient + the 1/(R+h) projection), and
// is discriminating: a radial/flat normal is off by the surface slope angle and fails.
TEST(CBTPlanetShading, AnalyticNormalMatchesGeometricNormal)
{
    const float radius = 2000.0f, amp = 60.0f, freq = 6.0f;
    const uint32_t oct = 3;
    const float eps = 5e-4f;
    float maxRadialAngleDeg = 0.0f;
    for (const Vec3& d : SampleDirections())
    {
        const Vec3 t1 = AnyTangent(d);
        const Vec3 t2 = Cross(d, t1);
        const Vec3 pu = SurfacePoint(Add(d, Scale(t1, eps)), radius, amp, freq, oct);
        const Vec3 mu = SurfacePoint(Sub(d, Scale(t1, eps)), radius, amp, freq, oct);
        const Vec3 pv = SurfacePoint(Add(d, Scale(t2, eps)), radius, amp, freq, oct);
        const Vec3 mv = SurfacePoint(Sub(d, Scale(t2, eps)), radius, amp, freq, oct);
        Vec3 nGeo = Normalize(Cross(Sub(pu, mu), Sub(pv, mv)));
        if (Dot(nGeo, d) < 0.0f)
            nGeo = Scale(nGeo, -1.0f);

        const std::array<float, 3> nA = PlanetShadingNormal(d, radius, amp, freq, oct);
        const float cosang = std::fmin(std::fmax(Dot(nA, nGeo), -1.0f), 1.0f);
        const float angleDeg = std::acos(cosang) * 57.2957795f;
        EXPECT_LT(angleDeg, 1.5f) << "analytic vs geometric normal diverged";

        // Discriminating: where relief has slope, the normal is measurably off radial.
        const float radialCos = std::fmin(std::fmax(Dot({nA[0], nA[1], nA[2]}, d), -1.0f), 1.0f);
        maxRadialAngleDeg = std::fmax(maxRadialAngleDeg, std::acos(radialCos) * 57.2957795f);
    }
    EXPECT_GT(maxRadialAngleDeg, 3.0f)
        << "normal never departs from radial — a flat/facet normal would pass this by mistake";
}

// The shading normal depends only on world direction, so at a shared cube edge/corner it is
// identical whether the neighbourhood is parametrised through face A or face B. Sampling a
// direction ON an edge and re-deriving it through both adjacent faces' (u,v) must agree.
TEST(CBTPlanetShading, NormalSeamFreeAcrossCubeEdgesAndCorners)
{
    const float radius = 3000.0f, amp = 90.0f, freq = 8.0f;
    const uint32_t oct = 3;
    // Directions exactly on shared cube edges and corners (integer cube coords -> exact).
    const std::array<Vec3, 6> edgeAndCorner = {{
        Normalize({1.0f, 1.0f, 0.3f}),  // +X/+Y edge region
        Normalize({1.0f, 0.0f, 1.0f}),  // +X/+Z edge
        Normalize({0.2f, 1.0f, 1.0f}),  // +Y/+Z edge
        Normalize({1.0f, 1.0f, 1.0f}),  // +X/+Y/+Z corner
        Normalize({-1.0f, 1.0f, 1.0f}), // -X/+Y/+Z corner
        Normalize({1.0f, -1.0f, 1.0f}), // +X/-Y/+Z corner
    }};
    for (const Vec3& d : edgeAndCorner)
    {
        // The normal is the same object regardless of which face WorldDirToFaceUV picks;
        // re-project through EVERY face the direction lands on and confirm the normal is
        // unchanged (it never consults the face for the relief part).
        const std::array<float, 3> nRef = PlanetShadingNormal(d, radius, amp, freq, oct);
        for (uint32_t f = 0; f < kCubeFaceCount; ++f)
        {
            float u, v;
            if (!ProjectDirOntoFace(f, d[0], d[1], d[2], 1e-3f, u, v))
                continue;
            float rx, ry, rz;
            FaceUVToWorldDir(f, u, v, rx, ry, rz);
            const std::array<float, 3> n2 = PlanetShadingNormal({rx, ry, rz}, radius, amp, freq, oct);
            const float cosang = std::fmin(std::fmax(Dot({nRef[0], nRef[1], nRef[2]},
                                                         {n2[0], n2[1], n2[2]}),
                                                     -1.0f),
                                           1.0f);
            EXPECT_GT(cosang, 0.99999f) << "normal differs across face " << f << " at a shared edge";
        }
    }
}

// A sculpt dab straddling a cube edge produces a normal (relief + direction-sampled sculpt)
// that is continuous across the shared edge — the seam-free-sculpt claim. Sample just inside
// both adjacent faces near the edge and confirm the normals converge as the samples approach.
TEST(CBTPlanetShading, SculptNormalContinuousAcrossCubeEdge)
{
    const float radius = 2000.0f, amp = 60.0f, freq = 6.0f;
    const uint32_t oct = 3;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom);
    // A dab centred on the +X/+Y shared edge direction, radius large enough to straddle it.
    const Vec3 centre = Normalize({1.0f, 1.0f, 0.0f});
    layer.ApplyDab(centre[0], centre[1], centre[2], 0.20f, 40.0f, /*lower=*/false);
    ASSERT_TRUE(layer.HasEdits());
    const SphereSculptSampler sculpt = layer.MakeSampler();

    const float step = SculptNormalAngularStep(geom.VirtualDim); // one virtual texel of arc

    // March a small arc across the edge (vary the Z tilt toward/away from the edge) and check
    // the normal changes smoothly — no jump at the crossing where the dominant face flips.
    float maxJumpDeg = 0.0f;
    std::array<float, 3> prev{};
    bool havePrev = false;
    for (int i = -20; i <= 20; ++i)
    {
        const float t = static_cast<float>(i) * 0.004f; // sweep across the edge
        const Vec3 d = Normalize({1.0f - std::fabs(t), 1.0f, t});
        const std::array<float, 3> n =
            PlanetShadingNormalWithSculpt(d, radius, amp, freq, oct, sculpt, step);
        if (havePrev)
        {
            const float cosang =
                std::fmin(std::fmax(n[0] * prev[0] + n[1] * prev[1] + n[2] * prev[2], -1.0f), 1.0f);
            maxJumpDeg = std::fmax(maxJumpDeg, std::acos(cosang) * 57.2957795f);
        }
        prev = n;
        havePrev = true;
    }
    // A per-face-baked seam would show a discontinuous spike here; a seam-free direction
    // sample keeps consecutive normals within a few degrees over this fine sweep.
    EXPECT_LT(maxJumpDeg, 5.0f) << "sculpt normal jumps at the cube-edge crossing (seam)";
}

// The slope+altitude splat weights are continuous across a cube edge (they are a pure
// function of the seam-free normal, dir and altitude), and they react to the RADIAL slope:
// flat ground (normal == dir) is grass, a steep normal is rock. A world-Y slope (the old
// fallback) would mislabel a whole face; this checks the fix.
TEST(CBTPlanetShading, SlopeAltitudeWeightsSeamFreeAndRadial)
{
    const float amp = 60.0f;
    // Flat ground at sea level: normal == dir -> grass dominates on every face (incl. faces
    // where the OLD world-Y slope would have been ~1 and produced all-rock). This is the
    // radial-slope fix: "up" on a globe is dir, not world Y.
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
    {
        float x, y, z;
        FaceUVToWorldDir(f, 0.5f, 0.5f, x, y, z);
        const Vec3 dir = {x, y, z};
        const std::array<float, 4> w = PlanetSlopeAltitudeWeights(dir, dir, 0.0f, amp);
        EXPECT_GT(w[0], 0.5f) << "flat ground on face " << f << " should be mostly grass, not rock";
        EXPECT_LT(w[1], 0.05f) << "flat ground on face " << f << " must have ~no rock (radial slope)";
    }

    // Steep ground: tilt the normal 60 deg off radial -> rock dominates.
    {
        const Vec3 dir = Normalize({1.0f, 0.2f, 0.3f});
        const Vec3 t = AnyTangent(dir);
        const Vec3 n = Normalize(Add(Scale(dir, std::cos(1.05f)), Scale(t, std::sin(1.05f))));
        const std::array<float, 4> w = PlanetSlopeAltitudeWeights(n, dir, 0.0f, amp);
        EXPECT_GT(w[1], 0.5f) << "steep ground should be mostly rock";
    }

    // Continuity across the +X/+Y edge: two directions a hair either side give near-equal
    // weights (no seam), for flat ground with a mild altitude.
    const Vec3 a = Normalize({1.0f, 0.98f, 0.0f});
    const Vec3 b = Normalize({0.98f, 1.0f, 0.0f});
    const std::array<float, 4> wa = PlanetSlopeAltitudeWeights(a, a, 15.0f, amp);
    const std::array<float, 4> wb = PlanetSlopeAltitudeWeights(b, b, 15.0f, amp);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR(wa[i], wb[i], 0.05f) << "splat weight " << i << " discontinuous across the edge";
}

// Multi-octave relief keeps the +/- 1.5*amplitude envelope for ANY octave count (so the
// on-shell bound in CBTSphereDomainTests stays valid), and adds FINE-SCALE detail: the
// shading normal wiggles more over a short arc as octaves increase (the higher frequencies
// contribute real high-frequency gradient content, which is the visible benefit). ampSum
// normalization bounds the amplitude, so the gain shows as fine light variation, not range.
TEST(CBTPlanetShading, MultiOctaveEnvelopeAndDetail)
{
    const float amp = 100.0f, freq = 6.0f, radius = 2000.0f;
    const float envelope = kReliefEnvelope * amp + 1e-3f;
    for (const Vec3& d : SampleDirections())
        for (uint32_t oct = 1; oct <= 4; ++oct)
        {
            const float h = PlanetRelief(d[0], d[1], d[2], amp, freq, oct);
            EXPECT_LE(std::fabs(h), envelope) << "relief exceeds the +/-1.5*amp envelope, oct=" << oct;
        }

    // Fine-scale normal roughness: sum the angle between consecutive shading normals along a
    // short tangent arc, sampled finely enough to resolve the highest octave (freq*8).
    auto normalRoughness = [&](uint32_t oct) {
        const Vec3 base = Normalize({0.7f, 0.5f, 0.4f});
        const Vec3 t1 = AnyTangent(base);
        double total = 0.0;
        std::array<float, 3> prev{};
        for (int i = -200; i <= 200; ++i)
        {
            const float theta = static_cast<float>(i) * 0.0015f; // fine arc, ~0.6 rad total
            const Vec3 d = Normalize(Add(base, Scale(t1, theta)));
            const std::array<float, 3> n = PlanetShadingNormal(d, radius, amp, freq, oct);
            if (i > -200)
            {
                const float c =
                    std::fmin(std::fmax(n[0] * prev[0] + n[1] * prev[1] + n[2] * prev[2], -1.0f), 1.0f);
                total += std::acos(c);
            }
            prev = n;
        }
        return total;
    };
    EXPECT_GT(normalRoughness(4), normalRoughness(1) * 1.5)
        << "adding octaves did not add fine-scale normal detail";
}

// The horizon-cull predicate is a CONSERVATIVE occlusion test — it must agree with the
// brute-force ray/floor-sphere ground truth over random cameras/points, and must NOT wrongly
// cull raised limb terrain the way a horizon-PLANE test does. Discriminating: it fails if the
// predicate is reverted to the plane test (which disagrees with brute-force here).
TEST(CBTPlanetShading, HorizonCullConservativeVsBruteForce)
{
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> elev(0.0f, 0.12f); // point radius above the floor
    std::uniform_real_distribution<float> camMul(1.2f, 6.0f);
    std::uniform_real_distribution<float> rDist(500.0f, 5000.0f);

    int clear = 0, planeWrong = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const float r = rDist(rng);
        Vec3 pd = Normalize({unit(rng), unit(rng), unit(rng)});
        Vec3 cd = Normalize({unit(rng), unit(rng), unit(rng)});
        if (Length(pd) < 0.1f || Length(cd) < 0.1f)
            continue;
        const Vec3 P = Scale(pd, r * (1.0f + elev(rng)));
        const Vec3 C = Scale(cd, r * camMul(rng));
        const bool bf = BruteForceOccluded(P, C, r);
        const bool cone = SphereCornerOccluded(P, C, r);
        if (std::fabs(ConeMargin(P, C, r)) > 2e-3f) // skip fp-grazing samples
        {
            EXPECT_EQ(cone, bf) << "cone predicate disagrees with brute-force occlusion";
            ++clear;
        }
        // The OLD horizon-plane test (dot(P,C) < r*|P|) is wrong somewhere -> the fix matters.
        const bool plane = Dot(P, C) < r * Length(P);
        if (plane != bf)
            ++planeWrong;
    }
    EXPECT_GT(clear, 1000) << "too few clear samples to be meaningful";
    EXPECT_GT(planeWrong, 0) << "the horizon-plane test must disagree with truth (the bug)";

    // The reviewer's worked example: a limb peak just beyond the geometric horizon is VISIBLE.
    const Vec3 P{0.0f, 2090.0f, 0.0f};
    const Vec3 C{6000.0f, 0.0f, 0.0f};
    const float r = 1910.0f; // 2000 - 1.5*60
    EXPECT_FALSE(SphereCornerOccluded(P, C, r)) << "conservative test must keep the visible peak";
    EXPECT_TRUE(Dot(P, C) < r * Length(P)) << "the plane test WOULD have wrongly culled it";
}

// Max angle between consecutive sculpt-shaded normals as `dir` sweeps a small arc — a seam
// (per-face baked normal or a face-UV-clamped gradient) shows as a spike where the dominant
// face flips; the direction-sampled sculpt FD stays continuous.
static float SculptSweepMaxJumpDeg(const std::vector<Vec3>& sweep,
                                   const SphereSculptSampler& sculpt, float radius, float amp,
                                   float freq, uint32_t oct, float step)
{
    float maxJumpDeg = 0.0f;
    std::array<float, 3> prev{};
    bool have = false;
    for (const Vec3& d : sweep)
    {
        const std::array<float, 3> n =
            PlanetShadingNormalWithSculpt(d, radius, amp, freq, oct, sculpt, step);
        if (have)
        {
            const float c =
                std::fmin(std::fmax(n[0] * prev[0] + n[1] * prev[1] + n[2] * prev[2], -1.0f), 1.0f);
            maxJumpDeg = std::fmax(maxJumpDeg, std::acos(c) * 57.2957795f);
        }
        prev = n;
        have = true;
    }
    return maxJumpDeg;
}

// The FACE-DEPENDENT path (the sculpt finite difference re-derives the dominant face per
// sample) must stay seam-free where THREE faces meet (a cube corner) and across a second edge
// orientation — the cases the dir-only relief normal cannot exercise. A dab straddling each is
// swept and the consecutive-normal jump must stay small across every dominant-face flip.
TEST(CBTPlanetShading, SculptNormalSeamFreeAtCornerAndSecondEdge)
{
    const float radius = 2000.0f, amp = 60.0f, freq = 6.0f;
    const uint32_t oct = 3;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    const float step = SculptNormalAngularStep(geom.VirtualDim);

    // 1) Three-face corner (+X/+Y/+Z). Sweep a small ring AROUND the corner direction so the
    //    dominant face cycles through all three faces the dab wrote.
    {
        SphereSculptLayer layer;
        layer.Configure(geom);
        const Vec3 corner = Normalize({1.0f, 1.0f, 1.0f});
        layer.ApplyDab(corner[0], corner[1], corner[2], 0.30f, 45.0f, false);
        const Vec3 t1 = AnyTangent(corner);
        const Vec3 t2 = Cross(corner, t1);
        std::vector<Vec3> ring;
        for (int i = 0; i <= 240; ++i)
        {
            const float a = static_cast<float>(i) * (6.28318531f / 240.0f);
            const float rad = 0.08f; // small angular radius around the corner
            ring.push_back(Normalize(Add(Scale(corner, std::cos(rad)),
                                         Scale(Add(Scale(t1, std::cos(a)), Scale(t2, std::sin(a))),
                                               std::sin(rad)))));
        }
        EXPECT_LT(SculptSweepMaxJumpDeg(ring, layer.MakeSampler(), radius, amp, freq, oct, step),
                  6.0f)
            << "sculpt normal jumps at a three-face corner (seam)";
    }

    // 2) Second edge orientation (+Y/+Z). March a short arc across the shared edge.
    {
        SphereSculptLayer layer;
        layer.Configure(geom);
        const Vec3 centre = Normalize({0.0f, 1.0f, 1.0f});
        layer.ApplyDab(centre[0], centre[1], centre[2], 0.20f, 40.0f, false);
        std::vector<Vec3> arc;
        for (int i = -20; i <= 20; ++i)
        {
            const float t = static_cast<float>(i) * 0.004f;
            arc.push_back(Normalize({t, 1.0f - std::fabs(t), 1.0f}));
        }
        EXPECT_LT(SculptSweepMaxJumpDeg(arc, layer.MakeSampler(), radius, amp, freq, oct, step),
                  5.0f)
            << "sculpt normal jumps across the +Y/+Z edge (seam)";
    }
}

namespace
{
// The OLD shading normal — sculpt gradient GATED on the centre height magnitude (|h| > gateEps),
// the pre-fix cbt_surface.glsl behaviour. Reproduced here purely so the oracle below can prove the
// gate was the crease source; the production path (PlanetShadingNormalWithSculpt) has no such gate.
Vec3 HeightGatedSculptNormal(const Vec3& dir, float radius, float amp, float freq, uint32_t oct,
                             const SphereSculptSampler& sculpt, float step, float gateEps)
{
    float h = PlanetRelief(dir[0], dir[1], dir[2], amp, freq, oct);
    const std::array<float, 3> g = PlanetReliefGradient(dir[0], dir[1], dir[2], amp, freq, oct);
    const float gd = g[0] * dir[0] + g[1] * dir[1] + g[2] * dir[2];
    std::array<float, 3> gs = {g[0] - gd * dir[0], g[1] - gd * dir[1], g[2] - gd * dir[2]};
    const float center = SampleSphereSculptByDir(sculpt, dir[0], dir[1], dir[2]);
    h += center;
    if (std::fabs(center) > gateEps)
    {
        const Vec3 t1 = AnyTangent(dir);
        const Vec3 t2 = Cross(dir, t1);
        auto stepDir = [&](const Vec3& t, float s) {
            return Vec3{dir[0] + t[0] * s, dir[1] + t[1] * s, dir[2] + t[2] * s};
        };
        const Vec3 p1 = stepDir(t1, step), m1 = stepDir(t1, -step);
        const Vec3 p2 = stepDir(t2, step), m2 = stepDir(t2, -step);
        const float g1 = (SampleSphereSculptByDir(sculpt, p1[0], p1[1], p1[2]) -
                          SampleSphereSculptByDir(sculpt, m1[0], m1[1], m1[2])) /
                         (2.0f * step);
        const float g2 = (SampleSphereSculptByDir(sculpt, p2[0], p2[1], p2[2]) -
                          SampleSphereSculptByDir(sculpt, m2[0], m2[1], m2[2])) /
                         (2.0f * step);
        gs = {gs[0] + g1 * t1[0] + g2 * t2[0], gs[1] + g1 * t1[1] + g2 * t2[1],
              gs[2] + g1 * t1[2] + g2 * t2[2]};
    }
    return SphereShadingNormalFromGrad(dir, radius, h, gs);
}
} // namespace

// The per-pixel sculpt normal has NO crease where the sculpt height crosses ZERO with a real slope —
// the steep-sculpting case (a raise dab abutting a lower dab, or overlapping strokes of opposite
// sign). There the height passes through 0 while the slope is near its maximum, so the OLD
// `abs(centerSculpt) > 1cm` gate dropped the sculpt gradient across the |h|<1cm band and SNAPPED the
// normal from its true (tens-of-degrees) tilt back to relief-only — a hard crease exactly on the
// steepest sculpted ground. The residency-gated production normal (mirrored by
// PlanetShadingNormalWithSculpt, which computes the gradient wherever the sampler is valid) stays
// continuous; the height-gated normal spikes. This FAILS if the height gate is reintroduced.
// (A single smoothstep dab's outer shoulder has near-zero slope, so it does NOT discriminate the
// gate — the zero-crossing between opposite-sign edits is where the gate actually creases.)
TEST(CBTPlanetShading, SculptNormalNoCreaseWhereSculptCrossesZero)
{
    const float radius = 2000.0f, amp = 60.0f, freq = 6.0f;
    const uint32_t oct = 3;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom);
    // A raise dab and a lower dab overlapping in their STEEP mid-falloff (centres one angular radius
    // apart) so the summed height crosses zero AT MAX SLOPE at the midpoint — steep sculpting. (At a
    // 2x-radius separation the dabs would meet at their zero-slope smoothstep edges and there would be
    // no gradient for the gate to drop; the closer spacing is what makes the crossing steep.)
    const Vec3 centre = Normalize({0.3f, 1.0f, 0.2f});
    const Vec3 t1 = AnyTangent(centre);
    const float angRad = 0.12f;
    const Vec3 rise = Normalize(Add(centre, Scale(t1, -angRad * 0.5f)));
    const Vec3 fall = Normalize(Add(centre, Scale(t1, angRad * 0.5f)));
    layer.ApplyDab(rise[0], rise[1], rise[2], angRad, 80.0f, /*lower=*/false);
    layer.ApplyDab(fall[0], fall[1], fall[2], angRad, 80.0f, /*lower=*/true);
    ASSERT_TRUE(layer.HasEdits());
    const SphereSculptSampler sculpt = layer.MakeSampler();
    const float step = SculptNormalAngularStep(geom.VirtualDim);

    // At the crossing (centre) the height is ~0 but the slope is near maximal. The production normal
    // reads that slope (a tens-of-degrees tilt); the height-gated normal drops the gradient there and
    // reverts to relief-only — the crease. Compare the two directly at the crossing.
    ASSERT_LT(std::fabs(SampleSphereSculptByDir(sculpt, centre[0], centre[1], centre[2])), 0.01f)
        << "crossing not at h~0 — the gate would not even engage here";
    const Vec3 nUngated = PlanetShadingNormalWithSculpt(centre, radius, amp, freq, oct, sculpt, step);
    const Vec3 nGated = HeightGatedSculptNormal(centre, radius, amp, freq, oct, sculpt, step, 0.01f);
    const float creaseDeg =
        std::acos(std::fmin(std::fmax(Dot(nUngated, nGated), -1.0f), 1.0f)) * 57.2957795f;
    EXPECT_GT(creaseDeg, 10.0f)
        << "height gate drops no meaningful gradient at a steep zero-crossing — not discriminating";

    // And the production normal is CONTINUOUS across the crossing: sweep it and confirm the per-step
    // change stays small (no spike), so the true slope shades smoothly rather than snapping.
    float ungatedMax = 0.0f;
    Vec3 prevU{};
    bool have = false;
    for (int i = -200; i <= 200; ++i)
    {
        const Vec3 d = Normalize(Add(centre, Scale(t1, static_cast<float>(i) * 0.0009f)));
        const Vec3 nu = PlanetShadingNormalWithSculpt(d, radius, amp, freq, oct, sculpt, step);
        if (have)
        {
            const float cu = std::fmin(std::fmax(Dot(nu, prevU), -1.0f), 1.0f);
            ungatedMax = std::fmax(ungatedMax, std::acos(cu) * 57.2957795f);
        }
        prevU = nu;
        have = true;
    }
    EXPECT_LT(ungatedMax, 2.0f) << "production sculpt normal spikes across the zero-crossing (a crease)";
}

// Splat altitude is the FINAL height (relief + sculpt): a sculpted mesa climbs into the snow band and
// a sculpted pit drops toward grass/dirt. PlanetSlopeAltitudeWeights is the shared weight function;
// the surface now feeds it the per-pixel analytic altitude (relief + sculpt), so this asserts the
// weights move monotonically with height — the "sculpted mesas get rock/snow bands" property.
TEST(CBTPlanetShading, SlopeAltitudeSplatRespondsToSculptHeight)
{
    const float radius = 2000.0f, amp = 60.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    const Vec3 dir = Normalize({0.25f, 1.0f, 0.15f});

    // Snow rises monotonically with altitude on flat ground (normal == dir); sea level (altitude 0)
    // is grass. Absolute altitudes (not relief-offset) so the bands are pinned to the envelope.
    const std::array<float, 4> low = PlanetSlopeAltitudeWeights(dir, dir, 0.0f, amp);
    const std::array<float, 4> mid = PlanetSlopeAltitudeWeights(dir, dir, 45.0f, amp);
    const std::array<float, 4> high = PlanetSlopeAltitudeWeights(dir, dir, 85.0f, amp);
    EXPECT_GT(low[0], 0.5f) << "sea-level flat ground should read grass";
    EXPECT_LT(low[3], 0.05f) << "sea-level flat ground should read no snow";
    EXPECT_LT(low[3], mid[3]) << "snow must increase with altitude";
    EXPECT_LT(mid[3], high[3]) << "snow must increase with altitude";
    EXPECT_GT(high[3], 0.5f) << "a high sculpted mesa should read mostly snow";

    // Driven by an actual sculpt dab: raising a region from sea level lifts its snow weight — the
    // end-to-end splat-from-final-height (the surface feeds this altitude = relief + sculpt).
    SphereSculptLayer layer;
    layer.Configure(geom);
    layer.ApplyDab(dir[0], dir[1], dir[2], 0.15f, 90.0f, /*lower=*/false);
    const SphereSculptSampler sculpt = layer.MakeSampler();
    const float sculptH = SampleSphereSculptByDir(sculpt, dir[0], dir[1], dir[2]);
    ASSERT_GT(sculptH, 40.0f) << "dab centre should rise into the snow band";
    const std::array<float, 4> flat = PlanetSlopeAltitudeWeights(dir, dir, 0.0f, amp);      // grass
    const std::array<float, 4> mesa = PlanetSlopeAltitudeWeights(dir, dir, sculptH, amp);   // sculpt-raised
    EXPECT_GT(mesa[3], flat[3] + 0.2f)
        << "sculpting a mesa did not raise its snow weight (splat not reading the final height)";
}

// ---------------------------------------------------------------------------
// RefinePlanetHit — the brush raycast lands on the DISPLACED surface, not the base sphere
// (#488 follow-up). The analytic RaycastPlanet solves |o + t d| = radius, ignoring relief +
// sculpt, so the cursor drifts off tall features; RefinePlanetHit closes the gap.
// ---------------------------------------------------------------------------
namespace
{
// Analytic nearest ray/sphere hit param, mirroring RaycastPlanet's base solve. d is unit.
bool BaseSphereHit(const Vec3& o, const Vec3& d, float radius, float& t)
{
    const float b = Dot(o, d);
    const float c = Dot(o, o) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f)
        return false;
    const float sq = std::sqrt(disc);
    t = -b - sq;
    if (t < 0.0f)
        t = -b + sq;
    return t >= 0.0f;
}
} // namespace

// A constant additive height inflates the surface to a sphere of radius + bump. The base hit
// sits on `radius`; the refined hit lands on `radius + bump` — a 150 m drift the fix removes.
TEST(RefinePlanetHit, ConstantHeightLandsOnInflatedSphere)
{
    const float radius = 2000.0f;
    const float bump = 150.0f;
    const Vec3 o{1800.0f, 0.0f, -6000.0f};       // grazing, hits near the limb
    const Vec3 d = Normalize({0.0f, 0.0f, 1.0f});

    float t = 0.0f;
    ASSERT_TRUE(BaseSphereHit(o, d, radius, t));
    const Vec3 baseP = Add(o, Scale(d, t));
    EXPECT_NEAR(Length(baseP), radius, 0.5f);

    auto heightFn = [bump](float, float, float) { return bump; };
    ASSERT_TRUE(RefinePlanetHit(o, d, radius, heightFn, t));
    const Vec3 p = Add(o, Scale(d, t));

    EXPECT_NEAR(Length(p), radius + bump, 0.5f);                        // lands on the displaced surface
    EXPECT_GT(std::fabs(Length(p) - Length(baseP)), 100.0f);           // moved well off the base sphere
}

// With the REAL multi-octave relief the refined hit converges onto the CPU height-function
// surface (residual -> 0), and is never worse than the base-sphere hit's residual.
TEST(RefinePlanetHit, TracksReliefSurface)
{
    const float radius = 2000.0f;
    const float amp = 200.0f, freq = 6.0f;
    const uint32_t oct = 3u;
    const Vec3 o{1700.0f, 500.0f, -6000.0f};
    const Vec3 d = Normalize({0.0f, -0.02f, 1.0f});

    float t = 0.0f;
    ASSERT_TRUE(BaseSphereHit(o, d, radius, t));
    const Vec3 baseP = Add(o, Scale(d, t));
    const Vec3 baseDir = Normalize(baseP);
    const float baseResidual = std::fabs(
        Length(baseP) - (radius + PlanetRelief(baseDir[0], baseDir[1], baseDir[2], amp, freq, oct)));

    auto heightFn = [&](float dx, float dy, float dz) {
        return PlanetRelief(dx, dy, dz, amp, freq, oct);
    };
    ASSERT_TRUE(RefinePlanetHit(o, d, radius, heightFn, t));
    const Vec3 p = Add(o, Scale(d, t));
    const Vec3 dir = Normalize(p);
    const float refinedResidual =
        std::fabs(Length(p) - (radius + PlanetRelief(dir[0], dir[1], dir[2], amp, freq, oct)));

    EXPECT_LT(refinedResidual, 0.5f);           // lands on the CPU height surface (the oracle)
    EXPECT_LE(refinedResidual, baseResidual);   // never worse than the un-refined base hit
}

// A non-positive radius is a no-op (leaves t untouched) — RaycastPlanet already rejected it.
TEST(RefinePlanetHit, RejectsNonPositiveRadius)
{
    const Vec3 o{0.0f, 0.0f, -100.0f};
    const Vec3 d = Normalize({0.0f, 0.0f, 1.0f});
    float t = 42.0f;
    auto heightFn = [](float, float, float) { return 0.0f; };
    EXPECT_FALSE(RefinePlanetHit(o, d, 0.0f, heightFn, t));
    EXPECT_FLOAT_EQ(t, 42.0f);
}

// --- Untextured-fallback layer variation (round-8e: purple grass / cliff banding fixes) ----------
namespace
{
// Default grass / rock palette entries (Terrain::kDefaultTerrainMaterials, TerrainMaterialRecord.h)
// — the tint + (scale, strength, saturation) the untextured planet shades with.
constexpr Vec3 kGrassTint = {0.35f, 0.55f, 0.18f};
constexpr float kGrassScale = 0.030f, kGrassStrength = 0.16f, kGrassSat = 0.10f;
constexpr Vec3 kRockTint = {0.55f, 0.50f, 0.42f};
constexpr float kRockScale = 0.022f, kRockStrength = 0.24f, kRockSat = 0.12f;

// Sample world positions on a 50 km planet surface (the round-8e scene shape) along a wall + a patch.
std::vector<Vec3> VariationSamplePositions()
{
    std::vector<Vec3> pts;
    const Vec3 dir = Normalize({0.3f, 0.9f, 0.32f});
    for (int i = 0; i < 400; ++i)
    {
        // Walk 0.25 m steps in world Y (a vertical wall) plus a lateral spread, at R~50000.
        const float y = static_cast<float>(i) * 0.25f;
        const float lat = static_cast<float>(i % 40) * 0.7f;
        pts.push_back({dir[0] * 50000.0f + lat, dir[1] * 50000.0f + y, dir[2] * 50000.0f});
    }
    return pts;
}
} // namespace

// The variation is HUE-PRESERVING: grass (green-dominant, G > R > B) never inverts its channel order,
// so it can never read purple/magenta (B rising above R/G) — the round-8e "purple grass" bug, which
// the old per-channel chroma jitter produced. Also checks the chroma DIRECTION (offset from the grey
// axis) stays parallel to the tint's, the crisp form of "hue unchanged".
TEST(CBTPlanetShading, LayerVariationPreservesHue)
{
    ASSERT_GT(kGrassTint[1], kGrassTint[0]); // sanity: tint is green-dominant, blue-least
    ASSERT_GT(kGrassTint[0], kGrassTint[2]);
    const float baseLum = kGrassTint[0] * 0.2126f + kGrassTint[1] * 0.7152f + kGrassTint[2] * 0.0722f;
    const Vec3 baseChroma = {kGrassTint[0] - baseLum, kGrassTint[1] - baseLum, kGrassTint[2] - baseLum};
    const float baseChromaLen =
        std::sqrt(baseChroma[0] * baseChroma[0] + baseChroma[1] * baseChroma[1] + baseChroma[2] * baseChroma[2]);
    for (const Vec3& p : VariationSamplePositions())
    {
        const Vec3 c = PlanetLayerVariation(kGrassTint, p[0], p[1], p[2], kGrassScale, kGrassStrength,
                                            kGrassSat, 0.0f);
        // Channel order preserved -> never purple.
        EXPECT_GE(c[1], c[0]) << "grass lost green dominance (would read olive/other)";
        EXPECT_GE(c[0], c[2]) << "grass blue rose above red -> purple cast (the pre-fix chroma bug)";
        // Chroma direction preserved (cosine ~ 1 with the tint's chroma).
        const float lum = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
        const Vec3 chroma = {c[0] - lum, c[1] - lum, c[2] - lum};
        const float len = std::sqrt(chroma[0] * chroma[0] + chroma[1] * chroma[1] + chroma[2] * chroma[2]);
        if (len > 1e-4f && baseChromaLen > 1e-4f)
        {
            const float cosSim = (chroma[0] * baseChroma[0] + chroma[1] * baseChroma[1] +
                                  chroma[2] * baseChroma[2]) /
                                 (len * baseChromaLen);
            EXPECT_GT(cosSim, 0.999f) << "variation shifted the hue direction";
        }
    }
}

// The variation Nyquist-fades to the flat tint as the pixel footprint grows past the noise cell:
// the deviation from the tint decreases monotonically to exactly zero (the distant-shimmer /
// grazing-streak fix). At footprint 0 it must still deviate (break-up retained — no #616 regression).
TEST(CBTPlanetShading, LayerVariationFadesToFlatTintWithFootprint)
{
    const Vec3 dir = Normalize({0.3f, 0.9f, 0.32f});
    const Vec3 p = {dir[0] * 50000.0f + 3.1f, dir[1] * 50000.0f + 7.7f, dir[2] * 50000.0f};
    auto deviation = [&](float footprint) {
        const Vec3 c = PlanetLayerVariation(kRockTint, p[0], p[1], p[2], kRockScale, kRockStrength,
                                            kRockSat, footprint);
        return std::fabs(c[0] - kRockTint[0]) + std::fabs(c[1] - kRockTint[1]) +
               std::fabs(c[2] - kRockTint[2]);
    };
    const float cell = 1.0f / kRockScale; // metres per noise cell
    const float d0 = deviation(0.0f);
    EXPECT_GT(d0, 0.01f) << "variation must still break up the flat tint at full detail (no regression)";
    // cellsPerPixel = footprint * scale; fade reaches 0 at cellsPerPixel = kVariationFadeHi.
    float prev = d0;
    for (float cpp = 0.1f; cpp <= 1.5f; cpp += 0.1f)
    {
        const float dev = deviation(cpp * cell);
        EXPECT_LE(dev, prev + 1e-4f) << "deviation must be monotonically non-increasing with footprint";
        prev = dev;
    }
    EXPECT_NEAR(deviation((kVariationFadeHi + 0.05f) * cell), 0.0f, 1e-5f)
        << "past the fade band the variation must be exactly the flat tint";
}
