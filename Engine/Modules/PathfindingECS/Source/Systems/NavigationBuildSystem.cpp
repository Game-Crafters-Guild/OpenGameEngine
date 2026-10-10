#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "SceneGeometryCollector.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/HeightFieldCollisionReadiness.h"
#include "Physics/PhysicsQuery.h"
#include "AssetCore/GUID.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/NavGridAsset.h"
#include "Assets/NavMeshAsset.h"
#include "Pathfinding/NavCacheManager.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace GameEngine::PathfindingECS
{

namespace
{

// ~2 seconds at 60 fps: long enough that a healthy drain never trips it,
// short enough that a wedged or starved gate surfaces while reproducible.
constexpr uint32 kGateStarvationWarnFrames = 120;

// The grid's cells, widened by one cell for the cell footprint and the
// alternating-row offset of hex grids, over every height.
Physics::AABB GridBounds(const Pathfinding::GridMap& grid)
{
    const auto& settings = grid.GetSettings();
    float32 minX = std::numeric_limits<float32>::max();
    float32 minZ = minX;
    float32 maxX = std::numeric_limits<float32>::lowest();
    float32 maxZ = maxX;
    for (uint32 x : {0u, settings.Width - 1})
        for (uint32 z : {0u, settings.Depth - 1})
        {
            float32 wx = 0.0f, wz = 0.0f;
            grid.CellToWorld(x, z, wx, wz);
            minX = std::min(minX, wx);
            minZ = std::min(minZ, wz);
            maxX = std::max(maxX, wx);
            maxZ = std::max(maxZ, wz);
        }
    const float32 margin = settings.CellSize;
    Physics::AABB box;
    box.min = Physics::Vector3(minX - margin, std::numeric_limits<float32>::lowest(), minZ - margin);
    box.max = Physics::Vector3(maxX + margin, std::numeric_limits<float32>::max(), maxZ + margin);
    return box;
}

uint64 CombineHash(uint64 running, uint64 value)
{
    return (running ^ value) * 1099511628211ull;
}

bool HasAssetGuid(const uint8 guid[16])
{
    for (int i = 0; i < 16; ++i)
    {
        if (guid[i] != 0)
            return true;
    }
    return false;
}

GUID GuidFromBytes(const uint8 bytes[16])
{
    GUID guid;
    std::memcpy(&guid, bytes, 16);
    return guid;
}

// Apply NavGridAsset cost/blocked data to a GridMap.
void ApplyGridAssetData(Pathfinding::GridMap* gridMap, const NavGridAsset* asset,
                        uint32 width, uint32 depth, uint32 entityId)
{
    if (!gridMap || !asset)
        return;

    const auto& costs = asset->GetCosts();
    const auto& blocked = asset->GetBlocked();
    const uint32 cellCount = width * depth;

    for (uint32 i = 0; i < cellCount && i < costs.size(); ++i)
    {
        uint32 cx = i % width;
        uint32 cz = i / width;
        gridMap->SetCellCost(cx, cz, costs[i]);
    }
    for (uint32 i = 0; i < cellCount && i < blocked.size(); ++i)
    {
        uint32 cx = i % width;
        uint32 cz = i / width;
        if (blocked[i])
            gridMap->SetCellBlocked(cx, cz, true);
    }

    Logger::Log::Info("[PathfindingECS] Applied NavGrid asset data for entity {}", entityId);
}

} // anonymous namespace

NavigationBuildSystem::NavigationBuildSystem()
    : m_AsyncState(std::make_shared<AsyncState>())
{
}

NavigationBuildSystem::~NavigationBuildSystem() = default;

void NavigationBuildSystem::NoteCollisionWait(ECS::EntityHandle grid,
                                              const PhysicsECS::HeightFieldCollisionWait& wait)
{
    auto& streak = m_CollisionWaits[grid];
    streak.LastUpdate = m_UpdateIndex;
    ++streak.Frames;
    if (streak.Frames == 1)
    {
        Logger::Log::Info("[PathfindingECS] Grid bake for entity {} waits for heightfield collision: entity {}: {}",
                          grid.id, wait.Entity.id, wait.Condition);
    }
    else if (streak.Frames == kGateStarvationWarnFrames)
    {
        Logger::Log::Warning(
            "[PathfindingECS] Grid bake for entity {} has waited {} frames for heightfield collision: "
            "entity {}: {}; the grid keeps its previous heights until that collision is current",
            grid.id, kGateStarvationWarnFrames, wait.Entity.id, wait.Condition);
    }
}

void NavigationBuildSystem::UpdateNavigationAssetWatch()
{
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;

    // Cheap per-frame gate. Mounting or rebinding an asset source is the only
    // thing that can move the project root, and both bump this counter, so an
    // unchanged version means the watch is still pointed at the right place.
    // Everything below (a registry read-lock, the watch service mutex) runs
    // only on the frames where that is not already true.
    const uint64 sourceSetVersion = assetManager->GetSourceSetVersion();
    if (sourceSetVersion == m_WatchedSourceSetVersion)
        return;
    m_WatchedSourceSetVersion = sourceSetVersion;

    const std::filesystem::path assetRoot = assetManager->GetAssetRoot();
    if (assetRoot == m_WatchedAssetRoot)
        return;

    // Release the old root's subscriptions before taking any on the new one:
    // the editor rebinds the project source in-process when the user opens a
    // different project (AssetManager::RebindSource), and a watch left on the
    // previous root would mark grids dirty for files no longer in the project.
    m_NavGridWatchSub.reset();
    m_NavMeshWatchSub.reset();
    m_WatchedAssetRoot.clear();

    // Authored navigation assets live under the project asset root, which is
    // therefore the watch scope. With no project mounted the root is empty,
    // and a pattern rooted at an empty directory matches no file at all.
    if (assetRoot.empty())
        return;

    // Watching is armed during editor startup, before the first frame ticks.
    // Where it is not armed (player, tests, UI replay) there is nothing to
    // subscribe to; the next mount or rebind retries.
    auto& watchService = FileWatchingService::GetInstance();
    if (!watchService.IsWatching())
        return;

    // The callbacks touch only the shared state. Destroying a subscription
    // waits for a callback already running, so none runs after the system.
    auto asyncState = m_AsyncState;

    FilePattern gridPattern(assetRoot, ".*", {".navgrid"}, true);
    m_NavGridWatchSub.emplace(watchService.Subscribe(gridPattern,
        [asyncState](const FileChangeEvent& event) {
            if (event.Type == FileChangeType::Modified)
            {
                std::lock_guard lock(asyncState->Mutex);
                asyncState->DirtyGridEntities.insert(UINT32_MAX);
            }
        }));

    FilePattern meshPattern(assetRoot, ".*", {".navmesh"}, true);
    m_NavMeshWatchSub.emplace(watchService.Subscribe(meshPattern,
        [asyncState](const FileChangeEvent& event) {
            if (event.Type == FileChangeType::Modified)
            {
                std::lock_guard lock(asyncState->Mutex);
                asyncState->DirtyMeshEntities.insert(UINT32_MAX);
            }
        }));

    m_WatchedAssetRoot = assetRoot;
}

void NavigationBuildSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    // Async path jobs read maps unlocked (NavigationWorld's thread-safety
    // contract), so every map mutation this system makes — AddGridMap /
    // AddDetourNavMap, the rebake/rebuild writes, and the dynamic-obstacle
    // reapply — must wait for a job-free window. All rebuild triggers
    // (NeedsRebake/NeedsRebuild flags, dirty sets, completed async loads)
    // persist across frames, so a deferred frame simply retries on the next one.
    //
    // This gate cannot fire as the engine currently stands: NavigationWorld::
    // SetJobSystem has no runtime caller, so m_JobSystem stays null, RequestPath
    // returns an invalid handle, no path job is ever submitted and
    // HasActivePathJobs() is permanently false. The check stands because it is
    // the correct guard the moment a job system is wired — at which point
    // NavGridBrushTool's ungated grid writes become a concurrent-mutation
    // hazard too, and must be gated in the same change.
    if (navWorld->HasActivePathJobs())
    {
        ++m_GateDeferredFrames;
        if (m_GateDeferredFrames == kGateStarvationWarnFrames)
        {
            Logger::Log::Warning(
                "[PathfindingECS] NavigationBuildSystem: map updates deferred for {} consecutive "
                "frames by in-flight path jobs; grids are not rebaking or applying obstacles",
                kGateStarvationWarnFrames);
        }
        return;
    }
    m_GateDeferredFrames = 0;
    ++m_UpdateIndex;

    UpdateNavigationAssetWatch();

    // Check for file-watcher-triggered dirty entities and mark them for rebuild.
    {
        std::unordered_set<uint32> dirtyGrids;
        std::unordered_set<uint32> dirtyMeshes;
        {
            std::lock_guard lock(m_AsyncState->Mutex);
            dirtyGrids.swap(m_AsyncState->DirtyGridEntities);
            dirtyMeshes.swap(m_AsyncState->DirtyMeshEntities);
        }

        if (!dirtyGrids.empty())
        {
            world.Query<ECS::Write<Components::NavigationGrid>>().Each(
                [&](ECS::EntityHandle /*e*/, Components::NavigationGrid& source) {
                    if (HasAssetGuid(source.AssetGuid))
                        source.NeedsRebake = true;
                });
        }
        if (!dirtyMeshes.empty())
        {
            world.Query<ECS::Write<Components::NavigationMesh>>().Each(
                [&](ECS::EntityHandle /*e*/, Components::NavigationMesh& source) {
                    if (HasAssetGuid(source.AssetGuid))
                        source.NeedsRebuild = true;
                });
        }
    }

    // Snapshot completed async loads under lock, then process outside the lock.
    std::unordered_set<uint32> completedGrids;
    std::unordered_set<uint32> completedMeshes;
    {
        std::lock_guard lock(m_AsyncState->Mutex);
        completedGrids.swap(m_AsyncState->CompletedGridLoads);
        completedMeshes.swap(m_AsyncState->CompletedMeshLoads);
    }

    // Initialize grid sources
    world.Query<ECS::Write<Components::NavigationGrid>,
                ECS::Optional<Components::Transform>,
                ECS::Optional<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, Components::NavigationGrid& source,
            const Components::Transform* localTransform,
            const Components::WorldTransform* wt)
        {
            const auto* ownedMap = ResolveNavigationGridMap(world, source);
            if (source.Initialized && !ownedMap)
                ResetNavigationGridMap(world, source);
            if (ownedMap && !source.NeedsRebake)
                return;

            if (!ownedMap)
            {
                // GridMap latches its origin at creation and exposes no origin
                // setter. Wait for the placed transform before creating this
                // binding, including after a world/service reset.
                // TransformHierarchySystem adds WorldTransform to every
                // Transform-bearing entity, so a placed grid whose WorldTransform
                // has not appeared yet must wait rather than default to (0,0,0).
                // A grid with no Transform at all has no placed position, and
                // (0,0,0) is its correct origin.
                if (localTransform && !wt)
                    return;

                Pathfinding::GridSettings settings;
                settings.Type = source.GridType;
                settings.CellSize = source.CellSize;
                settings.Width = source.Width;
                settings.Depth = source.Depth;
                settings.MaxSlope = source.MaxSlope;
                settings.MaxStepHeight = source.MaxStepHeight;

                // Use entity's WorldTransform as grid origin
                if (wt)
                {
                    settings.OriginX = wt->matrix[12];
                    settings.OriginY = wt->matrix[13];
                    settings.OriginZ = wt->matrix[14];
                }

                auto handle = CreateNavigationGridMap(world, source, settings);
                if (!handle.IsValid())
                {
                    Logger::Log::Error("[PathfindingECS] Failed to create grid map for entity {}", e.id);
                    return;
                }
                Logger::Log::Info("[PathfindingECS] Created grid map {}x{} (cellSize={}) for entity {}",
                                  source.Width, source.Depth, source.CellSize, e.id);

                // If this source references a .navgrid asset, try to apply it now.
                if (HasAssetGuid(source.AssetGuid))
                {
                    GUID assetGuid = GuidFromBytes(source.AssetGuid);
                    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                    auto loadedAsset = assetManager.GetAsset(assetGuid);

                    if (loadedAsset && loadedAsset->IsLoaded() &&
                        loadedAsset->GetType() == AssetType::NavigationGrid)
                    {
                        ApplyGridAssetData(ResolveNavigationGridMap(world, source),
                                           static_cast<NavGridAsset*>(loadedAsset.get()),
                                           source.Width, source.Depth, e.id);
                    }
                    else
                    {
                        // Reserve the in-flight slot under the lock, but call
                        // LoadAsset OUTSIDE it: LoadAsset may fire the callback
                        // inline (already-suppressed guid, or no job system) and
                        // the callback re-locks m_AsyncState->Mutex (non-recursive)
                        // -- firing under the lock would self-deadlock.
                        bool startLoad = false;
                        {
                            std::lock_guard lock(m_AsyncState->Mutex);
                            startLoad = m_AsyncState->InFlight.insert(e.id).second;
                        }
                        if (startLoad)
                        {
                            auto asyncState = m_AsyncState;
                            uint32 entityId = e.id;

                            assetManager.LoadAsset(assetGuid,
                                [asyncState, entityId](Result<SharedPtr<Asset>, AssetError> r) {
                                    std::lock_guard cbLock(asyncState->Mutex);
                                    asyncState->InFlight.erase(entityId);
                                    if (r.IsOk() && r.Value() && r.Value()->IsLoaded())
                                        asyncState->CompletedGridLoads.insert(entityId);
                                    else
                                        Logger::Log::Error("[PathfindingECS] Failed to load NavGrid asset: {}",
                                            r.IsOk() ? "asset not loaded" : ToString(r.Error()));
                                },
                                AssetLoadPriority::Normal);

                            Logger::Log::Info("[PathfindingECS] Started async load of NavGrid asset for entity {}", e.id);
                        }
                    }
                }
            }

            // Apply async-loaded grid asset data for entities whose load completed last frame.
            if (source.Initialized && HasAssetGuid(source.AssetGuid) &&
                completedGrids.count(e.id) > 0)
            {
                GUID assetGuid = GuidFromBytes(source.AssetGuid);
                auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                auto loadedAsset = assetManager.GetAsset(assetGuid);

                if (loadedAsset && loadedAsset->IsLoaded() &&
                    loadedAsset->GetType() == AssetType::NavigationGrid)
                {
                    ApplyGridAssetData(ResolveNavigationGridMap(world, source),
                                       static_cast<NavGridAsset*>(loadedAsset.get()),
                                       source.Width, source.Depth, e.id);
                }
            }

            if (source.NeedsRebake)
            {
                // Every rebake samples the physics world: heights come from
                // downward raycasts, and the default BakeSource::Physics also
                // box-tests each cell for obstacles. With no physics world there
                // is nothing to sample, so leave NeedsRebake set and bake on a
                // later frame rather than latching a flat, obstacle-free grid
                // that nothing can subsequently correct. The editor disables the
                // whole physics pipeline outside play mode
                // (PlayModeManager::SetRuntimeSimulationSystemsEnabled), so in
                // edit mode this defers until Play begins.
                auto* physicsWorld = PhysicsECS::PhysicsWorldService::TryGet();
                if (!physicsWorld)
                {
                    if (m_BakeDeferredLogged.insert(e.id).second)
                    {
                        Logger::Log::Info(
                            "[PathfindingECS] Grid bake deferred for entity {}: no physics world to "
                            "sample; the grid will bake once one exists", e.id);
                    }
                    return;
                }
                // Clear, not erase: deferral is keyed on the physics world being
                // absent, which is global, so reaching here ends the streak for
                // every grid at once. Erasing only this entity's marker would
                // strand the markers of grids destroyed while deferred.
                m_BakeDeferredLogged.clear();

                auto* gridMap = ResolveNavigationGridMap(world, source);
                if (gridMap)
                {
                    // Heights come from raycasts against heightfield collision, which
                    // appears a frame after its source and rebuilds after an edit. Keep
                    // the retry armed until it is current, before cache or ray-bake
                    // publication.
                    PhysicsECS::HeightFieldCollisionWait wait;
                    if (!PhysicsECS::IsHeightFieldCollisionCurrent(world, *physicsWorld, GridBounds(*gridMap), wait))
                    {
                        NoteCollisionWait(e, wait);
                        return;
                    }
                    m_CollisionWaits.erase(e);
                    bool cachedHeightsLoaded = false;
                    bool rebakeHasAssetGuid = HasAssetGuid(source.AssetGuid);

                    if (rebakeHasAssetGuid)
                    {
                        GUID cacheGuid;
                        std::memcpy(&cacheGuid, source.AssetGuid, 16);

                        // Compute content hash so editing the .navgrid file invalidates the cache
                        uint64 gridContentHash = 0;
                        auto& gridCacheAssetMgr = EngineCore::GetInstance().GetAssetManager();
                        auto gridCacheAsset = gridCacheAssetMgr.GetAsset(cacheGuid);
                        if (gridCacheAsset)
                            gridContentHash = Pathfinding::NavCacheManager::ComputeFileContentHash(gridCacheAsset->GetPath());

                        std::vector<float32> cachedHeights;
                        if (gridContentHash != 0 && Pathfinding::NavCacheManager::LoadGridCache(
                                cacheGuid,
                                reinterpret_cast<const uint8*>(&gridContentHash), sizeof(gridContentHash),
                                cachedHeights))
                        {
                            const uint32 cellCount = gridMap->GetCellCount();
                            if (cachedHeights.size() == cellCount)
                            {
                                for (uint32 i = 0; i < cellCount; ++i)
                                {
                                    uint32 cx = i % source.Width;
                                    uint32 cz = i / source.Width;
                                    gridMap->SetCellHeight(cx, cz, cachedHeights[i]);
                                }
                                cachedHeightsLoaded = true;
                                Logger::Log::Info("[PathfindingECS] Loaded cached heights for entity {}", e.id);
                            }
                        }
                    }

                    if (!cachedHeightsLoaded)
                    {
                        const float32 rayOriginY = source.RaycastOriginHeight;
                        gridMap->BakeHeights([physicsWorld, rayOriginY](float32 x, float32 z, float32& outHeight) {
                            Physics::RayCastQuery query;
                            query.ray.origin = Physics::Vector3(x, rayOriginY, z);
                            query.ray.direction = Physics::Vector3(0.0f, -1.0f, 0.0f);
                            query.maxDistance = rayOriginY * 2.0f;

                            Physics::RayCastResult result;
                            if (physicsWorld->RayCast(query, result))
                            {
                                outHeight = result.hitPoint.y;
                                return true;
                            }
                            return false;
                        });

                        // Save freshly baked heights to cache when an asset GUID is present.
                        if (rebakeHasAssetGuid)
                        {
                            GUID cacheGuid;
                            std::memcpy(&cacheGuid, source.AssetGuid, 16);

                            uint64 saveGridHash = 0;
                            auto& saveGridAssetMgr = EngineCore::GetInstance().GetAssetManager();
                            auto saveGridAsset = saveGridAssetMgr.GetAsset(cacheGuid);
                            if (saveGridAsset)
                                saveGridHash = Pathfinding::NavCacheManager::ComputeFileContentHash(saveGridAsset->GetPath());

                            if (saveGridHash != 0)
                            {
                                Pathfinding::NavCacheManager::SaveGridCache(
                                    cacheGuid,
                                    reinterpret_cast<const uint8*>(&saveGridHash), sizeof(saveGridHash),
                                    gridMap->GetHeightData(), gridMap->GetCellCount());
                            }
                        }
                    }

                    // Mark cells as blocked where slope to any neighbor exceeds MaxSlope.
                    // Per-neighbor world-space distance handles diagonals and hex grids correctly.
                    const auto& settings = gridMap->GetSettings();
                    const float32 maxSlopeRad = source.MaxSlope * (3.14159265f / 180.0f);

                    for (uint32 cz = 0; cz < settings.Depth; ++cz)
                    {
                        for (uint32 cx = 0; cx < settings.Width; ++cx)
                        {
                            if (gridMap->IsCellBlocked(cx, cz))
                                continue;

                            const float32 h = gridMap->GetCellHeight(cx, cz);

                            float32 cellWorldX = 0.0f, cellWorldZ = 0.0f;
                            gridMap->CellToWorld(cx, cz, cellWorldX, cellWorldZ);

                            uint32 neighborCellX[8], neighborCellZ[8];
                            uint32 neighborCount = gridMap->GetNeighborCells(cx, cz, neighborCellX, neighborCellZ, 8);

                            for (uint32 i = 0; i < neighborCount; ++i)
                            {
                                float32 nWorldX = 0.0f, nWorldZ = 0.0f;
                                gridMap->CellToWorld(neighborCellX[i], neighborCellZ[i], nWorldX, nWorldZ);

                                float32 dx = nWorldX - cellWorldX;
                                float32 dz = nWorldZ - cellWorldZ;
                                float32 neighborDist = std::sqrt(dx * dx + dz * dz);
                                float32 maxHeightDiffForNeighbor = std::tan(maxSlopeRad) * neighborDist;

                                const float32 nh = gridMap->GetCellHeight(neighborCellX[i], neighborCellZ[i]);
                                if (std::abs(nh - h) > maxHeightDiffForNeighbor)
                                {
                                    gridMap->SetCellBlocked(cx, cz, true);
                                    break;
                                }
                            }
                        }
                    }

                    // Volume-based obstacle detection: for each walkable cell, test whether a
                    // box the size of the agent (raised slightly above ground) overlaps any
                    // physics body. Hits indicate geometry occupying the agent's walking space.
                    if (source.BakeSource == Components::NavigationBakeSource::Physics)
                    {
                        constexpr float32 kGroundOffset = 0.1f;
                        const float32 agentHalfHeight = source.BakeAgentHeight * 0.5f;

                        std::vector<Physics::OverlapResult> overlaps;
                        overlaps.reserve(4);
                        for (uint32 cz = 0; cz < settings.Depth; ++cz)
                        {
                            for (uint32 cx = 0; cx < settings.Width; ++cx)
                            {
                                if (gridMap->IsCellBlocked(cx, cz))
                                    continue;

                                float32 worldX = 0.0f;
                                float32 worldZ = 0.0f;
                                gridMap->CellToWorld(cx, cz, worldX, worldZ);
                                const float32 cellHeight = gridMap->GetCellHeight(cx, cz);

                                Physics::BoxOverlapQuery overlapQuery;
                                // A trigger volume does not occupy the cell.
                                overlapQuery.filter.ignoreSensors = true;
                                overlapQuery.center = Physics::Vector3(
                                    worldX,
                                    cellHeight + kGroundOffset + agentHalfHeight,
                                    worldZ);
                                overlapQuery.halfExtents = Physics::Vector3(
                                    source.BakeAgentRadius,
                                    agentHalfHeight,
                                    source.BakeAgentRadius);

                                overlaps.clear();
                                if (physicsWorld->BoxOverlap(overlapQuery, overlaps, 4))
                                {
                                    gridMap->SetCellBlocked(cx, cz, true);
                                }
                            }
                        }

                        Logger::Log::Info("[PathfindingECS] Completed volume-based obstacle detection for entity {}", e.id);
                    }

                    Logger::Log::Info("[PathfindingECS] Baked grid heights for entity {}", e.id);
                }
                source.NeedsRebake = false;
            }
        });

    // A streak ends when its grid bakes or stops waiting (destroyed, bake no
    // longer requested); only grids that waited this update keep their count.
    std::erase_if(m_CollisionWaits, [this](const auto& entry) { return entry.second.LastUpdate != m_UpdateIndex; });

    // Initialize navmesh sources
    world.Query<ECS::Write<Components::NavigationMesh>>().Each(
        [&](ECS::EntityHandle e, Components::NavigationMesh& source)
        {
            // A world restored from raw component bytes (play-mode exit,
            // snapshot undo) keeps a binding whose map Clear already released.
            if (source.Initialized &&
                !navWorld->GetDetourNavMap({source.NavMapIndex, source.NavMapGeneration}))
                ReleaseNavigationMeshMap(source);
            if (source.Initialized && !source.NeedsRebuild)
                return;

            if (!source.Initialized)
            {
                auto handle = navWorld->AddDetourNavMap();
                if (!handle.IsValid())
                {
                    Logger::Log::Error("[PathfindingECS] Failed to create navmesh map for entity {}", e.id);
                    return;
                }
                source.NavMapIndex = handle.Index;
                source.NavMapGeneration = handle.Generation;
                source.Initialized = true;

                Logger::Log::Info("[PathfindingECS] Created navmesh map for entity {}", e.id);

                if (HasAssetGuid(source.AssetGuid))
                {
                    GUID assetGuid = GuidFromBytes(source.AssetGuid);
                    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                    auto loadedAsset = assetManager.GetAsset(assetGuid);

                    if (loadedAsset && loadedAsset->IsLoaded() &&
                        loadedAsset->GetType() == AssetType::NavigationMesh)
                    {
                        auto* navMeshAsset = static_cast<NavMeshAsset*>(loadedAsset.get());
                        const auto& assetSettings = navMeshAsset->GetSettings();
                        source.CellSize = assetSettings.CellSize;
                        source.CellHeight = assetSettings.CellHeight;
                        source.AgentRadius = assetSettings.AgentRadius;
                        source.AgentHeight = assetSettings.AgentHeight;
                        source.AgentMaxClimb = assetSettings.AgentMaxClimb;
                        source.AgentMaxSlope = assetSettings.AgentMaxSlope;

                        // Try restoring cached Detour data using content hash.
                        uint64 initMeshHash = Pathfinding::NavCacheManager::ComputeFileContentHash(loadedAsset->GetPath());
                        for (const auto& geomGuid : navMeshAsset->GetSourceGeometryGUIDs())
                        {
                            auto geomAsset = assetManager.GetAsset(geomGuid);
                            if (geomAsset)
                                initMeshHash = CombineHash(initMeshHash, Pathfinding::NavCacheManager::ComputeFileContentHash(geomAsset->GetPath()));
                        }

                        std::vector<uint8> cachedDetourData;
                        if (initMeshHash != 0 && Pathfinding::NavCacheManager::LoadNavMeshCache(
                                assetGuid,
                                reinterpret_cast<const uint8*>(&initMeshHash), sizeof(initMeshHash),
                                cachedDetourData))
                        {
                            auto* detourMap = navWorld->GetDetourNavMap(
                                Pathfinding::NavMapHandle{source.NavMapIndex, source.NavMapGeneration});
                            if (detourMap && detourMap->Deserialize(cachedDetourData.data(),
                                                                     static_cast<uint32>(cachedDetourData.size())))
                            {
                                Logger::Log::Info("[PathfindingECS] Loaded cached navmesh for entity {}", e.id);
                                source.NeedsRebuild = false;
                            }
                        }

                        Logger::Log::Info("[PathfindingECS] Applied NavMesh asset settings for entity {}", e.id);
                    }
                    else
                    {
                        // Reserve the in-flight slot under the lock, but call
                        // LoadAsset OUTSIDE it (see the NavGrid path above): the
                        // callback re-locks m_AsyncState->Mutex (non-recursive) and
                        // LoadAsset may fire it inline.
                        bool startLoad = false;
                        {
                            std::lock_guard lock(m_AsyncState->Mutex);
                            startLoad = m_AsyncState->InFlight.insert(e.id).second;
                        }
                        if (startLoad)
                        {
                            auto asyncState = m_AsyncState;
                            uint32 entityId = e.id;

                            assetManager.LoadAsset(assetGuid,
                                [asyncState, entityId](Result<SharedPtr<Asset>, AssetError> r) {
                                    std::lock_guard cbLock(asyncState->Mutex);
                                    asyncState->InFlight.erase(entityId);
                                    if (r.IsOk() && r.Value() && r.Value()->IsLoaded())
                                        asyncState->CompletedMeshLoads.insert(entityId);
                                    else
                                        Logger::Log::Error("[PathfindingECS] Failed to load NavMesh asset: {}",
                                            r.IsOk() ? "asset not loaded" : ToString(r.Error()));
                                },
                                AssetLoadPriority::Normal);

                            Logger::Log::Info("[PathfindingECS] Started async load of NavMesh asset for entity {}", e.id);
                        }
                    }
                }
            }

            // Apply async-loaded navmesh asset data.
            if (source.Initialized && HasAssetGuid(source.AssetGuid) &&
                completedMeshes.count(e.id) > 0)
            {
                GUID assetGuid = GuidFromBytes(source.AssetGuid);
                auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                auto loadedAsset = assetManager.GetAsset(assetGuid);

                if (loadedAsset && loadedAsset->IsLoaded() &&
                    loadedAsset->GetType() == AssetType::NavigationMesh)
                {
                    auto* navMeshAsset = static_cast<NavMeshAsset*>(loadedAsset.get());
                    const auto& assetSettings = navMeshAsset->GetSettings();
                    source.CellSize = assetSettings.CellSize;
                    source.CellHeight = assetSettings.CellHeight;
                    source.AgentRadius = assetSettings.AgentRadius;
                    source.AgentHeight = assetSettings.AgentHeight;
                    source.AgentMaxClimb = assetSettings.AgentMaxClimb;
                    source.AgentMaxSlope = assetSettings.AgentMaxSlope;
                    Logger::Log::Info("[PathfindingECS] Applied async-loaded NavMesh asset settings for entity {}", e.id);
                }
            }

            if (source.NeedsRebuild)
            {
                Pathfinding::NavMapHandle handle{source.NavMapIndex, source.NavMapGeneration};
                auto* detourMap = navWorld->GetDetourNavMap(handle);
                if (!detourMap)
                {
                    Logger::Log::Error("[PathfindingECS] DetourNavMap not found for entity {}", e.id);
                    source.NeedsRebuild = false;
                    return;
                }

                // Try loading from cache first using content hash.
                bool loadedFromCache = false;
                if (HasAssetGuid(source.AssetGuid))
                {
                    GUID assetGuid = GuidFromBytes(source.AssetGuid);
                    auto& rebuildAssetMgr = EngineCore::GetInstance().GetAssetManager();
                    auto rebuildAsset = rebuildAssetMgr.GetAsset(assetGuid);

                    uint64 rebuildMeshHash = 0;
                    if (rebuildAsset)
                    {
                        rebuildMeshHash = Pathfinding::NavCacheManager::ComputeFileContentHash(rebuildAsset->GetPath());
                        if (rebuildAsset->IsLoaded() && rebuildAsset->GetType() == AssetType::NavigationMesh)
                        {
                            auto* navAsset = static_cast<NavMeshAsset*>(rebuildAsset.get());
                            for (const auto& geomGuid : navAsset->GetSourceGeometryGUIDs())
                            {
                                auto geomAsset = rebuildAssetMgr.GetAsset(geomGuid);
                                if (geomAsset)
                                    rebuildMeshHash = CombineHash(rebuildMeshHash, Pathfinding::NavCacheManager::ComputeFileContentHash(geomAsset->GetPath()));
                            }
                        }
                    }

                    std::vector<uint8> cachedData;
                    if (rebuildMeshHash != 0 && Pathfinding::NavCacheManager::LoadNavMeshCache(
                            assetGuid,
                            reinterpret_cast<const uint8*>(&rebuildMeshHash), sizeof(rebuildMeshHash),
                            cachedData))
                    {
                        if (detourMap->Deserialize(cachedData.data(),
                                                   static_cast<uint32>(cachedData.size())))
                        {
                            loadedFromCache = true;
                            Logger::Log::Info("[PathfindingECS] Loaded cached navmesh for entity {}", e.id);
                        }
                    }
                }

                if (!loadedFromCache)
                {
                    // Gather geometry: prefer explicit asset references from NavMeshAsset,
                    // fall back to collecting all renderable scene geometry.
                    CollectedGeometry geometry;

                    if (HasAssetGuid(source.AssetGuid))
                    {
                        GUID assetGuid = GuidFromBytes(source.AssetGuid);
                        auto& assetManager = EngineCore::GetInstance().GetAssetManager();
                        auto loadedAsset = assetManager.GetAsset(assetGuid);

                        if (loadedAsset && loadedAsset->IsLoaded() &&
                            loadedAsset->GetType() == AssetType::NavigationMesh)
                        {
                            auto* navMeshAsset = static_cast<NavMeshAsset*>(loadedAsset.get());
                            const auto& sourceGuids = navMeshAsset->GetSourceGeometryGUIDs();
                            if (!sourceGuids.empty())
                            {
                                geometry = CollectGeometryFromAssets(sourceGuids);
                            }
                        }
                    }

                    // Fall back to full scene collection when no explicit geometry was provided.
                    if (geometry.VertexCount == 0)
                    {
                        geometry = CollectSceneGeometry(world);
                    }

                    if (geometry.VertexCount > 0 && geometry.TriangleCount > 0)
                    {
                        Pathfinding::NavMeshSettings settings;
                        settings.CellSize = source.CellSize;
                        settings.CellHeight = source.CellHeight;
                        settings.AgentRadius = source.AgentRadius;
                        settings.AgentHeight = source.AgentHeight;
                        settings.AgentMaxClimb = source.AgentMaxClimb;
                        settings.AgentMaxSlope = source.AgentMaxSlope;
                        settings.RegionMinSize = source.RegionMinSize;
                        settings.RegionMergeSize = source.RegionMergeSize;
                        settings.EdgeMaxLen = source.EdgeMaxLen;
                        settings.EdgeMaxError = source.EdgeMaxError;
                        settings.DetailSampleDist = source.DetailSampleDist;
                        settings.DetailSampleMaxError = source.DetailSampleMaxError;
                        settings.VertsPerPoly = source.VertsPerPoly;

                        Pathfinding::DetourNavMap::InputGeometry input;
                        input.Vertices = geometry.Vertices.data();
                        input.VertexCount = geometry.VertexCount;
                        input.Indices = geometry.Indices.data();
                        input.TriangleCount = geometry.TriangleCount;

                        if (detourMap->Build(settings, input))
                        {
                            Logger::Log::Info(
                                "[PathfindingECS] Built navmesh from {} vertices, {} triangles for entity {}",
                                geometry.VertexCount, geometry.TriangleCount, e.id);

                            // Cache the built navmesh for faster subsequent loads.
                            if (HasAssetGuid(source.AssetGuid))
                            {
                                GUID cacheGuid = GuidFromBytes(source.AssetGuid);
                                std::vector<uint8> serialized;
                                if (detourMap->Serialize(serialized))
                                {
                                    uint64 saveMeshHash = 0;
                                    auto& saveMeshAssetMgr = EngineCore::GetInstance().GetAssetManager();
                                    auto saveMeshAsset = saveMeshAssetMgr.GetAsset(cacheGuid);
                                    if (saveMeshAsset)
                                    {
                                        saveMeshHash = Pathfinding::NavCacheManager::ComputeFileContentHash(saveMeshAsset->GetPath());
                                        if (saveMeshAsset->IsLoaded() && saveMeshAsset->GetType() == AssetType::NavigationMesh)
                                        {
                                            auto* saveMeshNavAsset = static_cast<NavMeshAsset*>(saveMeshAsset.get());
                                            for (const auto& geomGuid : saveMeshNavAsset->GetSourceGeometryGUIDs())
                                            {
                                                auto geomAsset = saveMeshAssetMgr.GetAsset(geomGuid);
                                                if (geomAsset)
                                                    saveMeshHash = CombineHash(saveMeshHash, Pathfinding::NavCacheManager::ComputeFileContentHash(geomAsset->GetPath()));
                                            }
                                        }
                                    }

                                    if (saveMeshHash != 0)
                                    {
                                        Pathfinding::NavCacheManager::SaveNavMeshCache(
                                            cacheGuid,
                                            reinterpret_cast<const uint8*>(&saveMeshHash), sizeof(saveMeshHash),
                                            serialized);
                                    }
                                }
                            }
                        }
                        else
                        {
                            Logger::Log::Error("[PathfindingECS] Failed to build navmesh for entity {}", e.id);
                        }
                    }
                    else
                    {
                        Logger::Log::Warning("[PathfindingECS] No geometry found for navmesh bake (entity {})", e.id);
                    }
                }

                source.NeedsRebuild = false;
            }

            // Initialize crowd manager once the navmesh is built
            auto& crowd = navWorld->GetCrowdManager();
            if (!crowd.IsInitialized())
            {
                Pathfinding::NavMapHandle handle{source.NavMapIndex, source.NavMapGeneration};
                auto* detourMap = navWorld->GetDetourNavMap(handle);
                if (detourMap && detourMap->IsBuilt())
                {
                    constexpr uint32 kMaxCrowdAgents = 128;
                    crowd.Initialize(*detourMap, kMaxCrowdAgents);
                    Logger::Log::Info("[PathfindingECS] Initialized CrowdManager with {} max agents for entity {}",
                                      kMaxCrowdAgents, e.id);
                }
            }
        });

    // Last step so freshly (re)built grids also carry this frame's obstacle
    // marks. NavigationPathfindingSystem submits jobs in the next wave, so
    // paths computed this frame see this frame's obstacles.
    ApplyDynamicObstacles(world);
}

