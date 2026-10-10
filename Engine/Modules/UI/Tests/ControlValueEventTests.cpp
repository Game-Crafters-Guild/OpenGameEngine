// The controls' first-class value events (kEventValueChanging / kEventValueChanged) and the
// ScrollView's offset event (kEventScrollOffsetChanged).
//
// Every arm drives the control's REAL mutation path — Slider::SetValue, ToggleBase::SetValue,
// ScrollView::SetScrollY + the manager's flush — rather than dispatching the event by hand,
// because what is under test is that the notification sites reach the handler table at all.
// A synthetic DispatchEvent would pass on a build where no control dispatches anything.
//
// The properties pinned here are the ones a subscriber's correctness rests on: the member
// callbacks and the table coexist (a subscription must never displace a built-in's callback),
// SetValueWithoutNotify raises neither, an unchanged write raises nothing, the payload carries
// the committed value — numeric in UIEvent::Value, string in UIEvent::Text with its
// dispatch-window lifetime guarantee — and a value type no payload can carry dispatches
// NOTHING rather than an event with a meaningless number in it.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include "Input/KeyCodes.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
struct ValueLog
{
    std::vector<std::string> Order;
    std::vector<float> Changed;
    std::vector<float> Changing;
};

// Subscribe both value events on any Field, recording order and payload.
void WatchValues(UIElement& el, ValueLog& log)
{
    el.RegisterEventHandler(kEventValueChanging,
                            [&log](UIEvent& e)
                            {
                                log.Order.push_back("event:changing");
                                log.Changing.push_back(e.Value);
                            });
    el.RegisterEventHandler(kEventValueChanged,
                            [&log](UIEvent& e)
                            {
                                log.Order.push_back("event:changed");
                                log.Changed.push_back(e.Value);
                            });
}
} // namespace

// A field's Enter hook runs on Enter alone: not on blur or Escape, which also commit.
TEST(ControlValueEventTests, TheEnterHookRunsOnEnterOnly)
{
    TextField field;
    int entered = 0;
    field.SetOnEnter([&entered]() { ++entered; });
    field.OnFocusChanged(true);
    field.OnKey(Input::kKeyCode_Escape, 0, nullptr);
    field.OnFocusChanged(false);
    EXPECT_EQ(entered, 0);
    EXPECT_TRUE(field.OnKey(Input::kKeyCode_Enter, 0, nullptr));
    EXPECT_TRUE(field.OnKey(Input::kKeyCode_NumPadEnter, 0, nullptr));
    EXPECT_EQ(entered, 2);
}

TEST(ControlValueEventTests, SliderSetValueDispatchesBothEvents)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    ValueLog log;
    WatchValues(slider, log);

    slider.SetValue(4.0f);

    ASSERT_EQ(log.Changed.size(), 1u) << "the control's own SetValue must reach the handler table";
    EXPECT_FLOAT_EQ(log.Changed[0], 4.0f) << "the payload carries the committed value";
    ASSERT_EQ(log.Changing.size(), 1u);
    EXPECT_FLOAT_EQ(log.Changing[0], 4.0f);
    EXPECT_EQ(log.Order, (std::vector<std::string>{"event:changing", "event:changed"}))
        << "Changing precedes Changed, as the member callbacks already order them";
}

// The payload is the CLAMPED, quantised value the control committed — not the number the
// caller passed. A subscriber that had to re-derive it would drift from what a drag produces.
TEST(ControlValueEventTests, PayloadIsTheCommittedValueNotTheRequestedOne)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    ValueLog log;
    WatchValues(slider, log);

    slider.SetValue(999.0f);

    ASSERT_EQ(log.Changed.size(), 1u);
    EXPECT_FLOAT_EQ(log.Changed[0], slider.GetValue());
    EXPECT_FLOAT_EQ(log.Changed[0], 10.0f) << "clamped to the domain before anyone is told";
}

// Arm: a script subscribing must never destroy a built-in's callback, and a built-in setting
// one must never destroy the script's subscription. Same constraint ButtonClickEventTests
// pins for Button — this is why the ABI binds the table and not the member slot.
TEST(ControlValueEventTests, MemberCallbackAndSubscriptionCoexistAndOrder)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    ValueLog log;

    slider.SetOnValueChanged([&log](const float&) { log.Order.push_back("member:changed"); });
    WatchValues(slider, log);

    slider.SetValue(3.0f);

    EXPECT_EQ(log.Order, (std::vector<std::string>{"event:changing", "member:changed", "event:changed"}))
        << "members fire first within each notification, then that notification's subscribers";
    EXPECT_EQ(log.Changed.size(), 1u) << "the member callback did not displace the subscription";
}

