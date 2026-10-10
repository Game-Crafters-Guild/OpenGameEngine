#pragma once

#include "Types/GeometricReserve.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace GameEngine::Rendering {

/**
 * @brief Generational handle system for GPU resources
 *
 * A handle packs an 8-bit generation above a 56-bit index. Generations stay in
 * [1, 255]: a slot whose generation reaches 255 retires instead of wrapping, so
 * no live handle's id can be zero (the invalid id) and no generation repeats on
 * the same index. Each resource type has its own manager and handle alias.
 */

namespace Detail {
    // Handle implementation details
    using HandleType = uint64_t;
    static constexpr HandleType kInvalidHandle = 0;
    static constexpr uint8_t kGenerationBits = 8;
    static constexpr uint64_t kGenerationMask = (1ULL << kGenerationBits) - 1;
    static constexpr uint8_t kIndexBits = 64 - kGenerationBits;
    static constexpr uint64_t kIndexMask = (1ULL << kIndexBits) - 1;
}

/**
 * @brief Primary handle structure
 *
 * A 64-bit value: an 8-bit generation above a 56-bit index. A zero id is the
 * invalid handle.
 */
struct Handle {
    Detail::HandleType id = Detail::kInvalidHandle;

    // Default constructor creates invalid handle
    constexpr Handle() = default;

    // Create from raw ID
    explicit constexpr Handle(Detail::HandleType id) : id(id) {}

    // Create from index and generation
    constexpr Handle(uint64_t index, uint8_t generation)
        : id((static_cast<Detail::HandleType>(generation) << Detail::kIndexBits) | (index & Detail::kIndexMask)) {}

    // Extract components
    constexpr uint64_t Index() const { return id & Detail::kIndexMask; }
    constexpr uint8_t Generation() const { return static_cast<uint8_t>((id >> Detail::kIndexBits) & Detail::kGenerationMask); }

    // Validity check
    constexpr bool IsValid() const { return id != Detail::kInvalidHandle; }

    // Conversion to raw handle type
    constexpr operator Detail::HandleType() const { return id; }

    // Comparison operators
    constexpr bool operator==(const Handle& other) const { return id == other.id; }
    constexpr bool operator!=(const Handle& other) const { return id != other.id; }
    constexpr bool operator<(const Handle& other) const { return id < other.id; }
};

/**
 * @brief Zero-overhead strong handle wrapper for type-safe public APIs
 *
 * Layout and behavior mirror Handle (64-bit id, 8-bit generation), but each
 * distinct Tag produces a distinct type to prevent accidental interchange.
 */
template <typename Tag>
struct StrongHandle {
    Detail::HandleType id = Detail::kInvalidHandle;

    // Default: invalid
    constexpr StrongHandle() = default;
    // Construct from raw id
    explicit constexpr StrongHandle(Detail::HandleType raw) : id(raw) {}
    // Construct from index/generation
    constexpr StrongHandle(uint64_t index, uint8_t generation)
        : id((static_cast<Detail::HandleType>(generation) << Detail::kIndexBits) | (index & Detail::kIndexMask)) {}

    // Accessors compatible with Handle
    constexpr uint64_t Index() const { return id & Detail::kIndexMask; }
    constexpr uint8_t Generation() const { return static_cast<uint8_t>((id >> Detail::kIndexBits) & Detail::kGenerationMask); }
    constexpr bool IsValid() const { return id != Detail::kInvalidHandle; }

    // Interop conversions (temporary to ease migration)
    // N.B. These preserve zero-overhead but allow existing code taking Handle to keep compiling.
    constexpr StrongHandle(Handle h) : id(static_cast<Detail::HandleType>(h)) {}
    constexpr operator Handle() const { return Handle(id); }
    // Raw conversion (for logging/interop)
    constexpr operator Detail::HandleType() const { return id; }

