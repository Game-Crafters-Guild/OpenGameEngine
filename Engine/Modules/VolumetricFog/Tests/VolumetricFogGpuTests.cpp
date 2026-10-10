#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
using Vec4 = std::array<float, 4>;
constexpr uint32_t kSize = 8;
constexpr size_t kVoxelCount = kSize * kSize * kSize;

// The production shaders are dispatched directly, with controlled scene inputs.
// These inputs deliberately exercise nonzero light/atlas indices and local
// volume blending; a test of only light zero would miss a packed-array stride bug.
struct PackedLight
{
    uint32_t meta[4]{};
    Vec4 posRange{}, dirIntensity{}, colorAreaWidth{}, areaParams{}, spotParams{}, areaRight{}, areaUp{};
    int32_t shadowSlots[4]{-1, -1, -1, -1};
};
static_assert(sizeof(PackedLight) == 144);
struct Lights
{
    uint32_t header[4]{2, 0, 0, 0};
    PackedLight lights[2]{};
};
struct LocalVolume
{
    Vec4 centerShape{0, 0, 0.5f, 0};
    Vec4 axisX{1, 0, 0, 2}, axisY{0, 1, 0, 2}, axisZ{0, 0, 1, 2};
    Vec4 params{0, 1, 0.1f, 0};
    Vec4 albedo{1, 1, 1, 0}, emission{};
    Vec4 gradientLow{1, 1, 1, 0}, gradientHigh{1, 1, 1, 0}, extra{};
};
struct Volumes
{
    uint32_t header[4]{2, 0, 0, 0};
    LocalVolume volumes[2]{};
};
struct SpotShadow
{
    float vp[16]{};
    Vec4 params{1, 1, 0, 0}, extra{8, 0.01f, 10, 0};
};
struct PointSlot
{
    float vp[6][16]{};
    Vec4 params{}, extra{};
};
static_assert(sizeof(SpotShadow) == 96);
static_assert(sizeof(PointSlot) == 416);

void Identity(float* matrix)
{
    for (int i = 0; i < 16; ++i)
        matrix[i] = i % 5 == 0 ? 1.0f : 0.0f;
}

float DecodeHalf(uint16_t bits)
{
    const int exponent = (bits >> 10) & 31;
    const int mantissa = bits & 1023;
    const float sign = bits & 0x8000 ? -1.0f : 1.0f;
    if (exponent == 31)
        return mantissa ? NAN : sign * INFINITY;
    return sign * (exponent == 0 ? std::ldexp(float(mantissa), -24)
                                : std::ldexp(float(1024 + mantissa), exponent - 25));
}

class VolumetricFogGpu : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
#if defined(__APPLE__)
        desc.preferredAPI = GraphicsAPI::Metal;
#else
        desc.preferredAPI = GraphicsAPI::Vulkan;
