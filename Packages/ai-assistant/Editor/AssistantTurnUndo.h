#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
struct AssistantAction;

/// The editor's undo history as the AI Assistant reads it: the entry ids on each stack
/// (UndoRedoService::GetUndoEntryIdAt, GetRedoEntryIdAt) and the name of the step on top.
/// AssistantGate takes it at each editor update; the panel never holds the service.
struct AssistantUndoSnapshot
{
    /// Oldest first; the last is the next step Undo takes back.
    std::vector<uint64_t> Undo;
    /// The next step Redo brings back first.
    std::vector<uint64_t> Redo;
    /// The name of the step on top of the undo stack; empty when it is empty.
    std::string TopName;

    bool operator==(const AssistantUndoSnapshot&) const = default;
};

/// Whether "Undo this turn" can take back the undo steps a conversation's turn made, and
/// why not. Undo stays linear: the turn's steps are undone only when they are exactly the
/// top of the undo stack, newest first, so a user's edit is never undone and steps are
/// never undone out of order.
struct AssistantTurnUndo
{
    enum class State : uint8_t
    {
        /// The turn made no undo step: no button.
        None,
        /// The turn's steps are the top of the undo stack.
        Ready,
        /// Another step sits above them (the user's edit, a later turn's step), or only
        /// part of the turn is undone.
        Blocked,
        /// Every step is on the redo stack: the turn was undone.
        Undone,
        /// The oldest step was trimmed off the history's limit.
        TooOld,
        /// A step was undone and then dropped from the redo stack by a later edit.
        Gone,
    };

    State Status = State::None;
    /// The button's tooltip: what it does, or why it cannot.
    std::string Tooltip;
    /// The turn's steps, newest first, the order Undo takes them.
    std::vector<uint64_t> Steps;

    /// The state of turn `turn`'s steps among `actions` (the conversation's ledger) in
    /// `history`.
    static AssistantTurnUndo Describe(std::span<const AssistantAction> actions, uint64_t turn,
                                      const AssistantUndoSnapshot& history);
};
} // namespace GameEngine
