// Near-field merge direction, in BOTH domains.
//
// Kernel_Classify's near-field paths (the behind-eye / straddling force-split branches, one shared
// by both domains and one spherical-only apex test) force-split toward a world facet target. They
// must also decide the MERGE direction, or their facets can never carry CBT_STATE_SIMPLIFY and a
// facet that has become far finer than the target — because the target was raised, or the pose
// moved — stays pinned at its old size for as long as it remains on that branch.
//
// The branch is shared, so it is tested in both domains here rather than in either domain's
// demand-gate file.

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
#include "CBTTerrain/CBTSphereRoots.h" // kSphereBaseDepth
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u;
constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 900.0f;
constexpr float kSplitPx = 8.0f;
constexpr float kMergePx = 4.0f;
// The shipped near-field world facet target (CBTRenderFeature kNearFieldFacetTargetM).
constexpr float kNearFieldFacetTargetM = 0.5f;
// 16x the converged facet, so a coarsening near field has ~4 LEB levels to give back.
constexpr float kRaisedNearFieldTargetM = 8.0f;
constexpr float kOneLebLevel = 1.41421356f;

struct Proj
{
    float Px = 0.0f, Py = 0.0f;
    bool InFront = false;
};

// CPU mirror of CBT_ProjectPixels (cbt_layout.glsl): column-major clip = M * (x,y,z,1); InFront is
// false when the point is at/behind the near plane (clip.w <= 1e-5).
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

float WorldLen(const std::array<float, 3>& u, const std::array<float, 3>& v)
{
    const float dx = u[0] - v[0], dy = u[1] - v[1], dz = u[2] - v[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Steady-state readings over a settled tail: what the frame ASKS for versus what it SERVES, plus
// how much the topology actually moves. Separates a starved fixed point (served 0, live frozen)
// from churn (served > 0, live oscillating) — the probe the demand counters alone cannot settle.
struct TailReading
{
    int32_t SplitDemand = 0, SplitServed = 0, MergeDemand = 0, MergeServed = 0;
    int32_t SplitServedMax = 0, MergeServedMax = 0;
    int32_t OverflowDelta = 0;
    uint32_t LiveMin = 0xFFFFFFFFu, LiveMax = 0;
};
} // namespace

class CBTNearFieldMerge : public ::testing::Test
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

    // Live facets that take a near-field branch (the shader's !v0In || !v2In split-edge test) AND
    // whose world split edge sits below `mergeBelowM` — exactly the set the near-field merge rule
    // claims it will drain.
    uint64_t CountNearFieldBelow(CBTInstance& inst, const CBTFrameParams& p, float mergeBelowM)
    {
        const std::vector<uint64_t> heap = ReadHeap(inst);
        const std::vector<uint32_t> verts = inst.DebugReadWords(
            CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        auto asFloat = [](uint32_t bits) { float f; std::memcpy(&f, &bits, sizeof(f)); return f; };
        uint64_t n = 0;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            if (heap[s] == 0u)
                continue;
            const uint32_t base = s * kVertexWordsPerSlot;
            const std::array<float, 3> c0{asFloat(verts[base + 0u]), asFloat(verts[base + 1u]),
                                          asFloat(verts[base + 2u])};
            const std::array<float, 3> c2{asFloat(verts[base + 8u]), asFloat(verts[base + 9u]),
                                          asFloat(verts[base + 10u])};
            if (ProjectPix(p.ViewProjRel, c0).InFront && ProjectPix(p.ViewProjRel, c2).InFront)
                continue;
            if (WorldLen(c0, c2) < mergeBelowM)
                ++n;
        }
        return n;
    }

    TailReading RunTail(CBTInstance& inst, const CBTClassifyDesc& descIn, const CBTFrameParams& p,
                        uint32_t& frame, uint32_t frames)
    {
        TailReading t;
        CBTClassifyDesc desc = descIn;
        const int32_t overflowBefore = inst.ReadWorkQueueCounter(kWQOverflowCounter);
        for (uint32_t i = 0; i < frames; ++i, ++frame)
        {
            desc.GateVertexEval = 1u;
            RunFrame(inst, desc, p, frame);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            t.SplitServedMax = std::max(t.SplitServedMax, s.SplitServed);
            t.MergeServedMax = std::max(t.MergeServedMax, s.MergeServed);
            t.LiveMin = std::min(t.LiveMin, s.LiveCount);
            t.LiveMax = std::max(t.LiveMax, s.LiveCount);
        }
        const CBTTessellationStats f = inst.ReadTessellationStats();
        t.SplitDemand = f.SplitDemand;
        t.SplitServed = f.SplitServed;
        t.MergeDemand = f.MergeDemand;
        t.MergeServed = f.MergeServed;
        t.OverflowDelta = inst.ReadWorkQueueCounter(kWQOverflowCounter) - overflowBefore;
        return t;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

namespace
{
// A planar terrain the pool can comfortably serve, walked at eye height.
constexpr float kPlanarSizeM = 128.0f;
constexpr uint32_t kPlanarMaxDepth = 19u;
constexpr float kPlanarAltM = 2.0f;

// The shipped planet: R=2000 with relief, walked at 5 m — the altitude at which the near-field
// merge is load-bearing, and the pose the runtime look is aimed at.
constexpr float kPlanetRadiusM = 2000.0f;
constexpr float kPlanetReliefAmp = 60.0f;
constexpr float kPlanetReliefFreq = 6.0f;
constexpr float kWalkAltM = 5.0f;
constexpr uint32_t kDescendFrames = 160u;
constexpr uint32_t kSettleFrames = 60u;

void BuildPlanarWalkParams(CBTFrameParams& p, uint32_t maxDepth)
{
    using namespace GameEngine::Mathematics;
    p = CBTFrameParams{};
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = kSplitPx;
    p.Screen[3] = kMergePx;
    p.TerrainSize[0] = kPlanarSizeM;
    p.TerrainSize[1] = kPlanarSizeM;
    p.TerrainOrigin[2] = static_cast<float>(maxDepth);

    const Vector3 eye(kPlanarSizeM * 0.5f, kPlanarAltM, kPlanarSizeM * 0.5f);
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.05f, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + Vector3(0, 0, 1), Vector3(0, 1, 0));
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;
    p.DemandTuning[0] = kNearFieldFacetTargetM;
}

void BuildPlanetParams(CBTFrameParams& p, uint32_t cap)
{
    p = CBTFrameParams{};
    p.PlanetParams[0] = kPlanetRadiusM;
    p.PlanetParams[1] = kPlanetReliefAmp;
    p.PlanetParams[2] = kPlanetReliefFreq;
    p.PlanetParams[3] = 4.0f;
    p.Screen[0] = kScreenW;
    p.Screen[1] = kScreenH;
    p.Screen[2] = kSplitPx;
    p.Screen[3] = kMergePx;
    p.TerrainOrigin[2] = static_cast<float>(cap);
    p.DemandTuning[0] = kNearFieldFacetTargetM;
}

void PlanetPoseAt(CBTFrameParams& p, float alt)
{
    using namespace GameEngine::Mathematics;
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.2f, 16.0f / 9.0f, 0.05f, 20000.0f);
    const Vector3 eye(0.0f, 0.0f, -(kPlanetRadiusM + alt));
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, Vector3(0, 0, 0), Vector3(0, 1, 0));
    std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
    p.CameraPos[0] = eye.x;
    p.CameraPos[1] = eye.y;
    p.CameraPos[2] = eye.z;
    p.CameraPos[3] = 1.0f;
}
} // namespace

