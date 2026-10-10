#pragma once

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/BufferManager.h"
#include "Rendering/Core/TextureManager.h"
#include "Rendering/Core/PipelineManager.h"
#include "VulkanDevice.h"

namespace GameEngine {
namespace Rendering {

// Centralized helpers to get Vulkan backend pointers from opaque handles.
// These are inline and trivial so they are effectively zero-cost wrappers
// around the global managers.

inline VulkanTexture* GetVulkanTexture(TextureHandle h) {
    void* raw = GameEngine::Rendering::GetTexture(h);
#ifndef NDEBUG
    // Only assert when the handle is also considered valid by the TextureManager.
    // This avoids tripping on special non-managed handles (e.g., swapchain textures).
    if (h.IsValid() && !raw && GameEngine::Rendering::IsValidTexture(h)) {
        assert(false && "GetVulkanTexture: handle is valid in TextureManager but backing pointer is null");
    }
#endif
    return static_cast<VulkanTexture*>(raw);
}

inline VulkanBuffer* GetVulkanBuffer(BufferHandle h) {
    void* raw = GameEngine::Rendering::GetBuffer(h);
#ifndef NDEBUG
    if (h.IsValid() && !raw && GameEngine::Rendering::IsValidBuffer(h)) {
        assert(false && "GetVulkanBuffer: handle is valid in BufferManager but backing pointer is null");
    }
#endif
    return static_cast<VulkanBuffer*>(raw);
}

inline VulkanPipeline* GetVulkanPipeline(PipelineHandle h) {
    void* raw = GameEngine::Rendering::GetPipeline(h);
#ifndef NDEBUG
    if (h.IsValid() && !raw && GameEngine::Rendering::IsValidPipeline(h)) {
        assert(false && "GetVulkanPipeline: handle is valid in PipelineManager but backing pointer is null");
    }
#endif
    return static_cast<VulkanPipeline*>(raw);
}

// Const-view helpers (do not allow mutation through the returned pointer)
inline const VulkanTexture* GetVulkanTextureConst(TextureHandle h) {
    return GetVulkanTexture(h);
}

inline const VulkanBuffer* GetVulkanBufferConst(BufferHandle h) {
    return GetVulkanBuffer(h);
}

inline const VulkanPipeline* GetVulkanPipelineConst(PipelineHandle h) {
    return GetVulkanPipeline(h);
}

// Overloads for raw void* when code already has an opaque pointer.
inline VulkanTexture* GetVulkanTexture(void* ptr) {
    return static_cast<VulkanTexture*>(ptr);
}

inline VulkanBuffer* GetVulkanBuffer(void* ptr) {
    return static_cast<VulkanBuffer*>(ptr);
}

inline VulkanPipeline* GetVulkanPipeline(void* ptr) {
    return static_cast<VulkanPipeline*>(ptr);
}

inline const VulkanTexture* GetVulkanTextureConst(const void* ptr) {
    return static_cast<const VulkanTexture*>(ptr);
}

inline const VulkanBuffer* GetVulkanBufferConst(const void* ptr) {
    return static_cast<const VulkanBuffer*>(ptr);
}

inline const VulkanPipeline* GetVulkanPipelineConst(const void* ptr) {
    return static_cast<const VulkanPipeline*>(ptr);
}

} // namespace Rendering
} // namespace GameEngine

