#pragma once

#include "Types/Types.h"

#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

struct OceanTimeSample
{
    float32 Time = 0.0f;
    float32 DeltaTime = 0.0f;
    bool Paused = false;
};

class IOceanTimeProvider
{
public:
    virtual ~IOceanTimeProvider() = default;
    virtual OceanTimeSample Sample(float32 engineTime, float32 engineDeltaTime) = 0;
    virtual void Reset(float32 engineTime) = 0;
};

class OceanDefaultTimeProvider final : public IOceanTimeProvider
{
public:
    OceanTimeSample Sample(float32 engineTime, float32 engineDeltaTime) override;
    void Reset(float32 engineTime) override;
};

class OceanCustomTimeProvider final : public IOceanTimeProvider
{
public:
    void SetScale(float32 scale) { m_Scale = scale; }
    void SetOffset(float32 offset) { m_Offset = offset; }
    void SetFixedTime(bool enabled, float32 time) { m_Fixed = enabled; m_FixedTime = time; }
    OceanTimeSample Sample(float32 engineTime, float32 engineDeltaTime) override;
    void Reset(float32 engineTime) override;

private:
    float32 m_Scale = 1.0f;
    float32 m_Offset = 0.0f;
    float32 m_FixedTime = 0.0f;
    bool m_Fixed = false;
};

class OceanNetworkOffsetTimeProvider final : public IOceanTimeProvider
{
public:
    void SetNetworkOffset(float32 offset) { m_Offset = offset; }
    void SetRate(float32 rate) { m_Rate = rate; }
    OceanTimeSample Sample(float32 engineTime, float32 engineDeltaTime) override;
    void Reset(float32 engineTime) override;

private:
    float32 m_Offset = 0.0f;
    float32 m_Rate = 1.0f;
};

class OceanTimelineTimeProvider final : public IOceanTimeProvider
{
public:
    void SetTimelineTime(float32 time, bool playing, float32 playbackRate = 1.0f);
    OceanTimeSample Sample(float32 engineTime, float32 engineDeltaTime) override;
    void Reset(float32 engineTime) override;

private:
    float32 m_Time = 0.0f;
    float32 m_Rate = 1.0f;
    bool m_Playing = false;
};

struct OceanOriginShiftEvent
{
    float32 ShiftX = 0.0f;
    float32 ShiftY = 0.0f;
    float32 ShiftZ = 0.0f;
    bool Teleport = false;
    uint64 Sequence = 0u;
};

class IOceanOriginShiftListener
{
public:
    virtual ~IOceanOriginShiftListener() = default;
    virtual void OnOceanOriginShift(const OceanOriginShiftEvent& event) = 0;
};

class OceanOriginShiftNotifier
{
public:
    void AddListener(IOceanOriginShiftListener* listener);
    void RemoveListener(IOceanOriginShiftListener* listener);
    OceanOriginShiftEvent Notify(float32 shiftX, float32 shiftY, float32 shiftZ,
                                 bool teleport);
    uint64 GetSequence() const { return m_Sequence; }

private:
    std::vector<IOceanOriginShiftListener*> m_Listeners;
    uint64 m_Sequence = 0u;
};

} // namespace GameEngine::Ocean
