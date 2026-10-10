#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "Editor/Entities/EntityDuplicate.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Animation/Animator.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphStore.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Pathfinding/NavigationWorld.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PhysicsECS/Systems/CharacterControllerSystem.h"
#include "PhysicsECS/Systems/PhysicsInitSystem.h"
#include "PhysicsECS/Systems/PhysicsWorldHooks.h"
#include "PhysicsECS/Systems/PhysicsWritebackSystem.h"
#include "Scene/SceneIO.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/Components/TerrainPlanetFaceCollider.h"
#include "TerrainECS/Components/TerrainTileCollider.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/TerrainService.h"

#include "TestTempDir.h"

using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;

using GameEngine::Components::Animator;
using GameEngine::Components::BoxColliderShape;
using GameEngine::Components::CharacterController;
using GameEngine::Components::MeshGPUData;
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::MorphTargetWeights;
using GameEngine::Components::Name;
using GameEngine::Components::Parent;
using GameEngine::Components::PhysicsBody;
using GameEngine::Components::PhysicsCollider;
using GameEngine::Components::SkeletonRef;
using GameEngine::Components::SphereColliderShape;
using GameEngine::Components::SplineComponent;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;

using GameEngine::Engine::Renderer::SkeletonStore;

using GameEngine::Editor::DeleteEntitiesCommand;
using GameEngine::Editor::DuplicateEntitiesCommand;
using GameEngine::Editor::UndoRedoService;

using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{
struct CreatedSplineEntity
{
    GameEngine::ECS::EntityHandle Entity;
    GameEngine::SplineECS::SplineHandle Spline;
};

class EntityDuplicateTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }

    World* GetWorld() const
    {
        static World world;
        return &world;
    }
};

CreatedSplineEntity CreateSplineEntity(World& world, GameEngine::SplineECS::SplineService& splineService)
{
    const auto originalHandle =
        splineService.CreateSpline(GameEngine::Spline::SplineType::CatmullRom, false);
    auto* originalData = splineService.GetSplineData(originalHandle);
    EXPECT_NE(originalData, nullptr);
    if (!originalData)
        return {};
    originalData->AddPoint(Vector3(0.0f, 0.0f, 0.0f), 2.0f);
    originalData->AddPoint(Vector3(5.0f, 0.0f, 0.0f), 3.0f);
    originalData->SetPointRotation(1u, Vector3(0.0f, 45.0f, 0.0f));
    originalData->SetPointTangents(1u, Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
    splineService.RebuildCache(originalHandle);

    Entity entity = world.Create();
    entity.Set(Transform::FromTRS(
        Vector3(10.0f, 0.0f, 2.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    Name name{};
    std::strncpy(name.value, "Spline", sizeof(name.value) - 1);
    entity.Set(name);

    SplineComponent spline{};
    spline.SplineDataIndex = originalHandle.Index();
    spline.SplineDataGeneration = originalHandle.Generation();
    spline.DefaultRadius = 7.0f;
    entity.Set(spline);

    return {entity.GetHandle(), originalHandle};
}

std::size_t CountEntities(World& world)
{
    std::size_t count = 0;
    for (auto* archetype : world.GetAllArchetypes())
    {
        if (!archetype)
            continue;
        for (const auto& entity : archetype->CollectEntities())
        {
            if (entity.IsValid() && world.IsValid(entity))
                ++count;
        }
    }
    return count;
}

std::vector<std::uint8_t> CaptureTransformBytes(World& world, EntityHandle entity)
{
    std::vector<std::uint8_t> bytes;
    EXPECT_TRUE(world.CaptureComponentBytes(entity, GameEngine::ECS::GetComponentTypeId<Transform>(), bytes));
    return bytes;
}

EntityHandle CreateNamedTransformEntity(World& world, const char* name, const Vector3& position)
{
    Entity entity = world.Create();
    entity.Set(Transform::FromTRS(position, Quaternion(1.0f, 0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));

    Name n{};
    std::strncpy(n.value, name, sizeof(n.value) - 1);
    entity.Set(n);

    return entity.GetHandle();
}

struct DuplicatedSubtree
{
    EntityHandle OriginalRoot;
    EntityHandle OriginalChild;
    EntityHandle CloneRoot;
    std::vector<EntityHandle> Clones;
};

// Parent (Transform+Name) with one child (Transform+Parent), duplicated once.
// Returns the clone set the way the panel call sites build it: NewRoots plus
// their full subtrees via CollectSubtree.
DuplicatedSubtree BuildAndDuplicateParentChild(World& world)
{
    DuplicatedSubtree out{};
    out.OriginalRoot = CreateNamedTransformEntity(world, "Root", Vector3(1.0f, 2.0f, 3.0f));
    out.OriginalChild = CreateNamedTransformEntity(world, "Child", Vector3(-4.0f, 0.5f, 8.0f));
    world.ProcessCommands();
    world.AddComponentImmediate(out.OriginalChild, Parent{out.OriginalRoot});

    const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(world, {out.OriginalRoot});
    EXPECT_EQ(duplicate.NewRoots.size(), 1u);
    if (duplicate.NewRoots.empty())
        return out;

    out.CloneRoot = duplicate.NewRoots.front();
    out.Clones = DeleteEntitiesCommand::CollectSubtree(world, out.CloneRoot);
    EXPECT_EQ(out.Clones.size(), 2u);
    return out;
}

GameEngine::ECS::EntityHandle FindEntityByName(World& world, const char* name)
{
    for (auto* archetype : world.GetAllArchetypes())
    {
        if (!archetype)
            continue;

        for (const auto& entity : archetype->CollectEntities())
        {
            if (!entity.IsValid() || !world.IsValid(entity))
                continue;

            const auto* componentName = world.GetComponent<Name>(entity);
            if (componentName && componentName->View() == name)
                return entity;
        }
    }

    return {};
}

TEST_F(EntityDuplicateTests, DuplicatedNavigationGridCreatesItsOwnMap)
{
    using GameEngine::Components::NavigationGrid;
    using GameEngine::PathfindingECS::NavigationService;
    if (!NavigationService::IsInitialized())
        NavigationService::Initialize();
    World world;
    GameEngine::PathfindingECS::RegisterNavigationWorldHooks(world);
    GameEngine::PathfindingECS::NavigationBuildSystem build;

    NavigationGrid grid{};
    grid.Width = grid.Depth = 4;
    grid.BakeSource = GameEngine::Components::NavigationBakeSource::Manual;
    const EntityHandle original = world.Create(grid, Name{"Grid"}).GetHandle();
    build.Update(world, 0.0f);
    const auto* bound = world.GetComponent<NavigationGrid>(original);
    ASSERT_NE(bound, nullptr);
    ASSERT_TRUE(bound->Initialized);
    const GameEngine::Pathfinding::NavMapHandle originalMap{bound->NavMapIndex, bound->NavMapGeneration};
    ASSERT_NE(NavigationService::Get().GetGridMap(originalMap), nullptr);

    const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(world, {original});
    ASSERT_EQ(duplicate.NewRoots.size(), 1u);
    const EntityHandle copy = duplicate.NewRoots[0];
    const auto* copied = world.GetComponent<NavigationGrid>(copy);
    ASSERT_NE(copied, nullptr);
    EXPECT_FALSE(copied->Initialized);
    EXPECT_EQ(copied->Width, 4u);

    build.Update(world, 0.0f);
    copied = world.GetComponent<NavigationGrid>(copy);
    ASSERT_TRUE(copied->Initialized);
    const GameEngine::Pathfinding::NavMapHandle copyMap{copied->NavMapIndex, copied->NavMapGeneration};
    EXPECT_NE(copyMap, originalMap);
    EXPECT_NE(NavigationService::Get().GetGridMap(copyMap), nullptr);

    world.DestroyEntityImmediate(copy);
    EXPECT_NE(NavigationService::Get().GetGridMap(originalMap), nullptr);
    EXPECT_EQ(NavigationService::Get().GetGridMap(copyMap), nullptr);
}

TEST_F(EntityDuplicateTests, DuplicatingSplineCreatesIndependentSplineData)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto created = CreateSplineEntity(*world, *splineService);
    ASSERT_TRUE(created.Entity.IsValid());
    const auto originalHandle = created.Spline;

    world->ProcessCommands();

    const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(
        *world, {created.Entity});

    ASSERT_EQ(duplicate.NewRoots.size(), 1u);
    const auto clonedEntity = duplicate.NewRoots.front();
    ASSERT_TRUE(clonedEntity.IsValid());
    ASSERT_TRUE(world->IsValid(clonedEntity));

    const auto* originalSpline = world->GetComponent<SplineComponent>(created.Entity);
    ASSERT_NE(originalSpline, nullptr);
    const auto* clonedSpline = world->GetComponent<SplineComponent>(clonedEntity);
    ASSERT_NE(clonedSpline, nullptr);
    EXPECT_EQ(clonedSpline->DefaultRadius, originalSpline->DefaultRadius);
    EXPECT_EQ(Entity(world, clonedEntity).IsEnabled<SplineComponent>(),
              Entity(world, created.Entity).IsEnabled<SplineComponent>());

    EXPECT_NE(clonedSpline->SplineDataIndex, originalHandle.Index());

    const GameEngine::SplineECS::SplineHandle clonedHandle(
        clonedSpline->SplineDataIndex,
        clonedSpline->SplineDataGeneration);
    auto* originalData = splineService->GetSplineData(originalHandle);
    ASSERT_NE(originalData, nullptr);
    auto* clonedData = splineService->GetSplineData(clonedHandle);
    ASSERT_NE(clonedData, nullptr);

    ASSERT_EQ(clonedData->Points.size(), originalData->Points.size());
    EXPECT_EQ(clonedData->Type, originalData->Type);
    EXPECT_EQ(clonedData->Closed, originalData->Closed);
    EXPECT_NEAR(clonedData->Points[1].Position.x, originalData->Points[1].Position.x, 1.0e-5f);
    EXPECT_NEAR(clonedData->Points[1].Radius, originalData->Points[1].Radius, 1.0e-5f);
    EXPECT_NEAR(clonedData->Points[1].Rotation.y, originalData->Points[1].Rotation.y, 1.0e-5f);
    EXPECT_NEAR(clonedData->Points[1].TangentOut.x, originalData->Points[1].TangentOut.x, 1.0e-5f);

    clonedData->SetPointPosition(1u, Vector3(99.0f, 0.0f, 0.0f));
    splineService->RebuildCache(clonedHandle);

    EXPECT_NEAR(originalData->Points[1].Position.x, 5.0f, 1.0e-5f);
    EXPECT_NEAR(clonedData->Points[1].Position.x, 99.0f, 1.0e-5f);
}

TEST_F(EntityDuplicateTests, DuplicatedSplineStaysIndependentAfterSceneSaveLoad)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto created = CreateSplineEntity(*world, *splineService);
    ASSERT_TRUE(created.Entity.IsValid());

    world->ProcessCommands();

    const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(
        *world, {created.Entity});
    ASSERT_EQ(duplicate.NewRoots.size(), 1u);

    const auto* clonedSpline = world->GetComponent<SplineComponent>(duplicate.NewRoots.front());
    ASSERT_NE(clonedSpline, nullptr);

    const GameEngine::SplineECS::SplineHandle clonedHandle(
        clonedSpline->SplineDataIndex,
        clonedSpline->SplineDataGeneration);
    auto* clonedData = splineService->GetSplineData(clonedHandle);
    ASSERT_NE(clonedData, nullptr);
    clonedData->SetPointPosition(1u, Vector3(99.0f, 0.0f, 0.0f));
    splineService->RebuildCache(clonedHandle);

    const GameEngine::TestUtils::ScopedTempDir sceneDir{
        GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngine_EntityDuplicateTests")};
    const auto scenePath = sceneDir.Path() / "duplicated_spline_roundtrip.scene";

    ASSERT_TRUE(GameEngine::Scene::SaveSceneToFile(*world, scenePath, GameEngine::Scene::SaveOptions{}));

    World loadedWorld;
    ASSERT_TRUE(GameEngine::Scene::LoadSceneFromFile(
        loadedWorld,
        scenePath,
        GameEngine::Scene::LoadOptions{GameEngine::Scene::LoadMode::Replace}));

    const auto loadedOriginalEntity = FindEntityByName(loadedWorld, "Spline");
    const auto loadedCloneEntity = FindEntityByName(loadedWorld, "Spline (1)");
    ASSERT_TRUE(loadedOriginalEntity.IsValid());
    ASSERT_TRUE(loadedCloneEntity.IsValid());

    const auto* loadedOriginalSpline = loadedWorld.GetComponent<SplineComponent>(loadedOriginalEntity);
    const auto* loadedCloneSpline = loadedWorld.GetComponent<SplineComponent>(loadedCloneEntity);
    ASSERT_NE(loadedOriginalSpline, nullptr);
    ASSERT_NE(loadedCloneSpline, nullptr);
    EXPECT_NE(loadedOriginalSpline->SplineDataIndex, loadedCloneSpline->SplineDataIndex);

    const GameEngine::SplineECS::SplineHandle loadedOriginalHandle(
        loadedOriginalSpline->SplineDataIndex,
        loadedOriginalSpline->SplineDataGeneration);
    const GameEngine::SplineECS::SplineHandle loadedCloneHandle(
        loadedCloneSpline->SplineDataIndex,
        loadedCloneSpline->SplineDataGeneration);

    auto* loadedOriginalData = splineService->GetSplineData(loadedOriginalHandle);
    auto* loadedCloneData = splineService->GetSplineData(loadedCloneHandle);
    ASSERT_NE(loadedOriginalData, nullptr);
    ASSERT_NE(loadedCloneData, nullptr);
    ASSERT_GT(loadedOriginalData->Points.size(), 1u);
    ASSERT_GT(loadedCloneData->Points.size(), 1u);

    EXPECT_NEAR(loadedOriginalData->Points[1].Position.x, 5.0f, 1.0e-5f);
    EXPECT_NEAR(loadedCloneData->Points[1].Position.x, 99.0f, 1.0e-5f);

    loadedCloneData->SetPointPosition(1u, Vector3(42.0f, 0.0f, 0.0f));
    splineService->RebuildCache(loadedCloneHandle);

    EXPECT_NEAR(loadedOriginalData->Points[1].Position.x, 5.0f, 1.0e-5f);
    EXPECT_NEAR(loadedCloneData->Points[1].Position.x, 42.0f, 1.0e-5f);
}

TEST_F(EntityDuplicateTests, DuplicateUndoRemovesClonesAndKeepsOriginalsIntact)
{
    World* world = GetWorld();

    const DuplicatedSubtree dup = BuildAndDuplicateParentChild(*world);
    ASSERT_TRUE(dup.CloneRoot.IsValid());
    EXPECT_EQ(CountEntities(*world), 4u);

    const std::vector<std::uint8_t> rootBytesBefore = CaptureTransformBytes(*world, dup.OriginalRoot);
    const std::vector<std::uint8_t> childBytesBefore = CaptureTransformBytes(*world, dup.OriginalChild);

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", world, nullptr, dup.Clones,
        std::function<void()>{}, std::function<void()>{}));
    EXPECT_STREQ(svc.PeekUndoName(), "Duplicate Entity");
    // Committing an already-applied duplication must not mutate the world.
    EXPECT_EQ(CountEntities(*world), 4u);

    svc.Undo();

    for (const auto& clone : dup.Clones)
        EXPECT_FALSE(world->IsValid(clone));
    EXPECT_EQ(CountEntities(*world), 2u);

    ASSERT_TRUE(world->IsValid(dup.OriginalRoot));
    ASSERT_TRUE(world->IsValid(dup.OriginalChild));
    EXPECT_EQ(CaptureTransformBytes(*world, dup.OriginalRoot), rootBytesBefore);
    EXPECT_EQ(CaptureTransformBytes(*world, dup.OriginalChild), childBytesBefore);
    const auto* childParent = world->GetComponent<Parent>(dup.OriginalChild);
    ASSERT_NE(childParent, nullptr);
    EXPECT_EQ(childParent->parent.id, dup.OriginalRoot.id);
}

TEST_F(EntityDuplicateTests, DuplicateUndoRedoRevivesSameHandlesWithSameBytes)
{
    World* world = GetWorld();

    const DuplicatedSubtree dup = BuildAndDuplicateParentChild(*world);
    ASSERT_TRUE(dup.CloneRoot.IsValid());

    std::vector<std::vector<std::uint8_t>> cloneBytesAfterDuplicate;
    for (const auto& clone : dup.Clones)
        cloneBytesAfterDuplicate.push_back(CaptureTransformBytes(*world, clone));

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", world, nullptr, dup.Clones,
        std::function<void()>{}, std::function<void()>{}));

    svc.Undo();
    EXPECT_FALSE(world->IsValid(dup.CloneRoot));

    svc.Redo();

    // The exact same handles are alive again — later history entries that
    // reference the clones stay valid.
    for (std::size_t i = 0; i < dup.Clones.size(); ++i)
    {
        ASSERT_TRUE(world->IsValid(dup.Clones[i]));
        EXPECT_EQ(CaptureTransformBytes(*world, dup.Clones[i]), cloneBytesAfterDuplicate[i]);
    }
    EXPECT_EQ(CountEntities(*world), 4u);

    const auto* cloneName = world->GetComponent<Name>(dup.CloneRoot);
    ASSERT_NE(cloneName, nullptr);
    EXPECT_EQ(cloneName->View(), "Root (1)");
}

