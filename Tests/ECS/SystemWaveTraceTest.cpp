// SystemManager's wave trace (SystemWaveTrace) under a known plan: what each
// traced frame records about its waves, forks, joins and system bodies, and
// when the trace records nothing.
//
// The plan, on a 2-worker pool:
//   wave 0: A, B, Nester    forked (3 forks, one join); Nester runs 3 nested jobs
//   wave 1: Solo            inline on the caller
//   wave 2: Exclusive, C, D Exclusive inline first, then C and D forked (2 forks, one join)

#include <gtest/gtest.h>

#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class NamedSystem : public ISystem
{
public:
    explicit NamedSystem(const char* name, bool exclusive = false) : m_Name(name), m_Exclusive(exclusive) {}
    void Update(World&, float32) override { m_Updates.fetch_add(1, std::memory_order_relaxed); }
    const char* GetName() const override { return m_Name; }
    bool RequiresExclusiveUpdate() const override { return m_Exclusive; }

private:
    const char* m_Name;
    bool m_Exclusive;
    std::atomic<int> m_Updates{0};
};

// Forks kNestedJobs jobs of its own from inside its update and joins them.
class NesterSystem : public ISystem
{
public:
    static constexpr uint32 kNestedJobs = 3;
    explicit NesterSystem(JobSystem::WorkStealingThreadPool& pool) : m_Pool(pool) {}
    void Update(World&, float32) override
    {
        JobSystem::JobCounter counter;
        for (uint32 i = 0; i < kNestedJobs; ++i)
            m_Pool.Run([this] { m_Ran.fetch_add(1, std::memory_order_relaxed); }, counter);
        m_Pool.Wait(counter);
    }
    const char* GetName() const override { return "Nester"; }

private:
    JobSystem::WorkStealingThreadPool& m_Pool;
    std::atomic<uint32> m_Ran{0};
};

constexpr size_t kWorkers = 2;
constexpr int kFrames = 6;

struct KnownPlan
{
    JobSystem::WorkStealingThreadPool Pool{kWorkers};
    World TheWorld{nullptr};
    SystemManager Manager{&Pool};

    KnownPlan()
    {
        Manager.AddSystem<NamedSystem>("A");
        Manager.AddSystem<NamedSystem>("B");
        Manager.AddSystem<NesterSystem>(Pool);
        Manager.AddSystem<NamedSystem>("Solo");
        Manager.AddSystem<NamedSystem>("Exclusive", true);
        Manager.AddSystem<NamedSystem>("C");
        Manager.AddSystem<NamedSystem>("D");
        SystemExecutionPlan plan;
        plan.Waves.push_back({{0, 1, 2}});
        plan.Waves.push_back({{3}});
        plan.Waves.push_back({{4, 5, 6}});
        Manager.SetExecutionPlan(std::move(plan));
    }

    void Run(int frames)
    {
        for (int i = 0; i < frames; ++i)
            Manager.Update(TheWorld, 0.016f);
    }

    std::vector<SystemWaveTrace::Frame> Frames()
    {
        std::vector<SystemWaveTrace::Frame> frames;
        Manager.GetWaveTrace().CopyFrames(frames, SystemWaveTrace::kFrameCapacity);
        return frames;
    }

    std::string NameOf(const SystemWaveTrace::SystemSample& sample) const
    {
        const char* name = Manager.GetSequentialSystemName(sample.Slot);
        return name ? name : "";
    }
};
} // namespace

