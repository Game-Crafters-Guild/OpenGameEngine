// Stage 1.3 isolation tests for the RenderGraph submission plan + cross-queue timeline
// sync. Pure logic, no GPU.

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace GameEngine::Rendering::RenderGraph;

namespace
{
RGResourceId Tex(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.Name = name;
    return g.CreateResource(d);
}
RGResourceId Buf(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Buffer;
    d.SizeBytes = 256;
    d.Name = name;
    return g.CreateResource(d);
}
RGResourceId AccelerationStructure(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::AccelerationStructure;
    d.Name = name;
    return g.CreateResource(d);
}
// Texture whose physical sampled descriptors claim GENERAL (storage co-use).
RGResourceId TexGeneral(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.SampledInGeneralLayout = true;
    d.Name = name;
    return g.CreateResource(d);
}
const RGSemaphoreWait* FindWait(const RGSubmission& s, RGQueue producerQueue)
{
    for (uint32_t i = 0; i < s.WaitCount; ++i)
        if (s.Waits[i].Queue == static_cast<uint32_t>(producerQueue))
            return &s.Waits[i];
    return nullptr;
}
void Bake(RGGraph& g)
{
    g.Compile();
    g.Schedule();
    g.GenerateBarriers(RGGraph::kIdentityQueueMap);
    g.BuildSubmissionPlan();
}
// The frame's export/normalize sentinel batch (Pass == kInvalidId), or nullptr.
const RGBarrierBatch* ExportBatch(const RGGraph& g)
{
    for (const RGBarrierBatch& bb : g.BarrierBatches())
        if (bb.Pass == kInvalidId)
            return &bb;
    return nullptr;
}
// Global barrier indices (into Barriers()/BarrierSubmissions()) of every layout
// transition recorded on `res`.
std::vector<uint32_t> TransitionsOf(const RGGraph& g, RGResourceId res)
{
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < g.Barriers().size(); ++i)
    {
        const RGBarrier& b = g.Barriers()[i];
        if (b.Resource == res && b.IsTexture && b.OldLayout != b.NewLayout)
            out.push_back(i);
    }
    return out;
}
// Total timeline waits the plan derived across every submission — the semaphore
// traffic a placement change must leave untouched.
uint32_t TotalWaits(const RGGraph& g)
{
    uint32_t total = 0;
    for (const RGSubmission& s : g.Submissions())
        total += s.WaitCount;
    return total;
}
} // namespace

TEST(RGSubmission, SingleGraphicsQueueIsOneSubmissionWithNoWaits)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId depth = Tex(g, "Depth");
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color);
    RGPassId pre = g.AddPass({"DepthPrepass"});
    g.Write(pre, depth, RGAccess::DepthWrite);
    RGPassId world = g.AddPass({"World"});
    g.Read(world, depth, RGAccess::DepthRead);
    g.Write(world, color, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 1u);
    EXPECT_EQ(g.Submissions()[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(g.Submissions()[0].ScheduledCount, 2u);
    EXPECT_EQ(g.Submissions()[0].WaitCount, 0u);
    EXPECT_EQ(g.Submissions()[0].SignalValue, 1u);
}

TEST(RGSubmission, AsyncComputeForkJoinProducesTimelineWaits)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId depth = Tex(g, "Depth");
    RGResourceId lights = Buf(g, "LightLists");
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color);

    RGPassId pre = g.AddPass({"DepthPrepass", 0, RGQueue::Graphics});
    g.Write(pre, depth, RGAccess::DepthWrite);
    RGPassId cull = g.AddPass({"ClusterCull", 0, RGQueue::Compute});
    g.Read(cull, depth, RGAccess::Sampled);
    g.Write(cull, lights, RGAccess::StorageWrite);
    RGPassId world = g.AddPass({"World", 0, RGQueue::Graphics});
    g.Read(world, lights, RGAccess::StorageRead);
    g.Write(world, color, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const RGSubmission& s0 = g.Submissions()[0]; // graphics: depth
    const RGSubmission& s1 = g.Submissions()[1]; // compute: cull
    const RGSubmission& s2 = g.Submissions()[2]; // graphics: world
    EXPECT_EQ(s0.Queue, RGQueue::Graphics);
    EXPECT_EQ(s1.Queue, RGQueue::Compute);
    EXPECT_EQ(s2.Queue, RGQueue::Graphics);

    EXPECT_EQ(s0.WaitCount, 0u);
    // cull waits on the graphics depth submission.
    const RGSemaphoreWait* w1 = FindWait(s1, RGQueue::Graphics);
    ASSERT_NE(w1, nullptr);
    EXPECT_EQ(w1->Value, s0.SignalValue);
    // world waits on the compute cull submission.
    const RGSemaphoreWait* w2 = FindWait(s2, RGQueue::Compute);
    ASSERT_NE(w2, nullptr);
    EXPECT_EQ(w2->Value, s1.SignalValue);
    // graphics timeline advanced across its two submissions.
    EXPECT_EQ(s0.SignalValue, 1u);
    EXPECT_EQ(s2.SignalValue, 2u);
}

