#pragma once

// ECS change signaling, piece 2: a per-frame, append-only list of entities
// whose subscribed component changed. One
// subscribed type per World (WorldTransform in P2); the feed itself is
// component-agnostic — subscription and emission live on World.
//
// Storage is a World MEMBER (amendment A3, the m_GlobalSystemVersion
// precedent): member data on one object is image-safe by construction —
// Editor.exe and Engine.dll dereference the same World at the same offsets.
// No inline-global, no WorldId registry (see the seam review's Seam 2 for
// why the registry design was deleted).
//
// The feed is CANDIDATE SELECTION ONLY. Consumers keep their Version
// compare (e.g. SceneTlas CachedXfVersion) as the correctness authority;
// duplicate and dead entries are legal and must be tolerated.
//
// Buffering: double-buffered. Producers append to the pending buffer at any
// point in the frame; the engine tick calls Swap() exactly once per frame
// (C6 precedent — never inside ProcessCommands, which runs mid-frame).
// Consumers read BOTH buffers via Snapshot() at their own wave position:
// pending covers this frame's earlier waves (hierarchy, physics writeback),
// current covers last frame's post-consumer writers (editor tools, late
// managed sets) — so an entry is always readable at at least one consumer
// wave position before it is discarded, wherever in the frame it was
// appended. The overlap re-delivers some entries once; consumers are
// Version-gated so re-delivery is a cheap no-op.
//
// Thread safety: a plain mutex, deliberately. The only genuinely parallel
// producer (TransformHierarchySystem's flat ParallelBatchEach) appends one
// BATCH per chunk of actual movers, amortizing the lock to once per dirty
// chunk; every other producer is a serial main-thread site or already runs
// under worldMutex exclusive. This avoids the thread_local-buffer-per-world
// lifecycle machinery (thumbnail-world churn) and any header-inline static
// state (the Step-0 per-image split bug class).

#include "ECS/ECS.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace GameEngine::ECS
{

class ComponentDirtyFeed
{
public:
    // Per-buffer entry cap. Beyond it the buffer is marked overflowed and
    // further appends drop; consumers seeing Overflowed() must fall back to
    // their full poll for the frame. 2 MB worst case per buffer; a 110k
    // all-movers frame sits well under it.
    static constexpr std::size_t kOverflowCap = 1u << 18;

    void Append(EntityHandle e)
    {
        std::lock_guard lock(m_Mutex);
        AppendLocked(e);
    }

    void AppendBatch(const EntityHandle* entities, std::size_t count)
    {
        if (count == 0)
            return;
        std::lock_guard lock(m_Mutex);
        for (std::size_t i = 0; i < count; ++i)
            AppendLocked(entities[i]);
    }

    // Copies current + pending into out (cleared first). Consumers call this
    // at their wave position; entries may contain duplicates and handles of
    // since-destroyed entities.
    void Snapshot(std::vector<EntityHandle>& out) const
    {
        out.clear();
        std::lock_guard lock(m_Mutex);
        out.reserve(m_Current.size() + m_Pending.size());
        out.insert(out.end(), m_Current.begin(), m_Current.end());
        out.insert(out.end(), m_Pending.begin(), m_Pending.end());
    }

    // Combined current + pending entry count — what Snapshot() would copy,
    // duplicates included. A consumer that only needs to know whether
    // anything moved (extraction's on-demand view-churn check) reads this
    // without paying the copy.
    std::size_t SnapshotSize() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Current.size() + m_Pending.size();
    }

    // True when either readable buffer dropped appends this window —
    // Snapshot() is incomplete and consumers must take their poll fallback.
    bool Overflowed() const
    {
        std::lock_guard lock(m_Mutex);
        return m_CurrentOverflow || m_PendingOverflow;
    }

    // Engine tick, exactly once per frame: discard current, promote pending.
    void Swap()
    {
        std::lock_guard lock(m_Mutex);
        m_Current.swap(m_Pending);
        m_CurrentOverflow = m_PendingOverflow;
        m_Pending.clear();
        m_PendingOverflow = false;
        ++m_SwapGeneration;
    }

    // Swap-window generation for consumer cadence guards
    // (ECS/SwapGenerationGuard.h): increments once per Swap(). Consumers
    // cache the generation they last consumed; a gap > 1 means at least one
    // whole window was swapped out unseen and the consumer must run its
    // recovery pass (the lifecycle-buffer counterpart is
    // LifecycleEventBuffers::SwapGeneration).
    //
    // Mutex-guarded like the increment: a consumer may read from a
    // job-system worker while the engine tick swaps — sequenced by wave
    // dispatch today, but the getter must not rely on that. The member is a
    // plain uint64 under the existing lock (no atomic_ref<const T> on all
    // supported toolchains, and the lock is already here).
    uint64 SwapGeneration() const
    {
        std::lock_guard lock(m_Mutex);
        return m_SwapGeneration;
    }

    // World::Clear() / teardown: wipe both buffers (a feed surviving into a
    // new scene would be a semantic leak — the §6.1 Q4 rationale). The swap
    // generation is deliberately NOT reset: it is monotonic across Clear()
    // so a scene teardown cannot fake an unbroken cadence and hide a real
    // gap — teardown itself is signaled by the structural/reset generations
    // consumers already track.
    void Reset()
    {
        std::lock_guard lock(m_Mutex);
        m_Current.clear();
        m_Pending.clear();
        m_CurrentOverflow = false;
        m_PendingOverflow = false;
    }

private:
    void AppendLocked(EntityHandle e)
    {
        if (m_Pending.size() >= kOverflowCap)
        {
            m_PendingOverflow = true;
            return;
        }
        m_Pending.push_back(e);
    }

    mutable std::mutex m_Mutex;
    std::vector<EntityHandle> m_Pending;  // appended since the last Swap
    std::vector<EntityHandle> m_Current;  // previous window; readable until the next Swap
    uint64 m_SwapGeneration = 0;          // monotonic, survives Reset()
    bool m_PendingOverflow = false;
    bool m_CurrentOverflow = false;
};

} // namespace GameEngine::ECS
