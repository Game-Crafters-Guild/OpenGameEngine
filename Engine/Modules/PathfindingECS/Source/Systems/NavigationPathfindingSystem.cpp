#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/PathfindingTypes.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::PathfindingECS
{

namespace
{

// True while every cell of the agent's stamped square footprint is still owned
// by it, still marked as a body and not yet expired. A warm centre alone cannot
// prove a large body, so the whole footprint is tested. Cells outside the map
// are excluded, matching the edge clipping the stamp itself performs.
bool HasCurrentBody(const Pathfinding::GridMap& grid, const Components::NavigationAgent& agent,
                    float32 posX, float32 posZ, float32 currentTime)
{
    uint32 x = 0, z = 0;
    if (!grid.WorldToCell(posX, posZ, x, z))
        return false;

    // WorldToCell succeeded, so the centre is inside the map and Width/Depth are
    // at least one. Clamping the reach to the map first keeps x + radius from
    // wrapping for an agent whose radius dwarfs the grid.
    const Pathfinding::GridSettings& settings = grid.GetSettings();
    const uint32 radius = std::min(grid.FootprintRadiusInCells(agent.Radius),
                                   std::max(settings.Width, settings.Depth));
    const uint32 minX = radius < x ? x - radius : 0;
    const uint32 minZ = radius < z ? z - radius : 0;
    const uint32 maxX = std::min(x + radius, settings.Width - 1);
    const uint32 maxZ = std::min(z + radius, settings.Depth - 1);
    for (uint32 cz = minZ; cz <= maxZ; ++cz)
    {
        for (uint32 cx = minX; cx <= maxX; ++cx)
        {
            const auto cell = grid.GetCellReservation(cx, cz);
            if (cell.AgentId != agent.AgentId || cell.Occupancy == 0 || !cell.IsActive(currentTime))
                return false;
        }
    }
    return true;
}

void CacheWaypointsFromPath(Components::NavigationAgentState& state, const Pathfinding::PathBuffer& pathBuffer)
{
    Pathfinding::PathHandle pathHandle{state.PathIndex, state.PathGeneration};
    uint32 pointCount = pathBuffer.GetPointCount(pathHandle);
    if (pointCount == 0)
        return;

    uint32 currentIdx = state.CurrentPathPointIndex;
    if (currentIdx >= pointCount)
        currentIdx = pointCount - 1;

    Pathfinding::PathPoint current = pathBuffer.GetPoint(pathHandle, currentIdx);
    state.CurrentWaypointX = current.X;
    state.CurrentWaypointY = current.Y;
    state.CurrentWaypointZ = current.Z;

    uint32 nextIdx = currentIdx + 1;
    if (nextIdx < pointCount)
    {
        Pathfinding::PathPoint next = pathBuffer.GetPoint(pathHandle, nextIdx);
        state.NextWaypointX = next.X;
        state.NextWaypointY = next.Y;
        state.NextWaypointZ = next.Z;
    }
    else
    {
        state.NextWaypointX = current.X;
        state.NextWaypointY = current.Y;
        state.NextWaypointZ = current.Z;
    }
}

} // anonymous namespace

// No scene in the repository authors a NavigationAgent, so these queries match
// nothing today. That is a fact about the content, not about the component
// model: NavigationAgent has a registered scene schema
// (NavigationSceneSchemas.cpp), and adding it via that schema — scene load,
// the editor's add-component path, or the debug server — also adds
// NavigationAgentState, because RequiredComponents<NavigationAgent> names it
// (NavigationAgent.h) and both World::AddComponent and AddComponentImmediate
// honour that list. Author one agent and these queries iterate.
//
// What has no authoring path at all is the destination.
// NavigationAgentState::HasDestination gates every request below, and no code
// outside tests ever sets it true — the sole non-test write is
// NavigationMovementSystem clearing it on arrival. So an authored agent is
// visited but never solves a path, including through the synchronous fallback.
// A destination setter is the missing piece, not a bug in this file.
void NavigationPathfindingSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    m_AccumulatedTime += deltaTime;
    auto& pathBuffer = navWorld->GetPathBuffer();
    const uint64 worldId = world.GetWorldId();
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    const uint64 serviceGeneration = NavigationService::GetGeneration();

    // Step 1a: Collect all completed results into a vector keyed by entity ID.
    // This avoids O(N*M) by decoupling task harvesting from entity queries.
    m_CompletedResults.clear();

    for (auto it = m_InFlightRequests.begin(); it != m_InFlightRequests.end();)
    {
        const auto& pending = it->second;
        if (!pending.Task.IsDone())
        {
            ++it;
            continue;
        }

        PendingCompletion completion;
        completion.EntityId = it->first;
        completion.Success = pending.Task.TryGetResult<Pathfinding::PathResult>(completion.Result);
        if (pending.WorldId != worldId || pending.ResetGeneration != resetGeneration ||
            pending.ServiceGeneration != serviceGeneration)
        {
            // Clear restarts entity generations; another World can also have
            // the same handle. Retire the old result without touching an actor.
            // A replaced service already destroyed its own PathBuffer: its
            // numeric path handles must never be freed in the new buffer.
            if (pending.ServiceGeneration == serviceGeneration && completion.Success &&
                completion.Result.Path.IsValid())
                pathBuffer.FreePath(completion.Result.Path);
            it = m_InFlightRequests.erase(it);
            continue;
        }
        m_CompletedResults.push_back(std::move(completion));
        it = m_InFlightRequests.erase(it);
    }

    // Step 1b: Single query pass to write back all completed results
    if (!m_CompletedResults.empty())
    {
        world.Query<ECS::Read<Components::NavigationAgent>,
                    ECS::Write<Components::NavigationAgentState>>().Each(
            [&](ECS::EntityHandle e, const Components::NavigationAgent& /*agent*/,
                Components::NavigationAgentState& state)
            {
                uint64 entityKey = static_cast<uint64>(e.id);
                const PendingCompletion* found = nullptr;
                for (const auto& entry : m_CompletedResults)
                {
                    if (entry.EntityId == entityKey)
                    {
                        found = &entry;
                        break;
                    }
                }
                if (!found)
                    return;

                if (found->Success)
                {
                    state.PathIndex = found->Result.Path.Index;
                    state.PathGeneration = found->Result.Path.Generation;
                    state.Status = static_cast<uint8>(found->Result.Status);
                    state.CurrentPathPointIndex = 0;
                    state.PathDirty = false;

                    if (found->Result.Status == Pathfinding::PathStatus::Complete ||
                        found->Result.Status == Pathfinding::PathStatus::Partial)
                    {
                        CacheWaypointsFromPath(state, pathBuffer);
                    }
                }
                else
                {
                    state.Status = static_cast<uint8>(Pathfinding::PathStatus::Failed);
                    state.PathDirty = false;
                    Logger::Log::Warning("[PathfindingECS] Path request failed for entity {}", e.id);
                }
            });

        // Free path handles for completed requests whose entities no longer exist.
        // These are orphaned paths that would otherwise leak in the PathBuffer.
        for (const auto& entry : m_CompletedResults)
        {
            if (entry.Success && entry.Result.Path.IsValid())
            {
                // Check if this completion was consumed by the entity query above.
                // If the entity was destroyed, the query never matched it — free the path.
                bool consumed = false;
                world.Query<ECS::Read<Components::NavigationAgentState>>().Each(
                    [&](ECS::EntityHandle e, const Components::NavigationAgentState& state)
                    {
                        if (consumed)
                            return;
                        if (static_cast<uint64>(e.id) == entry.EntityId &&
                            state.PathIndex == entry.Result.Path.Index &&
                            state.PathGeneration == entry.Result.Path.Generation)
                        {
                            consumed = true;
                        }
                    });
                if (!consumed)
                {
                    pathBuffer.FreePath(entry.Result.Path);
                }
            }
        }
    }

    // Step 1.5: Re-evaluation - periodically mark agents for re-pathing.
    // Budget-limited to MaxReplansPerFrame to avoid CPU spikes when many agents
    // have timers expiring on the same frame. Timers are staggered on first init
    // so agents spread their replans across frames.
    uint32 replansThisFrame = 0;
    world.Query<ECS::Read<Components::NavigationAgent>,
                ECS::Write<Components::NavigationAgentState>>().Each(
        [&](ECS::EntityHandle e, const Components::NavigationAgent& agent,
            Components::NavigationAgentState& state)
        {
            if (!state.HasDestination)
                return;

            auto status = static_cast<Pathfinding::PathStatus>(state.Status);
            // Retry Failed even when ReplanInterval is 0 (a game may set it) so the
            // agent moves once a blocked dest or missing map clears. Occupancy
            // is a path tax, not a Failed.
            if (status == Pathfinding::PathStatus::Failed)
            {
                if (state.ReplanTimer <= 0.0f && !state.PathDirty)
                {
                    state.PathDirty = true;
                    state.ReplanTimer = kFailedPathRetrySeconds;
                }
                else
                    state.ReplanTimer -= deltaTime;
                return;
            }

            if (agent.ReplanInterval <= 0.0f)
                return;
            if (status != Pathfinding::PathStatus::Complete &&
                status != Pathfinding::PathStatus::Partial)
                return;

            // Stagger timer initialization: on first tick (timer == 0 and path
            // is active), set the timer to a fraction of the interval based on a
            // simple hash of the agent's ID. This spreads replans across frames
            // instead of all agents replanning simultaneously.
            if (state.ReplanTimer <= 0.0f && !state.PathDirty)
            {
                if (replansThisFrame >= MaxReplansPerFrame)
                    return; // defer to next frame

                state.PathDirty = true;
                ++replansThisFrame;

                // Stagger the next replan using a cheap hash to distribute across frames.
                // Use AgentId if set, otherwise fall back to entity ID to avoid all-zero hash.
                uint32 hashInput = (agent.AgentId != 0) ? agent.AgentId : static_cast<uint32>(e.id);
                uint32 hash = hashInput * 2654435761u; // Knuth multiplicative hash
                float32 jitter = static_cast<float32>(hash & 0xFFu) / 255.0f; // 0..1
                state.ReplanTimer = agent.ReplanInterval * (0.5f + 0.5f * jitter);
            }
            else
            {
                state.ReplanTimer -= deltaTime;
            }
        });

    // Prepare all bodies before the first grid solve, regardless of query
    // order. NMS owns ordinary frame-to-frame stamps; no grid request means
    // there is nothing for this pass to prepare.
    bool hasGridRequest = false;
    world.Query<ECS::Read<Components::NavigationAgent>,
                ECS::Read<Components::NavigationAgentState>,
                ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle, const Components::NavigationAgent& agent,
            const Components::NavigationAgentState& state, const Components::WorldTransform&)
        {
            if (!hasGridRequest && state.HasDestination && state.PathDirty)
                hasGridRequest = navWorld->GetGridMap({agent.NavMapIndex, agent.NavMapGeneration}) != nullptr;
        });

    if (hasGridRequest)
    {
        world.Query<ECS::Read<Components::NavigationAgent>,
                    ECS::Write<Components::NavigationAgentState>,
                    ECS::Read<Components::WorldTransform>>().Each(
            [&](ECS::EntityHandle, const Components::NavigationAgent& agent,
                Components::NavigationAgentState& state, const Components::WorldTransform& wt)
            {
                if (agent.AgentId == 0)
                    return;
                auto* grid = navWorld->GetGridMap({agent.NavMapIndex, agent.NavMapGeneration});
                if (!grid)
                    return;

                const bool requesting = state.HasDestination && state.PathDirty;
                const auto status = static_cast<Pathfinding::PathStatus>(state.Status);
                if (state.HasDestination && !state.PathDirty &&
                    (status == Pathfinding::PathStatus::Complete || status == Pathfinding::PathStatus::Partial) &&
                    state.CurrentPathPointIndex < pathBuffer.GetPointCount({state.PathIndex, state.PathGeneration}))
                    return; // Preserve the native moving body's complete lookahead corridor.

                // A dirty replanner deliberately drops its old corridor once.
                // Idle/direct peers need rebuilding only if actual body cells
                // are missing, expired or at their previous position. The
                // HasCellReservations hint can outlive its expired cells.
                if (!requesting && HasCurrentBody(*grid, agent, wt.matrix[12], wt.matrix[14], m_AccumulatedTime))
                    return;
                StampNavigationAgentOccupancy(agent, state, wt.matrix[12], wt.matrix[14], m_AccumulatedTime);
            });
    }

    // Step 2: Submit only after every eligible body's preparation above.
    world.Query<ECS::Read<Components::NavigationAgent>,
                ECS::Write<Components::NavigationAgentState>,
                ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, const Components::NavigationAgent& agent,
            Components::NavigationAgentState& state,
            const Components::WorldTransform& wt)
        {
            if (!state.HasDestination || !state.PathDirty)
                return;

            uint64 entityKey = static_cast<uint64>(e.id);

            // Skip if there is already an in-flight request for this entity.
            // Don't free the old path yet: the in-flight completion replaces it,
            // or is retired above once its scope has moved on.
            // Its body was prepared before any request could start.
            if (m_InFlightRequests.count(entityKey) > 0)
                return;

            // Free the old path now that we know we'll submit a new request
            Pathfinding::PathHandle oldPath{state.PathIndex, state.PathGeneration};
            if (oldPath.IsValid())
            {
                pathBuffer.FreePath(oldPath);
                state.PathIndex = 0;
                state.PathGeneration = 0;
            }

            Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
            if (!mapHandle.IsValid())
                return;

            // Extract position from WorldTransform (column-major: [12],[13],[14] = translation)
            float32 posX = wt.matrix[12];
            float32 posY = wt.matrix[13];
            float32 posZ = wt.matrix[14];

            Pathfinding::PathRequest request;
            request.StartX = posX;
            request.StartY = posY;
            request.StartZ = posZ;
            request.EndX = state.DestinationX;
            request.EndY = state.DestinationY;
            request.EndZ = state.DestinationZ;
            request.AgentRadius = agent.Radius;
            request.AgentHeight = agent.Height;
            request.ExcludeAgentId = agent.AgentId;
            request.CurrentTime = m_AccumulatedTime;
            request.SmoothPath = agent.SmoothPaths;

            // Mark as pending immediately
            state.Status = static_cast<uint8>(Pathfinding::PathStatus::Pending);

            // Try async path request; fall back to synchronous if no task handle returned
            auto taskHandle = navWorld->RequestPath(mapHandle, request);
            if (taskHandle.IsValid())
            {
                m_InFlightRequests[entityKey] = {
                    std::move(taskHandle), worldId, resetGeneration, serviceGeneration};
            }
            else
            {
                // Synchronous fallback
                Pathfinding::PathHandle outPath;
                Pathfinding::PathStatus status = navWorld->FindPath(mapHandle, request, outPath);

                state.PathIndex = outPath.Index;
                state.PathGeneration = outPath.Generation;
                state.Status = static_cast<uint8>(status);
                state.CurrentPathPointIndex = 0;
                state.PathDirty = false;

                if (status == Pathfinding::PathStatus::Complete ||
                    status == Pathfinding::PathStatus::Partial)
                {
                    CacheWaypointsFromPath(state, pathBuffer);
                }
            }
        });
}

} // namespace GameEngine::PathfindingECS
