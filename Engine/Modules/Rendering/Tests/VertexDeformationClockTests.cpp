// The vertex-stage deformation clock is an endpoint argument: every InstanceData a vertex
// modifier receives carries the animation clock it deforms at, stamped by the fetch that
// built it. A modifier that reads the clock from the per-view light block instead cannot be
// evaluated at a second endpoint, which is what motion vectors for vertex-deformed meshes
// need.
//
// Two contracts, each with its own instrument:
//   - the rigid path is untouched: a variant without a vertex modifier composes no
//     GE_VERTEX_DEFORMATION define and its vertex SPIR-V carries neither clock field;
//   - the deforming path is complete: every InstanceData-populating site stamps both fields
//     from the endpoint, a deforming variant composes the define and compiles against the
//     fields, and the shipped modifier reads the clock from its argument and nowhere else.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef RENDERING_SOURCE_DIR
#error "RENDERING_SOURCE_DIR must be defined by CMake (rendering_test_shader_paths)"
#endif
#ifndef EZTREE_PACKAGE_SHADERS_DIR
#error "EZTREE_PACKAGE_SHADERS_DIR must be defined by CMake (target_compile_definitions)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

constexpr const char* kDeformationDefine = "GE_VERTEX_DEFORMATION";
constexpr const char* kTimeField = "deformationTimeSeconds";
constexpr const char* kScrollField = "deformationScrollSeconds";
constexpr const char* kStampCall = "ge_StampDeformationClock(inst);";
constexpr const char* kGlobalClock = "Light.uTimeParams";

// Source-text reads go to the build input (StagedTestPaths.h): the subject is the authored
// text a developer edits, not a staged copy of it.
std::string ReadSourceText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "could not open " << path.string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Drops // and /* */ comments so a contract over declarations and calls can be neither
// satisfied nor broken by prose.
std::string StripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();)
    {
        if (src.compare(i, 2, "//") == 0)
        {
            while (i < src.size() && src[i] != '\n')
                ++i;
        }
        else if (src.compare(i, 2, "/*") == 0)
        {
            const size_t end = src.find("*/", i + 2);
            i = end == std::string::npos ? src.size() : end + 2;
        }
        else
        {
            out.push_back(src[i++]);
        }
    }
    return out;
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

fs::path MakeTempCacheRoot()
{
    static std::atomic<uint32_t> counter{0};
    return fs::temp_directory_path() /
           ("ge_deformation_clock_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(counter.fetch_add(1)));
}

// The world draw ORs Instanced onto the pass keywords (RenderServicesWorldPass.cpp) and the
// shipped forward graphs declare ForwardPlus + Shadows + IBL for the world pass; the depth
// prepass composes the depth-only fragment.
constexpr MaterialKeyword kWorldPass = MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus |
                                       MaterialKeyword::Shadows | MaterialKeyword::IBL;
constexpr MaterialKeyword kDepthPass = MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment;

MaterialBuildResult Build(const MaterialDocument& doc, MaterialKeyword keywords,
                          std::vector<fs::path> packageShaderDirs = {})
{
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = Tests::GetAdapterShaderDir();
    ctx.PackageShaderDirs = std::move(packageShaderDirs);
    ctx.CacheRoot = MakeTempCacheRoot();

    auto result = BuildMaterialToShaderPackage(
        doc, TestPaths::StagedEngineAssetsDir() / "Materials" / "DeformationClockProbe.material",
        "DeformationClockProbe", ctx, ShaderSourceKind::SpirV, keywords);
    std::error_code ec;
    fs::remove_all(ctx.CacheRoot, ec);
    return result;
}

// A stock material on the engine's built-in surface: no vertex modifier, the rigid path.
MaterialDocument StockDocument(const std::string& lightingModel)
{
    MaterialDocument doc{};
    doc.materialName = "DeformationClockProbe";
    doc.lightingModel = lightingModel;
    return doc;
}

// Mirrors Packages/eztree/Assets/Materials/EZTree/EZTree_Bark.material: the shipped PBR
// surface with the package's wind modifier, resolved through the package shader root.
MaterialDocument WindTreeDocument()
{
    MaterialDocument doc{};
    doc.materialName = "DeformationClockWindTree";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    return doc;
}

#define SKIP_WITHOUT_SHADERC(result)                                     \
    do                                                                   \
    {                                                                    \
        for (const auto& e : (result).errors)                            \
            if (e.find("shaderc is not available") != std::string::npos) \
                GTEST_SKIP() << "shaderc not built into this target";    \
    } while (0)

bool HasDefine(const MaterialBuildResult& result, const char* define)
{
    return std::find(result.composedDefines.begin(), result.composedDefines.end(), define) !=
           result.composedDefines.end();
}

// shaderc runs at optimization level zero, so OpName / OpMemberName debug strings survive and
// a GLSL identifier that reached the compiled vertex stage is searchable in its SPIR-V.
bool VertexSpvNames(const MaterialBuildResult& result, const std::string& needle)
{
    if (result.package == nullptr)
        return false;
    const auto it = result.package->stageBytes.find("vs");
    if (it == result.package->stageBytes.end())
        return false;
    const auto& spv = it->second;
    return std::search(spv.begin(), spv.end(), needle.begin(), needle.end()) != spv.end();
}

} // namespace

