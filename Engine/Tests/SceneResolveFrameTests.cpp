// The frame path never waits for an asset load. Each test drives the engine's
// own per-frame work (EngineCore::StepRenderingLoop, what the Player runs every
// frame) or the editor's scene-open build, with model and material imports held
// at a gate the test releases. A frame that waited for an import lasts until the
// gate's watchdog releases it, so a frame over kAFrameThatWaited is a wait.

#include "HeldAssetImportFixture.h"

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "EngineLogCapture.h"
#include "Logger/Logger.h"

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

class SceneResolveFrameTest : public Testing::HeldAssetImportFixture
{
  protected:
    ECS::EntityHandle Spawn(ECS::World& world, const GUID& modelGuid)
    {
        Components::MeshRenderer meshRenderer{};
        meshRenderer.modelAssetGuid.Set(modelGuid);
        return world.Create<Components::MeshRenderer>(meshRenderer).GetHandle();
    }

    static uint64 MeshHandle(ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* meshRenderer = world.GetComponent<Components::MeshRenderer>(entity);
        return meshRenderer ? meshRenderer->meshGpuHandleId : 0;
    }

    // Frames until `done` holds or a deadline; every frame must return without
    // waiting for a load. Returns whether `done` held.
    template <typename Done>
    bool StepFramesUntil(Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done() && std::chrono::steady_clock::now() < deadline)
            EXPECT_LT(StepFrame(), kAFrameThatWaited) << "a frame waited for a load";
        return done();
    }

    bool MaterialRegistered(const GUID& materialGuid) { return m_Services.Materials().Registry().Find(materialGuid) != nullptr; }
};

} // namespace

// A model spawned during play that is not loaded yet: the frame returns before
// the model resolves, and the entity binds on a later frame once it has landed.
TEST_F(SceneResolveFrameTest, ASpawnOfAnUncachedModelDoesNotBlockTheFrame)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame(); // the loop's first frame, before anything spawns
    const ECS::EntityHandle entity = Spawn(*world, modelGuid);

    EXPECT_LT(StepFrame(), kAFrameThatWaited) << "the spawn's frame waited for the model to load";
    EXPECT_EQ(MeshHandle(*world, entity), 0u) << "the model cannot have resolved: its import is still held";
    ASSERT_TRUE(m_Gate.WaitForEntered(1)) << "the spawn did not start the model's load";

    m_Gate.Release();
    EXPECT_TRUE(StepFramesUntil([&] { return MeshHandle(*world, entity) != 0; }))
        << "the entity was never bound once its model landed";
}

// A spawned model whose load fails is reported, named by GUID and path, and no
// frame waits for it.
TEST_F(SceneResolveFrameTest, AMissingModelIsReportedAndDoesNotHang)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    const GUID modelGuid = RegisterHeldModel(/*fails=*/true);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();
    const ECS::EntityHandle entity = Spawn(*world, modelGuid);

    EXPECT_LT(StepFrame(), kAFrameThatWaited) << "the spawn's frame waited for the failing load";
    ASSERT_TRUE(m_Gate.WaitForEntered(1));
    m_Gate.Release();
    const std::string miss = "failed to load asset " + modelGuid.ToString();
    EXPECT_TRUE(StepFramesUntil([&] {
        Logger::Log::Flush();
        return TestLog::CountLinesContaining(lines, miss) != 0;
    })) << "the failed load was never reported";
    EXPECT_EQ(TestLog::CountLinesContaining(lines, miss), 1u) << "the miss is reported once";
    EXPECT_NE(TestLog::FirstLineContaining(lines, miss).find("hold_triangle.gltf"), std::string::npos)
        << "the report names the asset's path: " << TestLog::FirstLineContaining(lines, miss);
    for (int frame = 0; frame < 3; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_EQ(MeshHandle(*world, entity), 0u);
    EXPECT_EQ(m_Gate.Entered(), 1) << "the failed load was started again";
}

