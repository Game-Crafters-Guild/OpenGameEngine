#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
// Test-owned components: the .cpp that touches them instantiates the World
// component templates itself (see ECS/ECSTemplates.h).
#include "ECS/ECSTemplates.h"
#include "TestComponents.h"
#include "ECS/ComponentRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"

using namespace GameEngine;
using namespace GameEngine::ECS::test;
using namespace GameEngine::ECS;

class QueryInvalidationAndOverflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        world = std::make_unique<World>(jobSystem.get());
    }
    void TearDown() override {
        world.reset();
        jobSystem.reset();
        ComponentRegistry::Clear();
    }
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<World> world;
};

TEST_F(QueryInvalidationAndOverflowTest, QueryCacheUpdatesOnStructuralChange) {
    // Initial entities with Position only
    std::vector<Entity> entities;
    for (int i = 0; i < 5; ++i) {
        auto e = world->Create();
        e.Set(Position{(float)i, 0, 0});
        entities.push_back(e);
    }
    world->ProcessCommands();

    // Build query for Position,Velocity and ensure zero count initially
    auto q = world->Query<Read<Position>, Read<Velocity>>();
    EXPECT_EQ(q.Count(), 0u);

    // Add Velocity to some entities (structural change to Position+Velocity archetype)
    for (int i = 0; i < 3; ++i) {
        entities[i].Set(Velocity{1,0,0});
    }
    world->ProcessCommands();

    // Query should now see updated archetypes without explicit invalidation
    size_t found = 0;
    q.Each([&](EntityHandle, const Position& p, const Velocity& v){ (void)p; (void)v; ++found; });
    EXPECT_EQ(found, 3u);
}

TEST_F(QueryInvalidationAndOverflowTest, CommandOverflowFallsBackToImmediate) {
    // Create a world with extremely small command buffers by reinitializing with tiny config
    WorldConfig cfg;
    cfg.CommandBufferSize = 2; // force overflow quickly
    World tinyWorld(cfg, jobSystem.get());

    // Create more commands than buffer capacity without processCommands in-between
    auto e1 = tinyWorld.Create();
    for (int i = 0; i < 10; ++i) {
        e1.Set(Position{(float)i, 0, 0});
    }
    // Some commands should have fallen back immediately; processing should still be safe
    tinyWorld.ProcessCommands();

    // Component must exist regardless of overflow
    auto* pos = e1.Get<Position>();
    ASSERT_NE(pos, nullptr);
}

