#pragma once

#include "CliSessionProvider.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
/// The user's own Claude Code login, run as `claude -p` with the stream-json output:
/// no built-in tools and no permission prompts; the only MCP server is the editor's,
/// when the turn attaches it (AgentTurnRequest::Tools), so a turn is a conversation
/// plus, at most, the editor's own tools. `system/init` reports the session id and the
/// model that answers, `stream_event` text deltas stream the reply, `result` ends the turn
/// with Claude Code's accounting.
class ClaudeSessionProvider final : public CliSessionProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "claude-session";
    /// The executable the settings default to: the native installer's `claude` on PATH.
    static constexpr std::string_view kDefaultExecutable = "claude";
    /// The oldest Claude Code that has every flag a turn passes (`--permission-prompts`).
    static constexpr uint32_t kMinimumVersion[3] = {2, 1, 259};

    /// The levels `--effort` accepts (claude 2.1.296), lowest first. Claude Code warns
    /// about any other value and ignores it.
    static constexpr std::string_view kEffortLevels[] = {"low", "medium", "high", "xhigh", "max"};

    using CliSessionProvider::CliSessionProvider;

    std::string_view Id() const override { return kId; }

    /// `claude auth status`: the reported authMethod, never the account's email or
    /// organization.
    LoginStatus CheckLogin(const CancelToken& cancel) const override;
    /// The sessions under `<CLAUDE_CONFIG_DIR or ~/.claude>/projects/` for the
    /// working directory (CliSessionFiles::ReadClaudeSessions).
    CliSessionList ListSessions(const CancelToken& cancel) const override;

    /// The arguments of one turn: the fixed flags, `--resume <SessionId>` when the
    /// request continues a session, `--mcp-config <file>` and `--allowedTools` naming
    /// each `mcp__<server>__<tool>` when it attaches the editor's tools, `--model
    /// <Model>` and `--effort <Effort>` when it names them and `--append-system-prompt
    /// <SystemPrompt>` when it has one. A resumed session takes them too: each turn is a
    /// process of its own, so a change applies from the next turn.
    std::vector<std::string> TurnArguments(const AgentTurnRequest& request) const override;

private:
    std::string_view DisplayName() const override { return "Claude Code"; }
    void ReadLine(std::string_view line, TurnState& turn) const override;
    /// A `stream_event` record: its text delta, if it carries one.
    static void ReadTextDelta(const nlohmann::json& record, TurnState& turn);
    /// An `assistant` record: its tool calls.
    static void ReadToolCalls(const nlohmann::json& record, TurnState& turn);
    /// A `user` record: the results of the tool calls, of which the failed ones are reported.
    static void ReadToolResults(const nlohmann::json& record, TurnState& turn);
    static std::string FirstErrorLine(const nlohmann::json& toolResult);
    /// The `result` record: the outcome and the accounting.
    static void ReadResult(const nlohmann::json& record, TurnState& turn);
    /// `claude --version` at or above kMinimumVersion, then a logged-in CheckLogin().
    std::string CheckReadiness(const CancelToken& cancel) const override;
};
} // namespace GameEngine