TEST(ControlValueEventTests, SetValueWithoutNotifyRaisesNothing)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    ValueLog log;
    WatchValues(slider, log);

    slider.SetValueWithoutNotify(7.0f);

    EXPECT_FLOAT_EQ(slider.GetValue(), 7.0f) << "the value did change";
    EXPECT_TRUE(log.Order.empty()) << "SetValueWithoutNotify is the documented way to break a "
                                      "feedback loop; dispatching from it would remove that escape";
}

// The equality early-out in each control is what protects bulk writes: the inspector writes
// fields it has already written, and an unchanged write must cost nothing at all.
TEST(ControlValueEventTests, UnchangedWriteRaisesNothing)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    slider.SetValue(2.0f);

    ValueLog log;
    WatchValues(slider, log);
    slider.SetValue(2.0f);

    EXPECT_TRUE(log.Order.empty());
}

TEST(ControlValueEventTests, ToggleReportsItsBoolAsZeroOrOne)
{
    Checkbox toggle;
    ValueLog log;
    WatchValues(toggle, log);

    toggle.SetChecked(true);
    ASSERT_EQ(log.Changed.size(), 1u);
    EXPECT_FLOAT_EQ(log.Changed[0], 1.0f);

    toggle.SetChecked(false);
    ASSERT_EQ(log.Changed.size(), 2u);
    EXPECT_FLOAT_EQ(log.Changed[1], 0.0f);
}

// A Field<std::string> dispatches its value through UIEvent::Text — UTF-8, dispatch-stable —
// while UIEvent::Value stays zero. This is the payload TextField's commit and Dropdown's
// selection ride; the drive here is the same public pair the controls' own paths call.
TEST(ControlValueEventTests, StringFieldDispatchesItsValueAsText)
{
    TextField field;
    std::vector<std::string> changing;
    std::vector<std::string> changed;
    float valueSeen = -1.0f;
    field.RegisterEventHandler(kEventValueChanging,
                               [&](UIEvent& e) { changing.emplace_back(e.Text); });
    field.RegisterEventHandler(kEventValueChanged,
                               [&](UIEvent& e)
                               {
                                   changed.emplace_back(e.Text);
                                   valueSeen = e.Value;
                               });

    field.SetValue(std::string("hello"));
    field.NotifyValueChanging();
    field.NotifyValueChanged();

    EXPECT_EQ(changing, (std::vector<std::string>{"hello"}));
    EXPECT_EQ(changed, (std::vector<std::string>{"hello"}));
    EXPECT_FLOAT_EQ(valueSeen, 0.0f) << "a string field has no numeric payload to put in Value";
}

// THE LIFETIME GUARANTEE, pinned from inside a handler: the Text payload every subscriber
// of ONE dispatch sees is the value as it was when they were told, however a handler mutates
// the field mid-dispatch. The mutating write
// reallocates the field's own buffer — without the dispatch-window copy this is a dangling
// view handed to the next subscriber, not a wrong value.
TEST(ControlValueEventTests, TextPayloadSurvivesAHandlerWritingTheField)
{
    const std::string original = "original-payload-well-beyond-sso-capacity";
    const std::string replacement = "a replacement long enough to force a fresh heap allocation";

    BaseField field; // Field<std::string>, driven exactly as TextFieldBase's commit drives it
    field.SetValue(original);

    std::vector<std::string> firstSaw;
    std::vector<std::string> secondSaw;
    field.RegisterEventHandler(kEventValueChanged,
                               [&](UIEvent& e)
                               {
                                   firstSaw.emplace_back(e.Text);
                                   // The ordinary validating shape: write the field being
                                   // handled. The write applies (downgraded, not dropped) and
                                   // the nested notify is swallowed by the re-entrancy guard.
                                   field.SetValue(replacement);
                                   field.NotifyValueChanged();
                                   // The payload this handler was given must not have moved
                                   // under it either.
                                   firstSaw.emplace_back(e.Text);
                               });
    field.RegisterEventHandler(kEventValueChanged,
                               [&](UIEvent& e) { secondSaw.emplace_back(e.Text); });

    field.NotifyValueChanged();

    EXPECT_EQ(firstSaw, (std::vector<std::string>{original, original}));
    EXPECT_EQ(secondSaw, (std::vector<std::string>{original}))
        << "the subscriber AFTER the mutating one read the buffer the mutation freed";
    EXPECT_EQ(field.GetValue(), replacement) << "the nested write must still APPLY";
}

