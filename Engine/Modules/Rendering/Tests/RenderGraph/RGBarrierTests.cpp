// Stage 1.3 isolation tests for RenderGraph whole-frame barrier generation: minimal
// transitions (RAW/WAR/WAW + layout), read-after-read coalescing, first-use
// init, import-at-tracked-layout, and per-pass batching. Pure logic, no GPU.

#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <gtest/gtest.h>

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
// Texture whose physical sampled descriptors claim GENERAL (storage co-use).
RGResourceId TexGeneral(RGGraph& g, const char* name)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.SampledInGeneralLayout = true;
    d.Name = name;
    return g.CreateResource(d);
}
RGResourceId TexArray(RGGraph& g, const char* name, uint32_t layers)
{
    RGResourceDesc d;
    d.Kind = RGResourceKind::Texture;
    d.ArrayLayers = layers;
    d.Name = name;
    return g.CreateResource(d);
}
const RGBarrier* FindBarrier(const RGGraph& g, RGResourceId r)
{
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == r)
            return &b;
    return nullptr;
}
void Bake(RGGraph& g)
{
    g.Compile();
    g.Schedule();
    g.GenerateBarriers(RGGraph::kIdentityQueueMap);
}
} // namespace

TEST(RGBarrier, BufferReadAfterWriteEmitsOneBarrier)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId y = Buf(g, "Y");
    g.MarkExternal(y);
    RGPassId a = g.AddPass({"WriteX"});
    g.Write(a, x, RGAccess::StorageWrite);
    RGPassId b = g.AddPass({"ReadXWriteY"});
    g.Read(b, x, RGAccess::StorageRead);
    g.Write(b, y, RGAccess::StorageWrite);
    Bake(g);

    // x: recycled-memory first-write exec dep + the RAW; y: its own first-write.
    EXPECT_EQ(g.BarrierCount(), 3u);
    const RGBarrier* raw = nullptr;
    for (const RGBarrier& bar : g.Barriers())
        if (bar.Resource == x && (bar.SrcAccess & RGAccessMask::ShaderWrite) != 0)
            raw = &bar;
    ASSERT_NE(raw, nullptr);
    EXPECT_FALSE(raw->IsTexture);
    EXPECT_TRUE((raw->DstAccess & RGAccessMask::ShaderRead) != 0);
    EXPECT_TRUE((raw->SrcStage & RGStage::ComputeShader) != 0);
}

// The graphics-side StorageRead stage mask must include VertexShader: the
// slice-3 contract orders compute skinning before world/depth/shadow passes
// via a Read whose consumer is the VERTEX shader (bone palettes bound
// descriptor-direct). FS-only visibility would leave the VS reading stale
// palettes — silently, since the data is merely one frame old.
TEST(RGBarrier, StorageReadOnGraphicsCoversTheVertexShaderStage)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId atlas = Buf(g, "SkinAtlas");
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);
    RGPassId skin = g.AddPass({"Skin"});
    g.Write(skin, atlas, RGAccess::StorageWrite);
    RGPassId world = g.AddPass({"World"});
    g.Read(world, atlas, RGAccess::StorageRead);
    g.Write(world, out, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* raw = nullptr;
    for (const RGBarrier& bar : g.Barriers())
        if (bar.Resource == atlas && (bar.SrcAccess & RGAccessMask::ShaderWrite) != 0)
            raw = &bar;
    ASSERT_NE(raw, nullptr);
    EXPECT_TRUE((raw->DstStage & RGStage::VertexShader) != 0)
        << "the VS reads storage buffers descriptor-direct — it needs visibility";
    EXPECT_TRUE((raw->DstStage & RGStage::FragmentShader) != 0);
}

// The compute-queue half of the same fix: the widened graphics mask must NOT
// leak onto a compute queue, where graphics stage bits are an invalid mask.
TEST(RGBarrier, StorageReadOnComputeStaysComputeOnly)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId atlas = Buf(g, "SkinAtlas");
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);
    RGPassId skin = g.AddPass({"Skin"});
    g.Write(skin, atlas, RGAccess::StorageWrite);
    RGPassId reduce = g.AddPass({"Reduce", 0, RGQueue::Compute});
    g.Read(reduce, atlas, RGAccess::StorageRead);
    g.Write(reduce, out, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* raw = nullptr;
    for (const RGBarrier& bar : g.Barriers())
        if (bar.Resource == atlas && (bar.SrcAccess & RGAccessMask::ShaderWrite) != 0 &&
            (bar.DstAccess & RGAccessMask::ShaderRead) != 0)
            raw = &bar;
    ASSERT_NE(raw, nullptr);
    EXPECT_TRUE((raw->DstStage & RGStage::ComputeShader) != 0);
    EXPECT_EQ(raw->DstStage & (RGStage::VertexShader | RGStage::FragmentShader), 0u)
        << "graphics stage bits are invalid on a compute queue";
}

// SampledVertex is the ocean-cascade shape: a compute sim storage-writes a
// displacement field that the surface draw then SAMPLES from the VERTEX stage
// (vertex modifier) as well as the fragment stage. Plain Sampled scopes the
// transition to FragmentShader only, so the VS would displace with non-visible
// data; the vertex-qualified read must land ShaderReadOnly with BOTH stages in
// the consumer scope.
TEST(RGBarrier, SampledVertexCoversVertexAndFragmentStages)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId cascade = Tex(g, "OceanCascade");
    RGResourceId out = Tex(g, "OutColor");
    g.MarkExternal(out);
    RGPassId sim = g.AddPass({"Sim"});
    g.Write(sim, cascade, RGAccess::StorageWrite);
    RGPassId surface = g.AddPass({"Surface"});
    g.Read(surface, cascade, RGAccess::SampledVertex);
    g.Write(surface, out, RGAccess::ColorAttachment);
    Bake(g);

    const RGBarrier* t = nullptr;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == cascade && b.OldLayout == RGImageLayout::General)
            t = &b;
    ASSERT_NE(t, nullptr) << "expected the General->ShaderReadOnly transition";
    EXPECT_EQ(t->NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_TRUE((t->SrcStage & RGStage::ComputeShader) != 0);
    EXPECT_TRUE((t->SrcAccess & RGAccessMask::ShaderWrite) != 0);
    EXPECT_TRUE((t->DstStage & RGStage::VertexShader) != 0)
        << "the vertex modifier samples the cascade — it needs visibility";
    EXPECT_TRUE((t->DstStage & RGStage::FragmentShader) != 0);
    EXPECT_TRUE((t->DstAccess & RGAccessMask::ShaderRead) != 0);
}

