#include <gtest/gtest.h>

#include "Components/Transform.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "Pathfinding/GridMap.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Components/TerrainTileCollider.h"
#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::PathfindingECS;
namespace TE = GameEngine::TerrainECS;
namespace PE = GameEngine::PhysicsECS;
constexpr float32 kDt = 1.0f / 60.0f;
constexpr float32 kSize = 32.0f;
constexpr float32 kScale = 16.0f;

class CapturingLogSink final : public Logger::LogSink
{
public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}
    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Out->push_back(std::string(message.Message));
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "NavigationReadinessLogSink"; }

private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};

// Routes log lines into *out for the scope and restores the default sinks on
// every exit path, since the sink holds a raw pointer to the test's vector.
class ScopedLogCapture
{
public:
    explicit ScopedLogCapture(std::vector<std::string>* out)
    {
        Logger::Log::Initialize({});
        Logger::Log::ClearSinks();
        Logger::Log::AddSink(std::make_unique<CapturingLogSink>(out));
    }
    ~ScopedLogCapture()
    {
        Logger::Log::ClearSinks();
        Logger::Log::Initialize({});
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
};

size_t CountLinesContaining(const std::vector<std::string>& lines, const std::string& needle)
{
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(),
        [&](const std::string& line) { return line.find(needle) != std::string::npos; }));
}

class NavigationTerrainReadinessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Physics::PhysicsWorldSettings settings{};
        settings.numThreads = 1;
        settings.maxBodies = 128;
        settings.maxBodyPairs = 128;
        settings.maxContactConstraints = 128;
        settings.tempAllocatorBytes = 4u * 1024u * 1024u;
        PE::PhysicsWorldService::Initialize(settings);
        TE::TerrainService::Initialize(); // Installs the actual terrain heightfield provider.
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        PE::RegisterPhysicsWorldHooks(*world);
        TE::RegisterTerrainWorldHooks(*world);
        RegisterNavigationWorldHooks(*world);
        TE::EnableTerrainModifierLifecycleEvents(*world);
    }

    void TearDown() override
    {
        world.reset(); // Release actual terrain, map and body owners before their services.
        NavigationService::Shutdown();
        TE::TerrainService::Shutdown();
        PE::PhysicsWorldService::Shutdown();
    }

    static Components::WorldTransform Placed(float32 x, float32 z)
    {
        Components::WorldTransform transform{};
        transform.matrix[12] = x;
        transform.matrix[14] = z;
        return transform;
    }

    ECS::EntityHandle MakeGrid(Components::NavigationBakeSource mode = Components::NavigationBakeSource::Physics)
    {
        auto entity = world->CreateEntity();
        Components::NavigationGrid source{};
        source.Width = 8;
        source.Depth = 8;
        source.CellSize = 1.0f;
        source.BakeSource = mode;
        source.RaycastOriginHeight = 32.0f;
        source.BakeAgentRadius = 0.2f;
        Components::Transform transform{};
        transform.matrix[12] = -4.0f;
        transform.matrix[14] = -4.0f;
        world->AddComponentImmediate(entity, transform);
        world->AddComponentImmediate(entity, Placed(-4.0f, -4.0f));
        world->AddComponentImmediate(entity, source);
        return entity;
    }

    ECS::EntityHandle MakeTerrain(bool dataReady, float32 height = 0.0f, float32 x = 0.0f,
                                  bool enabled = true)
    {
        auto entity = world->CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = terrain.SizeZ = kSize;
        terrain.HeightScale = kScale;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        if (dataReady)
        {
            const auto handle = CreateTerrainData(height);
            EXPECT_NE(TE::TerrainService::Get().GetTerrainData(handle), nullptr);
            terrain.TerrainDataHandle = handle.Index;
            terrain.TerrainDataGeneration = handle.Generation;
        }
        Components::Transform transform{};
        transform.matrix[12] = x;
        world->AddComponentImmediate(entity, transform);
        world->AddComponentImmediate(entity, Placed(x, 0.0f));
        world->AddComponentImmediate(entity, terrain);
        ECS::Entity(world.get(), entity).SetEnabled<Components::Terrain>(enabled);
        return entity;
    }

    void Flatten(float32 height)
    {
        auto modifier = world->CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Circle;
        volume.Radius = 64.0f;
        volume.Falloff = 0.0f;
        Components::TerrainFlattenEffect effect{};
        effect.UseVolumeHeight = false;
        effect.TargetHeight = height;
        world->AddComponentImmediate(modifier, volume);
        world->AddComponentImmediate(modifier, effect);
        world->AddComponentImmediate(modifier, Components::WorldTransform{});
        world->SwapLifecycleEvents();
        modifiers.Update(*world, kDt);
    }

    void InitializePhysics()
    {
        terrainPhysics.Update(*world, kDt);
        world->ProcessCommands();
        physicsInit.Update(*world, kDt);
        PE::PhysicsWorldService::Get().OptimizeBroadphase();
    }

    // Runs terrain provisioning and body builds across engine frame boundaries
    // until collision has caught up with the terrain, rebuilds included.
    void SettlePhysics()
    {
        physicsInit.SetRebuildQuiescenceSeconds(0.0f);
        for (int frame = 0; frame < 4; ++frame)
        {
            terrainPhysics.Update(*world, kDt);
            world->ProcessCommands();
            physicsInit.Update(*world, kDt);
            world->ProcessCommands();
        }
        PE::PhysicsWorldService::Get().OptimizeBroadphase();
    }

    TE::TerrainHandle CreateTerrainData(float32 height)
    {
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = config.HeightmapHeight = 33;
        config.WorldSizeX = config.WorldSizeZ = kSize;
        config.HeightScale = kScale;
        config.LODLevels = 2;
        auto handle = TE::TerrainService::Get().CreateTerrain(config);
        if (auto* data = TE::TerrainService::Get().GetTerrainData(handle))
        {
            for (uint32 z = 0; z < 33; ++z)
                for (uint32 sx = 0; sx < 33; ++sx)
                    data->Heightfield.SetSample(sx, z, height / kScale);
            data->MarkFullDirty();
            data->ResetSplatmapAndCommitRange();
        }
        return handle;
    }

    float32 GroundAt(float32 x, float32 z)
    {
        Physics::RayCastQuery query;
        query.ray.origin = Physics::Vector3(x, 64.0f, z);
        query.ray.direction = Physics::Vector3(0.0f, -1.0f, 0.0f);
        query.maxDistance = 128.0f;
        Physics::RayCastResult hit;
        return PE::PhysicsWorldService::Get().RayCast(query, hit) ? hit.hitPoint.y
                                                                  : std::numeric_limits<float32>::quiet_NaN();
    }

    Components::NavigationGrid* Grid(ECS::EntityHandle entity)
    {
        return world->GetComponentForWrite<Components::NavigationGrid>(entity);
    }

    void ExpectPending(ECS::EntityHandle grid)
    {
        ASSERT_TRUE(Grid(grid)->Initialized);
        EXPECT_TRUE(Grid(grid)->NeedsRebake);
    }

    void ExpectHeight(ECS::EntityHandle grid, float32 height)
    {
        ASSERT_TRUE(Grid(grid)->Initialized);
        EXPECT_FALSE(Grid(grid)->NeedsRebake);
        auto* map = ResolveNavigationGridMap(*world, *Grid(grid));
        ASSERT_NE(map, nullptr);
        for (uint32 z = 0; z < 8; ++z)
            for (uint32 x = 0; x < 8; ++x)
                EXPECT_NEAR(map->GetCellHeight(x, z), height, 0.02f);
    }

    std::unique_ptr<ECS::World> world;
    NavigationBuildSystem navigation;
    TE::TerrainPhysicsSystem terrainPhysics;
    TE::TerrainModifierSystem modifiers;
    PE::PhysicsInitSystem physicsInit;
};

TEST_F(NavigationTerrainReadinessTest, PendingTerrainHandleIsNotAnEmptyWorld)
{
    MakeTerrain(false);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    navigation.Update(*world, kDt);
    ExpectPending(grid);
}

TEST_F(NavigationTerrainReadinessTest, MissingTerrainWorldTransformDefers)
{
    auto terrain = MakeTerrain(true, 4.0f);
    world->RemoveComponentImmediate<Components::WorldTransform>(terrain);
    auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);

    // A far-away terrain whose placement is not finite cannot be shown disjoint either.
    world->RemoveComponentImmediate<Components::Terrain>(terrain);
    const auto farTerrain = MakeTerrain(false, 0.0f, 1000.0f);
    world->GetComponentForWrite<Components::WorldTransform>(farTerrain)->matrix[12] =
        std::numeric_limits<float32>::quiet_NaN();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
}

