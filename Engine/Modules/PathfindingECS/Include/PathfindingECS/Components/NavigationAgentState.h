#pragma once
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

// Runtime state for navigation agents (written every frame by movement/pathfinding systems).
// Separated from NavigationAgent config for cache efficiency - systems that only
// read config (e.g., debug display) don't pollute cache lines with hot runtime data.
struct NavigationAgentState
{
    // Destination
    float32 DestinationX = 0.0f;
    float32 DestinationY = 0.0f;
    float32 DestinationZ = 0.0f;
    bool HasDestination = false;
    bool PathDirty = true;
    // True while this agent owns at least one stamped cell (body or lookahead).
    bool HasCellReservations = false;

    // Path reference (PathHandle packed)
    uint32 PathIndex = 0;
    uint32 PathGeneration = 0;
    uint32 CurrentPathPointIndex = 0;

    // Cached waypoints (written by pathfinding, read by movement)
    float32 CurrentWaypointX = 0.0f;
    float32 CurrentWaypointY = 0.0f;
    float32 CurrentWaypointZ = 0.0f;
    float32 NextWaypointX = 0.0f;
    float32 NextWaypointY = 0.0f;
    float32 NextWaypointZ = 0.0f;

    // Velocity from steering
    float32 VelocityX = 0.0f;
    float32 VelocityY = 0.0f;
    float32 VelocityZ = 0.0f;

    // Status
    uint8 Status = 0; // PathStatus as uint8

    // Detour crowd handle (-1 = not in crowd)
    int32 CrowdAgentHandle = -1;

    // Replan timer (runtime, not saved)
    float32 ReplanTimer = 0.0f;
};

static_assert(std::is_trivially_copyable_v<NavigationAgentState>);
static_assert(std::is_standard_layout_v<NavigationAgentState>);

} // namespace GameEngine::Components
