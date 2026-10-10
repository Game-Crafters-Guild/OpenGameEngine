#pragma once
#ifndef GE_ECS_WORLD_TEMPLATE_IMPLEMENTATIONS_INL
#define GE_ECS_WORLD_TEMPLATE_IMPLEMENTATIONS_INL

// WorldTemplateImplementations.inl
// Template implementations for World class methods with C++20 Auto-Registration.
// All component read/write goes through the colocated ArchetypeTable.

#include "ECS/RequiredComponents.h"

// ============================================================================
// TEMPLATE IMPLEMENTATIONS WITH AUTO-REGISTRATION
// ============================================================================

namespace GameEngine::ECS {

// Deferred: queues the add (and any required-component adds) to the
// thread-local command buffer; falls back to immediate execution when full.
// Every required component is queued and decided at the flush, not here: the
// world at push time does not know what the commands queued before this one
// will add or remove by then.
template<Component T>
void World::AddComponent(EntityHandle entity, const T& component) {
    AutoComponentRegistrar<T>::EnsureRegistered();

    auto& buffer = GetCommandBuffer();
    bool ok = true;
    auto pushReq = [&](auto tag)
    {
        using Req = typename decltype(tag)::type;
        ok = ok && buffer.TryPush(Command::addRequiredComponent(entity, Req{}));
    };
    using ReqList = typename Detail::RequiredComponents<T>::type;
    [&]<typename... Reqs>(Detail::TypeList<Reqs...>)
    { (pushReq(Detail::TypeTag<Reqs>{}), ...); }(ReqList{});

    ok = ok && buffer.TryPush(Command::addComponent(entity, component));

    if (!ok) {
        AddComponentImmediate(entity, component);
    }
}

template<Component T>
void World::AddComponentImmediate(EntityHandle entity, const T& component) {
    CheckRemovalReentry();
    AutoComponentRegistrar<T>::EnsureRegistered();
    Detail::EnsureRequiredComponents<T>(*this, entity);
    AddComponentImpl(entity, component);
}

// Deferred: queues the remove to the thread-local command buffer; falls back
// to immediate execution when full.
//
// No componentSizes ensure here (it used to take an exclusive world lock per
// deferred remove): removal never needs T's size. The target archetype
// excludes T, and the sizes of the entity's remaining components were
// registered when they were added.
template<Component T>
void World::RemoveComponent(EntityHandle entity) {
    AutoComponentRegistrar<T>::EnsureRegistered();

    auto& buffer = GetCommandBuffer();
    Command cmd;
    cmd.type = Command::REMOVE_COMPONENT;
    cmd.entity = entity;
    cmd.componentType = GetComponentTypeId<T>();
    if (!buffer.TryPush(std::move(cmd))) {
        RemoveComponentImpl<T>(entity);
    }
}

template<Component T>
void World::RemoveComponentImmediate(EntityHandle entity) {
    AutoComponentRegistrar<T>::EnsureRegistered();
    RemoveComponentImpl<T>(entity);
}

template<Component T, bool Stamp>
T* World::GetComponentLookup(EntityHandle entity) {
    AutoComponentRegistrar<T>::EnsureRegistered();
    ComponentTypeId typeId = GetComponentTypeId<T>();

    // Ensure component size registered.
    {
        std::shared_lock readLock(worldMutex);
        if (componentSizes.find(typeId) == componentSizes.end())
        {
            readLock.unlock();
            std::unique_lock writeLock(worldMutex);
            componentSizes.try_emplace(typeId, sizeof(T));
        }
    }

    std::shared_lock lock(worldMutex);

    if (!IsValid(entity)) {
        return nullptr;
    }

    auto& metadata = entityMetadata[entity.index];
    if (!metadata.archetype) {
        return nullptr;
    }

    T* ptr = metadata.archetype->GetComponentAt<T>(metadata.chunkIndex, metadata.indexInChunk);

    if constexpr (Stamp) {
        // The mutable pointer is a write grant — callers write through it in
        // place (e.g. the editor's set_component handler), so stamp the
        // requested column of the owning chunk. Whole-column false positives
        // by design; false negatives never.
        if (ptr)
            metadata.archetype->StampColumnVersion(metadata.chunkIndex, typeId, NextGlobalSystemVersion());
    }

    return ptr;
}

template<Component T>
T* World::GetComponentForWrite(EntityHandle entity) {
    return GetComponentLookup<T, /*Stamp=*/true>(entity);
}

template<Component T>
const T* World::GetComponent(EntityHandle entity) const {
    // Never stamps: read access must not launder into the write-grant path
    // (design C1/C5 — the name split exists so per-frame pollers like the
    // SceneTlas refit loop can never self-dirty change filters).
    return const_cast<World*>(this)->GetComponentLookup<T, /*Stamp=*/false>(entity);
}

template<Component... Ts>
void World::GetComponentsBatch(std::span<const EntityHandle> entities,
                               std::span<std::tuple<const Ts*...>> out) const {
    assert(entities.size() == out.size());
    (AutoComponentRegistrar<Ts>::EnsureRegistered(), ...);

    // Known serial residue (refit impl review F3): column resolution still
    // runs FindColumnIndex per entity per type under the shared hold. A
    // per-(archetype, type) column-index cache would amortize it to one
    // lookup per archetype run — the next lever if a profile names this
    // loop, deliberately not built speculatively.

    // One-time component-size registration (first touch per type per world)
    // — the same shared-then-unique upgrade GetComponentLookup performs,
    // hoisted out of the per-entity loop.
    {
        bool allRegistered = true;
        {
            std::shared_lock readLock(worldMutex);
            ((allRegistered = allRegistered
                  && componentSizes.find(GetComponentTypeId<Ts>()) != componentSizes.end()),
             ...);
        }
        if (!allRegistered) {
            auto* self = const_cast<World*>(this);
            std::unique_lock writeLock(self->worldMutex);
            (self->componentSizes.try_emplace(GetComponentTypeId<Ts>(), sizeof(Ts)), ...);
        }
    }

    std::shared_lock lock(worldMutex);
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const EntityHandle entity = entities[i];
        if (!IsValid(entity)) {
            out[i] = {};
            continue;
        }
        const auto& metadata = entityMetadata[entity.index];
        const Archetype* archetype = metadata.archetype;
        if (!archetype) {
            out[i] = {};
            continue;
        }
        out[i] = std::tuple<const Ts*...>{
            archetype->GetComponentAt<Ts>(metadata.chunkIndex, metadata.indexInChunk)...};
    }
}

