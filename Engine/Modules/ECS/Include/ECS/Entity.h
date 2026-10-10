#pragma once

#include "ECS/ECS.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/ComponentFlags.h"
#include "ECS/ComponentHook.h"
#include "ECS/Components.h"
#include "Types/ScopedSubscription.h"
#include "ECS/ComponentDirtyFeed.h"
#include "ECS/LifecycleEvents.h"
#include "ECS/AutoRegistration.h"
#include "ECS/QueryPolicy.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <shared_mutex>
#include <span>
#include <tuple>
#include <vector>

namespace GameEngine {
namespace ECS {

// Forward declarations
class World;
class UnresolvedComponentStore; // ECS/UnresolvedComponentStore.h
struct ComponentLayoutChange;   // ECS/ComponentMigration.h
struct FieldByteMove;           // ECS/ComponentMigration.h
struct ComponentHookSubscriptionState;

template<QualifiedComponent... Ts> class Query;

// Command struct moved to ECS.h to avoid circular dependencies

// Dynamic component bundle for runtime entity creation
class ComponentBundle {
private:
    ComponentSignature signature;
    std::unordered_map<ComponentTypeId, std::unique_ptr<uint8_t[]>> componentData;
    std::unordered_map<ComponentTypeId, size_t> componentSizes;

public:
    ComponentBundle() = default;

    // Add a component to the bundle
    template<Component T>
    ComponentBundle& Add(const T& component) {
        // Auto-register the component to ensure it's available
        AutoComponentRegistrar<T>::EnsureRegistered();

        ComponentTypeId typeId = GetComponentTypeId<T>();
        signature.Add(typeId);

        // Store component data
        auto data = std::make_unique<uint8_t[]>(sizeof(T));
        std::memcpy(data.get(), &component, sizeof(T));
        componentData[typeId] = std::move(data);
        componentSizes[typeId] = sizeof(T);

        return *this;
    }

    // Get the component signature
    const ComponentSignature& GetSignature() const { return signature; }

    // Get component data (for internal use by World)
    const void* GetComponentData(ComponentTypeId typeId) const {
        auto it = componentData.find(typeId);
        return (it != componentData.end()) ? it->second.get() : nullptr;
    }

    // Get component size
    size_t GetComponentSize(ComponentTypeId typeId) const {
        auto it = componentSizes.find(typeId);
        return (it != componentSizes.end()) ? it->second : 0;
    }

    // Check if bundle has component
    template<Component T>
    bool Has() const {
        return signature.Contains(GetComponentTypeId<T>());
    }

    // Clear the bundle
    void Clear() {
        signature.Clear();
        componentData.clear();
        componentSizes.clear();
    }
};

// Configuration for World initialization
struct WorldConfig {
    size_t ExpectedEntityCount = 65'536;           // Pre-allocate for this many entities (grows on demand)
    size_t CommandBufferSize = 32768;              // Command buffer size per thread
    bool EnableDebugLogging = false;               // Enable detailed ECS debug logging
    bool EnableBulkOperations = true;              // Enable direct bulk operations mode
    float ArchetypeGrowthFactor = 1.5f;            // Growth factor for archetype storage

    // Archetype management
    bool EnableArchetypePruning = false;           // Enable automatic pruning of empty archetypes
    size_t ArchetypePruningThreshold = 100;        // Prune after this many command processing cycles
};

// SPSC (single-producer, single-consumer) lock-free command ring buffer.
// Each thread gets its own buffer via GetCommandBuffer() thread-local assignment.
// Only one thread pushes; ProcessCommands on the main thread pops all buffers.
class CommandBuffer {
private:
    size_t m_Mask; // capacity - 1; capacity is always a power of two
    std::unique_ptr<Command[]> m_Buffer;
    std::atomic<size_t> m_WritePos{0};
    std::atomic<size_t> m_ReadPos{0};

public:
    // Capacity is rounded up to a power of two so ring index arithmetic is a
    // mask instead of a runtime 64-bit division (~20-30 cycles per push/pop).
    explicit CommandBuffer(size_t size = 32768)
        : m_Mask(std::bit_ceil(std::max<size_t>(size, 2)) - 1),
          m_Buffer(std::make_unique<Command[]>(m_Mask + 1)) {}

    // Single-producer push: write data, then advance the write position (release).
    bool TryPush(Command&& cmd) {
        size_t currentWrite = m_WritePos.load(std::memory_order_relaxed);
        size_t nextWrite = (currentWrite + 1) & m_Mask;

        if (nextWrite == m_ReadPos.load(std::memory_order_acquire)) {
            return false; // Buffer full
        }

        m_Buffer[currentWrite] = std::move(cmd);
        m_WritePos.store(nextWrite, std::memory_order_release);
        return true;
    }

    // Single-consumer pop (only called from ProcessCommands on main thread).
    bool TryPop(Command& cmd) {
        size_t currentRead = m_ReadPos.load(std::memory_order_relaxed);

        if (currentRead == m_WritePos.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        cmd = std::move(m_Buffer[currentRead]);
        m_ReadPos.store((currentRead + 1) & m_Mask, std::memory_order_release);
        return true;
    }

    bool IsEmpty() const {
        return m_ReadPos.load(std::memory_order_acquire) == m_WritePos.load(std::memory_order_acquire);
    }

    size_t GetSize() const {
        size_t write = m_WritePos.load(std::memory_order_acquire);
        size_t read = m_ReadPos.load(std::memory_order_acquire);
        // Positions are already wrapped to [0, capacity); unsigned subtraction
        // plus the mask yields the occupied count across the wrap.
        return (write - read) & m_Mask;
    }

    size_t GetCapacity() const { return m_Mask + 1; }
};

// Entity wrapper for intuitive API
class Entity {
private:
    World* m_World;
    EntityHandle m_Handle;

public:
    Entity(World* w, EntityHandle h) : m_World(w), m_Handle(h) {}

    // Add or update component (PascalCase canonical)
    template<Component T>
    Entity& Set(const T& component);

    // Remove component
    template<Component T>
    Entity& Remove();

    // Get component read-only (returns nullptr if not present). Never
    // stamps — the name split's Entity-side twin (design §4.3).
    template<Component T>
    const T* Get() const;

    // Get component for mutation: a write grant, stamps the owning chunk
    // column (see World::GetComponentForWrite).
    template<Component T>
    T* GetForWrite();

    // Check if entity has component
    template<Component T>
    bool Has() const;

    // Destroy entity
    void Destroy();

    // Clone entity
    Entity Clone() const;

    // Enable state. Queries skip a disabled entity and a disabled component by
    // construction — the row is never visited — while Get/GetForWrite/Has
    // still see both, which is how scene IO and the inspector read them.
    Entity& SetEnabled(bool enabled);
    // This entity's own state, as authored and serialised.
    bool IsEnabled() const;
    // ...and the derived one: false when an ancestor is disabled too.
    bool IsEnabledInHierarchy() const;

    // Switch component T off on this entity without removing its data. A type
    // that declares NotToggleable (ComponentFlags.h) has no switch and does not
    // compile here.
    template<Component T>
    Entity& SetEnabled(bool enabled);

    template<Component T>
    bool IsEnabled() const;

    EntityHandle GetHandle() const { return m_Handle; }
    bool IsValid() const;


};

// Main world class with optimized storage
class World {
private:
    // Archetype performs swap-and-pop defragmentation and must keep per-entity
    // metadata (chunkIndex) consistent. World owns the metadata; Archetype updates
    // it via UpdateEntityLocationUnsafe under World's lock.
    friend class Archetype;

    // Stable identifier for this World instance. Useful for associating
    // renderer views/cameras and other subsystems with a specific ECS world.
    uint64 m_WorldId = 0;

    // Entity storage. freeIndices is strictly LIFO (push_back/pop_back), so a
    // vector beats a deque: no per-block heap allocations during destroy
    // bursts, no double-indirection per access.
    std::vector<EntityMetadata> entityMetadata;
    std::vector<EntityIndex> freeIndices;
    std::atomic<std::size_t> entityCount{0};

    // Archetype storage (sorted-vector signatures, hash-keyed for O(1) lookup)
    std::unordered_map<ComponentSignature, std::unique_ptr<Archetype>, ComponentSignatureHash> archetypes;
    std::unordered_map<ComponentTypeId, std::size_t> componentSizes;

