// Pass-level guard for the terminal encode's gradient-aware deband.
//
// The band class this targets arrives ALREADY stepped in the linear source
// (8-bit-authored albedo under exposure gain: 4-7 output-LSB steps, plateaus
// tens of pixels wide) — structure the 1-LSB TPDF dither cannot break. This
// test renders a synthetic staircase of exactly that class through the real
// AddSRGBEncodePassRG (production shaderpkg, reflected push constants, real
// pipeline) and asserts on the quantized statistics:
//   - the staircase collapses into a smooth ramp (adjacent column means step
//     well under the source's 6-LSB plateau jumps) while the ramp's total
//     rise is preserved (the filter smooths, never flattens);
//   - a hard 64-LSB edge in the same image survives at full contrast and
//     single-column sharpness (the threshold gate protects real detail);
//   - a flat field stays put (no drift from the tap averaging).
// A dead deband (zeroed/dropped debandThreshold push constant, broken tap
// gate, kill switch stuck) reproduces the 6-LSB staircase and fails the first
// assertion loudly; an over-eager one fails the edge/flat assertions.
//
// The deband settings are read once per process (like the dither kill
// switch), so this binary pins the environment before first use and the
// GE_DEBAND=0 path is runtime-verified in the editor instead.

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

// Inverse of the shader's LinearToSRGB (encode_srgb.frag) on one channel —
// the test authors its fixture in ENCODED space (where the band class is
// defined) and feeds the pass the linear preimage.
float SrgbToLinearRef(float e)
{
    if (e <= 0.04045f)
        return e / 12.92f;
    return std::pow((e + 0.055f) / 1.055f, 2.4f);
}

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 64 * 1024) {}
};

} // namespace

