#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "Pathfinding/NavigationWorld.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Scene/SceneIO.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{
constexpr float32 kDt = 1.f / 60.f;

// The only path a test allocates is its queued request's: a fresh PathBuffer
// hands it out as index 0, generation 1.
constexpr Pathfinding::PathHandle kFirstPath{0, 1};

// Holds the only worker of a one-thread pool at a gate and hands that pool to
// the navigation world, so a request submitted through the system stays queued
// until Finish releases the worker.
class HeldWorker
{
public:
    explicit HeldWorker(Pathfinding::NavigationWorld& navigation) : m_Navigation(navigation), m_Pool(1)
    {
        m_Blocker = m_Pool.Submit([this] {
            std::unique_lock lock(m_Mutex);
            m_Entered = true;
            m_Changed.notify_all();
            m_Changed.wait(lock, [this] { return m_Released; });
        });
        std::unique_lock lock(m_Mutex);
        if (!m_Changed.wait_for(lock, std::chrono::seconds(5), [this] { return m_Entered; }))
        {
            m_Released = true;
            lock.unlock();
            m_Changed.notify_all();
            m_Pool.Shutdown();
            throw std::runtime_error("worker did not enter fixture gate");
        }
        m_Navigation.SetJobSystem(&m_Pool);
    }

    ~HeldWorker() { Finish(); }

    void Finish()
    {
        if (m_Finished)
            return;
        {
            std::lock_guard lock(m_Mutex);
            m_Released = true;
        }
        m_Changed.notify_all();
        // Wait for the actual navigation task to finish before destroying its
        // pool or navigation world. Shutdown may cancel queued work, so first
        // wait for the navigation owner's active-job count to reach zero.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (m_Navigation.HasActivePathJobs() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        EXPECT_FALSE(m_Navigation.HasActivePathJobs());
        m_Pool.Shutdown();
        m_Navigation.SetJobSystem(nullptr);
        m_Finished = true;
    }

private:
    Pathfinding::NavigationWorld& m_Navigation;
    JobSystem::WorkStealingThreadPool m_Pool;
    JobSystem::TaskHandle m_Blocker;
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    bool m_Entered = false;
    bool m_Released = false;
    bool m_Finished = false;
};

class NavigationRequestReplacementTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        m_World = std::make_unique<ECS::World>(nullptr);
        RegisterNavigationWorldHooks(*m_World);
        Pathfinding::GridSettings settings;
        settings.Width = 32;
        settings.Depth = 32;
        settings.CellSize = 1;
        // This map is owned by the service fixture, not an ECS grid. Keeping it
        // unchanged across replacement obeys the async map-lifetime contract.
        m_Map = NavigationService::Get().AddGridMap(settings);
    }

    void TearDown() override
    {
        m_World.reset();
        NavigationService::Shutdown();
    }

    ECS::Entity Actor(ECS::World& world, bool moving)
    {
        Components::NavigationAgent agent;
        agent.NavMapIndex = m_Map.Index;
        agent.NavMapGeneration = m_Map.Generation;
        agent.AgentId = 0; // No reservation writes while the solver is queued.
        agent.Radius = .25f;
        agent.ReplanInterval = 0;
        Components::NavigationAgentState state;
        state.HasDestination = moving;
        state.PathDirty = moving;
        state.DestinationX = 25.5f;
        state.DestinationZ = 10.5f;
        Components::WorldTransform transform;
        transform.matrix[12] = 1.5f;
        transform.matrix[14] = 10.5f;
        auto entity = world.Create(agent, state, transform);
        return entity;
    }

    // A stopped replacement actor keeps its cleared navigation state. A stale
    // completion shows up as a path whose end is the old actor's destination,
    // which the failure carries.
    static testing::AssertionResult Untouched(ECS::Entity replacement, bool pathDirty = false)
    {
        const auto* state = replacement.Get<Components::NavigationAgentState>();
        if (!state)
            return testing::AssertionFailure() << "the replacement actor has no NavigationAgentState";
        if (!state->HasDestination && state->PathDirty == pathDirty && state->PathGeneration == 0 &&
            state->Status == static_cast<uint8>(Pathfinding::PathStatus::Invalid))
            return testing::AssertionSuccess();
        auto failure = testing::AssertionFailure()
                       << "an old async result changed the stopped replacement actor: entity="
                       << replacement.GetHandle().id << " destination=" << state->DestinationX << ','
                       << state->DestinationZ << " status=" << static_cast<uint32>(state->Status);
        if (state->PathGeneration)
        {
            const Pathfinding::PathHandle path{state->PathIndex, state->PathGeneration};
            const auto& buffer = NavigationService::Get().GetPathBuffer();
            if (const auto count = buffer.GetPointCount(path))
            {
                const auto end = buffer.GetPoint(path, count - 1);
                failure << " path end=" << end.X << ',' << end.Z;
            }
        }
        return failure;
    }

    std::unique_ptr<ECS::World> m_World;
    Pathfinding::NavMapHandle m_Map;
    NavigationPathfindingSystem m_System;
};

