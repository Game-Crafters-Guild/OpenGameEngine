#include "ECS/World.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ComponentMigration.h"
#include "ECS/Components.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/UnresolvedComponentStore.h"
#include "Logger/Logger.h"
#include "Types/GeometricReserve.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace GameEngine
{
namespace ECS
{

namespace
{
std::atomic<uint64> g_nextWorldId{1u};

void ReportRemovalFailure(EntityHandle entity, ComponentTypeId typeId, const char* reason) noexcept
{
    // Diagnostics must not become a second teardown failure (including OOM).
    try
    {
        Logger::Log::Error("[ECS] OnRemove notification threw for entity {}, component {}: {}. "
                           "The entity is still destroyed; cleanup owned by the callback may be "
                           "incomplete, so it must release or transfer its resources without throwing",
                           entity.id, typeId, reason);
    }
    catch (...)
    {
    }
}

// Which World is delivering entity-removal notifications on this thread, for
// which entity (every row while Clear/destruction runs). Trivially destructible
// so ~World may read it during process shutdown. A nested scope (a hook
// destroying an entity of another World) saves and restores the outer record.
struct RemovalRecord
{
    const World* Owner = nullptr;
    EntityHandle Entity{};
    bool All = false;
};
thread_local RemovalRecord t_Removal;

struct RemovalScope
{
    RemovalRecord Saved;
    RemovalScope(const World& owner, EntityHandle entity, bool all) : Saved(t_Removal)
    {
        t_Removal = RemovalRecord{&owner, entity, all};
    }
    ~RemovalScope() { t_Removal = Saved; }
    RemovalScope(const RemovalScope&) = delete;
    RemovalScope& operator=(const RemovalScope&) = delete;
};
} // namespace

// Phase 1b: ComponentTypeId is a consteval Hash64 of the normalized type name
// (see ECS/ComponentTypeName.h). The runtime allocator that used to live here —
// ComponentTypeIdState with its type_index map, name map, atomic next-id, mutex,
// and cross-module redirect pointer — is deleted. Runtime-keyed blob components
// derive their ID via Hash64(name) in ComponentRegistry directly.

thread_local World::ThreadLocalBuffers World::tlBuffers;

// ============================================================================
// Archetype implementation — colocated ArchetypeTable as sole storage
// ============================================================================

Archetype::Archetype(const ComponentSignature& sig, World* w)
    : signature(sig), world(w)
{
    // Check whether any registered OnRemove hooks overlap with this archetype's components.
    if (world)
    {
        m_HasRemoveHooks = signature.Intersects(world->GetRemoveHookSignature());
    }
}

void Archetype::RefreshRemoveHooks()
{
    if (world)
    {
        m_HasRemoveHooks = signature.Intersects(world->GetRemoveHookSignature());
    }
}

void Archetype::Reserve(size_t count)
{
    std::unique_lock lock(mutex);
    if (m_Table)
    {
        m_Table->Reserve(static_cast<uint32_t>(count));
    }
}

EntityLocation Archetype::AddEntityUnsafe(EntityHandle entity)
{
    // No lock — caller must hold worldMutex.
    if (!m_Table) [[unlikely]]
    {
        Logger::Log::Error("[ECS] Archetype::AddEntityUnsafe called without table");
        return {};
    }
    EntityLocation loc = m_Table->AddEntity(entity);
    // Change signaling: every row enters a chunk through this Append, so the
    // structural stamp here is what makes new/moved rows visible to gated
    // consumers — the whole creation/clone family (CreateHandle, CreateBatch*,
    // CloneEntity, CreateFromSignature/Bundle, MoveEntity destination, the
    // add-or-set structural branch) funnels through it (design C7: stamps
    // start at zero = "never changed"; correctness depends on this stamp,
    // never on zero-init).
    if (world)
        m_Table->StampAllColumnVersions(loc.ChunkIndex, world->NextGlobalSystemVersion());
    return loc;
}

EntityHandle Archetype::RemoveEntityUnsafe(EntityLocation loc)
{
    // No lock — caller must hold worldMutex.
    if (!m_Table) [[unlikely]]
        return EntityHandle::Invalid();

    EntityHandle moved = m_Table->RemoveEntity(loc);
    // Change signaling: swap-and-pop rewrote the vacated slot with the
    // chunk's last row — every column of this chunk changed for gated
    // consumers (covers all destroy paths, remove-component moves, and the
    // MoveEntity source side).
    if (world)
        m_Table->StampAllColumnVersions(loc.ChunkIndex, world->NextGlobalSystemVersion());

    // The last entity in the chunk was swapped into loc's position.
    if (moved.IsValid() && world)
    {
        world->UpdateEntityLocationUnsafe(moved, this, loc);
    }

    return moved;
}

void Archetype::InitializeTable(const std::unordered_map<ComponentTypeId, std::size_t>& /*componentSizes*/)
{
    // Build the colocated ArchetypeTable from the registered component sizes. Uses the shared
    // BuildSortedComponentMetas so the column sizing/alignment rule has a single definition (the
    // hot-reload migration path uses the same helper — they must never disagree on stride).
    if (!world)
        return;
    const std::vector<ComponentMeta> metas =
        BuildSortedComponentMetas(signature.GetComponents(), world->GetComponentSizes());
    if (!metas.empty())
        m_Table.emplace(std::span<const ComponentMeta>(metas));
}

// ============================================================================
// World implementation
// ============================================================================

namespace
{
// Live-world registry (C12 reload migration): every constructed World joins so
// MigrateComponentLayoutAcrossWorlds can reach ALL worlds holding a migrating
// component, not only the caller's. Function-local statics — worlds can be
// constructed before main in tests. Lock order: this mutex is taken BEFORE any
// worldMutex (both the sweep and ~World follow it).
//
// Each linked copy of the ECS code owns its own registry. Every product binary
// (Editor, Player, GameEngine.Native, native user modules) links the Engine
// import library and reaches the one copy inside Engine.dll; only test
// executables that link the ECS object library directly own a registry of
// their own. The sweep therefore always migrates the explicitly-passed target
// and the registry only ADDS coverage.
std::mutex& LiveWorldsMutex()
{
    static std::mutex m;
    return m;
}

std::vector<World*>& LiveWorlds()
{
    static std::vector<World*> worlds;
    return worlds;
}
} // namespace

// Weak lifetime marker for external subscriptions. Keeping this marker alive
// does not keep the World alive; UnregisterLiveComponentHook uses the registry
// lifetime lock before touching a World. Constructed/deleted in this ECS image.
struct ComponentHookSubscriptionState {};

World::World(JobSystem::WorkStealingThreadPool* js)
    : World(WorldConfig{}, js)
{
}

World::World(const WorldConfig& cfg, JobSystem::WorkStealingThreadPool* js)
    : m_WorldId(g_nextWorldId.fetch_add(1u, std::memory_order_relaxed)), jobSystem(js), config(cfg)
{
    entityMetadata.reserve(config.ExpectedEntityCount);

    // Command buffers are created lazily by GetCommandBuffer() on each
    // thread's first deferred op. Eagerly allocating one per worker thread
    // cost hardware_concurrency x ~3MB (96MB/World on a 32-thread machine)
    // for buffers most threads never touched.

    if (config.EnableDebugLogging)
    {
        Logger::Log::Info("[ECS] World initialized with config - entities: {}, command buffer size: {} (lazy per-thread)",
                          config.ExpectedEntityCount, config.CommandBufferSize);
    }
    // Publish only a fully constructed World. Allocation/logging failure before
    // this point must not leave a dangling entry for subscriptions or migration.
    std::lock_guard<std::mutex> lock(LiveWorldsMutex());
    LiveWorlds().push_back(this);
}

World::~World()
{
    {
        std::lock_guard<std::mutex> lock(LiveWorldsMutex());
        auto& worlds = LiveWorlds();
        worlds.erase(std::remove(worlds.begin(), worlds.end(), this), worlds.end());
    }
    // The weak subscription marker may remain lockable during member teardown.
    // Unsubscribe must resolve the World through the registry, where it is gone.
    // Do NOT call World::Clear() here — Clear() touches the thread_local
    // `tlBuffers.PerWorldBuffer` map which may already be destroyed during
    // process shutdown (TLS destructors can run before static singletons).
    // Stale entries in other threads' tlBuffers are safe: the key is this
    // World's monotonic WorldId, which is never reused, so no future lookup
    // can ever match them.
    //
    // We still want to fire OnRemove hooks so user code can release external
    // resources (asset refs, GPU handles) deterministically. InvokeAllRemoveHooks
    // only reads archetype storage; it does not touch any TLS.
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    InvokeAllRemoveHooks();
    archetypes.clear();
    entityMetadata.clear();
    freeIndices.clear();
    componentSizes.clear();
    commandBuffers.clear();
    entityCount.store(0, std::memory_order_relaxed);
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    ++m_WorldGeneration;
    bulkOperationMode = false;
}

UnresolvedComponentStore& World::GetUnresolvedComponents()
{
    if (!m_UnresolvedComponents)
        m_UnresolvedComponents = std::make_unique<UnresolvedComponentStore>();
    return *m_UnresolvedComponents;
}

void World::RefreshArchetypeRemoveHooks()
{
    for (auto& [sig, archetype] : archetypes)
    {
        archetype->RefreshRemoveHooks();
    }
}

World::ComponentHookStorage World::GetComponentHookStorage(ComponentHookKind kind)
{
    switch (kind)
    {
    case ComponentHookKind::Add: return {m_OnAddHooks, m_AddHookSignature};
    case ComponentHookKind::Set: return {m_OnSetHooks, m_SetHookSignature};
    case ComponentHookKind::Remove: return {m_OnRemoveHooks, m_RemoveHookSignature};
    }
    throw std::invalid_argument("Unknown component hook kind");
}

ComponentHookToken World::InstallComponentHook(ComponentHookKind kind, ComponentTypeId typeId, ComponentHook hook)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    if (m_NextComponentHookId == 0)
        throw std::overflow_error("World component hook registration IDs exhausted");
    hook.RegistrationId = m_NextComponentHookId++;
    auto storage = GetComponentHookStorage(kind);
    const bool hadSignature = storage.Signature.Contains(typeId);
    // Both containers can allocate. Publish the hook only after its signature
    // exists, and roll back that signature if map insertion fails.
    storage.Signature.Add(typeId);
    try
    {
        storage.Hooks.insert_or_assign(typeId, hook);
    }
    catch (...)
    {
        if (!hadSignature)
            storage.Signature.Remove(typeId);
        throw;
    }
    if (kind == ComponentHookKind::Remove)
        RefreshArchetypeRemoveHooks();
    return {m_WorldId, typeId, kind, hook.RegistrationId};
}

bool World::UnregisterComponentHook(ComponentHookToken token)
{
    if (!token || token.m_WorldId != m_WorldId)
        return false;
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    auto storage = GetComponentHookStorage(token.m_Kind);
    auto it = storage.Hooks.find(token.m_Component);
    if (it == storage.Hooks.end() || it->second.RegistrationId != token.m_RegistrationId)
        return false;
    storage.Hooks.erase(it);
    storage.Signature.Remove(token.m_Component);
    if (token.m_Kind == ComponentHookKind::Remove)
        RefreshArchetypeRemoveHooks();
    return true;
}

ScopedSubscription World::CreateComponentHookSubscription(ComponentHookToken token)
{
    if (!token)
        return {};
    try
    {
        if (!m_HookSubscriptionState)
            m_HookSubscriptionState = std::make_shared<ComponentHookSubscriptionState>();
        // Both the callable and its std::function manager are compiled here,
        // never in the subscribing native DLL. No raw World pointer is captured.
        return {m_HookSubscriptionState, [token] { UnregisterLiveComponentHook(token); }};
    }
    catch (...)
    {
        // A failed scope allocation must not leave an unreachable registration.
        UnregisterComponentHook(token);
        throw;
    }
}

void World::UnregisterLiveComponentHook(ComponentHookToken token)
{
    // Match the reload migrator's lifetime protocol: registry before worldMutex.
    // ~World removes itself under this lock before invoking hooks or destroying
    // storage. A locked weak marker alone would not protect the World itself.
    std::lock_guard<std::mutex> registryLock(LiveWorldsMutex());
    for (World* world : LiveWorlds())
    {
        if (world->GetWorldId() == token.m_WorldId)
        {
            world->UnregisterComponentHook(token);
            return;
        }
    }
}

void World::CheckRemovalReentry() const
{
    if (IsRemovingOnThisThread())
        throw std::logic_error("Same-world structural mutation is forbidden during entity removal notifications");
}

bool World::IsRemovingOnThisThread() const
{
    return t_Removal.Owner == this;
}

bool World::IsDuplicateRemoval(EntityHandle entity) const
{
    return IsRemovingOnThisThread() && (t_Removal.All || t_Removal.Entity == entity);
}

void World::InvokeRemoveHooksForEntity(Archetype* archetype, EntityLocation loc, EntityHandle entity)
{
    // Caller must hold worldMutex. Invokes OnRemove hooks for every component
    // column in the archetype that has a registered hook.
    if (!archetype || !archetype->HasRemoveHooks() || !archetype->HasTable())
        return;

    RemovalScope scope(*this, entity, false);

    for (auto typeId : archetype->GetSignature().GetComponents())
    {
        auto hookIt = m_OnRemoveHooks.find(typeId);
        if (hookIt == m_OnRemoveHooks.end())
            continue;

        void* ptr = archetype->GetComponentRawAt(
            static_cast<uint16_t>(loc.ChunkIndex),
            static_cast<uint16_t>(loc.IndexInChunk),
            typeId);
        if (ptr)
        {
            try
            {
                hookIt->second.Invoke(entity, ptr, hookIt->second.UserData);
            }
            catch (const std::exception& e)
            {
                ReportRemovalFailure(entity, typeId, e.what());
            }
            catch (...)
            {
                ReportRemovalFailure(entity, typeId, "non-standard exception");
            }
        }
    }
}

void World::InvokeAllRemoveHooks()
{
    // Caller must hold worldMutex. Invokes OnRemove hooks for every entity
    // in every archetype. Used by Clear().
    if (m_OnRemoveHooks.empty())
        return;

    RemovalScope scope(*this, EntityHandle::Invalid(), true);

    for (auto& [sig, archetype] : archetypes)
    {
        if (!archetype->HasRemoveHooks() || archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        for (auto typeId : archetype->GetSignature().GetComponents())
        {
            auto hookIt = m_OnRemoveHooks.find(typeId);
            if (hookIt == m_OnRemoveHooks.end())
                continue;

            int colIdx = table.FindColumnIndex(typeId);
            if (colIdx < 0)
                continue;

            for (auto& chunk : table.GetChunks())
            {
                uint32_t count = chunk.GetCount();
                if (count == 0)
                    continue;

                auto* bytePtr = static_cast<uint8_t*>(chunk.GetColumnRaw(colIdx));
                const EntityHandle* handles = chunk.GetEntityHandles();
                for (uint32_t i = 0; i < count; ++i)
                {
                    try
                    {
                        hookIt->second.Invoke(handles[i], bytePtr + i * hookIt->second.ComponentSize,
                                              hookIt->second.UserData);
                    }
                    catch (const std::exception& e)
                    {
                        ReportRemovalFailure(handles[i], typeId, e.what());
                    }
                    catch (...)
                    {
                        ReportRemovalFailure(handles[i], typeId, "non-standard exception");
                    }
                }
            }
        }
    }
}

void World::UpdateEntityLocationUnsafe(EntityHandle entity, Archetype* archetype, EntityLocation loc)
{
    // Caller must already hold worldMutex.
    if (entity.index >= entityMetadata.size())
        return;
    auto& md = entityMetadata[entity.index];
    md.archetype = archetype;
    md.SetLocation(loc);
}

// ============================================================================
// Lifecycle-event recording helpers (§6.1 — signature-shaped family)
// ============================================================================

void World::RecordAddedForSignature(const ComponentSignature& sig, EntityHandle entity)
{
    for (ComponentTypeId typeId : m_LifecycleEventSignature.GetComponents())
    {
        if (sig.Contains(typeId))
            m_LifecycleEvents.AppendAdded(typeId, entity);
    }
}

void World::RecordAddedForSignatureBatch(const ComponentSignature& sig,
                                         const EntityHandle* entities, std::size_t count)
{
    if (count == 0)
        return;
    for (ComponentTypeId typeId : m_LifecycleEventSignature.GetComponents())
    {
        if (sig.Contains(typeId))
            m_LifecycleEvents.AppendAddedBatch(typeId, entities, count);
    }
}

void World::RecordRemovedForSignature(const ComponentSignature& sig, EntityHandle entity)
{
    for (ComponentTypeId typeId : m_LifecycleEventSignature.GetComponents())
    {
        if (sig.Contains(typeId))
            m_LifecycleEvents.AppendRemoved(typeId, entity);
    }
}

EntityHandle World::CreateEntity()
{
    EntityIndex index;
    EntityVersion version;

    {
        CheckRemovalReentry();
        std::unique_lock lock(worldMutex);

        if (!freeIndices.empty())
        {
            index = freeIndices.back();
            freeIndices.pop_back();
        }
        else
        {
            index = static_cast<EntityIndex>(entityMetadata.size());
            entityMetadata.emplace_back();
        }

        auto& metadata = entityMetadata[index];
        metadata.version++;
        metadata.alive = true;
        metadata.archetype = nullptr;
        metadata.SetLocation(0, 0);
        version = metadata.version;
    }

    entityCount.fetch_add(1, std::memory_order_relaxed);

    return EntityHandle(index, version);
}

EntityHandle World::CreateEntityUnsafe()
{
    // This method assumes worldMutex is already held by the caller
    EntityIndex index;

    if (!freeIndices.empty())
    {
        index = freeIndices.back();
        freeIndices.pop_back();
    }
    else
    {
        index = static_cast<EntityIndex>(entityMetadata.size());
        entityMetadata.emplace_back();
    }

    auto& metadata = entityMetadata[index];
    metadata.version++;
    metadata.alive = true;
    metadata.archetype = nullptr;
    metadata.SetLocation(0, 0);

    entityCount.fetch_add(1, std::memory_order_relaxed);

    return EntityHandle(index, entityMetadata[index].version);
}

// Deferred: queues the destroy to the thread-local command buffer; falls back
// to immediate execution when the buffer is full (unless structural-change
// deferral is active, in which case the destroy is dropped with an error).
void World::DestroyEntity(EntityHandle entity)
{
    if (IsDuplicateRemoval(entity))
        return;
    auto& buffer = GetCommandBuffer();
    Command cmd;
    cmd.type = Command::DESTROY_ENTITY;
    cmd.entity = entity;
    if (!buffer.TryPush(std::move(cmd)))
    {
        if (m_DeferStructuralChanges)
        {
            Logger::Log::Error("[ECS] Command buffer full while deferral is active; destroy of entity {} dropped", entity.id);
            return;
        }

        DestroyEntityImmediate(entity);
    }
}

void World::DestroyEntityInternal(EntityHandle entity, bool preserveHandle)
{
    // Caller must hold worldMutex exclusively. Single body behind all three
    // per-entity destroy paths (immediate, preserve-handle, deferred playback).
    if (!IsValid(entity))
    {
        return;
    }

    auto& metadata = entityMetadata[entity.index];
    // Prepare every fallible ECS append before calling user code. A failure
    // here leaves the live row and its external ownership untouched.
    if (!preserveHandle)
        ReserveForAppend(freeIndices, 1);
    if (metadata.archetype && !m_LifecycleEvents.Empty())
    {
        for (auto typeId : m_LifecycleEventSignature.GetComponents())
            if (metadata.archetype->GetSignature().Contains(typeId))
                m_LifecycleEvents.PrepareRemoved(typeId, 1);
    }
    DestroyPreparedEntityInternal(entity, preserveHandle);
}

void World::DestroyPreparedEntityInternal(EntityHandle entity, bool preserveHandle)
{
    // Caller holds worldMutex and has validated/prepared this retirement. Read
    // the current location: an earlier batch removal may have moved this row.
    auto& metadata = entityMetadata[entity.index];
    if (metadata.archetype)
    {
        // OnRemove hooks fire before the row is destroyed (live T&).
        InvokeRemoveHooksForEntity(metadata.archetype, metadata.GetLocation(), entity);

        // Lifecycle events (§6.1): one Removed per subscribed type present in
        // the dying archetype's signature. This single site covers batch and
        // per-entity destroy paths — immediate, preserve-handle (editor
        // undo/redo, C4), deferred playback. Clear()/~World stay on the bulk
        // teardown path and emit the WorldReset signal instead (Q4).
        RecordRemovedForSignature(metadata.archetype->GetSignature(), entity);

        metadata.archetype->RemoveEntityUnsafe(metadata.GetLocation());
    }
    metadata.alive = false;
    metadata.archetype = nullptr;
    if (preserveHandle)
    {
        // Keep the index out of freeIndices so undo/redo can revive the same
        // EntityHandle (index+version) via ReviveEntityImmediatePreserveHandle.
        metadata.SetLocation(0, 0);
    }
    else
    {
        freeIndices.push_back(entity.index);

        // The scene loader's preserved overrides describe components this entity no longer has, and
        // the handle is going back into circulation. Only on this path: the preserve-handle destroy
        // above is undo/redo parking the same handle for revival, and the overrides must be there
        // when it comes back.
        if (m_UnresolvedComponents)
        {
            m_UnresolvedComponents->EraseEntity(entity);
        }
    }
    entityCount.fetch_sub(1, std::memory_order_relaxed);
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
}

void World::DestroyEntityImmediate(EntityHandle entity)
{
    if (IsDuplicateRemoval(entity))
        return;
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    DestroyEntityInternal(entity, false);
}

void World::DestroyEntitiesImmediate(std::span<const EntityHandle> entities)
{
    if (entities.empty())
        return;
    if (IsRemovingOnThisThread() &&
        std::all_of(entities.begin(), entities.end(), [this](EntityHandle entity)
                    { return IsDuplicateRemoval(entity); }))
        return;
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    const auto count = static_cast<std::size_t>(std::count_if(
        entities.begin(), entities.end(), [this](EntityHandle entity)
        { return IsValid(entity); }));
    if (count == 0)
        return;
    // Sized construction: the default constructor is noexcept, and under the
    // debug CRT it allocates its iterator proxy, so a denied allocation there
    // would terminate instead of propagating.
    std::vector<EntityHandle> requested(count);
    std::copy_if(entities.begin(), entities.end(), requested.begin(),
                 [this](EntityHandle entity) { return IsValid(entity); });

    // This is the only fallible ECS preparation phase. Duplicates conservatively
    // contribute capacity, avoiding a hash set or sorting the caller's order.
    ReserveForAppend(freeIndices, count);
    for (const auto typeId : m_LifecycleEventSignature.GetComponents())
    {
        std::size_t removed = 0;
        for (const auto entity : requested)
        {
            const auto* archetype = entityMetadata[entity.index].archetype;
            if (archetype && archetype->GetSignature().Contains(typeId))
                ++removed;
        }
        m_LifecycleEvents.PrepareRemoved(typeId, removed);
    }

    // Hooks may mutate the caller's span storage, but cannot structurally edit
    // this World. Revalidation skips duplicates after their first retirement.
    for (const auto entity : requested)
        if (IsValid(entity))
            DestroyPreparedEntityInternal(entity, false);
}

void World::DestroyEntityImmediatePreserveHandle(EntityHandle entity)
{
    if (IsDuplicateRemoval(entity))
        return;
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    DestroyEntityInternal(entity, true);
}

bool World::ReviveEntityImmediatePreserveHandle(EntityHandle entity)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    if (entity.index >= entityMetadata.size())
    {
        return false;
    }
    auto& metadata = entityMetadata[entity.index];
    if (metadata.version != entity.version)
    {
        return false;
    }
    if (metadata.alive)
    {
        return true; // idempotent
    }

    metadata.alive = true;
    metadata.archetype = nullptr;
    metadata.SetLocation(0, 0);
    entityCount.fetch_add(1, std::memory_order_relaxed);
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool World::IsValid(EntityHandle entity) const
{
    if (entity.index >= entityMetadata.size())
    {
        return false;
    }

    const auto& metadata = entityMetadata[entity.index];
    return metadata.alive && metadata.version == entity.version;
}

Archetype* World::GetOrCreateArchetype(const ComponentSignature& signature)
{
    auto it = archetypes.find(signature);
    if (it != archetypes.end())
    {
        return it->second.get();
    }

    auto archetype = std::make_unique<Archetype>(signature, this);
    archetype->InitializeTable(componentSizes);

    auto* ptr = archetype.get();
    archetypes[signature] = std::move(archetype);
    return ptr;
}

Archetype* World::GetOrCreateArchetypeForAdd(Archetype* source, ComponentTypeId typeId)
{
    if (Archetype* cached = source->GetAddEdge(typeId))
        return cached;

    ComponentSignature newSig = source->GetSignature();
    newSig.Add(typeId);
    Archetype* target = GetOrCreateArchetype(newSig);

    // No-op (source already had typeId, e.g. Add when present): don't cache a
    // self-loop on RemoveEdge. AddComponentImpl's fast-path normally catches
    // this case before we get here; this guard handles direct callers that
    // bypass the fast-path. Without it, a self-loop would later make
    // Remove(typeId) on source incorrectly resolve to source itself.
    if (target != source)
    {
        source->SetAddEdge(typeId, target);
        target->SetRemoveEdge(typeId, source);
    }

    return target;
}

Archetype* World::GetOrCreateArchetypeForRemove(Archetype* source, ComponentTypeId typeId)
{
    if (Archetype* cached = source->GetRemoveEdge(typeId))
        return cached;

    ComponentSignature newSig = source->GetSignature();
    newSig.Remove(typeId);
    Archetype* target = GetOrCreateArchetype(newSig);

    // No-op (source didn't have typeId): don't cache a self-loop on AddEdge.
    // Without this guard, RemoveComponent<T> on an entity that lacks T poisons
    // source's AddEdge[T] = source, which then makes any subsequent
    // AddComponent<T> on a sibling entity (in the same archetype) silently
    // skip the archetype migration, losing the component data.
    if (target != source)
    {
        source->SetRemoveEdge(typeId, target);
        target->SetAddEdge(typeId, source);
    }

    return target;
}

Archetype* World::GetOrCreateArchetypeForActivity(Archetype* source, bool disable)
{
    if (Archetype* cached = disable ? source->GetDisableEdge() : source->GetEnableEdge())
        return cached;

    const ComponentTypeId disabledId = GetComponentTypeId<Disabled>();
    const ComponentSignature& sourceSignature = source->GetSignature();
    const bool hadInHierarchy = sourceSignature.Contains(GetComponentTypeId<DisabledInHierarchy>());

    // Only Disabled moves when the source already carries the derived tag (a
    // descendant of a disabled parent) or lacks it (an authored tag no pass has
    // seen yet): an ordinary single-type transition, resolved through the
    // per-type edges the archetype already caches in both directions.
    if (disable && hadInHierarchy)
        return GetOrCreateArchetypeForAdd(source, disabledId);
    if (!disable && !hadInHierarchy)
        return GetOrCreateArchetypeForRemove(source, disabledId);

    // Both tags move: the activity pair, cached in both directions.
    ComponentSignature targetSignature = sourceSignature;
    if (disable)
    {
        targetSignature.Add(GetComponentTypeId<Disabled>());
        targetSignature.Add(GetComponentTypeId<DisabledInHierarchy>());
    }
    else
    {
        targetSignature.Remove(GetComponentTypeId<Disabled>());
        targetSignature.Remove(GetComponentTypeId<DisabledInHierarchy>());
    }
    Archetype* target = GetOrCreateArchetype(targetSignature);
    if (target == source)
        return target;

    if (disable)
    {
        source->SetDisableEdge(target);
        target->SetEnableEdge(source);
    }
    else
    {
        source->SetEnableEdge(target);
        target->SetDisableEdge(source);
    }

    return target;
}

bool World::SetEntityEnabledImmediate(EntityHandle entity, bool enabled)
{
    CheckRemovalReentry();
    AutoComponentRegistrar<Disabled>::EnsureRegistered();
    AutoComponentRegistrar<DisabledInHierarchy>::EnsureRegistered();

    std::unique_lock lock(worldMutex);
    if (!IsValid(entity))
        return false;

    auto& metadata = entityMetadata[entity.index];
    Archetype* current = metadata.archetype;
    if (!current)
        return false;

    const ComponentTypeId disabledId = GetComponentTypeId<Disabled>();
    const ComponentTypeId inHierarchyId = GetComponentTypeId<DisabledInHierarchy>();
    const bool wasDisabled = current->GetSignature().Contains(disabledId);
    if (wasDisabled == !enabled)
        return false;

    componentSizes.try_emplace(disabledId, sizeof(Disabled));
    componentSizes.try_emplace(inHierarchyId, sizeof(DisabledInHierarchy));

    // The derived tag only moves when it is not already in the state the
    // transition wants: an entity under a disabled ancestor already carries it.
    // Switching on, it also stays while the parent is still off, so the entity
    // does not return to queries before the hierarchy pass would take it out.
    const bool hasInHierarchy = current->GetSignature().Contains(inHierarchyId);
    const bool keepsInHierarchy = enabled && hasInHierarchy && IsParentInactive(current, metadata);
    const bool inHierarchyMoves = hasInHierarchy == enabled && !keepsInHierarchy;

    // OnRemove hooks see live data, so they run before the move; OnAdd/OnSet
    // see the destination row, so they run after it.
    if (enabled)
    {
        InvokeActivityRemoveHooks(entity, current, metadata, disabledId);
        if (inHierarchyMoves)
            InvokeActivityRemoveHooks(entity, current, metadata, inHierarchyId);
    }

    Archetype* target = keepsInHierarchy ? GetOrCreateArchetypeForRemove(current, disabledId)
                                         : GetOrCreateArchetypeForActivity(current, !enabled);
    if (current != target)
        MoveEntity(entity, current, target);

    if (enabled)
    {
        RecordComponentRemoved(disabledId, entity);
        if (inHierarchyMoves)
            RecordComponentRemoved(inHierarchyId, entity);
    }
    else
    {
        InvokeActivityAddHooks(entity, target, metadata, disabledId);
        if (inHierarchyMoves)
            InvokeActivityAddHooks(entity, target, metadata, inHierarchyId);
        RecordComponentAdded(disabledId, entity);
        if (inHierarchyMoves)
            RecordComponentAdded(inHierarchyId, entity);
    }

    EmitComponentDirty(disabledId, entity);
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void World::SetParentRelation(ComponentTypeId parentType, uint32 handleOffset)
{
    const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(parentType);
    assert(info != nullptr && "SetParentRelation: the parent component type is not registered");
    assert((info == nullptr || handleOffset + sizeof(EntityHandle) <= info->Size) &&
           "SetParentRelation: the handle offset lies outside the parent component");
    if (!info || handleOffset + sizeof(EntityHandle) > info->Size)
    {
        Logger::Log::Error("[ECS] SetParentRelation refused component type {} at offset {}: register the type "
                           "first and name the offset of an EntityHandle inside it",
                           parentType, handleOffset);
        return;
    }
    std::unique_lock lock(worldMutex);
    m_ParentRelationType = parentType;
    m_ParentRelationOffset = handleOffset;
}

bool World::IsParentInactive(const Archetype* archetype, const EntityMetadata& metadata) const
{
    if (m_ParentRelationType == 0 || !archetype->GetSignature().Contains(m_ParentRelationType))
        return false;
    const void* link = archetype->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, m_ParentRelationType);
    if (!link)
        return false;
    EntityHandle parent;
    std::memcpy(&parent, static_cast<const uint8_t*>(link) + m_ParentRelationOffset, sizeof(parent));
    if (!IsValid(parent))
        return false;
    const Archetype* parentArchetype = entityMetadata[parent.index].archetype;
    if (!parentArchetype)
        return false;
    const ComponentSignature& signature = parentArchetype->GetSignature();
    return signature.Contains(GetComponentTypeId<Disabled>()) ||
           signature.Contains(GetComponentTypeId<DisabledInHierarchy>());
}

bool World::SetComponentEnabledImmediate(EntityHandle entity, ComponentTypeId componentType, bool enabled)
{
    const ComponentTypeId disabledTag = ComponentRegistry::RegisterComponentDisabledTag(componentType);
    if (disabledTag == 0 || !IsValid(entity))
        return false;
    // Idempotent, like Entity::SetEnabled<T>: a redundant call must not bump the
    // structural version and invalidate every query cache in the world.
    const bool disabled = HasComponent(entity, disabledTag);
    if (enabled != disabled)
        return false;
    if (enabled)
        return RemoveComponentByTypeIdImmediate(entity, disabledTag);
    const uint8 tagByte = 0;
    return SetComponentBytesImmediate(entity, disabledTag, &tagByte, sizeof(tagByte));
}

bool World::IsComponentEnabled(EntityHandle entity, ComponentTypeId componentType) const
{
    const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(componentType);
    assert(info != nullptr && "IsComponentEnabled: the component type is not registered");
    if (!info)
    {
        Logger::Log::Error("[ECS] IsComponentEnabled asked about unregistered component type {}", componentType);
        return false;
    }
    return !HasComponent(entity, ComponentDisabledTypeId(info->Name));
}

void World::BuildDisabledEvents()
{
    const ComponentTypeId disabledId = GetComponentTypeId<Disabled>();
    const ComponentTypeId inHierarchyId = GetComponentTypeId<DisabledInHierarchy>();
    for (LifecycleEventBuffers::DisabledEvents& events : m_LifecycleEvents.DisabledEventTypes())
    {
        events.Disabled.clear();
        for (ComponentTypeId tag : {events.DisabledTag, disabledId, inHierarchyId})
        {
            for (EntityHandle entity : m_LifecycleEvents.CurrentAdded(tag))
            {
                if (!IsValid(entity) || !entityMetadata[entity.index].archetype)
                    continue;
                const ComponentSignature& signature = entityMetadata[entity.index].archetype->GetSignature();
                const bool off = signature.Contains(events.DisabledTag) || signature.Contains(disabledId) ||
                                 signature.Contains(inHierarchyId);
                if (off && signature.Contains(events.Component))
                    events.Disabled.push_back(entity);
            }
        }
        std::sort(events.Disabled.begin(), events.Disabled.end());
        events.Disabled.erase(std::unique(events.Disabled.begin(), events.Disabled.end()), events.Disabled.end());
    }
}

void World::InvokeActivityAddHooks(EntityHandle entity, Archetype* archetype,
                                   const EntityMetadata& metadata, ComponentTypeId typeId)
{
    if (!m_AddHookSignature.Contains(typeId) && !m_SetHookSignature.Contains(typeId))
        return;
    void* ptr = archetype->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
    if (!ptr)
        return;
    if (m_AddHookSignature.Contains(typeId))
        InvokeAddHook(typeId, entity, ptr);
    if (m_SetHookSignature.Contains(typeId))
        InvokeSetHook(typeId, entity, ptr);
}

void World::InvokeActivityRemoveHooks(EntityHandle entity, Archetype* archetype,
                                      const EntityMetadata& metadata, ComponentTypeId typeId)
{
    auto hookIt = m_OnRemoveHooks.find(typeId);
    if (hookIt == m_OnRemoveHooks.end())
        return;
    void* ptr = archetype->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
    if (ptr)
        hookIt->second.Invoke(entity, ptr, hookIt->second.UserData);
}

const std::vector<Archetype::CopyStep>& Archetype::GetOrBakeMovePlan(Archetype* target)
{
    auto it = m_MovePlans.find(target);
    if (it != m_MovePlans.end())
        return it->second;

    std::vector<CopyStep> plan;
    if (HasTable() && target->HasTable())
    {
        const auto& srcCols = m_Table->GetLayout().Columns;
        const auto& dstCols = target->GetTable().GetLayout().Columns;
        plan.reserve(std::min(srcCols.size(), dstCols.size()));

        // Merge-join on sorted TypeId — identical logic to ArchetypeChunk::CopyRow.
        std::size_t si = 0, di = 0;
        while (si < srcCols.size() && di < dstCols.size())
        {
            if (srcCols[si].TypeId < dstCols[di].TypeId)       ++si;
            else if (srcCols[si].TypeId > dstCols[di].TypeId)  ++di;
            else {
                plan.push_back({srcCols[si].Offset, dstCols[di].Offset,
                                std::min(srcCols[si].Size, dstCols[di].Size)});
                ++si; ++di;
            }
        }
    }
    return m_MovePlans.emplace(target, std::move(plan)).first->second;
}

void World::MoveEntity(EntityHandle entity, Archetype* from, Archetype* to)
{
    if (!from || !to || from == to)
        return;

    auto& metadata = entityMetadata[entity.index];
    EntityLocation fromLoc = metadata.GetLocation();

    // Caller always holds worldMutex exclusively, so skip the per-archetype
    // lock on both add and remove. Previously MoveEntity acquired
    // from->mutex + to->mutex each transition (~50ns wasted under redundant
    // locking) — measured 15-20% of the archetype-transition cost on
    // Remove+Add micro-benchmarks.
    EntityLocation newLoc = to->AddEntityUnsafe(entity);

    // Copy matching component data using the pre-baked plan on the edge.
    // First hit through an edge bakes the plan (merge-join), subsequent hits
    // skip that walk entirely.
    if (from->HasTable() && to->HasTable())
    {
        auto fromChunks = from->GetTable().GetChunks();
        auto toChunks = to->GetTable().GetChunks();
        if (fromLoc.ChunkIndex < fromChunks.size() && newLoc.ChunkIndex < toChunks.size())
        {
            const auto& plan = from->GetOrBakeMovePlan(to);
            auto& srcChunk = fromChunks[fromLoc.ChunkIndex];
            auto& dstChunk = toChunks[newLoc.ChunkIndex];
            std::byte* srcBase = srcChunk.GetMemory();
            std::byte* dstBase = dstChunk.GetMemory();
            const uint32_t srcSlot = fromLoc.IndexInChunk;
            const uint32_t dstSlot = newLoc.IndexInChunk;
            for (const auto& step : plan)
            {
                std::memcpy(
                    dstBase + step.DstOffset + static_cast<std::size_t>(dstSlot) * step.Size,
                    srcBase + step.SrcOffset + static_cast<std::size_t>(srcSlot) * step.Size,
                    step.Size);
            }
        }
    }

    // Remove from old archetype (swap-and-pop within chunk).
    from->RemoveEntityUnsafe(fromLoc);

    // Update metadata to new location.
    metadata.archetype = to;
    metadata.SetLocation(newLoc);
}

CommandBuffer& World::GetCommandBuffer()
{
    CheckRemovalReentry();
    // The thread-local hit needs no lock and no validation: an entry is only
    // written after its buffer is registered in commandBuffers (below), the
    // uint64 WorldId key is monotonic and never reused, and buffers live for
    // the World's entire lifetime — Clear() drains them but never destroys
    // them; only ~World frees, after which this World is unreachable. The
    // previous pointer-validation scan (shared_lock + linear compare over all
    // buffers) ran on every deferred op and could never fail for a live World.
    auto it = tlBuffers.PerWorldBuffer.find(m_WorldId);
    if (it != tlBuffers.PerWorldBuffer.end())
    {
        return *it->second;
    }

    std::unique_lock lock(worldMutex);
    commandBuffers.push_back(std::make_unique<CommandBuffer>(config.CommandBufferSize));
    auto* buf = commandBuffers.back().get();
    tlBuffers.PerWorldBuffer[m_WorldId] = buf;
    return *buf;
}

void World::ProcessCommands()
{
    CheckRemovalReentry();
    size_t commandsProcessed = 0;

    // Snapshot buffer pointers under a shared lock so a concurrent
    // GetCommandBuffer() allocation branch can't reallocate the vector and
    // invalidate our iterator. CommandBuffer lifetimes are safe because
    // destruction of the owning unique_ptrs only happens in Clear()/~World(),
    // which are specified to run only when no other thread is active.
    std::vector<CommandBuffer*> bufferSnapshot;
    {
        std::shared_lock readLock(worldMutex);
        bufferSnapshot.reserve(commandBuffers.size());
        for (const auto& up : commandBuffers) bufferSnapshot.push_back(up.get());
    }

    for (auto* buffer : bufferSnapshot)
    {
        Command cmd;
        // The cap check must precede TryPop: popping first dequeues a command
        // that the failed cap check would then silently discard (one lost per
        // non-empty buffer once the cap is reached).
        while (commandsProcessed < kMaxCommandsPerBatch && buffer->TryPop(cmd))
        {
            commandsProcessed++;
            switch (cmd.type)
            {
            case Command::CREATE_ENTITY:
                CreateEntity();
                break;

            case Command::DESTROY_ENTITY:
            {
                std::unique_lock lock(worldMutex);
                DestroyEntityInternal(cmd.entity, false);
                break;
            }

            case Command::ADD_REQUIRED_COMPONENT:
                if (HasComponent(cmd.entity, cmd.componentType))
                    break;
                [[fallthrough]];
            case Command::ADD_COMPONENT:
            {
                auto* handler = ComponentRegistry::GetHandler(cmd.componentType);
                if (handler)
                {
                    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(cmd.componentData.Data()), cmd.componentData.Size());
                    handler->AddComponentFromData(this, cmd.entity, data);
                }
                else
                {
                    Logger::Log::Warning("[ECS] No handler found for component type {} in ADD_COMPONENT command", cmd.componentType);
                }
                break;
            }

            case Command::REMOVE_COMPONENT:
            {
                auto* handler = ComponentRegistry::GetHandler(cmd.componentType);
                if (handler)
                {
                    handler->RemoveComponent(this, cmd.entity);
                }
                else
                {
                    Logger::Log::Warning("[ECS] No handler found for component type {} in REMOVE_COMPONENT command", cmd.componentType);
                }
                break;
            }

            case Command::SET_COMPONENT:
            {
                auto* handler = ComponentRegistry::GetHandler(cmd.componentType);
                if (handler)
                {
                    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(cmd.componentData.Data()), cmd.componentData.Size());
                    handler->AddComponentFromData(this, cmd.entity, data);
                }
                else
                {
                    Logger::Log::Warning("[ECS] No handler found for component type {} in SET_COMPONENT command", cmd.componentType);
                }
                break;
            }

            case Command::SET_ENTITY_ENABLED:
                SetEntityEnabledImmediate(cmd.entity, cmd.entityEnabled);
                break;
            }
        }
    }

    if (commandsProcessed >= kMaxCommandsPerBatch)
    {
        Logger::Log::Warning("[ECS] Command processing limit reached ({} commands). Potential infinite loop detected.", commandsProcessed);
    }
}

