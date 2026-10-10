#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowDispatch.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace
{
class ScreenSpaceShadowComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device) GTEST_SKIP() << "No Vulkan device";
        std::ifstream file(std::string(ENGINE_TEST_BUILD_DIR) +
            "/Engine/Tests/Shaders/screen_space_shadow_test.spv", std::ios::binary);
        ASSERT_TRUE(file.good());
        auto bytes = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        for (uint32_t i = 0; i < 4; ++i)
        {
            DescriptorBinding binding{};
            binding.binding = i;
            binding.type = i == 0 ? DescriptorType::CombinedImageSampler :
                i == 2 ? DescriptorType::UniformBuffer :
                DescriptorType::StorageBuffer;
            binding.count = 1;
            binding.shaderStages = kShaderStageCompute;
            layout.bindings.push_back(binding);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = bytes;
        desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        desc.DebugName = "ScreenSpaceShadow.Test";
        pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(pipeline.IsValid());
        std::ifstream prepareFile(std::string(ENGINE_TEST_BUILD_DIR) +
            "/Engine/Tests/Shaders/screen_space_shadow_prepare_test.spv", std::ios::binary);
        ASSERT_TRUE(prepareFile.good());
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(prepareFile), std::istreambuf_iterator<char>());
        prepareLayout.bindings.assign(layout.bindings.begin(), layout.bindings.begin() + 2);
        desc.DescriptorSetLayouts = {device->InternDescriptorSetLayout(prepareLayout)};
        preparePipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(preparePipeline.IsValid());
        std::ifstream resolveFile(std::string(ENGINE_TEST_BUILD_DIR) +
            "/Engine/Tests/Shaders/screen_space_shadow_resolve_test.spv", std::ios::binary);
        ASSERT_TRUE(resolveFile.good());
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(resolveFile), std::istreambuf_iterator<char>());
        resolveLayout.bindings.assign(layout.bindings.begin(), layout.bindings.begin() + 3);
        resolveLayout.bindings[1].type = DescriptorType::StorageImage;
        resolveLayout.bindings[2].type = DescriptorType::StorageBuffer;
        desc.DescriptorSetLayouts = {device->InternDescriptorSetLayout(resolveLayout)};
        resolvePipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(resolvePipeline.IsValid());
        ShaderPackage matchPackage;
        std::string packageError;
        ASSERT_TRUE(LoadShaderPkg("Shaders/screen_space_shadow_match.shaderpkg",
            device->PreferredShaderSource(), matchPackage, &packageError)) << packageError;
        matchMeta = std::move(matchPackage.meta);
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
            std::move(matchPackage.stageBytes.at("cs")));
        for (uint32_t i = 0; i < 6; ++i)
        {
            DescriptorBinding binding{};
            binding.binding = i; binding.count = 1; binding.shaderStages = kShaderStageCompute;
            binding.type = i < 2 ? DescriptorType::CombinedImageSampler :
                i == 2 ? DescriptorType::StorageBuffer : i == 3 ? DescriptorType::StorageImage : DescriptorType::UniformBuffer;
            matchLayout.bindings.push_back(binding);
        }
        desc.DescriptorSetLayouts = {device->InternDescriptorSetLayout(matchLayout)};
        matchPipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(matchPipeline.IsValid());
        std::ifstream planeFile(std::string(ENGINE_TEST_BUILD_DIR) +
            "/Engine/Tests/Shaders/shadow_receiver_plane_test.spv", std::ios::binary);
        ASSERT_TRUE(planeFile.good());
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(planeFile), std::istreambuf_iterator<char>());
        planeLayout.bindings = {layout.bindings[0]};
        planeLayout.bindings[0].type = DescriptorType::StorageBuffer;
        desc.DescriptorSetLayouts = {device->InternDescriptorSetLayout(planeLayout)};
        planePipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(planePipeline.IsValid());
        sampler = device->CreateSampler(SamplerDesc::PointClamp("ScreenSpaceShadow.Test"));
        ASSERT_TRUE(sampler.IsValid());
    }
    void TearDown() override
    {
        if (!device) return;
        device->WaitForIdle();
        for (auto value : sets) device->DestroyDescriptorSet(value);
        for (auto value : textures) device->DestroyTexture(value);
        for (auto value : buffers) device->DestroyBuffer(value);
        if (sampler.IsValid()) device->DestroySampler(sampler);
    }
    BufferHandle Buffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory, const void* data = nullptr)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        if (data && buffer.IsValid())
        {
            void* mapped = device->MapBuffer(buffer);
            if (mapped) { std::memcpy(mapped, data, bytes); device->UnmapBuffer(buffer); }
        }
        return buffer;
    }
    TextureHandle Texture(uint32_t width, uint32_t height, TextureFormat format, TextureUsage usage)
    {
        TextureDesc desc{};
        desc.width = width; desc.height = height;
        desc.depth = desc.mipLevels = desc.arrayLayers = desc.sampleCount = 1;
        desc.format = static_cast<uint32_t>(format);
        desc.usage = static_cast<uint32_t>(usage);
        auto result = device->CreateTexture(desc);
        textures.push_back(result);
        return result;
    }
    void Run(uint32_t width, uint32_t height, const std::vector<float>& depths,
             std::array<float, 4> light, float thickness = 0.005f, bool reverseOrder = false,
             const std::vector<uint32_t>& resolveInput = {}, float filterPixelsPerTexel = 0,
             float filterAngle = 0, int filterQuality = 0)
    {
        results.clear(); coverage.clear();
        ASSERT_EQ(depths.size(), width * height);
        ASSERT_TRUE(resolveInput.empty() || resolveInput.size() == depths.size());
        const auto plan = BuildScreenSpaceShadowDispatches(light.data(), width, height);
        ASSERT_GT(plan.Count, 0u);
        // The passes read depth as a sampled float, so the guide carries the
        // same values in a colour format. A buffer upload into a D32_FLOAT
        // image lands on some backends and silently does not on others, which
        // would leave every case below running against an all-zero depth.
        auto depth = Texture(width, height, TextureFormat::R32_FLOAT,
            TextureUsage::ShaderResource | TextureUsage::TransferDst);
        auto mask = Texture(width, height, TextureFormat::R32_UINT,
            TextureUsage::UnorderedAccess | TextureUsage::TransferDst | TextureUsage::TransferSrc | TextureUsage::ShaderResource);
        ASSERT_TRUE(depth.IsValid() && mask.IsValid());
        std::vector<uint32_t> empty(width * height, 0xdeadbeefu);
        std::vector<uint32_t> zero(width * height, 0);
        auto upload = Buffer(depths.size() * 4, BufferUsage::TransferSrc, BufferMemoryUsage::Upload, depths.data());
        auto init = Buffer(empty.size() * 4, BufferUsage::TransferSrc, BufferMemoryUsage::Upload, empty.data());
        auto readback = Buffer(empty.size() * 4, BufferUsage::TransferDst, BufferMemoryUsage::Readback);
        auto visibility = Buffer(empty.size() * 4, BufferUsage::Storage, BufferMemoryUsage::Upload,
                                 resolveInput.empty() ? nullptr : resolveInput.data());
        auto countReadback = Buffer(zero.size() * 4, BufferUsage::Storage, BufferMemoryUsage::Readback, zero.data());
        auto cmd = device->CreateCommandList(IDevice::QueueType::Graphics);
        cmd->Begin();
        cmd->CopyBufferToTextureSubresource(upload, depth, 0, 0, width, height, 0);
        cmd->CopyBufferToTextureSubresource(init, mask, 0, 0, width, height, 0);
        cmd->Barrier(ResourceBarrier::CreateTextureBarrier(depth, ResourceState::CopyDest, ResourceState::ShaderResource));
        cmd->Barrier(ResourceBarrier::CreateTextureBarrier(mask, ResourceState::CopyDest, ResourceState::UnorderedAccess));
        DescriptorSetDesc prepareDesc{}; prepareDesc.layout = prepareLayout;
        auto prepareSet = device->CreateDescriptorSet(prepareDesc); sets.push_back(prepareSet);
        device->UpdateCombinedImageSamplerBinding(prepareSet, 0, depth, sampler);
        device->UpdateStorageBufferBinding(prepareSet, 1, visibility, 0, empty.size() * 4);
        if (resolveInput.empty())
        {
            cmd->SetPipeline(preparePipeline);
            cmd->BindDescriptorSet(0, prepareSet, preparePipeline);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
        cmd->Barrier(ResourceBarrier::CreateBufferBarrier(visibility, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess));
        cmd->SetPipeline(pipeline);
        for (uint32_t dispatchIndex = 0; resolveInput.empty() && dispatchIndex < plan.Count; ++dispatchIndex)
        {
            const uint32_t i = reverseOrder ? plan.Count - 1 - dispatchIndex : dispatchIndex;
            struct Params { float Light[4]; int32_t Wave[4]; float Settings[4]; } params{};
            std::copy_n(plan.Light, 4, params.Light);
            std::copy_n(plan.Dispatches[i].Offset, 2, params.Wave);
            params.Wave[2] = static_cast<int32_t>(width); params.Wave[3] = static_cast<int32_t>(height);
            params.Settings[0] = thickness;
            params.Settings[1] = thickness * 4; params.Settings[2] = 4;
            auto uniform = Buffer(sizeof(params), BufferUsage::Uniform, BufferMemoryUsage::Upload, &params);
            DescriptorSetDesc desc{}; desc.layout = layout;
            auto set = device->CreateDescriptorSet(desc); sets.push_back(set);
            device->UpdateCombinedImageSamplerBinding(set, 0, depth, sampler);
            device->UpdateStorageBufferBinding(set, 1, visibility, 0, empty.size() * 4);
            device->UpdateBufferBinding(set, 2, uniform, 0, sizeof(params));
            device->UpdateStorageBufferBinding(set, 3, countReadback, 0, zero.size() * 4);
            cmd->BindDescriptorSet(0, set, pipeline);
            const auto& d = plan.Dispatches[i];
            cmd->Dispatch(d.Groups[0], d.Groups[1], d.Groups[2]);
        }
        cmd->Barrier(ResourceBarrier::CreateBufferBarrier(visibility, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess));
        DescriptorSetDesc resolveDesc{}; resolveDesc.layout = resolveLayout;
        auto resolveSet = device->CreateDescriptorSet(resolveDesc); sets.push_back(resolveSet);
        device->UpdateCombinedImageSamplerBinding(resolveSet, 0, depth, sampler);
        device->UpdateStorageImageBinding(resolveSet, 1, mask);
        device->UpdateStorageBufferBinding(resolveSet, 2, visibility, 0, empty.size() * 4);
        cmd->SetPipeline(resolvePipeline);
        cmd->BindDescriptorSet(0, resolveSet, resolvePipeline);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        auto readMask = mask;
        if (filterPixelsPerTexel > 0)
        {
            auto matched = Texture(width, height, TextureFormat::R32_UINT,
                TextureUsage::UnorderedAccess | TextureUsage::TransferSrc);
            struct MatchParams { float Inverse[16]; float Projection[4]; } params{};
            for (int i = 0; i < 16; i += 5) params.Inverse[i] = 1;
            params.Projection[1] = 0.1f; params.Projection[2] = 64;
            ShadowDataGPU shadow{};
            const float c = std::cos(filterAngle), s = std::sin(filterAngle);
            const float sx = float(width) / (64 * filterPixelsPerTexel);
            const float sy = float(height) / (64 * filterPixelsPerTexel);
            shadow.shadowVP[0][0] = sx * c; shadow.shadowVP[0][4] = -sy * s;
            shadow.shadowVP[0][1] = sx * s; shadow.shadowVP[0][5] = sy * c;
            shadow.shadowVP[0][10] = shadow.shadowVP[0][15] = 1;
            shadow.shadowSplits[0] = 100;
            shadow.shadowParams[2] = 1; shadow.shadowParams[3] = 100;
            shadow.shadowDebug[3] = float(filterQuality);
            auto uniform = Buffer(sizeof(params), BufferUsage::Uniform, BufferMemoryUsage::Upload, &params);
            auto shadowBuffer = Buffer(sizeof(shadow), BufferUsage::Uniform, BufferMemoryUsage::Upload, &shadow);
            DescriptorSetDesc desc{}; desc.layout = matchLayout;
            auto set = device->CreateDescriptorSet(desc); sets.push_back(set);
            device->UpdateCombinedImageSamplerBinding(set, 0, depth, sampler);
            device->UpdateCombinedImageSamplerBinding(set, 1, mask, sampler);
            device->UpdateStorageBufferBinding(set, 2, visibility, 0, empty.size() * 4);
            device->UpdateStorageImageBinding(set, 3, matched);
            // Use the packaged reflection names, as the live service does.
            // Numeric test bindings hid an unbound Params/Shadow regression.
            NamedDescriptorWriter writer(device.get(), set, matchMeta, 0);
            ASSERT_TRUE(writer.TryAddUniformBuffer("Params", uniform, 0, sizeof(params)));
            ASSERT_TRUE(writer.TryAddUniformBuffer("Shadow", shadowBuffer, 0, sizeof(shadow)));
            writer.Flush();
            cmd->Barrier(ResourceBarrier::CreateTextureBarrier(mask, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
            cmd->Barrier(ResourceBarrier::CreateTextureBarrier(matched, ResourceState::Undefined, ResourceState::UnorderedAccess));
            cmd->SetPipeline(matchPipeline); cmd->BindDescriptorSet(0, set, matchPipeline);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            readMask = matched;
        }
        cmd->Barrier(ResourceBarrier::CreateTextureBarrier(readMask, ResourceState::UnorderedAccess, ResourceState::CopySource));
        cmd->CopyTextureToBuffer(readMask, readback, width, height);
        cmd->End();
        device->ExecuteCommandLists({cmd.get()}); device->WaitForIdle();
        results.resize(empty.size()); coverage.resize(zero.size());
        void* mapped = device->MapBuffer(readback); ASSERT_NE(mapped, nullptr);
        for (size_t i = 0; i < results.size(); ++i)
        {
            uint32_t bits = static_cast<const uint32_t*>(mapped)[i];
            results[i][0] = float(bits & 255u) / 255.0f;
            bits &= 0xffffff00u;
            std::memcpy(&results[i][1], &bits, 4);
        }
        device->UnmapBuffer(readback);
        mapped = device->MapBuffer(countReadback); ASSERT_NE(mapped, nullptr);
        std::memcpy(coverage.data(), mapped, coverage.size() * 4); device->UnmapBuffer(countReadback);
        uint32_t holes = 0, overlap = 0;
        for (uint32_t count : coverage) { holes += count == 0; overlap += count > 1; }
        if (resolveInput.empty()) ASSERT_LE(holes, 1u);
        if (resolveInput.empty() && width > 16 && height > 16 && std::abs(light[0]) < 1 && std::abs(light[1]) < 1)
            EXPECT_GT(overlap, 0u); // Exercise the atomic merge, not only disjoint rays.
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (resolveInput.empty() && coverage[i] == 0u)
                ASSERT_TRUE(int(i % width) == int(std::floor(plan.Light[0])) &&
                            int(i / width) == int(std::floor(plan.Light[1])))
                    << "uncovered pixel " << i % width << "," << i / width;
            ASSERT_TRUE(std::isfinite(results[i][0]));
            ASSERT_GE(results[i][0], 0.0f); ASSERT_LE(results[i][0], 1.0f);
            ASSERT_NEAR(results[i][1], depths[i], std::abs(depths[i]) * 3.1e-5f);
        }
    }
    std::unique_ptr<IDevice> device;
    DescriptorSetLayoutDesc layout{}, prepareLayout{}, resolveLayout{}, matchLayout{}, planeLayout{};
    PipelineHandle pipeline{}, preparePipeline{}, resolvePipeline{}, matchPipeline{}, planePipeline{}; SamplerHandle sampler{};
    std::vector<TextureHandle> textures; std::vector<BufferHandle> buffers;
    std::vector<DescriptorSetHandle> sets;
    ShaderMeta matchMeta{};
    std::vector<std::array<float, 2>> results;
    std::vector<uint32_t> coverage;
};

TEST(ScreenSpaceShadowDispatch, RejectsInvalidAndUnstableInputs)
{
    std::array<float, 4> light{1, 0, 0, 1};
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(nullptr, 100, 100).Count, 0u);
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(light.data(), 0, 100).Count, 0u);
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(light.data(), 100, 16385).Count, 0u);
    light[3] = 0;
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(light.data(), 100, 100).Count, 0u);
    light[3] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(light.data(), 100, 100).Count, 0u);
    light = {1e20f, 0, 0, 1};
    EXPECT_EQ(BuildScreenSpaceShadowDispatches(light.data(), 100, 100).Count, 0u);
}

