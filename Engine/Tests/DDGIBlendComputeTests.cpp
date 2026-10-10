// Dispatch the shipped blend and upload kernels, not CPU copies of their equations.
#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "DDGIShippedKernel.h"
#include "TestDeviceHelper.h"
#include "Engine/Rendering/DDGIProbeWindow.h"
#include "Engine/Rendering/DDGIAtlasUploadState.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
class DDGIBlendComputeTest : public ::testing::Test
{
  protected:
    using V4 = std::array<float, 4>;
    static constexpr uint32_t kRays = 64;
    static constexpr uint32_t kTexels = 64;
    struct Params
    {
        V4 min{0, 0, 0, 0};
        V4 size{2, 2, 2, 0};
        std::array<int32_t, 4> count{2, 2, 2, 8};
        V4 policy{2, 0, 0.97f, 1};
        std::array<uint32_t, 4> dispatch{0, kRays, 1, 0};
        V4 timing{1000.0f / 60.0f, 4, 3, 0.2f};
        V4 depth{1, 6, 0, 0};
    } params;

    void SetUp() override
    {
        device = CreateVulkanDeviceFast();
        if (!device)
            GTEST_SKIP() << "No Vulkan device available";
        std::string error;
        ASSERT_TRUE(LoadDDGIShippedKernel(*device, "ddgi_blend", blend, error)) << error;
    }

    static uint32_t Binding(const DDGIShippedKernel& kernel, const char* name)
    {
        const uint32_t binding = kernel.Binding(name);
        EXPECT_NE(binding, DDGIShippedKernel::kMissingBinding) << "the kernel declares no " << name;
        return binding;
    }

    void TearDown() override
    {
        if (!device)
            return;
        device->WaitForIdle();
        for (auto buffer : buffers)
            device->DestroyBuffer(buffer);
        for (auto texture : textures)
            device->DestroyTexture(texture);
    }

