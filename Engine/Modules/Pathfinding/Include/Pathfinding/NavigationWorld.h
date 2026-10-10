#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/ObstacleAvoidance.h"

#include "JobSystem/TaskHandle.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Pathfinding
{

// Thread safety: NavigationWorld is NOT internally synchronized for map/link
// management. Map add/remove and NavLink mutations must occur on a single thread
// (typically the main/build thread). PathBuffer operations ARE mutex-protected.
// Async path requests (RequestPath, RequestPathAcrossLinks) capture `this` and
// run on job threads — callers must not modify maps while jobs are in flight:
// gate mutations on HasActivePathJobs() returning false.
// The goal is full thread safety in the future.
class NavigationWorld
{
public:
    NavigationWorld();
    ~NavigationWorld();

    // Map management
    NavMapHandle AddGridMap(const GridSettings& settings);
    NavMapHandle AddDetourNavMap();
    void RemoveMap(NavMapHandle handle);

    GridMap* GetGridMap(NavMapHandle handle);
    DetourNavMap* GetDetourNavMap(NavMapHandle handle);
    INavigationMap* GetMap(NavMapHandle handle);

    // Synchronous path query (blocks)
    PathStatus FindPath(NavMapHandle map, const PathRequest& request, PathHandle& outPath);

    // Asynchronous path query via JobSystem (non-blocking, returns TaskHandle)
    JobSystem::TaskHandle RequestPath(NavMapHandle map, const PathRequest& request);

    // Cross-map pathfinding via NavLinks
    JobSystem::TaskHandle RequestPathAcrossLinks(const PathRequest& request);

    // True while any async path job (RequestPath / RequestPathAcrossLinks) has
    // not finished executing. A false return orders every finished job's map
    // reads before the caller's subsequent writes (acquire load paired with the
    // job-side release decrement), so map mutation is safe exactly then.
    bool HasActivePathJobs() const;

    // NavLink management
    NavLinkId AddNavLink(const NavLink& link);
    void RemoveNavLink(NavLinkId id);
    void AutoGenerateBoundaryLinks(NavMapHandle mapA, NavMapHandle mapB);

    // NavLink queries
    const NavLink* GetNavLink(NavLinkId id) const;
    uint32 GetActiveNavLinkCount() const;

    using NavLinkVisitor = std::function<bool(NavLinkId, const NavLink&)>;
    void ForEachNavLink(const NavLinkVisitor& visitor) const;

    // Path buffer access
    PathBuffer& GetPathBuffer();
    const PathBuffer& GetPathBuffer() const;

    // Crowd manager for Detour-based obstacle avoidance
    CrowdManager& GetCrowdManager();

    // Job system
    void SetJobSystem(JobSystem::WorkStealingThreadPool* jobSystem);

private:
    enum class MapType : uint8 { Grid, DetourNavMesh };
    struct MapEntry
    {
        std::unique_ptr<INavigationMap> Map;
        MapType Type = MapType::Grid;
        bool Active = false;
        uint32 Generation = 0;
    };

    struct LinkEntry
    {
        NavLink Link;
        bool Active = false;
        uint32 Generation = 0;
    };

    MapEntry* GetMapEntry(NavMapHandle handle);
    const MapEntry* GetMapEntry(NavMapHandle handle) const;

    std::vector<MapEntry> m_Maps;
    std::vector<uint32> m_FreeMapSlots;

    std::vector<LinkEntry> m_Links;
    std::vector<uint32> m_FreeLinkSlots;
    uint32 m_NextLinkGeneration = 1;

    PathBuffer m_PathBuffer;
    CrowdManager m_CrowdManager;
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;

    // Count of path jobs submitted but not yet finished executing. Incremented
    // before Submit; decremented (release) as the job body's final act.
    std::atomic<uint32> m_ActivePathJobs{0};
};

} // namespace GameEngine::Pathfinding
