// WHICH SUBSCRIBERS A DISPATCH VISITS while the handler table is mutated underneath it.
//
// DispatchToHandlers walks the table IN PLACE and copies nothing, so the walk's cursor is
// exposed to every mutation a handler makes. The dangerous one is not registration — a deque
// append moves nothing — it is COMPACTION: when a handler returns and its frame was the
// outermost on the element, DrainInactiveHandlers erases every deactivated entry and remove_if
// slides the survivors DOWN over them. A cursor expressed as an index is silently wrong from
// that moment on, skipping one live subscriber for every entry erased ahead of it, and skipping
// a subscriber is invisible: nothing crashes, a callback just never arrives.
//
// The walk is therefore keyed on HandlerEntry::key, which names one entry for the element's
// lifetime and survives the moves. These pin the behaviour that choice buys, from the cases
// where an index and a key genuinely diverge. Run them against an index-based walk before
// trusting them — every one of them passes on a copy-first walk too, so their value is entirely
// in what they say about the in-place one.
//
// Registration DURING a walk is pinned elsewhere and not duplicated here:
// UIElementEventDispatchTests' OnceHandlerMayRegisterAReplacementMidDispatch and
// ReentrantRegisterDoesNotDisturbTheRestOfTheDispatch. When compaction may run at all, and what
// it may not do to an executing frame, belongs to HandlerTableCompactionTests.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <cstddef>
#include <vector>

using namespace GameEngine;

namespace
{
constexpr EventId kWalkEvent = HashEvent("UI.DispatchWalkProbe");

void Send(UIElement& el)
{
    UIEvent e{};
    e.Id = kWalkEvent;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// Records the ORDER subscribers ran in, not just how many did. A cursor that desynchronises
// skips subscribers rather than reordering them, so the count is what fails first — but the
// order is what proves the walk is still the registration-ordered walk callers rely on.
struct WalkLog
{
    std::vector<int> Fired;

    // Subscribes `count` numbered handlers that do nothing but record themselves.
    void Subscribe(UIElement& el, std::vector<UIElement::EventHandlerToken>& tokens, int count)
    {
        for (int i = 0; i < count; ++i)
            tokens.push_back(
                el.RegisterEventHandler(kWalkEvent, [this, i](UIEvent&) { Fired.push_back(i); }));
    }
};
} // namespace

// THE PIN THE IN-PLACE WALK EXISTS FOR. A subscriber unregisters two subscribers that ran
// BEFORE it. Both entries are deactivated and released immediately, and the drain that runs
// when this handler's frame unwinds erases them — so the deque loses two elements from in
// front of the cursor, mid-walk, and every remaining subscriber slides down by two.
//
// An index cursor resumes two entries too far and never calls subscribers 3 and 4. A key
// cursor asks for the first entry above the last key it offered a turn, which is unaffected
// by the move.
TEST(DispatchWalkStabilityTests, RemovingEarlierSubscribersDoesNotSkipTheLaterOnes)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;

    log.Subscribe(el, tokens, 2); // 0, 1 — removed from inside 2
    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(2);
                                                 EXPECT_TRUE(el.UnregisterEventHandler(tokens[0]));
                                                 EXPECT_TRUE(el.UnregisterEventHandler(tokens[1]));
                                             }));
    // Registered after the remover, so they are the ones an index cursor loses.
    for (int i = 3; i <= 5; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(el, kWalkEvent), 6u);

    Send(el);

    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 2, 3, 4, 5}))
        << "the walk skipped subscribers after two entries were erased from in front of its "
           "cursor; an index-based cursor loses exactly one subscriber per entry dropped ahead "
           "of it";
    // The erasure really did happen during the walk rather than after it, which is the
    // precondition the assertion above is only meaningful under.
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kWalkEvent), 4u)
        << "precondition: the two unregistered entries must be compacted away by this dispatch";
}

// The same shape at a size where an index cursor cannot merely mis-order but must run off the
// end: nearly every entry ahead of the cursor is dropped, so the surviving tail sits far below
// where an index left off.
TEST(DispatchWalkStabilityTests, RemovingManyEarlierSubscribersStillVisitsTheWholeTail)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;
    constexpr int kLeading = 8;
    constexpr int kTrailing = 8;

    log.Subscribe(el, tokens, kLeading);
    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(kLeading);
                                                 for (int i = 0; i < kLeading; ++i)
                                                     EXPECT_TRUE(
                                                         el.UnregisterEventHandler(tokens[i]));
                                             }));
    for (int i = kLeading + 1; i <= kLeading + kTrailing; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    std::vector<int> expected;
    for (int i = 0; i <= kLeading + kTrailing; ++i)
        expected.push_back(i);
    EXPECT_EQ(log.Fired, expected)
        << "dropping " << kLeading << " entries from in front of the cursor cost the walk part of "
           "its tail";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kWalkEvent),
              static_cast<std::size_t>(kTrailing) + 1u);
}

// A subscriber that unregisters ITSELF. Its entry is deactivated with its callable retained —
// the frame is still running out of it — and erased by the drain the moment that frame unwinds.
// So the cursor's own entry disappears from under it before the next step, which is the one
// case where an index cursor is off by one rather than by the number of removals.
TEST(DispatchWalkStabilityTests, ASelfUnregisteringSubscriberDoesNotSkipItsSuccessor)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;

    UIElement::EventHandlerToken self{};
    self = el.RegisterEventHandler(kWalkEvent,
                                   [&](UIEvent&)
                                   {
                                       log.Fired.push_back(0);
                                       EXPECT_TRUE(el.UnregisterEventHandler(self));
                                   });
    tokens.push_back(self);
    for (int i = 1; i <= 3; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 2, 3}))
        << "the successor of a self-unregistering subscriber was skipped";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kWalkEvent), 3u)
        << "precondition: the self-unregistered entry must be erased by this dispatch";

    log.Fired.clear();
    Send(el);
    EXPECT_EQ(log.Fired, (std::vector<int>{1, 2, 3})) << "the self-unregistered handler fired again";
}

