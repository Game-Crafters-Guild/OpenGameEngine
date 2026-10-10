// The allocation cost of a value change, measured rather than argued.
//
// THE CONTRACT: a committed value change costs ZERO allocations on the dispatch path — with no
// subscriber, with one, and with many. Unsubscribed, the presence bit turns each dispatch into a
// bit test. Subscribed, DispatchToHandlers walks the handler table IN PLACE, by key rather than
// by index so compaction cannot desynchronise it, and copies nothing. A committed value change
// fires two dispatches and a dragged slider fires one per pointer move, so this is a per-frame
// path.
//
// "Nothing" is a number, and behaviour tests cannot see it: an edit that drops the presence-bit
// test, reintroduces a per-dispatch copy of the handler list, or builds an event payload on the
// heap keeps every behavioural arm green while the inspector pays for it on every field write.
//
// A ZERO IS ONLY EVIDENCE IF THE PROBE CAN SEE THIS PATH, and it very nearly cannot — read the
// note on MeasuredField. AnAllocatingSubscriberMovesTheCountByExactlyItsOwnAllocations is this
// file's positive control: it runs the same chain with and without a subscriber that allocates
// on purpose, and demands the exact difference. Every zero here is worthless without it, so it
// must not be deleted, and must not be loosened from an equality to a bound.
//
// Its own target on purpose: the measurement is a process-wide allocation window, and the UI
// test executables each share one process across hundreds of tests. Same isolation rule as
// ToolbarDragDropAllocationTests.
#include <gtest/gtest.h>

#include "UI/Controls/BaseField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "Memory/AllocationCountScope.h"

#include <cstddef>
#include <iostream>
#include <memory>

using namespace GameEngine;

namespace
{
// WHAT THE PROBE CAN AND CANNOT SEE — the limit that decides how these arms are written.
//
// The allocation counter sees an image's allocations only where that image carries the
// allocation hook, and Engine.dll carries it only under GE_DEBUG_INSTRUMENTATION. On Windows
// every image's operator new is its own, so without the instrumentation a Slider-driven
// measurement (Slider::SetValue lives in Engine.dll, and the whole notify-and-dispatch chain
// inlines there) reads zero whether the path allocates or not: a false zero for a path that
// provably copies a vector, which would equally make the no-subscriber arm a vacuous pass.
//
// So the measured control is declared HERE. Field<T> is a template and NotifyValue is
// header-inline, so a local Field<float> instantiates the shipped chain in this binary:
// SetValue -> NotifyValueChanging + NotifyValueChanged -> the depth guard -> the member slot ->
// DispatchEvent<kId> -> the presence bit -> DispatchToHandlers. That is the whole path an
// inspector field write takes once it reaches the field, measured end to end rather than from
// its last link.
class MeasuredField : public Field<float>
{
public:
    // Exactly what Slider::SetValue does around the base setter: early-out on an unchanged
    // value, store, then notify both events. Reproduced rather than called, because calling
    // Slider's would put the chain inside Engine.dll, which the probe sees only where Engine
    // carries the allocation hook.
    void SetValueNotifying(float v)
    {
        if (GetValue() == v)
            return;
        Field<float>::SetValue(v);
        NotifyValueChanging();
        NotifyValueChanged();
    }
};

// Alternating values so the equality early-out never swallows a write, which would measure
// nothing while looking identical to a free path.
size_t AllocationsPerValueChange(MeasuredField& field, int iterations)
{
    const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
    for (int i = 0; i < iterations; ++i)
        field.SetValueNotifying((i % 2 == 0) ? 1.0f : 2.0f);
    return probe.Count();
}

// The string-payload chain, instantiated in this binary for the same reason MeasuredField is:
// the dispatch-window copy (ScopedValueText) is header-inline precisely so it is measured in
// this binary, where a control-driven measurement through Engine.dll could read a false zero.
class MeasuredTextField : public Field<std::string>
{
public:
    // What TextFieldBase's commit does around the base setter, reproduced rather than called
    // for the probe-visibility reason above.
    void SetValueNotifying(const std::string& v)
    {
        if (GetValue() == v)
            return;
        Field<std::string>::SetValue(v);
        NotifyValueChanging();
        NotifyValueChanged();
    }
};

// Both strings pre-built and equal-length: the loop's only string work is the copy into
// m_Value and (with a subscriber) into the dispatch window, both of which reuse capacity
// after warm-up. Long enough to defeat SSO, so a hidden allocation could not hide inside it.
const std::string kTextA(48, 'a');
const std::string kTextB(48, 'b');

size_t AllocationsPerTextChange(MeasuredTextField& field, int iterations)
{
    const Memory::AllocationCountScope probe(Memory::CountWindow::Process);
    for (int i = 0; i < iterations; ++i)
        field.SetValueNotifying((i % 2 == 0) ? kTextA : kTextB);
    return probe.Count();
}
} // namespace

