#pragma once

namespace GameEngine::ECS { class SystemScheduleBuilder; }

namespace GameEngine::PhysicsECS
{
// Adds physics systems to an existing dependency-aware schedule builder.
// This allows PhysicsECS to slot into the same schedule as rendering/audio.
void AddPhysicsSystemsToSchedule(ECS::SystemScheduleBuilder& schedule);

} // namespace GameEngine::PhysicsECS
