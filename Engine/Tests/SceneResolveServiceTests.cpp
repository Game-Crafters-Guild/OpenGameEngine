// SceneResolveService: the engine's persistent, non-waiting resolve. These pin
// its contract with its callers: the engine instance never turns on the editor's
// scene-build flag, a spawn made while the service is busy with a scene reaches
// its queue, and the Player's startup scene resolves through it without the
// first frame waiting, its skinned entities bound by the time the batch is done.
// And its contract as an object that outlives scenes, models and entities: a
// destroyed entity leaves the queue, a cleared world drops it, and a kept model
// registration is evicted when its model is reloaded or unloaded.

#include "HeldAssetImportFixture.h"

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "Engine/Rendering/SceneResolveService.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using GameEngine::Testing::kAFrameThatWaited;

namespace
{

class SceneResolveServiceTest : public Testing::HeldAssetImportFixture
{
  protected:
    ECS::EntityHandle Spawn(ECS::World& world, const GUID& modelGuid, const GUID& materialGuid = GUID::Null())
    {
        Components::MeshRenderer meshRenderer{};
        meshRenderer.modelAssetGuid.Set(modelGuid);
        if (!materialGuid.IsNull())
            meshRenderer.materialAssetGuid.Set(materialGuid);
        return world.Create<Components::MeshRenderer>(meshRenderer).GetHandle();
    }

    static uint64 MeshHandle(ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* meshRenderer = world.GetComponent<Components::MeshRenderer>(entity);
        return meshRenderer ? meshRenderer->meshGpuHandleId : 0;
    }

    static SceneResolveService& EngineResolves()
    {
        return EngineCore::GetInstance().GetRenderingLoop()->GetSceneResolveService();
    }
};

constexpr std::size_t kBusyBatchEntities = 5000;

} // namespace

// The engine's service holding a scene of thousands of entities whose model is
// still loading never sets the editor's flag, so a spawn made meanwhile reaches
// its queue; once the model lands the scene and the spawn bind over later frames
// and no frame waits.
TEST_F(SceneResolveServiceTest, ASpawnWhileTheServiceIsBusyReachesItsQueue)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();
    std::vector<ECS::EntityHandle> scene;
    for (std::size_t i = 0; i < kBusyBatchEntities; ++i)
        scene.push_back(Spawn(*world, modelGuid));
    const SceneResolveBatch batch = EngineResolves().EnqueueWorld(*world, m_Services);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_FALSE(EngineResolves().IsBatchComplete(batch)) << "the scene's model is still held at its import";
    EXPECT_FALSE(IsSceneBuildPumpActive()) << "the engine's service set the editor's scene-build flag";

    const ECS::EntityHandle spawn = Spawn(*world, modelGuid);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_TRUE(EngineResolves().IsResolving(*world, spawn)) << "the spawn made while the service was busy was dropped";
    EXPECT_EQ(EngineResolves().Counts().HeldEntities, kBusyBatchEntities + 1);

    m_Gate.Release();
    int frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((MeshHandle(*world, spawn) == 0 || !EngineResolves().IsBatchComplete(batch))
           && std::chrono::steady_clock::now() < deadline)
    {
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
        ++frames;
    }
    const SceneResolveBatchProgress progress = EngineResolves().BatchProgress(batch);
    EXPECT_NE(MeshHandle(*world, spawn), 0u) << "the spawn made while the service was busy never bound";
    EXPECT_TRUE(progress.Complete) << progress.Done << " of " << progress.Total << " done";
    EXPECT_NE(MeshHandle(*world, scene.back()), 0u);
    EXPECT_EQ(EngineResolves().Counts().ResolvedEntities, kBusyBatchEntities + 1);
    std::cout << "[SceneResolveService] " << kBusyBatchEntities + 1 << " entities of one landed model bound over "
              << frames << " frame(s) of " << RenderingLoop::kResolveFrameSlice.count() << " us each\n";
}

// Only the editor's scene-open build sets IsSceneBuildPumpActive: the engine's
// service holding entities whose model is still loading leaves it clear.
TEST_F(SceneResolveServiceTest, TheEngineServiceNeverSetsTheSceneBuildFlag)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    const ECS::EntityHandle held = Spawn(*world, modelGuid);
    StepFrame();
    ASSERT_TRUE(EngineResolves().IsResolving(*world, held)) << "the service holds the entity while its model loads";
    EXPECT_FALSE(IsSceneBuildPumpActive());

    ECS::World opened;
    Spawn(opened, modelGuid);
    SceneBuildPump build;
    build.Begin(opened, m_Services);
    EXPECT_TRUE(IsSceneBuildPumpActive()) << "the editor's build is the flag's setter";
    build.Reset();
    EXPECT_FALSE(IsSceneBuildPumpActive());
    EXPECT_TRUE(EngineResolves().IsResolving(*world, held)) << "the build's reset touched the engine's service";
}

