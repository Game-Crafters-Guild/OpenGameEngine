#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DescriptorWriter.h"

using namespace GameEngine::Rendering;

TEST(DescriptorWriterArray, MultipleBuffersAndImagesCoalesce) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding 0 = UBO[3], binding 1 = CombinedImageSampler[2]
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b0{}; b0.binding = 0; b0.type = DescriptorType::UniformBuffer; b0.count = 3; b0.shaderStages = 0x20;
    DescriptorBinding b1{}; b1.binding = 1; b1.type = DescriptorType::CombinedImageSampler; b1.count = 2; b1.shaderStages = 0x10;
    layout.bindings = { b0, b1 };

    DescriptorSetDesc ds{}; ds.layout = layout; ds.transient = true; ds.debugName = "DW_Array";
    auto set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    // Resources
    std::vector<BufferHandle> bufs;
    std::vector<size_t> offs, sizes;
    for (int i = 0; i < 3; ++i) {
        auto h = dev->CreateUploadBuffer(256, "Ubo");
        ASSERT_TRUE(h.IsValid());
        bufs.push_back(h); offs.push_back(0); sizes.push_back(256);
    }
    SamplerDesc sd{}; sd.debugName = "Samp"; auto s = dev->CreateSampler(sd); ASSERT_TRUE(s.IsValid());
    TextureDesc td{}; td.width=1; td.height=1; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)TextureUsage::ShaderResource;
    auto t0 = dev->CreateTexture(td); ASSERT_TRUE(t0.IsValid());
    auto t1 = dev->CreateTexture(td); ASSERT_TRUE(t1.IsValid());

    DescriptorWriter w(dev.get(), set);
    w.AddUniformBuffers(0, bufs, offs, sizes)
     .AddCombinedImageSamplers(1, {t0, t1}, {s, s});

    // Prefer coalesced flush
    w.FlushCoalesced();

    // Cleanup
    dev->DestroyDescriptorSet(set);
    dev->DestroyTexture(t1);
    dev->DestroyTexture(t0);
    dev->DestroySampler(s);
    for (auto b : bufs) dev->DestroyBuffer(b);
}