bool World::HasComponent(EntityHandle entity, ComponentTypeId typeId) const
{
    std::shared_lock lock(worldMutex);

    if (!IsValid(entity))
        return false;

    const Archetype* archetype = entityMetadata[entity.index].archetype;
    return archetype && archetype->GetSignature().Contains(typeId);
}

bool World::CaptureComponentBytes(EntityHandle entity,
                                  ComponentTypeId typeId,
                                  std::vector<uint8_t>& outBytes) const
{
    outBytes.clear();

    std::shared_lock lock(worldMutex);

    if (!IsValid(entity))
        return false;

    const auto& metadata = entityMetadata[entity.index];
    Archetype* archetype = metadata.archetype;
    if (!archetype)
        return false;

    if (!archetype->GetSignature().Contains(typeId))
        return false;

    auto* handler = ComponentRegistry::GetHandler(typeId);
    if (!handler)
        return false;

    outBytes = handler->SerializeComponent(archetype, metadata.chunkIndex, metadata.indexInChunk);

    if (outBytes.size() != handler->GetComponentSize())
    {
        outBytes.clear();
        return false;
    }

    return true;
}

bool World::ReadComponentBytes(EntityHandle entity,
                               ComponentTypeId typeId,
                               void* dest,
                               std::size_t destCap,
                               std::size_t& outSize) const
{
    outSize = 0;

    std::shared_lock lock(worldMutex);

    if (!IsValid(entity))
        return false;

    const auto& metadata = entityMetadata[entity.index];
    const Archetype* archetype = metadata.archetype;
    if (!archetype)
        return false;

    if (!archetype->GetSignature().Contains(typeId))
        return false;

    auto* handler = ComponentRegistry::GetHandler(typeId);
    if (!handler)
        return false;

    outSize = handler->GetComponentSize();
    if (!dest || destCap == 0)
        return true; // size query only

    const void* src = archetype->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
    if (!src)
    {
        outSize = 0;
        return false;
    }

    std::memcpy(dest, src, std::min(destCap, outSize));
    return true;
}

