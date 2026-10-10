#pragma once

#include "AssistantActionLedger.h"
#include "AssistantMode.h"
#include "AssistantTurnUndo.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
namespace Editor
{
struct DebugRequestGate;
struct DebugRequestGateContext;
struct DebugRequestVerdict;
class UndoRedoService;
} // namespace Editor

struct AssistantTool;

/// The editor's decision on what an AI Assistant conversation's agent may do. A
/// conversation that attaches the MCP server opens a session here and hands the
/// server its token; the server binds each connection with `assistant_bind {token}`,
/// and from then on every request on that connection is decided by the tool's class
/// (AssistantTools) and the conversation's mode: it runs, is refused, or waits on its
/// row for the user's answer (parked on the debug server until then). Every decided
/// request is recorded in the session's AssistantActionLedger, and an undoable edit runs
/// inside one undo step named for it ("Assistant: Create Entity (Lantern_01)"). At each
/// editor update the gate reads the undo history for the panel and takes back a turn's
/// steps the user asked to undo. Each decided request writes one editor log line. A
/// connection that never bound (a developer's own agent, the test harness) is left
/// alone. Main thread only, like the debug server's request loop and the panel.
class AssistantGate
{
public:
    /// The id of a session: one conversation's attachment.
    using SessionId = uint64_t;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// How long a call waits on its row (for the user's answer, or for the user's own
    /// edit to end) before it is refused. The MCP server's limit for one call is above it.
    static constexpr std::chrono::minutes kWaitLimit{10};

    /// The gate the editor's debug server consults (registered at module load).
    static AssistantGate& Get();

    AssistantGate();
    /// A gate that reads the time from `now` (tests).
    explicit AssistantGate(Clock now);
    ~AssistantGate();
    AssistantGate(const AssistantGate&) = delete;
    AssistantGate& operator=(const AssistantGate&) = delete;

    /// Opens a session in `mode` with a new random token, for a conversation whose
    /// project is at `projectRoot`; no turn runs yet.
    SessionId Open(AssistantMode mode, std::filesystem::path projectRoot);
    /// Ends a session: its token stops binding, its bound connections are refused and its
    /// ledger is dropped.
    void Close(SessionId session);
    /// The token the MCP server binds with; empty for a closed session.
    std::string Token(SessionId session) const;
    /// The mode the session's next request is decided in.
    void SetMode(SessionId session, AssistantMode mode);
    /// The conversation's turn `turn` runs: requests are recorded under it until EndTurn.
    /// Requests are refused while no turn runs (a CLI that outlives Stop).
    void StartTurn(SessionId session, uint64_t turn);
    /// The running turn ended (finished, failed or stopped); its waiting calls are
    /// cancelled at their next offer, or when their connection closes.
    void EndTurn(SessionId session);
    /// The user's answer to the waiting call `requestId` of the running turn, applied at the
    /// call's next offer (the editor's next update). Allow for this turn also lets every
    /// later call of the same tool in the turn run without asking.
    void Answer(SessionId session, uint64_t requestId, AssistantAnswer answer);
    /// The session's record of calls; null for a closed session.
    const AssistantActionLedger* Ledger(SessionId session) const;
    /// Undoes the steps turn `turn` of `session` made at the next editor update, newest
    /// first, when they are then still exactly the top of the undo stack
    /// (AssistantTurnUndo::Ready); otherwise nothing.
    void RequestUndoTurn(SessionId session, uint64_t turn);
    /// The editor's undo history as of the last update.
    const AssistantUndoSnapshot& UndoHistory() const { return m_UndoHistory; }
    /// Moves whenever UndoHistory() changes.
    uint64_t UndoHistoryRevision() const { return m_UndoHistoryRevision; }

