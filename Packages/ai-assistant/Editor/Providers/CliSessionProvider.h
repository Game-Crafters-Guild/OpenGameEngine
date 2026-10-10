#pragma once

#include "CliSessionList.h"
#include "IAgentProvider.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
/// A local CLI session (Claude Code, Codex) that keeps its own transcript: each turn
/// spawns the user's own executable with the prompt on stdin and reads one JSON
/// record per stdout line. The child inherits the editor's environment minus every
/// key variable a CLI would prefer over the user's login (kStrippedVariables), so the
/// session runs on the login and never on a key. The derived class supplies the
/// command line and reads the records.
class CliSessionProvider : public IAgentProvider
{
public:
    /// The variables removed from every child's environment, version and login checks
    /// included.
    static constexpr std::string_view kStrippedVariables[] = {
        "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CODEX_API_KEY", "OPENAI_API_KEY"};

    /// What the login check reports, for the settings page's Login status row.
    struct LoginStatus
    {
        /// The CLI reported a usable login.
        bool LoggedIn = false;
        /// One line for the user: the login method, or what is wrong and the fix.
        /// Never holds an email address, an organization or a key.
        std::string Text;
    };

    /// `executable` is a program name searched for by the platform or a full path;
    /// `workingDirectory` is the session's directory (the project root), empty to
    /// inherit the editor's.
    CliSessionProvider(std::string executable, std::filesystem::path workingDirectory);

    ProviderCapabilities Capabilities() const override;

    /// Checks the CLI once per provider (CheckReadiness) until it passes, then spawns
    /// the turn. A turn that attaches the editor's tools runs without them when
    /// ToolsRefusal() refuses, and its TurnResult::Note says why. Ends Stopped when `cancel` fires, checks included; Failed when the
    /// CLI cannot start, a check does not answer within its time limit, a stdout line
    /// exceeds 1 MiB, the CLI reports an error or exits before its completion record;
    /// Text keeps what arrived in every case.
    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) final;

    /// Runs the CLI's own login check under the stripped environment. Blocks for as
    /// long as the CLI takes (seconds, at most the check's time limit) or until
    /// `cancel` fires: call it off the UI thread.
    virtual LoginStatus CheckLogin(const CancelToken& cancel) const = 0;

    /// The sessions the CLI keeps on disk for WorkingDirectory(), read from its own
    /// files (CliSessionFiles); empty without a working directory. Reads files for as
    /// long as that takes or until `cancel` fires: call it off the UI thread.
    virtual CliSessionList ListSessions(const CancelToken& cancel) const = 0;

    /// Empty when a turn can attach the editor's tools through the Node.js executable
    /// `node` to the debug port `port`; otherwise why not, with the fix. Checks that
    /// `node` is Node 20 or later, then the CLI's own condition (ToolsBlocker), and
    /// remembers the answer per node and port. Blocks for as long as the checks take
    /// (seconds) or until `cancel` fires: call it off the UI thread.
    std::string ToolsRefusal(const std::string& node, uint16_t port, const CancelToken& cancel);

    /// The executable this provider runs.
    const std::string& Executable() const { return m_Executable; }
    /// The directory sessions run in; empty inherits the editor's.
    const std::filesystem::path& WorkingDirectory() const { return m_WorkingDirectory; }

protected:
    /// One turn's progress, filled by ReadLine().
    struct TurnState
    {
        AgentTurnEvents& Events;
        TurnResult Result;
        /// Set by ReadLine() on the record that ends the turn, with Result.Outcome
        /// Succeeded or Failed (Result.Error then says why).
        bool Completed = false;
    };

    /// What a short CLI command (a version or login check) printed.
    struct CommandOutput
    {
        /// Empty when the command ran to its end; otherwise why it did not (the
        /// executable is a .cmd or .bat script, could not start, was stopped or did
        /// not answer in time), with the fix.
        std::string Failure;
        /// The exit code; -1 when the executable could not be started.
        int ExitCode = -1;
        /// Every stdout line, each followed by '\n'.
        std::string Stdout;
        /// The end of stderr.
        std::string Stderr;
    };

    /// The CLI's name in messages ("Claude Code").
    virtual std::string_view DisplayName() const = 0;
    /// The arguments of one turn; the prompt itself goes on stdin.
    virtual std::vector<std::string> TurnArguments(const AgentTurnRequest& request) const = 0;
    /// Reads one stdout line of the turn and reports through `turn.Events`.
    virtual void ReadLine(std::string_view line, TurnState& turn) const = 0;
    /// Empty when the CLI can run a turn; otherwise the refusal with its fix. Returns
    /// early once `cancel` fires.
    virtual std::string CheckReadiness(const CancelToken& cancel) const = 0;
    /// The end of stderr of a turn that exited before its completion record says the
    /// session the turn continued does not exist (TurnResult::SessionNotFound). A CLI
    /// that reports it in its stdout records sets it in ReadLine() instead.
    virtual bool IsUnknownSessionError(std::string_view stderrTail) const;

    /// Empty when the model can reach the editor's port `port` only through the
    /// attached server; otherwise why not. A CLI whose model can run commands checks
    /// whether they can open that port (`node` runs the probe).
    virtual std::string ToolsBlocker(const std::string& node, uint16_t port, const CancelToken& cancel) const;

    /// Runs `<executable> <arguments>` with the stripped environment in the working
    /// directory, without a stdin payload, and waits for it until it exits, `cancel`
    /// fires or the check's time limit passes.
    CommandOutput RunCommand(const std::vector<std::string>& arguments, const CancelToken& cancel) const;
    /// RunCommand() for another program than the CLI (`program` names it in messages).
    CommandOutput RunProgram(const std::string& executable, std::string_view program,
                             const std::vector<std::string>& arguments, const CancelToken& cancel) const;
    /// The last non-empty line of `text` (a command's stderr), cut to the length a
    /// message quotes.
    static std::string QuotedLastLine(std::string_view text);
    /// The refusal for an executable that could not be started.
    std::string NotStartedMessage() const;
    /// Where the CLI keeps its files: the directory the environment variable
    /// `variable` names (the CLI reads the same environment), else `folder` in the
    /// user's home directory.
    static std::filesystem::path DataDirectory(const char* variable, const char* folder);

private:
    std::string m_Executable;
    std::filesystem::path m_WorkingDirectory;
    std::mutex m_ReadinessMutex;
    bool m_Ready = false;
    /// ToolsRefusal() answers by node and port.
    std::mutex m_ToolsMutex;
    std::map<std::pair<std::string, uint16_t>, std::string> m_ToolsRefusals;
};
} // namespace GameEngine
