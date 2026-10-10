// ECS Modular Architecture Integration Test
// This test verifies that components from different libraries work together seamlessly

#include <gtest/gtest.h>
#include <iostream>

// Include ECS core
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"  // Enable auto-registration for components
#include "Logger/Logger.h"
#include "TestComponents.h"

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

class ECSIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create world without job system for testing
        world = std::make_unique<World>(nullptr);
    }

    void TearDown() override {
        world.reset();
    }

    std::unique_ptr<World> world;
};

TEST_F(ECSIntegrationTest, MultiLibraryComponentConcepts) {
    // Test 1: Verify Multi-Library Component Concepts

    // ECS core components
    static_assert(Component<Disabled>, "ECS Disabled component should work");

    // Test components (from Tests library)
    static_assert(Component<Position>, "Test Position component should work");
    static_assert(Component<Velocity>, "Test Velocity component should work");
    static_assert(Component<Rotation>, "Test Rotation component should work");
    static_assert(Component<Scale>, "Test Scale component should work");
    static_assert(Component<Transform>, "Test Transform component should work");
    static_assert(Component<Health>, "Test Health component should work");
    static_assert(Component<Timer>, "Test Timer component should work");

    // All static assertions passed if we reach here
    SUCCEED();
}

TEST_F(ECSIntegrationTest, CrossLibraryComponentIntegration) {
    // Test 2: Cross-Library Component Integration

    // Create entities with components from different libraries
    Entity gameObject = world->Create();
    Entity testEntity = world->Create();
    Entity mixedEntity = world->Create();

    // Entity 1: Transform components only
    world->AddComponent(gameObject.GetHandle(), Position{1.0f, 2.0f, 3.0f});
    world->AddComponent(gameObject.GetHandle(), Velocity{0.1f, 0.2f, 0.3f});
    world->AddComponent(gameObject.GetHandle(), Rotation{0.0f, 0.0f, 0.0f, 1.0f});
    world->AddComponent(gameObject.GetHandle(), Scale{1.0f, 1.0f, 1.0f});

    // Entity 2: Game components only
    world->AddComponent(testEntity.GetHandle(), Health{100, 100});
    world->AddComponent(testEntity.GetHandle(), Timer{5.0f});

    // Entity 3: Mixed components from all libraries
    world->AddComponent(mixedEntity.GetHandle(), Position{5.0f, 6.0f, 7.0f});  // Transform
    world->AddComponent(mixedEntity.GetHandle(), Health{50, 100});             // Game
    world->AddComponent(mixedEntity.GetHandle(), Disabled{});                  // ECS Core

    // Process commands to ensure all registrations happen
    world->ProcessCommands();
}

TEST_F(ECSIntegrationTest, CrossLibraryQueries) {
        // Test 3: Cross-Library Queries

        // Create entities with components from different libraries
        Entity gameObject = world->Create();
        Entity testEntity = world->Create();
        Entity mixedEntity = world->Create();

        // Entity 1: Transform components only
        world->AddComponent(gameObject.GetHandle(), Position{1.0f, 2.0f, 3.0f});
        world->AddComponent(gameObject.GetHandle(), Velocity{0.1f, 0.2f, 0.3f});

        // Entity 2: Game components only
        world->AddComponent(testEntity.GetHandle(), Health{100, 100});

        // Entity 3: Mixed components from all libraries
        world->AddComponent(mixedEntity.GetHandle(), Position{5.0f, 6.0f, 7.0f});  // Transform
        world->AddComponent(mixedEntity.GetHandle(), Health{50, 100});             // Game

        // Process commands to ensure all registrations happen
        world->ProcessCommands();

        // Query 1: Transform components only
        auto transformQuery = world->Query<Position, Velocity>();
        int transformCount = 0;
        transformQuery.Each([&transformCount](EntityHandle /*entity*/, Position& pos, Velocity& /*vel*/) {
            transformCount++;
            EXPECT_GT(pos.x, 0.0f);
        });
        EXPECT_EQ(transformCount, 1); // gameObject

        // Query 2: Game components only
        auto gameQuery = world->Query<Health>();
        int gameCount = 0;
        gameQuery.Each([&gameCount](EntityHandle /*entity*/, Health& health) {
            gameCount++;
            EXPECT_GT(health.current, 0);
        });
        EXPECT_EQ(gameCount, 2); // testEntity and mixedEntity

        // Query 3: Mixed components from different libraries
        auto mixedQuery = world->Query<Position, Health>();
        int mixedCount = 0;
        mixedQuery.Each([&mixedCount](EntityHandle /*entity*/, Position& pos, Health& health) {
            mixedCount++;
            EXPECT_GT(pos.x, 0.0f);
            EXPECT_GT(health.current, 0);
        });
        EXPECT_EQ(mixedCount, 1); // mixedEntity
}

