// Stage 2b-3.2: command recording + submission over the REAL device. Execute()
// now records actual command buffers (barriers → render passes → pass lambdas)
// and submits them with timeline-semaphore sync. The debug-layer variant is the
// in-process arm of the VUID gate (the strict log assertion runs in the editor
// during Stage-3 validation).

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Source/Vulkan/VulkanCommandList.h"
#include "Tests/RenderGraph/RGTestDevice.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;

namespace
{
// The gate tests below read the validation VERDICT from GetValidationStats and
// fail the gtest assertion with the per-VUID table on any error. The
// process-wide abort-on-error default (GE_VK_VALIDATION_ASSERT) would kill the
// run before that evidence is collected — disarm it before any device work
// (the enable caches on the first error, so set it process-wide).
void DisarmValidationAssert()
{
#if defined(_WIN32)
    _putenv_s("GE_VK_VALIDATION_ASSERT", "0");
#else
    setenv("GE_VK_VALIDATION_ASSERT", "0", 1);
#endif
}

// Failure-message dump: the per-VUID table with first-occurrence context.
std::string VuidSummary(const ValidationStats& stats)
{
    std::string s = "errors=" + std::to_string(stats.ErrorCount) +
                    " warnings=" + std::to_string(stats.WarningCount);
    for (const auto& v : stats.Vuids)
    {
        s += "\n  [" + std::string(v.IsError ? "ERROR" : "warn") + " x" +
             std::to_string(v.Count) + "] " + v.Vuid;
        if (!v.FirstObjects.empty())
            s += " objects: " + v.FirstObjects;
        if (!v.FirstLabels.empty())
            s += " labels: " + v.FirstLabels;
        if (!v.FirstMessage.empty())
            s += "\n    " + v.FirstMessage;
    }
    return s;
}

std::unique_ptr<IDevice> MakeDevice(bool debugLayer)
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = debugLayer;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

TextureDesc ColorDesc(uint32_t w = 64, uint32_t h = 64)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}

TextureDesc DepthDesc(uint32_t w = 64, uint32_t h = 64)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    return d;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};

// A small editor-shaped frame: depth prepass -> async-eligible compute cull ->
// world (color+depth attach, reads cull buffer) -> post (reads color). Returns
// the number of lambdas that observed a live command list.
uint32_t BuildAndExecuteFrame(RGFrame& frame, uint64_t frameIndex)
{
    frame.BeginFrame(frameIndex);
    uint32_t sawCmd = 0;

    RGTexture depth = frame.CreateTexture("Depth", DepthDesc());
    RGTexture color = frame.CreateTexture("Color", ColorDesc());
    RGTexture ldr = frame.CreateTexture("LDR", ColorDesc());
    BufferDesc cullDesc;
    cullDesc.size = 4096;
    cullDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGBuffer cull = frame.CreateBuffer("CullArgs", cullDesc);

    auto see = [&](RGContext& ctx)
    {
        if (ctx.Cmd != nullptr)
            ++sawCmd;
    };

    frame.AddPass("DepthPrepass", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachDepth(depth, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}}); },
                  [&see](RGContext& ctx) { see(ctx); });
    frame.AddComputePass("ClusterCull", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(depth); // sampled on the compute queue
                             p.Write(cull);
                         },
                         [&see](RGContext& ctx) { see(ctx); });
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear, .Clear = {.Color = {0, 0, 0, 1}}});
                      p.AttachDepth(depth, {.Load = RGLoadOp::Load}, RGDepthAccess::ReadOnly);
                      p.Read(cull);
                  },
                  [&see](RGContext& ctx) { see(ctx); });
    frame.AddPass("Post", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(color);
                      p.AttachColor(0, ldr, {.Load = RGLoadOp::DontCare});
                  },
                  [&see](RGContext& ctx) { see(ctx); });
    frame.MarkOutput(ldr);
    frame.Execute();
    return sawCmd;
}
} // namespace

