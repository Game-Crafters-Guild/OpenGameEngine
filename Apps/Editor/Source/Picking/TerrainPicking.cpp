#include "Picking/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "TerrainECS/TerrainService.h"

namespace GameEngine::Editor::Picking
{

namespace
{

using Mathematics::Vector3;

// The normalized height range a terrain's baked field ACTUALLY spans.
//
// Modifier bakes accumulate straight into raw heightfield samples and never clamp
// (TerrainModifierSystem::ApplyHeightEffectSample), and TerrainRenderFeature uploads the field
// verbatim as R32_FLOAT — so a lowering volume draws ground below normalized 0, and a raise
// draws it above 1. Treating [0,1] as the bound is what silently clipped that ground out of
// the march: below the floor every coarse sample reads as "above the surface" and the cast
// misses outright; above the lid the first sample is already under the surface and the cast
// reports the lid instead of the ground.
//
// O(1) for a single terrain (the CDLOD root node's min/max, re-derived by every bake) and
// O(resident tiles) for a tiled one. The single-terrain scan is the documented fallback for a
// field whose quadtree is not built yet — a transient before the first extraction, when the
// field is still the zero fill.
void ResolveBakedHeightRange(const TerrainECS::TerrainData* data,
                             const TerrainECS::TiledTerrainData* tiled,
                             float32& outMinNorm, float32& outMaxNorm)
{
    // Nothing resolvable: keep the nominal band rather than collapsing the slab to nothing.
    outMinNorm = 0.0f;
    outMaxNorm = 1.0f;

    if (tiled)
    {
        bool any = false;
        for (const auto& entry : tiled->Tiles)
        {
            const auto& tile = entry.second;
            if (!tile || tile->LodState == TerrainECS::TileLodState::Empty ||
                tile->Heightfield.IsEmpty())
                continue;
            outMinNorm = any ? std::min(outMinNorm, tile->CachedMinH) : tile->CachedMinH;
            outMaxNorm = any ? std::max(outMaxNorm, tile->CachedMaxH) : tile->CachedMaxH;
            any = true;
        }
        return;
    }

    if (!data || data->Heightfield.IsEmpty())
        return;

    const uint32 w = data->Heightfield.GetWidth();
    const uint32 h = data->Heightfield.GetHeight();
    if (data->Quadtree.TryGetGlobalHeightRange(w, h, outMinNorm, outMaxNorm))
        return;
    data->Heightfield.GetMinMax(0, 0, static_cast<int32>(w), static_cast<int32>(h),
                                outMinNorm, outMaxNorm);
}

// Ray -> every planar terrain's heightfield footprint, nearest hit. The march walks geometry
// only the planar domain draws: a spherical terrain retains its heightfield but renders a
// displaced sphere instead, so marching it would offer a phantom flat target at the entity
// origin. Spherical terrains are handled by RaycastActivePlanet against what is drawn.
bool RaycastPlanarTerrains(const Mathematics::Ray3D& ray,
                           GameEngine::ECS::World& world,
                           float32 maxDistance,
                           TerrainPickHit& outHit)
{
    // The service owns the heightfields, so without it there is nothing planar to march.
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return false;

    bool    anyHit = false;
    float32 bestT  = maxDistance;
    Vector3 bestPos{};
    Vector3 bestNormal(0.0f, 1.0f, 0.0f);
    ECS::EntityHandle bestEntity{};

    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            if (terrain.Domain != Components::TerrainDomain::Planar)
                return;

            // Resolve either the single-terrain heightfield or the tiled terrain.
            // A tiled terrain has no single TerrainData (extraction zeroes its handle),
            // so the query must select the resident tile that contains each sample —
            // otherwise brush/zone strokes never hit a tiled terrain.
            const TerrainECS::TerrainData* data = terrainService->GetTerrainData(
                TerrainECS::TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration});
            const TerrainECS::TiledTerrainData* tiled = terrainService->GetTiledTerrainData(
                TerrainECS::TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});

            // Neither store resolved: the terrain has no height data yet (extraction has not
            // run for it), so there is no surface to hit and nothing drawn to click.
            const bool singleValid = data && !data->Heightfield.IsEmpty();
            if (!singleValid && !tiled)
                return;

            const float32 centerX = worldXf.matrix[12];
            const float32 centerZ = worldXf.matrix[14];
            const float32 originX = centerX - terrain.SizeX * 0.5f;
            const float32 originZ = centerZ - terrain.SizeZ * 0.5f;
            const float32 originY = worldXf.matrix[13];

            // World-space terrain height at (wx,wz). Returns false outside the terrain
            // footprint (single) or when the containing tile is not resident (tiled).
            auto sampleHeight = [&](float32 wx, float32 wz, float32& outY) -> bool
            {
                if (tiled)
                {
                    float32 h;
                    if (!TerrainECS::SampleTiledHeightNormalized(*tiled, wx, wz, h))
                        return false;
                    outY = originY + h * terrain.HeightScale;
                    return true;
                }
                const float32 u = (wx - originX) / terrain.SizeX;
                const float32 v = (wz - originZ) / terrain.SizeZ;
                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                    return false;
                outY = originY + data->Heightfield.SampleBilinear(u, v) * terrain.HeightScale;
                return true;
            };

