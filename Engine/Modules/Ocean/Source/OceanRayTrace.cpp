#include "Ocean/OceanRayTrace.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Ocean
{

struct OceanRayTracer::Impl
{
    struct PendingRay
    {
        OceanRayTraceDesc Desc{};
        std::vector<OceanSurfaceQueryPoint> Points;
    };

    explicit Impl(IOceanCollisionProvider& inProvider) : Provider(inProvider) {}

    IOceanCollisionProvider& Provider;
    mutable std::mutex Mutex;
    std::unordered_map<OceanCollisionQueryHandle, PendingRay> Rays;
};

OceanRayTracer::OceanRayTracer(IOceanCollisionProvider& provider)
    : m_Impl(std::make_unique<Impl>(provider))
{
}

OceanRayTracer::~OceanRayTracer() = default;

OceanCollisionQueryHandle OceanRayTracer::Submit(const OceanRayTraceDesc& ray)
{
    if (ray.Owner == 0u || ray.SampleCount < 2u || ray.MaximumDistance <= 0.0f)
        return 0u;
    const float32 length = std::sqrt(ray.Direction[0] * ray.Direction[0] +
                                     ray.Direction[1] * ray.Direction[1] +
                                     ray.Direction[2] * ray.Direction[2]);
    if (length <= 1e-6f)
        return 0u;

    Impl::PendingRay pending{};
    pending.Desc = ray;
    for (float32& component : pending.Desc.Direction)
        component /= length;
    pending.Desc.SampleCount = std::clamp(ray.SampleCount, 2u, 4096u);
    pending.Points.resize(pending.Desc.SampleCount);
    for (uint32 i = 0u; i < pending.Desc.SampleCount; ++i)
    {
        const float32 t = static_cast<float32>(i) /
                          static_cast<float32>(pending.Desc.SampleCount - 1u);
        const float32 distance = pending.Desc.MaximumDistance * t;
        pending.Points[i].X = pending.Desc.Origin[0] + pending.Desc.Direction[0] * distance;
        pending.Points[i].Z = pending.Desc.Origin[2] + pending.Desc.Direction[2] * distance;
    }

    OceanCollisionQueryDesc query{};
    query.Owner = ray.Owner;
    query.MinimumSpatialLength = ray.MinimumSpatialLength;
    query.Points = pending.Points.data();
    query.Count = static_cast<uint32>(pending.Points.size());
    query.Fields = OceanQueryField::Height | OceanQueryField::Normal |
                   OceanQueryField::Displacement;
    const OceanCollisionQueryHandle handle = m_Impl->Provider.Submit(query);
    if (handle == 0u)
        return 0u;
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    m_Impl->Rays[handle] = std::move(pending);
    return handle;
}

OceanQueryStatus OceanRayTracer::GetStatus(OceanCollisionQueryHandle handle) const
{
    return m_Impl->Provider.GetStatus(handle);
}

bool OceanRayTracer::TryResolve(OceanCollisionQueryHandle handle,
                                OceanRayTraceResult& outResult) const
{
    Impl::PendingRay ray{};
    {
        std::lock_guard<std::mutex> lock(m_Impl->Mutex);
        const auto found = m_Impl->Rays.find(handle);
        if (found == m_Impl->Rays.end())
            return false;
        ray = found->second;
    }

    std::vector<OceanSurfaceSample> samples(ray.Points.size());
    uint32 count = 0u;
    if (!m_Impl->Provider.CopyResults(handle, samples.data(), nullptr,
                                      static_cast<uint32>(samples.size()), &count) ||
        count != samples.size())
    {
        return false;
    }

    outResult = {};
    const auto rayY = [&](uint32 index) {
        const float32 t = static_cast<float32>(index) /
                          static_cast<float32>(samples.size() - 1u);
        return ray.Desc.Origin[1] + ray.Desc.Direction[1] * ray.Desc.MaximumDistance * t;
    };
    for (uint32 i = 1u; i < count; ++i)
    {
        if (!samples[i - 1u].Valid || !samples[i].Valid)
            continue;
        const float32 d0 = rayY(i - 1u) - samples[i - 1u].Height;
        const float32 d1 = rayY(i) - samples[i].Height;
        if ((d0 > 0.0f && d1 > 0.0f) || (d0 < 0.0f && d1 < 0.0f))
            continue;
        const float32 denom = d0 - d1;
        const float32 blend = std::abs(denom) > 1e-6f
                                  ? std::clamp(d0 / denom, 0.0f, 1.0f)
                                  : 0.0f;
        const float32 indexT = (static_cast<float32>(i - 1u) + blend) /
                               static_cast<float32>(count - 1u);
        outResult.Hit = true;
        outResult.Distance = ray.Desc.MaximumDistance * indexT;
        for (uint32 axis = 0u; axis < 3u; ++axis)
        {
            outResult.PositionWS[axis] = ray.Desc.Origin[axis] +
                                         ray.Desc.Direction[axis] * outResult.Distance;
            outResult.NormalWS[axis] = samples[i - 1u].NormalWS[axis] +
                                       (samples[i].NormalWS[axis] -
                                        samples[i - 1u].NormalWS[axis]) * blend;
        }
        outResult.Surface = blend < 0.5f ? samples[i - 1u] : samples[i];
        outResult.PositionWS[1] = samples[i - 1u].Height +
                                  (samples[i].Height - samples[i - 1u].Height) * blend;
        return true;
    }
    return true; // resolved successfully, but the segment did not hit water
}

bool OceanRayTracer::Cancel(OceanCollisionQueryHandle handle)
{
    {
        std::lock_guard<std::mutex> lock(m_Impl->Mutex);
        m_Impl->Rays.erase(handle);
    }
    return m_Impl->Provider.Cancel(handle);
}

void OceanRayTracer::Clear()
{
    std::lock_guard<std::mutex> lock(m_Impl->Mutex);
    m_Impl->Rays.clear();
    m_Impl->Provider.Clear();
}

} // namespace GameEngine::Ocean