    BufferHandle MakeBuffer(const void* data, size_t bytes, bool uniform = false)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(uniform ? BufferUsage::Uniform : BufferUsage::Storage);
        desc.memoryUsage = uniform ? BufferMemoryUsage::Upload : BufferMemoryUsage::Readback;
        desc.debugName = "DDGI.BlendRegression.Buffer";
        auto buffer = device->CreateBuffer(desc);
        buffers.push_back(buffer);
        if (buffer.IsValid())
        {
            auto* mapped = device->MapBuffer(buffer);
            if (mapped)
            {
                std::memcpy(mapped, data, bytes);
                device->UnmapBuffer(buffer);
            }
            else
                ADD_FAILURE() << "Could not map test buffer";
        }
        return buffer;
    }

    void Run(V4 ray, V4 history = {0, 0, 0, 0}, float age = 0, bool reset = false)
    {
        // Eight allocated slots but only slot zero is visited. The other seven
        // are sentinels for dispatch-window bounds, not expected CPU copies.
        std::vector<V4> rays(8 * kRays, ray);
        std::vector<V4> irradiance(8 * kTexels, history);
        std::vector<V4> depth(8 * kTexels, V4{2, 4, 0, 0});
        std::vector<float> temporal(8 * kTexels, age);
        std::vector<V4> cells(8, V4{0, 0, 0, 0});
        if (reset)
            cells[0] = {-1.0e30f, -1.0e30f, -1.0e30f, 0};
        std::array<BufferHandle, 6> handles{
            MakeBuffer(&params, sizeof(params), true),
            MakeBuffer(rays.data(), rays.size() * sizeof(V4)),
            MakeBuffer(irradiance.data(), irradiance.size() * sizeof(V4)),
            MakeBuffer(depth.data(), depth.size() * sizeof(V4)),
            MakeBuffer(temporal.data(), temporal.size() * sizeof(float)),
            MakeBuffer(cells.data(), cells.size() * sizeof(V4))};
        for (auto handle : handles)
            ASSERT_TRUE(handle.IsValid());
        DescriptorSetDesc dsDesc{};
        dsDesc.layout = blend.Layout;
        auto ds = device->CreateDescriptorSet(dsDesc);
        device->UpdateBufferBinding(ds, Binding(blend, "DDGIParams"), handles[0], 0, sizeof(params));
        const std::array<size_t, 5> sizes{rays.size() * sizeof(V4), irradiance.size() * sizeof(V4),
            depth.size() * sizeof(V4), temporal.size() * sizeof(float), cells.size() * sizeof(V4)};
        const std::array<const char*, 5> names{"DDGIRayBufferRO", "DDGIIrradianceState", "DDGIDepthState",
                                               "DDGITemporalState", "DDGIProbeCell"};
        for (uint32_t i = 1; i < 6; ++i)
            device->UpdateStorageBufferBinding(ds, Binding(blend, names[i - 1]), handles[i], 0, sizes[i - 1]);
        auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->SetPipeline(blend.Pipeline);
        commands->BindDescriptorSet(0, ds, blend.Pipeline);
        // Over-dispatch intentionally: the production bounds check must keep
        // unvisited slots untouched even though their allocations exist.
        commands->Dispatch(8, 1, 1);
        commands->End();
        device->ExecuteCommandLists({commands.get()});
        device->WaitForIdle();
        result.resize(irradiance.size());
        auto* mapped = device->MapBuffer(handles[2]);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(result.data(), mapped, sizes[1]);
        device->UnmapBuffer(handles[2]);
        depthResult.resize(depth.size());
        mapped = device->MapBuffer(handles[3]);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(depthResult.data(), mapped, sizes[2]);
        device->UnmapBuffer(handles[3]);
        for (size_t i = kTexels; i < result.size(); ++i)
            for (size_t channel = 0; channel < 4; ++channel)
                if (std::isfinite(history[channel]))
                    EXPECT_FLOAT_EQ(result[i][channel], history[channel]);
    }

    void ExpectFinite()
    {
        ASSERT_GE(result.size(), kTexels);
        for (size_t i = 0; i < kTexels; ++i)
            for (float value : result[i])
                EXPECT_TRUE(std::isfinite(value)) << "texel " << i;
    }

    void CheckWindowedUploads()
    {
        using namespace GameEngine::Engine::Renderer;
        DDGIShippedKernel upload;
        std::string error;
        ASSERT_TRUE(LoadDDGIShippedKernel(*device, "ddgi_upload", upload, error)) << error;
        std::array<TextureHandle, 4> atlases;
        for (auto& atlas : atlases)
        {
            TextureDesc td{};
            td.width = 16;
            td.height = 32;
            td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
            td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::TransferSrc);
            atlas = device->CreateTexture(td);
            textures.push_back(atlas);
            ASSERT_TRUE(atlas.IsValid());
        }
        struct UploadParams
        {
            V4 min{0, 0, 0, 0};
            V4 size{2, 2, 2, 0};
            std::array<int32_t, 4> count{2, 2, 2, 8};
            std::array<uint32_t, 4> dispatch{0, 8, 0, 0};
            V4 filter{0, 0.5f, 6, 0};
        } up;
        std::vector<V4> state(8 * kTexels), depth(8 * kTexels);
        std::vector<V4> cells(8);
        for (uint32_t p = 0; p < 8; ++p)
            cells[p] = {float(p % 2), float((p / 2) % 2), float(p / 4), 0};
        DDGIAtlasUploadState uploadState;
        uint32_t cursor = 0;
        for (uint32_t tick = 0; tick < 18; ++tick)
        {
            const auto window = ComputeDDGIProbeWindow(cursor, 1 + (tick * 3) % 7, 8);
            cursor = window.NextCursor;
            for (uint32_t p = window.Base; p < window.Base + window.Count; ++p)
                for (uint32_t t = 0; t < kTexels; ++t)
                {
                    state[p * kTexels + t] = {float(p + 1), float(t + tick), float(tick), 100};
                    depth[p * kTexels + t] = {float(p + tick + 1), float(t + 1), 0, 0};
                }
            // Allocation, filter strength and smoothness edits, a scroll and a
            // resize each refresh the whole atlas; every other tick uploads
            // only its window.
            if (tick == 6) up.filter[0] = 1;
            if (tick == 9) up.filter[1] = 0.75f;
            if (tick == 12) up.min[0] = 2;  // scroll: stale cells must upload black
            if (tick == 15) up.size[1] = 3;
            const bool full = uploadState.NeedsFullUpload(up.min.data(), up.size.data(),
                                                          up.filter[0], up.filter[1]);
            EXPECT_EQ(full, tick == 0 || tick == 6 || tick == 9 || tick == 12 || tick == 15) << "tick " << tick;
            const auto irradiance = MakeBuffer(state.data(), state.size() * sizeof(V4));
            const auto distances = MakeBuffer(depth.data(), depth.size() * sizeof(V4));
            const auto records = MakeBuffer(cells.data(), cells.size() * sizeof(V4));
            constexpr size_t kBytes = 16 * 32 * 8; // RGBA16F
            std::array<std::vector<uint8_t>, 4> captured;
            auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            for (uint32_t arm = 0; arm < 2; ++arm)
            {
                up.dispatch[0] = (full || arm == 1) ? 0 : window.Base;
                up.dispatch[1] = (full || arm == 1) ? 8 : window.Count;
                const auto uniform = MakeBuffer(&up, sizeof(up), true);
                DescriptorSetDesc setDesc{};
                setDesc.layout = upload.Layout;
                const auto set = device->CreateDescriptorSet(setDesc);
                device->UpdateBufferBinding(set, Binding(upload, "DDGIParams"), uniform, 0, sizeof(up));
                device->UpdateStorageBufferBinding(set, Binding(upload, "DDGIIrradianceStateRO"), irradiance, 0,
                                                   state.size() * sizeof(V4));
                device->UpdateStorageBufferBinding(set, Binding(upload, "DDGIDepthStateRO"), distances, 0,
                                                   depth.size() * sizeof(V4));
                device->UpdateStorageBufferBinding(set, Binding(upload, "DDGIProbeCellRO"), records, 0,
                                                   cells.size() * sizeof(V4));
                device->UpdateStorageImageBinding(set, Binding(upload, "uIrradianceAtlas"), atlases[arm * 2]);
                device->UpdateStorageImageBinding(set, Binding(upload, "uDepthAtlas"), atlases[arm * 2 + 1]);
                for (uint32_t a = arm * 2; a < arm * 2 + 2; ++a)
                    commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlases[a],
                        tick == 0 ? ResourceState::Undefined : ResourceState::CopySource,
                        ResourceState::UnorderedAccess));
                commands->SetPipeline(upload.Pipeline);
                commands->BindDescriptorSet(0, set, upload.Pipeline);
                commands->Dispatch(up.dispatch[1], 1, 1);
            }
            std::array<BufferHandle, 4> readbacks;
            for (uint32_t a = 0; a < 4; ++a)
            {
                BufferDesc rd{};
                rd.size = kBytes;
                rd.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
                rd.memoryUsage = BufferMemoryUsage::Readback;
                readbacks[a] = device->CreateBuffer(rd);
                buffers.push_back(readbacks[a]);
                ASSERT_TRUE(readbacks[a].IsValid());
                commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlases[a],
                    ResourceState::UnorderedAccess, ResourceState::CopySource));
                commands->CopyTextureToBuffer(atlases[a], readbacks[a], 16, 32);
            }
            commands->End();
            device->ExecuteCommandLists({commands.get()});
            device->WaitForIdle();
            for (uint32_t a = 0; a < 4; ++a)
            {
                const auto* mapped = device->MapBuffer(readbacks[a]);
                ASSERT_NE(mapped, nullptr);
                captured[a].resize(kBytes);
                std::memcpy(captured[a].data(), mapped, kBytes);
                device->UnmapBuffer(readbacks[a]);
            }
            EXPECT_EQ(captured[0], captured[2]) << "irradiance at tick " << tick;
            EXPECT_EQ(captured[1], captured[3]) << "depth at tick " << tick;
            uploadState.Commit(up.min.data(), up.size.data(), up.filter[0], up.filter[1]);
        }
    }

    std::unique_ptr<IDevice> device;
    DDGIShippedKernel blend;
    std::vector<BufferHandle> buffers;
    std::vector<TextureHandle> textures;
    std::vector<V4> result;
    std::vector<V4> depthResult;
};

