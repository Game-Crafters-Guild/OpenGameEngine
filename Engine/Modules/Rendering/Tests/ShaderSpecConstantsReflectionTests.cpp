#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ShaderSpecConstantsReflection, DetectIdsFromComputeShader) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    // Ensure SPIR-V exists (glslc auto-compiles .comp to .spv in build)
    std::vector<uint8_t> csBytes = Utils::LoadShaderFile("spec_constants.comp.spv");
    ASSERT_FALSE(csBytes.empty()) << "Missing spec_constants.comp.spv";
    ASSERT_EQ(csBytes.size() % 4, 0u);
    std::vector<uint32_t> cs(csBytes.size()/4);
    std::memcpy(cs.data(), csBytes.data(), csBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rcs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, rcs, &err)) << err;

    ShaderMeta meta = MergeStages({rcs});

    // Expect spec constants with Ids 0 and 1 present
    bool has0=false, has1=false;
    for (const auto& sc : meta.SpecConstants) {
        if (sc.Id == 0u) has0 = true;
        if (sc.Id == 1u) has1 = true;
    }
    EXPECT_TRUE(has0) << "Spec constant id 0 not found";
    EXPECT_TRUE(has1) << "Spec constant id 1 not found";
#endif
}

