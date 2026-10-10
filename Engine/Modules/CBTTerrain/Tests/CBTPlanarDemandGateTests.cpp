// Planar demand/occupancy-gate parity (clipmap/bisector-pool arc, round-10). The off-frustum demand
// gate the sphere carries (CBT_SplitGate + the CBT_GATE_FRUSTUM branch in Kernel_Classify) is gated
// `pc.domainMode == CBT_DOMAIN_SPHERICAL`, so the PLANAR domain reaches Classify's screen-space
// branch with no visibility shaping at all. PR #824's live run (tex_planar_notex: size 1024,
// samplesPerMeter 2 -> maxDepth 23 auto, TargetPixelError 8, pool 1048576) measured the consequence:
// live == pool (100% occupancy), freeCount 0, splitDemand 154460/frame against splitServed 0,
// overflowTotal 16.3 M and climbing, merge stalled — a split treadmill that never converges.
//
// Every pre-existing planar screen-space oracle drives an ORTHOGRAPHIC top-down camera
// (MakeOrthoTopDown: clip.w == 1 identically), so no planar test had ever exercised the perspective
// paths where CBT_ProjectPixels returns false (clip.w <= 1e-5 — at/behind the eye plane). Those are
// exactly the paths the live pose lives in. These tests drive a PERSPECTIVE planar pose.
//
//   PlanarSaturationDemandCensus — the measurement. Converges the real GPU CBT at the live scene's
//     pose and at a pose the pool can comfortably serve, prints the occupancy/demand/served/overflow
//     curve, then reads the live pool back and attributes both the live population and the split
//     demand to disjoint causes on the CPU (bit-faithful to Kernel_Classify: the same projected
//     split-edge length against the same near-bias-coarsened threshold, the same behind-eye/straddle
//     force-split, the same margin-frustum extraction). Exists to print numbers; its assertions lock
//     only the load-bearing shape.
//   SaturatedPlanarPoseReachesHonestSteadyState — the convergence oracle (fails-before): a converged
//     planar view must reach a state where the pool has headroom and the demand it raises is SERVED
//     rather than rolled back forever.
//   ComfortablePlanarTerrainTessellatesUnchanged — the no-regression oracle: a planar terrain whose
//     demand the pool CAN serve must tessellate bit-identically with the gate configured. Arm B
//     (keepCeil 0) is the shipped pre-change planar path on the SAME build, so this is a true
//     before/after comparison rather than a cross-build one.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

#include "CBTTerrain/CBTDemandTuning.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTNearBias.h"
#include "CBTTerrain/CBTPoolHealth.h" // kCBTSaturatedOccupancy — the controller's full mark
#include "CBTReliefHeightSource.h"
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24 — S2a 96B layout
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;

// tex_planar_notex (the PR #824 live pose): size 1024 m, samplesPerMeter 2 -> the auto MaxDepth is
// baseDepth 1 + round(2*log2(1024*2)) = 1 + 22 = 23, a 0.5 m facet floor. TargetPixelError 8.
constexpr float kLiveSizeM = 1024.0f;
constexpr uint32_t kLiveMaxDepth = 23u;
constexpr float kLiveTpe = 8.0f;
// Eye height for the walking pose (m above the flat terrain base).
constexpr float kAltM = 2.0f;

// A planar terrain the pool can comfortably serve — the no-regression arm. 128 m of terrain, with
// the cap two levels ABOVE the 0.5 m facet floor the near-field target bounds the behind-eye
// force-split at, so the converged tree is DEMAND-limited rather than cap-pinned: the oracle then
// sees split-side neutrality as well as merge-side neutrality.
constexpr float kComfortSizeM = 128.0f;
constexpr uint32_t kComfortMaxDepth = 19u;

// Mirrors of the in-shader constants the CPU attribution reproduces (cbt_kernels.comp /
// cbt_layout.glsl). Local to the probe: the assertions lock the SHAPE of the demand, not these
// values (CBTLayoutTests owns the GLSL/C++ constant locks).
constexpr float kSplitNdcMargin = 1.1f;
constexpr float kOffFrustumBehindNdc = 8.0f;
constexpr float kOffFrustumKeepNdc = 10.0f;
constexpr float kKeepRamp = 0.30f;
constexpr float kNearFieldOccCeil = 0.90f;
constexpr float kBehindEyeMinDistM = 1.0f;
constexpr float kNearBiasContentionLo = 0.50f;
constexpr float kNearBiasContentionHi = 0.90f;

constexpr uint32_t kMaxTier = 48u;

// The sibling census runs a relief arm at the shipped ComposedIsland heightScale, because the
// structural facts it locks are exact only when heightScale is 0.
constexpr float kCensusReliefHeightScale = 60.0f;
constexpr uint32_t kCensusReliefDim = 512u;

// CPU mirror of CBT_PlanarKeepBand (cbt_kernels.comp): the planar off-frustum keep band at a keep
// step, from the CBTDemandTuning.h schedule the layout test locks to the kernel.
float PlanarKeepBand(int32_t step)
{
    if (step <= 0)
        return std::numeric_limits<float>::max();
    if (step >= static_cast<int32_t>(kOffFrustumKeepSteps))
        return 0.0f;
    return kOffFrustumKeepWidestNdc *
           std::exp2(-static_cast<float>(step - 1) / static_cast<float>(kOffFrustumKeepStepsPerOctave));
}

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

float SmoothStep(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// CPU mirror of CBT_ProjectPixels (cbt_layout.glsl): column-major clip = M * (x,y,z,1); InFront is
// false when the point is at/behind the near plane (clip.w <= 1e-5).
struct Proj
{
    float Px = 0.0f, Py = 0.0f;
    bool InFront = false;
};
Proj ProjectPix(const float* m, const std::array<float, 3>& p)
{
    Proj r;
    const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    if (w <= 1e-5f)
        return r;
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    r.Px = (cx / w * 0.5f + 0.5f) * kScreenW;
    r.Py = (cy / w * 0.5f + 0.5f) * kScreenH;
    r.InFront = true;
    return r;
}

// CPU mirror of the margin-frustum half of CBT_SplitGate (cbt_kernels.comp) with the spherical
// horizon test omitted (a planar heightfield has no horizon). True when the whole triangle sits
// outside the margin-widened NDC box (or fully behind the eye); reports how far outside.
bool OffMarginFrustum(const Proj& p0, const Proj& p1, const Proj& p2, float& ndcExcess)
{
    ndcExcess = 0.0f;
    if (!p0.InFront && !p1.InFront && !p2.InFront)
    {
        ndcExcess = kOffFrustumBehindNdc;
        return true;
    }
    if (!p0.InFront || !p1.InFront || !p2.InFront)
        return false; // straddles the eye plane — genuinely close
    const float M = kSplitNdcMargin;
    auto ndc = [](const Proj& p) {
        return std::array<float, 2>{p.Px / kScreenW * 2.0f - 1.0f, p.Py / kScreenH * 2.0f - 1.0f};
    };
    const auto n0 = ndc(p0), n1 = ndc(p1), n2 = ndc(p2);
    const float exL = std::min(std::min(-M - n0[0], -M - n1[0]), -M - n2[0]);
    const float exR = std::min(std::min(n0[0] - M, n1[0] - M), n2[0] - M);
    const float exB = std::min(std::min(-M - n0[1], -M - n1[1]), -M - n2[1]);
    const float exT = std::min(std::min(n0[1] - M, n1[1] - M), n2[1] - M);
    const float ex = std::max(std::max(exL, exR), std::max(exB, exT));
    if (ex > 0.0f)
    {
        ndcExcess = ex;
        return true;
    }
    return false;
}

// sqrt(2) — CBT_LEB_PARENT_EDGE_SCALE in cbt_kernels.comp. One bisection of the split edge: a
// parent's hypotenuse is exactly this times its children's.
constexpr float kLebParentEdgeScale = 1.41421356f;

// CPU mirror of CBT_NearBiasCoarsen (cbt_kernels.comp).
float NearBiasCoarsen(const CBTFrameParams& p, float contention, const std::array<float, 3>& point)
{
    if (contention <= 0.0f)
        return 1.0f;
    const float dx = point[0] - p.CameraPos[0], dy = point[1] - p.CameraPos[1],
                dz = point[2] - p.CameraPos[2];
    const float farT = SmoothStep(p.NearBias[1], p.NearBias[2], std::sqrt(dx * dx + dy * dy + dz * dz));
    return 1.0f + contention * farT * std::max(p.NearBias[3] - 1.0f, 0.0f);
}

// CPU mirror of CBT_ParentSplitEdgeFar (cbt_kernels.comp): the parent hypotenuse endpoint this
// child does NOT hold, recovered by reflecting the one it does through the shared midpoint c1.
std::array<float, 3> ParentSplitEdgeFar(uint64_t heapId, const std::array<float, 3>& c0,
                                        const std::array<float, 3>& c1,
                                        const std::array<float, 3>& c2)
{
    const std::array<float, 3>& held = (heapId & 1ull) == 0ull ? c0 : c2;
    return {2.0f * c1[0] - held[0], 2.0f * c1[1] - held[1], 2.0f * c1[2] - held[2]};
}

// CPU mirror of Kernel_Classify's planar screen-space branch (cbt_kernels.comp), reporting the
// split/merge decision together with the normalized metric that drives it, r = edgePx / mergePx.
// r > splitPx/mergePx splits and r below leaves the facet UNCHANGED — but r does NOT decide the
// merge: that is measured on the PARENT's split edge (see WantMerge below), so r is reported for
// the split side and for characterizing how the two rules differ.  Both thresholds carry the SAME
// near-bias coarsening factor, so r is comparable across facets at different camera distances.
//
// Faithful while the planar off-frustum gate is dormant (keep step 0 leaves `poolUnderPressure`
// false, so the gate cannot override the metric) and in the planar domain, where the spherical
// priority floor and crease term are both excluded by domainMode.
struct PlanarDecision
{
    Proj P0, P1, P2;
    bool BehindAll = false;
    bool EyePlane = false; // the shader's (!v0In || !v2In) path — it decides SPLIT only
    bool WantSplit = false;
    bool WantMerge = false;
    float Ratio = 0.0f;       // edgePx / mergePx; meaningless on the EyePlane path
    float ParentRatio = 0.0f; // parentEdgePx / mergePx at the parent midpoint; < sqrt(2) merges
    bool ParentRated = false; // false => the parent's far endpoint is behind the eye plane
};

PlanarDecision ClassifyPlanarFacet(const CBTFrameParams& p, float contention, float occ,
                                   uint32_t depth, uint32_t maxDepth, uint32_t baseDepth,
                                   uint64_t heapId, const std::array<float, 3>& c0,
                                   const std::array<float, 3>& c1, const std::array<float, 3>& c2)
{
    PlanarDecision d;
    d.P0 = ProjectPix(p.ViewProjRel, c0);
    d.P1 = ProjectPix(p.ViewProjRel, c1);
    d.P2 = ProjectPix(p.ViewProjRel, c2);
    d.BehindAll = !d.P0.InFront && !d.P1.InFront && !d.P2.InFront;
    d.EyePlane = !d.P0.InFront || !d.P2.InFront;

    if (d.EyePlane)
    {
        const float dx = c0[0] - c2[0], dy = c0[1] - c2[1], dz = c0[2] - c2[2];
        const float edgeM = std::sqrt(dx * dx + dy * dy + dz * dz);
        bool wantsByTarget = edgeM > kNearFieldFacetTargetM;
        if (d.BehindAll && p.DemandTuning[0] > 0.0f)
        {
            // Behind the eye: the as-if-visible bound — the world edge at the perspective's pixels
            // per metre at the facet's distance, against the coarsened split threshold.
            const std::array<float, 3> mid = {0.5f * (c0[0] + c2[0]), 0.5f * (c0[1] + c2[1]),
                                              0.5f * (c0[2] + c2[2])};
            const float coarsen = NearBiasCoarsen(p, contention, mid);
            const float focalPx = std::sqrt(p.ViewProjRel[1] * p.ViewProjRel[1] +
                                            p.ViewProjRel[5] * p.ViewProjRel[5] +
                                            p.ViewProjRel[9] * p.ViewProjRel[9]) *
                                  0.5f * p.Screen[1];
            const float dist = std::max(std::hypot(mid[0] - p.CameraPos[0], mid[1] - p.CameraPos[1],
                                                   mid[2] - p.CameraPos[2]),
                                        kBehindEyeMinDistM);
            wantsByTarget = wantsByTarget && edgeM * focalPx / dist > p.Screen[2] * coarsen;
        }
        d.WantSplit = depth < maxDepth && occ < kNearFieldOccCeil && wantsByTarget;
        return d; // WantMerge stays false: the branch never decides the merge direction
    }

    const std::array<float, 3> mid = {0.5f * (c0[0] + c2[0]), 0.5f * (c0[1] + c2[1]),
                                      0.5f * (c0[2] + c2[2])};
    const float coarsen = NearBiasCoarsen(p, contention, mid);
    const float splitPx = p.Screen[2] * coarsen;
    const float mergePx = p.Screen[3] * coarsen;

    const float edgePx = std::hypot(d.P0.Px - d.P2.Px, d.P0.Py - d.P2.Py);
    d.Ratio = mergePx > 0.0f ? edgePx / mergePx : 0.0f;
    d.WantSplit = depth < maxDepth && edgePx > splitPx;

    // The merge is decided on the PARENT's split edge — the one edge both siblings share — with the
    // threshold taken at the parent's midpoint (c1, held bit-identically by both children) and
    // scaled by sqrt(2) to restate the child threshold on a parent-length edge. An unprojectable
    // parent endpoint falls back to the per-child answer, exactly as the shader does.
    const std::array<float, 3> parentFar = ParentSplitEdgeFar(heapId, c0, c1, c2);
    const Proj parentFarProj = ProjectPix(p.ViewProjRel, parentFar);
    const Proj& parentHeld = (heapId & 1ull) == 0ull ? d.P0 : d.P2;
    if (parentFarProj.InFront)
    {
        const float mergePxParent = p.Screen[3] * NearBiasCoarsen(p, contention, c1);
        const float parentPx =
            std::hypot(parentHeld.Px - parentFarProj.Px, parentHeld.Py - parentFarProj.Py);
        d.ParentRated = true;
        d.ParentRatio = mergePxParent > 0.0f ? parentPx / mergePxParent : 0.0f;
        d.WantMerge = parentPx < mergePxParent * kLebParentEdgeScale && depth > baseDepth;
    }
    else
    {
        d.WantMerge = edgePx < mergePx && depth > baseDepth;
    }
    return d;
}

// Histogram buckets for r = edgePx / mergePx. Resolution is concentrated just above 1.0: the
// hypothesis under test is that a sibling blocking a merge sits just PAST the hard threshold.
constexpr uint32_t kRatioBuckets = 10u;
constexpr float kRatioEdges[kRatioBuckets - 1u] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f,
                                                   1.5f,  2.0f, 3.0f,  5.0f};
