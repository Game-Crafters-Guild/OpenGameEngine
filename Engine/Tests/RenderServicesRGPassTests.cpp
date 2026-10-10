#include <gtest/gtest.h>

#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowFrameInfo.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSsboLayout.h"
#include "Engine/Rendering/PerFrameWritePool.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "AssetCore/GUID.h"
#include "Types/StringId.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

#include "PerFrameWritePoolRotation.h"
#include "TestDeviceHelper.h"

// Slice-3 pins for the RenderGraph pass arms (AddWorldPassForView /
// AddWorldDepthPrepassForView / AddShadowCascadePassForView /
// AddAreaShadowPassForView on RGFrame). Twin of
// RenderServicesContributorDispatchTests for the new arms, plus the
// attachment/ordering/shadow-identity contracts the old graph faked with
// order fences, activation predicates and PreventCulling.

namespace
{

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(Rendering::IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 65536) {}
};

TextureDesc ColorTargetDesc(uint32_t samples = 1,
                            TextureFormat format = TextureFormat::RGBA8_UNORM)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = samples;
    d.format = static_cast<uint32_t>(format);
    d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
    return d;
}

TextureDesc DepthTargetDesc()
{
    TextureDesc d = ColorTargetDesc();
    d.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    return d;
}

// Common per-test stand-up: device + RenderServices + one view with a clear
// request (clear-only frames must still declare/run the world pass).
struct RsFixture
{
    std::unique_ptr<Rendering::IDevice> Device;
    RenderServices Rs;
    ViewId View{};

    bool Up()
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        if (!Rs.Initialize(Device.get()))
            return false;
        const CameraId camId = Rs.Views().AllocateCamera("RGPassTestCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        Rs.Views().SetCameraData(camId, cd);
        View = Rs.Views().AllocateView("RGPassTestView", camId);
        Rs.Views().SetViewRenderLayerMask(View, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearColorValue[3] = 1.0f;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        // Targets stay 0 — the RenderGraph arms take frame-local values; the view
        // carries only the clear config (and camera) for the new path.
        Rs.Views().SetViewTargets(View, 0, 0, 0, clear);
        return true;
    }

    void Down()
    {
        Rs.Shutdown();
        Device->Shutdown();
    }
};

size_t ScheduledIndexOf(const RenderGraph::RGGraph& g, RenderGraph::RGPassId pass)
{
    const auto& order = g.ScheduledOrder();
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == pass)
            return i;
    return SIZE_MAX;
}

// Declared-access lookups by name, for contracts whose resources and passes are
// private to the declaring translation unit. Both scan the flat access records,
// so kInvalidId means "no pass declared any access on it" — which for a bound
// resource is itself the defect.
RenderGraph::RGPassId PassIdByName(const RenderGraph::RGGraph& g, const std::string& name)
{
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        if (name == g.PassName(a.Pass))
            return a.Pass;
    return RenderGraph::kInvalidId;
}

RenderGraph::RGResourceId ResourceIdByName(const RenderGraph::RGGraph& g, const std::string& name)
{
    for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        if (name == g.ResourceName(a.Resource))
            return a.Resource;
    return RenderGraph::kInvalidId;
}

// Minimal StandardPBR registration via the RenderServices surface — enough for
// the material to earn a MaterialParams SSBO index (headless: the pipeline
// compile no-ops without an Engine, but index assignment does not depend on it).
Material* RegisterPackGateMaterial(RenderServices& rs, const GUID& guid, const char* name)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    return rs.Materials().RegisterMaterialFromDocument(guid, doc);
}

// Reads the single D32 texel of `layer` out of a persistent 1x1 depth fallback.
//
// The fallbacks are the engine's live resources, so the caller's texture is left
// in DepthSampled — the state its consumers bind it in — not in CopySource.
// Returns a sentinel that is not a legal depth value, so a copy that silently
// wrote nothing cannot be mistaken for a reverse-Z far texel of 0.0.
float ReadDepthFallbackTexel(RsFixture& f, TextureHandle tex, uint32_t layer)
{
    constexpr float kNotRead = -1.0f;

    const BufferHandle readback =
        f.Device->CreateReadbackBuffer(sizeof(float), "DepthFallbackTexelReadback");
    if (!readback.IsValid())
        return kNotRead;

    auto cl = f.Device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (!cl)
    {
        f.Device->DestroyBuffer(readback);
        return kNotRead;
    }
    cl->Begin();
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        tex, Rendering::ResourceState::DepthSampled, Rendering::ResourceState::CopySource,
        /*baseMip*/ 0, /*levelCount*/ 1, /*baseLayer*/ layer, /*layerCount*/ 1));
    cl->CopyTextureSubresourceToBuffer(tex, /*mip*/ 0, layer, readback,
                                       /*width*/ 1, /*height*/ 1, /*srcX*/ 0, /*srcY*/ 0,
                                       /*dstOffset*/ 0, /*dstRowPitch*/ sizeof(float));
    cl->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
        tex, Rendering::ResourceState::CopySource, Rendering::ResourceState::DepthSampled,
        /*baseMip*/ 0, /*levelCount*/ 1, /*baseLayer*/ layer, /*layerCount*/ 1));
    cl->End();
    Rendering::CommandList* raw = cl.get();
    f.Device->ExecuteCommandLists({raw});
    f.Device->FinalizeFrame();
    f.Device->WaitForIdle();

    float texel = kNotRead;
    if (const void* mapped = f.Device->MapBuffer(readback))
    {
        std::memcpy(&texel, mapped, sizeof(texel));
        f.Device->UnmapBuffer(readback);
    }
    f.Device->DestroyBuffer(readback);
    return texel;
}

} // namespace

// T1 + T2 — the contributor-dispatch twin on the RenderGraph arms, plus the
// fence-free prepass->world ordering contract: WAW + the Load-derived read
// schedule the prepass first and give the world's depth test visibility of
// the prepass's writes. Both passes run on a clear-only frame (no draws).
TEST(RenderServicesRGPassTests, ContributorDispatchAndPrepassOrderingWithoutFence)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        int forwardCalls = 0;
        int depthCalls = 0;
        auto fwdHandle = f.Rs.RegisterForwardEmit(
            [&](ForwardEmitContext& ctx)
            {
                if (ctx.ViewId == f.View)
                    ++forwardCalls;
            });
        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType)
            {
                if (ctx.ViewId == f.View)
                    ++depthCalls;
            });

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RGT1.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RGT1.Depth", DepthTargetDesc());

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        ASSERT_TRUE(f.Rs.GetEntityBatchKeys(f.View).empty());

        // Fixed order, steps 1-3 (all graceful no-ops on an empty scene).
        f.Rs.ScheduleGpuSkinningAndRetarget(frame);
        f.Rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        f.Rs.ScheduleWorldBucketerDispatches(frame);

        const RenderGraph::RGPass prepass = f.Rs.AddWorldDepthPrepassForView(frame, f.View, depth, 0.0f);
        ASSERT_TRUE(prepass.IsValid());

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        EXPECT_EQ(world.EffectiveColor.Id, color.Id) << "no resolve: effective = color";

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        EXPECT_GE(forwardCalls, 1) << "forward emit must run with empty entity keys";
        EXPECT_GE(depthCalls, 1) << "depth emit must run with empty entity keys";
        EXPECT_EQ(frame.Stats().RenderPassesBegun, 2u)
            << "clear-only frame: prepass + world both begin their render passes";
        // F5 negative: a keyword-less world (no Shadows) must NOT have
        // allocated a shadow array — the pool holds exactly the two targets.
        EXPECT_EQ(pools.Persistent.Size(), 2u)
            << "keyword-less views must not allocate shadow arrays";

        EXPECT_FALSE(frame.Graph().IsCulled(prepass.Id))
            << "world's AttachDepth(Load) derives the read that keeps the prepass";
        const size_t prepassIdx = ScheduledIndexOf(frame.Graph(), prepass.Id);
        const size_t worldIdx = ScheduledIndexOf(frame.Graph(), world.Pass.Id);
        ASSERT_NE(prepassIdx, SIZE_MAX);
        ASSERT_NE(worldIdx, SIZE_MAX);
        EXPECT_LT(prepassIdx, worldIdx) << "prepass before world — no order fence required";

        // The depth test READS prepass results: visibility, not just
        // availability, of the prepass's depth writes.
        bool sawDepthChain = false;
        for (const RenderGraph::RGBarrier& b : frame.Graph().Barriers())
            if (b.Resource == depth.Id && (b.SrcAccess & RenderGraph::RGAccessMask::DepthWrite) != 0 &&
                (b.DstAccess & RenderGraph::RGAccessMask::DepthRead) != 0)
                sawDepthChain = true;
        EXPECT_TRUE(sawDepthChain) << "world depth test must chain from the prepass write";

        fwdHandle.Reset();
        depHandle.Reset();
    }
    f.Down();
}

