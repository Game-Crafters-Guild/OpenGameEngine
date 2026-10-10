// Far-field drain probes (clipmap/bisector-pool arc, round-9 far-field-drain design).
//
// THE PREMISE THESE PROBES WERE WRITTEN TO TEST HAS BEEN FALSIFIED TWICE, and they are kept because
// their own measurements are half of what falsified it. PR #620 root-caused the static-camera
// "updates only happen when the camera moves" symptom as a saturation-induced LEB merge deadlock: at
// a saturated pose ~66% of the pool stranded in invisible far-field detail that cannot coarsen
// because the distance-graded screen-space metric leaves a STAIRCASED tree with almost no
// uniform-depth mergeable diamonds. Neither half survived measurement. There is no stranded far
// field, and the staircase is not why merges fail: a rejection census reconciled against
// mergeDemand shows candidates dying at the pair-STATE test, two tests before the facing diamond,
// because Classify decides merge PER FACET on a quantity the LEB diamond's members do not share
// (far-field-drain-design.html §1, v1.1). These probes are SPHERICAL, where that is still the
// projected-area metric; the planar domain now decides on the parent's split edge (§7).
// Read what follows as measurements, not as evidence for a deadlock. The near-field force-split
// gate (#620) keeps the VISIBLE field responsive.
//
// These probes MEASURE the load-bearing claims the far-field-drain design rests on, on the REAL GPU
// kernels at her scene (R=50000, relief 1250, alt 4000):
//
//   Probe 1 (rounds-to-drain): hold the camera DEAD STILL at a coarsen-everything TargetPixelError and
//     run the update many rounds. Does the far field drain if given unlimited static rounds, or does it
//     stall (mergeServed -> 0 with the pool still full)? A/B the near-field gate. This is the decisive
//     test for candidate 1 (multi-round drain pass): if it drains cheaply, the simplest cure wins.
//
//   Probe 2 (tier-ordering census): one-shot structural census of the saturated tree by depth tier —
//     live count, same-depth-twin pairs, and fully-legal merge diamonds (facing quad uniform + all
//     wanting to coarsen) per tier. Proves WHERE the mergeable structure is (or isn't): whether a
//     deepest-first ordering (candidate 2) has legal diamonds to order at the deepest tier, or the
//     staircase is so irregular that no tier has any.
//
//   Probe 3 (bulk-reseed conformity + re-entry): the degenerate bulk-free — reseed the whole tree to
//     base after saturation (InitializeRoots) — must be conforming (Validate green), and re-entry
//     (reconverging the visible field from base under the look-back priority) must be bounded. Bounds
//     candidate 3 (region invalidate-and-reseed): the conformity floor and the rebuild cost.
//
// These EXIST to print measured numbers; the assertions are deliberately loose and lock only the load-
// bearing shape so the design's gates can be diffed against real GPU state.

#include <gtest/gtest.h>

#include <algorithm>
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
constexpr uint32_t kSphereBaseDepthLocal = 5u; // spherical base depth (mirror InitializeRoots)

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
} // namespace

