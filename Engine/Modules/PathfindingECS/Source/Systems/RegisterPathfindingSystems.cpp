#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
#include "ECS/SystemScheduling.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationDebugSystem.h"

namespace GameEngine::PathfindingECS
{

void AddPathfindingSystemsToSchedule(ECS::SystemScheduleBuilder& schedule)
{
    // NavigationBuildSystem consumes finished products, not merely the existence
    // of the physics world object:
    //   - TransformHierarchy produces the WorldTransform a grid uses as its
    //     origin. A freshly loaded entity has no WorldTransform until
    //     TransformHierarchy adds it, and a grid latches its origin on the frame
    //     it initializes.
    //   - PhysicsWriteback terminates the physics pipeline. Bodies exist from
    //     PhysicsInit onward, but ordering after the whole pipeline is what
    //     keeps the bake's Jolt queries off PhysicsStep's wave — the step is the
    //     one phase Jolt does not permit concurrent queries against.
    // OceanBuoyancy is the other post-physics Jolt writer; keeping the bake off
    // its wave avoids querying Jolt while it applies forces. Ocean is an
    // optional module, so that edge is optional and goes inactive without it.
    schedule.Add<NavigationBuildSystem>("NavigationBuildSystem", ECS::SystemPhase::Early, 10,
                                 {"TransformHierarchy", "PhysicsWriteback",
                                  ECS::OptionalDependency("OceanBuoyancy")});
    schedule.Add<NavigationPathfindingSystem>("NavigationPathfindingSystem", ECS::SystemPhase::Early, 20,
                                 {"NavigationBuildSystem"});
    schedule.Add<NavigationMovementSystem>("NavigationMovementSystem", ECS::SystemPhase::Early, 30,
                              {"NavigationPathfindingSystem"});
    schedule.Add<NavigationDebugSystem>("NavigationDebugSystem", ECS::SystemPhase::Render, 5,
                               {"NavigationMovementSystem"});
}

} // namespace GameEngine::PathfindingECS
