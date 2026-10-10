// Pass-level guard for the terminal sRGB encode's TPDF dither.
//
// The dither shipped broken for 15 weeks (the two-IGN difference collapsed to
// a DC offset — zero band-breaking power) while every plumbing log looked
// healthy: the pass ran, push constants "wrote", the image was plausible. Only
// the output STATISTICS were wrong. This test renders a synthetic shallow
// gradient through the real AddSRGBEncodePassRG (production shaderpkg,
// reflected push constants, real pipeline) into an offscreen RGBA8 target and
// asserts the quantization statistics of the result:
//   - dithered: mean same-value run length along the gradient must be short;
//   - the encode itself must stay within 1 dither LSB of the ideal transfer
//     curve (catches over-amplitude noise and broken sampling).
// A re-broken TPDF (DC offset), a zeroed ditherLsb push constant, or a dropped
// PC write all reproduce the undithered staircase (mean run ~20 px here) and
// fail the first assertion.
//
// A further guard lives here because the dither is on by default, which makes
// its AMPLITUDE load-bearing rather than academic:
//   - DitherLsbFollowsTheDestinationFormat: the format matrix. Pins every row,
//     including the float targets that must take no dither at all (an HDR/scRGB
//     output and the pre-composite RGBA16F encode).
//
// Its REACH is no longer a property of this pass: both filters always cover the
// whole image, and which pixels that image holds is settled by where the finalize
// is declared — a world view's own resolve sees no chrome. The pass-side
// guarantee that a composite of already-finalized content is left alone lives in
// SrgbEncodeQuantizerTests, on FinalizeQuantizer::None.

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/SRGBEncodePass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Reference of the shader's LinearToSRGB (encode_srgb.frag) on one channel.
float LinearToSrgbRef(float c)
{
    if (c <= 0.0031308f)
        return 12.92f * c;
    return 1.055f * std::pow(std::max(c, 1e-6f), 1.0f / 2.4f) - 0.055f;
}

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

} // namespace