bool World::ApplyComponentBytesImmediate(EntityHandle entity,
                                         ComponentTypeId typeId,
                                         const std::vector<uint8_t>& bytes)
{
    CheckRemovalReentry();
    // IsValid reads entityMetadata.size() and indexes into the vector — racey
    // against a concurrent CreateEntity that emplace_back's. Guard the read
    // with a shared_lock, then release it before dispatching to the handler
    // (the handler takes its own unique_lock internally, so holding shared
    // here would deadlock). The entity could still go invalid between the
    // two locks; the handler validates again under its own unique_lock and
    // no-ops on a destroyed entity.
    {
        std::shared_lock readLock(worldMutex);
        if (!IsValid(entity))
            return false;
    }

    auto* handler = ComponentRegistry::GetHandler(typeId);
    if (!handler)
        return false;

    const std::size_t expectedSize = handler->GetComponentSize();
    if (bytes.size() != expectedSize)
        return false;

    handler->AddComponentFromData(this, entity, bytes);
    return true;
}

void World::AddComponentImpl(EntityHandle entity, ComponentTypeId typeId,
                             const void* data, std::size_t sizeBytes)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    AddOrSetComponentBytesInternal(entity, typeId, data, sizeBytes);
}

void World::RemoveComponentImpl(EntityHandle entity, ComponentTypeId typeId)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    if (RemoveComponentInternal(entity, typeId) == RemoveComponentResult::ComponentAbsent)
    {
        // C++ removal invalidates structural caches even when there is no column
        // to remove. Registered handler replay and typed calls use this same rule.
        structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    }
}