// T2b — the EmitForwardSampledRead declare-hook: a producer-emitted forward
// draw that samples a graph-written texture descriptor-direct (probe-face
// ocean cascades) registers the import at emit time, and the view's world
// pass declares it as SampledVertex — ordering the writing pass first and
// restoring ShaderReadOnly with a scope that covers the vertex stage.
TEST(RenderServicesRGPassTests, ForwardSampledReadDeclaresOnWorldPass)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("RGT2b.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("RGT2b.Depth", DepthTargetDesc());

        // Stand-in sim cascade: storage-written by an early pass, sampled
        // descriptor-direct by an emitted forward draw.
        TextureDesc cascadeDesc = ColorTargetDesc();
        cascadeDesc.usage =
            static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        RenderGraph::RGTexture cascade =
            frame.ImportPersistentTexture("RGT2b.Cascade", cascadeDesc);

        const RenderGraph::RGPass sim = frame.AddPass(
            "RGT2b.Sim", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            { p.Write(cascade, RenderGraph::RGTextureWrite::Storage); },
            [](RenderGraph::RGContext&) {});
        ASSERT_TRUE(sim.IsValid());

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        f.Rs.ScheduleGpuSkinningAndRetarget(frame);
        f.Rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        f.Rs.ScheduleWorldBucketerDispatches(frame);

        f.Rs.EmitForwardSampledRead(frame, f.View, cascade);

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        EXPECT_FALSE(frame.Graph().IsCulled(sim.Id))
            << "declared read must keep the writing sim alive";
        const size_t simIdx = ScheduledIndexOf(frame.Graph(), sim.Id);
        const size_t worldIdx = ScheduledIndexOf(frame.Graph(), world.Pass.Id);
        ASSERT_NE(simIdx, SIZE_MAX);
        ASSERT_NE(worldIdx, SIZE_MAX);
        EXPECT_LT(simIdx, worldIdx) << "sim write must schedule before the sampling draw";

        // The restore is the SampledVertex scope: storage write -> shader read
        // visible to the VERTEX stage (vertex modifiers sample these), image
        // back in ShaderReadOnly.
        bool sawSampledVertexRestore = false;
        for (const RenderGraph::RGBarrier& b : frame.Graph().Barriers())
        {
            if (b.Resource != cascade.Id)
                continue;
            if ((b.SrcAccess & RenderGraph::RGAccessMask::ShaderWrite) == 0 ||
                (b.DstAccess & RenderGraph::RGAccessMask::ShaderRead) == 0)
                continue;
            if (b.NewLayout != RenderGraph::RGImageLayout::ShaderReadOnly)
                continue;
            EXPECT_NE(b.DstStage & RenderGraph::RGStage::VertexShader, 0u)
                << "SampledVertex read must cover the vertex stage";
            EXPECT_NE(b.DstStage & RenderGraph::RGStage::FragmentShader, 0u)
                << "SampledVertex read must cover the fragment stage";
            sawSampledVertexRestore = true;
        }
        EXPECT_TRUE(sawSampledVertexRestore)
            << "world pass must declare the emitted sampled read (write->read barrier)";
    }
    f.Down();
}

// T3 — attachment semantics: the clear-vs-load gate and read-only depth.
TEST(RenderServicesRGPassTests, WorldAttachmentClearLoadAndReadOnlyDepth)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // Frame 0: clearColor requested, color not initialized -> Clear; no
        // prepass -> depth ReadWrite + cleared.
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RGT3.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RGT3.Depth", DepthTargetDesc());
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        const RenderGraph::RGAttachmentRec* colorRec = nullptr;
        const RenderGraph::RGAttachmentRec* depthRec = nullptr;
        for (const auto& rec : frame.Attachments())
        {
            if (rec.Tex == color.Id) colorRec = &rec;
            if (rec.Tex == depth.Id) depthRec = &rec;
        }
        ASSERT_NE(colorRec, nullptr);
        ASSERT_NE(depthRec, nullptr);
        EXPECT_EQ(colorRec->Ops.Load, RenderGraph::RGLoadOp::Clear)
            << "clear requested + color uninitialized => Clear";
        EXPECT_EQ(depthRec->Ops.Load, RenderGraph::RGLoadOp::Clear) << "no prepass => world clears depth";
        EXPECT_FALSE(depthRec->ReadOnly) << "no prepass => world writes depth";

        // Frame 1: a sky backdrop / earlier pass initialized the color ->
        // Load; a declared prepass flips depth to read-only.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("RGT3.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("RGT3.Depth", DepthTargetDesc());
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        f.Rs.Views().MarkViewColorInitialized(f.View);

        const RenderGraph::RGPass prepass = f.Rs.AddWorldDepthPrepassForView(frame, f.View, depth, 0.0f);
        ASSERT_TRUE(prepass.IsValid());
        targets.Color = color;
        targets.Depth = depth;
        world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        colorRec = nullptr;
        depthRec = nullptr;
        for (const auto& rec : frame.Attachments())
        {
            if (rec.Tex == color.Id && rec.Pass == world.Pass.Id) colorRec = &rec;
            if (rec.Tex == depth.Id && rec.Pass == world.Pass.Id) depthRec = &rec;
        }
        ASSERT_NE(colorRec, nullptr);
        ASSERT_NE(depthRec, nullptr);
        EXPECT_EQ(colorRec->Ops.Load, RenderGraph::RGLoadOp::Load)
            << "color initialized => Load (blend-over semantics)";
        EXPECT_EQ(depthRec->Ops.Load, RenderGraph::RGLoadOp::Load) << "prepass declared => no world clear";
        EXPECT_TRUE(depthRec->ReadOnly) << "prepass declared, no forward-depth carve-out";
    }
    f.Down();
}

// T4 — the MSAA resolve three-way and the EffectiveColor contract.
TEST(RenderServicesRGPassTests, EffectiveColorFollowsResolveCollapseAndMismatch)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // (a) MSAA color + same-format 1-sample resolve => resolve attach.
        frame.BeginFrame(0);
        RenderGraph::RGTexture msaa = frame.ImportPersistentTexture("RGT4.Msaa", ColorTargetDesc(4));
        RenderGraph::RGTexture resolve = frame.ImportPersistentTexture("RGT4.Resolve", ColorTargetDesc());
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = msaa;
        targets.Resolve = resolve;
        auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        EXPECT_EQ(world.EffectiveColor.Id, resolve.Id);
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();
        {
            const RenderGraph::RGAttachmentRec* rec = nullptr;
            for (const auto& r : frame.Attachments())
                if (r.Tex == msaa.Id)
                    rec = &r;
            ASSERT_NE(rec, nullptr);
            EXPECT_EQ(rec->Resolve, resolve.Id) << "MSAA resolve attachment recorded";
        }

        // (b) MSAA off + pipeline resolve => collapse: render straight into
        // the resolve target.
        frame.BeginFrame(1);
        RenderGraph::RGTexture color1 = frame.ImportPersistentTexture("RGT4.Color1", ColorTargetDesc());
        RenderGraph::RGTexture pipeRes = frame.ImportPersistentTexture("RGT4.PipeRes", ColorTargetDesc());
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        targets = {};
        targets.Color = color1;
        targets.Resolve = pipeRes;
        targets.ResolveFromPipeline = true;
        world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        EXPECT_EQ(world.EffectiveColor.Id, pipeRes.Id) << "collapse renders into the resolve";
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();
        {
            bool attachedToPipeRes = false;
            bool attachedToColor1 = false;
            for (const auto& r : frame.Attachments())
            {
                if (r.Tex == pipeRes.Id) attachedToPipeRes = true;
                if (r.Tex == color1.Id) attachedToColor1 = true;
            }
            EXPECT_TRUE(attachedToPipeRes);
            EXPECT_FALSE(attachedToColor1) << "collapsed: the 1-sample color is not attached";
        }

        // (c) MSAA on + format-mismatched resolve => no resolve, effective =
        // the MSAA color itself.
        frame.BeginFrame(2);
        RenderGraph::RGTexture msaa2 = frame.ImportPersistentTexture("RGT4.Msaa2", ColorTargetDesc(4));
        RenderGraph::RGTexture badRes = frame.ImportPersistentTexture(
            "RGT4.BadRes", ColorTargetDesc(1, TextureFormat::R16G16B16A16_FLOAT));
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        targets = {};
        targets.Color = msaa2;
        targets.Resolve = badRes;
        world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        EXPECT_EQ(world.EffectiveColor.Id, msaa2.Id) << "format mismatch: resolve skipped";
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// T5 — shadow cascade arms: declaration-time gating, ONE pool import shared
// by producer and consumer (the C2 pin through the real arms), per-layer
// attach ranges, and the feature's adoption of the pooled physical.
TEST(RenderServicesRGPassTests, ShadowCascadesShareOnePoolImportWithTheWorldPass)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 256;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        // Genuine no-caster gate: AFTER the emit step ran (with no producer
        // registered) — asserting before BuildWorldBatchKeys would pass for
        // the wrong reason (emits simply hadn't fired yet).
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        // No caster yet: ctx.ViewNeedsCascades is false, so the feature arm declines.
        {
            const FeatureDeclareContext noCaster = f.Rs.MakeFeatureDeclareContext(
                frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
            EXPECT_FALSE(feature.DeclareCascadePass(frame, f.Rs, noCaster, 0, "Cascade0").IsValid());
        }

        // Synthetic caster: a depth contributor that emits into the shadow
        // cascade stream (HasShadowCasters via HasDepthCommands).
        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });
        f.Rs.BuildWorldBatchKeys(); // re-run the emit step with the producer live

        // The slice-4 CSM node contract: frame data cached BEFORE declaring.
        CascadeFrameData fd{};
        fd.NumCascades = 2;
        feature.CacheFrameData(f.View, fd);

        // Declare through the feature per-family entry (A1.1 S1). ctx snapshots
        // the now-live caster => ViewNeedsCascades true.
        const FeatureDeclareContext ctx = f.Rs.MakeFeatureDeclareContext(
            frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
        const RenderGraph::RGPass c0 = feature.DeclareCascadePass(frame, f.Rs, ctx, 0, "Cascade0");
        const RenderGraph::RGPass c1 = feature.DeclareCascadePass(frame, f.Rs, ctx, 1, "Cascade1");
        ASSERT_TRUE(c0.IsValid());
        ASSERT_TRUE(c1.IsValid());
        // Out-of-range cascade rejected by the cached frame data.
        EXPECT_FALSE(feature.DeclareCascadePass(frame, f.Rs, ctx, 2, "Cascade2").IsValid());

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RGT5.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RGT5.Depth", DepthTargetDesc());
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        // The shadow-array import (and read) is gated on the Shadows keyword
        // — keyword-less views must not allocate an array they never sample.
        const auto world =
            f.Rs.AddWorldPassForView(frame, f.View, targets, MaterialKeyword::Shadows);
        ASSERT_TRUE(world.Pass.IsValid());

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        // Per-layer attach recs on ONE shared resource id.
        const RenderGraph::RGAttachmentRec* rec0 = nullptr;
        const RenderGraph::RGAttachmentRec* rec1 = nullptr;
        for (const auto& r : frame.Attachments())
        {
            if (r.Pass == c0.Id) rec0 = &r;
            if (r.Pass == c1.Id) rec1 = &r;
        }
        ASSERT_NE(rec0, nullptr);
        ASSERT_NE(rec1, nullptr);
        EXPECT_EQ(rec0->Tex, rec1->Tex) << "both cascades attach the SAME pool import";
        EXPECT_EQ(rec0->Range.BaseLayer, 0u);
        EXPECT_EQ(rec1->Range.BaseLayer, 1u);
        EXPECT_EQ(rec0->Range.LayerCount, 1u);
        EXPECT_EQ(rec1->Range.LayerCount, 1u);

        // The world pass keeps the cascades alive (no PreventCulling) and
        // chains from their depth writes through the SAME resource id.
        EXPECT_FALSE(frame.Graph().IsCulled(c0.Id));
        EXPECT_FALSE(frame.Graph().IsCulled(c1.Id));
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), c0.Id),
                  ScheduledIndexOf(frame.Graph(), world.Pass.Id));
        bool sawShadowRaw = false;
        for (const RenderGraph::RGBarrier& b : frame.Graph().Barriers())
            if (b.Resource == rec0->Tex && (b.SrcAccess & RenderGraph::RGAccessMask::DepthWrite) != 0 &&
                (b.DstAccess & RenderGraph::RGAccessMask::ShaderRead) != 0)
                sawShadowRaw = true;
        EXPECT_TRUE(sawShadowRaw) << "world's sampled read chains from the cascade depth writes";

        // Adoption: the feature serves the POOLED physical to its consumers.
        EXPECT_EQ(feature.GetShadowMapTexture(f.View, frame),
                  frame.PhysicalTexture(RenderGraph::RGTexture{rec0->Tex}));

        depHandle.Reset();
    }
    f.Down();
}

