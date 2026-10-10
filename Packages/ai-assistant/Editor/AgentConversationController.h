#pragma once

#include "AssistantActionLedger.h"
#include "AssistantMode.h"
#include "AssistantTurnUndo.h"
#include "Conversation.h"
#include "TurnMailbox.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace GameEngine
{
class CancelToken;
class IAgentProvider;

/// What a view of the conversation's calls last showed: the ledger's revision and the
/// undo history's (AssistantGate::UndoHistoryRevision), compared as a pair.
struct AgentActionsRevision
{
    uint64_t Ledger = 0;
    uint64_t Undo = 0;

    bool operator==(const AgentActionsRevision&) const = default;
};

/// Runs one panel's conversation: turns wait in a serial queue and run one at a time
/// on the controller's own thread (created with the first turn), never on a JobSystem
/// worker, because a provider's RunTurn blocks for
/// as long as the reply takes. Each turn has its own CancelToken; Stop() ends the
/// running turn and clears the queue. The turn thread reports through a coalescing
/// TurnMailbox that Update() drains on the UI thread. A local connection's session
/// list is read on a second thread and arrives through the same mailbox. A connection
/// that can act with tools gets the editor's MCP server attached to each turn, with an
/// AssistantGate session deciding its requests in the conversation's mode. Every member
/// is called from the UI thread. The destructor never waits for a thread: a package
/// reload destroys the panel inside the new module's DllMain, where no thread can exit
/// (the loader lock), so it cancels its threads' work and lets them end on their own;
/// what they touch afterwards lives in state they share (the old module stays mapped).
class AgentConversationController
{
public:
    /// Returns the provider for a connection id when a turn starts; nullptr for an
    /// unknown id.
    using ProviderLookup = std::function<std::shared_ptr<IAgentProvider>(std::string_view providerId)>;

    explicit AgentConversationController(ProviderLookup lookup);
    /// Stops the running turn and every check or read in flight, and returns without
    /// waiting for their threads.
    ~AgentConversationController();

    AgentConversationController(const AgentConversationController&) = delete;
    AgentConversationController& operator=(const AgentConversationController&) = delete;

    /// The connection the conversation talks to. A different id while the
    /// conversation holds messages starts a new conversation (NewConversation()):
    /// a reply never lands under another provider's turns. Each local session keeps
    /// its session id, so switching back continues it.
    void SetProvider(std::string providerId);
    /// Queues the user's message for the current connection, answered with `model`
    /// and `effort` (empty: the provider's default); it runs once the turns before it
    /// end, with the model and effort it was sent with. Returns
    /// false, queuing nothing, when `text` is longer than Conversation::kMaxMessageBytes:
    /// a prompt is refused whole, never cut.
    bool Send(std::string text, std::string model, std::string effort);
    /// Sends the prompt of the failed reply at `messageIndex` again as a new turn,
    /// answered by the same connection (and so the same local session), model and effort.
    /// The failed reply stays, marked Retried, and is retried once: does nothing for
    /// a retried reply or any other message.
    void Retry(size_t messageIndex);
    /// Ends the running turn (its reply ends Stopped with the text so far) and every
    /// queued turn.
    void Stop();
    /// Stop(), then drops every message; the conversation no longer continues a
    /// chosen session.
    void NewConversation();
    /// NewConversation(), continuing `session`, a session of the current local
    /// connection the user chose: the next turn resumes `session.Id`
    /// (AgentSessionState::ResumeSession, which also stores it for the project). The
    /// CLI's own transcript is not replayed; ContinuedSession() describes it.
    void ContinueSession(CliSessionSummary session);
    /// The session this conversation continues; nullopt for a new conversation, and
    /// once the session could not be continued.
    const std::optional<CliSessionSummary>& ContinuedSession() const { return m_ContinuedSession; }
    /// The last finished turn failed because its session could not be continued: it
    /// carried a session id and the provider said the CLI does not know it
    /// (TurnResult::SessionNotFound). That session was forgotten, in memory and for
    /// the project, so a Retry starts a new one; any other failure keeps it. Cleared
    /// when the next turn starts.
    bool SessionWasLost() const { return m_SessionLost; }

    /// Reads the current connection's sessions on a thread of the controller's own;
    /// Update() takes the list from the mailbox into SessionList(). A connection that
    /// keeps no sessions gets an empty list at once. A request while a read runs
    /// reads again once it ends.
    void RequestSessionList();
    /// The current connection's sessions as last read; nullopt from a request until
    /// its read ends.
    const std::optional<CliSessionList>& SessionList() const { return m_SessionList; }

    /// Once per frame: applies what the turn thread reported since the last call,
    /// takes a session list that arrived, and starts the next queued turn when none
    /// runs. Returns true when the conversation changed.
    bool Update();

    /// What the assistant may do in the editor, from the conversation's next request
    /// (a running turn's included). Stores it as the last-used mode, which a new
    /// conversation starts in, and as the mode of the session the conversation runs.
    void SetMode(AssistantMode mode);
    AssistantMode Mode() const { return m_Mode; }
    /// The user's answer to the conversation's waiting call `requestId`
    /// (AssistantGate::Answer).
    void Answer(uint64_t requestId, AssistantAnswer answer);
    /// Every call the conversation's assistant made to the editor, in arrival order
    /// (AssistantGate's ledger); empty before the first attached turn.
    std::span<const AssistantAction> Actions() const;
    /// The editor's undo history as the gate last read it.
    const AssistantUndoSnapshot& UndoHistory() const;
    /// Undo this turn: takes back the undo steps `turn` made, at the editor's next
    /// update, when they are then still the top of the undo stack (AssistantTurnUndo).
    void UndoTurn(Conversation::TurnId turn);
    /// Moves whenever Actions() or UndoHistory() changes.
    AgentActionsRevision ActionsRevision() const;
    /// The revision of what turn `turn`'s rows show: its records' latest change, and the
    /// undo history's when the turn made undo steps (0 otherwise).
    AgentActionsRevision TurnRevision(Conversation::TurnId turn) const;

    /// Empty when the current connection's turns can act in the editor; otherwise why
    /// not, with the fix. A connection that cannot call tools at all answers empty:
    /// ask its capabilities first.
    const std::string& ActingRefusal() const { return m_ActingRefusal; }
    /// The current connection's check of its tools (Node.js, a CLI's own condition)
    /// is still running; ActingRefusal() has no answer from it yet.
    bool ActingCheckRunning() const { return m_ActingCheck.valid(); }

    const Conversation& GetConversation() const { return m_Conversation; }
    /// A turn is running or queued.
    bool IsBusy() const { return m_InFlight != nullptr || !m_Queue.empty(); }

private:
    struct TurnInFlight;
    struct ThreadShared;

    /// The turn thread's loop: runs each turn handed over until the controller shuts down.
    static void RunTurnThread(std::shared_ptr<ThreadShared> shared);
    void StartNextTurn();
    void TakeSessionList();
    void StartSessionRead();
    void ForgetLostSession(const IAgentProvider& provider);
    void FollowActing();
    void TakeActingCheck();
    void CloseActing();
    void AttachTools(TurnInFlight& turn);
    AssistantMode ModeOfSession(std::string_view sessionId) const;

    ProviderLookup m_Lookup;
    std::string m_ProviderId;
    Conversation m_Conversation;
    std::deque<Conversation::TurnId> m_Queue;
    /// The turn handed to the thread, until Update() applies its result.
    std::shared_ptr<TurnInFlight> m_InFlight;
    /// What the turn thread and the session read share with the controller, alive as long
    /// as any of them is.
    std::shared_ptr<ThreadShared> m_Shared;
    std::thread m_Thread;

    std::optional<CliSessionSummary> m_ContinuedSession;
    bool m_SessionLost = false;

    AssistantMode m_Mode;
    /// The AssistantGate session of this conversation's attached turns; 0 before the
    /// first one and after the conversation ends.
    uint64_t m_GateSession = 0;
    std::string m_ActingRefusal;
    /// The current connection's tools check, run on a detached thread; the future comes
    /// from a promise, so dropping it never waits for the check.
    std::future<std::string> m_ActingCheck;
    std::shared_ptr<CancelToken> m_ActingCheckCancel;

    std::optional<CliSessionList> m_SessionList;
    /// Reads one connection's sessions at a time; joined when its list is taken and
    /// by the destructor, which cancels it first.
    std::thread m_SessionReadThread;
    std::shared_ptr<CancelToken> m_SessionReadCancel;
    bool m_SessionReadRunning = false;
    bool m_SessionReadAgain = false;
};
} // namespace GameEngine
