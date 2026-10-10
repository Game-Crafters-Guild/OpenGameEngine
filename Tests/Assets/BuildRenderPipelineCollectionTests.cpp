// The render pipeline a build ships. game.config names it by asset path, the packaged
// Player resolves that path through the exported Assets/.assetmanifest, and a path the
// manifest does not list falls back to the built-in pipeline. The pipeline is usually
// the editor's default, not a project asset, so the collector has to find it where the
// mounts resolve it and ship it as a manifest entry under Assets/.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

constexpr const char* kPipelinePath = "RenderPipelines/ForwardPlus.rendergraph";
constexpr const char* kEmptyPipeline = R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [] })";

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A project asset root and an editor mount, the two places a build finds its pipeline.
// Tests write their files first, then Mount() scans both.
class BuildRenderPipelineCollection : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fs::create_directories(m_ProjectAssets);
        fs::create_directories(m_EditorAssets);
    }

    void TearDown() override { m_AssetManager.Shutdown(); }

    void Mount()
    {
        ASSERT_TRUE(m_AssetManager.Initialize(m_ProjectAssets, nullptr,
                                              m_Root.Path() / "project" / "AssetDatabase.assetdb",
                                              m_Root.Path() / "project" / ".Cache" / "AssetDatabase"));
        AssetSourceDesc editorSource{};
        editorSource.Alias = std::string(kAssetSourceAliasEditor);
        editorSource.Root = m_EditorAssets;
        editorSource.RegisterFileWatcher = false;
        ASSERT_TRUE(m_AssetManager.RegisterSource(editorSource));
        m_AssetManager.WaitForStartupScan(std::string(kAssetSourceAliasEditor));
        m_AssetManager.WaitForStartupScan();
    }

    GUID GuidOf(const fs::path& file) { return m_AssetManager.GetRegistry().GetAssetGUID(file); }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_pipeline_collect")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    const fs::path m_EditorAssets = m_Root.Path() / "editor" / "Assets";
    AssetManager m_AssetManager;
};

} // namespace

TEST_F(BuildRenderPipelineCollection, EditorMountPipelineIsAManifestEntryUnderAssets)
{
    WriteTextFile(m_EditorAssets / kPipelinePath, kEmptyPipeline);
    Mount();
    const GUID pipelineGuid = GuidOf(m_EditorAssets / kPipelinePath);
    ASSERT_FALSE(pipelineGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(pipelineGuid);
    ASSERT_NE(entry, nullptr) << "the configured pipeline is not in the build manifest";
    EXPECT_EQ(entry->outputPath.generic_string(), std::string("Assets/") + kPipelinePath);
    EXPECT_EQ(entry->type, AssetType::RenderPipeline);
}

TEST_F(BuildRenderPipelineCollection, AssetAnEditorMountPipelineReferencesByGuidShips)
{
    WriteTextFile(m_EditorAssets / kPipelinePath, kEmptyPipeline);
    WriteTextFile(m_EditorAssets / "Textures" / "Lut.png", "PNGDATA");
    Mount();
    const GUID lutGuid = GuidOf(m_EditorAssets / "Textures" / "Lut.png");
    ASSERT_FALSE(lutGuid.IsNull());
    WriteTextFile(m_EditorAssets / kPipelinePath,
                  std::string(R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "lut": ")") +
                      lutGuid.ToString() + R"(", "passes": [] })");

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(lutGuid);
    ASSERT_NE(entry, nullptr) << "the texture the pipeline references is not in the build manifest";
    EXPECT_EQ(entry->outputPath.generic_string(), "Assets/Textures/Lut.png");
}

