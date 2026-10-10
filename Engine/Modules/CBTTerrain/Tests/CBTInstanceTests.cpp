// End-to-end on a real GPU: root init + the full update sequence, reading back
// the indirect-draw count, the sum-tree root, and the three Validate counters.
// C2 lands the complete fan-out (four-pattern Bisect, chain-walk Split, sum-tree
// free-slot Allocate, three-kernel merge path) on a valid 2-triangle conforming
// base, so these tests exercise refinement, double/triple splits, merge/refine
// cycles with slot reuse, teleport reconvergence, and the FreeCount budget
// invariant — with Validate green throughout.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "Rendering/Core/CommandList.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

namespace
{
// Two live root bisectors -> 3 indices per triangle.
constexpr uint32_t kRootIndexCount = kRootHalfedgeCount * 3u; // 6
// S2a 96B CBTVertexData (sector tail appended; corner/meta word offsets unchanged).
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u; // 24

uint32_t RunUpdate(IDevice& device, CBTInstance& instance, const CBTClassifyDesc& classify,
                   uint32_t frameIndex, const CBTFrameParams& params = CBTIdentityFrameParams())
{
    // Depth-target metric drives these tests; identity terrain params map the unit
    // square to world XZ 1:1 at Y=0 (the default flat height texture is bound), so
    // VertexEval reproduces the unit-square corners in the world XZ plane.
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordUpdate(*cl, classify, params, frameIndex);
    instance.RecordReadback(*cl);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return instance.ReadDrawIndexCount(kDrawStreamAll);
}

// The sum-tree root = live-bisector occupancy popcount (heap node 0).
uint32_t ReadSumTreeRoot(CBTInstance& instance)
{
    const uint32_t rootWord = CBTSumTreeHeapBase(kDefaultBisectorPoolSize);
    const auto words = instance.DebugReadWords(CBTBinding::SumTree, 1u, rootWord);
    return words.front();
}

// Every validation invariant must be clean.
void ExpectValidateGreen(CBTInstance& instance, const char* where)
{
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u)
        << "link reciprocity broke: " << where;
    EXPECT_EQ(instance.ReadValidationCounter(kValidationBudgetCounter), 0u)
        << "FreeCount went negative: " << where;
    EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u)
        << "bitfield/HeapID zombie: " << where;
    EXPECT_EQ(instance.ReadValidationCounter(kValidationCompactCounter), 0u)
        << "IndicesAll is not exactly the live set: " << where;
}

CBTClassifyDesc Refine(uint32_t focusRoot, uint32_t targetDepth)
{
    return CBTClassifyDesc{kClassifyDepthTarget, focusRoot, targetDepth};
}
} // namespace

class CBTInstanceTest : public ::testing::Test
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

    std::unique_ptr<IDevice> m_Device;
    CBTKernelSet m_KernelSet;
};

// In-place re-provision safety at the instance level: RefreshTerrainSources retires the graphics
// work already submitted and drops the retired heightmap from every ring element, advancing the
// source generation — after a live update bound the heightmap into the ring. This is the
// deterministic, timing-independent replacement for the per-slot self-heal a SamplesPerMeter edit
// could lose. That it takes no device drain is pinned separately (CBTBringUpDrainTests).
TEST_F(CBTInstanceTest, RefreshTerrainSourcesDropsRetiredHeightmapAfterUpdate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = "ReproInstanceHeightmap";
    const TextureHandle retired = m_Device->CreateTexture(td);
    ASSERT_TRUE(retired.IsValid());

    // Bind the heightmap into the ring the way BuildFrameParams does, then run an update so the
    // descriptor set is genuinely referenced by executed GPU work before the drop.
    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        instance.SetHeightSource(slot, retired);
    RunUpdate(*m_Device, instance, CBTInertClassify(), 0u);
    ASSERT_EQ(instance.CountTerrainSourceSlots(retired), kCBTFrameParamsRing);

    const uint64_t genBefore = instance.GetTerrainSourceGeneration();
    EXPECT_TRUE(instance.RefreshTerrainSources());
    EXPECT_EQ(instance.CountTerrainSourceSlots(retired), 0u)
        << "no ring element may reference the retired heightmap after the refresh";
    EXPECT_GT(instance.GetTerrainSourceGeneration(), genBefore);

    // The ring stays usable: a subsequent bind + update runs clean (Validate green).
    instance.SetHeightSource(1u, retired);
    RunUpdate(*m_Device, instance, CBTInertClassify(), 1u);
    ExpectValidateGreen(instance, "post-refresh update");

    m_Device->DestroyTexture(retired);
}

TEST_F(CBTInstanceTest, RootInitProducesTwoBisectors)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    // Root upload: HeapID[i] = 2^baseDepth + i = {2, 3} (each u64 = two u32 words
    // {value, 0}); the rest free (0).
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, 4);
    EXPECT_EQ(heap[0], 2u);
    EXPECT_EQ(heap[2], 3u);

    // Inert update: the 2 root bisectors survive, 6 indices, Validate green.
    const uint32_t indexCount = RunUpdate(*m_Device, instance, CBTInertClassify(), 0u);
    EXPECT_EQ(indexCount, kRootIndexCount);
    ExpectValidateGreen(instance, "root init inert update");
}

TEST_F(CBTInstanceTest, InertUpdateIsStableAcrossFrames)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    for (uint32_t frame = 0; frame < 3u; ++frame)
    {
        const uint32_t indexCount = RunUpdate(*m_Device, instance, CBTInertClassify(), frame);
        EXPECT_EQ(indexCount, kRootIndexCount) << "inert update drifted at frame " << frame;
        ExpectValidateGreen(instance, "inert frame");
        EXPECT_EQ(ReadSumTreeRoot(instance), kRootHalfedgeCount)
            << "sum-tree root != live count at frame " << frame;
    }
}

// Drives the split path: refine root 0's subtree to a target depth. The chain
// walk pulls the twin (root 1) along for conformance, and refinement converges
// to a stable, deterministic count with reciprocal links (Validate green).
TEST_F(CBTInstanceTest, SplitRefinementConvergesDeterministically)
{
    auto runSequence = [&](uint32_t& outSumRoot) -> uint32_t {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots());
        uint32_t last = 0;
        for (uint32_t frame = 0; frame < 6u; ++frame)
        {
            last = RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 3u), frame);
            ExpectValidateGreen(instance, "refinement frame");
        }
        outSumRoot = ReadSumTreeRoot(instance);
        return last;
    };

    uint32_t sumRootA = 0;
    uint32_t sumRootB = 0;
    const uint32_t countA = runSequence(sumRootA);
    const uint32_t countB = runSequence(sumRootB);
    EXPECT_EQ(countA, countB) << "split refinement is non-deterministic";
    EXPECT_GT(countA, kRootIndexCount) << "refinement did not grow past the roots";
    EXPECT_EQ(sumRootA * 3u, countA) << "sum-tree root disagrees with the draw stream";
    EXPECT_EQ(sumRootA, sumRootB);
}

// Asymmetric refinement (root 0 deep, root 1 coarse) forces a depth gradient at
// the shared twin edge, so Split emits RIGHT/LEFT_DOUBLE and Bisect exercises the
// 2/3-child paths. The per-pattern telemetry proves which variants fired; Validate
// stays green and the topology + per-pattern counts are deterministic across runs.
//
// Reachability on the 2-triangle base: refining one root's subtree across the
// single internal (twin) edge produces both RIGHT_DOUBLE and LEFT_DOUBLE (the
// coarser twin is subdivided along both axes to conform). TRIPLE requires a
// bisector whose BOTH leg-neighbors are simultaneously finer; the assertions
// below pin the observed reachability rather than forcing an unreachable variant.
TEST_F(CBTInstanceTest, ChainWalkExercisesMultiChildFanOut)
{
    struct FanOut
    {
        int32_t Right = 0;
        int32_t Left = 0;
        int32_t Triple = 0;
        uint32_t SumRoot = 0;
        uint32_t Count = 0;
    };
    auto runSequence = [&]() -> FanOut {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots());
        FanOut f;
        for (uint32_t frame = 0; frame < 7u; ++frame)
        {
            f.Count = RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 4u), frame);
            ExpectValidateGreen(instance, "chain-walk frame");
        }
        f.Right = instance.ReadWorkQueueCounter(kWQRightDoubleCounter);
        f.Left = instance.ReadWorkQueueCounter(kWQLeftDoubleCounter);
        f.Triple = instance.ReadWorkQueueCounter(kWQTripleCounter);
        f.SumRoot = ReadSumTreeRoot(instance);
        return f;
    };

    const FanOut a = runSequence();
    const FanOut b = runSequence();
    EXPECT_GT(a.Right + a.Left, 0) << "no RIGHT/LEFT_DOUBLE splits — the fan-out never fired";
    EXPECT_EQ(a.Right, b.Right) << "RIGHT_DOUBLE fan-out is non-deterministic";
    EXPECT_EQ(a.Left, b.Left) << "LEFT_DOUBLE fan-out is non-deterministic";
    EXPECT_EQ(a.Triple, b.Triple) << "TRIPLE fan-out is non-deterministic";
    EXPECT_EQ(a.Count, b.Count) << "chain-walk refinement is non-deterministic";
    EXPECT_EQ(a.SumRoot * 3u, a.Count) << "sum-tree root disagrees with the draw stream";
}

