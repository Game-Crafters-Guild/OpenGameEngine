// FinalizeQuantizer contract guard (FinalizeContract.h).
//
// `FinalizeInputSpace` says what the pass's INPUT holds; `FinalizeQuantizer`
// says which quantizer step its two filters — the deband gate and the TPDF
// dither — are sized to. The pass can see its own destination format but not
// where in a chain it sits, so both are stated by the caller, and the two
// non-default values are the ones the Player's SDR HUD chain is built on:
//
//   None      — the terminal transfer. The source already holds destination
//               code values, so the pass must move them and filter NOTHING.
//               This is what makes HUD pixels reach the screen byte-clean.
//   Presented — the world finalize into the FP16 composite. A float
//               destination has no quantizer of its own, so sizing to it
//               would size the dither to zero; the filters are sized to the
//               swapchain step the bytes will actually land on instead.
//
// Both values are structurally one `switch` away from `Destination`, and every
// other suite in the tree passes `Destination` — so without this file a change
// that collapsed `None` into `Destination` would keep every test green while
// silently re-graining the Player's HUD. Each test carries its own control arm
// measured on the SAME input, so a green result cannot come from an input the
// filters were never going to touch.
//
// Own process: the deband/dither environment kill switches latch into
// function-local statics on first read, so this file pins BOTH filters ON —
// the strongest form of the assertion, since `None` then has to defeat two
// filters that the environment is actively demanding.

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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
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

// Both filters demanded ON, so a pass that respects `None` has to turn off two
// things the environment is asking for.
void PinEnvironment()
{
    SetEnv("GE_HEADLESS_TEST", "1");
    SetEnv("GE_OUTPUT_DITHER", "1");
    SetEnv("GE_DEBAND", "1");
    SetEnv("GE_DEBAND_THRESHOLD", "");
    SetEnv("GE_DEBAND_RADIUS", "");

#ifdef RENDERING_SHADER_OUTPUT_DIR
    Utils::SetShaderPathResolver(
        +[](const std::filesystem::path& rel) -> std::filesystem::path
        { return std::filesystem::path(RENDERING_SHADER_OUTPUT_DIR).parent_path() / rel; });
#endif
}

std::unique_ptr<IDevice> MakeDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

// Reference of the shader's LinearToSRGB (encode_srgb.frag) on one channel.
float LinearToSrgbRef(float c)
{
    if (c <= 0.0031308f)
        return 12.92f * c;
    return 1.055f * std::pow(std::max(c, 1e-6f), 1.0f / 2.4f) - 0.055f;
}

// Uploads `pixels` (RGBA32F, kW x kH) into a sampled texture rested at
// ShaderResource, ready for the pass to read.
TextureHandle MakeSourceTexture(IDevice* device, uint32_t w, uint32_t h,
                               const std::vector<float>& pixels, const char* name)
{
    TextureDesc srcDesc{};
    srcDesc.width = w;
    srcDesc.height = h;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = name;
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    if (!srcTex.IsValid())
        return srcTex;

    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "QuantizerTest.Upload");
    if (!upload.IsValid())
        return {};
    device->UpdateBuffer(upload, 0, srcBytes, pixels.data());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    if (!cl)
        return {};
    cl->Begin();
    cl->CopyBufferToTextureSubresource(upload, srcTex, 0, 0, w, h, 0,
                                       static_cast<size_t>(w) * 4 * sizeof(float));
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(srcTex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();
    return srcTex;
}

// Runs one finalize with the given quantizer and returns the destination's
// bytes. `dstFormat` picks the arm under test; `bytesPerPixel` sizes the
// readback. `presentedFormat` states the swapchain format the Presented arm
// sizes to; unset leaves the pass reading the device, which is what every
// pre-existing caller here does.
std::vector<uint8_t> RunFinalize(IDevice* device, TextureHandle srcTex, uint32_t w, uint32_t h,
                                 TextureFormat srcFormat, TextureFormat dstFormat,
                                 size_t bytesPerPixel, Passes::FinalizeInputSpace inputSpace,
                                 Passes::FinalizeQuantizer quantizer, float debandLsb,
                                 const char* passName,
                                 std::optional<TextureFormat> presentedFormat = std::nullopt)
{
    const size_t outBytes = static_cast<size_t>(w) * h * bytesPerPixel;
    const BufferHandle readback = device->CreateReadbackBuffer(outBytes, "QuantizerTest.Readback");
    if (!readback.IsValid())
        return {};

    FramePools pools(device);
    RG::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src =
        frame.ImportExternalTexture("QuantizerTest.Src", srcTex, ResourceState::ShaderResource,
                                    srcFormat);
    if (!src.IsValid())
        return {};

    TextureDesc dstDesc{};
    dstDesc.width = w;
    dstDesc.height = h;
    dstDesc.depth = 1;
    dstDesc.mipLevels = 1;
    dstDesc.arrayLayers = 1;
    dstDesc.sampleCount = 1;
    dstDesc.format = static_cast<uint32_t>(dstFormat);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dstDesc.debugName = "QuantizerTest.Dst";
    const RG::RGTexture dst = frame.CreateTexture("QuantizerTest.Dst", dstDesc);
    if (!dst.IsValid())
        return {};

    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = inputSpace,
         .Quantizer = quantizer,
         .VolumeDebandThresholdLsb = debandLsb,
         .PresentedFormat = presentedFormat},
        passName);
    if (!encodePass.IsValid())
        return {};

    frame.AddPass(
        "QuantizerTest.Readback", PassPhase::kFinalize,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(dst, RG::RGTextureRead::CopySrc);
            p.PreventCulling(); // consumed by the CPU, not the graph
        },
        [dst, readback, w, h](RG::RGContext& ctx)
        { ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, readback, w, h); });

    frame.Execute();
    device->WaitForIdle();

    const void* mapped = device->MapBuffer(readback);
    if (!mapped)
        return {};
    std::vector<uint8_t> out(outBytes);
    std::copy_n(static_cast<const uint8_t*>(mapped), outBytes, out.begin());
    device->UnmapBuffer(readback);
    return out;
}

