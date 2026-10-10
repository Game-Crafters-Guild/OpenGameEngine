#pragma once

#include "CliSessionProvider.h"

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
/// The user's own Codex login, run as `codex exec --json` in a read-only sandbox.
/// `thread.started` reports the session (thread) id, each `agent_message` item is
/// one whole reply message, `turn.completed` ends the turn with its token usage.
/// `error` items are Codex's non-fatal warnings (an unknown configuration key) and
/// do not end the turn; `turn.failed` and a top-level `error` record do. A turn that
/// attaches the editor's tools runs without them while ToolsBlocker() refuses.
class CodexSessionProvider final : public CliSessionProvider
{
public:
    /// The provider's id (Id()).
    static constexpr std::string_view kId = "codex-session";

    using CliSessionProvider::CliSessionProvider;

    std::string_view Id() const override { return kId; }

    /// `codex login status`: its first line, cut before " - " so a masked key the
    /// line may end with is never shown.
    LoginStatus CheckLogin(const CancelToken& cancel) const override;
    /// The sessions under `<CODEX_HOME or ~/.codex>/sessions/` whose directory is the
    /// working directory (CliSessionFiles::ReadCodexSessions).
    CliSessionList ListSessions(const CancelToken& cancel) const override;

    /// The arguments of one turn: `exec` with the JSON, sandbox and directory
    /// options, `--ignore-user-config` and the `-c mcp_servers.<server>.*` entries when
    /// it attaches the editor's tools, `-m <Model>` when the request names one, `resume <SessionId>` when
    /// it continues a session, and `-` (the prompt is read from stdin). Codex has no
    /// option to append to its system prompt, so SystemPrompt is not sent.
    std::vector<std::string> TurnArguments(const AgentTurnRequest& request) const override;

private:
    std::string_view DisplayName() const override { return "Codex"; }
    void ReadLine(std::string_view line, TurnState& turn) const override;
    /// A logged-in CheckLogin(). Codex has no version floor: every flag a turn
    /// passes is in 0.162.
    std::string CheckReadiness(const CancelToken& cancel) const override;
    /// Codex keeps a shell tool in its read-only sandbox, and a shell command that can
    /// open the editor's port reaches it outside the attached server and its checks.
    /// Runs `codex sandbox` in the sandbox mode a turn uses with a `node` command that
    /// connects to 127.0.0.1:`port`: a refused connection is the only passing answer.
    std::string ToolsBlocker(const std::string& node, uint16_t port, const CancelToken& cancel) const override;
    /// `codex exec resume <id>` with an id Codex does not know exits 1 with "no
    /// rollout found for thread id <id>" on stderr (0.162.0-alpha.2).
    bool IsUnknownSessionError(std::string_view stderrTail) const override;
    /// An `item.completed` record: a reply message or a tool call.
    static void ReadItem(const nlohmann::json& item, TurnState& turn);
};
} // namespace GameEngine
