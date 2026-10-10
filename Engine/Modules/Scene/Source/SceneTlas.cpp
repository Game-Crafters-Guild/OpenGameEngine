#include "Scene/SceneTlas.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include "TlasInternal.h"

namespace GameEngine::Scene
{

using Internal::SceneTlasImpl;
using Internal::TlasLeaf;
using Internal::TlasNode;

namespace
{

// Canonical TLAS world-AABB derivation, shared by the refit fast paths and
// ReconcileFromRecords so their bitwise bounds compares stay coherent.
// (Extraction's per-instance bounds are a SPHERE — local center transformed,
// radius scaled by the max axis scale — so there is no cross-system AABB
// derivation to stay in lockstep with; refit design review A3.)
inline Mathematics::AABB DeriveWorldAabb(
    const Components::WorldTransform& wt,
    const Components::LocalBounds& localBounds)
{
    // BoundingBox stores center + halfExtents; convert to min/max corners.
    const auto& box = localBounds.Box;
    const Mathematics::Vector3 minP{
        box.center.x - box.halfExtents.x,
        box.center.y - box.halfExtents.y,
        box.center.z - box.halfExtents.z};
    const Mathematics::Vector3 maxP{
        box.center.x + box.halfExtents.x,
        box.center.y + box.halfExtents.y,
        box.center.z + box.halfExtents.z};
    const float32* m = wt.matrix;

    const Mathematics::Vector3 corners[8] = {
        {minP.x, minP.y, minP.z}, {maxP.x, minP.y, minP.z},
        {minP.x, maxP.y, minP.z}, {maxP.x, maxP.y, minP.z},
        {minP.x, minP.y, maxP.z}, {maxP.x, minP.y, maxP.z},
        {minP.x, maxP.y, maxP.z}, {maxP.x, maxP.y, maxP.z},
    };
    Mathematics::AABB out = Mathematics::AABB::Empty();
    for (int i = 0; i < 8; ++i)
    {
        out.Expand(Mathematics::Vector3(
            m[0] * corners[i].x + m[4] * corners[i].y + m[8]  * corners[i].z + m[12],
            m[1] * corners[i].x + m[5] * corners[i].y + m[9]  * corners[i].z + m[13],
            m[2] * corners[i].x + m[6] * corners[i].y + m[10] * corners[i].z + m[14]));
    }
    return out;
}

// ---- S2 parallel leaf phase tuning --------------------------------------

// Auto-serial threshold (review A2): below this many deduped candidates
// SyncEntities never forks. Fork overhead is ~50-100 us (chunk submission,
// worker wake, participating-Wait join); the post-S1b serial body costs
// ~0.5-0.7 us/candidate, so the paper break-even is ~200 candidates — the
// clamp sits an order higher, where the serial cost first exceeds ~1 ms:
// below that, forking buys under a millisecond per frame and wakes the
// pool during small/idle editor scenes for nothing.
constexpr std::size_t kParallelRefitMinCandidates = 2048;

// Fork shape (extraction A2.1 precedent, RenderExtractionSystem.cpp):
// ~4 chunks per worker so idle workers can steal unevenly-priced chunks,
// floored so small candidate counts don't over-split.
constexpr std::size_t kParallelRefitChunksPerWorker = 4;
constexpr std::size_t kParallelRefitMinChunkSize    = 1000;

// GE_TLAS_PARALLEL_CHUNKS: bench-lane chunk-count override (review A2's
// worker-scaling proxy — no worker-count env knob exists on the pool).
// Latched once; 0/unset = auto shape. Does not bypass the threshold (the
// scaling lanes run well above it); the per-instance test seam does.
uint32 ParallelChunkOverrideFromEnv()
{
    static const uint32 s_Chunks = []
    {
        if (const char* v = std::getenv("GE_TLAS_PARALLEL_CHUNKS"))
            return static_cast<uint32>(std::strtoul(v, nullptr, 10));
        return 0u;
    }();
    return s_Chunks;
}

}  // anon namespace

RefitExecution SceneTlas::DefaultRefitExecution()
{
    // ON unless "0" (house idiom, mirrors GE_TLAS_DIRTY_GATE); latched once
    // per process so a mid-run env change cannot split behavior. The
    // update system reads the same variable at its construction and passes
    // its policy explicitly — this default serves direct-API callers.
    static const RefitExecution s_Default = []
    {
        if (const char* v = std::getenv("GE_TLAS_PARALLEL_REFIT");
            v && std::strcmp(v, "0") == 0)
            return RefitExecution::Serial;
        return RefitExecution::Parallel;
    }();
    return s_Default;
}

SceneTlas::SceneTlas() : m_Impl(std::make_unique<Internal::SceneTlasImpl>()) {}
SceneTlas::~SceneTlas()
{
    // Work still in flight reads and writes this TLAS: join it first.
    if (!m_Impl->InFlight.IsZero() && m_Impl->InFlightPool)
        m_Impl->InFlightPool->Wait(m_Impl->InFlight);
}

uint32 SceneTlas::TraverseRay(const Mathematics::Ray3D& ray,
                              const TraverseRayOptions& options,
                              FunctionRef<bool(const TlasInstance&,
                                               float32 tEnter,
                                               float32 tExit)> visit) const
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.TraverseRay");
    assert(m_Impl->InFlight.IsZero() &&
           "SceneTlas traversed while work is in flight: wait for RequestCurrent to report Ready "
           "(or call WaitCurrent) first");
    std::shared_lock lock(m_Impl->Mutex);
    return Internal::TraverseRayAcrossSectors(*m_Impl, ray, options, visit);
}

uint32 SceneTlas::TraverseSphere(const Mathematics::Vector3& center,
                                 float32 radius,
                                 const TraverseSphereOptions& options,
                                 FunctionRef<void(const TlasInstance&)> visit) const
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.TraverseSphere");
    assert(m_Impl->InFlight.IsZero() &&
           "SceneTlas traversed while work is in flight: wait for RequestCurrent to report Ready "
           "(or call WaitCurrent) first");
    std::shared_lock lock(m_Impl->Mutex);
    return Internal::TraverseSphereAcrossSectors(*m_Impl, center, radius, options, visit);
}

