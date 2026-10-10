#pragma once
#include "ECS/Systems.h"
#include "Assets/FileWatchingService.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine::Pathfinding
{
class NavigationWorld;
} // namespace GameEngine::Pathfinding

namespace GameEngine::PhysicsECS
{
struct HeightFieldCollisionWait;
} // namespace GameEngine::PhysicsECS

namespace GameEngine::PathfindingECS
{

class NavigationBuildSystem : public ECS::ISystem
{
public:
    NavigationBuildSystem();
    ~NavigationBuildSystem() override;

    const char* GetName() const override { return "NavigationBuildSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Rasterizes NavigationObstacle footprints into every grid's dynamic-blocked
    // cells (clear + reapply). Lives here, not in NavigationMovementSystem,
    // because dynamic-blocked is an unsynchronized field read by in-flight path
    // jobs — every write to it must sit behind this system's job-free gate.
    void ApplyDynamicObstacles(ECS::World& world);

    // Keeps the .navgrid/.navmesh watch pointed at the current project asset
    // root, re-subscribing when the root moves. Called every update; gated on
    // the asset manager's source-set version so the common frame costs one
    // atomic load. Does nothing while watching is unarmed or no project is
    // mounted, and retries on the next mount or rebind.
    void UpdateNavigationAssetWatch();

    // Counts a frame of a grid's wait for heightfield collision and logs the
    // blocking entity and condition when the streak starts and when it has lasted
    // kGateStarvationWarnFrames.
    void NoteCollisionWait(ECS::EntityHandle grid, const PhysicsECS::HeightFieldCollisionWait& wait);

    // Consecutive frames the job-free gate has deferred map mutation. Triggers
    // persist, so deferral delays work rather than losing it — but a long streak
    // means grids are not updating and is logged once per streak.
    uint32 m_GateDeferredFrames = 0;

    // Grid entities whose bake is waiting for a physics world to exist. Holds a
    // one-log-per-streak marker only: NeedsRebake is what actually carries the
    // retry, and the whole set is cleared as soon as a physics world exists.
    std::unordered_set<uint32> m_BakeDeferredLogged;

    // Grid entities whose bake is waiting for heightfield collision to become
    // current, with the streak length for the once-per-streak log lines. The
    // retry itself rides on NeedsRebake. Entries not refreshed in an update are
    // dropped at its end.
    struct CollisionWaitStreak
    {
        uint32 Frames = 0;
        uint64 LastUpdate = 0;
    };
    std::unordered_map<ECS::EntityHandle, CollisionWaitStreak, ECS::EntityHandleHash> m_CollisionWaits;
    uint64 m_UpdateIndex = 0;

    // Shared state accessed by the main thread, async load callbacks, and
    // file-watcher callbacks. Held by shared_ptr and captured by value into
    // every callback, so it outlives the system: an async load can complete
    // after ~NavigationBuildSystem. Marking a dirty entity on a system nobody
    // will update again is harmless; touching its freed mutex is not.
    struct AsyncState
    {
        std::mutex Mutex;
        std::unordered_set<uint32> InFlight;           // entities with pending loads
        std::unordered_set<uint32> CompletedGridLoads; // grid assets ready to apply
        std::unordered_set<uint32> CompletedMeshLoads; // mesh assets ready to apply
        std::unordered_set<uint32> DirtyGridEntities;  // .navgrid edits awaiting rebake
        std::unordered_set<uint32> DirtyMeshEntities;  // .navmesh edits awaiting rebuild
    };
    std::shared_ptr<AsyncState> m_AsyncState;

    // File watcher subscriptions for navigation asset edits, and the asset root
    // they cover (empty while unsubscribed). Source-set version they were built
    // for: mounting or rebinding a source is the only thing that can move the
    // project root, and bumps it. Version 0 means no source is mounted yet, so
    // starting there costs no initial re-evaluation.
    uint64 m_WatchedSourceSetVersion = 0;
    std::filesystem::path m_WatchedAssetRoot;
    std::optional<FileWatchSubscription> m_NavGridWatchSub;
    std::optional<FileWatchSubscription> m_NavMeshWatchSub;
};

} // namespace GameEngine::PathfindingECS