// F3 (slice-3-final review) — the F1 UAF's REAL trigger: a pooled-physical
// change on a LATER frame (the first adopt skips invalidation, so T5 could
// never reach the cache erase). The arm fetches the cache AFTER the
// adopt-triggered invalidation: the change frame declines (one-frame skip,
// the old recreate semantics) and the next frame recovers. Under the
// pre-fix order this frame would have READ FREED MEMORY and declared.
TEST(RenderServicesRGPassTests, CascadeArmSkipsOneFrameOnAPooledPhysicalChange)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 128;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        auto runFrame = [&](uint64_t n) -> RenderGraph::RGPass
        {
            frame.BeginFrame(n);
            f.Rs.BeginWorldDrawFrame();
            f.Rs.BuildWorldBatchKeys();
            CascadeFrameData fd{};
            fd.NumCascades = 2;
            feature.CacheFrameData(f.View, fd);
            const FeatureDeclareContext ctx = f.Rs.MakeFeatureDeclareContext(
                frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
            const RenderGraph::RGPass c0 = feature.DeclareCascadePass(frame, f.Rs, ctx, 0, "C0");
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("F3.Color", ColorTargetDesc());
            frame.AddPass("Anchor", 0,
                          [&](RenderGraph::RGPassBuilder& p)
                          { p.AttachColor(0, color, {.Load = RenderGraph::RGLoadOp::Clear}); },
                          [](RenderGraph::RGContext&) {});
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
            return c0;
        };

        // Frame 0: first adopt (no invalidation) — declares.
        EXPECT_TRUE(runFrame(0).IsValid());

        // Simulate a pooled-physical change (pool eviction/realloc of a
        // hidden view, a future config setter): plant a DIFFERENT physical
        // on the feature so the next arm's adopt sees a CHANGE and runs the
        // invalidation path that erases the cached frame data.
        TextureDesc otherDesc = DepthTargetDesc();
        otherDesc.arrayLayers = 2;
        const TextureHandle otherTex = f.Device->CreateTexture(otherDesc);
        ASSERT_TRUE(otherTex.IsValid());
        feature.AdoptPooledShadowMap(f.View, otherTex, f.Rs, frame);

        // Change frame: adopt invalidates the just-cached frame data; the
        // arm must observe the null cache and decline — not read freed
        // memory through a pre-fetched pointer.
        EXPECT_FALSE(runFrame(1).IsValid()) << "one-frame skip on the physical-change frame";

        // Recovery: the feature holds the pooled physical again.
        EXPECT_TRUE(runFrame(2).IsValid());

        depHandle.Reset();
        f.Device->DestroyTexture(otherTex);
    }
    f.Down();
}

// The cascade-lifetime guard. All three import sites are gated on
// ViewNeedsCascades, so a view whose casters go away stops re-adopting while the
// render-graph pool goes on to free the physical it last adopted. Nothing about
// the handle changes when that happens — TextureHandle::IsValid() is an id != 0
// test, not a liveness test — so an unguarded cache serves a dead handle to the
// world pass's binding table for as long as the view keeps rendering.
//
// The frame stamp is what makes the absence observable: the feature serves the
// physical only to the frame it was adopted for. This drives the real declare
// path (so the adopt is genuine, not planted) and then destroys the physical
// underneath the feature to make the staleness concrete.
TEST(RenderServicesRGPassTests, CascadeHandleGoesStaleWhenTheArmStopsImporting)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 128;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        // Frame 0: a caster exists, so the arm imports and adopts.
        frame.BeginFrame(0);
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        CascadeFrameData fd{};
        fd.NumCascades = 2;
        feature.CacheFrameData(f.View, fd);
        const FeatureDeclareContext ctx0 = f.Rs.MakeFeatureDeclareContext(
            frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
        ASSERT_TRUE(feature.DeclareCascadePass(frame, f.Rs, ctx0, 0, "Stale.C0").IsValid());

        const TextureHandle adopted = feature.GetShadowMapTexture(f.View, frame);
        ASSERT_TRUE(adopted.IsValid()) << "the arm's adopt must serve the pooled physical";

        // Casters gone: ViewNeedsCascades is false next frame, so nothing
        // imports and nothing re-adopts.
        depHandle.Reset();

        frame.BeginFrame(1);
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        const FeatureDeclareContext ctx1 = f.Rs.MakeFeatureDeclareContext(
            frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
        EXPECT_FALSE(feature.DeclareCascadePass(frame, f.Rs, ctx1, 0, "Stale.C1").IsValid())
            << "sanity: with no casters the arm must decline (so it never re-adopts)";

        // Make the staleness real: the pool frees an idle persistent physical on
        // exactly this schedule. The handle id is unchanged and still reads
        // IsValid(); only the image is gone.
        f.Device->DestroyTexture(adopted);
        EXPECT_TRUE(adopted.IsValid())
            << "IsValid() is an id test — a destroyed texture's handle still passes it, which is "
               "precisely why the frame stamp and not IsValid() has to be the liveness test";

        EXPECT_FALSE(feature.GetShadowMapTexture(f.View, frame).IsValid())
            << "a handle adopted for an EARLIER frame must be reported absent, so the world pass "
               "binds its typed fallback instead of a freed image";
    }
    f.Down();
}

// B3: the PCSS bindless cache-hit path is guarded too.
//
// EnsurePcssBindlessTextures memoises per view, and the memoised indices name
// single-layer VIEWS OF the adopted physical. Checking liveness only where a
// fresh registration reads the handle would leave the early-out serving those
// stale slots on exactly the frames the guard exists for — the blocker search
// would texelFetch through views of a freed image.
TEST(RenderServicesRGPassTests, PcssBindlessCacheHitIsFrameGuardedToo)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 128;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        if (!f.Rs.Textures().IsBindlessEnabled())
            GTEST_SKIP() << "Bindless disabled — PCSS raw-depth slots are never registered";

        TextureDesc arrayDesc = DepthTargetDesc();
        arrayDesc.arrayLayers = cfg.NumCascades;
        const TextureHandle tex = f.Device->CreateTexture(arrayDesc);
        ASSERT_TRUE(tex.IsValid());

        feature.AdoptPooledShadowMap(f.View, tex, f.Rs, frame);
        // Populate the memo on the adopting frame.
        ASSERT_NE(feature.EnsurePcssBindlessTextures(f.View, f.Rs, frame), nullptr);

        // Next frame with no re-adopt: the memo is still populated, so only a
        // guard placed ABOVE the cache hit can report absence.
        frame.BeginFrame(1);
        EXPECT_EQ(feature.EnsurePcssBindlessTextures(f.View, f.Rs, frame), nullptr)
            << "the memoised PCSS slots describe a physical that is no longer servable this "
               "frame; the cache hit must not bypass the liveness check";

        f.Device->DestroyTexture(tex);
    }
    f.Down();
}

// Part 2's resource, and the two properties it exists for. Receivers declare
// `sampler2DArrayShadow ge_shadowMapArray` and select a layer by cascade index,
// so the fallback must be (a) a DEPTH array carrying every cascade layer, and
// (b) actually CLEARED — an allocated-but-undefined array is exactly the read
// this fallback replaces, so shape alone buys nothing.
//
// Both are asserted directly. Layer count comes from the device rather than from
// registering a per-layer bindless view: an out-of-range view still yields a
// NON-ZERO bindless index (TextureService::RegisterTextureBindless returns the
// slot and silently substitutes the whole-texture view when CreateTextureView
// fails), so that probe passes on a 1-layer image and cannot fail.
TEST(RenderServicesRGPassTests, CascadeShadowFallbackIsADepthArrayWithEveryCascadeLayer)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        const TextureHandle fallback = f.Rs.GetCascadeShadowFallbackTexture();
        ASSERT_TRUE(fallback.IsValid())
            << "ge_shadowMapArray's fallback must be provisioned alongside its siblings";
        EXPECT_EQ(f.Device->GetTextureFormat(fallback), TextureFormat::D32_FLOAT)
            << "a comparison sampler requires a depth format";
        EXPECT_EQ(f.Device->GetTextureArrayLayers(fallback), kMaxShadowCascades)
            << "receivers index layers 0.." << (kMaxShadowCascades - 1)
            << "; a shorter array is an out-of-range layer read for every cascade above its last";

        // The cleared VALUE, read off EVERY layer. Reading only the last one
        // passes on an array whose middle layers were skipped, and every layer
        // is selected by some cascade index.
        for (uint32_t layer = 0; layer < kMaxShadowCascades; ++layer)
        {
            EXPECT_FLOAT_EQ(ReadDepthFallbackTexel(f, fallback, layer), 0.0f)
                << "layer " << layer
                << " is not at reverse-Z far, so the fallback reads as an occluder (or as "
                   "undefined memory) instead of fully lit";
        }
    }
    f.Down();
}

