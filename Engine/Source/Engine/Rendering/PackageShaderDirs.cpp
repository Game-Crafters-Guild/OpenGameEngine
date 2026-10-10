#include "Engine/Rendering/PackageShaderDirs.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"

namespace GameEngine
{

std::vector<std::filesystem::path> CollectPackageShaderDirs(const AssetManager& assetManager)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    for (const AssetSourceDesc& source : assetManager.GetRegisteredSources())
    {
        if (source.Alias == kAssetSourceAliasEditor)
            continue;
        std::error_code ec;
        fs::path shadersDir = (source.Root / "Shaders").lexically_normal();
        if (fs::is_directory(shadersDir, ec))
            dirs.push_back(std::move(shadersDir));
    }
    return dirs;
}

std::vector<std::filesystem::path> CollectProjectRoots(const AssetManager& assetManager)
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path projectRoot = assetManager.GetSourceRoot(kAssetSourceAliasProject);
    if (!projectRoot.empty())
        roots.push_back(std::move(projectRoot));
    return roots;
}

std::vector<Rendering::MaterialBuildContext::AssetSourceRoot> CollectAssetSourceRoots(
    const AssetManager& assetManager)
{
    std::vector<Rendering::MaterialBuildContext::AssetSourceRoot> roots;
    for (const AssetSourceDesc& source : assetManager.GetRegisteredSources())
        roots.push_back({source.Alias, source.Root});
    return roots;
}

void AppendStagedModuleShaderDirs(const std::filesystem::path& adapterShaderDir,
                                  std::vector<std::filesystem::path>& packageShaderDirs)
{
    namespace fs = std::filesystem;
    std::error_code dirEc;
    if (!fs::is_directory(adapterShaderDir, dirEc))
        return;

    std::error_code iterEc;
    for (const auto& entry : fs::directory_iterator(adapterShaderDir, iterEc))
    {
        if (iterEc)
            break;
        std::error_code entryEc;
        if (!entry.is_directory(entryEc) || entryEc)
            continue;
        const fs::path moduleRoot = entry.path();
        if (!fs::is_directory(moduleRoot / "Surfaces", entryEc) || entryEc)
            continue;

        bool alreadyPresent = false;
        for (const fs::path& existing : packageShaderDirs)
        {
            if (existing.lexically_normal() == moduleRoot.lexically_normal())
            {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent)
            packageShaderDirs.push_back(moduleRoot.lexically_normal());
    }
}

std::filesystem::path ResolveAdapterShaderDir(const AssetManager& assetManager)
{
    // Directory, not AnyEntry: the result is walked into as a shader root, so a
    // file named Shaders/ in the editor mount must fall through rather than be
    // handed to the composer.
    return assetManager.ResolveAssetPathPreferringSource("Shaders", kAssetSourceAliasEditor,
                                                         AssetPathKind::Directory);
}

} // namespace GameEngine
