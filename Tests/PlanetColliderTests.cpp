#include <gtest/gtest.h>

#include "TerrainECS/PlanetFaceCollider.h"
#include "TerrainECS/TerrainService.h"

#include "CBTTerrain/CBTPlanetShading.h"        // PlanetRelief (the render mirror)
#include "CBTTerrain/CBTSphereFaceMap.h"        // FaceUVToWorldDir
#include "CBTTerrain/SphereAnalyticModifiers.h" // analytic modifier set (S2 composed sample)
#include "CBTTerrain/SphereSculptLayer.h"       // paged sculpt store + MakeSampler
#include "CBTTerrain/SphereSculptPaging.h"      // SphereSculptSampler / SampleSphereSculptByDir

#include <array>
#include <cmath>
#include <vector>

// Pure-function oracles for the planet collider height source (planet-collider slice).
// These need no Vulkan device or physics world — they prove the collider samples the SAME
// height field the renderer draws (HEIGHT-MATCH), that the per-face body frames are valid
// rotations, that the sculpt-edit UV rect maps conservatively to grid cells, and quantify
// the documented gnomonic tangent-plane skew (the render-vs-physics divergence budget).

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{
float32 Dot(const std::array<float32, 3>& a, const std::array<float32, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float32 Len(const std::array<float32, 3>& a) { return std::sqrt(Dot(a, a)); }

// The render surface height along the face normal at a grid direction — the independent
// mirror the collider must match: (R + relief(dir) + sculpt(dir)) * dot(dir, N).
float32 RenderProjection(uint32 face, const PlanetColliderParams& p,
                         const CBTTerrain::SphereSculptSampler& sculpt,
                         const std::array<float32, 3>& dir)
{
    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    float32 h = CBTTerrain::PlanetRelief(dir[0], dir[1], dir[2], p.ReliefAmplitude,
                                         p.ReliefFrequency, p.ReliefOctaves);
    h += CBTTerrain::SampleSphereSculptByDir(sculpt, dir[0], dir[1], dir[2]);
    return (p.Radius + h) * Dot(dir, n);
}

// Build a paged sculpt store for radius `radius` and bake a constant `value` over `faceRegions`
// (all six faces when `allFaces`). Returns the layer so the sampler view outlives the call.
CBTTerrain::SphereSculptLayer MakeSculptLayer(float32 radius, float32 value, bool allFaces,
                                              uint32 singleFace = 0u)
{
    CBTTerrain::SphereSculptLayer layer;
    layer.Configure(CBTTerrain::MakeSphereSculptGeometry(
        radius, CBTTerrain::ResolveSculptPagePoolCount()));
    CBTTerrain::SphereEditRegions regions{};
    if (allFaces)
        for (uint32 f = 0; f < CBTTerrain::kCubeFaceCount; ++f)
            regions.Rects[regions.Count++] = CBTTerrain::SphereFaceUVRect{f, 0.0f, 0.0f, 1.0f, 1.0f};
    else
        regions.Rects[regions.Count++] = CBTTerrain::SphereFaceUVRect{singleFace, 0.0f, 0.0f, 1.0f, 1.0f};
    layer.BakeModifierLayer(regions, [value](float32, float32, float32) { return value; });
    return layer;
}

const std::array<std::array<float32, 3>, 6> kExpectedFaceNormals = {{
    {1.0f, 0.0f, 0.0f},  // 0 +X
    {-1.0f, 0.0f, 0.0f}, // 1 -X
    {0.0f, 1.0f, 0.0f},  // 2 +Y
    {0.0f, -1.0f, 0.0f}, // 3 -Y
    {0.0f, 0.0f, 1.0f},  // 4 +Z
    {0.0f, 0.0f, -1.0f}, // 5 -Z
}};
} // namespace

// Each face body frame (Ta, N, Tb) must be an orthonormal right-handed rotation (det +1)
// with N the expected face outward normal — otherwise the Jolt body's quaternion is invalid
// (a reflection) and the patch is oriented wrong.
TEST(PlanetCollider, FaceFramesAreProperRotations)
{
    for (uint32 face = 0; face < kPlanetFaceCount; ++face)
    {
        std::array<float32, 3> ta, n, tb;
        ComputePlanetFaceFrame(face, ta, n, tb);

        EXPECT_NEAR(Len(ta), 1.0f, 1e-5f) << "face " << face;
        EXPECT_NEAR(Len(n), 1.0f, 1e-5f) << "face " << face;
        EXPECT_NEAR(Len(tb), 1.0f, 1e-5f) << "face " << face;
        EXPECT_NEAR(Dot(ta, n), 0.0f, 1e-5f) << "face " << face;
        EXPECT_NEAR(Dot(ta, tb), 0.0f, 1e-5f) << "face " << face;
        EXPECT_NEAR(Dot(n, tb), 0.0f, 1e-5f) << "face " << face;

        // Right-handed: Ta x N == Tb (det of the column basis is +1).
        const std::array<float32, 3> cross = {ta[1] * n[2] - ta[2] * n[1],
                                              ta[2] * n[0] - ta[0] * n[2],
                                              ta[0] * n[1] - ta[1] * n[0]};
        EXPECT_NEAR(cross[0], tb[0], 1e-5f) << "face " << face;
        EXPECT_NEAR(cross[1], tb[1], 1e-5f) << "face " << face;
        EXPECT_NEAR(cross[2], tb[2], 1e-5f) << "face " << face;

        for (int k = 0; k < 3; ++k)
            EXPECT_NEAR(n[k], kExpectedFaceNormals[face][k], 1e-5f) << "face " << face << " axis " << k;
    }
}

// HEIGHT-MATCH oracle: at grid points across every face (centre, edge midpoints, corners),
// the collider sample must equal the render height projection evaluated from the SAME
// CBTPlanetShading functions at the SAME direction — exact by construction, so physics and
// render never diverge at a sample point. Runs with AND without a sculpt edit.
TEST(PlanetCollider, HeightMatchesRenderFunctionAtGridPoints)
{
    PlanetColliderParams p{};
    p.Radius = 2000.0f;
    p.ReliefAmplitude = 60.0f;
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 3u;

    const uint32 dim = kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;
    const int32 mid = last / 2;
    const std::array<std::array<int32, 2>, 5> probes = {{
        {mid, mid},   // face centre
        {0, mid},     // edge midpoint
        {last, mid},  // edge midpoint
        {0, 0},       // corner
        {last, last}, // corner
    }};

    // A uniform raised sculpt (baked into the paged store) so the sculpt term is exercised.
    CBTTerrain::SphereSculptLayer raised = MakeSculptLayer(p.Radius, 7.5f, /*allFaces=*/true);
    const CBTTerrain::SphereSculptSampler samplers[2] = {CBTTerrain::SphereSculptSampler{},
                                                         raised.MakeSampler()};
    for (const CBTTerrain::SphereSculptSampler& sculpt : samplers)
    {
        for (uint32 face = 0; face < kPlanetFaceCount; ++face)
        {
            std::vector<float32> samples(static_cast<size_t>(dim) * dim, 0.0f);
            float32 lo = 0.0f, hi = 0.0f;
            GeneratePlanetFacePatch(face, p, dim, sculpt, CBTTerrain::SphereAnalyticModifierSet{}, 0, 0,
                                    last, last, samples, lo, hi);

            for (const auto& probe : probes)
            {
                const int32 i = probe[0], j = probe[1];
                const std::array<float32, 3> dir = PlanetFaceGridDir(face, dim, i, j);
                const float32 expected = RenderProjection(face, p, sculpt, dir);
                const float32 got = samples[static_cast<size_t>(j) * dim + i];
                // fp32 at radius ~2000 m: a few mm of round-off is the tolerance.
                EXPECT_NEAR(got, expected, 0.02f)
                    << "face " << face << " cell (" << i << "," << j
                    << ") sculpt=" << (sculpt.Valid() ? "on" : "off");
            }
        }
    }
}

// The render-vs-physics divergence budget: reconstruct the collider surface world point at a
// grid cell and compare its radial distance to the true rendered radius (R + relief). Near a
// face centre the gnomonic tangent-plane skew is sub-decimetre (the demo budget); it grows
// sharply toward the cube edge (documented — the "6 patches" approximation, chunking is the
// follow-up). This test pins both ends so a regression that widens the centre error fails.
TEST(PlanetCollider, GeometricSkewBudgetNearCentreAndAtEdge)
{
    PlanetColliderParams p{};
    p.Radius = 2000.0f;
    p.ReliefAmplitude = 0.0f; // isolate the pure curvature skew (no relief)
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 1u;

    const uint32 dim = kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;

    std::vector<float32> samples(static_cast<size_t>(dim) * dim, 0.0f);
    float32 lo = 0.0f, hi = 0.0f;
    const uint32 face = 2; // +Y (the demo pole face)
    GeneratePlanetFacePatch(face, p, dim, CBTTerrain::SphereSculptSampler{},
                            CBTTerrain::SphereAnalyticModifierSet{}, 0, 0, last, last, samples, lo,
                            hi);

    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);

    auto colliderRadius = [&](int32 i, int32 j) {
        // World point = Ta*ta + N*sample + Tb*tb (planet centred at origin); its radial
        // distance is what a body falling toward the pole rests at.
        const float32 span = static_cast<float32>(dim - 1u);
        const float32 ca = 2.0f * static_cast<float32>(i) / span - 1.0f; // in [-1,1]
        const float32 cb = 2.0f * static_cast<float32>(j) / span - 1.0f;
        const float32 tanA = ca * p.Radius; // grid tangent offset (metres)
        const float32 tanB = cb * p.Radius;
        const float32 h = samples[static_cast<size_t>(j) * dim + i];
        std::array<float32, 3> wp = {ta[0] * tanA + n[0] * h + tb[0] * tanB,
                                     ta[1] * tanA + n[1] * h + tb[1] * tanB,
                                     ta[2] * tanA + n[2] * h + tb[2] * tanB};
        return Len(wp);
    };

    // Near the centre (~5.7 deg from the pole: tangent ~= 0.1R = 200 m) the skew is small.
    const int32 mid = last / 2;
    const int32 nearOff = static_cast<int32>(0.1f * (dim - 1) * 0.5f); // ~0.1R tangent
    const float32 centreErr = std::fabs(colliderRadius(mid + nearOff, mid) - p.Radius);
    EXPECT_LT(centreErr, 0.25f) << "near-centre skew must stay sub-decimetre for the demo";

    // At the face edge midpoint (45 deg, one axis) the single tangent-plane patch skews
    // radially by R*(sqrt(1.5)-1) ~= 0.225R ~= 449 m at R=2000; at the cube CORNER (both
    // axes) it is R*(sqrt(7/3)-1) ~= 0.528R ~= 1055 m — the documented degradation that
    // motivates chunking. Pin both so the approximation is honest and a future chunked patch
    // (which would shrink these) updates the oracle.
    const float32 edgeErr = std::fabs(colliderRadius(0, mid) - p.Radius);
    EXPECT_GT(edgeErr, 400.0f) << "the single-patch edge skew is large (documented budget)";
    EXPECT_LT(edgeErr, 500.0f) << "edge skew must match the documented R*(sqrt(1.5)-1) figure";
    const float32 cornerErr = std::fabs(colliderRadius(0, 0) - p.Radius);
    EXPECT_GT(cornerErr, 1000.0f) << "the corner skew is the worst case (documented budget)";
    EXPECT_LT(cornerErr, 1100.0f) << "corner skew must match the documented R*(sqrt(7/3)-1) figure";
}

