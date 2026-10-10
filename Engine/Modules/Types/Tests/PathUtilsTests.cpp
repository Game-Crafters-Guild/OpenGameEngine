#include <gtest/gtest.h>

#include "Types/PathUtils.h"

#include <filesystem>

using namespace GameEngine;
namespace fs = std::filesystem;

TEST(PathUtilsTests, AFileInsideTheRootIsSpelledRelativeToIt)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/Surfaces/mine.glsl", "/proj/Shaders"),
              fs::path("Surfaces/mine.glsl"));
}

TEST(PathUtilsTests, BothSidesAreNormalisedBeforeComparing)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/../Shaders/./a.glsl", "/proj/Shaders/"),
              fs::path("a.glsl"));
}

TEST(PathUtilsTests, OutsideTheRootIsEmpty)
{
    EXPECT_TRUE(RelativePathUnderRoot("/elsewhere/a.glsl", "/proj/Shaders").empty());
    EXPECT_TRUE(RelativePathUnderRoot("/proj/a.glsl", "/proj/Shaders").empty())
        << "a sibling of the root walks out through ..";
    EXPECT_TRUE(RelativePathUnderRoot("Surfaces/a.glsl", "/proj/Shaders").empty())
        << "a relative path shares no root with an absolute one";
}

TEST(PathUtilsTests, TheRootItselfIsNotUnderTheRoot)
{
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders", "/proj/Shaders").empty());
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders/", "/proj/Shaders").empty());
}

TEST(PathUtilsTests, AnEmptyRootContainsNothing)
{
    EXPECT_TRUE(RelativePathUnderRoot("/proj/Shaders/a.glsl", "").empty());
}

TEST(PathUtilsTests, ANameThatStartsWithTwoDotsIsNotAWalkOut)
{
    EXPECT_EQ(RelativePathUnderRoot("/proj/Shaders/..hidden.glsl", "/proj/Shaders"),
              fs::path("..hidden.glsl"));
}