    // Comparisons
    constexpr bool operator==(const StrongHandle& other) const { return id == other.id; }
    constexpr bool operator!=(const StrongHandle& other) const { return id != other.id; }
    constexpr bool operator<(const StrongHandle& other) const { return id < other.id; }
};

// Domain-specific strong handle tags and aliases
struct BufferTag{};      using BufferHandle = StrongHandle<BufferTag>;
struct TextureTag{};     using TextureHandle = StrongHandle<TextureTag>;
struct SamplerTag{};     using SamplerHandle = StrongHandle<SamplerTag>;
struct PipelineTag{};    using PipelineHandle = StrongHandle<PipelineTag>;
struct DescriptorSetTag{}; using DescriptorSetHandle = StrongHandle<DescriptorSetTag>;
struct RenderPassTag{};  using RenderPassHandle = StrongHandle<RenderPassTag>;
struct FramebufferTag{}; using FramebufferHandle = StrongHandle<FramebufferTag>;
struct CommandListTag{}; using CommandListHandle = StrongHandle<CommandListTag>;
struct SemaphoreTag{};   using SemaphoreHandle = StrongHandle<SemaphoreTag>;
struct WindowTargetTag{}; using WindowTargetHandle = StrongHandle<WindowTargetTag>;

struct TextureViewTag{}; using TextureViewHandle = StrongHandle<TextureViewTag>;

// Opaque handle to one of the acceleration-structure backend's independent
// TLAS slots. Each slot is a full TLAS object (storage + scratch region +
// address) so unrelated consumers can hold different filtered instance sets
// over the SAME shared BLAS pool — e.g. the ray-traced shadow-mask lane's
// cast-shadow-only TLAS and the GI lane's unfiltered TLAS. Acquire once per
// consumer lifetime; slots are cheap (a few ever exist) so there is no
// recycling.
//
// Lives here rather than in AccelerationStructure.h because
// DescriptorSetUpdate names it: binding a TLAS to a shader is part of the
// device's descriptor vocabulary, and AccelerationStructure.h sits above
// Device.h in the include order. BLAS handles stay in AccelerationStructure.h
// — nothing binds a BLAS directly.
// Encoding: raw id = slot index + 1, 0 = invalid — the same wire format the
// other AS handle (AccelerationStructureHandle) uses, now expressed through
// the shared StrongHandle machinery instead of a hand-rolled struct.
struct TlasSlotTag{};    using TlasSlotHandle = StrongHandle<TlasSlotTag>;

// Zero‑overhead and triviality guarantees (compile‑time)
static_assert(sizeof(TextureHandle) == sizeof(Handle), "TextureHandle must be zero-overhead");
static_assert(alignof(TextureHandle) == alignof(Handle), "TextureHandle alignment must match");
static_assert(std::is_trivially_copyable_v<TextureHandle>, "TextureHandle must be trivially copyable");
static_assert(std::is_standard_layout_v<TextureHandle>, "TextureHandle must be standard layout");

static_assert(sizeof(BufferHandle) == sizeof(Handle), "BufferHandle must be zero-overhead");
static_assert(sizeof(SamplerHandle) == sizeof(Handle), "SamplerHandle must be zero-overhead");
static_assert(sizeof(PipelineHandle) == sizeof(Handle), "PipelineHandle must be zero-overhead");
static_assert(sizeof(DescriptorSetHandle) == sizeof(Handle), "DescriptorSetHandle must be zero-overhead");
static_assert(sizeof(RenderPassHandle) == sizeof(Handle), "RenderPassHandle must be zero-overhead");
static_assert(sizeof(FramebufferHandle) == sizeof(Handle), "FramebufferHandle must be zero-overhead");
static_assert(sizeof(CommandListHandle) == sizeof(Handle), "CommandListHandle must be zero-overhead");
static_assert(sizeof(SemaphoreHandle) == sizeof(Handle), "SemaphoreHandle must be zero-overhead");
static_assert(sizeof(WindowTargetHandle) == sizeof(Handle), "WindowTargetHandle must be zero-overhead");
static_assert(sizeof(TextureViewHandle) == sizeof(Handle), "TextureViewHandle must be zero-overhead");

