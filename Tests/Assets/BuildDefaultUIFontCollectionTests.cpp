// The default game UI font a build ships. The packaged Player reads it by path from its
// asset root (Assets/Fonts/Roboto-Regular.ttf) with no authored dependency edge, and the
// font is the editor's unless the project supplies its own copy, so the collector has to
// find it where the mounts resolve it and ship it as a manifest entry under Assets/.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

constexpr const char* kFontPath = "Fonts/Roboto-Regular.ttf";

void WriteFontFile(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "TTFDATA";
}

// A project asset root and an editor mount, the two places a build finds the font.
// Tests write their files first, then Mount() scans both.
class BuildDefaultUIFontCollection : public ::testing::Test
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

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_default_font_collect")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    const fs::path m_EditorAssets = m_Root.Path() / "editor" / "Assets";
    AssetManager m_AssetManager;
};

} // namespace

TEST_F(BuildDefaultUIFontCollection, EditorMountFontIsAManifestEntryUnderAssets)
{
    WriteFontFile(m_EditorAssets / kFontPath);
    Mount();
    const GUID fontGuid = GuidOf(m_EditorAssets / kFontPath);
    ASSERT_FALSE(fontGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectDefaultUIFont(manifest));
    const AssetManifestEntry* entry = manifest.FindByGuid(fontGuid);
    ASSERT_NE(entry, nullptr) << "the default UI font is not in the build manifest";
    EXPECT_EQ(entry->outputPath.generic_string(), std::string("Assets/") + kFontPath);
    EXPECT_EQ(entry->type, AssetType::Font);
    EXPECT_EQ(entry->sourceAlias, std::string(kAssetSourceAliasEditor));
}

TEST_F(BuildDefaultUIFontCollection, AlreadyCollectedFontIsTrueAndNotDuplicated)
{
    WriteFontFile(m_EditorAssets / kFontPath);
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectDefaultUIFont(manifest));
    ASSERT_TRUE(collector.CollectDefaultUIFont(manifest));
    EXPECT_EQ(manifest.entries.size(), 1u);
}

TEST_F(BuildDefaultUIFontCollection, ProjectCopyShadowsTheEditorFont)
{
    WriteFontFile(m_ProjectAssets / kFontPath);
    WriteFontFile(m_EditorAssets / kFontPath);
    Mount();
    const GUID projectGuid = GuidOf(m_ProjectAssets / kFontPath);
    const GUID editorGuid = GuidOf(m_EditorAssets / kFontPath);
    ASSERT_FALSE(projectGuid.IsNull());
    ASSERT_FALSE(editorGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    ASSERT_TRUE(collector.CollectDefaultUIFont(manifest));
    EXPECT_NE(manifest.FindByGuid(projectGuid), nullptr) << "the project's copy did not ship";
    EXPECT_EQ(manifest.FindByGuid(editorGuid), nullptr) << "the shadowed editor copy shipped";
    EXPECT_EQ(manifest.entries.size(), 1u);
}

TEST_F(BuildDefaultUIFontCollection, FontNoMountSuppliesIsFalseAndAddsNothing)
{
    Mount();

    AssetCollector collector(m_AssetManager);
    AssetManifest manifest;
    EXPECT_FALSE(collector.CollectDefaultUIFont(manifest));
    EXPECT_TRUE(manifest.entries.empty());
}
