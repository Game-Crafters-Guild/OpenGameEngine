#include "Placement/SplineSurfaceConform.h"

#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Mathematics/Ray.h"
#include "Picking/MeshPickingService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "Types/Types.h"

#include <cstring>

namespace GameEngine::Editor
{
namespace
{

// Conform rays start this far above the sample and probe straight down.
constexpr float32 kConformRayLift = 100.0f;
constexpr float32 kConformRayMaxDistance = 1000.0f;

// Order-sensitive 64-bit fold. A change detector needs only "a different input
// gives a different digest"; the query order that feeds it is stable within a
// frame, which is all the ordering this relies on.
void FoldRevision(uint64& digest, uint64 value)
{
    digest ^= value + 0x9E3779B97F4A7C15ull + (digest << 6) + (digest >> 2);
}

// A float's bits, so the fold is a change DETECTOR rather than a comparison:
// NaN is equal to itself here (a float compare would re-arm forever) and +0/-0
// stay distinguishable. Mirrors TerrainModifierSystem's own terrain-state hash
// over the same fields.
uint64 FloatBits(float32 value)
{
    uint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint64>(bits);
}

} // namespace

std::vector<ECS::EntityHandle> CollectConformRayIgnoreList(ECS::World& world)
{
    std::vector<ECS::EntityHandle> ignore;
    // Generated geometry is cleaned up whatever its enable state.
    world.Query<ECS::Read<Components::RuntimeOnlyEntity>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle e, const Components::RuntimeOnlyEntity&)
        {
            ignore.push_back(e);
        });
    return ignore;
}

void ConformMissTally::Record(bool hit, const Mathematics::Vector3& sample)
{
    ++Probes;
    if (hit)
        return;
    if (Misses == 0u)
        FirstMiss = sample;
    ++Misses;
}

bool ConformRayDown(ECS::World& world, const Mathematics::Vector3& sample,
                    std::span<const ECS::EntityHandle> ignore,
                    Mathematics::Vector3& outHit,
                    Mathematics::Vector3* outNormal,
                    ConformMissTally* tally,
                    Components::SplineConformTarget target)
{
    Mathematics::Ray3D ray;
    ray.origin = Mathematics::Vector3(sample.x, sample.y + kConformRayLift, sample.z);
    ray.direction = Mathematics::Vector3(0.0f, -1.0f, 0.0f);

    Picking::PickOptions options;
    options.MaxDistance = kConformRayMaxDistance;
    options.IgnoreEntities = ignore;
    // The conformed geometry is authored from these hits and is rebuilt only when the ground
    // revision moves, which a BVH being published does not do: a bounds hit would stay.
    options.WaitForMeshBvh = true;
    if (target == Components::SplineConformTarget::TerrainOnly)
    {
        options.IncludeMeshes = false;
        options.IncludePrimitives = false;
        // Also the bounds fallback, which answers for renderers with no CPU mesh
        // data and is tested BEFORE the mesh/primitive filters — clearing the two
        // flags above happens to skip the whole traversal today, but this states
        // "terrain, nothing else" here rather than resting on that.
        options.IncludeBoundsFallback = false;
    }
    const Picking::PickResult result = Picking::RaycastScene(ray, world, options);
    if (tally)
        tally->Record(result.Hit, sample);
    if (!result.Hit)
        return false;
    outHit = result.Best.WorldPosition;
    if (outNormal)
        *outNormal = result.Best.WorldNormal;
    return true;
}

void ReportConformMisses(const ConformMissTally& tally, uint32 entityId,
                         uint32& lastReportedMisses)
{
    if (tally.Misses == lastReportedMisses)
        return;
    lastReportedMisses = tally.Misses;
    if (tally.Misses == 0u)
        return;

    Logger::Log::Warning(
        "SplineConform: entity {} found no ground under {} of {} samples — those keep their "
        "authored altitude, so pieces there float clear of the surface. First at "
        "({:.2f}, {:.2f}, {:.2f}). Each ray starts {:.0f} m above its sample and probes {:.0f} m "
        "down: check the spline lies inside the terrain footprint and that the ground under it "
        "is within that reach.",
        entityId, tally.Misses, tally.Probes,
        tally.FirstMiss.x, tally.FirstMiss.y, tally.FirstMiss.z,
        kConformRayLift, kConformRayMaxDistance);
}

