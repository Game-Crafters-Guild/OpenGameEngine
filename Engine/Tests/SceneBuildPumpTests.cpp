// Locks the SceneBuildPump slicing contract: Begin hands the post-load resolve
// work to the build, and each StepBudgeted under a zero budget takes exactly one
// ready item, advancing the progress counters deterministically until the build
// completes. This is what lets the editor bound the scene-build work per frame
// so frames keep pumping during a heavy load.
//
// The entities here carry an unresolvable model GUID, so each item is a fast
// miss (the asset load misses at once and the build records it): the test pins
// the SLICING, not GPU resolution — that path is covered by
// ModelRenderSetupTests / SceneResolveFrameTests. Device-gated only because
// constructing a RenderServices (for the material registry query) needs a real
// device.

#include <gtest/gtest.h>

#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/SceneBuildPump.h"
#include "Engine/Rendering/RenderServices.h"

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"

#include "HeldAssetImportFixture.h"
#include "TestDeviceHelper.h"

#include "EngineLogCapture.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

GUID UnresolvableModelGuid()
{
    // A fixed, deterministic GUID that no test asset source resolves. Shared
    // across the fixture entities so the pump's per-model cache issues exactly
    // one (failing) load and every entity after the first hits the cached miss.
    uint8_t bytes[16] = {0xEE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    return GUID::FromBytes(bytes);
}

// Create `count` entities that look exactly like freshly deserialized scene
// MeshRenderers awaiting resolve: a model GUID, no GPU handle.
void SpawnUnresolvedMeshRenderers(ECS::World& world, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        Components::MeshRenderer mr{};
        mr.meshGpuHandleId = 0;
        mr.modelAssetGuid.Set(UnresolvableModelGuid());
        world.Create<Components::MeshRenderer>(mr);
    }
}

} // namespace

TEST(SceneBuildPumpTest, AZeroBudgetStepTakesExactlyOneItemAndCompletes)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    SpawnUnresolvedMeshRenderers(world, 7);

    SceneBuildPump pump;
    pump.Begin(world, rs);

    EXPECT_TRUE(pump.IsActive());
    EXPECT_FALSE(pump.IsComplete());
    EXPECT_EQ(pump.TotalItems(), 7u);
    EXPECT_EQ(pump.ProcessedItems(), 0u);

    // Each zero-budget step: exactly one item.
    for (std::size_t item = 1; item <= 6; ++item)
    {
        EXPECT_EQ(pump.StepBudgeted(world, rs, std::chrono::milliseconds(0)), 1u);
        EXPECT_EQ(pump.ProcessedItems(), item);
        EXPECT_FALSE(pump.IsComplete());
    }

    // The last model; the fixture has no standalone materials, so the build completes.
    EXPECT_EQ(pump.StepBudgeted(world, rs, std::chrono::milliseconds(0)), 1u);
    EXPECT_EQ(pump.ProcessedItems(), 7u);
    EXPECT_TRUE(pump.IsComplete());
    EXPECT_FALSE(pump.IsActive());

    // Stepping a completed build is a no-op.
    EXPECT_EQ(pump.StepBudgeted(world, rs, std::chrono::milliseconds(0)), 0u);
    EXPECT_EQ(pump.ProcessedItems(), 7u);

    device->Shutdown();
}

TEST(SceneBuildPumpTest, EmptyWorldCompletesImmediately)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world; // nothing to resolve

    SceneBuildPump pump;
    pump.Begin(world, rs);

    EXPECT_TRUE(pump.IsComplete());
    EXPECT_FALSE(pump.IsActive());
    EXPECT_EQ(pump.TotalItems(), 0u);
    EXPECT_EQ(pump.StepBudgeted(world, rs, std::chrono::milliseconds(8)), 0u);

    device->Shutdown();
}

TEST(SceneBuildPumpTest, StepBudgetedAlwaysMakesProgress)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    SpawnUnresolvedMeshRenderers(world, 4);

    SceneBuildPump pump;
    pump.Begin(world, rs);

    // A zero budget must still advance by at least one item (do-while guarantee),
    // so a pathologically small per-frame budget can never stall the load.
    std::size_t total = 0;
    int guard = 0;
    while (!pump.IsComplete() && guard++ < 100)
    {
        std::size_t n = pump.StepBudgeted(world, rs, std::chrono::milliseconds(0));
        EXPECT_GE(n, 1u);
        total += n;
    }
    EXPECT_TRUE(pump.IsComplete());
    EXPECT_EQ(total, 4u);

    device->Shutdown();
}

