// The cooked-material contract: a project's material variants are compiled
// ahead of time into a shader cache, that cache travels with the content, and a
// runtime with NO shader compiler resolves every cooked variant from it.
//
// Both halves are proven against the real material path (ShaderComposer ->
// ShaderCompileService), not a stand-in:
//  - RELOCATION: cook under directory A, move the whole tree to B, resolve from
//    B. Anything machine-specific in the composed source or the cache key shows
//    up here as a miss.
//  - COOK-ONLY: with ShaderCompileService::SetCompilerAvailable(false) the
//    process behaves exactly as a shaderc-less build (wasm). A cooked variant
//    must still yield SPIR-V; an uncooked one must fail with a message naming
//    the entry that was missing.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AssetCore/GUID.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "GPUFogParticles/GPUFogParticlesMaterial.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "TerrainGrass/GrassDrawMode.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "ScopedCompatShaderProfile.h"
#include "StagedTestPaths.h"
#include "WebWgslCook.h"

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
        Root = fs::temp_directory_path()
             / (std::string(prefix) + "_"
                + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_"
                + std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

void WriteTextFile(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Minimal conforming surface: the adapter calls exactly this signature.
constexpr const char* kProbeSurface =
    "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
    "{\n"
    "    SurfaceOutput o = DefaultSurfaceOutput();\n"
    "    o.baseColor = vec3(0.25, 0.5, 0.75);\n"
    "    o.normalWS = normalize(sIn.normalWS);\n"
    "    return o;\n"
    "}\n";

// Restores the compiler on every exit, including a failed assertion.
struct ScopedCookOnlyRuntime
{
    ScopedCookOnlyRuntime() { ShaderCompileService::SetCompilerAvailable(false); }
    ~ScopedCookOnlyRuntime() { ShaderCompileService::SetCompilerAvailable(true); }
};

using TestSupport::ScopedCompatShaderProfile;

// The SPIR-V target feeds the program cache KEY, and a device publishes its own at init —
// so in a suite where anything created a device first, every later compile keys at that
// backend's target. A cache cooked for the web was keyed at the default 1.5, so a test
// asserting the runtime addresses that cache has to pin the target as deliberately as it
// pins the compat profile, or it passes alone and fails after any device-creating test.
struct ScopedWebCacheSpirvTarget
{
    using Target = Rendering::ShaderCompileService::SpirvTarget;
    ScopedWebCacheSpirvTarget() : Prev(Rendering::ShaderCompileService::MaxSupportedSpirv())
    {
        Rendering::ShaderCompileService::SetMaxSupportedSpirv(Target::Spirv_1_5);
    }
    ~ScopedWebCacheSpirvTarget() { Rendering::ShaderCompileService::SetMaxSupportedSpirv(Prev); }
    Target Prev;
};

// A project laid out the way the export step ships one: material + surface
// shader under Shaders/, and the cooked cache beside them.
struct CookedProject
{
    fs::path Root;
    fs::path MaterialPath;
    MaterialBuildContext Context;
};

CookedProject MakeProject(const fs::path& root, const fs::path& engineShaderDir)
{
    CookedProject p{};
    p.Root = root;
    p.MaterialPath = root / "Materials" / "probe.material";
    fs::create_directories(p.MaterialPath.parent_path());
    WriteTextFile(root / "Shaders" / "Surfaces" / "probe_surface.glsl", kProbeSurface);

    p.Context.AdapterShaderDir = engineShaderDir;
    p.Context.PackageShaderDirs = {root / "Shaders"};
    p.Context.ProjectRoots = {root};
    p.Context.CacheRoot = root / ".Cache" / "Shaders";
    return p;
}

MaterialDocument MakeDocument()
{
    MaterialDocument doc{};
    doc.materialName = "CookProbe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/probe_surface.glsl";
    return doc;
}

// The pass-keyword sets a forward-rendered opaque material is asked for: the
// colour pass and the depth/shadow pass. Anything the cook does not enumerate
// is a miss at runtime, loudly, by name.
constexpr MaterialKeyword kColorVariant =
    MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
constexpr MaterialKeyword kDepthVariant = MaterialKeyword::Instanced;

} // namespace

class MaterialVariantCook : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_EngineShaderDir =
            GameEngine::TestPaths::StagedRoot() / "Engine" / "Modules" / "Rendering" / "Shaders";
        if (!fs::exists(m_EngineShaderDir))
            GTEST_SKIP() << "Staged engine shader tree not found: " << m_EngineShaderDir.string();

        // The cook itself needs a compiler; without one there is nothing to
        // prove about serving its output.
        if (!ShaderCompileService::IsCompilerAvailable())
            GTEST_SKIP() << "no shader compiler in this build";
    }

    // One variant through the real material build path.
    MaterialBuildResult Build(const CookedProject& project, MaterialKeyword passKeywords,
                              VertexAttributeFlags vertexFlags = VertexAttributeFlags::StandardMesh)
    {
        return BuildMaterialToShaderPackage(MakeDocument(), project.MaterialPath, "cook_probe",
                                            project.Context, ShaderSourceKind::SpirV, passKeywords, vertexFlags);
    }

    fs::path m_EngineShaderDir;
};

TEST_F(MaterialVariantCook, CookedVariantsRelocateAndResolveWithoutACompiler)
{
    TempTree cookDir("ge_variant_cook_a");
    TempTree shipDir("ge_variant_cook_b");

    // --- Cook, in directory A.
    const CookedProject cooked = MakeProject(cookDir.Root, m_EngineShaderDir);
    for (const MaterialKeyword variant : {kColorVariant, kDepthVariant})
    {
        const MaterialBuildResult built = Build(cooked, variant);
        ASSERT_TRUE(built.success) << "cook failed for keywords "
                                   << static_cast<uint64_t>(variant) << ": "
                                   << (built.errors.empty() ? std::string{} : built.errors.front());
        ASSERT_NE(built.package, nullptr);
        EXPECT_FALSE(built.package->stageBytes.empty());
    }

    // --- Ship: the whole tree moves, cache included.
    std::error_code ec;
    fs::copy(cookDir.Root, shipDir.Root,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::remove_all(cookDir.Root, ec);

    // --- Run, in directory B, as a runtime with no compiler.
    const CookedProject shipped = MakeProject(shipDir.Root, m_EngineShaderDir);
    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    {
        ScopedCookOnlyRuntime cookOnly;
        ASSERT_FALSE(ShaderCompileService::IsCompilerAvailable());

        for (const MaterialKeyword variant : {kColorVariant, kDepthVariant})
        {
            const MaterialBuildResult served = Build(shipped, variant);
            ASSERT_TRUE(served.success)
                << "cooked variant " << static_cast<uint64_t>(variant)
                << " did not survive relocation: "
                << (served.errors.empty() ? std::string{} : served.errors.front());
            ASSERT_NE(served.package, nullptr);
            EXPECT_FALSE(served.package->stageBytes.empty())
                << "a served variant must carry the SPIR-V the pipeline is built from";
            EXPECT_GT(served.package->stageBytes.count("vs"), 0u);
            EXPECT_GT(served.package->stageBytes.count("fs"), 0u);
        }
    }
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore)
        << "every variant must have been SERVED from the cooked cache, not recompiled";
}