uint32 SceneTlas::TraverseFrustum(std::span<const Mathematics::Plane> planes,
                                  const TraverseFrustumOptions& options,
                                  FunctionRef<void(const TlasInstance&)> visit) const
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.TraverseFrustum");
    assert(m_Impl->InFlight.IsZero() &&
           "SceneTlas traversed while work is in flight: wait for RequestCurrent to report Ready "
           "(or call WaitCurrent) first");
    std::shared_lock lock(m_Impl->Mutex);
    return Internal::TraverseFrustumAcrossSectors(*m_Impl, planes, options, visit);
}

bool SceneTlas::Contains(GameEngine::ECS::EntityHandle entity) const
{
    std::shared_lock lock(m_Impl->Mutex);
    return m_Impl->EntityToSectors.find(entity.id) != m_Impl->EntityToSectors.end();
}

uint32 SceneTlas::InstanceCount() const
{
    std::shared_lock lock(m_Impl->Mutex);
    // Counts unique entities, not replicated leaves. A boundary-spanning
    // entity is one entity even though it has multiple TlasLeaf instances.
    return static_cast<uint32>(m_Impl->EntityToSectors.size());
}

uint32 SceneTlas::NodeCount() const
{
    std::shared_lock lock(m_Impl->Mutex);
    uint32 total = static_cast<uint32>(m_Impl->PrimarySector.Nodes.size());
    for (const auto& kv : m_Impl->Sectors)
        total += static_cast<uint32>(kv.second->Nodes.size());
    return total;
}

bool SceneTlas::IsEmpty() const
{
    std::shared_lock lock(m_Impl->Mutex);
    return m_Impl->EntityToSectors.empty();
}

void SceneTlas::SetSectorSizeForTesting(float32 sectorSize)
{
    std::unique_lock lock(m_Impl->Mutex);
    // Guard the documented invariant: changing sectorSize on a populated
    // TLAS would leave the existing replica sets indexed by the old size,
    // silently desynchronizing routing. Force the caller to Clear() first.
    assert(m_Impl->EntityToSectors.empty()
           && m_Impl->Sectors.empty()
           && m_Impl->PrimarySector.Leaves.empty()
           && m_Impl->EntityStableAabb.empty()
           && "SetSectorSizeForTesting requires an empty TLAS — call Clear() first");
    assert(sectorSize > 0.0f && "sectorSize must be positive");
    m_Impl->SectorSize.store(sectorSize, std::memory_order_relaxed);
}

float32 SceneTlas::GetSectorSize() const
{
    // Lock-free read: SectorSize is std::atomic, and the only writer
    // (SetSectorSizeForTesting) gates on an empty-TLAS assert under the
    // unique_lock so a load racing with a write at runtime would have
    // already violated other invariants.
    return m_Impl->SectorSize.load(std::memory_order_relaxed);
}

void SceneTlas::Clear()
{
    std::unique_lock lock(m_Impl->Mutex);
    auto& primary = m_Impl->PrimarySector;
    primary.Nodes.clear();
    primary.Leaves.clear();
    primary.EntityToLeaf.clear();
    primary.DirtyLeavesScratch.clear();
    m_Impl->Sectors.clear();
    m_Impl->EntityToSectors.clear();
    m_Impl->EntityStableAabb.clear();
}

void SceneTlas::SyncFromRecords(
    std::span<const Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord> records)
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.SyncFromRecords");
    std::unique_lock lock(m_Impl->Mutex);
    Internal::ReconcileFromRecords(*m_Impl, records);
    ++m_Impl->ReconcileCount;
}

namespace
{

// Per-leaf refit body. The caller has passed the Version gate and pushes the
// leaf into its dirty list after this returns. This stores the candidate's
// world bounds (DeriveCandidateBounds, computed where its components were
// read) and raises the sector-crossing reconcile flag.
void RefitLeaf(Internal::SceneTlasImpl& impl,
               Internal::SectorState& sector,
               uint32 leafIndex,
               const Internal::SceneTlasImpl::RefitInput& candidate,
               const Internal::SceneTlasImpl::ReplicaSet* replicas,
               float32 sectorSize,
               bool& needsReconcile)
{
    Internal::TlasLeaf& leaf = sector.Leaves[leafIndex];

    leaf.WorldBounds     = candidate.WorldBounds;
    leaf.CachedXfVersion = candidate.Version;

    // Sector-crossing feedback: the refit updated bounds in-place but
    // cannot migrate replicas. If the new bounds now reach a sector
    // the entity has no replica in, flag a reconcile for next tick.
    // `replicas` is the caller-held EntityToSectors entry (S1.b — no
    // re-find); null means the entity has no current replicas, for which
    // the answer was always false (Sync never inserts).
    if (!needsReconcile && replicas
        && Internal::ReplicaSetWouldChange(
               impl, *replicas, leaf.Entity.id, leaf.WorldBounds, sectorSize))
    {
        needsReconcile = true;
    }
}

}  // anon namespace

