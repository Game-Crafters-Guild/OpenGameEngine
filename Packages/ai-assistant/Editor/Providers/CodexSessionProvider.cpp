#include "CodexSessionProvider.h"

#include "CliSessionFiles.h"
#include "JsonFields.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// A request may wait on the editor (a capture, a confirmation) longer than Codex's
// 60 s default; the server's own time limit is 660 s.
constexpr int kToolTimeoutSeconds = 660;

// `text` as a TOML basic string, which is how `-c` reads a value.
std::string TomlString(std::string_view text)
{
    std::string quoted = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}

// The `-c` entries that define the editor's MCP server for one turn: its command, its
// environment, the tools Codex may call, approval left to the editor's gate, and the
// time limit.
std::vector<std::string> ServerOverrides(const AgentToolAttachment& tools)
{
    const std::string key = "mcp_servers." + tools.ServerName + ".";
    std::string environment;
    for (const auto& [name, value] : tools.Environment)
        environment += (environment.empty() ? "" : ", ") + name + " = " + TomlString(value);
    std::string enabled;
    for (const std::string& tool : tools.ToolNames)
        enabled += (enabled.empty() ? "" : ", ") + TomlString(tool);
    const std::string entries[] = {
        key + "command=" + TomlString(tools.Node),
        key + "args=[" + TomlString(tools.ServerScript) + "]",
        key + "env={" + environment + "}",
        key + "enabled_tools=[" + enabled + "]",
        key + "default_tools_approval_mode=\"approve\"",
        key + "tool_timeout_sec=" + std::to_string(kToolTimeoutSeconds),
    };
    std::vector<std::string> arguments;
    for (const std::string& entry : entries)
    {
        arguments.push_back("-c");
        arguments.push_back(entry);
    }
    return arguments;
}

// The first non-empty line of `text`, without its line ending.
std::string_view FirstLine(std::string_view text)
{
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    const std::size_t end = text.find_first_of("\r\n");
    return end == std::string_view::npos ? text : text.substr(0, end);
}
} // namespace

std::vector<std::string> CodexSessionProvider::TurnArguments(const AgentTurnRequest& request) const
{
    // The exec options go before `resume`: the subcommand takes none of its own here.
    std::vector<std::string> arguments = {"exec", "--json", "--skip-git-repo-check"};
    if (!WorkingDirectory().empty())
    {
        arguments.push_back("-C");
        arguments.push_back(WorkingDirectory().string());
    }
    arguments.insert(arguments.end(), {"-s", "read-only"});
    if (request.Tools)
    {
        // The user's own configuration (and the MCP servers in it) stays out of the turn;
        // the login is still read from CODEX_HOME.
        arguments.push_back("--ignore-user-config");
        const std::vector<std::string> overrides = ServerOverrides(*request.Tools);
        arguments.insert(arguments.end(), overrides.begin(), overrides.end());
    }
    if (!request.Model.empty())
    {
        arguments.push_back("-m");
        arguments.push_back(request.Model);
    }
    if (!request.SessionId.empty())
    {
        arguments.push_back("resume");
        arguments.push_back(request.SessionId);
    }
    arguments.push_back("-");
    return arguments;
}

std::string CodexSessionProvider::ToolsBlocker(const std::string& node, uint16_t port,
                                               const CancelToken& cancel) const
{
    const std::string probe = "require('net').connect(" + std::to_string(port) +
                              ",'127.0.0.1').on('connect',function(){console.log('CONNECTED');process.exit(0)})"
                              ".on('error',function(e){console.log('REFUSED '+e.code);process.exit(0)})";
    const CommandOutput output =
        RunCommand({"sandbox", "-c", "sandbox_mode=\"read-only\"", "--", node, "-e", probe}, cancel);
    if (output.Failure.empty() && output.Stdout.find("REFUSED") != std::string::npos)
        return {};
    if (output.Failure.empty() && output.Stdout.find("CONNECTED") != std::string::npos)
        return "Codex's shell can reach the editor's port outside the assistant's checks, so Codex runs as a "
               "conversation only. Use Claude (local session) for the editor's tools.";
    std::string reason = output.Failure.empty() ? "exit code " + std::to_string(output.ExitCode) : output.Failure;
    if (const std::string stderrLine = QuotedLastLine(output.Stderr); !stderrLine.empty())
        reason += ": " + stderrLine;
    return "Codex's sandbox could not be checked (" + reason +
           "), so Codex runs as a conversation only. Check Settings > AI Assistant > Codex executable, and that "
           "CODEX_HOME, when set, names an existing folder.";
}

