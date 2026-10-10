// Skeleton-runtime ownership across the ECS: one model instance is several
// entities (submeshes, imported bone nodes) whose SkeletonRefs all name the
// same SkeletonStore runtime, while the release hook is per-component. These
// pin the reference-counting rule documented in SkeletonStore.h:
//
//   every live SkeletonRef with runtimeId != 0 holds exactly one reference.
//
// Without it, deleting one entity of a multi-entity model frees a runtime its
// siblings still point at, and the free list hands that slot to the next
// spawn — the surviving entities then animate off a stranger's palette.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderWorldHooks.h"
#include "Rendering/Core/Device.h"

#include "TestDeviceHelper.h"

#include <vector>

using GameEngine::Components::SkeletonRef;
using GameEngine::Components::SkinnedMeshRenderer;
using GameEngine::Engine::Renderer::SkeletonStore;

namespace
{

// The engine registers this hook on its primary world (Engine.cpp) and the
// editor on every thumbnail world; a bare test world starts with none, so the
// tests wire the same release policy they are pinning.
void RegisterSkeletonReleaseHook(GameEngine::ECS::World& world)
{
    world.RegisterOnRemove<SkeletonRef>(&GameEngine::Engine::Renderer::ReleaseSkeletonRuntime);
}

class SkeletonRuntimeOwnershipTest : public ::testing::Test
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
    }

    // The ModelEntityFactory spawn shape: one runtime, N entities naming it.
    // The factory's construction reference is released once the components
    // that hold their own references exist.
    std::vector<GameEngine::ECS::EntityHandle> SpawnInstance(GameEngine::ECS::World& world,
                                                            uint32_t entityCount,
                                                            uint32_t& outRuntimeId)
    {
        auto& store = SkeletonStore::Instance();
        outRuntimeId = store.CreateRuntime(m_SkelId);

        std::vector<GameEngine::ECS::EntityHandle> entities;
        for (uint32_t i = 0; i < entityCount; ++i)
        {
            SkeletonRef ref{};
            ref.skeletonId = m_SkelId;
            ref.runtimeId = outRuntimeId;
            entities.push_back(world.Create<SkeletonRef>(ref).GetHandle());
            store.RetainRuntime(outRuntimeId);
        }

        store.ReleaseRuntime(outRuntimeId); // construction reference
        return entities;
    }

    uint32_t m_SkelId = 0;
};

} // namespace

// Defect 1, first half: deleting ONE entity of a multi-entity model must not
// free the runtime its siblings still name.
TEST_F(SkeletonRuntimeOwnershipTest, DeletingOneSiblingKeepsSharedRuntimeAlive)
{
    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    uint32_t runtimeId = 0;
    auto entities = SpawnInstance(world, 3, runtimeId);
    ASSERT_NE(runtimeId, 0u);
    ASSERT_EQ(store.GetRuntimeRefCount(runtimeId), 3u);

    world.DestroyEntityImmediate(entities[1]);

    EXPECT_NE(store.GetRuntime(runtimeId), nullptr)
        << "deleting one submesh freed the runtime its two siblings still hold";
    EXPECT_EQ(store.GetRuntimeSkeletonId(runtimeId), m_SkelId);
    EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 2u);

    world.DestroyEntityImmediate(entities[0]);
    world.DestroyEntityImmediate(entities[2]);

    EXPECT_EQ(store.GetRuntime(runtimeId), nullptr)
        << "the last SkeletonRef went away and the runtime was not freed";
}

