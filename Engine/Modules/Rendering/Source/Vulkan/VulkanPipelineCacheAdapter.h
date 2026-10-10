#pragma once
#include "Rendering/Core/IPipelineCache.h"
#include "VulkanPipelineCache.h" // Vulkan-specific cache

namespace GameEngine { namespace Rendering {

// Thin adapter to expose VulkanPipelineCache via backend-agnostic interface
class VulkanPipelineCacheAdapter final : public IPipelineCache {
public:
    explicit VulkanPipelineCacheAdapter(VulkanPipelineCache* vkCache) : m_VkCache(vkCache) {}
    ~VulkanPipelineCacheAdapter() override = default;

    bool Initialize() override { return m_VkCache ? m_VkCache->Initialize() : false; }
    void Shutdown() override { if (m_VkCache) m_VkCache->Shutdown(); }

    bool SaveCache() override { return m_VkCache ? m_VkCache->SaveCache() : false; }
    bool LoadCache() override { return m_VkCache ? m_VkCache->LoadCache() : false; }
    void ClearCache() override { if (m_VkCache) m_VkCache->ClearCache(); }

    bool IsValid() const override { return m_VkCache ? m_VkCache->IsValid() : false; }

    Statistics GetStatistics() const override {
        Statistics s{};
        if (m_VkCache) {
            const auto& vs = m_VkCache->GetStatistics();
            s.TotalPipelines = vs.TotalPipelines;
            s.CacheHits = vs.CacheHits;
            s.CacheMisses = vs.CacheMisses;
            s.CacheSizeBytes = vs.CacheSize;
            s.AverageCreationTimeMs = vs.AverageCreationTime;
            s.AverageCachedCreationTimeMs = vs.AverageCachedCreationTime;
        }
        return s;
    }

    void* GetNativeHandle() const override {
        // Portable: do not attempt to cast non-dispatchable handles to pointers
        // If backends need the native handle, they should access VulkanPipelineCache directly for now.
        return nullptr;
    }

private:
    VulkanPipelineCache* m_VkCache = nullptr; // non-owning
};

}} // namespace GameEngine::Rendering

