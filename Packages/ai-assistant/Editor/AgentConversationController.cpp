#include "AgentConversationController.h"

#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "AssistantAttachment.h"
#include "AssistantGate.h"
#include "Providers/CancelToken.h"
#include "Providers/CliSessionProvider.h"
#include "Providers/IAgentProvider.h"

#include "Editor/EditorPaths.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kSystemPrompt =
    "You are the AI Assistant inside a game engine's editor. Answer concisely and practically.";

// The system prompt's line for a turn that has the editor's tools, naming the mode and
// what it lets run, so the model plans for it.
std::string ModeLine(AssistantMode mode)
{
    std::string line = " You can act in the editor through the editor_assistant tools. The conversation's mode "
                       "is " +
                       std::string(DescribeAssistantMode(mode).Label) + ": ";
    switch (mode)
    {
    case AssistantMode::ReadOnly:
        line += "only reads run (state, hierarchy, components, logs, screenshots, the render graph); anything "
                "that changes the scene, the view or the project is refused.";
        break;
    case AssistantMode::Auto:
        line += "reads, view changes, scene edits the user can undo and injected input run; actions that cannot "
                "be undone (saving, project files, play mode, project settings, builds, captures) wait for the "
                "user to allow them.";
        break;
    case AssistantMode::AskBeforeEveryEdit:
        line += "reads and view changes run; every scene edit, injected input and action that cannot be undone "
                "waits for the user to allow it.";
        break;
    case AssistantMode::EditFreely:
        line += "every tool you are given runs, including saving, project files, play mode and project "
                "settings, which cannot be undone as one step.";
        break;
    }
    return line + " A refused call says why; tell the user what to change instead of retrying it.";
}

// The editor's project root, where each local session's records are kept.
std::filesystem::path ProjectRoot()
{
    return Editor::GetCurrentEditorProjectPaths().projectRoot;
}
// Forwards a turn's events from the turn thread into the mailbox. A local session's id
// is recorded at once, so the next turn continues the session even when this one fails
// or is stopped later.
class MailboxTurnEvents final : public AgentTurnEvents
{
public:
    MailboxTurnEvents(TurnMailbox& mailbox, Conversation::TurnId turn, const IAgentProvider& provider)
        : m_Mailbox(mailbox)
        , m_Turn(turn)
        , m_Provider(provider)
    {
    }

    void OnTextDelta(std::string_view text) override
    {
        m_Text.append(text);
        m_Mailbox.PostText(m_Turn, text);
    }
    void OnToolActivity(const ToolActivity& activity) override { m_Mailbox.PostToolActivity(m_Turn, activity); }
    void OnToolFailed(const ToolFailure& failure) override { m_Mailbox.PostToolFailed(m_Turn, failure); }
    void OnSessionId(std::string_view sessionId) override
    {
        AgentSessionState::Get().SetSessionId(m_Provider, std::string(sessionId));
    }
    void OnFinished(const TurnResult& result) override
    {
        m_Finished = true;
        m_Mailbox.PostFinished(m_Turn, result);
    }

    // Ends a turn whose provider threw before reporting how it ended. The reply keeps
    // the text that streamed before the failure, as a provider's own failure would.
    void FailUnfinished(std::string_view what)
    {
        if (m_Finished)
            return;
        TurnResult result;
        result.Outcome = TurnOutcome::Failed;
        result.Text = std::move(m_Text);
        result.Error = "The provider stopped with an unexpected error: " + std::string(what) +
                       ". Send the message again; if it repeats, the editor log has the details.";
        OnFinished(result);
    }

private:
    TurnMailbox& m_Mailbox;
    Conversation::TurnId m_Turn;
    const IAgentProvider& m_Provider;
    std::string m_Text;
    bool m_Finished = false;
};

// Whether `provider` can attach the editor's tools through `node` to `port`, on the
// acting check's detached thread, answered through `answer`. An exception that escapes a
// thread function terminates the process, so a check that throws refuses instead.
void CheckTools(std::promise<std::string> answer, std::shared_ptr<CliSessionProvider> provider, std::string node,
                uint16_t port, std::shared_ptr<CancelToken> cancel)
{
    try
    {
        answer.set_value(provider->ToolsRefusal(node, port, *cancel));
    }
    catch (const std::exception& e)
    {
        answer.set_value(std::string("Checking the editor's tools failed: ") + e.what());
    }
}