template<Component T>
bool World::HasComponent(EntityHandle entity) const {
    AutoComponentRegistrar<T>::EnsureRegistered();
    return GetComponent<T>(entity) != nullptr;
}

template<typename T>
void World::AddComponentImpl(EntityHandle entity, const T& component) {
    // Components are trivially copyable; typed writes and handler replay share
    // the same locked byte operation.
    static_assert(Component<T>, "AddComponentImpl requires a Component type");
    AddComponentImpl(entity, GetComponentTypeId<T>(), &component, sizeof(T));
}

template<typename T>
void World::RemoveComponentImpl(EntityHandle entity) {
    static_assert(Component<T>, "RemoveComponentImpl requires a Component type");
    RemoveComponentImpl(entity, GetComponentTypeId<T>());
}

template<typename T>
void World::SetComponentDirect(EntityHandle entity, const T& component) {
    if (bulkOperationMode) {
        AddComponentImpl(entity, component);
    } else {
        AddComponent(entity, component);
    }
}

template<typename T>
void World::SetBulkComponents(const std::vector<EntityHandle>& entities,
                             const std::vector<T>& components) {
    static_assert(Component<T>, "SetBulkComponents requires a Component type");
    if (entities.size() != components.size()) {
        return;
    }

    const ComponentTypeId typeId = GetComponentTypeId<T>();

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);
    componentSizes.try_emplace(typeId, sizeof(T));

    // Structural transitions go through the O(1) archetype edge cache instead
    // of copying the full signature and hashing it per entity. The T column's
    // byte offset is resolved once per distinct destination archetype (bulk
    // sets overwhelmingly target one), so the write is a direct chunk access
    // with no per-entity column scan.
    Archetype* offsetArchetype = nullptr;
    uint32_t columnOffset = 0;
    int columnIndex = -1;

    // One write-grant version for the whole batch (design §4.4 allows per-
    // batch increments). Structural cases below stamp through AddEntityUnsafe
    // / MoveEntity; this covers the data-only cached-offset writes.
    const uint64 stampVersion = NextGlobalSystemVersion();

    // Lazily-resolved destination for entities that had no archetype at all.
    Archetype* emptyToT = nullptr;

    for (size_t i = 0; i < entities.size(); ++i) {
        const EntityHandle entity = entities[i];
        if (!IsValid(entity)) {
            continue;
        }

        auto& metadata = entityMetadata[entity.index];
        Archetype* current = metadata.archetype;

        Archetype* target;
        if (current && current->GetSignature().Contains(typeId)) {
            target = current; // data-only set, no structural change
        } else if (current) {
            target = GetOrCreateArchetypeForAdd(current, typeId);
            MoveEntity(entity, current, target);
            // Lifecycle events: a structural ADD outside the unified body
            // (§6.1 completeness — an unrecorded bulk add would be an
            // R1-class silent false negative for Added consumers).
            RecordComponentAdded(typeId, entity);
        } else {
            if (!emptyToT) {
                ComponentSignature sig;
                sig.Add(typeId);
                emptyToT = GetOrCreateArchetype(sig);
            }
            target = emptyToT;
            EntityLocation loc = target->AddEntityUnsafe(entity);
            metadata.archetype = target;
            metadata.SetLocation(loc);
            RecordComponentAdded(typeId, entity);
        }

        if (target != offsetArchetype) {
            if (!target->HasTable()) {
                continue;
            }
            const auto& layout = target->GetTable().GetLayout();
            columnIndex = layout.FindColumnIndex(typeId);
            if (columnIndex < 0) {
                continue;
            }
            offsetArchetype = target;
            columnOffset = layout.Columns[columnIndex].Offset;
        }

        auto chunks = target->GetTable().GetChunks();
        chunks[metadata.chunkIndex].template GetColumnByOffset<T>(columnOffset)[metadata.indexInChunk] =
            components[i];
        // Data-only bulk write bypasses the unified add-or-set body — stamp
        // the touched chunk's T column here.
        target->GetTable().StampColumnVersion(metadata.chunkIndex,
                                              static_cast<std::size_t>(columnIndex), stampVersion);
    }

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// TEMPLATE-BASED ENTITY CREATION IMPLEMENTATIONS
// ============================================================================

