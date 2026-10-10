#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Assets/FileWatchingService.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDeltaTime = 1.0f / 60.0f;
constexpr uint32 kGridWidth = 8;
constexpr uint32 kGridDepth = 8;
constexpr float32 kCellSize = 1.0f;

// These tests run with no PhysicsWorldService, which is also the editor's
// edit-mode state: PlayModeManager disables the whole physics pipeline outside
// play mode, so PhysicsWorldService::TryGet() is null there.
class NavigationBuildGatingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    Components::NavigationGrid MakeGrid() const
    {
        Components::NavigationGrid grid{};
        grid.GridType = Pathfinding::GridType::Square;
        grid.CellSize = kCellSize;
        grid.Width = kGridWidth;
        grid.Depth = kGridDepth;
        return grid;
    }

    static Components::WorldTransform MakeWorldTransform(float32 x, float32 y, float32 z)
    {
        Components::WorldTransform wt{};
        wt.matrix[12] = x;
        wt.matrix[13] = y;
        wt.matrix[14] = z;
        return wt;
    }

    void RunBuild()
    {
        NavigationBuildSystem system;
        system.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    std::unique_ptr<ECS::World> world;
};

} // namespace

// B2. A scene-loaded grid entity carries Transform immediately but does not get
// WorldTransform until TransformHierarchySystem adds it. Initializing before
// then reads no origin and latches (0,0,0) permanently — Initialized never
// clears and GridMap exposes no origin setter. The build must wait instead.
TEST_F(NavigationBuildGatingTest, GridWithTransformButNoWorldTransformDefersInitialization)
{
    auto entity = world->Create();
    entity.Set(MakeGrid());
    entity.Set(Components::Transform{});
    world->ProcessCommands();

    RunBuild();

    const auto* grid = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    EXPECT_FALSE(grid->Initialized)
        << "grid initialized before WorldTransform existed; its origin would latch at (0,0,0)";
}

// ...and once WorldTransform appears, the grid initializes at the placed
// position rather than the world origin.
TEST_F(NavigationBuildGatingTest, GridInitializesAtPlacedOriginOnceWorldTransformExists)
{
    constexpr float32 kPlacedX = 25.0f;
    constexpr float32 kPlacedY = 3.0f;
    constexpr float32 kPlacedZ = -40.0f;

    auto entity = world->Create();
    entity.Set(MakeGrid());
    entity.Set(Components::Transform{});
    world->ProcessCommands();

    RunBuild();
    ASSERT_FALSE(entity.Get<Components::NavigationGrid>()->Initialized);

    entity.Set(MakeWorldTransform(kPlacedX, kPlacedY, kPlacedZ));
    world->ProcessCommands();

    RunBuild();

    const auto* grid = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);
    auto* gridMap = navWorld->GetGridMap(
        Pathfinding::NavMapHandle{grid->NavMapIndex, grid->NavMapGeneration});
    ASSERT_NE(gridMap, nullptr);

    const auto& settings = gridMap->GetSettings();
    EXPECT_FLOAT_EQ(settings.OriginX, kPlacedX);
    EXPECT_FLOAT_EQ(settings.OriginY, kPlacedY);
    EXPECT_FLOAT_EQ(settings.OriginZ, kPlacedZ);
}

// A grid on an entity with no Transform at all has no placed position, so the
// world origin is correct for it and it must not be deferred forever.
TEST_F(NavigationBuildGatingTest, GridWithoutAnyTransformInitializesAtWorldOrigin)
{
    auto entity = world->Create();
    entity.Set(MakeGrid());
    world->ProcessCommands();

    RunBuild();

    const auto* grid = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);
    auto* gridMap = navWorld->GetGridMap(
        Pathfinding::NavMapHandle{grid->NavMapIndex, grid->NavMapGeneration});
    ASSERT_NE(gridMap, nullptr);
    EXPECT_FLOAT_EQ(gridMap->GetSettings().OriginX, 0.0f);
    EXPECT_FLOAT_EQ(gridMap->GetSettings().OriginZ, 0.0f);
}

