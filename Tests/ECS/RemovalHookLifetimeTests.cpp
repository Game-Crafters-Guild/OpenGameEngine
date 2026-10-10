#include <gtest/gtest.h>
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"

namespace
{
using namespace GameEngine::ECS;
struct OwnedResource { int Value = 0; };
struct OtherResource { int Value = 0; };
int released = 0, replacementReleased = 0;
EntityHandle releasedEntity{};
void Release(OwnedResource& value) { released += value.Value; }
void ReplacementRelease(OwnedResource& value) { replacementReleased += value.Value; }
void ReleaseWithEntity(EntityHandle entity, OwnedResource& value)
{
    releasedEntity = entity;
    released += value.Value;
}
void ReleaseOther(OtherResource& value) { replacementReleased += value.Value; }
class RemovalHookLifetime : public testing::Test
{
    void SetUp() override { released = replacementReleased = 0; releasedEntity = {}; }
};

TEST_F(RemovalHookLifetime, ImagePinsIncludeEveryLiveWorldUntilUnregistered)
{
    const auto address = reinterpret_cast<std::uintptr_t>(&Release);
    World first(nullptr), second(nullptr);
    const auto firstHook = first.RegisterOnRemove<OwnedResource>(Release);
    const auto secondHook = second.RegisterOnRemove<OwnedResource>(Release);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 2u);
    first.UnregisterComponentHook(firstHook);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 1u);
    second.UnregisterComponentHook(secondHook);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 0u);
}

TEST_F(RemovalHookLifetime, ImagePinsFollowReplacementAndWorldDestruction)
{
    const auto oldAddress = reinterpret_cast<std::uintptr_t>(&Release);
    const auto newAddress = reinterpret_cast<std::uintptr_t>(&ReplacementRelease);
    {
        World world(nullptr);
        world.RegisterOnRemove<OwnedResource>(Release);
        world.RegisterOnRemove<OwnedResource>(ReplacementRelease);
        EXPECT_EQ(World::CountComponentHooksOwnedByImage(oldAddress, 1), 0u);
        EXPECT_EQ(World::CountComponentHooksOwnedByImage(newAddress, 1), 1u);
        world.Clear(); // Clear does not unregister hooks.
        EXPECT_EQ(World::CountComponentHooksOwnedByImage(newAddress, 1), 1u);
    }
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(newAddress, 1), 0u);
}

TEST_F(RemovalHookLifetime, ImagePinsIncludeAddAndSetHooksAndCountEachSlotOnce)
{
    World world(nullptr);
    world.RegisterOnRemove<OwnedResource>(Release);
    world.RegisterOnAdd<OwnedResource>(Release);
    world.RegisterOnSet<OwnedResource>(Release);
    const auto address = reinterpret_cast<std::uintptr_t>(&Release);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 3u);
    // Both each thunk and its callback are in this range, but each slot pins once.
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(1, UINT64_MAX), 3u);
}

TEST_F(RemovalHookLifetime, ImageRangesAreHalfOpenAndDoNotWrap)
{
    World world(nullptr);
    world.RegisterOnRemove<OwnedResource>(Release);
    const auto address = reinterpret_cast<std::uintptr_t>(&Release);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address - 1, 1), 0u);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address - 1, 2), 1u);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 0), 0u);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(0, UINT64_MAX), 0u);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(UINT64_MAX - 1, 10), 0u);
}

TEST_F(RemovalHookLifetime, RemovingHookRefreshesExistingAndFutureArchetypes)
{
    World world(nullptr);
    const auto hook = world.RegisterOnRemove<OwnedResource>(Release);
    auto first = world.Create(OwnedResource{3});
    world.ProcessCommands();
    EXPECT_TRUE(world.UnregisterComponentHook(hook));
    EXPECT_FALSE(world.HasOnRemoveHook(GetComponentTypeId<OwnedResource>()));
    first.Destroy();
    auto second = world.Create(OwnedResource{5}, OtherResource{7});
    world.ProcessCommands();
    second.Destroy();
    world.ProcessCommands();
    EXPECT_EQ(released, 0);
}

TEST_F(RemovalHookLifetime, OldOwnerCannotUnregisterReplacement)
{
    World world(nullptr);
    const auto hook = world.RegisterOnRemove<OwnedResource>(Release);
    world.RegisterOnRemove<OwnedResource>(ReplacementRelease);
    EXPECT_FALSE(world.UnregisterComponentHook(hook));
    auto entity = world.Create(OwnedResource{3});
    world.ProcessCommands();
    entity.Destroy();
    world.ProcessCommands();
    EXPECT_EQ(released, 0);
    EXPECT_EQ(replacementReleased, 3);
}

TEST_F(RemovalHookLifetime, RepeatedUnregistrationIsHarmless)
{
    World world(nullptr);
    EXPECT_FALSE(world.UnregisterComponentHook({}));
    const auto hook = world.RegisterOnRemove<OwnedResource>(Release);
    EXPECT_TRUE(world.UnregisterComponentHook(hook));
    EXPECT_FALSE(world.UnregisterComponentHook(hook));
}

TEST_F(RemovalHookLifetime, OtherComponentsRetainTheirRemovalHooks)
{
    World world(nullptr);
    const auto hook = world.RegisterOnRemove<OwnedResource>(Release);
    world.RegisterOnRemove<OtherResource>(ReleaseOther);
    world.Create(OwnedResource{3}, OtherResource{7});
    world.ProcessCommands();
    world.UnregisterComponentHook(hook);
    world.Clear();
    EXPECT_EQ(released, 0);
    EXPECT_EQ(replacementReleased, 7);
}

TEST_F(RemovalHookLifetime, EntityAwareHookCanBeRemovedAndReplaced)
{
    World world(nullptr);
    const auto hook = world.RegisterOnRemove<OwnedResource>(ReleaseWithEntity);
    EXPECT_TRUE(world.UnregisterComponentHook(hook));
    world.RegisterOnRemove<OwnedResource>(ReleaseWithEntity);
    auto entity = world.Create(OwnedResource{11});
    world.ProcessCommands();
    const auto expected = entity.GetHandle();
    entity.Remove<OwnedResource>();
    world.ProcessCommands();
    EXPECT_EQ(released, 11);
    EXPECT_EQ(releasedEntity.id, expected.id);
}

TEST_F(RemovalHookLifetime, WorldDestructionDoesNotCallUnregisteredCode)
{
    {
        World world(nullptr);
        const auto hook = world.RegisterOnRemove<OwnedResource>(Release);
        world.Create(OwnedResource{13});
        world.ProcessCommands();
        world.UnregisterComponentHook(hook);
    }
    EXPECT_EQ(released, 0);
}
} // namespace