// Runs one turn on the turn thread. An exception that escapes a thread function
// terminates the process, so whatever the provider throws ends the turn instead.
void RunProviderTurn(IAgentProvider& provider, const AgentTurnRequest& request, MailboxTurnEvents& events,
                     const CancelToken& cancel)
{
    try
    {
        provider.RunTurn(request, events, cancel);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AI Assistant: provider '{}' threw during a turn: {}", provider.Id(), e.what());
        events.FailUnfinished(e.what());
    }
    catch (...)
    {
        Logger::Log::Error("AI Assistant: provider '{}' threw a non-standard exception during a turn",
                           provider.Id());
        events.FailUnfinished("an unknown exception");
    }
}

// Reads `provider`'s sessions on the session read thread and posts them. An exception
// that escapes a thread function terminates the process, so a failed read posts an
// empty list instead.
void ReadSessions(std::shared_ptr<CliSessionProvider> provider, std::shared_ptr<CancelToken> cancel,
                  std::shared_ptr<TurnMailbox> mailbox)
{
    SessionListUpdate update;
    update.ProviderId = std::string(provider->Id());
    try
    {
        update.List = provider->ListSessions(*cancel);
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AI Assistant: reading the sessions of '{}' threw: {}", provider->Id(), e.what());
    }
    catch (...)
    {
        Logger::Log::Error("AI Assistant: reading the sessions of '{}' threw a non-standard exception",
                           provider->Id());
    }
    mailbox->PostSessionList(std::move(update));
}
} // namespace

// What the controller shares with its turn thread and its session read, alive as long as
// any of them is: the destructor lets the threads end on their own.
struct AgentConversationController::ThreadShared
{
    std::shared_ptr<TurnMailbox> Mailbox = std::make_shared<TurnMailbox>();
    std::mutex HandoffMutex;
    std::condition_variable HandoffReady;
    /// Written by the UI thread, taken by the turn thread; both under HandoffMutex.
    std::shared_ptr<TurnInFlight> Handoff;
    bool ShuttingDown = false;
};

// Shared by the UI thread (which cancels it) and the turn thread (which runs it).
// Owns the history the request's span points into.
struct AgentConversationController::TurnInFlight
{
    Conversation::TurnId Turn = 0;
    std::shared_ptr<IAgentProvider> Provider;
    std::vector<ConversationMessage> History;
    AgentTurnRequest Request;
    CancelToken Cancel;
    /// The editor's MCP server the request attaches, when it does.
    std::optional<AgentToolAttachment> Tools;
};

AgentConversationController::AgentConversationController(ProviderLookup lookup)
    : m_Lookup(std::move(lookup))
    , m_Shared(std::make_shared<ThreadShared>())
    , m_Mode(AiAssistantSettings::LastMode())
{
}

AgentConversationController::~AgentConversationController()
{
    {
        std::lock_guard lock(m_Shared->HandoffMutex);
        m_Shared->ShuttingDown = true;
    }
    if (m_InFlight)
        m_InFlight->Cancel.Cancel();
    m_Shared->HandoffReady.notify_one();
    if (m_SessionReadCancel)
        m_SessionReadCancel->Cancel();
    if (m_ActingCheckCancel)
        m_ActingCheckCancel->Cancel();
    // Never joined: see the class comment. The threads hold what they touch.
    if (m_Thread.joinable())
        m_Thread.detach();
    if (m_SessionReadThread.joinable())
        m_SessionReadThread.detach();
    CloseActing();
}

void AgentConversationController::SetProvider(std::string providerId)
{
    if (providerId == m_ProviderId)
        return;
    if (!m_Conversation.Messages().empty() || m_ContinuedSession)
        NewConversation();
    m_ProviderId = std::move(providerId);
    m_SessionList.reset();
    // The connection's own session, if it continues one, keeps the mode it ran in.
    m_Mode = ModeOfSession(AgentSessionState::Get().SessionId(m_ProviderId));
    FollowActing();
}