bool World::AddOrSetComponentBytesInternal(EntityHandle entity,
                                           ComponentTypeId typeId,
                                           const void* data,
                                           std::size_t sizeBytes)
{
    // Caller must hold worldMutex exclusively. Single body behind typed
    // AddComponentImpl<T>, type-erased SetComponentBytesImmediate, and (via
    // the component handlers) deferred ADD_COMPONENT/ADD_REQUIRED_COMPONENT/SET_COMPONENT
    // playback.
    if (!IsValid(entity))
        return false;

    auto& metadata = entityMetadata[entity.index];
    Archetype* current = metadata.archetype;

    // Data-only update fast path (no structural change) — SET semantics.
    if (current && current->GetSignature().Contains(typeId))
    {
        current->SetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId, data, sizeBytes);

        // Change signaling: the P-1-unified body makes this the singular
        // data-only-set stamp site (typed immediate, ABI byte-sets, deferred
        // SET/ADD playback, editor undo restores all funnel here). The
        // structural branch below needs no stamp — its write lands in a chunk
        // AddEntityUnsafe/MoveEntity just stamped in full.
        current->StampColumnVersion(metadata.chunkIndex, typeId, NextGlobalSystemVersion());

        // Dirty-feed emission for the entire Set-shaped writer class (P2
        // amendment A1 — the producer lives HERE, not on an OnSet hook:
        // hooks carry no world identity and are single-slot last-wins).
        // Covers undo byte-restores too: feed consumers key on Version !=,
        // so a backward restore is a legitimate entry.
        EmitComponentDirty(typeId, entity);

        // OnSet fires on value update
        if (m_SetHookSignature.Contains(typeId))
        {
            void* ptr = current->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
            if (ptr)
                InvokeSetHook(typeId, entity, ptr);
        }
        return true;
    }

    // Ensure component size registered for archetype init.
    componentSizes.try_emplace(typeId, sizeBytes);

    // Structural change: add component to entity — ADD semantics.
    Archetype* target;
    if (current)
    {
        target = GetOrCreateArchetypeForAdd(current, typeId);
    }
    else
    {
        ComponentSignature newSignature;
        newSignature.Add(typeId);
        target = GetOrCreateArchetype(newSignature);
    }

    if (current != target)
    {
        if (current == nullptr)
        {
            EntityLocation newLoc = target->AddEntityUnsafe(entity);
            metadata.archetype = target;
            metadata.SetLocation(newLoc);
        }
        else
        {
            MoveEntity(entity, current, target);
        }
    }

    target->SetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId, data, sizeBytes);

    // Invoke OnAdd and OnSet hooks
    if (m_AddHookSignature.Contains(typeId) || m_SetHookSignature.Contains(typeId))
    {
        void* ptr = target->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
        if (ptr)
        {
            if (m_AddHookSignature.Contains(typeId))
                InvokeAddHook(typeId, entity, ptr);
            if (m_SetHookSignature.Contains(typeId))
                InvokeSetHook(typeId, entity, ptr);
        }
    }

    // Structural adds also emit (A1: both branches). The TLAS reconcile
    // path would catch them via structuralChangeVersion anyway; emitting
    // keeps the feed a complete record of Set-shaped writes and consumers
    // dedupe by their Version gate.
    EmitComponentDirty(typeId, entity);

    // Lifecycle events (§6.1): the P-1-unified body makes this the singular
    // Added site for the whole add-shaped writer class — typed immediate,
    // type-erased/ABI byte-sets, blob components, deferred ADD/SET playback,
    // deserialize-per-component, and undo revive+byte-restore (C4). The
    // data-only fast path above records nothing (SetIsNotAdd).
    RecordComponentAdded(typeId, entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool World::SetComponentBytesImmediate(EntityHandle entity,
                                       ComponentTypeId typeId,
                                       const void* data,
                                       std::size_t sizeBytes)
{
    if (!data && sizeBytes != 0)
        return false;

    auto* handler = ComponentRegistry::GetHandler(typeId);
    if (!handler)
        return false;

    const std::size_t expected = handler->GetComponentSize();
    if (expected != sizeBytes)
        return false;

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    return AddOrSetComponentBytesInternal(entity, typeId, data, sizeBytes);
}

World::RemoveComponentResult World::RemoveComponentInternal(EntityHandle entity, ComponentTypeId typeId)
{
    // Caller must hold worldMutex exclusively. Single body behind typed
    // RemoveComponentImpl<T> and type-erased RemoveComponentByTypeIdImmediate.
    if (!IsValid(entity))
        return RemoveComponentResult::InvalidEntity;

    auto& metadata = entityMetadata[entity.index];
    Archetype* current = metadata.archetype;
    if (!current)
        return RemoveComponentResult::InvalidEntity;

    if (!current->GetSignature().Contains(typeId))
        return RemoveComponentResult::ComponentAbsent;

    // Invoke OnRemove hook for the specific component being removed —
    // before the data is destroyed (live T&).
    auto hookIt = m_OnRemoveHooks.find(typeId);
    if (hookIt != m_OnRemoveHooks.end())
    {
        void* ptr = current->GetComponentRawAt(metadata.chunkIndex, metadata.indexInChunk, typeId);
        if (ptr)
        {
            hookIt->second.Invoke(entity, ptr, hookIt->second.UserData);
        }
    }

    // The scene loader's preserved field overrides describe THIS component instance, which is about
    // to cease to exist — so they go with it. Otherwise a later re-add of the same type inherits the
    // previous load's authored text over its own defaults.
    if (m_UnresolvedComponents)
    {
        m_UnresolvedComponents->EraseFieldsOfComponent(entity, typeId);
    }

    Archetype* target = GetOrCreateArchetypeForRemove(current, typeId);
    if (current != target)
    {
        MoveEntity(entity, current, target);
    }

    // Lifecycle events (§6.1): the P-1-unified body makes this the singular
    // component-Removed site — typed, type-erased/ABI, blob, and deferred
    // REMOVE_COMPONENT playback. Absent-component calls returned above and
    // record nothing.
    RecordComponentRemoved(typeId, entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return RemoveComponentResult::Removed;
}

bool World::RemoveComponentByTypeIdImmediate(EntityHandle entity, ComponentTypeId typeId)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    return RemoveComponentInternal(entity, typeId) == RemoveComponentResult::Removed;
}

void World::MigrateArchetypeInstances(Archetype& arch, ComponentTypeId id,
                                      const ComponentLayoutChange& change,
                                      const std::vector<FieldByteMove>& remap)
{
    // New column metas at the (already-updated) componentSizes — same shared helper as
    // Archetype::InitializeTable, so the migrated column stride matches steady-state exactly.
    const std::vector<ComponentMeta> metas =
        BuildSortedComponentMetas(arch.GetSignature().GetComponents(), componentSizes);
    if (metas.empty())
        return;

    ArchetypeTable newTable{std::span<const ComponentMeta>(metas)};
    const int newIdCol = newTable.FindColumnIndex(id);

    ArchetypeTable& oldTable = arch.GetTable();
    const int oldIdCol = oldTable.FindColumnIndex(id);

    auto oldChunks = oldTable.GetChunks();
    for (auto& chunk : oldChunks)
    {
        const uint32_t count = chunk.GetCount();
        for (uint32_t i = 0; i < count; ++i)
        {
            const EntityHandle e = chunk.GetEntityHandles()[i];
            const EntityLocation newLoc = newTable.AddEntity(e);
            // Re-fetch the chunk span after AddEntity (it may have grown m_Chunks).
            ArchetypeChunk& newChunk = newTable.GetChunks()[newLoc.ChunkIndex];

            // Copy all shared columns at min-size: unchanged components come over verbatim,
            // the changed one comes over as a raw prefix that we overwrite next.
            ArchetypeChunk::CopyRow(chunk, i, newChunk, newLoc.IndexInChunk);

            // Re-pack the changed component: start from the new defaults, then carry over
            // each field that survived the layout change (matched by name+type+size).
            if (newIdCol >= 0 && oldIdCol >= 0)
            {
                auto* dst = static_cast<std::uint8_t*>(newChunk.GetComponentRaw(newIdCol, newLoc.IndexInChunk));
                const auto* src = static_cast<const std::uint8_t*>(chunk.GetComponentRaw(oldIdCol, i));
                std::memcpy(dst, change.NewDefaults.data(), change.NewSize);
                for (const FieldByteMove& move : remap)
                    std::memcpy(dst + move.DstOffset, src + move.SrcOffset, move.Size);
            }

            EntityMetadata& md = entityMetadata[e.index];
            md.archetype = &arch;
            // Checked overload — asserts in debug if a grown layout's smaller per-chunk capacity
            // pushes a chunk/slot index past the uint16 metadata field, rather than silently truncating.
            md.SetLocation(newLoc);
        }
    }

    arch.m_Table = std::move(newTable);
    // (Move-plans are invalidated globally by MigrateComponentLayout once all archetypes migrate.)

    // Change signaling: the rebuilt table's rows arrived via direct table
    // AddEntity + CopyRow, bypassing AddEntityUnsafe's stamp. Migration is a
    // rare full-rebuild event — stamp every chunk (design: STAMP-ALL).
    const uint64 migrateVersion = NextGlobalSystemVersion();
    ArchetypeTable& migrated = arch.GetTable();
    for (std::size_t c = 0; c < migrated.GetChunkCount(); ++c)
        migrated.StampAllColumnVersions(c, migrateVersion);
}

bool World::MigrateComponentLayout(const ComponentLayoutChange& change)
{
    if (change.Id == 0 || change.NewSize == 0 || change.NewDefaults.size() != change.NewSize)
        return false;

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    const ComponentTypeId id = change.Id;

    // The recorded size lives in two places: the process-global BlobComponentHandler (updated
    // here, idempotent across worlds — the reload guarded out RegisterBlobComponent, so without
    // this the Add-menu size check would still reject the new layout) and this World's per-world
    // componentSizes + archetype column strides (updated below). Every world that HOLDS
    // instances of `id` must migrate after a size change — reload paths go through
    // MigrateComponentLayoutAcrossWorlds, which sweeps the live-world registry so an
    // un-migrated world can never keep an old stride while the shared handler reports the
    // new size.
    ComponentRegistry::UpdateBlobComponentSize(id, change.NewSize);

    // This world only has storage for the component if it has been placed here.
    if (componentSizes.find(id) == componentSizes.end())
        return false;
    componentSizes[id] = change.NewSize; // direct assign; other writers are first-write-wins

    const std::vector<FieldByteMove> remap = BuildFieldRemap(change.OldFields, change.NewFields);

    bool migrated = false;
    for (auto& [sig, archPtr] : archetypes)
    {
        Archetype* arch = archPtr.get();
        if (!arch || !arch->HasTable() || !arch->GetSignature().Contains(id))
            continue;
        // Rebuild the table at the new stride and re-pack its rows. An empty archetype just gets
        // a fresh empty table at the new layout (the row loop is a no-op), so no special case.
        MigrateArchetypeInstances(*arch, id, change, remap);
        migrated = true;
    }

    if (!migrated)
        return false;

    // Per-field carry diagnostics, at the one choke point both reload paths
    // (native migrator + C# blob schema ABI) share. A type/size change under a
    // kept name is a RESET (never a raw byte copy) and warrants a warning; a
    // removed field's value is dropped; added fields default quietly.
    {
        const ComponentRegistry::ComponentInfo* info = ComponentRegistry::GetComponentInfo(id);
        const std::string_view compName =
            info ? std::string_view(info->Name) : std::string_view("<unregistered>");
        for (const FieldInfo& of : change.OldFields)
        {
            const FieldInfo* match = nullptr;
            for (const FieldInfo& nf : change.NewFields)
            {
                if (nf.Name == of.Name)
                {
                    match = &nf;
                    break;
                }
            }
            if (!match)
                Logger::Log::Info("[ECS] Component '{}' field '{}' removed by the reload — value dropped",
                                  compName, of.Name);
            else if (match->Type != of.Type || match->Size != of.Size)
                Logger::Log::Warning("[ECS] Component '{}' field '{}' changed type/size across the "
                                     "reload — RESET to its default (no byte carry)",
                                     compName, of.Name);
        }
    }

    // Invalidate caches: baked move-plans hold stale column offsets; query caches key on the
    // structural version (they re-collect archetypes + re-resolve column offsets on a bump).
    for (auto& [sig, archPtr] : archetypes)
        if (archPtr)
            archPtr->m_MovePlans.clear();
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t World::CountComponentHooksOwnedByImage(uint64_t base, uint64_t size)
{
    if (base == 0 || size == 0)
        return 0;
    const auto inImage = [=](const void* pointer) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= base && address - base < size;
    };
    std::size_t count = 0;
    std::lock_guard<std::mutex> registryLock(LiveWorldsMutex());
    for (const World* world : LiveWorlds())
    {
        std::shared_lock worldLock(world->worldMutex);
        const auto countHooks = [&](const auto& hooks) {
            for (const auto& [type, hook] : hooks)
            {
                if (inImage(reinterpret_cast<const void*>(hook.Invoke)) || inImage(hook.UserData))
                    ++count;
            }
        };
        countHooks(world->m_OnRemoveHooks);
        countHooks(world->m_OnAddHooks);
        countHooks(world->m_OnSetHooks);
    }
    return count;
}

bool World::MigrateComponentLayoutAcrossWorlds(World& target, const ComponentLayoutChange& change)
{
    bool anyMigrated = false;
    bool sawTarget = false;
    std::size_t extraWorlds = 0;
    {
        // Held across the per-world migrations: consistent lock order with
        // ~World (registry mutex before worldMutex), and a world cannot be
        // destroyed mid-sweep.
        std::lock_guard<std::mutex> lock(LiveWorldsMutex());
        for (World* world : LiveWorlds())
        {
            if (world == &target)
                sawTarget = true;
            const bool migrated = world->MigrateComponentLayout(change);
            anyMigrated |= migrated;
            if (migrated && world != &target)
                ++extraWorlds;
        }
    }
    // A target invisible to this linked copy's registry (the scripting ABI's
    // statically-linked ECS operating on a host world) still always migrates.
    if (!sawTarget)
        anyMigrated |= target.MigrateComponentLayout(change);

    if (extraWorlds > 0)
        Logger::Log::Info("[ECS] Component layout change (id {}) migrated in {} additional live "
                          "world(s) beyond the target",
                          change.Id, extraWorlds);
    return anyMigrated;
}

std::size_t World::GetArchetypeCount() const
{
    std::shared_lock lock(worldMutex);
    return archetypes.size();
}

std::vector<Archetype*> World::GetAllArchetypes() const
{
    std::shared_lock lock(worldMutex);
    std::vector<Archetype*> result;
    result.reserve(archetypes.size());

    for (const auto& pair : archetypes)
    {
        result.push_back(pair.second.get());
    }

    return result;
}

namespace
{
// The enable-state half of the archetype match. The common shape — exclude by
// default, nothing opted back in — is one Intersects against a signature the
// query instantiation owns; the per-type opt-in walks the (tiny) exclude set
// so an opted-in tag stops rejecting while entity activity still does.
bool CarriesExcludedEnableStateTag(const ComponentSignature& signature,
                                   const ArchetypeFilter& filter)
{
    if (filter.DisabledExclude == nullptr)
        return false;
    if (filter.DisabledInclude == nullptr)
        return signature.Intersects(*filter.DisabledExclude);

    for (ComponentTypeId id : filter.DisabledExclude->GetComponents())
    {
        if (signature.Contains(id) && !filter.DisabledInclude->Contains(id))
            return true;
    }
    return false;
}
} // namespace

void World::CollectMatchingArchetypes(const ArchetypeFilter& filter,
                                      std::vector<Archetype*>& out) const
{
    std::shared_lock lock(worldMutex);
    out.clear();
    out.reserve(archetypes.size());

    for (const auto& pair : archetypes)
    {
        const ComponentSignature& signature = pair.second->GetSignature();
        if (filter.Required != nullptr && !signature.ContainsAll(*filter.Required))
            continue;
        if (filter.Include != nullptr && !signature.ContainsAll(*filter.Include))
            continue;
        if (filter.Exclude != nullptr && signature.Intersects(*filter.Exclude))
            continue;
        if (CarriesExcludedEnableStateTag(signature, filter))
            continue;
        out.push_back(pair.second.get());
    }
}

uint64 World::GetColumnStampCount() const
{
    std::shared_lock lock(worldMutex);
    uint64 total = 0;
    for (const auto& pair : archetypes)
    {
        const Archetype* archetype = pair.second.get();
        if (archetype->HasTable())
            total += archetype->GetTable().GetStampCount();
    }
    return total;
}

std::vector<EntityHandle> World::GetAliveEntitiesSnapshot() const
{
    std::vector<EntityHandle> out;
    GetAliveEntitiesSnapshot(out);
    return out;
}

void World::GetAliveEntitiesSnapshot(std::vector<EntityHandle>& out) const
{
    std::shared_lock lock(worldMutex);
    out.clear();
    out.reserve(entityCount.load(std::memory_order_relaxed));

    for (EntityIndex idx = 0; idx < static_cast<EntityIndex>(entityMetadata.size()); ++idx)
    {
        const auto& md = entityMetadata[idx];
        if (!md.alive)
            continue;
        out.emplace_back(idx, md.version);
    }
}

void World::Clear()
{
    if (IsRemovingOnThisThread() && t_Removal.All)
        return;
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    // Drop deferred commands before invalidating entity ids.
    for (auto& buffer : commandBuffers)
    {
        Command discarded;
        while (buffer->TryPop(discarded))
        {
        }
    }

    // Invoke OnRemove hooks for all live entities.
    InvokeAllRemoveHooks();

    archetypes.clear();
    entityMetadata.clear();
    freeIndices.clear();
    entityCount.store(0);
    structuralChangeVersion.fetch_add(1);
    ++m_WorldGeneration; // Invalidate thread_local archetype caches in CreateHandle
    bulkOperationMode = false;
    m_Singletons.clear();

    // Preserved scene components and field overrides are keyed by EntityHandle; entity indices are
    // recycled after Clear (versions restart with the metadata array), so a
    // stale entry would silently attach to an unrelated new entity, and
    // reopening a scene would accumulate duplicates.
    if (m_UnresolvedComponents)
        m_UnresolvedComponents->Clear();

    // Wipe the dirty feed — entity ids are recycled after Clear, so a
    // surviving entry would alias an unrelated new entity in the next scene.
    // The subscription itself persists (bootstrap-time property).
    m_DirtyFeed.Reset();

    // Lifecycle events: Clear is a single WorldReset SIGNAL, not a
    // per-entity Removed burst (Q4 — Clear already discarded all pending
    // deferred commands above, so per-entity event parity with "what
    // happened" is unattainable anyway). Wipes BOTH event windows (a buffer
    // surviving into the new scene would alias recycled entity indices) and
    // bumps the generation consumers key their full-reset on. OnRemove
    // hooks still fired per-entity above (HookParity carves out Clear).
    m_LifecycleEvents.SignalReset();

    // Reset thread-local buffer mapping for this World on the caller's thread.
    // Other threads' entries stay valid: their buffers were drained above but
    // never destroyed (only ~World frees them), and uint64 WorldId keys are
    // immune to address reuse.
    tlBuffers.PerWorldBuffer.erase(m_WorldId);
}

// BATCH CREATION IMPLEMENTATIONS

std::vector<EntityHandle> World::CreateBatchHandle(size_t count)
{
    std::vector<EntityHandle> entities;
    entities.reserve(count);

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    size_t currentSize = entityMetadata.size();
    size_t requiredSize = currentSize + count;
    if (requiredSize > entityMetadata.capacity())
    {
        entityMetadata.reserve(static_cast<size_t>(requiredSize * config.ArchetypeGrowthFactor));
    }

    for (size_t i = 0; i < count; ++i)
    {
        EntityIndex index;

        if (!freeIndices.empty())
        {
            index = freeIndices.back();
            freeIndices.pop_back();
        }
        else
        {
            index = static_cast<EntityIndex>(entityMetadata.size());
            entityMetadata.emplace_back();
        }

        auto& metadata = entityMetadata[index];
        metadata.version++;
        metadata.alive = true;
        metadata.archetype = nullptr;
        metadata.SetLocation(0, 0);

        entities.emplace_back(index, metadata.version);
    }

    entityCount.fetch_add(count, std::memory_order_relaxed);

    if (config.EnableDebugLogging)
    {
        Logger::Log::Info("[ECS] Batch created {} entities", count);
    }

    return entities;
}

std::vector<Entity> World::CreateBatch(size_t count)
{
    auto handles = CreateBatchHandle(count);
    std::vector<Entity> entities;
    entities.reserve(count);

    for (auto handle : handles)
    {
        entities.push_back(Entity(this, handle));
    }

    return entities;
}

void World::ReserveEntities(size_t count)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    entityMetadata.reserve(count);

    if (config.EnableDebugLogging)
    {
        Logger::Log::Info("[ECS] Reserved capacity for {} entities", count);
    }
}

