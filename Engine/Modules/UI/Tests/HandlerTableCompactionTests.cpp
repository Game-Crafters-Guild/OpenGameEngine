// When the handler table may be COMPACTED, and what a nested dispatch on the same
// element is allowed to do to it.
//
// DispatchToHandlers invokes the LIVE table entry and holds no copy of any of them, so
// for the whole duration of a handler call there is a frame
// standing on a deque element. Compaction erases deque elements and, via remove_if,
// MOVES the ones it keeps. Either operation performed under such a frame corrupts the
// callable that frame is running out of, which is why the sweep belongs to the
// outermost dispatch on the element and to no other.
//
// These pin that rule from both sides: the entry being swept (self-unregister,
// self-replacement, a spent `once`) and the entry merely being relocated past a
// dropped neighbour. The first two are heap-use-after-free and want an ASan build to
// fail loudly; the last two are deterministic under any allocator.
#include <gtest/gtest.h>

#include "UIEventHandlerAccess.h"

#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <array>
#include <cstdint>
#include <string>

using namespace GameEngine;

namespace
{
constexpr EventId kProbeEvent = HashEvent("UI.CompactionProbe");

void Send(UIElement& el, EventId id = kProbeEvent)
{
    UIEvent e{};
    e.Id = id;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

// Big enough that MSVC puts the closure on the heap rather than in std::function's
// inline buffer, which is what turns "destroyed under its own frame" into an ASan
// heap-use-after-free instead of a read of stale-but-mapped bytes.
using FatPad = std::array<std::uint64_t, 24>;

std::uint64_t Sum(const FatPad& pad)
{
    std::uint64_t sum = 0;
    for (std::uint64_t v : pad)
        sum += v;
    return sum;
}

FatPad MakePad(std::uint64_t seed)
{
    FatPad pad{};
    for (std::size_t i = 0; i < pad.size(); ++i)
        pad[i] = seed + i;
    return pad;
}
} // namespace

// A handler that unregisters ITSELF and then dispatches the SAME id on the SAME
// element. UnregisterEventHandler deliberately keeps an executing callable alive and
// leaves the entry inactive for a compaction to sweep — so the nested dispatch finds
// an inactive entry and, if it sweeps on its own behalf, frees the closure the outer
// frame is still inside.
TEST(HandlerTableCompactionTests, SelfUnregisterThenNestedSameIdDispatchKeepsTheClosureAlive)
{
    UIElement el;
    const FatPad expected = MakePad(0xB6B6B6B6B6B6B600ull);
    int fired = 0;
    std::uint64_t observed = 0;

    UIElement::EventHandlerToken token{};
    token = el.RegisterEventHandler(
        kProbeEvent,
        [&el, &fired, &token, &observed, pad = MakePad(0xB6B6B6B6B6B6B600ull)](UIEvent&)
        {
            if (fired++ > 0)
                return;
            el.UnregisterEventHandler(token);
            Send(el);
            // Reading our own captures after the nested dispatch's compaction had
            // its chance is the whole assertion.
            observed = Sum(pad);
        });

    Send(el);

    EXPECT_EQ(fired, 1);
    EXPECT_EQ(observed, Sum(expected)) << "the executing handler's captures were destroyed by a "
                                          "nested dispatch's compaction";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 0u)
        << "the deferred sweep never happened — the outermost dispatch must erase what the "
           "nested one was not allowed to";
}

