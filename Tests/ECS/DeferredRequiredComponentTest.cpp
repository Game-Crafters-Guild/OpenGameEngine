// DeferredRequiredComponentTest.cpp - a deferred add queues the components its
// type requires; at the flush a requirement only fills a gap. It never replaces
// a value the entity has, including one queued earlier in the same flush.
#include <gtest/gtest.h>
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{
struct Placement
{
    float X = 0.0f;
};
struct NeedsPlacement
{
    int Value = 0;
};
} // namespace

namespace GameEngine::ECS::Detail
{
template <>
struct RequiredComponents<::NeedsPlacement>
{
    using type = TypeList<::Placement>;
};
} // namespace GameEngine::ECS::Detail

TEST(DeferredRequiredComponent, QueuedValueSurvivesALaterRequirement)
{
    World world;
    const EntityHandle entity = world.CreateEntity();

    world.AddComponent(entity, Placement{1024.0f});
    world.AddComponent(entity, NeedsPlacement{7});
    world.ProcessCommands();

    const auto* placement = world.GetComponent<Placement>(entity);
    ASSERT_NE(placement, nullptr);
    EXPECT_EQ(placement->X, 1024.0f) << "the requirement's default replaced the queued value";
    ASSERT_NE(world.GetComponent<NeedsPlacement>(entity), nullptr);
    EXPECT_EQ(world.GetComponent<NeedsPlacement>(entity)->Value, 7);
}

TEST(DeferredRequiredComponent, ValueQueuedAfterTheRequirementWins)
{
    World world;
    const EntityHandle entity = world.CreateEntity();

    world.AddComponent(entity, NeedsPlacement{7});
    world.AddComponent(entity, Placement{1024.0f});
    world.ProcessCommands();

    const auto* placement = world.GetComponent<Placement>(entity);
    ASSERT_NE(placement, nullptr);
    EXPECT_EQ(placement->X, 1024.0f);
}

TEST(DeferredRequiredComponent, RequirementRemovedEarlierInTheFlushIsAddedBack)
{
    World world;
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, Placement{1024.0f});

    world.RemoveComponent<Placement>(entity);
    world.AddComponent(entity, NeedsPlacement{7});
    world.ProcessCommands();

    const auto* placement = world.GetComponent<Placement>(entity);
    ASSERT_NE(placement, nullptr) << "the requirement was decided before the queued removal landed";
    EXPECT_EQ(placement->X, 0.0f);
}

TEST(DeferredRequiredComponent, MissingRequirementIsAddedWithItsDefault)
{
    World world;
    const EntityHandle entity = world.CreateEntity();

    world.AddComponent(entity, NeedsPlacement{7});
    world.ProcessCommands();

    const auto* placement = world.GetComponent<Placement>(entity);
    ASSERT_NE(placement, nullptr);
    EXPECT_EQ(placement->X, 0.0f);
}