#endif
        desc.enableSwapchain = false;
        device = DeviceFactory::CreateDevice(desc);
        if (!device || !device->Initialize(desc))
            GTEST_SKIP() << "No headless graphics device available";
        linear = device->CreateSampler(SamplerDesc::MaterialLinearClamp("FogTest.Linear"));
        compare = device->CreateSampler(SamplerDesc::ShadowComparePCF("FogTest.Compare"));
        ASSERT_TRUE(linear.IsValid());
        ASSERT_TRUE(compare.IsValid());
        for (size_t base : {0u, 4u, 28u})
            for (size_t column = 0; column < 4; ++column)
                params[base + column][column] = 1.0f;
        params[8] = params[32] = {0, 0, 1, 0};
        params[9] = {0, 0, -1, 0};
        params[12] = {1, 1, 1, 0.1f};
        params[14] = {0, 100, 0, 1};
        params[23][2] = 1; // orthographic: identity maps [0,1] depth to world Z
        params[24] = {1, 1, 0.92f, 0};
        params[25] = {kSize, kSize, kSize, 0};
        params[26] = {0, 1, kSize, kSize};
        std::array<uint32_t, 256> zeros{};
        zero = Buffer(zeros.data(), sizeof(zeros));
        neutral = VolumeTexture(std::vector<Vec4>(kVoxelCount, {0, 0, 0, 0}));
        litSpot = DepthTexture(1, 0);
        litPoint = DepthTexture(12, 0);
        TextureDesc cubeDesc{};
        cubeDesc.width = cubeDesc.height = 2;
        cubeDesc.arrayLayers = 6;
        cubeDesc.flags = TextureCreateFlags::CubeCompatible;
        cubeDesc.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        cubeDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        cube = device->CreateTexture(cubeDesc);
        textures.push_back(cube);
        std::array<Vec4, 4> black{};
        auto upload = Buffer(black.data(), sizeof(black));
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        for (uint32_t layer = 0; layer < 6; ++layer)
            cl->CopyBufferToTextureSubresource(upload, cube, 0, layer, 2, 2);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(cube, ResourceState::CopyDest, ResourceState::ShaderResource));
        Submit(cl.get());
    }

    void TearDown() override
    {
        if (!device)
            return;
        device->WaitForIdle();
        for (auto set : sets)
            device->DestroyDescriptorSet(set);
        for (auto texture : textures)
            device->DestroyTexture(texture);
        for (auto buffer : buffers)
            device->DestroyBuffer(buffer);
        if (linear.IsValid()) device->DestroySampler(linear);
        if (compare.IsValid()) device->DestroySampler(compare);
        device->Shutdown();
    }

    void Submit(CommandList* cl)
    {
        cl->End();
        device->ExecuteCommandLists({cl});
        device->WaitForIdle();
    }

    BufferHandle Buffer(const void* data, size_t bytes)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::TransferSrc);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = device->CreateBuffer(desc);
        if (!buffer.IsValid()) throw std::runtime_error("Fog test buffer allocation failed");
        buffers.push_back(buffer);
        device->UpdateBuffer(buffer, 0, bytes, data);
        return buffer;
    }

    TextureHandle NewVolume(TextureFormat format)
    {
        TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        desc.format = static_cast<uint32_t>(format);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                                           TextureUsage::TransferDst | TextureUsage::TransferSrc);
        auto texture = device->CreateTexture(desc);
        if (!texture.IsValid()) throw std::runtime_error("Fog test volume allocation failed");
        textures.push_back(texture);
        return texture;
    }

    TextureHandle VolumeTexture(const std::vector<Vec4>& pixels)
    {
        auto texture = NewVolume(TextureFormat::R32G32B32A32_FLOAT);
        auto upload = Buffer(pixels.data(), pixels.size() * sizeof(Vec4));
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(upload, texture, 0, 0, width, height, 0,
                                           width * sizeof(Vec4), depth, width * height * sizeof(Vec4));
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        Submit(cl.get());
        return texture;
    }

    TextureHandle DepthTexture(uint32_t layers, float depthValue)
    {
        TextureDesc desc{};
        desc.width = desc.height = kSize;
        desc.arrayLayers = layers;
        desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::DepthStencil);
        auto texture = device->CreateTexture(desc);
        if (!texture.IsValid()) throw std::runtime_error("Fog test shadow allocation failed");
        textures.push_back(texture);
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        for (uint32_t layer = 0; layer < layers; ++layer)
        {
            RenderPassDesc pass{};
            pass.depthTarget = texture;
            pass.clearDepth = true;
            pass.clearDepthValue = depthValue;
            pass.useDepthView = true;
            pass.depthViewDesc.aspect = TextureAspect::Depth;
            pass.depthViewDesc.baseLayer = layer;
            pass.depthViewDesc.layerCount = 1;
            pass.depthViewDesc.levelCount = 1;
            cl->BeginRenderPass(pass);
            cl->EndRenderPass();
        }
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::DepthWrite, ResourceState::ShaderResource));
        Submit(cl.get());
        return texture;
    }

    struct Inputs
    {
        TextureHandle current{}, history{}, emission{}, spotMap{}, pointMap{}, sunMap{};
        BufferHandle volumes{}, lights{}, spotData{}, pointData{}, lightIndices{}, lightClusters{}, sunData{};
        size_t lightBytes = sizeof(Lights);
        size_t pointBytes = sizeof(PointSlot);
    };
    struct Outputs { TextureHandle color, emission; };

    Outputs Dispatch(const char* shader, const Inputs& inputs)
    {
        const std::string path = std::string(GE_FOG_SHADER_DIR) + "/volumetric_fog_" + shader + ".comp.spv";
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> spv{std::istreambuf_iterator<char>(file), {}};
        if (spv.empty()) throw std::runtime_error("Missing production fog shader: " + path);
        DescriptorSetLayoutDesc layout{};
        for (uint32_t binding = 0; binding <= 20; ++binding)
        {
            auto type = DescriptorType::CombinedImageSampler;
            if (binding == 0 || binding == 15) type = DescriptorType::StorageImage;
            if (binding == 2 || binding == 4 || binding == 9 || binding == 10 || binding == 17)
                type = DescriptorType::UniformBuffer;
            if (binding == 6 || binding == 7 || binding == 8 || binding == 12 || binding == 19)
                type = DescriptorType::StorageBuffer;
            layout.bindings.push_back({binding, type, 1, kShaderStageCompute});
        }
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(spv));
        pipelineDesc.DescriptorSetLayouts.push_back(device->InternDescriptorSetLayout(layout));
        auto pipeline = device->GetOrCreateComputePipeline(device->InternComputePipeline(pipelineDesc));
        if (!pipeline.IsValid()) throw std::runtime_error("Fog test pipeline creation failed: " + path);
        DescriptorSetDesc setDesc{};
        setDesc.layout = layout;
        auto set = device->CreateDescriptorSet(setDesc);
        sets.push_back(set);
        Outputs out{NewVolume(TextureFormat::R16G16B16A16_FLOAT), NewVolume(TextureFormat::R16G16B16A16_FLOAT)};
        device->UpdateStorageImageBinding(set, 0, out.color);
        device->UpdateStorageImageBinding(set, 15, out.emission);
        auto sampled = [&](uint32_t binding, TextureHandle texture, TextureHandle fallback, SamplerHandle sampler) {
            device->UpdateCombinedImageSamplerBinding(set, binding, texture.IsValid() ? texture : fallback, sampler);
        };
        sampled(1, inputs.current, neutral, linear);
        sampled(3, inputs.history, neutral, linear);
        sampled(5, inputs.sunMap, litPoint, compare);
        sampled(11, {}, neutral, linear);
        sampled(13, {}, neutral, linear);
        sampled(14, {}, cube, linear);
        sampled(16, inputs.emission, neutral, linear);
        sampled(18, inputs.spotMap, litSpot, compare);
        sampled(20, inputs.pointMap, litPoint, compare);
        device->UpdateBufferBinding(set, 2, Buffer(params.data(), sizeof(params)), 0, sizeof(params));
        device->UpdateBufferBinding(set, 4, inputs.sunData.IsValid() ? inputs.sunData : zero, 0,
                                    inputs.sunData.IsValid() ? 28 * sizeof(Vec4) : 1024);
        for (uint32_t binding : {9u, 10u})
            device->UpdateBufferBinding(set, binding, zero, 0, 1024);
        device->UpdateBufferBinding(set, 17, inputs.spotData.IsValid() ? inputs.spotData : zero, 0, sizeof(SpotShadow));
        device->UpdateStorageBufferBinding(set, 6, inputs.lightIndices.IsValid() ? inputs.lightIndices : zero, 0,
                                           inputs.lightIndices.IsValid() ? 0 : 1024);
        device->UpdateStorageBufferBinding(set, 7, inputs.lightClusters.IsValid() ? inputs.lightClusters : zero, 0,
                                           inputs.lightClusters.IsValid() ? 0 : 1024);
        device->UpdateStorageBufferBinding(set, 8, inputs.lights.IsValid() ? inputs.lights : zero, 0,
                                           inputs.lights.IsValid() ? inputs.lightBytes : 1024);
        device->UpdateStorageBufferBinding(set, 12, inputs.volumes.IsValid() ? inputs.volumes : zero, 0,
                                           inputs.volumes.IsValid() ? sizeof(Volumes) : 1024);
        device->UpdateStorageBufferBinding(set, 19, inputs.pointData.IsValid() ? inputs.pointData : zero, 0, inputs.pointBytes);
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        for (auto texture : {out.color, out.emission})
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::UnorderedAccess));
        const bool cull = std::strcmp(shader, "cull") == 0;
        if (cull)
            for (auto buffer : {inputs.lightIndices, inputs.lightClusters})
                cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::Undefined, ResourceState::UnorderedAccess));
        cl->SetPipeline(pipeline);
        cl->BindDescriptorSet(0, set, pipeline);
        cl->Dispatch((width + 7) / 8, (height + 7) / 8, (depth + 7) / 8);
        if (cull)
            for (auto buffer : {inputs.lightIndices, inputs.lightClusters})
                cl->Barrier(ResourceBarrier::CreateBufferBarrier(buffer, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
        for (auto texture : {out.color, out.emission})
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
        Submit(cl.get());
        return out;
    }

    std::vector<Vec4> Read(TextureHandle texture)
    {
        BufferDesc desc{};
        desc.size = static_cast<size_t>(width) * height * depth * 8;
        desc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Readback;
        auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource, ResourceState::CopySource));
        cl->CopyTextureSubresourceToBuffer(texture, 0, 0, buffer, width, height, 0, 0, 0,
                                          width * 8, depth, width * height * 8);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopySource, ResourceState::ShaderResource));
        Submit(cl.get());
        const auto* mapped = static_cast<const uint16_t*>(device->MapBuffer(buffer));
        if (!mapped) throw std::runtime_error("Fog test readback failed");
        std::vector<Vec4> result(static_cast<size_t>(width) * height * depth);
        for (size_t i = 0; i < result.size(); ++i)
            for (size_t c = 0; c < 4; ++c)
                result[i][c] = DecodeHalf(mapped[4 * i + c]);
        device->UnmapBuffer(buffer);
        return result;
    }

    void SetLights(Inputs& inputs, const std::vector<PackedLight>& lights)
    {
        std::vector<uint8_t> bytes(16 + lights.size() * sizeof(PackedLight));
        const uint32_t count = static_cast<uint32_t>(lights.size());
        std::memcpy(bytes.data(), &count, sizeof(count));
        std::memcpy(bytes.data() + 16, lights.data(), lights.size() * sizeof(PackedLight));
        inputs.lights = Buffer(bytes.data(), bytes.size());
        inputs.lightBytes = bytes.size();
    }

    uint32_t Cull(Inputs& inputs)
    {
        const uint32_t count = ((width + 7) / 8) * ((height + 7) / 8) * ((depth + 7) / 8);
        std::vector<uint32_t> indices(count * 64);
        std::vector<uint32_t> clusters(4 + count * 2);
        inputs.lightIndices = Buffer(indices.data(), indices.size() * sizeof(uint32_t));
        inputs.lightClusters = Buffer(clusters.data(), clusters.size() * sizeof(uint32_t));
        Dispatch("cull", inputs);
        const auto* mapped = static_cast<const uint32_t*>(device->MapBuffer(inputs.lightClusters));
        if (!mapped) throw std::runtime_error("Fog cluster readback failed");
        const uint32_t hits = mapped[5];
        device->UnmapBuffer(inputs.lightClusters);
        return hits;
    }

    Lights LocalLight(uint32_t type)
    {
        Lights lights{};
        auto& light = lights.lights[1]; // index zero intentionally disabled
        light.meta[0] = type;
        light.meta[1] = light.meta[3] = 1;
        light.posRange = {0, 0, 1, 10};
        light.dirIntensity = {0, 0, -1, 4};
        light.colorAreaWidth = {0, 1, 0, 0};
        light.areaParams[3] = 0.8f;
        light.spotParams = {0.5f, 1, 0, 0};
        light.shadowSlots[0] = 1;
        return lights;
    }

    uint32_t width = kSize, height = kSize, depth = kSize;
    std::unique_ptr<IDevice> device;
    std::vector<TextureHandle> textures;
    std::vector<BufferHandle> buffers;
    std::vector<DescriptorSetHandle> sets;
    SamplerHandle linear{}, compare{};
    TextureHandle neutral{}, litSpot{}, litPoint{}, cube{};
    BufferHandle zero{};
    std::array<Vec4, 33> params{};
};

