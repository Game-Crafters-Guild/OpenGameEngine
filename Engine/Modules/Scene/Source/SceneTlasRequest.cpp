// SceneTlas::RequestCurrent and WaitCurrent: bringing the tree current on
// demand. The main thread detects what changed (the ECS change filter and the
// world's structural version) and copies it into a snapshot the TLAS owns;
// the refit or the reconcile then runs as one job on the world's job system,
// reading only that snapshot and the tree, so it never races the next
// frame's systems. Nothing here runs unless a consumer asks.

#include "Scene/SceneTlas.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <vector>

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/ChangeFilter.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TlasInternal.h"

namespace GameEngine::Scene
{

namespace
{

using Engine::Renderer::RenderExtractionSystem;

// Every renderable, as the reconcile consumes it: the same set extraction
// publishes to the GPU scene.
void GatherRecords(ECS::World& world,
                   std::vector<RenderExtractionSystem::WorldRenderableRecord>& records)
{
    using namespace GameEngine::ECS;
    records.clear();
    world.Query<Read<Components::WorldTransform>, Read<Components::MeshRenderer>,
                Optional<Components::LocalBounds>, Optional<Components::WorldSectorCoord>>()
        .Each([&records](EntityHandle entity, const Components::WorldTransform& transform,
                         const Components::MeshRenderer& mesh, const Components::LocalBounds* bounds,
                         const Components::WorldSectorCoord* sector)
        {
            RenderExtractionSystem::WorldRenderableRecord record{};
            record.entity         = entity;
            record.worldTransform = transform;
            record.meshRenderer   = mesh;
            record.hasBounds      = bounds != nullptr;
            if (bounds)
                record.bounds = *bounds;
            record.hasSectorCoord = sector != nullptr;
            if (sector)
                record.sectorCoord = *sector;
            records.push_back(record);
        });
}

using MovedChunk = Internal::SceneTlasImpl::MovedChunk;

void SnapshotMovedChunks(std::span<const MovedChunk> chunks, float32 sectorSize,
                         std::vector<Internal::SceneTlasImpl::RefitInput>& snapshot)
{
    for (const MovedChunk& chunk : chunks)
    {
        for (std::size_t row = 0; row < chunk.Count; ++row)
        {
            Internal::SceneTlasImpl::RefitInput& input = snapshot[chunk.Offset + row];
            input.Entity      = chunk.Entities[row];
            input.Version     = chunk.Transforms[row].Version;
            input.WorldBounds = Internal::DeriveCandidateBounds(
                chunk.Transforms[row], chunk.Bounds ? &chunk.Bounds[row] : nullptr,
                chunk.Sectors ? &chunk.Sectors[row] : nullptr, sectorSize);
        }
    }
}

// The renderables whose WorldTransform column was written since `gate`, as
// the refit needs them: entity, transform version and world bounds (32 bytes
// each, where their components are about 110). One pass over the changed
// chunks records where each chunk's rows are (O(chunks)); the bounds are then
// computed in a fork-join over the chunks on `pool`, the main thread
// participating. The leaf's cached transform version decides later whether
// each one actually moved. Runs between frames, so no structural change can
// move the chunks it read.
void SnapshotMovedRenderables(ECS::World& world, ECS::ChangeGate& gate, float32 sectorSize,
                              JobSystem::WorkStealingThreadPool* pool, std::vector<MovedChunk>& chunks,
                              std::vector<Internal::SceneTlasImpl::RefitInput>& snapshot)
{
    using namespace GameEngine::ECS;
    chunks.clear();
    std::size_t total = 0;
    auto query = world.Query<Read<Components::WorldTransform>, Optional<Components::LocalBounds>,
                             Optional<Components::WorldSectorCoord>>();
    query.With<Components::MeshRenderer>();
    query.Changed<Components::WorldTransform>(gate);
    query.BatchEach([&chunks, &total](const EntityHandle* entities, const Components::WorldTransform* transforms,
                                      const Components::LocalBounds* bounds,
                                      const Components::WorldSectorCoord* sectors, std::size_t count)
    {
        chunks.push_back({entities, transforms, bounds, sectors, count, total});
        total += count;
    });
    snapshot.resize(total);
    constexpr std::size_t kMinChunksPerTask = 4;
    if (!pool || chunks.size() < 2 * kMinChunksPerTask)
    {
        SnapshotMovedChunks(chunks, sectorSize, snapshot);
        return;
    }
    const std::size_t tasks =
        std::min((chunks.size() + kMinChunksPerTask - 1) / kMinChunksPerTask,
                 std::max<std::size_t>(1, pool->GetWorkerCount()) * 4u);
    const std::size_t perTask = (chunks.size() + tasks - 1) / tasks;
    const std::size_t taskCount = (chunks.size() + perTask - 1) / perTask;
    const auto snapshotTask = [&chunks, perTask, sectorSize, &snapshot](std::size_t task)
    {
        const std::size_t begin = task * perTask;
        SnapshotMovedChunks(std::span<const MovedChunk>(chunks.data() + begin, std::min(perTask, chunks.size() - begin)),
                            sectorSize, snapshot);
    };
    JobSystem::ParallelFor(pool, taskCount, snapshotTask);
}

// True when any renderable's LocalBounds column was written since `gate` (a
// mesh reload republishes it; so does an edit). The refit only re-reads
// transforms, so a bounds change takes the reconcile.
bool AnyBoundsWritten(ECS::World& world, ECS::ChangeGate& gate)
{
    using namespace GameEngine::ECS;
    bool written = false;
    auto query = world.Query<Read<Components::LocalBounds>>();
    query.With<Components::MeshRenderer>();
    query.Changed<Components::LocalBounds>(gate);
    query.BatchEach([&written](const Components::LocalBounds*, std::size_t count) { written = written || count > 0; });
    return written;
}

} // namespace

SceneTlas::Readiness SceneTlas::RequestCurrent(GameEngine::ECS::World& world)
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.RequestCurrent");
    Internal::SceneTlasImpl& impl = *m_Impl;
    if (!impl.InFlight.IsZero())
        return Readiness::Updating;

