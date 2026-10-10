#pragma once

#include "AgentCallRow.h"
#include "AgentConversationRowModel.h"

#include "UI/UIElement.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
class TextArea;
class UIManager;
struct AgentReplyAction;

/// Sets `element`'s height override to `heightPx`. Returns true when the override
/// moved by more than half a pixel, so a caller can tell whether layout has settled.
bool PinHeightPx(UIElement& element, float heightPx);

/// One message of the AI Assistant panel: the layout comes from the package's
/// `Editor/UI/controls/AgentConversationRow.uxml` (header, streamed text, the calls'
/// rows, status, Stop, Retry on a failed reply, the registered reply actions); the row
/// only fills it from an AgentConversationRowModel.
class AgentConversationRow final : public UIElement
{
public:
    /// Called with the action a reply-action button runs.
    using ReplyActionHandler = std::function<void(const AgentReplyAction&)>;

    /// `index` is the message's position in the conversation; it keys the row's
    /// element ids so every row's are unique. `onStop` runs when Stop is clicked,
    /// `onRetry` when Retry is, `onAnswer` when a waiting call is answered and
    /// `onUndoTurn` when Undo this turn is.
    AgentConversationRow(size_t index, std::function<void()> onStop, std::function<void()> onRetry,
                         ReplyActionHandler onReplyAction, AgentCallRow::AnswerHandler onAnswer,
                         std::function<void()> onUndoTurn);

    /// Instantiates the row layout through `ui`. False (and logged) when the layout
    /// is not staged or is missing an element.
    bool Build(UIManager& ui);
    /// Shows `model`, touching only what changed since the last call. `time` is the
    /// message's local time of day.
    void Show(const AgentConversationRowModel& model, const std::string& time);
    /// Pins the text to the height its wrapped lines need, sizes the calls' inline images to
    /// the row's width, and pins the row to its content (a TextArea grows its own height and
    /// never shrinks it). Returns true while a
    /// height still moved, so the caller can wait for the rows to settle.
    bool FitHeight();
    /// Redraws the text; the text area culls lines to the scroll view's viewport, so
    /// the panel calls this when the history scrolls.
    void RefreshTextVisuals();
    /// The Allow button of a call of this reply that waits for the user's answer; null
    /// when none waits.
    UIElement* AnswerFocus() const;
    /// Stacks every call row for a narrow panel (AgentCallRow::SetStacked), later ones too.
    void SetCallsStacked(bool stacked);

private:
    void BuildReplyActions();
    void ShowStatus(const AgentConversationRowModel& model);
    void ShowCalls(const std::vector<AgentCallRowModel>& calls);

    size_t m_Index = 0;
    std::function<void()> m_OnStop;
    std::function<void()> m_OnRetry;
    ReplyActionHandler m_OnReplyAction;
    AgentCallRow::AnswerHandler m_OnAnswer;
    std::function<void()> m_OnUndoTurn;
    Label* m_Header = nullptr;
    Label* m_Time = nullptr;
    Button* m_Stop = nullptr;
    Button* m_Retry = nullptr;
    Button* m_UndoTurn = nullptr;
    Label* m_Retried = nullptr;
    TextArea* m_Text = nullptr;
    UIElement* m_Calls = nullptr;
    /// One per shown call row, in order.
    std::vector<AgentCallRow*> m_CallRows;
    bool m_CallsStacked = false;
    /// One label per part of the status ("5 undo steps", "1 failed"), so the footer wraps
    /// between parts and never inside one.
    UIElement* m_Status = nullptr;
    UIElement* m_Actions = nullptr;
    bool m_ReplyActionsBuilt = false;
    /// What the elements show now; nullopt before the first Show().
    std::optional<AgentConversationRowModel> m_Shown;
};
} // namespace GameEngine