// VertexEval must produce correct conforming LEB geometry (latent-broken in C1 —
// Classify only used `area >= 0`, never the corners). After one split of root 0,
// its two children (heapIDs 2*root and 2*root+1) must share the split-edge apex
// (0,0) and midpoint (0.5,0.5) and differ in the third corner.
TEST_F(CBTInstanceTest, VertexEvalProducesConformingLebGeometry)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    // Frame 0 splits root 0 (+ root 1 via conformance) to depth 2; frame 1 leaves
    // it stable (root 1's would-be merge aborts against root 0's finer twins) and
    // recomputes the depth-2 corners into CurrentVertex.
    const CBTClassifyDesc refine = Refine(0u, kDefaultBaseDepth + 1u);
    RunUpdate(*m_Device, instance, refine, 0u);
    RunUpdate(*m_Device, instance, refine, 1u);
    ExpectValidateGreen(instance, "vertex-eval");

    // Root 0 = slot 0 (heapID 2 -> 4 in place); its child (heapID 5) is at slot 2
    // or 3 (decode order). Locate it.
    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, 8);
    ASSERT_EQ(heap[0], 4u) << "root 0 did not promote in place to heapID 4";
    const uint32_t slot5 = (heap[4] == 5u) ? 2u : ((heap[6] == 5u) ? 3u : 0xFFFFFFFFu);
    ASSERT_NE(slot5, 0xFFFFFFFFu) << "child heapID 5 not found";

    auto readCorners = [&](uint32_t slot) {
        // The 3 corners are the first 12
        // words, each (worldX, height, worldZ, uv.x). With identity terrain params the
        // unit-square coord maps to world XZ 1:1 at Y=0, so read (x, z) as (u, v).
        const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 12u, slot * kVertexWordsPerSlot);
        std::array<std::array<float, 2>, 3> c{};
        for (uint32_t k = 0; k < 3u; ++k)
        {
            std::memcpy(&c[k][0], &w[k * 4u + 0u], sizeof(float)); // worldX == uv.x
            std::memcpy(&c[k][1], &w[k * 4u + 2u], sizeof(float)); // worldZ == uv.y
        }
        return c;
    };
    auto contains = [](const std::array<std::array<float, 2>, 3>& c, float x, float y) {
        for (const auto& p : c)
            if (std::abs(p[0] - x) < 1e-4f && std::abs(p[1] - y) < 1e-4f)
                return true;
        return false;
    };

    const auto c4 = readCorners(0u);      // heapID 4
    const auto c5 = readCorners(slot5);   // heapID 5
    // Shared split edge: apex (0,0) and bisection midpoint (0.5,0.5).
    EXPECT_TRUE(contains(c4, 0.0f, 0.0f) && contains(c4, 0.5f, 0.5f));
    EXPECT_TRUE(contains(c5, 0.0f, 0.0f) && contains(c5, 0.5f, 0.5f));
    // Distinct third corners (reference LEB split matrix convention: bit 0 keeps the
    // v2-side corner, bit 1 the v0-side): heapID 4 keeps (0,1), heapID 5 keeps (1,0).
    EXPECT_TRUE(contains(c4, 0.0f, 1.0f)) << "heapID 4 missing its (0,1) corner";
    EXPECT_TRUE(contains(c5, 1.0f, 0.0f)) << "heapID 5 missing its (1,0) corner";
    EXPECT_FALSE(contains(c4, 1.0f, 0.0f)) << "children are not distinct (identical matrices?)";
}

// The C3 indexed-indirect draw resources are filled once by InitializeRoots: the
// identity index buffer must read index[i] == i (so gl_VertexIndex walks the run)
// and the draw-count buffer must hold exactly 1 (draw the single ALL record).
TEST_F(CBTInstanceTest, DrawResourcesFilledIdentityIndexAndCount)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const BufferHandle ib = instance.GetResources().GetIdentityIndexBuffer();
    const BufferHandle count = instance.GetResources().GetDrawCountBuffer();
    ASSERT_TRUE(ib.IsValid());
    ASSERT_TRUE(count.IsValid());

    constexpr uint32_t kProbe = 8u; // first 8 index entries, then the count word
    const size_t bytes = static_cast<size_t>(kProbe + 1u) * sizeof(uint32_t);
    const BufferHandle readback = m_Device->CreateReadbackBuffer(bytes, "CBT.Test.DrawReadback");
    ASSERT_TRUE(readback.IsValid());

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(ib, ResourceState::IndexBuffer,
                                                     ResourceState::CopySource));
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(count, ResourceState::IndirectArgs,
                                                     ResourceState::CopySource));
    cl->CopyBuffer(ib, readback, static_cast<size_t>(kProbe) * sizeof(uint32_t), 0, 0);
    cl->CopyBuffer(count, readback, sizeof(uint32_t), 0,
                   static_cast<size_t>(kProbe) * sizeof(uint32_t));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitForIdle();

    const void* mapped = m_Device->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    const uint32_t* words = static_cast<const uint32_t*>(mapped);
    for (uint32_t i = 0; i < kProbe; ++i)
        EXPECT_EQ(words[i], i) << "identity index buffer entry " << i << " != i";
    EXPECT_EQ(words[kProbe], kDrawCountValue) << "draw-count buffer != 1";
    m_Device->UnmapBuffer(readback);
    m_Device->DestroyBuffer(readback);
}

// C3 focus-hole regression: bisectors CREATED this frame (Bisect) must have
// non-degenerate corners in the SAME frame the draw bins them. VertexEval runs
// after the split path now; before the fix it ran before Bisect, so this frame's
// new children carried zero/stale corners and rasterized as degenerate triangles
// (the square hole at the deepest-refinement focus). One split, checked same-frame.
TEST_F(CBTInstanceTest, NewChildrenHaveNonDegenerateVerticesSameFrame)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 1u), 0u);
    ExpectValidateGreen(instance, "same-frame vertex-eval");

    const auto heap = instance.DebugReadWords(CBTBinding::HeapID, 8);
    ASSERT_EQ(heap[0], 4u) << "root 0 did not promote in place to heapID 4";
    const uint32_t slot5 = (heap[4] == 5u) ? 2u : ((heap[6] == 5u) ? 3u : 0xFFFFFFFFu);
    ASSERT_NE(slot5, 0xFFFFFFFFu) << "child heapID 5 (created this frame) not found";

    auto triangleArea = [&](uint32_t slot) {
        // The 3 corners are the first 12 words of a slot, corner k = (worldX,
        // height, worldZ, uv.x). Identity params -> world XZ == unit-square, so the XZ
        // footprint area is the LEB area.
        const auto w = instance.DebugReadWords(CBTBinding::CurrentVertex, 12u, slot * kVertexWordsPerSlot);
        std::array<std::array<float, 2>, 3> c{};
        for (uint32_t k = 0; k < 3u; ++k)
        {
            std::memcpy(&c[k][0], &w[k * 4u + 0u], sizeof(float)); // worldX
            std::memcpy(&c[k][1], &w[k * 4u + 2u], sizeof(float)); // worldZ
        }
        return std::abs((c[1][0] - c[0][0]) * (c[2][1] - c[0][1]) -
                        (c[2][0] - c[0][0]) * (c[1][1] - c[0][1])) *
               0.5f;
    };
    // A degenerate (zero/stale-corner) triangle has ~zero area and vanishes.
    EXPECT_GT(triangleArea(0u), 1e-5f) << "heapID 4 degenerate same-frame";
    EXPECT_GT(triangleArea(slot5), 1e-5f)
        << "child heapID 5 (created this frame) has degenerate corners — the focus hole";
}

