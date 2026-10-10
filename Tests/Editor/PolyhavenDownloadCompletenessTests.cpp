#include "Assets/PolyhavenService.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace GameEngine
{
namespace
{

class PolyhavenDownloadCompletenessTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("gameengine-polyhaven-download-completeness-tests");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    static void WriteFile(const std::filesystem::path& path, std::string_view contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(out.good());
    }

    std::filesystem::path m_Root;
};

TEST_F(PolyhavenDownloadCompletenessTests, GltfIsHiddenUntilEveryDependencyExists)
{
    constexpr std::string_view slug = "root_cluster_02";
    const auto assetDir = m_Root / "Polyhaven" / slug;
    const auto gltf = assetDir / "root_cluster_02.gltf";
    WriteFile(gltf, R"({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "root_cluster_02.bin"}],
        "images": [{"uri": "textures/root_cluster_02_diff_1k.jpg"}]
    })");

    EXPECT_TRUE(PolyhavenService::FindDownloadedFile(std::string(slug), m_Root).empty());

    WriteFile(assetDir / "root_cluster_02.bin", "geometry");
    EXPECT_TRUE(PolyhavenService::FindDownloadedFile(std::string(slug), m_Root).empty());

    WriteFile(assetDir / "textures/root_cluster_02_diff_1k.jpg", "pixels");
    EXPECT_EQ(PolyhavenService::FindDownloadedFile(std::string(slug), m_Root), gltf);
}

TEST_F(PolyhavenDownloadCompletenessTests, PartialGltfIsNotMovedIntoProject)
{
    const auto cacheDir = m_Root / "cache" / "jacaranda_tree";
    const auto projectDir = m_Root / "project" / "jacaranda_tree";
    const auto gltf = cacheDir / "jacaranda_tree.gltf";
    WriteFile(gltf, R"({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "jacaranda_tree.bin"}]
    })");

    EXPECT_TRUE(PolyhavenService::MoveDownloadToProject(gltf, cacheDir, projectDir).empty());
    EXPECT_TRUE(std::filesystem::exists(gltf));
    EXPECT_FALSE(std::filesystem::exists(projectDir / "jacaranda_tree.gltf"));
}

} // namespace
} // namespace GameEngine
