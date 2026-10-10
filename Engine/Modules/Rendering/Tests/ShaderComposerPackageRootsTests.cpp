// Package shader roots (ARC 3a / seam 3): surface and vertex-modifier
// references resolve materialDir -> MaterialBuildContext::ProjectRoots ->
// PackageShaderDirs (mount priority order) -> AdapterShaderDir, so packages
// can ship shaders without materials knowing where the files physically
// live — and shaders of
// the same relative name from different origins never share a disk-cache key
// (the composed source embeds a relocatable include spelling — the authored
// relative when an include root serves that file, else a deepest-root relative
// — and the cache key hashes the source bytes).

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteTextFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

// Self-cleaning unique temp tree for fake material/package/engine roots.
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

std::string Norm(const fs::path& p)
{
    return p.lexically_normal().generic_string();
}
} // namespace

TEST(ShaderComposerPackageRoots, ResolutionOrder_MaterialDirThenPackagesThenEngine)
{
    TempTree tree("ge_pkg_shader_roots");
    const fs::path materialDir = tree.Root / "Materials";
    const fs::path pkgA = tree.Root / "PkgA" / "Shaders";
    const fs::path pkgB = tree.Root / "PkgB" / "Shaders";
    const fs::path engine = tree.Root / "Engine" / "Shaders";

    const char* rel = "Surfaces/dup_surface.glsl";
    WriteTextFile(materialDir / rel, "// material copy\n");
    WriteTextFile(pkgA / rel, "// pkgA copy\n");
    WriteTextFile(pkgB / rel, "// pkgB copy\n");
    WriteTextFile(engine / rel, "// engine copy\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = engine;
    ctx.PackageShaderDirs = {pkgA, pkgB};

    // materialDir wins when it has the file.
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(materialDir / rel));

    // Highest-priority package wins once the materialDir copy is gone.
    fs::remove(materialDir / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(pkgA / rel));

    // Package order is the context order (mount priority), not alphabetical luck.
    fs::remove(pkgA / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(pkgB / rel));

    // Engine tree is the final fallback.
    fs::remove(pkgB / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(engine / rel));

    // Nowhere found: the materialDir candidate names the concrete miss.
    fs::remove(engine / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(materialDir / rel));
}

// The spelling a derived artifact records for a resolved file: by the first
// root of the same probe chain that contains it, never a machine path.
TEST(ShaderComposerPackageRoots, ShaderRootRelative_SpellsByTheFirstContainingRoot)
{
    TempTree tree("ge_shader_root_relative");
    const fs::path materialDir = tree.Root / "Materials";
    const fs::path project = tree.Root / "Project";
    const fs::path pkgA = tree.Root / "PkgA" / "Shaders";
    const fs::path engine = tree.Root / "Engine" / "Shaders";

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = engine;
    ctx.ProjectRoots = {project};
    ctx.PackageShaderDirs = {pkgA};

    EXPECT_EQ(ShaderComposer::ShaderRootRelative(materialDir / "Surfaces" / "mine.glsl", materialDir, ctx),
              "Surfaces/mine.glsl");
    EXPECT_EQ(ShaderComposer::ShaderRootRelative(project / "water.glsl", materialDir, ctx), "water.glsl");
    EXPECT_EQ(ShaderComposer::ShaderRootRelative(pkgA / "Surfaces" / "bark.glsl", materialDir, ctx),
              "Surfaces/bark.glsl");
    EXPECT_EQ(ShaderComposer::ShaderRootRelative(engine / "Adapters" / "adapter_forward.glsl", materialDir, ctx),
              "Adapters/adapter_forward.glsl");
    EXPECT_EQ(ShaderComposer::ShaderRootRelative(engine / "Adapters" / "adapter_forward.glsl", fs::path{}, ctx),
              "Adapters/adapter_forward.glsl");
    EXPECT_EQ(ShaderComposer::ShaderRootRelative(tree.Root / "Elsewhere" / "stray.glsl", materialDir, ctx),
              "stray.glsl");
}

