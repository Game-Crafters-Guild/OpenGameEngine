#include <gtest/gtest.h>
#include <memory>
#include <chrono>

// Minimal ECS implementation for testing
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Components.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Systems.h"
#include "ECS/ECSTemplates.h"  // Enable auto-registration for components
#include "Logger/Logger.h"
#include "TestComponents.h"

using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

class StandaloneECSTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Logger is already initialized by the engine

        // Create world without job system for testing
        world = std::make_unique<World>(nullptr);

        // No need to manually register components - unified auto-registration handles this!
    }

    void TearDown() override {
        world.reset();
    }

    std::unique_ptr<World> world;
};

TEST_F(StandaloneECSTest, EntityCreationAndDestruction) {
    // Test entity creation
    auto entity1 = world->Create();
    auto entity2 = world->Create();
    
    EXPECT_TRUE(entity1.IsValid());
    EXPECT_TRUE(entity2.IsValid());
    EXPECT_NE(entity1.GetHandle().id, entity2.GetHandle().id);

    // Test entity count
    EXPECT_EQ(world->GetEntityCount(), 2);
    
    // Test entity destruction
    entity1.Destroy();
    world->ProcessCommands(); // Process deferred destruction
    
    EXPECT_EQ(world->GetEntityCount(), 1);
    EXPECT_TRUE(entity2.IsValid());
}

TEST_F(StandaloneECSTest, ComponentAdditionAndRetrieval) {
    Logger::Log::Info("[ECS Test] Creating entity...");
    auto entity = world->Create();
    auto handle = entity.GetHandle();

    Logger::Log::Info("[ECS Test] Entity created with handle: index={}, version={}",
                static_cast<uint32_t>(handle.index), static_cast<uint32_t>(handle.version));

    // Test using the public API which triggers auto-registration
    Logger::Log::Info("[ECS Test] Adding Position component...");
    Position testPos{10.0f, 20.0f, 30.0f};
    Logger::Log::Info("[ECS Test] Test position values before adding: x={}, y={}, z={}", testPos.x, testPos.y, testPos.z);

    // Use the public API instead of addComponentImpl - this triggers auto-registration
    world->AddComponent(handle, testPos);
    world->ProcessCommands(); // Process the deferred command

    Logger::Log::Info("[ECS Test] Retrieving Position component...");
    auto* pos = world->GetComponent<Position>(handle);

    ASSERT_NE(pos, nullptr);
    Logger::Log::Info("[ECS Test] Retrieved position values: x={}, y={}, z={}", pos->x, pos->y, pos->z);

    EXPECT_FLOAT_EQ(pos->x, 10.0f);
    EXPECT_FLOAT_EQ(pos->y, 20.0f);
    EXPECT_FLOAT_EQ(pos->z, 30.0f);
}

TEST_F(StandaloneECSTest, ComponentHasCheck) {
    auto entity = world->Create();
    auto handle = entity.GetHandle();

    // Initially no components
    EXPECT_FALSE(world->HasComponent<Position>(handle));
    EXPECT_FALSE(world->HasComponent<Velocity>(handle));

    // Add a component using public API
    world->AddComponent(handle, Position{1.0f, 2.0f, 3.0f});
    world->ProcessCommands(); // Process the deferred command

    // Check has component
    EXPECT_TRUE(world->HasComponent<Position>(handle));
    EXPECT_FALSE(world->HasComponent<Velocity>(handle));
}

TEST_F(StandaloneECSTest, ComponentRemoval) {
    auto entity = world->Create();
    auto handle = entity.GetHandle();

    // Add components using public API
    world->AddComponent(handle, Position{1.0f, 2.0f, 3.0f});
    world->AddComponent(handle, Velocity{4.0f, 5.0f, 6.0f});
    world->ProcessCommands(); // Process the deferred commands

    EXPECT_TRUE(world->HasComponent<Position>(handle));
    EXPECT_TRUE(world->HasComponent<Velocity>(handle));

    // Remove a component using public API
    world->RemoveComponent<Velocity>(handle);
    world->ProcessCommands(); // Process the deferred command

    EXPECT_TRUE(world->HasComponent<Position>(handle));
    EXPECT_FALSE(world->HasComponent<Velocity>(handle));
}

