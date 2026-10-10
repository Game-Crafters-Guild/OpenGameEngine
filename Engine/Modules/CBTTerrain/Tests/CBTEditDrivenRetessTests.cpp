// Headless oracle for edit-driven retessellation (round-8b): a live sculpt edit must refine the CBT
// triangulation at the crease WITHOUT any camera motion. The screen-space AREA metric reacts only to
// a facet's projected SIZE, which is invariant to what the facet approximates — so a coarse facet
// straddling a freshly-cut cliff keeps its subdivision and the wall renders as a jagged stair-step of
// coarse facets. The user's workaround (move the camera far so everything merges, then re-approach so
// it re-splits against the new heights) is exactly what this oracle forbids: the camera never moves.
//
// This drives the REAL GPU update kernels (RecordUpdate, screen-space Classify) over the paged sculpt
// store, then reads back HeapID + CurrentVertex and measures the projected geometric error at the
// split-edge midpoint of every facet whose split edge crosses the cliff. The pass condition mirrors
// the shader's crease term: once converged, no visible crease facet may project more than the pixel
// error the area metric targets. On main (no crease term) the coarse facets never refine, so the
// max crease error stays an order of magnitude over threshold and this test fails — the fails-before.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout (sector tail appended; corner/meta word offsets unchanged)
constexpr double kScreenW = 1600.0;
constexpr double kScreenH = 900.0;
constexpr double kSplitPx = 8.0; // TargetPixelError analog (matches CBTDemandAuditTests)
constexpr double kMergePx = 4.0;

struct Proj
{
    double Px = 0.0;
    double Py = 0.0;
    bool InFront = false;
};

// Project a world point through the column-major viewProj (render origin inactive in this test, so
// viewProjRel == the world viewProj) to viewport pixels. Mirror of CBT_ProjectPixels.
Proj ProjectPix(const float m[16], double x, double y, double z)
{
    const double cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const double cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    const double cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    Proj r;
    r.InFront = cw > 1e-5;
    if (!r.InFront)
        return r;
    r.Px = (cx / cw * 0.5 + 0.5) * kScreenW;
    r.Py = (cy / cw * 0.5 + 0.5) * kScreenH;
    return r;
}

double Length3(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

// Barycentric weights of (pu,pv) in the UV triangle (a,b,c). inside == all weights >= -eps.
struct Bary
{
    double W0 = 0.0, W1 = 0.0, W2 = 0.0;
    bool Inside = false;
};
Bary BarycentricUV(double pu, double pv, double au, double av, double bu, double bv, double cu,
                   double cv)
{
    Bary r;
    const double v0u = bu - au, v0v = bv - av;
    const double v1u = cu - au, v1v = cv - av;
    const double v2u = pu - au, v2v = pv - av;
    const double den = v0u * v1v - v1u * v0v;
    if (std::fabs(den) < 1e-24)
        return r;
    r.W1 = (v2u * v1v - v1u * v2v) / den; // weight for b
    r.W2 = (v0u * v2v - v2u * v0v) / den; // weight for c
    r.W0 = 1.0 - r.W1 - r.W2;             // weight for a
    constexpr double kEps = 1e-5;
    r.Inside = r.W0 >= -kEps && r.W1 >= -kEps && r.W2 >= -kEps;
    return r;
}
} // namespace

