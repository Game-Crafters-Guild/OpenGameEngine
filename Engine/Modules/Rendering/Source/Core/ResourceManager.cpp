#include "Rendering/Core/ResourceManager.h"
#include "Rendering/Core/Device.h"
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace GameEngine::Rendering {

    // Resource info structures
    struct BufferInfo {
        BufferDesc desc;
        uint32_t refCount = 1;
        size_t memorySize = 0;
        std::string debugName;
        bool isValid = true;
    };

    struct TextureInfo {
        TextureDesc desc;
        uint32_t refCount = 1;
        size_t memorySize = 0;
        std::string debugName;
        bool isValid = true;
    };

    struct SamplerInfo {
        SamplerDesc desc;
        uint32_t refCount = 1;
        std::string debugName;
        bool isValid = true;
    };

    /**
     * @brief Real ResourceManager implementation
     *
     * Manages GPU resources with proper tracking, reference counting, and leak detection.
     */
    class ResourceManager::Impl {
    public:
        explicit Impl(IDevice* device) : m_Device(device) {}

        IDevice* m_Device;
        mutable std::mutex m_Mutex;

        // Resource tracking
        std::unordered_map<BufferHandle, BufferInfo> m_Buffers;
        std::unordered_map<TextureHandle, TextureInfo> m_Textures;
        std::unordered_map<SamplerHandle, SamplerInfo> m_Samplers;

        // Statistics
        std::atomic<uint32_t> m_TotalBuffers{0};
        std::atomic<uint32_t> m_TotalTextures{0};
        std::atomic<uint32_t> m_TotalSamplers{0};
        std::atomic<size_t> m_TotalMemoryUsed{0};


    };

    ResourceManager::ResourceManager(IDevice* device)
        : m_Impl(std::make_unique<Impl>(device)) {
        std::cout << "ResourceManager: Created with real implementation" << std::endl;
    }

    ResourceManager::~ResourceManager() {
        // Check for resource leaks
        CheckForLeaks();
        std::cout << "ResourceManager: Destroyed" << std::endl;
    }

    void ResourceManager::TrackBuffer(BufferHandle handle, const BufferDesc& desc) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        BufferInfo info;
        info.desc = desc;
        info.memorySize = desc.size;
        info.debugName = desc.debugName ? desc.debugName : "unnamed_buffer";

        m_Impl->m_Buffers[handle] = info;
        m_Impl->m_TotalBuffers++;
        m_Impl->m_TotalMemoryUsed += desc.size;

        std::cout << "ResourceManager: Tracking buffer '" << info.debugName
                  << "' handle=" << handle << " size=" << desc.size << " bytes" << std::endl;
    }

    void ResourceManager::TrackTexture(TextureHandle handle, const TextureDesc& desc) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        TextureInfo info;
        info.desc = desc;
        // Estimate memory usage
        uint32_t bytesPerPixel = 4; // Assume RGBA8 for now
        info.memorySize = desc.width * desc.height * bytesPerPixel;
        info.debugName = desc.debugName ? desc.debugName : "unnamed_texture";

        m_Impl->m_Textures[handle] = info;
        m_Impl->m_TotalTextures++;
        m_Impl->m_TotalMemoryUsed += info.memorySize;

        std::cout << "ResourceManager: Tracking texture '" << info.debugName
                  << "' handle=" << handle << " size=" << desc.width << "x" << desc.height << std::endl;
    }

    void ResourceManager::TrackSampler(SamplerHandle handle, const SamplerDesc& desc) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        SamplerInfo info;
        info.desc = desc;
        info.debugName = desc.debugName ? desc.debugName : "unnamed_sampler";

        m_Impl->m_Samplers[handle] = info;
        m_Impl->m_TotalSamplers++;

        std::cout << "ResourceManager: Tracking sampler '" << info.debugName
                  << "' handle=" << handle << std::endl;
    }

    void ResourceManager::UntrackBuffer(BufferHandle handle) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        auto it = m_Impl->m_Buffers.find(handle);
        if (it != m_Impl->m_Buffers.end()) {
            m_Impl->m_TotalMemoryUsed -= it->second.memorySize;
            m_Impl->m_TotalBuffers--;
            std::cout << "ResourceManager: Untracking buffer '" << it->second.debugName
                      << "' handle=" << handle << std::endl;
            m_Impl->m_Buffers.erase(it);
        }
    }

    void ResourceManager::UntrackTexture(TextureHandle handle) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        auto it = m_Impl->m_Textures.find(handle);
        if (it != m_Impl->m_Textures.end()) {
            m_Impl->m_TotalMemoryUsed -= it->second.memorySize;
            m_Impl->m_TotalTextures--;
            std::cout << "ResourceManager: Untracking texture '" << it->second.debugName
                      << "' handle=" << handle << std::endl;
            m_Impl->m_Textures.erase(it);
        }
    }

    void ResourceManager::UntrackSampler(SamplerHandle handle) {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        auto it = m_Impl->m_Samplers.find(handle);
        if (it != m_Impl->m_Samplers.end()) {
            m_Impl->m_TotalSamplers--;
            std::cout << "ResourceManager: Untracking sampler '" << it->second.debugName
                      << "' handle=" << handle << std::endl;
            m_Impl->m_Samplers.erase(it);
        }
    }

    ResourceStats ResourceManager::GetStats() const {
        ResourceStats stats;
        stats.TotalBuffers = m_Impl->m_TotalBuffers.load();
        stats.TotalTextures = m_Impl->m_TotalTextures.load();
        stats.TotalSamplers = m_Impl->m_TotalSamplers.load();
        stats.TotalMemoryUsed = m_Impl->m_TotalMemoryUsed.load();
        return stats;
    }

    void ResourceManager::PrintResourceReport() const {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        std::cout << "\n=== Resource Manager Report ===" << std::endl;
        std::cout << "Buffers: " << m_Impl->m_TotalBuffers.load() << std::endl;
        std::cout << "Textures: " << m_Impl->m_TotalTextures.load() << std::endl;
        std::cout << "Samplers: " << m_Impl->m_TotalSamplers.load() << std::endl;
        std::cout << "Total Memory: " << (m_Impl->m_TotalMemoryUsed.load() / 1024 / 1024) << " MB" << std::endl;
        std::cout << "===============================" << std::endl;
    }

    void ResourceManager::CheckForLeaks() const {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

        bool hasLeaks = false;

        if (!m_Impl->m_Buffers.empty()) {
            std::cout << "⚠️  RESOURCE LEAK: " << m_Impl->m_Buffers.size() << " buffers not destroyed:" << std::endl;
            for (const auto& [handle, info] : m_Impl->m_Buffers) {
                std::cout << "  - Buffer '" << info.debugName << "' handle=" << handle << std::endl;
            }
            hasLeaks = true;
        }

        if (!m_Impl->m_Textures.empty()) {
            std::cout << "⚠️  RESOURCE LEAK: " << m_Impl->m_Textures.size() << " textures not destroyed:" << std::endl;
            for (const auto& [handle, info] : m_Impl->m_Textures) {
                std::cout << "  - Texture '" << info.debugName << "' handle=" << handle << std::endl;
            }
            hasLeaks = true;
        }

        if (!m_Impl->m_Samplers.empty()) {
            std::cout << "⚠️  RESOURCE LEAK: " << m_Impl->m_Samplers.size() << " samplers not destroyed:" << std::endl;
            for (const auto& [handle, info] : m_Impl->m_Samplers) {
                std::cout << "  - Sampler '" << info.debugName << "' handle=" << handle << std::endl;
            }
            hasLeaks = true;
        }

        if (!hasLeaks) {
            std::cout << "✅ No resource leaks detected" << std::endl;
        }
    }

    std::vector<BufferHandle> ResourceManager::GetLiveBuffers() const {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);
        std::vector<BufferHandle> out;
        out.reserve(m_Impl->m_Buffers.size());
        for (const auto& kv : m_Impl->m_Buffers) out.push_back(kv.first);
        return out;
    }

    std::vector<TextureHandle> ResourceManager::GetLiveTextures() const {
        std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);
        std::vector<TextureHandle> out;
        out.reserve(m_Impl->m_Textures.size());
        for (const auto& kv : m_Impl->m_Textures) out.push_back(kv.first);
        return out;
    }

} // namespace GameEngine::Rendering