// IEEE half -> float, so the F16 intermediate can be read as the numbers the
// terminal pass actually saw rather than as a model of them.
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0)
    {
        if (mant != 0)
        {
            int e = -1;
            uint32_t m = mant;
            do
            {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            bits = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
        else
        {
            bits = sign;
        }
    }
    else if (exp == 0x1Fu)
    {
        bits = sign | 0x7F800000u | (mant << 13);
    }
    else
    {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// The editor's SDR chain, both stages in ONE frame: a world view's finalize into
// the F16 image the UI composites, then the terminal pass that moves that image
// to the presented surface. Returns BOTH ends raw, so the terminal can be checked
// against its own input instead of against a reconstruction of it.
struct ChainResult
{
    std::vector<uint16_t> Intermediate; // RGBA16F texels, 4 per pixel
    std::vector<uint8_t> Destination;
};

ChainResult RunEditorChain(IDevice* device, TextureHandle srcTex, uint32_t w, uint32_t h,
                           TextureFormat dstFormat, size_t dstBytesPerPixel,
                           Passes::FinalizeQuantizer terminalQuantizer, float debandLsb)
{
    const size_t midBytes = static_cast<size_t>(w) * h * 4 * sizeof(uint16_t);
    const size_t dstBytes = static_cast<size_t>(w) * h * dstBytesPerPixel;
    const BufferHandle midReadback = device->CreateReadbackBuffer(midBytes, "Chain.MidReadback");
    const BufferHandle dstReadback = device->CreateReadbackBuffer(dstBytes, "Chain.DstReadback");
    if (!midReadback.IsValid() || !dstReadback.IsValid())
        return {};

    FramePools pools(device);
    RG::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "Chain.Src", srcTex, ResourceState::ShaderResource, TextureFormat::R32G32B32A32_FLOAT);
    if (!src.IsValid())
        return {};

    TextureDesc midDesc{};
    midDesc.width = w;
    midDesc.height = h;
    midDesc.depth = 1;
    midDesc.mipLevels = 1;
    midDesc.arrayLayers = 1;
    midDesc.sampleCount = 1;
    midDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    midDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                    static_cast<uint32_t>(TextureUsage::ShaderResource) |
                    static_cast<uint32_t>(TextureUsage::TransferSrc);
    midDesc.debugName = "Chain.ViewFinalized";
    const RG::RGTexture mid = frame.CreateTexture("Chain.ViewFinalized", midDesc);
    if (!mid.IsValid())
        return {};

    TextureDesc dstDesc = midDesc;
    dstDesc.format = static_cast<uint32_t>(dstFormat);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                    static_cast<uint32_t>(TextureUsage::TransferSrc);
    dstDesc.debugName = "Chain.Presented";
    const RG::RGTexture dst = frame.CreateTexture("Chain.Presented", dstDesc);
    if (!dst.IsValid())
        return {};

    // Stage 1 — the view's own finalize. Linear in, filters sized to the
    // PRESENTED surface even though this destination is a float intermediate.
    if (!Passes::AddSRGBEncodePassRG(frame, src, mid,
                                     {.InputSpace = Passes::FinalizeInputSpace::Linear,
                                      .Quantizer = Passes::FinalizeQuantizer::Presented,
                                      .VolumeDebandThresholdLsb = debandLsb},
                                     "Chain.ViewFinalize")
             .IsValid())
        return {};

    // Stage 2 — the terminal. The composite already carries the curve and the
    // noise, so the editor names None here.
    if (!Passes::AddSRGBEncodePassRG(frame, mid, dst,
                                     {.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb,
                                      .Quantizer = terminalQuantizer,
                                      .VolumeDebandThresholdLsb = debandLsb},
                                     "Chain.Terminal")
             .IsValid())
        return {};

    frame.AddPass(
        "Chain.Readback", PassPhase::kFinalize,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(mid, RG::RGTextureRead::CopySrc);
            p.Read(dst, RG::RGTextureRead::CopySrc);
            p.PreventCulling();
        },
        [mid, dst, midReadback, dstReadback, w, h](RG::RGContext& ctx)
        {
            ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(mid), 0, 0, midReadback, w, h);
            ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(dst), 0, 0, dstReadback, w, h);
        });

    frame.Execute();
    device->WaitForIdle();

    ChainResult out{};
    if (const void* mapped = device->MapBuffer(midReadback))
    {
        out.Intermediate.resize(static_cast<size_t>(w) * h * 4);
        std::memcpy(out.Intermediate.data(), mapped, midBytes);
        device->UnmapBuffer(midReadback);
    }
    if (const void* mapped = device->MapBuffer(dstReadback))
    {
        out.Destination.resize(dstBytes);
        std::memcpy(out.Destination.data(), mapped, dstBytes);
        device->UnmapBuffer(dstReadback);
    }
    if (out.Intermediate.empty() || out.Destination.empty())
        return {};
    return out;
}