TEST(SrgbEncodeDeband, StaircaseCollapsesEdgesAndFlatsSurvive)
{
    SetEnv("GE_HEADLESS_TEST", "1");
    // Settings are latched once per process — pin them so stray environment
    // overrides cannot turn this guard into a false result. Empty strings are
    // rejected by the override parser, leaving the shipped defaults active.
    SetEnv("GE_OUTPUT_DITHER", "1");
    SetEnv("GE_DEBAND", "1");
    SetEnv("GE_DEBAND_THRESHOLD", "");
    SetEnv("GE_DEBAND_RADIUS", "");

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

    constexpr uint32_t kW = 512;
    constexpr uint32_t kH = 96;

    // Fixture, authored in encoded space:
    //   x [0, 384)  — staircase: 6-LSB steps every 16 px starting at 64/255,
    //                 the measured band class (steps of several LSB with
    //                 plateaus the dither cannot bridge).
    //   x [384,448) — flat field A at 76.5/255.
    //   x [448,512) — flat field B at 140.25/255: a hard ~64-LSB edge at 448.
    constexpr float kStairBase = 64.0f / 255.0f;
    constexpr float kStairStep = 6.0f / 255.0f;
    constexpr int kPlateauPx = 16;
    constexpr int kStairEnd = 384;
    constexpr int kEdgeX = 448;
    constexpr float kFlatA = 0.3f;  // 76.5 LSB
    constexpr float kFlatB = 0.55f; // 140.25 LSB — 63.75-LSB edge vs kFlatA

    auto encodedAt = [&](uint32_t x) -> float
    {
        if (static_cast<int>(x) < kStairEnd)
            return kStairBase + kStairStep * static_cast<float>(x / kPlateauPx);
        return (static_cast<int>(x) < kEdgeX) ? kFlatA : kFlatB;
    };

    TextureDesc srcDesc{};
    srcDesc.width = kW;
    srcDesc.height = kH;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = "DebandTest.LinearStaircase";
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    ASSERT_TRUE(srcTex.IsValid());

    std::vector<float> pixels(static_cast<size_t>(kW) * kH * 4);
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const float v = SrgbToLinearRef(encodedAt(x));
            float* px = &pixels[(static_cast<size_t>(y) * kW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "DebandTest.Upload");
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
    const BufferHandle readback = device->CreateReadbackBuffer(outBytes, "DebandTest.Readback");
    ASSERT_TRUE(readback.IsValid());

    FramePools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "DebandTest.Linear", srcTex, ResourceState::ShaderResource,
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
    dstDesc.debugName = "DebandTest.Encoded";
    const RG::RGTexture dst = frame.CreateTexture("DebandTest.Encoded", dstDesc);
    ASSERT_TRUE(dst.IsValid());

    // Offscreen dst → outEncoding=1 (manual sRGB): SDR dither step AND the
    // SDR deband threshold, exactly the screen's configuration. The filter is
    // opt-in (baseline 0), so the test passes the reviewed 6-LSB gate the way
    // an enabling DebandEffect volume would.
    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = Passes::FinalizeInputSpace::Linear,
         .Quantizer = Passes::FinalizeQuantizer::Destination,
         .VolumeDebandThresholdLsb = 6.0f},
        "DebandTest.Encode");
    ASSERT_TRUE(encodePass.IsValid())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    frame.AddPass(
        "DebandTest.Readback", PassPhase::kFinalize,
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

    // Column means of the green channel over all rows: rows are independent
    // dither realizations, so the mean isolates the deband's structure from
    // the TPDF noise (the same instrument as the editor strip profiler).
    std::vector<double> colMean(kW, 0.0);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            colMean[x] += out[(static_cast<size_t>(y) * kW + x) * 4 + 1];
    for (uint32_t x = 0; x < kW; ++x)
        colMean[x] /= static_cast<double>(kH);
    device->UnmapBuffer(readback);

    // ── 1. Staircase collapsed. Interior margin keeps tap clamp-at-border and
    // the staircase/flat boundary out of the measurement. Undebanded, plateau
    // boundaries jump ~6 LSB between adjacent column means; debanded + dithered
    // they must ramp.
    double maxAdjStep = 0.0;
    for (int x = 32; x < 351; ++x)
        maxAdjStep = std::max(maxAdjStep, std::abs(colMean[x + 1] - colMean[x]));
    EXPECT_LE(maxAdjStep, 2.0) << "staircase survived the deband — adjacent column means "
                                  "still step " << maxAdjStep << " LSB";

    // ── 2. Ramp preserved: the filter smooths toward local averages, so the
    // total rise across the measured span must match the authored staircase.
    const double idealRise = 6.0 * ((344 / kPlateauPx) - (40 / kPlateauPx));
    const double measuredRise = colMean[344] - colMean[40];
    EXPECT_NEAR(measuredRise, idealRise, 4.0)
        << "staircase rise not preserved (flattened or overshot)";

    // ── 3. Hard edge survives at full contrast and single-column sharpness:
    // the threshold gate must reject a ~64-LSB edge outright.
    const double idealEdge = (kFlatB - kFlatA) * 255.0;
    EXPECT_NEAR(colMean[500] - colMean[400], idealEdge, 2.0)
        << "edge contrast changed — the deband is eating real edges";
    EXPECT_GE(colMean[kEdgeX] - colMean[kEdgeX - 1], idealEdge - 3.0)
        << "edge blurred across columns — the threshold gate failed";

    // ── 4. Flat field stays put (tap averaging must not drift a constant
    // region; measured away from the edge so no tap crosses it).
    double flatMean = 0.0;
    for (int x = 392; x < 424; ++x)
        flatMean += colMean[x];
    flatMean /= 32.0;
    EXPECT_NEAR(flatMean, kFlatA * 255.0, 1.0) << "flat field drifted under the deband";
}