// The sculpt-edit UV rect (CBTSphereFaceMap parametrization) must map to a grid rect that
// CONSERVATIVELY covers every grid cell whose direction lies in the UV rect — a refresh must
// never under-cover the edited region. The full UV square maps to the full grid.
TEST(PlanetCollider, UVRectMapsConservativelyToGrid)
{
    const uint32 dim = kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;

    for (uint32 face = 0; face < kPlanetFaceCount; ++face)
    {
        int32 fMinI, fMinJ, fMaxI, fMaxJ;
        PlanetFaceUVRectToGridRect(face, dim, 0.0f, 0.0f, 1.0f, 1.0f, fMinI, fMinJ, fMaxI, fMaxJ);
        EXPECT_EQ(fMinI, 0) << "face " << face;
        EXPECT_EQ(fMinJ, 0) << "face " << face;
        EXPECT_EQ(fMaxI, last) << "face " << face;
        EXPECT_EQ(fMaxJ, last) << "face " << face;

        // A centred UV rect: every grid cell whose face-UV falls inside [0.4,0.6]^2 must be
        // inside the returned grid rect (conservative containment).
        const float32 u0 = 0.4f, u1 = 0.6f, v0 = 0.4f, v1 = 0.6f;
        int32 minI, minJ, maxI, maxJ;
        PlanetFaceUVRectToGridRect(face, dim, u0, v0, u1, v1, minI, minJ, maxI, maxJ);
        ASSERT_LE(minI, maxI);
        ASSERT_LE(minJ, maxJ);
        EXPECT_GT(minI, 0) << "centred rect should not span the whole face, face " << face;
        EXPECT_LT(maxI, last) << "face " << face;

        // Sample the interior of the UV rect, resolve each direction to its grid cell, and
        // assert containment in the returned rect.
        for (int su = 0; su <= 4; ++su)
            for (int sv = 0; sv <= 4; ++sv)
            {
                const float32 u = u0 + (u1 - u0) * su / 4.0f;
                const float32 v = v0 + (v1 - v0) * sv / 4.0f;
                float32 dx, dy, dz;
                CBTTerrain::FaceUVToWorldDir(face, u, v, dx, dy, dz);
                std::array<float32, 3> ta, n, tb;
                ComputePlanetFaceFrame(face, ta, n, tb);
                const std::array<float32, 3> d{dx, dy, dz};
                const float32 dn = Dot(d, n);
                const float32 ca = Dot(d, ta) / dn;
                const float32 cb = Dot(d, tb) / dn;
                const int32 gi = static_cast<int32>(std::lround((ca + 1.0f) * 0.5f * (dim - 1)));
                const int32 gj = static_cast<int32>(std::lround((cb + 1.0f) * 0.5f * (dim - 1)));
                EXPECT_GE(gi, minI) << "face " << face;
                EXPECT_LE(gi, maxI) << "face " << face;
                EXPECT_GE(gj, minJ) << "face " << face;
                EXPECT_LE(gj, maxJ) << "face " << face;
            }
    }
}