class CBTEditDrivenRetess : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no headless Vulkan device";
        if (!m_Device->GetCapabilities().supportsShaderInt64)
            GTEST_SKIP() << "device lacks shaderInt64";
        if (!m_KernelSet.Initialize(*m_Device, ShaderOutputDir()))
            GTEST_SKIP() << "cbt_kernels.comp.spv missing (glslc unavailable at build)";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    void RunFrame(CBTInstance& inst, const SphereSculptLayer& layer, bool uploadSculpt,
                  const CBTClassifyDesc& desc, const CBTFrameParams& params, uint32_t frame)
    {
        if (uploadSculpt)
        {
            inst.UploadSphereSculptPool(frame, layer.Pool().data(),
                                        static_cast<uint32_t>(layer.Pool().size()));
            inst.UploadSphereSculptPageTable(frame, layer.PageTable().data(),
                                             static_cast<uint32_t>(layer.PageTable().size()));
        }
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::vector<float> ReadVerts(CBTInstance& inst)
    {
        const auto w = inst.DebugReadWords(CBTBinding::CurrentVertex,
                                                 kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        std::vector<float> f(w.size());
        std::memcpy(f.data(), w.data(), w.size() * sizeof(uint32_t));
        return f;
    }
    std::vector<uint64_t> ReadHeap(CBTInstance& inst)
    {
        const auto w = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        std::vector<uint64_t> h(kDefaultBisectorPoolSize);
        for (uint32_t i = 0; i < kDefaultBisectorPoolSize; ++i)
            h[i] = static_cast<uint64_t>(w[i * 2u]) | (static_cast<uint64_t>(w[i * 2u + 1u]) << 32);
        return h;
    }

    // The worst-case projected geometric error over VISIBLE facets whose split edge crosses the
    // cliff. This is the exact quantity the shader crease term thresholds: the deviation of the flat
    // facet from the sculpted surface at the split-edge midpoint, in screen pixels.
    double MaxVisibleCreaseErrorPx(CBTInstance& inst, const CBTFrameParams& p,
                                   const SphereSculptSampler& sampler, float radius,
                                   uint32_t baseDepth, uint32_t& outCreaseCount, uint32_t& outLiveCount)
    {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> v = ReadVerts(inst);
        double maxErrPx = 0.0;
        uint32_t creaseCount = 0;
        uint32_t liveCount = 0;
        const float kCreaseGateM = 0.25f; // mirror CBT_CREASE_GATE_M
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = heap[s];
            if (h == 0u)
                continue;
            ++liveCount;
            const uint32_t base = s * kVertexWordsPerSlot;
            const double c0x = v[base + 0], c0y = v[base + 1], c0z = v[base + 2];
            const double c2x = v[base + 8], c2y = v[base + 9], c2z = v[base + 10];
            const double h0 = Length3(c0x, c0y, c0z) - radius;
            const double h2 = Length3(c2x, c2y, c2z) - radius;
            if (std::fabs(h0 - h2) <= kCreaseGateM)
                continue; // flat split edge — not a crease facet

            // Split-edge midpoint direction + the shader's exact face-local midpoint UV (the corner
            // face-UVs ride corner.w / meta, and the cube->faceUV map is affine so the midpoint UV is
            // their average). Face is the subdivision-invariant root ancestor / 4.
            uint32_t depth = 0;
            for (uint64_t t = h; t > 1u; t >>= 1)
                ++depth;
            const uint32_t rootIndex =
                static_cast<uint32_t>(h >> (depth - baseDepth)) - (1u << baseDepth);
            if (rootIndex >= kSphereRootCount)
                continue;
            const uint32_t face = rootIndex / kSlicesPerFace;
            const float fu0 = v[base + 3], fv0 = v[base + 12];
            const float fu2 = v[base + 11], fv2 = v[base + 14];
            const float midU = 0.5f * (fu0 + fu2);
            const float midV = 0.5f * (fv0 + fv2);
            const double scMid = SampleSculptFaceUV(sampler, face, midU, midV);
            const double scLerp = 0.5 * (h0 + h2); // relief amplitude 0 -> composed height == sculpt
            const double errM = std::fabs(scMid - scLerp);

            const double mx = 0.5 * (c0x + c2x), my = 0.5 * (c0y + c2y), mz = 0.5 * (c0z + c2z);
            const double ml = Length3(mx, my, mz);
            if (ml <= 0.0)
                continue;
            const double dx = mx / ml, dy = my / ml, dz = mz / ml;
            const double hLerp = 0.5 * (h0 + h2);
            const Proj pb = ProjectPix(p.ViewProjRel, dx * (radius + hLerp), dy * (radius + hLerp),
                                       dz * (radius + hLerp));
            const Proj pt = ProjectPix(p.ViewProjRel, dx * (radius + hLerp + errM),
                                       dy * (radius + hLerp + errM), dz * (radius + hLerp + errM));
            if (!pb.InFront || !pt.InFront)
                continue; // behind the eye — the crease term leaves these to the area metric
            // On-screen (generous margin) so an off-frustum crease does not skew the visible max.
            if (pb.Px < -kScreenW || pb.Px > 2.0 * kScreenW || pb.Py < -kScreenH ||
                pb.Py > 2.0 * kScreenH)
                continue;
            ++creaseCount;
            maxErrPx = std::max(maxErrPx, std::hypot(pb.Px - pt.Px, pb.Py - pt.Py));
        }
        outCreaseCount = creaseCount;
        outLiveCount = liveCount;
        return maxErrPx;
    }

    // Projected approximation error of the live triangulation AT a single edited point (the disc
    // centre). Unlike MaxVisibleCreaseErrorPx this does NOT gate on a corner-height gradient — it is
    // exactly the measurement the corner gate is blind to. Find the live facet(s) whose face-local UV
    // triangle contains (centreU, centreV), barycentric-interpolate the three corner heights there
    // (the flat facet's linear approximation of the surface), and compare against the true edited
    // height at the centre. On main the coarse facet containing an interior edit never subdivides, so
    // its flat approximation stays ~0 while the true height is riseM -> a large projected error that
    // never shrinks. Returns the worst projected error (px) over containing facets and reports the
    // deepest containing facet + the split-edge world length of the coarse container (the "interior
    // regime" witness: facet >> disc so both split-edge corners lie on the flat surround).
    double InteriorEditCenterErrorPx(CBTInstance& inst, const CBTFrameParams& p, uint32_t targetFace,
                                     double centreU, double centreV, double dirX, double dirY,
                                     double dirZ, double trueHeightM, float radius, uint32_t baseDepth,
                                     uint32_t& outContainCount, uint32_t& outMaxContainDepth,
                                     double& outCoarseEdgeLenM, double& outCornerGradientM)
    {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> v = ReadVerts(inst);
        double maxErrPx = 0.0;
        uint32_t containCount = 0;
        uint32_t maxContainDepth = 0;
        outCoarseEdgeLenM = 0.0;
        outCornerGradientM = 0.0;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = heap[s];
            if (h == 0u)
                continue;
            uint32_t depth = 0;
            for (uint64_t t = h; t > 1u; t >>= 1)
                ++depth;
            if (depth < baseDepth)
                continue;
            const uint32_t rootIndex =
                static_cast<uint32_t>(h >> (depth - baseDepth)) - (1u << baseDepth);
            if (rootIndex >= kSphereRootCount || rootIndex / kSlicesPerFace != targetFace)
                continue;
            const uint32_t base = s * kVertexWordsPerSlot;
            const double u0 = v[base + 3], v0 = v[base + 12];
            const double u1 = v[base + 7], v1 = v[base + 13];
            const double u2 = v[base + 11], v2 = v[base + 14];
            const Bary b = BarycentricUV(centreU, centreV, u0, v0, u1, v1, u2, v2);
            if (!b.Inside)
                continue;
            const double h0 = Length3(v[base + 0], v[base + 1], v[base + 2]) - radius;
            const double h1 = Length3(v[base + 4], v[base + 5], v[base + 6]) - radius;
            const double h2 = Length3(v[base + 8], v[base + 9], v[base + 10]) - radius;
            const double approxHeight = b.W0 * h0 + b.W1 * h1 + b.W2 * h2;
            const Proj pb = ProjectPix(p.ViewProjRel, dirX * (radius + approxHeight),
                                       dirY * (radius + approxHeight), dirZ * (radius + approxHeight));
            const Proj pt = ProjectPix(p.ViewProjRel, dirX * (radius + trueHeightM),
                                       dirY * (radius + trueHeightM), dirZ * (radius + trueHeightM));
            if (pb.InFront && pt.InFront)
                maxErrPx = std::max(maxErrPx, std::hypot(pb.Px - pt.Px, pb.Py - pt.Py));
            ++containCount;
            if (depth >= maxContainDepth)
            {
                maxContainDepth = depth;
                // Split-edge (corner0..corner2) world length + its corner height gradient — the coarse
                // container witnesses the interior regime (edge >> disc, corners equal-height).
                outCoarseEdgeLenM = Length3(v[base + 0] - v[base + 8], v[base + 1] - v[base + 9],
                                            v[base + 2] - v[base + 10]);
                outCornerGradientM = std::fabs(h0 - h2);
            }
        }
        outContainCount = containCount;
        outMaxContainDepth = maxContainDepth;
        return maxErrPx;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// Raise a plateau on a smooth planet, hold the camera dead still, and require the crease to resolve.
// Fails on main (the coarse crease never refines without camera motion); passes once Classify carries
// the geometric-error term.
TEST_F(CBTEditDrivenRetess, StationaryEditRefinesCliffCreaseWithoutCameraMotion)
{
    using namespace GameEngine::Mathematics;

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    const uint32_t baseDepth = inst.GetBaseDepth();

    // R=2000 (the demand-audit's proven pool-fit radius): the crease MECHANISM — the area metric's
    // blindness to a sub-facet cliff — is radius-independent, so this validates it in a pool with
    // clear headroom, free of the global capacity limit that saturates the pool at R=50000 (that
    // scaling limit is orthogonal to this slice and is the editor A/B's job). Her actual R=50000
    // planet is verified live in the editor.
    const float radius = 2000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom);
    const uint32_t cap = baseDepth + kMaxDecodeSubdiv;

    // Screen-space params. relief amplitude 0 -> the composed corner height IS the sculpt, so the
    // oracle's height math is exact. The camera looks OBLIQUELY across the plateau so the near-vertical
    // cliff projects to real pixels (a top-down look would foreshorten the radial error to ~0).
    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = 0.0f; // amplitude
    p.PlanetParams[2] = 6.0f;
    p.PlanetParams[3] = 1.0f;
    p.AtlasParams0[0] = static_cast<float>(geom.VirtualDim);
    p.AtlasParams0[1] = static_cast<float>(geom.Cap);
    p.AtlasParams0[2] = static_cast<float>(geom.PagesPerAxis);
    p.AtlasParams0[3] = static_cast<float>(geom.PoolPageCount);
    p.Screen[0] = static_cast<float>(kScreenW);
    p.Screen[1] = static_cast<float>(kScreenH);
    p.Screen[2] = static_cast<float>(kSplitPx);
    p.Screen[3] = static_cast<float>(kMergePx);
    p.TerrainOrigin[2] = static_cast<float>(cap);

    // Plateau centre direction (near +Z, off the pole/face-centre to avoid singularities).
    const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
    const Vector3 surf = dir * radius;
    // A radial "up" and a tangent to stand the camera off along.
    const Vector3 tangent = Vector3::Cross(dir, Vector3(0, 1, 0)).Normalize();
    const float alt = 120.0f;      // camera altitude above the surface
    const float standoff = 350.0f; // tangential stand-off so the look is oblique across the rim
    const Vector3 eye = surf + dir * alt - tangent * standoff;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.1f, 16.0f / 9.0f, 0.05f, 200000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, surf, dir);
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;

    // The editor's shipped demand shaping (CBTRenderFeature BuildFrameParams): near-bias coarsens
    // the far field so the fixed pool is spent near the camera, and the walking-headroom tuning
    // bounds the behind-eye/grazing over-refinement. WITHOUT this the smooth hemisphere at splitPx=8
    // saturates the pool (chronic overflow) and leaves no headroom for the crease — so the oracle
    // must run the SAME shaping the editor does, or it tests an unrepresentatively saturated pool.
    const float camLen = static_cast<float>(Length3(eye.x, eye.y, eye.z));
    const float altitude = std::max(camLen - radius, 1.0f);
    const CBTNearBiasRadii nb = ComputeNearBiasRadii(altitude);
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = 0.5f;
    p.DemandTuning[1] = 0.9f;
    p.DemandTuning[2] = 3.0f * static_cast<float>(kSplitPx);
    p.DemandTuning[3] = 0.9f;

    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;
    c.SphereSculptEnabled = 0u;

    // ---- Phase 1: converge the SMOOTH sphere at the fixed pose (no edits). ----
    for (uint32_t f = 0; f < 90u; ++f)
    {
        c.GateVertexEval = (f == 0u) ? 0u : 1u;
        RunFrame(inst, layer, /*uploadSculpt=*/false, c, p, f);
    }

    // ---- Phase 2: raise a plateau (a sharp cliff). A cap edit: +riseM inside the cap, 0 outside. ----
    const float ang = 0.05f;   // angular cap radius (rad) -> ~100 m plateau at R=2000
    const float riseM = 60.0f; // plateau height above the smooth surface (a tall, unambiguous cliff)
    const auto capNorm = dir;
    const float capCos = std::cos(ang);
    SphereEditRegions regions = ClassifySphereCapEdit(dir.x, dir.y, dir.z, ang);
    ASSERT_GT(regions.Count, 0u);
    layer.BakeModifierLayer(regions, [capNorm, capCos, riseM](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        if (l <= 0.0f)
            return 0.0f;
        const float d = (x * capNorm.x + y * capNorm.y + z * capNorm.z) / l;
        return d >= capCos ? riseM : 0.0f;
    });
    const SphereSculptSampler sampler = layer.MakeSampler();

    // Edit lands: enable the sculpt, force a full re-eval (the round-8 editing-frame behavior), hand
    // Classify the edit's dirty rect this ONE frame (the transient pulse the editor emits).
    c.SphereSculptEnabled = 1u;
    c.GateVertexEval = 0u;
    c.DirtyFace = regions.Rects[0].Face;
    c.DirtyMinU = regions.Rects[0].MinU;
    c.DirtyMinV = regions.Rects[0].MinV;
    c.DirtyMaxU = regions.Rects[0].MaxU;
    c.DirtyMaxV = regions.Rects[0].MaxV;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 90u);

    uint32_t creaseBefore = 0;
    uint32_t liveBefore = 0;
    const double errBeforePx =
        MaxVisibleCreaseErrorPx(inst, p, sampler, radius, baseDepth, creaseBefore, liveBefore);
    std::printf("[edit-retess] after edit: live=%u creaseFacets=%u maxCreaseErrorPx=%.1f (target<=%.1f)\n",
                liveBefore, creaseBefore, errBeforePx, kSplitPx);
    ASSERT_GT(creaseBefore, 0u) << "the plateau must produce facets whose split edge crosses the cliff";
    EXPECT_GT(errBeforePx, kSplitPx * 2.0)
        << "the freshly-edited crease must start UNDER-resolved (a coarse facet spanning the cliff) — "
           "if this is already small the scenario failed to reproduce the jagged wall";

    // ---- Phase 3: hold the camera DEAD STILL, gated re-eval, EMPTY dirty rect (no transient pulse).
    // With no camera motion and no dirty pulse, only the persistent crease term can drive refinement. ----
    c.GateVertexEval = 1u;
    c.DirtyFace = 0u;
    c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
    constexpr uint32_t kStationaryFrames = 24u;
    for (uint32_t f = 0; f < kStationaryFrames; ++f)
    {
        RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 91u + f);
        if (f < 4u)
        {
            uint32_t cc = 0, lc = 0;
            const double e = MaxVisibleCreaseErrorPx(inst, p, sampler, radius, baseDepth, cc, lc);
            std::printf("[edit-retess]  frame %u: live=%u crease=%u maxErrPx=%.2f evald=%d overflow=%d\n",
                        f, lc, cc, e, inst.ReadVertexEvalCount(),
                        inst.ReadWorkQueueCounter(kWQOverflowCounter));
        }
    }

    uint32_t creaseAfter = 0;
    uint32_t liveAfter = 0;
    const double errAfterPx =
        MaxVisibleCreaseErrorPx(inst, p, sampler, radius, baseDepth, creaseAfter, liveAfter);
    std::printf("[edit-retess] after %u stationary frames: live=%u creaseFacets=%u maxCreaseErrorPx=%.2f\n",
                kStationaryFrames, liveAfter, creaseAfter, errAfterPx);

    // The fix's guarantee: with the camera never moving, the crease resolves until no visible facet
    // straddling the cliff projects more than the pixel budget the area metric targets (a small
    // hysteresis margin for the held band). On main this stays ~errBeforePx and the test fails.
    EXPECT_LT(errAfterPx, kSplitPx * 2.0)
        << "a stationary edit must refine the cliff crease to the pixel target without camera motion "
           "(fails on main: the area metric never re-splits a coarse facet whose projected size did "
           "not change)";
    EXPECT_GT(creaseAfter, creaseBefore)
        << "convergence must ADD crease facets (subdivision concentrated at the cliff)";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the refined tree must stay conforming";
}