TEST(RGBarrier, ReadAfterReadEmitsNoSecondBarrier)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId y = Buf(g, "Y");
    RGResourceId z = Buf(g, "Z");
    g.MarkExternal(y);
    g.MarkExternal(z);
    RGPassId w = g.AddPass({"WriteX"});
    g.Write(w, x, RGAccess::StorageWrite);
    RGPassId r1 = g.AddPass({"Read1"});
    g.Read(r1, x, RGAccess::StorageRead);
    g.Write(r1, y, RGAccess::StorageWrite);
    RGPassId r2 = g.AddPass({"Read2"});
    g.Read(r2, x, RGAccess::StorageRead);
    g.Write(r2, z, RGAccess::StorageWrite);
    Bake(g);

    // x: first-write exec dep + ONE RAW; the second covered reader adds nothing
    // (y and z carry their own first-write barriers).
    uint32_t onX = 0;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == x)
            ++onX;
    EXPECT_EQ(onX, 2u) << "second reader of X needs no barrier (read-after-read)";
}

TEST(RGBarrier, WriteAfterWriteEmitsBarrier)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    g.MarkExternal(x);
    RGPassId a = g.AddPass({"Write1"});
    g.Write(a, x, RGAccess::StorageWrite);
    RGPassId b = g.AddPass({"Write2"});
    g.Write(b, x, RGAccess::StorageWrite);
    Bake(g);

    // First-write exec dep + the WAW.
    EXPECT_EQ(g.BarrierCount(), 2u);
    const RGBarrier* waw = nullptr;
    for (const RGBarrier& bar : g.Barriers())
        if (bar.Resource == x && (bar.SrcAccess & RGAccessMask::ShaderWrite) != 0)
            waw = &bar;
    ASSERT_NE(waw, nullptr) << "WAW hazard on X";
    EXPECT_TRUE((waw->DstAccess & RGAccessMask::ShaderWrite) != 0);
}

TEST(RGBarrier, TextureLayoutTransitionOnWriteThenSample)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId sceneColor = Tex(g, "SceneColor");
    RGResourceId outColor = Tex(g, "OutColor");
    g.MarkExternal(outColor);
    RGPassId draw = g.AddPass({"DrawScene"});
    g.Write(draw, sceneColor, RGAccess::ColorAttachment);
    RGPassId post = g.AddPass({"PostProcess"});
    g.Read(post, sceneColor, RGAccess::Sampled);
    g.Write(post, outColor, RGAccess::ColorAttachment);
    Bake(g);

    // Init barrier for SceneColor (Undefined->Color) + its Color->ShaderReadOnly
    // transition + init barrier for OutColor.
    const RGBarrier* t = nullptr;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == sceneColor && b.OldLayout == RGImageLayout::ColorAttachment)
            t = &b;
    ASSERT_NE(t, nullptr) << "expected the ColorAttachment->ShaderReadOnly transition";
    EXPECT_EQ(t->NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_TRUE((t->SrcStage & RGStage::ColorAttachmentOutput) != 0);
    EXPECT_TRUE((t->DstStage & RGStage::FragmentShader) != 0);
    EXPECT_TRUE((t->SrcAccess & RGAccessMask::ColorWrite) != 0);
    EXPECT_TRUE((t->DstAccess & RGAccessMask::ShaderRead) != 0);
}

// Slice-7e contract: MarkExternal+SetExportLayout are per-resource FLAGS, not
// positional fences — a pass declared AFTER the output is marked (the editor
// overlay shape: outline/composite Load the pipeline's FinalColor post-spine)
// merely moves the resource's last access. The export must still be exactly
// ONE trailing transition, ordered after the late writer, in the sentinel
// batch (Pass == kInvalidId, ScheduledIndex == pass count); the Load-derived
// read keeps the late writer alive and edged against the producer.
TEST(RGBarrier, WriteAfterExportMarkTrailsTheExportTransition)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId finalColor = Tex(g, "FinalColor");
    RGResourceId gizmoColor = Tex(g, "GizmoColor");

    RGPassId world = g.AddPass({"World"});
    g.Write(world, finalColor, RGAccess::ColorAttachment);

    // The spine's step-7 output policy runs here — BEFORE the overlays exist.
    g.MarkExternal(finalColor);
    g.SetExportLayout(finalColor, RGImageLayout::ShaderReadOnly);

    RGPassId overlay = g.AddPass({"GizmoOverlay"});
    g.Write(overlay, gizmoColor, RGAccess::ColorAttachment);

    RGPassId composite = g.AddPass({"GizmoComposite"});
    g.Read(composite, gizmoColor, RGAccess::Sampled);
    g.Read(composite, finalColor, RGAccess::ColorLoad); // AttachColor(Load) derivation
    g.Write(composite, finalColor, RGAccess::ColorAttachment);
    Bake(g);

    // The export anchors cull through the whole overlay chain — no
    // PreventCulling anywhere.
    EXPECT_FALSE(g.IsCulled(world));
    EXPECT_FALSE(g.IsCulled(overlay));
    EXPECT_FALSE(g.IsCulled(composite));

    // Exactly ONE ColorAttachment->ShaderReadOnly export transition for
    // FinalColor, and it lives in the trailing sentinel batch (after every
    // scheduled pass), not between world and composite.
    const RGBarrier* exportT = nullptr;
    uint32_t exportCount = 0;
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource == finalColor && b.NewLayout == RGImageLayout::ShaderReadOnly)
        {
            exportT = &b;
            ++exportCount;
        }
    }
    ASSERT_NE(exportT, nullptr);
    EXPECT_EQ(exportCount, 1u);
    EXPECT_EQ(exportT->OldLayout, RGImageLayout::ColorAttachment);

    const RGBarrierBatch* sentinel = nullptr;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == kInvalidId)
            sentinel = &batch;
    ASSERT_NE(sentinel, nullptr) << "export must ride the trailing sentinel batch";
    EXPECT_EQ(sentinel->ScheduledIndex, static_cast<uint32_t>(g.ScheduledOrder().size()));
    const uint32_t exportIdx = static_cast<uint32_t>(exportT - g.Barriers().data());
    EXPECT_GE(exportIdx, sentinel->First);
    EXPECT_LT(exportIdx, sentinel->First + sentinel->Count);

    EXPECT_EQ(g.FinalLayout(finalColor), RGImageLayout::ShaderReadOnly);
}

