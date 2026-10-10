#pragma once

#include "AssistantTools.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
/// Where one of the assistant's calls to the editor stands.
enum class AssistantActionState : uint8_t
{
    /// Held unanswered: waiting for the user's answer, or for the user's own edit to end.
    Waiting,
    /// Ran; the editor answered it (or will, for a response that arrives frames later).
    Done,
    /// Ran, and the editor refused it or failed.
    Failed,
    /// Refused before it ran: the conversation's mode, the tool's class, a path outside
    /// the project, no turn running, or no answer in time.
    Refused,
    /// The user answered Deny.
    Declined,
    /// The session ended (Stop, or the assistant's connection closed) while it waited.
    Cancelled,
};

/// What a waiting call waits for.
enum class AssistantWaitReason : uint8_t
{
    None,
    /// The user's Allow, Allow for this turn or Deny.
    Answer,
    /// The user's own interactive edit (a drag) to commit or cancel.
    Edit,
};

/// The user's answer to a call that asks.
enum class AssistantAnswer : uint8_t
{
    None,
    Allow,
    /// Allow this call and every call of the same turn that would ask.
    AllowForTurn,
    Deny,
};

/// One call of the assistant that reached the editor.
struct AssistantAction
{
    /// The debug server's id for the request (DebugRequestGateContext::RequestId).
    uint64_t RequestId = 0;
    /// The connection it came on.
    uint32_t ClientId = 0;
    /// The conversation turn (Conversation::TurnId) that was running; 0 for none.
    uint64_t Turn = 0;
    /// The tool's name ("set_component"), or the method for one no tool sends.
    std::string Tool;
    AssistantToolClass Class = AssistantToolClass::Denied;
    /// The call's arguments as JSON text.
    std::string Arguments;
    /// AssistantTools::Subject of the call.
    std::string Subject;
    AssistantActionState State = AssistantActionState::Waiting;
    /// While Waiting: what it waits for.
    AssistantWaitReason WaitingFor = AssistantWaitReason::None;
    AssistantAnswer Answer = AssistantAnswer::None;
    /// Why it did not run or failed, as the assistant was told; empty otherwise.
    std::string Message;
    /// The undo step the call produced (UndoRedoService entry id); 0 for none.
    uint64_t UndoEntryId = 0;
    /// That step's name in the Edit menu and the Undo History.
    std::string UndoName;
    /// The editor's response as JSON text, cut at kMaxResultBytes; empty before it is
    /// sent and for a response the editor serialized without keeping it.
    std::string Result;
    /// When the call arrived and when the editor answered it.
    std::chrono::system_clock::time_point Requested;
    std::chrono::system_clock::time_point Answered;
    /// When it first waited, on the gate's clock, for the waiting limit.
    std::chrono::steady_clock::time_point WaitingSince;
    /// The ledger's revision when the record last changed (AssistantActionLedger::Revision).
    uint64_t Revision = 0;
};

/// The record of the calls one conversation's assistant made to the editor, in arrival
/// order: the one source of the conversation's call rows, a reply's counts and the
/// undo steps a turn produced. Written by AssistantGate; main thread only.
class AssistantActionLedger
{
public:
    /// The most of a response a record keeps, in bytes.
    static constexpr size_t kMaxResultBytes = 4096;

    void Add(AssistantAction action);
    /// The record of `requestId`, to change; counts as a change. Null when there is none.
    AssistantAction* Edit(uint64_t requestId);
    const AssistantAction* Find(uint64_t requestId) const;
    std::span<const AssistantAction> Actions() const { return m_Actions; }
    /// Moves on every change; never the same value for two ledgers.
    uint64_t Revision() const { return m_Revision; }

private:
    void Touch();

    std::vector<AssistantAction> m_Actions;
    uint64_t m_Revision = 0;
};
} // namespace GameEngine
