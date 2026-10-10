#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "AssetCore/GUID.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"

#include "Components/Rendering/WorldSectorCoord.h" // kWorldSectorSize (unified sector scale)
#include "ECS/ChangeFilter.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "JobSystem/JobCounter.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "Scene/SceneTlas.h"
#include "Types/Types.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::Scene::Internal
{

using Mathematics::AABB;
using Mathematics::Vector3;

constexpr uint32 kLeafCapacity   = 4u;       // max leaves per BVH leaf node
constexpr uint32 kSahBins        = 16u;
constexpr uint32 kMaxStackDepth  = 128u;     // tree depth ceiling for traversal stack

// E.5.1: hard cap on how many sectors a single entity can replicate into.
// 4 covers any entity up to roughly 2*sectorSize across (a 200m bridge in a
// 100m sector grid spans 2x2 = 4 cells in the worst case). The cap protects
// against pathological inputs (e.g. a 10km bounding box with sectorSize=10m
// would otherwise replicate into millions of sectors). Skipping route when
// exceeded; the existing replica-set leaves still refit so bounds stay
// fresh under in-place AABB growth.
constexpr uint32 kMaxReplicaSectors = 64u;

// E.5.3: hard cap on 3D-DDA grid-walk steps to defend against very long
// rays at tiny sectorSize. 1024 cells is enough for a 100km ray at
// sectorSize=100m, well past any practical use.
constexpr uint32 kMaxRayDdaSteps = 1024u;

// E.5.4: replica-set migration hysteresis. An entity whose world AABB
// drifted by less than this fraction of sectorSize on every axis keeps
// its current replica set even if the centroid technically crossed a
// sector boundary, avoiding per-frame thrash on entities oscillating
// exactly at edges. Picked at 10% to match the plan's specification.
constexpr float32 kReplicaHysteresisFraction = 0.10f;

// Trivially-copyable small-vector with N inline slots and a heap fallback
// for the rare overflow case. Used by ReplicaSet so the ~99% K=1 path
// pays no heap allocation; the rare boundary-spanning entity (K up to
// kMaxReplicaSectors=64) heap-allocates the spill once per migration.
//
// T must be trivially copyable. The class is NOT a drop-in for std::vector
// — it only exposes what ReplicaSet needs (size, empty, [], push_back,
// clear, range-based iteration, copy/move).
template<typename T, std::size_t N>
class SmallVec
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SmallVec<T,N> requires T to be trivially copyable; "
                  "non-trivial destruction is not supported.");

public:
    SmallVec() noexcept = default;
    ~SmallVec() noexcept { reset_storage(); }

    SmallVec(const SmallVec& o) { copy_from(o); }
    SmallVec(SmallVec&& o) noexcept { move_from(std::move(o)); }
    SmallVec& operator=(const SmallVec& o)
    {
        if (this != &o) { reset_storage(); copy_from(o); }
        return *this;
    }
    SmallVec& operator=(SmallVec&& o) noexcept
    {
        if (this != &o) { reset_storage(); move_from(std::move(o)); }
        return *this;
    }

    std::size_t size()  const noexcept { return m_Size; }
    bool        empty() const noexcept { return m_Size == 0; }

    T*       begin()       noexcept { return data(); }
    T*       end()         noexcept { return data() + m_Size; }
    const T* begin() const noexcept { return data(); }
    const T* end()   const noexcept { return data() + m_Size; }

    T&       operator[](std::size_t i)       noexcept { return data()[i]; }
    const T& operator[](std::size_t i) const noexcept { return data()[i]; }

    void clear() noexcept { m_Size = 0; }

    void reserve(std::size_t n)
    {
        if (n <= capacity()) return;
        grow_to(n);
    }

    void push_back(const T& v)
    {
        if (m_Size == capacity())
            grow_to(m_Size == 0 ? N : m_Size * 2);
        data()[m_Size++] = v;
    }