// A shallow linear ramp: encodes across ~26 sRGB levels, so a 1-LSB dither is
// unmistakable and a staircase is measurable.
constexpr uint32_t kRampW = 512;
constexpr uint32_t kRampH = 64;
constexpr float kRampMaxLinear = 0.01f;

float RampLinearAt(uint32_t x)
{
    return kRampMaxLinear * static_cast<float>(x) / static_cast<float>(kRampW - 1);
}

std::vector<float> MakeRampPixels()
{
    std::vector<float> pixels(static_cast<size_t>(kRampW) * kRampH * 4);
    for (uint32_t y = 0; y < kRampH; ++y)
    {
        for (uint32_t x = 0; x < kRampW; ++x)
        {
            const float v = RampLinearAt(x);
            float* px = &pixels[(static_cast<size_t>(y) * kRampW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    return pixels;
}

// The staircase the Player's composite hands its terminal pass: values already
// sitting exactly on 8-bit sRGB code levels, stepping 6 counts every 16 px.
// Both filters have something to do here — the dither would move bytes off
// these levels, and a 6-LSB step is inside the reviewed deband gate.
constexpr uint32_t kStairW = 384;
constexpr uint32_t kStairH = 64;
constexpr int kStairBaseCode = 64;
constexpr int kStairStepCode = 6;
constexpr int kStairPlateauPx = 16;
constexpr float kDebandGateLsb = 6.0f;

int StairCodeAt(uint32_t x)
{
    return kStairBaseCode + kStairStepCode * static_cast<int>(x / kStairPlateauPx);
}

} // namespace

// `None` must transfer the bytes and filter nothing. The control arm proves the
// input is one both filters demonstrably act on, so byte-exactness under `None`
// cannot be an artefact of a quiet input.
TEST(SrgbEncodeQuantizer, NoneTransfersBytesWithNeitherFilter)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    // Already-ENCODED source: exact 8-bit code levels in a float texture, which
    // is what an FP16 composite holds on the HUD chain.
    std::vector<float> pixels(static_cast<size_t>(kStairW) * kStairH * 4);
    for (uint32_t y = 0; y < kStairH; ++y)
    {
        for (uint32_t x = 0; x < kStairW; ++x)
        {
            const float v = static_cast<float>(StairCodeAt(x)) / 255.0f;
            float* px = &pixels[(static_cast<size_t>(y) * kStairW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kStairW, kStairH, pixels, "QuantizerTest.EncodedStaircase");
    ASSERT_TRUE(srcTex.IsValid());

    // Arm under test: EncodedSrgb + None into a UNORM destination — the
    // terminal transfer (shader arm 5, raw store).
    const std::vector<uint8_t> none = RunFinalize(
        device.get(), srcTex, kStairW, kStairH, TextureFormat::R32G32B32A32_FLOAT,
        TextureFormat::RGBA8_UNORM, 4, Passes::FinalizeInputSpace::EncodedSrgb,
        Passes::FinalizeQuantizer::None, kDebandGateLsb, "QuantizerTest.None");
    ASSERT_FALSE(none.empty())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    // CONTROL: identical input and destination, quantizer Destination. This is
    // the same pass with the same arm, differing only in the filter sizing, so
    // any difference it shows is exactly what `None` had to suppress.
    const std::vector<uint8_t> destination = RunFinalize(
        device.get(), srcTex, kStairW, kStairH, TextureFormat::R32G32B32A32_FLOAT,
        TextureFormat::RGBA8_UNORM, 4, Passes::FinalizeInputSpace::EncodedSrgb,
        Passes::FinalizeQuantizer::Destination, kDebandGateLsb, "QuantizerTest.Destination");
    ASSERT_FALSE(destination.empty());

    int noneMaxDev = 0;
    int controlMaxDev = 0;
    size_t controlMovedPixels = 0;
    for (uint32_t y = 0; y < kStairH; ++y)
    {
        for (uint32_t x = 0; x < kStairW; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * kStairW + x) * 4;
            const int expected = StairCodeAt(x);
            noneMaxDev = std::max(noneMaxDev, std::abs(static_cast<int>(none[i]) - expected));
            const int cdev = std::abs(static_cast<int>(destination[i]) - expected);
            controlMaxDev = std::max(controlMaxDev, cdev);
            if (cdev != 0)
                ++controlMovedPixels;
        }
    }

    // The control must actually move bytes, or the arm under test proves nothing.
    EXPECT_GT(controlMaxDev, 0)
        << "control arm left every byte untouched — the filters are inert on this input, so "
           "the None arm below is vacuous";
    EXPECT_GT(controlMovedPixels, static_cast<size_t>(kStairW) * kStairH / 100)
        << "control arm moved only " << controlMovedPixels << " pixels";

    // The contract: not one byte moves.
    EXPECT_EQ(noneMaxDev, 0)
        << "FinalizeQuantizer::None moved a byte by " << noneMaxDev
        << " LSB — the terminal transfer is filtering, so HUD pixels are not byte-clean";
}

// `Presented` must size the filters to the swapchain step even though the
// destination is a float surface with no step of its own. The control arm is
// the same write under `Destination`, which correctly dithers by nothing.
TEST(SrgbEncodeQuantizer, PresentedSizesFiltersPastAFloatDestination)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    constexpr uint32_t kW = 512;
    constexpr uint32_t kH = 64;
    // Shallow linear ramp, as in the dither guard: encodes across ~26 sRGB
    // levels, so a 1-LSB dither is unmistakable against the ideal curve.
    constexpr float kMaxLinear = 0.01f;

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
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kW, kH, pixels, "QuantizerTest.LinearRamp");
    ASSERT_TRUE(srcTex.IsValid());

    // A float destination is the Player's composite: no quantizer of its own.
    // Deband stays out of it (0) so this measures the dither sizing alone.
    const std::vector<uint8_t> presentedBytes = RunFinalize(
        device.get(), srcTex, kW, kH, TextureFormat::R32G32B32A32_FLOAT,
        TextureFormat::R32G32B32A32_FLOAT, 16, Passes::FinalizeInputSpace::Linear,
        Passes::FinalizeQuantizer::Presented, 0.0f, "QuantizerTest.Presented");
    ASSERT_FALSE(presentedBytes.empty())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/";

    // CONTROL: same float destination under Destination. Its step is 0, so the
    // encode must land on the ideal curve with no noise at all.
    const std::vector<uint8_t> destinationBytes = RunFinalize(
        device.get(), srcTex, kW, kH, TextureFormat::R32G32B32A32_FLOAT,
        TextureFormat::R32G32B32A32_FLOAT, 16, Passes::FinalizeInputSpace::Linear,
        Passes::FinalizeQuantizer::Destination, 0.0f, "QuantizerTest.FloatDestination");
    ASSERT_FALSE(destinationBytes.empty());

    const auto* presented = reinterpret_cast<const float*>(presentedBytes.data());
    const auto* destination = reinterpret_cast<const float*>(destinationBytes.data());

    float presentedMaxDev = 0.0f;
    float destinationMaxDev = 0.0f;
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
            const float lin = kMaxLinear * static_cast<float>(x) / static_cast<float>(kW - 1);
            const float ideal = LinearToSrgbRef(lin);
            presentedMaxDev = std::max(presentedMaxDev, std::abs(presented[i] - ideal));
            destinationMaxDev = std::max(destinationMaxDev, std::abs(destination[i] - ideal));
        }
    }

    // The step the filters are supposed to be sized to. Read from the format
    // table (pinned row by row in SrgbEncodeDitherTests) rather than assumed,
    // so this test states an AMPLITUDE BOUND, not an identity.
    const float presentedLsb =
        Passes::EncodeDitherLsbForFormat(device->GetSwapchainTextureFormat());
    ASSERT_GT(presentedLsb, 0.0f) << "no presented step available to size against";

    // CONTROL: a float destination has no step, so Destination dithers by
    // nothing and the write is the bare transfer curve.
    EXPECT_LT(destinationMaxDev, presentedLsb * 0.25f)
        << "FinalizeQuantizer::Destination dithered a float destination (max deviation "
        << destinationMaxDev << ") — a surface with no quantizer must take no filter";

    // The contract: Presented dithers anyway, at the presented step.
    EXPECT_GT(presentedMaxDev, presentedLsb * 0.25f)
        << "FinalizeQuantizer::Presented left the float destination undithered (max deviation "
        << presentedMaxDev << ") — the composite would carry the swapchain's banding "
           "to a pass that can no longer tell world pixels from chrome";
    EXPECT_LT(presentedMaxDev, presentedLsb * 1.5f)
        << "Presented dither amplitude " << presentedMaxDev << " exceeds 1.5x the presented step "
        << presentedLsb << " — sized to the wrong quantizer";
}


