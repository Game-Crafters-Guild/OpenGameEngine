#pragma once

#include "Ocean/OceanQuery.h"

#include <cstdint>
#include <memory>

namespace GameEngine::Ocean
{

enum class OceanCollisionProviderMode : uint32
{
    Auto = 0,
    GPUQueries = 1,
    BakedFFTCPU = 2,
    AnalyticGerstner = 3,
    None = 4,
};

enum class OceanQueryStatus : uint32
{
    Success = 0,
    NotReady = 1u << 0u,
    InvalidArgument = 1u << 1u,
    ProviderUnavailable = 1u << 2u,
    CapacityExceeded = 1u << 3u,
    AssetMismatch = 1u << 4u,
    Cancelled = 1u << 5u,
};

inline OceanQueryStatus operator|(OceanQueryStatus a, OceanQueryStatus b)
{
    return static_cast<OceanQueryStatus>(static_cast<uint32>(a) | static_cast<uint32>(b));
}

inline bool OceanQuerySucceeded(OceanQueryStatus status)
{
    return status == OceanQueryStatus::Success;
}

enum class OceanQueryField : uint32
{
    Height = 1u << 0u,
    Displacement = 1u << 1u,
    Normal = 1u << 2u,
    Velocity = 1u << 3u,
    Flow = 1u << 4u,
    WaterDepth = 1u << 5u,
    AllSurface = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u) | (1u << 5u),
    All = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u) | (1u << 4u) |
          (1u << 5u),
};

inline OceanQueryField operator|(OceanQueryField a, OceanQueryField b)
{
    return static_cast<OceanQueryField>(static_cast<uint32>(a) | static_cast<uint32>(b));
}

inline bool OceanQueryHasField(OceanQueryField fields, OceanQueryField field)
{
    return (static_cast<uint32>(fields) & static_cast<uint32>(field)) != 0u;
}

using OceanCollisionOwnerId = uint64;
using OceanCollisionQueryHandle = uint64;

struct OceanCollisionQueryDesc
{
    OceanCollisionOwnerId Owner = 0;
    float32 MinimumSpatialLength = 0.0f;
    const OceanSurfaceQueryPoint* Points = nullptr;
    uint32 Count = 0;
    OceanQueryField Fields = OceanQueryField::AllSurface;
};

// Owner-keyed, non-blocking collision provider contract. Submit updates an
// owner's persistent query registration. Update advances provider work; callers
// retrieve the newest completed result without stalling the render/physics thread.
class IOceanCollisionProvider
{
public:
    virtual ~IOceanCollisionProvider() = default;

    virtual OceanCollisionProviderMode GetMode() const = 0;
    virtual OceanCollisionQueryHandle Submit(const OceanCollisionQueryDesc& query) = 0;
    virtual OceanQueryStatus GetStatus(OceanCollisionQueryHandle handle) const = 0;
    virtual bool CopyResults(OceanCollisionQueryHandle handle,
                             OceanSurfaceSample* outSurface,
                             OceanCurrentSample* outFlow,
                             uint32 capacity,
                             uint32* outCount = nullptr) const = 0;
    virtual void Update(uint32 maxBatches = 0u) = 0;
    virtual bool Cancel(OceanCollisionQueryHandle handle) = 0;
    virtual void Clear() = 0;
};

class OceanRenderFeature;
class OceanFFTCollisionAsset;

// Feature-backed provider used by the runtime router. It immediately supplies
// the existing analytic/baked-height fallback contract and is structured so the
// arbitrary-point GPU provider can replace the update stage without changing
// gameplay APIs.
class OceanFeatureCollisionProvider final : public IOceanCollisionProvider
{
public:
    explicit OceanFeatureCollisionProvider(OceanRenderFeature& feature);
    ~OceanFeatureCollisionProvider() override;

    OceanCollisionProviderMode GetMode() const override;
    void SetMode(OceanCollisionProviderMode mode);
    void SetBakedFFTAsset(std::shared_ptr<const OceanFFTCollisionAsset> asset);
    std::shared_ptr<const OceanFFTCollisionAsset> GetBakedFFTAsset() const;
    void SetExpectedSpectrumHash(uint64 hash);
    uint64 GetExpectedSpectrumHash() const;
    void SetMaxQueryPoints(uint32 count);
    uint32 GetMaxQueryPoints() const;
    void SetFallbackPolicy(bool allowGPUQueries, bool allowBakedFFT,
                           bool allowGerstnerFallback,
                           float32 defaultMinimumSpatialLength = 0.0f);
    OceanCollisionQueryHandle Submit(const OceanCollisionQueryDesc& query) override;
    OceanQueryStatus GetStatus(OceanCollisionQueryHandle handle) const override;
    bool CopyResults(OceanCollisionQueryHandle handle,
                     OceanSurfaceSample* outSurface,
                     OceanCurrentSample* outFlow,
                     uint32 capacity,
                     uint32* outCount = nullptr) const override;
    void Update(uint32 maxBatches = 0u) override;
    bool Cancel(OceanCollisionQueryHandle handle) override;
    void Clear() override;

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

} // namespace GameEngine::Ocean