// B3. Heights are sampled by raycasting physics, and BakeSource::Physics also
// box-tests each cell there. With no physics world the bake must be deferred,
// not satisfied with flat ground and latched off — a latched flat grid is
// permanently wrong, because NeedsRebake is the only thing that would ever
// re-run it.
TEST_F(NavigationBuildGatingTest, BakeIsDeferredNotLatchedWhenNoPhysicsWorldExists)
{
    auto entity = world->Create();
    entity.Set(MakeGrid());
    world->ProcessCommands();

    RunBuild();

    const auto* grid = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized) << "map creation does not depend on physics";
    EXPECT_TRUE(grid->NeedsRebake)
        << "bake latched off with no physics world; the grid can never be corrected";

    // Still deferred after further frames — the retry is durable, not one-shot.
    RunBuild();
    RunBuild();
    EXPECT_TRUE(entity.Get<Components::NavigationGrid>()->NeedsRebake);
}

// The leak this branch would otherwise introduce: loading a scene re-runs the
// component schema on an entity that already owns a NavigationWorld map and
// clears its handle. Unless the schema hands the map back, every scene open
// allocates a fresh GridMap and orphans the previous one for the process
// lifetime. RemoveMap recycles the slot, so a released map is observable as
// index reuse; a leaked one takes a new index.
TEST_F(NavigationBuildGatingTest, SceneReloadReleasesPreviousNavMap)
{
    auto entity = world->Create();
    entity.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* first = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->Initialized);
    // By value: the component is rewritten below, and a pointer into its
    // storage would then read the new map's fields instead of the old one's.
    const uint32 firstIndex = first->NavMapIndex;
    const uint32 firstGeneration = first->NavMapGeneration;

    const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationGrid");
    ASSERT_NE(schema, nullptr);

    // Applying any property is the scene-load path; it clears the runtime handle.
    Scene::SceneLoadContext ctx{};
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(*world, entity.GetHandle(), ctx, "cellsize", "1.0", &error))
        << error;
    world->ProcessCommands();

    ASSERT_FALSE(entity.Get<Components::NavigationGrid>()->Initialized);

    RunBuild();

    const auto* second = entity.Get<Components::NavigationGrid>();
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(second->Initialized);
    EXPECT_EQ(second->NavMapIndex, firstIndex)
        << "previous GridMap was not released; each scene open leaks one map per grid";
    EXPECT_NE(second->NavMapGeneration, firstGeneration)
        << "slot reused without a generation bump; stale handles would resolve";
}

// An authored obstacle radius is unvalidated and drives an O(cellRadius^2)
// stamp per obstacle per grid every frame. Radius=1000 at CellSize=0.1 is 4e8
// iterations, and cellRadius*cellRadius overflows uint32 past ~65535. The
// clamp bounds that loop.
//
// A centred obstacle cannot tell a correct clamp from one that loses coverage:
// the farthest cell from the centre of an 8x8 grid is only 32 cells-squared
// away, inside any clamp at or above 6. CornerObstacleWithHugeRadiusStillCovers
// is the probe that discriminates; keep both.
TEST_F(NavigationBuildGatingTest, HugeObstacleRadiusIsClampedToGridExtent)
{
    auto gridEntity = world->Create();
    gridEntity.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* grid = gridEntity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 1000.0f;
    obstacleEntity.Set(obstacle);
    obstacleEntity.Set(MakeWorldTransform(4.0f, 0.0f, 4.0f));
    world->ProcessCommands();

    // Completing at all is the assertion: unclamped this is ~4e8 iterations per
    // frame at CellSize=1, and far worse for a finer grid.
    RunBuild();

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);
    auto* gridMap = navWorld->GetGridMap(
        Pathfinding::NavMapHandle{grid->NavMapIndex, grid->NavMapGeneration});
    ASSERT_NE(gridMap, nullptr);

    // The obstacle covers the whole grid, so every cell is dynamically blocked.
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(0, 0));
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(kGridWidth - 1, kGridDepth - 1));
}