TEST_F(MaterialVariantCook, UncookedVariantFailsWithAnActionableMessage)
{
    TempTree cookDir("ge_variant_cook_gap");
    const CookedProject project = MakeProject(cookDir.Root, m_EngineShaderDir);

    ASSERT_TRUE(Build(project, kColorVariant).success);

    ScopedCookOnlyRuntime cookOnly;
    // A keyword combination the cook never enumerated.
    const MaterialBuildResult missed =
        Build(project, kColorVariant | MaterialKeyword::AlphaTest);
    EXPECT_FALSE(missed.success);
    ASSERT_FALSE(missed.errors.empty());

    std::string joined;
    for (const std::string& e : missed.errors)
        joined += e + "\n";
    EXPECT_NE(joined.find("program.shaderpkg"), std::string::npos)
        << "the miss must name the cache entry it expected:\n" << joined;
    EXPECT_NE(joined.find("Cook"), std::string::npos)
        << "the miss must say how to produce it:\n" << joined;
}

// --- Web profile -----------------------------------------------------------
//
// A browser ingests no SPIR-V, so a cooked variant that only carries SPIR-V is
// unusable there. The desktop material SPIR-V is not translatable either: the
// GE_INSTANCED path fetches instances through buffer_device_address
// (PhysicalStorageBufferAddresses, which naga refuses outright) and the
// fragment stage indexes bindless arrays non-uniformly. The compat profile
// replaces both — plain SSBO + push constant, fixed material bindings — and
// this test is the proof that what it produces survives the whole browser
// toolchain: glslc --target-env=vulkan1.1 -> spirv-opt split -> naga -> tint.

namespace
{

// The chain's own instrument check. It cooks an embedded shader end to end, so
// a pass means glslc, spirv-opt, naga AND tint are all present and working —
// exactly the preconditions this test needs, verified rather than assumed.
//
// GE_PYTHON is the same override CookWebWgslIntoPackage reads, so the guard
// cannot pass on one interpreter while the cook under test runs another.
bool ShaderCookToolchainWorks(const fs::path& script, std::string& outWhy)
{
    const char* overridden = std::getenv("GE_PYTHON");
#if defined(_WIN32)
    const std::string interpreter = overridden ? overridden : "python";
#else
    const std::string interpreter = overridden ? overridden : "python3";
#endif
    const ShellProcessResult run =
        RunProcessCaptured(interpreter, {script.string(), "--self-test"});
    outWhy = run.output.empty() ? "cannot run '" + interpreter + "'" : run.output;
    return run.exitCode == 0;
}

} // namespace

class WebMaterialVariantCook : public MaterialVariantCook
{
  protected:
    void SetUp() override
    {
        MaterialVariantCook::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        m_ShaderCookScript = fs::path(GE_RENDERER_REPO_ROOT) / "Tools" / "ShaderCook" / "shadercook.py";
        if (!fs::exists(m_ShaderCookScript))
            GTEST_SKIP() << "shadercook.py not found: " << m_ShaderCookScript.string();
        std::string why;
        if (!ShaderCookToolchainWorks(m_ShaderCookScript, why))
            GTEST_SKIP() << "WGSL toolchain unavailable (glslc/naga/tint):\n" << why;
    }

    fs::path m_ShaderCookScript;
};

TEST_F(WebMaterialVariantCook, CompatVariantsTranslateToWgslAndServeWithoutACompiler)
{
    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;
    TempTree cookDir("ge_web_variant_cook");
    const CookedProject project = MakeProject(cookDir.Root, m_EngineShaderDir);

    for (const MaterialKeyword variant : {kColorVariant, kDepthVariant})
    {
        const MaterialBuildResult built = Build(project, variant);
        ASSERT_TRUE(built.success)
            << "compat cook failed for keywords " << static_cast<uint64_t>(variant) << ": "
            << (built.errors.empty() ? std::string{} : built.errors.front());
        ASSERT_FALSE(built.composedVertexSource.empty());
        ASSERT_FALSE(built.composedFragmentSource.empty());

        GameEngine::Tools::WebWgslCookRequest request{};
        request.ShaderCookScript = m_ShaderCookScript;
        request.ScratchDir = cookDir.Root / "WebCook";
        request.PackagePath = built.generatedShaderPkgPath;
        request.VertexSource = built.composedVertexSource;
        request.FragmentSource = built.composedFragmentSource;
        request.Defines = built.composedDefines;
        request.IncludeRoots = BuildMaterialIncludeRoots(project.Context, project.MaterialPath.parent_path());
        request.DebugName = "probe_" + std::to_string(static_cast<uint64_t>(variant));

        // tint runs inside this call and a rejection fails it, so a pass here IS
        // the "translates to valid WGSL" claim — not a proxy for it.
        std::string wgslError;
        ASSERT_TRUE(GameEngine::Tools::CookWebWgslIntoPackage(request, wgslError)) << wgslError;
    }

    // --- Run as a runtime with no compiler, which is the only mode a browser has.
    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    {
        ScopedCookOnlyRuntime cookOnly;
        for (const MaterialKeyword variant : {kColorVariant, kDepthVariant})
        {
            const MaterialBuildResult served = Build(project, variant);
            ASSERT_TRUE(served.success)
                << "web variant " << static_cast<uint64_t>(variant) << " was not served: "
                << (served.errors.empty() ? std::string{} : served.errors.front());

            // The WGSL rides the package on disk; a WGSL-ingesting device is
            // served it in place of the SPIR-V, so the package is what must
            // carry it.
            ShaderPackage pkg{};
            std::string loadError;
            ASSERT_TRUE(LoadShaderPkg(served.generatedShaderPkgPath, ShaderSourceKind::Wgsl, pkg,
                                      &loadError))
                << loadError;
            ASSERT_EQ(pkg.wgslStages.count("vs"), 1u);
            ASSERT_EQ(pkg.wgslStages.count("fs"), 1u);
            const std::string vertexWgsl(pkg.stageBytes["vs"].begin(), pkg.stageBytes["vs"].end());
            const std::string fragmentWgsl(pkg.stageBytes["fs"].begin(),
                                           pkg.stageBytes["fs"].end());
            EXPECT_NE(vertexWgsl.find("@vertex"), std::string::npos);
            EXPECT_NE(fragmentWgsl.find("@fragment"), std::string::npos);
            // The push-constant block a browser rejects must have become a UBO
            // at the reserved group the backend binds its emulation buffer to.
            EXPECT_NE(vertexWgsl.find("@group(3) @binding(0)"), std::string::npos)
                << "the compat instance push constant did not become a group-3 uniform";
        }
    }
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore)
        << "the web variants must be SERVED from the cooked cache, not recompiled";
}

