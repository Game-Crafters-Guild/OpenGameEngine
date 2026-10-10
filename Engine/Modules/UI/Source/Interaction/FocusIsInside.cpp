#include "UI/Interaction/FocusIsInside.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <string>

namespace GameEngine::UI
{

bool FocusIsInside(const UIElement& element)
{
    const UIManager* manager = element.GetOwnerManager();
    if (!manager)
        return false;

    const std::string& focusId = manager->GetFocusedElementId();
    if (focusId.empty())
        return false;

    // Resolve the id from the root exactly as the dispatcher does, then walk
    // up: an id that also exists inside this element is not focus unless it is
    // the element the manager actually resolved.
    UIElement* root = manager->GetRootElement();
    if (!root)
        return false;

    // Depth budget on the ancestor walk, as the event bubble and the hover
    // descendant test carry: a malformed parent cycle must not hang a keystroke.
    constexpr int kMaxDepth = 2048;
    int depth = 0;
    for (const UIElement* el = root->FindById(focusId); el && depth++ < kMaxDepth; el = el->GetParent())
    {
        if (el == &element)
            return true;
    }
    return false;
}

} // namespace GameEngine::UI
