// Stage 2b-3 increment 1: the RenderGraph public frame layer over the REAL device —
// typed handles, lambda-scoped declaration, attachment ops, Execute() with
// post-cull realization from the pools, scheduled lambda invocation, and the
// C3 cross-frame pool state write-back.

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGWindowFrame.h"
#include "Tests/RenderGraph/RGTestDevice.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine::Rendering;     // TextureDesc/BufferDesc/handles/enums/IDevice
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;

namespace
{
TextureDesc ColorDesc(uint32_t w = 64, uint32_t h = 64)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}
} // namespace

#define RG_REQUIRE_DEVICE(dev)                                                                        \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
    GTEST_SKIP() << "no headless device available"

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};

TEST(RGFrame, ExecuteRunsLambdasInScheduledOrderWithRealizedResources)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    std::vector<std::string> ran;
    RGTexture colorA = frame.CreateTexture("ColorA", ColorDesc());
    RGTexture colorB = frame.CreateTexture("ColorB", ColorDesc());

    // Recording-order contract: producers are declared before their consumers
    // (declaring a read before the resource's write is a WAR — "read previous
    // contents" — by design). An unrelated pass declared FIRST must not disturb
    // the chain, and execution must follow the schedule, not raw declaration.
    RGTexture colorC = frame.CreateTexture("ColorC", ColorDesc());
    frame.AddPass("Other", 0, [&](RGPassBuilder& p) { p.AttachColor(0, colorC); },
                  [&](RGContext&) { ran.push_back("Other"); });
    frame.AddPass("A", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachColor(0, colorA, {.Load = RGLoadOp::Clear, .Clear = {.Color = {0, 0, 0, 1}}}); },
                  [&, colorA](RGContext& ctx)
                  {
                      ran.push_back("A");
                      EXPECT_TRUE(ctx.GetTexture(colorA).IsValid());
                  });
    frame.AddPass("B", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(colorA); // default: sampled
                      p.AttachColor(0, colorB, {.Load = RGLoadOp::DontCare});
                  },
                  [&, colorA, colorB](RGContext& ctx)
                  {
                      ran.push_back("B");
                      EXPECT_TRUE(ctx.GetTexture(colorA).IsValid());
                      EXPECT_TRUE(ctx.GetTexture(colorB).IsValid());
                  });
    frame.MarkOutput(colorB);
    frame.MarkOutput(colorC);
    frame.Execute();

    ASSERT_EQ(ran.size(), 3u);
    // A->B contiguous and ordered (one component); Other is its own component.
    const auto posOf = [&](const char* n)
    {
        for (size_t i = 0; i < ran.size(); ++i)
            if (ran[i] == n)
                return static_cast<int>(i);
        return -1;
    };
    EXPECT_EQ(posOf("B"), posOf("A") + 1) << "consumer runs right after its producer";
    EXPECT_EQ(pools.Transient.TexturePoolSize(), 3u);
    EXPECT_EQ(pools.Transient.Allocs(), 3u);
}

TEST(RGFrame, CulledPassNeverRunsAndAllocatesNothing)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    bool deadRan = false;
    bool liveRan = false;
    RGTexture live = frame.CreateTexture("Live", ColorDesc());
    RGTexture dead = frame.CreateTexture("Dead", ColorDesc(2048, 2048)); // would be expensive
    frame.AddPass("Dead", 0, [&](RGPassBuilder& p) { p.AttachColor(0, dead); },
                  [&](RGContext&) { deadRan = true; });
    frame.AddPass("Live", 0, [&](RGPassBuilder& p) { p.AttachColor(0, live); },
                  [&](RGContext&) { liveRan = true; });
    frame.MarkOutput(live);
    frame.Execute();

    EXPECT_TRUE(liveRan);
    EXPECT_FALSE(deadRan) << "culled pass lambda must never run";
    EXPECT_EQ(pools.Transient.TexturePoolSize(), 1u) << "culled pass's texture never realized";
    EXPECT_EQ(frame.Graph().CullReason(RGPassId{0}), RGCullReason::NoConsumer);
}

TEST(RGFrame, AllCulledRunsNothingSafely)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    bool ran = false;
    RGTexture t = frame.CreateTexture("Orphan", ColorDesc());
    frame.AddPass("Orphan", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t); },
                  [&](RGContext&) { ran = true; });
    // No MarkOutput — the no-sink trap. Execute must warn (log) and run nothing.
    frame.Execute();

    EXPECT_FALSE(ran);
    EXPECT_EQ(pools.Transient.TexturePoolSize(), 0u);
}

// Declare-side history (the per-view temporal sample) may only promote a frame's
// state to "previous" once that frame was submitted, and this count is its
// evidence: it moves exactly once per Execute that recorded a live pass, and not
// at all for a frame declared and then abandoned before Execute, or whose every
// pass was culled.
TEST(RGFrame, SubmittedFrameCountMovesOnlyWhenExecuteRecordsALivePass)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    EXPECT_EQ(frame.SubmittedFrameCount(), 0u);

    auto declareLive = [&](uint64_t frameIndex)
    {
        frame.BeginFrame(frameIndex);
        RGTexture live = frame.CreateTexture("Live", ColorDesc());
        frame.AddPass("Live", 0, [&](RGPassBuilder& p) { p.AttachColor(0, live); },
                      [](RGContext&) {});
        frame.MarkOutput(live);
    };

    declareLive(0);
    frame.Execute();
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u);

    // Declared and abandoned: the host returns before Execute.
    declareLive(1);
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u) << "a declared frame is not a submitted one";

    // Every pass culled (no sink): Execute records and submits nothing.
    frame.BeginFrame(2);
    RGTexture orphan = frame.CreateTexture("Orphan", ColorDesc());
    frame.AddPass("Orphan", 0, [&](RGPassBuilder& p) { p.AttachColor(0, orphan); },
                  [](RGContext&) {});
    frame.Execute();
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u) << "an all-culled Execute submitted nothing";

    declareLive(3);
    frame.Execute();
    EXPECT_EQ(frame.SubmittedFrameCount(), 2u) << "the count resumes on the next submitted frame";
}