TEST(RGBarrier, ImportedTextureTransitionsFromTrackedLayout)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId hist = Tex(g, "History");
    g.MarkImported(hist, RGImageLayout::ShaderReadOnly); // last frame left it sampled
    RGPassId draw = g.AddPass({"WriteHistory"});
    g.Write(draw, hist, RGAccess::ColorAttachment);
    Bake(g);

    ASSERT_EQ(g.BarrierCount(), 1u);
    const RGBarrier* b = FindBarrier(g, hist);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->OldLayout, RGImageLayout::ShaderReadOnly) << "must start from the tracked layout, not Undefined";
    EXPECT_EQ(b->NewLayout, RGImageLayout::ColorAttachment);
}

// Review fix (1): barrier-less reads must ACCUMULATE into the cell, not
// overwrite it. A second reader at a stage never synchronized against the
// producer gets an expansion barrier whose source is the WRITER's scope.
TEST(RGBarrier, SecondReaderAtNewStageGetsExpansionBarrier)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId w = g.AddPass({"Writer"});
    g.Write(w, x, RGAccess::StorageWrite);
    RGPassId r1 = g.AddPass({"IndirectReader"});
    g.Read(r1, x, RGAccess::IndirectRead); // DrawIndirect stage
    g.Write(r1, o1, RGAccess::StorageWrite);
    RGPassId r2 = g.AddPass({"VertexReader"});
    g.Read(r2, x, RGAccess::VertexRead); // VertexInput stage — never synchronized
    g.Write(r2, o2, RGAccess::StorageWrite);
    Bake(g);

    uint32_t barriersOnX = 0;
    const RGBarrier* expansion = nullptr;
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != x)
            continue;
        ++barriersOnX;
        if ((b.DstStage & RGStage::VertexInput) != 0)
            expansion = &b;
    }
    EXPECT_EQ(barriersOnX, 3u) << "first-write exec dep + RAW for reader 1 + expansion for reader 2";
    ASSERT_NE(expansion, nullptr) << "the second reader must get its own barrier";
    EXPECT_TRUE((expansion->SrcStage & RGStage::ComputeShader) != 0)
        << "expansion source is the WRITER's stage, not the previous reader's";
    EXPECT_TRUE((expansion->SrcAccess & RGAccessMask::ShaderWrite) != 0);
}

// Review fix (1b): a write after multiple readers must WAR-wait on the UNION of
// all synchronized reader stages, not just the most recent one.
TEST(RGBarrier, WriteAfterMultipleReadersWaitsOnAllReaderStages)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(x);
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId w = g.AddPass({"Writer"});
    g.Write(w, x, RGAccess::StorageWrite);
    RGPassId r1 = g.AddPass({"IndirectReader"});
    g.Read(r1, x, RGAccess::IndirectRead);
    g.Write(r1, o1, RGAccess::StorageWrite);
    RGPassId r2 = g.AddPass({"VertexReader"});
    g.Read(r2, x, RGAccess::VertexRead);
    g.Write(r2, o2, RGAccess::StorageWrite);
    RGPassId w2 = g.AddPass({"Rewriter"});
    g.Write(w2, x, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* war = nullptr;
    int pos = -1;
    const auto& ord = g.ScheduledOrder();
    for (int i = 0; i < static_cast<int>(ord.size()); ++i)
        if (ord[i] == w2)
            pos = i;
    ASSERT_GE(pos, 0);
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == w2)
            for (uint32_t i = 0; i < batch.Count; ++i)
                if (g.Barriers()[batch.First + i].Resource == x)
                    war = &g.Barriers()[batch.First + i];
    ASSERT_NE(war, nullptr);
    EXPECT_TRUE((war->SrcStage & RGStage::DrawIndirect) != 0) << "must wait on reader 1's stage";
    EXPECT_TRUE((war->SrcStage & RGStage::VertexInput) != 0) << "must wait on reader 2's stage";
}

// Review fix (2): access mapping is queue-aware — Sampled on a compute queue
// must target the compute stage (FragmentShader is an invalid mask there).
TEST(RGBarrier, ComputeQueueSampledMapsToComputeStage)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "SceneColor");
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);
    RGPassId draw = g.AddPass({"Draw", 0, RGQueue::Graphics});
    g.Write(draw, t, RGAccess::ColorAttachment);
    RGPassId comp = g.AddPass({"ComputeSample", 0, RGQueue::Compute});
    g.Read(comp, t, RGAccess::Sampled);
    g.Write(comp, out, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* sample = nullptr;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == t && b.NewLayout == RGImageLayout::ShaderReadOnly)
            sample = &b;
    ASSERT_NE(sample, nullptr);
    EXPECT_TRUE((sample->DstStage & RGStage::ComputeShader) != 0);
    EXPECT_TRUE((sample->DstStage & RGStage::FragmentShader) == 0)
        << "FragmentShader is an invalid stage mask on a compute queue";
}

// Review fix (3): first-use/imported source scope must be BottomOfPipe — a
// recycled pooled physical may still be touched by a prior in-flight frame, and
// TopOfPipe creates no execution dependency at all.
TEST(RGBarrier, FirstUseAndImportedSourceIsBottomOfPipe)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId fresh = Tex(g, "Fresh");
    RGResourceId hist = Tex(g, "History");
    g.MarkExternal(fresh);
    g.MarkImported(hist, RGImageLayout::ShaderReadOnly);
    RGPassId draw = g.AddPass({"Draw"});
    g.Write(draw, fresh, RGAccess::ColorAttachment); // first use (recycled memory)
    g.Write(draw, hist, RGAccess::ColorAttachment);  // imported from pool
    Bake(g);

    for (const RGBarrier& b : g.Barriers())
        EXPECT_TRUE((b.SrcStage & RGStage::BottomOfPipe) != 0)
            << "init/import transitions need an execution dependency on ALL prior work";
}