// Self-contained probe fixture (mirrors CBTDemandAuditTest / CBTSaturationRetess bring-up). Owns the
// headless device + kernel set; each probe builds its own CBTInstance so runs never share GPU state.
class CBTFarFieldDrainProbe : public ::testing::Test
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

    // Real conformity signal (link-reciprocity + budget + zombie invariants) for the last update:
    // RecordUpdate dispatches the whole-pool Validate kernel (m_ValidateEachUpdate defaults on), and
    // the accessor does its own readback. Call after a RunFrame.
    uint32_t ValidationErrors(CBTInstance& inst) { return inst.ReadValidationErrorCount(); }

    std::vector<uint64_t> ReadHeap(CBTInstance& inst)
    {
        const auto w = inst.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
        std::vector<uint64_t> h(kDefaultBisectorPoolSize);
        for (uint32_t i = 0; i < kDefaultBisectorPoolSize; ++i)
            h[i] = static_cast<uint64_t>(w[i * 2u]) | (static_cast<uint64_t>(w[i * 2u + 1u]) << 32);
        return h;
    }

    // Her scene shape with the editor's shipped demand shaping (near-bias + demand-tuning + priority).
    // Copied from CBTSaturationRetess::BuildHerParams so the probe drives the EXACT production demand
    // model. altM = camera altitude above the surface; oblique look across the sub-camera point.
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
        p.TerrainOrigin[2] = static_cast<float>(kSphereBaseDepthLocal + kMaxDecodeSubdiv); // maxDepth cap
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

    // Descend from orbit to restAltM at TargetPixelError = splitPx. The abrupt descent leaves the same
    // saturated-and-staircased tree her smooth navigation does — the state from which the far field must
    // drain. c.NearFieldGate selects the A/B branch. Returns the next frame index.
    uint32_t DescendToSaturation(CBTInstance& inst, CBTClassifyDesc& c, float radius, float restAltM,
                                 float splitPx, const SphereSculptGeometry& geom)
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
                RunFrame(inst, c, p, frame);
                ++frame;
            }
        }
        return frame;
    }

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// ============================================================================
// PROBE 1 — rounds-to-drain. Saturate at her scene, then hold the camera DEAD STILL at a coarsen-
// everything TargetPixelError and run the update many rounds. Log the drain curve (occupancy + merge
// demand/served + overflow per round). The decisive question for candidate 1: does the far field drain
// if given unlimited static rounds, or does it stall with the pool still full and mergeServed pinned
// at ~0? A/B the near-field gate — gate OFF is #620's frozen baseline (should NOT drain), gate ON is
// current main (the open question).
// ============================================================================
TEST_F(CBTFarFieldDrainProbe, RoundsToDrainUnderStaticCoarsenDemand)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());
    constexpr uint32_t kDrainRounds = 200u;

    auto run = [&](uint32_t gate, const char* label) -> std::pair<double, uint32_t> {
        CBTInstance inst;
        if (!inst.Initialize(*m_Device, m_KernelSet) || !inst.InitializeRoots(kDomainSpherical))
        {
            ADD_FAILURE() << label << ": CBTInstance init failed";
            return {0.0, 0u};
        }
        const uint32_t cap = inst.GetBaseDepth() + kMaxDecodeSubdiv;
        CBTClassifyDesc c{};
        c.Mode = kClassifyScreenSpace;
        c.TargetDepth = cap;
        c.NearFieldGate = gate;
        uint32_t frame = DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);

        // Coarsen-everything hold: a huge merge threshold so every visible facet's edge/area is under
        // the merge band (nothing wants to split; the whole tree wants to coarsen). SAME camera as the
        // descent's final pose — it never moves for the rest of the probe. This is #620's TPE=1000
        // condition made concrete.
        CBTFrameParams p;
        BuildHerParams(p, radius, 4000.0f, /*splitPx=*/2000.0f, /*mergePx=*/1000.0f, geom);

        const CBTTessellationStats s0 = inst.ReadTessellationStats();
        const double occ0 = static_cast<double>(s0.LiveCount) / s0.PoolSize;
        std::printf("\n[drain:%s] saturated start: live=%u occ=%.4f (pool=%u)\n", label, s0.LiveCount,
                    occ0, s0.PoolSize);
        std::printf("[drain:%s] round   live      occ   mergeDemand mergeServed splitDemand overflowTot\n",
                    label);

        uint32_t prevLive = s0.LiveCount;
        uint32_t stallStreak = 0;
        uint32_t roundsToFloor = kDrainRounds;
        for (uint32_t r = 0; r < kDrainRounds; ++r, ++frame)
        {
            c.GateVertexEval = 1u;
            RunFrame(inst, c, p, frame);
            const CBTTessellationStats s = inst.ReadTessellationStats();
            const double occ = static_cast<double>(s.LiveCount) / s.PoolSize;
            if (r < 12u || r % 20u == 0u || r == kDrainRounds - 1u)
                std::printf("[drain:%s] %4u %9u %8.4f %11d %11d %11d %11d\n", label, r, s.LiveCount, occ,
                            s.MergeDemand, s.MergeServed, s.SplitDemand, s.OverflowTotal);
            // Floor detection: live count stopped moving (< 0.1% change) for 8 consecutive rounds.
            const uint32_t delta = prevLive > s.LiveCount ? prevLive - s.LiveCount : s.LiveCount - prevLive;
            if (delta < prevLive / 1000u + 1u)
            {
                if (++stallStreak == 8u && roundsToFloor == kDrainRounds)
                    roundsToFloor = r - 7u;
            }
            else
            {
                stallStreak = 0;
            }
            prevLive = s.LiveCount;
        }

        const CBTTessellationStats sf = inst.ReadTessellationStats();
        const double occF = static_cast<double>(sf.LiveCount) / sf.PoolSize;
        std::printf("[drain:%s] FINAL after %u static rounds: live=%u occ=%.4f roundsToFloor=%u "
                    "(mergeServed=%d mergeDemand=%d)\n",
                    label, kDrainRounds, sf.LiveCount, occF, roundsToFloor, sf.MergeServed, sf.MergeDemand);
        EXPECT_EQ(ValidationErrors(inst), 0u) << label << ": tree must stay conforming while draining";
        return std::pair<double, uint32_t>{occF, roundsToFloor};
    };

    const auto off = run(0u, "gateOFF");
    const auto on = run(1u, "gateON");
    std::printf("\n[drain] SUMMARY  gateOFF finalOcc=%.4f (roundsToFloor=%u)   gateON finalOcc=%.4f "
                "(roundsToFloor=%u)\n\n",
                off.first, off.second, on.first, on.second);

    // Load-bearing shape (loose): the probe exists to PRINT the drain curve. Lock only that the run
    // completed and the far field's final occupancy is recorded for the design's gate. gateOFF is #620's
    // frozen baseline; gateON is the open question the printed curve answers.
    EXPECT_GT(off.first, 0.0);
    EXPECT_GT(on.first, 0.0);
}