// The same mechanism through the self-REPLACEMENT idiom, spelled with the raw
// primitives so it does not depend on any convenience wrapper: a handler revokes its
// own subscription, registers a successor, and then dispatches the same id.
TEST(HandlerTableCompactionTests, SelfReplacementThenNestedSameIdDispatchKeepsTheClosureAlive)
{
    UIElement el;
    const FatPad expected = MakePad(0xA5A5A5A5A5A5A500ull);
    int original = 0;
    int replacement = 0;
    std::uint64_t observed = 0;

    UIElement::EventHandlerToken token{};
    token = el.RegisterEventHandler(
        kProbeEvent,
        [&el, &original, &replacement, &token, &observed, pad = MakePad(0xA5A5A5A5A5A5A500ull)](
            UIEvent&)
        {
            if (original++ > 0)
                return;
            el.UnregisterEventHandler(token);
            el.RegisterEventHandler(kProbeEvent, [&replacement](UIEvent&) { ++replacement; });
            Send(el);
            observed = Sum(pad);
        });

    Send(el);

    EXPECT_EQ(original, 1);
    EXPECT_EQ(observed, Sum(expected)) << "the executing handler's captures were destroyed by a "
                                          "nested dispatch's compaction";
    // The successor's key is above the bound the outer walk fixed when it started, so
    // that walk never offers it a turn; the NESTED dispatch is the one that reaches it.
    EXPECT_EQ(replacement, 1);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u)
        << "the replaced entry outlived the dispatch that was supposed to sweep it";
}

// The half a key-matched guard does not cover. Here the EXECUTING entry is never
// dropped — an earlier sibling is. remove_if compacts by moving every kept element
// down over the dropped one, so the executing entry is move-assigned out of the slot
// its live frame is running in, and the moved-from source is then destroyed. For a
// callable held in std::function's inline buffer that relocation IS the corruption:
// the frame's captures are moved away underneath it. Hence a small capture here, the
// exact opposite of the two above.
TEST(HandlerTableCompactionTests, DroppingAnEarlierEntryDoesNotRelocateTheExecutingOne)
{
    struct Fixture
    {
        UIElement* Element = nullptr;
        UIElement::EventHandlerToken EarlierToken{};
        int Depth = 0;
        int EarlierFired = 0;
        std::string Observed;
    };

    UIElement el;
    Fixture fx;
    fx.Element = &el;

    fx.EarlierToken =
        el.RegisterEventHandler(kProbeEvent, [&fx](UIEvent&) { ++fx.EarlierFired; });

    const std::string kPayload = "captures must survive a neighbour's removal";
    el.RegisterEventHandler(kProbeEvent,
                            [payload = kPayload, &fx](UIEvent&)
                            {
                                if (fx.Depth++ > 0)
                                    return;
                                // Drops the entry AHEAD of this one, so the nested
                                // compaction has something to compact past.
                                fx.Element->UnregisterEventHandler(fx.EarlierToken);
                                Send(*fx.Element);
                                fx.Observed = payload;
                            });

    Send(el);

    EXPECT_EQ(fx.Observed, kPayload)
        << "the executing entry was relocated by a nested dispatch's compaction and read its own "
           "moved-from captures";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u)
        << "the outermost dispatch did not sweep the entry the nested one deferred";
}

// A spent `once` handler must not run a second time because a nested dispatch of the
// same id reached it before any sweep did. Deactivating where it fires — rather than
// listing it for the compaction that may never come — is what makes the once contract
// hold independently of when the entry is physically erased.
TEST(HandlerTableCompactionTests, ASpentOnceHandlerDoesNotFireAgainInANestedDispatch)
{
    UIElement el;
    int onceFired = 0;
    int depth = 0;

    el.RegisterEventHandlerOnce(kProbeEvent, [&onceFired](UIEvent&) { ++onceFired; });
    el.RegisterEventHandler(kProbeEvent,
                            [&el, &depth](UIEvent&)
                            {
                                if (depth++ == 0)
                                    Send(el);
                            });

    Send(el);

    EXPECT_EQ(onceFired, 1) << "the `once` handler fired again in the nested dispatch";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u)
        << "the spent `once` entry was not swept once the outermost dispatch returned";
}