// Review fix (5): reads never take queue ownership. Cross-queue reads are
// synchronized by the submission plan's semaphores; the cells must not
// ping-pong SrcQueue and fabricate ownership-transfer halves.
TEST(RGBarrier, CrossQueueReadsDoNotPingPongOwnership)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId w = g.AddPass({"Writer", 0, RGQueue::Graphics});
    g.Write(w, x, RGAccess::StorageWrite);
    RGPassId rc = g.AddPass({"ComputeReader", 0, RGQueue::Compute});
    g.Read(rc, x, RGAccess::StorageRead);
    g.Write(rc, o1, RGAccess::StorageWrite);
    RGPassId rg2 = g.AddPass({"GraphicsReader", 0, RGQueue::Graphics});
    g.Read(rg2, x, RGAccess::StorageRead);
    g.Write(rg2, o2, RGAccess::StorageWrite);
    Bake(g);

    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != x)
            continue;
        EXPECT_EQ(b.SrcQueue, static_cast<uint32_t>(RGQueue::Graphics))
            << "source queue stays the writer's; reads must not flip ownership";
    }
}

// Cross-queue HZB fix (part 2): a texture read that also performs a LAYOUT
// TRANSITION is a PRODUCER (it rewrites WriteStage/Layout) and MUST take queue
// ownership, unlike a pure read (CrossQueueReadsDoNotPingPongOwnership above,
// which only exercises buffers — buffers never transition). Compute writes X in
// GENERAL, graphics SAMPLES it (General->ShaderReadOnly transition), then compute
// reads it again (back to General). The graphics transition must own the cell so
// the later compute barrier sources from GRAPHICS (making it cross-physical and
// eligible for the recording layer's CrossQueueConsumerSrcScope rewrite). Before
// the fix, ownership stayed COMPUTE while WriteStage held the graphics FRAGMENT
// stage, so the compute barrier looked same-physical and emitted a raw FRAGMENT
// stage on the compute queue (VUID-vkCmdPipelineBarrier2-srcStageMask-09675).
// Pure IR, no GPU.
TEST(RGBarrier, TextureReadLayoutTransitionTakesQueueOwnership)
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
    g.Read(rg, x, RGAccess::Sampled); // read-LAYOUT-TRANSITION General->ShaderReadOnly
    g.Write(rg, o1, RGAccess::ColorAttachment);
    RGPassId rc = g.AddPass({"ComputeReadAgain", 0, RGQueue::Compute});
    g.Read(rc, x, RGAccess::StorageRead); // transition ShaderReadOnly->General
    g.Write(rc, o2, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* toSample = nullptr;      // General -> ShaderReadOnly (graphics)
    const RGBarrier* toComputeAgain = nullptr; // ShaderReadOnly -> General (compute)
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != x || !b.IsTexture)
            continue;
        if (b.NewLayout == RGImageLayout::ShaderReadOnly)
            toSample = &b;
        else if (b.NewLayout == RGImageLayout::General &&
                 b.OldLayout == RGImageLayout::ShaderReadOnly)
            toComputeAgain = &b;
    }

    // The graphics sample's transition is PRODUCED by the compute writer.
    ASSERT_NE(toSample, nullptr);
    EXPECT_EQ(toSample->SrcQueue, static_cast<uint32_t>(RGQueue::Compute));
    EXPECT_EQ(toSample->DstQueue, static_cast<uint32_t>(RGQueue::Graphics));

    // THE FIX: the graphics read-transition took ownership, so the subsequent
    // compute consumer sources from GRAPHICS (not the stale compute writer), and
    // still carries the graphics fragment stage in the advisory IR.
    ASSERT_NE(toComputeAgain, nullptr);
    EXPECT_EQ(toComputeAgain->SrcQueue, static_cast<uint32_t>(RGQueue::Graphics))
        << "a read-layout-transition must take queue ownership";
    EXPECT_EQ(toComputeAgain->DstQueue, static_cast<uint32_t>(RGQueue::Compute));
    EXPECT_TRUE((toComputeAgain->SrcStage & RGStage::FragmentShader) != 0)
        << "carries the transition's fragment stage (recording layer sanitizes it)";

    // Mirror the recording layer's decision (RGRecord.cpp: crossPhysical =
    // physicalOf[b.SrcQueue] != subPhysicalQueue) under the identity map: the
    // compute consumer is CROSS-physical -> its src scope is rewritten via
    // CrossQueueConsumerSrcScope; a graphics consumer would be SAME-physical ->
    // raw stage kept. A stale compute SrcQueue would have inverted both (the bug).
    const auto* pm = RGGraph::kIdentityQueueMap;
    EXPECT_NE(pm[toComputeAgain->SrcQueue], pm[static_cast<uint32_t>(RGQueue::Compute)])
        << "compute consumer is cross-physical -> src scope must be rewritten";
    EXPECT_EQ(pm[toComputeAgain->SrcQueue], pm[static_cast<uint32_t>(RGQueue::Graphics)])
        << "a graphics consumer would be same-physical -> raw stage kept";
}

