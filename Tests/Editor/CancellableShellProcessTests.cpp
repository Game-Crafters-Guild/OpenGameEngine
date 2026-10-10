// Runtime tests for CancellableShellProcess: output capture, exit-code reporting,
// per-line streaming, and cooperative cancellation. Uses only universal shell
// builtins (echo / exit) plus a long no-op command that the cancellation test
// terminates early, so the suite never hangs.
//
// RunProcessCapturedTests covers the sibling free function, whose contract is the
// opposite one: no shell parses the arguments, so each one must arrive in the
// child's argv exactly as it was passed. The ArgvEcho fixture reports what
// arrived.
//
// The argv Run() tests drive ProcessStreamProbe, which writes known shapes to
// stdout and stderr and reports its stdin, working directory and environment.

#include "Engine/Build/CancellableShellProcess.h"

#include "Platform/Process.h"
#include "Platform/Shell.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

using GameEngine::CancellableShellProcess;
using GameEngine::kMaxProcessStdinBytes;
using GameEngine::kMaxProcessStdoutLineBytes;
using GameEngine::kProcessStderrTailBytes;
using GameEngine::ProcessLaunch;
using GameEngine::RunProcessCaptured;
using GameEngine::ShellProcessResult;

namespace {
#if defined(_WIN32)
const char* kEchoCommand  = "echo CSP_marker_12345";
const char* kExit3Command = "exit 3";
const char* kLongCommand  = "powershell -NoProfile -Command Start-Sleep -Seconds 30";  // 30s if never cancelled
const char* kArgvEchoName = "ArgvEcho.exe";
const char* kProbeName    = "ProcessStreamProbe.exe";
#else
const char* kEchoCommand  = "echo CSP_marker_12345";
const char* kExit3Command = "exit 3";
const char* kLongCommand  = "sleep 30";
const char* kArgvEchoName = "ArgvEcho";
const char* kProbeName    = "ProcessStreamProbe";
#endif

// A hung child would otherwise stall the suite; ArgvEcho prints and exits.
constexpr std::chrono::seconds kArgvEchoTimeout{30};

auto NeverCancel = [] { return false; };

// Anchored to this executable, never to the working directory: the fixture is
// staged beside the test binary.
std::filesystem::path ArgvEchoPath()
{
    const std::filesystem::path self = GameEngine::Platform::GetExecutablePath();
    if (self.empty())
        return {};
    return self.parent_path() / kArgvEchoName;
}

std::filesystem::path ProbePath()
{
    return GameEngine::Platform::GetExecutablePath().parent_path() / kProbeName;
}

ProcessLaunch ProbeLaunch(std::vector<std::string> arguments)
{
    ProcessLaunch launch;
    launch.Executable = ProbePath().string();
    launch.Arguments = std::move(arguments);
    return launch;
}

// Runs the argv overload, collecting the lines handed to the callback.
ShellProcessResult RunCollecting(const ProcessLaunch& launch, std::vector<std::string>& lines)
{
    CancellableShellProcess proc;
    return proc.Run(launch, NeverCancel, [&lines](const std::string& line) { lines.push_back(line); });
}

// Sets a variable in this process for one test and restores the previous state after.
class ScopedParentVariable
{
public:
    ScopedParentVariable(const char* name, const char* value) : m_Name(name)
    {
        if (const char* previous = std::getenv(name))
            m_Previous = previous;
        Set(value);
    }
    ~ScopedParentVariable() { Set(m_Previous ? m_Previous->c_str() : nullptr); }

    ScopedParentVariable(const ScopedParentVariable&) = delete;
    ScopedParentVariable& operator=(const ScopedParentVariable&) = delete;

private:
    void Set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(m_Name.c_str(), value ? value : "");
#else
        if (value)
            setenv(m_Name.c_str(), value, 1);
        else
            unsetenv(m_Name.c_str());
#endif
    }

    std::string m_Name;
    std::optional<std::string> m_Previous;
};

