# Pathfinding Module Architecture

## Overview

The pathfinding system is split into two modules following the Physics/PhysicsECS pattern:

- **Pathfinding** (`Engine/Modules/Pathfinding/`) -- Core library. No ECS dependency. Owns navigation maps, solvers, path storage, and obstacle avoidance.
- **PathfindingECS** (`Engine/Modules/PathfindingECS/`) -- ECS integration. Components, systems, and the `NavigationService` singleton that bridges the core library to the entity world.

### Dependency Chain

```
Pathfinding --> Logger, Types, Mathematics, JobSystem
  + optional: RecastNavigation (Recast, Detour, DetourCrowd) via GE_PATHFINDING_BACKEND_DETOUR

PathfindingECS --> Pathfinding, ECS, ECSComponents, Types, Mathematics, Logger
```

Both modules are linked into the `Engine` target.

---

## Core Module (`Pathfinding`)

### Directory Layout

```
Include/Pathfinding/
  PathfindingTypes.h       Types, enums, handles (PathPoint, PathHandle, NavMapHandle, NavLink, etc.)
  PathBuffer.h             Pool of variable-length path buffers (mutex-guarded)
  INavigationMap.h         Abstract interface for all navigation maps
  GridMap.h                Square + hex grid map (costs, heights, blocked, reservations)
  DetourNavMap.h           Recast baking + Detour runtime queries (Pimpl)
  ObstacleAvoidance.h      CrowdManager wrapping dtCrowd
  NavigationWorld.h        Facade: owns maps, PathBuffer, CrowdManager, NavLinks, async API

Source/
  PathBuffer.cpp
  GridMap.cpp
  AStarSolver.h / .cpp     Private. Stateless, thread-safe A* implementation.
  HexGridHelpers.h / .cpp  Private. Odd-r offset hex coordinate math.
  DetourNavMap.cpp          Full Recast pipeline + Detour queries (behind #if guard)
  ObstacleAvoidance.cpp     dtCrowd wrapper (behind #if guard)
  NavigationWorld.cpp       Facade with map/link management, sync/async path queries
```

### Key Abstractions

#### INavigationMap

Abstract interface implemented by all navigation map types.

```
FindPath(request, pathBuffer, outPath) --> PathStatus
IsPointNavigable(x, y, z, radius) --> bool
GetClosestNavigablePoint(x, y, z, searchRadius, out) --> bool
Raycast(start, end, hitPoint) --> bool
```

#### GridMap

In-house grid-based navigation map. Supports both square (8-neighbor) and hex (6-neighbor, odd-r offset) grids.

**Data layout**: Three flat contiguous arrays indexed by `cellZ * width + cellX`:
- `m_Costs` (`float32`) -- per-cell traversal cost multiplier (default 1.0)
- `m_Heights` (`float32`) -- per-cell world Y height
- `m_Blocked` (`uint8`) -- per-cell blocked flag

**Reservations**: A parallel flat array `m_Reservations` stores `(agentId, expirationTime)` per cell. Agents reserve cells along their path within a time horizon. The A* solver adds a penalty cost for cells reserved by other agents (expensive, not impassable). A `shared_mutex` protects the reservation array for concurrent reads (A* queries) and exclusive writes (reserve/clear).

**Coordinate conversion**: `WorldToCell` / `CellToWorld` handle both square and hex layouts. Hex uses odd-r offset coordinates with flat-top orientation.

**Baking**: `BakeHeights(heightQueryFn)` accepts a callback so the module doesn't depend on Physics. The callback is provided by the ECS layer using `PhysicsWorld::RayCast`.

#### A* Solver

Private, stateless class. All working memory is local to each `Solve()` call, so multiple requests run concurrently on different threads.

- Open list: array-backed binary heap (`std::vector<Node>`)
- Closed set: flat `std::vector<uint8>`
- gScore + parent: flat `std::vector<float32>` / `std::vector<uint32>`
- Square: 8 neighbors, octile heuristic, diagonal cost = 1.414
- Hex: 6 neighbors, hex distance heuristic
- Step cost = `base_distance * cell_cost * (1 + kReservationPenalty)` if reserved
- Terrain checks: slope angle vs MaxSlope, height difference vs MaxStepHeight
- Iteration limit: `min(cellCount * 2, 65536)`

