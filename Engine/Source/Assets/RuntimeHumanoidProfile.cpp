#include "Assets/RuntimeHumanoidProfile.h"
#include "Animation/SkeletonProfile.h"
#include "Assets/AssetManager.h"

namespace GameEngine
{
GUID ResolveRuntimeHumanoidProfile(AssetManager& assets)
{
    const auto source = assets.GetSourceRoot(kAssetSourceAliasEditor).empty()
                            ? kAssetSourceAliasProject
                            : kAssetSourceAliasEditor;
    return assets.ResolveAssetGuid(kRuntimeHumanoidProfilePath, source);
}

std::unique_ptr<Animation::SkeletonProfile> LoadRuntimeHumanoidProfile(AssetManager& assets)
{
    const auto guid = ResolveRuntimeHumanoidProfile(assets);
    AssetMetadata metadata;
    if (guid.IsNull() || !assets.GetRegistry().TryGetAssetMetadata(guid, metadata) ||
        metadata.Type != AssetType::SkeletonProfile)
        return {};
    // PostLoad runs on model decode workers. Waiting on a second decode can
    // starve that same pool; borrowing a resident profile also races reload's
    // in-place payload mutation. This small file needs only a private snapshot.
    auto profile = std::make_unique<Animation::SkeletonProfile>(
        guid, assets.ResolveAssetPath(metadata.Path));
    if (!profile->Load())
        return {};
    return profile;
}
} // namespace GameEngine
