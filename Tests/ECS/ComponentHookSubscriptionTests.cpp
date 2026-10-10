#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include <chrono>
#include <future>
#include <memory>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
using namespace GameEngine;
using namespace GameEngine::ECS;
struct Resource { int Value = 0; };
int released = 0;
EntityHandle observed{};
void Release(Resource& resource) { released += resource.Value; }
void ReleaseWithEntity(EntityHandle entity, Resource& resource) { observed = entity; Release(resource); }

ComponentHookToken Register(World& world, ComponentHookKind kind, bool entityAware)
{
    switch (kind)
    {
    case ComponentHookKind::Add:
        return entityAware ? world.RegisterOnAdd<Resource>(ReleaseWithEntity) : world.RegisterOnAdd<Resource>(Release);
    case ComponentHookKind::Set:
        return entityAware ? world.RegisterOnSet<Resource>(ReleaseWithEntity) : world.RegisterOnSet<Resource>(Release);
    case ComponentHookKind::Remove:
        return entityAware ? world.RegisterOnRemove<Resource>(ReleaseWithEntity) : world.RegisterOnRemove<Resource>(Release);
    }
    return {};
}

ScopedSubscription Subscribe(World& world, ComponentHookKind kind, bool entityAware)
{
    switch (kind)
    {
    case ComponentHookKind::Add:
        return entityAware ? world.SubscribeOnAdd<Resource>(ReleaseWithEntity) : world.SubscribeOnAdd<Resource>(Release);
    case ComponentHookKind::Set:
        return entityAware ? world.SubscribeOnSet<Resource>(ReleaseWithEntity) : world.SubscribeOnSet<Resource>(Release);
    case ComponentHookKind::Remove:
        return entityAware ? world.SubscribeOnRemove<Resource>(ReleaseWithEntity) : world.SubscribeOnRemove<Resource>(Release);
    }
    return {};
}

EntityHandle CreateAndDestroy(World& world, int value)
{
    auto entity = world.Create();
    // Exercise the component-add path for Add/Set, then the destruction path
    // for Remove. Bulk creation with initial values has separate semantics.
    entity.Set(Resource{value});
    world.ProcessCommands();
    const auto handle = entity.GetHandle();
    entity.Destroy();
    world.ProcessCommands();
    return handle;
}

class ComponentHookSubscription : public testing::Test
{
    void SetUp() override { released = 0; observed = {}; }
};

TEST_F(ComponentHookSubscription, SameCallbackReplacementInvalidatesOldTokenForAllSixOverloads)
{
    for (const auto kind : {ComponentHookKind::Add, ComponentHookKind::Set, ComponentHookKind::Remove})
        for (const bool entityAware : {false, true})
        {
            World world(nullptr);
            const auto old = Register(world, kind, entityAware);
            const auto current = Register(world, kind, entityAware);
            EXPECT_NE(old, current);
            EXPECT_FALSE(world.UnregisterComponentHook(old));
            released = 0;
            const auto entity = CreateAndDestroy(world, 3);
            EXPECT_EQ(released, 3);
            if (entityAware) EXPECT_EQ(observed.id, entity.id);
            EXPECT_TRUE(world.UnregisterComponentHook(current));
            CreateAndDestroy(world, 7);
            EXPECT_EQ(released, 3);
        }
}

TEST_F(ComponentHookSubscription, ScopesDetachAllSixOverloads)
{
    for (const auto kind : {ComponentHookKind::Add, ComponentHookKind::Set, ComponentHookKind::Remove})
        for (const bool entityAware : {false, true})
        {
            World world(nullptr);
            released = 0;
            {
                auto scope = Subscribe(world, kind, entityAware);
                EXPECT_TRUE(scope);
                const auto entity = CreateAndDestroy(world, 3);
                EXPECT_EQ(released, 3);
                if (entityAware) EXPECT_EQ(observed.id, entity.id);
            }
            CreateAndDestroy(world, 7);
            EXPECT_EQ(released, 3);
        }
}