// The clamp bounds the loop; it must not shrink the footprint. An obstacle whose
// radius spans the grid blocks every cell of it wherever it sits, so the corner
// case is the real test: from cell (0,0) the opposite corner is
// (W-1)^2 + (D-1)^2 = 98 cells-squared away, which exceeds max(W,D)^2 = 64.
// Clamping the radius to max(W,D) therefore drops the far corner, while any
// clamp at or above the grid diagonal keeps it.
TEST_F(NavigationBuildGatingTest, CornerObstacleWithHugeRadiusStillCoversFarCorner)
{
    auto gridEntity = world->Create();
    gridEntity.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* grid = gridEntity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 1000.0f;
    obstacleEntity.Set(obstacle);
    // Cell (0,0): WorldToCell floors, and the grid origin is (0,0,0).
    obstacleEntity.Set(MakeWorldTransform(0.5f, 0.0f, 0.5f));
    world->ProcessCommands();

    RunBuild();

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);
    auto* gridMap = navWorld->GetGridMap(
        Pathfinding::NavMapHandle{grid->NavMapIndex, grid->NavMapGeneration});
    ASSERT_NE(gridMap, nullptr);

    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(0, 0));
    EXPECT_TRUE(gridMap->IsCellDynamicBlocked(kGridWidth - 1, kGridDepth - 1))
        << "clamp shrank the obstacle footprint: the far corner is inside a "
           "radius that spans the grid, but outside the clamped radius";
}

// Radius <= 0 blocks nothing at all, including the obstacle's own cell. This is
// a deliberate change from taking ceil() of a non-positive radius: that path
// blocked the centre cell for Radius=0, and for a NaN radius it ran
// static_cast<uint32>(NaN), which is undefined. Rejecting the whole non-positive
// domain up front is both defined and the more defensible reading of "an
// obstacle with no radius".
TEST_F(NavigationBuildGatingTest, NonPositiveObstacleRadiusBlocksNothing)
{
    auto gridEntity = world->Create();
    gridEntity.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* grid = gridEntity.Get<Components::NavigationGrid>();
    ASSERT_NE(grid, nullptr);
    ASSERT_TRUE(grid->Initialized);

    auto obstacleEntity = world->Create();
    Components::NavigationObstacle obstacle{};
    obstacle.Radius = 0.0f;
    obstacleEntity.Set(obstacle);
    obstacleEntity.Set(MakeWorldTransform(4.0f, 0.0f, 4.0f));
    world->ProcessCommands();

    RunBuild();

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);
    auto* gridMap = navWorld->GetGridMap(
        Pathfinding::NavMapHandle{grid->NavMapIndex, grid->NavMapGeneration});
    ASSERT_NE(gridMap, nullptr);

    EXPECT_FALSE(gridMap->IsCellDynamicBlocked(4, 4))
        << "a zero-radius obstacle blocked its own cell";
}

