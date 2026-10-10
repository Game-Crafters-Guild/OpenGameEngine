#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

TEST(StorageImageDescriptorHelpers, UpdateStorageImageBinding_Smoke) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Create a descriptor set layout with a single storage image binding at slot 0
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::StorageImage; b.count = 1; b.shaderStages = 0; b.debugName = "StorageImage0";
    layout.bindings.push_back(b); layout.debugName = "StorageImageLayout";

    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "StorageImageDS"; ds.transient = true;
    DescriptorSetHandle set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    // Create a 1x1 RGBA8 texture with UAV (unordered access) usage
    TextureDesc td{}; td.width = 1; td.height = 1; td.format = (uint32_t)TextureFormat::RGBA8_UNORM; td.usage = (uint32_t)TextureUsage::UnorderedAccess; td.debugName = "UAV_Tex_1x1";
    TextureHandle tex = dev->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    // Smoke: update binding via convenience helper (should not assert/crash)
    dev->UpdateStorageImageBinding(set, 0, tex);

    // Cleanup
    dev->DestroyTexture(tex);
    dev->DestroyDescriptorSet(set);
}

