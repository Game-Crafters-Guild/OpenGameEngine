#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

void CopyGraphSamples(const fs::path& project)
{
    const fs::path source = GameEngine::TestPaths::StagedRoot() / "Assets/Materials/Graph";
    ASSERT_TRUE(fs::is_regular_file(source / "GraphSamplePBR.material"));
    ASSERT_TRUE(fs::is_regular_file(source / "PreviewSphereTest.material"));
    const fs::path graphs = project / "Assets/Materials/Graph";
    fs::create_directories(graphs);
    fs::copy(source, graphs, fs::copy_options::recursive);
}

// The repository's script, as the web editor's cook uses: setup.py provisions naga
// and tint in the toolchain directory beside it, where the script looks first.
fs::path ShaderCookScript()
{
    return fs::path(GE_RENDERER_REPO_ROOT) / "Tools/ShaderCook/shadercook.py";
}

GameEngine::ShellProcessResult Cook(const fs::path& project, bool web = false,
                                    const std::vector<std::string>& extraArguments = {})
{
    const fs::path fixtures = GameEngine::TestPaths::StagedRoot();
    const fs::path executableDirectory = GameEngine::TestPaths::ExecutableDirectory();
    const fs::path tool = executableDirectory.parent_path() / "Tools" /
                          GE_MATERIAL_VARIANT_COOK_FILENAME;
    std::vector<std::string> arguments{
        "--project", (project / "Assets").string(),
        "--engine-shaders", (fixtures / "Engine/Modules/Rendering/Shaders").string(),
        "--package-shaders", (fixtures / "Engine/Modules/GPUFogParticles/Shaders").string(),
        "--package-shaders", (executableDirectory /
            "Fixtures/MaterialVariantCook/PackageShaders").string(),
        "--cache", (project / ".Cache/Shaders").string(), "--verify"};
    arguments.insert(arguments.end(), extraArguments.begin(), extraArguments.end());
    if (web)
    {
        arguments.emplace_back("--web");
        arguments.emplace_back("--shadercook");
        arguments.push_back(ShaderCookScript().string());
    }
    // The web cook translates every built-in target; on a loaded host it takes 25 minutes.
    return GameEngine::RunProcessCaptured(tool.string(), arguments, std::chrono::minutes(45));
}

std::size_t CookedVariantCount(const std::string& output, const std::string& material)
{
    const std::string prefix = "  ok   " + material + " [";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        ++count;
        position += prefix.size();
    }
    return count;
}

// The rows `material` cooked whose row name carries the vertex-colour stream ("+color").
std::size_t CookedVertexColorVariantCount(const std::string& output, const std::string& material)
{
    const std::string prefix = "  ok   " + material + " [";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        position += prefix.size();
        const std::string row = output.substr(position, output.find(']', position) - position);
        count += row.find("+color") != std::string::npos ? 1u : 0u;
    }
    return count;
}

// A cook that verified, and the table it cooked: the imported mesh materials (the built-in default PBR)
// with their vertex-colour rows, a project's materials with the same table less those rows, and no
// particle shape with any.
void CheckCook(const GameEngine::ShellProcessResult& result)
{
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("verify: every cooked variant resolves with no compiler"),
              std::string::npos) << result.output;
    const auto controlCount = CookedVariantCount(result.output, "engine-default-pbr");
    const auto controlColorCount = CookedVertexColorVariantCount(result.output, "engine-default-pbr");
    ASSERT_GT(controlCount, 0u) << result.output;
    EXPECT_GT(controlColorCount, 0u) << "the default material cooks the vertex-colour rows\n" << result.output;
    for (const char* sample : {"GraphSamplePBR", "PreviewSphereTest"})
        EXPECT_EQ(CookedVariantCount(result.output, sample), controlCount - controlColorCount)
            << sample << '\n' << result.output;
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    for (std::size_t shape = 0; shape < shapes; ++shape)
        EXPECT_EQ(CookedVertexColorVariantCount(result.output, "particles-" + std::to_string(shape)), 0u)
            << "particles-" << shape << " cooks no vertex-colour row\n" << result.output;
}

std::set<std::string> FileNames(const fs::path& directory)
{
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(directory))
        names.insert(entry.path().filename().string());
    return names;
}

