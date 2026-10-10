// Far-field drain CURE probes (round-9 far-field-drain, design candidate 3) — and the measured
// FALSIFICATION of the design's core premise. The design recommended a conformity-safe reseed of the
// INTERIOR of a wholly-horizon-invisible root region (CBTInstance::RegionFreeToBase, driven by
// FarFieldReseedDetector), on the belief that ~90% of the pool is stranded in an invisible far field
// the LEB merge cannot cascade out of. Driving the REAL GPU kernels at her scene (R=50000, relief
// 1250, alt 4000) shows two things:
//
//   1. The region-reseed KERNEL is watertight (boundary conformity, the design's slice-2 gate): free a
//      deeply-REFINED root set (the near face the descent built) and the tree stays conforming — 0
//      validation errors — while the pool drains massively. The boundary-sanitation pass (CLEAR severs
//      retained neighbors' back-links; LINK writes INVALID toward retained neighbors) keeps the
//      base/refined seam a valid domain boundary. Re-entry rebuilds it, still conforming.
//
//   2. But the CURE has nothing to reclaim, because the premise is FALSE at her pose. The ~90%
//      occupancy is the VISIBLE near face (the +Z the camera looks at, roots 16-19 here), deeply
//      refined; the horizon-invisible far side is already COARSE (a handful of bisectors per root). On
//      a look-away the previously-refined face MERGES cleanly to base under a static camera (474k -> a
//      few) — the uniform horizon-merge cascades fine; it does NOT deadlock. So the detector's
//      interior-invisible free set holds almost no bisectors and the reseed drains nothing. The merge
//      stall the design measured is a STATIC-camera phenomenon on the VISIBLE, distance-graded near
//      field — not a stranded invisible far field. See the far-field-reseed report HTML.
//
// The detector's decision logic (hysteresis, edit non-interference, rate-limit) is locked by pure-CPU
// tests independent of the GPU.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
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
#include "CBTTerrain/CBTPlanetShading.h" // SphereCornerOccluded, kReliefEnvelope (CPU horizon mirror)
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTerrain/FarFieldReseedDetector.h"
#include "CBTTerrain/SphereSculptLayer.h"  // ResolveSculptPagePoolCount
#include "CBTTerrain/SphereSculptPaging.h" // MakeSphereSculptGeometry
#include "CBTTestHarness.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
using GameEngine::Mathematics::Vector3;

namespace
{
// S2a 96B CBTVertexData (sector tail appended; corner word offsets unchanged).
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24
constexpr double kScreenW = 1600.0;
constexpr double kScreenH = 900.0;
constexpr uint32_t kSphereBaseDepthLocal = 5u;
const Vector3 kDescentDir(0.03f, 0.0f, 1.0f);  // her oblique +Z pose
const Vector3 kAwayDir(0.03f, 0.0f, -1.0f);    // the antipodal look-away (+Z goes behind the horizon)

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
uint32_t RootOfHeap(uint64_t h)
{
    const uint32_t depth = HeapDepth(h);
    if (depth < kSphereBaseDepthLocal)
        return 0xFFFFFFFFu;
    return static_cast<uint32_t>(h >> (depth - kSphereBaseDepthLocal)) - (1u << kSphereBaseDepthLocal);
}
} // namespace

