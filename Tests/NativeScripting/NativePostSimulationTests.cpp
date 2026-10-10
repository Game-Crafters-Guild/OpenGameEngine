#include <gtest/gtest.h>

#include "Components/Transform.h"
#include "ECS/SystemScheduling.h"
#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "GameSDK/System.h"
#include "NativeScripting/NativeScriptingABI.h"
#include "PathfindingECS/Systems/RegisterPathfindingSystems.h"
#include "PhysicsECS/Systems/RegisterPhysicsSystems.h"
#include "Scripting/NativePostSimulationSystem.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace GameEngine;
namespace ns = GameEngine::NativeScripting;

namespace
{
struct Probe : ns::IUserSystem
{
    int Starts = 0, Updates = 0, Posts = 0, Stops = 0;
    float LastDelta = 0;
    bool ThrowPost = false, ClearDuringUpdate = false, StopDuringUpdate = false;
    bool ReenterPost = false;
    void OnStart(ECS::World&) override { ++Starts; }
    void OnUpdate(ECS::World& world, float) override
    {
        ++Updates;
        if (ClearDuringUpdate)
            world.Clear();
        if (StopDuringUpdate)
            ns::StopUserSystems(world);
    }
    void OnDestroy(ECS::World&) override { ++Stops; }
    const char* Name() const override { return "PostProbe"; }
    bool HasPostSimulation() const override { return true; }
    void OnPostSimulation(ECS::World& world, float dt) override
    {
        ++Posts;
        LastDelta = dt;
        if (ReenterPost)
            EXPECT_FALSE(ns::PostSimulateUserSystems(world));
        if (ThrowPost)
            throw std::runtime_error("late callback test");
    }
};

class NativePostSimulation : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ns::ClearUserSystems();
        ns::SetActiveRegistrationModule({});
    }
    void TearDown() override
    {
        ns::ClearUserSystems();
        ns::SetActiveRegistrationModule({});
    }
};

struct AdapterProbe : ECS::SystemBase
{
    static inline int Posts = 0;
    void OnPostSimulation(ECS::World&, float) { ++Posts; }
};
struct OldStyleSystem : ECS::SystemBase
{
    void OnUpdate(ECS::World&, float) {}
};
struct DeferredProbe : ECS::SystemBase
{
    static inline ECS::EntityHandle Target;
    void OnPostSimulation(ECS::World& world, float)
    {
        Components::Transform value;
        value.matrix[12] = 9.f;
        ECS::Entity(&world, Target).Set(value);
    }
};

TEST_F(NativePostSimulation, AdapterOptsInAndLegacySystemsRemainNoOp)
{
    ECS::World world;
    ECS::UserSystemAdapter<AdapterProbe> late("AdapterProbe");
    ECS::UserSystemAdapter<OldStyleSystem> old("OldStyleSystem");
    EXPECT_TRUE(late.HasPostSimulation());
    EXPECT_FALSE(old.HasPostSimulation());
    EXPECT_EQ(GE_USERMODULE_ABI_VERSION, 2u);
    AdapterProbe::Posts = 0;
    ns::RegisterUserSystem(&late);
    ns::RegisterUserSystem(&old);
    ns::StartUserSystems(world);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    ns::TickUserSystems(world, .125f);
    EXPECT_EQ(AdapterProbe::Posts, 0);
    EXPECT_TRUE(ns::PostSimulateUserSystems(world));
    EXPECT_EQ(AdapterProbe::Posts, 1);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
}

TEST_F(NativePostSimulation, OncePerCompletedTickUsesNativeDeltaAndExactWorld)
{
    ECS::World world, preview;
    Probe probe;
    probe.ReenterPost = true;
    ns::RegisterUserSystem(&probe);
    ns::TickUserSystems(world, .1f); // no play lifecycle
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .25f);
    EXPECT_FALSE(ns::PostSimulateUserSystems(preview));
    NativePostSimulationSystem bridge(nullptr);
    bridge.Update(world, 99.f);
    EXPECT_EQ(probe.Posts, 1);
    EXPECT_FLOAT_EQ(probe.LastDelta, .25f);
    bridge.Update(world, 99.f); // redraw / another view / paused frame
    EXPECT_EQ(probe.Posts, 1);
    ns::TickUserSystems(world, .5f);
    bridge.Update(world, 99.f);
    EXPECT_EQ(probe.Posts, 2);
    EXPECT_FLOAT_EQ(probe.LastDelta, .5f);
    ns::TickUserSystems(world, .5f);
    ns::StopUserSystems(world);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
}