void World::BeginBulkOperations()
{
    CheckRemovalReentry();
    bulkOperationMode = true;
    if (config.EnableDebugLogging)
    {
        Logger::Log::Info("[ECS] Bulk operations mode enabled");
    }
}

void World::EndBulkOperations()
{
    CheckRemovalReentry();
    if (bulkOperationMode)
    {
        ProcessCommands();
        bulkOperationMode = false;

        if (config.EnableDebugLogging)
        {
            Logger::Log::Info("[ECS] Bulk operations mode disabled");
        }
    }
}

EntityHandle World::CreateDirect()
{
    if (!bulkOperationMode)
    {
        return CreateEntity();
    }

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    EntityIndex index;
    if (!freeIndices.empty())
    {
        index = freeIndices.back();
        freeIndices.pop_back();
    }
    else
    {
        index = static_cast<EntityIndex>(entityMetadata.size());
        entityMetadata.emplace_back();
    }

    auto& metadata = entityMetadata[index];
    metadata.version++;
    metadata.alive = true;
    metadata.archetype = nullptr;
    metadata.SetLocation(0, 0);

    entityCount.fetch_add(1, std::memory_order_relaxed);

    return EntityHandle(index, metadata.version);
}

Archetype* World::GetEntityArchetype(EntityHandle entity) const
{
    std::shared_lock lock(worldMutex);

    if (!IsValid(entity))
    {
        return nullptr;
    }

    return entityMetadata[entity.index].archetype;
}

