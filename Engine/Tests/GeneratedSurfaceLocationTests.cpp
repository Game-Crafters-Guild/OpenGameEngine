// A graph-backed material materialises its surface on every compile. Written
// beside the .material it sat in scanned, watched project content, so the
// compiler's own write looked like an author edit: the watcher queued the
// compile that rewrote the file, and the cycle never settled (833 rewrites in
// 124 idle seconds on the 2026-07-27 walk). The generated surface is derived
// data and belongs under the shader cache root, which is excluded from asset
// scanning and from the watcher — there the cycle cannot form.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

// A minimal shader-graph surface: the @sg-* tag block is what marks the file as
// a graph source, and a SurfaceOutput node gives the compiler something to emit.
constexpr const char* kGraphSource = R"(// Auto-generated. The @sg-* tag block is authoritative.
//
// @sg-graph     TestGraph
// @sg-version   1
// @sg-stage     surface
// @sg-lighting  StandardPBR
//
// @sg-node      node_color   type=ColorConstant  pos=(120,140)  r=0.5 g=0.25 b=0.125
// @sg-node      node_output  type=SurfaceOutput  pos=(600,140)
//
// @sg-edge      node_color.value -> node_output.BaseColor

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    return o;
}
)";

class GeneratedSurfaceLocationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::error_code ec;
        m_Root = std::filesystem::temp_directory_path(ec) /
                 ("ge_gen_surface_" + std::to_string(::testing::UnitTest::GetInstance()
                                                         ->random_seed()) +
                  "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        m_MaterialDir = m_Root / "assets" / "materials";
        m_CacheRoot = m_Root / ".Cache" / "Shaders";
        std::filesystem::create_directories(m_MaterialDir, ec);
        std::filesystem::create_directories(m_CacheRoot, ec);

        m_GraphPath = m_MaterialDir / "test_graph.glsl";
        std::ofstream(m_GraphPath) << kGraphSource;
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    MaterialDocument MakeGraphMaterial() const
    {
        MaterialDocument doc;
        doc.surfaceShader = "test_graph.glsl"; // sibling of the .material
        return doc;
    }

    Rendering::MaterialBuildContext MakeContext() const
    {
        Rendering::MaterialBuildContext ctx;
        ctx.CacheRoot = m_CacheRoot;
        ctx.ProjectRoots = {m_Root / "assets"};
        ctx.AssetSourceRoots = {{"project", m_Root / "assets"}};
        return ctx;
    }

    std::filesystem::path MaterialPath() const { return m_MaterialDir / "test.material"; }

    std::filesystem::path m_Root;
    std::filesystem::path m_MaterialDir;
    std::filesystem::path m_CacheRoot;
    std::filesystem::path m_GraphPath;
};

// Count the generated surfaces the material directory holds — the loop's
// footprint was exactly one such file, rewritten forever.
size_t CountBuiltGlsl(const std::filesystem::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (entry.path().filename().string().find("_built.glsl") != std::string::npos)
            ++n;
    }
    return n;
}

} // namespace

TEST_F(GeneratedSurfaceLocationTest, GeneratedSurfaceLandsUnderTheCacheRootNotBesideTheMaterial)
{
    MaterialDocument doc = MakeGraphMaterial();
    Rendering::MaterialBuildContext ctx = MakeContext();
    std::vector<std::string> errors;

    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, MaterialPath(), ctx, errors))
        << (errors.empty() ? std::string("(no errors reported)") : errors.front());

    const std::filesystem::path generated(doc.surfaceShader);
    EXPECT_TRUE(generated.is_absolute()) << "an absolute reference resolves without a material-dir probe";
    EXPECT_TRUE(std::filesystem::exists(generated)) << generated.generic_string();

    const std::string generatedStr = generated.generic_string();
    EXPECT_NE(generatedStr.find(m_CacheRoot.generic_string()), std::string::npos)
        << "generated surface must live under the shader cache root: " << generatedStr;
    EXPECT_EQ(CountBuiltGlsl(m_MaterialDir), 0u)
        << "nothing may be written into the watched material directory";
}