TEST_F(DDGIBlendComputeTest, NonFiniteRaysDoNotPoisonIrradiance)
{
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()})
    {
        Run({invalid, 1, 2, 2});
        ExpectFinite();
        for (size_t i = 0; i < kTexels; ++i)
            EXPECT_FLOAT_EQ(result[i][0], 0.0f);
    }
}

TEST_F(DDGIBlendComputeTest, PoisonedHistoryRecoversOnNextFiniteVisit)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Run({1, 1, 1, 2}, {nan, nan, nan, nan}, nan);
    ExpectFinite();
    for (size_t i = 0; i < kTexels; ++i)
        EXPECT_NEAR(result[i][0], 1.0f, 0.04f);
}

TEST_F(DDGIBlendComputeTest, ConstantRadianceSeedsInIrradianceOverPiUnits)
{
    Run({1, 2, 3, 2});
    ExpectFinite();
    for (size_t i = 0; i < kTexels; ++i)
    {
        EXPECT_NEAR(result[i][0], 1.0f, 0.04f);
        EXPECT_NEAR(result[i][1], 2.0f, 0.08f);
        EXPECT_NEAR(result[i][2], 3.0f, 0.12f);
    }
}

TEST_F(DDGIBlendComputeTest, ChangedCellDiscardsHistoryWhileStableCellRetainsIt)
{
    Run({0, 0, 0, 2}, {1, 1, 1, 1}, 32);
    ExpectFinite();
    EXPECT_GT(result[0][0], 0.5f);
    EXPECT_LT(result[0][0], 1.0f);
    Run({0, 0, 0, 2}, {1, 1, 1, 1}, 32, true);
    ExpectFinite();
    for (size_t i = 0; i < kTexels; ++i)
        EXPECT_FLOAT_EQ(result[i][0], 0.0f);
}
TEST_F(DDGIBlendComputeTest, ReflectionMomentsKeepTheTrueDistanceWhileDiffuseMomentsStayBounded)
{
    // Two-metre probe spacing: the diffuse moments' local bound.
    const float bound = 1.5f * std::sqrt(12.0f);
    const float volumeDiagonal = std::sqrt(12.0f);
    constexpr float kFarWall = 20.0f;
    constexpr float kMiss = -1.0f;
    for (const auto& [rayDistance, reflectionDistance] :
         {std::pair{kFarWall, kFarWall}, std::pair{kMiss, volumeDiagonal}})
    {
        for (bool cellChanged : {true, false})
        {
            // Either way the texel takes the fresh estimate whole, so it holds this
            // tick's moments of rays that all travelled the same distance: a changed
            // cell discards its history, and the unchanged cell, whose irradiance
            // history is converged, has a depth history ({2, 4, 0, 0}) with a
            // bounded pair but no reflection pair, which is unseeded state and must
            // not be blended in.
            SCOPED_TRACE(cellChanged ? "changed cell" : "history without a reflection pair");
            Run({1, 1, 1, rayDistance}, {1, 1, 1, 1}, 32, cellChanged);
            ASSERT_GE(depthResult.size(), kTexels);
            for (size_t i = 0; i < kTexels; ++i)
            {
                EXPECT_NEAR(depthResult[i][0], bound, bound * 1.0e-4f) << "texel " << i;
                EXPECT_NEAR(depthResult[i][1], bound * bound, bound * bound * 1.0e-4f) << "texel " << i;
                EXPECT_NEAR(depthResult[i][2], reflectionDistance, reflectionDistance * 1.0e-4f) << "texel " << i;
                EXPECT_NEAR(depthResult[i][3], reflectionDistance * reflectionDistance,
                            reflectionDistance * reflectionDistance * 1.0e-4f)
                    << "texel " << i;
            }
        }
    }
}

