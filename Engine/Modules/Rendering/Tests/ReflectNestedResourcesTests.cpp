#include <gtest/gtest.h>
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include <nlohmann/json.hpp>
#include <vector>

#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ReflectNestedResources, MeshOrComputeNestedRoundTrip) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    // Prefer mesh shader if present, else compute
    std::vector<uint32_t> code;
    ShaderStageKind stage = ShaderStageKind::Compute;
    if (GameEngine::Rendering::Tests::ReadSpirvWords("mesh_triangle.mesh.spv", code)) {
        stage = ShaderStageKind::Mesh;
    } else {
        ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("pc_compute.comp.spv", code));
        stage = ShaderStageKind::Compute;
    }

    ReflectionOptions opts{};
    StageReflectionResult r{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(stage, code.data(), code.size(), opts, r, &err)) << err;

    ShaderMeta meta = MergeStages({r});
    // Round-trip JSON
    nlohmann::json j = meta; ShaderMeta meta2 = j.get<ShaderMeta>();
    EXPECT_EQ(meta2.Version, meta.Version);
    EXPECT_EQ(meta2.EntryPoints, meta.EntryPoints);
}

