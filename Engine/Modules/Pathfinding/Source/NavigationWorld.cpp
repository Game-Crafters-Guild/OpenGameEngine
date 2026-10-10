#include "Pathfinding/NavigationWorld.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::Pathfinding
{

namespace
{
// One counter for every NavigationWorld in the process. A handle a destroyed
// or sibling instance handed out can never name a map created later by another
// instance, so a stale handle held across a service rebuild resolves to nothing
// instead of an unrelated map. Relaxed: uniqueness needs no ordering.
std::atomic<uint32> s_NextMapGeneration{1};

uint32 NextMapGeneration()
{
    return s_NextMapGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Decrements the active-job count when the job body exits (any path, including
// early returns). The release decrement pairs with the acquire load in
// HasActivePathJobs: a caller that reads zero sees every map access the job
// made. Must be the job body's first local so it destructs last — after the
// returned PathResult (which touches no maps) is materialized.
struct ActivePathJobGuard
{
    std::atomic<uint32>& Count;
    ~ActivePathJobGuard() { Count.fetch_sub(1, std::memory_order_release); }
};

// Balances the pre-Submit increment if Submit itself throws; dismissed once the
// job is enqueued (the job body's ActivePathJobGuard then owns the decrement).
// Without this, a failed Submit would leave the count permanently nonzero and
// wedge the map-mutation gate shut for the session.
struct SubmitCountGuard
{
    std::atomic<uint32>* Count;
    ~SubmitCountGuard()
    {
        if (Count)
            Count->fetch_sub(1, std::memory_order_release);
    }
    void Dismiss() { Count = nullptr; }
};

} // anonymous namespace

NavigationWorld::NavigationWorld() = default;
NavigationWorld::~NavigationWorld() = default;

NavigationWorld::MapEntry* NavigationWorld::GetMapEntry(NavMapHandle handle)
{
    if (handle.Index >= m_Maps.size())
        return nullptr;
    auto& entry = m_Maps[handle.Index];
    if (!entry.Active || entry.Generation != handle.Generation)
        return nullptr;
    return &entry;
}

const NavigationWorld::MapEntry* NavigationWorld::GetMapEntry(NavMapHandle handle) const
{
    if (handle.Index >= m_Maps.size())
        return nullptr;
    const auto& entry = m_Maps[handle.Index];
    if (!entry.Active || entry.Generation != handle.Generation)
        return nullptr;
    return &entry;
}

NavMapHandle NavigationWorld::AddGridMap(const GridSettings& settings)
{
    uint32 index = 0;
    if (!m_FreeMapSlots.empty())
    {
        index = m_FreeMapSlots.back();
        m_FreeMapSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Maps.size());
        m_Maps.emplace_back();
    }

    MapEntry& entry = m_Maps[index];
    entry.Map = std::make_unique<GridMap>(settings);
    entry.Type = MapType::Grid;
    entry.Active = true;
    entry.Generation = NextMapGeneration();

    return NavMapHandle{index, entry.Generation};
}

NavMapHandle NavigationWorld::AddDetourNavMap()
{
    uint32 index = 0;
    if (!m_FreeMapSlots.empty())
    {
        index = m_FreeMapSlots.back();
        m_FreeMapSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Maps.size());
        m_Maps.emplace_back();
    }

    MapEntry& entry = m_Maps[index];
    entry.Map = std::make_unique<DetourNavMap>();
    entry.Type = MapType::DetourNavMesh;
    entry.Active = true;
    entry.Generation = NextMapGeneration();

    return NavMapHandle{index, entry.Generation};
}

void NavigationWorld::RemoveMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry)
        return;

    entry->Map.reset();
    entry->Active = false;
    m_FreeMapSlots.push_back(handle.Index);
}

GridMap* NavigationWorld::GetGridMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry || entry->Type != MapType::Grid)
        return nullptr;
    return static_cast<GridMap*>(entry->Map.get());
}

