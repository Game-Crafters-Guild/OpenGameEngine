// The allocation cost of a mouse click, measured rather than argued.
//
// THE CONTRACT this inherits: the #1089 transition queue's own test measured
// "zero per-event heap allocation" as the pending vector's CAPACITY holding
// steady across a click soak — that is, steady-state clicking not growing its
// storage. The queue is gone; that guarantee is not, and it is restated here
// against the path that replaced it. "We deleted a vector" is not evidence
// that nothing took its place.
//
// WHAT MEASURES IT. UIManager::OnMouseButton lives in Engine.dll, so the
// count has to include Engine's own allocations. The allocation counter does
// wherever Engine carries the allocation hook (GE_DEBUG_INSTRUMENTATION, and on
// macOS and Linux every configuration, where the replacements coalesce), so this
// suite is not registered where it would read a false zero: Windows Release
// (UI/CMakeLists.txt). The window is ThisThread, so the job system's background
// work cannot be counted as a click's cost.
//
// AnAllocatingHandlerMovesTheCountByExactlyItsOwnAllocations is this file's
// positive control. Every number here is worthless without it: it proves the
// hook can see allocations made on THIS path at all. It must not be deleted,
// and must not be loosened to a bound.
#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include "Memory/AllocationCountScope.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

using namespace GameEngine;

namespace
{

// Button under a 200x40 root, cursor parked over it and hover resolved, so a
// click dispatches through a control that actually acts — the expensive case.
// Layout only, never rendered: this binary configures no shader path resolver,
// and hover is decided by the interactive Update's hit test regardless.
struct ClickFixture
{
    std::unique_ptr<Rendering::IDevice> Dev;
    std::unique_ptr<UIManager> Ui;
    Button* Btn = nullptr;
    int Clicks = 0;

    bool Init()
    {
        Dev = MakeHeadlessDevice();
        if (!Dev)
            return false;
        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        auto btnOwned = std::make_unique<Button>();
        Btn = btnOwned.get();
        Btn->SetId("btn");
        Btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ++Clicks; });
        root->AddChild(std::move(btnOwned));

        Ui = std::make_unique<UIManager>(Dev.get());
        Ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_mouse_dispatch_alloc.css";
        {
            std::ofstream f(css);
            f << R"(
#root { display: flex; width: 200px; height: 40px; }
#btn { width: 100px; height: 20px; }
)";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return false;

        Ui->Update(0.0f, /*interactive=*/false);
        Ui->OnMouseMove(Btn->GetLayoutX() + 5.0f, Btn->GetLayoutY() + 5.0f);
        Ui->Update(0.0f, /*interactive=*/true);
        return true;
    }

    // One click, dispatched entirely at the callback — no frame in between,
    // which is the shape being measured. Frames do layout and hover work whose
    // cost is not this path's.
    void Click()
    {
        Ui->OnMouseButton(0, true);
        Ui->OnMouseButton(0, false);
    }

    // Enough clicks that any first-use capacity growth (element ids, the
    // thread-local bubble-chain buffer, focus-id storage) has settled outside
    // the measurement window.
    void Warmup(int clicks)
    {
        for (int i = 0; i < clicks; ++i)
            Click();
    }
};

size_t AllocationsOverClicks(ClickFixture& f, int clicks)
{
    const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
    for (int i = 0; i < clicks; ++i)
        f.Click();
    return probe.Count();
}
} // namespace

// THE POSITIVE CONTROL FOR EVERY NUMBER IN THIS FILE.
//
// It clicks the same button twice over, once with a handler that allocates on
// purpose and once without, and demands the DIFFERENCE equal what that handler
// allocated. A difference is the right instrument check because it is
// independent of what dispatch itself costs: it comes out right whether the
// path allocates nothing (as it must) or allocates per click, so a failure here
// means the hook cannot see this path — never merely that the path got dearer.
//
// Without this, "the count did not move" and "the hook was never armed on this
// path" are the same reading, and the second is not hypothetical: a probe in an
// image without the allocation hook cannot see Engine.dll's allocations.
TEST(UIMouseDispatchAllocationTests, AnAllocatingHandlerMovesTheCountByExactlyItsOwnAllocations)
{
    ClickFixture quiet;
    if (!quiet.Init())
        GTEST_SKIP() << "Device init failed";

    constexpr int kClicks = 64;
    quiet.Warmup(16);
    const size_t quietCount = AllocationsOverClicks(quiet, kClicks);
    ASSERT_GT(quiet.Clicks, 0) << "control: the quiet arm must actually be clicking";

    ClickFixture loud;
    if (!loud.Init())
        GTEST_SKIP() << "Device init failed";
    // Exactly one allocation per press, released immediately so nothing
    // accumulates that could be measured instead of the dispatch. A direct call
    // to the replaceable function, because an optimizer may drop a
    // new-expression whose result does not escape, but never a call.
    size_t sink = 0;
    int loudHandlerCalls = 0;
    loud.Btn->RegisterEventHandler(kEventMouseDown,
                                   [&](UIEvent&)
                                   {
                                       void* const memory = ::operator new(sizeof(int));
                                       sink += static_cast<size_t>(++loudHandlerCalls);
                                       ::operator delete(memory);
                                   });
    loud.Warmup(16);
    const int callsBeforeWindow = loudHandlerCalls;
    const size_t loudCount = AllocationsOverClicks(loud, kClicks);

    ASSERT_EQ(loudHandlerCalls - callsBeforeWindow, kClicks)
        << "the allocating handler must fire on every click inside the window, or the difference "
           "below is not measuring what it allocated";
    ASSERT_GT(sink, 0u) << "the allocated value was never read back; the allocation may not be real";

    ASSERT_GE(loudCount, quietCount)
        << "POSITIVE CONTROL FAILED: adding an allocating handler lowered the measured count";
    EXPECT_EQ(loudCount - quietCount, static_cast<size_t>(kClicks))
        << "POSITIVE CONTROL FAILED: " << kClicks
        << " clicks x one allocating handler should move the count by exactly that much (measured "
        << loudCount << " loud vs " << quietCount
        << " quiet); this hook cannot see, or is miscounting, allocations made on the mouse "
           "dispatch path — which voids every number in this file";
}