TEST_F(StandaloneECSTest, BasicQuery) {
    // Create some entities with different component combinations
    auto entity1 = world->Create();
    auto handle1 = entity1.GetHandle();
    world->AddComponent(handle1, Position{1.0f, 2.0f, 3.0f});
    world->AddComponent(handle1, Velocity{0.1f, 0.2f, 0.3f});

    auto entity2 = world->Create();
    auto handle2 = entity2.GetHandle();
    world->AddComponent(handle2, Position{4.0f, 5.0f, 6.0f});
    // No velocity for entity2

    auto entity3 = world->Create();
    auto handle3 = entity3.GetHandle();
    world->AddComponent(handle3, Position{7.0f, 8.0f, 9.0f});
    world->AddComponent(handle3, Velocity{0.4f, 0.5f, 0.6f});

    // Process all deferred commands
    world->ProcessCommands();

    // Query for entities with both Position and Velocity
    auto query = world->Query<Read<Position>, Read<Velocity>>();

    // First, let's verify the components are still accessible directly
    Logger::Log::Info("[ECS Test] Entity1 handle: index={}, version={}",
                static_cast<uint32_t>(handle1.index), static_cast<uint32_t>(handle1.version));
    Logger::Log::Info("[ECS Test] Entity3 handle: index={}, version={}",
                static_cast<uint32_t>(handle3.index), static_cast<uint32_t>(handle3.version));

    Logger::Log::Info("[ECS Test] About to call getComponent for entity1...");
    Logger::Log::Info("[ECS Test] Entity1 should have Position + Velocity components");
    auto* pos1 = world->GetComponent<Position>(handle1);
    Logger::Log::Info("[ECS Test] getComponent for entity1 returned: {}", static_cast<const void*>(pos1));

    Logger::Log::Info("[ECS Test] About to call getComponent for entity3...");
    Logger::Log::Info("[ECS Test] Entity3 should have Position + Velocity components");
    auto* pos3 = world->GetComponent<Position>(handle3);
    Logger::Log::Info("[ECS Test] getComponent for entity3 returned: {}", static_cast<const void*>(pos3));

    // Let's also check entity2 (the Position-only entity)
    Logger::Log::Info("[ECS Test] About to call getComponent for entity2...");
    Logger::Log::Info("[ECS Test] Entity2 should have Position only");
    auto* pos2 = world->GetComponent<Position>(handle2);
    Logger::Log::Info("[ECS Test] getComponent for entity2 returned: {}", static_cast<const void*>(pos2));

    if (pos1) {
        Logger::Log::Info("[ECS Test] Entity1 pos pointer: {}, size: {}", static_cast<const void*>(pos1), sizeof(*pos1));
        Logger::Log::Info("[ECS Test] Entity1 pos memory: x={}, y={}, z={}", pos1->x, pos1->y, pos1->z);

        // Let's examine the raw memory
        auto* rawBytes = reinterpret_cast<const uint8_t*>(pos1);
        Logger::Log::Info("[ECS Test] Entity1 pos raw bytes: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                    rawBytes[0], rawBytes[1], rawBytes[2], rawBytes[3],
                    rawBytes[4], rawBytes[5], rawBytes[6], rawBytes[7],
                    rawBytes[8], rawBytes[9], rawBytes[10], rawBytes[11]);
    } else {
        Logger::Log::Warning("[ECS Test] Failed to get Position component for entity1");
    }

    if (pos2) {
        Logger::Log::Info("[ECS Test] Entity2 pos pointer: {}, size: {}", static_cast<const void*>(pos2), sizeof(*pos2));
        Logger::Log::Info("[ECS Test] Entity2 pos memory: x={}, y={}, z={}", pos2->x, pos2->y, pos2->z);
    } else {
        Logger::Log::Warning("[ECS Test] Failed to get Position component for entity2");
    }

    if (pos3) {
        Logger::Log::Info("[ECS Test] Entity3 pos pointer: {}, size: {}", static_cast<const void*>(pos3), sizeof(*pos3));
        Logger::Log::Info("[ECS Test] Entity3 pos memory: x={}, y={}, z={}", pos3->x, pos3->y, pos3->z);
    } else {
        Logger::Log::Warning("[ECS Test] Failed to get Position component for entity3");
    }

    int count = 0;
    query.Each([&count](EntityHandle entity, const Position& pos, const Velocity& vel) {
        Logger::Log::Info("[ECS Test] Query found entity {} with pos({}, {}, {}) vel({}, {}, {})",
                    static_cast<uint32_t>(entity.index), pos.x, pos.y, pos.z, vel.x, vel.y, vel.z);
        count++;
    });

    // Should find 2 entities (entity1 and entity3) that have both components
    EXPECT_EQ(count, 2);

    // Test query count method
    EXPECT_EQ(query.Count(), 2);
}

