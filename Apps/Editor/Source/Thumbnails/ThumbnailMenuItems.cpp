#include "Thumbnails/ThumbnailMenuItems.h"

#include "Editor/Registries/EditorMenuRegistry.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/EditorIcons.h"

#include <filesystem>

namespace GameEngine::Editor
{

void RegisterThumbnailMenuItems(IThumbnailProvider& provider)
{
    EditorMenuRegistry::Get().RegisterDirectoryMenuItem(
        {"Generate Thumbnails", 0, EditorIcons::kCamera,
         [&provider](const std::filesystem::path& folder) { provider.GenerateFolderThumbnails(folder); }});
}

} // namespace GameEngine::Editor
