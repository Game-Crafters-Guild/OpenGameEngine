#include <gtest/gtest.h>

#include "Assets/AssetDeletePathGuard.h"

#include "../TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <system_error>

using GameEngine::IsAssetPathStrictlyUnder;
using GameEngine::TestUtils::MakeUniqueTempDirectory;

namespace
{

std::filesystem::path NormalizePath(const std::filesystem::path& p)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : canonical;
}

} // namespace

TEST(AssetDeletePathGuard, FileInsideAssetsRootIsStrictlyUnder)
{
    EXPECT_TRUE(IsAssetPathStrictlyUnder(std::filesystem::path("/proj/Assets/Mesh.fbx"),
                                         std::filesystem::path("/proj/Assets")));
}

TEST(AssetDeletePathGuard, AssetsRootItselfIsNotStrictlyUnder)
{
    const std::filesystem::path root{"/proj/Assets"};
    EXPECT_FALSE(IsAssetPathStrictlyUnder(root, root));
}

TEST(AssetDeletePathGuard, SiblingOfAssetsRootIsNotStrictlyUnder)
{
    EXPECT_FALSE(IsAssetPathStrictlyUnder(std::filesystem::path("/proj/Documents/secret.txt"),
                                          std::filesystem::path("/proj/Assets")));
}

TEST(AssetDeletePathGuard, LexicalWalkOutIsNotStrictlyUnder)
{
    EXPECT_FALSE(IsAssetPathStrictlyUnder(std::filesystem::path("/proj/Assets/../Documents/secret.txt"),
                                          std::filesystem::path("/proj/Assets")));
}

TEST(AssetDeletePathGuard, RefusesSymlinkWhoseTargetEscapesAssetsRoot)
{
    const std::filesystem::path root = MakeUniqueTempDirectory("ge_asset_delete_guard");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const std::filesystem::path assetsRoot = root / "Assets";
    const std::filesystem::path outsideDir = root / "Documents";
    const std::filesystem::path outsideFile = outsideDir / "secret.txt";
    const std::filesystem::path link = assetsRoot / "escape";
    std::filesystem::create_directories(assetsRoot, ec);
    std::filesystem::create_directories(outsideDir, ec);
    {
        std::ofstream out(outsideFile);
        ASSERT_TRUE(out.is_open());
        out << "secret";
    }

    std::filesystem::create_directory_symlink(outsideDir, link, ec);
    if (ec)
    {
        std::filesystem::remove_all(root, ec);
        GTEST_SKIP() << "directory symlink not supported: " << ec.message();
    }

    const std::filesystem::path use = NormalizePath(link);
    const std::filesystem::path normalizedRoot = NormalizePath(assetsRoot);
    EXPECT_FALSE(IsAssetPathStrictlyUnder(use, normalizedRoot))
        << "canonical path " << use.string() << " vs assets root " << normalizedRoot.string();

    const std::filesystem::path inside = assetsRoot / "ok.txt";
    {
        std::ofstream out(inside);
        ASSERT_TRUE(out.is_open());
        out << "ok";
    }
    EXPECT_TRUE(IsAssetPathStrictlyUnder(NormalizePath(inside), normalizedRoot));

    std::filesystem::remove_all(root, ec);
}