TEST(RGRecord, RecordsSubmitsAndSignalsAcrossQueues)
{
    auto dev = MakeDevice(/*debugLayer=*/false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // true-async pin

    const uint32_t sawCmd = BuildAndExecuteFrame(frame, 0);
    dev->WaitForIdle();

    EXPECT_EQ(sawCmd, 4u) << "every pass lambda must receive a live CommandList";
    const auto& stats = frame.Stats();
    // Graphics{DepthPrepass} -> Compute{ClusterCull} -> Graphics{World, Post}.
    EXPECT_EQ(stats.SubmissionsMade, 3u);
    EXPECT_EQ(stats.RenderPassesBegun, 3u) << "three raster passes, compute has none";
    EXPECT_GT(stats.BarrierBatchesEmitted, 0u);

    // Hoist accounting: every BottomOfPipe-sourced IR barrier was hoisted, and
    // everything emitted is accounted for.
    uint32_t bottomSourced = 0;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.SrcStage & RGStage::BottomOfPipe)
            ++bottomSourced;
    EXPECT_EQ(stats.BarriersHoisted, bottomSourced);
    EXPECT_EQ(stats.BarriersEmitted, frame.Graph().BarrierCount());
    EXPECT_GT(stats.BarriersHoisted, 0u) << "first-use inits exist in this frame";

    // Cross-queue timeline sync was planned (compute waits on graphics, world
    // waits on compute).
    ASSERT_EQ(frame.Graph().Submissions().size(), 3u);
    EXPECT_GT(frame.Graph().Submissions()[1].WaitCount, 0u);
    EXPECT_GT(frame.Graph().Submissions()[2].WaitCount, 0u);

    // Frame 2: persistent command lists + timelines reused; signal values advance.
    const uint32_t sawCmd2 = BuildAndExecuteFrame(frame, 1);
    dev->WaitForIdle();
    EXPECT_EQ(sawCmd2, 4u);
    EXPECT_GT(frame.Graph().Submissions()[0].SignalValue, 1u)
        << "timeline values persist across frames";
}

TEST(RGRecord, AliasedQueueMapCollapsesToSingleSubmission)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    // kSingleQueueMap is the DEFAULT — this pin runs it unmodified.

    BuildAndExecuteFrame(frame, 0);
    dev->WaitForIdle();

    EXPECT_EQ(frame.Stats().SubmissionsMade, 1u) << "aliased queues collapse the fork/join";
    ASSERT_EQ(frame.Graph().Submissions().size(), 1u);
    EXPECT_EQ(frame.Graph().Submissions()[0].WaitCount, 0u);
}

