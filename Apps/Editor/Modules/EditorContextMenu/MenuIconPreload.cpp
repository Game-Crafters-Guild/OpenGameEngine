#include "EditorContextMenu/MenuIconPreload.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

#include <cstdint>

namespace GameEngine::MenuIcons
{

void PreloadEditorIconAssets()
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    uint32_t requested = 0;
    for (const GUID& guid : assets.GetRegistry().GetAssetsByType(AssetType::Texture))
    {
        AssetMetadata meta{};
        if (!assets.GetRegistry().TryGetAssetMetadata(guid, meta))
            continue;

        // The editor's own icon folder, not the project's textures: a project
        // can hold gigabytes of them and none of it reaches a menu row.
        bool isEditorIcon = false;
        for (const auto& part : meta.Path)
        {
            if (part == "Icons")
            {
                isEditorIcon = true;
                break;
            }
        }
        if (!isEditorIcon)
            continue;

        if (const SharedPtr<Asset> loaded = assets.GetAsset(guid);
            loaded && loaded->IsLoaded() && !loaded->HasFailed())
            continue;

        assets.LoadAsset(guid, AssetLoadResultCallback{}, AssetLoadPriority::Low);
        ++requested;
    }

    if (requested > 0)
        Logger::Log::Info("Menu icons: requested {} editor icon assets", requested);
}

} // namespace GameEngine::MenuIcons