private:
    std::size_t capacity() const noexcept { return m_OnHeap ? m_HeapCap : N; }

    T*       data()       noexcept { return m_OnHeap ? m_Heap : reinterpret_cast<T*>(&m_Inline); }
    const T* data() const noexcept { return m_OnHeap ? m_Heap : reinterpret_cast<const T*>(&m_Inline); }

    void grow_to(std::size_t newCap)
    {
        if (newCap < N) newCap = N;
        T* newHeap = static_cast<T*>(::operator new(newCap * sizeof(T), std::align_val_t{alignof(T)}));
        if (m_Size > 0)
            std::memcpy(newHeap, data(), m_Size * sizeof(T));
        if (m_OnHeap)
            ::operator delete(m_Heap, std::align_val_t{alignof(T)});
        m_Heap    = newHeap;
        m_HeapCap = newCap;
        m_OnHeap  = true;
    }

    void reset_storage() noexcept
    {
        if (m_OnHeap)
            ::operator delete(m_Heap, std::align_val_t{alignof(T)});
        m_OnHeap  = false;
        m_Heap    = nullptr;
        m_HeapCap = 0;
        m_Size    = 0;
    }

    void copy_from(const SmallVec& o)
    {
        if (o.m_Size > N)
            grow_to(o.m_Size);
        if (o.m_Size > 0)
            std::memcpy(data(), o.data(), o.m_Size * sizeof(T));
        m_Size = o.m_Size;
    }

    void move_from(SmallVec&& o) noexcept
    {
        if (o.m_OnHeap)
        {
            // Steal the heap buffer; o becomes empty/inline.
            m_Heap    = o.m_Heap;
            m_HeapCap = o.m_HeapCap;
            m_OnHeap  = true;
            m_Size    = o.m_Size;
            o.m_Heap    = nullptr;
            o.m_HeapCap = 0;
            o.m_OnHeap  = false;
            o.m_Size    = 0;
        }
        else if (o.m_Size > 0)
        {
            // Inline-only — bytewise copy is correct for trivial T.
            std::memcpy(&m_Inline, &o.m_Inline, o.m_Size * sizeof(T));
            m_Size   = o.m_Size;
            o.m_Size = 0;
        }
    }

    alignas(T) std::byte m_Inline[sizeof(T) * N]{};
    T*          m_Heap    = nullptr;
    std::size_t m_HeapCap = 0;
    std::size_t m_Size    = 0;
    bool        m_OnHeap  = false;
};

// Compact 3D sector index. Plain int32 fields (round-2 N2 from the plan
// audit chose three-int over packed bit-fields for portability and hash
// uniformity).
struct SectorKey
{
    int32 X = 0;
    int32 Y = 0;
    int32 Z = 0;
    bool operator==(const SectorKey& o) const noexcept
    {
        return X == o.X && Y == o.Y && Z == o.Z;
    }
};

// Teschner-Heidelberger 3-prime grid hash. Well-distributed for signed grid
// coords; far better than a naive XOR which collides on the diagonal.
struct SectorKeyHash
{
    std::size_t operator()(const SectorKey& k) const noexcept
    {
        return static_cast<std::size_t>(
            static_cast<uint64>(static_cast<uint32>(k.X)) * 73856093ull
          ^ static_cast<uint64>(static_cast<uint32>(k.Y)) * 19349663ull
          ^ static_cast<uint64>(static_cast<uint32>(k.Z)) * 83492791ull);
    }
};

// Hot fields packed at front (Bounds + child indices) for cache-friendly
// non-leaf descent. Cold fields (FirstLeaf, LeafCount, SplitAxis) at back.
struct TlasNode
{
    AABB   Bounds;            // 24 B — hot
    uint32 Left      = 0u;    // child node index (0 if leaf)
    uint32 Right     = 0u;
    uint32 Parent    = 0u;    // for refit propagation; ~0u at root
    uint32 FirstLeaf = 0u;    // index into SectorState::Leaves; valid only when LeafCount > 0
    uint32 LeafCount = 0u;    // 0 == internal
    uint8  SplitAxis = 0u;
    uint8  _pad[3]   = {};
};