TEST(SceneBuildPumpTest, ActiveFlagTracksBeginAndComplete)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    EXPECT_FALSE(IsSceneBuildPumpActive());

    ECS::World world;
    SpawnUnresolvedMeshRenderers(world, 3);

    SceneBuildPump pump;
    pump.Begin(world, rs);
    EXPECT_TRUE(IsSceneBuildPumpActive());
    EXPECT_TRUE(pump.IsActive());

    pump.DrainNow(world, rs);
    EXPECT_TRUE(pump.IsComplete());
    EXPECT_FALSE(IsSceneBuildPumpActive());

    device->Shutdown();
}

namespace
{

class HeldImportTest : public Testing::HeldAssetImportFixture
{
  protected:
    // Steps until the build completes or a deadline; fails when one step took a second or more.
    void StepUntilComplete(SceneBuildPump& pump, ECS::World& world)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (pump.IsActive() && std::chrono::steady_clock::now() < deadline)
        {
            EngineCore::GetInstance().GetAssetManager().Update();
            const auto start = std::chrono::steady_clock::now();
            pump.StepBudgeted(world, m_Services, std::chrono::milliseconds(5));
            EXPECT_LT(std::chrono::steady_clock::now() - start, Testing::kAFrameThatWaited) << "a step waited";
        }
    }
};

} // namespace

// A model still importing does not hold the main thread: a step sets its entity aside and returns
// at once, and a later step resolves it once the import lands.
TEST_F(HeldImportTest, AStepDoesNotWaitForAModelStillImporting)
{
    const GUID modelGuid = RegisterHeldModel(/*fails=*/false);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World world;
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    const auto entity = world.Create<Components::MeshRenderer>(mr);

    SceneBuildPump pump;
    pump.Begin(world, m_Services);
    const auto start = std::chrono::steady_clock::now();
    pump.StepBudgeted(world, m_Services, std::chrono::milliseconds(0));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1)) << "the step waited for the import";
    EXPECT_EQ(pump.ProcessedItems(), 0u);
    EXPECT_TRUE(pump.IsActive());
    ASSERT_TRUE(m_Gate.WaitForEntered(1)) << "the import never reached the held model";

    m_Gate.Release();
    StepUntilComplete(pump, world);
    EXPECT_FALSE(pump.IsActive()) << "the entity was never resolved once the import landed";
    EXPECT_EQ(pump.ProcessedItems(), 1u);
    const auto* resolvedRenderer = world.GetComponent<Components::MeshRenderer>(entity.GetHandle());
    ASSERT_NE(resolvedRenderer, nullptr);
    EXPECT_NE(resolvedRenderer->meshGpuHandleId, 0u) << "the entity has no GPU mesh after the pump completed";
}

// An import that fails is recorded as a miss once: the entity is processed without a mesh, the
// failure is reported once, and no step starts a second import or waits for one.
TEST_F(HeldImportTest, AFailedImportIsRecordedOnceAndNeverWaitedFor)
{
    std::vector<std::string> messages;
    TestLog::ScopedEngineLogCapture capture(&messages);
    const GUID modelGuid = RegisterHeldModel(/*fails=*/true);
    ASSERT_FALSE(modelGuid.IsNull());
    ECS::World world;
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    const auto entity = world.Create<Components::MeshRenderer>(mr);

    SceneBuildPump pump;
    pump.Begin(world, m_Services);
    pump.StepBudgeted(world, m_Services, std::chrono::milliseconds(0));
    ASSERT_TRUE(m_Gate.WaitForEntered(1)) << "the import never reached the held model";
    m_Gate.Release();
    StepUntilComplete(pump, world);
    EXPECT_FALSE(pump.IsActive());
    EXPECT_EQ(m_Gate.Entered(), 1) << "the failed import was started again";
    Logger::Log::Flush();
    EXPECT_EQ(TestLog::CountLinesContaining(messages, "failed to load asset " + modelGuid.ToString()), 1u)
        << "the failure was reported more than once: the resolve asked for the asset again";
    EXPECT_EQ(pump.ProcessedItems(), 1u);
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(entity.GetHandle());
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->meshGpuHandleId, 0u);
}
