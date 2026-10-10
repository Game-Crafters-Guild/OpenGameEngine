// TextureService must hand out the sampler ResolveSamplerPreset describes on the
// device it runs on. A compute probe samples a texture whose mip levels each hold
// one constant, through a footprint kFootprintTexels long and one texel wide, so
// the value it reads back names the level the sampler chose: isotropic trilinear
// filtering takes the level of the long axis, anisotropic filtering the level of
// the long axis divided by the anisotropy the sampler allows. The death test holds
// the other side of the contract: a profile that requests more anisotropy than the
// device allows stops at sampler creation instead of reaching the driver.
#include <gtest/gtest.h>

#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kTextureSize = 64;
constexpr uint32_t kMipLevels = 7; // 64 down to 1
// Level L holds L * kLevelStep in every channel, so a trilinear blend between two
// levels reads back as the fractional level itself.
constexpr uint32_t kLevelStep = 32;
// The footprint is 16 texels along U and one along V: an isotropic sampler reads
// level log2(16) = 4, a sampler allowed 16x reads level 0 and one allowed 8x level 1.
constexpr float kFootprintTexels = 16.0f;
// Half a level: wide enough for a hardware's level-of-detail rounding, narrow
// enough that neighbouring anisotropy settings (8x against 16x) never overlap.
constexpr float kLevelTolerance = 0.5f;

constexpr std::array<std::pair<uint32_t, DescriptorType>, 3> kBindings{{
    {0, DescriptorType::CombinedImageSampler},
    {1, DescriptorType::StorageBuffer},
    {2, DescriptorType::StorageBuffer},
}};

// Mirrors ProbeFootprint in Tests/Shaders/sampler_footprint_probe.comp.
struct ProbeFootprint
{
    std::array<float, 4> Centre;
    std::array<float, 4> Gradients;
};

float ExpectedLevel(float anisotropy)
{
    return std::log2(kFootprintTexels / std::min(kFootprintTexels, anisotropy));
}

class TextureServiceSamplerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "No Vulkan device available";
        profile = RendererProfile::FromCapabilities(device->GetCapabilities());
        ASSERT_TRUE(textures.Initialize(device.get(), registry, profile));
        texturesInitialized = true;

        std::ifstream input(GameEngine::TestPaths::StagedRoot() / "Shaders" / "sampler_footprint_probe.comp.spv",
                            std::ios::binary);
        ASSERT_TRUE(input.good()) << "sampler_footprint_probe.comp.spv is not staged under <build>/Shaders";
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
        desc.DebugName = "SamplerFootprintProbe";
        pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(desc));
        ASSERT_TRUE(pipeline.IsValid());

        CreateLevelTexture();
        ASSERT_TRUE(levelTexture.IsValid());
    }

    void TearDown() override
    {
        if (!device)
            return;
        device->WaitForIdle();
        for (auto buffer : buffers)
            device->DestroyBuffer(buffer);
        if (levelTexture.IsValid())
            device->DestroyTexture(levelTexture);
        if (texturesInitialized)
            textures.Shutdown();
        device->Shutdown();
    }

    void CreateLevelTexture()
    {
        TextureDesc td{};
        td.width = td.height = kTextureSize;
        td.mipLevels = kMipLevels;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.debugName = "SamplerFootprintProbe.Levels";
        levelTexture = device->CreateTexture(td);
        if (!levelTexture.IsValid())
            return;

        std::array<std::vector<uint8_t>, kMipLevels> pixels;
        std::array<TextureMipUploadEntry, kMipLevels> mips{};
        for (uint32_t level = 0; level < kMipLevels; ++level)
        {
            const uint32_t size = kTextureSize >> level;
            pixels[level].assign(static_cast<size_t>(size) * size * 4, static_cast<uint8_t>(level * kLevelStep));
            mips[level] = {pixels[level].data(), pixels[level].size(), size, size};
        }
        UploadTextureMips(device.get(), levelTexture, mips.data(), kMipLevels, TextureFormat::RGBA8_UNORM,
                          "SamplerFootprintProbe.Staging");
        device->WaitForIdle();
    }

    BufferHandle Buffer(const void* data, uint64_t bytes, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        desc.memoryUsage = memory;
        desc.debugName = "SamplerFootprintProbe.Buffer";
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

    // The mip level `sampler` selects for the probe footprint.
    float SampledLevel(SamplerHandle sampler)
    {
        constexpr float kTexel = 1.0f / static_cast<float>(kTextureSize);
        const ProbeFootprint footprint{{0.5f, 0.5f, 0.0f, 0.0f}, {kFootprintTexels * kTexel, 0.0f, 0.0f, kTexel}};
        constexpr uint64_t kResultBytes = 4 * sizeof(float);
        const auto footprintBuffer = Buffer(&footprint, sizeof(footprint), BufferMemoryUsage::Upload);
        const auto resultBuffer = Buffer(nullptr, kResultBytes, BufferMemoryUsage::Readback);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = layout;
        const auto ds = device->CreateDescriptorSet(dsDesc);
        device->UpdateCombinedImageSamplerBinding(ds, 0, levelTexture, sampler);
        device->UpdateStorageBufferBinding(ds, 1, footprintBuffer, 0, sizeof(footprint));
        device->UpdateStorageBufferBinding(ds, 2, resultBuffer, 0, kResultBytes);

        auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, ds, pipeline);
        commands->Dispatch(1, 1, 1);
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();
        device->DestroyDescriptorSet(ds);

        std::array<float, 4> sampled{};
        auto* mapped = device->MapBuffer(resultBuffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            std::memcpy(sampled.data(), mapped, sizeof(sampled));
            device->UnmapBuffer(resultBuffer);
        }
        return sampled[0] * 255.0f / static_cast<float>(kLevelStep);
    }

    std::unique_ptr<IDevice> device;
    RendererProfile profile{};
    MaterialRegistry registry;
    TextureService textures;
    bool texturesInitialized = false;
    DescriptorSetLayoutDesc layout{};
    PipelineHandle pipeline{};
    TextureHandle levelTexture{};
    std::vector<BufferHandle> buffers;
};

