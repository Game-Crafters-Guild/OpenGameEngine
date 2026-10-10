#include "ClaudeSessionProvider.h"

#include "CliSessionFiles.h"
#include "JsonFields.h"

#include <array>
#include <charconv>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
using Version = std::array<uint32_t, 3>;

// How `claude -p --resume <id>` 2.1.290 begins the message of an id it does not know.
constexpr std::string_view kUnknownSessionMessage = "No conversation found with session ID";

// "2.1.290 (Claude Code)" -> {2, 1, 290}; nullopt when the text does not start with
// three dot-separated numbers.
std::optional<Version> ParseVersion(std::string_view text)
{
    Version version{};
    const char* cursor = text.data();
    const char* end = text.data() + text.size();
    for (std::size_t part = 0; part < version.size(); ++part)
    {
        if (part > 0)
        {
            if (cursor == end || *cursor != '.')
                return std::nullopt;
            ++cursor;
        }
        const auto [next, error] = std::from_chars(cursor, end, version[part]);
        if (error != std::errc{})
            return std::nullopt;
        cursor = next;
    }
    return version;
}

std::string VersionText(const Version& version)
{
    return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." + std::to_string(version[2]);
}
} // namespace

std::vector<std::string> ClaudeSessionProvider::TurnArguments(const AgentTurnRequest& request) const
{
    std::vector<std::string> arguments = {"-p", "--output-format", "stream-json", "--verbose",
                                          "--include-partial-messages"};
    if (!request.SessionId.empty())
    {
        arguments.push_back("--resume");
        arguments.push_back(request.SessionId);
    }
    arguments.insert(arguments.end(), {"--permission-prompts", "none", "--tools", "", "--strict-mcp-config",
                                       "--system-prompt-snapshot", "off"});
    if (request.Tools)
    {
        // The server comes from the editor's file alone (--strict-mcp-config), no built-in
        // tool is available (--tools ""), and a tool outside this list is denied without a
        // prompt (--permission-prompts none).
        std::string allowed;
        for (const std::string& tool : request.Tools->ToolNames)
            allowed += (allowed.empty() ? "" : ",") + ("mcp__" + request.Tools->ServerName + "__" + tool);
        arguments.insert(arguments.end(),
                         {"--mcp-config", request.Tools->ConfigFile.string(), "--allowedTools", allowed});
    }
    if (!request.Model.empty())
    {
        arguments.push_back("--model");
        arguments.push_back(request.Model);
    }
    if (!request.Effort.empty())
    {
        arguments.push_back("--effort");
        arguments.push_back(request.Effort);
    }
    if (!request.SystemPrompt.empty())
    {
        arguments.push_back("--append-system-prompt");
        arguments.push_back(request.SystemPrompt);
    }
    return arguments;
}

void ClaudeSessionProvider::ReadLine(std::string_view line, TurnState& turn) const
{
    // Lines that are not JSON objects (a CLI warning) carry nothing for the turn.
    const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
    if (!record.is_object())
        return;

    const std::string type = JsonFields::String(record, "type");
    if (type == "system" && JsonFields::String(record, "subtype") == "init")
    {
        const std::string sessionId = JsonFields::String(record, "session_id");
        if (!sessionId.empty())
            turn.Events.OnSessionId(sessionId);
        // The model the alias or the user's own setting resolved to.
        turn.Result.Model = JsonFields::String(record, "model");
    }
    else if (type == "stream_event")
    {
        ReadTextDelta(record, turn);
    }
    else if (type == "assistant")
    {
        ReadToolCalls(record, turn);
    }
    else if (type == "user")
    {
        ReadToolResults(record, turn);
    }
    else if (type == "result")
    {
        ReadResult(record, turn);
    }
}

void ClaudeSessionProvider::ReadToolCalls(const nlohmann::json& record, TurnState& turn)
{
    // The message's text was streamed already; only its tool calls are new here.
    const nlohmann::json& message = JsonFields::Object(record, "message");
    const auto content = message.find("content");
    if (content == message.end() || !content->is_array())
        return;
    for (const nlohmann::json& block : *content)
    {
        if (!block.is_object() || JsonFields::String(block, "type") != "tool_use")
            continue;
        turn.Events.OnToolActivity({JsonFields::String(block, "name"), JsonFields::Object(block, "input").dump(),
                                    JsonFields::String(block, "id")});
    }
}

// The first line of a tool_result's text (a string, or the first text block), at most
// kMaxErrorBytes and then ending in an ellipsis; empty when it carries no text. The call's
// row shows two lines of it and its Details the whole.
std::string ClaudeSessionProvider::FirstErrorLine(const nlohmann::json& toolResult)
{
    constexpr size_t kMaxErrorBytes = 4096;
    const auto content = toolResult.find("content");
    std::string text;
    if (content != toolResult.end() && content->is_string())
        text = content->get<std::string>();
    else if (content != toolResult.end() && content->is_array())
        for (const nlohmann::json& part : *content)
            if (part.is_object() && JsonFields::String(part, "type") == "text")
            {
                text = JsonFields::String(part, "text");
                break;
            }
    text = text.substr(0, text.find('\n'));
    if (text.size() > kMaxErrorBytes)
    {
        size_t end = kMaxErrorBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        text.resize(end);
        text += "\xE2\x80\xA6";
    }
    return text;
}

