#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(PushConstantStageMaskEndToEnd, Compute_ReflectedMaskPropagatesToPipeline) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    PipelineDesc pd{}; pd.type = PipelineType::Compute; pd.debugName = "PC_Mask_EndToEnd_Compute";

    // Load compute shader
    pd.computeShader = Utils::LoadShaderFile("pc_compute.comp.spv");
    ASSERT_FALSE(pd.computeShader.empty());

    // Explicit push-constant metadata: 64 bytes in compute stage only (VK_SHADER_STAGE_COMPUTE_BIT = 0x00000020)
    pd.pushConstantSize = 64;
    pd.pushConstantStagesMask = 0x00000020u;

    PipelineHandle ph = dev->CreatePipeline(pd);
    ASSERT_TRUE(ph.IsValid());

    // Verify pipeline stored push-constant layout fields reflect provided mask via introspection API
    PipelinePushConstantInfo info{};
    ASSERT_TRUE(dev->GetPipelinePushConstantInfo(ph, info));
    EXPECT_EQ(info.size, 64u);
    EXPECT_EQ(info.stagesMask, pd.pushConstantStagesMask);
}

