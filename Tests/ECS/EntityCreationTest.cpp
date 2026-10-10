// NewBatchCreationTest.cpp - Comprehensive verification of new batch creation methods
#include <gtest/gtest.h>
#include "ECS/ECSTemplates.h"
#include "ECS/Components.h"
#include "Logger/Logger.h"
#include "TestComponents.h"
#include <chrono>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

class EntityCreationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create world with performance-optimized config
        WorldConfig config;
        config.EnableDebugLogging = false;
        world = std::make_unique<World>(config);
    }

    void TearDown() override {
        world.reset();
    }

    std::unique_ptr<World> world;
};

// Test correctness of new template-based creation
TEST_F(EntityCreationTest, TemplateCreationCorrectness) {
    // Test single entity creation with components
    auto entity = world->Create<Position, Velocity>(
        Position{1.0f, 2.0f, 3.0f},
        Velocity{0.1f, 0.2f, 0.3f}
    );
    
    world->ProcessCommands();
    
    // Verify entity is valid and has components
    EXPECT_TRUE(entity.IsValid());
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());
    
    // Verify component values
    auto* pos = entity.Get<Position>();
    auto* vel = entity.Get<Velocity>();
    ASSERT_NE(pos, nullptr);
    ASSERT_NE(vel, nullptr);
    
    EXPECT_FLOAT_EQ(pos->x, 1.0f);
    EXPECT_FLOAT_EQ(pos->y, 2.0f);
    EXPECT_FLOAT_EQ(pos->z, 3.0f);
    EXPECT_FLOAT_EQ(vel->x, 0.1f);
    EXPECT_FLOAT_EQ(vel->y, 0.2f);
    EXPECT_FLOAT_EQ(vel->z, 0.3f);
    
    // Verify archetype count (should be 1)
    EXPECT_EQ(world->GetArchetypeCount(), 1);
}

// Test batch creation with same components
TEST_F(EntityCreationTest, BatchCreationCorrectness) {
    const size_t batchSize = 100;
    
    auto entities = world->CreateBatch<Position, Velocity>(batchSize,
        Position{5.0f, 6.0f, 7.0f},
        Velocity{0.5f, 0.6f, 0.7f}
    );
    
    world->ProcessCommands();
    
    // Verify all entities were created
    EXPECT_EQ(entities.size(), batchSize);
    EXPECT_EQ(world->GetEntityCount(), batchSize);
    
    // Verify all entities have correct components
    for (auto& entity : entities) {
        EXPECT_TRUE(entity.IsValid());
        EXPECT_TRUE(entity.Has<Position>());
        EXPECT_TRUE(entity.Has<Velocity>());

        auto* pos = entity.Get<Position>();
        auto* vel = entity.Get<Velocity>();
        ASSERT_NE(pos, nullptr);
        ASSERT_NE(vel, nullptr);

        EXPECT_FLOAT_EQ(pos->x, 5.0f);
        EXPECT_FLOAT_EQ(vel->x, 0.5f);
    }
    
    // Should only create one archetype for all entities
    EXPECT_EQ(world->GetArchetypeCount(), 1);
}

// Test dynamic creation correctness
TEST_F(EntityCreationTest, DynamicCreationCorrectness) {
    // Test ComponentBundle-based entity creation
    // This test verifies that createFromBundle() properly:
    // 1. Registers component factories in the World
    // 2. Creates archetype with proper component arrays
    // 3. Sets component data correctly on the entity

    ComponentBundle bundle;
    bundle.Add(Position{10.0f, 11.0f, 12.0f})
          .Add(Velocity{1.0f, 1.1f, 1.2f});

    auto entity = world->CreateFromBundle(bundle);
    world->ProcessCommands();

    // Verify entity was created successfully with components
    EXPECT_TRUE(entity.IsValid());
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());

    // Verify component values are correct
    auto* pos = entity.Get<Position>();
    ASSERT_NE(pos, nullptr);
    EXPECT_FLOAT_EQ(pos->x, 10.0f);
    EXPECT_FLOAT_EQ(pos->y, 11.0f);
    EXPECT_FLOAT_EQ(pos->z, 12.0f);

    auto* vel = entity.Get<Velocity>();
    ASSERT_NE(vel, nullptr);
    EXPECT_FLOAT_EQ(vel->x, 1.0f);
    EXPECT_FLOAT_EQ(vel->y, 1.1f);
    EXPECT_FLOAT_EQ(vel->z, 1.2f);
}

