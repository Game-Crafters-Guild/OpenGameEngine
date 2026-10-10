// Stage 2c step 1: RGContext parity — pipeline format-key derivation from the
// pass's declared attachments (lazy, memoized, per-pass isolated), MSAA sample
// plumbing, external-import format resolution, and variant-lookup guards. Runs
// over the REAL headless Vulkan device.

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Tests/RenderGraph/RGTestDevice.h"

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;

namespace
{
TextureDesc Tex(TextureFormat fmt, uint32_t usage, uint32_t samples = 1, uint32_t w = 64,
                uint32_t h = 64)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(fmt);
    d.usage = usage;
    d.sampleCount = samples;
    return d;
}
TextureDesc Color(TextureFormat fmt = TextureFormat::RGBA8_UNORM, uint32_t samples = 1)
{
    return Tex(fmt, static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget),
               samples);
}
TextureDesc Depth()
{
    return Tex(TextureFormat::D32_FLOAT,
               static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource));
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};
} // namespace

#define RG_REQUIRE_DEVICE(dev)                                                                        \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
    GTEST_SKIP() << "no headless device available"

TEST(RGContext, FormatKeyDerivesFromDeclaredAttachments)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture c0 = frame.CreateTexture("C0", Color(TextureFormat::R16G16B16A16_FLOAT));
    RGTexture c1 = frame.CreateTexture("C1", Color(TextureFormat::RGBA8_UNORM));
    RGTexture d = frame.CreateTexture("D", Depth());

    PipelineFormatKey fk{};
    frame.AddPass("Mrt", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.AttachColor(0, c0, {.Load = RGLoadOp::Clear});
                      p.AttachColor(1, c1, {.Load = RGLoadOp::Clear});
                      p.AttachDepth(d, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}});
                  },
                  [&](RGContext& ctx) { fk = ctx.BuildCurrentFormatKey(); });
    frame.MarkOutput(c0);
    frame.MarkOutput(c1);
    frame.Execute();

    EXPECT_EQ(fk.ColorCount, 2u);
    EXPECT_EQ(fk.ColorFormats[0], TextureFormat::R16G16B16A16_FLOAT);
    EXPECT_EQ(fk.ColorFormats[1], TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(fk.DepthFormat, TextureFormat::D32_FLOAT);
    EXPECT_EQ(fk.StencilFormat, TextureFormat{}) << "stencil never participates in the cache key";
    EXPECT_EQ(fk.RasterizationSamples, 1u);
}

TEST(RGContext, MsaaSampleCountFlowsFromDeclaredDescAndKeysOnTheMsaaTarget)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture msaa = frame.CreateTexture("Msaa", Color(TextureFormat::RGBA8_UNORM, /*samples=*/4));
    RGTexture resolve = frame.CreateTexture("Resolve", Color());

    PipelineFormatKey fk{};
    uint32_t samples = 0;
    frame.AddPass("WorldMsaa", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachColorResolve(0, msaa, resolve, {.Load = RGLoadOp::Clear}); },
                  [&](RGContext& ctx)
                  {
                      fk = ctx.BuildCurrentFormatKey();
                      samples = ctx.GetCurrentSampleCount();
                  });
    frame.MarkOutput(resolve);
    frame.Execute();

    EXPECT_EQ(samples, 4u);
    EXPECT_EQ(fk.RasterizationSamples, 4u);
    EXPECT_EQ(fk.ColorCount, 1u);
    EXPECT_EQ(fk.ColorFormats[0], TextureFormat::RGBA8_UNORM) << "keys on the MSAA target, not the resolve";
}

// Depth-only is the engine's most common pass shape (depth prepass, 4 CSM
// cascades): DepthFormat set, ColorCount 0 — and the swapchain fallback must
// NOT fire (it applies only to passes binding nothing at all).
TEST(RGContext, DepthOnlyPassKeyHasNoColorSlots)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture d = frame.CreateTexture("D", Depth());
    PipelineFormatKey fk{};
    frame.AddPass("DepthPrepass", 0,
                  [&](RGPassBuilder& p)
                  { p.AttachDepth(d, {.Load = RGLoadOp::Clear, .Clear = {.Depth = 0.0f}}); },
                  [&](RGContext& ctx) { fk = ctx.BuildCurrentFormatKey(); });
    frame.MarkOutput(d);
    frame.Execute();

    EXPECT_EQ(fk.ColorCount, 0u) << "depth-only must not enter the swapchain fallback";
    EXPECT_EQ(fk.DepthFormat, TextureFormat::D32_FLOAT);
    EXPECT_EQ(fk.RasterizationSamples, 1u);
}