// The area/spot fallback carries FOUR consumers, and 0.0 is the no-op for all
// four — which is why the value, not just the allocation, is what is asserted:
//   ge_areaShadowMap / ge_spotShadowMap (sampler2DShadow, GreaterOrEqual): the
//     callers clamp Dref into [0,1], so Dref >= 0.0 always passes => fully lit.
//   ge_areaShadowMapRaw (sampler2D, PCSS blocker search): the search counts a
//     blocker where `stored > refDepth`; 0.0 > refDepth is false across [0,1],
//     so it finds none and returns lit.
//   ge_sceneDepth (sampler2D, LinearClamp — NOT a comparison sampler): 0.0 is
//     reverse-Z FAR, which every consumer reads as "no opaque surface in front".
// An unwritten image satisfies none of these except by accident, and freshly
// allocated VRAM often reads as zero — so this assertion is only meaningful
// alongside the mutation check that a non-far clear value fails it.
TEST(RenderServicesRGPassTests, AreaShadowFallbackTexelIsReverseZFar)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        const TextureHandle fallback = f.Rs.GetAreaShadowFallbackTexture();
        ASSERT_TRUE(fallback.IsValid())
            << "ge_areaShadowMap/ge_spotShadowMap/ge_sceneDepth have no texture to bind";
        EXPECT_EQ(f.Device->GetTextureFormat(fallback), TextureFormat::D32_FLOAT)
            << "a comparison sampler requires a depth format";

        EXPECT_FLOAT_EQ(ReadDepthFallbackTexel(f, fallback, 0), 0.0f)
            << "the area/spot fallback is not at reverse-Z far, so it reads as an occluder (or as "
               "undefined memory) on the comparison paths and as a near opaque surface on "
               "ge_sceneDepth";
    }
    f.Down();
}

// ge_pointShadowMap is a sampler2DArrayShadow indexed baseLayer + face, so all
// six face layers are reachable and every one must carry the far value. Layer
// count is asserted from the device: a short array is an out-of-range layer read
// for every face above its last.
TEST(RenderServicesRGPassTests, PointShadowFallbackIsASixLayerArrayAtReverseZFarOnEveryLayer)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        const TextureHandle fallback = f.Rs.GetPointShadowFallbackTexture();
        ASSERT_TRUE(fallback.IsValid()) << "ge_pointShadowMap has no texture to bind";
        EXPECT_EQ(f.Device->GetTextureFormat(fallback), TextureFormat::D32_FLOAT)
            << "a comparison sampler requires a depth format";
        ASSERT_EQ(f.Device->GetTextureArrayLayers(fallback), kPointShadowFaceCount)
            << "receivers index layers 0.." << (kPointShadowFaceCount - 1)
            << "; a shorter array is an out-of-range layer read for the faces above its last";

        for (uint32_t layer = 0; layer < kPointShadowFaceCount; ++layer)
        {
            EXPECT_FLOAT_EQ(ReadDepthFallbackTexel(f, fallback, layer), 0.0f)
                << "face layer " << layer
                << " is not at reverse-Z far, so a point light with no atlas shadows that face "
                   "instead of leaving it lit";
        }
    }
    f.Down();
}

// "A cascade array is missing" is only a producer bug if the producer had
// COMMITTED to importing one. ShadowMapNode::DeclareForView has six early-outs
// above the import (no feature, upload-alloc failure, no view desc / no camera
// id, no camera data, and a zero projection — a thumbnail camera before its
// first update); the feature adds ViewNeedsCascades and the directional-light
// resolve on top. On every one of those the view still has shadow casters and a
// shadow-casting directional, and still renders a world pass.
//
// This reproduces that shape — casters live, a casting directional submitted,
// and no cascade declare — and requires SILENCE. A consumer-side predicate that
// re-derives the decision cannot distinguish it from a real producer bug, and
// the diagnostic is an Error plus assert(false) under GE_DEV_DIAG (live in
// DebugFast), so a false positive aborts here rather than merely logging.
TEST(RenderServicesRGPassTests, NoCascadeDiagnosticWhenTheArmNeverCommittedToImporting)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        // Casters live => ViewNeedsShadowCascadePasses is true.
        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });
        f.Rs.BeginWorldDrawFrame();

        // A shadow-casting primary directional. Submitted AFTER
        // BeginWorldDrawFrame, which clears the per-frame light lists — submitting
        // before it silently drops the light and makes this test vacuous.
        const auto* viewDesc = f.Rs.Views().FindViewDesc(f.View);
        ASSERT_NE(viewDesc, nullptr);
        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        f.Rs.SubmitLight(viewDesc->worldId, sun);
        f.Rs.BuildWorldBatchKeys();

        // Verify BOTH halves of the replaced predicate are satisfied, or a green
        // result here would prove nothing about the diagnostic.
        ASSERT_TRUE(f.Rs.ViewNeedsShadowCascadePasses(f.View))
            << "setup: the caster half must be TRUE";
        const ExtractedLight* primary =
            SelectPrimaryDirectional(f.Rs.GetWorldLights(viewDesc->worldId));
        ASSERT_NE(primary, nullptr) << "setup: the directional half must resolve a light";
        ASSERT_NE(primary->castsShadows, 0u) << "setup: that light must cast shadows";

        // Deliberately NO cascade declare: this is the producer bailing at one of
        // its gates. The world pass must bind the fallback without complaining.
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("Owed.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Owed.Depth", DepthTargetDesc());
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world =
            f.Rs.AddWorldPassForView(frame, f.View, targets, MaterialKeyword::Shadows);
        ASSERT_TRUE(world.Pass.IsValid());

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        depHandle.Reset();
    }
    f.Down();
}

// The Shadows keyword is blueprint-driven (WorldRenderNode parses it from the
// pipeline JSON) and never consults ShadowMapRenderFeature, so a Shadows pass can
// be declared in a pipeline that has no ShadowMap node — and then the feature,
// which owns the comparison sampler the world pass used to depend on, does not
// exist. Binding 9 is statically declared by every such receiver, so leaving it
// unwritten is the VUID-08114 family. Both halves of the pair must therefore come
// from somewhere that is always present: RenderServices itself.
TEST(RenderServicesRGPassTests, CascadeShadowTexturePairIsAvailableWithoutTheShadowFeature)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        ASSERT_EQ(f.Rs.GetFeature<ShadowMapRenderFeature>(), nullptr)
            << "setup: this fixture must NOT have the shadow feature, that is the case under test";
        EXPECT_TRUE(f.Rs.GetCascadeShadowFallbackTexture().IsValid())
            << "ge_shadowMapArray has no texture to bind on a Shadows pass with no ShadowMap node";
        EXPECT_TRUE(f.Rs.GetCascadeShadowSampler().IsValid())
            << "ge_shadowMapArray has no comparison sampler to bind: the feature-owned sampler is "
               "the only other source and the feature is absent";
    }
    f.Down();
}

// Adopting a different pooled physical must RELEASE the old physical's PCSS
// bindless registrations, not merely drop the feature's index map.
//
// The registrations own the single-layer VkImageViews the blocker search reads
// through, and TextureService's bindless cache is keyed on the texture's handle
// id. Dropping the index map alone leaves those slots allocated forever (one per
// cascade per realloc) with their descriptors pointing at views of a texture the
// pool is about to free, and leaves the id-keyed entries in place — so the next
// registration for the same id is served from the stale cache instead of being
// re-issued.
//
// Adopting A -> B -> A makes that observable without depending on handle-id
// reuse: A's slots are freed when the feature adopts away from it, so the second
// registration on A must hand back FRESH indices. Under the unfixed code A's
// cache entries survive and the identical indices come back.
TEST(RenderServicesRGPassTests, AdoptingANewPhysicalReleasesTheOldPcssBindlessSlots)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        // The adopt/serve pair is frame-scoped; one live frame is all this probe
        // needs — it is about slot RELEASE, not about staleness.
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 128;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        if (!f.Rs.Textures().IsBindlessEnabled())
            GTEST_SKIP() << "Bindless disabled — PCSS raw-depth slots are never registered";

        TextureDesc arrayDesc = DepthTargetDesc();
        arrayDesc.arrayLayers = cfg.NumCascades;
        const TextureHandle texA = f.Device->CreateTexture(arrayDesc);
        const TextureHandle texB = f.Device->CreateTexture(arrayDesc);
        ASSERT_TRUE(texA.IsValid());
        ASSERT_TRUE(texB.IsValid());

        auto registerOn = [&](TextureHandle tex, uint32_t (&out)[2])
        {
            feature.AdoptPooledShadowMap(f.View, tex, f.Rs, frame);
            const auto* idx = feature.EnsurePcssBindlessTextures(f.View, f.Rs, frame);
            ASSERT_NE(idx, nullptr);
            for (uint32_t c = 0; c < cfg.NumCascades; ++c)
            {
                // 0 is the bindless "not set" sentinel — a real slot is never 0.
                ASSERT_NE(idx->Indices[c], 0u);
                out[c] = idx->Indices[c];
            }
        };

        uint32_t firstOnA[2]{};
        uint32_t onB[2]{};
        uint32_t secondOnA[2]{};
        registerOn(texA, firstOnA);
        registerOn(texB, onB);
        registerOn(texA, secondOnA);

        for (uint32_t c = 0; c < cfg.NumCascades; ++c)
        {
            EXPECT_NE(secondOnA[c], firstOnA[c])
                << "cascade " << c << ": re-registering texA returned its ORIGINAL slot, so "
                   "adopting texB never released texA's bindless registrations";
            // A distinct live texture must never share a slot with another.
            EXPECT_NE(onB[c], firstOnA[c]) << "cascade " << c;
        }

        f.Device->DestroyTexture(texA);
        f.Device->DestroyTexture(texB);
    }
    f.Down();
}