// A `once` handler running its own nested dispatch is the same hazard as a
// self-unregister: firing it deactivates it, and the nested dispatch must not treat
// that as licence to erase the entry it is executing.
TEST(HandlerTableCompactionTests, AOnceHandlerSurvivesDispatchingItsOwnIdFromInsideItself)
{
    UIElement el;
    const FatPad expected = MakePad(0xC7C7C7C7C7C7C700ull);
    int fired = 0;
    std::uint64_t observed = 0;

    el.RegisterEventHandlerOnce(kProbeEvent,
                                [&el, &fired, &observed, pad = MakePad(0xC7C7C7C7C7C7C700ull)](
                                    UIEvent&)
                                {
                                    // Bounded on purpose. An entry that is still `active` when the
                                    // nested dispatch reaches it re-enters here, and letting that
                                    // recurse freely overflows the stack — which would take the
                                    // whole single-process suite down instead of failing one test.
                                    if (++fired == 1)
                                        Send(el);
                                    observed = Sum(pad);
                                });

    Send(el);

    EXPECT_EQ(fired, 1) << "the `once` handler re-entered itself";
    EXPECT_EQ(observed, Sum(expected))
        << "the spent `once` entry was erased under its own executing frame";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 0u);
}

// A nested dispatch of a DIFFERENT id defers its sweep too — and the drain settles it
// anyway, because it runs over every id the moment the last frame unwinds rather than
// only over the id being dispatched. Deferral therefore costs nothing observable: no
// entry has to wait for its own id to be dispatched again, which is what would have
// let an exclusively-nested id accumulate without bound.
TEST(HandlerTableCompactionTests, ANestedDifferentIdsEntriesAreDrainedWhenTheOutermostFrameUnwinds)
{
    constexpr EventId kOuterEvent = HashEvent("UI.CompactionProbe.Outer");

    UIElement el;
    int innerFired = 0;
    int depth = 0;

    UIElement::EventHandlerToken innerToken =
        el.RegisterEventHandler(kProbeEvent, [&innerFired](UIEvent&) { ++innerFired; });

    el.RegisterEventHandler(kOuterEvent,
                            [&](UIEvent&)
                            {
                                if (depth++ > 0)
                                    return;
                                el.UnregisterEventHandler(innerToken);
                                Send(el, kProbeEvent); // nested, different id
                            });

    Send(el, kOuterEvent);

    EXPECT_EQ(innerFired, 0) << "an entry deactivated before the nested dispatch still fired";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 0u)
        << "the drain left an entry on an id it was not dispatching";

    // And nothing resurfaces on that id's own next dispatch.
    Send(el, kProbeEvent);
    EXPECT_EQ(innerFired, 0);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 0u);
}

// ---------------------------------------------------------------------------
// Re-entrancy from the SWEEP itself, and the deferral's accumulation bound.
//
// "No sweeping while a frame stands on the deque" has a second edge: the sweep
// destroys the callables it drops, and a capture destructor is user code that can
// dispatch on this element. It does so with no handler frame on the stack — between
// the loop and the erase — so a re-entrant dispatch passes the outermost-frame gate
// and compacts the deque the outer erase is midway through.
//
// And "swept by the next dispatch of its id that is not itself nested" is no bound
// at all when that dispatch never comes.
// ---------------------------------------------------------------------------

namespace
{
// A capture whose DESTRUCTOR dispatches on the owning element. Disarmed on copy so
// that any incidental copy stays inert and only the table-resident original (and
// whatever a move carries it into) fires — the arming has to follow the ONE object
// the table owns, or the test measures its own temporaries. Fat enough to stay off
// the inline buffer.
struct DtorDispatcher
{
    UIElement* El = nullptr;
    int* Runs = nullptr;
    bool Armed = false;
    FatPad Pad{};

    DtorDispatcher(UIElement* el, int* runs) : El(el), Runs(runs), Armed(true) {}
    DtorDispatcher(const DtorDispatcher& o) : El(o.El), Runs(o.Runs), Armed(false) {}
    DtorDispatcher(DtorDispatcher&& o) noexcept : El(o.El), Runs(o.Runs), Armed(o.Armed)
    {
        o.Armed = false;
    }
    DtorDispatcher& operator=(const DtorDispatcher&) = delete;
    DtorDispatcher& operator=(DtorDispatcher&&) = delete;
    ~DtorDispatcher()
    {
        if (!Armed)
            return;
        Armed = false;
        ++*Runs;
        Send(*El);
    }
};
} // namespace

