#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "../Source/Vulkan/VulkanDevice.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

static bool ReadFileResolved(const char* name, std::vector<uint8_t>& out) {
    out = Utils::LoadShaderFile(name);
    return !out.empty();
}

static bool ReadWordsResolved(const char* name, std::vector<uint32_t>& out) {
    std::vector<uint8_t> bytes; if (!ReadFileResolved(name, bytes)) return false;
    if (bytes.size() % 4 != 0) return false;
    out.resize(bytes.size()/4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

TEST(PipelineLayoutDedup, GraphicsTwoIdenticalPipelinesReuseLayouts) {
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true; // headless: no swapchain render pass — dynamic rendering is required for graphics pipeline creation
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    // Build minimal ShaderMeta with one UBO at set0/binding0 used in VS|FS
    ShaderMeta meta{};
    DescriptorBindingMeta b0{}; b0.Binding = 0; b0.Name = "PerFrame"; b0.Type = 0; b0.Count = 1; b0.StagesMask = (1<<0) | (1<<1);
    DescriptorSetMeta s0{}; s0.Set = 0; s0.Bindings = { b0 };
    meta.Sets.push_back(s0);

    // Build DescriptorSetLayoutDesc via MaterialBuilder
    auto setLayouts = MaterialBuilder::BuildSetLayouts(meta);

    // Load shaders
    std::vector<uint8_t> vsBytes, fsBytes;
    ASSERT_TRUE(ReadFileResolved("triangle.vert.spv", vsBytes));
    ASSERT_TRUE(ReadFileResolved("triangle.frag.spv", fsBytes));

    auto makeDesc = [&](const char* name){
        PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = name;
        pd.vertexShader = vsBytes; pd.pixelShader = fsBytes;
        pd.descriptorSetLayouts = setLayouts;
        // Provide color format for dynamic rendering so graphics pipeline can compile
        pd.colorAttachmentFormats = { (uint32_t)TextureFormat::BGRA8_UNORM }; // engine TextureFormat, NOT raw VkFormat
        // Default states are fine
        return pd;
    };

    auto p1 = dev->CreatePipeline(makeDesc("GfxDedupA"));
    auto p2 = dev->CreatePipeline(makeDesc("GfxDedupB"));
    ASSERT_TRUE(p1.IsValid());
    ASSERT_TRUE(p2.IsValid());

    // Downcast to VulkanDevice to inspect native layouts
    auto* vkDev = dynamic_cast<VulkanDevice*>(dev.get());
    ASSERT_NE(vkDev, nullptr);

    VkPipelineLayout l1 = vkDev->GetPipelineLayout(p1);
    VkPipelineLayout l2 = vkDev->GetPipelineLayout(p2);
    ASSERT_NE(l1, VK_NULL_HANDLE);
    ASSERT_NE(l2, VK_NULL_HANDLE);
    EXPECT_EQ(l1, l2) << "Pipeline layouts should be deduplicated";

    VkDescriptorSetLayout s1 = vkDev->GetFirstDescriptorSetLayoutForPipeline(p1);
    VkDescriptorSetLayout s2 = vkDev->GetFirstDescriptorSetLayoutForPipeline(p2);
    ASSERT_NE(s1, VK_NULL_HANDLE);
    ASSERT_NE(s2, VK_NULL_HANDLE);
    EXPECT_EQ(s1, s2) << "Descriptor set layouts should be deduplicated";
}

TEST(PipelineLayoutDedup, ComputeTwoIdenticalPipelinesReuseLayouts) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true; // headless: no swapchain render pass — dynamic rendering is required for graphics pipeline creation
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    std::vector<uint32_t> csWords;
    ASSERT_TRUE(ReadWordsResolved("minimal_test.comp.spv", csWords));

    StageReflectionResult r{}; ReflectionOptions opts{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, csWords.data(), csWords.size(), opts, r, &err)) << err;
    ShaderMeta meta = MergeStages({ r });

    auto makeDesc = [&](const char* name){
        PipelineDesc pd{}; pd.type = PipelineType::Compute; pd.debugName = name;
        pd.computeShader.resize(csWords.size()*sizeof(uint32_t));
        std::memcpy(pd.computeShader.data(), csWords.data(), pd.computeShader.size());
        MaterialBuilder::BuildPipelineDescFromMeta(meta, pd);
        return pd;
    };

    auto p1 = dev->CreatePipeline(makeDesc("CompDedupA"));
    auto p2 = dev->CreatePipeline(makeDesc("CompDedupB"));
    ASSERT_TRUE(p1.IsValid());
    ASSERT_TRUE(p2.IsValid());

    auto* vkDev = dynamic_cast<VulkanDevice*>(dev.get());
    ASSERT_NE(vkDev, nullptr);

    VkPipelineLayout l1 = vkDev->GetPipelineLayout(p1);
    VkPipelineLayout l2 = vkDev->GetPipelineLayout(p2);
    ASSERT_NE(l1, VK_NULL_HANDLE);
    ASSERT_NE(l2, VK_NULL_HANDLE);
    EXPECT_EQ(l1, l2) << "Compute pipeline layouts should be deduplicated";

    VkDescriptorSetLayout s1 = vkDev->GetFirstDescriptorSetLayoutForPipeline(p1);
    VkDescriptorSetLayout s2 = vkDev->GetFirstDescriptorSetLayoutForPipeline(p2);
    ASSERT_NE(s1, VK_NULL_HANDLE);
    ASSERT_NE(s2, VK_NULL_HANDLE);
    EXPECT_EQ(s1, s2) << "Compute descriptor set layouts should be deduplicated";
}



