#include <gtest/gtest.h>
#include "TestDeviceHelper.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace GameEngine::Rendering;
namespace
{
using Pixel = std::array<float, 4>;
struct Extent { uint32_t sourceW, sourceH, outputW, outputH; };

// Independent scalar point-tap oracle: no bilinear pair packing and no shader
// helper reuse. Include out-of-bounds taps before clamping, as the sampler does.
Pixel TentReference(const std::vector<Pixel>& source, Extent e, uint32_t x, uint32_t y)
{
    const double rx = std::max(double(e.sourceW) / e.outputW, 1.0);
    const double ry = std::max(double(e.sourceH) / e.outputH, 1.0);
    const double cx = (x + .5) * e.sourceW / e.outputW;
    const double cy = (y + .5) * e.sourceH / e.outputH;
    std::array<double, 4> sum{};
    double total = 0;
    for (int iy = int(std::floor(cy - ry)) - 1; iy <= int(std::ceil(cy + ry)); ++iy)
        for (int ix = int(std::floor(cx - rx)) - 1; ix <= int(std::ceil(cx + rx)); ++ix)
        {
            const double w = std::max(1.0 - std::abs(ix + .5 - cx) / rx, 0.0) *
                             std::max(1.0 - std::abs(iy + .5 - cy) / ry, 0.0);
            const auto& p = source[std::clamp(iy, 0, int(e.sourceH) - 1) * e.sourceW +
                                   std::clamp(ix, 0, int(e.sourceW) - 1)];
            for (int c = 0; c < 4; ++c) sum[c] += w * p[c];
            total += w;
        }
    Pixel result{};
    for (int c = 0; c < 4; ++c) result[c] = float(sum[c] / total);
    return result;
}

class SsaaResolveTests : public testing::TestWithParam<Extent>
{
protected:
    std::unique_ptr<IDevice> device;
    std::vector<TextureHandle> textures;
    std::vector<BufferHandle> buffers;
    SamplerHandle sampler{};
    PipelineHandle pipeline{};
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device) GTEST_SKIP() << "No Vulkan device";
    }
    void TearDown() override
    {
        if (!device) return;
        device->WaitForIdle();
        for (auto t : textures) device->DestroyTexture(t);
        for (auto b : buffers) device->DestroyBuffer(b);
        if (sampler.IsValid()) device->DestroySampler(sampler);
        if (pipeline.IsValid()) device->DestroyPipeline(pipeline);
        device->Shutdown();
    }

    void Check(const std::vector<Pixel>& source)
    {
        const auto e = GetParam();
        ShaderPackage pkg{};
        std::string error;
        ASSERT_TRUE(LoadShaderPkg("Shaders/spatial_upscale.shaderpkg", device->PreferredShaderSource(), pkg, &error)) << error;
        ASSERT_TRUE(pkg.stageBytes.contains("vs"));
        ASSERT_TRUE(pkg.stageBytes.contains("fs"));
        DescriptorSetLayoutDesc layout{};
        layout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment});
        PipelineDesc pd{};
        pd.type = PipelineType::Graphics;
        pd.vertexShader = pkg.stageBytes.at("vs");
        pd.pixelShader = pkg.stageBytes.at("fs");
        pd.EnableDepthTest(false);
        pd.SetCullingMode(CullModeFlagBits::None, FrontFace::CounterClockwise);
        pd.AddDynamicState(DynamicState::Viewport);
        pd.AddDynamicState(DynamicState::Scissor);
        // FP32 isolates kernel error from FP16 quantization in the live HDR chain.
        pd.colorAttachmentFormats.push_back(uint32_t(TextureFormat::R32G32B32A32_FLOAT));
        pd.descriptorSetLayouts.push_back(layout);
        pd.pushConstantSize = 16;
        pd.pushConstantStagesMask = kShaderStageFragment;
        pipeline = device->CreatePipeline(pd);
        ASSERT_TRUE(pipeline.IsValid());

        TextureDesc td{};
        td.width = e.sourceW; td.height = e.sourceH;
        td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = uint32_t(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = uint32_t(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        auto input = device->CreateTexture(td);
        textures.push_back(input);
        ASSERT_TRUE(input.IsValid());
        td.width = e.outputW; td.height = e.outputH;
        td.usage = uint32_t(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        auto output = device->CreateTexture(td);
        textures.push_back(output);
        ASSERT_TRUE(output.IsValid());
        auto upload = device->CreateUploadBuffer(source.size() * sizeof(Pixel));
        auto readback = device->CreateReadbackBuffer(e.outputW * e.outputH * sizeof(Pixel));
        buffers = {upload, readback};
        ASSERT_TRUE(upload.IsValid());
        ASSERT_TRUE(readback.IsValid());
        device->UpdateBuffer(upload, 0, source.size() * sizeof(Pixel), source.data());
        sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("SsaaOracle"));
        ASSERT_TRUE(sampler.IsValid());
        DescriptorSetDesc sd{}; sd.layout = layout; sd.transient = true;
        auto set = device->CreateDescriptorSet(sd);
        ASSERT_TRUE(set.IsValid());
        device->UpdateCombinedImageSamplerBinding(set, 0, input, sampler);

        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(input, ResourceState::Undefined, ResourceState::CopyDest));
        cl->CopyBufferToTextureSubresource(upload, input, 0, 0, e.sourceW, e.sourceH);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(input, ResourceState::CopyDest, ResourceState::ShaderResource));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(output, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{}; rp.colorTargets[0] = output; rp.colorTargetCount = 1;
        rp.clearColor[0] = true; rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0, 0, float(e.outputW), float(e.outputH));
        cl->SetScissor(0, 0, e.outputW, e.outputH);
        cl->SetPipeline(pipeline);
        cl->BindDescriptorSet(0, set, pipeline);
        const std::array<float, 4> params{float(e.outputW), float(e.outputH), 0, 0};
        cl->SetPushConstants(params);
        cl->Draw(3, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(output, ResourceState::RenderTarget, ResourceState::CopySource));
        cl->CopyTextureSubresourceToBuffer(output, 0, 0, readback, e.outputW, e.outputH, 0, 0, 0, 0);
        cl->End();
        device->ExecuteCommandLists({cl.get()});
        device->FinalizeFrame();
        device->WaitForIdle();
        std::vector<Pixel> actual(e.outputW * e.outputH);
        const auto* mapped = device->MapBuffer(readback);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(actual.data(), mapped, actual.size() * sizeof(Pixel));
        device->UnmapBuffer(readback);
        // Hardware bilinear weights have `subTexelPrecisionBits` bits (8 on the
        // NVIDIA that failed this suite: 1/256). 0.3% of peak is tighter than
        // that at odd-extent scales: a 1/256 weight error on a 64-code texel
        // is 0.25 against a 0.192 bound. 1% sits above interpolator ULP and still
        // fails a missing tap, a phase error, wrap, or a box kernel.
        constexpr float kHardwareFilterRelativeTolerance = 0.01f;
        Pixel peak{1,1,1,1};
        for (const auto& p : source)
            for (int c = 0; c < 4; ++c) peak[c] = std::max(peak[c], p[c]);
        for (uint32_t y = 0; y < e.outputH; ++y)
            for (uint32_t x = 0; x < e.outputW; ++x)
            {
                const auto expected = TentReference(source, e, x, y);
                for (int c = 0; c < 4; ++c)
                {
                    const float value = actual[y * e.outputW + x][c];
                    ASSERT_TRUE(std::isfinite(value)) << x << ',' << y << " channel " << c;
                    EXPECT_NEAR(value, expected[c], peak[c] * kHardwareFilterRelativeTolerance)
                        << x << ',' << y << " channel " << c;
                }
            }
    }
};