// Mutation guard for the whole point of the change: a second compile must not
// put anything back beside the material, no matter how many times it runs.
TEST_F(GeneratedSurfaceLocationTest, RepeatedCompilesNeverWriteIntoTheMaterialDirectory)
{
    Rendering::MaterialBuildContext ctx = MakeContext();
    std::filesystem::path first;
    for (int i = 0; i < 3; ++i)
    {
        MaterialDocument doc = MakeGraphMaterial();
        std::vector<std::string> errors;
        ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, MaterialPath(), ctx, errors))
            << (errors.empty() ? std::string("(no errors reported)") : errors.front());
        if (i == 0)
            first = doc.surfaceShader;
        else
            EXPECT_EQ(std::filesystem::path(doc.surfaceShader), first)
                << "the generated path must be stable across compiles";
    }
    EXPECT_EQ(CountBuiltGlsl(m_MaterialDir), 0u);
}

// Two materials that share a stem in different folders kept apart by their
// directories before; one flat cache directory has nothing else to separate them.
TEST_F(GeneratedSurfaceLocationTest, SameStemInDifferentFoldersGetsDistinctGeneratedSurfaces)
{
    std::error_code ec;
    const std::filesystem::path otherDir = m_Root / "assets" / "other";
    std::filesystem::create_directories(otherDir, ec);
    std::ofstream(otherDir / "test_graph.glsl") << kGraphSource;

    Rendering::MaterialBuildContext ctx = MakeContext();

    MaterialDocument a = MakeGraphMaterial();
    std::vector<std::string> errorsA;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(a, MaterialPath(), ctx, errorsA));

    MaterialDocument b = MakeGraphMaterial();
    std::vector<std::string> errorsB;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(b, otherDir / "test.material", ctx, errorsB));

    EXPECT_NE(a.surfaceShader, b.surfaceShader)
        << "two same-named materials must not collide on one generated surface";
}

// A project material and a package material can share a relative path. Named by
// that path alone they shared one generated surface, and whichever was prepared
// last compiled into the other's variants. The name carries the source's alias,
// not its location, so a package keeps its surfaces wherever it is mounted from.
TEST_F(GeneratedSurfaceLocationTest, SameRelativePathInTwoSourcesGetsDistinctGeneratedSurfaces)
{
    const std::filesystem::path packageRoot = m_Root / "packages" / "sample-package" / "Assets";
    const std::filesystem::path movedPackageRoot = m_Root / "moved" / "Assets";
    for (const std::filesystem::path& root : {packageRoot, movedPackageRoot})
    {
        std::error_code ec;
        std::filesystem::create_directories(root / "materials", ec);
        std::ofstream(root / "materials" / "test_graph.glsl") << kGraphSource;
    }

    Rendering::MaterialBuildContext ctx = MakeContext();
    ctx.AssetSourceRoots.push_back({"sample-package", packageRoot});
    Rendering::MaterialBuildContext movedCtx = MakeContext();
    movedCtx.AssetSourceRoots.push_back({"sample-package", movedPackageRoot});
    std::vector<std::string> errors;

    MaterialDocument project = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(project, MaterialPath(), ctx, errors));
    MaterialDocument package = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(
        package, packageRoot / "materials" / "test.material", ctx, errors));
    MaterialDocument movedPackage = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(
        movedPackage, movedPackageRoot / "materials" / "test.material", movedCtx, errors));

    EXPECT_NE(project.surfaceShader, package.surfaceShader)
        << "a project material and a package material with one relative path share a surface";
    EXPECT_EQ(package.surfaceShader, movedPackage.surfaceShader)
        << "mounting a package from another directory must not rename its generated surfaces";
}

