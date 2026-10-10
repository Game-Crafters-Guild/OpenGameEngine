#include "TerrainECS/TerrainEntityProvisioning.h"

#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainFactory.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace GameEngine::TerrainECS
{

namespace
{

// A Shape::Global volume covers every terrain in the scene at weight 1, so the default rows
// belong to the SCENE, not to one terrain: a second copy would composite on top of the first
// rather than scoping itself to the new terrain.
bool SceneHasGlobalSurfaceRules(ECS::World& world)
{
    bool found = false;
    world.Query<ECS::Read<Components::TerrainModifierVolume>,
                ECS::Read<Components::TerrainSurfaceRulesEffect>>()
        .Each([&](const Components::TerrainModifierVolume& volume,
                  const Components::TerrainSurfaceRulesEffect&) {
            found = found || volume.Shape == Components::TerrainVolumeShape::Global;
        });
    return found;
}

// The default surface rules, as an ORDINARY entity. Nothing marks it, nothing re-creates it once
// deleted, and it is indistinguishable in the scene file from one authored by hand. It appears
// when a terrain is created and never on load, so deleting it is permanent for that scene.
ECS::EntityHandle SpawnDefaultSurfaceRulesVolume(ECS::World& world)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return entity;

    // Bounded copy with an explicit terminator: Name is a fixed char buffer, so the length has to
    // be clamped to it rather than trusted from the label.
    static constexpr char kLabel[] = "Terrain Surface Rules";
    Components::Name name{};
    const std::size_t copied = std::min(sizeof(name.value) - 1u, sizeof(kLabel) - 1u);
    std::memcpy(name.value, kLabel, copied);
    name.value[copied] = '\0';
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, Components::Transform{});

    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    world.AddComponentImmediate(entity, volume);

    world.AddComponentImmediate(entity, MakeDefaultTerrainSurfaceRules());
    return entity;
}

} // namespace

TerrainProvisioningResult ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                                 const Components::Terrain& config,
                                                 const Components::TerrainPlanetRelief& relief)
{
    TerrainProvisioningResult result;
    const bool spherical = config.Domain == Components::TerrainDomain::Spherical;
    if (!spherical)
    {
        const float32 maxExtent = MaxTerrainExtentMetres(config.SamplesPerMeter);
        if (!(config.SizeX > 0.0f && config.SizeX <= maxExtent && config.SizeZ > 0.0f && config.SizeZ <= maxExtent))
        {
            result.Error = std::format("A terrain at {:g} samples per meter is up to {:.0f} m on each side; this one "
                                       "is {:.0f} x {:.0f} m. Make it smaller or lower its samples per meter.",
                                       config.SamplesPerMeter, maxExtent, config.SizeX, config.SizeZ);
            return result;
        }
    }
    // A terrain wider than the per-tile cap auto-tiles: extraction owns its provisioning (unified
    // maps + per-tile colliders), so eager single-heightfield creation here would just be
    // destroyed at the tiling flip and its collider stranded.
    const bool willTile = Terrain::TerrainNeedsTiling(config.SizeX, config.SizeZ, config.SamplesPerMeter);

    if (!world.GetComponent<Components::Terrain>(entity))
        world.AddComponentImmediate(entity, config);
    else
        *world.GetComponentForWrite<Components::Terrain>(entity) = config;

    if (!world.GetComponent<Components::TerrainGrass>(entity))
        world.AddComponentImmediate(entity, Components::TerrainGrass{});

    // A planet's base relief is a companion component; a planar terrain carries none.
    // Re-provisioning an existing spherical entity refreshes it to the passed values.
    if (spherical)
        world.AddComponentImmediate(entity, relief);
    else if (world.GetComponent<Components::TerrainPlanetRelief>(entity))
        world.RemoveComponentImmediate<Components::TerrainPlanetRelief>(entity);

    // Single-tile terrains get eager noise data so they render immediately; tiled terrains are
    // provisioned by the extraction system on the next tick.
    if (!willTile)
    {
        if (auto* terrainService = TerrainService::TryGet())
        {
            const auto terrainConfig = Terrain::TerrainConfig::FromSamplesPerMeter(
                config.SizeX, config.SizeZ, config.HeightScale, config.SamplesPerMeter);
            const auto terrainHandle = CreateDefaultTerrainWithNoise(*terrainService, terrainConfig);
            if (auto* comp = world.GetComponentForWrite<Components::Terrain>(entity))
            {
                comp->TerrainDataHandle = terrainHandle.Index;
                comp->TerrainDataGeneration = terrainHandle.Generation;
            }
        }
    }

    // Heightfield collision is planar, single-tile only. A planet's collider is a queued slice; a
    // tiled terrain uses per-tile colliders.
    if (!spherical && !willTile)
    {
        const auto* terrain = world.GetComponent<Components::Terrain>(entity);

        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        body.mass = 0.0f;
        body.gravityScale = 0.0f;
        world.AddComponentImmediate(entity, body);

        Components::PhysicsCollider collider{};
        collider.layer = Physics::Layers::Static;
        world.AddComponentImmediate(entity, collider);

        Components::HeightFieldColliderShape shape{};
        if (terrain)
        {
            shape.dataHandle = terrain->TerrainDataHandle;
            shape.dataGeneration = terrain->TerrainDataGeneration;
            shape.sizeX = terrain->SizeX;
            shape.sizeZ = terrain->SizeZ;
            shape.heightScale = terrain->HeightScale;
        }
        world.AddComponentImmediate(entity, shape);
    }

    // Rules evaluate on the planar bake only, so a planet gets none: an invisible modifier that
    // cannot fire would be worse than no modifier.
    if (!spherical && !SceneHasGlobalSurfaceRules(world))
        result.SurfaceRulesVolume = SpawnDefaultSurfaceRulesVolume(world);

    return result;
}

} // namespace GameEngine::TerrainECS
