// P5's per-target format query and its consumption by the Presented arm.
//
// Two live windows on one device hold two different swapchain formats
// (WindowTargetHdrRequestTests pins the banking); every FinalizeQuantizer::
// Presented pass sized from the device's AMBIENT format therefore depends on
// which target happens to be active when it declares. This suite pins the two
// halves of the fix:
//
//   (i)  IDevice::GetWindowTargetSwapchainFormat(handle) answers for ANY live
//        target without switching actives — the query production callers
//        resolve their own window's format through, per frame.
//   (ii) CONSUMPTION: with window A (10-bit) active, an encode pass handed
//        window B's queried format dithers at B's 8-bit step — the parameter
//        beats the active target.
//   (iii) CONTROL: the same declare with the parameter unset dithers at A's
//        10-bit step — the ambient read follows the ACTIVE target. This arm IS
//        the pre-P5 behaviour: it proves wrong-format consumption is real the
//        moment declare-order and presentation diverge, and that the
//        instrument resolves the difference arm (ii) rests on.
//
// The SDR divergence lever is SetPreferTenBitSwapchain flipped between the two
// CreateAndActivateWindowTarget calls (a VulkanDevice member; module tests may
// cast, as DeviceDiagnosticsTests does). Discipline inherited from the HDR
// sibling: when the two targets fail to produce two different dither steps on
// this agent, the run reports a loud SKIP rather than passing on assertions
// that could not discriminate.

#include "Source/Vulkan/VulkanDevice.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Rendering/Passes/SRGBEncodePass.h"

#include <gtest/gtest.h>

#if defined(HAVE_GLFW)
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
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

// The dither is the instrument; the deband would turn the encode into a
// neighbourhood filter the amplitude model here does not reproduce. Latched
// per process on first read, so pinned before any pass declares.
void PinEnvironment()
{
    SetEnv("GE_OUTPUT_DITHER", "1");
    SetEnv("GE_DEBAND", "0");
    SetEnv("GE_DEBAND_THRESHOLD", "");
    SetEnv("GE_DEBAND_RADIUS", "");

#ifdef RENDERING_SHADER_OUTPUT_DIR
    Utils::SetShaderPathResolver(
        +[](const std::filesystem::path& rel) -> std::filesystem::path
        { return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / rel; });
#endif
}

#if defined(HAVE_GLFW)

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

constexpr uint32_t kW = 512;
constexpr uint32_t kH = 64;
// Shallow linear ramp (the SrgbEncodeQuantizer instrument): encodes across ~26
// sRGB levels, so a dither of either candidate step is unmistakable against
// the ideal curve.
constexpr float kMaxLinear = 0.01f;

TextureHandle MakeRampSource(IDevice* device)
{
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

    TextureDesc srcDesc{};
    srcDesc.width = kW;
    srcDesc.height = kH;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = "PresentedFormatTest.Src";
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    if (!srcTex.IsValid())
        return srcTex;

    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "PresentedFormatTest.Upload");
    if (!upload.IsValid())
        return {};
    device->UpdateBuffer(upload, 0, srcBytes, pixels.data());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    if (!cl)
        return {};
    cl->Begin();
    cl->CopyBufferToTextureSubresource(upload, srcTex, 0, 0, kW, kH, 0,
                                       static_cast<size_t>(kW) * 4 * sizeof(float));
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(srcTex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();
    return srcTex;
}