SceneTlas::SyncResult SceneTlas::SyncEntities(
    GameEngine::ECS::World& world,
    std::span<const GameEngine::ECS::EntityHandle> entities,
    RefitExecution execution,
    JobSystem::WorkStealingThreadPool* fallbackPool)
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.SyncEntities");
    // Refit of the given candidates, resolved from the world now. The
    // per-leaf body is RequestCurrent's (RefitResolvedCandidates); the
    // CachedXfVersion gate remains the correctness authority, which also
    // makes duplicate candidates no-ops.

    std::unique_lock lock(m_Impl->Mutex);

    // Empty TLAS: nothing can refit, so skip dedup + batch resolve entirely
    // (impl review F7 — a candidate list full of non-renderable movers would
    // otherwise pay the full 3-type resolve for zero refits).
    if (m_Impl->EntityToSectors.empty())
        return SyncResult{};

    // S1.a: dedup the snapshot. The feed's double-buffered window
    // re-delivers ~N duplicate entries under continuous motion, and each
    // dup previously paid the full hash-find + IsValid + GetComponent
    // sequence before the Version gate skipped it. A generation-stamped
    // array keyed on entity INDEX — matching on the FULL 32-bit id so a
    // recycled index with a new version is processed, not skipped (I7) —
    // drops dups in O(1) with first-occurrence order preserved. The
    // surviving visit sequence is the serial sequence minus Version-gated
    // no-ops, so leaf state and DirtyLeavesScratch content/order stay
    // bit-identical to the pre-dedup loop under the single-writer-wave
    // assumption (I9: no WorldTransform writer runs concurrently with this
    // refit wave — the same assumption the un-deduped loop already
    // required; a mid-wave writer could otherwise turn a dup into a second
    // refit that dedup would elide).
    auto& dedup = m_Impl->DedupScratch;
    dedup.clear();
    {
        auto& stamps = m_Impl->DedupStamps;
        if (++m_Impl->DedupGeneration == 0u)
        {
            // uint32 wrap (once per ~4.3e9 waves): stale stamps could alias
            // a reused generation; reset and continue from 1.
            std::fill(stamps.begin(), stamps.end(), Internal::DedupStamp{});
            m_Impl->DedupGeneration = 1u;
        }
        const uint32 generation = m_Impl->DedupGeneration;

        // Size to the world's live entity-index bound, grown on demand —
        // never eagerly to the 2^20 handle-space bound (8 MB resident for a
        // 110k world; review A6). Out-of-bound indexes (hand-built handles
        // in tests; dead by construction) bypass dedup and fall through to
        // the loop's existing dead-handle skips.
        const std::size_t indexBound = world.GetEntityIndexBound();
        if (stamps.size() < indexBound)
            stamps.resize(indexBound);

        dedup.reserve(entities.size());
        for (const GameEngine::ECS::EntityHandle entity : entities)
        {
            if (entity.index < stamps.size())
            {
                Internal::DedupStamp& stamp = stamps[entity.index];
                if (stamp.Generation == generation && stamp.Id == entity.id)
                    continue;
                stamp.Generation = generation;
                stamp.Id         = entity.id;
            }
            dedup.push_back(entity);
        }
    }

    // S1b (review A1): batched component resolve — ONE worldMutex
    // shared-lock hold resolves {WorldTransform, LocalBounds,
    // WorldSectorCoord} for every deduped candidate (embedded IsValid;
    // dead/absent resolve to nullptr), replacing up to three GetComponent
    // calls at two lock cycles each per unique mover. Candidates are
    // resolved and visited in dedup order — no chunk sorting — so the
    // visit sequence, and with it DirtyLeavesScratch content and order,
    // is unchanged. Dereferencing resolved pointers after the world lock
    // is released is the same exposure the per-call code had (it also
    // used each pointer after GetComponentLookup dropped its lock); the
    // single-writer-wave assumption (I9) covers both shapes.
    auto& resolved = m_Impl->ResolvedScratch;
    resolved.resize(dedup.size());
    world.GetComponentsBatch<Components::WorldTransform,
                             Components::LocalBounds,
                             Components::WorldSectorCoord>(
        dedup, std::span{resolved});

    // Dead handles and movers without a WorldTransform resolved to null and
    // are not candidates.
    const float32 sectorSize = m_Impl->SectorSize.load(std::memory_order_relaxed);
    auto& candidates = m_Impl->RefitCandidates;
    candidates.clear();
    for (std::size_t i = 0; i < dedup.size(); ++i)
    {
        const auto& [wt, lb, sc] = resolved[i];
        if (wt)
            candidates.push_back({dedup[i], wt->Version, Internal::DeriveCandidateBounds(*wt, lb, sc, sectorSize)});
    }

    // The world's own pool is authoritative; the fallback is the
    // direct-API/bench seam for an unwired world.
    JobSystem::WorkStealingThreadPool* pool = nullptr;
    if (execution == RefitExecution::Parallel)
        pool = world.GetJobSystem() ? world.GetJobSystem() : fallbackPool;
    return RefitResolvedCandidates(pool);
}