// E1 (A1.1 pre-move lock) — the cascade producer's read-set equivalence: with
// the SkinPaletteAtlas published into the frame, the declared cascade pass must
// Read the IDENTICAL RGBuffer id the spine published (m_FrameRG.SkinPaletteAtlas),
// re-derived through the FeatureDeclareContext snapshot. This is an edge/read-set
// assertion, not a pass-name one: it locks that moving the body from RenderServices
// into ShadowMapRenderFeature preserves the skinning->cascade RAW edge on the
// same id. Written against the CURRENT RS declaration path; re-pointed to the
// feature path when the body moves (S1). (DrawStreamOrdering's positive edge needs
// a live bucketer — unavailable in this minimal harness; its coverage lives in the
// full-pipeline RenderPipelineDeclareTests golden assertion.)
TEST(RenderServicesRGPassTests, CascadeProducerReadSetSurvivesTheFeatureMove)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 256;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        // Live caster (HasShadowCasters via HasDepthCommands) so ViewNeedsCascades.
        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        f.Rs.BeginWorldDrawFrame();
        // Prime the per-frame pools exactly as the render loop does after
        // BeginWorldDrawFrame — this is what allocates the BonePalette buffer the
        // SkinPaletteAtlas publishes (BeginWorldDrawFrame alone does not).
        const uint32_t frameIdx = f.Device->GetFrameIndex();
        f.Rs.GetPerFrameWritePool().BeginFrame(frameIdx);
        f.Rs.GetSkinPaletteAtlas().BeginFrame(f.Rs.GetPerFrameWritePool());
        f.Rs.BuildWorldBatchKeys();

        CascadeFrameData fd{};
        fd.NumCascades = 2;
        feature.CacheFrameData(f.View, fd);

        // Publish the SkinPaletteAtlas spine value (the atlas pool is live above).
        // This is the id the cascade producer must Read.
        f.Rs.ScheduleGpuSkinningAndRetarget(frame);
        const RenderGraph::RGBuffer atlas = f.Rs.FrameRG().SkinPaletteAtlas;
        ASSERT_TRUE(atlas.IsValid())
            << "SkinPaletteAtlas must be published for the read-set lock to mean anything";

        // The ctx snapshot must carry the IDENTICAL id (snapshot fidelity — the E1
        // failure mode is a stale/foreign/re-derived value). MakeFeatureDeclareContext
        // is the single snapshot site the feature path reads through.
        FeatureDeclareContext ctx = f.Rs.MakeFeatureDeclareContext(
            frame, f.View, cfg.NumCascades, cfg.Resolution, nullptr, {});
        EXPECT_EQ(ctx.SkinPaletteAtlas.Id, atlas.Id)
            << "ctx snapshot must carry the published atlas id, frame-validated";
        EXPECT_TRUE(ctx.ViewNeedsCascades);

        // Declare through the feature path (A1.1 S1) — the body moved out of
        // RenderServices, but the read-set must be unchanged.
        const RenderGraph::RGPass cascade =
            feature.DeclareCascadePass(frame, f.Rs, ctx, 0, "Cascade0");
        ASSERT_TRUE(cascade.IsValid());

        // THE lock: the pass Reads the published atlas id (not a re-derived one,
        // not a dropped edge).
        EXPECT_TRUE(frame.Graph().HasReadAccess(cascade.Id, atlas.Id))
            << "cascade must Read the published SkinPaletteAtlas id (skinning->cascade RAW edge)";

        depHandle.Reset();
    }
    f.Down();
}

// F9 (deferred from the slice-3 review) — the area-shadow arm: declared only
// with a valid area light, scheduled before the world pass, and the world's
// sampled read chains from its depth write (the AreaShadowData upload-alloc
// identity is structural: one ViewFrameRG field written by the arm, read by
// the binding table). Re-pointed to the feature per-family entry after the
// punctual body moved to ShadowMapRenderFeature (A1.1 S2): the "valid area
// light" gate is now the ctx.AreaShadow snapshot MakeFeatureDeclareContext
// resolves, so the decline case builds a ctx before submitting the light.
TEST(RenderServicesRGPassTests, AreaShadowArmDeclaresOnlyWithAValidAreaLight)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 256;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        // A caster (HasShadowCasters) — same synthetic emit as T5.
        auto depHandle = f.Rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();

        // No valid area light yet: the ctx snapshot carries no AreaShadow, so the
        // feature arm declines at declaration.
        {
            const FeatureDeclareContext noLight =
                f.Rs.MakeFeatureDeclareContext(frame, f.View, cfg.NumCascades, 128, nullptr, {});
            EXPECT_FALSE(feature.DeclareAreaPass(frame, f.Rs, noLight).IsValid());
        }

        ExtractedLight area{};
        area.type = GameEngine::Components::LightType::Area;
        area.castsLight = 1;
        area.castsShadows = 1;
        area.directionWS[1] = -1.0f;
        area.upWS[2] = 1.0f;
        area.rightWS[0] = 1.0f;
        area.areaWidth = 1.0f;
        area.areaHeight = 1.0f;
        area.range = 10.0f;
        area.intensity = 1.0f;
        f.Rs.SubmitLight(/*worldId=*/0u, area);

        // Now the ctx resolves a valid AreaShadow => the arm declares.
        const FeatureDeclareContext ctx =
            f.Rs.MakeFeatureDeclareContext(frame, f.View, cfg.NumCascades, 128, nullptr, {});
        const RenderGraph::RGPass areaPass = feature.DeclareAreaPass(frame, f.Rs, ctx);
        ASSERT_TRUE(areaPass.IsValid());

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("F9.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("F9.Depth", DepthTargetDesc());
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world =
            f.Rs.AddWorldPassForView(frame, f.View, targets, MaterialKeyword::Shadows);
        ASSERT_TRUE(world.Pass.IsValid());

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        // The area pass attaches the pool-imported map; the world's sampled
        // read chains from its depth write on the SAME resource id.
        const RenderGraph::RGAttachmentRec* areaRec = nullptr;
        for (const auto& rec : frame.Attachments())
            if (rec.Pass == areaPass.Id && rec.IsDepth)
                areaRec = &rec;
        ASSERT_NE(areaRec, nullptr);
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), areaPass.Id),
                  ScheduledIndexOf(frame.Graph(), world.Pass.Id));
        bool sawRaw = false;
        for (const RenderGraph::RGBarrier& b : frame.Graph().Barriers())
            if (b.Resource == areaRec->Tex && (b.SrcAccess & RenderGraph::RGAccessMask::DepthWrite) != 0 &&
                (b.DstAccess & RenderGraph::RGAccessMask::ShaderRead) != 0)
                sawRaw = true;
        EXPECT_TRUE(sawRaw) << "world's area-map sample must chain from the area depth write";

        depHandle.Reset();
    }
    f.Down();
}

// T6 — the wrong-order contract: buckets registered but the bucketer never
// declared into this frame => the world pass is still declared, but NO
// DrawStreamOrdering read exists (a stale/foreign value must never be read;
// the arm logs the misuse loudly).
TEST(RenderServicesRGPassTests, WorldWithoutBucketerDeclaresNoOrderingRead)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RGT6.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RGT6.Depth", DepthTargetDesc());

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();

        // Slices registered directly (the scatter's CPU staging) — but
        // ScheduleWorldBucketerDispatches(frame) is deliberately NOT called.
        auto* builder = f.Rs.GetDrawStreamBuilder();
        ASSERT_NE(builder, nullptr);
        builder->BeginArenaFrame();
        Rendering::GPUDrawStreamBuilder::SliceRegistration slice{};
        slice.viewId = 1u;
        builder->RegisterSlice(slice);
        ASSERT_NE(builder->GetPendingSliceCount(), 0u);

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid()) << "misuse is diagnosed, not fatal";
        EXPECT_FALSE(f.Rs.FrameRG().DrawStreamOrdering.IsValid())
            << "no bucketer declaration into this frame => no ordering value published";

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// T7 — the consume-only shadow gate: a Shadows-keyword world pass with NO
// cascade producer this frame must not CREATE the pooled array (a
// Resolution²×cascades D32 allocation for an array nothing ever writes —
// the shadowless-pipeline-with-shadow-capable-materials shape). The world
// binds the dummy exactly like a keyword-less view. The CSM feature is
// initialized so the old behavior (world-creates) WOULD have allocated.
TEST(RenderServicesRGPassTests, ShadowsKeywordWithoutAProducerAllocatesNoArray)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& feature = f.Rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        cfg.NumCascades = 2;
        cfg.Resolution = 256;
        ASSERT_TRUE(feature.Initialize(f.Device.get(), cfg));

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RGT7.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RGT7.Depth", DepthTargetDesc());

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world =
            f.Rs.AddWorldPassForView(frame, f.View, targets, MaterialKeyword::Shadows);
        ASSERT_TRUE(world.Pass.IsValid());

        EXPECT_EQ(pools.Persistent.Size(), 2u)
            << "no producer arm ran this frame — the world must not create the shadow array";

        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// View-participation contract that the editor scene view leans on. Every
