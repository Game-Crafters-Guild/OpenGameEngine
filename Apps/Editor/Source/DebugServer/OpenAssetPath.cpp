#include "DebugServer/OpenAssetPath.h"

#include "Assets/AssetManager.h"

namespace GameEngine::Editor
{

std::filesystem::path ResolveOpenAssetPath(const AssetManager& assetManager, const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    if (path.is_absolute())
        return path.lexically_normal();
    return assetManager.ResolveAssetPath(path);
}

} // namespace GameEngine::Editor