// Review fix (M4): combining DepthRead + Sampled on one texture in one pass must
// resolve to the same layout regardless of declaration order (DepthReadOnly is
// valid for both usages).
TEST(RGBarrier, SamePassDepthReadPlusSampledIsOrderIndependent)
{
    auto build = [](bool depthFirst) -> RGImageLayout
    {
        RGGraph g;
        g.BeginFrame();
        RGResourceId depth = Tex(g, "Depth");
        RGResourceId color = Tex(g, "Color");
        g.MarkExternal(color);
        RGPassId pre = g.AddPass({"DepthPrepass"});
        g.Write(pre, depth, RGAccess::DepthWrite);
        RGPassId world = g.AddPass({"World"});
        if (depthFirst)
        {
            g.Read(world, depth, RGAccess::DepthRead);
            g.Read(world, depth, RGAccess::Sampled);
        }
        else
        {
            g.Read(world, depth, RGAccess::Sampled);
            g.Read(world, depth, RGAccess::DepthRead);
        }
        g.Write(world, color, RGAccess::ColorAttachment);
        Bake(g);
        for (const RGBarrier& b : g.Barriers())
            if (b.Resource == depth && b.OldLayout == RGImageLayout::DepthAttachment)
                return b.NewLayout;
        return RGImageLayout::Undefined;
    };

    EXPECT_EQ(build(true), RGImageLayout::DepthReadOnly);
    EXPECT_EQ(build(false), RGImageLayout::DepthReadOnly)
        << "layout must not depend on declaration order";
}

// Re-review fix (1): read coverage is QUEUE-LOCAL. A graphics reader's stage
// bits say nothing about the compute queue's timeline — W(C)→R1(G)→R2(C) used
// to leave R2 with zero barrier AND zero semaphore (same-queue early return).
TEST(RGBarrier, ReadCoverageIsQueueLocal)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId w = g.AddPass({"Writer", 0, RGQueue::Compute});
    g.Write(w, x, RGAccess::StorageWrite);
    RGPassId r1 = g.AddPass({"GraphicsReader", 0, RGQueue::Graphics});
    g.Read(r1, x, RGAccess::StorageRead);
    g.Write(r1, o1, RGAccess::StorageWrite);
    RGPassId r2 = g.AddPass({"ComputeReader", 0, RGQueue::Compute});
    g.Read(r2, x, RGAccess::StorageRead);
    g.Write(r2, o2, RGAccess::StorageWrite);
    Bake(g);

    bool r2HasXBarrier = false;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == r2)
            for (uint32_t i = 0; i < batch.Count; ++i)
                if (g.Barriers()[batch.First + i].Resource == x)
                    r2HasXBarrier = true;
    EXPECT_TRUE(r2HasXBarrier)
        << "the compute reader is NOT covered by the graphics reader's coverage";
}

// Re-review fix (2): a freshly-created (pool-recycled) BUFFER's first write must
// emit an execution dependency — the transient pool blanket-frees at CPU frame
// end, so the physical may still be touched by a prior in-flight frame.
TEST(RGBarrier, RecycledBufferFirstWriteEmitsExecDep)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    g.MarkExternal(x);
    RGPassId w = g.AddPass({"Writer"});
    g.Write(w, x, RGAccess::StorageWrite);
    Bake(g);

    ASSERT_EQ(g.BarrierCount(), 1u);
    EXPECT_TRUE((g.Barriers()[0].SrcStage & RGStage::BottomOfPipe) != 0)
        << "recycled-memory exec dep, hoistable to the submission front";
    EXPECT_EQ(g.Barriers()[0].SrcAccess, RGAccessMask::None);
}

// Re-review fix (5): a read-layout TRANSITION is a producer even though it
// stores read access bits — a later reader on a NEW queue (uncovered slot) must
// still chain from it.
TEST(RGBarrier, NewQueueReaderAfterReadTransitionGetsBarrier)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "SceneColor");
    RGResourceId o1 = Buf(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId draw = g.AddPass({"Draw", 0, RGQueue::Graphics});
    g.Write(draw, t, RGAccess::ColorAttachment);
    RGPassId r1 = g.AddPass({"SampleG", 0, RGQueue::Graphics});
    g.Read(r1, t, RGAccess::Sampled); // Color -> ShaderReadOnly transition
    g.Write(r1, o1, RGAccess::StorageWrite);
    RGPassId r2 = g.AddPass({"SampleC", 0, RGQueue::Compute});
    g.Read(r2, t, RGAccess::Sampled); // same layout, new queue
    g.Write(r2, o2, RGAccess::StorageWrite);
    Bake(g);

    bool r2HasTBarrier = false;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == r2)
            for (uint32_t i = 0; i < batch.Count; ++i)
                if (g.Barriers()[batch.First + i].Resource == t)
                    r2HasTBarrier = true;
    EXPECT_TRUE(r2HasTBarrier) << "the transition is the producer; the new-queue reader chains from it";
}

TEST(RGBarrier, MultipleInputsCoalesceIntoOnePassBatch)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId a = Buf(g, "A");
    RGResourceId b = Buf(g, "B");
    RGResourceId c = Buf(g, "C");
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);
    RGPassId wa = g.AddPass({"WA"});
    g.Write(wa, a, RGAccess::StorageWrite);
    RGPassId wb = g.AddPass({"WB"});
    g.Write(wb, b, RGAccess::StorageWrite);
    RGPassId wc = g.AddPass({"WC"});
    g.Write(wc, c, RGAccess::StorageWrite);
    RGPassId rd = g.AddPass({"ReadAll"});
    g.Read(rd, a, RGAccess::StorageRead);
    g.Read(rd, b, RGAccess::StorageRead);
    g.Read(rd, c, RGAccess::StorageRead);
    g.Write(rd, out, RGAccess::StorageWrite);
    Bake(g);

    // ReadAll's batch coalesces its 3 RAW transitions plus its own output's
    // first-write into ONE call (writers carry their own first-write batches).
    const RGBarrierBatch* rdBatch = nullptr;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == rd)
            rdBatch = &batch;
    ASSERT_NE(rdBatch, nullptr);
    EXPECT_EQ(rdBatch->Count, 4u) << "3 RAW + own output first-write, one call";
    EXPECT_TRUE((rdBatch->SrcStageUnion & RGStage::ComputeShader) != 0);
    EXPECT_TRUE((rdBatch->DstStageUnion & RGStage::ComputeShader) != 0);
    uint32_t rawCount = 0;
    for (uint32_t i = 0; i < rdBatch->Count; ++i)
        if ((g.Barriers()[rdBatch->First + i].SrcAccess & RGAccessMask::ShaderWrite) != 0)
            ++rawCount;
    EXPECT_EQ(rawCount, 3u);
}

