// Button's first-class click event (kEventButtonClick).
//
// Button owns the notion of a click — armed (the press began on it) and still inside at
// mouse-up, or Space/Enter while focused — and funnels all of it through TriggerClick.
// These pin that TriggerClick dispatches kEventButtonClick through the handler table, that
// subscriptions are additive and individually revocable (no subscriber can displace
// another's -- the property that makes the table safe for the scripting ABI), and that
// dispatching an event from inside another dispatch on the same element does not corrupt
// the table.
//
// The armed-and-inside semantics themselves are pinned end-to-end through the real pointer
// routing in GameUIHostTests (a release over a button whose press began elsewhere is not a
// click); here TriggerClick is the entry point, which is also the programmatic click API.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
void Send(UIElement& el, EventId id, int key = 0)
{
    UIEvent e{};
    e.Id = id;
    e.Key = key;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}
} // namespace

TEST(ButtonClickEventTests, TriggerClickDispatchesClickEvent)
{
    Button btn;
    int clicks = 0;
    int mods = -1;
    btn.RegisterEventHandler(kEventButtonClick,
                             [&](UIEvent& e)
                             {
                                 ++clicks;
                                 mods = e.Mods;
                             });

    btn.TriggerClick(7);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(mods, 7) << "the modifier bitmask must reach handler-table subscribers";
}

// Keyboard activation is a click by the button's own definition, so it must reach the same
// subscribers as a mouse click — otherwise a script-driven button is silently
// keyboard-inaccessible.
TEST(ButtonClickEventTests, KeyboardActivationDispatchesClickEvent)
{
    Button btn;
    int clicks = 0;
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++clicks; });

    Send(btn, kEventKeyDown, Input::kKeyCode_Space);
    EXPECT_EQ(clicks, 1);
    Send(btn, kEventKeyDown, Input::kKeyCode_Enter);
    EXPECT_EQ(clicks, 2);
}

// Subscribers fire in registration order. With the click members gone, no SUBSCRIBER holds
// a privileged slot any more, so a caller that needs to run before another has to register
// first rather than reach for a member slot. (The control's own OnEvent still runs ahead of
// the whole table — DispatchEvent calls it first — but Button::OnEvent does not branch on
// kEventButtonClick, so nothing consumes a click before these.)
TEST(ButtonClickEventTests, ClickSubscribersFireInRegistrationOrder)
{
    Button btn;
    std::vector<std::string> order;

    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { order.push_back("first"); });
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { order.push_back("second"); });

    btn.TriggerClick();
    ASSERT_EQ(order.size(), 2u) << "one of the two click consumers was displaced by the other";
    EXPECT_EQ(order[0], "first");
    EXPECT_EQ(order[1], "second");
}

// Two independent subscribers on one button: both fire, and revoking one leaves the other.
// Additivity is the property that makes the handler table safe for the scripting ABI.
TEST(ButtonClickEventTests, ClickSubscriptionsAreAdditiveAndIndividuallyRevocable)
{
    Button btn;
    int a = 0, b = 0;
    auto tokenA = btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++a; });
    btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++b; });

    btn.TriggerClick();
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);

    EXPECT_TRUE(btn.UnregisterEventHandler(tokenA));
    btn.TriggerClick();
    EXPECT_EQ(a, 1) << "revoked subscription still fired";
    EXPECT_EQ(b, 2) << "revoking one subscription took the other with it";
}

// ---------------------------------------------------------------------------
// SetOnClick — the convenience wrapper. It is a registration and nothing else, so
// what these pin is the ONE place it differs from RegisterEventHandler (a single
// slot that replaces itself) and the places it must NOT differ (payload, ordering,
// and every other subscriber's registration).
// ---------------------------------------------------------------------------

