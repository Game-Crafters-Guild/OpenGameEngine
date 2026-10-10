#include <gtest/gtest.h>

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;

struct FirstResource
{
    int Value = 0;
};
struct SecondResource
{
    int Value = 0;
};
struct NeedsFirstResource
{
    int Value = 0;
};
} // namespace

namespace GameEngine::ECS::Detail
{
template <>
struct RequiredComponents<::NeedsFirstResource>
{
    using type = TypeList<::FirstResource>;
};
} // namespace GameEngine::ECS::Detail

namespace
{
bool throwNotifications = false;
int firstCalls = 0;
int secondCalls = 0;
std::function<void(EntityHandle)> removalAction;

void FirstRemoval(EntityHandle entity, FirstResource& value)
{
    ++firstCalls;
    value.Value = 0;
    if (removalAction)
        removalAction(entity);
    if (throwNotifications)
        throw std::runtime_error("first removal notification");
}

void SecondRemoval(SecondResource& value)
{
    ++secondCalls;
    value.Value = 0;
    if (throwNotifications)
        throw 42; // Non-standard exceptions must not veto other notifications.
}

class EntityDestroyNotifications : public testing::Test
{
  protected:
    World world;

    void SetUp() override
    {
        firstCalls = secondCalls = 0;
        throwNotifications = true;
        world.RegisterOnRemove<FirstResource>(FirstRemoval);
        world.RegisterOnRemove<SecondResource>(SecondRemoval);
        world.EnableLifecycleEvents<FirstResource>();
        world.EnableLifecycleEvents<SecondResource>();
    }

    void TearDown() override
    {
        throwNotifications = false;
        removalAction = {};
    }

    EntityHandle Create()
    {
        return world.CreateHandle(FirstResource{1}, SecondResource{2});
    }