// A ray-query pass on the compute queue that reads a TLAS the graphics queue builds
// in the same frame. The reader's earlier phase would sort it first; the declared
// structure must order it after the build and make the compute submission wait on
// the build's, with no barrier recorded for the structure itself (#2532).
TEST(RGSubmission, AccelerationStructureReaderWaitsForItsBuildAcrossQueues)
{
    RGGraph g;
    g.BeginFrame();
    const RGResourceId tlas = AccelerationStructure(g, "TLAS");
    const RGResourceId rays = Buf(g, "Rays");
    g.MarkExternal(rays);

    const RGPassId build = g.AddPass({"Build", 0, RGQueue::Graphics});
    g.Write(build, tlas, RGAccess::AccelerationStructureBuild);
    const RGPassId trace = g.AddPass({"Trace", -100, RGQueue::Compute});
    g.Read(trace, tlas, RGAccess::AccelerationStructureRead);
    g.Write(trace, rays, RGAccess::StorageWrite);
    Bake(g);

    const auto& order = g.ScheduledOrder();
    const auto at = [&](RGPassId p) { return std::find(order.begin(), order.end(), p) - order.begin(); };
    ASSERT_EQ(order.size(), 2u);
    EXPECT_LT(at(build), at(trace));

    ASSERT_EQ(g.Submissions().size(), 2u);
    const RGSubmission& graphics = g.Submissions()[0];
    const RGSubmission& compute = g.Submissions()[1];
    ASSERT_EQ(graphics.Queue, RGQueue::Graphics);
    ASSERT_EQ(compute.Queue, RGQueue::Compute);
    const RGSemaphoreWait* wait = FindWait(compute, RGQueue::Graphics);
    ASSERT_NE(wait, nullptr) << "the trace does not wait for the build";
    EXPECT_EQ(wait->Value, graphics.SignalValue);

    for (const RGBarrier& b : g.Barriers())
        EXPECT_NE(b.Resource, tlas) << "the graph recorded a barrier for an acceleration structure";
}

// Review fix (4): timeline values must come from PERSISTENT counters. Frames
// overlap in flight — restarting at 1 every frame would collide with frame
// N-1's still-pending signal of the same value.
TEST(RGSubmission, TimelineValuesPersistAcrossFrames)
{
    RGGraph g;
    auto frame = [&]() -> uint64_t
    {
        g.BeginFrame();
        RGResourceId color = Tex(g, "Color");
        g.MarkExternal(color);
        RGPassId p = g.AddPass({"Draw"});
        g.Write(p, color, RGAccess::ColorAttachment);
        Bake(g);
        return g.Submissions()[0].SignalValue;
    };
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(frame(), 2u) << "frame N must signal a value frame N-1 has not used";
    EXPECT_EQ(frame(), 3u);
}

// Review fix (M5): on devices whose compute queue aliases graphics (MoltenVK),
// the physical-queue remap collapses the fork/join into ONE submission with no
// semaphores — same graph, no pointless splits.
TEST(RGSubmission, AliasedPhysicalQueuesCollapseSubmissions)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId depth = Tex(g, "Depth");
    RGResourceId lights = Buf(g, "LightLists");
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color);
    RGPassId pre = g.AddPass({"DepthPrepass", 0, RGQueue::Graphics});
    g.Write(pre, depth, RGAccess::DepthWrite);
    RGPassId cull = g.AddPass({"ClusterCull", 0, RGQueue::Compute});
    g.Read(cull, depth, RGAccess::Sampled);
    g.Write(cull, lights, RGAccess::StorageWrite);
    RGPassId world = g.AddPass({"World", 0, RGQueue::Graphics});
    g.Read(world, lights, RGAccess::StorageRead);
    g.Write(world, color, RGAccess::ColorAttachment);

    g.Compile();
    g.Schedule();
    g.GenerateBarriers(RGGraph::kIdentityQueueMap);
    const uint8_t allAliased[3] = {0, 0, 0}; // compute/transfer == graphics
    g.BuildSubmissionPlan(allAliased);

    ASSERT_EQ(g.Submissions().size(), 1u) << "aliased queues must not split submissions";
    EXPECT_EQ(g.Submissions()[0].ScheduledCount, 3u);
    EXPECT_EQ(g.Submissions()[0].WaitCount, 0u) << "no semaphores on one physical queue";
}

TEST(RGSubmission, SameQueueDependencyAcrossSubmissionsNeedsNoSemaphore)
{
    // world depends on BOTH the graphics depth (same queue) and the compute cull
    // (different queue). Only the cross-queue dependency should produce a wait.
    RGGraph g;
    g.BeginFrame();
    RGResourceId depth = Tex(g, "Depth");
    RGResourceId lights = Buf(g, "LightLists");
    RGResourceId color = Tex(g, "Color");
    g.MarkExternal(color);

    RGPassId pre = g.AddPass({"DepthPrepass", 0, RGQueue::Graphics});
    g.Write(pre, depth, RGAccess::DepthWrite);
    RGPassId cull = g.AddPass({"ClusterCull", 0, RGQueue::Compute});
    g.Read(cull, depth, RGAccess::Sampled);
    g.Write(cull, lights, RGAccess::StorageWrite);
    RGPassId world = g.AddPass({"World", 0, RGQueue::Graphics});
    g.Read(world, depth, RGAccess::DepthRead); // same-queue dependency on pre
    g.Read(world, lights, RGAccess::StorageRead); // cross-queue dependency on cull
    g.Write(world, color, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const RGSubmission& s2 = g.Submissions()[2]; // graphics: world
    ASSERT_EQ(s2.WaitCount, 1u) << "only the compute dependency needs a semaphore";
    EXPECT_EQ(s2.Waits[0].Queue, static_cast<uint32_t>(RGQueue::Compute));
}

// An explicit ordering edge carries no access records, so the hazard-derived
// wait loops can't see it: across physical queues it must still produce a
// timeline wait (schedule order alone doesn't execute in order across queues).
TEST(RGSubmission, OrderingEdgeAcrossPhysicalQueuesProducesTimelineWait)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId b = Buf(g, "B");
    g.MarkExternal(a);
    g.MarkExternal(b);
    RGPassId gfx = g.AddPass({"Gfx", 0, RGQueue::Graphics});
    g.Write(gfx, a, RGAccess::ColorAttachment);
    RGPassId comp = g.AddPass({"Comp", 0, RGQueue::Compute});
    g.Write(comp, b, RGAccess::StorageWrite);
    g.AddOrderingEdge(gfx, comp); // no shared resource: pure ordering
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u);
    const RGSubmission& s0 = g.Submissions()[0];
    const RGSubmission& s1 = g.Submissions()[1];
    EXPECT_EQ(s0.Queue, RGQueue::Graphics);
    EXPECT_EQ(s1.Queue, RGQueue::Compute);
    const RGSemaphoreWait* w = FindWait(s1, RGQueue::Graphics);
    ASSERT_NE(w, nullptr) << "cross-physical ordering edge must emit a timeline wait";
    EXPECT_EQ(w->Value, s0.SignalValue);

    // Same graph under the collapsed map: one submission, no self-wait.
    RGGraph g2;
    g2.BeginFrame();
    RGResourceId a2 = Tex(g2, "A");
    RGResourceId b2 = Buf(g2, "B");
    g2.MarkExternal(a2);
    g2.MarkExternal(b2);
    RGPassId gfx2 = g2.AddPass({"Gfx", 0, RGQueue::Graphics});
    g2.Write(gfx2, a2, RGAccess::ColorAttachment);
    RGPassId comp2 = g2.AddPass({"Comp", 0, RGQueue::Compute});
    g2.Write(comp2, b2, RGAccess::StorageWrite);
    g2.AddOrderingEdge(gfx2, comp2);
    g2.Compile();
    g2.Schedule();
    g2.GenerateBarriers(RGGraph::kIdentityQueueMap);
    const uint8_t collapsed[3] = {0, 0, 0};
    g2.BuildSubmissionPlan(collapsed);
    ASSERT_EQ(g2.Submissions().size(), 1u);
    EXPECT_EQ(g2.Submissions()[0].WaitCount, 0u);
}