const char* const kRatioLabels[kRatioBuckets] = {"  <0.25", "0.25-0.5", "0.5-0.75", "0.75-1.0",
                                                 "1.0-1.25", "1.25-1.5", "1.5-2.0", "2.0-3.0",
                                                 "3.0-5.0", "   >=5.0"};
uint32_t RatioBucket(float r)
{
    for (uint32_t i = 0; i < kRatioBuckets - 1u; ++i)
        if (r < kRatioEdges[i])
            return i;
    return kRatioBuckets - 1u;
}

struct Census
{
    CBTTessellationStats Final{};
    double FinalOcc = 0.0;
    double PeakOcc = 0.0;
    int32_t OverflowDeltaTail = 0; // overflowTotal growth over the final tail window
    uint32_t TailFrames = 0;
    // Live-count spread across the tail window. Zero spread == an exact fixed point: the topology
    // stopped changing, which is what separates an honest steady state from a churning treadmill.
    uint32_t TailLiveMin = 0xFFFFFFFFu, TailLiveMax = 0;
    // Peak merge groups PrepareSimplify served in any frame — proof the far-field drain really ran
    // (as opposed to the pool merely never filling).
    int32_t MergeServedPeak = 0;
    // Tail frames where Classify raised merge demand and PrepareSimplify served NONE of it. Printed,
    // not asserted: at a FIXED POINT this is a stable no-op (Classify re-flags the same LEB-illegal
    // diamonds every frame, zero work, zero state change), not a stall costing the pool anything.
    // The comfortable pose below shows the same residual at 32% occupancy with zero overflow, so it
    // is a pre-existing diamond-legality property of the merge path, not a saturation symptom.
    uint32_t TailMergeStallFrames = 0;
    uint64_t Live = 0, AtCap = 0;
    uint64_t LiveBehind = 0, LiveStraddle = 0, LiveFrontOff = 0, LiveFrontIn = 0;
    uint64_t WantBehind = 0, WantStraddle = 0, WantFrontOff = 0, WantFrontIn = 0;
    uint64_t ExcessOverKeepNdc = 0; // off-frustum facets whose ndcExcess exceeds the full keep band
    std::array<uint64_t, kMaxTier> LiveByTier{};
};
} // namespace