// Performance comparison test
TEST_F(EntityCreationTest, PerformanceComparison) {
    const size_t entityCount = 1000;
    
    Logger::Log::Info("[Performance Test] Creating {} entities", entityCount);
    
    // Method 1: Traditional creation (baseline)
    {
        auto testWorld = std::make_unique<World>();
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < entityCount; ++i) {
            auto handle = testWorld->CreateEntity();
            Entity entity(testWorld.get(), handle);
            entity.Set(Position{static_cast<float>(i), 0.0f, 0.0f});
            entity.Set(Velocity{1.0f, 0.0f, 0.0f});
        }

        // Process commands with safety check
        auto commandStart = std::chrono::high_resolution_clock::now();
        testWorld->ProcessCommands();
        auto commandEnd = std::chrono::high_resolution_clock::now();
        auto commandDuration = std::chrono::duration_cast<std::chrono::milliseconds>(commandEnd - commandStart);

        // Safety check: if command processing takes too long, something is wrong
        ASSERT_LT(commandDuration.count(), 5000) << "Command processing took too long: " << commandDuration.count() << "ms";
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        Logger::Log::Info("Traditional creation: {} μs ({} μs/entity)", 
                         duration.count(), duration.count() / entityCount);
    }
    
    // Method 2: New template-based batch creation
    {
        auto testWorld = std::make_unique<World>();
        auto start = std::chrono::high_resolution_clock::now();

        auto entities = testWorld->CreateBatch<Position, Velocity>(entityCount,
            Position{0.0f, 0.0f, 0.0f},
            Velocity{1.0f, 0.0f, 0.0f}
        );

        // Process commands with safety check
        auto commandStart = std::chrono::high_resolution_clock::now();
        testWorld->ProcessCommands();
        auto commandEnd = std::chrono::high_resolution_clock::now();
        auto commandDuration = std::chrono::duration_cast<std::chrono::milliseconds>(commandEnd - commandStart);

        // Safety check: if command processing takes too long, something is wrong
        ASSERT_LT(commandDuration.count(), 5000) << "Template batch command processing took too long: " << commandDuration.count() << "ms";
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        Logger::Log::Info("Template batch creation: {} μs ({} μs/entity)", 
                         duration.count(), duration.count() / entityCount);
        
        EXPECT_EQ(entities.size(), entityCount);
        EXPECT_EQ(testWorld->GetEntityCount(), entityCount);
    }
    
    // Method 3: Dynamic bundle creation
    {
        auto testWorld = std::make_unique<World>();
        ComponentBundle bundle;
        bundle.Add(Position{0.0f, 0.0f, 0.0f})
              .Add(Velocity{1.0f, 0.0f, 0.0f});

        auto start = std::chrono::high_resolution_clock::now();

        auto entities = testWorld->CreateBatchFromBundle(entityCount, bundle);

        // Process commands with safety check
        auto commandStart = std::chrono::high_resolution_clock::now();
        testWorld->ProcessCommands();
        auto commandEnd = std::chrono::high_resolution_clock::now();
        auto commandDuration = std::chrono::duration_cast<std::chrono::milliseconds>(commandEnd - commandStart);

        // Safety check: if command processing takes too long, something is wrong
        ASSERT_LT(commandDuration.count(), 5000) << "Bundle batch command processing took too long: " << commandDuration.count() << "ms";

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        Logger::Log::Info("Dynamic bundle creation: {} μs ({} μs/entity)",
                         duration.count(), duration.count() / entityCount);

        EXPECT_EQ(entities.size(), entityCount);
        EXPECT_EQ(testWorld->GetEntityCount(), entityCount);
    }
}

// Test archetype efficiency
TEST_F(EntityCreationTest, ArchetypeEfficiency) {
    // Create entities with different component combinations
    auto entity1 = world->Create<Position>(Position{1, 2, 3});
    auto entity2 = world->Create<Position, Velocity>(Position{4, 5, 6}, Velocity{1, 0, 0});
    auto entity3 = world->Create<Position>(Position{7, 8, 9}); // Should reuse first archetype
    
    world->ProcessCommands();
    
    // Should only create 2 archetypes (Position) and (Position, Velocity)
    EXPECT_EQ(world->GetArchetypeCount(), 2);
    
    // Verify entities are in correct archetypes
    EXPECT_TRUE(entity1.Has<Position>());
    EXPECT_FALSE(entity1.Has<Velocity>());
    
    EXPECT_TRUE(entity2.Has<Position>());
    EXPECT_TRUE(entity2.Has<Velocity>());
    
    EXPECT_TRUE(entity3.Has<Position>());
    EXPECT_FALSE(entity3.Has<Velocity>());
}