// The interior-edit oracle — the case the #610 corner-gradient gate MISSED (her twice-reported
// complaint). At a large radius viewed from altitude the coarse facets are hundreds of metres, so a
// 100 m plateau lands INTERIOR to one facet: both split-edge corners sit on the flat surround (equal
// heights) and the sculpt lives only at the split-edge MIDPOINT. The old gate's cheap early-exit
// (abs(cornerHeight0 - cornerHeight2) <= 0.25 m) declared the facet flat and never sampled the
// midpoint, so it never refined until camera motion shrank facets enough for the corners to straddle
// the plateau. This holds the camera DEAD STILL and requires the residency gate to sample the midpoint
// (the plateau's page is resident) and refine. Fails on the corner-gate shader (the interior facet
// stays coarse, disc-centre error pinned at ~riseM); passes on the residency gate.
TEST_F(CBTEditDrivenRetess, InteriorEditRefinesFlatFacetWithoutCameraMotion)
{
    using namespace GameEngine::Mathematics;

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    const uint32_t baseDepth = inst.GetBaseDepth();

    // Her regime: a large planet viewed from altitude so the converged facets are hundreds of metres —
    // the scale at which a 100 m edit is INTERIOR to a facet (the R=2000 #610 oracle had facets small
    // relative to the disc, so its corners straddled the crease and the corner gate fired).
    const float radius = 20000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    SphereSculptLayer layer;
    layer.Configure(geom);
    const uint32_t cap = baseDepth + kMaxDecodeSubdiv;

    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = 0.0f; // amplitude 0 -> composed corner height IS the sculpt (exact oracle math)
    p.PlanetParams[2] = 6.0f;
    p.PlanetParams[3] = 1.0f;
    p.AtlasParams0[0] = static_cast<float>(geom.VirtualDim);
    p.AtlasParams0[1] = static_cast<float>(geom.Cap);
    p.AtlasParams0[2] = static_cast<float>(geom.PagesPerAxis);
    p.AtlasParams0[3] = static_cast<float>(geom.PoolPageCount);
    p.Screen[0] = static_cast<float>(kScreenW);
    p.Screen[1] = static_cast<float>(kScreenH);
    p.Screen[2] = static_cast<float>(kSplitPx);
    p.Screen[3] = static_cast<float>(kMergePx);
    p.TerrainOrigin[2] = static_cast<float>(cap);

    // Oblique high-altitude view near +Z. altitude is large so the near facets converge to ~hundreds
    // of metres at splitPx=8 (facet edge ~= splitPx * dist / focal); the plateau then sits inside one.
    const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
    const Vector3 surf = dir * radius;
    const Vector3 tangent = Vector3::Cross(dir, Vector3(0, 1, 0)).Normalize();
    const float alt = 15000.0f;      // camera altitude above the surface
    const float standoff = 22000.0f; // tangential stand-off (oblique look across the plateau)
    const Vector3 eye = surf + dir * alt - tangent * standoff;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.1f, 16.0f / 9.0f, 0.5f, 4000000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, surf, dir);
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;

    // Shipped demand shaping (CBTRenderFeature BuildFrameParams): near-bias coarsens the far field so
    // the fixed pool is spent near the camera, leaving headroom for the crease (the same shaping the
    // R=2000 oracle runs — without it the far hemisphere saturates the pool at splitPx=8).
    const float camLen = static_cast<float>(Length3(eye.x, eye.y, eye.z));
    const float altitude = std::max(camLen - radius, 1.0f);
    const CBTNearBiasRadii nb = ComputeNearBiasRadii(altitude);
    p.NearBias[0] = 1.0f;
    p.NearBias[1] = nb.NearRadius;
    p.NearBias[2] = nb.FarRadius;
    p.NearBias[3] = kNearBiasMaxCoarsen;
    p.DemandTuning[0] = 0.5f;
    p.DemandTuning[1] = 0.9f;
    p.DemandTuning[2] = 3.0f * static_cast<float>(kSplitPx);
    p.DemandTuning[3] = 0.9f;

    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;
    c.SphereSculptEnabled = 0u;

    // ---- Phase 1: converge the SMOOTH sphere at the fixed pose (no edits). ----
    for (uint32_t f = 0; f < 110u; ++f)
    {
        c.GateVertexEval = (f == 0u) ? 0u : 1u;
        RunFrame(inst, layer, /*uploadSculpt=*/false, c, p, f);
    }

    // ---- Phase 2: locate the coarse facet under `dir` and centre the plateau on its split-edge
    // MIDPOINT, so the edit is guaranteed interior (both split-edge corners on the flat surround) and
    // the split-edge midpoint — the point the residency gate must sample — lands dead centre. ----
    const SphereFaceUV targetFuv = WorldDirToFaceUV(dir.x, dir.y, dir.z);
    Vector3 discDir = dir;
    double coarseEdgeM = 0.0;
    {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> vv = ReadVerts(inst);
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = heap[s];
            if (h == 0u)
                continue;
            uint32_t depth = 0;
            for (uint64_t t = h; t > 1u; t >>= 1)
                ++depth;
            if (depth < baseDepth)
                continue;
            const uint32_t rootIndex =
                static_cast<uint32_t>(h >> (depth - baseDepth)) - (1u << baseDepth);
            if (rootIndex >= kSphereRootCount || rootIndex / kSlicesPerFace != targetFuv.Face)
                continue;
            const uint32_t base = s * kVertexWordsPerSlot;
            const double u0 = vv[base + 3], v0 = vv[base + 12];
            const double u1 = vv[base + 7], v1 = vv[base + 13];
            const double u2 = vv[base + 11], v2 = vv[base + 14];
            const Bary b = BarycentricUV(targetFuv.U, targetFuv.V, u0, v0, u1, v1, u2, v2);
            if (!b.Inside)
                continue;
            // Midpoint of the split edge (corner0..corner2) — the LEB bisection point + the residency
            // sample point. Centre the plateau there.
            const double mx = 0.5 * (vv[base + 0] + vv[base + 8]);
            const double my = 0.5 * (vv[base + 1] + vv[base + 9]);
            const double mz = 0.5 * (vv[base + 2] + vv[base + 10]);
            const double ml = Length3(mx, my, mz);
            discDir = Vector3(static_cast<float>(mx / ml), static_cast<float>(my / ml),
                              static_cast<float>(mz / ml));
            coarseEdgeM = Length3(vv[base + 0] - vv[base + 8], vv[base + 1] - vv[base + 9],
                                  vv[base + 2] - vv[base + 10]);
            break;
        }
    }
    ASSERT_GT(coarseEdgeM, 0.0) << "no live facet found under the target direction";
    // Interior regime witness: the containing facet must be far larger than the 100 m plateau, so both
    // split-edge corners land on the flat surround (equal heights) — the case the corner gate misses.
    const float discRadiusM = 100.0f;
    const float discAng = discRadiusM / radius; // angular cap radius
    std::printf("[interior-edit] coarse facet split-edge=%.0f m, plateau radius=%.0f m (interior=%s)\n",
                coarseEdgeM, discRadiusM, coarseEdgeM > 2.5 * discRadiusM ? "yes" : "NO");
    ASSERT_GT(coarseEdgeM, 2.5 * discRadiusM)
        << "the containing facet must dwarf the plateau so the edit is interior (raise altitude if not)";

    const SphereFaceUV discFuv = WorldDirToFaceUV(discDir.x, discDir.y, discDir.z);

    // ---- Phase 3: raise a tall, narrow plateau centred on the facet midpoint. ----
    const float riseM = 800.0f; // tall enough its radial error projects well past splitPx from altitude
    const Vector3 capNorm = discDir;
    const float capCos = std::cos(discAng);
    SphereEditRegions regions = ClassifySphereCapEdit(discDir.x, discDir.y, discDir.z, discAng);
    ASSERT_GT(regions.Count, 0u);
    layer.BakeModifierLayer(regions, [capNorm, capCos, riseM](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        if (l <= 0.0f)
            return 0.0f;
        const float d = (x * capNorm.x + y * capNorm.y + z * capNorm.z) / l;
        return d >= capCos ? riseM : 0.0f;
    });

    // Apply the edit's HEIGHT (full re-eval + the transient dirty pulse) but hold the crease term OFF
    // (EditRetessEnabled=0). For an INTERIOR edit that is behaviourally identical to main's corner
    // gate: neither samples the midpoint, so neither refines. This isolates the fix — the ONLY thing
    // that changes between the two phases below is the crease term firing.
    c.SphereSculptEnabled = 1u;
    c.EditRetessEnabled = 0u;
    c.GateVertexEval = 0u;
    c.DirtyFace = regions.Rects[0].Face;
    c.DirtyMinU = regions.Rects[0].MinU;
    c.DirtyMinV = regions.Rects[0].MinV;
    c.DirtyMaxU = regions.Rects[0].MaxU;
    c.DirtyMaxV = regions.Rects[0].MaxV;
    RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 110u);

    // ---- Phase "before": hold DEAD STILL with the crease term OFF. The interior plateau must stay
    // UNREFINED (this is what main does to an interior edit — the corner gate never fires). ----
    c.GateVertexEval = 1u;
    c.DirtyFace = 0u;
    c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
    for (uint32_t f = 0; f < 12u; ++f)
        RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 111u + f);

    uint32_t containBefore = 0, depthBefore = 0;
    double edgeBefore = 0.0, gradientBefore = 0.0;
    const double errBeforePx =
        InteriorEditCenterErrorPx(inst, p, discFuv.Face, discFuv.U, discFuv.V, discDir.x, discDir.y,
                                  discDir.z, riseM, radius, baseDepth, containBefore, depthBefore,
                                  edgeBefore, gradientBefore);
    std::printf("[interior-edit] crease OFF, 12 still frames: containDepth=%u edge=%.0fm cornerGrad=%.3fm "
                "centreErrPx=%.1f (target<=%.1f)\n",
                depthBefore, edgeBefore, gradientBefore, errBeforePx, kSplitPx);
    ASSERT_GT(containBefore, 0u) << "a facet must contain the disc centre";
    // The discriminator vs #610: the container's split-edge corners are EQUAL height — the corner gate
    // sees "flat" and skips. This is precisely the interior blind spot.
    EXPECT_LE(gradientBefore, 0.25)
        << "the interior container's split-edge corners must be equal-height (the corner gate's blind "
           "spot) — if they already differ this is a straddle, not the interior case #610 missed";
    EXPECT_GT(errBeforePx, kSplitPx * 2.0)
        << "with the crease term OFF a stationary interior plateau stays UNDER-resolved (a coarse flat "
           "facet spanning it) — this is the stuck state main leaves an interior edit in";

    // ---- Phase "after": flip the crease term ON, camera STILL, empty dirty rect. Only the residency-
    // gated crease term can drive refinement now (no camera motion, no dirty pulse). On main's corner
    // gate this phase changes NOTHING for the interior edit — that is the fails-before. ----
    c.EditRetessEnabled = 1u;
    constexpr uint32_t kStationaryFrames = 32u;
    for (uint32_t f = 0; f < kStationaryFrames; ++f)
    {
        RunFrame(inst, layer, /*uploadSculpt=*/true, c, p, 123u + f);
        if (f < 6u)
        {
            uint32_t cc = 0, dd = 0;
            double e0 = 0.0, g0 = 0.0;
            const double e = InteriorEditCenterErrorPx(inst, p, discFuv.Face, discFuv.U, discFuv.V,
                                                       discDir.x, discDir.y, discDir.z, riseM, radius,
                                                       baseDepth, cc, dd, e0, g0);
            std::printf("[interior-edit]  crease ON frame %u: containDepth=%u centreErrPx=%.2f overflow=%d\n",
                        f, dd, e, inst.ReadWorkQueueCounter(kWQOverflowCounter));
        }
    }

    uint32_t containAfter = 0, depthAfter = 0;
    double edgeAfter = 0.0, gradientAfter = 0.0;
    const double errAfterPx =
        InteriorEditCenterErrorPx(inst, p, discFuv.Face, discFuv.U, discFuv.V, discDir.x, discDir.y,
                                  discDir.z, riseM, radius, baseDepth, containAfter, depthAfter,
                                  edgeAfter, gradientAfter);
    std::printf("[interior-edit] crease ON, %u still frames: containDepth=%u (was %u) centreErrPx=%.2f\n",
                kStationaryFrames, depthAfter, depthBefore, errAfterPx);

    // The fix's guarantee: with the camera never moving, flipping the residency-gated crease term ON
    // refines the interior plateau until its approximation at the edited point is under the pixel
    // budget. On the corner-gate shader this phase never subdivides the container, so it stays
    // ~errBeforePx and both assertions fail — the fails-before.
    EXPECT_LT(errAfterPx, kSplitPx * 2.0)
        << "a stationary INTERIOR edit must refine once the crease term is on (fails on the corner-gate "
           "shader: the flat facet's corners match, so the midpoint is never sampled)";
    EXPECT_GT(depthAfter, depthBefore)
        << "convergence must subdivide the container (the plateau is resolved by deeper facets)";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the refined tree must stay conforming";
}