TEST_F(NavigationTerrainReadinessTest, QueuedColdColliderWaitsForComposedSurfaceNextFrame)
{
    const auto terrain = MakeTerrain(true); // CPU base creation; no GPU extraction claim.
    const auto grid = MakeGrid();
    terrainPhysics.Update(*world, kDt);
    ASSERT_FALSE(world->HasComponent<Components::HeightFieldColliderShape>(terrain));
    physicsInit.Update(*world, kDt); // Same-frame schedule sees no deferred collider.
    navigation.Update(*world, kDt);
    ExpectPending(grid);

    world->ProcessCommands(); // Actual engine boundary: next frame, before the systems.
    Flatten(4.0f); // Actual modifier owner composes the authored surface.
    terrainPhysics.Update(*world, kDt);
    physicsInit.Update(*world, kDt);
    PE::PhysicsWorldService::Get().OptimizeBroadphase();
    const auto* shape = world->GetComponent<Components::HeightFieldColliderShape>(terrain);
    ASSERT_NE(shape, nullptr);
    const auto data = PE::GetHeightFieldDataProvider()(shape->dataHandle, shape->dataGeneration, 0);
    ASSERT_EQ(shape->lastBuiltVersion, data.version);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, PublishedComponentsAloneAreNotBackendReadiness)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    terrainPhysics.Update(*world, kDt);
    world->ProcessCommands();
    ASSERT_TRUE(world->HasComponent<Components::HeightFieldColliderShape>(terrain));
    ASSERT_FALSE(world->GetComponent<Components::PhysicsBody>(terrain)->initialized);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    physicsInit.Update(*world, kDt);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, RequestedRebakeWaitsForActualThrottledBodyRebuild)
{
    const auto terrain = MakeTerrain(true, 1.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 1.0f);
    const auto* map = ResolveNavigationGridMap(*world, *Grid(grid));
    const auto topology = map->GetTopologyVersion();
    const auto oldBody = world->GetComponent<Components::PhysicsBody>(terrain)->body;
    physicsInit.SetRebuildQuiescenceSeconds(0.08f);
    Flatten(4.0f); // Full dirty region takes the existing tier-two rebuild path.
    Grid(grid)->NeedsRebake = true;
    physicsInit.Update(*world, kDt);
    ASSERT_EQ(world->GetComponent<Components::PhysicsBody>(terrain)->body, oldBody);
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    EXPECT_FLOAT_EQ(map->GetCellHeight(0, 0), 1.0f);
    EXPECT_EQ(map->GetTopologyVersion(), topology);
    for (int frame = 0; frame < 3; ++frame)
    {
        physicsInit.Update(*world, kDt);
        navigation.Update(*world, kDt);
        ExpectPending(grid);
        EXPECT_FLOAT_EQ(map->GetCellHeight(0, 0), 1.0f);
        EXPECT_EQ(map->GetTopologyVersion(), topology);
    }
    for (int frame = 0; frame < 10; ++frame)
        physicsInit.Update(*world, kDt);
    ASSERT_NE(world->GetComponent<Components::PhysicsBody>(terrain)->body, oldBody);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, NoTerrainRetainsExistingFlatWorldBehavior)
{
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
}

TEST_F(NavigationTerrainReadinessTest, DisjointPendingTerrainDoesNotDelayGrid)
{
    MakeTerrain(false, 0.0f, 1000.0f);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
}

TEST_F(NavigationTerrainReadinessTest, DisabledPendingTerrainDoesNotDelayGrid)
{
    MakeTerrain(false, 0.0f, 0.0f, false);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
}

TEST_F(NavigationTerrainReadinessTest, ExplicitlyDisabledColliderDoesNotWaitForABody)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    terrainPhysics.Update(*world, kDt);
    world->ProcessCommands();
    ECS::Entity(world.get(), terrain).SetEnabled<Components::HeightFieldColliderShape>(false);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
}