template<Component... Components>
Entity World::Create(const Components&... components) {
    // Delegate to the handle-returning variant so both paths benefit from the
    // thread_local archetype+column-offset cache (CreateHandle::s_Cache).
    return Entity(this, CreateHandle<Components...>(components...));
}

template<Component... Components>
EntityHandle World::CreateHandle(const Components&... components) {
    (AutoComponentRegistrar<Components>::EnsureRegistered(), ...);

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    // Fast path: cache archetype + column byte offsets for repeated CreateHandle<same types> calls.
    // Eliminates FindColumnIndex (linear scan) and bounds checks on every entity creation.
    // Keyed on (WorldId, Generation) to survive World address reuse and Clear() cycles.
    // - WorldId: monotonic uint64 assigned per World instance — immune to address reuse.
    // - Generation: incremented on Clear() and destruction — catches same-World reset.
    struct CachedArchetypeLookup {
        uint64 CachedWorldId = 0;
        Archetype* CachedArchetype = nullptr;
        std::size_t CachedGeneration = 0;
        std::array<uint32_t, sizeof...(Components)> ColOffsets{};
    };
    static thread_local CachedArchetypeLookup s_Cache;

    Archetype* archetype;
    if (s_Cache.CachedWorldId == m_WorldId &&
        s_Cache.CachedArchetype != nullptr &&
        s_Cache.CachedGeneration == m_WorldGeneration)
    {
        archetype = s_Cache.CachedArchetype;
    }
    else
    {
        const std::array<ComponentTypeId, sizeof...(Components)> typeIds = {
            GetComponentTypeId<Components>()...
        };

        bool allRegistered = true;
        for (auto id : typeIds) {
            if (componentSizes.find(id) == componentSizes.end()) {
                allRegistered = false;
                break;
            }
        }
        if (!allRegistered) {
            ((componentSizes.try_emplace(GetComponentTypeId<Components>(), sizeof(Components))), ...);
        }

        ComponentSignature signature;
        for (auto id : typeIds) signature.Add(id);
        archetype = GetOrCreateArchetype(signature);

        // Cache column byte offsets for direct writes (no FindColumnIndex per entity).
        if (archetype->HasTable()) {
            const auto& layout = archetype->GetTable().GetLayout();
            std::size_t idx = 0;
            ((s_Cache.ColOffsets[idx++] = [&]() -> uint32_t {
                int ci = layout.FindColumnIndex(GetComponentTypeId<Components>());
                return (ci >= 0) ? layout.Columns[ci].Offset : 0;
            }()), ...);
        }

        s_Cache.CachedWorldId = m_WorldId;
        s_Cache.CachedArchetype = archetype;
        s_Cache.CachedGeneration = m_WorldGeneration;
    }

    EntityHandle entity = CreateEntityUnsafe();
    auto& metadata = entityMetadata[entity.index];
    metadata.archetype = archetype;
    EntityLocation loc = archetype->AddEntityUnsafe(entity);
    metadata.SetLocation(loc);

    // Write component data directly using cached byte offsets — no FindColumnIndex, no bounds checks.
    {
        auto chunks = archetype->GetTable().GetChunks();
        auto& chunk = chunks[loc.ChunkIndex];
        std::size_t colI = 0;
        ((chunk.template GetColumnByOffset<Components>(s_Cache.ColOffsets[colI++])[loc.IndexInChunk] = components), ...);
    }

    // Lifecycle events (§6.1): typed creation bypasses the unified add body.
    RecordAddedForSignature(archetype->GetSignature(), entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entity;
}

template<Component... Components>
Entity World::CreateInArchetype() {
    EntityHandle handle = CreateInArchetypeHandle<Components...>();
    return Entity(this, handle);
}

template<Component... Components>
EntityHandle World::CreateInArchetypeHandle() {
    (AutoComponentRegistrar<Components>::EnsureRegistered(), ...);

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    ((componentSizes.try_emplace(GetComponentTypeId<Components>(), sizeof(Components))), ...);

    ComponentSignature signature;
    (signature.Add(GetComponentTypeId<Components>()), ...);

    Archetype* archetype = GetOrCreateArchetype(signature);

    EntityHandle entity = CreateEntityUnsafe();
    auto& metadata = entityMetadata[entity.index];
    metadata.archetype = archetype;
    EntityLocation loc = archetype->AddEntityUnsafe(entity);
    metadata.SetLocation(loc);

    ((archetype->SetComponentAt<Components>(metadata.chunkIndex, metadata.indexInChunk, Components{})), ...);

    // Lifecycle events (§6.1): archetype creation bypasses the unified add body.
    RecordAddedForSignature(archetype->GetSignature(), entity);

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entity;
}

template<Component... Components>
std::vector<EntityHandle> World::CreateBatchHandle(size_t count, const Components&... components) {
    (AutoComponentRegistrar<Components>::EnsureRegistered(), ...);

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    ((componentSizes.try_emplace(GetComponentTypeId<Components>(), sizeof(Components))), ...);

    ComponentSignature signature;
    (signature.Add(GetComponentTypeId<Components>()), ...);

    Archetype* archetype = GetOrCreateArchetype(signature);
    archetype->Reserve(archetype->GetEntityCount() + count);

    // Pre-reserve entityMetadata so the per-entity emplace_back doesn't
    // trigger geometric regrowth mid-batch. freeIndices may absorb some of
    // `count`, so this is an upper bound — harmless over-reservation.
    if (entityMetadata.size() + count > entityMetadata.capacity()) {
        entityMetadata.reserve(entityMetadata.size() + count);
    }

    std::vector<EntityHandle> entities;
    entities.reserve(count);

    // Cache each component's column byte offset ONCE — avoids a per-entity,
    // per-component FindColumnIndex (linear scan) in the hot loop. Mirrors
    // CreateHandle's s_Cache.ColOffsets strategy; this path sees it for free
    // because every entity in the batch lives in the same archetype.
    std::array<uint32_t, sizeof...(Components)> colOffsets{};
    if (archetype->HasTable()) {
        const auto& layout = archetype->GetTable().GetLayout();
        std::size_t idx = 0;
        ((colOffsets[idx++] = [&]() -> uint32_t {
            int ci = layout.FindColumnIndex(GetComponentTypeId<Components>());
            return (ci >= 0) ? layout.Columns[ci].Offset : 0;
        }()), ...);
    }

    // Single-pass create + write. Previously two loops iterated the entity set
    // twice (create-only then write-only). Merging halves the loop overhead
    // and keeps chunk memory hot in cache between the handle write and the
    // component writes.
    for (size_t i = 0; i < count; ++i) {
        EntityHandle entity = CreateEntityUnsafe();
        auto& metadata = entityMetadata[entity.index];
        metadata.archetype = archetype;
        EntityLocation loc = archetype->AddEntityUnsafe(entity);
        metadata.SetLocation(loc);

        auto chunks = archetype->GetTable().GetChunks();
        auto& chunk = chunks[loc.ChunkIndex];
        std::size_t colI = 0;
        ((chunk.template GetColumnByOffset<Components>(colOffsets[colI++])[loc.IndexInChunk] = components), ...);

        entities.push_back(entity);
    }

    // Lifecycle events (§6.1): one batched append per subscribed type.
    RecordAddedForSignatureBatch(archetype->GetSignature(), entities.data(), entities.size());

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entities;
}

template<Component... Components, typename InitFn>
std::vector<EntityHandle> World::CreateBatchWithInit(size_t count, InitFn&& initFn) {
    static_assert(sizeof...(Components) > 0,
                  "CreateBatchWithInit requires at least one component type");
    static_assert(std::is_invocable_v<InitFn, std::size_t, Components&...>,
                  "InitFn must be invocable as initFn(size_t index, Components&...)");

    (AutoComponentRegistrar<Components>::EnsureRegistered(), ...);

    CheckRemovalReentry();
    std::unique_lock lock(worldMutex);

    ((componentSizes.try_emplace(GetComponentTypeId<Components>(), sizeof(Components))), ...);

    ComponentSignature signature;
    (signature.Add(GetComponentTypeId<Components>()), ...);

    Archetype* archetype = GetOrCreateArchetype(signature);
    archetype->Reserve(archetype->GetEntityCount() + count);

    // Pre-reserve entityMetadata (see CreateBatchHandle for rationale).
    if (entityMetadata.size() + count > entityMetadata.capacity()) {
        entityMetadata.reserve(entityMetadata.size() + count);
    }

    std::vector<EntityHandle> entities;
    entities.reserve(count);

    // Cache each component's column byte offset once — one per-batch instead
    // of one per-entity-per-component. Same pattern as CreateBatchHandle.
    std::array<uint32_t, sizeof...(Components)> colOffsets{};
    if (archetype->HasTable()) {
        const auto& layout = archetype->GetTable().GetLayout();
        std::size_t idx = 0;
        ((colOffsets[idx++] = [&]() -> uint32_t {
            int ci = layout.FindColumnIndex(GetComponentTypeId<Components>());
            return (ci >= 0) ? layout.Columns[ci].Offset : 0;
        }()), ...);
    }

    // Single-pass: create entity, resolve chunk+slot, hand component refs
    // directly from column storage to the caller's init function.
    // Strong exception safety: if initFn throws, every entity created so far
    // is rolled back (swap-popped from archetype storage, metadata marked
    // dead, index returned to freeIndices) before the exception propagates.
    // No hooks fire for rolled-back entities — they were never observable.
    try {
        for (size_t i = 0; i < count; ++i) {
            EntityHandle entity = CreateEntityUnsafe();
            auto& metadata = entityMetadata[entity.index];
            metadata.archetype = archetype;
            EntityLocation loc = archetype->AddEntityUnsafe(entity);
            metadata.SetLocation(loc);
            entities.push_back(entity);  // push BEFORE initFn so rollback sees it

            auto chunks = archetype->GetTable().GetChunks();
            auto& chunk = chunks[loc.ChunkIndex];

            // Dispatch with per-component-index ordering from std::index_sequence
            // rather than `colI++` in a function-arg pack expansion (arg evaluation
            // order is unspecified in C++, which would make post-increment UB).
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                initFn(i,
                    chunk.template GetColumnByOffset<Components>(colOffsets[Is])[loc.IndexInChunk]...);
            }(std::make_index_sequence<sizeof...(Components)>{});
        }
    } catch (...) {
        for (auto it = entities.rbegin(); it != entities.rend(); ++it) {
            EntityHandle handle = *it;
            auto& m = entityMetadata[handle.index];
            if (m.archetype) {
                m.archetype->RemoveEntityUnsafe(m.GetLocation());
            }
            m.alive = false;
            m.archetype = nullptr;
            freeIndices.push_back(handle.index);
        }
        entityCount.fetch_sub(entities.size(), std::memory_order_relaxed);
        throw;
    }

    // Lifecycle events (§6.1 C15): recorded only AFTER every initFn
    // completed — the rollback above swap-pops the batch as "never
    // observable", and a phantom Added event would violate exactly that
    // invariant (RollbackEmitsNothing). The rollback also bypasses
    // DestroyEntityInternal, so it emits no Removed events either.
    RecordAddedForSignatureBatch(archetype->GetSignature(), entities.data(), entities.size());

    structuralChangeVersion.fetch_add(1, std::memory_order_relaxed);
    return entities;
}

template<Component... Components>
std::vector<Entity> World::CreateBatch(size_t count, const Components&... components) {
    auto handles = CreateBatchHandle<Components...>(count, components...);

    std::vector<Entity> entities;
    entities.reserve(count);

    for (EntityHandle handle : handles) {
        entities.push_back(Entity(this, handle));
    }

    return entities;
}

template<typename... Components>
void World::ReserveArchetypeCapacity(size_t count) {
    (AutoComponentRegistrar<Components>::EnsureRegistered(), ...);

    this->CheckRemovalReentry();
    std::unique_lock lock(this->worldMutex);

    ((this->componentSizes.try_emplace(GetComponentTypeId<Components>(), sizeof(Components))), ...);

    ComponentSignature signature;
    (signature.Add(GetComponentTypeId<Components>()), ...);
    Archetype* archetype = this->GetOrCreateArchetype(signature);

    archetype->Reserve(count);
}

} // namespace GameEngine::ECS

#endif // GE_ECS_WORLD_TEMPLATE_IMPLEMENTATIONS_INL
