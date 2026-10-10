#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{
using Grid = Components::NavigationGrid;
using Map = Pathfinding::NavMapHandle;
Map Handle(const Grid& grid) { return {grid.NavMapIndex, grid.NavMapGeneration}; }
void ExpectBindingEqual(const Grid& actual, const Grid& expected)
{
    EXPECT_EQ(Handle(actual), Handle(expected));
    EXPECT_EQ(actual.OwnerWorldId, expected.OwnerWorldId);
    EXPECT_EQ(actual.Initialized, expected.Initialized);
    EXPECT_EQ(actual.NeedsRebake, expected.NeedsRebake);
}

class NavigationGridOwnershipTest : public ::testing::Test
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
        NavigationService::Shutdown();
    }
    ECS::EntityHandle Make(ECS::World& owner)
    {
        Grid source{};
        source.Width = source.Depth = 8;
        source.BakeSource = Components::NavigationBakeSource::Manual;
        // A complete manually supplied grid; NavigationBuildGating covers
        // pending physics bakes.
        source.NeedsRebake = false;
        const auto entity = owner.Create(source).GetHandle();
        build.Update(owner, 0);
        auto* runtime = owner.GetComponentForWrite<Grid>(entity);
        if (auto* map = NavigationService::Get().GetGridMap(Handle(*runtime)))
        {
            // Fill the new map through its owner, then publish readiness:
            // creation alone is never a bake.
            for (uint32 z = 0; z < 8; ++z)
                for (uint32 x = 0; x < 8; ++x)
                {
                    map->SetCellHeight(x, z, 0);
                    map->SetCellBlocked(x, z, false);
                }
            runtime->NeedsRebake = false;
        }
        return entity;
    }
    // Rebuilds the service and adds one map into the slot the original held.
    // The slot repeats; the generation cannot, so the old handle names nothing.
    Map RecreateService(Map original)
    {
        auto* previous = NavigationService::Get().GetGridMap(original);
        if (!previous) return {};
        const auto settings = previous->GetSettings();
        NavigationService::Shutdown();
        NavigationService::Initialize();
        const auto unrelated = NavigationService::Get().AddGridMap(settings);
        EXPECT_EQ(original.Index, unrelated.Index);
        EXPECT_NE(original.Generation, unrelated.Generation);
        NavigationService::Get().GetGridMap(unrelated)->SetCellDynamicBlocked(2, 3, true);
        return unrelated;
    }
    void ExpectSentinel(Map map)
    {
        auto* sentinel = NavigationService::Get().GetGridMap(map);
        ASSERT_NE(sentinel, nullptr) << "foreign map was retired through a stale binding";
        EXPECT_TRUE(sentinel->IsCellBlocked(2, 3)) << "foreign map was mutated through a stale binding";
    }
    std::unique_ptr<ECS::World> world;
    NavigationBuildSystem build;
};

TEST_F(NavigationGridOwnershipTest, RecreatedServiceIsRejectedBeforeBuildFastPathAndDynamicWrites)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Grid>(entity)));
    ASSERT_TRUE(unrelated.IsValid());
    build.Update(*world, 0);
    const auto* rebuilt = world->GetComponent<Grid>(entity);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(Handle(*rebuilt), unrelated);
    EXPECT_TRUE(rebuilt->NeedsRebake);
    ExpectSentinel(unrelated);
}

// Play-mode exit and WorldSnapshotCommand undo restore the world from raw
// component bytes: Clear releases every map, then the restored grid still says
// Initialized with the released handle. The next build must rebind it.
TEST_F(NavigationGridOwnershipTest, WorldSnapshotRestoreRebuildsTheGrid)
{
    const auto entity = Make(*world);
    const auto before = Handle(*world->GetComponent<Grid>(entity));
    const auto snapshot = world->SerializeWorld();
    world->DeserializeWorld(snapshot);
    const auto* restored = world->GetComponent<Grid>(entity);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->Initialized);
    EXPECT_EQ(Handle(*restored), before);
    EXPECT_EQ(NavigationService::Get().GetGridMap(before), nullptr);
    EXPECT_EQ(ResolveNavigationGridMap(*world, *restored), nullptr);
    build.Update(*world, 0);
    restored = world->GetComponent<Grid>(entity);
    EXPECT_TRUE(restored->Initialized);
    EXPECT_TRUE(restored->NeedsRebake);
    EXPECT_NE(Handle(*restored), before);
    EXPECT_NE(ResolveNavigationGridMap(*world, *restored), nullptr);
}