// A handler for one string field may write ANOTHER string field, whose notification opens a
// nested dispatch window while the outer one is still live. Each window owns its own buffer
// (the per-thread stack in ScopedValueText), so the outer payload survives the nested
// dispatch — including the container growth the first-ever nesting causes.
//
// THE DISPATCH RUNS ON A DEDICATED THREAD, and that is what makes this arm an instrument
// rather than a decoration. The buffer stack is thread_local, and what is pinned here is the
// FIRST nesting on a thread: growth while level 0 still holds an SSO string, so a container
// that relocated its elements would move the very bytes the outer view points at. Any earlier
// string dispatch on the same thread leaves level 0 holding heap capacity, and moving THAT
// string moves a pointer rather than the bytes — so on a warm thread this arm passes under the
// exact defect it exists to catch, including on its own second run. Only the dispatch is
// off-thread: the per-thread state under test is the window stack, not the elements.
TEST(ControlValueEventTests, ANestedStringDispatchKeepsTheOuterPayloadIntact)
{
    BaseField outer;
    BaseField inner;
    outer.SetValue(std::string("short")); // SSO-sized on purpose: the worst case for a
                                          // buffer that relocates when the stack grows
    inner.SetValue(std::string("the inner field's own value, long enough to live on the heap"));

    std::string outerBefore;
    std::string outerAfter;
    std::string innerSaw;
    inner.RegisterEventHandler(kEventValueChanged, [&](UIEvent& e) { innerSaw = std::string(e.Text); });
    outer.RegisterEventHandler(kEventValueChanged,
                               [&](UIEvent& e)
                               {
                                   outerBefore = std::string(e.Text);
                                   inner.NotifyValueChanged(); // nested window
                                   outerAfter = std::string(e.Text);
                               });

    std::thread dispatch([&] { outer.NotifyValueChanged(); });
    dispatch.join();

    EXPECT_EQ(outerBefore, "short");
    EXPECT_EQ(outerAfter, "short") << "the nested window relocated the outer window's buffer";
    EXPECT_EQ(innerSaw, inner.GetValue());
}

// Dropdown is a Field<std::string> whose value is the selected option's VALUE string — that
// is what its selection change dispatches. The label is presentation and readable separately.
TEST(ControlValueEventTests, DropdownSelectionDispatchesTheOptionValue)
{
    Dropdown dropdown;
    dropdown.SetOptions({{"val-a", "Label A", ""}, {"val-b", "Label B", ""}}, 0);

    std::vector<std::string> changed;
    dropdown.RegisterEventHandler(kEventValueChanged,
                                  [&](UIEvent& e) { changed.emplace_back(e.Text); });

    dropdown.SetSelectedIndex(1);
    EXPECT_EQ(changed, (std::vector<std::string>{"val-b"}));
    EXPECT_EQ(dropdown.GetSelectedLabel(), "Label B");

    dropdown.SetSelectedIndexWithoutNotify(0);
    EXPECT_EQ(changed.size(), 1u) << "the without-notify selection told the handler table anyway";
    EXPECT_EQ(dropdown.GetSelectedValue(), "val-a");
    EXPECT_EQ(dropdown.GetSelectedLabel(), "Label A");
}