// The Player's startup: the freshly loaded scene goes to the engine's service as
// one batch; the first frame returns with the models still loading; the batch
// completes on a later frame with every entity bound and its standalone material
// registered, and by then each skinned entity carries the SkeletonRef and
// AnimatorRef an autoplay animator start needs.
TEST_F(SceneResolveServiceTest, ThePlayersStartupSceneResolvesWithoutBlockingTheFirstFrame)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false, /*skinned=*/true);
    const GUID materialGuid = RegisterHeldMaterial(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull() || materialGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    const ECS::EntityHandle prop = Spawn(*world, modelGuid, materialGuid);
    const ECS::EntityHandle character = Spawn(*world, modelGuid);
    world->AddComponentImmediate(character, Components::SkinnedMeshRenderer{});
    const SceneResolveBatch startup = EngineResolves().EnqueueWorld(*world, m_Services);

    EXPECT_LT(StepFrame(), kAFrameThatWaited) << "the first frame waited for the startup scene";
    EXPECT_FALSE(EngineResolves().IsBatchComplete(startup));
    EXPECT_EQ(world->GetComponent<Components::SkeletonRef>(character), nullptr)
        << "the binder bound a skinned entity still waiting for its model";

    m_Gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!EngineResolves().IsBatchComplete(startup) && std::chrono::steady_clock::now() < deadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_TRUE(EngineResolves().IsBatchComplete(startup)) << "the startup batch never completed";
    EXPECT_NE(MeshHandle(*world, prop), 0u);
    EXPECT_NE(MeshHandle(*world, character), 0u);
    EXPECT_NE(m_Services.Materials().Registry().Find(materialGuid), nullptr)
        << "the startup scene's standalone material was not registered";
    const auto* skeleton = world->GetComponent<Components::SkeletonRef>(character);
    ASSERT_NE(skeleton, nullptr) << "the skinned entity was not bound when the batch completed";
    EXPECT_NE(skeleton->runtimeId, 0u);
    EXPECT_NE(world->GetComponent<Components::AnimatorRef>(character), nullptr);
    const SceneResolveCounts counts = EngineResolves().Counts();
    EXPECT_EQ(counts.ResolvedEntities, 2u);
    EXPECT_EQ(counts.MissedEntities, 0u);
    EXPECT_EQ(counts.RegisteredMaterials, 1u);
    EXPECT_EQ(counts.HeldEntities, 0u);
}

// A standalone material that fails to load is a miss the batch counts as done:
// the batch completes, the miss is counted, and nothing waits for it.
TEST_F(SceneResolveServiceTest, AMissingMaterialCompletesTheBatchAsAMiss)
{
    const GUID materialGuid = RegisterHeldMaterial(/*fails=*/true);
    ASSERT_FALSE(materialGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    Components::MeshRenderer meshRenderer{};
    meshRenderer.materialAssetGuid.Set(materialGuid);
    world->Create<Components::MeshRenderer>(meshRenderer);
    const SceneResolveBatch batch = EngineResolves().EnqueueWorld(*world, m_Services);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    m_Gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!EngineResolves().IsBatchComplete(batch) && std::chrono::steady_clock::now() < deadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_TRUE(EngineResolves().IsBatchComplete(batch));
    EXPECT_EQ(EngineResolves().Counts().MissedMaterials, 1u);
    EXPECT_EQ(EngineResolves().Counts().PendingMaterials, 0u);
}

// A held entity destroyed while its model is still loading is dropped from the
// queue by the next step, not when the model lands, and its batch counts it
// done; the entity held beside it still binds.
TEST_F(SceneResolveServiceTest, AnEntityDestroyedWhileQueuedIsDropped)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    const ECS::EntityHandle destroyed = Spawn(*world, modelGuid);
    const ECS::EntityHandle kept = Spawn(*world, modelGuid);
    const SceneResolveBatch batch = EngineResolves().EnqueueWorld(*world, m_Services);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_EQ(EngineResolves().Counts().HeldEntities, 2u);

    world->DestroyEntityImmediate(destroyed);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_FALSE(EngineResolves().IsResolving(*world, destroyed));
    EXPECT_EQ(EngineResolves().Counts().HeldEntities, 1u) << "the destroyed entity stayed queued";
    const SceneResolveBatchProgress progress = EngineResolves().BatchProgress(batch);
    EXPECT_EQ(progress.Done, 1u) << "the batch did not count the destroyed entity done";
    EXPECT_FALSE(progress.Complete);

    m_Gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!EngineResolves().IsBatchComplete(batch) && std::chrono::steady_clock::now() < deadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_TRUE(EngineResolves().IsBatchComplete(batch));
    EXPECT_NE(MeshHandle(*world, kept), 0u);
    EXPECT_EQ(EngineResolves().Counts().ResolvedEntities, 1u);
    EXPECT_EQ(EngineResolves().Counts().MissedEntities, 0u);
}