// THE ARM for the no-subscriber claim — the path every inspector field write takes. It must cost
// zero allocations: the presence bit turns each of the two dispatches into a bit test before
// anything can allocate.
TEST(UIDispatchAllocationTests, AValueChangeWithNoSubscriberAllocatesNothing)
{
    MeasuredField field;
    field.SetValueNotifying(1.0f); // settle any first-write state outside the window

    constexpr int kIterations = 256;
    const size_t allocations = AllocationsPerValueChange(field, kIterations);

    EXPECT_EQ(allocations, 0u)
        << "an unsubscribed value change allocated " << allocations << " times over " << kIterations
        << " changes; the inspector writes fields in bulk, so this path has to stay free";
}

// THE POSITIVE CONTROL FOR EVERY ZERO IN THIS FILE, and the reason they mean anything.
//
// It measures the same chain twice — once with a subscriber that allocates deliberately, once
// with one that does not — and asserts the DIFFERENCE is exactly the allocations the handler
// made. A difference is the right instrument check because it is INDEPENDENT of what the walk
// itself costs: it comes out right whether the dispatch path allocates nothing (as it must) or
// allocates per dispatch (as a copy-first walk would), so a failure here means the probe cannot
// see this path or is miscounting it, and never merely that the path got more expensive. The
// arms below are what test the path's own cost.
//
// This is the difference between "the count was zero" and "the counter was never armed on this
// path" — which is not hypothetical: a measurement through an image without the allocation
// hook reads a confident zero for a path that provably copies a vector. If this fails, no
// other arm in this file is evidence of anything.
TEST(UIDispatchAllocationTests, AnAllocatingSubscriberMovesTheCountByExactlyItsOwnAllocations)
{
    constexpr int kIterations = 256;

    size_t sink = 0;
    MeasuredField quietField;
    int quietSeen = 0;
    quietField.RegisterEventHandler(kEventValueChanging, [&](UIEvent&) { ++quietSeen; });
    quietField.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++quietSeen; });
    quietField.SetValueNotifying(3.0f); // outside the window: grows the deques
    const size_t quiet = AllocationsPerValueChange(quietField, kIterations);

    MeasuredField loudField;
    int loudSeen = 0;
    // Allocated and released immediately: exactly one operator new per call, and nothing
    // accumulated that could be measured instead of the dispatch. A direct call to the
    // replaceable function, because an optimizer may drop a new-expression whose result does
    // not escape (clang does in Release once the replacement lives in another translation
    // unit), but never a call.
    auto allocateOnce = [&](UIEvent&) {
        void* const memory = ::operator new(sizeof(int));
        sink += static_cast<size_t>(++loudSeen);
        ::operator delete(memory);
    };
    loudField.RegisterEventHandler(kEventValueChanging, allocateOnce);
    loudField.RegisterEventHandler(kEventValueChanged, allocateOnce);
    loudField.SetValueNotifying(3.0f); // outside the window: grows the deques
    const size_t loud = AllocationsPerValueChange(loudField, kIterations);

    ASSERT_EQ(quietSeen, (kIterations + 1) * 2)
        << "the quiet arm's handlers must fire on every change, or the two arms are not the same "
           "walk";
    ASSERT_EQ(loudSeen, (kIterations + 1) * 2)
        << "the allocating handlers must fire on every change, or the difference below is not "
           "measuring what they allocated";
    ASSERT_GT(sink, 0u) << "the allocated value was never read back; the allocation may not be real";

    ASSERT_GE(loud, quiet) << "POSITIVE CONTROL FAILED: adding allocations to a handler lowered "
                              "the measured count";
    EXPECT_EQ(loud - quiet, static_cast<size_t>(kIterations) * 2)
        << "POSITIVE CONTROL FAILED: " << kIterations
        << " changes x 2 allocating subscribers should move the count by exactly that much "
           "(measured " << loud << " loud vs " << quiet
        << " quiet); this probe cannot see, or is miscounting, allocations made on the dispatch "
           "path — which voids every zero in this file";
}