// A project whose one material binds a height map on the standard surface, which is the whole opt-in
// to the relief march; the map's GUID need not resolve for the program to compose.
void WriteReliefMaterial(const fs::path& project)
{
    const fs::path materials = project / "Assets/Materials";
    fs::create_directories(materials);
    std::ofstream(materials / "Relief.material") << R"({
  "schemaVersion": 3,
  "materialName": "Relief",
  "lightingModel": "StandardPBR",
  "alphaMode": "Opaque",
  "surfaceShader": "Surfaces/standard_pbr.glsl",
  "textures": {
    "heightMap": {"guid": "69a5e97a-f000-40f6-924c-c14f53956ed3", "path": "Textures/height.png"}
  }
})";
}

// Each cooked variant of `material` with its package, from the cook's "  ok   <material> [<variant>]
// -> <package>" lines.
std::vector<std::pair<std::string, fs::path>> CookedPackages(const std::string& output, const std::string& material)
{
    std::vector<std::pair<std::string, fs::path>> packages;
    const std::string prefix = "  ok   " + material + " [";
    const std::string arrow = "] -> ";
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        const std::size_t variantStart = position + prefix.size();
        const std::size_t arrowAt = output.find(arrow, variantStart);
        const std::size_t lineEnd = output.find_first_of("\r\n", variantStart);
        if (arrowAt == std::string::npos || arrowAt > lineEnd)
            break;
        packages.emplace_back(output.substr(variantStart, arrowAt - variantStart),
                              fs::path(output.substr(arrowAt + arrow.size(), lineEnd - arrowAt - arrow.size())));
        position = lineEnd;
    }
    return packages;
}

// SPIR-V's OpCapability, and the capability interpolateAtOffset needs (gated on sampleRateShading).
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr std::size_t kSpirvHeaderWords = 5;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kCapabilityInterpolationFunction = 52;

// Whether a SPIR-V module declares `capability`; capabilities come first after the header.
bool DeclaresCapability(const std::vector<uint8_t>& bytes, uint32_t capability)
{
    if (bytes.size() % sizeof(uint32_t) != 0 || bytes.size() < kSpirvHeaderWords * sizeof(uint32_t))
        return false;
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] != kSpirvMagic)
        return false;
    for (std::size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size())
            return false;
        if (opcode == kOpCapability && count == 2 && words[at + 1] == capability)
            return true;
        at += count;
    }
    return false;
}

std::map<std::string, fs::file_time_type> PackageWriteTimes(const fs::path& cache)
{
    std::map<std::string, fs::file_time_type> result;
    for (const auto& entry : fs::recursive_directory_iterator(cache))
        if (entry.path().extension() == ".shaderpkg")
            result.emplace(entry.path().lexically_relative(cache).generic_string(),
                           entry.last_write_time());
    return result;
}
} // namespace

TEST(MaterialVariantCookIntegration, CommittedGraphSamplesCookAndVerifyWithoutACompiler)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_sample_cook")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    const auto result = Cook(project.Path());
    ASSERT_NO_FATAL_FAILURE(CheckCook(result));
    // Every particle shape cooks exactly the particle renderer's request set, not the mesh-material table.
    std::set<std::string> requested;
    for (const auto& variant : GameEngine::Particles::ParticleRenderVariants())
        requested.insert(variant.Name);
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    for (std::size_t shape = 0; shape < shapes; ++shape)
    {
        std::set<std::string> cooked;
        for (const auto& [variant, package] : CookedPackages(result.output, "particles-" + std::to_string(shape)))
            cooked.insert(variant);
        EXPECT_EQ(cooked, requested) << "particles-" << shape;
    }
    const fs::path generated = project.Path() / ".Cache/Shaders/Generated";
    ASSERT_TRUE(fs::is_directory(generated)) << generated;
    std::size_t materializedSamples = 0;
    for (const auto& entry : fs::recursive_directory_iterator(generated))
        if (entry.path().extension() == ".glsl")
            ++materializedSamples;
    EXPECT_EQ(materializedSamples, 2u);
}

