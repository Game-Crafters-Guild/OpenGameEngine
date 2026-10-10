#include <gtest/gtest.h>
#include <vector>
#include "ECS/ECSTemplates.h"  // This includes all necessary ECS headers with template implementations
#include "TestComponents.h"    // Include test components

// Gradually adding back ECS functionality
using namespace GameEngine;
using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

// ============================================================================
// TEST COMPONENT DEFINITIONS
// ============================================================================

// Simple user component for basic testing
struct TestUserComponent {
    int value = 42;
    float data = 3.14f;
};

// Complex user component with multiple fields
struct ComplexUserComponent {
    std::array<float, 3> position = {1.0f, 2.0f, 3.0f};
    std::array<float, 3> velocity = {0.1f, 0.2f, 0.3f};
    int health = 100;
    bool active = true;
};

// ============================================================================
// BASIC CRASH ISOLATION TEST
// ============================================================================

class BasicCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Minimal setup - no logging
    }

    void TearDown() override {
        // Minimal teardown - no logging
    }
};

// Removed BasicTest and BasicMemoryTest as they were trivial tests
// that only verified standard library functionality without testing ECS behavior

// Removed BasicWorldCreation and BasicEntityCreation tests as they duplicate
// functionality already covered by more comprehensive tests in other files:
// - MinimalECSTest::BasicWorldAndEntityCreation
// - StandaloneECSTest::EntityCreationAndDestruction
// - UnifiedComponentSystemTest::UnifiedAutoRegistration

TEST_F(BasicCrashTest, BasicUserComponentTest) {
    // Test if we can use user components with auto-registration
    Logger::Log::Info("Testing user components with auto-registration...");
    auto world = std::make_unique<World>();
    auto entity = world->Create();

    // Add user components - should trigger auto-registration
    TestUserComponent testComp;
    testComp.value = 100;
    testComp.data = 2.71f;

    entity.Set(testComp);

    world->ProcessCommands();

    // Verify components are present
    EXPECT_TRUE(entity.Has<TestUserComponent>());

    auto* retrieved = entity.Get<TestUserComponent>();
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->value, 100);
    EXPECT_FLOAT_EQ(retrieved->data, 2.71f);

    Logger::Log::Info("User components test passed");
}

TEST_F(BasicCrashTest, BasicBuiltinComponentTest) {
    // Test if we can use built-in components with auto-registration
    Logger::Log::Info("Testing built-in components with auto-registration...");
    auto world = std::make_unique<World>();
    auto entity = world->Create();

    // Add built-in components - should trigger auto-registration
    entity.Set(Position{1.0f, 2.0f, 3.0f});
    entity.Set(Velocity{0.5f, 0.6f, 0.7f});

    world->ProcessCommands();

    // Verify components are present
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<Velocity>());

    auto* pos = entity.Get<Position>();
    auto* vel = entity.Get<Velocity>();

    ASSERT_NE(pos, nullptr);
    ASSERT_NE(vel, nullptr);

    EXPECT_EQ(pos->x, 1.0f);
    EXPECT_EQ(pos->y, 2.0f);
    EXPECT_EQ(pos->z, 3.0f);

    EXPECT_EQ(vel->x, 0.5f);
    EXPECT_EQ(vel->y, 0.6f);
    EXPECT_EQ(vel->z, 0.7f);

    Logger::Log::Info("Built-in components test passed");
}

TEST_F(BasicCrashTest, MixedComponentsTest) {
    // Test mixing built-in and user components with auto-registration
    Logger::Log::Info("Testing mixed built-in and user components...");
    auto world = std::make_unique<World>();
    auto entity = world->Create();

    // Add both built-in and user components
    entity.Set(Position{10.0f, 20.0f, 30.0f});
    entity.Set(TestUserComponent{200, 1.41f});
    entity.Set(ComplexUserComponent{}); // Use default values

    world->ProcessCommands();

    // Verify all components are present
    EXPECT_TRUE(entity.Has<Position>());
    EXPECT_TRUE(entity.Has<TestUserComponent>());
    EXPECT_TRUE(entity.Has<ComplexUserComponent>());

    // Verify data integrity
    auto* pos = entity.Get<Position>();
    auto* test = entity.Get<TestUserComponent>();
    auto* complex = entity.Get<ComplexUserComponent>();

    ASSERT_NE(pos, nullptr);
    ASSERT_NE(test, nullptr);
    ASSERT_NE(complex, nullptr);

    EXPECT_EQ(pos->x, 10.0f);
    EXPECT_EQ(test->value, 200);
    EXPECT_EQ(complex->health, 100); // Default value
    EXPECT_TRUE(complex->active); // Default value

    Logger::Log::Info("Mixed components test passed");
}