// Transfer usage on a transient is DERIVED from the frame's own declared copies,
// never restated in the desc. The pool keys on the whole desc, so the derived bit
// is observable: two otherwise-identical descs pool apart exactly when one of them
// is copied.
TEST(RGFrame, TransientCopyAccessDerivesTransferUsage)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    auto declareCopiedPair = [&](RGTexture& src, RGTexture& dst)
    {
        src = frame.CreateTexture("Scene", ColorDesc());
        dst = frame.CreateTexture("Snapshot", ColorDesc());
        frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, src); },
                      [](RGContext&) {});
        frame.AddPass("Copy", 1,
                      [&](RGPassBuilder& p)
                      {
                          p.Read(src, RGTextureRead::CopySrc);
                          p.Write(dst, RGTextureWrite::CopyDst);
                      },
                      [](RGContext&) {});
        frame.MarkOutput(dst);
    };

    // Frame 0 — nothing copies it. The realized desc is the declared one.
    frame.BeginFrame(0);
    RGTexture plain = frame.CreateTexture("Scene", ColorDesc());
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, plain); },
                  [](RGContext&) {});
    frame.MarkOutput(plain);
    frame.Execute();
    ASSERT_EQ(pools.Transient.Allocs(), 1u);

    // Frame 1 — same descs, but one is read as a copy source and one written as a
    // copy destination. Neither can match frame 0's entry.
    frame.BeginFrame(1);
    RGTexture src1{};
    RGTexture dst1{};
    declareCopiedPair(src1, dst1);
    frame.Execute();
    EXPECT_EQ(pools.Transient.Allocs(), 3u)
        << "the copy source and destination each need a usage frame 0's entry lacks";

    // Frame 2 — identical shape: the derived descs match the pooled entries, so
    // derivation is stable rather than per-frame churn.
    const uint64_t hitsBefore = pools.Transient.Hits();
    frame.BeginFrame(2);
    RGTexture src2{};
    RGTexture dst2{};
    declareCopiedPair(src2, dst2);
    frame.Execute();
    EXPECT_EQ(pools.Transient.Allocs(), 3u) << "derived usage must be stable across frames";
    EXPECT_EQ(pools.Transient.Hits(), hitsBefore + 2u);
}

// A culled copy never records, so it requires nothing: the derivation must read
// cull state, not raw declaration, or every abandoned capture would widen a target.
TEST(RGFrame, CulledCopyPassDerivesNoTransferUsage)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    frame.BeginFrame(0);
    RGTexture plain = frame.CreateTexture("Scene", ColorDesc());
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, plain); },
                  [](RGContext&) {});
    frame.MarkOutput(plain);
    frame.Execute();
    ASSERT_EQ(pools.Transient.Allocs(), 1u);

    // Frame 1: the copy destination reaches no sink, so the copy pass is culled.
    frame.BeginFrame(1);
    RGTexture live = frame.CreateTexture("Scene", ColorDesc());
    RGTexture orphan = frame.CreateTexture("Snapshot", ColorDesc());
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, live); },
                  [](RGContext&) {});
    frame.AddPass("Copy", 1,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(live, RGTextureRead::CopySrc);
                      p.Write(orphan, RGTextureWrite::CopyDst);
                  },
                  [](RGContext&) {});
    frame.MarkOutput(live);
    frame.Execute();

    ASSERT_EQ(frame.Graph().CullReason(RGPassId{1}), RGCullReason::NoConsumer);
    EXPECT_EQ(pools.Transient.Allocs(), 1u) << "a culled copy must not widen its source";
    EXPECT_EQ(pools.Transient.Hits(), 1u) << "frame 0's entry must still match";
}

