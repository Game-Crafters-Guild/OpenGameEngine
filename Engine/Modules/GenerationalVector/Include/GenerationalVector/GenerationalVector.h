#pragma once

#include "GenerationalHandle.h"
#include "GenerationalManager.h"
#include <vector>
#include <algorithm>
#include <cassert>
#include <new>
#include <utility>
#include <type_traits>

namespace GenerationalVector {

/**
 * @brief A vector that uses generational handles for safe access
 *
 * This container provides O(1) access to elements using generational handles.
 * Handles become invalid when their corresponding element is destroyed,
 * preventing use-after-free bugs.
 *
 * Handles remain valid across Create calls, but raw element pointers and
 * references do NOT: Create may grow the storage, which relocates every live
 * element by move-construction. Re-fetch through the handle after any Create.
 *
 * @tparam T The type of elements stored in the vector
 */
template<typename T>
class GenerationalVector {
public:
    using value_type = T;
    using size_type = size_t;
    using handle_type = Handle;

    GenerationalVector() = default;
    ~GenerationalVector() { Clear(); }

    // Non-copyable but movable
    GenerationalVector(const GenerationalVector&) = delete;
    GenerationalVector& operator=(const GenerationalVector&) = delete;
    GenerationalVector(GenerationalVector&&) = default;
    GenerationalVector& operator=(GenerationalVector&& other) noexcept {
        if (this != &other) {
            Clear(); // The overwritten elements must be destroyed, not abandoned.
            m_Manager = std::move(other.m_Manager);
            m_Data = std::move(other.m_Data);
        }
        return *this;
    }

    /**
     * @brief Create a new element in-place
     * @tparam Args Constructor argument types
     * @param args Constructor arguments
     * @return Handle to the created element
     */
    template<typename... Args>
    Handle Create(Args&&... args);

    /**
     * @brief Create a new element by moving
     * @param data The data to move into the vector
     * @return Handle to the created element
     */
    Handle Create(T&& data);

    /**
     * @brief Create a new element by copying
     * @param data The data to copy into the vector
     * @return Handle to the created element
     */
    Handle Create(const T& data);

    /**
     * @brief Destroy an element, invalidating its handle
     * @param handle The handle to the element to destroy
     */
    void Destroy(Handle handle);

    /**
     * @brief Get a reference to an element
     * @param handle The handle to the element
     * @return Reference to the element
     * @note Asserts if handle is invalid
     */
    T& operator[](Handle handle);

    /**
     * @brief Get a const reference to an element
     * @param handle The handle to the element
     * @return Const reference to the element
     * @note Asserts if handle is invalid
     */
    const T& operator[](Handle handle) const;

    /**
     * @brief Get a pointer to an element if handle is valid
     * @param handle The handle to the element
     * @return Pointer to the element, or nullptr if handle is invalid
     */
    T* Get(Handle handle);

    /**
     * @brief Get a const pointer to an element if handle is valid
     * @param handle The handle to the element
     * @return Const pointer to the element, or nullptr if handle is invalid
     */
    const T* Get(Handle handle) const;

    /**
     * @brief Check if a handle is valid and points to a live element
     * @param handle The handle to check
     * @return True if the handle is valid
     */
    bool IsValid(Handle handle) const;

    /**
     * @brief Get the number of elements currently stored
     * @return Number of live elements
     */
    size_t Size() const { return m_Manager.AliveCount(); }

    /**
     * @brief Check if the vector is empty
     * @return True if no elements are stored
     */
    bool Empty() const { return Size() == 0; }

    /**
     * @brief Get the capacity of the underlying storage
     * @return Current capacity
     */
    size_t Capacity() const { return m_Data.capacity(); }

    /**
     * @brief Reserve space for a certain number of elements
     * @param capacity The number of elements to reserve space for
     */
    void Reserve(size_t capacity);

    /**
     * @brief Clear all elements, invalidating all handles
     */
    void Clear();

    /**
     * @brief Iterate over all valid elements
     * @tparam Func Function type that takes (Handle, T&)
     * @param func Function to call for each element
     */
    template<typename Func>
    void ForEach(Func&& func);

    /**
     * @brief Iterate over all valid elements (const version)
     * @tparam Func Function type that takes (Handle, const T&)
     * @param func Function to call for each element
     */
    template<typename Func>
    void ForEach(Func&& func) const;

private:
    GenerationalManager m_Manager;
    std::vector<std::aligned_storage_t<sizeof(T), alignof(T)>> m_Data;

    void EnsureCapacity(Handle::IndexType index);

    T* GetStorage(Handle::IndexType index) {
        return reinterpret_cast<T*>(&m_Data[index]);
    }

    const T* GetStorage(Handle::IndexType index) const {
        return reinterpret_cast<const T*>(&m_Data[index]);
    }
};

} // namespace GenerationalVector

// Include implementation
#include "GenerationalVector.inl"