TEST(ShaderComposerPackageRoots, ResolveShaderReference_AbsolutePathPassesThrough)
{
    TempTree tree("ge_pkg_shader_abs");
    const fs::path absolute = tree.Root / "Anywhere" / "surface.glsl";
    WriteTextFile(absolute, "// absolute\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = tree.Root / "Engine";
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(absolute.string(), tree.Root, ctx)),
              Norm(absolute));
}

TEST(ShaderComposerPackageRoots, Compose_SurfaceAndModifierResolveFromPackageRoot)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    TempTree tree("ge_pkg_shader_compose");
    const fs::path materialDir = tree.Root / "Materials";
    const fs::path pkgShaders = tree.Root / "Pkg" / "Assets" / "Shaders";
    fs::create_directories(materialDir);

    // The EZTree extraction shape: engine-convention references, files shipped
    // by a package instead of the engine tree.
    WriteTextFile(pkgShaders / "Surfaces" / "pkg_test_surface.glsl", "// package surface\n");
    WriteTextFile(pkgShaders / "VertexModifiers" / "pkg_test_wind.glsl",
                  "vec3 ModifyVertex(vec3 pos, InstanceData inst) { return pos; }\n");

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/pkg_test_surface.glsl";
    doc.vertexModifier = "VertexModifiers/pkg_test_wind.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.materialKeywords = MaterialKeyword::Instanced | MaterialKeyword::HasVertexMod;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;
    ctx.PackageShaderDirs = {pkgShaders};

    std::vector<std::string> errors;
    auto result = ShaderComposer::Compose(doc, key, materialDir, ctx, &errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());

    // Both literals name the file relative to the package root that resolves
    // it — never absolutely, or the composed source (a cache key input) would
    // pin the compiled variant to this machine.
    EXPECT_NE(result.fragmentSource.find("#include \"Surfaces/pkg_test_surface.glsl\""),
              std::string::npos)
        << "surface include literal must be spelled relative to the package shader root";
    EXPECT_NE(result.vertexSource.find("#include \"VertexModifiers/pkg_test_wind.glsl\""),
              std::string::npos)
        << "vertex modifier include literal must be spelled relative to the package shader root";
    EXPECT_EQ(result.fragmentSource.find(Norm(pkgShaders.string())), std::string::npos)
        << "no absolute path may appear in the composed source";
}

TEST(ShaderComposerPackageRoots, Compose_SameReferenceFromTwoOrigins_ProducesRelocatableSource)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    TempTree tree("ge_pkg_shader_origin");
    const fs::path materialDir = tree.Root / "Materials";
    const fs::path originA = tree.Root / "PkgA" / "Shaders";
    const fs::path originB = tree.Root / "PkgB" / "Shaders";
    fs::create_directories(materialDir);

    const char* rel = "Surfaces/dup_origin_surface.glsl";
    WriteTextFile(originA / rel, "// origin A\n");
    WriteTextFile(originB / rel, "// origin B\n");

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = rel;

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctxA{};
    ctxA.AdapterShaderDir = adapterDir;
    ctxA.PackageShaderDirs = {originA};

    MaterialBuildContext ctxB = ctxA;
    ctxB.PackageShaderDirs = {originB};

    std::vector<std::string> errors;
    auto composedA = ShaderComposer::Compose(doc, key, materialDir, ctxA, &errors);
    auto composedB = ShaderComposer::Compose(doc, key, materialDir, ctxB, &errors);
    ASSERT_TRUE(composedA.IsValid());
    ASSERT_TRUE(composedB.IsValid());

    // Composition names the file, not its location: both origins produce the
    // same bytes. What keeps the two out of one cache entry is the #include
    // closure CONTENT the key folds (ShaderCompileCacheKey.SameIncludeSpelling
    // DifferentRootContentIsADifferentKey), which is the durable discriminator —
    // an absolute literal would only have distinguished them by accident of
    // where the tree happened to sit.
    EXPECT_EQ(composedA.fragmentSource, composedB.fragmentSource);
    EXPECT_NE(composedA.fragmentSource.find("#include \"" + std::string(rel) + "\""),
              std::string::npos);
    EXPECT_EQ(composedA.fragmentSource.find(Norm(originA.string())), std::string::npos)
        << "no absolute path may appear in the composed source";
}