// The rigid fast path is byte-identical because nothing about the clock reaches a variant
// without a vertex modifier: no define, no struct fields.
TEST(VertexDeformationClock, RigidVariantsComposeNoDeformationDefineAndNoClockFields)
{
    struct Case
    {
        const char* lightingModel;
        MaterialKeyword keywords;
        const char* label;
    };
    const Case cases[] = {
        {"StandardPBR", kWorldPass, "StandardPBR world pass"},
        {"Unlit", kWorldPass, "Unlit world pass"},
        {"StandardPBR", kDepthPass, "StandardPBR depth-only"},
    };
    for (const Case& c : cases)
    {
        const auto result = Build(StockDocument(c.lightingModel), c.keywords);
        SKIP_WITHOUT_SHADERC(result);
        for (const auto& e : result.errors)
            ADD_FAILURE() << c.label << ": " << e;
        ASSERT_TRUE(result.success) << c.label;
        ASSERT_NE(result.package, nullptr) << c.label;

        EXPECT_FALSE(HasDefine(result, kDeformationDefine))
            << c.label << " composed " << kDeformationDefine << " without a vertex modifier";
        EXPECT_FALSE(VertexSpvNames(result, kTimeField))
            << c.label << ": the vertex SPIR-V carries " << kTimeField << " on the rigid path";
        EXPECT_FALSE(VertexSpvNames(result, kScrollField))
            << c.label << ": the vertex SPIR-V carries " << kScrollField << " on the rigid path";
    }
}

// The positive control for the absence gate above, and the compiled half of the deforming
// contract: the shipped wind modifier composes the define, compiles against the stamped fields
// (it reads one of them), and both fields are in the module — so the scan above is detecting a
// real name, not a missing one.
//
// Once per fetch form a deforming draw can take, because the fields and the block they are
// stamped from are guarded and a guard can narrow: the instanced buffer-reference fetch (the
// world pass), the same fetch under the depth-only fragment (prepass and shadow families), and
// the non-instanced push-constant fetch, which is what the editor's shader-graph preview draws
// through. A guard that stopped covering one of them fails to compile here rather than at
// runtime in whichever pass reaches it first. Whether each site STAMPS is the neighbouring
// source contract; this row is the compiled half.
TEST(VertexDeformationClock, ShippedWindModifierComposesTheDefineAndCompilesAgainstTheClockFields)
{
    const fs::path packageShaders(EZTREE_PACKAGE_SHADERS_DIR);
    ASSERT_TRUE(fs::exists(packageShaders / "VertexModifiers" / "ez_tree_wind.glsl"))
        << "eztree package shaders not found under " << packageShaders.string();

    struct Case
    {
        MaterialKeyword keywords;
        const char* label;
    };
    const Case cases[] = {
        {MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus, "instanced world pass"},
        {kDepthPass, "instanced depth-only"},
        {MaterialKeyword::ForwardPlus, "non-instanced push constant"},
    };
    for (const Case& c : cases)
    {
        const auto result = Build(WindTreeDocument(), c.keywords, {packageShaders});
        SKIP_WITHOUT_SHADERC(result);
        for (const auto& e : result.errors)
            ADD_FAILURE() << c.label << ": " << e;
        ASSERT_TRUE(result.success) << c.label;
        ASSERT_NE(result.package, nullptr) << c.label;

        ASSERT_TRUE(HasDefine(result, "HAS_VERTEX_MODIFIER"))
            << c.label
            << ": the wind modifier no longer derives the simple modifier form - this test's "
               "premise has changed";
        EXPECT_TRUE(HasDefine(result, kDeformationDefine))
            << c.label << ": a variant with a vertex modifier must compose " << kDeformationDefine;
        EXPECT_TRUE(VertexSpvNames(result, kTimeField))
            << c.label << ": the deforming vertex SPIR-V does not carry " << kTimeField;
        EXPECT_TRUE(VertexSpvNames(result, kScrollField))
            << c.label << ": the deforming vertex SPIR-V does not carry " << kScrollField;
    }
}