void AgentConversationController::SetMode(AssistantMode mode)
{
    m_Mode = mode;
    AiAssistantSettings::SetLastMode(mode);
    if (m_GateSession != 0)
        AssistantGate::Get().SetMode(m_GateSession, mode);
    AgentSessionState::Get().SetSessionMode(m_ProviderId, mode);
}

void AgentConversationController::Answer(uint64_t requestId, AssistantAnswer answer)
{
    if (m_GateSession != 0)
        AssistantGate::Get().Answer(m_GateSession, requestId, answer);
}

std::span<const AssistantAction> AgentConversationController::Actions() const
{
    const AssistantActionLedger* ledger = m_GateSession != 0 ? AssistantGate::Get().Ledger(m_GateSession) : nullptr;
    return ledger ? ledger->Actions() : std::span<const AssistantAction>();
}

const AssistantUndoSnapshot& AgentConversationController::UndoHistory() const
{
    return AssistantGate::Get().UndoHistory();
}

void AgentConversationController::UndoTurn(Conversation::TurnId turn)
{
    if (m_GateSession != 0)
        AssistantGate::Get().RequestUndoTurn(m_GateSession, turn);
}

AgentActionsRevision AgentConversationController::ActionsRevision() const
{
    const AssistantGate& gate = AssistantGate::Get();
    const AssistantActionLedger* ledger = m_GateSession != 0 ? gate.Ledger(m_GateSession) : nullptr;
    return {ledger ? ledger->Revision() : 0, gate.UndoHistoryRevision()};
}

AgentActionsRevision AgentConversationController::TurnRevision(Conversation::TurnId turn) const
{
    AgentActionsRevision revision;
    bool steps = false;
    for (const AssistantAction& action : Actions())
    {
        if (action.Turn != turn)
            continue;
        revision.Ledger = std::max(revision.Ledger, action.Revision);
        steps = steps || action.UndoEntryId != 0;
    }
    if (steps)
        revision.Undo = AssistantGate::Get().UndoHistoryRevision();
    return revision;
}

AssistantMode AgentConversationController::ModeOfSession(std::string_view sessionId) const
{
    return AgentSessionState::Get().SessionMode(m_ProviderId, sessionId).value_or(AiAssistantSettings::LastMode());
}

void AgentConversationController::FollowActing()
{
    if (m_ActingCheckCancel)
        m_ActingCheckCancel->Cancel();
    m_ActingCheckCancel.reset();
    m_ActingCheck = {};
    m_ActingRefusal = AssistantAttachment::Unavailable();
    if (!m_ActingRefusal.empty())
        return;
    std::shared_ptr<CliSessionProvider> provider = std::dynamic_pointer_cast<CliSessionProvider>(m_Lookup(m_ProviderId));
    if (!provider)
        return;
    m_ActingCheckCancel = std::make_shared<CancelToken>();
    std::promise<std::string> answer;
    m_ActingCheck = answer.get_future();
    std::thread(CheckTools, std::move(answer), std::move(provider), AiAssistantSettings::NodeExecutable(),
                Editor::EditorDebugPort(), m_ActingCheckCancel)
        .detach();
}

