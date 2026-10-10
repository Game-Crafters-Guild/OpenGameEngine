#pragma once

#include "ECS/ComponentTypeName.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/TaskTypes.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Types.h"

#include <algorithm>
#include <cassert>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef ECS_SIMD_ENABLED
#ifdef ECS_SIMD_ARM64
#include <arm_neon.h>
#elif defined(ECS_SIMD_X64)
#include <immintrin.h>
#endif
#endif

namespace GameEngine
{

// Use fully qualified JobSystem types to avoid conflicts

namespace ECS
{

// Forward declarations
class World;
class Archetype;
template <typename T>
class ComponentHandle;

// Concepts for component validation
template <typename T>
concept Component = std::is_trivially_copyable_v<T> &&
                    std::is_standard_layout_v<T> &&
                    !std::is_pointer_v<T>;

template <typename T>
concept System = requires(T t) {
    { t.update(std::declval<World&>(), 0.0f) } -> std::same_as<void>;
};

// Type aliases
//
// ComponentTypeId is the consteval Hash64 of the normalized type name
// (see ComponentTypeName.h). Explicit uint64_t — must not narrow to size_t,
// which is 32-bit on some ARM targets.
using ComponentTypeId = std::uint64_t;
using EntityId = uint32;
using EntityVersion = uint16;
using EntityIndex = uint32;

// Constants
inline constexpr std::size_t kChunkSize = 16 * 1024; // 16KB chunks for cache efficiency
inline constexpr std::size_t kCacheLineSize = 64;
inline constexpr EntityId kInvalidEntity = std::numeric_limits<EntityId>::max();
inline constexpr EntityIndex kEntityIndexBits = 20;     // 1M entities
inline constexpr EntityVersion kEntityVersionBits = 12; // 4096 versions
inline constexpr EntityIndex kEntityIndexMask = (1u << kEntityIndexBits) - 1;
inline constexpr EntityVersion kEntityVersionMask = (1u << kEntityVersionBits) - 1;

// Two-level entity location within an ArchetypeTable.
// ChunkIndex identifies the 16KB chunk; IndexInChunk is the slot within it.
struct EntityLocation
{
    uint32_t ChunkIndex = 0;
    uint32_t IndexInChunk = 0;
};


// Enhanced entity handle with stable reference guarantee
struct EntityHandle
{
    union
    {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) // nonstandard extension used: nameless struct/union
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
        struct
        {
            uint32 index : kEntityIndexBits;
            uint32 version : kEntityVersionBits;
        };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        uint32 id;
    };

    EntityHandle() : id(kInvalidEntity) {}
    EntityHandle(EntityIndex idx, EntityVersion ver) : index(idx), version(ver) {}

    static EntityHandle Invalid()
    {
        return EntityHandle();
    }
    explicit EntityHandle(uint32 entityId) : id(entityId) {}

    bool IsValid() const
    {
        return id != kInvalidEntity;
    }
    operator bool() const
    {
        return IsValid();
    }

    bool operator==(const EntityHandle& other) const
    {
        return id == other.id;
    }
    bool operator!=(const EntityHandle& other) const
    {
        return id != other.id;
    }
    bool operator<(const EntityHandle& other) const
    {
        return id < other.id;
    }
};
static_assert(sizeof(EntityHandle) == sizeof(uint32),
    "EntityHandle must be exactly 4 bytes (index + version packed in a single uint32)");

// Hash support for EntityHandle
struct EntityHandleHash
{
    std::size_t operator()(const EntityHandle& handle) const
    {
        return std::hash<uint32>{}(handle.id);
    }
};

// Forward declaration so Command::addComponent can call GetComponentTypeId<T>().
// Definition is below — consteval, returns the 64-bit Hash64 of the normalized type name.
template <Component T>
consteval ComponentTypeId GetComponentTypeId();

// Small-buffer-optimized storage for command component data.
// Avoids heap allocation for components <= kInlineCapacity bytes (covers ~99% of components).
class CommandData
{
    static constexpr size_t kInlineCapacity = 64;

    alignas(16) std::byte m_Inline[kInlineCapacity];
    std::byte* m_Heap = nullptr;
    uint32_t m_Size = 0;

  public:
    CommandData() = default;
    ~CommandData() { delete[] m_Heap; }

    CommandData(CommandData&& other) noexcept
        : m_Heap(other.m_Heap), m_Size(other.m_Size)
    {
        if (other.m_Heap)
        {
            other.m_Heap = nullptr;
        }
        else
        {
            std::memcpy(m_Inline, other.m_Inline, other.m_Size);
        }
        other.m_Size = 0;
    }

    CommandData& operator=(CommandData&& other) noexcept
    {
        if (this != &other)
        {
            delete[] m_Heap;
            m_Heap = other.m_Heap;
            m_Size = other.m_Size;
            if (other.m_Heap)
            {
                other.m_Heap = nullptr;
            }
            else
            {
                std::memcpy(m_Inline, other.m_Inline, other.m_Size);
            }
            other.m_Size = 0;
        }
        return *this;
    }