uint64 World::GetEntityColumnVersion(EntityHandle entity, ComponentTypeId typeId) const
{
    std::shared_lock lock(worldMutex);

    if (!IsValid(entity))
    {
        return 0;
    }

    const auto& metadata = entityMetadata[entity.index];
    if (!metadata.archetype)
    {
        return 0;
    }
    return metadata.archetype->GetColumnVersion(typeId, metadata.chunkIndex);
}

void World::StampComponentWriteBatch(const EntityHandle* entities, std::size_t count,
                                     ComponentTypeId typeId)
{
    if (!entities || count == 0)
    {
        return;
    }

    // Shared lock, exactly as the GetComponentForWrite grant takes: a stamp is
    // a store into the owning chunk's version array, not a structural edit.
    // Taken once for the whole pass — the writes it covers already happened.
    std::shared_lock lock(worldMutex);

    // One version for the pass: consumers compare `>` against their gate, so
    // entities that moved together read as a single event. Every entity of a
    // run that shares a table and chunk with the previous one would store the
    // same version into the same slot, so each run is stamped once. Runs are
    // long when the span keeps chunk neighbours together (entity index order).
    const uint64_t version = NextGlobalSystemVersion();
    const Archetype* runArchetype = nullptr;
    uint32_t runChunk = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const EntityHandle entity = entities[i];
        if (!IsValid(entity))
        {
            continue;
        }
        const auto& metadata = entityMetadata[entity.index];
        if (!metadata.archetype)
        {
            continue;
        }
        if (metadata.archetype == runArchetype && metadata.chunkIndex == runChunk)
        {
            continue;
        }
        runArchetype = metadata.archetype;
        runChunk = metadata.chunkIndex;
        metadata.archetype->StampColumnVersion(metadata.chunkIndex, typeId, version);
    }
}

EntityHandle World::CloneEntity(EntityHandle source)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    if (!IsValid(source))
    {
        Logger::Log::Warning("[ECS] Cannot clone invalid entity {}", source.id);
        return EntityHandle{};
    }

    Archetype* sourceArchetype = entityMetadata[source.index].archetype;
    EntityLocation sourceLoc = entityMetadata[source.index].GetLocation();

    if (!sourceArchetype)
    {
        return CreateEntityUnsafe();
    }

    EntityHandle newHandle = CreateEntityUnsafe();
    auto& metadata = entityMetadata[newHandle.index];
    metadata.archetype = sourceArchetype;
    EntityLocation newLoc = sourceArchetype->AddEntityUnsafe(newHandle);
    metadata.SetLocation(newLoc);

    // Copy all component data using CopyRow (merge-join of all columns).
    if (sourceArchetype->HasTable())
    {
        auto chunks = sourceArchetype->GetTable().GetChunks();
        if (sourceLoc.ChunkIndex < chunks.size() && newLoc.ChunkIndex < chunks.size())
        {
            ArchetypeChunk::CopyRow(
                chunks[sourceLoc.ChunkIndex], sourceLoc.IndexInChunk,
                chunks[newLoc.ChunkIndex], newLoc.IndexInChunk);
        }
    }

    // Lifecycle events (§6.1 C3): a duplicated entity (editor Ctrl+D) fires
    // Added per subscribed type in the cloned archetype's signature — the
    // clone never passes through the unified add body.
    RecordAddedForSignature(sourceArchetype->GetSignature(), newHandle);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return newHandle;
}

