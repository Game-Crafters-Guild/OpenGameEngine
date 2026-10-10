#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECSTemplates.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/SkeletonResolveState.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Scene/SceneIO.h"

#include "EngineLogCapture.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Engine::Renderer;

namespace
{
class ModelSkeletonPersistence : public testing::Test
{
  protected:
    void SetUp() override
    {
        Directory = std::filesystem::temp_directory_path() /
            ("model-skeleton-" + GUID::Generate().ToString());
        std::filesystem::create_directories(Directory);
        Path = Directory / "roundtrip.scene";
        Model = MakeShared<ModelAsset>(ModelGuid, Directory / "rig.fbx");
        Model->SetMeshesForTest({});
        Model->SetSkeletonIdForTest(NewSkeleton());
        Assets.RegisterLoadedAsset(ModelGuid, Model);
    }
    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(Directory, ignored);
    }
    uint32 NewSkeleton()
    {
        auto& store = SkeletonStore::Instance();
        const uint32 id = store.CreateSkeleton(3);
        store.Get(id)->SkinJointCount = 3;
        store.Get(id)->JointNodes = {0, 1, 2};
        return id;
    }
    void Hooks(ECS::World& world)
    {
        world.RegisterOnRemove<SkeletonRef>(&ReleaseSkeletonRuntime);
    }
    ECS::EntityHandle Entity(ECS::World& world, const char* tag)
    {
        SceneEntityTag value{};
        std::strncpy(value.value, tag, sizeof(value.value) - 1);
        return world.Create<SceneEntityTag>(value).GetHandle();
    }
    ECS::EntityHandle Find(ECS::World& world, const char* tag)
    {
        ECS::EntityHandle result;
        world.Query<ECS::Read<SceneEntityTag>>().Each([&](auto h, const auto& value)
        {
            const std::string_view text = value.View();
            if (text == tag || text.ends_with("." + std::string(tag))) result = h;
        });
        return result;
    }
    ECS::EntityHandle Part(ECS::World& world, const char* tag, ECS::EntityHandle owner, bool helper = false)
    {
        const auto entity = Entity(world, tag);
        SkeletonRef ref{};
        ref.sourceModelGuid.Set(ModelGuid);
        ref.ownerMode = SkeletonInstanceOwner::Entity;
        ref.instanceOwner = owner;
        world.AddComponentImmediate(entity, ref);
        Parent parent{};
        parent.parent = owner;
        world.AddComponentImmediate(entity, parent);
        if (helper)
            world.AddComponentImmediate(entity, AnimatedNodeRef{1});
        else
        {
            MeshRenderer mesh{};
            mesh.modelAssetGuid.Set(ModelGuid);
            world.AddComponentImmediate(entity, mesh);
            world.AddComponentImmediate(entity, SkinnedMeshRenderer{});
        }
        return entity;
    }
    std::string Read() const
    {
        std::ifstream file(Path);
        return {std::istreambuf_iterator<char>(file), {}};
    }
    void Write(const std::string& text)
    {
        std::ofstream file(Path);
        file << text;
    }
    void Load(ECS::World& world, Scene::LoadMode mode = Scene::LoadMode::Replace)
    {
        Scene::SceneLoadDegradation degradation;
        Scene::LoadOptions options{mode};
        options.outDegradation = &degradation;
        ASSERT_TRUE(Scene::LoadSceneFromFile(world, Path, options));
        EXPECT_TRUE(degradation.skips.empty());
    }
    AssetManager Assets;
    // The binder's resolve service: idle unless a test hands it an entity.
    SceneResolveService Resolves;
    GUID ModelGuid = GUID::Generate();
    SharedPtr<ModelAsset> Model;
    std::filesystem::path Directory, Path;
};
} // namespace