// Audit batch-1 pin (1b): under a COLLAPSED physical-queue map (the production
// default), a compute-logical writer's WAR source scope must include a
// graphics-logical reader's stages — same physical queue means no semaphore,
// so the barrier is the only synchronization there is.
TEST(RGBarrier, CollapsedMapWARCoversCrossLogicalReaders)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Buf(g, "X");
    g.MarkImported(x, RGImageLayout::Undefined);
    RGPassId r = g.AddPass({"ReadGfx"});
    g.PreventCulling(r); // consume-only reader: keep it live
    g.Read(r, x, RGAccess::StorageRead);
    RGPassDesc wd;
    wd.Name = "WriteCompute";
    wd.Queue = RGQueue::Compute;
    RGPassId w = g.AddPass(wd);
    g.Write(w, x, RGAccess::StorageWrite);
    g.Compile();
    g.Schedule();
    constexpr uint8_t kCollapsed[kQueueCount] = {0, 0, 0};
    g.GenerateBarriers(kCollapsed);

    const RGBarrier* war = nullptr;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == x && (b.DstAccess & RGAccessMask::WriteBits) != 0)
            war = &b;
    ASSERT_NE(war, nullptr);
    // Graphics StorageRead contributes FragmentShader — a bit only the reader
    // supplies. Logical-queue indexing (the bug) dropped it entirely.
    EXPECT_TRUE((war->SrcStage & RGStage::FragmentShader) != 0)
        << "collapsed-map WAR must wait on the cross-logical reader's stages";
}

// Audit batch-1 pin (1c): FirstTouch is generation ground truth for the
// submission-front hoist. A writer AFTER pure reads of an import inherits
// BottomOfPipe in its source scope (the old hoist heuristic) but must NOT be
// flagged first-touch — it has intra-frame consumers to stay ordered against.
TEST(RGBarrier, WriterAfterImportedReadIsNotFirstTouch)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId t = Tex(g, "History");
    g.MarkImported(t, RGImageLayout::ShaderReadOnly);
    RGPassId r = g.AddPass({"SampleHistory"});
    g.PreventCulling(r); // consume-only reader: keep it live
    g.Read(r, t, RGAccess::Sampled); // same layout: import-sync barrier only
    RGPassId w = g.AddPass({"OverwriteHistory"});
    g.Write(w, t, RGAccess::ColorAttachment); // layout change after the read
    Bake(g);

    const RGBarrier* firstTouch = nullptr;
    const RGBarrier* writeBar = nullptr;
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != t)
            continue;
        if (b.NewLayout == RGImageLayout::ColorAttachment)
            writeBar = &b;
        else
            firstTouch = &b;
    }
    ASSERT_NE(firstTouch, nullptr);
    ASSERT_NE(writeBar, nullptr);
    EXPECT_TRUE(firstTouch->FirstTouch) << "the import-sync barrier is hoist-safe";
    EXPECT_FALSE(writeBar->FirstTouch)
        << "a WAR barrier after readers must stay in schedule position";
    EXPECT_TRUE((writeBar->SrcStage & RGStage::BottomOfPipe) != 0)
        << "precondition: the old BottomOfPipe heuristic WOULD have hoisted this";
}

// Slice 4 (barrier-tracker redesign): the descriptor-claim constraint that
// subsumes the PinGeneralLayout read-pin. A texture whose sampled descriptors
// claim GENERAL (TextureDesc::sampledInGeneralLayout) must never transition to
// ShaderReadOnly: compute storage-writes it, graphics samples it, compute
// storage-reads it — the DepthResolved shape whose layout ping-pong tripped
// VUID-09600/09675 with async compute. Every barrier stays in GENERAL, so no
// cross-queue layout-producer edges are needed for it.
TEST(RGBarrier, SampledInGeneralTextureStaysGeneralAcrossQueues)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = TexGeneral(g, "DepthResolved");
    RGResourceId o1 = Tex(g, "Out1");
    RGResourceId o2 = Buf(g, "Out2");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId wc = g.AddPass({"Resolve", 0, RGQueue::Compute});
    g.Write(wc, x, RGAccess::StorageWrite);
    RGPassId rg = g.AddPass({"WorldSample", 0, RGQueue::Graphics});
    g.Read(rg, x, RGAccess::Sampled);
    g.Write(rg, o1, RGAccess::ColorAttachment);
    RGPassId rc = g.AddPass({"HzbRead", 0, RGQueue::Compute});
    g.Read(rc, x, RGAccess::StorageRead);
    g.Write(rc, o2, RGAccess::StorageWrite);
    Bake(g);

    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != x)
            continue;
        EXPECT_EQ(b.NewLayout, RGImageLayout::General)
            << "sampled reads must observe the claimed GENERAL layout";
        if (b.OldLayout != RGImageLayout::Undefined) // first write discards
            EXPECT_EQ(b.OldLayout, RGImageLayout::General) << "no layout ping-pong";
    }
}

// The constraint is scoped to SAMPLED reads: a copy read of the same texture
// keeps its API-required TransferSrc layout. The retired read-pin forced
// GENERAL onto every read including copies, contradicting the backend copy
// path's TRANSFER_SRC transition; the mix is ordered by the submission plan's
// RAW/WAR-on-layout edges instead.
TEST(RGBarrier, SampledInGeneralCopyReadKeepsTransferLayout)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = TexGeneral(g, "DepthResolved");
    RGResourceId o1 = Tex(g, "Out1");
    RGResourceId o2 = Buf(g, "Readback");
    g.MarkExternal(o1);
    g.MarkExternal(o2);
    RGPassId wc = g.AddPass({"Resolve", 0, RGQueue::Compute});
    g.Write(wc, x, RGAccess::StorageWrite);
    RGPassId rg = g.AddPass({"WorldSample", 0, RGQueue::Graphics});
    g.Read(rg, x, RGAccess::Sampled); // stays General (claimed layout)
    g.Write(rg, o1, RGAccess::ColorAttachment);
    RGPassId copy = g.AddPass({"Capture", 0, RGQueue::Graphics});
    g.Read(copy, x, RGAccess::CopySrc); // General -> TransferSrc (API-required)
    g.Write(copy, o2, RGAccess::CopyDst);
    Bake(g);

    const RGBarrier* toTransfer = nullptr;
    for (const RGBarrier& b : g.Barriers())
        if (b.Resource == x && b.NewLayout == RGImageLayout::TransferSrc)
            toTransfer = &b;
    ASSERT_NE(toTransfer, nullptr) << "a copy read must transition to TransferSrc";
    EXPECT_EQ(toTransfer->OldLayout, RGImageLayout::General);
}

