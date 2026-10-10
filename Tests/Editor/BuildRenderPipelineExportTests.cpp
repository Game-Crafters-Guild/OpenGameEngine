// The render pipelines an export ships. The export resolves one pipeline from the build
// settings (ResolveBuildGlobalRenderPipeline: the assigned one, else the pipeline the editor
// renders the project with, else ForwardPlus) and the collector ships it and no other: the
// engine pipelines the build tree stages and the project pipelines nothing assigns stay out.
//
// Its own process: the resolver reads the build settings of EngineCore's workspace root, so
// each test points that process-wide root at a temporary project.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Editor/Settings/BuildRenderPipelineSettings.h"
#include "Engine/Build/AssetCollector.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

constexpr const char* kEmptyPipeline = R"({ "schemaVersion": 2, "pipelineName": "Pipeline", "passes": [] })";

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::vector<std::string> RenderGraphOutputPaths(const AssetManifest& manifest)
{
    std::vector<std::string> paths;
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (entry.outputPath.extension() == ".rendergraph")
            paths.push_back(entry.outputPath.generic_string());
    }
    return paths;
}

// A project with no build settings, its startup scene and two pipelines, and an editor mount
// that holds the engine's three pipelines.
class BuildRenderPipelineExport : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        for (const char* engineName : {"ForwardPlus", "ForwardPlus_DebugOverlay", "Sample_PostStack"})
            WriteTextFile(m_EditorAssets / "RenderPipelines" / (std::string(engineName) + ".rendergraph"),
                          kEmptyPipeline);
        WriteTextFile(m_ProjectAssets / "RenderPipelines" / "Custom.rendergraph", kEmptyPipeline);
        WriteTextFile(m_ProjectAssets / "RenderPipelines" / "Unused.rendergraph", kEmptyPipeline);
        WriteTextFile(m_ProjectAssets / "Scenes" / "Main.scene",
                      "[scene name=\"Main\"]\n\n[entity id=\"e1\"]\nTransform.position = 0,0,0\n");

        EngineCore::GetInstance().SetWorkspaceRoot(m_ProjectRoot);

        ASSERT_TRUE(m_AssetManager.Initialize(m_ProjectAssets, nullptr, m_ProjectRoot / "AssetDatabase.assetdb",
                                              m_ProjectRoot / ".Cache" / "AssetDatabase"));
        AssetSourceDesc editorSource{};
        editorSource.Alias = std::string(kAssetSourceAliasEditor);
        editorSource.Root = m_EditorAssets;
        editorSource.RegisterFileWatcher = false;
        ASSERT_TRUE(m_AssetManager.RegisterSource(editorSource));
        m_AssetManager.WaitForStartupScan(std::string(kAssetSourceAliasEditor));
        m_AssetManager.WaitForStartupScan();
    }

    void TearDown() override { m_AssetManager.Shutdown(); }

    // BuildPipeline::CollectAssets' order: the startup scene's walk, then the pipeline the
    // export resolved from the build settings.
    AssetManifest CollectExport()
    {
        AssetCollector collector(m_AssetManager);
        AssetManifest manifest = collector.CollectFromScenes({"Scenes/Main.scene"});
        EXPECT_FALSE(manifest.entries.empty()) << "the startup scene did not ship";
        EXPECT_TRUE(collector.CollectRenderPipeline(ResolveBuildGlobalRenderPipeline(), manifest));
        return manifest;
    }

    GUID GuidOf(const fs::path& file) { return m_AssetManager.GetRegistry().GetAssetGUID(file); }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_pipeline_export")};
    const fs::path m_ProjectRoot = m_Root.Path() / "project";
    const fs::path m_ProjectAssets = m_ProjectRoot / "Assets";
    const fs::path m_EditorAssets = m_Root.Path() / "editor" / "Assets";
    AssetManager m_AssetManager;
};

} // namespace

TEST_F(BuildRenderPipelineExport, ACustomPipelineAssignedShipsNoOtherRenderGraph)
{
    SaveBuildGlobalRenderPipeline("RenderPipelines/Custom.rendergraph");

    const AssetManifest manifest = CollectExport();
    EXPECT_EQ(RenderGraphOutputPaths(manifest),
              std::vector<std::string>{"Assets/RenderPipelines/Custom.rendergraph"});
    const AssetManifestEntry* entry =
        manifest.FindByGuid(GuidOf(m_ProjectAssets / "RenderPipelines" / "Custom.rendergraph"));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasProject));
}

TEST_F(BuildRenderPipelineExport, NoPipelineAssignedShipsOnlyForwardPlus)
{
    const AssetManifest manifest = CollectExport();
    EXPECT_EQ(RenderGraphOutputPaths(manifest),
              std::vector<std::string>{"Assets/RenderPipelines/ForwardPlus.rendergraph"});
    const AssetManifestEntry* entry =
        manifest.FindByGuid(GuidOf(m_EditorAssets / "RenderPipelines" / "ForwardPlus.rendergraph"));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasEditor));
}