TEST_F(ComponentHookSubscription, MoveAssignmentCannotEraseReplacementBySameCallback)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    scope = world.SubscribeOnRemove<Resource>(Release);
    CreateAndDestroy(world, 5);
    EXPECT_EQ(released, 5);
    ScopedSubscription moved(std::move(scope));
    EXPECT_FALSE(scope);
    moved.Reset();
    moved.Reset();
    CreateAndDestroy(world, 7);
    EXPECT_EQ(released, 5);
}

TEST_F(ComponentHookSubscription, OldScopeCannotDetachNewPersistentRegistration)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    world.RegisterOnRemove<Resource>(Release);
    scope.Reset();
    CreateAndDestroy(world, 5);
    EXPECT_EQ(released, 5);
}

TEST_F(ComponentHookSubscription, TokensAreBoundToWorldAndHookKind)
{
    World first(nullptr), second(nullptr);
    const auto firstHook = first.RegisterOnRemove<Resource>(Release);
    const auto secondHook = second.RegisterOnRemove<Resource>(Release);
    EXPECT_FALSE(second.UnregisterComponentHook(firstHook));
    const auto added = second.RegisterOnAdd<Resource>(Release);
    const auto set = second.RegisterOnSet<Resource>(Release);
    EXPECT_TRUE(second.UnregisterComponentHook(added));
    EXPECT_TRUE(second.UnregisterComponentHook(set));
    CreateAndDestroy(second, 5);
    EXPECT_EQ(released, 5);
    EXPECT_TRUE(second.UnregisterComponentHook(secondHook));
    EXPECT_TRUE(first.UnregisterComponentHook(firstHook));
}

TEST_F(ComponentHookSubscription, ClearPreservesSubscriptionsAndDoesNotReuseRegistrationIds)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    world.Create(Resource{3}); world.ProcessCommands();
    world.Clear();
    EXPECT_EQ(released, 3);
    const auto current = world.RegisterOnRemove<Resource>(Release);
    scope.Reset();
    CreateAndDestroy(world, 5);
    EXPECT_EQ(released, 8);
    EXPECT_TRUE(world.UnregisterComponentHook(current));
}

TEST_F(ComponentHookSubscription, TokenCanUnregisterItsPersistentHookAfterClear)
{
    World world(nullptr);
    const auto token = world.RegisterOnRemove<Resource>(Release);
    world.Clear();
    EXPECT_TRUE(world.UnregisterComponentHook(token));
    CreateAndDestroy(world, 3);
    EXPECT_EQ(released, 0);
}

TEST_F(ComponentHookSubscription, ResetDetachesWithoutInvokingResourceCleanup)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    world.Create(Resource{7}); world.ProcessCommands();
    scope.Reset();
    EXPECT_EQ(released, 0);
    world.Clear();
    EXPECT_EQ(released, 0);
}

TEST_F(ComponentHookSubscription, ScopeOutlivesWorldAndWorldStillRunsCleanup)
{
    ScopedSubscription scope;
    {
        World world(nullptr);
        scope = world.SubscribeOnRemove<Resource>(Release);
        world.Create(Resource{7}); world.ProcessCommands();
    }
    EXPECT_EQ(released, 7);
    scope.Reset();
    EXPECT_FALSE(scope);
}

TEST_F(ComponentHookSubscription, OldScopeCannotAffectAWorldReusingTheSameAddress)
{
    alignas(World) std::byte storage[sizeof(World)];
    auto* first = std::construct_at(reinterpret_cast<World*>(storage), nullptr);
    auto old = first->SubscribeOnRemove<Resource>(Release);
    std::destroy_at(first);
    auto* second = std::construct_at(reinterpret_cast<World*>(storage), nullptr);
    auto current = second->SubscribeOnRemove<Resource>(Release);
    old.Reset();
    CreateAndDestroy(*second, 9);
    EXPECT_EQ(released, 9);
    current.Reset();
    std::destroy_at(second);
}

