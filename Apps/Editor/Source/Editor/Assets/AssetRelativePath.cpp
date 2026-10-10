#include "Editor/Assets/AssetRelativePath.h"

#include "Assets/AssetManager.h"

namespace GameEngine::Editor
{

std::string TryMakeAssetRelativePathString(const AssetManager& assetManager,
                                           const std::filesystem::path& absolutePath)
{
    if (absolutePath.empty())
        return {};

    auto tryRel = [&](const std::filesystem::path& root) -> std::string
    {
        if (root.empty())
            return {};
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(absolutePath, root, ec);
        if (ec)
            return {};
        const std::string s = rel.generic_string();
        // Reject anything that escapes the root.
        if (s.empty() || s.rfind("..", 0) == 0)
            return {};
        return s;
    };

    if (std::string s = tryRel(assetManager.GetAssetRoot()); !s.empty())
        return s;

    for (const auto& source : assetManager.GetRegisteredSources())
    {
        if (std::string s = tryRel(source.Root); !s.empty())
            return s;
    }

    return {};
}

} // namespace GameEngine::Editor
