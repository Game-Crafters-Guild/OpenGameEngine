#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

static std::unique_ptr<IDevice> CreateVulkanDevice(const char* appName) {
    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = appName;
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc)) return nullptr;
    return device;
}

TEST(TextureViewDepthAspectTests, CreateDepthViewAndBind) {
    auto device = CreateVulkanDevice("TexView_DepthAspect");
    if (!device) GTEST_SKIP() << "No Vulkan device available";

    // Create a depth texture suitable for sampling
    TextureDesc td{};
    td.width = 32; td.height = 32; td.mipLevels = 1; td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::DepthStencil);
    td.debugName = "DepthTex";

    TextureHandle depthTex = device->CreateTexture(td);
    ASSERT_TRUE(depthTex.IsValid());

    // Create a depth-only view
    TextureViewDesc vd{}; vd.aspect = TextureAspect::Depth; vd.debugName = "DepthView";
    TextureViewHandle depthView = device->CreateTextureView(depthTex, vd);
    ASSERT_TRUE(depthView.IsValid());

    // Create minimal descriptor set and bind as sampled depth
    SamplerDesc sd = SamplerDesc::MaterialLinearRepeat("DepthSampler");
    SamplerHandle samp = device->CreateSampler(sd);
    ASSERT_TRUE(samp.IsValid());

    DescriptorSetDesc ds{}; ds.debugName = "DepthDS";
    ds.layout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1});
    DescriptorSetHandle set = device->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    EXPECT_NO_THROW({ device->UpdateCombinedImageSamplerBinding(set, 0, depthView, samp); });

    device->DestroyDescriptorSet(set);
    device->DestroySampler(samp);
    device->DestroyTextureView(depthView);
    device->DestroyTexture(depthTex);
    device->Shutdown();
}

