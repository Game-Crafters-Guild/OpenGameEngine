#include "LinuxDialogPaths.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using GameEngine::Platform::SplitLinuxDialogPaths;

TEST(SplitLinuxDialogPaths, PipeInsideNameStaysOnePathWhenSeparatorIsNewline)
{
    const std::string output = "/tmp/foo|bar.txt\n/tmp/baz.txt";
    const std::vector<std::filesystem::path> paths = SplitLinuxDialogPaths(output, '\n');
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], std::filesystem::path("/tmp/foo|bar.txt"));
    EXPECT_EQ(paths[1], std::filesystem::path("/tmp/baz.txt"));
}

TEST(SplitLinuxDialogPaths, SinglePathWithPipeIsUnchangedWhenSeparatorIsNewline)
{
    const std::vector<std::filesystem::path> paths =
        SplitLinuxDialogPaths("/tmp/foo|bar.txt", '\n');
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], std::filesystem::path("/tmp/foo|bar.txt"));
}

TEST(SplitLinuxDialogPaths, PipeSeparatorSplitsPipeInsideName)
{
    const std::vector<std::filesystem::path> paths =
        SplitLinuxDialogPaths("/tmp/foo|bar.txt", '|');
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], std::filesystem::path("/tmp/foo"));
    EXPECT_EQ(paths[1], std::filesystem::path("bar.txt"));
}

TEST(SplitLinuxDialogPaths, EmptyOutputYieldsNoPaths)
{
    EXPECT_TRUE(SplitLinuxDialogPaths({}, '\n').empty());
}