    // Thread-local command buffers: per-thread map keyed by monotonic WorldId.
    // Keying on uint64 WorldId (not raw World*) avoids the ABA hazard where a
    // destroyed World's address is reused by a new World — other threads would
    // otherwise retain a stale CommandBuffer* under the recycled pointer key.
    // Mirrors the same pattern used by CreateHandle's thread_local cache.
    struct ThreadLocalBuffers {
        std::unordered_map<uint64, CommandBuffer*> PerWorldBuffer;
    };
    static thread_local ThreadLocalBuffers tlBuffers;

    std::vector<std::unique_ptr<CommandBuffer>> commandBuffers;

    // Synchronization
    mutable std::shared_mutex worldMutex;

    // Job system integration
    JobSystem::WorkStealingThreadPool* jobSystem = nullptr;

    // Query scheduling policy (configurable)
    QueryPolicy queryPolicy{};

    // Component type registry (no longer needs factory map — ArchetypeTable columns handle storage)

    // Singleton component storage: one value per component type, accessed by type.
    // Type-erased: stores {data, size, destructor} per singleton.
    struct SingletonEntry {
        std::unique_ptr<std::byte[]> Data;
        std::size_t Size = 0;
        void (*Destructor)(void*) = nullptr; // optional typed destructor
    };
    std::unordered_map<ComponentTypeId, SingletonEntry> m_Singletons;

    // Per-component-type lifecycle hooks (type-erased).
    // OnRemove: invoked before component data is destroyed (entity destruction, component removal, world clear).
    // OnAdd: invoked after a component is first added to an entity (not on update/set of existing).
    // OnSet: invoked after a component's value is written (both initial add and subsequent updates).
    //
    // Invoke receives the source entity handle. Callbacks registered via the
    // legacy `void(*)(T&)` API ignore the entity; the entity-aware
    // `void(*)(EntityHandle, T&)` API uses it. Both coexist.
    struct ComponentHook {
        void(*Invoke)(EntityHandle, void* componentPtr, void* userData);
        void* UserData = nullptr;             // per-registration context (e.g. typed callback pointer)
        std::size_t ComponentSize;            // sizeof(T), for batch iteration stride
        uint64 RegistrationId = 0;
    };
    std::unordered_map<ComponentTypeId, ComponentHook> m_OnRemoveHooks;
    std::unordered_map<ComponentTypeId, ComponentHook> m_OnAddHooks;
    std::unordered_map<ComponentTypeId, ComponentHook> m_OnSetHooks;
    uint64 m_NextComponentHookId = 1; // Never reset by Clear(). Zero retires an exhausted counter.
    std::shared_ptr<ComponentHookSubscriptionState> m_HookSubscriptionState;

    // Dense signatures for quick archetype-level checks
    DenseSignature m_RemoveHookSignature;
    DenseSignature m_AddHookSignature;
    DenseSignature m_SetHookSignature;

    // Query cache invalidation tracking
    std::atomic<std::size_t> structuralChangeVersion{0};

    // Change-signaling write-grant counter (ECS/ChangeFilter.h) — the DOTS
    // GlobalSystemVersion analogue. uint64 (design C8): per-access increment
    // rates wrap uint32 in MINUTES; idle gates then silently invert. Compare
    // is plain monotonic `stamp > gate`, and a fresh zero gate means "see
    // everything once" for the process lifetime.
    //
    // Per-World member so every image (Editor.exe, GameEngine.Native.dll)
    // reads/writes the SAME memory — inline globals duplicate per image and
    // froze movers in the Step-0 prototype (M9/R10 dual-image hazard).
    // Relaxed ordering: consumers order through wave joins, not this counter.
    std::atomic<uint64> m_GlobalSystemVersion{1};

    // Per-entity dirty feed (ECS/ComponentDirtyFeed.h, change-signaling §5
    // as amended): one subscribed component type per World, set once at
    // world bootstrap before systems run and immutable afterwards (the
    // m_SetHookSignature shape; P3's lifecycle events reuse the pattern).
    // 0 = disabled — ComponentTypeId is the consteval Hash64 of a type
    // name, so no real component ever hashes to it (Command::componentType
    // relies on the same convention). World member for the same dual-image
    // reason as m_GlobalSystemVersion above.
    ComponentDirtyFeed m_DirtyFeed;
    ComponentTypeId m_DirtyFeedTypeId = 0;

    // Added<T>/Removed<T> lifecycle event buffers (ECS/LifecycleEvents.h,
    // change-signaling §6 P3). Subscription set once at world bootstrap
    // before systems run and immutable afterwards (the m_AddHookSignature
    // shape); the signature is the recording-site gate so unsubscribed
    // types cost one empty binary_search. World member for the same
    // dual-image reason as m_DirtyFeed above.
    LifecycleEventBuffers m_LifecycleEvents;
    DenseSignature m_LifecycleEventSignature;

    // The parent link SetEntityEnabledImmediate reads (SetParentRelation).
    // 0 = no relation, by the same no-component-hashes-to-zero convention.
    ComponentTypeId m_ParentRelationType = 0;
    uint32 m_ParentRelationOffset = 0;

    // Generation counter: incremented only on Clear() or destruction.
    // Used by CreateHandle's thread_local cache to detect world reset/reuse.
    std::size_t m_WorldGeneration = 0;

private:
    // Bulk operations mode
    bool bulkOperationMode = false;
    // When true, ABI structural operations (create/destroy/add/remove component) queue
    // to the command buffer instead of executing immediately. Set by ManagedSystemBridge
    // during managed system tick to prevent archetype moves that would invalidate Span<T>
    // pointers held by IEntitySystem chunk iteration.
    bool m_DeferStructuralChanges = false;
    WorldConfig config;

    // Update an entity's archetype+location without taking locks.
    // Caller must already hold worldMutex.
    void UpdateEntityLocationUnsafe(EntityHandle entity, Archetype* archetype, EntityLocation loc);

    // Scene components whose type was unknown at load time, kept off archetype storage and
    // preserved for round-trip + re-apply (see GetUnresolvedComponents). unique_ptr keeps the
    // store definition out of this header.
    std::unique_ptr<UnresolvedComponentStore> m_UnresolvedComponents;

public:
    explicit World(JobSystem::WorkStealingThreadPool* js = nullptr);
    explicit World(const WorldConfig& cfg, JobSystem::WorkStealingThreadPool* js = nullptr);
    ~World();

    uint64 GetWorldId() const { return m_WorldId; }

    // Side-table of scene components whose type was not registered at load time (e.g. a user
    // native module still compiling). Preserved verbatim so a save round-trips them, and the
    // editor re-applies them once the type registers. Lazily created; main-thread access only.
    UnresolvedComponentStore& GetUnresolvedComponents();
    const UnresolvedComponentStore* TryGetUnresolvedComponents() const { return m_UnresolvedComponents.get(); }

    // Snapshot helpers
    //
    // Enumerate the World's currently-alive entity handles via the canonical metadata array.
    // This acquires a shared read lock on the World and does not observe transient archetype-move
    // states (e.g. during component add/remove operations).
    //
    // Performance notes:
    // - Prefer the out-parameter overload to reuse an existing buffer (avoids allocations + copies).
    // - The return-by-value overload is convenience and may allocate.
    void GetAliveEntitiesSnapshot(std::vector<EntityHandle>& out) const;
    std::vector<EntityHandle> GetAliveEntitiesSnapshot() const;

    // Register component type (records size for ArchetypeTable column layout).
    template<Component T>
    void RegisterComponent() {
        AutoComponentRegistrar<T>::EnsureRegistered();
        ComponentTypeId typeId = GetComponentTypeId<T>();
        componentSizes[typeId] = sizeof(T);
    }

	    // Register multiple component types at once (optional pre-registration)
	    template<Component... Ts>
	    void RegisterComponents() {
	        (RegisterComponent<Ts>(), ...);
	    }