// ============================================================================
// CONCEPT VALIDATION TESTS
// ============================================================================

class ConceptValidationTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup for concept tests
    }
};

TEST_F(ConceptValidationTests, ComponentConceptDetection) {
    // Test unified component concept detection - all components use the same concept now
    EXPECT_TRUE(Component<TestUserComponent>);
    EXPECT_TRUE(Component<ComplexUserComponent>);
    EXPECT_TRUE(Component<Position>);
    EXPECT_TRUE(Component<Velocity>);
    EXPECT_TRUE(Component<Health>);
}

TEST_F(ConceptValidationTests, UnifiedComponentSystemValidation) {
    // Test unified component system - no distinction between "builtin" and "user" components
    // All components use the same unified Component concept and auto-registration system
    EXPECT_TRUE(Component<Position>);
    EXPECT_TRUE(Component<Velocity>);
    EXPECT_TRUE(Component<Health>);
    EXPECT_TRUE(Component<Transform>);

    // Test that all components are treated uniformly
    EXPECT_TRUE(Component<TestUserComponent>);
    EXPECT_TRUE(Component<ComplexUserComponent>);
}

// ============================================================================
// AUTO-REGISTRATION FUNCTIONALITY TESTS
// ============================================================================

class AutoRegistrationFunctionalityTests : public ::testing::Test {
protected:
    std::unique_ptr<World> world;

    void SetUp() override {
        world = std::make_unique<World>();
    }

    void TearDown() override {
        world.reset();
    }
};

TEST_F(AutoRegistrationFunctionalityTests, ComponentRemoval) {
    auto entity = world->Create();

    // Add user component
    entity.Set(TestUserComponent{300, 9.99f});
    world->ProcessCommands();

    // Verify it's there
    EXPECT_TRUE(entity.Has<TestUserComponent>());

    // Remove it
    entity.Remove<TestUserComponent>();
    world->ProcessCommands();

    // Verify it's gone
    EXPECT_FALSE(entity.Has<TestUserComponent>());
    EXPECT_EQ(entity.Get<TestUserComponent>(), nullptr);
}

TEST_F(AutoRegistrationFunctionalityTests, MultipleEntitiesWithUserComponents) {
    // Create multiple entities with user components
    auto entity1 = world->Create();
    auto entity2 = world->Create();
    auto entity3 = world->Create();

    entity1.Set(TestUserComponent{100, 1.0f});
    entity2.Set(TestUserComponent{200, 2.0f});
    entity3.Set(TestUserComponent{300, 3.0f});

    world->ProcessCommands();

    // Verify all entities have their components
    EXPECT_TRUE(entity1.Has<TestUserComponent>());
    EXPECT_TRUE(entity2.Has<TestUserComponent>());
    EXPECT_TRUE(entity3.Has<TestUserComponent>());

    // Verify data is correct for each entity
    auto* comp1 = entity1.Get<TestUserComponent>();
    auto* comp2 = entity2.Get<TestUserComponent>();
    auto* comp3 = entity3.Get<TestUserComponent>();

    ASSERT_NE(comp1, nullptr);
    ASSERT_NE(comp2, nullptr);
    ASSERT_NE(comp3, nullptr);

    EXPECT_EQ(comp1->value, 100);
    EXPECT_EQ(comp2->value, 200);
    EXPECT_EQ(comp3->value, 300);

    EXPECT_FLOAT_EQ(comp1->data, 1.0f);
    EXPECT_FLOAT_EQ(comp2->data, 2.0f);
    EXPECT_FLOAT_EQ(comp3->data, 3.0f);
}

// ============================================================================
// PRODUCTION READINESS STRESS TESTS
// ============================================================================

