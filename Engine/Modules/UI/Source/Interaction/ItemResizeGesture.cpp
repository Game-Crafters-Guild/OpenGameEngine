#include "UI/Interaction/ItemResizeGesture.h"

#include "Input/KeyCodes.h"

namespace GameEngine::UI
{
namespace
{
ItemResizeGestureMatcher g_Matcher = nullptr;
} // namespace

void SetItemResizeGestureMatcher(ItemResizeGestureMatcher matcher)
{
    g_Matcher = matcher;
}

bool MatchesItemResizeGesture(int modifiers)
{
    return g_Matcher ? g_Matcher(modifiers) : Input::IsPrimaryShortcutModifier(modifiers);
}

} // namespace GameEngine::UI
