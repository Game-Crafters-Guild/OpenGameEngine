// Contract for the multicast Event the editor notification services broadcast
// through (VCS status, missing-asset rescans). Header-only, so this compiles
// against it directly rather than through any publisher.
//
// The property that matters: an EventSubscription may outlive the Event it came
// from. The editor destroys its services during OnShutdown and the UI panels
// holding subscriptions to them only afterwards, so unsubscribing from a dead
// publisher is the normal path, not a misuse.

#include "Events/Event.h"

#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

using GameEngine::Event;
using GameEngine::EventSubscription;

TEST(EventTest, ASubscriptionOutlivingItsEventIsInert)
{
    int calls = 0;
    EventSubscription subscription;
    {
        Event<> event;
        subscription = event.Subscribe([&calls]() { ++calls; });
        event.Invoke();
        ASSERT_EQ(calls, 1);
    }

    // Unsubscribing from a destroyed event is a no-op, not a use-after-free.
    subscription.Reset();
    EXPECT_EQ(calls, 1);
}

TEST(EventTest, ResetStopsDelivery)
{
    Event<> event;
    int calls = 0;
    EventSubscription subscription = event.Subscribe([&calls]() { ++calls; });

    subscription.Reset();
    event.Invoke();
    EXPECT_EQ(calls, 0);
}

TEST(EventTest, DestroyingTheHandleUnsubscribes)
{
    Event<> event;
    int calls = 0;
    {
        EventSubscription subscription = event.Subscribe([&calls]() { ++calls; });
    }
    event.Invoke();
    EXPECT_EQ(calls, 0);
}

TEST(EventTest, InvokeForwardsArguments)
{
    Event<int, const std::string&> event;
    std::optional<int> gotNumber;
    std::string gotText;
    EventSubscription subscription =
        event.Subscribe([&](int number, const std::string& text) {
            gotNumber = number;
            gotText = text;
        });

    event.Invoke(42, "hello");
    ASSERT_TRUE(gotNumber.has_value());
    EXPECT_EQ(*gotNumber, 42);
    EXPECT_EQ(gotText, "hello");
}

// Panels move their handles into members; a moved-from handle must not take the
// live subscription down with it when it dies.
TEST(EventTest, MovingAHandleKeepsExactlyOneSubscription)
{
    Event<> event;
    int calls = 0;

    EventSubscription held;
    {
        EventSubscription temp = event.Subscribe([&calls]() { ++calls; });
        held = std::move(temp);
    }
    event.Invoke();
    EXPECT_EQ(calls, 1);

    held.Reset();
    event.Invoke();
    EXPECT_EQ(calls, 1);
}

TEST(EventTest, SelfMoveAssignmentKeepsTheSubscription)
{
    Event<> event;
    int calls = 0;
    EventSubscription subscription = event.Subscribe([&calls]() { ++calls; });

    EventSubscription& alias = subscription;
    subscription = std::move(alias);
    event.Invoke();
    EXPECT_EQ(calls, 1);
}

// Reassigning drops the old subscription rather than leaking it — this is what
// makes a panel's SetContext(a) -> SetContext(b) stop observing a.
TEST(EventTest, ReassigningAHandleDropsThePreviousSubscription)
{
    Event<> event;
    int first = 0;
    int second = 0;

    EventSubscription subscription = event.Subscribe([&first]() { ++first; });
    subscription = event.Subscribe([&second]() { ++second; });

    event.Invoke();
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
}

// An empty callable would otherwise sit in the list and be invoked as a null
// std::function by every broadcast.
TEST(EventTest, AnEmptyCallableRegistersNothing)
{
    Event<> event;
    EventSubscription subscription = event.Subscribe({});
    event.Invoke();  // would throw std::bad_function_call if it registered
}

// Snapshot-then-invoke is the publisher contract: a callback that unsubscribes
// itself (a panel tearing down inside a refresh) must not invalidate the
// iteration it is running inside.
TEST(EventTest, AListenerMayUnsubscribeItselfDuringABroadcast)
{
    Event<> event;
    auto subscription = std::make_shared<EventSubscription>();
    int calls = 0;

    *subscription = event.Subscribe([&calls, subscription]() {
        ++calls;
        subscription->Reset();
    });

    event.Invoke();
    EXPECT_EQ(calls, 1);

    event.Invoke();
    EXPECT_EQ(calls, 1);
}

TEST(EventTest, PlusEqualsSubscribes)
{
    Event<> event;
    int calls = 0;
    EventSubscription subscription = event += [&calls]() { ++calls; };

    event.Invoke();
    EXPECT_EQ(calls, 1);
}

TEST(EventTest, MinusEqualsResetsTheHandle)
{
    Event<> event;
    int calls = 0;
    EventSubscription subscription = event += [&calls]() { ++calls; };

    event -= subscription;
    event.Invoke();
    EXPECT_EQ(calls, 0);

    // Idempotent: resetting an already-reset handle is a no-op.
    event -= subscription;
}