// Refine deep, then merge everything back to the roots, then re-refine. The
// merge path must return the tree toward the roots, the freed slots must be
// re-decoded (FreeCount recovers to pool − liveCount each frame), the pool must
// never overflow, and the re-refine must reproduce the original converged count
// — only possible if merged slots are cleanly reused (the sum-tree decode).
TEST_F(CBTInstanceTest, MergeRefineCycleRecoversBudgetAndReusesSlots)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const CBTClassifyDesc refine = Refine(0u, kDefaultBaseDepth + 3u);
    const CBTClassifyDesc mergeAll = CBTInertClassify(); // target = baseDepth over all roots

    uint32_t frame = 0;
    uint32_t refinedCount = 0;
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
    {
        refinedCount = RunUpdate(*m_Device, instance, refine, frame);
        ExpectValidateGreen(instance, "cycle refine");
    }
    EXPECT_GT(refinedCount, kRootIndexCount);

    // Merge everything back toward the roots.
    uint32_t mergedCount = refinedCount;
    for (uint32_t i = 0; i < 8u; ++i, ++frame)
    {
        mergedCount = RunUpdate(*m_Device, instance, mergeAll, frame);
        ExpectValidateGreen(instance, "cycle merge");
    }
    EXPECT_EQ(mergedCount, kRootIndexCount) << "merge did not collapse back to the roots";
    // FreeCount is recomputed from the sum tree each frame, so after the merge it
    // has recovered to the full free pool (freed slots returned to the budget).
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQFreeCount),
              static_cast<int32_t>(kDefaultBisectorPoolSize - kRootHalfedgeCount))
        << "FreeCount did not recover after merge — slots leaked";

    // Re-refine: must reproduce the converged count using reused slots, no overflow.
    uint32_t reRefinedCount = 0;
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
    {
        reRefinedCount = RunUpdate(*m_Device, instance, refine, frame);
        ExpectValidateGreen(instance, "cycle re-refine");
    }
    EXPECT_EQ(reRefinedCount, refinedCount)
        << "re-refinement after merge diverged — slot reuse is broken";
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQOverflowCounter), 0)
        << "pool overflowed during the merge/refine cycle";
}

// Converge on root 0, then teleport the focus to root 1. The old region must
// simplify and the new region refine; the count must re-stabilize (converged)
// and be deterministic across two runs, with Validate green throughout.
TEST_F(CBTInstanceTest, TeleportReconvergesDeterministically)
{
    auto runSequence = [&](uint32_t& outPrev, uint32_t& outLast) {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots());
        uint32_t frame = 0;
        for (uint32_t i = 0; i < 6u; ++i, ++frame)
        {
            RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 3u), frame);
            ExpectValidateGreen(instance, "teleport phase A");
        }
        // Teleport the focus to the other root.
        outPrev = 0;
        outLast = 0;
        for (uint32_t i = 0; i < 8u; ++i, ++frame)
        {
            outPrev = outLast;
            outLast = RunUpdate(*m_Device, instance, Refine(1u, kDefaultBaseDepth + 3u), frame);
            ExpectValidateGreen(instance, "teleport phase B");
        }
    };

    uint32_t prevA = 0, lastA = 0, prevB = 0, lastB = 0;
    runSequence(prevA, lastA);
    runSequence(prevB, lastB);
    EXPECT_EQ(prevA, lastA) << "did not reconverge within the frame budget after teleport";
    EXPECT_EQ(lastA, lastB) << "teleport reconvergence is non-deterministic";
    EXPECT_GT(lastA, kRootIndexCount);
}

// The FreeCount budget invariant (Validate budget counter) must hold under a
// deep, aggressive refinement that drives many chain-walk reservations.
TEST_F(CBTInstanceTest, BudgetInvariantHoldsUnderHeavyRefinement)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    for (uint32_t frame = 0; frame < 8u; ++frame)
    {
        RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 5u), frame);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationBudgetCounter), 0u)
            << "FreeCount went negative at frame " << frame;
        EXPECT_GE(instance.ReadWorkQueueCounter(kWQFreeCount), 0)
            << "FreeCount observed negative at frame " << frame;
        ExpectValidateGreen(instance, "heavy refine");
    }
}

// Soak: hundreds of update frames with a MOVING focus (alternating root + swept
// target depth, including the deep regime where the editor TDR'd). A compute-side
// race that corrupts heap IDs surfaces as a Validate failure — the depth-band
// check added to Validate catches the exact malformation that underflowed
// VertexEval's decode loop. Because every kernel loop is now bounded, corruption
// degrades into a failed assert here instead of hanging the GPU (device lost).
// Best-effort headless reproduction of the intermittent editor hang; the bounded
// loops are the guarantee regardless of whether it reproduces on this device.
TEST_F(CBTInstanceTest, SoakMovingFocusStaysValid)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    constexpr uint32_t kFrames = 300u;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        const uint32_t focus = frame % 2u;                        // teleport every frame
        const uint32_t target = kDefaultBaseDepth + 2u + (frame % 6u); // depth 3..8
        RunUpdate(*m_Device, instance, Refine(focus, target), frame);
        // Every frame: link reciprocity + budget + zombie/depth-band corruption.
        ExpectValidateGreen(instance, "soak");
    }
}

// Corruption oracle for the 3-lane work-queue aliasing (slot-diet round 2). The six logical work
// queues share THREE physical payload lanes (LANE 0 Split->PropagateBisect->Simplify, LANE 1
// SimplifyClass->PropagateSimplify, LANE 2 Allocate); the aliasing is safe only because each lane's
// queues have pairwise-disjoint live spans over the per-frame kernel sequence (the interval-graph
// 3-colouring proven at the CBTLayout.h definition). A MIS-colouring — two concurrently-live queues
// forced onto one lane — clobbers the split or merge payload and mis-drives Bisect/Simplify into
// wrong-slot topology edits: a Validate link-reciprocity / zombie / budget failure.
//
// This drives SATURATED simultaneous split+merge churn. Each teleport of the refine focus makes the
// abandoned root's deep subtree COLLAPSE to base (flooding the merge path: SimplifyClass -> Simplify
// -> PropagateSimplify) while the new focus CLIMBS deep (flooding the split path: Split -> Allocate
// -> Bisect -> PropagateBisect) in the SAME frame — so all three lanes hold live payload at once,
// including the max-clique-3 moment where Split writes Allocate (LANE 2) while SimplifyClass (LANE 1)
// is still held for PrepareSimplify. Validate ON every frame asserts all three counters stay 0; the
// per-frame draw counts must also be deterministic across two fresh runs (a subtle corruption that
// slipped past Validate would still diverge the live-bisector count, which is order-independent here
// because the 2-root base at this depth is far from pool saturation). VERIFIED can-fail: temporarily
// aliasing LANE 2 onto LANE 1 (Allocate overlapping the held SimplifyClass span) turns this red.
TEST_F(CBTInstanceTest, WorkQueueLaneAliasingChurnStaysValid)
{
    auto runChurn = [&](std::vector<uint32_t>& counts) {
        CBTInstance instance;
        EXPECT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        EXPECT_TRUE(instance.InitializeRoots());
        constexpr uint32_t kCycles = 16u;
        constexpr uint32_t kFramesPerCycle = 5u;
        constexpr uint32_t kDeep = kDefaultBaseDepth + 7u; // hundreds of bisectors in flight per root
        uint32_t peakPropagateBisect = 0;
        uint32_t peakPropagateSimplify = 0;
        static_assert(kWQPropagateSimplifyCounter == kWQPropagateBisectCounter + 1u);
        uint32_t frame = 0;
        for (uint32_t cycle = 0; cycle < kCycles; ++cycle)
        {
            const uint32_t focus = cycle & 1u; // teleport each cycle: old focus merges, new focus splits
            for (uint32_t i = 0; i < kFramesPerCycle; ++i, ++frame)
            {
                counts.push_back(RunUpdate(*m_Device, instance, Refine(focus, kDeep), frame));
                ExpectValidateGreen(instance, "work-queue lane aliasing churn");
                // These counters survive the indirect dispatches until the next Reset. A green
                // topology oracle only covers propagation if both queues actually contained work.
                const auto propagation = instance.DebugReadWords(
                    CBTBinding::WorkQueue, 2u, kWQPropagateBisectCounter);
                ASSERT_EQ(propagation.size(), 2u);
                EXPECT_LE(propagation[0], kDefaultBisectorPoolSize);
                EXPECT_LE(propagation[1], kDefaultBisectorPoolSize);
                peakPropagateBisect = std::max(peakPropagateBisect, propagation[0]);
                peakPropagateSimplify = std::max(peakPropagateSimplify, propagation[1]);
            }
        }
        EXPECT_GT(peakPropagateBisect, 0u) << "churn did not exercise PropagateBisect";
        EXPECT_GT(peakPropagateSimplify, 0u) << "churn did not exercise PropagateSimplify";
    };

    std::vector<uint32_t> a;
    std::vector<uint32_t> b;
    runChurn(a);
    runChurn(b);
    EXPECT_EQ(a, b) << "saturated split/merge churn diverged — a work-queue lane aliased two live spans";
    ASSERT_FALSE(a.empty());
    EXPECT_GT(*std::max_element(a.begin(), a.end()), kRootIndexCount)
        << "churn never refined past the roots — the split path never populated the queues";
}

