// The wave trace allocates nothing per frame: with tracing on, a frame of a
// forking plan makes exactly as many allocations as with tracing off.
//
// The comparison is a difference, not an absolute zero: the wave runner itself
// allocates per wave (its list of enabled systems), so the claim measured here
// is the trace's own share. Its own target because the window is process-wide:
// forked bodies run on workers.

#include <gtest/gtest.h>

#include "ECS/SystemWaveTrace.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Memory/AllocationCountScope.h"

#include <cstdint>
#include <cstdio>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class EmptySystem : public ISystem
{
public:
    void Update(World&, float32) override {}
    const char* GetName() const override { return "Empty"; }
};

class ExclusiveSystem : public EmptySystem
{
public:
    bool RequiresExclusiveUpdate() const override { return true; }
};

constexpr int kWarmFrames = 200;
constexpr int kMeasuredFrames = 100;

// Three forked waves, an inline wave and a wave an exclusive system splits.
void BuildPlan(SystemManager& manager)
{
    for (int i = 0; i < 9; ++i)
        manager.AddSystem<EmptySystem>();
    manager.AddSystem<ExclusiveSystem>();
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1, 2}});
    plan.Waves.push_back({{3, 4}});
    plan.Waves.push_back({{5}});
    plan.Waves.push_back({{9, 6, 7}});
    plan.Waves.push_back({{8}});
    manager.SetExecutionPlan(std::move(plan));
}

std::uint64_t CountFrameAllocations(SystemManager& manager, World& world)
{
    for (int i = 0; i < kWarmFrames; ++i)
        manager.Update(world, 0.016f);
    Memory::AllocationCountScope scope(Memory::CountWindow::Process);
    for (int i = 0; i < kMeasuredFrames; ++i)
        manager.Update(world, 0.016f);
    return scope.Count();
}
} // namespace

TEST(SystemWaveTraceAllocation, TracingAddsNoAllocationsPerFrame)
{
    JobSystem::WorkStealingThreadPool pool(2);
    World world(nullptr);
    SystemManager manager(&pool);
    BuildPlan(manager);

    const std::uint64_t untraced = CountFrameAllocations(manager, world);
    manager.GetWaveTrace().SetEnabled(true);
    const std::uint64_t traced = CountFrameAllocations(manager, world);
    std::printf("allocations over %d frames: untraced %llu, traced %llu\n", kMeasuredFrames,
                static_cast<unsigned long long>(untraced), static_cast<unsigned long long>(traced));
    EXPECT_EQ(traced, untraced);
    EXPECT_GE(manager.GetWaveTrace().GetFramesRecorded(), static_cast<std::uint64_t>(kMeasuredFrames));
}