#if defined(_WIN32)
// An inheritable event in this process, named so the probe's "holds" mode can tell
// whether the handle value it was given names this event in the child.
class InheritableMarker
{
public:
    InheritableMarker() : m_Name("GE_CSP_Marker_" + std::to_string(::GetCurrentProcessId()))
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        m_Event = ::CreateEventA(&sa, TRUE, FALSE, m_Name.c_str());
    }
    ~InheritableMarker() { ::CloseHandle(m_Event); }

    InheritableMarker(const InheritableMarker&) = delete;
    InheritableMarker& operator=(const InheritableMarker&) = delete;

    bool IsValid() const { return m_Event != nullptr; }
    const std::string& Name() const { return m_Name; }
    std::string HandleValue() const { return std::to_string(reinterpret_cast<std::uintptr_t>(m_Event)); }

private:
    std::string m_Name;
    HANDLE m_Event = nullptr;
};
#endif
} // namespace

TEST(CancellableShellProcessTests, CapturesOutputAndZeroExit)
{
    CancellableShellProcess proc;
    const ShellProcessResult result = proc.Run(kEchoCommand, NeverCancel, nullptr);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_FALSE(result.cancelled);
    EXPECT_NE(result.output.find("CSP_marker_12345"), std::string::npos) << result.output;
}

TEST(CancellableShellProcessTests, ReportsNonZeroExitCode)
{
    CancellableShellProcess proc;
    const ShellProcessResult result = proc.Run(kExit3Command, NeverCancel, nullptr);
    EXPECT_EQ(result.exitCode, 3);
    EXPECT_FALSE(result.cancelled);
}

TEST(CancellableShellProcessTests, StreamsOutputLinesToCallback)
{
    CancellableShellProcess proc;
    std::vector<std::string> lines;
    const ShellProcessResult result =
        proc.Run(kEchoCommand, NeverCancel,
                 [&](const std::string& line) { lines.push_back(line); });

    EXPECT_EQ(result.exitCode, 0);
    bool sawMarker = false;
    for (const std::string& line : lines)
        if (line.find("CSP_marker_12345") != std::string::npos)
            sawMarker = true;
    EXPECT_TRUE(sawMarker) << "lines streamed: " << lines.size();
}

TEST(CancellableShellProcessTests, CancellationTerminatesPromptlyWithoutHanging)
{
    CancellableShellProcess proc;
    // Cancel on the first poll: Run() must terminate the long-running child and
    // return quickly (well under the command's 30s), reporting cancellation.
    const ShellProcessResult result = proc.Run(kLongCommand, [] { return true; }, nullptr);

    EXPECT_TRUE(result.cancelled);
    EXPECT_NE(result.exitCode, 0);  // terminated, not a clean exit
}

TEST(RunProcessCapturedTests, ArgumentsReachTheChildVerbatim)
{
    const std::filesystem::path echo = ArgvEchoPath();
    ASSERT_FALSE(echo.empty()) << "could not resolve this executable's own path";
    ASSERT_TRUE(std::filesystem::exists(echo)) << echo.string();

    // Every shape a command line can lose: a space (forces quoting), an empty
    // argument (vanishes unless emitted as ""), an embedded quote, and a
    // trailing backslash (which would otherwise escape the closing quote).
    const std::vector<std::string> arguments = {
        "plain", "two words", "", "say \"hi\"", "C:\\dir name\\", "-D", "HAS_POSITION",
    };
    const ShellProcessResult result =
        RunProcessCaptured(echo.string(), arguments, kArgvEchoTimeout);

    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_FALSE(result.cancelled);

    std::string expected;
    for (const std::string& argument : arguments)
        expected += "[" + argument + "]\n";

    // The Windows CRT writes stdout in text mode, so the child's \n reaches the
    // pipe as \r\n; the argument text itself carries no carriage returns.
    std::string actual = result.output;
    actual.erase(std::remove(actual.begin(), actual.end(), '\r'), actual.end());
    EXPECT_EQ(actual, expected);
}

