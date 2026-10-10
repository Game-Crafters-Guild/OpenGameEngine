// Run the production depth-normal reconstruction on a known sloped plane.
// A clamped sampler cannot supply a neighbour outside the viewport: treating
// that repeated depth as a new point tilts the edge normal toward the camera,
// brightening the border and seeding horizontal trails in temporal DDGI.
#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "TestDeviceHelper.h"
#include "StagedTestPaths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
class DDGIDepthNormalComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "No Vulkan device available";
        std::ifstream input(GameEngine::TestPaths::StagedRoot() / "Shaders" / "ddgi_depth_normal_test.comp.spv",
                            std::ios::binary);
        ASSERT_TRUE(input.good()) << "ddgi_depth_normal_test.comp.spv is not staged under <build>/Shaders";
        auto bytes = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        for (uint32_t binding = 0; binding < 3; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = binding == 0 ? DescriptorType::CombinedImageSampler : DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = bytes;
        desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        desc.DebugName = "DDGI.DepthNormalRegression";
        pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(pipeline.IsValid());
        sampler = device->CreateSampler(SamplerDesc::PointClamp("DDGI.DepthNormalRegression.Depth"));
        ASSERT_TRUE(sampler.IsValid());
    }

    void TearDown() override
    {
        if (!device)
            return;
        device->WaitForIdle();
        for (auto texture : textures)
            device->DestroyTexture(texture);
        for (auto buffer : buffers)
            device->DestroyBuffer(buffer);
        if (sampler.IsValid())
            device->DestroySampler(sampler);
    }

    BufferHandle Buffer(uint64_t bytes, uint32_t usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = usage;
        desc.memoryUsage = memory;
        desc.debugName = "DDGI.DepthNormalRegression.Buffer";
        auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        return buffer;
    }

    struct Parameters
    {
        std::array<float, 16> invProj{};
        std::array<float, 16> invView{};
        std::array<float, 4> camera{};
    };

    void DispatchIntoResult(uint32_t width, uint32_t height, const std::vector<float>& depth,
                            const Parameters& params)
    {
        result.clear();
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.debugName = "DDGI.DepthNormalRegression.PlaneDepth";
        const auto texture = device->CreateTexture(td);
        textures.push_back(texture);
        ASSERT_TRUE(texture.IsValid());
        const auto upload = Buffer(depth.size() * sizeof(float),
                                   static_cast<uint32_t>(BufferUsage::TransferSrc), BufferMemoryUsage::Upload);
        const auto parameters = Buffer(sizeof(params), static_cast<uint32_t>(BufferUsage::Storage),
                                       BufferMemoryUsage::Upload);
        const auto output = Buffer(depth.size() * sizeof(float) * 4,
                                   static_cast<uint32_t>(BufferUsage::Storage), BufferMemoryUsage::Readback);
        for (auto h : {upload, parameters, output})
            ASSERT_TRUE(h.IsValid());
        auto* mapped = device->MapBuffer(upload);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, depth.data(), depth.size() * sizeof(float));
        device->UnmapBuffer(upload);
        mapped = device->MapBuffer(parameters);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, &params, sizeof(params));
        device->UnmapBuffer(parameters);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = layout;
        const auto ds = device->CreateDescriptorSet(dsDesc);
        device->UpdateCombinedImageSamplerBinding(ds, 0, texture, sampler);
        device->UpdateStorageBufferBinding(ds, 1, parameters, 0, sizeof(params));
        device->UpdateStorageBufferBinding(ds, 2, output, 0, depth.size() * sizeof(float) * 4);
        auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, width, height);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                                ResourceState::ShaderResource));
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, ds, pipeline);
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();

        mapped = device->MapBuffer(output);
        ASSERT_NE(mapped, nullptr);
        result.resize(depth.size() * 4);
        std::memcpy(result.data(), mapped, result.size() * sizeof(float));
        device->UnmapBuffer(output);
    }

    std::vector<float> Dispatch(uint32_t width, uint32_t height, const std::vector<float>& depth,
                                const Parameters& params)
    {
        DispatchIntoResult(width, height, depth, params);
        return result;
    }

    void CheckPlane(uint32_t width, uint32_t height, float slopeX, float slopeY)
    {
        SCOPED_TRACE(::testing::Message() << width << "x" << height << " plane " << slopeX << "," << slopeY);
        // invProj maps clip (x,y,z,1) to homogeneous view (x,y,1,z),
        // i.e. positive-Z view with reverse depth 1/z. The plane is
        // slopeX*x + slopeY*y + z = 4, so its raw depth is affine in NDC.
        Parameters params;
        params.invProj[0] = params.invProj[5] = params.invProj[11] = params.invProj[14] = 1.0f;
        params.invView[0] = params.invView[5] = params.invView[10] = params.invView[15] = 1.0f;
        std::vector<float> depth(width * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / width - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / height;
                depth[y * width + x] = (slopeX * ndcX + slopeY * ndcY + 1.0f) / 4.0f;
            }
        auto normals = Dispatch(width, height, depth, params);
        ASSERT_EQ(normals.size(), depth.size() * 4);
        const float length = std::sqrt(slopeX * slopeX + slopeY * slopeY + 1.0f);
        float minInteriorAgreement = 1.0f;
        float minBorderAgreement = 1.0f;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float* n = &normals[(y * width + x) * 4];
                ASSERT_TRUE(std::isfinite(n[0]) && std::isfinite(n[1]) && std::isfinite(n[2]));
                const float agreement = -(slopeX * n[0] + slopeY * n[1] + n[2]) / length;
                if (x == 0 || y == 0 || x + 1 == width || y + 1 == height)
                    minBorderAgreement = std::min(minBorderAgreement, agreement);
                else
                    minInteriorAgreement = std::min(minInteriorAgreement, agreement);
            }
        EXPECT_GT(minInteriorAgreement, 0.9999f);
        EXPECT_GT(minBorderAgreement, 0.9999f)
            << "screen-edge normals must describe the same plane as the interior";
    }

    std::vector<float> result;
    std::unique_ptr<IDevice> device;
    DescriptorSetLayoutDesc layout{};
    PipelineHandle pipeline{};
    SamplerHandle sampler{};
    std::vector<TextureHandle> textures;
    std::vector<BufferHandle> buffers;
};

TEST_F(DDGIDepthNormalComputeTest, SlopedPlanesKeepTheirNormalsAtViewportBorders)
{
    CheckPlane(32, 24, 0.2f, 0.5f);
    CheckPlane(33, 25, -0.2f, -0.5f);
}
}  // namespace
