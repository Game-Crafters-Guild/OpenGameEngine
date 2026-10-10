// Render-graph seam: CBTUpdateNode declares one "CBT.Update" compute pass whose
// exec records the whole hand-barriered kernel sequence on ctx.Cmd (the Ocean
// pattern). Drives the update through a real RGFrame on the headless device and
// proves (1) the pass schedules and runs, (2) it produces the SAME result as the
// direct CommandList path, and (3) Validate stays green.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "CBTTerrain/CBTActivityReadback.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTResources.h"
#include "CBTTerrain/CBTUpdateNode.h"
#include "CBTTerrain/CBTUpdateRestGate.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "RGPassQuery.h"
#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
namespace RGQuery = GameEngine::Testing::RGQuery;

namespace
{
struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

// Copy the draw records + validation counters out, then read the all-stream count.
uint32_t ReadbackDrawCount(IDevice& device, CBTInstance& instance)
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    instance.RecordReadback(*cl);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return instance.ReadDrawIndexCount(kDrawStreamAll);
}

uint32_t ReadSumTreeRoot(CBTInstance& instance)
{
    const auto words =
        instance.DebugReadWords(CBTBinding::SumTree, 1u, CBTSumTreeHeapBase(kDefaultBisectorPoolSize));
    return words.front();
}

// 4x4 R32_FLOAT height texture cleared to a constant, settled in ShaderResource — a
// valid handle so DeclareUpdatePass takes its haveHeight branch (import + read edge).
TextureHandle MakeShaderResourceHeight(IDevice& device, float value)
{
    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.UpdateNodeTest.Height";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::Undefined,
                                                      ResourceState::CopyDest));
    const float clearValue[4] = {value, 0.0f, 0.0f, 0.0f};
    cl->ClearColorImageSubresource(tex, 0, 0, clearValue);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    return tex;
}

// Slot-indexed state snapshot for cross-path equality. Bounded prefixes (the
// readback staging buffer is tiny): HeapID (not ping-ponged) + NeighborsA, plus
// the slot-invariant live count and draw count. Identical dispatches on the same
// device assign slots identically, so the raw prefixes must match byte-for-byte.
struct StateSnapshot
{
    std::vector<uint32_t> Heap;
    std::vector<uint32_t> NeighborsA;
    uint32_t SumRoot = 0;
    uint32_t DrawCount = 0;
};

StateSnapshot Snapshot(IDevice& device, CBTInstance& instance)
{
    StateSnapshot s;
    s.DrawCount = ReadbackDrawCount(device, instance);
    s.Heap = instance.DebugReadWords(CBTBinding::HeapID, 28u);       // 14 slots
    s.NeighborsA = instance.DebugReadWords(CBTBinding::NeighborsA, 28u); // 7 slots
    s.SumRoot = ReadSumTreeRoot(instance);
    return s;
}
} // namespace

class CBTUpdateNodeTest : public ::testing::Test
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

TEST_F(CBTUpdateNodeTest, DeclaresAndRunsUpdatePassThroughRenderGraph)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};
    uint32_t lastCount = 0;
    for (uint32_t f = 0; f < 4u; ++f)
    {
        frame.BeginFrame(f);
        CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), f,
                                         TextureHandle{});
        frame.Execute();
        m_Device->WaitForIdle();
        EXPECT_TRUE(RGQuery::ScheduledIndex(frame.Graph(), RGQuery::Exact{"CBT.Update"}).has_value())
            << "CBT.Update pass was culled/never scheduled at frame " << f;

        lastCount = ReadbackDrawCount(*m_Device, instance);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationBudgetCounter), 0u);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u);
    }
    EXPECT_GT(lastCount, kRootHalfedgeCount * 3u) << "the graph-driven update never refined";
}

// The declared update pass comes back on the outputs. The caller needs it to order this pass
// against a producer this module has no business knowing about — the terrain texture upload,
// which shares no declared access with it and so is not ordered by the phase. A handle that came
// back invalid, or pointed at some other declared pass, would silently drop that ordering edge.
TEST_F(CBTUpdateNodeTest, DeclaredOutputsCarryTheUpdatePass)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};

    frame.BeginFrame(0u);
    const CBTUpdateOutputs out = CBTUpdateNode::DeclareUpdatePass(
        frame, instance, refine, CBTIdentityFrameParams(), 0u, TextureHandle{});

    ASSERT_TRUE(out.Update.IsValid())
        << "the caller cannot add an ordering edge to an invalid pass";
    EXPECT_EQ(std::string(frame.Graph().PassName(out.Update.Id)), "CBT.Update")
        << "the returned handle must be the update pass itself, not another declared pass";

    frame.Execute();
    m_Device->WaitForIdle();
}