TEST_F(EntityDuplicateTests, ParentedSubtreeCloneParentLinksSurviveUndoRedo)
{
    World* world = GetWorld();

    const DuplicatedSubtree dup = BuildAndDuplicateParentChild(*world);
    ASSERT_TRUE(dup.CloneRoot.IsValid());

    ASSERT_EQ(dup.Clones.size(), 2u);
    const EntityHandle cloneChild = (dup.Clones[0].id == dup.CloneRoot.id) ? dup.Clones[1] : dup.Clones[0];

    {
        const auto* p = world->GetComponent<Parent>(cloneChild);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->parent.id, dup.CloneRoot.id);
    }

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", world, nullptr, dup.Clones,
        std::function<void()>{}, std::function<void()>{}));

    svc.Undo();
    svc.Redo();

    ASSERT_TRUE(world->IsValid(cloneChild));
    ASSERT_TRUE(world->IsValid(dup.CloneRoot));
    const auto* p = world->GetComponent<Parent>(cloneChild);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->parent.id, dup.CloneRoot.id);
    EXPECT_NE(p->parent.id, dup.OriginalRoot.id);
}

namespace
{
// A placer with two generated children, the shape the spline placement and fence
// controllers build: the recipe entity is the user's, the pieces are output.
struct PlacerWithPieces
{
    EntityHandle Placer{};
    std::vector<EntityHandle> Pieces;
};

PlacerWithPieces BuildPlacerWithGeneratedPieces(World& world)
{
    PlacerWithPieces out;
    Entity placer = world.Create();
    placer.Set(Transform{});
    out.Placer = placer.GetHandle();

    for (int i = 0; i < 2; ++i)
    {
        Entity piece = world.Create();
        piece.Set(Transform{});
        piece.Set(GameEngine::Components::RuntimeOnlyEntity{});
        piece.Set(Parent{out.Placer});
        out.Pieces.push_back(piece.GetHandle());
    }
    world.ProcessCommands();
    return out;
}
} // namespace

// Generated children are regenerated by their controller, never carried by the
// user's delete. Capturing them in the delete snapshot would make Undo revive a
// stale set next to the one the controller rebuilds — doubled geometry that
// nothing owns.
TEST_F(EntityDuplicateTests, DeleteSubtreeSkipsGeneratedChildren)
{
    World* world = GetWorld();
    const PlacerWithPieces built = BuildPlacerWithGeneratedPieces(*world);

    const auto subtree = DeleteEntitiesCommand::CollectSubtree(*world, built.Placer);
    ASSERT_EQ(subtree.size(), 1u);
    EXPECT_EQ(subtree[0].id, built.Placer.id);

    // Selecting a generated piece directly still deletes that piece: only the
    // descent past a placer is filtered, never the requested root itself.
    const auto pieceSubtree = DeleteEntitiesCommand::CollectSubtree(*world, built.Pieces[0]);
    ASSERT_EQ(pieceSubtree.size(), 1u);
    EXPECT_EQ(pieceSubtree[0].id, built.Pieces[0].id);
}

// Duplicating a placer must produce a bare placer: its controller regenerates
// the pieces from the recipe the clone carries. Cloning them instead leaves the
// duplicate holding a stale set that its controller then duplicates again.
TEST_F(EntityDuplicateTests, DuplicateSkipsGeneratedChildren)
{
    World* world = GetWorld();
    const PlacerWithPieces built = BuildPlacerWithGeneratedPieces(*world);

    const auto result = GameEngine::Editor::DuplicateEntitySubtreeRoots(
        *world, std::vector<EntityHandle>{built.Placer});
    ASSERT_EQ(result.NewRoots.size(), 1u);
    const EntityHandle clone = result.NewRoots[0];
    ASSERT_TRUE(world->IsValid(clone));
    EXPECT_NE(clone.id, built.Placer.id);

    int clonedChildren = 0;
    world->Query<GameEngine::ECS::Read<Parent>>().Each(
        [&](EntityHandle, const Parent& p)
        {
            if (p.parent.id == clone.id)
                ++clonedChildren;
        });
    EXPECT_EQ(clonedChildren, 0);

    // The originals are untouched — the filter drops them from the clone walk,
    // it does not destroy them.
    EXPECT_TRUE(world->IsValid(built.Pieces[0]));
    EXPECT_TRUE(world->IsValid(built.Pieces[1]));
}

// The matching half of the CollectSubtree filter. Generated children are not captured, so
// Undo cannot revive them — but they must still be destroyed with their parent, or they sit
// parented to a dead entity, one hierarchy warning apiece, until the generator's next sweep.
// Undo brings back only the authored entity; the generator rebuilds the rest.
TEST_F(EntityDuplicateTests, DeleteDestroysGeneratedChildrenWithoutRevivingThemOnUndo)
{
    World* world = GetWorld();
    const PlacerWithPieces built = BuildPlacerWithGeneratedPieces(*world);

    const auto subtree = DeleteEntitiesCommand::CollectSubtree(*world, built.Placer);
    ASSERT_EQ(subtree.size(), 1u);

    UndoRedoService svc;
    svc.Execute(std::make_unique<DeleteEntitiesCommand>("Delete Entity", world, nullptr, subtree));

    EXPECT_FALSE(world->IsValid(built.Placer));
    EXPECT_FALSE(world->IsValid(built.Pieces[0])) << "generated child outlived its parent";
    EXPECT_FALSE(world->IsValid(built.Pieces[1])) << "generated child outlived its parent";

    svc.Undo();
    EXPECT_TRUE(world->IsValid(built.Placer));
    EXPECT_FALSE(world->IsValid(built.Pieces[0])) << "undo revived a stale generated child";
    EXPECT_FALSE(world->IsValid(built.Pieces[1])) << "undo revived a stale generated child";

    svc.Redo();
    EXPECT_FALSE(world->IsValid(built.Placer));
    EXPECT_FALSE(world->IsValid(built.Pieces[0]));
}

TEST_F(EntityDuplicateTests, InterleavedDuplicateDeleteUndoRedoSequence)
{
    World* world = GetWorld();

    const DuplicatedSubtree dup = BuildAndDuplicateParentChild(*world);
    ASSERT_TRUE(dup.CloneRoot.IsValid());

    const std::vector<std::uint8_t> rootBytesBefore = CaptureTransformBytes(*world, dup.OriginalRoot);

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", world, nullptr, dup.Clones,
        std::function<void()>{}, std::function<void()>{}));

    // Delete the clone subtree as a second history entry.
    svc.Execute(std::make_unique<DeleteEntitiesCommand>("Delete Entity", world, nullptr, dup.Clones));
    EXPECT_EQ(CountEntities(*world), 2u);
    EXPECT_FALSE(world->IsValid(dup.CloneRoot));

    // Undo delete: clones revived.
    svc.Undo();
    EXPECT_EQ(CountEntities(*world), 4u);
    for (const auto& clone : dup.Clones)
        EXPECT_TRUE(world->IsValid(clone));

    // Undo duplicate: clones gone, originals untouched.
    svc.Undo();
    EXPECT_EQ(CountEntities(*world), 2u);
    for (const auto& clone : dup.Clones)
        EXPECT_FALSE(world->IsValid(clone));
    ASSERT_TRUE(world->IsValid(dup.OriginalRoot));
    EXPECT_EQ(CaptureTransformBytes(*world, dup.OriginalRoot), rootBytesBefore);

    // Redo duplicate: same handles back.
    svc.Redo();
    EXPECT_EQ(CountEntities(*world), 4u);
    for (const auto& clone : dup.Clones)
        EXPECT_TRUE(world->IsValid(clone));

    // Redo delete: clones gone again.
    svc.Redo();
    EXPECT_EQ(CountEntities(*world), 2u);
    for (const auto& clone : dup.Clones)
        EXPECT_FALSE(world->IsValid(clone));
}