// per-view consumer — ScheduleViewCullingDispatches, the shadow dispatches,
// extraction, the world pass — skips a view whose ActiveRenderLayerMask() is 0.
// An OnDemand view carries that mask only while its owner re-arms it each frame
// via RequestViewFrame; a scene-view controller whose pane stops declaring
// (quad view toggled off, a collapsed/hidden pane, an inactive split) simply
// stops re-arming, so the view lapses out of GPU culling within two frames
// without a ReleaseView — its viewId, and thus its HZB visibility history,
// survive so re-declaring revives it. Regression guard against the orphaned
// scene-view culling dispatches that used to run forever.
TEST(RenderServicesRGPassTests, OnDemandViewLapsesOutOfCullingWhenNotReArmed)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        const CameraId camId = f.Rs.Views().AllocateCamera("OnDemandTestCam");
        const ViewId onDemand = f.Rs.Views().AllocateView("OnDemandTestView", camId,
                                                  ViewPurpose::EditorScene,
                                                  ViewParticipation::OnDemand);
        f.Rs.Views().SetViewRenderLayerMask(onDemand, 1u);

        const auto mask = [&](ViewId id) -> uint32_t
        {
            const ViewDesc* v = f.Rs.Views().FindViewDesc(id);
            return v ? v->ActiveRenderLayerMask() : 0u;
        };

        // f.View is an Always view (fixture default); the fresh OnDemand view is
        // dormant until armed.
        EXPECT_EQ(mask(onDemand), 0u) << "unarmed OnDemand view must not be culled";
        EXPECT_NE(mask(f.View), 0u) << "Always view participates unconditionally";

        // Arm for the frame: participates now and through the next BeginWorldDrawFrame
        // (RequestViewFrame primes participationFrames = 2).
        f.Rs.Views().RequestViewFrame(onDemand);
        EXPECT_NE(mask(onDemand), 0u) << "armed OnDemand view participates";

        f.Rs.BeginWorldDrawFrame(); // 2 -> 1
        EXPECT_NE(mask(onDemand), 0u) << "armed view still feeds the frame it was requested for";

        f.Rs.BeginWorldDrawFrame(); // 1 -> 0
        EXPECT_EQ(mask(onDemand), 0u)
            << "an OnDemand view that is not re-armed lapses out of per-view GPU work";
        EXPECT_NE(mask(f.View), 0u) << "the Always view keeps participating regardless of frames";

        // Re-arming revives the same view — no realloc, HZB history (keyed by viewId) intact.
        f.Rs.Views().RequestViewFrame(onDemand);
        EXPECT_NE(mask(onDemand), 0u) << "re-arming a lapsed OnDemand view revives it";
    }
    f.Down();
}

// The world scatter registers slices only for views that participate this frame.
// A view with no active layer (the "Game View" CameraSystem allocates before a
// render setup binds its targets, an idle thumbnail lane) declares no pass that
// reads a slice, so it must not cost a scatter dispatch either: without the gate
// its slice skipped visibility and wrote a record for every instance of its world.
TEST(RenderServicesRGPassTests, WorldScatterRegistersSlicesOnlyForParticipatingViews)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto* builder = f.Rs.GetDrawStreamBuilder();
        ASSERT_NE(builder, nullptr);
        if (!builder->GetOrCreateScatterPipeline().IsValid())
            GTEST_SKIP() << "draw_command_scatter.shaderpkg unavailable";

        Rendering::GPUInstance instance{};
        f.Rs.GetGPUScene()->AddInstance(instance);

        const ViewId dormant = f.Rs.Views().AllocateView(
            "DormantTestView", f.Rs.Views().FindViewDesc(f.View)->cameraId);
        f.Rs.Views().SetViewRenderLayerMask(dormant, 0u);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        f.Rs.ScheduleGpuSkinningAndRetarget(frame);
        f.Rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        f.Rs.ScheduleWorldBucketerDispatches(frame);

        using Phase = GPUDrawStreamBuilder::SlicePhase;
        EXPECT_TRUE(builder->HasPublishedRangesForPhase(
            static_cast<uint32_t>(f.View), GPUDrawStreamBuilder::kCascadeIndexNone, Phase::A))
            << "the participating view gets its slice";
        EXPECT_FALSE(builder->HasPublishedRangesForPhase(
            static_cast<uint32_t>(dormant), GPUDrawStreamBuilder::kCascadeIndexNone, Phase::A))
            << "a view with no active layer gets no slice";

        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// L6 — the forward-sampled buffer read stamp. EmitForwardSampledBufferRead
// stamps each entry with the frame it was imported into (pointer, frameIndex);
// the world pass declares an entry only while that stamp matches, so a
// re-begun incarnation (same pointer, new index) or a foreign frame pointer
// must not validate it.
namespace
{
bool HasForwardSampledBufferReadFor(const RenderServices& rs, ViewId viewId,
                                    RenderGraph::RGFrame& frame, RenderGraph::RGBuffer buffer)
{
    const auto* pv = rs.Views().FindPerView(viewId);
    if (!pv)
        return false;
    for (const auto& read : pv->ForwardSampledBufferRG)
    {
        if (read.Buffer.Id == buffer.Id && read.For.IsFor(frame))
            return true;
    }
    return false;
}
} // namespace

TEST(RenderServicesRGPassTests, ForwardSampledBufferReadGoesStaleOnReBeginAndForeignFrame)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        BufferDesc bd{};
        bd.size = 64;
        bd.usage = static_cast<uint32_t>(BufferUsage::Indirect);
        const RenderGraph::RGBuffer args = frame.CreateBuffer("ForwardArgs.Probe", bd);
        ASSERT_TRUE(args.IsValid());

        f.Rs.EmitForwardSampledBufferRead(frame, f.View, args, RenderGraph::RGBufferRead::Indirect,
                                           ForwardBufferReaders::WorldPass);
        EXPECT_TRUE(HasForwardSampledBufferReadFor(f.Rs, f.View, frame, args))
            << "the importing frame sees the emitted read";

        // Re-begin the SAME frame pointer at a new index: the stamped entry is a
        // stale incarnation, so the world pass must not declare it.
        frame.BeginFrame(1);
        EXPECT_FALSE(HasForwardSampledBufferReadFor(f.Rs, f.View, frame, args))
            << "a re-begun incarnation must not validate the prior frame's ids";

        // A foreign frame pointer never matches either.
        RenderGraph::RGFrame foreign(f.Device.get(), &pools.Persistent, &pools.Transient,
                                     &pools.Ring);
        foreign.BeginFrame(0);
        EXPECT_FALSE(HasForwardSampledBufferReadFor(f.Rs, f.View, foreign, args))
            << "a foreign frame must not validate the emitted read";
    }
    f.Down();
}

// A forward producer's prepass head rides with its colour draw (#2433). EmitForwardCommand puts the head into
// the view's camera-prepass stream with the draw, and emitting the view's forward draws again (a frame or a
// capture that runs the producers a second time) replaces the heads with the draws, so the prepass never
// draws a head twice or the head of a draw that is no longer emitted. Re-running the light-space depth
// producers (ShadowMapNode, once the cascades are cached) leaves the heads alone.
TEST(RenderServicesRGPassTests, AForwardDrawsPrepassHeadIsReplacedWithItWhenTheViewIsEmittedAgain)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        constexpr uint32_t kHeadMarker = 7u;
        uint32_t emits = 0;
        const auto producer = f.Rs.RegisterForwardEmit(
            [&](ForwardEmitContext& ctx)
            {
                if (ctx.ViewId != f.View)
                    return;
                ++emits;
                DrawCommand colour{};
                colour.IndexCount = 3;
                DrawCommand head = colour;
                head.FirstIndex = kHeadMarker;
                f.Rs.EmitForwardCommand(ctx.ViewId, colour, ForwardDrawDepth::Prepass, &head);
            });

        f.Rs.EmitProducerForwardCommandsForView(f.View);
        ASSERT_EQ(emits, 1u);
        ASSERT_EQ(f.Rs.GetForwardCommands(f.View).size(), 1u);
        const auto heads = f.Rs.GetDepthCommands(f.View, DepthPassType::Prepass);
        ASSERT_EQ(heads.size(), 1u) << "the draw's head goes into the prepass stream with the draw";
        EXPECT_EQ(heads[0].FirstIndex, kHeadMarker);

        f.Rs.EmitProducerForwardCommandsForView(f.View);
        ASSERT_EQ(emits, 2u);
        EXPECT_EQ(f.Rs.GetForwardCommands(f.View).size(), 1u);
        EXPECT_EQ(f.Rs.GetDepthCommands(f.View, DepthPassType::Prepass).size(), 1u)
            << "emitting the view again replaces its prepass heads with its draws";

        f.Rs.EmitProducerDepthCommandsForView(f.View);
        EXPECT_EQ(f.Rs.GetDepthCommands(f.View, DepthPassType::Prepass).size(), 1u)
            << "re-running the light-space depth producers leaves the camera prepass's heads alone";
    }
    f.Down();
}

// The camera prepass declares a read of every buffer the heads of its view's forward draws read
// (ForwardBufferReaders::WorldPassAndPrepass): the compute that writes the buffer this frame (CBT.Update, the
// grass placement) is then scheduled ahead of the prepass and the prepass draws this frame's geometry, not
// last frame's. A buffer only the world pass's draws read (ForwardBufferReaders::WorldPass) is not read by
// the prepass.
TEST(RenderServicesRGPassTests, ThePrepassReadsTheBuffersItsHeadsReadAfterTheirWriter)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        BufferDesc bd{};
        bd.size = 64;
        bd.usage = static_cast<uint32_t>(BufferUsage::Indirect) | static_cast<uint32_t>(BufferUsage::Storage);
        const RenderGraph::RGBuffer headArgs = frame.CreateBuffer("HeadArgs.Probe", bd);
        const RenderGraph::RGBuffer worldArgs = frame.CreateBuffer("WorldArgs.Probe", bd);
        const RenderGraph::RGPass writer = frame.AddComputePass(
            "HeadArgs.Write", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Write(headArgs, RenderGraph::RGBufferWrite::Storage);
                p.Write(worldArgs, RenderGraph::RGBufferWrite::Storage);
            },
            [](RenderGraph::RGContext&) {});
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("HeadReads.Color", ColorTargetDesc());
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("HeadReads.Depth", DepthTargetDesc());

        f.Rs.BeginWorldDrawFrame();
        f.Rs.BuildWorldBatchKeys();
        f.Rs.EmitForwardSampledBufferRead(frame, f.View, headArgs, RenderGraph::RGBufferRead::Indirect,
                                          ForwardBufferReaders::WorldPassAndPrepass);
        f.Rs.EmitForwardSampledBufferRead(frame, f.View, worldArgs, RenderGraph::RGBufferRead::Indirect,
                                          ForwardBufferReaders::WorldPass);
        f.Rs.ScheduleGpuSkinningAndRetarget(frame);
        f.Rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
        f.Rs.ScheduleWorldBucketerDispatches(frame);
        const RenderGraph::RGPass prepass = f.Rs.AddWorldDepthPrepassForView(frame, f.View, depth, 0.0f);
        ASSERT_TRUE(prepass.IsValid());
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto world = f.Rs.AddWorldPassForView(frame, f.View, targets);
        ASSERT_TRUE(world.Pass.IsValid());
        frame.MarkOutput(world.EffectiveColor);
        frame.Execute();
        f.Device->WaitForIdle();

        bool prepassReadsHeadArgs = false;
        bool prepassReadsWorldArgs = false;
        bool worldReadsWorldArgs = false;
        for (const RenderGraph::RGAccessRecord& access : frame.Graph().Accesses())
        {
            prepassReadsHeadArgs |= access.Pass == prepass.Id && access.Resource == headArgs.Id;
            prepassReadsWorldArgs |= access.Pass == prepass.Id && access.Resource == worldArgs.Id;
            worldReadsWorldArgs |= access.Pass == world.Pass.Id && access.Resource == worldArgs.Id;
        }
        EXPECT_TRUE(prepassReadsHeadArgs) << "the prepass declares the read its heads need";
        EXPECT_FALSE(prepassReadsWorldArgs) << "a buffer only the world pass's draws read stays out of the prepass";
        EXPECT_TRUE(worldReadsWorldArgs);
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), writer.Id), ScheduledIndexOf(frame.Graph(), prepass.Id))
            << "the buffer's writer runs before the prepass that draws from it";
    }
    f.Down();
}