TEST_F(ComponentHookSubscription, NullCallbacksDoNotReplaceExistingRegistration)
{
    World world(nullptr);
    void (*plain)(Resource&) = nullptr;
    void (*aware)(EntityHandle, Resource&) = nullptr;
    world.RegisterOnAdd<Resource>(Release);
    world.RegisterOnSet<Resource>(Release);
    world.RegisterOnRemove<Resource>(Release);
    EXPECT_FALSE(world.RegisterOnAdd<Resource>(plain));
    EXPECT_FALSE(world.RegisterOnAdd<Resource>(aware));
    EXPECT_FALSE(world.RegisterOnSet<Resource>(plain));
    EXPECT_FALSE(world.RegisterOnSet<Resource>(aware));
    EXPECT_FALSE(world.RegisterOnRemove<Resource>(plain));
    EXPECT_FALSE(world.RegisterOnRemove<Resource>(aware));
    EXPECT_FALSE(world.SubscribeOnAdd<Resource>(plain));
    EXPECT_FALSE(world.SubscribeOnAdd<Resource>(aware));
    EXPECT_FALSE(world.SubscribeOnSet<Resource>(plain));
    EXPECT_FALSE(world.SubscribeOnSet<Resource>(aware));
    EXPECT_FALSE(world.SubscribeOnRemove<Resource>(plain));
    EXPECT_FALSE(world.SubscribeOnRemove<Resource>(aware));
    CreateAndDestroy(world, 3);
    EXPECT_EQ(released, 9);
}

TEST_F(ComponentHookSubscription, ImagePinEndsWhenScopeResets)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    const auto address = reinterpret_cast<std::uintptr_t>(&Release);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 1u);
    scope.Reset();
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 0u);
}

TEST_F(ComponentHookSubscription, FailedWorldConstructionDoesNotPublishARegistryEntry)
{
    World world(nullptr);
    auto scope = world.SubscribeOnRemove<Resource>(Release);
    WorldConfig impossible;
    impossible.ExpectedEntityCount = std::numeric_limits<std::size_t>::max();
    EXPECT_THROW({ World rejected(impossible, nullptr); }, std::length_error);
    const auto address = reinterpret_cast<std::uintptr_t>(&Release);
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 1u);
    scope.Reset();
    EXPECT_EQ(World::CountComponentHooksOwnedByImage(address, 1), 0u);
}

std::promise<void>* enteredDestruction = nullptr;
std::shared_future<void> allowDestruction;
void BlockDestruction(Resource&)
{
    enteredDestruction->set_value();
    allowDestruction.wait();
}

TEST_F(ComponentHookSubscription, ResetDuringWorldDestructionDoesNotTouchItsLockedStorage)
{
    using namespace std::chrono_literals;
    std::promise<void> entered, proceed;
    auto enteredFuture = entered.get_future();
    enteredDestruction = &entered;
    allowDestruction = proceed.get_future().share();
    auto world = std::make_unique<World>(nullptr);
    auto scope = world->SubscribeOnRemove<Resource>(BlockDestruction);
    world->Create(Resource{1}); world->ProcessCommands();
    std::jthread destroyer([owned = std::move(world)]() mutable { owned.reset(); });
    const auto enteredStatus = enteredFuture.wait_for(5s);
    EXPECT_EQ(enteredStatus, std::future_status::ready);
    auto reset = std::async(std::launch::async, [&scope] { scope.Reset(); });
    const auto resetStatus = reset.wait_for(5s);
    // Always release the destructor before any failure can unwind the threads.
    proceed.set_value();
    EXPECT_EQ(resetStatus, std::future_status::ready);
    reset.get();
    destroyer.join();
    enteredDestruction = nullptr;
    allowDestruction = {};
}

} // namespace
