#include "Terrain/TerrainBakeCacheSave.h"

#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "TerrainECS/TerrainModifierSystem.h"

namespace GameEngine::Editor
{

void RequestTerrainBakeStoreForSave(ECS::World& world, const std::optional<std::filesystem::path>& previousScenePath,
                                    const std::filesystem::path& scenePath, AssetRegistry& registry)
{
    // The system lives on the rendering loop's schedule, the only place holding it.
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    auto* modifiers = systems ? systems->GetSystem<TerrainECS::TerrainModifierSystem>() : nullptr;
    if (!modifiers)
        return;
    const GUID previousScene = previousScenePath ? registry.GetOrCreateAssetGUID(*previousScenePath) : GUID{};
    modifiers->RequestBakeCacheStore(world, previousScene, registry.GetOrCreateAssetGUID(scenePath));
}

} // namespace GameEngine::Editor