uint64 ConformSurfaceRevision(ECS::World& world,
                              const TerrainECS::TerrainModifierSystem* modifierSystem)
{
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return 0u;

    const uint64 appliedGroundRevision =
        modifierSystem ? modifierSystem->GetAppliedGroundRevision() : 0u;

    uint64 revision = 0u;
    FoldRevision(revision, terrainService->SphereSculptVersion());

    // The same query TerrainPicking marches, so the digest covers exactly the
    // terrains a conform ray can hit: a terrain the pick ignores must not be
    // able to schedule a rebuild.
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {

            // Every input TerrainPicking turns into a world-space hit besides the
            // samples themselves (TerrainPicking.cpp: the footprint origin comes
            // from the translation and Size, the altitude from originY and
            // HeightScale). The heightfield version cannot stand in for these: a
            // modifier-free terrain never bakes, so dragging or rescaling one
            // moves every hit while its version sits still.
            //
            // Bit patterns, not float compares — the same treatment the modifier
            // system's own per-frame terrain-state hash gives these fields. An
            // edit that lands on a bit-identical value is not an edit.
            FoldRevision(revision, FloatBits(worldXf.matrix[12]));
            FoldRevision(revision, FloatBits(worldXf.matrix[13]));
            FoldRevision(revision, FloatBits(worldXf.matrix[14]));
            FoldRevision(revision, FloatBits(terrain.SizeX));
            FoldRevision(revision, FloatBits(terrain.SizeZ));
            FoldRevision(revision, FloatBits(terrain.HeightScale));
            FoldRevision(revision, static_cast<uint64>(terrain.Domain));

            // Identity as well as version: destroying and re-creating a terrain
            // restarts its version at 0, and the ground under the pieces has
            // certainly moved when that happens.
            FoldRevision(revision, entity.id);
            FoldRevision(revision, (static_cast<uint64>(terrain.TerrainDataGeneration) << 32) |
                                       terrain.TerrainDataHandle);
            FoldRevision(revision, (static_cast<uint64>(terrain.TiledTerrainGeneration) << 32) |
                                       terrain.TiledTerrainHandle);

            if (const auto* data = terrainService->GetTerrainData(TerrainECS::TerrainHandle{
                    terrain.TerrainDataHandle, terrain.TerrainDataGeneration}))
                FoldRevision(revision, data->HeightfieldVersion);

            if (const auto* tiled =
                    terrainService->GetTiledTerrainData(TerrainECS::TiledTerrainHandle{
                        terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}))
            {
                // The per-tile versions below cover only what is RESIDENT, and a
                // consumer that reads composed ground reads past that. An edit
                // off-camera moves no resident tile, so without this term the
                // digest sits still while the composed bed moves — the water stays
                // on a stale bed, then its chunks retire in front of the camera the
                // moment those tiles finally arrive and bump their versions.
                FoldRevision(revision, appliedGroundRevision);

                for (const auto& entry : tiled->Tiles)
                {
                    const auto& tile = entry.second;
                    if (!tile || tile->LodState == TerrainECS::TileLodState::Empty)
                        continue;
                    // The coord travels with the version: a tile evicted and
                    // another arriving at the same version must not cancel out.
                    FoldRevision(revision, (static_cast<uint64>(
                                                static_cast<uint32>(entry.first.X)) << 32) |
                                               static_cast<uint32>(entry.first.Z));
                    FoldRevision(revision, tile->HeightfieldVersion);
                }
            }
        });

    return revision;
}

uint64 ObservedSurfaceRevision(bool readsSurface, uint64 worldDigest)
{
    return readsSurface ? worldDigest : 0u;
}

