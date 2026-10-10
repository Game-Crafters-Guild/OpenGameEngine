// SetParallelWavesEnabled(false) must keep every system of a multi-system wave
// on the thread that calls Update. Backends whose GPU objects belong to the
// thread that created them depend on it: a rendering system that initialises a
// feature or compiles a pipeline inside its update kills a worker otherwise.

#include <gtest/gtest.h>

#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
/// Records the thread each update ran on, so a test can assert dispatch.
class ThreadRecordingSystem : public ISystem
{
public:
    ThreadRecordingSystem(const char* name, std::vector<std::thread::id>* seen)
        : m_Name(name), m_Seen(seen)
    {
    }

    void Update(World&, float32) override { m_Seen->push_back(std::this_thread::get_id()); }
    const char* GetName() const override { return m_Name; }

private:
    const char* m_Name;
    std::vector<std::thread::id>* m_Seen;
};

/// A plan with both systems in one wave — the only shape that dispatches.
SystemExecutionPlan OneWaveOfTwo()
{
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1}});
    return plan;
}

constexpr size_t kWorkerCount = 2;
constexpr int kUpdateCount = 8;
} // namespace

TEST(SystemWaveThreadPinning, PinnedWavesRunEverySystemOnTheCallingThread)
{
    JobSystem::WorkStealingThreadPool pool(kWorkerCount);
    World world(nullptr);
    SystemManager sm(&pool);

    std::vector<std::thread::id> a;
    std::vector<std::thread::id> b;
    sm.AddSystem<ThreadRecordingSystem>("A", &a);
    sm.AddSystem<ThreadRecordingSystem>("B", &b);
    sm.SetExecutionPlan(OneWaveOfTwo());
    sm.SetParallelWavesEnabled(false);

    const std::thread::id caller = std::this_thread::get_id();
    for (int i = 0; i < kUpdateCount; ++i)
        sm.Update(world, 0.016f);

    ASSERT_EQ(a.size(), static_cast<size_t>(kUpdateCount));
    ASSERT_EQ(b.size(), static_cast<size_t>(kUpdateCount));
    for (int i = 0; i < kUpdateCount; ++i)
    {
        EXPECT_EQ(a[i], caller) << "wave system A left the calling thread";
        EXPECT_EQ(b[i], caller) << "wave system B left the calling thread";
    }
}

TEST(SystemWaveThreadPinning, PinningDoesNotChangeHowOftenSystemsRun)
{
    JobSystem::WorkStealingThreadPool pool(kWorkerCount);
    World world(nullptr);
    SystemManager sm(&pool);

    std::vector<std::thread::id> a;
    std::vector<std::thread::id> b;
    sm.AddSystem<ThreadRecordingSystem>("A", &a);
    sm.AddSystem<ThreadRecordingSystem>("B", &b);
    sm.SetExecutionPlan(OneWaveOfTwo());

    sm.Update(world, 0.016f);
    sm.SetParallelWavesEnabled(false);
    sm.Update(world, 0.016f);
    sm.SetParallelWavesEnabled(true);
    sm.Update(world, 0.016f);

    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(b.size(), 3u);
}