// The PostProcessVolume lever: a volume-resolved threshold of 0 (DebandEffect
// disabled, or ThresholdLsb 0, or simply no volume — the opt-in default) must
// reach the pass as debandThreshold 0 — the dynamically uniform off branch —
// and reproduce the undebanded staircase. Together with the enabled-path test
// above (which passes 6 explicitly), the pair pins the lever plumbing in both
// directions.
TEST(SrgbEncodeDeband, VolumeThresholdZeroDisablesTheFilter)
{
    SetEnv("GE_HEADLESS_TEST", "1");
    // Same process-wide pins as the enabled-path test (settings latch once;
    // identical pins make test order irrelevant). GE_DEBAND stays ON — this
    // test proves the VOLUME lever alone turns the filter off.
    SetEnv("GE_OUTPUT_DITHER", "1");
    SetEnv("GE_DEBAND", "1");
    SetEnv("GE_DEBAND_THRESHOLD", "");
    SetEnv("GE_DEBAND_RADIUS", "");

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

    // Staircase-only slice of the enabled-path fixture: 6-LSB steps every
    // 16 px. With the filter off these steps must SURVIVE quantization.
    constexpr uint32_t kW = 384;
    constexpr uint32_t kH = 64;
    constexpr float kStairBase = 64.0f / 255.0f;
    constexpr float kStairStep = 6.0f / 255.0f;
    constexpr int kPlateauPx = 16;

    TextureDesc srcDesc{};
    srcDesc.width = kW;
    srcDesc.height = kH;
    srcDesc.depth = 1;
    srcDesc.mipLevels = 1;
    srcDesc.arrayLayers = 1;
    srcDesc.sampleCount = 1;
    srcDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
    srcDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    srcDesc.debugName = "DebandOffTest.LinearStaircase";
    const TextureHandle srcTex = device->CreateTexture(srcDesc);
    ASSERT_TRUE(srcTex.IsValid());

    std::vector<float> pixels(static_cast<size_t>(kW) * kH * 4);
    for (uint32_t y = 0; y < kH; ++y)
    {
        for (uint32_t x = 0; x < kW; ++x)
        {
            const float v = SrgbToLinearRef(
                kStairBase + kStairStep * static_cast<float>(x / kPlateauPx));
            float* px = &pixels[(static_cast<size_t>(y) * kW + x) * 4];
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 1.0f;
        }
    }
    const size_t srcBytes = pixels.size() * sizeof(float);
    const BufferHandle upload = device->CreateUploadBuffer(srcBytes, "DebandOffTest.Upload");
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
    const BufferHandle readback = device->CreateReadbackBuffer(outBytes, "DebandOffTest.Readback");
    ASSERT_TRUE(readback.IsValid());

    FramePools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    const RG::RGTexture src = frame.ImportExternalTexture(
        "DebandOffTest.Linear", srcTex, ResourceState::ShaderResource,
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
    dstDesc.debugName = "DebandOffTest.Encoded";
    const RG::RGTexture dst = frame.CreateTexture("DebandOffTest.Encoded", dstDesc);
    ASSERT_TRUE(dst.IsValid());

    // The lever under test: volume-resolved threshold 0 through the real
    // argument (env untouched — the volume alone must win here).
    const RG::RGPass encodePass = Passes::AddSRGBEncodePassRG(
        frame, src, dst,
        {.InputSpace = Passes::FinalizeInputSpace::Linear,
         .Quantizer = Passes::FinalizeQuantizer::Destination,
         .VolumeDebandThresholdLsb = 0.0f},
        "DebandOffTest.Encode");
    ASSERT_TRUE(encodePass.IsValid())
        << "AddSRGBEncodePassRG declared nothing — encode_srgb.shaderpkg not staged "
           "under the build root's Shaders/ (tests run from CMAKE_BINARY_DIR)";

    frame.AddPass(
        "DebandOffTest.Readback", PassPhase::kFinalize,
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

    std::vector<double> colMean(kW, 0.0);
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            colMean[x] += out[(static_cast<size_t>(y) * kW + x) * 4 + 1];
    for (uint32_t x = 0; x < kW; ++x)
        colMean[x] /= static_cast<double>(kH);
    device->UnmapBuffer(readback);

    // With the filter off, plateau boundaries keep their ~6-LSB jumps between
    // adjacent column means (the 1-LSB TPDF cannot bridge them — the exact
    // observation that motivated the deband). Interior margin mirrors the
    // enabled-path test.
    double maxAdjStep = 0.0;
    for (int x = 32; x < 351; ++x)
        maxAdjStep = std::max(maxAdjStep, std::abs(colMean[x + 1] - colMean[x]));
    EXPECT_GE(maxAdjStep, 4.0)
        << "staircase did NOT survive — the deband ran despite a volume threshold of 0 "
           "(lever argument dropped, or ResolveOutputDebandThresholdLsb ignored it)";

    // And the encode itself must remain faithful: each plateau's interior mean
    // stays within the dither envelope of the authored encoded value.
    for (int plateau = 3; plateau < 21; ++plateau)
    {
        const int x0 = plateau * kPlateauPx + 4;
        const int x1 = (plateau + 1) * kPlateauPx - 4;
        double mean = 0.0;
        for (int x = x0; x < x1; ++x)
            mean += colMean[x];
        mean /= static_cast<double>(x1 - x0);
        const double ideal = 64.0 + 6.0 * plateau;
        EXPECT_NEAR(mean, ideal, 1.0)
            << "plateau " << plateau << " drifted with the filter off";
    }
}