TEST_F(VolumetricFogGpu, ClearingFogDoesNotRetainHistoryOpacity)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {0.2f, 0.2f, 0.2f, 0.8f}));
    inputs.history = VolumeTexture(std::vector<Vec4>(kVoxelCount, {0.2f, 0.2f, 0.2f, 0.2f}));
    for (auto pixel : Read(Dispatch("temporal", inputs).color))
        EXPECT_NEAR(pixel[3], 0.8f, 0.001f);
}

TEST_F(VolumetricFogGpu, FilterPreservesDensityBoundaryAndSharpLightShaft)
{
    std::vector<Vec4> pixels(kVoxelCount);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = i % kSize < 4 ? Vec4{1, 0, 0, 0.2f} : Vec4{0, 0, 0, 0};
    Inputs inputs{};
    inputs.current = VolumeTexture(pixels);
    auto filtered = Read(Dispatch("filter", inputs).color);
    for (size_t i = 0; i < filtered.size(); ++i)
    {
        EXPECT_NEAR(filtered[i][3], pixels[i][3], 0.0002f);
        EXPECT_NEAR(filtered[i][0], pixels[i][0], 0.005f);
    }
    // A light boundary inside uniform fog must also remain sharp.
    for (auto& pixel : pixels) pixel[3] = 0.2f;
    inputs.current = VolumeTexture(pixels);
    filtered = Read(Dispatch("filter", inputs).color);
    EXPECT_GT(filtered[3][0], 0.98f);
    EXPECT_LT(filtered[4][0], 0.02f);
}

