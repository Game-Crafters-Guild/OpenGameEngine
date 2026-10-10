// The per-element handler-presence bits (UIEvents.h: kNamedEventIds, m_HandlerBits).
//
// The bits exist to make a dispatch nobody subscribed to a bit test instead of a hash
// lookup. They are pure optimisation, so nothing about them is visible in ordinary
// behaviour — which is the problem these tests exist to solve: a bit wrongly CLEARED
// silently drops events, and looks exactly like an element with no subscribers.
//
// So each arm here pins one half of the maintenance rule: set on register, cleared only
// where the id's vector is provably empty, escape bit never cleared. The arm that would
// go red on a regression is ClearedBitSuppressesDispatch — it drops a bit out from under
// a live handler and requires the dispatch to skip, proving the fast path is consulted at
// all rather than being dead code that happens to agree.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

using namespace GameEngine;

namespace
{
// An id deliberately outside kNamedEventIds — the escape-bit case. Minting one is a
// supported thing to do (HashEvent is public and tests already do it), which is why the
// escape bit exists at all.
constexpr EventId kUnlistedEvent = HashEvent("Test.UnlistedEvent");
constexpr EventId kOtherUnlistedEvent = HashEvent("Test.OtherUnlistedEvent");

void Send(UIElement& el, EventId id)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// Counts its own OnEvent calls: the presence bits must never gate the CONTROL's own
// handling, only subscribers'.
class OnEventCountingElement : public UIElement
{
public:
    int OnEventCalls = 0;

    void OnEvent(UIEvent&) override { ++OnEventCalls; }
};
} // namespace

TEST(UIEventHandlerBitsTests, FreshElementHasNoBits)
{
    UIElement el;
    EXPECT_EQ(UIEventHandlerAccess::HandlerBits(el), 0u)
        << "an element nobody subscribed to must take the zero-bits early-out";
}

TEST(UIEventHandlerBitsTests, RegisterSetsTheNamedBit)
{
    UIElement el;
    el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});

    const std::uint32_t bits = UIEventHandlerAccess::HandlerBits(el);
    EXPECT_EQ(bits, EventHandlerBit(kEventMouseDown));
    EXPECT_NE(bits & EventHandlerBit(kEventMouseDown), 0u);
    EXPECT_EQ(bits & EventHandlerBit(kEventMouseUp), 0u)
        << "one id's registration must not claim another id's bit";
}

TEST(UIEventHandlerBitsTests, UnlistedIdTakesTheEscapeBit)
{
    UIElement el;
    el.RegisterEventHandler(kUnlistedEvent, [](UIEvent&) {});

    EXPECT_EQ(UIEventHandlerAccess::HandlerBits(el), kEventEscapeHandlerBit);
}

// The fast path must not change what subscribers see. Both dispatch entry points are
// covered: the runtime-id one (DispatchEvent) and the compile-time-id one that Button
// uses (the compile-time DispatchEvent overload, reached through TriggerClick).
TEST(UIEventHandlerBitsTests, DispatchStillReachesHandlers)
{
    UIElement el;
    int seen = 0;
    el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++seen; });
    Send(el, kEventMouseDown);
    EXPECT_EQ(seen, 1);

    Button btn;
    int clicks = 0;
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    btn.TriggerClick(0);
    EXPECT_EQ(clicks, 1);
}

// THE ARM. Clearing a bit behind a live handler must suppress the dispatch — if it does
// not, the bit is not being consulted and every other guarantee here is vacuous.
TEST(UIEventHandlerBitsTests, ClearedBitSuppressesDispatch)
{
    UIElement el;
    int seen = 0;
    el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++seen; });

    Send(el, kEventMouseDown);
    ASSERT_EQ(seen, 1) << "positive control: the handler fires while its bit is set";

    UIEventHandlerAccess::ClearHandlerBitForTest(el, kEventMouseDown);
    Send(el, kEventMouseDown);
    EXPECT_EQ(seen, 1) << "with the bit cleared the dispatch must take the early-out";

    // And the same for the compile-time-id path, which tests a literal rather than a
    // lookup — a separate branch that could regress independently.
    Button btn;
    int clicks = 0;
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });
    btn.TriggerClick(0);
    ASSERT_EQ(clicks, 1);
    UIEventHandlerAccess::ClearHandlerBitForTest(btn, kEventButtonClick);
    btn.TriggerClick(0);
    EXPECT_EQ(clicks, 1);
}

// The control's own OnEvent is not a subscriber and must run whether or not anything is
// listening. Skipping it would turn an optimisation into a behaviour change.
TEST(UIEventHandlerBitsTests, OnEventRunsWithNoSubscribers)
{
    OnEventCountingElement el;
    Send(el, kEventMouseDown);
    EXPECT_EQ(el.OnEventCalls, 1) << "zero-bits early-out must come AFTER OnEvent";

    el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});
    UIEventHandlerAccess::ClearHandlerBitForTest(el, kEventMouseDown);
    Send(el, kEventMouseDown);
    EXPECT_EQ(el.OnEventCalls, 2) << "a cleared bit must skip subscribers, not the control";
}

