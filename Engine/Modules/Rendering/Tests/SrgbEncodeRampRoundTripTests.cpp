// KILL-CONDITION gate for the Finalize contract's `_SRGB`-destination arm
// (#767 P6b, design measurement 5).
//
// When the SDR blend-space flip lands, the terminal encode receives an input
// that is ALREADY sRGB-encoded. On a UNORM destination it stores those bytes
// raw. On an `_SRGB` destination it cannot: the ROP applies the sRGB OETF on
// write, so storing the encoded value would encode it twice. The contract's
// answer is to hand the ROP the sRGB DECODE D(c) and rely on the hardware
// re-encode to reproduce the byte — the round trip E(D(x)).
//
// That round trip is a DRIVER property, not a mathematical identity: Vulkan
// permits tolerance in the fixed-function sRGB conversion, and a hardware LUT
// approximation can miss byte-exactness. So it is measured here rather than
// assumed. The gate is absolute — ANY level off by one fails, because a
// terminal encode that shifts a level is a visible banding/tint defect on
// every frame that takes this arm.
//
// On failure the design's fallback is VK_KHR_swapchain_mutable_format UNORM
// views. That fallback is deliberately NOT implemented on spec: this test
// names the failing levels so the decision is made on evidence.
//
// What this test proves: for an `_SRGB` colour attachment on this device and
// driver, the shader's arm-6 output survives the ROP encode byte-exact for all
// 256 levels. What it does NOT prove: that a real `_SRGB` SWAPCHAIN image
// behaves identically (same format and same ROP, so it should — but this test
// renders offscreen and never presents), nor anything about the compositor or
// display link downstream of present.

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Rendering/Passes/SRGBEncodePass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
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

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

} // namespace

TEST(SrgbEncodeRampRoundTrip, EncodedInputSurvivesSrgbRopForAll256Levels)
{
    SetEnv("GE_HEADLESS_TEST", "1");
    // Dither and deband both perturb the output ON PURPOSE — they are the rest
    // of what this arm does. This gate measures the transfer round trip in
    // isolation, so both are pinned off; a ±1 from dither would be
    // indistinguishable from the driver defect being hunted.
    SetEnv("GE_OUTPUT_DITHER", "0");
    SetEnv("GE_DEBAND", "0");

#ifdef RENDERING_SHADER_OUTPUT_DIR
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

    // One column per 8-bit level; a few rows so a per-pixel fault (rather than
    // a per-level one) is visible as a row disagreement.
    constexpr uint32_t kW = 256;
    constexpr uint32_t kH = 4;

    // ── Source: RGBA32F holding ENCODED values. This is the flip's world —
    // the composite carries sRGB-encoded values in a float target, which is
    // exactly why the readback/space declarations of P5 exist. Level i is
    // i/255 exactly.
    TextureDesc srcDesc{};
    srcDesc.width = kW;
    srcDesc.height = kH;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = "RampTest.EncodedRamp";
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    ASSERT_TRUE(srcTex.IsValid());

    std::vector<float> pixels(static_cast<size_t>(kW) * kH * 4);
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const float v = static_cast<float>(x) / 255.0f;
            float* px = &pixels[(static_cast<size_t>(y) * kW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "RampTest.Upload");
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

    const size_t outBytes = static_cast<size_t>(kW) * kH * 4;
    const BufferHandle readback = device->CreateReadbackBuffer(outBytes, "RampTest.Readback");
    ASSERT_TRUE(readback.IsValid());

    FramePools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "RampTest.EncodedRamp", srcTex, ResourceState::ShaderResource,
        TextureFormat::R32G32B32A32_FLOAT);
    ASSERT_TRUE(src.IsValid());

    // The destination that makes this test the gate it is: an `_SRGB` format,
    // so the ROP applies the hardware encode on write. The pass keys arm 6 off
    // this format, not off backbuffer-ness — which is precisely what lets the
    // arm be tested offscreen at all.
    TextureDesc dstDesc{};
    dstDesc.width = kW;
    dstDesc.height = kH;
    dstDesc.depth = 1;
    dstDesc.mipLevels = 1;
    dstDesc.arrayLayers = 1;
    dstDesc.sampleCount = 1;
    dstDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_SRGB);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dstDesc.debugName = "RampTest.SrgbRop";
    const RG::RGTexture dst = frame.CreateTexture("RampTest.SrgbRop", dstDesc);
    ASSERT_TRUE(dst.IsValid());

    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb,
         .Quantizer = Passes::FinalizeQuantizer::Destination,
         .VolumeDebandThresholdLsb = 0.0f},
        "RampTest.Finalize");
    ASSERT_TRUE(encodePass.IsValid())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    frame.AddPass(
        "RampTest.Readback", PassPhase::kFinalize,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(dst, RG::RGTextureRead::CopySrc);
            p.PreventCulling();
        },
        [dst, readback](RG::RGContext& ctx)
        { ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, kW, kH); });

    frame.Execute();
    device->WaitForIdle();

    const void* mapped = device->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    const uint8_t* out = static_cast<const uint8_t*>(mapped);

    // ── The gate. Copying an `_SRGB` image to a buffer yields the STORED bytes
    // with no decode, so out[x] is E(D(x)) quantized — the byte a display
    // would receive.
    std::string failures;
    int failureCount = 0;
    int maxAbsError = 0;
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const int got = static_cast<int>(out[(static_cast<size_t>(y) * kW + x) * 4]);
            const int want = static_cast<int>(x);
            const int err = got - want;
            if (err != 0)
            {
                ++failureCount;
                maxAbsError = std::max(maxAbsError, std::abs(err));
                if (failureCount <= 32)
                {
                    failures += "\n  level " + std::to_string(want) + " (row " +
                                std::to_string(y) + ") -> " + std::to_string(got) +
                                "  (error " + (err > 0 ? "+" : "") + std::to_string(err) + ")";
                }
            }
        }
    }
    device->UnmapBuffer(readback);

    EXPECT_EQ(failureCount, 0)
        << "E(D(x)) round trip is NOT byte-exact on this driver: " << failureCount
        << " of " << (kW * kH) << " samples differ, max |error| = " << maxAbsError
        << " level(s)." << failures
        << "\n\nThis is the #767 P6b kill condition. Do NOT relax this gate. The design's"
           "\nfallback is VK_KHR_swapchain_mutable_format UNORM views for the `_SRGB` arm;"
           "\nreport the failing levels above and let that decision be made on this evidence.";
}