    // Register a per-component removal callback. The callback fires before
    // component data is destroyed (entity destruction, component removal,
    // world clear). Only one hook per component type; last registration wins.
    // Must be called before systems run (e.g. during world setup).
    //
    // OnRemove hooks vs Removed<T> lifecycle events — the deliberate split
    // (§6 P3): hooks are for REMOVAL-WITH-DATA — they receive the live T&
    // pre-destruction, so external resources keyed by component payload
    // (GPU handles, SkeletonStore runtime ids) release here. Removed events
    // (GetRemoved<T>) deliver only the EntityHandle, one frame later, with
    // the component already gone — use them for handle-keyed teardown in
    // frame-scheduled systems. Note the Clear() asymmetry: hooks still fire
    // per-entity on world clear, while events are replaced by the WorldReset
    // signal (GetLifecycleResetGeneration).
    //
    // Entity destruction and Clear treat Remove hooks as notifications, never
    // vetoes: an exception is logged, the remaining hooks still run, and the
    // row is retired exactly once. A hook must release or transfer what it
    // owns without throwing; the ECS cannot recover ownership abandoned inside
    // a callback. Hooks run on the destroying thread under the exclusive world
    // lock: use the supplied live data, not same-world queries or structural
    // mutation. On that thread, destroying the current entity again (or any
    // entity while Clear/destruction is running) is a harmless no-op and every
    // other same-world structural request throws std::logic_error before it
    // would take the lock; another thread simply waits for the lock. Other
    // worlds may be mutated freely. Component-only removal keeps its existing
    // behavior: a throwing hook propagates and the component stays.
    template<Component T>
    ComponentHookToken RegisterOnRemove(void(*callback)(T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Remove, callback);
    }

    // Entity-aware variant. Receives the source entity handle alongside the
    // component. Useful for maintaining side indices (e.g. RelationIndex).
    template<Component T>
    ComponentHookToken RegisterOnRemove(void(*callback)(EntityHandle, T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Remove, callback);
    }

    // Count registrations whose callback OR invocation thunk lies in a module
    // image [base, base + size), once per hook. Includes add/set/remove hooks
    // across this linked ECS copy's live worlds. Main-thread unload boundary:
    // no concurrent hook registration or structural mutation. A nonzero count
    // must keep the image mapped; revocation could discard resource teardown.
    static std::size_t CountComponentHooksOwnedByImage(uint64_t base, uint64_t size);

    // Removes only the matching registration in this World. No callback is invoked.
    // Register, unregister and reset subscriptions at quiescent boundaries: never
    // from a hook, World-owned destructor during Clear, or concurrently with
    // structural changes. Drain owned resources
    // before detaching the callback that releases them.
    bool UnregisterComponentHook(ComponentHookToken token);

    // Scoped alternatives for external owners. Retain the handle for the desired lifetime.
    // Reset/destruction safely becomes a no-op after replacement or World destruction.
    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnAdd(void(*callback)(T&)) {
        return CreateComponentHookSubscription(RegisterOnAdd<T>(callback));
    }

    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnAdd(void(*callback)(EntityHandle, T&)) {
        return CreateComponentHookSubscription(RegisterOnAdd<T>(callback));
    }

    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnSet(void(*callback)(T&)) {
        return CreateComponentHookSubscription(RegisterOnSet<T>(callback));
    }

    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnSet(void(*callback)(EntityHandle, T&)) {
        return CreateComponentHookSubscription(RegisterOnSet<T>(callback));
    }

    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnRemove(void(*callback)(T&)) {
        return CreateComponentHookSubscription(RegisterOnRemove<T>(callback));
    }

    template<Component T>
    [[nodiscard]] ScopedSubscription SubscribeOnRemove(void(*callback)(EntityHandle, T&)) {
        return CreateComponentHookSubscription(RegisterOnRemove<T>(callback));
    }

    // Register a per-component addition callback. Fires after a component is
    // first added to an entity (not on update/set of existing). Only one hook
    // per component type; last registration wins.
    template<Component T>
    ComponentHookToken RegisterOnAdd(void(*callback)(T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Add, callback);
    }

    template<Component T>
    ComponentHookToken RegisterOnAdd(void(*callback)(EntityHandle, T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Add, callback);
    }

    // Register a per-component set callback. Fires after a component's value is
    // written — both on initial add AND on subsequent updates. Only one hook per
    // component type; last registration wins.
    template<Component T>
    ComponentHookToken RegisterOnSet(void(*callback)(T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Set, callback);
    }

    template<Component T>
    ComponentHookToken RegisterOnSet(void(*callback)(EntityHandle, T&)) {
        return RegisterComponentHook<T>(ComponentHookKind::Set, callback);
    }

    // Check whether any OnRemove hook is registered for a given type.
    bool HasOnRemoveHook(ComponentTypeId typeId) const {
        return m_OnRemoveHooks.find(typeId) != m_OnRemoveHooks.end();
    }

    // Access the hook signature (used by Archetype::RefreshRemoveHooks).
    const DenseSignature& GetRemoveHookSignature() const { return m_RemoveHookSignature; }

    // Invoke lifecycle hooks for a single component instance (type-erased).
    // Caller must ensure ptr points to a valid T instance for the given typeId
    // and that `entity` is the owner.
    void InvokeRemoveHook(ComponentTypeId typeId, EntityHandle entity, void* ptr) const {
        auto it = m_OnRemoveHooks.find(typeId);
        if (it != m_OnRemoveHooks.end()) {
            it->second.Invoke(entity, ptr, it->second.UserData);
        }
    }

    void InvokeAddHook(ComponentTypeId typeId, EntityHandle entity, void* ptr) const {
        auto it = m_OnAddHooks.find(typeId);
        if (it != m_OnAddHooks.end()) {
            it->second.Invoke(entity, ptr, it->second.UserData);
        }
    }

    void InvokeSetHook(ComponentTypeId typeId, EntityHandle entity, void* ptr) const {
        auto it = m_OnSetHooks.find(typeId);
        if (it != m_OnSetHooks.end()) {
            it->second.Invoke(entity, ptr, it->second.UserData);
        }
    }

    // Entity creation (PascalCase)
    EntityHandle CreateEntity();
    Entity Create() { return Entity(this, CreateEntity()); }

    // TEMPLATE-BASED CREATION - Create entities with components in one go

    // Create single entity with all components directly in correct archetype
    template<Component... Components>
    Entity Create(const Components&... components);

    // Create single entity handle with all components directly in correct archetype (maximum performance)
    template<Component... Components>
    EntityHandle CreateHandle(const Components&... components);

    // Create entity in specific archetype (no components set)
    template<Component... Components>
    Entity CreateInArchetype();

    // Create entity handle in specific archetype (no components set)
    template<Component... Components>
    EntityHandle CreateInArchetypeHandle();

    // BATCH CREATION APIs - High Performance Entity Creation

    // Batch entity creation - creates multiple entities efficiently
    std::vector<EntityHandle> CreateBatchHandle(size_t count);
    std::vector<Entity> CreateBatch(size_t count);

    // Batch create entities with same component values (returns Entity objects)
    template<Component... Components>
    std::vector<Entity> CreateBatch(size_t count, const Components&... components);

    // Batch create entity handles with same component values (returns EntityHandle objects)
    template<Component... Components>
    std::vector<EntityHandle> CreateBatchHandle(size_t count, const Components&... components);

    // Batch create entities with per-entity initialization via a callback.
    //
    // Calls `initFn(size_t index, Components&... refs)` for each new entity,
    // giving the caller direct references into column storage. No intermediate
    // copies, no temp component instances. The entire batch runs under a
    // single worldMutex unique_lock and uses cached column offsets.
    //
    // Use this when entities share a component set but each needs different
    // values — e.g. procedural scene generation. For uniform values use the
    // const-components overload above.
    //
    // Returns the handles of the created entities in index order.
    template<Component... Components, typename InitFn>
    std::vector<EntityHandle> CreateBatchWithInit(size_t count, InitFn&& initFn);

    // Bulk component assignment - bypasses command system - implementations in WorldTemplates.h
    template<typename T>
    void SetBulkComponents(const std::vector<EntityHandle>& entities,
                          const std::vector<T>& components);

    // Direct operations mode - bypasses command system for bulk operations
    void BeginBulkOperations();
    void EndBulkOperations();

    // Direct entity creation (no command buffering in bulk mode)
    EntityHandle CreateDirect();

    // Direct component operations (no command buffering in bulk mode) - implementations in WorldTemplates.h
    template<typename T>
    void SetComponentDirect(EntityHandle entity, const T& component);

