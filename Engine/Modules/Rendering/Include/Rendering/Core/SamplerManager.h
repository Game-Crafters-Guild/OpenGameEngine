#pragma once

#include "Handle.h"
#include <mutex>

namespace GameEngine::Rendering {

/**
 * @brief Specialized sampler resource manager
 * 
 * Follows Single Responsibility Principle by managing only sampler resources.
 * Based on reference implementation patterns but with proper encapsulation.
 */
class SamplerManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static SamplerManager& Instance();

    // Non-copyable, non-movable singleton
    SamplerManager(const SamplerManager&) = delete;
    SamplerManager& operator=(const SamplerManager&) = delete;
    SamplerManager(SamplerManager&&) = delete;
    SamplerManager& operator=(SamplerManager&&) = delete;

    /**
     * @brief Create a new sampler
     * @param sampler backend-native sampler handle to store
     * @return Handle to the created sampler
     */
    SamplerHandle CreateSampler(void* sampler);

    /**
     * @brief Resolve the opaque backend sampler object for a handle
     * @param handle Sampler handle
     * @return The stored backend sampler, or nullptr if the handle is not alive
     *
     * Returns the stored pointer BY VALUE, loaded under m_Mutex. The manager must
     * never hand out a pointer into m_Samplers: GenerationalVector::Get() yields an
     * interior pointer, and a concurrent CreateSampler reallocates the backing
     * vector out from under it.
     */
    void* GetSampler(SamplerHandle handle) const;

    /**
     * @brief Destroy sampler
     * @param handle Sampler handle to destroy
     */
    void DestroySampler(SamplerHandle handle);

    /**
     * @brief Check if sampler handle is valid
     * @param handle Sampler handle to check
     * @return True if handle is valid
     */
    bool IsValidSampler(SamplerHandle handle) const;

    /**
     * @brief Get number of active samplers
     * @return Number of samplers currently managed
     */
    size_t GetSamplerCount() const;

    /**
     * @brief Clear all samplers (for shutdown)
     */
    void Clear();

    /**
     * @brief Get sampler statistics
     */
    struct SamplerStats {
        size_t TotalSamplers = 0;
    };
    
    SamplerStats GetStats() const;

    // Return a copy of all currently alive sampler handles (for shutdown sweeping)
    std::vector<SamplerHandle> GetAllHandlesCopy() const;

private:
    SamplerManager() = default;
    ~SamplerManager() = default;

    // Thread safety for global access
    mutable std::mutex m_Mutex;

    // Sampler storage using simplified generational vector of opaque backend handles
    GenerationalVector<void*> m_Samplers;
    // Track alive handles for iteration when needed (e.g., shutdown)
    std::vector<SamplerHandle> m_Handles;
};

// === Clean C-style API Functions ===

/**
 * @brief Create a sampler using global sampler manager
 * @param sampler backend-native sampler handle to create
 * @return Handle to the created sampler
 */
SamplerHandle CreateSampler(void* sampler);

/**
 * @brief Resolve the opaque backend sampler object for a handle
 * @param handle Sampler handle
 * @return The stored backend sampler, or nullptr if the handle is not alive
 */
void* GetSampler(SamplerHandle handle);

/**
 * @brief Destroy sampler
 * @param handle Sampler handle to destroy
 */
void DestroySampler(SamplerHandle handle);

/**
 * @brief Check if sampler handle is valid
 * @param handle Sampler handle to check
 * @return True if handle is valid
 */
bool IsValidSampler(SamplerHandle handle);

// === Inline Implementation ===

inline SamplerManager& SamplerManager::Instance() {
    static SamplerManager instance;
    return instance;
}

inline SamplerHandle SamplerManager::CreateSampler(void* sampler) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    SamplerHandle h = m_Samplers.Create(sampler);
    if (h.IsValid()) m_Handles.push_back(h);
    return h;
}

inline void* SamplerManager::GetSampler(SamplerHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    void* const* slot = m_Samplers.Get(handle);
    return slot ? *slot : nullptr;
}

inline void SamplerManager::DestroySampler(SamplerHandle handle) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Samplers.Destroy(handle);
    auto it = std::remove(m_Handles.begin(), m_Handles.end(), handle);
    if (it != m_Handles.end()) m_Handles.erase(it, m_Handles.end());
}

inline bool SamplerManager::IsValidSampler(SamplerHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Samplers.IsValid(handle);
}

inline size_t SamplerManager::GetSamplerCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Samplers.Size();
}

inline void SamplerManager::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Samplers.Clear();
    m_Handles.clear();
}

inline SamplerManager::SamplerStats SamplerManager::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    SamplerStats stats;
    stats.TotalSamplers = m_Samplers.Size();

    return stats;
}

inline std::vector<SamplerHandle> SamplerManager::GetAllHandlesCopy() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Handles;
}

// === C-style API Functions ===

inline SamplerHandle CreateSampler(void* sampler) {
    return SamplerManager::Instance().CreateSampler(sampler);
}

inline void* GetSampler(SamplerHandle handle) {
    return SamplerManager::Instance().GetSampler(handle);
}

inline void DestroySampler(SamplerHandle handle) {
    SamplerManager::Instance().DestroySampler(handle);
}

inline bool IsValidSampler(SamplerHandle handle) {
    return SamplerManager::Instance().IsValidSampler(handle);
}

inline std::vector<SamplerHandle> GetAllSamplerHandlesCopy() {
    return SamplerManager::Instance().GetAllHandlesCopy();
}

} // namespace GameEngine::Rendering