namespace
{
// The pool's own dispatch ceiling: no CBT kernel can ever need more threads than pool
// slots, so ceil(P / WG) groups bounds every legitimate indirect dispatch (mirror of the
// GLSL CBT_MAX_DISPATCH_GROUPS derivation).
constexpr uint32_t kMaxDispatchGroups =
    (kDefaultBisectorPoolSize + kComputeWorkgroupSize - 1u) / kComputeWorkgroupSize;

// Overwrite one u32 word of a CBT storage buffer from the CPU — the corruption
// injector for the dispatch-clamp tests. Same barrier discipline as the production
// zero-fill (InitializeRoots fillWhole): UAV -> CopyDest -> fill -> back to UAV.
void PoisonWord(IDevice& device, BufferHandle buffer, uint32_t wordOffset, uint32_t value,
                uint32_t wordCount = 1u)
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::UnorderedAccess,
                                                     ResourceState::CopyDest));
    cl->FillBuffer(buffer, static_cast<size_t>(wordOffset) * sizeof(uint32_t),
                   static_cast<size_t>(wordCount) * sizeof(uint32_t),
                   value);
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::CopyDest,
                                                     ResourceState::UnorderedAccess));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

// Dispatch ONE named kernel exactly the way CBTInstance::DispatchDirect does, so the
// dispatch-arg writers can be driven against poisoned inputs WITHOUT the indirect
// dispatch that would consume the args. That containment is the point: the can-fail
// arm of the clamp tests (clamp removed) must fail on the arg READBACK, not by
// actually launching a 2^26-group dispatch on the test machine (a real TDR).
void DispatchSingleKernel(IDevice& device, CBTKernelSet& kernels, CBTInstance& instance,
                          CBTKernel kernel, const CBTPushConstants& pc)
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    const PipelineHandle pipe = kernels.GetPipeline(kernel);
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(0, instance.GetResources().GetDescriptorSet(), pipe);
    cl->SetPushConstants(pc);
    cl->Dispatch(1, 1, 1); // Reset / PrepareIndirect are singleton kernels
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

CBTPushConstants DefaultPlanarPush()
{
    CBTPushConstants pc{};
    pc.BaseDepth = kDefaultBaseDepth;
    pc.PoolSize = kDefaultBisectorPoolSize;
    pc.TotalElements = kDefaultBisectorPoolSize;
    pc.NeighborsReadIsA = 1u;
    return pc;
}
} // namespace

TEST_F(CBTInstanceTest, OverflowTelemetrySaturatesUnderConcurrentRollbacks)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    const auto queue = instance.GetResources().GetBuffer(CBTBinding::WorkQueue);
    // All 64 queued requests name root zero and fail their reservation. Start just below the
    // signed limit so one workgroup crosses it concurrently; another workgroup must stay capped.
    PoisonWord(*m_Device, queue, WQSplitQueueOffset(kDefaultBisectorPoolSize), 0u,
               kComputeWorkgroupSize);
    PoisonWord(*m_Device, queue, kWQSplitCounter, kComputeWorkgroupSize);
    PoisonWord(*m_Device, queue, kWQFreeCount, 0u);
    PoisonWord(*m_Device, queue, kWQOverflowCounter, 0x7FFFFFFEu);
    for (uint32_t pass = 0; pass < 2u; ++pass)
    {
        DispatchSingleKernel(*m_Device, m_KernelSet, instance, CBTKernel::Split,
                             DefaultPlanarPush());
        EXPECT_EQ(instance.ReadTessellationStats().OverflowTotal, 0x7FFFFFFF);
        EXPECT_EQ(instance.ReadWorkQueueCounter(kWQFreeCount), 0);
    }
}

// A restart refines the fresh roots to one uniform depth before the first draw: every leaf at that
// depth, the tree conforming, the sum tree counting exactly those leaves.
TEST_F(CBTInstanceTest, RefineUniformReachesOneDepthEverywhere)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    constexpr uint32_t kDepth = 9u;
    instance.RefineUniform(kDepth, 0u);
    EXPECT_EQ(ReadSumTreeRoot(instance), 1u << kDepth) << "two roots at depth 1 tile into 2^depth leaves";
    const CBTTreeAuditResult audit = AuditTree(ReadTreeSnapshot(instance, false));
    EXPECT_EQ(audit.LiveCount, 1u << kDepth);
    EXPECT_EQ(audit.MaxDepth, kDepth);
    EXPECT_TRUE(audit.Watertight());
}

// A live re-seed restarts the tree's history too: the counters a previous terrain left (the
// cumulative overflow total, the pressure and keep steps) read zero afterwards.
TEST_F(CBTInstanceTest, ReseedZeroesTheWorkQueueCounters)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    const auto queue = instance.GetResources().GetBuffer(CBTBinding::WorkQueue);
    for (const uint32_t slot : {kWQOverflowCounter, kWQPressureStep, kWQOffFrustumKeepStep})
        PoisonWord(*m_Device, queue, slot, 12345u);
    ASSERT_EQ(instance.ReadTessellationStats().OverflowTotal, 12345);
    ASSERT_TRUE(instance.InitializeRoots());
    const CBTTessellationStats stats = instance.ReadTessellationStats();
    EXPECT_EQ(stats.OverflowTotal, 0);
    EXPECT_EQ(stats.PressureStep, 0);
    EXPECT_EQ(stats.OffFrustumKeepStep, 0);
}