TEST_F(NativePostSimulation, ResetBeforeOrDuringUpdateRevokesTheOldScene)
{
    ECS::World world;
    Probe probe;
    ns::RegisterUserSystem(&probe);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    world.Clear();
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    probe.ClearDuringUpdate = true;
    ns::TickUserSystems(world, .1f);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    probe.ClearDuringUpdate = false;
    ns::TickUserSystems(world, .1f);
    EXPECT_TRUE(ns::PostSimulateUserSystems(world));
    EXPECT_EQ(probe.Posts, 1);
    probe.StopDuringUpdate = true;
    ns::TickUserSystems(world, .1f);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
}

TEST_F(NativePostSimulation, ModuleReplacementRevokesOldDeliveryAndRetainsOtherOwners)
{
    ECS::World world;
    Probe project, package, replacement;
    ns::SetActiveRegistrationModule("Project");
    ns::RegisterUserSystem(&project);
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&package);
    ns::SetActiveRegistrationModule({});
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    ns::ClearUserSystems("Package");
    ns::SetActiveRegistrationModule("Package");
    ns::RegisterUserSystem(&replacement);
    ns::SetActiveRegistrationModule({});
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    ns::TickUserSystems(world, .1f);
    EXPECT_TRUE(ns::PostSimulateUserSystems(world));
    EXPECT_EQ(project.Posts, 1);
    EXPECT_EQ(package.Posts, 0);
    EXPECT_EQ(replacement.Posts, 1);
}

TEST_F(NativePostSimulation, ThrowingLateHookKeepsTheGuardAndDoesNotReplay)
{
    ECS::World world;
    Probe throwing, healthy;
    throwing.ThrowPost = true;
    ns::RegisterUserSystem(&throwing);
    ns::RegisterUserSystem(&healthy);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    EXPECT_TRUE(ns::PostSimulateUserSystems(world));
    EXPECT_EQ(throwing.Posts, 1);
    EXPECT_EQ(healthy.Posts, 1);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
}

TEST_F(NativePostSimulation, LateHookKeepsOrdinaryDeferredCommandSemantics)
{
    ECS::World world;
    const auto entity = world.Create();
    DeferredProbe::Target = entity.GetHandle();
    Components::Transform initial;
    initial.matrix[12] = 2.f;
    world.AddComponentImmediate(DeferredProbe::Target, initial);
    ECS::UserSystemAdapter<DeferredProbe> adapter("DeferredProbe");
    ns::RegisterUserSystem(&adapter);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    NativePostSimulationSystem{nullptr}.Update(world, .1f);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Transform>(DeferredProbe::Target)->matrix[12], 2.f);
    world.ProcessCommands();
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Transform>(DeferredProbe::Target)->matrix[12], 9.f);
}

TEST_F(NativePostSimulation, LegacyOnlyRegistrationDoesNotDispatchOrRequestALateDrain)
{
    ECS::World world;
    ECS::UserSystemAdapter<OldStyleSystem> old("OldStyleSystem");
    ns::RegisterUserSystem(&old);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
    EXPECT_FALSE(ns::PostSimulateUserSystems(world));
}

TEST_F(NativePostSimulation, ThrowingCapabilityQueryKeepsTheCrashGuard)
{
    struct ThrowingCapability : Probe
    {
        bool HasPostSimulation() const override { throw std::runtime_error("capability test"); }
    } bad;
    ECS::World world;
    Probe healthy;
    ns::RegisterUserSystem(&bad);
    ns::RegisterUserSystem(&healthy);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    bool dispatched = false;
    EXPECT_NO_THROW(dispatched = ns::PostSimulateUserSystems(world));
    EXPECT_TRUE(dispatched);
    EXPECT_EQ(bad.Posts, 0);
    EXPECT_EQ(healthy.Posts, 1);
}

struct WaveState
{
    std::atomic<int> Active{0}, BeforeFinished{0}, AfterFinished{0}, ExclusiveCalls{0};
    std::thread::id Caller = std::this_thread::get_id();
};
class PeerSystem : public ECS::ISystem
{
  public:
    PeerSystem(WaveState* state, bool after) : State(state), After(after) {}
    const char* GetName() const override { return "Peer"; }
    void Update(ECS::World&, float32) override
    {
        ++State->Active;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (After)
        {
            EXPECT_GT(State->ExclusiveCalls.load(), 0);
            ++State->AfterFinished;
        }
        else
            ++State->BeforeFinished;
        --State->Active;
    }
    WaveState* State;
    bool After;
};
class ExclusiveSystem : public ECS::ISystem
{
  public:
    explicit ExclusiveSystem(WaveState* state) : State(state) {}
    const char* GetName() const override { return "Exclusive"; }
    // No override specifier so the same regression compiles against the old
    // scheduler: its ignored requirement then fails the actual wave assertions.
    bool RequiresExclusiveUpdate() const { return true; }
    void Update(ECS::World&, float32) override
    {
        EXPECT_EQ(std::this_thread::get_id(), State->Caller);
        EXPECT_EQ(State->Active.load(), 0);
        EXPECT_GE(State->BeforeFinished.load(), 2);
        ++State->ExclusiveCalls;
    }
    WaveState* State;
};