#if defined(_WIN32)
TEST(RunProcessCapturedTests, ChildInheritsOnlyItsOutputPipe)
{
    const InheritableMarker marker;
    ASSERT_TRUE(marker.IsValid());

    const ShellProcessResult result =
        RunProcessCaptured(ProbePath().string(), {"holds", marker.HandleValue(), marker.Name()}, kArgvEchoTimeout);

    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("holds=0"), std::string::npos) << result.output;
}
#endif

TEST(RunProcessCapturedTests, MissingExecutableFailsWithoutOutput)
{
    // A caller cannot tell "the tool ran and rejected the input" from "the tool
    // was never there" unless the second case is silent and non-zero.
    const ShellProcessResult result =
        RunProcessCaptured("ge_no_such_program_9f3c1a", {"arg"}, kArgvEchoTimeout);

    EXPECT_NE(result.exitCode, 0);
    EXPECT_TRUE(result.output.empty()) << result.output;
}

TEST(CancellableShellProcessTests, ArgvRunPassesArgumentsVerbatimWithoutAShell)
{
    const std::vector<std::string> arguments = {
        "literal;$(touch injected)", "a & echo injected", "two words", "", "say \"hi\"", "C:\\dir name\\",
    };
    ProcessLaunch launch;
    launch.Executable = ArgvEchoPath().string();
    launch.Arguments = arguments;
    std::vector<std::string> lines;

    const ShellProcessResult result = RunCollecting(launch, lines);

    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    std::vector<std::string> expected;
    for (const std::string& argument : arguments)
        expected.push_back("[" + argument + "]");
    EXPECT_EQ(lines, expected);
}

TEST(CancellableShellProcessTests, ArgvRunAppliesEnvironmentEditsInTheWorkingDirectory)
{
    const ScopedParentVariable replaced("GE_CSP_REPLACED", "parent value");
    const ScopedParentVariable removed("GE_CSP_REMOVED", "parent value");
    const ScopedParentVariable kept("GE_CSP_KEPT", "kept");
    const GameEngine::TestUtils::ScopedTempDir directory(
        GameEngine::TestUtils::MakeUniqueTempDirectory("csp argv cwd"));
    ProcessLaunch launch = ProbeLaunch({"report", "GE_CSP_REPLACED", "GE_CSP_REMOVED", "GE_CSP_KEPT", "GE_CSP_ADDED"});
    launch.WorkingDirectory = directory.Path();
    launch.Environment = {
        {"GE_CSP_REPLACED", "value with spaces"},
        {"GE_CSP_REMOVED", std::nullopt},
        {"GE_CSP_ADDED", "added"},
    };
    std::vector<std::string> expected = {
        "cwd=" + std::filesystem::canonical(directory.Path()).generic_string(),
        "[GE_CSP_REPLACED=value with spaces]",
        "[GE_CSP_REMOVED unset]",
        "[GE_CSP_KEPT=kept]",
        "[GE_CSP_ADDED=added]",
    };
#if defined(_WIN32)
    // Names are case-insensitive on Windows: unsetting one spelling removes the
    // variable whatever case this process inherited it in.
    const ScopedParentVariable mixed("Ge_Csp_Mixed", "parent value");
    launch.Arguments.push_back("GE_CSP_MIXED");
    launch.Environment.push_back({"GE_CSP_MIXED", std::nullopt});
    expected.push_back("[GE_CSP_MIXED unset]");
#endif
    std::vector<std::string> lines;

    const ShellProcessResult result = RunCollecting(launch, lines);

    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    EXPECT_EQ(lines, expected);
}

TEST(CancellableShellProcessTests, ArgvRunRefusesAnEnvironmentEditWithoutAValidName)
{
    // An empty name would match Windows' "=C:=..." drive-directory entries.
    for (const std::string name : {"", "GE_CSP=SPLIT"})
    {
        ProcessLaunch launch = ProbeLaunch({"report", "GE_CSP_KEPT"});
        launch.Environment = {{name, std::nullopt}};
        std::vector<std::string> lines;

        const ShellProcessResult result = RunCollecting(launch, lines);

        EXPECT_EQ(result.exitCode, -1) << "[" << name << "]";
        EXPECT_TRUE(lines.empty()) << "[" << name << "]";
    }
}