TEST_F(ECSIntegrationTest, ComponentRetrievalAcrossLibraries) {
        // Test 4: Component Retrieval Across Libraries

        // Create entities with components from different libraries
        Entity gameObject = world->Create();
        Entity testEntity = world->Create();
        Entity mixedEntity = world->Create();

        // Entity 1: Transform components only
        world->AddComponent(gameObject.GetHandle(), Position{1.0f, 2.0f, 3.0f});
        world->AddComponent(gameObject.GetHandle(), Velocity{0.1f, 0.2f, 0.3f});

        // Entity 2: Game components only
        world->AddComponent(testEntity.GetHandle(), Health{100, 100});
        world->AddComponent(testEntity.GetHandle(), Timer{5.0f});

        // Entity 3: Mixed components from all libraries
        world->AddComponent(mixedEntity.GetHandle(), Position{5.0f, 6.0f, 7.0f});  // Transform
        world->AddComponent(mixedEntity.GetHandle(), Health{50, 100});             // Game
        world->AddComponent(mixedEntity.GetHandle(), Disabled{});                  // ECS Core

        // Process commands to ensure all registrations happen
        world->ProcessCommands();

        // Test Transform components
        auto* pos = world->GetComponent<Position>(gameObject.GetHandle());
        auto* vel = world->GetComponent<Velocity>(gameObject.GetHandle());
        ASSERT_NE(pos, nullptr);
        ASSERT_NE(vel, nullptr);
        EXPECT_FLOAT_EQ(pos->x, 1.0f);
        EXPECT_FLOAT_EQ(pos->y, 2.0f);
        EXPECT_FLOAT_EQ(pos->z, 3.0f);

        // Test Game components
        auto* health = world->GetComponent<Health>(testEntity.GetHandle());
        auto* timer = world->GetComponent<Timer>(testEntity.GetHandle());
        ASSERT_NE(health, nullptr);
        ASSERT_NE(timer, nullptr);
        EXPECT_EQ(health->current, 100);
        EXPECT_EQ(health->maximum, 100);

        // Mixed components
        auto* mixedPos = world->GetComponent<Position>(mixedEntity.GetHandle());
        auto* mixedHealth = world->GetComponent<Health>(mixedEntity.GetHandle());
        auto* disabled = world->GetComponent<Disabled>(mixedEntity.GetHandle());
        ASSERT_NE(mixedPos, nullptr);
        ASSERT_NE(mixedHealth, nullptr);
        ASSERT_NE(disabled, nullptr);
        EXPECT_FLOAT_EQ(mixedPos->x, 5.0f);
        EXPECT_EQ(mixedHealth->current, 50);
}

TEST_F(ECSIntegrationTest, ComponentExistenceChecks) {
        // Test 5: Component Existence Checks

        // Create entities with components from different libraries
        Entity gameObject = world->Create();
        Entity testEntity = world->Create();
        Entity mixedEntity = world->Create();

        // Entity 1: Transform components only
        world->AddComponent(gameObject.GetHandle(), Position{1.0f, 2.0f, 3.0f});
        world->AddComponent(gameObject.GetHandle(), Velocity{0.1f, 0.2f, 0.3f});

        // Entity 2: Game components only
        world->AddComponent(testEntity.GetHandle(), Health{100, 100});
        world->AddComponent(testEntity.GetHandle(), Timer{5.0f});

        // Entity 3: Mixed components from all libraries
        world->AddComponent(mixedEntity.GetHandle(), Position{5.0f, 6.0f, 7.0f});  // Transform
        world->AddComponent(mixedEntity.GetHandle(), Health{50, 100});             // Game
        world->AddComponent(mixedEntity.GetHandle(), Disabled{});                  // ECS Core

        // Process commands to ensure all registrations happen
        world->ProcessCommands();

        // Transform components
        EXPECT_TRUE(world->HasComponent<Position>(gameObject.GetHandle()));
        EXPECT_TRUE(world->HasComponent<Velocity>(gameObject.GetHandle()));
        EXPECT_FALSE(world->HasComponent<Health>(gameObject.GetHandle()));

        // Game components
        EXPECT_TRUE(world->HasComponent<Health>(testEntity.GetHandle()));
        EXPECT_TRUE(world->HasComponent<Timer>(testEntity.GetHandle()));
        EXPECT_FALSE(world->HasComponent<Position>(testEntity.GetHandle()));

        // Mixed components
        EXPECT_TRUE(world->HasComponent<Position>(mixedEntity.GetHandle()));
        EXPECT_TRUE(world->HasComponent<Health>(mixedEntity.GetHandle()));
        EXPECT_TRUE(world->HasComponent<Disabled>(mixedEntity.GetHandle()));
        EXPECT_FALSE(world->HasComponent<Velocity>(mixedEntity.GetHandle()));
}

// Architecture Summary:
//
// ECS Library (Minimal Core):
//   - Only contains Disabled component (used by ECS system itself)
//   - Provides unified Component concept and auto-registration system
//   - No hardcoded knowledge of specific component types
//
// Test Library:
//   - Contains test-specific components (Position, Velocity, Rotation, Scale, Transform, Health, Timer, etc.)
//   - Auto-registers components when first used in tests
//   - Uses the unified ECS auto-registration system
//
// This demonstrates the perfect modular architecture where:
//   1. ECS core is truly minimal and generic
//   2. Each library owns and registers its own components
//   3. All components use the same elegant auto-registration system
//   4. No manual registration or hardcoded component lists needed
//   5. Components from different categories work seamlessly together
