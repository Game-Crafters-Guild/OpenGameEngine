#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"
#include "PushBlockMslLayout.h"

using namespace GameEngine::Rendering;

static void BytesToWords(const std::vector<uint8_t>& bytes, std::vector<uint32_t>& out) {
    ASSERT_EQ(bytes.size() % 4, 0u);
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
}

TEST(ShaderReflectionMetadata, VertexFragment_StageIO_Mapping) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    std::vector<uint8_t> vsBytes = Utils::LoadShaderFile("vertex_input_triangle.vert.spv");
    std::vector<uint8_t> fsBytes = Utils::LoadShaderFile("vertex_input_triangle.frag.spv");
    ASSERT_FALSE(vsBytes.empty()); ASSERT_FALSE(fsBytes.empty());

    std::vector<uint32_t> vs, fs; BytesToWords(vsBytes, vs); BytesToWords(fsBytes, fs);
    ReflectionOptions opts{}; StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    ShaderMeta meta = MergeStages({rvs, rfs});

    // Validate stage IO counts and basic types/locations
    ASSERT_TRUE(meta.Stages.count("vs") > 0);
    ASSERT_TRUE(meta.Stages.count("fs") > 0);

    const StageMeta& vsm = meta.Stages.at("vs");
    const StageMeta& fsm = meta.Stages.at("fs");

    // VS: 2 inputs (pos,color) and at least 1 output (color)
    ASSERT_GE(vsm.Inputs.size(), 2u);
    EXPECT_EQ(vsm.Inputs[0].Location, 0u);
    EXPECT_EQ(vsm.Inputs[1].Location, 1u);
    ASSERT_GE(vsm.Outputs.size(), 1u);
    EXPECT_EQ(vsm.Outputs[0].Location, 0u);

    // FS: 1 input at location 0 and 1 output at location 0
    ASSERT_GE(fsm.Inputs.size(), 1u);
    EXPECT_EQ(fsm.Inputs[0].Location, 0u);
    ASSERT_GE(fsm.Outputs.size(), 1u);
    EXPECT_EQ(fsm.Outputs[0].Location, 0u);
#endif
}

// A variable with no Component decoration starts at component 0. The reflection library reports
// its own not-present sentinel for that case, which is not a component index, and every consumer
// of this field reads it as one: the meta serializer omits component 0, and the validator's span
// check treats anything outside 0..3 as claiming the whole location — so a plain varying left
// with the sentinel reports a false overlap against a variable legally packed beside it
// (layout(location = N, component = 2)).
TEST(ShaderReflectionMetadata, UndecoratedStageIOStartsAtComponentZero) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    std::vector<uint8_t> vsBytes = Utils::LoadShaderFile("vertex_input_triangle.vert.spv");
    std::vector<uint8_t> fsBytes = Utils::LoadShaderFile("vertex_input_triangle.frag.spv");
    ASSERT_FALSE(vsBytes.empty()); ASSERT_FALSE(fsBytes.empty());

    std::vector<uint32_t> vs, fs; BytesToWords(vsBytes, vs); BytesToWords(fsBytes, fs);
    ReflectionOptions opts{}; StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    const ShaderMeta meta = MergeStages({rvs, rfs});
    ASSERT_TRUE(meta.Stages.count("vs") > 0);
    ASSERT_TRUE(meta.Stages.count("fs") > 0);

    // The fixture declares no component qualifier anywhere, so every slot it reports must start
    // at 0. A failure here reads as a four-billion component index.
    for (const auto& [stageName, stage] : meta.Stages) {
        for (const auto& v : stage.Inputs)
            EXPECT_EQ(v.Component, 0u) << stageName << " input " << v.Name;
        for (const auto& v : stage.Outputs)
            EXPECT_EQ(v.Component, 0u) << stageName << " output " << v.Name;
    }
#endif
}