TEST_F(ScreenSpaceShadowComputeTest, SkyIsLitAndRadialCoverageOnlyOmitsTheLightCenter)
{
    for (auto size : {std::array<uint32_t, 2>{1, 1}, {17, 13}, {129, 67}, {320, 180}})
        for (float w : {-1.0f, 1.0f})
            for (auto xy : {std::array<float, 2>{0, 0}, {-3, 0.7f}, {2, -4}, {0.13f, -0.31f}})
            {
                SCOPED_TRACE(::testing::Message() << size[0] << "x" << size[1] << " w=" << w << " x=" << xy[0]);
                Run(size[0], size[1], std::vector<float>(size[0] * size[1], 0), {xy[0], xy[1], 0, w});
                ASSERT_FALSE(results.empty());
                for (auto pixel : results) ASSERT_FLOAT_EQ(pixel[0], 1);
            }
}

TEST_F(ScreenSpaceShadowComputeTest, CameraFacingFlatReceiverStaysLitIncludingBorders)
{
    for (float depth : {0.00001f, 0.2f, 0.9f})
        for (auto xy : {std::array<float, 2>{0, 0}, {-3, 0.7f}, {2, -4}})
        {
            Run(129, 67, std::vector<float>(129 * 67, depth), {xy[0], xy[1], 0, -1});
            ASSERT_FALSE(results.empty());
            for (auto pixel : results) ASSERT_NEAR(pixel[0], 1, 1e-5f);
        }
}