    // Memory pre-allocation
    void ReserveEntities(size_t count);
    template<typename... Components>
    void ReserveArchetypeCapacity(size_t count);

    // Entity destruction (deferred; falls back to immediate when the command
    // buffer is full and structural-change deferral is not active)
    void DestroyEntity(EntityHandle entity);

    // Immediate entity destruction
    void DestroyEntityImmediate(EntityHandle entity);

    // Snapshot and destroy the valid handles in input order. Invalid/stale
    // handles and duplicates are ignored. All ECS retirement capacity is
    // prepared before any OnRemove notification; preparation failure leaves
    // every entity and lifecycle event intact. Notifications cannot veto
    // finalization, as with DestroyEntityImmediate.
    // Uses O(N) transient handle storage. Duplicates may reserve extra capacity.
    // Same-world notification reentry is forbidden, except empty requests or
    // duplicates of the entity currently being removed. Does not walk children.
    void DestroyEntitiesImmediate(std::span<const EntityHandle> entities);

    // Editor/tooling: destroy without releasing the entity index back to the free list.
    // This allows undo/redo flows to revive the same EntityHandle (index+version) later.
    void DestroyEntityImmediatePreserveHandle(EntityHandle entity);
    // Editor/tooling: revive a previously preserved entity handle (same index+version).
    // Returns true on success (or if already alive); false if the handle is incompatible.
    bool ReviveEntityImmediatePreserveHandle(EntityHandle entity);


    // DYNAMIC CREATION - Runtime component specification

    // Create entity from component bundle
    Entity CreateFromBundle(const ComponentBundle& bundle);
    EntityHandle CreateFromBundleHandle(const ComponentBundle& bundle);

    // Create entity from component signature (no components set)
    Entity CreateFromSignature(const ComponentSignature& signature);
    EntityHandle CreateFromSignatureHandle(const ComponentSignature& signature);

    // Batch create entities from bundle
    std::vector<EntityHandle> CreateBatchFromBundle(size_t count, const ComponentBundle& bundle);

    // Component operations (thread-safe, deferred by default) -
    // implementations in WorldTemplateImplementations.inl
    template<Component T>
    void AddComponent(EntityHandle entity, const T& component);

    template<Component T>
    void AddComponentImmediate(EntityHandle entity, const T& component);

    template<Component T>
    void RemoveComponent(EntityHandle entity);

    template<Component T>
    void RemoveComponentImmediate(EntityHandle entity);

    // Read-only component access: never stamps a write grant, so it can be
    // called from per-frame pollers without dirtying change filters. Split by
    // NAME from GetComponentForWrite (design §4.3, C5/M2/M3) — overload
    // constness would silently misclassify the dominant non-const World&
    // call shape.
    template<Component T>
    const T* GetComponent(EntityHandle entity) const;

    // Mutable component access: a write grant. Stamps the requested column
    // of the owning chunk (O(1) via entityMetadata) so Changed<>-filtered
    // consumers see the write. Whole-column false positives by design;
    // false negatives never.
    template<Component T>
    T* GetComponentForWrite(EntityHandle entity);

    // Batched read-only multi-component resolve (TLAS refit S1b, review A1):
    // for each handle, writes one pointer per requested type into `out`
    // (nullptr where the entity is dead or lacks the component) under a
    // SINGLE worldMutex shared-lock hold — vs two lock cycles per
    // GetComponent call. Never stamps (same contract as GetComponent).
    // First use of an unregistered type takes the same one-time unique-lock
    // size-registration upgrade GetComponent does. Pointer lifetime matches
    // GetComponent: valid until the next structural change — callers rely
    // on their own no-structural-writers window (the extraction/refit
    // single-writer-wave assumption).
    template<Component... Ts>
    void GetComponentsBatch(std::span<const EntityHandle> entities,
                            std::span<std::tuple<const Ts*...>> out) const;

    // Check if entity has component
    template<Component T>
    bool HasComponent(EntityHandle entity) const;

    // Runtime (by type id) presence check: signature test only, no serialization.
    // For reflection/editor code that holds a ComponentTypeId rather than a type.
    bool HasComponent(EntityHandle entity, ComponentTypeId typeId) const;

    // Query creation with auto-registration support (PascalCase canonical)
    template<QualifiedComponent... Ts>
    GameEngine::ECS::Query<Ts...> Query() {
        // Auto-register underlying component types on first use (unified approach)
        (AutoComponentRegistrar<Underlying<Ts>>::EnsureRegistered(), ...);

        // Ensure component sizes are registered (needed for archetype table creation).
        {
            bool allRegistered = true;
            {
                std::shared_lock readLock(worldMutex);
                ((allRegistered = allRegistered &&
                    componentSizes.find(GetComponentTypeId<Underlying<Ts>>()) != componentSizes.end()), ...);
            }
            if (!allRegistered)
            {
                std::unique_lock lock(worldMutex);
                ((componentSizes.try_emplace(GetComponentTypeId<Underlying<Ts>>(), sizeof(Underlying<Ts>))), ...);
            }
        }

        return GameEngine::ECS::Query<Ts...>(this);
    }

    // ================================================================
    // Singleton Components — per-world values accessed by type, no entity needed.
    // Useful for global state: camera, time, input, game config, etc.
    // Thread-safe (shares worldMutex).
    // ================================================================

    // Set (or overwrite) a singleton value. Creates it if it doesn't exist.
    // Thread-safe (takes unique_lock only for map insertion on first set).
    template<Component T>
    void SetSingleton(const T& value) {
        AutoComponentRegistrar<T>::EnsureRegistered();
        ComponentTypeId typeId = GetComponentTypeId<T>();
        CheckRemovalReentry();
        std::unique_lock lock(worldMutex);

        auto it = m_Singletons.find(typeId);
        if (it != m_Singletons.end()) {
            std::memcpy(it->second.Data.get(), &value, sizeof(T));
        } else {
            SingletonEntry entry;
            entry.Data = std::make_unique<std::byte[]>(sizeof(T));
            entry.Size = sizeof(T);
            std::memcpy(entry.Data.get(), &value, sizeof(T));
            m_Singletons.emplace(typeId, std::move(entry));
        }
    }

    // Get a singleton value. Returns nullptr if not set.
    // Uses shared_lock to be safe against concurrent SetSingleton (which may rehash the map).
    template<Component T>
    T* GetSingleton() {
        ComponentTypeId typeId = GetComponentTypeId<T>();
        std::shared_lock lock(worldMutex);
        auto it = m_Singletons.find(typeId);
        return (it != m_Singletons.end()) ? reinterpret_cast<T*>(it->second.Data.get()) : nullptr;
    }

    template<Component T>
    const T* GetSingleton() const {
        ComponentTypeId typeId = GetComponentTypeId<T>();
        std::shared_lock lock(worldMutex);
        auto it = m_Singletons.find(typeId);
        return (it != m_Singletons.end()) ? reinterpret_cast<const T*>(it->second.Data.get()) : nullptr;
    }

    // Check if a singleton exists.
    template<Component T>
    bool HasSingleton() const {
        ComponentTypeId typeId = GetComponentTypeId<T>();
        std::shared_lock lock(worldMutex);
        return m_Singletons.contains(typeId);
    }

    // Remove a singleton.
    template<Component T>
    void RemoveSingleton() {
        ComponentTypeId typeId = GetComponentTypeId<T>();
        CheckRemovalReentry();
        std::unique_lock lock(worldMutex);
        m_Singletons.erase(typeId);
    }





    // Process deferred commands