// A skinned model spawned during play whose model lands frames later: no frame
// waits, the skeleton binder leaves the entity alone while its model is still
// loading, and the entity ends with its SkeletonRef and AnimatorRef bound once.
TEST_F(SceneResolveFrameTest, ASkinnedSpawnBindsOnceWhenItsModelLands)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false, /*skinned=*/true);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();
    const ECS::EntityHandle entity = Spawn(*world, modelGuid);
    world->AddComponentImmediate(entity, Components::SkinnedMeshRenderer{});
    const uint64 runtimesBefore = SkeletonStore::Instance().GetRuntimeCreateEpoch();

    for (int frame = 0; frame < 3; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited) << "a frame waited for the skinned model";
    EXPECT_EQ(world->GetComponent<Components::SkeletonRef>(entity), nullptr)
        << "the binder bound the entity before its model resolved";
    ASSERT_TRUE(m_Gate.WaitForEntered(1));

    m_Gate.Release();
    EXPECT_TRUE(StepFramesUntil([&] {
        const auto* skeleton = world->GetComponent<Components::SkeletonRef>(entity);
        return skeleton && skeleton->runtimeId != 0;
    })) << "the skinned entity never bound its skeleton";
    EXPECT_NE(world->GetComponent<Components::AnimatorRef>(entity), nullptr);
    for (int frame = 0; frame < 3; ++frame)
        StepFrame();
    EXPECT_EQ(SkeletonStore::Instance().GetRuntimeCreateEpoch() - runtimesBefore, 1u)
        << "the skeleton runtime was created more than once";
}

// An animated helper node names a model that is not resident: the skeleton
// binder never waits for it, and the helper binds once the model has landed.
TEST_F(SceneResolveFrameTest, TheSkeletonBinderNeverWaitsForAModel)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false, /*skinned=*/true);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();
    const ECS::EntityHandle owner = world->Create<Components::Transform>(Components::Transform{}).GetHandle();
    Components::SkeletonRef helperRef{};
    helperRef.sourceModelGuid.Set(modelGuid);
    helperRef.ownerMode = Components::SkeletonInstanceOwner::Entity;
    helperRef.instanceOwner = owner;
    const ECS::EntityHandle helper = world->Create<Components::SkeletonRef>(helperRef).GetHandle();

    for (int frame = 0; frame < 3; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited) << "the skeleton binder waited for the helper's model";
    EXPECT_EQ(world->GetComponent<Components::SkeletonRef>(helper)->runtimeId, 0u);

    m_Gate.Release();
    EXPECT_TRUE(StepFramesUntil([&] { return world->GetComponent<Components::SkeletonRef>(helper)->runtimeId != 0; }))
        << "the helper never bound once its model landed";
}

// A skinned spawn whose model fails to load: the binder skipped it while its
// model was loading, and the miss hands it back, so the binder still visits it
// and records the source as unavailable rather than leaving it unseen.
TEST_F(SceneResolveFrameTest, ASkinnedSpawnWhoseModelMissesIsHandedBackToTheBinder)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/true, /*skinned=*/true);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();
    const ECS::EntityHandle entity = Spawn(*world, modelGuid);
    world->AddComponentImmediate(entity, Components::SkinnedMeshRenderer{});
    for (int frame = 0; frame < 2; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);
    EXPECT_EQ(world->GetComponent<Components::SkeletonRef>(entity), nullptr);

    m_Gate.Release();
    ASSERT_TRUE(StepFramesUntil([&] { return world->GetComponent<Components::SkeletonRef>(entity) != nullptr; }))
        << "the binder never visited the skinned entity after its model missed";
    EXPECT_EQ(world->GetComponent<Components::SkeletonRef>(entity)->runtimeId, 0u);
    EXPECT_EQ(MeshHandle(*world, entity), 0u);
}

// While the editor's scene-open build runs, a spawn made meanwhile is not
// dropped: it binds once the build has finished. A spawn with no build running
// binds as well. The spawn made during the build lives in an archetype of its
// own, so no write to the opened entity's chunk can bring it back to the bind's
// change filter: only a gate that did not move past it while the build ran does.
TEST_F(SceneResolveFrameTest, ASpawnResolvesWhetherOrNotAnEditorSceneBuildIsRunning)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    StepFrame();

    const ECS::EntityHandle opened = Spawn(*world, modelGuid);
    SceneBuildPump build;
    build.Begin(*world, m_Services);
    ASSERT_TRUE(IsSceneBuildPumpActive()) << "the build holds the opened entity until its model lands";
    Components::MeshRenderer spawnedMesh{};
    spawnedMesh.modelAssetGuid.Set(modelGuid);
    const ECS::EntityHandle spawnedDuringBuild =
        world->Create<Components::MeshRenderer, Components::Name>(spawnedMesh, Components::Name{"spawned"}).GetHandle();
    for (int frame = 0; frame < 3; ++frame)
        EXPECT_LT(StepFrame(), kAFrameThatWaited);

    m_Gate.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (build.IsActive() && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        build.StepBudgeted(*world, m_Services, std::chrono::milliseconds(5));
        StepFrame();
    }
    ASSERT_FALSE(IsSceneBuildPumpActive());
    EXPECT_NE(MeshHandle(*world, opened), 0u);
    EXPECT_TRUE(StepFramesUntil([&] { return MeshHandle(*world, spawnedDuringBuild) != 0; }))
        << "the spawn made during the editor's build was dropped";

    const ECS::EntityHandle spawnedAfter = Spawn(*world, modelGuid);
    EXPECT_TRUE(StepFramesUntil([&] { return MeshHandle(*world, spawnedAfter) != 0; }))
        << "a spawn with no build running did not bind";
}

