#pragma once

#include "Assets/AudioAsset.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

// Audio Asset Parser - handles .wav, .mp3, .ogg, etc.
class AudioAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Audio; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".wav", ".mp3", ".ogg", ".flac", ".aac"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<AudioAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create audio asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "AudioAssetParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
