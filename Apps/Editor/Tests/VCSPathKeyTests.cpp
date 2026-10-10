#include "VCSIntegration/VCSPathKey.h"
#include <gtest/gtest.h>

using GameEngine::VCS::RepoRelative;
namespace fs = std::filesystem;

TEST(VCSPathKey, EmptyInputs)
{
    EXPECT_TRUE(RepoRelative({}, {}).empty());
    EXPECT_TRUE(RepoRelative("/repo/a", {}).empty());
    EXPECT_TRUE(RepoRelative({}, "/repo").empty());
}

TEST(VCSPathKey, NonexistentChildNeedsNoFilesystemResolution)
{
    EXPECT_EQ(RepoRelative("/nonexistent-vcs-test/Assets/Mesh.fbx", "/nonexistent-vcs-test").generic_string(),
              "Assets/Mesh.fbx");
}

TEST(VCSPathKey, PreservesFilenameCaseAndSpaces)
{
    EXPECT_EQ(RepoRelative("/repo/Assets/My Model.FBX", "/repo").generic_string(), "Assets/My Model.FBX");
}

TEST(VCSPathKey, NormalizesDotSegments)
{
    EXPECT_EQ(RepoRelative("/repo/Assets/../Materials/./a.material", "/repo").generic_string(),
              "Materials/a.material");
}

TEST(VCSPathKey, RootItselfIsDot)
{
    EXPECT_EQ(RepoRelative("/repo", "/repo").generic_string(), ".");
}

TEST(VCSPathKey, TrailingRootSeparator)
{
    EXPECT_EQ(RepoRelative("/repo/Assets/a.png", "/repo/").generic_string(), "Assets/a.png");
}

TEST(VCSPathKey, OutsideRootMatchesPreviousFallback)
{
    const fs::path root = fs::temp_directory_path() / "vcs-path-key-nonexistent-repo";
    const fs::path file = fs::temp_directory_path() / "vcs-path-key-other" / "a.txt";
    std::error_code ec;
    const auto expected = fs::relative(file, root, ec);
    EXPECT_EQ(RepoRelative(file, root), ec ? fs::path{} : expected);
}

TEST(VCSPathKey, RelativeInputs)
{
    EXPECT_EQ(RepoRelative("repo/Assets/a.png", "repo").generic_string(), "Assets/a.png");
}