struct TlasLeaf
{
    GameEngine::ECS::EntityHandle Entity{};
    AABB                          WorldBounds{};
    uint32                        LayerMask        = 0xFFFFFFFFu;
    uint8                         InstanceMask     = 0xFFu;
    uint8                         _pad             = 0u;
    uint32                        CachedXfVersion  = 0u;  // last-seen WorldTransform.Version
    uint32                        ParentNode       = 0u;  // owning leaf-node index in SectorState::Nodes
    SectorKey                     OwningSector{};         // E.5.1: which sector hosts this replica
};

// Per-sector BVH state. At E.5.0 there is exactly one of these per
// SceneTlas instance (the primary sector at world origin); E.5.1+ adds
// a hash-map of additional sectors keyed by SectorKey.
//
// All BVH-shaped data is dense (std::vector) for cache-coherent traversal.
// EntityToLeaf is the only hash structure on the hot insert/lookup path;
// it stays std::unordered_map for v1 (matches existing behavior) and is
// queued for a flat-hash-map swap when profile data justifies it.
struct SectorState
{
    std::vector<TlasNode>                                    Nodes;
    std::vector<TlasLeaf>                                    Leaves;
    std::unordered_map<GameEngine::ECS::EntityId, uint32>    EntityToLeaf;

    // Reconciliation scratch (kept across calls to avoid re-alloc).
    // DirtyLeavesScratch: leaf indices queued for refit.
    // LeafSeenThisSyncScratch: visited-bitmap for ancestor walk in RefitDirty.
    std::vector<uint32>                                      DirtyLeavesScratch;
    std::vector<uint32>                                      LeafSeenThisSyncScratch;
};

// Feed-snapshot dedup stamp (refit S1.a): one slot per entity INDEX, keyed
// on the full 32-bit id so a recycled index with a new version mismatches
// and is processed, never skipped (invariant I7). Generation bump replaces
// per-call clears; Generation 0 is reserved as "never stamped".
struct DedupStamp
{
    uint32 Generation = 0u;
    uint32 Id         = 0u;
};

// All TLAS state for a single SceneTlas instance. Lives at namespace scope
// (not nested in SceneTlas) so internal headers can refer to it without
// access-control gymnastics.
//
// E.5.1: PrimarySector hosts the (0,0,0) sector. Sectors map holds the rest.
// Per-replica leaves live in their owning sector's storage; the global
// EntityToSectors map records, per entity, the small set of sector keys it
// is replicated into so despawn / migration paths don't have to scan all
// populated sectors.
struct SceneTlasImpl
{
    mutable std::shared_mutex                                Mutex;

    // Hot path's most common sector. Stored inline so single-sector worlds
    // hit it without a hash probe; non-(0,0,0) sectors live in the map.
    SectorState                                              PrimarySector;

    // Non-(0,0,0) sectors. Heap-allocated so a `Sectors` rehash doesn't
    // invalidate `SectorState*` snapshots taken outside the structural
    // lock; the `unique_ptr` is the long-term owner and is only accessed
    // under `Mutex`. (A future lock-free read path would swap this for
    // `std::atomic<SectorState*>` per the E.5 plan's D4 design — deferred
    // until profile shows the lock dominates.)
    std::unordered_map<SectorKey,
                       std::unique_ptr<SectorState>,
                       SectorKeyHash>                        Sectors;

    // E.5.1: per-entity replica set. Typical K=1 (entity in one sector);
    // larger K covers boundary-spanning entities like a 60m bridge in a 50m
    // sector grid. SmallVec<SectorKey, 4> keeps the K<=4 case (which covers
    // every non-pathological case at sectorSize=100m+) heap-free; K up to
    // kMaxReplicaSectors=64 spills to a single per-replica-set heap allocation.
    using ReplicaSet = SmallVec<SectorKey, 4>;
    std::unordered_map<GameEngine::ECS::EntityId, ReplicaSet> EntityToSectors;

    // E.5.4: per-entity world AABB snapshot taken when the current replica
    // set was computed. Used to apply hysteresis on the next Sync — if the
    // new world AABB differs from this snapshot by less than
    // kReplicaHysteresisFraction * SectorSize on every axis, the replica
    // set is held even if floor-rounding would otherwise migrate it.
    std::unordered_map<GameEngine::ECS::EntityId, AABB> EntityStableAabb;

