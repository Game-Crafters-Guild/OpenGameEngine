#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace GameEngine
{
/// What a conversation's assistant may do in the editor, chosen per conversation beside
/// Session…. Each mode lets a set of tool classes run (AssistantGate decides).
enum class AssistantMode : uint8_t
{
    /// Reads only: nothing changes, not even the view.
    ReadOnly,
    /// Reads, view changes, edits the user can undo and injected input run; an action
    /// that cannot be undone waits for the user's answer.
    Auto,
    /// Reads and view changes run; every edit, injected input and action that cannot be
    /// undone waits for the user's answer.
    AskBeforeEveryEdit,
    /// Everything the assistant is allowed runs without asking.
    EditFreely,
};

/// One mode as the control and the settings page show it.
struct AssistantModeChoice
{
    AssistantMode Mode;
    /// Stored in settings (`aiAssistant.lastMode`) and in a session's record.
    std::string_view Id;
    std::string_view Label;
    /// One word for the control on a line too narrow for Label (ModeControlLayoutFor).
    std::string_view CompactLabel;
    /// One line under the label in the control's list.
    std::string_view Description;
};

/// Every mode, in the order the control lists them.
std::span<const AssistantModeChoice> AssistantModeChoices();
/// The choice describing `mode`.
const AssistantModeChoice& DescribeAssistantMode(AssistantMode mode);
/// The mode stored as `id`; nullopt for an id no mode has.
std::optional<AssistantMode> ParseAssistantMode(std::string_view id);
} // namespace GameEngine