TEST_F(CBTInstanceTest, ReseedIgnoresPoisonedDeadPayloadAcrossDomains)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    uint32_t frame = 0;
    for (const uint32_t domain : {kDomainPlanar, kDomainSpherical, kDomainPlanar})
    {
        // Poison the root records AND plenty of not-yet-live slots, catching dependencies on
        // fresh allocations accidentally containing zeros. Guards remain intact until re-seed.
        for (const CBTBinding binding : {CBTBinding::NeighborsA, CBTBinding::NeighborsB,
                 CBTBinding::BisectorData, CBTBinding::CurrentVertex, CBTBinding::IndicesAll,
                 CBTBinding::IndicesVisible, CBTBinding::IndicesModified})
            PoisonWord(*m_Device, instance.GetResources().GetBuffer(binding), 0u, 0x7FC00000u,
                       2048u); // NaN floats and out-of-range pointers
        ASSERT_TRUE(instance.InitializeRoots(domain));
        // A dead vertex record is deliberately retained rather than clearing the whole pool.
        EXPECT_EQ(instance.DebugReadWords(CBTBinding::CurrentVertex, 1u, 1024u).front(),
                  0x7FC00000u);
        CBTFrameParams params = CBTIdentityFrameParams();
        params.PlanetParams[0] = 50.0f;
        const uint32_t base = instance.GetBaseDepth();
        auto classify = Refine(kFocusRootAll, base + 3u);
        classify.GateVertexEval = 1u;
        for (uint32_t i = 0; i < 6u; ++i)
            RunUpdate(*m_Device, instance, classify, frame++, params);
        ExpectValidateGreen(instance, "poisoned payload refinement");
        const uint32_t liveCount = instance.ReadDrawIndexCount(kDrawStreamAll) / 3u;
        ASSERT_GT(liveCount, instance.GetRootCount());
        const auto live = instance.DebugReadWords(CBTBinding::IndicesAll, liveCount);
        ASSERT_EQ(live.size(), liveCount);
        const uint32_t maxSlot = *std::max_element(live.begin(), live.end());
        const auto vertices = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                       (maxSlot + 1u) * kVertexWordsPerSlot);
        for (const uint32_t slot : live)
        {
            for (uint32_t corner = 0; corner < 3u; ++corner)
            {
                float xyz[3];
                std::memcpy(xyz, vertices.data() + slot * kVertexWordsPerSlot + corner * 4u,
                            sizeof(xyz));
                for (float coordinate : xyz)
                    EXPECT_TRUE(std::isfinite(coordinate));
                if (domain == kDomainSpherical)
                    EXPECT_NEAR(std::sqrt(xyz[0]*xyz[0] + xyz[1]*xyz[1] + xyz[2]*xyz[2]),
                                50.0f, 0.01f);
                else
                    EXPECT_EQ(xyz[1], 0.0f);
            }
        }
        classify.TargetDepth = base;
        for (uint32_t i = 0; i < 6u; ++i)
            RunUpdate(*m_Device, instance, classify, frame++, params);
        ExpectValidateGreen(instance, "poisoned payload merge");
        EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamAll), instance.GetRootCount() * 3u);
    }
}

TEST_F(CBTInstanceTest, RegionReseedRefreshesRootGeometryOnNextGatedUpdate)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    auto params = CBTIdentityFrameParams();
    params.PlanetParams[0] = 50.0f;
    auto classify = Refine(kFocusRootAll, kSphereBaseDepth);
    classify.GateVertexEval = 1u;
    RunUpdate(*m_Device, instance, classify, 0u, params);
    auto rootGeometry = [&]() {
        const auto heap = instance.DebugReadWords(CBTBinding::HeapID, kSphereRootCount * 2u);
        const auto vertices = instance.DebugReadWords(CBTBinding::CurrentVertex,
                                                       kSphereRootCount * kVertexWordsPerSlot);
        std::map<uint64_t, std::array<uint32_t, kVertexWordsPerSlot>> geometry;
        for (uint32_t slot = 0; slot < kSphereRootCount; ++slot)
        {
            const uint64_t id = uint64_t(heap[slot * 2u]) | (uint64_t(heap[slot * 2u + 1u]) << 32u);
            std::array<uint32_t, kVertexWordsPerSlot> corners;
            std::memcpy(corners.data(), vertices.data() + slot * kVertexWordsPerSlot,
                        sizeof(corners));
            geometry.emplace(id, corners);
        }
        return geometry;
    };
    const auto expected = rootGeometry();
    classify.TargetDepth = kSphereBaseDepth + 3u;
    for (uint32_t frame = 1; frame < 8u; ++frame)
        RunUpdate(*m_Device, instance, classify, frame, params);
    instance.RegionFreeToBase((1u << kSphereRootCount) - 1u);
    classify.TargetDepth = kSphereBaseDepth;
    RunUpdate(*m_Device, instance, classify, 8u, params);
    ExpectValidateGreen(instance, "gated region reseed");
    EXPECT_EQ(instance.ReadDrawIndexCount(kDrawStreamAll), kSphereRootCount * 3u);
    EXPECT_EQ(rootGeometry(), expected)
        << "reseeded roots retained refined corner payloads under the quiescence gate";
}

// One modified slot beyond the first workgroup of IndicesAll must still be evaluated by a
// single workgroup. This distinguishes compact modified dispatch from the old live-list scan:
// reducing its dispatch width alone would leave the high slot's stale geometry untouched.
TEST_F(CBTInstanceTest, GatedVertexEvalUsesCompactModifiedStream)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    const auto refine = Refine(kFocusRootAll, kDefaultBaseDepth + 7u);
    uint32_t indexCount = 0;
    for (uint32_t frame = 0; frame < 10u; ++frame)
        indexCount = RunUpdate(*m_Device, instance, refine, frame);
    ExpectValidateGreen(instance, "compact modified setup");

    const auto live = instance.DebugReadWords(CBTBinding::IndicesAll, indexCount / 3u);
    ASSERT_GT(live.size(), kComputeWorkgroupSize);
    const uint32_t target = live.back();
    const uint32_t vertexWord = target * kVertexWordsPerSlot + 1u; // corner0.y
    const uint32_t flagWord = target * (sizeof(CBTBisectorData) / sizeof(uint32_t)) +
        offsetof(CBTBisectorData, Flags) / sizeof(uint32_t);
    auto& resources = instance.GetResources();
    // Manufacture one valid dirty entry, with visibly stale height, while the live stream stays
    // unchanged. All other flags settled in the final same-target update above.
    PoisonWord(*m_Device, resources.GetBuffer(CBTBinding::BisectorData), flagWord,
               kFlagVisible | kFlagModified);
    PoisonWord(*m_Device, resources.GetBuffer(CBTBinding::CurrentVertex), vertexWord, 0x42F60000u); // 123
    PoisonWord(*m_Device, resources.GetBuffer(CBTBinding::IndicesModified), 0u, target);
    PoisonWord(*m_Device, resources.GetBuffer(CBTBinding::IndirectDraw),
               kDrawStreamModified * kDrawStreamStride, 3u);
    PoisonWord(*m_Device, resources.GetBuffer(CBTBinding::WorkQueue), kWQVertexEvalCounter, 0u);
    resources.UploadFrameParams(0u, CBTIdentityFrameParams());
    CBTPushConstants pc = DefaultPlanarPush();
    pc.GateVertexEval = 1u;
    DispatchSingleKernel(*m_Device, m_KernelSet, instance, CBTKernel::VertexEval, pc);

    const auto height = instance.DebugReadWords(CBTBinding::CurrentVertex, 1u, vertexWord);
    ASSERT_EQ(height.size(), 1u);
    EXPECT_EQ(height[0], 0u) << "the compact dirty entry retained its stale height";
    EXPECT_EQ(instance.ReadVertexEvalCount(), 1) << "only the compact dirty entry should run";
}