class CBTPlanarDemandGate : public ::testing::Test
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

    void RunFrame(CBTInstance& inst, const CBTClassifyDesc& desc, const CBTFrameParams& params,
                  uint32_t frame)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        inst.RecordUpdate(*cl, desc, params, frame);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    uint32_t ValidationErrors(CBTInstance& inst) { return inst.ReadValidationErrorCount(); }

    std::vector<uint64_t> ReadHeap(CBTInstance& inst)
    {
        const auto w = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        std::vector<uint64_t> h(kDefaultBisectorPoolSize);
        for (uint32_t i = 0; i < kDefaultBisectorPoolSize; ++i)
            h[i] = static_cast<uint64_t>(w[i * 2u]) | (static_cast<uint64_t>(w[i * 2u + 1u]) << 32);
        return h;
    }

    // The tex_planar_notex walking pose: a PERSPECTIVE camera at eye height over the middle of a
    // flat `sizeM` planar terrain anchored at world origin, looking horizontally down +Z. Terrain
    // geometry therefore lies in front of, beside, and BEHIND the camera — the regime every
    // pre-existing (orthographic) planar oracle cannot reach.
    void BuildPlanarWalkParams(CBTFrameParams& p, float sizeM, float altM, float splitPx,
                               float mergePx, uint32_t maxDepth, float heightScale = 0.0f)
    {
        using namespace GameEngine::Mathematics;
        p = CBTFrameParams{};
        p.Screen[0] = kScreenW;
        p.Screen[1] = kScreenH;
        p.Screen[2] = splitPx;
        p.Screen[3] = mergePx;
        p.TerrainSize[0] = sizeM;
        p.TerrainSize[1] = sizeM;
        // heightScale (CBTLayout.h). The demand-shaping probes run flat because their mechanism
        // is projected geometry; the sibling census runs BOTH, because sibling geometry is exact
        // only when every corner of a facet shares one y.
        p.TerrainSize[2] = heightScale;
        p.TerrainSize[3] = 0.0f; // originY
        p.TerrainOrigin[0] = 0.0f;
        p.TerrainOrigin[1] = 0.0f;
        p.TerrainOrigin[2] = static_cast<float>(maxDepth); // Classify's depth cap
        p.TerrainOrigin[3] = 0.0f;

        const Vector3 eye(sizeM * 0.5f, altM, sizeM * 0.5f);
        const Matrix4x4 proj =
            MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + Vector3(0, 0, 1), Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;

        const CBTNearBiasRadii nb = ComputeNearBiasRadii(altM);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = kNearFieldFacetTargetM;
        p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.DemandTuning[2] = kEdgeRescueTpeMul * splitPx;
        p.DemandTuning[3] = kEdgeRescueOcc;
        p.PriorityParams[0] = 1.0f;
        p.PriorityParams[1] = kPriorityRampStartOcc;
        p.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
        p.PriorityParams[3] = kPriorityKeepLargeNdc;
    }

    // Converge `frames` updates at `p`, then census the live pool. `tailFrames` sets the window the
    // overflow-growth reading covers (the treadmill signal). `label` non-null => print the curve.
    Census ConvergeAndCensus(CBTInstance& inst, CBTClassifyDesc& c, const CBTFrameParams& p,
                             uint32_t maxDepth, uint32_t frames, uint32_t tailFrames,
                             const char* label)
    {
        Census r;
        if (label)
            std::printf("\n[%s] frame     live      occ  splitDemand  splitServed  mergeDemand  "
                        "mergeServed  overflowTot\n",
                        label);
        int32_t overflowAtTailStart = 0;
        for (uint32_t f = 0; f < frames; ++f)
        {
            c.GateVertexEval = (f == 0u) ? 0u : 1u;
            RunFrame(inst, c, p, f);
            const bool inTail = f + tailFrames >= frames;
            const bool show = label && (f < 8u || f % 20u == 0u || f == frames - 1u);
            if (inTail || show)
            {
                const CBTTessellationStats s = inst.ReadTessellationStats();
                const double occ = static_cast<double>(s.LiveCount) / s.PoolSize;
                r.PeakOcc = std::max(r.PeakOcc, occ);
                r.MergeServedPeak = std::max(r.MergeServedPeak, s.MergeServed);
                if (inTail)
                {
                    ++r.TailFrames;
                    r.TailLiveMin = std::min(r.TailLiveMin, s.LiveCount);
                    r.TailLiveMax = std::max(r.TailLiveMax, s.LiveCount);
                    if (s.MergeDemand > 0 && s.MergeServed == 0)
                        ++r.TailMergeStallFrames;
                }
                if (f + tailFrames == frames)
                    overflowAtTailStart = s.OverflowTotal;
                if (show)
                    std::printf("[%s] %5u %8u %8.4f %12d %12d %12d %12d %12d\n", label, f,
                                s.LiveCount, occ, s.SplitDemand, s.SplitServed, s.MergeDemand,
                                s.MergeServed, s.OverflowTotal);
            }
        }

        r.Final = inst.ReadTessellationStats();
        r.FinalOcc = static_cast<double>(r.Final.LiveCount) / r.Final.PoolSize;
        r.PeakOcc = std::max(r.PeakOcc, r.FinalOcc);
        r.OverflowDeltaTail = r.Final.OverflowTotal - overflowAtTailStart;

        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<uint32_t> verts = inst.DebugReadWords(
            CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
        auto corner = [&](uint32_t slot, uint32_t k) {
            const uint32_t base = slot * kVertexWordsPerSlot + k * 4u;
            return std::array<float, 3>{asFloat(verts[base + 0u]), asFloat(verts[base + 1u]),
                                        asFloat(verts[base + 2u])};
        };

        const float occ = static_cast<float>(r.FinalOcc);
        const float contention = SmoothStep(kNearBiasContentionLo, kNearBiasContentionHi, occ);

        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = heap[s];
            if (h == 0u)
                continue;
            ++r.Live;
            const uint32_t depth = HeapDepth(h);
            if (depth < kMaxTier)
                ++r.LiveByTier[depth];
            const auto c0 = corner(s, 0), c1 = corner(s, 1), c2 = corner(s, 2);
            const PlanarDecision d = ClassifyPlanarFacet(p, contention, occ, depth, maxDepth,
                                                         inst.GetBaseDepth(), h, c0, c1, c2);
            const Proj p0 = d.P0, p1 = d.P1, p2 = d.P2;
            const bool behindAll = d.BehindAll;
            const bool eyePlanePath = d.EyePlane; // the shader's (!v0In || !v2In)
            float ndcExcess = 0.0f;
            const bool offFrustum = OffMarginFrustum(p0, p1, p2, ndcExcess);
            if (offFrustum && ndcExcess > kOffFrustumKeepNdc)
                ++r.ExcessOverKeepNdc;

            if (behindAll)
                ++r.LiveBehind;
            else if (eyePlanePath)
                ++r.LiveStraddle;
            else if (offFrustum)
                ++r.LiveFrontOff;
            else
                ++r.LiveFrontIn;

            if (depth >= maxDepth)
            {
                ++r.AtCap;
                continue; // the metric cannot ask for more
            }
            if (!d.WantSplit)
                continue;
            if (behindAll)
                ++r.WantBehind;
            else if (eyePlanePath)
                ++r.WantStraddle;
            else if (offFrustum)
                ++r.WantFrontOff;
            else
                ++r.WantFrontIn;
        }
        return r;
    }

    static void PrintCensus(const char* label, const Census& r)
    {
        const uint64_t wantTotal = r.WantBehind + r.WantStraddle + r.WantFrontOff + r.WantFrontIn;
        const uint64_t removable = r.WantBehind + r.WantFrontOff;
        std::printf("[%s] CONVERGED: live=%u/%u occ=%.4f free=%d splitDemand=%d splitServed=%d "
                    "mergeDemand=%d mergeServed=%d overflowTotal=%d overflowGrowthTail=%d "
                    "mergeStallTail=%u/%u mergeServedPeak=%d tailLiveSpread=%u\n",
                    label, r.Final.LiveCount, r.Final.PoolSize, r.FinalOcc, r.Final.FreeCount,
                    r.Final.SplitDemand, r.Final.SplitServed, r.Final.MergeDemand,
                    r.Final.MergeServed, r.Final.OverflowTotal, r.OverflowDeltaTail,
                    r.TailMergeStallFrames, r.TailFrames, r.MergeServedPeak,
                    r.TailLiveMax - r.TailLiveMin);
        std::printf("[%s] LIVE census: total=%llu atCap=%llu | behindEye=%llu straddle=%llu "
                    "frontOffFrustum=%llu frontInFrustum=%llu | ndcExcess>keepBand=%llu\n",
                    label, static_cast<unsigned long long>(r.Live),
                    static_cast<unsigned long long>(r.AtCap),
                    static_cast<unsigned long long>(r.LiveBehind),
                    static_cast<unsigned long long>(r.LiveStraddle),
                    static_cast<unsigned long long>(r.LiveFrontOff),
                    static_cast<unsigned long long>(r.LiveFrontIn),
                    static_cast<unsigned long long>(r.ExcessOverKeepNdc));
        std::printf("[%s] SPLIT-DEMAND attribution: total=%llu | behindEye=%llu straddle=%llu "
                    "frontOffFrustum=%llu frontInFrustum=%llu -> off-frustum share %.1f%%\n",
                    label, static_cast<unsigned long long>(wantTotal),
                    static_cast<unsigned long long>(r.WantBehind),
                    static_cast<unsigned long long>(r.WantStraddle),
                    static_cast<unsigned long long>(r.WantFrontOff),
                    static_cast<unsigned long long>(r.WantFrontIn),
                    wantTotal ? 100.0 * static_cast<double>(removable) / wantTotal : 0.0);
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// ============================================================================
// The measurement: the live saturating pose and a pose the pool can comfortably serve.
// ============================================================================
TEST_F(CBTPlanarDemandGate, PlanarSaturationDemandCensus)
{
    struct Pose
    {
        const char* Label;
        float SizeM;
        uint32_t MaxDepth;
        uint32_t Frames;
    };
    const Pose poses[] = {{"planar-live", kLiveSizeM, kLiveMaxDepth, 220u},
                          {"planar-comfort", kComfortSizeM, kComfortMaxDepth, 120u}};

    for (const Pose& pose : poses)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = pose.MaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, pose.SizeM, kAltM, kLiveTpe, kLiveTpe * 0.5f, pose.MaxDepth);

        const Census r = ConvergeAndCensus(inst, c, p, pose.MaxDepth, pose.Frames, 20u, pose.Label);
        PrintCensus(pose.Label, r);
        std::printf("[%s] live-by-depth:", pose.Label);
        for (uint32_t d = 0; d < kMaxTier; ++d)
            if (r.LiveByTier[d])
                std::printf(" d%u=%llu", d, static_cast<unsigned long long>(r.LiveByTier[d]));
        std::printf("\n");
        EXPECT_EQ(ValidationErrors(inst), 0u) << pose.Label << ": tree must stay conforming";
        EXPECT_GT(r.Live, 0u) << pose.Label;
        inst.Shutdown();
    }
    std::printf("\n");
}

// ============================================================================
// The convergence oracle (fails-before), with a POSITIVE CONTROL. A converged planar view must reach
// an honest steady state: the pool keeps headroom under the ceiling the off-frustum gate names, the
// topology stops changing, and no split is rolled back for want of pool.
//
// The control arm zeroes DemandTuning.x and .y (the documented "every field 0 => the pre-slice
// metric" contract): no near-field target, so the straddling and behind-eye facets force-split to
// the cap, and no off-frustum keep gate — the planar path as shipped before the walking-headroom
// slice, on the SAME build. It must SATURATE. Without that arm the treatment assertions could pass
// on a pose that simply never filled the pool, and the test would prove nothing. (With the near-field
// target alone — behind-eye facets bounded like the field in view — this pose no longer saturates:
// 0.95 occupancy, no rollbacks in the tail. The flood the keep gate was measured against was the
// behind-eye force-split.)
// ============================================================================
TEST_F(CBTPlanarDemandGate, SaturatedPlanarPoseReachesHonestSteadyState)
{
    constexpr uint32_t kFrames = 260u;
    constexpr uint32_t kTail = 20u;

    auto run = [&](bool gateConfigured, const char* label) {
        CBTInstance inst;
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainPlanar))
        {
            ADD_FAILURE() << label << ": CBTInstance init failed";
            return std::pair<Census, uint32_t>{Census{}, 1u};
        }
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kLiveMaxDepth;
        c.NearFieldGate = 1u;
        // Both arms hold the pool-pressure scale at 1x: this oracle characterises the walking-headroom
        // demand gate, and its control arm must be allowed to pin (the scale would un-pin it).
        c.PoolPressure = 0u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, kLiveSizeM, kAltM, kLiveTpe, kLiveTpe * 0.5f, kLiveMaxDepth);
        if (!gateConfigured)
        {
            p.DemandTuning[0] = 0.0f; // no near-field target => force-split to the cap (pre-slice)
            p.DemandTuning[1] = 0.0f; // keepCeil 0 => off-frustum gate inert (pre-slice)
        }
        const Census r = ConvergeAndCensus(inst, c, p, kLiveMaxDepth, kFrames, kTail, label);
        PrintCensus(label, r);
        const uint32_t errors = ValidationErrors(inst);
        inst.Shutdown();
        return std::pair<Census, uint32_t>{r, errors};
    };

    const auto [ungated, ungatedErrors] = run(false, "steady-ungated");
    const auto [gated, gatedErrors] = run(true, "steady-gated");

    ASSERT_EQ(ungated.TailFrames, kTail) << "tail window not sampled — the readings are not readings";
    ASSERT_EQ(gated.TailFrames, kTail) << "tail window not sampled — the readings are not readings";

    // ---- POSITIVE CONTROL: the pose really is an unsatisfiable one without the gate ----
    EXPECT_GT(ungated.FinalOcc, 0.99)
        << "control arm did not saturate — this pose cannot discriminate the gate, so the treatment "
           "assertions below prove nothing";
    EXPECT_GT(ungated.OverflowDeltaTail, 0)
        << "control arm shows no rolled-back split demand — there is no treadmill to cure";

    // ---- TREATMENT ----
    // 1. Occupancy lands under the headroom target the off-frustum keep ceiling names, with real
    //    free slots for a camera move to claim, instead of pinning at 1.0.
    EXPECT_LT(gated.FinalOcc, static_cast<double>(kOffFrustumKeepOcc))
        << "planar occupancy still pins at/above the keep ceiling — the pool has no headroom";
    EXPECT_GT(gated.Final.FreeCount, 0);

    // 2. The treadmill is gone: over the final tail window not one split is rolled back for want of
    //    pool. The strongest form of "demand decays to what the pool can serve".
    EXPECT_EQ(gated.OverflowDeltaTail, 0)
        << "overflowTotal still climbing over the last " << kTail
        << " frames — unsatisfiable split demand persists";

    // 3. The far-field drain really ran (this is "mergeStalled clears" in the sense that matters:
    //    the stranded off-frustum field was returned to the pool, not merely never allocated).
    EXPECT_GT(gated.MergeServedPeak, 0) << "merge never served a group — the far field never drained";

    // 4. The end state is a FIXED POINT, not a churning equilibrium: the live count does not move at
    //    all across the tail. This is what separates an honest steady state from a treadmill that
    //    happens to average out, and it is the assertion a limit cycle around the ramp would fail.
    EXPECT_EQ(gated.TailLiveMax, gated.TailLiveMin)
        << "live count still moving across the tail — the converged view is not at a fixed point";

    // 5. Conformity is never traded for headroom, in either arm.
    EXPECT_EQ(gatedErrors, 0u) << "tree must stay conforming through the drain";
    EXPECT_EQ(ungatedErrors, 0u) << "control arm tree must also stay conforming";

    // The visible field is the point of the exercise: freeing the pool from geometry the camera
    // cannot see must leave MORE facets in the frustum, not fewer.
    std::printf("[steady] in-frustum facets: ungated=%llu gated=%llu (%.2fx)\n",
                static_cast<unsigned long long>(ungated.LiveFrontIn),
                static_cast<unsigned long long>(gated.LiveFrontIn),
                ungated.LiveFrontIn ? static_cast<double>(gated.LiveFrontIn) /
                                          static_cast<double>(ungated.LiveFrontIn)
                                    : 0.0);
    EXPECT_GT(gated.LiveFrontIn, ungated.LiveFrontIn)
        << "the visible field did not gain from the reclaimed pool — the gate traded away detail";
}

