#include "CliSessionProvider.h"

#include "CancelToken.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Platform/Process.h"
#include "Types/StringUtils.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
// A stderr line quoted in a failure message is cut here.
constexpr std::size_t kMaxQuotedStderrChars = 300;
// A version or login check that has not exited by then is ended and reported: the
// real CLIs answer in under 4 s.
constexpr std::chrono::seconds kCheckTimeout{20};
// The oldest Node.js the MCP server supports (mcp/package.json "engines").
constexpr int kMinimumNodeMajor = 20;

std::vector<Platform::EnvironmentEdit> StrippedEnvironment()
{
    std::vector<Platform::EnvironmentEdit> edits;
    for (std::string_view name : CliSessionProvider::kStrippedVariables)
        edits.push_back({std::string(name), std::nullopt});
    return edits;
}

// A .cmd or .bat script runs through cmd.exe, which reparses every argument; npm's
// `claude.cmd` shim is one.
bool IsCommandScript(const std::string& executable)
{
    const std::string extension = std::filesystem::path(executable).extension().string();
    return EqualsIgnoreCase(extension, ".cmd") || EqualsIgnoreCase(extension, ".bat");
}

// The last non-empty line of `text`, cut at kMaxQuotedStderrChars.
std::string LastLine(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    const std::size_t start = text.find_last_of('\n');
    std::string_view line = start == std::string_view::npos ? text : text.substr(start + 1);
    if (line.size() > kMaxQuotedStderrChars)
        line = line.substr(0, kMaxQuotedStderrChars);
    return std::string(line);
}

#if defined(_WIN32)
constexpr const char* kHomeVariable = "USERPROFILE";
#else
constexpr const char* kHomeVariable = "HOME";
#endif

// The path the environment variable `name` holds; empty when it is unset. Read wide
// on Windows, so a profile path outside the ANSI code page survives.
std::filesystem::path EnvironmentPath(const char* name)
{
#if defined(_WIN32)
    const std::wstring wideName(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wideName.c_str());
#else
    const char* value = std::getenv(name);
#endif
    return value ? std::filesystem::path(value) : std::filesystem::path();
}
} // namespace

CliSessionProvider::CliSessionProvider(std::string executable, std::filesystem::path workingDirectory)
    : m_Executable(std::move(executable))
    , m_WorkingDirectory(std::move(workingDirectory))
{
}

ProviderCapabilities CliSessionProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = true, .NeedsKey = false, .CanActWithTools = true};
}

std::string CliSessionProvider::ToolsRefusal(const std::string& node, uint16_t port, const CancelToken& cancel)
{
    std::lock_guard lock(m_ToolsMutex);
    const auto key = std::make_pair(node, port);
    if (const auto known = m_ToolsRefusals.find(key); known != m_ToolsRefusals.end())
        return known->second;

    const CommandOutput version = RunProgram(node, "Node.js", {"-p", "process.versions.node"}, cancel);
    std::string refusal;
    if (!version.Failure.empty() || version.ExitCode != 0)
    {
        refusal = "Node.js did not run (" + node + "): install Node " + std::to_string(kMinimumNodeMajor) +
                  " or later, or set its path in Settings > AI Assistant > Node executable.";
    }
    else if (std::atoi(version.Stdout.c_str()) < kMinimumNodeMajor)
    {
        refusal = "Node.js " + LastLine(version.Stdout) + " is too old for the editor's tools: install Node " +
                  std::to_string(kMinimumNodeMajor) + " or later.";
    }
    else
    {
        refusal = ToolsBlocker(node, port, cancel);
    }
    // A stopped check answers nothing: the next turn asks again.
    if (!cancel.IsCancelled())
        m_ToolsRefusals[key] = refusal;
    return refusal;
}

std::string CliSessionProvider::ToolsBlocker(const std::string&, uint16_t, const CancelToken&) const
{
    return {};
}

void CliSessionProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel)
{
    TurnState turn{events};
    {
        std::lock_guard lock(m_ReadinessMutex);
        if (!m_Ready)
        {
            std::string refusal = CheckReadiness(cancel);
            if (cancel.IsCancelled())
            {
                turn.Result.Outcome = TurnOutcome::Stopped;
                events.OnFinished(turn.Result);
                return;
            }
            if (!refusal.empty())
            {
                turn.Result.Error = std::move(refusal);
                events.OnFinished(turn.Result);
                return;
            }
            m_Ready = true;
        }
    }

    AgentTurnRequest attached = request;
    if (request.Tools)
    {
        const std::string refusal = ToolsRefusal(request.Tools->Node, request.Tools->Port, cancel);
        if (cancel.IsCancelled())
        {
            turn.Result.Outcome = TurnOutcome::Stopped;
            events.OnFinished(turn.Result);
            return;
        }
        if (!refusal.empty())
        {
            attached.Tools = nullptr;
            turn.Result.Note = "This reply ran without the editor's tools. " + refusal;
        }
    }

    ProcessLaunch launch;
    launch.Executable = m_Executable;
    launch.Arguments = TurnArguments(attached);
    launch.WorkingDirectory = m_WorkingDirectory;
    launch.Environment = StrippedEnvironment();
    launch.StdinPayload = request.UserText;

    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel] { return cancel.IsCancelled(); },
        [this, &turn](const std::string& line)
        {
            if (!turn.Completed)
                ReadLine(line, turn);
        });

    TurnResult& result = turn.Result;
    if (run.cancelled)
    {
        result.Outcome = TurnOutcome::Stopped;
    }
    else if (run.exitCode == -1)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = NotStartedMessage();
    }
    else if (run.overlongStdoutLines > 0)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The reply exceeded 1 MiB in one line, so it could not be read whole.";
    }
    else if (!turn.Completed)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = std::string(DisplayName()) + " exited with code " + std::to_string(run.exitCode) +
                       " before finishing the reply";
        const std::string reason = LastLine(run.stderrTail);
        result.Error += reason.empty() ? "." : ": " + reason;
        result.SessionNotFound = !request.SessionId.empty() && IsUnknownSessionError(run.stderrTail);
    }
    events.OnFinished(result);
}

CliSessionProvider::CommandOutput CliSessionProvider::RunCommand(const std::vector<std::string>& arguments,
                                                                const CancelToken& cancel) const
{
    return RunProgram(m_Executable, DisplayName(), arguments, cancel);
}

CliSessionProvider::CommandOutput CliSessionProvider::RunProgram(const std::string& executable,
                                                                std::string_view program,
                                                                const std::vector<std::string>& arguments,
                                                                const CancelToken& cancel) const
{
    CommandOutput output;
    if (IsCommandScript(executable))
    {
        output.Failure = std::string(program) + " executable " + executable +
                         " is a command script, which is not supported: set it to the native program in "
                         "Settings > AI Assistant.";
        return output;
    }

    ProcessLaunch launch;
    launch.Executable = executable;
    launch.Arguments = arguments;
    launch.WorkingDirectory = m_WorkingDirectory;
    launch.Environment = StrippedEnvironment();

    const auto deadline = std::chrono::steady_clock::now() + kCheckTimeout;
    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel, deadline] { return cancel.IsCancelled() || std::chrono::steady_clock::now() >= deadline; },
        [&output](const std::string& line)
        {
            output.Stdout += line;
            output.Stdout += '\n';
        });
    output.ExitCode = run.exitCode;
    output.Stderr = run.stderrTail;
    if (run.cancelled && cancel.IsCancelled())
    {
        output.Failure = "Stopped.";
    }
    else if (run.cancelled)
    {
        std::string command;
        for (const std::string& argument : arguments)
            command += (command.empty() ? "" : " ") + argument;
        output.Failure = std::string(program) + " did not answer `" + command + "` within " +
                         std::to_string(kCheckTimeout.count()) +
                         " s: check its executable in Settings > AI Assistant.";
    }
    else if (run.exitCode == -1)
    {
        output.Failure = executable == m_Executable ? NotStartedMessage()
                                                    : "Could not start " + std::string(program) + " (" + executable + ").";
    }
    return output;
}

std::string CliSessionProvider::QuotedLastLine(std::string_view text)
{
    return LastLine(text);
}

bool CliSessionProvider::IsUnknownSessionError(std::string_view) const
{
    return false;
}

std::filesystem::path CliSessionProvider::DataDirectory(const char* variable, const char* folder)
{
    if (std::filesystem::path named = EnvironmentPath(variable); !named.empty())
        return named;
    const std::filesystem::path home = EnvironmentPath(kHomeVariable);
    return home.empty() ? std::filesystem::path() : home / folder;
}

std::string CliSessionProvider::NotStartedMessage() const
{
    if (m_Executable.empty())
        return "No " + std::string(DisplayName()) + " executable: set its path in Settings > AI Assistant.";
    return "Could not start " + std::string(DisplayName()) + " (" + m_Executable +
           "): set its path in Settings > AI Assistant.";
}
} // namespace GameEngine