// The editor's SDR chain, end to end and byte-for-byte: a world view finalizes
// into the F16 image the UI composites, and the terminal pass moves it to the
// presented surface. The contract under test is the second stage — every
// destination code must be exactly the rounding of the intermediate value that
// produced it, with nothing added.
//
// This is measured against the intermediate READ BACK FROM THE GPU rather than
// against a CPU model of the encode, so it cannot pass by two models agreeing
// with each other while the GPU does something else. The control arm re-runs the
// identical chain with the terminal at Destination: it must move bytes, which is
// what makes the None arm's exactness a result instead of a tautology.
TEST(SrgbEncodeQuantizer, TheTerminalTransferAddsNothingToAFinalizedComposite)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    const std::vector<float> pixels = MakeRampPixels();
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kRampW, kRampH, pixels, "Chain.LinearRamp");
    ASSERT_TRUE(srcTex.IsValid());

    // 8-bit UNORM destination: universally renderable, so this arm always runs.
    // The 10-bit arm below covers the depth this machine actually presents at.
    const ChainResult none =
        RunEditorChain(device.get(), srcTex, kRampW, kRampH, TextureFormat::RGBA8_UNORM, 4,
                       Passes::FinalizeQuantizer::None, kDebandGateLsb);
    ASSERT_FALSE(none.Destination.empty())
        << "the chain declared nothing — encode_srgb.shaderpkg not staged under the build "
           "root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    const ChainResult control =
        RunEditorChain(device.get(), srcTex, kRampW, kRampH, TextureFormat::RGBA8_UNORM, 4,
                       Passes::FinalizeQuantizer::Destination, kDebandGateLsb);
    ASSERT_FALSE(control.Destination.empty());

    // Two discriminators, neither of which depends on a rounding rule.
    //
    // 1. The intermediates must be BYTE-IDENTICAL between the two arms. The view
    //    finalize is the same call in both, and its dither is a screen-space hash,
    //    so this establishes that the arms differ at the terminal and NOWHERE
    //    else — without it, a difference downstream could be the finalize's.
    // 2. BRACKETING: each destination code must be one of the two codes that
    //    bracket the composite value (floor or ceil). That is exactly "quantized,
    //    not moved": it is blind to which way the hardware breaks ties and to the
    //    sub-quantum error a float sampler is allowed, and a value displaced
    //    before the store escapes the bracket. The mean-displacement bound below
    //    turns that from a pass/fail into a measured separation, because a 1-LSB
    //    TPDF only escapes the bracket on a minority of pixels.
    ASSERT_EQ(none.Intermediate.size(), control.Intermediate.size());
    EXPECT_EQ(std::memcmp(none.Intermediate.data(), control.Intermediate.data(),
                          none.Intermediate.size() * sizeof(uint16_t)),
              0)
        << "the two arms' finalized composites differ — the arms are not isolated to the "
           "terminal, so nothing below attributes cleanly";

    size_t noneEscapes = 0;
    size_t controlEscapes = 0;
    double noneDisplacementSum = 0.0;
    double controlDisplacementSum = 0.0;
    float noneWorst = 0.0f;
    for (uint32_t y = 0; y < kRampH; ++y)
    {
        for (uint32_t x = 0; x < kRampW; ++x)
        {
            const size_t px = static_cast<size_t>(y) * kRampW + x;
            const float mid = std::clamp(HalfToFloat(none.Intermediate[px * 4]), 0.0f, 1.0f);
            const float v = mid * 255.0f;
            const float lo = std::floor(v);
            const float hi = std::ceil(v);

            const float got = static_cast<float>(none.Destination[px * 4]);
            if (got < lo - 0.5f || got > hi + 0.5f)
                ++noneEscapes;
            noneDisplacementSum += std::abs(got - v);
            noneWorst = std::max(noneWorst, std::abs(got - v));

            const float cgot = static_cast<float>(control.Destination[px * 4]);
            if (cgot < lo - 0.5f || cgot > hi + 0.5f)
                ++controlEscapes;
            controlDisplacementSum += std::abs(cgot - v);
        }
    }

    const size_t total = static_cast<size_t>(kRampW) * kRampH;
    const double noneMeanDisplacement = noneDisplacementSum / static_cast<double>(total);
    const double controlMeanDisplacement = controlDisplacementSum / static_cast<double>(total);

    // The control must actually move values, or the None arm proves nothing. A
    // pure quantization averages 0.25 LSB of displacement (mean |uniform(-.5,.5)|);
    // a 1-LSB TPDF added before the store pushes that toward 0.45.
    EXPECT_GT(controlMeanDisplacement, 0.35)
        << "the Destination control displaced by only " << controlMeanDisplacement
        << " LSB on average — its filters are inert on this input, so the None arm is vacuous";
    EXPECT_GT(controlEscapes, total / 50)
        << "the control escaped the quantization bracket on only " << controlEscapes << "/" << total
        << " pixels";

    // The contract: the terminal quantizes and moves nothing.
    EXPECT_EQ(noneEscapes, 0u)
        << noneEscapes << "/" << total
        << " destination codes fall outside the two codes bracketing their composite value "
           "(worst displacement "
        << noneWorst << " LSB) — the terminal is still filtering, so chrome is not byte-clean";
    EXPECT_LT(noneMeanDisplacement, 0.30)
        << "mean displacement " << noneMeanDisplacement
        << " LSB exceeds what a pure quantization can produce (0.25) — the terminal is adding "
           "noise; the control measured " << controlMeanDisplacement;
}