// Finding A (barrier-tracker redesign, Slice 1): a read that performs a LAYOUT
// TRANSITION is a producer, so a later reader on a DIFFERENT physical queue must
// wait on the transition's submission. This mirrors the barrier-IR scenario in
// RGBarrier.TextureReadLayoutTransitionTakesQueueOwnership and asserts the
// submission plan now emits the reader->reader-transition edge. Compute writes X
// (GENERAL); graphics SAMPLES it (General->ShaderReadOnly transition — the layout
// producer); compute reads it again. The compute reader's RAW wait resolves to the
// compute WRITER (same physical queue, no semaphore), so before Slice 1 there was
// NO edge forcing the graphics transition to retire first — a silent cross-queue
// hazard with async compute on.
TEST(RGSubmission, CrossQueueReadTransitionConsumerWaitsOnLayoutProducer)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId o1 = Tex(g, "Out1");
    RGResourceId o2 = Tex(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId wc = g.AddPass({"ComputeWrite", 0, RGQueue::Compute});
    g.Write(wc, x, RGAccess::StorageWrite); // X -> GENERAL, owned by Compute
    RGPassId rg = g.AddPass({"GraphicsSample", 0, RGQueue::Graphics});
    g.Read(rg, x, RGAccess::Sampled); // read-transition General->ShaderReadOnly (producer)
    g.Write(rg, o1, RGAccess::ColorAttachment);
    RGPassId rc = g.AddPass({"ComputeReadAgain", 0, RGQueue::Compute});
    g.Read(rc, x, RGAccess::StorageRead); // depends on the graphics transition
    g.Write(rc, o2, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const RGSubmission& sWrite = g.Submissions()[0];    // compute: wc
    const RGSubmission& sProducer = g.Submissions()[1]; // graphics: rg (layout producer)
    const RGSubmission& sConsumer = g.Submissions()[2]; // compute: rc
    EXPECT_EQ(sWrite.Queue, RGQueue::Compute);
    EXPECT_EQ(sProducer.Queue, RGQueue::Graphics);
    EXPECT_EQ(sConsumer.Queue, RGQueue::Compute);

    // THE FIX: the compute consumer waits on the graphics layout producer.
    const RGSemaphoreWait* wLayout = FindWait(sConsumer, RGQueue::Graphics);
    ASSERT_NE(wLayout, nullptr)
        << "cross-queue reader must wait on the read-transition producer (Finding A)";
    EXPECT_EQ(wLayout->Value, sProducer.SignalValue);

    // ...and it does NOT gain a redundant wait on its own-queue writer: RAW to the
    // compute writer is same physical queue, ordered by submission sequence.
    EXPECT_EQ(FindWait(sConsumer, RGQueue::Compute), nullptr)
        << "RAW to the compute writer is same physical queue: no semaphore";
    EXPECT_EQ(sConsumer.WaitCount, 1u) << "exactly the layout-producer edge, nothing more";
}

// Slice 1 must not over-emit: when the layout producer and a later consumer share
// the same PHYSICAL queue, the queue timeline already orders them — no wait is
// added even across a submission boundary. wG writes X; prodG samples it
// (ColorAttachment->ShaderReadOnly transition — the producer); a compute pass
// bridges (splitting the graphics queue into two submissions); consG samples X on
// the SAME graphics queue. consG's only wait is the cross-queue compute bridge from
// the ordering edge — never a redundant graphics-queue wait on prodG.
TEST(RGSubmission, SamePhysicalQueueReadTransitionConsumerGetsNoRedundantWait)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId o1 = Tex(g, "Out1");
    RGResourceId z = Buf(g, "Bridge");
    RGResourceId o2 = Tex(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(z);
    g.MarkExternal(o2);
    RGPassId wG = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
    g.Write(wG, x, RGAccess::ColorAttachment); // X -> ColorAttachment
    RGPassId prodG = g.AddPass({"GraphicsSample", 0, RGQueue::Graphics});
    g.Read(prodG, x, RGAccess::Sampled); // read-transition ColorAttachment->ShaderReadOnly (producer)
    g.Write(prodG, o1, RGAccess::ColorAttachment);
    RGPassId midC = g.AddPass({"ComputeBridge", 0, RGQueue::Compute});
    g.Write(midC, z, RGAccess::StorageWrite);
    RGPassId consG = g.AddPass({"GraphicsSampleAgain", 0, RGQueue::Graphics});
    g.Read(consG, x, RGAccess::Sampled); // same ShaderReadOnly layout: pure read
    g.Write(consG, o2, RGAccess::ColorAttachment);
    // Force the compute bridge between the two graphics samples so prodG and consG
    // land in SEPARATE graphics submissions on one physical queue. midC shares no
    // resource with them, so the ordering edges are the only sequencing.
    g.AddOrderingEdge(prodG, midC);
    g.AddOrderingEdge(midC, consG);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const RGSubmission& sProducer = g.Submissions()[0]; // graphics: wG, prodG
    const RGSubmission& sBridge = g.Submissions()[1];   // compute: midC
    const RGSubmission& sConsumer = g.Submissions()[2]; // graphics: consG
    EXPECT_EQ(sProducer.Queue, RGQueue::Graphics);
    EXPECT_EQ(sBridge.Queue, RGQueue::Compute);
    EXPECT_EQ(sConsumer.Queue, RGQueue::Graphics);

    // The consumer shares the producer's graphics physical queue: the layout-
    // producer edge is suppressed (ordered by submission sequence). Its only wait
    // is the cross-queue compute bridge from the ordering edge.
    ASSERT_EQ(sConsumer.WaitCount, 1u) << "no redundant same-physical-queue layout-producer wait";
    EXPECT_EQ(sConsumer.Waits[0].Queue, static_cast<uint32_t>(RGQueue::Compute));
    EXPECT_EQ(FindWait(sConsumer, RGQueue::Graphics), nullptr)
        << "same physical queue as the layout producer: parallelism preserved";
}

// Slice 1b (barrier-tracker redesign): the WAR-on-layout half of Finding A. A
// read-transition is !IsWrite, so it used to skip the WAR drain entirely: an
// earlier concurrent cross-queue reader was never ordered against a LATER
// read-transition that re-transitions the layout out from under it. P0 graphics
// writes X (ColorAttachment); P1 compute samples it (-> ShaderReadOnly, layout
// producer); P2 graphics samples it (pure read, same layout); P3 compute
// storage-reads it (ShaderReadOnly -> General RE-transition). Before the fix
// P3's RAW resolved to P0 (graphics value 1), its layout-producer edge to P1
// (same physical queue - no-op), and nothing ordered it after P2 - so P3 could
// re-transition X while P2 was still sampling it. A read-transition must drain
// prior concurrent readers exactly as a write does.
TEST(RGSubmission, ReadTransitionDrainsPriorCrossQueueReaders)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Tex(g, "Out2");
    RGResourceId o3 = Buf(g, "Out3");
    g.MarkExternal(o2);
    g.MarkExternal(o3);
    RGPassId p0 = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
    g.Write(p0, x, RGAccess::ColorAttachment); // X -> ColorAttachment
    RGPassId p1 = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
    g.Read(p1, x, RGAccess::Sampled); // Color -> ShaderReadOnly (layout producer)
    g.Write(p1, o1, RGAccess::StorageWrite);
    RGPassId p2 = g.AddPass({"GraphicsSample", 0, RGQueue::Graphics});
    g.Read(p2, x, RGAccess::Sampled); // pure read, same ShaderReadOnly layout
    g.Read(p2, o1, RGAccess::StorageRead); // chain after P1 (deterministic order)
    g.Write(p2, o2, RGAccess::ColorAttachment);
    RGPassId p3 = g.AddPass({"ComputeRetransition", 0, RGQueue::Compute});
    g.Read(p3, x, RGAccess::StorageRead); // ShaderReadOnly -> General re-transition
    g.Write(p3, o3, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 4u);
    const RGSubmission& s0 = g.Submissions()[0]; // graphics: p0
    const RGSubmission& s1 = g.Submissions()[1]; // compute:  p1
    const RGSubmission& s2 = g.Submissions()[2]; // graphics: p2
    const RGSubmission& s3 = g.Submissions()[3]; // compute:  p3
    EXPECT_EQ(s0.Queue, RGQueue::Graphics);
    EXPECT_EQ(s1.Queue, RGQueue::Compute);
    EXPECT_EQ(s2.Queue, RGQueue::Graphics);
    EXPECT_EQ(s3.Queue, RGQueue::Compute);

    // THE FIX: the re-transitioning reader drains the prior graphics reader, so
    // its graphics wait dedups UP to p2's signal (subsuming the RAW on p0).
    // Before the fix the wait existed but stopped at the writer's value.
    const RGSemaphoreWait* w = FindWait(s3, RGQueue::Graphics);
    ASSERT_NE(w, nullptr) << "re-transition must wait on the graphics queue";
    EXPECT_EQ(w->Value, s2.SignalValue)
        << "WAR-on-layout: must drain the concurrent graphics reader (p2), not just RAW to p0";
    EXPECT_EQ(s3.WaitCount, 1u) << "one deduped wait per foreign physical queue";

    // Sanity: p2 itself waits on the compute layout producer (RAW-on-layout).
    const RGSemaphoreWait* w2 = FindWait(s2, RGQueue::Compute);
    ASSERT_NE(w2, nullptr);
    EXPECT_EQ(w2->Value, s1.SignalValue);
}