TEST_F(ScreenSpaceShadowComputeTest, ContactStepCastsShadowOnTheReceiverBesideIt)
{
    constexpr uint32_t width = 257, height = 97;
    std::vector<float> depths(width * height, 0.4f);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width / 2; ++x) depths[y * width + x] = 0.405f;
    Run(width, height, depths, {-4, 0, 0, -1});
    ASSERT_FALSE(results.empty());
    uint32_t shadowedReceivers = 0;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = width / 2; x < width / 2 + 30; ++x)
            shadowedReceivers += results[y * width + x][0] < 0.9f;
    EXPECT_GT(shadowedReceivers, height);
}

TEST_F(ScreenSpaceShadowComputeTest, OverlappingRaysAreIndependentOfDispatchOrder)
{
    std::vector<float> depths(257 * 131);
    for (size_t i = 0; i < depths.size(); ++i)
        depths[i] = 0.4f + float((i * 13) % 97) * 0.0001f;
    Run(257, 131, depths, {0.271f, -0.18f, 0, -1});
    const auto forward = results;
    Run(257, 131, depths, {0.271f, -0.18f, 0, -1}, 0.005f, true);
    EXPECT_EQ(results, forward);
}

TEST_F(ScreenSpaceShadowComputeTest, ThinOpaqueBlockerDoesNotCreateAPartialVisibilityBand)
{
    constexpr uint32_t width = 257, height = 97, blockerX = 100;
    std::vector<float> depths(width * height, 0.4f);
    for (uint32_t y = 0; y < height; ++y)
        depths[y * width + blockerX] = 0.405f;
    Run(width, height, depths, {-6, 0, 0, -1});
    ASSERT_EQ(results.size(), depths.size());
    uint32_t darkCore = 0;
    // This offset is beyond the four hard-contact samples and the resolve's
    // two-pixel radius. One interleaved lane sees an opaque blocker; averaging
    // four lanes would incorrectly cap its shadow opacity at 25 percent.
    for (uint32_t y = 4; y < height - 4; ++y)
        for (uint32_t x = blockerX + 8; x < blockerX + 24; ++x)
            darkCore += results[y * width + x][0] < 0.15f;
    EXPECT_GT(darkCore, height / 2);
}

