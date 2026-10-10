#pragma once

// ECS change signaling, piece 3 (design v0.2 §6): per-frame Added<T> /
// Removed<T> lifecycle event buffers. Events are recorded once per operation
// class inside the P-1 unified structural bodies (add-or-set structural
// branch, unified remove, DestroyEntityInternal) plus the enumerated
// creation/clone family, for SUBSCRIBED component types only — unsubscribed
// types record nothing (the hook-signature gating pattern).
//
// Storage is a World MEMBER for the same dual-image reason as the P2 dirty
// feed (M9/R10): member data on one object is image-safe by construction —
// Editor.exe and GameEngine.Native.dll dereference the same World at the
// same offsets. No inline-global, no registry.
//
// Buffering: double-buffered per subscribed type. Producers append to the
// pending window; the engine tick calls World::SwapLifecycleEvents() exactly
// once per frame (C6 — never inside ProcessCommands, which has mid-frame
// call sites). Consumers read the CURRENT window only (GetAdded/GetRemoved):
// unlike the dirty feed's both-buffer Snapshot, an event must be delivered
// exactly once — there is no Version-compare authority to dedupe a
// re-delivery against, and Added-driven init work is not idempotent.
//
// LIFETIME vs CONSUMER CADENCE (the dirty-feed review's F1 invariant, stated
// here because events inherit the same double-buffer window WITHOUT the
// feed's poll fallback): an event is readable for exactly ONE swap window.
// A consumer that skips a window (system throttling via
// ConfigureRenderingSystemEveryNFrames, a system disabled mid-session via
// SetRenderingSystemEnabled — the editor's play-mode pause does exactly
// this — conditional stepping, a disabled rendering loop) misses the
// events swapped out during the gap; the buffers have no self-heal. The
// mechanical guard is the SWAP GENERATION: Swap() increments a monotonic
// counter; a consumer caches the generation it last consumed and, on
// observing a gap > 1, runs a one-shot full re-scan of its per-entity
// bookkeeping (the poll it replaced) before trusting the current window
// again. The gap arithmetic is encapsulated in ECS/SwapGenerationGuard.h;
// AudioEmitterSystem is the reference consumer of the gap-heal pattern.
//
// Delivery is per-WINDOW, not per-read: the editor can step the system
// waves more than once between swaps (RefreshWorldRenderForPassiveFrame
// calls StepRenderingLoop on top of the auto-driven tick), so a consumer
// may see the SAME spans on every run within one window and must be
// idempotent within it (insert-if-absent, erase-tolerates-missing).
//
// Visibility inversion (C6): structural ops executed via a mid-frame flush
// land in pending and surface NEXT frame — an entity can be query-visible up
// to a full frame before its Added event. Consumers must tolerate "entity
// exists but Added not yet delivered" (init keyed off the event, per-frame
// work off the query).
//
// Removed carries NO data: the component is already destroyed at consumption
// time and the entity itself may be dead — treat the handle as an ID, never
// dereference. Removal-with-data stays exclusively on RegisterOnRemove hooks
// (they receive a live T& pre-destruction).
//
// World::Clear() does not burst per-entity events (Q4): it wipes BOTH
// windows and bumps the reset generation — consumers compare
// GetLifecycleResetGeneration() against their cached value and drop all
// per-entity bookkeeping on change.
//
// Thread safety: NONE internally, deliberately. Every producer site runs
// under worldMutex exclusive (the unified bodies and the creation family all
// require it), World::SwapLifecycleEvents() takes worldMutex, and consumers
// read the current window from schedule waves that never overlap the
// engine-tick swap. No destroy-storm overflow cap (R7): scene unload is
// Clear (a signal, not a burst), mid-frame storms are bounded by the
// once-per-frame swap, and the vectors retain capacity across frames.

#include "ECS/ECS.h"
#include "Types/GeometricReserve.h"