// Canonical graphics push range (kGraphicsPushConstantStages, [0, policy max)):
// graphics pipeline layouts that agree on descriptor set layouts must be the
// SAME VkPipelineLayout regardless of declared push-constant size or stages —
// that identity is what keeps descriptor binds that survive a pipeline switch
// (MaterialBinder sticky binds) compatible (VUID-*-None-08600).
TEST(PipelineLayoutDedup, GraphicsPushConstantSizeVariantsUnifyLayouts) {
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true; // headless: no swapchain render pass — dynamic rendering is required for graphics pipeline creation
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    // Minimal set layouts: one UBO in VS
    DescriptorSetLayoutDesc set0{}; DescriptorBinding b0{};
    b0.binding = 0; b0.type = DescriptorType::UniformBuffer; b0.count = 1; b0.shaderStages = VK_SHADER_STAGE_VERTEX_BIT;
    set0.bindings.push_back(b0);

    std::vector<uint8_t> vsBytes, fsBytes;
    ASSERT_TRUE(ReadFileResolved("triangle.vert.spv", vsBytes));
    ASSERT_TRUE(ReadFileResolved("triangle.frag.spv", fsBytes));

    auto makeDesc = [&](uint32_t pcSize){
        PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "PCSize";
        pd.vertexShader = vsBytes; pd.pixelShader = fsBytes;
        pd.descriptorSetLayouts = { set0 };
        pd.colorAttachmentFormats = { (uint32_t)TextureFormat::BGRA8_UNORM }; // engine TextureFormat, NOT raw VkFormat
        pd.pushConstantSize = pcSize; pd.pushConstantStagesMask = (uint32_t)VK_SHADER_STAGE_VERTEX_BIT;
        return pd;
    };

    auto p1 = dev->CreatePipeline(makeDesc(32));
    auto p2 = dev->CreatePipeline(makeDesc(64));
    ASSERT_TRUE(p1.IsValid()); ASSERT_TRUE(p2.IsValid());

    auto* vkDev = dynamic_cast<VulkanDevice*>(dev.get());
    ASSERT_NE(vkDev, nullptr);
    VkPipelineLayout l1 = vkDev->GetPipelineLayout(p1);
    VkPipelineLayout l2 = vkDev->GetPipelineLayout(p2);
    ASSERT_NE(l1, VK_NULL_HANDLE); ASSERT_NE(l2, VK_NULL_HANDLE);
    EXPECT_EQ(l1, l2) << "Canonical push range: declared push size must not fork graphics pipeline layouts";
}