TEST_F(ScreenSpaceShadowComputeTest, FarOffscreenLightAndViewportResizePreserveCoverage)
{
    for (auto size : {std::array<uint32_t, 2>{1023, 577}, {127, 65}, {1025, 579}})
        for (auto light : {std::array<float, 4>{0.2f, -0.1f, 0, -0.0011f},
                           {0.2f, -0.1f, 0, 0.0011f}})
        {
            SCOPED_TRACE(::testing::Message() << size[0] << "x" << size[1] << " light w=" << light[3]);
            Run(size[0], size[1], std::vector<float>(size[0] * size[1], 0), light);
            for (auto pixel : results) ASSERT_FLOAT_EQ(pixel[0], 1);
        }
}

TEST_F(ScreenSpaceShadowComputeTest, DiscontinuitiesAndThicknessExtremesStayFinite)
{
    std::vector<float> depths(131 * 73);
    for (size_t i = 0; i < depths.size(); ++i)
        depths[i] = i % 7 == 0 ? 0 : float((i * 17) % 991 + 1) / 1000;
    for (float thickness : {0.0001f, 0.005f, 0.05f})
        for (float w : {-1.0f, 1.0f})
            Run(131, 73, depths, {0.271f, -0.18f, -0.0001f * w, w}, thickness);
}

