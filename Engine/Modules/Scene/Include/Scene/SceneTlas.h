#pragma once

#include <memory>
#include <span>
#include <vector>

#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "Scene/FunctionRef.h"
#include "Scene/TlasInstance.h"
#include "Types/Types.h"

namespace GameEngine::ECS { class World; }
namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::Scene::Internal { struct SceneTlasImpl; }

namespace GameEngine::Scene
{

// Execution policy for the SyncEntities refit leaf phase (refit S2).
// Parallel forks the per-candidate loop over the JobSystem pool (chunk-
// local dirty lists merged in chunk-index order — byte-identical output);
// it degrades to the serial loop below the auto-serial threshold or when
// no pool is reachable. Serial pins the S1 loop unconditionally — the
// GE_TLAS_PARALLEL_REFIT=0 kill-switch path.
enum class RefitExecution : uint8
{
    Serial,
    Parallel,
};

// Scene-level CPU spatial index over per-entity world AABBs: O(log N)
// traversals over a binary BVH instead of O(N) entity scans. Editor picking
// is its consumer today; AI line-of-sight, audio occlusion and streaming
// radius queries are candidates. It is not a ray-tracing acceleration
// structure.
//
// Nothing keeps it current per frame. A consumer brings it current on demand
// (RequestCurrent / WaitCurrent): the main thread detects what changed and
// snapshots it, and the refit or reconcile runs on the world's job system
// from that snapshot, never reading the ECS, so the next frame's systems run
// beside it. A world that is never queried never pays for it.
//
// Threading: RequestCurrent and WaitCurrent are main-thread only and run
// between frames. Traversals (TraverseRay/Sphere/Frustum) are valid when the
// tree is ready (no work in flight) and answer for the poses of the last
// request's snapshot; debug builds assert readiness. Contains and the counts
// may be read from any thread and may briefly block on in-flight work.
//
// Lifetime: per-World. Storage owned by the Scene module's internal
// registry (see GetSceneTlas / ReleaseSceneTlas free functions). Editor
// World-swap paths (play-mode start, scene close) must call
// ReleaseSceneTlas to free the TLAS for the destroyed World.

// Lifetime: option structs are read by-value at the call site; storage
// referenced by `IgnoreEntities` (a span) must outlive the Traverse* call.
//
// `IncludeDisabled` is currently a NO-OP at the traversal layer: the query
// engine excludes disabled rows by default, so the TLAS's record gather and
// RenderExtractionSystem never see them and they never enter the TLAS in the
// first place. Setting it true does not resurrect them. To honor it requires extracting all entities and tagging the
// disabled ones at the leaf — deferred until a consumer (e.g. an editor
// "reveal hidden" mode) demonstrably needs it.
struct TraverseRayOptions
{
    float32                                                MaxDistance     = std::numeric_limits<float32>::infinity();
    uint32                                                 LayerMask       = 0xFFFFFFFFu;
    bool                                                   IncludeDisabled = false;  // see header note above
    std::span<const GameEngine::ECS::EntityHandle>         IgnoreEntities{};  // ≤8 typical; filtered at leaf-visit
};

struct TraverseSphereOptions
{
    uint32 LayerMask       = 0xFFFFFFFFu;
    bool   IncludeDisabled = false;  // see header note above TraverseRayOptions
};

struct TraverseFrustumOptions
{
    uint32 LayerMask       = 0xFFFFFFFFu;
    bool   IncludeDisabled = false;  // see header note above TraverseRayOptions
};

class SceneTlas
{
public:
    SceneTlas();
    ~SceneTlas();
    SceneTlas(const SceneTlas&)            = delete;
    SceneTlas& operator=(const SceneTlas&) = delete;

    // ---- Read-side (thread-safe; may briefly block on rebuild) -----------

    // Visit every leaf whose AABB intersects the ray, in entry-t order.
    // Visit returns true to continue, false to stop early. The shared lock
    // is released around each callback invocation so heavy work (BLAS
    // dispatch) inside the callback doesn't block writers.
    //
    // Returns leaf-visit count (diagnostic).
    uint32 TraverseRay(const Mathematics::Ray3D& ray,
                       const TraverseRayOptions& options,
                       FunctionRef<bool(const TlasInstance&,
                                        float32 tEnter,
                                        float32 tExit)> visit) const;

    // Visit every leaf whose AABB overlaps the sphere. NO ordering. Visit
    // returns void — early-exit on unordered traversal would yield a non-
    // deterministic subset.
    uint32 TraverseSphere(const Mathematics::Vector3& center,
                          float32 radius,
                          const TraverseSphereOptions& options,
                          FunctionRef<void(const TlasInstance&)> visit) const;

