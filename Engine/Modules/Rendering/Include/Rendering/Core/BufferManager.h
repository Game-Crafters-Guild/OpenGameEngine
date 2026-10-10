#pragma once

#include "Handle.h"
#include <mutex>

namespace GameEngine::Rendering {

/**
 * @brief Specialized buffer resource manager
 *
 * Follows Single Responsibility Principle by managing only buffer resources.
 * Based on reference implementation patterns but with proper encapsulation.
 */
class BufferManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static BufferManager& Instance();

    // Non-copyable, non-movable singleton
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;
    BufferManager(BufferManager&&) = delete;
    BufferManager& operator=(BufferManager&&) = delete;

    /**
     * @brief Create a new buffer
     * @param buffer Pointer to opaque backend buffer
     * @return Handle to the created buffer
     */
    BufferHandle CreateBuffer(void* buffer);

    /**
     * @brief Resolve the opaque backend buffer object for a handle
     * @param handle Buffer handle
     * @return The stored backend pointer, or nullptr if the handle is not alive
     *
     * Returns the stored pointer BY VALUE, loaded under m_Mutex. The manager must
     * never hand out a pointer into m_Buffers: GenerationalVector::Get() yields an
     * interior pointer, and a concurrent CreateBuffer reallocates the backing
     * vector out from under it.
     */
    void* GetBuffer(BufferHandle handle) const;

    /**
     * @brief Destroy buffer
     * @param handle Buffer handle to destroy
     */
    void DestroyBuffer(BufferHandle handle);

    /**
     * @brief Check if buffer handle is valid
     * @param handle Buffer handle to check
     * @return True if handle is valid
     */
    bool IsValidBuffer(BufferHandle handle) const;

    /**
     * @brief Get number of active buffers
     * @return Number of buffers currently managed
     */
    size_t GetBufferCount() const;

    /**
     * @brief Clear all buffers (for shutdown)
     */
    void Clear();

    /**
     * @brief Get buffer statistics
     */
    struct BufferStats {
        size_t TotalBuffers = 0;
        size_t TotalMemoryUsed = 0;  // In bytes
        size_t AverageBufferSize = 0;
    };

    BufferStats GetStats() const;

private:
    BufferManager() = default;
    ~BufferManager() = default;

    // Thread safety for global access
    mutable std::mutex m_Mutex;

    // Buffer storage using simplified generational vector of void pointers
    GenerationalVector<void*> m_Buffers;
};

// === Clean C-style API Functions ===

/**
 * @brief Create a buffer using global buffer manager
 * @param buffer Pointer to opaque backend buffer
 * @return Handle to the created buffer
 */
BufferHandle CreateBuffer(void* buffer);

/**
 * @brief Resolve the opaque backend buffer object for a handle
 * @param handle Buffer handle
 * @return The stored backend pointer, or nullptr if the handle is not alive
 */
void* GetBuffer(BufferHandle handle);

/**
 * @brief Destroy buffer by handle
 * @param handle Buffer handle to destroy
 */
void DestroyBuffer(BufferHandle handle);

/**
 * @brief Check if buffer handle is valid
 * @param handle Buffer handle to check
 * @return True if handle is valid
 */
bool IsValidBuffer(BufferHandle handle);

// === Inline Implementation ===

inline BufferManager& BufferManager::Instance() {
    static BufferManager instance;
    return instance;
}

inline BufferHandle BufferManager::CreateBuffer(void* buffer) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Buffers.Create(buffer);
}

inline void* BufferManager::GetBuffer(BufferHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    void* const* slot = m_Buffers.Get(handle);
    return slot ? *slot : nullptr;
}

inline void BufferManager::DestroyBuffer(BufferHandle handle) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buffers.Destroy(handle);
}

inline bool BufferManager::IsValidBuffer(BufferHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Buffers.IsValid(handle);
}

inline size_t BufferManager::GetBufferCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Buffers.Size();
}

inline void BufferManager::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buffers.Clear();
}

inline BufferManager::BufferStats BufferManager::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    BufferStats stats;
    stats.TotalBuffers = m_Buffers.Size();

    return stats;
}

// === C-style API Functions ===

inline BufferHandle CreateBuffer(void* buffer) {
    return BufferManager::Instance().CreateBuffer(buffer);
}

inline void* GetBuffer(BufferHandle handle) {
    return BufferManager::Instance().GetBuffer(handle);
}

inline void DestroyBuffer(BufferHandle handle) {
    BufferManager::Instance().DestroyBuffer(handle);
}

inline bool IsValidBuffer(BufferHandle handle) {
    return BufferManager::Instance().IsValidBuffer(handle);
}

} // namespace GameEngine::Rendering