void CodexSessionProvider::ReadLine(std::string_view line, TurnState& turn) const
{
    const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
    if (!record.is_object())
        return;

    TurnResult& result = turn.Result;
    const std::string type = JsonFields::String(record, "type");
    if (type == "thread.started")
    {
        const std::string threadId = JsonFields::String(record, "thread_id");
        if (!threadId.empty())
            turn.Events.OnSessionId(threadId);
    }
    else if (type == "item.completed")
    {
        ReadItem(JsonFields::Object(record, "item"), turn);
    }
    else if (type == "turn.completed")
    {
        turn.Completed = true;
        result.Outcome = TurnOutcome::Succeeded;
        const nlohmann::json& usage = JsonFields::Object(record, "usage");
        // input_tokens already includes cached_input_tokens.
        result.InputTokens = JsonFields::Count(usage, "input_tokens");
        result.OutputTokens = JsonFields::Count(usage, "output_tokens");
    }
    else if (type == "turn.failed" || type == "error")
    {
        std::string message = type == "error" ? JsonFields::String(record, "message")
                                              : JsonFields::String(JsonFields::Object(record, "error"), "message");
        turn.Completed = true;
        result.Outcome = TurnOutcome::Failed;
        result.Error = "Codex reported an error: " + (message.empty() ? std::string("no detail given") : message);
    }
}

void CodexSessionProvider::ReadItem(const nlohmann::json& item, TurnState& turn)
{
    const std::string type = JsonFields::String(item, "type");
    if (type == "agent_message")
    {
        std::string text = JsonFields::String(item, "text");
        if (text.empty())
            return;
        // Each message arrives whole; a later one starts a new paragraph.
        if (!turn.Result.Text.empty())
            text.insert(0, "\n\n");
        turn.Result.Text += text;
        turn.Events.OnTextDelta(text);
    }
    else if (type != "error" && type != "reasoning" && !type.empty())
    {
        turn.Events.OnToolActivity({type, item.dump()});
    }
}

CliSessionProvider::LoginStatus CodexSessionProvider::CheckLogin(const CancelToken& cancel) const
{
    const CommandOutput output = RunCommand({"login", "status"}, cancel);
    if (!output.Failure.empty())
        return {false, output.Failure};
    if (output.ExitCode != 0)
        return {false, "Not logged in: run `codex login`."};

    // Codex 0.162 prints the status on stderr.
    std::string_view line = FirstLine(output.Stdout);
    if (line.empty())
        line = FirstLine(output.Stderr);
    if (const std::size_t cut = line.find(" - "); cut != std::string_view::npos)
        line = line.substr(0, cut);
    return {true, line.empty() ? std::string("Logged in") : std::string(line)};
}

CliSessionList CodexSessionProvider::ListSessions(const CancelToken& cancel) const
{
    return CliSessionFiles::ReadCodexSessions(DataDirectory("CODEX_HOME", ".codex"), WorkingDirectory(), cancel);
}

bool CodexSessionProvider::IsUnknownSessionError(std::string_view stderrTail) const
{
    return stderrTail.find("no rollout found for thread id") != std::string_view::npos;
}

std::string CodexSessionProvider::CheckReadiness(const CancelToken& cancel) const
{
    const LoginStatus login = CheckLogin(cancel);
    return login.LoggedIn ? std::string() : login.Text;
}
} // namespace GameEngine
