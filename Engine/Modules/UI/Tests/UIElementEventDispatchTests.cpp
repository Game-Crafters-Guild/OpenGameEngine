// UIElement::DispatchEvent handler-table invariants.
//
// UnregisterEventHandler releases the callable and deactivates the entry; the
// entry itself is swept by DispatchEvent's compaction. These pin that the sweep
// actually runs, that the callable is released at unregister time (a handler
// owned by a hot-swappable native module must not outlive its unregister), and
// that a handler which registers another handler mid-dispatch cannot corrupt the
// bookkeeping — the register push_backs into the very deque the dispatch walk is
// holding an iterator into.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>

using namespace GameEngine;

namespace
{
void Send(UIElement& el, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// Returns whether the event came back Handled — the level UIManager's parent-ward loop
// reads to decide whether to deliver to the next ancestor (UIManager_Input.cpp).
bool SendMouseButton(UIElement& el, EventId id, int button)
{
    UIEvent e{};
    e.Id = id;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
    return e.Handled;
}
} // namespace

// A register-on-start / unregister-on-stop caller (a script wiring a click each play
// session) must not grow the element's handler vector without bound. Unregister leaves
// the entry in place; the compaction has to sweep those entries on a plain dispatch, not
// just on one where a `once` handler happened to fire.
TEST(UIElementEventDispatchTests, UnregisteredHandlersAreSweptOnAPlainDispatch)
{
    UIElement el;
    int liveCalls = 0;
    const auto liveToken =
        el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++liveCalls; });
    ASSERT_TRUE(liveToken);

    constexpr int kCycles = 32;
    for (int i = 0; i < kCycles; ++i)
    {
        const auto token = el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});
        ASSERT_TRUE(token);
        EXPECT_TRUE(el.UnregisterEventHandler(token));
    }

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown),
              static_cast<std::size_t>(kCycles) + 1u)
        << "precondition: unregister is expected to deactivate in place, leaving the entries";

    Send(el, kEventMouseDown);

    EXPECT_EQ(liveCalls, 1) << "the still-registered handler must fire";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 1u)
        << "dispatch left unregistered handlers on the vector — they accumulate until the "
           "element is destroyed";

    // A second round proves the sweep is per-dispatch, not a one-off.
    for (int i = 0; i < kCycles; ++i)
    {
        const auto token = el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});
        EXPECT_TRUE(el.UnregisterEventHandler(token));
    }
    Send(el, kEventMouseDown);
    EXPECT_EQ(liveCalls, 2);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 1u);
}

// A `once` handler that registers a replacement for the same event is the natural
// re-arm pattern, and it is the case that reallocates the handler vector mid-iteration
// (size == capacity == 1 before the inner register). The dispatch loop must read the
// firing entry's key/once BEFORE invoking it: reading them afterwards dereferences an
// iterator the reallocation invalidated. Asserts the resulting bookkeeping — the fired
// `once` entry is gone, the replacement survived and is armed for the next dispatch.
TEST(UIElementEventDispatchTests, OnceHandlerMayRegisterAReplacementMidDispatch)
{
    UIElement el;
    int firstCalls = 0;
    int replacementCalls = 0;

    el.RegisterEventHandlerOnce(kEventMouseDown,
                                [&](UIEvent&)
                                {
                                    ++firstCalls;
                                    el.RegisterEventHandler(
                                        kEventMouseDown, [&](UIEvent&) { ++replacementCalls; });
                                });

    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 1u);

    Send(el, kEventMouseDown);
    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(replacementCalls, 0) << "a handler registered during a dispatch must not run in it";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 1u)
        << "the fired `once` handler was not removed, or the replacement was lost";

    Send(el, kEventMouseDown);
    EXPECT_EQ(firstCalls, 1) << "the `once` handler fired twice";
    EXPECT_EQ(replacementCalls, 1) << "the replacement registered mid-dispatch never armed";
}

// The same re-entrancy against a non-`once` handler, with enough live handlers that the
// inner register reallocates a vector the loop is mid-way through. Every originally
// registered handler must still fire exactly once in this dispatch.
TEST(UIElementEventDispatchTests, ReentrantRegisterDoesNotDisturbTheRestOfTheDispatch)
{
    UIElement el;
    constexpr int kExisting = 4;
    int calls[kExisting] = {};
    int addedCalls = 0;

    for (int i = 0; i < kExisting; ++i)
        el.RegisterEventHandler(kEventMouseDown, [&calls, i](UIEvent&) { ++calls[i]; });
    // Registers from inside the dispatch, after some handlers have already run.
    el.RegisterEventHandler(kEventMouseDown,
                            [&](UIEvent&)
                            {
                                el.RegisterEventHandler(kEventMouseDown,
                                                        [&](UIEvent&) { ++addedCalls; });
                            });

    Send(el, kEventMouseDown);
    for (int i = 0; i < kExisting; ++i)
        EXPECT_EQ(calls[i], 1) << "handler " << i << " did not fire exactly once";
    EXPECT_EQ(addedCalls, 0) << "a handler registered during a dispatch must not run in it";

    Send(el, kEventMouseDown);
    for (int i = 0; i < kExisting; ++i)
        EXPECT_EQ(calls[i], 2);
    EXPECT_GE(addedCalls, 1) << "the mid-dispatch registration never armed";
}

