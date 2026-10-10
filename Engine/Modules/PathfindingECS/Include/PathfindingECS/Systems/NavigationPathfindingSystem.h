#pragma once
#include "ECS/Systems.h"
#include "JobSystem/TaskHandle.h"
#include "Pathfinding/PathfindingTypes.h"
#include <unordered_map>
#include <vector>

namespace GameEngine::PathfindingECS
{

class NavigationPathfindingSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "NavigationPathfindingSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Maximum number of timer-based replans allowed per frame.
    // Replans triggered by proximity checks or manual PathDirty are not limited.
    static constexpr uint32 kDefaultMaxReplansPerFrame = 8;
    // How long a Failed agent waits before retrying. Occupancy no longer fails a
    // path (A* taxes it), so what is left to clear is a blocked destination or a
    // nav map that was not built yet.
    static constexpr float32 kFailedPathRetrySeconds = 0.2f;
    uint32 MaxReplansPerFrame = kDefaultMaxReplansPerFrame;

private:
    // The scope a request was submitted in. Entity handles restart with every
    // world reset and path handles with every navigation service, so a
    // completion is applied only while all three still match the world being
    // updated; a mismatch retires the result without touching an actor.
    struct InFlightRequest
    {
        JobSystem::TaskHandle Task;
        uint64 WorldId;
        uint64 ResetGeneration;
        uint64 ServiceGeneration;
    };

    struct PendingCompletion
    {
        uint64 EntityId;
        Pathfinding::PathResult Result;
        bool Success;
    };

    std::unordered_map<uint64, InFlightRequest> m_InFlightRequests;
    std::vector<PendingCompletion> m_CompletedResults;
    float32 m_AccumulatedTime = 0.0f;
    uint32 m_ReplanSeed = 0; // for staggering timer initialization
};

} // namespace GameEngine::PathfindingECS