TEST_F(NavigationRequestReplacementTest, DefaultServiceUsesSynchronousFallbackAcrossClear)
{
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_FALSE(NavigationService::Get().HasActivePathJobs());
    ASSERT_EQ(ECS::Entity(m_World.get(), old).Get<Components::NavigationAgentState>()->Status,
              static_cast<uint8>(Pathfinding::PathStatus::Complete));
    m_World->Clear();
    const auto replacement = Actor(*m_World, false);
    ASSERT_EQ(replacement.GetHandle().id, old.id);
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(Untouched(replacement));
}

TEST_F(NavigationRequestReplacementTest, AsyncCompletionReachesItsOriginalActor)
{
    HeldWorker worker(NavigationService::Get());
    const auto actor = Actor(*m_World, true);
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    ASSERT_EQ(actor.Get<Components::NavigationAgentState>()->Status,
              static_cast<uint8>(Pathfinding::PathStatus::Pending));
    worker.Finish();
    m_System.Update(*m_World, kDt);
    EXPECT_EQ(actor.Get<Components::NavigationAgentState>()->Status,
              static_cast<uint8>(Pathfinding::PathStatus::Complete));
    EXPECT_NE(actor.Get<Components::NavigationAgentState>()->PathGeneration, 0u);
}

TEST_F(NavigationRequestReplacementTest, DestroyRecreateGenerationDoesNotConsumeOldCompletion)
{
    HeldWorker worker(NavigationService::Get());
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    m_World->DestroyEntityImmediate(old);
    const auto replacement = Actor(*m_World, false);
    ASSERT_NE(replacement.GetHandle().id, old.id);
    worker.Finish();
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(Untouched(replacement));
}

TEST_F(NavigationRequestReplacementTest, OutstandingRequestCannotReachReusedHandleAfterClear)
{
    HeldWorker worker(NavigationService::Get());
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    m_World->Clear();
    const auto replacement = Actor(*m_World, false);
    ASSERT_EQ(replacement.GetHandle().id, old.id);
    worker.Finish();
    const auto& buffer = NavigationService::Get().GetPathBuffer();
    ASSERT_GT(buffer.GetPointCount(kFirstPath), 0u) << "the queued request did not allocate the first path";
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(Untouched(replacement));
    EXPECT_EQ(buffer.GetPointCount(kFirstPath), 0u) << "the discarded result's path was not released to its buffer";
}

TEST_F(NavigationRequestReplacementTest, OutstandingRequestCannotReachDifferentWorld)
{
    HeldWorker worker(NavigationService::Get());
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    ECS::World replacementWorld(nullptr);
    const auto replacement = Actor(replacementWorld, false);
    ASSERT_EQ(replacement.GetHandle().id, old.id);
    ASSERT_NE(replacementWorld.GetWorldId(), m_World->GetWorldId());
    worker.Finish();
    m_System.Update(replacementWorld, kDt);
    EXPECT_TRUE(Untouched(replacement));
}

