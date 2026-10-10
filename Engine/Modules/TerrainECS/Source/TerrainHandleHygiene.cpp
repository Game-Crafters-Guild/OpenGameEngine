#include "TerrainECS/TerrainHandleHygiene.h"

#include "TerrainECS/TerrainService.h"

#include "Components/Terrain/Terrain.h"

namespace GameEngine::TerrainECS
{

void ClearUnresolvedTerrainHandles(Components::Terrain& terrain)
{
    auto* service = TerrainService::TryGet();

    if (!service ||
        !service->GetTerrainData(TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration}))
    {
        terrain.TerrainDataHandle = 0;
        terrain.TerrainDataGeneration = 0;
    }

    if (!service || !service->GetTiledTerrainData(
                        TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}))
    {
        terrain.TiledTerrainHandle = 0;
        terrain.TiledTerrainGeneration = 0;
    }
}

} // namespace GameEngine::TerrainECS