TEST(PipelineLayoutDedup, GraphicsPushConstantStageVariantsUnifyLayouts) {
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true; // headless: no swapchain render pass — dynamic rendering is required for graphics pipeline creation
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    DescriptorSetLayoutDesc set0{}; DescriptorBinding b0{};
    b0.binding = 0; b0.type = DescriptorType::UniformBuffer; b0.count = 1; b0.shaderStages = VK_SHADER_STAGE_VERTEX_BIT;
    set0.bindings.push_back(b0);

    std::vector<uint8_t> vsBytes, fsBytes;
    ASSERT_TRUE(ReadFileResolved("triangle.vert.spv", vsBytes));
    ASSERT_TRUE(ReadFileResolved("triangle.frag.spv", fsBytes));

    auto makeDesc = [&](uint32_t stages){
        PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "PCStages";
        pd.vertexShader = vsBytes; pd.pixelShader = fsBytes;
        pd.descriptorSetLayouts = { set0 };
        pd.colorAttachmentFormats = { (uint32_t)TextureFormat::BGRA8_UNORM }; // engine TextureFormat, NOT raw VkFormat
        pd.pushConstantSize = 32; pd.pushConstantStagesMask = stages;
        return pd;
    };

    // Depth-only variants reflect VERTEX-only push ranges while material
    // variants reflect VERTEX|FRAGMENT — exactly the pair that used to fork
    // layouts and spec-disturb sticky set-0 binds in the depth prepass.
    auto p1 = dev->CreatePipeline(makeDesc((uint32_t)VK_SHADER_STAGE_VERTEX_BIT));
    auto p2 = dev->CreatePipeline(makeDesc((uint32_t)(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)));
    ASSERT_TRUE(p1.IsValid()); ASSERT_TRUE(p2.IsValid());

    auto* vkDev = dynamic_cast<VulkanDevice*>(dev.get());
    ASSERT_NE(vkDev, nullptr);
    VkPipelineLayout l1 = vkDev->GetPipelineLayout(p1);
    VkPipelineLayout l2 = vkDev->GetPipelineLayout(p2);
    ASSERT_NE(l1, VK_NULL_HANDLE); ASSERT_NE(l2, VK_NULL_HANDLE);
    EXPECT_EQ(l1, l2) << "Canonical push range: declared push stages must not fork graphics pipeline layouts";
}

TEST(PipelineLayoutDedup, GraphicsReorderedSetLayoutsProduceDifferentLayouts) {
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true; // headless: no swapchain render pass — dynamic rendering is required for graphics pipeline creation
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    // Two sets with different bindings
    DescriptorSetLayoutDesc setA{}; DescriptorBinding a0{};
    a0.binding = 0; a0.type = DescriptorType::UniformBuffer; a0.count = 1; a0.shaderStages = VK_SHADER_STAGE_VERTEX_BIT; setA.bindings.push_back(a0);
    DescriptorSetLayoutDesc setB{}; DescriptorBinding b0{};
    b0.binding = 0; b0.type = DescriptorType::Sampler; b0.count = 1; b0.shaderStages = VK_SHADER_STAGE_FRAGMENT_BIT; setB.bindings.push_back(b0);

    std::vector<uint8_t> vsBytes, fsBytes;
    ASSERT_TRUE(ReadFileResolved("triangle.vert.spv", vsBytes));
    ASSERT_TRUE(ReadFileResolved("triangle.frag.spv", fsBytes));

    auto makeDesc = [&](bool orderAB){
        PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = orderAB?"OrderAB":"OrderBA";
        pd.vertexShader = vsBytes; pd.pixelShader = fsBytes;
        pd.descriptorSetLayouts = orderAB ? std::vector<DescriptorSetLayoutDesc>{ setA, setB }
                                          : std::vector<DescriptorSetLayoutDesc>{ setB, setA };
        pd.colorAttachmentFormats = { (uint32_t)TextureFormat::BGRA8_UNORM }; // engine TextureFormat, NOT raw VkFormat
        pd.pushConstantSize = 32; pd.pushConstantStagesMask = (uint32_t)(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT);
        return pd;
    };

    auto p1 = dev->CreatePipeline(makeDesc(true));
    auto p2 = dev->CreatePipeline(makeDesc(false));
    ASSERT_TRUE(p1.IsValid()); ASSERT_TRUE(p2.IsValid());

    auto* vkDev = dynamic_cast<VulkanDevice*>(dev.get());
    ASSERT_NE(vkDev, nullptr);
    VkPipelineLayout l1 = vkDev->GetPipelineLayout(p1);
    VkPipelineLayout l2 = vkDev->GetPipelineLayout(p2);
    ASSERT_NE(l1, VK_NULL_HANDLE); ASSERT_NE(l2, VK_NULL_HANDLE);
    EXPECT_NE(l1, l2) << "Reordered set layouts should produce different pipeline layouts (set index order matters)";
}