TEST_F(EntityDuplicateTests, SelectionCallbacksFireAfterWorldMutation)
{
    World* world = GetWorld();

    const DuplicatedSubtree dup = BuildAndDuplicateParentChild(*world);
    ASSERT_TRUE(dup.CloneRoot.IsValid());

    int restoredCalls = 0;
    int removedCalls = 0;
    bool removedSawClonesGone = false;
    bool restoredSawClonesAlive = false;
    const EntityHandle cloneRoot = dup.CloneRoot;

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", world, nullptr, dup.Clones,
        [&]()
        {
            ++restoredCalls;
            restoredSawClonesAlive = world->IsValid(cloneRoot);
        },
        [&]()
        {
            ++removedCalls;
            removedSawClonesGone = !world->IsValid(cloneRoot);
        }));

    // Commit of an already-applied duplication runs neither callback — the call
    // site applies the initial selection itself.
    EXPECT_EQ(restoredCalls, 0);
    EXPECT_EQ(removedCalls, 0);

    svc.Undo();
    EXPECT_EQ(removedCalls, 1);
    EXPECT_EQ(restoredCalls, 0);
    EXPECT_TRUE(removedSawClonesGone);

    svc.Redo();
    EXPECT_EQ(removedCalls, 1);
    EXPECT_EQ(restoredCalls, 1);
    EXPECT_TRUE(restoredSawClonesAlive);
}

// A skinned model instance is several entities — submeshes and imported bone
// nodes — whose SkeletonRefs all name ONE reference-counted SkeletonStore
// runtime (SkeletonStore.h ownership rule). Duplicating the instance must
// produce a SECOND instance: its own runtime, so the two animate
// independently, with the clone's entities sharing that one runtime exactly
// as the original's entities share theirs.
class SkinnedEntityDuplicateTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& store = SkeletonStore::Instance();
        m_SkelId = store.CreateSkeleton(3);
        auto* skel = store.Get(m_SkelId);
        ASSERT_NE(skel, nullptr);
        skel->SkinJointCount = 3;
        skel->JointNodes = {0, 1, 2};

        // Mirrors the engine's hook (Engine/Source/Core/Engine.cpp): a live
        // SkeletonRef owns one reference, dropped when the component goes.
        m_World.RegisterOnRemove<SkeletonRef>([](SkeletonRef& ref) {
            if (ref.runtimeId != 0)
                SkeletonStore::Instance().ReleaseRuntime(ref.runtimeId);
        });
    }

    // ModelEntityFactory's spawn shape: one runtime, a root and one child
    // naming it, each holding one reference.
    void SpawnSkinnedInstance()
    {
        auto& store = SkeletonStore::Instance();
        m_RuntimeId = store.CreateRuntime(m_SkelId);

        m_Root = CreateNamedTransformEntity(m_World, "Skinned", Vector3(1.0f, 2.0f, 3.0f));
        m_Child = CreateNamedTransformEntity(m_World, "SkinnedChild", Vector3(0.0f, 1.0f, 0.0f));
        m_World.ProcessCommands();
        m_World.AddComponentImmediate(m_Child, Parent{m_Root});

        SkeletonRef ref{};
        ref.skeletonId = m_SkelId;
        ref.runtimeId = m_RuntimeId;
        for (EntityHandle e : {m_Root, m_Child})
        {
            m_World.AddComponentImmediate(e, ref);
            store.RetainRuntime(m_RuntimeId);
        }
        store.ReleaseRuntime(m_RuntimeId); // construction reference
    }

    std::vector<EntityHandle> DuplicateInstance()
    {
        const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(m_World, {m_Root});
        EXPECT_EQ(duplicate.NewRoots.size(), 1u);
        if (duplicate.NewRoots.empty())
            return {};
        return DeleteEntitiesCommand::CollectSubtree(m_World, duplicate.NewRoots.front());
    }

    std::uint32_t RuntimeOf(EntityHandle e) const
    {
        const auto* ref = m_World.GetComponent<SkeletonRef>(e);
        return ref ? ref->runtimeId : 0u;
    }

    World m_World;
    std::uint32_t m_SkelId = 0;
    std::uint32_t m_RuntimeId = 0;
    EntityHandle m_Root{};
    EntityHandle m_Child{};
};

TEST_F(SkinnedEntityDuplicateTests, DuplicateGetsItsOwnRuntimeSharedAcrossItsSubtree)
{
    auto& store = SkeletonStore::Instance();

    SpawnSkinnedInstance();
    ASSERT_NE(m_RuntimeId, 0u);
    ASSERT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u);

    const auto clones = DuplicateInstance();
    ASSERT_EQ(clones.size(), 2u);

    const std::uint32_t cloneRuntime = RuntimeOf(clones[0]);
    ASSERT_NE(cloneRuntime, 0u) << "the clone lost its skeleton binding entirely";
    EXPECT_NE(cloneRuntime, m_RuntimeId)
        << "the clone shares the original's runtime — one pose for two instances";
    EXPECT_EQ(RuntimeOf(clones[1]), cloneRuntime)
        << "the clone's own entities must share ONE runtime, as the original's do";

    for (EntityHandle e : clones)
    {
        const auto* ref = m_World.GetComponent<SkeletonRef>(e);
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->skeletonId, m_SkelId) << "the clone must keep the shared skeleton data";
    }

    // Reference accounting: two components on each side, two references each.
    EXPECT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u)
        << "duplicating changed the ORIGINAL instance's reference count";
    EXPECT_EQ(store.GetRuntimeRefCount(cloneRuntime), 2u)
        << "the clone's two SkeletonRefs must hold exactly two references";
}

// The user-visible defect: duplicate a skinned model, delete the original, and
// the duplicate must still animate.
TEST_F(SkinnedEntityDuplicateTests, DeletingTheOriginalLeavesTheDuplicateIntact)
{
    auto& store = SkeletonStore::Instance();

    SpawnSkinnedInstance();
    const auto clones = DuplicateInstance();
    ASSERT_EQ(clones.size(), 2u);
    const std::uint32_t cloneRuntime = RuntimeOf(clones[0]);
    ASSERT_NE(cloneRuntime, 0u);

    m_World.DestroyEntityImmediate(m_Child);
    m_World.DestroyEntityImmediate(m_Root);

    EXPECT_EQ(store.GetRuntime(m_RuntimeId), nullptr)
        << "the original's runtime should be freed once its last SkeletonRef goes";
    ASSERT_NE(store.GetRuntime(cloneRuntime), nullptr)
        << "deleting the original freed the runtime the duplicate still names";
    EXPECT_EQ(store.GetRuntimeSkeletonId(cloneRuntime), m_SkelId);
    EXPECT_EQ(store.GetRuntimeRefCount(cloneRuntime), 2u);

    // A spawn after the delete must not be handed the clone's live slot.
    const std::uint32_t intruder = store.CreateRuntime(m_SkelId);
    EXPECT_NE(intruder, cloneRuntime)
        << "the clone's runtime was on the free list while the clone still named it";
    store.ReleaseRuntime(intruder);
}

// The duplicate appears already posed, rather than snapping to bind pose until
// something re-evaluates the animation.
TEST_F(SkinnedEntityDuplicateTests, DuplicateStartsFromTheOriginalPose)
{
    auto& store = SkeletonStore::Instance();

    SpawnSkinnedInstance();
    auto* originalPose = store.GetRuntime(m_RuntimeId);
    ASSERT_NE(originalPose, nullptr);
    ASSERT_GE(originalPose->CompactSkinMatrices.size(), 14u);
    originalPose->CompactSkinMatrices[13] = 4.25f;

    const auto clones = DuplicateInstance();
    ASSERT_EQ(clones.size(), 2u);
    const std::uint32_t cloneRuntime = RuntimeOf(clones[0]);
    ASSERT_NE(cloneRuntime, 0u);

    auto* clonePose = store.GetRuntime(cloneRuntime);
    ASSERT_NE(clonePose, nullptr);
    ASSERT_EQ(clonePose->CompactSkinMatrices.size(), originalPose->CompactSkinMatrices.size());
    EXPECT_FLOAT_EQ(clonePose->CompactSkinMatrices[13], 4.25f)
        << "the duplicate did not inherit the pose it was duplicated from";

    // Independent storage, not an alias: posing one must not move the other.
    clonePose->CompactSkinMatrices[13] = 9.5f;
    EXPECT_FLOAT_EQ(originalPose->CompactSkinMatrices[13], 4.25f);

    // Per-frame atlas bookkeeping is NOT inherited — those offsets name slots
    // the atlas owns for the original this frame.
    EXPECT_FALSE(clonePose->PrevAtlasPaletteValid);
    EXPECT_EQ(clonePose->AtlasPaletteOffsetBones, 0u);
}

TEST_F(SkinnedEntityDuplicateTests, DuplicateUndoRedoCyclesKeepTheReferenceCountExact)
{
    auto& store = SkeletonStore::Instance();

    SpawnSkinnedInstance();
    const auto clones = DuplicateInstance();
    ASSERT_EQ(clones.size(), 2u);
    const std::uint32_t cloneRuntime = RuntimeOf(clones[0]);
    ASSERT_NE(cloneRuntime, 0u);

    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", &m_World, nullptr, clones,
        std::function<void()>{}, std::function<void()>{}));

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        svc.Undo();
        EXPECT_EQ(store.GetRuntimeRefCount(cloneRuntime), 2u)
            << "cycle " << cycle << ": the pending redo must hold what the clones held";
        EXPECT_NE(store.GetRuntime(cloneRuntime), nullptr)
            << "cycle " << cycle << ": undoing the duplicate freed a runtime redo will name";

        svc.Redo();
        ASSERT_TRUE(m_World.IsValid(clones[0])) << "cycle " << cycle;
        EXPECT_EQ(RuntimeOf(clones[0]), cloneRuntime) << "cycle " << cycle;
        EXPECT_EQ(store.GetRuntimeRefCount(cloneRuntime), 2u) << "cycle " << cycle;
    }

    // The original is untouched by the whole sequence.
    EXPECT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u);

    for (EntityHandle e : clones)
        m_World.DestroyEntityImmediate(e);
    EXPECT_EQ(store.GetRuntime(cloneRuntime), nullptr)
        << "the duplicate's runtime leaked once its entities were gone";

    // Destroying the clones must not have reached the original. Without these,
    // an aliasing duplicate satisfies the assertion above by driving the ONE
    // shared runtime to zero — the original's two live entities are left naming
    // a freed runtime and the test passes because of the defect it pins.
    EXPECT_NE(store.GetRuntime(m_RuntimeId), nullptr)
        << "destroying the duplicate freed the runtime the ORIGINAL still names";
    EXPECT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u)
        << "the original's two SkeletonRefs must still hold exactly two references";
}

EntityHandle DuplicateOnce(World& world, EntityHandle root)
{
    const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(world, {root});
    EXPECT_EQ(duplicate.NewRoots.size(), 1u);
    if (duplicate.NewRoots.empty())
        return {};
    return duplicate.NewRoots.front();
}

// ---------------------------------------------------------------------------
// Terrain and physics runtimes are NOT reference counted: one owner, freed on
// release. A byte-copied handle pair therefore still RESOLVES — it names the
// source's live resource — so a copy that keeps it frees the original's terrain
// or Jolt body the moment the copy is released. Two keystrokes reach that:
// Ctrl+D then Ctrl+Z, where the undo is a preserve-handle destroy of the clones
// and every removal hook fires against handles the clone never owned.
//
// The repair zeroes the copy's pairs, which is also the provisioning systems'
// "not provisioned yet" state, so the copy builds its own.
// ---------------------------------------------------------------------------

using GameEngine::Components::Terrain;
using GameEngine::Components::TerrainPlanetFaceCollider;
using GameEngine::Components::TerrainTileCollider;
using GameEngine::TerrainECS::TerrainService;

GameEngine::Terrain::TerrainConfig SingleTerrainConfig()
{
    GameEngine::Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = 129;
    cfg.HeightmapHeight = 129;
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.LODLevels = 4;
    return cfg;
}

GameEngine::TerrainECS::TiledTerrainConfig MakeTiledTerrainConfig()
{
    GameEngine::TerrainECS::TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 256.0f;
    cfg.SamplesPerMeter = 2.0f;
    return cfg;
}

// Commit the duplication as the editor does and undo it — the Ctrl+D / Ctrl+Z
// pair, through the real command objects.
void UndoADuplicationOf(World& world, EntityHandle cloneRoot)
{
    UndoRedoService undo;
    undo.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", &world, nullptr,
        DeleteEntitiesCommand::CollectSubtree(world, cloneRoot),
        std::function<void()>{}, std::function<void()>{}));
    undo.Undo();
}

class TerrainEntityDuplicateTests : public ::testing::Test
{
protected:
    // Shared with TerrainEntityUndoResolutionTests in this binary, which also
    // leaves the service up: shutting it down here would pull it out from under
    // whichever of the two runs second.
    void SetUp() override
    {
        if (!TerrainService::IsInitialized())
            TerrainService::Initialize();
    }

    static EntityHandle CreateTerrainEntity(World& world, GameEngine::TerrainECS::TerrainHandle handle)
    {
        const auto entity = world.CreateEntity();
        Terrain t{};
        t.SizeX = 256.0f;
        t.SizeZ = 256.0f;
        t.HeightScale = 64.0f;
        t.TerrainDataHandle = handle.Index;
        t.TerrainDataGeneration = handle.Generation;
        world.AddComponentImmediate(entity, t);
        return entity;
    }