// The three reduce kernels, recorded as CBTInstance::RecordReduce records them, over the bitfield
// as it stands. The per-binding barrier between dispatches is the instance's data barrier.
void DispatchReduce(IDevice& device, CBTKernelSet& kernels, CBTInstance& instance)
{
    const BufferHandle sumTree = instance.GetResources().GetBuffer(CBTBinding::SumTree);
    const BufferHandle bitfield = instance.GetResources().GetBuffer(CBTBinding::Bitfield);
    const std::array<ResourceBarrier, 2> barriers = {
        ResourceBarrier::CreateBufferBarrier(sumTree, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess),
        ResourceBarrier::CreateBufferBarrier(bitfield, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess)};
    const CBTPushConstants pc = DefaultPlanarPush();
    const std::array<std::pair<CBTKernel, uint32_t>, 3> passes = {{
        {CBTKernel::ReducePrePass, CBTLeafPackedWords(kDefaultBisectorPoolSize) / kComputeWorkgroupSize},
        {CBTKernel::ReduceFirstPass, CBTLeafNodeCount(kDefaultBisectorPoolSize) / kComputeWorkgroupSize},
        {CBTKernel::ReduceSecondPass, 1u},
    }};
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    for (const auto& [kernel, groups] : passes)
    {
        const PipelineHandle pipe = kernels.GetPipeline(kernel);
        cl->SetPipeline(pipe);
        cl->BindDescriptorSet(0, instance.GetResources().GetDescriptorSet(), pipe);
        cl->SetPushConstants(pc);
        cl->Dispatch(groups, 1, 1);
        for (const ResourceBarrier& barrier : barriers)
            cl->Barrier(barrier);
    }
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

// The reduce rebuilds every node of the sum tree from the occupancy bitfield: each leaf is its
// word's popcount, each internal node the sum of its two children, across every leaf block and
// level the reduce splits between its dispatches. The bitfield is filled with runs of differing
// words over the whole pool, so every block of leaves and every subtree carries its own sums.
TEST_F(CBTInstanceTest, ReduceSumsTheBitfieldAtEveryNode)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));

    const uint32_t words = CBTBitfieldWords(kDefaultBisectorPoolSize);
    constexpr uint32_t kRun = 37u; // not a power of two: runs straddle every block boundary
    std::vector<uint32_t> bitfield(words);
    {
        const BufferHandle buffer = instance.GetResources().GetBuffer(CBTBinding::Bitfield);
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(cl);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::UnorderedAccess,
                                                         ResourceState::CopyDest));
        for (uint32_t first = 0u, run = 0u; first < words; first += kRun, ++run)
        {
            const uint32_t count = std::min(kRun, words - first);
            const uint32_t value = run % 5u == 0u ? 0u : (run * 2654435761u) ^ (run << 7u);
            std::fill_n(bitfield.begin() + first, count, value);
            cl->FillBuffer(buffer, static_cast<size_t>(first) * sizeof(uint32_t),
                           static_cast<size_t>(count) * sizeof(uint32_t), value);
        }
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::CopyDest,
                                                         ResourceState::UnorderedAccess));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();
    }

    DispatchReduce(*m_Device, m_KernelSet, instance);

    const uint32_t leaves = CBTLeafNodeCount(kDefaultBisectorPoolSize);
    std::vector<uint32_t> expected(CBTSumTreeNodeCount(kDefaultBisectorPoolSize));
    for (uint32_t leaf = 0u; leaf < leaves; ++leaf)
        expected[CBTLeafLevelOffset(kDefaultBisectorPoolSize) + leaf] =
            static_cast<uint32_t>(std::popcount(bitfield[leaf]));
    for (uint32_t node = CBTLeafLevelOffset(kDefaultBisectorPoolSize); node-- > 0u;)
        expected[node] = expected[2u * node + 1u] + expected[2u * node + 2u];

    const auto heap = instance.DebugReadWords(CBTBinding::SumTree, CBTSumTreeNodeCount(kDefaultBisectorPoolSize),
                                              CBTSumTreeHeapBase(kDefaultBisectorPoolSize));
    ASSERT_EQ(heap.size(), expected.size());
    uint32_t wrong = 0u;
    for (uint32_t node = 0u; node < expected.size(); ++node)
    {
        if (heap[node] == expected[node])
            continue;
        if (++wrong <= 8u)
            ADD_FAILURE() << "sum-tree node " << node << " reads " << heap[node] << ", the bitfield gives "
                          << expected[node];
    }
    EXPECT_EQ(wrong, 0u) << "of " << expected.size() << " nodes";
    EXPECT_GT(expected[0], 0u);
}

// First-load TDR hardening: Kernel_Reset seeds dispatch slot 0 from the sum-tree root
// (CBT_LiveCount). A corrupted root (raced/garbage memory) otherwise becomes a
// ~2^26-group indirect Classify — and because Classify's per-thread guard reads the
// SAME corrupt root, every one of those ~4.3B threads does full work: a multi-second
// GPU packet the OS kills (device loss). The clamp bounds the width to the pool's own
// thread budget and counts the engagement in the since-init telemetry slot, which
// Reset must NOT clear (second dispatch accumulates to 2).
TEST_F(CBTInstanceTest, ResetClampsCorruptSumTreeRootDispatch)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    // Corrupt the sum-tree root (heap node 0) to the worst representable live count.
    PoisonWord(*m_Device, instance.GetResources().GetBuffer(CBTBinding::SumTree),
               CBTSumTreeHeapBase(kDefaultBisectorPoolSize), 0xFFFFFFFFu);

    DispatchSingleKernel(*m_Device, m_KernelSet, instance, CBTKernel::Reset,
                         DefaultPlanarPush());

    const auto dispatch = instance.DebugReadWords(CBTBinding::IndirectDispatch, 3u, 0u);
    EXPECT_EQ(dispatch[0], kMaxDispatchGroups)
        << "Reset wrote an unbounded group count from the corrupt sum-tree root";
    EXPECT_EQ(dispatch[1], 1u);
    EXPECT_EQ(dispatch[2], 1u);
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQDispatchClampCounter), 1)
        << "the clamp engagement was not counted";

    // The telemetry is since-init: a second Reset over the still-corrupt root must
    // accumulate, not restart (Reset clears the per-frame counters, never this slot).
    DispatchSingleKernel(*m_Device, m_KernelSet, instance, CBTKernel::Reset,
                         DefaultPlanarPush());
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQDispatchClampCounter), 2)
        << "Reset cleared the since-init clamp telemetry";
}

// Same hardening for Kernel_PrepareIndirect, which promotes a work-queue counter into
// dispatch slot 0: a corrupted counter (INT_MAX) must clamp to the pool bound instead
// of becoming a ~2^25-group split dispatch.
TEST_F(CBTInstanceTest, PrepareIndirectClampsCorruptCounterDispatch)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    PoisonWord(*m_Device, instance.GetResources().GetBuffer(CBTBinding::WorkQueue),
               kWQSplitCounter, 0x7FFFFFFFu);

    CBTPushConstants pc = DefaultPlanarPush();
    pc.PassIndex = kWQSplitCounter;
    DispatchSingleKernel(*m_Device, m_KernelSet, instance, CBTKernel::PrepareIndirect, pc);

    const auto dispatch = instance.DebugReadWords(CBTBinding::IndirectDispatch, 3u, 0u);
    EXPECT_EQ(dispatch[0], kMaxDispatchGroups)
        << "PrepareIndirect wrote an unbounded group count from the corrupt counter";
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQDispatchClampCounter), 1)
        << "the clamp engagement was not counted";
}

// Neutrality oracle: healthy frames can never exceed the pool bound, so the clamp
// telemetry must stay exactly zero through refine / merge / teleport churn — the
// steady-state path is bit-identical with the clamp in place.
TEST_F(CBTInstanceTest, DispatchClampTelemetryStaysZeroThroughChurn)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    uint32_t frame = 0;
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
        RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 4u), frame);
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
        RunUpdate(*m_Device, instance, Refine(1u, kDefaultBaseDepth + 4u), frame); // teleport
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
        RunUpdate(*m_Device, instance, CBTInertClassify(), frame); // merge back

    ExpectValidateGreen(instance, "dispatch-clamp neutrality churn");
    EXPECT_EQ(instance.ReadWorkQueueCounter(kWQDispatchClampCounter), 0)
        << "the dispatch clamp engaged on a healthy frame — steady state is no longer "
           "bit-identical";
}

namespace
{
// Live slots and how far across the pool they reach, from the whole-pool HeapID buffer
// (a slot is live iff its 64-bit HeapID is non-zero — 0 is the free-slot sentinel).
struct LiveSlotSurvey
{
    std::vector<uint32_t> Slots; // ascending physical slot indices
    uint32_t MaxSlot = 0;        // highest live physical address
};

LiveSlotSurvey SurveyLiveSlots(CBTInstance& instance)
{
    const auto heap =
        instance.DebugReadWords(CBTBinding::HeapID, kDefaultBisectorPoolSize * 2u);
    const uint32_t slotCount = static_cast<uint32_t>(heap.size() / 2u);
    LiveSlotSurvey survey;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        if ((heap[slot * 2u] | heap[slot * 2u + 1u]) == 0u)
            continue;
        survey.Slots.push_back(slot);
        survey.MaxSlot = slot;
    }
    return survey;
}

