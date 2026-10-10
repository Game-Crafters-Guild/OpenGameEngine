// SystemManager performance maxima: a reset makes MaxTime the slowest update
// since the reset, so one slow frame can be measured after a slower one (a scene
// load's bake) already set the lifetime maximum.

#include <gtest/gtest.h>

#include "ECS/World.h"
#include "ECS/Systems.h"

#include <chrono>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
class SlowFirstUpdateSystem : public ISystem
{
public:
    void Update(World&, float32) override
    {
        if (m_Updates++ == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const char* GetName() const override { return "SlowFirstUpdate"; }

private:
    int m_Updates = 0;
};
} // namespace

TEST(SystemPerformanceMaxima, ResetMakesMaxTheSlowestUpdateSinceTheReset)
{
    World world(nullptr);
    SystemManager sm; // no job system: the update runs inline
    sm.AddSystem<SlowFirstUpdateSystem>();
    sm.SetPerformanceTracking(true);

    sm.Update(world, 0.016f);
    ASSERT_EQ(sm.GetPerformanceData().size(), 1u);
    EXPECT_GE(sm.GetPerformanceData()[0].MaxTime, 40.0f) << "MaxTime is in milliseconds";

    sm.ResetPerformanceMaxima();
    EXPECT_EQ(sm.GetPerformanceData()[0].MaxTime, 0.0f);
    sm.Update(world, 0.016f);
    const float32 maxAfterReset = sm.GetPerformanceData()[0].MaxTime;
    EXPECT_LT(maxAfterReset, 40.0f) << "the slow update before the reset still reads as the maximum";
    EXPECT_EQ(maxAfterReset, sm.GetPerformanceData()[0].LastTime);
    EXPECT_EQ(sm.GetPerformanceData()[0].UpdateCount, 2u) << "a reset keeps the other statistics";
}