// Gate-1 fleet pin: a collapsed-map frame whose FIRST scheduled pass is
// COMPUTE. The submission takes its logical label from that first pass —
// before the physical-queue recording fix, the whole frame (render passes
// included) recorded into a compute-family command buffer and submitted on
// the compute queue. The pin: one submission, all render passes begun, and
// the recording is device-clean (validation layer when available).
TEST(RGRecord, ComputeFirstFrameUnderCollapsedMapRecordsOnGraphics)
{
    auto dev = MakeDevice(/*debugLayer=*/true);
    if (!dev)
        dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture color = frame.CreateTexture("CF.Color", ColorDesc());
    BufferDesc bd{};
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGBuffer data = frame.CreateBuffer("CF.Data", bd);

    // Compute FIRST (no graphics pass precedes it in declaration or schedule).
    frame.AddComputePass("CF.Seed", -100,
                         [&](RGPassBuilder& p) { p.Write(data); },
                         [](RGContext&) {});
    frame.AddPass("CF.World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(data);
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    EXPECT_EQ(frame.Stats().SubmissionsMade, 1u) << "collapsed map: one submission";
    EXPECT_EQ(frame.Stats().RenderPassesBegun, 1u)
        << "the render pass must record (a compute-family command buffer would reject it)";
    ASSERT_EQ(frame.Graph().Submissions().size(), 1u);
    EXPECT_EQ(frame.Graph().Submissions()[0].Queue, RGQueue::Compute)
        << "the submission label IS compute-first — the recording must key on PhysicalQueue";
    EXPECT_EQ(frame.Graph().Submissions()[0].PhysicalQueue, 0u);
}

// The in-process arm of the VUID gate: the same frame recorded under the
// Vulkan validation layer, with the layer's verdict READ BACK — reset the
// stats, execute, assert zero errors (the per-VUID table prints on failure).
// Necessary, not sufficient: no VUID ties image layouts to queue capability,
// so a clean run proves the recorded stage masks are legal, not that
// transitions execute on the right queue — the structural RGSubmission/
// RGBarrier tests carry that half.
TEST(RGRecord, ValidationLayerFrameExecutesClean)
{
    DisarmValidationAssert();
    auto dev = MakeDevice(/*debugLayer=*/true);
    if (!dev)
        GTEST_SKIP() << "no validation-layer device available";
    if (!dev->GetValidationStats().Enabled)
        GTEST_SKIP() << "validation layer not present on this machine";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // true-async pin

    dev->ResetValidationStats(); // exclude device-creation noise from the window

    const uint32_t sawCmd = BuildAndExecuteFrame(frame, 0);
    dev->WaitForIdle();
    EXPECT_EQ(sawCmd, 4u);
    EXPECT_EQ(frame.Stats().SubmissionsMade, 3u);

    BuildAndExecuteFrame(frame, 1); // second frame exercises import/reuse paths
    dev->WaitForIdle();

    const ValidationStats stats = dev->GetValidationStats();
    EXPECT_EQ(stats.ErrorCount, 0u) << VuidSummary(stats);
}

// The cross-physical transition gate on a real device: graphics renders into a
// colour attachment, compute samples it (§1's IBL_EnvCapture crossing). The
// ColorAttachment->ShaderReadOnly transition must record at the GRAPHICS tail —
// the only queue where COLOR_ATTACHMENT_OUTPUT is a legal source stage — and
// the frame must still validate clean. Necessary, not sufficient: no VUID ties
// a layout to queue capability, so the placement half is asserted structurally
// and the layer only certifies the recorded masks.
TEST(RGRecord, CrossQueueLayoutTransitionRecordsAtTheProducerTailAndValidatesClean)
{
    DisarmValidationAssert();
    auto dev = MakeDevice(/*debugLayer=*/true);
    if (!dev)
        GTEST_SKIP() << "no validation-layer device available";
    const bool validationActive = dev->GetValidationStats().Enabled;
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // true-async pin

    dev->ResetValidationStats();

    frame.BeginFrame(0);
    RGTexture cap = frame.CreateTexture("Xq.Capture", ColorDesc());
    TextureDesc mipDesc = ColorDesc();
    mipDesc.usage =
        static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess);
    RGTexture mip = frame.CreateTexture("Xq.Mip", mipDesc);
    frame.AddPass("Xq.Capture", 0,
                  [&](RGPassBuilder& p) { p.AttachColor(0, cap, {.Load = RGLoadOp::Clear}); },
                  [](RGContext&) {});
    frame.AddComputePass("Xq.Downsample", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(cap, RGTextureRead::Sampled);
                             p.Write(mip, RGTextureWrite::Storage);
                         },
                         [](RGContext&) {});
    frame.MarkOutput(mip);
    frame.Execute();
    dev->WaitForIdle();

    const auto& subs = frame.Graph().Submissions();
    ASSERT_EQ(subs.size(), 2u);
    EXPECT_EQ(subs[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(subs[1].Queue, RGQueue::Compute);

    const auto& barriers = frame.Graph().Barriers();
    const auto& placement = frame.Graph().BarrierSubmissions();
    ASSERT_EQ(placement.size(), barriers.size());
    uint32_t crossings = 0;
    for (uint32_t i = 0; i < barriers.size(); ++i)
    {
        const RGBarrier& b = barriers[i];
        if (b.Resource != cap.Id || !b.IsTexture || b.OldLayout != RGImageLayout::ColorAttachment ||
            b.NewLayout != RGImageLayout::ShaderReadOnly)
            continue;
        ++crossings;
        EXPECT_EQ(b.SrcStage, RGStage::ColorAttachmentOutput);
        EXPECT_EQ(placement[i], 0u)
            << "the attachment->sampled transition must record on the graphics submission";
    }
    EXPECT_EQ(crossings, 1u) << "exactly ONE barrier per crossing";

    if (!validationActive)
        GTEST_SKIP() << "validation layer not present: placement asserted, verdict unavailable";
    const ValidationStats stats = dev->GetValidationStats();
    EXPECT_EQ(stats.ErrorCount, 0u) << VuidSummary(stats);
}

// The export-hole gate: a compute pass writes a persistent texture whose
// export contract restores ShaderReadOnly, under the identity (true-async)
// map on a real device. The restore must record at the COMPUTE tail — the
// queue that owns the cells — not on the last graphics submission (the old
// placement raced the compute write in release and asserted in debug; the
// Settle workarounds existed to dodge exactly this shape). Frame 1 then
// samples the texture on graphics, proving the resting-state claim the
// export wrote back is TRUE on the device (a false claim is a layout
// mismatch the validation layer reports).
TEST(RGRecord, ComputeWrittenExportRecordsAtItsQueueTailAndValidatesClean)
{
    DisarmValidationAssert();
    auto dev = MakeDevice(/*debugLayer=*/true);
    if (!dev)
        GTEST_SKIP() << "no validation-layer device available";
    const bool validationActive = dev->GetValidationStats().Enabled;
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // true-async pin

    dev->ResetValidationStats();

    // Frame 0 — the bake shape: graphics work first, then the compute bake.
    frame.BeginFrame(0);
    TextureDesc bakeDesc = ColorDesc();
    bakeDesc.usage =
        static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess);
    RGTexture baked = frame.ImportPersistentTexture("Exp.Baked", bakeDesc);
    RGTexture color = frame.CreateTexture("Exp.Color", ColorDesc());
    frame.AddPass("Exp.World", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachColor(0, color, {.Load = RGLoadOp::Clear}); },
                  [](RGContext&) {});
    frame.AddComputePass("Exp.Bake", 0,
                         [&](RGPassBuilder& p) { p.Write(baked, RGTextureWrite::Storage); },
                         [](RGContext&) {});
    frame.MarkOutput(color);
    frame.MarkOutput(baked, RGImageLayout::ShaderReadOnly);
    frame.Execute();
    dev->WaitForIdle();

    // Structure: [graphics, compute] submissions; ONE export barrier (the
    // restore), owned by the compute queue and placed at the compute tail.
    const auto& subs = frame.Graph().Submissions();
    ASSERT_EQ(subs.size(), 2u);
    EXPECT_EQ(subs[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(subs[1].Queue, RGQueue::Compute);
    const RGBarrierBatch* exportBatch = nullptr;
    for (const RGBarrierBatch& bb : frame.Graph().BarrierBatches())
        if (bb.Pass == kInvalidId)
            exportBatch = &bb;
    ASSERT_NE(exportBatch, nullptr);
    ASSERT_EQ(exportBatch->Count, 1u) << "exactly the baked texture's restore";
    EXPECT_EQ(frame.Graph().BarrierSubmissions()[exportBatch->First], 1u)
        << "the restore must record on the compute submission that owns the cells";
    const RGBarrierBatch* sentinel = nullptr;
    for (const RGBarrierBatch& bb : frame.Graph().BarrierBatches())
        if (bb.Pass == kInvalidId)
            sentinel = &bb;
    ASSERT_NE(sentinel, nullptr);
    const RGBarrier& restore = frame.Graph().Barriers()[sentinel->First];
    EXPECT_EQ(restore.OldLayout, RGImageLayout::General);
    EXPECT_EQ(restore.NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(restore.SrcQueue, static_cast<uint32_t>(RGQueue::Compute));
    EXPECT_EQ(frame.Graph().FinalLayout(baked.Id), RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(pools.Persistent.GetState("Exp.Baked"), ResourceState::ShaderResource)
        << "the pool must carry the exported resting state into frame 1";

    // Frame 1 — the claim consumer: graphics samples the baked texture at the
    // imported (pool-tracked) ShaderReadOnly state.
    frame.BeginFrame(1);
    RGTexture baked1 = frame.ImportPersistentTexture("Exp.Baked", bakeDesc);
    RGTexture color1 = frame.CreateTexture("Exp.Color", ColorDesc());
    frame.AddPass("Exp.WorldSample", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color1, {.Load = RGLoadOp::Clear});
                      p.Read(baked1, RGTextureRead::Sampled);
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color1);
    frame.Execute();
    dev->WaitForIdle();

    if (validationActive)
    {
        const ValidationStats stats = dev->GetValidationStats();
        EXPECT_EQ(stats.ErrorCount, 0u) << VuidSummary(stats);
    }
}

// Floating-window close path: destroying the graph right after Execute — with
// GPU work still in flight — must drain its own timelines first (destroying a
// semaphore the GPU still signals is a device-loss class bug). Run under the
// validation layer so an in-use destroy would surface.
TEST(RGRecord, DestructionRightAfterExecuteDrainsOwnWork)
{
    auto dev = MakeDevice(/*debugLayer=*/true);
    if (!dev)
        GTEST_SKIP() << "no validation-layer device available";
    FramePools pools(dev.get());
    {
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        BuildAndExecuteFrame(frame, 0);
        // NO WaitForIdle — ~RGFrame must wait its own timelines before destroy.
    }
    dev->WaitForIdle(); // device still healthy afterwards
}

// SubmissionToken replaces the old GetSubmissionToken for readback consumers:
// a timeline (semaphore, value) pair pollable without a device stall.
TEST(RGRecord, SubmissionTokenTracksPerQueueCompletion)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // per-queue tokens need real queues

    EXPECT_FALSE(frame.SubmissionToken().IsValid()) << "nothing submitted yet";

    BuildAndExecuteFrame(frame, 0); // G/C/G shape: both queues submit
    const auto gfx0 = frame.SubmissionToken(RGQueue::Graphics);
    const auto cmp0 = frame.SubmissionToken(RGQueue::Compute);
    ASSERT_TRUE(gfx0.IsValid());
    ASSERT_TRUE(cmp0.IsValid());
    dev->WaitForIdle();
    EXPECT_EQ(dev->QueryGpuSyncToken(gfx0), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(dev->QueryGpuSyncToken(cmp0), IDevice::GpuSyncStatus::Complete);
    EXPECT_TRUE(dev->WaitGpuSyncToken(gfx0, 0)) << "already-signaled wait must not block";

    BuildAndExecuteFrame(frame, 1);
    const auto gfx1 = frame.SubmissionToken(RGQueue::Graphics);
    EXPECT_GT(gfx1.value, gfx0.value) << "tokens advance monotonically across frames";
    dev->WaitForIdle();
}

// Class pin (instance 1 of "absence of a sync token reads as completion"): a queue
// that submitted nothing this frame mints no token, and the device must answer
// Unknown for it — never Complete. A reuse gate stamped with the per-queue tokens
// of a frame (the transient pool's fix direction) would otherwise read "the compute
// queue never ran" as "the compute queue finished", which is the same fail-open
// shape as an unarmed frame fence short-circuiting to "waited".
TEST(RGRecord, AbsentQueueSubmissionTokenIsUnknownNotComplete)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap);

    // Before any submission every queue's token is absent.
    const auto preCompute = frame.SubmissionToken(RGQueue::Compute);
    ASSERT_FALSE(preCompute.IsValid()) << "nothing submitted yet";
    EXPECT_EQ(dev->QueryGpuSyncToken(preCompute), IDevice::GpuSyncStatus::Unknown)
        << "an absent token is absence of information, not proof of completion";
    EXPECT_FALSE(dev->WaitGpuSyncToken(preCompute, 0))
        << "waiting an absent token must not report success";

    // Positive companion: the same device answers Complete for a token it CAN
    // observe, so the Unknown above is a real verdict and not a dead instrument.
    BuildAndExecuteFrame(frame, 0);
    const auto gfx = frame.SubmissionToken(RGQueue::Graphics);
    ASSERT_TRUE(gfx.IsValid());
    dev->WaitForIdle();
    EXPECT_EQ(dev->QueryGpuSyncToken(gfx), IDevice::GpuSyncStatus::Complete);
}

