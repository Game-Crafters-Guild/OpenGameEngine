#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <functional>

namespace GameEngine::Pathfinding
{

enum class PathStatus : uint8
{
    Invalid,
    Pending,
    Complete,
    Partial,
    Failed
};

enum class GridType : uint8
{
    Square,
    Hex
};

struct PathPoint
{
    float32 X = 0.0f;
    float32 Y = 0.0f;
    float32 Z = 0.0f;
};

struct PathHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
};

struct PathResult
{
    PathStatus Status = PathStatus::Invalid;
    PathHandle Path;
};

struct PathRequest
{
    float32 StartX = 0.0f, StartY = 0.0f, StartZ = 0.0f;
    float32 EndX = 0.0f, EndY = 0.0f, EndZ = 0.0f;
    float32 AgentRadius = 0.5f;
    float32 AgentHeight = 2.0f;
    uint32 ExcludeAgentId = 0; // reservation owner to ignore during A*
    float32 CurrentTime = 0.0f; // game time for reservation expiration checks
    bool SmoothPath = true;    // Apply string-pulling to simplify grid paths
};

struct GridSettings
{
    GridType Type = GridType::Square;
    float32 CellSize = 1.0f;
    float32 OriginX = 0.0f, OriginY = 0.0f, OriginZ = 0.0f;
    uint32 Width = 64;
    uint32 Depth = 64;
    float32 MaxSlope = 45.0f;
    float32 MaxStepHeight = 0.4f;
};

struct NavMeshSettings
{
    float32 CellSize = 0.3f;
    float32 CellHeight = 0.2f;
    float32 AgentRadius = 0.6f;
    float32 AgentHeight = 2.0f;
    float32 AgentMaxClimb = 0.9f;
    float32 AgentMaxSlope = 45.0f;
    float32 RegionMinSize = 8.0f;
    float32 RegionMergeSize = 20.0f;
    float32 EdgeMaxLen = 12.0f;
    float32 EdgeMaxError = 1.3f;
    float32 DetailSampleDist = 6.0f;
    float32 DetailSampleMaxError = 1.0f;
    int32 VertsPerPoly = 6;
};

struct NavMapHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
    bool operator==(const NavMapHandle& other) const { return Index == other.Index && Generation == other.Generation; }
    bool operator!=(const NavMapHandle& other) const { return !(*this == other); }
};

struct NavMapHandleHash
{
    std::size_t operator()(const NavMapHandle& h) const
    {
        return std::hash<uint64>{}(static_cast<uint64>(h.Index) | (static_cast<uint64>(h.Generation) << 32));
    }
};

struct NavLinkId
{
    uint32 Index = 0;
    uint32 Generation = 0;
    bool IsValid() const { return Generation != 0; }
};

struct NavLink
{
    NavMapHandle SourceMap;
    float32 SourceX = 0.0f, SourceY = 0.0f, SourceZ = 0.0f;
    NavMapHandle TargetMap;
    float32 TargetX = 0.0f, TargetY = 0.0f, TargetZ = 0.0f;
    float32 TraversalCost = 1.0f;
    bool Bidirectional = true;
};

struct CellReservation
{
    uint32 AgentId = 0;
    float32 ExpirationTime = 0.0f;
    // 1 = agent body is in this cell now. A* taxes it; movement refuses
    // it (except dest stand-off). 0 = lookahead tax only.
    uint8 Occupancy = 0;
    bool IsActive(float32 currentTime) const { return AgentId != 0 && ExpirationTime > currentTime; }
};

} // namespace GameEngine::Pathfinding
