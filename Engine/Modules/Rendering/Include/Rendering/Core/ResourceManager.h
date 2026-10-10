#pragma once

#include "Device.h"
#include <memory>
#include <cstdint>

namespace GameEngine {
namespace Rendering {

    // Forward declarations
    class IDevice;

    /**
     * @brief Resource statistics
     */
    struct ResourceStats {
        uint32_t TotalBuffers = 0;
        uint32_t TotalTextures = 0;
        uint32_t TotalSamplers = 0;
        size_t TotalMemoryUsed = 0;
    };

    /**
     * @brief Resource Manager for tracking GPU resources
     *
     * Provides comprehensive resource tracking with reference counting,
     * leak detection, and memory usage monitoring.
     */
    class ResourceManager {
    public:
        explicit ResourceManager(IDevice* device);
        ~ResourceManager();

        // Resource tracking
        void TrackBuffer(BufferHandle handle, const BufferDesc& desc);
        void TrackTexture(TextureHandle handle, const TextureDesc& desc);
        void TrackSampler(SamplerHandle handle, const SamplerDesc& desc);

        // Resource untracking
        void UntrackBuffer(BufferHandle handle);
        void UntrackTexture(TextureHandle handle);
        void UntrackSampler(SamplerHandle handle);

        // Statistics and debugging
        ResourceStats GetStats() const;
        void PrintResourceReport() const;
        void CheckForLeaks() const;

        // Enumeration helpers
        std::vector<BufferHandle> GetLiveBuffers() const;
        std::vector<TextureHandle> GetLiveTextures() const;

    private:
        class Impl;
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Rendering
} // namespace GameEngine