// Residual review pin: a cross-queue consumer barrier must NOT replay the
// producer's stage mask (invalid on the consuming queue — the timeline wait
// provides ordering); the recorded source scope is a minimal mask valid on
// the consumer. This is the rule the record loop applies to every barrier
// whose SrcQueue differs from the submission's queue (G→C sampled depth).
TEST(RGRecord, CrossQueueConsumerSrcScopeIsConsumerQueueValid)
{
    uint64_t stage = 0, access = 0;

    CrossQueueConsumerSrcScope(RGQueue::Compute, stage, access);
    EXPECT_EQ(stage, static_cast<uint64_t>(PipelineStageMask::ComputeShader))
        << "graphics producer stages must not leak onto the compute queue";
    EXPECT_EQ(access, static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
    EXPECT_EQ(stage & (static_cast<uint64_t>(PipelineStageMask::GraphicsFragment) |
                       static_cast<uint64_t>(PipelineStageMask::GraphicsColor) |
                       static_cast<uint64_t>(PipelineStageMask::GraphicsDepth) |
                       static_cast<uint64_t>(PipelineStageMask::GraphicsVertex)),
              0u);

    CrossQueueConsumerSrcScope(RGQueue::Graphics, stage, access);
    EXPECT_EQ(stage, static_cast<uint64_t>(PipelineStageMask::GraphicsFragment));
    EXPECT_EQ(access, static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
    EXPECT_EQ(stage & static_cast<uint64_t>(PipelineStageMask::ComputeShader), 0u);

    // Transfer queues accept neither shader stages nor shader accesses.
    CrossQueueConsumerSrcScope(RGQueue::Transfer, stage, access);
    EXPECT_EQ(stage, static_cast<uint64_t>(PipelineStageMask::Transfer));
    EXPECT_EQ(access, static_cast<uint64_t>(ResourceAccessMask::TransferRead));
}

// The per-layer cascade contract at the RECORDING level: a sub-range
// AttachDepth must reach the backend as a custom attachment view. The
// declaration-level Attachments() recs cannot prove this — barrier gen
// tracked per-cell correctly while the recorded pass silently bound a
// default whole-array view and every cascade rendered into layer 0.
TEST(RGRecord, SubRangeAttachReachesTheBackendAsACustomView)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    TextureDesc arrayDesc = DepthDesc();
    arrayDesc.arrayLayers = 4;
    RGTexture shadowArr = frame.ImportPersistentTexture("Rec.ShadowArr", arrayDesc);
    RGTexture color = frame.CreateTexture("Rec.Color", ColorDesc());

    RenderGraph::RGPass cascade0{};
    RenderGraph::RGPass cascade2{};
    cascade0 = frame.AddPass("Cascade0", 0,
                             [&](RGPassBuilder& p)
                             {
                                 p.AttachDepth(shadowArr,
                                               {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}},
                                               RGDepthAccess::ReadWrite,
                                               RGRange{.BaseLayer = 0, .LayerCount = 1});
                             },
                             [](RGContext&) {});
    cascade2 = frame.AddPass("Cascade2", 0,
                             [&](RGPassBuilder& p)
                             {
                                 p.AttachDepth(shadowArr,
                                               {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}},
                                               RGDepthAccess::ReadWrite,
                                               RGRange{.BaseLayer = 2, .LayerCount = 1});
                             },
                             [](RGContext&) {});
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                      p.Read(shadowArr, RGTextureRead::Sampled);
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    // Recover each pass's contiguous attachment range, then assert the EXACT
    // desc handed to the backend.
    auto descFor = [&](RGPassId pass, RenderPassDesc& out)
    {
        const auto& atts = frame.Attachments();
        uint32_t first = 0, count = 0;
        for (uint32_t i = 0; i < atts.size(); ++i)
        {
            if (atts[i].Pass != pass)
                continue;
            if (count == 0)
                first = i;
            ++count;
        }
        ASSERT_GT(count, 0u);
        frame.BuildRenderPassDesc(first, count, out);
    };

    RenderPassDesc d0{};
    descFor(cascade0.Id, d0);
    ASSERT_TRUE(d0.useDepthView) << "layer-0 single-layer attach of a 4-layer array IS a sub-range";
    EXPECT_EQ(d0.depthViewDesc.baseLayer, 0u);
    EXPECT_EQ(d0.depthViewDesc.layerCount, 1u);
    EXPECT_EQ(d0.depthViewDesc.aspect, TextureAspect::Depth);
    EXPECT_EQ(d0.depthViewDesc.viewType, TextureViewType::View2D);

    RenderPassDesc d2{};
    descFor(cascade2.Id, d2);
    ASSERT_TRUE(d2.useDepthView);
    EXPECT_EQ(d2.depthViewDesc.baseLayer, 2u) << "cascade 2 must NOT render into layer 0";
    EXPECT_EQ(d2.depthViewDesc.layerCount, 1u);

    // Whole-resource attaches keep the default view (no spurious custom views).
    RenderPassDesc dw{};
    {
        const auto& atts = frame.Attachments();
        for (uint32_t i = 0; i < atts.size(); ++i)
        {
            if (atts[i].Tex == color.Id)
            {
                frame.BuildRenderPassDesc(i, 1, dw);
                break;
            }
        }
    }
    EXPECT_FALSE(dw.useColorView[0]) << "whole-resource attach needs no custom view";
}

