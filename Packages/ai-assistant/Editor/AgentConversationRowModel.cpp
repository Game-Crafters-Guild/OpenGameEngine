#include "AgentConversationRowModel.h"

#include "AiAssistantSettings.h"
#include "AssistantActionLedger.h"
#include "AssistantTurnUndo.h"
#include "Conversation.h"
#include "ConversationMessage.h"
#include "ModelDisplayName.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace GameEngine
{
namespace
{
// Ends a stopped or truncated reply's text where it was cut (U+2026).
constexpr const char* kCutMarker = "\xE2\x80\xA6";
// Leads a truncated reply's status; the size is Conversation::kMaxMessageBytes.
constexpr const char* kTruncatedStatus = "Reply truncated at 64 KB";
static_assert(Conversation::kMaxMessageBytes == 64 * 1024, "kTruncatedStatus names the cap");

std::string ProviderLabel(const std::string& providerId)
{
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::ProviderChoices())
        if (choice.Id == providerId)
            return std::string(choice.Label);
    return providerId;
}

// "Claude (local session) · Opus 5.5 · High": the connection, the model that answered
// (the one asked for until the turn reports it) and the effort asked for, if any.
std::string ReplyHeader(const ConversationMessage& message)
{
    std::string model;
    if (!message.AnsweredModel.empty())
        model = ModelDisplayName(message.AnsweredModel);
    else if (!message.Model.empty())
        model = AiAssistantSettings::ModelLabel(message.ProviderId, message.Model);
    else
        model = "default model";
    std::string header = ProviderLabel(message.ProviderId) + " · " + model;
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::EffortChoices(message.ProviderId))
        if (!message.Effort.empty() && choice.Id == message.Effort)
            header += " · " + std::string(choice.Label);
    return header;
}

// "$0.0123 · 1200 tokens in, 340 out"; the parts the provider reported, empty if none.
std::string Accounting(const ConversationMessage& message)
{
    std::string text;
    if (message.CostUsd > 0.0)
    {
        char cost[32];
        std::snprintf(cost, sizeof(cost), "$%.4f", message.CostUsd);
        text = cost;
    }
    if (message.InputTokens > 0 || message.OutputTokens > 0)
    {
        if (!text.empty())
            text += " · ";
        text += std::to_string(message.InputTokens) + (message.InputTokens == 1 ? " token in, " : " tokens in, ") +
                std::to_string(message.OutputTokens) + " out";
    }
    return text;
}

std::string JoinSentences(const std::string& first, const std::string& second)
{
    if (first.empty())
        return second;
    if (second.empty())
        return first;
    return first + " · " + second;
}

std::string ReplyStatus(const ConversationMessage& message)
{
    switch (message.Status)
    {
    case MessageStatus::Queued:
        return "Queued";
    case MessageStatus::InProgress:
        return "Generating...";
    case MessageStatus::Complete:
        return JoinSentences(message.Notice, Accounting(message));
    case MessageStatus::Failed:
        return message.Notice.empty() ? std::string("Failed") : message.Notice;
    case MessageStatus::Stopped:
        return JoinSentences("Stopped by you", Accounting(message));
    case MessageStatus::Cancelled:
        return "Cancelled before it started";
    }
    return {};
}
} // namespace

AgentConversationRowModel AgentConversationRowModel::From(const ConversationMessage& message,
                                                          std::span<const AssistantAction> actions, uint64_t turn,
                                                          const AssistantUndoSnapshot& history)
{
    AgentConversationRowModel row;
    row.Text = message.Text;
    if (message.Role == MessageRole::User)
    {
        row.Header = "You";
        row.FromUser = true;
        return row;
    }

    row.Header = ReplyHeader(message);
    row.Calls = AgentCallRowModel::For(message.ToolCalls, actions, turn, history);
    row.Asks = std::any_of(row.Calls.begin(), row.Calls.end(), [](const AgentCallRowModel& call) { return call.Asks; });
    row.Status = JoinSentences(ReplyStatus(message), AgentCallRowModel::Counts(message.ToolCalls, actions, turn));
    if (message.Truncated)
        row.Status = JoinSentences(kTruncatedStatus, row.Status);
    row.CanStop = message.Status == MessageStatus::InProgress;
    row.Failed = message.Status == MessageStatus::Failed;
    row.Retried = row.Failed && message.Retried;
    row.Stopped = message.Status == MessageStatus::Stopped;
    // A reply with no text (queued, cancelled, or not yet streaming) has no body line.
    row.ShowsText = !message.Text.empty();
    // A stopped reply's text is as final as a finished one's, so it can be copied.
    row.ShowsReplyActions =
        message.Status == MessageStatus::Complete || (row.Stopped && !message.Text.empty());
    if ((row.Stopped || message.Truncated) && !message.Text.empty())
        row.Text += kCutMarker;
    // Offered once the reply is finished: its turn adds no step after that.
    const AssistantTurnUndo undo = AssistantTurnUndo::Describe(actions, turn, history);
    row.ShowsUndoTurn = undo.Status != AssistantTurnUndo::State::None && !row.CanStop &&
                        message.Status != MessageStatus::Queued;
    row.CanUndoTurn = undo.Status == AssistantTurnUndo::State::Ready;
    row.UndoTurnTooltip = undo.Tooltip;
    return row;
}
} // namespace GameEngine