TEST_F(NavigationTerrainReadinessTest, MatchingVersionWithDestroyedBackendBodyStaysPending)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    InitializePhysics();
    auto* body = world->GetComponentForWrite<Components::PhysicsBody>(terrain);
    PE::PhysicsWorldService::Get().DestroyBody(body->body); // Keep stale component value as the negative.
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    physicsInit.Update(*world, kDt);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, RecreatedTerrainDataRebuildsTheColliderBeforeTheBake)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);

    // A structural terrain rebuild destroys the data and hands the terrain a new handle.
    auto* component = world->GetComponentForWrite<Components::Terrain>(terrain);
    TE::TerrainService::Get().DestroyTerrain({component->TerrainDataHandle, component->TerrainDataGeneration});
    const auto replacement = CreateTerrainData(6.0f);
    component = world->GetComponentForWrite<Components::Terrain>(terrain);
    component->TerrainDataHandle = replacement.Index;
    component->TerrainDataGeneration = replacement.Generation;
    Grid(grid)->NeedsRebake = true;
    navigation.Update(*world, kDt);
    ExpectPending(grid);

    SettlePhysics();
    const auto* shape = world->GetComponent<Components::HeightFieldColliderShape>(terrain);
    EXPECT_EQ(shape->dataHandle, replacement.Index);
    EXPECT_EQ(shape->dataGeneration, replacement.Generation);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 6.0f);
}

TEST_F(NavigationTerrainReadinessTest, MissingProviderDoesNotHoldBackABuiltCollider)
{
    MakeTerrain(true, 4.0f);
    InitializePhysics();
    const auto provider = PE::GetHeightFieldDataProvider();
    PE::SetHeightFieldDataProvider(nullptr); // Nothing could rebuild it, so it is as current as it gets.
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    PE::SetHeightFieldDataProvider(provider);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, HeightScaleEditRebuildsTheColliderBeforeTheBake)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);

    world->GetComponentForWrite<Components::Terrain>(terrain)->HeightScale = 2.0f * kScale;
    Grid(grid)->NeedsRebake = true;
    navigation.Update(*world, kDt);
    ExpectPending(grid);

    SettlePhysics();
    EXPECT_FLOAT_EQ(world->GetComponent<Components::HeightFieldColliderShape>(terrain)->heightScale, 2.0f * kScale);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 8.0f);
}

TEST_F(NavigationTerrainReadinessTest, SizeEditRebuildsTheColliderToTheNewExtent)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
    const float32 beyondOldEdge = 0.75f * kSize; // Outside the old half-extent, inside the new one.
    EXPECT_TRUE(std::isnan(GroundAt(beyondOldEdge, 0.0f)));

    auto* component = world->GetComponentForWrite<Components::Terrain>(terrain);
    component->SizeX = component->SizeZ = 2.0f * kSize;
    Grid(grid)->NeedsRebake = true;
    navigation.Update(*world, kDt);
    ExpectPending(grid);

    SettlePhysics();
    EXPECT_NEAR(GroundAt(beyondOldEdge, 0.0f), 4.0f, 0.02f);
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, TerrainDataThatNeverArrivesWaitsAndSaysWhy)
{
    const auto terrain = MakeTerrain(false);
    const auto grid = MakeGrid();
    std::vector<std::string> lines;
    {
        ScopedLogCapture capture(&lines);
        for (int frame = 0; frame < 130; ++frame)
        {
            terrainPhysics.Update(*world, kDt);
            world->ProcessCommands();
            physicsInit.Update(*world, kDt);
            navigation.Update(*world, kDt);
        }
    }
    ExpectPending(grid);
    const std::string blocker = "entity " + std::to_string(terrain.id) + ": terrain height data is not loaded";
    EXPECT_EQ(CountLinesContaining(lines, "waits for heightfield collision: " + blocker), 1u);
    EXPECT_EQ(CountLinesContaining(lines, "has waited 120 frames for heightfield collision: " + blocker), 1u);
}

TEST_F(NavigationTerrainReadinessTest, TerrainWithoutTheTerrainModuleStillBakes)
{
    // TerrainECS is optional: without it nothing reports terrain as pending, so
    // the grid bakes against the collision that exists.
    TE::TerrainService::Shutdown();
    MakeTerrain(false);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
}