TEST(CancellableShellProcessTests, ArgvRunReportsAFailedSpawnWithoutRunningAnything)
{
    const GameEngine::TestUtils::ScopedTempDir directory(
        GameEngine::TestUtils::MakeUniqueTempDirectory("csp argv spawn"));
    ProcessLaunch missingExecutable = ProbeLaunch({"streams"});
    missingExecutable.Executable = (directory.Path() / "no-such-program").string();
    ProcessLaunch missingDirectory = ProbeLaunch({"streams"});
    missingDirectory.WorkingDirectory = directory.Path() / "no-such-directory";

    for (const ProcessLaunch& launch : {missingExecutable, missingDirectory})
    {
        std::vector<std::string> lines;
        const ShellProcessResult result = RunCollecting(launch, lines);
        EXPECT_EQ(result.exitCode, -1) << launch.Executable << " in " << launch.WorkingDirectory.string();
        EXPECT_TRUE(lines.empty());
    }
}

TEST(CancellableShellProcessTests, ArgvRunWritesTheStdinPayloadAndClosesIt)
{
    // Larger than any pipe buffer; the child reads all of it before it writes.
    std::string payload = "first line of the prompt\n";
    payload.append(3u * 1024u * 1024u, 'p');
    std::vector<std::string> lines;
    ProcessLaunch launch = ProbeLaunch({"stdin"});
    launch.StdinPayload = payload;

    const ShellProcessResult result = RunCollecting(launch, lines);

    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    const std::vector<std::string> expected = {
        "bytes=" + std::to_string(payload.size()),
        "first=first line of the prompt",
    };
    EXPECT_EQ(lines, expected);
}

TEST(CancellableShellProcessTests, ArgvRunWritesStdinWhileTheChildWritesFirst)
{
    // The child writes more than a pipe buffer before it reads, and the payload is
    // more than a pipe buffer too: only a stdin writer apart from the reading
    // thread gets both through. Were that lost the run would hang, so a watchdog
    // kills it and the test fails instead of stalling the suite.
    constexpr std::size_t kLineBytes = 256u * 1024u;
    const std::string payload(3u * 1024u * 1024u, 'p');
    ProcessLaunch launch = ProbeLaunch({"write-then-read", std::to_string(kLineBytes)});
    launch.StdinPayload = payload;
    CancellableShellProcess proc;
    std::vector<std::string> lines;
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!finished.load())
            proc.Kill();
    });

    const ShellProcessResult result =
        proc.Run(launch, NeverCancel, [&lines](const std::string& line) { lines.push_back(line); });
    finished.store(true);
    watchdog.join();

    EXPECT_FALSE(result.cancelled) << "the run deadlocked and the watchdog killed it";
    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    const std::vector<std::string> expected = {std::string(kLineBytes, 'o'), "bytes=" + std::to_string(payload.size())};
    EXPECT_EQ(lines, expected);
}

#if defined(_WIN32)
TEST(CancellableShellProcessTests, ArgvRunChildInheritsOnlyItsOwnPipeEnds)
{
    const InheritableMarker marker;
    ASSERT_TRUE(marker.IsValid());
    std::vector<std::string> lines;

    const ShellProcessResult result = RunCollecting(ProbeLaunch({"holds", marker.HandleValue(), marker.Name()}), lines);

    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    EXPECT_EQ(lines, std::vector<std::string>{"holds=0"});
}

