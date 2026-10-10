#include "SplineECS/SplineService.h"
#include "Spline/SplineEvaluator.h"

#include <cassert>
#include <mutex>

namespace GameEngine::SplineECS
{

std::unique_ptr<SplineService> SplineService::s_Instance;

void SplineService::Initialize()
{
    assert(!s_Instance && "SplineService already initialized");
    s_Instance = std::make_unique<SplineService>();
}

void SplineService::Shutdown()
{
    s_Instance.reset();
}

SplineService& SplineService::Get()
{
    assert(s_Instance && "SplineService not initialized");
    return *s_Instance;
}

SplineService* SplineService::TryGet()
{
    return s_Instance.get();
}

bool SplineService::IsInitialized()
{
    return s_Instance != nullptr;
}

SplineHandle SplineService::CreateSpline(Spline::SplineType type, bool closed)
{
    std::unique_lock lock(m_Mutex);

    Spline::SplineData data{};
    data.Type = type;
    data.Closed = closed;
    m_EditEpoch.fetch_add(1, std::memory_order_relaxed);
    return m_Splines.Create(std::move(data));
}

void SplineService::DestroySpline(SplineHandle handle)
{
    std::unique_lock lock(m_Mutex);

    if (!m_Splines.IsValid(handle))
        return;
    m_Splines.Destroy(handle);
    m_EditEpoch.fetch_add(1, std::memory_order_relaxed);
}

Spline::SplineData* SplineService::GetSplineData(SplineHandle handle)
{
    std::shared_lock lock(m_Mutex);

    if (!m_Splines.IsValid(handle))
        return nullptr;
    return &m_Splines[handle];
}

const Spline::SplineData* SplineService::GetSplineData(SplineHandle handle) const
{
    std::shared_lock lock(m_Mutex);

    if (!m_Splines.IsValid(handle))
        return nullptr;
    return &m_Splines[handle];
}

bool SplineService::IsValid(SplineHandle handle) const
{
    std::shared_lock lock(m_Mutex);
    return m_Splines.IsValid(handle);
}

void SplineService::RebuildCache(SplineHandle handle)
{
    std::unique_lock lock(m_Mutex);

    if (!m_Splines.IsValid(handle))
        return;
    Spline::RebuildSplineCache(m_Splines[handle]);
    m_EditEpoch.fetch_add(1, std::memory_order_relaxed);
}

uint32 SplineService::GetActiveSplineCount() const
{
    std::shared_lock lock(m_Mutex);
    return static_cast<uint32>(m_Splines.Size());
}

} // namespace GameEngine::SplineECS