// S4 x capture: a CopySrc capture makes a claim-GENERAL texture layout-dynamic
// across frames for the first time — the pool seeds the NEXT frame's import at
// TransferSrc (the capture's end layout, restored by the readback export in
// steady state, but a frame can legitimately begin at TransferSrc). The first
// reader must re-transition TransferSrc -> General (never ShaderReadOnly: the
// descriptors claim GENERAL), and because that read-transition is a layout
// producer, a reader on the other physical queue must wait on its submission.
TEST(RGSubmission, CaptureSeededImportReTransitionsToGeneralAndOrdersCrossQueue)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = TexGeneral(g, "DepthResolved");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Tex(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    g.MarkImported(x, RGImageLayout::TransferSrc); // prior frame ended in a capture
    RGPassId pc = g.AddPass({"HzbRead", 0, RGQueue::Compute});
    g.Read(pc, x, RGAccess::StorageRead); // TransferSrc -> General (layout producer)
    g.Write(pc, o1, RGAccess::StorageWrite);
    RGPassId pg = g.AddPass({"WorldSample", 0, RGQueue::Graphics});
    g.Read(pg, x, RGAccess::Sampled); // claimed GENERAL: pure read, same layout
    g.Write(pg, o2, RGAccess::ColorAttachment);
    Bake(g);

    // Barrier IR: exactly one re-transition, TransferSrc -> General; no barrier
    // ever parks the texture in ShaderReadOnly (descriptor claim).
    const RGBarrier* seed = nullptr;
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != x)
            continue;
        EXPECT_NE(b.NewLayout, RGImageLayout::ShaderReadOnly)
            << "sampled reads must observe the claimed GENERAL layout";
        if (b.OldLayout == RGImageLayout::TransferSrc)
        {
            EXPECT_EQ(seed, nullptr) << "one seed re-transition, not per-reader";
            seed = &b;
        }
    }
    ASSERT_NE(seed, nullptr) << "first reader must consume the capture-seeded TransferSrc";
    EXPECT_EQ(seed->NewLayout, RGImageLayout::General);

    // Submission plan: the graphics reader waits on the compute submission that
    // performed the re-transition (RAW-on-layout producer edge).
    ASSERT_EQ(g.Submissions().size(), 2u);
    const RGSubmission& sC = g.Submissions()[0];
    const RGSubmission& sG = g.Submissions()[1];
    EXPECT_EQ(sC.Queue, RGQueue::Compute);
    EXPECT_EQ(sG.Queue, RGQueue::Graphics);
    const RGSemaphoreWait* w = FindWait(sG, RGQueue::Compute);
    ASSERT_NE(w, nullptr) << "cross-queue reader must wait on the layout producer";
    EXPECT_EQ(w->Value, sC.SignalValue);
}