TEST(ShaderReflectionMetadata, Compute_SSBO_PushConstants_LocalSize) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    std::vector<uint8_t> csBytes = Utils::LoadShaderFile("frustum_culling.comp.spv");
    ASSERT_FALSE(csBytes.empty());
    std::vector<uint32_t> cs; BytesToWords(csBytes, cs);

    ReflectionOptions opts{}; StageReflectionResult rcs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, cs.data(), cs.size(), opts, rcs, &err)) << err;

    ShaderMeta meta = MergeStages({rcs});

    // Push constants: expect at least 16 bytes (4 uints)
    ASSERT_GE(meta.PushConstants.size(), 1u);
    EXPECT_GE(meta.PushConstants[0].Size, 16u);

    // Descriptor sets: expect at least three SSBO bindings in set 0
    size_t totalBindings = 0; for (const auto& s : meta.Sets) totalBindings += s.Bindings.size();
    EXPECT_GE(totalBindings, 3u);

    // Local size should be reflected
    ASSERT_TRUE(meta.Stages.count("cs") > 0);
    const StageMeta& csm = meta.Stages.at("cs");
    ASSERT_TRUE(csm.ComputeLocalSize.has_value());
    EXPECT_EQ(csm.ComputeLocalSize->X, 64u);
    EXPECT_EQ(csm.ComputeLocalSize->Y, 1u);
    EXPECT_EQ(csm.ComputeLocalSize->Z, 1u);
#endif
}

TEST(ShaderReflectionMetadata, SamplerBinding_FragmentStage) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    std::vector<uint8_t> fsBytes = Utils::LoadShaderFile("sample_texture.frag.spv");
    ASSERT_FALSE(fsBytes.empty());
    std::vector<uint32_t> fs; BytesToWords(fsBytes, fs);

    ReflectionOptions opts{}; StageReflectionResult rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    ShaderMeta meta = MergeStages({rfs});

    // Expect at least one descriptor binding with fragment stage visibility
    size_t bindingsWithFS = 0;
    for (const auto& set : meta.Sets) {
        for (const auto& b : set.Bindings) {
            if (b.StagesMask != 0) bindingsWithFS++;
        }
    }
    EXPECT_GE(bindingsWithFS, 1u);
#endif
}


// Every compiled engine shader's push block ends on a multiple of its largest
// member alignment, so its SPIR-V size (what the engine pushes) equals the
// size of the MSL struct Metal binds (PushBlockMslLayout.h). A block that
// ends short of it — an ivec2 followed by three ints, a vec4 followed by six
// floats — makes the Metal API validation layer abort the draw or dispatch.
// The fix is a trailing pad member in the shader and its C++ mirror.
TEST(ShaderReflectionMetadata, PushBlocksEndOnTheirLargestAlignment) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    namespace fs = std::filesystem;
    const fs::path shaderDir = RENDERING_SHADER_OUTPUT_DIR;
    ASSERT_TRUE(fs::is_directory(shaderDir)) << shaderDir;

    struct StageSuffix { const char* Suffix; ShaderStageKind Stage; };
    constexpr StageSuffix kStages[] = {
        {".vert.spv", ShaderStageKind::Vertex},   {".frag.spv", ShaderStageKind::Fragment},
        {".comp.spv", ShaderStageKind::Compute},  {".geom.spv", ShaderStageKind::Geometry},
        {".mesh.spv", ShaderStageKind::Mesh},     {".tesc.spv", ShaderStageKind::TessControl},
        {".tese.spv", ShaderStageKind::TessEval},
    };

    size_t modules = 0;
    size_t blocks = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(shaderDir)) {
        const std::string name = entry.path().filename().string();
        const StageSuffix* stage = nullptr;
        for (const StageSuffix& s : kStages) {
            const std::string_view suffix = s.Suffix;
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                stage = &s;
        }
        if (!entry.is_regular_file() || stage == nullptr)
            continue;

        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> words;
        BytesToWords(bytes, words);
        ReflectionOptions opts{};
        StageReflectionResult reflected{};
        std::string err;
        ASSERT_TRUE(ReflectSpirv(stage->Stage, words.data(), words.size(), opts, reflected, &err))
            << entry.path() << ": " << err;
        ++modules;
        for (const PushConstantRangeMeta& range : reflected.Pcr) {
            ++blocks;
            EXPECT_EQ(PushBlockMslLayout::SpirvEnd(range.Block), PushBlockMslLayout::MslSize(range.Block))
                << fs::relative(entry.path(), shaderDir).string() << " push block '" << range.Name
                << "' ends at byte " << PushBlockMslLayout::SpirvEnd(range.Block)
                << ", short of its largest alignment ("
                << PushBlockMslLayout::LargestAlignment(range.Block) << "); pad it in the shader and its C++ mirror";
        }
    }
    // The instrument saw the engine's shaders, not an empty or stale directory.
    EXPECT_GT(modules, 100u);
    EXPECT_GT(blocks, 50u);
#endif
}