// The crash this pins (native user-module hot swap, editor Game View): the
// callable a handler owns is code and constant data inside the registering
// image. UnregisterEventHandler used to leave that callable stored on the
// element until some later dispatch compacted it away, so a module unloaded in
// between left a std::function whose vtable lived in unmapped address space —
// and that compaction destroys the callable, reading through exactly that vtable.
// Releasing the callable at unregister time is
// what makes the unload safe, so assert on the captured state's lifetime rather
// than on whether the handler fires.
TEST(UIElementEventDispatchTests, UnregisterReleasesTheCallableImmediately)
{
    UIElement el;
    auto owned = std::make_shared<int>(7);
    ASSERT_EQ(owned.use_count(), 1);

    const auto token = el.RegisterEventHandler(kEventMouseUp, [owned](UIEvent&) { *owned = 8; });
    ASSERT_TRUE(token);
    ASSERT_EQ(owned.use_count(), 2) << "the handler does not own the capture";

    EXPECT_TRUE(el.UnregisterEventHandler(token));
    EXPECT_EQ(owned.use_count(), 1)
        << "the callable outlived its unregister — a hot-swapped module's handler would dangle";

    // And the entry is still swept, so nothing accumulates.
    Send(el, kEventMouseUp);
    EXPECT_EQ(GameEngine::UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
}

// The exception to the rule above: a handler that unregisters ITSELF is running out
// of the very callable the release would destroy, so that one release must wait for
// the drain that erases the entry.
//
// The observable is the entry's own callable, read out of the table from inside the
// running handler — the invariant itself, not a proxy for it. A capture-destructor
// witness would answer a different question: it reports when the payload died, which
// for a shared_ptr capture is a refcount reaching zero somewhere and for an inline
// one is whichever move destroyed the source last. Ask the table what it still
// holds.
TEST(UIElementEventDispatchTests, SelfUnregisterKeepsItsOwnCallableForTheRestOfTheCall)
{
    UIElement el;
    UIElement::EventHandlerToken token{};
    bool ownCallableSurvived = false;
    int ran = 0;

    token = el.RegisterEventHandler(kEventMouseUp,
                                    [&](UIEvent&)
                                    {
                                        ++ran;
                                        EXPECT_TRUE(el.UnregisterEventHandler(token));
                                        ownCallableSurvived =
                                            GameEngine::UIEventHandlerAccess::HandlerHasCallable(
                                                el, kEventMouseUp, token.Key);
                                    });
    ASSERT_TRUE(token);

    Send(el, kEventMouseUp);

    EXPECT_EQ(ran, 1);
    EXPECT_TRUE(ownCallableSurvived)
        << "the callable was destroyed while its own operator() frame was live";
    // The compaction still sweeps the entry once the call returns.
    EXPECT_EQ(GameEngine::UIEventHandlerAccess::HandlerCount(el, kEventMouseUp), 0u);
}

// The flow that actually ships: clicking the toolbar's Stop button tears down play
// mode from inside a UI dispatch, and the user systems' OnDestroy unregister their
// HUD handlers from there. Those handlers belong to a different element than the one
// dispatching, so nothing is running out of them and they must be released on the
// spot — a guard that merely asked "is a dispatch in progress" would skip exactly
// this case and leave the module-owned callable to dangle.
TEST(UIElementEventDispatchTests, UnregisterFromInsideAnotherElementsDispatchStillReleases)
{
    UIElement dispatcher;
    UIElement subject;
    auto owned = std::make_shared<int>(0);

    const auto subjectToken =
        subject.RegisterEventHandler(kEventMouseUp, [owned](UIEvent&) { *owned = 1; });
    ASSERT_TRUE(subjectToken);
    ASSERT_EQ(owned.use_count(), 2);

    long useCountDuringDispatch = 0;
    dispatcher.RegisterEventHandler(kEventMouseDown,
                                    [&](UIEvent&)
                                    {
                                        EXPECT_TRUE(subject.UnregisterEventHandler(subjectToken));
                                        useCountDuringDispatch = owned.use_count();
                                    });

    Send(dispatcher, kEventMouseDown);

    EXPECT_EQ(useCountDuringDispatch, 1)
        << "the unregistered handler kept its callable because an unrelated element was "
           "mid-dispatch; a module unloaded before the next click would leave it dangling";
    EXPECT_EQ(*owned, 0) << "the unregistered handler must not have run";
}

// --------------------------------------------------------------------------------------
// Handled is a TRANSITION in the same-element walk, not a level.
//
// DispatchEvent runs the control's own OnEvent BEFORE the handler table, so a control that
// consumes input there (Button arms and stops a left press) hands the table an event that is
// already Handled. Same-element table subscribers are observers of that element's event, not
// successive propagation targets, so an already-Handled event must still reach all of them;
// only a subscriber that flips Handled during the walk suppresses the ones behind it.
// --------------------------------------------------------------------------------------

// The shipping shape. Every same-element subscriber past the first was starved, and which one
// survived was decided by registration order alone — so a feature whose subscription happened
// to register second was silently dead (three editor drag features, PR #1107).
TEST(UIElementEventDispatchTests, AControlConsumingInOnEventDoesNotStarveItsTableSubscribers)
{
    Button btn;
    int first = 0;
    int second = 0;
    btn.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++first; });
    btn.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++second; });

    ASSERT_TRUE(SendMouseButton(btn, kEventMouseDown, 0))
        << "precondition: Button::OnEvent is expected to consume an armed left press before the "
           "table walk — without that this test is not exercising the already-Handled path";

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 1)
        << "the second subscriber was starved by the control's own OnEvent consuming the event";
}