// Invalid handle constants
static constexpr Handle        INVALID_HANDLE{};
static constexpr BufferHandle  INVALID_BUFFER_HANDLE{};
static constexpr TextureHandle INVALID_TEXTURE_HANDLE{};
static constexpr TextureViewHandle INVALID_TEXTURE_VIEW_HANDLE{};
static constexpr SamplerHandle INVALID_SAMPLER_HANDLE{};
static constexpr PipelineHandle INVALID_PIPELINE_HANDLE{};

// Helpers to convert between generic Handle and strong handles (internal use)
template <typename Tag>
inline constexpr Handle ToGeneric(StrongHandle<Tag> h) { return Handle(h.id); }
template <typename Tag>
inline constexpr StrongHandle<Tag> FromGeneric(Handle h) { return StrongHandle<Tag>(static_cast<Detail::HandleType>(h)); }

// Direct comparison helpers between strong handles and generic Handle.
// These remove ambiguity when comparing StrongHandle<Tag> against INVALID_HANDLE
// while preserving the convenience of using the generic invalid sentinel.
template <typename Tag>
constexpr bool operator==(StrongHandle<Tag> a, Handle b) { return a.id == b.id; }
template <typename Tag>
constexpr bool operator==(Handle a, StrongHandle<Tag> b) { return a.id == b.id; }
template <typename Tag>
constexpr bool operator!=(StrongHandle<Tag> a, Handle b) { return a.id != b.id; }
template <typename Tag>
constexpr bool operator!=(Handle a, StrongHandle<Tag> b) { return a.id != b.id; }

/**
 * @brief Generational handle manager
 *
 * Owns the index space: hands out indices, recycles freed ones through a FIFO,
 * and bumps a slot's generation on destroy so stale handles stop validating.
 * A slot whose generation reaches kGenerationMask retires rather than wrapping.
 */
class HandleManager {
public:
    HandleManager() = default;
    ~HandleManager() = default;

    // Non-copyable but movable
    HandleManager(const HandleManager&) = delete;
    HandleManager& operator=(const HandleManager&) = delete;
    HandleManager(HandleManager&& other) noexcept
        : m_FreeIndices(std::move(other.m_FreeIndices)),
          m_FreeHead(other.m_FreeHead), m_FreeCount(other.m_FreeCount),
          m_Generations(std::move(other.m_Generations)),
          m_Alive(std::move(other.m_Alive)), m_LiveCount(other.m_LiveCount)
    {
        other.Reset();
    }
    HandleManager& operator=(HandleManager&& other) noexcept {
        if (this != &other) {
            m_FreeIndices = std::move(other.m_FreeIndices);
            m_FreeHead = other.m_FreeHead;
            m_FreeCount = other.m_FreeCount;
            m_Generations = std::move(other.m_Generations);
            m_Alive = std::move(other.m_Alive);
            m_LiveCount = other.m_LiveCount;
            other.Reset();
        }
        return *this;
    }

    /**
     * @brief Create a new handle
     * @return A new valid handle
     */
    Handle Create() {
        uint64_t index;

        if (m_FreeCount)
        {
            // Reuse a freed index
            index = m_FreeIndices[m_FreeHead];
            m_FreeHead = (m_FreeHead + 1) % m_FreeIndices.size();
            --m_FreeCount;
            m_Alive[index] = true;
        }
        else
        {
            // Provision FIFO retirement storage at acquisition. Growth happens
            // only when the queue is empty, so no ring entries need relocating.
            // Reserve both metadata arrays before publishing the new slot too.
            const size_t required = m_Generations.size() + 1;
            if (required > m_FreeIndices.size())
            {
                const size_t capacity = std::max(
                    required,
                    NextGeometricCapacity(m_FreeIndices.size(), m_FreeIndices.max_size()));
                m_Generations.reserve(capacity);
                m_Alive.reserve(capacity);
                m_FreeIndices.resize(capacity);
            }
            m_FreeHead = 0;
            // Allocate new index
            m_Generations.push_back(1);  // Start from generation 1, not 0
            m_Alive.push_back(true);
            index = m_Generations.size() - 1;
            assert(index < (1ULL << Detail::kIndexBits) && "Index overflow");
        }

        ++m_LiveCount;
        return Handle(index, m_Generations[index]);
    }