#### DetourNavMap

Wraps Recast (baking) and Detour (runtime queries) behind a Pimpl.

- `Build(settings, geometry)` -- full Recast pipeline: heightfield, filtering, compact heightfield, erosion, regions, contours, poly mesh, detail mesh, dtNavMesh creation
- `FindPath` -- `dtNavMeshQuery::findPath` + `findStraightPath`, results stored in PathBuffer
- `Serialize` / `Deserialize` -- dtNavMesh tile data for asset caching
- Debug mesh extraction for visualization

All behind `#if GE_PATHFINDING_BACKEND_DETOUR`. When disabled, all methods return false/empty.

#### CrowdManager

Wraps Detour's `dtCrowd` for navmesh obstacle avoidance.

- `Initialize(navMap, maxAgents)` -- sets up dtCrowd with 4 quality presets
- `AddAgent` / `RemoveAgent` -- manage crowd agents
- `SetAgentTarget` / `GetAgentTarget` -- destination management
- `GetAgentPosition` / `GetAgentVelocity` -- read simulation results
- `Update(deltaTime)` -- steps the crowd simulation

#### NavigationWorld

Top-level facade that owns everything.

- **Map management**: `AddGridMap`, `AddDetourNavMap`, `RemoveMap` with generational handle validation
- **Sync queries**: `FindPath(mapHandle, request, outPath)` -- blocks, for editor/tools
- **Async queries**: `RequestPath(mapHandle, request)` -- submits to JobSystem, returns `TaskHandle`
- **Cross-map**: `RequestPathAcrossLinks(request)` -- BFS on link graph to find map sequence, then solves per-segment
- **NavLinks**: `AddNavLink`, `RemoveNavLink`, `GetNavLink`, `ForEachNavLink`, `AutoGenerateBoundaryLinks`
- **PathBuffer**: Thread-safe pool of variable-length path buffers

#### PathBuffer

Pool that stores full waypoint sequences. Each path is a `std::vector<PathPoint>` in a slot with a generation counter. A `std::mutex` guards all operations.

- `AllocatePath(points, count)` --> `PathHandle` (index + generation)
- `FreePath(handle)` -- returns slot to free list
- `GetPoint(handle, index)` -- returns a copy (safe across threads)
- `CopyPoints(handle, outBuffer, capacity)` -- bulk copy into caller-owned buffer
- `GetPointCount(handle)` -- count of waypoints

---

## ECS Module (`PathfindingECS`)

### Directory Layout

```
Include/PathfindingECS/
  NavigationService.h                Singleton service + NavDebugData struct
  Components/
    NavigationAgent.h                Agent config, path state, velocity, crowd handle
    NavigationGrid.h                 Grid map ownership and bake config
    NavigationMesh.h                 Navmesh ownership and build config
    NavigationObstacle.h             Dynamic obstacle marker
    NavigationDebugSettings.h        Debug visualization toggles
  Systems/
    NavigationBuildSystem.h               Creates/bakes navigation maps
    NavigationPathfindingSystem.h           Computes paths for agents
    NavigationMovementSystem.h              Steers agents along paths
    NavigationDebugSystem.h             Generates debug visualization data
    RegisterPathfindingSystems.h     Schedule registration

Source/
  NavigationService.cpp
  Systems/
    NavigationBuildSystem.cpp
    NavigationPathfindingSystem.cpp
    NavigationMovementSystem.cpp
    NavigationDebugSystem.cpp
    RegisterPathfindingSystems.cpp
```

### Components (namespace `GameEngine::components`, all POD)

| Component | Purpose |
|-----------|---------|
| **NavigationAgent** | Speed, acceleration, radius, height, avoidance settings, nav map handle, destination, path handle, cached waypoints, velocity, crowd handle, agent ID |
| **NavigationGrid** | Grid type, cell size, dimensions, slope/step limits, nav map handle, initialized/needs-rebake flags |
| **NavigationMesh** | Recast build parameters, nav map handle, initialized/needs-rebuild flags |
| **NavigationObstacle** | Radius, height, enabled flag |
| **NavigationDebugSettings** | ShowGrid, ShowNavMesh, ShowPaths, ShowAgentRadii, ShowCellCosts, ShowReservations, line thicknesses |