void World::CopyComponent(EntityHandle source, EntityHandle target, ComponentTypeId typeId)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    if (!IsValid(source) || !IsValid(target))
    {
        Logger::Log::Warning("[ECS] Cannot copy component - invalid entity handles");
        return;
    }

    auto* sourceArchetype = entityMetadata[source.index].archetype;
    if (!sourceArchetype)
    {
        Logger::Log::Warning("[ECS] Cannot copy component - source entity has no archetype");
        return;
    }

    auto& sourceMd = entityMetadata[source.index];
    auto& targetMd = entityMetadata[target.index];

    if (!sourceMd.archetype || !targetMd.archetype)
    {
        Logger::Log::Warning("[ECS] Cannot copy component - invalid archetype for source or target entity");
        return;
    }

    // Copy via type-erased raw access
    auto* handler = ComponentRegistry::GetHandler(typeId);
    if (handler)
    {
        handler->CopyComponent(sourceMd.archetype, sourceMd.chunkIndex, sourceMd.indexInChunk,
                               targetMd.archetype, targetMd.chunkIndex, targetMd.indexInChunk);
        // Change signaling: the handler wrote the target's column directly.
        targetMd.archetype->StampColumnVersion(targetMd.chunkIndex, typeId,
                                               NextGlobalSystemVersion());
    }
    else
    {
        Logger::Log::Warning("[ECS] No handler found for component type {} in copyComponent", typeId);
    }
}

// SERIALIZATION IMPLEMENTATIONS
//
// Format per entity: <version: EntityVersion> <componentCount: uint32_t>
//                    [<typeId: ComponentTypeId 8 bytes LE><dataSize: uint32_t><data...>]*
//
// ComponentTypeId is the consteval Hash64 of the normalized type name
// (Phase 1b). The same component name produces the same 8-byte hash on every
// compiler and platform — scenes saved on one OS load identically on another
// without a translation layer.

std::vector<uint8_t> World::SerializeEntityUnsafe(EntityHandle entity) const
{
    if (!IsValid(entity))
    {
        return {};
    }

    auto& metadata = entityMetadata[entity.index];
    std::vector<uint8_t> result;

    EntityVersion version = entity.version;
    result.insert(result.end(), reinterpret_cast<const uint8_t*>(&version),
                  reinterpret_cast<const uint8_t*>(&version) + sizeof(version));

    const auto typeIds = metadata.archetype ? metadata.archetype->GetSignature().GetComponents()
                                          : std::span<const ComponentTypeId>{};
    auto componentCount = static_cast<uint32_t>(typeIds.size());
    result.insert(result.end(), reinterpret_cast<const uint8_t*>(&componentCount),
                  reinterpret_cast<const uint8_t*>(&componentCount) + sizeof(componentCount));

    // Serialize each component
    for (auto typeId : typeIds)
    {
        result.insert(result.end(), reinterpret_cast<const uint8_t*>(&typeId),
                      reinterpret_cast<const uint8_t*>(&typeId) + sizeof(typeId));

        auto* handler = ComponentRegistry::GetHandler(typeId);
        if (handler)
        {
            auto componentData = handler->SerializeComponent(metadata.archetype, metadata.chunkIndex, metadata.indexInChunk);
            auto dataSize = static_cast<uint32_t>(componentData.size());

            result.insert(result.end(), reinterpret_cast<const uint8_t*>(&dataSize),
                          reinterpret_cast<const uint8_t*>(&dataSize) + sizeof(dataSize));

            result.insert(result.end(), componentData.begin(), componentData.end());
        }
    }

    return result;
}

std::vector<uint8_t> World::SerializeEntity(EntityHandle entity)
{
    std::shared_lock lock(worldMutex);
    return SerializeEntityUnsafe(entity);
}

EntityHandle World::DeserializeEntity(std::span<const uint8_t> data, EntityHandle entity)
{
    if (data.size() < sizeof(EntityVersion) + sizeof(uint32_t))
    {
        return EntityHandle::Invalid();
    }

    size_t offset = 0;

    EntityVersion version;
    std::memcpy(&version, data.data() + offset, sizeof(version));
    offset += sizeof(version);

    uint32_t componentCount;
    std::memcpy(&componentCount, data.data() + offset, sizeof(componentCount));
    offset += sizeof(componentCount);

    for (uint32_t i = 0; i < componentCount; ++i)
    {
        if (offset + sizeof(ComponentTypeId) + sizeof(uint32_t) > data.size())
        {
            break;
        }

        ComponentTypeId typeId;
        std::memcpy(&typeId, data.data() + offset, sizeof(typeId));
        offset += sizeof(typeId);

        uint32_t dataSize;
        std::memcpy(&dataSize, data.data() + offset, sizeof(dataSize));
        offset += sizeof(dataSize);

        if (offset + dataSize > data.size())
        {
            break;
        }

        std::vector<uint8_t> componentData(data.begin() + offset, data.begin() + offset + dataSize);
        offset += dataSize;

        auto* handler = ComponentRegistry::GetHandler(typeId);
        if (handler)
        {
            {
                CheckRemovalReentry();
                std::unique_lock lock(worldMutex);
                componentSizes.try_emplace(typeId, handler->GetComponentSize());
            }

            handler->AddComponentFromData(this, entity, componentData);
        }
    }

    ProcessCommands();
    return entity;
}

std::string World::GetEntityAsJsonUnsafe(EntityHandle entity) const
{
    if (!IsValid(entity))
    {
        return "{}";
    }

    auto& metadata = entityMetadata[entity.index];
    if (!metadata.archetype)
    {
        return "{\"entity\":{\"index\":" + std::to_string(entity.index) +
               ",\"version\":" + std::to_string(entity.version) + ",\"components\":[]}}";
    }

    std::ostringstream json;
    json << "{\"entity\":{";
    json << "\"index\":" << entity.index << ",";
    json << "\"version\":" << entity.version << ",";
    json << "\"components\":[";

    auto& signature = metadata.archetype->GetSignature();
    bool first = true;
    for (auto typeId : signature.GetComponents())
    {
        if (!first)
            json << ",";

        auto* handler = ComponentRegistry::GetHandler(typeId);
        if (handler)
        {
            json << handler->GetComponentAsJson(metadata.archetype, metadata.chunkIndex, metadata.indexInChunk);
        }
        first = false;
    }

    json << "]}}";
    return json.str();
}

std::string World::GetEntityAsJson(EntityHandle entity)
{
    std::shared_lock lock(worldMutex);
    return GetEntityAsJsonUnsafe(entity);
}

namespace
{
constexpr uint32_t kWorldSnapshotMagic = 0x57435345; // ECSW
constexpr uint32_t kWorldSnapshotVersion = 1;
}

std::vector<uint8_t> World::SerializeWorld()
{
    std::shared_lock lock(worldMutex);

    std::vector<uint8_t> result;
    const auto append = [&result](const auto& value)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        result.insert(result.end(), bytes, bytes + sizeof(value));
    };
    // ECSW v1: magic, format version, slot count, free count, then each slot's
    // generation + entity-data length + data (length 0 = dead), then free indices
    // in allocation order. Dead slots absent from the free list are undo-reserved.
    // Explicit fields only: archetype pointers/locations are rebuilt, not serialized.
    append(kWorldSnapshotMagic);
    append(kWorldSnapshotVersion);
    append(static_cast<uint32_t>(entityMetadata.size()));
    append(static_cast<uint32_t>(freeIndices.size()));

    for (EntityIndex i = 0; i < entityMetadata.size(); ++i)
    {
        const auto& metadata = entityMetadata[i];
        append(metadata.version);
        if (metadata.alive)
        {
            EntityHandle entity(i, metadata.version);
            auto entityData = SerializeEntityUnsafe(entity);
            append(static_cast<uint32_t>(entityData.size()));
            result.insert(result.end(), entityData.begin(), entityData.end());
        }
        else
        {
            append(uint32_t{0});
        }
    }
    for (const auto index : freeIndices)
        append(index);

    return result;
}

