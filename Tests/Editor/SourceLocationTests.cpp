#include <gtest/gtest.h>

#include "ScriptEditor/SourceLocation.h"

#include <filesystem>
#include <optional>

using GameEngine::Editor::FindSourceLocationInLogLine;
using GameEngine::Editor::SourceLocation;

TEST(SourceLocationTests, ColonFormCarriesLineAndColumn)
{
    const std::optional<SourceLocation> location =
        FindSourceLocationInLogLine("error CS1002: /proj/Assets/Player.cs:12:5 ; expected");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Path, std::filesystem::path("/proj/Assets/Player.cs"));
    EXPECT_EQ(location->Line, 12u);
    EXPECT_EQ(location->Column, 5u);
}

TEST(SourceLocationTests, ColonFormWithLineOnly)
{
    const std::optional<SourceLocation> location = FindSourceLocationInLogLine("Assets/Player.cs:42");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Path, std::filesystem::path("Assets/Player.cs"));
    EXPECT_EQ(location->Line, 42u);
    EXPECT_EQ(location->Column, 0u);
}

TEST(SourceLocationTests, ParenthesisFormCarriesLineAndColumn)
{
    const std::optional<SourceLocation> location =
        FindSourceLocationInLogLine("Assets/Enemy.cs(7, 19): error CS0103: name does not exist");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Path, std::filesystem::path("Assets/Enemy.cs"));
    EXPECT_EQ(location->Line, 7u);
    EXPECT_EQ(location->Column, 19u);
}

TEST(SourceLocationTests, ParenthesisFormWithLineOnly)
{
    const std::optional<SourceLocation> location = FindSourceLocationInLogLine("Assets/Enemy.cs(7)");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Line, 7u);
    EXPECT_EQ(location->Column, 0u);
}

TEST(SourceLocationTests, PathStopsAtQuotesAndBrackets)
{
    const std::optional<SourceLocation> quoted = FindSourceLocationInLogLine("failed in \"Assets/Door.cs\"");
    ASSERT_TRUE(quoted.has_value());
    EXPECT_EQ(quoted->Path, std::filesystem::path("Assets/Door.cs"));
    EXPECT_EQ(quoted->Line, 0u);

    const std::optional<SourceLocation> bracketed = FindSourceLocationInLogLine("[Assets/Door.cs:3:1]");
    ASSERT_TRUE(bracketed.has_value());
    EXPECT_EQ(bracketed->Path, std::filesystem::path("Assets/Door.cs"));
    EXPECT_EQ(bracketed->Line, 3u);
    EXPECT_EQ(bracketed->Column, 1u);
}

TEST(SourceLocationTests, TheLastCSharpFileWins)
{
    const std::optional<SourceLocation> location =
        FindSourceLocationInLogLine("Assets/Caller.cs:1 called Assets/Callee.cs:9");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Path, std::filesystem::path("Assets/Callee.cs"));
    EXPECT_EQ(location->Line, 9u);
}

TEST(SourceLocationTests, LineWithoutCSharpFileNamesNothing)
{
    EXPECT_FALSE(FindSourceLocationInLogLine("Loaded scene in 12 ms").has_value());
    EXPECT_FALSE(FindSourceLocationInLogLine("").has_value());
}