TEST_F(VolumetricFogGpu, FilterSmoothsSmallVariationsWithinUniformFog)
{
    std::vector<Vec4> pixels(kVoxelCount, {1, 1, 1, 0.2f});
    pixels[3] = {1.05f, 1.05f, 1.05f, 0.2f};
    Inputs inputs{};
    inputs.current = VolumeTexture(pixels);
    const auto filtered = Read(Dispatch("filter", inputs).color);
    EXPECT_GT(filtered[3][0], 1.0f);
    EXPECT_LT(filtered[3][0], 1.04f);
    EXPECT_NEAR(filtered[3][3], 0.2f, 0.0002f);
}

TEST_F(VolumetricFogGpu, LocalEmissionReachesLightingAndReplaceOverridesAdditive)
{
    // The renderer allocates the emission grid, and sets this flag, only when a
    // local volume can change emission — which is exactly the case under test.
    params[27][3] = 1;
    Volumes volumes{};
    volumes.volumes[0].emission = {0.5f, 0, 0, 0};
    volumes.volumes[1].emission = {0, 0.75f, 0, 0};
    Inputs inputs{};
    inputs.volumes = Buffer(&volumes, sizeof(volumes));
    const auto media = Dispatch("media", inputs);
    inputs.current = media.color;
    inputs.emission = media.emission;
    for (auto pixel : Read(Dispatch("light", inputs).color))
    {
        EXPECT_NEAR(pixel[0], 0.5f, 0.001f);
        EXPECT_NEAR(pixel[1], 0.75f, 0.001f);
        EXPECT_NEAR(pixel[2], 0.0f, 0.001f);
        EXPECT_GT(pixel[3], 0.29f);
    }
    volumes.volumes[1].params[3] = 2; // replace
    inputs.volumes = Buffer(&volumes, sizeof(volumes));
    const auto replacement = Dispatch("media", inputs);
    inputs.current = replacement.color;
    inputs.emission = replacement.emission;
    for (auto pixel : Read(Dispatch("light", inputs).color))
    {
        EXPECT_NEAR(pixel[0], 0.0f, 0.001f);
        EXPECT_NEAR(pixel[1], 0.75f, 0.001f);
    }

    // With no volume able to change emission the renderer leaves the grid at one
    // texel and clears the flag, and the lighting pass must fall back to the
    // view's own emission — the value every texel of that grid would have held.
    params[27][3] = 0;
    params[13] = {0.25f, 0.125f, 0, 0};
    for (auto pixel : Read(Dispatch("light", inputs).color))
    {
        EXPECT_NEAR(pixel[0], 0.25f, 0.001f);
        EXPECT_NEAR(pixel[1], 0.125f, 0.001f);
    }
    params[13] = {0, 0, 0, 0};
}

