#include "DebugServer/ElementPointerTarget.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/InjectedInput.h"
#include "UI/UIElement.h"

namespace GameEngine::Editor
{

bool ResolveElementPointerTarget(UIElement* root, const std::string& elementId, float& x, float& y,
                                 nlohmann::json& error)
{
    if (!root)
    {
        error = RefuseRequest("No root element");
        return false;
    }

    UIElement* target = root->FindById(elementId);
    if (!target)
    {
        error = RefuseRequest("Element not found: " + elementId);
        return false;
    }

    // Refused only when the box has collapsed on BOTH axes. A box that is flat on one axis
    // is still a real target: a splitter is zero-wide and full-height, its centre lands on
    // the line the user grabs, and hit-testing accepts a point on that edge — elementId
    // drags on splitters resolve through here and work.
    const float width = target->GetLayoutWidth();
    const float height = target->GetLayoutHeight();
    if (!(width > 0.0f) && !(height > 0.0f))
    {
        error = RefuseRequest("Element '" + elementId + "' has no layout box (" + std::to_string(width) + "x" +
                                  std::to_string(height) + "), so it occupies no pixels and cannot be pointed at. It is "
                                  "hidden, not laid out yet, or a host whose content is mounted under a different id. "
                                  "Target a visible descendant instead, or pass explicit x/y coordinates.");
        return false;
    }

    const float centreX = target->GetLayoutX() + width * 0.5f;
    const float centreY = target->GetLayoutY() + height * 0.5f;
    // A finite size says nothing about the origin, and the sibling coordinate gate only sees
    // numbers a request carried, not ones resolved from a layout box. Injection re-checks and
    // then drops silently, which loses the attribution: refused here, it still names the id.
    if (!IsInjectablePointerPosition(centreX, centreY))
    {
        error = RefuseRequest("Element '" + elementId + "' resolved to a non-finite point (" +
                                  std::to_string(centreX) + ", " + std::to_string(centreY) +
                                  "). Its layout is not in a usable state; pass explicit x/y "
                                  "coordinates instead.");
        return false;
    }

    x = centreX;
    y = centreY;
    return true;
}

} // namespace GameEngine::Editor