// The merge-63 replacement shape, per-subresource: an imported array partially
// written per-layer in separate passes (the CSM cascade contract), then read
// whole-array. The compiled per-cell truth must give the reader's barriers the
// exact per-rectangle OldLayouts — DepthAttachment for the written layers, the
// imported layout for the untouched ones — because under the graph-authoritative
// rule the backend records these verbatim (the deleted cross-submission
// recovery used to patch this shape at record time instead).
TEST(RGBarrier, PartialPerLayerWriteThenWholeArrayReadCarriesPerRectangleOldLayouts)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId arr = TexArray(g, "ShadowArray", 4);
    RGResourceId out = Buf(g, "Out");
    g.MarkExternal(out);
    g.MarkImported(arr, RGImageLayout::ShaderReadOnly); // resting sampled state
    RGPassId c0 = g.AddPass({"Cascade0"});
    g.Write(c0, arr, RGAccess::DepthWrite, RGRange{0, 1, 0, 1});
    RGPassId c1 = g.AddPass({"Cascade1"});
    g.Write(c1, arr, RGAccess::DepthWrite, RGRange{0, 1, 1, 1});
    RGPassId world = g.AddPass({"World"});
    g.Read(world, arr, RGAccess::Sampled); // whole array
    g.Write(world, out, RGAccess::StorageWrite);
    Bake(g);

    const RGBarrier* writtenRect = nullptr;   // layers 0-1: DepthAttachment -> sampled
    const RGBarrier* untouchedRect = nullptr; // layers 2-3: imported first-touch sync
    for (const RGBarrierBatch& batch : g.BarrierBatches())
    {
        if (batch.Pass != world)
            continue;
        for (uint32_t i = 0; i < batch.Count; ++i)
        {
            const RGBarrier& b = g.Barriers()[batch.First + i];
            if (b.Resource != arr)
                continue;
            if (b.OldLayout == RGImageLayout::DepthAttachment)
            {
                EXPECT_EQ(writtenRect, nullptr) << "written layers merge into one rectangle";
                writtenRect = &b;
            }
            else
            {
                EXPECT_EQ(b.OldLayout, RGImageLayout::ShaderReadOnly)
                    << "untouched layers keep the imported layout";
                EXPECT_EQ(untouchedRect, nullptr) << "untouched layers merge into one rectangle";
                untouchedRect = &b;
            }
        }
    }
    ASSERT_NE(writtenRect, nullptr);
    EXPECT_EQ(writtenRect->Range.BaseLayer, 0u);
    EXPECT_EQ(writtenRect->Range.LayerCount, 2u);
    EXPECT_EQ(writtenRect->NewLayout, RGImageLayout::ShaderReadOnly);
    ASSERT_NE(untouchedRect, nullptr);
    EXPECT_EQ(untouchedRect->Range.BaseLayer, 2u);
    EXPECT_EQ(untouchedRect->Range.LayerCount, 2u);
    EXPECT_EQ(untouchedRect->NewLayout, RGImageLayout::ShaderReadOnly);
}

// The partner shape with NO whole-array reader: the frame ends heterogeneous
// (written layers DepthAttachment, untouched layers at the imported layout),
// so the NORMALIZE arm of the export sentinel batch must equalize the array —
// the pool stores ONE state and the next frame's authoritative seed depends on
// it (FinalLayout asserts uniformity).
TEST(RGBarrier, PartialPerLayerWriteWithoutReaderEmitsNormalizeSentinel)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId arr = TexArray(g, "ShadowArray", 4);
    g.MarkImported(arr, RGImageLayout::ShaderReadOnly);
    RGPassId c0 = g.AddPass({"Cascade0"});
    g.Write(c0, arr, RGAccess::DepthWrite, RGRange{0, 1, 0, 1});
    RGPassId c1 = g.AddPass({"Cascade1"});
    g.Write(c1, arr, RGAccess::DepthWrite, RGRange{0, 1, 1, 1});
    Bake(g);

    // The sentinel batch (Pass == kInvalidId, ScheduledIndex == pass count)
    // must equalize the untouched layers to the written layers' layout.
    const RGBarrierBatch* sentinel = nullptr;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == kInvalidId)
            sentinel = &batch;
    ASSERT_NE(sentinel, nullptr) << "heterogeneous imported array must normalize at frame end";
    const RGBarrier* equalize = nullptr;
    for (uint32_t i = 0; i < sentinel->Count; ++i)
    {
        const RGBarrier& b = g.Barriers()[sentinel->First + i];
        if (b.Resource != arr)
            continue;
        EXPECT_EQ(equalize, nullptr) << "untouched layers merge into one rectangle";
        equalize = &b;
    }
    ASSERT_NE(equalize, nullptr);
    EXPECT_EQ(equalize->OldLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(equalize->NewLayout, RGImageLayout::DepthAttachment)
        << "normalize target is the first cell's (written) layout";
    EXPECT_EQ(equalize->Range.BaseLayer, 2u);
    EXPECT_EQ(equalize->Range.LayerCount, 2u);
    // Post-normalize the resource is uniform — the pool write-back contract.
    EXPECT_EQ(g.FinalLayout(arr), RGImageLayout::DepthAttachment);
}