TEST_F(VolumetricFogGpu, SecondSpotLightUsesItsShadowAndHonorsCasterFlag)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    auto lights = LocalLight(2);
    inputs.lights = Buffer(&lights, sizeof(lights));
    SpotShadow shadow{};
    Identity(shadow.vp);
    inputs.spotData = Buffer(&shadow, sizeof(shadow));
    const size_t center = 4 + 4 * kSize + 4 * kSize * kSize;
    const auto lit = Read(Dispatch("light", inputs).color);
    EXPECT_GT(lit[center][1], 0.01f);
    EXPECT_NEAR(lit[center][0], 0.0f, 0.0001f);
    inputs.spotMap = DepthTexture(1, 1);
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][1], 0.0f, 0.0001f);
    lights.lights[1].meta[1] = 0;
    inputs.lights = Buffer(&lights, sizeof(lights));
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][1], lit[center][1], 0.001f);
}

TEST_F(VolumetricFogGpu, PointShadowUsesAssignedAtlasSlotAndRejectsMissingSlots)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    auto lights = LocalLight(1);
    inputs.lights = Buffer(&lights, sizeof(lights));
    std::array<PointSlot, 2> slots{};
    for (auto& matrix : slots[1].vp) Identity(matrix);
    slots[1].params = {1, 6, 0, 0};
    slots[1].extra = {4, 0.01f, 10, 0.5f};
    inputs.pointData = Buffer(slots.data(), sizeof(slots));
    inputs.pointBytes = sizeof(slots);
    const size_t center = 4 + 4 * kSize + 4 * kSize * kSize;
    const float lit = Read(Dispatch("light", inputs).color)[center][1];
    EXPECT_GT(lit, 0.01f);
    inputs.pointMap = DepthTexture(12, 1);
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][1], 0.0f, 0.0001f);
    inputs.pointBytes = sizeof(PointSlot); // only disabled slot zero is bound
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][1], lit, 0.001f);
}
TEST_F(VolumetricFogGpu, FogListsCullDistantLightsWithoutThe64LightCutoff)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    std::vector<PackedLight> lights(128, LocalLight(1).lights[1]);
    for (auto& light : lights)
    {
        light.meta[1] = 0;
        light.posRange = {100, 100, 100, 1};
    }
    lights[100].posRange = {0, 0, 1, 10};
    SetLights(inputs, lights);
    const auto full = Read(Dispatch("light", inputs).color);
    ASSERT_EQ(Cull(inputs), 1u);
    const auto culled = Read(Dispatch("light", inputs).color);
    EXPECT_GT(full[292][1], 0.01f);
    for (size_t i = 0; i < full.size(); ++i)
        EXPECT_NEAR(culled[i][1], full[i][1], 0.001f);
}

