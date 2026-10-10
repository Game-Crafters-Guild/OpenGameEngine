#include <gtest/gtest.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>

// Minimal ECS test with Google Test
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"  // Enable auto-registration for components
#include "Logger/Logger.h"
#include "TestComponents.h"

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;
#include "ECS/WorldBuilder.h"
#include "ECS/Scheduler.h"


class ECSCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
        // Cleanup
    }
};

TEST_F(ECSCoreTest, BasicWorldAndEntityCreation) {

        // Create world
        auto world = std::make_unique<World>(nullptr);
        ASSERT_NE(world.get(), nullptr);

        // Create entity
        auto entity = world->Create();
        auto handle = entity.GetHandle();
        EXPECT_GE(handle.index, 0);  // Index can start at 0
        EXPECT_GT(handle.version, 0);

        // Test component addition using public API (triggers auto-registration)
        Position testPos{10.0f, 20.0f, 30.0f};
        world->AddComponent(handle, testPos);
        world->ProcessCommands(); // Process the deferred command

        // Test component retrieval
        auto* pos = world->GetComponent<Position>(handle);
        ASSERT_NE(pos, nullptr);
        EXPECT_FLOAT_EQ(pos->x, 10.0f);
        EXPECT_FLOAT_EQ(pos->y, 20.0f);
        EXPECT_FLOAT_EQ(pos->z, 30.0f);
}

// World job-system wiring lock: World::GetJobSystem() is authoritative for
// parallel execution — non-null means Query parallel APIs fan out on exactly
// that pool, null means deliberately serial. This locks the construction seam
// the engine wiring rides on (EngineCore::EnsurePrimaryWorld constructs the
// primary world with &GetJobSystem(); full engine init is impractical in this
// standalone host, so the wiring line itself is grep-locked instead — no
// EngineCore pool reach-ins exist in system code).
TEST_F(ECSCoreTest, WorldJobSystemIsAuthoritativeAtConstruction) {
    JobSystem::WorkStealingThreadPool js(2);

    World wired(&js);
    EXPECT_EQ(wired.GetJobSystem(), &js);

    WorldConfig cfg{};
    World cfgWired(cfg, &js);
    EXPECT_EQ(cfgWired.GetJobSystem(), &js);

    World unwired;
    EXPECT_EQ(unwired.GetJobSystem(), nullptr);

    // SetJobSystem is the post-construction seam (TLAS parity tests wire
    // bare worlds through it); it must be equally authoritative.
    unwired.SetJobSystem(&js);
    EXPECT_EQ(unwired.GetJobSystem(), &js);
    unwired.SetJobSystem(nullptr);
    EXPECT_EQ(unwired.GetJobSystem(), nullptr);
}

// Null-pool worlds run Query parallel APIs inline on the calling thread —
// the serial half of the wiring contract (Query::ParallelBatchEach falls
// back to BatchEach when world->jobSystem is null; no engine-pool ladder).
TEST_F(ECSCoreTest, ParallelBatchEachOnUnwiredWorldRunsInline) {
    World w;  // deliberately serial
    ASSERT_EQ(w.GetJobSystem(), nullptr);

    w.BeginBulkOperations();
    for (int i = 0; i < 24; ++i)
        w.CreateHandle(Position{static_cast<float>(i), 0, 0});
    w.EndBulkOperations();

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<size_t> visited{0};
    std::atomic<int> offThread{0};
    auto q = w.Query<Read<Position>>();
    q.ParallelBatchEach([&](const Position* p, std::size_t count) {
        ASSERT_NE(p, nullptr);
        if (std::this_thread::get_id() != caller)
            offThread.fetch_add(1, std::memory_order_relaxed);
        visited.fetch_add(count, std::memory_order_relaxed);
    });

    EXPECT_EQ(visited.load(), 24u);
    EXPECT_EQ(offThread.load(), 0) << "an unwired world fanned out — null must mean serial";
}

// Removed MultipleComponentsTest as it duplicates functionality already covered by:
// - StandaloneECSTest::ComponentAdditionAndRetrieval (single component)
// - StandaloneECSTest::MultipleComponentsOnEntity (multiple components)
// - AutoRegistrationTests::BasicBuiltinComponentTest (Position + Velocity)
// - IntegrationTest::ComponentRetrievalAcrossLibraries (comprehensive multi-component testing)

// New tests for chunk iteration and scheduler sequencing
TEST_F(ECSCoreTest, ChunkIteration_OptionalPointerAndCount) {
    JobSystem::WorkStealingThreadPool js(2);
    WorldBuilder b; b.SetJobSystem(&js).PreRegister<Position, Velocity>();
    auto w = b.Build();

    w->BeginBulkOperations();
    for (int i = 0; i < 24; ++i) {
        if (i % 3 == 0) w->CreateHandle(Position{float(i),0,0}, Velocity{1,0,0});
        else            w->CreateHandle(Position{float(i),0,0});
    }
    w->EndBulkOperations();

    auto q = w->Query<Read<Position>, Optional<Velocity>>();

    size_t total = 0; size_t withVel = 0; size_t withoutVel = 0;
    q.ForEachChunk([&](const Position* p, const Velocity* v, size_t count){
        ASSERT_NE(p, nullptr);
        ASSERT_GT(count, 0u);
        total += count;
        if (v) ++withVel; else ++withoutVel;
    });

    EXPECT_EQ(w->Query<Read<Velocity>>().Count(), static_cast<size_t>(8));
    EXPECT_EQ(w->Query<Read<Position>>().Count(), static_cast<size_t>(24));
    EXPECT_EQ(total, static_cast<size_t>(24));
    EXPECT_GT(withoutVel + withVel, 0u);
}