// The export hole, closed (cross-queue sync design, slice 1). An exported
// texture last WRITTEN on compute records its restore transition at the tail
// of the COMPUTE submission that owns the cells — same-queue with its
// producer, source scope expressible verbatim — never on the last graphics
// submission with no derived edge (which raced the compute write in release
// and asserted in debug; the IBL Settle passes existed to dodge this shape).
// ── Cross-physical layout-transition placement (slice 2) ────────────────────
// §1's live crossing (IBL_EnvCapture): a graphics pass leaves the texture in
// ColorAttachment and a compute pass samples it. Recorded on the compute
// consumer the source scope can only be DISCARDED — COLOR_ATTACHMENT_OUTPUT is
// not a legal mask on a compute queue — so the dependency rests entirely on the
// semaphore and the recorded scope is fiction. The transition must instead
// record at the tail of the graphics submission that owns the cells, where the
// true producer scope is expressible verbatim.
TEST(RGSubmission, CrossPhysicalReadTransitionRecordsAtTheProducerTail)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId cap = Tex(g, "IBL_EnvCapture");
    RGResourceId mip = Tex(g, "IBL_EnvMip1");
    g.MarkExternal(mip);
    RGPassId face = g.AddPass({"IBLGen.SkyCapture.Face0", 0, RGQueue::Graphics});
    g.Write(face, cap, RGAccess::ColorAttachment);
    RGPassId down = g.AddPass({"IBLGen.EnvMipDownsample.Mip1", 0, RGQueue::Compute});
    g.Read(down, cap, RGAccess::Sampled);
    g.Write(down, mip, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u);
    EXPECT_EQ(g.Submissions()[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(g.Submissions()[1].Queue, RGQueue::Compute);

    const std::vector<uint32_t> trans = TransitionsOf(g, cap);
    ASSERT_EQ(trans.size(), 2u) << "the first-use init and the crossing — ONE barrier per crossing";
    const RGBarrier& init = g.Barriers()[trans[0]];
    const RGBarrier& cross = g.Barriers()[trans[1]];

    EXPECT_TRUE(init.FirstTouch);
    EXPECT_EQ(g.BarrierSubmissions()[trans[0]], kInvalidId)
        << "first-touch barriers belong to the submission-front hoist, never to a tail";

    ASSERT_FALSE(cross.FirstTouch);
    EXPECT_EQ(cross.OldLayout, RGImageLayout::ColorAttachment);
    EXPECT_EQ(cross.NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(cross.SrcQueue, static_cast<uint32_t>(RGQueue::Graphics));
    EXPECT_EQ(cross.DstQueue, static_cast<uint32_t>(RGQueue::Compute));
    EXPECT_EQ(cross.SrcStage, RGStage::ColorAttachmentOutput)
        << "the TRUE producer stage — the mask a compute queue cannot name";
    // THE SLICE: relocated onto the producing submission's tail.
    EXPECT_EQ(g.BarrierSubmissions()[trans[1]], 0u)
        << "the crossing records where ColorAttachmentOutput is legal, not on the consumer";

    // ...and it costs nothing: the consumer already RAW-waits on that submission.
    EXPECT_EQ(g.Submissions()[0].WaitCount, 0u);
    ASSERT_EQ(g.Submissions()[1].WaitCount, 1u) << "exactly the RAW the sampler already had";
    EXPECT_EQ(g.Submissions()[1].Waits[0].Queue, static_cast<uint32_t>(RGQueue::Graphics));
    EXPECT_EQ(g.Submissions()[1].Waits[0].Value, g.Submissions()[0].SignalValue);
}

// The write path crosses the same way: a compute pass STORAGE-writing a texture
// a graphics pass left as a colour attachment needs ColorAttachment->General,
// whose honest source is ColorAttachmentOutput/ColorWrite. Same relocation.
TEST(RGSubmission, CrossPhysicalWriteTransitionRecordsAtTheProducerTail)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    g.MarkExternal(x);
    RGPassId gw = g.AddPass({"GraphicsAttachmentWrite", 0, RGQueue::Graphics});
    g.Write(gw, x, RGAccess::ColorAttachment);
    RGPassId cw = g.AddPass({"ComputeStorageWrite", 0, RGQueue::Compute});
    g.Write(cw, x, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u);
    const std::vector<uint32_t> trans = TransitionsOf(g, x);
    ASSERT_EQ(trans.size(), 2u);
    const RGBarrier& cross = g.Barriers()[trans[1]];
    EXPECT_EQ(cross.OldLayout, RGImageLayout::ColorAttachment);
    EXPECT_EQ(cross.NewLayout, RGImageLayout::General);
    EXPECT_EQ(cross.SrcStage, RGStage::ColorAttachmentOutput);
    EXPECT_EQ(cross.SrcAccess, RGAccessMask::ColorWrite);
    EXPECT_EQ(g.BarrierSubmissions()[trans[1]], 0u);
    // WAW on the graphics writer is the wait that already ordered this crossing.
    ASSERT_EQ(g.Submissions()[1].WaitCount, 1u);
    EXPECT_EQ(g.Submissions()[1].Waits[0].Value, g.Submissions()[0].SignalValue);
}

// The precondition, stated as a hazard: relocation is DECLINED whenever any
// accessor since the last write sits on another physical queue. Here a compute
// pass reads X (General, no transition) before the compute sampler transitions
// it. The graphics submission is still the producer — but hoisting the
// transition to its tail would re-layout X while that earlier compute read is
// in flight, and ordering the two would need a timeline wait the plan does not
// have. Declining leaves today's consumer-side barrier, which is always safe.
TEST(RGSubmission, CrossPhysicalTransitionAfterAConsumerQueueReaderStaysOnTheConsumer)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId bridge = Buf(g, "Bridge");
    RGResourceId out = Tex(g, "Out");
    g.MarkExternal(out);
    RGPassId gw = g.AddPass({"GraphicsStorageWrite", 0, RGQueue::Graphics});
    g.Write(gw, x, RGAccess::StorageWrite); // X -> GENERAL, owned by graphics
    RGPassId cr = g.AddPass({"ComputeReadGeneral", 0, RGQueue::Compute});
    g.Read(cr, x, RGAccess::StorageRead); // pure read, no transition
    g.Write(cr, bridge, RGAccess::StorageWrite);
    RGPassId cs = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
    g.Read(cs, bridge, RGAccess::StorageRead); // forces cs after cr
    g.Read(cs, x, RGAccess::Sampled);         // General -> ShaderReadOnly, crossing
    g.Write(cs, out, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u) << "the two compute passes share one submission";
    const std::vector<uint32_t> trans = TransitionsOf(g, x);
    ASSERT_EQ(trans.size(), 2u);
    const RGBarrier& cross = g.Barriers()[trans[1]];
    EXPECT_EQ(cross.OldLayout, RGImageLayout::General);
    EXPECT_EQ(cross.NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(cross.SrcQueue, static_cast<uint32_t>(RGQueue::Graphics)) << "still a crossing";
    EXPECT_EQ(g.BarrierSubmissions()[trans[1]], kInvalidId)
        << "a foreign-queue reader trails the producer: the transition must stay on its consumer";
}

// The relocation counters, on the two shapes that must read differently. They
// exist to size the residual a release/acquire pair design would still carry:
// a DECLINED crossing is one that keeps recording on its consumer with a source
// scope the recording queue cannot express. Declined is only interpretable next
// to Considered — a frame with no crossings reports declined=0 too, and reading
// that as "nothing declines" is the false zero this pair exists to prevent.
TEST(RGSubmission, RelocationCountersSeparateNoCrossingsFromNoDeclines)
{
    // Relocatable: every accessor of X so far sits on the producing queue.
    {
        RGGraph g;
        g.BeginFrame();
        RGResourceId cap = Tex(g, "Cap");
        RGResourceId out = Tex(g, "Out");
        g.MarkExternal(out);
        RGPassId gw = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
        g.Write(gw, cap, RGAccess::ColorAttachment);
        RGPassId cs = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
        g.Read(cs, cap, RGAccess::Sampled);
        g.Write(cs, out, RGAccess::StorageWrite);
        Bake(g);

        EXPECT_EQ(g.CrossQueueTransitionsConsidered(), 1u);
        EXPECT_EQ(g.CrossQueueRelocationsDeclined(), 0u) << "all accessors on the producer";
    }

    // Declined: a consumer-queue reader trails the producer, so the placement
    // would be unsafe and the barrier stays on its consumer.
    {
        RGGraph g;
        g.BeginFrame();
        RGResourceId x = Tex(g, "X");
        RGResourceId bridge = Buf(g, "Bridge");
        RGResourceId out = Tex(g, "Out");
        g.MarkExternal(out);
        RGPassId gw = g.AddPass({"GraphicsStorageWrite", 0, RGQueue::Graphics});
        g.Write(gw, x, RGAccess::StorageWrite);
        RGPassId cr = g.AddPass({"ComputeReadGeneral", 0, RGQueue::Compute});
        g.Read(cr, x, RGAccess::StorageRead);
        g.Write(cr, bridge, RGAccess::StorageWrite);
        RGPassId cs = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
        g.Read(cs, bridge, RGAccess::StorageRead);
        g.Read(cs, x, RGAccess::Sampled);
        g.Write(cs, out, RGAccess::StorageWrite);
        Bake(g);

        EXPECT_EQ(g.CrossQueueTransitionsConsidered(), 1u);
        EXPECT_EQ(g.CrossQueueRelocationsDeclined(), 1u);
    }

    // No crossing at all: declined is 0 for the OTHER reason, and Considered is
    // what says so.
    {
        RGGraph g;
        g.BeginFrame();
        RGResourceId x = Tex(g, "X");
        RGResourceId out = Tex(g, "Out");
        g.MarkExternal(out);
        RGPassId gw = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
        g.Write(gw, x, RGAccess::ColorAttachment);
        RGPassId gs = g.AddPass({"GraphicsSample", 0, RGQueue::Graphics});
        g.Read(gs, x, RGAccess::Sampled);
        g.Write(gs, out, RGAccess::ColorAttachment);
        Bake(g);

        EXPECT_EQ(g.CrossQueueTransitionsConsidered(), 0u);
        EXPECT_EQ(g.CrossQueueRelocationsDeclined(), 0u);
    }
}

// A same-physical transition is untouched — no placement, so the recording path
// is byte-identical to a frame with no crossings at all.
TEST(RGSubmission, SamePhysicalTransitionIsNeverRelocated)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId out = Tex(g, "Out");
    g.MarkExternal(out);
    RGPassId gw = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
    g.Write(gw, x, RGAccess::ColorAttachment);
    RGPassId gs = g.AddPass({"GraphicsSample", 0, RGQueue::Graphics});
    g.Read(gs, x, RGAccess::Sampled);
    g.Write(gs, out, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 1u);
    for (uint32_t placement : g.BarrierSubmissions())
        EXPECT_EQ(placement, kInvalidId) << "one physical queue ⇒ nothing to relocate";
    EXPECT_EQ(TotalWaits(g), 0u);
}

