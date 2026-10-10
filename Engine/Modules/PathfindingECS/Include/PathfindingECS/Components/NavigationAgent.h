#pragma once
#include "Types/Types.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "ECS/RequiredComponents.h"
#include <type_traits>

namespace GameEngine::Components
{

struct NavigationAgent
{
    // Configuration (set once, read by many systems)
    float32 Speed = 3.5f;
    float32 Acceleration = 8.0f;
    float32 StoppingDistance = 0.1f;
    float32 Radius = 0.5f;
    float32 Height = 2.0f;
    bool SmoothPaths = true;

    // Obstacle avoidance config
    bool AvoidanceEnabled = true;
    uint8 AvoidanceQuality = 3;
    float32 SeparationWeight = 2.0f;

    // Nav map reference
    uint32 NavMapIndex = 0;
    uint32 NavMapGeneration = 0;

    // Replan config
    float32 ReplanInterval = 2.0f;

    // Entity ID used for reservation ownership
    uint32 AgentId = 0;
};

static_assert(std::is_trivially_copyable_v<NavigationAgent>);
static_assert(std::is_standard_layout_v<NavigationAgent>);

} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{

template<>
struct RequiredComponents<GameEngine::Components::NavigationAgent>
{
    using type = TypeList<GameEngine::Components::NavigationAgentState>;
};

} // namespace GameEngine::ECS::Detail