// The import twin of TransientCopyAccessDerivesTransferUsage. An import's
// physical predates the copy's declaration, so the derived usage cannot reach
// it in the same frame; the pool carries it into the NEXT materialization —
// one realloc, observable as a new physical — and then holds steady.
TEST(RGFrame, PersistentImportCopyAccessWidensThePoolEntryForTheNextFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const TextureDesc desc = ColorDesc();
    auto drawInto = [&](RGTexture target)
    {
        frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, target); },
                      [](RGContext&) {});
    };
    auto copyOut = [&](RGTexture src)
    {
        RGTexture dst = frame.CreateTexture("Snapshot", desc);
        frame.AddPass("Copy", 1,
                      [&](RGPassBuilder& p)
                      {
                          p.Read(src, RGTextureRead::CopySrc);
                          p.Write(dst, RGTextureWrite::CopyDst);
                      },
                      [](RGContext&) {});
        frame.MarkOutput(dst);
    };

    // Frame 0 — imported and drawn; nothing copies it.
    frame.BeginFrame(0);
    RGTexture t0 = frame.ImportPersistentTexture("Output", desc);
    drawInto(t0);
    frame.MarkOutput(t0);
    frame.Execute();
    const TextureHandle h0 = frame.PhysicalTexture(t0);
    ASSERT_TRUE(h0.IsValid());

    // Frame 1 — copied. This frame's physical is still frame 0's: it was
    // realized at import, before the copy was declared.
    frame.BeginFrame(1);
    RGTexture t1 = frame.ImportPersistentTexture("Output", desc);
    drawInto(t1);
    copyOut(t1);
    EXPECT_EQ(frame.PhysicalTexture(t1), h0) << "an import's physical cannot change after declaration";
    frame.Execute();
    EXPECT_EQ(pools.Persistent.DeferredCount(), 0u);

    // Frame 2 — the pool applies what frame 1 derived: a new physical.
    frame.BeginFrame(2);
    RGTexture t2 = frame.ImportPersistentTexture("Output", desc);
    const TextureHandle h2 = frame.PhysicalTexture(t2);
    EXPECT_NE(h2, h0) << "the derived TransferSrc must reach the next materialization";
    EXPECT_EQ(pools.Persistent.DeferredCount(), 1u);
    drawInto(t2);
    copyOut(t2);
    frame.Execute();

    // Frame 3 — stable: the unwidened import desc matches the widened entry.
    frame.BeginFrame(3);
    RGTexture t3 = frame.ImportPersistentTexture("Output", desc);
    EXPECT_EQ(frame.PhysicalTexture(t3), h2) << "widening is one realloc, not per-frame churn";
    EXPECT_EQ(pools.Persistent.DeferredCount(), 1u);
    drawInto(t3);
    frame.MarkOutput(t3);
    frame.Execute();
    dev->WaitForIdle();
}

// A culled copy of an import widens nothing either: the write-back reads the
// same cull-aware derivation as the transient path.
TEST(RGFrame, CulledCopyOfAPersistentImportWidensNothing)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    const TextureDesc desc = ColorDesc();

    frame.BeginFrame(0);
    RGTexture t0 = frame.ImportPersistentTexture("Output", desc);
    RGTexture orphan = frame.CreateTexture("Snapshot", desc);
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t0); },
                  [](RGContext&) {});
    frame.AddPass("Copy", 1,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(t0, RGTextureRead::CopySrc);
                      p.Write(orphan, RGTextureWrite::CopyDst);
                  },
                  [](RGContext&) {});
    frame.MarkOutput(t0); // the copy's destination reaches no sink
    frame.Execute();
    ASSERT_EQ(frame.Graph().CullReason(RGPassId{1}), RGCullReason::NoConsumer);
    const TextureHandle h0 = frame.PhysicalTexture(t0);

    frame.BeginFrame(1);
    RGTexture t1 = frame.ImportPersistentTexture("Output", desc);
    EXPECT_EQ(frame.PhysicalTexture(t1), h0) << "a culled copy must not widen its import";
    EXPECT_EQ(pools.Persistent.DeferredCount(), 0u);
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t1); },
                  [](RGContext&) {});
    frame.MarkOutput(t1);
    frame.Execute();
    dev->WaitForIdle();
}

// A requirement stated for a graph-external consumer joins the derived usage.
// A transient is realized after Compile, so it carries it THIS frame and pools
// apart from an identical transient nothing requires — the same observable as
// TransientCopyAccessDerivesTransferUsage.
TEST(RGFrame, RequireTransferUsageRealizesATransientWithItThisFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    auto drawnOutput = [&](bool required)
    {
        RGTexture t = frame.CreateTexture("Output", ColorDesc());
        frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t); },
                      [](RGContext&) {});
        frame.MarkOutput(t);
        if (required)
            frame.RequireTransferUsage(t, TextureUsage::TransferSrc);
    };

    frame.BeginFrame(0);
    drawnOutput(/*required*/ false);
    frame.Execute();
    ASSERT_EQ(pools.Transient.Allocs(), 1u);

    frame.BeginFrame(1);
    drawnOutput(/*required*/ true);
    frame.Execute();
    EXPECT_EQ(pools.Transient.Allocs(), 2u) << "the required usage is part of the realized desc";

    frame.BeginFrame(2);
    drawnOutput(/*required*/ true);
    frame.Execute();
    EXPECT_EQ(pools.Transient.Allocs(), 2u) << "stable across frames";
    dev->WaitForIdle();
}

// A pool import's physical predates the statement, so the requirement reaches
// it exactly as a derived copy does: on the next materialization. Stated on
// frame 0 — before any copy exists — it makes frame 1's physical the widened
// one, so a capture declared on frame 1 copies a correctly declared image.
TEST(RGFrame, RequireTransferUsageWidensAPoolImportForTheNextFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    const TextureDesc desc = ColorDesc();

    auto drawnOutput = [&]()
    {
        RGTexture t = frame.ImportPersistentTexture("Output", desc);
        frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t); },
                      [](RGContext&) {});
        frame.MarkOutput(t);
        frame.RequireTransferUsage(t, TextureUsage::TransferSrc);
        return t;
    };

    frame.BeginFrame(0);
    const TextureHandle h0 = frame.PhysicalTexture(drawnOutput());
    frame.Execute();

    frame.BeginFrame(1);
    const TextureHandle h1 = frame.PhysicalTexture(drawnOutput());
    EXPECT_NE(h1, h0) << "frame 0's requirement must reach frame 1's physical";
    EXPECT_EQ(pools.Persistent.DeferredCount(), 1u);
    frame.Execute();

    frame.BeginFrame(2);
    EXPECT_EQ(frame.PhysicalTexture(drawnOutput()), h1) << "restating the requirement is free";
    EXPECT_EQ(pools.Persistent.DeferredCount(), 1u);
    frame.Execute();
    dev->WaitForIdle();
}