// Defect 1, second half — the visible symptom. After one sibling is deleted,
// the next spawn must not be handed the runtime the survivors are animating
// off, or they adopt its pose.
TEST_F(SkeletonRuntimeOwnershipTest, NextSpawnDoesNotReceiveASurvivingSiblingsRuntime)
{
    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    uint32_t sharedRuntime = 0;
    auto entities = SpawnInstance(world, 2, sharedRuntime);
    ASSERT_NE(sharedRuntime, 0u);

    // Give the shared runtime a distinguishable pose, as an animating
    // character would have.
    auto* pose = store.GetRuntime(sharedRuntime);
    ASSERT_NE(pose, nullptr);
    pose->CompactSkinMatrices[12] = 7.5f; // translation.x of joint 0

    world.DestroyEntityImmediate(entities[0]);

    uint32_t secondRuntime = 0;
    auto second = SpawnInstance(world, 2, secondRuntime);
    EXPECT_NE(secondRuntime, sharedRuntime)
        << "the next spawn was handed the surviving sibling's runtime";

    auto* survivorPose = store.GetRuntime(sharedRuntime);
    ASSERT_NE(survivorPose, nullptr);
    EXPECT_FLOAT_EQ(survivorPose->CompactSkinMatrices[12], 7.5f)
        << "the surviving entity's pose was reset by a foreign spawn";

    world.DestroyEntityImmediate(entities[1]);
    for (auto e : second)
        world.DestroyEntityImmediate(e);
    EXPECT_EQ(store.GetRuntime(sharedRuntime), nullptr);
    EXPECT_EQ(store.GetRuntime(secondRuntime), nullptr);
}

// Two independent instances must not interfere: deleting all of instance A
// leaves instance B's runtime alone.
TEST_F(SkeletonRuntimeOwnershipTest, InstancesReleaseIndependently)
{
    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    uint32_t runtimeA = 0, runtimeB = 0;
    auto a = SpawnInstance(world, 2, runtimeA);
    auto b = SpawnInstance(world, 2, runtimeB);
    ASSERT_NE(runtimeA, runtimeB);

    for (auto e : a)
        world.DestroyEntityImmediate(e);

    EXPECT_EQ(store.GetRuntime(runtimeA), nullptr);
    EXPECT_NE(store.GetRuntime(runtimeB), nullptr) << "freeing instance A took instance B with it";
    EXPECT_EQ(store.GetRuntimeRefCount(runtimeB), 2u);

    for (auto e : b)
        world.DestroyEntityImmediate(e);
    EXPECT_EQ(store.GetRuntime(runtimeB), nullptr);
}

// Defect 2: ResolveSkinnedMeshAnimation re-keys a SkeletonRef whose skeletonId
// no longer matches. The write is a plain value update (no OnRemove hook), so
// the old runtime is only reclaimed if the re-key releases it explicitly.
// No model source, so the SkinnedMeshRenderer's skeletonId stands.
TEST_F(SkeletonRuntimeOwnershipTest, ResolveSkinnedMeshAnimation_ReKeyReleasesTheOldRuntime)
{
    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    const uint32_t otherSkelId = store.CreateSkeleton(3);
    auto* otherSkel = store.Get(otherSkelId);
    ASSERT_NE(otherSkel, nullptr);
    otherSkel->SkinJointCount = 3;
    otherSkel->JointNodes = {0, 1, 2};

    const uint32_t staleRuntime = store.CreateRuntime(m_SkelId);
    ASSERT_NE(staleRuntime, 0u);

    SkinnedMeshRenderer smr{};
    smr.skeletonId = otherSkelId; // disagrees with the SkeletonRef below
    SkeletonRef ref{};
    ref.skeletonId = m_SkelId;
    ref.runtimeId = staleRuntime;
    auto entity = world.Create<SkinnedMeshRenderer, SkeletonRef>(smr, ref).GetHandle();

    GameEngine::AssetManager assets;
    GameEngine::Engine::Renderer::ResolveSkinnedMeshAnimation(world, assets);

    const auto* updated = world.GetComponent<SkeletonRef>(entity);
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->skeletonId, otherSkelId);
    ASSERT_NE(updated->runtimeId, 0u);
    ASSERT_NE(updated->runtimeId, staleRuntime);

    EXPECT_EQ(store.GetRuntime(staleRuntime), nullptr)
        << "the re-keyed runtime was orphaned: resident forever, uploading nothing";
    EXPECT_EQ(store.GetRuntimeRefCount(updated->runtimeId), 1u);

    const auto updatedRuntime = updated->runtimeId;
    world.DestroyEntityImmediate(entity);
    EXPECT_EQ(store.GetRuntime(updatedRuntime), nullptr);
}

