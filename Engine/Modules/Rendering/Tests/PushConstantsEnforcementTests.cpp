#include <gtest/gtest.h>
#include <vector>
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Core/Handle.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

// This test validates that the API enforces push constant size policy during pipeline creation
// by deriving the required size from reflection when unset and failing if explicitly over 128B.
TEST(PushConstantsEnforcement, PipelineCreationFailsWhenPCTooLarge) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan; desc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc)) { GTEST_SKIP() << "Device init failed"; }

    // Use minimal triangle shaders (no push constants), then inject an oversized PC to ensure failure.
    // CompileShaders, a dependency of GameEngineRendering, writes them to the build output.
    const std::vector<uint8_t> vs = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.vert.spv");
    const std::vector<uint8_t> fs = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.frag.spv");
    ASSERT_FALSE(vs.empty()) << "triangle.vert.spv is missing from " << RENDERING_SHADER_OUTPUT_DIR
                             << "; build the CompileShaders target";
    ASSERT_FALSE(fs.empty()) << "triangle.frag.spv is missing from " << RENDERING_SHADER_OUTPUT_DIR
                             << "; build the CompileShaders target";

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "PCTooLarge";
    p.vertexShader = vs; p.pixelShader = fs;
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    // Enforce an explicit too-large push constant block to simulate violation
    p.pushConstantSize = 512; // bytes

    auto pipeline = dev->CreatePipeline(p);
    // Expect invalid handle on failure enforcing policy
    EXPECT_EQ(pipeline, INVALID_PIPELINE_HANDLE);
}