// The batch's failure contract (WebWgslCook.h): one error per request in request order,
// a cooked request's scratch removed and its package carrying WGSL, a failed request's
// scratch kept (its error names the file there) and its package left without WGSL.
TEST_F(WebMaterialVariantCook, ABatchReportsEachFailureAtItsOwnIndexAndKeepsOnlyItsScratch)
{
    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;
    TempTree cookDir("ge_web_variant_batch");
    const CookedProject project = MakeProject(cookDir.Root, m_EngineShaderDir);

    const MaterialBuildResult color = Build(project, kColorVariant);
    const MaterialBuildResult depth = Build(project, kDepthVariant);
    ASSERT_TRUE(color.success);
    ASSERT_TRUE(depth.success);
    // The failing request's own package: a SPIR-V-only copy nothing else writes.
    const fs::path brokenPackage = cookDir.Root / "broken.shaderpkg";
    fs::copy_file(depth.generatedShaderPkgPath, brokenPackage);

    std::vector<GameEngine::Tools::WebWgslCookRequest> requests(3);
    const MaterialBuildResult* sources[3] = {&color, &depth, &depth};
    for (size_t i = 0; i < requests.size(); ++i)
    {
        GameEngine::Tools::WebWgslCookRequest& request = requests[i];
        request.ShaderCookScript = m_ShaderCookScript;
        request.ScratchDir = cookDir.Root / "Batch" / std::to_string(i);
        request.VertexSource = sources[i]->composedVertexSource;
        request.FragmentSource = sources[i]->composedFragmentSource;
        request.Defines = sources[i]->composedDefines;
        request.IncludeRoots = BuildMaterialIncludeRoots(project.Context, project.MaterialPath.parent_path());
        request.DebugName = "batch_" + std::to_string(i);
    }
    requests[0].PackagePath = color.generatedShaderPkgPath;
    requests[1].PackagePath = brokenPackage;
    requests[1].VertexSource = "#version 450\nvoid main() { this does not compile; }\n";
    requests[2].PackagePath = depth.generatedShaderPkgPath;

    const std::vector<std::string> errors = GameEngine::Tools::CookWebWgslBatch(requests, 3);

    ASSERT_EQ(errors.size(), 3u);
    EXPECT_TRUE(errors[0].empty()) << errors[0];
    EXPECT_TRUE(errors[2].empty()) << errors[2];
    EXPECT_NE(errors[1].find("batch_1_vs.vert"), std::string::npos) << errors[1];
    EXPECT_FALSE(fs::exists(requests[0].ScratchDir));
    EXPECT_TRUE(fs::exists(requests[1].ScratchDir / "batch_1_vs.vert"));
    EXPECT_FALSE(fs::exists(requests[2].ScratchDir));

    const auto wgslStageCount = [](const fs::path& path) {
        ShaderPackage pkg{};
        std::string loadError;
        EXPECT_TRUE(LoadShaderPkg(path.string(), ShaderSourceKind::SpirV, pkg, &loadError)) << loadError;
        return pkg.wgslStages.count("vs") + pkg.wgslStages.count("fs");
    };
    EXPECT_EQ(wgslStageCount(requests[0].PackagePath), 2u);
    EXPECT_EQ(wgslStageCount(requests[1].PackagePath), 0u);
    EXPECT_EQ(wgslStageCount(requests[2].PackagePath), 2u);
}

// The cook and the runtime never configure the same include roots: an editor or
// Player mounts the asset ROOT above the engine shader tree
// (MaterialBuildContext::IncludeDirs), an offline cook points straight at the
// shader tree. If the composer spelled its substituted literals against probe
// ORDER, the shallower root would win in one host and not the other, the
// composed source would differ by one #line string, and every cooked variant
// would miss by a cache key. It spells against the deepest root that names the
// file instead, so the two hosts agree.
TEST_F(MaterialVariantCook, CookedVariantsSurviveADifferentIncludeRootShape)
{
    TempTree cookDir("ge_variant_cook_roots");
    const CookedProject cooked = MakeProject(cookDir.Root, m_EngineShaderDir);
    ASSERT_TRUE(Build(cooked, kColorVariant).success);

    // The runtime shape: the shader tree's PARENT is also an include root.
    CookedProject runtime = MakeProject(cookDir.Root, m_EngineShaderDir);
    runtime.Context.IncludeDirs = {m_EngineShaderDir.parent_path()};

    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    ScopedCookOnlyRuntime cookOnly;
    const MaterialBuildResult served =
        BuildMaterialToShaderPackage(MakeDocument(), runtime.MaterialPath, "cook_probe",
                                     runtime.Context, ShaderSourceKind::SpirV, kColorVariant,
                                     VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(served.success)
        << "a host with an extra ancestor include root did not address the cooked entry: "
        << (served.errors.empty() ? std::string{} : served.errors.front());
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore);
}