class CBTFarFieldReseedProbe : public ::testing::Test
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

    // Any kValidation*Counter slot. Safe to call repeatedly between frames: the counters only
    // change when an update runs.
    uint32_t ValidationCounter(CBTInstance& inst, uint32_t slot)
    {
        return inst.ReadValidationCounter(slot);
    }

    uint32_t ValidationErrors(CBTInstance& inst)
    {
        return ValidationCounter(inst, kValidationErrorCounter);
    }

    // Camera eye (planet-centre space) for the given altitude looking at the surface point under
    // `lookDir` — the horizon mirror uses the identical camera.
    Vector3 HerEye(float radius, float altM, Vector3 lookDir = kDescentDir) const
    {
        const Vector3 dir = lookDir.Normalize();
        const Vector3 surf = dir * radius;
        const Vector3 tangent = Vector3::Cross(dir, Vector3(0, 1, 0)).Normalize();
        return surf + dir * altM - tangent * (altM * 1.4f);
    }

    void BuildHerParams(CBTFrameParams& p, float radius, float altM, float splitPx, float mergePx,
                        const SphereSculptGeometry& geom, Vector3 lookDir = kDescentDir)
    {
        using namespace GameEngine::Mathematics;
        p = CBTFrameParams{};
        p.PlanetParams[0] = radius;
        p.PlanetParams[1] = 1250.0f;
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
        p.TerrainOrigin[2] = static_cast<float>(kSphereBaseDepthLocal + kMaxDecodeSubdiv);
        const Vector3 dir = lookDir.Normalize();
        const Vector3 surf = dir * radius;
        const Vector3 eye = HerEye(radius, altM, lookDir);
        const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(1.1f, 16.0f / 9.0f, 0.5f, 8000000.0f);
        const Matrix4x4 vp = proj * MakeLookAtLH(eye, surf, dir);
        std::memcpy(p.ViewProjRel, &vp.GetGLM()[0][0], sizeof(p.ViewProjRel));
        p.CameraPos[0] = eye.x;
        p.CameraPos[1] = eye.y;
        p.CameraPos[2] = eye.z;
        p.CameraPos[3] = 1.0f;
        const float camLen = std::sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z);
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

    uint32_t DescendToSaturation(CBTInstance& inst, CBTClassifyDesc& c, float radius, float restAltM,
                                 float splitPx, const SphereSculptGeometry& geom,
                                 Vector3 lookDir = kDescentDir)
    {
        const float descent[] = {160000.0f, 60000.0f, 24000.0f, 12000.0f, 7000.0f, restAltM};
        uint32_t frame = 0;
        for (int di = 0; di < 6; ++di)
        {
            CBTFrameParams p;
            BuildHerParams(p, radius, descent[di], splitPx, splitPx * 0.5f, geom, lookDir);
            for (uint32_t f = 0; f < 40u; ++f)
            {
                c.GateVertexEval = (frame == 0u) ? 0u : 1u;
                RunFrame(inst, c, p, frame);
                ++frame;
            }
        }
        return frame;
    }

    // Per-root live-bisector count + per-root horizon visibility (bit r) from a full GPU readback and
    // the CPU horizon mirror (SphereCornerOccluded — the exact cone test the kernel runs). A root is
    // horizon-VISIBLE if any of its live bisectors has a corner not floor-sphere-occluded.
    void ReadRootState(CBTInstance& inst, float radius, float altM, Vector3 lookDir,
                       std::array<uint32_t, kSphereRootCount>& rootLive, uint32_t& visMask,
                       uint32_t& liveMask)
    {
        const Vector3 eye = HerEye(radius, altM, lookDir);
        const std::array<float, 3> C = {eye.x, eye.y, eye.z};
        const float floorR = radius - kReliefEnvelope * std::abs(1250.0f);
        const auto heap = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        const auto vtx =
            inst.DebugReadWords(CBTBinding::CurrentVertex, kDefaultBisectorPoolSize * kVertexWordsPerSlot);
        rootLive.fill(0u);
        visMask = 0u;
        liveMask = 0u;
        for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
        {
            const uint64_t h = static_cast<uint64_t>(heap[s * 2u]) |
                               (static_cast<uint64_t>(heap[s * 2u + 1u]) << 32);
            if (h == 0u)
                continue;
            const uint32_t root = RootOfHeap(h);
            if (root >= kSphereRootCount)
                continue;
            ++rootLive[root];
            liveMask |= (1u << root);
            auto corner = [&](uint32_t ci) {
                std::array<float, 3> P;
                std::memcpy(P.data(), &vtx[s * kVertexWordsPerSlot + ci * 4u], 3 * sizeof(float));
                return P;
            };
            if (!(SphereCornerOccluded(corner(0), C, floorR) &&
                  SphereCornerOccluded(corner(1), C, floorR) &&
                  SphereCornerOccluded(corner(2), C, floorR)))
                visMask |= (1u << root);
        }
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// ============================================================================
// SLICE 2 (boundary conformity — THE HARD PART) — the region-reseed KERNEL is watertight on a real
// deeply-REFINED region. Descend to build the staircase on the near face, free that whole (refined)
// face's roots back to base in one RegionFreeToBase event, and assert the tree stays conforming (0
// validation errors) while the pool drains massively. This is the design's named gate ("free a region
// -> 0 validation errors; no zombies/T-junctions"), exercised against the deepest boundary the scene
// produces (freed refined face vs its retained neighbors), not the near-empty invisible far field.
// ============================================================================
TEST_F(CBTFarFieldReseedProbe, RegionFreeOfRefinedFaceIsConforming)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    c.NearFieldGate = 1u;
    uint32_t frame = DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);
    ASSERT_EQ(ValidationErrors(inst), 0u) << "the saturated tree must start conforming";

    // The deeply-refined near face = the roots holding the bulk of the pool. Free exactly those.
    std::array<uint32_t, kSphereRootCount> rootLive{};
    uint32_t visMask = 0u, liveMask = 0u;
    ReadRootState(inst, radius, 4000.0f, kDescentDir, rootLive, visMask, liveMask);
    uint32_t refinedMask = 0u;
    uint32_t refinedLive = 0u;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        if (rootLive[r] > 1000u)
        {
            refinedMask |= (1u << r);
            refinedLive += rootLive[r];
        }
    const CBTTessellationStats sSat = inst.ReadTessellationStats();
    std::printf("\n[kernel] refinedMask=0x%06x (%d roots, %u live of %u) — freeing it to base\n",
                refinedMask, std::popcount(refinedMask), refinedLive, sSat.LiveCount);
    ASSERT_NE(refinedMask, 0u);
    ASSERT_GT(refinedLive, kDefaultBisectorPoolSize / 4u)
        << "the refined face must hold a large share of the pool (a real boundary stress)";

    inst.RegionFreeToBase(refinedMask);
    const CBTTessellationStats sFreed = inst.ReadTessellationStats();
    // Validate the freed state by running one update at the AWAY pose (so the freed face is not
    // immediately re-refined) — the tree must be conforming through the free AND the next update.
    CBTFrameParams p;
    BuildHerParams(p, radius, 4000.0f, 6.5f, 3.25f, geom, kAwayDir);
    c.GateVertexEval = 0u;
    RunFrame(inst, c, p, frame++);
    const uint32_t valErrors = ValidationErrors(inst);
    std::printf("[kernel] after free: live %u->%u (occ %.4f->%.4f) validationErrors=%u\n",
                sSat.LiveCount, sFreed.LiveCount,
                static_cast<double>(sSat.LiveCount) / sSat.PoolSize,
                static_cast<double>(sFreed.LiveCount) / sFreed.PoolSize, valErrors);

    EXPECT_EQ(valErrors, 0u) << "freeing a refined region must leave a conforming tree (boundary gate)";
    // RegionFreeToBase collapses the live count by orders of magnitude, so the compact stream it
    // leaves behind is what the next update's indirect-over-live kernels dereference. This is the
    // exact path whose missing reindex made retained bisectors keep stale NEXT records; the counter
    // names that cause directly instead of leaving it to surface as broken link reciprocity.
    EXPECT_EQ(ValidationCounter(inst, kValidationCompactCounter), 0u)
        << "RegionFreeToBase left IndicesAll stale — the freed region's slots are still in the "
           "compact stream the next update runs over";
    EXPECT_LT(sFreed.LiveCount, sSat.LiveCount - refinedLive / 2u)
        << "the free must actually drain the refined region";
}

