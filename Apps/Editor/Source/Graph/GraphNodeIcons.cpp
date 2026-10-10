#include "Graph/GraphNodeIcons.h"

#include "UI/UIElement.h"

#include <vector>

namespace GameEngine {
namespace GraphNodeIcons {
namespace {

constexpr std::string_view kClassPrefix = "gn-icon--";

} // namespace

std::string IconPathForStem(std::string_view stem)
{
    if (stem.empty())
        return {};
    std::string path = "editor:Icons/GraphNodes/";
    path += stem;
    path += ".svg";
    return path;
}

void ApplyIconClass(UIElement& element, std::string_view stem)
{
    std::vector<std::string> stale;
    for (const std::string& className : element.GetClasses())
    {
        if (className.starts_with(kClassPrefix))
            stale.push_back(className);
    }
    for (const std::string& className : stale)
        element.RemoveClass(className);

    if (stem.empty())
        return;
    std::string className(kClassPrefix);
    className += stem;
    element.AddClass(className);
}

} // namespace GraphNodeIcons
} // namespace GameEngine