// Unregistering a subscriber that has NOT run yet must stop it running — the walk reads each
// entry's `active` at the moment it reaches it, not at the moment the dispatch began. This is
// the guarantee a walk over a private copy of the table would break, and the reason the walk
// re-finds the LIVE entry on every step rather than invoking anything it captured earlier.
TEST(DispatchWalkStabilityTests, RemovingALaterSubscriberStopsItRunning)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;

    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(0);
                                                 // tokens[2] has not run yet.
                                                 EXPECT_TRUE(el.UnregisterEventHandler(tokens[2]));
                                             }));
    for (int i = 1; i <= 3; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 3}))
        << "a subscriber unregistered earlier in the same dispatch still ran";
}

// Removing a later subscriber AND earlier ones in one call, so the cursor has to survive an
// erase on both sides of itself in a single compaction.
TEST(DispatchWalkStabilityTests, RemovingSubscribersOnBothSidesOfTheCursorKeepsTheRestExact)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;

    for (int i = 0; i <= 1; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));
    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(2);
                                                 EXPECT_TRUE(el.UnregisterEventHandler(tokens[0]));
                                                 EXPECT_TRUE(el.UnregisterEventHandler(tokens[4]));
                                             }));
    for (int i = 3; i <= 5; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 2, 3, 5}))
        << "an erase on either side of the cursor cost the walk a subscriber it owed";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kWalkEvent), 4u);
}

// A nested dispatch of the SAME id from inside a subscriber. The nested walk runs the same set
// from the top and finishes, and the OUTER walk must then resume where it was — on a table the
// nested dispatch was free to compact, since the nested walk's own drain is refused while the
// outer frame stands on an entry, but the inner subscriber's frames come and go underneath it.
TEST(DispatchWalkStabilityTests, ANestedSameIdDispatchLeavesTheOuterWalkOnTrack)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;
    int depth = 0;

    tokens.push_back(
        el.RegisterEventHandler(kWalkEvent, [&log](UIEvent&) { log.Fired.push_back(0); }));
    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(1);
                                                 if (depth++ > 0)
                                                     return;
                                                 Send(el); // same id, same element
                                             }));
    for (int i = 2; i <= 3; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    // Outer 0, outer 1, then the whole nested walk (0,1,2,3 — the nested 1 re-enters no
    // further), then the outer walk resumes at 2.
    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 0, 1, 2, 3, 2, 3}))
        << "the outer walk did not resume at the subscriber after the one that dispatched";
}

// The nested case with a removal inside it: the nested dispatch unregisters a subscriber the
// OUTER walk has not reached yet. The outer walk must observe that, because it re-reads each
// entry's `active` when it arrives — and it must not lose its place to the compaction the
// removal causes.
TEST(DispatchWalkStabilityTests, ARemovalInsideANestedDispatchIsSeenByTheOuterWalk)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;
    int depth = 0;

    tokens.push_back(el.RegisterEventHandler(kWalkEvent,
                                             [&](UIEvent&)
                                             {
                                                 log.Fired.push_back(0);
                                                 if (depth++ > 0)
                                                     return;
                                                 EXPECT_TRUE(
                                                     el.UnregisterEventHandler(tokens[2]));
                                                 Send(el);
                                             }));
    for (int i = 1; i <= 3; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);

    // Outer 0 removes 2 and dispatches; the nested walk runs 0 (re-entry guarded), 1, 3; the
    // outer walk then resumes and runs 1 and 3.
    EXPECT_EQ(log.Fired, (std::vector<int>{0, 0, 1, 3, 1, 3}))
        << "the outer walk either re-ran the removed subscriber or lost its place after the "
           "nested dispatch compacted the table";
}

// Registration order IS dispatch order, and the walk's key cursor is what preserves it: keys
// ascend with registration and the deque holds them in that order, so "the next key above the
// last one offered a turn" is "the next subscriber registered". Pinned on its own because every
// other test here would still pass if the walk ran the right SET in the wrong order.
TEST(DispatchWalkStabilityTests, SubscribersRunInRegistrationOrderAcrossRemovalCycles)
{
    UIElement el;
    WalkLog log;
    std::vector<UIElement::EventHandlerToken> tokens;

    log.Subscribe(el, tokens, 4);
    // A register/unregister churn that leaves dead entries for the next dispatch to compact,
    // so the surviving subscribers are visited across a table that has been rewritten.
    for (int i = 0; i < 16; ++i)
    {
        const auto scratch = el.RegisterEventHandler(kWalkEvent, [](UIEvent&) {});
        EXPECT_TRUE(el.UnregisterEventHandler(scratch));
    }
    for (int i = 4; i <= 6; ++i)
        tokens.push_back(
            el.RegisterEventHandler(kWalkEvent, [&log, i](UIEvent&) { log.Fired.push_back(i); }));

    Send(el);
    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));

    log.Fired.clear();
    Send(el);
    EXPECT_EQ(log.Fired, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}))
        << "order or membership drifted once the dead entries had been compacted away";
}