// ============================================================================
// Round-8e saturation-deadlock oracles. At a moderate viewing altitude over a
// large relief planet (her scene: R=50000, relief 1250) the visible-plus-descent
// demand saturates the fixed bisector pool. Once saturated the topology DEADLOCKS:
// splits cannot allocate (FreeCount ~= 0) and the LEB merge throttles to ~0 (the
// distance-graded screen-space metric leaves a staircased tree with almost no
// uniform-depth mergeable diamonds), so NO static-camera event can update the
// tessellation — her "updates only happen when the camera moves". The near-field
// force-split occupancy gate (NearFieldGate) stops the invisible near-plane/behind-
// eye force-split treadmill that pins the pool at 100%, restoring the headroom that
// lets a static-camera edit / TargetPixelError change re-tessellate. These oracles
// drive the REAL GPU kernels through the exact descent-then-static path and A/B the
// gate (fails-before with NearFieldGate=0, passes-after with NearFieldGate=1).
// ============================================================================
class CBTSaturationRetess : public CBTEditDrivenRetess
{
  protected:
    struct AreaStats
    {
        uint32_t Live = 0;
        uint32_t Vis = 0;
    };

    // Live count + count of facets whose three corners project in front and on-screen.
    AreaStats SphereVisStats(CBTInstance& inst, const CBTFrameParams& p)
    {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> v = ReadVerts(inst);
        AreaStats st;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue;
            ++st.Live;
            const uint32_t b = s * kVertexWordsPerSlot;
            const Proj p0 = ProjectPix(p.ViewProjRel, v[b + 0], v[b + 1], v[b + 2]);
            const Proj p1 = ProjectPix(p.ViewProjRel, v[b + 4], v[b + 5], v[b + 6]);
            const Proj p2 = ProjectPix(p.ViewProjRel, v[b + 8], v[b + 9], v[b + 10]);
            if (!p0.InFront || !p1.InFront || !p2.InFront)
                continue;
            const double cx = (p0.Px + p1.Px + p2.Px) / 3.0;
            const double cy = (p0.Py + p1.Py + p2.Py) / 3.0;
            if (cx < 0.0 || cx > kScreenW || cy < 0.0 || cy > kScreenH)
                continue;
            ++st.Vis;
        }
        return st;
    }

    // Her scene shape with the editor's shipped demand shaping (near-bias + demand-tuning +
    // priority). altM = camera altitude above the surface; oblique look across the sub-camera point.
    void BuildHerParams(CBTFrameParams& p, float radius, float altM, float splitPx, float mergePx,
                        const SphereSculptGeometry& geom)
    {
        using namespace GameEngine::Mathematics;
        p = CBTFrameParams{};
        p.PlanetParams[0] = radius;
        p.PlanetParams[1] = 1250.0f; // relief amplitude (her mountains)
        p.PlanetParams[2] = 6.0f;
        p.PlanetParams[3] = 4.0f;
        p.AtlasParams0[0] = static_cast<float>(geom.VirtualDim);
        p.AtlasParams0[1] = static_cast<float>(geom.Cap);
        p.AtlasParams0[2] = static_cast<float>(geom.PagesPerAxis);
        p.AtlasParams0[3] = static_cast<float>(geom.PoolPageCount);
        p.Screen[0] = static_cast<float>(kScreenW);
        p.Screen[1] = static_cast<float>(kScreenH);
        p.Screen[2] = splitPx;
        p.Screen[3] = mergePx;
        p.TerrainOrigin[2] = static_cast<float>(5u + kMaxDecodeSubdiv); // maxDepth cap (spherical baseDepth 5)
        const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
        const Vector3 surf = dir * radius;
        const Vector3 tangent = Vector3::Cross(dir, Vector3(0, 1, 0)).Normalize();
        const Vector3 eye = surf + dir * altM - tangent * (altM * 1.4f);
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.1f, 16.0f / 9.0f, 0.5f, 8000000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, surf, dir);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        const float camLen = static_cast<float>(Length3(eye.x, eye.y, eye.z));
        const float altitude = std::max(camLen - radius, 1.0f);
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(altitude);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = 0.5f;
        p.DemandTuning[1] = 0.9f;
        p.DemandTuning[2] = 3.0f * splitPx;
        p.DemandTuning[3] = 0.9f;
        p.PriorityParams[0] = 1.0f;
        p.PriorityParams[1] = 0.85f;
        p.PriorityParams[2] = 48.0f;
        p.PriorityParams[3] = 1.5f;
    }

    // Descend from orbit to restAltM (camera motion builds the tree, as her navigation does),
    // holding TargetPixelError = splitPx. Returns the next frame index. c.NearFieldGate selects
    // the A/B branch. The abrupt descent leaves the same saturated-and-staircased tree her smooth
    // navigation does — the state from which a static edit must still update.
    uint32_t DescendToSaturation(CBTInstance& inst, SphereSculptLayer& layer, CBTClassifyDesc& c,
                                 float radius, float restAltM, float splitPx, const SphereSculptGeometry& geom)
    {
        const float descent[] = {160000.0f, 60000.0f, 24000.0f, 12000.0f, 7000.0f, restAltM};
        uint32_t frame = 0;
        for (int di = 0; di < 6; ++di)
        {
            CBTFrameParams p;
            BuildHerParams(p, radius, descent[di], splitPx, splitPx * 0.5f, geom);
            for (uint32_t f = 0; f < 40u; ++f)
            {
                c.GateVertexEval = (frame == 0u) ? 0u : 1u;
                RunFrame(inst, layer, false, c, p, frame);
                ++frame;
            }
        }
        return frame;
    }
};