// Drive the pool into a state that is both SCATTERED and topologically INERT — the combination
// the carry-over oracles need, and one the obvious churns do NOT produce.
//
// Allocation decodes the k-th UNSET occupancy bit, so it always refills the lowest free slots:
// any phase that ends by SPLITTING hands back a compact [0, live) prefix, and an oracle measured
// there would be blind to exactly the addresses a range-bounded copy gets wrong. Holes survive
// only where a phase ends by MERGING, with nothing left to reallocate them. Refining one root
// wide, collapsing part of it, then teleporting the focus to the other root and settling shallow
// leaves the surviving bisectors strewn above the live count and stable there.
//
// Measured trajectory of this sequence (planar base, 1M pool): live peaks in the low hundreds
// filling a contiguous span, and the tail settles at a live count in the teens whose highest live
// address sits several slots above it, holding steady with zero allocate and zero simplify per
// frame. The exact counts drift run to run (the split/merge kernels claim work through atomics),
// so the callers assert the SHAPE — inert, and scattered — never specific numbers. Returns the
// next frame index.
constexpr uint32_t kScatterPeakDepth = kDefaultBaseDepth + 6u;
constexpr uint32_t kScatterMidDepth = kDefaultBaseDepth + 4u;
constexpr uint32_t kScatterTeleportDepth = kDefaultBaseDepth + 5u;
constexpr uint32_t kScatterSettleRoot = 1u;
constexpr uint32_t kScatterSettleDepth = kDefaultBaseDepth + 3u;

// The classify the churn settles on; re-issuing it leaves the tree untouched.
CBTClassifyDesc ScatteredSettleClassify()
{
    return Refine(kScatterSettleRoot, kScatterSettleDepth);
}

uint32_t RunScatteringChurn(IDevice& device, CBTInstance& instance)
{
    uint32_t frame = 0;
    for (uint32_t i = 0; i < 10u; ++i, ++frame)
        RunUpdate(device, instance, Refine(0u, kScatterPeakDepth), frame);
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
        RunUpdate(device, instance, Refine(0u, kScatterMidDepth), frame);
    for (uint32_t i = 0; i < 10u; ++i, ++frame)
        RunUpdate(device, instance, Refine(kScatterSettleRoot, kScatterTeleportDepth), frame);
    for (uint32_t i = 0; i < 10u; ++i, ++frame)
        RunUpdate(device, instance, ScatteredSettleClassify(), frame);
    return frame;
}
} // namespace

// The neighbor ping-pong's carry-over must reach EVERY live slot, not a bounded prefix of the
// pool. A topologically INERT update has exactly one writer of the NEXT buffer — the carry-over
// itself — so afterwards NEXT must byte-equal CURRENT at every live slot. A carry-over that
// skipped a live slot leaves that slot's NEXT record as stale as the last frame that wrote it,
// which surfaces here as a mismatch. The comparison is over the two PHYSICAL buffers, so it is
// parity-independent, and it holds for the whole-pool CopyBuffer too — a genuine cross-arm
// oracle rather than a golden pinned to one implementation.
//
// Run after refine -> teleport churn so the live set is large AND scattered across the pool
// (LiveSlotAddressesAreScatteredAcrossThePool pins the scatter): those high, hole-punctuated
// addresses are exactly what a range-bounded copy would miss.
TEST_F(CBTInstanceTest, NeighborCarryOverReachesEveryLiveSlotAfterChurn)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const CBTClassifyDesc settled = ScatteredSettleClassify();
    uint32_t frame = RunScatteringChurn(*m_Device, instance);
    ExpectValidateGreen(instance, "pre-measurement churn");

    // Verify the instrument before trusting the reading: the measurement frame must genuinely be
    // topology-inert, or NEXT has writers besides the carry-over and the equality below asserts
    // nothing.
    RunUpdate(*m_Device, instance, settled, frame);
    ExpectValidateGreen(instance, "carry-over measurement frame");
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQAllocateCounter), 0)
        << "measurement frame split — Bisect also wrote NEXT, so the oracle is not isolated";
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQSimplifyCounter), 0)
        << "measurement frame merged — Simplify also wrote NEXT, so the oracle is not isolated";

    const LiveSlotSurvey live = SurveyLiveSlots(instance);
    ASSERT_GT(live.Slots.size(), static_cast<size_t>(kRootHalfedgeCount))
        << "churn never refined past the roots — nothing meaningful to carry over";
    // The oracle is only meaningful if it covers slots a range-bounded copy would miss, so pin
    // that the measured live set really is hole-punctuated rather than a compact prefix.
    ASSERT_GT(live.MaxSlot + 1u, static_cast<uint32_t>(live.Slots.size()))
        << "the measured live set is compact — this oracle is not covering the scattered case";

    const uint32_t words = (live.MaxSlot + 1u) * 4u;
    const auto a = instance.DebugReadWords(CBTBinding::NeighborsA, words);
    const auto b = instance.DebugReadWords(CBTBinding::NeighborsB, words);
    ASSERT_EQ(a.size(), static_cast<size_t>(words));
    ASSERT_EQ(b.size(), static_cast<size_t>(words));

    uint32_t mismatches = 0;
    uint32_t firstMismatch = 0;
    for (uint32_t slot : live.Slots)
    {
        bool equal = true;
        for (uint32_t c = 0; c < 4u; ++c)
            equal = equal && (a[slot * 4u + c] == b[slot * 4u + c]);
        if (!equal && mismatches++ == 0u)
            firstMismatch = slot;
    }
    EXPECT_EQ(mismatches, 0u)
        << "the carry-over skipped " << mismatches << " of " << live.Slots.size()
        << " live slots (first " << firstMismatch << ", highest live address " << live.MaxSlot
        << ") — CURRENT and NEXT disagree after a topology-inert update";
}

// The falsification guard for range-bounding the carry-over — or any future per-live-slot pass.
// CBT_DecodeFreeSlot hands out the k-th UNSET occupancy bit, so allocation refills low holes
// first; but Simplify frees arbitrary slots anywhere in the pool, so the live set is a SCATTERED
// subset rather than a compact [0, liveCount) range. This samples the teleport churn frame by
// frame and requires at least one frame whose highest live address sits above the live count: on
// such a frame a [0, liveCount) CopyBuffer provably skips live slots.
//
// Falsifiable by construction — if the allocator really did keep the live set compact, no frame
// would show a hole and this fails. That failure would be the evidence justifying a range copy.
TEST_F(CBTInstanceTest, LiveSlotAddressesAreScatteredAcrossThePool)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    uint32_t frame = 0;
    for (uint32_t i = 0; i < 6u; ++i, ++frame)
        RunUpdate(*m_Device, instance, Refine(0u, kDefaultBaseDepth + 4u), frame);

    uint32_t maxHoles = 0;
    uint32_t holeFrames = 0;
    uint32_t worstMaxSlot = 0;
    uint32_t worstLive = 0;
    for (uint32_t i = 0; i < 8u; ++i, ++frame)
    {
        RunUpdate(*m_Device, instance, Refine(1u, kDefaultBaseDepth + 4u), frame);
        ExpectValidateGreen(instance, "scatter survey frame");
        const LiveSlotSurvey live = SurveyLiveSlots(instance);
        ASSERT_FALSE(live.Slots.empty());
        const uint32_t span = live.MaxSlot + 1u;
        const uint32_t liveCount = static_cast<uint32_t>(live.Slots.size());
        if (span > liveCount)
        {
            ++holeFrames;
            maxHoles = std::max(maxHoles, span - liveCount);
            worstMaxSlot = live.MaxSlot;
            worstLive = liveCount;
        }
    }

    EXPECT_GT(holeFrames, 0u)
        << "no churn frame left a hole below the highest live slot — the live set stayed compact, "
           "which would make a [0, liveCount) range copy correct after all";
    EXPECT_GT(maxHoles, 0u) << "worst frame: highest live address " << worstMaxSlot << " with "
                            << worstLive << " live slots";
}