    void ExpectRemoved(EntityHandle entity, int calls)
    {
        EXPECT_FALSE(world.IsValid(entity));
        EXPECT_EQ(firstCalls, calls);
        EXPECT_EQ(secondCalls, calls);
    }
};

TEST_F(EntityDestroyNotifications, ImmediateAttemptsEveryHookAndFinalizesOnce)
{
    const auto entity = Create();
    EXPECT_NO_THROW(world.DestroyEntityImmediate(entity));
    ExpectRemoved(entity, 1);
    EXPECT_NO_THROW(world.DestroyEntityImmediate(entity));
    ExpectRemoved(entity, 1);
    world.SwapLifecycleEvents();
    ASSERT_EQ(world.GetRemoved<FirstResource>().size(), 1u);
    ASSERT_EQ(world.GetRemoved<SecondResource>().size(), 1u);
    EXPECT_EQ(world.GetRemoved<FirstResource>()[0], entity);
    const auto replacement = Create();
    EXPECT_EQ(replacement.index, entity.index);
    EXPECT_NE(replacement.version, entity.version);
}

TEST_F(EntityDestroyNotifications, BatchPreservesOrderSkipsDuplicatesAndLeavesReusedHandlesAlone)
{
    throwNotifications = false;
    const auto stale = world.CreateHandle(FirstResource{9});
    world.DestroyEntityImmediate(stale);
    const auto survivor = world.CreateHandle(SecondResource{8});
    ASSERT_EQ(survivor.index, stale.index);
    ASSERT_NE(survivor.version, stale.version);
    const auto first = Create();
    const auto second = world.CreateHandle(FirstResource{3});
    const auto empty = world.CreateEntity();
    const std::array requested{EntityHandle::Invalid(), stale, first, second, first, empty, stale, second};
    std::vector<EntityHandle> observed;
    removalAction = [&](EntityHandle entity)
    { observed.push_back(entity); };
    firstCalls = secondCalls = 0;
    const auto version = world.GetStructuralChangeVersion();
    world.DestroyEntitiesImmediate(requested);
    EXPECT_EQ(observed, (std::vector{first, second}));
    EXPECT_EQ(firstCalls, 2);
    EXPECT_EQ(secondCalls, 1);
    EXPECT_EQ(world.GetStructuralChangeVersion(), version + 3);
    EXPECT_EQ(world.GetEntityCount(), 1u);
    EXPECT_TRUE(world.IsValid(survivor));
    EXPECT_EQ(world.GetComponent<SecondResource>(survivor)->Value, 8);
    EXPECT_FALSE(world.IsValid(first));
    EXPECT_FALSE(world.IsValid(second));
    EXPECT_FALSE(world.IsValid(empty));
    world.SwapLifecycleEvents();
    const auto removed = world.GetRemoved<FirstResource>();
    ASSERT_EQ(removed.size(), 3u);
    EXPECT_EQ(removed[0], stale); // Existing pending events survive preparation.
    EXPECT_EQ(removed[1], first);
    EXPECT_EQ(removed[2], second);
    ASSERT_EQ(world.GetRemoved<SecondResource>().size(), 1u);
    EXPECT_EQ(world.GetRemoved<SecondResource>()[0], first);
    removalAction = {};
    const auto reuseEmpty = world.CreateEntity();
    const auto reuseSecond = world.CreateEntity();
    const auto reuseFirst = world.CreateEntity();
    EXPECT_EQ(reuseEmpty.index, empty.index);
    EXPECT_EQ(reuseSecond.index, second.index);
    EXPECT_EQ(reuseFirst.index, first.index);
}

TEST_F(EntityDestroyNotifications, BatchSnapshotSurvivesCallerSpanReplacementDuringNotification)
{
    throwNotifications = false;
    const auto first = Create();
    const auto second = Create();
    const auto survivor = Create();
    std::vector requested{first, second};
    std::array<EntityHandle, 2> observed;
    std::size_t calls = 0;
    removalAction = [&](EntityHandle entity)
    {
        if (calls < observed.size())
            observed[calls] = entity;
        ++calls;
        if (entity == first)
        {
            // Overwrite the span's storage in place, then release it: an
            // implementation still reading the caller's span would retire the
            // survivor in every CRT, not only where freed memory is scrubbed.
            requested[1] = survivor;
            std::vector{survivor}.swap(requested);
        }
    };
    world.DestroyEntitiesImmediate(requested);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(observed, (std::array{first, second}));
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
    EXPECT_TRUE(world.IsValid(survivor));
    removalAction = {};
}

TEST_F(EntityDestroyNotifications, BatchThrowingNotificationsStillFinishEveryEntity)
{
    const auto first = Create();
    const auto second = Create();
    const auto empty = world.CreateEntity();
    const std::array requested{first, second, empty};
    EXPECT_NO_THROW(world.DestroyEntitiesImmediate(requested));
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
    EXPECT_FALSE(world.IsValid(empty));
    EXPECT_EQ(world.GetEntityCount(), 0u);
    world.SwapLifecycleEvents();
    EXPECT_EQ(world.GetRemoved<FirstResource>().size(), 2u);
    EXPECT_EQ(world.GetRemoved<SecondResource>().size(), 2u);
}

TEST_F(EntityDestroyNotifications, BatchReentryOnlyAcceptsEmptyOrCurrentEntityDuplicates)
{
    throwNotifications = false;
    const auto first = Create();
    const auto second = Create();
    const auto outside = Create();
    const std::array requested{first, second};
    removalAction = [&](EntityHandle current)
    {
        const std::array duplicates{current, current};
        const std::array sibling{current == first ? second : first};
        const std::array unrelated{outside};
        EXPECT_NO_THROW(world.DestroyEntitiesImmediate({}));
        EXPECT_NO_THROW(world.DestroyEntitiesImmediate(duplicates));
        EXPECT_THROW(world.DestroyEntitiesImmediate(requested), std::logic_error);
        EXPECT_THROW(world.DestroyEntitiesImmediate(sibling), std::logic_error);
        EXPECT_THROW(world.DestroyEntitiesImmediate(unrelated), std::logic_error);
    };
    EXPECT_NO_THROW(world.DestroyEntitiesImmediate(requested));
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
    EXPECT_TRUE(world.IsValid(outside));
    removalAction = {};
}

TEST_F(EntityDestroyNotifications, DeferredFailureDoesNotLoseFollowingDestruction)
{
    const auto first = Create();
    const auto second = Create();
    world.DestroyEntity(first);
    world.DestroyEntity(first);
    world.DestroyEntity(second);
    EXPECT_NO_THROW(world.ProcessCommands());
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
}

TEST_F(EntityDestroyNotifications, PreserveHandleCompletesAndCanBeRevived)
{
    const auto entity = Create();
    EXPECT_NO_THROW(world.DestroyEntityImmediatePreserveHandle(entity));
    ExpectRemoved(entity, 1);
    const auto other = Create();
    EXPECT_NE(other.index, entity.index);
    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(entity));
    EXPECT_TRUE(world.IsValid(entity));
    EXPECT_FALSE(world.HasComponent<FirstResource>(entity));
    world.DestroyEntityImmediate(entity);
    EXPECT_EQ(firstCalls, 1);
}

