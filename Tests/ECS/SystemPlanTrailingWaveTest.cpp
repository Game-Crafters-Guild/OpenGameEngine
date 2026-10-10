// SystemManager execution-plan invariants for systems added AFTER a plan was
// built (trailing waves). M1c regression cover: a packaged game registers
// ManagedSystemBridge as a trailing wave; any later SetExecutionPlan used to
// replace Waves wholesale and silently stop the bridge — C# never ticked.

#include <gtest/gtest.h>

#include "ECS/World.h"
#include "ECS/Systems.h"

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class CountingSystem : public ISystem
{
public:
    CountingSystem(const char* name, int* counter) : m_Name(name), m_Counter(counter) {}
    void Update(World&, float32) override { ++(*m_Counter); }
    const char* GetName() const override { return m_Name; }

private:
    const char* m_Name;
    int* m_Counter;
};
} // namespace

TEST(SystemPlanTrailingWave, LateAddedSystemSurvivesPlanReplacement)
{
    World world(nullptr);
    SystemManager sm; // no job system → waves run inline

    int aTicks = 0;
    int bTicks = 0;
    sm.AddSystem<CountingSystem>("A", &aTicks);

    SystemExecutionPlan plan;
    plan.Waves.push_back({{0}});
    sm.SetExecutionPlan(std::move(plan));

    // Added after the plan was built → trailing wave.
    sm.AddSystem<CountingSystem>("B", &bTicks);
    sm.Update(world, 0.016f);
    EXPECT_EQ(aTicks, 1);
    EXPECT_EQ(bTicks, 1);

    // A rebuilt plan that only schedules A must not wipe B's trailing wave.
    SystemExecutionPlan rebuilt;
    rebuilt.Waves.push_back({{0}});
    sm.SetExecutionPlan(std::move(rebuilt));

    sm.Update(world, 0.016f);
    EXPECT_EQ(aTicks, 2);
    EXPECT_EQ(bTicks, 2) << "late-added system wiped by SetExecutionPlan";
}

TEST(SystemPlanTrailingWave, PlanCoveringLateSystemRunsItExactlyOnce)
{
    World world(nullptr);
    SystemManager sm;

    int aTicks = 0;
    int bTicks = 0;
    sm.AddSystem<CountingSystem>("A", &aTicks);

    SystemExecutionPlan plan;
    plan.Waves.push_back({{0}});
    sm.SetExecutionPlan(std::move(plan));

    sm.AddSystem<CountingSystem>("B", &bTicks);

    // A rebuilt plan that DOES schedule the late system must not double-run it
    // through an extra re-appended trailing wave.
    SystemExecutionPlan rebuilt;
    rebuilt.Waves.push_back({{0, 1}});
    sm.SetExecutionPlan(std::move(rebuilt));

    sm.Update(world, 0.016f);
    EXPECT_EQ(aTicks, 1);
    EXPECT_EQ(bTicks, 1);
}

TEST(SystemPlanTrailingWave, ClearDropsPlanAndLateTracking)
{
    World world(nullptr);
    SystemManager sm;

    int aTicks = 0;
    sm.AddSystem<CountingSystem>("A", &aTicks);
    SystemExecutionPlan plan;
    plan.Waves.push_back({{0}});
    sm.SetExecutionPlan(std::move(plan));

    int bTicks = 0;
    sm.AddSystem<CountingSystem>("B", &bTicks); // tracked as late-added

    sm.Clear();
    EXPECT_FALSE(sm.GetExecutionPlan().IsValid()) << "stale plan survived Clear()";

    // Fresh registration after Clear: flat sequential path, no stale indices.
    int cTicks = 0;
    sm.AddSystem<CountingSystem>("C", &cTicks);
    sm.Update(world, 0.016f);
    EXPECT_EQ(cTicks, 1);
    EXPECT_EQ(aTicks, 0);
    EXPECT_EQ(bTicks, 0);

    // A new plan after Clear must not resurrect pre-Clear late-added indices.
    SystemExecutionPlan fresh;
    fresh.Waves.push_back({{0}});
    sm.SetExecutionPlan(std::move(fresh));
    ASSERT_EQ(sm.GetExecutionPlan().Waves.size(), 1u)
        << "pre-Clear late-added tracking leaked into the new plan";
    sm.Update(world, 0.016f);
    EXPECT_EQ(cTicks, 2);
}
