#include <gtest/gtest.h>

#include "AssetCore/FoldedPathIndex.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

// A package tree whose file names carry the author's case, mirroring the shape
// Packages/eztree publishes: nested directories, mixed-case names, and a
// mixed-case extension-bearing leaf.
class FoldedPathIndexTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_folded_path_index");
        fs::create_directories(m_Root / "Editor" / "UI" / "panels");
        fs::create_directories(m_Root / "Textures" / "EZTree" / "bark");
        Touch("Editor/UI/EZTreeEditorChrome.css");
        Touch("Editor/UI/panels/EZTreeStatsPanel.uxml");
        Touch("Textures/EZTree/bark/oak_color_1k.jpg");
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    void Touch(const std::string& relative) const
    {
        std::ofstream out(m_Root / fs::path(relative));
        out << "x";
    }

    fs::path m_Root;
};

TEST_F(FoldedPathIndexTest, FoldedManifestPathMapsToRealOnDiskCase)
{
    const auto index = GameEngine::AssetPaths::BuildFoldedPathIndex(m_Root);

    // The spelling a macOS/Windows-published manifest records for this file.
    const auto it = index.find("editor/ui/eztreeeditorchrome.css");
    ASSERT_NE(it, index.end())
        << "folded manifest path did not map to any file under the package root";
    EXPECT_EQ(it->second.filename().string(), "EZTreeEditorChrome.css");
    EXPECT_TRUE(fs::exists(it->second));
}

TEST_F(FoldedPathIndexTest, IndexesNestedFilesAndKeysOnGenericSeparators)
{
    const auto index = GameEngine::AssetPaths::BuildFoldedPathIndex(m_Root);

    EXPECT_EQ(index.size(), 3u);
    // Forward slashes regardless of host separator: manifest paths are generic.
    EXPECT_NE(index.find("editor/ui/panels/eztreestatspanel.uxml"), index.end());
    EXPECT_NE(index.find("textures/eztree/bark/oak_color_1k.jpg"), index.end());
}

TEST_F(FoldedPathIndexTest, PathAbsentFromTheTreeDoesNotResolve)
{
    const auto index = GameEngine::AssetPaths::BuildFoldedPathIndex(m_Root);

    // A record naming a vanished file must stay unresolved — the fallback
    // repairs case, it does not invent a file.
    EXPECT_EQ(index.find("editor/ui/eztreedeleted.css"), index.end());
}

TEST(FoldedPathIndexRootTest, UnreadableRootYieldsAnEmptyIndexRatherThanThrowing)
{
    const fs::path missing =
        GameEngine::TestUtils::MakeUniqueTempDirectory("ge_folded_path_index_absent");
    ASSERT_FALSE(fs::exists(missing));

    EXPECT_TRUE(GameEngine::AssetPaths::BuildFoldedPathIndex(missing).empty());
}

} // namespace