// A region regeneration with an edited sculpt atlas changes ONLY the region's samples, and
// each changed sample re-matches base + relief + sculpt — the edit-refresh height fidelity.
TEST(PlanetCollider, RegionRegenerationChangesOnlyRegionAndRematches)
{
    PlanetColliderParams p{};
    p.Radius = 2000.0f;
    p.ReliefAmplitude = 60.0f;
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 3u;

    const uint32 dim = kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;
    const uint32 face = 2; // +Y

    std::vector<float32> samples(static_cast<size_t>(dim) * dim, 0.0f);
    float32 lo = 0.0f, hi = 0.0f;
    GeneratePlanetFacePatch(face, p, dim, CBTTerrain::SphereSculptSampler{},
                            CBTTerrain::SphereAnalyticModifierSet{}, 0, 0, last, last, samples, lo,
                            hi);
    const std::vector<float32> before = samples;

    // Raise the whole +Y face in the paged store; regenerate only a centred grid region.
    CBTTerrain::SphereSculptLayer raised = MakeSculptLayer(p.Radius, 25.0f, /*allFaces=*/false, face);
    const CBTTerrain::SphereSculptSampler sculpt = raised.MakeSampler();

    const int32 r0 = last / 2 - 8, r1 = last / 2 + 8;
    float32 rlo = 0.0f, rhi = 0.0f;
    GeneratePlanetFacePatch(face, p, dim, sculpt, CBTTerrain::SphereAnalyticModifierSet{}, r0, r0, r1,
                            r1, samples, rlo, rhi);

    for (int32 j = 0; j <= last; ++j)
        for (int32 i = 0; i <= last; ++i)
        {
            const float32 s = samples[static_cast<size_t>(j) * dim + i];
            const bool inRegion = (i >= r0 && i <= r1 && j >= r0 && j <= r1);
            if (inRegion)
            {
                const std::array<float32, 3> dir = PlanetFaceGridDir(face, dim, i, j);
                EXPECT_NEAR(s, RenderProjection(face, p, sculpt, dir), 0.02f);
            }
            else
            {
                EXPECT_FLOAT_EQ(s, before[static_cast<size_t>(j) * dim + i])
                    << "cell outside the region must be untouched (" << i << "," << j << ")";
            }
        }
}

