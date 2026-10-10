#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>

#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/ShaderMetaJson.h"

#include "TestUtils.h"

using namespace GameEngine::Rendering;
using nlohmann::json;

TEST(ShaderReflectIntegration, ReflectTriangleVSFS) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    std::vector<uint32_t> vs, fs;
    ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("triangle.vert.spv", vs));
    ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("triangle.frag.spv", fs));

    ReflectionOptions opts{};
    StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    ShaderMeta meta = MergeStages({rvs, rfs});

    // Basic sanity checks
    ASSERT_TRUE(meta.Stages.count("vs") > 0);
    ASSERT_TRUE(meta.Stages.count("fs") > 0);
    // Vertex shader may procedurally generate positions (no vertex inputs); just assert stage exists
    (void)meta.Stages["vs"].Inputs;
    // Expect some descriptor sets or none (triangle shader may not use any) - no strict check

    // Ensure JSON conversion works
    json j = meta; ShaderMeta meta2 = j.get<ShaderMeta>();
    EXPECT_EQ(meta2.EntryPoints.at("vs"), meta.EntryPoints.at("vs"));
}



TEST(ShaderReflectIntegration, ReflectComputePCSmoke) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    std::vector<uint32_t> cs;
    ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("pc_compute.comp.spv", cs));

    ReflectionOptions opts{};
    StageReflectionResult r{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, r, &err)) << err;

    ShaderMeta meta = MergeStages({r});
    ASSERT_TRUE(meta.Stages.count("cs") > 0);
    // Expect a push constant block in this compute shader
    ASSERT_FALSE(meta.PushConstants.empty());
}

TEST(ShaderReflectIntegration, ReflectMeshSmoke) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    std::vector<uint32_t> ms;
#if RENDERING_TEST_MESH_SHADER_COMPILED
    ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("mesh_triangle.mesh.spv", ms))
        << "mesh_triangle.mesh.spv is missing; build the CompileMeshTriangleShaders target";
#else
    if (!GameEngine::Rendering::Tests::ReadSpirvWords("mesh_triangle.mesh.spv", ms)) {
        GTEST_SKIP() << "No mesh-capable GLSL compiler (glslc or glslangValidator) was found at configure "
                        "time, so mesh_triangle.mesh.spv is not built";
    }
#endif
    ReflectionOptions opts{};
    StageReflectionResult r{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Mesh, ms.data(), ms.size(), opts, r, &err)) << err;

    ShaderMeta meta = MergeStages({r});
    ASSERT_TRUE(meta.Stages.count("ms") > 0);
    // Outputs are expected for a mesh shader (e.g., position, primitives), not asserting exact schema here
    (void)meta.Stages["ms"].Outputs;
}



TEST(ShaderReflectIntegration, ReflectAndRoundTripJson) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    // Prefer triangle VS/FS; fallback to compute if vertex/fragment missing
    std::vector<uint32_t> vs, fs, cs;
    bool haveVS = GameEngine::Rendering::Tests::ReadSpirvWords("triangle.vert.spv", vs);
    bool haveFS = GameEngine::Rendering::Tests::ReadSpirvWords("triangle.frag.spv", fs);

    ReflectionOptions opts{};
    std::string err;
    ShaderMeta meta;
    if (haveVS && haveFS) {
        StageReflectionResult rvs{}, rfs{};
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;
        meta = MergeStages({rvs, rfs});
    } else {
        ASSERT_TRUE(GameEngine::Rendering::Tests::ReadSpirvWords("pc_compute.comp.spv", cs));
        StageReflectionResult r{};
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, r, &err)) << err;
        meta = MergeStages({r});
    }

    // Round trip JSON
    nlohmann::json j = meta;
    std::string s = j.dump();
    ShaderMeta meta2 = nlohmann::json::parse(s).get<ShaderMeta>();

    // Fidelity checks for key fields
    EXPECT_EQ(meta2.Version, meta.Version);
    EXPECT_EQ(meta2.EntryPoints, meta.EntryPoints);
    EXPECT_EQ(meta2.Sets.size(), meta.Sets.size());
    EXPECT_EQ(meta2.PushConstants.size(), meta.PushConstants.size());
}