TEST_F(ScreenSpaceShadowComputeTest, ResolveRemovesSubpixelNoiseOnSlopingReceivers)
{
    constexpr uint32_t width = 33, height = 19;
    std::vector<float> depths(width * height);
    std::vector<uint32_t> noisy(width * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            depths[y * width + x] = 0.3f + x * 0.003f + y * 0.002f;
            noisy[y * width + x] = ((x + y) & 1) ? 255u : 0u;
        }
    Run(width, height, depths, {0, 0, 0, -1}, 0.005f, false, noisy);
    ASSERT_EQ(results.size(), depths.size());
    for (uint32_t y = 2; y < height - 2; ++y)
        for (uint32_t x = 2; x < width - 2; ++x)
            EXPECT_NEAR(results[y * width + x][0], 0.5f, 1.0f / 255.0f);
}

TEST_F(ScreenSpaceShadowComputeTest, ResolveDoesNotLeakAcrossDepthSilhouettesOrIntoSky)
{
    constexpr uint32_t width = 33, height = 19;
    std::vector<float> depths(width * height);
    std::vector<uint32_t> input(width * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            depths[y * width + x] = x < 16 ? 0.4f : (x < 27 ? 0.6f : 0.0f);
            input[y * width + x] = x < 16 ? 0u : 255u;
        }
    Run(width, height, depths, {0, 0, 0, -1}, 0.005f, false, input);
    ASSERT_EQ(results.size(), depths.size());
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            EXPECT_FLOAT_EQ(results[y * width + x][0], x < 16 ? 0.0f : 1.0f);
}