// Cost visibility: the carry-over's recorded width must track the LIVE set, not the pool. It
// dispatches indirect from dispatch slot 1, whose group count Reset derives from the sum-tree
// root, so the width the frame actually recorded is readable afterwards — the claim is measured,
// not narrated. Debug-headless MECHANISM check only: this asserts dispatch width and bytes
// touched, never milliseconds.
TEST_F(CBTInstanceTest, NeighborCarryOverDispatchWidthTracksLiveCountNotPoolSize)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const CBTClassifyDesc settled = ScatteredSettleClassify();
    uint32_t frame = RunScatteringChurn(*m_Device, instance);

    // An inert measurement frame keeps Reset's start-of-frame live count equal to the tail live
    // count read back below, so the two are directly comparable.
    RunUpdate(*m_Device, instance, settled, frame);
    ExpectValidateGreen(instance, "carry-over width measurement frame");
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQAllocateCounter), 0);
    ASSERT_EQ(instance.ReadWorkQueueCounter(kWQSimplifyCounter), 0);

    const uint32_t live = ReadSumTreeRoot(instance);
    ASSERT_GT(live, kRootHalfedgeCount) << "nothing refined — the width claim would be vacuous";

    const auto slot = instance.DebugReadWords(CBTBinding::IndirectDispatch, 3u,
                                              kDispatchSlotLive * 3u);
    const uint32_t expectedGroups = (live + kComputeWorkgroupSize - 1u) / kComputeWorkgroupSize;
    EXPECT_EQ(slot[0], expectedGroups)
        << "the carry-over dispatch width does not match ceil(live / WG) — live=" << live;
    EXPECT_EQ(slot[1], 1u);
    EXPECT_EQ(slot[2], 1u);

    // The headline: the recorded width is orders of magnitude below the whole-pool width the
    // 16 MiB CopyBuffer was equivalent to.
    EXPECT_LT(slot[0], kMaxDispatchGroups / 100u)
        << "carry-over width " << slot[0] << " is not far below the whole-pool width "
        << kMaxDispatchGroups;

    // Same claim in bytes: the whole-pool copy moved sizeof(CBTNeighbors) * poolSize every
    // update regardless of live count; the carry-over touches sizeof(CBTNeighbors) * live.
    const uint64_t poolBytes =
        static_cast<uint64_t>(kDefaultBisectorPoolSize) * sizeof(CBTNeighbors);
    const uint64_t liveBytes = static_cast<uint64_t>(live) * sizeof(CBTNeighbors);
    EXPECT_LT(liveBytes * 1000u, poolBytes)
        << "carried " << liveBytes << " B against a whole-pool " << poolBytes << " B";
}

namespace
{
// Overwrite one word of a live SSBO with a transfer write plus an explicit CopyDest ->
// UnorderedAccess settle, then wait, so the next update's shaders read it. The
// can-fail proof for the compact-stream invariant needs corruption the engine cannot produce on
// its own — every kernel that writes IndicesAll writes it correctly.
void PokeWord(IDevice& device, CBTInstance& instance, CBTBinding binding, uint32_t wordIndex,
              uint32_t value)
{
    BufferHandle buffer = instance.GetResources().GetBuffer(binding);
    device.UpdateBuffer(buffer, static_cast<size_t>(wordIndex) * 4u, sizeof(uint32_t), &value);
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::CopyDest,
                                                     ResourceState::UnorderedAccess));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
}

uint32_t ReadOccupancyBit(CBTInstance& instance, uint32_t slot)
{
    const auto word = instance.DebugReadWords(CBTBinding::Bitfield, 1u, slot / 32u);
    return (word.front() >> (slot & 31u)) & 1u;
}
} // namespace

// CAN-FAIL PROOF, entry arm. The compact stream is load-bearing topology state: the per-update
// neighbor carry-over scatters through IndicesAll[0 .. liveCount), so an entry naming a slot that
// is no longer live means some retained bisector was never carried over and keeps a stale NEXT
// record. That is silent corruption — it only surfaces later, as broken link reciprocity, at a
// kernel that is not the culprit.
//
// This is exactly the shape RegionFreeToBase used to leave behind (it collapsed the live count
// without rebuilding the stream, so the prefix held just-freed slots), reproduced here as a direct
// poison because the engine can no longer produce it. The counter must fire on the very update
// that dereferences the poisoned entry, and must clear once the next Indexation rebuilds the
// stream — a counter that stayed hot would be measuring something other than the poison.
TEST_F(CBTInstanceTest, CompactStreamAssertFiresWhenALivePrefixEntryNamesAFreedSlot)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const CBTClassifyDesc settled = ScatteredSettleClassify();
    uint32_t frame = RunScatteringChurn(*m_Device, instance);
    RunUpdate(*m_Device, instance, settled, frame++);
    ExpectValidateGreen(instance, "pre-poison settle");

    // Verify the instrument before trusting the reading: the poison value must genuinely be a
    // FREE slot, or the check has nothing to catch. One past the highest live address qualifies by
    // construction, and the occupancy bit — the authority the check reads — confirms it.
    const LiveSlotSurvey live = SurveyLiveSlots(instance);
    ASSERT_GT(live.Slots.size(), static_cast<size_t>(kRootHalfedgeCount))
        << "churn never refined past the roots — the poisoned prefix would be trivially small";
    const uint32_t freedSlot = live.MaxSlot + 1u;
    ASSERT_LT(freedSlot, kDefaultBisectorPoolSize);
    ASSERT_EQ(ReadOccupancyBit(instance, freedSlot), 0u)
        << "slot " << freedSlot << " is occupied — it is not the dead entry this poison needs";

    const uint32_t victim = instance.DebugReadWords(CBTBinding::IndicesAll, 1u, 0u).front();
    ASSERT_NE(victim, freedSlot);
    PokeWord(*m_Device, instance, CBTBinding::IndicesAll, 0u, freedSlot);

    RunUpdate(*m_Device, instance, settled, frame++);
    EXPECT_GT(instance.ReadValidationCounter(kValidationCompactCounter), 0u)
        << "IndicesAll[0] named freed slot " << freedSlot << " instead of live slot " << victim
        << " and the compact-stream check did not fire";

    // Self-healing: that update's own Indexation rebuilt the stream, so the next one is clean.
    // Without this the fire above could have come from any persistent state, not the poison.
    RunUpdate(*m_Device, instance, settled, frame++);
    EXPECT_EQ(instance.ReadValidationCounter(kValidationCompactCounter), 0u)
        << "the compact-stream check stayed hot after Indexation rebuilt the stream";
}

// CAN-FAIL PROOF, coverage arm. The entry arm alone cannot see UNDER-coverage — a stream whose
// entries are all live but which does not reach every live slot. The frame tail closes that: the
// number of entries Indexation binned must equal the live count the sum-tree reduce derived from
// the occupancy bitfield, and Indexation is repeat-free by construction (one atomicAdd per live
// slot, each returning a distinct index), so equal counts plus live entries means the stream IS
// the live set.
//
// Poisoning the count means desyncing the two occupancy authorities: an occupancy bit set for a
// slot whose HeapID is still the free sentinel is counted by the reduce and skipped by Indexation.
// That is also a zombie by definition, so the zombie counter fires too — this arm is falsifiable
// jointly with it, not independently.
TEST_F(CBTInstanceTest, CompactStreamAssertFiresWhenTheLiveCountExceedsTheStreamLength)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    const CBTClassifyDesc settled = ScatteredSettleClassify();
    uint32_t frame = RunScatteringChurn(*m_Device, instance);
    RunUpdate(*m_Device, instance, settled, frame++);
    ExpectValidateGreen(instance, "pre-poison settle");

    const LiveSlotSurvey live = SurveyLiveSlots(instance);
    const uint32_t phantom = live.MaxSlot + 1u;
    ASSERT_LT(phantom, kDefaultBisectorPoolSize);
    ASSERT_EQ(ReadOccupancyBit(instance, phantom), 0u);

    // Set the occupancy bit alone — HeapID stays 0, so the slot is live to the sum tree and free
    // to Indexation.
    const uint32_t bitWord =
        instance.DebugReadWords(CBTBinding::Bitfield, 1u, phantom / 32u).front();
    PokeWord(*m_Device, instance, CBTBinding::Bitfield, phantom / 32u,
             bitWord | (1u << (phantom & 31u)));

    RunUpdate(*m_Device, instance, settled, frame++);
    EXPECT_GT(instance.ReadValidationCounter(kValidationCompactCounter), 0u)
        << "slot " << phantom
        << " is counted live by the reduce but binned by nothing, and the coverage check did not "
           "fire";
}
