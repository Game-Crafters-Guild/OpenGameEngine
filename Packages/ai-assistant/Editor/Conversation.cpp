#include "Conversation.h"

#include "Providers/IAgentProvider.h"

#include "UI/Utf8Helpers.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
MessageStatus StatusOf(TurnOutcome outcome)
{
    switch (outcome)
    {
    case TurnOutcome::Succeeded:
        return MessageStatus::Complete;
    case TurnOutcome::Failed:
        return MessageStatus::Failed;
    case TurnOutcome::Stopped:
        return MessageStatus::Stopped;
    }
    return MessageStatus::Failed;
}

// Appends `text` to `message` while it stays within the cap. The first byte past the
// cap ends the message: it is cut back to the last whole character and marked
// Truncated, and nothing more is appended to it.
void AppendCapped(ConversationMessage& message, std::string_view text)
{
    if (message.Truncated)
        return;
    const size_t room = Conversation::kMaxMessageBytes - message.Text.size();
    message.Text.append(text.substr(0, room + 1));
    if (message.Text.size() <= Conversation::kMaxMessageBytes)
        return;
    message.Text.resize(Utf8::Prev(message.Text, Conversation::kMaxMessageBytes + 1));
    message.Truncated = true;
}
} // namespace

Conversation::TurnId Conversation::AddTurn(std::string userText, std::string providerId, std::string model,
                                           std::string effort, std::chrono::system_clock::time_point now)
{
    const TurnId turn = m_NextTurn++;

    ConversationMessage user;
    user.Role = MessageRole::User;
    AppendCapped(user, userText);
    user.Time = now;
    m_Messages.push_back(std::move(user));
    m_Turns.push_back(turn);

    ConversationMessage reply;
    reply.Role = MessageRole::Assistant;
    reply.ProviderId = std::move(providerId);
    reply.Model = std::move(model);
    reply.Effort = std::move(effort);
    reply.Time = now;
    reply.Status = MessageStatus::Queued;
    m_Messages.push_back(std::move(reply));
    m_Turns.push_back(turn);

    ++m_Revision;
    return turn;
}

void Conversation::Start(TurnId turn)
{
    ConversationMessage* reply = FindReply(turn);
    if (!reply || reply->Status != MessageStatus::Queued)
        return;
    reply->Status = MessageStatus::InProgress;
    ++m_Revision;
}

void Conversation::AppendReply(TurnId turn, std::string_view text, std::span<const ToolActivity> toolActivities,
                               std::span<const ToolFailure> failedToolCalls)
{
    ConversationMessage* reply = FindReply(turn);
    if (!reply || (text.empty() && toolActivities.empty() && failedToolCalls.empty()))
        return;
    AppendCapped(*reply, text);
    for (const ToolActivity& activity : toolActivities)
        reply->ToolCalls.push_back({activity.Name, activity.InputJson, activity.Id});
    for (const ToolFailure& failure : failedToolCalls)
    {
        for (ToolCall& call : reply->ToolCalls)
        {
            if (call.Id.empty() || call.Id != failure.CallId)
                continue;
            call.End = failure.Refused ? ToolCallEnd::Refused : ToolCallEnd::Failed;
            if (!failure.Error.empty())
                call.Error = failure.Error;
        }
    }
    ++m_Revision;
}

void Conversation::Finish(TurnId turn, const TurnResult& result)
{
    ConversationMessage* reply = FindReply(turn);
    if (!reply)
        return;
    reply->Text.clear();
    reply->Truncated = false;
    AppendCapped(*reply, result.Text);
    reply->Status = StatusOf(result.Outcome);
    reply->Notice = result.Outcome == TurnOutcome::Failed ? result.Error : result.Note;
    reply->AnsweredModel = result.Model;
    reply->CostUsd = result.CostUsd;
    reply->InputTokens = result.InputTokens;
    reply->OutputTokens = result.OutputTokens;
    ++m_Revision;
}

void Conversation::CancelQueued(TurnId turn)
{
    ConversationMessage* reply = FindReply(turn);
    if (!reply || reply->Status != MessageStatus::Queued)
        return;
    reply->Status = MessageStatus::Cancelled;
    ++m_Revision;
}

void Conversation::MarkRetried(TurnId turn)
{
    ConversationMessage* reply = FindReply(turn);
    if (!reply || reply->Status != MessageStatus::Failed || reply->Retried)
        return;
    reply->Retried = true;
    ++m_Revision;
}

void Conversation::Clear()
{
    m_Messages.clear();
    m_Turns.clear();
    ++m_Revision;
}

const ConversationMessage* Conversation::UserMessage(TurnId turn) const
{
    const size_t index = IndexOfUser(turn);
    return index < m_Messages.size() ? &m_Messages[index] : nullptr;
}

const ConversationMessage* Conversation::Reply(TurnId turn) const
{
    const size_t index = IndexOfUser(turn);
    return index + 1 < m_Messages.size() ? &m_Messages[index + 1] : nullptr;
}

Conversation::TurnId Conversation::TurnOf(size_t messageIndex) const
{
    return messageIndex < m_Turns.size() ? m_Turns[messageIndex] : 0;
}

std::vector<ConversationMessage> Conversation::HistoryBefore(TurnId turn) const
{
    std::vector<ConversationMessage> history;
    const size_t end = std::min(IndexOfUser(turn), m_Messages.size());
    for (size_t index = 0; index + 1 < end; index += 2)
    {
        const ConversationMessage& reply = m_Messages[index + 1];
        if (reply.Status != MessageStatus::Complete || reply.Text.empty())
            continue;
        history.push_back(m_Messages[index]);
        history.push_back(m_Messages[index + 1]);
    }
    return history;
}

ConversationMessage* Conversation::FindReply(TurnId turn)
{
    const size_t index = IndexOfUser(turn);
    return index + 1 < m_Messages.size() ? &m_Messages[index + 1] : nullptr;
}

// Messages come in pairs, user then reply, and turn ids increase with the index, so
// the first entry of a turn found by binary search is its user message.
size_t Conversation::IndexOfUser(TurnId turn) const
{
    const auto it = std::lower_bound(m_Turns.begin(), m_Turns.end(), turn);
    if (it == m_Turns.end() || *it != turn)
        return m_Messages.size();
    return static_cast<size_t>(it - m_Turns.begin());
}
} // namespace GameEngine