TEST_F(BuildRenderPipelineCollection, TextureAnEditorMountPipelineReferencesByPathShips)
{
    // The ForwardPlus shape: BloomLensDirtComposite names its texture by path.
    WriteTextFile(m_EditorAssets / "Textures" / "Bloom" / "lensDirt1.png", "PNGDATA");
    WriteTextFile(m_EditorAssets / kPipelinePath,
                  R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [ { "id": "BloomLensDirtComposite", )"
                  R"("assetTextures": { "uLensDirt": "Textures/Bloom/lensDirt1.png" } } ] })");
    Mount();
    const GUID dirtGuid = GuidOf(m_EditorAssets / "Textures" / "Bloom" / "lensDirt1.png");
    ASSERT_FALSE(dirtGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(dirtGuid);
    ASSERT_NE(entry, nullptr) << "the texture the pipeline names by path is not in the build manifest";
    EXPECT_EQ(entry->outputPath.generic_string(), "Assets/Textures/Bloom/lensDirt1.png");
}

TEST_F(BuildRenderPipelineCollection, ShippedForwardPlusLensDirtCollectsWithoutUnresolvedReferences)
{
    // The shipped pipeline and its 3 MB lens dirt PNG: the texture is binary, so the
    // walk reads no references out of it. Read as text, its compressed bytes held a
    // span that resolved to the mount root, a reference with no metadata.
    const fs::path stagedAssets = TestPaths::StagedEngineAssetsDir();
    const fs::path dirtPath = fs::path("Textures") / "Bloom" / "lensDirt1.png";
    fs::create_directories((m_EditorAssets / dirtPath).parent_path());
    fs::create_directories((m_EditorAssets / kPipelinePath).parent_path());
    fs::copy_file(stagedAssets / dirtPath, m_EditorAssets / dirtPath);
    fs::copy_file(stagedAssets / kPipelinePath, m_EditorAssets / kPipelinePath);
    Mount();
    const GUID dirtGuid = GuidOf(m_EditorAssets / dirtPath);
    ASSERT_FALSE(dirtGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));

    EXPECT_TRUE(manifest.unresolvedDependencies.empty())
        << manifest.unresolvedDependencies.size() << " reference(s) with no metadata";
    EXPECT_NE(manifest.FindByGuid(dirtGuid), nullptr) << "the lens dirt did not ship";
    EXPECT_EQ(manifest.entries.size(), 2u) << "the pipeline and its lens dirt ship, nothing else";
}

TEST_F(BuildRenderPipelineCollection, ProjectShaderPackagesAPassNamesShip)
{
    // The game shape from #2652: a project pass names its packages through the project
    // source, under "shaderPkg" and under a key the game's own pass type reads. The
    // packaged Player refuses a pipeline whose package is missing and draws nothing.
    WriteTextFile(m_ProjectAssets / "RenderPipelines" / "Shaders" / "mission_fog.shaderpkg", "SHADERPKG");
    WriteTextFile(m_ProjectAssets / "RenderPipelines" / "Shaders" / "mission_minimap.shaderpkg", "SHADERPKG");
    WriteTextFile(m_ProjectAssets / kPipelinePath,
                  R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [ { "id": "MissionFog", )"
                  R"("type": "MissionFog", "shaderPkg": "project:RenderPipelines/Shaders/mission_fog.shaderpkg", )"
                  R"("minimapShaderPkg": "project:RenderPipelines/Shaders/mission_minimap.shaderpkg" } ] })");
    Mount();
    const GUID fogGuid = GuidOf(m_ProjectAssets / "RenderPipelines" / "Shaders" / "mission_fog.shaderpkg");
    const GUID minimapGuid = GuidOf(m_ProjectAssets / "RenderPipelines" / "Shaders" / "mission_minimap.shaderpkg");
    ASSERT_FALSE(fogGuid.IsNull());
    ASSERT_FALSE(minimapGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    const AssetManifestEntry* fog = manifest.FindByGuid(fogGuid);
    ASSERT_NE(fog, nullptr) << "the shader package the pass names is not in the build manifest";
    EXPECT_EQ(fog->outputPath.generic_string(), "Assets/RenderPipelines/Shaders/mission_fog.shaderpkg");
    const AssetManifestEntry* minimap = manifest.FindByGuid(minimapGuid);
    ASSERT_NE(minimap, nullptr) << "the shader package the game's pass type names is not in the build manifest";
    EXPECT_EQ(minimap->outputPath.generic_string(), "Assets/RenderPipelines/Shaders/mission_minimap.shaderpkg");
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());
}

TEST_F(BuildRenderPipelineCollection, SourcePrefixSelectsTheMountThatShips)
{
    // A prefixed name resolves in the source it names only, as the runtime resolves it:
    // "editor:" ships the editor's package even where the project holds the same path,
    // which a plain relative lookup would find first.
    WriteTextFile(m_ProjectAssets / "Shaders" / "fog.shaderpkg", "PROJECT");
    WriteTextFile(m_EditorAssets / "Shaders" / "fog.shaderpkg", "EDITOR");
    WriteTextFile(m_EditorAssets / kPipelinePath,
                  R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [ { "id": "Fog", )"
                  R"("type": "FullscreenShader", "shaderPkg": "editor:Shaders/fog.shaderpkg" } ] })");
    Mount();
    const GUID projectGuid = GuidOf(m_ProjectAssets / "Shaders" / "fog.shaderpkg");
    const GUID editorGuid = GuidOf(m_EditorAssets / "Shaders" / "fog.shaderpkg");
    ASSERT_FALSE(projectGuid.IsNull());
    ASSERT_FALSE(editorGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(editorGuid);
    ASSERT_NE(entry, nullptr) << "the editor package the prefix names did not ship";
    EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasEditor));
    EXPECT_EQ(manifest.FindByGuid(projectGuid), nullptr) << "the project's package shipped for an editor: name";
}

TEST_F(BuildRenderPipelineCollection, UnprefixedEnginePackagesAreLeftToTheEngineShaderCopy)
{
    // Unprefixed names are the engine's own packages: the export copies the staged engine
    // Shaders folder whole, so the walk adds no manifest rows for them.
    WriteTextFile(m_EditorAssets / "Shaders" / "copy.shaderpkg", "ENGINE");
    WriteTextFile(m_EditorAssets / kPipelinePath,
                  R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [ { "id": "FinalCopy", )"
                  R"("type": "FullscreenShader", "shaderPkg": "Shaders/copy.shaderpkg" } ] })");
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    EXPECT_EQ(manifest.entries.size(), 1u) << "only the pipeline itself ships through the manifest";
}

TEST_F(BuildRenderPipelineCollection, UnknownSourcePrefixShipsNothing)
{
    // A prefix that names no mounted source resolves nowhere, as at runtime: the walk
    // does not fall back to another mount's file at the same relative path.
    WriteTextFile(m_ProjectAssets / "Shaders" / "ghost.shaderpkg", "PROJECT");
    WriteTextFile(m_ProjectAssets / kPipelinePath,
                  R"({ "schemaVersion": 2, "pipelineName": "ForwardPlus", "passes": [ { "id": "Ghost", )"
                  R"("type": "FullscreenShader", "shaderPkg": "nosuchpkg:Shaders/ghost.shaderpkg" } ] })");
    Mount();
    const GUID ghostGuid = GuidOf(m_ProjectAssets / "Shaders" / "ghost.shaderpkg");
    ASSERT_FALSE(ghostGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    EXPECT_EQ(manifest.FindByGuid(ghostGuid), nullptr) << "a project file shipped for an unknown source prefix";
    EXPECT_EQ(manifest.entries.size(), 1u) << "only the pipeline itself ships";
}

TEST_F(BuildRenderPipelineCollection, ProjectPipelineShipsFromTheProjectMount)
{
    WriteTextFile(m_ProjectAssets / "RenderPipelines" / "Custom.rendergraph", kEmptyPipeline);
    Mount();
    const GUID pipelineGuid = GuidOf(m_ProjectAssets / "RenderPipelines" / "Custom.rendergraph");
    ASSERT_FALSE(pipelineGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline("RenderPipelines/Custom.rendergraph", manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(pipelineGuid);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->outputPath.generic_string(), "Assets/RenderPipelines/Custom.rendergraph");
    EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasProject));
}

TEST_F(BuildRenderPipelineCollection, ProjectCopyShadowsTheEditorPipeline)
{
    WriteTextFile(m_ProjectAssets / kPipelinePath, kEmptyPipeline);
    WriteTextFile(m_EditorAssets / kPipelinePath, kEmptyPipeline);
    Mount();
    const GUID projectGuid = GuidOf(m_ProjectAssets / kPipelinePath);
    const GUID editorGuid = GuidOf(m_EditorAssets / kPipelinePath);
    ASSERT_FALSE(projectGuid.IsNull());
    ASSERT_FALSE(editorGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectRenderPipeline(kPipelinePath, manifest));
    EXPECT_NE(manifest.FindByGuid(projectGuid), nullptr) << "the project's copy did not ship";
    EXPECT_EQ(manifest.FindByGuid(editorGuid), nullptr) << "the shadowed editor copy shipped";
    EXPECT_EQ(manifest.entries.size(), 1u);
}

TEST_F(BuildRenderPipelineCollection, PipelineNoMountSuppliesIsFalseAndAddsNothing)
{
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    EXPECT_FALSE(collector.CollectRenderPipeline("RenderPipelines/Nowhere.rendergraph", manifest));
    EXPECT_TRUE(manifest.entries.empty());
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());
}

TEST_F(BuildRenderPipelineCollection, NoConfiguredPipelineIsTrueAndAddsNothing)
{
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    EXPECT_TRUE(collector.CollectRenderPipeline("", manifest));
    EXPECT_TRUE(manifest.entries.empty());
}