// The editor mount is the Assets folder beside the executable, or on macOS the
// user's EditorAssets copy. Its graph materials keep one surface name whichever
// of those roots, and whichever configuration's executable directory, holds them.
TEST_F(GeneratedSurfaceLocationTest, EditorMountMaterialsKeepOneNameAcrossMountRoots)
{
    const std::filesystem::path editorRoots[] = {m_Root / "bin" / "DebugFast" / "Assets",
                                                 m_Root / "bin" / "Release" / "Assets",
                                                 m_Root / "user" / "EditorAssets"};
    std::filesystem::path first;
    for (const std::filesystem::path& root : editorRoots)
    {
        const std::filesystem::path materialDir = root / "Materials" / "Graph";
        std::error_code ec;
        std::filesystem::create_directories(materialDir, ec);
        std::ofstream(materialDir / "test_graph.glsl") << kGraphSource;

        Rendering::MaterialBuildContext ctx = MakeContext();
        ctx.AssetSourceRoots.push_back({"editor", root});
        MaterialDocument doc = MakeGraphMaterial();
        std::vector<std::string> errors;
        ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, materialDir / "test.material", ctx,
                                                            errors))
            << root.generic_string() << ": "
            << (errors.empty() ? std::string("(no errors reported)") : errors.front());
        if (first.empty())
            first = doc.surfaceShader;
        else
            EXPECT_EQ(std::filesystem::path(doc.surfaceShader), first) << root.generic_string();
    }
}

// A package can be mounted from a folder inside another source's root. Its
// materials are named by the package, the deepest root that holds them, not by
// the enclosing source listed before it.
TEST_F(GeneratedSurfaceLocationTest, NestedRootNamesTheSurfaceByTheDeepestRoot)
{
    const std::filesystem::path nested = m_Root / "assets" / "vendor" / "Assets";
    const std::filesystem::path material = nested / "materials" / "test.material";
    std::error_code ec;
    std::filesystem::create_directories(material.parent_path(), ec);
    std::ofstream(material.parent_path() / "test_graph.glsl") << kGraphSource;

    Rendering::MaterialBuildContext ctx = MakeContext();
    ctx.AssetSourceRoots.push_back({"vendor-package", nested});
    Rendering::MaterialBuildContext nestedOnly = MakeContext();
    nestedOnly.AssetSourceRoots = {{"vendor-package", nested}};
    std::vector<std::string> errors;

    MaterialDocument mounted = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(mounted, material, ctx, errors));
    MaterialDocument expected = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(expected, material, nestedOnly, errors));
    EXPECT_EQ(mounted.surfaceShader, expected.surfaceShader)
        << "a material in a nested root must be named by that root's alias";
}

// Two sources can share one root: a Player development run mounts the project
// and the editor at one folder. The name then comes from the source with the
// higher mount priority, whatever order the two were mounted in.
TEST_F(GeneratedSurfaceLocationTest, SourcesSharingARootNameTheSurfaceByTheHigherPriorityAlias)
{
    const std::filesystem::path shared = m_Root / "shared" / "Assets";
    const std::filesystem::path material = shared / "materials" / "test.material";
    std::error_code ec;
    std::filesystem::create_directories(material.parent_path(), ec);
    std::ofstream(material.parent_path() / "test_graph.glsl") << kGraphSource;

    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(m_Root / "assets", nullptr, m_Root / "registry.assetdb",
                                  m_Root / "cache"));
    AssetSourceDesc lower{};
    lower.Alias = "lower-priority";
    lower.Root = shared;
    lower.Priority = 10;
    lower.RequiresScan = false;
    ASSERT_TRUE(assets.RegisterSource(lower));
    AssetSourceDesc higher = lower;
    higher.Alias = "higher-priority";
    higher.Priority = 20;
    ASSERT_TRUE(assets.RegisterSource(higher));
    Rendering::MaterialBuildContext ctx = MakeContext();
    ctx.AssetSourceRoots = CollectAssetSourceRoots(assets);
    assets.Shutdown();

    Rendering::MaterialBuildContext higherOnly = MakeContext();
    higherOnly.AssetSourceRoots = {{"higher-priority", shared}};
    std::vector<std::string> errors;
    MaterialDocument mounted = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(mounted, material, ctx, errors));
    MaterialDocument expected = MakeGraphMaterial();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(expected, material, higherOnly, errors));
    EXPECT_EQ(mounted.surfaceShader, expected.surfaceShader)
        << "sources sharing a root must name it by the higher-priority alias";
}