// L1 (A1.3 S0 pre-move lock) — the R0.1 MaterialParams SSBO pack-skip gate.
// FinalizeFrameBuffers -> PackMaterialSSBO repacks a per-ring-slot
// only when the content stamp (SSBO generation + material count + the global
// material content epoch + the three texture default bindless indices) OR the
// ring placement changed since that slot last packed. On an idle scene the pack
// count therefore climbs once per RING SLOT and then FREEZES; a property edit
// (MarkDirty -> epoch bump) or an unregister (generation bump) busts the stamp
// and advances it exactly one more ring cycle. Nothing else in the suite pins
// this gate today — only the editor IPC observes it at runtime. Authored
// against the CURRENT RS surface so it survives the S1 facade move untouched.
TEST(RenderServicesRGPassTests, MaterialParamsPackSkipGate)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Rs.Textures().IsBindlessEnabled())
    {
        f.Down();
        GTEST_SKIP() << "Bindless unavailable — PackMaterialSSBO is gated on it";
    }
    {
        // Two registered materials -> materialCount 2 folded into every pack.
        const GUID guidA = GUID::Generate();
        const GUID guidB = GUID::Generate();
        Material* matA = RegisterPackGateMaterial(f.Rs, guidA, "PackGateA");
        Material* matB = RegisterPackGateMaterial(f.Rs, guidB, "PackGateB");
        ASSERT_NE(matA, nullptr);
        ASSERT_NE(matB, nullptr);

        // One simulated frame: the render-loop priming idiom (BeginWorldDrawFrame
        // -> pool.BeginFrame -> BuildWorldBatchKeys) followed by the pack. Driving
        // pool.BeginFrame with an incrementing counter steps the ring one slot per
        // call, exactly as a changing device frame index does at runtime.
        uint32_t frameCounter = 0;
        auto RunFrame = [&]()
        {
            f.Rs.BeginWorldDrawFrame();
            f.Rs.GetPerFrameWritePool().BeginFrame(frameCounter++);
            f.Rs.BuildWorldBatchKeys();
            f.Rs.Materials().FinalizeFrameBuffers();
        };

        // The pack stamp is per RING SLOT, and PerFrameWritePool sizes MaterialParams
        // deeper than the device paces (it is filled in the update phase), so a cycle
        // that must reach every slot counts slots, not frames in flight. Counted by
        // stepping the pool alone once round the ring, which packs nothing.
        auto& pool = f.Rs.GetPerFrameWritePool();
        pool.BeginFrame(frameCounter++);
        const uint32_t slots = Testing::RotateBackToCurrentSlot(
            pool, FrameWriteUsage::MaterialParams, [&] { pool.BeginFrame(frameCounter++); });
        ASSERT_GT(slots, f.Device->GetFramesInFlight())
            << "an update-phase ring must be deeper than the device's pacing";

        // First cycle: every slot's stored stamp starts at 0 while the live stamp is
        // non-zero, so each slot packs once — the count advances.
        const uint64_t before = f.Rs.Materials().GetMaterialSSBOPackCount();
        for (uint32_t i = 0; i < slots; ++i)
            RunFrame();
        const uint64_t afterFirstCycle = f.Rs.Materials().GetMaterialSSBOPackCount();
        EXPECT_GT(afterFirstCycle, before)
            << "pack count must advance over the first full ring cycle";

        // Idle frames: every slot already holds a byte-identical pack at the same
        // {buffer, offset}, so the skip fires and the count FREEZES.
        for (uint32_t i = 0; i < slots * 2u; ++i)
            RunFrame();
        EXPECT_EQ(f.Rs.Materials().GetMaterialSSBOPackCount(), afterFirstCycle)
            << "pack count must freeze while idle (the R0.1 skip gate)";

        // A property edit routes through MarkDirty and bumps the global content
        // epoch, busting the stamp -> the count advances one more ring cycle then
        // re-freezes. roughness is a statically-seeded schema lane, so the setter
        // works without shader reflection (headless).
        matA->SetFloat(HashStringId("roughness"), 0.42f);
        for (uint32_t i = 0; i < slots; ++i)
            RunFrame();
        const uint64_t afterEdit = f.Rs.Materials().GetMaterialSSBOPackCount();
        EXPECT_GT(afterEdit, afterFirstCycle)
            << "a MarkDirty (property edit) must bust the pack-skip stamp";
        for (uint32_t i = 0; i < slots * 2u; ++i)
            RunFrame();
        EXPECT_EQ(f.Rs.Materials().GetMaterialSSBOPackCount(), afterEdit)
            << "pack count must re-freeze once the edit is packed into every slot";

        // Unregister bumps the SSBO generation (pre-unregister callback), busting
        // the stamp -> another repack cycle.
        f.Rs.Materials().Registry().Unregister(guidB);
        for (uint32_t i = 0; i < slots; ++i)
            RunFrame();
        EXPECT_GT(f.Rs.Materials().GetMaterialSSBOPackCount(), afterEdit)
            << "an unregister (generation bump) must bust the pack-skip stamp";
    }
    f.Down();
}

// A material table that outgrows the MaterialParams ring slot grows the slot in
// the frame that needs it: no frame shades from the fallback row while the
// grow cap still covers the table (a folder of models registering theirs
// crosses the starting size mid-session).
TEST(RenderServicesRGPassTests, MaterialParamsGrowInTheFrameThatNeedsIt)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Rs.Textures().IsBindlessEnabled())
    {
        f.Down();
        GTEST_SKIP() << "Bindless unavailable — PackMaterialSSBO is gated on it";
    }
    {
        uint32_t frameCounter = 0;
        auto RunFrame = [&]()
        {
            f.Rs.BeginWorldDrawFrame();
            f.Rs.GetPerFrameWritePool().BeginFrame(frameCounter++);
            f.Rs.BuildWorldBatchKeys();
            f.Rs.Materials().FinalizeFrameBuffers();
        };

        std::vector<GUID> guids;
        for (int i = 0; i < 2; ++i)
        {
            guids.push_back(GUID::Generate());
            ASSERT_NE(RegisterPackGateMaterial(f.Rs, guids.back(), "GrowSeed"), nullptr);
        }
        RunFrame();
        const size_t stride = f.Rs.Materials().PackedMaterialParams().Size / 2u;
        ASSERT_GT(stride, 0u);

        const size_t capacity =
            f.Rs.GetPerFrameWritePool().GetCapacity(FrameWriteUsage::MaterialParams);
        const size_t grownCount = capacity / stride + 1u;
        for (size_t i = guids.size(); i < grownCount; ++i)
        {
            guids.push_back(GUID::Generate());
            ASSERT_NE(RegisterPackGateMaterial(f.Rs, guids.back(), "GrowFill"), nullptr);
        }

        RunFrame();
        EXPECT_EQ(f.Rs.Materials().GetMaterialParamsOverflowCount(), 0u)
            << "a table under the grow cap must never shade from the fallback row";
        EXPECT_EQ(f.Rs.Materials().PackedMaterialParams().Size, grownCount * stride)
            << "the frame that outgrows the slot must publish the full table";
    }
    f.Down();
}

