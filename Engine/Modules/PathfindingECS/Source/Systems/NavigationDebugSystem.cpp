#include "PathfindingECS/Systems/NavigationDebugSystem.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/PathfindingTypes.h"

#include <cmath>

namespace GameEngine::PathfindingECS
{

namespace
{

constexpr float32 kPi = 3.14159265358979323846f;
constexpr uint32 kCircleSegments = 16;

void AddGridTopologyLines(NavDebugData& debugData,
                          const Pathfinding::GridMap& gridMap,
                          bool showCosts)
{
    const auto& settings = gridMap.GetSettings();
    float32 cellSize = settings.CellSize;

    for (uint32 cz = 0; cz < settings.Depth; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            float32 worldX = 0.0f;
            float32 worldZ = 0.0f;
            gridMap.CellToWorld(cx, cz, worldX, worldZ);

            float32 height = gridMap.GetCellHeight(cx, cz);

            // Cell corner positions
            float32 halfCell = cellSize * 0.5f;
            float32 x0 = worldX - halfCell;
            float32 x1 = worldX + halfCell;
            float32 z0 = worldZ - halfCell;
            float32 z1 = worldZ + halfCell;
            float32 y = height + 0.01f; // Slight offset to avoid z-fighting

            // Determine color based on cell state
            float32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.5f;

            if (gridMap.IsCellBlocked(cx, cz))
            {
                r = 0.5f; g = 0.5f; b = 0.5f; a = 0.7f; // Gray for blocked
            }
            else if (showCosts)
            {
                float32 cost = gridMap.GetCellCost(cx, cz);
                if (cost <= 1.0f)
                {
                    r = 0.0f; g = 0.8f; b = 0.0f; // Green for low cost
                }
                else if (cost <= 3.0f)
                {
                    float32 t = (cost - 1.0f) / 2.0f;
                    r = t; g = 0.8f * (1.0f - t * 0.5f); b = 0.0f; // Yellow
                }
                else
                {
                    r = 0.8f; g = 0.0f; b = 0.0f; // Red for high cost
                }
            }
            else
            {
                r = 0.3f; g = 0.6f; b = 0.3f; // Default green
            }

            // Draw cell edges only on boundary or between different-cost neighbors
            bool drawTop = (cz == 0);
            bool drawBottom = (cz == settings.Depth - 1);
            bool drawLeft = (cx == 0);
            bool drawRight = (cx == settings.Width - 1);

            if (!drawTop && cz > 0)
            {
                float32 neighborCost = gridMap.GetCellCost(cx, cz - 1);
                bool neighborBlocked = gridMap.IsCellBlocked(cx, cz - 1);
                if (neighborBlocked != gridMap.IsCellBlocked(cx, cz) ||
                    std::fabs(neighborCost - gridMap.GetCellCost(cx, cz)) > 0.5f)
                    drawTop = true;
            }
            if (!drawBottom && cz + 1 < settings.Depth)
            {
                float32 neighborCost = gridMap.GetCellCost(cx, cz + 1);
                bool neighborBlocked = gridMap.IsCellBlocked(cx, cz + 1);
                if (neighborBlocked != gridMap.IsCellBlocked(cx, cz) ||
                    std::fabs(neighborCost - gridMap.GetCellCost(cx, cz)) > 0.5f)
                    drawBottom = true;
            }
            if (!drawLeft && cx > 0)
            {
                float32 neighborCost = gridMap.GetCellCost(cx - 1, cz);
                bool neighborBlocked = gridMap.IsCellBlocked(cx - 1, cz);
                if (neighborBlocked != gridMap.IsCellBlocked(cx, cz) ||
                    std::fabs(neighborCost - gridMap.GetCellCost(cx, cz)) > 0.5f)
                    drawLeft = true;
            }
            if (!drawRight && cx + 1 < settings.Width)
            {
                float32 neighborCost = gridMap.GetCellCost(cx + 1, cz);
                bool neighborBlocked = gridMap.IsCellBlocked(cx + 1, cz);
                if (neighborBlocked != gridMap.IsCellBlocked(cx, cz) ||
                    std::fabs(neighborCost - gridMap.GetCellCost(cx, cz)) > 0.5f)
                    drawRight = true;
            }

            if (drawTop)
                debugData.CachedLines.push_back({x0, y, z0, x1, y, z0, r, g, b, a});
            if (drawBottom)
                debugData.CachedLines.push_back({x0, y, z1, x1, y, z1, r, g, b, a});
            if (drawLeft)
                debugData.CachedLines.push_back({x0, y, z0, x0, y, z1, r, g, b, a});
            if (drawRight)
                debugData.CachedLines.push_back({x1, y, z0, x1, y, z1, r, g, b, a});
        }
    }
}

void AddGridReservationLines(NavDebugData& debugData,
                             const Pathfinding::GridMap& gridMap,
                             float32 currentTime)
{
    const auto& settings = gridMap.GetSettings();
    float32 cellSize = settings.CellSize;

    for (uint32 cz = 0; cz < settings.Depth; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            Pathfinding::CellReservation reservation = gridMap.GetCellReservation(cx, cz);
            if (!reservation.IsActive(currentTime))
                continue;

            float32 worldX = 0.0f;
            float32 worldZ = 0.0f;
            gridMap.CellToWorld(cx, cz, worldX, worldZ);

            float32 height = gridMap.GetCellHeight(cx, cz);
            float32 halfCell = cellSize * 0.5f;
            float32 x0 = worldX - halfCell;
            float32 x1 = worldX + halfCell;
            float32 z0 = worldZ - halfCell;
            float32 z1 = worldZ + halfCell;
            float32 y = height + 0.01f;

            // Body occupancy (bright). Lookahead reservation (dim).
            float32 rr = 1.0f;
            float32 rg = reservation.Occupancy != 0 ? 0.45f : 0.78f;
            float32 rb = reservation.Occupancy != 0 ? 0.0f : 0.2f;
            float32 ra = reservation.Occupancy != 0 ? 0.9f : 0.4f;
            debugData.DynamicLines.push_back({x0, y, z0, x1, y, z0, rr, rg, rb, ra});
            debugData.DynamicLines.push_back({x1, y, z0, x1, y, z1, rr, rg, rb, ra});
            debugData.DynamicLines.push_back({x1, y, z1, x0, y, z1, rr, rg, rb, ra});
            debugData.DynamicLines.push_back({x0, y, z1, x0, y, z0, rr, rg, rb, ra});
        }
    }
}