TEST(ExclusiveSystemExecution, MixedWaveJoinsBothSidesAndHonorsEnabledFrequency)
{
    JobSystem::WorkStealingThreadPool pool(2);
    ECS::SystemManager manager(&pool);
    ECS::World world;
    WaveState state;
    manager.AddSystem<PeerSystem>(&state, false);
    manager.AddSystem<PeerSystem>(&state, false);
    auto* exclusive = manager.AddSystem<ExclusiveSystem>(&state);
    manager.AddSystem<PeerSystem>(&state, true);
    manager.AddSystem<PeerSystem>(&state, true);
    ECS::SystemExecutionPlan plan;
    plan.Waves.push_back({{0, 1, 2, 3, 4}});
    manager.SetExecutionPlan(std::move(plan));
    manager.SetEveryNFrames<ExclusiveSystem>(2);
    for (int i = 0; i < 3; ++i)
        manager.Update(world, .01f);
    EXPECT_EQ(state.ExclusiveCalls, 2);
    EXPECT_EQ(state.BeforeFinished, 6);
    EXPECT_EQ(state.AfterFinished, 6);
    exclusive->SetEnabled(false);
    manager.Update(world, .01f);
    EXPECT_EQ(state.ExclusiveCalls, 2);
    exclusive->SetEnabled(true);
    manager.SetParallelWavesEnabled(false);
    manager.Update(world, .01f);
    manager.Update(world, .01f);
    EXPECT_EQ(state.ExclusiveCalls, 3);
}

TEST(ExclusiveSystemExecution, LegacyParallelPathHonorsTheSameExclusiveRequirement)
{
    JobSystem::WorkStealingThreadPool pool(2);
    ECS::SystemManager manager(&pool);
    ECS::World world;
    WaveState state;
    manager.AddParallelSystem<PeerSystem>(&state, false);
    manager.AddParallelSystem<PeerSystem>(&state, false);
    manager.AddParallelSystem<ExclusiveSystem>(&state);
    manager.AddParallelSystem<PeerSystem>(&state, true);
    manager.AddParallelSystem<PeerSystem>(&state, true);
    manager.Update(world, .01f);
    EXPECT_EQ(state.ExclusiveCalls, 1);
    EXPECT_EQ(state.AfterFinished, 2);
}

TEST(NativePostSimulationSchedule, RealModuleSchedulePlacesHookAfterMovementBeforeExtraction)
{
    ECS::SystemScheduleBuilder builder;
    Engine::Renderer::AddRenderingSystemsToSchedule(builder, nullptr);
    PhysicsECS::AddPhysicsSystemsToSchedule(builder);
    PathfindingECS::AddPathfindingSystemsToSchedule(builder);
    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager);
    auto waveFor = [&](const char* name)
    {
        for (size_t w = 0; w < manager.GetExecutionPlan().Waves.size(); ++w)
            for (size_t index : manager.GetExecutionPlan().Waves[w].SystemIndices)
                if (std::string_view(manager.GetSequentialSystemName(index)) == name)
                    return w;
        return size_t(-1);
    };
    const auto late = waveFor("NativePostSimulation");
    ASSERT_NE(late, size_t(-1));
    EXPECT_LT(waveFor("TransformHierarchy"), late);
    EXPECT_LT(waveFor("NavigationMovementSystem"), late);
    EXPECT_LT(waveFor("PhysicsWriteback"), late);
    EXPECT_LT(waveFor("CharacterControllerWriteback"), late);
    EXPECT_GT(waveFor("RenderExtraction"), late);
    EXPECT_GT(waveFor("RenderGraphBuild"), late);
    ASSERT_NE(manager.GetSystem<NativePostSimulationSystem>(), nullptr);
    EXPECT_TRUE(manager.GetSystem<NativePostSimulationSystem>()->RequiresExclusiveUpdate());
}
} // namespace