void ClaudeSessionProvider::ReadToolResults(const nlohmann::json& record, TurnState& turn)
{
    const nlohmann::json& message = JsonFields::Object(record, "message");
    const auto content = message.find("content");
    if (content == message.end() || !content->is_array())
        return;
    for (const nlohmann::json& block : *content)
    {
        if (!block.is_object() || JsonFields::String(block, "type") != "tool_result")
            continue;
        const auto isError = block.find("is_error");
        if (isError != block.end() && isError->is_boolean() && isError->get<bool>())
            turn.Events.OnToolFailed({JsonFields::String(block, "tool_use_id"), false, FirstErrorLine(block)});
    }
}

void ClaudeSessionProvider::ReadResult(const nlohmann::json& record, TurnState& turn)
{
    TurnResult& result = turn.Result;
    turn.Completed = true;
    if (const auto cost = record.find("total_cost_usd"); cost != record.end() && cost->is_number())
        result.CostUsd = cost->get<double>();
    result.ModelRoundTrips = static_cast<uint32_t>(JsonFields::Count(record, "num_turns"));
    const nlohmann::json& usage = JsonFields::Object(record, "usage");
    result.InputTokens = JsonFields::Count(usage, "input_tokens") +
                         JsonFields::Count(usage, "cache_creation_input_tokens") +
                         JsonFields::Count(usage, "cache_read_input_tokens");
    result.OutputTokens = JsonFields::Count(usage, "output_tokens");
    if (const auto denials = record.find("permission_denials"); denials != record.end() && denials->is_array())
    {
        result.PermissionDenials = static_cast<uint32_t>(denials->size());
        // A call the CLI refused itself: its tool_result already reported it as an error.
        for (const nlohmann::json& denial : *denials)
            if (denial.is_object())
                if (std::string callId = JsonFields::String(denial, "tool_use_id"); !callId.empty())
                    turn.Events.OnToolFailed({std::move(callId), true, {}});
    }

    if (const auto isError = record.find("is_error"); isError != record.end() && isError->is_boolean() && isError->get<bool>())
    {
        // A turn that never ran (an unknown --resume id) has no result text; its
        // message is the first of `errors`.
        std::string detail = JsonFields::String(record, "result");
        if (const auto errors = record.find("errors");
            detail.empty() && errors != record.end() && errors->is_array() && !errors->empty() &&
            errors->front().is_string())
            detail = errors->front().get<std::string>();
        if (detail.empty())
            detail = JsonFields::String(record, "subtype");
        result.SessionNotFound = detail.starts_with(kUnknownSessionMessage);
        result.Outcome = TurnOutcome::Failed;
        result.Error = "Claude Code reported an error: " + (detail.empty() ? std::string("no detail given") : detail);
        return;
    }

    result.Outcome = TurnOutcome::Succeeded;
    // A reply that streamed no deltas still arrives whole in the result.
    if (result.Text.empty())
    {
        result.Text = JsonFields::String(record, "result");
        if (!result.Text.empty())
            turn.Events.OnTextDelta(result.Text);
    }
}

CliSessionProvider::LoginStatus ClaudeSessionProvider::CheckLogin(const CancelToken& cancel) const
{
    const CommandOutput output = RunCommand({"auth", "status"}, cancel);
    if (!output.Failure.empty())
        return {false, output.Failure};

    const nlohmann::json status = nlohmann::json::parse(output.Stdout, nullptr, false);
    if (!status.is_object())
        return {false, "Could not read `claude auth status` (exit code " + std::to_string(output.ExitCode) + ")."};

    const auto loggedIn = status.find("loggedIn");
    if (loggedIn == status.end() || !loggedIn->is_boolean() || !loggedIn->get<bool>())
        return {false, "Not logged in: run `claude auth login`."};

    // Only the method: the same output holds the account's email and organization.
    std::string method = JsonFields::String(status, "authMethod");
    if (method.empty())
        method = "unknown";
    if (method == "claude.ai")
        return {true, "Logged in (claude.ai)"};
    return {true, "Logged in (" + method + "): turns are billed through that credential"};
}

CliSessionList ClaudeSessionProvider::ListSessions(const CancelToken& cancel) const
{
    return CliSessionFiles::ReadClaudeSessions(DataDirectory("CLAUDE_CONFIG_DIR", ".claude"), WorkingDirectory(),
                                               cancel);
}

std::string ClaudeSessionProvider::CheckReadiness(const CancelToken& cancel) const
{
    const CommandOutput output = RunCommand({"--version"}, cancel);
    if (!output.Failure.empty())
        return output.Failure;

    const Version minimum = {kMinimumVersion[0], kMinimumVersion[1], kMinimumVersion[2]};
    const std::optional<Version> version = ParseVersion(output.Stdout);
    if (!version)
        return "Could not read the Claude Code version from `claude --version`.";
    if (*version < minimum)
        return "Claude Code " + VersionText(minimum) + " or later is required (found " + VersionText(*version) +
               "): run `claude update`.";

    const LoginStatus login = CheckLogin(cancel);
    return login.LoggedIn ? std::string() : login.Text;
}

void ClaudeSessionProvider::ReadTextDelta(const nlohmann::json& record, TurnState& turn)
{
    const nlohmann::json& event = JsonFields::Object(record, "event");
    if (JsonFields::String(event, "type") != "content_block_delta")
        return;
    const nlohmann::json& delta = JsonFields::Object(event, "delta");
    if (JsonFields::String(delta, "type") != "text_delta")
        return;
    const std::string text = JsonFields::String(delta, "text");
    if (text.empty())
        return;
    turn.Result.Text += text;
    turn.Events.OnTextDelta(text);
}
} // namespace GameEngine