void AddPathLines(NavDebugData& debugData,
                  const Components::NavigationAgentState& state,
                  const Pathfinding::PathBuffer& pathBuffer)
{
    Pathfinding::PathHandle pathHandle{state.PathIndex, state.PathGeneration};
    uint32 pointCount = pathBuffer.GetPointCount(pathHandle);
    if (pointCount < 2)
        return;

    // Blue line strip for path
    constexpr float32 r = 0.0f, g = 0.3f, b = 1.0f, a = 1.0f;

    for (uint32 i = 0; i + 1 < pointCount; ++i)
    {
        Pathfinding::PathPoint p0 = pathBuffer.GetPoint(pathHandle, i);
        Pathfinding::PathPoint p1 = pathBuffer.GetPoint(pathHandle, i + 1);
        float32 yOffset = 0.05f; // Slight lift above ground
        debugData.DynamicLines.push_back({
            p0.X, p0.Y + yOffset, p0.Z,
            p1.X, p1.Y + yOffset, p1.Z,
            r, g, b, a});
    }
}

void AddAgentRadiusCircle(NavDebugData& debugData,
                          float32 posX, float32 posY, float32 posZ,
                          float32 radius)
{
    // Cyan circle on the XZ plane
    constexpr float32 r = 0.0f, g = 0.8f, b = 0.8f, a = 0.8f;
    float32 yOffset = posY + 0.05f;

    for (uint32 i = 0; i < kCircleSegments; ++i)
    {
        float32 angle0 = (static_cast<float32>(i) / static_cast<float32>(kCircleSegments)) * 2.0f * kPi;
        float32 angle1 = (static_cast<float32>(i + 1) / static_cast<float32>(kCircleSegments)) * 2.0f * kPi;

        float32 x0 = posX + radius * std::cos(angle0);
        float32 z0 = posZ + radius * std::sin(angle0);
        float32 x1 = posX + radius * std::cos(angle1);
        float32 z1 = posZ + radius * std::sin(angle1);

        debugData.DynamicLines.push_back({x0, yOffset, z0, x1, yOffset, z1, r, g, b, a});
    }
}

