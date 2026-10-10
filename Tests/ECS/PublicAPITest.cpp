#include <gtest/gtest.h>
#include <memory>

#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "TestComponents.h"

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

// Explicit template instantiations for World methods with test components
namespace GameEngine::ECS {
    template void World::AddComponent<test::Health>(EntityHandle, const test::Health&);
    template test::Health* World::GetComponentForWrite<test::Health>(EntityHandle);
    template void World::RemoveComponent<test::Health>(EntityHandle);
    template void World::RemoveComponent<test::Position>(EntityHandle);
    template void World::RemoveComponent<test::Velocity>(EntityHandle);


}
using namespace GameEngine::ECS::test;

class PublicAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create world without job system for testing
        world = std::make_unique<World>(nullptr);

        // No need to manually register components - unified auto-registration handles this!
    }

    void TearDown() override {
        world.reset();
    }

    std::unique_ptr<World> world;
};

TEST_F(PublicAPITest, EntitySetGetRemoveAPI) {
    Logger::Log::Info("[PublicAPI Test] Testing entity.set(), entity.get(), entity.remove() API");
    
    // Create entity using public API
    auto handle = world->CreateEntity();
    Entity entity(world.get(), handle);
    
    // Test entity.set() - this should work now without hardcoded instantiations
    Position testPos{10.0f, 20.0f, 30.0f};
    entity.Set(testPos);

    // Process commands to make components available immediately
    world->ProcessCommands();

    // Test entity.get()
    auto* retrievedPos = entity.Get<Position>();
    ASSERT_NE(retrievedPos, nullptr);
    EXPECT_FLOAT_EQ(retrievedPos->x, 10.0f);
    EXPECT_FLOAT_EQ(retrievedPos->y, 20.0f);
    EXPECT_FLOAT_EQ(retrievedPos->z, 30.0f);
    
    // Test entity.has()
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_FALSE(entity.Has<Velocity>());
    
    // Test adding another component
    Velocity testVel{1.0f, 2.0f, 3.0f};
    entity.Set(testVel);

    // Process commands again
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());
    
    auto* retrievedVel = entity.Get<Velocity>();
    ASSERT_NE(retrievedVel, nullptr);
    EXPECT_FLOAT_EQ(retrievedVel->x, 1.0f);
    EXPECT_FLOAT_EQ(retrievedVel->y, 2.0f);
    EXPECT_FLOAT_EQ(retrievedVel->z, 3.0f);
    
    // Test entity.remove()
    entity.Remove<Velocity>();

    // Process commands for removal
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_FALSE(entity.Has<Velocity>());
    
    auto* removedVel = entity.Get<Velocity>();
    EXPECT_EQ(removedVel, nullptr);
    
    Logger::Log::Info("[PublicAPI Test] All public API methods work correctly!");
}

TEST_F(PublicAPITest, FluentAPIChaining) {
    Logger::Log::Info("[PublicAPI Test] Testing fluent API chaining");
    
    // Test method chaining
    auto handle = world->CreateEntity();
    Entity entity(world.get(), handle);
    
    entity.Set(Position{1.0f, 2.0f, 3.0f})
          .Set(Velocity{0.1f, 0.2f, 0.3f})
          .Set(Health{100, 100});

    // Process commands for chained operations
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());
    EXPECT_TRUE(entity.Has<Health>());
    
    auto* pos = entity.Get<Position>();
    auto* vel = entity.Get<Velocity>();
    auto* health = entity.Get<Health>();
    
    ASSERT_NE(pos, nullptr);
    ASSERT_NE(vel, nullptr);
    ASSERT_NE(health, nullptr);
    
    EXPECT_FLOAT_EQ(pos->x, 1.0f);
    EXPECT_FLOAT_EQ(vel->x, 0.1f);
    EXPECT_EQ(health->current, 100);
    
    // Test remove chaining
    entity.Remove<Velocity>().Remove<Health>();

    // Process commands for chained removals
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_FALSE(entity.Has<Velocity>());
    EXPECT_FALSE(entity.Has<Health>());
    
    Logger::Log::Info("[PublicAPI Test] Fluent API chaining works correctly!");
}

TEST_F(PublicAPITest, ExistingComponentTypes) {
    Logger::Log::Info("[PublicAPI Test] Testing with existing component types that have explicit instantiations");

    // Test with existing component types that should work
    auto handle = world->CreateEntity();
    Entity entity(world.get(), handle);

    // Test with Position (should have explicit instantiation)
    Position pos{5.0f, 10.0f, 15.0f};
    entity.Set(pos);

    // Process commands
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Position>());
    auto* retrievedPos = entity.Get<Position>();
    ASSERT_NE(retrievedPos, nullptr);
    EXPECT_FLOAT_EQ(retrievedPos->x, 5.0f);
    EXPECT_FLOAT_EQ(retrievedPos->y, 10.0f);
    EXPECT_FLOAT_EQ(retrievedPos->z, 15.0f);

    // Test with Velocity (should have explicit instantiation)
    Velocity vel{1.5f, 2.5f, 3.5f};
    entity.Set(vel);

    // Process commands
    world->ProcessCommands();

    EXPECT_TRUE(entity.Has<Velocity>());
    auto* retrievedVel = entity.Get<Velocity>();
    ASSERT_NE(retrievedVel, nullptr);
    EXPECT_FLOAT_EQ(retrievedVel->x, 1.5f);
    EXPECT_FLOAT_EQ(retrievedVel->y, 2.5f);
    EXPECT_FLOAT_EQ(retrievedVel->z, 3.5f);

    // Test removal
    entity.Remove<Position>();

    // Process commands for removal
    world->ProcessCommands();

    EXPECT_FALSE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());

    Logger::Log::Info("[PublicAPI Test] Existing component types work correctly!");
}
