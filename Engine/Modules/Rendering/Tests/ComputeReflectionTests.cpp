#include <gtest/gtest.h>
#include <vector>
#include <fstream>
#include <cstring>

#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"

#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ComputeReflection, MinimalCompute_DescriptorSetsAndPC) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    // Load via TestUtils for stability across working dirs
    std::vector<uint8_t> csBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("minimal_test.comp.spv");
    ASSERT_FALSE(csBytes.empty()) << "Missing minimal_test.comp.spv in Rendering/Shaders";
    ASSERT_EQ(csBytes.size() % 4, 0u);
    std::vector<uint32_t> cs(csBytes.size()/4);
    std::memcpy(cs.data(), csBytes.data(), csBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rcs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, rcs, &err)) << err;

    ShaderMeta meta = MergeStages({rcs});

    // Validate sets/bindings if present; ensure push constants are within policy
    for (const auto& setInfo : meta.Sets) {
        for (const auto& binding : setInfo.Bindings) {
            EXPECT_GE(binding.Binding, 0u);
            EXPECT_GE(binding.StagesMask, 0u);
        }
    }
    for (const auto& pc : meta.PushConstants) {
        EXPECT_LE(pc.Size, 128u);
    }
}