// The same contract at the depth this machine actually presents at. Skipped
// rather than faked when the device cannot render 10-bit offscreen, and the skip
// says so — a silent pass here would hide the whole point, which is that the
// chain is correct at 1/1023 and not only at 1/255.
TEST(SrgbEncodeQuantizer, TheTerminalTransferAddsNothingAtTenBitDepth)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    const uint32_t tenBitUsage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                                 static_cast<uint32_t>(TextureUsage::TransferSrc);
    if (!device->IsTextureFormatSupported(TextureFormat::RGB10A2_UNORM, tenBitUsage))
        GTEST_SKIP() << "RGB10A2_UNORM is not renderable offscreen on this device";

    const std::vector<float> pixels = MakeRampPixels();
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kRampW, kRampH, pixels, "Chain.LinearRamp10");
    ASSERT_TRUE(srcTex.IsValid());

    const ChainResult none =
        RunEditorChain(device.get(), srcTex, kRampW, kRampH, TextureFormat::RGB10A2_UNORM, 4,
                       Passes::FinalizeQuantizer::None, kDebandGateLsb);
    ASSERT_FALSE(none.Destination.empty());

    size_t escapes = 0;
    double displacementSum = 0.0;
    float worst = 0.0f;
    for (uint32_t y = 0; y < kRampH; ++y)
    {
        for (uint32_t x = 0; x < kRampW; ++x)
        {
            const size_t px = static_cast<size_t>(y) * kRampW + x;
            uint32_t packed = 0;
            std::memcpy(&packed, &none.Destination[px * 4], sizeof(packed));
            // A2B10G10R10_UNORM_PACK32: R in the low 10 bits.
            const float got = static_cast<float>(packed & 0x3FFu);
            const float mid = std::clamp(HalfToFloat(none.Intermediate[px * 4]), 0.0f, 1.0f);
            const float v = mid * 1023.0f;
            if (got < std::floor(v) - 0.5f || got > std::ceil(v) + 0.5f)
                ++escapes;
            displacementSum += std::abs(got - v);
            worst = std::max(worst, std::abs(got - v));
        }
    }
    const size_t total = static_cast<size_t>(kRampW) * kRampH;
    EXPECT_EQ(escapes, 0u)
        << escapes << " of " << total
        << " ten-bit codes fall outside the two codes bracketing their composite value (worst "
           "displacement "
        << worst << " LSB) — at 10 bits the transfer is not clean";
    EXPECT_LT(displacementSum / static_cast<double>(total), 0.30)
        << "mean ten-bit displacement " << (displacementSum / static_cast<double>(total))
        << " LSB exceeds what a pure quantization can produce";
}

