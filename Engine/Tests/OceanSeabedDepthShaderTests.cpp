#include <gtest/gtest.h>

#include "Ocean/OceanTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
// Set 2 binding 1 is the OceanParamsBuffer the ocean draw binds by name.
constexpr uint32_t kOceanParamsSet = 2;
constexpr uint32_t kOceanParamsBinding = 1;

// The probe reads one seabed-depth texel, so the cascade texture is the
// smallest the sampler accepts; every texel carries the same depth.
constexpr uint32_t kProbeCascadeExtent = 2;
constexpr float kProbeSeabedDepthMeters = 2.75f;
// OceanSampleSeabedDepth's feature-unavailable return (ocean_common.glsl).
constexpr float kSeabedDepthUnavailableMeters = 1.0e6f;

std::vector<uint8_t> LoadProbe()
{
    // The normal shader target compiles/stages this source alongside the other
    // compute probes. Missing SPIR-V is a build error, not a reason to skip.
    return Utils::ReadFile((TestPaths::StagedRoot() / "Shaders" /
                            "ocean_seabed_depth_probe.comp.spv").string());
}

class OceanSeabedDepthShaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_View.IsValid())
            m_Device->DestroyTextureView(m_View);
        if (m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        for (const auto buffer : m_Buffers)
        {
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        }
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(size_t size, BufferUsage usage, const void* data)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (buffer.IsValid())
        {
            void* mapped = m_Device->MapBuffer(buffer);
            EXPECT_NE(mapped, nullptr);
            if (mapped)
                std::memcpy(mapped, data, size);
            m_Device->UnmapBuffer(buffer);
        }
        return buffer;
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<BufferHandle> m_Buffers;
    TextureHandle m_Texture{};
    TextureViewHandle m_View{};
    SamplerHandle m_Sampler{};
};
} // namespace

TEST(OceanSeabedDepthShaderLayout, ReflectedTypesAndOffsetsMatchUploadedParameters)
{
    const auto bytes = LoadProbe();
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.size() % sizeof(uint32_t), 0u);
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    StageReflectionResult stage{};
    ReflectionOptions options{};
    std::string error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, words.data(), words.size(),
                            options, stage, &error)) << error;
    const auto meta = MergeStages({stage});
    const BlockLayout* block = nullptr;
    for (const auto& set : meta.Sets)
    {
        if (set.Set != kOceanParamsSet)
            continue;
        for (const auto& binding : set.Bindings)
            if (binding.Binding == kOceanParamsBinding && binding.Block)
                block = &*binding.Block;
    }
    ASSERT_NE(block, nullptr);

    // The block is one packed mirror of OceanParamsGPU: every member starts
    // where the previous one ended, and the runtime-sized tail starts at the CPU
    // struct's Waves offset. A member added, removed or resized on one side
    // alone breaks the walk.
    uint32_t cursor = 0;
    const Member* waves = nullptr;
    for (const auto& member : block->Members)
    {
        if (member.Name == "uWaves")
        {
            waves = &member;
            break;
        }
        EXPECT_EQ(member.Offset, cursor) << member.Name;
        cursor += member.Size;
    }
    ASSERT_NE(waves, nullptr);
    EXPECT_EQ(cursor, offsetof(OceanParamsGPU, Waves));
    EXPECT_EQ(waves->Offset, offsetof(OceanParamsGPU, Waves));
    ASSERT_TRUE(waves->ArrayStride.has_value());
    EXPECT_EQ(*waves->ArrayStride, sizeof(OceanParamsGPU{}.Waves[0]));

    // Every field the CPU uploads as uint32 is declared unsigned in the block.
    // Read through a float lane an uploaded 1 arrives as a denormal, so the flag
    // reads false and its feature stays off with no diagnostic anywhere.
    const std::vector<uint32_t> uploadedAsUnsigned = {
        offsetof(OceanParamsGPU, GerstnerWaveCount),
        offsetof(OceanParamsGPU, WaveMode),
        offsetof(OceanParamsGPU, FFTCascadeCount),
        offsetof(OceanParamsGPU, PlanarReflections),
        offsetof(OceanParamsGPU, RefractionAvailable),
        offsetof(OceanParamsGPU, RefractionDepthMatched),
        offsetof(OceanParamsGPU, SeabedDepthAvailable),
        offsetof(OceanParamsGPU, CausticsAvailable),
        offsetof(OceanParamsGPU, Underwater),
        offsetof(OceanParamsGPU, FlowAvailable),
        offsetof(OceanParamsGPU, DynamicWavesAvailable),
        offsetof(OceanParamsGPU, ClipAvailable),
        offsetof(OceanParamsGPU, AlbedoAvailable),
        offsetof(OceanParamsGPU, NormalTextureAvailable),
        offsetof(OceanParamsGPU, FoamTextureAvailable),
        offsetof(OceanParamsGPU, CausticsTextureAvailable),
        offsetof(OceanParamsGPU, PlanarReflectionAvailable),
        offsetof(OceanParamsGPU, CombineWavesAvailable),
        offsetof(OceanParamsGPU, FoamDebugMode),
        offsetof(OceanParamsGPU, WaveMaskAvailable),
        offsetof(OceanParamsGPU, LocalFFTAvailable),
    };
    std::vector<uint32_t> declaredUnsigned;
    for (const auto& member : block->Members)
    {
        if (member.Type.Base != BaseType::UInt)
            continue;
        const uint32_t lanes =
            member.Type.Kind == TypeKind::Vector ? member.Type.VecSize : 1u;
        for (uint32_t lane = 0; lane < lanes; ++lane)
            declaredUnsigned.push_back(member.Offset + lane * sizeof(uint32_t));
    }
    std::sort(declaredUnsigned.begin(), declaredUnsigned.end());
    std::vector<uint32_t> expectedUnsigned = uploadedAsUnsigned;
    std::sort(expectedUnsigned.begin(), expectedUnsigned.end());
    EXPECT_EQ(declaredUnsigned, expectedUnsigned);
}