// A table past the grow cap cannot fit, so PackMaterialSSBO counts the frame
// and binds the app-lifetime fallback row rather than keeping a ring handle.
// That row carries the TextureService's unassigned slot defaults, never zeros
// (bindless index 0 is the never-written sentinel, so a zeroed row would point
// every draw in the scene at an unwritten descriptor).
TEST(RenderServicesRGPassTests, MaterialParamsPastTheGrowCapFallBack)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Rs.Textures().IsBindlessEnabled())
    {
        f.Down();
        GTEST_SKIP() << "Bindless unavailable — PackMaterialSSBO is gated on it";
    }
    {
        // Re-initialize the pool with no MaterialParams headroom: its grow cap
        // is its starting size. MaterialSystem holds the pool by address, so
        // the re-initialized object is the one it allocates from.
        PerFrameWritePoolConfig capped{};
        capped.FramesInFlight = std::max(1u, f.Device->GetFramesInFlight());
        const size_t ssboAlign = std::max<size_t>(
            16, f.Device->GetCapabilities().minStorageBufferOffsetAlignment);
        for (auto& usage : capped.usages)
        {
            if (usage.bufferUsage == BufferUsage::Storage)
                usage.alignment = std::max(usage.alignment, ssboAlign);
        }
        auto& materialParams =
            capped.usages[static_cast<uint32_t>(FrameWriteUsage::MaterialParams)];
        materialParams.maxCapacityBytes = materialParams.capacityBytes;
        f.Rs.GetPerFrameWritePool().Shutdown();
        ASSERT_TRUE(f.Rs.GetPerFrameWritePool().Initialize(f.Device.get(), capped));

        uint32_t frameCounter = 0;
        auto RunFrame = [&]()
        {
            f.Rs.BeginWorldDrawFrame();
            f.Rs.GetPerFrameWritePool().BeginFrame(frameCounter++);
            f.Rs.BuildWorldBatchKeys();
            f.Rs.Materials().FinalizeFrameBuffers();
        };

        std::vector<GUID> guids;
        for (int i = 0; i < 2; ++i)
        {
            guids.push_back(GUID::Generate());
            ASSERT_NE(RegisterPackGateMaterial(f.Rs, guids.back(), "OverflowSeed"), nullptr);
        }
        RunFrame();
        const auto packed = f.Rs.Materials().PackedMaterialParams();
        const size_t stride = packed.Size / 2u;
        ASSERT_GT(stride, 0u);
        ASSERT_EQ(f.Rs.Materials().GetMaterialParamsOverflowCount(), 0u);

        const size_t overflowCount = materialParams.capacityBytes / stride + 1u;
        for (size_t i = guids.size(); i < overflowCount; ++i)
        {
            guids.push_back(GUID::Generate());
            ASSERT_NE(RegisterPackGateMaterial(f.Rs, guids.back(), "OverflowFill"), nullptr);
        }

        RunFrame();
        EXPECT_EQ(f.Rs.Materials().GetMaterialParamsOverflowCount(), 1u)
            << "the frame that does not fit must be counted, not silently dropped";

        const auto afterOverflow = f.Rs.Materials().PackedMaterialParams();
        EXPECT_FALSE(afterOverflow.Buffer == packed.Buffer)
            << "the previous frame's ring binding must not be silently reused";
        EXPECT_EQ(afterOverflow.Size, stride)
            << "the overflow frame must publish the single fallback row";

        // The fallback row is what the whole scene shades from on an overflow
        // frame, so its texture indices must be the TextureService defaults.
        // Zero is the reserved bindless index whose descriptor is never written:
        // sampling it is undefined, and the validation layer cannot see it while
        // descriptor buffers are enabled. The buffer is host-visible and
        // persistently mapped (RenderServices creates it as an upload buffer).
        {
            const auto& textures = f.Rs.Textures();
            const uint32_t white = textures.DefaultWhiteBindlessIndex();
            const uint32_t flatNormal = textures.DefaultFlatNormalBindlessIndex();
            const uint32_t black = textures.DefaultBlackBindlessIndex();
            ASSERT_NE(white, 0u) << "TextureService defaults must exist before this pin means anything";
            ASSERT_NE(flatNormal, 0u);
            ASSERT_NE(black, 0u);
            const uint32_t expected[8] = {white, flatNormal, white, white,
                                          white, flatNormal, white, black};

            void* mapped = f.Device->MapBuffer(afterOverflow.Buffer);
            ASSERT_NE(mapped, nullptr) << "the fallback row must be readable to be checked";
            uint32_t actual[8]{};
            std::memcpy(actual,
                        static_cast<const uint8_t*>(mapped) + kMaterialParamBytes,
                        sizeof(actual));
            f.Device->UnmapBuffer(afterOverflow.Buffer);
            for (uint32_t slot = 0; slot < 8u; ++slot)
            {
                EXPECT_EQ(actual[slot], expected[slot])
                    << "fallback row texture slot " << slot
                    << " must hold a written bindless descriptor, not the reserved index 0";
            }
        }
    }
    f.Down();
}

// The DDGI glossy resolve's declared set must cover its bound set on the arm
// where the lobes are OFF. Both kernels are one variant with the lobe flag as a
// UBO field / push constant, so the two passes bind Rough, Glossy, RoughBlur and
// GlossyBlur on every frame; a storage image that is bound but declared by no
// pass never leaves Undefined, and each dispatch binding it is a layout error.
// A default volume gives GlossyLobesActive() == false, which is exactly that arm
// — and the default DDGI configuration.
//
// Declaration-only, like the other read-set pins here: the contract lives in the
// builder lambdas, and the graph records it before anything executes.
TEST(RenderServicesRGPassTests, GlossyResolveDeclaresTheLobeTargetsItBindsWithLobesOff)
{
    RsFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    // Exercise the non-aliasing device contract on this real Vulkan device.
    // Native alias reuse and placeholder dimensions are covered separately by
    // RenderPipelineDeclareTests.DdgiInactiveLobesRespectStorageAliasingCapability.
    auto& caps = const_cast<RenderingDeviceCapabilities&>(f.Device->GetCapabilities());
    struct RestoreAliasingCapability
    {
        bool& Value;
        bool Previous;
        ~RestoreAliasingCapability() { Value = Previous; }
    } restore{caps.supportsAliasedStorageTextureBindings,
              caps.supportsAliasedStorageTextureBindings};
    caps.supportsAliasedStorageTextureBindings = false;
    Rendering::GPUScene* const gpuScene = f.Rs.GetGPUScene();
    if (!gpuScene)
    {
        f.Down();
        GTEST_SKIP() << "RenderServices published no GPUScene on this device, so "
                        "DDGIProbeFeature::Initialize cannot run — the declaration "
                        "contract is unchecked on this machine";
    }
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        auto& ddgi = f.Rs.EnsureFeature<DDGIProbeFeature>();
        ASSERT_TRUE(ddgi.Initialize(f.Device.get(), /*sceneAS=*/nullptr, gpuScene,
                                    &f.Rs.GetMeshGPURegistry(), &f.Rs.Materials()));
        ASSERT_FALSE(ddgi.GlossyLobesActive())
            << "a default volume must leave the lobes off — that is the arm under test";

        BufferDesc viewParamsDesc{};
        viewParamsDesc.size = 1024;
        viewParamsDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        viewParamsDesc.memoryUsage = BufferMemoryUsage::Upload;
        viewParamsDesc.debugName = "RGTDDGI.ViewParams";
        const BufferHandle viewParams = f.Device->CreateBuffer(viewParamsDesc);
        ASSERT_TRUE(viewParams.IsValid());

        // The resolve reconstructs positions from a 1-sample sampled depth and
        // declines outright on an MSAA one, so the depth's sample count is part
        // of reaching the code under test.
        const RenderGraph::RGTexture viewDepth =
            frame.ImportPersistentTexture("RGTDDGI.Depth", DepthTargetDesc());
        constexpr uint32_t kRenderWidth = 64;
        constexpr uint32_t kRenderHeight = 64;
        constexpr uint32_t kViewId = 0;

        RenderGraph::RGTexture rough{};
        RenderGraph::RGTexture glossy{};
        RenderGraph::RGTexture irradiance{};
        RenderGraph::RGTexture shadingNormal{};
        ASSERT_TRUE(ddgi.DeclareGlossyResolveForView(
            frame, f.Rs, viewParams, 0, viewParamsDesc.size, viewDepth, kRenderWidth, kRenderHeight,
            kViewId, /*viewExposureScale=*/1.0f, "DDGIGlossyResolve", rough, glossy, irradiance,
            shadingNormal))
            << "the resolve declares unconditionally on a 1-sample depth; a decline here means "
               "a shaderpkg did not stage next to the test binary, not that lobes are off";

        const RenderGraph::RGGraph& g = frame.Graph();
        const RenderGraph::RGPassId resolvePass = PassIdByName(g, "DDGIGlossyResolve");
        const RenderGraph::RGPassId blurPass = PassIdByName(g, "DDGIGlossyResolveBlur");
        ASSERT_NE(resolvePass, RenderGraph::kInvalidId);
        ASSERT_NE(blurPass, RenderGraph::kInvalidId);

        const std::string suffix = "." + std::to_string(kViewId);
        const RenderGraph::RGResourceId roughId =
            ResourceIdByName(g, "DDGIGlossyResolve.Rough" + suffix);
        const RenderGraph::RGResourceId glossyId =
            ResourceIdByName(g, "DDGIGlossyResolve.Glossy" + suffix);
        const RenderGraph::RGResourceId roughBlurId =
            ResourceIdByName(g, "DDGIGlossyResolve.RoughBlur" + suffix);
        const RenderGraph::RGResourceId glossyBlurId =
            ResourceIdByName(g, "DDGIGlossyResolve.GlossyBlur" + suffix);
        ASSERT_NE(roughId, RenderGraph::kInvalidId)
            << "Rough is bound as uResolveRough and uRoughIn but no pass declares it";
        ASSERT_NE(glossyId, RenderGraph::kInvalidId)
            << "Glossy is bound as uResolveGlossy and uGlossyIn but no pass declares it";
        ASSERT_NE(roughBlurId, RenderGraph::kInvalidId)
            << "RoughBlur is bound as uRoughOut but no pass declares it";
        ASSERT_NE(glossyBlurId, RenderGraph::kInvalidId)
            << "GlossyBlur is bound as uGlossyOut but no pass declares it";

        EXPECT_TRUE(g.HasWriteAccess(resolvePass, roughId))
            << "the resolve binds uResolveRough as a storage image every frame";
        EXPECT_TRUE(g.HasWriteAccess(resolvePass, glossyId))
            << "the resolve binds uResolveGlossy as a storage image every frame";
        EXPECT_TRUE(g.HasReadAccess(blurPass, roughId))
            << "the blur samples uRoughIn every frame";
        EXPECT_TRUE(g.HasReadAccess(blurPass, glossyId))
            << "the blur samples uGlossyIn every frame";
        EXPECT_TRUE(g.HasWriteAccess(blurPass, roughBlurId))
            << "the blur binds uRoughOut as a storage image every frame";
        EXPECT_TRUE(g.HasWriteAccess(blurPass, glossyBlurId))
            << "the blur binds uGlossyOut as a storage image every frame";

        // What the world pass samples is the blurred pair, so the published
        // handles must be the resources the blur declared its writes on.
        EXPECT_EQ(rough.Id, roughBlurId);
        EXPECT_EQ(glossy.Id, glossyBlurId);

        f.Device->DestroyBuffer(viewParams);
    }
    f.Down();
}