// ============================================================================
// The no-regression oracle. A planar terrain whose demand the pool CAN serve must tessellate
// bit-identically with the gate configured: same live count, same per-depth histogram, same draw
// stream. Arm B zeroes the off-frustum keep ceiling (DemandTuning.y), which is the documented
// "every field 0 => the pre-slice metric" contract and therefore the shipped pre-change planar
// path on the SAME build — so a divergence here is a real regression, not a build difference.
// ============================================================================
TEST_F(CBTPlanarDemandGate, ComfortablePlanarTerrainTessellatesUnchanged)
{
    auto run = [&](bool gateConfigured, const char* label) {
        CBTInstance inst;
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainPlanar))
        {
            ADD_FAILURE() << label << ": CBTInstance init failed";
            return Census{};
        }
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kComfortMaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, kComfortSizeM, kAltM, kLiveTpe, kLiveTpe * 0.5f, kComfortMaxDepth);
        if (!gateConfigured)
            p.DemandTuning[1] = 0.0f; // keepCeil 0 => the off-frustum gate is inert (pre-change)
        const Census r = ConvergeAndCensus(inst, c, p, kComfortMaxDepth, 120u, 20u, label);
        PrintCensus(label, r);
        EXPECT_EQ(ValidationErrors(inst), 0u) << label;
        inst.Shutdown();
        return r;
    };

    const Census on = run(true, "comfort-gated");
    const Census off = run(false, "comfort-ungated");

    // The sample must be a real one: a converged tree of substance that the pool serves with room to
    // spare, carrying genuine off-frustum population (otherwise "unchanged" is vacuous).
    ASSERT_GT(off.Live, 1000u) << "comfortable pose produced a trivial tree — nothing to compare";
    ASSERT_LT(off.PeakOcc, 0.60) << "comfortable pose reached pool pressure — not the neutral regime";
    ASSERT_GT(off.LiveFrontOff + off.LiveBehind, 100u)
        << "no off-frustum population — the oracle cannot see a gate that shapes off-frustum demand";

    EXPECT_EQ(on.Final.LiveCount, off.Final.LiveCount)
        << "gate changed the converged live count of a terrain the pool can serve";
    for (uint32_t d = 0; d < kMaxTier; ++d)
        EXPECT_EQ(on.LiveByTier[d], off.LiveByTier[d])
            << "gate changed the depth-" << d << " population of a terrain the pool can serve";
    EXPECT_EQ(on.LiveBehind, off.LiveBehind);
    EXPECT_EQ(on.LiveFrontOff, off.LiveFrontOff);
    EXPECT_EQ(on.LiveFrontIn, off.LiveFrontIn);
}

// Terrain entirely behind the camera refines at the density the screen metric would give it in
// front — the ALL stream's shadow casters keep a tessellation — never toward the near-field world
// target: the 0.5 m force-split every behind-eye facet used to inherit from the straddling facet
// at the eye plane is 8.4M facets for this 1024 m plane, the flood that pinned the pool at the keep
// ramp's knee on a heading over open water (#1413). A third arm turns a front-converged tree
// around: the behind-eye MERGE term must coarsen it toward the bounded density without a split.
TEST_F(CBTPlanarDemandGate, TerrainBehindTheEyeRefinesLikeTheFieldInView)
{
    using namespace GameEngine::Mathematics;
    // Four eye heights, so the arms hold over a spread of pinned front densities rather than the
    // one an altitude happens to produce. The turn-around arm does not need any of them to land in
    // the keep gate's behind-eye band — it sets the band itself on its own pinned pose (below) — so
    // the spread buys it four independent samples of the same contract (#1471).
    constexpr float kAltitudesM[] = {40.0f, 50.0f, 60.0f, 80.0f};
    constexpr uint32_t kPhaseFrames = 60u;
    // A facet entirely behind the eye enters the off-frustum keep gate at ndcExcess
    // kOffFrustumBehindNdc, and a planar terrain reaches the gate's pressure arm only at a keep
    // step of at least 1, so the band holds a behind-eye facet at depth for every keep step whose
    // band (PlanarKeepBand) is at least that wide. The turn-around arm engages the step through the
    // integrator itself: its front phase runs with the keep ceiling out of reach (the step stays
    // 0), one update with the ceiling under the pinned occupancy engages it (one step, or the
    // saturated stride when the pool reads full), and the behind phase then holds
    // the pinned occupancy in the middle of the step's hold band, [keepCeil - kKeepRamp,
    // keepCeil), until the drain leaves it.
    constexpr float kKeepCeilOutOfReach = 2.0f;
    constexpr float kKeepCeilUnderPinned = 0.01f;
    constexpr float kKeepCeilAbovePinnedHoldMid = kKeepRamp * 0.5f;
    // The turned tree converges to the density the bounded behind-eye demand reaches from roots, so
    // that count — not the front pose's racy pinned one — is the reference. The ceiling sits
    // between the converged ratio and the frozen one (1.08-1.11 vs 1.59-1.89 observed).
    constexpr double kTurnedOverBoundedCeil = 1.25;
    // Whether the run keeps the shipped keep ceiling or engages the keep step on the pose the first
    // phase pinned at.
    enum class KeepCeilSource
    {
        Shipped,
        PinnedPose,
    };
    // One instance driven through one eye Z per phase, kPhaseFrames each; stats after every phase.
    auto run = [&](float altitudeM, std::initializer_list<float> eyeZs, const char* label,
                   KeepCeilSource keepCeilSource = KeepCeilSource::Shipped) {
        std::vector<CBTTessellationStats> out;
        CBTInstance inst;
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainPlanar))
        {
            ADD_FAILURE() << label << ": CBTInstance init failed";
            out.resize(eyeZs.size());
            return out;
        }
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kLiveMaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, kLiveSizeM, altitudeM, kLiveTpe, kLiveTpe * 0.5f, kLiveMaxDepth);
        if (keepCeilSource == KeepCeilSource::PinnedPose)
            p.DemandTuning[1] = kKeepCeilOutOfReach;
        const Matrix4x4 proj =
            MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
        uint32_t frame = 0;
        for (float eyeZ : eyeZs)
        {
            const Vector3 eye(kLiveSizeM * 0.5f, altitudeM, eyeZ);
            const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + Vector3(0, 0, 1), Vector3(0, 1, 0));
            std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
            p.CameraPos[0] = eye.x;
            p.CameraPos[1] = eye.y;
            p.CameraPos[2] = eye.z;
            for (uint32_t f = 0; f < kPhaseFrames; ++f, ++frame)
            {
                c.GateVertexEval = (frame == 0u) ? 0u : 1u;
                RunFrame(inst, c, p, frame);
            }
            const CBTTessellationStats s = inst.ReadTessellationStats();
            const double occ = static_cast<double>(s.LiveCount) / s.PoolSize;
            std::printf("[behind-eye alt %.0f %s phase %zu] live=%u/%u (occ %.4f) keepCeil=%.4f "
                        "overflow=%d mergeDemand=%d\n",
                        altitudeM, label, out.size(), s.LiveCount, s.PoolSize, occ,
                        p.DemandTuning[1], s.OverflowTotal, s.MergeDemand);
            out.push_back(s);
            if (keepCeilSource == KeepCeilSource::PinnedPose && out.size() == 1u)
            {
                EXPECT_EQ(inst.ReadWorkQueueCounter(kWQOffFrustumKeepStep), 0)
                    << label << ": the keep step engaged with the keep ceiling out of reach";
                p.DemandTuning[1] = kKeepCeilUnderPinned;
                RunFrame(inst, c, p, frame++);
                const int32_t keepStep = inst.ReadWorkQueueCounter(kWQOffFrustumKeepStep);
                EXPECT_GE(keepStep, 1) << label << ": the keep step did not engage on the pinned pose";
                EXPECT_LE(keepStep, static_cast<int32_t>(kOffFrustumKeepSaturatedStride))
                    << label << ": the keep step moved more than one update's worth";
                EXPECT_GE(PlanarKeepBand(keepStep), kOffFrustumBehindNdc)
                    << label << ": the engaged keep step no longer holds a behind-eye facet in the band";
                p.DemandTuning[1] = static_cast<float>(occ) + kKeepCeilAbovePinnedHoldMid;
            }
        }
        EXPECT_EQ(ValidationErrors(inst), 0u) << label;
        inst.Shutdown();
        return out;
    };

    // The plane spans Z in [0, size]: an eye one metre before it looks across the whole plane, an
    // eye one metre past it has every corner behind the eye plane.
    constexpr float kFrontZ = -1.0f;
    constexpr float kBehindZ = kLiveSizeM + 1.0f;
    for (const float altitudeM : kAltitudesM)
    {
        const CBTTessellationStats front = run(altitudeM, {kFrontZ}, "front").back();
        const CBTTessellationStats behind = run(altitudeM, {kBehindZ}, "behind").back();
        EXPECT_GT(behind.LiveCount, behind.RootCount * 1000u)
            << "altitude " << altitudeM
            << ": the plane behind the camera did not refine at all — shadow casters lost their "
               "tessellation";
        // The flood exceeds the pool and pins it; the bounded field never brings the pool under
        // the off-frustum keep policy's pressure, and — the sharp clause — never has a split
        // rolled back.
        EXPECT_LT(static_cast<double>(behind.LiveCount) / behind.PoolSize, kOffFrustumKeepOcc)
            << "altitude " << altitudeM
            << ": the plane behind the camera refined toward the near-field target instead of the "
               "screen's";
        EXPECT_EQ(behind.OverflowTotal, 0)
            << "altitude " << altitudeM << ": the bounded behind-eye demand should never roll back";
        std::printf("[behind-eye alt %.0f] behind/front live ratio %.2f\n", altitudeM,
                    front.LiveCount ? static_cast<double>(behind.LiveCount) / front.LiveCount : 0.0);

        // Turn around: the tree the front pose refined is now wholly behind the eye, with the keep
        // gate's window centred on the density that pose pinned at. The behind-eye merge term
        // coarsens the tree to the bounded density (without the term the window holds it at the
        // front count) and raises no split of its own, so the rollback counter does not move.
        const std::vector<CBTTessellationStats> turned =
            run(altitudeM, {kFrontZ, kBehindZ}, "turned", KeepCeilSource::PinnedPose);
        ASSERT_EQ(turned.size(), 2u);
        std::printf("[behind-eye alt %.0f turned] front live=%u (occ %.4f) -> behind live=%u "
                    "(turned/front %.3f, bounded from roots %u, turned/bounded %.3f) "
                    "mergeDemand=%d\n",
                    altitudeM, turned[0].LiveCount,
                    static_cast<double>(turned[0].LiveCount) / turned[0].PoolSize,
                    turned[1].LiveCount,
                    turned[0].LiveCount
                        ? static_cast<double>(turned[1].LiveCount) / turned[0].LiveCount
                        : 0.0,
                    behind.LiveCount,
                    behind.LiveCount
                        ? static_cast<double>(turned[1].LiveCount) / behind.LiveCount
                        : 0.0,
                    turned[1].MergeDemand);
        EXPECT_LT(static_cast<double>(turned[1].LiveCount),
                  behind.LiveCount * kTurnedOverBoundedCeil)
            << "altitude " << altitudeM
            << ": turning the camera away did not coarsen the field behind it to the bounded "
               "density (front live "
            << turned[0].LiveCount << ", behind live " << turned[1].LiveCount
            << ", bounded from roots " << behind.LiveCount << ", mergeDemand "
            << turned[1].MergeDemand << ")";
        EXPECT_EQ(turned[1].OverflowTotal, turned[0].OverflowTotal)
            << "altitude " << altitudeM << ": a facet behind the eye asked for a split after the turn";
    }
}

