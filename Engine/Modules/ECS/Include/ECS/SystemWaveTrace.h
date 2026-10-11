#pragma once

#include "Types/Types.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace GameEngine::ECS
{

/**
 * @brief A SystemManager's record of how its waves ran: per frame, when each
 * wave began and ended, which systems it forked onto the job pool and when
 * each fork was published and joined, when and on which thread each system
 * body ran, and how many jobs each body and each wave's publish loop
 * published.
 *
 * Off by default. SetEnabled(true) allocates a fixed ring of kFrameCapacity
 * frames once; while it is on, a traced frame writes into the ring's next
 * slot and allocates nothing. While it is off, SystemManager::Update pays one
 * relaxed load per frame. Gaps, makespans and waits are derived by the
 * reader from the stamps; nothing derived is stored.
 *
 * Threads: the frame, its waves and its fork records are written by the
 * thread that calls SystemManager::Update. A system sample is written by the
 * thread that runs the body, into a slot no other thread writes, and the
 * wave's join orders those writes before the frame is published. CopyFrames
 * must therefore run on the thread that calls Update, outside Update (the
 * editor reads it from a debug request, between frames); it never sees a
 * frame being written.
 *
 * On macOS each traced wave, join and system body is also an os_signpost
 * interval (subsystem "com.gameengine.ecs", category "SystemWaves") while an
 * Instruments recording has the log enabled.
 */
class SystemWaveTrace
{
  public:
    static constexpr size_t kFrameCapacity = 256;
    static constexpr size_t kMaxWavesPerFrame = 64;
    static constexpr size_t kMaxSystemsPerFrame = 160;
    // SystemSample::Worker for a body run on the thread that calls Update.
    static constexpr uint16 kCallerThread = 0xFFFF;
    // Returned by AddSystem when the frame's system table is full.
    static constexpr uint16 kNoSample = 0xFFFF;

    // Steady-clock nanoseconds since its epoch.
    static uint64 NowNs();

    /// One system update inside a traced frame.
    struct SystemSample
    {
        uint32 Slot = 0;      // the system's SystemManager slot
        uint16 Wave = 0;      // index into Frame::Waves
        uint16 Worker = kCallerThread; // the compute worker that ran it, or kCallerThread
        uint64 PublishNs = 0; // when its fork was published; 0 for a body run inline
        uint64 BeginNs = 0;
        uint64 EndNs = 0;
        // Jobs published on the body's thread while it ran, including those of
        // jobs its own participating Wait ran inline (PublishCountScope).
        uint32 Publishes = 0;
    };

    /// One wave of the plan inside a traced frame.
    struct WaveSample
    {
        uint32 PlanWave = 0;    // index into the execution plan's Waves
        uint16 FirstSystem = 0; // its first entry in Frame::Systems
        uint16 SystemCount = 0; // its entries in Frame::Systems (enabled systems that ran)
        uint16 Forks = 0;       // systems published to the job pool
        uint16 Joins = 0;       // Wait calls (one per forked range; an exclusive system splits ranges)
        uint64 BeginNs = 0;
        uint64 EndNs = 0;
        uint64 JoinWaitNs = 0;  // summed time the calling thread spent in those Waits
    };

    /// One SystemManager::Update.
    struct Frame
    {
        uint64 Index = 0; // frames traced since the trace was last cleared
        uint64 BeginNs = 0;
        uint64 EndNs = 0;
        // WorkStealingThreadPool::GetPublishedJobCount at BeginNs and EndNs:
        // jobs published by every thread in between, this frame's included.
        uint64 PublishedAtBegin = 0;
        uint64 PublishedAtEnd = 0;
        // Jobs the waves' publish loops published (one stub per fork).
        uint32 WaveForkPublishes = 0;
        uint16 WaveCount = 0;
        uint16 SystemCount = 0;
        // Waves and systems that ran past the fixed tables, unrecorded.
        uint32 DroppedWaves = 0;
        uint32 DroppedSystems = 0;
        WaveSample Waves[kMaxWavesPerFrame];
        SystemSample Systems[kMaxSystemsPerFrame];
    };

    SystemWaveTrace();
    ~SystemWaveTrace();
    SystemWaveTrace(const SystemWaveTrace&) = delete;
    SystemWaveTrace& operator=(const SystemWaveTrace&) = delete;

    bool IsEnabled() const { return m_Enabled.load(std::memory_order_relaxed); }

    /// Turns tracing on or off. Turning it on allocates the ring the first
    /// time and clears it; turning it off keeps the recorded frames readable.
    /// Call on the thread that calls SystemManager::Update, outside Update.
    void SetEnabled(bool enable);

    /// Drops every recorded frame. SystemManager calls it whenever its system
    /// set changes, because a frame names systems by slot. A clear during a
    /// traced frame (a system installed from inside an update) drops that
    /// frame too.
    void Clear();

    /// Frames recorded since the last clear, the ones overwritten included.
    uint64 GetFramesRecorded() const;

    /// Copies up to `maxFrames` of the newest recorded frames into `out`,
    /// oldest first, replacing its contents. See the class comment for the
    /// thread it must run on.
    void CopyFrames(std::vector<Frame>& out, size_t maxFrames) const;

    // Recording, for SystemManager. BeginFrame returns the frame to fill;
    // the caller checked IsEnabled().
    Frame& BeginFrame(uint64 publishedJobs);
    void EndFrame(Frame& frame, uint64 publishedJobs);
    // Null when the frame's wave table is full (counted in DroppedWaves).
    WaveSample* BeginWave(Frame& frame, uint32 planWave);
    void EndWave(Frame& frame, WaveSample& wave);
    // kNoSample when the frame's system table is full (counted in DroppedSystems).
    uint16 AddSystem(Frame& frame, const WaveSample& wave, uint32 slot);

    /// The os_signpost interval of one system body, or nothing off macOS or
    /// while no recording has the log enabled.
    class BodyInterval
    {
      public:
        explicit BodyInterval(const char* systemName);
        ~BodyInterval();
        BodyInterval(const BodyInterval&) = delete;
        BodyInterval& operator=(const BodyInterval&) = delete;

      private:
        uint64 m_Id = 0;
    };

    /// The os_signpost interval of one wave's join.
    class JoinInterval
    {
      public:
        JoinInterval(uint32 planWave, uint32 forks);
        ~JoinInterval();
        JoinInterval(const JoinInterval&) = delete;
        JoinInterval& operator=(const JoinInterval&) = delete;

      private:
        uint64 m_Id = 0;
    };

  private:
    std::atomic<bool> m_Enabled{false};
    std::unique_ptr<Frame[]> m_Frames;
    uint64 m_FramesRecorded = 0;
    bool m_ClearPending = false;
    // The os_signpost id of the wave in flight; 0 when none is open.
    uint64 m_WaveSignpost = 0;
};

} // namespace GameEngine::ECS
