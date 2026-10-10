#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"
#include "ECS/SystemScheduling.h"

#include "PhysicsECS/Systems/CharacterControllerSystem.h"
#include "PhysicsECS/Systems/CharacterControllerWritebackSystem.h"
#include "PhysicsECS/Systems/PhysicsEventsSystem.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsStepSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldBootstrapSystem.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"

namespace GameEngine::PhysicsECS
{
void AddPhysicsSystemsToSchedule(ECS::SystemScheduleBuilder& schedule)
{
    using namespace ECS;

    // These names are defined by the rendering schedule builder.
    // We depend on TransformHierarchy so WorldTransform is ready before we step.
    constexpr const char* kHierarchy = "TransformHierarchy";

    constexpr const char* kPhysicsBootstrap = "PhysicsWorldBootstrap";
    constexpr const char* kPhysicsEvents = "PhysicsEvents";
    constexpr const char* kPhysicsInit = "PhysicsInit";
    constexpr const char* kCharacterController = "CharacterController";
    constexpr const char* kPhysicsStep = "PhysicsStep";
    constexpr const char* kCharacterWriteback = "CharacterControllerWriteback";
    constexpr const char* kPhysicsWriteback = "PhysicsWriteback";

    // RenderExtraction declares optional edges on the two writebacks below;
    // that edge is what orders extraction after them. The wave solver orders
    // by declared edges only; phase and order sort within a wave.
    schedule.Add<PhysicsWorldBootstrapSystem>(kPhysicsBootstrap, SystemPhase::Extraction, 1, {});
    schedule.Add<PhysicsEventsSystem>(kPhysicsEvents, SystemPhase::Extraction, 2, {kPhysicsBootstrap});
    // PhysicsInit runs after TerrainPhysics so HeightFieldColliderShape
    // components are provisioned before the gather. TerrainECS is an optional
    // module — a project that never registers it is a valid configuration, so
    // the edge is declared optional and simply goes inactive there.
    schedule.Add<PhysicsInitSystem>(kPhysicsInit, SystemPhase::Extraction, 3,
                             {kHierarchy, kPhysicsBootstrap, ECS::OptionalDependency("TerrainPhysics")});
    schedule.Add<CharacterControllerSystem>(kCharacterController, SystemPhase::Extraction, 4,
                                     {kHierarchy, kPhysicsInit, kPhysicsBootstrap});
    schedule.Add<PhysicsStepSystem>(kPhysicsStep, SystemPhase::Extraction, 5,
                             {kPhysicsInit, kPhysicsEvents, kCharacterController});
    schedule.Add<CharacterControllerWritebackSystem>(kCharacterWriteback, SystemPhase::Extraction, 6, {kPhysicsStep});
    schedule.Add<PhysicsWritebackSystem>(kPhysicsWriteback, SystemPhase::Extraction, 7, {kPhysicsStep});
}

} // namespace GameEngine::PhysicsECS

