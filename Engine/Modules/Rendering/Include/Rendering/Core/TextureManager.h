#pragma once

#include "Handle.h"
#include <mutex>

namespace GameEngine::Rendering {

/**
 * @brief Specialized texture resource manager
 * 
 * Follows Single Responsibility Principle by managing only texture resources.
 * Based on reference implementation patterns but with proper encapsulation.
 */
class TextureManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static TextureManager& Instance();

    // Non-copyable, non-movable singleton
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;
    TextureManager(TextureManager&&) = delete;
    TextureManager& operator=(TextureManager&&) = delete;

    /**
     * @brief Create a new texture
     * @param texture Pointer to opaque backend texture object
     * @return Handle to the created texture
     */
    TextureHandle CreateTexture(void* texture);

    /**
     * @brief Resolve the opaque backend texture object for a handle
     * @param handle Texture handle
     * @return The stored backend pointer, or nullptr if the handle is not alive
     *
     * Returns the stored pointer BY VALUE, loaded under m_Mutex. The manager must
     * never hand out a pointer into m_Textures: GenerationalVector::Get() yields an
     * interior pointer, and a concurrent CreateTexture reallocates the backing
     * vector out from under it. Registration runs on ECS extraction workers
     * (TextureService::GetOrUpload), so that is a live race, not a theoretical one.
     */
    void* GetTexture(TextureHandle handle) const;

    /**
     * @brief Destroy texture
     * @param handle Texture handle to destroy
     */
    void DestroyTexture(TextureHandle handle);

    /**
     * @brief Check if texture handle is valid
     * @param handle Texture handle to check
     * @return True if handle is valid
     */
    bool IsValidTexture(TextureHandle handle) const;

    /**
     * @brief Get number of active textures
     * @return Number of textures currently managed
     */
    size_t GetTextureCount() const;

    /**
     * @brief Clear all textures (for shutdown)
     */
    void Clear();

    /**
     * @brief Return a copy of all currently alive texture handles (for shutdown sweeping)
     */
    std::vector<TextureHandle> GetAllHandlesCopy() const;

    /**
     * @brief Get texture statistics
     */
    struct TextureStats {
        size_t TotalTextures = 0;
        size_t TotalMemoryUsed = 0;  // In bytes
        size_t AverageTextureSize = 0;
        size_t TexturesByFormat[16] = {0};  // Count by format (simplified)
    };

    TextureStats GetStats() const;

private:
    TextureManager() = default;
    ~TextureManager() = default;

    // Thread safety for global access
    mutable std::mutex m_Mutex;

    // Texture storage using simplified generational vector of void pointers
    GenerationalVector<void*> m_Textures;
    // Track alive handles for iteration when needed (e.g., shutdown sweep)
    std::vector<TextureHandle> m_Handles;
};

// === Clean C-style API Functions ===

/**
 * @brief Create a texture using global texture manager
 * @param texture Pointer to opaque backend texture
 * @return Handle to the created texture
 */
TextureHandle CreateTexture(void* texture);

/**
 * @brief Resolve the opaque backend texture object for a handle
 * @param handle Texture handle
 * @return The stored backend pointer, or nullptr if the handle is not alive
 */
void* GetTexture(TextureHandle handle);

/**
 * @brief Destroy texture by handle
 * @param handle Texture handle to destroy
 */
void DestroyTexture(TextureHandle handle);

/**
 * @brief Check if texture handle is valid
 * @param handle Texture handle to check
 * @return True if handle is valid
 */
bool IsValidTexture(TextureHandle handle);

// === Inline Implementation ===

inline TextureManager& TextureManager::Instance() {
    static TextureManager instance;
    return instance;
}

inline TextureHandle TextureManager::CreateTexture(void* texture) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    TextureHandle h = m_Textures.Create(texture);
    if (h.IsValid()) m_Handles.push_back(h);
    return h;
}

inline void* TextureManager::GetTexture(TextureHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    void* const* slot = m_Textures.Get(handle);
    return slot ? *slot : nullptr;
}

inline void TextureManager::DestroyTexture(TextureHandle handle) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Textures.Destroy(handle);
    auto it = std::remove(m_Handles.begin(), m_Handles.end(), handle);
    if (it != m_Handles.end()) m_Handles.erase(it, m_Handles.end());
}

inline bool TextureManager::IsValidTexture(TextureHandle handle) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Textures.IsValid(handle);
}

inline size_t TextureManager::GetTextureCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Textures.Size();
}

inline void TextureManager::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Textures.Clear();
    m_Handles.clear();
}

inline std::vector<TextureHandle> TextureManager::GetAllHandlesCopy() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Handles;
}

inline TextureManager::TextureStats TextureManager::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    TextureStats stats;
    stats.TotalTextures = m_Textures.Size();

    return stats;
}

// === C-style API Functions ===

inline TextureHandle CreateTexture(void* texture) {
    return TextureManager::Instance().CreateTexture(texture);
}

inline void* GetTexture(TextureHandle handle) {
    return TextureManager::Instance().GetTexture(handle);
}

inline void DestroyTexture(TextureHandle handle) {
    TextureManager::Instance().DestroyTexture(handle);
}

inline bool IsValidTexture(TextureHandle handle) {
    return TextureManager::Instance().IsValidTexture(handle);
}

// Helper to fetch all alive texture handles (for device shutdown sweeping)
inline std::vector<TextureHandle> GetAllTextureHandlesCopy() {
    return TextureManager::Instance().GetAllHandlesCopy();
}

} // namespace GameEngine::Rendering