// The IBL bake-frame export contract (the VUID-09600 startup-storm fix,
// re-homed from the deleted graphics "settle" reads). A compute StorageWrite
// leaves an imported external GENERAL; the export contract (MarkOutput at
// ShaderReadOnly) restores the resting-state claim with ONE sentinel
// transition whose source queue is the COMPUTE writer — the submission plan
// places it on that queue's tail with derived edges (RGSubmission twins).
// That keeps the import-at-ShaderResource claim of every LATER frame TRUE:
// under the graph-authoritative oldLayout rule a false claim compiles a
// no-op barrier that parks the image in GENERAL under sampling, with no
// tracker to absorb it.
TEST(RGBarrier, ComputeBakeExportRestoresRestingLayoutFromComputeQueue)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId x = Tex(g, "IBL_Irradiance");
    g.MarkImported(x, RGImageLayout::ShaderReadOnly); // the resting-state claim
    g.MarkExternal(x);
    g.SetExportLayout(x, RGImageLayout::ShaderReadOnly);
    RGPassId bake = g.AddPass({"Convolve", 0, RGQueue::Compute});
    g.Write(bake, x, RGAccess::StorageWrite); // -> GENERAL on compute
    Bake(g);

    EXPECT_EQ(g.FinalLayout(x), RGImageLayout::ShaderReadOnly)
        << "a bake frame must end at the resting-state claim";
    const RGBarrierBatch* sentinel = nullptr;
    for (const RGBarrierBatch& batch : g.BarrierBatches())
        if (batch.Pass == kInvalidId)
            sentinel = &batch;
    ASSERT_NE(sentinel, nullptr) << "the restore rides the trailing sentinel batch";
    const RGBarrier* restore = nullptr;
    for (uint32_t i = 0; i < sentinel->Count; ++i)
    {
        const RGBarrier& b = g.Barriers()[sentinel->First + i];
        if (b.Resource != x)
            continue;
        EXPECT_EQ(restore, nullptr) << "exactly one restore transition";
        restore = &b;
    }
    ASSERT_NE(restore, nullptr);
    EXPECT_EQ(restore->OldLayout, RGImageLayout::General);
    EXPECT_EQ(restore->NewLayout, RGImageLayout::ShaderReadOnly);
    EXPECT_EQ(restore->SrcQueue, static_cast<uint32_t>(RGQueue::Compute))
        << "the compute writer owns the cells — placement and scope derive from it";
    EXPECT_TRUE((restore->SrcStage & RGStage::ComputeShader) != 0)
        << "own-queue source scope, expressible verbatim on the compute tail";
    EXPECT_EQ(restore->SrcStage & RGStage::BottomOfPipe, 0u)
        << "export barriers must never be frame-front hoisted";
}

// Why a read-only depth attachment must record no store op (RGRecord twin:
// ReadOnlyDepthAttachRecordsNoStore). A read that changes layout BECOMES the
// cell's producer scope, so after a read-only depth pass the tracked producer
// is a DepthRead — and the next depth writer's barrier is built from it: a
// DepthReadOnly -> DepthAttachment transition whose source access is DepthRead
// and nothing else. Any depth-attachment WRITE the pass performs outside its
// declared access (a store op) is therefore never made available across that
// transition. That is the editor game view's every-frame WRITE_AFTER_WRITE:
// DepthPrepass(A) -> world (read-only depth) -> DepthPrepass(B) recover.
//
// CHARACTERIZATION PIN, not a regression test for the store-op fix. It drives
// RGGraph directly and never calls AttachDepth, so it stays green whether or not
// a read-only attach records RGStoreOp::None. What it pins is the barrier
// derivation the fix RELIES on — the reason a store op is unsafe here. The
// regression arm is the RGRecord twin named above.
TEST(RGBarrier, ReadOnlyDepthProducerScopeCarriesNoWrite)
{
    RGGraph g;
    g.BeginFrame();
    RGResourceId depth = Tex(g, "View.Depth");
    RGResourceId color = Tex(g, "View.Color");
    // Both survive the cull the way the real ones do: the game view's depth and
    // colour are pool imports the next frame reads.
    g.MarkExternal(depth);
    g.MarkExternal(color);
    RGPassId prepass = g.AddPass({"DepthPrepass"});
    g.Write(prepass, depth, RGAccess::DepthWrite);
    RGPassId world = g.AddPass({"RenderEntities"}); // read-only depth attach
    g.Read(world, depth, RGAccess::DepthRead);
    g.Write(world, color, RGAccess::ColorAttachment);
    RGPassId recover = g.AddPass({"DepthPrepassB"}); // Load + ReadWrite attach
    g.Read(recover, depth, RGAccess::DepthRead);
    g.Write(recover, depth, RGAccess::DepthWrite);
    Bake(g);

    ASSERT_EQ(g.ScheduledOrder().size(), 3u)
        << "the chain must survive the cull or there are no barriers to inspect";

    const RGBarrier* toReadOnly = nullptr;
    const RGBarrier* backToAttachment = nullptr;
    for (const RGBarrier& b : g.Barriers())
    {
        if (b.Resource != depth)
            continue;
        if (b.OldLayout == RGImageLayout::DepthAttachment &&
            b.NewLayout == RGImageLayout::DepthReadOnly)
            toReadOnly = &b;
        else if (b.OldLayout == RGImageLayout::DepthReadOnly &&
                 b.NewLayout == RGImageLayout::DepthAttachment)
            backToAttachment = &b;
    }

    // The prepass write IS made available to the read-only pass.
    ASSERT_NE(toReadOnly, nullptr);
    EXPECT_TRUE((toReadOnly->SrcAccess & RGAccessMask::DepthWrite) != 0);

    // The recover pass's transition carries no write: the read-only pass is not
    // a producer of depth CONTENT, so nothing after it can be relying on this
    // barrier to make a depth write available. The source scope is asserted by
    // what it must NOT contain — an exact mask would fail on any future read
    // access that legitimately joins it.
    ASSERT_NE(backToAttachment, nullptr);
    EXPECT_TRUE((backToAttachment->SrcAccess & RGAccessMask::DepthRead) != 0);
    EXPECT_EQ(backToAttachment->SrcAccess & RGAccessMask::WriteBits, 0u)
        << "a store op in the read-only pass would be an unsynchronized WAW here";
    EXPECT_TRUE((backToAttachment->DstAccess & RGAccessMask::DepthWrite) != 0);
    EXPECT_TRUE((backToAttachment->SrcStage & RGStage::LateFragmentTests) != 0);
}