TEST_F(EntityDestroyNotifications, ClearAttemptsEveryRowAndResetsLifecycleState)
{
    const auto first = Create();
    const auto second = Create();
    const auto reset = world.GetLifecycleResetGeneration();
    EXPECT_NO_THROW(world.Clear());
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
    EXPECT_GT(world.GetLifecycleResetGeneration(), reset);
    world.SwapLifecycleEvents();
    EXPECT_TRUE(world.GetRemoved<FirstResource>().empty());
    EXPECT_TRUE(world.GetAdded<FirstResource>().empty());
}

TEST_F(EntityDestroyNotifications, SelfDestructionIsHarmlessThroughEveryEntryPoint)
{
    const auto entity = Create();
    removalAction = [&](EntityHandle current)
    {
        EXPECT_TRUE(world.IsValid(current)); // Still live until all notifications finish.
        // A throw here would be swallowed by the notification isolation, so each
        // duplicate request is asserted harmless, not merely survivable.
        EXPECT_NO_THROW(world.DestroyEntityImmediate(current));
        EXPECT_NO_THROW(world.DestroyEntityImmediatePreserveHandle(current));
        EXPECT_NO_THROW(world.DestroyEntity(current)); // No preexisting thread-local command buffer.
    };
    EXPECT_NO_THROW(world.DestroyEntityImmediate(entity));
    ExpectRemoved(entity, 1);
    removalAction = {};
    const auto replacement = Create();
    world.ProcessCommands(); // No stale self-destroy command targets the recycled slot.
    EXPECT_TRUE(world.IsValid(replacement));
}

TEST_F(EntityDestroyNotifications, BulkDuplicateDestroyAndClearAreHarmless)
{
    const auto first = Create();
    const auto second = Create();
    removalAction = [&](EntityHandle current)
    {
        EXPECT_NO_THROW(world.DestroyEntity(current));
        EXPECT_NO_THROW(world.DestroyEntityImmediate(second));
        EXPECT_NO_THROW(world.DestroyEntityImmediatePreserveHandle(first));
        EXPECT_NO_THROW(world.Clear());
    };
    EXPECT_NO_THROW(world.Clear());
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
    removalAction = {};
    const auto replacement = Create();
    world.ProcessCommands();
    EXPECT_TRUE(world.IsValid(replacement));
}

TEST_F(EntityDestroyNotifications, OtherSameWorldStructuralRequestsAreRejectedBeforeLocks)
{
    const auto first = Create();
    const auto second = Create();
    const std::vector<uint8_t> bytes(sizeof(FirstResource), 0);
    removalAction = [&](EntityHandle)
    {
        EXPECT_THROW(world.DestroyEntityImmediate(second), std::logic_error);
        EXPECT_THROW(world.DestroyEntityImmediatePreserveHandle(second), std::logic_error);
        EXPECT_THROW(world.DestroyEntity(second), std::logic_error);
        EXPECT_THROW(world.Clear(), std::logic_error);
        EXPECT_THROW(world.ProcessCommands(), std::logic_error);
        EXPECT_THROW(world.CreateEntity(), std::logic_error);
        EXPECT_THROW(world.CreateHandle(FirstResource{}), std::logic_error);
        EXPECT_THROW(world.AddComponentImmediate<FirstResource>(second, FirstResource{}), std::logic_error);
        EXPECT_THROW(world.AddComponentImmediate<NeedsFirstResource>(second, NeedsFirstResource{}), std::logic_error);
        EXPECT_THROW(world.ApplyComponentBytesImmediate(second, GetComponentTypeId<FirstResource>(), bytes), std::logic_error);
        EXPECT_THROW(world.RemoveComponentImmediate<FirstResource>(second), std::logic_error);
        EXPECT_THROW(world.RegisterOnRemove<FirstResource>(FirstRemoval), std::logic_error);
        EXPECT_THROW(world.EnableLifecycleEvents<FirstResource>(), std::logic_error);
        EXPECT_THROW(world.SwapLifecycleEvents(), std::logic_error);
        EXPECT_THROW(world.BeginBulkOperations(), std::logic_error);
    };
    EXPECT_NO_THROW(world.DestroyEntityImmediate(first));
    ExpectRemoved(first, 1);
    EXPECT_TRUE(world.IsValid(second));
    removalAction = {};
    world.ProcessCommands();
    EXPECT_TRUE(world.IsValid(second));
}