// The whole point of the single signature: one callback gets the entire event, so a
// caller that wants modifiers reads them off the same parameter a caller that ignores
// them leaves untouched. This is what removed the need for a ClickHandler /
// ClickHandlerWithMods split.
TEST(ButtonClickEventTests, SetOnClickDeliversTheWholeEvent)
{
    Button btn;
    int clicks = 0;
    int mods = -1;
    const UIElement* target = nullptr;
    EventId id = 0;
    btn.SetOnClick(
        [&](UIEvent& e)
        {
            ++clicks;
            mods = e.Mods;
            target = e.Target;
            id = e.Id;
        });

    // A literal bitmask, as elsewhere in this file: the modifier constants live in
    // InputSystem.h and what is under test is that Mods arrives intact, not its encoding.
    constexpr int kMods = 0b0101;
    btn.TriggerClick(kMods);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(mods, kMods)
        << "the convenience callback must receive the same payload a table subscriber does";
    EXPECT_EQ(target, &btn);
    EXPECT_EQ(id, kEventButtonClick);
}

// Keyboard activation reaches it too — it is a subscription like any other, so there is
// no path by which a button wired this way is mouse-only.
TEST(ButtonClickEventTests, SetOnClickReceivesKeyboardActivation)
{
    Button btn;
    int clicks = 0;
    btn.SetOnClick([&](UIEvent&) { ++clicks; });

    Send(btn, kEventKeyDown, Input::kKeyCode_Space);
    EXPECT_EQ(clicks, 1);
}

// SET, not ADD. This is the assertion that makes the name honest: the second call
// revokes the first rather than stacking on it.
TEST(ButtonClickEventTests, SetOnClickReplacesThePreviousCallback)
{
    Button btn;
    int first = 0, second = 0;
    btn.SetOnClick([&](UIEvent&) { ++first; });
    btn.SetOnClick([&](UIEvent&) { ++second; });

    btn.TriggerClick();
    EXPECT_EQ(first, 0) << "the replaced callback still fired — SetOnClick added instead of set";
    EXPECT_EQ(second, 1);
    // Counted AFTER the dispatch on purpose. UnregisterEventHandler deactivates in place
    // and DrainInactiveHandlers — which runs when the outermost frame unwinds — is what
    // erases it, so between the two SetOnClick
    // calls the table legitimately holds two entries, one of them inactive. A set/clear
    // cycle therefore settles at the next dispatch rather than immediately — the same
    // bound a direct register/unregister pair has.
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 1u)
        << "the replaced subscription was not swept";
}

// Clearing. The button must be left with no click subscription at all, not with a
// dead entry that still counts.
TEST(ButtonClickEventTests, SetOnClickWithNullptrUnregisters)
{
    Button btn;
    int clicks = 0;
    btn.SetOnClick([&](UIEvent&) { ++clicks; });
    btn.SetOnClick(nullptr);

    btn.TriggerClick();
    EXPECT_EQ(clicks, 0) << "the cleared callback still fired";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 0u)
        << "clearing left the entry behind; a set/clear cycle would accumulate them";

    // And clearing an already-clear button is not an error.
    btn.SetOnClick(nullptr);
    btn.TriggerClick();
    EXPECT_EQ(clicks, 0);
}

// The constraint §6 rejected the m_OnClick-adoption design over, stated as an assertion:
// the convenience slot may only ever revoke ITS OWN subscription. A direct
// RegisterEventHandler subscriber — a built-in, the scripting ABI, another panel —
// survives any number of SetOnClick calls. Registration order is also pinned here: a
// callback set through the wrapper has no ordering privilege, because it is nothing but
// a table entry minted when SetOnClick ran.
TEST(ButtonClickEventTests, SetOnClickNeverDisplacesADirectSubscriber)
{
    Button btn;
    std::vector<std::string> order;
    int direct = 0;

    btn.SetOnClick([&](UIEvent&) { order.push_back("wrapper-first"); });
    btn.RegisterEventHandler(kEventButtonClick,
                             [&](UIEvent&)
                             {
                                 ++direct;
                                 order.push_back("direct");
                             });
    btn.SetOnClick([&](UIEvent&) { order.push_back("wrapper-second"); });

    btn.TriggerClick();
    EXPECT_EQ(direct, 1) << "a direct subscriber was destroyed by SetOnClick";
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "direct")
        << "the replacement re-registered at the tail, so it must fire after the subscriber "
           "that registered before it — the wrapper carries no ordering privilege";
    EXPECT_EQ(order[1], "wrapper-second");

    btn.SetOnClick(nullptr);
    btn.TriggerClick();
    EXPECT_EQ(direct, 2) << "clearing the convenience slot took a direct subscriber with it";
}

