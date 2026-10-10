#pragma once

#include "Ocean/OceanCollisionProvider.h"

#include <memory>

namespace GameEngine::Ocean
{

struct OceanRayTraceDesc
{
    OceanCollisionOwnerId Owner = 0u;
    float32 Origin[3] = {0.0f, 0.0f, 0.0f};
    float32 Direction[3] = {0.0f, -1.0f, 0.0f};
    float32 MaximumDistance = 1000.0f;
    float32 MinimumSpatialLength = 0.0f;
    uint32 SampleCount = 32u;
};

struct OceanRayTraceResult
{
    bool Hit = false;
    float32 Distance = 0.0f;
    float32 PositionWS[3] = {0.0f, 0.0f, 0.0f};
    float32 NormalWS[3] = {0.0f, 1.0f, 0.0f};
    OceanSurfaceSample Surface{};
};

// Persistent non-blocking ray helper. Submit registers evenly spaced collision
// samples; TryResolve consumes the newest completed provider batch and locates
// the first signed-height crossing without ever waiting for render readback.
class OceanRayTracer
{
public:
    explicit OceanRayTracer(IOceanCollisionProvider& provider);
    ~OceanRayTracer();

    OceanCollisionQueryHandle Submit(const OceanRayTraceDesc& ray);
    OceanQueryStatus GetStatus(OceanCollisionQueryHandle handle) const;
    bool TryResolve(OceanCollisionQueryHandle handle, OceanRayTraceResult& outResult) const;
    bool Cancel(OceanCollisionQueryHandle handle);
    void Clear();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Ocean