TEST_F(VolumetricFogGpu, OverflowFallsBackWithoutDroppingContributions)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    std::vector<PackedLight> lights(65, LocalLight(1).lights[1]);
    for (auto& light : lights)
    {
        light.meta[1] = 0;
        light.colorAreaWidth = {1, 0, 0, 0};
    }
    lights.back().colorAreaWidth = {0, 1, 0, 0};
    SetLights(inputs, lights);
    const auto full = Read(Dispatch("light", inputs).color);
    ASSERT_EQ(Cull(inputs), 65u);
    const auto culled = Read(Dispatch("light", inputs).color);
    EXPECT_GT(full[292][1], 0.01f);
    for (size_t i = 0; i < full.size(); ++i)
        for (size_t c = 0; c < 3; ++c)
            EXPECT_EQ(culled[i][c], full[i][c]);
}

TEST_F(VolumetricFogGpu, ConservativeBoundsMatchFullScanForPerspectiveAndOrthographic)
{
    std::mt19937 rng(7129);
    std::uniform_real_distribution<float> position(-4.0f, 4.0f), range(0.2f, 1.5f);
    std::vector<PackedLight> lights;
    for (uint32_t i = 0; i < 128; ++i)
    {
        auto light = LocalLight(i % 3 == 0 ? 4u : (i % 3 == 1 ? 2u : 1u)).lights[1];
        light.meta[1] = 0;
        light.meta[2] = i % 4;
        light.posRange = {position(rng), position(rng), position(rng), range(rng)};
        light.colorAreaWidth = {0.5f, 0.2f, 0.1f, 3};
        light.areaParams[0] = light.areaParams[1] = 2;
        light.areaRight[0] = 1;
        lights.push_back(light);
    }
    for (float orthographic : {0.0f, 1.0f})
    {
        params[23][2] = orthographic;
        Inputs inputs{};
        inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
        SetLights(inputs, lights);
        const auto full = Read(Dispatch("light", inputs).color);
        const uint32_t count = Cull(inputs);
        ASSERT_GT(count, 0u);
        ASSERT_LT(count, 64u) << "Exercise the culled path, not overflow fallback";
        const auto culled = Read(Dispatch("light", inputs).color);
        float energy = 0;
        for (size_t i = 0; i < full.size(); ++i)
        {
            energy += full[i][0];
            for (size_t c = 0; c < 3; ++c)
                EXPECT_NEAR(culled[i][c], full[i][c], std::max(0.001f, full[i][c] * 0.002f));
        }
        EXPECT_GT(energy, 0.01f);
    }
}
TEST_F(VolumetricFogGpu, PartialBricksAndJitteredDepthMatchFullScan)
{
    width = 17;
    height = depth = 9;
    params[25] = {float(width), float(height), float(depth), 0};
    params[24] = {12, 2, 0.92f, 1};
    params[26][0] = 31;
    std::vector<PackedLight> lights(80, LocalLight(1).lights[1]);
    for (size_t i = 0; i < lights.size(); ++i)
    {
        lights[i].meta[1] = 0;
        lights[i].posRange = i < 10 ? Vec4{float(i % 3) - 1, 0, -float(i), 2}
                                   : Vec4{100, 100, 100, 1};
    }
    for (float orthographic : {0.0f, 1.0f})
    {
        params[23][2] = orthographic;
        Inputs inputs{};
        inputs.current = VolumeTexture(std::vector<Vec4>(width * height * depth, {1, 1, 1, 0.2f}));
        SetLights(inputs, lights);
        const auto full = Read(Dispatch("light", inputs).color);
        Cull(inputs);
        const auto* header = static_cast<const uint32_t*>(device->MapBuffer(inputs.lightClusters));
        ASSERT_NE(header, nullptr);
        EXPECT_EQ(header[0], 3u);
        EXPECT_EQ(header[1], 2u);
        EXPECT_EQ(header[2], 2u);
        device->UnmapBuffer(inputs.lightClusters);
        const auto culled = Read(Dispatch("light", inputs).color);
        float energy = 0;
        for (size_t i = 0; i < full.size(); ++i)
        {
            energy += full[i][1];
            EXPECT_NEAR(culled[i][1], full[i][1], std::max(0.001f, full[i][1] * 0.002f));
        }
        EXPECT_GT(energy, 0.01f);
    }
}
} // namespace