TEST_F(NavigationGridOwnershipTest, WorldSnapshotRestoreRebuildsTheNavMesh)
{
    using Mesh = Components::NavigationMesh;
    const auto entity = world->Create(Mesh{}).GetHandle();
    build.Update(*world, 0);
    const auto* mesh = world->GetComponent<Mesh>(entity);
    ASSERT_NE(mesh, nullptr);
    ASSERT_TRUE(mesh->Initialized);
    const Map before{mesh->NavMapIndex, mesh->NavMapGeneration};
    ASSERT_NE(NavigationService::Get().GetDetourNavMap(before), nullptr);
    const auto snapshot = world->SerializeWorld();
    world->DeserializeWorld(snapshot);
    mesh = world->GetComponent<Mesh>(entity);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(mesh->Initialized);
    EXPECT_EQ(NavigationService::Get().GetDetourNavMap(before), nullptr);
    build.Update(*world, 0);
    mesh = world->GetComponent<Mesh>(entity);
    EXPECT_TRUE(mesh->Initialized);
    const Map after{mesh->NavMapIndex, mesh->NavMapGeneration};
    EXPECT_NE(after, before);
    EXPECT_NE(NavigationService::Get().GetDetourNavMap(after), nullptr);
}

enum class Removal { Destroy, Clear, SchemaRemove, SchemaProperty };
class NavigationGridRemovalTest : public NavigationGridOwnershipTest,
                                  public ::testing::WithParamInterface<Removal>
{
protected:
    void Remove(ECS::EntityHandle entity)
    {
        const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationGrid");
        ASSERT_NE(schema, nullptr);
        switch (GetParam())
        {
        case Removal::Destroy: world->DestroyEntityImmediate(entity); break;
        case Removal::Clear: world->Clear(); break;
        case Removal::SchemaRemove: ASSERT_TRUE(schema->Remove(*world, entity)); break;
        case Removal::SchemaProperty:
        {
            std::string error;
            ASSERT_TRUE(schema->ApplyProperty(*world, entity, {}, "width", "9", &error)) << error;
            break;
        }
        }
    }
};

TEST_P(NavigationGridRemovalTest, StaleServiceReleasePreservesUnrelatedMap)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Grid>(entity)));
    ASSERT_TRUE(unrelated.IsValid());
    Remove(entity);
    ExpectSentinel(unrelated);
}

TEST_P(NavigationGridRemovalTest, CurrentOwnerReleasesAndSlotIsReusable)
{
    const auto entity = Make(*world);
    const auto owned = Handle(*world->GetComponent<Grid>(entity));
    const auto settings = NavigationService::Get().GetGridMap(owned)->GetSettings();
    Remove(entity);
    EXPECT_EQ(NavigationService::Get().GetGridMap(owned), nullptr);
    const auto replacement = NavigationService::Get().AddGridMap(settings);
    EXPECT_EQ(replacement.Index, owned.Index);
    EXPECT_NE(replacement.Generation, owned.Generation);
    // Schema removal plus the registered hook must not retire the slot twice.
    const auto second = NavigationService::Get().AddGridMap(settings);
    EXPECT_NE(second.Index, replacement.Index);
}
INSTANTIATE_TEST_SUITE_P(AllRemovalOwners, NavigationGridRemovalTest,
    ::testing::Values(Removal::Destroy, Removal::Clear, Removal::SchemaRemove, Removal::SchemaProperty));

TEST_F(NavigationGridOwnershipTest, ReusedBuilderRejectsCopiedBindingFromAnotherWorld)
{
    const auto first = Make(*world);
    const Grid copied = *world->GetComponent<Grid>(first);
    const auto original = Handle(copied);
    NavigationService::Get().GetGridMap(original)->SetCellDynamicBlocked(2, 3, true);
    ECS::World other;
    const auto second = other.Create(copied).GetHandle();
    build.Update(other, 0);
    EXPECT_NE(Handle(*other.GetComponent<Grid>(second)), original);
    ExpectSentinel(original);
    // `other` registers no hooks, so its map is handed back by hand.
    NavigationService::Get().RemoveMap(Handle(*other.GetComponent<Grid>(second)));
}

TEST_F(NavigationGridOwnershipTest, ForgottenCopyInTheSameWorldRebindsWithoutReleasingTheOriginal)
{
    const auto first = Make(*world);
    const auto original = Handle(*world->GetComponent<Grid>(first));
    // Static blocking: the build pass legitimately rewrites the original's
    // dynamic-blocked cells while it stays resolvable.
    NavigationService::Get().GetGridMap(original)->SetCellBlocked(2, 3, true);
    const auto copy = world->CloneEntity(first);
    auto* copied = world->GetComponentForWrite<Grid>(copy);
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(ResolveNavigationGridMap(*world, *copied), NavigationService::Get().GetGridMap(original));
    ForgetNavigationGridMap(*copied);
    EXPECT_FALSE(copied->Initialized);
    ExpectSentinel(original);
    build.Update(*world, 0);
    const auto rebound = Handle(*world->GetComponent<Grid>(copy));
    EXPECT_NE(rebound, original);
    EXPECT_NE(NavigationService::Get().GetGridMap(rebound), nullptr);
    world->DestroyEntityImmediate(copy);
    ExpectSentinel(original);
    EXPECT_EQ(NavigationService::Get().GetGridMap(rebound), nullptr);
}