// The editor's scene-open build requests every standalone material at once and
// registers each as it lands: a step never waits for one. Only DrainNow waits.
TEST_F(SceneResolveFrameTest, OnlyDrainNowWaitsForStandaloneMaterials)
{
    const GUID first = RegisterHeldMaterial(/*fails=*/false, "hold_a.material");
    const GUID second = RegisterHeldMaterial(/*fails=*/false, "hold_b.material");
    const GUID third = RegisterHeldMaterial(/*fails=*/false, "hold_c.material");
    ASSERT_FALSE(first.IsNull() || second.IsNull() || third.IsNull());
    ECS::World world;
    for (const GUID& material : {first, second, third})
    {
        Components::MeshRenderer meshRenderer{};
        meshRenderer.materialAssetGuid.Set(material);
        world.Create<Components::MeshRenderer>(meshRenderer);
    }

    SceneBuildPump build;
    build.Begin(world, m_Services);
    const auto start = std::chrono::steady_clock::now();
    build.StepBudgeted(world, m_Services, std::chrono::milliseconds(0));
    EXPECT_LT(std::chrono::steady_clock::now() - start, kAFrameThatWaited) << "the step waited for a material";
    EXPECT_TRUE(build.IsActive());
    EXPECT_TRUE(m_Gate.WaitForEntered(3)) << "the materials were not all requested at once: "
                                          << m_Gate.Entered() << " of 3 loads started";

    // DrainNow, with the loads still held, waits until they land.
    constexpr auto kHeldFor = std::chrono::milliseconds(200);
    std::thread releaser([this, kHeldFor] {
        std::this_thread::sleep_for(kHeldFor);
        m_Gate.Release();
    });
    const auto drainStart = std::chrono::steady_clock::now();
    build.DrainNow(world, m_Services);
    const auto drained = std::chrono::steady_clock::now() - drainStart;
    releaser.join();
    EXPECT_GE(drained, kHeldFor / 2) << "DrainNow returned before the held loads landed";
    EXPECT_TRUE(build.IsComplete());
    EXPECT_TRUE(MaterialRegistered(first) && MaterialRegistered(second) && MaterialRegistered(third))
        << "DrainNow returned with a material unregistered";
}

// A model that is resident and registered binds its spawns in their spawn frame
// however many arrive at once (projectiles, debris, a wave of units): the first
// pass for a registered model runs when the spawn is handed over, outside the
// resolve service's frame slice, which bounds only models that had to land.
TEST_F(SceneResolveFrameTest, AResidentSpawnBindsInItsSpawnFrameUnderABurst)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World* world = EnableRenderingLoop();
    ASSERT_NE(world, nullptr);
    m_Gate.Release();
    const ECS::EntityHandle first = Spawn(*world, modelGuid);
    ASSERT_TRUE(StepFramesUntil([&] { return MeshHandle(*world, first) != 0; })) << "the model never landed";

    // Enough entities that binding them takes far longer than one frame slice.
    constexpr std::size_t kBurst = 10000;
    std::vector<ECS::EntityHandle> burst;
    burst.reserve(kBurst);
    for (std::size_t i = 0; i < kBurst; ++i)
        burst.push_back(Spawn(*world, modelGuid));
    const auto frame = StepFrame();

    std::size_t bound = 0;
    for (ECS::EntityHandle entity : burst)
        bound += MeshHandle(*world, entity) != 0 ? 1 : 0;
    EXPECT_EQ(bound, kBurst) << "spawns of a registered model were left for later frames";
    const auto* meshRenderer = world->GetComponent<Components::MeshRenderer>(burst.back());
    ASSERT_NE(meshRenderer, nullptr);
    const GUID materialGuid = meshRenderer->materialAssetGuid.ToGuid();
    EXPECT_TRUE(materialGuid.IsNull() || MaterialRegistered(materialGuid))
        << "the spawn frame bound a mesh whose material is not registered";
    std::cout << "[SceneResolveFrame] " << kBurst << " resident spawns bound in their spawn frame of "
              << std::chrono::duration<double, std::milli>(frame).count() << " ms\n";
}