TEST_F(ScreenSpaceShadowComputeTest, ResolveSoftensEdgesWithoutErasingThinContactShadows)
{
    constexpr uint32_t width = 33, height = 19;
    std::vector<float> depths(width * height, 0.4f);
    std::vector<uint32_t> input(width * height, 255u);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 15; x <= 17; ++x) input[y * width + x] = 0;
    Run(width, height, depths, {0, 0, 0, -1}, 0.005f, false, input);
    ASSERT_EQ(results.size(), depths.size());
    for (uint32_t y = 0; y < height; ++y)
    {
        const auto at = [&](uint32_t x) { return results[y * width + x][0]; };
        EXPECT_LT(at(16), 0.25f); // A three-pixel contact retains a dark core.
        EXPECT_GT(at(14), at(15));
        EXPECT_GT(at(15), at(16));
        EXPECT_GT(at(14), 0.0f);
        EXPECT_LT(at(14), 1.0f); // The boundary is fractional, not stippled.
        // Two pixels out the filter footprint still reaches the contact, so a
        // lit 3x3 neighborhood alone must not skip the filter.
        EXPECT_GT(at(13), at(14));
        EXPECT_LT(at(13), 1.0f);
        EXPECT_FLOAT_EQ(at(12), 1.0f);
        EXPECT_FLOAT_EQ(at(20), 1.0f);
    }

    // The same contact across rows: the window reaches two rows out as well.
    std::fill(input.begin(), input.end(), 255u);
    for (uint32_t y = 8; y <= 10; ++y)
        for (uint32_t x = 0; x < width; ++x) input[y * width + x] = 0;
    Run(width, height, depths, {0, 0, 0, -1}, 0.005f, false, input);
    ASSERT_EQ(results.size(), depths.size());
    for (uint32_t x = 0; x < width; ++x)
    {
        const auto at = [&](uint32_t y) { return results[y * width + x][0]; };
        EXPECT_GT(at(6), at(7));
        EXPECT_LT(at(6), 1.0f);
        EXPECT_GT(at(12), at(11));
        EXPECT_LT(at(12), 1.0f);
        EXPECT_FLOAT_EQ(at(5), 1.0f);
        EXPECT_FLOAT_EQ(at(13), 1.0f);
    }
}
TEST_F(ScreenSpaceShadowComputeTest, ResolveSmoothsShallowDiagonalContactCoverage)
{
    constexpr uint32_t width = 65, height = 41;
    std::vector<float> depths(width * height);
    std::vector<uint32_t> input(width * height, 255u);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            depths[y * width + x] = 0.3f + x * 0.001f + y * 0.002f;
            const uint32_t edge = 8 + x / 4;
            if (y >= edge && y < edge + 3) input[y * width + x] = 0;
        }
    Run(width, height, depths, {0, 0, 0, -1}, 0.005f, false, input);
    ASSERT_EQ(results.size(), depths.size());
    float previousCenter = 0.0f;
    float squaredStepError = 0.0f;
    uint32_t count = 0;
    for (uint32_t x = 8; x < width - 8; ++x)
    {
        float mass = 0.0f, moment = 0.0f;
        for (uint32_t y = 0; y < height; ++y)
        {
            const float occlusion = 1.0f - results[y * width + x][0];
            mass += occlusion;
            moment += occlusion * (y + 0.5f);
        }
        EXPECT_GT(mass, 2.5f); // Keep the thin shadow's integrated coverage.
        EXPECT_LT(results[(9 + x / 4) * width + x][0], 0.3f);
        const float center = moment / mass;
        if (x > 8)
        {
            const float error = center - previousCenter - 0.25f;
            squaredStepError += error * error;
            ++count;
        }
        previousCenter = center;
    }
    // The isotropic 5x5 resolve leaves a periodic one-pixel staircase whose
    // centroid-step RMS is about 0.088 pixels. Follow the edge without blur
    // across the contact to reconstruct smoother fractional coverage.
    EXPECT_LT(std::sqrt(squaredStepError / count), 0.07f);
}
TEST_F(ScreenSpaceShadowComputeTest, ContinuousBlockerDoesNotFallBetweenThinTraceSamples)
{
    constexpr uint32_t width = 257, height = 97;
    // A depth-continuous wedge whose surface descends toward the ray faster
    // than the thin hit window is wide: between two adjacent samples the ray
    // passes through it without either sample landing inside the window, so
    // only a segment crossing reports the blocker for most receivers.
    std::vector<float> depths(width * height, 0.4f);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width / 2; ++x)
            depths[y * width + x] =
                std::max(0.4005f, 0.42f - float(width / 2 - x) * 0.005f);
    Run(width, height, depths, {-4, 0, 0, -1});
    ASSERT_EQ(results.size(), depths.size());
    // This light projection gives about six pixels of contact coverage.
    // Check its interior rather than assuming a longer physical shadow.
    uint32_t covered = 0, total = 0;
    for (uint32_t y = 8; y < height - 8; ++y)
        for (uint32_t x = width / 2 + 2; x < width / 2 + 6; ++x)
        {
            covered += results[y * width + x][0] < 0.25f;
            ++total;
        }
    EXPECT_GT(covered, total * 9 / 10);
}
} // namespace