TEST_F(ModelSkeletonPersistence, SceneOmitsNumericCachesAndLegacyValuesAreIgnored)
{
    ECS::World world;
    const auto owner = Entity(world, "owner");
    const auto part = Part(world, "part", owner);
    auto ref = *world.GetComponent<SkeletonRef>(part);
    ref.skeletonId = 123456;
    ref.runtimeId = 654321;
    ref.runtimeGeneration = 123;
    world.AddComponentImmediate(part, ref);
    auto skin = *world.GetComponent<SkinnedMeshRenderer>(part);
    skin.skeletonId = 123456;
    world.AddComponentImmediate(part, skin);
    ASSERT_TRUE(Scene::SaveSceneToFile(world, Path));
    const auto text = Read();
    EXPECT_EQ(text.find("skeletonId ="), std::string::npos);
    EXPECT_EQ(text.find("runtimeId ="), std::string::npos);
    EXPECT_EQ(text.find("runtimeGeneration ="), std::string::npos);
    EXPECT_NE(text.find("SkeletonRef.sourceModelGuid"), std::string::npos);
    Write(text + "\n[entity id=\"legacy\"]\nSkeletonRef.skeletonId = 123456\n"
                 "SkeletonRef.runtimeId = 654321\nSkinnedMeshRenderer.skeletonId = 123456\n");
    ECS::World restored;
    Load(restored);
    const auto legacy = Find(restored, "legacy");
    ASSERT_TRUE(legacy.IsValid());
    const auto* loadedRef = restored.GetComponent<SkeletonRef>(legacy);
    ASSERT_NE(loadedRef, nullptr);
    EXPECT_EQ(loadedRef->skeletonId, 0u);
    EXPECT_EQ(loadedRef->runtimeId, 0u);
    EXPECT_EQ(restored.GetComponent<SkinnedMeshRenderer>(legacy)->skeletonId, 0u);
}

TEST_F(ModelSkeletonPersistence, AdditiveLoadRemapsGroupAndRebuildsFromCurrentModelIndex)
{
    auto& store = SkeletonStore::Instance();
    ECS::World original;
    Hooks(original);
    const auto root = Entity(original, "root");
    const auto a = Part(original, "body", root);
    const auto b = Part(original, "head", root);
    const auto helper = Part(original, "socket", root, true);
    ResolveSkinnedMeshAnimation(original, Assets);
    const auto old = *original.GetComponent<SkeletonRef>(a);
    ASSERT_NE(old.runtimeId, 0u);
    EXPECT_EQ(original.GetComponent<SkeletonRef>(b)->runtimeId, old.runtimeId);
    EXPECT_EQ(original.GetComponent<SkeletonRef>(helper)->runtimeId, old.runtimeId);
    EXPECT_EQ(store.GetRuntimeRefCount(old.runtimeId), 3u);
    ASSERT_TRUE(Scene::SaveSceneToFile(original, Path));
    original.Clear();
    EXPECT_EQ(store.GetRuntime(old.runtimeId), nullptr);
    Model->SetSkeletonIdForTest(NewSkeleton()); // Different import/store order after restart.

    ECS::World restored;
    Hooks(restored);
    for (int i = 0; i < 9; ++i) (void)Entity(restored, ("existing" + std::to_string(i)).c_str());
    Load(restored, Scene::LoadMode::Additive);
    const auto newRoot = Find(restored, "root");
    ASSERT_TRUE(newRoot.IsValid());
    ASSERT_NE(newRoot, root);
    for (const char* name : {"body", "head", "socket"})
    {
        const auto entity = Find(restored, name);
        ASSERT_TRUE(entity.IsValid());
        const auto* ref = restored.GetComponent<SkeletonRef>(entity);
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->instanceOwner, newRoot);
        EXPECT_EQ(ref->ownerMode, SkeletonInstanceOwner::Entity);
        EXPECT_EQ(ref->runtimeId, 0u);
    }
    ResolveSkinnedMeshAnimation(restored, Assets);
    const auto live = *restored.GetComponent<SkeletonRef>(Find(restored, "body"));
    EXPECT_EQ(live.skeletonId, Model->GetSkeletonId());
    EXPECT_NE(live.skeletonId, old.skeletonId);
    ASSERT_NE(live.runtimeId, 0u);
    EXPECT_EQ(store.GetRuntimeRefCount(live.runtimeId), 3u);
    EXPECT_EQ(restored.GetComponent<SkeletonRef>(Find(restored, "socket"))->runtimeId, live.runtimeId);
    EXPECT_EQ(store.GetRuntime(live.runtimeId)->Source.InstanceOwner, newRoot);
    EXPECT_EQ(store.GetRuntime(live.runtimeId)->Source.WorldId, restored.GetWorldId());
    restored.DestroyEntityImmediate(Find(restored, "head"));
    EXPECT_EQ(store.GetRuntimeRefCount(live.runtimeId), 2u);
    ResolveSkinnedMeshAnimation(restored, Assets);
    EXPECT_EQ(store.GetRuntimeRefCount(live.runtimeId), 2u);
    restored.Clear();
    EXPECT_EQ(store.GetRuntime(live.runtimeId), nullptr);
}