// ── SelectTransferQuantizer: the shared requantize decision ──
//
// Both hosts' movie encodes AND both hosts' terminal transfers route through
// this one function (GameViewMovieCapture, PlayerApplication twice,
// EditorApplication), so a divergence here is a recording that stops matching
// the screen, or a terminal that drifts from the movies — the class of bug
// hand-copied decision blocks invite. The terminals call it with source step ==
// destination format, which is why the same-step rows below are pinned in both
// directions. It is pure and device-free, so these pins run wherever the suite
// does.
//
// The discriminating cases are the ones where the two step depths differ. A
// same-depth pair cannot tell `None` from a mistaken `Destination` on its own,
// which is why the 10-bit-source and 10-bit-destination rows are both here and
// point OPPOSITE ways: any comparison error flips exactly one of them.

// A linear source means this transfer owns the OETF and the step, whatever the
// formats say. This is the arm every non-flipped host runs.
TEST(SelectTransferQuantizer, LinearSourceAlwaysOwnsTheStep)
{
    EXPECT_EQ(Passes::SelectTransferQuantizer(false, TextureFormat::BGRA8_UNORM,
                                              TextureFormat::BGRA8_UNORM),
              Passes::FinalizeQuantizer::Destination);
    EXPECT_EQ(Passes::SelectTransferQuantizer(false, TextureFormat::RGB10A2_UNORM,
                                              TextureFormat::BGRA8_UNORM),
              Passes::FinalizeQuantizer::Destination);
    // Even where an encoded source would answer None, a linear one must not:
    // there are no code values to move yet.
    EXPECT_EQ(Passes::SelectTransferQuantizer(false, TextureFormat::BGRA8_UNORM,
                                              TextureFormat::RGB10A2_UNORM),
              Passes::FinalizeQuantizer::Destination);
}

// An encoded source at the SAME step as the destination is the common
// recording: the bytes already sit where they are going, so the transfer must
// move them and filter nothing.
TEST(SelectTransferQuantizer, EncodedSourceAtTheSameStepIsMovedNotRequantized)
{
    EXPECT_EQ(Passes::SelectTransferQuantizer(true, TextureFormat::BGRA8_UNORM,
                                              TextureFormat::BGRA8_UNORM),
              Passes::FinalizeQuantizer::None)
        << "an 8-bit screen recorded to 8-bit must not be requantized — the filters would "
           "re-grain bytes that already sit on the destination step";
    EXPECT_EQ(Passes::SelectTransferQuantizer(true, TextureFormat::RGB10A2_UNORM,
                                              TextureFormat::RGB10A2_UNORM),
              Passes::FinalizeQuantizer::None);
}

// The one case that needs the step back: a 10-bit screen recorded to 8-bit. The
// source's code values are finer than the destination can hold, so this
// transfer owns the rounding — and therefore the deband gate and the dither
// sized to the SAME step.
TEST(SelectTransferQuantizer, ACoarserDestinationTakesTheStepBack)
{
    EXPECT_EQ(Passes::SelectTransferQuantizer(true, TextureFormat::RGB10A2_UNORM,
                                              TextureFormat::BGRA8_UNORM),
              Passes::FinalizeQuantizer::Destination)
        << "a 10-bit screen recorded to 8-bit must requantize — otherwise the movie carries "
           "1/1023 detail into a 1/255 store with no dither and no deband";
}

// The mirror, and the reason the row above cannot be a constant: a destination
// FINER than the source needs nothing done. Rounding 8-bit code values into a
// 10-bit target is exact, so requantizing here would only add filters that have
// nothing to break.
TEST(SelectTransferQuantizer, AFinerDestinationStillMovesTheBytes)
{
    EXPECT_EQ(Passes::SelectTransferQuantizer(true, TextureFormat::BGRA8_UNORM,
                                              TextureFormat::RGB10A2_UNORM),
              Passes::FinalizeQuantizer::None)
        << "an 8-bit source into a 10-bit target is an exact widening — nothing to requantize";
}

// A float source-step format reports no quantizer at all (the scRGB/FP16 row of
// EncodeDitherLsbForFormat). "No step" is not "an infinitely fine step": the
// destination's step is then the only one in play, so the transfer must own it.
TEST(SelectTransferQuantizer, AStepLessSourceHandsTheStepToTheDestination)
{
    ASSERT_EQ(Passes::EncodeDitherLsbForFormat(TextureFormat::R16G16B16A16_FLOAT), 0.0f)
        << "premise: the float row reports no quantizer";
    EXPECT_EQ(Passes::SelectTransferQuantizer(true, TextureFormat::R16G16B16A16_FLOAT,
                                              TextureFormat::BGRA8_UNORM),
              Passes::FinalizeQuantizer::Destination);
}