TEST_F(StandaloneECSTest, ArchetypeCreation) {
    // Initially no archetypes
    EXPECT_EQ(world->GetArchetypeCount(), 0);

    // Create entity with Position - should create archetype
    auto entity1 = world->Create();
    auto handle1 = entity1.GetHandle();
    world->AddComponent(handle1, Position{1.0f, 1.0f, 1.0f});
    world->ProcessCommands();

    EXPECT_EQ(world->GetArchetypeCount(), 1);

    // Create entity with Position and Velocity - should create new archetype
    auto entity2 = world->Create();
    auto handle2 = entity2.GetHandle();
    world->AddComponent(handle2, Position{2.0f, 2.0f, 2.0f});
    world->AddComponent(handle2, Velocity{1.0f, 1.0f, 1.0f});
    world->ProcessCommands();

    EXPECT_EQ(world->GetArchetypeCount(), 2);

    // Create another entity with Position only - should reuse first archetype
    auto entity3 = world->Create();
    auto handle3 = entity3.GetHandle();
    world->AddComponent(handle3, Position{3.0f, 3.0f, 3.0f});
    world->ProcessCommands();

    EXPECT_EQ(world->GetArchetypeCount(), 2); // Still 2 archetypes
}

TEST_F(StandaloneECSTest, WorldClear) {
    // Create some entities
    for (int i = 0; i < 5; ++i) {
        auto entity = world->Create();
        auto handle = entity.GetHandle();
        world->AddComponent(handle, Position{static_cast<float>(i), 0.0f, 0.0f});
    }
    world->ProcessCommands();

    EXPECT_EQ(world->GetEntityCount(), 5);
    EXPECT_GT(world->GetArchetypeCount(), 0);

    // Clear the world
    world->Clear();

    EXPECT_EQ(world->GetEntityCount(), 0);
    EXPECT_EQ(world->GetArchetypeCount(), 0);
}

// Performance test with many entities
TEST_F(StandaloneECSTest, PerformanceTest) {
    const int NUM_ENTITIES = 1000; // Reduced for faster testing

    // Create many entities
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<EntityHandle> handles;
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        auto entity = world->Create();
        auto handle = entity.GetHandle();
        handles.push_back(handle);
        world->AddComponent(handle, Position{static_cast<float>(i), 0.0f, 0.0f});
        world->AddComponent(handle, Velocity{1.0f, 0.0f, 0.0f});
    }
    world->ProcessCommands(); // Process all deferred commands

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log::Info("[ECS Test] Created {} entities in {} ms", NUM_ENTITIES, duration.count());

    EXPECT_EQ(world->GetEntityCount(), NUM_ENTITIES);

    // Simple iteration performance test (without complex Query system)
    start = std::chrono::high_resolution_clock::now();

    int count = 0;
    for (const auto& handle : handles) {
        auto* pos = world->GetComponentForWrite<Position>(handle);
        auto* vel = world->GetComponent<Velocity>(handle);
        if (pos && vel) {
            pos->x += vel->x * 0.016f; // Simulate 60 FPS update
            count++;
        }
    }

    end = std::chrono::high_resolution_clock::now();
    auto microDuration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    Logger::Log::Info("[ECS Test] Processed {} entities in {} μs", count, microDuration.count());

    EXPECT_EQ(count, NUM_ENTITIES);
    EXPECT_LT(microDuration.count(), 50000); // Should be under 50ms for 1k entities
}

// Component Registry Tests
TEST_F(StandaloneECSTest, ComponentRegistryTest) {
    // First, use some components to trigger auto-registration
    auto entity = world->Create();
    auto handle = entity.GetHandle();

    // Use Position and Health components to trigger auto-registration
    world->AddComponent(handle, Position{1.0f, 2.0f, 3.0f});
    world->AddComponent(handle, Health{100, 100});
    world->ProcessCommands();

    // Now test component registration
    auto componentNames = ComponentRegistry::GetAllComponentNames();
    EXPECT_GT(componentNames.size(), 0);

    // Debug: Print all registered component names
    Logger::Log::Info("[ECS Test] Auto-registered component names:");
    for (const auto& name : componentNames) {
        Logger::Log::Info("[ECS Test]   - {}", name);
    }

    // Test that components are auto-registered. Names come from
    // ComponentTypeName<T>(), so they are the same on every compiler; this case
    // pins the count, and ComponentTypeNameTest pins the spellings.
    EXPECT_GE(ComponentRegistry::GetComponentCount(), 2);

    // Test that we can find components by their type IDs (which is what matters)
    ComponentTypeId positionTypeId = GetComponentTypeId<Position>();
    ComponentTypeId healthTypeId = GetComponentTypeId<Health>();

    // Verify the components are registered by type ID
    auto positionInfo = ComponentRegistry::GetComponentInfo(positionTypeId);
    auto healthInfo = ComponentRegistry::GetComponentInfo(healthTypeId);

    ASSERT_NE(positionInfo, nullptr);
    ASSERT_NE(healthInfo, nullptr);

    EXPECT_EQ(positionInfo->Size, sizeof(Position));
    EXPECT_EQ(healthInfo->Size, sizeof(Health));
    EXPECT_EQ(positionInfo->TypeId, positionTypeId);
    EXPECT_EQ(healthInfo->TypeId, healthTypeId);

    Logger::Log::Info("[ECS Test] Auto-registration working correctly with {} components",
                ComponentRegistry::GetComponentCount());
}