TEST_P(SsaaResolveTests, PreservesConstantHdrAndAlpha)
{
    const auto e = GetParam();
    Check(std::vector<Pixel>(e.sourceW * e.sourceH, Pixel{32, .125f, 4, .375f}));
}

TEST_P(SsaaResolveTests, MatchesIndependentTentForDetailAndClampedBorders)
{
    const auto e = GetParam();
    std::vector<Pixel> source(e.sourceW * e.sourceH);
    for (uint32_t y = 0; y < e.sourceH; ++y)
        for (uint32_t x = 0; x < e.sourceW; ++x)
            source[y * e.sourceW + x] = {
                x == 0 || (x == e.sourceW / 2 && y == e.sourceH / 2) ? 64.f : 0.f,
                (x + y) % 2 ? 1.f : 0.f,
                y == e.sourceH - 1 ? 8.f : float((x * 7 + y * 3) % 11) / 11.f,
                float((x + 2 * y) % 5) / 4.f};
    Check(source);
}

// Exact 1.25/1.5/2x plus odd output extents with independently rounded axes,
// and a one-pixel destination to exercise fully clamped filter footprints.
INSTANTIATE_TEST_SUITE_P(ScalesAndExtents, SsaaResolveTests, testing::Values(
    Extent{20,15,16,12}, Extent{24,18,16,12}, Extent{32,24,16,12},
    Extent{22,16,17,13}, Extent{26,20,17,13}, Extent{34,26,17,13},
    Extent{2,2,1,1}));
}
