#include "Ocean/Systems/RegisterOceanSystems.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace GameEngine::Ocean
{
namespace
{

int WaveOf(const ECS::SystemManager& manager, std::string_view systemName)
{
    const ECS::SystemExecutionPlan& plan = manager.GetExecutionPlan();
    for (std::size_t wave = 0; wave < plan.Waves.size(); ++wave)
    {
        for (std::size_t index : plan.Waves[wave].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(index);
            if (name && std::string_view(name) == systemName)
                return static_cast<int>(wave);
        }
    }
    return -1;
}

// OceanExtraction samples spline data through pointers into SplineService
// storage, which SplineExtraction grows and rebuilds; systems inside one wave
// run in parallel, so the ordering has to be a declared edge. Built from the two
// modules' own schedule contributions only, so an undeclared edge puts both in
// wave 0 and fails here.
TEST(OceanSplineSchedule, SplineExtractionIsScheduledBeforeOceanExtraction)
{
    ECS::SystemScheduleBuilder builder;
    SplineECS::AddSplineSystemsToSchedule(builder);
    AddOceanSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    // Partial by design: this fixture's falsifiability depends on TransformHierarchy
    // and Camera being ABSENT. Registering them would place OceanExtraction in a
    // later wave than the dependency-less SplineExtraction whether or not the edge
    // under test exists, and the assertion below would pass vacuously.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);
    ASSERT_TRUE(manager.GetExecutionPlan().IsValid());

    const int splineWave = WaveOf(manager, "SplineExtraction");
    const int oceanWave = WaveOf(manager, "OceanExtraction");
    ASSERT_GE(splineWave, 0);
    ASSERT_GE(oceanWave, 0);
    EXPECT_LT(splineWave, oceanWave)
        << "OceanExtraction does not declare a dependency on SplineExtraction — "
           "same-wave systems execute in parallel";
}

} // namespace
} // namespace GameEngine::Ocean