// The string path's one deliberate asymmetry: with nothing subscribed, the dispatch — and
// with it the control's own OnEvent — is skipped entirely, because the presence gate is what
// keeps the dispatch-window copy off the bulk-write path. Sound for THIS event kind only:
// value events are self-dispatched, so a control never legitimately learns of its own value
// change from its own OnEvent. The numeric path keeps OnEvent unconditional; if this arm
// starts failing because someone made the paths uniform, the copy is now paid on every
// unsubscribed programmatic text write — see the allocation arms before accepting that.
TEST(ControlValueEventTests, AnUnsubscribedStringFieldSkipsItsOwnOnEventToo)
{
    struct ProbeField : BaseField
    {
        int ValueEventsSeen = 0;
        void OnEvent(UIEvent& e) override
        {
            if (e.Id == kEventValueChanged)
                ++ValueEventsSeen;
            BaseField::OnEvent(e);
        }
    };

    ProbeField field;
    field.SetValue(std::string("one"));
    field.NotifyValueChanged();
    EXPECT_EQ(field.ValueEventsSeen, 0) << "nothing subscribed: the gate skips the whole dispatch";

    int handlerRan = 0;
    field.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++handlerRan; });
    field.SetValue(std::string("two"));
    field.NotifyValueChanged();
    EXPECT_EQ(field.ValueEventsSeen, 1) << "subscribed: OnEvent runs first, as in every dispatch";
    EXPECT_EQ(handlerRan, 1);
}

// RE-ENTRANCY, prevented at the root rather than bounded.
//
// A handler writing back to the field it is handling used to recurse: the control's
// unchanged-value early-out ends a handler that writes what it read, but not one that writes
// something new each time, and that was an unbounded stack overflow. Now the nested write
// applies with the control's normal clamping and simply does not notify — SetValueWithoutNotify
// semantics, arrived at by downgrade.
//
// The assertion is EXACT, not a bound: one external change produces exactly one notification,
// whatever the handler does. Restoring plain recursion turns this back into the old runaway.
TEST(ControlValueEventTests, ANestedWriteAppliesWithoutNotifyingAgain)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(1000.0f);

    int changed = 0;
    std::vector<float> seen;
    slider.RegisterEventHandler(kEventValueChanged,
                                [&](UIEvent& e)
                                {
                                    ++changed;
                                    seen.push_back(e.Value);
                                    // Always a different value: the equality early-out can never
                                    // stop this, which is exactly the shape that used to recurse.
                                    slider.SetValue(e.Value + 1.0f);
                                });

    slider.SetValue(1.0f);

    EXPECT_EQ(changed, 1) << "the nested write notified again — recursion is back";
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_FLOAT_EQ(seen[0], 1.0f) << "subscribers saw the value as it was when they were told";
    EXPECT_FLOAT_EQ(slider.GetValue(), 2.0f)
        << "the nested write must still APPLY — it is downgraded, not dropped";
}

// The member slot and the handler table are downgraded together. A cap that still ran the member
// callback would bound nothing, and the members are where the editor's own logic lives.
TEST(ControlValueEventTests, ANestedWriteSkipsTheMemberCallbackToo)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(1000.0f);

    int memberCalls = 0;
    slider.SetOnValueChanged(
        [&](const float& v)
        {
            ++memberCalls;
            slider.SetValue(v + 1.0f);
        });

    slider.SetValue(1.0f);

    EXPECT_EQ(memberCalls, 1);
    EXPECT_FLOAT_EQ(slider.GetValue(), 2.0f);
}

// The clamp fight: two fields that write each other toward incompatible targets. Under recursion
// this never settled. Now it terminates because the write back into the first field arrives while
// that field is still notifying, so it applies without notifying — and both fields end on values
// that are consistent with what each was last told.
TEST(ControlValueEventTests, MutualClampingFieldsSettleAndStayConsistent)
{
    Slider a;
    a.SetMin(0.0f);
    a.SetMax(100.0f);
    Slider b;
    b.SetMin(0.0f);
    b.SetMax(100.0f);

    int aNotifications = 0;
    int bNotifications = 0;

    // a pushes b above itself; b pushes a below itself. No pair satisfies both.
    a.RegisterEventHandler(kEventValueChanged,
                           [&](UIEvent& e)
                           {
                               ++aNotifications;
                               b.SetValue(e.Value + 10.0f);
                           });
    b.RegisterEventHandler(kEventValueChanged,
                           [&](UIEvent& e)
                           {
                               ++bNotifications;
                               a.SetValue(e.Value - 20.0f);
                           });

    a.SetValue(50.0f); // must return

    EXPECT_EQ(aNotifications, 1) << "a's re-entry from b must not notify a second time";
    EXPECT_EQ(bNotifications, 1);
    // a: 50 notified -> b: 60 notified -> a: 40 applied silently. Final state is settled and
    // every value is one the control actually holds; nothing is mid-flight.
    EXPECT_FLOAT_EQ(a.GetValue(), 40.0f);
    EXPECT_FLOAT_EQ(b.GetValue(), 60.0f);
}