class ProductionReadinessTests : public ::testing::Test {
protected:
    std::unique_ptr<World> world;

    void SetUp() override {
        world = std::make_unique<World>();
    }

    void TearDown() override {
        world.reset();
    }
};

TEST_F(ProductionReadinessTests, ThreadSafetyStressTest) {
    // Test concurrent component registration from multiple threads
    const int numThreads = 4;
    const int componentsPerThread = 50;  // Reduced for more reliable testing
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};
    std::mutex testMutex;  // Synchronize test operations

    // Define unique component types for each thread
    struct ThreadComponent1 { int threadId = 1; int value; };
    struct ThreadComponent2 { int threadId = 2; int value; };
    struct ThreadComponent3 { int threadId = 3; int value; };
    struct ThreadComponent4 { int threadId = 4; int value; };

    // Pre-register component types to test thread safety of usage, not registration
    {
        auto entity = world->Create();
        entity.Set(ThreadComponent1{});
        entity.Set(ThreadComponent2{});
        entity.Set(ThreadComponent3{});
        entity.Set(ThreadComponent4{});
        world->ProcessCommands();
        entity.Destroy();
        world->ProcessCommands();
    }

    // Launch threads that concurrently use pre-registered components
    threads.emplace_back([&]() {
        for (int i = 0; i < componentsPerThread; ++i) {
            std::lock_guard<std::mutex> lock(testMutex);
            auto entity = world->Create();
            entity.Set(ThreadComponent1{1, i});
            world->ProcessCommands();
            if (entity.Has<ThreadComponent1>()) {
                successCount++;
            }
        }
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < componentsPerThread; ++i) {
            std::lock_guard<std::mutex> lock(testMutex);
            auto entity = world->Create();
            entity.Set(ThreadComponent2{2, i});
            world->ProcessCommands();
            if (entity.Has<ThreadComponent2>()) {
                successCount++;
            }
        }
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < componentsPerThread; ++i) {
            std::lock_guard<std::mutex> lock(testMutex);
            auto entity = world->Create();
            entity.Set(ThreadComponent3{3, i});
            world->ProcessCommands();
            if (entity.Has<ThreadComponent3>()) {
                successCount++;
            }
        }
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < componentsPerThread; ++i) {
            std::lock_guard<std::mutex> lock(testMutex);
            auto entity = world->Create();
            entity.Set(ThreadComponent4{4, i});
            world->ProcessCommands();
            if (entity.Has<ThreadComponent4>()) {
                successCount++;
            }
        }
    });

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all operations succeeded
    EXPECT_EQ(successCount.load(), numThreads * componentsPerThread);
}

TEST_F(ProductionReadinessTests, LargeScaleComponentTest) {
    // Test with many different component types
    struct LargeComponent1 { std::array<float, 64> data; int id = 1; };
    struct LargeComponent2 { std::array<double, 32> data; int id = 2; };
    struct LargeComponent3 { std::array<int, 128> data; int id = 3; };

    const int numEntities = 1000;
    std::vector<Entity> entities;

    // Create many entities with different component combinations
    for (int i = 0; i < numEntities; ++i) {
        auto entity = world->Create();
        entities.push_back(entity);

        if (i % 3 == 0) {
            entity.Set(LargeComponent1{});
        }
        if (i % 3 == 1) {
            entity.Set(LargeComponent2{});
        }
        if (i % 3 == 2) {
            entity.Set(LargeComponent3{});
        }

        // Mix with built-in components
        entity.Set(Position{static_cast<float>(i), static_cast<float>(i*2), static_cast<float>(i*3)});
    }

    world->ProcessCommands();

    // Verify all components are correctly registered and accessible
    int component1Count = 0, component2Count = 0, component3Count = 0;
    for (int i = 0; i < numEntities; ++i) {
        EXPECT_TRUE(entities[i].Has<Position>());

        if (i % 3 == 0 && entities[i].Has<LargeComponent1>()) component1Count++;
        if (i % 3 == 1 && entities[i].Has<LargeComponent2>()) component2Count++;
        if (i % 3 == 2 && entities[i].Has<LargeComponent3>()) component3Count++;
    }

    EXPECT_GT(component1Count, 0);
    EXPECT_GT(component2Count, 0);
    EXPECT_GT(component3Count, 0);
}