    static EntityHandle CreateTiledTerrainEntity(World& world,
                                                 GameEngine::TerrainECS::TiledTerrainHandle handle)
    {
        const auto entity = world.CreateEntity();
        Terrain t{};
        t.SizeX = 4096.0f;
        t.SizeZ = 4096.0f;
        t.SamplesPerMeter = 2.0f;
        t.TiledTerrainHandle = handle.Index;
        t.TiledTerrainGeneration = handle.Generation;
        world.AddComponentImmediate(entity, t);
        return entity;
    }

    static EntityHandle DuplicateOne(World& world, EntityHandle source)
    {
        const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(world, {source});
        EXPECT_EQ(duplicate.NewRoots.size(), 1u);
        return duplicate.NewRoots.empty() ? EntityHandle{} : duplicate.NewRoots.front();
    }
};

TEST_F(TerrainEntityDuplicateTests, DuplicateDoesNotInheritTheSourcesTerrainHandles)
{
    auto& svc = TerrainService::Get();
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto handle = svc.CreateTerrain(SingleTerrainConfig());
    const auto original = CreateTerrainEntity(world, handle);

    const auto clone = DuplicateOne(world, original);
    ASSERT_TRUE(clone.IsValid());

    const auto* cloned = world.GetComponent<Terrain>(clone);
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->TerrainDataHandle, 0u)
        << "the copy names the source's TerrainService slot without owning it";
    EXPECT_EQ(cloned->TerrainDataGeneration, 0u);

    // The authoring fields are what the copy re-provisions from, so they must
    // survive the repair.
    EXPECT_FLOAT_EQ(cloned->SizeX, 256.0f);
    EXPECT_FLOAT_EQ(cloned->HeightScale, 64.0f);

    // The source keeps its terrain: repair reads only the copy.
    const auto* survivor = world.GetComponent<Terrain>(original);
    ASSERT_NE(survivor, nullptr);
    EXPECT_EQ(survivor->TerrainDataHandle, handle.Index);
    EXPECT_NE(svc.GetTerrainData(handle), nullptr);
}

// The two-keystroke defect: Ctrl+D, Ctrl+Z.
TEST_F(TerrainEntityDuplicateTests, UndoingADuplicateLeavesTheOriginalsTerrainAlive)
{
    auto& svc = TerrainService::Get();
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto handle = svc.CreateTerrain(SingleTerrainConfig());
    const auto original = CreateTerrainEntity(world, handle);
    ASSERT_NE(svc.GetTerrainData(handle), nullptr);

    const auto clone = DuplicateOne(world, original);
    ASSERT_TRUE(clone.IsValid());

    UndoADuplicationOf(world, clone);

    ASSERT_FALSE(world.IsValid(clone)) << "undo must have destroyed the clone";
    EXPECT_NE(svc.GetTerrainData(handle), nullptr)
        << "undoing the duplicate destroyed the ORIGINAL entity's terrain";
    const auto* survivor = world.GetComponent<Terrain>(original);
    ASSERT_NE(survivor, nullptr);
    EXPECT_EQ(survivor->TerrainDataHandle, handle.Index);
    EXPECT_EQ(survivor->TerrainDataGeneration, handle.Generation);
}

// Deleting the duplicate outright, rather than undoing the duplication.
TEST_F(TerrainEntityDuplicateTests, DeletingTheDuplicateLeavesTheOriginalsTerrainAlive)
{
    auto& svc = TerrainService::Get();
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto handle = svc.CreateTerrain(SingleTerrainConfig());
    const auto original = CreateTerrainEntity(world, handle);

    const auto clone = DuplicateOne(world, original);
    ASSERT_TRUE(clone.IsValid());
    world.DestroyEntityImmediate(clone);

    EXPECT_NE(svc.GetTerrainData(handle), nullptr)
        << "deleting the duplicate destroyed the ORIGINAL entity's terrain";
    const auto* survivor = world.GetComponent<Terrain>(original);
    ASSERT_NE(survivor, nullptr);
    EXPECT_EQ(survivor->TerrainDataHandle, handle.Index);
}

// Extraction zeroes TerrainDataHandle on a tiled terrain and carries only the
// tiled pair, so a repair that clears the single pair alone still aliases.
TEST_F(TerrainEntityDuplicateTests, UndoingADuplicateLeavesTheOriginalsTiledTerrainAlive)
{
    auto& svc = TerrainService::Get();
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto tiled = svc.CreateTiledTerrain(MakeTiledTerrainConfig());
    const auto original = CreateTiledTerrainEntity(world, tiled);
    ASSERT_NE(svc.GetTiledTerrainData(tiled), nullptr);

    const auto clone = DuplicateOne(world, original);
    ASSERT_TRUE(clone.IsValid());
    const auto* cloned = world.GetComponent<Terrain>(clone);
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->TiledTerrainHandle, 0u)
        << "the copy names the source's tiled store without owning it";
    EXPECT_EQ(cloned->TiledTerrainGeneration, 0u);

    UndoADuplicationOf(world, clone);

    EXPECT_NE(svc.GetTiledTerrainData(tiled), nullptr)
        << "undoing the duplicate destroyed the ORIGINAL entity's tiled terrain";
}

// A non-tiled terrain's heightfield collider lives on the terrain entity itself
// (TerrainPhysicsSystem's single-terrain block), so Ctrl+D copies it along with
// the Terrain component — and it carries a SECOND copy of the terrain handle.
// Zeroing only the Terrain pair would send the clone off to build its own
// terrain while its collider still named the source's heightfield.
//
// The editor attaches the shape at terrain-creation time, so this is reachable
// without ever entering Play.
TEST_F(TerrainEntityDuplicateTests, DuplicateDoesNotInheritTheSourcesHeightFieldCollider)
{
    auto& svc = TerrainService::Get();
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto handle = svc.CreateTerrain(SingleTerrainConfig());
    const auto original = CreateTerrainEntity(world, handle);

    // As TerrainEntityProvisioning leaves it: the shape names the same terrain
    // the Terrain component does.
    GameEngine::Components::HeightFieldColliderShape shape{};
    shape.dataHandle = handle.Index;
    shape.dataGeneration = handle.Generation;
    shape.sizeX = 256.0f;
    shape.sizeZ = 256.0f;
    shape.heightScale = 64.0f;
    shape.lastBuiltVersion = 7;
    shape.lastSeenVersion = 7;
    world.AddComponentImmediate(original, shape);

    const auto clone = DuplicateOne(world, original);
    ASSERT_TRUE(clone.IsValid());

    // Removed, not zeroed: TerrainPhysicsSystem provisions by presence and never
    // re-points an existing shape, so a zeroed one would never be rebuilt and the
    // clone would have no collision at all.
    EXPECT_EQ(world.GetComponent<GameEngine::Components::HeightFieldColliderShape>(clone), nullptr)
        << "the copy kept a heightfield collider naming the SOURCE's terrain data";

    // The source keeps its collider intact; the repair reads only the copy.
    const auto* sourceShape =
        world.GetComponent<GameEngine::Components::HeightFieldColliderShape>(original);
    ASSERT_NE(sourceShape, nullptr) << "the repair reached the ORIGINAL's collider";
    EXPECT_EQ(sourceShape->dataHandle, handle.Index);
    EXPECT_EQ(sourceShape->lastBuiltVersion, 7u);
    EXPECT_NE(svc.GetTerrainData(handle), nullptr);
}

// The per-tile and per-face collider entities are runtime provisioning links
// that carry a heightfield physics handle. They are standalone roots rather
// than children of the terrain, so Ctrl+D reaches them only by selecting one
// directly — the hierarchy lists them, so that is a real two-keystroke path.
TEST_F(TerrainEntityDuplicateTests, DuplicateDoesNotInheritColliderPhysicsHandles)
{
    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);

    const auto tileEntity = world.CreateEntity();
    TerrainTileCollider tile{};
    tile.TiledIndex = 7;
    tile.TiledGeneration = 2;
    tile.TileX = 3;
    tile.TileZ = 4;
    tile.PhysicsHandleIndex = 11;
    tile.PhysicsHandleGeneration = 5;
    world.AddComponentImmediate(tileEntity, tile);

    const auto faceEntity = world.CreateEntity();
    TerrainPlanetFaceCollider face{};
    face.TerrainEntityIndex = 9;
    face.TerrainEntityVersion = 1;
    face.Face = 4;
    face.PhysicsHandleIndex = 13;
    face.PhysicsHandleGeneration = 6;
    world.AddComponentImmediate(faceEntity, face);

    const auto tileClone = DuplicateOne(world, tileEntity);
    ASSERT_TRUE(tileClone.IsValid());
    const auto* clonedTile = world.GetComponent<TerrainTileCollider>(tileClone);
    ASSERT_NE(clonedTile, nullptr);
    EXPECT_EQ(clonedTile->PhysicsHandleIndex, 0u)
        << "the copy names the source tile's heightfield physics handle";
    EXPECT_EQ(clonedTile->PhysicsHandleGeneration, 0u);
    EXPECT_EQ(clonedTile->TileX, 3) << "only the runtime handle is cleared";

    const auto faceClone = DuplicateOne(world, faceEntity);
    ASSERT_TRUE(faceClone.IsValid());
    const auto* clonedFace = world.GetComponent<TerrainPlanetFaceCollider>(faceClone);
    ASSERT_NE(clonedFace, nullptr);
    EXPECT_EQ(clonedFace->PhysicsHandleIndex, 0u)
        << "the copy names the source face's heightfield physics handle";
    EXPECT_EQ(clonedFace->PhysicsHandleGeneration, 0u);
    EXPECT_EQ(clonedFace->Face, 4u) << "only the runtime handle is cleared";

    // The originals are untouched.
    const auto* sourceTile = world.GetComponent<TerrainTileCollider>(tileEntity);
    ASSERT_NE(sourceTile, nullptr);
    EXPECT_EQ(sourceTile->PhysicsHandleIndex, 11u);
    const auto* sourceFace = world.GetComponent<TerrainPlanetFaceCollider>(faceEntity);
    ASSERT_NE(sourceFace, nullptr);
    EXPECT_EQ(sourceFace->PhysicsHandleIndex, 13u);
}

