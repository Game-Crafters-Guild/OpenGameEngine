#pragma once

#include "UI/Interaction/ContextMenuManipulator.h"

#include <filesystem>
#include <vector>

namespace GameEngine
{
/// The right-click menu of a file a call row shows (its inline image, an asset tile): the Assets
/// panel's items for a file, in its words and with its icons: Open (the editor's open-asset
/// policy, which hands an image to the operating system's application), Show in Explorer
/// (Finder, File Manager; Editor::ShowInFileManagerLabel) and Copy Full Path, each through
/// EditorAssetActions as installed when the item is chosen.
std::vector<ContextMenuManipulator::Item> AgentCallFileMenu(const std::filesystem::path& file);
} // namespace GameEngine