// The retained half of the rule: a subscriber that stops DURING the walk still suppresses the
// subscribers after it. A plain UIElement's OnEvent does nothing, so the event reaches the
// table unhandled and this subscriber's Stop() is a real false->true transition.
TEST(UIElementEventDispatchTests, ASubscriberStoppingDuringTheWalkSuppressesTheOnesAfterIt)
{
    UIElement el;
    int first = 0;
    int second = 0;
    el.RegisterEventHandler(kEventMouseDown,
                            [&](UIEvent& e)
                            {
                                ++first;
                                e.Stop();
                            });
    el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++second; });

    EXPECT_TRUE(SendMouseButton(el, kEventMouseDown, 0));
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0) << "stopPropagation from a table subscriber stopped suppressing later ones";
}

// The corner where the two halves meet, pinned so the semantics are not accidentally changed:
// once the control has already consumed the event, a subscriber's Stop() cannot transition it,
// so it does NOT suppress the subscribers behind it. Suppression is available to the first
// consumer of an event, not to everyone downstream of it.
TEST(UIElementEventDispatchTests, AStopCannotSuppressSiblingsWhenTheEventArrivedHandled)
{
    Button btn;
    int first = 0;
    int second = 0;
    btn.RegisterEventHandler(kEventMouseDown,
                             [&](UIEvent& e)
                             {
                                 ++first;
                                 e.Stop();
                             });
    btn.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++second; });

    ASSERT_TRUE(SendMouseButton(btn, kEventMouseDown, 0));
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 1);
}

// The bubbling boundary. The same-element rule must not leak into parent-ward propagation:
// UIManager's bubbling loop reads e.Handled as a LEVEL after DispatchEvent returns, so the
// walk must never clear it — whether the control set it, or a subscriber did, or a further
// subscriber ran afterwards.
TEST(UIElementEventDispatchTests, TheWalkNeverClearsHandledSoBubblingStillStops)
{
    {
        // Control consumed it, and a subscriber ran after that point.
        Button btn;
        bool ran = false;
        btn.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ran = true; });
        EXPECT_TRUE(SendMouseButton(btn, kEventMouseDown, 0))
            << "the control's consumption was lost, so the event would bubble to its parent";
        EXPECT_TRUE(ran);
    }
    {
        // A subscriber consumed it on an element whose OnEvent does nothing.
        UIElement el;
        el.RegisterEventHandler(kEventMouseDown, [](UIEvent& e) { e.Stop(); });
        EXPECT_TRUE(SendMouseButton(el, kEventMouseDown, 0))
            << "a subscriber's Stop() no longer reaches the bubbling loop";
    }
    {
        // Nobody consumed it: the event stays unhandled and must still bubble.
        UIElement el;
        el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});
        EXPECT_FALSE(SendMouseButton(el, kEventMouseDown, 0))
            << "the walk marked an unconsumed event Handled and would block bubbling";
    }
}
