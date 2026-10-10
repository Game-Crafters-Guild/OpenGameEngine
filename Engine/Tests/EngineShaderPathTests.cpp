#include <gtest/gtest.h>

#include "../Source/Core/EngineShaderPath.h"
#include "Assets/RenderPipelineAsset.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace
{
using namespace GameEngine;
namespace fs = std::filesystem;
namespace R = Rendering;
namespace P = Engine::Renderer::Pipeline;

AssetManager* s_Assets = nullptr;
fs::path Resolve(const fs::path& path)
{
    return Detail::ResolveEngineShaderPath(*s_Assets, path);
}

// Where the fixture's executable would run from: bin/Apps/Editor under its root.
fs::path s_ExecutableDirectory;
std::string RebuildAction(const std::string& packageName)
{
    return Detail::ShaderPackageRebuildAction(packageName, s_ExecutableDirectory);
}

class EngineShaderPath : public testing::Test
{
  protected:
    void SetUp() override
    {
        Root = fs::temp_directory_path() / ("ge_shader_source_" + GUID::Generate().ToString());
        fs::create_directories(Root / "Project");
        fs::create_directories(Root / "Editor");
        fs::create_directories(Root / "Package");
        ASSERT_TRUE(Assets.Initialize(Root / "Project", nullptr, Root / "registry.assetdb", Root / "Cache"));
        AssetSourceDesc source{};
        source.Alias = "editor";
        source.Root = Root / "Editor";
        ASSERT_TRUE(Assets.RegisterSource(source));
        source.Alias = "nature-pack";
        source.Root = Root / "Package";
        ASSERT_TRUE(Assets.RegisterSource(source));
        s_Assets = &Assets;
        R::Utils::SetShaderPathResolver(&Resolve);
        s_ExecutableDirectory = Root / "bin" / "Apps" / "Editor";
        R::SetShaderPackageRebuildAction(&RebuildAction);
    }
    void TearDown() override
    {
        R::SetShaderPackageRebuildAction(nullptr);
        s_ExecutableDirectory.clear();
        R::Utils::SetShaderPathResolver(nullptr);
        s_Assets = nullptr;
        Assets.Shutdown();
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
    fs::path File(const char* source, const char* name)
    {
        const auto path = Root / source / name;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << "source fixture";
        return path;
    }
    fs::path Package(const char* source, const char* name)
    {
        const auto path = Root / source / name;
        fs::create_directories(path.parent_path());
        R::ShaderMeta meta;
        meta.Version = 1;
        const std::unordered_map<std::string, std::vector<uint8_t>> stages{
            {"vs", {1, 2, 3, 4}}, {"fs", {5, 6, 7, 8}}};
        std::string error;
        EXPECT_TRUE(R::SaveShaderPkg(path, meta, stages, std::nullopt, &error)) << error;
        return path;
    }
    // A package this build refuses: a current package with its format version
    // word set to 1, as a package written by an older build would carry.
    fs::path VersionOnePackage(const char* source, const char* name)
    {
        const auto path = Package(source, name);
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t version = 1;
        file.seekp(8);
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        EXPECT_TRUE(file.good()) << path;
        return path;
    }
    P::RenderPipelineBlueprint Compile(const char* path)
    {
        const auto json = nlohmann::json{
            {"schemaVersion", 2},
            {"passes", nlohmann::json::array({{
                {"id", "Custom"}, {"type", "FullscreenShader"},
                {"output", "View.Resolve"}, {"shaderPkg", path}}})}}.dump();
        RenderPipelineAsset asset(GUID::Null(), "fixture.rendergraph");
        asset.LoadFromData(Vector<uint8>(json.begin(), json.end()));
        P::RenderPipelineNodeRegistry registry;
        registry.Register("FullscreenShader", [] { return std::unique_ptr<P::IRenderPipelineNode>{}; }, true);
        return P::RenderPipelineCompiler{}.Compile(asset, registry);
    }
    AssetManager Assets;
    fs::path Root;
};

TEST_F(EngineShaderPath, AuthoredProjectShaderLoadsThroughProductionResolverAndCompiler)
{
    const auto expected = Package("Project", "RenderPipelines/Shaders/custom.shaderpkg");
    EXPECT_EQ(Resolve("project:RenderPipelines/Shaders/custom.shaderpkg"), expected);
    R::ShaderPackage package;
    std::string error;
    EXPECT_TRUE(R::LoadShaderPkg("project:RenderPipelines/Shaders/custom.shaderpkg",
                                R::ShaderSourceKind::SpirV, package, &error)) << error;
    const auto result = Compile("project:RenderPipelines/Shaders/custom.shaderpkg");
    for (const auto& issue : result.issues)
        EXPECT_NE(issue.severity, P::PipelineIssueSeverity::Error) << issue.message;
    EXPECT_FALSE(result.HasErrors());
}

TEST_F(EngineShaderPath, VersionOneProjectPackageIsRefusedWithoutRewriting)
{
    const auto packagePath = VersionOnePackage("Project", "RenderPipelines/Shaders/custom.shaderpkg");
    const auto originalBytes = R::Utils::ReadFile(packagePath.string());
    ASSERT_FALSE(originalBytes.empty());
    const auto originalWriteTime = fs::last_write_time(packagePath);
    constexpr const char* projectPath = "project:RenderPipelines/Shaders/custom.shaderpkg";
    EXPECT_EQ(Resolve(projectPath), packagePath);

    R::ShaderPackage package;
    std::string error;
    EXPECT_FALSE(R::LoadShaderPkg(projectPath, R::ShaderSourceKind::SpirV, package, &error));
    EXPECT_NE(error.find("version 1"), std::string::npos) << error;
    EXPECT_NE(error.find("this build reads version " + std::to_string(R::kShaderPackageVersion)), std::string::npos)
        << error;
    EXPECT_NE(error.find("ShaderReflect"), std::string::npos) << error;
    EXPECT_NE(error.find("--out \"" + packagePath.generic_string() + "\""), std::string::npos) << error;
    EXPECT_EQ(error.find("CompileShaderPkgs"), std::string::npos) << error;

    const auto result = Compile(projectPath);
    ASSERT_TRUE(result.HasErrors());
    bool foundPackageRefusal = false;
    for (const auto& issue : result.issues)
    {
        if (issue.severity == P::PipelineIssueSeverity::Error &&
            issue.message.find(projectPath) != std::string::npos &&
            issue.message.find("version 1") != std::string::npos &&
            issue.message.find("ShaderReflect") != std::string::npos)
            foundPackageRefusal = true;
        EXPECT_EQ(issue.message.find("CompileShaderPkgs"), std::string::npos) << issue.message;
        EXPECT_NE(issue.message.find(packagePath.generic_string()), std::string::npos) << issue.message;
    }
    EXPECT_TRUE(foundPackageRefusal);
    EXPECT_EQ(R::Utils::ReadFile(packagePath.string()), originalBytes);
    EXPECT_EQ(fs::last_write_time(packagePath), originalWriteTime);
}

TEST_F(EngineShaderPath, VersionOneEnginePackagesNameTheEngineBuildTarget)
{
    const auto packagePath = VersionOnePackage("Editor", "Shaders/encode_srgb.shaderpkg");
    const auto originalBytes = R::Utils::ReadFile(packagePath.string());
    const auto originalWriteTime = fs::last_write_time(packagePath);
    for (const char* packageName : {"Shaders/encode_srgb.shaderpkg", "EDITOR:/Assets/Shaders/encode_srgb.shaderpkg"})
    {
        const auto result = Compile(packageName);
        ASSERT_TRUE(result.HasErrors());
        ASSERT_EQ(result.issues.size(), 1u);
        const auto& message = result.issues.front().message;
        EXPECT_NE(message.find("version 1"), std::string::npos) << message;
        EXPECT_NE(message.find("CompileShaderPkgs"), std::string::npos) << message;
        EXPECT_NE(message.find(packagePath.generic_string()), std::string::npos) << message;
        EXPECT_EQ(message.find("ShaderReflect"), std::string::npos) << message;
    }
    EXPECT_EQ(R::Utils::ReadFile(packagePath.string()), originalBytes);
    EXPECT_EQ(fs::last_write_time(packagePath), originalWriteTime);
}

TEST_F(EngineShaderPath, DirectLoadersNameTheEngineBuildTargetForAVersionOneEnginePackage)
{
    const auto packagePath = VersionOnePackage("Editor", "Shaders/encode_srgb.shaderpkg");
    const std::string rebuild = "CompileShaderPkgs to replace \"" + packagePath.generic_string() + "\"";

    R::ShaderPackage package;
    std::string loadError;
    EXPECT_FALSE(R::LoadShaderPkg("Shaders/encode_srgb.shaderpkg", R::ShaderSourceKind::SpirV, package, &loadError));
    EXPECT_NE(loadError.find("version 1"), std::string::npos) << loadError;
    EXPECT_NE(loadError.find(rebuild), std::string::npos) << loadError;

    std::string computeError;
    EXPECT_TRUE(R::LoadComputeStageBytes("Shaders/encode_srgb.shaderpkg", R::ShaderSourceKind::SpirV, nullptr,
                                         &computeError)
                    .empty());
    EXPECT_NE(computeError.find("version 1"), std::string::npos) << computeError;
    EXPECT_NE(computeError.find(rebuild), std::string::npos) << computeError;
}

TEST_F(EngineShaderPath, WithoutARebuildActionTheLoadersReportOnlyTheMismatch)
{
    VersionOnePackage("Editor", "Shaders/encode_srgb.shaderpkg");
    R::SetShaderPackageRebuildAction(nullptr);
    const std::string mismatch =
        "Unsupported shaderpkg version 1; this build reads version " + std::to_string(R::kShaderPackageVersion) + ".";

    R::ShaderPackage package;
    std::string loadError;
    EXPECT_FALSE(R::LoadShaderPkg("Shaders/encode_srgb.shaderpkg", R::ShaderSourceKind::SpirV, package, &loadError));
    EXPECT_EQ(loadError, mismatch);

    std::string computeError;
    EXPECT_TRUE(R::LoadComputeStageBytes("Shaders/encode_srgb.shaderpkg", R::ShaderSourceKind::SpirV, nullptr,
                                         &computeError)
                    .empty());
    EXPECT_EQ(computeError, "parse failed: " + mismatch);
}

TEST_F(EngineShaderPath, ShaderReflectIsNamedWhereTheBuildStagedIt)
{
    VersionOnePackage("Project", "RenderPipelines/Shaders/custom.shaderpkg");
    constexpr const char* projectPath = "project:RenderPipelines/Shaders/custom.shaderpkg";
    R::ShaderPackage package;

    // A packaged game ships no tools: the action names the tool, not a location.
    std::string unstaged;
    EXPECT_FALSE(R::LoadShaderPkg(projectPath, R::ShaderSourceKind::SpirV, package, &unstaged));
    EXPECT_NE(unstaged.find("with this SDK's ShaderReflect (--vs/--fs/--cs"), std::string::npos) << unstaged;

#if defined(_WIN32)
    const auto tool = File("bin", "Tools/ShaderReflect.exe");
#else
    const auto tool = File("bin", "Tools/ShaderReflect");
#endif
    std::string staged;
    EXPECT_FALSE(R::LoadShaderPkg(projectPath, R::ShaderSourceKind::SpirV, package, &staged));
    EXPECT_NE(staged.find("with this SDK's ShaderReflect \"" + tool.generic_string() + "\" (--vs/--fs/--cs"),
              std::string::npos)
        << staged;
}

TEST_F(EngineShaderPath, MalformedPackageGetsNoRebuildAction)
{
    // Damaged, not of another version: rebuilding would not be the fix.
    const auto packagePath = Package("Editor", "Shaders/encode_srgb.shaderpkg");
    {
        std::fstream file(packagePath, std::ios::in | std::ios::out | std::ios::binary);
        const uint32_t chunkCount = 0;
        file.seekp(12);
        file.write(reinterpret_cast<const char*>(&chunkCount), sizeof(chunkCount));
        ASSERT_TRUE(file.good());
    }
    R::ShaderPackage package;
    std::string error;
    EXPECT_FALSE(R::LoadShaderPkg("Shaders/encode_srgb.shaderpkg", R::ShaderSourceKind::SpirV, package, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.find("Rebuild"), std::string::npos) << error;
}

TEST_F(EngineShaderPath, VersionOneSourceAliasNamesShaderReflectAndTheResolvedOutput)
{
    const auto packagePath = VersionOnePackage("Package", "Shaders/custom.shaderpkg");
    const auto originalBytes = R::Utils::ReadFile(packagePath.string());
    const auto originalWriteTime = fs::last_write_time(packagePath);
    const auto result = Compile("NATURE-PACK:/Assets/Shaders/custom.shaderpkg");
    ASSERT_TRUE(result.HasErrors());
    ASSERT_EQ(result.issues.size(), 1u);
    const auto& message = result.issues.front().message;
    EXPECT_NE(message.find("ShaderReflect"), std::string::npos) << message;
    EXPECT_NE(message.find("--out \"" + packagePath.generic_string() + "\""), std::string::npos) << message;
    EXPECT_EQ(message.find("CompileShaderPkgs"), std::string::npos) << message;
    EXPECT_EQ(R::Utils::ReadFile(packagePath.string()), originalBytes);
    EXPECT_EQ(fs::last_write_time(packagePath), originalWriteTime);
}

TEST_F(EngineShaderPath, NamedRootedAndNormalizedPrefixesSelectOnlyTheirSource)
{
    const auto expected = File("Package", "Shaders/custom.shaderpkg");
    File("Editor", "Shaders/custom.shaderpkg");
    File("Project", "Shaders/custom.shaderpkg");
    EXPECT_EQ(Resolve("nature-pack:Shaders/custom.shaderpkg"), expected);
    EXPECT_EQ(Resolve("NATURE-PACK:/Assets/Shaders/custom.shaderpkg"), expected);
}

TEST_F(EngineShaderPath, UnprefixedShaderStaysPinnedToEditor)
{
    const auto expected = File("Editor", "Shaders/engine.shaderpkg");
    File("Project", "Shaders/engine.shaderpkg");
    EXPECT_EQ(Resolve("Shaders/engine.shaderpkg"), expected);
    EXPECT_EQ(Resolve("editor:Shaders/engine.shaderpkg"), expected);
}

TEST_F(EngineShaderPath, MissingEditorShaderNeverFallsBackToProject)
{
    Package("Project", "Shaders/missing_engine.shaderpkg");
    EXPECT_TRUE(Resolve("Shaders/missing_engine.shaderpkg").empty());
    EXPECT_TRUE(Compile("Shaders/missing_engine.shaderpkg").HasErrors());
    // The absence remains strict on repeated shader retry frames.
    EXPECT_TRUE(Resolve("Shaders/missing_engine.shaderpkg").empty());
}

TEST_F(EngineShaderPath, MissingOrUnknownAuthoredSourceNeverFallsBackToEditor)
{
    Package("Editor", "Shaders/editor_only.shaderpkg");
    EXPECT_TRUE(Resolve("project:Shaders/editor_only.shaderpkg").empty());
    EXPECT_TRUE(Compile("project:Shaders/editor_only.shaderpkg").HasErrors());
    EXPECT_TRUE(Resolve("unknown:Shaders/editor_only.shaderpkg").empty());
    EXPECT_TRUE(Compile("unknown:Shaders/editor_only.shaderpkg").HasErrors());
}

TEST_F(EngineShaderPath, EditorlessAndAbsoluteResolutionKeepTheirExistingBehavior)
{
    const auto project = File("Project", "Shaders/shared.shaderpkg");
    const auto absolute = File("Package", "Shaders/absolute.shaderpkg");
    EXPECT_EQ(Resolve(absolute), absolute);
    Assets.Shutdown();
    ASSERT_TRUE(Assets.Initialize(Root / "Project", nullptr, Root / "registry.assetdb", Root / "Cache"));
    ASSERT_TRUE(Assets.GetSourceRoot("editor").empty());
    EXPECT_EQ(Resolve("Shaders/shared.shaderpkg"), project);
    EXPECT_EQ(Resolve("project:Shaders/shared.shaderpkg"), project);
    EXPECT_TRUE(Resolve("unknown:Shaders/shared.shaderpkg").empty());
}

TEST_F(EngineShaderPath, ExplicitAssetSourceArgumentRemainsAuthoritative)
{
    const auto expected = File("Editor", "Shaders/shared.shaderpkg");
    File("Project", "Shaders/shared.shaderpkg");
    EXPECT_EQ(Assets.ResolveAssetPath("Shaders/shared.shaderpkg", "editor"), expected);
    EXPECT_NE(Assets.ResolveAssetPath("project:Shaders/shared.shaderpkg", "editor"),
              Root / "Project/Shaders/shared.shaderpkg");
}
} // namespace