// C3 write-back proof: the pool carries the imported texture's end-of-frame
// layout into the next frame, so frame N+1's barrier transitions from the REAL
// prior layout instead of discarding via Undefined.
TEST(RGFrame, ImportPersistentStateRoundTripsAcrossFrames)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const TextureDesc histDesc = ColorDesc(128, 128);

    // Frame 0: import (fresh -> Undefined), render into it.
    frame.BeginFrame(0);
    RGTexture hist0 = frame.ImportPersistentTexture("TAA.History", histDesc);
    frame.AddPass("WriteHistory", 0, [&](RGPassBuilder& p) { p.AttachColor(0, hist0); },
                  [](RGContext&) {});
    frame.Execute();
    EXPECT_EQ(pools.Persistent.GetState("TAA.History"), ResourceState::RenderTarget)
        << "end-of-frame layout written back to the pool";

    // Frame 1: re-import — same physical, and the write barrier must start from
    // the carried ColorAttachment layout, not Undefined.
    frame.BeginFrame(1);
    RGTexture hist1 = frame.ImportPersistentTexture("TAA.History", histDesc);
    frame.AddPass("WriteHistoryAgain", 0, [&](RGPassBuilder& p) { p.AttachColor(0, hist1); },
                  [](RGContext&) {});
    frame.Execute();

    const RGBarrier* bar = nullptr;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == hist1.Id)
            bar = &b;
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->OldLayout, RGImageLayout::ColorAttachment)
        << "carried across frames via the pool, not Undefined";
    EXPECT_EQ(frame.PhysicalTexture(hist1), frame.PhysicalTexture(hist1)); // stable accessor
    EXPECT_EQ(pools.Persistent.Size(), 1u);
}

// The RGFrame leg of the texture freshness arm: MarkPersistentTextureInitialized
// discharges through the pool ONLY when the marking frame actually executes, and
// the discharge round-trips the graph's resource name back to the pool key.
TEST(RGFrame, PersistentTextureFreshnessDischargesOnlyThroughAnExecutedFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    // Frame 0 imports, marks, then is ABANDONED before Execute (the
    // swapchain-acquire-failure shape): nothing may discharge.
    frame.BeginFrame(0);
    bool fresh = false;
    RGTexture h0 = frame.ImportPersistentTexture("Fresh.History", ColorDesc(), &fresh);
    EXPECT_TRUE(fresh) << "first use is fresh";
    frame.MarkPersistentTextureInitialized(h0);

    // Frame 1: still fresh (the abandoned mark never executed); this time the
    // marking frame declares the write and executes.
    frame.BeginFrame(1);
    fresh = false;
    RGTexture h1 = frame.ImportPersistentTexture("Fresh.History", ColorDesc(), &fresh);
    EXPECT_TRUE(fresh) << "an abandoned marking frame must not discharge the arm";
    frame.MarkPersistentTextureInitialized(h1);
    frame.AddPass("WriteHistory", 0, [&](RGPassBuilder& p) { p.AttachColor(0, h1); },
                  [](RGContext&) {});
    frame.Execute();

    // Frame 2: the executed frame's mark reached the pool under the pool key.
    frame.BeginFrame(2);
    fresh = true;
    (void)frame.ImportPersistentTexture("Fresh.History", ColorDesc(), &fresh);
    EXPECT_FALSE(fresh) << "an executed marking frame discharges the arm";
    // Mutations: skip the m_InitializedTextureIds discharge loop in Execute ->
    // third assert red; discharge in MarkPersistentTextureInitialized instead
    // of Execute -> second assert red.
}

TEST(RGFrame, TypedUploadAllocIsWritable)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    struct CameraUBO
    {
        float View[16];
        float Proj[16];
    };
    auto cam = frame.AllocUpload<CameraUBO>();
    ASSERT_TRUE(cam.Valid());
    EXPECT_TRUE(cam.Buffer.IsValid());
    cam.Ptr->View[0] = 1.0f; // really writable, real mapped memory
    cam.Ptr->Proj[15] = 2.0f;
    EXPECT_EQ(pools.Ring.BytesUsedThisFrame(), sizeof(CameraUBO));
}

// External imports dedup by physical handle: one physical = one resource id =
// one hazard state. Two ids for the same image would each track layout
// independently — a silent race (the MSM-moments / skinning-atlas shape).
TEST(RGFrame, ExternalImportsDedupByPhysicalHandle)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const TextureHandle tex = dev->CreateTexture(ColorDesc());
    ASSERT_TRUE(tex.IsValid());
    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle buf = dev->CreateBuffer(bd);
    ASSERT_TRUE(buf.IsValid());

    frame.BeginFrame(0);
    RGTexture t1 = frame.ImportExternalTexture("MsmMoments", tex, ResourceState::Common);
    RGTexture t2 = frame.ImportExternalTexture("MsmMoments.WorldRead", tex, ResourceState::Common);
    EXPECT_EQ(t1.Id, t2.Id) << "same physical texture must dedup to one resource";
    RGBuffer b1 = frame.ImportExternalBuffer("SkinAtlas", buf);
    RGBuffer b2 = frame.ImportExternalBuffer("SkinAtlas.WorldRead", buf);
    EXPECT_EQ(b1.Id, b2.Id) << "same physical buffer must dedup to one resource";

    // Writer + reader through the two aliases share ONE hazard state: the
    // reader gets a real RAW barrier against the writer.
    frame.AddComputePass("Skin", 0, [&](RGPassBuilder& p) { p.Write(b1); }, [](RGContext&) {});
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(b2);
                      p.AttachColor(0, t1, {.Load = RGLoadOp::Clear});
                  },
                  [](RGContext&) {});
    frame.MarkOutput(t2);
    frame.Execute();
    dev->WaitForIdle();
    EXPECT_EQ(frame.PhysicalBuffer(b2), buf);
    EXPECT_EQ(frame.PhysicalTexture(t2), tex);

    bool sawRaw = false;
    for (const RGBarrier& bar : frame.Graph().Barriers())
        if (bar.Resource == b2.Id && (bar.SrcAccess & RGAccessMask::ShaderWrite) != 0)
            sawRaw = true;
    EXPECT_TRUE(sawRaw) << "aliased reader must chain from the writer (one hazard state)";

    dev->DestroyTexture(tex);
    dev->DestroyBuffer(buf);
}