DetourNavMap* NavigationWorld::GetDetourNavMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry || entry->Type != MapType::DetourNavMesh)
        return nullptr;
    return static_cast<DetourNavMap*>(entry->Map.get());
}

INavigationMap* NavigationWorld::GetMap(NavMapHandle handle)
{
    MapEntry* entry = GetMapEntry(handle);
    if (!entry)
        return nullptr;
    return entry->Map.get();
}

PathStatus NavigationWorld::FindPath(NavMapHandle map, const PathRequest& request, PathHandle& outPath)
{
    INavigationMap* navMap = GetMap(map);
    if (!navMap)
        return PathStatus::Failed;
    return navMap->FindPath(request, m_PathBuffer, outPath);
}

JobSystem::TaskHandle NavigationWorld::RequestPath(NavMapHandle map, const PathRequest& request)
{
    if (!m_JobSystem)
        return JobSystem::TaskHandle{};

    // release: the increment must be visible before the job can run, without
    // leaning on Submit's queue publish to order it.
    m_ActivePathJobs.fetch_add(1, std::memory_order_release);
    SubmitCountGuard submitGuard{&m_ActivePathJobs};
    JobSystem::TaskHandle handle = m_JobSystem->Submit([this, map, request]() -> PathResult
    {
        ActivePathJobGuard jobGuard{m_ActivePathJobs};

        PathResult result;
        INavigationMap* navMap = GetMap(map);
        if (!navMap)
        {
            result.Status = PathStatus::Failed;
            return result;
        }

        result.Status = navMap->FindPath(request, m_PathBuffer, result.Path);
        return result;
    });
    submitGuard.Dismiss();
    return handle;
}