    // Visit every leaf whose AABB is touched by the frustum (per-plane test
    // with inward-pointing normals: AABB rejected if any plane reports it
    // strictly outside). NO ordering; visit returns void.
    uint32 TraverseFrustum(std::span<const Mathematics::Plane> planes,
                           const TraverseFrustumOptions& options,
                           FunctionRef<void(const TlasInstance&)> visit) const;

    // Diagnostics
    bool   Contains(GameEngine::ECS::EntityHandle entity) const;
    uint32 InstanceCount() const;
    uint32 NodeCount() const;
    bool   IsEmpty() const;

    // ---- Bringing the tree current (main thread, between frames) --------

    enum class Readiness : uint8
    {
        Ready,    // traversable now
        Updating, // work is in flight on the job system
    };

    /// Brings the tree current with `world` on demand and reports whether it
    /// can be traversed now. Detects what changed since the last request: a
    /// structural change or a LocalBounds write reconciles from a gather of
    /// every renderable; otherwise the entities in chunks whose
    /// WorldTransform was written are refit. The changed data is copied into
    /// a snapshot here, and the work runs on the world's job system (inline
    /// on a world without one), so the main thread pays the detection and the
    /// copy only. Nothing changed: Ready, no work. Work in flight: joins it
    /// and reports Updating until it finishes; a refit that moves an entity
    /// into a sector it has no replica in is followed by a reconcile before
    /// the tree reports Ready.
    Readiness RequestCurrent(GameEngine::ECS::World& world);

    /// Finishes the last request without looking for new changes: Updating
    /// while its work is in flight (or while the reconcile that follows a
    /// sector crossing runs), Ready once the tree answers for that request's
    /// snapshot. For a caller resolving a query it deferred until the work it
    /// waited for is done; calling RequestCurrent instead would start new work
    /// whenever anything kept moving, and the query would never resolve.
    Readiness CheckRequest(GameEngine::ECS::World& world);

    /// RequestCurrent, then joins the work with a participating wait until
    /// the tree is ready. For callers that need an answer inside the same call
    /// (placement and drape tools); blocks only when something changed since
    /// the last request.
    void WaitCurrent(GameEngine::ECS::World& world);

    // ---- Building blocks (also the test seams) --------------------------

    // Outcome of a refit. NeedsReconcile is set when a refitted leaf's new
    // world AABB overlaps a sector its entity has no replica in: a refit
    // cannot migrate replicas (that needs the record set), so a reconcile
    // must follow to restore sector coverage.
    struct SyncResult
    {
        bool NeedsReconcile = false;
    };

    // Process-wide default execution policy for SyncEntities: latched once
    // from GE_TLAS_PARALLEL_REFIT (ON unless "0", the house kill-switch
    // idiom) at first use, so a mid-run env change cannot split behavior.
    // RequestCurrent's refit uses it, and so do direct-API callers (tests), letting the whole suite run under either
    // state via the env var (review A5).
    static RefitExecution DefaultRefitExecution();

    // Refit of the given candidate entities, reading their components from
    // `world` now: the same per-leaf refit body RequestCurrent's job runs on
    // its snapshot. The CachedXfVersion compare remains the correctness
    // authority: duplicate entries, dead entities, and non-TLAS entities are
    // all tolerated and skipped (the candidates are deduped internally).
    // Structural changes stay on the reconcile path.
    //
    // S2: under RefitExecution::Parallel the leaf phase forks over a
    // JobSystem pool — the world's own if wired (World::GetJobSystem() is
    // authoritative; null = deliberately serial), else `fallbackPool`, a
    // direct-API/bench seam for injecting a pool into an unwired world
    // (SceneTlas itself stays Engine-free). Output is byte-identical to
    // Serial: chunks are
    // contiguous ranges of the deduped candidate array and chunk-local
    // dirty lists merge in chunk-index order, so per-sector dirty order
    // reproduces the serial visit order exactly; the internal-node pass
    // (RefitDirty) is serial in both modes. Degrades to the serial loop
    // below the auto-serial threshold or without a pool.
    SyncResult SyncEntities(GameEngine::ECS::World& world,
                            std::span<const GameEngine::ECS::EntityHandle> entities,
                            RefitExecution execution = DefaultRefitExecution(),
                            JobSystem::WorkStealingThreadPool* fallbackPool = nullptr);

    // Validation-only scan (debug shadow validator, §5.1): counts leaves
    // whose live WorldTransform.Version differs from CachedXfVersion. After
    // a complete feed-driven refit this must be zero — a nonzero count means
    // a producer bumped Version without emitting a feed entry (the
    // frozen-leaf bug class the feed's kill switch exists for).
    uint32 CountStaleLeavesForValidation(GameEngine::ECS::World& world) const;

