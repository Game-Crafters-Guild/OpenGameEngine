#include "Editor/Assets/EditorAssetActions.h"

#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"

#include <string_view>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

constexpr std::string_view kEnginePrefix = "engine:";

EditorAssetActions s_EditorActions;

} // namespace

const EditorAssetActions& GetEditorAssetActions()
{
    return s_EditorActions;
}

void SetEditorAssetActions(EditorAssetActions actions)
{
    s_EditorActions = std::move(actions);
}

std::string ShowInFileManagerLabel()
{
#if defined(_WIN32)
    return "Show in Explorer";
#elif defined(__APPLE__)
    return "Show in Finder";
#else
    return "Show in File Manager";
#endif
}

void ApplyThumbnailImage(UIElement& element, const std::string& image)
{
    if (image.empty())
    {
        UI::Layout::ClearBackgroundOverride(element);
        return;
    }
    if (image.starts_with(kEnginePrefix))
    {
        UI::Layout::SetBackgroundResourceName(element, image.substr(kEnginePrefix.size()));
        return;
    }
    UI::Layout::SetBackgroundPath(element, image);
}

} // namespace GameEngine::Editor