// A converged planar tree with pool headroom must leave no split demand it never serves.
//
// Kernel_Split yields to a facing neighbor that is "being modified" on the grounds that the
// neighbor's compatibility-chain walk will drive this subdivision. Only a SPLITTING neighbor does
// that. A neighbor flagged SIMPLIFY does no split work at all, and it can hold that flag for as
// long as its diamond stays incomplete. When this test was written the per-facet merge metric made
// that population enormous — tens of thousands of permanently-unservable flags on a converged tree
// — and every split candidate facing one was starved indefinitely, with the pool 68% empty and zero
// overflow, which is what makes this a state-flag defect rather than a capacity one. The
// parent-level metric has since removed ~86-89% of those flags at the source; this predicate is
// what keeps the remainder from starving splits.
//
// Fails-before: on the shipped predicate this measured splitDemand 24 / splitServed 0 at 707,988
// free slots (comfort) and 3,982 / 0 at 419,378 free (live), with the bail census attributing
// 24/24 and 3,842/3,982 of those to a SIMPLIFY-flagged neighbor.
TEST_F(CBTPlanarDemandGate, ConvergedPlanarTreeServesItsSplitDemand)
{
    struct Arm
    {
        const char* Label;
        float SizeM;
        uint32_t MaxDepth;
    };
    const Arm arms[] = {{"comfort", kComfortSizeM, kComfortMaxDepth},
                        {"live", kLiveSizeM, kLiveMaxDepth}};

    for (const Arm& a : arms)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = a.MaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, a.SizeM, kAltM, kLiveTpe, kLiveTpe * 0.5f, a.MaxDepth);
        const Census r = ConvergeAndCensus(inst, c, p, a.MaxDepth, 120u, 20u, nullptr);

        std::printf("[split:%s] live=%u occ=%.4f free=%d splitDemand=%d splitServed=%d "
                    "overflowGrowthTail=%d\n",
                    a.Label, r.Final.LiveCount, r.FinalOcc, r.Final.FreeCount, r.Final.SplitDemand,
                    r.Final.SplitServed, r.OverflowDeltaTail);

        // The demand must be starved for want of a decision, not for want of slots — otherwise
        // this asserts nothing about the bail predicate.
        ASSERT_LT(r.FinalOcc, 0.90) << a.Label << ": pool under pressure, not the regime under test";
        ASSERT_GT(r.Final.FreeCount, 100000) << a.Label << ": too few free slots to attribute";
        ASSERT_EQ(r.OverflowDeltaTail, 0) << a.Label << ": splits are being rolled back for want of "
                                             "slots — a capacity limit, not a starved decision";

        EXPECT_EQ(r.Final.SplitDemand, 0)
            << a.Label << ": " << r.Final.SplitDemand << " split candidates stand unserved against "
            << r.Final.FreeCount << " free slots — the fixed point is not a fixed point";
        inst.Shutdown();
    }
}