#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::ECS
{

class LifecycleEventBuffers
{
public:
    // Bootstrap-time registration (idempotent). Immutable once systems run —
    // recording sites read the subscription without synchronization.
    void Register(ComponentTypeId typeId)
    {
        if (typeId == 0 || Find(typeId))
            return;
        m_Types.push_back(TypeBuffers{typeId});
    }

    bool IsRegistered(ComponentTypeId typeId) const { return Find(typeId) != nullptr; }
    bool Empty() const { return m_Types.empty(); }

    // Producers (caller holds worldMutex exclusive). Appends to the pending
    // window; silently ignores unregistered types (callers pre-gate on the
    // World's lifecycle-event signature).
    void AppendAdded(ComponentTypeId typeId, EntityHandle entity)
    {
        if (TypeBuffers* t = Find(typeId))
            t->PendingAdded.push_back(entity);
    }

    void AppendRemoved(ComponentTypeId typeId, EntityHandle entity)
    {
        if (TypeBuffers* t = Find(typeId))
            t->PendingRemoved.push_back(entity);
    }

    // Entity destruction prepares all pending slots before OnRemove can release
    // external ownership.
    void PrepareRemoved(ComponentTypeId typeId, std::size_t count)
    {
        if (TypeBuffers* t = Find(typeId))
            ReserveForAppend(t->PendingRemoved, count);
    }

    void AppendAddedBatch(ComponentTypeId typeId, const EntityHandle* entities, std::size_t count)
    {
        TypeBuffers* t = Find(typeId);
        if (!t || count == 0)
            return;
        t->PendingAdded.insert(t->PendingAdded.end(), entities, entities + count);
    }

    // Consumers: the current window only (delivered exactly once, one frame).
    // Returns an empty span for unregistered types.
    std::span<const EntityHandle> CurrentAdded(ComponentTypeId typeId) const
    {
        const TypeBuffers* t = Find(typeId);
        return t ? std::span<const EntityHandle>(t->CurrentAdded) : std::span<const EntityHandle>{};
    }

    std::span<const EntityHandle> CurrentRemoved(ComponentTypeId typeId) const
    {
        const TypeBuffers* t = Find(typeId);
        return t ? std::span<const EntityHandle>(t->CurrentRemoved) : std::span<const EntityHandle>{};
    }

    // Frame boundary: discard current, promote pending. swap+clear retains
    // vector capacity across frames. Increments the swap generation — the
    // consumer-cadence guard (see header): consumers compare against the
    // generation they last consumed; a gap > 1 means at least one window
    // was swapped out unseen and per-entity bookkeeping must be re-scanned.
    void Swap()
    {
        for (TypeBuffers& t : m_Types)
        {
            t.CurrentAdded.swap(t.PendingAdded);
            t.PendingAdded.clear();
            t.CurrentRemoved.swap(t.PendingRemoved);
            t.PendingRemoved.clear();
        }
        ++m_SwapGeneration;
    }

    uint64 SwapGeneration() const { return m_SwapGeneration; }

    // World::Clear(): the WorldReset signal (Q4). Wipes BOTH windows — a
    // buffer surviving into the next scene would alias recycled entity
    // indices — and bumps the generation consumers key their full-reset on.
    void SignalReset()
    {
        for (TypeBuffers& t : m_Types)
        {
            t.PendingAdded.clear();
            t.CurrentAdded.clear();
            t.PendingRemoved.clear();
            t.CurrentRemoved.clear();
        }
        for (DisabledEvents& d : m_DisabledEvents)
            d.Disabled.clear();
        ++m_ResetGeneration;
    }

    uint64 ResetGeneration() const { return m_ResetGeneration; }

    // Enable-state transitions (World::GetDisabled<T>): per component type, the
    // entities whose T stopped taking part in queries during the window just
    // promoted. The World fills Disabled inside its swap, from the current
    // windows of the tags and the entities' state at that point, so readers
    // only ever read.
    struct DisabledEvents
    {
        ComponentTypeId Component = 0;
        ComponentTypeId DisabledTag = 0;
        std::vector<EntityHandle> Disabled;
    };

    // Bootstrap-time, like Register (idempotent).
    void RegisterDisabledEvents(ComponentTypeId component, ComponentTypeId disabledTag)
    {
        for (const DisabledEvents& d : m_DisabledEvents)
            if (d.Component == component)
                return;
        m_DisabledEvents.push_back(DisabledEvents{component, disabledTag, {}});
    }

    std::span<DisabledEvents> DisabledEventTypes() { return m_DisabledEvents; }

    std::span<const EntityHandle> CurrentDisabled(ComponentTypeId component) const
    {
        for (const DisabledEvents& d : m_DisabledEvents)
            if (d.Component == component)
                return d.Disabled;
        return {};
    }

private:
    struct TypeBuffers
    {
        ComponentTypeId Type = 0;
        std::vector<EntityHandle> PendingAdded;   // recording window
        std::vector<EntityHandle> CurrentAdded;   // consumer-visible window
        std::vector<EntityHandle> PendingRemoved;
        std::vector<EntityHandle> CurrentRemoved;
    };

    // Linear scan: subscriptions are a handful of types (one in P3's
    // shipped configuration); a scan over an inline vector beats hashing.
    TypeBuffers* Find(ComponentTypeId typeId)
    {
        for (TypeBuffers& t : m_Types)
            if (t.Type == typeId)
                return &t;
        return nullptr;
    }

    const TypeBuffers* Find(ComponentTypeId typeId) const
    {
        return const_cast<LifecycleEventBuffers*>(this)->Find(typeId);
    }

    std::vector<TypeBuffers> m_Types;
    std::vector<DisabledEvents> m_DisabledEvents;
    uint64 m_ResetGeneration = 0;
    uint64 m_SwapGeneration = 0;
};

} // namespace GameEngine::ECS