// Pool imports dedup the same way: a producer arm and a consumer arm both
// importing one pool name in one frame (the shadow-array shape: cascade
// passes attach it, the world pass samples it) must share one resource id —
// and the consumer must chain from the producer with a real barrier.
TEST(RGFrame, PersistentImportsDedupWithinAFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    TextureDesc smDesc = ColorDesc();
    smDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    smDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    smDesc.arrayLayers = 4;

    RGTexture sm1 = frame.ImportPersistentTexture("ShadowMapArray.View1", smDesc);
    RGTexture sm2 = frame.ImportPersistentTexture("ShadowMapArray.View1", smDesc);
    EXPECT_EQ(sm1.Id, sm2.Id) << "same pool name = same physical = ONE resource id";
    EXPECT_EQ(pools.Persistent.Size(), 1u);

    RGTexture color = frame.CreateTexture("Color", ColorDesc());
    frame.AddPass("Cascade0", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachDepth(sm1, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}},
                                    RGDepthAccess::ReadWrite,
                                    RGRange{.BaseLayer = 0, .LayerCount = 1});
                  },
                  [](RGContext&) {});
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                      p.Read(sm2, RGTextureRead::Sampled);
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    bool sawRaw = false;
    for (const RGBarrier& bar : frame.Graph().Barriers())
        if (bar.Resource == sm2.Id && (bar.SrcAccess & RGAccessMask::DepthWrite) != 0 &&
            (bar.DstAccess & RGAccessMask::ShaderRead) != 0)
            sawRaw = true;
    EXPECT_TRUE(sawRaw) << "world's sample must chain from the cascade's depth write";
}

// PreventCulling on the builder: a pass whose output is consumed OUTSIDE the
// graph (CPU readback ring) must run without an in-graph sink.
TEST(RGFrame, BuilderPreventCullingKeepsSideEffectPass)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    BufferDesc bd;
    bd.size = 64;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle ring = dev->CreateBuffer(bd);
    RGBuffer readback = frame.ImportExternalBuffer("SdsmRing", ring);

    bool ran = false;
    frame.AddComputePass("DepthReduce", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Write(readback);
                             p.PreventCulling(); // CPU reads the ring; no in-graph consumer
                         },
                         [&](RGContext&) { ran = true; });
    frame.Execute();
    dev->WaitForIdle();
    EXPECT_TRUE(ran);

    frame.WaitForPendingWork(); // graph-scoped drain — must return, not hang
    dev->DestroyBuffer(ring);
}

// MarkOutput(finalLayout) replaces the old ExportTexture: the exported texture
// gets one trailing transition after its last access, and the pool write-back
// carries the exported layout — the UI samples the pooled physical next frame
// with no graph-external transition. THE scene-view compositing contract.
TEST(RGFrame, ExportLayoutTransitionsAndRoundTripsThroughThePool)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture out = frame.ImportPersistentTexture("SceneView.Output", ColorDesc());
    frame.AddPass("World", 0, [&](RGPassBuilder& p) { p.AttachColor(0, out, {.Load = RGLoadOp::Clear}); },
                  [](RGContext&) {});
    frame.MarkOutput(out, RGImageLayout::ShaderReadOnly);
    frame.Execute();
    dev->WaitForIdle();

    EXPECT_EQ(frame.Graph().FinalLayout(out.Id), RGImageLayout::ShaderReadOnly)
        << "export transition must update the tracked final layout";
    EXPECT_EQ(pools.Persistent.GetState("SceneView.Output"), ResourceState::ShaderResource)
        << "the pool carries the exported layout for next frame's UI sampling";

    const RGBarrier* exp = nullptr;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == out.Id && b.NewLayout == RGImageLayout::ShaderReadOnly)
            exp = &b;
    ASSERT_NE(exp, nullptr);
    EXPECT_EQ(exp->OldLayout, RGImageLayout::ColorAttachment);
    EXPECT_EQ(exp->SrcStage & RGStage::BottomOfPipe, 0u)
        << "export barriers must never be frame-front hoisted";
    EXPECT_TRUE((exp->DstAccess & RGAccessMask::ShaderRead) != 0);
}