// ============================================================================
// Re-entry (design gate 5) — after freeing a refined region back to base, looking at it again rebuilds
// it via the normal update while staying conforming (no permanent seam that breaks Validate).
// ============================================================================
TEST_F(CBTFarFieldReseedProbe, FreedRegionReentryRebuildsConforming)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    c.NearFieldGate = 1u;
    uint32_t frame = DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);

    std::array<uint32_t, kSphereRootCount> rootLive{};
    uint32_t visMask = 0u, liveMask = 0u;
    ReadRootState(inst, radius, 4000.0f, kDescentDir, rootLive, visMask, liveMask);
    uint32_t refinedMask = 0u;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        if (rootLive[r] > 1000u)
            refinedMask |= (1u << r);
    inst.RegionFreeToBase(refinedMask);
    // Settle one away-frame, then look back at the freed face and rebuild.
    CBTFrameParams pa;
    BuildHerParams(pa, radius, 4000.0f, 6.5f, 3.25f, geom, kAwayDir);
    c.GateVertexEval = 0u;
    RunFrame(inst, c, pa, frame++);
    const CBTTessellationStats sFreed = inst.ReadTessellationStats();

    for (uint32_t f = 0; f < 40u; ++f, ++frame)
    {
        CBTFrameParams p;
        BuildHerParams(p, radius, 4000.0f, 6.5f, 3.25f, geom, kDescentDir); // look back
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }
    const CBTTessellationStats sBack = inst.ReadTessellationStats();
    std::printf("\n[reentry] freed live=%u -> look-back live=%u (occ %.4f) validationErrors=%u\n",
                sFreed.LiveCount, sBack.LiveCount,
                static_cast<double>(sBack.LiveCount) / sBack.PoolSize, ValidationErrors(inst));

    EXPECT_EQ(ValidationErrors(inst), 0u) << "re-entry must stay conforming";
    EXPECT_EQ(ValidationCounter(inst, kValidationCompactCounter), 0u)
        << "the compact stream drifted from the live set while rebuilding the freed region";
    EXPECT_GT(sBack.LiveCount, sFreed.LiveCount * 4u) << "looking back must rebuild the freed detail";
}