// THE core fix demonstration. Descend to saturate at alt 4000, then hold the camera DEAD STILL and
// LOWER TargetPixelError. With the near-field gate OFF (main) the pool is pinned at 100% and the
// visible facet count does NOT respond to the lower TPE — the topology is frozen (her "lowering
// doesn't show differences until movement"). With the gate ON the pool keeps headroom and the
// visible field refines to the lower TPE with the camera never moving.
TEST_F(CBTSaturationRetess, LoweringTpeRefinesUnderSaturationStaticCamera)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    auto run = [&](uint32_t gate, uint32_t& visHi, uint32_t& visLo, double& occHi) {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
        const uint32_t cap = inst.GetBaseDepth() + kMaxDecodeSubdiv;
        SphereSculptLayer layer;
        layer.Configure(geom);
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = cap;
        c.NearFieldGate = gate;
        uint32_t frame = DescendToSaturation(inst, layer, c, radius, 4000.0f, 6.5f, geom);
        // Static hold at TPE 6.5, then at TPE 2.0 — the camera never moves between them.
        CBTFrameParams pHi;
        BuildHerParams(pHi, radius, 4000.0f, 6.5f, 3.25f, geom);
        for (uint32_t f = 0; f < 70u; ++f, ++frame)
        {
            c.GateVertexEval = 1u;
            RunFrame(inst, layer, false, c, pHi, frame);
        }
        const AreaStats hi = SphereVisStats(inst, pHi);
        visHi = hi.Vis;
        occHi = static_cast<double>(hi.Live) / kDefaultBisectorPoolSize;
        CBTFrameParams pLo;
        BuildHerParams(pLo, radius, 4000.0f, 2.0f, 1.0f, geom); // SAME camera, lower TPE
        for (uint32_t f = 0; f < 70u; ++f, ++frame)
        {
            c.GateVertexEval = 1u;
            RunFrame(inst, layer, false, c, pLo, frame);
        }
        visLo = SphereVisStats(inst, pLo).Vis;
    };

    uint32_t offHi = 0, offLo = 0, onHi = 0, onLo = 0;
    double offOcc = 0.0, onOcc = 0.0;
    run(0u, offHi, offLo, offOcc); // fails-before: gate OFF (main)
    run(1u, onHi, onLo, onOcc);    // passes-after: gate ON (fix)

    const double offDelta = offHi ? (static_cast<double>(offLo) - offHi) / offHi : 0.0;
    const double onDelta = onHi ? (static_cast<double>(onLo) - onHi) / onHi : 0.0;
    std::printf("[tpe-oracle] GATE OFF: occ=%.3f visTPE6.5=%u visTPE2=%u (%.1f%%)  "
                "GATE ON: occ=%.3f visTPE6.5=%u visTPE2=%u (%.1f%%)\n",
                offOcc, offHi, offLo, offDelta * 100.0, onOcc, onHi, onLo, onDelta * 100.0);

    // Fails-before: the pool is pinned at ~100% and lowering TPE does not add visible facets.
    EXPECT_GE(offOcc, 0.995) << "gate OFF: the descent must saturate the pool (the deadlock precondition)";
    EXPECT_LT(offDelta, 0.01)
        << "gate OFF (main): lowering TargetPixelError with a static camera must NOT refine the "
           "visible field — the saturated topology is frozen (her reported symptom)";
    // Passes-after: the gate keeps headroom (below the crease ceiling) and the visible field responds.
    EXPECT_LE(onOcc, 0.96) << "gate ON: the near-field gate must keep the pool below the crease ceiling";
    EXPECT_GT(onDelta, 0.03)
        << "gate ON (fix): lowering TargetPixelError with a static camera must refine the visible "
           "field (splits happen immediately, matching what raising TPE already did)";
}