TEST(MaterialVariantCookIntegration, RelocatedGraphSamplesReuseEveryCookedPackage)
{
    GameEngine::TestUtils::ScopedTempDir original{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_original")};
    GameEngine::TestUtils::ScopedTempDir relocated{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_relocated")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(original.Path()));
    ASSERT_NO_FATAL_FAILURE(CheckCook(Cook(original.Path())));
    fs::copy(original.Path(), relocated.Path(), fs::copy_options::recursive);
    const fs::path cache = relocated.Path() / ".Cache/Shaders";
    const auto before = PackageWriteTimes(cache);
    ASSERT_FALSE(before.empty());
    ASSERT_NO_FATAL_FAILURE(CheckCook(Cook(relocated.Path())));
    EXPECT_EQ(PackageWriteTimes(cache), before)
        << "Relocating a cooked project must neither add nor rewrite shader packages";
}

// A cooked graph variant is found at runtime only if the cook and the runtime
// name its generated surface alike: the cook from its --project and --scan
// roots, the runtime from its mount table. The samples sit at one relative path
// in the project, in an embedded package whose alias the cook derives from its
// manifest, and in a root named by a mixed-case --scan-alias that the runtime
// lower-cases; no two of them may share a surface.
TEST(MaterialVariantCookIntegration, CookNamesGraphSurfacesLikeTheRuntimeMountTable)
{
    using namespace GameEngine;
    TestUtils::ScopedTempDir project{TestUtils::MakeUniqueTempDirectory("material_graph_mounts")};
    TestUtils::ScopedTempDir extra{TestUtils::MakeUniqueTempDirectory("material_graph_extra")};
    TestUtils::ScopedTempDir runtime{TestUtils::MakeUniqueTempDirectory("material_graph_runtime")};
    const fs::path package = project.Path() / "Packages/graph.samples_kit";
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(package));
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(extra.Path()));
    std::ofstream(package / "package.json") << R"({"name": "graph.samples_kit", "version": "1.0.0"})";
    std::ofstream(project.Path() / "Packages/manifest.json")
        << R"({"dependencies": {"graph.samples_kit": "embedded"}})";

    const ShellProcessResult cooked =
        Cook(project.Path(), false,
             {"--scan", (package / "Assets").string(), "--scan", (extra.Path() / "Assets").string(),
              "--scan-alias", "Graph-Samples"});
    ASSERT_FALSE(cooked.cancelled) << cooked.output;
    ASSERT_EQ(cooked.exitCode, 0) << cooked.output;

    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(project.Path() / "Assets", nullptr,
                                  runtime.Path() / "registry.assetdb", runtime.Path() / "Cache"));
    const PackageResolution packages =
        PackageResolver::Resolve(project.Path(), runtime.Path() / "NoEnginePackages");
    ASSERT_TRUE(packages.Errors.empty()) << packages.Errors.front();
    ASSERT_EQ(MountResolvedPackages(assets, packages, runtime.Path() / "AssetDatabase").size(), 1u);
    AssetSourceDesc extraSource{};
    extraSource.Alias = "Graph-Samples";
    extraSource.Root = extra.Path() / "Assets";
    ASSERT_TRUE(assets.RegisterSource(extraSource));
    Rendering::MaterialBuildContext context;
    context.ProjectRoots = CollectProjectRoots(assets);
    context.AssetSourceRoots = CollectAssetSourceRoots(assets);
    context.CacheRoot = runtime.Path() / "Shaders";
    assets.Shutdown();

    const std::set<std::string> cookedNames = FileNames(project.Path() / ".Cache/Shaders/Generated");
    std::set<std::string> runtimeNames;
    for (const fs::path& root : {project.Path(), package, extra.Path()})
    {
        for (const std::string sample : {"GraphSamplePBR", "PreviewSphereTest"})
        {
            const fs::path material = root / "Assets/Materials/Graph" / (sample + ".material");
            MaterialAsset asset(GUID::Generate(), material);
            ASSERT_TRUE(asset.Load()) << material;
            const Engine::Renderer::MaterialShaderPackageBuilder builder(
                asset.GetDocument(), material, sample, context);
            ASSERT_TRUE(builder.IsPrepared())
                << material << ": "
                << (builder.GetErrors().empty() ? std::string{} : builder.GetErrors().front());
            const std::string name =
                fs::path(builder.GetDocument().surfaceShader).filename().string();
            EXPECT_TRUE(cookedNames.contains(name))
                << material << ": the runtime asks for " << name << ", which the cook did not write";
            runtimeNames.insert(name);
        }
    }
    EXPECT_EQ(runtimeNames.size(), 6u);
    EXPECT_EQ(cookedNames, runtimeNames);
}