// Navigation asset hot-reload watches the project asset root. With no asset
// manager there is no root, and a pattern rooted at an empty directory matches
// no file — so subscribing one buys a permanently dead callback and a watcher
// that logs "Directory does not exist" every time it is started. The system
// must subscribe to nothing instead.
//
// The system is held alive across the sampling: its subscriptions are RAII, so
// reading the stats after it is destroyed would pass no matter what it did.
TEST_F(NavigationBuildGatingTest, NoNavigationAssetWatchIsCreatedWithoutAnAssetRoot)
{
    namespace fs = std::filesystem;

    FileWatchingService& watchService = FileWatchingService::GetInstance();
    ASSERT_TRUE(watchService.StartWatching());
    ASSERT_TRUE(watchService.IsWatching())
        << "the subscribe path under test only runs while the service is armed";

    const auto before = watchService.GetStats();

    NavigationBuildSystem system;
    system.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto afterBuild = watchService.GetStats();
    EXPECT_EQ(afterBuild.TotalSubscriptions, before.TotalSubscriptions)
        << "NavigationBuildSystem subscribed a file watch with no asset root";
    EXPECT_EQ(afterBuild.TotalDirectories, before.TotalDirectories)
        << "an empty watch directory created a watcher that can never start";

    // Positive control: these counters do move for a real subscription, so the
    // equalities above report a skipped subscribe and not a dead instrument.
    const fs::path probeDir = fs::temp_directory_path() / "ge_nav_watch_probe";
    std::error_code ec;
    fs::create_directories(probeDir, ec);
    {
        auto probe = watchService.Subscribe(FilePattern(probeDir, ".*", {".navgrid"}, true),
                                            [](const FileChangeEvent&) {});
        const auto withProbe = watchService.GetStats();
        EXPECT_EQ(withProbe.TotalSubscriptions, before.TotalSubscriptions + 1);
    }
    fs::remove_all(probeDir, ec);

    watchService.StopWatching();
}

// The path a real scene open actually takes. The editor does not mutate the
// surviving component in place; it tears the old entities down and builds new
// ones, so the release has to hang off component teardown rather than off the
// schema's property apply. Without the OnRemove hook the map is orphaned and
// each scene open consumes another NavigationWorld slot.
TEST_F(NavigationBuildGatingTest, DestroyingGridEntityReleasesItsNavMap)
{
    auto first = world->Create();
    first.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* firstGrid = first.Get<Components::NavigationGrid>();
    ASSERT_NE(firstGrid, nullptr);
    ASSERT_TRUE(firstGrid->Initialized);
    const uint32 firstIndex = firstGrid->NavMapIndex;

    world->DestroyEntity(first.GetHandle());
    world->ProcessCommands();

    auto second = world->Create();
    second.Set(MakeGrid());
    world->ProcessCommands();
    RunBuild();

    const auto* secondGrid = second.Get<Components::NavigationGrid>();
    ASSERT_NE(secondGrid, nullptr);
    ASSERT_TRUE(secondGrid->Initialized);
    EXPECT_EQ(secondGrid->NavMapIndex, firstIndex)
        << "destroyed grid's map was not released; every scene open leaks one map per grid";
}

namespace
{

// Fixture with a real mounted asset root, which is what NavigationBuildSystem
// scopes its navigation-asset watch to. Constructing an EngineCore binds the
// EngineCore::GetInstance() singleton the system reads, so the watch resolves
// to this test's temp directory.
class NavigationAssetWatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Engine::Initialize moves the CWD to WorkspaceDirectory, and Windows
        // refuses to remove a directory containing the CWD.
        originalCwd = std::filesystem::current_path();

        testDir = std::filesystem::temp_directory_path() /
                  ("ge_nav_watch_" + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(testDir);

        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config));

        assetRoot = engine->GetAssetManager().GetAssetRoot();
        ASSERT_FALSE(assetRoot.empty());

        // The system only subscribes while the service is armed.
        ASSERT_TRUE(FileWatchingService::GetInstance().StartWatching());

        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
        FileWatchingService::GetInstance().StopWatching();
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();

        if (engine)
        {
            engine->Shutdown();
            engine.reset();
        }

        std::error_code ec;
        std::filesystem::current_path(originalCwd, ec);
        std::filesystem::remove_all(testDir, ec);
    }

    // Rewrites the file so a Modified event is produced even if the create was
    // already delivered and debounced.
    void TouchNavGrid(const std::filesystem::path& path, int revision) const
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "navgrid probe revision " << revision;
    }

    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::filesystem::path assetRoot;
    std::unique_ptr<EngineCore> engine;
    std::unique_ptr<ECS::World> world;
};

} // namespace

