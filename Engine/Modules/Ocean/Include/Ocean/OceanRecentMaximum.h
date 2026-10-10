#pragma once

#include <algorithm>
#include <chrono>

namespace GameEngine::Ocean
{

/// Running maximum of a sampled value over at least the last `Window`: two
/// back-to-back buckets of one window each, so Get() covers between one and two
/// windows of samples without storing them. A gap longer than two windows
/// forgets everything older.
class OceanRecentMaximum
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::duration Window = std::chrono::seconds(1);

    void Add(float value, Clock::time_point now)
    {
        Roll(now);
        m_Current = m_HasSamples ? std::max(m_Current, value) : value;
        m_HasSamples = true;
    }

    /// False until the first sample, and once every sample is older than two windows.
    bool Get(Clock::time_point now, float& outMaximum)
    {
        Roll(now);
        if (!m_HasSamples)
            return false;
        outMaximum = std::max(m_Current, m_Previous);
        return true;
    }

private:
    void Roll(Clock::time_point now)
    {
        if (!m_HasSamples)
        {
            m_BucketStart = now;
            return;
        }
        if (now - m_BucketStart < Window)
            return;
        const bool adjacent = now - m_BucketStart < 2 * Window;
        m_Previous = adjacent ? m_Current : 0.0f;
        m_HasSamples = adjacent;
        m_Current = 0.0f;
        m_BucketStart = now;
    }

    Clock::time_point m_BucketStart{};
    float m_Current = 0.0f;
    float m_Previous = 0.0f;
    bool m_HasSamples = false;
};

} // namespace GameEngine::Ocean