// The four declared outputs carry their physical byte size into the graph. A size-less
// import reads as a 0-byte resource, which is what a reader bounding a copy against it —
// the debug server's buffer readback — refuses.
TEST_F(CBTUpdateNodeTest, DeclaredOutputsCarryTheirByteSize)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};

    frame.BeginFrame(0u);
    const CBTUpdateOutputs out = CBTUpdateNode::DeclareUpdatePass(
        frame, instance, refine, CBTIdentityFrameParams(), 0u, TextureHandle{});
    frame.Execute();
    m_Device->WaitForIdle();

    ASSERT_TRUE(out.IndirectDraw.IsValid());
    CBTResources& res = instance.GetResources();
    const auto declaredSize = [&frame](RGBuffer b) {
        return frame.Graph().ResourceDesc(b.Id).SizeBytes;
    };
    EXPECT_GT(res.GetBufferByteSize(CBTBinding::IndirectDraw), 0u);
    EXPECT_EQ(declaredSize(out.IndirectDraw), res.GetBufferByteSize(CBTBinding::IndirectDraw));
    EXPECT_EQ(declaredSize(out.IndicesAll), res.GetBufferByteSize(CBTBinding::IndicesAll));
    EXPECT_EQ(declaredSize(out.IndicesVisible), res.GetBufferByteSize(CBTBinding::IndicesVisible));
    EXPECT_EQ(declaredSize(out.CurrentVertex), res.GetBufferByteSize(CBTBinding::CurrentVertex));
}

// The RG-pass path and the direct CommandList path must converge to the same
// topology — the exec lambda calls the same RecordUpdate, so any divergence
// would be a graph-side barrier/ordering bug.
TEST_F(CBTUpdateNodeTest, RenderGraphPathMatchesDirectPath)
{
    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};
    constexpr uint32_t kFrames = 5u;

    // Direct path.
    StateSnapshot direct;
    {
        CBTInstance instance;
        ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(instance.InitializeRoots());
        for (uint32_t f = 0; f < kFrames; ++f)
        {
            auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            instance.RecordUpdate(*cl, refine, CBTIdentityFrameParams(), f);
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            m_Device->ExecuteCommandLists(lists);
            m_Device->WaitForIdle();
        }
        direct = Snapshot(*m_Device, instance);
    }

    // Render-graph path.
    StateSnapshot graph;
    {
        CBTInstance instance;
        ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
        ASSERT_TRUE(instance.InitializeRoots());
        FramePools pools(m_Device.get());
        RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        for (uint32_t f = 0; f < kFrames; ++f)
        {
            frame.BeginFrame(f);
            CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), f,
                                             TextureHandle{});
            frame.Execute();
            m_Device->WaitForIdle();
        }
        graph = Snapshot(*m_Device, instance);
    }

    EXPECT_GT(direct.DrawCount, kRootHalfedgeCount * 3u) << "the update never refined";
    EXPECT_EQ(graph.DrawCount, direct.DrawCount) << "RG draw count diverged from the direct path";
    EXPECT_EQ(graph.SumRoot, direct.SumRoot) << "RG live count diverged from the direct path";
    EXPECT_EQ(graph.Heap, direct.Heap) << "RG HeapID prefix diverged — a barrier/ordering bug";
    EXPECT_EQ(graph.NeighborsA, direct.NeighborsA) << "RG neighbor prefix diverged";
}

// C5 read edge (adversarial-review m4): the other two update-node tests pass an
// invalid TextureHandle, so haveHeight is always false — the ImportExternalTexture +
// SampledCompute read edge + the kEarlySetup+1 phase were never exercised. This binds
// a REAL height texture and drives the update through the graph: the pass must still
// schedule, run, refine, and stay Validate-clean with the read edge declared.
TEST_F(CBTUpdateNodeTest, RunsWithBoundHeightTextureReadEdge)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());

    TextureHandle height = MakeShaderResourceHeight(*m_Device, 0.5f);
    ASSERT_TRUE(height.IsValid());
    for (uint32_t s = 0; s < kCBTFrameParamsRing; ++s)
        instance.SetHeightSource(s, height); // any frame slot samples the real texture

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};
    uint32_t lastCount = 0;
    for (uint32_t f = 0; f < 4u; ++f)
    {
        frame.BeginFrame(f);
        // Real height handle -> DeclareUpdatePass imports it and declares the compute
        // read edge (the branch the {} tests skip).
        CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), f,
                                         height);
        frame.Execute();
        m_Device->WaitForIdle();
        EXPECT_TRUE(RGQuery::ScheduledIndex(frame.Graph(), RGQuery::Exact{"CBT.Update"}).has_value())
            << "CBT.Update with a bound height texture was culled at frame " << f;
        lastCount = ReadbackDrawCount(*m_Device, instance);
        EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u) << "frame " << f;
        EXPECT_EQ(instance.ReadValidationCounter(kValidationZombieCounter), 0u) << "frame " << f;
    }
    EXPECT_GT(lastCount, kRootHalfedgeCount * 3u) << "the height-bound update never refined";

    m_Device->DestroyTexture(height);
}

