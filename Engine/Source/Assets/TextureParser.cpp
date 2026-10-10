#include "Assets/Parsers/TextureParser.h"

#include "Assets/AssetManager.h"
#include "Assets/PackagedTexturePayload.h"
#include "Assets/TextureAsset.h"

namespace GameEngine
{
AssetParseResult TextureParser::Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager)
{
    return AssetParseResult(std::make_shared<TextureAsset>(metadata.Guid, metadata.Path));
}

std::filesystem::path TextureParser::ResolveReadPath(
    const AssetMetadata& metadata, const AssetManager& assetManager) const
{
    const auto payload = ResolvePackagedTexturePayload(assetManager.GetRegistry(), metadata.Guid,
        TextureAsset::GetGpuTranscodeTarget() == TextureGpuTranscodeTarget::BC7);
    return payload.value_or(metadata.Path);
}
} // namespace GameEngine