TEST(SrgbEncodeDither, GradientQuantizesDithered)
{
    SetEnv("GE_HEADLESS_TEST", "1");
    // The pass reads the kill switch once per process; pin it ON so a stray
    // environment override cannot turn this test into a false failure.
    SetEnv("GE_OUTPUT_DITHER", "1");
    // Pin the deband OFF: this guard measures the dither in isolation, and the
    // deband's tap clamping at the gradient's borders would otherwise lean on
    // the transfer-curve deviation budget below. SrgbEncodeDebandTests owns the
    // deband statistics in its own process (both latch once per process).
    SetEnv("GE_DEBAND", "0");

#ifdef RENDERING_SHADER_OUTPUT_DIR
    // The pass loads Shaders/encode_srgb.shaderpkg through the shader path
    // resolver. Point it at this build tree's compiled shader output so the
    // test is independent of ctest's working directory.
    Utils::SetShaderPathResolver(
        +[](const std::filesystem::path& rel) -> std::filesystem::path
        { return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / rel; });
#endif

    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    constexpr uint32_t kW = 512;
    constexpr uint32_t kH = 64;
    // Shallow linear ramp: encodes to ~26 sRGB levels across 512 px, so an
    // undithered quantize produces ~20 px runs — unmistakably distinct from
    // the dithered ~1.5-2 px.
    constexpr float kMaxLinear = 0.01f;

    // ── Source: RGBA32F horizontal gradient, uploaded and rested at
    // ShaderResource before the graph runs.
    TextureDesc srcDesc{};
    srcDesc.width = kW;
    srcDesc.height = kH;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = "DitherTest.LinearGradient";
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    ASSERT_TRUE(srcTex.IsValid());

    std::vector<float> pixels(static_cast<size_t>(kW) * kH * 4);
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const float v = kMaxLinear * static_cast<float>(x) / static_cast<float>(kW - 1);
            float* px = &pixels[(static_cast<size_t>(y) * kW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "DitherTest.Upload");
    ASSERT_TRUE(upload.IsValid());
    device->UpdateBuffer(upload, 0, srcBytes, pixels.data());

    {
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_NE(cl, nullptr);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, srcTex, 0, 0, kW, kH, 0,
                                           static_cast<size_t>(kW) * 4 * sizeof(float));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(srcTex, ResourceState::CopyDest,
                                                          ResourceState::ShaderResource));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        device->ExecuteCommandLists(lists);
        device->WaitForIdle();
    }

    // ── Graph: real terminal encode into a transient RGBA8 target, then a
    // test-owned copy into a readback buffer.
    const size_t outBytes = static_cast<size_t>(kW) * kH * 4;
    const BufferHandle readback = device->CreateReadbackBuffer(outBytes, "DitherTest.Readback");
    ASSERT_TRUE(readback.IsValid());

    FramePools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "DitherTest.Linear", srcTex, ResourceState::ShaderResource,
        TextureFormat::R32G32B32A32_FLOAT);
    ASSERT_TRUE(src.IsValid());

    TextureDesc dstDesc{};
    dstDesc.width = kW;
    dstDesc.height = kH;
    dstDesc.depth = 1;
    dstDesc.mipLevels = 1;
    dstDesc.arrayLayers = 1;
    dstDesc.sampleCount = 1;
    dstDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dstDesc.debugName = "DitherTest.Encoded";
    const RG::RGTexture dst = frame.CreateTexture("DitherTest.Encoded", dstDesc);
    ASSERT_TRUE(dst.IsValid());

    // Offscreen dst → the pass selects outEncoding=1 (manual sRGB) with the
    // SDR dither step. A missing shaderpkg would silently skip the pass and
    // this guard with it — that must be loud, not a skip.
    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = Passes::FinalizeInputSpace::Linear,
         .Quantizer = Passes::FinalizeQuantizer::Destination},
        "DitherTest.Encode");
    ASSERT_TRUE(encodePass.IsValid())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    frame.AddPass(
        "DitherTest.Readback", PassPhase::kFinalize,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(dst, RG::RGTextureRead::CopySrc);
            p.PreventCulling(); // consumed by the CPU, not the graph
        },
        [dst, readback](RG::RGContext& ctx)
        { ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, kW, kH); });

    frame.Execute();
    device->WaitForIdle();

    const void* mapped = device->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    const uint8_t* out = static_cast<const uint8_t*>(mapped);

    // ── Statistics on the red channel along the gradient axis (all channels
    // carry the same signal; rows are independent dither realizations).
    uint64_t runs = 0;
    uint8_t minV = 255;
    uint8_t maxV = 0;
    int maxAbsDev = 0;
    for (uint32_t y = 0; y < kH; ++y)
    {
        int prev = -1;
        for (uint32_t x = 0; x < kW; ++x)
        {
            const uint8_t v = out[(static_cast<size_t>(y) * kW + x) * 4];
            if (static_cast<int>(v) != prev)
            {
                ++runs;
                prev = v;
            }
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);

            const float lin = kMaxLinear * static_cast<float>(x) / static_cast<float>(kW - 1);
            const int ideal =
                static_cast<int>(std::lround(LinearToSrgbRef(lin) * 255.0f));
            maxAbsDev = std::max(maxAbsDev, std::abs(static_cast<int>(v) - ideal));
        }
    }
    device->UnmapBuffer(readback);

    const double meanRun = static_cast<double>(kW) * kH / static_cast<double>(runs);

    // The gradient must actually span its encoded range — a constant or black
    // output would otherwise pass a run-length test trivially in either
    // direction.
    EXPECT_GE(static_cast<int>(maxV) - static_cast<int>(minV), 15)
        << "encoded gradient span collapsed (min=" << +minV << " max=" << +maxV << ")";

    // Dither present: TPDF at 1/255 breaks the staircase into ~1.5-2 px runs.
    // Undithered (broken TPDF, zeroed/dropped ditherLsb PC) measures ~20 px.
    EXPECT_LT(meanRun, 3.0) << "quantization staircase detected — terminal encode dither "
                               "is dead (mean run "
                            << meanRun << " px over " << runs << " runs)";

    // Noise budget: a correct 1-LSB TPDF stays within 1 LSB of the ideal
    // transfer curve (+1 for UNORM store rounding). Catches over-amplitude
    // dither and garbage sampling that a run-length test alone would bless.
    EXPECT_LE(maxAbsDev, 2) << "encoded output deviates from the sRGB transfer curve by "
                            << maxAbsDev << " LSB — noise exceeds the dither budget";
}

// The format matrix, pinned row by row. This is the single decision that keeps
// the dither amplitude and the deband's N-LSB gate agreeing about how deep the
// destination is, so it is worth asserting directly rather than only through the
// one format an offscreen render test happens to use.
TEST(SrgbEncodeDither, DitherLsbFollowsTheDestinationFormat)
{
    constexpr float kLsb8Bit = 1.0f / 255.0f;
    constexpr float kLsb10Bit = 1.0f / 1023.0f;

    // 8-bit UNORM and 8-bit _SRGB both quantize 256 levels; the _SRGB ROP moves
    // WHERE the transfer function runs, never how deep the store is.
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::RGBA8_UNORM), kLsb8Bit);
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::BGRA8_UNORM), kLsb8Bit);
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::RGBA8_SRGB), kLsb8Bit);
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::BGRA8_SRGB), kLsb8Bit);

    // The 10-bit swapchain this engine prefers on Windows. A 1/255 amplitude
    // here would be 4x the real quantizer — the regression that made a
    // fullscreen dither visibly grain flat surfaces.
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::RGB10A2_UNORM), kLsb10Bit);

    // Float destinations have no quantizer of their own: no dither, no deband.
    // Covers the HDR scRGB surface — and it is exactly why the Player's FP16 HUD
    // composite names FinalizeQuantizer::Presented rather than taking its
    // destination's (absent) step.
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::R16G16B16A16_FLOAT), 0.0f);
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::R32G32B32A32_FLOAT), 0.0f);
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::R11G11B10_FLOAT), 0.0f);

    // Unknown is the imported-target case (Format 0) and must stay conservative:
    // assume a quantizer exists rather than silently dropping the dither.
    EXPECT_FLOAT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::Unknown), kLsb8Bit);
}