// The watch has to be rooted at the project asset root and filtered to
// .navgrid, which only an end-to-end event proves: a grid carrying an asset
// GUID is marked for rebake when a .navgrid under that root is edited.
//
// This is the assertion that fails if the system regresses to an empty watch
// directory: FileWatchingService refuses that pattern, so no event ever
// arrives and NeedsRebake never comes back.
TEST_F(NavigationAssetWatchTest, EditingANavGridUnderTheAssetRootMarksGridsForRebake)
{
    auto entity = world->Create();
    Components::NavigationGrid grid{};
    grid.GridType = Pathfinding::GridType::Square;
    grid.CellSize = kCellSize;
    grid.Width = kGridWidth;
    grid.Depth = kGridDepth;
    grid.AssetGuid[0] = 0x42; // HasAssetGuid: only GUID-backed grids rebake.
    entity.Set(grid);
    world->ProcessCommands();

    NavigationBuildSystem system;
    system.Update(*world, kDeltaTime);
    world->ProcessCommands();

    // Clear the flag the watch is expected to set again.
    entity.GetForWrite<Components::NavigationGrid>()->NeedsRebake = false;
    ASSERT_FALSE(entity.Get<Components::NavigationGrid>()->NeedsRebake);

    const std::filesystem::path probe = assetRoot / "WatchProbe.navgrid";
    bool rebakeRequested = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int revision = 0; std::chrono::steady_clock::now() < deadline; ++revision)
    {
        TouchNavGrid(probe, revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        system.Update(*world, kDeltaTime);
        world->ProcessCommands();

        if (entity.Get<Components::NavigationGrid>()->NeedsRebake)
        {
            rebakeRequested = true;
            break;
        }
    }

    EXPECT_TRUE(rebakeRequested)
        << "editing a .navgrid under the asset root did not reach the grid; the "
           "navigation watch is not subscribed on the project asset root";
}

// Shutdown-order hazard: the editor destroys the SystemManager (and every
// system in it) before it stops file watching, so a navigation-asset event can
// land while the system is being destroyed.
//
// A higher-priority co-subscriber on the same file reproduces that window
// exactly: it runs first, inside the same dispatch loop, and destroys the
// system while the system's own callback is already matched and pending. The
// system's unsubscribe must keep that pending call from running.
TEST_F(NavigationAssetWatchTest, EventDispatchedWhileTheSystemIsDestroyedDoesNotTouchIt)
{
    auto system = std::make_unique<NavigationBuildSystem>();
    system->Update(*world, kDeltaTime);
    world->ProcessCommands();

    std::atomic<bool> systemDestroyed{false};

    // Priority 10 > the system's default 0, so this runs first in the dispatch
    // loop. Both subscriptions stay on the asset root's watcher, so tearing the
    // system down here cannot stop (and self-join) the watcher thread we are on.
    // Matching the system's own Modified-only filter is what makes this bite:
    // on any other event type the system's callback returns before touching
    // its captured state, so destroying during one would prove nothing.
    constexpr int kAheadOfTheSystem = 10;
    auto destroyer = FileWatchingService::GetInstance().Subscribe(
        FilePattern(assetRoot, ".*", {".navgrid"}, true),
        [&system, &systemDestroyed](const FileChangeEvent& event) {
            if (event.Type != FileChangeType::Modified)
                return;
            if (!systemDestroyed.exchange(true))
                system.reset();
        },
        kAheadOfTheSystem);

    const std::filesystem::path probe = assetRoot / "TeardownProbe.navgrid";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int revision = 0; !systemDestroyed.load() &&
                           std::chrono::steady_clock::now() < deadline; ++revision)
    {
        TouchNavGrid(probe, revision);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ASSERT_TRUE(systemDestroyed.load())
        << "no .navgrid event was delivered, so the teardown window was never entered";

    // Let the dispatch that destroyed the system finish its loop, past the
    // system's own pending callback. Reaching here without a crash or a
    // sanitizer report is the assertion.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(system, nullptr);
}