ConformSurfaceReadiness ConformSurfaceState(ECS::World& world)
{
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return ConformSurfaceReadiness::NoSurface;

    bool anyTerrain = false;
    bool anyUnprovisioned = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](const Components::Terrain& terrain, const Components::WorldTransform&)
        {
            anyTerrain = true;

            // Either handle standing up is enough to march: a planar terrain
            // carries one and a tiled terrain the other, and the digest folds
            // whichever resolves. Neither resolving is what "declared but not
            // here yet" looks like from the outside.
            const bool planarReady =
                terrainService->GetTerrainData(TerrainECS::TerrainHandle{
                    terrain.TerrainDataHandle, terrain.TerrainDataGeneration}) != nullptr;
            const bool tiledReady =
                terrainService->GetTiledTerrainData(TerrainECS::TiledTerrainHandle{
                    terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}) != nullptr;
            if (!planarReady && !tiledReady)
                anyUnprovisioned = true;
        });

    if (!anyTerrain)
        return ConformSurfaceReadiness::NoSurface;
    return anyUnprovisioned ? ConformSurfaceReadiness::Provisioning
                            : ConformSurfaceReadiness::Ready;
}

bool DeferFirstBuild(ConformSurfaceReadiness readiness, bool readsSurface, bool appliedOnce,
                     bool surfaceSettled, float32 deltaSeconds, uint32 entityId,
                     float32& deferredSeconds, bool& warned)
{
    // Nothing to wait for: already built once, reads no ground, or the scene
    // carries no terrain at all and none is coming.
    if (appliedOnce || !readsSurface || readiness == ConformSurfaceReadiness::NoSurface)
    {
        deferredSeconds = 0.0f;
        return false;
    }

    // The ground is here AND has stopped moving — the only state a first
    // measurement can be trusted against.
    if (readiness == ConformSurfaceReadiness::Ready && surfaceSettled)
    {
        deferredSeconds = 0.0f;
        return false;
    }

    deferredSeconds += deltaSeconds;
    if (deferredSeconds < kFirstBuildDeferBudgetSeconds)
        return true;

    if (!warned)
    {
        warned = true;
        Logger::Log::Warning(
            "SplineConform: entity {} waited {:.0f} s for ground that never became final — the "
            "terrain never provisioned its surface data, or it has not stopped changing — so it "
            "is placing against whatever lies under it now. Pieces there may stand on props, or "
            "hold their authored altitude where nothing does. Check that the terrain entity's "
            "asset reference resolves and that its tiles have finished streaming.",
            entityId, deferredSeconds);
    }
    return false;
}

void ReportTiltClamp(const TiltClampReport& report, uint32 entityId, float32 maxTiltDegrees,
                     uint32& lastReportedClamps)
{
    if (report.ClampedStations == lastReportedClamps)
        return;
    lastReportedClamps = report.ClampedStations;
    if (report.ClampedStations == 0u)
        return;

    Logger::Log::Warning(
        "SplinePlacement: entity {} has {} station(s) on ground steeper than the {:.0f} degree "
        "placement tilt ceiling — steepest {:.0f} degrees at ({:.2f}, {:.2f}, {:.2f}). Those "
        "pieces still conform in height but hold the ceiling, so they cut into the face instead "
        "of lying on it. Raise Max Tilt to follow the ground further, or route the spline around "
        "the steep section.",
        entityId, report.ClampedStations, maxTiltDegrees, report.SteepestDegrees,
        report.SteepestAt.x, report.SteepestAt.y, report.SteepestAt.z);
}

void ReportSpanRollBorrowed(uint32 borrowedSpans, uint32 entityId, uint32& lastReportedBorrows)
{
    if (borrowedSpans == lastReportedBorrows)
        return;
    lastReportedBorrows = borrowedSpans;
    if (borrowedSpans == 0u)
        return;

    Logger::Log::Warning(
        "SplineFence: entity {} has {} span(s) whose two posts stand almost vertically above one "
        "another — ground that steep leaves a span no roll of its own, so those spans hold their "
        "opening post's frame and stop reaching both posts. Add an authored point at the top and "
        "the bottom of the face, or route the run across it rather than straight up it.",
        entityId, borrowedSpans);
}

} // namespace GameEngine::Editor