    // Sector dimensions in world units. THE shared sector scale
    // (Components::kWorldSectorSize = 1024m), the same constant the camera-
    // relative render path reconstructs through (RenderOrigin.h::kSectorSize),
    // so a tagged entity's picked/raycast world position matches where it
    // renders. Existing scenes fit inside one sector at world origin and take
    // the no-op single-sector path. Test fixtures override via
    // SetSectorSizeForTesting() to shrink sectors and exercise replication /
    // multi-sector traversal.
    //
    // std::atomic so GetSectorSize() (read by the picker per pick) doesn't
    // need to take Mutex. Writers (SetSectorSizeForTesting) hold the
    // unique_lock anyway and assert the TLAS is empty before storing.
    std::atomic<float32>                                     SectorSize{GameEngine::Components::kWorldSectorSize};

    // Refit-fast-path scratch (SyncEntities, S1.a): dedup stamps sized to
    // the world's live entity-index bound (grown on demand — never eagerly
    // to the 2^20 handle-space bound), the surviving candidate list in
    // first-occurrence order, and the per-wave generation.
    std::vector<DedupStamp>                                  DedupStamps;
    std::vector<GameEngine::ECS::EntityHandle>               DedupScratch;
    uint32                                                   DedupGeneration = 0u;

    // S1b batched-resolve scratch: one entry per deduped candidate, filled
    // by World::GetComponentsBatch under a single worldMutex shared-lock
    // hold. nullptr WorldTransform doubles as the dead/absent skip.
    using ResolvedComponents = std::tuple<const Components::WorldTransform*,
                                          const Components::LocalBounds*,
                                          const Components::WorldSectorCoord*>;
    std::vector<ResolvedComponents>                          ResolvedScratch;

    // What the refit needs of one candidate: its transform version and the
    // world bounds that version gives it. Computed where the components are
    // read (SyncEntities' resolve, or RequestCurrent's kick), so the refit
    // never reads the ECS.
    struct RefitInput
    {
        GameEngine::ECS::EntityHandle Entity{};
        uint32                        Version = 0u;
        AABB                          WorldBounds{};
    };
    // The refit core's input: SyncEntities fills it directly; RequestCurrent's
    // job swaps its snapshot in.
    std::vector<RefitInput>                                  RefitCandidates;

    // S2 parallel leaf phase: persistent per-chunk scratch (the
    // RenderExtractionParallelState precedent — bounded by chunk count,
    // capacity retained across frames). Dirty entries are (sector, leaf)
    // pairs merged into per-sector DirtyLeavesScratch in chunk-index order
    // after the join.
    struct RefitChunkScratch
    {
        std::vector<std::pair<SectorState*, uint32>> Dirty;
        bool                                          NeedsReconcile = false;
    };
    std::vector<RefitChunkScratch>                           ChunkScratch;

    // Test seam: force the fork to exactly N chunks (bypasses the
    // auto-serial threshold; 1 = pinned serial, 0 = auto).
    uint32                                                   ForcedParallelChunks = 0u;
    // Diagnostic: SyncEntities calls that actually forked.
    uint64                                                   ParallelForkCount = 0u;
    // Diagnostics: leaves refit by any path, and reconciles run.
    uint64                                                   RefitLeafCount = 0u;
    uint64                                                   ReconcileCount = 0u;