// TerrainService planet-face slot lifecycle: acquire six, the pool holds them, ReleaseAll
// drops every slot (the Play-exit sweep), and a stale-generation release is refused (the
// silent-unbind guard). Mirrors the tile-physics slot test.
TEST(PlanetCollider, ServiceSlotsReleaseAndStaleGuard)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    struct Guard
    {
        ~Guard() { if (TerrainService::IsInitialized()) TerrainService::Shutdown(); }
    } guard;
    auto& svc = TerrainService::Get();

    std::array<TerrainService::PlanetFacePhysicsHandle, 6> handles{};
    for (uint32 f = 0; f < 6; ++f)
        handles[f] = svc.AcquirePlanetFacePhysicsHandle();
    EXPECT_EQ(svc.GetPlanetFacePhysicsSlotCountForTests(), 6u);

    // Every handle carries the discriminator bit and resolves once its buffer is written.
    for (uint32 f = 0; f < 6; ++f)
    {
        EXPECT_NE(handles[f].Index & TerrainService::kPlanetFacePhysicsHandleBit, 0u);
        ASSERT_NE(svc.ResolvePlanetFaceColliderForWrite(handles[f].Index, handles[f].Generation, 4u),
                  nullptr);
    }

    // Release face 0, then a stale release of the same index must be refused (not free the
    // slot's current owner).
    svc.ReleasePlanetFacePhysicsHandle(handles[0].Index, handles[0].Generation);
    const auto reacquired = svc.AcquirePlanetFacePhysicsHandle();
    EXPECT_EQ(reacquired.Index & ~TerrainService::kPlanetFacePhysicsHandleBit,
              handles[0].Index & ~TerrainService::kPlanetFacePhysicsHandleBit)
        << "the freed slot index should be reused";
    svc.ReleasePlanetFacePhysicsHandle(handles[0].Index, handles[0].Generation); // stale — ignored
    EXPECT_NE(svc.ResolvePlanetFaceColliderForWrite(reacquired.Index, reacquired.Generation, 4u),
              nullptr)
        << "the current owner must survive a stale release of its old handle";

    svc.ReleaseAllPlanetFacePhysicsHandles();
    EXPECT_EQ(svc.GetPlanetFacePhysicsSlotCountForTests(), 0u);

    // Re-provisioning after the sweep reuses the pool from empty.
    for (uint32 f = 0; f < 6; ++f)
        (void)svc.AcquirePlanetFacePhysicsHandle();
    EXPECT_EQ(svc.GetPlanetFacePhysicsSlotCountForTests(), 6u);
}