// Export placement follows the queue that touched the resource: a LATER
// compute submission that never touches the exported texture does not move
// the export off the graphics submission that owns it — the derived
// placement (BarrierSubmissions) points at the toucher, not at
// whatever submission happens to end the frame.
TEST(RGFrame, ExportBatchRecordsOnTheQueueThatTouchedTheResource)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetPhysicalQueueMap(RGFrame::kIdentityQueueMap); // multi-submission shape pin
    frame.BeginFrame(0);

    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGTexture color = frame.CreateTexture("Color", ColorDesc());
    RGBuffer data = frame.CreateBuffer("Data", bd);
    RGBuffer analysis = frame.CreateBuffer("Analysis", bd);
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                      p.Write(data);
                  },
                  [](RGContext&) {});
    frame.AddComputePass("Analyze", 0, // final submission is COMPUTE; never touches color
                         [&](RGPassBuilder& p)
                         {
                             p.Read(data);
                             p.Write(analysis);
                         },
                         [](RGContext&) {});
    frame.MarkOutput(analysis);
    frame.MarkOutput(color, RGImageLayout::ShaderReadOnly);
    frame.Execute();
    dev->WaitForIdle(); // validation-relevant: barriers must be queue-valid

    const RGBarrierBatch* exportBatch = nullptr;
    for (const RGBarrierBatch& bb : frame.Graph().BarrierBatches())
        if (bb.Pass == kInvalidId)
            exportBatch = &bb;
    ASSERT_NE(exportBatch, nullptr) << "sentinel export batch exists";
    EXPECT_EQ(frame.Graph().FinalLayout(color.Id), RGImageLayout::ShaderReadOnly);
    const auto& subs = frame.Graph().Submissions();
    ASSERT_GE(subs.size(), 2u) << "frame really is multi-submission";
    ASSERT_EQ(exportBatch->Count, 1u);
    const uint32_t placement = frame.Graph().BarrierSubmissions()[exportBatch->First];
    ASSERT_LT(placement, subs.size());
    EXPECT_EQ(subs[placement].Queue, RGQueue::Graphics)
        << "the graphics pass owns the texture — the trailing compute submission must not attract the export";
}

TEST(RGFrame, AttachDepthResolveRecordsResolveTarget)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    TextureDesc depthMsaa = ColorDesc();
    depthMsaa.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthMsaa.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    depthMsaa.sampleCount = 4;
    TextureDesc depthResolved = depthMsaa;
    depthResolved.sampleCount = 1;
    depthResolved.usage =
        static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);

    RGTexture msaa = frame.CreateTexture("DepthMsaa", depthMsaa);
    RGTexture resolved = frame.CreateTexture("DepthResolved", depthResolved);
    frame.AddPass("DepthMsaaPass", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachDepthResolve(msaa, resolved, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}}); },
                  [](RGContext&) {});
    frame.MarkOutput(resolved);
    frame.Execute();
    dev->WaitForIdle();

    ASSERT_EQ(frame.Attachments().size(), 1u);
    const RGAttachmentRec& rec = frame.Attachments()[0];
    EXPECT_TRUE(rec.IsDepth);
    EXPECT_EQ(rec.Resolve, resolved.Id);
    EXPECT_TRUE(frame.PhysicalTexture(resolved).IsValid());
}

// LoadOp::Load consumes prior contents — the derived read must (a) keep the
// producer alive through cull and (b) put the READ access in the consumer's
// barrier scope (visibility, not just availability, of the producer's write).
TEST(RGFrame, DepthLoadAttachKeepsPrepassAliveAndReadVisible)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    TextureDesc depthDesc = ColorDesc();
    depthDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    RGTexture depth = frame.CreateTexture("Depth", depthDesc);
    RGTexture color = frame.CreateTexture("Color", ColorDesc());

    bool prepassRan = false;
    frame.AddPass("DepthPrepass", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachDepth(depth, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}}); },
                  [&](RGContext&) { prepassRan = true; });
    // World depth-tests against the prepass (Load) and writes — NOTHING else
    // reads depth. Without the derived read, depth is never "needed" and the
    // prepass culls (the silent z-prepass-discard bug class).
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear});
                      p.AttachDepth(depth, {.Load = RGLoadOp::Load});
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
    dev->WaitForIdle();

    EXPECT_TRUE(prepassRan) << "loaded depth must keep its producer alive";
    const RGBarrier* waw = nullptr;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == depth.Id && (b.SrcAccess & RGAccessMask::DepthWrite) != 0)
            waw = &b;
    ASSERT_NE(waw, nullptr) << "world chains from the prepass";
    EXPECT_TRUE((waw->DstAccess & RGAccessMask::DepthRead) != 0)
        << "the depth test READS prepass results — visibility scope required";
    EXPECT_TRUE((waw->DstAccess & RGAccessMask::DepthWrite) != 0);
}

TEST(RGFrame, BlendCompositeOverLoadGetsColorReadVisibility)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture scene = frame.CreateTexture("SceneColor", ColorDesc());
    bool worldRan = false;
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p) { p.AttachColor(0, scene, {.Load = RGLoadOp::Clear}); },
                  [&](RGContext&) { worldRan = true; });
    frame.AddPass("UiComposite", 0, // blends over the scene: Load, write-only before the fix
                  [&](RGPassBuilder& p) { p.AttachColor(0, scene, {.Load = RGLoadOp::Load}); },
                  [](RGContext&) {});
    frame.MarkOutput(scene);
    frame.Execute();
    dev->WaitForIdle();

    EXPECT_TRUE(worldRan) << "blend source must keep its producer";
    const RGBarrier* waw = nullptr;
    for (const RGBarrier& b : frame.Graph().Barriers())
        if (b.Resource == scene.Id && (b.SrcAccess & RGAccessMask::ColorWrite) != 0)
            waw = &b;
    ASSERT_NE(waw, nullptr);
    EXPECT_TRUE((waw->DstAccess & RGAccessMask::ColorRead) != 0)
        << "blending READS the attachment — ColorWrite alone is availability without visibility";
}