JobSystem::TaskHandle NavigationWorld::RequestPathAcrossLinks(const PathRequest& request)
{
    if (!m_JobSystem)
        return JobSystem::TaskHandle{};

    // release: the increment must be visible before the job can run, without
    // leaning on Submit's queue publish to order it.
    m_ActivePathJobs.fetch_add(1, std::memory_order_release);
    SubmitCountGuard submitGuard{&m_ActivePathJobs};
    JobSystem::TaskHandle handle = m_JobSystem->Submit([this, request]() -> PathResult
    {
        ActivePathJobGuard jobGuard{m_ActivePathJobs};

        PathResult result;
        result.Status = PathStatus::Failed;

        // Build adjacency graph from NavLinks
        // Key: NavMapHandle, Value: list of (targetMapHandle, linkEntry index)
        std::unordered_map<NavMapHandle, std::vector<std::pair<NavMapHandle, uint32>>, NavMapHandleHash> adjacency;

        for (uint32 i = 0; i < static_cast<uint32>(m_Links.size()); ++i)
        {
            const auto& linkEntry = m_Links[i];
            if (!linkEntry.Active)
                continue;

            adjacency[linkEntry.Link.SourceMap].push_back({linkEntry.Link.TargetMap, i});
            if (linkEntry.Link.Bidirectional)
            {
                adjacency[linkEntry.Link.TargetMap].push_back({linkEntry.Link.SourceMap, i});
            }
        }

        // Find which map contains the start point
        NavMapHandle startMap{};
        NavMapHandle endMap{};

        for (uint32 i = 0; i < static_cast<uint32>(m_Maps.size()); ++i)
        {
            const auto& entry = m_Maps[i];
            if (!entry.Active)
                continue;

            NavMapHandle handle{i, entry.Generation};

            if (!startMap.IsValid() && entry.Map->IsPointNavigable(request.StartX, request.StartY, request.StartZ, request.AgentRadius))
            {
                startMap = handle;
            }
            if (!endMap.IsValid() && entry.Map->IsPointNavigable(request.EndX, request.EndY, request.EndZ, request.AgentRadius))
            {
                endMap = handle;
            }
            if (startMap.IsValid() && endMap.IsValid())
                break;
        }

        if (!startMap.IsValid() || !endMap.IsValid())
            return result;

        // If same map, direct path
        if (startMap == endMap)
        {
            INavigationMap* navMap = GetMap(startMap);
            if (navMap)
            {
                result.Status = navMap->FindPath(request, m_PathBuffer, result.Path);
            }
            return result;
        }

        // Dijkstra to find cheapest map sequence from startMap to endMap,
        // weighted by NavLink::TraversalCost.
        struct DijkNode
        {
            NavMapHandle Map;
            uint32 ParentIndex;
            uint32 LinkIndex;
            float32 Cost;
        };

        std::vector<DijkNode> nodes;
        std::unordered_map<NavMapHandle, float32, NavMapHandleHash> bestCost;

        nodes.push_back({startMap, UINT32_MAX, UINT32_MAX, 0.0f});
        bestCost[startMap] = 0.0f;

        // Simple priority queue via linear scan (few maps expected)
        uint32 endNodeIndex = UINT32_MAX;

        for (uint32 front = 0; front < static_cast<uint32>(nodes.size()); ++front)
        {
            // Find unvisited node with lowest cost (linear scan — fine for small graph)
            uint32 best = UINT32_MAX;
            float32 bestVal = std::numeric_limits<float32>::max();
            for (uint32 k = front; k < static_cast<uint32>(nodes.size()); ++k)
            {
                if (nodes[k].Cost < bestVal)
                {
                    bestVal = nodes[k].Cost;
                    best = k;
                }
            }
            if (best == UINT32_MAX)
                break;
            // Swap to front position
            if (best != front)
                std::swap(nodes[front], nodes[best]);

            const DijkNode& cur = nodes[front];

            if (cur.Map == endMap)
            {
                endNodeIndex = front;
                break;
            }

            auto it = adjacency.find(cur.Map);
            if (it == adjacency.end())
                continue;

            for (const auto& [targetMap, linkIdx] : it->second)
            {
                float32 linkCost = m_Links[linkIdx].Link.TraversalCost;
                float32 newCost = cur.Cost + linkCost;

                auto costIt = bestCost.find(targetMap);
                if (costIt != bestCost.end() && costIt->second <= newCost)
                    continue;

                bestCost[targetMap] = newCost;
                nodes.push_back({targetMap, front, linkIdx, newCost});
            }
        }

        if (endNodeIndex == UINT32_MAX)
            return result;

        // Reconstruct map sequence and link sequence
        struct Segment
        {
            NavMapHandle Map;
            uint32 LinkIndex;
        };

        std::vector<Segment> mapSequence;
        uint32 nodeIdx = endNodeIndex;
        while (nodeIdx != UINT32_MAX)
        {
            mapSequence.push_back({nodes[nodeIdx].Map, nodes[nodeIdx].LinkIndex});
            nodeIdx = nodes[nodeIdx].ParentIndex;
        }
        std::reverse(mapSequence.begin(), mapSequence.end());

        // Solve path within each segment and concatenate
        std::vector<PathPoint> allPoints;

        for (uint32 s = 0; s < static_cast<uint32>(mapSequence.size()); ++s)
        {
            INavigationMap* navMap = GetMap(mapSequence[s].Map);
            if (!navMap)
                return result;

            PathRequest segRequest;
            if (s == 0)
            {
                segRequest.StartX = request.StartX;
                segRequest.StartY = request.StartY;
                segRequest.StartZ = request.StartZ;
            }
            else
            {
                // Start from the link target point
                const auto& link = m_Links[mapSequence[s].LinkIndex].Link;
                if (link.TargetMap == mapSequence[s].Map)
                {
                    segRequest.StartX = link.TargetX;
                    segRequest.StartY = link.TargetY;
                    segRequest.StartZ = link.TargetZ;
                }
                else
                {
                    segRequest.StartX = link.SourceX;
                    segRequest.StartY = link.SourceY;
                    segRequest.StartZ = link.SourceZ;
                }
            }

            if (s == static_cast<uint32>(mapSequence.size()) - 1)
            {
                segRequest.EndX = request.EndX;
                segRequest.EndY = request.EndY;
                segRequest.EndZ = request.EndZ;
            }
            else
            {
                // End at the link source point on this map
                uint32 nextLinkIdx = mapSequence[s + 1].LinkIndex;
                const auto& link = m_Links[nextLinkIdx].Link;
                if (link.SourceMap == mapSequence[s].Map)
                {
                    segRequest.EndX = link.SourceX;
                    segRequest.EndY = link.SourceY;
                    segRequest.EndZ = link.SourceZ;
                }
                else
                {
                    segRequest.EndX = link.TargetX;
                    segRequest.EndY = link.TargetY;
                    segRequest.EndZ = link.TargetZ;
                }
            }

            segRequest.AgentRadius = request.AgentRadius;
            segRequest.AgentHeight = request.AgentHeight;
            segRequest.ExcludeAgentId = request.ExcludeAgentId;
            segRequest.CurrentTime = request.CurrentTime;
            segRequest.SmoothPath = request.SmoothPath;

            PathHandle segPath;
            PathStatus segStatus = navMap->FindPath(segRequest, m_PathBuffer, segPath);
            if (segStatus == PathStatus::Failed)
                return result;

            // Append points, skipping the first point of subsequent segments to avoid duplicates
            uint32 pointCount = m_PathBuffer.GetPointCount(segPath);
            if (pointCount > 0)
            {
                std::vector<PathPoint> segPoints(pointCount);
                m_PathBuffer.CopyPoints(segPath, segPoints.data(), pointCount);
                uint32 startIdx = (s > 0) ? 1 : 0;
                for (uint32 p = startIdx; p < pointCount; ++p)
                {
                    allPoints.push_back(segPoints[p]);
                }
            }

            m_PathBuffer.FreePath(segPath);
        }

        if (allPoints.empty())
            return result;

        result.Path = m_PathBuffer.AllocatePath(allPoints.data(), static_cast<uint32>(allPoints.size()));
        result.Status = PathStatus::Complete;
        return result;
    });
    submitGuard.Dismiss();
    return handle;
}