### Systems

| System | Phase | Order | Dependencies | Purpose |
|--------|-------|-------|-------------|---------|
| **NavigationBuildSystem** | Early | 10 | -- | Creates GridMap/DetourNavMap from source components, bakes heights |
| **NavigationPathfindingSystem** | Early | 20 | NavigationBuildSystem | Harvests async results, submits new path requests for dirty agents |
| **NavigationMovementSystem** | Early | 30 | NavigationPathfindingSystem | Steers agents along paths, manages reservations, reads crowd positions |
| **NavigationDebugSystem** | Render | 5 | NavigationMovementSystem | Populates NavDebugData with lines/triangles for editor consumption |

### Data Flow

```
1. Entity has NavigationGrid --> NavigationBuildSystem creates GridMap, stores handle
2. Entity has NavigationAgent + HasDestination=true --> NavigationPathfindingSystem computes path
3. Path stored in PathBuffer, handle + cached waypoints written to NavigationAgent
4. NavigationMovementSystem steers agent toward waypoints, reserves grid cells
5. NavigationDebugSystem generates visualization --> NavDebugData --> Editor gizmos
```

### NavigationService

Singleton following `PhysicsWorldService` pattern.

- `Initialize()` / `Shutdown()` -- creates/destroys the `NavigationWorld`
- `Get()` / `TryGet()` -- access the world
- `GetGeneration()` -- monotonic counter, increments on init/shutdown
- `GetDebugData()` -- returns `NavDebugData` (lines + triangles for editor rendering)

### NavDebugData

Generated each frame by `NavigationDebugSystem`. Contains:
- **Lines**: grid edges (color-coded by cost), reservation outlines (orange), path strips (blue), agent radius circles (cyan)
- **Triangles**: navmesh faces (semi-transparent green)

The Editor layer is responsible for consuming this data and drawing it via `GizmoRenderContext`.

---

## Thread Safety

| Resource | Protection | Details |
|----------|-----------|---------|
| PathBuffer | `std::mutex` | All alloc/free/read operations lock |
| GridMap reservations | `std::shared_mutex` | Readers (A*, GetCellReservation) share; writers (Reserve, Clear) exclusive |
| GridMap cell data | None (read-only during queries) | Cell costs/heights/blocked are set during bake, read-only during pathfinding |
| A* solver | Stateless | All working memory is stack-local per Solve() call |
| NavigationWorld maps | None (single-threaded access assumed) | Map add/remove happens in init systems; queries happen in pathfinding systems |
| NavigationPathfindingSystem::m_InFlightRequests | None (single-threaded system) | Systems run sequentially per schedule |

---

## Recast/Detour Integration

Added via vcpkg (`recastnavigation` port, community-maintained repo). Guarded by `GE_PATHFINDING_BACKEND_DETOUR` CMake option (ON by default).

When disabled, `DetourNavMap` and `CrowdManager` compile as stubs returning false/empty for all operations. Grid-based pathfinding works independently of this flag.

---

## NavMesh Baking Pipeline

`SceneGeometryCollector` (in PathfindingECS) gathers CPU-side mesh vertex and index data from the ECS world. It queries all entities with `MeshRenderer` + `WorldTransform`, loads the referenced `ModelAsset`, and transforms each submesh's vertices into world space using `sourceNodeTransform * worldTransform` (column-major multiplication).

When an entity's `NavigationMesh` component has `NeedsRebuild` set, `NavigationBuildSystem` drives the pipeline:

1. **Cache check** -- If the entity has an asset GUID, `NavCacheManager::LoadNavMeshCache` attempts to load a previously serialized navmesh. On hit, `DetourNavMap::Deserialize` restores it and baking is skipped.
2. **Geometry collection** -- If the `NavMeshAsset` specifies explicit source geometry GUIDs, only those models are collected (via `CollectGeometryFromAssets`, model-space only). Otherwise, `CollectSceneGeometry` gathers all renderable geometry in the scene.
3. **Bake** -- The collected vertices and indices are passed to `DetourNavMap::Build` along with `NavMeshSettings` from the component (cell size, agent radius, etc.).
4. **Cache save** -- On successful build, the navmesh is serialized and stored via `NavCacheManager::SaveNavMeshCache` for future loads.