// Under the collapsed map (MoltenVK: compute/transfer alias graphics) there are
// no cross-PHYSICAL transitions, so the whole mechanism is inert — the property
// that keeps a logical-queue-annotated graph portable.
TEST(RGSubmission, CollapsedQueueMapRelocatesNothing)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId cap = Tex(g, "Cap");
    RGResourceId mip = Tex(g, "Mip");
    g.MarkExternal(mip);
    RGPassId face = g.AddPass({"Capture", 0, RGQueue::Graphics});
    g.Write(face, cap, RGAccess::ColorAttachment);
    RGPassId down = g.AddPass({"Downsample", 0, RGQueue::Compute});
    g.Read(down, cap, RGAccess::Sampled);
    g.Write(down, mip, RGAccess::StorageWrite);

    const uint8_t allAliased[3] = {0, 0, 0};
    g.Compile();
    g.Schedule();
    g.GenerateBarriers(allAliased);
    g.BuildSubmissionPlan(allAliased);

    ASSERT_EQ(g.Submissions().size(), 1u);
    for (uint32_t placement : g.BarrierSubmissions())
        EXPECT_EQ(placement, kInvalidId);
    EXPECT_EQ(TotalWaits(g), 0u);
}

// The hoist boundary: an imported texture whose FIRST touch this frame is a
// cross-queue transition carries a BottomOfPipe source (the cross-frame
// execution dependency) and no intra-frame producer to record against. It stays
// with the submission-front hoist — relocating it would have nowhere to go.
TEST(RGSubmission, FirstTouchCrossQueueTransitionIsNotRelocated)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "Imported");
    g.MarkImported(x, RGImageLayout::ShaderReadOnly);
    g.MarkExternal(x);
    RGPassId cw = g.AddPass({"ComputeWrite", 0, RGQueue::Compute});
    g.Write(cw, x, RGAccess::StorageWrite); // ShaderReadOnly -> General on compute
    Bake(g);

    const std::vector<uint32_t> trans = TransitionsOf(g, x);
    ASSERT_EQ(trans.size(), 1u);
    const RGBarrier& b = g.Barriers()[trans[0]];
    EXPECT_TRUE(b.FirstTouch);
    EXPECT_EQ(b.SrcStage, RGStage::BottomOfPipe);
    EXPECT_EQ(b.SrcQueue, static_cast<uint32_t>(RGQueue::Graphics)) << "cell init queue";
    EXPECT_EQ(g.BarrierSubmissions()[trans[0]], kInvalidId);
}

