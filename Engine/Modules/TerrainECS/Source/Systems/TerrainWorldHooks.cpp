#include "TerrainECS/Systems/TerrainWorldHooks.h"

#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"

#include "Components/Terrain/Terrain.h"
#include "TerrainECS/Components/TerrainPlanetFaceCollider.h"
#include "TerrainECS/Components/TerrainTileCollider.h"
#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::TerrainECS
{

void ReleaseTerrainOwnedResources(Components::Terrain& terrain)
{
    auto* service = TerrainService::TryGet();
    if (!service)
        return;

    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    auto* feature = renderServices ? renderServices->GetFeature<TerrainRenderFeature>() : nullptr;

    // Resolve through the service before releasing. TerrainRenderFeature::
    // ReleaseTerrainResources keys purely on handle.Index and compares no
    // generation, so releasing a stale handle would destroy whichever terrain
    // now occupies the recycled slot — including its live bindless indices.
    const TerrainHandle single{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
    if (service->GetTerrainData(single))
    {
        if (feature)
            feature->ReleaseTerrainResources(single);
        service->DestroyTerrain(single);
    }
    terrain.TerrainDataHandle = 0;
    terrain.TerrainDataGeneration = 0;

    // The tiled path renders through one unified texture set keyed by the tiled
    // terrain's own global GPU handle, not by the (zeroed) single handle above.
    const TiledTerrainHandle tiled{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
    if (const auto* tiledData = service->GetTiledTerrainData(tiled))
    {
        if (feature)
            feature->ReleaseTerrainResources(TerrainHandle{tiledData->GlobalGpuHandleIndex,
                                                           tiledData->GlobalGpuHandleGeneration});
        service->DestroyTiledTerrain(tiled);
    }
    terrain.TiledTerrainHandle = 0;
    terrain.TiledTerrainGeneration = 0;
}

void ReleaseTerrainTileCollider(Components::TerrainTileCollider& collider)
{
    if (auto* service = TerrainService::TryGet())
        service->ReleaseTilePhysicsHandle(collider.PhysicsHandleIndex,
                                          collider.PhysicsHandleGeneration);
    collider.PhysicsHandleIndex = 0;
    collider.PhysicsHandleGeneration = 0;
}

void ReleaseTerrainPlanetFaceCollider(Components::TerrainPlanetFaceCollider& collider)
{
    if (auto* service = TerrainService::TryGet())
        service->ReleasePlanetFacePhysicsHandle(collider.PhysicsHandleIndex,
                                                collider.PhysicsHandleGeneration);
    collider.PhysicsHandleIndex = 0;
    collider.PhysicsHandleGeneration = 0;
}

void RegisterTerrainWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::Terrain>(&ReleaseTerrainOwnedResources);
    world.RegisterOnRemove<Components::TerrainTileCollider>(&ReleaseTerrainTileCollider);
    world.RegisterOnRemove<Components::TerrainPlanetFaceCollider>(
        &ReleaseTerrainPlanetFaceCollider);
}

} // namespace GameEngine::TerrainECS