SceneTlas::SyncResult SceneTlas::RefitResolvedCandidates(JobSystem::WorkStealingThreadPool* pool)
{
    // Caller holds the unique lock and has filled RefitCandidates (unique
    // live entities, each with its version and world bounds).
    const float32 sectorSize = m_Impl->SectorSize.load(std::memory_order_relaxed);
    bool needsReconcile = false;
    const auto& dedup = m_Impl->RefitCandidates;

    m_Impl->PrimarySector.DirtyLeavesScratch.clear();
    for (auto& kv : m_Impl->Sectors)
        kv.second->DirtyLeavesScratch.clear();

    // Per-candidate refit body, shared verbatim by the serial loop and the
    // parallel chunks; only the dirty sink and the reconcile flag differ.
    // Workers touch: the candidate table (read), the TLAS maps (read — the
    // only structural writer is reconcile, which takes this same
    // unique_lock), their exclusive leaves (post-dedup an entity appears
    // in exactly one chunk, and all its replica leaves with it), and the
    // caller-owned sink/flag. Zero worldMutex traffic (S1b).
    auto refitCandidate = [&](std::size_t i, auto&& pushDirty, bool& reconcileFlag)
    {
        const Internal::SceneTlasImpl::RefitInput& candidate = dedup[i];
        const GameEngine::ECS::EntityHandle entity = candidate.Entity;
        // Non-TLAS entities miss the replica find: legal candidates, a cheap skip.
        auto replicaIt = m_Impl->EntityToSectors.find(entity.id);
        if (replicaIt == m_Impl->EntityToSectors.end())
            return;

        for (const Internal::SectorKey& key : replicaIt->second)
        {
            Internal::SectorState* sector = Internal::TryGetSector(*m_Impl, key);
            if (!sector)
                continue;
            auto leafIt = sector->EntityToLeaf.find(entity.id);
            if (leafIt == sector->EntityToLeaf.end())
                continue;
            if (candidate.Version == sector->Leaves[leafIt->second].CachedXfVersion)
                continue;

            RefitLeaf(*m_Impl, *sector, leafIt->second, candidate, &replicaIt->second, sectorSize,
                      reconcileFlag);
            pushDirty(sector, leafIt->second);
        }
    };

    // S2 execution decision: a pool means Parallel was asked and one is
    // reachable. The per-instance test seam overrides the shape AND bypasses
    // the auto-serial threshold; the GE_TLAS_PARALLEL_CHUNKS bench knob only
    // overrides the shape above it.
    std::size_t numChunks = 1;
    if (pool && !dedup.empty())
    {
        {
            if (m_Impl->ForcedParallelChunks > 0u)
            {
                numChunks = std::min<std::size_t>(m_Impl->ForcedParallelChunks,
                                                  dedup.size());
            }
            else if (dedup.size() >= kParallelRefitMinCandidates)
            {
                if (const uint32 envChunks = ParallelChunkOverrideFromEnv();
                    envChunks > 0u)
                {
                    numChunks = std::min<std::size_t>(envChunks, dedup.size());
                }
                else
                {
                    const std::size_t workers =
                        std::max<std::size_t>(std::size_t{1}, pool->GetWorkerCount());
                    const std::size_t targetChunks =
                        workers * kParallelRefitChunksPerWorker;
                    const std::size_t chunkSize = std::max(
                        kParallelRefitMinChunkSize,
                        (dedup.size() + targetChunks - 1) / targetChunks);
                    numChunks = (dedup.size() + chunkSize - 1) / chunkSize;
                }
            }
        }
    }

    if (numChunks > 1)
    {
        // Fork-join strictly inside the TLAS unique_lock (I6): Run +
        // participating Wait on a JobCounter — never ParallelFor (the
        // extraction A2.1 precedent). Chunks are contiguous ranges of the
        // deduped array.
        ++m_Impl->ParallelForkCount;
        auto& chunks = m_Impl->ChunkScratch;
        if (chunks.size() < numChunks)
            chunks.resize(numChunks);
        const std::size_t chunkSize = (dedup.size() + numChunks - 1) / numChunks;

        auto runChunk = [&](std::size_t c)
        {
            Internal::SceneTlasImpl::RefitChunkScratch& scratch = chunks[c];
            scratch.Dirty.clear();  // retains capacity across frames
            scratch.NeedsReconcile = false;
            const std::size_t begin = c * chunkSize;
            const std::size_t end   = std::min(begin + chunkSize, dedup.size());
            for (std::size_t i = begin; i < end; ++i)
            {
                refitCandidate(
                    i,
                    [&scratch](Internal::SectorState* sector, uint32 leafIndex)
                    { scratch.Dirty.emplace_back(sector, leafIndex); },
                    scratch.NeedsReconcile);
            }
        };

        JobSystem::JobCounter counter;
        for (std::size_t c = 0; c < numChunks; ++c)
            pool->Run([&runChunk, c] { runChunk(c); }, counter);
        // On a worker this participates (runs only same-counter chunks); on
        // the main thread it parks while workers drain. Deadlock-free
        // either way (extraction CONC-F3 argument).
        pool->Wait(counter);
        // A failed chunk (job-body exception) means a partial leaf pass where
        // the serial loop would have propagated — force the reconcile path,
        // whose full gather supersedes everything (impl review F8).
        needsReconcile = needsReconcile || counter.HasAnyFailed();

        // Merge in chunk-index order: the concatenation of contiguous
        // ranges reproduces the serial visit order exactly, so per-sector
        // DirtyLeavesScratch content AND order are bytewise identical to
        // the serial loop for any chunk count (I2) — and RefitDirty's
        // fixpoint is order-independent besides (review F7). The reconcile
        // flag is an OR-reduce; per-chunk short-circuiting means
        // ReplicaSetWouldChange call COUNTS can differ from serial, but
        // RSWC is pure (F6), so no state diverges and the OR is identical.
        for (std::size_t c = 0; c < numChunks; ++c)
        {
            for (const auto& [sector, leafIndex] : chunks[c].Dirty)
                sector->DirtyLeavesScratch.push_back(leafIndex);
            needsReconcile = needsReconcile || chunks[c].NeedsReconcile;
        }
    }
    else
    {
        for (std::size_t i = 0; i < dedup.size(); ++i)
        {
            refitCandidate(
                i,
                [](Internal::SectorState* sector, uint32 leafIndex)
                { sector->DirtyLeavesScratch.push_back(leafIndex); },
                needsReconcile);
        }
    }

    m_Impl->RefitLeafCount += m_Impl->PrimarySector.DirtyLeavesScratch.size();
    for (auto& kv : m_Impl->Sectors)
        m_Impl->RefitLeafCount += kv.second->DirtyLeavesScratch.size();

    if (!m_Impl->PrimarySector.DirtyLeavesScratch.empty())
        Internal::RefitDirty(m_Impl->PrimarySector);
    for (auto& kv : m_Impl->Sectors)
    {
        if (!kv.second->DirtyLeavesScratch.empty())
            Internal::RefitDirty(*kv.second);
    }

    return SyncResult{needsReconcile};
}

uint32 SceneTlas::CountStaleLeavesForValidation(GameEngine::ECS::World& world) const
{
    std::shared_lock lock(m_Impl->Mutex);

    uint32 stale = 0;
    auto scanSector = [&](const Internal::SectorState& sector)
    {
        for (const Internal::TlasLeaf& leaf : sector.Leaves)
        {
            if (!world.IsValid(leaf.Entity))
                continue;
            const auto* wt = world.GetComponent<Components::WorldTransform>(leaf.Entity);
            if (wt && wt->Version != leaf.CachedXfVersion)
                ++stale;
        }
    };
    scanSector(m_Impl->PrimarySector);
    for (const auto& kv : m_Impl->Sectors)
        scanSector(*kv.second);
    return stale;
}

uint32 SceneTlas::CountStaleLeavesForValidationWindow(GameEngine::ECS::World& world,
                                                      uint64& cursor,
                                                      uint32 budget) const
{
    std::shared_lock lock(m_Impl->Mutex);

    // Flatten the leaf index space [PrimarySector, then map sectors] for
    // this scan. Sector-map order can shift across reconciles; the cursor
    // walks the flat space modulo its total size, so a reshuffle only
    // changes which window a leaf lands in, not its eventual coverage.
    std::vector<const Internal::SectorState*> sectors;
    sectors.reserve(1 + m_Impl->Sectors.size());
    sectors.push_back(&m_Impl->PrimarySector);
    for (const auto& kv : m_Impl->Sectors)
        sectors.push_back(kv.second.get());

    std::size_t total = 0;
    for (const auto* s : sectors)
        total += s->Leaves.size();
    if (total == 0)
        return 0;

    const std::size_t count = std::min<std::size_t>(budget, total);
    std::size_t flat = static_cast<std::size_t>(cursor % total);
    cursor += count;

    // Locate the sector containing the flat start index (skips empties;
    // total > 0 guarantees termination).
    std::size_t sectorIdx = 0;
    while (flat >= sectors[sectorIdx]->Leaves.size())
    {
        flat -= sectors[sectorIdx]->Leaves.size();
        ++sectorIdx;
    }

    uint32 stale = 0;
    for (std::size_t visited = 0; visited < count; ++visited)
    {
        const Internal::TlasLeaf& leaf = sectors[sectorIdx]->Leaves[flat];
        if (world.IsValid(leaf.Entity))
        {
            const auto* wt = world.GetComponent<Components::WorldTransform>(leaf.Entity);
            if (wt && wt->Version != leaf.CachedXfVersion)
                ++stale;
        }
        if (++flat >= sectors[sectorIdx]->Leaves.size())
        {
            flat = 0;
            do
            {
                sectorIdx = (sectorIdx + 1) % sectors.size();
            } while (sectors[sectorIdx]->Leaves.empty());
        }
    }
    return stale;
}