TEST_F(VolumetricFogGpu, MovingSunShadowHasPartialCoverageWithinDepthCells)
{
    // A planar blocker cuts a depth cell into lit and shadowed portions. A
    // small camera translation must change its coverage, not toggle a whole
    // slice between dark and lit (the visible bands during camera movement).
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    params[10] = {1, 1, 1, 0};
    const size_t center = 4 + 4 * kSize + 4 * kSize * kSize;
    const float fullyLit = Read(Dispatch("light", inputs).color)[center][0];
    ASSERT_GT(fullyLit, 0.01f);
    std::array<Vec4, 28> shadow{};
    for (size_t column = 0; column < 4; ++column)
        shadow[column][column] = 1;
    shadow[16] = {10, 10, 10, 10};
    shadow[17] = {0, 0, 1, 10};
    inputs.sunData = Buffer(shadow.data(), sizeof(shadow));
    inputs.sunMap = DepthTexture(12, 0.5f);
    for (float distribution : {1.0f, 1.6f, 2.0f})
    {
        params[24][1] = distribution;
        const float nearDistance = std::pow(4.0f / kSize, distribution);
        const float farDistance = std::pow(5.0f / kSize, distribution);
        for (int quarter = 0; quarter <= 4; ++quarter)
        {
            params[3][2] = nearDistance - 0.5f + float(quarter) * 0.25f * (farDistance - nearDistance);
            const float actual = Read(Dispatch("light", inputs).color)[center][0] / fullyLit;
            EXPECT_NEAR(actual, float(quarter) * 0.25f, 0.02f)
                << "distribution " << distribution << ", camera step " << quarter;
        }
    }
}