TEST_F(NavigationTerrainReadinessTest, TwoReadyTerrainsUnderOneGridBake)
{
    MakeTerrain(true, 4.0f, -0.5f * kSize);
    MakeTerrain(true, 4.0f, 0.5f * kSize);
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

TEST_F(NavigationTerrainReadinessTest, TiledTerrainWaitsForItsResidentTileColliders)
{
    TE::TiledTerrainConfig config{};
    config.WorldSizeX = config.WorldSizeZ = 64.0f;
    config.HeightScale = kScale;
    config.SamplesPerMeter = 1.0f;
    auto& service = TE::TerrainService::Get();
    const auto handle = service.CreateTiledTerrain(config);
    ASSERT_NE(service.GetTiledTerrainData(handle), nullptr);
    auto* tile = service.LoadTile(handle, TE::TileCoord{0, 0});
    ASSERT_NE(tile, nullptr);
    tile->LodState = TE::TileLodState::Full;
    tile->MarkFullDirty();

    auto entity = world->CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = terrain.SizeZ = 64.0f;
    terrain.HeightScale = kScale;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    world->AddComponentImmediate(entity, Components::Transform{});
    world->AddComponentImmediate(entity, Components::WorldTransform{});
    world->AddComponentImmediate(entity, terrain);

    // A grid over the resident tile.
    const float32 x = tile->WorldOriginX + 4.0f;
    const float32 z = tile->WorldOriginZ + 4.0f;
    const auto grid = MakeGrid();
    world->GetComponentForWrite<Components::Transform>(grid)->matrix[12] = x;
    world->GetComponentForWrite<Components::Transform>(grid)->matrix[14] = z;
    *world->GetComponentForWrite<Components::WorldTransform>(grid) = Placed(x, z);

    navigation.Update(*world, kDt);
    ExpectPending(grid);
    terrainPhysics.Update(*world, kDt); // Queues the tile collider for the next frame.
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    SettlePhysics();
    size_t tileColliders = 0;
    world->Query<ECS::Read<Components::TerrainTileCollider>>().Each(
        [&](const Components::TerrainTileCollider&) { ++tileColliders; });
    ASSERT_GE(tileColliders, 1u);
    navigation.Update(*world, kDt);
    EXPECT_FALSE(Grid(grid)->NeedsRebake);
}

TEST_F(NavigationTerrainReadinessTest, WorldClearRetriesNewPendingTerrainWithSameSystem)
{
    MakeTerrain(true, 4.0f);
    auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
    world->Clear();
    MakeTerrain(false);
    grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectPending(grid);
}

TEST_F(NavigationTerrainReadinessTest, AlreadyCompletedManualMapIsNotAdopted)
{
    MakeTerrain(false);
    const auto grid = MakeGrid(Components::NavigationBakeSource::Manual);
    Pathfinding::GridSettings settings{};
    settings.Width = settings.Depth = 8;
    ASSERT_TRUE(CreateNavigationGridMap(*world, *Grid(grid), settings).IsValid());
    auto* map = ResolveNavigationGridMap(*world, *Grid(grid));
    ASSERT_NE(map, nullptr);
    map->SetCellHeight(0, 0, 7.0f);
    Grid(grid)->NeedsRebake = false;
    const auto topology = map->GetTopologyVersion();
    navigation.Update(*world, kDt);
    EXPECT_FALSE(Grid(grid)->NeedsRebake);
    EXPECT_FLOAT_EQ(map->GetCellHeight(0, 0), 7.0f);
    EXPECT_EQ(map->GetTopologyVersion(), topology);
}

TEST_F(NavigationTerrainReadinessTest, RemovedTerrainTakesItsColliderAlong)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);

    // Removing the Terrain releases its data; nothing will ever rebuild the collider.
    world->RemoveComponentImmediate<Components::Terrain>(terrain);
    Grid(grid)->NeedsRebake = true;
    SettlePhysics();
    EXPECT_FALSE(world->HasComponent<Components::HeightFieldColliderShape>(terrain));
    EXPECT_FALSE(world->HasComponent<Components::PhysicsBody>(terrain));
    EXPECT_TRUE(std::isnan(GroundAt(0.0f, 0.0f))); // No stale collision left behind.
    navigation.Update(*world, kDt);
    EXPECT_FALSE(Grid(grid)->NeedsRebake); // The bake completes instead of waiting on the orphan.
}