// ============================================================================
// Sibling merge metric — why the merge side of a converged planar tree never serves.
// ============================================================================
// A merge collapses a whole LEB diamond, and Kernel_PrepareSimplify serves one only when EVERY
// member carries CBT_STATE_SIMPLIFY. Two siblings do NOT share a split edge — each child's
// hypotenuse is a different LEG of the parent, and the two legs run in different directions — so
// under a grazing perspective camera they project an ORDER OF MAGNITUDE apart. Deciding the merge
// on a facet's own edge therefore had exactly one sibling asking and the other never doing so, and
// the demand was unservable by construction.
//
// Classify now decides the planar merge on the PARENT's split edge, which both siblings measure
// identically. This census stays because the parent metric RESTS on the three structural facts it
// locks — a decode change that broke any of them would silently invalidate the metric — and because
// the population statistics it prints are how the two rules were compared:
//   1. the corner convention — siblings share c1, and evenC2 == oddC0, so the parent's hypotenuse
//      runs between the two UNSHARED corners with c1 as its midpoint;
//   2. ON FLAT GROUND ONLY, sibling split edges are exactly congruent in world space, so the
//      pixel spread there is purely projection. This is NOT a general LEB fact and a world-space
//      merge criterion is NOT diamond-consistent for free: the relief arm measures the same ratio
//      at 0.249-3.917, which is why CBT_NearFieldWantMerge's world-space bound carries the very
//      asymmetry the domain metric no longer does;
//   3. ON FLAT GROUND ONLY, reconstructing that hypotenuse from either child agrees on the merge
//      decision. With relief the height is extrapolated rather than recovered, and the relief arm
//      measures 97 of 115,610 pairs reaching opposite decisions (0.084%) — the price of the
//      parent-level test, against the ~40% sibling disagreement the per-child rule carried.
// The blocked-population counts are PRINTED, not asserted — they characterize how the rules differ
// and are near-empty now. CBTParentMergeMetric asserts the BEHAVIOR on the GPU's own state buffer.
TEST_F(CBTPlanarDemandGate, SiblingMergeMetricCensus)
{
    struct Arm
    {
        const char* Label;
        float SizeM;
        uint32_t MaxDepth;
        uint32_t Frames;
        float HeightScale; // 0 = flat, the only regime where the facts below are EXACT
    };
    const Arm arms[] = {{"comfort", kComfortSizeM, kComfortMaxDepth, 120u, 0.0f},
                        {"live", kLiveSizeM, kLiveMaxDepth, 220u, 0.0f},
                        {"relief", kComfortSizeM, kComfortMaxDepth, 120u, kCensusReliefHeightScale}};

    for (const Arm& a : arms)
    {
        CBTInstance inst;
        ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
        const ReliefHeightSource relief(*m_Device, inst, a.HeightScale, kCensusReliefDim);
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = a.MaxDepth;
        c.NearFieldGate = 1u;
        CBTFrameParams p;
        BuildPlanarWalkParams(p, a.SizeM, kAltM, kLiveTpe, kLiveTpe * 0.5f, a.MaxDepth,
                              a.HeightScale);
        const Census r = ConvergeAndCensus(inst, c, p, a.MaxDepth, a.Frames, 20u, nullptr);
        ASSERT_EQ(ValidationErrors(inst), 0u)
            << a.Label << ": census must read a conforming tree, or its geometry proves nothing";

        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<uint32_t> verts = inst.DebugReadWords(
            CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
        auto corner = [&](uint32_t slot, uint32_t k) {
            const uint32_t base = slot * kVertexWordsPerSlot + k * 4u;
            return std::array<float, 3>{asFloat(verts[base + 0u]), asFloat(verts[base + 1u]),
                                        asFloat(verts[base + 2u])};
        };

        const float occ = static_cast<float>(r.FinalOcc);
        const float contention = SmoothStep(kNearBiasContentionLo, kNearBiasContentionHi, occ);
        const uint32_t baseDepth = inst.GetBaseDepth();

        // Siblings share a parent heapID and heapIDs are unique, so sorting the live set by heapID
        // puts every sibling pair adjacent.
        std::vector<std::pair<uint64_t, uint32_t>> live;
        live.reserve(r.Final.LiveCount);
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
            if (heap[s] != 0u)
                live.emplace_back(heap[s], s);
        std::sort(live.begin(), live.end());

        // Threshold pair at a world split-edge midpoint, carrying the same near-bias coarsening
        // Kernel_Classify applies (both thresholds scale by the SAME factor).
        auto thresholdsAt = [&](const std::array<float, 3>& mid, float& splitPx, float& mergePx) {
            splitPx = p.Screen[2];
            mergePx = p.Screen[3];
            if (contention <= 0.0f)
                return;
            const float dx = mid[0] - p.CameraPos[0], dy = mid[1] - p.CameraPos[1],
                        dz = mid[2] - p.CameraPos[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float farT = SmoothStep(p.NearBias[1], p.NearBias[2], dist);
            const float coarsen = 1.0f + contention * farT * std::max(p.NearBias[3] - 1.0f, 0.0f);
            splitPx *= coarsen;
            mergePx *= coarsen;
        };
        auto worldLen = [](const std::array<float, 3>& u, const std::array<float, 3>& v) {
            const float dx = u[0] - v[0], dy = u[1] - v[1], dz = u[2] - v[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        // splitPx/mergePx is scale-free (both carry the same coarsening), so r above it is the
        // "would split if the cap allowed" band and r below 1 is the merge band.
        const float splitRatio = p.Screen[3] > 0.0f ? p.Screen[2] / p.Screen[3] : 2.0f;
        constexpr float kOneLebLevel = 1.41421356f; // sqrt(2): one bisection of the split edge

        uint64_t sibPairs = 0, bothMerge = 0, onlyEven = 0, onlyOdd = 0, neither = 0;
        uint64_t blockerEyePlane = 0, blockerAtCap = 0, blockerOverSplit = 0, blockerDeadBand = 0;
        std::array<uint64_t, kRatioBuckets> blockerHist{}, mergerHist{}, parentHist{};
        double blockerRatioSum = 0.0, spreadSum = 0.0, worldRatioSum = 0.0;
        uint64_t spreadN = 0, worldN = 0, parentRated = 0, parentWouldMerge = 0;
        uint64_t parentReconPairs = 0, parentReconDisagree = 0;
        float blockerRatioMax = 0.0f, worldRatioMin = 1e30f, worldRatioMax = 0.0f;
        // Which corner indices do two siblings share? Settles the LEB corner convention from data
        // rather than from the decode's documentation: [i][j] counts pairs whose even-child corner
        // i is bit-equal to odd-child corner j. Computed over EVERY pair, so the parent
        // reconstruction below rests on a full-population fact, not a sample.
        uint64_t shareMatrix[3][3] = {};

        for (size_t i = 0; i + 1 < live.size(); ++i)
        {
            const uint64_t hA = live[i].first, hB = live[i + 1].first;
            if ((hA >> 1) != (hB >> 1))
                continue;
            ++sibPairs;
            const uint32_t depth = HeapDepth(hA);
            const bool aIsEven = (hA & 1ull) == 0ull;
            const uint32_t evenSlot = aIsEven ? live[i].second : live[i + 1].second;
            const uint32_t oddSlot = aIsEven ? live[i + 1].second : live[i].second;
            const std::array<float, 3> ec[3] = {corner(evenSlot, 0), corner(evenSlot, 1),
                                                corner(evenSlot, 2)};
            const std::array<float, 3> oc[3] = {corner(oddSlot, 0), corner(oddSlot, 1),
                                                corner(oddSlot, 2)};
            for (uint32_t ci = 0; ci < 3u; ++ci)
                for (uint32_t cj = 0; cj < 3u; ++cj)
                    if (ec[ci] == oc[cj])
                        ++shareMatrix[ci][cj];

            // Would a PARENT-level merge test agree between the two children? Each child can only
            // reconstruct the parent's hypotenuse from its own corners — the even child from
            // (c0, 2*c1 - c0), the odd from (2*c1 - c2, c2) — and those are different fp
            // expressions for the same segment. This measures the residual disagreement the
            // recommended fix would carry, over the whole live population.
            {
                const std::array<float, 3> evFar = {2.0f * ec[1][0] - ec[0][0],
                                                    2.0f * ec[1][1] - ec[0][1],
                                                    2.0f * ec[1][2] - ec[0][2]};
                const std::array<float, 3> odFar = {2.0f * oc[1][0] - oc[2][0],
                                                    2.0f * oc[1][1] - oc[2][1],
                                                    2.0f * oc[1][2] - oc[2][2]};
                const Proj a0 = ProjectPix(p.ViewProjRel, ec[0]);
                const Proj a1 = ProjectPix(p.ViewProjRel, evFar);
                const Proj b0 = ProjectPix(p.ViewProjRel, odFar);
                const Proj b1 = ProjectPix(p.ViewProjRel, oc[2]);
                if (a0.InFront && a1.InFront && b0.InFront && b1.InFront)
                {
                    float sPx = 0.0f, mPx = 0.0f;
                    thresholdsAt(ec[1], sPx, mPx);
                    const bool mergeFromEven =
                        std::hypot(a0.Px - a1.Px, a0.Py - a1.Py) < mPx * kOneLebLevel;
                    const bool mergeFromOdd =
                        std::hypot(b0.Px - b1.Px, b0.Py - b1.Py) < mPx * kOneLebLevel;
                    ++parentReconPairs;
                    if (mergeFromEven != mergeFromOdd)
                        ++parentReconDisagree;
                }
            }

            const uint64_t evenHeap = aIsEven ? hA : hB;
            const uint64_t oddHeap = aIsEven ? hB : hA;
            const PlanarDecision dEven = ClassifyPlanarFacet(
                p, contention, occ, depth, a.MaxDepth, baseDepth, evenHeap, ec[0], ec[1], ec[2]);
            const PlanarDecision dOdd = ClassifyPlanarFacet(
                p, contention, occ, depth, a.MaxDepth, baseDepth, oddHeap, oc[0], oc[1], oc[2]);

            // World congruence of the two split edges, over the WHOLE population rather than only
            // the pairs that disagree: it is the structural fact the parent metric rests on (the
            // pixel-space spread is projection, not geometry), and with the parent metric in the
            // disagreeing population is empty, which would leave nothing to measure it on.
            const float wEvenAll = worldLen(ec[0], ec[2]), wOddAll = worldLen(oc[0], oc[2]);
            if (wEvenAll > 0.0f)
            {
                const float wr = wOddAll / wEvenAll;
                worldRatioSum += wr;
                ++worldN;
                worldRatioMin = std::min(worldRatioMin, wr);
                worldRatioMax = std::max(worldRatioMax, wr);
            }
            if (dEven.WantMerge && dOdd.WantMerge)
            {
                ++bothMerge;
                continue;
            }
            if (dOdd.WantMerge)
            {
                ++onlyOdd; // no candidate is even enqueued: Classify pushes only the EVEN heapID
                continue;
            }
            if (!dEven.WantMerge)
            {
                ++neither;
                continue;
            }
            ++onlyEven; // this parent contributes one PrepareSimplify entry, and it cannot be served

            ++mergerHist[RatioBucket(dEven.Ratio)];
            if (dOdd.EyePlane)
                ++blockerEyePlane;
            else
            {
                if (depth >= a.MaxDepth)
                    ++blockerAtCap;
                if (dOdd.Ratio > splitRatio)
                    ++blockerOverSplit;
                else
                    ++blockerDeadBand;
                ++blockerHist[RatioBucket(dOdd.Ratio)];
                blockerRatioSum += dOdd.Ratio;
                blockerRatioMax = std::max(blockerRatioMax, dOdd.Ratio);
                if (dEven.Ratio > 0.0f)
                {
                    spreadSum += dOdd.Ratio / dEven.Ratio;
                    ++spreadN;
                }
            }

            // The parent this diamond would collapse to. Its hypotenuse runs between the children's
            // UNSHARED corners (even c0, odd c2) with the shared corner c1 as its midpoint — the
            // one piece of geometry every diamond member agrees on. Would a parent-level metric
            // have asked for this merge at all?
            const Proj pp0 = ProjectPix(p.ViewProjRel, ec[0]);
            const Proj pp2 = ProjectPix(p.ViewProjRel, oc[2]);
            if (pp0.InFront && pp2.InFront)
            {
                float sPx = 0.0f, mPx = 0.0f;
                thresholdsAt(ec[1], sPx, mPx);
                const float parentPx = std::hypot(pp0.Px - pp2.Px, pp0.Py - pp2.Py);
                const float pr = mPx > 0.0f ? parentPx / mPx : 0.0f;
                ++parentHist[RatioBucket(pr)];
                ++parentRated;
                if (parentPx < mPx * kOneLebLevel)
                    ++parentWouldMerge;
            }
        }

        const uint64_t rated = onlyEven - blockerEyePlane;
        std::printf("\n[sib:%s] converged live=%u occ=%.4f | mergeDemand=%d mergeServed=%d "
                    "splitDemand=%d splitServed=%d overflowTail=%d\n",
                    a.Label, r.Final.LiveCount, r.FinalOcc, r.Final.MergeDemand,
                    r.Final.MergeServed, r.Final.SplitDemand, r.Final.SplitServed,
                    r.OverflowDeltaTail);
        std::printf("[sib:%s] sibling pairs=%llu | bothMerge=%llu onlyEven=%llu onlyOdd=%llu "
                    "neither=%llu | GPU mergeDemand=%d (onlyEven+bothMerge=%llu, remainder has no "
                    "live sibling)\n",
                    a.Label, static_cast<unsigned long long>(sibPairs),
                    static_cast<unsigned long long>(bothMerge),
                    static_cast<unsigned long long>(onlyEven),
                    static_cast<unsigned long long>(onlyOdd),
                    static_cast<unsigned long long>(neither), r.Final.MergeDemand,
                    static_cast<unsigned long long>(onlyEven + bothMerge));
        std::printf("[sib:%s] blocker class: eyePlane=%llu atCap=%llu overSplitBand=%llu "
                    "deadBand=%llu | mean r=%.4f max r=%.4f mean(r_blocker/r_merger)=%.4f\n",
                    a.Label, static_cast<unsigned long long>(blockerEyePlane),
                    static_cast<unsigned long long>(blockerAtCap),
                    static_cast<unsigned long long>(blockerOverSplit),
                    static_cast<unsigned long long>(blockerDeadBand),
                    rated ? blockerRatioSum / static_cast<double>(rated) : 0.0, blockerRatioMax,
                    spreadN ? spreadSum / static_cast<double>(spreadN) : 0.0);
        // The legend is per-arm: a spread away from 1.0 means opposite things in the two regimes.
        // Flat, the edges ARE congruent, so any pixel spread is projection. With relief the world
        // lengths genuinely differ, and reading that as projection is the error this arm exists to
        // prevent.
        std::printf("[sib:%s] WORLD split-edge length ratio odd/even over ALL pairs: n=%llu "
                    "mean=%.6f min=%.6f max=%.6f (%s)\n",
                    a.Label, static_cast<unsigned long long>(worldN),
                    worldN ? worldRatioSum / static_cast<double>(worldN) : 0.0, worldRatioMin,
                    worldRatioMax,
                    a.HeightScale == 0.0f
                        ? "flat: 1.0 == congruent, so any pixel spread is PROJECTION"
                        : "relief: the spread is real GEOMETRY, not projection");
        std::printf("[sib:%s] PARENT diamond metric on blocked pairs: rated=%llu wouldMerge "
                    "(parentPx < sqrt(2)*mergePx)=%llu (%.1f%%)\n",
                    a.Label, static_cast<unsigned long long>(parentRated),
                    static_cast<unsigned long long>(parentWouldMerge),
                    parentRated ? 100.0 * static_cast<double>(parentWouldMerge) /
                                      static_cast<double>(parentRated)
                                : 0.0);
        std::printf("[sib:%s] PARENT reconstruction agreement across the sibling pair: n=%llu "
                    "disagree=%llu (%.4f%%) — the fp residual a parent-level merge test carries\n",
                    a.Label, static_cast<unsigned long long>(parentReconPairs),
                    static_cast<unsigned long long>(parentReconDisagree),
                    parentReconPairs ? 100.0 * static_cast<double>(parentReconDisagree) /
                                           static_cast<double>(parentReconPairs)
                                     : 0.0);
        std::printf("[sib:%s] r = edgePx/mergePx histogram (merger | blocker | parent):\n", a.Label);
        for (uint32_t b = 0; b < kRatioBuckets; ++b)
            std::printf("[sib:%s]   %-9s %10llu | %10llu | %10llu\n", a.Label, kRatioLabels[b],
                        static_cast<unsigned long long>(mergerHist[b]),
                        static_cast<unsigned long long>(blockerHist[b]),
                        static_cast<unsigned long long>(parentHist[b]));
        std::printf("[sib:%s] sibling shared-corner matrix (evenCorner x oddCorner, all %llu "
                    "pairs):\n",
                    a.Label, static_cast<unsigned long long>(sibPairs));
        for (uint32_t ci = 0; ci < 3u; ++ci)
            std::printf("[sib:%s]   c%u: %8llu %8llu %8llu\n", a.Label, ci,
                        static_cast<unsigned long long>(shareMatrix[ci][0]),
                        static_cast<unsigned long long>(shareMatrix[ci][1]),
                        static_cast<unsigned long long>(shareMatrix[ci][2]));

        ASSERT_GT(sibPairs, 1000u) << a.Label << ": too few sibling pairs to characterize";

        // 1. The corner convention. Every pair shares c1 with c1 and evenC2 with oddC0, and shares
        //    nothing else — so evenC0 and oddC2 are the parent hypotenuse endpoints and c1 is its
        //    midpoint. A decode change that broke this would silently invalidate any parent-level
        //    metric built on it.
        EXPECT_EQ(shareMatrix[1][1], sibPairs) << a.Label << ": siblings no longer share c1";
        EXPECT_EQ(shareMatrix[2][0], sibPairs) << a.Label << ": evenC2 is no longer oddC0";
        for (uint32_t ci = 0; ci < 3u; ++ci)
            for (uint32_t cj = 0; cj < 3u; ++cj)
                if (!(ci == 1u && cj == 1u) && !(ci == 2u && cj == 0u))
                    EXPECT_EQ(shareMatrix[ci][cj], 0u)
                        << a.Label << ": unexpected shared corner c" << ci << "/c" << cj;

        // 2 and 3 are REGIME-SPLIT, because both are exact only on flat ground.
        //
        // Sibling split edges are congruent in world space, and either child reconstructs the same
        // parent hypotenuse, ONLY when heightScale is 0 and every corner shares one y. With relief
        // each leg carries its own sampled heights, so the world lengths diverge, and each child
        // EXTRAPOLATES the shared endpoint's height (2*h(mid) - h(held)) rather than recovering it.
        // Asserting the exact form on a flat-only arm is how a green suite says nothing about a
        // production terrain, which ships heightScale 60.
        ASSERT_GT(worldN, 1000u) << a.Label << ": congruence sample too small";
        ASSERT_GT(parentReconPairs, 1000u) << a.Label << ": reconstruction sample too small";
        if (a.HeightScale == 0.0f)
        {
            EXPECT_NEAR(worldRatioMin, 1.0f, 1e-4f)
                << a.Label << ": sibling split edges are not congruent on flat ground";
            EXPECT_NEAR(worldRatioMax, 1.0f, 1e-4f)
                << a.Label << ": sibling split edges are not congruent on flat ground";
            EXPECT_EQ(parentReconDisagree, 0u)
                << a.Label << ": " << parentReconDisagree << " of " << parentReconPairs
                << " sibling pairs reconstruct the parent hypotenuse to opposite merge decisions "
                   "where that reconstruction is exact";
        }
        else
        {
            // Relief. The world-length spread is real geometry, not a defect, and it is also why a
            // WORLD-space merge bound is not diamond-consistent either (see the near-field note in
            // cbt_kernels.comp). These bounds assert the spread EXISTS rather than pinning a value,
            // so the arm cannot pass by silently losing its height source.
            EXPECT_LT(worldRatioMin, 0.95f)
                << a.Label << ": relief arm reports congruent split edges, so the height source "
                              "never reached the corners";
            EXPECT_GT(worldRatioMax, 1.05f)
                << a.Label << ": relief arm reports congruent split edges, so the height source "
                              "never reached the corners";
            // The reconstruction residual is the price of the parent metric, and it is only worth
            // paying while it stays far below the sibling disagreement the per-child rule carried.
            const double disagreeFraction =
                static_cast<double>(parentReconDisagree) / static_cast<double>(parentReconPairs);
            EXPECT_LT(disagreeFraction, 0.02)
                << a.Label << ": " << parentReconDisagree << " of " << parentReconPairs << " ("
                << 100.0 * disagreeFraction
                << "%) sibling pairs reconstruct to opposite merge decisions on relief";
        }
        inst.Shutdown();
    }
}

// ============================================================================
// The planar pool-pressure scale. A planar view whose VISIBLE field alone exceeds the pool used to
// pin it at 100% and freeze the topology (#1450): every split rolled back, no merge freeing
// anything, an edit or target change in view unable to re-tessellate. The fixture is the editor's
// "Terrain — Large (4 km)" preset — 4096 m, HeightScale 512, the tiled world-space value noise the
// terrain service fills it with — seen from (0, 650, 0) looking 55 degrees down through a 3067x900
// viewport at TargetPixelError 8, which holds 92% of the pool in the frustum. The control arm holds
// the scale at 1x and must pin; the treatment arm must reach a fixed point inside the hold band with
// headroom and zero rolled-back splits, and must reach one again after a turn-and-back. The STEP is
// not the invariant — the climb happens while the pool is still filling, so it lands within one step
// of itself depending on the excursion; the pool resting under the full mark with nothing rolled
// back is what holds on every path.
// ============================================================================
namespace
{
constexpr float kPresetSizeM = 4096.0f;
constexpr float kPresetHeightScale = 512.0f;
constexpr uint32_t kPresetReliefDim = 4097u;
constexpr uint32_t kPresetMaxDepth = 25u;
constexpr float kPresetTpe = 8.0f;
constexpr float kPresetAltM = 650.0f;
constexpr float kPresetPitchDeg = -55.0f;
constexpr float kPresetScreenW = 3067.0f;
constexpr float kPresetScreenH = 900.0f;
// TerrainService.h kTileNoise*: the relief a tiled planar terrain with the default (ProceduralNoise)
// base is provisioned with.
constexpr float kTileNoiseFrequency = 0.004f;
constexpr uint32_t kTileNoiseOctaves = 5u;
constexpr uint32_t kTileNoiseSeed = 42u;
// The controller's marks are the engine's existing saturation and pressure constants, not numbers
// of this test's own — cbt_layout.glsl's CBT_PRESSURE_FULL_OCC / CBT_PRESSURE_RECOVER_OCC are
// locked to these two by CBTLayout.GlslPressureConstantsMirrorCpp.
constexpr double kPressureFullOcc = kCBTSaturatedOccupancy;
constexpr double kPressureRecoverOcc = static_cast<double>(kOffFrustumKeepOcc);

// Heightfield.cpp NoiseHash / SmoothNoise, so the fixture carries the shipped relief rather than a
// synthetic one: the preset pins only because of this relief's small-scale slopes.
float TileNoiseHash(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u +
                 seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h & 0x7FFFFFFFu) / static_cast<float>(0x7FFFFFFF);
}

float TileSmoothNoise(float x, float z, uint32_t seed)
{
    const int32_t ix = static_cast<int32_t>(std::floor(x));
    const int32_t iz = static_cast<int32_t>(std::floor(z));
    const float fx = x - static_cast<float>(ix), fz = z - static_cast<float>(iz);
    const float sx = fx * fx * (3.0f - 2.0f * fx), sz = fz * fz * (3.0f - 2.0f * fz);
    const float n00 = TileNoiseHash(ix, iz, seed), n10 = TileNoiseHash(ix + 1, iz, seed);
    const float n01 = TileNoiseHash(ix, iz + 1, seed), n11 = TileNoiseHash(ix + 1, iz + 1, seed);
    const float nx0 = n00 + sx * (n10 - n00), nx1 = n01 + sx * (n11 - n01);
    return nx0 + sz * (nx1 - nx0);
}

TextureHandle MakeTiledPresetHeightTexture(IDevice& device, uint32_t dim, float originX,
                                           float originZ, float sizeM)
{
    TextureDesc td{};
    td.width = dim;
    td.height = dim;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.Test.TiledPresetRelief";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;
    std::vector<float> data(static_cast<size_t>(dim) * dim);
    const float spacing = sizeM / static_cast<float>(dim - 1u);
    for (uint32_t y = 0; y < dim; ++y)
        for (uint32_t x = 0; x < dim; ++x)
        {
            const float wx = originX + static_cast<float>(x) * spacing;
            const float wz = originZ + static_cast<float>(y) * spacing;
            float value = 0.0f, freq = kTileNoiseFrequency, amp = 1.0f, total = 0.0f;
            for (uint32_t o = 0; o < kTileNoiseOctaves; ++o)
            {
                value += TileSmoothNoise(wx * freq, wz * freq, kTileNoiseSeed + o) * amp;
                total += amp;
                freq *= 2.0f;
                amp *= 0.5f;
            }
            data[static_cast<size_t>(y) * dim + x] = value / total;
        }
    const size_t bytes = data.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(dim) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.TiledPresetReliefStaging");
    device.UpdateBuffer(staging, 0, bytes, data.data());
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, dim, dim, 0, rowPitch, 1, 0, 0, 0,
                                       ResourceState::Undefined);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
    return tex;
}

// A phase's verdict window: the trailing frames it is judged over, after the transient it opens
// with. Live-count spread across the window is the fixed-point test, so the window has to be long
// enough that a slow limit cycle could not sit inside it.
struct PresetPhase
{
    CBTTessellationStats Final{};
    double FinalOcc = 0.0;
    int32_t WindowOverflowDelta = 0;
    uint32_t WindowLiveMin = 0xFFFFFFFFu, WindowLiveMax = 0;
    int32_t WindowSplitDemand = 0, WindowSplitServed = 0;
};
} // namespace

TEST_F(CBTPlanarDemandGate, OverDemandingPlanarPoseConvergesUnderThePoolPressureScale)
{
    using namespace GameEngine::Mathematics;
    // The rest and back phases are judged over a 100-frame window: settling takes ~72 frames from
    // cold and ~60 from a full drain, so a 200-frame phase leaves the window entirely inside the
    // rest state, and a limit cycle slower than the controller's own 1-step-per-frame rate would
    // still show up as live-count spread. The turn phase only has to drain the pool, so it keeps a
    // short window and carries no fixed-point assertion.
    constexpr uint32_t kFillFrames = 200u;
    constexpr uint32_t kTurnFrames = 100u;
    constexpr uint32_t kBackFrames = 200u;
    constexpr uint32_t kRestWindow = 100u;
    constexpr uint32_t kTurnWindow = 20u;

    auto buildParams = [&](CBTFrameParams& p, float yawDeg) {
        p = CBTFrameParams{};
        p.Screen[0] = kPresetScreenW;
        p.Screen[1] = kPresetScreenH;
        p.Screen[2] = kPresetTpe;
        p.Screen[3] = kPresetTpe * 0.5f;
        p.TerrainSize[0] = kPresetSizeM;
        p.TerrainSize[1] = kPresetSizeM;
        p.TerrainSize[2] = kPresetHeightScale;
        p.TerrainSize[3] = 0.0f; // the preset's origin Y: heights in [0, HeightScale]
        p.TerrainOrigin[0] = -0.5f * kPresetSizeM;
        p.TerrainOrigin[1] = -0.5f * kPresetSizeM;
        p.TerrainOrigin[2] = static_cast<float>(kPresetMaxDepth);
        p.TerrainOrigin[3] = 0.0f;
        const Vector3 eye(0.0f, kPresetAltM, 0.0f);
        const float pitch = kPresetPitchDeg * 0.017453292519943295f;
        const float yaw = yawDeg * 0.017453292519943295f;
        const Vector3 fwd(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                          std::cos(yaw) * std::cos(pitch));
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(
            1.0471975512f, kPresetScreenW / kPresetScreenH, 0.5f, 20000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + fwd, Vector3(0, 1, 0));
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        // The editor's demand shaping at this altitude (CBTRenderFeature.cpp).
        const CBTNearBiasRadii nb = ComputeNearBiasRadii(kPresetAltM);
        p.NearBias[0] = 1.0f;
        p.NearBias[1] = nb.NearRadius;
        p.NearBias[2] = nb.FarRadius;
        p.NearBias[3] = kNearBiasMaxCoarsen;
        p.DemandTuning[0] = kNearFieldFacetTargetM;
        p.DemandTuning[1] = kOffFrustumKeepOcc;
        p.DemandTuning[2] = kEdgeRescueTpeMul * kPresetTpe;
        p.DemandTuning[3] = kEdgeRescueOcc;
        p.PriorityParams[0] = 1.0f;
        p.PriorityParams[1] = kPriorityRampStartOcc;
        p.PriorityParams[2] = kPriorityMaxAreaFloorPx2;
        p.PriorityParams[3] = kPriorityKeepLargeNdc;
    };

    // One instance driven through three poses; the tail window of each phase is sampled.
    auto run = [&](uint32_t poolPressure, const char* label) {
        std::vector<PresetPhase> phases;
        CBTInstance inst;
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainPlanar))
        {
            ADD_FAILURE() << label << ": CBTInstance init failed";
            phases.resize(3);
            return phases;
        }
        const TextureHandle relief = MakeTiledPresetHeightTexture(
            *m_Device, kPresetReliefDim, -0.5f * kPresetSizeM, -0.5f * kPresetSizeM, kPresetSizeM);
        if (!relief.IsValid())
        {
            ADD_FAILURE() << label << ": relief texture failed";
            inst.Shutdown();
            phases.resize(3);
            return phases;
        }
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            inst.SetHeightSource(slot, relief);

        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = kPresetMaxDepth;
        c.NearFieldGate = 1u;
        c.PoolPressure = poolPressure;
        CBTFrameParams front, turned;
        buildParams(front, 0.0f);
        buildParams(turned, 180.0f);
        const struct {
            const CBTFrameParams* Params;
            uint32_t Frames;
            uint32_t Window;
            const char* Name;
        } poses[] = {{&front, kFillFrames, kRestWindow, "rest"},
                     {&turned, kTurnFrames, kTurnWindow, "turned"},
                     {&front, kBackFrames, kRestWindow, "back"}};
        uint32_t frame = 0;
        for (const auto& pose : poses)
        {
            PresetPhase r;
            int32_t overflowAtWindowStart = 0;
            for (uint32_t f = 0; f < pose.Frames; ++f, ++frame)
            {
                c.GateVertexEval = (frame == 0u) ? 0u : 1u;
                RunFrame(inst, c, *pose.Params, frame);
                if (f + pose.Window >= pose.Frames)
                {
                    const CBTTessellationStats s = inst.ReadTessellationStats();
                    if (f + pose.Window == pose.Frames)
                        overflowAtWindowStart = s.OverflowTotal;
                    r.WindowLiveMin = std::min(r.WindowLiveMin, s.LiveCount);
                    r.WindowLiveMax = std::max(r.WindowLiveMax, s.LiveCount);
                    r.WindowSplitDemand += s.SplitDemand;
                    r.WindowSplitServed += s.SplitServed;
                }
            }
            r.Final = inst.ReadTessellationStats();
            r.FinalOcc = static_cast<double>(r.Final.LiveCount) / r.Final.PoolSize;
            r.WindowOverflowDelta = r.Final.OverflowTotal - overflowAtWindowStart;
            std::printf("[pool-pressure %s %s] live=%u occ=%.4f free=%d step=%d scale=%.3f "
                        "window=%uf split=%d/%d overflow=%d liveSpread=%u mergeDemand=%d\n",
                        label, pose.Name, r.Final.LiveCount, r.FinalOcc, r.Final.FreeCount,
                        r.Final.PressureStep,
                        std::exp2(r.Final.PressureStep / static_cast<double>(kPressureStepsPerOctave)),
                        pose.Window, r.WindowSplitDemand, r.WindowSplitServed,
                        r.WindowOverflowDelta, r.WindowLiveMax - r.WindowLiveMin,
                        r.Final.MergeDemand);
            phases.push_back(r);
        }
        EXPECT_EQ(ValidationErrors(inst), 0u) << label << ": tree must stay conforming";
        inst.Shutdown();
        m_Device->DestroyTexture(relief);
        return phases;
    };

    const std::vector<PresetPhase> pinned = run(0u, "off");
    const std::vector<PresetPhase> scaled = run(1u, "on");
    ASSERT_EQ(pinned.size(), 3u);
    ASSERT_EQ(scaled.size(), 3u);

    // ---- POSITIVE CONTROL: with the scale held at 1x this pose really is over-demanding ----
    EXPECT_GT(pinned[0].FinalOcc, 0.99)
        << "control arm did not pin — the pose cannot discriminate the scale";
    EXPECT_GT(pinned[0].WindowOverflowDelta, 0)
        << "control arm rolls back no splits — there is no frozen frontier to cure";
    EXPECT_GT(pinned[0].WindowSplitDemand, 0);
    // Frozen, not literally motionless: the pinned pool stands at one free slot, so over a long
    // window a handful of splits do land as merges hand slots back (16 of 1.53M demanded across the
    // measured 100 frames). What makes it frozen is that essentially everything it is asked for is
    // rolled back instead — a threshold, not an exact zero, or the assertion just measures the
    // window length.
    EXPECT_LT(pinned[0].WindowSplitServed, 1 + pinned[0].WindowSplitDemand / 1000)
        << "control arm serves a real share of its split demand — the pool is not frozen";
    EXPECT_EQ(pinned[0].Final.PressureStep, 0) << "the off arm must hold the scale at 1x";

    // ---- TREATMENT ----
    // 1. The scale engaged and the pool holds the view under the full mark with real headroom.
    EXPECT_GT(scaled[0].Final.PressureStep, 0) << "the scale never engaged on an over-demanding pose";
    EXPECT_LT(scaled[0].FinalOcc, kPressureFullOcc) << "the pool is still at the full mark";
    EXPECT_GE(scaled[0].FinalOcc, kPressureRecoverOcc)
        << "the scale overshot: the pool sits below the recover mark at rest";
    // 2. Nothing is frozen: every split the metric asks for is served and none is rolled back.
    EXPECT_EQ(scaled[0].WindowSplitDemand, scaled[0].WindowSplitServed)
        << "splits still go unserved at rest";
    EXPECT_EQ(scaled[0].WindowOverflowDelta, 0) << "splits are still rolled back at rest";
    // 3. A fixed point, not a churning equilibrium: the live count does not move at all across the
    //    100-frame window.
    EXPECT_EQ(scaled[0].WindowLiveMax, scaled[0].WindowLiveMin)
        << "live count still moving across the rest window — the scale is oscillating";
    // 4. The same contract after an excursion, on a pool the turn drained and the return refilled.
    //    The STEP is not part of the contract: the controller climbs while the pool is still
    //    filling, so it stops at the first step to read under the full mark for THAT refill and
    //    lands within one notch of the cold-start step. One notch is ~4.4% of threshold and ~3.5%
    //    of the field, which is what sets the live-count band below.
    EXPECT_GT(scaled[2].Final.PressureStep, 0) << "the scale did not re-engage after the return";
    EXPECT_LT(scaled[2].FinalOcc, kPressureFullOcc) << "the pool is back at the full mark after the return";
    EXPECT_GE(scaled[2].FinalOcc, kPressureRecoverOcc)
        << "the scale overshot after the return: the pool sits below the recover mark";
    const int32_t stepDelta = scaled[2].Final.PressureStep - scaled[0].Final.PressureStep;
    EXPECT_LE(stepDelta < 0 ? -stepDelta : stepDelta, 1)
        << "the step after turn-and-back is more than one notch from the step at rest";
    EXPECT_NEAR(static_cast<double>(scaled[2].Final.LiveCount), scaled[0].Final.LiveCount,
                0.04 * scaled[0].Final.LiveCount)
        << "the field after turn-and-back differs from the field at rest by more than one step";
    EXPECT_EQ(scaled[2].WindowSplitDemand, scaled[2].WindowSplitServed)
        << "splits go unserved after the return";
    EXPECT_EQ(scaled[2].WindowOverflowDelta, 0) << "splits still rolled back after the return";
    EXPECT_EQ(scaled[2].WindowLiveMax, scaled[2].WindowLiveMin) << "not a fixed point after the return";
}
