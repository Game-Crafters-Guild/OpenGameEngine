#include "Assets/PackagedTexturePayload.h"

#include "AssetDatabase/AssetDatabasePaths.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

#include <string>

namespace GameEngine
{
namespace
{
bool IsPayloadFilename(const std::string& filename)
{
    return !filename.empty() && filename.find_first_of("/\\:") == std::string::npos &&
           std::filesystem::path(filename).extension() == ".ktx2";
}
}

std::optional<std::filesystem::path> ResolvePackagedTexturePayload(
    const AssetRegistry& registry, const GUID& guid, bool preferCompressed)
{
    AssetMetadata metadata;
    if (registry.GetDerivedArtifactPolicy(guid).CooksOnMiss ||
        !registry.TryGetAssetMetadata(guid, metadata))
        return std::nullopt;
    std::string portable;
    if (!registry.TryGetMetaValue(metadata.Path, kTexturePackagedPortableMetaKey, portable))
        return std::nullopt;
    const auto root = registry.TryGetCacheRoot(guid);
    std::string compressed;
    registry.TryGetMetaValue(metadata.Path, kTexturePackagedCompressedMetaKey, compressed);
    if (!root || !IsPayloadFilename(portable) || (!compressed.empty() && !IsPayloadFilename(compressed)))
    {
        Logger::Log::Error("TextureAsset[{}]: invalid packaged payload; rebuild the package", metadata.Path.string());
        return std::filesystem::path{};
    }
    const auto directory = *root / AssetDatabase::kTextureCacheDirectoryName;
    std::error_code error;
    if (preferCompressed && !compressed.empty() && std::filesystem::is_regular_file(directory / compressed, error))
        return directory / compressed;
    return directory / portable;
}
} // namespace GameEngine