// One Presented encode of the ramp into an offscreen F32 destination; returns
// the max |stored - ideal curve| over the red channel — the total displacement
// the dither produced, which a float destination preserves exactly (no
// rounding term of its own). Negative on failure.
float RunPresentedEncodeMaxDeviation(IDevice* device, TextureHandle srcTex,
                                     std::optional<TextureFormat> presentedFormat,
                                     const char* passName)
{
    const size_t outBytes = static_cast<size_t>(kW) * kH * 16;
    const BufferHandle readback =
        device->CreateReadbackBuffer(outBytes, "PresentedFormatTest.Readback");
    if (!readback.IsValid())
        return -1.0f;

    FramePools pools(device);
    RG::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "PresentedFormatTest.Src", srcTex, ResourceState::ShaderResource,
        TextureFormat::R32G32B32A32_FLOAT);
    if (!src.IsValid())
        return -1.0f;

    TextureDesc dstDesc{};
    dstDesc.width = kW;
    dstDesc.height = kH;
    dstDesc.depth = 1;
    dstDesc.mipLevels = 1;
    dstDesc.arrayLayers = 1;
    dstDesc.sampleCount = 1;
    dstDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dstDesc.debugName = "PresentedFormatTest.Dst";
    const RG::RGTexture dst = frame.CreateTexture("PresentedFormatTest.Dst", dstDesc);
    if (!dst.IsValid())
        return -1.0f;

    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = Passes::FinalizeInputSpace::Linear,
         .Quantizer = Passes::FinalizeQuantizer::Presented,
         .VolumeDebandThresholdLsb = 0.0f,
         .PresentedFormat = presentedFormat},
        passName);
    if (!encodePass.IsValid())
        return -1.0f;

    frame.AddPass(
        "PresentedFormatTest.Readback", PassPhase::kFinalize,
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
    if (!mapped)
        return -1.0f;
    std::vector<float> out(static_cast<size_t>(kW) * kH * 4);
    std::memcpy(out.data(), mapped, outBytes);
    device->UnmapBuffer(readback);

    float maxDev = 0.0f;
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
            const float lin = kMaxLinear * static_cast<float>(x) / static_cast<float>(kW - 1);
            maxDev = std::max(maxDev, std::abs(out[i] - LinearToSrgbRef(lin)));
        }
    }
    return maxDev;
}

#endif // HAVE_GLFW

} // namespace