TEST_F(OceanSeabedDepthShaderTest, UploadedUintFlagControlsProductionDepthSample)
{
    const auto bytes = LoadProbe();
    ASSERT_FALSE(bytes.empty());
    DescriptorSetLayoutDesc outputLayout{}, emptyLayout{}, oceanLayout{};
    auto binding = [](uint32_t slot, DescriptorType type) {
        DescriptorBinding value{};
        value.binding = slot;
        value.type = type;
        value.count = 1;
        value.shaderStages = kShaderStageCompute;
        return value;
    };
    outputLayout.bindings = {binding(0, DescriptorType::StorageBuffer)};
    oceanLayout.bindings = {binding(1, DescriptorType::StorageBuffer),
                           binding(4, DescriptorType::UniformBuffer),
                           binding(6, DescriptorType::CombinedImageSampler)};
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(bytes);
    for (const auto& layout : {outputLayout, emptyLayout, oceanLayout})
        pipelineDesc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    const auto pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipelineDesc));
    ASSERT_TRUE(pipeline.IsValid());

    TextureDesc textureDesc{};
    textureDesc.width = textureDesc.height = kProbeCascadeExtent;
    textureDesc.depth = textureDesc.mipLevels = textureDesc.arrayLayers = textureDesc.sampleCount = 1;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    m_Texture = m_Device->CreateTexture(textureDesc);
    ASSERT_TRUE(m_Texture.IsValid());
    TextureViewDesc viewDesc{};
    viewDesc.viewType = TextureViewType::View2DArray;
    viewDesc.levelCount = viewDesc.layerCount = 1;
    m_View = m_Device->CreateTextureView(m_Texture, viewDesc);
    ASSERT_TRUE(m_View.IsValid());
    m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("SeabedProbe"));
    ASSERT_TRUE(m_Sampler.IsValid());
    constexpr std::array<float, kProbeCascadeExtent * kProbeCascadeExtent> depths{
        kProbeSeabedDepthMeters, kProbeSeabedDepthMeters, kProbeSeabedDepthMeters,
        kProbeSeabedDepthMeters};
    const auto upload = MakeBuffer(sizeof(depths), BufferUsage::TransferSrc, depths.data());
    ASSERT_TRUE(upload.IsValid());
    auto copy = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    copy->Begin();
    copy->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::Undefined, ResourceState::CopyDest));
    copy->CopyBufferToTextureSubresource(upload, m_Texture, 0, 0, kProbeCascadeExtent,
                                         kProbeCascadeExtent);
    copy->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
    copy->End();
    m_Device->ExecuteCommandLists({copy.get()});
    m_Device->WaitForIdle();

    // Write the real CPU structure, never a test-specific packed mirror. The
    // adjacent fields and tail prove that repairing the type did not move data.
    OceanParamsGPU params{};
    params.DepthFogFalloffMode = 2.0f;
    params.CausticsScale = 3.25f;
    params.Waves[0].Amplitude = 7.5f;
    OceanSampledCascadeLayoutsGPU cascades{};
    OceanCascadeLayoutGPU& cascade = cascades[OceanSampledCascade::SeabedDepth];
    cascade.CascadeOriginScale[0][2] = 1.0f;
    const auto paramsBuffer = MakeBuffer(sizeof(params), BufferUsage::Storage, &params);
    const auto cascadeBuffer = MakeBuffer(sizeof(cascades), BufferUsage::Uniform, &cascades);
    const std::array<float, 4> poison{-17, -17, -17, -17};
    const auto outputBuffer = MakeBuffer(sizeof(poison), BufferUsage::Storage, poison.data());
    ASSERT_TRUE(paramsBuffer.IsValid());
    ASSERT_TRUE(cascadeBuffer.IsValid());
    ASSERT_TRUE(outputBuffer.IsValid());
    DescriptorSetDesc descriptor{};
    descriptor.layout = outputLayout;
    const auto outputSet = m_Device->CreateDescriptorSet(descriptor);
    descriptor.layout = oceanLayout;
    const auto oceanSet = m_Device->CreateDescriptorSet(descriptor);
    ASSERT_TRUE(outputSet.IsValid());
    ASSERT_TRUE(oceanSet.IsValid());
    m_Device->UpdateStorageBufferBinding(outputSet, 0, outputBuffer, 0, sizeof(poison));
    m_Device->UpdateStorageBufferBinding(oceanSet, 1, paramsBuffer, 0, sizeof(params));
    m_Device->UpdateBufferBinding(oceanSet, 4, cascadeBuffer, 0, sizeof(cascades));
    m_Device->UpdateCombinedImageSamplerBinding(oceanSet, 6, m_View, m_Sampler);

    struct Case { uint32_t available; uint32_t lodCount; float expected; };
    for (const Case test : {Case{0, 1, kSeabedDepthUnavailableMeters},
                            Case{1, 1, kProbeSeabedDepthMeters},
                            Case{0, 1, kSeabedDepthUnavailableMeters},
                            Case{1, 0, kSeabedDepthUnavailableMeters}})
    {
        SCOPED_TRACE(::testing::Message() << "available=" << test.available << " lodCount=" << test.lodCount);
        params.SeabedDepthAvailable = test.available;
        cascade.LodCount = test.lodCount;
        m_Device->UpdateBuffer(paramsBuffer, 0, sizeof(params), &params);
        m_Device->UpdateBuffer(cascadeBuffer, 0, sizeof(cascades), &cascades);
        m_Device->UpdateBuffer(outputBuffer, 0, sizeof(poison), poison.data());
        auto command = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->SetPipeline(pipeline);
        command->BindDescriptorSet(0, outputSet, pipeline);
        command->BindDescriptorSet(2, oceanSet, pipeline);
        command->Dispatch(1, 1, 1);
        command->End();
        m_Device->ExecuteCommandLists({command.get()});
        m_Device->WaitForIdle();
        std::array<float, 4> result{};
        void* mapped = m_Device->MapBuffer(outputBuffer);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(result.data(), mapped, sizeof(result));
        m_Device->UnmapBuffer(outputBuffer);
        EXPECT_FLOAT_EQ(result[0], test.expected);
        EXPECT_FLOAT_EQ(result[1], params.DepthFogFalloffMode);
        EXPECT_FLOAT_EQ(result[2], params.CausticsScale);
        EXPECT_FLOAT_EQ(result[3], params.Waves[0].Amplitude);
    }
}