// A copied PhysicsBody carries the ORIGINAL's body and shape handles, and those
// handles are valid — the original still owns them. PhysicsInitSystem's
// "already has a valid body" early return therefore provisions nothing for the
// copy, and PhysicsWritebackSystem then poses the copy from a body it does not
// own: the duplicate does not merely lack physics, it MOVES WITH the thing it
// was duplicated from.
//
// Runtime simulation systems are off in Edit mode, so the window is a duplicate
// taken while a body is live — during Play or Paused, which Ctrl+D allows.
class PhysicsEntityDuplicateTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        using GameEngine::PhysicsECS::PhysicsWorldService;
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();

        GameEngine::Physics::PhysicsWorldSettings settings{};
        settings.gravity = GameEngine::Physics::Vector3(0.0f, -9.81f, 0.0f);
        settings.numThreads = 1;
        settings.maxBodies = 1024;
        settings.maxBodyPairs = 1024;
        settings.maxContactConstraints = 1024;
        settings.tempAllocatorBytes = 16u * 1024u * 1024u;
        PhysicsWorldService::Initialize(settings);

        // Writeback reads an alpha PhysicsStepSystem owns in a function-scope
        // static. Pin it so no earlier test in this process decides which pose
        // these assertions read. Nothing here steps the world, so bodies stay
        // exactly where they were created.
        PhysicsWorldService::SetRenderInterpolationAlpha(1.0f);
    }

    void TearDown() override
    {
        using GameEngine::PhysicsECS::PhysicsWorldService;
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }

    // Authoring components only, exactly what a scene carries: the handles are
    // PhysicsInitSystem's to provision.
    EntityHandle CreatePhysicsEntity(const Vector3& position)
    {
        const EntityHandle e = CreateNamedTransformEntity(m_World, "Body", position);
        m_World.ProcessCommands();

        WorldTransform wt{};
        const Transform local =
            Transform::FromTRS(position, Quaternion(1.0f, 0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        std::memcpy(wt.matrix, local.matrix, sizeof(wt.matrix));
        m_World.AddComponentImmediate(e, wt);

        m_World.AddComponentImmediate(e, PhysicsCollider{});

        SphereColliderShape sphere{};
        sphere.radius = 0.5f;
        m_World.AddComponentImmediate(e, sphere);

        PhysicsBody body{};
        body.motionType = GameEngine::Physics::MotionType::Dynamic;
        body.mass = 2.5f;
        body.gravityScale = 0.75f;
        m_World.AddComponentImmediate(e, body);
        return e;
    }

    void RunInit() { m_Init.Update(m_World, 1.0f / 60.0f); }
    void RunWriteback() { m_Writeback.Update(m_World, 1.0f / 60.0f); }

    bool HasLiveBody(EntityHandle e) const
    {
        auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
        const auto* body = m_World.GetComponent<PhysicsBody>(e);
        return pw && body && body->initialized && pw->IsBodyValid(body->body) &&
               pw->IsShapeValid(body->shape);
    }

    void MoveTo(EntityHandle e, const Vector3& position)
    {
        const Transform local =
            Transform::FromTRS(position, Quaternion(1.0f, 0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        m_World.AddComponentImmediate(e, local);

        WorldTransform wt{};
        if (const auto* existing = m_World.GetComponent<WorldTransform>(e))
            wt = *existing;
        std::memcpy(wt.matrix, local.matrix, sizeof(wt.matrix));
        ++wt.Version;
        m_World.AddComponentImmediate(e, wt);
    }

    // Column-major: translation is column 3.
    Vector3 WorldPositionOf(EntityHandle e) const
    {
        const auto* wt = m_World.GetComponent<WorldTransform>(e);
        if (!wt)
            return Vector3(0.0f, 0.0f, 0.0f);
        return Vector3(wt->matrix[12], wt->matrix[13], wt->matrix[14]);
    }

    World m_World;
    GameEngine::PhysicsECS::PhysicsInitSystem m_Init;
    GameEngine::PhysicsECS::PhysicsWritebackSystem m_Writeback;
};

TEST_F(PhysicsEntityDuplicateTests, DuplicateArrivesOwningNoBody)
{
    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    // Precondition, not decoration: the defect needs the original to hold a
    // LIVE body. Duplicate before one exists and the copied handles are dead,
    // both entities rebuild, and every assertion below passes on broken code.
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    const auto* body = m_World.GetComponent<PhysicsBody>(clone);
    ASSERT_NE(body, nullptr);
    EXPECT_FALSE(body->initialized) << "the copy claims to be provisioned";
    EXPECT_FALSE(body->body.IsValid()) << "the copy names the original's body";
    EXPECT_FALSE(body->shape.IsValid()) << "the copy names the original's shape";
    // The other two fields ClearPhysicsBodyRuntimeState resets are gated where a
    // fixture can actually set them: childShapeCount by
    // DuplicatingACompoundLeavesTheOriginalsChildShapesAlone (a second collider),
    // hasPrevPhysicsTransform by DuplicateIsNotBlendedFromTheOriginalsPreviousPose
    // (a mid-step alpha). Asserting them here would only restate this fixture's
    // single-sphere, never-stepped starting state.

    // Authoring data is not runtime state; clearing the handles must not reset it.
    const auto* originalBody = m_World.GetComponent<PhysicsBody>(original);
    ASSERT_NE(originalBody, nullptr);
    EXPECT_EQ(body->motionType, originalBody->motionType);
    EXPECT_FLOAT_EQ(body->mass, originalBody->mass);
    EXPECT_FLOAT_EQ(body->gravityScale, originalBody->gravityScale);
    EXPECT_EQ(Entity(&m_World, clone).IsEnabled<PhysicsBody>(), Entity(&m_World, original).IsEnabled<PhysicsBody>());
}

TEST_F(PhysicsEntityDuplicateTests, DuplicateGetsItsOwnBodyAndLeavesTheOriginalsAlone)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";
    const auto originalBodyHandle = m_World.GetComponent<PhysicsBody>(original)->body;
    const auto originalShapeHandle = m_World.GetComponent<PhysicsBody>(original)->shape;

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());
    RunInit();

    // The original is untouched. This is what a repair that clears `initialized`
    // but leaves the copied handles breaks: the copy stops taking the early
    // return, falls into the rebuild path, and destroys what it names — which
    // is the original's body, shape and compound children.
    const auto* originalAfter = m_World.GetComponent<PhysicsBody>(original);
    ASSERT_NE(originalAfter, nullptr);
    EXPECT_EQ(originalAfter->body.Value(), originalBodyHandle.Value())
        << "duplicating re-provisioned the ORIGINAL's body";
    EXPECT_TRUE(pw->IsBodyValid(originalAfter->body)) << "the copy destroyed the original's body";
    EXPECT_TRUE(pw->IsShapeValid(originalAfter->shape)) << "the copy destroyed the original's shape";

    // And the copy is simulated in its own right.
    //
    // The `initialized` / IsBodyValid pair below cannot fail in an unrepaired
    // world — the copy arrives holding the original's LIVE handles, so both are
    // satisfied by the defect itself. They are deliberately not the gate. They
    // guard the opposite failure: a repair that over-clears (or one that removes
    // the copy's PhysicsBody outright) leaves the duplicate with no body at all,
    // and only these two would notice. The EXPECT_NE pair below is what pins the
    // aliasing.
    const auto* cloneAfter = m_World.GetComponent<PhysicsBody>(clone);
    ASSERT_NE(cloneAfter, nullptr);
    EXPECT_TRUE(cloneAfter->initialized) << "PhysicsInitSystem never provisioned the duplicate";
    ASSERT_TRUE(pw->IsBodyValid(cloneAfter->body)) << "the duplicate has no body of its own";
    EXPECT_NE(cloneAfter->body.Value(), originalBodyHandle.Value())
        << "the duplicate shares the original's body — two entities, one rigid body";
    EXPECT_NE(cloneAfter->shape.Value(), originalShapeHandle.Value())
        << "the duplicate shares the original's shape";
}

// The undo entry is built the instant the duplicate is made — HierarchyPanel and
// SceneViewPanel both construct the command on the line after
// DuplicateEntitySubtreeRoots — and DeleteEntitiesCommand snapshots component
// bytes in its constructor. So the snapshot freezes the clone's PhysicsBody as
// this channel just left it: owning nothing. PhysicsInitSystem then gives the
// clone a real body a tick later, and Undo destroys the entity without freeing
// it. Redo replays "owns nothing" and a fresh body is built beside the stranded
// one.
//
// This fixture's world registers no removal hooks, so ReleaseSnapshotPhysicsBodies
// is the only thing standing between the clone's body and that leak — which is
// what this gate is for. RegisterPhysicsWorldHooks would also free it, and
// DestroyingADuplicateUnderTheRemovalHookSparesTheOriginalsBody covers that
// route; the command may not lean on it, since not every world has hooks.
//
// The ORDER below is the whole test. Provision the clone before committing the
// command and the snapshot names the live body instead, Redo re-adopts it, and
// the count balances on its own — which is why deleting an already-simulated
// entity does not leak today, and why this gate has to reproduce the editor's
// ordering rather than a convenient one.
TEST_F(PhysicsEntityDuplicateTests, DuplicateUndoRedoCyclesKeepTheBodyCountExact)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";
    const auto originalBodyHandle = m_World.GetComponent<PhysicsBody>(original)->body;

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    // Committed before any tick, exactly as Ctrl+D does it.
    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", &m_World, nullptr, std::vector<EntityHandle>{clone},
        std::function<void()>{}, std::function<void()>{}));

    RunInit();
    ASSERT_TRUE(HasLiveBody(clone)) << "the clone never got a body of its own to strand";

    // Two entities, two bodies. Anything else and the counts below are measuring
    // something other than what this test claims to measure.
    ASSERT_EQ(pw->GetBodyCount(), 2u) << "the fixture is not in the state this test assumes";

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        const auto* before = m_World.GetComponent<PhysicsBody>(clone);
        ASSERT_NE(before, nullptr) << "cycle " << cycle;
        const auto strandedBody = before->body;
        const auto strandedShape = before->shape;

        svc.Undo();
        ASSERT_FALSE(m_World.IsValid(clone)) << "cycle " << cycle << ": undo did not remove the clone";
        EXPECT_EQ(pw->GetBodyCount(), 1u)
            << "cycle " << cycle << ": undoing the duplicate left its body simulating";
        EXPECT_FALSE(pw->IsBodyValid(strandedBody))
            << "cycle " << cycle << ": the clone's body outlived the entity that owned it";
        EXPECT_FALSE(pw->IsShapeValid(strandedShape))
            << "cycle " << cycle << ": the clone's shape outlived the entity that owned it";

        svc.Redo();
        ASSERT_TRUE(m_World.IsValid(clone)) << "cycle " << cycle << ": redo did not restore the clone";
        RunInit();
        EXPECT_TRUE(HasLiveBody(clone)) << "cycle " << cycle << ": the restored clone was never re-provisioned";
        EXPECT_EQ(pw->GetBodyCount(), 2u)
            << "cycle " << cycle << ": redo minted a body without the previous one being freed";
    }

    // The original never participated. Without this, a fix that tears the whole
    // physics world down on undo satisfies every count above.
    EXPECT_TRUE(HasLiveBody(original)) << "the undo/redo cycles reached the ORIGINAL's body";
    const auto* originalAfterCycles = m_World.GetComponent<PhysicsBody>(original);
    ASSERT_NE(originalAfterCycles, nullptr) << "the cycles removed the ORIGINAL's PhysicsBody";
    EXPECT_EQ(originalAfterCycles->body.Value(), originalBodyHandle.Value())
        << "the original was re-provisioned by a cycle that should not have touched it";
}

// The same command, reached the way every other delete reaches it: the Delete key
// in the Scene View and the Hierarchy (both via DeleteEntitiesSelectionCommand,
// and both with a direct fallback when no undo service is wired), MCP
// delete_entity (with the same fallback), and SceneViewDropUndoCommand. None of
// them is a duplicate, and none was gated.
//
// The ordering is what makes this a different path rather than a restatement of
// the test above. Ctrl+D commits its undo entry before the clone is provisioned,
// so the snapshot already says "owns nothing" and only the freeing half of
// ReleaseSnapshotPhysicsBodies does any work. A plain delete snapshots a LIVE
// body, so the scrubbing half is what decides whether Undo revives an entity that
// owns nothing — and is re-provisioned — or one that names a body this command
// just freed.
//
// It shares the duplicate fixture because the subject is the same: a provisioned
// physics entity, and the one command that deletes it.
TEST_F(PhysicsEntityDuplicateTests, DeletingAProvisionedEntityFreesItsBodyAndUndoRebuildsIt)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle entity = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();

    // Precondition, not decoration: this path exists only when the snapshot is
    // taken over a LIVE body. Build the command before provisioning and it
    // degenerates into the duplicate ordering the test above already covers.
    ASSERT_TRUE(HasLiveBody(entity)) << "the fixture never gave the entity a body";
    ASSERT_EQ(pw->GetBodyCount(), 1u) << "the fixture is not in the state this test assumes";

    const auto* beforeDelete = m_World.GetComponent<PhysicsBody>(entity);
    ASSERT_NE(beforeDelete, nullptr);
    const auto deletedBody = beforeDelete->body;
    const auto deletedShape = beforeDelete->shape;
    const float massBefore = beforeDelete->mass;
    const float gravityScaleBefore = beforeDelete->gravityScale;

    DeleteEntitiesCommand cmd("Delete Entity", &m_World, nullptr, {entity});
    cmd.Do();

    ASSERT_FALSE(m_World.IsValid(entity)) << "the delete did not remove the entity";
    EXPECT_EQ(pw->GetBodyCount(), 0u) << "the deleted entity left its body simulating";
    EXPECT_FALSE(pw->IsBodyValid(deletedBody)) << "the body outlived the entity that owned it";
    EXPECT_FALSE(pw->IsShapeValid(deletedShape)) << "the shape outlived the entity that owned it";

    cmd.Undo();
    ASSERT_TRUE(m_World.IsValid(entity)) << "undo did not revive the entity";

    // Restored bytes that still named the freed body would satisfy
    // PhysicsInitSystem's "already provisioned" test on a dead handle.
    const auto* revived = m_World.GetComponent<PhysicsBody>(entity);
    ASSERT_NE(revived, nullptr) << "undo did not restore the PhysicsBody component";
    EXPECT_FALSE(revived->initialized) << "the revived entity claims a body the delete freed";
    EXPECT_FALSE(revived->body.IsValid()) << "the revived entity names a freed body";
    EXPECT_FALSE(revived->shape.IsValid()) << "the revived entity names a freed shape";
    EXPECT_EQ(pw->GetBodyCount(), 0u) << "undo alone minted a body";

    // Everything from here down is the opposite failure, not the gate, and it is
    // worth being explicit about which is which. Measured against both halves of
    // ReleaseSnapshotPhysicsBodies removed in turn, the assertions above redden
    // and these do not: PhysicsInitSystem rebuilds from authoring data either
    // way, so "the entity ends up simulating" survives the defect. These pin the
    // repair that goes too far — one that frees the body but never re-provisions
    // it, or scrubs the authoring fields the rebuild reads.
    RunInit();
    EXPECT_TRUE(HasLiveBody(entity)) << "the revived entity was never re-provisioned";
    EXPECT_EQ(pw->GetBodyCount(), 1u) << "the rebuild left a second body behind";

    const auto* rebuilt = m_World.GetComponent<PhysicsBody>(entity);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_FLOAT_EQ(rebuilt->mass, massBefore) << "the scrub reset authoring data";
    EXPECT_FLOAT_EQ(rebuilt->gravityScale, gravityScaleBefore) << "the scrub reset authoring data";
}