// The contract's subscribed arm — the number 14.5(b) asks for. The dispatch walks the handler
// table in place, so a subscriber costs the dispatch nothing at all.
TEST(UIDispatchAllocationTests, AValueChangeWithOneSubscriberAllocatesNothing)
{
    MeasuredField field;
    int seen = 0;
    field.RegisterEventHandler(kEventValueChanging, [&](UIEvent&) { ++seen; });
    field.RegisterEventHandler(kEventValueChanged, [&](UIEvent&) { ++seen; });
    field.SetValueNotifying(3.0f); // first dispatch outside the window: it grows the deques

    constexpr int kIterations = 256;
    const size_t allocations = AllocationsPerValueChange(field, kIterations);

    ASSERT_EQ(seen, (kIterations + 1) * 2)
        << "control: both handlers must fire on every change, or the count below would mean "
           "'nothing ran' rather than 'nothing allocated'";

    RecordProperty("AllocationsPerSubscribedValueChange", static_cast<int>(allocations));
    std::cout << "[ MEASURED ] " << (static_cast<double>(allocations) / kIterations)
              << " allocations per committed value change with one subscriber on each event ("
              << allocations << " over " << kIterations << " changes, two dispatches each)"
              << std::endl;

    EXPECT_EQ(allocations, 0u)
        << "a committed value change with one subscriber allocated " << allocations << " times "
        << "over " << kIterations << " changes; something on the dispatch path is copying the "
           "handler list or building a payload on the heap. A dragged slider pays this per "
           "pointer move";
}

// SCALING, which is the half a single-subscriber arm cannot see. A per-dispatch copy of the
// handler list costs one allocation for the list plus one per closure too big for
// std::function's inline budget, so its cost RISES with the subscriber count while a
// one-subscriber arm stays cheap enough to look acceptable. Fat captures on purpose, for that
// second term.
TEST(UIDispatchAllocationTests, ManyFatSubscribersStillAllocateNothingPerChange)
{
    // Comfortably past std::function's inline budget (48 bytes usable on this toolchain), so a
    // copied entry would have to reach the heap.
    struct FatCapture
    {
        char Padding[192] = {};
    };

    MeasuredField field;
    int seen = 0;
    constexpr int kSubscribers = 8;
    for (int i = 0; i < kSubscribers; ++i)
    {
        field.RegisterEventHandler(kEventValueChanging,
                                   [&seen, cap = FatCapture{}](UIEvent&) { seen += cap.Padding[0] + 1; });
        field.RegisterEventHandler(kEventValueChanged,
                                   [&seen, cap = FatCapture{}](UIEvent&) { seen += cap.Padding[0] + 1; });
    }
    field.SetValueNotifying(3.0f); // outside the window: grows the deques to their final size

    constexpr int kIterations = 256;
    const size_t allocations = AllocationsPerValueChange(field, kIterations);

    ASSERT_EQ(seen, (kIterations + 1) * kSubscribers * 2)
        << "control: all " << (kSubscribers * 2) << " subscribers must fire on every change, or a "
           "low count below would mean the walk stopped early rather than allocated nothing";

    RecordProperty("AllocationsPerFatSubscribedValueChange", static_cast<int>(allocations));
    std::cout << "[ MEASURED ] " << (static_cast<double>(allocations) / kIterations)
              << " allocations per committed value change with " << (kSubscribers * 2)
              << " fat subscribers (" << allocations << " over " << kIterations << " changes)"
              << std::endl;

    EXPECT_EQ(allocations, 0u)
        << "a committed value change with " << (kSubscribers * 2) << " subscribers allocated "
        << allocations << " times over " << kIterations
        << " changes; the dispatch cost is scaling with the subscriber count, which is what a "
           "per-dispatch copy of the handler list looks like";
}