void SceneTlas::SetParallelRefitChunkCountForTesting(uint32 chunks)
{
    std::unique_lock lock(m_Impl->Mutex);
    m_Impl->ForcedParallelChunks = chunks;
}

uint64 SceneTlas::GetRefitLeafCountForTesting() const
{
    std::shared_lock lock(m_Impl->Mutex);
    return m_Impl->RefitLeafCount;
}

uint64 SceneTlas::GetLeafCountForTesting() const
{
    std::shared_lock lock(m_Impl->Mutex);
    uint64 leaves = m_Impl->PrimarySector.Leaves.size();
    for (const auto& kv : m_Impl->Sectors)
        leaves += kv.second->Leaves.size();
    return leaves;
}

bool SceneTlas::IsWorkInFlightForTesting() const
{
    return !m_Impl->InFlight.IsZero();
}

uint64 SceneTlas::GetReconcileCountForTesting() const
{
    std::shared_lock lock(m_Impl->Mutex);
    return m_Impl->ReconcileCount;
}

uint64 SceneTlas::GetParallelRefitForkCountForTesting() const
{
    std::shared_lock lock(m_Impl->Mutex);
    return m_Impl->ParallelForkCount;
}

void SceneTlas::SnapshotTreeForTesting(std::vector<uint8>& out) const
{
    std::shared_lock lock(m_Impl->Mutex);
    out.clear();

    // Field-by-field serialization (never whole-struct memcpy): TlasLeaf
    // carries compiler-inserted padding whose bytes are not guaranteed
    // preserved across copies, and a padding mismatch would fail the
    // parity memcmp spuriously.
    auto append = [&out](const auto& value)
    {
        const auto* bytes = reinterpret_cast<const uint8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    };
    auto appendAabb = [&](const Mathematics::AABB& box)
    {
        append(box.min.x); append(box.min.y); append(box.min.z);
        append(box.max.x); append(box.max.y); append(box.max.z);
    };
    auto appendSector = [&](const Internal::SectorKey& key,
                            const Internal::SectorState& sector)
    {
        append(key.X); append(key.Y); append(key.Z);
        append(static_cast<uint32>(sector.Nodes.size()));
        for (const Internal::TlasNode& node : sector.Nodes)
        {
            appendAabb(node.Bounds);
            append(node.Left);
            append(node.Right);
            append(node.Parent);
            append(node.FirstLeaf);
            append(node.LeafCount);
            append(node.SplitAxis);
        }
        append(static_cast<uint32>(sector.Leaves.size()));
        for (const Internal::TlasLeaf& leaf : sector.Leaves)
        {
            append(leaf.Entity.id);
            appendAabb(leaf.WorldBounds);
            append(leaf.LayerMask);
            append(leaf.InstanceMask);
            append(leaf.CachedXfVersion);
            append(leaf.ParentNode);
            append(leaf.OwningSector.X);
            append(leaf.OwningSector.Y);
            append(leaf.OwningSector.Z);
        }
    };

    // Canonical sector order: primary first, then map sectors sorted by
    // key — two instances built from the same data can differ in map
    // iteration order.
    appendSector(Internal::SectorKey{}, m_Impl->PrimarySector);
    std::vector<std::pair<Internal::SectorKey, const Internal::SectorState*>> sorted;
    sorted.reserve(m_Impl->Sectors.size());
    for (const auto& kv : m_Impl->Sectors)
        sorted.emplace_back(kv.first, kv.second.get());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b)
              {
                  return std::tie(a.first.X, a.first.Y, a.first.Z)
                       < std::tie(b.first.X, b.first.Y, b.first.Z);
              });
    for (const auto& [key, sector] : sorted)
        appendSector(key, *sector);
}

namespace
{

// Per-World registry of TLAS instances, owned by the Scene module. Keyed by
// World::GetWorldId(), which is never reused, so a World constructed at a
// destroyed World's address never inherits its TLAS.
struct TlasRegistry
{
    std::mutex                                         Mutex;
    std::unordered_map<uint64, std::unique_ptr<SceneTlas>> Map;
};

TlasRegistry& Registry()
{
    static TlasRegistry g_Registry;
    return g_Registry;
}

}  // anon namespace

SceneTlas& GetSceneTlas(GameEngine::ECS::World& world)
{
    auto& reg = Registry();
    std::lock_guard guard(reg.Mutex);
    auto& slot = reg.Map[world.GetWorldId()];
    if (!slot)
        slot = std::make_unique<SceneTlas>();
    return *slot;
}

const SceneTlas& GetSceneTlas(const GameEngine::ECS::World& world)
{
    return GetSceneTlas(const_cast<GameEngine::ECS::World&>(world));
}

void ReleaseSceneTlas(GameEngine::ECS::World& world)
{
    auto& reg = Registry();
    std::lock_guard guard(reg.Mutex);
    reg.Map.erase(world.GetWorldId());
}

void ClearAllSceneTlasForTesting()
{
    auto& reg = Registry();
    std::lock_guard guard(reg.Mutex);
    reg.Map.clear();
}

// ---- Full reconciliation (SyncFromRecords and RequestCurrent's job) ----

