#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(PushConstantPolicy, PipelineCreationFailsWhenOverLimit_Graphics) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.maxPushConstantBytes = 64; // tighten policy
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    std::vector<uint8_t> vs = Utils::LoadShaderFile("triangle.vert.spv");
    std::vector<uint8_t> fs = Utils::LoadShaderFile("triangle.frag.spv");
    ASSERT_FALSE(vs.empty());
    ASSERT_FALSE(fs.empty());

    PipelineDesc pd{}; pd.type = PipelineType::Graphics;
    pd.vertexShader = vs; pd.pixelShader = fs;
    pd.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };
    pd.pushConstantSize = 80; // exceed policy 64

    auto ph = dev->CreatePipeline(pd);
    EXPECT_FALSE(ph.IsValid());
}

TEST(PushConstantPolicy, PipelineCreationFailsWhenOverLimit_Compute) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.maxPushConstantBytes = 32;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    std::vector<uint8_t> cs = Utils::LoadShaderFile("minimal_test.comp.spv");
    ASSERT_FALSE(cs.empty());
    PipelineDesc pd{}; pd.type = PipelineType::Compute;
    pd.computeShader = cs;
    pd.pushConstantSize = 64; // exceed policy 32

    auto ph = dev->CreatePipeline(pd);
    EXPECT_FALSE(ph.IsValid());
}