TEST_F(VolumetricFogGpu, ZeroSunSkipsItsTapsAndADimSunKeepsThem)
{
    // The sun loop is skipped only where skipping cannot change the result: an
    // exactly black sun. A threshold instead of an exact zero would silently
    // drop sun shadows from a dim sky, so a dim sun still reads the cascades.
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    const size_t center = 4 + 4 * kSize + 4 * kSize * kSize;

    params[10] = {0, 0, 0, 0};
    EXPECT_EQ(Read(Dispatch("light", inputs).color)[center][0], 0.0f);

    constexpr float kDimSun = 1.0f / 1024.0f;
    params[10] = {kDimSun, kDimSun, kDimSun, 0};
    const float unshadowed = Read(Dispatch("light", inputs).color)[center][0];
    ASSERT_GT(unshadowed, 0.0f);

    std::array<Vec4, 28> shadow{};
    for (size_t column = 0; column < 4; ++column)
        shadow[column][column] = 1;
    shadow[16] = {10, 10, 10, 10};
    shadow[17] = {0, 0, 1, 10};
    inputs.sunData = Buffer(shadow.data(), sizeof(shadow));
    inputs.sunMap = DepthTexture(12, 0.5f);
    params[3][2] = 4.0f / kSize - 0.5f;
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][0], 0.0f, unshadowed * 0.02f);
    params[3][2] = 5.0f / kSize - 0.5f;
    EXPECT_NEAR(Read(Dispatch("light", inputs).color)[center][0], unshadowed, unshadowed * 0.02f);
}

TEST_F(VolumetricFogGpu, MovingLocalShadowsHavePartialCoverageWithinDepthCells)
{
    Inputs inputs{};
    inputs.current = VolumeTexture(std::vector<Vec4>(kVoxelCount, {1, 1, 1, 0.2f}));
    SpotShadow spot{};
    Identity(spot.vp);
    inputs.spotData = Buffer(&spot, sizeof(spot));
    std::array<PointSlot, 2> slots{};
    for (auto& matrix : slots[1].vp) Identity(matrix);
    slots[1].params = {1, 6, 0, 0};
    slots[1].extra = {8, 0.01f, 10, 1};
    inputs.pointData = Buffer(slots.data(), sizeof(slots));
    inputs.pointBytes = sizeof(slots);
    inputs.spotMap = DepthTexture(1, 0.5f);
    inputs.pointMap = DepthTexture(12, 0.5f);
    const size_t center = 4 + 4 * kSize + 4 * kSize * kSize;
    for (uint32_t type : {1u, 2u})
    {
        for (int quarter = 0; quarter <= 4; ++quarter)
        {
            params[3][2] = float(quarter) / (4 * kSize);
            auto lights = LocalLight(type);
            lights.lights[1].meta[1] = 0;
            inputs.lights = Buffer(&lights, sizeof(lights));
            const float fullyLit = Read(Dispatch("light", inputs).color)[center][1];
            ASSERT_GT(fullyLit, 0.01f);
            lights.lights[1].meta[1] = 1;
            inputs.lights = Buffer(&lights, sizeof(lights));
            const float actual = Read(Dispatch("light", inputs).color)[center][1] / fullyLit;
            EXPECT_NEAR(actual, float(quarter) * 0.25f, 0.02f)
                << "light type " << type << ", camera step " << quarter;
        }
    }
}