	    // World public API (PascalCase canonical)
	    // ProcessCommands executes at most this many deferred commands per call;
	    // any remainder stays queued and is drained by subsequent calls.
	    static constexpr std::size_t kMaxCommandsPerBatch = 100'000;
	    void ProcessCommands();
	    std::size_t GetEntityCount() const { return entityCount.load(); }
	    // Exclusive upper bound on live entity INDEXES (entityMetadata size —
	    // grows monotonically as handles are minted; recycled indexes stay
	    // under it). Consumers sizing index-keyed scratch (e.g. the SceneTlas
	    // refit dedup stamps) use this instead of the 2^20 handle-space bound.
	    std::size_t GetEntityIndexBound() const
	    {
	        std::shared_lock lock(worldMutex);
	        return entityMetadata.size();
	    }
	    std::size_t GetArchetypeCount() const;
	    std::vector<Archetype*> GetAllArchetypes() const;
	    // Fills `out` with every archetype the filter matches: contains all of
	    // Required and Include, shares no component with Exclude, and carries
	    // none of DisabledExclude except the tags DisabledInclude names back in.
	    // A null member is no filter at all. `out` is cleared and reserved once,
	    // so a buffer reused across calls stops allocating. Queries iterate this
	    // copy rather than the archetype map: an archetype created while a query
	    // iterates cannot invalidate it.
	    void CollectMatchingArchetypes(const ArchetypeFilter& filter,
	                                   std::vector<Archetype*>& out) const;
	    void SetJobSystem(JobSystem::WorkStealingThreadPool* js) { jobSystem = js; }
	    JobSystem::WorkStealingThreadPool* GetJobSystem() const { return jobSystem; }
            void SetQueryPolicy(const QueryPolicy& p) { queryPolicy = p; }
            const QueryPolicy& GetQueryPolicy() const { return queryPolicy; }

	    bool IsValid(EntityHandle entity) const;
	    Archetype* GetEntityArchetype(EntityHandle entity) const;
	    void CopyComponent(EntityHandle src, EntityHandle dst, ComponentTypeId typeId);
	    EntityHandle CloneEntity(EntityHandle source);
	    // In-memory snapshots preserve entity handles, including undo-reserved dead
	    // slots. Restore still resets the World's lifecycle generation via Clear(),
	    // and leaves the World untouched for any stream that is not a snapshot of
	    // the current format.
	    std::vector<uint8_t> SerializeWorld();
	    void DeserializeWorld(const std::vector<uint8_t>& data);
            void Clear();

            // ----------------------------------------------------------------
            // Type-erased component snapshot helpers (Editor undo/redo, tooling)
            // ----------------------------------------------------------------
            //
            // These helpers allow editor systems to capture/apply component state
            // without relying on stable component pointers (archetype migrations
            // can move component storage).
            //
            // - CaptureComponentBytes serializes the component instance for the
            //   given entity/type into outBytes. Returns false if the entity is
            //   invalid, the component is missing, or no handler exists.
            // - ApplyComponentBytesImmediate applies the provided bytes using the
            //   registered component handler and executes immediately (not via
            //   deferred command buffers).
            bool CaptureComponentBytes(EntityHandle entity,
                                       ComponentTypeId typeId,
                                       std::vector<uint8_t>& outBytes) const;
            // Allocation-free variant for per-frame readers (scripting ABI): copies
            // min(destCap, componentSize) bytes straight from chunk storage into dest
            // and reports the full component size in outSize. dest may be null with
            // destCap 0 to query the size. Returns false if the entity is invalid,
            // the component is missing, or no handler exists.
            bool ReadComponentBytes(EntityHandle entity,
                                    ComponentTypeId typeId,
                                    void* dest,
                                    std::size_t destCap,
                                    std::size_t& outSize) const;
            bool ApplyComponentBytesImmediate(EntityHandle entity,
                                             ComponentTypeId typeId,
                                             const std::vector<uint8_t>& bytes);

            // ----------------------------------------------------------------
            // Type-erased structural component operations (runtime components)
            // ----------------------------------------------------------------
            // These perform archetype moves using the ComponentRegistry handler table.
            // Intended for scripting and tooling where component types are identified
            // by ComponentTypeId rather than templates.
            bool SetComponentBytesImmediate(EntityHandle entity,
                                            ComponentTypeId typeId,
                                            const void* data,
                                            std::size_t sizeBytes);
            bool RemoveComponentByTypeIdImmediate(EntityHandle entity, ComponentTypeId typeId);

            // Migrate a component whose byte layout changed across a hot-reload: resize the
            // recorded size + every archetype column that holds it, re-packing each existing
            // instance (preserve same-named fields by name+type+size, default new fields).
            // Safe to call between frames; takes worldMutex exclusively and bumps the
            // structural-change version so query caches and baked move-plans rebuild.
            // Returns true when this world held storage for the component and re-packed it.
            bool MigrateComponentLayout(const ComponentLayoutChange& change);

            // Reload entry point: apply the layout change to EVERY live world (the
            // process-wide live-world registry) so no world keeps an old stride while the
            // process-global handler reports the new size — plus `target` when it is not
            // visible in this linked copy's registry (the scripting ABI's statically-linked
            // ECS operating on a host world). Main-thread, like module loads. Returns true
            // when any world re-packed instances.
            static bool MigrateComponentLayoutAcrossWorlds(World& target,
                                                           const ComponentLayoutChange& change);


    // ARCHETYPE MANAGEMENT

    // Prune empty archetypes (optional optimization)
    size_t PruneEmptyArchetypes();

    // Compact empty chunks across all archetypes. Frees memory and updates entity metadata.
    // Returns total chunks freed. Safe to call periodically (e.g. after ProcessCommands).
    size_t CompactAllChunks();

    // Configure archetype pruning
    void SetArchetypePruningEnabled(bool enabled) { config.EnableArchetypePruning = enabled; }
    void SetArchetypePruningThreshold(size_t threshold) { config.ArchetypePruningThreshold = threshold; }

    // Access component size registry (public for ArchetypeTable initialization).
    const std::unordered_map<ComponentTypeId, std::size_t>& GetComponentSizes() const { return componentSizes; }

