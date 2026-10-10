#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ShaderReflectionConflicts, DetectTypeCountMismatchAcrossStages) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    // VS declares set0,binding0 as UBO; FS declares same as sampler2D
    std::vector<uint8_t> vsBytes = Utils::LoadShaderFile("conflict_vs.vert.spv");
    std::vector<uint8_t> fsBytes = Utils::LoadShaderFile("conflict_fs.frag.spv");
    ASSERT_FALSE(vsBytes.empty()); ASSERT_FALSE(fsBytes.empty());
    ASSERT_EQ(vsBytes.size()%4, 0u); ASSERT_EQ(fsBytes.size()%4, 0u);
    std::vector<uint32_t> vs(vsBytes.size()/4), fs(fsBytes.size()/4);
    std::memcpy(vs.data(), vsBytes.data(), vsBytes.size());
    std::memcpy(fs.data(), fsBytes.data(), fsBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    ShaderMeta meta = MergeStages({rvs, rfs});

    // Expect a conflict note recorded in requirements with detailed types/counts
    bool conflictFound = false;
    bool hasDetail = false;
    for (const auto& req : meta.Requirements) {
        if (req.find("conflict: set=0, binding=0") != std::string::npos) {
            conflictFound = true;
            if (req.find("lhsType=") != std::string::npos && req.find("rhsType=") != std::string::npos) hasDetail = true;
            break;
        }
    }
    EXPECT_TRUE(conflictFound) << "Expected conflict requirement for set=0,binding=0";
    EXPECT_TRUE(hasDetail) << "Expected detailed type/count info in conflict message";
#endif
}