bool NavigationWorld::HasActivePathJobs() const
{
    return m_ActivePathJobs.load(std::memory_order_acquire) != 0;
}

NavLinkId NavigationWorld::AddNavLink(const NavLink& link)
{
    uint32 index = 0;
    if (!m_FreeLinkSlots.empty())
    {
        index = m_FreeLinkSlots.back();
        m_FreeLinkSlots.pop_back();
    }
    else
    {
        index = static_cast<uint32>(m_Links.size());
        m_Links.emplace_back();
    }

    LinkEntry& entry = m_Links[index];
    entry.Link = link;
    entry.Active = true;
    entry.Generation = m_NextLinkGeneration++;

    return NavLinkId{index, entry.Generation};
}

void NavigationWorld::RemoveNavLink(NavLinkId id)
{
    if (id.Index >= m_Links.size())
        return;

    LinkEntry& entry = m_Links[id.Index];
    if (!entry.Active || entry.Generation != id.Generation)
        return;

    entry.Active = false;
    m_FreeLinkSlots.push_back(id.Index);
}

const NavLink* NavigationWorld::GetNavLink(NavLinkId id) const
{
    if (id.Index >= m_Links.size())
        return nullptr;
    const auto& entry = m_Links[id.Index];
    if (!entry.Active || entry.Generation != id.Generation)
        return nullptr;
    return &entry.Link;
}

uint32 NavigationWorld::GetActiveNavLinkCount() const
{
    uint32 count = 0;
    for (const auto& entry : m_Links)
    {
        if (entry.Active)
            ++count;
    }
    return count;
}

void NavigationWorld::ForEachNavLink(const NavLinkVisitor& visitor) const
{
    for (uint32 i = 0; i < static_cast<uint32>(m_Links.size()); ++i)
    {
        const auto& entry = m_Links[i];
        if (!entry.Active)
            continue;
        NavLinkId id{i, entry.Generation};
        if (!visitor(id, entry.Link))
            return;
    }
}