TEST_F(NavigationRequestReplacementTest, ReplacementDestinationWaitsForOldRequestThenSolvesItsOwnRoute)
{
    HeldWorker worker(NavigationService::Get());
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    m_World->Clear();
    auto replacement = Actor(*m_World, true);
    ASSERT_EQ(replacement.GetHandle().id, old.id);
    auto* state = replacement.GetForWrite<Components::NavigationAgentState>();
    state->DestinationX = 3.5f;
    state->DestinationZ = 25.5f;
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(state->PathDirty);
    EXPECT_EQ(state->PathGeneration, 0u);
    worker.Finish();
    m_System.Update(*m_World, kDt);
    ASSERT_EQ(state->Status, static_cast<uint8>(Pathfinding::PathStatus::Complete));
    const Pathfinding::PathHandle path{state->PathIndex, state->PathGeneration};
    auto& buffer = NavigationService::Get().GetPathBuffer();
    const auto count = buffer.GetPointCount(path);
    ASSERT_GT(count, 0u);
    const auto last = buffer.GetPoint(path, count - 1);
    EXPECT_FLOAT_EQ(last.X, 3.5f);
    EXPECT_FLOAT_EQ(last.Z, 25.5f);
}

TEST_F(NavigationRequestReplacementTest, OldServiceResultCannotAliasOrFreeNewServicePath)
{
    HeldWorker worker(NavigationService::Get());
    auto actor = Actor(*m_World, true);
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    worker.Finish(); // Respect the old NavigationWorld's async lifetime contract.
    const auto oldGeneration = NavigationService::GetGeneration();
    NavigationService::Shutdown();
    NavigationService::Initialize();
    ASSERT_NE(NavigationService::GetGeneration(), oldGeneration);
    auto* state = actor.GetForWrite<Components::NavigationAgentState>();
    *state = {};
    state->PathDirty = false;
    const Pathfinding::PathPoint sentinel[] = {{2, 0, 2}, {3, 0, 3}};
    auto& newBuffer = NavigationService::Get().GetPathBuffer();
    const auto newPath = newBuffer.AllocatePath(sentinel, 2);
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(Untouched(actor));
    EXPECT_EQ(newBuffer.GetPointCount(newPath), 2u);
    newBuffer.FreePath(newPath);
}

TEST_F(NavigationRequestReplacementTest, OutstandingRequestCannotReachActualSceneReplacement)
{
    const TestUtils::ScopedTempDir sceneDir{TestUtils::MakeUniqueTempDirectory("navigation-replacement")};
    const auto file = sceneDir.Path() / "navigation-replacement.scene";
    {
        ECS::World authored(nullptr);
        Actor(authored, false);
        ASSERT_TRUE(Scene::SaveSceneToFile(authored, file));
    }
    HeldWorker worker(NavigationService::Get());
    const auto old = Actor(*m_World, true).GetHandle();
    m_System.Update(*m_World, kDt);
    ASSERT_TRUE(NavigationService::Get().HasActivePathJobs());
    Scene::LoadOptions options;
    options.mode = Scene::LoadMode::Replace;
    Scene::SceneLoadDegradation degradation;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(*m_World, file, options));
    ASSERT_FALSE(degradation.IsDegraded());
    ECS::EntityHandle replacement;
    m_World->Query<ECS::Read<Components::NavigationAgent>>().Each(
        [&](ECS::EntityHandle handle, const Components::NavigationAgent&) { replacement = handle; });
    ASSERT_EQ(replacement.id, old.id);
    worker.Finish();
    m_System.Update(*m_World, kDt);
    EXPECT_TRUE(Untouched(ECS::Entity(m_World.get(), replacement), true));
}
} // namespace