// Her TOP directive: an EDIT must re-tessellate with a static camera. Descend to saturate, raise a
// plateau (the sculpt edit), hold the camera DEAD STILL. Under saturation the crease term is gated
// off (occupancy >= the crease ceiling) AND its splits cannot allocate, so with the near-field gate
// OFF the fresh cliff stays a coarse stair-step (her "tessellation doesn't happen upon edit until
// the camera moves"). With the gate ON the pool drops below the crease ceiling and the edit refines.
TEST_F(CBTSaturationRetess, EditRetessellatesUnderSaturationStaticCamera)
{
    using namespace GameEngine::Mathematics;
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    auto run = [&](uint32_t gate, double& errAfterPx, uint32_t& creaseAfter, double& occ) {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
        const uint32_t baseDepth = inst.GetBaseDepth();
        const uint32_t cap = baseDepth + kMaxDecodeSubdiv;
        SphereSculptLayer layer;
        layer.Configure(geom);
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = cap;
        c.NearFieldGate = gate;
        uint32_t frame = DescendToSaturation(inst, layer, c, radius, 4000.0f, 6.5f, geom);

        // Raise a plateau centred under the camera (a sharp cliff). relief amplitude is nonzero here,
        // so the crease term isolates the sculpt layer (composed minus closed-form relief).
        const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
        const float discAng = 800.0f / radius; // ~800 m plateau
        const float riseM = 2500.0f;           // a tall, unambiguous cliff above the relief
        const Vector3 capNorm = dir;
        const float capCos = std::cos(discAng);
        SphereEditRegions regions = ClassifySphereCapEdit(dir.x, dir.y, dir.z, discAng);
        layer.BakeModifierLayer(regions, [capNorm, capCos, riseM](float x, float y, float z) {
            const float l = std::sqrt(x * x + y * y + z * z);
            if (l <= 0.0f)
                return 0.0f;
            const float d = (x * capNorm.x + y * capNorm.y + z * capNorm.z) / l;
            return d >= capCos ? riseM : 0.0f;
        });
        const SphereSculptSampler sampler = layer.MakeSampler();

        CBTFrameParams p;
        BuildHerParams(p, radius, 4000.0f, 6.5f, 3.25f, geom);
        // Edit lands: enable sculpt, force a full re-eval, hand Classify the dirty rect this frame.
        c.SphereSculptEnabled = 1u;
        c.GateVertexEval = 0u;
        c.DirtyFace = regions.Rects[0].Face;
        c.DirtyMinU = regions.Rects[0].MinU;
        c.DirtyMinV = regions.Rects[0].MinV;
        c.DirtyMaxU = regions.Rects[0].MaxU;
        c.DirtyMaxV = regions.Rects[0].MaxV;
        RunFrame(inst, layer, true, c, p, frame++);

        // Hold the camera DEAD STILL, gated re-eval, empty dirty rect: only the crease term can refine.
        c.GateVertexEval = 1u;
        c.DirtyFace = 0u;
        c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
        for (uint32_t f = 0; f < 48u; ++f, ++frame)
            RunFrame(inst, layer, true, c, p, frame);

        uint32_t liveAfter = 0;
        errAfterPx = MaxVisibleCreaseErrorPx(inst, p, sampler, radius, baseDepth, creaseAfter, liveAfter);
        occ = static_cast<double>(liveAfter) / kDefaultBisectorPoolSize;
    };

    double offErr = 0.0, onErr = 0.0, offOcc = 0.0, onOcc = 0.0;
    uint32_t offCrease = 0, onCrease = 0;
    run(0u, offErr, offCrease, offOcc); // fails-before: gate OFF
    run(1u, onErr, onCrease, onOcc);    // passes-after: gate ON
    std::printf("[edit-oracle] GATE OFF: occ=%.3f crease=%u maxErrPx=%.1f  GATE ON: occ=%.3f "
                "crease=%u maxErrPx=%.1f (target<=%.1f)\n",
                offOcc, offCrease, offErr, onOcc, onCrease, onErr, kSplitPx);

    // Fails-before: the saturated pool pins at 100%, which holds occupancy at/above the crease term's
    // occupancy ceiling — so the edit-driven crease term is gated OFF and the fresh cliff cannot refine
    // at all with a static camera (her "tessellation doesn't happen upon edit until the camera moves").
    EXPECT_GE(offOcc, 0.99)
        << "gate OFF (main): the descent+edit saturates the pool to ~100%, which gates the crease term "
           "off (occupancy >= the crease ceiling) — the deadlock precondition";
    EXPECT_GT(offErr, kSplitPx * 3.0)
        << "gate OFF (main): the static-camera edit leaves the cliff a coarse stair-step (her symptom)";
    // Passes-after: the near-field gate drops occupancy below the crease ceiling, so the crease term
    // RE-ENGAGES and concentrates fresh subdivision at the cliff — the edit is no longer frozen with a
    // static camera. NOTE: fully resolving a large cliff under HEAVY saturation is still merge-limited
    // (the residual invisible far field cannot drain — the architectural follow-up), so this asserts the
    // edit response is restored, not that the worst-case cliff facet reaches the pixel target.
    EXPECT_LT(onOcc, 0.98)
        << "gate ON (fix): occupancy drops below the crease occupancy ceiling so the term re-engages";
    EXPECT_GT(onCrease, offCrease)
        << "gate ON (fix): the re-engaged crease term adds fresh subdivision at the cliff with the "
           "camera never moving (the frozen edit response is restored)";
}

