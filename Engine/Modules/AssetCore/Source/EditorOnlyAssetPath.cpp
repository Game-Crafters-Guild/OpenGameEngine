#include "AssetCore/EditorOnlyAssetPath.h"

#include <cctype>
#include <string>

namespace GameEngine
{

bool IsEditorOnlyAssetPath(const std::filesystem::path& relativePath)
{
    constexpr size_t kEditorLength = 6; // "Editor"
    for (const auto& segment : relativePath)
    {
        std::string name = segment.string();
        if (name.size() != kEditorLength)
            continue;
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == "editor")
            return true;
    }
    return false;
}

} // namespace GameEngine