    CommandData(const CommandData&) = delete;
    CommandData& operator=(const CommandData&) = delete;

    void Assign(const void* data, size_t size)
    {
        delete[] m_Heap;
        m_Heap = nullptr;
        m_Size = static_cast<uint32_t>(size);
        if (size <= kInlineCapacity)
        {
            std::memcpy(m_Inline, data, size);
        }
        else
        {
            m_Heap = new std::byte[size];
            std::memcpy(m_Heap, data, size);
        }
    }

    const std::byte* Data() const { return m_Heap ? m_Heap : m_Inline; }
    size_t Size() const { return m_Size; }
    bool Empty() const { return m_Size == 0; }
};

// Command types for deferred operations
struct Command
{
    enum Type
    {
        CREATE_ENTITY,
        DESTROY_ENTITY,
        ADD_COMPONENT,
        REMOVE_COMPONENT,
        SET_COMPONENT,
        // A required component (Detail::RequiredComponents): added with this value only
        // when the entity lacks it at the flush, so it never replaces a value the entity
        // has, including one queued earlier in the same flush.
        ADD_REQUIRED_COMPONENT,
        // World::SetEntityEnabledImmediate at flush: the same single move as an
        // immediate switch, so a queued switch keeps the derived tag exactly
        // where an immediate one would (see SetEntityEnabledImmediate).
        SET_ENTITY_ENABLED
    };

    Type type;
    EntityHandle entity;
    ComponentTypeId componentType = 0;
    CommandData componentData;
    bool entityEnabled = false; // SET_ENTITY_ENABLED only

    template <Component T>
    static Command addComponent(EntityHandle entity, const T& component)
    {
        Command cmd;
        cmd.type = ADD_COMPONENT;
        cmd.entity = entity;
        cmd.componentType = GetComponentTypeId<T>();
        cmd.componentData.Assign(&component, sizeof(T));
        return cmd;
    }

    template <Component T>
    static Command addRequiredComponent(EntityHandle entity, const T& component)
    {
        Command cmd = addComponent(entity, component);
        cmd.type = ADD_REQUIRED_COMPONENT;
        return cmd;
    }
};

template <Component T>
consteval ComponentTypeId GetComponentTypeId()
{
    // Compile-time 64-bit FNV-1a of the normalized type name. Cross-DLL and
    // cross-platform stable by construction (no allocator, no shared state).
    // MSVC, clang-cl and clang agree on the normalized name byte-for-byte;
    // Tests/ECS/ComponentTypeNameTest.cpp pins that equality.
    return ComponentTypeHash<T>();
}

// Archetype signature: sorted ascending list of component IDs. Sole signature
// type after Phase 1b — the pre-existing DenseSignature bitset was capped at
// 1024 IDs and is replaced by an alias to this class (below) so that 64-bit
// consteval hashes can flow through unchanged.
class ComponentSignature
{
  private:
    std::vector<ComponentTypeId> components;
    mutable std::size_t cachedHash = 0;

  public:
    void Add(ComponentTypeId id)
    {
        // Sorted insert; skip if already present.
        auto it = std::lower_bound(components.begin(), components.end(), id);
        if (it == components.end() || *it != id)
        {
            components.insert(it, id);
            cachedHash = 0;
        }
    }

    void Remove(ComponentTypeId id)
    {
        auto it = std::lower_bound(components.begin(), components.end(), id);
        if (it != components.end() && *it == id)
        {
            components.erase(it);
            cachedHash = 0;
        }
    }

    void Clear()
    {
        components.clear();
        cachedHash = 0;
    }

    bool Contains(ComponentTypeId id) const
    {
        return std::binary_search(components.begin(), components.end(), id);
    }

    // True iff every component in `required` is also in this signature.
    // Sorted-merge two-pointer: O(N + M).
    bool ContainsAll(const ComponentSignature& required) const
    {
        auto a = components.begin();
        const auto aEnd = components.end();
        for (auto id : required.components)
        {
            while (a != aEnd && *a < id) ++a;
            if (a == aEnd || *a != id) return false;
        }
        return true;
    }

    // True iff this signature and `other` share at least one component.
    // Sorted-merge two-pointer: O(N + M).
    bool Intersects(const ComponentSignature& other) const
    {
        auto a = components.begin();
        auto b = other.components.begin();
        const auto aEnd = components.end();
        const auto bEnd = other.components.end();
        while (a != aEnd && b != bEnd)
        {
            if (*a == *b) return true;
            if (*a < *b) ++a;
            else ++b;
        }
        return false;
    }

    // Add every component in `other` to this signature. Idempotent per id.
    void UnionInPlace(const ComponentSignature& other)
    {
        for (auto id : other.components) Add(id);
    }

