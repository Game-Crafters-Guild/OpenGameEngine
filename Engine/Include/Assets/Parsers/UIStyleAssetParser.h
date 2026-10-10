#pragma once

#include "Assets/ParserRegistry.h"
#include "UI/Assets/UIStyleAsset.h"

namespace GameEngine
{

// UI Style Asset Parser - handles .css, .uss files
class UIStyleAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::UIStyle; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".css", ".uss"}; }

    AssetParseResult Parse(const AssetMetadata& metadata [[maybe_unused]],
                           [[maybe_unused]] AssetManager& assetManager) override
    {
        auto asset = std::make_shared<UIStyleAsset>(metadata.Guid, metadata.Path);
        return AssetParseResult(asset);
    }

    std::string GetName() const override { return "UIStyleAssetParser"; }

    int GetPriority() const override { return 100; }

    // Walk the CSS / USS text and emit UIElementImage edges for every
    // `url(...)` reference (background-image, etc.). Mount-prefix forms
    // ("editor:Icons/foo.png", "@editor/Icons/foo.png") have the prefix
    // stripped so the path-form edge resolves through the registry's
    // multi-source ResolvePathTarget.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