// Test createInArchetype methods
TEST_F(EntityCreationTest, CreateInArchetypeTest) {
    // Check initial state - should be 0 archetypes in fresh world
    size_t initialCount = world->GetArchetypeCount();
    EXPECT_EQ(initialCount, 0) << "Fresh world should have 0 archetypes";

    // Create entity in archetype without setting components
    auto entity = world->CreateInArchetype<Position, Velocity>();
    world->ProcessCommands();

    size_t afterCreateCount = world->GetArchetypeCount();

    // Entity should be valid and in correct archetype
    EXPECT_TRUE(entity.IsValid());

    // Analysis: createInArchetype<Position, Velocity>() creates exactly 1 archetype
    // - The method creates a ComponentSignature with Position and Velocity type IDs
    // - getOrCreateArchetype() creates exactly one archetype for this unique signature
    // - No intermediate archetypes are created during this process
    EXPECT_EQ(afterCreateCount, 1) << "createInArchetype should create exactly 1 archetype for Position+Velocity signature";
    
    // Components should exist but have default values
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());
    
    // Now set components - this should NOT create new archetypes since entity is already in Position+Velocity archetype
    entity.Set(Position{100.0f, 200.0f, 300.0f});
    entity.Set(Velocity{10.0f, 20.0f, 30.0f});
    world->ProcessCommands();

    size_t afterSetCount = world->GetArchetypeCount();

    // Verify values were set correctly
    auto* pos = entity.Get<Position>();
    auto* vel = entity.Get<Velocity>();
    ASSERT_NE(pos, nullptr);
    ASSERT_NE(vel, nullptr);

    EXPECT_FLOAT_EQ(pos->x, 100.0f);
    EXPECT_FLOAT_EQ(vel->x, 10.0f);

    // Critical test: Setting components on an entity already in the correct archetype should NOT create new archetypes
    // The entity was created in Position+Velocity archetype, so setting Position and Velocity should not cause migration
    // Root cause of previous failure: ComponentSignature::add() was allowing duplicates, creating invalid signatures
    // Fix: ComponentSignature::add() now checks for duplicates before adding component type IDs
    EXPECT_EQ(afterSetCount, 1) << "Setting components on entity already in correct archetype should not create new archetypes";

    // Final verification: archetype count should remain stable throughout the test
    EXPECT_EQ(afterSetCount, afterCreateCount) << "Archetype count should not change when setting components on entity in correct archetype";
}

// Entity-index recycling is observable through handle reuse: destroyed indices
// return in strict LIFO order (last destroyed is recycled first). Pins the
// freeIndices container's ordering contract.
TEST_F(EntityCreationTest, FreeIndexRecyclingIsLifo) {
    EntityHandle a = world->CreateEntity();
    EntityHandle b = world->CreateEntity();
    EntityHandle c = world->CreateEntity();

    world->DestroyEntityImmediate(a);
    world->DestroyEntityImmediate(b);
    world->DestroyEntityImmediate(c);

    EntityHandle r0 = world->CreateEntity();
    EntityHandle r1 = world->CreateEntity();
    EntityHandle r2 = world->CreateEntity();

    // LIFO: c's index first, then b's, then a's — each with a bumped version.
    EXPECT_EQ(r0.index, c.index);
    EXPECT_EQ(r1.index, b.index);
    EXPECT_EQ(r2.index, a.index);
    EXPECT_EQ(r0.version, c.version + 1);
    EXPECT_EQ(r1.version, b.version + 1);
    EXPECT_EQ(r2.version, a.version + 1);

    // Old handles must not resolve to the recycled entities.
    EXPECT_FALSE(world->IsValid(a));
    EXPECT_FALSE(world->IsValid(b));
    EXPECT_FALSE(world->IsValid(c));
    EXPECT_TRUE(world->IsValid(r0));
}