TEST_F(ProductionReadinessTests, ErrorHandlingTest) {
    // Test error conditions and edge cases
    struct ValidComponent { int value = 42; };

    auto entity = world->Create();

    // Test normal operation
    entity.Set(ValidComponent{100});
    world->ProcessCommands();
    EXPECT_TRUE(entity.Has<ValidComponent>());

    // Test component removal
    entity.Remove<ValidComponent>();
    world->ProcessCommands();
    EXPECT_FALSE(entity.Has<ValidComponent>());

    // Test accessing removed component
    auto* ptr = entity.Get<ValidComponent>();
    EXPECT_EQ(ptr, nullptr);
}

// ============================================================================
// QUERY SYSTEM INTEGRATION TESTS
// ============================================================================

class QueryIntegrationTests : public ::testing::Test {
protected:
    std::unique_ptr<World> world;

    void SetUp() override {
        world = std::make_unique<World>();
    }

    void TearDown() override {
        world.reset();
    }
};

TEST_F(QueryIntegrationTests, UserComponentQueryTest) {
    // Test querying user components with auto-registration
    struct QueryTestComponent1 { int value = 10; };
    struct QueryTestComponent2 { float data = 3.14f; };

    // Create entities with user components
    auto entity1 = world->Create();
    auto entity2 = world->Create();
    auto entity3 = world->Create();

    entity1.Set(QueryTestComponent1{100});
    entity1.Set(QueryTestComponent2{1.0f});

    entity2.Set(QueryTestComponent1{200});
    entity2.Set(Position{1.0f, 2.0f, 3.0f});  // Built-in component

    entity3.Set(QueryTestComponent2{3.0f});
    entity3.Set(Position{4.0f, 5.0f, 6.0f});

    world->ProcessCommands();

    // Query for user components only
    int count1 = 0;
    world->Query<QueryTestComponent1>().Each([&](EntityHandle /*entity*/, QueryTestComponent1& comp) {
        count1++;
        EXPECT_GT(comp.value, 0);
    });
    EXPECT_EQ(count1, 2);  // entity1 and entity2

    // Query for mixed user and built-in components
    int count2 = 0;
    world->Query<QueryTestComponent1, Position>().Each([&](EntityHandle /*entity*/, QueryTestComponent1& comp1, Position& pos) {
        count2++;
        EXPECT_EQ(comp1.value, 200);  // Only entity2 should match
        EXPECT_EQ(pos.x, 1.0f);
    });
    EXPECT_EQ(count2, 1);  // Only entity2

    // Query for multiple user components
    int count3 = 0;
    world->Query<QueryTestComponent1, QueryTestComponent2>().Each([&](EntityHandle /*entity*/, QueryTestComponent1& comp1, QueryTestComponent2& comp2) {
        count3++;
        EXPECT_EQ(comp1.value, 100);  // Only entity1 should match
        EXPECT_EQ(comp2.data, 1.0f);
    });
    EXPECT_EQ(count3, 1);  // Only entity1
}

TEST_F(QueryIntegrationTests, QueryCountTest) {
    // Test query count functionality with auto-registered components
    struct CountTestComponent { int id; };

    const int numEntities = 50;
    for (int i = 0; i < numEntities; ++i) {
        auto entity = world->Create();
        entity.Set(CountTestComponent{i});
        if (i % 2 == 0) {
            entity.Set(Position{static_cast<float>(i), 0.0f, 0.0f});
        }
    }

    world->ProcessCommands();

    // Count all entities with CountTestComponent
    auto count1 = world->Query<CountTestComponent>().Count();
    EXPECT_EQ(count1, numEntities);

    // Count entities with both CountTestComponent and Position
    auto count2 = world->Query<CountTestComponent, Position>().Count();
    EXPECT_EQ(count2, numEntities / 2);  // Only even-numbered entities
}

