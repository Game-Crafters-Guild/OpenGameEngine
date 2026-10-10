#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Materials/ShaderMeta.h"

using namespace GameEngine::Rendering;

static ShaderMeta MakeSimpleMeta() {
    ShaderMeta m{};
    DescriptorSetMeta s0{}; s0.Set = 0;
    DescriptorBindingMeta ubo{}; ubo.Binding = 0; ubo.Name = "Globals"; ubo.Type = (uint32_t)DescriptorType::UniformBuffer; ubo.Count = 1; ubo.StagesMask = 0x20; // compute
    DescriptorBindingMeta cis{}; cis.Binding = 1; cis.Name = "Albedo"; cis.Type = (uint32_t)DescriptorType::CombinedImageSampler; cis.Count = 1; cis.StagesMask = 0x10; // fragment
    s0.Bindings.push_back(ubo);
    s0.Bindings.push_back(cis);
    m.Sets.push_back(s0);
    return m;
}

TEST(NamedDescriptorWriter, ResolvesBindingsAndFlushes) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Create layout from our meta for set 0
    auto meta = MakeSimpleMeta();
    DescriptorSetLayoutDesc layout{};
    for (const auto& s : meta.Sets) if (s.Set == 0) {
        for (const auto& b : s.Bindings) {
            DescriptorBinding db{}; db.binding = b.Binding; db.type = (DescriptorType)b.Type; db.count = b.Count; db.shaderStages = b.StagesMask; db.debugName = b.Name.c_str();
            layout.bindings.push_back(db);
        }
    }
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "NamedWriterSet"; ds.transient = true;
    DescriptorSetHandle set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    // Make dummy resources to bind
    BufferHandle ubo = dev->CreateUploadBuffer(256, "Ubo");
    ASSERT_TRUE(ubo.IsValid());
    TextureDesc td{}; td.width=1; td.height=1; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)TextureUsage::ShaderResource;
    TextureHandle tex = dev->CreateTexture(td); ASSERT_TRUE(tex.IsValid());
    SamplerDesc sd{}; sd.debugName = "Samp"; SamplerHandle samp = dev->CreateSampler(sd); ASSERT_TRUE(samp.IsValid());

    NamedDescriptorWriter writer(dev.get(), set, meta, /*setIndex*/0);
    ASSERT_TRUE(writer.Has("Globals"));
    ASSERT_TRUE(writer.Has("Albedo"));
    ASSERT_FALSE(writer.Has("Nonexistent"));

    writer.AddUniformBuffer("Globals", ubo, 0, 256)
          .AddCombinedImageSampler("Albedo", tex, samp)
          .Flush();

    // Cleanup
    dev->DestroyDescriptorSet(set);
    dev->DestroySampler(samp);
    dev->DestroyTexture(tex);
    dev->DestroyBuffer(ubo);

    SUCCEED();
}