    // The work that just finished may have found a sector crossing (or
    // failed): only a reconcile restores coverage, so the tree is not ready
    // until one has run.
    bool reconcile = impl.InFlightNeedsReconcile.exchange(false, std::memory_order_acq_rel) ||
                     impl.InFlight.HasAnyFailed();
    impl.InFlight.Reset();

    // Entry-sampled, as ECS/ChangeFilter.h requires: a write after this
    // sample is seen by the next request.
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    reconcile = reconcile || !impl.Reconciled || structuralVersion != impl.StructuralVersion;
    if (!ECS::ChangeFilter::Enabled())
    {
        impl.TransformGate.LastRunVersion = 0;
        impl.BoundsGate.LastRunVersion = 0;
    }
    reconcile = AnyBoundsWritten(world, impl.BoundsGate) || reconcile;
    impl.BoundsGate.LastRunVersion = entryVersion;

    JobSystem::WorkStealingThreadPool* pool = world.GetJobSystem();
    impl.InFlightPool = pool; // null: the work runs inline below
    if (reconcile)
    {
        GatherRecords(world, impl.RecordSnapshot);
        impl.StructuralVersion = structuralVersion;
        impl.TransformGate.LastRunVersion = entryVersion; // the records carry every transform
        impl.Reconciled = true;
        if (pool)
        {
            pool->Run([this] { SyncFromRecords(m_Impl->RecordSnapshot); }, impl.InFlight);
            return Readiness::Updating;
        }
        SyncFromRecords(impl.RecordSnapshot);
        return Readiness::Ready;
    }

    SnapshotMovedRenderables(world, impl.TransformGate, impl.SectorSize.load(std::memory_order_relaxed), pool,
                             impl.MovedChunks, impl.RefitSnapshot);
    impl.TransformGate.LastRunVersion = entryVersion;
    if (impl.RefitSnapshot.empty())
    {
#ifndef NDEBUG
        // Nothing was written since the last request, so every leaf must be
        // current: a stale one means a WorldTransform writer bumped Version
        // without a write grant (the change filter never saw it). Sampled:
        // a bounded window on every 64th such request.
        constexpr uint32 kValidatorLeafBudget = 4096u;
        if ((++impl.ValidateCounter & 63u) == 0u)
        {
            const uint32 stale =
                CountStaleLeavesForValidationWindow(world, impl.ValidateCursor, kValidatorLeafBudget);
            if (stale != 0u)
            {
                Logger::Log::Error("[SceneTlas] {} leaf(s) stale with no WorldTransform write recorded "
                                   "since the last request: a writer changed a WorldTransform without a "
                                   "write grant (GetComponentForWrite, a Write<> query or "
                                   "StampComponentWriteBatch). Reconciling.",
                                   stale);
                impl.Reconciled = false;
                return RequestCurrent(world);
            }
        }
#endif
        return Readiness::Ready;
    }

    if (pool)
    {
        pool->Run([this] { RefitFromSnapshot(); }, impl.InFlight);
        return Readiness::Updating;
    }
    RefitFromSnapshot();
    return RequestCurrent(world); // a crossing found inline reconciles now
}

SceneTlas::Readiness SceneTlas::CheckRequest(GameEngine::ECS::World& world)
{
    Internal::SceneTlasImpl& impl = *m_Impl;
    if (!impl.InFlight.IsZero())
        return Readiness::Updating;
    const bool followUp = impl.InFlightNeedsReconcile.load(std::memory_order_acquire) || impl.InFlight.HasAnyFailed();
    return followUp ? RequestCurrent(world) : Readiness::Ready;
}

void SceneTlas::WaitCurrent(GameEngine::ECS::World& world)
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.WaitCurrent");
    while (RequestCurrent(world) == Readiness::Updating)
        m_Impl->InFlightPool->Wait(m_Impl->InFlight);
}

void SceneTlas::RefitFromSnapshot()
{
    GE_CPU_PROFILE_SCOPE("SceneTlas.RefitFromSnapshot");
    Internal::SceneTlasImpl& impl = *m_Impl;
    std::unique_lock lock(impl.Mutex);
    if (impl.EntityToSectors.empty())
        return;
    impl.RefitCandidates.swap(impl.RefitSnapshot);
    JobSystem::WorkStealingThreadPool* pool =
        DefaultRefitExecution() == RefitExecution::Parallel ? impl.InFlightPool : nullptr;
    const SyncResult result = RefitResolvedCandidates(pool);
    impl.RefitCandidates.swap(impl.RefitSnapshot);
    impl.InFlightNeedsReconcile.store(result.NeedsReconcile, std::memory_order_release);
}

} // namespace GameEngine::Scene
