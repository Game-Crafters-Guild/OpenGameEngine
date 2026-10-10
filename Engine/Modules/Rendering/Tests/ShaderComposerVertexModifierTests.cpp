// A materialized shader graph is one file for both stages: EvaluateSurface for
// the fragment adapter and, when Vertex Pos is wired, ModifyVertex for the
// vertex adapter. Compose must bind that file as the vertex modifier too and
// hand each adapter a stage define so the file can hide the other stage's
// entry point.

#include <gtest/gtest.h>

#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "TestUtils.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

fs::path UniqueTempDir(const char* prefix)
{
    static std::atomic<uint32_t> counter{0};
    fs::path root = fs::temp_directory_path() /
                    (std::string(prefix) + "_" +
                     std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                     std::to_string(counter.fetch_add(1)));
    fs::create_directories(root);
    return root;
}

ComposedShaderSource ComposeSurface(const fs::path& materialDir, const char* surfaceRelative,
                                   std::vector<std::string>& errors,
                                   MaterialKeyword keywords = MaterialKeyword::None)
{
    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = surfaceRelative;

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = keywords;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    return ShaderComposer::Compose(doc, key, materialDir, ctx, &errors);
}

constexpr const char* kGraphSurfaceWithVertexModifier =
    "// Auto-generated. The @sg-* tag block is authoritative.\n"
    "//\n"
    "// @sg-graph     VertexOffset\n"
    "// @sg-version   1\n"
    "// @sg-stage     both\n"
    "// @sg-lighting  StandardPBR\n"
    "// @sg-variant   HAS_VERTEX_MODIFIER\n"
    "//\n"
    "#ifndef GE_STAGE_VERTEX\n"
    "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
    "{\n"
    "    return DefaultSurfaceOutput();\n"
    "}\n"
    "#endif // !GE_STAGE_VERTEX\n"
    "\n"
    "#ifdef GE_STAGE_VERTEX\n"
    "vec3 ModifyVertex(vec3 position, InstanceData inst)\n"
    "{\n"
    "    return position + vec3(0.0, 0.25, 0.0);\n"
    "}\n"
    "#endif // GE_STAGE_VERTEX\n";
} // namespace

TEST(ComposeVertexModifier, EveryStageGetsItsOwnDefine)
{
    const fs::path materialDir = UniqueTempDir("ge_compose_stage_defines");
    WriteFile(materialDir / "Surfaces" / "plain.glsl",
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n");

    std::vector<std::string> errors;
    const auto result = ComposeSurface(materialDir, "Surfaces/plain.glsl", errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());

    EXPECT_NE(result.vertexSource.find("#define GE_STAGE_VERTEX\n"), std::string::npos);
    EXPECT_EQ(result.vertexSource.find("#define GE_STAGE_FRAGMENT\n"), std::string::npos);
    EXPECT_NE(result.fragmentSource.find("#define GE_STAGE_FRAGMENT\n"), std::string::npos);
    EXPECT_EQ(result.fragmentSource.find("#define GE_STAGE_VERTEX\n"), std::string::npos);

    // A plain surface has no vertex modifier: the marker stays behind its
    // undefined HAS_VERTEX_MODIFIER guard and the surface is not pulled into
    // the vertex stage.
    const std::string surfaceInclude =
        "#include \"" + (materialDir / "Surfaces" / "plain.glsl").generic_string() + "\"";
    EXPECT_EQ(result.vertexSource.find(surfaceInclude), std::string::npos);
    EXPECT_EQ(result.vertexSource.find("#define HAS_VERTEX_MODIFIER"), std::string::npos);

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}

TEST(ComposeVertexModifier, GraphSurfaceWithVertexModifierIsBoundToTheVertexStage)
{
    const fs::path materialDir = UniqueTempDir("ge_compose_graph_vertex_modifier");
    WriteFile(materialDir / "Generated" / "offset_built.glsl", kGraphSurfaceWithVertexModifier);

    std::vector<std::string> errors;
    const auto result = ComposeSurface(materialDir, "Generated/offset_built.glsl", errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());

    // The composer spells the include as authored (root-relative): the
    // composed text is a cache key and must not carry this machine's path.
    const std::string surfaceInclude = "#include \"Generated/offset_built.glsl\"";
    EXPECT_NE(result.vertexSource.find("#define HAS_VERTEX_MODIFIER"), std::string::npos);
    EXPECT_NE(result.vertexSource.find(surfaceInclude), std::string::npos)
        << "the graph surface must be its own vertex modifier";
    EXPECT_EQ(result.vertexSource.find("#include GE_VERTEX_MODIFIER_PATH"), std::string::npos)
        << "the raw marker must never reach the compiler";
    EXPECT_NE(result.fragmentSource.find(surfaceInclude), std::string::npos);

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}

// GE_VERTEX_DEFORMATION is derived from the final define list, after the graph's variant
// defines are merged: a materialized graph reaches HAS_VERTEX_MODIFIER through its
// @sg-variant tag and never through the key, so a key-only derivation would miss it. A
// surface with no modifier of either form composes no such define, which is what keeps the
// rigid path's InstanceData and SPIR-V unchanged.
TEST(ComposeVertexModifier, DeformationDefineFollowsEitherModifierForm)
{
    const fs::path materialDir = UniqueTempDir("ge_compose_deformation_define");
    WriteFile(materialDir / "Surfaces" / "plain.glsl",
              "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { return DefaultSurfaceOutput(); }\n");
    WriteFile(materialDir / "Generated" / "offset_built.glsl", kGraphSurfaceWithVertexModifier);
    const std::string define = "#define GE_VERTEX_DEFORMATION\n";

    std::vector<std::string> errors;
    const auto rigid = ComposeSurface(materialDir, "Surfaces/plain.glsl", errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(rigid.IsValid());
    EXPECT_EQ(rigid.vertexSource.find(define), std::string::npos)
        << "a surface without a vertex modifier must not compose the deformation define";

    const auto graph = ComposeSurface(materialDir, "Generated/offset_built.glsl", errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(graph.IsValid());
    EXPECT_NE(graph.vertexSource.find(define), std::string::npos)
        << "a graph-emitted vertex modifier must compose the deformation define";

    const auto extended = ComposeSurface(materialDir, "Surfaces/plain.glsl", errors,
                                         MaterialKeyword::HasVertexOutputMod);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(extended.IsValid());
    EXPECT_NE(extended.vertexSource.find(define), std::string::npos)
        << "the extended modifier form must compose the deformation define";

    std::error_code ec;
    fs::remove_all(materialDir, ec);
}