TEST_F(ECSCoreTest, ChunkIteration_AdapterWithoutCount) {
    JobSystem::WorkStealingThreadPool js(2);
    auto w = WorldBuilder{}.SetJobSystem(&js).PreRegister<Position, Velocity>().Build();

    w->BeginBulkOperations();
    for (int i = 0; i < 12; ++i) {
        if (i % 2 == 0) w->CreateHandle(Position{float(i),0,0}, Velocity{1,0,0});
        else            w->CreateHandle(Position{float(i),0,0});
    }
    w->EndBulkOperations();

    auto q = w->Query<Read<Position>, Optional<Velocity>>();

    size_t called = 0;
    q.ForEachChunk([&](const Position* p, const Velocity* v){ (void)p; (void)v; called++; });
    EXPECT_GT(called, 0u);
}

TEST_F(ECSCoreTest, Scheduler_Sequencing_WriteVisibleToRead) {
    JobSystem::WorkStealingThreadPool js(3);
    auto w = WorldBuilder{}.SetJobSystem(&js).PreRegister<Position, Velocity>().Build();

    w->BeginBulkOperations();
    for (int i = 0; i < 36; ++i) {
        if (i % 3 == 0) w->CreateHandle(Position{float(i),0,0}, Velocity{0,0,0});
        else            w->CreateHandle(Position{float(i),0,0});
    }
    w->EndBulkOperations();

    auto qWrite = w->Query<Write<Velocity>>();
    auto qRead  = w->Query<Read<Position>, Optional<Velocity>>();

    Scheduler sched(&js);
    auto jWrite = Scheduler::SystemJob::Parallel(qWrite, [&](EntityHandle, Velocity& v){ v.x += 2.0f; }, 0).Name("Bump");
    auto jRead  = Scheduler::SystemJob::ForEachChunk(qRead, [&](const Position* p, const Velocity* v, size_t count){
        if (!v) return;
        for (size_t i = 0; i < count; ++i) ASSERT_GE(v[i].x, 2.0f);
        (void)p;
    }).Name("Read");

    jRead.DependsOn(0); // ensure read after write

    std::vector<Scheduler::SystemJob> sysJobs; sysJobs.push_back(jRead); sysJobs.push_back(jWrite);
    sched.Run(sysJobs);

    size_t changed = 0;
    w->Query<Read<Velocity>>().Each([&](EntityHandle, const Velocity& v){ if (v.x >= 2.0f) changed++; });
    EXPECT_EQ(changed, w->Query<Read<Velocity>>().Count());
}


TEST_F(ECSCoreTest, GameComponentsTest) {
        // Test game-specific components
        auto world = std::make_unique<World>(nullptr);
        ASSERT_NE(world.get(), nullptr);

        // Create entity
        auto entity = world->Create();
        auto handle = entity.GetHandle();

        // Add Health component using public API
        Health testHealth{100, 100};
        world->AddComponent(handle, testHealth);

        // Add Timer component using public API
        Timer testTimer{5.0f};
        world->AddComponent(handle, testTimer);

        // Process commands
        world->ProcessCommands();

        // Test Health component retrieval
        auto* health = world->GetComponent<Health>(handle);
        ASSERT_NE(health, nullptr);
        EXPECT_EQ(health->current, 100);
        EXPECT_EQ(health->maximum, 100);

        // Test Timer component retrieval
        auto* timer = world->GetComponent<Timer>(handle);
        ASSERT_NE(timer, nullptr);
        EXPECT_FLOAT_EQ(timer->duration, 5.0f);

        // Test component existence checks
        EXPECT_TRUE(world->HasComponent<Health>(handle));
        EXPECT_TRUE(world->HasComponent<Timer>(handle));
        EXPECT_FALSE(world->HasComponent<Position>(handle));
}

// Regression test for the archetype-edge self-loop bug (introduced in
// 61d0424f, "Colocated ECS storage", 2026-04-16). Removing a component from
// an entity that doesn't have it used to cache `arch.AddEdge[T] = arch` (a
// self-loop), so a subsequent AddComponent<T> on a sibling entity in the
// same archetype was silently a no-op. Caught by SceneIO LoadBasicScene
// (Parent missing on child after sibling root had Parent removed first).
TEST_F(ECSCoreTest, RemoveAbsentComponent_DoesNotPoisonAddEdgeCache)
{
    auto world = std::make_unique<World>(nullptr);
    auto entityA = world->CreateEntity();
    auto entityB = world->CreateEntity();

    // Both entities share archetype{Position} after this.
    world->AddComponentImmediate(entityA, Position{1, 2, 3});
    world->AddComponentImmediate(entityB, Position{4, 5, 6});

    // Remove Velocity from entityA — entityA never had Velocity. Pre-fix
    // this poisoned arch{Position}.AddEdge[Velocity] -> arch{Position}.
    world->RemoveComponentImmediate<Velocity>(entityA);

    // Add Velocity to entityB. Pre-fix this silently no-oped because the
    // poisoned cache entry made GetOrCreateArchetypeForAdd return the same
    // archetype, skipping the migration.
    world->AddComponentImmediate(entityB, Velocity{10, 20, 30});

    auto* vel = world->GetComponent<Velocity>(entityB);
    ASSERT_NE(vel, nullptr) << "Velocity should exist after AddComponentImmediate; "
                               "regression of archetype-edge self-loop bug";
    EXPECT_FLOAT_EQ(vel->x, 10.0f);
    EXPECT_FLOAT_EQ(vel->y, 20.0f);
    EXPECT_FLOAT_EQ(vel->z, 30.0f);
}