TEST_F(TextureServiceSamplerTest, MaterialSamplerFiltersWithTheResolvedAnisotropy)
{
    const float anisotropy = ResolveSamplerPreset(SamplerPreset::LinearRepeat, profile).maxAnisotropy;
    if (anisotropy < 2.0f)
        GTEST_SKIP() << "The device allows no anisotropic filtering (maxSamplerAnisotropy "
                     << device->GetCapabilities().maxSamplerAnisotropy
                     << "), so the probe cannot tell the material sampler from an isotropic one";

    // Control: an isotropic trilinear sampler must read the long axis's level, or the
    // probe cannot separate the filters here (a driver-wide anisotropy override
    // makes every sampler anisotropic).
    const SamplerHandle isotropic =
        device->CreateSampler(SamplerDesc::MaterialLinearRepeat("SamplerFootprintProbe.Isotropic"));
    ASSERT_TRUE(isotropic.IsValid());
    const float isotropicLevel = SampledLevel(isotropic);
    device->DestroySampler(isotropic);
    RecordProperty("IsotropicLevel", std::to_string(isotropicLevel));
    ASSERT_NEAR(isotropicLevel, ExpectedLevel(1.0f), kLevelTolerance)
        << "An isotropic sampler did not read the long axis's level; a driver setting that forces "
           "anisotropic filtering leaves this probe unable to see the material sampler's anisotropy";

    const float materialLevel = SampledLevel(textures.GetSampler(SamplerPreset::LinearRepeat));
    RecordProperty("ResolvedAnisotropy", std::to_string(anisotropy));
    RecordProperty("MaterialLevel", std::to_string(materialLevel));
    EXPECT_NEAR(materialLevel, ExpectedLevel(anisotropy), kLevelTolerance)
        << "GetSampler(LinearRepeat) did not filter with the resolved " << anisotropy << "x anisotropy";
}

// The clamp-to-edge colour sampler a terrain's Planar basemap reads through must filter with the
// same anisotropy as the material sampler beside it. The isotropic LinearClamp, which heightmaps,
// atlases and LUTs read as data, picked level 4 for this footprint where the material sampler
// picks level 0: a basemap four mips softer than its neighbours at a grazing angle.
TEST_F(TextureServiceSamplerTest, ClampAnisotropicSamplerFiltersWithTheResolvedAnisotropy)
{
    // The profile's material anisotropy, not the preset's resolution: the expectation must not come
    // from the code under test.
    const float anisotropy = profile.MaterialSamplerAnisotropy;
    if (anisotropy < 2.0f)
        GTEST_SKIP() << "The device allows no anisotropic filtering (maxSamplerAnisotropy "
                     << device->GetCapabilities().maxSamplerAnisotropy
                     << "), so the probe cannot tell the sampler from an isotropic one";

    const float dataLevel = SampledLevel(textures.GetSampler(SamplerPreset::LinearClamp));
    RecordProperty("LinearClampLevel", std::to_string(dataLevel));
    ASSERT_NEAR(dataLevel, ExpectedLevel(1.0f), kLevelTolerance)
        << "The isotropic LinearClamp did not read the long axis's level; a driver setting that "
           "forces anisotropic filtering leaves this probe unable to see the difference";

    const float clampLevel = SampledLevel(textures.GetSampler(SamplerPreset::LinearClampAnisotropic));
    RecordProperty("LinearClampAnisotropicLevel", std::to_string(clampLevel));
    EXPECT_NEAR(clampLevel, ExpectedLevel(anisotropy), kLevelTolerance)
        << "GetSampler(LinearClampAnisotropic) did not filter with the resolved " << anisotropy
        << "x anisotropy";
}

#if !defined(NDEBUG)
TEST(TextureServiceSamplerDeathTest, AnisotropyAboveTheDeviceLimitStopsAtSamplerCreation)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto profile = RendererProfile::FromCapabilities(device->GetCapabilities());
    profile.MaterialSamplerAnisotropy = 2.0f * device->GetCapabilities().maxSamplerAnisotropy;
    MaterialRegistry registry;
    TextureService textures;
    // Bindless creates every preset in Initialize; the classic path creates the
    // material sampler on its first GetSampler.
    ASSERT_DEATH(
        {
            textures.Initialize(device.get(), registry, profile);
            textures.GetSampler(SamplerPreset::LinearRepeat);
        },
        "SamplerDesc::maxAnisotropy outside");
    device->Shutdown();
}
#endif
} // namespace
