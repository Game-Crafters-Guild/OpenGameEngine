// ModelThumbnailHandler, part: The PNG cache files: their names and paths, the cache root, and the
// sweep that deletes the model PNGs of other bake versions.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "Engine/Rendering/ShaderGraphMaterial.h"

#include <string_view>
#include <system_error>

namespace GameEngine
{

namespace
{
// Every model PNG name starts with the family prefix; the current bake version
// follows it. Raising the version makes every project bake its model tiles
// again, and RemoveStaleModelCacheFiles deletes the PNGs of other versions.
constexpr std::string_view kModelCacheFileFamily = "modelthumb_";

constexpr std::string_view kModelCacheFilePrefix = "modelthumb_v2_";
} // namespace

bool ModelThumbnailHandler::IsEphemeralPreviewMaterialPath(const std::filesystem::path& path)
{
    return Engine::Renderer::IsGraphLivePreviewMaterialPath(path);
}

std::string ModelThumbnailHandler::MakeMaterialCacheFileName(const GUID& guid, bool previewIblEnabled)
{
    std::string name = "materialthumb_" + guid.ToString();
    if (!previewIblEnabled)
        name += "_noibl";
    name += ".png";
    return name;
}

std::filesystem::path ModelThumbnailHandler::ComputeMaterialCachePath(const GUID& guid,
                                                                      bool previewIblEnabled) const
{
    if (m_DiskCacheRoot.empty())
        return {};
    return m_DiskCacheRoot / MakeMaterialCacheFileName(guid, previewIblEnabled);
}

void ModelThumbnailHandler::SetCacheRoot(const std::filesystem::path& root)
{
    m_DiskCacheRoot = root;
    m_StaleCacheSweepPending = !root.empty();
}

std::string ModelThumbnailHandler::MakeModelCacheFileName(const GUID& guid)
{
    return std::string(kModelCacheFilePrefix) + guid.ToString() + ".png";
}

size_t ModelThumbnailHandler::RemoveStaleModelCacheFiles(const std::filesystem::path& cacheRoot)
{
    size_t removed = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(cacheRoot, ec); !ec && it != std::filesystem::end(it);
         it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (!name.starts_with(kModelCacheFileFamily) || name.starts_with(kModelCacheFilePrefix))
            continue;
        std::error_code removeError;
        if (std::filesystem::remove(it->path(), removeError))
            ++removed;
    }
    return removed;
}

std::filesystem::path ModelThumbnailHandler::ComputeModelCachePath(const GUID& guid) const
{
    if (m_DiskCacheRoot.empty())
        return {};
    return m_DiskCacheRoot / MakeModelCacheFileName(guid);
}

} // namespace GameEngine
