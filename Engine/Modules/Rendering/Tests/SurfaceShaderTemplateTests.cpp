// The Create → Surface Shader teaching template must COMPILE through the real
// pipeline (compose + shaderc to SPIR-V), in both keyword variants, and its
// companion material must be pre-wired to it. A template that drifts from the
// engine contract is worse than no template — these tests pin the pair to the
// machinery it teaches.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Rendering/Materials/SurfaceShaderTemplate.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

struct TempTree
{
    fs::path Root;

    explicit TempTree(const char* prefix)
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() /
               (std::string(prefix) + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

// Stage the template pair into a temp "project" dir exactly as the editor
// writes it, then build through the production path.
MaterialBuildResult BuildTemplatePair(const TempTree& tree, const MaterialDocument& doc,
                                      const std::string& stem)
{
    const fs::path materialDir = tree.Root / "Materials";
    std::error_code ec;
    fs::create_directories(materialDir, ec);
    {
        std::ofstream out(materialDir / (stem + ".glsl"), std::ios::binary | std::ios::trunc);
        out << MakeSurfaceShaderTemplateSource();
    }

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.CacheRoot = tree.Root / "Cache";
    return BuildMaterialToShaderPackage(doc, materialDir / (stem + ".material"), stem, ctx, ShaderSourceKind::SpirV,
                                        MaterialKeyword::Instanced);
}

#define SKIP_WITHOUT_SHADERC(result)                                                        \
    do                                                                                      \
    {                                                                                       \
        for (const auto& e : (result).errors)                                               \
            if (e.find("shaderc is not available") != std::string::npos)                    \
                GTEST_SKIP() << "shaderc not built into this target";                       \
    } while (0)

void ExpectBuildSucceeded(const MaterialBuildResult& result)
{
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
}

} // namespace

TEST(SurfaceShaderTemplate, PairCompilesThroughRealPipeline)
{
    TempTree tree("ge_tmpl_pair");
    const MaterialDocument doc = MakeSurfaceShaderTemplateMaterial("NewSurface");
    const auto result = BuildTemplatePair(tree, doc, "NewSurface");
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(SurfaceShaderTemplate, CompilesWithoutTheKeywordVariant)
{
    // The #else arm of the GE_USER_PULSE block must be just as compilable —
    // a user's first move is often deleting the keyword from the material.
    TempTree tree("ge_tmpl_nokw");
    MaterialDocument doc = MakeSurfaceShaderTemplateMaterial("NewSurface");
    doc.keywords.clear();
    const auto result = BuildTemplatePair(tree, doc, "NewSurface");
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(SurfaceShaderTemplate, MaterialIsPreWiredToTheShader)
{
    const MaterialDocument doc = MakeSurfaceShaderTemplateMaterial("MyEffect");
    EXPECT_EQ(doc.surfaceShader, "MyEffect.glsl");
    EXPECT_EQ(doc.materialName, "MyEffect");
    ASSERT_EQ(doc.keywords.size(), 1u);
    EXPECT_EQ(doc.keywords[0], "PULSE");
    // Overrides only: every parameter starts at the default its @property line
    // declares, so the companion material authors none.
    EXPECT_TRUE(doc.properties.empty());
    // The sampled texture keys are seeded so the inspector shows assignable
    // rows for them (empty ref = unassigned).
    EXPECT_NE(doc.textures.find("albedoMap"), doc.textures.end());
    EXPECT_NE(doc.textures.find("accentMask"), doc.textures.end());
}

TEST(SurfaceShaderTemplate, DeclaresWellKnownPlusUserTextureSlot)
{
    const auto slots = ShaderComposer::ResolveTextureSlots(MakeSurfaceShaderTemplateSource());
    ASSERT_TRUE(slots.HasDeclarations);
    ASSERT_FALSE(slots.Rejected) << slots.RejectReason;
    ASSERT_EQ(slots.DeclaredSlots.size(), 2u);
    EXPECT_EQ(slots.DeclaredSlots[0].first, "albedoMap");
    EXPECT_EQ(slots.DeclaredSlots[0].second, 0);
    EXPECT_EQ(slots.DeclaredSlots[1].first, "accentMask");
    EXPECT_EQ(slots.DeclaredSlots[1].second, 1) << "user name packs into the lowest free slot";
}

TEST(SurfaceShaderTemplate, DeclaresNamedRangedGroupedProperties)
{
    const auto table = BuildShaderPropertyTable(
        {ShaderPropertySource{MakeSurfaceShaderTemplateSource(), "T.glsl", ShaderPropertyOrigin::Surface}});
    ASSERT_FALSE(table.Rejected()) << table.Errors.front().Message;
    ASSERT_TRUE(table.HasSurfaceDeclarations);
    const ShaderProperty* nits = table.Find("accentNits");
    ASSERT_NE(nits, nullptr);
    EXPECT_EQ(nits->DisplayName, "Accent (nits)");
    EXPECT_TRUE(nits->HasRange);
    EXPECT_FLOAT_EQ(nits->RangeMax, 2000.0f);
    EXPECT_EQ(nits->Group, "Accent");
    EXPECT_FALSE(nits->Tooltip.empty());
    EXPECT_FLOAT_EQ(nits->Default[0], 406.0f);
    const ShaderProperty* accent = table.Find("accentColor");
    ASSERT_NE(accent, nullptr);
    EXPECT_EQ(accent->Type, ShaderPropertyType::Color);
    EXPECT_EQ(accent->ComponentCount(), 3u);
    EXPECT_NE(table.Find("baseColor"), nullptr);
    EXPECT_NE(table.Find("metallic"), nullptr);
    EXPECT_NE(table.Find("roughness"), nullptr);
    EXPECT_NE(table.Find("pulseSpeed"), nullptr);
    // The adapter's cutoff read takes a lane only when the surface declares the
    // name; the template does, so Create -> Surface Shader + alphaMode=Mask has
    // a cutoff control, shown only in that mode.
    const ShaderProperty* cutoff = table.Find("alphaCutoff");
    ASSERT_NE(cutoff, nullptr);
    EXPECT_TRUE(cutoff->HasRange);
    EXPECT_FLOAT_EQ(cutoff->Default[0], 0.5f);
    EXPECT_EQ(cutoff->VisibleIf, "alphaMode=Mask");
}

// Create → Surface Shader drops straight into inline rename, so any name baked into the
// template is stale the moment the author types. The header must not quote one.
TEST(SurfaceShaderTemplate, TheHeaderCommentNamesNoFile)
{
    const std::string source = MakeSurfaceShaderTemplateSource();
    const std::string header = source.substr(0, source.find("\n"));

    EXPECT_NE(header.find("Surface shader"), std::string::npos);
    EXPECT_EQ(header.find(".glsl"), std::string::npos)
        << "header quotes a file name, which a rename would falsify: " << header;
    EXPECT_EQ(source.find("NewSurface"), std::string::npos)
        << "the creation-time stem leaked into the template body";
}
