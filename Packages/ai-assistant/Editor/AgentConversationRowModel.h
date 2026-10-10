#pragma once

#include "AgentCallRows.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
struct AssistantAction;
struct AssistantUndoSnapshot;
struct ConversationMessage;

/// What one row of the AI Assistant panel shows for a message, apart from the
/// elements that show it.
struct AgentConversationRowModel
{
    /// "You", or the connection and model that answered ("Claude (API) · claude-opus-5-5").
    std::string Header;
    /// The message text; on a reply, what has streamed so far, and on a stopped or
    /// truncated reply followed by an ellipsis where it was cut.
    std::string Text;
    /// The calls a reply made, one row each or folded (AgentCallRowModel::For); empty
    /// without any.
    std::vector<AgentCallRowModel> Calls;
    /// A call of the reply waits for the user's answer: the card is marked.
    bool Asks = false;
    /// The finished reply's turn made undo steps: the row offers Undo this turn.
    bool ShowsUndoTurn = false;
    /// Undo this turn can take them back now (AssistantTurnUndo::Ready).
    bool CanUndoTurn = false;
    /// What Undo this turn does, or why it cannot (AssistantTurnUndo::Tooltip).
    std::string UndoTurnTooltip;
    /// Where a reply stands: "Queued", "Generating...", "Stopped by you", the failure
    /// with its fix, the note on a short reply, the provider's accounting and the calls'
    /// counts ("5 undo steps · 1 refused"), led by "Reply truncated at 64 KB" on a
    /// truncated reply; empty on a user message.
    std::string Status;
    /// The user wrote the message.
    bool FromUser = false;
    /// The reply is being written, so the row offers Stop.
    bool CanStop = false;
    /// The reply has text that will not change (finished, or stopped part-way), so the
    /// row offers the registered reply actions.
    bool ShowsReplyActions = false;
    /// The reply failed: the status reads as an error, and the row offers Retry (its
    /// prompt sent again as a new turn) where a finished reply offers its actions.
    bool Failed = false;
    /// The failed reply was retried: the row says "Retried below" in place of Retry.
    bool Retried = false;
    /// The user stopped the reply part-way; the status is marked.
    bool Stopped = false;
    /// The row has a body: false for a reply with no text (queued, cancelled before
    /// it started, or not yet streaming).
    bool ShowsText = true;

    /// The row for `message`, the message of turn `turn`; `actions` are the calls the
    /// conversation's assistant made to the editor (its ledger), of which the row shows
    /// those of `turn`, their steps read in the editor's undo `history`.
    static AgentConversationRowModel From(const ConversationMessage& message, std::span<const AssistantAction> actions,
                                          uint64_t turn, const AssistantUndoSnapshot& history);
};
} // namespace GameEngine