// The cook refuses a scan alias the runtime would refuse to mount, and a scan
// root whose alias it cannot derive, before cooking anything.
TEST(MaterialVariantCookIntegration, CookRefusesScanAliasesTheRuntimeCannotMount)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_scan_alias_refusals")};
    const fs::path scan = project.Path() / "Loose/Assets";
    fs::create_directories(project.Path() / "Assets");
    fs::create_directories(scan);
    const struct
    {
        std::vector<std::string> Arguments;
        const char* Expected;
    } cases[] = {
        {{"--scan", scan.string(), "--scan-alias", ""}, "is not a valid source alias"},
        {{"--scan", scan.string(), "--scan-alias", "@scope/My Pkg"}, "is not a valid source alias"},
        {{"--scan", scan.string()}, "pass --scan-alias"},
        {{"--particle-row", "nope"}, "--particle-row nope: not a row of the particle request set"},
    };
    for (const auto& refusal : cases)
    {
        const GameEngine::ShellProcessResult result = Cook(project.Path(), false, refusal.Arguments);
        EXPECT_EQ(result.exitCode, 2) << result.output;
        EXPECT_NE(result.output.find(refusal.Expected), std::string::npos) << result.output;
    }
}

// The desktop cook composes for a desktop device with every feature, as that device's runtime does:
// every variant it writes for an opaque relief material shades, marches and takes the march's
// footprint at the pixel centre, so each declares the interpolation functions. A cook composing
// without them writes programs under keys that runtime never asks for, and the runtime then compiles
// every parallax program at first draw.
TEST(MaterialVariantCookIntegration, TheDesktopCookComposesTheReliefForADeviceWithInterpolationFunctions)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_relief_cook")};
    ASSERT_NO_FATAL_FAILURE(WriteReliefMaterial(project.Path()));
    const GameEngine::ShellProcessResult result = Cook(project.Path());
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;

    const auto packages = CookedPackages(result.output, "Relief");
    ASSERT_FALSE(packages.empty()) << "the relief material was not cooked\n" << result.output;
    for (const auto& [variant, path] : packages)
    {
        GameEngine::Rendering::ShaderPackage package{};
        std::string error;
        ASSERT_TRUE(GameEngine::Rendering::LoadShaderPkg(path.string(), GameEngine::Rendering::ShaderSourceKind::SpirV,
                                                         package, &error))
            << variant << ": " << error;
        const auto fragment = package.stageBytes.find("fs");
        ASSERT_NE(fragment, package.stageBytes.end()) << variant << " has no fragment stage";
        EXPECT_TRUE(DeclaresCapability(fragment->second, kCapabilityInterpolationFunction))
            << variant << " marches without the interpolation functions a desktop device composes it with";
    }
}

TEST(MaterialVariantCookIntegration, CommittedGraphSamplesCookValidatedWebPackages)
{
    const char* configuredPython = std::getenv("GE_PYTHON");
    const auto preflight = GameEngine::RunProcessCaptured(
        configuredPython ? configuredPython : "python3",
        {ShaderCookScript().string(), "--self-test"});
    if (preflight.exitCode != 0)
        GTEST_SKIP() << "WGSL toolchain unavailable (glslc/naga/tint): " << preflight.output;
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_web_cook")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    // Each particle shape cooks one row, the keyword-less base program; it composes both
    // stages' particle varyings, which WGSL must accept as stage interfaces. The export
    // cooks the whole request set (Particles::ParticleRenderVariants).
    const auto result = Cook(project.Path(), true, {"--particle-row", "base"});
    ASSERT_NO_FATAL_FAILURE(CheckCook(result));
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    ASSERT_GT(shapes, 0u);
    for (std::size_t shape = 0; shape <= shapes; ++shape)
        EXPECT_EQ(CookedVariantCount(result.output, "particles-" + std::to_string(shape)),
                  shape < shapes ? 1u : 0u)
            << "particles-" << shape << " of " << shapes << " shapes\n" << result.output;
}
