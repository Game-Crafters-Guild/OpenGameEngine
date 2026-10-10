// Run the production reflection gather and diffuse sample against a depth atlas
// whose two moment pairs disagree. The bounded pair (.rg) occludes the lower
// probe layer and the true-distance pair (.ba) the upper one; each layer's
// atlases carry one colour, so the colour a gather returns names the pair it
// read. Reflections must read the true distance: bounded moments put every wall
// past the local bound at the bound, which rejects probes a mirror needs.
// Diffuse sampling must read the bounded pair, which is what stops a distant
// hit from inflating the moments of a nearby occluder.
#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
using Vec4 = std::array<float, 4>;

// Mirrors DDGIVolumeData in Includes/ddgi_probes.glsl (std140).
struct VolumeData
{
    Vec4 GridMinWS{0, 0, 0, 0};
    Vec4 GridSizeWS{2, 2, 2, 0};
    std::array<int32_t, 4> ProbeCount{2, 2, 2, 8};
    Vec4 Params0{2, 0, 1, 1};  // min cell, no surface bias, full Chebyshev, full classify
    Vec4 Params1{1, 1, 1, 1};  // enabled, intensity, reflections enabled, reflection intensity
    std::array<int32_t, 4> Params2{0, 0, 0, 0};  // no glossy atlas
    std::array<int32_t, 4> Params3{0, 6, 0, 0};  // depth moments at the shared tile resolution
};

// Mirrors MomentCase in Tests/Shaders/ddgi_moment_pairs_test.comp.
struct MomentCase
{
    Vec4 PositionWS;
    Vec4 Direction;
};

constexpr uint32_t kTile = 8;  // GE_DDGI_OCT_RES + 2 * GE_DDGI_BORDER
constexpr uint32_t kAtlasWidth = 2 * kTile;
constexpr uint32_t kAtlasHeight = 2 * 2 * kTile;
constexpr uint32_t kProbes = 8;

// Moments of a surface far past the receiver, and of one well short of it.
constexpr Vec4 kClearOverOccluded{10.0f, 100.0f, 0.5f, 0.25f};
constexpr Vec4 kOccludedOverClear{0.5f, 0.25f, 10.0f, 100.0f};
constexpr Vec4 kLowerLayerColour{1, 0, 0, 1};
constexpr Vec4 kUpperLayerColour{0, 1, 0, 1};

constexpr std::array<std::pair<uint32_t, DescriptorType>, 14> kBindings{{
    {0, DescriptorType::StorageBuffer},         {1, DescriptorType::StorageBuffer},
    {29, DescriptorType::CombinedImageSampler}, {30, DescriptorType::UniformBuffer},
    {31, DescriptorType::CombinedImageSampler}, {32, DescriptorType::StorageBuffer},
    {33, DescriptorType::CombinedImageSampler}, {34, DescriptorType::UniformBuffer},
    {35, DescriptorType::CombinedImageSampler}, {36, DescriptorType::StorageBuffer},
    {37, DescriptorType::CombinedImageSampler}, {38, DescriptorType::CombinedImageSampler},
    {39, DescriptorType::CombinedImageSampler}, {40, DescriptorType::CombinedImageSampler},
}};

// One value per probe tile, laid out as GE_DDGIProbeTileOriginRes addresses it.
std::vector<Vec4> LayeredAtlas(const Vec4& lowerLayer, const Vec4& upperLayer)
{
    std::vector<Vec4> texels(kAtlasWidth * kAtlasHeight);
    for (uint32_t probe = 0; probe < kProbes; ++probe)
    {
        const uint32_t x = probe % 2;
        const uint32_t y = (probe / 2) % 2;
        const uint32_t z = probe / 4;
        for (uint32_t ty = 0; ty < kTile; ++ty)
            for (uint32_t tx = 0; tx < kTile; ++tx)
                texels[((y + z * 2) * kTile + ty) * kAtlasWidth + x * kTile + tx] = y == 0 ? lowerLayer : upperLayer;
    }
    return texels;
}

class DDGIMomentPairComputeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "No Vulkan device available";
        std::ifstream input(GameEngine::TestPaths::StagedRoot() / "Shaders" / "ddgi_moment_pairs_test.comp.spv",
                            std::ios::binary);
        ASSERT_TRUE(input.good()) << "ddgi_moment_pairs_test.comp.spv is not staged under <build>/Shaders";
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(input),
                                                                 std::istreambuf_iterator<char>());
        for (const auto& [binding, type] : kBindings)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = type;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = bytes;
        desc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        desc.DebugName = "DDGI.MomentPairs";
        pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(pipeline.IsValid());
        sampler = device->CreateSampler(SamplerDesc::PointClamp("DDGI.MomentPairs.Atlas"));
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

    BufferHandle Buffer(const void* data, uint64_t bytes, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        desc.debugName = "DDGI.MomentPairs.Buffer";
        auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        EXPECT_TRUE(buffer.IsValid());
        if (data && buffer.IsValid())
        {
            auto* mapped = device->MapBuffer(buffer);
            EXPECT_NE(mapped, nullptr);
            if (mapped)
            {
                std::memcpy(mapped, data, bytes);
                device->UnmapBuffer(buffer);
            }
        }
        return buffer;
    }

    TextureHandle Upload(CommandList& commands, const std::vector<Vec4>& texels)
    {
        TextureDesc td{};
        td.width = kAtlasWidth;
        td.height = kAtlasHeight;
        td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.debugName = "DDGI.MomentPairs.Atlas";
        const auto texture = device->CreateTexture(td);
        textures.push_back(texture);
        EXPECT_TRUE(texture.IsValid());
        const auto staging = Buffer(texels.data(), texels.size() * sizeof(Vec4), BufferUsage::TransferSrc,
                                    BufferMemoryUsage::Upload);
        commands.CopyBufferToTextureSubresource(staging, texture, 0, 0, kAtlasWidth, kAtlasHeight);
        commands.Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                               ResourceState::ShaderResource));
        return texture;
    }

    // Returns {rough reflection lobe, diffuse irradiance} at the receiver.
    std::array<Vec4, 2> Run(const Vec4& position, const Vec4& direction)
    {
        const VolumeData volume;
        VolumeData fineCascade;  // bound because the include declares it; disabled, never sampled
        fineCascade.Params1 = {0, 0, 0, 0};
        const std::vector<Vec4> probeState(kProbes, Vec4{0, 0, 0, 1});
        const MomentCase moment{position, direction};
        constexpr uint64_t kResultBytes = 2 * sizeof(Vec4);

        const auto volumeBuffer = Buffer(&volume, sizeof(volume), BufferUsage::Uniform, BufferMemoryUsage::Upload);
        const auto fineBuffer =
            Buffer(&fineCascade, sizeof(fineCascade), BufferUsage::Uniform, BufferMemoryUsage::Upload);
        const auto stateBuffer = Buffer(probeState.data(), probeState.size() * sizeof(Vec4), BufferUsage::Storage,
                                        BufferMemoryUsage::Upload);
        const auto caseBuffer = Buffer(&moment, sizeof(moment), BufferUsage::Storage, BufferMemoryUsage::Upload);
        const auto resultBuffer = Buffer(nullptr, kResultBytes, BufferUsage::Storage, BufferMemoryUsage::Readback);

        auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        const auto depth = Upload(*commands, LayeredAtlas(kOccludedOverClear, kClearOverOccluded));
        const auto colour = Upload(*commands, LayeredAtlas(kLowerLayerColour, kUpperLayerColour));

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = layout;
        const auto ds = device->CreateDescriptorSet(dsDesc);
        device->UpdateStorageBufferBinding(ds, 0, caseBuffer, 0, sizeof(moment));
        device->UpdateStorageBufferBinding(ds, 1, resultBuffer, 0, kResultBytes);
        device->UpdateBufferBinding(ds, 30, volumeBuffer, 0, sizeof(volume));
        device->UpdateBufferBinding(ds, 34, fineBuffer, 0, sizeof(fineCascade));
        for (uint32_t binding : {32u, 36u})
            device->UpdateStorageBufferBinding(ds, binding, stateBuffer, 0, probeState.size() * sizeof(Vec4));
        for (uint32_t binding : {31u, 35u})
            device->UpdateCombinedImageSamplerBinding(ds, binding, depth, sampler);
        for (uint32_t binding : {29u, 33u, 37u, 38u, 39u, 40u})
            device->UpdateCombinedImageSamplerBinding(ds, binding, colour, sampler);
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, ds, pipeline);
        commands->Dispatch(1, 1, 1);
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();

        std::array<Vec4, 2> results{};
        auto* mapped = device->MapBuffer(resultBuffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            std::memcpy(results.data(), mapped, sizeof(results));
            device->UnmapBuffer(resultBuffer);
        }
        return results;
    }

    std::unique_ptr<IDevice> device;
    DescriptorSetLayoutDesc layout{};
    PipelineHandle pipeline{};
    SamplerHandle sampler{};
    std::vector<TextureHandle> textures;
    std::vector<BufferHandle> buffers;
};

TEST_F(DDGIMomentPairComputeTest, ReflectionsReadTheTrueDistanceAndDiffuseTheBoundedPair)
{
    // The receiver sits at the centre of a 2 m cell, equally far (sqrt 3) from
    // all eight probes and equally weighted by the trilinear term, so only the
    // visibility weights separate the two layers.
    const auto [reflection, diffuse] = Run({1, 1, 1, 0}, {1, 0, 0, 0});

    // True distance: the lower layer is clear and the upper occluded.
    EXPECT_GT(reflection[0], 0.99f) << "the reflection gather did not read the true-distance moments (.ba)";
    EXPECT_LT(reflection[1], 0.01f);
    // Bounded pair: the lower layer is occluded and the upper clear.
    EXPECT_GT(diffuse[1], 0.9f) << "diffuse sampling did not read the bounded moments (.rg)";
    EXPECT_LT(diffuse[0], 0.1f);
}
}  // namespace