// Modifier DELETE (never tested anywhere): removing a modifier must RESTORE the footprint height
// (the sculpt contribution goes away) with a static camera. Coarsening the now-flat region back is
// merge-limited under saturation (the architectural follow-up), but the geometry — what she sees —
// must return to the base surface. This drives the sculpt re-bake + the force-full re-eval the
// editor issues on a sculpt-version change and checks the composed corner height at the footprint.
TEST_F(CBTSaturationRetess, ModifierDeleteRestoresFootprintHeightStaticCamera)
{
    using namespace GameEngine::Mathematics;
    const float radius = 20000.0f; // headroom radius: isolates the delete-restore from saturation
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    const uint32_t baseDepth = inst.GetBaseDepth();
    const uint32_t cap = baseDepth + kMaxDecodeSubdiv;
    SphereSculptLayer layer;
    layer.Configure(geom);
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;

    CBTFrameParams p;
    BuildHerParams(p, radius, 6000.0f, 6.5f, 3.25f, geom);
    p.PlanetParams[1] = 0.0f; // smooth base -> composed corner height IS the sculpt (exact oracle math)
    p.PlanetParams[3] = 1.0f;
    uint32_t frame = 0;
    for (uint32_t f = 0; f < 120u; ++f, ++frame)
    {
        c.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, layer, false, c, p, frame);
    }

    // Locate a live facet under the camera direction and take its corner0 direction as the probe.
    const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
    // Raise a plateau (create the modifier).
    const float discAng = 400.0f / radius;
    const float riseM = 900.0f;
    const Vector3 capNorm = dir;
    const float capCos = std::cos(discAng);
    SphereEditRegions regions = ClassifySphereCapEdit(dir.x, dir.y, dir.z, discAng);
    layer.BakeModifierLayer(regions, [capNorm, capCos, riseM](float x, float y, float z) {
        const float l = std::sqrt(x * x + y * y + z * z);
        if (l <= 0.0f)
            return 0.0f;
        const float d = (x * capNorm.x + y * capNorm.y + z * capNorm.z) / l;
        return d >= capCos ? riseM : 0.0f;
    });
    c.SphereSculptEnabled = 1u;
    c.GateVertexEval = 0u;
    c.DirtyFace = regions.Rects[0].Face;
    c.DirtyMinU = regions.Rects[0].MinU;
    c.DirtyMinV = regions.Rects[0].MinV;
    c.DirtyMaxU = regions.Rects[0].MaxU;
    c.DirtyMaxV = regions.Rects[0].MaxV;
    RunFrame(inst, layer, true, c, p, frame++);
    c.DirtyFace = 0u;
    c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
    c.GateVertexEval = 1u;
    for (uint32_t f = 0; f < 8u; ++f, ++frame)
        RunFrame(inst, layer, true, c, p, frame);

    // Max composed height over live corners near the plateau centre — the modifier raised it.
    auto maxHeightNearCentre = [&]() {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> v = ReadVerts(inst);
        double maxH = 0.0;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue;
            const uint32_t b = s * kVertexWordsPerSlot;
            const double cx = v[b + 0], cy = v[b + 1], cz = v[b + 2];
            const double l = Length3(cx, cy, cz);
            if (l <= 0.0)
                continue;
            const double d = (cx * dir.x + cy * dir.y + cz * dir.z) / l;
            if (d >= capCos)
                maxH = std::max(maxH, l - radius);
        }
        return maxH;
    };
    const double raisedH = maxHeightNearCentre();
    std::printf("[delete-oracle] after create: maxFootprintHeight=%.1f m (rise=%.0f)\n", raisedH, riseM);
    ASSERT_GT(raisedH, riseM * 0.5) << "the modifier must have raised the footprint";

    // DELETE the modifier: re-bake the sculpt WITHOUT it (empty stack over the footprint), bump the
    // version, force a full re-eval — exactly the editor's delete path. Camera never moves.
    layer.BakeModifierLayer(regions, [](float, float, float) { return 0.0f; });
    c.GateVertexEval = 0u; // force-full re-eval (the sculpt-version-change behavior)
    c.DirtyFace = regions.Rects[0].Face;
    c.DirtyMinU = regions.Rects[0].MinU;
    c.DirtyMinV = regions.Rects[0].MinV;
    c.DirtyMaxU = regions.Rects[0].MaxU;
    c.DirtyMaxV = regions.Rects[0].MaxV;
    RunFrame(inst, layer, true, c, p, frame++);
    c.DirtyFace = 0u;
    c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
    c.GateVertexEval = 1u;
    for (uint32_t f = 0; f < 8u; ++f, ++frame)
        RunFrame(inst, layer, true, c, p, frame);

    const double restoredH = maxHeightNearCentre();
    std::printf("[delete-oracle] after delete: maxFootprintHeight=%.2f m (was %.1f)\n", restoredH, raisedH);
    // The footprint must restore to the base surface (the modifier's height is gone), camera-still.
    EXPECT_LT(restoredH, riseM * 0.05)
        << "deleting the modifier must restore the footprint to the base surface with a static camera "
           "(the sculpt re-bake + force-full re-eval remove the height)";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the tree must stay conforming after the delete";
}