// Re-entrant replacement: a callback rewiring the button from inside its own dispatch.
// The revoke-then-register order is what makes this safe — UnregisterEventHandler keeps
// an executing callable alive and lets the drain erase it once no frame stands on it, so
// the frame is not running out of freed captures. The replacement must not also fire in
// the dispatch that installed it: the walk bounds itself by the highest key issued before
// its first call, and the replacement's key is above that.
//
// THIS TEST BLESSES AN IDIOM WHOSE SAFETY IS NOT LOCAL. It rests on the rule that the
// entry a live frame is executing is never erased under it, which is now enforced by
// DrainInactiveHandlers running ONLY from the outermost frame — the dispatch tail and the
// last ExecutingHandlerScope to unwind (UIElement.h).
//
// That rule is younger than the idiom. Under the previous end-of-dispatch compaction a
// nested SAME-id dispatch erased the outer frame's executing-but-deactivated entry and
// freed its closure under the live frame — a heap-UAF reproduced under ASan two ways,
// filed as an escape against main in the 2026-08-18 review row of this branch and closed
// by fix/dispatch-compaction-uaf (ledger row 2026-08-19). §6's old nested-dispatch safety
// argument scoped to DIFFERING ids only, which is why Arm 11 below did not cover it.
//
// So: if the outermost-frame rule is ever weakened, this is the test that says the idiom
// was sanctioned. Read it together with that escape before changing the drain.
TEST(ButtonClickEventTests, SetOnClickCanReplaceItselfFromInsideItsOwnDispatch)
{
    Button btn;
    int original = 0, replacement = 0;
    btn.SetOnClick(
        [&](UIEvent&)
        {
            ++original;
            btn.SetOnClick([&](UIEvent&) { ++replacement; });
        });

    btn.TriggerClick();
    EXPECT_EQ(original, 1);
    EXPECT_EQ(replacement, 0) << "the replacement fired in the same dispatch that installed it";

    btn.TriggerClick();
    EXPECT_EQ(original, 1) << "the replaced callback survived its own replacement";
    EXPECT_EQ(replacement, 1);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 1u)
        << "the self-replacement leaked an entry";
}

// Arm 11. Nested dispatch on ONE element: a mouse-up subscriber triggers the click, so the
// kEventButtonClick dispatch runs while the outer kEventMouseUp dispatch holds a reference into
// m_EventHandlers. The nested dispatch inserts and can rehash the map; std::unordered_map
// rehashing invalidates iterators but not references to mapped values, which is what keeps
// the outer `vec` valid. Many distinct event ids are registered first so the insertion
// really does rehash rather than fitting in the initial bucket count.
//
// Red the day m_EventHandlers becomes a flat/open-addressed map — the outer reference would
// dangle, and under ASan this is a use-after-free rather than a silent pass.
TEST(ButtonClickEventTests, NestedClickDispatchInsideMouseUpDispatchIsSafe)
{
    Button btn;

    // Fill the table with unrelated ids so the kEventButtonClick insertion below grows it.
    for (int i = 0; i < 64; ++i)
        btn.RegisterEventHandler(HashEvent("UI.Filler" + std::to_string(i)), [](UIEvent&) {});

    int outer = 0, inner = 0;
    btn.RegisterEventHandler(kEventMouseUp,
                             [&](UIEvent&)
                             {
                                 ++outer;
                                 // Registering the inner subscription DURING the outer dispatch
                                 // is what forces the insert+rehash to happen under the
                                 // outer's live reference.
                                 btn.RegisterEventHandler(kEventButtonClick, [&](UIEvent&) { ++inner; });
                                 btn.TriggerClick();
                             });
    // A second outer subscriber: the loop must survive the nested dispatch and keep going.
    int outerTail = 0;
    btn.RegisterEventHandler(kEventMouseUp, [&](UIEvent&) { ++outerTail; });

    Send(btn, kEventMouseUp);

    EXPECT_EQ(outer, 1);
    EXPECT_EQ(inner, 1) << "the nested click dispatch did not reach its subscriber";
    EXPECT_EQ(outerTail, 1) << "the outer dispatch loop did not survive the nested dispatch";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventButtonClick), 1u);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(btn, kEventMouseUp), 2u);
}