// Red-green in the PLANAR domain: converge with a fine near-field target so the near field refines
// to it, then raise the target well above the converged facet size and keep running. With the merge
// direction decided the near field coarsens toward the new target; without it those facets never
// move — measured byte-identical live and near-field counts across the raise.
TEST_F(CBTNearFieldMerge, PlanarNearFieldCoarsensWhenItsTargetRises)
{
    constexpr uint32_t kPhaseFrames = 120u;
    constexpr float kBelowMergeM = kRaisedNearFieldTargetM / kOneLebLevel;

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainPlanar));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = kPlanarMaxDepth;
    c.NearFieldGate = 1u;
    CBTFrameParams p;
    BuildPlanarWalkParams(p, kPlanarMaxDepth);

    uint32_t frame = 0;
    for (uint32_t i = 0; i < kPhaseFrames; ++i, ++frame)
    {
        c.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, c, p, frame);
    }
    const uint32_t liveFine = inst.ReadTessellationStats().LiveCount;
    const uint64_t pinnedFine = CountNearFieldBelow(inst, p, kBelowMergeM);
    ASSERT_GT(pinnedFine, 1000u) << "no near-field population — the branch under test is idle";
    ASSERT_EQ(ValidationErrors(inst), 0u) << "the refined tree must start conforming";

    p.DemandTuning[0] = kRaisedNearFieldTargetM;
    for (uint32_t i = 0; i < kPhaseFrames; ++i, ++frame)
    {
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }
    const uint32_t liveCoarse = inst.ReadTessellationStats().LiveCount;
    const uint64_t pinnedCoarse = CountNearFieldBelow(inst, p, kBelowMergeM);

    std::printf("[nf:planar] target %.2f -> %.2f m | live %u -> %u | below-merge %llu -> %llu\n",
                static_cast<double>(kNearFieldFacetTargetM),
                static_cast<double>(kRaisedNearFieldTargetM), liveFine, liveCoarse,
                static_cast<unsigned long long>(pinnedFine),
                static_cast<unsigned long long>(pinnedCoarse));

    EXPECT_EQ(ValidationErrors(inst), 0u) << "the coarsened tree must stay conforming";
    EXPECT_LT(pinnedCoarse, pinnedFine / 2u)
        << "near field held " << pinnedCoarse << " facets under the merge threshold against "
        << pinnedFine << " after its target rose 16x — the branch never decides the merge direction";
    inst.Shutdown();
}