TEST_F(NavigationGridOwnershipTest, ClearRejectsSavedRuntimeCopyBeforeBuilderReuse)
{
    const auto first = Make(*world);
    const Grid copied = *world->GetComponent<Grid>(first);
    const auto old = Handle(copied);
    world->Clear();
    const auto second = world->Create(copied).GetHandle();
    build.Update(*world, 0);
    EXPECT_NE(Handle(*world->GetComponent<Grid>(second)), old);
    EXPECT_NE(NavigationService::Get().GetGridMap(Handle(*world->GetComponent<Grid>(second))), nullptr);
}

TEST_F(NavigationGridOwnershipTest, ActualDiskLoadOmitsRuntimeBindingAndRebuilds)
{
    const auto entity = Make(*world);
    const auto old = Handle(*world->GetComponent<Grid>(entity));
    const auto path = std::filesystem::temp_directory_path() /
        ("navigation-grid-owner-" + std::to_string(world->GetWorldId()) + ".scene");
    struct FileCleanup { std::filesystem::path Path; ~FileCleanup() { std::error_code error; std::filesystem::remove(Path, error); } } cleanup{path};
    ASSERT_TRUE(Scene::SaveSceneToFile(*world, path));
    std::ifstream input(path);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(text.find("NavMap"), std::string::npos);
    EXPECT_EQ(text.find("OwnerWorld"), std::string::npos);
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options; options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(*world, path, options));
    ASSERT_FALSE(degradation.IsDegraded());
    EXPECT_EQ(NavigationService::Get().GetGridMap(old), nullptr);
    EXPECT_EQ(world->Query<ECS::Read<Grid>>().Count(), 1u);
    world->Query<ECS::Read<Grid>>().Each([](const Grid& grid) {
        EXPECT_FALSE(grid.Initialized); EXPECT_EQ(grid.NavMapGeneration, 0u); EXPECT_TRUE(grid.NeedsRebake);
        EXPECT_EQ(grid.OwnerWorldId, 0u);
    });
    build.Update(*world, 0);
    world->Query<ECS::Read<Grid>>().Each([](const Grid& grid) {
        EXPECT_TRUE(grid.Initialized); EXPECT_TRUE(grid.NeedsRebake);
        EXPECT_NE(NavigationService::Get().GetGridMap(Handle(grid)), nullptr);
    });
}

TEST_F(NavigationGridOwnershipTest, ManualCreationPublishesCurrentOwnerWithoutClaimingBakeReadiness)
{
    Grid grid{};
    grid.NeedsRebake = false; // An incoming flag cannot make fresh storage ready.
    Pathfinding::GridSettings settings{}; settings.Width = settings.Depth = 8; settings.OriginX = 17;
    const auto handle = CreateNavigationGridMap(*world, grid, settings);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(grid.OwnerWorldId, world->GetWorldId());
    EXPECT_TRUE(grid.Initialized);
    EXPECT_TRUE(grid.NeedsRebake);
    auto* map = ResolveNavigationGridMap(*world, grid);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->GetSettings().OriginX, 17);
    ResetNavigationGridMap(*world, grid);
    EXPECT_EQ(NavigationService::Get().GetGridMap(handle), nullptr);
    EXPECT_FALSE(grid.Initialized);
    EXPECT_EQ(grid.OwnerWorldId, 0u);
    ResetNavigationGridMap(*world, grid);
}

TEST_F(NavigationGridOwnershipTest, CreateRefusesCurrentOwnerWithoutReplacingOrMutatingIt)
{
    const auto entity = Make(*world);
    auto* grid = world->GetComponentForWrite<Grid>(entity);
    const Grid original = *grid;
    auto* map = ResolveNavigationGridMap(*world, *grid);
    ASSERT_NE(map, nullptr);
    map->SetCellDynamicBlocked(2, 3, true);
    auto different = map->GetSettings(); different.Width = 19;
    EXPECT_FALSE(CreateNavigationGridMap(*world, *grid, different).IsValid());
    ExpectBindingEqual(*grid, original);
    ExpectSentinel(Handle(original));
    EXPECT_EQ(map->GetSettings().Width, 8u);
}

