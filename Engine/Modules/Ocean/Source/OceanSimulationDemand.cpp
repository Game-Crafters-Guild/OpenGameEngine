#include "Ocean/OceanSimulationDemand.h"

namespace GameEngine::Ocean
{

void OceanSimulationDemand::Update(Clock::time_point now)
{
    if (!m_SurfaceQueried.exchange(false, std::memory_order_relaxed))
        return;
    m_HasQuery = true;
    m_LastQuery = now;
}

void OceanSimulationDemand::NoteWaveDispatch(Clock::time_point now)
{
    m_HasWaveDispatch = true;
    m_LastWaveDispatch = now;
}

bool OceanSimulationDemand::HasRecentSurfaceQueries(Clock::time_point now) const
{
    return m_HasQuery && now - m_LastQuery < kHold;
}

bool OceanSimulationDemand::IsWaveDataStale(Clock::time_point now) const
{
    return !m_HasWaveDispatch || now - m_LastWaveDispatch >= kHold;
}

} // namespace GameEngine::Ocean