    /**
     * @brief Check if handle is alive
     * @param handle Handle to check
     * @return True if handle is valid and alive
     */
    bool IsAlive(Handle handle) const {
        if (!handle.IsValid()) return false;

        uint64_t index = handle.Index();
        if (index >= m_Generations.size()) return false;

        return m_Alive[index] && m_Generations[index] == handle.Generation();
    }

    /**
     * @brief Invoke func(Handle) for each currently-alive slot.
     *        Used by cleanup sweeps that can't rely on external tracking.
     */
    template<typename Func>
    void ForEachAlive(Func&& func) const {
        for (uint64_t i = 0; i < m_Generations.size(); ++i) {
            if (m_Alive[i]) {
                func(Handle(i, m_Generations[i]));
            }
        }
    }

    /**
     * @brief Destroy a handle
     * @param handle Handle to destroy
     */
    void Destroy(Handle handle) {
        if (!IsAlive(handle)) return;

        uint64_t index = handle.Index();
        m_Alive[index] = false;
        --m_LiveCount;
        // Retire exhausted slots instead of wrapping the 8-bit generation:
        // wrap would return the invalid zero handle at index 0 and resurrect
        // stale handles at every index. The 56-bit index space permits one
        // additional metadata slot per 255 allocations of a recycled slot,
        // while Create/Destroy retain constant-time free-list operations.
        if (m_Generations[index] != Detail::kGenerationMask) {
            ++m_Generations[index];
            assert(m_FreeCount < m_FreeIndices.size());
            m_FreeIndices[(m_FreeHead + m_FreeCount) % m_FreeIndices.size()] = index;
            ++m_FreeCount;
        }
    }

    /**
     * @brief Reset the manager, invalidating all handles
     */
    void Reset() {
        m_Generations.clear();
        m_Alive.clear();
        m_LiveCount = 0;
        m_FreeIndices.clear();
        m_FreeHead = m_FreeCount = 0;
    }

    /**
     * @brief Get current number of allocated handles
     */
    size_t Size() const {
        return m_LiveCount;
    }

private:
    // Ring storage sized before a slot is created: Destroy must never allocate
    // after invalidating a resource. Reuse order stays FIFO.
    std::vector<uint64_t> m_FreeIndices;
    size_t m_FreeHead = 0;
    size_t m_FreeCount = 0;
    std::vector<uint8_t> m_Generations;
    // Parallel to m_Generations. True while a slot is allocated. Lets
    // ForEachAlive walk the vector without needing to peek inside the
    // free-index FIFO. std::vector<bool> is bit-packed — negligible overhead.
    std::vector<bool> m_Alive;
    size_t m_LiveCount = 0;
};

/**
 * @brief Handle-keyed storage built on HandleManager
 *
 * Holds one T per index in a dense vector, addressed by the handles the manager
 * hands out. A destroyed slot releases its value immediately.
 */
template<typename T>
class GenerationalVector {
public:
    GenerationalVector() = default;
    ~GenerationalVector() = default;

    // Non-copyable but movable
    GenerationalVector(const GenerationalVector&) = delete;
    GenerationalVector& operator=(const GenerationalVector&) = delete;
    GenerationalVector(GenerationalVector&&) = default;
    GenerationalVector& operator=(GenerationalVector&&) = default;