namespace Internal
{

AABB DeriveCandidateBounds(const Components::WorldTransform& transform,
                           const Components::LocalBounds* bounds,
                           const Components::WorldSectorCoord* sector,
                           float32 sectorSize)
{
    static const Components::LocalBounds kDefaultLocalBounds{};
    const Components::WorldTransform effective =
        Components::ComposeEffectiveWorldTransform(transform, sector, sectorSize);
    return DeriveWorldAabb(effective, bounds ? *bounds : kDefaultLocalBounds);
}

SectorState& GetOrCreateSector(SceneTlasImpl& impl, const SectorKey& key)
{
    if (key.X == 0 && key.Y == 0 && key.Z == 0)
        return impl.PrimarySector;
    auto it = impl.Sectors.find(key);
    if (it != impl.Sectors.end())
        return *it->second;
    auto inserted = impl.Sectors.emplace(key, std::make_unique<SectorState>());
    return *inserted.first->second;
}

namespace
{

// Build a TlasLeaf from a record + pre-computed world AABB. The leaf is
// initially un-parented (ParentNode left at 0, which BuildFromLeaves will
// overwrite during the next rebuild).
TlasLeaf MakeLeafFromRecord(
    const Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord& rec,
    const AABB& worldBounds,
    const SectorKey& owningSector)
{
    TlasLeaf leaf{};
    leaf.Entity          = rec.entity;
    leaf.WorldBounds     = worldBounds;
    leaf.LayerMask       = rec.meshRenderer.renderLayerMask;
    leaf.InstanceMask    = static_cast<uint8>(rec.meshRenderer.renderLayerMask & 0xFFu);
    leaf.CachedXfVersion = rec.worldTransform.Version;
    leaf.OwningSector    = owningSector;
    return leaf;
}

// Compact a sector's Leaves vector and EntityToLeaf map after structural
// changes (inserts/removes/migrations). After compaction, EntityToLeaf
// maps to fresh contiguous indices and BuildFromLeaves can be called.
void CompactAndRebuildSector(SectorState& sector)
{
    if (sector.EntityToLeaf.empty())
    {
        sector.Leaves.clear();
        sector.Nodes.clear();
        return;
    }

    // In-place compaction via std::erase_if: keep leaves whose EntityId
    // is still present in EntityToLeaf, drop the rest. Avoids the prior
    // ~80KB-per-1000-leaves allocation churn on every despawn frame.
    // EntityToLeaf is then re-derived from the compacted layout.
    std::erase_if(sector.Leaves, [&sector](const TlasLeaf& leaf) {
        return sector.EntityToLeaf.find(leaf.Entity.id) == sector.EntityToLeaf.end();
    });
    sector.EntityToLeaf.clear();
    for (uint32 i = 0; i < sector.Leaves.size(); ++i)
        sector.EntityToLeaf[sector.Leaves[i].Entity.id] = i;
    Internal::BuildFromLeaves(sector);
}

// True if the two replica sets contain the exact same SectorKeys.
// Positional compare works because BuildReplicaSet always emits keys in
// row-major iteration order, AND because E.5.4 hysteresis keeps the
// previous (already row-major) replica set verbatim when held — it
// never injects out-of-order keys.
bool ReplicaSetsEqual(const SceneTlasImpl::ReplicaSet& a, const SceneTlasImpl::ReplicaSet& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}

bool ReplicaSetContains(const SceneTlasImpl::ReplicaSet& set, const SectorKey& key)
{
    for (const auto& k : set) if (k == key) return true;
    return false;
}

// E.5.4: hysteresis test. Returns true when the new world AABB has not
// drifted by more than kReplicaHysteresisFraction * sectorSize on any
// axis (min or max corner) since the snapshot was taken. Caller can
// then keep the current replica set even if floor-rounding would
// otherwise migrate it.
bool WithinHysteresis(const Mathematics::AABB& newAabb,
                      const Mathematics::AABB& stableAabb,
                      float32 sectorSize)
{
    const float32 threshold = kReplicaHysteresisFraction * sectorSize;
    return std::abs(newAabb.min.x - stableAabb.min.x) <= threshold
        && std::abs(newAabb.min.y - stableAabb.min.y) <= threshold
        && std::abs(newAabb.min.z - stableAabb.min.z) <= threshold
        && std::abs(newAabb.max.x - stableAabb.max.x) <= threshold
        && std::abs(newAabb.max.y - stableAabb.max.y) <= threshold
        && std::abs(newAabb.max.z - stableAabb.max.z) <= threshold;
}

// Build a replica set: all sectors whose 3D extent overlaps the world
// AABB. Iteration is row-major (Z outermost). Caps at kMaxReplicaSectors;
// returns true if the cap was hit (callers should skip + warn the entity).
bool BuildReplicaSet(const AABB& worldAabb,
                     float32 sectorSize,
                     SceneTlasImpl::ReplicaSet& out)
{
    out.clear();
    const SectorRange range = SectorRangeForAabb(worldAabb, sectorSize);
    const uint32 count = range.Count();
    if (count == 0u)
    {
        // Degenerate AABB (shouldn't happen with finite inputs); fall
        // back to centroid-like assignment so the entity still appears
        // somewhere.
        const Vector3 c{
            (worldAabb.min.x + worldAabb.max.x) * 0.5f,
            (worldAabb.min.y + worldAabb.max.y) * 0.5f,
            (worldAabb.min.z + worldAabb.max.z) * 0.5f,
        };
        out.push_back(SectorKey{
            SectorIndexForCoord(c.x, sectorSize),
            SectorIndexForCoord(c.y, sectorSize),
            SectorIndexForCoord(c.z, sectorSize),
        });
        return false;
    }
    if (count > kMaxReplicaSectors)
        return true;
    out.reserve(count);
    for (int32 z = range.Min.Z; z <= range.Max.Z; ++z)
    for (int32 y = range.Min.Y; y <= range.Max.Y; ++y)
    for (int32 x = range.Min.X; x <= range.Max.X; ++x)
        out.push_back(SectorKey{x, y, z});
    return false;
}

}  // anon namespace

bool ReplicaSetWouldChange(
    SceneTlasImpl& impl,
    const SceneTlasImpl::ReplicaSet& currentReplicas,
    GameEngine::ECS::EntityId eid,
    const AABB& newBounds,
    float32 sectorSize)
{
    // The caller passes the EntityToSectors entry it already holds (S1.b);
    // the no-current-replicas early-out lives at the call sites.
    SceneTlasImpl::ReplicaSet desired;
    const bool capExceeded = BuildReplicaSet(newBounds, sectorSize, desired);
    if (capExceeded)
        return false;  // reconcile also refits-in-place (no migration) at the cap.

    if (ReplicaSetsEqual(currentReplicas, desired))
        return false;

    // Floor-rounded set changed. Apply the same hysteresis reconcile uses:
    // if the bounds barely drifted since the last replica snapshot, reconcile
    // would hold the current set, so no migration would actually occur — don't
    // force a (pointless, thrash-inducing) reconcile.
    auto stableIt = impl.EntityStableAabb.find(eid);
    if (stableIt != impl.EntityStableAabb.end()
        && WithinHysteresis(newBounds, stableIt->second, sectorSize))
        return false;

    return true;
}

void ReconcileFromRecords(
    SceneTlasImpl& impl,
    std::span<const Engine::Renderer::RenderExtractionSystem::WorldRenderableRecord> records)
{
    using Internal::TlasLeaf;

    // Boundsless entities (no LocalBounds component) get a default unit-
    // cube bound at the entity's local origin — matches the legacy
    // RaycastScene fallback for primitives like Sphere/Plane that
    // PrimitiveGenerator doesn't populate LocalBounds on. Without this,
    // primitives become invisible to broad-phase picking.
    static const Components::LocalBounds kDefaultLocalBounds{};

    const float32 sectorSize = impl.SectorSize.load(std::memory_order_relaxed);

    // Per-thread reusable scratch sets — clear() preserves bucket capacity
    // so allocations amortize across Syncs. ReconcileFromRecords runs under
    // SceneTlasImpl::Mutex's unique_lock so concurrent calls on the same
    // thread are serialized; thread-local works because each TLAS instance
    // takes the same scratch in turn (no nesting expected at this layer).
    thread_local std::unordered_set<SectorKey, SectorKeyHash> tl_SectorsRebuild;
    thread_local std::unordered_set<SectorKey, SectorKeyHash> tl_SectorsRefit;
    thread_local std::unordered_set<GameEngine::ECS::EntityId> tl_SeenEntities;
    auto& sectorsRebuild = tl_SectorsRebuild;
    auto& sectorsRefit   = tl_SectorsRefit;
    auto& seenEntities   = tl_SeenEntities;
    sectorsRebuild.clear();
    sectorsRefit.clear();
    seenEntities.clear();
    seenEntities.reserve(records.size());

    // Reusable scratch for per-record replica computation.
    SceneTlasImpl::ReplicaSet newReplicas;

    // 1. Walk records: route each entity into its (potentially replicated)
    //    set of sectors. Diff against the current replica set so we only
    //    touch sectors that gained/lost the entity.
    for (const auto& rec : records)
    {
        const auto eid = rec.entity.id;
        seenEntities.insert(eid);

        const Components::LocalBounds& effectiveBounds =
            rec.hasBounds ? rec.bounds : kDefaultLocalBounds;

        // E.5.2: when entity authors WorldSectorCoord, compose the world
        // matrix as Translate(sectorCoord * sectorSize) * worldTransform.
        const Components::WorldSectorCoord* secPtr =
            rec.hasSectorCoord ? &rec.sectorCoord : nullptr;
        const Components::WorldTransform effectiveXf =
            Components::ComposeEffectiveWorldTransform(rec.worldTransform, secPtr, sectorSize);

        const Mathematics::AABB worldBounds =
            DeriveWorldAabb(effectiveXf, effectiveBounds);

        const bool capExceeded = BuildReplicaSet(worldBounds, sectorSize, newReplicas);
        if (capExceeded)
        {
            // Pathological: an entity AABB so large it would replicate into
            // more than kMaxReplicaSectors. Skip MIGRATION but still REFIT
            // the leaves in the entity's current replica set (if any) so the
            // TLAS reports world-fresh bounds on the existing replicas. A
            // ragdoll explosion or similar in-place AABB growth gets correct
            // bounds in its current sectors; rays clipping NEW sectors that
            // the entity now overlaps will miss it (cap's design intent).
            // The caller should reduce the entity's footprint, raise the
            // cap, or shrink sectorSize.
            auto stableIt = impl.EntityStableAabb.find(eid);
            if (stableIt != impl.EntityStableAabb.end())
            {
                auto curIt = impl.EntityToSectors.find(eid);
                if (curIt != impl.EntityToSectors.end())
                {
                    for (const auto& key : curIt->second)
                    {
                        SectorState* sec = TryGetSector(impl, key);
                        if (!sec) continue;
                        auto leafIt = sec->EntityToLeaf.find(eid);
                        if (leafIt == sec->EntityToLeaf.end()) continue;
                        const uint32 leafIdx = leafIt->second;
                        TlasLeaf& leaf = sec->Leaves[leafIdx];
                        leaf.WorldBounds     = worldBounds;
                        leaf.CachedXfVersion = rec.worldTransform.Version;
                        leaf.LayerMask       = rec.meshRenderer.renderLayerMask;
                        leaf.InstanceMask    = static_cast<uint8>(rec.meshRenderer.renderLayerMask & 0xFFu);
                        sec->DirtyLeavesScratch.push_back(leafIdx);
                        sectorsRefit.insert(key);
                    }
                }
                // Refresh the hysteresis snapshot so a later shrink below the
                // cap doesn't compare against pre-explosion bounds.
                stableIt->second = worldBounds;
            }
            continue;
        }

        auto& currentReplicas = impl.EntityToSectors[eid];

        if (currentReplicas.empty())
        {
            // Brand new entity: insert leaves into each replica sector.
            for (const auto& key : newReplicas)
            {
                SectorState& sec = GetOrCreateSector(impl, key);
                TlasLeaf leaf = MakeLeafFromRecord(rec, worldBounds, key);
                const uint32 newIdx = static_cast<uint32>(sec.Leaves.size());
                sec.Leaves.push_back(leaf);
                sec.EntityToLeaf[eid] = newIdx;
                sectorsRebuild.insert(key);
            }
            currentReplicas = newReplicas;
            impl.EntityStableAabb[eid] = worldBounds;
            continue;
        }

        // Existing entity: compare replica sets.
        bool sameReplicas = ReplicaSetsEqual(currentReplicas, newReplicas);

        // E.5.4: hysteresis. If the floor-rounded set changed but the
        // world AABB only drifted by < kReplicaHysteresisFraction *
        // sectorSize on every axis since the last replica-set snapshot,
        // hold the current replicas. This eliminates per-frame
        // migration on an entity oscillating exactly at a sector edge.
        if (!sameReplicas)
        {
            auto stableIt = impl.EntityStableAabb.find(eid);
            if (stableIt != impl.EntityStableAabb.end()
                && WithinHysteresis(worldBounds, stableIt->second, sectorSize))
            {
                sameReplicas = true;  // pretend nothing changed; refit only.
            }
        }

        if (sameReplicas)
        {
            // Hot path. Refit each replica leaf if AABB or version changed.
            // The same FP-bitwise-compare rationale as E.5.0 applies:
            // DeriveWorldAabb is deterministic for an unchanged entity, so
            // a difference on any axis means real movement (or LocalBounds
            // change). Switch to epsilon compare if FP nondeterminism is
            // ever introduced.
            for (const auto& key : currentReplicas)
            {
                SectorState* sec = TryGetSector(impl, key);
                if (!sec) continue;  // shouldn't happen — defensive.
                auto leafIt = sec->EntityToLeaf.find(eid);
                if (leafIt == sec->EntityToLeaf.end()) continue;
                const uint32 leafIdx = leafIt->second;
                TlasLeaf& leaf = sec->Leaves[leafIdx];
                leaf.LayerMask    = rec.meshRenderer.renderLayerMask;
                leaf.InstanceMask = static_cast<uint8>(rec.meshRenderer.renderLayerMask & 0xFFu);
                const auto& wb = worldBounds;
                const auto& lb = leaf.WorldBounds;
                const bool boundsChanged =
                    wb.min.x != lb.min.x || wb.min.y != lb.min.y || wb.min.z != lb.min.z ||
                    wb.max.x != lb.max.x || wb.max.y != lb.max.y || wb.max.z != lb.max.z;
                if (rec.worldTransform.Version != leaf.CachedXfVersion || boundsChanged)
                {
                    leaf.WorldBounds     = worldBounds;
                    leaf.CachedXfVersion = rec.worldTransform.Version;
                    sec->DirtyLeavesScratch.push_back(leafIdx);
                    sectorsRefit.insert(key);
                }
            }
            continue;
        }

        // Replica set changed: entity migrated across at least one boundary.
        // Remove leaves from sectors only in old set, insert into sectors
        // only in new set, refit in shared sectors.
        for (const auto& oldKey : currentReplicas)
        {
            if (ReplicaSetContains(newReplicas, oldKey))
                continue;  // shared, handled below
            SectorState* sec = TryGetSector(impl, oldKey);
            if (!sec) continue;
            sec->EntityToLeaf.erase(eid);
            // Don't compact yet — we batch all per-sector compactions at
            // end. EntityToLeaf removal is enough to mark the entity gone
            // from this sector's BVH; CompactAndRebuildSector reads the
            // updated map.
            sectorsRebuild.insert(oldKey);
        }
        for (const auto& newKey : newReplicas)
        {
            SectorState& sec = GetOrCreateSector(impl, newKey);
            if (sec.EntityToLeaf.find(eid) != sec.EntityToLeaf.end())
            {
                // Shared: just refit.
                const uint32 leafIdx = sec.EntityToLeaf[eid];
                TlasLeaf& leaf = sec.Leaves[leafIdx];
                leaf.WorldBounds     = worldBounds;
                leaf.CachedXfVersion = rec.worldTransform.Version;
                leaf.LayerMask       = rec.meshRenderer.renderLayerMask;
                leaf.InstanceMask    = static_cast<uint8>(rec.meshRenderer.renderLayerMask & 0xFFu);
                sec.DirtyLeavesScratch.push_back(leafIdx);
                sectorsRefit.insert(newKey);
            }
            else
            {
                // New replica in this sector.
                TlasLeaf leaf = MakeLeafFromRecord(rec, worldBounds, newKey);
                const uint32 newIdx = static_cast<uint32>(sec.Leaves.size());
                sec.Leaves.push_back(leaf);
                sec.EntityToLeaf[eid] = newIdx;
                sectorsRebuild.insert(newKey);
            }
        }
        currentReplicas = newReplicas;
        impl.EntityStableAabb[eid] = worldBounds;  // refresh hysteresis snapshot
    }

    // 2. Despawn pass: entities in EntityToSectors but not in `seenEntities`
    //    were removed from the renderable set this frame. Drop their leaves
    //    from every replica sector and erase the entry.
    std::vector<GameEngine::ECS::EntityId> despawnIds;
    for (const auto& kv : impl.EntityToSectors)
    {
        if (seenEntities.find(kv.first) == seenEntities.end())
            despawnIds.push_back(kv.first);
    }
    for (auto eid : despawnIds)
    {
        const auto& replicas = impl.EntityToSectors[eid];
        for (const auto& key : replicas)
        {
            SectorState* sec = TryGetSector(impl, key);
            if (!sec) continue;
            sec->EntityToLeaf.erase(eid);
            sectorsRebuild.insert(key);
        }
        impl.EntityToSectors.erase(eid);
        impl.EntityStableAabb.erase(eid);
    }

    // 3. Per-sector finalization. Rebuild dominates: a sector that had
    //    both a structural change and a refit only needs the rebuild.
    for (const auto& key : sectorsRebuild)
    {
        SectorState* sec = TryGetSector(impl, key);
        if (!sec) continue;
        sec->DirtyLeavesScratch.clear();  // discard refit work, rebuild covers it
        CompactAndRebuildSector(*sec);
        sectorsRefit.erase(key);
    }
    for (const auto& key : sectorsRefit)
    {
        SectorState* sec = TryGetSector(impl, key);
        if (!sec) continue;
        if (!sec->DirtyLeavesScratch.empty())
            Internal::RefitDirty(*sec);
    }

    // 4. Empty-sector GC: remove non-primary sectors that lost their last
    //    entity. PrimarySector stays even when empty (cheap scaffolding,
    //    matches the E.5.0 single-sector behavior).
    for (auto it = impl.Sectors.begin(); it != impl.Sectors.end(); )
    {
        if (it->second->Leaves.empty() && it->second->EntityToLeaf.empty())
            it = impl.Sectors.erase(it);
        else
            ++it;
    }
}

}  // namespace Internal

}  // namespace GameEngine::Scene
