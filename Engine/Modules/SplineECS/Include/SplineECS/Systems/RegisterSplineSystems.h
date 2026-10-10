#pragma once

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
} // namespace GameEngine::ECS

namespace GameEngine::SplineECS
{

// Add spline systems to an external schedule builder.
void AddSplineSystemsToSchedule(ECS::SystemScheduleBuilder& b);

} // namespace GameEngine::SplineECS
