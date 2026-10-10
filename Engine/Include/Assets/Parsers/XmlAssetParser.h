#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/XmlAsset.h"

namespace GameEngine
{

// Generic XML Asset Parser - handles .xml files that are NOT UI layouts
class XmlAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::XML; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".xml"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<XmlAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create XML asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "XmlAssetParser"; }

    int GetPriority() const override { return 20; } // Lower than UILayout sniffing parser
};

} // namespace GameEngine
