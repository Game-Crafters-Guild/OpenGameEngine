// Demand-audit instrumentation for the CBT split/merge metric (clipmap/bisector-pool arc,
// reference-parity audit). #593 measured the pool 100% saturated with ~16 M dropped splits at
// the 150 m R=2000 surface pose, median facet frozen at ~33.5 m. The reference (AnisB/large_cbt,
// Benyoub & Dupuy 2024) renders an Earth-sized planet at ~64 k live triangles from the SAME
// 131072-slot pool with centimetric ground detail — so the saturation is our DEMAND model, not
// the method or the pool size.
//
// This test converges the real GPU CBT at the probe pose, reads the live pool back, and recomputes
// Kernel_Classify's split/merge decision on the CPU (bit-faithful to cbt_kernels.comp: the same
// projected split-edge length vs the 8 px threshold, the same behind-near-plane force-split, the
// same frustum extraction). It then ATTRIBUTES the per-frame split demand to disjoint causes —
// behind-near-plane, occluded far-side, off-frustum, and grazing (projected-edge-inflated but
// tiny-area) — and contrasts the edge-length demand with a screen-space AREA demand (the
// reference's metric). The money artifact: where the ~66 k/frame demand comes from when the
// visible screen budget justifies ~10^5, and whether an area metric bounds it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPlanetShading.h" // SphereCornerOccluded (horizon oracle mirror)
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout (sector tail appended; corner/meta word offsets unchanged)

uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1;
        ++d;
    }
    return d;
}

// Mirror of CBT_ProjectPixels (cbt_layout.glsl): column-major clip = M * (x,y,z,1); false when
// the point is at/behind the near plane (clip.w <= 1e-5). Column-major index = col*4 + row.
struct Proj
{
    double Px = 0.0, Py = 0.0, W = 0.0;
    bool InFront = false;
};
Proj ProjectPix(const float* m, double x, double y, double z, double sw, double sh)
{
    Proj r;
    r.W = static_cast<double>(m[3]) * x + m[7] * y + m[11] * z + m[15];
    if (r.W <= 1e-5)
        return r;
    const double cx = static_cast<double>(m[0]) * x + m[4] * y + m[8] * z + m[12];
    const double cy = static_cast<double>(m[1]) * x + m[5] * y + m[9] * z + m[13];
    r.Px = (cx / r.W * 0.5 + 0.5) * sw;
    r.Py = (cy / r.W * 0.5 + 0.5) * sh;
    r.InFront = true;
    return r;
}

// Mirror of CBT_TriangleInFrustum (cbt_layout.glsl): Gribb-Hartmann planes from the column-major
// world->clip rows; culls only when all three corners fall strictly outside one plane.
bool TriInFrustum(const float* m, const std::array<double, 3>& a, const std::array<double, 3>& b,
                  const std::array<double, 3>& c)
{
    const std::array<double, 4> r0 = {m[0], m[4], m[8], m[12]};
    const std::array<double, 4> r1 = {m[1], m[5], m[9], m[13]};
    const std::array<double, 4> r2 = {m[2], m[6], m[10], m[14]};
    const std::array<double, 4> r3 = {m[3], m[7], m[11], m[15]};
    std::array<std::array<double, 4>, 6> planes = {{{r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]},
                                                    {r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]},
                                                    {r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]},
                                                    {r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]},
                                                    {r3[0] + r2[0], r3[1] + r2[1], r3[2] + r2[2], r3[3] + r2[3]},
                                                    {r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]}}};
    for (const auto& pl : planes)
    {
        const double da = pl[0] * a[0] + pl[1] * a[1] + pl[2] * a[2] + pl[3];
        const double db = pl[0] * b[0] + pl[1] * b[1] + pl[2] * b[2] + pl[3];
        const double dc = pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3];
        if (da < 0.0 && db < 0.0 && dc < 0.0)
            return false;
    }
    return true;
}