TEST_F(ModelSkeletonPersistence, MissingSharedOwnerStaysUnresolvedThroughResave)
{
    ECS::World original;
    const auto root = Entity(original, "root");
    Part(original, "orphan", root, true);
    original.DestroyEntityImmediate(root);
    ASSERT_TRUE(Scene::SaveSceneToFile(original, Path));
    EXPECT_NE(Read().find("SkeletonRef.ownerMode = Entity"), std::string::npos);
    ECS::World restored;
    Hooks(restored);
    Load(restored);
    const auto orphan = Find(restored, "orphan");
    ResolveSkinnedMeshAnimation(restored, Assets);
    const auto* ref = restored.GetComponent<SkeletonRef>(orphan);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->ownerMode, SkeletonInstanceOwner::Entity);
    EXPECT_FALSE(ref->instanceOwner.IsValid());
    EXPECT_EQ(ref->runtimeId, 0u);
    ASSERT_TRUE(Scene::SaveSceneToFile(restored, Path));
    Load(restored);
    ResolveSkinnedMeshAnimation(restored, Assets);
    EXPECT_EQ(restored.GetComponent<SkeletonRef>(Find(restored, "orphan"))->runtimeId, 0u);
}

TEST_F(ModelSkeletonPersistence, StaleSameSlotRemovalCannotReleaseAnotherActor)
{
    auto& store = SkeletonStore::Instance();
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "old-owner");
    const auto a = Part(world, "old-part", owner);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto stale = *world.GetComponent<SkeletonRef>(a);
    world.DestroyEntityImmediate(a);
    const auto nextOwner = Entity(world, "new-owner");
    const auto b = Part(world, "new-part", nextOwner);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto live = *world.GetComponent<SkeletonRef>(b);
    ASSERT_EQ(live.runtimeId, stale.runtimeId);
    ASSERT_NE(live.runtimeGeneration, stale.runtimeGeneration);
    store.GetRuntime(live.runtimeId)->CompactSkinMatrices[12] = 7.5f;
    const auto copied = world.Create<SkeletonRef>(stale).GetHandle();
    world.DestroyEntityImmediate(copied); // Removal BEFORE the resolver can repair it.
    EXPECT_EQ(store.GetRuntimeRefCount(live.runtimeId), 1u);
    ASSERT_NE(store.GetRuntime(live.runtimeId), nullptr);
    EXPECT_FLOAT_EQ(store.GetRuntime(live.runtimeId)->CompactSkinMatrices[12], 7.5f);
}