// Layout [once(dtor), inactive, keeper] gives the erase real work — two entries to
// drop and a survivor to move down over them — so a drain re-entered from the
// destructor would have a half-compacted deque to corrupt.
//
// The armed capture does NOT die in that move. It is fat enough to be heap-held, so
// the collect pass moves it into the sink and it is destroyed on the way out of the
// drain, once the erase is done and the sweep flag has dropped. That is why the
// destructor's own dispatch sees a settled table, which is what the keeperFired
// assertion below turns on.
TEST(HandlerTableCompactionTests, ACaptureDestructorDispatchingDuringTheSweepDoesNotReenterIt)
{
    UIElement el;
    int dtorRuns = 0;
    int keeperFired = 0;

    el.RegisterEventHandlerOnce(kProbeEvent, [cap = DtorDispatcher(&el, &dtorRuns)](UIEvent&) {});

    UIElement::EventHandlerToken doomed = el.RegisterEventHandler(kProbeEvent, [](UIEvent&) {});
    el.UnregisterEventHandler(doomed);

    el.RegisterEventHandler(kProbeEvent, [&keeperFired](UIEvent&) { ++keeperFired; });

    Send(el);

    EXPECT_EQ(dtorRuns, 1) << "the probe never armed: no capture destructor ran";
    // TWO, not one, and that is the correct answer rather than a tolerated one: the
    // destructor calls Send itself, so a subscribed keeper is reached by the test's
    // dispatch and by the destructor's. Suppressing the second would be a worse bug
    // than the double-free this arm exists for — a dispatch that silently does not
    // deliver. What the fix owes is that both dispatches are well-formed: the drain
    // hands the destructor a consistent table, not one mid-erase.
    EXPECT_EQ(keeperFired, 2) << "the destructor's own dispatch did not reach the keeper";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u);

    Send(el);
    EXPECT_EQ(keeperFired, 3) << "the re-entrant sweep left the table unusable";
}

// The same mechanism through a plain SELF-UNREGISTER — the shipped editor flow of a
// handler unsubscribing itself. No `once` involved.
TEST(HandlerTableCompactionTests, ASelfUnregisterCaptureDestructorDoesNotReenterTheSweep)
{
    UIElement el;
    int dtorRuns = 0;
    int keeperFired = 0;

    UIElement::EventHandlerToken self{};
    self = el.RegisterEventHandler(kProbeEvent,
                                   [&el, &self, cap = DtorDispatcher(&el, &dtorRuns)](UIEvent&)
                                   { el.UnregisterEventHandler(self); });

    UIElement::EventHandlerToken doomed = el.RegisterEventHandler(kProbeEvent, [](UIEvent&) {});
    el.UnregisterEventHandler(doomed);

    el.RegisterEventHandler(kProbeEvent, [&keeperFired](UIEvent&) { ++keeperFired; });

    Send(el);

    EXPECT_EQ(dtorRuns, 1) << "the probe never armed";
    // Two for the same reason as above: the destructor dispatches, and that dispatch
    // must deliver.
    EXPECT_EQ(keeperFired, 2) << "the destructor's own dispatch did not reach the keeper";
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u);
}

// An id whose EVERY dispatch is nested: one dead entry would accumulate per
// register/unregister cycle, unbounded. The walk still offers every one of them a
// turn — a partition_point search each, just to read `active` and skip — so the
// deque grows without limit and each dispatch pays for the whole of it.
TEST(HandlerTableCompactionTests, AnIdDispatchedOnlyFromNestedContextsDoesNotAccumulateDeadEntries)
{
    constexpr EventId kOuterEvent = HashEvent("UI.CompactionProbe.Outer");

    UIElement el;
    int innerFired = 0;
    UIElement::EventHandlerToken current{};

    el.RegisterEventHandler(kOuterEvent, [&](UIEvent&) { Send(el, kProbeEvent); });

    constexpr int kCycles = 64;
    for (int i = 0; i < kCycles; ++i)
    {
        current = el.RegisterEventHandler(kProbeEvent, [&innerFired](UIEvent&) { ++innerFired; });
        Send(el, kOuterEvent);
        el.UnregisterEventHandler(current);
    }

    EXPECT_EQ(innerFired, kCycles);
    EXPECT_LE(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u)
        << "dead entries accumulated one per cycle and were never swept";
}