    // Bounded variant of the shadow-validator scan (refit-arc rider S0):
    // visits at most `budget` leaves per call, resuming from the rotating
    // `cursor` so successive firings cover the whole scene round-robin.
    // The full O(leaves) scan was a ~20-35 ms DebugFast hitch at 110k
    // leaves on every 256th feed frame; a bounded window keeps the
    // protection default-on at ~1 ms per firing. The cursor is a sampling
    // heuristic, not an exact bookmark — sector-map iteration order can
    // shift across reconciles, which only reshuffles WHICH window a leaf
    // lands in, never whether it is eventually visited.
    uint32 CountStaleLeavesForValidationWindow(GameEngine::ECS::World& world,
                                               uint64& cursor,
                                               uint32 budget) const;

    // Full reconciliation from a per-entity record vector (typically the
    // one published by RenderExtractionSystem). Inserts new entities,
    // despawns entities no longer in the record set, refits movers based
    // on WorldTransform.Version diffs, triggers full rebuild on
    // structural change. RequestCurrent's reconcile job runs it on its
    // gathered records.
    void SyncFromRecords(
        std::span<const Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord> records);

    void Clear();

    // Override the per-instance sector size (default Components::kWorldSectorSize
    // = 1024m, the same scale the camera-relative render path uses). Test
    // fixtures call this with a smaller value (e.g. 50m or 100m) to exercise
    // multi-sector replication without spreading test entities thousands of
    // meters apart. Production code leaves the default. Calling this on a
    // non-empty TLAS is a logic error (existing replica sets become stale);
    // call after construction or after Clear().
    void SetSectorSizeForTesting(float32 sectorSize);
    float32 GetSectorSize() const;

    // Test seam (S2 parity/determinism tests): force the parallel leaf
    // phase to exactly `chunks` chunks, bypassing the auto-serial threshold
    // so small in-process workloads exercise a genuine fork. 1 pins the
    // serial loop; 0 restores the auto shape. Production leaves this at 0
    // (the GE_TLAS_PARALLEL_CHUNKS env knob is the bench-lane override).
    void SetParallelRefitChunkCountForTesting(uint32 chunks);

    // Test/diagnostic: leaves refit by any path since construction, and
    // reconciles run (SyncFromRecords calls, RequestCurrent's included).
    uint64 GetRefitLeafCountForTesting() const;
    uint64 GetReconcileCountForTesting() const;
    // Test/diagnostic: leaves in every sector (an entity has one per sector
    // replica, so this can exceed InstanceCount).
    uint64 GetLeafCountForTesting() const;
    // Test seam: whether RequestCurrent's work is still running.
    bool IsWorkInFlightForTesting() const;

    // Test/diagnostic: number of SyncEntities calls that actually forked.
    // Lets tests assert the poll lane, overflow frames, and below-threshold
    // candidate counts never fork.
    uint64 GetParallelRefitForkCountForTesting() const;

    // Test-only: serialize every sector's BVH (canonical sector order,
    // field-by-field — no struct padding bytes) so parity tests can memcmp
    // serial-vs-parallel refit results and repeated runs.
    void SnapshotTreeForTesting(std::vector<uint8>& out) const;

private:
    // The refit core shared by SyncEntities and RequestCurrent's job: the
    // caller holds the unique lock and has filled the impl's candidate and
    // resolved-component scratch in matching order. A non-null pool forks
    // the leaf phase.
    SyncResult RefitResolvedCandidates(JobSystem::WorkStealingThreadPool* pool);
    // RequestCurrent's refit job: the refit core over the kick's snapshot.
    void RefitFromSnapshot();

    // Defined in Source/TlasInternal.h. Public free functions (Detail::*)
    // operate on it. The unique_ptr is destroyed in SceneTlas.cpp where
    // SceneTlasImpl is complete.
    std::unique_ptr<Internal::SceneTlasImpl> m_Impl;
};

// Per-World accessor. Lazy-creates on first call. The TLAS is owned by an
// internal registry keyed by World::GetWorldId(); lifetime is bounded by explicit
// ReleaseSceneTlas calls from World-swap paths.
SceneTlas&       GetSceneTlas(GameEngine::ECS::World& world);
const SceneTlas& GetSceneTlas(const GameEngine::ECS::World& world);

// Release the TLAS associated with a World. Editor lifecycle code (play-
// mode start/stop, scene close, project unload) must call this when the
// World is about to be destroyed. Idempotent: safe to call when no TLAS
// exists for the World.
void ReleaseSceneTlas(GameEngine::ECS::World& world);

// Test-side helper: drop all per-World TLAS state. Used in test teardown
// to prevent cross-test pollution.
void ClearAllSceneTlasForTesting();

}