TEST(ShaderComposerPackageRoots, CompileCache_DistinctSourceBytes_DistinctCacheKeys)
{
    // ShaderCompileService keys the on-disk cache over the stage SOURCE BYTES
    // (among stage name/path/defines). Two compiles identical in everything but
    // the embedded include literal — exactly what two origins of a same-named
    // package shader produce — must land in different .Cache/Shaders entries.
    TempTree tree("ge_pkg_shader_cachekey");

    auto makeRequest = [&](const std::string& includeLiteralLine) {
        ShaderProgramCompileRequest req{};
        req.debugName = "origin_key_probe";
        req.baseDirectory = tree.Root;
        req.cacheRoot = tree.Root / "Cache";
        ShaderStageCompileSpec vs{};
        vs.stage = "vs";
        vs.sourcePath = tree.Root / "probe.vert"; // logical name only (identical on purpose)
        vs.inlineSource = "#version 450\n// " + includeLiteralLine + "\nvoid main() { gl_Position = vec4(0.0); }\n";
        req.stages.push_back(std::move(vs));
        return req;
    };

    ShaderProgramCompileResult resultA{};
    std::string err;
    const bool okA = ShaderCompileService::CompileProgramToCache(
        makeRequest("include \"C:/PkgA/Shaders/Surfaces/dup.glsl\""), ShaderSourceKind::SpirV, resultA, &err);
    if (!okA && err.find("shaderc is not available") != std::string::npos)
        GTEST_SKIP() << "shaderc not built into this target";
    ASSERT_TRUE(okA) << err;

    ShaderProgramCompileResult resultB{};
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        makeRequest("include \"C:/PkgB/Shaders/Surfaces/dup.glsl\""), ShaderSourceKind::SpirV, resultB, &err))
        << err;

    EXPECT_NE(resultA.outputDir, resultB.outputDir)
        << "same-named shaders from different origins must never collide in .Cache/Shaders";

    // Sanity: byte-identical requests DO share an entry (the dedup half).
    ShaderProgramCompileResult resultA2{};
    ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
        makeRequest("include \"C:/PkgA/Shaders/Surfaces/dup.glsl\""), ShaderSourceKind::SpirV, resultA2, &err))
        << err;
    EXPECT_EQ(resultA.outputDir, resultA2.outputDir);
}

// ---------------------------------------------------------------------------
// Surface-shader project portability: MaterialBuildContext::ProjectRoots in
// the resolution chain, the empty-materialDir contract (an unavailable
// material asset path is never impersonated by a stand-in directory), and the
// authored-surface hard compose failure.
// ---------------------------------------------------------------------------

TEST(ShaderComposerProjectRoots, ResolutionOrder_ProjectRootBetweenMaterialDirAndPackages)
{
    TempTree tree("ge_project_shader_roots");
    const fs::path materialDir = tree.Root / "Project" / "Materials";
    const fs::path projectRoot = tree.Root / "Project";
    const fs::path pkg = tree.Root / "Pkg" / "Shaders";
    const fs::path engine = tree.Root / "Engine" / "Shaders";

    const char* rel = "dup_surface.glsl";
    WriteTextFile(materialDir / rel, "// material copy\n");
    WriteTextFile(projectRoot / rel, "// project copy\n");
    WriteTextFile(pkg / rel, "// pkg copy\n");
    WriteTextFile(engine / rel, "// engine copy\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = engine;
    ctx.ProjectRoots = {projectRoot};
    ctx.PackageShaderDirs = {pkg};

    // Material asset path present: the material's own directory wins.
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(materialDir / rel));

    // The live project root wins over packages once the materialDir copy is gone.
    fs::remove(materialDir / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(projectRoot / rel));

    fs::remove(projectRoot / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(pkg / rel));

    fs::remove(pkg / rel);
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, materialDir, ctx)),
              Norm(engine / rel));
}