// Fog materials author Surfaces/gpu_fog_particles.glsl. The cook resolves that
// through --package-shaders GPUFogParticles/Shaders (the real surface). The
// editor stages the same file under Shaders/GPUFogParticles/Surfaces and leaves
// a one-line forwarder at Shaders/Surfaces/gpu_fog_particles.glsl. A runtime
// that only has the adapter tree hashes the forwarder and misses every cooked
// row; appending the staged module root as a package dir makes the two hosts
// resolve the same file.
TEST_F(MaterialVariantCook, FogMaterialCookHitsEditorStagedModuleRoot)
{
    const fs::path fogShaders =
        GameEngine::TestPaths::StagedRoot() / "Engine" / "Modules" / "GPUFogParticles" / "Shaders";
    if (!fs::exists(fogShaders / "Surfaces" / "gpu_fog_particles.glsl"))
        GTEST_SKIP() << "staged GPUFogParticles shaders not found: " << fogShaders.string();

    TempTree dir("ge_variant_cook_fog");
    const fs::path cacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "fog_particles.material";
    fs::create_directories(materialPath.parent_path());

    const MaterialDocument doc = GPUFogParticles::CreateDefaultMaterial();

    MaterialBuildContext cookCtx{};
    cookCtx.AdapterShaderDir = m_EngineShaderDir;
    cookCtx.PackageShaderDirs = {fogShaders};
    cookCtx.CacheRoot = cacheRoot;

    const MaterialBuildResult cooked =
        BuildMaterialToShaderPackage(doc, materialPath, "smoke-particles", cookCtx, ShaderSourceKind::SpirV,
                                     MaterialKeyword::None, VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(cooked.success) << (cooked.errors.empty() ? std::string{} : cooked.errors.front());

    TempTree editorTree("ge_variant_cook_smoke_editor");
    const fs::path editorShaders = editorTree.Root / "Shaders";
    std::error_code copyEc;
    fs::copy(m_EngineShaderDir, editorShaders, fs::copy_options::recursive, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(editorShaders / "GPUFogParticles" / "Surfaces", copyEc);
    fs::copy(fogShaders / "Surfaces", editorShaders / "GPUFogParticles" / "Surfaces",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();

    MaterialBuildContext runtimeCtx = cookCtx;
    runtimeCtx.AdapterShaderDir = editorShaders;
    runtimeCtx.PackageShaderDirs.clear();

    {
        ScopedCookOnlyRuntime cookOnly;
        const MaterialBuildResult missed =
            BuildMaterialToShaderPackage(doc, materialPath, "Smoke Particle Emitter", runtimeCtx, ShaderSourceKind::SpirV,
                                         MaterialKeyword::None, VertexAttributeFlags::StandardMesh);
        EXPECT_FALSE(missed.success)
            << "the engine Surfaces/ forwarder must not address the cooked smoke-particles key";
    }

    AppendStagedModuleShaderDirs(editorShaders, runtimeCtx.PackageShaderDirs);
    ASSERT_FALSE(runtimeCtx.PackageShaderDirs.empty());

    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    ScopedCookOnlyRuntime cookOnly;
    const MaterialBuildResult served =
        BuildMaterialToShaderPackage(doc, materialPath, "Smoke Particle Emitter", runtimeCtx, ShaderSourceKind::SpirV,
                                     MaterialKeyword::None, VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(served.success) << (served.errors.empty() ? std::string{} : served.errors.front());
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore)
        << "the editor-staged GPUFogParticles root must address the cooked entry";
}

// Tree Generator Leaves intern the same way smoke particles do: cook resolves
// Surfaces/ez_tree_leaves.glsl + the wind modifier through --package-shaders
// Packages/eztree/Assets/Shaders. A compiler-less runtime that only
// has the adapter tree cannot compose that surface at all. Staging the
// package shader tree under Shaders/EZTree and appending it as a module
// root makes intern hash the same file. Extra IncludeDirs (the editor's
// asset root) must not fork the key either — that is the cook vs runtime
// include-root shape.
// Extraction registers each emitter's material from BuildParticleRenderMaterialDocument; the
// variant cook cooks ParticleRenderMaterialShapes. An emitter's own values, textures and source
// material change no program, so the cooked shape with its lighting, layout and maps serves it on a
// runtime with no shader compiler.
TEST_F(MaterialVariantCook, ParticleShapeServesAnEmittersOwnMaterial)
{
    TempTree dir("ge_variant_cook_particles");
    MaterialBuildContext context{};
    context.AdapterShaderDir = m_EngineShaderDir;
    context.CacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "particles.material";
    fs::create_directories(materialPath.parent_path());

    Components::ParticleRenderer renderer;
    renderer.Lighting = Components::ParticleLightingMode::SixWay;
    renderer.SixWayLayout = Components::ParticleSixWayLayout::TopLeftRightBottomBackFront;
    renderer.SixWayMapA.Set(GUID::Generate());
    renderer.SixWayMapB.Set(GUID::Generate());
    renderer.Texture.Set(GUID::Generate());
    renderer.Columns = 4;
    renderer.Rows = 4;
    renderer.EmissionIntensity = 3.0f;
    const MaterialDocument source = GPUFogParticles::CreateLargeFogMaterial();
    const MaterialDocument emitterMaterial = Particles::BuildParticleRenderMaterialDocument(renderer, &source);

    // Every shape the cook writes compiles.
    const std::vector<MaterialDocument> shapes = Particles::ParticleRenderMaterialShapes();
    for (const MaterialDocument& shape : shapes)
    {
        const MaterialBuildResult cooked =
            BuildMaterialToShaderPackage(shape, materialPath, "particles", context, ShaderSourceKind::SpirV,
                                         kColorVariant, VertexAttributeFlags::StandardMesh);
        ASSERT_TRUE(cooked.success) << (cooked.errors.empty() ? std::string{} : cooked.errors.front());
    }

    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    ScopedCookOnlyRuntime cookOnly;
    const MaterialBuildResult served =
        BuildMaterialToShaderPackage(emitterMaterial, materialPath, "Particle Emitter", context, ShaderSourceKind::SpirV,
                                     kColorVariant, VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(served.success) << (served.errors.empty() ? std::string{} : served.errors.front());
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore);
}

namespace
{
// ShaderMeta's abstract stage bits (MaterialBuilder::MapAbstractStagesToBackend).
constexpr uint32_t kVertexStageBit = 1u << 0;
constexpr uint32_t kFragmentStageBit = 1u << 1;

// SPIR-V's module header length in words and the opcodes a sampling count reads: the entry point, a
// function's start, end and call, and OpImageSampleImplicitLod through OpImageSampleProjDrefExplicitLod
// (the SPIR-V specification's Mode-Setting, Function and Image Instructions).
constexpr size_t kSpirvHeaderWords = 5;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpFunction = 54;
constexpr uint32_t kOpFunctionEnd = 56;
constexpr uint32_t kOpFunctionCall = 57;
constexpr uint32_t kOpImageSampleFirst = 87;
constexpr uint32_t kOpImageSampleLast = 94;

// One function of a SPIR-V module: the sampling instructions in its body and the functions it calls,
// once per call site.
struct SpirvFunction
{
    size_t Samples = 0;
    std::vector<uint32_t> Calls;
};

size_t SampleSitesFrom(uint32_t function, const std::unordered_map<uint32_t, SpirvFunction>& functions)
{
    const auto found = functions.find(function);
    if (found == functions.end())
        return 0;
    size_t sites = found->second.Samples;
    for (const uint32_t callee : found->second.Calls)
        sites += SampleSitesFrom(callee, functions);
    return sites;
}

// The set-0 binding a compiled program declares under `name`, or nullptr.
const DescriptorBindingMeta* FindSetZeroBinding(const ShaderMeta& meta, const std::string& name)
{
    for (const DescriptorSetMeta& set : meta.Sets)
        for (const DescriptorBindingMeta& binding : set.Bindings)
            if (set.Set == 0u && binding.Name == name)
                return &binding;
    return nullptr;
}

// How many texture samples a SPIR-V module's entry point can take: its sampling instructions counted
// once per call path from the entry point, so a sampling helper called twice counts twice and one no
// path calls counts nothing. GLSL has no recursion, so the call graph is a tree walk.
size_t CountEntryPointSampleSites(const std::vector<uint8_t>& module)
{
    std::vector<uint32_t> words(module.size() / sizeof(uint32_t));
    std::memcpy(words.data(), module.data(), words.size() * sizeof(uint32_t));
    std::unordered_map<uint32_t, SpirvFunction> functions;
    uint32_t entry = 0;
    uint32_t current = 0;
    for (size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const uint32_t length = words[at] >> 16u;
        if (length == 0u || at + length > words.size())
            break;
        if (opcode == kOpEntryPoint)
            entry = words[at + 2];
        else if (opcode == kOpFunction)
            current = words[at + 2];
        else if (opcode == kOpFunctionEnd)
            current = 0;
        else if (opcode == kOpFunctionCall && current != 0)
            functions[current].Calls.push_back(words[at + 3]);
        else if (opcode >= kOpImageSampleFirst && opcode <= kOpImageSampleLast && current != 0)
            ++functions[current].Samples;
        at += length;
    }
    return SampleSitesFrom(entry, functions);
}
} // namespace

// Unlit particles divide their colour and emission by the view's exposure: the program reads the
// metered scale from the view's ExposureHistory and the static scale from ViewParams, at the offset
// the C++ upload writes. Lit particles stay scene-linear and bind neither.
TEST_F(MaterialVariantCook, UnlitParticlesReadTheViewsExposure)
{
    TempTree dir("ge_variant_cook_particle_exposure");
    MaterialBuildContext context{};
    context.AdapterShaderDir = m_EngineShaderDir;
    context.CacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "particles.material";
    fs::create_directories(materialPath.parent_path());

    Components::ParticleRenderer unlit;
    unlit.Lighting = Components::ParticleLightingMode::Unlit;
    const MaterialBuildResult unlitProgram =
        BuildMaterialToShaderPackage(Particles::BuildParticleRenderMaterialDocument(unlit, nullptr), materialPath,
                                     "unlit particles", context, ShaderSourceKind::SpirV, kColorVariant,
                                     VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(unlitProgram.success) << (unlitProgram.errors.empty() ? std::string{} : unlitProgram.errors.front());
    const DescriptorBindingMeta* history = FindSetZeroBinding(unlitProgram.package->meta, "ExposureHistory");
    ASSERT_NE(history, nullptr) << "the unlit program must read the metered exposure";
    EXPECT_EQ(history->Binding, 47u);
    EXPECT_NE(history->StagesMask & kVertexStageBit, 0u) << "the vertex stage resolves the exposure for the draw";
    EXPECT_EQ(history->StagesMask & kFragmentStageBit, 0u) << "no fragment reads the exposure";
    const DescriptorBindingMeta* viewParams = FindSetZeroBinding(unlitProgram.package->meta, "ViewParams");
    ASSERT_NE(viewParams, nullptr);
    ASSERT_TRUE(viewParams->Block.has_value());
    bool exposureAtMirrorOffset = false;
    for (const Member& member : viewParams->Block->Members)
        if (member.Name == "ge_exposureParams")
            exposureAtMirrorOffset = member.Offset == offsetof(ViewParamsUBO, ge_exposureParams);
    EXPECT_TRUE(exposureAtMirrorOffset) << "ge_exposureParams where ViewParamsUploadNode writes it";

    Components::ParticleRenderer lit;
    lit.Lighting = Components::ParticleLightingMode::Lit;
    const MaterialBuildResult litProgram =
        BuildMaterialToShaderPackage(Particles::BuildParticleRenderMaterialDocument(lit, nullptr), materialPath,
                                     "lit particles", context, ShaderSourceKind::SpirV, kColorVariant,
                                     VertexAttributeFlags::StandardMesh);
    ASSERT_TRUE(litProgram.success) << (litProgram.errors.empty() ? std::string{} : litProgram.errors.front());
    EXPECT_EQ(FindSetZeroBinding(litProgram.package->meta, "ExposureHistory"), nullptr)
        << "lit particles are scene-linear: the scene's lights set their brightness";
}

// A particle material without a texture draws the procedural shape on the default white albedo: its
// fragment program samples no sheet, where one with a texture takes the frame fetch and, behind the
// draw's blend constant, the blended frame's.
TEST_F(MaterialVariantCook, ParticlesWithoutATextureFetchNoSheet)
{
    TempTree dir("ge_variant_cook_particle_sheet");
    MaterialBuildContext context{};
    context.AdapterShaderDir = m_EngineShaderDir;
    context.CacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "particles.material";
    fs::create_directories(materialPath.parent_path());

    const auto fragmentSamples = [&](const Components::ParticleRenderer& renderer, const char* name)
    {
        const MaterialBuildResult program =
            BuildMaterialToShaderPackage(Particles::BuildParticleRenderMaterialDocument(renderer, nullptr), materialPath,
                                         name, context, ShaderSourceKind::SpirV, kColorVariant,
                                         VertexAttributeFlags::StandardMesh);
        EXPECT_TRUE(program.success) << (program.errors.empty() ? std::string{} : program.errors.front());
        if (!program.package)
            return size_t{0};
        const auto fragment = program.package->stageBytes.find("fs");
        EXPECT_NE(fragment, program.package->stageBytes.end()) << name;
        return fragment == program.package->stageBytes.end() ? size_t{0} : CountEntryPointSampleSites(fragment->second);
    };
    Components::ParticleRenderer plain;
    Components::ParticleRenderer textured;
    textured.Texture.Set(GUID::Generate());
    const size_t withoutTexture = fragmentSamples(plain, "untextured particles");
    const size_t withTexture = fragmentSamples(textured, "textured particles");
    EXPECT_EQ(withTexture - withoutTexture, 2u) << "the untextured program still samples the sheet: " << withoutTexture
                                                << " samples without a texture, " << withTexture << " with one";
}

TEST_F(MaterialVariantCook, EZTreeLeafRuntimeShapeInternsFromPackageAndStagedModuleRoot)
{
    const fs::path eztreeShaders =
        fs::path(GE_RENDERER_REPO_ROOT) / "Packages" / "eztree" / "Assets" / "Shaders";
    if (!fs::exists(eztreeShaders / "Surfaces" / "ez_tree_leaves.glsl")
        || !fs::exists(eztreeShaders / "VertexModifiers" / "ez_tree_wind.glsl"))
    {
        GTEST_SKIP() << "eztree package shaders not found: " << eztreeShaders.string();
    }

    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;
    TempTree dir("ge_variant_cook_eztree_leaf");
    const fs::path cacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "eztree_leaves.material";
    fs::create_directories(materialPath.parent_path());

    const MaterialDocument doc = EZTreeECS::MakeLeafRuntimeMaterialShape();

    MaterialBuildContext cookCtx{};
    cookCtx.AdapterShaderDir = m_EngineShaderDir;
    cookCtx.PackageShaderDirs = {eztreeShaders};
    cookCtx.CacheRoot = cacheRoot;

    const MaterialKeyword internKeywords[] = {
        MaterialKeyword::None,
        kColorVariant,
        kDepthVariant,
        kDepthVariant | MaterialKeyword::DepthOnlyFragment,
    };
    for (const MaterialKeyword keywords : internKeywords)
    {
        const MaterialBuildResult cooked =
            BuildMaterialToShaderPackage(doc, materialPath, "eztree-leaves", cookCtx, ShaderSourceKind::SpirV, keywords,
                                         VertexAttributeFlags::StandardMesh);
        ASSERT_TRUE(cooked.success) << (cooked.errors.empty() ? std::string{} : cooked.errors.front());
    }

    // Runtime A: cook's package root plus the editor asset-root IncludeDir.
    MaterialBuildContext includeRuntime = cookCtx;
    includeRuntime.IncludeDirs = {m_EngineShaderDir.parent_path()};
    {
        const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
        ScopedCookOnlyRuntime cookOnly;
        for (const MaterialKeyword keywords : internKeywords)
        {
            const MaterialBuildResult served =
                BuildMaterialToShaderPackage(doc, materialPath, "Tree Generator Leaves",
                                             includeRuntime, ShaderSourceKind::SpirV, keywords,
                                             VertexAttributeFlags::StandardMesh);
            ASSERT_TRUE(served.success)
                << "extra IncludeDirs must not miss the cooked leaf key: "
                << (served.errors.empty() ? std::string{} : served.errors.front());
        }
        EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore);
    }

    // Runtime B: no package mount — only the staged Shaders/EZTree module root,
    // which is what AppendStagedModuleShaderDirs sees on wasm after preload.
    TempTree editorTree("ge_variant_cook_eztree_editor");
    const fs::path editorShaders = editorTree.Root / "Shaders";
    std::error_code copyEc;
    fs::copy(m_EngineShaderDir, editorShaders, fs::copy_options::recursive, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(editorShaders / "EZTree", copyEc);
    fs::copy(eztreeShaders, editorShaders / "EZTree",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();

    MaterialBuildContext stagedRuntime = cookCtx;
    stagedRuntime.AdapterShaderDir = editorShaders;
    stagedRuntime.PackageShaderDirs.clear();
    stagedRuntime.IncludeDirs = {editorShaders.parent_path()};
    {
        ScopedCookOnlyRuntime cookOnly;
        const MaterialBuildResult missed =
            BuildMaterialToShaderPackage(doc, materialPath, "Tree Generator Leaves", stagedRuntime, ShaderSourceKind::SpirV,
                                         kColorVariant, VertexAttributeFlags::StandardMesh);
        EXPECT_FALSE(missed.success)
            << "the adapter tree without EZTree as a module root must not intern the cooked leaf key";
    }

    AppendStagedModuleShaderDirs(editorShaders, stagedRuntime.PackageShaderDirs);
    ASSERT_FALSE(stagedRuntime.PackageShaderDirs.empty());

    const uint64_t compilesBefore = ShaderCompileService::GetTotalCompilations();
    ScopedCookOnlyRuntime cookOnly;
    for (const MaterialKeyword keywords : internKeywords)
    {
        const MaterialBuildResult served =
            BuildMaterialToShaderPackage(doc, materialPath, "Tree Generator Leaves", stagedRuntime, ShaderSourceKind::SpirV,
                                         keywords, VertexAttributeFlags::StandardMesh);
        ASSERT_TRUE(served.success)
            << "the editor-staged EZTree module root must address the cooked leaf key: "
            << (served.errors.empty() ? std::string{} : served.errors.front());
    }
    EXPECT_EQ(ShaderCompileService::GetTotalCompilations(), compilesBefore);
}

// ShaderCompilationCache::GetOrCompile does not intern the extractor's full
// document. It synthesizes surface + lighting + modifier and relies on the
// registry variant key for AlphaTest / HasVertexMod. Cook uses the full
// document. If those two hashes diverge, wasm reports "no cooked program"
// for Tree Generator Leaves even though eztree-leaves rows exist.
TEST_F(MaterialVariantCook, EZTreeLeafGetOrCompileShapeInternsCookedRows)
{
    const fs::path eztreeShaders =
        fs::path(GE_RENDERER_REPO_ROOT) / "Packages" / "eztree" / "Assets" / "Shaders";
    if (!fs::exists(eztreeShaders / "Surfaces" / "ez_tree_leaves.glsl")
        || !fs::exists(eztreeShaders / "VertexModifiers" / "ez_tree_wind.glsl"))
    {
        GTEST_SKIP() << "eztree package shaders not found: " << eztreeShaders.string();
    }

    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;
    TempTree dir("ge_variant_cook_eztree_leaf_intern");
    const fs::path cacheRoot = dir.Root / ".Cache" / "Shaders";
    const fs::path materialPath = dir.Root / "Synthetic" / "eztree_leaves.material";
    fs::create_directories(materialPath.parent_path());

    const MaterialDocument cookDoc = EZTreeECS::MakeLeafRuntimeMaterialShape();
    MaterialDocument internDoc{};
    internDoc.surfaceShader = cookDoc.surfaceShader;
    internDoc.lightingModel = cookDoc.lightingModel;
    internDoc.vertexModifier = cookDoc.vertexModifier;

    MaterialBuildContext cookCtx{};
    cookCtx.AdapterShaderDir = m_EngineShaderDir;
    cookCtx.PackageShaderDirs = {eztreeShaders};
    cookCtx.CacheRoot = cacheRoot;

    const MaterialKeyword registryBase =
        MaterialKeyword::AlphaTest | MaterialKeyword::HasVertexMod;
    const MaterialKeyword colorPass = kColorVariant | registryBase;
    const MaterialKeyword internKeywords[] = {
        MaterialKeyword::None,
        kColorVariant,
        kDepthVariant,
        kDepthVariant | MaterialKeyword::DepthOnlyFragment,
    };
    for (const MaterialKeyword keywords : internKeywords)
    {
        const MaterialBuildResult cooked =
            BuildMaterialToShaderPackage(cookDoc, materialPath, "eztree-leaves", cookCtx, ShaderSourceKind::SpirV, keywords,
                                         VertexAttributeFlags::StandardMesh);
        ASSERT_TRUE(cooked.success) << (cooked.errors.empty() ? std::string{} : cooked.errors.front());
    }

    const fs::path internPath = cacheRoot / "Synthetic" / "cache_leaf_intern.material";
    auto internOnce = [&](const char* label, MaterialDocument doc, MaterialKeyword keywords) {
        MaterialBuildContext ctx = cookCtx;
        std::vector<std::string> prepErrors;
        if (!Engine::Renderer::PrepareMaterialDocumentForShaderPackage(doc, internPath, ctx,
                                                                       prepErrors))
        {
            ADD_FAILURE() << label << " Prepare failed: "
                          << (prepErrors.empty() ? std::string{} : prepErrors.front());
            return;
        }
        const MaterialBuildResult served =
            BuildMaterialToShaderPackage(doc, internPath, "Tree Generator Leaves", ctx, ShaderSourceKind::SpirV, keywords,
                                         VertexAttributeFlags::StandardMesh);
        EXPECT_TRUE(served.success)
            << label << ": " << (served.errors.empty() ? std::string{} : served.errors.front())
            << " expected " << served.generatedShaderPkgPath;
        EXPECT_NE(served.composedFragmentSource.find("#include \"Surfaces/ez_tree_leaves.glsl\""),
                  std::string::npos)
            << label << " intern fragment must spell the authored leaf surface, not an absolute path";
        EXPECT_NE(served.composedVertexSource.find("#include \"VertexModifiers/ez_tree_wind.glsl\""),
                  std::string::npos)
            << label << " intern vertex must spell the authored wind modifier, not an absolute path";
    };

    // GetOrCompile synthesizes a surface/lighting/modifier document. Registry
    // intern stamps AlphaTest on the variant key; reconstituting Mask on the
    // synthetic doc also ORs it, so a keyword-less intern still hashes the
    // cook's Mask row.
    MaterialDocument internDocMask = internDoc;
    internDocMask.alphaMode = MaterialAlphaMode::Mask;

    ScopedCookOnlyRuntime cookOnly;
    internOnce("stripped+registryBase", internDoc, registryBase);
    internOnce("stripped+colorPass", internDoc, colorPass);
    internOnce("stripped+depthMask", internDoc,
               kDepthVariant | registryBase | MaterialKeyword::DepthOnlyFragment);
    internOnce("stripped+Mask+None", internDocMask, MaterialKeyword::None);
    internOnce("stripped+Mask+color", internDocMask, kColorVariant);
    internOnce("stripped+Mask+depthMask", internDocMask,
               kDepthVariant | MaterialKeyword::DepthOnlyFragment);
    internOnce("full+None", cookDoc, MaterialKeyword::None);
    internOnce("full+kColorVariant", cookDoc, kColorVariant);
    internOnce("full+registryBase", cookDoc, registryBase);
    internOnce("full+colorPass", cookDoc, colorPass);
    internOnce("full+depthMask", cookDoc,
               kDepthVariant | MaterialKeyword::DepthOnlyFragment);
}

TEST_F(MaterialVariantCook, EZTreeLeafInternsShippedWebEditorCache)
{
    const fs::path cacheRoot =
        fs::path(GE_RENDERER_REPO_ROOT) / "build" / "wasm-release" / "WebEditorMaterialCache";
    const fs::path eztreeShaders =
        fs::path(GE_RENDERER_REPO_ROOT) / "Packages" / "eztree" / "Assets" / "Shaders";
    if (!fs::is_directory(cacheRoot) || !fs::exists(eztreeShaders / "Surfaces" / "ez_tree_leaves.glsl"))
        GTEST_SKIP() << "shipped web editor cache not present: " << cacheRoot.string();

    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;
    const MaterialDocument cookDoc = EZTreeECS::MakeLeafRuntimeMaterialShape();
    MaterialDocument internDoc{};
    internDoc.surfaceShader = cookDoc.surfaceShader;
    internDoc.lightingModel = cookDoc.lightingModel;
    internDoc.vertexModifier = cookDoc.vertexModifier;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = m_EngineShaderDir;
    ctx.PackageShaderDirs = {eztreeShaders};
    ctx.CacheRoot = cacheRoot;
    ctx.IncludeDirs = {m_EngineShaderDir.parent_path().parent_path().parent_path()};

    TempTree editorTree("ge_variant_cook_eztree_webcache");
    const fs::path editorShaders = editorTree.Root / "Shaders";
    std::error_code copyEc;
    fs::copy(m_EngineShaderDir, editorShaders, fs::copy_options::recursive, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(editorShaders / "EZTree", copyEc);
    fs::copy(eztreeShaders, editorShaders / "EZTree",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();

    MaterialBuildContext wasmCtx = ctx;
    wasmCtx.AdapterShaderDir = editorShaders;
    wasmCtx.PackageShaderDirs.clear();
    wasmCtx.IncludeDirs = {editorShaders.parent_path()};
    AppendStagedModuleShaderDirs(editorShaders, wasmCtx.PackageShaderDirs);

    const MaterialKeyword registryBase =
        MaterialKeyword::AlphaTest | MaterialKeyword::HasVertexMod;
    const fs::path internPath = cacheRoot / "Synthetic" / "cache_leaf_web.material";
    MaterialDocument internDocMask = internDoc;
    internDocMask.alphaMode = MaterialAlphaMode::Mask;

    auto internOnce = [&](const char* label, MaterialBuildContext internCtx, MaterialDocument doc,
                          MaterialKeyword keywords) {
        std::vector<std::string> prepErrors;
        if (!Engine::Renderer::PrepareMaterialDocumentForShaderPackage(doc, internPath, internCtx,
                                                                       prepErrors))
        {
            ADD_FAILURE() << label << " Prepare failed: "
                          << (prepErrors.empty() ? std::string{} : prepErrors.front());
            return;
        }
        const MaterialBuildResult served =
            BuildMaterialToShaderPackage(doc, internPath, "Tree Generator Leaves", internCtx, ShaderSourceKind::SpirV,
                                         keywords, VertexAttributeFlags::StandardMesh);
        const bool pkgExists =
            !served.generatedShaderPkgPath.empty() && fs::exists(served.generatedShaderPkgPath);
        EXPECT_TRUE(served.success)
            << label << ": " << (served.errors.empty() ? std::string{} : served.errors.front())
            << " expected " << served.generatedShaderPkgPath << " exists=" << pkgExists;
        if (!served.success && !served.composedDefines.empty())
        {
            std::string defs;
            for (const auto& d : served.composedDefines)
            {
                if (!defs.empty())
                    defs += " ";
                defs += d;
            }
            ADD_FAILURE() << label << " defines: " << defs;
        }
    };

    ScopedCookOnlyRuntime cookOnly;
    internOnce("cookCtx stripped registryBase", ctx, internDoc, registryBase);
    internOnce("cookCtx full None", ctx, cookDoc, MaterialKeyword::None);
    internOnce("wasmCtx stripped registryBase", wasmCtx, internDoc, registryBase);
    internOnce("wasmCtx full None", wasmCtx, cookDoc, MaterialKeyword::None);
    internOnce("wasmCtx stripped colorPass", wasmCtx, internDoc, kColorVariant | registryBase);
    internOnce("wasmCtx stripped depthMask", wasmCtx, internDoc,
               registryBase | MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment);
    internOnce("wasmCtx stripped color+IBL", wasmCtx, internDoc,
               kColorVariant | registryBase | MaterialKeyword::IBL);
    internOnce("wasmCtx stripped color+SSSR", wasmCtx, internDoc,
               kColorVariant | registryBase | MaterialKeyword::SSSRNormalRoughness);
    internOnce("wasmCtx stripped color+IBL+GTAO+SSSR", wasmCtx, internDoc,
               kColorVariant | registryBase | MaterialKeyword::IBL | MaterialKeyword::GTAO
                   | MaterialKeyword::SSSRNormalRoughness);
    internOnce("wasmCtx stripped Mask+None", wasmCtx, internDocMask, MaterialKeyword::None);
    internOnce("wasmCtx stripped Mask+color", wasmCtx, internDocMask, kColorVariant);

    const fs::path webShaders =
        fs::path(GE_RENDERER_REPO_ROOT) / "build" / "wasm-release" / "WebEditorShaders";
    const fs::path engineShadersSrc =
        fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "Rendering" / "Shaders";
    if (!fs::is_directory(webShaders) || !fs::is_directory(engineShadersSrc / "Adapters"))
        return;

    TempTree memfs("ge_variant_cook_eztree_memfs");
    const fs::path assets = memfs.Root / "Assets";
    const fs::path shaders = assets / "Shaders";
    fs::create_directories(shaders);
    fs::copy(webShaders, shaders, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    for (const char* sub : {"Adapters", "Includes", "Surfaces"})
    {
        fs::copy(engineShadersSrc / sub, shaders / sub,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
        ASSERT_FALSE(copyEc) << copyEc.message();
    }
    fs::create_directories(shaders / "EZTree");
    fs::copy(eztreeShaders, shaders / "EZTree",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::copy(eztreeShaders / "Surfaces" / "ez_tree_leaves.glsl",
             shaders / "Surfaces" / "ez_tree_leaves.glsl",
             fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(shaders / "VertexModifiers");
    fs::copy(eztreeShaders / "VertexModifiers" / "ez_tree_wind.glsl",
             shaders / "VertexModifiers" / "ez_tree_wind.glsl",
             fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::copy(fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "GPUFogParticles" / "Shaders"
                 / "Surfaces",
             shaders / "GPUFogParticles" / "Surfaces",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);

    MaterialBuildContext memfsCtx{};
    memfsCtx.AdapterShaderDir = shaders;
    memfsCtx.CacheRoot = cacheRoot;
    memfsCtx.IncludeDirs = {assets};
    memfsCtx.ProjectRoots = {fs::path(GE_RENDERER_REPO_ROOT) / "Tools" / "Web" / "smoke-project"};
    memfsCtx.PackageShaderDirs = {eztreeShaders};
    AppendStagedModuleShaderDirs(shaders, memfsCtx.PackageShaderDirs);

    internOnce("memfs stripped registryBase", memfsCtx, internDoc, registryBase);
    internOnce("memfs stripped colorPass", memfsCtx, internDoc, kColorVariant | registryBase);
    internOnce("memfs stripped color+SSSR", memfsCtx, internDoc,
               kColorVariant | registryBase | MaterialKeyword::SSSRNormalRoughness);
    internOnce("memfs stripped color+IBL+GTAO+SSSR", memfsCtx, internDoc,
               kColorVariant | registryBase | MaterialKeyword::IBL | MaterialKeyword::GTAO
                   | MaterialKeyword::SSSRNormalRoughness);
    internOnce("memfs stripped Mask+None", memfsCtx, internDocMask, MaterialKeyword::None);
    internOnce("memfs stripped Mask+color", memfsCtx, internDocMask, kColorVariant);

    MaterialBuildContext adapterOnly = memfsCtx;
    adapterOnly.PackageShaderDirs.clear();
    AppendStagedModuleShaderDirs(shaders, adapterOnly.PackageShaderDirs);
    internOnce("adapterOnly stripped registryBase", adapterOnly, internDoc, registryBase);
    internOnce("adapterOnly stripped Mask+None", adapterOnly, internDocMask, MaterialKeyword::None);
}

// The dither/A2C grass materials are the keyworded siblings of the Blend one
// (doc.keywords = GRASS_DITHER / GRASS_A2C select the surface's alpha path).
// Same divergence class the EZTree test above pins: the wasm runtime interns a
// synthesized doc + registry keywords, the cook interns the full module doc.
// The Blend rows serve as the control — if Blend interns and Dither does not,
// the skew is in the keyword lane specifically.
TEST_F(MaterialVariantCook, TerrainGrassDitherInternsShippedWebEditorCache)
{
    const fs::path cacheRoot =
        fs::path(GE_RENDERER_REPO_ROOT) / "build" / "wasm-release" / "WebEditorMaterialCache";
    const fs::path grassShaders = fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules"
                                  / "TerrainGrass" / "Shaders" / "TerrainGrass";
    const fs::path cbtShaders =
        fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "CBTTerrain" / "Shaders" / "CBT";
    const fs::path engineShadersSrc =
        fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "Rendering" / "Shaders";
    if (!fs::is_directory(cacheRoot) || !fs::is_directory(grassShaders))
        GTEST_SKIP() << "shipped web editor cache not present: " << cacheRoot.string();

    ScopedCompatShaderProfile compat;
    ScopedWebCacheSpirvTarget webSpirv;

    // The wasm editor's /Assets/Shaders: engine tree + the TerrainGrass and CBT
    // preload mounts (Apps/Editor/CMakeLists.txt --preload-file list).
    TempTree memfs("ge_variant_cook_grass_dither_web");
    const fs::path shaders = memfs.Root / "Assets" / "Shaders";
    std::error_code copyEc;
    fs::create_directories(shaders);
    fs::copy(engineShadersSrc, shaders,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(shaders / "TerrainGrass");
    fs::copy(grassShaders, shaders / "TerrainGrass",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();
    fs::create_directories(shaders / "CBT");
    fs::copy(cbtShaders, shaders / "CBT",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
    ASSERT_FALSE(copyEc) << copyEc.message();

    MaterialBuildContext wasmCtx{};
    wasmCtx.AdapterShaderDir = shaders;
    wasmCtx.CacheRoot = cacheRoot;
    wasmCtx.IncludeDirs = {shaders.parent_path()};
    AppendStagedModuleShaderDirs(shaders, wasmCtx.PackageShaderDirs);

    auto internOnce = [&](const char* label, MaterialDocument doc, MaterialKeyword keywords) {
        const fs::path internPath = cacheRoot / "Synthetic" / "cache_grass_web.material";
        MaterialBuildContext ctx = wasmCtx;
        std::vector<std::string> prepErrors;
        if (!Engine::Renderer::PrepareMaterialDocumentForShaderPackage(doc, internPath, ctx,
                                                                       prepErrors))
        {
            ADD_FAILURE() << label << " Prepare failed: "
                          << (prepErrors.empty() ? std::string{} : prepErrors.front());
            return;
        }
        const MaterialBuildResult served = BuildMaterialToShaderPackage(
            doc, internPath, label, ctx, ShaderSourceKind::SpirV, keywords, VertexAttributeFlags::None);
        EXPECT_TRUE(served.success)
            << label << ": " << (served.errors.empty() ? std::string{} : served.errors.front())
            << " expected " << served.generatedShaderPkgPath;
    };

    // What the wasm runtime asks: GetOrCompile synthesizes surface + lighting +
    // modifier + userKeywords; the registry key contributes HasVertexMod and the
    // draw contributes the world pass keywords (the cook's grass-forward row).
    const MaterialKeyword grassPass =
        MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::Instanced |
        MaterialKeyword::IBL | MaterialKeyword::ProceduralVertexOutput |
        MaterialKeyword::HasVertexMod;

    auto runtimeShapeOf = [](const MaterialDocument& cookDoc) {
        MaterialDocument doc{};
        doc.surfaceShader = cookDoc.surfaceShader;
        doc.lightingModel = cookDoc.lightingModel;
        doc.vertexModifier = cookDoc.vertexModifier;
        doc.keywords = cookDoc.keywords;
        return doc;
    };

    // The per-mode document TerrainGrassRenderFeature registers, mirrored here
    // because that builder is private to the feature TU. Only the fields the cook
    // keys on are reproduced; a drift in the mode set shows up as an uncooked
    // variant at runtime, which is what the GrassDrawMode switch below guards.
    const auto grassDocFor = [](TerrainGrass::GrassDrawMode mode) {
        MaterialDocument doc{};
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
        doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
        doc.doubleSided = true;
        doc.properties["roughness"] = 0.9f;
        switch (mode)
        {
        case TerrainGrass::GrassDrawMode::Blend:
            doc.materialName = "Terrain/Grass";
            doc.alphaMode = MaterialAlphaMode::Blend;
            break;
        case TerrainGrass::GrassDrawMode::Opaque:
            doc.materialName = "Terrain/GrassOpaque";
            doc.alphaMode = MaterialAlphaMode::Opaque;
            doc.keywords = {"GRASS_OPAQUE"};
            break;
        case TerrainGrass::GrassDrawMode::Dither:
            doc.materialName = "Terrain/GrassDither";
            doc.alphaMode = MaterialAlphaMode::Opaque;
            doc.keywords = {"GRASS_DITHER"};
            break;
        case TerrainGrass::GrassDrawMode::DitherA2C:
            doc.materialName = "Terrain/GrassA2C";
            doc.alphaMode = MaterialAlphaMode::Opaque;
            doc.keywords = {"GRASS_A2C"};
            break;
        }
        return doc;
    };
    const MaterialDocument blendDoc = grassDocFor(TerrainGrass::GrassDrawMode::Blend);
    const MaterialDocument opaqueDoc = grassDocFor(TerrainGrass::GrassDrawMode::Opaque);
    const MaterialDocument ditherDoc = grassDocFor(TerrainGrass::GrassDrawMode::Dither);
    const MaterialDocument a2cDoc = grassDocFor(TerrainGrass::GrassDrawMode::DitherA2C);

    ScopedCookOnlyRuntime cookOnly;
    internOnce("blend stripped base", runtimeShapeOf(blendDoc), MaterialKeyword::None);
    internOnce("blend stripped forward", runtimeShapeOf(blendDoc), grassPass);
    internOnce("dither stripped base", runtimeShapeOf(ditherDoc), MaterialKeyword::None);
    internOnce("dither stripped forward", runtimeShapeOf(ditherDoc), grassPass);
    internOnce("a2c stripped forward", runtimeShapeOf(a2cDoc), grassPass);
    internOnce("opaque stripped forward", runtimeShapeOf(opaqueDoc), grassPass);
    internOnce("dither full forward", ditherDoc, grassPass);
    const MaterialKeyword ddgiPasses[] = {
        grassPass | MaterialKeyword::DDGI,
        grassPass | MaterialKeyword::DDGI | MaterialKeyword::GTAO,
        grassPass | MaterialKeyword::DDGI | MaterialKeyword::SSSRNormalRoughness,
        grassPass | MaterialKeyword::DDGI | MaterialKeyword::GTAO |
            MaterialKeyword::SSSRNormalRoughness,
    };
    for (const MaterialDocument* doc : {&blendDoc, &opaqueDoc, &ditherDoc, &a2cDoc})
    {
        for (const MaterialKeyword pass : ddgiPasses)
            internOnce("grass DDGI world-pass variant", runtimeShapeOf(*doc), pass);
    }
}