TEST_F(ModelSkeletonPersistence, OwnerEditRegroupsWithoutLeakingThePreviousRuntime)
{
    auto& store = SkeletonStore::Instance();
    ECS::World world;
    Hooks(world);
    const auto ownerA = Entity(world, "a");
    const auto ownerB = Entity(world, "b");
    const auto a = Part(world, "a-part", ownerA);
    const auto b = Part(world, "b-part", ownerB);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto old = *world.GetComponent<SkeletonRef>(a);
    const auto other = *world.GetComponent<SkeletonRef>(b);
    ASSERT_NE(old.runtimeId, other.runtimeId);
    auto updated = old;
    updated.instanceOwner = ownerB;
    world.AddComponentImmediate(a, updated);
    ResolveSkinnedMeshAnimation(world, Assets);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(a)->runtimeId, other.runtimeId);
    EXPECT_EQ(store.GetRuntime(old.runtimeId), nullptr);
    EXPECT_EQ(store.GetRuntimeRefCount(other.runtimeId), 2u);
}

TEST_F(ModelSkeletonPersistence, RawResetCannotBorrowOrReleaseAnotherWorldsReissuedRuntime)
{
    auto& store = SkeletonStore::Instance();
    ECS::World original;
    Hooks(original);
    const auto owner = Entity(original, "owner");
    const auto part = Part(original, "part", owner);
    ResolveSkinnedMeshAnimation(original, Assets);
    const auto old = *original.GetComponent<SkeletonRef>(part);
    const auto snapshot = original.SerializeWorld();
    const auto generation = original.GetLifecycleResetGeneration();
    original.Clear();

    ECS::World other;
    Hooks(other);
    const auto otherOwner = Entity(other, "other-owner");
    const auto otherPart = Part(other, "other-part", otherOwner);
    ResolveSkinnedMeshAnimation(other, Assets);
    const auto foreign = *other.GetComponent<SkeletonRef>(otherPart);
    ASSERT_EQ(foreign.runtimeId, old.runtimeId);
    store.GetRuntime(foreign.runtimeId)->CompactSkinMatrices[12] = 9.0f;

    original.DeserializeWorld(snapshot);
    ASSERT_GT(original.GetLifecycleResetGeneration(), generation);
    ResolveSkinnedMeshAnimation(original, Assets);
    const auto restored = *original.GetComponent<SkeletonRef>(part);
    ASSERT_NE(restored.runtimeId, 0u);
    EXPECT_NE(restored.runtimeId, foreign.runtimeId);
    EXPECT_EQ(store.GetRuntime(restored.runtimeId)->Source.WorldId, original.GetWorldId());
    EXPECT_EQ(store.GetRuntime(restored.runtimeId)->Source.WorldGeneration, original.GetLifecycleResetGeneration());
    EXPECT_EQ(store.GetRuntimeRefCount(foreign.runtimeId), 1u);
    EXPECT_FLOAT_EQ(store.GetRuntime(foreign.runtimeId)->CompactSkinMatrices[12], 9.0f);
}

TEST_F(ModelSkeletonPersistence, SelfOwnedActorsWithOneModelKeepIndependentRuntimes)
{
    auto& store = SkeletonStore::Instance();
    ECS::World world;
    Hooks(world);
    SkeletonRef ref{};
    ref.sourceModelGuid.Set(ModelGuid);
    const auto a = world.Create<SkeletonRef>(ref).GetHandle();
    const auto b = world.Create<SkeletonRef>(ref).GetHandle();
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto first = *world.GetComponent<SkeletonRef>(a);
    const auto second = *world.GetComponent<SkeletonRef>(b);
    ASSERT_NE(first.runtimeId, 0u);
    EXPECT_NE(first.runtimeId, second.runtimeId);
    EXPECT_EQ(store.GetRuntime(first.runtimeId)->Source.InstanceOwner, a);
    EXPECT_EQ(store.GetRuntime(second.runtimeId)->Source.InstanceOwner, b);
    ResolveSkinnedMeshAnimation(world, Assets);
    EXPECT_EQ(store.GetRuntimeRefCount(first.runtimeId), 1u);
    EXPECT_EQ(store.GetRuntimeRefCount(second.runtimeId), 1u);
}