// Declared access is what the barriers are derived from, so the RECORDED ops
// must not exceed it. A read-only depth attach declares a DepthRead and no
// write; a StoreOp::Store there is a DEPTH_STENCIL_ATTACHMENT_WRITE at
// LATE_FRAGMENT_TESTS performed by vkCmdEndRendering that no declared access
// covers, and the next depth writer's barrier — built from a read-only producer
// scope (see the RGBarrier twin) — carries only DEPTH_STENCIL_ATTACHMENT_READ
// in its source scope. That is the every-frame WAW the editor's game view hit
// between the world pass and the phase-B depth recover pass.
TEST(RGRecord, ReadOnlyDepthAttachRecordsNoStore)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture depth = frame.CreateTexture("RO.Depth", DepthDesc());
    RGTexture color = frame.CreateTexture("RO.Color", ColorDesc());

    const RenderGraph::RGPass prepass =
        frame.AddPass("Prepass", 0,
                      [&](RGPassBuilder& p)
                      { p.AttachDepth(depth, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}}); },
                      [](RGContext&) {});
    // Store is spelled out exactly as the world pass spells it: the attach, not
    // the caller, is what has to drop it.
    const RenderGraph::RGPass world =
        frame.AddPass("World", 0,
                      [&](RGPassBuilder& p)
                      {
                          p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                          p.AttachDepth(depth, {.Load = RGLoadOp::Load, .Store = RGStoreOp::Store},
                                        RGDepthAccess::ReadOnly);
                      },
                      [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    auto descFor = [&](RGPassId pass, RenderPassDesc& out)
    {
        const auto& atts = frame.Attachments();
        uint32_t first = 0, count = 0;
        for (uint32_t i = 0; i < atts.size(); ++i)
        {
            if (atts[i].Pass != pass)
                continue;
            if (count == 0)
                first = i;
            ++count;
        }
        ASSERT_GT(count, 0u);
        frame.BuildRenderPassDesc(first, count, out);
    };

    RenderPassDesc worldDesc{};
    descFor(world.Id, worldDesc);
    EXPECT_TRUE(worldDesc.depthReadOnly);
    EXPECT_EQ(worldDesc.depthStoreOp, RenderPassDesc::StoreOp::None)
        << "a read-only depth attach must record no attachment write";
    EXPECT_EQ(worldDesc.stencilStoreOp, RenderPassDesc::StoreOp::None)
        << "read-only covers both aspects of a combined-format target";

    // The declaration-level rec carries it too, and a depth-WRITING attach is
    // untouched — the rule is scoped to read-only, not to depth.
    const RGAttachmentRec* worldDepth = nullptr;
    const RGAttachmentRec* prepassDepth = nullptr;
    for (const RGAttachmentRec& rec : frame.Attachments())
    {
        if (!rec.IsDepth)
            continue;
        if (rec.Pass == world.Id)
            worldDepth = &rec;
        else if (rec.Pass == prepass.Id)
            prepassDepth = &rec;
    }
    ASSERT_NE(worldDepth, nullptr);
    ASSERT_NE(prepassDepth, nullptr);
    EXPECT_EQ(worldDepth->Ops.Store, RGStoreOp::None);
    EXPECT_EQ(prepassDepth->Ops.Store, RGStoreOp::Store);

    RenderPassDesc prepassDesc{};
    descFor(prepass.Id, prepassDesc);
    EXPECT_FALSE(prepassDesc.depthReadOnly);
    EXPECT_EQ(prepassDesc.depthStoreOp, RenderPassDesc::StoreOp::Store);
}

// A pass can BOTH fork its interior into a worker secondary and attach depth
// read-only — the world pass and the transmittance shadow pass do exactly that.
// It matters because the two facts interact: a render pass filled by secondaries
// records no state on the primary, so the secondary is the only place
// vkCmdSetDepthWriteEnable(VK_FALSE) is issued, and without it an opaque
// pipeline's compiled depthWriteEnable=true writes an attachment whose
// STORE_OP_NONE only preserves contents while nothing writes it. RGFrame's fork
// phase reads depthReadOnly off this same RenderPassDesc, so this pins the value
// it conveys. The fork itself needs a job pool and the GE_PARALLEL_RECORD kill
// switch, neither of which a headless graph test can turn on.
TEST(RGRecord, ForkedPassCanAttachDepthReadOnly)
{
    auto dev = MakeDevice(false);
    if (!dev)
        GTEST_SKIP() << "no headless device available";
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture depth = frame.CreateTexture("Fork.Depth", DepthDesc());
    RGTexture color = frame.CreateTexture("Fork.Color", ColorDesc());

    const RenderGraph::RGPass prepass =
        frame.AddPass("Prepass", 0,
                      [&](RGPassBuilder& p)
                      {
                          p.RecordInSecondary();
                          p.AttachDepth(depth, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}});
                      },
                      [](RGContext&) {});
    const RenderGraph::RGPass world =
        frame.AddPass("World", 0,
                      [&](RGPassBuilder& p)
                      {
                          p.RecordInSecondary();
                          p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                          p.AttachDepth(depth, {.Load = RGLoadOp::Load}, RGDepthAccess::ReadOnly);
                      },
                      [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    auto descFor = [&](RGPassId pass, RenderPassDesc& out)
    {
        const auto& atts = frame.Attachments();
        uint32_t first = 0, count = 0;
        for (uint32_t i = 0; i < atts.size(); ++i)
        {
            if (atts[i].Pass != pass)
                continue;
            if (count == 0)
                first = i;
            ++count;
        }
        ASSERT_GT(count, 0u);
        frame.BuildRenderPassDesc(first, count, out);
    };

    RenderPassDesc worldDesc{};
    descFor(world.Id, worldDesc);
    EXPECT_TRUE(worldDesc.depthReadOnly)
        << "a forked pass reaching a read-only depth attach is the case the "
           "secondary's depth-write-enable state exists for";
    EXPECT_EQ(worldDesc.depthStoreOp, RenderPassDesc::StoreOp::None);

    RenderPassDesc prepassDesc{};
    descFor(prepass.Id, prepassDesc);
    EXPECT_FALSE(prepassDesc.depthReadOnly)
        << "a forked DEPTH-WRITING pass must still take the pipeline's own "
           "depthWriteEnable";
}

// The backend half of the same story: the recorder hands the command list
// StoreOp::None, and VK_ATTACHMENT_STORE_OP_NONE preserves an attachment's
// contents only for as long as nothing writes it during the render pass. What
// suppresses the writes is vkCmdSetDepthWriteEnable(VK_FALSE), which SetPipeline
// issues only inside a dynamic render pass or a secondary inheriting one. A
// LEGACY render pass leaves every pipeline's compiled depthWriteEnable in force,
// so NONE there would leave the depth contents undefined — and the device
// version alone does not rule that combination out: the minimal-fallback path in
// VulkanDevice::CreateLogicalDevice clears dynamic-rendering support without
// lowering the reported apiVersion. Degrade to STORE, which preserves the
// contents; DONT_CARE would discard depth a later pass still samples.
TEST(RGRecord, StoreOpNoneDegradesToStoreWhenWritesAreNotSuppressed)
{
    using SO = RenderPassDesc::StoreOp;

    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::None, /*storeOpNoneUsable=*/true),
              VK_ATTACHMENT_STORE_OP_NONE);
    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::None, /*storeOpNoneUsable=*/false),
              VK_ATTACHMENT_STORE_OP_STORE)
        << "NONE without the write suppression must fall back to STORE, not DONT_CARE";

    // The caller's own choice is passed through on both paths.
    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::Store, true), VK_ATTACHMENT_STORE_OP_STORE);
    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::Store, false), VK_ATTACHMENT_STORE_OP_STORE);
    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::DontCare, true),
              VK_ATTACHMENT_STORE_OP_DONT_CARE);
    EXPECT_EQ(VulkanCommandList::ResolveStoreOp(SO::DontCare, false),
              VK_ATTACHMENT_STORE_OP_DONT_CARE);
}