void AddNavMeshDebugTriangles(NavDebugData& debugData,
                              const Pathfinding::DetourNavMap& navMap)
{
    uint32 vertCount = navMap.GetDebugVertexCount();
    uint32 triCount = navMap.GetDebugTriangleCount();
    const float32* verts = navMap.GetDebugVertices();
    const uint32* indices = navMap.GetDebugIndices();

    if (!verts || !indices || vertCount == 0 || triCount == 0)
        return;

    // Semi-transparent green triangles
    constexpr float32 r = 0.0f, g = 0.6f, b = 0.2f, a = 0.3f;

    for (uint32 i = 0; i < triCount; ++i)
    {
        uint32 i0 = indices[i * 3 + 0];
        uint32 i1 = indices[i * 3 + 1];
        uint32 i2 = indices[i * 3 + 2];

        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount)
            continue;

        float32 yOffset = 0.02f;
        debugData.CachedTriangles.push_back({
            verts[i0 * 3 + 0], verts[i0 * 3 + 1] + yOffset, verts[i0 * 3 + 2],
            verts[i1 * 3 + 0], verts[i1 * 3 + 1] + yOffset, verts[i1 * 3 + 2],
            verts[i2 * 3 + 0], verts[i2 * 3 + 1] + yOffset, verts[i2 * 3 + 2],
            r, g, b, a});
    }
}

} // anonymous namespace

void NavigationDebugSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;
    m_Time += deltaTime;

    NavDebugData& debugData = NavigationService::GetDebugData();
    debugData.Clear();

    // Find active debug settings (use first one found)
    bool showGrid = false;
    bool showNavMesh = false;
    bool showPaths = false;
    bool showAgentRadii = false;
    bool showCellCosts = false;
    bool showReservations = false;

    world.Query<ECS::Read<Components::NavigationDebugSettings>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationDebugSettings& settings)
        {
            showGrid = showGrid || settings.ShowGrid;
            showNavMesh = showNavMesh || settings.ShowNavMesh;
            showPaths = showPaths || settings.ShowPaths;
            showAgentRadii = showAgentRadii || settings.ShowAgentRadii;
            showCellCosts = showCellCosts || settings.ShowCellCosts;
            showReservations = showReservations || settings.ShowReservations;
        });

    // Early out if nothing is enabled
    if (!showGrid && !showNavMesh && !showPaths && !showAgentRadii &&
        !showCellCosts && !showReservations)
        return;

    auto& pathBuffer = navWorld->GetPathBuffer();

    // Grid visualization
    if (showGrid || showReservations || showCellCosts)
    {
        world.Query<ECS::Read<Components::NavigationGrid>>().Each(
            [&](ECS::EntityHandle, const Components::NavigationGrid& source)
            {
                auto* gridMap = ResolveNavigationGridMap(world, source);
                if (!gridMap)
                    return;

                uint64 key = (static_cast<uint64>(source.NavMapGeneration) << 32) | source.NavMapIndex;
                uint32 currentVersion = gridMap->GetTopologyVersion();
                auto it = m_GridVersionCache.find(key);
                bool needsRegen = (it == m_GridVersionCache.end() ||
                                   it->second != currentVersion ||
                                   m_LastShowCosts != showCellCosts);

                if (needsRegen && (showGrid || showCellCosts))
                {
                    debugData.ClearCached();
                    AddGridTopologyLines(debugData, *gridMap, showCellCosts);
                    m_GridVersionCache[key] = currentVersion;
                }

                if (showReservations)
                {
                    AddGridReservationLines(debugData, *gridMap, m_Time);
                }
            });
        m_LastShowCosts = showCellCosts;
    }

    // NavMesh visualization
    if (showNavMesh)
    {
        world.Query<ECS::Read<Components::NavigationMesh>>().Each(
            [&](ECS::EntityHandle /*e*/, const Components::NavigationMesh& source)
            {
                if (!source.Initialized)
                    return;

                Pathfinding::NavMapHandle mapHandle{source.NavMapIndex, source.NavMapGeneration};
                auto* detourMap = navWorld->GetDetourNavMap(mapHandle);
                if (detourMap && detourMap->IsBuilt())
                {
                    AddNavMeshDebugTriangles(debugData, *detourMap);
                }
            });
    }

    // Path and agent visualization
    if (showPaths || showAgentRadii)
    {
        world.Query<ECS::Read<Components::NavigationAgent>,
                    ECS::Read<Components::NavigationAgentState>,
                    ECS::Read<Components::WorldTransform>>().Each(
            [&](ECS::EntityHandle /*e*/, const Components::NavigationAgent& agent,
                const Components::NavigationAgentState& state,
                const Components::WorldTransform& wt)
            {
                float32 posX = wt.matrix[12];
                float32 posY = wt.matrix[13];
                float32 posZ = wt.matrix[14];

                if (showPaths)
                {
                    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
                    if (status == Pathfinding::PathStatus::Complete ||
                        status == Pathfinding::PathStatus::Partial)
                    {
                        AddPathLines(debugData, state, pathBuffer);
                    }
                }

                if (showAgentRadii)
                {
                    AddAgentRadiusCircle(debugData, posX, posY, posZ, agent.Radius);
                }
            });
    }

    debugData.Finalize();
}

} // namespace GameEngine::PathfindingECS