TEST_F(ScreenSpaceShadowComputeTest, MatchedPcfFootprintScalesWithProjectionAndQuality)
{
    constexpr uint32_t w = 129, h = 41;
    std::vector<float> depth(w * h, 0.4f);
    std::vector<uint32_t> step(w * h, 255);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) step[y * w + x] = 0;
    for (int quality : {0, 1})
        for (float scale : {2.0f, 4.0f})
        {
            Run(w, h, depth, {-4, 0, 0, -1}, 0.005f, false, step, scale, 0, quality);
            ASSERT_EQ(results.size(), depth.size());
            const int halfSize = quality == 0 ? 2 : 1;
            // One map texel projects to scale screen pixels. The lit side
            // remains partially shadowed until the same PCF support ends.
            EXPECT_LT(results[20 * w + w / 2 + int(scale * halfSize) - 1][0], 0.95f);
            EXPECT_FLOAT_EQ(results[20 * w + w / 2 + int(scale * halfSize) + 1][0], 1);
            EXPECT_FLOAT_EQ(results[20 * w + w / 2 - int(scale * halfSize) - 2][0], 0);
            for (uint32_t x = 1; x < w; ++x)
                EXPECT_GE(results[20 * w + x][0], results[20 * w + x - 1][0]);
        }
}