    std::size_t Hash() const
    {
        if (cachedHash == 0)
        {
            for (auto id : components)
            {
                cachedHash ^= id + 0x9e3779b9 + (cachedHash << 6) + (cachedHash >> 2);
            }
            if (cachedHash == 0) cachedHash = 1; // reserve 0 as "uncached" sentinel
        }
        return cachedHash;
    }

    bool operator==(const ComponentSignature& other) const
    {
        return components == other.components;
    }

    bool operator!=(const ComponentSignature& other) const
    {
        return components != other.components;
    }

    const std::vector<ComponentTypeId>& GetComponents() const
    {
        return components;
    }
};

// Pre-Phase-1b code used a fixed-size 1024-bit `DenseSignature` bitset for fast
// archetype matching. That worked for allocator-assigned small IDs but cannot
// hold 64-bit consteval hashes. We retain the name as an alias to keep typed
// scheduler and hook members (`JobDesc::ReadSet`, `m_AddHookSignature`, etc.)
// and the few remaining non-renamed `DenseSignature` locals readable; new code
// should prefer `ComponentSignature` directly.
using DenseSignature = ComponentSignature;

struct ComponentSignatureHash
{
    std::size_t operator()(const ComponentSignature& sig) const
    {
        return sig.Hash();
    }
};

namespace Detail
{
template <Component... Cs>
ComponentSignature BuildSignature()
{
    ComponentSignature signature;
    (signature.Add(GetComponentTypeId<Cs>()), ...);
    return signature;
}
} // namespace Detail

// The sorted id set of a component pack, built once per instantiation and
// shared by every caller that names that pack. A pack is fixed at compile time,
// so its signature is a constant: a caller holds a reference to this one instead
// of filling a fresh ComponentSignature and paying its vector growth.
template <Component... Cs>
const ComponentSignature& SignatureOf()
{
    static const ComponentSignature signature = Detail::BuildSignature<Cs...>();
    return signature;
}

// The archetype match a query resolves to. Every member points at a signature
// owned elsewhere — the per-instantiation constants a Query<Ts...> holds, or
// the shared SignatureOf<> of a filter's type pack — so building one allocates
// nothing. A null member is no filter at all.
struct ArchetypeFilter
{
    // Components an archetype must have (Read<>/Write<> parameters).
    const ComponentSignature* Required = nullptr;
    // With<>: further components an archetype must have.
    const ComponentSignature* Include = nullptr;
    // Without<>: components an archetype must not have.
    const ComponentSignature* Exclude = nullptr;
    // The enable-state tags that take an archetype out of this query: the two
    // entity-activity tags plus ComponentDisabled<T> per required T. Null when
    // the query opted in with IncludeDisabled().
    const ComponentSignature* DisabledExclude = nullptr;
    // IncludeDisabled<T...>(): the ComponentDisabled<T> ids of the types the
    // query opted back in. Entity activity is never in here, so a per-type
    // opt-in still skips inactive entities.
    const ComponentSignature* DisabledInclude = nullptr;
};

// Entity metadata for efficient lookups.
// Stores a two-level {chunkIndex, indexInChunk} that locates the entity
// inside the archetype's colocated ArchetypeTable.
struct EntityMetadata
{
    Archetype* archetype = nullptr; // 8 bytes
    uint16_t chunkIndex = 0;       // Which 16KB chunk in the table (max 65535)
    uint16_t indexInChunk = 0;     // Slot within that chunk (max 65535, typical ~200)
    EntityVersion version = 0;     // 2 bytes
    bool alive = false;            // 1 byte + 1 padding = 16 bytes total

    EntityLocation GetLocation() const { return {chunkIndex, indexInChunk}; }
    void SetLocation(const EntityLocation& loc)
    {
        // 16-bit fields support up to 65535 chunks × ~600 entities/chunk ≈ 39M entities per archetype.
        // Assert in debug to catch overflow before silent truncation.
        assert(loc.ChunkIndex <= UINT16_MAX && "ChunkIndex overflow — archetype exceeds 65535 chunks");
        assert(loc.IndexInChunk <= UINT16_MAX && "IndexInChunk overflow — chunk capacity exceeds 65535");
        chunkIndex = static_cast<uint16_t>(loc.ChunkIndex);
        indexInChunk = static_cast<uint16_t>(loc.IndexInChunk);
    }
    void SetLocation(uint16_t ci, uint16_t ii)
    {
        chunkIndex = ci;
        indexInChunk = ii;
    }
};
// Pointer + 6 payload bytes + tail padding to pointer alignment: 16 bytes on
// 64-bit, 12 on wasm32. Guards against accidental padding growth on both.
static_assert(sizeof(EntityMetadata) == sizeof(Archetype*) + 8, "EntityMetadata must stay pointer + 8 bytes");


} // namespace ECS
} // namespace GameEngine