    /// The answer to one debug-server request: DebugRequestGate::Before.
    Editor::DebugRequestVerdict Before(const Editor::DebugRequestGateContext& context);
    /// Closes the undo step an admitted edit opened: DebugRequestGate::Handled.
    void Handled(const Editor::DebugRequestGateContext& context);
    /// Records the editor's response: DebugRequestGate::After.
    void After(const Editor::DebugRequestGateContext& context, const nlohmann::json* response);
    /// Reads the undo history and runs a requested turn's undo: DebugRequestGate::Update.
    void Update(Editor::UndoRedoService* undo);
    /// Forgets the binding of a connection that closed and cancels its waiting calls:
    /// DebugRequestGate::ClientClosed.
    void ClientClosed(uint32_t clientId);
    /// This gate as the registry takes it, claiming assistant_bind and
    /// assistant_authorize; `this` must outlive the registration.
    Editor::DebugRequestGate AsDebugRequestGate();

private:
    struct Session
    {
        SessionId Id = 0;
        std::string Token;
        AssistantMode Mode = AssistantMode::Auto;
        /// The tools that act on the machine read and write only inside it.
        std::filesystem::path ProjectRoot;
        /// The running turn; 0 while none runs.
        uint64_t Turn = 0;
        /// The tools the user answered Allow for this turn: their later calls in the running
        /// turn run without asking; other tools still ask.
        std::vector<std::string> AllowedForTurn;
        AssistantActionLedger Ledger;
    };
    class UndoStep;

    Editor::DebugRequestVerdict Bind(uint32_t clientId, const nlohmann::json* params);
    Session* BoundSession(uint32_t clientId);
    /// The user answered Allow for this turn on a call of `tool` in the running turn.
    static bool IsAllowedForTurn(const Session& session, std::string_view tool);
    Editor::DebugRequestVerdict Offer(Session& session, const Editor::DebugRequestGateContext& context);
    Editor::DebugRequestVerdict OfferAgain(Session& session, const AssistantAction& action,
                                           const Editor::DebugRequestGateContext& context);
    Editor::DebugRequestVerdict Admit(Session& session, const AssistantAction& action,
                                      const Editor::DebugRequestGateContext& context);
    Editor::DebugRequestVerdict Settle(Session& session, uint64_t requestId, AssistantActionState state,
                                       std::string message);
    void Log(const Session& session, uint64_t requestId) const;
    void ReadUndoHistory(const Editor::UndoRedoService* undo);
    void UndoRequestedTurn(Editor::UndoRedoService& undo);

    Clock m_Now;
    std::unordered_map<SessionId, Session> m_Sessions;
    /// A bound connection and the session it serves, until the connection closes; the
    /// session may have closed first, and its connections are then refused.
    std::unordered_map<uint32_t, SessionId> m_Bindings;
    SessionId m_NextSession = 1;
    /// The undo step of the admitted edit whose handler runs now; closed by Handled.
    std::unique_ptr<UndoStep> m_OpenStep;
    AssistantUndoSnapshot m_UndoHistory;
    uint64_t m_UndoHistoryRevision = 0;
    /// The turn whose steps the next update undoes.
    struct UndoTurnRequest
    {
        SessionId Session = 0;
        uint64_t Turn = 0;
    };
    std::optional<UndoTurnRequest> m_UndoTurnRequest;
};

/// The verdict for a call of `tool` (null: a method no assistant tool sends) in `mode`,
/// with `arguments` the call's own (null when it has none): Run, Refuse with a message
/// naming the tool and what the user can do, or Park for a call that asks the user first.
Editor::DebugRequestVerdict DecideAssistantCall(AssistantMode mode, std::string_view requested,
                                                const AssistantTool* tool, const nlohmann::json* arguments);

/// The name of the undo step an admitted call of the undoable `tool` with `arguments`
/// records: "Assistant: " and the edit with its subject ("Assistant: Set
/// DirectionalLight (Sun)"): an entity by its name in the edit world, or by its id when
/// the world has no such entity.
std::string AssistantUndoStepName(const AssistantTool& tool, const nlohmann::json* arguments);

/// The editor log line for a decided call of `session`'s assistant: "AI Assistant:
/// conversation <session> turn <turn>: <tool> <subject> -> <ran | failed: error | refused:
/// reason | declined | cancelled | parked>[ undo #<entry id>]". The subject is cut at 120
/// bytes; no line carries more of a call's arguments.
std::string AssistantActionLogLine(AssistantGate::SessionId session, const AssistantAction& action);
} // namespace GameEngine
