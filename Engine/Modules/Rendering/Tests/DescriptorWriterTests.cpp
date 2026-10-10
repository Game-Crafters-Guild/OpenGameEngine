#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DescriptorWriter.h"

using namespace GameEngine::Rendering;

TEST(DescriptorWriter, BatchUpdatesSmoke) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding 0 = UBO, binding 1 = CombinedImageSampler
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b0{}; b0.binding = 0; b0.type = DescriptorType::UniformBuffer; b0.count = 1; b0.shaderStages = 0x20; // compute
    DescriptorBinding b1{}; b1.binding = 1; b1.type = DescriptorType::CombinedImageSampler; b1.count = 1; b1.shaderStages = 0x10; // fragment
    layout.bindings.push_back(b0);
    layout.bindings.push_back(b1);

    DescriptorSetDesc ds{}; ds.layout = layout; ds.transient = true; ds.debugName = "DW_Set";
    DescriptorSetHandle h = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(h.IsValid());

    // Resources
    BufferHandle ubo = dev->CreateUploadBuffer(256, "DW_UBO");
    ASSERT_TRUE(ubo.IsValid());
    SamplerDesc sd{}; sd.debugName = "DW_Sampler";
    SamplerHandle samp = dev->CreateSampler(sd);
    ASSERT_TRUE(samp.IsValid());
    TextureDesc td{}; td.width = 1; td.height = 1; td.format = (uint32_t)TextureFormat::RGBA8_UNORM; td.usage = (uint32_t)TextureUsage::ShaderResource;
    TextureHandle tex = dev->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    // Writer: stage both bindings then flush
    DescriptorWriter writer(dev.get(), h);
    writer.AddUniformBuffer(0, ubo, 0, 256)
          .AddCombinedImageSampler(1, tex, samp)
          .Flush();

    // Cleanup
    dev->DestroyDescriptorSet(h);
    dev->DestroyTexture(tex);
    dev->DestroySampler(samp);
    dev->DestroyBuffer(ubo);

    SUCCEED();
}

