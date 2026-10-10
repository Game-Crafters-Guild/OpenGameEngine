#include <gtest/gtest.h>
#include "ECS/ECS.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/AutoRegistration.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"

// Test components for the unified system
namespace UnifiedSystemTest {

// Test component that should work with the unified system
struct TestComponent {
    int value = 42;
    float data = 3.14f;
};

// Another test component
struct AnotherComponent {
    bool flag = true;
    char name[16] = "test";
};

// Test that the Engine's transform components work
struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

} // namespace UnifiedSystemTest

using namespace GameEngine::ECS;
namespace UT = UnifiedSystemTest;

// Explicit template instantiations for Query templates with custom components
namespace GameEngine::ECS {
    template class Query<UnifiedSystemTest::TestComponent>;
    template class Query<UnifiedSystemTest::Position>;
    template class Query<UnifiedSystemTest::TestComponent, UnifiedSystemTest::Position>;
}

class ComponentSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        world = std::make_unique<World>();
    }

    void TearDown() override {
        world.reset();
    }

    std::unique_ptr<World> world;
};

// Test that the Component concept works for all component types
TEST_F(ComponentSystemTest, ComponentConceptValidation) {
    // Test that all components satisfy the Component concept
    static_assert(Component<UT::TestComponent>, "TestComponent should satisfy Component concept");
    static_assert(Component<UT::AnotherComponent>, "AnotherComponent should satisfy Component concept");
    static_assert(Component<UT::Position>, "Position should satisfy Component concept");
    static_assert(Component<Disabled>, "Disabled should satisfy Component concept");
    
    // Test that invalid types are rejected
    static_assert(!Component<int*>, "Pointer types should not satisfy Component concept");
    static_assert(!Component<void>, "void should not satisfy Component concept");
    
    SUCCEED(); // Test passes if static_asserts compile
}

// Test that auto-registration works for all components
TEST_F(ComponentSystemTest, UnifiedAutoRegistration) {
    // Create an entity
    EntityHandle handle = world->CreateEntity();
    Entity entity(world.get(), handle);

    // Test that we can add any component type using the unified system
    entity.Set(UT::TestComponent{100, 2.5f});
    entity.Set(UT::AnotherComponent{false, "hello"});
    entity.Set(UT::Position{1.0f, 2.0f, 3.0f});
    entity.Set(Disabled{});

    // Process any pending commands
    world->ProcessCommands();
    
    // Test that we can retrieve the components
    auto* testComp = entity.Get<UT::TestComponent>();
    auto* anotherComp = entity.Get<UT::AnotherComponent>();
    auto* posComp = entity.Get<UT::Position>();
    auto* disabledComp = entity.Get<Disabled>();
    
    ASSERT_NE(testComp, nullptr);
    ASSERT_NE(anotherComp, nullptr);
    ASSERT_NE(posComp, nullptr);
    ASSERT_NE(disabledComp, nullptr);
    
    // Verify component data
    EXPECT_EQ(testComp->value, 100);
    EXPECT_FLOAT_EQ(testComp->data, 2.5f);
    EXPECT_FALSE(anotherComp->flag);
    EXPECT_STREQ(anotherComp->name, "hello");
    EXPECT_FLOAT_EQ(posComp->x, 1.0f);
    EXPECT_FLOAT_EQ(posComp->y, 2.0f);
    EXPECT_FLOAT_EQ(posComp->z, 3.0f);
}

// Test that component registration works uniformly
TEST_F(ComponentSystemTest, UniformRegistration) {
    // Manually trigger registration for all component types
    AutoComponentRegistrar<UT::TestComponent>::EnsureRegistered();
    AutoComponentRegistrar<UT::AnotherComponent>::EnsureRegistered();
    AutoComponentRegistrar<UT::Position>::EnsureRegistered();
    AutoComponentRegistrar<Disabled>::EnsureRegistered();

    SUCCEED(); // Test passes if no exceptions are thrown
}

// Test that the system works with queries
TEST_F(ComponentSystemTest, QueryIntegration) {
    // Create entities with different component combinations
    EntityHandle handle1 = world->CreateEntity();
    EntityHandle handle2 = world->CreateEntity();
    EntityHandle handle3 = world->CreateEntity();
    Entity entity1(world.get(), handle1);
    Entity entity2(world.get(), handle2);
    Entity entity3(world.get(), handle3);
    
    entity1.Set(UT::TestComponent{1, 1.0f}).Set(UT::Position{1.0f, 0.0f, 0.0f});
    entity2.Set(UT::TestComponent{2, 2.0f}).Set(UT::AnotherComponent{true, "test2"});
    entity3.Set(UT::Position{3.0f, 0.0f, 0.0f}).Set(UT::AnotherComponent{false, "test3"});

    world->ProcessCommands();
    
    // Test queries with the unified component system
    auto query1 = world->Query<UT::TestComponent>();
    auto query2 = world->Query<UT::Position>();
    auto query3 = world->Query<UT::TestComponent, UT::Position>();
    
    // Count entities in each query
    int count1 = 0, count2 = 0, count3 = 0;
    
    query1.Each([&count1](EntityHandle, UT::TestComponent&) {
        count1++;
    });

    query2.Each([&count2](EntityHandle, UT::Position&) {
        count2++;
    });

    query3.Each([&count3](EntityHandle, UT::TestComponent&, UT::Position&) {
        count3++;
    });
    
    EXPECT_EQ(count1, 2); // entity1 and entity2 have TestComponent
    EXPECT_EQ(count2, 2); // entity1 and entity3 have Position
    EXPECT_EQ(count3, 1); // only entity1 has both TestComponent and Position
}