TEST(SystemWaveTraceTest, RecordsEveryWaveForkJoinAndBodyOfAKnownPlan)
{
    KnownPlan known;
    known.Manager.GetWaveTrace().SetEnabled(true);
    known.Run(kFrames);

    const std::vector<SystemWaveTrace::Frame> frames = known.Frames();
    ASSERT_EQ(frames.size(), static_cast<size_t>(kFrames));
    EXPECT_EQ(known.Manager.GetWaveTrace().GetFramesRecorded(), static_cast<uint64>(kFrames));

    for (size_t f = 0; f < frames.size(); ++f)
    {
        SCOPED_TRACE("frame " + std::to_string(f));
        const SystemWaveTrace::Frame& frame = frames[f];
        EXPECT_EQ(frame.Index, f);
        ASSERT_EQ(frame.WaveCount, 3u);
        ASSERT_EQ(frame.SystemCount, 7u);
        EXPECT_EQ(frame.DroppedWaves, 0u);
        EXPECT_EQ(frame.DroppedSystems, 0u);

        // Forks per wave, joins per forked range, and the publish loops' stubs.
        const uint16 expectedForks[3] = {3, 0, 2};
        const uint16 expectedJoins[3] = {1, 0, 1};
        const uint16 expectedSystems[3] = {3, 1, 3};
        for (uint16 w = 0; w < 3; ++w)
        {
            const SystemWaveTrace::WaveSample& wave = frame.Waves[w];
            EXPECT_EQ(wave.PlanWave, w);
            EXPECT_EQ(wave.SystemCount, expectedSystems[w]);
            EXPECT_EQ(wave.Forks, expectedForks[w]);
            EXPECT_EQ(wave.Joins, expectedJoins[w]);
            EXPECT_LE(wave.BeginNs, wave.EndNs);
            if (wave.Joins > 0)
            {
                EXPECT_GT(wave.JoinWaitNs, 0u);
                EXPECT_LE(wave.JoinWaitNs, wave.EndNs - wave.BeginNs);
            }
            else
            {
                EXPECT_EQ(wave.JoinWaitNs, 0u);
            }
        }
        EXPECT_EQ(frame.WaveForkPublishes, 5u);
        // Every job published during the frame: the 5 fork stubs and Nester's 3.
        EXPECT_EQ(frame.PublishedAtEnd - frame.PublishedAtBegin, 5u + NesterSystem::kNestedJobs);

        // Waves run in order and inside the frame.
        EXPECT_LE(frame.BeginNs, frame.Waves[0].BeginNs);
        EXPECT_LE(frame.Waves[0].EndNs, frame.Waves[1].BeginNs);
        EXPECT_LE(frame.Waves[1].EndNs, frame.Waves[2].BeginNs);
        EXPECT_LE(frame.Waves[2].EndNs, frame.EndNs);

        // The systems in recording order: wave 0's forks, Solo, Exclusive, then wave 2's forks.
        const char* expectedNames[7] = {"A", "B", "Nester", "Solo", "Exclusive", "C", "D"};
        const bool expectedForked[7] = {true, true, true, false, false, true, true};
        for (uint16 s = 0; s < 7; ++s)
        {
            const SystemWaveTrace::SystemSample& sample = frame.Systems[s];
            SCOPED_TRACE(expectedNames[s]);
            EXPECT_EQ(known.NameOf(sample), expectedNames[s]);
            const SystemWaveTrace::WaveSample& wave = frame.Waves[sample.Wave];
            EXPECT_GE(s, wave.FirstSystem);
            EXPECT_LT(s, wave.FirstSystem + wave.SystemCount);
            EXPECT_LE(wave.BeginNs, sample.BeginNs);
            EXPECT_LE(sample.BeginNs, sample.EndNs);
            EXPECT_LE(sample.EndNs, wave.EndNs);
            EXPECT_EQ(sample.Publishes, s == 2 ? NesterSystem::kNestedJobs : 0u);
            if (expectedForked[s])
            {
                // A native caller parks in Wait, so a forked body runs on a worker.
                EXPECT_NE(sample.PublishNs, 0u);
                EXPECT_LE(wave.BeginNs, sample.PublishNs);
                EXPECT_LE(sample.PublishNs, sample.BeginNs);
                EXPECT_LT(sample.Worker, kWorkers);
            }
            else
            {
                EXPECT_EQ(sample.PublishNs, 0u);
                EXPECT_EQ(sample.Worker, SystemWaveTrace::kCallerThread);
            }
        }
    }
}

TEST(SystemWaveTraceTest, RecordsNothingWhileDisabledAndClearsWhenEnabledAgain)
{
    KnownPlan known;
    SystemWaveTrace& trace = known.Manager.GetWaveTrace();
    known.Run(3);
    EXPECT_FALSE(trace.IsEnabled());
    EXPECT_EQ(trace.GetFramesRecorded(), 0u);
    EXPECT_TRUE(known.Frames().empty());

    trace.SetEnabled(true);
    known.Run(4);
    trace.SetEnabled(false);
    known.Run(3);
    // Disabling keeps what was recorded and records nothing more.
    EXPECT_EQ(trace.GetFramesRecorded(), 4u);
    EXPECT_EQ(known.Frames().size(), 4u);

    trace.SetEnabled(true);
    EXPECT_EQ(trace.GetFramesRecorded(), 0u);
    known.Run(2);
    const std::vector<SystemWaveTrace::Frame> frames = known.Frames();
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].Index, 0u);
}

