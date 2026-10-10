#include "AssistantTurnUndo.h"

#include "AssistantActionLedger.h"

#include <algorithm>
#include <functional>

namespace GameEngine
{
namespace
{
bool Holds(const std::vector<uint64_t>& stack, uint64_t id)
{
    return std::find(stack.begin(), stack.end(), id) != stack.end();
}

// A step the conversation's assistant made, in any turn.
bool IsAssistantStep(std::span<const AssistantAction> actions, uint64_t id)
{
    return std::any_of(actions.begin(), actions.end(),
                       [id](const AssistantAction& action) { return action.UndoEntryId == id; });
}

AssistantTurnUndo Make(AssistantTurnUndo::State state, std::string tooltip, std::vector<uint64_t> steps)
{
    AssistantTurnUndo undo;
    undo.Status = state;
    undo.Tooltip = std::move(tooltip);
    undo.Steps = std::move(steps);
    return undo;
}
} // namespace

AssistantTurnUndo AssistantTurnUndo::Describe(std::span<const AssistantAction> actions, uint64_t turn,
                                              const AssistantUndoSnapshot& history)
{
    // Newest first, by entry id (ids only grow): two connections of one turn can land their
    // steps in another order than the ledger recorded the calls.
    std::vector<uint64_t> steps;
    for (const AssistantAction& action : actions)
        if (action.Turn == turn && action.UndoEntryId != 0)
            steps.push_back(action.UndoEntryId);
    std::sort(steps.begin(), steps.end(), std::greater<>());
    if (steps.empty())
        return {};

    size_t undone = 0;
    size_t missing = 0;
    for (const uint64_t step : steps)
    {
        if (Holds(history.Redo, step))
            ++undone;
        else if (!Holds(history.Undo, step))
            ++missing;
    }
    if (missing > 0)
    {
        // Ids grow with every push, so a step older than the oldest one kept was trimmed.
        const bool trimmed = !history.Undo.empty() && steps.back() < history.Undo.front();
        if (trimmed)
            return Make(State::TooOld, "Too old to undo as a turn: its first edit is past the undo history's limit.",
                        std::move(steps));
        return Make(State::Gone, "This turn's edits are no longer in the undo history.", std::move(steps));
    }
    if (undone == steps.size())
        return Make(State::Undone, "This turn was undone; Redo (Ctrl+Y) brings its edits back one by one.",
                    std::move(steps));
    if (undone > 0)
        return Make(State::Blocked, "Part of this turn is undone: redo it (Ctrl+Y) or use Undo History.",
                    std::move(steps));

    const bool onTop = history.Undo.size() >= steps.size() &&
                       std::equal(steps.begin(), steps.end(), history.Undo.rbegin());
    if (onTop)
    {
        std::string tooltip = steps.size() == 1 ? "Undo the edit this reply made."
                                                : "Undo the " + std::to_string(steps.size()) +
                                                      " edits this reply made, newest first.";
        return Make(State::Ready, std::move(tooltip), std::move(steps));
    }
    const std::string name = "'" + history.TopName + "'";
    const std::string above = IsAssistantStep(actions, history.Undo.back()) ? name : "Your edit " + name;
    return Make(State::Blocked, above + " came after this turn: undo it first, or use Undo History.", std::move(steps));
}
} // namespace GameEngine