// The string path's no-subscriber arm — the path the inspector's bulk text writes take. What
// it pins is the COST: such a write allocates for the value assignment and nothing else, the
// same "nothing" the float arm pins.
//
// It does NOT pin the presence gate, and must not be read as doing so. Delete the gate and
// this arm stays green: the warm-up outside the window would grow the dispatch-window buffer
// too, leaving every measured iteration a memcpy into retained capacity, and a dispatch with
// nothing registered returns on a bit test before it can allocate. The gate is
// allocation-invisible at steady state by design — that is the whole claim of §14.6(2)'s
// option A. Its own tripwire is ControlValueEventTests.AnUnsubscribedStringFieldSkipsItsOwnOnEventToo
// (UITests), which counts OnEvent calls: a thing the gate changes and an allocator cannot see.
TEST(UIDispatchAllocationTests, ATextChangeWithNoSubscriberAllocatesNothing)
{
    MeasuredTextField field;
    // Settle capacity outside the window: m_Value grows to hold both value lengths once.
    field.SetValueNotifying(kTextA);
    field.SetValueNotifying(kTextB);

    constexpr int kIterations = 256;
    const size_t allocations = AllocationsPerTextChange(field, kIterations);

    EXPECT_EQ(allocations, 0u)
        << "an unsubscribed text change allocated " << allocations << " times over " << kIterations
        << " changes; the inspector's bulk text writes must stay allocation-free";
}

// The string path costs the same nothing the float path does, which is the stronger claim of the
// two: the dispatch-window copy that guarantees payload lifetime keeps its buffer's capacity
// across changes, so it is a memcpy rather than an allocation. A regression here reads as "the
// string path got more expensive than the numeric one", which is exactly the claim §14.6(2)'s
// option A made.
TEST(UIDispatchAllocationTests, ASubscribedTextChangeAllocatesNothing)
{
    MeasuredTextField field;
    int seen = 0;
    // Compared INSIDE the handler, not kept: the payload is only guaranteed for the dispatch,
    // and a test for that guarantee is the last place that should outlive it. A bool also
    // costs no allocation, which this arm is measuring.
    bool lastPayloadWasB = false;
    field.RegisterEventHandler(kEventValueChanging, [&](UIEvent&) { ++seen; });
    field.RegisterEventHandler(kEventValueChanged,
                               [&](UIEvent& e)
                               {
                                   ++seen;
                                   lastPayloadWasB = e.Text == kTextB;
                               });
    // Warm-up outside the window: grows the handler vectors, m_Value's capacity, and the
    // per-thread dispatch buffer to both value lengths.
    field.SetValueNotifying(kTextA);
    field.SetValueNotifying(kTextB);

    constexpr int kIterations = 256;
    const size_t allocations = AllocationsPerTextChange(field, kIterations);

    ASSERT_EQ(seen, (kIterations + 2) * 2)
        << "control: both handlers must fire on every change, or the count below would mean "
           "'nothing ran' rather than 'nothing extra allocated'";
    ASSERT_TRUE(lastPayloadWasB) << "control: the payload really was delivered through Text";

    RecordProperty("AllocationsPerSubscribedTextChange", static_cast<int>(allocations));
    std::cout << "[ MEASURED ] " << (static_cast<double>(allocations) / kIterations)
              << " allocations per committed text change with one subscriber on each event ("
              << allocations << " over " << kIterations << " changes, two dispatches each)"
              << std::endl;

    EXPECT_EQ(allocations, 0u)
        << "a committed text change with one subscriber allocated " << allocations << " times "
        << "over " << kIterations
        << " changes; the dispatch-window copy is allocating per dispatch instead of reusing its "
           "buffer, or something on the dispatch path is copying the handler list";
}