TEST(SystemWaveTraceTest, ChangingTheSystemSetDropsRecordedFrames)
{
    KnownPlan known;
    SystemWaveTrace& trace = known.Manager.GetWaveTrace();
    trace.SetEnabled(true);
    known.Run(3);
    ASSERT_EQ(trace.GetFramesRecorded(), 3u);

    known.Manager.AddSystem<NamedSystem>("Late");
    EXPECT_EQ(trace.GetFramesRecorded(), 0u);
    EXPECT_TRUE(known.Frames().empty());

    // The late system ran as its own trailing wave in the frames after the change.
    known.Run(2);
    const std::vector<SystemWaveTrace::Frame> frames = known.Frames();
    ASSERT_EQ(frames.size(), 2u);
    ASSERT_EQ(frames[1].WaveCount, 4u);
    EXPECT_EQ(known.NameOf(frames[1].Systems[frames[1].SystemCount - 1]), "Late");

    known.Manager.RetireSystem(7);
    EXPECT_EQ(trace.GetFramesRecorded(), 0u);
}

TEST(SystemWaveTraceTest, DisabledSystemsAreNotRecorded)
{
    KnownPlan known;
    known.Manager.SetSystemEnabledByName("B", false);
    known.Manager.GetWaveTrace().SetEnabled(true);
    known.Run(1);
    const std::vector<SystemWaveTrace::Frame> frames = known.Frames();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].SystemCount, 6u);
    EXPECT_EQ(frames[0].Waves[0].Forks, 2u);
    EXPECT_EQ(frames[0].WaveForkPublishes, 4u);
}

TEST(SystemWaveTraceTest, WavesPastTheFixedTableAreCountedAsDropped)
{
    JobSystem::WorkStealingThreadPool pool(kWorkers);
    World world(nullptr);
    SystemManager manager(&pool);
    constexpr size_t kWaves = SystemWaveTrace::kMaxWavesPerFrame + 6;
    SystemExecutionPlan plan;
    for (size_t i = 0; i < kWaves; ++i)
    {
        manager.AddSystem<NamedSystem>("Single");
        plan.Waves.push_back({{i}});
    }
    manager.SetExecutionPlan(std::move(plan));
    manager.GetWaveTrace().SetEnabled(true);
    manager.Update(world, 0.016f);

    std::vector<SystemWaveTrace::Frame> frames;
    manager.GetWaveTrace().CopyFrames(frames, 1);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].WaveCount, SystemWaveTrace::kMaxWavesPerFrame);
    EXPECT_EQ(frames[0].DroppedWaves, 6u);
    EXPECT_EQ(frames[0].SystemCount, SystemWaveTrace::kMaxWavesPerFrame);
}

TEST(SystemWaveTraceTest, TheRingKeepsTheNewestFrames)
{
    KnownPlan known;
    known.Manager.GetWaveTrace().SetEnabled(true);
    constexpr int kRun = static_cast<int>(SystemWaveTrace::kFrameCapacity) + 10;
    known.Run(kRun);
    const std::vector<SystemWaveTrace::Frame> frames = known.Frames();
    ASSERT_EQ(frames.size(), SystemWaveTrace::kFrameCapacity);
    EXPECT_EQ(frames.front().Index, 10u);
    EXPECT_EQ(frames.back().Index, static_cast<uint64>(kRun - 1));
    // A slot reused after the ring wraps starts from a clean sample, so a
    // wrapped frame's counts are its own.
    EXPECT_EQ(known.NameOf(frames.back().Systems[2]), "Nester");
    EXPECT_EQ(frames.back().Systems[2].Publishes, NesterSystem::kNestedJobs);
}

TEST(SystemWaveTraceTest, TracedFramesStillFeedPerformanceTracking)
{
    KnownPlan known;
    known.Manager.SetPerformanceTracking(true);
    known.Manager.GetWaveTrace().SetEnabled(true);
    known.Run(kFrames);
    const auto& performance = known.Manager.GetPerformanceData();
    ASSERT_EQ(performance.size(), 7u);
    for (const auto& system : performance)
        EXPECT_EQ(system.UpdateCount, static_cast<uint32>(kFrames)) << system.Name;
}
