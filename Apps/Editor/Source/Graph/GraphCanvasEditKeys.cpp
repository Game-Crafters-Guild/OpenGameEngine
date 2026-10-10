#include "Graph/GraphCanvasEditKeys.h"

#include "UI/UIElement.h"

namespace GameEngine {

bool GraphCanvasShouldIgnoreEditKeys(UIElement* focused)
{
    for (UIElement* p = focused; p; p = p->GetParent())
    {
        if (p->HasClass("graph-inline-field") ||
            p->HasClass("float-field") ||
            p->HasClass("vector3-field") ||
            p->HasClass("text-field") ||
            p->HasClass("dropdown") ||
            p->HasClass("checkbox") ||
            p->HasClass("toggle"))
            return true;
    }
    return false;
}

} // namespace GameEngine