// With no cache root there is nowhere derived data can safely go, and falling
// back beside the material would re-arm the loop. Fail loudly instead.
TEST_F(GeneratedSurfaceLocationTest, MissingCacheRootIsAnErrorNotAFallbackBesideTheMaterial)
{
    MaterialDocument doc = MakeGraphMaterial();
    Rendering::MaterialBuildContext ctx; // CacheRoot deliberately empty
    std::vector<std::string> errors;

    EXPECT_FALSE(PrepareMaterialDocumentForShaderPackage(doc, MaterialPath(), ctx, errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(CountBuiltGlsl(m_MaterialDir), 0u);
}

// A generated surface is a build artifact. Carrying the authoring graph in its
// tag block made a node drag or a canvas pan rewrite it, which misses the shader
// cache and recompiles the material for an edit that cannot reach a shader.
TEST_F(GeneratedSurfaceLocationTest, GeneratedSurfaceKeepsSemanticTagsAndDropsAuthoringLayout)
{
    MaterialDocument doc = MakeGraphMaterial();
    Rendering::MaterialBuildContext ctx = MakeContext();
    std::vector<std::string> errors;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(doc, MaterialPath(), ctx, errors))
        << (errors.empty() ? std::string("(no errors reported)") : errors.front());

    std::ifstream in(doc.surfaceShader);
    ASSERT_TRUE(in.is_open());
    const std::string generated((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

    // @sg-graph is what marks the file as a graph source to IsShaderGraphSource,
    // and @sg-lighting still selects the lighting model.
    EXPECT_NE(generated.find("@sg-graph"), std::string::npos);
    EXPECT_NE(generated.find("@sg-lighting"), std::string::npos);
    EXPECT_NE(generated.find("SurfaceOutput EvaluateSurface"), std::string::npos);

    EXPECT_EQ(generated.find("@sg-node"), std::string::npos) << "node layout is authoring state";
    EXPECT_EQ(generated.find("@sg-edge"), std::string::npos) << "wiring is authoring state";
    EXPECT_EQ(generated.find("@sg-viewport"), std::string::npos) << "the camera is authoring state";
}

// The observable that costs real time: an authoring-only edit must leave the
// generated surface untouched, so the watcher stays asleep and the material's
// cached shader stays valid.
TEST_F(GeneratedSurfaceLocationTest, MovingANodeDoesNotRewriteTheGeneratedSurface)
{
    Rendering::MaterialBuildContext ctx = MakeContext();

    MaterialDocument first = MakeGraphMaterial();
    std::vector<std::string> errors;
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(first, MaterialPath(), ctx, errors));
    const std::filesystem::path generated(first.surfaceShader);
    const auto firstWrite = std::filesystem::last_write_time(generated);

    // Sleep past filesystem timestamp granularity so a rewrite is guaranteed to
    // move the mtime.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::string moved(kGraphSource);
    const std::string fromPos = "pos=(120,140)";
    const size_t at = moved.find(fromPos);
    ASSERT_NE(at, std::string::npos);
    moved.replace(at, fromPos.size(), "pos=(880,-60)");
    std::ofstream(m_GraphPath) << moved;

    MaterialDocument second = MakeGraphMaterial();
    errors.clear();
    ASSERT_TRUE(PrepareMaterialDocumentForShaderPackage(second, MaterialPath(), ctx, errors));
    ASSERT_EQ(second.surfaceShader, first.surfaceShader);

    EXPECT_EQ(std::filesystem::last_write_time(generated), firstWrite)
        << "a node move rewrote the generated surface — the shader cache entry dies with it";
}
