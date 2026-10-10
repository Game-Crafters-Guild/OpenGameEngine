#include <gtest/gtest.h>
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include <cstring>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;

static std::unique_ptr<IDevice> CreateVulkanDevice(const char* appName) {
    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = appName;
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc)) {
        return nullptr;
    }
    return device;
}

TEST(TextureViewTests, CreateDestroy_Succeeds) {
    auto device = CreateVulkanDevice("TextureViewTests_CreateDestroy");
    if (!device) GTEST_SKIP() << "No Vulkan device available";

    TextureDesc td{};
    td.width = 64; td.height = 64; td.mipLevels = 1; td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = "TexViewTestTex";

    TextureHandle tex = device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    TextureViewDesc vd{};
    vd.aspect = TextureAspect::Color;
    vd.r = TextureSwizzle::G; vd.g = TextureSwizzle::B; vd.b = TextureSwizzle::R; vd.a = TextureSwizzle::A;
    vd.debugName = "TexViewTestView";

    TextureViewHandle view = device->CreateTextureView(tex, vd);
    EXPECT_TRUE(view.IsValid());

    device->DestroyTextureView(view);
    device->DestroyTexture(tex);

    device->Shutdown();
}

TEST(TextureViewTests, DescriptorUpdate_WithView_Succeeds) {
    auto device = CreateVulkanDevice("TextureViewTests_DescriptorUpdate");
    if (!device) GTEST_SKIP() << "No Vulkan device available";

    // Create texture + view
    TextureDesc td{};
    td.width = 32; td.height = 32; td.mipLevels = 1; td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = "TexViewDescUpdateTex";
    TextureHandle tex = device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    TextureViewDesc vd{};
    vd.aspect = TextureAspect::Color;
    TextureViewHandle view = device->CreateTextureView(tex, vd);
    ASSERT_TRUE(view.IsValid());

    // Create a simple sampler
    SamplerDesc sd = SamplerDesc::MaterialLinearRepeat("TexViewTestSampler");
    SamplerHandle samp = device->CreateSampler(sd);
    ASSERT_TRUE(samp.IsValid());

    // Create a descriptor set with one combined image sampler binding
    DescriptorSetDesc ds{};
    ds.debugName = "TexViewDescSet";
    ds.layout.bindings.push_back({/*binding*/0, DescriptorType::CombinedImageSampler, /*count*/1});
    DescriptorSetHandle set = device->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    // Update using the view overload (should not assert/crash)
    EXPECT_NO_THROW({ device->UpdateCombinedImageSamplerBinding(set, 0, view, samp); });

    // Cleanup
    device->DestroyDescriptorSet(set);
    device->DestroySampler(samp);
    device->DestroyTextureView(view);
    device->DestroyTexture(tex);
    device->Shutdown();
}

TEST(TextureViewTests, Texture3D_CreateViewAndUpload_Succeeds) {
    auto device = CreateVulkanDevice("TextureViewTests_Texture3D");
    if (!device) GTEST_SKIP() << "No Vulkan device available";

    TextureDesc td{};
    td.width = 8;
    td.height = 4;
    td.depth = 4;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = "Tex3DTestTex";

    TextureHandle tex = device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    TextureViewDesc vd{};
    vd.aspect = TextureAspect::Color;
    vd.viewType = TextureViewType::View3D;
    vd.debugName = "Tex3DTestView";
    TextureViewHandle view = device->CreateTextureView(tex, vd);
    EXPECT_TRUE(view.IsValid());

    std::vector<uint8_t> voxels(static_cast<size_t>(td.width) * td.height * td.depth * 4u, 0);
    for (uint32_t z = 0; z < td.depth; ++z) {
        for (uint32_t y = 0; y < td.height; ++y) {
            for (uint32_t x = 0; x < td.width; ++x) {
                const size_t i = (((static_cast<size_t>(z) * td.height + y) * td.width) + x) * 4u;
                voxels[i + 0] = static_cast<uint8_t>(x * 16u);
                voxels[i + 1] = static_cast<uint8_t>(y * 32u);
                voxels[i + 2] = static_cast<uint8_t>(z * 48u);
                voxels[i + 3] = 255u;
            }
        }
    }

    EXPECT_NO_THROW({
        UploadTexture3D(device.get(), tex, voxels.data(), td.width, td.height, td.depth, td.width * 4u);
        device->WaitForIdle();
    });

    device->DestroyTextureView(view);
    device->DestroyTexture(tex);
    device->Shutdown();
}

// End-to-end roundtrip: upload a deterministic 3D pattern, read it back via
// CopyTextureSubresourceToBuffer, and verify every voxel matches.
TEST(TextureViewTests, Texture3D_UploadReadback_RoundTrips) {
    auto device = CreateVulkanDevice("TextureViewTests_Texture3DReadback");
    if (!device) GTEST_SKIP() << "No Vulkan device available";

    constexpr uint32_t kWidth = 8;
    constexpr uint32_t kHeight = 4;
    constexpr uint32_t kDepth = 4;
    constexpr size_t kBytesPerTexel = 4;
    constexpr size_t kVoxelCount = static_cast<size_t>(kWidth) * kHeight * kDepth;
    constexpr size_t kByteCount = kVoxelCount * kBytesPerTexel;

    TextureDesc td{};
    td.width = kWidth;
    td.height = kHeight;
    td.depth = kDepth;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst) |
               static_cast<uint32_t>(TextureUsage::TransferSrc);
    td.debugName = "Tex3DRoundtripTex";

    TextureHandle tex = device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    // Deterministic per-voxel pattern: encode (x, y, z, channel marker).
    std::vector<uint8_t> uploaded(kByteCount, 0);
    for (uint32_t z = 0; z < kDepth; ++z) {
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                const size_t i = (((static_cast<size_t>(z) * kHeight + y) * kWidth) + x) * kBytesPerTexel;
                uploaded[i + 0] = static_cast<uint8_t>(x * 16u);
                uploaded[i + 1] = static_cast<uint8_t>(y * 32u);
                uploaded[i + 2] = static_cast<uint8_t>(z * 48u);
                uploaded[i + 3] = 0xA5u;
            }
        }
    }

    UploadTexture3D(device.get(), tex, uploaded.data(), kWidth, kHeight, kDepth, kWidth * kBytesPerTexel);

    // Read the full volume back in a single CopyTextureSubresourceToBuffer call.
    BufferHandle readback = device->CreateReadbackBuffer(kByteCount, "Tex3DReadbackBuffer");
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::ShaderResource, ResourceState::CopySource));
    cl->CopyTextureSubresourceToBuffer(
        tex, /*mip*/0, /*layer*/0,
        readback, kWidth, kHeight,
        /*SrcX*/0, /*SrcY*/0,
        /*DstOffsetBytes*/0, /*dstRowPitchBytes*/kWidth * kBytesPerTexel,
        /*depth*/kDepth, /*dstSlicePitchBytes*/0);
    cl->End();
    CommandList* raw = cl.get();
    device->ExecuteCommandLists({raw});
    device->WaitForIdle();

    const void* mapped = device->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);

    std::vector<uint8_t> downloaded(kByteCount, 0);
    std::memcpy(downloaded.data(), mapped, kByteCount);
    device->UnmapBuffer(readback);

    // Every voxel must roundtrip exactly.
    for (size_t i = 0; i < kByteCount; ++i) {
        ASSERT_EQ(downloaded[i], uploaded[i]) << "byte " << i << " mismatch";
    }

    device->DestroyBuffer(readback);
    device->DestroyTexture(tex);
    device->Shutdown();
}