// A still view settles: once the tree is refined and the readback shows quiet updates, the update
// stops being declared, the frames that skip it leave the buffers the draw reads as the last update
// wrote them, and new inputs bring the update back.
TEST_F(CBTUpdateNodeTest, AStillViewSettlesAndStopsUpdating)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    CBTActivityReadback readback;
    ASSERT_TRUE(readback.Initialize(*m_Device));
    CBTUpdateRestGate gate;

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 3u};
    // Runs one frame; true when it declared the update.
    const auto runFrame = [&](uint32_t f)
    {
        frame.BeginFrame(f);
        uint64_t sequence = 0;
        CBTUpdateActivity activity{};
        if (readback.TryRead(sequence, activity))
            gate.OnActivityRead(sequence, activity);
        std::vector<uint8_t> inputs(sizeof(refine));
        std::memcpy(inputs.data(), &refine, sizeof(refine));
        const bool skip = gate.CanSkip(inputs);
        if (!skip)
            CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), f,
                                             TextureHandle{},
                                             readback.Begin(frame, gate.OnUpdateRecorded(false)));
        frame.Execute();
        readback.OnFrameSubmitted(frame, frame.SubmissionToken());
        m_Device->WaitForIdle();
        EXPECT_EQ(RGQuery::ScheduledIndex(frame.Graph(), RGQuery::Exact{"CBT.Update"}).has_value(), !skip)
            << "frame " << f;
        return !skip;
    };

    constexpr uint32_t kFrames = 40u;
    uint32_t firstSkipped = kFrames;
    uint32_t settledCount = 0;
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        const bool declared = runFrame(f);
        if (!declared && firstSkipped == kFrames)
        {
            firstSkipped = f;
            settledCount = ReadbackDrawCount(*m_Device, instance);
        }
        if (firstSkipped < f)
            EXPECT_FALSE(declared) << "a settled still view declared the update again at frame " << f;
    }
    ASSERT_LT(firstSkipped, kFrames) << "the update never settled";
    EXPECT_GT(settledCount, kRootHalfedgeCount * 3u) << "settled before the tree refined";
    EXPECT_EQ(ReadbackDrawCount(*m_Device, instance), settledCount)
        << "the skipped frames changed the buffers the draw reads";
    EXPECT_EQ(instance.ReadValidationCounter(kValidationErrorCounter), 0u);

    // Rest means an update with these inputs would change nothing: run one and compare.
    frame.BeginFrame(kFrames);
    CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), kFrames,
                                     TextureHandle{});
    frame.Execute();
    m_Device->WaitForIdle();
    EXPECT_EQ(ReadbackDrawCount(*m_Device, instance), settledCount)
        << "the update was skipped before the tree came to rest";

    refine.TargetDepth = kDefaultBaseDepth + 4u;
    EXPECT_TRUE(runFrame(kFrames + 1u)) << "new inputs did not bring the update back";
    EXPECT_GT(ReadbackDrawCount(*m_Device, instance), settledCount);
    readback.Shutdown();
}

// A slot whose copy never ran (zero-filled) must not read as an update's counters: zeros would look
// like a quiet update and rest the gate on a tree that never settled. The copy's stamp tells them apart.
TEST_F(CBTUpdateNodeTest, AnUnwrittenActivitySlotIsNeverRead)
{
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*m_Device, m_KernelSet));
    ASSERT_TRUE(instance.InitializeRoots());
    CBTActivityReadback readback;
    ASSERT_TRUE(readback.Initialize(*m_Device));

    FramePools pools(m_Device.get());
    RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    const CBTClassifyDesc refine{kClassifyDepthTarget, 0u, kDefaultBaseDepth + 1u};
    const auto runFrame = [&](uint32_t f, bool record)
    {
        frame.BeginFrame(f);
        const CBTActivitySlot slot = readback.Begin(frame, f);
        CBTUpdateNode::DeclareUpdatePass(frame, instance, refine, CBTIdentityFrameParams(), f,
                                         TextureHandle{}, record ? slot : CBTActivitySlot{});
        frame.Execute();
        readback.OnFrameSubmitted(frame, frame.SubmissionToken());
        m_Device->WaitForIdle();
    };

    uint64_t sequence = 0;
    CBTUpdateActivity activity{};
    runFrame(0u, false);
    EXPECT_FALSE(readback.TryRead(sequence, activity)) << "an unwritten slot was read";
    runFrame(1u, true);
    EXPECT_TRUE(readback.TryRead(sequence, activity)) << "a written slot was not read";
    EXPECT_EQ(sequence, 1u);
    readback.Shutdown();
}
