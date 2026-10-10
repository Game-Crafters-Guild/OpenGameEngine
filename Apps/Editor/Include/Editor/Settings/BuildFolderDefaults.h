#pragma once

#include <string_view>

namespace GameEngine::Editor
{
/** Default relative folder name for packaged game builds (under workspace). */
inline constexpr std::string_view kDefaultBuildFolderName = "Build Folder";
}
