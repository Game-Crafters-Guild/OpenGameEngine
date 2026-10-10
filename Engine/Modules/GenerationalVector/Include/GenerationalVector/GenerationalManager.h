#pragma once

#include "GenerationalHandle.h"
#include <vector>
#include <cassert>

namespace GenerationalVector {

/**
 * @brief Manages generation tracking for handles
 * 
 * This class tracks which slots are alive and their current generation.
 * It provides efficient allocation and validation of handles.
 */
class GenerationalManager {
public:
    GenerationalManager() = default;
    ~GenerationalManager() = default;

    // Non-copyable but movable
    GenerationalManager(const GenerationalManager&) = delete;
    GenerationalManager& operator=(const GenerationalManager&) = delete;
    GenerationalManager(GenerationalManager&&) = default;
    GenerationalManager& operator=(GenerationalManager&&) = default;

    /**
     * @brief Create a new handle
     * @return A new valid handle
     */
    Handle Create();

    /**
     * @brief Destroy a handle, making it invalid
     * @param handle The handle to destroy
     */
    void Destroy(Handle handle);

    /**
     * @brief Check if a handle is currently alive
     * @param handle The handle to check
     * @return True if the handle is valid and alive
     */
    bool IsAlive(Handle handle) const;

    /**
     * @brief Get the live handle occupying a slot
     * @param index The slot index
     * @return The slot's current handle, or an invalid handle if the slot is dead
     */
    Handle HandleAt(Handle::IndexType index) const;

    /**
     * @brief Get the current size (number of allocated slots)
     * @return Number of slots that have been allocated
     */
    size_t Size() const { return m_Generations.size(); }

    /**
     * @brief Get the number of currently alive handles
     * @return Number of handles that are currently valid
     */
    size_t AliveCount() const { return m_AliveCount; }

    /**
     * @brief Reserve space for a certain number of handles
     * @param capacity The number of handles to reserve space for
     */
    void Reserve(size_t capacity);

    /**
     * @brief Clear all handles, making them invalid
     */
    void Clear();

private:
    struct SlotInfo {
        Handle::GenerationType Generation = Handle::kInvalidGeneration + 1; // Start at 1
        bool Alive = false;
    };

    std::vector<SlotInfo> m_Generations;
    std::vector<Handle::IndexType> m_FreeList;
    size_t m_AliveCount = 0;

    Handle::IndexType AllocateSlot();
    void FreeSlot(Handle::IndexType index);
};

} // namespace GenerationalVector
