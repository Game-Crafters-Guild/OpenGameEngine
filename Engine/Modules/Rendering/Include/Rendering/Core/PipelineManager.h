#pragma once

#include "Handle.h"
#include <mutex>

namespace GameEngine::Rendering {

/**
 * @brief Specialized pipeline resource manager
 *
 * Follows Single Responsibility Principle by managing only pipeline resources.
 * Based on reference implementation patterns but with proper encapsulation.
 */
class PipelineManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static PipelineManager& Instance();

    // Non-copyable, non-movable singleton
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;
    PipelineManager(PipelineManager&&) = delete;
    PipelineManager& operator=(PipelineManager&&) = delete;

    /**
     * @brief Create a new pipeline
     * @param pipeline Pointer to opaque backend pipeline
     * @return Handle to the created pipeline
     */
    PipelineHandle CreatePipeline(void* pipeline);

    /**
     * @brief Resolve the opaque backend pipeline object for a handle
     * @param handle Pipeline handle
     * @return The stored backend pointer, or nullptr if the handle is not alive
     *
     * Returns the stored pointer BY VALUE, loaded under m_Mutex. The manager must
     * never hand out a pointer into m_Pipelines: GenerationalVector::Get() yields an
     * interior pointer, and a concurrent CreatePipeline reallocates the backing
     * vector out from under it.
     */
    void* GetPipeline(PipelineHandle handle) const;

    /**
     * @brief Destroy pipeline
     * @param handle Pipeline handle to destroy
     */
    void DestroyPipeline(PipelineHandle handle);

    /**
     * @brief Check if pipeline handle is valid
     * @param handle Pipeline handle to check
     * @return True if handle is valid
     */
    bool IsValidPipeline(PipelineHandle handle) const;

    /**
     * @brief Get number of active pipelines
     * @return Number of pipelines currently managed
     */
    size_t GetPipelineCount() const;


    /**
     * @brief Clear all pipelines (for shutdown)
     */
    void Clear();

    /**
     * @brief Get pipeline statistics
     */
    struct PipelineStats {
        size_t TotalPipelines = 0;
        size_t GraphicsPipelines = 0;
        size_t ComputePipelines = 0;
        size_t RayTracingPipelines = 0;
    };

    PipelineStats GetStats() const;

    // Return a copy of all currently alive pipeline handles (for shutdown sweeping)
    std::vector<PipelineHandle> GetAllHandlesCopy() const;

private:
    PipelineManager() = default;
    ~PipelineManager() = default;

    // Thread safety for global access
    mutable std::mutex m_Mutex;

    // Pipeline storage using simplified generational vector of void pointers
    GenerationalVector<void*> m_Pipelines;
    // Track alive handles for iteration when needed (e.g., shutdown)
    std::vector<PipelineHandle> m_Handles;
};

// === Clean C-style API Functions ===

/**
 * @brief Create a pipeline using global pipeline manager
 * @param pipeline Pointer to opaque backend pipeline
 * @return Handle to the created pipeline
 */
PipelineHandle CreatePipeline(void* pipeline);

/**
 * @brief Resolve the opaque backend pipeline object for a handle
 * @param handle Pipeline handle
 * @return The stored backend pointer, or nullptr if the handle is not alive
 */
void* GetPipeline(PipelineHandle handle);

/**
 * @brief Destroy pipeline by handle
 * @param handle Pipeline handle to destroy
 */
void DestroyPipeline(PipelineHandle handle);

/**
 * @brief Check if pipeline handle is valid
 * @param handle Pipeline handle to check
 * @return True if handle is valid
 */
bool IsValidPipeline(PipelineHandle handle);

// === Inline Implementation ===

inline PipelineManager& PipelineManager::Instance() {
    static PipelineManager instance;
    return instance;
}

inline PipelineHandle PipelineManager::CreatePipeline(void* pipeline) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    PipelineHandle h = m_Pipelines.Create(pipeline);
    if (h.IsValid()) m_Handles.push_back(h);
    return h;
}

inline void* PipelineManager::GetPipeline(PipelineHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    void* const* slot = m_Pipelines.Get(handle);
    return slot ? *slot : nullptr;
}

inline void PipelineManager::DestroyPipeline(PipelineHandle handle) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pipelines.Destroy(handle);
    auto it = std::remove(m_Handles.begin(), m_Handles.end(), handle);
    if (it != m_Handles.end()) m_Handles.erase(it, m_Handles.end());
}

inline bool PipelineManager::IsValidPipeline(PipelineHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Pipelines.IsValid(handle);
}


inline size_t PipelineManager::GetPipelineCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Pipelines.Size();
}

inline void PipelineManager::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pipelines.Clear();
    m_Handles.clear();
}

inline PipelineManager::PipelineStats PipelineManager::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    PipelineStats stats;
    stats.TotalPipelines = m_Pipelines.Size();

    return stats;
}

inline std::vector<PipelineHandle> PipelineManager::GetAllHandlesCopy() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Handles;
}

// === C-style API Functions ===

inline PipelineHandle CreatePipeline(void* pipeline) {
    return PipelineManager::Instance().CreatePipeline(pipeline);
}

inline void* GetPipeline(PipelineHandle handle) {
    return PipelineManager::Instance().GetPipeline(handle);
}

inline void DestroyPipeline(PipelineHandle handle) {
    PipelineManager::Instance().DestroyPipeline(handle);
}

inline bool IsValidPipeline(PipelineHandle handle) {
    return PipelineManager::Instance().IsValidPipeline(handle);
}

} // namespace GameEngine::Rendering