// Zero added semaphore operations, over a frame carrying THREE crossings on two
// resources. Relocation derives no new edges by construction — its precondition
// is that every candidate accessor already sits on the placement's own physical
// queue, where submission order alone orders them. This pins the whole frame's
// wait set so a future placement rule cannot buy correctness with traffic.
TEST(RGSubmission, RelocationDerivesNoAdditionalTimelineWaits)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Tex(g, "A");
    RGResourceId b = Tex(g, "B");
    RGResourceId out = Tex(g, "Out");
    RGResourceId final = Tex(g, "Final");
    g.MarkExternal(final); // the only sink: every other pass survives through it
    RGPassId gw = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
    g.Write(gw, a, RGAccess::ColorAttachment);
    g.Write(gw, b, RGAccess::ColorAttachment);
    RGPassId cs = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
    g.Read(cs, a, RGAccess::Sampled);  // crossing 1: ColorAttachment -> ShaderReadOnly
    g.Read(cs, b, RGAccess::Sampled);  // crossing 2: same
    g.Write(cs, out, RGAccess::StorageWrite);
    RGPassId gr = g.AddPass({"GraphicsResample", 0, RGQueue::Graphics});
    g.Read(gr, out, RGAccess::Sampled); // crossing 3: General -> ShaderReadOnly, C->G
    g.Write(gr, final, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const std::vector<uint32_t> ta = TransitionsOf(g, a);
    const std::vector<uint32_t> tb = TransitionsOf(g, b);
    const std::vector<uint32_t> to = TransitionsOf(g, out);
    ASSERT_EQ(ta.size(), 2u);
    ASSERT_EQ(tb.size(), 2u);
    ASSERT_EQ(to.size(), 2u);
    EXPECT_EQ(g.BarrierSubmissions()[ta[1]], 0u) << "A's crossing on the graphics producer";
    EXPECT_EQ(g.BarrierSubmissions()[tb[1]], 0u) << "B's crossing on the graphics producer";
    EXPECT_EQ(g.BarrierSubmissions()[to[1]], 1u) << "Out's crossing on the compute producer";

    // The frame's entire semaphore traffic: the compute sampler's RAW on the
    // graphics writer, and the graphics resampler's RAW on the compute writer.
    EXPECT_EQ(TotalWaits(g), 2u) << "one wait per real cross-queue dependency, and no more";
    EXPECT_EQ(g.Submissions()[0].WaitCount, 0u);
    ASSERT_EQ(g.Submissions()[1].WaitCount, 1u);
    EXPECT_EQ(g.Submissions()[1].Waits[0].Value, g.Submissions()[0].SignalValue);
    ASSERT_EQ(g.Submissions()[2].WaitCount, 1u);
    EXPECT_EQ(g.Submissions()[2].Waits[0].Value, g.Submissions()[1].SignalValue);
}