// The clock has one source. Every site that builds an InstanceData for the vertex stage stamps
// both fields through the one helper, and that helper reads the endpoint's clock lanes. A site
// that forgets, or a stamp of zero, is a modifier deforming at time 0 on that draw path.
TEST(VertexDeformationClock, EveryInstanceDataSiteStampsBothClockLanesFromTheEndpoint)
{
    const fs::path shaders = fs::path(RENDERING_SOURCE_DIR) / "Shaders";
    const std::string instanceIo =
        StripComments(ReadSourceText(shaders / "Includes" / "instance_io.glsl"));
    const std::string adapter =
        StripComments(ReadSourceText(shaders / "Adapters" / "adapter_vertex.glsl"));
    ASSERT_GT(instanceIo.size(), 200u) << "instance_io.glsl read back empty or stripped to nothing";
    ASSERT_GT(adapter.size(), 200u) << "adapter_vertex.glsl read back empty or stripped to nothing";

    // A populating site declares `InstanceData inst;` and fills it field by field; the adapter's
    // `InstanceData inst = ge_FetchInstanceData();` is a consumer of a site, not a site.
    const std::string site = "InstanceData inst;";
    const size_t siteCount = CountOccurrences(instanceIo, site) + CountOccurrences(adapter, site);
    ASSERT_GE(siteCount, 5u)
        << "fewer InstanceData-populating sites than the shipped vertex path carries (three fetch "
           "forms in instance_io.glsl, the push-constant fetch and the procedural block in "
           "adapter_vertex.glsl) - this contract's premise has changed";

    EXPECT_EQ(CountOccurrences(instanceIo, kStampCall) + CountOccurrences(adapter, kStampCall), siteCount)
        << "an InstanceData-populating site does not stamp the deformation clock";

    // zw, not the xy globals: the endpoint's animation lane is the view's REBASED
    // deformation time, and stamping it from the absolute global would compile and
    // render identically while silently losing endpoint precision at long uptime.
    EXPECT_EQ(CountOccurrences(instanceIo, "inst.deformationTimeSeconds = Light.uTimeParams.z;"), 1u)
        << "the animation lane is not stamped from the endpoint's rebased clock exactly once";
    EXPECT_EQ(CountOccurrences(instanceIo, "inst.deformationScrollSeconds = Light.uTimeParams.w;"), 1u)
        << "the scroll lane is not stamped from the endpoint's clock exactly once";
    EXPECT_EQ(CountOccurrences(instanceIo, "Light.uTimeParams.x"), 0u)
        << "an endpoint lane is stamped from the process-wide global, which two endpoints of one "
           "frame cannot difference exactly at long uptime";
}

// The shipped modifier reads the clock from its argument and from nowhere else. The grass
// modifier's own suite holds the same contract for its file (GrassWindClock in
// TerrainGrassStatsTests); this row covers the package modifier the engine tree ships.
TEST(VertexDeformationClock, ShippedWindModifierReadsTheClockFromItsEndpointOnly)
{
    const std::string wind = StripComments(
        ReadSourceText(fs::path(EZTREE_PACKAGE_SHADERS_DIR) / "VertexModifiers" / "ez_tree_wind.glsl"));
    ASSERT_GT(wind.size(), 200u) << "ez_tree_wind.glsl read back empty or stripped to nothing";
    ASSERT_NE(wind.find("ModifyVertex("), std::string::npos)
        << "ez_tree_wind.glsl no longer declares a vertex modifier - this contract's premise has changed";

    EXPECT_NE(wind.find(std::string("inst.") + kTimeField), std::string::npos)
        << "the wind modifier does not read the endpoint deformation clock";
    EXPECT_EQ(wind.find(kGlobalClock), std::string::npos)
        << "the wind modifier still reads the per-view global clock, which cannot be evaluated at a "
           "second endpoint";
}
