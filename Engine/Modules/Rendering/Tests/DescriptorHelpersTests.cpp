#include <gtest/gtest.h>
#include <memory>

#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

TEST(DescriptorHelpers, UpdateBufferBindingConvenience) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Create a simple descriptor set layout: set0, binding0 = UniformBuffer
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = kShaderStageCompute; // compute
    layout.bindings.push_back(b);
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "HelpersDS"; ds.transient = true;
    DescriptorSetHandle dsh = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(dsh.IsValid());

    // Create a small upload buffer and bind via helper
    const size_t kSize = 64;
    BufferHandle buf = dev->CreateUploadBuffer(kSize, "HelpersUploadUBO");
    ASSERT_TRUE(buf.IsValid());

    dev->UpdateBufferBinding(dsh, /*binding*/0, buf, /*offset*/0, /*size*/kSize);

    // Cleanup to avoid VMA assert on leak
    dev->DestroyDescriptorSet(dsh);
    dev->DestroyBuffer(buf);

    SUCCEED();
}

TEST(DescriptorHelpers, UpdateCombinedImageSamplerConvenience_Smoke) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding0 = CombinedImageSampler
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::CombinedImageSampler; b.count = 1; b.shaderStages = 0x10; // fragment
    layout.bindings.push_back(b);
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "HelpersDS_CIS"; ds.transient = true;
    DescriptorSetHandle dsh = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(dsh.IsValid());

    // Create dummy sampler (valid) and a tiny texture (if backend permits creating without data). If not, just ensure helper call doesn't crash.
    SamplerDesc sampDesc{}; sampDesc.debugName = "HelpersSampler";
    SamplerHandle sh = dev->CreateSampler(sampDesc);
    ASSERT_TRUE(sh.IsValid());

    TextureDesc texDesc{}; texDesc.width = 1; texDesc.height = 1; texDesc.format = (uint32_t)TextureFormat::RGBA8_UNORM; texDesc.usage = (uint32_t)TextureUsage::ShaderResource; texDesc.debugName = "HelpersTex";
    TextureHandle th = dev->CreateTexture(texDesc);
    ASSERT_TRUE(th.IsValid());

    // Bind via helper (smoke)
    dev->UpdateCombinedImageSamplerBinding(dsh, 0, th, sh);

    // Cleanup to prevent leaks/asserts
    dev->DestroyDescriptorSet(dsh);
    dev->DestroyTexture(th);
    dev->DestroySampler(sh);

    SUCCEED();
}


TEST(DescriptorHelpers, ArrayHelpersSmoke) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding0 = UBO[3], binding1 = CombinedImageSampler[2]
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b0{}; b0.binding = 0; b0.type = DescriptorType::UniformBuffer; b0.count = 3; b0.shaderStages = 0x20;
    DescriptorBinding b1{}; b1.binding = 1; b1.type = DescriptorType::CombinedImageSampler; b1.count = 2; b1.shaderStages = 0x10;
    layout.bindings.push_back(b0);
    layout.bindings.push_back(b1);
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "HelpersDS_Array"; ds.transient = true;
    DescriptorSetHandle set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    std::vector<BufferHandle> bufs; std::vector<size_t> offs, sizes;
    for (int i=0;i<3;++i) { auto h = dev->CreateUploadBuffer(64, "UBO"); ASSERT_TRUE(h.IsValid()); bufs.push_back(h); offs.push_back(0); sizes.push_back(64); }

    SamplerDesc sd{}; sd.debugName = "Samp"; SamplerHandle sh = dev->CreateSampler(sd); ASSERT_TRUE(sh.IsValid());
    TextureDesc td{}; td.width=1; td.height=1; td.format=(uint32_t)TextureFormat::RGBA8_UNORM; td.usage=(uint32_t)TextureUsage::ShaderResource; td.debugName = "Img";
    TextureHandle t0 = dev->CreateTexture(td); ASSERT_TRUE(t0.IsValid());
    TextureHandle t1 = dev->CreateTexture(td); ASSERT_TRUE(t1.IsValid());

    dev->UpdateUniformBuffers(set, 0, bufs, offs, sizes);
    dev->UpdateCombinedImageSamplers(set, 1, {t0,t1}, {sh,sh});

    // Cleanup
    dev->DestroyDescriptorSet(set);
    for (auto h : bufs) dev->DestroyBuffer(h);
    dev->DestroyTexture(t0); dev->DestroyTexture(t1);
    dev->DestroySampler(sh);

    SUCCEED();
}



TEST(DescriptorHelpers, UpdateImageAndSamplerBindings) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Create layout: binding0=Texture, binding1=Sampler
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b0{}; b0.binding = 0; b0.type = DescriptorType::Texture; b0.count = 1; b0.shaderStages = 0x10; // fragment
    DescriptorBinding b1{}; b1.binding = 1; b1.type = DescriptorType::Sampler; b1.count = 1; b1.shaderStages = 0x10;
    layout.bindings.push_back(b0);
    layout.bindings.push_back(b1);

    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "HelpersDS_TexSamp"; ds.transient = true;
    DescriptorSetHandle dsh = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(dsh.IsValid());

    // Create a 1x1 texture and a sampler
    TextureDesc texDesc{}; texDesc.width = 1; texDesc.height = 1; texDesc.format = (uint32_t)TextureFormat::RGBA8_UNORM; texDesc.usage = (uint32_t)TextureUsage::ShaderResource; texDesc.debugName = "HelpersTex_1x1";
    TextureHandle th = dev->CreateTexture(texDesc);
    ASSERT_TRUE(th.IsValid());

    SamplerDesc sampDesc{}; sampDesc.debugName = "HelpersSampler_Simple";
    SamplerHandle sh = dev->CreateSampler(sampDesc);
    ASSERT_TRUE(sh.IsValid());

    // Bind via helpers
    dev->UpdateImageBinding(dsh, 0, th);
    dev->UpdateSamplerBinding(dsh, 1, sh);

    // Cleanup
    dev->DestroyDescriptorSet(dsh);
    dev->DestroyTexture(th);
    dev->DestroySampler(sh);

    SUCCEED();
}


TEST(DeviceCapabilities, ForceDisableBindless_GatesOff)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.forceDisableBindlessResources = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    const auto& caps = dev->GetCapabilities();
    EXPECT_FALSE(caps.supportsBindlessResources);
}