// Property CHANGE (Radius/Falloff/TargetHeight via the inspector): editing a live modifier's property
// re-bakes the sculpt at the NEW footprint and the geometry must follow with a static camera. Here the
// modifier's radius GROWS; a point that was outside the old disc (base height) must become raised once
// it falls inside the new disc — the property change propagated to the mesh, camera-still. Headroom
// radius so this isolates the property-change lifecycle from the saturation deadlock.
TEST_F(CBTSaturationRetess, PropertyChangeRetessellatesStaticCamera)
{
    using namespace GameEngine::Mathematics;
    const float radius = 20000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    const uint32_t cap = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    SphereSculptLayer layer;
    layer.Configure(geom);
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;

    CBTFrameParams p;
    BuildHerParams(p, radius, 6000.0f, 6.5f, 3.25f, geom);
    p.PlanetParams[1] = 0.0f; // smooth base -> composed corner height IS the sculpt (exact oracle math)
    p.PlanetParams[3] = 1.0f;
    uint32_t frame = 0;
    for (uint32_t f = 0; f < 120u; ++f, ++frame)
    {
        c.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, layer, false, c, p, frame);
    }

    const Vector3 dir = Vector3(0.03f, 0.0f, 1.0f).Normalize();
    const float riseM = 600.0f;
    const float smallAng = 200.0f / radius; // initial radius ~200 m
    const float bigAng = 800.0f / radius;   // grown radius ~800 m (the property change)
    auto bake = [&](float capAng) {
        const Vector3 capNorm = dir;
        const float capCos = std::cos(capAng);
        SphereEditRegions regions = ClassifySphereCapEdit(dir.x, dir.y, dir.z, capAng);
        layer.BakeModifierLayer(regions, [capNorm, capCos, riseM](float x, float y, float z) {
            const float l = std::sqrt(x * x + y * y + z * z);
            if (l <= 0.0f)
                return 0.0f;
            const float d = (x * capNorm.x + y * capNorm.y + z * capNorm.z) / l;
            return d >= capCos ? riseM : 0.0f;
        });
        return regions;
    };
    auto applyEdit = [&](const SphereEditRegions& regions) {
        c.SphereSculptEnabled = 1u;
        c.GateVertexEval = 0u; // force-full re-eval (the sculpt-version-change behavior)
        c.DirtyFace = regions.Rects[0].Face;
        c.DirtyMinU = regions.Rects[0].MinU;
        c.DirtyMinV = regions.Rects[0].MinV;
        c.DirtyMaxU = regions.Rects[0].MaxU;
        c.DirtyMaxV = regions.Rects[0].MaxV;
        RunFrame(inst, layer, true, c, p, frame++);
        c.DirtyFace = 0u;
        c.DirtyMinU = c.DirtyMinV = c.DirtyMaxU = c.DirtyMaxV = 0.0f;
        c.GateVertexEval = 1u;
        for (uint32_t f = 0; f < 10u; ++f, ++frame)
            RunFrame(inst, layer, true, c, p, frame);
    };
    // Max composed height in the annular band [smallAng, bigAng] — outside the old disc, inside the new.
    auto maxHeightInBand = [&]() {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<float> v = ReadVerts(inst);
        const double cosSmall = std::cos(smallAng * 1.3f); // strictly OUTSIDE the old disc
        const double cosBig = std::cos(bigAng * 0.85f);     // strictly INSIDE the grown disc
        double maxH = 0.0;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue;
            const uint32_t b = s * kVertexWordsPerSlot;
            const double cx = v[b + 0], cy = v[b + 1], cz = v[b + 2];
            const double l = Length3(cx, cy, cz);
            if (l <= 0.0)
                continue;
            const double d = (cx * dir.x + cy * dir.y + cz * dir.z) / l;
            if (d <= cosSmall && d >= cosBig) // inside the annular band
                maxH = std::max(maxH, l - radius);
        }
        return maxH;
    };

    applyEdit(bake(smallAng));
    const double bandBefore = maxHeightInBand();
    std::printf("[property-oracle] radius small: band height=%.2f m\n", bandBefore);
    EXPECT_LT(bandBefore, riseM * 0.25)
        << "before the property change the annular band is outside the disc — base height";

    // CHANGE the radius property (re-bake at the larger footprint). Camera never moves.
    applyEdit(bake(bigAng));
    const double bandAfter = maxHeightInBand();
    std::printf("[property-oracle] radius grown: band height=%.2f m (was %.2f)\n", bandAfter, bandBefore);
    EXPECT_GT(bandAfter, riseM * 0.5)
        << "changing the modifier radius must raise the newly-covered band with a static camera "
           "(the property change propagated to the mesh)";
    EXPECT_EQ(inst.ReadValidationErrorCount(), 0u) << "the tree must stay conforming after the change";
}