// ============================================================================
// FALSIFICATION (the load-bearing finding) — the horizon-invisible far field is NOT stranded, so the
// far-field reseed has nothing to reclaim. Measured: at the static descent pose the ~90% occupancy is
// the VISIBLE near face; the invisible far side is coarse. On a look-away the previously-refined face
// MERGES cleanly to base under a static camera (the uniform horizon-merge cascades — no deadlock), so
// the refinement simply follows the camera. The detector's interior-invisible free set therefore holds
// only a trivial number of bisectors. This documents WHY the design's premise (invisible far field
// stranded at ~90%) does not hold — the stall is the visible, distance-graded near field.
// ============================================================================
TEST_F(CBTFarFieldReseedProbe, HorizonInvisibleFarFieldIsNotStranded)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    c.NearFieldGate = 1u;
    uint32_t frame = DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);

    // (a) At the static descent pose: where is the pool? Sum the live count of horizon-INVISIBLE roots
    // vs VISIBLE roots. The design predicts the invisible far field holds ~90%; measurement shows the
    // opposite — the visible near face holds it.
    std::array<uint32_t, kSphereRootCount> rootLive{};
    uint32_t visMask = 0u, liveMask = 0u;
    ReadRootState(inst, radius, 4000.0f, kDescentDir, rootLive, visMask, liveMask);
    uint32_t liveVisible = 0u, liveInvisible = 0u, maxInvisibleRoot = 0u;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        if ((visMask >> r) & 1u)
            liveVisible += rootLive[r];
        else
        {
            liveInvisible += rootLive[r];
            maxInvisibleRoot = std::max(maxInvisibleRoot, rootLive[r]);
        }
    }
    const CBTTessellationStats sSat = inst.ReadTessellationStats();
    std::printf("\n[falsify] static descent: pool live=%u  VISIBLE roots hold=%u  INVISIBLE roots "
                "hold=%u (max single invisible root=%u)\n",
                sSat.LiveCount, liveVisible, liveInvisible, maxInvisibleRoot);
    EXPECT_GT(liveVisible, liveInvisible * 4u)
        << "the pool lives in the VISIBLE near field, not the invisible far field (premise falsified)";

    // (b) Look away (camera teleports to the antipode, then holds static). The previously-refined face
    // is now behind the horizon. Measure that it MERGES to base — the far field drains via the normal
    // merge, it is not stranded.
    const uint32_t refinedBefore = *std::max_element(rootLive.begin(), rootLive.end());
    const uint32_t refinedRoot =
        static_cast<uint32_t>(std::max_element(rootLive.begin(), rootLive.end()) - rootLive.begin());
    for (uint32_t f = 0; f < 60u; ++f, ++frame)
    {
        CBTFrameParams p;
        BuildHerParams(p, radius, 4000.0f, 6.5f, 3.25f, geom, kAwayDir);
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
    }
    std::array<uint32_t, kSphereRootCount> rootLiveAway{};
    uint32_t visAway = 0u, liveMaskAway = 0u;
    ReadRootState(inst, radius, 4000.0f, kAwayDir, rootLiveAway, visAway, liveMaskAway);
    const CBTTessellationStats sAway = inst.ReadTessellationStats();
    const uint32_t liveAway = sAway.LiveCount;
    std::printf("[falsify] after look-away: the descent's most-refined root %u went %u -> %u live "
                "(visible now: %s)\n",
                refinedRoot, refinedBefore, rootLiveAway[refinedRoot],
                ((visAway >> refinedRoot) & 1u) ? "VIS" : "invisible");
    EXPECT_EQ((visAway >> refinedRoot) & 1u, 0u) << "the refined face is behind the horizon now";
    EXPECT_LT(rootLiveAway[refinedRoot], refinedBefore / 10u)
        << "a horizon-invisible refined face merges cleanly to base — NOT stranded (no deadlock)";

    // (c) Consequently the detector's interior-invisible free set reclaims almost nothing.
    FarFieldReseedDetector detector(kSphereRootCount, FarFieldReseedConfig{1u, 1u, 0.80f});
    detector.Update(true, static_cast<float>(liveAway) / kDefaultBisectorPoolSize, visAway,
                    liveMaskAway, 0u, frame);
    const uint32_t freeSet = detector.ComputeReseedSet();
    uint32_t reclaimable = 0u;
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        if ((freeSet >> r) & 1u)
            reclaimable += rootLiveAway[r];
    std::printf("[falsify] detector interior-invisible freeSet=0x%06x reclaims only %u bisectors "
                "(of %u live) — the cure has nothing to drain\n",
                freeSet, reclaimable, liveAway);
    EXPECT_LT(reclaimable, kDefaultBisectorPoolSize / 100u)
        << "the invisible interior is already coarse; the reseed reclaims a trivial share of the pool";
}