// ============================================================================
// PROBE 2 — tier-ordering census. One-shot structural census of the saturated tree by depth tier. For
// each depth: live count, count of live bisectors whose twin (neighbor.z) is at the SAME depth (a
// necessary precondition for a merge diamond), and count of fully-legal diamonds (the current bisector
// is the lower-heapID of a same-depth pair whose facing twins are also same-depth — the exact
// PrepareSimplify eligibility, minus the SIMPLIFY-state check which is demand, not structure). This
// answers candidate 2: does the deepest tier have legal diamonds to order deepest-first, or is the
// staircase so irregular that NO tier has any?
// ============================================================================
TEST_F(CBTFarFieldDrainProbe, LegalDiamondCensusByDepthTier)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    c.NearFieldGate = 1u; // current main
    DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);

    // Read heap + the CURRENT neighbor buffer. After RecordUpdate the ping-pong parity flipped, so the
    // buffer holding the frame's final topology is selected by GetResources().GetNeighborsReadIsA().
    const std::vector<uint64_t> heap = ReadHeap(inst);
    const bool readIsA = inst.GetNeighborsReadIsA();
    const auto nWords = inst.DebugReadWords(
        readIsA ? CBTBinding::NeighborsA : CBTBinding::NeighborsB, kDefaultBisectorPoolSize * 4u);
    auto neighborZ = [&](uint32_t slot) { return nWords[slot * 4u + 2u]; }; // .z = twin edge
    auto neighborX = [&](uint32_t slot) { return nWords[slot * 4u + 0u]; };
    auto neighborY = [&](uint32_t slot) { return nWords[slot * 4u + 1u]; };

    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr uint32_t kMaxTier = 40u;
    std::array<uint64_t, kMaxTier> liveByTier{};
    std::array<uint64_t, kMaxTier> sameDepthTwin{};
    std::array<uint64_t, kMaxTier> legalDiamond{};
    uint64_t live = 0;

    for (uint32_t s = 0; s < kDefaultBisectorPoolSize; ++s)
    {
        const uint64_t h = heap[s];
        if (h == 0u)
            continue;
        ++live;
        const uint32_t depth = HeapDepth(h);
        if (depth >= kMaxTier)
            continue;
        ++liveByTier[depth];

        const uint32_t twin = neighborZ(s);
        if (twin == kInvalid || twin >= kDefaultBisectorPoolSize || heap[twin] == 0u)
            continue;
        if (HeapDepth(heap[twin]) != depth)
            continue;
        ++sameDepthTwin[depth];

        // Legal-diamond precondition (structural half of PrepareSimplify): currentID is the lower-heapID
        // of the pair (cN.x), the pair is same-depth, and the facing twins (pair.x = twinLow, cN.y =
        // twinHigh) are both same-depth. Count once per diamond (from its lower-heapID driver).
        const uint32_t pair = neighborX(s);
        if (pair == kInvalid || pair >= kDefaultBisectorPoolSize || heap[pair] == 0u)
            continue;
        if (HeapDepth(heap[pair]) != depth || h > heap[pair])
            continue; // the lower-id of the pair drives; skip the higher
        const uint32_t twinLow = neighborX(pair);
        const uint32_t twinHigh = neighborY(s);
        bool facingUniform = true;
        if (twinLow != kInvalid)
        {
            if (twinLow >= kDefaultBisectorPoolSize || twinHigh == kInvalid ||
                twinHigh >= kDefaultBisectorPoolSize || heap[twinLow] == 0u || heap[twinHigh] == 0u ||
                HeapDepth(heap[twinLow]) != depth || HeapDepth(heap[twinHigh]) != depth)
                facingUniform = false;
        }
        if (facingUniform)
            ++legalDiamond[depth];
    }

    std::printf("\n[census] saturated tree @ R=50000 alt=4000 (gate ON): live=%llu\n",
                (unsigned long long)live);
    std::printf("[census] tier   live   sameDepthTwin   legalDiamonds\n");
    uint32_t maxTier = 0, minTier = kMaxTier;
    uint64_t totalDiamonds = 0, deepestDiamonds = 0;
    for (uint32_t d = 0; d < kMaxTier; ++d)
    {
        if (liveByTier[d] == 0u)
            continue;
        maxTier = std::max(maxTier, d);
        minTier = std::min(minTier, d);
        totalDiamonds += legalDiamond[d];
        std::printf("[census] %4u %8llu %13llu %15llu\n", d, (unsigned long long)liveByTier[d],
                    (unsigned long long)sameDepthTwin[d], (unsigned long long)legalDiamond[d]);
    }
    deepestDiamonds = legalDiamond[maxTier];
    std::printf("[census] tier span [%u..%u] (%u levels of staircase); total legal diamonds=%llu; "
                "diamonds at deepest tier %u = %llu\n\n",
                minTier, maxTier, maxTier - minTier, (unsigned long long)totalDiamonds, maxTier,
                (unsigned long long)deepestDiamonds);

    // Load-bearing: the tree IS a multi-level staircase (that is the deadlock precondition), and the
    // census recorded diamond structure per tier for the design's tier-ordering analysis.
    EXPECT_GT(maxTier - minTier, 1u) << "the saturated tree must be a multi-level staircase";
    EXPECT_GT(live, kDefaultBisectorPoolSize / 2u) << "the tree must be saturated for this census";
}

