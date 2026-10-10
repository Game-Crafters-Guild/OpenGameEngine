#pragma once

#include <cstddef>
#include <cstdint>

namespace GameEngine { namespace Rendering {

// Backend-agnostic pipeline cache interface (wraps native API pipeline cache objects)
class IPipelineCache {
public:
    struct Statistics {
        uint32_t TotalPipelines = 0;
        uint32_t CacheHits = 0;
        uint32_t CacheMisses = 0;
        size_t   CacheSizeBytes = 0;
        double   AverageCreationTimeMs = 0.0;
        double   AverageCachedCreationTimeMs = 0.0;
    };

    virtual ~IPipelineCache() = default;

    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;

    virtual bool SaveCache() = 0;
    virtual bool LoadCache() = 0;
    virtual void ClearCache() = 0;

    virtual bool IsValid() const = 0;
    virtual Statistics GetStatistics() const = 0;

    // Opaque native handle access for backend-internal use.
    // Consumers must cast appropriately based on backend.
    virtual void* GetNativeHandle() const = 0;
};

}} // namespace GameEngine::Rendering