TEST_F(NavigationGridOwnershipTest, StaleCreationNeverReleasesOrAdoptsTheRecreatedServicesMap)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Grid>(entity)));
    auto* grid = world->GetComponentForWrite<Grid>(entity);
    const auto original = *grid;
    ASSERT_FALSE(original.NeedsRebake);
    EXPECT_EQ(ResolveNavigationGridMap(*world, *grid), nullptr);
    ExpectBindingEqual(*grid, original); // Resolving is read-only.
    const auto settings = NavigationService::Get().GetGridMap(unrelated)->GetSettings();
    const auto replacement = CreateNavigationGridMap(*world, *grid, settings);
    EXPECT_TRUE(replacement.IsValid());
    EXPECT_NE(replacement, unrelated);
    EXPECT_TRUE(grid->NeedsRebake);
    EXPECT_NE(ResolveNavigationGridMap(*world, *grid), nullptr);
    ExpectSentinel(unrelated);
}

TEST_F(NavigationGridOwnershipTest, MissingServiceCreationLeavesExistingComponentUnchanged)
{
    const auto entity = Make(*world);
    auto* grid = world->GetComponentForWrite<Grid>(entity);
    const Grid original = *grid;
    const auto settings = ResolveNavigationGridMap(*world, *grid)->GetSettings();
    NavigationService::Shutdown();
    EXPECT_FALSE(CreateNavigationGridMap(*world, *grid, settings).IsValid());
    ExpectBindingEqual(*grid, original);
    EXPECT_EQ(ResolveNavigationGridMap(*world, *grid), nullptr);
    ResetNavigationGridMap(*world, *grid);
    EXPECT_FALSE(grid->Initialized);
    EXPECT_EQ(grid->OwnerWorldId, 0u);
}

TEST_F(NavigationGridOwnershipTest, UnstampedLegacyBindingIsNeverAdoptedOrReleased)
{
    Pathfinding::GridSettings settings{}; settings.Width = settings.Depth = 8;
    const auto unrelated = NavigationService::Get().AddGridMap(settings);
    NavigationService::Get().GetGridMap(unrelated)->SetCellDynamicBlocked(2, 3, true);
    Grid legacy{};
    legacy.NavMapIndex = unrelated.Index; legacy.NavMapGeneration = unrelated.Generation; legacy.Initialized = true;
    EXPECT_EQ(ResolveNavigationGridMap(*world, legacy), nullptr);
    ReleaseNavigationGridMap(legacy);
    EXPECT_FALSE(legacy.Initialized);
    ExpectSentinel(unrelated);
}

TEST_F(NavigationGridOwnershipTest, WorldAwareSchemaResetPreservesForeignOwners)
{
    const auto entity = Make(*world);
    const auto original = *world->GetComponent<Grid>(entity);
    NavigationService::Get().GetGridMap(Handle(original))->SetCellDynamicBlocked(2, 3, true);
    ECS::World other;
    RegisterNavigationWorldHooks(other);
    const auto copy = other.Create(original).GetHandle();
    const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationGrid");
    ASSERT_NE(schema, nullptr);
    std::string error;
    ASSERT_TRUE(schema->ApplyProperty(other, copy, {}, "width", "10", &error)) << error;
    ExpectSentinel(Handle(original));
    EXPECT_FALSE(other.GetComponent<Grid>(copy)->Initialized);
    other.AddComponentImmediate(copy, original);
    ASSERT_TRUE(schema->Remove(other, copy));
    ExpectSentinel(Handle(original));
}

TEST_F(NavigationGridOwnershipTest, MovementBeforeRebuildDoesNotExpireForeignReservations)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Grid>(entity)));
    auto* sentinel = NavigationService::Get().GetGridMap(unrelated);
    ASSERT_NE(sentinel, nullptr);
    sentinel->ReserveCell(4, 5, 117, .5f);
    NavigationMovementSystem movement;
    movement.Update(*world, 1);
    EXPECT_EQ(sentinel->GetCellReservation(4, 5).AgentId, 117u);
    ExpectSentinel(unrelated);
}

TEST_F(NavigationGridOwnershipTest, MovementStillExpiresCurrentOwnerReservations)
{
    const auto entity = Make(*world);
    auto* map = ResolveNavigationGridMap(*world, *world->GetComponent<Grid>(entity));
    ASSERT_NE(map, nullptr);
    map->ReserveCell(4, 5, 117, .5f);
    NavigationMovementSystem movement;
    movement.Update(*world, 1);
    EXPECT_EQ(map->GetCellReservation(4, 5).AgentId, 0u);
}
} // namespace