TEST_F(NavigationTerrainReadinessTest, TerrainThatBecomesTiledDropsItsPlanarCollider)
{
    const auto terrain = MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid();
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);

    // What extraction does when a terrain grows past the tiling threshold: the
    // planar data is destroyed and the terrain moves to a tiled handle.
    TE::TiledTerrainConfig config{};
    config.WorldSizeX = config.WorldSizeZ = 64.0f;
    config.HeightScale = kScale;
    config.SamplesPerMeter = 1.0f;
    const auto tiled = TE::TerrainService::Get().CreateTiledTerrain(config);
    auto* component = world->GetComponentForWrite<Components::Terrain>(terrain);
    TE::TerrainService::Get().DestroyTerrain({component->TerrainDataHandle, component->TerrainDataGeneration});
    component->TerrainDataHandle = 0;
    component->TerrainDataGeneration = 0;
    component->TiledTerrainHandle = tiled.Index;
    component->TiledTerrainGeneration = tiled.Generation;
    Grid(grid)->NeedsRebake = true;
    SettlePhysics();
    EXPECT_FALSE(world->HasComponent<Components::HeightFieldColliderShape>(terrain));
    EXPECT_TRUE(std::isnan(GroundAt(0.0f, 0.0f)));
    navigation.Update(*world, kDt);
    EXPECT_FALSE(Grid(grid)->NeedsRebake); // No tile is resident, so there is no terrain collision to wait for.
}

// The obstacle pass tests an agent-sized box per cell. A trigger volume beside
// a cell centre (so the height ray misses it) must leave the cell walkable; a
// solid box in the same place blocks its cell.
TEST_F(NavigationTerrainReadinessTest, TriggerVolumesDoNotBlockCells)
{
    const auto grid = MakeGrid();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 0.0f);
    auto* map = ResolveNavigationGridMap(*world, *Grid(grid));
    ASSERT_NE(map, nullptr);

    const auto placeBox = [&](uint32 cellX, uint32 cellZ, bool trigger)
    {
        float32 x = 0.0f, z = 0.0f;
        map->CellToWorld(cellX, cellZ, x, z);
        x += 0.15f;
        auto entity = world->CreateEntity();
        world->AddComponentImmediate(entity, Placed(x, z));
        Components::Transform transform{};
        transform.matrix[12] = x;
        transform.matrix[14] = z;
        world->AddComponentImmediate(entity, transform);
        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        world->AddComponentImmediate(entity, body);
        Components::PhysicsCollider collider{};
        collider.layer = Physics::Layers::Static;
        collider.isTrigger = trigger;
        world->AddComponentImmediate(entity, collider);
        Components::BoxColliderShape shape{};
        shape.halfExtentsX = shape.halfExtentsZ = 0.1f;
        shape.halfExtentsY = 0.5f;
        world->AddComponentImmediate(entity, shape);
    };
    placeBox(2, 2, true);
    placeBox(5, 5, false);
    physicsInit.Update(*world, kDt);
    PE::PhysicsWorldService::Get().OptimizeBroadphase();

    Grid(grid)->NeedsRebake = true;
    navigation.Update(*world, kDt);
    EXPECT_FALSE(Grid(grid)->NeedsRebake);
    EXPECT_FALSE(map->IsCellBlocked(2, 2));
    EXPECT_TRUE(map->IsCellBlocked(5, 5));
}

class NavigationTerrainBakeModeTest : public NavigationTerrainReadinessTest,
                                      public ::testing::WithParamInterface<Components::NavigationBakeSource> {};

TEST_P(NavigationTerrainBakeModeTest, EveryHeightSamplingModeWaitsForTerrain)
{
    MakeTerrain(true, 4.0f);
    const auto grid = MakeGrid(GetParam());
    navigation.Update(*world, kDt);
    ExpectPending(grid);
    InitializePhysics();
    navigation.Update(*world, kDt);
    ExpectHeight(grid, 4.0f);
}

INSTANTIATE_TEST_SUITE_P(HeightSources, NavigationTerrainBakeModeTest, ::testing::Values(
    Components::NavigationBakeSource::Physics, Components::NavigationBakeSource::RenderGeometry,
    Components::NavigationBakeSource::BoundingBoxes, Components::NavigationBakeSource::Manual));
} // namespace