TEST_F(ModelSkeletonPersistence, UnloadedSourceClearsCachesAndReloadRestoresTheGroup)
{
    auto& store = SkeletonStore::Instance();
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "owner");
    const auto a = Part(world, "a", owner);
    const auto b = Part(world, "b", owner, true);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto old = *world.GetComponent<SkeletonRef>(a);
    Model->Unload(); // Retained unloaded asset must not fall back to cached IDs.
    ResolveSkinnedMeshAnimation(world, Assets);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(a)->runtimeId, 0u);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(b)->skeletonId, 0u);
    EXPECT_EQ(world.GetComponent<SkinnedMeshRenderer>(a)->skeletonId, 0u);
    EXPECT_EQ(store.GetRuntime(old.runtimeId), nullptr);
    Model->SetMeshesForTest({});
    Model->SetSkeletonIdForTest(NewSkeleton());
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto live = *world.GetComponent<SkeletonRef>(a);
    EXPECT_NE(live.runtimeId, 0u);
    EXPECT_EQ(live.skeletonId, Model->GetSkeletonId());
    EXPECT_EQ(world.GetComponent<SkeletonRef>(b)->runtimeId, live.runtimeId);
    EXPECT_EQ(store.GetRuntimeRefCount(live.runtimeId), 2u);
}

TEST_F(ModelSkeletonPersistence, FactoryHelpersCarryTheSharedDurableOwner)
{
    ECS::World world;
    Hooks(world);
    RenderServices services;
    ImportedSceneNodeData node{};
    node.Name = "Socket";
    node.SourceNodeIndex = 1;
    node.ParentSourceNodeIndex = -1;
    Model->SetSceneNodesForTest({node});
    const auto result = ModelEntityFactory::CreateFromModel(services, world, *Model, ModelGuid, "Rig");
    ASSERT_TRUE(result.rootEntity.IsValid());
    ECS::EntityHandle helper;
    world.Query<ECS::Read<AnimatedNodeRef>>().Each([&](auto h, const auto&) { helper = h; });
    ASSERT_TRUE(helper.IsValid());
    const auto before = *world.GetComponent<SkeletonRef>(helper);
    EXPECT_EQ(before.sourceModelGuid.ToGuid(), ModelGuid);
    EXPECT_EQ(before.ownerMode, SkeletonInstanceOwner::Entity);
    EXPECT_EQ(before.instanceOwner, result.rootEntity);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto after = *world.GetComponent<SkeletonRef>(helper);
    ASSERT_NE(after.runtimeId, 0u);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntime(after.runtimeId)->Source.InstanceOwner, result.rootEntity);
}

TEST_F(ModelSkeletonPersistence, NonResidentModelDoesNotUseNumericCacheButProceduralRigWorks)
{
    ECS::World world;
    AssetManager withoutModel;
    Hooks(world);
    SkinnedMeshRenderer skin{};
    skin.skeletonId = Model->GetSkeletonId();
    MeshRenderer mesh{};
    mesh.modelAssetGuid.Set(ModelGuid);
    const auto modelEntity = world.Create<SkinnedMeshRenderer, MeshRenderer>(skin, mesh).GetHandle();
    const auto procedural = world.Create<SkinnedMeshRenderer>(skin).GetHandle();
    ResolveSkinnedMeshAnimation(world, withoutModel);
    EXPECT_EQ(world.GetComponent<SkinnedMeshRenderer>(modelEntity)->skeletonId, 0u);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(modelEntity)->runtimeId, 0u);
    EXPECT_NE(world.GetComponent<SkeletonRef>(procedural)->runtimeId, 0u);
}