// The same, but the nesting frame is a member CALLBACK SLOT rather than a
// handler-table subscriber. The slot publishes a frame on this element while holding
// no reference into the table, which is what makes the stranding reachable from
// ordinary control code rather than only from a nested dispatch.
TEST(HandlerTableCompactionTests, AMemberSlotFrameDoesNotStrandDeferredEntries)
{
    Button btn;
    int innerFired = 0;
    UIElement::EventHandlerToken current{};

    btn.SetOnMouseDown([&](Button& b) { Send(b, kProbeEvent); });

    constexpr int kCycles = 16;
    for (int i = 0; i < kCycles; ++i)
    {
        current = btn.RegisterEventHandler(kProbeEvent, [&innerFired](UIEvent&) { ++innerFired; });
        UIEvent down{};
        down.Id = kEventMouseDown;
        down.Target = &btn;
        down.CurrentTarget = &btn;
        down.Button = 0;
        btn.DispatchEvent(down);
        btn.UnregisterEventHandler(current);
    }

    EXPECT_EQ(innerFired, kCycles);
    EXPECT_LE(UIEventHandlerAccess::HandlerCount(btn, kProbeEvent), 1u)
        << "a member-slot frame stranded the deferred entries";
}

// Two `once` handlers where the first dispatches nested: each fires exactly once in
// total, from whichever walk reaches it first.
TEST(HandlerTableCompactionTests, TwoOnceHandlersWithANestedDispatchEachFireExactlyOnce)
{
    UIElement el;
    int firstFired = 0;
    int secondFired = 0;

    el.RegisterEventHandlerOnce(kProbeEvent,
                                [&](UIEvent&)
                                {
                                    if (++firstFired == 1)
                                        Send(el);
                                });
    el.RegisterEventHandlerOnce(kProbeEvent, [&](UIEvent&) { ++secondFired; });

    Send(el);

    EXPECT_EQ(firstFired, 1);
    EXPECT_EQ(secondFired, 1);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 0u);
}

// A `once` handler that sets e.Handled breaks the loop. The entry is already spent,
// and the sweep must still erase it.
TEST(HandlerTableCompactionTests, AHandledOnceHandlerIsStillSwept)
{
    UIElement el;
    int fired = 0;
    int laterFired = 0;

    el.RegisterEventHandlerOnce(kProbeEvent,
                                [&](UIEvent& e)
                                {
                                    ++fired;
                                    e.Handled = true;
                                });
    el.RegisterEventHandler(kProbeEvent, [&](UIEvent&) { ++laterFired; });

    Send(el);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(laterFired, 0);
    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(el, kProbeEvent), 1u)
        << "the spent once entry survived a Handled break";

    Send(el);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(laterFired, 1);
}

// Nesting on a DIFFERENT element must not defer: each element has its own frame
// stack and its own deque.
TEST(HandlerTableCompactionTests, NestingOnADifferentElementDoesNotDeferThatElementsSweep)
{
    UIElement a;
    UIElement b;
    UIElement::EventHandlerToken tb = b.RegisterEventHandler(kProbeEvent, [](UIEvent&) {});
    b.UnregisterEventHandler(tb);

    a.RegisterEventHandler(kProbeEvent, [&](UIEvent&) { Send(b); });
    Send(a);

    EXPECT_EQ(UIEventHandlerAccess::HandlerCount(b, kProbeEvent), 0u)
        << "element B's sweep was suppressed by element A's frame";
}