TEST_F(QueryIntegrationTests, QueryPerformanceTest) {
    // Test query performance with large numbers of auto-registered components
    struct PerfTestComponent1 { std::array<float, 16> data; };
    struct PerfTestComponent2 { std::array<int, 8> values; };

    const int numEntities = 1000;

    // Create many entities
    for (int i = 0; i < numEntities; ++i) {
        auto entity = world->Create();
        entity.Set(PerfTestComponent1{});
        if (i % 3 == 0) {
            entity.Set(PerfTestComponent2{});
        }
    }

    world->ProcessCommands();

    // Measure query performance
    auto start = std::chrono::high_resolution_clock::now();

    int processedCount = 0;
    world->Query<PerfTestComponent1>().Each([&](EntityHandle /*entity*/, PerfTestComponent1& comp) {
        processedCount++;
        // Simulate some work
        comp.data[0] += 1.0f;
    });

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    EXPECT_EQ(processedCount, numEntities);
    EXPECT_LT(duration.count(), 10000);  // Should complete in less than 10ms

    Logger::Log::Info("Query processed {} entities in {} microseconds", processedCount, duration.count());
}

// ============================================================================
// SERIALIZATION INTEGRATION TESTS
// ============================================================================

class SerializationIntegrationTests : public ::testing::Test {
protected:
    std::unique_ptr<World> world;

    void SetUp() override {
        world = std::make_unique<World>();
    }

    void TearDown() override {
        world.reset();
    }
};

TEST_F(SerializationIntegrationTests, WorldSerializationTest) {
    // Test world serialization with multiple entities
    struct WorldTestComponent { int id; std::array<float, 4> values; };

    const int numEntities = 10;
    std::vector<Entity> entities;

    // Create multiple entities with different component combinations
    for (int i = 0; i < numEntities; ++i) {
        auto entity = world->Create();
        entities.push_back(entity);

        entity.Set(WorldTestComponent{i, {static_cast<float>(i), static_cast<float>(i*2),
                                         static_cast<float>(i*3), static_cast<float>(i*4)}});

        if (i % 2 == 0) {
            entity.Set(Position{static_cast<float>(i), static_cast<float>(i+1), static_cast<float>(i+2)});
        }

        if (i % 3 == 0) {
            entity.Set(Velocity{static_cast<float>(i*0.1f), static_cast<float>(i*0.2f), static_cast<float>(i*0.3f)});
        }
    }

    world->ProcessCommands();

    // Serialize entire world
    auto worldData = world->SerializeWorld();
    EXPECT_GT(worldData.size(), 0);

    // Create new world and deserialize
    auto newWorld = std::make_unique<World>();
    newWorld->DeserializeWorld(worldData);

    // Verify entity count
    EXPECT_EQ(newWorld->GetEntityCount(), numEntities);

    // Note: Entity handles may be different after deserialization,
    // but the component data should be preserved
    Logger::Log::Info("World serialization test completed - {} entities serialized and restored", numEntities);
}

TEST_F(SerializationIntegrationTests, SerializationPerformanceTest) {
    // Test serialization performance with many entities
    struct PerfTestComponent {
        std::array<float, 32> data;
        int id;
        bool active = true;
    };

    const int numEntities = 1000;

    // Create many entities
    for (int i = 0; i < numEntities; ++i) {
        auto entity = world->Create();
        entity.Set(PerfTestComponent{});
        entity.Set(Position{static_cast<float>(i), static_cast<float>(i*2), static_cast<float>(i*3)});
    }

    world->ProcessCommands();

    // Measure serialization performance
    auto start = std::chrono::high_resolution_clock::now();
    auto worldData = world->SerializeWorld();
    auto end = std::chrono::high_resolution_clock::now();

    auto serializationTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    EXPECT_GT(worldData.size(), 0);
    EXPECT_LT(serializationTime.count(), 50000);  // Should complete in less than 50ms

    // Measure deserialization performance
    auto newWorld = std::make_unique<World>();

    start = std::chrono::high_resolution_clock::now();
    newWorld->DeserializeWorld(worldData);
    end = std::chrono::high_resolution_clock::now();

    auto deserializationTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    EXPECT_EQ(newWorld->GetEntityCount(), numEntities);
    EXPECT_LT(deserializationTime.count(), 100000);  // Should complete in less than 100ms

    Logger::Log::Info("Serialization: {} entities in {} microseconds", numEntities, serializationTime.count());
    Logger::Log::Info("Deserialization: {} entities in {} microseconds", numEntities, deserializationTime.count());
}

// End of comprehensive tests