// ============================================================================
// PROBE 3 — bulk-reseed conformity + re-entry cost. The degenerate bulk-free: after saturation, reseed
// the WHOLE tree back to base (InitializeRoots re-zeroes + re-seeds the roots — exactly the planet-
// switch path candidate 3 would invoke per-subtree). Assert (a) the reseeded base is conforming
// (Validate green, live == root count), and (b) re-entry — reconverging the visible field from base
// under the look-back priority — is bounded (reaches steady state in a small frame budget). This bounds
// candidate 3's conformity floor and rebuild cost. A per-SUBTREE reseed's boundary conformity needs a
// new kernel (out of scope for the design probe); this measures the whole-tree endpoints it interpolates.
// ============================================================================
TEST_F(CBTFarFieldDrainProbe, BulkReseedIsConformingAndReentryIsBounded)
{
    const float radius = 50000.0f;
    const auto geom = MakeSphereSculptGeometry(radius, ResolveSculptPagePoolCount());

    CBTInstance inst;
    ASSERT_TRUE(inst.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));
    const uint32_t cap = inst.GetBaseDepth() + kMaxDecodeSubdiv;
    const uint32_t rootCount = inst.GetRootCount();
    CBTClassifyDesc c{};
    c.Mode = kClassifyScreenSpace;
    c.TargetDepth = cap;
    c.NearFieldGate = 1u;
    uint32_t frame = DescendToSaturation(inst, c, radius, 4000.0f, 6.5f, geom);

    const CBTTessellationStats sSat = inst.ReadTessellationStats();
    std::printf("\n[reseed] saturated: live=%u occ=%.4f\n", sSat.LiveCount,
                static_cast<double>(sSat.LiveCount) / sSat.PoolSize);

    // BULK FREE (degenerate whole-tree case): re-seed to base. This is the planet-switch re-seed the
    // design's region invalidate-and-reseed generalizes to a subtree. Its graphics-token wait orders the
    // re-zero after the frames above, so no in-flight work races it.
    ASSERT_TRUE(inst.InitializeRoots(kDomainSpherical));

    // One inert-ish frame at the saturated pose to let Validate run over the reseeded base + repopulate
    // the counters (InitializeRoots itself does not run the update kernels).
    CBTFrameParams p;
    BuildHerParams(p, radius, 4000.0f, 6.5f, 3.25f, geom);
    c.GateVertexEval = 0u;
    RunFrame(inst, c, p, frame++);
    const CBTTessellationStats sBase = inst.ReadTessellationStats();
    std::printf("[reseed] after bulk reseed + 1 frame: live=%u (rootCount=%u) validationErrors=%u\n",
                sBase.LiveCount, rootCount, ValidationErrors(inst));
    // Conformity floor: the bulk-freed base mesh is conforming. (The reseed drops to roots; one screen-
    // space frame may already begin re-refining the near field, so bound live modestly above roots.)
    EXPECT_EQ(ValidationErrors(inst), 0u) << "the bulk-reseeded base must be conforming";
    EXPECT_LT(sBase.LiveCount, kDefaultBisectorPoolSize / 4u)
        << "the bulk reseed must actually free the pool (drop far below the saturated live count)";

    // RE-ENTRY: reconverge from base at the SAME static pose under the look-back priority. Measure the
    // rounds to reach steady state (live count stops climbing) — the rebuild cost bound candidate 3 pays
    // when a coarsened region comes back into view.
    uint32_t prevLive = sBase.LiveCount;
    uint32_t settleRound = 0;
    uint32_t stallStreak = 0;
    for (uint32_t r = 0; r < 90u; ++r, ++frame)
    {
        c.GateVertexEval = 1u;
        RunFrame(inst, c, p, frame);
        const CBTTessellationStats s = inst.ReadTessellationStats();
        const uint32_t delta = s.LiveCount > prevLive ? s.LiveCount - prevLive : prevLive - s.LiveCount;
        if (r < 8u || r % 15u == 0u)
            std::printf("[reseed] reentry round %2u: live=%u occ=%.4f overflow=%d\n", r, s.LiveCount,
                        static_cast<double>(s.LiveCount) / s.PoolSize, s.OverflowTotal);
        if (delta < prevLive / 200u + 1u)
        {
            if (++stallStreak == 5u && settleRound == 0u)
                settleRound = r - 4u;
        }
        else
        {
            stallStreak = 0;
        }
        prevLive = s.LiveCount;
    }
    std::printf("[reseed] re-entry settled at round %u (from base to steady-state visible field)\n\n",
                settleRound);
    EXPECT_EQ(ValidationErrors(inst), 0u) << "the re-entered tree must stay conforming";
    EXPECT_GT(prevLive, sBase.LiveCount) << "re-entry must rebuild detail from base under the static pose";
}