    // Query cache invalidation tracking (public for scripting ABI utilities)
    std::size_t GetStructuralChangeVersion() const { return structuralChangeVersion.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------
    // Change-signaling write-grant versions (ECS/ChangeFilter.h).
    // ------------------------------------------------------------------

    // Entry sample for consumer gates: write THIS value back as the next
    // gate, never a fresh end-of-run sample (design M14). Also serves as
    // per-world stamp telemetry — the counter increments once per write
    // grant, so its delta over a frame is the frame's grant count.
    uint64 GetGlobalSystemVersion() const
    {
        return m_GlobalSystemVersion.load(std::memory_order_relaxed);
    }

    // Increment-before-stamp (design §4.4): a write between two gated runs
    // always stamps strictly greater than the earlier run's entry sample.
    // One increment per grant (query dispatch or bypass call/batch), not per
    // entity write.
    uint64 NextGlobalSystemVersion()
    {
        return m_GlobalSystemVersion.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // Test hook for StaleAndFreshGatesSurviveCounterChurn (design C8): jump
    // the counter far forward (e.g. past 2^32) without 4 billion writes.
    // Not for production use — gates must only ever be written from entry
    // samples.
    void DebugAdvanceGlobalSystemVersion(uint64 delta)
    {
        m_GlobalSystemVersion.fetch_add(delta, std::memory_order_relaxed);
    }

    // World-wide column-stamp telemetry (design §4.6 IdleEditorStampCount):
    // total write-grant stamps taken across all archetype tables. The editor
    // stats surface exposes it so an idle frame's delta can be asserted ~0 —
    // the gate that catches read-path launderers. Never used for correctness.
    uint64 GetColumnStampCount() const;

    // Single-entity point probe of the per-(chunk, column) write-grant
    // version for `typeId` — for consumers that watch ONE entity (e.g. the
    // Inspector's live-value gate) where a Changed<> query scan is the wrong
    // shape. Chunk-granular: co-located entities share the version, so a
    // moved gate can over-report, never miss. Returns 0 when the entity is
    // invalid or lacks the component. Pair with an entry-sampled
    // GetGlobalSystemVersion() per the ChangeGate contract.
    uint64 GetEntityColumnVersion(EntityHandle entity, ComponentTypeId typeId) const;

    // Record write grants for a batch of entities' `typeId` column — the same
    // stamp GetComponentForWrite and a Write<T> query visit issue, for writers
    // that mutate through pointers cached OUTSIDE the granting call (today:
    // the transform hierarchy's node cache, whose WorldTransform pointers are
    // captured once at topology rebuild). Without it the write is invisible to
    // every Changed<T> consumer: stamping is what Changed<T> filters on, and
    // the mutated bytes themselves carry no signal.
    //
    // Batched deliberately — one lock and one version for the whole pass:
    // never slower than per-entity stamping, and entities moved together get
    // single-event semantics under one version instead of N. Consecutive
    // entities in the same chunk are stamped once; the saving depends only on
    // that adjacency, so a span that keeps chunk neighbours together (entity
    // index order, as extraction's patch lane uses) pays about one stamp per
    // chunk run.
    void StampComponentWriteBatch(const EntityHandle* entities, std::size_t count,
                                  ComponentTypeId typeId);

    // ------------------------------------------------------------------
    // Per-entity component dirty feed (ECS/ComponentDirtyFeed.h, §5 P2).
    // ------------------------------------------------------------------

    // Subscribe the feed to ONE component type (WorldTransform in P2). Call
    // at world bootstrap, before systems run; immutable afterwards — the
    // emit sites read the subscription without synchronization.
    void EnableComponentDirtyFeed(ComponentTypeId typeId) { m_DirtyFeedTypeId = typeId; }

    // True when the feed is subscribed to typeId. Producers that stage
    // batches (e.g. the hierarchy flat path) check this once up front to
    // skip collection entirely on unsubscribed worlds (thumbnail worlds).
    bool IsComponentDirtyFeedEnabledFor(ComponentTypeId typeId) const
    {
        return typeId != 0 && typeId == m_DirtyFeedTypeId;
    }

    // Emission: one predictable compare on unsubscribed types (A5). Callable
    // from any thread; the feed synchronizes internally.
    void EmitComponentDirty(ComponentTypeId typeId, EntityHandle entity)
    {
        if (typeId == m_DirtyFeedTypeId)
            m_DirtyFeed.Append(entity);
    }
    void EmitComponentDirtyBatch(ComponentTypeId typeId, const EntityHandle* entities, std::size_t count)
    {
        if (typeId == m_DirtyFeedTypeId)
            m_DirtyFeed.AppendBatch(entities, count);
    }

    // Consumers Snapshot() at their own wave position (§5.1 M5).
    ComponentDirtyFeed& GetComponentDirtyFeed() { return m_DirtyFeed; }
    const ComponentDirtyFeed& GetComponentDirtyFeed() const { return m_DirtyFeed; }

    // The frame-boundary swap. Owned by the engine tick — called exactly
    // once per frame next to the deferred-command flush, never from
    // ProcessCommands (C6: that has mid-frame call sites which would
    // destroy entries before late consumers run). Non-engine-driven worlds
    // that want a consumer must drive their own swap.
    void SwapComponentDirtyFeed() { m_DirtyFeed.Swap(); }

    // ------------------------------------------------------------------
    // Added<T>/Removed<T> lifecycle events (ECS/LifecycleEvents.h, §6 P3).
    // ------------------------------------------------------------------
    //
    // The setup/teardown-split pattern these exist for:
    //
    //   for (EntityHandle e : world.GetAdded<MeshRenderer>())   { /* init */ }
    //   world.Query<Read<WorldTransform>, Read<MeshRenderer>>().Each(...);
    //   for (EntityHandle e : world.GetRemoved<MeshRenderer>()) { /* teardown */ }
    //
    // Removed carries no data (the component is destroyed, the entity may be
    // dead — the handle is an ID, never dereference it). Teardown that needs
    // the component's payload (T&) stays on RegisterOnRemove hooks; the two
    // mechanisms deliberately coexist (see the hook registration docs above).

    // Subscribe a component type for lifecycle events. Call at world
    // bootstrap, before systems run; immutable afterwards — recording
    // sites read the subscription without synchronization.
    void EnableLifecycleEvents(ComponentTypeId typeId)
    {
        CheckRemovalReentry();
        m_LifecycleEvents.Register(typeId);
        m_LifecycleEventSignature.Add(typeId);
    }

    // The typed form also subscribes GetDisabled<T>(), which is built from the
    // lifecycle windows of T's tag and the two entity tags.
    template<Component T>
    void EnableLifecycleEvents()
    {
        AutoComponentRegistrar<T>::EnsureRegistered();
        EnableLifecycleEvents(GetComponentTypeId<T>());
        if constexpr (!kIsEnableStateTag<T>)
        {
            AutoComponentRegistrar<ComponentDisabled<T>>::EnsureRegistered();
            EnableLifecycleEvents(GetComponentTypeId<ComponentDisabled<T>>());
            EnableLifecycleEvents(GetComponentTypeId<Disabled>());
            EnableLifecycleEvents(GetComponentTypeId<DisabledInHierarchy>());
            m_LifecycleEvents.RegisterDisabledEvents(GetComponentTypeId<T>(),
                                                     GetComponentTypeId<ComponentDisabled<T>>());
        }
    }

    // True when lifecycle events are subscribed for typeId. Consumers whose
    // incremental paths are load-bearing on Removed<T> (e.g. the editor
    // Hierarchy panel's parent-remove detection) probe this on their bound
    // world and fall back to full rescans on unsubscribed worlds, where the
    // event spans are silently empty. Registration is bootstrap-only, so
    // the unsynchronized read mirrors the recording sites (dirty-feed
    // IsComponentDirtyFeedEnabledFor precedent).
    bool IsLifecycleEventsEnabledFor(ComponentTypeId typeId) const
    {
        return m_LifecycleEvents.IsRegistered(typeId);
    }

    // Consumer reads: the CURRENT window only — each event is delivered for
    // exactly one SWAP WINDOW (the window after its swap), then discarded.
    // Per-WINDOW, not per-read: a consumer stepped more than once between
    // swaps (editor passive-refresh frames run the waves again) sees the
    // same spans each run and must be idempotent within a window. Events
    // have no poll fallback: a consumer that skips a window misses its
    // events permanently (the dirty-feed F1 invariant, inherited) — guard
    // with GetLifecycleSwapGeneration(): a gap > 1 since the generation the
    // consumer last consumed means unseen windows were discarded, and its
    // per-entity bookkeeping must be re-scanned once before resuming.
    // Empty span for unsubscribed types. Main-thread / schedule-wave reads
    // only; the span is invalidated by the next SwapLifecycleEvents().
    std::span<const EntityHandle> GetAdded(ComponentTypeId typeId) const
    {
        return m_LifecycleEvents.CurrentAdded(typeId);
    }

    std::span<const EntityHandle> GetRemoved(ComponentTypeId typeId) const
    {
        return m_LifecycleEvents.CurrentRemoved(typeId);
    }

    template<Component T>
    std::span<const EntityHandle> GetAdded() const
    {
        return GetAdded(GetComponentTypeId<T>());
    }

    template<Component T>
    std::span<const EntityHandle> GetRemoved() const
    {
        return GetRemoved(GetComponentTypeId<T>());
    }

    // Entities whose T stopped taking part in queries without being removed:
    // T's own ComponentDisabled<T> arrived, or the entity became inactive
    // (Disabled, or DisabledInHierarchy through an ancestor). The component is
    // still on the entity, so a consumer can read it and release what it owns
    // — a physics body, an audio voice. Re-enabling needs no event: the row is
    // visited by queries again and is treated like any new one.
    //
    // Only entities that are still off and still carry T at the window's swap
    // are reported, each once per window. The report can include an entity
    // whose T was already off (created disabled, or a second tag arriving), so
    // a consumer's teardown must tolerate one it never set up or already tore
    // down. Same window contract as GetAdded/GetRemoved; subscribed by
    // EnableLifecycleEvents<T>().
    template<Component T>
    std::span<const EntityHandle> GetDisabled() const
    {
        return m_LifecycleEvents.CurrentDisabled(GetComponentTypeId<T>());
    }

    // The lifecycle-event frame boundary. Owned by the engine tick — called
    // exactly once per frame next to the deferred-command flush and the
    // dirty-feed swap, NEVER from ProcessCommands (C6: 14 call sites,
    // several mid-frame — the managed tick alone would swap-and-clear
    // events 2-4x per editor frame, destroying them before late systems
    // run). Non-engine-driven worlds (thumbnail worlds, tests) drive their
    // own swap explicitly; a world nobody swaps accumulates pending events
    // — bounded by subscription, but stale.
    void SwapLifecycleEvents()
    {
        CheckRemovalReentry();
        std::unique_lock lock(worldMutex);
        m_LifecycleEvents.Swap();
        BuildDisabledEvents();
    }

    // ------------------------------------------------------------------
    // Enable state.
    // ------------------------------------------------------------------

    // Turn an entity off or on. Off adds Disabled and DisabledInHierarchy in
    // ONE archetype move. On removes Disabled, and DisabledInHierarchy with it
    // unless the parent named through SetParentRelation still carries either
    // tag: the entity then stays out of every query until the hierarchy pass
    // re-derives it. The lookup trusts the parent's tags as they stand: a stale
    // tag on the parent keeps the entity out one pass longer, and an ancestor
    // above the parent that was switched off since the last pass reaches the
    // entity with that pass, as it reaches the parent. With no relation set
    // both tags go. Returns true when the state changed.
    bool SetEntityEnabledImmediate(EntityHandle entity, bool enabled);

    // Name the component that links an entity to its parent: its type id and
    // the byte offset of the EntityHandle inside it. The type must already be
    // registered and the handle must lie inside it; anything else asserts and
    // is refused. The ECS has no notion of a hierarchy; the system that derives
    // DisabledInHierarchy from the parent chain sets this so
    // SetEntityEnabledImmediate can keep the derived tag. The relation belongs
    // to this world and outlives its setter: whoever sets it keeps deriving
    // DisabledInHierarchy on this world, or a tag kept here is never cleared.
    void SetParentRelation(ComponentTypeId parentType, uint32 handleOffset);

    // Switch a component off or on by type id — the form the scripting ABI and
    // the editor use; Entity::SetEnabled<T> is the typed one and writes the same
    // tag. Returns true when the state changed; false for an invalid entity or a
    // component type the registry does not know.
    bool SetComponentEnabledImmediate(EntityHandle entity, ComponentTypeId componentType, bool enabled);
    // False when the component's ComponentDisabled tag is on the entity. Entity
    // activity is separate (IsEnabled / IsEnabledInHierarchy on Entity).
    bool IsComponentEnabled(EntityHandle entity, ComponentTypeId componentType) const;

    // WorldReset signal (Q4): Clear() wipes both event windows instead of
    // bursting per-entity Removed events. Consumers cache this value and, on
    // change, drop ALL per-entity bookkeeping (the handler every scene-load
    // consumer needs regardless).
    uint64 GetLifecycleResetGeneration() const { return m_LifecycleEvents.ResetGeneration(); }

    // Swap generation — the consumer-cadence guard (F1 pattern). Increments
    // once per SwapLifecycleEvents(). A consumer caches the generation it
    // last consumed; on observing a gap > 1 (it was disabled or throttled
    // across at least one whole window — e.g. the editor's play-mode pause
    // disables gameplay systems via SetRenderingSystemEnabled while the
    // engine tick keeps swapping), it must run a one-shot full re-scan of
    // its per-entity bookkeeping before consuming the current window.
    uint64 GetLifecycleSwapGeneration() const { return m_LifecycleEvents.SwapGeneration(); }

    // Structural change deferral for managed system ticks.
    // When enabled, ABI callers should route operations through the command buffer.
    void SetDeferStructuralChanges(bool defer) { m_DeferStructuralChanges = defer; }
    bool GetDeferStructuralChanges() const { return m_DeferStructuralChanges; }

    // Push a command directly into the thread-local command buffer.
    // Returns true on success, false if the buffer is full.
    // Unlike DestroyEntity, this never falls back to immediate execution.
    bool TryPushCommand(Command&& cmd)
    {
        return GetCommandBuffer().TryPush(std::move(cmd));
    }


    /// Shared C++ component operations for typed calls and registered handlers.
    /// The byte span must match the registered component type and size.
    void AddComponentImpl(EntityHandle entity, ComponentTypeId typeId,
                          const void* data, std::size_t sizeBytes);
    /// Removing an absent C++ component still advances the structural version.
    void RemoveComponentImpl(EntityHandle entity, ComponentTypeId typeId);

    template<typename T>
    void AddComponentImpl(EntityHandle entity, const T& component);

    template<typename T>
    void RemoveComponentImpl(EntityHandle entity);

    // Get or create archetype. Public so WorldBuilder (and tests) can pre-reserve
    // archetypes during setup. Used to be the public face of GetOrCreateArchetypeDense
    // before Phase 1b consolidated the signature types.
    Archetype* GetOrCreateArchetype(const ComponentSignature& signature);

private:
    // Shared lookup behind GetComponent (const, Stamp=false) and
    // GetComponentForWrite (Stamp=true). Stamp=true records the write grant
    // for the requested column under the same shared lock; the const path
    // instantiates Stamp=false so reads can never launder into a stamp
    // (design C1/C5).
    template<Component T, bool Stamp>
    T* GetComponentLookup(EntityHandle entity);

    // ----------------------------------------------------------------
    // Shared structural-mutation bodies. Each operation class (destroy,
    // add-or-set, remove) funnels through exactly one body so future
    // change recording has a single site per class. All three require
    // the caller to hold worldMutex exclusively.
    //
    // World::Clear()/~World stay on the bulk teardown path
    // (InvokeAllRemoveHooks) — a world reset, not a per-entity destroy
    // burst. Creation-family bodies (CreateHandle/CreateBatch*/
    // CloneEntity/CreateFromSignatureHandle/CreateFromBundleHandle/
    // CreateBatchFromBundle) remain separate per-shape writers by
    // design; they are enumerated as individual rows in the
    // change-signaling stamping matrix rather than funneled.
    // ----------------------------------------------------------------

    // Unified destroy body behind DestroyEntityImmediate,
    // DestroyEntityImmediatePreserveHandle, and deferred DESTROY_ENTITY
    // playback. preserveHandle keeps the entity index out of freeIndices
    // so undo/redo can revive the same EntityHandle (index+version).
    void DestroyEntityInternal(EntityHandle entity, bool preserveHandle);
    // Caller holds worldMutex, has validated the handle, and has prepared all
    // free-index/lifecycle appends. Shared by singular and batch destruction.
    void DestroyPreparedEntityInternal(EntityHandle entity, bool preserveHandle);
    // Removal-notification scope of the calling thread (World.cpp keeps it in
    // thread-local storage; every product binary reaches the one ECS copy inside
    // Engine.dll, so a per-image thread_local is per-process in practice).
    void CheckRemovalReentry() const;
    bool IsRemovingOnThisThread() const;
    bool IsDuplicateRemoval(EntityHandle entity) const;

    // Unified add-or-set body behind typed AddComponentImpl<T>, type-erased
    // SetComponentBytesImmediate, and (via the component handlers) deferred
    // ADD_COMPONENT/ADD_REQUIRED_COMPONENT/SET_COMPONENT playback. The fast
    // path is a data-only SET; the structural branch is an ADD — that
    // classification happens only here.
    // Components are trivially copyable (Component concept), so the raw byte
    // write is equivalent to typed assignment. Returns false if the entity is
    // invalid.
    bool AddOrSetComponentBytesInternal(EntityHandle entity, ComponentTypeId typeId,
                                        const void* data, std::size_t sizeBytes);

    // Shared removal behind C++ operations and runtime blob removal. The tri-state
    // result lets C++ operations invalidate queries even when the component is
    // absent, while blob removal reports absence without a structural change.
    enum class RemoveComponentResult { Removed, ComponentAbsent, InvalidEntity };
    RemoveComponentResult RemoveComponentInternal(EntityHandle entity, ComponentTypeId typeId);

    // ----------------------------------------------------------------
    // Lifecycle-event recording (§6.1). Caller holds worldMutex exclusive
    // (every producer site is a unified structural body or a creation-family
    // body that requires it). Single-type helpers gate on the subscription
    // signature — one binary_search over a near-always-empty vector when
    // nothing subscribes (zero-cost-unsubscribed contract).
    // ----------------------------------------------------------------

    void RecordComponentAdded(ComponentTypeId typeId, EntityHandle entity)
    {
        if (m_LifecycleEventSignature.Contains(typeId))
            m_LifecycleEvents.AppendAdded(typeId, entity);
    }

    void RecordComponentRemoved(ComponentTypeId typeId, EntityHandle entity)
    {
        if (m_LifecycleEventSignature.Contains(typeId))
            m_LifecycleEvents.AppendRemoved(typeId, entity);
    }

    // Signature-shaped recording for the creation/clone/destroy family: one
    // Added/Removed per SUBSCRIBED type present in the archetype signature.
    // Iterates the subscription list (a handful of types) against the
    // signature, not the reverse — an unsubscribed world runs an empty loop.
    void RecordAddedForSignature(const ComponentSignature& sig, EntityHandle entity);
    void RecordAddedForSignatureBatch(const ComponentSignature& sig,
                                      const EntityHandle* entities, std::size_t count);
    void RecordRemovedForSignature(const ComponentSignature& sig, EntityHandle entity);

    // Fills each GetDisabled<T> window from the tag windows just promoted.
    // Caller holds worldMutex exclusive (SwapLifecycleEvents).
    void BuildDisabledEvents();

    // Move entity between archetypes
    void MoveEntity(EntityHandle entity, Archetype* from, Archetype* to);

    // Rebuild one archetype's table at the migrated component's new size and re-pack every
    // instance into it (preserve fields per `remap`, default the rest). Caller holds worldMutex.
    void MigrateArchetypeInstances(Archetype& arch, ComponentTypeId id,
                                   const ComponentLayoutChange& change,
                                   const std::vector<FieldByteMove>& remap);

    // Create entity without acquiring mutex (for internal use when mutex is already held)
    EntityHandle CreateEntityUnsafe();

    // Get thread-local command buffer
    CommandBuffer& GetCommandBuffer();

    // Archetype graph edge resolution: given a source archetype and a component to add/remove,
    // return the target archetype, creating it if needed. Caches the edge for O(1) future lookups.
    Archetype* GetOrCreateArchetypeForAdd(Archetype* source, ComponentTypeId typeId);
    Archetype* GetOrCreateArchetypeForRemove(Archetype* source, ComponentTypeId typeId);
    // The same resolution for the entity-activity pair, which moves as one:
    // `disable` adds both tags, otherwise both are removed. Caches on the
    // archetype's own activity edge so a steady-state toggle allocates nothing.
    Archetype* GetOrCreateArchetypeForActivity(Archetype* source, bool disable);

    // Hook dispatch for the activity pair, which does not pass through the
    // unified add/remove bodies. Caller holds worldMutex exclusively.
    void InvokeActivityAddHooks(EntityHandle entity, Archetype* archetype,
                                const EntityMetadata& metadata, ComponentTypeId typeId);
    void InvokeActivityRemoveHooks(EntityHandle entity, Archetype* archetype,
                                   const EntityMetadata& metadata, ComponentTypeId typeId);
    // True when a parent relation is set, the entity carries its component,
    // and the parent it names carries Disabled or DisabledInHierarchy. Caller
    // holds worldMutex.
    bool IsParentInactive(const Archetype* archetype, const EntityMetadata& metadata) const;

    // Per-entity serialization/JSON plumbing for the public
    // SerializeWorld/DeserializeWorld pair (and the debug JSON dump).
    std::vector<uint8_t> SerializeEntity(EntityHandle entity);
    EntityHandle DeserializeEntity(std::span<const uint8_t> data, EntityHandle entity);
    std::string GetEntityAsJson(EntityHandle entity);
    std::string GetWorldAsJson();

    // Lock-free internal variants of serialization/JSON methods.
    // Caller must already hold worldMutex (shared or unique).
    std::vector<uint8_t> SerializeEntityUnsafe(EntityHandle entity) const;
    std::string GetEntityAsJsonUnsafe(EntityHandle entity) const;

    ComponentHookToken InstallComponentHook(ComponentHookKind kind, ComponentTypeId typeId, ComponentHook hook);
    ScopedSubscription CreateComponentHookSubscription(ComponentHookToken token);
    static void UnregisterLiveComponentHook(ComponentHookToken token);
    struct ComponentHookStorage {
        std::unordered_map<ComponentTypeId, ComponentHook>& Hooks;
        DenseSignature& Signature;
    };
    ComponentHookStorage GetComponentHookStorage(ComponentHookKind kind);

    template<Component T>
    ComponentHookToken RegisterComponentHook(ComponentHookKind kind, void(*callback)(T&)) {
        if (!callback)
            return {};
        AutoComponentRegistrar<T>::EnsureRegistered();
        static_assert(sizeof(void*) >= sizeof(callback), "void* must be able to hold a function pointer");
        ComponentHook hook{};
        hook.UserData = reinterpret_cast<void*>(callback);
        hook.Invoke = [](EntityHandle, void* ptr, void* data) {
            reinterpret_cast<void(*)(T&)>(data)(*static_cast<T*>(ptr));
        };
        hook.ComponentSize = sizeof(T);
        return InstallComponentHook(kind, GetComponentTypeId<T>(), hook);
    }

    template<Component T>
    ComponentHookToken RegisterComponentHook(ComponentHookKind kind, void(*callback)(EntityHandle, T&)) {
        if (!callback)
            return {};
        AutoComponentRegistrar<T>::EnsureRegistered();
        static_assert(sizeof(void*) >= sizeof(callback), "void* must be able to hold a function pointer");
        ComponentHook hook{};
        hook.UserData = reinterpret_cast<void*>(callback);
        hook.Invoke = [](EntityHandle entity, void* ptr, void* data) {
            reinterpret_cast<void(*)(EntityHandle, T&)>(data)(entity, *static_cast<T*>(ptr));
        };
        hook.ComponentSize = sizeof(T);
        return InstallComponentHook(kind, GetComponentTypeId<T>(), hook);
    }

    // Refresh existing archetype flags after registration changes; caller holds worldMutex.
    void RefreshArchetypeRemoveHooks();

    // Invoke OnRemove hooks for all components in an archetype for a specific entity location.
    // Caller must hold worldMutex. The entity at the given location is about to be destroyed.
    void InvokeRemoveHooksForEntity(Archetype* archetype, EntityLocation loc, EntityHandle entity);

    // Invoke OnRemove hooks for all entities in all archetypes (used by Clear()).
    // Caller must hold worldMutex.
    void InvokeAllRemoveHooks();

    friend class Entity;
    friend class Archetype;
    template<QualifiedComponent... Ts> friend class Query;
};

// Template method implementations - placed after World class definition
template<Component T>
Entity& Entity::Set(const T& component) {
    // Auto-register ALL components on first use (unified approach)
    AutoComponentRegistrar<T>::EnsureRegistered();
    m_World->AddComponent(m_Handle, component);
    return *this;
}

template<Component T>
Entity& Entity::Remove() {
    // Auto-register ALL components on first use (unified approach)
    AutoComponentRegistrar<T>::EnsureRegistered();
    m_World->RemoveComponent<T>(m_Handle);
    return *this;
}

template<Component T>
T* Entity::GetForWrite() {
    // Auto-register ALL components on first use (unified approach)
    AutoComponentRegistrar<T>::EnsureRegistered();
    return m_World->GetComponentForWrite<T>(m_Handle);
}

template<Component T>
const T* Entity::Get() const {
    // Auto-register ALL components on first use (unified approach)
    AutoComponentRegistrar<T>::EnsureRegistered();
    // Routed through the const World overload so a const Entity read never
    // launders into a write-grant stamp (design C1, pinned by
    // ConstAccessorsDoNotStamp).
    return static_cast<const World*>(m_World)->GetComponent<T>(m_Handle);
}

template<Component T>
bool Entity::Has() const {
    // Auto-register ALL components on first use (unified approach)
    AutoComponentRegistrar<T>::EnsureRegistered();
    // Presence checks are reads — const path, never stamps (design C1).
    return static_cast<const World*>(m_World)->GetComponent<T>(m_Handle) != nullptr;
}

template<Component T>
Entity& Entity::SetEnabled(bool enabled) {
    static_assert(!HasAnyFlag(ComponentFlagsOf<T>(), ComponentFlags::NotToggleable),
                  "This component declares NotToggleable: it has no on/off state");
    // Idempotent: a redundant call must not bump the structural version and
    // invalidate every query cache in the world.
    const bool disabled = Has<ComponentDisabled<T>>();
    if (enabled && disabled)
        m_World->RemoveComponentImmediate<ComponentDisabled<T>>(m_Handle);
    else if (!enabled && !disabled)
        m_World->AddComponentImmediate<ComponentDisabled<T>>(m_Handle, ComponentDisabled<T>{});
    return *this;
}

template<Component T>
bool Entity::IsEnabled() const {
    return !Has<ComponentDisabled<T>>();
}

// World template implementations moved to World.h (after Archetype class definition)

} // namespace ECS
} // namespace GameEngine
