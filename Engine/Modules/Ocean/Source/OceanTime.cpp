#include "Ocean/OceanTime.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{

OceanTimeSample OceanDefaultTimeProvider::Sample(float32 engineTime, float32 engineDeltaTime)
{
    return {engineTime, std::max(engineDeltaTime, 0.0f), engineDeltaTime == 0.0f};
}

void OceanDefaultTimeProvider::Reset(float32) {}

OceanTimeSample OceanCustomTimeProvider::Sample(float32 engineTime, float32 engineDeltaTime)
{
    if (m_Fixed)
        return {m_FixedTime, 0.0f, true};
    const float32 scale = std::max(m_Scale, 0.0f);
    return {engineTime * scale + m_Offset,
            std::max(engineDeltaTime, 0.0f) * scale,
            scale == 0.0f || engineDeltaTime == 0.0f};
}

void OceanCustomTimeProvider::Reset(float32) {}

OceanTimeSample OceanNetworkOffsetTimeProvider::Sample(float32 engineTime,
                                                       float32 engineDeltaTime)
{
    const float32 rate = std::max(m_Rate, 0.0f);
    return {engineTime * rate + m_Offset,
            std::max(engineDeltaTime, 0.0f) * rate,
            rate == 0.0f || engineDeltaTime == 0.0f};
}

void OceanNetworkOffsetTimeProvider::Reset(float32) {}

void OceanTimelineTimeProvider::SetTimelineTime(float32 time, bool playing,
                                                float32 playbackRate)
{
    m_Time = std::isfinite(time) ? time : 0.0f;
    m_Playing = playing;
    m_Rate = std::max(playbackRate, 0.0f);
}

OceanTimeSample OceanTimelineTimeProvider::Sample(float32, float32 engineDeltaTime)
{
    const float32 delta = m_Playing ? std::max(engineDeltaTime, 0.0f) * m_Rate : 0.0f;
    const OceanTimeSample result{m_Time, delta, !m_Playing || delta == 0.0f};
    m_Time += delta;
    return result;
}

void OceanTimelineTimeProvider::Reset(float32) {}

void OceanOriginShiftNotifier::AddListener(IOceanOriginShiftListener* listener)
{
    if (!listener || std::find(m_Listeners.begin(), m_Listeners.end(), listener) != m_Listeners.end())
        return;
    m_Listeners.push_back(listener);
}

void OceanOriginShiftNotifier::RemoveListener(IOceanOriginShiftListener* listener)
{
    m_Listeners.erase(std::remove(m_Listeners.begin(), m_Listeners.end(), listener),
                      m_Listeners.end());
}

OceanOriginShiftEvent OceanOriginShiftNotifier::Notify(float32 shiftX, float32 shiftY,
                                                       float32 shiftZ, bool teleport)
{
    OceanOriginShiftEvent event{shiftX, shiftY, shiftZ, teleport, ++m_Sequence};
    const std::vector<IOceanOriginShiftListener*> listeners = m_Listeners;
    for (IOceanOriginShiftListener* listener : listeners)
        if (listener)
            listener->OnOceanOriginShift(event);
    return event;
}

} // namespace GameEngine::Ocean
