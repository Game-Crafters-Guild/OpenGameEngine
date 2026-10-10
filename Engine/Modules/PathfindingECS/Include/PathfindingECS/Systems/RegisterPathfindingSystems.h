#pragma once
namespace GameEngine::ECS { class SystemScheduleBuilder; }

namespace GameEngine::PathfindingECS
{

void AddPathfindingSystemsToSchedule(ECS::SystemScheduleBuilder& schedule);

} // namespace GameEngine::PathfindingECS