TEST_F(ModelSkeletonPersistence, ThrowingAddHookLeavesAnUnresolvedReferenceAndRetryWorks)
{
    ECS::World world;
    Hooks(world);
    MeshRenderer mesh{};
    mesh.modelAssetGuid.Set(ModelGuid);
    const auto entity = world.Create<SkinnedMeshRenderer, MeshRenderer>(SkinnedMeshRenderer{}, mesh).GetHandle();
    const auto hook = world.RegisterOnAdd<SkeletonRef>([](SkeletonRef&)
    { throw std::runtime_error("test add hook after publication"); });
    EXPECT_THROW(ResolveSkinnedMeshAnimation(world, Assets), std::runtime_error);
    const auto* published = world.GetComponent<SkeletonRef>(entity);
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(published->runtimeId, 0u);
    EXPECT_EQ(published->runtimeGeneration, 0u);
    EXPECT_EQ(published->sourceModelGuid.ToGuid(), ModelGuid);
    world.UnregisterComponentHook(hook);
    ResolveSkinnedMeshAnimation(world, Assets);
    const auto* repaired = world.GetComponent<SkeletonRef>(entity);
    ASSERT_NE(repaired, nullptr);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntimeRefCount(repaired->runtimeId), 1u);
    EXPECT_NE(world.GetComponent<AnimatorRef>(entity), nullptr);
}

TEST_F(ModelSkeletonPersistence, DirectSceneLoadHydratesThroughSharedBinderWithoutSteadyFrameWrites)
{
    ECS::World source;
    const auto owner = Entity(source, "owner");
    Part(source, "body", owner);
    Part(source, "helper", owner, true);
    ASSERT_TRUE(Scene::SaveSceneToFile(source, Path));
    SkeletonResolveState state;
    BindChangedSkinnedMeshAnimation(source, state, Resolves, Assets);
    Hooks(source);
    ECS::World restored;
    Hooks(restored);
    Load(restored);
    BindChangedSkinnedMeshAnimation(restored, state, Resolves, Assets);
    const auto body = Find(restored, "body");
    const auto helper = Find(restored, "helper");
    const auto live = *restored.GetComponent<SkeletonRef>(body);
    ASSERT_NE(live.runtimeId, 0u);
    EXPECT_EQ(restored.GetComponent<SkeletonRef>(helper)->runtimeId, live.runtimeId);
    const auto epoch = SkeletonStore::Instance().GetRuntimeCreateEpoch();
    const auto version = restored.GetGlobalSystemVersion();
    for (int i = 0; i < 3; ++i) BindChangedSkinnedMeshAnimation(restored, state, Resolves, Assets);
    EXPECT_EQ(restored.GetGlobalSystemVersion(), version);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntimeCreateEpoch(), epoch);
    restored.DestroyEntityImmediate(Find(restored, "owner"));
    BindChangedSkinnedMeshAnimation(restored, state, Resolves, Assets);
    EXPECT_EQ(restored.GetComponent<SkeletonRef>(body)->runtimeId, 0u);
    EXPECT_EQ(restored.GetComponent<SkeletonRef>(helper)->runtimeId, 0u);
}

TEST_F(ModelSkeletonPersistence, SharedBinderRetainsChangesUntilScenePumpFinishes)
{
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "owner");
    const auto part = Part(world, "part", owner);
    RenderServices services;
    SceneBuildPump pump;
    pump.Begin(world, services);
    ASSERT_TRUE(pump.IsActive());
    SkeletonResolveState state;
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(part)->runtimeId, 0u);
    EXPECT_EQ(state.WorldId, 0u);
    pump.Reset();
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    EXPECT_NE(world.GetComponent<SkeletonRef>(part)->runtimeId, 0u);
}

TEST_F(ModelSkeletonPersistence, SharedBinderObservesHelperSourceReadinessAndUnloadWithoutEcsWrites)
{
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "owner");
    const auto helper = Part(world, "helper", owner, true);
    Model->Unload();
    SkeletonResolveState state;
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(helper)->runtimeId, 0u);
    for (int i = 0; i < 3; ++i) BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    const auto version = world.GetGlobalSystemVersion();
    Model->SetMeshesForTest({});
    Model->SetSkeletonIdForTest(NewSkeleton());
    EXPECT_EQ(world.GetGlobalSystemVersion(), version);
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    const auto ready = *world.GetComponent<SkeletonRef>(helper);
    ASSERT_NE(ready.runtimeId, 0u);
    EXPECT_EQ(ready.skeletonId, Model->GetSkeletonId());
    for (int i = 0; i < 3; ++i) BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    Model->Unload();
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(helper)->runtimeId, 0u);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntime(ready.runtimeId), nullptr);
}