// The writeback half specifically: with the copy holding the original's handle,
// PhysicsWritebackSystem poses the copy from the original's body every frame.
TEST_F(PhysicsEntityDuplicateTests, DuplicateIsPosedFromItsOwnBodyNotTheOriginals)
{
    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    // A duplicate the user drags off the original before it is provisioned.
    // Its body must be built HERE, and it must stay here.
    MoveTo(clone, Vector3(20.0f, 5.0f, 0.0f));

    RunInit();
    RunWriteback();

    EXPECT_NEAR(WorldPositionOf(clone).x, 20.0f, 1.0e-3f)
        << "the duplicate was posed from the ORIGINAL's body and snapped onto it";
    EXPECT_NEAR(WorldPositionOf(original).x, 0.0f, 1.0e-3f)
        << "the original moved when its duplicate did";
}

// childShapeCount is the only bound on childShapes[], so a copy that keeps the
// count keeps the original's child handles addressable. PhysicsInitSystem's
// rebuild path then walks that count and destroys every handle it finds before
// building the copy's own compound — and those handles are the ORIGINAL's.
//
// This needs a compound body, which is what a body with more than one collider
// gets. A single-collider body never populates childShapes[] at all, which is
// why the single-sphere fixture the other tests use cannot see this.
TEST_F(PhysicsEntityDuplicateTests, DuplicatingACompoundLeavesTheOriginalsChildShapesAlone)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    BoxColliderShape box{};
    box.halfExtentsX = 0.25f;
    box.halfExtentsY = 0.25f;
    box.halfExtentsZ = 0.25f;
    m_World.AddComponentImmediate(original, box);

    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";

    // Precondition, not decoration: with one collider the body is not a
    // compound, childShapes[] stays empty, and every assertion below passes on
    // broken code.
    const auto* originalBody = m_World.GetComponent<PhysicsBody>(original);
    ASSERT_NE(originalBody, nullptr);
    ASSERT_EQ(originalBody->childShapeCount, 2u) << "the fixture did not build a compound body";
    const auto originalChildA = originalBody->childShapes[0];
    const auto originalChildB = originalBody->childShapes[1];
    ASSERT_TRUE(pw->IsShapeValid(originalChildA));
    ASSERT_TRUE(pw->IsShapeValid(originalChildB));

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());
    EXPECT_EQ(m_World.GetComponent<PhysicsBody>(clone)->childShapeCount, 0u)
        << "the copy claims the original's compound children";

    // The rebuild that destroys them, if it can reach them.
    RunInit();

    EXPECT_TRUE(pw->IsShapeValid(originalChildA))
        << "provisioning the duplicate destroyed the ORIGINAL's first child shape";
    EXPECT_TRUE(pw->IsShapeValid(originalChildB))
        << "provisioning the duplicate destroyed the ORIGINAL's second child shape";

    const auto* originalAfter = m_World.GetComponent<PhysicsBody>(original);
    ASSERT_NE(originalAfter, nullptr);
    EXPECT_EQ(originalAfter->childShapeCount, 2u);
    EXPECT_EQ(originalAfter->childShapes[0].Value(), originalChildA.Value());
    EXPECT_EQ(originalAfter->childShapes[1].Value(), originalChildB.Value());

    // And the copy owns a compound of its own rather than none.
    const auto* cloneAfter = m_World.GetComponent<PhysicsBody>(clone);
    ASSERT_NE(cloneAfter, nullptr);
    ASSERT_EQ(cloneAfter->childShapeCount, 2u) << "the duplicate never built its own compound";
    EXPECT_NE(cloneAfter->childShapes[0].Value(), originalChildA.Value());
    EXPECT_NE(cloneAfter->childShapes[1].Value(), originalChildB.Value());
}

// prevPhysicsTransform is where the ORIGINAL's body was before the last step.
// Copied along with its valid flag, the duplicate's first writeback blends its
// own pose toward a position it was never at — the copy is drawn part-way back
// onto the thing it was duplicated from for as long as it takes the step system
// to write a snapshot of its own.
//
// Only visible while a step is in flight, which is what alpha < 1 means. The
// fixture pins alpha to 1 so the other tests read the raw body pose; this test
// is the one that must un-pin it.
TEST_F(PhysicsEntityDuplicateTests, DuplicateIsNotBlendedFromTheOriginalsPreviousPose)
{
    using GameEngine::PhysicsECS::PhysicsWorldService;
    auto* pw = PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";

    // What PhysicsStepSystem leaves on a body that moved: a snapshot of where it
    // was, well away from where it is.
    {
        auto* body = m_World.GetComponentForWrite<PhysicsBody>(original);
        ASSERT_NE(body, nullptr);
        body->prevPhysicsTransform = pw->GetBodyTransform(body->body);
        body->prevPhysicsTransform.position.x = -40.0f;
        body->hasPrevPhysicsTransform = true;
    }

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());
    MoveTo(clone, Vector3(20.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(clone)) << "the clone has no body of its own to be posed from";

    // Mid-step. Without this the interpolation branch is dead and the
    // assertion below cannot distinguish a cleared flag from a copied one.
    PhysicsWorldService::SetRenderInterpolationAlpha(0.5f);
    ASSERT_LT(PhysicsWorldService::GetRenderInterpolationAlpha(), 1.0f);

    // The clone's own body is at x=20, so the only route to any other x is the
    // interpolation branch reading a snapshot the clone did not take.
    const auto* cloneBody = m_World.GetComponent<PhysicsBody>(clone);
    ASSERT_NE(cloneBody, nullptr);
    ASSERT_NEAR(pw->GetBodyTransform(cloneBody->body).position.x, 20.0f, 1.0e-3f);

    RunWriteback();

    EXPECT_NEAR(WorldPositionOf(clone).x, 20.0f, 1.0e-3f)
        << "the duplicate was blended toward a pose the ORIGINAL's body was in";
}

// The other tests here reach a body through DeleteEntitiesCommand, which frees
// bodies by hand. RegisterPhysicsWorldHooks adds a second, blunter route: an
// OnRemove hook that destroys whatever the component's handles name, fired by
// any entity destruction including ~World. A copy that still named the source's
// body would therefore free it on a plain delete, with no undo command involved.
TEST_F(PhysicsEntityDuplicateTests, DestroyingADuplicateUnderTheRemovalHookSparesTheOriginalsBody)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(m_World);

    const EntityHandle original = CreatePhysicsEntity(Vector3(0.0f, 5.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveBody(original)) << "the fixture never gave the original a body";
    const auto originalBodyHandle = m_World.GetComponent<PhysicsBody>(original)->body;

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());
    RunInit();
    ASSERT_TRUE(HasLiveBody(clone)) << "the clone never got a body of its own";
    const auto cloneBodyHandle = m_World.GetComponent<PhysicsBody>(clone)->body;

    m_World.DestroyEntityImmediate(clone);

    // Instrument check first: if the hook were not registered, the clone's body
    // would simply leak and the assertion below would pass without the hook ever
    // having run — measuring nothing.
    EXPECT_FALSE(pw->IsBodyValid(cloneBodyHandle))
        << "the removal hook never fired, so this test cannot see what it claims to";

    EXPECT_TRUE(pw->IsBodyValid(originalBodyHandle))
        << "destroying the duplicate freed the ORIGINAL entity's body";
    EXPECT_TRUE(HasLiveBody(original));
}

// MorphTargetWeights.runtimeModelGuid / runtimeMeshGpuHandleId name a mesh
// MorphTargetSystem baked from ONE entity's weights, registered under a GUID
// derived from that entity's id. Copied verbatim, the copy reads the source's
// entry as its own stale one and unregisters it on its first update —
// MeshGPURegistry is not reference counted, so that one call destroys it.
//
// SCOPE: this gate pins the channel's output, which is the input to the guard
// that fires the unregister. It does NOT drive MorphTargetSystem — that needs a
// MeshGPURegistry and therefore an IDevice, which EditorTests has no business
// creating. The registry consequence is established by reading
// MorphTargetSystem.cpp, not by this test.
class MorphEntityDuplicateTests : public ::testing::Test
{
protected:
    static void FillGuidBytes(std::uint8_t (&bytes)[16], std::uint8_t seed)
    {
        for (std::size_t i = 0; i < 16; ++i)
            bytes[i] = static_cast<std::uint8_t>(seed + i);
    }

    static bool GuidBytesAreZero(const std::uint8_t (&bytes)[16])
    {
        for (std::size_t i = 0; i < 16; ++i)
        {
            if (bytes[i] != 0)
                return false;
        }
        return true;
    }

    // A character with a blendshape dialled in, after MorphTargetSystem has
    // baked and registered a mesh for it.
    EntityHandle CreateMorphingEntity()
    {
        const EntityHandle e = CreateNamedTransformEntity(m_World, "Morphing", Vector3(1.0f, 0.0f, 0.0f));
        m_World.ProcessCommands();

        MeshRenderer renderer{};
        renderer.meshId = 3;
        m_World.AddComponentImmediate(e, renderer);

        MorphTargetWeights morph{};
        morph.weightCount = 2;
        morph.weights[0] = 0.75f;
        morph.weights[1] = 0.25f;
        morph.version = 5;
        morph.appliedVersion = 5;
        morph.sourceMeshId = 3;
        morph.sourceMeshGpuHandleId = 0x1111u;
        FillGuidBytes(morph.sourceModelGuid, 0xA0);
        // What MorphTargetSystem writes once it owns a baked runtime mesh.
        FillGuidBytes(morph.runtimeModelGuid, 0xB0);
        morph.runtimeMeshGpuHandleId = 0x2222u;
        m_World.AddComponentImmediate(e, morph);
        return e;
    }

    World m_World;
};

TEST_F(MorphEntityDuplicateTests, DuplicateCarriesNoRuntimeMorphEntry)
{
    const EntityHandle original = CreateMorphingEntity();

    const auto* beforePtr = m_World.GetComponent<MorphTargetWeights>(original);
    ASSERT_NE(beforePtr, nullptr);
    // Read out by value: duplicating adds components and can move the source.
    const MorphTargetWeights before = *beforePtr;

    // Precondition: the defect needs the source to actually own a baked entry.
    ASSERT_FALSE(GuidBytesAreZero(before.runtimeModelGuid))
        << "the fixture never gave the source a runtime morph entry";
    ASSERT_NE(before.runtimeMeshGpuHandleId, 0u);

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    const auto* cloned = m_World.GetComponent<MorphTargetWeights>(clone);
    ASSERT_NE(cloned, nullptr);
    EXPECT_TRUE(GuidBytesAreZero(cloned->runtimeModelGuid))
        << "the duplicate names the source's baked morph mesh and will unregister it";
    EXPECT_EQ(cloned->runtimeMeshGpuHandleId, 0u)
        << "the duplicate names the source's morph mesh GPU handle";

    // The source is untouched — it keeps the entry it owns.
    const auto* originalAfter = m_World.GetComponent<MorphTargetWeights>(original);
    ASSERT_NE(originalAfter, nullptr);
    EXPECT_EQ(std::memcmp(originalAfter->runtimeModelGuid, before.runtimeModelGuid,
                          sizeof(before.runtimeModelGuid)),
              0)
        << "repairing the duplicate rewrote the SOURCE's runtime entry";
    EXPECT_EQ(originalAfter->runtimeMeshGpuHandleId, before.runtimeMeshGpuHandleId);
}

TEST_F(MorphEntityDuplicateTests, DuplicateKeepsItsWeightsAndSharedSourceMesh)
{
    const EntityHandle original = CreateMorphingEntity();
    const MorphTargetWeights before = *m_World.GetComponent<MorphTargetWeights>(original);

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    const auto* cloned = m_World.GetComponent<MorphTargetWeights>(clone);
    ASSERT_NE(cloned, nullptr);

    // The dialled-in expression travels: clearing the runtime entry must not
    // reset the copy to neutral.
    EXPECT_TRUE(Entity(&m_World, clone).IsEnabled<MorphTargetWeights>());
    EXPECT_EQ(cloned->weightCount, before.weightCount);
    EXPECT_FLOAT_EQ(cloned->weights[0], before.weights[0]);
    EXPECT_FLOAT_EQ(cloned->weights[1], before.weights[1]);

    // sourceMesh* name the shared model submesh every instance registers and no
    // instance unregisters — not runtime state the copy has to give up.
    EXPECT_EQ(cloned->sourceMeshId, before.sourceMeshId);
    EXPECT_EQ(cloned->sourceMeshGpuHandleId, before.sourceMeshGpuHandleId);
    EXPECT_EQ(std::memcmp(cloned->sourceModelGuid, before.sourceModelGuid,
                          sizeof(before.sourceModelGuid)),
              0);
}