    // ---- RequestCurrent (SceneTlasRequest.cpp) -------------------------
    // The moved renderables as of the kick: entity, version, world bounds.
    std::vector<RefitInput>                                  RefitSnapshot;
    // One changed chunk's share of the kick: where its columns are and where
    // its rows go in RefitSnapshot. LocalBounds and WorldSectorCoord are
    // optional per chunk (null when the chunk's archetype lacks them).
    struct MovedChunk
    {
        const GameEngine::ECS::EntityHandle* Entities = nullptr;
        const Components::WorldTransform*    Transforms = nullptr;
        const Components::LocalBounds*       Bounds = nullptr;
        const Components::WorldSectorCoord*  Sectors = nullptr;
        std::size_t                          Count = 0;
        std::size_t                          Offset = 0;
    };
    // The kick's chunk list, kept so a request does not allocate it.
    std::vector<MovedChunk>                                  MovedChunks;
    std::vector<Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord> RecordSnapshot;
    // The work in flight (one job), the pool it runs on, and what it found.
    JobSystem::JobCounter                                    InFlight;
    JobSystem::WorkStealingThreadPool*                       InFlightPool = nullptr;
    std::atomic<bool>                                        InFlightNeedsReconcile{false};
    // What the last request saw: whether a reconcile has ever run, the
    // world's structural version, and the change gates (entry-sampled).
    bool                                                     Reconciled = false;
    std::size_t                                              StructuralVersion = 0u;
    GameEngine::ECS::ChangeGate                              TransformGate{};
    GameEngine::ECS::ChangeGate                              BoundsGate{};
    // Debug validator cadence and window cursor (requests that found
    // nothing changed, where every leaf must be current).
    uint32                                                   ValidateCounter = 0u;
    uint64                                                   ValidateCursor = 0u;
};

// Compute the (inclusive) sector index for a 1D world coordinate. Floor
// of (coord / sectorSize). Free function so tests can spot-check.
inline int32 SectorIndexForCoord(float32 worldCoord, float32 sectorSize)
{
    return static_cast<int32>(std::floor(worldCoord / sectorSize));
}

// Compute the inclusive sector range an AABB overlaps. Min-corner gives the
// minimum sector index per axis; max-corner gives the maximum. Caller
// iterates the 3D range (rangeMin..rangeMax inclusive).
struct SectorRange
{
    SectorKey Min;
    SectorKey Max;

    uint32 Count() const noexcept
    {
        const int64 dx = static_cast<int64>(Max.X) - Min.X + 1;
        const int64 dy = static_cast<int64>(Max.Y) - Min.Y + 1;
        const int64 dz = static_cast<int64>(Max.Z) - Min.Z + 1;
        if (dx <= 0 || dy <= 0 || dz <= 0) return 0u;
        return static_cast<uint32>(dx * dy * dz);
    }
};

// World-space AABB of a sector cell. Used for cheap sector-vs-query culls
// in cross-sector traversal (sphere/frustum) and for the 3D-DDA ray walk
// boundary checks.
inline AABB SectorVolumeForKey(const SectorKey& key, float32 sectorSize)
{
    return AABB{
        Vector3{
            static_cast<float32>(key.X)     * sectorSize,
            static_cast<float32>(key.Y)     * sectorSize,
            static_cast<float32>(key.Z)     * sectorSize,
        },
        Vector3{
            static_cast<float32>(key.X + 1) * sectorSize,
            static_cast<float32>(key.Y + 1) * sectorSize,
            static_cast<float32>(key.Z + 1) * sectorSize,
        },
    };
}

inline SectorRange SectorRangeForAabb(const AABB& worldAabb, float32 sectorSize)
{
    return SectorRange{
        SectorKey{
            SectorIndexForCoord(worldAabb.min.x, sectorSize),
            SectorIndexForCoord(worldAabb.min.y, sectorSize),
            SectorIndexForCoord(worldAabb.min.z, sectorSize),
        },
        SectorKey{
            SectorIndexForCoord(worldAabb.max.x, sectorSize),
            SectorIndexForCoord(worldAabb.max.y, sectorSize),
            SectorIndexForCoord(worldAabb.max.z, sectorSize),
        },
    };
}

// Lookup helpers — return nullptr when the sector doesn't exist in the
// Sectors map. PrimarySector ((0,0,0)) is special-cased.
inline SectorState* TryGetSector(SceneTlasImpl& impl, const SectorKey& key)
{
    if (key.X == 0 && key.Y == 0 && key.Z == 0) return &impl.PrimarySector;
    auto it = impl.Sectors.find(key);
    return it != impl.Sectors.end() ? it->second.get() : nullptr;
}

