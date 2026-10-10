#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Common/Utils.h"
#include <fstream>

#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ComputePipelineReflection, CreateComputePipelineFromMeta) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    // Create device
    DeviceDesc d{}; d.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(d);
    ASSERT_TRUE(dev && dev->Initialize(d));

    // Read compute SPIR-V using test helper
    std::vector<uint8_t> csBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("minimal_test.comp.spv");
    ASSERT_FALSE(csBytes.empty());
    ASSERT_EQ(csBytes.size() % 4, 0u);
    std::vector<uint32_t> csWords(csBytes.size()/4);
    std::memcpy(csWords.data(), csBytes.data(), csBytes.size());

    // Reflect to meta
    StageReflectionResult r{}; ReflectionOptions opts{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, csWords.data(), csWords.size(), opts, r, &err)) << err;
    ShaderMeta meta = MergeStages({ r });

    // Build pipeline desc
    PipelineDesc pd{}; pd.type = PipelineType::Compute;
    pd.computeShader.resize(csWords.size()*sizeof(uint32_t));
    std::memcpy(pd.computeShader.data(), csWords.data(), pd.computeShader.size());
    MaterialBuilder::BuildPipelineDescFromMeta(meta, pd);

    // Create pipeline
    auto p = dev->CreatePipeline(pd);
    ASSERT_TRUE(p.IsValid());
}