void NavigationBuildSystem::ApplyDynamicObstacles(ECS::World& world)
{
    // Clear dynamic obstacle blocking on all grids
    world.Query<ECS::Read<Components::NavigationGrid>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationGrid& source)
        {
            auto* gridMap = ResolveNavigationGridMap(world, source);
            if (gridMap)
                gridMap->ClearAllDynamicBlocked();
        });

    // Rasterize each obstacle's footprint into every grid it overlaps
    world.Query<ECS::Read<Components::NavigationObstacle>,
                ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationObstacle& obstacle,
            const Components::WorldTransform& wt)
        {
            float32 posX = wt.matrix[12];
            float32 posZ = wt.matrix[14];
            float32 radius = obstacle.Radius;

            world.Query<ECS::Read<Components::NavigationGrid>>().Each(
                [&](ECS::EntityHandle /*e*/, const Components::NavigationGrid& source)
                {
                    auto* gridMap = ResolveNavigationGridMap(world, source);
                    if (!gridMap)
                        return;

                    uint32 centerX = 0, centerZ = 0;
                    if (!gridMap->WorldToCell(posX, posZ, centerX, centerZ))
                        return;

                    const auto& gridSettings = gridMap->GetSettings();
                    const float32 cellSize = gridSettings.CellSize;

                    // Radius is authored and unvalidated, and this stamp is
                    // O(cellRadius^2) per obstacle per grid, every frame:
                    // Radius=1000 at CellSize=0.1 is 4e8 iterations. Clamping the
                    // cell radius bounds that loop.
                    //
                    // Clamp to Width + Depth, not to max(Width, Depth). The
                    // farthest in-grid cell from an in-grid centre is up to
                    // (Width-1)^2 + (Depth-1)^2 away, which exceeds
                    // max(Width, Depth)^2 for any square grid of 4 cells or more —
                    // clamping to the longer side would drop the far corners of an
                    // obstacle that really does span the grid. (Width + Depth)^2
                    // exceeds that distance for every grid, so the loop is bounded
                    // and the footprint is unchanged.
                    //
                    // A non-positive radius blocks nothing, including the
                    // obstacle's own cell. This also covers NaN, whose only other
                    // route is static_cast<uint32>(NaN) — undefined.
                    if (!(radius > 0.0f))
                        return;
                    // Clamped before the cast: radius/cellSize is inf for
                    // CellSize=0, and casting that to an integer is undefined.
                    const float32 maxCellRadius = static_cast<float32>(gridSettings.Width) +
                                                  static_cast<float32>(gridSettings.Depth);
                    const int32 r =
                        static_cast<int32>(std::min(std::ceil(radius / cellSize), maxCellRadius));
                    // 64-bit: dx^2 + dz^2 reaches 2r^2, which leaves int32 range
                    // for a grid past ~32k cells on a side.
                    const int64 rSq = static_cast<int64>(r) * r;

                    for (int32 dz = -r; dz <= r; ++dz)
                    {
                        for (int32 dx = -r; dx <= r; ++dx)
                        {
                            if (static_cast<int64>(dx) * dx + static_cast<int64>(dz) * dz > rSq)
                                continue;

                            int32 cx = static_cast<int32>(centerX) + dx;
                            int32 cz = static_cast<int32>(centerZ) + dz;
                            if (cx < 0 || cz < 0)
                                continue;

                            gridMap->SetCellDynamicBlocked(
                                static_cast<uint32>(cx), static_cast<uint32>(cz), true);
                        }
                    }
                });
        });
}

} // namespace GameEngine::PathfindingECS