// ============================================================================
// Detector decision logic (pure CPU — no device). Fires only when armed; edit non-interference;
// hysteresis + cooldown. Slice 1's telemetry logic, locked independent of the GPU kernels.
// ============================================================================
TEST(FarFieldReseedDetectorTest, FiresAtStallSilentWhenUnsaturated)
{
    FarFieldReseedDetector detector(kSphereRootCount, FarFieldReseedConfig{2u, 1u, 0.85f});
    const uint32_t liveMask = 0x00FFFFFFu;
    const uint32_t visMask = 0x000000FFu; // only the low 8 roots visible

    for (uint32_t f = 0; f < 10; ++f)
        detector.Update(/*mergeStalled=*/true, /*occ=*/0.50f, visMask, liveMask, 0u, f);
    EXPECT_FALSE(detector.Armed());
    EXPECT_EQ(detector.ComputeReseedSet(), 0u) << "an unsaturated pool must never reseed";

    FarFieldReseedDetector armed(kSphereRootCount, FarFieldReseedConfig{2u, 1u, 0.85f});
    armed.Update(true, 0.91f, visMask, liveMask, 0u, 0);
    EXPECT_TRUE(armed.Armed());
    EXPECT_EQ(armed.ComputeReseedSet(), 0u) << "hysteresis: not yet invisible for N frames";
    armed.Update(true, 0.91f, visMask, liveMask, 0u, 1);
    const uint32_t freeSet = armed.ComputeReseedSet();
    EXPECT_NE(freeSet, 0u) << "after N invisible frames the interior must be eligible";
    EXPECT_EQ(freeSet & visMask, 0u) << "no visible root may be freed";
}

