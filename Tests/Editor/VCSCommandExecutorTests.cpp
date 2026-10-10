// VCSCommandExecutor's argument-vector Execute runs the VCS binary with no shell
// in between, so a commit message or path reaches the child as one argv entry
// and shell syntax inside it stays literal text, and its environment overrides
// reach the child on top of the inherited environment. The ArgvEcho and
// EnvEcho fixtures report what arrived.

#include "VCSIntegration/VCSCommandExecutor.h"

#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

#if defined(_WIN32)
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

// Anchored to this executable, never to the working directory: the fixtures
// are staged beside the test binary.
std::filesystem::path FixturePath(const std::string& name)
{
    const std::filesystem::path self = Platform::GetExecutablePath();
    if (self.empty())
        return {};
    return self.parent_path() / (name + kExecutableSuffix);
}

std::filesystem::path ArgvEchoPath()
{
    return FixturePath("ArgvEcho");
}

std::filesystem::path EnvEchoPath()
{
    return FixturePath("EnvEcho");
}

std::string WithoutCarriageReturns(std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

} // namespace

TEST(VCSCommandExecutorTests, ArgumentsReachTheChildVerbatim)
{
    const std::filesystem::path echo = ArgvEchoPath();
    ASSERT_FALSE(echo.empty()) << "could not resolve this executable's own path";
    ASSERT_TRUE(std::filesystem::exists(echo)) << echo.string();

    // A multi-word commit message, a path with a space, an empty argument,
    // embedded quotes, a trailing backslash, and shell syntax that a
    // /bin/sh -c line would split, expand or run.
    const std::vector<std::string> arguments = {
        "commit", "-m", "Fix the bug", "Assets/My Scene.scene", "", "say \"hi\"", "it's",
        "dir name\\", "$(echo injected)", "`echo injected`", "a;b", "x|y", "$HOME",
    };
    const VCSCommandExecutor::Result result =
        VCSCommandExecutor::Execute(echo, echo.parent_path(), arguments, true);

    std::string expected;
    for (const std::string& argument : arguments)
        expected += "[" + argument + "]\n";

    // The Windows CRT writes stdout in text mode, so the child's \n reaches the
    // pipe as \r\n; the argument text itself carries no carriage returns.
    const std::string actual = WithoutCarriageReturns(result.output);
    EXPECT_EQ(actual, expected) << result.error;
    // Callers branch on success, so a captured run must report the child's exit.
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.success);
}

TEST(VCSCommandExecutorTests, OutputWrittenJustBeforeExitIsCaptured)
{
    const std::filesystem::path echo = ArgvEchoPath();
    ASSERT_FALSE(echo.empty()) << "could not resolve this executable's own path";
    ASSERT_TRUE(std::filesystem::exists(echo)) << echo.string();

    // About 26 KB of output: several pipe reads' worth, written in one burst
    // before the child exits, and still under the Windows command-line limit.
    constexpr size_t kArgumentCount = 200;
    constexpr size_t kArgumentLength = 128;
    std::vector<std::string> arguments;
    std::string expected;
    for (size_t i = 0; i < kArgumentCount; ++i)
    {
        std::string argument = std::to_string(i);
        argument.resize(kArgumentLength, 'x');
        expected += "[" + argument + "]\n";
        arguments.push_back(std::move(argument));
    }

    const VCSCommandExecutor::Result result =
        VCSCommandExecutor::Execute(echo, echo.parent_path(), arguments, true);

    const std::string actual = WithoutCarriageReturns(result.output);
    EXPECT_EQ(actual.size(), expected.size());
    EXPECT_EQ(actual, expected) << result.error;
}

TEST(VCSCommandExecutorTests, EnvironmentOverridesReachTheChild)
{
    const std::filesystem::path echo = EnvEchoPath();
    ASSERT_FALSE(echo.empty()) << "could not resolve this executable's own path";
    ASSERT_TRUE(std::filesystem::exists(echo)) << echo.string();

    // Shell syntax in the value stays literal: no shell expands it.
    const VCSCommandExecutor::Environment environment = {{"GE_VCS_TEST_TOKEN", "a b $HOME `x`"}};
    const std::vector<std::string> variables = {"GE_VCS_TEST_TOKEN", "PATH"};
    const VCSCommandExecutor::Result result =
        VCSCommandExecutor::Execute(echo, echo.parent_path(), variables, true, environment);

    const std::string output = WithoutCarriageReturns(result.output);
    EXPECT_NE(output.find("[GE_VCS_TEST_TOKEN=a b $HOME `x`]\n"), std::string::npos) << output;
    // The override adds to the inherited environment instead of replacing it.
    EXPECT_EQ(output.find("[PATH unset]"), std::string::npos) << output;
    EXPECT_TRUE(result.success) << result.error;
}

TEST(VCSCommandExecutorTests, NoOverridesLeaveTheInheritedEnvironment)
{
    const std::filesystem::path echo = EnvEchoPath();
    ASSERT_FALSE(echo.empty()) << "could not resolve this executable's own path";
    ASSERT_TRUE(std::filesystem::exists(echo)) << echo.string();

    const std::vector<std::string> variables = {"GE_VCS_TEST_TOKEN", "PATH"};
    const VCSCommandExecutor::Result result =
        VCSCommandExecutor::Execute(echo, echo.parent_path(), variables, true);

    const std::string output = WithoutCarriageReturns(result.output);
    EXPECT_NE(output.find("[GE_VCS_TEST_TOKEN unset]\n"), std::string::npos) << output;
    EXPECT_EQ(output.find("[PATH unset]"), std::string::npos) << output;
}

} // namespace GameEngine