TEST(ShaderComposerProjectRoots, EmptyMaterialDirResolvesAgainstProjectRoot)
{
    // The material's asset path is UNAVAILABLE (cache-driven compile): the
    // materialDir probe is skipped and a project-relative reference still
    // resolves against the LIVE project root — the copied-project scenario
    // where the old AdapterShaderDir stand-in silently shadowed it.
    TempTree tree("ge_project_root_no_matdir");
    const fs::path projectRoot = tree.Root / "ProjectCopy";
    const char* rel = "Materials/water_project_surface.glsl";
    WriteTextFile(projectRoot / rel, "// project surface\n");

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = tree.Root / "Engine" / "Shaders";
    ctx.ProjectRoots = {projectRoot};

    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(rel, fs::path{}, ctx)),
              Norm(projectRoot / rel));
}

TEST(ShaderComposerProjectRoots, EmptyMaterialDirMissNamesEngineCandidate)
{
    TempTree tree("ge_project_root_miss");
    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = tree.Root / "Engine" / "Shaders";

    // Nothing exists anywhere: with no material dir, the diagnostic candidate
    // is the engine-tree path (never a fabricated material dir).
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference("missing.glsl", fs::path{}, ctx)),
              Norm(ctx.AdapterShaderDir / "missing.glsl"));
}

TEST(ShaderComposerProjectRoots, Compose_EmptyMaterialDirResolvesAuthoredSurfaceFromProjectRoot)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    TempTree tree("ge_project_root_compose");
    const fs::path projectRoot = tree.Root / "ProjectCopy";
    const char* rel = "Materials/water_portability_surface.glsl";
    WriteTextFile(projectRoot / rel,
                  "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
                  "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                  "    o.baseColor = Mat.uBaseColor.rgb;\n"
                  "    return o;\n"
                  "}\n");

    MaterialDocument doc{};
    doc.materialName = "WaterPortability";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = rel;

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;
    ctx.ProjectRoots = {projectRoot};

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, fs::path{}, ctx, &errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());
    const std::string authoredInclude = std::string("\"") + rel + "\"";
    EXPECT_NE(result.fragmentSource.find(authoredInclude), std::string::npos)
        << "authored surface must stay a relocatable include so intern matches the cook; "
           "shaderc finds it because ProjectRoots are compile include roots. composed:\n"
        << result.fragmentSource.substr(0, 800);
    const auto includeRoots = BuildMaterialIncludeRoots(ctx, fs::path{});
    const bool projectRootIsIncludeRoot =
        std::any_of(includeRoots.begin(), includeRoots.end(),
                    [&](const fs::path& r) { return Norm(r) == Norm(projectRoot); });
    EXPECT_TRUE(projectRootIsIncludeRoot)
        << "BuildMaterialIncludeRoots must search ProjectRoots or the authored "
           "relative include cannot compile";
}

TEST(ShaderComposerProjectRoots, Compose_AuthoredSurfaceUnresolved_FailsHard)
{
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    TempTree tree("ge_authored_surface_miss");

    MaterialDocument doc{};
    doc.materialName = "WaterFlowMissing";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "no_such_surface_xyz.glsl";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;
    ctx.ProjectRoots = {tree.Root / "ProjectCopy"};

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, fs::path{}, ctx, &errors);

    // Hard failure: no sources, no silent default-surface fallback.
    EXPECT_FALSE(result.IsValid());
    EXPECT_TRUE(result.fragmentSource.empty());
    ASSERT_FALSE(errors.empty());
    // The error names the material, the unresolved reference, and the roots.
    EXPECT_NE(errors[0].find("WaterFlowMissing"), std::string::npos) << errors[0];
    EXPECT_NE(errors[0].find("no_such_surface_xyz.glsl"), std::string::npos) << errors[0];
    EXPECT_NE(errors[0].find("project root"), std::string::npos) << errors[0];
}

