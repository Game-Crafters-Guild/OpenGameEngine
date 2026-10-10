#pragma once

#include <cstdint>
#include <limits>
#include <functional>

namespace GenerationalVector {

/**
 * @brief A generational handle that provides safe access to resources
 * 
 * Handles consist of an index and a generation. The generation is incremented
 * each time a slot is reused, preventing access to stale handles.
 */
class Handle {
public:
    using IndexType = uint32_t;
    using GenerationType = uint32_t;
    using HandleType = uint64_t;

    static constexpr IndexType kInvalidIndex = std::numeric_limits<IndexType>::max();
    static constexpr GenerationType kInvalidGeneration = 0;
    static constexpr HandleType kInvalidHandle = 0;

    // Default constructor creates an invalid handle
    constexpr Handle() noexcept : m_Value(kInvalidHandle) {}

    // Create handle from index and generation
    constexpr Handle(IndexType index, GenerationType generation) noexcept
        : m_Value(static_cast<HandleType>(generation) << 32 | static_cast<HandleType>(index)) {}

    // Create handle from raw value
    explicit constexpr Handle(HandleType value) noexcept : m_Value(value) {}

    // Get the index component
    constexpr IndexType Index() const noexcept {
        return static_cast<IndexType>(m_Value & 0xFFFFFFFF);
    }

    // Get the generation component
    constexpr GenerationType Generation() const noexcept {
        return static_cast<GenerationType>(m_Value >> 32);
    }

    // Get the raw handle value
    constexpr HandleType Value() const noexcept {
        return m_Value;
    }

    // Check if handle is valid
    constexpr bool IsValid() const noexcept {
        return m_Value != kInvalidHandle && Index() != kInvalidIndex && Generation() != kInvalidGeneration;
    }

    // Comparison operators
    constexpr bool operator==(const Handle& other) const noexcept {
        return m_Value == other.m_Value;
    }

    constexpr bool operator!=(const Handle& other) const noexcept {
        return m_Value != other.m_Value;
    }

    constexpr bool operator<(const Handle& other) const noexcept {
        return m_Value < other.m_Value;
    }

    // Conversion to bool for validity checking
    explicit constexpr operator bool() const noexcept {
        return IsValid();
    }

    // Conversion to raw value for serialization/hashing
    explicit constexpr operator HandleType() const noexcept {
        return m_Value;
    }

private:
    HandleType m_Value;
};

} // namespace GenerationalVector

// Hash support for std::unordered_map
namespace std {
    template<>
    struct hash<GenerationalVector::Handle> {
        size_t operator()(const GenerationalVector::Handle& handle) const noexcept {
            return hash<GenerationalVector::Handle::HandleType>{}(handle.Value());
        }
    };
}
