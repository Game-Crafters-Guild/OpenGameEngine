#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Tests/TestUtils.h"

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Tests;

TEST(GraphicsPipelineReflection, CreatePipelineWithReflectedLayouts)
{
    DeviceDesc desc{}; desc.applicationName = "GraphicsPipelineReflection"; desc.preferredAPI = GraphicsAPI::Vulkan; desc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(desc);
    if (!dev || !dev->Initialize(desc)) { GTEST_SKIP() << "Device init failed"; }

#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif

    // Load SPIR-V blobs for a minimal triangle
    std::vector<uint8_t> vs, fs;
    ASSERT_TRUE(ReadSpirvBytes("triangle.vert.spv", vs));
    ASSERT_TRUE(ReadSpirvBytes("triangle.frag.spv", fs));

    PipelineDesc p{}; p.type = PipelineType::Graphics; p.debugName = "ReflectedTriangle";
    p.vertexShader = vs; p.pixelShader = fs;
    // Leave descriptorSetLayouts, pushConstantSize/stages empty to force reflection path
    p.colorAttachmentFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };

    auto pipeline = dev->CreatePipeline(p);
    ASSERT_NE(pipeline, INVALID_HANDLE);
}