// The same property on the SPHERE, at the shipped walking pose. The spherical path reaches the
// near-field branches through different geometry — a curved surface puts the eye plane through the
// limb, and the horizon/frustum gates can override the branch verdict afterwards — so the planar
// arm does not cover it.
TEST_F(CBTNearFieldMerge, SphericalNearFieldCoarsensWhenItsTargetRises)
{
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    constexpr float kBelowMergeM = kRaisedNearFieldTargetM / kOneLebLevel;

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;
    c.NearFieldGate = 1u;
    CBTFrameParams p;
    BuildPlanetParams(p, cap);

    // Descend the way the audit probe does, so the tree converges through the path the runtime
    // takes rather than snapping to a pose it never reaches.
    uint32_t frame = 0;
    for (uint32_t i = 0; i < kDescendFrames; ++i, ++frame)
    {
        PlanetPoseAt(p, std::max(kWalkAltM, 300.0f * std::pow(0.96f, static_cast<float>(i))));
        c.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, c, p, frame);
    }
    PlanetPoseAt(p, kWalkAltM);
    for (uint32_t i = 0; i < kSettleFrames; ++i, ++frame)
    {
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }
    const uint32_t liveFine = inst.ReadTessellationStats().LiveCount;
    const uint64_t pinnedFine = CountNearFieldBelow(inst, p, kBelowMergeM);
    ASSERT_EQ(ValidationErrors(inst), 0u) << "the descended tree must start conforming";
    ASSERT_GT(pinnedFine, 100u) << "no near-field population at the walking pose";

    p.DemandTuning[0] = kRaisedNearFieldTargetM;
    for (uint32_t i = 0; i < 120u; ++i, ++frame)
    {
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }
    const uint32_t liveCoarse = inst.ReadTessellationStats().LiveCount;
    const uint64_t pinnedCoarse = CountNearFieldBelow(inst, p, kBelowMergeM);

    std::printf("[nf:sphere] target %.2f -> %.2f m | live %u -> %u | below-merge %llu -> %llu\n",
                static_cast<double>(kNearFieldFacetTargetM),
                static_cast<double>(kRaisedNearFieldTargetM), liveFine, liveCoarse,
                static_cast<unsigned long long>(pinnedFine),
                static_cast<unsigned long long>(pinnedCoarse));

    EXPECT_EQ(ValidationErrors(inst), 0u) << "the coarsened tree must stay conforming";
    EXPECT_LT(pinnedCoarse, pinnedFine / 2u)
        << "spherical near field held " << pinnedCoarse << " facets under the merge threshold "
        << "against " << pinnedFine << " after its target rose 16x";
    inst.Shutdown();
}

// Standing split demand at the walking pose: a STARVED fixed point (the same candidates unserved
// every frame — the class the split-bail fix cured) or CHURN (splits served every frame while
// facets cross between the near-field branch and the area metric)? The demand counter alone cannot
// tell those apart; served-in-the-tail and live-spread can.
TEST_F(CBTNearFieldMerge, WalkingPoseSplitDemandIsServedNotStarved)
{
    const uint32_t cap = kSphereBaseDepth + kMaxDecodeSubdiv;
    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;
    c.NearFieldGate = 1u;
    CBTFrameParams p;
    BuildPlanetParams(p, cap);

    uint32_t frame = 0;
    for (uint32_t i = 0; i < kDescendFrames; ++i, ++frame)
    {
        PlanetPoseAt(p, std::max(kWalkAltM, 300.0f * std::pow(0.96f, static_cast<float>(i))));
        c.GateVertexEval = (frame == 0u) ? 0u : 1u;
        RunFrame(inst, c, p, frame);
    }
    PlanetPoseAt(p, kWalkAltM);
    for (uint32_t i = 0; i < kSettleFrames; ++i, ++frame)
    {
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }

    const TailReading t = RunTail(inst, c, p, frame, 40u);
    const CBTTessellationStats s = inst.ReadTessellationStats();
    const double occ = s.PoolSize ? static_cast<double>(s.LiveCount) / s.PoolSize : 0.0;
    std::printf("[nf:walk] occ=%.4f free=%d | splitDemand=%d splitServed=%d (tailMax=%d) | "
                "mergeDemand=%d mergeServed=%d (tailMax=%d) | overflowDelta=%d liveSpread=%u\n",
                occ, s.FreeCount, t.SplitDemand, t.SplitServed, t.SplitServedMax, t.MergeDemand,
                t.MergeServed, t.MergeServedMax, t.OverflowDelta, t.LiveMax - t.LiveMin);

    EXPECT_EQ(ValidationErrors(inst), 0u) << "the walking pose must stay conforming";
    EXPECT_EQ(t.OverflowDelta, 0) << "splits are rolled back for want of slots at this pose";
    // Positive control: the near-field merge must generate demand at this pose,
    // or the served-not-starved assertion below measures nothing.
    ASSERT_GT(t.SplitDemand, 0)
        << "the walking pose generated no split demand — the near-field merge "
           "stopped firing here and this test can no longer discriminate";
    EXPECT_GT(t.SplitServedMax, 0)
        << "the walking pose holds " << t.SplitDemand
        << " split candidates never served across a whole 40-frame tail against " << s.FreeCount
        << " free slots — a starved fixed point, not churn";
    inst.Shutdown();
}