// MeshGPUData.instanceIndex names a GPUScene instance slot. The engine frees it
// through RegisterOnRemove<MeshGPUData> (Engine/Source/Core/Engine.cpp,
// ReleaseMeshGpuInstance) when the entity is destroyed, and GPUScene reissues
// freed slots LIFO (GPUScene::AllocateInstanceSlot pops the free list's back),
// so the slot a delete frees is the very next one a spawn is handed. The hook
// only ever sees the LIVE component; DeleteEntitiesCommand's snapshot bytes are
// what Undo replays, and extraction's one ownership test is
// instanceIndex != 0xFFFFFFFF (RenderExtractionSystem's Op::Add gate) — a
// replayed stale index is treated as owned and drives a slot that by then
// belongs to someone else.
//
// SCOPE: these gates pin the command's restore contract at the component level.
// They do NOT drive RenderExtractionSystem or a real GPUScene — those need an
// IDevice, which EditorTests has no business creating. The slot lifecycle is
// mirrored the way SkinnedEntityDuplicateTests mirrors the SkeletonRef hook: a
// captureless OnRemove hook over a LIFO free list reproducing
// AllocateInstanceSlot's reuse order, with the LIFO premise asserted in-test
// rather than assumed.
class MeshGpuDataEntityDeleteTests : public ::testing::Test
{
protected:
    static constexpr std::uint32_t kUnassigned = 0xFFFFFFFFu;

    // GPUScene's slot lifecycle where these tests need it: fresh slots count
    // up, freed slots are reissued LIFO (GPUScene.cpp, AllocateInstanceSlot /
    // FreeInstanceSlot). FreeCount is the audit trail: every destroy of an
    // assigned bridge must free exactly one slot, and nothing else may.
    struct SlotAllocator
    {
        std::uint32_t NextFresh = 0;
        std::vector<std::uint32_t> FreeSlots;
        std::uint32_t FreeCount = 0;

        std::uint32_t Allocate()
        {
            if (!FreeSlots.empty())
            {
                const std::uint32_t slot = FreeSlots.back();
                FreeSlots.pop_back();
                return slot;
            }
            return NextFresh++;
        }

        void Free(std::uint32_t slot)
        {
            FreeSlots.push_back(slot);
            ++FreeCount;
        }
    };

    void SetUp() override
    {
        s_Slots = &m_Slots;
        // Mirrors the engine's hook (Engine/Source/Core/Engine.cpp,
        // ReleaseMeshGpuInstance): destroying the entity frees the LIVE
        // component's slot and sentinels it. RegisterOnRemove takes a plain
        // function pointer, hence the fixture-static allocator. The null guard
        // matters at fixture teardown: ~World fires the hook for entities still
        // alive, and m_Slots (declared before m_World, so destroyed after)
        // must be the only state it touches.
        m_World.RegisterOnRemove<MeshGPUData>([](MeshGPUData& gpu) {
            if (gpu.instanceIndex == kUnassigned)
                return;
            if (s_Slots)
                s_Slots->Free(gpu.instanceIndex);
            gpu.instanceIndex = kUnassigned;
        });
    }

    EntityHandle CreateRenderableEntity(const char* name)
    {
        const EntityHandle e = CreateNamedTransformEntity(m_World, name, Vector3(0.0f, 0.0f, 0.0f));
        m_World.ProcessCommands();
        MeshRenderer renderer{};
        renderer.meshId = 3;
        m_World.AddComponentImmediate(e, renderer);
        m_World.AddComponentImmediate(e, MeshGPUData{});
        return e;
    }

    // Extraction's Op::Add apply, reduced to the slot handoff: only an
    // unassigned bridge is given a slot; any other value is treated as owned
    // (RenderExtractionSystem's instanceIndex == 0xFFFFFFFF test).
    std::uint32_t ExtractOnce(EntityHandle e)
    {
        auto* gpu = m_World.GetComponentForWrite<MeshGPUData>(e);
        EXPECT_NE(gpu, nullptr);
        if (!gpu)
            return kUnassigned;
        if (gpu->instanceIndex == kUnassigned)
            gpu->instanceIndex = m_Slots.Allocate();
        return gpu->instanceIndex;
    }

    SlotAllocator m_Slots; // outlives m_World: ~World fires the hook above
    World m_World;
    static SlotAllocator* s_Slots;
};

MeshGpuDataEntityDeleteTests::SlotAllocator* MeshGpuDataEntityDeleteTests::s_Slots = nullptr;

// The user-visible defect: delete an extracted entity, let anything spawn into
// the scene, undo — the revived entity comes back naming the slot the delete
// freed, which the spawn now owns. Two entities, one GPU instance: whichever
// extracts last drives it, and the other's edits land on the wrong mesh.
//
// The ORDER is the delete ordering: the snapshot is taken over a LIVE bridge.
// (The duplicate ordering snapshots a clone whose bridge the copy channel
// already reset, and is pinned by the cycles test below.)
TEST_F(MeshGpuDataEntityDeleteTests, UndoRevivesAnUnassignedBridgeNotTheFreedSlot)
{
    const EntityHandle entity = CreateRenderableEntity("Extracted");
    const std::uint32_t deletedSlot = ExtractOnce(entity);
    ASSERT_NE(deletedSlot, kUnassigned);

    // Junk in the paired fields proves the restore is the full default, not a
    // lone index scrub that leaves half a stale bridge behind.
    {
        auto* gpu = m_World.GetComponentForWrite<MeshGPUData>(entity);
        ASSERT_NE(gpu, nullptr);
        gpu->meshIndex = 7u;
        gpu->materialIndex = 9u;
        gpu->LastTransformVersion = 42u;
        gpu->hlodEvicted = true;
    }

    DeleteEntitiesCommand cmd("Delete Entity", &m_World, nullptr, {entity});
    cmd.Do();

    ASSERT_FALSE(m_World.IsValid(entity)) << "the delete did not remove the entity";
    ASSERT_EQ(m_Slots.FreeCount, 1u) << "the destroy hook did not free the slot";

    // The LIFO premise, asserted rather than assumed: the next spawn is handed
    // exactly the slot the delete freed.
    const EntityHandle intruder = CreateRenderableEntity("Intruder");
    const std::uint32_t intruderSlot = ExtractOnce(intruder);
    ASSERT_EQ(intruderSlot, deletedSlot)
        << "the fixture no longer reproduces GPUScene's LIFO reuse";

    cmd.Undo();
    ASSERT_TRUE(m_World.IsValid(entity)) << "undo did not revive the entity";

    const auto* revived = m_World.GetComponent<MeshGPUData>(entity);
    ASSERT_NE(revived, nullptr) << "undo did not restore the MeshGPUData component";
    EXPECT_EQ(revived->instanceIndex, kUnassigned)
        << "the revived entity names the freed slot, which the intruder now owns";
    EXPECT_EQ(revived->meshIndex, kUnassigned) << "half the bridge survived the scrub";
    EXPECT_EQ(revived->materialIndex, kUnassigned) << "half the bridge survived the scrub";
    EXPECT_EQ(revived->LastTransformVersion, kUnassigned)
        << "a stale skip-gate cache survived the scrub";
    EXPECT_FALSE(revived->hlodEvicted)
        << "a preserved eviction flag can strand the revived entity invisible";

    // Extraction re-provisions: the revived entity gets a slot of its own.
    const std::uint32_t revivedSlot = ExtractOnce(entity);
    EXPECT_NE(revivedSlot, kUnassigned) << "the revived entity was never re-provisioned";
    EXPECT_NE(revivedSlot, intruderSlot) << "two entities drive one GPU instance slot";
    EXPECT_EQ(m_Slots.FreeCount, 1u) << "reviving freed something";
}

// Undo/redo/undo again: every destroy frees exactly one slot, every revive
// restores an unassigned bridge, and re-extraction rebalances the books. The
// slot INDEX may repeat across cycles — with no intruder the free list hands
// the same one back — but only ever through a free/reallocate pair, which is
// what FreeCount and the free-list size pin. Unfixed, the revived bridge keeps
// its index without reclaiming it from the free list, and the next redo frees
// it a second time — the double-free the size assertion catches.
TEST_F(MeshGpuDataEntityDeleteTests, DeleteUndoRedoCyclesKeepSlotAccountingExact)
{
    const EntityHandle entity = CreateRenderableEntity("Extracted");
    ASSERT_NE(ExtractOnce(entity), kUnassigned);

    UndoRedoService svc;
    svc.Execute(std::make_unique<DeleteEntitiesCommand>("Delete Entity", &m_World, nullptr,
                                                        std::vector<EntityHandle>{entity}));

    std::uint32_t expectedFrees = 1u;
    ASSERT_EQ(m_Slots.FreeCount, expectedFrees) << "the delete did not free the slot";

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        svc.Undo();
        ASSERT_TRUE(m_World.IsValid(entity)) << "cycle " << cycle << ": undo did not revive";

        const auto* revived = m_World.GetComponent<MeshGPUData>(entity);
        ASSERT_NE(revived, nullptr) << "cycle " << cycle;
        ASSERT_EQ(revived->instanceIndex, kUnassigned)
            << "cycle " << cycle << ": undo revived a slot a previous cycle freed";
        EXPECT_EQ(m_Slots.FreeCount, expectedFrees)
            << "cycle " << cycle << ": undo alone changed the books";

        ASSERT_NE(ExtractOnce(entity), kUnassigned) << "cycle " << cycle;
        EXPECT_TRUE(m_Slots.FreeSlots.empty())
            << "cycle " << cycle << ": re-extraction left a freed slot unclaimed";

        svc.Redo();
        ASSERT_FALSE(m_World.IsValid(entity)) << "cycle " << cycle << ": redo did not delete";
        ++expectedFrees;
        EXPECT_EQ(m_Slots.FreeCount, expectedFrees)
            << "cycle " << cycle << ": redo freed more or fewer than the one slot";
        EXPECT_EQ(m_Slots.FreeSlots.size(), 1u)
            << "cycle " << cycle << ": the same slot was freed twice";
    }
}

// The duplicate ordering: Ctrl+D commits its undo entry before the clone is
// ever extracted, so the snapshot already holds the unassigned default and the
// working half is the destroy hook. What this pins is the delegation —
// DuplicateEntitiesCommand's Undo must free the slot extraction gave the clone
// AFTER the commit, and its Redo must revive a bridge extraction re-provisions
// — cycle after cycle, with the books exact and the original untouched.
TEST_F(MeshGpuDataEntityDeleteTests, DuplicateUndoRedoCyclesKeepSlotAccountingExact)
{
    const EntityHandle original = CreateRenderableEntity("Original");
    const std::uint32_t originalSlot = ExtractOnce(original);
    ASSERT_NE(originalSlot, kUnassigned);

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    {
        const auto* cloneGpu = m_World.GetComponent<MeshGPUData>(clone);
        ASSERT_NE(cloneGpu, nullptr);
        ASSERT_EQ(cloneGpu->instanceIndex, kUnassigned)
            << "the clone arrived owning the original's slot — the copy channel regressed";
    }

    // Committed before the clone is extracted, exactly as Ctrl+D does it.
    UndoRedoService svc;
    svc.CommitAlreadyApplied(std::make_unique<DuplicateEntitiesCommand>(
        "Duplicate Entity", &m_World, nullptr, std::vector<EntityHandle>{clone},
        std::function<void()>{}, std::function<void()>{}));

    ASSERT_NE(ExtractOnce(clone), kUnassigned);

    std::uint32_t expectedFrees = 0u;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        svc.Undo();
        ASSERT_FALSE(m_World.IsValid(clone)) << "cycle " << cycle << ": undo did not remove the clone";
        ++expectedFrees;
        EXPECT_EQ(m_Slots.FreeCount, expectedFrees)
            << "cycle " << cycle << ": undoing the duplicate left its slot claimed";

        svc.Redo();
        ASSERT_TRUE(m_World.IsValid(clone)) << "cycle " << cycle << ": redo did not restore the clone";
        const auto* revived = m_World.GetComponent<MeshGPUData>(clone);
        ASSERT_NE(revived, nullptr) << "cycle " << cycle;
        ASSERT_EQ(revived->instanceIndex, kUnassigned)
            << "cycle " << cycle << ": redo revived a freed slot";
        ASSERT_NE(ExtractOnce(clone), kUnassigned) << "cycle " << cycle;
        EXPECT_TRUE(m_Slots.FreeSlots.empty())
            << "cycle " << cycle << ": re-extraction left a freed slot unclaimed";
    }

    // The original never participated.
    const auto* originalGpu = m_World.GetComponent<MeshGPUData>(original);
    ASSERT_NE(originalGpu, nullptr);
    EXPECT_EQ(originalGpu->instanceIndex, originalSlot) << "the cycles reached the ORIGINAL's slot";
    EXPECT_EQ(m_Slots.FreeCount, expectedFrees) << "something other than the clone's destroys freed a slot";
}

class CharacterEntityDuplicateTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        using GameEngine::PhysicsECS::PhysicsWorldService;
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();

        GameEngine::Physics::PhysicsWorldSettings settings{};
        settings.gravity = GameEngine::Physics::Vector3(0.0f, -9.81f, 0.0f);
        settings.numThreads = 1;
        settings.maxBodies = 1024;
        settings.maxBodyPairs = 1024;
        settings.maxContactConstraints = 1024;
        settings.tempAllocatorBytes = 16u * 1024u * 1024u;
        PhysicsWorldService::Initialize(settings);
    }

    void TearDown() override
    {
        using GameEngine::PhysicsECS::PhysicsWorldService;
        if (PhysicsWorldService::IsInitialized())
            PhysicsWorldService::Shutdown();
    }

    EntityHandle CreateCharacterEntity(const Vector3& position)
    {
        const EntityHandle e = CreateNamedTransformEntity(m_World, "Character", position);
        m_World.ProcessCommands();

        WorldTransform wt{};
        const Transform local =
            Transform::FromTRS(position, Quaternion(1.0f, 0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
        std::memcpy(wt.matrix, local.matrix, sizeof(wt.matrix));
        m_World.AddComponentImmediate(e, wt);

        CharacterController cc{};
        cc.mass = 70.0f;
        m_World.AddComponentImmediate(e, cc);
        return e;
    }

    void RunInit() { m_Init.Update(m_World, 1.0f / 60.0f); }

    bool HasLiveCharacter(EntityHandle e) const
    {
        auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
        const auto* cc = m_World.GetComponent<CharacterController>(e);
        return pw && cc && cc->initialized && pw->IsCharacterValid(cc->character);
    }

    World m_World;
    GameEngine::PhysicsECS::CharacterControllerSystem m_Init;
};

TEST_F(CharacterEntityDuplicateTests, DuplicateArrivesOwningNoCharacter)
{
    const EntityHandle original = CreateCharacterEntity(Vector3(0.0f, 2.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveCharacter(original)) << "the fixture never gave the original a character";
    const auto originalHandle = m_World.GetComponent<CharacterController>(original)->character;

    const EntityHandle clone = DuplicateOnce(m_World, original);
    ASSERT_TRUE(clone.IsValid());

    const auto* cc = m_World.GetComponent<CharacterController>(clone);
    ASSERT_NE(cc, nullptr);
    EXPECT_FALSE(cc->initialized) << "the copy claims to be provisioned";
    EXPECT_FALSE(cc->character.IsValid()) << "the copy names a character handle";

    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);
    EXPECT_TRUE(pw->IsCharacterValid(originalHandle))
        << "repair destroyed the ORIGINAL's character instead of clearing the copy";
}

TEST_F(CharacterEntityDuplicateTests, DestroyingADuplicateUnderTheRemovalHookSparesTheOriginal)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);
    GameEngine::PhysicsECS::RegisterPhysicsWorldHooks(m_World);

    const EntityHandle original = CreateCharacterEntity(Vector3(0.0f, 2.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveCharacter(original));
    const auto originalHandle = m_World.GetComponent<CharacterController>(original)->character;

    const EntityHandle clone = DuplicateOnce(m_World, original);
    RunInit();
    ASSERT_TRUE(HasLiveCharacter(clone));
    const auto cloneHandle = m_World.GetComponent<CharacterController>(clone)->character;
    ASSERT_NE(cloneHandle.Value(), originalHandle.Value());

    m_World.DestroyEntityImmediate(clone);

    EXPECT_FALSE(pw->IsCharacterValid(cloneHandle))
        << "the removal hook never fired, so this test cannot see what it claims to";
    EXPECT_TRUE(pw->IsCharacterValid(originalHandle))
        << "destroying the duplicate freed the ORIGINAL entity's character";
}

TEST_F(CharacterEntityDuplicateTests, DeletingAProvisionedEntityFreesItsCharacterAndUndoRebuildsIt)
{
    auto* pw = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
    ASSERT_NE(pw, nullptr);

    const EntityHandle entity = CreateCharacterEntity(Vector3(0.0f, 2.0f, 0.0f));
    RunInit();
    ASSERT_TRUE(HasLiveCharacter(entity));

    const auto* beforeDelete = m_World.GetComponent<CharacterController>(entity);
    ASSERT_NE(beforeDelete, nullptr);
    const auto deletedHandle = beforeDelete->character;
    const float massBefore = beforeDelete->mass;

    DeleteEntitiesCommand cmd("Delete Entity", &m_World, nullptr, {entity});
    cmd.Do();

    ASSERT_FALSE(m_World.IsValid(entity));
    EXPECT_FALSE(pw->IsCharacterValid(deletedHandle))
        << "the character outlived the entity that owned it";

    cmd.Undo();
    ASSERT_TRUE(m_World.IsValid(entity));

    const auto* revived = m_World.GetComponent<CharacterController>(entity);
    ASSERT_NE(revived, nullptr);
    EXPECT_FALSE(revived->initialized) << "the revived entity claims a character the delete freed";
    EXPECT_FALSE(revived->character.IsValid()) << "the revived entity names a freed character";

    RunInit();
    EXPECT_TRUE(HasLiveCharacter(entity));
    const auto* rebuilt = m_World.GetComponent<CharacterController>(entity);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_FLOAT_EQ(rebuilt->mass, massBefore) << "the scrub reset authoring data";
}

TEST(AnimatorGraphDuplicateTests, DuplicateDoesNotShareGraphStoreSlot)
{
    using GameEngine::Animation::AnimationEventCollectorStore;
    using GameEngine::Animation::AnimationGraphPlayer;
    using GameEngine::Animation::AnimationGraphStore;
    auto& store = AnimationGraphStore::Instance();
    store.ClearForTest();
    const uint64_t graphId = store.Create(std::make_unique<AnimationGraphPlayer>());
    ASSERT_NE(graphId, 0u);
    auto& collectors = AnimationEventCollectorStore::Instance();
    const uint64_t collectorId = collectors.Create();
    ASSERT_NE(collectorId, 0u);

    World world;
    GameEngine::Engine::Renderer::RegisterRenderWorldHooks(world);
    const EntityHandle original = CreateNamedTransformEntity(world, "Anim", Vector3{});
    world.ProcessCommands();
    Animator animator{};
    animator.source = GameEngine::Components::AnimatorPlaybackSource::Graph;
    animator.graphRuntimeId = graphId;
    animator.eventCollectorId = collectorId;
    world.AddComponentImmediate(original, animator);

    const EntityHandle clone = DuplicateOnce(world, original);
    ASSERT_TRUE(clone.IsValid());
    const auto* copied = world.GetComponent<Animator>(clone);
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(copied->graphRuntimeId, 0u);
    EXPECT_TRUE(copied->graphInstanceGuid.IsNull());
    EXPECT_EQ(copied->eventCollectorId, 0u);
    EXPECT_NE(store.Get(graphId), nullptr);

    world.DestroyEntityImmediate(clone);
    EXPECT_NE(store.Get(graphId), nullptr);
    EXPECT_NE(collectors.Get(collectorId), nullptr) << "the copy's removal left the source's collector";
    store.ClearForTest();
    collectors.ClearForTest();
}

} // namespace

// A model-backed instance carries its durable rig source (model GUID plus the
// instance owner) beside the runtime caches. Duplicating it must yield a
// SECOND group: the clone's entities name the cloned owner, their repaired
// runtime carries that identity, and the shared resolver keeps that runtime
// instead of regrouping the clone under the original or orphaning the repair.
class ModelBackedSkinnedDuplicateTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& store = SkeletonStore::Instance();
        m_SkelId = store.CreateSkeleton(3);
        auto* skel = store.Get(m_SkelId);
        ASSERT_NE(skel, nullptr);
        skel->SkinJointCount = 3;
        skel->JointNodes = {0, 1, 2};

        m_Model = GameEngine::MakeShared<GameEngine::ModelAsset>(m_ModelGuid, "rig.fbx");
        m_Model->SetMeshesForTest({});
        m_Model->SetSkeletonIdForTest(m_SkelId);
        m_Assets.RegisterLoadedAsset(m_ModelGuid, m_Model);

        m_World.RegisterOnRemove<SkeletonRef>(&GameEngine::Engine::Renderer::ReleaseSkeletonRuntime);
    }

    // ModelEntityFactory's spawn shape after resolution: a container root
    // owning the group, a submesh and a helper node naming it.
    void SpawnResolvedInstance()
    {
        m_Root = CreateNamedTransformEntity(m_World, "Skinned", Vector3(1.0f, 2.0f, 3.0f));
        m_Child = CreateNamedTransformEntity(m_World, "SkinnedChild", Vector3(0.0f, 1.0f, 0.0f));
        m_Helper = CreateNamedTransformEntity(m_World, "SkinnedSocket", Vector3(0.0f, 2.0f, 0.0f));
        m_World.ProcessCommands();
        for (EntityHandle e : {m_Child, m_Helper})
        {
            m_World.AddComponentImmediate(e, Parent{m_Root});
            SkeletonRef ref{};
            ref.sourceModelGuid.Set(m_ModelGuid);
            ref.ownerMode = GameEngine::Components::SkeletonInstanceOwner::Entity;
            ref.instanceOwner = m_Root;
            m_World.AddComponentImmediate(e, ref);
        }
        GameEngine::Engine::Renderer::ResolveSkinnedMeshAnimation(m_World, m_Assets);
        m_RuntimeId = RuntimeOf(m_Child);
    }

    std::vector<EntityHandle> DuplicateInstance()
    {
        const auto duplicate = GameEngine::Editor::DuplicateEntitySubtreeRoots(m_World, {m_Root});
        EXPECT_EQ(duplicate.NewRoots.size(), 1u);
        if (duplicate.NewRoots.empty())
            return {};
        m_CloneRoot = duplicate.NewRoots.front();
        return DeleteEntitiesCommand::CollectSubtree(m_World, m_CloneRoot);
    }

    std::uint32_t RuntimeOf(EntityHandle e) const
    {
        const auto* ref = m_World.GetComponent<SkeletonRef>(e);
        return ref ? ref->runtimeId : 0u;
    }

    World m_World;
    GameEngine::AssetManager m_Assets;
    GameEngine::GUID m_ModelGuid = GameEngine::GUID::Generate();
    GameEngine::SharedPtr<GameEngine::ModelAsset> m_Model;
    std::uint32_t m_SkelId = 0;
    std::uint32_t m_RuntimeId = 0;
    EntityHandle m_Root{};
    EntityHandle m_Child{};
    EntityHandle m_Helper{};
    EntityHandle m_CloneRoot{};
};

TEST_F(ModelBackedSkinnedDuplicateTests, DuplicateOwnsItsOwnGroupThroughTheSharedResolver)
{
    auto& store = SkeletonStore::Instance();

    SpawnResolvedInstance();
    ASSERT_NE(m_RuntimeId, 0u);
    ASSERT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u);

    const auto clones = DuplicateInstance();
    ASSERT_EQ(clones.size(), 3u);
    std::vector<EntityHandle> cloneParts;
    for (EntityHandle e : clones)
        if (m_World.GetComponent<SkeletonRef>(e))
            cloneParts.push_back(e);
    ASSERT_EQ(cloneParts.size(), 2u);

    const std::uint32_t repaired = RuntimeOf(cloneParts[0]);
    ASSERT_NE(repaired, 0u) << "the clone lost its skeleton binding entirely";
    ASSERT_NE(repaired, m_RuntimeId);
    for (EntityHandle e : cloneParts)
    {
        const auto* ref = m_World.GetComponent<SkeletonRef>(e);
        EXPECT_EQ(ref->instanceOwner, m_CloneRoot)
            << "a copied part still names the ORIGINAL owner; the resolver will regroup it there"
            << " (owner=" << ref->instanceOwner.id << " original=" << m_Root.id << " clone=" << m_CloneRoot.id << ")";
        EXPECT_EQ(ref->runtimeGeneration, store.GetRuntimeGeneration(repaired))
            << "the copied generation names the original's allocation, not the repaired one";
    }

    // The frame after a duplicate, the shared binder resolves the changed refs.
    GameEngine::Engine::Renderer::ResolveSkinnedMeshAnimation(m_World, m_Assets);

    const std::uint32_t cloneRuntime = RuntimeOf(cloneParts[0]);
    ASSERT_NE(cloneRuntime, 0u);
    EXPECT_NE(cloneRuntime, m_RuntimeId)
        << "the clone shares the original's runtime — one pose for two instances";
    EXPECT_EQ(RuntimeOf(cloneParts[1]), cloneRuntime)
        << "the clone's own entities must share ONE runtime, as the original's do";
    EXPECT_EQ(cloneRuntime, repaired)
        << "the resolver discarded the repaired runtime and allocated another (clone=" << cloneRuntime
        << " original=" << m_RuntimeId << " repaired=" << repaired << ")";
    EXPECT_EQ(store.GetRuntimeRefCount(m_RuntimeId), 2u)
        << "duplicating changed the ORIGINAL instance's reference count";
    EXPECT_EQ(store.GetRuntimeRefCount(cloneRuntime), 2u)
        << "the clone's two SkeletonRefs must hold exactly two references";
    if (cloneRuntime != repaired)
        EXPECT_EQ(store.GetRuntime(repaired), nullptr)
            << "the repaired runtime is orphaned: resident forever, named by nobody";
}
