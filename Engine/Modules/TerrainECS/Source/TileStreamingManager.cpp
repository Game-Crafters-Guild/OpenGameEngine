#include "TerrainECS/TileStreamingManager.h"
#include "TerrainECS/TerrainService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace GameEngine::TerrainECS
{

// Pre-compute per-node min/max for the global quadtree finest level.
// Runs on job threads to avoid scanning heightfield on the main thread.
static void ComputeQuadtreeNodesFromHeightfield(
    const Terrain::HeightfieldData& hf,
    uint32 tileFinestNodes,
    std::vector<NodeMinMax>& outNodes,
    uint32& outNodesPerAxis)
{
    outNodesPerAxis = tileFinestNodes;
    outNodes.resize(static_cast<size_t>(tileFinestNodes) * tileFinestNodes);

    const uint32 hfW = hf.GetWidth();
    const uint32 hfH = hf.GetHeight();
    const uint32 samplesPerNodeX = std::max(1u, (hfW - 1) / tileFinestNodes);
    const uint32 samplesPerNodeZ = std::max(1u, (hfH - 1) / tileFinestNodes);

    for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
    {
        for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
        {
            float32 nodeMinH, nodeMaxH;
            hf.GetMinMax(static_cast<int32>(nx * samplesPerNodeX),
                         static_cast<int32>(nz * samplesPerNodeZ),
                         static_cast<int32>(samplesPerNodeX + 1),
                         static_cast<int32>(samplesPerNodeZ + 1),
                         nodeMinH, nodeMaxH);

            outNodes[nz * tileFinestNodes + nx] = {nodeMinH, nodeMaxH};
        }
    }
}

TileStreamingManager::TileStreamingManager(JobSystem::WorkStealingThreadPool* jobSystem,
                                           const StreamingConfig& config)
    : m_Config(config)
    , m_JobSystem(jobSystem)
{
}

TileStreamingManager::~TileStreamingManager()
{
    CancelAll();
}

void TileStreamingManager::CancelPendingJobs(TerrainStream& stream)
{
    for (auto& [coord, tracker] : stream.Trackers)
    {
        if (tracker.ActiveJob.IsValid() && !tracker.ActiveJob.IsDone())
            tracker.ActiveJob.Cancel();
    }
}

void TileStreamingManager::Forget(uint32 tiledIndex)
{
    const auto it = m_Streams.find(tiledIndex);
    if (it == m_Streams.end())
        return;
    CancelPendingJobs(it->second);
    m_Streams.erase(it);
}

void TileStreamingManager::CancelAll()
{
    // Jobs produce only CPU data; dropping each terrain's results queue is enough (no GPU
    // resources are owned here — the unified textures live on the render feature).
    for (auto& [index, stream] : m_Streams)
        CancelPendingJobs(stream);
    m_Streams.clear();
}

TileStreamingManager::Stats TileStreamingManager::GetStats() const
{
    Stats stats{};
    for (const auto& [index, stream] : m_Streams)
    {
        for (const auto& [coord, tracker] : stream.Trackers)
        {
            switch (tracker.State)
            {
            case TileState::Active:
                ++stats.TilesActive;
                break;
            case TileState::LoadingCoarse:
            case TileState::LoadingFull:
                ++stats.TilesLoading;
                break;
            case TileState::Requested:
                ++stats.TilesPending;
                break;
            default:
                break;
            }
        }
        stats.IntegrationTimeMs += stream.LastIntegrationTimeMs;
        stats.TeleportModeActive = stats.TeleportModeActive || stream.TeleportModeActive;
    }
    return stats;
}

// ---- Main update (called once per frame per tiled terrain from extraction system) ----

const std::vector<TileCoord>& TileStreamingManager::Update(TiledTerrainHandle tiledHandle,
                                                           TiledTerrainData& tiled,
                                                           TerrainService& service,
                                                           const std::vector<Mathematics::Vector3>& cameraPositions,
                                                           Mathematics::Vector3 viewDirection,
                                                           float32 streamingRadius,
                                                           float32 deltaTime)
{
    TerrainStream& stream = m_Streams[tiledHandle.Index];

    // A new terrain in this slot (entity deleted/recreated, re-provisioned): start over. The
    // fresh results queue strands whatever the old terrain's running jobs still deliver.
    if (stream.Handle != tiledHandle)
    {
        CancelPendingJobs(stream);
        stream = TerrainStream{};
        stream.Handle = tiledHandle;
    }

    ++stream.FrameCounter;
    stream.IntegratedThisFrame.clear();

    DetectTeleportation(stream, cameraPositions, deltaTime);

    if (stream.TeleportModeActive)
        HandleTeleportMode(stream, tiled, cameraPositions, streamingRadius);

    EvaluateTilePriorities(stream, tiled, cameraPositions, viewDirection, streamingRadius);
    DispatchPendingJobs(stream, tiled);
    IntegrateCompletedTiles(stream, tiled, service);
    UnloadDistantTiles(stream, tiled, service, cameraPositions, streamingRadius);
    return stream.IntegratedThisFrame;
}

// ---- Priority evaluation ----

void TileStreamingManager::EvaluateTilePriorities(
    TerrainStream& stream,
    const TiledTerrainData& tiled,
    const std::vector<Mathematics::Vector3>& cameraPositions,
    Mathematics::Vector3 viewDirection,
    float32 streamingRadius)
{
    if (cameraPositions.empty())
        return;

    const float32 streamRSq = streamingRadius * streamingRadius;
    const float32 tileSize = tiled.Config.TileWorldSize;

    // Determine tile range from all cameras.
    int32 minTileX = static_cast<int32>(tiled.Config.TilesPerAxisX);
    int32 minTileZ = static_cast<int32>(tiled.Config.TilesPerAxisZ);
    int32 maxTileX = -1;
    int32 maxTileZ = -1;

    for (const auto& camPos : cameraPositions)
    {
        int32 lo = std::max(0, static_cast<int32>(std::floor((camPos.x - streamingRadius - tiled.WorldOriginX) / tileSize)));
        int32 hi = std::min(static_cast<int32>(tiled.Config.TilesPerAxisX) - 1,
                            static_cast<int32>(std::floor((camPos.x + streamingRadius - tiled.WorldOriginX) / tileSize)));
        minTileX = std::min(minTileX, lo);
        maxTileX = std::max(maxTileX, hi);

        lo = std::max(0, static_cast<int32>(std::floor((camPos.z - streamingRadius - tiled.WorldOriginZ) / tileSize)));
        hi = std::min(static_cast<int32>(tiled.Config.TilesPerAxisZ) - 1,
                      static_cast<int32>(std::floor((camPos.z + streamingRadius - tiled.WorldOriginZ) / tileSize)));
        minTileZ = std::min(minTileZ, lo);
        maxTileZ = std::max(maxTileZ, hi);
    }

    for (int32 tz = minTileZ; tz <= maxTileZ; ++tz)
    {
        for (int32 tx = minTileX; tx <= maxTileX; ++tx)
        {
            TileCoord coord{tx, tz};
            const float32 tileCenterX = tiled.WorldOriginX + (tx + 0.5f) * tileSize;
            const float32 tileCenterZ = tiled.WorldOriginZ + (tz + 0.5f) * tileSize;

            // Check distance to nearest camera.
            float32 bestDistSq = std::numeric_limits<float32>::max();
            for (const auto& camPos : cameraPositions)
            {
                const float32 dx = tileCenterX - camPos.x;
                const float32 dz = tileCenterZ - camPos.z;
                bestDistSq = std::min(bestDistSq, dx * dx + dz * dz);
            }

            if (bestDistSq > streamRSq)
                continue;

            auto& tracker = stream.Trackers[coord];
            tracker.LastAccessFrame = stream.FrameCounter;

            // Approximate frustum test: tile is "in front" if the vector from
            // camera to tile center has a positive dot product with view direction.
            // Tiles behind the camera still load (they're in streaming radius)
            // but at lower priority.
            const auto& camPos = cameraPositions[0];
            const bool inFront = TileIsInFrontOfCameraXZ(
                tileCenterX, tileCenterZ, camPos.x, camPos.z, viewDirection);

            TileStreamingPriority priority{};
            priority.DistanceSq = bestDistSq;
            priority.InFrustum = inFront;
            tracker.Priority = priority;

            // Request tile if not yet in the pipeline.
            if (tracker.State == TileState::Idle)
            {
                tracker.State = TileState::Requested;
                tracker.Result = std::make_shared<TileGenerationResult>();
                tracker.Result->Coord = coord;
            }
            // If tile is Active but only at Coarse LOD, request upgrade.
            else if (tracker.State == TileState::Active)
            {
                auto it = tiled.Tiles.find(coord);
                if (it != tiled.Tiles.end() && it->second &&
                    it->second->LodState == TileLodState::Coarse &&
                    !stream.TeleportModeActive)
                {
                    tracker.State = TileState::Requested;
                    tracker.Priority.IsUpgrade = true;
                    if (!tracker.Result)
                    {
                        tracker.Result = std::make_shared<TileGenerationResult>();
                        tracker.Result->Coord = coord;
                    }
                }
            }
        }
    }
}

// ---- Job dispatch ----

void TileStreamingManager::DispatchPendingJobs(TerrainStream& stream, const TiledTerrainData& tiled)
{
    // Collect requested tiles, sorted by priority.
    std::vector<TileCoord> requested;
    for (auto& [coord, tracker] : stream.Trackers)
    {
        if (tracker.State == TileState::Requested)
            requested.push_back(coord);
    }

    std::sort(requested.begin(), requested.end(),
        [&stream](const TileCoord& a, const TileCoord& b) {
            return stream.Trackers[a].Priority < stream.Trackers[b].Priority;
        });

    for (const auto& coord : requested)
    {
        if (stream.ActiveJobCount >= m_Config.MaxConcurrentJobs)
            break;

        auto& tracker = stream.Trackers[coord];

        if (tracker.Priority.IsUpgrade)
        {
            // Upgrade: skip coarse, go directly to full.
            DispatchFullJob(stream, tiled, tracker, coord);
        }
        else if (stream.TeleportModeActive)
        {
            // Teleport mode: coarse only for fast coverage.
            DispatchCoarseJob(stream, tiled, tracker, coord);
        }
        else
        {
            // Normal mode: coarse first for progressive refinement.
            DispatchCoarseJob(stream, tiled, tracker, coord);
        }
    }
}

void TileStreamingManager::DispatchCoarseJob(TerrainStream& stream,
                                             const TiledTerrainData& tiled,
                                             TileTracker& tracker,
                                             TileCoord coord)
{
    if (!m_JobSystem)
        return;

    auto result = tracker.Result;
    const float32 worldOriginX = tiled.WorldOriginX + coord.X * tiled.Config.TileWorldSize;
    const float32 worldOriginZ = tiled.WorldOriginZ + coord.Z * tiled.Config.TileWorldSize;
    const float32 terrainOriginX = tiled.WorldOriginX;
    const float32 terrainOriginZ = tiled.WorldOriginZ;
    const float32 tileWorld = tiled.Config.TileWorldSize;
    const uint32 coarseRes = m_Config.CoarseResolution;
    const uint32 tileLODLevels = tiled.Config.TileConfig.LODLevels;
    // By value: the copy shares the base heightmap, so the job reads it safely even
    // if the terrain is re-provisioned or the decode cache evicts it meanwhile.
    const TiledTerrainConfig config = tiled.Config;

    // Capture shared_ptr to completed queue so lambdas are safe even if
    // the streaming manager is destroyed while jobs are still in flight.
    auto completedQueue = stream.Completed;

    tracker.ActiveJob = m_JobSystem->Submit([result, worldOriginX, worldOriginZ,
                                             terrainOriginX, terrainOriginZ, config,
                                             tileWorld, coarseRes, coord,
                                             tileLODLevels, completedQueue]()
    {
        result->Coord = coord;
        result->CoarseHeightfield.Resize(coarseRes, coarseRes, 0.0f);
        FillTiledBaseRegion(result->CoarseHeightfield, config, terrainOriginX, terrainOriginZ,
                            worldOriginX, worldOriginZ, tileWorld, tileWorld, 0, 0,
                            static_cast<int32>(coarseRes) - 1, static_cast<int32>(coarseRes) - 1);

        // Compute min/max.
        float32 minH = std::numeric_limits<float32>::max();
        float32 maxH = std::numeric_limits<float32>::lowest();
        const auto* samples = result->CoarseHeightfield.GetRawSamples();
        const uint32 count = coarseRes * coarseRes;
        for (uint32 i = 0; i < count; ++i)
        {
            minH = std::min(minH, samples[i]);
            maxH = std::max(maxH, samples[i]);
        }
        result->MinHeight = minH;
        result->MaxHeight = maxH;

        // Size the coarse splatmap so the streaming tile has one to upload. It stays
        // unbaked (reads as channel 0) until the modifier bake composites the surface
        // rules on arrival. Extraction bilinear-upsamples this (and the coarse
        // heightfield) into the unified tile region; the coarse NORMAL is
        // intentionally not generated — the tile is distant (streaming edge) where
        // flat-up lighting is imperceptible, and Full uploads the real normal on upgrade.
        ResetSplatmap(
            result->CoarseHeightfield,
            result->Splatmap, result->SplatmapWidth, result->SplatmapHeight);

        // Pre-compute quadtree node min/max for main-thread integration.
        const uint32 tileFinestNodes = 1u << (tileLODLevels - 1);
        ComputeQuadtreeNodesFromHeightfield(
            result->CoarseHeightfield, tileFinestNodes,
            result->QuadtreeNodes, result->QuadtreeNodesPerAxis);

        result->CompletedStage = TileState::CoarseReady;

        std::lock_guard<std::mutex> lock(completedQueue->Mutex);
        completedQueue->Results.push_back(result);
    });

    tracker.State = TileState::LoadingCoarse;
    ++stream.ActiveJobCount;
}

void TileStreamingManager::DispatchFullJob(TerrainStream& stream,
                                           const TiledTerrainData& tiled,
                                           TileTracker& tracker,
                                           TileCoord coord)
{
    if (!m_JobSystem)
        return;

    auto result = tracker.Result;
    const float32 worldOriginX = tiled.WorldOriginX + coord.X * tiled.Config.TileWorldSize;
    const float32 worldOriginZ = tiled.WorldOriginZ + coord.Z * tiled.Config.TileWorldSize;
    const float32 terrainOriginX = tiled.WorldOriginX;
    const float32 terrainOriginZ = tiled.WorldOriginZ;
    const float32 tileWorld = tiled.Config.TileWorldSize;
    const uint32 fullW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 fullH = tiled.Config.TileConfig.HeightmapHeight;
    // By value, for the same reason as the coarse job's copy.
    const TiledTerrainConfig config = tiled.Config;

    auto completedQueue = stream.Completed;
    const float32 heightScale = tiled.Config.HeightScale;
    const uint32 tileLODLevels = tiled.Config.TileConfig.LODLevels;

    tracker.ActiveJob = m_JobSystem->Submit([result, worldOriginX, worldOriginZ,
                                             terrainOriginX, terrainOriginZ, config,
                                             tileWorld, fullW, fullH, coord,
                                             heightScale,
                                             tileLODLevels, completedQueue]()
    {
        result->Coord = coord;
        result->FullHeightfield.Resize(fullW, fullH, 0.0f);
        FillTiledBaseRegion(result->FullHeightfield, config, terrainOriginX, terrainOriginZ,
                            worldOriginX, worldOriginZ, tileWorld, tileWorld, 0, 0,
                            static_cast<int32>(fullW) - 1, static_cast<int32>(fullH) - 1);

        // Compute accurate min/max.
        float32 minH = std::numeric_limits<float32>::max();
        float32 maxH = std::numeric_limits<float32>::lowest();
        const auto* samples = result->FullHeightfield.GetRawSamples();
        const uint32 count = fullW * fullH;
        for (uint32 i = 0; i < count; ++i)
        {
            minH = std::min(minH, samples[i]);
            maxH = std::max(maxH, samples[i]);
        }
        result->MinHeight = minH;
        result->MaxHeight = maxH;

        // Size the splatmap and build the normalmap in the same job to avoid a race
        // where the heightfield is moved to the tile before the map job can read it.
        // The splat stays unbaked until the modifier bake runs on arrival.
        ResetSplatmap(
            result->FullHeightfield,
            result->Splatmap, result->SplatmapWidth, result->SplatmapHeight);

        GenerateNormalmapFromHeightfield(
            result->FullHeightfield, tileWorld, tileWorld, heightScale,
            result->Normalmap, result->NormalmapWidth, result->NormalmapHeight);

        // Pre-compute quadtree node min/max for main-thread integration.
        const uint32 tileFinestNodes = 1u << (tileLODLevels - 1);
        ComputeQuadtreeNodesFromHeightfield(
            result->FullHeightfield, tileFinestNodes,
            result->QuadtreeNodes, result->QuadtreeNodesPerAxis);

        result->CompletedStage = TileState::FullReady;

        std::lock_guard<std::mutex> lock(completedQueue->Mutex);
        completedQueue->Results.push_back(result);
    });

    tracker.State = TileState::LoadingFull;
    ++stream.ActiveJobCount;
}

// ---- Main-thread integration (budget-limited) ----

void TileStreamingManager::IntegrateCompletedTiles(TerrainStream& stream,
                                                   TiledTerrainData& tiled,
                                                   TerrainService& service)
{
    const TiledTerrainHandle tiledHandle = stream.Handle;

    // Swap completed results out under lock (minimize hold time).
    std::vector<std::shared_ptr<TileGenerationResult>> results;
    {
        std::lock_guard<std::mutex> lock(stream.Completed->Mutex);
        results.swap(stream.Completed->Results);
    }

    if (results.empty())
    {
        stream.LastIntegrationTimeMs = 0.0f;
        return;
    }

    // Sort by priority (nearest camera first).
    std::sort(results.begin(), results.end(),
        [&stream](const auto& a, const auto& b) {
            auto itA = stream.Trackers.find(a->Coord);
            auto itB = stream.Trackers.find(b->Coord);
            if (itA == stream.Trackers.end()) return false;
            if (itB == stream.Trackers.end()) return true;
            return itA->second.Priority < itB->second.Priority;
        });

    const auto startTime = std::chrono::high_resolution_clock::now();
    const float32 budgetMs = stream.TeleportModeActive
        ? m_Config.IntegrationBudgetMsTeleport
        : m_Config.IntegrationBudgetMs;
    uint32 integrated = 0;

    for (auto& result : results)
    {
        // Check budget (but always integrate at least one).
        if (integrated > 0)
        {
            const auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
            const float32 elapsedMs = std::chrono::duration<float32, std::milli>(elapsed).count();
            if (elapsedMs >= budgetMs)
            {
                // Push remaining results back.
                std::lock_guard<std::mutex> lock(stream.Completed->Mutex);
                for (size_t i = integrated; i < results.size(); ++i)
                    stream.Completed->Results.push_back(std::move(results[i]));
                break;
            }
        }

        auto trackerIt = stream.Trackers.find(result->Coord);
        if (trackerIt == stream.Trackers.end())
        {
            // Tile was cancelled/removed while job was running.
            if (stream.ActiveJobCount > 0) --stream.ActiveJobCount;
            ++integrated;
            continue;
        }

        // Only the result the tracker's latest job writes integrates. A job whose tile was
        // unloaded, or cancelled in teleport mode, after it started still delivers once the
        // tile has been requested again; integrating it would put a stale stage in the tile
        // and dispatch the full job onto the Result the tracker's own job is still writing.
        if (trackerIt->second.Result != result)
        {
            if (stream.ActiveJobCount > 0) --stream.ActiveJobCount;
            ++integrated;
            continue;
        }

        // Validate coord is still in bounds for the current tiled terrain.
        // After recreation (size change), old results may reference invalid coords.
        if (result->Coord.X < 0 || result->Coord.Z < 0 ||
            result->Coord.X >= static_cast<int32>(tiled.Config.TilesPerAxisX) ||
            result->Coord.Z >= static_cast<int32>(tiled.Config.TilesPerAxisZ))
        {
            if (stream.ActiveJobCount > 0) --stream.ActiveJobCount;
            stream.Trackers.erase(trackerIt);
            ++integrated;
            continue;
        }

        auto& tracker = trackerIt->second;
        if (stream.ActiveJobCount > 0) --stream.ActiveJobCount;

        switch (result->CompletedStage)
        {
        case TileState::CoarseReady:
        {
            // Ensure tile slot exists.
            if (tiled.Tiles.find(result->Coord) == tiled.Tiles.end())
                service.CreateEmptyTile(tiledHandle, result->Coord);

            // Move coarse heightfield into the tile.
            service.SetTileHeightfield(tiledHandle, result->Coord,
                                       std::move(result->CoarseHeightfield),
                                       result->MinHeight, result->MaxHeight);

            // Move the coarse splatmap only — the coarse job generates no normalmap
            // (extraction uploads a flat-up normal for the coarse region), so pass an
            // empty normal rather than shipping/wiping one. Extraction bilinear-
            // upsamples this splat into the unified tile region.
            if (!result->Splatmap.empty())
            {
                service.SetTileMaps(tiledHandle, result->Coord,
                                    std::move(result->Splatmap),
                                    result->SplatmapWidth, result->SplatmapHeight,
                                    std::vector<uint8>{}, 0, 0);
            }

            // Store pre-computed data on the tile for the extraction system.
            {
                auto tileIt = tiled.Tiles.find(result->Coord);
                if (tileIt != tiled.Tiles.end() && tileIt->second)
                {
                    auto& t = *tileIt->second;
                    t.PrecomputedQuadtreeNodes = std::move(result->QuadtreeNodes);
                    t.PrecomputedQuadtreeNodesPerAxis = result->QuadtreeNodesPerAxis;
                }
            }

            stream.IntegratedThisFrame.push_back(result->Coord);

            // Update global height range incrementally.
            if (result->MinHeight < tiled.CachedGlobalMinH ||
                result->MaxHeight > tiled.CachedGlobalMaxH)
            {
                tiled.CachedGlobalMinH = std::min(tiled.CachedGlobalMinH, result->MinHeight);
                tiled.CachedGlobalMaxH = std::max(tiled.CachedGlobalMaxH, result->MaxHeight);
                tiled.GlobalHeightRangeDirty = false; // We just updated it.
            }

            // Dispatch full upgrade immediately if job slots are available,
            // but reserve 2 slots for new coarse tile loading to prevent
            // upgrade starvation during camera movement.
            if (!stream.TeleportModeActive &&
                stream.ActiveJobCount + 2 < m_Config.MaxConcurrentJobs)
            {
                DispatchFullJob(stream, tiled, tracker, result->Coord);
            }
            else
            {
                // Defer upgrade — EvaluateTilePriorities will schedule it
                // via the priority system on the next frame.
                tracker.State = TileState::Active;
            }
            break;
        }

        case TileState::FullReady:
        {
            // Ensure tile slot exists.
            if (tiled.Tiles.find(result->Coord) == tiled.Tiles.end())
                service.CreateEmptyTile(tiledHandle, result->Coord);

            // Move full heightfield into tile (replaces coarse if present).
            service.SetTileHeightfield(tiledHandle, result->Coord,
                                       std::move(result->FullHeightfield),
                                       result->MinHeight, result->MaxHeight);
            stream.IntegratedThisFrame.push_back(result->Coord);

            // Move splatmap + normalmap (generated in the same job).
            if (!result->Splatmap.empty())
            {
                service.SetTileMaps(tiledHandle, result->Coord,
                                    std::move(result->Splatmap),
                                    result->SplatmapWidth, result->SplatmapHeight,
                                    std::move(result->Normalmap),
                                    result->NormalmapWidth, result->NormalmapHeight);
            }

            // Store pre-computed data on the tile.
            {
                auto tileIt = tiled.Tiles.find(result->Coord);
                if (tileIt != tiled.Tiles.end() && tileIt->second)
                {
                    auto& t = *tileIt->second;
                    t.PrecomputedQuadtreeNodes = std::move(result->QuadtreeNodes);
                    t.PrecomputedQuadtreeNodesPerAxis = result->QuadtreeNodesPerAxis;
                }
            }

            // Update global height range.
            tiled.CachedGlobalMinH = std::min(tiled.CachedGlobalMinH, result->MinHeight);
            tiled.CachedGlobalMaxH = std::max(tiled.CachedGlobalMaxH, result->MaxHeight);
            tiled.GlobalHeightRangeDirty = false;

            tracker.State = TileState::Active;
            tracker.Result.reset();
            break;
        }

        default:
            break;
        }

        ++integrated;
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    stream.LastIntegrationTimeMs = std::chrono::duration<float32, std::milli>(endTime - startTime).count();
}

// ---- Teleportation detection ----

void TileStreamingManager::DetectTeleportation(
    TerrainStream& stream,
    const std::vector<Mathematics::Vector3>& cameraPositions,
    float32 /*deltaTime*/)
{
    if (cameraPositions.empty())
        return;

    const auto& currentPos = cameraPositions[0];

    if (stream.FrameCounter > 1) // Skip first frame
    {
        const float32 dx = currentPos.x - stream.LastCameraPosition.x;
        const float32 dz = currentPos.z - stream.LastCameraPosition.z;
        const float32 dist = std::sqrt(dx * dx + dz * dz);

        // Use absolute distance threshold (not velocity-based) to avoid
        // false positives from fast camera orbiting. The threshold is high
        // enough that normal camera movement (even rapid) won't trigger it.
        if (dist > m_Config.TeleportThreshold)
        {
            stream.TeleportModeActive = true;
            stream.TeleportCooldownFrames = 30; // ~0.5s at 60fps
            LOG_INFO("TileStreaming: teleport detected (dist={:.1f}), entering teleport mode", dist);
        }
    }

    if (stream.TeleportModeActive && stream.TeleportCooldownFrames > 0)
        --stream.TeleportCooldownFrames;

    if (stream.TeleportCooldownFrames == 0 && stream.TeleportModeActive)
    {
        stream.TeleportModeActive = false;
        LOG_INFO("TileStreaming: teleport mode ended, resuming full-detail streaming");
    }

    stream.LastCameraPosition = currentPos;
}

void TileStreamingManager::HandleTeleportMode(
    TerrainStream& stream,
    const TiledTerrainData& tiled,
    const std::vector<Mathematics::Vector3>& cameraPositions,
    float32 streamingRadius)
{
    const float32 streamRSq = streamingRadius * streamingRadius;
    const float32 tileSize = tiled.Config.TileWorldSize;

    // Cancel jobs for tiles that are now distant.
    std::vector<TileCoord> toCancel;
    for (auto& [coord, tracker] : stream.Trackers)
    {
        if (tracker.State == TileState::Active || tracker.State == TileState::Idle)
            continue;

        const float32 tileCenterX = tiled.WorldOriginX + (coord.X + 0.5f) * tileSize;
        const float32 tileCenterZ = tiled.WorldOriginZ + (coord.Z + 0.5f) * tileSize;

        bool inRange = false;
        for (const auto& camPos : cameraPositions)
        {
            const float32 dx = tileCenterX - camPos.x;
            const float32 dz = tileCenterZ - camPos.z;
            if (dx * dx + dz * dz <= streamRSq)
            {
                inRange = true;
                break;
            }
        }

        if (!inRange)
            toCancel.push_back(coord);
    }

    for (const auto& coord : toCancel)
    {
        auto& tracker = stream.Trackers[coord];
        if (tracker.ActiveJob.IsValid() && !tracker.ActiveJob.IsDone())
        {
            tracker.ActiveJob.Cancel();
            if (stream.ActiveJobCount > 0)
                --stream.ActiveJobCount;
        }
        tracker.State = TileState::Idle;
        tracker.Result.reset();
    }
}

// ---- Tile unloading ----

void TileStreamingManager::UnloadDistantTiles(
    TerrainStream& stream,
    TiledTerrainData& tiled,
    TerrainService& service,
    const std::vector<Mathematics::Vector3>& cameraPositions,
    float32 streamingRadius)
{
    const float32 unloadR = streamingRadius * m_Config.UnloadHysteresis;
    const float32 unloadRSq = unloadR * unloadR;
    const float32 tileSize = tiled.Config.TileWorldSize;

    std::vector<TileCoord> tilesToUnload;

    for (auto& [coord, tracker] : stream.Trackers)
    {
        if (tracker.State == TileState::Idle)
            continue;

        const float32 tileCenterX = tiled.WorldOriginX + (coord.X + 0.5f) * tileSize;
        const float32 tileCenterZ = tiled.WorldOriginZ + (coord.Z + 0.5f) * tileSize;

        bool outsideAll = true;
        for (const auto& camPos : cameraPositions)
        {
            const float32 dx = tileCenterX - camPos.x;
            const float32 dz = tileCenterZ - camPos.z;
            if (dx * dx + dz * dz <= unloadRSq)
            {
                outsideAll = false;
                break;
            }
        }

        if (outsideAll)
            tilesToUnload.push_back(coord);
    }

    for (const auto& coord : tilesToUnload)
    {
        auto& tracker = stream.Trackers[coord];

        // Cancel any in-flight job.
        if (tracker.ActiveJob.IsValid() && !tracker.ActiveJob.IsDone())
        {
            tracker.ActiveJob.Cancel();
            if (stream.ActiveJobCount > 0)
                --stream.ActiveJobCount;
        }

        // The unified GPU textures span the whole terrain and are owned by the
        // render feature, so unloading a tile frees only its CPU data — the tile's
        // unified sub-rect simply stops being refreshed until it streams back in.
        service.UnloadTile(stream.Handle, coord);

        static const bool kTileStreamDebug = std::getenv("GE_TERRAIN_TILE_DEBUG") != nullptr;
        if (kTileStreamDebug)
            Logger::Log::Info("Terrain.TileStream out tile=({},{})", coord.X, coord.Z);

        tracker.State = TileState::Idle;
        tracker.Result.reset();
    }

    // Clean up idle trackers to prevent unbounded growth.
    for (const auto& coord : tilesToUnload)
        stream.Trackers.erase(coord);
}

} // namespace GameEngine::TerrainECS