TEST_F(ScreenSpaceShadowComputeTest, MatchedPcfDoesNotBorrowAcrossReceiverSilhouettes)
{
    constexpr uint32_t w = 65, h = 41;
    std::vector<float> depth(w * h, 0.4f);
    std::vector<uint32_t> visibility(w * h, 255);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w / 2; ++x)
        {
            depth[y * w + x] = 0.5f;
            visibility[y * w + x] = 0;
        }
    for (float angle : {0.0f, 0.5f, 1.1f})
    {
        Run(w, h, depth, {-4, 0, 0, -1}, 0.005f, false, visibility, 3, angle);
        ASSERT_EQ(results.size(), depth.size());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = w / 2; x < w; ++x) EXPECT_FLOAT_EQ(results[y * w + x][0], 1);
    }
}

TEST_F(ScreenSpaceShadowComputeTest, MatchedPcfFadesContinuouslyAtGrazingFootprintLimit)
{
    constexpr uint32_t w = 129, h = 41;
    std::vector<float> depth(w * h, 0.4f);
    std::vector<uint32_t> step(w * h, 255);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) step[y * w + x] = 0;
    Run(w, h, depth, {-4, 0, 0, -1}, 0.005f, false, step, 23.9f);
    const auto before = results;
    Run(w, h, depth, {-4, 0, 0, -1}, 0.005f, false, step, 24.1f);
    ASSERT_EQ(results.size(), before.size());
    for (size_t i = 0; i < results.size(); ++i)
        EXPECT_NEAR(results[i][0], before[i][0], 1.0f / 255);
}

TEST_F(ScreenSpaceShadowComputeTest, ReceiverPlanePcfKeepsSlopesLitWithoutLosingNearbyBlockers)
{
    std::array<uint32_t,4> values{};
    auto result = Buffer(sizeof(values), BufferUsage::Storage, BufferMemoryUsage::Readback, values.data());
    DescriptorSetDesc desc{}; desc.layout = planeLayout;
    auto set = device->CreateDescriptorSet(desc); sets.push_back(set);
    device->UpdateStorageBufferBinding(set, 0, result, 0, sizeof(values));
    auto cmd = device->CreateCommandList(IDevice::QueueType::Graphics);
    cmd->Begin(); cmd->SetPipeline(planePipeline); cmd->BindDescriptorSet(0,set,planePipeline);
    cmd->Dispatch(1,1,1); cmd->End(); device->ExecuteCommandLists({cmd.get()}); device->WaitForIdle();
    auto* mapped = device->MapBuffer(result); ASSERT_NE(mapped,nullptr);
    std::memcpy(values.data(),mapped,sizeof(values)); device->UnmapBuffer(result);
    EXPECT_LT(values[0],100u); // Center-only comparison falsely shadows this plane.
    EXPECT_EQ(values[1],100u); // Every receiver sample is lit with plane correction.
    EXPECT_EQ(values[2],0u);   // Nearby real blockers still occlude every sample.
    EXPECT_EQ(values[3],1u);   // A singular projection has a finite fallback.
}
