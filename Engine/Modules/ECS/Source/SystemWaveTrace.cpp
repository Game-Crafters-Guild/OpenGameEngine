#include "ECS/SystemWaveTrace.h"

#include <algorithm>
#include <chrono>

#if defined(__APPLE__)
#include <os/signpost.h>
#endif

namespace GameEngine::ECS
{

namespace
{
#if defined(__APPLE__)
os_log_t SignpostLog()
{
    static const os_log_t log = os_log_create("com.gameengine.ecs", "SystemWaves");
    return log;
}

// A fresh interval id while a recording has the log enabled; 0 otherwise.
uint64 BeginSignpostId()
{
    const os_log_t log = SignpostLog();
    return os_signpost_enabled(log) ? os_signpost_id_generate(log) : 0;
}
#endif
} // namespace

uint64 SystemWaveTrace::NowNs()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

SystemWaveTrace::SystemWaveTrace() = default;
SystemWaveTrace::~SystemWaveTrace() = default;

void SystemWaveTrace::SetEnabled(bool enable)
{
    if (enable == IsEnabled())
        return;
    if (enable)
    {
        if (!m_Frames)
            m_Frames = std::make_unique<Frame[]>(kFrameCapacity);
        m_FramesRecorded = 0;
        m_ClearPending = false;
    }
    m_Enabled.store(enable, std::memory_order_relaxed);
}

void SystemWaveTrace::Clear()
{
    m_ClearPending = true;
}

uint64 SystemWaveTrace::GetFramesRecorded() const
{
    return m_ClearPending ? 0 : m_FramesRecorded;
}

void SystemWaveTrace::CopyFrames(std::vector<Frame>& out, size_t maxFrames) const
{
    out.clear();
    if (!m_Frames || m_ClearPending)
        return;
    const uint64 available = std::min<uint64>(m_FramesRecorded, kFrameCapacity);
    const uint64 count = std::min<uint64>(available, maxFrames);
    out.reserve(static_cast<size_t>(count));
    for (uint64 index = m_FramesRecorded - count; index < m_FramesRecorded; ++index)
        out.push_back(m_Frames[index % kFrameCapacity]);
}

SystemWaveTrace::Frame& SystemWaveTrace::BeginFrame(uint64 publishedJobs)
{
    if (m_ClearPending)
    {
        m_FramesRecorded = 0;
        m_ClearPending = false;
    }
    Frame& frame = m_Frames[m_FramesRecorded % kFrameCapacity];
    frame.Index = m_FramesRecorded;
    frame.PublishedAtBegin = publishedJobs;
    frame.PublishedAtEnd = publishedJobs;
    frame.WaveForkPublishes = 0;
    frame.WaveCount = 0;
    frame.SystemCount = 0;
    frame.DroppedWaves = 0;
    frame.DroppedSystems = 0;
    frame.BeginNs = NowNs();
    frame.EndNs = frame.BeginNs;
    return frame;
}

void SystemWaveTrace::EndFrame(Frame& frame, uint64 publishedJobs)
{
    frame.EndNs = NowNs();
    frame.PublishedAtEnd = publishedJobs;
    if (m_ClearPending)
    {
        // The system set changed during this frame: its slots may name a
        // different system now, so the frame is dropped with the rest.
        m_FramesRecorded = 0;
        m_ClearPending = false;
        return;
    }
    ++m_FramesRecorded;
}

SystemWaveTrace::WaveSample* SystemWaveTrace::BeginWave(Frame& frame, uint32 planWave)
{
    if (frame.WaveCount >= kMaxWavesPerFrame)
    {
        ++frame.DroppedWaves;
        return nullptr;
    }
    WaveSample& wave = frame.Waves[frame.WaveCount++];
    wave = WaveSample{};
    wave.PlanWave = planWave;
    wave.FirstSystem = frame.SystemCount;
#if defined(__APPLE__)
    m_WaveSignpost = BeginSignpostId();
    if (m_WaveSignpost != 0)
        os_signpost_interval_begin(SignpostLog(), m_WaveSignpost, "Wave", "wave %u", planWave);
#endif
    wave.BeginNs = NowNs();
    return &wave;
}

void SystemWaveTrace::EndWave(Frame& frame, WaveSample& wave)
{
    wave.EndNs = NowNs();
    wave.SystemCount = static_cast<uint16>(frame.SystemCount - wave.FirstSystem);
#if defined(__APPLE__)
    if (m_WaveSignpost != 0)
        os_signpost_interval_end(SignpostLog(), m_WaveSignpost, "Wave", "%u systems, %u forks",
                                 static_cast<uint32>(wave.SystemCount), static_cast<uint32>(wave.Forks));
    m_WaveSignpost = 0;
#endif
}

uint16 SystemWaveTrace::AddSystem(Frame& frame, const WaveSample& wave, uint32 slot)
{
    if (frame.SystemCount >= kMaxSystemsPerFrame)
    {
        ++frame.DroppedSystems;
        return kNoSample;
    }
    const uint16 index = frame.SystemCount++;
    SystemSample& sample = frame.Systems[index];
    sample = SystemSample{};
    sample.Slot = slot;
    sample.Wave = static_cast<uint16>(&wave - frame.Waves);
    return index;
}

SystemWaveTrace::BodyInterval::BodyInterval([[maybe_unused]] const char* systemName)
{
#if defined(__APPLE__)
    m_Id = BeginSignpostId();
    if (m_Id != 0)
        os_signpost_interval_begin(SignpostLog(), m_Id, "System", "%{public}s", systemName ? systemName : "");
#endif
}

SystemWaveTrace::BodyInterval::~BodyInterval()
{
#if defined(__APPLE__)
    if (m_Id != 0)
        os_signpost_interval_end(SignpostLog(), m_Id, "System");
#endif
}

SystemWaveTrace::JoinInterval::JoinInterval([[maybe_unused]] uint32 planWave, [[maybe_unused]] uint32 forks)
{
#if defined(__APPLE__)
    m_Id = BeginSignpostId();
    if (m_Id != 0)
        os_signpost_interval_begin(SignpostLog(), m_Id, "Join", "wave %u, %u forks", planWave, forks);
#endif
}

SystemWaveTrace::JoinInterval::~JoinInterval()
{
#if defined(__APPLE__)
    if (m_Id != 0)
        os_signpost_interval_end(SignpostLog(), m_Id, "Join");
#endif
}

} // namespace GameEngine::ECS
