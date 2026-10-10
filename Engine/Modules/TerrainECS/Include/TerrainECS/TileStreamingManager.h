#pragma once

#include "TerrainECS/TerrainService.h"
#include "Terrain/Heightfield.h"
#include "Mathematics/Vector3.h"
#include "JobSystem/TaskHandle.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::TerrainECS
{

// Configuration for the tile streaming system.
struct StreamingConfig
{
    float32 IntegrationBudgetMs = 1.0f;    // Max main-thread time per frame for tile integration
    float32 TeleportThreshold = 2000.0f;   // Camera distance jump that triggers teleport mode
    uint32 MaxConcurrentJobs = 8;          // Max simultaneous tile generation jobs
    float32 IntegrationBudgetMsTeleport = 4.0f; // Higher budget during teleport for fast coarse loading
    uint32 CoarseResolution = 129;         // Coarse LOD heightfield resolution
    float32 UnloadHysteresis = 1.5f;       // Unload radius = streaming radius * this
};

// State machine for each tile in the streaming pipeline.
enum class TileState : uint8
{
    Idle,            // Not requested
    Requested,       // In priority queue, job not yet dispatched
    LoadingCoarse,   // Job thread: generating coarse heightfield
    CoarseReady,     // Coarse data complete, awaiting main-thread integration
    LoadingFull,     // Job thread: generating full heightfield
    FullReady,       // Full data complete, awaiting main-thread integration
    Active,          // Fully loaded and integrated
    Unloading        // Marked for removal
};

// Priority for ordering tile load requests.
struct TileStreamingPriority
{
    float32 DistanceSq = 0.0f;
    bool InFrustum = false;
    bool IsUpgrade = false; // true if upgrading from coarse to full

    // Lower is higher priority: in-frustum + near beats out-of-frustum + far.
    bool operator<(const TileStreamingPriority& o) const
    {
        if (InFrustum != o.InFrustum) return InFrustum;
        if (IsUpgrade != o.IsUpgrade) return !IsUpgrade; // New tiles before upgrades
        return DistanceSq < o.DistanceSq;
    }
};

// LH MakeLookAtLH view matrix: row 2 is camera forward (same as
// SkyRenderNode::ExtractCameraVectors and LensFlareRenderNode). Tile priority
// uses XZ only. Identity view / look along +Z => (0, 0, 1).
inline Mathematics::Vector3 TilePriorityViewDirectionXZ(const float view[16])
{
    return Mathematics::Vector3{view[2], 0.0f, view[10]};
}

// Tile is "in front" when camera-to-tile dots the XZ look. A zero look
// (no camera yet) treats every tile as in front so streaming still proceeds.
inline bool TileIsInFrontOfCameraXZ(float tileCenterX, float tileCenterZ,
                                    float camX, float camZ,
                                    const Mathematics::Vector3& viewDirection)
{
    if (viewDirection.x == 0.0f && viewDirection.z == 0.0f)
        return true;
    const float toTileX = tileCenterX - camX;
    const float toTileZ = tileCenterZ - camZ;
    return (toTileX * viewDirection.x + toTileZ * viewDirection.z) > 0.0f;
}

// Self-contained buffer for tile data generated off-thread.
// Job threads write exclusively to this struct — never to TerrainService state.
struct TileGenerationResult
{
    TileCoord Coord;
    TileState CompletedStage = TileState::Idle;

    // Stage 1 output: coarse heightfield
    Terrain::HeightfieldData CoarseHeightfield;

    // Stage 2 output: full heightfield
    Terrain::HeightfieldData FullHeightfield;

    // Stage 3 output: splatmap + normalmap
    std::vector<uint8> Splatmap;
    uint32 SplatmapWidth = 0;
    uint32 SplatmapHeight = 0;
    std::vector<uint8> Normalmap;
    uint32 NormalmapWidth = 0;
    uint32 NormalmapHeight = 0;

    float32 MinHeight = 0.0f;
    float32 MaxHeight = 0.0f;

    // Pre-computed per-node min/max for the global quadtree finest level.
    // Row-major: QuadtreeNodes[nz * tileFinestNodes + nx].
    std::vector<NodeMinMax> QuadtreeNodes;
    uint32 QuadtreeNodesPerAxis = 0;
};

// Manages async tile lifecycle for every tiled terrain. All CPU-intensive work
// (base fill, splatmap/normalmap computation) runs on job threads.
// The main thread performs only budget-limited integration of completed results.
// GPU uploads are NOT this class's concern — it produces CPU tile data (heightfield/
// splat/normal) that the extraction system patches into the unified GPU textures on
// the graphics queue (C8: no per-tile textures, no transfer-queue uploads).
//
// Each tiled terrain streams through its own state (trackers, results queue, job budget,
// teleport detection), keyed by its handle's slot index, so terrains sharing tile
// coordinates never see each other's results. The job budget and the integration budget
// apply per terrain.
class TileStreamingManager
{
public:
    TileStreamingManager(JobSystem::WorkStealingThreadPool* jobSystem,
                         const StreamingConfig& config = {});
    ~TileStreamingManager();

    // Called once per frame for each tiled terrain from TerrainExtractionSystem::Update.
    // Evaluates priorities, dispatches jobs, integrates completed tiles
    // (budget-limited), detects teleportation, and unloads distant tiles.
    // viewDirection is the primary camera's forward vector (XZ only, for
    // frustum-aware priority — tiles in front of the camera load first).
    // Returns the tiles this call integrated (for the incremental quadtree update); the
    // reference stays valid until the next Update or Forget for the same terrain.
    const std::vector<TileCoord>& Update(TiledTerrainHandle tiledHandle,
                                         TiledTerrainData& tiled,
                                         TerrainService& service,
                                         const std::vector<Mathematics::Vector3>& cameraPositions,
                                         Mathematics::Vector3 viewDirection,
                                         float32 streamingRadius,
                                         float32 deltaTime);

    // Cancel one terrain's pending jobs and drop its streaming state (re-provision or
    // teardown of the terrain in that slot). Jobs already running finish into a results
    // queue nothing reads any more.
    void Forget(uint32 tiledIndex);

    // Debug stats, summed over every terrain.
    struct Stats
    {
        uint32 TilesActive = 0;
        uint32 TilesLoading = 0;
        uint32 TilesPending = 0;
        float32 IntegrationTimeMs = 0.0f;
        bool TeleportModeActive = false;
    };
    Stats GetStats() const;

    const StreamingConfig& GetConfig() const { return m_Config; }

private:
    // Per-tile tracking state.
    struct TileTracker
    {
        TileState State = TileState::Idle;
        TileStreamingPriority Priority;
        JobSystem::TaskHandle ActiveJob;
        std::shared_ptr<TileGenerationResult> Result;
        uint64 LastAccessFrame = 0;
    };

    // Completed results queue. Written by job threads, read by main thread.
    // Shared via shared_ptr so that in-flight lambdas that outlive their terrain's state
    // (Cancel stops only a job that has not started) write to a valid object instead of
    // dangling memory.
    struct CompletedQueue
    {
        std::mutex Mutex;
        std::vector<std::shared_ptr<TileGenerationResult>> Results;
    };

    // The streaming state of one tiled terrain.
    struct TerrainStream
    {
        // The terrain this state streams; a different generation in the same slot is a new terrain.
        TiledTerrainHandle Handle;
        std::unordered_map<TileCoord, TileTracker, TileCoordHash> Trackers;
        std::shared_ptr<CompletedQueue> Completed = std::make_shared<CompletedQueue>();

        // Tiles integrated by the latest Update (for incremental quadtree updates).
        std::vector<TileCoord> IntegratedThisFrame;

        // Teleportation state.
        Mathematics::Vector3 LastCameraPosition{0, 0, 0};
        bool TeleportModeActive = false;
        uint32 TeleportCooldownFrames = 0;

        uint64 FrameCounter = 0;
        float32 LastIntegrationTimeMs = 0.0f;
        uint32 ActiveJobCount = 0;
    };

    // Cancel every job of the stream that has not started yet.
    static void CancelPendingJobs(TerrainStream& stream);

    // Cancel every terrain's pending jobs and drop all streaming state (destruction).
    void CancelAll();

    // ---- Priority evaluation ----
    void EvaluateTilePriorities(TerrainStream& stream,
                                const TiledTerrainData& tiled,
                                const std::vector<Mathematics::Vector3>& cameraPositions,
                                Mathematics::Vector3 viewDirection,
                                float32 streamingRadius);

    // ---- Job dispatch ----
    void DispatchPendingJobs(TerrainStream& stream, const TiledTerrainData& tiled);

    void DispatchCoarseJob(TerrainStream& stream,
                           const TiledTerrainData& tiled,
                           TileTracker& tracker,
                           TileCoord coord);

    void DispatchFullJob(TerrainStream& stream,
                         const TiledTerrainData& tiled,
                         TileTracker& tracker,
                         TileCoord coord);

    // ---- Main-thread integration (budget-limited) ----
    void IntegrateCompletedTiles(TerrainStream& stream,
                                 TiledTerrainData& tiled,
                                 TerrainService& service);

    // ---- Teleportation detection ----
    void DetectTeleportation(TerrainStream& stream,
                             const std::vector<Mathematics::Vector3>& cameraPositions,
                             float32 deltaTime);
    static void HandleTeleportMode(TerrainStream& stream,
                                   const TiledTerrainData& tiled,
                                   const std::vector<Mathematics::Vector3>& cameraPositions,
                                   float32 streamingRadius);

    // ---- Tile unloading ----
    void UnloadDistantTiles(TerrainStream& stream,
                            TiledTerrainData& tiled,
                            TerrainService& service,
                            const std::vector<Mathematics::Vector3>& cameraPositions,
                            float32 streamingRadius);

    // ---- State ----
    // Keyed by TiledTerrainHandle::Index; TerrainStream::Handle tells a recycled slot apart.
    std::unordered_map<uint32, TerrainStream> m_Streams;

    // Dependencies.
    StreamingConfig m_Config;
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
};

} // namespace GameEngine::TerrainECS