// Headless guard: without a swapchain, ImportBackbuffer must return an invalid
// handle and declare NOTHING — the rest of the frame proceeds normally.
TEST(RGFrame, ImportBackbufferIsGracefulWithoutASwapchain)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture bb = frame.ImportBackbuffer();
    EXPECT_FALSE(bb.IsValid());
    EXPECT_EQ(frame.Graph().ResourceCount(), 0u) << "no phantom resource declared";

    bool ran = false;
    RGTexture c = frame.CreateTexture("C", ColorDesc());
    frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, c, {.Load = RGLoadOp::Clear}); },
                  [&](RGContext&) { ran = true; });
    frame.MarkOutput(c);
    frame.Execute();
    dev->WaitForIdle();
    EXPECT_TRUE(ran);
}

// The backbuffer's queue contract. Pinned on the pure rule rather than through
// RGFrame, because the headless harness has no swapchain to import — the shape
// under test (a compute final blit writing the swapchain image) cannot be
// declared through ImportBackbuffer here at all.
TEST(RGFrame, BackbufferWrittenOffTheGraphicsQueueIsDetected)
{
    struct FinalBlit
    {
        RGPassId Pass = kInvalidId;
        RGResourceId Scene = kInvalidId;
        RGResourceId Backbuffer = kInvalidId;
    };
    // One graph shape, one variable: the queue the final blit is declared on.
    // Every arm also carries an ordinary async-compute pass that never touches
    // the backbuffer, so "the graph has a compute pass" cannot be what trips.
    auto declareFinalBlit = [](RGGraph& g, RGQueue queue)
    {
        g.BeginFrame();
        RGResourceDesc rd;
        rd.Kind = RGResourceKind::Texture;
        rd.Name = "SceneColor";
        FinalBlit blit;
        blit.Scene = g.CreateResource(rd);
        rd.Name = "Bloom";
        const RGResourceId bloom = g.CreateResource(rd);
        rd.Name = "Backbuffer";
        blit.Backbuffer = g.CreateResource(rd);
        g.MarkExternal(blit.Backbuffer);

        RGPassDesc async;
        async.Name = "Bloom";
        async.Queue = RGQueue::Compute;
        const RGPassId asyncPass = g.AddPass(async);
        g.Read(asyncPass, blit.Scene, RGAccess::StorageRead);
        g.Write(asyncPass, bloom, RGAccess::StorageWrite);

        RGPassDesc d;
        d.Name = "FinalBlit";
        d.Queue = queue;
        blit.Pass = g.AddPass(d);
        g.Read(blit.Pass, bloom, RGAccess::StorageRead);
        g.Write(blit.Pass, blit.Backbuffer, RGAccess::StorageWrite);
        return blit;
    };

    RGGraph compute;
    const FinalBlit onCompute = declareFinalBlit(compute, RGQueue::Compute);
    EXPECT_EQ(FindNonGraphicsBackbufferAccess(compute, onCompute.Backbuffer), onCompute.Pass);

    RGGraph transfer;
    const FinalBlit onTransfer = declareFinalBlit(transfer, RGQueue::Transfer);
    EXPECT_EQ(FindNonGraphicsBackbufferAccess(transfer, onTransfer.Backbuffer), onTransfer.Pass);

    // Control: the same graph with the blit on graphics is the supported shape,
    // async-compute pass and all.
    RGGraph graphics;
    const FinalBlit onGraphics = declareFinalBlit(graphics, RGQueue::Graphics);
    EXPECT_EQ(FindNonGraphicsBackbufferAccess(graphics, onGraphics.Backbuffer), kInvalidId);

    // Headless: nothing imported the swapchain, so the rule polices nothing.
    EXPECT_EQ(FindNonGraphicsBackbufferAccess(compute, kInvalidId), kInvalidId);
}

// Pin: two same-desc transients in ONE frame must realize to DISTINCT physical
// textures — the pool may only recycle across frames, never within one.
TEST(RGFrame, SameDescTransientsGetDistinctPhysicalsWithinAFrame)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture a = frame.CreateTexture("A", ColorDesc());
    RGTexture b = frame.CreateTexture("B", ColorDesc()); // identical desc
    frame.AddPass("WriteA", 0, [&](RGPassBuilder& p) { p.AttachColor(0, a); }, [](RGContext&) {});
    frame.AddPass("WriteB", 0, [&](RGPassBuilder& p) { p.AttachColor(0, b); }, [](RGContext&) {});
    frame.MarkOutput(a);
    frame.MarkOutput(b);
    frame.Execute();

    EXPECT_TRUE(frame.PhysicalTexture(a).IsValid());
    EXPECT_TRUE(frame.PhysicalTexture(b).IsValid());
    EXPECT_NE(frame.PhysicalTexture(a), frame.PhysicalTexture(b));
}

TEST(RGFrame, AttachmentOpsAreRecordedAndDriveLayouts)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    TextureDesc depthDesc = ColorDesc();
    depthDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);

    RGTexture color = frame.CreateTexture("Color", ColorDesc());
    RGTexture depth = frame.CreateTexture("Depth", depthDesc);
    frame.AddPass("World", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, color, {.Load = RGLoadOp::Clear, .Clear = {.Color = {0.1f, 0.2f, 0.3f, 1}}});
                      p.AttachDepth(depth, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}});
                  },
                  [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();

    ASSERT_EQ(frame.Attachments().size(), 2u);
    const RGAttachmentRec& c = frame.Attachments()[0];
    const RGAttachmentRec& d = frame.Attachments()[1];
    EXPECT_FALSE(c.IsDepth);
    EXPECT_EQ(c.Slot, 0u);
    EXPECT_EQ(c.Ops.Load, RGLoadOp::Clear);
    EXPECT_FLOAT_EQ(c.Ops.Clear.Color[2], 0.3f);
    EXPECT_TRUE(d.IsDepth);
    EXPECT_FLOAT_EQ(d.Ops.Clear.Depth, 0.0f); // reverse-Z far

    bool sawColorLayout = false;
    bool sawDepthLayout = false;
    for (const RGBarrier& b : frame.Graph().Barriers())
    {
        if (b.Resource == color.Id && b.NewLayout == RGImageLayout::ColorAttachment)
            sawColorLayout = true;
        if (b.Resource == depth.Id && b.NewLayout == RGImageLayout::DepthAttachment)
            sawDepthLayout = true;
    }
    EXPECT_TRUE(sawColorLayout);
    EXPECT_TRUE(sawDepthLayout);
}

