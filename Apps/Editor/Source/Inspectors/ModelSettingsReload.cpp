#include "Inspectors/ModelSettingsReload.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Editor/Assets/AsyncAssetHelpers.h"

#include <string>

namespace GameEngine::Editor
{

void ReloadModelForChangedSettings(AssetManager& assets, const GUID& guid, const std::filesystem::path& path)
{
    if (guid.IsNull())
        return;
    // Worker parse + main-thread adopt. DrainCompletedReloads dispatches
    // AssetReloaded (MeshGPURegistry + ModelThumbnailHandler).
    if (assets.RequestAsyncReload(guid))
        return;
    // Not loaded and not loading: its next load reads the new settings. That load starts here and
    // its reload is announced when it lands, from the main thread's poll; an asset owner outlives
    // the frame loop, so there is no element to post to, and no world is written.
    RunWhenAssetLoaded(
        assets, guid, AssetLoadPriority::High, nullptr, nullptr,
        [&assets, guid, source = path.string()]
        {
            if (assets.GetAsset(guid))
                assets.GetEventDispatcher().DispatchEvent(
                    AssetEvent(AssetEventType::AssetReloaded, guid, AssetType::Model, source));
        },
        path.filename().string());
}

} // namespace GameEngine::Editor