// The amplitude the view finalize writes must follow the PRESENTED surface's
// depth, and the assertion is written so that it can FAIL: on a swapchain deeper
// than 8 bits it demands noise visibly smaller than an 8-bit LSB, which is
// exactly what a pass that had sized to 1/255 would violate. On an 8-bit
// swapchain the two hypotheses coincide and the test says so rather than
// pretending to have discriminated.
TEST(SrgbEncodeQuantizer, PresentedAmplitudeFollowsTheSwapchainDepth)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    const std::vector<float> pixels = MakeRampPixels();
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kRampW, kRampH, pixels, "Chain.AmplitudeRamp");
    ASSERT_TRUE(srcTex.IsValid());

    // Deband off (0) so this measures the dither's amplitude alone.
    const ChainResult chain =
        RunEditorChain(device.get(), srcTex, kRampW, kRampH, TextureFormat::RGBA8_UNORM, 4,
                       Passes::FinalizeQuantizer::None, 0.0f);
    ASSERT_FALSE(chain.Intermediate.empty());

    const TextureFormat swapFormat = device->GetSwapchainTextureFormat();
    const float presentedLsb = Passes::EncodeDitherLsbForFormat(swapFormat);
    ASSERT_GT(presentedLsb, 0.0f) << "no presented step available to size against";
    constexpr float kEightBitLsb = 1.0f / 255.0f;

    float maxDev = 0.0f;
    for (uint32_t y = 0; y < kRampH; ++y)
    {
        for (uint32_t x = 0; x < kRampW; ++x)
        {
            const size_t px = static_cast<size_t>(y) * kRampW + x;
            const float got = HalfToFloat(chain.Intermediate[px * 4]);
            maxDev = std::max(maxDev, std::abs(got - LinearToSrgbRef(RampLinearAt(x))));
        }
    }

    EXPECT_GT(maxDev, presentedLsb * 0.25f)
        << "the view finalize left its output undithered (max deviation " << maxDev << ")";
    EXPECT_LT(maxDev, presentedLsb * 1.5f)
        << "dither amplitude " << maxDev << " exceeds 1.5x the presented step " << presentedLsb
        << " — sized to the wrong quantizer";

    if (presentedLsb < kEightBitLsb * 0.9f)
    {
        EXPECT_LT(maxDev, kEightBitLsb * 0.5f)
            << "this swapchain quantizes at " << presentedLsb << " (deeper than 8 bits) yet the "
            << "amplitude " << maxDev << " is 8-bit-sized — the format table row was not used";
    }
    else
    {
        GTEST_LOG_(INFO) << "swapchain step is " << presentedLsb
                         << " (8-bit): the 8-vs-10-bit discrimination is vacuous on this "
                            "machine and only the bounds above were exercised";
    }
}