    /**
     * @brief Create new element
     */
    template<typename... Args>
    Handle Create(Args&&... args) {
        Handle handle = m_Manager.Create();
        uint64_t index = handle.Index();

        // Ensure vector is large enough
        if (index >= m_Data.size()) {
            m_Data.resize(index + 1);
        }

        // Construct element directly (much simpler than aligned_storage_t)
        m_Data[index] = T(std::forward<Args>(args)...);

        return handle;
    }

    /**
     * @brief Create from existing object
     */
    Handle Create(T&& data) {
        Handle handle = m_Manager.Create();
        uint64_t index = handle.Index();

        if (index >= m_Data.size()) {
            m_Data.resize(index + 1);
        }

        m_Data[index] = std::move(data);
        return handle;
    }

    /**
     * @brief Destroy element
     */
    void Destroy(Handle handle) {
        const bool valid = IsValid(handle);
        assert(valid && "Attempting to destroy invalid handle");
        if (!valid) return;
        m_Manager.Destroy(handle);
        // A dead slot owns nothing: a retired slot is never reused, and an ordinary
        // free slot must not keep its value alive until some later Create overwrites
        // it. Raw pointers stay non-owning; backends remain responsible for the
        // teardown of the GPU resources they point at.
        m_Data[handle.Index()] = T{};
    }

    /**
     * @brief Access element (with validation)
     */
    T& operator[](Handle handle) {
        assert(IsValid(handle) && "Invalid handle access");
        return m_Data[handle.Index()];
    }

    const T& operator[](Handle handle) const {
        assert(IsValid(handle) && "Invalid handle access");
        return m_Data[handle.Index()];
    }

    /**
     * @brief Get element pointer (returns nullptr if invalid)
     *
     * Returns an INTERIOR pointer into m_Data. Any subsequent Create() may resize
     * the vector and invalidate it, so the result must be consumed before the next
     * mutation and must never outlive the lock a caller used to obtain it. Callers
     * that publish across a lock boundary copy the element out instead — see
     * TextureManager::GetTexture.
     */
    T* Get(Handle handle) {
        return IsValid(handle) ? &m_Data[handle.Index()] : nullptr;
    }

    const T* Get(Handle handle) const {
        return IsValid(handle) ? &m_Data[handle.Index()] : nullptr;
    }

    /**
     * @brief Check if handle is valid
     */
    bool IsValid(Handle handle) const {
        return m_Manager.IsAlive(handle);
    }

    /**
     * @brief Get number of alive elements
     */
    size_t Size() const {
        return m_Manager.Size();
    }

    /**
     * @brief Clear all elements
     */
    void Clear() {
        m_Data.clear();
        m_Manager.Reset();
    }

    /**
     * @brief Invoke func(Handle, T&) for every currently-alive element.
     *        Intended for teardown sweeps. O(N) over the capacity, not the
     *        alive count.
     */
    template<typename Func>
    void ForEach(Func&& func) {
        m_Manager.ForEachAlive([&](Handle h) { func(h, m_Data[h.Index()]); });
    }

    template<typename Func>
    void ForEach(Func&& func) const {
        m_Manager.ForEachAlive([&](Handle h) { func(h, m_Data[h.Index()]); });
    }

private:
    HandleManager m_Manager;
    std::vector<T> m_Data;  // Much simpler than aligned_storage_t
};

} // namespace GameEngine::Rendering

// Hash function for Handle type to enable use in std::unordered_map
namespace std {
    template<>
    struct hash<GameEngine::Rendering::Handle> {
        size_t operator()(const GameEngine::Rendering::Handle& handle) const noexcept {
            return std::hash<uint64_t>{}(handle.id);
        }
    };

    // Hash for StrongHandle<T>
    template <typename Tag>
    struct hash<GameEngine::Rendering::StrongHandle<Tag>> {
        size_t operator()(const GameEngine::Rendering::StrongHandle<Tag>& h) const noexcept {
            return std::hash<uint64_t>{}(static_cast<uint64_t>(h));
        }
    };

}