// The world cleared under the service (a synchronous scene switch) drops its
// queue and batches: no handle from the old world binds, or aliases an entity
// created after the clear, when the model lands.
TEST_F(SceneResolveServiceTest, TheQueueIsDroppedWhenTheWorldIsCleared)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    const ECS::EntityHandle old = Spawn(*world, modelGuid);
    const SceneResolveBatch batch = EngineResolves().EnqueueWorld(*world, m_Services);
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_TRUE(EngineResolves().IsResolving(*world, old));

    world->Clear();
    // Created after the clear: it may take the old entity's slot and version.
    const ECS::EntityHandle created = world->Create<Components::MeshRenderer>(Components::MeshRenderer{}).GetHandle();
    EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_EQ(EngineResolves().Counts().HeldEntities, 0u) << "the queue outlived the world's clear";
    EXPECT_FALSE(EngineResolves().IsResolving(*world, created));
    EXPECT_TRUE(EngineResolves().IsBatchComplete(batch)) << "a batch of the cleared world never completes";

    m_Gate.Release();
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!assets.GetAsset(modelGuid) && std::chrono::steady_clock::now() < deadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_NE(assets.GetAsset(modelGuid), nullptr) << "the model never landed";
    for (int frame = 0; frame < 3; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    const auto* meshRenderer = world->GetComponent<Components::MeshRenderer>(created);
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_TRUE(meshRenderer->modelAssetGuid.IsNull()) << "an old world's work was applied to a new entity";
    EXPECT_EQ(meshRenderer->meshGpuHandleId, 0u);
    EXPECT_EQ(EngineResolves().Counts().ResolvedEntities, 0u);
}

// The registration the service keeps for spawns of a model follows its asset: a
// reload drops it and the next spawn registers the model again (in its spawn
// frame, the model still resident); an unload makes it stale and the next spawn
// loads the model again and binds to live mesh handles, never the old ones.
TEST_F(SceneResolveServiceTest, AKeptModelIsEvictedWhenItIsReloadedOrUnloaded)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    m_Gate.Release();
    const ECS::EntityHandle first = Spawn(*world, modelGuid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (MeshHandle(*world, first) == 0 && std::chrono::steady_clock::now() < deadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    ASSERT_NE(MeshHandle(*world, first), 0u) << "the model never landed";
    ASSERT_EQ(EngineResolves().Counts().RegisteredModels, 1u);
    ASSERT_EQ(EngineResolves().Counts().KeptModels, 1u);

    const ECS::EntityHandle cached = Spawn(*world, modelGuid);
    StepFrame();
    EXPECT_NE(MeshHandle(*world, cached), 0u) << "a spawn of a kept model did not bind in its spawn frame";
    EXPECT_EQ(EngineResolves().Counts().RegisteredModels, 1u) << "a kept model was registered again";

    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    AssetMetadata metadata{};
    ASSERT_TRUE(Registry().TryGetAssetMetadata(modelGuid, metadata));
    assets.GetEventDispatcher().DispatchEvent(
        AssetEvents::AssetReloaded(modelGuid, AssetType::Model, metadata.Path.string()));
    StepFrame();
    EXPECT_EQ(EngineResolves().Counts().KeptModels, 0u) << "the reload did not evict the kept registration";
    const ECS::EntityHandle afterReload = Spawn(*world, modelGuid);
    StepFrame();
    EXPECT_NE(MeshHandle(*world, afterReload), 0u) << "a spawn of the reloaded, resident model waited a frame";
    EXPECT_EQ(EngineResolves().Counts().RegisteredModels, 2u) << "the reloaded model was not registered again";

    // Spawned as soon as the asset manager has let the model go. UnloadAssetAsync
    // raises no invalidation, and another owner still holds the old asset, so the
    // kept registration's weak reference locks and its mesh handles resolve: only
    // the asset manager's answer shows it stale. The spawn loads the model again.
    SharedPtr<Asset> heldByAnotherOwner = assets.GetAsset(modelGuid);
    ASSERT_NE(heldByAnotherOwner, nullptr);
    (void)assets.UnloadAssetAsync(modelGuid);
    const auto unloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (assets.GetAsset(modelGuid) && std::chrono::steady_clock::now() < unloadDeadline)
        std::this_thread::yield();
    ASSERT_EQ(assets.GetAsset(modelGuid), nullptr) << "the model was not unloaded";
    const ECS::EntityHandle afterUnload = Spawn(*world, modelGuid);
    const auto reloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (MeshHandle(*world, afterUnload) == 0 && std::chrono::steady_clock::now() < reloadDeadline)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    heldByAnotherOwner.reset();
    for (int frame = 0; frame < 3; ++frame)
        StepFrame();
    const uint64 handle = MeshHandle(*world, afterUnload);
    ASSERT_NE(handle, 0u) << "the spawn after the unload never bound";
    EXPECT_EQ(EngineResolves().Counts().RegisteredModels, 3u)
        << "the spawn after the unload was bound from the registration made before it";
    EXPECT_NE(m_Services.GetMeshGPURegistry().Find(static_cast<Rendering::MeshGPUHandle>(handle)), nullptr)
        << "the spawn after the unload was bound to a released mesh handle";
}