void AgentConversationController::TakeActingCheck()
{
    if (!m_ActingCheck.valid() || m_ActingCheck.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    m_ActingRefusal = m_ActingCheck.get();
    m_ActingCheckCancel.reset();
}

void AgentConversationController::CloseActing()
{
    if (m_GateSession == 0)
        return;
    AssistantGate::Get().Close(m_GateSession);
    AssistantAttachment::Remove(m_GateSession);
    m_GateSession = 0;
}

void AgentConversationController::AttachTools(TurnInFlight& turn)
{
    if (!turn.Provider->Capabilities().CanActWithTools || !m_ActingRefusal.empty())
        return;
    AssistantGate& gate = AssistantGate::Get();
    if (m_GateSession == 0)
        m_GateSession = gate.Open(m_Mode, ProjectRoot());
    turn.Tools = AssistantAttachment::Write(AiAssistantSettings::NodeExecutable(), Editor::EditorDebugPort(),
                                            gate.Token(m_GateSession), ProjectRoot(), m_GateSession);
    if (!turn.Tools)
    {
        Logger::Log::Warning("AI Assistant: the MCP configuration for this turn could not be written; the turn "
                             "runs without the editor's tools");
        return;
    }
    turn.Request.Tools = &*turn.Tools;
    turn.Request.SystemPrompt += ModeLine(m_Mode);
    gate.StartTurn(m_GateSession, turn.Turn);
}

bool AgentConversationController::Send(std::string text, std::string model, std::string effort)
{
    if (text.size() > Conversation::kMaxMessageBytes)
        return false;
    const Conversation::TurnId turn =
        m_Conversation.AddTurn(std::move(text), m_ProviderId, std::move(model), std::move(effort),
                               std::chrono::system_clock::now());
    m_Queue.push_back(turn);
    return true;
}

void AgentConversationController::Retry(size_t messageIndex)
{
    const auto messages = m_Conversation.Messages();
    if (messageIndex == 0 || messageIndex >= messages.size())
        return;
    const ConversationMessage& reply = messages[messageIndex];
    if (reply.Role != MessageRole::Assistant || reply.Status != MessageStatus::Failed || reply.Retried ||
        reply.ProviderId != m_ProviderId)
        return;
    // AddTurn takes copies, made before it grows the messages these refer to.
    const Conversation::TurnId turn = m_Conversation.AddTurn(messages[messageIndex - 1].Text, m_ProviderId,
                                                             reply.Model, reply.Effort,
                                                             std::chrono::system_clock::now());
    m_Queue.push_back(turn);
    m_Conversation.MarkRetried(m_Conversation.TurnOf(messageIndex));
}

void AgentConversationController::Stop()
{
    for (const Conversation::TurnId turn : m_Queue)
        m_Conversation.CancelQueued(turn);
    m_Queue.clear();
    if (!m_InFlight)
        return;
    m_InFlight->Cancel.Cancel();
    // The turn's calls waiting on their rows are cancelled now, not when its CLI exits.
    if (m_InFlight->Tools && m_GateSession != 0)
        AssistantGate::Get().EndTurn(m_GateSession);
}

void AgentConversationController::NewConversation()
{
    Stop();
    CloseActing();
    m_Conversation.Clear();
    m_ContinuedSession.reset();
    m_SessionLost = false;
    m_Mode = AiAssistantSettings::LastMode();
}

void AgentConversationController::ContinueSession(CliSessionSummary session)
{
    NewConversation();
    m_Mode = ModeOfSession(session.Id);
    AgentSessionState::Get().ResumeSession(m_ProviderId, session.Id);
    m_ContinuedSession = std::move(session);
}

void AgentConversationController::RequestSessionList()
{
    m_SessionList.reset();
    if (m_SessionReadRunning)
    {
        m_SessionReadAgain = true;
        return;
    }
    StartSessionRead();
}

void AgentConversationController::StartSessionRead()
{
    // The same lookup turns use; only a local session keeps a session list.
    std::shared_ptr<CliSessionProvider> provider = std::dynamic_pointer_cast<CliSessionProvider>(m_Lookup(m_ProviderId));
    if (!provider)
    {
        m_SessionList = CliSessionList{};
        return;
    }
    // The previous read has ended (TakeSessionList saw its list); reap its thread.
    if (m_SessionReadThread.joinable())
        m_SessionReadThread.join();
    m_SessionReadCancel = std::make_shared<CancelToken>();
    m_SessionReadRunning = true;
    m_SessionReadAgain = false;
    m_SessionReadThread = std::thread(ReadSessions, std::move(provider), m_SessionReadCancel, m_Shared->Mailbox);
}

void AgentConversationController::TakeSessionList()
{
    std::optional<SessionListUpdate> update = m_Shared->Mailbox->TakeSessionList();
    if (!update)
        return;
    m_SessionReadRunning = false;
    if (m_SessionReadThread.joinable())
        m_SessionReadThread.join();
    // A list read for a connection the user has since left is dropped.
    if (update->ProviderId == m_ProviderId)
        m_SessionList = std::move(update->List);
    if (m_SessionReadAgain)
        StartSessionRead();
}

void AgentConversationController::ForgetLostSession(const IAgentProvider& provider)
{
    AgentSessionState::Get().SetSessionId(provider, {});
    m_ContinuedSession.reset();
    m_SessionLost = true;
}

bool AgentConversationController::Update()
{
    const uint64_t revision = m_Conversation.Revision();
    for (TurnUpdate& update : m_Shared->Mailbox->Take())
    {
        m_Conversation.AppendReply(update.Turn, update.Text, update.ToolActivities, update.FailedToolCalls);
        if (!update.Result)
            continue;
        m_Conversation.Finish(update.Turn, *update.Result);
        if (update.Result->Outcome == TurnOutcome::Succeeded && !update.Result->Model.empty())
            if (const ConversationMessage* reply = m_Conversation.Reply(update.Turn); reply && !reply->Model.empty())
                AiAssistantSettings::SetAnsweredModel(reply->ProviderId, reply->Model, update.Result->Model);
        if (!m_InFlight || m_InFlight->Turn != update.Turn)
            continue;
        if (m_InFlight->Tools && m_GateSession != 0)
            AssistantGate::Get().EndTurn(m_GateSession);
        // The session the turn reported (if any) ran in the mode chosen now, a change
        // made while the turn ran included.
        AgentSessionState::Get().SetSessionMode(m_InFlight->Provider->Id(), m_Mode);
        if (update.Result->Outcome == TurnOutcome::Failed && update.Result->SessionNotFound &&
            !m_InFlight->Request.SessionId.empty())
            ForgetLostSession(*m_InFlight->Provider);
        m_InFlight.reset();
    }
    TakeSessionList();
    TakeActingCheck();
    while (!m_InFlight && !m_Queue.empty())
        StartNextTurn();
    return m_Conversation.Revision() != revision;
}

void AgentConversationController::StartNextTurn()
{
    const Conversation::TurnId turn = m_Queue.front();
    m_Queue.pop_front();
    const ConversationMessage* user = m_Conversation.UserMessage(turn);
    const ConversationMessage* reply = m_Conversation.Reply(turn);
    if (!user || !reply)
        return;

    auto inFlight = std::make_shared<TurnInFlight>();
    inFlight->Turn = turn;
    inFlight->Provider = m_Lookup(reply->ProviderId);
    if (!inFlight->Provider)
    {
        TurnResult refused;
        refused.Error = "Unknown connection '" + reply->ProviderId + "': choose one in Settings > AI Assistant.";
        m_Conversation.Finish(turn, refused);
        return;
    }

    // A message API resends the history; a local session continues its own transcript.
    inFlight->History = m_Conversation.HistoryBefore(turn);
    inFlight->Request.SystemPrompt = kSystemPrompt;
    inFlight->Request.History = inFlight->History;
    inFlight->Request.UserText = user->Text;
    inFlight->Request.Model = reply->Model;
    inFlight->Request.Effort = reply->Effort;
    if (inFlight->Provider->Capabilities().OwnsTranscript)
        inFlight->Request.SessionId = AgentSessionState::Get().SessionId(reply->ProviderId);
    AttachTools(*inFlight);

    m_Conversation.Start(turn);
    m_SessionLost = false;
    // The shared_ptr keeps the provider alive for the whole turn, so a provider the
    // settings replace mid-turn cannot record its session into the new connection
    // (AgentSessionState::SetSessionId matches by instance).
    m_InFlight = inFlight;
    {
        std::lock_guard lock(m_Shared->HandoffMutex);
        m_Shared->Handoff = std::move(inFlight);
    }
    if (!m_Thread.joinable())
        m_Thread = std::thread(RunTurnThread, m_Shared);
    m_Shared->HandoffReady.notify_one();
}

void AgentConversationController::RunTurnThread(std::shared_ptr<ThreadShared> shared)
{
    for (;;)
    {
        std::shared_ptr<TurnInFlight> turn;
        {
            std::unique_lock lock(shared->HandoffMutex);
            shared->HandoffReady.wait(lock, [&shared] { return shared->ShuttingDown || shared->Handoff; });
            if (shared->ShuttingDown)
                return;
            turn = std::move(shared->Handoff);
        }
        MailboxTurnEvents events(*shared->Mailbox, turn->Turn, *turn->Provider);
        RunProviderTurn(*turn->Provider, turn->Request, events, turn->Cancel);
    }
}
} // namespace GameEngine