TEST(RGSubmission, ExportOfComputeWrittenTextureLandsOnItsQueueTail)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId color = Tex(g, "Color");
    RGResourceId x = Tex(g, "IBL_Irradiance");
    g.MarkExternal(color);
    g.MarkImported(x, RGImageLayout::ShaderReadOnly); // the resting-state claim
    g.MarkExternal(x);
    g.SetExportLayout(x, RGImageLayout::ShaderReadOnly);
    RGPassId world = g.AddPass({"World", 0, RGQueue::Graphics});
    g.Write(world, color, RGAccess::ColorAttachment);
    RGPassId bake = g.AddPass({"Convolve", 0, RGQueue::Compute});
    g.Write(bake, x, RGAccess::StorageWrite); // -> GENERAL on compute
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u);
    EXPECT_EQ(g.Submissions()[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(g.Submissions()[1].Queue, RGQueue::Compute);

    const RGBarrierBatch* exp = ExportBatch(g);
    ASSERT_NE(exp, nullptr);
    ASSERT_EQ(exp->Count, 1u);
    EXPECT_EQ(g.BarrierSubmissions()[exp->First], 1u)
        << "the restore records on the compute submission that owns the cells";
    // Owner and producer share the submission: no cross-queue edge is needed,
    // and none may be invented.
    EXPECT_EQ(g.Submissions()[0].WaitCount, 0u);
    EXPECT_EQ(g.Submissions()[1].WaitCount, 0u);
    EXPECT_EQ(g.FinalLayout(x), RGImageLayout::ShaderReadOnly);
}

// A foreign reader AFTER the owner's last submission moves the export there
// instead of waiting backward: waits must always point at EARLIER submissions
// (a wait on a later submission's signal is a wait-before-signal deadlock).
// Graphics writes X, compute reads it later — the restore rides the compute
// tail (source sanitized at recording) and the only cross-queue edge is the
// RAW the reader already had.
TEST(RGSubmission, ExportPlacementFollowsALaterForeignReader)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(x);
    g.MarkExternal(out);
    g.SetExportLayout(x, RGImageLayout::ShaderReadOnly);
    RGPassId wG = g.AddPass({"GraphicsWrite", 0, RGQueue::Graphics});
    g.Write(wG, x, RGAccess::StorageWrite); // -> GENERAL, owned by graphics
    RGPassId rC = g.AddPass({"ComputeRead", 0, RGQueue::Compute});
    g.Read(rC, x, RGAccess::StorageRead); // pure read, same GENERAL layout
    g.Write(rC, out, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 2u);
    const RGSubmission& sW = g.Submissions()[0]; // graphics: writer/owner
    const RGSubmission& sR = g.Submissions()[1]; // compute: trailing reader
    EXPECT_EQ(sW.Queue, RGQueue::Graphics);
    EXPECT_EQ(sR.Queue, RGQueue::Compute);

    const RGBarrierBatch* exp = ExportBatch(g);
    ASSERT_NE(exp, nullptr);
    ASSERT_EQ(exp->Count, 1u);
    EXPECT_EQ(g.BarrierSubmissions()[exp->First], 1u)
        << "the restore must trail the foreign reader, not race it from the owner's tail";
    EXPECT_EQ(sW.WaitCount, 0u) << "never a wait on a LATER submission's signal";
    ASSERT_EQ(sR.WaitCount, 1u) << "exactly the RAW the reader already had";
    EXPECT_EQ(sR.Waits[0].Queue, static_cast<uint32_t>(RGQueue::Graphics));
    EXPECT_EQ(sR.Waits[0].Value, sW.SignalValue);
}

// The derived WAR edge itself: when the export rides its owner's queue but a
// foreign reader ran in between, the restore's submission gains a timeline
// wait draining that reader — the edge the sentinel batch never had (it is
// the only barrier class the plan used to skip).
TEST(RGSubmission, ExportAfterCrossQueueReadersDerivesTheWARWait)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "X");
    RGResourceId o1 = Tex(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(x);
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    g.SetExportLayout(x, RGImageLayout::ShaderReadOnly);
    RGPassId wC = g.AddPass({"ComputeWrite", 0, RGQueue::Compute});
    g.Write(wC, x, RGAccess::StorageWrite); // -> GENERAL, owned by compute
    RGPassId rG = g.AddPass({"GraphicsRead", 0, RGQueue::Graphics});
    g.Read(rG, x, RGAccess::StorageRead); // pure read, same GENERAL layout
    g.Write(rG, o1, RGAccess::ColorAttachment);
    RGPassId tail = g.AddPass({"ComputeTail", 0, RGQueue::Compute}); // unrelated
    g.Write(tail, o2, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.Submissions().size(), 3u);
    const RGSubmission& s0 = g.Submissions()[0]; // compute: writer/owner
    const RGSubmission& s1 = g.Submissions()[1]; // graphics: reader
    const RGSubmission& s2 = g.Submissions()[2]; // compute: tail
    EXPECT_EQ(s0.Queue, RGQueue::Compute);
    EXPECT_EQ(s1.Queue, RGQueue::Graphics);
    EXPECT_EQ(s2.Queue, RGQueue::Compute);

    const RGBarrierBatch* exp = ExportBatch(g);
    ASSERT_NE(exp, nullptr);
    ASSERT_EQ(exp->Count, 1u);
    EXPECT_EQ(g.BarrierSubmissions()[exp->First], 2u)
        << "own-queue tail: the latest compute submission, after the foreign reader";
    // THE FIX: the restore's submission drains the graphics reader. Without
    // the derived edge s2 had no waits at all (it shares no resource with
    // anything) and the restore re-transitioned X under the reader.
    const RGSemaphoreWait* w = FindWait(s2, RGQueue::Graphics);
    ASSERT_NE(w, nullptr) << "export WAR edge: restore must drain the foreign reader";
    EXPECT_EQ(w->Value, s1.SignalValue);
    EXPECT_EQ(s2.WaitCount, 1u);
    // Sanity: the reader still RAW-waits on the compute writer.
    const RGSemaphoreWait* raw = FindWait(s1, RGQueue::Compute);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->Value, s0.SignalValue);
}