TEST(FarFieldReseedDetectorTest, EditNonInterferenceNeverFreesEditedFace)
{
    const uint32_t liveMask = 0x00FFFFFFu;
    const uint32_t visMask = 0x0000000Fu; // only face 0's 4 roots visible

    FarFieldReseedDetector detector(kSphereRootCount, FarFieldReseedConfig{1u, 1u, 0.85f});
    detector.Update(true, 0.92f, visMask, liveMask, /*editFaceMask=*/0u, 0);
    const uint32_t noEdit = detector.ComputeReseedSet();
    ASSERT_NE(noEdit, 0u);

    FarFieldReseedDetector edited(kSphereRootCount, FarFieldReseedConfig{1u, 1u, 0.85f});
    edited.Update(true, 0.92f, visMask, liveMask, /*editFaceMask=*/(1u << 5), 0);
    const uint32_t withEdit = edited.ComputeReseedSet();
    const uint32_t face5Roots = 0x00F00000u; // roots 20..23
    EXPECT_EQ(withEdit & face5Roots, 0u) << "an edited face's roots must never be reseeded";
    EXPECT_NE(noEdit & face5Roots, 0u) << "control: without the edit those roots WERE eligible";
}

TEST(FarFieldReseedDetectorTest, HysteresisAndReseedCooldown)
{
    FarFieldReseedConfig cfg{3u, 5u, 0.85f};
    FarFieldReseedDetector detector(kSphereRootCount, cfg);
    const uint32_t liveMask = 0x00FFFFFFu;
    const uint32_t visMask = 0x000000FFu;

    detector.Update(true, 0.91f, visMask, liveMask, 0u, 0);
    EXPECT_EQ(detector.ComputeReseedSet(), 0u); // 1 frame < 3
    detector.Update(true, 0.91f, visMask, liveMask, 0u, 1);
    EXPECT_EQ(detector.ComputeReseedSet(), 0u); // 2 frames < 3
    detector.Update(true, 0.91f, visMask, liveMask, 0u, 2);
    const uint32_t freeSet = detector.ComputeReseedSet();
    EXPECT_NE(freeSet, 0u); // 3 frames >= 3

    detector.NotifyReseeded(freeSet, 2);
    detector.Update(true, 0.91f, visMask, liveMask, 0u, 3);
    EXPECT_EQ(detector.ComputeReseedSet(), 0u) << "rate-limit: cooldown active";
    detector.Update(true, 0.91f, visMask, liveMask, 0u, 6);
    EXPECT_EQ(detector.ComputeReseedSet(), 0u) << "still within the cooldown window";

    FarFieldReseedDetector glance(kSphereRootCount, cfg);
    glance.Update(true, 0.91f, visMask, liveMask, 0u, 0);
    glance.Update(true, 0.91f, visMask, liveMask, 0u, 1);
    glance.Update(true, 0.91f, 0x00FFFFFFu, liveMask, 0u, 2); // a full look-around this frame
    glance.Update(true, 0.91f, visMask, liveMask, 0u, 3);
    EXPECT_EQ(glance.ComputeReseedSet(), 0u) << "a look-away resets hysteresis; no premature free";
}