TEST(CancellableShellProcessTests, ArgvRunReturnsWhenTheChildExitsThoughADescendantHoldsItsStdin)
{
    // The child hands its stdin to a descendant that lives 4 s, then exits without
    // reading. The payload is more than a pipe buffer, so the write is still
    // blocked when the child is gone; Run() must not wait for the descendant.
    ProcessLaunch launch = ProbeLaunch({"stdin-holder", "4000"});
    launch.StdinPayload.assign(1u * 1024u * 1024u, 'p');
    std::vector<std::string> lines;
    const auto begin = std::chrono::steady_clock::now();

    const ShellProcessResult result = RunCollecting(launch, lines);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_EQ(result.exitCode, 0) << result.stderrTail;
    EXPECT_FALSE(result.cancelled);
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
}
#endif

TEST(CancellableShellProcessTests, ArgvRunKeepsStderrOutOfTheLineStream)
{
    std::vector<std::string> lines;

    const ShellProcessResult result = RunCollecting(ProbeLaunch({"streams"}), lines);

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(lines, std::vector<std::string>{"to-stdout"});
    EXPECT_TRUE(result.output.empty()) << "the argv Run hands lines to the callback and keeps none";
    EXPECT_NE(result.stderrTail.find("to-stderr"), std::string::npos) << result.stderrTail;
}

TEST(CancellableShellProcessTests, ArgvRunDropsOnlyTheCarriageReturnThatEndsALine)
{
    std::vector<std::string> lines;

    const ShellProcessResult result = RunCollecting(ProbeLaunch({"carriage-returns"}), lines);

    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(lines, (std::vector<std::string>{"a\rb", "c"}));
}

TEST(CancellableShellProcessTests, ArgvRunReportsAnOverCapLineInsteadOfTruncatingIt)
{
    std::vector<std::string> atCap;
    const ShellProcessResult fits =
        RunCollecting(ProbeLaunch({"line", std::to_string(kMaxProcessStdoutLineBytes)}), atCap);
    ASSERT_EQ(atCap.size(), 3u);
    EXPECT_EQ(atCap[1].size(), kMaxProcessStdoutLineBytes);
    EXPECT_EQ(fits.overlongStdoutLines, 0u);

    std::vector<std::string> overCap;
    const ShellProcessResult over =
        RunCollecting(ProbeLaunch({"line", std::to_string(kMaxProcessStdoutLineBytes + 1u)}), overCap);
    EXPECT_EQ(overCap, (std::vector<std::string>{"before", "after"}));
    EXPECT_EQ(over.overlongStdoutLines, 1u);
    EXPECT_EQ(over.exitCode, 0);
}

TEST(CancellableShellProcessTests, ArgvRunBoundsTheStderrTailAndRefusesAnOverCapStdin)
{
    std::vector<std::string> lines;
    const ShellProcessResult flood =
        RunCollecting(ProbeLaunch({"stderr-flood", std::to_string(3u * kProcessStderrTailBytes)}), lines);
    EXPECT_EQ(lines, std::vector<std::string>{"done"});
    EXPECT_EQ(flood.stderrTail.size(), kProcessStderrTailBytes);
    EXPECT_TRUE(flood.stderrTail.ends_with("END")) << flood.stderrTail.substr(flood.stderrTail.size() - 16);

    lines.clear();
    ProcessLaunch overCap = ProbeLaunch({"stdin"});
    overCap.StdinPayload.assign(kMaxProcessStdinBytes + 1u, 'p');
    const ShellProcessResult refused = RunCollecting(overCap, lines);
    EXPECT_EQ(refused.exitCode, -1);
    EXPECT_TRUE(lines.empty());
}

TEST(CancellableShellProcessTests, ArgvRunKillMidRunEndsTheChildPromptly)
{
    CancellableShellProcess proc;
    std::atomic<bool> started{false};
    std::thread killer([&] {
        while (!started.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        proc.Kill();
    });
    const auto begin = std::chrono::steady_clock::now();

    const ShellProcessResult result =
        proc.Run(ProbeLaunch({"sleep"}), NeverCancel, [&started](const std::string&) { started.store(true); });
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    killer.join();

    EXPECT_TRUE(result.cancelled);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_LT(elapsed, std::chrono::seconds(10));  // the probe sleeps 30 s unless killed
}