// A handler writing back the value it was handed is the ordinary validating shape and must stay
// cheap: the control's equality early-out returns before it ever reaches the notification.
TEST(ControlValueEventTests, HandlerWritingTheSameValueTerminatesImmediately)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    int changed = 0;
    slider.RegisterEventHandler(kEventValueChanged,
                                [&](UIEvent& e)
                                {
                                    ++changed;
                                    slider.SetValue(e.Value);
                                });

    slider.SetValue(5.0f);
    EXPECT_EQ(changed, 1);
}

// The legitimate deep chain, and the one the downgrade must NOT touch: a parent notifying its
// children is not re-entry on one element. Each element tracks its own notification, so a fan-out
// notifies fully at every level — this is the arm that goes red if the flag is ever hoisted to
// something shared.
TEST(ControlValueEventTests, AParentDrivingChildFieldsNotifiesEveryLevel)
{
    Slider parent;
    parent.SetMin(0.0f);
    parent.SetMax(100.0f);
    Slider childX;
    childX.SetMin(0.0f);
    childX.SetMax(100.0f);
    Slider childY;
    childY.SetMin(0.0f);
    childY.SetMax(100.0f);

    int parentChanged = 0;
    int xChanged = 0;
    int yChanged = 0;
    parent.RegisterEventHandler(kEventValueChanged,
                                [&](UIEvent& e)
                                {
                                    ++parentChanged;
                                    childX.SetValue(e.Value);
                                    childY.SetValue(e.Value * 0.5f);
                                });
    childX.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++xChanged; });
    childY.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++yChanged; });

    parent.SetValue(20.0f);

    EXPECT_EQ(parentChanged, 1);
    EXPECT_EQ(xChanged, 1) << "a child driven from the parent's handler must notify normally";
    EXPECT_EQ(yChanged, 1);
    EXPECT_FLOAT_EQ(childX.GetValue(), 20.0f);
    EXPECT_FLOAT_EQ(childY.GetValue(), 10.0f);
}

// The API the downgrade points at, on the controls that override it: the value and every visual
// consequence land, and nobody is told — including the handler table, which is the half a
// pre-event implementation could not have covered.
TEST(ControlValueEventTests, SetValueWithoutNotifyTellsNobody)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    int sliderEvents = 0;
    int sliderMembers = 0;
    slider.SetOnValueChanged([&](const float&) { ++sliderMembers; });
    slider.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++sliderEvents; });

    slider.SetValueWithoutNotify(7.0f);
    EXPECT_FLOAT_EQ(slider.GetValue(), 7.0f);
    EXPECT_EQ(sliderEvents, 0);
    EXPECT_EQ(sliderMembers, 0);

    Checkbox toggle;
    int toggleEvents = 0;
    toggle.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++toggleEvents; });
    toggle.SetValueWithoutNotify(true);
    EXPECT_TRUE(toggle.IsChecked()) << "the value and its checked-class sync still land";
    EXPECT_EQ(toggleEvents, 0);
}

// What makes the scripting ABI's registration guard CORRECT, pinned as a fact about the
// controls rather than as a restatement of the guard: the ABI refuses a scroll-offset
// subscription on anything but a ScrollView, and refuses a value subscription on a control
// whose value type does not dispatch. Both refusals are only right while these hold. This goes
// red the day another control learns to dispatch one of them — which is exactly the day
// UIElementABI.cpp's ElementDispatchesEvent must widen with it.
//
// (The export itself lives in GameEngine.Native.dll; its type rules are driven end to end in
// GameUIElementHandleTests and mirrored in the managed suite's element double.)
TEST(ControlValueEventTests, OnlyTheDocumentedControlsDispatchTheseEvents)
{
    Slider slider;
    slider.SetMin(0.0f);
    slider.SetMax(10.0f);
    int sliderScrolls = 0;
    slider.RegisterEventHandler(kEventScrollOffsetChanged, [&](UIEvent&) { ++sliderScrolls; });
    slider.SetValue(5.0f);
    EXPECT_EQ(sliderScrolls, 0) << "a Slider is not a ScrollView, however value-shaped it is";

    ScrollView view;
    view.SetContentSize(100.0f, 1000.0f);
    view.SetViewportSize(100.0f, 100.0f);
    int viewValues = 0;
    view.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++viewValues; });
    view.SetScrollY(30.0f);
    view.FlushPendingScrollChanged();
    EXPECT_EQ(viewValues, 0) << "a ScrollView has no Field value to report";

    UIElement plain;
    int plainEvents = 0;
    plain.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++plainEvents; });
    plain.RegisterEventHandler(kEventScrollOffsetChanged, [&](UIEvent&) { ++plainEvents; });
    EXPECT_EQ(plainEvents, 0) << "nothing dispatches either of these on a plain element";
}