// THE CONTRACT, stated as what is actually true rather than as a slogan.
//
// A click is not promised to be allocation-free: its operator-new count depends
// on the platform's standard library and on what the dispatch path does today
// (the measured value is recorded below). What the
// #1089 queue test actually pinned was narrower than "zero" — it asserted the
// pending vector's CAPACITY held steady across a 500-click soak, i.e. that
// steady-state clicking does not GROW its storage. That is the guarantee this
// arm inherits, and it is the one that matters: an unbounded per-click cost is
// a leak in slow motion, while a small fixed cost is a constant.
//
// So this asserts flatness, not zero: the allocations charged to the first
// hundred clicks of a soak must equal those charged to the last hundred. A
// per-click container that regrows, a cache that accumulates per event, or a
// string that reallocates as ids lengthen all break this while a bare
// "count > 0" check would not.
//
// The absolute number is recorded rather than asserted, because pinning it
// would fail on any unrelated and legitimate change to the dispatch path
// without indicating a real regression. The ceiling below exists only to catch
// an order-of-magnitude blow-up.
TEST(UIMouseDispatchAllocationTests, SteadyStateClickingDoesNotGrowItsAllocationCost)
{
    ClickFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    constexpr int kWindow = 100;
    constexpr int kMiddle = 300;

    f.Warmup(16);
    const int clicksBeforeFirst = f.Clicks;
    const size_t firstWindow = AllocationsOverClicks(f, kWindow);
    ASSERT_EQ(f.Clicks - clicksBeforeFirst, kWindow)
        << "control: every click in the first window must have landed, or a flat comparison below "
           "would be comparing two runs of nothing";

    // Soak between the windows: whatever grows per click has 300 clicks to do it.
    f.Warmup(kMiddle);

    const int clicksBeforeLast = f.Clicks;
    const size_t lastWindow = AllocationsOverClicks(f, kWindow);
    ASSERT_EQ(f.Clicks - clicksBeforeLast, kWindow)
        << "control: every click in the last window must have landed";

    RecordProperty("AllocationsPerClick", static_cast<int>(firstWindow / kWindow));
    std::cout << "[ MEASURED ] " << (static_cast<double>(firstWindow) / kWindow)
              << " allocations per click at the start of the soak, "
              << (static_cast<double>(lastWindow) / kWindow) << " after " << kMiddle
              << " more clicks" << std::endl;

    EXPECT_EQ(lastWindow, firstWindow)
        << "clicking grew more expensive over a " << kMiddle << "-click soak (" << firstWindow
        << " allocations for the first " << kWindow << " clicks, " << lastWindow
        << " for the last); something on the dispatch path accumulates per event. This is the "
           "guarantee the deleted transition queue's capacity-stability test protected";

    // Order-of-magnitude tripwire only. A click doing tens of allocations is a
    // different design from one doing a handful, and nobody should reach that
    // by accident.
    constexpr size_t kCeilingPerClick = 40;
    EXPECT_LE(firstWindow / kWindow, kCeilingPerClick)
        << "a click now costs " << (firstWindow / kWindow)
        << " allocations, past the " << kCeilingPerClick
        << " ceiling this path was designed under";
}

// The same flatness claim over the shape a user actually produces: clicks
// interleaved with the frames that render them. The frames are inside the
// window on purpose — a transition leaves hover re-evaluation and an :active
// re-cascade for the next frame, and a regression that merely moved a
// per-event allocation from the callback into the frame it schedules would
// otherwise hide.
TEST(UIMouseDispatchAllocationTests, ClickingWithFramesDoesNotGrowItsAllocationCost)
{
    ClickFixture f;
    if (!f.Init())
        GTEST_SKIP() << "Device init failed";

    auto clickAndFrame = [&f]()
    {
        f.Click();
        f.Ui->Update(0.0f, /*interactive=*/true);
    };
    auto measure = [&](int n)
    {
        const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
        for (int i = 0; i < n; ++i)
            clickAndFrame();
        return probe.Count();
    };

    for (int i = 0; i < 16; ++i)
        clickAndFrame();

    constexpr int kWindow = 100;
    constexpr int kMiddle = 200;

    const int clicksBeforeFirst = f.Clicks;
    const size_t firstWindow = measure(kWindow);
    ASSERT_EQ(f.Clicks - clicksBeforeFirst, kWindow) << "control: every click must have landed";

    for (int i = 0; i < kMiddle; ++i)
        clickAndFrame();

    const int clicksBeforeLast = f.Clicks;
    const size_t lastWindow = measure(kWindow);
    ASSERT_EQ(f.Clicks - clicksBeforeLast, kWindow) << "control: every click must have landed";

    RecordProperty("AllocationsPerClickWithFrame", static_cast<int>(firstWindow / kWindow));
    std::cout << "[ MEASURED ] " << (static_cast<double>(firstWindow) / kWindow)
              << " allocations per click including its follow-up frame at the start of the soak, "
              << (static_cast<double>(lastWindow) / kWindow) << " after " << kMiddle << " more"
              << std::endl;

    EXPECT_EQ(lastWindow, firstWindow)
        << "a click plus its frame grew more expensive over the soak (" << firstWindow << " then "
        << lastWindow << " per " << kWindow
        << " iterations); the per-event cost may be accumulating in the frame the transition "
           "schedules rather than in the callback";
}