inline const SectorState* TryGetSector(const SceneTlasImpl& impl, const SectorKey& key)
{
    if (key.X == 0 && key.Y == 0 && key.Z == 0) return &impl.PrimarySector;
    auto it = impl.Sectors.find(key);
    return it != impl.Sectors.end() ? it->second.get() : nullptr;
}

SectorState& GetOrCreateSector(SceneTlasImpl& impl, const SectorKey& key);

// Implementation entry points used by the public API. All operate on a
// SectorState; SceneTlas.cpp passes impl.PrimarySector at E.5.0.
void   BuildFromLeaves(SectorState& sector);
void   RefitDirty(SectorState& sector);

// The world bounds a renderable's transform, local bounds (default box when
// absent) and sector coordinate give it, as a leaf stores them.
AABB   DeriveCandidateBounds(const Components::WorldTransform& transform,
                             const Components::LocalBounds* bounds,
                             const Components::WorldSectorCoord* sector,
                             float32 sectorSize);

void   ReconcileFromRecords(
    SceneTlasImpl& impl,
    std::span<const Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord> records);

// Refit-fast-path helper: returns true when `eid`'s refitted world bounds
// would land in a different replica set than `currentReplicas` (the set the
// caller already holds — S1.b threads it through instead of re-finding it
// in EntityToSectors), applying the same BuildReplicaSet + hysteresis rules
// ReconcileFromRecords uses. The refit path can't migrate replicas, so a
// true result means the caller must run a full SyncFromRecords next tick to
// restore coverage. Returns false for the cap-exceeded case (reconcile also
// refits-in-place there); entities with no current replicas are the
// caller's early-out (Sync never inserts).
bool   ReplicaSetWouldChange(
    SceneTlasImpl& impl,
    const SceneTlasImpl::ReplicaSet& currentReplicas,
    GameEngine::ECS::EntityId eid,
    const AABB& newBounds,
    float32 sectorSize);

// Per-sector traversal kernels. Operate on a single SectorState's BVH and
// stream callbacks. The SceneTlas public API wraps these with cross-sector
// iteration + dedup (TraverseRayAcrossSectors and friends below).
uint32 TraverseRay(const SectorState& sector,
                   const Mathematics::Ray3D& ray,
                   const TraverseRayOptions& options,
                   FunctionRef<bool(const TlasInstance&,
                                    float32 tEnter,
                                    float32 tExit)> visit);

uint32 TraverseSphere(const SectorState& sector,
                      const Mathematics::Vector3& center,
                      float32 radius,
                      const TraverseSphereOptions& options,
                      FunctionRef<void(const TlasInstance&)> visit);

uint32 TraverseFrustum(const SectorState& sector,
                       std::span<const Mathematics::Plane> planes,
                       const TraverseFrustumOptions& options,
                       FunctionRef<void(const TlasInstance&)> visit);

// Cross-sector entry points. Iterate the primary sector and every populated
// sector in the map, dedup on EntityId so a replicated boundary-spanning
// entity is reported only once per query. E.5.1 walks all populated sectors
// naively; E.5.3 will replace the iteration with 3D-DDA pruning + a
// top-level BVH over occupied-sector hulls.
uint32 TraverseRayAcrossSectors(const SceneTlasImpl& impl,
                                const Mathematics::Ray3D& ray,
                                const TraverseRayOptions& options,
                                FunctionRef<bool(const TlasInstance&,
                                                 float32 tEnter,
                                                 float32 tExit)> visit);

uint32 TraverseSphereAcrossSectors(const SceneTlasImpl& impl,
                                   const Mathematics::Vector3& center,
                                   float32 radius,
                                   const TraverseSphereOptions& options,
                                   FunctionRef<void(const TlasInstance&)> visit);

uint32 TraverseFrustumAcrossSectors(const SceneTlasImpl& impl,
                                    std::span<const Mathematics::Plane> planes,
                                    const TraverseFrustumOptions& options,
                                    FunctionRef<void(const TlasInstance&)> visit);

}  // namespace GameEngine::Scene::Internal