double TriAreaPx(const Proj& p0, const Proj& p1, const Proj& p2)
{
    const double ux = p1.Px - p0.Px, uy = p1.Py - p0.Py;
    const double vx = p2.Px - p0.Px, vy = p2.Py - p0.Py;
    return 0.5 * std::abs(ux * vy - uy * vx);
}

CBTFrameParams SphereParams(float radius, float reliefAmp, float reliefFreq)
{
    CBTFrameParams p{};
    p.PlanetParams[0] = radius;
    p.PlanetParams[1] = reliefAmp;
    p.PlanetParams[2] = reliefFreq;
    return p;
}
} // namespace

class CBTDemandAuditTest : public ::testing::Test
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
            GTEST_SKIP() << "cbt_kernels.comp.spv missing";
    }
    void TearDown() override
    {
        m_KernelSet.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }
    void RunFrame(CBTInstance& instance, const CBTClassifyDesc& desc, const CBTFrameParams& params,
                  uint32_t frame)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        instance.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// THE MONEY ARTIFACT. Converge at the shipped 150 m R=2000 surface pose, then attribute the
// per-frame split demand. Prints a [demand] histogram; the assertions lock in the shape of the
// pathology so a fix can be diffed against it (they are deliberately loose — this test EXISTS to
// print, and to fail loudly if the demand ever stops being dominated by non-visible causes).
TEST_F(CBTDemandAuditTest, SplitDemandHistogramAtSurfacePose)
{
    using namespace GameEngine::Mathematics;
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));

    const float radius = 2000.0f;
    const float amp = 60.0f, freq = 6.0f;
    const float finalAlt = 150.0f;
    const double reliefFloor = radius - kReliefEnvelope * amp; // conservative horizon floor (1910 m)
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 20000.0f);
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    const double kSplitPx = 8.0, kMergePx = 4.0;
    const double kScreenW = 1600.0, kScreenH = 900.0;
    // Our edge threshold implies an area target: a right-isoceles LEB triangle whose split edge
    // (hypotenuse) is L has legs L/sqrt(2) and area L^2/4, so 8 px -> 16 px^2. The reference targets
    // ~49 px^2. Report the demand under both so the fix's budget is explicit.
    const double kAreaOurs = (kSplitPx * kSplitPx) / 4.0; // 16 px^2
    const double kAreaRef = 49.0;

    CBTFrameParams p = SphereParams(radius, amp, freq);
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = static_cast<float>(kScreenW);
    p.Screen[1] = static_cast<float>(kScreenH);
    p.Screen[2] = static_cast<float>(kSplitPx);
    p.Screen[3] = static_cast<float>(kMergePx);
    p.TerrainOrigin[2] = static_cast<float>(cap);

    CBTClassifyDesc classify{};
    classify.Mode = kClassifyScreenSpace;
    classify.TargetDepth = cap;

    auto poseAt = [&](float alt) {
        const Vector3 eye(0.0f, 0.0f, -(radius + alt));
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, Vector3(0, 0, 0), Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x; p.CameraPos[1] = eye.y; p.CameraPos[2] = eye.z; p.CameraPos[3] = 1.0f;
    };

    constexpr uint32_t kDescend = 160u;
    for (uint32_t f = 0; f < kDescend; ++f)
    {
        poseAt(std::max(finalAlt, 300.0f * std::pow(0.96f, static_cast<float>(f))));
        RunFrame(instance, classify, p, f);
    }
    poseAt(finalAlt);
    for (uint32_t f = 0; f < 40u; ++f)
        RunFrame(instance, classify, p, kDescend + f);

    // Per-frame dropped-split delta: the overflow counter is cumulative (Reset does not clear it),
    // so read it, run ONE more settled frame, and read again. The delta is the STEADY-STATE per-frame
    // demand the pool could not satisfy — the number a converged frame should drive to ~0.
    const int overflowBefore = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    poseAt(finalAlt);
    RunFrame(instance, classify, p, kDescend + 40u);
    const int overflowAfter = instance.ReadWorkQueueCounter(kWQOverflowCounter);
    const int perFrameOverflow = overflowAfter - overflowBefore;

    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    const auto verts = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                     kDefaultBisectorPoolSize * kVertexWordsPerSlot);

    // Attribution buckets (disjoint decision tree over the split demand).
    uint64_t live = 0, wantSplitEdge = 0, wantMergeEdge = 0;
    uint64_t behindNear = 0, occludedFar = 0, offFrustum = 0, visibleReal = 0, visibleGrazing = 0;
    uint64_t wantSplitAreaOurs = 0, wantSplitAreaRef = 0;
    uint64_t cantMergeOccluded = 0; // occluded/far-side bisectors that are NOT flagged to merge
    std::array<uint64_t, 48> demandByDepth{};

    for (uint32_t slot = 0; slot < kDefaultBisectorPoolSize; ++slot)
    {
        if (heap[slot * 2u] == 0u && heap[slot * 2u + 1u] == 0u)
            continue;
        ++live;
        const uint64_t h = static_cast<uint64_t>(heap[slot * 2u]) |
                           (static_cast<uint64_t>(heap[slot * 2u + 1u]) << 32);
        const uint32_t depth = HeapDepth(h);

        const uint32_t base = slot * kVertexWordsPerSlot;
        auto corner = [&](uint32_t k) {
            float x, y, z;
            std::memcpy(&x, &verts[base + k * 4u + 0u], 4);
            std::memcpy(&y, &verts[base + k * 4u + 1u], 4);
            std::memcpy(&z, &verts[base + k * 4u + 2u], 4);
            return std::array<double, 3>{x, y, z};
        };
        const std::array<double, 3> c0 = corner(0), c1 = corner(1), c2 = corner(2);

        // Screen-space metric on the split edge (corner0, corner2) — the exact Kernel_Classify path.
        const Proj q0 = ProjectPix(p.ViewProjRel, c0[0], c0[1], c0[2], kScreenW, kScreenH);
        const Proj q2 = ProjectPix(p.ViewProjRel, c2[0], c2[1], c2[2], kScreenW, kScreenH);
        bool wSplit = false, wMerge = false;
        double edgePx = 0.0;
        const bool behind = !q0.InFront || !q2.InFront;
        if (behind)
        {
            wSplit = (depth < cap);
        }
        else
        {
            edgePx = std::hypot(q0.Px - q2.Px, q0.Py - q2.Py);
            wSplit = (depth < cap) && edgePx > kSplitPx;
            wMerge = edgePx < kMergePx;
        }
        if (wMerge)
            ++wantMergeEdge;

        // Category flags (independent of the split decision).
        const bool inFrustum = TriInFrustum(p.ViewProjRel, c0, c1, c2);
        auto occ = [&](const std::array<double, 3>& c) {
            return SphereCornerOccluded({float(c[0]), float(c[1]), float(c[2])},
                                        {p.CameraPos[0], p.CameraPos[1], p.CameraPos[2]},
                                        float(reliefFloor));
        };
        const bool occluded = occ(c0) && occ(c1) && occ(c2); // whole triangle below the horizon

        // Occluded/far-side bisectors NOT merging = the can't-merge population that keeps the far
        // hemisphere fine (they neither cull nor coarsen under the current metric).
        if (occluded && depth > kSphereBaseDepth && !wMerge)
            ++cantMergeOccluded;

        // Area demand (reference metric): project all three corners; a fully-in-front triangle whose
        // projected area exceeds the target wants to split. A behind/foreshortened triangle has ~0 area.
        const Proj q1 = ProjectPix(p.ViewProjRel, c1[0], c1[1], c1[2], kScreenW, kScreenH);
        double areaPx = 0.0;
        if (q0.InFront && q1.InFront && q2.InFront)
            areaPx = TriAreaPx(q0, q1, q2);
        if ((depth < cap) && inFrustum && !occluded && areaPx > kAreaOurs)
            ++wantSplitAreaOurs;
        if ((depth < cap) && inFrustum && !occluded && areaPx > kAreaRef)
            ++wantSplitAreaRef;

        if (!wSplit)
            continue;
        ++wantSplitEdge;
        if (depth < demandByDepth.size())
            ++demandByDepth[depth];
        // Disjoint attribution of the EDGE-metric demand.
        if (behind)
            ++behindNear;
        else if (occluded)
            ++occludedFar;
        else if (!inFrustum)
            ++offFrustum;
        else if (areaPx >= kAreaOurs)
            ++visibleReal;
        else
            ++visibleGrazing;
    }

    const double screenBudgetOurs = kScreenW * kScreenH / kAreaOurs; // ~90 k at 16 px^2
    const double screenBudgetRef = kScreenW * kScreenH / kAreaRef;   // ~29 k at 49 px^2

    std::printf("\n===== [demand] split-demand attribution @ 150 m R=2000 (bias OFF) =====\n");
    std::printf("[demand] live=%llu  overflow(cumulative)=%d  overflow(per-frame)=%d\n",
                (unsigned long long)live, overflowAfter, perFrameOverflow);
    std::printf("[demand] EDGE-metric wantSplit=%llu  wantMerge=%llu\n",
                (unsigned long long)wantSplitEdge, (unsigned long long)wantMergeEdge);
    std::printf("[demand]   behind-near-plane (force-split) = %llu\n", (unsigned long long)behindNear);
    std::printf("[demand]   occluded / far-side (below horizon) = %llu\n", (unsigned long long)occludedFar);
    std::printf("[demand]   off-frustum (front, out of view)    = %llu\n", (unsigned long long)offFrustum);
    std::printf("[demand]   visible + real area (>=%.0f px^2)    = %llu\n", kAreaOurs,
                (unsigned long long)visibleReal);
    std::printf("[demand]   visible + grazing (<%.0f px^2)       = %llu\n", kAreaOurs,
                (unsigned long long)visibleGrazing);
    std::printf("[demand] AREA-metric wantSplit: ours(>%.0f)=%llu  ref(>%.0f)=%llu\n", kAreaOurs,
                (unsigned long long)wantSplitAreaOurs, kAreaRef, (unsigned long long)wantSplitAreaRef);
    std::printf("[demand] screen budget: ours(16px^2)=%.0f  ref(49px^2)=%.0f tris\n", screenBudgetOurs,
                screenBudgetRef);
    std::printf("[demand] can't-merge occluded/far-side = %llu\n", (unsigned long long)cantMergeOccluded);
    std::printf("[demand] wantSplit by depth:");
    for (uint32_t d = 0; d < demandByDepth.size(); ++d)
        if (demandByDepth[d] > 0)
            std::printf(" d%u=%llu", d, (unsigned long long)demandByDepth[d]);
    std::printf("\n=======================================================================\n\n");

    // CURED-state guard (fails-before / passes-after the demand fix). The load-bearing, metric-
    // agnostic invariant read straight from GPU state: the live pool is no longer DOMINATED by
    // occluded far-side geometry. Pre-fix ~86% of the 131072-slot pool was occluded/far-side
    // bisectors that neither drew nor merged (they starved the visible near field); the Classify
    // visibility gate coarsens them to base, so the pool now holds visible near-field detail.
    EXPECT_LT(cantMergeOccluded, live / 10u)
        << "the live pool is still full of occluded/far-side bisectors (visibility gate ineffective): "
        << cantMergeOccluded << " of " << live << " live";
    // The reference screen-space AREA metric bounds the genuinely-visible demand to ~the screen
    // budget (the demand cure): far below the pool, so a converged frame re-requests ~0.
    EXPECT_LT(static_cast<double>(wantSplitAreaRef), screenBudgetRef * 3.0)
        << "area-metric visible demand is not screen-bounded — the demand cure does not hold";
}