// The same ownership rule through the REAL spawn path. ModelEntityFactory
// allocates one runtime per model instance and writes it into a SkeletonRef on
// every skinned submesh, so a two-submesh model must leave two references
// behind — and deleting one submesh must not free the other's runtime.
// Device-gated: the factory registers GPU meshes through RenderServices.
TEST_F(SkeletonRuntimeOwnershipTest, ModelEntityFactory_TakesOneReferencePerSkeletonRef)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    GameEngine::Engine::Renderer::RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    GameEngine::ModelAsset model(GameEngine::GUID::Generate(), "skinned.fbx");
    model.SetMeshesForTest([] {
        GameEngine::Vector<GameEngine::Mesh> v;
        for (const char* name : {"SM_Body", "SM_Head"})
        {
            GameEngine::Mesh m;
            m.Name = name;
            m.MaterialIndex = 0;
            m.Vertices.resize(3);
            m.Indices = {0u, 1u, 2u};
            m.Skinned = true;
            m.Joints0.resize(m.Vertices.size() * 4);
            m.Weights0.resize(m.Vertices.size() * 4);
            v.push_back(std::move(m));
        }
        return v;
    }());
    model.SetSkeletonIdForTest(m_SkelId);

    auto result = GameEngine::Engine::Renderer::ModelEntityFactory::CreateFromModel(
        rs, world, model, GameEngine::GUID::Generate(), "Skinned");

    ASSERT_EQ(result.submeshEntities.size(), 2u);

    // One runtime, shared by both skinned submeshes.
    uint32_t runtimeId = 0;
    for (auto e : result.submeshEntities)
    {
        const auto* ref = world.GetComponent<SkeletonRef>(e);
        ASSERT_NE(ref, nullptr) << "a skinned submesh spawned without a SkeletonRef";
        ASSERT_NE(ref->runtimeId, 0u);
        if (runtimeId == 0)
            runtimeId = ref->runtimeId;
        EXPECT_EQ(ref->runtimeId, runtimeId) << "submeshes of one instance must share one runtime";
        EXPECT_EQ(ref->runtimeGeneration, store.GetRuntimeGeneration(runtimeId));
        EXPECT_EQ(ref->instanceOwner, result.rootEntity);
        EXPECT_EQ(ref->ownerMode, GameEngine::Components::SkeletonInstanceOwner::Entity);
        EXPECT_FALSE(ref->sourceModelGuid.IsNull());
    }

    EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 2u)
        << "two SkeletonRefs name this runtime, so it must carry two references "
           "(the factory's construction reference must also be gone)";

    world.DestroyEntityImmediate(result.submeshEntities[0]);
    EXPECT_NE(store.GetRuntime(runtimeId), nullptr)
        << "deleting one submesh freed the runtime the other submesh still animates from";
    EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 1u);

    world.DestroyEntityImmediate(result.submeshEntities[1]);
    EXPECT_EQ(store.GetRuntime(runtimeId), nullptr)
        << "the last SkeletonRef went away and the runtime leaked";

    device->Shutdown();
}

// The runtimeId == 0 repair branch of the same site must not release anything
// (there is nothing to release) and must leave exactly one reference behind.
TEST_F(SkeletonRuntimeOwnershipTest, ResolveSkinnedMeshAnimation_FillsMissingRuntimeWithOneReference)
{
    GameEngine::ECS::World world;
    RegisterSkeletonReleaseHook(world);
    auto& store = SkeletonStore::Instance();

    SkinnedMeshRenderer smr{};
    smr.skeletonId = m_SkelId;
    SkeletonRef ref{};
    ref.skeletonId = m_SkelId;
    ref.runtimeId = 0; // never resolved
    auto entity = world.Create<SkinnedMeshRenderer, SkeletonRef>(smr, ref).GetHandle();

    GameEngine::AssetManager assets;
    GameEngine::Engine::Renderer::ResolveSkinnedMeshAnimation(world, assets);

    const auto* updated = world.GetComponent<SkeletonRef>(entity);
    ASSERT_NE(updated, nullptr);
    ASSERT_NE(updated->runtimeId, 0u);
    EXPECT_EQ(store.GetRuntimeRefCount(updated->runtimeId), 1u);

    const auto updatedRuntime = updated->runtimeId;
    world.DestroyEntityImmediate(entity);
    EXPECT_EQ(store.GetRuntime(updatedRuntime), nullptr);
}