TEST(ShaderComposerProjectRoots, Compose_UnauthoredSurfaceStillUsesDefaultSurface)
{
    // The default-surface path survives ONLY for materials that author no
    // surface at all.
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();

    MaterialDocument doc{};
    doc.materialName = "NoSurfaceAuthored";
    doc.lightingModel = "StandardPBR";

    ShaderVariantKey key{};
    key.vertexFlags = VertexAttributeFlags::StandardMesh;
    key.lightingModel = LightingModel::kStandardPBR;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;

    std::vector<std::string> errors;
    const auto result = ShaderComposer::Compose(doc, key, fs::path{}, ctx, &errors);
    for (const auto& e : errors)
        ADD_FAILURE() << "Compose error: " << e;
    ASSERT_TRUE(result.IsValid());
    EXPECT_NE(result.fragmentSource.find("standard_surface.glsl"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Arc 3a acceptance C: the REAL extracted eztree shaders. The engine tree no
// longer contains ez_tree_leaves/ez_tree_wind — they ship in
// Packages/eztree/Assets/Shaders — so this compose+compile succeeds ONLY
// via the package root chain, through the full production build path
// (capability detection, compose, real shaderc to SPIR-V).
// ---------------------------------------------------------------------------

#ifdef EZTREE_PACKAGE_SHADERS_DIR
TEST(ShaderComposerPackageRoots, EZTreeRealShadersComposeAndCompileFromPackageRoot)
{
    const fs::path pkgShaders(EZTREE_PACKAGE_SHADERS_DIR);
    ASSERT_TRUE(fs::is_directory(pkgShaders)) << pkgShaders;
    const auto adapterDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    TempTree tree("ge_eztree_real_shaders");
    const fs::path materialDir = tree.Root / "Materials";
    fs::create_directories(materialDir);

    MaterialDocument doc{};
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/ez_tree_leaves.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = adapterDir;
    ctx.PackageShaderDirs = {pkgShaders};
    ctx.CacheRoot = tree.Root / "Cache";

    // Both engine-convention references resolve INTO the package root (the
    // engine tree has no copy to fall back to anymore).
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(doc.surfaceShader, materialDir, ctx)),
              Norm(pkgShaders / "Surfaces" / "ez_tree_leaves.glsl"));
    EXPECT_EQ(Norm(ShaderComposer::ResolveShaderReference(doc.vertexModifier, materialDir, ctx)),
              Norm(pkgShaders / "VertexModifiers" / "ez_tree_wind.glsl"));

    const auto result = BuildMaterialToShaderPackage(
        doc, materialDir / "eztree_leaves.material", "eztree_leaves", ctx, ShaderSourceKind::SpirV,
        MaterialKeyword::Instanced);
    for (const auto& e : result.errors)
    {
        if (e.find("shaderc is not available") != std::string::npos)
            GTEST_SKIP() << "shaderc not built into this target";
        ADD_FAILURE() << "Build error: " << e;
    }
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_NE(result.package->stageBytes.count("vs"), 0u);
    EXPECT_NE(result.package->stageBytes.count("fs"), 0u);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
    EXPECT_NE(result.composedFragmentSource.find("#include \"Surfaces/ez_tree_leaves.glsl\""),
              std::string::npos);
    EXPECT_NE(result.composedVertexSource.find("#include \"VertexModifiers/ez_tree_wind.glsl\""),
              std::string::npos);
    EXPECT_EQ(result.composedFragmentSource.find("/Packages/"), std::string::npos);
    EXPECT_EQ(result.composedVertexSource.find("/Packages/"), std::string::npos);
}
#endif // EZTREE_PACKAGE_SHADERS_DIR
