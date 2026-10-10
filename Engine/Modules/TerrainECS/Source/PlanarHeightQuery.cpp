#include "TerrainECS/PlanarHeightQuery.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

bool PlanarHeightQuery::SampleHeight(float32 worldX, float32 worldZ, float32& outY) const
{
    if (Tiled)
    {
        float32 normalized;
        if (!SampleTiledHeightNormalized(*Tiled, worldX, worldZ, normalized))
            return false;
        outY = OriginY + normalized * HeightScale;
        return true;
    }
    if (!Single || SizeX <= 0.0f || SizeZ <= 0.0f)
        return false;

    const float32 u = (worldX - OriginX) / SizeX;
    const float32 v = (worldZ - OriginZ) / SizeZ;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return false;
    outY = OriginY + Single->Heightfield.SampleBilinear(u, v) * HeightScale;
    return true;
}

bool PlanarHeightQuery::ContainsXZ(float32 worldX, float32 worldZ) const
{
    if (!IsValid())
        return false;
    const float32 sizeX = FootprintSizeX();
    const float32 sizeZ = FootprintSizeZ();
    if (sizeX <= 0.0f || sizeZ <= 0.0f)
        return false;
    const float32 originX = FootprintOriginX();
    const float32 originZ = FootprintOriginZ();
    return worldX >= originX && worldX <= originX + sizeX && worldZ >= originZ &&
           worldZ <= originZ + sizeZ;
}

float32 PlanarHeightQuery::FootprintOriginX() const
{
    return Tiled ? Tiled->WorldOriginX : OriginX;
}

float32 PlanarHeightQuery::FootprintOriginZ() const
{
    return Tiled ? Tiled->WorldOriginZ : OriginZ;
}

float32 PlanarHeightQuery::FootprintSizeX() const
{
    return Tiled ? Tiled->Config.WorldSizeX : SizeX;
}

float32 PlanarHeightQuery::FootprintSizeZ() const
{
    return Tiled ? Tiled->Config.WorldSizeZ : SizeZ;
}

float32 PlanarHeightQuery::LatticeSpacingX() const
{
    if (Tiled)
    {
        // Tiles share their edge samples, so the global lattice is uniform at
        // the per-tile spacing.
        const uint32 width = Tiled->Config.TileConfig.HeightmapWidth;
        if (width < 2u || Tiled->Config.TileWorldSize <= 0.0f)
            return 0.0f;
        return Tiled->Config.TileWorldSize / static_cast<float32>(width - 1u);
    }
    if (!Single || Single->Heightfield.GetWidth() < 2u || SizeX <= 0.0f)
        return 0.0f;
    return SizeX / static_cast<float32>(Single->Heightfield.GetWidth() - 1u);
}

float32 PlanarHeightQuery::LatticeSpacingZ() const
{
    if (Tiled)
    {
        const uint32 height = Tiled->Config.TileConfig.HeightmapHeight;
        if (height < 2u || Tiled->Config.TileWorldSize <= 0.0f)
            return 0.0f;
        return Tiled->Config.TileWorldSize / static_cast<float32>(height - 1u);
    }
    if (!Single || Single->Heightfield.GetHeight() < 2u || SizeZ <= 0.0f)
        return 0.0f;
    return SizeZ / static_cast<float32>(Single->Heightfield.GetHeight() - 1u);
}

PlanarHeightQuery ResolvePlanarHeightQuery(ECS::World& world)
{
    PlanarHeightQuery query{};
    bool found = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](const Components::Terrain& terrain, const Components::WorldTransform& xf)
        {
            if (found)
                return;
            found = true;
            if (terrain.Domain == Components::TerrainDomain::Spherical)
                return; // a planet carries no planar heightfield to read

            const float32 centerX = xf.matrix[12];
            const float32 centerZ = xf.matrix[14];
            query.OriginX = centerX - terrain.SizeX * 0.5f;
            query.OriginZ = centerZ - terrain.SizeZ * 0.5f;
            query.OriginY = xf.matrix[13];
            query.SizeX = terrain.SizeX;
            query.SizeZ = terrain.SizeZ;
            query.HeightScale = terrain.HeightScale;

            auto* service = TerrainService::TryGet();
            if (!service)
                return;
            query.Single = service->GetTerrainData(TerrainHandle{
                terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
            query.Tiled = service->GetTiledTerrainData(TiledTerrainHandle{
                terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
            // An allocated-but-empty heightfield is not a surface: sampling it
            // would report the origin altitude everywhere.
            if (query.Single && query.Single->Heightfield.IsEmpty())
                query.Single = nullptr;
        });
    return query;
}

float32 TerrainHorizonTangent(const PlanarHeightQuery& query, float32 eyeX, float32 eyeY,
                              float32 eyeZ, float32 dirX, float32 dirZ, float32 maxDistance,
                              float32 stepMetres)
{
    if (!query.IsValid() || maxDistance <= 0.0f)
        return kNoTerrainHorizon;

    const float32 dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (dirLen < 1e-6f)
        return kNoTerrainHorizon;
    const float32 nx = dirX / dirLen;
    const float32 nz = dirZ / dirLen;

    // The lattice is the resolution limit: a step finer than it interpolates between the same
    // two corners twice. Clamped from below anyway so a degenerate lattice cannot spin here.
    const float32 lattice =
        std::min(query.LatticeSpacingX(), query.LatticeSpacingZ());
    const float32 step = std::max({stepMetres, lattice, 0.25f});

    float32 best = kNoTerrainHorizon;
    bool sampledAny = false;
    for (float32 t = step; t <= maxDistance; t += step)
    {
        float32 height = 0.0f;
        if (!query.SampleHeight(eyeX + nx * t, eyeZ + nz * t, height))
        {
            // Outside the footprint, or a tile that has not streamed in. Neither is ground, and
            // treating an unsampleable station as height 0 would fabricate a horizon out of a
            // streaming hole -- so the march simply learns nothing here and continues.
            continue;
        }
        sampledAny = true;
        const float32 tangent = (height - eyeY) / t;
        if (tangent > best)
            best = tangent;
    }
    return sampledAny ? best : kNoTerrainHorizon;
}

} // namespace GameEngine::TerrainECS
