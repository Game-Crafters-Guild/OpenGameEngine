#include "AgentCallFileMenu.h"

#include "Editor/Assets/EditorAssetActions.h"
#include "UI/EditorIcons.h"

#include <functional>

namespace GameEngine
{
namespace
{
using FileAction = std::function<void(const std::filesystem::path&)> Editor::EditorAssetActions::*;

// Runs the installed action `action` on `file`; nothing where the editor installed none.
void Run(FileAction action, const std::filesystem::path& file)
{
    if (const auto& run = Editor::GetEditorAssetActions().*action)
        run(file);
}
} // namespace

std::vector<ContextMenuManipulator::Item> AgentCallFileMenu(const std::filesystem::path& file)
{
    using Actions = Editor::EditorAssetActions;
    return {
        {.Path = "Open", .IconPath = EditorIcons::kFolderOpen, .OnActivate = [file] { Run(&Actions::Open, file); }},
        {.Path = Editor::ShowInFileManagerLabel(),
         .IconPath = EditorIcons::kEye,
         .OnActivate = [file] { Run(&Actions::ShowInFileManager, file); }},
        {.Path = "Copy Full Path", .IconPath = EditorIcons::kCopy, .OnActivate = [file] { Run(&Actions::CopyPath, file); }},
    };
}
} // namespace GameEngine