// Unregister leaves the entry in place (so a self-unregister keeps its captures), so the
// bit survives until the dispatch that compacts the vector empty. Both halves matter: the
// stale bit must not drop events, and the clear must eventually happen.
TEST(UIEventHandlerBitsTests, BitClearsOnlyWhenTheVectorEmpties)
{
    UIElement el;
    int seen = 0;
    auto token = el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++seen; });

    ASSERT_TRUE(el.UnregisterEventHandler(token));
    EXPECT_NE(UIEventHandlerAccess::HandlerBits(el) & EventHandlerBit(kEventMouseDown), 0u)
        << "unregister only deactivates; the bit may not be cleared while the entry is there";

    Send(el, kEventMouseDown); // deactivated entry does not fire, and compaction erases it
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 0u);
    EXPECT_EQ(UIEventHandlerAccess::HandlerBits(el) & EventHandlerBit(kEventMouseDown), 0u)
        << "the compaction that empties the vector is where the bit is cleared";

    // Re-registering after the clear must restore the bit, or the element is permanently
    // deaf to that id.
    int again = 0;
    el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++again; });
    Send(el, kEventMouseDown);
    EXPECT_EQ(again, 1);
}

// THE ARM FOR THE CLEAR RULE ITSELF. Every other arm here would survive a version that cleared
// the bit as soon as ANY handler for the id went away, because they only ever have one. This one
// does not: it leaves a live handler behind the bit and requires the clear not to happen.
//
// That is the failure the maintenance rule exists to prevent, and it is silent — the survivor
// simply stops being called, with a live registration still sitting in the table.
TEST(UIEventHandlerBitsTests, ClearingRespectsAnotherLiveHandlerForTheSameId)
{
    UIElement el;
    int survivor = 0;
    auto doomed = el.RegisterEventHandler(kEventMouseDown, [](UIEvent&) {});
    el.RegisterEventHandler(kEventMouseDown, [&](UIEvent&) { ++survivor; });

    ASSERT_TRUE(el.UnregisterEventHandler(doomed));

    // First dispatch: the survivor runs, and the compaction erases the unregistered entry —
    // leaving the vector NON-empty, so the bit must stay.
    Send(el, kEventMouseDown);
    ASSERT_EQ(survivor, 1);
    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(el, kEventMouseDown), 1u)
        << "precondition: exactly one entry survived the compaction";
    EXPECT_NE(UIEventHandlerAccess::HandlerBits(el) & EventHandlerBit(kEventMouseDown), 0u)
        << "a live handler is still registered — clearing the bit here would deafen it silently";

    // Second dispatch is what a wrongly cleared bit would swallow.
    Send(el, kEventMouseDown);
    EXPECT_EQ(survivor, 2) << "the surviving handler must keep firing after a SIBLING was removed";
}

// The escape bit stands for every unlisted id at once, so one of them emptying says
// nothing about the others. Clearing it there would silently deafen the survivors — this
// is the asymmetry the maintenance rule exists to encode.
TEST(UIEventHandlerBitsTests, EscapeBitSurvivesOneUnlistedIdEmptying)
{
    UIElement el;
    int other = 0;
    auto token = el.RegisterEventHandler(kUnlistedEvent, [](UIEvent&) {});
    el.RegisterEventHandler(kOtherUnlistedEvent, [&](UIEvent&) { ++other; });

    ASSERT_TRUE(el.UnregisterEventHandler(token));
    Send(el, kUnlistedEvent); // compacts kUnlistedEvent's vector to empty
    ASSERT_EQ(UIEventHandlerAccess::HandlerCount(el, kUnlistedEvent), 0u);

    EXPECT_EQ(UIEventHandlerAccess::HandlerBits(el), kEventEscapeHandlerBit)
        << "the escape bit is shared, so it is never cleared";
    Send(el, kOtherUnlistedEvent);
    EXPECT_EQ(other, 1) << "the surviving unlisted subscription must still fire";
}

// A `once` handler leaves through the compaction too, and it is the path that does not go
// through UnregisterEventHandler at all.
TEST(UIEventHandlerBitsTests, OnceHandlerClearsTheBitAfterItFires)
{
    UIElement el;
    int seen = 0;
    el.RegisterEventHandlerOnce(kEventMouseDown, [&](UIEvent&) { ++seen; });

    Send(el, kEventMouseDown);
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(UIEventHandlerAccess::HandlerBits(el) & EventHandlerBit(kEventMouseDown), 0u);

    Send(el, kEventMouseDown);
    EXPECT_EQ(seen, 1) << "a once handler fires exactly once, bits or no bits";
}