// The pure derivation core — incl. the swapchain-fallback branch the headless
// harness can't reach through the device (enableSwapchain=false ⇒ Unknown).
TEST(RGContext, DeriveFormatKeyFallbackGateAndSlotClamp)
{
    // Attachment-less + real swapchain format: fallback fires.
    PipelineFormatKey fk = DeriveFormatKey(nullptr, 0, TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(fk.ColorCount, 1u);
    EXPECT_EQ(fk.ColorFormats[0], TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(fk.RasterizationSamples, 1u);

    // Attachment-less + no swapchain (headless): empty key.
    fk = DeriveFormatKey(nullptr, 0, TextureFormat{});
    EXPECT_EQ(fk.ColorCount, 0u);
    EXPECT_EQ(fk.DepthFormat, TextureFormat{});

    // Depth-only + swapchain available: fallback must NOT fire.
    const RGAttachmentKeyInput depthOnly[] = {{0, true, TextureFormat::D32_FLOAT, 1}};
    fk = DeriveFormatKey(depthOnly, 1, TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(fk.ColorCount, 0u) << "fallback applies ONLY when nothing is bound";
    EXPECT_EQ(fk.DepthFormat, TextureFormat::D32_FLOAT);

    // Out-of-range color slot: release-safe skip (old context's clamp) — the
    // record contributes NOTHING (declaration already rejected + logged it);
    // the valid slot still keys.
    const RGAttachmentKeyInput wild[] = {{0, false, TextureFormat::RGBA8_UNORM, 1},
                                         {PipelineFormatKey::kMaxColors + 3, false,
                                          TextureFormat::R16G16B16A16_FLOAT, 4}};
    fk = DeriveFormatKey(wild, 2, TextureFormat{});
    EXPECT_EQ(fk.ColorCount, 1u);
    EXPECT_EQ(fk.ColorFormats[0], TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(fk.RasterizationSamples, 1u) << "a rejected record contributes nothing";
}

TEST(RGContext, FormatKeyIsPerPassIsolated)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture a = frame.CreateTexture("A", Color(TextureFormat::R16G16B16A16_FLOAT));
    RGTexture b = frame.CreateTexture("B", Color(TextureFormat::RGBA8_UNORM));
    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    RGBuffer out = frame.CreateBuffer("Out", bd);

    PipelineFormatKey k1{}, k1Again{}, k2{}, kCompute{};
    frame.AddPass("P1", 0, [&](RGPassBuilder& p) { p.AttachColor(0, a); },
                  [&](RGContext& ctx)
                  {
                      k1 = ctx.BuildCurrentFormatKey();
                      // Note: memoization is a perf property this can't falsify
                      // (a recompute is deterministic); the pinned property is
                      // per-pass ISOLATION below.
                      k1Again = ctx.BuildCurrentFormatKey();
                  });
    frame.AddPass("P2", 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(a);
                      p.AttachColor(0, b);
                  },
                  [&](RGContext& ctx) { k2 = ctx.BuildCurrentFormatKey(); });
    // Attachment-less compute pass AFTER raster passes: on a headless device
    // (no swapchain ⇒ no fallback format) the key must come back EMPTY — a
    // stale key from P2 here means the record loop forgot m_KeyDirty.
    frame.AddComputePass("C", 0,
                         [&](RGPassBuilder& p)
                         {
                             p.Read(b);
                             p.Write(out);
                         },
                         [&](RGContext& ctx) { kCompute = ctx.BuildCurrentFormatKey(); });
    frame.MarkOutput(out);
    frame.Execute();

    EXPECT_TRUE(k1 == k1Again);
    EXPECT_EQ(k1.ColorFormats[0], TextureFormat::R16G16B16A16_FLOAT);
    EXPECT_EQ(k2.ColorFormats[0], TextureFormat::RGBA8_UNORM);
    EXPECT_FALSE(k1 == k2) << "different attachments must produce different keys";
    EXPECT_EQ(kCompute.ColorCount, 0u) << "no stale key leaked into the attachment-less pass";
    EXPECT_EQ(kCompute.DepthFormat, TextureFormat{});
    EXPECT_EQ(kCompute.RasterizationSamples, 1u);
}

TEST(RGContext, ExternalImportResolvesFormatExplicitlyOrViaDeviceQuery)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    const TextureDesc extDesc = Color(TextureFormat::R16G16B16A16_FLOAT);
    const TextureHandle ext = dev->CreateTexture(extDesc);
    ASSERT_TRUE(ext.IsValid());

    // Unknown format: the key build falls back to the backend's GetTextureFormat.
    frame.BeginFrame(0);
    RGTexture t0 = frame.ImportExternalTexture("Ext", ext, ResourceState::Common, TextureFormat{});
    PipelineFormatKey fk{};
    frame.AddPass("DrawToExt", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t0); },
                  [&](RGContext& ctx) { fk = ctx.BuildCurrentFormatKey(); });
    frame.MarkOutput(t0);
    frame.Execute();
    EXPECT_EQ(fk.ColorFormats[0], TextureFormat::R16G16B16A16_FLOAT)
        << "format=0 import must resolve via the device query";

    // Explicit format: no device query needed, same key.
    frame.BeginFrame(1);
    RGTexture t1 = frame.ImportExternalTexture("Ext", ext, ResourceState::RenderTarget,
                                               TextureFormat::R16G16B16A16_FLOAT);
    PipelineFormatKey fk2{};
    frame.AddPass("DrawToExt", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t1); },
                  [&](RGContext& ctx) { fk2 = ctx.BuildCurrentFormatKey(); });
    frame.MarkOutput(t1);
    frame.Execute();
    EXPECT_TRUE(fk == fk2) << "explicit and queried formats must produce the same cache key";

    dev->WaitForIdle();
    dev->DestroyTexture(ext);
}

TEST(RGContext, InvalidPipelineIdsAreGuarded)
{
    RG_REQUIRE_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RGTexture c = frame.CreateTexture("C", Color());
    bool ran = false;
    frame.AddPass("Guards", 0, [&](RGPassBuilder& p) { p.AttachColor(0, c); },
                  [&](RGContext& ctx)
                  {
                      ran = true;
                      EXPECT_FALSE(ctx.GetOrCreatePipelineVariant(GraphicsPipelineId{}).IsValid());
                      EXPECT_FALSE(ctx.GetOrCreatePipelineVariant(ComputePipelineId{}).IsValid());
                      ctx.SetPipelineAuto(GraphicsPipelineId{}); // logs, must not crash or bind
                      ctx.SetPipelineAuto(ComputePipelineId{});
                      EXPECT_NE(ctx.GetDevice(), nullptr);
                  });
    frame.MarkOutput(c);
    frame.Execute();
    dev->WaitForIdle();
    EXPECT_TRUE(ran);
}