            // Terrain world AABB: XZ is the footprint; Y spans the range the baked field
            // actually holds, NOT the nominal [0,1] band — modifier volumes drive samples
            // outside that band and the renderer draws them there (see ResolveBakedHeightRange).
            // A small Y margin keeps a ray grazing the exact peak/trough from being clipped out
            // by fp rounding. The marcher reach is this box regardless of size, so far-side hits
            // on large terrains land.
            constexpr float32 kHeightMargin = 1.0f;
            float32 minNorm = 0.0f;
            float32 maxNorm = 1.0f;
            ResolveBakedHeightRange(singleValid ? data : nullptr, tiled, minNorm, maxNorm);
            // HeightScale orders the two products; take the extremes so a negative scale
            // cannot invert the slab.
            const float32 yA = originY + minNorm * terrain.HeightScale;
            const float32 yB = originY + maxNorm * terrain.HeightScale;
            const Vector3 aabbMin(originX, std::min(yA, yB) - kHeightMargin, originZ);
            const Vector3 aabbMax(originX + terrain.SizeX, std::max(yA, yB) + kHeightMargin,
                                  originZ + terrain.SizeZ);

            // Central-difference heightfield normal at (wx, wz), stepped at the
            // heightfield's own sample spacing. Falls back to +Y wherever a tap
            // leaves the footprint (or the containing tile is not resident).
            auto surfaceNormal = [&](float32 wx, float32 wz) -> Vector3
            {
                const float32 step =
                    1.0f / std::clamp(terrain.SamplesPerMeter, 0.05f, 20.0f);
                float32 hxp, hxn, hzp, hzn;
                if (!sampleHeight(wx + step, wz, hxp) || !sampleHeight(wx - step, wz, hxn) ||
                    !sampleHeight(wx, wz + step, hzp) || !sampleHeight(wx, wz - step, hzn))
                    return Vector3(0.0f, 1.0f, 0.0f);
                // Heightfield y = h(x, z) has normal along (-dh/dx, 1, -dh/dz).
                const float32 inv = 1.0f / (2.0f * step);
                const Vector3 n((hxn - hxp) * inv, 1.0f, (hzn - hzp) * inv);
                return n * (1.0f / std::sqrt(Vector3::Dot(n, n)));
            };

            float32 hitT = 0.0f;
            Vector3 hitPos{};
            if (MarchTerrainSurface(ray, aabbMin, aabbMax, bestT, sampleHeight, hitT, hitPos)
                && hitT < bestT)
            {
                bestT = hitT;
                bestPos = hitPos;
                bestNormal = surfaceNormal(hitPos.x, hitPos.z);
                bestEntity = entity;
                anyHit = true;
            }
        });

    if (!anyHit)
        return false;

    outHit.Entity        = bestEntity;
    outHit.WorldPosition = bestPos;
    outHit.WorldNormal   = bestNormal;
    outHit.Distance      = bestT;
    return true;
}

// The CBT render feature when it is currently drawing a spherical planet. It is the
// authority for both halves of the planet's geometry: GetDomainConfig().PlanetRadius is the
// radius the GPU displaces FROM, and SampleSphereSurfaceHeight is the CPU mirror of the
// height it displaces BY (procedural relief from the active planet tuning + TerrainService's
// editable sculpt layer). Reading the renderer rather than the Terrain component is what
// makes a pick land on the ground the user can see, including mid-edit frames where the
// component has already moved and the drawn geometry has not.
//
// No feature, or a feature not on the spherical domain, means no planet is on screen: there
// is then nothing to pick, and inventing a radius would place hits on a sphere the renderer
// never drew.
const CBTTerrainECS::CBTRenderFeature* ActivePlanetFeature()
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return nullptr;
    const auto* feature = renderServices->GetFeature<CBTTerrainECS::CBTRenderFeature>();
    if (!feature)
        return nullptr;
    const auto& domain = feature->GetDomainConfig();
    if (domain.DomainMode != CBTTerrain::kDomainSpherical || !(domain.PlanetRadius > 0.0f))
        return nullptr;
    return feature;
}

// Ray -> the one spherical terrain, resolved through the SAME active-terrain resolver the
// renderer tunes from and the sculpt brush routes by, so pick, sculpt and draw can never
// disagree about which terrain is the planet. CBT is single-active-terrain by design, so
// this runs once per cast rather than once per entity.
bool RaycastActivePlanet(const Mathematics::Ray3D& ray,
                         GameEngine::ECS::World& world,
                         float32 maxDistance,
                         TerrainPickHit& outHit)
{
    const CBTTerrainECS::CBTRenderFeature* feature = ActivePlanetFeature();
    if (!feature)
        return false;

    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(world);
    if (!entity.IsValid())
        return false;
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || terrain->Domain != Components::TerrainDomain::Spherical)
        return false;

    float32 t = 0.0f;
    Vector3 pos{};
    Vector3 normal{};
    if (!SolvePlanetSurfaceHit(ray, feature->GetDomainConfig().PlanetRadius, maxDistance,
                               [feature](float32 dx, float32 dy, float32 dz) {
                                   return feature->SampleSphereSurfaceHeight(dx, dy, dz);
                               },
                               t, pos, normal))
        return false;

    outHit.Entity        = entity;
    outHit.WorldPosition = pos;
    outHit.WorldNormal   = normal;
    outHit.Distance      = t;
    return true;
}

} // namespace

bool RaycastTerrain(const Mathematics::Ray3D& ray,
                    GameEngine::ECS::World& world,
                    float32 maxDistance,
                    TerrainPickHit& outHit)
{
    bool    anyHit = false;
    float32 bestT  = std::min(maxDistance, std::numeric_limits<float32>::max());
    TerrainPickHit best{};
    TerrainPickHit hit{};

    if (RaycastPlanarTerrains(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }
    // Capped at bestT, so a planar terrain (or, through RaycastScene, a mesh) in front of the
    // planet still wins.
    if (RaycastActivePlanet(ray, world, bestT, hit))
    {
        best   = hit;
        bestT  = hit.Distance;
        anyHit = true;
    }

    if (!anyHit)
        return false;

    outHit = best;
    return true;
}

}
