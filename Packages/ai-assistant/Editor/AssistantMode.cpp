#include "AssistantMode.h"

namespace GameEngine
{
namespace
{
constexpr AssistantModeChoice kModes[] = {
    {AssistantMode::ReadOnly, "readOnly", "Read only", "Read", "Looks and answers; changes nothing, not even the view."},
    {AssistantMode::Auto, "auto", "Auto", "Auto",
     "Edits you can undo run; anything that cannot be undone asks you first."},
    {AssistantMode::AskBeforeEveryEdit, "ask", "Ask before every edit", "Ask",
     "Looks and moves the view; asks you before every edit."},
    {AssistantMode::EditFreely, "editFreely", "Edit freely", "Edit",
     "Runs everything allowed without asking; undo still holds each scene edit."},
};
} // namespace

std::span<const AssistantModeChoice> AssistantModeChoices()
{
    return kModes;
}

const AssistantModeChoice& DescribeAssistantMode(AssistantMode mode)
{
    for (const AssistantModeChoice& choice : kModes)
        if (choice.Mode == mode)
            return choice;
    return kModes[0];
}

std::optional<AssistantMode> ParseAssistantMode(std::string_view id)
{
    for (const AssistantModeChoice& choice : kModes)
        if (choice.Id == id)
            return choice.Mode;
    return std::nullopt;
}
} // namespace GameEngine
