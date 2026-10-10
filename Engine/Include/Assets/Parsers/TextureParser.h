#pragma once

#include "Assets/ParserRegistry.h"

namespace GameEngine
{
/// Shared texture construction and packaged-payload selection for raster/container parsers.
class TextureParser : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Texture; }
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;
    std::filesystem::path ResolveReadPath(const AssetMetadata& metadata, const AssetManager& assetManager) const override;
};
} // namespace GameEngine
