#pragma once

#include "Assets/AnimationClip.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

class AnimationAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Animation; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".anim", ".animation"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<AnimationClip>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create animation asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "AnimationAssetParser"; }
    int GetPriority() const override { return 100; }

    // Walk the animation JSON and emit a single AnimationSkeleton edge for
    // its skeleton reference. Field precedence:
    //   1. "skeletonGuid" (explicit GUID)
    //   2. "skeletonPath" (explicit path)
    //   3. "skeleton"     (sniffed: GUID-shape → GUID, otherwise → path)
    // Files that aren't JSON or don't carry a skeleton field return true
    // with no edges (cleanly handled, nothing to emit).
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