TEST_F(EntityDestroyNotifications, UncaughtForbiddenRequestDoesNotVetoItsOwner)
{
    const auto first = Create();
    const auto second = Create();
    removalAction = [&](EntityHandle)
    { world.DestroyEntity(second); };
    EXPECT_NO_THROW(world.DestroyEntityImmediate(first));
    ExpectRemoved(first, 1);
    EXPECT_TRUE(world.IsValid(second));
}

TEST_F(EntityDestroyNotifications, IndependentWorldRemovalPreservesAncestorScope)
{
    World other;
    const auto foreign = other.CreateHandle(SecondResource{3});
    other.RegisterOnRemove<SecondResource>(SecondRemoval);
    const auto entity = Create();
    removalAction = [&](EntityHandle current)
    {
        EXPECT_NO_THROW(other.DestroyEntityImmediate(foreign));
        EXPECT_FALSE(other.IsValid(foreign));
        EXPECT_NO_THROW(world.DestroyEntityImmediate(current));
    };
    EXPECT_NO_THROW(world.DestroyEntityImmediate(entity));
    EXPECT_FALSE(world.IsValid(entity));
    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(secondCalls, 2);
}

TEST_F(EntityDestroyNotifications, WorldDestructorAttemptsEveryNotification)
{
    {
        World temporary;
        temporary.RegisterOnRemove<FirstResource>(FirstRemoval);
        temporary.RegisterOnRemove<SecondResource>(SecondRemoval);
        temporary.CreateHandle(FirstResource{1}, SecondResource{2});
        temporary.CreateHandle(FirstResource{3}, SecondResource{4});
    }
    EXPECT_EQ(firstCalls, 2);
    EXPECT_EQ(secondCalls, 2);
}

TEST_F(EntityDestroyNotifications, ComponentOnlyRemovalKeepsItsExistingExceptionContract)
{
    const auto entity = Create();
    EXPECT_THROW(world.RemoveComponentImmediate<FirstResource>(entity), std::runtime_error);
    EXPECT_TRUE(world.IsValid(entity));
    EXPECT_TRUE(world.HasComponent<FirstResource>(entity));
    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(secondCalls, 0);
}

TEST_F(EntityDestroyNotifications, OtherThreadRetainsOrdinaryLockAndRemovalSemantics)
{
    using namespace std::chrono_literals;
    const auto first = Create();
    const auto second = Create();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    removalAction = [&](EntityHandle current)
    {
        if (current == first)
        {
            entered.set_value();
            releaseFuture.wait();
        }
    };
    auto firstDestroy = std::async(std::launch::async, [&]
                                   { world.DestroyEntityImmediate(first); });
    const auto enteredStatus = enteredFuture.wait_for(2s);
    auto secondDestroy = std::async(std::launch::async, [&]
                                    { world.DestroyEntityImmediate(second); });
    const auto waitingStatus = secondDestroy.wait_for(20ms);
    release.set_value(); // Release before assertions or future destruction can unwind.
    EXPECT_EQ(enteredStatus, std::future_status::ready);
    EXPECT_EQ(waitingStatus, std::future_status::timeout);
    EXPECT_NO_THROW(firstDestroy.get());
    EXPECT_NO_THROW(secondDestroy.get());
    ExpectRemoved(first, 2);
    ExpectRemoved(second, 2);
}
} // namespace