TEST_F(DDGIBlendComputeTest, FineDepthBlendAndUploadCarryBothMomentPairs)
{
    DDGIShippedKernel depthBlend;
    DDGIShippedKernel depthUpload;
    std::string error;
    ASSERT_TRUE(LoadDDGIShippedKernel(*device, "ddgi_depth_blend", depthBlend, error)) << error;
    ASSERT_TRUE(LoadDDGIShippedKernel(*device, "ddgi_depth_upload", depthUpload, error)) << error;

    constexpr uint32_t kProbes = 8;
    constexpr uint32_t kFineTile = 16;  // GE_DDGI_DEPTH_TILE_FINE
    constexpr uint32_t kFineTexels = kFineTile * kFineTile;
    constexpr uint32_t kAtlasWidth = 2 * kFineTile;
    constexpr uint32_t kAtlasHeight = 2 * 2 * kFineTile;
    constexpr float kFarWall = 20.0f;
    const float bound = 1.5f * std::sqrt(12.0f);  // two-metre probe spacing

    // Every slot's cell record matches its cell, so nothing reads as scrolled.
    std::vector<V4> cells(kProbes);
    for (uint32_t p = 0; p < kProbes; ++p)
        cells[p] = {float(p % 2), float((p / 2) % 2), float(p / 4), 0};
    const auto records = MakeBuffer(cells.data(), cells.size() * sizeof(V4));

    // Blend: every ray hits a wall past the local bound. The bounded pair
    // stops at the bound and the true-distance pair keeps the wall.
    Params fine = params;
    fine.dispatch = {0, kRays, kProbes, 0};
    fine.depth = {1, 14, 0, 0};  // GE_DDGI_DEPTH_OCT_RES_FINE interior
    std::vector<V4> rays(kProbes * kRays, V4{1, 1, 1, kFarWall});
    std::vector<V4> state(kProbes * kFineTexels, V4{0, 0, 0, 0});
    const auto blendParams = MakeBuffer(&fine, sizeof(fine), true);
    const auto rayBuffer = MakeBuffer(rays.data(), rays.size() * sizeof(V4));
    const auto stateBuffer = MakeBuffer(state.data(), state.size() * sizeof(V4));
    DescriptorSetDesc blendSetDesc{};
    blendSetDesc.layout = depthBlend.Layout;
    const auto blendSet = device->CreateDescriptorSet(blendSetDesc);
    device->UpdateBufferBinding(blendSet, Binding(depthBlend, "DDGIParams"), blendParams, 0, sizeof(fine));
    device->UpdateStorageBufferBinding(blendSet, Binding(depthBlend, "DDGIRayBufferRO"), rayBuffer, 0,
                                       rays.size() * sizeof(V4));
    device->UpdateStorageBufferBinding(blendSet, Binding(depthBlend, "DDGIDepthState"), stateBuffer, 0,
                                       state.size() * sizeof(V4));
    device->UpdateStorageBufferBinding(blendSet, Binding(depthBlend, "DDGIProbeCellRO"), records, 0,
                                       cells.size() * sizeof(V4));
    auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(depthBlend.Pipeline);
    commands->BindDescriptorSet(0, blendSet, depthBlend.Pipeline);
    commands->Dispatch(kProbes, 1, 1);
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();
    const auto* blended = static_cast<const V4*>(device->MapBuffer(stateBuffer));
    ASSERT_NE(blended, nullptr);
    for (uint32_t i = 0; i < kProbes * kFineTexels; ++i)
    {
        EXPECT_NEAR(blended[i][0], bound, bound * 1.0e-4f) << "texel " << i;
        EXPECT_NEAR(blended[i][1], bound * bound, bound * bound * 1.0e-4f) << "texel " << i;
        EXPECT_NEAR(blended[i][2], kFarWall, kFarWall * 1.0e-4f) << "texel " << i;
        EXPECT_NEAR(blended[i][3], kFarWall * kFarWall, kFarWall * kFarWall * 1.0e-4f) << "texel " << i;
    }
    device->UnmapBuffer(stateBuffer);

    // Upload: an authored state whose four channels are exact in half
    // precision, so the RGBA16F atlas is checked bit for bit.
    constexpr std::array<uint16_t, 4> kExpectedHalfBits{0x4000, 0x4400, 0x4D00, 0x5E40};  // 2, 4, 20, 400
    std::vector<V4> authored(kProbes * kFineTexels, V4{2, 4, 20, 400});
    const auto authoredBuffer = MakeBuffer(authored.data(), authored.size() * sizeof(V4));
    struct UploadParams
    {
        V4 min{0, 0, 0, 0};
        V4 size{2, 2, 2, 0};
        std::array<int32_t, 4> count{2, 2, 2, 8};
        std::array<uint32_t, 4> dispatch{0, kProbes, 0, 0};
    } up;
    const auto uploadParams = MakeBuffer(&up, sizeof(up), true);
    TextureDesc td{};
    td.width = kAtlasWidth;
    td.height = kAtlasHeight;
    td.depth = td.mipLevels = td.arrayLayers = td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::TransferSrc);
    const auto atlas = device->CreateTexture(td);
    textures.push_back(atlas);
    ASSERT_TRUE(atlas.IsValid());
    DescriptorSetDesc uploadSetDesc{};
    uploadSetDesc.layout = depthUpload.Layout;
    const auto uploadSet = device->CreateDescriptorSet(uploadSetDesc);
    device->UpdateBufferBinding(uploadSet, Binding(depthUpload, "DDGIParams"), uploadParams, 0,
                                sizeof(up));
    device->UpdateStorageBufferBinding(uploadSet, Binding(depthUpload, "DDGIDepthStateRO"), authoredBuffer, 0,
                                       authored.size() * sizeof(V4));
    device->UpdateStorageBufferBinding(uploadSet, Binding(depthUpload, "DDGIProbeCellRO"), records, 0,
                                       cells.size() * sizeof(V4));
    device->UpdateStorageImageBinding(uploadSet, Binding(depthUpload, "uDepthAtlas"), atlas);
    constexpr size_t kAtlasBytes = size_t{kAtlasWidth} * kAtlasHeight * 4 * sizeof(uint16_t);
    BufferDesc rd{};
    rd.size = kAtlasBytes;
    rd.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    rd.memoryUsage = BufferMemoryUsage::Readback;
    const auto readback = device->CreateBuffer(rd);
    buffers.push_back(readback);
    ASSERT_TRUE(readback.IsValid());
    commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlas, ResourceState::Undefined,
                                                            ResourceState::UnorderedAccess));
    commands->SetPipeline(depthUpload.Pipeline);
    commands->BindDescriptorSet(0, uploadSet, depthUpload.Pipeline);
    commands->Dispatch(kProbes, 1, 1);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(atlas, ResourceState::UnorderedAccess,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(atlas, readback, kAtlasWidth, kAtlasHeight);
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();
    const auto* texels = static_cast<const uint16_t*>(device->MapBuffer(readback));
    ASSERT_NE(texels, nullptr);
    for (uint32_t i = 0; i < kAtlasWidth * kAtlasHeight; ++i)
        for (uint32_t channel = 0; channel < 4; ++channel)
            EXPECT_EQ(texels[i * 4 + channel], kExpectedHalfBits[channel]) << "texel " << i << " channel " << channel;
    device->UnmapBuffer(readback);
}

TEST_F(DDGIBlendComputeTest, WindowedGpuUploadsMatchFullUploadsAcrossBudgetsFilterScrollAndResize)
{
    CheckWindowedUploads();
}
} // namespace