// The Presented arm must size to the format it is TOLD the presented surface
// has, and this is the test that can tell that apart from sizing to a constant.
//
// Every other Presented assertion in this file is measured against
// EncodeDitherLsbForFormat(device->GetSwapchainTextureFormat()) — the exact
// expression the pass evaluates — so a mutation to which format the arm reads
// moves the expectation with it and cancels. Worse, this suite runs headless:
// with no swapchain the device reports Unknown, the format matrix reads Unknown
// as the 8-bit step, and the arm yields 1/255 — numerically identical to what an
// 8-bit Destination yields. Both hypotheses coincide, which is why
// PresentedAmplitudeFollowsTheSwapchainDepth logs its 8-vs-10-bit discrimination
// as vacuous rather than asserting it.
//
// The coincidence is destination-specific and does not generalise: against a
// FLOAT destination the pair is 1/255 against 0, which a measurement separates
// trivially and PresentedSizesFiltersPastAFloatDestination asserts today. Read
// "the arms are indistinguishable headless" as scoped to an 8-bit destination,
// of which there is none on a Presented path in tree.
//
// `presentedFormat` breaks the coincidence: it supplies the depth a windowed
// host would have had, so the two arms below MUST separate. The expectations are
// hardcoded 1/1023 and 1/255 and never call the format matrix — deriving them
// from EncodeDitherLsbForFormat would rebuild the same cancellation one level
// down.
TEST(SrgbEncodeQuantizer, PresentedSizesToTheSuppliedPresentedFormat)
{
    PinEnvironment();
    auto device = MakeDevice();
    if (!device)
        GTEST_SKIP() << "Vulkan device unavailable on this machine";

    // The step each arm must land on, stated as constants rather than resolved
    // through the code under test.
    constexpr float kTenBitLsb = 1.0f / 1023.0f;
    constexpr float kEightBitLsb = 1.0f / 255.0f;

    const std::vector<float> pixels = MakeRampPixels();
    const TextureHandle srcTex =
        MakeSourceTexture(device.get(), kRampW, kRampH, pixels, "QuantizerTest.PresentedFormatRamp");
    ASSERT_TRUE(srcTex.IsValid());

    // A FLOAT destination throughout: it has no step of its own, so what is
    // measured is the Presented sizing alone and nothing the destination's own
    // rounding contributed. A UNORM dst would round a 1/1023 dither away and the
    // two arms would converge for a reason that has nothing to do with the arm.
    // Deband off (0) so this measures the dither amplitude alone.
    auto runWithPresented = [&](std::optional<TextureFormat> presentedFormat, const char* name)
    {
        return RunFinalize(device.get(), srcTex, kRampW, kRampH,
                           TextureFormat::R32G32B32A32_FLOAT, TextureFormat::R32G32B32A32_FLOAT, 16,
                           Passes::FinalizeInputSpace::Linear, Passes::FinalizeQuantizer::Presented,
                           0.0f, name, presentedFormat);
    };

    const std::vector<uint8_t> tenBitBytes =
        runWithPresented(TextureFormat::RGB10A2_UNORM, "QuantizerTest.Presented10");
    ASSERT_FALSE(tenBitBytes.empty())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/";
    const std::vector<uint8_t> eightBitBytes =
        runWithPresented(TextureFormat::RGBA8_UNORM, "QuantizerTest.Presented8");
    ASSERT_FALSE(eightBitBytes.empty());
    const std::vector<uint8_t> deviceBytes = runWithPresented(std::nullopt, "QuantizerTest.PresentedDevice");
    ASSERT_FALSE(deviceBytes.empty());

    auto maxDeviationFromIdeal = [](const std::vector<uint8_t>& bytes)
    {
        const auto* px = reinterpret_cast<const float*>(bytes.data());
        float maxDev = 0.0f;
        for (uint32_t y = 0; y < kRampH; ++y)
        {
            for (uint32_t x = 0; x < kRampW; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * kRampW + x) * 4;
                maxDev = std::max(maxDev, std::abs(px[i] - LinearToSrgbRef(RampLinearAt(x))));
            }
        }
        return maxDev;
    };

    const float tenBitMaxDev = maxDeviationFromIdeal(tenBitBytes);
    const float eightBitMaxDev = maxDeviationFromIdeal(eightBitBytes);

    // THE discrimination: a 10-bit presented step must produce noise visibly
    // smaller than an 8-bit LSB. An arm that ignored presentedFormat and read
    // the device would measure ~1/255 here and fail by 2x.
    EXPECT_LT(tenBitMaxDev, kEightBitLsb * 0.5f)
        << "presentedFormat named a 10-bit surface (step " << kTenBitLsb << ") yet the amplitude "
        << tenBitMaxDev << " is 8-bit-sized — the Presented arm did not size to the supplied "
           "format";
    // ...and it must still BE a dither. Without this, an arm that collapsed to
    // Destination (float dst, step 0, no noise at all) would satisfy the bound
    // above by writing nothing.
    EXPECT_GT(tenBitMaxDev, kTenBitLsb * 0.25f)
        << "the 10-bit arm left the float destination undithered (max deviation " << tenBitMaxDev
        << ") — Presented collapsed to its destination's absent step";
    EXPECT_LT(tenBitMaxDev, kTenBitLsb * 1.5f)
        << "10-bit arm amplitude " << tenBitMaxDev << " exceeds 1.5x the 10-bit step "
        << kTenBitLsb;

    // The 8-bit arm is the other side of the pair: same code path, coarser
    // format, so it must dither at the coarser step.
    EXPECT_GT(eightBitMaxDev, kEightBitLsb * 0.25f)
        << "the 8-bit arm left the float destination undithered (max deviation " << eightBitMaxDev
        << ")";
    EXPECT_LT(eightBitMaxDev, kEightBitLsb * 1.5f)
        << "8-bit arm amplitude " << eightBitMaxDev << " exceeds 1.5x the 8-bit step "
        << kEightBitLsb;

    // The two arms differ ONLY in presentedFormat, so byte-identical output is
    // proof the field was not consulted. This is the exact failure shape that
    // makes a Presented test vacuous headless, asserted directly rather than
    // inferred from amplitudes.
    EXPECT_NE(std::memcmp(tenBitBytes.data(), eightBitBytes.data(), tenBitBytes.size()), 0)
        << "the 10-bit and 8-bit presented arms wrote identical bytes — presentedFormat "
           "changed nothing";
    EXPECT_GT(eightBitMaxDev, tenBitMaxDev * 2.0f)
        << "8-bit amplitude " << eightBitMaxDev << " is not meaningfully coarser than the 10-bit "
        << tenBitMaxDev << " — the two arms did not separate by depth";

    // COMPATIBILITY PIN, not a discrimination: unset must still take the device
    // read. Headless that lands on Unknown -> the 8-bit step, so it must match
    // the 8-bit arm byte-for-byte. This can fail (a mis-wired optional taking a
    // different path when empty) but it proves only that unset is unchanged, NOT
    // that the override works — the assertions above are what prove that.
    EXPECT_EQ(std::memcmp(deviceBytes.data(), eightBitBytes.data(), deviceBytes.size()), 0)
        << "an unset presentedFormat diverged from the device read (headless: Unknown, the "
           "8-bit step) — the default path is not byte-identical to today's";

    // The unset arm's bytes as one number, so "unset is byte-identical to the
    // device read" can be COMPARED across builds rather than argued from the
    // shape of the expression.
    uint64_t deviceHash = 1469598103934665603ull; // FNV-1a
    for (const uint8_t b : deviceBytes)
    {
        deviceHash ^= b;
        deviceHash *= 1099511628211ull;
    }
    GTEST_LOG_(INFO) << "presented amplitudes — 10-bit arm " << tenBitMaxDev << " (step "
                     << kTenBitLsb << "), 8-bit arm " << eightBitMaxDev << " (step " << kEightBitLsb
                     << "); unset-arm byte hash " << deviceHash;
}