TEST(WindowTargetPresentedFormat, QueryAnswersPerTargetAndTheParameterBeatsTheActiveTarget)
{
#if !defined(HAVE_GLFW)
    GTEST_SKIP() << "GLFW not available on this build agent";
#else
    PinEnvironment();

    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* windowA = glfwCreateWindow(320, 200, "PresentedFormatTargetA", nullptr, nullptr);
    GLFWwindow* windowB = glfwCreateWindow(256, 160, "PresentedFormatTargetB", nullptr, nullptr);
    ASSERT_NE(windowA, nullptr);
    ASSERT_NE(windowB, nullptr);

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc))
    {
        glfwDestroyWindow(windowB);
        glfwDestroyWindow(windowA);
        glfwTerminate();
        GTEST_SKIP() << "No Vulkan device available on this agent";
    }
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    // Window A under the default preference (ten-bit on Windows).
    vk->SetPreferTenBitSwapchain(true);
    WindowTargetHandle targetA{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowA, 320, 200, &targetA));
    const TextureFormat formatA = dev->GetSwapchainTextureFormat();
    ASSERT_NE(formatA, TextureFormat::Unknown)
        << "window target A reports no swapchain format; nothing below can discriminate";

    // Window B with the ten-bit preference dropped: the SDR divergence lever.
    vk->SetPreferTenBitSwapchain(false);
    WindowTargetHandle targetB{};
    ASSERT_TRUE(dev->CreateAndActivateWindowTarget(windowB, 256, 160, &targetB));
    const TextureFormat formatB = dev->GetSwapchainTextureFormat();
    ASSERT_NE(formatB, TextureFormat::Unknown);

    // What the consumption arms discriminate on is the two formats' DITHER
    // STEPS, not their identities: same-step formats cannot separate the
    // parameter read from the ambient read, so a run that failed to diverge
    // proves nothing and must say so.
    const float stepA = Passes::EncodeDitherLsbForFormat(formatA);
    const float stepB = Passes::EncodeDitherLsbForFormat(formatB);
    const bool diverged = formatA != formatB && stepA > 0.0f && stepB > 2.0f * stepA;

    // ── Arm (i): the query answers for every live target, actives untouched. ──
    // B is active; A's format must be readable without activating A.
    ASSERT_EQ(dev->GetActiveWindowTarget().id, targetB.id);
    EXPECT_EQ(dev->GetWindowTargetSwapchainFormat(targetA), formatA)
        << "the banked target's format must be readable without switching actives; got "
        << ToString(dev->GetWindowTargetSwapchainFormat(targetA)) << ", expected "
        << ToString(formatA);
    EXPECT_EQ(dev->GetWindowTargetSwapchainFormat(targetB), formatB)
        << "the ACTIVE target's format must come from the live member";
    EXPECT_EQ(dev->GetActiveWindowTarget().id, targetB.id)
        << "the query switched the active target — it must be a pure read";
    EXPECT_EQ(dev->GetWindowTargetSwapchainFormat(WindowTargetHandle{}), TextureFormat::Unknown)
        << "an invalid handle must report Unknown, never a neighbour's format";

    float suppliedDev = -1.0f;
    float ambientDev = -1.0f;
    float controlDev = -1.0f;
    if (diverged)
    {
        // ── Arms (ii)+(iii): consumption. A (the finer step) is ACTIVE; the ──
        // pass writes an offscreen float destination either way, so the only
        // difference between the arms is which format sizes the dither.
        ASSERT_TRUE(dev->SetActiveWindowTarget(targetA));
        ASSERT_EQ(dev->GetSwapchainTextureFormat(), formatA);

        const TextureHandle srcTex = MakeRampSource(dev.get());
        ASSERT_TRUE(srcTex.IsValid());

        // (ii) The parameter beats the active target: B's queried format is
        // supplied while A is active, and the dither must land at B's step.
        suppliedDev = RunPresentedEncodeMaxDeviation(
            dev.get(), srcTex, dev->GetWindowTargetSwapchainFormat(targetB),
            "PresentedFormatTest.SuppliedB");
        ASSERT_GE(suppliedDev, 0.0f)
            << "encode pass declared nothing — encode_srgb.shaderpkg not staged under the build "
               "root's Shaders/";

        // (iii) CONTROL — the ambient read follows the active target. This arm
        // is the pre-P5 production behaviour, kept as the pin that the
        // coupling arm (ii) severs is real.
        ambientDev = RunPresentedEncodeMaxDeviation(dev.get(), srcTex, std::nullopt,
                                                    "PresentedFormatTest.AmbientA");
        ASSERT_GE(ambientDev, 0.0f);

        // And the discriminator's positive control: supplying A's own format
        // must agree with the ambient read while A is active.
        controlDev = RunPresentedEncodeMaxDeviation(
            dev.get(), srcTex, dev->GetWindowTargetSwapchainFormat(targetA),
            "PresentedFormatTest.SuppliedA");
        ASSERT_GE(controlDev, 0.0f);

        std::cout << "[ MEASURED ] steps: A(" << ToString(formatA) << ") " << stepA << ", B("
                  << ToString(formatB) << ") " << stepB << "; max deviation supplied-B "
                  << suppliedDev << ", ambient-A " << ambientDev << ", supplied-A " << controlDev
                  << std::endl;

        // (ii): sized to B — above anything A's step can produce (a unit TPDF
        // cannot displace past 1.5x its own step) and inside 1.5x B's.
        EXPECT_GT(suppliedDev, 2.0f * stepA)
            << "the supplied format did not beat the active target: max deviation " << suppliedDev
            << " is inside what the ACTIVE target's step " << stepA << " produces";
        EXPECT_LE(suppliedDev, 1.5f * stepB)
            << "max deviation " << suppliedDev << " exceeds 1.5x the supplied step " << stepB;

        // (iii): sized to A — dither present, and bounded by A's step, which
        // is what a declare under a different active target would break.
        EXPECT_GT(ambientDev, 0.25f * stepA)
            << "the ambient arm left the destination undithered (" << ambientDev << ")";
        EXPECT_LE(ambientDev, 1.5f * stepA)
            << "the ambient arm's max deviation " << ambientDev
            << " exceeds 1.5x the ACTIVE target's step " << stepA
            << " — the device read did not follow the active target";
        EXPECT_LE(controlDev, 1.5f * stepA)
            << "supplying the active target's own format must agree with the ambient read";
    }

    // A destroyed target's handle must stop answering: stale handles report
    // Unknown, never whatever target is active.
    EXPECT_TRUE(dev->DestroyWindowTarget(targetB));
    EXPECT_EQ(dev->GetWindowTargetSwapchainFormat(targetB), TextureFormat::Unknown)
        << "a destroyed target's handle still reports a format";
    EXPECT_TRUE(dev->DestroyWindowTarget(targetA));
    dev.reset();

    glfwDestroyWindow(windowB);
    glfwDestroyWindow(windowA);
    glfwTerminate();

    if (!diverged)
    {
        GTEST_SKIP() << "window targets resolved to " << ToString(formatA) << " and "
                     << ToString(formatB) << " (steps " << stepA << ", " << stepB
                     << "); this agent cannot produce two different presented steps, so the "
                        "consumption arms went unexercised";
    }
#endif
}