TEST_F(ModelSkeletonPersistence, MeshModelReassignmentChangesOnlyThatSourcesInstanceGroup)
{
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "owner");
    const auto mesh = Part(world, "mesh", owner);
    const auto helper = Part(world, "helper", owner, true);
    SkeletonResolveState state;
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    const auto previous = *world.GetComponent<SkeletonRef>(mesh);
    const auto otherGuid = GUID::Generate();
    auto otherModel = MakeShared<ModelAsset>(otherGuid, Directory / "other.fbx");
    otherModel->SetMeshesForTest({});
    otherModel->SetSkeletonIdForTest(NewSkeleton());
    Assets.RegisterLoadedAsset(otherGuid, otherModel);
    auto changedMesh = *world.GetComponent<MeshRenderer>(mesh);
    changedMesh.modelAssetGuid.Set(otherGuid);
    world.AddComponentImmediate(mesh, changedMesh);
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    const auto updated = *world.GetComponent<SkeletonRef>(mesh);
    EXPECT_EQ(updated.sourceModelGuid.ToGuid(), otherGuid);
    EXPECT_EQ(updated.skeletonId, otherModel->GetSkeletonId());
    EXPECT_NE(updated.runtimeId, previous.runtimeId);
    EXPECT_EQ(world.GetComponent<SkeletonRef>(helper)->runtimeId, previous.runtimeId);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntimeRefCount(previous.runtimeId), 1u);
    EXPECT_EQ(SkeletonStore::Instance().GetRuntimeRefCount(updated.runtimeId), 1u);
}

// A helper node saved before rig sources were durable carries only a numeric
// cache. Nothing can rebuild its rig, and the artist has to hear that once,
// with the fix, rather than watch the socket stop following the animation.
TEST_F(ModelSkeletonPersistence, SourcelessHelperIsRefusedLoudlyOncePerOwner)
{
    ECS::World world;
    Hooks(world);
    const auto owner = Entity(world, "owner");
    const auto socket = Entity(world, "legacy-socket");
    world.AddComponentImmediate(socket, Parent{owner});
    world.AddComponentImmediate(socket, AnimatedNodeRef{1});
    Name name{};
    std::strncpy(name.value, "Hand_Socket", sizeof(name.value) - 1);
    world.AddComponentImmediate(socket, name);
    SkeletonRef legacy{};
    legacy.skeletonId = 123456;
    world.AddComponentImmediate(socket, legacy);

    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Error);
    SkeletonResolveState state;
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    world.AddComponentImmediate(socket, legacy); // a second change must not report again
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Hand_Socket"), 1u);
    EXPECT_NE(TestLog::FirstLineContaining(lines, "Hand_Socket").find("sourceModelGuid"), std::string::npos)
        << "the refusal must name the fix";
    EXPECT_EQ(world.GetComponent<SkeletonRef>(socket)->runtimeId, 0u);

    auto repaired = *world.GetComponent<SkeletonRef>(socket);
    repaired.sourceModelGuid.Set(ModelGuid);
    repaired.ownerMode = SkeletonInstanceOwner::Entity;
    repaired.instanceOwner = owner;
    world.AddComponentImmediate(socket, repaired);
    BindChangedSkinnedMeshAnimation(world, state, Resolves, Assets);
    EXPECT_NE(world.GetComponent<SkeletonRef>(socket)->runtimeId, 0u);
    EXPECT_TRUE(state.ReportedSourceless.empty()) << "a resolved node leaves the reported set";

    ResolveSkinnedMeshAnimation(world, Assets);
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "Hand_Socket"), 1u);
}
