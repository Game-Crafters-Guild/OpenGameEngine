// BuildHost: which targets each editor host compiles itself, and the refusal
// that tells the user how to build the others from that host.

#include "Engine/Build/BuildHost.h"

#include <gtest/gtest.h>

#include <string_view>

using GameEngine::BuildHost;
using GameEngine::BuildHostCompilesTarget;
using GameEngine::UnsupportedBuildTargetMessage;

namespace
{

bool Mentions(std::string_view message, std::string_view text)
{
    return message.find(text) != std::string_view::npos;
}

} // namespace

TEST(BuildHostTests, EachHostCompilesOnlyItsNativeTargets)
{
    EXPECT_TRUE(BuildHostCompilesTarget(BuildHost::Windows, "Windows"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Windows, "Linux"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Windows, "Steam"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Windows, "Mac"));

    EXPECT_TRUE(BuildHostCompilesTarget(BuildHost::Mac, "Mac"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Mac, "Windows"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Mac, "Linux"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Mac, "Steam"));

    EXPECT_TRUE(BuildHostCompilesTarget(BuildHost::Linux, "Linux"));
    EXPECT_TRUE(BuildHostCompilesTarget(BuildHost::Linux, "Steam"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Linux, "Windows"));
    EXPECT_FALSE(BuildHostCompilesTarget(BuildHost::Linux, "Mac"));
}

// Windows has no runtime-template path, so its message must not send the user
// looking for one; it names the Linux editor and the WSL2 Deck build instead.
TEST(BuildHostTests, WindowsRefusalNamesFixesAvailableOnWindows)
{
    const std::string_view message = UnsupportedBuildTargetMessage(BuildHost::Windows);
    EXPECT_TRUE(Mentions(message, "Linux x64 editor"));
    EXPECT_TRUE(Mentions(message, "build_deck.py"));
    // The editor may run without an engine checkout; no repository path.
    EXPECT_FALSE(Mentions(message, "Tools/Scripts"));
    EXPECT_TRUE(Mentions(message, "WSL2"));
    EXPECT_TRUE(Mentions(message, "macOS editor"));
    EXPECT_FALSE(Mentions(message, "template"));
}

TEST(BuildHostTests, MacRefusalNamesTheOtherEditors)
{
    const std::string_view message = UnsupportedBuildTargetMessage(BuildHost::Mac);
    EXPECT_TRUE(Mentions(message, "runtime template"));
    EXPECT_TRUE(Mentions(message, "Windows editor"));
    EXPECT_TRUE(Mentions(message, "Linux x64 editor"));
}

TEST(BuildHostTests, LinuxRefusalNamesTheOtherEditors)
{
    const std::string_view message = UnsupportedBuildTargetMessage(BuildHost::Linux);
    EXPECT_TRUE(Mentions(message, "Windows editor"));
    EXPECT_TRUE(Mentions(message, "macOS editor"));
    EXPECT_FALSE(Mentions(message, "template"));
}