// ── Capture-by-name (the debug server's capture_resource path) ───────────────

// The lookup is KIND-CHECKED: a name that belongs to a buffer must never come
// back as a texture, because the caller mints a typed handle from the result
// and would otherwise read a buffer's id as an image.
TEST(RGFrame, FindByNameIsKindCheckedAndExact)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RGTexture color = frame.CreateTexture("Capture.Color", ColorDesc());
    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const RGBuffer data = frame.CreateBuffer("Capture.Data", bd);

    EXPECT_EQ(frame.FindTexture("Capture.Color").Id, color.Id);
    EXPECT_EQ(frame.FindBuffer("Capture.Data").Id, data.Id);

    // Wrong kind for the name -> invalid, never the other kind's id.
    EXPECT_FALSE(frame.FindTexture("Capture.Data").IsValid());
    EXPECT_FALSE(frame.FindBuffer("Capture.Color").IsValid());

    // Exact match only; no prefix/suffix/empty leniency at this layer.
    EXPECT_FALSE(frame.FindTexture("Capture").IsValid());
    EXPECT_FALSE(frame.FindTexture("Capture.Color.Extra").IsValid());
    EXPECT_FALSE(frame.FindTexture("").IsValid());
    EXPECT_FALSE(frame.FindTexture(nullptr).IsValid());
}

// Imports carry names into the same keyspace as transients — the capture path
// must reach a persistent history target, not just this frame's scratch.
TEST(RGFrame, FindByNameSeesImportedResources)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RGTexture history = frame.ImportPersistentTexture("View.TAA.History", ColorDesc());
    ASSERT_TRUE(history.IsValid());
    EXPECT_EQ(frame.FindTexture("View.TAA.History").Id, history.Id);

    BufferDesc bd;
    bd.size = 128;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle external = dev->CreateBuffer(bd);
    const RGBuffer imported = frame.ImportExternalBuffer("Scene.Exposure", external);
    EXPECT_EQ(frame.FindBuffer("Scene.Exposure").Id, imported.Id);

    frame.Execute();
    dev->WaitForIdle();
    dev->DestroyBuffer(external);
}

// The property the whole capture path rests on: declaring a CopySrc read of a
// target nothing else consumes is what makes that target live, so its producer
// survives culling and the captured bytes are the ones the producer wrote.
// The control arm proves the producer really would have been culled — without
// it this test would pass on a graph that never culls anything.
TEST(RGFrame, CapturingAnUnconsumedTargetKeepsItsProducerAlive)
{
    RG_REQUIRE_DEVICE(dev);

    auto runFrame = [&dev](bool declareCapture)
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        const RGTexture target = frame.CreateTexture("Capture.Orphan", ColorDesc());
        bool producerRan = false;
        frame.AddPass(
            "Producer", 0,
            [&](RGPassBuilder& p)
            {
                RGAttachmentOps ops{};
                ops.Load = RGLoadOp::Clear;
                p.AttachColor(0, target, ops);
            },
            [&](RGContext&) { producerRan = true; });

        if (declareCapture)
        {
            // Exactly what the readback helper declares, reached by name.
            const RGTexture found = frame.FindTexture("Capture.Orphan");
            EXPECT_EQ(found.Id, target.Id);
            frame.AddPass(
                "RGReadback.Capture.Orphan", static_cast<int32_t>(PassPhase::kFinalize),
                [&](RGPassBuilder& p)
                {
                    p.Read(found, RGTextureRead::CopySrc);
                    p.PreventCulling();
                },
                [](RGContext&) {});
        }

        frame.Execute();
        dev->WaitForIdle();
        return producerRan;
    };

    EXPECT_FALSE(runFrame(/*declareCapture*/ false))
        << "control: an unconsumed producer must be culled, else this test proves nothing";
    EXPECT_TRUE(runFrame(/*declareCapture*/ true))
        << "capturing a target must keep its producer alive";
}

TEST(RGWindowFrame, MakeWindowRGFrameBuildsAFrameStreamThatExecutes)
{
    RG_REQUIRE_DEVICE(dev);
    RGWindowFrame window = MakeWindowRGFrame(*dev);
    ASSERT_NE(window.PersistentPool, nullptr);
    ASSERT_NE(window.TransientPool, nullptr);
    ASSERT_NE(window.UploadRing, nullptr);
    ASSERT_NE(window.Frame, nullptr);

    bool ran = false;
    for (uint64_t frameIndex = 0; frameIndex < 2; ++frameIndex)
    {
        ran = false;
        window.Frame->BeginFrame(frameIndex);
        RGTexture color = window.Frame->CreateTexture("Color", ColorDesc());
        window.Frame->AddPass("Clear", 0,
                              [&](RGPassBuilder& p)
                              { p.AttachColor(0, color, {.Load = RGLoadOp::Clear}); },
                              [&](RGContext&) { ran = true; });
        window.Frame->MarkOutput(color);
        window.Frame->Execute();
        EXPECT_TRUE(ran) << "frame " << frameIndex;
    }
}