TEST(ControlValueEventTests, ScrollViewOffsetChangeDispatchesOnFlush)
{
    ScrollView view;
    view.SetContentSize(100.0f, 1000.0f);
    view.SetViewportSize(100.0f, 100.0f);

    int events = 0;
    float x = -1.0f;
    float y = -1.0f;
    view.RegisterEventHandler(kEventScrollOffsetChanged,
                              [&](UIEvent& e)
                              {
                                  ++events;
                                  x = e.ScrollX;
                                  y = e.ScrollY;
                              });

    view.SetScrollY(40.0f);
    EXPECT_EQ(events, 0) << "the offset event is deferred to the manager's flush, not raised "
                            "inside the scroll machinery";

    EXPECT_TRUE(view.FlushPendingScrollChanged());
    EXPECT_EQ(events, 1);
    EXPECT_FLOAT_EQ(y, 40.0f);
    EXPECT_FLOAT_EQ(x, 0.0f);
}

// Coalescing is the reason the event hangs off the flush: a wheel spin writes the offset many
// times per frame, and a subscriber wants where it ENDED, once.
TEST(ControlValueEventTests, MultipleOffsetWritesCoalesceIntoOneEvent)
{
    ScrollView view;
    view.SetContentSize(100.0f, 1000.0f);
    view.SetViewportSize(100.0f, 100.0f);

    int events = 0;
    float y = -1.0f;
    view.RegisterEventHandler(kEventScrollOffsetChanged,
                              [&](UIEvent& e)
                              {
                                  ++events;
                                  y = e.ScrollY;
                              });

    view.SetScrollY(10.0f);
    view.SetScrollY(20.0f);
    view.SetScrollY(30.0f);
    EXPECT_TRUE(view.FlushPendingScrollChanged());

    EXPECT_EQ(events, 1);
    EXPECT_FLOAT_EQ(y, 30.0f) << "the settled offset, not the first one";

    EXPECT_FALSE(view.FlushPendingScrollChanged()) << "nothing pending, nothing raised";
    EXPECT_EQ(events, 1);
}

// The flush dispatches subscriber code, and a subscriber is entitled to mutate the tree — a HUD
// closing the panel it just scrolled is the obvious case. Without the dispatch-depth guard,
// UIElement::RemoveChild does NOT defer, so the removal would destroy the ScrollView while
// FlushPendingScrollChanged is still running on it: a use-after-free on `this`, and on every
// later raw pointer in UIManager's flush loop.
//
// The arm asserts the OBSERVABLE consequence of the guard — the child survives the dispatch and
// leaves later, at the safe point — because "did not crash" is not an assertion.
TEST(ControlValueEventTests, AHandlerRemovingTheTreeDuringTheFlushIsDeferred)
{
    UIElement root;
    auto ownedView = std::make_unique<ScrollView>();
    ScrollView* view = ownedView.get();
    root.AddChild(std::move(ownedView));
    view->SetContentSize(100.0f, 1000.0f);
    view->SetViewportSize(100.0f, 100.0f);

    bool sawSelfStillParented = false;
    view->RegisterEventHandler(kEventScrollOffsetChanged,
                               [&](UIEvent&)
                               {
                                   EXPECT_TRUE(UIElement::IsInEventDispatch())
                                       << "the flush must run subscribers inside a dispatch scope, or "
                                          "tree mutation from a handler is immediate and unsafe";
                                   root.RemoveChild(view);
                                   // Deferred, so the element is still ours for the rest of this call.
                                   sawSelfStillParented = (root.GetChildren().size() == 1);
                               });

    view->SetScrollY(50.0f);
    view->FlushPendingScrollChanged();

    EXPECT_TRUE(sawSelfStillParented)
        << "RemoveChild ran immediately during the dispatch — the ScrollView would have been "
           "destroyed under FlushPendingScrollChanged's own frame";
    EXPECT_FALSE(UIElement::IsInEventDispatch()) << "the scope must be released on the way out";
}