void NavigationWorld::AutoGenerateBoundaryLinks(NavMapHandle mapA, NavMapHandle mapB)
{
    GridMap* gridA = GetGridMap(mapA);
    GridMap* gridB = GetGridMap(mapB);

    if (!gridA || !gridB)
        return;

    const auto& settingsA = gridA->GetSettings();
    const auto& settingsB = gridB->GetSettings();

    // Check all boundary cells of map A against boundary cells of map B
    // A boundary cell is on the edge of the grid

    // For each edge cell on map A, compute its world position and check if
    // it's adjacent to an edge cell on map B (within tolerance of one cell size)
    const float32 tolerance = std::max(settingsA.CellSize, settingsB.CellSize) * 0.75f;

    // Collect boundary cells from map A
    struct BoundaryCell
    {
        uint32 CellX;
        uint32 CellZ;
        float32 WorldX;
        float32 WorldZ;
    };

    auto collectBoundaryCells = [](const GridMap* grid, const GridSettings& settings) -> std::vector<BoundaryCell>
    {
        std::vector<BoundaryCell> cells;
        cells.reserve((settings.Width + settings.Depth) * 2);

        for (uint32 x = 0; x < settings.Width; ++x)
        {
            for (uint32 z = 0; z < settings.Depth; ++z)
            {
                if (x == 0 || x == settings.Width - 1 || z == 0 || z == settings.Depth - 1)
                {
                    if (grid->IsCellBlocked(x, z))
                        continue;

                    float32 worldX = 0.0f, worldZ = 0.0f;
                    grid->CellToWorld(x, z, worldX, worldZ);
                    cells.push_back({x, z, worldX, worldZ});
                }
            }
        }
        return cells;
    };

    auto boundaryCellsA = collectBoundaryCells(gridA, settingsA);
    auto boundaryCellsB = collectBoundaryCells(gridB, settingsB);

    // Sort B cells by WorldX to enable binary-search narrowing: O(N*log(M) + M*log(M))
    std::sort(boundaryCellsB.begin(), boundaryCellsB.end(),
              [](const BoundaryCell& a, const BoundaryCell& b) { return a.WorldX < b.WorldX; });

    const float32 toleranceSq = tolerance * tolerance;

    for (const auto& cellA : boundaryCellsA)
    {
        // Find the range of B cells whose WorldX is within tolerance of cellA.WorldX
        const float32 minX = cellA.WorldX - tolerance;
        const float32 maxX = cellA.WorldX + tolerance;

        auto lower = std::lower_bound(boundaryCellsB.begin(), boundaryCellsB.end(), minX,
                                       [](const BoundaryCell& cell, float32 val) { return cell.WorldX < val; });

        for (auto it = lower; it != boundaryCellsB.end() && it->WorldX <= maxX; ++it)
        {
            const float32 dx = cellA.WorldX - it->WorldX;
            const float32 dz = cellA.WorldZ - it->WorldZ;
            const float32 distSq = dx * dx + dz * dz;

            if (distSq <= toleranceSq)
            {
                NavLink link;
                link.SourceMap = mapA;
                link.SourceX = cellA.WorldX;
                link.SourceY = gridA->GetCellHeight(cellA.CellX, cellA.CellZ);
                link.SourceZ = cellA.WorldZ;
                link.TargetMap = mapB;
                link.TargetX = it->WorldX;
                link.TargetY = gridB->GetCellHeight(it->CellX, it->CellZ);
                link.TargetZ = it->WorldZ;
                link.TraversalCost = 1.0f;
                link.Bidirectional = true;

                AddNavLink(link);
            }
        }
    }
}

PathBuffer& NavigationWorld::GetPathBuffer()
{
    return m_PathBuffer;
}

const PathBuffer& NavigationWorld::GetPathBuffer() const
{
    return m_PathBuffer;
}

CrowdManager& NavigationWorld::GetCrowdManager()
{
    return m_CrowdManager;
}

void NavigationWorld::SetJobSystem(JobSystem::WorkStealingThreadPool* jobSystem)
{
    m_JobSystem = jobSystem;
}

} // namespace GameEngine::Pathfinding
