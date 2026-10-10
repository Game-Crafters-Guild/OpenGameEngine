#pragma once

#include "ConversationMessage.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
struct ToolActivity;
struct ToolFailure;
struct TurnResult;

/// One AI Assistant conversation, apart from any view of it: the messages in order,
/// each turn a user message followed by the assistant's reply, and the history a
/// provider receives. Messages are only appended or updated in place until Clear(),
/// so a view can keep one row per index. Not thread-safe: the panel's UI thread owns it.
class Conversation
{
public:
    /// Names one turn (a user message and its reply) for as long as the conversation
    /// holds it; never reused, so a report for a cleared turn matches nothing.
    using TurnId = uint64_t;

    /// The most text a message keeps, in bytes. A reply past it keeps its first
    /// kMaxMessageBytes (cut on a character boundary) and is marked Truncated; a
    /// prompt past it is refused before it becomes a message
    /// (AgentConversationController::Send).
    static constexpr size_t kMaxMessageBytes = 64 * 1024;

    /// Appends the user's message and a Queued reply to be answered by `providerId`
    /// with `model` and `effort` (empty: the provider's default).
    TurnId AddTurn(std::string userText, std::string providerId, std::string model, std::string effort,
                   std::chrono::system_clock::time_point now);
    /// The reply of `turn` is being written: Queued becomes InProgress.
    void Start(TurnId turn);
    /// Appends streamed text and tool calls to the reply of `turn`, and marks the reply's
    /// calls `failedToolCalls` names as Failed or Refused (a later report for a call wins); text past kMaxMessageBytes is
    /// dropped and the reply marked Truncated.
    void AppendReply(TurnId turn, std::string_view text, std::span<const ToolActivity> toolActivities,
                     std::span<const ToolFailure> failedToolCalls);
    /// Ends the reply of `turn` with the provider's result: the whole text (capped as
    /// AppendReply caps it), the outcome as its status, the error or note, and the
    /// accounting.
    void Finish(TurnId turn, const TurnResult& result);
    /// Ends the queued reply of `turn` as Cancelled: Stop cleared it before it ran.
    void CancelQueued(TurnId turn);
    /// Marks the failed reply of `turn` as retried: its prompt was sent again.
    void MarkRetried(TurnId turn);
    /// Drops every message.
    void Clear();

    /// Every message, oldest first.
    std::span<const ConversationMessage> Messages() const { return m_Messages; }
    /// The user message of `turn`; nullptr when the conversation no longer holds it.
    const ConversationMessage* UserMessage(TurnId turn) const;
    /// The reply of `turn`; nullptr when the conversation no longer holds it.
    const ConversationMessage* Reply(TurnId turn) const;
    /// The turn the message at `messageIndex` belongs to; 0 (never a turn) when out of range.
    TurnId TurnOf(size_t messageIndex) const;
    /// What a message API resends before `turn`: every earlier turn whose reply is
    /// Complete with text, user message then reply. A failed, stopped or unfinished
    /// turn, and one whose reply has no text, is left out, so the provider never sees
    /// a question without its answer or two user messages in a row.
    std::vector<ConversationMessage> HistoryBefore(TurnId turn) const;
    /// Increases on every change, so a view refreshes only when it moved.
    uint64_t Revision() const { return m_Revision; }

private:
    ConversationMessage* FindReply(TurnId turn);
    size_t IndexOfUser(TurnId turn) const;

    std::vector<ConversationMessage> m_Messages;
    /// The turn of each message, parallel to m_Messages.
    std::vector<TurnId> m_Turns;
    TurnId m_NextTurn = 1;
    uint64_t m_Revision = 0;
};
} // namespace GameEngine