// The dispatch scope around the flush is an INPUT to the virtualized views, not just a safety
// guard: ListView, GridView and TreeView drive UpdateVirtualization from m_OnScrollChanged and
// pass `allowShrink = !UIElement::IsInEventDispatch()`, so raising the depth here defers pool
// shrink out of scroll flushes. This arm pins that input; the deferral it produces is pinned
// from the other end by VirtualWindowCoreTests.ShrinkBlockedWhileInDispatchFiresOnNextSafeUpdate,
// which drives the core directly with allowShrink false then true.
//
// Kept separate from the use-after-free arm above because it is a different claim about the same
// scope, and the day someone narrows the guard to "only while a subscriber exists" this is the
// one that goes red.
TEST(ControlValueEventTests, ScrollFlushRaisesTheDispatchDepth)
{
    ScrollView view;
    view.SetContentSize(100.0f, 1000.0f);
    view.SetViewportSize(100.0f, 100.0f);

    ASSERT_FALSE(UIElement::IsInEventDispatch()) << "precondition: nothing else raised it";

    bool insideDispatch = false;
    view.SetOnScrollChanged([&](float, float) { insideDispatch = UIElement::IsInEventDispatch(); });

    view.SetScrollY(20.0f);
    ASSERT_TRUE(view.FlushPendingScrollChanged());

    EXPECT_TRUE(insideDispatch)
        << "the virtualization callback ran outside a dispatch scope — the views would then pass "
           "allowShrink=true and destroy pooled elements from inside the flush";
    EXPECT_FALSE(UIElement::IsInEventDispatch()) << "and the scope must be released on the way out";
}

// Which value types dispatch is a payload question, and the answer for `int` is deliberately
// "not yet" rather than "obviously fine". A float payload represents int32 exactly only up to
// 2^24; above that a value event would report a DIFFERENT number than the control holds, and
// silently. int therefore still waits for a properly typed payload of its own. This arm pins
// the deferral so it is a decision rather than an oversight, and goes red the day a
// FieldEventValue<int> appears without one. It also pins each dispatching type to exactly ONE
// payload shape — a type that claimed both would write Value and Text in the same event.
TEST(ControlValueEventTests, PayloadShapesArePinnedPerValueType)
{
    EXPECT_FALSE(FieldEventValue<int>::kNumericPayload)
        << "an int32 above 2^24 is not exactly representable in the float payload; adding this "
           "specialisation needs a payload that can carry it, plus the matching ABI guard row";
    EXPECT_FALSE(FieldEventValue<int>::kTextPayload);

    // Both flags on every dispatching type, so "exactly one shape" is what is pinned — not
    // just "the expected one is on".
    EXPECT_TRUE(FieldEventValue<float>::kNumericPayload);
    EXPECT_FALSE(FieldEventValue<float>::kTextPayload);
    EXPECT_TRUE(FieldEventValue<bool>::kNumericPayload);
    EXPECT_FALSE(FieldEventValue<bool>::kTextPayload);
    EXPECT_FALSE(FieldEventValue<std::string>::kNumericPayload);
    EXPECT_TRUE(FieldEventValue<std::string>::kTextPayload);
}

TEST(ControlValueEventTests, ScrollMemberCallbackAndSubscriptionCoexist)
{
    ScrollView view;
    view.SetContentSize(100.0f, 1000.0f);
    view.SetViewportSize(100.0f, 100.0f);

    std::vector<std::string> order;
    view.SetOnScrollChanged([&order](float, float) { order.push_back("member"); });
    view.RegisterEventHandler(kEventScrollOffsetChanged, [&order](UIEvent&) { order.push_back("event"); });

    view.SetScrollY(25.0f);
    view.FlushPendingScrollChanged();

    EXPECT_EQ(order, (std::vector<std::string>{"member", "event"}))
        << "the virtualization callback keeps its slot and runs first";
}