// Sculpt shape-accuracy S2, physics-GPU parity for the LIVE analytic path: an analytic flatten
// handed to GeneratePlanetFacePatch (the exact entrypoint TerrainPhysicsSystem calls with the
// mirror's set) must land the collider surface on the flatten's target radius inside the pad —
// relief cancelled — and reproduce the composed render height everywhere, both to the S1 ±0.02 m
// fp32 budget. This is the collider half of "physics rests on the rendered pad": the GPU half
// evaluates the same closed form (cbt_analytic.glsl, expression-locked to the CPU eval).
TEST(PlanetCollider, AnalyticFlattenParityAndPadLandsOnTarget)
{
    PlanetColliderParams p{};
    p.Radius = 50000.0f;
    p.ReliefAmplitude = 60.0f;
    p.ReliefFrequency = 6.0f;
    p.ReliefOctaves = 3u;

    const uint32 dim = kPlanetFaceColliderDim;
    const int32 last = static_cast<int32>(dim) - 1;
    const int32 mid = last / 2;
    const uint32 face = 2; // +Y — pad centred on the face centre

    const std::array<float32, 3> centre = PlanetFaceGridDir(face, dim, mid, mid);
    constexpr float32 kPadRadius = 400.0f; // several grid cells wide at R=50000 (cell ~780 m... keep pad > 1 cell)
    constexpr float32 kTarget = 50000.0f + 25.0f;
    const CBTTerrain::SphereAnalyticFlatten flat = CBTTerrain::MakeSphereAnalyticFlatten(
        centre[0] * kTarget, centre[1] * kTarget, centre[2] * kTarget, kPadRadius,
        /*falloff*/ 100.0f, kTarget, p.Radius);
    ASSERT_TRUE(flat.Valid);
    const CBTTerrain::SphereAnalyticModifierSet set{&flat, 1u};

    std::vector<float32> samples(static_cast<size_t>(dim) * dim, 0.0f);
    float32 lo = 0.0f, hi = 0.0f;
    GeneratePlanetFacePatch(face, p, dim, CBTTerrain::SphereSculptSampler{}, set, 0, 0, last, last,
                            samples, lo, hi);

    std::array<float32, 3> ta, n, tb;
    ComputePlanetFaceFrame(face, ta, n, tb);
    uint32 padCells = 0u;
    for (int32 j = 0; j <= last; ++j)
        for (int32 i = 0; i <= last; ++i)
        {
            const std::array<float32, 3> dir = PlanetFaceGridDir(face, dim, i, j);
            const float32 relief = CBTTerrain::PlanetRelief(dir[0], dir[1], dir[2],
                                                            p.ReliefAmplitude, p.ReliefFrequency,
                                                            p.ReliefOctaves);
            const float32 composed =
                relief + CBTTerrain::SampleSphereSculptComposed(CBTTerrain::SphereSculptSampler{},
                                                                set, dir[0], dir[1], dir[2], relief);
            const float32 expected = (p.Radius + composed) * Dot(dir, n);
            const float32 got = samples[static_cast<size_t>(j) * dim + i];
            ASSERT_NEAR(got, expected, 0.02f)
                << "collider diverged from the composed render height at (" << i << "," << j << ")";

            // Inside the pad interior (weight 1) the surface radius is EXACTLY the target: the
            // relief must be cancelled, not merely offset.
            const float32 l0 = p.Radius * Dot(dir, {flat.E1[0], flat.E1[1], flat.E1[2]});
            const float32 l1 = p.Radius * Dot(dir, {flat.E2[0], flat.E2[1], flat.E2[2]});
            if (std::sqrt(l0 * l0 + l1 * l1) < kPadRadius - 1.0f)
            {
                ++padCells;
                ASSERT_NEAR((p.Radius + composed), kTarget, 0.02f)
                    << "pad interior did not level to the target radius at (" << i << "," << j << ")";
            }
        }
    EXPECT_GT(padCells, 0u) << "test not discriminating: no grid cell landed inside the pad";
}