void World::DeserializeWorld(const std::vector<uint8_t>& data)
{
    if (data.size() < sizeof(uint32_t))
    {
        return;
    }

    size_t offset = 0;
    uint32_t magic;
    std::memcpy(&magic, data.data() + offset, sizeof(magic));
    offset += sizeof(magic);
    if (magic != kWorldSnapshotMagic)
        return;

    // Validate the whole envelope before Clear: malformed snapshots must not
    // replace the current scene with a partially restored world.
    const auto read = [&](auto& value) {
        if (sizeof(value) > data.size() - offset)
            return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    };
    uint32_t formatVersion, slotCount, freeCount;
    if (!read(formatVersion) || formatVersion != kWorldSnapshotVersion || !read(slotCount) || !read(freeCount)
        || slotCount > kEntityIndexMask + 1u || freeCount > slotCount
        || uint64_t{slotCount} * (sizeof(EntityVersion) + sizeof(uint32_t))
           + uint64_t{freeCount} * sizeof(EntityIndex) > data.size() - offset)
        return;

    struct SlotSnapshot
    {
        EntityVersion Version;
        std::span<const uint8_t> Data;
    };
    std::vector<SlotSnapshot> slots;
    slots.reserve(slotCount);
    uint32_t aliveCount = 0;
    for (uint32_t i = 0; i < slotCount; ++i)
    {
        EntityVersion version;
        uint32_t dataSize;
        if (!read(version) || version > kEntityVersionMask || !read(dataSize)
            || dataSize > data.size() - offset)
            return;
        const std::span<const uint8_t> entityData(data.data() + offset, dataSize);
        offset += dataSize;
        if (!entityData.empty())
        {
            // Validate the entity envelope too, including every component's
            // byte range. Unknown component types remain skippable on restore.
            constexpr auto headerBytes = sizeof(EntityVersion) + sizeof(uint32_t);
            if (entityData.size() < headerBytes)
                return;
            EntityVersion entityVersion;
            uint32_t componentCount;
            std::memcpy(&entityVersion, entityData.data(), sizeof(entityVersion));
            std::memcpy(&componentCount, entityData.data() + sizeof(entityVersion), sizeof(componentCount));
            if (entityVersion != version || !EntityHandle(i, version).IsValid())
                return;
            size_t cursor = headerBytes;
            for (uint32_t component = 0; component < componentCount; ++component)
            {
                constexpr auto componentHeaderBytes = sizeof(ComponentTypeId) + sizeof(uint32_t);
                if (componentHeaderBytes > entityData.size() - cursor)
                    return;
                uint32_t componentBytes;
                std::memcpy(&componentBytes, entityData.data() + cursor + sizeof(ComponentTypeId),
                            sizeof(componentBytes));
                cursor += componentHeaderBytes;
                if (componentBytes > entityData.size() - cursor)
                    return;
                cursor += componentBytes;
            }
            if (cursor != entityData.size())
                return;
            ++aliveCount;
        }
        slots.push_back({version, entityData});
    }
    std::vector<EntityIndex> restoredFreeIndices;
    restoredFreeIndices.reserve(freeCount);
    std::vector<bool> isFree(slotCount, false);
    for (uint32_t i = 0; i < freeCount; ++i)
    {
        EntityIndex index;
        if (!read(index) || index >= slotCount || !slots[index].Data.empty() || isFree[index])
            return;
        isFree[index] = true;
        restoredFreeIndices.push_back(index);
    }
    if (offset != data.size())
        return;

    // A snapshot restore brings back the same document with the same entity handles, so the scene
    // data this binary could not represent stays with it: Clear would otherwise hand the next save a
    // world that silently lost its preserved components, fields and reference paths.
    std::unique_ptr<UnresolvedComponentStore> unresolved = std::move(m_UnresolvedComponents);
    Clear();
    m_UnresolvedComponents = std::move(unresolved);
    {
        CheckRemovalReentry();
        std::unique_lock lock(worldMutex);
        entityMetadata.resize(slotCount);
        freeIndices = std::move(restoredFreeIndices);
        for (uint32_t i = 0; i < slotCount; ++i)
        {
            entityMetadata[i].version = slots[i].Version;
            entityMetadata[i].alive = !slots[i].Data.empty();
        }
        entityCount.store(aliveCount, std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < slotCount; ++i)
    {
        if (!slots[i].Data.empty())
            DeserializeEntity(slots[i].Data, EntityHandle(i, slots[i].Version));
    }
    if (m_UnresolvedComponents)
    {
        // Entries of an entity the snapshot does not hold (created after it, or its slot free in it)
        // go: that handle may be handed to an unrelated entity next. An undo-reserved slot (dead and
        // not free) keeps its entries, as the preserve-handle destroy does.
        const auto isStale = [&](EntityHandle entity)
        {
            return entity.index >= slotCount || slots[entity.index].Version != entity.version ||
                   isFree[entity.index];
        };
        std::erase_if(m_UnresolvedComponents->Map(), [&](const auto& entry) { return isStale(entry.first); });
        std::erase_if(m_UnresolvedComponents->FieldMap(),
                      [&](const auto& entry) { return isStale(entry.first); });
    }
}

std::string World::GetWorldAsJson()
{
    std::shared_lock lock(worldMutex);

    std::ostringstream json;
    json << "{\"world\":{";
    json << "\"entityCount\":" << entityCount.load(std::memory_order_relaxed) << ",";
    json << "\"archetypeCount\":" << archetypes.size() << ",";
    json << "\"entities\":[";

    bool first = true;
    for (EntityIndex i = 0; i < entityMetadata.size(); ++i)
    {
        const auto& metadata = entityMetadata[i];
        if (metadata.alive)
        {
            if (!first)
                json << ",";
            EntityHandle entity(i, metadata.version);
            json << GetEntityAsJsonUnsafe(entity);
            first = false;
        }
    }

    json << "]}}";
    return json.str();
}

// ============================================================================
// DYNAMIC ENTITY CREATION IMPLEMENTATIONS
// ============================================================================

Entity World::CreateFromBundle(const ComponentBundle& bundle)
{
    EntityHandle handle = CreateFromBundleHandle(bundle);
    return Entity(this, handle);
}

EntityHandle World::CreateFromBundleHandle(const ComponentBundle& bundle)
{
    const auto& signature = bundle.GetSignature();

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    // Register component sizes for archetype table creation.
    for (ComponentTypeId typeId : signature.GetComponents())
    {
        componentSizes.try_emplace(typeId, bundle.GetComponentSize(typeId));
    }

    Archetype* archetype = GetOrCreateArchetype(signature);

    EntityHandle entity = CreateEntityUnsafe();
    auto& metadata = entityMetadata[entity.index];
    metadata.archetype = archetype;
    EntityLocation loc = archetype->AddEntityUnsafe(entity);
    metadata.SetLocation(loc);

    // Set all components from bundle via type-erased raw write.
    for (ComponentTypeId typeId : signature.GetComponents())
    {
        const void* componentData = bundle.GetComponentData(typeId);
        size_t componentSize = bundle.GetComponentSize(typeId);

        if (componentData && componentSize > 0)
        {
            archetype->SetComponentRawAt(
                metadata.chunkIndex, metadata.indexInChunk,
                typeId, componentData, componentSize);
        }
    }

    // Lifecycle events (§6.1): bundle creation bypasses the unified add body.
    RecordAddedForSignature(signature, entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entity;
}

Entity World::CreateFromSignature(const ComponentSignature& signature)
{
    EntityHandle handle = CreateFromSignatureHandle(signature);
    return Entity(this, handle);
}

EntityHandle World::CreateFromSignatureHandle(const ComponentSignature& signature)
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    Archetype* archetype = GetOrCreateArchetype(signature);

    EntityHandle entity = CreateEntityUnsafe();
    auto& metadata = entityMetadata[entity.index];
    metadata.archetype = archetype;
    EntityLocation loc = archetype->AddEntityUnsafe(entity);
    metadata.SetLocation(loc);

    // Lifecycle events (§6.1 C3): signature-create makes query-visible
    // entities without writing data — Added still fires per subscribed type.
    RecordAddedForSignature(signature, entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entity;
}

std::vector<EntityHandle> World::CreateBatchFromBundle(size_t count, const ComponentBundle& bundle)
{
    const auto& signature = bundle.GetSignature();

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    for (ComponentTypeId typeId : signature.GetComponents())
    {
        componentSizes.try_emplace(typeId, bundle.GetComponentSize(typeId));
    }

    Archetype* archetype = GetOrCreateArchetype(signature);

    archetype->Reserve(archetype->GetEntityCount() + count);

    std::vector<EntityHandle> entities;
    entities.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        EntityHandle entity = CreateEntityUnsafe();
        auto& metadata = entityMetadata[entity.index];
        metadata.archetype = archetype;
        EntityLocation loc = archetype->AddEntityUnsafe(entity);
        metadata.SetLocation(loc);
        entities.push_back(entity);
    }

    for (EntityHandle entity : entities)
    {
        auto& metadata = entityMetadata[entity.index];
        for (ComponentTypeId typeId : signature.GetComponents())
        {
            const void* componentData = bundle.GetComponentData(typeId);
            size_t componentSize = bundle.GetComponentSize(typeId);

            if (componentData && componentSize > 0)
            {
                archetype->SetComponentRawAt(
                    metadata.chunkIndex, metadata.indexInChunk,
                    typeId, componentData, componentSize);
            }
        }
    }

    // Lifecycle events (§6.1 C3): batch-bundle is a separate body from
    // CreateFromBundleHandle — recorded per batch, per subscribed type.
    RecordAddedForSignatureBatch(signature, entities.data(), entities.size());

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entities;
}

// ============================================================================
// CHUNK COMPACTION
// ============================================================================

uint32_t Archetype::CompactChunks()
{
    if (!m_Table || !world)
        return 0;

    return m_Table->CompactChunks(
        [this](uint32_t oldChunkIdx, uint32_t newChunkIdx, ArchetypeChunk& chunk)
        {
            // Update all entities in the relocated chunk to their new chunk index.
            const EntityHandle* handles = chunk.GetEntityHandles();
            uint32_t count = chunk.GetCount();
            for (uint32_t i = 0; i < count; ++i)
            {
                EntityHandle eh = handles[i];
                if (eh.index < world->entityMetadata.size())
                {
                    auto& md = world->entityMetadata[eh.index];
                    if (md.archetype == this && md.chunkIndex == static_cast<uint16_t>(oldChunkIdx))
                    {
                        md.chunkIndex = static_cast<uint16_t>(newChunkIdx);
                    }
                }
            }
        });
}

// ============================================================================
// ARCHETYPE COMPATIBILITY METHODS
// ============================================================================

std::vector<EntityHandle> Archetype::CollectEntities() const
{
    std::vector<EntityHandle> result;
    if (!m_Table)
        return result;
    result.reserve(m_Table->GetEntityCount());
    for (const auto& chunk : m_Table->GetChunks())
    {
        const EntityHandle* handles = chunk.GetEntityHandles();
        uint32_t count = chunk.GetCount();
        for (uint32_t i = 0; i < count; ++i)
            result.push_back(handles[i]);
    }
    return result;
}

std::pair<void*, std::size_t> Archetype::GetChunkDataRaw(ComponentTypeId typeId, std::size_t chunkIndex)
{
    if (!m_Table)
        return {nullptr, 0};
    int colIdx = m_Table->FindColumnIndex(typeId);
    if (colIdx < 0)
        return {nullptr, 0};
    auto chunks = m_Table->GetChunks();
    if (chunkIndex >= chunks.size())
        return {nullptr, 0};
    // Change signaling: this hands out a mutable span over the column — a
    // write grant (managed/ABI writers, including the v1.6 slice paths).
    // Managed READS should migrate to the read-only ABI entry (v1.7) so they
    // stop over-stamping (design Q7/M11).
    if (world)
    {
        m_Table->StampColumnVersion(chunkIndex, static_cast<std::size_t>(colIdx),
                                    world->NextGlobalSystemVersion());
        // Dirty-feed coverage for the span-writer class (P2 amendment A4):
        // a mutable span is invisible to both the unified Set body and the
        // in-place bump helper, so a subscribed-type grant emits the whole
        // chunk — coarse over-report, symmetric with the coarse
        // stamp-at-grant above. Without this a managed WorldTransform span
        // write would freeze feed consumers indefinitely (data-only write:
        // no reconcile trigger, no feed entry).
        if (world->IsComponentDirtyFeedEnabledFor(typeId))
        {
            auto& chunk = chunks[chunkIndex];
            world->EmitComponentDirtyBatch(typeId, chunk.GetEntityHandles(),
                                           chunk.GetCount());
        }
    }
    return {chunks[chunkIndex].GetColumnRaw(colIdx), chunks[chunkIndex].GetCount()};
}

std::pair<const void*, std::size_t> Archetype::GetChunkDataRaw(ComponentTypeId typeId, std::size_t chunkIndex) const
{
    if (!m_Table)
        return {nullptr, 0};
    int colIdx = m_Table->FindColumnIndex(typeId);
    if (colIdx < 0)
        return {nullptr, 0};
    auto chunks = m_Table->GetChunks();
    if (chunkIndex >= chunks.size())
        return {nullptr, 0};
    return {chunks[chunkIndex].GetColumnRaw(colIdx), chunks[chunkIndex].GetCount()};
}

// ============================================================================
// ARCHETYPE MANAGEMENT IMPLEMENTATIONS
// ============================================================================

size_t World::CompactAllChunks()
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    size_t totalFreed = 0;
    for (auto& [sig, archetype] : archetypes)
    {
        totalFreed += archetype->CompactChunks();
    }

    // Chunk layout changed (empty chunks freed, others may have been swapped).
    // Bump structural version so caches relying on "data location may have moved"
    // signals (e.g. user-side change tracking) can detect the invalidation.
    if (totalFreed > 0)
        structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);

    return totalFreed;
}

size_t World::PruneEmptyArchetypes()
{
    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    // Two-pass prune: first collect the set of archetypes to remove, then erase
    // all graph edges in surviving archetypes that point to them, THEN destroy.
    // This prevents dangling Archetype* pointers in m_AddEdges / m_RemoveEdges.
    std::unordered_set<Archetype*> toPrune;
    for (auto& [sig, arch] : archetypes)
    {
        if (arch->GetEntityCount() == 0)
            toPrune.insert(arch.get());
    }

    if (toPrune.empty())
        return 0;

    // Scrub edges in surviving archetypes.
    for (auto& [sig, arch] : archetypes)
    {
        if (toPrune.count(arch.get()))
            continue;
        arch->EraseEdgesTo(toPrune);
    }

    // Now safe to destroy the archetypes themselves.
    size_t prunedCount = 0;
    for (auto it = archetypes.begin(); it != archetypes.end();)
    {
        if (toPrune.count(it->second.get()))
        {
            it = archetypes.erase(it);
            ++prunedCount;
        }
        else
        {
            ++it;
        }
    }

    // Invalidate query caches (so Query::UpdateCache rebuilds m_CachedArchetypes)
    // and CreateHandle caches (so thread_local cached Archetype* is discarded).
    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    ++m_WorldGeneration;

    Logger::Log::Info("[ECS] Pruned {} empty archetypes", prunedCount);
    return prunedCount;
}

} // namespace ECS
} // namespace GameEngine
